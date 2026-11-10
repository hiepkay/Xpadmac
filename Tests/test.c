/* The harness behind test.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

unsigned long test_checks, test_failures;
const char *test_current = "?";

static void head(const char *file, int line)
{
    test_failures++;
    fprintf(stderr, "FAIL %s:%d [%s] ", file, line, test_current);
}

void test_fail(const char *file, int line, const char *what)
{
    head(file, line);
    fprintf(stderr, "%s\n", what);
}

void test_fail_int(const char *file, int line, const char *a, const char *b, long long va, long long vb)
{
    head(file, line);
    fprintf(stderr, "%s == %s: %lld (0x%llx) != %lld (0x%llx)\n", a, b, va, (unsigned long long)va, vb,
            (unsigned long long)vb);
}

static void dump(const char *label, const uint8_t *p, size_t n)
{
    size_t i;

    fprintf(stderr, "    %-6s", label);
    for (i = 0; i < n; i++) fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, "\n");
}

void test_fail_mem(const char *file, int line, const char *a, const char *b, const uint8_t *pa,
                   const uint8_t *pb, size_t n)
{
    size_t i = 0;

    while (i < n && pa[i] == pb[i]) i++;
    head(file, line);
    fprintf(stderr, "%s == %s: differ at byte %zu of %zu\n", a, b, i, n);
    dump("got", pa, n);
    dump("want", pb, n);
}

void test_fail_str(const char *file, int line, const char *a, const char *b, const char *pa, const char *pb)
{
    head(file, line);
    fprintf(stderr, "%s == %s: \"%s\" != \"%s\"\n", a, b, pa ? pa : "(null)", pb ? pb : "(null)");
}

int test_done(const char *suite)
{
    if (test_failures) {
        printf("%-14s FAILED: %lu of %lu checks\n", suite, test_failures, test_checks);
        return 1;
    }
    printf("%-14s ok (%lu checks)\n", suite, test_checks);
    return 0;
}
