/*
 * hooks.c — Neptunium Planet: ALPM-compatible hooks (implementation).
 * ================================================================
 *
 * See hooks.h for the format and the policy. Implementation notes:
 *
 *   - .INSTALL scripts are pulled from the CACHED payload (pkgfile_fetch
 *     returns the verified file), sourced by /bin/sh with the ALPM positional
 *     arguments, and the temp copy is removed afterwards.
 *   - Repo hooks are parsed from INI-ish text with a deliberately small parser
 *     (sections, `Key = Value`, `#`/`;` comments). Unknown keys are ignored so
 *     hook files written for newer pacman still load.
 *   - Target matching uses fnmatch(), so the glob syntax users already write
 *     in .hook files behaves exactly as they expect.
 */
#define _GNU_SOURCE  /* strdup() under -std=c11 */

#include "hooks.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pkgfile.h"

static char g_err[512];
static int  g_enabled = 1;

const char *hooks_error(void) { return g_err; }

void hooks_set_enabled(int on) { g_enabled = on; }
int  hooks_enabled(void) { return g_enabled; }

static int fail(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

const char *install_phase_func(install_phase p) {
    switch (p) {
        case IP_PRE_INSTALL:  return "pre_install";
        case IP_POST_INSTALL: return "post_install";
        case IP_PRE_UPGRADE:  return "pre_upgrade";
        case IP_POST_UPGRADE: return "post_upgrade";
        case IP_PRE_REMOVE:   return "pre_remove";
        case IP_POST_REMOVE:  return "post_remove";
    }
    return NULL;
}

const char *hook_point_key(hook_point p) {
    switch (p) {
        case HP_PRE_TRANSACTION:  return "PreTransaction";
        case HP_POST_TRANSACTION: return "PostTransaction";
        case HP_PRE_INSTALL:      return "PreInstall";
        case HP_POST_INSTALL:     return "PostInstall";
        case HP_PRE_UPGRADE:      return "PreUpgrade";
        case HP_POST_UPGRADE:     return "PostUpgrade";
        case HP_PRE_REMOVE:       return "PreRemove";
        case HP_POST_REMOVE:      return "PostRemove";
    }
    return NULL;
}

/* Strip leading/trailing whitespace IN PLACE, including the trailing newline
 * that fgets() leaves behind — without the '\n' every parsed value keeps it
 * and no comparison against a keyword ever matches. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    return s;
}

/* ---- per-package .INSTALL scripts ---------------------------------------- */

/* Run `func` from `script` with the ALPM positional arguments:
 *   $1 version, $2 caller args, $3 root, plus the usual environment. */
static int source_and_call(const char *script, const char *func,
                           const char *version, const char *args,
                           const char *root) {
    pid_t pid = fork();
    if (pid < 0) return fail("fork: %s", strerror(errno));

    if (pid == 0) {
        /* Child: run inside the target root so scripts that use absolute
         * paths operate on the system being modified. */
        if (root && chdir(root) != 0) _exit(127);

        /* ALPM semantics: the .INSTALL script is SOURCED and then one of its
         * functions is called with (version, caller args, root). Running
         * "sh script func ..." would merely EXECUTE the script with `func` as
         * $1, and a script that only defines functions would do nothing.
         *
         * In `sh -c cmd name a b c`, $0=name, $1=a ... so:
         *   . "$1"    source the script   ($1 = script path)
         *   shift      drop that argument
         *   "$@"       call the function   (func version args root)
         */
        char arg1[256], arg2[512], arg3[4096];
        snprintf(arg1, sizeof arg1, "%s", version ? version : "");
        snprintf(arg2, sizeof arg2, "%s", args ? args : "");
        snprintf(arg3, sizeof arg3, "%s", root ? root : "/");

        char *argv[10];
        argv[0] = (char *)"/bin/sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)". \"$1\" || exit 1; shift; \"$@\"";
        argv[3] = (char *)"planet-install";
        argv[4] = (char *)script;
        argv[5] = (char *)func;
        argv[6] = arg1;
        argv[7] = arg2;
        argv[8] = arg3;
        argv[9] = NULL;
        execv("/bin/sh", argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("waitpid: %s", strerror(errno));
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    if (WIFEXITED(status))
        return fail("%s exited with status %d", func, WEXITSTATUS(status));
    return fail("%s terminated by signal %d", func, WTERMSIG(status));
}

int hooks_run_install_script(const package *p, const char *root,
                             install_phase phase, const char *args) {
    if (!g_enabled || !p) return 0;

    /* The script only exists inside the payload. pkgfile_fetch returns the
     * cached (already verified) file; for a package being removed the payload
     * is normally already in the cache from its install. */
    char *payload = pkgfile_fetch(p);
    if (!payload) return 0;   /* not available: treat as "no script" */

    char script[512];
    snprintf(script, sizeof script, "/tmp/.planet-install-%ld-%s",
             (long)getpid(), p->name);
    int have = pkgfile_extract_install_script(payload, script);
    free(payload);
    if (have < 0) { remove(script); return -1; }
    if (have == 0) { remove(script); return 0; }   /* no .INSTALL: nothing to do */

    const char *func = install_phase_func(phase);
    /* Sourcing a script that does not define the function is not an error —
     * that is the normal case for most phases. Probe first. */
    char probe[1024];
    snprintf(probe, sizeof probe,
             ". '%s' 2>/dev/null; command -v %s >/dev/null 2>&1", script, func);
    if (system(probe) != 0) { remove(script); return 0; }

    int rc = source_and_call(script, func, p->version, args, root);
    remove(script);
    return rc;
}

/* ---- repository .hook files ---------------------------------------------- */

/* One parsed .hook file. */
typedef struct {
    char *operations;   /* comma-separated list from [Trigger] Operation */
    char *type;         /* "Package" or "Path" */
    char *target;       /* glob(s), comma-separated, for Type=Package */
    char *when;         /* [Action] When keyword */
    char *exec_cmd;     /* [Action] Exec */
    char *description;  /* [Action] Description (for messages) */
    char *path;         /* file it came from */
} repo_hook;

/* Trim and split a comma-separated list into an array. */
static int split_commas(const char *s, char ***out) {
    int n = 0, cap = 4;
    char **v = malloc((size_t)cap * sizeof *v);
    if (!v) return -1;
    const char *p = s;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char *item = malloc(len + 1);
        if (!item) break;
        memcpy(item, p, len);
        item[len] = '\0';
        char *t = trim(item);
        if (*t) {
            if (n == cap) {
                cap *= 2;
                char **nv = realloc(v, (size_t)cap * sizeof *nv);
                if (!nv) { free(item); break; }
                v = nv;
            }
            v[n++] = t;
        } else {
            free(item);
        }
        if (!comma) break;
        p = comma + 1;
    }
    *out = v;
    return n;
}

static void free_list(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* Does the hook's Operation list contain `op`? */
static int hook_matches_op(const repo_hook *h, const char *op) {
    char **ops; int n = split_commas(h->operations ? h->operations : "", &ops);
    int hit = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(ops[i], op) == 0) { hit = 1; break; }
    free_list(ops, n);
    return hit;
}

/* Does any of `pkgs` match the hook's Target glob list? */
static int hook_matches_target(const repo_hook *h, const package *const *pkgs,
                               int npkgs) {
    char **tg; int n = split_commas(h->target ? h->target : "", &tg);
    int hit = 0;
    for (int i = 0; i < n && !hit; i++)
        for (int k = 0; k < npkgs; k++)
            if (fnmatch(tg[i], pkgs[k]->name, 0) == 0) { hit = 1; break; }
    free_list(tg, n);
    return hit;
}

/* Parse one .hook file. Unknown keys are ignored. */
static int parse_hook_file(const char *path, repo_hook *h) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(h, 0, sizeof *h);
    h->path = strdup(path);

    char line[1024], section[32] = "";
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) { *end = '\0'; snprintf(section, sizeof section, "%s", s + 1); }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);

        if (!strcmp(section, "Trigger")) {
            if      (!strcmp(key, "Operation")) h->operations = strdup(val);
            else if (!strcmp(key, "Type"))      h->type = strdup(val);
            else if (!strcmp(key, "Target"))    h->target = strdup(val);
        } else if (!strcmp(section, "Action")) {
            if      (!strcmp(key, "When"))        h->when = strdup(val);
            else if (!strcmp(key, "Exec"))        h->exec_cmd = strdup(val);
            else if (!strcmp(key, "Description")) h->description = strdup(val);
        }
    }
    fclose(f);
    return 0;
}

static void free_hook(repo_hook *h) {
    free(h->operations); free(h->type); free(h->target);
    free(h->when); free(h->exec_cmd); free(h->description); free(h->path);
}

/* Directory holding repository hooks for a root. */
static char *hooks_dir(const char *root) {
    size_t n = strlen(root) + 48;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/etc/planet/hooks", root);
    return p;
}

/* Run one hook's Exec with the ALPM environment. */
static int run_hook_exec(const repo_hook *h, const char *root,
                         const package *const *pkgs, int npkgs) {
    /* CAASVARS: newline-separated "name=value" for every package, which is how
     * pacman passes bulk information to hooks. */
    char caas[8192];
    size_t used = 0;
    caas[0] = '\0';
    for (int i = 0; i < npkgs && used + 128 < sizeof caas; i++)
        used += (size_t)snprintf(caas + used, sizeof caas - used,
                                 "NAME%d=%s\nVERSION%d=%s\n",
                                 i, pkgs[i]->name, i, pkgs[i]->version);

    setenv("ROOT", root, 1);
    setenv("CAASVARS", caas, 1);
    setenv("PACKAGE", npkgs == 1 ? pkgs[0]->name : "", 1);
    setenv("PACKAGE_NAME", npkgs == 1 ? pkgs[0]->name : "", 1);
    setenv("VERSION", npkgs == 1 ? pkgs[0]->version : "", 1);
    setenv("DATABASE", "/var/lib/planet/local", 1);
    if (npkgs == 1) setenv("OLDPACKAGE", pkgs[0]->version, 1);

    pid_t pid = fork();
    if (pid < 0) return fail("fork: %s", strerror(errno));
    if (pid == 0) {
        /* Hooks operate on the system being modified. */
        if (chdir(root) != 0) _exit(127);
        execl("/bin/sh", "sh", "-c", h->exec_cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    if (WIFEXITED(status))
        return fail("hook '%s' exited with status %d",
                    h->description ? h->description : h->path,
                    WEXITSTATUS(status));
    return fail("hook '%s' terminated by signal %d",
                h->description ? h->description : h->path,
                WTERMSIG(status));
}

int hooks_run_repo(const char *root, const char *op, hook_point point,
                   const package *const *pkgs, int npkgs) {
    if (!g_enabled || !root) return 0;

    char *dir = hooks_dir(root);
    if (!dir) return -1;
    DIR *d = opendir(dir);
    if (!d) { free(dir); return 0; }   /* no hooks directory: nothing to do */

    const char *want = hook_point_key(point);
    int failures = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t ln = strlen(ent->d_name);
        if (ln < 5 || strcmp(ent->d_name + ln - 5, ".hook") != 0) continue;

        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);

        repo_hook h;
        if (parse_hook_file(path, &h) != 0) continue;

        int run = 0;
        int type_ok = (h.type && strcmp(h.type, "Path") == 0) ||
                      hook_matches_target(&h, pkgs, npkgs);
        if (h.exec_cmd && h.when && strcmp(h.when, want) == 0 &&
            hook_matches_op(&h, op) && type_ok)
            run = 1;

        if (run && run_hook_exec(&h, root, pkgs, npkgs) != 0) {
            fprintf(stderr, "hook failed: %s\n",
                    h.description ? h.description : path);
            failures++;
        }
        free_hook(&h);
    }
    closedir(d);
    free(dir);

    if (failures) return -1;
    return 0;
}