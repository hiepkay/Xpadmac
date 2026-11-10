/*
 * A tiny test harness for XPadMac's shared core: checks that report the
 * file, line and values, and keep going so one run shows every failure.
 *
 *   CHECK(cond)            any condition
 *   CHECK_EQ(a, b)         two integers (printed in decimal and hex)
 *   CHECK_MEM(a, b, n)     two byte buffers (printed as hex on failure)
 *   CHECK_STR(a, b)        two strings
 *   TEST(name)             groups the checks that follow, for the report
 *   return test_done("x"); at the end of main: prints a summary, exit code
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPAD_TEST_H
#define XPAD_TEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned long test_checks, test_failures;
extern const char *test_current;

void test_fail(const char *file, int line, const char *what);
void test_fail_int(const char *file, int line, const char *a, const char *b, long long va, long long vb);
void test_fail_mem(const char *file, int line, const char *a, const char *b, const uint8_t *pa,
                   const uint8_t *pb, size_t n);
void test_fail_str(const char *file, int line, const char *a, const char *b, const char *pa, const char *pb);
int test_done(const char *suite);

#define TEST(name) (test_current = (name))

#define CHECK(cond)                                                            \
    do {                                                                       \
        test_checks++;                                                         \
        if (!(cond)) test_fail(__FILE__, __LINE__, #cond);                     \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        long long va_ = (long long)(a), vb_ = (long long)(b);                  \
        test_checks++;                                                         \
        if (va_ != vb_) test_fail_int(__FILE__, __LINE__, #a, #b, va_, vb_);   \
    } while (0)

#define CHECK_MEM(a, b, n)                                                     \
    do {                                                                       \
        const uint8_t *pa_ = (const uint8_t *)(a), *pb_ = (const uint8_t *)(b);\
        size_t n_ = (size_t)(n);                                               \
        test_checks++;                                                         \
        if (memcmp(pa_, pb_, n_) != 0)                                         \
            test_fail_mem(__FILE__, __LINE__, #a, #b, pa_, pb_, n_);           \
    } while (0)

#define CHECK_STR(a, b)                                                        \
    do {                                                                       \
        const char *sa_ = (a), *sb_ = (b);                                     \
        test_checks++;                                                         \
        if (!sa_ || !sb_ || strcmp(sa_, sb_) != 0)                             \
            test_fail_str(__FILE__, __LINE__, #a, #b, sa_, sb_);               \
    } while (0)

/* ---- the host xpad_log (xpad_log_host.c) keeps the lines it is given ------- */

#define TEST_LOG_KEEP 1024          /* the most recent lines kept */

void test_log_clear(void);
unsigned long test_log_total(void);         /* lines logged since the last clear */
const char *test_log_line(unsigned long i); /* i-th since the last clear, if still kept; else NULL */
/* Index of the first kept line at or after `from` containing `needle`, or -1. */
long test_log_find(unsigned long from, const char *needle);

/* ---- the host xpad_random (xpad_log_host.c) is deterministic ---------------- */

void test_random_feed(const uint8_t *p, size_t n);  /* these bytes first, in order */
size_t test_random_left(void);                       /* fed bytes not yet drawn */
void test_random_seed(uint32_t seed);                /* drop the feed, restart the stream */
unsigned long test_random_drawn(void);               /* bytes drawn since the last seed */

#endif
