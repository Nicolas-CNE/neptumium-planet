/*
 * test_repo.c — lock down the Arch sync-database parser.
 * =======================================================
 *
 * Two layers of coverage:
 *
 *   1. A SYNTHETIC database, built in memory through libarchive's write API
 *      and parsed back with archrepo_load_mem(). This exercises the format
 *      rules deterministically, with no dependence on the network or on
 *      upstream repository churn:
 *        - %NAME%/%VERSION% are authoritative, not the directory name,
 *          including epochs ("1:2.3-4");
 *        - multi-valued DEPENDS/CONFLICTS/PROVIDES/REPLACES;
 *        - download metadata (FILENAME/CSIZE/ISIZE/SHA256SUM) and origin;
 *        - a "files" entry must be skipped (only "desc" is parsed);
 *        - a malformed entry (no %VERSION%) is counted, not fatal;
 *        - re-loading the same image deduplicates (name+version).
 *
 *   2. REAL databases from /tmp/opencode/archdb/ (skipped with a notice when
 *      absent, so `make test` stays hermetic). Spot-checks chosen from the
 *      actual mirror contents:
 *        - bash: depends on readline + a versioned soname "libreadline.so=8-64",
 *          PROVIDES "sh" (virtual provider lookup must find it);
 *        - fakeroot: carries an epoch ("1:…") that must survive parsing;
 *        - iptables: the classic Arch idiom — CONFLICTS arptables/ebtables
 *          while simultaneously PROVIDES them, plus REPLACES iptables-nft;
 *        - core+extra merge into one repo with zero duplicates.
 */
#include "archrepo.h"
#include "pkg.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", name);
    cond ? g_pass++ : g_fail++;
}

/* ---- synthetic database builder ----------------------------------------- */

/* A tar.gz image in memory: we hand libarchive a fixed buffer because our
 * synthetic content is tiny (a few KB). */
#define IMG_CAP (1u << 20)

typedef struct {
    struct archive *a;
    unsigned char  *buf;
    size_t          used;
    int             ok;
} dbimg;

static void img_init(dbimg *im) {
    im->buf  = malloc(IMG_CAP);
    im->ok   = im->buf != NULL;
    im->used = 0;
    if (!im->ok) return;
    im->a = archive_write_new();
    archive_write_set_format_pax(im->a);      /* pax handles odd names fine */
    archive_write_add_filter_gzip(im->a);     /* .db.tar.gz is gzip'd        */
    if (archive_write_open_memory(im->a, im->buf, IMG_CAP, &im->used)
        != ARCHIVE_OK)
        im->ok = 0;
}

static void img_add_file(dbimg *im, const char *path, const char *content) {
    if (!im->ok) return;
    struct archive_entry *e = archive_entry_new();
    size_t len = strlen(content);
    archive_entry_set_pathname(e, path);
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_size(e, (la_int64_t)len);
    if (archive_write_header(im->a, e) != ARCHIVE_OK) im->ok = 0;
    if (archive_write_data(im->a, content, len) != (la_ssize_t)len)
        im->ok = 0;
    archive_entry_free(e);
}

static void img_finish(dbimg *im) {
    if (!im->ok) return;
    if (archive_write_close(im->a) != ARCHIVE_OK) im->ok = 0;
    archive_write_free(im->a);
}

static void img_free(dbimg *im) {
    free(im->buf);
    im->buf = NULL;
}

/* A complete, realistic 'desc' for the synthetic "foo" package. Note the tag
 * ORDER: FILENAME first, like real Arch dbs (parsers must not assume NAME
 * comes first). */
static const char *desc_foo =
    "%FILENAME%\n"
    "foo-1:2.3-4-x86_64.pkg.tar.zst\n"
    "\n"
    "%NAME%\n"
    "foo\n"
    "\n"
    "%VERSION%\n"
    "1:2.3-4\n"
    "\n"
    "%DESC%\n"
    "Foo provider of libfoo\n"
    "\n"
    "%CSIZE%\n"
    "1234\n"
    "\n"
    "%ISIZE%\n"
    "5678\n"
    "\n"
    "%SHA256SUM%\n"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
    "\n"
    "%ARCH%\n"
    "x86_64\n"
    "\n"
    "%PROVIDES%\n"
    "libfoo.so=3.0.0\n"
    "shim\n"
    "\n"
    "%DEPENDS%\n"
    "bar>=2\n"
    "bash\n"
    "\n"
    "%CONFLICTS%\n"
    "oldfoo\n"
    "\n"
    "%REPLACES%\n"
    "oldfoo\n";

static const char *desc_bar =
    "%NAME%\n"
    "bar\n"
    "\n"
    "%VERSION%\n"
    "2.5-1\n"
    "\n"
    "%DEPENDS%\n"
    "bash\n";

/* Malformed on purpose: no %VERSION% at all. */
static const char *desc_broken =
    "%NAME%\n"
    "broken\n"
    "\n"
    "%DESC%\n"
    "this entry has no version\n";

/* ---- tests --------------------------------------------------------------- */

static void test_origin_of(void) {
    char *o = archrepo_origin_of("https://geo.mirror.pkgbuild.com/core/os/x86_64/core.db.tar.gz");
    check("origin: mirror URL stripped to directory", o && strcmp(o, "https://geo.mirror.pkgbuild.com/core/os/x86_64/") == 0);
    free(o);

    o = archrepo_origin_of("file:///var/cache/planet/extra.db.tar.gz");
    check("origin: file URL", o && strcmp(o, "file:///var/cache/planet/") == 0);
    free(o);

    check("origin: NULL input", archrepo_origin_of(NULL) == NULL);
}

static void test_synthetic(void) {
    dbimg im;
    img_init(&im);
    /* Directory names deliberately differ from the authoritative tags (the
     * "files" entry, a missing epoch in the dir name) to prove the parser
     * trusts %NAME%/%VERSION% only. */
    img_add_file(&im, "foo-2.3-4/desc", desc_foo);
    img_add_file(&im, "foo-2.3-4/files", "%FILES%\nusr/bin/foo\n");
    img_add_file(&im, "bar-2.5-1/desc", desc_bar);
    img_add_file(&im, "broken-1.0/desc", desc_broken);
    img_finish(&im);
    check("synthetic: image built", im.ok);

    repo        *r = repo_new();
    archrepo_stats st;
    char         err[256] = {0};
    const char  *origin   = "https://mirror.example/core/os/x86_64/";

    int rc = archrepo_load_mem(r, im.buf, im.used, origin, &st, err, sizeof err);
    check("synthetic: load succeeds", rc == 0);
    /* Entries: foo/desc + bar/desc are good, foo/files is skipped (not a
     * desc), broken/desc has no %VERSION% => malformed. */
    check("synthetic: 2 packages (files skipped, broken counted)", st.packages == 2 && repo_count(r) == 2);
    check("synthetic: 1 malformed entry counted", st.malformed == 1);
    check("synthetic: no duplicates on first load", st.duplicates == 0);

    /* foo: metadata + relations */
    const package *foo = repo_find(r, "foo", "1:2.3-4");
    check("synthetic: foo found by name+epoch version", foo != NULL);
    if (foo) {
        check("synthetic: foo arch", strcmp(foo->arch, "x86_64") == 0);
        check("synthetic: foo desc line", foo->desc && strcmp(foo->desc, "Foo provider of libfoo") == 0);
        check("synthetic: foo filename", foo->filename && strcmp(foo->filename, "foo-1:2.3-4-x86_64.pkg.tar.zst") == 0);
        check("synthetic: foo csize/isize", foo->csize == 1234 && foo->isize == 5678);
        check("synthetic: foo sha256", foo->sha256 && strlen(foo->sha256) == 64);
        check("synthetic: foo origin stamped", foo->origin && strcmp(foo->origin, origin) == 0);

        check("synthetic: foo has 2 depends", foo->ndepends == 2);
        if (foo->ndepends == 2) {
            check("synthetic: dep bar>=2 parsed", strcmp(foo->depends[0].name, "bar") == 0
                  && foo->depends[0].op == DEP_GE
                  && strcmp(foo->depends[0].version, "2") == 0);
            check("synthetic: unversioned dep bash", strcmp(foo->depends[1].name, "bash") == 0
                  && foo->depends[1].op == DEP_ANY);
        }
        check("synthetic: foo conflicts oldfoo", foo->nconflicts == 1
              && foo->conflicts[0].op == DEP_ANY
              && strcmp(foo->conflicts[0].name, "oldfoo") == 0);
        check("synthetic: foo replaces oldfoo", foo->nreplaces == 1
              && strcmp(foo->replaces[0].name, "oldfoo") == 0);
        check("synthetic: foo provides 2 virtuals", foo->nprovides == 2);
        if (foo->nprovides == 2) {
            check("synthetic: versioned provide libfoo.so=3.0.0", foo->provides[0].op == DEP_EQ
                  && strcmp(foo->provides[0].version, "3.0.0") == 0);
            check("synthetic: unversioned provide shim", foo->provides[1].op == DEP_ANY);
        }
    }

    /* provider resolution through the index */
    const package **hits = malloc(8 * sizeof *hits);
    int n = repo_lookup(r, "libfoo.so", DEP_ANY, NULL, hits, 8);
    check("synthetic: virtual libfoo.so resolves to foo", n == 1 && hits[0] == foo);
    n = repo_lookup(r, "libfoo.so", DEP_GE, "2.0", hits, 8);
    check("synthetic: versioned virtual lookup ok", n == 1);
    n = repo_lookup(r, "libfoo.so", DEP_GE, "4.0", hits, 8);
    check("synthetic: virtual too old rejected", n == 0);
    n = repo_lookup(r, "shim", DEP_ANY, NULL, hits, 8);
    check("synthetic: unversioned virtual shim", n == 1);
    free(hits);

    /* re-loading the same image must deduplicate, not double the repo */
    archrepo_stats st2;
    rc = archrepo_load_mem(r, im.buf, im.used, origin, &st2, err, sizeof err);
    check("synthetic: reload succeeds", rc == 0);
    check("synthetic: reload dedupes (2 dupes, 0 new)", st2.packages == 0
          && st2.duplicates == 2 && repo_count(r) == 2);

    repo_free(r);
    img_free(&im);
}

/* ---- real database spot-checks ------------------------------------------- */

#define CORE_DB "/tmp/opencode/archdb/core.db.tar.gz"
#define EXTRA_DB "/tmp/opencode/archdb/extra.db.tar.gz"

static void test_real_dbs(void) {
    if (access(CORE_DB, R_OK) != 0) {
        printf("SKIP  real-db checks (%s not present)\n", CORE_DB);
        return;
    }

    repo *r = repo_new();
    archrepo_stats st;
    char err[256] = {0};
    const char *origin = "https://geo.mirror.pkgbuild.com/core/os/x86_64/";

    int rc = archrepo_load_file(r, CORE_DB, origin, &st, err, sizeof err);
    char label[128];
    snprintf(label, sizeof label, "core: loads (rc=0, %d pkgs)", st.packages);
    check(label, rc == 0);
    check("core: hundreds of packages", st.packages >= 250);
    check("core: no duplicates", st.duplicates == 0);
    check("core: no malformed entries", st.malformed == 0);
    check("core: every dep line parsed", st.baddeps == 0);

    /* bash: versioned soname dep + virtual 'sh' provider */
    const package *bash = repo_newest(r, "bash", DEP_ANY, NULL);
    check("core: bash present", bash != NULL);
    if (bash) {
        int has_soname = 0, has_readline = 0;
        for (int i = 0; i < bash->ndepends; i++) {
            if (strcmp(bash->depends[i].name, "libreadline.so") == 0
                && bash->depends[i].op == DEP_EQ)
                has_soname = 1;
            if (strcmp(bash->depends[i].name, "readline") == 0)
                has_readline = 1;
        }
        check("core: bash depends on readline", has_readline);
        check("core: bash soname dep libreadline.so=8-64 parsed", has_soname);
        check("core: bash provides virtual sh", pkg_provides(bash, "sh", DEP_ANY, NULL));
        check("core: bash filename metadata", bash->filename
              && strncmp(bash->filename, "bash-", 5) == 0
              && strstr(bash->filename, ".pkg.tar.zst") != NULL);
        check("core: bash sha256 populated", bash->sha256 && strlen(bash->sha256) == 64);
        check("core: bash csize sane", bash->csize > 0);
        check("core: bash origin stamped", bash->origin && strcmp(bash->origin, origin) == 0);
    }

    /* fakeroot: epoch must survive as part of the version */
    const package *fr = repo_newest(r, "fakeroot", DEP_ANY, NULL);
    check("core: fakeroot has an epoch", fr != NULL && fr->version[0] == '1'
          && fr->version[1] == ':' && ver_cmp(fr->version, "1.37.2-3") > 0);

    /* iptables: CONFLICTS + REPLACES + PROVIDES of the very same names */
    const package *ipt = repo_newest(r, "iptables", DEP_ANY, NULL);
    check("core: iptables present", ipt != NULL);
    if (ipt) {
        int c_arptables = 0, r_nft = 0, p_libip4 = 0, p_arptables = 0;
        for (int i = 0; i < ipt->nconflicts; i++)
            if (strcmp(ipt->conflicts[i].name, "arptables") == 0) c_arptables = 1;
        for (int i = 0; i < ipt->nreplaces; i++)
            if (strcmp(ipt->replaces[i].name, "iptables-nft") == 0) r_nft = 1;
        for (int i = 0; i < ipt->nprovides; i++) {
            if (strcmp(ipt->provides[i].name, "libip4tc.so") == 0
                && ipt->provides[i].op == DEP_EQ) p_libip4 = 1;
            if (strcmp(ipt->provides[i].name, "arptables") == 0) p_arptables = 1;
        }
        check("core: iptables conflicts arptables", c_arptables);
        check("core: iptables replaces iptables-nft", r_nft);
        check("core: iptables provides versioned soname", p_libip4);
        check("core: iptables provides what it conflicts (Arch idiom)", p_arptables);
    }

    /* glibc: '+'-laden version strings must not break ordering */
    const package *gl = repo_newest(r, "glibc", DEP_ANY, NULL);
    check("core: glibc version sorts above 2.0", gl && ver_cmp(gl->version, "2.0") > 0);

    /* merge extra on top: no name+version collisions between the two dbs */
    if (access(EXTRA_DB, R_OK) == 0) {
        const char *origin2 = "https://geo.mirror.pkgbuild.com/extra/os/x86_64/";
        archrepo_stats st2;
        rc = archrepo_load_file(r, EXTRA_DB, origin2, &st2, err, sizeof err);
        check("extra: loads", rc == 0);
        check("extra: tens of thousands of packages", st2.packages >= 10000);
        check("extra: no collision with core", st2.duplicates == 0);

        const package *vim = repo_newest(r, "vim", DEP_ANY, NULL);
        check("extra: vim resolvable after merge", vim != NULL);
        check("extra: vim comes from the extra origin", vim && vim->origin
              && strcmp(vim->origin, origin2) == 0);
        /* a core package must still carry the core origin after the merge */
        check("core: bash origin survives merge", bash && bash->origin
              && strcmp(bash->origin, origin) == 0);
        printf("INFO  merged repo: %d candidates\n", repo_count(r));
    } else {
        printf("SKIP  extra-db merge checks (%s not present)\n", EXTRA_DB);
    }

    repo_free(r);
}

int main(void) {
    test_origin_of();
    test_synthetic();
    test_real_dbs();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
