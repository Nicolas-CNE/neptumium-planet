/*
 * net.h — Neptumium Planet: minimal network layer (HTTP/HTTPS fetch to memory).
 *
 * WHAT THIS IS
 *   A small wrapper around libcurl with a single responsibility: return the
 *   response body of a URL in a caller-owned buffer. Both network users in
 *   Planet go through it:
 *
 *     1. the repository databases  (core.db.tar.gz / extra.db.tar.gz),
 *     2. the package payloads      (.pkg.tar.zst / .tango.tar.zst) — later.
 *
 * DESIGN NOTES
 *   - Fetches into a growable buffer driven by libcurl's WRITEFUNCTION
 *     callback; no temp files on disk, and the archive readers can read the
 *     image straight from memory.
 *   - Follows redirects (mirrors redirect frequently), verifies TLS against
 *     the system CA bundle, and sends an identifiable User-Agent so mirror
 *     operators can identify clients in their access logs.
 *   - Transfers always carry a total timeout and a stall timeout, so an
 *     unresponsive mirror cannot block the CLI indefinitely.
 *   - net_global_init() / net_global_cleanup() wrap curl_global_init(EVERY);
 *     call init once at startup and cleanup at exit.
 *
 *   Every function returns 0 on success and -1 on failure with a
 *   human-readable message available from net_error().
 */
#ifndef NEPTUMIUM_NET_H
#define NEPTUMIUM_NET_H

#include <stddef.h>

/* Default timeout (seconds) applied to the whole transfer unless overridden. */
#define NET_DEFAULT_TIMEOUT 120L

/* Initialise / tear down the underlying library. Call net_global_init() once
 * before any fetch and net_global_cleanup() at process exit. Return 0/-1. */
int  net_global_init(void);
void net_global_cleanup(void);

/* Fetch URL into a freshly malloc'd NUL-terminated buffer. On success stores
 * the buffer in *out_buf, its length in *out_len (excludes the NUL) and
 * returns 0. On any failure returns -1 (net_error() explains) and leaves the
 * outputs untouched. The caller owns the buffer (free()). timeout_secs<=0
 * selects NET_DEFAULT_TIMEOUT. */
int  net_fetch(const char *url, unsigned char **out_buf, size_t *out_len,
               long timeout_secs);

/* Fetch URL into a file already opened by the caller (e.g. a cache file).
 * Uses CURLOPT_WRITEDATA on the FILE*, so no whole-file buffer is needed —
 * important for multi-hundred-MB package payloads. Returns 0/-1. */
int  net_fetch_to_file(const char *url, void *fp /* FILE* */,
                       long timeout_secs);

/* Describe the most recent failure of any net_* call in this process. Never
 * NULL; "" when there is nothing to report. */
const char *net_error(void);

#endif /* NEPTUMIUM_NET_H */
