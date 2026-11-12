// The Mac's internet router (natrouter): a fake Mac's TCP/IP stack talks
// through it to real servers on this machine.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "../src/host/natposix.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const u8 macEther[6] = {0x08, 0x00, 0x07, 0x01, 0x02, 0x03};
static const u8 macIp[4] = {10, 0, 2, 15};
static const u8 gw[4] = {10, 0, 2, 2};
static const u8 lo[4] = {127, 0, 0, 1};

static u16 csum(const u8 *p, u32 n, u32 s = 0)
{
    for (u32 i = 0; i + 1 < n; i += 2) s += (u32)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (u32)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}
static u32 pseudo(const u8 *a, const u8 *b, u8 proto, u32 len)
{
    return (u32)((a[0] << 8) | a[1]) + (u32)((a[2] << 8) | a[3]) + (u32)((b[0] << 8) | b[1]) + (u32)((b[2] << 8) | b[3]) + proto + len;
}

struct FakeMac {
    NatRouter &r;
    std::vector<std::vector<u8>> got;
    explicit FakeMac(NatRouter &router) : r(router) {}

    void ether(const u8 *dst, u16 type, const u8 *p, u32 n) {
        std::vector<u8> f(14 + n);
        memcpy(&f[0], dst, 6); memcpy(&f[6], macEther, 6); putBe16(&f[12], type);
        memcpy(&f[14], p, n);
        CHECK(r.fromMac(f.data(), (u32)f.size()));
    }
    void ip(const u8 *src, const u8 *dst, u8 proto, const u8 *p, u32 n) {
        std::vector<u8> pk(20 + n);
        pk[0] = 0x45; putBe16(&pk[2], (u16)(20 + n)); pk[8] = 64; pk[9] = proto;
        memcpy(&pk[12], src, 4); memcpy(&pk[16], dst, 4);
        putBe16(&pk[10], csum(pk.data(), 20));
        memcpy(&pk[20], p, n);
        static const u8 router[6] = {0x52, 0x54, 0x00, 0x12, 0x35, 0x02};
        ether(router, 0x0800, pk.data(), (u32)pk.size());
    }
    void udp(const u8 *dst, u16 sport, u16 dport, const void *d, u32 n, const u8 *src = macIp) {
        std::vector<u8> u(8 + n);
        putBe16(&u[0], sport); putBe16(&u[2], dport); putBe16(&u[4], (u16)(8 + n));
        memcpy(&u[8], d, n);
        putBe16(&u[6], csum(u.data(), (u32)u.size(), pseudo(src, dst, 17, (u32)u.size())));
        ip(src, dst, 17, u.data(), (u32)u.size());
    }
    void tcp(const u8 *dst, u16 sport, u16 dport, u32 seq, u32 ack, u8 flags, const void *d = 0, u32 n = 0, u16 win = 16384) {
        std::vector<u8> t(24 + n);
        putBe16(&t[0], sport); putBe16(&t[2], dport); putBe32(&t[4], seq); putBe32(&t[8], ack);
        u32 hl = (flags & 2) ? 24 : 20;
        t.resize(hl + n);
        t[12] = (u8)((hl / 4) << 4); t[13] = flags; putBe16(&t[14], win);
        if (hl == 24) { t[20] = 2; t[21] = 4; putBe16(&t[22], 1460); }
        if (n) memcpy(&t[hl], d, n);
        putBe16(&t[16], csum(t.data(), (u32)t.size(), pseudo(macIp, dst, 6, (u32)t.size())));
        ip(macIp, dst, 6, t.data(), (u32)t.size());
    }
    // Run the router until frames come back (or time passes).
    void pump(int ms = 50) {
        for (int i = 0; i < ms; i++) {
            r.poll();
            u8 f[1514];
            while (u32 n = r.toMac(f)) got.emplace_back(f, f + n);
            usleep(1000);
        }
    }
    // The next IPv4 packet of protocol `proto` (its IP header on), or empty.
    std::vector<u8> next(u8 proto) {
        for (size_t i = 0; i < got.size(); i++) {
            std::vector<u8> &f = got[i];
            if (f.size() > 34 && be16(&f[12]) == 0x0800 && f[23] == proto) {
                std::vector<u8> p(f.begin() + 14, f.end());
                got.erase(got.begin() + (long)i);
                return p;
            }
        }
        return {};
    }
};

static int tcpServer(u16 &port, int &lfd)
{
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = {};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lfd, (sockaddr *)&a, sizeof a);
    listen(lfd, 4);
    socklen_t l = sizeof a;
    getsockname(lfd, (sockaddr *)&a, &l);
    port = ntohs(a.sin_port);
    return lfd;
}

int main()
{
    PosixNatSockets sockets;
    NatRouter router(sockets);
    FakeMac mac(router);

    // Not for the router: AppleTalk.
    u8 aarp[60] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};
    putBe16(aarp + 12, 0x0024);
    CHECK(!router.fromMac(aarp, 60));

    // ARP: any address but the asker's own.
    {
        u8 a[28] = {0, 1, 8, 0, 6, 4, 0, 1};
        memcpy(a + 8, macEther, 6); memcpy(a + 14, macIp, 4); memcpy(a + 24, gw, 4);
        static const u8 bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        mac.ether(bc, 0x0806, a, 28);
        mac.pump(1);
        CHECK(mac.got.size() == 1 && be16(&mac.got[0][12]) == 0x0806 && be16(&mac.got[0][20]) == 2);
        CHECK(!memcmp(&mac.got[0][28], gw, 4) && !memcmp(&mac.got[0][0], macEther, 6));
        mac.got.clear();
        memcpy(a + 24, macIp, 4);                       // its own address: no answer
        mac.ether(bc, 0x0806, a, 28);
        memset(a + 14, 0, 4); memcpy(a + 24, macIp, 4); // a probe: no answer
        mac.ether(bc, 0x0806, a, 28);
        mac.pump(1);
        CHECK(mac.got.empty());
    }

    // DHCP discover -> offer, with the router and DNS.
    {
        std::vector<u8> d(300, 0);
        d[0] = 1; d[1] = 1; d[2] = 6; putBe32(&d[4], 0x12345678);
        memcpy(&d[28], macEther, 6);
        putBe32(&d[236], 0x63825363);
        d[240] = 53; d[241] = 1; d[242] = 1; d[243] = 255;
        static const u8 zero[4] = {0, 0, 0, 0}, all[4] = {255, 255, 255, 255};
        mac.udp(all, 68, 67, d.data(), 300, zero);
        mac.pump(1);
        std::vector<u8> p = mac.next(17);
        CHECK(p.size() > 28 + 240);
        if (p.size() > 28 + 240) {
            const u8 *b = &p[28];
            CHECK(be16(&p[22]) == 68 && b[0] == 2 && be32(b + 4) == 0x12345678 && !memcmp(b + 16, macIp, 4));
            CHECK(b[240] == 53 && b[242] == 2);         // an offer
            CHECK(csum(&p[0], 20) == 0);
        }
    }

    // Ping the router.
    {
        u8 e[16] = {8, 0, 0, 0, 0, 1, 0, 1, 'h', 'e', 'l', 'l', 'o', '!', '!', '!'};
        putBe16(e + 2, csum(e, 16));
        mac.ip(macIp, gw, 1, e, 16);
        mac.pump(1);
        std::vector<u8> p = mac.next(1);
        CHECK(p.size() == 36 && p[20] == 0 && !memcmp(&p[28], "hello!!!", 8) && !memcmp(&p[12], gw, 4));
    }

    // UDP out and back (an echo server here), and DNS through 10.0.2.3.
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a = {};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(s, (sockaddr *)&a, sizeof a);
        socklen_t l = sizeof a;
        getsockname(s, (sockaddr *)&a, &l);
        u16 port = ntohs(a.sin_port);
        std::thread echo([s] {
            for (int i = 0; i < 2; i++) {
                char b[512]; sockaddr_in f; socklen_t fl = sizeof f;
                ssize_t n = recvfrom(s, b, sizeof b, 0, (sockaddr *)&f, &fl);
                if (n > 0) sendto(s, b, (size_t)n, 0, (sockaddr *)&f, fl);
            }
        });
        mac.udp(lo, 5000, port, "ping over udp", 13);
        mac.pump(30);
        std::vector<u8> p = mac.next(17);
        CHECK(p.size() == 28 + 13 && !memcmp(&p[28], "ping over udp", 13) && be16(&p[20]) == port && be16(&p[22]) == 5000);
        if (p.size() > 28) CHECK(csum(&p[20], (u32)p.size() - 20, pseudo(&p[12], &p[16], 17, (u32)p.size() - 20)) == 0);

        memcpy(sockets.dns, lo, 4);                      // "the host's DNS server" is the echo server
        sockets.dnsPort = port;
        static const u8 dnsAddr[4] = {10, 0, 2, 3};
        mac.udp(dnsAddr, 5001, 53, "a dns query", 11);
        mac.pump(30);
        p = mac.next(17);
        CHECK(p.size() == 28 + 11 && !memcmp(&p[12], dnsAddr, 4) && be16(&p[20]) == 53 && be16(&p[22]) == 5001);
        echo.join();
        close(s);
    }

    // TCP: connect, send a request, receive 200 KB, then the server closes.
    {
        u16 port; int lfd;
        tcpServer(port, lfd);
        const u32 kBig = 200000;
        std::atomic<bool> gotRequest(false);
        std::thread server([&] {
            int c = accept(lfd, 0, 0);
            char req[64] = {};
            ssize_t n = recv(c, req, sizeof req, 0);
            gotRequest = n == 4 && !memcmp(req, "GET\n", 4);
            std::vector<u8> big(kBig);
            for (u32 i = 0; i < kBig; i++) big[i] = (u8)(i * 7);
            u32 sent = 0;
            while (sent < kBig) { ssize_t k = send(c, big.data() + sent, kBig - sent, 0); if (k <= 0) break; sent += (u32)k; }
            close(c);
        });
        u32 iss = 1000;
        mac.tcp(lo, 6000, port, iss, 0, 0x02);           // SYN
        std::vector<u8> p;
        for (int i = 0; i < 20 && p.empty(); i++) { mac.pump(10); p = mac.next(6); }
        CHECK(!p.empty() && p[20 + 13] == 0x12);          // SYN+ACK
        u32 their = p.empty() ? 0 : be32(&p[24]) + 1;
        CHECK(!p.empty() && be32(&p[28]) == iss + 1);
        mac.tcp(lo, 6000, port, iss + 1, their, 0x10);   // ACK
        mac.tcp(lo, 6000, port, iss + 1, their, 0x18, "GET\n", 4);
        std::vector<u8> recvd;
        bool fin = false;
        for (int round = 0; round < 3000 && !fin; round++) {
            mac.pump(2);
            while (!(p = mac.next(6)).empty()) {
                u32 hl = (u32)(p[32] >> 4) * 4;
                u32 n = (u32)p.size() - 20 - hl;
                u32 seq = be32(&p[24]);
                if (seq == their && n) { recvd.insert(recvd.end(), p.begin() + 20 + hl, p.end()); their += n; }
                if ((p[33] & 0x01) && seq + n == their) { their++; fin = true; }
                mac.tcp(lo, 6000, port, iss + 5, their, 0x10);
            }
        }
        CHECK(gotRequest);
        CHECK(recvd.size() == kBig);
        bool same = recvd.size() == kBig;
        for (u32 i = 0; same && i < kBig; i++) if (recvd[i] != (u8)(i * 7)) same = false;
        CHECK(same);
        CHECK(fin);
        mac.tcp(lo, 6000, port, iss + 5, their, 0x11);   // our FIN
        mac.pump(5);
        CHECK(router.connections() == 0);
        server.join();
        close(lfd);
    }

    // A refused connection is reset; a stray segment too.
    {
        u16 port; int lfd;
        tcpServer(port, lfd);
        close(lfd);                                      // nobody listens there now
        mac.got.clear();
        mac.tcp(lo, 6001, port, 5000, 0, 0x02);
        std::vector<u8> p;
        for (int i = 0; i < 50 && p.empty(); i++) { mac.pump(5); p = mac.next(6); }
        CHECK(!p.empty() && (p[33] & 0x04));
        mac.tcp(lo, 6002, port, 7000, 1, 0x10, "x", 1);
        mac.pump(1);
        p = mac.next(6);
        CHECK(!p.empty() && (p[33] & 0x04));
        CHECK(router.connections() == 0);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
