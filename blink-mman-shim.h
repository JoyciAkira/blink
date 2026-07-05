#ifndef BLINK_MMAN_SHIM_H
#define BLINK_MMAN_SHIM_H
/* Linux mman constants for blink's SOFTWARE MMU (guest x86 memory emulation).
   musl-wasm32 hides these under #ifndef __wasm__ (NO-MMU kernel). blink needs
   them as plain numeric values; they never reach the host wasm kernel mmap. */
#include <sys/mman.h>
#ifndef PROT_NONE
#define PROT_NONE  0
#endif
#ifndef PROT_READ
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#endif
#ifndef MAP_SHARED
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANON      0x20
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_GROWSDOWN
#define MAP_GROWSDOWN 0x0100
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif
#endif

/* --- appended: mmap/munmap decls + msync flags (hidden under __wasm__ in musl) --- */
#ifndef MS_ASYNC
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4
#endif
#ifdef __wasm__
#include <stddef.h>
void *mmap(void *, size_t, int, int, int, long);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
int msync(void *, size_t, int);
#endif

/* fork() hidden under __wasm__ in musl unistd.h; impl exists (clone-based). */
#ifdef __wasm__
#include <sys/types.h>
pid_t fork(void);
#endif
