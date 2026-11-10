/* See hid_walk.h. Follows the HID 1.11 item rules (6.2.2), with the subset
 * of features a gamepad descriptor uses: no Push/Pop, no delimiters, no
 * extended (32-bit) usages beyond page:usage pairs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "hid_walk.h"

#include <string.h>

#define MAX_USAGES 32

typedef struct {
    uint16_t page;
    int32_t lmin, lmax;
    unsigned size, count;
    uint8_t id;
} globals;

typedef struct {
    uint32_t usages[MAX_USAGES];    /* page << 16 | usage when given in 32 bits, else usage */
    size_t nusages;
    uint32_t umin, umax;
    int has_min, has_max;
} locals;

static uint32_t item_u(const uint8_t *p, unsigned n)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_s(const uint8_t *p, unsigned n)
{
    uint32_t v = item_u(p, n);

    if (n == 1) return (int8_t)v;
    if (n == 2) return (int16_t)v;
    return (int32_t)v;
}

static hid_report *report(hid_layout *l, int type, uint8_t id)
{
    size_t i;

    for (i = 0; i < l->nreports; i++)
        if (l->reports[i].type == type && l->reports[i].report_id == id) return &l->reports[i];
    if (l->nreports == HID_MAX_REPORTS) return NULL;
    l->reports[l->nreports].type = type;
    l->reports[l->nreports].report_id = id;
    l->reports[l->nreports].bits = 0;
    return &l->reports[l->nreports++];
}

/* The usage of field k of a main item, as HID 1.11 6.2.2.8 assigns them. */
static uint32_t usage_of(const locals *lo, unsigned k)
{
    if (lo->has_min && lo->has_max) {
        uint32_t u = lo->umin + k;
        return u > lo->umax ? lo->umax : u;
    }
    if (lo->nusages == 0) return 0;
    return lo->usages[k < lo->nusages ? k : lo->nusages - 1];
}

static int main_item(hid_layout *l, int type, unsigned data, const globals *g, const locals *lo)
{
    hid_report *r = report(l, type, g->id);
    unsigned k;

    if (!r) return 0;
    for (k = 0; k < g->count; k++) {
        hid_field *f;
        uint32_t u = usage_of(lo, k);

        /* An array (bit 1 clear) holds indices, not one usage per field;
         * this descriptor has none, so they are listed with no usage. */
        if (l->nfields == HID_MAX_FIELDS) return 0;
        f = &l->fields[l->nfields++];
        f->type = type;
        f->report_id = g->id;
        f->offset = r->bits + k * g->size;
        f->size = g->size;
        f->flags = data;
        f->logical_min = g->lmin;
        f->logical_max = g->lmax;
        if ((data & 0x01) || !(data & 0x02) || u == 0) {
            f->page = f->usage = 0;
        } else {
            f->page = (u >> 16) ? (uint16_t)(u >> 16) : g->page;
            f->usage = (uint16_t)(u & 0xFFFF);
        }
    }
    r->bits += g->count * g->size;
    return 1;
}

int hid_walk(const uint8_t *d, size_t n, hid_layout *out)
{
    globals g;
    locals lo;
    size_t i = 0;

    memset(out, 0, sizeof *out);
    memset(&g, 0, sizeof g);
    memset(&lo, 0, sizeof lo);
    while (i < n) {
        uint8_t prefix = d[i];
        unsigned size = (unsigned[]){ 0, 1, 2, 4 }[prefix & 0x03];
        unsigned type = (prefix >> 2) & 0x03, tag = prefix >> 4;
        const uint8_t *p = d + i + 1;
        uint32_t u;

        if (prefix == 0xFE) return -(int)i - 1;             /* long items: not used by pads */
        if (i + 1 + size > n) return -(int)i - 1;
        u = item_u(p, size);
        out->items++;

        switch (type) {
        case 0:                                             /* main */
            switch (tag) {
            case 0x8: if (!main_item(out, HID_INPUT, u, &g, &lo)) return -(int)i - 1; break;
            case 0x9: if (!main_item(out, HID_OUTPUT, u, &g, &lo)) return -(int)i - 1; break;
            case 0xB: if (!main_item(out, HID_FEATURE, u, &g, &lo)) return -(int)i - 1; break;
            case 0xA: out->collections_open++; break;
            case 0xC: out->collections_open--; break;
            default: return -(int)i - 1;
            }
            memset(&lo, 0, sizeof lo);                      /* locals end with every main item */
            break;
        case 1:                                             /* global */
            switch (tag) {
            case 0x0: g.page = (uint16_t)u; break;
            case 0x1: g.lmin = item_s(p, size); break;
            case 0x2: g.lmax = item_s(p, size); break;
            case 0x3: case 0x4: case 0x5: case 0x6: break;  /* physical range, unit exponent, unit */
            case 0x7: g.size = u; break;
            case 0x8: g.id = (uint8_t)u; break;
            case 0x9: g.count = u; break;
            default: return -(int)i - 1;                    /* push/pop */
            }
            break;
        case 2:                                             /* local */
            switch (tag) {
            case 0x0:
                if (lo.nusages == MAX_USAGES) return -(int)i - 1;
                lo.usages[lo.nusages++] = u;
                break;
            case 0x1: lo.umin = u; lo.has_min = 1; break;
            case 0x2: lo.umax = u; lo.has_max = 1; break;
            default: break;                                 /* designators, strings */
            }
            break;
        default:
            return -(int)i - 1;
        }
        i += 1 + size;
    }
    return 0;
}

const hid_report *hid_find_report(const hid_layout *l, int type, uint8_t id)
{
    size_t i;

    for (i = 0; i < l->nreports; i++)
        if (l->reports[i].type == type && l->reports[i].report_id == id) return &l->reports[i];
    return NULL;
}

const hid_field *hid_find_usage(const hid_layout *l, int type, uint16_t page, uint16_t usage)
{
    size_t i;

    for (i = 0; i < l->nfields; i++)
        if (l->fields[i].type == type && l->fields[i].page == page && l->fields[i].usage == usage)
            return &l->fields[i];
    return NULL;
}
