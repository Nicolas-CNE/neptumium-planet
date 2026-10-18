/*
 * state.c — Neptunium Planet: transaction locking and system profiles.
 * ======================================================================
 *
 * See state.h for the rationale.
 */
#define _GNU_SOURCE  /* getline(), strdup() under -std=c11 */

#include "state.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "localdb.h"

static char g_err[512];

const char *state_error(void) { return g_err; }

static int fail(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

/* Directory holding planet's state on the target root. */
static char *state_dir(const char *root) {
    size_t n = strlen(root) + 40;
    char *p = malloc(n);
    if (!p) return NULL;
    const char *sep = (root[0] == '/' && root[1] == '\0') ? "" : "/";
    snprintf(p, n, "%s%svar/lib/planet", root, sep);
    return p;
}

/* mkdir -p: the target root may be brand new, in which case its /var/lib
 * parent does not exist yet and a single mkdir() would fail. */
static int mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

/* ---- locking ------------------------------------------------------------- */

static char  g_lockfile[1024];
static int   g_locked = 0;
static int   g_lock_fd = -1;

void lock_release(void) {
    if (g_locked && g_lockfile[0]) remove(g_lockfile);
    g_locked = 0;
    g_lock_fd = -1;
}

/* Is the process recorded in the lock file still alive? */
static int pid_alive(long pid) {
    if (pid <= 0) return 0;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

/* Read the PID from an existing lock file. Returns -1 when unreadable. */
static long lockfile_pid(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[64] = {0};
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    fclose(f);
    return strtol(line, NULL, 10);
}

int lock_acquire(const char *root) {
    char *dir = state_dir(root);
    if (!dir) return fail("out of memory");
    if (mkdirs(dir) != 0) {
        int e = errno;
        int r = fail("cannot create %s: %s", dir, strerror(e));
        free(dir);
        return r;
    }

    snprintf(g_lockfile, sizeof g_lockfile, "%s/db.lck", dir);

    /* O_EXCL makes creation atomic: whoever wins the create holds the lock. */
    int fd = open(g_lockfile, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0 && errno == EEXIST) {
        /* A lock exists. Reclaim it only if its owner is gone (a crash);
         * otherwise refuse so we never interleave with a live transaction. */
        long pid = lockfile_pid(g_lockfile);
        if (pid_alive(pid))
            return fail("another planet transaction is running (pid %ld); "
                        "if that is wrong, remove %s", pid, g_lockfile);
        remove(g_lockfile);            /* stale: reclaim */
        fd = open(g_lockfile, O_CREAT | O_EXCL | O_WRONLY, 0644);
    }
    if (fd < 0) {
        int e = errno;
        free(dir);
        return fail("cannot lock %s: %s", g_lockfile, strerror(e));
    }

    dprintf(fd, "%ld\n", (long)getpid());
    g_lock_fd = fd;
    g_locked = 1;
    free(dir);

    /* Release automatically on abnormal termination, so a crash never leaves
     * a lock that outlives us. */
    atexit(lock_release);
    return 0;
}

/* ---- profiles ------------------------------------------------------------ */

int profile_save(const char *root, const char *name) {
    char *dir = state_dir(root);
    if (!dir) return fail("out of memory");

    char profdir[1200];
    snprintf(profdir, sizeof profdir, "%s/profiles", dir);
    if (mkdir(profdir, 0755) != 0 && errno != EEXIST) {
        int e = errno;
        free(dir);
        return fail("cannot create %s: %s", profdir, strerror(e));
    }

    char path[1400];
    snprintf(path, sizeof path, "%s/%s", profdir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        int e = errno;
        free(dir);
        return fail("cannot write %s: %s", path, strerror(e));
    }

    int n = 0;
    package **inst = localdb_list(root, &n);
    fprintf(f, "# planet profile '%s' — %d package(s)\n", name, n);
    for (int i = 0; inst && i < n; i++)
        fprintf(f, "%s=%s\n", inst[i]->name, inst[i]->version);
    localdb_free_list(inst);
    fclose(f);
    free(dir);
    return 0;
}

char *profile_list(const char *root) {
    char *dir = state_dir(root);
    if (!dir) return NULL;
    char profdir[1200];
    snprintf(profdir, sizeof profdir, "%s/profiles", dir);
    free(dir);

    char *out = calloc(1, 1);
    if (!out) return NULL;
    DIR *d = opendir(profdir);
    if (!d) return out;                 /* no profiles yet */
    struct dirent *ent;
    size_t used = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t need = used + strlen(ent->d_name) + 2;
        char *grown = realloc(out, need);
        if (!grown) break;
        out = grown;
        used += (size_t)snprintf(out + used, need - used, "%s%s",
                                 used ? "\n" : "", ent->d_name);
    }
    closedir(d);
    return out;
}

int profile_load(const char *root, const char *name, char ***out) {
    *out = NULL;
    char *dir = state_dir(root);
    if (!dir) return -1;
    char path[1400];
    snprintf(path, sizeof path, "%s/profiles/%s", dir, name);
    free(dir);

    FILE *f = fopen(path, "r");
    if (!f) return fail("no such profile '%s'", name);

    int n = 0, cap = 16;
    char **v = malloc((size_t)cap * sizeof *v);
    if (!v) { fclose(f); return -1; }

    char *line = NULL;
    size_t cap_line = 0;
    ssize_t len;
    while ((len = getline(&line, &cap_line, f)) > 0) {
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;
        if (n == cap) {
            cap *= 2;
            char **nv = realloc(v, (size_t)cap * sizeof *nv);
            if (!nv) break;
            v = nv;
        }
        v[n] = strdup(line);
        if (!v[n]) break;
        n++;
    }
    free(line);
    fclose(f);
    if (n == 0) { free(v); return fail("profile '%s' is empty", name); }
    *out = v;
    return n;
}

int profile_remove(const char *root, const char *name) {
    char *dir = state_dir(root);
    if (!dir) return -1;
    char path[1400];
    snprintf(path, sizeof path, "%s/profiles/%s", dir, name);
    free(dir);
    if (remove(path) != 0)
        return fail("cannot remove profile '%s': %s", name, strerror(errno));
    return 0;
}

int lock_write(const char *root) {
    char *dir = state_dir(root);
    if (!dir) return fail("out of memory");
    char path[1200];
    snprintf(path, sizeof path, "%s/installed.lock", dir);
    free(dir);

    FILE *f = fopen(path, "w");
    if (!f) return fail("cannot write %s: %s", path, strerror(errno));

    int n = 0;
    package **inst = localdb_list(root, &n);
    fprintf(f, "# planet lock file — %d package(s)\n", n);
    for (int i = 0; inst && i < n; i++)
        fprintf(f, "%s=%s\n", inst[i]->name, inst[i]->version);
    localdb_free_list(inst);
    fclose(f);
    return 0;
}