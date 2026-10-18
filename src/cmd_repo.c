/*
 * cmd_repo.c — Neptunium Planet: repository-facing commands.
 * =============================================
 *
 * sync, search, info, stats, solve, dump-cnf and doctor: everything that
 * reads or refreshes the repository databases and inspects what they offer.
 */
#define _GNU_SOURCE

#include "cli.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <archive.h>
#include <curl/curl.h>

#include "archrepo.h"
#include "encode.h"
#include "localdb.h"
#include "net.h"
#include "pkgfile.h"

int cmd_sync(const options *o) {
    if (net_global_init() != 0) die(1, "network initialisation failed");

    if (o->dry_run)
        printf("Synchronising repositories %s%s(dry run)%s\n",
               " ", C_DIM, C_OFF);
    else
        printf("Synchronising repositories\n");

    for (int i = 0; i < g_nrepos; i++) {
        const char *name = g_repos[i].name;
        size_t ulen = strlen(g_repos[i].url) + strlen(name) + 16;
        char *url = malloc(ulen);
        if (!url) die(1, "out of memory");
        /* Databases live at <mirror-dir>/<name>.db.tar.gz */
        snprintf(url, ulen, "%s%s.db.tar.gz", g_repos[i].url, name);

        char *path = NULL;
        FILE *fp = NULL;
        if (!o->dry_run) {
            path = db_cache_path(o->cache_dir, name);
            fp = fopen(path, "wb");
            if (!fp) die(1, "cannot write cache file %s (does the cache "
                            "directory exist? try 'mkdir -p %s')",
                            path, o->cache_dir);
        }

        double t0 = now_ms();
        if (o->verbose) printf("  fetching %s\n", url);

        int rc;
        if (fp) {
            /* Stream straight to the cache file, then parse from disk — the
             * file is the offline artefact we wanted anyway. */
            rc = net_fetch_to_file(url, fp, 0);
            fclose(fp);
        } else {
            /* Dry run: fetch to memory, parse, discard. */
            unsigned char *buf = NULL;
            size_t len = 0;
            rc = net_fetch(url, &buf, &len, 0);
            if (rc == 0) {
                archrepo_stats st;
                char err[256] = {0};
                repo *tmp = repo_new();
                rc = archrepo_load_mem(tmp, buf, len, g_repos[i].url, &st,
                                       err, sizeof err);
                if (rc == 0)
                    printf("  %-6s %s%d packages%s (%.0f ms, nothing written)\n",
                           name, C_GREEN, st.packages, C_OFF, now_ms() - t0);
                else
                    printf("  %-6s %sparse error:%s %s\n", name, C_RED, C_OFF,
                           err);
                repo_free(tmp);
                free(buf);
            }
        }

        if (rc != 0) {
            if (path) remove(path);   /* never leave a truncated db behind */
            const char *why = net_error();
            fprintf(stderr, "  %-6s %sfailed:%s %s\n", name, C_RED, C_OFF,
                    why && *why ? why : "network error");
            free(url); free(path);
            net_global_cleanup();
            return 1;
        }

        if (!o->dry_run) {
            /* Parse the freshly written file for the summary line. */
            archrepo_stats st;
            char err[256] = {0};
            repo *tmp = repo_new();
            if (archrepo_load_file(tmp, path, g_repos[i].url, &st, err,
                                   sizeof err) != 0)
                fprintf(stderr, "  %-6s %swritten but unparseable:%s %s\n",
                        name, C_RED, C_OFF, err);
            else
                printf("  %-6s %s%d packages%s  %s\n", name, C_GREEN,
                       st.packages, C_OFF, path);
            repo_free(tmp);
        }

        free(url);
        free(path);
    }

    net_global_cleanup();
    printf("%sDone.%s\n", C_DIM, C_OFF);
    return 0;
}

int cmd_search(const options *o, const char *term) {
    repo *r = load_repo(o);
    int   shown = 0;

    printf("%s%-24s %-16s %sDescription%s\n", C_BOLD, "Name", "Version", "", C_OFF);
    for (int i = 0; i < repo_count(r); i++) {
        const package *p = repo_get(r, i);
        int hit = strcasestr(p->name, term) != NULL
                  || (p->desc && strcasestr(p->desc, term) != NULL);
        if (!hit) continue;
        printf("%s%-24s%s %-16s %s\n", C_CYAN, p->name, C_OFF, p->version,
               p->desc ? p->desc : "");
        shown++;
    }
    if (shown == 0) printf("%s(no matches for '%s')%s\n", C_DIM, term, C_OFF);
    else            printf("%s%d match(es)%s\n", C_DIM, shown, C_OFF);
    repo_free(r);
    return shown ? 0 : 1;
}

int cmd_info(const options *o, const char *name) {
    repo *r = load_repo(o);
    const package *p = repo_newest(r, name, DEP_ANY, NULL);
    if (!p) {
        fprintf(stderr, "%serror:%s no package named '%s'\n", C_RED, C_OFF, name);
        repo_free(r);
        return 1;
    }

    printf("%s%s %s%s\n", C_BOLD, p->name, p->version, C_OFF);
    if (p->desc) printf("    %s\n", p->desc);
    printf("    repository url  : %s\n", p->origin ? p->origin : "-");
    printf("    filename        : %s\n", p->filename ? p->filename : "-");
    printf("    download size   : %s\n", p->csize > 0 ? hr_size(p->csize) : "-");
    printf("    installed size  : %s\n", p->isize > 0 ? hr_size(p->isize) : "-");
    printf("    sha256          : %s\n", p->sha256 ? p->sha256 : "-");

    /* How many versions of this name does the repo carry? */
    const package **all = malloc((size_t)(repo_count(r) ? repo_count(r) : 1)
                                 * sizeof *all);
    int n = all ? repo_lookup(r, name, DEP_ANY, NULL, all, repo_count(r)) : 0;
    if (n > 1) {
        printf("    versions        :");
        for (int i = 0; i < n; i++) printf(" %s", all[i]->version);
        printf("\n");
    }
    free(all);

    if (p->ndepends) {
        printf("    %sdepends on%s     :", C_DIM, C_OFF);
        for (int i = 0; i < p->ndepends; i++)
            printf(" %s%s%s", p->depends[i].name, dep_op_str(p->depends[i].op),
                   p->depends[i].version ? p->depends[i].version : "");
        printf("\n");
    }
    if (p->nprovides) {
        printf("    %sprovides%s       :", C_DIM, C_OFF);
        for (int i = 0; i < p->nprovides; i++)
            printf(" %s%s%s", p->provides[i].name,
                   dep_op_str(p->provides[i].op),
                   p->provides[i].version ? p->provides[i].version : "");
        printf("\n");
    }
    if (p->nconflicts) {
        printf("    %sconflicts with%s :", C_DIM, C_OFF);
        for (int i = 0; i < p->nconflicts; i++)
            printf(" %s", p->conflicts[i].name);
        printf("\n");
    }
    if (p->nreplaces) {
        printf("    %sreplaces%s        :", C_DIM, C_OFF);
        for (int i = 0; i < p->nreplaces; i++)
            printf(" %s", p->replaces[i].name);
        printf("\n");
    }
    repo_free(r);
    return 0;
}

int cmd_stats(const options *o) {
    repo *r = load_repo(o);
    long  total_csize = 0, total_isize = 0;
    int   with_provides = 0, multi_version = 0;
    int   n = repo_count(r);

    for (int i = 0; i < n; i++) {
        const package *p = repo_get(r, i);
        if (p->csize > 0) total_csize += p->csize;
        if (p->isize > 0) total_isize += p->isize;
        if (p->nprovides) with_provides++;
    }

    /* Multi-version names: sort the candidate names and count runs of length
     * > 1. (A per-name repo_lookup here would be O(n^2) on a 15k repo.) */
    int slots = n > 0 ? n : 1;          /* never let a negative count wrap */
    const char **names = malloc((size_t)slots * sizeof *names);
    if (names) {
        for (int i = 0; i < n; i++) names[i] = repo_get(r, i)->name;
        qsort(names, (size_t)n, sizeof *names, cmp_str_ptr);
        for (int i = 0; i < n; ) {
            int j = i + 1;
            while (j < n && strcmp(names[i], names[j]) == 0) j++;
            if (j - i > 1) multi_version++;
            i = j;
        }
        free(names);
    }

    printf("%sRepository%s\n", C_BOLD, C_OFF);
    printf("  candidates        : %d\n", n);
    printf("  multi-version     : %d\n", multi_version);
    printf("  providing virtuals: %d\n", with_provides);
    printf("  total download    : %s\n", hr_size((long)total_csize));
    printf("  total installed   : %s\n", hr_size((long)total_isize));
    repo_free(r);
    return 0;
}

int cmd_solve(const options *o, int argc, char **argv) {
    if (argc == 0) die(2, "solve needs at least one package name");
    repo *r = load_repo(o);

    int   nreq = 0;
    dep  *reqs = parse_requests(argc, argv, &nreq);

    /* Reject names the repos simply do not have: nothing to optimise. */
    for (int i = 0; i < nreq; i++) {
        if (!repo_has_provider(r, reqs[i].name, reqs[i].op, reqs[i].version)) {
            fprintf(stderr, "%serror:%s nothing in the repositories provides "
                            "'%s%s%s'\n", C_RED, C_OFF, reqs[i].name,
                    dep_op_str(reqs[i].op), reqs[i].version ? reqs[i].version : "");
            repo_free(r);
            return 1;
        }
    }

    encoder *e = enc_new(r);
    if (!e) die(1, "out of memory");
    for (int i = 0; i < nreq; i++) enc_request(e, reqs[i].name, reqs[i].op,
                                               reqs[i].version, 0);

    printf("Resolving");
    for (int i = 0; i < nreq; i++)
        printf(" %s%s%s%s%s", C_CYAN, reqs[i].name, dep_op_str(reqs[i].op),
               reqs[i].version ? reqs[i].version : "", C_OFF);
    printf("  %s(%d candidates)%s\n\n", C_DIM, enc_ncandidates(e), C_OFF);

    double t0 = now_ms();
    int    rc = enc_solve(e);
    double dt = now_ms() - t0;

    if (rc != MAXSAT_OPTIMAL) {
        /* Distinguish "a request is individually impossible" from "the
         * requests fight each other" — with a small request list this costs
         * only a handful of extra solves and turns a bare UNSAT into an
         * explanation the user can act on. */
        fprintf(stderr, "%sno consistent system exists%s for that request "
                        "set.\n", C_RED, C_OFF);
        if (nreq <= 6) {
            for (int i = 0; i < nreq; i++) {
                encoder *one = enc_new(r);
                enc_request(one, reqs[i].name, reqs[i].op, reqs[i].version, 0);
                if (enc_solve(one) != MAXSAT_OPTIMAL)
                    fprintf(stderr, "  %s%s%s is uninstallable on its own "
                            "(its dependency closure cannot be satisfied)\n",
                            C_YELLOW, reqs[i].name, C_OFF);
                enc_free(one);
            }
            if (nreq > 1)
                fprintf(stderr, "  %s(if every request is individually "
                        "installable, they conflict with each other)%s\n",
                        C_DIM, C_OFF);
        }
        enc_free(e);
        repo_free(r);
        return 1;
    }

    /* ---- print the optimal plan ------------------------------------------ */

    /* Chosen packages, sorted by name for a stable, readable table. */
    int nchosen = 0;
    for (int i = 0; i < enc_ncandidates(e); i++)
        if (enc_chosen(e, i)) nchosen++;

    const package **plan = malloc((size_t)(nchosen ? nchosen : 1) * sizeof *plan);
    char           *why  = malloc((size_t)(nchosen ? nchosen : 1));
    if (!plan || !why) die(1, "out of memory");
    for (int i = 0, k = 0; i < enc_ncandidates(e); i++) {
        if (!enc_chosen(e, i)) continue;
        const package *p = enc_candidate(e, i);
        plan[k] = p;
        /* "requested" if this exact candidate satisfies one of the user's
         * requests; otherwise it was pulled in as a dependency. */
        why[k] = 'D';
        for (int j = 0; j < nreq; j++)
            if (pkg_provides(p, reqs[j].name, reqs[j].op, reqs[j].version)) {
                why[k] = 'R';
                break;
            }
        k++;
    }
    /* insertion sort by name (n is small: a transaction, not a repo) */
    for (int i = 1; i < nchosen; i++) {
        const package *pv = plan[i]; char wv = why[i];
        int j = i - 1;
        while (j >= 0 && strcmp(plan[j]->name, pv->name) > 0) {
            plan[j + 1] = plan[j]; why[j + 1] = why[j]; j--;
        }
        plan[j + 1] = pv; why[j + 1] = wv;
    }

    printf("%s%-24s %-16s %10s %10s  %s%s\n", C_BOLD,
           "Package", "Version", "Download", "Installed", "Why", C_OFF);
    long dl = 0, inst = 0;
    for (int i = 0; i < nchosen; i++) {
        const package *p = plan[i];
        const char *tag = why[i] == 'R' ? "requested" : "dependency";
        const char *tagc = why[i] == 'R' ? C_GREEN : C_DIM;
        printf("%s%-24s%s %-16s %10s %10s  %s%s%s\n",
               C_CYAN, p->name, C_OFF, p->version,
               p->csize > 0 ? hr_size(p->csize) : "-",
               p->isize > 0 ? hr_size(p->isize) : "-",
               tagc, tag, C_OFF);
        if (p->csize > 0) dl += p->csize;
        if (p->isize > 0) inst += p->isize;
    }

    printf("\n  %d package(s), %s to download, %s installed\n",
           nchosen, hr_size(dl), hr_size(inst));
    printf("  %soptimal cost: %ld (broken soft weight)%s\n", C_DIM,
           enc_cost(e), C_OFF);

    if (o->solver_stats) {
        sat_stats st;
        enc_stats(e, &st);
        double per_dec = st.decisions ? (double)st.propagations / st.decisions
                                      : 0.0;
        printf("\n  %sSolver statistics%s\n", C_BOLD, C_OFF);
        printf("    BCP propagations  : %ld  (%.1f per decision)\n",
               st.propagations, per_dec);
        printf("    conflicts         : %ld\n", st.conflicts);
        printf("    decisions         : %ld\n", st.decisions);
        printf("    learned clauses   : %ld\n", st.learnts);
        printf("    restarts          : %ld\n", st.restarts);
        printf("    VSIDS decay       : %.3f\n", (double)st.vsids_decay);
        printf("    IHS core rounds   : %ld\n", enc_rounds(e));
        printf("    requests satisfied: %d/%d\n", enc_satisfied(e), nreq);
        printf("    total time        : %.2f ms\n", dt);
    }

    free(plan); free(why);
    enc_free(e);
    repo_free(r);
    for (int i = 0; i < nreq; i++) { free(reqs[i].name); free(reqs[i].version); }
    free(reqs);
    return 0;
}

int cmd_dump_cnf(const options *o, int argc, char **argv) {
    if (argc == 0) die(2, "dump-cnf needs at least one package name");
    repo *r = load_repo(o);

    int  nreq = 0;
    dep *reqs = parse_requests(argc, argv, &nreq);
    encoder *e = enc_new(r);
    if (!e) die(1, "out of memory");
    for (int i = 0; i < nreq; i++)
        if (!enc_request(e, reqs[i].name, reqs[i].op, reqs[i].version, 0))
            die(1, "nothing provides '%s'", reqs[i].name);

    FILE *fp = stdout;
    if (o->out_file) {
        fp = fopen(o->out_file, "w");
        if (!fp) die(1, "cannot write '%s'", o->out_file);
    }

    int n = enc_dump_cnf(e, fp);
    if (n < 0) die(1, "failed to build the instance");
    if (o->out_file) {
        fclose(fp);
        printf("%swrote %d clauses to %s%s\n", C_GREEN, n, o->out_file, C_OFF);
    }

    enc_free(e);
    repo_free(r);
    for (int i = 0; i < nreq; i++) { free(reqs[i].name); free(reqs[i].version); }
    free(reqs);
    return 0;
}

int cmd_doctor(const options *o) {
    int problems = 0;

    printf("%sPlanet doctor%s\n\n", C_BOLD, C_OFF);

    /* 1. libraries */
    printf("  %-26s %s\n", "libcurl", curl_version());
    printf("  %-26s %s\n", "libarchive", archive_version_string());

    /* 2. cache directory */
    if (access(o->cache_dir, W_OK) == 0)
        printf("  %-26s %sok%s (%s)\n", "cache directory", C_GREEN, C_OFF,
               o->cache_dir);
    else {
        printf("  %-26s %smissing/unwritable%s (%s)\n", "cache directory",
               C_RED, C_OFF, o->cache_dir);
        problems++;
    }

    /* 3. databases parse? */
    for (int i = 0; i < g_nrepos; i++) {
        char *path = db_cache_path(o->cache_dir, g_repos[i].name);
        if (access(path, R_OK) != 0) {
            printf("  %-26s %snot cached%s (%s)\n", g_repos[i].name, C_YELLOW,
                   C_OFF, path);
            problems++;
        } else {
            repo *tmp = repo_new();
            archrepo_stats st;
            char err[256] = {0};
            if (archrepo_load_file(tmp, path, g_repos[i].url, &st, err,
                                   sizeof err) == 0)
                printf("  %-26s %sok%s (%d packages, %d malformed)\n",
                       g_repos[i].name, C_GREEN, C_OFF, st.packages,
                       st.malformed);
            else {
                printf("  %-26s %scorrupt:%s %s\n", g_repos[i].name, C_RED,
                       C_OFF, err);
                problems++;
            }
            repo_free(tmp);
        }
        free(path);
    }

    /* 4. target root */
    if (access(o->root, W_OK) != 0) {
        printf("  %-26s %snot writable%s (%s — use sudo for system changes)\n",
               "target root", C_YELLOW, C_OFF, o->root);
    } else {
        printf("  %-26s %sok%s (%s)\n", "target root", C_GREEN, C_OFF, o->root);
    }

    printf("\n  %s\n", problems ? "problems found — see above"
                                : "everything looks healthy");
    return problems ? 1 : 0;
}
