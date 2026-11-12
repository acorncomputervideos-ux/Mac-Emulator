#include "natrouter.h"

namespace macemu {

static const u8 kRouterMac[6] = {0x52, 0x54, 0x00, 0x12, 0x35, 0x02};
static const u8 kGateway[4] = {10, 0, 2, 2};
static const u8 kDns[4] = {10, 0, 2, 3};
static const u8 kMacIp[4] = {10, 0, 2, 15};
static const u8 kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum { FIN = 0x01, SYN = 0x02, RST = 0x04, PSH = 0x08, ACK = 0x10 };
enum { TcpConnecting = 1, TcpSynAckSent = 2, TcpOpen = 3 };
enum { kWindow = 8760, kRto = 800, kMaxRetries = 10, kTcpIdleMs = 600000, kUdpIdleMs = 120000 };

static bool same4(const u8 *a, const u8 *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }
static bool seqLess(u32 a, u32 b) { return (s32)(a - b) < 0; }
static bool seqLessEq(u32 a, u32 b) { return (s32)(a - b) <= 0; }

NatRouter::NatRouter(NatSockets &sockets) : net(sockets)
{
    haveMac = false;
    memFill(macMac, 0, sizeof macMac);
    memFill(tcp, 0, sizeof tcp);
    memFill(udp, 0, sizeof udp);
    queue = new Frame[kQueue];
    qHead = qTail = 0;
    ipId = 1;
    issClock = 0x10000;
}

NatRouter::~NatRouter()
{
    for (Tcp &c : tcp) if (c.used) tcpFree(c);
    for (Udp &u : udp) if (u.used) net.udpClose(u.handle);
    delete[] queue;
}

u32 NatRouter::connections() const
{
    u32 n = 0;
    for (const Tcp &c : tcp) if (c.used) n++;
    return n;
}

// ---------------------------------------------------------------- output

u16 NatRouter::checksum(const u8 *p, u32 len, u32 sum)
{
    for (u32 i = 0; i + 1 < len; i += 2) sum += (u32)((p[i] << 8) | p[i + 1]);
    if (len & 1) sum += (u32)(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

u32 NatRouter::pseudo(const u8 *src, const u8 *dst, u8 proto, u32 len)
{
    return (u32)((src[0] << 8) | src[1]) + (u32)((src[2] << 8) | src[3]) +
           (u32)((dst[0] << 8) | dst[1]) + (u32)((dst[2] << 8) | dst[3]) + proto + len;
}

void NatRouter::send(const u8 *dstMac, u16 type, const u8 *payload, u32 len)
{
    if (len > 1500) return;
    int next = (qHead + 1) % kQueue;
    if (next == qTail) return;                          // the Mac is behind: drop (TCP resends)
    Frame &f = queue[qHead];
    memCopy(f.data, dstMac, 6);
    memCopy(f.data + 6, kRouterMac, 6);
    putBe16(f.data + 12, type);
    memCopy(f.data + 14, payload, len);
    f.len = (u16)(14 + len);
    qHead = next;
}

u32 NatRouter::toMac(u8 *out)
{
    if (qTail == qHead) return 0;
    Frame &f = queue[qTail];
    u32 n = f.len;
    memCopy(out, f.data, n);
    qTail = (qTail + 1) % kQueue;
    return n;
}

void NatRouter::sendIp(const u8 *src, const u8 *dst, u8 proto, const u8 *payload, u32 len)
{
    static u8 pkt[1500];
    if (len > 1480) return;
    u8 *h = pkt;
    h[0] = 0x45; h[1] = 0;
    putBe16(h + 2, (u16)(20 + len));
    putBe16(h + 4, ipId++);
    putBe16(h + 6, 0x4000);                             // don't fragment
    h[8] = 64; h[9] = proto;
    putBe16(h + 10, 0);
    memCopy(h + 12, src, 4);
    memCopy(h + 16, dst, 4);
    putBe16(h + 10, checksum(h, 20));
    memCopy(pkt + 20, payload, len);
    send(haveMac ? macMac : kBroadcastMac, 0x0800, pkt, 20 + len);
}

// ---------------------------------------------------------------- input

bool NatRouter::fromMac(const u8 *f, u32 len)
{
    if (len < 14) return false;
    u16 type = be16(f + 12);
    if (type != 0x0800 && type != 0x0806) return false;
    if (!(f[6] & 1)) { memCopy(macMac, f + 6, 6); haveMac = true; }
    if (type == 0x0806) arp(f + 14, len - 14);
    else ip(f + 14, len - 14);
    return true;
}

// ARP: the router answers for every address except the asker's own, so
// that whatever the Mac's gateway is set to, its packets come here.
void NatRouter::arp(const u8 *p, u32 len)
{
    if (len < 28 || be16(p) != 1 || be16(p + 2) != 0x0800 || p[4] != 6 || p[5] != 4) return;
    if (be16(p + 6) != 1) return;                       // requests only
    const u8 *spa = p + 14, *tpa = p + 24;
    if ((spa[0] | spa[1] | spa[2] | spa[3]) == 0) return;   // an address probe: stay quiet
    if (same4(spa, tpa)) return;                        // a gratuitous announcement
    u8 r[28];
    putBe16(r, 1); putBe16(r + 2, 0x0800); r[4] = 6; r[5] = 4;
    putBe16(r + 6, 2);
    memCopy(r + 8, kRouterMac, 6);
    memCopy(r + 14, tpa, 4);
    memCopy(r + 18, p + 8, 6);
    memCopy(r + 24, spa, 4);
    send(p + 8, 0x0806, r, 28);
}

void NatRouter::ip(const u8 *p, u32 len)
{
    if (len < 20 || (p[0] >> 4) != 4) return;
    u32 ihl = (u32)(p[0] & 15) * 4;
    u32 total = be16(p + 2);
    if (ihl < 20 || total < ihl || total > len) return;
    if (be16(p + 6) & 0x3FFF) return;                   // fragments: not supported
    if (checksum(p, ihl) != 0) return;
    const u8 *payload = p + ihl;
    u32 plen = total - ihl;
    switch (p[9]) {
    case 1: icmp(p, payload, plen); break;
    case 6: tcpIn(p, payload, plen); break;
    case 17: udpIn(p, payload, plen); break;
    default: break;
    }
}

// Ping: the router and the DNS address answer.  (Pings beyond would need
// raw sockets, which the host does not offer.)
void NatRouter::icmp(const u8 *iph, const u8 *p, u32 len)
{
    if (len < 8 || p[0] != 8) return;
    if (!same4(iph + 16, kGateway) && !same4(iph + 16, kDns)) return;
    if (checksum(p, len) != 0) return;
    memCopy(scratch, p, len);
    scratch[0] = 0;                                     // echo reply
    putBe16(scratch + 2, 0);
    putBe16(scratch + 2, checksum(scratch, len));
    sendIp(iph + 16, iph + 12, 1, scratch, len);
}

// ---------------------------------------------------------------- UDP

void NatRouter::udpIn(const u8 *iph, const u8 *p, u32 len)
{
    if (len < 8) return;
    u32 ulen = be16(p + 4);
    if (ulen < 8 || ulen > len) return;
    u16 sport = be16(p), dport = be16(p + 2);
    if (dport == 67) { dhcp(p + 8, ulen - 8); return; }
    const u8 *dst = iph + 16;
    if (dst[0] == 255 || (dst[0] == 10 && dst[1] == 0 && dst[2] == 2 && dst[3] == 255)) return;   // broadcasts stay here

    bool dns = same4(dst, kDns) && dport == 53;
    u8 to[4];
    if (dns) { if (!net.dnsServer(to)) return; }
    else memCopy(to, dst, 4);

    Udp *u = 0, *freeSlot = 0, *oldest = 0;
    for (Udp &s : udp) {
        if (s.used && s.macPort == sport && same4(s.macIp, iph + 12) && s.dns == dns) { u = &s; break; }
        if (!s.used && !freeSlot) freeSlot = &s;
        if (s.used && (!oldest || s.lastActivity < oldest->lastActivity)) oldest = &s;
    }
    if (!u) {
        if (!freeSlot) { net.udpClose(oldest->handle); oldest->used = false; freeSlot = oldest; }
        int h = net.udpOpen();
        if (h < 0) return;
        u = freeSlot;
        u->used = true;
        u->handle = h;
        memCopy(u->macIp, iph + 12, 4);
        u->macPort = sport;
        u->dns = dns;
    }
    memCopy(u->remoteIp, to, 4);
    u->remotePort = dport;
    u->lastActivity = net.milliseconds();
    net.udpSend(u->handle, to, dport, p + 8, ulen - 8);
}

// DHCP and BootP: the Mac is always 10.0.2.15.
void NatRouter::dhcp(const u8 *p, u32 len)
{
    if (len < 236 || p[0] != 1) return;
    int type = 0;                                       // 0: plain BootP
    if (len >= 240 && be32(p + 236) == 0x63825363) {
        for (u32 i = 240; i < len && p[i] != 255; ) {
            if (p[i] == 0) { i++; continue; }
            if (i + 1 >= len) break;
            u32 l = p[i + 1];
            if (p[i] == 53 && l >= 1 && i + 2 < len) type = p[i + 2];
            i += 2 + l;
        }
    }
    if (type != 0 && type != 1 && type != 3) return;    // discover, request (or BootP)

    u8 r[548];
    memFill(r, 0, sizeof r);
    r[0] = 2; r[1] = 1; r[2] = 6;
    memCopy(r + 4, p + 4, 4);                           // xid
    memCopy(r + 10, p + 10, 2);                         // flags
    memCopy(r + 16, kMacIp, 4);                         // yiaddr
    memCopy(r + 20, kGateway, 4);                       // siaddr
    memCopy(r + 28, p + 28, 16);                        // chaddr
    putBe32(r + 236, 0x63825363);
    u32 o = 240;
    if (type) { r[o++] = 53; r[o++] = 1; r[o++] = type == 1 ? 2 : 5; }   // offer / ack
    r[o++] = 54; r[o++] = 4; memCopy(r + o, kGateway, 4); o += 4;
    if (type) { r[o++] = 51; r[o++] = 4; putBe32(r + o, 86400); o += 4; }
    r[o++] = 1; r[o++] = 4; r[o++] = 255; r[o++] = 255; r[o++] = 255; r[o++] = 0;
    r[o++] = 3; r[o++] = 4; memCopy(r + o, kGateway, 4); o += 4;
    r[o++] = 6; r[o++] = 4; memCopy(r + o, kDns, 4); o += 4;
    r[o++] = 255;
    if (o < 300) o = 300;                               // BootP's minimum

    u8 *pkt = scratch;
    putBe16(pkt, 67); putBe16(pkt + 2, 68);
    putBe16(pkt + 4, (u16)(8 + o)); putBe16(pkt + 6, 0);
    memCopy(pkt + 8, r, o);
    static const u8 everyone[4] = {255, 255, 255, 255};
    putBe16(pkt + 6, checksum(pkt, 8 + o, pseudo(kGateway, everyone, 17, 8 + o)));
    // To the broadcast address: the Mac has no address to answer on yet.
    static u8 ipkt[600];
    u8 *h = ipkt;
    h[0] = 0x45; h[1] = 0; putBe16(h + 2, (u16)(28 + o)); putBe16(h + 4, ipId++);
    putBe16(h + 6, 0); h[8] = 64; h[9] = 17; putBe16(h + 10, 0);
    memCopy(h + 12, kGateway, 4); memCopy(h + 16, everyone, 4);
    putBe16(h + 10, checksum(h, 20));
    memCopy(ipkt + 20, pkt, 8 + o);
    send(kBroadcastMac, 0x0800, ipkt, 28 + o);
}

// ---------------------------------------------------------------- TCP

NatRouter::Tcp *NatRouter::tcpFind(const u8 *macIp, u16 macPort, const u8 *remoteIp, u16 remotePort)
{
    for (Tcp &c : tcp)
        if (c.used && c.macPort == macPort && c.remotePort == remotePort &&
            same4(c.macIp, macIp) && same4(c.remoteIp, remoteIp)) return &c;
    return 0;
}

void NatRouter::tcpFree(Tcp &c)
{
    if (c.handle >= 0) net.tcpClose(c.handle);
    delete[] c.tx;
    memFill(&c, 0, sizeof c);
}

// A segment to the Mac, from the remote end of connection c.
void NatRouter::tcpSegment(Tcp &c, u8 flags, u32 seq, const u8 *data, u32 len)
{
    u8 *t = scratch;
    u32 hl = (flags & SYN) ? 24 : 20;
    putBe16(t, c.remotePort); putBe16(t + 2, c.macPort);
    putBe32(t + 4, seq); putBe32(t + 8, c.rcvNxt);
    t[12] = (u8)((hl / 4) << 4); t[13] = flags;
    putBe16(t + 14, kWindow); putBe16(t + 16, 0); putBe16(t + 18, 0);
    if (flags & SYN) { t[20] = 2; t[21] = 4; putBe16(t + 22, 1460); }   // our MSS
    if (len) memCopy(t + hl, data, len);
    putBe16(t + 16, checksum(t, hl + len, pseudo(c.remoteIp, c.macIp, 6, hl + len)));
    sendIp(c.remoteIp, c.macIp, 6, t, hl + len);
    c.lastSend = net.milliseconds();
}

// RST for a segment that belongs to no connection.
void NatRouter::tcpReset(const u8 *iph, const u8 *th, u32 dataLen)
{
    u8 flags = th[13];
    if (flags & RST) return;
    u8 *t = scratch;
    putBe16(t, be16(th + 2)); putBe16(t + 2, be16(th));
    if (flags & ACK) { putBe32(t + 4, be32(th + 8)); putBe32(t + 8, 0); t[13] = RST; }
    else {
        u32 ack = be32(th + 4) + dataLen + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0);
        putBe32(t + 4, 0); putBe32(t + 8, ack); t[13] = RST | ACK;
    }
    t[12] = 5 << 4;
    putBe16(t + 14, 0); putBe16(t + 16, 0); putBe16(t + 18, 0);
    putBe16(t + 16, checksum(t, 20, pseudo(iph + 16, iph + 12, 6, 20)));
    sendIp(iph + 16, iph + 12, 6, t, 20);
}

void NatRouter::tcpIn(const u8 *iph, const u8 *th, u32 len)
{
    if (len < 20) return;
    u32 hl = (u32)(th[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    if (checksum(th, len, pseudo(iph + 12, iph + 16, 6, len)) != 0) return;
    u16 sport = be16(th), dport = be16(th + 2);
    u32 seq = be32(th + 4), ack = be32(th + 8);
    u8 flags = th[13];
    const u8 *data = th + hl;
    u32 dlen = len - hl;
    u32 now = net.milliseconds();

    Tcp *cp = tcpFind(iph + 12, sport, iph + 16, dport);
    if (!cp) {
        if ((flags & (SYN | ACK | RST)) != SYN) { tcpReset(iph, th, dlen); return; }
        for (Tcp &c : tcp) if (!c.used) { cp = &c; break; }
        if (!cp) { tcpReset(iph, th, dlen); return; }   // too many connections
        int h = net.tcpOpen(iph + 16, dport);
        if (h < 0) { tcpReset(iph, th, dlen); return; }
        Tcp &c = *cp;
        memFill(&c, 0, sizeof c);
        c.used = true;
        c.state = TcpConnecting;
        c.handle = h;
        memCopy(c.macIp, iph + 12, 4); memCopy(c.remoteIp, iph + 16, 4);
        c.macPort = sport; c.remotePort = dport;
        issClock += 0x10000 + now;
        c.iss = issClock;
        c.sndUna = c.sndNxt = c.iss;
        c.rcvNxt = seq + 1;
        c.macWindow = be16(th + 14);
        c.mss = 536;
        for (u32 i = 20; i + 1 < hl; ) {                // the Mac's MSS
            u8 kind = th[i];
            if (kind == 0) break;
            if (kind == 1) { i++; continue; }
            u8 l = th[i + 1];
            if (kind == 2 && l == 4 && i + 3 < hl) c.mss = be16(th + i + 2);
            if (l < 2) break;
            i += l;
        }
        if (c.mss > 1460) c.mss = 1460;
        if (c.mss < 64) c.mss = 64;
        c.tx = new u8[kTxBuffer];
        c.lastActivity = now;
        return;
    }
    Tcp &c = *cp;
    c.lastActivity = now;
    if (flags & RST) { tcpFree(c); return; }
    if (flags & SYN) {                                  // a repeated SYN
        if (c.state == TcpSynAckSent) tcpSegment(c, SYN | ACK, c.iss, 0, 0);
        return;
    }
    if (!(flags & ACK) || c.state == TcpConnecting) return;

    // What the Mac has received.
    if (c.state == TcpSynAckSent && ack == c.iss + 1) {
        c.state = TcpOpen;
        c.sndUna = c.sndNxt = c.iss + 1;
    }
    if (c.state != TcpOpen) return;
    if (seqLess(c.sndUna, ack) && seqLessEq(ack, c.sndNxt)) {
        u32 n = ack - c.sndUna;
        u32 dataAcked = n > c.txLen ? c.txLen : n;      // the rest is our FIN
        if (dataAcked) {
            for (u32 i = dataAcked; i < c.txLen; i++) c.tx[i - dataAcked] = c.tx[i];
            c.txLen -= dataAcked;
        }
        c.sndUna = ack;
        c.retries = 0;
    }
    c.macWindow = be16(th + 14);

    // What the Mac sends.
    bool ackNow = false;
    if (dlen) {
        if (seq == c.rcvNxt && !c.macClosed) {
            int n = net.tcpSend(c.handle, data, dlen);
            if (n < 0) { tcpSegment(c, RST | ACK, c.sndNxt, 0, 0); tcpFree(c); return; }
            c.rcvNxt += (u32)n;                         // what the server could not take comes again
        }
        ackNow = true;
    }
    if ((flags & FIN) && seq + dlen == c.rcvNxt && !c.macClosed) {
        c.macClosed = true;
        c.rcvNxt++;
        ackNow = true;
    }
    if (ackNow) tcpSegment(c, ACK, c.sndNxt, 0, 0);
    if (c.finSent && c.sndUna == c.sndNxt && c.macClosed) { tcpFree(c); return; }
    tcpPush(c);
}

// Send what the window allows of the server's data, then the FIN.
void NatRouter::tcpPush(Tcp &c)
{
    if (c.state != TcpOpen) return;
    u32 window = c.macWindow;
    if (window > 8 * c.mss) window = 8 * c.mss;         // be gentle with the Mac's buffers
    u32 offset = c.sndNxt - c.sndUna;
    if (c.finSent) return;
    while (offset < c.txLen && offset < window) {
        u32 n = c.txLen - offset;
        if (n > c.mss) n = c.mss;
        if (n > window - offset) n = window - offset;
        tcpSegment(c, ACK | PSH, c.sndNxt, c.tx + offset, n);
        c.sndNxt += n;
        offset += n;
    }
    bool drained = offset >= c.txLen;
    if (!c.finSent && drained && (c.serverClosed || (c.macClosed && c.txLen == 0))) {
        tcpSegment(c, FIN | ACK, c.sndNxt, 0, 0);
        c.sndNxt++;
        c.finSent = true;
        if (c.handle >= 0 && c.macClosed) { net.tcpClose(c.handle); c.handle = -1; }
    }
}

void NatRouter::poll()
{
    net.poll();
    u32 now = net.milliseconds();

    for (Udp &u : udp) {
        if (!u.used) continue;
        for (int i = 0; i < 8; i++) {
            u8 from[4];
            u16 port = 0;
            static u8 buf[1472];
            int n = net.udpReceive(u.handle, buf, sizeof buf, from, port);
            if (n <= 0) break;
            u.lastActivity = now;
            const u8 *src = u.dns ? kDns : from;
            if (u.dns) port = 53;
            u8 *pkt = scratch;
            putBe16(pkt, port); putBe16(pkt + 2, u.macPort);
            putBe16(pkt + 4, (u16)(8 + n)); putBe16(pkt + 6, 0);
            memCopy(pkt + 8, buf, (u32)n);
            u16 sum = checksum(pkt, 8 + (u32)n, pseudo(src, u.macIp, 17, 8 + (u32)n));
            putBe16(pkt + 6, sum ? sum : 0xFFFF);
            sendIp(src, u.macIp, 17, pkt, 8 + (u32)n);
        }
        if (now - u.lastActivity > kUdpIdleMs) { net.udpClose(u.handle); u.used = false; }
    }

    for (Tcp &c : tcp) {
        if (!c.used) continue;
        if (c.state == TcpConnecting) {
            int s = net.tcpState(c.handle);
            if (s == NatSockets::Connected) {
                c.state = TcpSynAckSent;
                tcpSegment(c, SYN | ACK, c.iss, 0, 0);
                c.sndNxt = c.iss + 1;
            } else if (s == NatSockets::Failed) {
                tcpSegment(c, RST | ACK, c.iss + 1, 0, 0);   // connection refused
                tcpFree(c);
            } else if (now - c.lastActivity > 30000) {
                tcpSegment(c, RST | ACK, c.iss + 1, 0, 0);
                tcpFree(c);
            }
            continue;
        }
        if (c.state == TcpSynAckSent) {
            if (now - c.lastSend > kRto) {
                if (++c.retries > kMaxRetries) { tcpFree(c); continue; }
                tcpSegment(c, SYN | ACK, c.iss, 0, 0);
            }
            continue;
        }
        // From the server, as far as the buffer goes.
        while (c.handle >= 0 && !c.serverClosed && c.txLen < kTxBuffer) {
            int n = net.tcpReceive(c.handle, c.tx + c.txLen, kTxBuffer - c.txLen);
            if (n < 0) { c.serverClosed = true; break; }
            if (n == 0) break;
            c.txLen += (u32)n;
            c.lastActivity = now;
        }
        // Nothing heard: resend from the first byte not acknowledged.
        if (c.sndNxt != c.sndUna && now - c.lastSend > kRto) {
            if (++c.retries > kMaxRetries) { tcpSegment(c, RST | ACK, c.sndUna, 0, 0); tcpFree(c); continue; }
            c.sndNxt = c.sndUna;
            c.finSent = false;
            if (c.macWindow == 0 && c.txLen) {          // a closed window: probe with one byte
                tcpSegment(c, ACK, c.sndNxt, c.tx, 1);
                c.sndNxt++;
                continue;
            }
        }
        tcpPush(c);
        if (c.finSent && c.sndUna == c.sndNxt && c.macClosed) { tcpFree(c); continue; }
        if (now - c.lastActivity > kTcpIdleMs) { tcpSegment(c, RST | ACK, c.sndNxt, 0, 0); tcpFree(c); }
    }
}

} // namespace macemu
