// Harissa64 - HTTPS downloads from the console (the update check, box art and
// game information).
//
// The Xbox 360 libraries speak no modern TLS: BearSSL (third_party/bearssl,
// MIT) does TLS 1.2. Server certificates are not checked against root
// certificates (the console has none to give BearSSL), so a download can only
// be trusted as much as the network: callers use what they fetch as data
// (version numbers, pictures, text), never as code, and check it as such.
// Sockets to the internet need XNET_STARTUP_BYPASS_SECURITY and the
// "insecure" socket option, which RGH/JTAG kernels honour.
//
// Calls block (DNS, connection, transfer; 10 s time-outs): use them from a
// worker thread, never from the UI thread. Several threads may download at
// the same time (each call has its own TLS state).
#ifndef XB_NET_H
#define XB_NET_H

#include "../../core/common/h64_types.h"

// Starts the network stack (first call) and waits up to 15 s for a link and
// an address. Returns 1 when online. Later calls return the first answer
// at once (no new wait) unless `retry` is set.
int NetStart(int retry);

// GET https://host/path over HTTP/1.1. Redirects are followed (up to 4) when
// `follow` is set; otherwise a redirect's Location goes to `location`. With
// `body` NULL only the headers are read. The body (Content-Length, chunked or
// up to the end of the connection) is malloc'd with a NUL after it; answers
// longer than `maxLen` fail.
// Returns the HTTP status (200, 404...), or 0 when the server could not be
// reached (no DNS answer, no connection, TLS failure, time-out).
int NetGet(const char *host, const char *path, int follow, u8 **body, u32 *len, u32 maxLen, char *location, int locSize);

// POST `data` (`type`: its Content-Type, NULL for a form) to https://host/path;
// `agent` is added to the User-Agent (NULL: nothing). No redirects. The answer
// and the result are as NetGet's.
int NetPost(const char *host, const char *path, const char *data, const char *type, const char *agent, u8 **body, u32 *len, u32 maxLen);
// Downloads https://... to `file`, following redirects. Bytes already in the
// file (an interrupted download) are kept and the rest is asked for (Range);
// a dropped connection is tried again a few times. `expected`: the whole
// size (0: unknown). progress(user, done, total) runs between blocks; non-zero
// stops. Returns 1 when the file is complete, 0 on failure, -1 when stopped.
int NetDownload(const char *url, const char *file, u32 expected, int (*progress)(void *user, u32 done, u32 total), void *user);
// The SHA-256 of a file as 64 lowercase hex digits. Returns 1, 0 (unreadable), -1 (stopped).
int NetSha256File(const char *file, char hex[65], int (*progress)(void *user, u32 done, u32 total), void *user);

#endif
