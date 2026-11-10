/*
 * xpad_log() for host builds of the shared core (the driver has its own,
 * on os_log). It keeps the most recent lines so tests can check what the
 * GIP code says, and prints them to stderr when XPAD_TEST_VERBOSE is set.
 *
 * xpad_random() for the tests: deterministic. Bytes a test queued with
 * test_random_feed() come first, in order; after them, a fixed xorshift
 * stream (test_random_seed() restarts it).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gip.h"
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define LINE_MAX_LEN 256

static char lines[TEST_LOG_KEEP][LINE_MAX_LEN];
static unsigned long total;
static int verbose = -1;

void xpad_log(const char *fmt, ...)
{
    char *line = lines[total % TEST_LOG_KEEP];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, LINE_MAX_LEN, fmt, ap);
    va_end(ap);
    total++;

    if (verbose < 0) verbose = getenv("XPAD_TEST_VERBOSE") != NULL;
    if (verbose) fprintf(stderr, "  log: %s\n", line);
}

void test_log_clear(void)
{
    total = 0;
}

unsigned long test_log_total(void)
{
    return total;
}

const char *test_log_line(unsigned long i)
{
    if (i >= total || total - i > TEST_LOG_KEEP) return NULL;
    return lines[i % TEST_LOG_KEEP];
}

long test_log_find(unsigned long from, const char *needle)
{
    unsigned long i;

    if (total > TEST_LOG_KEEP && from < total - TEST_LOG_KEEP) from = total - TEST_LOG_KEEP;
    for (i = from; i < total; i++)
        if (strstr(lines[i % TEST_LOG_KEEP], needle)) return (long)i;
    return -1;
}

/* ---- xpad_random --------------------------------------------------------------- */

#define FEED_MAX 4096

static uint8_t feed[FEED_MAX];
static size_t feed_len, feed_pos;
static uint32_t xs = 0x2545F491u;
static unsigned long drawn;

void test_random_feed(const uint8_t *p, size_t n)
{
    if (feed_pos == feed_len) feed_pos = feed_len = 0;
    if (n > FEED_MAX - feed_len) n = FEED_MAX - feed_len;
    memcpy(feed + feed_len, p, n);
    feed_len += n;
}

size_t test_random_left(void)
{
    return feed_len - feed_pos;
}

void test_random_seed(uint32_t seed)
{
    xs = seed ? seed : 0x2545F491u;
    feed_pos = feed_len = 0;
    drawn = 0;
}

unsigned long test_random_drawn(void)
{
    return drawn;
}

void xpad_random(void *buf, size_t n)
{
    uint8_t *b = (uint8_t *)buf;
    size_t i;

    for (i = 0; i < n; i++) {
        if (feed_pos < feed_len) {
            b[i] = feed[feed_pos++];
        } else {
            xs ^= xs << 13;
            xs ^= xs >> 17;
            xs ^= xs << 5;
            b[i] = (uint8_t)(xs >> 24);
        }
        drawn++;
    }
}
