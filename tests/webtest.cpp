// Tests for the Pi's web file manager (src/circle/webserver.cpp), run on the
// host: Circle's sockets and FatFs are replaced by POSIX ones (tests/webshim).

#include "../src/circle/webserver.h"
#include "../src/frontend/hfs.h"
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
#include <vector>

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
    if ((mode & FA_CREATE_NEW) && access(hostPath(path).c_str(), F_OK) == 0) return FR_EXIST;
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
FRESULT f_lseek(FIL *fp, unsigned long long ofs) { return fseek(fp->f, (long)ofs, SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR; }
FRESULT f_stat(const char *path, FILINFO *fi)
{
    struct stat st;
    if (stat(hostPath(path).c_str(), &st) != 0) return FR_NO_FILE;
    fi->fsize = (unsigned long long)st.st_size;
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    return FR_OK;
}
FRESULT f_mkdir(const char *path) { return mkdir(hostPath(path).c_str(), 0755) == 0 ? FR_OK : FR_DISK_ERR; }
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
    const char *SharedDir() override { return "shared"; }
    void StatusText(char *o, unsigned n) override { snprintf(o, n, "Mac\tQuadra 650 & <b>\nFloppy drive 1\tSys.dsk\t0\nNetwork\tup\n"); }
    const char *Version() override { return "test <1>"; }
    macemu::u32 MacTime() override { return 0xE0000000u; }
    macemu::u32 MaxFloppyBytes() override { return 1474560; }
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

    static Host host;
    static CNetSubSystem net;
    CWebServer *server = new CWebServer(&net, "SD:", &host);
    std::thread([server] { server->Run(); }).detach();

    // The emulator's main loop: carries out flush and restart requests.
    static std::atomic<int> flushes(0), restarts(0);   // static: the server threads outlive main()
    static std::string inserted, driveName;
    static std::atomic<int> driveWhich(-9);
    std::thread([server] {
        for (;;) {
            if (server->FlushRequested()) { flushes++; server->FlushDone(); }
            if (server->RestartRequested()) { restarts++; server->RestartDone(); }
            if (const char *n = server->InsertRequested()) { inserted = n; server->InsertDone(true); }
            int w; const char *dn;
            if (server->DriveRequested(&w, &dn)) { driveName = dn; driveWhich = w; server->DriveDone("done & dusted"); }
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

    // The status panel: the host's lines, escaped; /status as plain text.
    CHECK(r.body.find("Quadra 650 &amp; &lt;b&gt;") != std::string::npos);
    r = request("GET", "/status");
    CHECK(r.code == 200 && r.body.find("Network\tup") != std::string::npos);

    // The shared folder: listed (not the Finder's Desktop file); a
    // MacBinary sent into it becomes the Mac file, forks and Finder info.
    mkdir((root + "/shared").c_str(), 0755);
    writeFile("shared/Desktop", "finder");
    writeFile("shared/notes.txt", "hello");
    r = request("GET", "/");
    CHECK(r.body.find("notes.txt") != std::string::npos);
    CHECK(r.body.find("/files/shared/Desktop") == std::string::npos);
    {
        macemu::HfsEntry e = {};
        strcpy(e.name, "Caf\x8E");             // Mac Roman e-acute
        e.type = 0x54455854; e.creator = 0x74747874;   // TEXT ttxt
        e.dataLen = 5; e.rsrcLen = 3; e.finderFlags = 0x0100;
        u8 h[128];
        macemu::macBinaryHeader(e, h);
        std::string bin((char *)h, 128);
        bin += "data!" + std::string(123, '\0') + "RSC" + std::string(125, '\0');
        r = request("PUT", "/files/shared/cafe.bin", bin);
        CHECK(r.code == 201 && r.body.find("unpacked") != std::string::npos);
        CHECK(readFile("shared/cafe.bin") == "<missing>");
        CHECK(readFile("shared/Caf\x82") == "data!");   // the card's code page 850
        std::string ad = readFile("shared/._Caf\x82");
        CHECK(ad.size() == 85 && ad.substr(82) == "RSC" && ad.substr(50, 8) == "TEXTttxt");
        // Not MacBinary: kept as it is.
        r = request("PUT", "/files/shared/plain.bin", "just bytes");
        CHECK(r.code == 201 && readFile("shared/plain.bin") == "just bytes");
    }

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

    // Insert and eject from the page: buttons for floppy-sized images and
    // drives with a disk; the request reaches the main loop.
    r = request("GET", "/");
    writeFile("disks/Free Floppy.dsk", std::string(819200, 'F'));
    writeFile("disks/Big.hda", std::string(2000000, 'H'));
    writeFile("disks/Games.iso", std::string(4096, 'C'));
    r = request("GET", "/");
    CHECK(r.body.find("drv('any','Free%20Floppy.dsk')") != std::string::npos);
    CHECK(r.body.find("drv('any','Big.hda')") == std::string::npos);      // a hard disk
    CHECK(r.body.find("drv('cd','Games.iso')") != std::string::npos);
    CHECK(r.body.find("drv('0','')") != std::string::npos);
    CHECK(r.body.find("drv('any','inuse.dsk')") == std::string::npos);    // in use
    r = request("POST", "/drive?d=any&name=Free%20Floppy.dsk");
    CHECK(r.code == 200 && r.body.find("done & dusted") != std::string::npos);
    CHECK(driveWhich == -1 && driveName == "Free Floppy.dsk");
    r = request("POST", "/drive?d=cd&name=");
    CHECK(r.code == 200 && driveWhich == 2 && driveName.empty());
    r = request("POST", "/drive?d=0&name=..%2Fhd0.img");
    CHECK(r.code == 400);

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

    // Software update: start-up files may be replaced and folders made,
    // settings, ROMs and disks may not.
    r = request("PUT", "/update/kernel8.img", "new kernel");
    CHECK(r.code == 201 && readFile("kernel8.img") == "new kernel");
    r = request("PUT", "/update/firmware/brcmfmac43455-sdio.raspberrypi,5-model-b.bin", "fw");
    CHECK(r.code == 201 && readFile("firmware/brcmfmac43455-sdio.raspberrypi,5-model-b.bin") == "fw");
    r = request("PUT", "/update/overlays/a/b.dtbo", "ov");
    CHECK(r.code == 201 && readFile("overlays/a/b.dtbo") == "ov");
    writeFile("macemu.cfg", "wifi_password=secret");
    r = request("PUT", "/update/macemu.cfg", "x");
    CHECK(r.code == 403 && readFile("macemu.cfg") == "wifi_password=secret");
    r = request("PUT", "/update/mac.rom", "x");
    CHECK(r.code == 403);
    r = request("PUT", "/update/disks/inuse.dsk", "x");
    CHECK(r.code == 403);
    r = request("PUT", "/update/../x", "x");
    CHECK(r.code == 403);
    r = request("PUT", "/update/hd0.img", "x");
    CHECK(r.code == 409);

    // Files with the Wi-Fi password are never shown or sent.
    writeFile("macemu-wifi.conf", "psk=secret");
    r = request("GET", "/");
    CHECK(r.body.find("/files/macemu.cfg") == std::string::npos && r.body.find("/files/macemu-wifi") == std::string::npos);
    CHECK(r.body.find("test &lt;1&gt;") != std::string::npos);
    CHECK(r.body.find("Update the emulator") != std::string::npos);
    r = request("GET", "/files/macemu-wifi.conf");
    CHECK(r.code == 403);
    r = request("GET", "/files/MACEMU.CFG");
    CHECK(r.code == 403);
    r = request("DELETE", "/files/macemu.cfg");
    CHECK(r.code == 403);

    // Inside a Mac disk image: browse, download as MacBinary and data fork.
    {
        static std::vector<u8> img(macemu::kHfsFloppyBytes);
        static u8 rsrc[700], data[5000];
        for (int i = 0; i < 700; i++) rsrc[i] = (u8)(i * 3);
        for (int i = 0; i < 5000; i++) data[i] = (u8)(i * 5 + 1);
        macemu::HfsNewFile f = {};
        strcpy(f.name, "Hello App");
        f.type = 0x4150504C; f.creator = 0x48454C4F;
        f.data = data; f.dataLen = 5000; f.rsrc = rsrc; f.rsrcLen = 700;
        CHECK(macemu::hfsMakeFloppy(img.data(), "Test Disk", &f, 1, 0xE0000000u));
        writeFile("disks/test.dsk", std::string(img.begin(), img.end()));
        r = request("GET", "/");
        CHECK(r.body.find("/hfs/disks/test.dsk") != std::string::npos);
        r = request("GET", "/hfs/disks/test.dsk");
        CHECK(r.code == 200 && r.body.find("Test Disk") != std::string::npos && r.body.find("Hello App") != std::string::npos);
        CHECK(r.body.find("APPL/HELO") != std::string::npos);
        r = request("GET", "/hfsget/disks/test.dsk?id=16&f=bin");
        macemu::HfsNewFile back;
        CHECK(r.code == 200 && macemu::macBinaryParse((const u8 *)r.body.data(), (u32)r.body.size(), back));
        CHECK(back.dataLen == 5000 && back.rsrcLen == 700 && !memcmp(back.rsrc, rsrc, 700) && !memcmp(back.data, data, 5000));
        CHECK(back.type == 0x4150504C && !strcmp(back.name, "Hello App"));
        r = request("GET", "/hfsget/disks/test.dsk?id=16&f=data");
        CHECK(r.code == 200 && r.body.size() == 5000 && !memcmp(r.body.data(), data, 5000));
        r = request("GET", "/hfs/mac.rom");
        CHECK(r.code == 415);
        r = request("GET", "/hfs/macemu.cfg");
        CHECK(r.code == 404 || r.code == 403);
    }

    // Send files to the Mac: a transfer floppy, put in drive 2.
    {
        r = request("POST", "/makefloppy?clear=1");
        CHECK(r.code == 200);
        mkdir((root + "/transfer").c_str(), 0755);
        r = request("PUT", "/files/transfer/notes.txt", "line one\nline two\n");
        CHECK(r.code == 201);
        r = request("PUT", "/files/transfer/picture.gif", std::string(3000, 'g'));
        CHECK(r.code == 201);
        r = request("POST", "/makefloppy?insert=2");
        CHECK(r.code == 200 && r.body.find("Transfer 1.dsk") != std::string::npos && r.body.find("drive 2") != std::string::npos);
        CHECK(inserted == "Transfer 1.dsk");
        std::string made = readFile("disks/Transfer 1.dsk");
        CHECK(made.size() == macemu::kHfsFloppyBytes);
        struct S : macemu::Storage {
            std::string *d;
            macemu::u32 size() override { return (macemu::u32)d->size(); }
            bool read(macemu::u32 o, void *b, macemu::u32 n) override { memcpy(b, d->data() + o, n); return true; }
            bool write(macemu::u32, const void *, macemu::u32) override { return false; }
            bool readOnly() override { return true; }
        } st;
        st.d = &made;
        macemu::HfsVolume v;
        CHECK(v.open(&st) && !strcmp(v.name(), "Transfer 1"));
        macemu::HfsEntry e[8];
        int n = v.list(2, e, 8);
        CHECK(n == 2);
        for (int i = 0; i < n; i++) {
            if (!strcmp(e[i].name, "notes.txt")) {
                char buf[32] = {};
                CHECK(e[i].type == 0x54455854 && v.readFork(e[i], false, 0, buf, e[i].dataLen));
                CHECK(!strcmp(buf, "line one\rline two\r"));          // Mac line endings
            } else {
                CHECK(!strcmp(e[i].name, "picture.gif") && e[i].type == 0x47494666 && e[i].dataLen == 3000);
            }
        }
        CHECK(readFile("transfer/notes.txt") == "<missing>");          // the upload folder is emptied
        r = request("POST", "/makefloppy");
        CHECK(r.code == 400);                                          // nothing to send
        r = request("PUT", "/files/transfer/x.txt", "x");
        r = request("POST", "/makefloppy");
        CHECK(r.code == 200 && r.body.find("Transfer 2.dsk") != std::string::npos);
        // Too much for 800K: a 1.44 MB floppy (this Mac has a SuperDrive).
        r = request("PUT", "/files/transfer/big.bin", std::string(1000000, 'B'));
        r = request("POST", "/makefloppy");
        CHECK(r.code == 200 && r.body.find("Transfer 3.dsk") != std::string::npos);
        CHECK(readFile("disks/Transfer 3.dsk").size() == 1474560);

        // Too much for any floppy: a CD of the size needed, both forks kept.
        std::string mb(128, '\0');
        const char *app = "Big App";
        mb[1] = (char)strlen(app);
        memcpy(&mb[2], app, strlen(app));
        memcpy(&mb[65], "APPLTEST", 8);
        mb[73] = 0x20;                                                 // has a bundle
        const macemu::u32 dl = 3000000, rl = 2500000;
        for (int i = 0; i < 4; i++) { mb[83 + i] = (char)(dl >> (24 - 8 * i)); mb[87 + i] = (char)(rl >> (24 - 8 * i)); }
        std::string dataFork(dl, 'D'), rsrcFork(rl, 'R');
        for (macemu::u32 i = 0; i < dl; i += 4096) dataFork[i] = (char)(i >> 12);
        for (macemu::u32 i = 0; i < rl; i += 4096) rsrcFork[i] = (char)(i >> 13);
        mb += dataFork;
        mb.resize(128 + macemu::macBinaryPad(dl), '\0');
        mb += rsrcFork;
        r = request("PUT", "/files/transfer/Big App.bin", mb);
        CHECK(r.code == 201);
        r = request("PUT", "/files/transfer/movie.mov", std::string(2000000, 'M'));
        CHECK(r.code == 201);
        inserted.clear();
        r = request("POST", "/makefloppy?insert=2");
        CHECK(r.code == 200 && r.body.find("Transfer 4.toast") != std::string::npos && r.body.find("CD-ROM") != std::string::npos);
        CHECK(inserted == "Transfer 4.toast");
        std::string cd = readFile("disks/Transfer 4.toast");
        CHECK(cd.size() % 2048 == 0 && cd.size() > 7500000 && cd.size() < 7700000);
        st.d = &cd;
        macemu::HfsVolume cv;
        CHECK(cv.open(&st) && !strcmp(cv.name(), "Transfer 4"));
        n = cv.list(2, e, 8);
        CHECK(n == 2);
        for (int i = 0; i < n; i++) {
            if (!strcmp(e[i].name, "Big App")) {
                CHECK(e[i].type == 0x4150504C && e[i].dataLen == dl && e[i].rsrcLen == rl);
                std::string d2(dl, 0), r2(rl, 0);
                CHECK(cv.readFork(e[i], false, 0, &d2[0], dl) && d2 == dataFork);
                CHECK(cv.readFork(e[i], true, 0, &r2[0], rl) && r2 == rsrcFork);
            } else {
                CHECK(!strcmp(e[i].name, "movie.mov") && e[i].dataLen == 2000000 && e[i].rsrcLen == 0);
            }
        }
        CHECK(readFile("transfer/movie.mov") == "<missing>");
    }

    // Restart the Mac.
    r = request("POST", "/restart");
    CHECK(r.code == 200 && restarts == 1);
    r = request("POST", "/reboot");
    CHECK(r.code == 200);
    usleep(500000);
    CHECK(server->RebootRequested());

    // The server is still answering after all that.
    r = request("GET", "/");
    CHECK(r.code == 200);

    std::string cmd = "rm -rf " + root;
    if (system(cmd.c_str())) {}
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
