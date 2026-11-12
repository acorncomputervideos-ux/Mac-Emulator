// The HTTPS helper: a browser's requests through HttpsProxy to a local
// server (always) and to real sites over HTTPS (NET_TESTS=1).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "../src/host/natposix.h"
#include "../src/frontend/httpsproxy.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// One request as the browser sends it; the whole answer back.
static std::string fetch(HttpsProxy &p, const std::string &request, u16 port = 8080, const u8 *dest = 0, int ms = 20000)
{
    static const u8 helper[4] = {10, 0, 2, 2};
    int h = p.tcpOpen(dest ? dest : helper, port);
    if (h < 0) return "(no connection)";
    p.tcpSend(h, (const u8 *)request.data(), (u32)request.size());
    std::string out;
    for (int i = 0; i < ms; i++) {
        p.poll();
        u8 buf[4096];
        int n;
        while ((n = p.tcpReceive(h, buf, sizeof buf)) > 0) out.append((const char *)buf, (size_t)n);
        if (n < 0) break;
        usleep(1000);
    }
    p.tcpClose(h);
    return out;
}

static bool has(const std::string &s, const char *what) { return s.find(what) != std::string::npos; }

// A plain HTTP server that answers once, chunked, in UTF-8.
static u16 pageServer(int &lfd, std::string &seen)
{
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = {};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lfd, (sockaddr *)&a, sizeof a);
    listen(lfd, 2);
    socklen_t l = sizeof a;
    getsockname(lfd, (sockaddr *)&a, &l);
    std::thread([lfd, &seen] {
        int c = accept(lfd, 0, 0);
        char req[2048] = {};
        ssize_t n = recv(c, req, sizeof req - 1, 0);
        if (n > 0) seen.assign(req, (size_t)n);
        const char *page =
            "<html><a href=\"https://example.com/x\">link</a> caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x82\xAC" "5 "
            "<img src=\"HTTPS://img.example/a.gif\"> http://keep.me/ \xE6\x97\xA5</html>";
        std::string body = page;
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                           "Transfer-Encoding: chunked\r\nStrict-Transport-Security: max-age=1\r\n"
                           "Set-Cookie: id=1; Secure; Path=/\r\nLocation: https://example.com/next\r\n\r\n";
        // Two chunks, split in the middle of "https://" and of a UTF-8 character.
        size_t cut = body.find("tps://");
        std::string a1 = body.substr(0, cut), a2 = body.substr(cut);
        char sz[16];
        snprintf(sz, sizeof sz, "%zx\r\n", a1.size()); resp += sz; resp += a1; resp += "\r\n";
        snprintf(sz, sizeof sz, "%zx\r\n", a2.size()); resp += sz; resp += a2; resp += "\r\n0\r\n\r\n";
        send(c, resp.data(), resp.size(), 0);
        close(c);
    }).detach();
    return ntohs(a.sin_port);
}

int main()
{
    PosixNatSockets sockets;
    HttpsProxy proxy(sockets);

    // The helper's own page, and CONNECT refused.
    std::string r = fetch(proxy, "GET http://10.0.2.2/ HTTP/1.0\r\n\r\n");
    CHECK(has(r, "HTTP/1.0 200") && has(r, "HTTPS helper"));
    r = fetch(proxy, "CONNECT example.com:443 HTTP/1.0\r\n\r\n");
    CHECK(has(r, "HTTP/1.0 501"));

    // A plain HTTP site on its own port: chunks joined, links and text fixed.
    {
        int lfd;
        std::string seen;
        u16 port = pageServer(lfd, seen);
        std::string req = "GET http://127.0.0.1:" + std::to_string(port) +
                          "/page.html HTTP/1.0\r\nUser-Agent: Mozilla/3.0 (Macintosh; I; 68K)\r\n"
                          "Proxy-Connection: Keep-Alive\r\nAccept-Encoding: gzip\r\n\r\n";
        r = fetch(proxy, req, 8080, 0, 3000);
        CHECK(has(seen, "GET /page.html HTTP/1.1\r\n"));
        CHECK(has(seen, "Host: 127.0.0.1:"));
        CHECK(has(seen, "Accept-Encoding: identity") && !has(seen, "gzip") && !has(seen, "Proxy-Connection"));
        CHECK(has(seen, "User-Agent: Mozilla/3.0"));
        CHECK(has(r, "HTTP/1.0 200 OK"));
        CHECK(!has(r, "Transfer-Encoding") && !has(r, "Strict-Transport") && !has(r, "charset"));
        CHECK(has(r, "Set-Cookie: id=1; Path=/") && has(r, "Location: http://example.com/next"));
        CHECK(has(r, "href=\"http://example.com/x\"") && has(r, "src=\"http://img.example/a.gif\""));
        CHECK(!has(r, "https://") && has(r, "http://keep.me/"));
        CHECK(has(r, "caf\x8E"));
        CHECK(has(r, "\xD2quoted\xD3"));
        CHECK(has(r, "\xDB" "5"));
        CHECK(has(r, "/ ?</html>"));                    // a character Mac Roman lacks
        if (failures) { for (unsigned char ch : r) { if (ch >= 32 && ch < 127) putchar(ch); else printf("<%02X>", ch); } puts(""); }
        CHECK(!has(r, "\r\n0\r\n"));                    // no chunk framing left
        close(lfd);
    }

    // Real sites (needs the internet).
    if (getenv("NET_TESTS")) {
        // Without trusted roots: refused, not downgraded.
        r = fetch(proxy, "GET http://example.com/ HTTP/1.0\r\n\r\n");
        CHECK(has(r, "Certificate problem") && has(r, "not issued by a trusted authority"));
        CHECK(proxy.certificateErrors == 1);

        const char *roots = getenv("HTTPS_ROOTS") ? getenv("HTTPS_ROOTS") : "sdcard/certs/cacert.pem";
        for (const char *path = roots; *path; ) {
            std::string one(path, strcspn(path, ":"));
            path += one.size() + (path[one.size()] == ':');
            FILE *f = fopen(one.c_str(), "rb");
            if (!f) continue;
            std::string pem;
            char b[65536];
            size_t n;
            while ((n = fread(b, 1, sizeof b, f)) > 0) pem.append(b, n);
            fclose(f);
            printf("%u roots from %s\n", proxy.loadRoots(pem.data(), (u32)pem.size()), one.c_str());
        }
        CHECK(proxy.roots() > 100);
        r = fetch(proxy, "GET http://example.com/ HTTP/1.0\r\nUser-Agent: Mozilla/3.0\r\n\r\n");
        printf("--- example.com via HTTPS:\n%.300s\n---\n", r.c_str());
        CHECK(has(r, "HTTP/1.0 200") && has(r, "Example Domain"));
        // Through the Mac's own port 80 connection (no proxy set): Host header.
        static const u8 someIp[4] = {93, 184, 215, 14};
        r = fetch(proxy, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n", 80, someIp);
        CHECK(has(r, "HTTP/1.0 200") && has(r, "Example Domain"));
        // No clock on the host (NTP blocked): the date comes from the site.
        {
            PosixNatSockets noClock;
            noClock.clockKnown = false;
            HttpsProxy p2(noClock);
            const char *rp = getenv("HTTPS_ROOTS") ? getenv("HTTPS_ROOTS") : "sdcard/certs/cacert.pem";
            for (const char *path = rp; *path; ) {
                std::string one(path, strcspn(path, ":"));
                path += one.size() + (path[one.size()] == ':');
                FILE *f = fopen(one.c_str(), "rb");
                if (!f) continue;
                std::string pem; char b[65536]; size_t n;
                while ((n = fread(b, 1, sizeof b, f)) > 0) pem.append(b, n);
                fclose(f);
                p2.loadRoots(pem.data(), (u32)pem.size());
            }
            r = fetch(p2, "GET http://example.com/ HTTP/1.0\r\n\r\n");
            CHECK(has(r, "HTTP/1.0 200") && has(r, "Example Domain"));
            if (!has(r, "Example Domain")) printf("--- without a clock:\n%.600s\n---\n", r.c_str());
        }
        r = fetch(proxy, "GET http://no-such-host.invalid/ HTTP/1.0\r\n\r\n");
        CHECK(has(r, "No such site") || has(r, "No answer from the name server"));
        printf("pages fetched: %u\n", proxy.pagesFetched);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
