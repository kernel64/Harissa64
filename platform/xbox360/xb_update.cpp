// Harissa64 - is a newer release on GitHub? (see xb_update.h)
//
// github.com only speaks HTTPS (TLS 1.2), which the Xbox 360 libraries do
// not: BearSSL (third_party/bearssl, MIT) does the TLS. The request is
// GET /kernel64/Harissa64/releases/latest, whose answer is a redirect to
// .../releases/tag/v<version>: the version is read from the Location header,
// the page itself is never downloaded.
//
// The server's certificate is not checked against root certificates (the
// console has none to give BearSSL): only a version number is read, and the
// worst a fake server could do is show a wrong "new version" message.
// Sockets to the internet need XNET_STARTUP_BYPASS_SECURITY and the
// "insecure" socket option, which RGH/JTAG kernels honour.

#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include "../../core/common/h64_log.h"
#include "xb_update.h"

extern "C" VOID XeCryptRandom(PBYTE pb, DWORD cb);   // xboxkrnl export

#define UPDATE_HOST "github.com"
#define UPDATE_PATH "/kernel64/Harissa64/releases/latest"
#define SO_MARKINSECURE 0x5801   // RGH homebrew: allow this socket outside Xbox Live

static volatile LONG s_started, s_found;
static char s_current[32], s_latest[32];
static HANDLE s_thread;

// ---- a certificate check that accepts a chain with no trusted root
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

// "v2.0.1", "2.0.2-dev" -> 2, 0, 1 (missing parts 0).
static void parse_version(const char *s, int v[3])
{
    int i;
    if (*s == 'v' || *s == 'V') s++;
    for (i = 0; i < 3; i++)
    {
        v[i] = atoi(s);
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '.') s++;
    }
}

static int newer(const char *latest, const char *current)
{
    int a[3], b[3], i;
    parse_version(latest, a);
    parse_version(current, b);
    for (i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return 0;
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

static int resolve(const char *host, IN_ADDR *addr)
{
    XNDNS *dns = NULL;
    WSAEVENT ev = WSACreateEvent();
    int ok = 0;
    if (ev == WSA_INVALID_EVENT) return 0;
    if (XNetDnsLookup(host, ev, &dns) == 0 && dns)
    {
        WaitForSingleObject(ev, 10000);
        if (dns->iStatus == 0 && dns->cina > 0) { *addr = dns->aina[0]; ok = 1; }
        XNetDnsRelease(dns);
    }
    WSACloseEvent(ev);
    return ok;
}

// Fills `version` from the Location header of the redirect; 0 on failure.
static int fetch_latest(char *version, size_t size)
{
    static br_ssl_client_context sc;
    static br_x509_minimal_context xc;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    static char resp[8192];
    NoAnchor na;
    br_sslio_context ioc;
    unsigned char seed[32];
    uint32_t days, secs;
    IN_ADDR ip;
    SOCKADDR_IN sa;
    SOCKET s;
    BOOL one = TRUE;
    DWORD timeout = 10000;
    const char *req = "GET " UPDATE_PATH " HTTP/1.1\r\nHost: " UPDATE_HOST "\r\nUser-Agent: Harissa64\r\nConnection: close\r\n\r\n";
    int n = 0, r, ok = 0;
    char *p, *e;

    if (!resolve(UPDATE_HOST, &ip)) { H64_INFO("[update] cannot resolve " UPDATE_HOST); return 0; }
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
        H64_INFO("[update] cannot connect (%d)", WSAGetLastError());
        closesocket(s);
        return 0;
    }

    br_ssl_client_init_full(&sc, &xc, NULL, 0);
    ssl_time(&days, &secs);
    br_x509_minimal_set_time(&xc, days, secs);
    na.vtable = &s_noAnchorVtable;
    na.inner = &xc.vtable;
    br_ssl_engine_set_x509(&sc.eng, &na.vtable);
    XeCryptRandom(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
    br_ssl_client_reset(&sc, UPDATE_HOST, 0);
    br_sslio_init(&ioc, &sc.eng, sock_read, &s, sock_write, &s);

    if (br_sslio_write_all(&ioc, req, strlen(req)) == 0 && br_sslio_flush(&ioc) == 0)
    {
        // The headers are enough.
        while (n < (int)sizeof(resp) - 1)
        {
            r = br_sslio_read(&ioc, resp + n, sizeof(resp) - 1 - n);
            if (r <= 0) break;
            n += r;
            resp[n] = 0;
            if (strstr(resp, "\r\n\r\n")) break;
        }
    }
    resp[n] = 0;
    if (!n) H64_INFO("[update] TLS error %d", br_ssl_engine_last_error(&sc.eng));
    closesocket(s);

    // "Location: https://github.com/kernel64/Harissa64/releases/tag/v2.0.1"
    p = strstr(resp, "/releases/tag/");
    if (p)
    {
        p += 14;
        for (e = p; *e && *e != '\r' && *e != '\n' && *e != '"' && *e != '/'; e++) ;
        if (e > p && (size_t)(e - p) < size)
        {
            memcpy(version, p, e - p);
            version[e - p] = 0;
            if (*version == 'v' || *version == 'V') memmove(version, version + 1, strlen(version));
            ok = version[0] >= '0' && version[0] <= '9';
        }
    }
    if (!ok && n) H64_INFO("[update] no release in the answer (%.40s)", resp);
    return ok;
}

static DWORD WINAPI update_thread(LPVOID)
{
    XNetStartupParams xnsp;
    WSADATA wsa;
    char latest[32];
    DWORD t0 = GetTickCount();
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&xnsp) != 0 || WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { H64_INFO("[update] no network stack"); return 0; }
    if (!wait_network()) { H64_INFO("[update] no network"); return 0; }
    if (!fetch_latest(latest, sizeof(latest))) return 0;
    H64_INFO("[update] latest release %s, running %s (%u ms)", latest, s_current, (unsigned)(GetTickCount() - t0));
    if (newer(latest, s_current))
    {
        strncpy(s_latest, latest, sizeof(s_latest) - 1);
        InterlockedExchange(&s_found, 1);
    }
    return 0;
}

void UpdateCheckStart(const char *current)
{
    if (InterlockedExchange(&s_started, 1)) return;
    strncpy(s_current, current, sizeof(s_current) - 1);
    // A big stack for the TLS handshake (RSA/ECDHE); hardware thread 3, away
    // from the CPU (0) and graphics (2, 5) threads.
    s_thread = CreateThread(NULL, 256 * 1024, update_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_thread) return;
    XSetThreadProcessor(s_thread, 3);
    SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(s_thread);
}

int UpdateCheckTake(char *version, size_t size)
{
    if (!InterlockedExchange(&s_found, 0)) return 0;
    strncpy(version, s_latest, size - 1);
    version[size - 1] = 0;
    return 1;
}
