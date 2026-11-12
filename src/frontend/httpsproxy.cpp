#include "httpsproxy.h"
#include "util.h"
#include "bearssl.h"

namespace macemu {

// ---------------------------------------------------------------- roots

struct TrustAnchors {
    br_x509_trust_anchor *list = 0;
    u32 count = 0, cap = 0;
    ~TrustAnchors() {
        for (u32 i = 0; i < count; i++) {
            delete[] list[i].dn.data;
            if (list[i].pkey.key_type == BR_KEYTYPE_RSA) {
                delete[] list[i].pkey.key.rsa.n;
                delete[] list[i].pkey.key.rsa.e;
            } else {
                delete[] list[i].pkey.key.ec.q;
            }
        }
        delete[] list;
    }
};

namespace {

struct Bytes {                          // a growing byte buffer
    u8 *data = 0;
    u32 len = 0, cap = 0;
    ~Bytes() { delete[] data; }
    void add(const void *p, u32 n) {
        if (len + n > cap) {
            u32 c = cap ? cap * 2 : 256;
            while (c < len + n) c *= 2;
            u8 *d = new u8[c];
            if (len) memCopy(d, data, len);
            delete[] data;
            data = d; cap = c;
        }
        memCopy(data + len, p, n);
        len += n;
    }
};

void appendDn(void *ctx, const void *buf, size_t len) { ((Bytes *)ctx)->add(buf, (u32)len); }
void appendDer(void *ctx, const void *buf, size_t len) { ((Bytes *)ctx)->add(buf, (u32)len); }

u8 *copyOf(const u8 *p, size_t n)
{
    u8 *d = new u8[n ? n : 1];
    if (n) memCopy(d, p, (u32)n);
    return d;
}

// One certificate (DER) -> trust anchor.
bool addAnchor(TrustAnchors &t, const u8 *der, u32 len)
{
    Bytes dn;
    br_x509_decoder_context dc;
    br_x509_decoder_init(&dc, appendDn, &dn);
    br_x509_decoder_push(&dc, der, len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || dn.len == 0) return false;
    if (t.count == t.cap) {
        u32 c = t.cap ? t.cap * 2 : 64;
        br_x509_trust_anchor *l = new br_x509_trust_anchor[c];
        for (u32 i = 0; i < t.count; i++) l[i] = t.list[i];
        delete[] t.list;
        t.list = l; t.cap = c;
    }
    br_x509_trust_anchor &a = t.list[t.count];
    a.dn.data = copyOf(dn.data, dn.len);
    a.dn.len = dn.len;
    a.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    a.pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        a.pkey.key.rsa.n = copyOf(pk->key.rsa.n, pk->key.rsa.nlen);
        a.pkey.key.rsa.nlen = pk->key.rsa.nlen;
        a.pkey.key.rsa.e = copyOf(pk->key.rsa.e, pk->key.rsa.elen);
        a.pkey.key.rsa.elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        a.pkey.key.ec.curve = pk->key.ec.curve;
        a.pkey.key.ec.q = copyOf(pk->key.ec.q, pk->key.ec.qlen);
        a.pkey.key.ec.qlen = pk->key.ec.qlen;
    } else {
        delete[] a.dn.data;
        return false;
    }
    t.count++;
    return true;
}

// ---------------------------------------------------------------- text

bool startsNoCase(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) if (lower(*s) != lower(*prefix)) return false;
    return true;
}

bool containsNoCase(const char *s, const char *needle)
{
    for (; *s; s++) if (startsNoCase(s, needle)) return true;
    return false;
}

// Unicode -> Mac Roman, or 0 if there is none (then '?').
u8 macRoman(u32 c)
{
    static const u8 latin1[96] = {
        0xCA, 0xC1, 0xA2, 0xA3, 0xDB, 0xB4, '|',  0xA4, 0xAC, 0xA9, 0xBB, 0xC7, 0xC2, '-',  0xA8, 0xF8,
        0xA1, 0xB1, '2',  '3',  0xAB, 0xB5, 0xA6, 0xE1, 0xFC, '1',  0xBC, 0xC8, '?',  '?',  '?',  0xC0,
        0xCB, 0xE7, 0xE5, 0xCC, 0x80, 0x81, 0xAE, 0x82, 0xE9, 0x83, 0xE6, 0xE8, 0xED, 0xEA, 0xEB, 0xEC,
        'D',  0x84, 0xF1, 0xEE, 0xEF, 0xCD, 0x85, 'x',  0xAF, 0xF4, 0xF2, 0xF3, 0x86, 'Y',  'T',  0xA7,
        0x88, 0x87, 0x89, 0x8B, 0x8A, 0x8C, 0xBE, 0x8D, 0x8F, 0x8E, 0x90, 0x91, 0x93, 0x92, 0x94, 0x95,
        'd',  0x96, 0x98, 0x97, 0x99, 0x9B, 0x9A, 0xD6, 0xBF, 0x9D, 0x9C, 0x9E, 0x9F, 'y',  't',  0xD8,
    };
    if (c < 0x80) return (u8)c;
    if (c >= 0xA0 && c <= 0xFF) return latin1[c - 0xA0];
    switch (c) {
    case 0x2013: return 0xD0;  case 0x2014: return 0xD1;  case 0x2018: return 0xD4;
    case 0x2019: return 0xD5;  case 0x201C: return 0xD2;  case 0x201D: return 0xD3;
    case 0x201A: return 0xE2;  case 0x201E: return 0xE3;  case 0x2022: return 0xA5;
    case 0x2026: return 0xC9;  case 0x2122: return 0xAA;  case 0x20AC: return 0xDB;
    case 0x2020: return 0xA0;  case 0x2021: return 0xE0;  case 0x2030: return 0xE4;
    case 0x0152: return 0xCE;  case 0x0153: return 0xCF;  case 0x0178: return 0xD9;
    case 0x2044: return 0xDA;  case 0x2039: return 0xDC;  case 0x203A: return 0xDD;
    case 0xFB01: return 0xDE;  case 0xFB02: return 0xDF;  case 0x2264: return 0xB2;
    case 0x2265: return 0xB3;  case 0x2260: return 0xAD;  case 0x221E: return 0xB0;
    case 0x03C0: return 0xB9;  case 0x2211: return 0xB7;  case 0x220F: return 0xB8;
    case 0x2202: return 0xB6;  case 0x0394: case 0x2206: return 0xC6;
    case 0x03A9: case 0x2126: return 0xBD;  case 0x221A: return 0xC3;
    case 0x0192: return 0xC4;  case 0x2248: return 0xC5;  case 0x25CA: return 0xD7;
    case 0x02C6: return 0xF6;  case 0x02DC: return 0xF7;  case 0x2032: return '\'';
    case 0x2033: return '"';   case 0x2212: return '-';   case 0x2010: case 0x2011: return '-';
    case 0x2002: case 0x2003: case 0x2009: case 0x202F: return ' ';
    default: return 0;
    }
}

} // namespace

// ---------------------------------------------------------------- a connection

enum ConnState { StRequest, StResolve, StConnect, StTransfer, StDone };

struct ProxyConn {
    int state = StRequest;
    bool viaPort80 = false;             // the Mac's own port 80 connection (no proxy set)
    u8 origIp[4] = {0, 0, 0, 0};
    u32 lastActivity = 0;

    Bytes req;                          // from the Mac, until the headers are complete
    u32 bodyLeft = 0;                   // request body still to forward
    char method[16] = "";
    char host[256] = "";
    char path[1024] = "";
    u16 port = 80;
    bool explicitPort = false;
    bool tls = true;                    // trying HTTPS (first)
    Bytes toServer;                     // request bytes not yet sent upstream
    u32 toServerSent = 0;

    int dnsSocket = -1;
    u16 dnsId = 0;
    u32 dnsSentAt = 0;
    int dnsTries = 0;
    u8 ip[4] = {0, 0, 0, 0};

    int up = -1;                        // the host socket to the server
    u32 connectStarted = 0;
    struct Tls {
        br_ssl_client_context sc;
        br_x509_minimal_context xc;
        u8 iobuf[BR_SSL_BUFSIZE_BIDI];
    } *t = 0;
    bool gotResponse = false;
    bool timeProbe = false;             // asking the site's port 80 for the date first
    Bytes probe;                        // its answer

    Bytes hdr;                          // response headers being collected
    bool headersDone = false;
    bool chunked = false;
    int chunkState = 0;                 // 0 size line, 1 data, 2 CRLF after data, 3 trailers
    u32 chunkLeft = 0;
    char chunkLine[20] = "";
    u32 chunkLineLen = 0;
    bool rewrite = false;               // https:// -> http://
    bool macRomanize = false;           // UTF-8 -> Mac Roman
    u8 carry[8];
    u32 carryLen = 0;
    u32 uni = 0, uniNeed = 0;           // a UTF-8 sequence in progress
    u8 uniBytes[4];
    u32 uniHave = 0;

    u8 *out = 0;                        // to the Mac
    u32 outHead = 0, outLen = 0;
    enum { kOut = 32768 };
    bool done = false;                  // nothing more will be added to `out`

    ProxyConn() { out = new u8[kOut]; }
    ~ProxyConn() { delete[] out; delete t; }
    u32 outFree() const { return kOut - outLen; }
    void emit(const void *p, u32 n) {
        if (outHead && outLen + n > kOut - outHead) {   // compact
            for (u32 i = 0; i < outLen - outHead; i++) out[i] = out[outHead + i];
            outLen -= outHead; outHead = 0;
        }
        if (n > kOut - outLen) n = kOut - outLen;       // (callers keep within outFree)
        memCopy(out + outLen, p, n);
        outLen += n;
    }
    void emitStr(const char *s) { emit(s, strLen(s)); }
    u32 room() const { return kOut - (outLen - outHead); }
};

HttpsProxy::HttpsProxy(NatSockets &h) : host(h)
{
    for (int i = 0; i < kMaxConns; i++) conns[i] = 0;
    anchors = new TrustAnchors;
}

HttpsProxy::~HttpsProxy()
{
    for (int i = 0; i < kMaxConns; i++) if (conns[i]) tcpClose(kHandleBase + i);
    delete anchors;
}

u32 HttpsProxy::roots() const { return anchors->count; }

u32 HttpsProxy::loadRoots(const char *pem, u32 len)
{
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    Bytes der;
    bool inCert = false;
    u32 before = anchors->count;
    while (len > 0) {
        size_t n = br_pem_decoder_push(&pc, pem, len);
        pem += n; len -= (u32)n;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ:
            inCert = !strEq(br_pem_decoder_name(&pc), "CERTIFICATE") ? false : true;
            der.len = 0;
            br_pem_decoder_setdest(&pc, inCert ? appendDer : 0, &der);
            break;
        case BR_PEM_END_OBJ:
            if (inCert) addAnchor(*anchors, der.data, der.len);
            inCert = false;
            break;
        case BR_PEM_ERROR:
            return anchors->count - before;
        default:
            break;
        }
    }
    return anchors->count - before;
}

// ---------------------------------------------------------------- NatSockets

ProxyConn *HttpsProxy::conn(int h)
{
    int i = h - kHandleBase;
    return i >= 0 && i < kMaxConns ? conns[i] : 0;
}

int HttpsProxy::tcpOpen(const u8 *ip, u16 port)
{
    bool mine = (ip[0] == 10 && ip[1] == 0 && ip[2] == 2 && ip[3] == 2 && port == kProxyPort) ||
                (transparent && port == 80);
    if (!mine) return host.tcpOpen(ip, port);
    for (int i = 0; i < kMaxConns; i++) {
        if (conns[i]) continue;
        ProxyConn *c = new ProxyConn;
        c->viaPort80 = port == 80;
        memCopy(c->origIp, ip, 4);
        c->lastActivity = host.milliseconds();
        conns[i] = c;
        return kHandleBase + i;
    }
    return host.tcpOpen(ip, port);                      // all busy: straight through
}

int HttpsProxy::tcpState(int h)
{
    if (h < kHandleBase) return host.tcpState(h);
    return conn(h) ? Connected : Failed;
}

int HttpsProxy::tcpSend(int h, const u8 *data, u32 len)
{
    if (h < kHandleBase) return host.tcpSend(h, data, len);
    ProxyConn *c = conn(h);
    if (!c) return -1;
    c->lastActivity = host.milliseconds();
    if (c->state == StRequest) {
        if (c->req.len + len > 65536) return -1;        // headers far too long
        c->req.add(data, len);
        parseRequest(*c);
        return (int)len;
    }
    if (c->bodyLeft) {                                  // the rest of a POST body
        if (c->toServer.len - c->toServerSent > 32768) return 0;
        u32 n = len < c->bodyLeft ? len : c->bodyLeft;
        c->toServer.add(data, n);
        c->bodyLeft -= n;
        return (int)len;
    }
    return (int)len;                                    // anything after the request: ignored
}

int HttpsProxy::tcpReceive(int h, u8 *buf, u32 max)
{
    if (h < kHandleBase) return host.tcpReceive(h, buf, max);
    ProxyConn *c = conn(h);
    if (!c) return -1;
    u32 n = c->outLen - c->outHead;
    if (n == 0) return c->done ? -1 : 0;
    if (n > max) n = max;
    memCopy(buf, c->out + c->outHead, n);
    c->outHead += n;
    if (c->outHead == c->outLen) c->outHead = c->outLen = 0;
    return (int)n;
}

void HttpsProxy::tcpClose(int h)
{
    if (h < kHandleBase) { host.tcpClose(h); return; }
    ProxyConn *c = conn(h);
    if (!c) return;
    if (c->up >= 0) host.tcpClose(c->up);
    if (c->dnsSocket >= 0) host.udpClose(c->dnsSocket);
    delete c;
    conns[h - kHandleBase] = 0;
}

// ---------------------------------------------------------------- request

static const char *findHeaderEnd(const u8 *p, u32 n)
{
    for (u32 i = 0; i + 3 < n; i++)
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') return (const char *)p + i + 4;
    for (u32 i = 0; i + 1 < n; i++)                     // bare LF line ends (some old clients)
        if (p[i] == '\n' && p[i + 1] == '\n') return (const char *)p + i + 2;
    return 0;
}

// Splits "host[:port]" into c.host/c.port.
static void setHost(ProxyConn &c, const char *s, u32 n)
{
    u32 i = 0, k = 0;
    while (i < n && s[i] != ':' && s[i] != '/' && k + 1 < sizeof c.host) c.host[k++] = lower(s[i++]);
    c.host[k] = 0;
    if (i < n && s[i] == ':') {
        u32 p = 0;
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') p = p * 10 + (u32)(s[i++] - '0');
        if (p && p < 65536) { c.port = (u16)p; c.explicitPort = p != 80; }
    }
}

void HttpsProxy::parseRequest(ProxyConn &c)
{
    const char *end = findHeaderEnd(c.req.data, c.req.len);
    if (!end) return;
    const char *p = (const char *)c.req.data;
    u32 headLen = (u32)(end - p);

    // Request line.
    u32 i = 0, k = 0;
    while (i < headLen && p[i] != ' ' && k + 1 < sizeof c.method) c.method[k++] = p[i++];
    c.method[k] = 0;
    while (i < headLen && p[i] == ' ') i++;
    const char *target = p + i;
    u32 tlen = 0;
    while (i < headLen && p[i] != ' ' && p[i] != '\r' && p[i] != '\n') { i++; tlen++; }

    if (strEqNoCase(c.method, "CONNECT")) {
        errorPage(c, 501, "Secure connections from the Mac are not possible",
                  "The browser asked for an encrypted connection. Browsers of this age cannot do "
                  "today's encryption: type the address with <b>http://</b> instead, and the HTTPS "
                  "helper will make the secure connection for it.");
        return;
    }

    // The target: absolute (as a proxy sees it) or a path (port 80).
    const char *pathStart = target;
    u32 plen = tlen;
    if (tlen > 7 && startsNoCase(target, "http://")) {
        const char *h = target + 7;
        u32 hl = 0;
        while (7 + hl < tlen && h[hl] != '/') hl++;
        setHost(c, h, hl);
        pathStart = h + hl;
        plen = tlen - 7 - hl;
    }
    if (plen == 0) { pathStart = "/"; plen = 1; }
    if (plen >= sizeof c.path) plen = sizeof c.path - 1;
    memCopy(c.path, pathStart, plen);
    c.path[plen] = 0;

    // Headers: Host if the target had none; Content-Length for a body.
    const char *line = p;
    while (*line != '\n' && line < end) line++;
    line++;
    u32 contentLength = 0;
    Bytes keep;                                         // headers passed on
    while (line < end) {
        const char *eol = line;
        while (eol < end && *eol != '\n') eol++;
        u32 n = (u32)(eol - line);
        if (n && line[n - 1] == '\r') n--;
        if (n == 0) break;
        char name[40];
        u32 j = 0;
        while (j < n && line[j] != ':' && j + 1 < sizeof name) { name[j] = line[j]; j++; }
        name[j] = 0;
        const char *val = line + j + 1;
        u32 vl = n > j + 1 ? n - j - 1 : 0;
        while (vl && *val == ' ') { val++; vl--; }
        if (strEqNoCase(name, "Host")) {
            if (!c.host[0]) setHost(c, val, vl);
        } else if (strEqNoCase(name, "Content-Length")) {
            for (u32 m = 0; m < vl && val[m] >= '0' && val[m] <= '9'; m++) contentLength = contentLength * 10 + (u32)(val[m] - '0');
            keep.add(line, n); keep.add("\r\n", 2);
        } else if (strEqNoCase(name, "Connection") || strEqNoCase(name, "Proxy-Connection") ||
                   strEqNoCase(name, "Keep-Alive") || strEqNoCase(name, "Accept-Encoding") ||
                   strEqNoCase(name, "Upgrade-Insecure-Requests") || strEqNoCase(name, "TE") ||
                   strEqNoCase(name, "Upgrade") || strEqNoCase(name, "Proxy-Authorization")) {
            // dropped
        } else if ((strEqNoCase(name, "Referer") || strEqNoCase(name, "Origin")) && vl > 7 && startsNoCase(val, "http://")) {
            keep.add(name, strLen(name));               // fixed below, once we know whether it is HTTPS
            keep.add(": \x01", 3);
            keep.add(val + 7, vl - 7);
            keep.add("\r\n", 2);
        } else {
            keep.add(line, n); keep.add("\r\n", 2);
        }
        line = eol + 1;
    }

    if (!c.host[0] && c.viaPort80) {                    // no Host header: the address it dialled
        char a[16] = "";
        for (int b = 0; b < 4; b++) { appendUint(a, c.origIp[b], sizeof a); if (b < 3) strAppend(a, ".", sizeof a); }
        strCopy(c.host, a, sizeof c.host);
    }
    bool isUs = !c.host[0] || strEq(c.host, "10.0.2.2") || strEq(c.host, "macemu") || strEq(c.host, "https.helper");
    if (isUs && !c.viaPort80) { helpPage(c); return; }

    // HTTPS first, unless the address names a port of its own or is a
    // numeric one (no certificate names those).
    bool numeric = true;
    for (const char *h = c.host; *h; h++) if (!((*h >= '0' && *h <= '9') || *h == '.')) numeric = false;
    c.tls = !c.explicitPort && !numeric;
    if (c.tls) c.port = 443;

    // The request as the server will see it.
    Bytes &r = c.toServer;
    r.add(c.method, strLen(c.method)); r.add(" ", 1);
    r.add(c.path, strLen(c.path));
    r.add(" HTTP/1.1\r\nHost: ", 17);
    r.add(c.host, strLen(c.host));
    if (c.explicitPort) { char pn[8] = ":"; appendUint(pn, c.port, sizeof pn); r.add(pn, strLen(pn)); }
    r.add("\r\n", 2);
    for (u32 m = 0; m < keep.len; m++) {
        if (keep.data[m] == 0x01) { const char *s = c.tls ? "https://" : "http://"; r.add(s, strLen(s)); }
        else r.add(&keep.data[m], 1);
    }
    static const char tail[] = "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
    r.add(tail, sizeof tail - 1);
    u32 bodyHere = c.req.len - headLen;
    if (bodyHere > contentLength) bodyHere = contentLength;
    if (bodyHere) r.add(end, bodyHere);
    c.bodyLeft = contentLength - bodyHere;

    // Numeric addresses need no name lookup.
    u32 parts[4], np = 0, v = 0;
    bool ok = numeric;
    for (const char *h = c.host; ok; h++) {
        if (*h >= '0' && *h <= '9') { v = v * 10 + (u32)(*h - '0'); if (v > 255) ok = false; }
        else if (*h == '.' || !*h) { if (np < 4) parts[np++] = v; v = 0; if (!*h) break; }
    }
    if (ok && np == 4) {
        for (int b = 0; b < 4; b++) c.ip[b] = (u8)parts[b];
        c.state = StConnect;
        connect(c);
    } else {
        c.state = StResolve;
        resolve(c);
    }
}

// ---------------------------------------------------------------- DNS

void HttpsProxy::resolve(ProxyConn &c)
{
    u8 dns[4];
    if (!host.dnsServer(dns)) { errorPage(c, 502, "No name server", "The Pi has no DNS server to look names up with."); return; }
    if (c.dnsSocket < 0) c.dnsSocket = host.udpOpen();
    if (c.dnsSocket < 0) { errorPage(c, 502, "Busy", "No network socket was free. Try again in a moment."); return; }
    u8 q[300];
    c.dnsId = (u16)(host.milliseconds() * 2654435761u >> 16);
    putBe16(q, c.dnsId); putBe16(q + 2, 0x0100); putBe16(q + 4, 1);
    putBe16(q + 6, 0); putBe16(q + 8, 0); putBe16(q + 10, 0);
    u32 n = 12;
    const char *h = c.host;
    while (*h && n < 280) {
        const char *dot = h;
        while (*dot && *dot != '.') dot++;
        u32 l = (u32)(dot - h);
        if (l == 0 || l > 63) break;
        q[n++] = (u8)l;
        memCopy(q + n, h, l); n += l;
        h = *dot ? dot + 1 : dot;
    }
    q[n++] = 0;
    putBe16(q + n, 1); putBe16(q + n + 2, 1); n += 4;   // A, IN
    host.udpSend(c.dnsSocket, dns, 53, q, n);
    c.dnsSentAt = host.milliseconds();
    c.dnsTries++;
}

static bool skipName(const u8 *p, u32 len, u32 &i)
{
    while (i < len) {
        u8 l = p[i];
        if ((l & 0xC0) == 0xC0) { i += 2; return i <= len; }
        i++;
        if (l == 0) return true;
        i += l;
    }
    return false;
}

// ---------------------------------------------------------------- connecting

void HttpsProxy::connect(ProxyConn &c)
{
    c.up = host.tcpOpen(c.ip, c.port);
    if (c.up < 0) { errorPage(c, 502, "Busy", "No network socket was free. Try again in a moment."); return; }
    c.connectStarted = host.milliseconds();
}

bool HttpsProxy::now(u32 &t)
{
    if (host.wallClock(t) && t > 1700000000u) return true;
    if (!haveDate) return false;
    t = dateBase + (host.milliseconds() - dateAtMs) / 1000;
    return true;
}

// "Date: Thu, 12 Nov 2026 16:20:05 GMT" among the headers.
void HttpsProxy::noteDate(const char *h, u32 len)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (u32 i = 0; i + 30 < len; i++) {
        if (!(i == 0 || h[i - 1] == '\n') || !startsNoCase(h + i, "Date:")) continue;
        const char *p = h + i + 5;
        while (*p == ' ') p++;
        while (*p && *p != ',' && *p != '\r') p++;     // the day name
        if (*p != ',') return;
        p++;
        int day = 0, year = 0, hh = 0, mm = 0, ss = 0, mon = 0;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') day = day * 10 + (*p++ - '0');
        while (*p == ' ') p++;
        for (int m = 0; m < 12; m++) {
            const char *mn = months + 3 * m;
            if (lower(p[0]) == lower(mn[0]) && lower(p[1]) == lower(mn[1]) && lower(p[2]) == lower(mn[2])) { mon = m + 1; break; }
        }
        p += 3;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') year = year * 10 + (*p++ - '0');
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') hh = hh * 10 + (*p++ - '0');
        if (*p++ != ':') return;
        while (*p >= '0' && *p <= '9') mm = mm * 10 + (*p++ - '0');
        if (*p++ != ':') return;
        while (*p >= '0' && *p <= '9') ss = ss * 10 + (*p++ - '0');
        if (!mon || year < 2024 || day < 1 || day > 31) return;
        dateBase = macTime(year, mon, day, hh, mm, ss) - 2082844800u;   // Mac epoch -> Unix
        dateAtMs = host.milliseconds();
        haveDate = true;
        return;
    }
}

void HttpsProxy::startTls(ProxyConn &c)
{
    u32 now = 0;
    if (!this->now(now)) {
        // No clock yet (NTP blocked or not done): ask the site's plain
        // HTTP side for the date first, then come back here.
        if (!c.timeProbe) {
            c.timeProbe = true;
            host.tcpClose(c.up);
            c.up = -1;
            c.port = 80;
            c.state = StConnect;
            connect(c);
            return;
        }
        errorPage(c, 503, "The Pi does not know the time yet",
                  "Checking a site's certificate needs today's date, which the Pi gets from the "
                  "internet (from a time server, or a web site) once it is on the network. Try again "
                  "in a moment.");
        return;
    }
    c.t = new ProxyConn::Tls;
    br_ssl_client_init_full(&c.t->sc, &c.t->xc, anchors->list, anchors->count);
    br_x509_minimal_set_time(&c.t->xc, now / 86400 + 719528, now % 86400);
    br_ssl_engine_set_buffer(&c.t->sc.eng, c.t->iobuf, sizeof c.t->iobuf, 1);
    u8 seed[32];
    host.randomBytes(seed, sizeof seed);
    br_ssl_engine_inject_entropy(&c.t->sc.eng, seed, sizeof seed);
    br_ssl_client_reset(&c.t->sc, c.host, 0);
}

// ---------------------------------------------------------------- moving data

void HttpsProxy::poll()
{
    host.poll();
    u32 now = host.milliseconds();
    for (int i = 0; i < kMaxConns; i++) {
        ProxyConn *c = conns[i];
        if (!c) continue;
        if (c->state != StDone && now - c->lastActivity > 60000) {
            if (!c->gotResponse) errorPage(*c, 504, "No answer", "The site did not answer in time.");
            else finish(*c, 0);
        }
        for (int n = 0; n < 8 && c->state != StDone; n++) {
            int before = c->state;
            u32 outBefore = c->outLen;
            step(*c);
            if (c->state == before && c->outLen == outBefore) break;
        }
    }
}

void HttpsProxy::step(ProxyConn &c)
{
    u32 now = host.milliseconds();
    switch (c.state) {
    case StResolve: {
        u8 buf[512], from[4];
        u16 fport = 0;
        int n = host.udpReceive(c.dnsSocket, buf, sizeof buf, from, fport);
        if (n >= 12 && be16(buf) == c.dnsId) {
            u32 len = (u32)n, i = 12;
            u16 qd = be16(buf + 4), an = be16(buf + 6);
            bool found = false;
            if ((buf[3] & 0x0F) == 0) {
                for (u16 k = 0; k < qd && skipName(buf, len, i); k++) i += 4;
                for (u16 k = 0; k < an && !found && skipName(buf, len, i) && i + 10 <= len; k++) {
                    u16 type = be16(buf + i), rdlen = be16(buf + i + 8);
                    i += 10;
                    if (type == 1 && rdlen == 4 && i + 4 <= len) { memCopy(c.ip, buf + i, 4); found = true; }
                    i += rdlen;
                }
            }
            host.udpClose(c.dnsSocket);
            c.dnsSocket = -1;
            if (!found) {
                errorPage(c, 502, "No such site", "The name could not be found. Check the address.");
                return;
            }
            c.state = StConnect;
            c.lastActivity = now;
            connect(c);
        } else if (now - c.dnsSentAt > 2500) {
            if (c.dnsTries >= 3) errorPage(c, 504, "No answer from the name server", "The site's name could not be looked up.");
            else resolve(c);
        }
        return;
    }
    case StConnect: {
        int s = host.tcpState(c.up);
        if (s == Connected) {
            c.state = StTransfer;
            c.lastActivity = now;
            if (c.timeProbe && c.port == 80) {          // the date, over plain HTTP
                static const char head1[] = "HEAD / HTTP/1.1\r\nHost: ";
                static const char head2[] = "\r\nConnection: close\r\n\r\n";
                Bytes q;
                q.add(head1, sizeof head1 - 1); q.add(c.host, strLen(c.host)); q.add(head2, sizeof head2 - 1);
                host.tcpSend(c.up, q.data, q.len);
                return;
            }
            if (c.tls) startTls(c);
        } else if (s == Failed || now - c.connectStarted > 15000) {
            host.tcpClose(c.up);
            c.up = -1;
            if (c.timeProbe && c.port == 80) {
                errorPage(c, 503, "The Pi does not know the time yet",
                          "Checking a site's certificate needs today's date. The Pi could not get it "
                          "from a time server (NTP) or from the site. Try again in a moment.");
                return;
            }
            if (c.tls) {                                // no HTTPS there: plain HTTP
                c.tls = false;
                c.port = 80;
                // The request's Referer/Origin said https://: harmless.
                connect(c);
            } else {
                errorPage(c, 502, "Cannot connect", "The site could not be reached.");
            }
        }
        return;
    }
    case StTransfer:
        pump(c);
        return;
    default:
        return;
    }
}

void HttpsProxy::pump(ProxyConn &c)
{
    u32 now = host.milliseconds();
    if (c.timeProbe && c.port == 80) {                  // reading the date
        u8 buf[1024];
        int n = host.tcpReceive(c.up, buf, sizeof buf);
        if (n > 0) c.probe.add(buf, (u32)n);
        if (n < 0 || findHeaderEnd(c.probe.data, c.probe.len) || c.probe.len > 8192) {
            if (c.probe.len) noteDate((const char *)c.probe.data, c.probe.len);
            host.tcpClose(c.up);
            c.up = -1;
            c.port = 443;                               // and now the real thing
            c.state = StConnect;
            connect(c);
        }
        return;
    }
    if (!c.tls) {
        // Plain HTTP: the request out, the answer in.
        while (c.toServerSent < c.toServer.len) {
            int n = host.tcpSend(c.up, c.toServer.data + c.toServerSent, c.toServer.len - c.toServerSent);
            if (n <= 0) break;
            c.toServerSent += (u32)n;
            c.lastActivity = now;
        }
        while (c.room() > 4096 && c.state == StTransfer) {
            u8 buf[2048];
            int n = host.tcpReceive(c.up, buf, sizeof buf);
            if (n < 0) { finish(c, 0); return; }
            if (n == 0) break;
            c.lastActivity = now;
            response(c, buf, (u32)n);
        }
        return;
    }

    if (!c.t) return;
    br_ssl_engine_context *eng = &c.t->sc.eng;
    for (int guard = 0; guard < 16 && c.state == StTransfer; guard++) {
        unsigned st = br_ssl_engine_current_state(eng);
        bool progress = false;
        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(eng);
            finish(c, err == BR_ERR_OK ? 0 : err);
            return;
        }
        if (st & BR_SSL_SENDREC) {
            size_t len;
            u8 *buf = br_ssl_engine_sendrec_buf(eng, &len);
            int n = host.tcpSend(c.up, buf, (u32)len);
            if (n < 0) { finish(c, -1); return; }
            if (n > 0) { br_ssl_engine_sendrec_ack(eng, (size_t)n); progress = true; c.lastActivity = now; }
        }
        if ((st & BR_SSL_SENDAPP) && c.toServerSent < c.toServer.len) {
            size_t len;
            u8 *buf = br_ssl_engine_sendapp_buf(eng, &len);
            u32 n = c.toServer.len - c.toServerSent;
            if (n > len) n = (u32)len;
            memCopy(buf, c.toServer.data + c.toServerSent, n);
            br_ssl_engine_sendapp_ack(eng, n);
            c.toServerSent += n;
            if (c.toServerSent == c.toServer.len && !c.bodyLeft) br_ssl_engine_flush(eng, 0);
            progress = true;
        }
        if ((st & BR_SSL_RECVAPP) && c.room() > 4096) {
            size_t len;
            u8 *buf = br_ssl_engine_recvapp_buf(eng, &len);
            u32 n = (u32)len;
            if (n > c.room() - 2048) n = c.room() - 2048;
            response(c, buf, n);
            br_ssl_engine_recvapp_ack(eng, n);
            progress = true;
            c.lastActivity = now;
            if (c.state != StTransfer) return;
        }
        if (st & BR_SSL_RECVREC) {
            size_t len;
            u8 *buf = br_ssl_engine_recvrec_buf(eng, &len);
            int n = host.tcpReceive(c.up, buf, (u32)len);
            if (n < 0) {                                // closed without close_notify: common
                if (c.gotResponse) finish(c, 0);
                else finish(c, br_ssl_engine_last_error(eng) ? br_ssl_engine_last_error(eng) : -1);
                return;
            }
            if (n > 0) { br_ssl_engine_recvrec_ack(eng, (size_t)n); progress = true; c.lastActivity = now; }
        }
        if (!progress) break;
    }
}

// The upstream connection has ended (err 0: normally).
void HttpsProxy::finish(ProxyConn &c, int err)
{
    if (c.up >= 0) { host.tcpClose(c.up); c.up = -1; }
    if (c.state == StDone) return;
    if (err != 0 && !c.gotResponse) {
        if (err >= BR_ERR_X509_OK && err < 64) {
            certificateErrors++;
            const char *why =
                err == BR_ERR_X509_NOT_TRUSTED ? "it was not issued by a trusted authority" :
                err == BR_ERR_X509_BAD_SERVER_NAME ? "it belongs to a different site" :
                err == BR_ERR_X509_EXPIRED ? "it has expired (or the Pi's clock is wrong)" :
                err == BR_ERR_X509_TIME_UNKNOWN ? "the Pi does not know the date yet" :
                "it could not be checked";
            char text[400] = "The site's security certificate was refused: ";
            strAppend(text, why, sizeof text);
            strAppend(text, ". The page was not loaded, because someone could be pretending to be "
                            "this site. (Root certificates are in certs/cacert.pem on the SD card.)", sizeof text);
            errorPage(c, 502, "Certificate problem", text);
            return;
        }
        if (c.tls && !c.explicitPort) {                 // HTTPS failed outright: try plain HTTP
            delete c.t; c.t = 0;
            c.tls = false;
            c.port = 80;
            c.toServerSent = 0;
            c.state = StConnect;
            connect(c);
            return;
        }
        char text[120] = "The connection to the site failed (error ";
        appendUint(text, (u32)(err < 0 ? 0 : err), sizeof text);
        strAppend(text, ").", sizeof text);
        errorPage(c, 502, "Connection failed", text);
        return;
    }
    if (!c.headersDone && c.hdr.len) responseHeaders(c);     // headers without a body
    if (c.carryLen) { emitBody(c, c.carry, c.carryLen); c.carryLen = 0; }
    if (c.state == StTransfer) pagesFetched++;
    c.state = StDone;
    c.done = true;
}

// ---------------------------------------------------------------- the answer

void HttpsProxy::response(ProxyConn &c, const u8 *data, u32 len)
{
    c.gotResponse = true;
    if (!c.headersDone) {
        u32 before = c.hdr.len;
        c.hdr.add(data, len);
        const char *end = findHeaderEnd(c.hdr.data, c.hdr.len);
        if (!end) {
            if (c.hdr.len > 32768) { finish(c, 0); }    // not HTTP
            return;
        }
        u32 headLen = (u32)(end - (const char *)c.hdr.data);
        responseHeaders(c);
        u32 rest = c.hdr.len - headLen;
        (void)before;
        if (rest) body(c, c.hdr.data + headLen, rest);
        return;
    }
    body(c, data, len);
}

void HttpsProxy::responseHeaders(ProxyConn &c)
{
    const char *p = (const char *)c.hdr.data;
    const char *end = findHeaderEnd(c.hdr.data, c.hdr.len);
    if (!end) end = p + c.hdr.len;
    if (!c.tls && !haveDate) noteDate(p, (u32)(end - p));
    c.headersDone = true;

    // Status line: as HTTP/1.0.
    const char *sp = p;
    while (sp < end && *sp != ' ') sp++;
    const char *eol = sp;
    while (eol < end && *eol != '\n') eol++;
    u32 sl = (u32)(eol - sp);
    if (sl && sp[sl - 1] == '\r') sl--;
    c.emitStr("HTTP/1.0");
    c.emit(sp, sl);
    c.emitStr("\r\n");

    // First pass: what the body is.
    bool text = false, utf8 = false, charsetGiven = false;
    for (const char *line = eol + 1; line < end; ) {
        const char *e = line;
        while (e < end && *e != '\n') e++;
        if (startsNoCase(line, "Content-Type:")) {
            char v[128];
            u32 n = (u32)(e - line) - 13;
            if (n >= sizeof v) n = sizeof v - 1;
            memCopy(v, line + 13, n); v[n] = 0;
            text = containsNoCase(v, "text/") || containsNoCase(v, "javascript") ||
                   containsNoCase(v, "json") || containsNoCase(v, "xml");
            if (containsNoCase(v, "charset")) { charsetGiven = true; utf8 = containsNoCase(v, "utf-8") || containsNoCase(v, "utf8"); }
            if (!charsetGiven && (containsNoCase(v, "text/html") || containsNoCase(v, "text/plain"))) utf8 = true;
        }
        if (startsNoCase(line, "Transfer-Encoding:") && containsNoCase(line, "chunked")) c.chunked = true;
        line = e + 1;
    }
    c.rewrite = text;
    c.macRomanize = text && utf8;

    // Second pass: the headers the Mac gets.
    for (const char *line = eol + 1; line < end; ) {
        const char *e = line;
        while (e < end && *e != '\n') e++;
        u32 n = (u32)(e - line);
        if (n && line[n - 1] == '\r') n--;
        if (n == 0) break;
        bool drop = startsNoCase(line, "Transfer-Encoding:") || startsNoCase(line, "Connection:") ||
                    startsNoCase(line, "Keep-Alive:") || startsNoCase(line, "Strict-Transport-Security:") ||
                    startsNoCase(line, "Content-Security-Policy") || startsNoCase(line, "Alt-Svc:") ||
                    startsNoCase(line, "Upgrade:") || startsNoCase(line, "Public-Key-Pins") ||
                    startsNoCase(line, "Expect-CT:") || startsNoCase(line, "NEL:") || startsNoCase(line, "Report-To:") ||
                    (c.rewrite && startsNoCase(line, "Content-Length:"));
        if (!drop) {
            char buf[2048];
            if (n >= sizeof buf) n = sizeof buf - 1;
            u32 o = 0;
            bool isType = startsNoCase(line, "Content-Type:");
            bool isCookie = startsNoCase(line, "Set-Cookie:");
            for (u32 i = 0; i < n && o + 1 < sizeof buf; ) {
                if (startsNoCase(line + i, "https://")) { memCopy(buf + o, "http://", 7); o += 7; i += 8; continue; }
                if (isCookie && line[i] == ';') {           // the Secure flag: the Mac's side is plain HTTP
                    u32 j = i + 1;
                    while (j < n && line[j] == ' ') j++;
                    if (startsNoCase(line + j, "secure") && (j + 6 == n || line[j + 6] == ';' || line[j + 6] == ' ')) { i = j + 6; continue; }
                }
                if (isType && c.macRomanize && line[i] == ';' && containsNoCase(line + i, "charset")) break;   // no charset: Mac Roman
                buf[o++] = line[i++];
            }
            c.emit(buf, o);
            c.emitStr("\r\n");
        }
        line = e + 1;
    }
    c.emitStr("Connection: close\r\n\r\n");
}

// Chunked transfer coding off, then the text changes.
void HttpsProxy::body(ProxyConn &c, const u8 *data, u32 len)
{
    if (!c.chunked) { emitBody(c, data, len); return; }
    u32 i = 0;
    while (i < len) {
        switch (c.chunkState) {
        case 0: {                                       // size line
            u8 ch = data[i++];
            if (ch == '\n') {
                c.chunkLine[c.chunkLineLen] = 0;
                u32 v = 0;
                for (u32 k = 0; k < c.chunkLineLen; k++) {
                    char h = c.chunkLine[k];
                    int d = h >= '0' && h <= '9' ? h - '0' : (h | 32) >= 'a' && (h | 32) <= 'f' ? (h | 32) - 'a' + 10 : -1;
                    if (d < 0) break;
                    v = v * 16 + (u32)d;
                }
                c.chunkLineLen = 0;
                c.chunkLeft = v;
                c.chunkState = v ? 1 : 3;
            } else if (c.chunkLineLen + 1 < sizeof c.chunkLine) {
                c.chunkLine[c.chunkLineLen++] = (char)ch;
            }
            break;
        }
        case 1: {                                       // data
            u32 n = len - i < c.chunkLeft ? len - i : c.chunkLeft;
            emitBody(c, data + i, n);
            i += n;
            c.chunkLeft -= n;
            if (!c.chunkLeft) c.chunkState = 2;
            break;
        }
        case 2:                                         // the CRLF after the data
            if (data[i++] == '\n') c.chunkState = 0;
            break;
        default:                                        // trailers: ignored
            i = len;
            break;
        }
    }
}

void HttpsProxy::emitBody(ProxyConn &c, const u8 *data, u32 len)
{
    if (!c.rewrite) { c.emit(data, len); return; }
    // https:// -> http://, across buffer boundaries (up to 7 bytes carried).
    static const char pat[] = "https://";
    u8 tmp[2048 + 16];
    u32 o = 0;
    auto put = [&](u8 b) {
        if (!c.macRomanize) { tmp[o++] = b; return; }
        // UTF-8 -> Mac Roman; bytes that are not UTF-8 count as Latin-1.
        if (c.uniNeed) {
            if ((b & 0xC0) == 0x80) {
                c.uni = (c.uni << 6) | (b & 0x3F);
                c.uniBytes[c.uniHave++] = b;
                if (--c.uniNeed == 0) {
                    if (c.uni == 0xFEFF || c.uni == 0x200B || c.uni == 0x200C || c.uni == 0x200D) return;
                    u8 m = macRoman(c.uni);
                    tmp[o++] = m ? m : '?';
                }
                return;
            }
            for (u32 k = 0; k < c.uniHave; k++) { u8 m = macRoman(c.uniBytes[k]); tmp[o++] = m ? m : '?'; }
            c.uniNeed = 0;
        }
        if (b < 0x80) { tmp[o++] = b; return; }
        if ((b & 0xE0) == 0xC0) { c.uni = b & 0x1F; c.uniNeed = 1; }
        else if ((b & 0xF0) == 0xE0) { c.uni = b & 0x0F; c.uniNeed = 2; }
        else if ((b & 0xF8) == 0xF0) { c.uni = b & 0x07; c.uniNeed = 3; }
        else { u8 m = macRoman(b); tmp[o++] = m ? m : '?'; return; }
        c.uniBytes[0] = b;
        c.uniHave = 1;
    };
    u32 i = 0;
    while (i < len) {
        // Bring the carried bytes and new ones together to look for the pattern.
        u8 b = data[i++];
        c.carry[c.carryLen++] = b;
        u32 m = 0;
        while (m < c.carryLen && lower((char)c.carry[m]) == pat[m]) m++;
        if (m == c.carryLen && m < 8) continue;         // could still become "https://"
        if (m == 8) {                                   // found: write http://
            for (const char *s = "http://"; *s; s++) put((u8)*s);
            c.carryLen = 0;
        } else {
            put(c.carry[0]);                            // the first byte cannot start a match: out
            for (u32 k = 1; k < c.carryLen; k++) c.carry[k - 1] = c.carry[k];
            c.carryLen--;
            // The rest may start a match: test again with the next byte
            // (a short loop, at most 7 bytes).
            while (c.carryLen) {
                u32 mm = 0;
                while (mm < c.carryLen && lower((char)c.carry[mm]) == pat[mm]) mm++;
                if (mm == c.carryLen) break;
                put(c.carry[0]);
                for (u32 k = 1; k < c.carryLen; k++) c.carry[k - 1] = c.carry[k];
                c.carryLen--;
            }
        }
        if (o > 2048) { c.emit(tmp, o); o = 0; }
    }
    if (o) c.emit(tmp, o);
}

// ---------------------------------------------------------------- pages

void HttpsProxy::errorPage(ProxyConn &c, int status, const char *title, const char *text)
{
    if (c.up >= 0) { host.tcpClose(c.up); c.up = -1; }
    c.emitStr("HTTP/1.0 ");
    char num[8] = "";
    appendUint(num, (u32)status, sizeof num);
    c.emitStr(num);
    c.emitStr(" Error\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
    c.emitStr("<html><head><title>");
    c.emitStr(title);
    c.emitStr("</title></head><body><h2>");
    c.emitStr(title);
    c.emitStr("</h2><p>");
    c.emitStr(text);
    if (c.host[0]) { c.emitStr("</p><p>Site: <b>"); c.emitStr(c.host); c.emitStr("</b>"); }
    c.emitStr("</p><hr><p><i>The emulator's HTTPS helper</i></p></body></html>\r\n");
    c.state = StDone;
    c.done = true;
}

void HttpsProxy::helpPage(ProxyConn &c)
{
    c.emitStr("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
              "<html><head><title>HTTPS helper</title></head><body>"
              "<h2>The emulator's HTTPS helper</h2>"
              "<p>Type addresses as <b>http://</b>... and the helper fetches the page securely "
              "(over HTTPS) for this Mac, checking the site's certificate.</p>"
              "<p>It works without any set-up for browsers that send the site's name (Netscape 2 "
              "and later, Internet Explorer, iCab). For others, set the browser's HTTP proxy to "
              "<b>10.0.2.2</b>, port <b>8080</b>.</p>"
              "<p>Some sites to try: <a href=\"http://frogfind.com/\">FrogFind</a>, "
              "<a href=\"http://68k.news/\">68k.news</a>, "
              "<a href=\"http://en.wikipedia.org/\">Wikipedia</a>.</p>"
              "<p>Pages arrive as they are: modern sites may still be too much for an old browser "
              "(and pictures in new formats will not show).</p>"
              "</body></html>\r\n");
    c.state = StDone;
    c.done = true;
}

} // namespace macemu
