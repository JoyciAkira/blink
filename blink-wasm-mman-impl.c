/* blink-wasm-mman-impl.c — mmap/munmap shim for blink on wasm32 NO-MMU.
 *
 * DESIGN: sub-page allocator.
 *
 *   FLAG_pagesize = 4096 (patched in map.c for __wasm32__).
 *   FreePage invariant: round-down with stride=4096, so returned pointers
 *   must be 4KB-aligned.  This lets us sub-allocate 4KB slots inside 64KB
 *   wasm pages without per-slot 64KB waste.
 *
 *   Slab layout (one slab = 1 wasm page = 64KB):
 *     [wasm-page-base, 64KB-aligned]
 *     slot 0 : bytes [0,    4095]   <- 4KB-aligned ✓
 *     slot 1 : bytes [4096, 8191]   <- 4KB-aligned ✓
 *     ...
 *     slot 15: bytes [61440,65535]  <- 4KB-aligned ✓
 *
 *   Slab header: stored in a separately-allocated slab_meta[] array
 *   (avoids overwriting slot 0 before it's marked used).
 *     .base   : wasm page base address
 *     .used   : uint16_t bitmask of occupied slots (bit i = slot i)
 *     .next   : intrusive linked list of slabs with free slots
 *
 *   mmap(0, 4096, ..., anon):
 *     pop a slab from g_partial; find lowest free slot via ctz16; mark it;
 *     if slab now full, remove from partial list.
 *     return slabbase + slot*4096.
 *
 *   mmap(0, n, ...) where 4096 < n <= 65536:
 *     need ceil(n/4096) consecutive slots in one slab.
 *     scan partial slabs for a run; if none, grow a new slab.
 *
 *   mmap(0, n, ...) where n > 65536:
 *     allocate ceil(n/65536) contiguous wasm pages via memory.grow.
 *     stored in g_big_free list on munmap.
 *
 *   munmap(ptr, len):
 *     - compute slab base = ptr & ~0xFFFF
 *     - find meta for that base; clear slot bits
 *     - if slab was full, re-add to g_partial
 *     - if slab now completely empty AND it was grown just for one large
 *       alloc (nslots>1 big), put on g_big_free for reuse
 *
 *   file-backed mmap: with FLAG_pagesize=4096, mugskew is always 0 for
 *   ELF LOAD segments (offset always 4KB-aligned), so mugsize=4096 always.
 *   We read the fd content into the slot exactly as before.
 *
 *   Thread safety: blink DISABLE_THREADS — single-threaded, no locks needed.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */
#define WASM_PAGE    65536u
#define SLOTS_PER_SLAB  16u        /* 16 × 4096 = 65536 = 1 wasm page */
#define SLOT_SIZE    4096u

/* -------------------------------------------------------------------------
 * Slab metadata
 * ---------------------------------------------------------------------- */
#define MAX_SLABS  4096            /* max concurrent slabs = 4096 × 64KB = 256MB backing */

typedef struct {
    uint8_t  *base;                /* wasm-page-aligned base, NULL if slot unused */
    uint16_t  used;                /* bitmask: bit i = slot i occupied            */
    uint16_t  nslots;              /* 1 for normal slabs; >1 for big allocs        */
    int       in_partial;          /* true if on g_partial list                    */
    int       next_partial;        /* index of next slab in g_partial list (-1=end)*/
} SlabMeta;

static SlabMeta g_meta[MAX_SLABS];
static int      g_meta_count = 0;

static int g_partial = -1;         /* head of partial-slab free list (index into g_meta) */

/* -------------------------------------------------------------------------
 * Big-block free list (for allocations > 64KB)
 * ---------------------------------------------------------------------- */
typedef struct BigFree {
    uint8_t        *base;
    size_t          nwasm_pages;
    struct BigFree *next;
} BigFree;

static BigFree *g_big_free = NULL; /* simple LIFO — size is rarely reused exactly */

/* -------------------------------------------------------------------------
 * wasm memory.grow helpers
 * ---------------------------------------------------------------------- */
static void *bw_grow(size_t nslots) {
    size_t cur = __builtin_wasm_memory_grow(0, 0); /* current pages probe */
    size_t prev = __builtin_wasm_memory_grow(0, nslots);
    if (prev == (size_t)-1) {
        char p[160];
        int pn = snprintf(p, sizeof(p),
            "[T5M] WASM_GROW_FAIL req_pages=%zu cur_pages_before=%zu pid=%d uid=%d\n",
            nslots, cur, (int)getpid(), (int)getuid());
        if (pn > 0) { ssize_t w = write(2, p, (size_t)pn); (void)w; }
        return NULL;
    }
    return (void *)(uintptr_t)(prev * WASM_PAGE);
}

/* -------------------------------------------------------------------------
 * Slab metadata management
 * ---------------------------------------------------------------------- */
static int meta_find_by_base(uint8_t *base) {
    for (int i = 0; i < g_meta_count; i++) {
        if (g_meta[i].base == base) return i;
    }
    return -1;
}

static int meta_alloc(void) {
    /* Prefer a reusable slot (base==NULL) */
    for (int i = 0; i < g_meta_count; i++) {
        if (g_meta[i].base == NULL) return i;
    }
    if (g_meta_count >= MAX_SLABS) return -1;
    return g_meta_count++;
}

static void partial_add(int idx) {
    if (g_meta[idx].in_partial) return;
    g_meta[idx].next_partial = g_partial;
    g_partial = idx;
    g_meta[idx].in_partial = 1;
}

static void partial_remove(int idx) {
    if (!g_meta[idx].in_partial) return;
    int *cur = &g_partial;
    while (*cur != -1) {
        if (*cur == idx) { *cur = g_meta[idx].next_partial; break; }
        cur = &g_meta[*cur].next_partial;
    }
    g_meta[idx].in_partial = 0;
    g_meta[idx].next_partial = -1;
}

/* -------------------------------------------------------------------------
 * Allocate a single 4KB slot from an existing partial slab or grow new
 * ---------------------------------------------------------------------- */
static uint8_t *alloc_4k(void) {
    /* Scan partial list for a slab with a free slot */
    int idx = g_partial;
    while (idx != -1) {
        SlabMeta *m = &g_meta[idx];
        if (m->nslots == 1 && m->used != 0xFFFF) {
            /* find lowest free slot */
            uint16_t free_mask = (uint16_t)~m->used;
            int slot = __builtin_ctz(free_mask);  /* 0..15 */
            m->used |= (uint16_t)(1u << slot);
            if (m->used == 0xFFFF) partial_remove(idx);
            return m->base + slot * SLOT_SIZE;
        }
        idx = m->next_partial;
    }
    /* No partial slab available — grow a new wasm page */
    uint8_t *base = (uint8_t *)bw_grow(1);
    if (!base) return NULL;
    int mi = meta_alloc();
    if (mi < 0) return NULL;
    SlabMeta *m = &g_meta[mi];
    m->base       = base;
    m->used       = 0x0001;   /* slot 0 in use */
    m->nslots     = 1;
    m->in_partial = 0;
    m->next_partial = -1;
    partial_add(mi);          /* add to partial (15 free slots remain) */
    return base;              /* slot 0 */
}

/* -------------------------------------------------------------------------
 * Allocate n consecutive 4KB slots (n <= 16) in one slab
 * ---------------------------------------------------------------------- */
static uint8_t *alloc_nslots_in_slab(int n) {
    uint16_t run_mask = (uint16_t)((1u << n) - 1u);
    /* Scan partial list */
    int idx = g_partial;
    while (idx != -1) {
        SlabMeta *m = &g_meta[idx];
        if (m->nslots == 1) {
            /* try each alignment */
            for (int start = 0; start + n <= (int)SLOTS_PER_SLAB; start++) {
                uint16_t mask = (uint16_t)(run_mask << start);
                if ((m->used & mask) == 0) {
                    m->used |= mask;
                    if (m->used == 0xFFFF) partial_remove(idx);
                    else if (n == (int)SLOTS_PER_SLAB) partial_remove(idx);
                    return m->base + start * SLOT_SIZE;
                }
            }
        }
        idx = m->next_partial;
    }
    /* Grow a fresh slab */
    uint8_t *base = (uint8_t *)bw_grow(1);
    if (!base) return NULL;
    int mi = meta_alloc();
    if (mi < 0) return NULL;
    SlabMeta *m = &g_meta[mi];
    m->base       = base;
    m->used       = run_mask;  /* first n slots in use */
    m->nslots     = 1;
    m->in_partial = 0;
    m->next_partial = -1;
    if (m->used != 0xFFFF) partial_add(mi);
    return base;
}

/* -------------------------------------------------------------------------
 * Allocate multiple contiguous wasm pages (for requests > 64KB)
 * ---------------------------------------------------------------------- */
static uint8_t *alloc_big(size_t nwasm_pages) {
    /* Check big-free list for exact size match */
    BigFree **prev = &g_big_free;
    BigFree  *cur  = g_big_free;
    while (cur) {
        if (cur->nwasm_pages == nwasm_pages) {
            *prev = cur->next;
            uint8_t *p = cur->base;
            free(cur);
            return p;
        }
        prev = &cur->next;
        cur  = cur->next;
    }
    return (uint8_t *)bw_grow(nwasm_pages);
}

/* -------------------------------------------------------------------------
 * Instrumentation counters
 * ---------------------------------------------------------------------- */
unsigned long      bw_shim_mmap_count        = 0;
unsigned long      bw_shim_malloc_fail_count = 0;
unsigned long long bw_shim_requested_bytes   = 0;
unsigned long long bw_shim_allocated_bytes   = 0;
unsigned long long bw_shim_largest_request   = 0;

void bw_dump_shim_stats(void) {
    char b[512];
    unsigned long long req   = bw_shim_requested_bytes;
    unsigned long long alloc = bw_shim_allocated_bytes;
    unsigned long long ratio = req ? (alloc * 1000ull) / req : 0ull;
    int n = snprintf(b, sizeof(b),
        "\n[SUBP-SHIM] mmapCount=%lu mallocFailCount=%lu\n"
        "[SUBP-SHIM] mappedGuestBytes=%llu realBackingBytes=%llu\n"
        "[SUBP-SHIM] largestRequestBytes=%llu\n"
        "[SUBP-SHIM] overheadRatio_milli=%llu (realBacking/mappedGuest x1000)\n",
        bw_shim_mmap_count, bw_shim_malloc_fail_count,
        req, alloc, bw_shim_largest_request, ratio);
    if (n > 0) { ssize_t w = write(2, b, (size_t)n); (void)w; }
}

/* -------------------------------------------------------------------------
 * mmap
 * ---------------------------------------------------------------------- */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
    (void)addr; (void)prot; (void)flags;

    if (length == 0) length = 1;

    bw_shim_mmap_count++;
    bw_shim_requested_bytes += (unsigned long long)length;
    if ((unsigned long long)length > bw_shim_largest_request)
        bw_shim_largest_request = (unsigned long long)length;

    if ((bw_shim_mmap_count % 512) == 0) {
        char p[200];
        int pn = snprintf(p, sizeof(p),
            "[SUBP-PROG] mmaps=%lu mappedGuestMB=%llu realSlabMB=%llu fails=%lu\n",
            bw_shim_mmap_count,
            bw_shim_requested_bytes  / (1024ull * 1024ull),
            bw_shim_allocated_bytes  / (1024ull * 1024ull),
            bw_shim_malloc_fail_count);
        if (pn > 0) { ssize_t w = write(2, p, (size_t)pn); (void)w; }
    }

    void *data;
    size_t rounded = (length + SLOT_SIZE - 1) & ~(size_t)(SLOT_SIZE - 1);
    int    nslots  = (int)(rounded / SLOT_SIZE);  /* 1..N */

    if (nslots == 1) {
        data = alloc_4k();
        bw_shim_allocated_bytes += SLOT_SIZE;
    } else if (nslots <= (int)SLOTS_PER_SLAB) {
        data = alloc_nslots_in_slab(nslots);
        bw_shim_allocated_bytes += (unsigned long long)nslots * SLOT_SIZE;
    } else {
        size_t npages = (rounded + WASM_PAGE - 1) / WASM_PAGE;
        data = alloc_big(npages);
        bw_shim_allocated_bytes += npages * WASM_PAGE;
    }

    if (!data) {
        bw_shim_malloc_fail_count++;
        errno = ENOMEM;
        return MAP_FAILED;
    }

    /* wasm zero-inits fresh pages; zero reused slots for correctness */
    memset(data, 0, rounded);

    /* File-backed: read fd content into slot */
    if (fd >= 0 && length > 0) {
        off_t saved = lseek(fd, 0, SEEK_CUR);
        if (saved != (off_t)-1) {
            if (lseek(fd, offset, SEEK_SET) != (off_t)-1) {
                size_t got = 0;
                ssize_t r;
                const size_t chunk = 4096u;
                while (got < length) {
                    size_t want = length - got;
                    if (want > chunk) want = chunk;
                    r = read(fd, (char *)data + got, want);
                    if (r <= 0) break;
                    got += (size_t)r;
                }
            }
            (void)lseek(fd, saved, SEEK_SET);
        }
    }

    return data;
}

/* -------------------------------------------------------------------------
 * munmap
 * ---------------------------------------------------------------------- */
int munmap(void *addr, size_t length) {
    if (!addr || addr == MAP_FAILED) return 0;

    uint8_t *ptr     = (uint8_t *)addr;
    uint8_t *slab_base = (uint8_t *)((uintptr_t)ptr & ~(uintptr_t)(WASM_PAGE - 1));
    int       slot0    = (int)((ptr - slab_base) / SLOT_SIZE);
    size_t    rounded  = (length + SLOT_SIZE - 1) & ~(size_t)(SLOT_SIZE - 1);
    int       nslots   = (int)(rounded / SLOT_SIZE);

    if (nslots <= (int)SLOTS_PER_SLAB) {
        int idx = meta_find_by_base(slab_base);
        if (idx >= 0) {
            SlabMeta *m = &g_meta[idx];
            uint16_t mask = (uint16_t)(((1u << nslots) - 1u) << slot0);
            int was_full = (m->used == 0xFFFF);
            m->used &= (uint16_t)~mask;
            if (was_full && m->used != 0xFFFF) partial_add(idx);
            if (m->used == 0) {
                partial_remove(idx);
                m->base = NULL;  /* slab slot reusable */
                /* wasm pages can't be returned to OS; just track for reuse */
            }
        }
        /* if not found in meta (e.g. a big-alloc that fits in one wasm page):
           fall through and treat as big-free below */
    }

    if (nslots > (int)SLOTS_PER_SLAB) {
        size_t npages = (rounded + WASM_PAGE - 1) / WASM_PAGE;
        BigFree *bf = (BigFree *)malloc(sizeof(BigFree));
        if (bf) {
            bf->base        = slab_base;
            bf->nwasm_pages = npages;
            bf->next        = g_big_free;
            g_big_free      = bf;
        }
    }

    return 0;
}

int mprotect(void *addr, size_t length, int prot) {
    (void)addr; (void)length; (void)prot;
    return 0;
}

int msync(void *addr, size_t length, int flags) {
    (void)addr; (void)length; (void)flags;
    return 0;
}

char *Demangle(char *p, const char *symbol, size_t n) {
    if (p && symbol && n) { strncpy(p, symbol, n - 1); p[n - 1] = 0; }
    return p;
}
