# Planet

**Planet** is the package manager of **Neptunium**. Its dependency resolver is
a from-scratch **CDCL MaxSAT solver** written in C, so every transaction Planet
prints is *provably optimal* rather than merely feasible — there is no greedy
heuristic anywhere in the resolution path.

```
planet sync                      # refresh the repository databases
planet install zstd vim          # optimal transaction, applied
planet remove vim                # keeps everything that still needs glibc
planet upgrade -S                # newest feasible set, with solver stats
planet autoremove                # drop what nothing depends on
planet mark explicit vim         # remember why it should stay
planet profile save before       # snapshot the installed set
planet rollback before           # re-solve back to that snapshot
planet lock                      # write the exact installed set
```

Inspection:

```
planet query list                # what is installed, and why it is there
planet query orphans             # candidates for autoremove
planet query check               # broken dependency edges
planet why zstd                  # walk the graph back to an explicit request
planet files zstd                # which paths a package owns
planet owns /usr/bin/zstd        # which package owns a path
planet deptest zstd              # are these dependencies satisfiable?
planet cache                     # what payloads we have downloaded
```

Targeting another tree (`--installroot` is pacman's synonym for `--root`):

```
planet install --installroot /mnt/stage zstd
```

## Why a MaxSAT solver

Package resolution is an optimisation problem: find a consistent set of
packages that satisfies every dependency, honouring conflicts and replacements,
while minimising what the user actually cares about (disk footprint, staying
on the newest versions, keeping requested packages). Planet encodes that as a
**weighted partial MaxSAT** instance and solves it to proven optimality.

- One boolean variable per candidate package (`name@version`).
- **Hard clauses** — at-most-one version per name (a sequential-ladder AMO, so
  the clause count stays linear), dependency satisfaction, and non-coexistence
  for both `Conflicts` and `Replaces`. `Provides` is folded into provider
  lookup rather than getting its own clause.
- **Weighted soft clauses** — user requests, and a combined
  footprint + newest-version family that means "unneeded packages stay out,
  and when one must be installed, take the newest feasible version".
- The MaxSAT layer itself uses **Implicit Hitting Set**: a master solves the
  minimum-weight hitting set over unsatisfiable cores, an oracle proves each
  core. This is optimal for *any* sound core sequence, unlike naive
  core-guided reweighting.

Install/remove/upgrade/autoremove all funnel through the same machinery:
everything currently installed that must stay is registered as a **hard**
requirement, the new wishes as soft ones, and the solver computes the target
state. The transaction is then the *difference*, so an operation can never
orphan a dependency or install something that conflicts.

## Repository support

Planet reads Arch-format sync databases (`core.db.tar.gz`, `extra.db.tar.gz`):
downloads them over HTTPS, parses every `desc` entry with libarchive, and
indexes 15,000+ candidates by name and virtual provides. Payloads are fetched
over the same layer, **verified against the database's SHA-256 before they are
unpacked**, and cached content-addressed by that digest.

## Hooks

Both ALPM hook mechanisms are supported, so existing packages and repository
hook configurations work unchanged:

- **Per-package `.INSTALL` scripts** — `pre_install`, `post_install`,
  `pre_upgrade`, `post_upgrade`, `pre_remove`, `post_remove` are sourced and
  invoked with ALPM's positional arguments (`version`, `caller args`, `root`)
  from inside the target root.
- **Repository hooks** — `etc/planet/hooks/*.hook` in the usual INI form
  (`[Trigger]` / `[Action]`, with `Operation`, `Type`, `Target` globs, `When`,
  `Exec`), run with the standard ALPM environment (`PACKAGE`, `VERSION`,
  `ROOT`, `CAASVARS`, ...).

Use `--no-hooks` to skip them.

## Building

Requires a C11 compiler, `libcurl` and `libarchive`. SHA-256 is vendored, so
there is no crypto-library dependency.

```sh
make            # build the test binaries
make planet     # build the CLI
make test       # run the full suite (251 checks)
```

## Layout

| Path | Purpose |
|---|---|
| `src/sat.c` | CDCL solver: watches, BCP, VSIDS, clause learning, assumptions |
| `src/maxsat.c` | Implicit-Hitting-Set MaxSAT layer |
| `src/encode.c` | packages → weighted partial MaxSAT |
| `src/amo.c` | at-most-one encodings (sequential ladder) |
| `src/pkg.c` | package model, Arch version ordering, indexed repo |
| `src/archrepo.c` | sync-database reader (`.db.tar.gz` → packages) |
| `src/net.c` | libcurl fetch layer |
| `src/pkgfile.c` | payload download, SHA-256 verify, extraction |
| `src/localdb.c` | installed-system database |
| `src/hooks.c` | ALPM hooks |
| `src/cli.h` | shared CLI plumbing (options, colour, declarations) |
| `src/main.c` | argument parsing and dispatch only |
| `src/cli_util.c` | CLI helpers and repository loading |
| `src/cli_txn.c` | the transaction machinery |
| `src/cmd_repo.c` | sync, search, info, stats, solve, dump-cnf, doctor |
| `src/cmd_query.c` | query, files, owns, why, mark, deptest |
| `src/cmd_admin.c` | lock, profile, cache |
| `src/state.c` | transaction locking and system profiles |

## Transaction safety

Mutating commands take an exclusive lock on `<root>/var/lib/planet/db.lck` for
their duration. A lock left behind by a crashed process is detected through
its recorded PID and reclaimed automatically once that process is gone, so a
crash never leaves a system permanently locked.

Profiles are named snapshots of the installed set. Because the target state is
computed by the solver rather than replayed, a rollback is validated before
anything is written and refused outright if the old set is no longer
satisfiable (a repository can drop packages over time).