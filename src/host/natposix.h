// NatSockets on POSIX non-blocking sockets, for the host runner and tests.

#ifndef MACEMU_NATPOSIX_H
#define MACEMU_NATPOSIX_H

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include "../frontend/natrouter.h"

namespace macemu {

class PosixNatSockets : public NatSockets {
public:
    u8 dns[4] = {0, 0, 0, 0};           // the DNS server; 0.0.0.0 = from /etc/resolv.conf
    u16 dnsPort = 53;                   // for tests

    ~PosixNatSockets() override { for (int fd : fds) if (fd >= 0) close(fd); }

    int udpOpen() override {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return -1;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        return add(fd, true);
    }
    bool udpSend(int h, const u8 *ip, u16 port, const u8 *data, u32 len) override {
        sockaddr_in a = addr(ip, port);
        if (!memcmp(ip, dns, 4) && port == 53) a.sin_port = htons(dnsPort);
        return sendto(fds[h], data, len, 0, (sockaddr *)&a, sizeof a) == (ssize_t)len;
    }
    int udpReceive(int h, u8 *buf, u32 max, u8 *ip, u16 &port) override {
        sockaddr_in a = {};
        socklen_t al = sizeof a;
        ssize_t n = recvfrom(fds[h], buf, max, 0, (sockaddr *)&a, &al);
        if (n <= 0) return 0;
        memcpy(ip, &a.sin_addr, 4);
        port = ntohs(a.sin_port);
        return (int)n;
    }
    void udpClose(int h) override { drop(h); }

    int tcpOpen(const u8 *ip, u16 port) override {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        sockaddr_in a = addr(ip, port);
        if (connect(fd, (sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(fd); return -1; }
        return add(fd, false);
    }
    int tcpState(int h) override {
        int fd = fds[h];
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        timeval t = {0, 0};
        if (select(fd + 1, 0, &w, 0, &t) <= 0) return Connecting;
        int err = 0;
        socklen_t l = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        return err ? Failed : Connected;
    }
    int tcpSend(int h, const u8 *data, u32 len) override {
        ssize_t n = send(fds[h], data, len, MSG_NOSIGNAL);
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        return (int)n;
    }
    int tcpReceive(int h, u8 *buf, u32 max) override {
        ssize_t n = recv(fds[h], buf, max, 0);
        if (n == 0) return -1;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        return (int)n;
    }
    void tcpClose(int h) override { drop(h); }

    bool dnsServer(u8 *ip) override {
        if (!(dns[0] | dns[1] | dns[2] | dns[3])) {
            if (FILE *f = fopen("/etc/resolv.conf", "r")) {
                char line[256];
                while (fgets(line, sizeof line, f)) {
                    char a[64];
                    if (sscanf(line, "nameserver %63s", a) == 1 && inet_pton(AF_INET, a, dns) == 1) break;
                }
                fclose(f);
            }
            if (!(dns[0] | dns[1] | dns[2] | dns[3])) { dns[0] = 8; dns[1] = 8; dns[2] = 8; dns[3] = 8; }
        }
        memcpy(ip, dns, 4);
        return true;
    }
    bool clockKnown = true;             // tests: a host without a clock (no NTP yet)
    bool wallClock(u32 &t) override { t = (u32)time(nullptr); return clockKnown; }
    void randomBytes(u8 *buf, u32 len) override {
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f || fread(buf, 1, len, f) != len) for (u32 i = 0; i < len; i++) buf[i] = (u8)rand();
        if (f) fclose(f);
    }
    u32 milliseconds() override {
        timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (u32)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
    }

private:
    std::vector<int> fds;
    static sockaddr_in addr(const u8 *ip, u16 port) {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        memcpy(&a.sin_addr, ip, 4);
        return a;
    }
    int add(int fd, bool) {
        for (size_t i = 0; i < fds.size(); i++) if (fds[i] < 0) { fds[i] = fd; return (int)i; }
        fds.push_back(fd);
        return (int)fds.size() - 1;
    }
    void drop(int h) { if (h >= 0 && h < (int)fds.size() && fds[h] >= 0) { close(fds[h]); fds[h] = -1; } }
};

} // namespace macemu

#endif
