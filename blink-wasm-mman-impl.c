/* mmap/munmap/mprotect/msync per blink su wasm32 NO-MMU.
 *
 * blink e' un emulatore x86-64: usa mmap per ottenere blocchi di RAM per la
 * memoria del guest x86 (page table, arena, caricamento ELF). Su wasm32 NO-MMU
 * il kernel non offre mmap; implementiamo con malloc + allineamento manuale.
 *
 * Perche' non arena: il __simple_malloc di musl-wasm32 (bump allocator NO-MMU)
 * ha un heap limitato (~16-64MB). Un pre-alloc (256MB) fallisce → arena_base=NULL
 * → ogni mmap fallisce → MAP_FAILED → blink crash. Meglio allocazioni singole.
 *
 * munmap usa header tracking: prima dell'area allineata salviamo il raw pointer
 * malloc, cosi' free() libera correttamente la memoria.
 *
 * File-backed (fd >= 0): read best-effort al chunk dall'offset richiesto.
 * Se fallisce, il buffer rimane zero-init.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>   /* for MAP_FAILED (definito prima del guard __wasm__) */

/* Dimensione di pagina dinamica: sysconf(_SC_PAGESIZE).
 * Su arch/wasm = 65536 (WASM page), ma ci allineiamo al minimo per non sprecare.
 * Il waste medio e' page/2 per chiamata, che su 65536 e' ~32KB. */
static size_t bw_page_size(void) {
    long p = sysconf(_SC_PAGESIZE);
    return (p > 0) ? (size_t)p : 4096;
}

static size_t bw_round_up(size_t n, size_t page) {
    return (n + page - 1) & ~(page - 1);
}

/* --- M114-B2-P probe: shim allocation accounting -----------------------------
 * These monotonic counters expose the eager-commit / overhead behaviour of the
 * software-MMU backing allocator at runtime, so the ENOMEM crash of Node (99MB
 * guest) can be discriminated between:
 *   B  eager commit          -> committed pages balloon; realBackingBytes >> mappedGuestBytes
 *   C  heap fragmentation     -> malloc fails while committed is modest
 * The dump is emitted at the ENOMEM panic (see memorymalloc.c). Guarded by the
 * same NDEBUG story as blink's -Z stats: present in the wasm build (no -DNDEBUG). */
unsigned long bw_shim_mmap_count            = 0;  /* number of mmap() calls           */
unsigned long bw_shim_malloc_fail_count     = 0;  /* malloc() returning NULL (ENOMEM) */
unsigned long long bw_shim_requested_bytes  = 0;  /* sum of guest `length` requested  */
unsigned long long bw_shim_allocated_bytes  = 0;  /* sum of real malloc(need) backing */
unsigned long long bw_shim_largest_request  = 0;  /* largest single guest `length`    */
unsigned long long bw_shim_align_waste_bytes = 0; /* estimated per-call alignment waste */

/* Emitted at the ENOMEM panic. Uses only signal-safe primitives (snprintf into a
 * stack buffer + a single write(2)) so it is safe from an abort/panic context. */
void bw_dump_shim_stats(void) {
    char b[512];
    unsigned long long req = bw_shim_requested_bytes;
    unsigned long long alloc = bw_shim_allocated_bytes;
    /* overhead ratio *1000 (avoid float in panic path); realBacking/mappedGuest */
    unsigned long long ratio_milli = req ? (alloc * 1000ull) / req : 0ull;
    int n = snprintf(b, sizeof(b),
        "\n[B2P-SHIM] mmapCount=%lu mallocFailCount=%lu\n"
        "[B2P-SHIM] mappedGuestBytes=%llu realBackingBytes=%llu\n"
        "[B2P-SHIM] largestRequestBytes=%llu alignWasteBytes=%llu\n"
        "[B2P-SHIM] overheadRatio_milli=%llu (realBacking/mappedGuest x1000)\n",
        bw_shim_mmap_count, bw_shim_malloc_fail_count,
        req, alloc, bw_shim_largest_request, bw_shim_align_waste_bytes,
        ratio_milli);
    if (n > 0) { ssize_t w = write(2, b, (size_t)n); (void)w; }
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
    (void)addr; (void)prot; (void)flags;

    size_t page = bw_page_size();
    size_t len = length;
    if (len == 0) len = 1;
    len = bw_round_up(len, page);
    if (len < page) len = page;

    /* Header tracking: salviamo il raw pointer malloc prima dell'area allineata.
     * Allochiamo: [8 byte raw_ptr] [padding] [area allineata di len bytes] */
    size_t hdr  = sizeof(void *);
    size_t need = hdr + len + page;

    /* --- M114-B2-P probe accounting (before malloc so failures are counted) --- */
    bw_shim_mmap_count++;
    bw_shim_requested_bytes += (unsigned long long)length;
    bw_shim_allocated_bytes += (unsigned long long)need;
    if ((unsigned long long)length > bw_shim_largest_request)
        bw_shim_largest_request = (unsigned long long)length;
    /* waste = real backing minus the guest bytes actually requested */
    if (need > length) bw_shim_align_waste_bytes += (unsigned long long)(need - length);

    /* Periodic progress trace: converts an otherwise opaque MUG-loop stall into
     * visible evidence of committed bytes climbing (proves B: eager per-chunk
     * commit). Emitted every 512 mmap calls to keep console noise bounded. */
    if ((bw_shim_mmap_count % 512) == 0) {
        char p[192];
        int pn = snprintf(p, sizeof(p),
            "[B2P-PROG] mmaps=%lu mappedGuestMB=%llu realBackingMB=%llu fails=%lu\n",
            bw_shim_mmap_count,
            bw_shim_requested_bytes / (1024ull * 1024ull),
            bw_shim_allocated_bytes / (1024ull * 1024ull),
            bw_shim_malloc_fail_count);
        if (pn > 0) { ssize_t w = write(2, p, (size_t)pn); (void)w; }
    }

    void  *raw  = malloc(need);
    if (!raw) { bw_shim_malloc_fail_count++; errno = ENOMEM; return MAP_FAILED; }

    /* Allinea in avanti di page, con spazio per l'header */
    uintptr_t raw_start = (uintptr_t)raw + hdr;
    uintptr_t aligned   = (raw_start + page - 1) & ~(page - 1);
    void     *data      = (void *)aligned;

    /* Salva raw pointer nell'header (subito prima dell'area allineata) */
    ((void **)data)[-1] = raw;

    /* Zero-init */
    memset(data, 0, len);

    /* File-backed: read dall'fd all'offset richiesto.
     * blink carica segmenti ELF dal file binario con mmap file-backed.
     * Chunking a 64KB: una singola read enorme (es. 99MB per il binario Node)
     * fa stallare il VFS/virtio-blk NO-MMU del kernel wasm; read piccole no. */
    if (fd >= 0 && length > 0) {
        off_t saved = lseek(fd, 0, SEEK_CUR);
        if (saved != (off_t)-1) {
            if (lseek(fd, offset, SEEK_SET) != (off_t)-1) {
                size_t got = 0;
                ssize_t r;
                const size_t chunk = 64u << 10; /* 64KB per read (under virtio-blk ring capacity) */
                while (got < length) {
                    size_t want = length - got;
                    if (want > chunk) want = chunk;
                    r = read(fd, (char *)data + got, want);
                    if (r <= 0) break;
                    got += (size_t)r;
                }
            }
            /* restore posizione originale fd */
            (void)lseek(fd, saved, SEEK_SET);
        }
    }

    return data;
}

int munmap(void *addr, size_t length) {
    (void)length;
    if (!addr) return 0;
    /* raw pointer e' salvato subito prima dell'area allineata */
    void *raw = ((void **)addr)[-1];
    free(raw);
    return 0;
}

int mprotect(void *addr, size_t length, int prot) {
    (void)addr; (void)length; (void)prot;
    return 0;  /* No-op: wasm non ha protezione pagine */
}

int msync(void *addr, size_t length, int flags) {
    (void)addr; (void)length; (void)flags;
    return 0;  /* No-op: niente pagine dirty da sync */
}

/* Demangle stub per link (oneoff/demangle.c escluso dalla build). */
char *Demangle(char *p, const char *symbol, size_t n) {
    if (p && symbol && n) { strncpy(p, symbol, n - 1); p[n - 1] = 0; }
    return p;
}
