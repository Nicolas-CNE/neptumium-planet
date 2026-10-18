/*
 * hooks.h — Neptunium Planet: ALPM-compatible hooks.
 * =====================================================
 *
 * Two independent hook mechanisms exist in the Arch ecosystem, and Planet
 * supports both so existing packages and repository hook configurations work
 * unchanged:
 *
 * 1. PER-PACKAGE .INSTALL SCRIPTS
 *    A package payload may carry a bash script named `.INSTALL` at the archive
 *    root. It defines any of these functions:
 *
 *        pre_install()  post_install()
 *        pre_upgrade()  post_upgrade()
 *        pre_remove()   post_remove()
 *
 *    and is sourced rather than executed by the shell, with the same
 *    positional arguments alpm uses: $1 = package version, $2 = caller-supplied
 *    arguments, $3 = target root. They execute from inside the target root,
 *    which is what scripts operating on absolute paths such as /usr/... expect.
 *
 * 2. REPOSITORY HOOKS  (read from <root>/etc/planet/hooks/, files ending .hook)
 *    The INI-style files pacman also reads from /etc/pacman.d/hooks:
 *
 *        [Trigger]
 *        Operation = Install          # Install | Remove | Upgrade
 *        Type      = Package          # Package | Path
 *        Target    = glibc*           # glob, for Type=Package
 *
 *        [Action]
 *        Description = Regenerate the module list
 *        When        = PostInstall    # see hook_point below
 *        Exec        = depmod -a
 *
 *    These run with the standard environment ALPM provides (PACKAGE,
 *    PACKAGES, VERSION, ROOT, REPO, REPOVER, CAASVARS, ...), so hook scripts
 *    written for pacman behave identically here.
 *
 * FAILURE POLICY
 *   A failing hook aborts the transaction. Silently ignoring hook failures is
 *   how systems end up subtly broken (missing depmod output, missing
 *   ldconfig cache) with no indication of why.
 */
#ifndef NEPTUMIUM_HOOKS_H
#define NEPTUMIUM_HOOKS_H

#include "pkg.h"

/* Point in a transaction at which a repository hook may fire. The naming
 * follows ALPM so existing .hook files work unchanged. */
typedef enum {
    HP_PRE_TRANSACTION,
    HP_POST_TRANSACTION,
    HP_PRE_INSTALL,
    HP_POST_INSTALL,
    HP_PRE_UPGRADE,
    HP_POST_UPGRADE,
    HP_PRE_REMOVE,
    HP_POST_REMOVE
} hook_point;

/* Which .INSTALL function to invoke. */
typedef enum {
    IP_PRE_INSTALL,
    IP_POST_INSTALL,
    IP_PRE_UPGRADE,
    IP_POST_UPGRADE,
    IP_PRE_REMOVE,
    IP_POST_REMOVE
} install_phase;

/* The bash function name for a phase ("pre_install", ...). */
const char *install_phase_func(install_phase p);

/* Map a hook_point to its .hook keyword ("PostInstall", ...). */
const char *hook_point_key(hook_point p);

/* Run a package's .INSTALL function for `phase`.
 *
 * `pkg` only needs its name/version (for messages) and — for packages already
 * in the cache — the payload is located via pkgfile_fetch's cache. `args` is
 * the caller-supplied argument string (may be NULL).
 *
 * Returns 0 when there is no .INSTALL script or the package does not define
 * that function, and -1 when the hook exists but failed. */
int hooks_run_install_script(const package *p, const char *root,
                             install_phase phase, const char *args);

/* Run every repository hook matching `op` ("Install"/"Remove"/"Upgrade") whose
 * Trigger targets one of `pkgs` and whose Action fires at `point`.
 * Returns 0 on success, -1 if any hook failed. */
int hooks_run_repo(const char *root, const char *op, hook_point point,
                   const package *const *pkgs, int npkgs);

/* Whether hooks are enabled for this transaction (--no-hooks disables). */
void hooks_set_enabled(int on);
int  hooks_enabled(void);

/* Describe the most recent failure. */
const char *hooks_error(void);

#endif /* NEPTUMIUM_HOOKS_H */