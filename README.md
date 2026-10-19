# Planet

The package manager of **Neptunium**.

Every transaction Planet prints is **provably optimal**, not merely a working
answer. The resolver is a from-scratch CDCL MaxSAT solver written in C, and
it searches the space of consistent package sets for the cheapest one. There is
no greedy heuristic in the resolution path, so when Planet installs something
you can rely on the guarantee it states: no cheaper set exists.

```sh
planet sync
planet install zstd
planet remove zstd
```

## Contents

- [Requirements](#requirements)
- [Build and install](#build-and-install)
- [Getting started](#getting-started)
- [Command reference](#command-reference)
- [Why a MaxSAT solver](#why-a-maxsat-solver)
- [Repository support](#repository-support)
- [Hooks](#hooks)
- [Transactions and safety](#transactions-and-safety)
- [Source layout](#source-layout)

## Requirements

- A C11 compiler (GCC or Clang)
- `libcurl` and `libarchive` development headers
- `make`

SHA-256 is vendored, so there is no dependency on a crypto library.

To install packages into your own system, Planet needs to run as `root` — it
writes to `/usr`, creates device nodes and runs package scripts. Everything
else, including resolution, works unprivileged against a staging root.

## Build and install

```sh
make planet          # build the CLI -> build/planet
make test            # run the test suite
sudo make install    # install to /usr/local/bin
```

`make install` places the binary at `$(PREFIX)/bin`, `/usr/local/bin` by default;
set `PREFIX=/usr` for the conventional Arch location, or use `DESTDIR` for a
staged install. There is nothing else to install: the default repositories are
compiled in.

## Getting started

```sh
# 1. Fetch the repository databases (once, and whenever they change).
planet sync

# 2. See what is installed and why it is there.
planet query list

# 3. Install something. The plan is shown before anything is touched.
planet install zstd
```

A transaction always prints its plan and asks before applying it:

```
Transaction plan
  + zstd                      1.5.7-3
  + lz4                       1:1.10.0-2
  installing lz4 1:1.10.0-2
  installing zstd 1.5.7-3
```

Use `-n` to see the plan without applying it, and `-y` to skip the prompt.

## Command reference

Managing packages:

```sh
planet sync                    # refresh the repository databases
planet install <name...>       # install, or reinstall if already present
planet remove <name...>        # remove, refusing anything still needed
planet upgrade                 # move to the newest feasible set
planet update                  # synonym for upgrade
planet autoremove              # remove what no explicit package needs
planet mark explicit <name>    # "I asked for this" — protects it from autoremove
planet mark dependency <name>  # "this came in as a dependency"
```

Inspecting the system:

```sh
planet query list              # what is installed, and why
planet query orphans           # candidates for autoremove
planet query check             # broken dependency edges
planet why <name>              # walk the graph back to an explicit request
planet files <name>            # which paths a package owns
planet owns <path>             # which package owns a path
planet deptest <name...>       # are these dependencies satisfiable?
planet lock                    # write the installed set to a lock file
```

Snapshots:

```sh
planet profile save before     # snapshot the installed set
planet profile list
planet rollback before         # re-solve back to that snapshot
planet profile remove before
```

Resolution:

```sh
planet search <term>           # find packages by name or description
planet info <name>             # one package in full
planet stats                   # summarise the loaded repositories
planet solve <name...>         # the optimal set for a request, without applying
planet dump-cnf <name...>      # export the MaxSAT instance as WCNF
planet solve -S <name...>      # same, with solver statistics
planet doctor                  # check the cache, databases and environment
```

Version constraints are accepted wherever a package name is:

```sh
planet install 'vim>=9'
planet install 'gcc=15.2.1'
```

Global options:

| Option | Meaning |
|---|---|
| `-r`, `--root <dir>` | target directory (default `/`) |
| `--installroot <dir>` | synonym for `--root` |
| `--sysroot <dir>` | also relocate config and hooks, as pacman does |
| `-c`, `--cache-dir <dir>` | where databases and payloads live |
| `-n` | show the plan, change nothing |
| `-y` | apply without prompting |
| `-S` | print solver statistics |
| `-v` | verbose |
| `--demo` | use the built-in example repository |
| `--no-hooks` | skip package and repository hooks |

## Why a MaxSAT solver

Package resolution is an optimisation problem: find a set of packages that
satisfies every dependency, honours conflicts and replacements, and minimises
what you actually care about — disk footprint, staying current, and keeping what
you explicitly asked for. Planet encodes that as a **weighted partial MaxSAT**
instance and solves it to proven optimality.

- One boolean variable per candidate package (`name@version`). Candidates are
  restricted to the closure reachable from your requests, which is what keeps
  the solve in the millisecond range against a 15,000-package repository.
- **Hard clauses** — at-most-one version per name (a sequential-ladder AMO, so
  the clause count stays linear), dependency satisfaction, and non-coexistence
  for both `Conflicts` and `Replaces`. `Provides` is folded into provider lookup
  rather than getting a clause of its own.
- **Weighted soft clauses** — user requests, plus a combined footprint and
  newest-version family that means "unneeded packages stay out, and when one
  must be installed, take the newest feasible version".
- The MaxSAT layer uses **Implicit Hitting Set**: a master solves the
  minimum-weight hitting set over the unsatisfiable cores found so far, and an
  oracle proves each core. This is optimal for *any* sound core sequence, which
  is precisely what naive core-guided reweighting is not.

Install, remove, upgrade and autoremove all funnel through the same machinery:
everything that must stay is registered as a hard requirement, the new wishes
as soft ones, and the solver computes the target state. The transaction is the
*difference* between the current and target states, so an operation cannot
orphan a dependency or install something that conflicts.

When a transaction changes nothing, Planet says which of the three situations
it is, rather than reporting only that there is nothing to do:

```
all 1 requested package already satisfied
cannot remove glibc: it is still required by the system
no package in the configured repositories provides foo
```

## Repository support

Planet reads Arch-format sync databases (`core.db.tar.gz`, `extra.db.tar.gz`):
it downloads them over HTTPS, parses every `desc` entry with libarchive, and
indexes candidates by name and by virtual provides. Payloads are fetched
through the same layer, **verified against the SHA-256 recorded in the
database before they are unpacked**, and cached content-addressed by that
digest.

Arch's exact-version convention is honoured: a pin written without a pkgrel
(`systemd-libs=262.1`) matches the provider however it was built
(`systemd-libs-262.1-1`), which is what makes split package families resolvable.

## Hooks

Both ALPM hook mechanisms are supported, so existing package and repository
hook configurations work unchanged:

- **Per-package `.INSTALL` scripts** — `pre_install`, `post_install`,
  `pre_upgrade`, `post_upgrade`, `pre_remove` and `post_remove` are sourced and
  invoked with ALPM's positional arguments (version, caller arguments, root),
  running from inside the target root.
- **Repository hooks** — `etc/planet/hooks/*.hook` in the usual INI form
  (`[Trigger]` and `[Action]`, with `Operation`, `Type`, `Target` globs, `When`
  and `Exec`), run with the standard ALPM environment (`PACKAGE`, `VERSION`,
  `ROOT`, `CAASVARS`, ...).

Pass `--no-hooks` to skip them.

## Transactions and safety

Mutating commands take an exclusive lock on `<root>/var/lib/planet/db.lck` for
their duration. A lock left behind by a crashed process is detected through its
recorded PID and reclaimed once that process is gone, so a crash never leaves a
system permanently locked.

Profiles are named snapshots of the installed set. Because the target state is
computed by the solver rather than replayed, a rollback is validated before
anything is written, and is refused outright if the old set is no longer
satisfiable — which happens when a repository drops packages over time.

Autoremove works from explicit marks: packages you asked for are the roots, and
everything transitively required from those roots is kept. A package that
nothing needs is removed; one you asked for is never removed on its own.

## Source layout

| Path | Purpose |
|---|---|
| `src/sat.c` | CDCL solver: watches, BCP, VSIDS, clause learning, assumptions |
| `src/maxsat.c` | Implicit-Hitting-Set MaxSAT layer |
| `src/encode.c` | packages → weighted partial MaxSAT |
| `src/amo.c` | at-most-one encodings (sequential ladder) |
| `src/pkg.c` | package model, Arch version ordering, indexed repository |
| `src/archrepo.c` | sync-database reader (`.db.tar.gz` → packages) |
| `src/net.c` | libcurl fetch layer |
| `src/pkgfile.c` | payload download, SHA-256 verification, extraction |
| `src/localdb.c` | installed-system database |
| `src/hooks.c` | ALPM hooks |
| `src/state.c` | transaction locking and system profiles |
| `src/cli.h` | shared CLI plumbing (options, colour, declarations) |
| `src/main.c` | argument parsing and dispatch only |
| `src/cli_util.c` | CLI helpers and repository loading |
| `src/cli_txn.c` | the transaction machinery |
| `src/cmd_repo.c` | sync, search, info, stats, solve, dump-cnf, doctor |
| `src/cmd_query.c` | query, files, owns, why, mark, deptest |
| `src/cmd_admin.c` | lock, profile, cache |