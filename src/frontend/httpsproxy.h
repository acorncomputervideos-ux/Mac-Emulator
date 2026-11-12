// The HTTPS helper: lets the Mac's old web browsers reach today's sites,
// which mostly only speak HTTPS with modern encryption.
//
// The browser talks plain HTTP to the helper (as its proxy, 10.0.2.2:8080,
// or without any set-up: the router hands it the Mac's connections to
// port 80).  The helper fetches the page over HTTPS from the host, checking
// the site's certificate against the root certificates on the SD card,
// and passes it on as plain HTTP:
//   - https:// links become http:// (the helper upgrades them again);
//   - pages in UTF-8 are converted to Mac Roman;
//   - chunked replies are put back together, and headers that only make
//     sense over HTTPS (HSTS, Secure cookies) are taken out.
// Sites without HTTPS are fetched over plain HTTP.  A certificate that
// cannot be verified gives an error page, never a silent downgrade.
//
// It sits between the NAT router and the host's sockets (a NatSockets of
// its own), so the router needs to know nothing about it.  TLS is BearSSL.

#ifndef MACEMU_HTTPSPROXY_H
#define MACEMU_HTTPSPROXY_H

#include "natrouter.h"

namespace macemu {

struct ProxyConn;
struct TrustAnchors;

class HttpsProxy : public NatSockets {
public:
    explicit HttpsProxy(NatSockets &host);
    ~HttpsProxy() override;

    // Root certificates (PEM text, such as Mozilla's cacert.pem).  Returns
    // how many were loaded.
    u32 loadRoots(const char *pem, u32 len);
    u32 roots() const;

    bool transparent = true;            // also serve the Mac's port 80 connections
    enum { kProxyPort = 8080, kMaxConns = 8 };

    // Statistics for the menu and the log.
    u32 pagesFetched = 0, certificateErrors = 0;

    // NatSockets: everything passes through to the host but the helper's
    // own connections.
    int udpOpen() override { return host.udpOpen(); }
    bool udpSend(int h, const u8 *ip, u16 port, const u8 *data, u32 len) override { return host.udpSend(h, ip, port, data, len); }
    int udpReceive(int h, u8 *buf, u32 max, u8 *ip, u16 &port) override { return host.udpReceive(h, buf, max, ip, port); }
    void udpClose(int h) override { host.udpClose(h); }
    int tcpOpen(const u8 *ip, u16 port) override;
    int tcpState(int h) override;
    int tcpSend(int h, const u8 *data, u32 len) override;
    int tcpReceive(int h, u8 *buf, u32 max) override;
    void tcpClose(int h) override;
    bool dnsServer(u8 *ip) override { return host.dnsServer(ip); }
    u32 milliseconds() override { return host.milliseconds(); }
    bool wallClock(u32 &unixTime) override { return host.wallClock(unixTime); }
    void randomBytes(u8 *buf, u32 len) override { host.randomBytes(buf, len); }
    void poll() override;

private:
    enum { kHandleBase = 0x100000 };
    NatSockets &host;
    ProxyConn *conns[kMaxConns];
    TrustAnchors *anchors;
    // Without a clock (no NTP), the time from a web server's Date header.
    bool haveDate = false;
    u32 dateBase = 0, dateAtMs = 0;
    bool now(u32 &unixTime);
    void noteDate(const char *headers, u32 len);

    ProxyConn *conn(int h);
    void step(ProxyConn &c);
    void parseRequest(ProxyConn &c);
    void resolve(ProxyConn &c);
    void connect(ProxyConn &c);
    void startTls(ProxyConn &c);
    void pump(ProxyConn &c);
    void response(ProxyConn &c, const u8 *data, u32 len);
    void responseHeaders(ProxyConn &c);
    void body(ProxyConn &c, const u8 *data, u32 len);
    void emitBody(ProxyConn &c, const u8 *data, u32 len);
    void errorPage(ProxyConn &c, int status, const char *title, const char *text);
    void helpPage(ProxyConn &c);
    void finish(ProxyConn &c, int err);
};

} // namespace macemu

#endif
