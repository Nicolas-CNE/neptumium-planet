/*
 * cmd_query.c — Neptumium Planet: inspecting the installed system.
 * =====================================================
 *
 * Read-only views of what is on the target root: what is installed, which
 * files it owns, who owns a path, and why a package is there at all.
 */
#define _GNU_SOURCE

#include "cli.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "localdb.h"

/* Load the installed packages, sorted by name. */
int installed_packages(const options *o, package ***out) {
    int n = 0;
    package **list = localdb_list(o->root, &n);
    if (!list) die(1, "cannot read the local database under %s", o->root);
    for (int i = 1; i < n; i++) {           /* insertion sort by name */
        package *k = list[i];
        int j = i - 1;
        while (j >= 0 && strcmp(list[j]->name, k->name) > 0) {
            list[j + 1] = list[j]; j--;
        }
        list[j + 1] = k;
    }
    *out = list;
    return n;
}

/* Path of an installed package's recorded file list. */
static void files_path(char *buf, size_t n, const options *o, const package *p) {
    snprintf(buf, n, "%s/%s-%s/files", localdb_path(o->root), p->name, p->version);
}

/* Index of an installed package by name, or -1. Returning an index rather
 * than a pointer keeps callers from doing pointer arithmetic on the array. */
static int find_installed(package **inst, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(inst[i]->name, name) == 0) return i;
    return -1;
}

int cmd_files(const options *o, int argc, char **argv) {
    if (argc < 1) die(2, "files needs a package name");
    int rc = 0;
    for (int a = 0; a < argc; a++) {
        package **inst = NULL;
        int n = installed_packages(o, &inst);
        int pi = find_installed(inst, n, argv[a]);
        if (pi < 0) {
            fprintf(stderr, "%serror:%s '%s' is not installed\n", C_RED, C_OFF, argv[a]);
            localdb_free_list(inst);
            rc = 1;
            continue;
        }
        const package *p = inst[pi];
        char path[2048];
        files_path(path, sizeof path, o, p);
        FILE *f = fopen(path, "r");
        if (!f) {
            printf("%s%s: no files recorded%s\n", p->name, C_DIM, C_OFF);
        } else {
            char line[4096];
            while (fgets(line, sizeof line, f)) {
                size_t l = strlen(line);
                while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
                if (l) printf("%s\n", line);
            }
            fclose(f);
        }
        localdb_free_list(inst);
    }
    return rc;
}

int cmd_owns(const options *o, int argc, char **argv) {
    if (argc < 1) die(2, "owns needs at least one path");
    int rc = 0;
    for (int a = 0; a < argc; a++) {
        /* Accept absolute paths under the root and relative ones alike. */
        const char *path = argv[a];
        size_t rl = strlen(o->root);
        if (strncmp(path, o->root, rl) == 0) path += rl;
        while (*path == '/') path++;

        package **inst = NULL;
        int n = installed_packages(o, &inst);
        int found = 0;
        for (int i = 0; i < n && !found; i++) {
            char path2[2048];
            files_path(path2, sizeof path2, o, inst[i]);
            FILE *f = fopen(path2, "r");
            if (!f) continue;
            char line[4096];
            while (fgets(line, sizeof line, f)) {
                size_t l = strlen(line);
                while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
                /* Directory entries carry a trailing slash; anything beneath
                 * one still belongs to that package. */
                if (strcmp(line, path) == 0 ||
                    (strcmp(line, path) > 0 && strncmp(path, line, l) == 0)) {
                    printf("%s %s%s%s\n", inst[i]->name, C_DIM, argv[a], C_OFF);
                    found = 1;
                    break;
                }
            }
            fclose(f);
        }
        if (!found) {
            fprintf(stderr, "%serror:%s no package owns '%s'\n", C_RED, C_OFF, argv[a]);
            rc = 1;
        }
        localdb_free_list(inst);
    }
    return rc;
}

int cmd_why(const options *o, int argc, char **argv) {
    if (argc < 1) die(2, "why needs a package name");
    int rc = 0;
    for (int a = 0; a < argc; a++) {
        package **inst = NULL;
        int n = installed_packages(o, &inst);
        int ti = find_installed(inst, n, argv[a]);
        const package *target = ti >= 0 ? inst[ti] : NULL;
        if (!target) {
            fprintf(stderr, "%serror:%s '%s' is not installed\n", C_RED, C_OFF, argv[a]);
            localdb_free_list(inst);
            rc = 1;
            continue;
        }
        printf("%s%s %s%s %s(%s)%s\n", C_BOLD, target->name, target->version, C_OFF,
               C_DIM,
               target->explicit_installed ? "explicitly installed"
                                          : "installed as a dependency",
               C_OFF);
        if (target->explicit_installed) { localdb_free_list(inst); continue; }

        /* Walk the installed dependency graph back to an explicit root. */
        unsigned char *seen = calloc((size_t)n, 1);
        const package **queue = malloc((size_t)n * sizeof *queue);
        int head = 0, tail = 0, found = 0;
        if (seen && queue) {
            seen[ti] = 1;
            queue[tail++] = target;
            while (head < tail && !found) {
                const package *cur = queue[head++];
                for (int i = 0; i < n && !found; i++) {
                    const package *cand = inst[i];
                    if (seen[i]) continue;
                    for (int d = 0; d < cand->ndepends; d++)
                        if (pkg_provides(cur, cand->depends[d].name,
                                         cand->depends[d].op,
                                         cand->depends[d].version)) {
                            seen[i] = 1;
                            queue[tail++] = cand;
                            printf("  %s%s %s%s depends on %s%s %s%s\n",
                                   C_DIM, cand->name, cand->version, C_OFF,
                                   C_CYAN, cur->name, cur->version, C_OFF);
                            if (cand->explicit_installed) found = 1;
                            break;
                        }
                }
            }
        }
        free(seen); free(queue);
        if (!found)
            printf("  %s(nothing explicitly installed depends on it — orphan candidate)%s\n",
                   C_YELLOW, C_OFF);
        localdb_free_list(inst);
    }
    return rc;
}

int cmd_query(const options *o, int argc, char **argv) {
    const char *sub = argc > 0 ? argv[0] : "list";
    package **inst = NULL;
    int n = installed_packages(o, &inst);

    if (!strcmp(sub, "list")) {
        printf("%s%d package(s) installed%s\n", C_BOLD, n, C_OFF);
        for (int i = 0; i < n; i++)
            printf("%s%-24s %-22s%s %s%s%s\n", C_CYAN, inst[i]->name, inst[i]->version,
                   C_OFF,
                   inst[i]->explicit_installed ? C_GREEN : C_DIM,
                   inst[i]->explicit_installed ? "explicit" : "dependency", C_OFF);
    } else if (!strcmp(sub, "orphans")) {
        int orphans = 0;
        for (int i = 0; i < n; i++) {
            if (inst[i]->explicit_installed) continue;
            int required = 0;
            for (int j = 0; j < n && !required; j++) {
                if (i == j) continue;
                for (int d = 0; d < inst[j]->ndepends; d++)
                    if (pkg_provides(inst[i], inst[j]->depends[d].name,
                                     inst[j]->depends[d].op,
                                     inst[j]->depends[d].version)) { required = 1; break; }
            }
            if (!required) {
                printf("%s%s %s%s  %s(orphan)%s\n", C_CYAN, inst[i]->name,
                       inst[i]->version, C_OFF, C_YELLOW, C_OFF);
                orphans++;
            }
        }
        if (!orphans) printf("%sno orphans%s\n", C_DIM, C_OFF);
    } else if (!strcmp(sub, "check")) {
        int broken = 0;
        for (int i = 0; i < n; i++)
            for (int d = 0; d < inst[i]->ndepends; d++) {
                int ok = 0;
                for (int j = 0; j < n && !ok; j++)
                    if (pkg_provides(inst[j], inst[i]->depends[d].name,
                                     inst[i]->depends[d].op,
                                     inst[i]->depends[d].version)) ok = 1;
                if (!ok) {
                    printf("%s%s%s: missing dependency %s%s%s\n", C_RED, inst[i]->name,
                           C_OFF, C_YELLOW, inst[i]->depends[d].name, C_OFF);
                    broken++;
                }
            }
        if (!broken) printf("%sno broken dependencies%s\n", C_DIM, C_OFF);
        else printf("%s%d broken dependency edge(s)%s\n", C_RED, broken, C_OFF);
    } else {
        localdb_free_list(inst);
        die(2, "unknown query '%s' (try list, orphans or check)", sub);
    }
    localdb_free_list(inst);
    return 0;
}

int cmd_mark(const options *o, int argc, char **argv) {
    if (argc < 2) die(2, "mark needs 'explicit' or 'dependency' and a package name");
    int explicit = !strcmp(argv[0], "explicit");
    if (!explicit && strcmp(argv[0], "dependency"))
        die(2, "mark takes 'explicit' or 'dependency'");

    int rc = 0;
    for (int a = 1; a < argc; a++) {
        package **inst = NULL;
        int n = installed_packages(o, &inst);
        int pi = find_installed(inst, n, argv[a]);
        if (pi < 0) {
            fprintf(stderr, "%serror:%s '%s' is not installed\n", C_RED, C_OFF, argv[a]);
            localdb_free_list(inst);
            rc = 1;
            continue;
        }
        const package *p = inst[pi];
        if (localdb_set_explicit(o->root, p->name, p->version, explicit) != 0) {
            fprintf(stderr, "%serror:%s %s\n", C_RED, C_OFF, localdb_error());
            rc = 1;
        } else {
            printf("%s%s%s marked as %s\n", C_CYAN, p->name, C_OFF,
                   explicit ? "explicit" : "dependency");
        }
        localdb_free_list(inst);
    }
    return rc;
}

int cmd_deptest(const options *o, int argc, char **argv) {
    if (argc < 1) die(2, "deptest needs package names");
    repo *r = load_repo(o);
    int unsat = 0;
    for (int i = 0; i < argc; i++) {
        dep d;
        if (!dep_parse(&d, argv[i])) die(2, "cannot understand '%s'", argv[i]);
        const package *p = repo_newest(r, d.name, d.op, d.version);
        if (!p) {
            printf("%s%s: not found%s\n", C_RED, d.name, C_OFF);
            unsat++;
            dep_free(&d);
            continue;
        }
        printf("%s%s %s%s\n", C_CYAN, p->name, p->version, C_OFF);
        int missing = 0;
        for (int k = 0; k < p->ndepends; k++)
            if (!repo_has_provider(r, p->depends[k].name, p->depends[k].op,
                                   p->depends[k].version)) {
                printf("    %sunsatisfiable:%s %s\n", C_RED, C_OFF,
                       p->depends[k].name);
                missing++;
            }
        if (missing) unsat++;
        dep_free(&d);
    }
    repo_free(r);
    return unsat ? 1 : 0;
}