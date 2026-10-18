/*
 * cli_txn.c — Neptunium Planet: the transaction machinery.
 * =============================================
 *
 * One path serves every mutating command:
 *
 *   1. load the repositories and merge the installed system state into them;
 *   2. build a request list — everything installed that must remain becomes a
 *      hard requirement, new wishes are soft ones, removals are excluded;
 *   3. let the MaxSAT solver compute the optimal target state;
 *   4. diff old against new to get installs and removals;
 *   5. confirm, then apply, running per-package and repository hooks around
 *      each step.
 *
 * install, remove, upgrade, autoremove and rollback differ only in how they
 * build that request list, which is why they share this machinery entirely.
 */
#define _GNU_SOURCE

#include "cli.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "encode.h"
#include "hooks.h"
#include "localdb.h"
#include "pkgfile.h"
#include "state.h"

typedef struct {
    dep *reqs;      /* soft wishes (install)                          */
    int  n;
    dep *keep;      /* hard requirements (must stay installed)         */
    int  nkeep;
} request_list;

/* Build the request lists for a transaction: `add` packages (soft install
 * wishes) minus `drop` packages (hard removals), while pinning every
 * currently-installed package that is not being removed as a HARD keep. */

request_list build_transaction(const char *root,
                                      const dep *add, int nadd,
                                      const dep *drop, int ndrop) {
    int ncap = 64, kcap = 64;
    request_list rl = { malloc((size_t)ncap * sizeof *rl.reqs), 0,
                        malloc((size_t)kcap * sizeof *rl.keep), 0 };
    if (!rl.reqs || !rl.keep) {
        free(rl.reqs); free(rl.keep);
        request_list empty = { NULL, 0, NULL, 0 };
        return empty;
    }

    /* Hard keeps: every installed package stays, unless it is being removed. */
    int ninst = 0;
    package **inst = localdb_list(root, &ninst);
    for (int i = 0; inst && i < ninst; i++) {
        int dropping = 0;
        for (int k = 0; k < ndrop; k++)
            if (strcmp(inst[i]->name, drop[k].name) == 0) { dropping = 1; break; }
        if (dropping) continue;
        if (rl.nkeep == kcap) {
            kcap *= 2;
            dep *nk = realloc(rl.keep, (size_t)kcap * sizeof *nk);
            if (!nk) { free(rl.reqs); free(rl.keep); request_list e2 = { NULL, 0, NULL, 0 }; return e2; }
            rl.keep = nk;
        }
        char *spec = malloc(strlen(inst[i]->name) + strlen(inst[i]->version) + 2);
        if (spec) {
            sprintf(spec, "%s=%s", inst[i]->name, inst[i]->version);
            dep d;
            if (dep_parse(&d, spec)) rl.keep[rl.nkeep++] = d;
            free(spec);
        }
    }
    localdb_free_list(inst);

    /* Soft adds: the explicitly requested packages (already parsed deps). */
    for (int i = 0; i < nadd; i++) {
        if (rl.n == ncap) {
            ncap *= 2;
            dep *nr = realloc(rl.reqs, (size_t)ncap * sizeof *nr);
            if (!nr) { free(rl.reqs); free(rl.keep); request_list e2 = { NULL, 0, NULL, 0 }; return e2; }
            rl.reqs = nr;
        }
        rl.reqs[rl.n] = (dep){ strdup(add[i].name), add[i].op,
                               add[i].version ? strdup(add[i].version) : NULL };
        rl.n++;
    }
    return rl;
}

void request_list_free(request_list *rl) {
    for (int i = 0; i < rl->n; i++)     dep_free(&rl->reqs[i]);
    for (int i = 0; i < rl->nkeep; i++) dep_free(&rl->keep[i]);
    free(rl->reqs);
    free(rl->keep);
}

dep *parse_requests(int argc, char **argv, int *n_out) {
    dep *d = calloc((size_t)argc, sizeof *d);
    if (!d) die(1, "out of memory");
    for (int i = 0; i < argc; i++) {
        if (!dep_parse(&d[i], argv[i]))
            die(2, "cannot understand request '%s' (try 'name', 'name>=1.2', "
                   "'name=1.2-1')", argv[i]);
    }
    *n_out = argc;
    return d;
}

int load_system_state(repo *r, const char *root) {
    int n = 0;
    package **inst = localdb_list(root, &n);
    if (!inst) return -1;
    for (int i = 0; i < n; i++) {
        /* If the repository already offers this exact name+version, mark that
         * candidate installed instead of adding a duplicate. Two candidates
         * for one name+version would share an AMO group, so the solver could
         * pick the repo copy and "remove" the installed one — showing a bogus
         * reinstall of a package that is already on disk. */
        const package *existing = repo_find(r, inst[i]->name, inst[i]->version);
        if (existing) {
            ((package *)existing)->installed = 1;
            pkg_free(inst[i]);   /* redundant copy: the repo owns `existing` */
            continue;
        }
        inst[i]->installed = 1;
        /* An installed package that the repos no longer offer: no download
         * origin, so it is treated as already on disk. */
        free(inst[i]->origin);
        inst[i]->origin = NULL;
        if (!repo_add(r, inst[i])) {
            localdb_free_list(inst);
            return -1;
        }
    }
    /* Added packages are now owned by r; free the array but not the packages. */
    free(inst);
    return n;
}

int do_install_one(const options *o, const package *p) {
    char **files = NULL;
    int    nfiles = 0;

    if (p->origin && p->filename) {
        char *path = pkgfile_fetch(p);
        if (!path) {
            fprintf(stderr, "%serror:%s %s: %s\n", C_RED, C_OFF, p->name,
                    pkgfile_error());
            return -1;
        }
        if (pkgfile_extract(path, o->root, &files, &nfiles) != 0) {
            fprintf(stderr, "%serror:%s extract %s: %s\n", C_RED, C_OFF, p->name,
                    pkgfile_error());
            free(path);
            return -1;
        }
        free(path);
    }

    /* Record the package together with the files it owns, so removal can
     * delete exactly those paths. */
    if (localdb_add(o->root, p, (const char *const *)files, nfiles) != 0) {
        fprintf(stderr, "%serror:%s could not record %s in local db: %s\n",
                C_YELLOW, C_OFF, p->name, localdb_error());
        pkgfile_free_list(files, nfiles);
        return -1;
    }
    if (o->verbose)
        printf("    %srecorded %d file(s) for %s%s\n", C_DIM, nfiles, p->name, C_OFF);
    pkgfile_free_list(files, nfiles);
    return 0;
}

int do_remove_one(const options *o, const package *p) {
    char *base = localdb_path(o->root);
    if (!base) return -1;
    size_t n = strlen(base) + strlen(p->name) + strlen(p->version) + 16;
    char *files = malloc(n);
    snprintf(files, n, "%s/%s-%s/files", base, p->name, p->version);

    int removed = 0, failed = 0;
    FILE *f = fopen(files, "r");
    if (f) {
        char line[4096];
        /* Collect first, then delete in reverse: the archive stores
         * directories before their contents, so reverse order empties them. */
        char **paths = NULL;
        int np = 0, cap = 0;
        while (fgets(line, sizeof line, f)) {
            size_t len = strlen(line);
            while (len && (line[len-1] == '\n' || line[len-1] == '\r'))
                line[--len] = '\0';
            if (len == 0) continue;
            if (np == cap) {
                cap = cap ? cap * 2 : 64;
                char **npaths = realloc(paths, (size_t)cap * sizeof *npaths);
                if (!npaths) { failed = 1; break; }
                paths = npaths;
            }
            paths[np] = strdup(line);
            if (!paths[np]) { failed = 1; break; }
            np++;
        }
        fclose(f);

        for (int i = np - 1; i >= 0; i--) {
            size_t fl = strlen(o->root) + strlen(paths[i]) + 2;
            char *full = malloc(fl);
            if (!full) { failed = 1; break; }
            snprintf(full, fl, "%s/%s", o->root, paths[i]);
            struct stat st;
            if (lstat(full, &st) == 0) {
                /* Directories need rmdir; everything else unlink handles
                 * (files, symlinks). A non-empty directory means another
                 * package still owns something inside — leave it alone. */
                int rc = S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
                if (rc != 0 && errno != ENOENT) {
                    if (o->verbose)
                        fprintf(stderr, "%swarning:%s could not delete %s%s\n",
                                C_YELLOW, C_OFF, paths[i], C_OFF);
                } else {
                    removed++;
                }
            }
            free(full);
        }
        for (int i = 0; i < np; i++) free(paths[i]);
        free(paths);
    }
    free(files);
    free(base);

    if (localdb_remove(o->root, p->name, p->version) != 0) {
        fprintf(stderr, "%serror:%s %s\n", C_RED, C_OFF, localdb_error());
        return -1;
    }
    if (o->verbose)
        printf("    %sdeleted %d path(s)%s\n", C_DIM, removed, C_OFF);
    return failed ? -1 : 0;
}

void classify_upgrades(const package **pkgs, int npkgs,
                              const options *o, unsigned char *is_upgrade) {
    int ninst = 0;
    package **inst = localdb_list(o->root, &ninst);
    for (int i = 0; i < npkgs; i++) {
        is_upgrade[i] = 0;
        for (int k = 0; inst && k < ninst; k++)
            if (strcmp(pkgs[i]->name, inst[k]->name) == 0) { is_upgrade[i] = 1; break; }
    }
    localdb_free_list(inst);
}

int run_pkg_hook(const options *o, const package *p, install_phase phase) {
    if (!hooks_enabled()) return 0;
    if (hooks_run_install_script(p, o->root, phase, NULL) == 0) return 0;
    fprintf(stderr, "%shook error:%s %s: %s\n", C_RED, C_OFF,
            install_phase_func(phase), hooks_error());
    return -1;
}

int run_transaction(const options *o, const char *label,
                           const dep *add, int nadd,
                           const dep *drop, int ndrop) {
    /* Keep the payload cache beside the database cache the user chose. */
    pkgfile_cache_dir_set(o->cache_dir);

    /* Serialise mutating transactions: two concurrent runs against one root
     * would interleave their local-database writes and could produce a system
     * that neither intended. Released automatically at exit. */
    if (lock_acquire(o->root) != 0) {
        fprintf(stderr, "%serror:%s %s\n", C_RED, C_OFF, state_error());
        return 1;
    }

    repo *r = load_repo(o);
    if (!r) die(1, "cannot load repositories");
    load_system_state(r, o->root);

    request_list rl = build_transaction(o->root, add, nadd, drop, ndrop);
    if (!rl.reqs) die(1, "out of memory");

    encoder *e = enc_new(r);
    if (!e) { request_list_free(&rl); repo_free(r); die(1, "out of memory"); }
    /* Hard keeps first: packages that must stay installed. */
    for (int i = 0; i < rl.nkeep; i++)
        if (!enc_require(e, rl.keep[i].name, rl.keep[i].op, rl.keep[i].version))
            die(1, "cannot keep '%s': nothing in the repositories provides it",
                rl.keep[i].name);
    /* Soft adds: install wishes the solver may break only at a cost. */
    for (int i = 0; i < rl.n; i++)
        if (!enc_request(e, rl.reqs[i].name, rl.reqs[i].op, rl.reqs[i].version, 0))
            die(1, "nothing provides '%s'", rl.reqs[i].name);

    double t0 = now_ms();
    int rc = enc_solve(e);
    double dt = now_ms() - t0;
    if (rc != MAXSAT_OPTIMAL) {
        fprintf(stderr, "%serror:%s cannot satisfy that request\n", C_RED, C_OFF);
        request_list_free(&rl); enc_free(e); repo_free(r);
        return 1;
    }

    /* Diff: chosen-and-not-installed = to install; installed-and-not-chosen =
     * to remove. */
    int nci = 0, ncr = 0;
    int  ncand = enc_ncandidates(e);
    int  nrepo = repo_count(r);
    const package **to_install = malloc((size_t)(ncand ? ncand : 1) * sizeof *to_install);
    const package **to_remove  = malloc((size_t)(nrepo ? nrepo : 1) * sizeof *to_remove);
    /* Installs come from the candidate set (chosen and not already present). */
    for (int i = 0; i < ncand; i++) {
        const package *p = enc_candidate(e, i);
        if (enc_chosen(e, i) && !p->installed) to_install[nci++] = p;
    }
    /* Removals are decided over the INSTALLED set, not the candidate set. A
     * package that nothing depends on is outside the encoding closure — no
     * request reaches it, so it never becomes a candidate — yet `planet remove`
     * must still be able to take it off the system. Asking "is this installed
     * package absent from the optimal set?" covers both cases uniformly. */
    for (int i = 0; i < nrepo; i++) {
        const package *p = repo_get(r, i);
        if (p->installed && !enc_chosen_pkg(e, p)) to_remove[ncr++] = p;
    }

    if (!confirm_transaction(label, o->dry_run, to_install, nci, to_remove, ncr)) {
        if (o->dry_run)
            printf("%sdry run: nothing was changed%s\n", C_DIM, C_OFF);
        else
            fprintf(stderr, "aborted\n");
        free(to_install); free(to_remove);
        request_list_free(&rl); enc_free(e); repo_free(r);
        return 0;
    }


    int errs = 0;
    unsigned char *upg = calloc((size_t)(nci ? nci : 1), 1);
    if (!upg) { free(to_install); free(to_remove); request_list_free(&rl); enc_free(e); repo_free(r); die(1, "out of memory"); }
    if (nci) classify_upgrades(to_install, nci, o, upg);

    /* Repository hooks around the whole transaction. */
    if (nci) hooks_run_repo(o->root, "Install", HP_PRE_TRANSACTION,
                            to_install, nci);
    if (ncr) hooks_run_repo(o->root, "Remove", HP_PRE_TRANSACTION,
                            to_remove, ncr);

    for (int i = 0; i < nci; i++) {
        printf("  %sinstalling%s %s %s\n", C_GREEN, C_OFF,
               to_install[i]->name, to_install[i]->version);
        /* pre_install for a new package, pre_upgrade when replacing one. */
        run_pkg_hook(o, to_install[i],
                     upg[i] ? IP_PRE_UPGRADE : IP_PRE_INSTALL);
        hooks_run_repo(o->root, "Install",
                       upg[i] ? HP_PRE_UPGRADE : HP_PRE_INSTALL,
                       &to_install[i], 1);
        if (do_install_one(o, to_install[i]) != 0) { errs++; continue; }
        /* Record why the package is here: anything the user named explicitly
         * is an EXPLICIT install, everything pulled in to satisfy it is a
         * dependency. `autoremove` removes only what is not reachable from an
         * explicit package, so this distinction has to be persisted here. */
        for (int a = 0; a < nadd; a++)
            if (pkg_provides(to_install[i], add[a].name, add[a].op, add[a].version)) {
                localdb_set_explicit(o->root, to_install[i]->name,
                                     to_install[i]->version, 1);
                break;
            }
        run_pkg_hook(o, to_install[i],
                     upg[i] ? IP_POST_UPGRADE : IP_POST_INSTALL);
        hooks_run_repo(o->root, "Install",
                       upg[i] ? HP_POST_UPGRADE : HP_POST_INSTALL,
                       &to_install[i], 1);
    }
    for (int i = 0; i < ncr; i++) {
        printf("  %sremoving%s  %s %s\n", C_RED, C_OFF,
               to_remove[i]->name, to_remove[i]->version);
        run_pkg_hook(o, to_remove[i], IP_PRE_REMOVE);
        hooks_run_repo(o->root, "Remove", HP_PRE_REMOVE, &to_remove[i], 1);
        if (do_remove_one(o, to_remove[i]) != 0) { errs++; continue; }
        run_pkg_hook(o, to_remove[i], IP_POST_REMOVE);
        hooks_run_repo(o->root, "Remove", HP_POST_REMOVE, &to_remove[i], 1);
    }
    free(upg);

    if (nci) hooks_run_repo(o->root, "Install", HP_POST_TRANSACTION,
                            to_install, nci);
    if (ncr) hooks_run_repo(o->root, "Remove", HP_POST_TRANSACTION,
                            to_remove, ncr);

    if (o->solver_stats) {
        sat_stats st;
        enc_stats(e, &st);
        printf("\n  %sSolver%s: %ld conflicts, %ld propagations (%.1f/decision), "
               "%ld learned, VSIDS %.3f, %.2f ms\n", C_BOLD, C_OFF,
               st.conflicts, st.propagations,
               st.decisions ? (double)st.propagations / st.decisions : 0.0,
               st.learnts_total, (double)st.vsids_decay, dt);
        printf("    %sencoding%s: %d vars, %d hard clauses, %d softs; "
               "%ld oracle calls over %ld IHS rounds\n",
               C_DIM, C_OFF, st.nvars, st.nclauses, enc_nsofts(e),
               st.solve_calls, enc_rounds(e));
        printf("    %slearned%s: %ld total (%ld live at exit)\n",
               C_DIM, C_OFF, st.learnts_total, st.learnts);
    }

    if (errs)
        fprintf(stderr, "%s%d package(s) failed to apply%s\n", C_RED, errs, C_OFF);

    free(to_install); free(to_remove);
    request_list_free(&rl); enc_free(e); repo_free(r);
    return errs ? 1 : 0;
}

int cmd_install(const options *o, int argc, char **argv) {
    if (argc == 0) die(2, "install needs at least one package name");
    dep *reqs = parse_requests(argc, argv, &argc);
    int rc = run_transaction(o, "Install", reqs, argc, NULL, 0);
    for (int i = 0; i < argc; i++) dep_free(&reqs[i]);
    free(reqs);
    return rc;
}

int cmd_remove(const options *o, int argc, char **argv) {
    if (argc == 0) die(2, "remove needs at least one package name");
    dep *reqs = parse_requests(argc, argv, &argc);
    int rc = run_transaction(o, "Remove", NULL, 0, reqs, argc);
    for (int i = 0; i < argc; i++) dep_free(&reqs[i]);
    free(reqs);
    return rc;
}

int cmd_upgrade(const options *o, int argc, char **argv) {
    /* Upgrade = keep everything installed (at >= current version), resolve to
     * the newest feasible set, apply the delta. Extra args pin versions. */
    dep *pins = parse_requests(argc, argv, &argc);
    int rc = run_transaction(o, "Upgrade", pins, argc, NULL, 0);
    for (int i = 0; i < argc; i++) dep_free(&pins[i]);
    free(pins);
    return rc;
}

int cmd_autoremove(const options *o, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Orphans: installed packages that nothing else requires. Solve for the
     * system with all non-orphan installed packages kept, and remove the rest. */
    repo *r = load_repo(o);
    if (!r) die(1, "cannot load repositories");
    load_system_state(r, o->root);

    int ninst = 0;
    package **inst = localdb_list(o->root, &ninst);
    if (!inst) die(1, "cannot read local db");

    /* Reachability from the EXPLICIT packages: everything the user asked for,
     * plus everything transitively required to keep that working, stays.
     *
     * The explicit set must seed the search. Deriving the keep-list only from
     * "some other installed package depends on this" has no starting point for
     * a package nothing depends on — which is precisely what an explicitly
     * installed leaf is — so `autoremove` would delete the user's own
     * selections along with the true orphans. This mirrors pacman: explicit
     * marks are the roots, and reachability from them defines the rest. */
    int ncap = 64, nkeep = 0;
    dep *keep = malloc((size_t)ncap * sizeof *keep);
    unsigned char *kept = calloc((size_t)(ninst ? ninst : 1), 1);

    /* Seeds: explicitly installed packages. */
    for (int i = 0; i < ninst; i++) {
        if (!inst[i]->explicit_installed) continue;
        kept[i] = 1;
        if (nkeep == ncap) { ncap *= 2; keep = realloc(keep, (size_t)ncap * sizeof *keep); }
        char *spec = malloc(strlen(inst[i]->name) + strlen(inst[i]->version) + 2);
        sprintf(spec, "%s=%s", inst[i]->name, inst[i]->version);
        dep d;
        if (dep_parse(&d, spec)) keep[nkeep++] = d;
        free(spec);
    }

    /* Expand transitively: anything a kept package needs is kept too. */
    for (int i = 0; i < ninst; i++) {
        if (!kept[i]) continue;
        for (int j = 0; j < ninst; j++) {
            if (kept[j]) continue;
            int required = 0;
            for (int d = 0; d < inst[i]->ndepends; d++)
                if (pkg_provides(inst[j], inst[i]->depends[d].name,
                                 inst[i]->depends[d].op,
                                 inst[i]->depends[d].version)) {
                    required = 1; break;
                }
            if (!required) continue;
            kept[j] = 1;
            if (nkeep == ncap) { ncap *= 2; keep = realloc(keep, (size_t)ncap * sizeof *keep); }
            char *spec = malloc(strlen(inst[j]->name) + strlen(inst[j]->version) + 2);
            sprintf(spec, "%s=%s", inst[j]->name, inst[j]->version);
            dep d;
            if (dep_parse(&d, spec)) keep[nkeep++] = d;
            free(spec);
        }
    }
    free(kept);

    encoder *e = enc_new(r);
    if (!e) { free(keep); localdb_free_list(inst); repo_free(r); die(1, "out of memory"); }
    for (int i = 0; i < nkeep; i++)
        if (!enc_require(e, keep[i].name, keep[i].op, keep[i].version))
            die(1, "internal: request '%s' not satisfiable", keep[i].name);
    int rc = enc_solve(e);
    if (rc != MAXSAT_OPTIMAL) {
        fprintf(stderr, "%serror:%s cannot resolve the system\n", C_RED, C_OFF);
        for (int i = 0; i < nkeep; i++) dep_free(&keep[i]);
        free(keep); localdb_free_list(inst); enc_free(e); repo_free(r);
        return 1;
    }

    /* Remove installed packages the solver did not keep. */
    int errs = 0;
    /* Collect the orphans first, then honour the dry-run flag. The removals
     * used to be applied inline, which meant `-n` printed the plan and then
     * deleted the packages anyway. */
    const package **orphans = malloc((size_t)(ninst ? ninst : 1) * sizeof *orphans);
    int norphans = 0;
    for (int i = 0; i < ninst; i++) {
        if (enc_chosen_pkg(e, inst[i])) continue;   /* still in the optimal set */
        printf("  %sremoving%s  %s %s (orphan)\n", C_RED, C_OFF,
               inst[i]->name, inst[i]->version);
        orphans[norphans++] = inst[i];
    }
    if (norphans == 0) printf("  %sno orphans found%s\n", C_DIM, C_OFF);

    if (o->dry_run) {
        printf("%sdry run: nothing was changed%s\n", C_DIM, C_OFF);
    } else {
        for (int i = 0; i < norphans; i++)
            if (do_remove_one(o, orphans[i]) != 0) errs++;
    }

    free(orphans);
    for (int i = 0; i < nkeep; i++) dep_free(&keep[i]);
    free(keep); localdb_free_list(inst); enc_free(e); repo_free(r);
    return errs ? 1 : 0;
}
