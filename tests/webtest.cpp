// Tests for the Pi's web file manager (src/circle/webserver.cpp), run on the
// host: Circle's sockets and FatFs are replaced by POSIX ones (tests/webshim).

#include "../src/circle/webserver.h"
#include <circle/sched/scheduler.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <fatfs/ff.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>
#include <thread>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string root;
static const u16 kPort = 18000 + (getpid() % 1000);

// ----------------------------------------------------------------- shims

CScheduler *CScheduler::Get() { static CScheduler s; return &s; }
void CScheduler::MsSleep(unsigned ms) { usleep(ms * 1000); }
CLogger *CLogger::Get() { static CLogger l; return &l; }
void CLogger::Write(const char *, TLogSeverity, const char *, ...) {}
CTimer *CTimer::Get() { static CTimer t; return &t; }
unsigned CTimer::GetTicks()
{
    timeval tv;
    gettimeofday(&tv, 0);
    return (unsigned)(tv.tv_sec * HZ + tv.tv_usec / (1000000 / HZ));
}

CSocket::CSocket(CNetSubSystem *, int) : fd(socket(AF_INET, SOCK_STREAM, 0))
{
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
}
CSocket::~CSocket() { close(fd); }
int CSocket::Bind(u16)
{
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return bind(fd, (sockaddr *)&a, sizeof a);
}
int CSocket::Listen(unsigned n) { return listen(fd, (int)n); }
CSocket *CSocket::Accept(CIPAddress *, u16 *)
{
    int c = accept(fd, 0, 0);
    return c < 0 ? 0 : new CSocket(c);
}
int CSocket::Send(const void *p, unsigned n, int) { return (int)send(fd, p, n, MSG_NOSIGNAL); }
int CSocket::Receive(void *p, unsigned n, int) { return (int)recv(fd, p, n, 0); }
int CSocket::SetOptionReceiveTimeout(unsigned us)
{
    timeval tv = {(time_t)(us / 1000000), (suseconds_t)(us % 1000000)};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
int CSocket::SetOptionSendTimeout(unsigned) { return 0; }

static std::string hostPath(const char *path)
{
    return root + "/" + (strncmp(path, "SD:/", 4) == 0 ? path + 4 : path);
}

FRESULT f_open(FIL *fp, const char *path, int mode)
{
    fp->f = fopen(hostPath(path).c_str(), (mode & FA_WRITE) ? "wb" : "rb");
    if (!fp->f) return FR_NO_FILE;
    struct stat st;
    fstat(fileno(fp->f), &st);
    fp->size = (unsigned long long)st.st_size;
    return FR_OK;
}
FRESULT f_read(FIL *fp, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, fp->f); return FR_OK; }
FRESULT f_write(FIL *fp, const void *buf, UINT n, UINT *done) { *done = (UINT)fwrite(buf, 1, n, fp->f); return FR_OK; }
FRESULT f_close(FIL *fp) { fclose(fp->f); return FR_OK; }
FRESULT f_unlink(const char *path) { return unlink(hostPath(path).c_str()) == 0 ? FR_OK : FR_NO_FILE; }
FRESULT f_rename(const char *a, const char *b)
{
    return rename(hostPath(a).c_str(), hostPath(b).c_str()) == 0 ? FR_OK : FR_DISK_ERR;
}
FRESULT f_findnext(DIR *dp, FILINFO *fi)
{
    fi->fname[0] = 0;
    while (dirent *e = readdir(dp->d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(fi->fname, sizeof fi->fname, "%s", e->d_name);
        struct stat st;
        stat((std::string(dp->path) + "/" + e->d_name).c_str(), &st);
        fi->fsize = (unsigned long long)st.st_size;
        fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
        break;
    }
    return FR_OK;
}
FRESULT f_findfirst(DIR *dp, FILINFO *fi, const char *path, const char *)
{
    snprintf(dp->path, sizeof dp->path, "%s", hostPath(path).c_str());
    dp->d = opendir(dp->path);
    if (!dp->d) return FR_NO_FILE;
    return f_findnext(dp, fi);
}
FRESULT f_closedir(DIR *dp) { if (dp->d) closedir(dp->d); return FR_OK; }
FRESULT f_getfree(const char *, DWORD *clusters, FATFS **fs)
{
    static FATFS f = {64};
    *clusters = 1000;
    *fs = &f;
    return FR_OK;
}

// ----------------------------------------------------------------- the test

class Host : public CWebServerHost {
public:
    boolean FileInUse(const char *p) override { return strcmp(p, "hd0.img") == 0 || strcmp(p, "disks/inuse.dsk") == 0; }
    const char *DiskDir() override { return "disks"; }
};

struct Reply { int code; std::string body; };

static Reply request(const std::string &method, const std::string &path, const std::string &body = "",
                     long claimedLength = -1)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 100 && connect(fd, (sockaddr *)&a, sizeof a) != 0; i++) {
        close(fd);
        usleep(20000);
        fd = socket(AF_INET, SOCK_STREAM, 0);
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: test\r\n";
    if (method == "PUT")
        req += "Content-Length: " + std::to_string(claimedLength >= 0 ? claimedLength : (long)body.size()) + "\r\n";
    req += "\r\n" + body;
    for (size_t off = 0; off < req.size(); ) {
        ssize_t n = send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += (size_t)n;
    }
    if (claimedLength >= 0) { close(fd); return {0, ""}; }   // a broken transfer
    std::string resp;
    char buf[65536];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof buf, 0)) > 0) resp.append(buf, (size_t)n);
    close(fd);
    Reply r = {0, ""};
    if (resp.size() > 12) r.code = atoi(resp.c_str() + 9);
    size_t h = resp.find("\r\n\r\n");
    if (h != std::string::npos) r.body = resp.substr(h + 4);
    return r;
}

static std::string readFile(const std::string &p)
{
    FILE *f = fopen((root + "/" + p).c_str(), "rb");
    if (!f) return "<missing>";
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static void writeFile(const std::string &p, const std::string &data)
{
    FILE *f = fopen((root + "/" + p).c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc > 1) {                     // webtest DIR: just serve DIR, to look at the page
        root = argv[1];
        Host host;
        CNetSubSystem net;
        printf("http://127.0.0.1:%u/\n", kPort);
        fflush(stdout);
        CWebServer(&net, "SD:", &host).Run();
        return 0;
    }

    char tmpl[] = "/tmp/webtestXXXXXX";
    root = mkdtemp(tmpl);
    mkdir((root + "/disks").c_str(), 0755);
    writeFile("kernel8.img", "kernel");
    writeFile("config.txt", "cfg");
    writeFile("mac.rom", std::string(1024, 'R'));
    writeFile("hd0.img", std::string(4096, 'H'));
    writeFile("disks/inuse.dsk", std::string(819200, 'F'));
    writeFile("disks/Old & <odd>.dsk", "x");

    Host host;
    CNetSubSystem net;
    CWebServer *server = new CWebServer(&net, "SD:", &host);
    std::thread([server] { server->Run(); }).detach();

    // The emulator's main loop: carries out flush and restart requests.
    std::atomic<int> flushes(0), restarts(0);
    std::thread([&] {
        for (;;) {
            if (server->FlushRequested()) { flushes++; server->FlushDone(); }
            if (server->RestartRequested()) { restarts++; server->RestartDone(); }
            usleep(5000);
        }
    }).detach();

    // The page lists the card, hides the Pi's boot files, escapes names.
    Reply r = request("GET", "/");
    CHECK(r.code == 200);
    CHECK(r.body.find("mac.rom") != std::string::npos);
    CHECK(r.body.find("hd0.img") != std::string::npos);
    CHECK(r.body.find("kernel8.img") == std::string::npos);
    CHECK(r.body.find("config.txt") == std::string::npos);
    CHECK(r.body.find("Old &amp; &lt;odd&gt;.dsk") != std::string::npos);
    CHECK(r.body.find("/files/disks/Old%20%26%20%3Codd%3E.dsk") != std::string::npos);
    CHECK(r.body.find("in use") != std::string::npos);
    CHECK(r.body.find("the disks folder") != std::string::npos);
    CHECK(r.body.find("</html>") != std::string::npos);

    // Upload, in a size that is not a multiple of anything.
    std::string data;
    for (int i = 0; i < 1234567; i++) data += (char)(i * 7 + (i >> 9));
    r = request("PUT", "/files/disks/New%20Disk.img", data);
    CHECK(r.code == 201);
    CHECK(readFile("disks/New Disk.img") == data);
    CHECK(readFile("disks/New Disk.img.part") == "<missing>");

    // Replace it with something smaller.
    r = request("PUT", "/files/disks/New%20Disk.img", "small");
    CHECK(r.code == 201);
    CHECK(readFile("disks/New Disk.img") == "small");

    // Download.
    r = request("GET", "/files/mac.rom");
    CHECK(r.code == 200 && r.body == std::string(1024, 'R'));
    r = request("GET", "/files/disks/nothing.img");
    CHECK(r.code == 404);

    // Downloading a disk the Mac is using flushes the emulator first.
    r = request("GET", "/files/hd0.img");
    CHECK(r.code == 200 && r.body.size() == 4096);
    CHECK(flushes == 1);

    // Disks in use and the Pi's boot files are protected.
    r = request("PUT", "/files/hd0.img", "evil");
    CHECK(r.code == 409);
    CHECK(readFile("hd0.img") == std::string(4096, 'H'));
    r = request("DELETE", "/files/disks/inuse.dsk");
    CHECK(r.code == 409);
    r = request("PUT", "/files/kernel8.img", "evil");
    CHECK(r.code == 403);
    r = request("DELETE", "/files/config.txt");
    CHECK(r.code == 403);
    CHECK(readFile("kernel8.img") == "kernel" && readFile("config.txt") == "cfg");

    // No escaping from the card.
    r = request("PUT", "/files/../escape.img", "x");
    CHECK(r.code == 403);
    r = request("GET", "/files/disks/../../etc/passwd");
    CHECK(r.code == 403);
    r = request("GET", "/files/%2e%2e/x");
    CHECK(r.code == 403);
    r = request("GET", "/nothing");
    CHECK(r.code == 404);

    // A transfer that breaks off leaves nothing behind.
    request("PUT", "/files/disks/broken.img", std::string(5000, 'b'), 100000);
    usleep(300000);
    CHECK(readFile("disks/broken.img") == "<missing>");
    CHECK(readFile("disks/broken.img.part") == "<missing>");

    // Delete.
    r = request("DELETE", "/files/disks/New%20Disk.img");
    CHECK(r.code == 200);
    CHECK(readFile("disks/New Disk.img") == "<missing>");
    r = request("DELETE", "/files/disks/New%20Disk.img");
    CHECK(r.code == 404);

    // Restart the Mac.
    r = request("POST", "/restart");
    CHECK(r.code == 200 && restarts == 1);

    // The server is still answering after all that.
    r = request("GET", "/");
    CHECK(r.code == 200);

    std::string cmd = "rm -rf " + root;
    if (system(cmd.c_str())) {}
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
