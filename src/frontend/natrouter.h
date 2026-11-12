// Internet for the emulated Mac: a small router between the Mac's Ethernet
// and the host's own network connections, like QEMU's "user" networking.
//
// The Mac sees a private network 10.0.2.0/24:
//   10.0.2.2   the router (gateway); answers ping
//   10.0.2.3   the DNS server (queries go to the host's DNS server)
//   10.0.2.15  the Mac, handed out by DHCP and BootP
// It answers ARP for every address but the Mac's own, so a Mac set up by
// hand with other addresses works too.  UDP and TCP to anywhere else are
// carried by the host's sockets (NAT): the Mac's TCP connections end in the
// router, which opens its own connection to the server and relays the data.
// Connections from the internet to the Mac are not possible.
//
// Freestanding: no C library; the host provides sockets and a clock.

#ifndef MACEMU_NATROUTER_H
#define MACEMU_NATROUTER_H

#include "../core/types.h"

namespace macemu {

// The host's side: non-blocking sockets.  Handles are small integers >= 0.
class NatSockets {
public:
    virtual ~NatSockets() {}
    virtual int udpOpen() = 0;                          // -1 if none left
    virtual bool udpSend(int h, const u8 *ip, u16 port, const u8 *data, u32 len) = 0;
    // Bytes received (from ip:port), 0 if nothing is waiting.
    virtual int udpReceive(int h, u8 *buf, u32 max, u8 *ip, u16 &port) = 0;
    virtual void udpClose(int h) = 0;

    virtual int tcpOpen(const u8 *ip, u16 port) = 0;    // starts connecting; -1 if impossible
    enum { Connecting = 0, Connected = 1, Failed = -1 };
    virtual int tcpState(int h) = 0;
    // Bytes taken, 0 if the socket cannot take more now, -1 if it is broken.
    virtual int tcpSend(int h, const u8 *data, u32 len) = 0;
    // Bytes read, 0 if nothing yet, -1 when the server has closed (and all is read).
    virtual int tcpReceive(int h, u8 *buf, u32 max) = 0;
    virtual void tcpClose(int h) = 0;

    virtual bool dnsServer(u8 *ip) = 0;                 // the host's DNS server
    virtual u32 milliseconds() = 0;
    // Seconds since 1970 (UTC), false if the host does not know yet.
    virtual bool wallClock(u32 &unixTime) { (void)unixTime; return false; }
    virtual void randomBytes(u8 *buf, u32 len) { for (u32 i = 0; i < len; i++) buf[i] = (u8)(milliseconds() * 2654435761u >> 24); }
    virtual void poll() {}                              // called by the router's poll()
};

class NatRouter {
public:
    explicit NatRouter(NatSockets &sockets);
    ~NatRouter();

    // An Ethernet frame from the Mac.  Returns false if it is not for the
    // router (not IPv4 or ARP): AppleTalk, say.
    bool fromMac(const u8 *frame, u32 len);
    // Frames for the Mac, one at a time: length, or 0 if none.  `out` has
    // room for 1514 bytes.
    u32 toMac(u8 *out);
    // Move data between the sockets and the Mac; call often.
    void poll();

    u32 connections() const;                            // TCP connections open now

    enum { kMaxTcp = 24, kMaxUdp = 16, kQueue = 48, kTxBuffer = 16384 };

private:
    struct Tcp {
        bool used;
        int state;
        int handle;
        u8 macIp[4], remoteIp[4];
        u16 macPort, remotePort;
        u32 iss, sndUna, sndNxt, rcvNxt;
        u32 macWindow;
        u32 mss;
        u8 *tx;                         // data from the server, from sndUna on
        u32 txLen;
        bool serverClosed, finSent, macClosed;
        u32 lastSend, lastActivity;
        int retries;
    };
    struct Udp {
        bool used;
        int handle;
        u8 macIp[4];
        u16 macPort;
        u8 remoteIp[4];                 // first destination (replies may come from others)
        u16 remotePort;
        bool dns;                       // the Mac asked 10.0.2.3: answers come from there
        u32 lastActivity;
    };
    struct Frame { u16 len; u8 data[1514]; };

    NatSockets &net;
    u8 macMac[6];                       // the Mac's Ethernet address, once seen
    bool haveMac;
    Tcp tcp[kMaxTcp];
    Udp udp[kMaxUdp];
    Frame *queue;
    int qHead, qTail;
    u16 ipId;
    u32 issClock;
    u8 scratch[1514];

    void send(const u8 *dstMac, u16 type, const u8 *payload, u32 len);
    void sendIp(const u8 *src, const u8 *dst, u8 proto, const u8 *payload, u32 len);
    void arp(const u8 *p, u32 len);
    void ip(const u8 *p, u32 len);
    void icmp(const u8 *iph, const u8 *p, u32 len);
    void udpIn(const u8 *iph, const u8 *p, u32 len);
    void dhcp(const u8 *p, u32 len);
    void tcpIn(const u8 *iph, const u8 *p, u32 len);
    void tcpSegment(Tcp &c, u8 flags, u32 seq, const u8 *data, u32 len);
    void tcpReset(const u8 *iph, const u8 *th, u32 dataLen);
    void tcpFree(Tcp &c);
    void tcpPush(Tcp &c);
    Tcp *tcpFind(const u8 *macIp, u16 macPort, const u8 *remoteIp, u16 remotePort);
    static u16 checksum(const u8 *p, u32 len, u32 sum = 0);
    static u32 pseudo(const u8 *src, const u8 *dst, u8 proto, u32 len);
};

} // namespace macemu

#endif
