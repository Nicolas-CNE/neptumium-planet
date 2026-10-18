/*
 * main.c — the `planet` command-line front end.
 * ========================================
 *
 * This file does two things only: parse the command line into an `options`
 * struct, then dispatch to the named command. Everything a command needs is
 * declared in cli.h and implemented in one of:
 *
 *   cli_util.c   helpers and repository loading
 *   cli_txn.c    the transaction machinery
 *   cmd_repo.c   sync, search, info, stats, solve, dump-cnf, doctor
 *   cmd_admin.c  lock, profile, cache
 *   cmd_query.c  query, files, owns, why, mark, deptest
 *
 * GLOBAL OPTIONS
 *   Options may appear anywhere on the command line, so both `planet solve
 *   git -S` and `planet -S solve git` work.
 *
 * REQUEST SYNTAX
 *   Package arguments accept the same constraint syntax as the repositories
 *   themselves, so `planet install 'vim>=9' 'gcc=15'` pins versions exactly the
 *   way a depends line would.
 */
#define _GNU_SOURCE  /* strdup() under -std=c11 */

#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hooks.h"

#define PLANET_VERSION "0.1.0"

static void usage(FILE *fp) {
    fprintf(fp,
"%sNeptunium Planet%s %s — dependency resolution you can prove optimal\n"
"\n"
"%sUsage:%s planet [options] <command> [arguments]\n"
"\n"
"%sCommands:%s\n"
"  sync                 download the repository databases into the cache\n"
"  install <name...>    download, verify, extract and record packages\n"
"  remove <name...>     remove packages and their files\n"
"  upgrade [name...]    move installed packages to the newest feasible set\n"
"  update [name...]     synonym for upgrade\n"
"  autoremove           remove packages nothing depends on\n"
"  mark explicit|dependency <name...>  set how a package was installed\n"
"  profile save|list|remove <name>     snapshot the installed set\n"
"  rollback [profile]   re-solve the system to a saved profile\n"
"  lock                 write the installed system state to a lock file\n"
"  query [list|orphans|check]          inspect the installed system\n"
"  files <name>         list the files a package owns\n"
"  owns <path...>       which package owns a path\n"
"  why <name>           why a package is installed\n"
"  deptest <name...>    check dependencies are satisfiable\n"
"  cache [list|clean]   inspect or clear downloaded payloads\n"
"  search <term>        find packages by name or description\n"
"  info <name>          show a package in full detail\n"
"  stats                summarise the loaded repository\n"
"  solve <name...>      compute the optimal package set for a request\n"
"                       (accepts constraints: 'vim>=9' 'gcc=15.2')\n"
"  dump-cnf <name...>   export the MaxSAT instance (WCNF) for a request\n"
"  doctor               check cache, databases and environment\n"
"  help | version       this text / the version number\n"
"\n"
"%sOptions:%s\n"
"  -r, --root <dir>      operate on <dir> as the target system root\n"
"      --installroot <dir>  synonym for --root (staged installs)\n"
"      --sysroot <dir>     shift config and hook lookup as well\n"
"      --no-hooks          skip ALPM/.INSTALL hooks\n"
"  -c, --cache-dir <dir> where databases live (default: ~/.cache/planet)\n"
"      --repo <name=URL> add or override a repository (repeatable)\n"
"      --demo            use the built-in example repository (no network)\n"
"  -n, --dry-run         report what would happen, change nothing\n"
"  -y, --yes             assume yes; skip confirmation prompts\n"
"  -S, --solver-stats    print solver statistics (BCP rate, learned clauses,\n"
"                        VSIDS decay, timing)\n"
"  -o, --output <file>   write output to <file> (dump-cnf)\n"
"  -v, --verbose         explain each step\n"
"  -h, --help            this text\n"
"  -V, --version         print the version\n"
"\n"
"%sExamples:%s\n"
"  planet sync\n"
"  planet search editor\n"
"  planet solve git vim            # the optimal set for both, exactly\n"
"  planet solve git -S             # ...with solver statistics\n"
"  planet dump-cnf git -o git.wcnf # reproduce the instance elsewhere\n",
        C_BOLD, C_OFF, PLANET_VERSION,
        C_BOLD, C_OFF, C_BOLD, C_OFF, C_BOLD, C_OFF, C_BOLD, C_OFF);
}

static char *default_cache_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        size_t n = strlen(xdg) + 16;
        char *p = malloc(n);
        if (p) snprintf(p, n, "%s/planet", xdg);
        return p;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        size_t n = strlen(home) + 24;
        char *p = malloc(n);
        if (p) snprintf(p, n, "%s/.cache/planet", home);
        return p;
    }
    return strdup(".planet-cache");
}

int main(int argc, char **argv) {
    options o = { .root = "/", .sysroot = NULL, .cache_dir = NULL,
                  .out_file = NULL, .dry_run = 0, .verbose = 0,
                  .solver_stats = 0, .yes = 0, .demo = 0 };

    /* Split the command line into global options and the command itself, so
     * options may appear before or after the command name. */
    char *rest[128];
    int   nrest = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-h") || !strcmp(a, "--help"))    { usage(stdout); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("planet %s (Neptunium)\n", PLANET_VERSION); return 0; }
        else if (!strcmp(a, "-n") || !strcmp(a, "--dry-run")) o.dry_run = 1;
        else if (!strcmp(a, "-y") || !strcmp(a, "--yes"))      o.yes = 1;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) o.verbose = 1;
        else if (!strcmp(a, "-S") || !strcmp(a, "--solver-stats")) o.solver_stats = 1;
        else if (!strcmp(a, "--demo"))                        o.demo = 1;
        else if (!strcmp(a, "--no-hooks"))                    hooks_set_enabled(0);
        else if (!strcmp(a, "-r") || !strcmp(a, "--root") ||
                 !strcmp(a, "--installroot")) {
            /* --installroot is pacman's synonym for --root: the directory that
             * becomes "/" for this transaction (a staged install, a test
             * rootfs, or a mount point). */
            if (++i >= argc) die(2, "%s needs a directory", a);
            o.root = argv[i];
        } else if (!strcmp(a, "--sysroot")) {
            /* --sysroot shifts both the target root AND where planet looks
             * for its own configuration and hooks, as pacman does. */
            if (++i >= argc) die(2, "--sysroot needs a directory");
            o.root = argv[i];
            o.sysroot = argv[i];
        } else if (!strcmp(a, "-c") || !strcmp(a, "--cache-dir")) {
            if (++i >= argc) die(2, "%s needs a directory", a);
            o.cache_dir = argv[i];
        } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
            if (++i >= argc) die(2, "%s needs a file name", a);
            o.out_file = argv[i];
        } else if (!strcmp(a, "--repo")) {
            if (++i >= argc) die(2, "--repo needs name=URL");
            char *eq = strchr(argv[i], '=');
            if (!eq) die(2, "--repo wants the form name=URL");
            *eq = '\0';
            /* Replace a repository of the same name, else append. */
            int replaced = 0;
            for (int k = 0; k < g_nrepos; k++)
                if (!strcmp(g_repos[k].name, argv[i])) {
                    free(g_repos[k].url);
                    g_repos[k].url = strdup(eq + 1);
                    replaced = 1;
                    break;
                }
            if (!replaced) repo_src_add(argv[i], eq + 1);
            *eq = '=';
        } else {
            if (nrest == 128) die(2, "too many arguments");
            rest[nrest++] = argv[i];
        }
    }

    /* Colour only on a real terminal, and honour NO_COLOR. */
    g_color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
    g_yes = o.yes;

    /* own_cache_dir is malloc'd when we chose it, borrowed when the user did. */
    char *own_cache_dir = NULL;
    if (!o.cache_dir) { own_cache_dir = default_cache_dir(); o.cache_dir = own_cache_dir; }
    if (g_nrepos == 0) repos_default();

    if (nrest == 0) { usage(stdout); free(own_cache_dir); return 0; }
    const char *cmd = rest[0];
    int nargs = nrest - 1;
    char **args = rest + 1;

    int rc = 0;
    if      (!strcmp(cmd, "help"))       usage(stdout);
    else if (!strcmp(cmd, "version"))    printf("planet %s (Neptunium)\n", PLANET_VERSION);
    else if (!strcmp(cmd, "sync"))       rc = cmd_sync(&o);
    else if (!strcmp(cmd, "search"))     { if (nargs < 1) die(2, "search needs a term");
                                           rc = cmd_search(&o, args[0]); }
    else if (!strcmp(cmd, "info"))       { if (nargs < 1) die(2, "info needs a package name");
                                           rc = cmd_info(&o, args[0]); }
    else if (!strcmp(cmd, "stats"))      rc = cmd_stats(&o);
    else if (!strcmp(cmd, "solve"))      rc = cmd_solve(&o, nargs, args);
    else if (!strcmp(cmd, "dump-cnf"))   rc = cmd_dump_cnf(&o, nargs, args);
    else if (!strcmp(cmd, "doctor"))     rc = cmd_doctor(&o);
    else if (!strcmp(cmd, "install"))    rc = cmd_install(&o, nargs, args);
    else if (!strcmp(cmd, "remove"))     rc = cmd_remove(&o, nargs, args);
    else if (!strcmp(cmd, "upgrade"))    rc = cmd_upgrade(&o, nargs, args);
    else if (!strcmp(cmd, "update"))     rc = cmd_upgrade(&o, nargs, args);
    else if (!strcmp(cmd, "autoremove")) rc = cmd_autoremove(&o, nargs, args);
    else if (!strcmp(cmd, "rollback"))   rc = cmd_rollback(&o, nargs, args);
    else if (!strcmp(cmd, "query"))      rc = cmd_query(&o, nargs, args);
    else if (!strcmp(cmd, "files"))      rc = cmd_files(&o, nargs, args);
    else if (!strcmp(cmd, "owns"))       rc = cmd_owns(&o, nargs, args);
    else if (!strcmp(cmd, "why"))        rc = cmd_why(&o, nargs, args);
    else if (!strcmp(cmd, "mark"))       rc = cmd_mark(&o, nargs, args);
    else if (!strcmp(cmd, "deptest"))    rc = cmd_deptest(&o, nargs, args);
    else if (!strcmp(cmd, "lock"))       rc = cmd_lock(&o, nargs, args);
    else if (!strcmp(cmd, "profile"))    rc = cmd_profile(&o, nargs, args);
    else if (!strcmp(cmd, "cache"))      rc = cmd_cache(&o, nargs, args);
    else die(2, "unknown command '%s' (try 'planet help')", cmd);

    free(own_cache_dir);
    return rc;
}
