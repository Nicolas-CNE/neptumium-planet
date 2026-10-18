/*
 * net.c — Neptumium Planet: HTTP/HTTPS fetch to memory / to a FILE*.
 *
 * See net.h for the module contract.  Implementation notes:
 *
 *   - The growable buffer is a realloc-based byte vector. libcurl passes data
 *     in chunks of arbitrary size; the capacity doubles when needed, so the
 *     total copying stays amortised O(n).
 *   - Returning a short count from the write callback makes libcurl abort the
 *     transfer with CURLE_WRITE_ERROR; that is how an allocation failure in
 *     the buffer is reported, with its own message.
 *   - CURLOPT_FAILONERROR is intentionally NOT set: error responses may carry
 *     a useful body, and the HTTP status is checked explicitly afterwards
 *     (see perform_and_check), which produces a clearer message.
 *   - Errors from curl_easy_strerror are prefixed with the failing URL so the
 *     user can tell which repository (or mirror) is at fault.
 */
#include "net.h"

#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------ */

/* Last error message, printf-style formatted by the failing call.  Static
 * buffer: Planet is single-threaded (the solver is the parallelism story, and
 * it doesn't do I/O), so a static is fine and avoids an allocation on the
 * error path. */
static char g_err[1024];

const char *net_error(void) { return g_err; }

/* Format into g_err and return -1, so callers can `return net_fail(...)`. */
static int net_fail(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static int net_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Growable download buffer                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    unsigned char *data; /* owned bytes, always NUL-terminated            */
    size_t         len;  /* valid byte count (excludes the NUL)           */
    size_t         cap;  /* allocated capacity INCLUDING space for the NUL */
} growbuf;

static void gb_init(growbuf *g) {
    g->data = NULL;
    g->len = g->cap = 0;
}

static void gb_free(growbuf *g) {
    free(g->data);
    g->data = NULL;
    g->len = g->cap = 0;
}

/* libcurl write callback: append (size*nmemb) bytes to the growbuf.
 * Signature must match curl_write_callback: size_t (char*, size_t, size_t,
 * void*). Returning a value != size*nmemb makes libcurl abort the transfer
 * with CURLE_WRITE_ERROR, which is how an allocation failure here surfaces
 * to the caller. */
static size_t gb_append(char *ptr, size_t size, size_t nmemb, void *userdata) {
    growbuf *g = (growbuf *)userdata;
    size_t   n = size * nmemb;

    /* Grow (with room for the trailing NUL we keep at all times). */
    if (g->len + n + 1 > g->cap) {
        size_t want = g->len + n + 1;
        size_t cap  = g->cap ? g->cap : 8192;
        unsigned char *p;
        while (cap < want) cap *= 2;
        p = (unsigned char *)realloc(g->data, cap);
        if (!p) return 0; /* aborts the transfer, see above */
        g->data = p;
        g->cap  = cap;
    }

    memcpy(g->data + g->len, ptr, n);
    g->len += n;
    g->data[g->len] = '\0'; /* keep it a C string: desc files are text */
    return n;
}

/* ------------------------------------------------------------------------ */
/* Global init                                                               */
/* ------------------------------------------------------------------------ */

int net_global_init(void) {
    /* CURL_GLOBAL_ALL brings up SSL (OpenSSL) + sockets + c-ares/IPv6 where
     * available. Must happen exactly once per process before any easy handle
     * is created. */
    if (curl_global_init(CURL_GLOBAL_ALL) != 0)
        return net_fail("curl_global_init failed");
    return 0;
}

void net_global_cleanup(void) { curl_global_cleanup(); }

/* ------------------------------------------------------------------------ */
/* The transfer engine shared by net_fetch / net_fetch_to_file               */
/* ------------------------------------------------------------------------ */

/* Build a configured easy handle for `url`. Returns NULL on failure with
 * g_err set. */
static CURL *make_handle(const char *url, long timeout_secs) {
    CURL *h = curl_easy_init();
    if (!h) {
        net_fail("curl_easy_init failed");
        return NULL;
    }

    curl_easy_setopt(h, CURLOPT_URL, url);

    /* Identify the client in mirror access logs. */
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Neptumium-Planet/0.1 (+planet)");

    /* Mirror URLs commonly redirect (geo-DNS to the nearest edge); follow up
     * to 10 hops. */
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);

    /* TLS verification against the system CA bundle. There is deliberately no
     * option to disable it: a package manager must not offer an insecure mode,
     * since packages are executed with root privileges. */
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);

    /* Two bounds: the transfer as a whole (CURLOPT_TIMEOUT) and the stall
     * timeout (abort when the transfer drops below 1 KB/s for 30 s). */
    curl_easy_setopt(h, CURLOPT_TIMEOUT,
                     timeout_secs > 0 ? timeout_secs : NET_DEFAULT_TIMEOUT);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);

    /* libcurl otherwise uses SIGALRM internally for timeouts, which would
     * interfere with any future signal handling of our own. */
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

    return h;
}

/* Perform the transfer described by `h`, then check the HTTP status. Returns
 * 0 on success, -1 with g_err set otherwise. */
static int perform_and_check(CURL *h, const char *url) {
    CURLcode   rc;
    long       status = 0;

    rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        int r = net_fail("fetch %s: %s", url, curl_easy_strerror(rc));
        curl_easy_cleanup(h);
        return r;
    }

    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 400) {
        int r = net_fail("fetch %s: HTTP %ld", url, status);
        curl_easy_cleanup(h);
        return r;
    }

    curl_easy_cleanup(h);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

int net_fetch(const char *url, unsigned char **out_buf, size_t *out_len,
              long timeout_secs) {
    growbuf g;
    CURL   *h;

    if (!url || !out_buf || !out_len)
        return net_fail("net_fetch: NULL argument");

    gb_init(&g);

    h = make_handle(url, timeout_secs);
    if (!h) {
        gb_free(&g);
        return -1;
    }

    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, gb_append);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &g);

    if (perform_and_check(h, url) != 0) {
        gb_free(&g);
        return -1;
    }

    /* Empty body is legal (rare, but don't crash): hand back "". */
    if (!g.data) {
        g.data = (unsigned char *)malloc(1);
        if (!g.data) {
            gb_free(&g);
            return net_fail("fetch %s: out of memory", url);
        }
        g.data[0] = '\0';
        g.len = 0;
    }

    *out_buf = g.data;
    *out_len = g.len;
    return 0;
}

int net_fetch_to_file(const char *url, void *fp, long timeout_secs) {
    CURL *h;

    if (!url || !fp) return net_fail("net_fetch_to_file: NULL argument");

    h = make_handle(url, timeout_secs);
    if (!h) return -1;

    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, NULL); /* default = fwrite */
    curl_easy_setopt(h, CURLOPT_WRITEDATA, fp);

    return perform_and_check(h, url);
}
