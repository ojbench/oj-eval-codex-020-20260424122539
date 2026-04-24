#include "buddy.h"
#include <stdint.h>
#include <stdlib.h>

#define NULL ((void *)0)

#define MAXR 16
#define PGSIZE 4096

static uint8_t *g_alloc_rank = NULL;   /* rank if allocated block starts here, 0 otherwise */
static uint8_t *g_free_rank = NULL;    /* rank if free block starts here, 0 otherwise */
static int *g_next = NULL;             /* next index for free list (by start page index) */
static int g_head[MAXR + 1];           /* free list heads per rank: page-index or -1 */
static int g_free_cnt[MAXR + 1];       /* free counts per rank */
static char *g_base = NULL;            /* base address */
static int g_pages = 0;                /* total pages */

static inline int size_in_pages(int rank) { return 1 << (rank - 1); }
static inline int in_range_ptr(void *p) {
    if (g_base == NULL) return 0;
    uintptr_t off = (uintptr_t)((char *)p - g_base);
    if ((intptr_t)off < 0) return 0;
    if (off % PGSIZE) return 0;
    if (off / PGSIZE >= (uintptr_t)g_pages) return 0;
    return 1;
}

static void push_free(int rank, int start_idx) {
    g_free_rank[start_idx] = (uint8_t)rank;
    g_next[start_idx] = g_head[rank];
    g_head[rank] = start_idx;
    g_free_cnt[rank]++;
}

static int remove_free(int rank, int start_idx) {
    int prev = -1;
    int cur = g_head[rank];
    while (cur != -1) {
        if (cur == start_idx) {
            if (prev == -1) g_head[rank] = g_next[cur];
            else g_next[prev] = g_next[cur];
            g_free_rank[cur] = 0;
            g_free_cnt[rank]--;
            return 1;
        }
        prev = cur;
        cur = g_next[cur];
    }
    return 0;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) return -EINVAL;

    g_base = (char *)p;
    g_pages = pgcount;

    /* allocate/clear metadata */
    free(g_alloc_rank);
    free(g_free_rank);
    free(g_next);
    g_alloc_rank = (uint8_t *)calloc((size_t)pgcount, sizeof(uint8_t));
    g_free_rank = (uint8_t *)calloc((size_t)pgcount, sizeof(uint8_t));
    g_next = (int *)malloc(sizeof(int) * (size_t)pgcount);
    if (!g_alloc_rank || !g_free_rank || !g_next) return -EINVAL;

    for (int i = 0; i <= MAXR; ++i) {
        g_head[i] = -1;
        g_free_cnt[i] = 0;
    }

    for (int i = 0; i < pgcount; ++i) g_next[i] = -1;

    /* Initialize free lists by greedily packing from largest rank down */
    int pos = 0;
    for (int r = MAXR; r >= 1; --r) {
        int sz = size_in_pages(r);
        while ((pos % sz) == 0 && (pgcount - pos) >= sz) {
            push_free(r, pos);
            pos += sz;
        }
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAXR) return ERR_PTR(-EINVAL);
    /* find first available rank >= rank */
    int src = -1;
    for (int r = rank; r <= MAXR; ++r) {
        if (g_head[r] != -1) { src = r; break; }
    }
    if (src == -1) return ERR_PTR(-ENOSPC);

    int start = g_head[src];
    /* pop from src */
    g_head[src] = g_next[start];
    g_free_rank[start] = 0;
    g_free_cnt[src]--;

    int cur = src;
    while (cur > rank) {
        /* split into two buddies at cur-1 */
        int half_sz = size_in_pages(cur - 1);
        int buddy = start + half_sz;
        /* keep 'start' as the left half to continue splitting; push right half */
        push_free(cur - 1, buddy);
        cur--;
    }

    g_alloc_rank[start] = (uint8_t)rank;
    return (void *)(g_base + (size_t)start * PGSIZE);
}

int return_pages(void *p) {
    if (!in_range_ptr(p)) return -EINVAL;
    uintptr_t off = (uintptr_t)((char *)p - g_base);
    int start = (int)(off / PGSIZE);
    int cur = g_alloc_rank[start];
    if (cur < 1 || cur > MAXR) return -EINVAL;

    /* mark as unallocated before coalescing */
    g_alloc_rank[start] = 0;

    while (cur < MAXR) {
        int sz = size_in_pages(cur);
        int group = start / sz;
        int buddy_start;
        if ((group & 1) == 0) buddy_start = start + sz;
        else buddy_start = start - sz;

        if (buddy_start < 0 || buddy_start >= g_pages) break;

        /* buddy must be free and same rank */
        if (g_free_rank[buddy_start] == (uint8_t)cur) {
            /* remove buddy from free list and merge */
            if (!remove_free(cur, buddy_start)) {
                /* shouldn't happen; stop coalescing */
                break;
            }
            /* pick the lower start */
            if (buddy_start < start) start = buddy_start;
            cur++;
            continue;
        } else {
            break;
        }
    }

    /* finally push the (possibly merged) block */
    push_free(cur, start);
    return OK;
}

int query_ranks(void *p) {
    if (!in_range_ptr(p)) return -EINVAL;
    uintptr_t off = (uintptr_t)((char *)p - g_base);
    int idx = (int)(off / PGSIZE);

    if (g_alloc_rank[idx] >= 1 && g_alloc_rank[idx] <= MAXR) return g_alloc_rank[idx];

    for (int r = MAXR; r >= 1; --r) {
        int sz = size_in_pages(r);
        int start = idx - (idx % sz);
        if (start >= 0 && start < g_pages && g_free_rank[start] == (uint8_t)r) {
            return r;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAXR) return -EINVAL;
    return g_free_cnt[rank];
}
