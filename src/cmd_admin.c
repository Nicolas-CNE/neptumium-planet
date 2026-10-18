/*
 * cmd_admin.c — Neptunium Planet: lock files, profiles and the package cache.
 * ========================================================================
 *
 * A profile is a named snapshot of the installed set. Because transactions
 * are computed by the solver, "roll back to a profile" is just "solve for
 * that set and apply the difference" — which is validated before anything on
 * disk is touched.
 */
#define _GNU_SOURCE

#include "cli.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "localdb.h"
#include "pkgfile.h"
#include "state.h"

int cmd_lock(const options *o, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Lock the current system: write /var/lib/planet/installed.lock with the
     * exact set of installed name=version pairs. Applying the lock later
     * reproduces this system state exactly. */
    int n = 0;
    package **inst = localdb_list(o->root, &n);
    if (!inst) die(1, "cannot read local db");

    char *path = malloc(strlen(o->root) + 64);
    snprintf(path, strlen(o->root) + 64, "%s/var/lib/planet/installed.lock", o->root);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "%serror:%s cannot write %s: %s\n", C_RED, C_OFF, path,
                strerror(errno));
        free(path); localdb_free_list(inst);
        return 1;
    }
    fprintf(f, "# planet lock file — generated %s", ctime(&(time_t){time(NULL)}));
    for (int i = 0; i < n; i++)
        fprintf(f, "%s=%s\n", inst[i]->name, inst[i]->version);
    fclose(f);
    printf("%swrote %d package(s) to %s%s\n", C_GREEN, n, path, C_OFF);
    free(path); localdb_free_list(inst);
    return 0;
}

/* ---- cache --------------------------------------------------------------- */

int cmd_cache(const options *o, int argc, char **argv) {
    const char *sub = argc > 0 ? argv[0] : "list";
    /* Report the cache the rest of the tool actually uses, honouring an
     * explicit -c/--cache-dir. */
    pkgfile_cache_dir_set(o->cache_dir);
    char *dir = pkgfile_cache_dir();
    if (!dir) die(1, "out of memory");

    if (!strcmp(sub, "clean")) {
        int removed = 0;
        long freed = 0;
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char p[2048];
                struct stat st;
                snprintf(p, sizeof p, "%s/%s", dir, ent->d_name);
                if (stat(p, &st) != 0) continue;
                if (remove(p) == 0) { removed++; freed += (long)st.st_size; }
            }
            closedir(d);
        }
        printf("%sremoved %d cached package(s), %s freed%s\n", C_GREEN,
               removed, hr_size(freed), C_OFF);
        free(dir);
        return 0;
    }

    int count = 0;
    long total = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char p[2048];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", dir, ent->d_name);
            if (stat(p, &st) != 0) continue;
            count++;
            total += (long)st.st_size;
        }
        closedir(d);
    }
    printf("%s%d cached package(s), %s total%s\n  %s(%s)%s\n", C_BOLD, count,
           hr_size(total), C_OFF, C_DIM, dir, C_OFF);
    free(dir);
    return 0;
}

/* ---- profiles ------------------------------------------------------------ */

int cmd_profile(const options *o, int argc, char **argv) {
    const char *sub = argc > 0 ? argv[0] : "list";

    if (!strcmp(sub, "save") || !strcmp(sub, "create")) {
        if (argc < 2) die(2, "profile save needs a name");
        if (profile_save(o->root, argv[1]) != 0) {
            fprintf(stderr, "%serror:%s %s\n", C_RED, C_OFF, state_error());
            return 1;
        }
        printf("%ssaved profile '%s'%s\n", C_GREEN, argv[1], C_OFF);
        return 0;
    }
    if (!strcmp(sub, "list")) {
        char *names = profile_list(o->root);
        if (!names || !*names) { printf("%sno profiles%s\n", C_DIM, C_OFF); free(names); return 0; }
        char *line = names, *next;
        while (line && *line) {
            next = strchr(line, '\n');
            if (next) *next++ = '\0';
            printf("  %s\n", line);
            line = next;
        }
        free(names);
        return 0;
    }
    if (!strcmp(sub, "remove") || !strcmp(sub, "delete")) {
        if (argc < 2) die(2, "profile remove needs a name");
        if (profile_remove(o->root, argv[1]) != 0) {
            fprintf(stderr, "%serror:%s %s\n", C_RED, C_OFF, state_error());
            return 1;
        }
        printf("%sremoved profile '%s'%s\n", C_GREEN, argv[1], C_OFF);
        return 0;
    }
    die(2, "unknown profile action '%s' (try save, list or remove)", sub);
    return 2;
}

/* ---- rollback ------------------------------------------------------------ */

int cmd_rollback(const options *o, int argc, char **argv) {
    const char *name = argc > 0 ? argv[0] : "base";
    char **lines = NULL;
    int n = profile_load(o->root, name, &lines);
    if (n < 0) {
        fprintf(stderr, "%serror:%s %s\n", C_RED, C_OFF, state_error());
        return 1;
    }
    printf("%srolling back to profile '%s' (%d package(s))%s\n", C_BOLD, name, n, C_OFF);

    /* The profile is a request set; run it through the normal transaction
     * path so rollback gets the same solving, diffing and hooks. */
    dep *reqs = calloc((size_t)n, sizeof *reqs);
    int nr = 0;
    if (reqs)
        for (int i = 0; i < n; i++)
            if (dep_parse(&reqs[nr], lines[i])) nr++;
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines);

    int rc = run_transaction(o, "Rollback", reqs, nr, NULL, 0);
    for (int i = 0; i < nr; i++) dep_free(&reqs[i]);
    free(reqs);
    return rc;
}
