// Harissa64 - HTTPS downloads from the console (see xb_net.h).
#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "xb_net.h"

extern "C" VOID XeCryptRandom(PBYTE pb, DWORD cb);   // xboxkrnl export

#define SO_MARKINSECURE 0x5801   // RGH homebrew: allow this socket outside Xbox Live
#define NET_TIMEOUT_MS 10000
#define NET_HEAD_MAX 16384
#define NET_URL_MAX 4096        // signed storage URLs of GitHub release assets run near 1 KB

// ---- Network stack ----
// 0: not started, 1: starting (another thread waits), 2: online, 3: offline.
static volatile LONG s_netState;
static volatile LONG s_stackUp;

static int wait_network(void)
{
    XNADDR xna;
    DWORD r = XNET_GET_XNADDR_PENDING;
    int i;
    for (i = 0; i < 150; i++)   // DHCP may still be running: up to 15 s
    {
        if (!(XNetGetEthernetLinkStatus() & XNET_ETHERNET_LINK_ACTIVE)) { Sleep(100); continue; }
        r = XNetGetTitleXnAddr(&xna);
        if (r != XNET_GET_XNADDR_PENDING) break;
        Sleep(100);
    }
    return r != XNET_GET_XNADDR_PENDING && r != XNET_GET_XNADDR_NONE;
}

int NetStart(int retry)
{
    for (;;)
    {
        LONG st = InterlockedCompareExchange(&s_netState, 1, 0);
        if (st == 0) break;                              // this thread starts it
        if (st == 1) { Sleep(50); continue; }            // another thread is starting it
        if (st == 3 && retry && InterlockedCompareExchange(&s_netState, 1, 3) == 3) break;
        return st == 2;
    }
    if (!s_stackUp)
    {
        XNetStartupParams xnsp;
        WSADATA wsa;
        memset(&xnsp, 0, sizeof(xnsp));
        xnsp.cfgSizeOfStruct = sizeof(xnsp);
        xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
        if (XNetStartup(&xnsp) != 0 || WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            H64_INFO("[net] no network stack");
            InterlockedExchange(&s_netState, 3);
            return 0;
        }
        InterlockedExchange(&s_stackUp, 1);
    }
    if (!wait_network())
    {
        H64_INFO("[net] no network");
        InterlockedExchange(&s_netState, 3);
        return 0;
    }
    InterlockedExchange(&s_netState, 2);
    return 1;
}

// ---- A certificate check that accepts a chain with no trusted root
// (BearSSL's tools/certs.c x509_noanchor, MIT).
struct NoAnchor
{
    const br_x509_class *vtable;
    const br_x509_class **inner;
};

static void na_start_chain(const br_x509_class **ctx, const char *name)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    (*in)->start_chain(in, name);
}
static void na_start_cert(const br_x509_class **ctx, uint32_t length)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    (*in)->start_cert(in, length);
}
static void na_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    (*in)->append(in, buf, len);
}
static void na_end_cert(const br_x509_class **ctx)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    (*in)->end_cert(in);
}
static unsigned na_end_chain(const br_x509_class **ctx)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    unsigned r = (*in)->end_chain(in);
    return r == BR_ERR_X509_NOT_TRUSTED ? 0 : r;
}
static const br_x509_pkey *na_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    return (*in)->get_pkey(in, usages);
}
static const br_x509_class s_noAnchorVtable = {
    sizeof(NoAnchor), na_start_chain, na_start_cert, na_append, na_end_cert, na_end_chain, na_get_pkey
};

// ---- socket callbacks for br_sslio
static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int n = recv(*(SOCKET *)ctx, (char *)buf, (int)len, 0);
    return n > 0 ? n : -1;
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int n = send(*(SOCKET *)ctx, (const char *)buf, (int)len, 0);
    return n > 0 ? n : -1;
}

// The console's clock as BearSSL wants it (days since 1 January of year 0,
// seconds in the day): certificates are checked for their dates.
static void ssl_time(uint32_t *days, uint32_t *secs)
{
    FILETIME ft;
    ULARGE_INTEGER t;
    unsigned long long unixSecs;
    GetSystemTimeAsFileTime(&ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    unixSecs = (t.QuadPart - 116444736000000000ULL) / 10000000ULL;
    *days = (uint32_t)(unixSecs / 86400) + 719528;
    *secs = (uint32_t)(unixSecs % 86400);
}

static int resolve(const char *host, IN_ADDR *addr)
{
    XNDNS *dns = NULL;
    WSAEVENT ev = WSACreateEvent();
    int ok = 0;
    if (ev == WSA_INVALID_EVENT) return 0;
    if (XNetDnsLookup(host, ev, &dns) == 0 && dns)
    {
        WaitForSingleObject(ev, NET_TIMEOUT_MS);
        if (dns->iStatus == 0 && dns->cina > 0) { *addr = dns->aina[0]; ok = 1; }
        XNetDnsRelease(dns);
    }
    WSACloseEvent(ev);
    return ok;
}

// One connection's TLS state (~50 KB: allocated per request).
struct NetTls
{
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    NoAnchor na;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

// The value of header `name` ("content-length") in `head`, or NULL; its length in *n.
static const char *header(const char *head, const char *name, int *n)
{
    size_t k = strlen(name);
    const char *p = head;
    while ((p = strstr(p, "\r\n")) != NULL)
    {
        p += 2;
        if (!_strnicmp(p, name, k) && p[k] == ':')
        {
            const char *v = p + k + 1, *e;
            while (*v == ' ' || *v == '\t') v++;
            for (e = v; *e && *e != '\r' && *e != '\n'; e++) ;
            *n = (int)(e - v);
            return v;
        }
    }
    return NULL;
}

// Decodes a chunked body in place; returns its length, or -1 when malformed or cut.
static int dechunk(u8 *buf, u32 len)
{
    u32 in = 0, out = 0;
    for (;;)
    {
        u32 size = 0;
        int digits = 0;
        while (in < len)
        {
            u8 c = buf[in];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) break;
            if (size > 0x0FFFFFFF) return -1;
            size = size * 16 + (u32)v;
            digits++;
            in++;
        }
        if (!digits) return -1;
        while (in < len && buf[in] != '\n') in++;   // chunk extensions, CR
        if (in >= len) return -1;
        in++;
        if (!size) return (int)out;
        if (size > len - in) return -1;
        memmove(buf + out, buf + in, size);
        out += size;
        in += size;
        if (in < len && buf[in] == '\r') in++;
        if (in < len && buf[in] == '\n') in++;
    }
}

// One GET (no redirects). Returns the status or 0; see NetGet.
static int request(const char *host, const char *path, u8 **body, u32 *bodyLen, u32 maxLen, char *location, int locSize)
{
    NetTls *t;
    IN_ADDR ip;
    SOCKADDR_IN sa;
    SOCKET s;
    BOOL one = TRUE;
    DWORD timeout = NET_TIMEOUT_MS;
    unsigned char seed[32];
    uint32_t days, secs;
    char *req = NULL, *head = NULL;
    int n = 0, r, status = 0, hlen = 0, ok = 0;
    u8 *buf = NULL;
    u32 cap = 0, got = 0;

    if (location && locSize) location[0] = 0;
    if (strlen(path) + strlen(host) > NET_URL_MAX) return 0;
    if (!resolve(host, &ip)) { H64_INFO("[net] cannot resolve %s", host); return 0; }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    setsockopt(s, SOL_SOCKET, SO_MARKINSECURE, (const char *)&one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    sa.sin_addr = ip;
    if (connect(s, (const sockaddr *)&sa, sizeof(sa)) != 0)
    {
        H64_INFO("[net] cannot connect to %s (%d)", host, WSAGetLastError());
        closesocket(s);
        return 0;
    }
    t = (NetTls *)malloc(sizeof(NetTls));
    head = (char *)malloc(NET_HEAD_MAX + 1);
    req = (char *)malloc(NET_URL_MAX + 512);
    if (!t || !head || !req) { free(t); free(head); free(req); closesocket(s); return 0; }

    br_ssl_client_init_full(&t->sc, &t->xc, NULL, 0);
    ssl_time(&days, &secs);
    br_x509_minimal_set_time(&t->xc, days, secs);
    t->na.vtable = &s_noAnchorVtable;
    t->na.inner = &t->xc.vtable;
    br_ssl_engine_set_x509(&t->sc.eng, &t->na.vtable);
    XeCryptRandom(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof(seed));
    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof(t->iobuf), 1);
    br_ssl_client_reset(&t->sc, host, 0);
    br_sslio_init(&t->ioc, &t->sc.eng, sock_read, &s, sock_write, &s);

    _snprintf(req, NET_URL_MAX + 511,
              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Harissa64/" H64_VERSION_STRING
              " (Xbox 360; https://github.com/kernel64/Harissa64)\r\nAccept: */*\r\nConnection: close\r\n\r\n",
              path, host);
    req[NET_URL_MAX + 511] = 0;
    if (br_sslio_write_all(&t->ioc, req, strlen(req)) != 0 || br_sslio_flush(&t->ioc) != 0) goto done;

    // Headers (whatever body arrives with them is kept).
    while (n < NET_HEAD_MAX)
    {
        char *e;
        r = br_sslio_read(&t->ioc, head + n, NET_HEAD_MAX - n);
        if (r <= 0) break;
        n += r;
        head[n] = 0;
        if ((e = strstr(head, "\r\n\r\n")) != NULL) { hlen = (int)(e - head) + 4; break; }
    }
    if (!hlen)
    {
        if (!n) H64_INFO("[net] %s: TLS error %d", host, br_ssl_engine_last_error(&t->sc.eng));
        goto done;
    }
    head[hlen - 2] = 0;   // the headers alone, one CRLF kept for header()
    if (strncmp(head, "HTTP/1.", 7) || strlen(head) < 12) goto done;
    status = atoi(head + 9);
    {
        int vn;
        const char *v = header(head, "location", &vn);
        if (v && location && locSize > 0)
        {
            if (vn >= locSize) vn = locSize - 1;
            memcpy(location, v, vn);
            location[vn] = 0;
        }
    }
    if (!body || status < 200 || status >= 300) { ok = 1; goto done; }

    // The body: Content-Length bytes, chunked, or up to the end of the connection.
    {
        int vn, chunked = 0;
        const char *te = header(head, "transfer-encoding", &vn), *cl = header(head, "content-length", &vn);
        u32 want = 0;
        if (te && !_strnicmp(te, "chunked", 7)) chunked = 1;
        if (cl && !chunked) want = (u32)strtoul(cl, NULL, 10);
        if (want > maxLen) { H64_INFO("[net] %s%s: %u bytes, more than %u", host, path, want, maxLen); goto done; }
        cap = want ? want : 64 * 1024;
        buf = (u8 *)malloc(cap + 1);
        if (!buf) goto done;
        got = (u32)(n - hlen);
        if (got > cap) got = cap;
        memcpy(buf, head + hlen, got);
        for (;;)
        {
            if (want && got >= want) break;
            if (got == cap)
            {
                u8 *nb;
                // A chunked answer carries its framing on top of the data.
                if (cap >= maxLen + (chunked ? maxLen / 16 + 4096 : 0)) { H64_INFO("[net] %s%s: more than %u bytes", host, path, maxLen); goto done; }
                cap *= 2;
                nb = (u8 *)realloc(buf, cap + 1);
                if (!nb) goto done;
                buf = nb;
            }
            r = br_sslio_read(&t->ioc, buf + got, cap - got);
            if (r <= 0) break;
            got += (u32)r;
        }
        if (want && got < want) { H64_INFO("[net] %s%s: cut at %u of %u bytes", host, path, got, want); goto done; }
        if (chunked)
        {
            int d = dechunk(buf, got);
            if (d < 0) { H64_INFO("[net] %s%s: bad chunked answer", host, path); goto done; }
            got = (u32)d;
        }
        if (got > maxLen) goto done;
        buf[got] = 0;
        *body = buf;
        *bodyLen = got;
        buf = NULL;
        ok = 1;
    }

done:
    closesocket(s);
    free(buf);
    free(head);
    free(req);
    free(t);
    return ok ? status : 0;
}

int NetGet(const char *host, const char *path, int follow, u8 **body, u32 *len, u32 maxLen, char *location, int locSize)
{
    char curHost[128], *curPath = (char *)malloc(NET_URL_MAX + 1), *loc = (char *)malloc(NET_URL_MAX + 1);
    int hop, status = 0, done = 0;
    if (body) *body = NULL;
    if (len) *len = 0;
    if (!curPath || !loc) { free(curPath); free(loc); return 0; }
    strncpy(curHost, host, sizeof(curHost) - 1);
    curHost[sizeof(curHost) - 1] = 0;
    strncpy(curPath, path, NET_URL_MAX);
    curPath[NET_URL_MAX] = 0;
    for (hop = 0; hop < 5 && !done; hop++)
    {
        status = request(curHost, curPath, body, len, maxLen, loc, NET_URL_MAX + 1);
        if (location && locSize > 0)
        {
            strncpy(location, loc, locSize - 1);
            location[locSize - 1] = 0;
        }
        if (!follow || status < 300 || status >= 400 || !loc[0]) { done = 1; break; }
        // https://host/path, or /path on the same host.
        if (!strncmp(loc, "https://", 8))
        {
            const char *h = loc + 8, *slash = strchr(h, '/');
            size_t hl = slash ? (size_t)(slash - h) : strlen(h);
            if (hl >= sizeof(curHost)) { status = 0; break; }
            memcpy(curHost, h, hl);
            curHost[hl] = 0;
            strncpy(curPath, slash ? slash : "/", NET_URL_MAX);
        }
        else if (loc[0] == '/')
            strncpy(curPath, loc, NET_URL_MAX);
        else
        {
            status = 0;   // plain HTTP or a relative path: not followed
            break;
        }
        curPath[NET_URL_MAX] = 0;
    }
    if (!done) status = 0;   // too many redirects, or one not followed
    free(curPath);
    free(loc);
    return status;
}
