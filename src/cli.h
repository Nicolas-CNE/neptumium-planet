/*
 * cli.h — Neptunium Planet: shared CLI plumbing.
 * =============================================
 *
 * `main.c` only parses the command line and dispatches. The work lives in
 * focused modules that share the definitions declared here:
 *
 *   cli_util.c   helpers (errors, sizes, prompts) and repository loading
 *   cli_txn.c     the transaction machinery shared by every mutating command
 *   cmd_repo.c   sync, search, info, stats, solve, dump-cnf, doctor
 *   cmd_admin.c   lock, profile, cache
 *   cmd_query.c  query, files, owns, why, mark, deptest
 *
 * Declaring the shared surface in one header avoids the alternative failure
 * mode: every command growing its own idea of what a transaction is.
 */
#ifndef NEPTUMIUM_CLI_H
#define NEPTUMIUM_CLI_H

#include <stdio.h>

#include "pkg.h"

/* ---- global options ------------------------------------------------------ */

typedef struct {
    const char *root;       /* target system root, default "/"              */
    const char *sysroot;    /* --sysroot: shifts config lookup too          */
    const char *cache_dir;  /* where the repo databases live                */
    const char *out_file;   /* -o target for dump-cnf                       */
    int   dry_run;
    int   verbose;
    int   solver_stats;
    int   yes;              /* -y/--yes: skip confirmation prompts          */
    int   demo;             /* use the built-in demo repository             */
} options;

/* ---- colour -------------------------------------------------------------- */

/* Colour is a garnish: disabled when stdout is not a terminal or when
 * NO_COLOR is set (the de-facto standard). Defined in cli_util.c. */
extern int g_color;
extern int g_yes;

#define C_BOLD   (g_color ? "\033[1m"  : "")
#define C_DIM    (g_color ? "\033[2m"  : "")
#define C_CYAN   (g_color ? "\033[36m" : "")
#define C_GREEN  (g_color ? "\033[32m" : "")
#define C_YELLOW (g_color ? "\033[33m" : "")
#define C_RED    (g_color ? "\033[31m" : "")
#define C_OFF    (g_color ? "\033[0m"  : "")

/* ---- repositories -------------------------------------------------------- */

typedef struct { char *name; char *url; } repo_src;

#define MAX_REPOS 8
extern repo_src g_repos[MAX_REPOS];
extern int      g_nrepos;

void repo_src_add(const char *name, const char *url);
void repos_default(void);
char *db_cache_path(const char *cache_dir, const char *name);
repo *load_repo(const options *o);

/* ---- small helpers ------------------------------------------------------- */

void die(int code, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));
double now_ms(void);
const char *hr_size(long bytes);
int  ask_yes(const char *prompt);
/* `empty_reason` explains an empty transaction in the user's terms ("already
 * installed: foo", "cannot remove bar: baz depends on it"). NULL falls back to
 * a generic line. */
int  confirm_transaction(const char *label, int dry_run,
                         const package **install, int ninstall,
                         const package **remove, int nremove,
                         const char *empty_reason);
dep *parse_requests(int argc, char **argv, int *n_out);
/* qsort() comparator for an array of const char* (never cast strcmp). */
int cmp_str_ptr(const void *a, const void *b);

/* Installed packages, sorted by name; caller frees with localdb_free_list. */
int  installed_packages(const options *o, package ***out);

/* ---- transaction machinery (cli_txn.c) ------------------------------------ */

int  load_system_state(repo *r, const char *root);
int  do_install_one(const options *o, const package *p);
int  do_remove_one(const options *o, const package *p);
int  run_transaction(const options *o, const char *label,
                     const dep *add, int nadd, const dep *drop, int ndrop);

/* ---- commands ------------------------------------------------------------ */

int cmd_sync(const options *o);
int cmd_search(const options *o, const char *term);
int cmd_info(const options *o, const char *name);
int cmd_stats(const options *o);
int cmd_solve(const options *o, int argc, char **argv);
int cmd_dump_cnf(const options *o, int argc, char **argv);
int cmd_doctor(const options *o);

int cmd_install(const options *o, int argc, char **argv);
int cmd_remove(const options *o, int argc, char **argv);
int cmd_upgrade(const options *o, int argc, char **argv);
int cmd_autoremove(const options *o, int argc, char **argv);
int cmd_rollback(const options *o, int argc, char **argv);

int cmd_query(const options *o, int argc, char **argv);
int cmd_files(const options *o, int argc, char **argv);
int cmd_owns(const options *o, int argc, char **argv);
int cmd_why(const options *o, int argc, char **argv);
int cmd_mark(const options *o, int argc, char **argv);
int cmd_deptest(const options *o, int argc, char **argv);

int cmd_lock(const options *o, int argc, char **argv);
int cmd_profile(const options *o, int argc, char **argv);
int cmd_cache(const options *o, int argc, char **argv);

#endif /* NEPTUMIUM_CLI_H */