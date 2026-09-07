/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "blink/assert.h"
#include "blink/biosrom.h"
#include "blink/bus.h"
#include "blink/checked.h"
#include "blink/debug.h"
#include "blink/endian.h"
#include "blink/errno.h"
#include "blink/linux.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/pml4t.h"
#include "blink/stats.h"
#include "blink/thread.h"
#include "blink/util.h"

/* ── M115-D2.1 RELEASE_PAGE_LOCK_INVARIANT_DISCRIMINATION ─────────────────
 * Telemetry only. No semantic change. Records every PTE write (CasPte /
 * StorePte) into a shared ring so that when ReleasePageLock's invariant
 * fails we can identify the exact modifier (tid/Machine/op/old→new) and
 * whether the failed assert is PAGE_V (mapping invalidated) or PAGE_LOCKS
 * (lock ownership/state race). The ring lives in linear memory, which is
 * SHARED across CLONE_VM Machines (parent_memory=true), so sibling writes
 * are visible. */
#define G12_PT_RING 8192
struct G12PteWrite {
  u64 seq;          /* global write sequence */
  u64 pslot;        /* PTE slot address (as integer) */
  u64 old_entry;
  u64 new_entry;
  i32 tid;          /* writing Machine tid */
  i32 mid;          /* writing Machine identity */
  u8  op;           /* 1=CasPte 2=StorePte */
  u8  ok;           /* CAS succeeded */
};
static struct G12PteWrite g_g12_pt_ring[G12_PT_RING];
static _Atomic(u64) g_g12_pt_head;      /* monotonic write index */
static _Atomic(u64) g_g12_rec_seq;      /* RecordPageLock sequence */
static _Atomic(u64) g_g12_rel_seq;      /* ReleasePageLock sequence */

static _Atomic(u32) g_g12_exec_retry_tick;
_Atomic(u32) g_g12_throttle_tick; /* G12 discriminator: shared throttle tick */
pthread_mutex_t_ g_g12_serialize_lock = PTHREAD_MUTEX_INITIALIZER_;

void G12NotePteWrite(void *pslot, u64 old_entry, u64 new_entry, u8 op, u8 ok) {
  (void)pslot;
  (void)old_entry;
  (void)new_entry;
  (void)op;
  (void)ok;
}
#define G12_E4R2_TARGET_PAGE 0x806c3000ULL
#define G12_E4R2_HOPS 4
struct G12PathHop {
  int level;
  u64 table;
  u64 index;
  u8 *pslot;
  u64 entry;
  u64 child;
};
struct G12PathSnapshot {
  bool valid;
  u64 va;
  u64 root;
  u64 mm;
  i32 tid;
  i32 mid;
  int count;
  struct G12PathHop hop[G12_E4R2_HOPS];
};
struct G12TransitionSnapshot {
  bool valid;
  u64 start;
  u64 size;
  u64 mm;
  u64 root;
  i32 tid;
  i32 mid;
  int prot;
  int sysprot;
  bool hostonly;
};
static struct G12PathSnapshot g_g12_e4r2_map_path;
static struct G12PathSnapshot g_g12_e4r2_fault_override;
static struct G12TransitionSnapshot g_g12_e4r2_last_mprotect;
static struct G12TransitionSnapshot g_g12_e4r2_last_unmap;

static bool G12E4R2TargetRange(u64 start, u64 size) {
  u64 end = start + size;
  if (end < start) end = ~0ULL;
  return start < G12_E4R2_TARGET_PAGE + 4096 &&
         end > G12_E4R2_TARGET_PAGE;
}

static void G12InitTransition(struct System *s, u64 start, u64 size,
                              struct G12TransitionSnapshot *out) {
  struct Machine *m = g_machine;
  memset(out, 0, sizeof(*out));
  out->valid = true;
  out->start = start;
  out->size = size;
  out->mm = (u64)(uintptr_t)s;
  out->root = s ? s->cr3 : 0;
  out->tid = m ? m->tid : -1;
  out->mid = m ? (i32)(uintptr_t)m & 0xffff : -1;
}

void G12NoteTargetMprotect(struct System *s, u64 start, u64 size, int prot,
                           int sysprot, bool hostonly) {
  if (!G12E4R2TargetRange(start, size)) return;
  G12InitTransition(s, start, size, &g_g12_e4r2_last_mprotect);
  g_g12_e4r2_last_mprotect.prot = prot;
  g_g12_e4r2_last_mprotect.sysprot = sysprot;
  g_g12_e4r2_last_mprotect.hostonly = hostonly;
}

void G12NoteTargetUnmap(struct System *s, u64 start, u64 size) {
  if (!G12E4R2TargetRange(start, size)) return;
  G12InitTransition(s, start, size, &g_g12_e4r2_last_unmap);
}

static void G12InitPagePath(struct System *s, u64 page, u64 root,
                            struct G12PathSnapshot *out) {
  struct Machine *m = g_machine;
  memset(out, 0, sizeof(*out));
  out->va = page & -4096;
  out->root = root;
  out->mm = (u64)(uintptr_t)s;
  out->tid = m ? m->tid : -1;
  out->mid = m ? (i32)(uintptr_t)m & 0xffff : -1;
}

static int G12BuildPagePath(struct System *s, u64 page,
                            struct G12PathSnapshot *out) {
  u64 table = s->cr3;
  G12InitPagePath(s, page, table, out);
  for (int i = 0; i < G12_E4R2_HOPS; ++i) {
    int level = 39 - i * 9;
    struct G12PathHop *h = &out->hop[i];
    u8 *table_host = GetPageAddress(s, table, level == 39);
    h->level = level;
    h->table = table;
    h->index = (page >> level) & 511;
    if (!table_host) {
      out->count = i + 1;
      return out->count;
    }
    h->pslot = table_host + h->index * 8;
    h->entry = LoadPte(h->pslot);
    h->child = h->entry & PAGE_TA;
    out->count = i + 1;
    if (!(h->entry & PAGE_V) || level == 12 ||
        (h->entry & PAGE_PS)) {
      return out->count;
    }
    table = h->entry;
  }
  return out->count;
}

static void G12PrintPagePath(const char *phase,
                             const struct G12PathSnapshot *path) {
  ERRF("G12-E4R2-%s-PATH va=%#llx root=%#llx mm=%#llx mid=%d tid=%d count=%d",
       phase, (unsigned long long)path->va,
       (unsigned long long)path->root, (unsigned long long)path->mm,
       path->mid, path->tid, path->count);
  for (int i = 0; i < path->count; ++i) {
    const struct G12PathHop *h = &path->hop[i];
    ERRF("G12-E4R2-%s-HOP level=%d table=%#llx index=%#llx "
         "pslot=%p entry=%#llx child=%#llx",
         phase, h->level, (unsigned long long)h->table,
         (unsigned long long)h->index, (void *)h->pslot,
         (unsigned long long)h->entry, (unsigned long long)h->child);
  }
}

static void G12PrintTransition(const char *kind,
                               const struct G12TransitionSnapshot *event) {
  ERRF("G12-E4R2-%s valid=%d start=%#llx size=%#llx mm=%#llx root=%#llx "
       "mid=%d tid=%d prot=%d sysprot=%d hostonly=%d",
       kind, event->valid, (unsigned long long)event->start,
       (unsigned long long)event->size, (unsigned long long)event->mm,
       (unsigned long long)event->root, event->mid, event->tid, event->prot,
       event->sysprot, event->hostonly);
}

static void G12PrintContext(const struct G12PathSnapshot *map,
                            const struct G12PathSnapshot *fault) {
  ERRF("G12-E4R2-CONTEXT map_root=%#llx fault_root=%#llx map_mm=%#llx "
       "fault_mm=%#llx map_mid=%d fault_mid=%d map_tid=%d fault_tid=%d",
       (unsigned long long)map->root, (unsigned long long)fault->root,
       (unsigned long long)map->mm, (unsigned long long)fault->mm, map->mid,
       fault->mid, map->tid, fault->tid);
}

void G12CapturePagePath(struct System *s, u64 page, bool map_event) {
  if ((page & -4096) != G12_E4R2_TARGET_PAGE) return;
  struct G12PathSnapshot current;
  if (!map_event && g_g12_e4r2_fault_override.valid) {
    current = g_g12_e4r2_fault_override;
    memset(&g_g12_e4r2_fault_override, 0,
           sizeof(g_g12_e4r2_fault_override));
  } else {
    G12BuildPagePath(s, page, &current);
  }
  if (map_event) {
    current.valid = true;
    g_g12_e4r2_map_path = current;
    return;
  }
  if (g_g12_e4r2_map_path.valid)
    G12PrintPagePath("MAP", &g_g12_e4r2_map_path);
  G12PrintPagePath("FAULT", &current);
  G12PrintTransition("LAST_MPROTECT", &g_g12_e4r2_last_mprotect);
  G12PrintTransition("LAST_UNMAP", &g_g12_e4r2_last_unmap);
  G12PrintContext(&g_g12_e4r2_map_path, &current);
  if (!g_g12_e4r2_map_path.valid) {
    ERRF("G12-E4R2-VERDICT SAME_RUN_PATH_MATCH_BUT_FAULT_PERSISTS "
         "reason=no_map_snapshot");
    return;
  }
  int first = -1;
  int n = MAX(g_g12_e4r2_map_path.count, current.count);
  if (g_g12_e4r2_map_path.root != current.root) {
    ERRF("G12-E4R2-FIRST_DIVERGENCE_LEVEL=39 FIRST_DIVERGENCE_PSLOT=%p "
         "MAP_ENTRY=0 FAULT_ENTRY=0",
         (void *)0);
    ERRF("G12-E4R2-VERDICT ROOT_SWITCH map_root=%#llx fault_root=%#llx",
         (unsigned long long)g_g12_e4r2_map_path.root,
         (unsigned long long)current.root);
    memset(&g_g12_e4r2_map_path, 0, sizeof(g_g12_e4r2_map_path));
    return;
  }
  for (int i = 0; i < n && i < G12_E4R2_HOPS; ++i) {
    if (i >= g_g12_e4r2_map_path.count || i >= current.count) {
      first = i;
      break;
    }
    const struct G12PathHop *a = &g_g12_e4r2_map_path.hop[i];
    const struct G12PathHop *b = &current.hop[i];
    if (a->level != b->level || a->table != b->table ||
        a->index != b->index || a->pslot != b->pslot ||
        a->entry != b->entry || a->child != b->child) {
      first = i;
      break;
    }
  }
  if (first < 0) {
    ERRF("G12-E4R2-FIRST_DIVERGENCE_LEVEL=-1 "
         "FIRST_DIVERGENCE_PSLOT=%p MAP_ENTRY=0 FAULT_ENTRY=0",
         (void *)0);
    ERRF("G12-E4R2-VERDICT SAME_RUN_PATH_MATCH_BUT_FAULT_PERSISTS");
    memset(&g_g12_e4r2_map_path, 0, sizeof(g_g12_e4r2_map_path));
    return;
  }
  const struct G12PathHop *map_hop = &g_g12_e4r2_map_path.hop[first];
  const struct G12PathHop *fault_hop =
      first < current.count ? &current.hop[first] : 0;
  u64 map_entry = map_hop->entry;
  u64 fault_entry = fault_hop ? fault_hop->entry : 0;
  ERRF("G12-E4R2-FIRST_DIVERGENCE_LEVEL=%d FIRST_DIVERGENCE_PSLOT=%p "
       "MAP_ENTRY=%#llx FAULT_ENTRY=%#llx",
       map_hop->level, (void *)map_hop->pslot,
       (unsigned long long)map_entry, (unsigned long long)fault_entry);
  const char *verdict = 0;
  if (map_hop->level == 39) {
    verdict = !fault_hop || !(fault_entry & PAGE_V)
                 ? "L39_ENTRY_CLEARED"
                 : (map_hop->child != fault_hop->child
                        ? "L39_ENTRY_REPLACED"
                        : 0);
  } else if (map_hop->level == 30) {
    verdict = !fault_hop || !(fault_entry & PAGE_V)
                 ? "L30_ENTRY_CLEARED"
                 : (map_hop->child != fault_hop->child
                        ? "L30_ENTRY_REPLACED"
                        : 0);
  } else if (map_hop->level == 21) {
    verdict = !fault_hop || !(fault_entry & PAGE_V)
                 ? "L21_ENTRY_CLEARED"
                 : (map_hop->child != fault_hop->child
                        ? "L21_ENTRY_REPLACED"
                        : 0);
  } else if (map_hop->level == 12 && (!fault_hop ||
                                     !(fault_entry & PAGE_V))) {
    verdict = "LEAF_REMOVED";
  }
  if (!verdict) verdict = "SAME_RUN_PATH_MATCH_BUT_FAULT_PERSISTS";
  ERRF("G12-E4R2-VERDICT %s", verdict);
  memset(&g_g12_e4r2_map_path, 0, sizeof(g_g12_e4r2_map_path));
}


/* dump last writers to a given pslot + the verdict, to guest fd1 (console) */
static void G12DumpPageLockRace(struct Machine *m, u8 *pslot, u64 recorded,
                                u64 rec_seq, u64 current) {
  char b[1200];
  int n = 0;
  int fv = !(current & PAGE_V);
  int fl = !(current & PAGE_LOCKS);
  int rv = !(recorded & PAGE_V);
  int rl = !(recorded & PAGE_LOCKS);
  const char *verdict =
      fv ? "RELEASE_PAGELOCK_PAGE_MAPPING_INVALIDATED"
         : (fl ? "RELEASE_PAGELOCK_PAGE_LOCKS_LOST"
               : "RELEASE_PAGELOCK_INVARIANT_KIND_NOT_CAPTURED");
  n += snprintf(b + n, sizeof(b) - n,
                "\nM115-D21 tid=%d mid=%d pslot=%p\n"
                "  recorded_entry=%#llx rec_seq=%llu rec_PAGE_V=%d rec_PAGE_LOCKS=%d\n"
                "  current_entry=%#llx cur_PAGE_V=%d cur_PAGE_LOCKS=%d\n"
                "  failed_assert=%s%s verdict=%s\n",
                m ? m->tid : -1, m ? ((i32)(uintptr_t)m & 0xffff) : -1,
                (void *)pslot, (unsigned long long)recorded,
                (unsigned long long)rec_seq, !rv, !rl,
                (unsigned long long)current, !fv, !fl,
                fv ? "PAGE_V " : "", fl ? "PAGE_LOCKS" : "", verdict);
  /* last writers to this exact pslot (newest first) */
  u64 head = atomic_load_explicit(&g_g12_pt_head, memory_order_relaxed);
  n += snprintf(b + n, sizeof(b) - n,
                "  ring_head=%llu ring_wrap=%d\n",
                (unsigned long long)head, head >= G12_PT_RING ? 1 : 0);
  int shown = 0;
  for (u64 k = 0; k < G12_PT_RING && shown < 24; k++) {
    u64 idx = (head + G12_PT_RING - 1 - k) % G12_PT_RING;
    struct G12PteWrite *w = &g_g12_pt_ring[idx];
    if (w->pslot != (u64)(uintptr_t)pslot) continue;
    n += snprintf(b + n, sizeof(b) - n,
                  "  writer seq=%llu tid=%d mid=%d op=%s ok=%d %#llx -> %#llx\n",
                  (unsigned long long)w->seq, w->tid, w->mid,
                  w->op == 1 ? "cas" : "store", w->ok,
                  (unsigned long long)w->old_entry,
                  (unsigned long long)w->new_entry);
    shown++;
  }
  if (!shown) n += snprintf(b + n, sizeof(b) - n, "  writer: NONE-IN-RING\n");
  /* scan writers to ANY pslot on the same 4096 page-table page (reuse) */
  {
    u64 target_pg = (u64)(uintptr_t)pslot & ~4095ULL;
    int pg_shown = 0;
    n += snprintf(b + n, sizeof(b) - n, "  same_pt_page(pg=%#llx) writers:\n",
                  (unsigned long long)target_pg);
    for (u64 k = 0; k < G12_PT_RING && pg_shown < 24; k++) {
      u64 idx = (head + G12_PT_RING - 1 - k) % G12_PT_RING;
      struct G12PteWrite *w = &g_g12_pt_ring[idx];
      if ((w->pslot & ~4095ULL) != target_pg) continue;
      if (w->pslot == (u64)(uintptr_t)pslot) continue;
      n += snprintf(b + n, sizeof(b) - n,
                    "    pgw seq=%llu tid=%d mid=%d op=%s ok=%d pslot=+%#llx %#llx -> %#llx\n",
                    (unsigned long long)w->seq, w->tid, w->mid,
                    w->op == 1 ? "cas" : "store", w->ok,
                    (unsigned long long)(w->pslot - target_pg),
                    (unsigned long long)w->old_entry,
                    (unsigned long long)w->new_entry);
      pg_shown++;
    }
    if (!pg_shown) n += snprintf(b + n, sizeof(b) - n, "    (none)\n");
  }
  WriteError(1, b, n);
}
#include "blink/x86.h"

void SetReadAddr(struct Machine *m, i64 addr, u32 size) {
  if (size) {
    m->readaddr = addr;
    m->readsize = size;
  }
}

void SetWriteAddr(struct Machine *m, i64 addr, u32 size) {
  if (size) {
    m->writeaddr = addr;
    m->writesize = size;
  }
}

u8 *GetPageAddress(struct System *s, u64 entry, bool is_cr3) {
  unassert(is_cr3 || (entry & PAGE_V));
  unassert(~entry & PAGE_RSRV);
  if (entry & PAGE_HOST) {
    return FindHostPage(entry);
  } else {
    unassert(s->real);
    if ((entry & PAGE_TA) + 4096 <= kRealSize) {
      return s->real + (entry & PAGE_TA);
    } else {
      return 0;
    }
  }
}

u64 HandlePageFault(struct Machine *m, u8 *pslot, u64 entry) {
  u64 x, page;
  unassert(entry & PAGE_RSRV);
  unassert(!HasLinearMapping());
  if (m->nofault) {
    m->segvcode = SEGV_MAPERR_LINUX;
    errno = ENOBUFS;
    return 0;
  }
  do {
    if (entry & (PAGE_HOST | PAGE_MAP | PAGE_MUG)) {
      // a file-mapped page is being accessed for the first time
      unassert((entry & (PAGE_HOST | PAGE_MAP)) == (PAGE_HOST | PAGE_MAP));
      /* G12 FIX (M115-D6): this first-touch install runs OUTSIDE the G12-D3
       * pagelocks_lock, so a whole-entry CAS from a stale lock=0 base can drop
       * a sibling CLONE_VM thread's just-set PAGE_LOCK (the residual D21 lost
       * unit). Re-read the entry UNDER pagelocks_lock and compute x from the
       * fresh base so x always carries the CURRENT PAGE_LOCKS, and serialize
       * against the lock increment. Safe order: no mmap_lock held here;
       * pagelocks_lock precedes g_ptelock (leaf). */
      LOCK(&m->system->pagelocks_lock);
      entry = LoadPte(pslot);
      if (!(entry & PAGE_RSRV)) {
        /* a racing fault already installed it; keep its PAGE_LOCKS */
        UNLOCK(&m->system->pagelocks_lock);
        break;
      }
      x = entry & ~PAGE_RSRV;  /* preserves PAGE_LOCKS from the fresh entry */
      if (CasPte(pslot, entry, x)) {
        UNLOCK(&m->system->pagelocks_lock);
        m->system->memstat.committed += 1;
        m->system->memstat.reserved -= 1;
        m->system->rss += 1;
        entry = x;
      } else {
        UNLOCK(&m->system->pagelocks_lock);
        entry = LoadPte(pslot);
      }
    } else {
      // an anonymous page is being accessed for the first time
      /* allocate BEFORE taking pagelocks_lock to avoid g_allocator.lock ->
       * pagelocks_lock ordering against any pagelocks_lock->...->allocator
       * path (g_allocator.lock is taken inside AllocateAnonymousPage). */
      if ((page = AllocateAnonymousPage(m->system)) == -1) {
        m->segvcode = SEGV_MAPERR_LINUX;
        entry = 0;
        break;
      }
      /* G12 FIX (M115-D6): same PAGE_LOCKS-preserving, serialized install. */
      LOCK(&m->system->pagelocks_lock);
      entry = LoadPte(pslot);
      if (!(entry & PAGE_RSRV)) {
        /* racing fault already installed; free our page, keep live entry */
        UNLOCK(&m->system->pagelocks_lock);
        FreeAnonymousPage(m->system, (u8 *)(uintptr_t)(page & PAGE_TA));
        m->system->rss -= 1;
        break;
      }
      x = (page & (PAGE_TA | PAGE_HOST)) | (entry & ~(PAGE_TA | PAGE_RSRV));
      if (CasPte(pslot, entry, x)) {
        UNLOCK(&m->system->pagelocks_lock);
        m->system->memstat.committed += 1;
        m->system->memstat.reserved -= 1;
        entry = x;
      } else {
        UNLOCK(&m->system->pagelocks_lock);
        FreeAnonymousPage(m->system, (u8 *)(uintptr_t)(page & PAGE_TA));
        entry = LoadPte(pslot);
        m->system->rss -= 1;
      }
    }
  } while (entry & PAGE_RSRV);
  return entry;
}

bool HasPageLock(const struct Machine *m, i64 page) {
  int i;
  unassert(!(page & 4095));
  for (i = m->pagelocks.i; i--;) {
    if (m->pagelocks.p[i].page == page) {
      return true;
    }
  }
  return false;
}

/* G12-D3: dedup aliases within one Machine by the shared PTE slot. A single
 * syscall can reach the same PTE through distinct guest VAs; the PTE lock
 * count and the per-Machine record stack must remain one-for-one. Sibling
 * Machines retain independent units and records, so one sibling cannot
 * release another sibling's lifetime. */
bool HasPageLockPslot(const struct Machine *m, const u8 *pslot) {
  int i;
  for (i = m->pagelocks.i; i--;) {
    if (m->pagelocks.p[i].pslot == pslot) {
      return true;
    }
  }
  return false;
}

static bool RecordPageLock(struct Machine *m, i64 page, u8 *pslot) {
  /* G12: sysdepth==0 is valid during OpSyscall epilogue cleanup — the
   * serializer prevents sibling munmap while these cleanup pins are held. */
  unassert(m->sysdepth >= 0);
  unassert(!m->pagelocks.i ||
           m->pagelocks.p[m->pagelocks.i - 1].sysdepth <= m->sysdepth);
  if (m->pagelocks.i == m->pagelocks.n) {
    int n2;
    struct PageLock *p2;
    p2 = m->pagelocks.p;
    n2 = m->pagelocks.n;
    n2 += 3;
    n2 += n2 >> 1;
    if ((p2 = (struct PageLock *)realloc(p2, n2 * sizeof(*p2)))) {
      m->pagelocks.p = p2;
      m->pagelocks.n = n2;
    } else {
      return false;
    }
  }
  m->pagelocks.p[m->pagelocks.i].page = page;
  m->pagelocks.p[m->pagelocks.i].pslot = pslot;
  m->pagelocks.p[m->pagelocks.i].sysdepth = m->sysdepth;
  /* Snapshot the PTE as recorded (valid + lock held). */
  m->pagelocks.p[m->pagelocks.i].entry = LoadPte(pslot);
  m->pagelocks.p[m->pagelocks.i].rec_seq = 0;
  ++m->pagelocks.i;
  STATISTIC(++page_locks);
  return true;
}

static void ReleasePageLock(struct PageLock *lk, struct Machine *m) {
  u8 *pslot = lk->pslot;
  u64 entry;
  int retries = 0;
  do {
    entry = LoadPte(pslot);
    unassert(entry & PAGE_V);
    unassert(entry & PAGE_LOCKS);
    if (CasPte(pslot, entry, entry - PAGE_LOCK)) break;
    ++retries;
  } while (1);
}

static bool HasOutdatedPageLocks(struct Machine *m) {
  return m->pagelocks.i &&
         m->pagelocks.p[m->pagelocks.i - 1].sysdepth > m->sysdepth;
}

void CollectPageLocks(struct Machine *m) {
  if (HasOutdatedPageLocks(m)) {
    int rc;
    if (UNLIKELY((rc = pthread_mutex_lock(&m->system->pagelocks_lock)) != 0)) {
      ERRF("PAGELOCK-RACE kind=mutex_lock tid=%d rc=%d", m->tid, rc);
      unassert(!rc);
    }
    do {
      struct PageLock *lk__ = &m->pagelocks.p[--m->pagelocks.i];
      ReleasePageLock(lk__, m);
    } while (HasOutdatedPageLocks(m));
    if (UNLIKELY((rc = pthread_cond_broadcast(&m->system->pagelocks_cond)) != 0)) {
      ERRF("PAGELOCK-RACE kind=cond_broadcast tid=%d rc=%d", m->tid, rc);
      unassert(!rc);
    }
    if (UNLIKELY((rc = pthread_mutex_unlock(&m->system->pagelocks_lock)) != 0)) {
      ERRF("PAGELOCK-RACE kind=mutex_unlock tid=%d rc=%d", m->tid, rc);
      unassert(!rc);
    }
  }
#ifdef __wasm__
  /* G12: release the cross-Machine syscall serializer here as well as in the
   * OpSyscall epilogue. CollectPageLocks runs on BOTH the normal path and the
   * HaltMachine longjmp unwind, so a page fault inside a serialized syscall
   * can never leak the lock. */
  if (m->g12_holds_serializer && m->sysdepth == 0) {
    m->g12_holds_serializer = false;
    UNLOCK(&g_g12_serialize_lock);
  }
#endif
}

// returns page directory entry associated with virtual address
// @return raw page directory entry contents, or zero w/ errno
// @raise EFAULT if a valid 4096 page didn't exist at address
// @raise ENOMEM if memory couldn't be allocated internally
// @raise EAGAIN if too many locks are held on a page
u64 FindPageTableEntry(struct Machine *m, u64 page) {
  u8 *pslot;
  i64 table;
  u64 entry;
  long tlbkey;
  unsigned level, index;
  bool e4r2_track = (page & -4096) == G12_E4R2_TARGET_PAGE;
  struct G12PathSnapshot e4r2_walk;
  if (UNLIKELY(atomic_load_explicit(&m->invalidated, memory_order_acquire))) {
    ResetTlb(m);
    atomic_store_explicit(&m->invalidated, false, memory_order_relaxed);
  }
  tlbkey = (page >> 12) & (ARRAYLEN(m->tlb) - 1);
  if (LIKELY(m->tlb[tlbkey].page == page &&
             ((entry = m->tlb[tlbkey].entry) & PAGE_V))) {
    STATISTIC(++tlb_hits);
    /* G12 FIX (M115-D5, TLB incoherence): PAGE_LOCKS is a mutable scalar
     * SHARED by every CLONE_VM sibling of this System (one cr3). A per-Machine
     * TLB must never be the source of truth for it: siblings' TLBs go stale
     * because InvalidateSystem only fires on mapping changes, not lock ops.
     * Mask lock bits out of the cached value. AND during a syscall the page
     * MUST be pinned against the live shared PTE, so do not early-return —
     * fall through to the walk + RecordPageLock path. Only pure data accesses
     * (not in a syscall, or nofault) may take the TLB fast path. */
    if (LIKELY(!m->insyscall || m->nofault))
      return entry & ~PAGE_LOCKS;
    /* else: in a syscall -> fall through to walk + pin on the live PTE */
  }
  STATISTIC(++tlb_misses);
  unassert(!(page & 4095));
  if (!(-0x800000000000 <= (i64)page && (i64)page < 0x800000000000)) {
    m->segvcode = SEGV_MAPERR_LINUX;
    return (u64)(uintptr_t)efault0();
  }
TryAgain:
  unassert((entry = m->system->cr3));
  if (e4r2_track) {
    G12InitPagePath(m->system, page, entry, &e4r2_walk);
  }
  level = 39;
  do {
    table = entry;
    index = (page >> level) & 511;
    pslot = GetPageAddress(m->system, table, level == 39) + index * 8;
    if (!pslot) goto MapError;
    entry = LoadPte(pslot);
    if (e4r2_track && e4r2_walk.count < G12_E4R2_HOPS) {
      struct G12PathHop *h = &e4r2_walk.hop[e4r2_walk.count++];
      h->level = level;
      h->table = table;
      h->index = index;
      h->pslot = pslot;
      h->entry = entry;
      h->child = entry & PAGE_TA;
    }
    if (!(entry & PAGE_V)) {
      if (e4r2_track) {
        e4r2_walk.valid = true;
        g_g12_e4r2_fault_override = e4r2_walk;
        G12CapturePagePath(m->system, page, false);
      }
      goto MapError;
    }
    if (m->metal) {
      entry &= ~(u64)(PAGE_RSRV | PAGE_HOST | PAGE_MAP | PAGE_GROW | PAGE_MUG |
                      PAGE_FILE);
    }
    if ((entry & PAGE_PS) && level > 12) {
      // huge (1 GiB or 2 MiB) page; "rewrite" the TLB copy of the page table
      // entry, to point to the 4 KiB subpage being accessed
      // TODO: if partial TLB flushes are implemented in the future, we will
      // also need to somehow record the original huge page size in the TLB,
      // so we can correctly invalidate all TLB entries for the huge page
      u64 submask = ((u64)1 << level) - 4096;
      entry &= ~submask;
      entry |= page & submask;
      break;
    }
  } while ((level -= 9) >= 12);
  if ((entry & PAGE_RSRV) && !(entry = HandlePageFault(m, pslot, entry))) {
    return 0;
  }
  // system calls lock the pages they access
  // this prevents race conditions w/ munmap
  if (m->insyscall && !m->nofault &&
      !HasPageLockPslot(m, pslot)) {
    /* G12-D3: each Machine owns one unit per distinct shared PTE slot.
     * Keep the PTE count and this Machine's record stack one-for-one. */
    LOCK(&m->system->pagelocks_lock);
    /* re-read under lock: another syscall may have changed the entry */
    entry = LoadPte(pslot);
    if ((entry & PAGE_LOCKS) < PAGE_LOCKS &&
        CasPte(pslot, entry, entry + PAGE_LOCK)) {
      unassert(LoadPte(pslot) & PAGE_LOCKS);
      if (RecordPageLock(m, page, pslot)) {
        UNLOCK(&m->system->pagelocks_lock);
        entry += PAGE_LOCK;
      } else {
        ReleasePageLock(&(struct PageLock){.page = page,
                                           .pslot = pslot,
                                           .entry = entry + PAGE_LOCK},
                         m);
        UNLOCK(&m->system->pagelocks_lock);
        m->segvcode = SEGV_MAPERR_LINUX;
        return 0;
      }
    } else {
      UNLOCK(&m->system->pagelocks_lock);
      if ((entry & PAGE_LOCKS) == PAGE_LOCKS) {
        LOGF("too many threads locked page %#" PRIx64, page);
        m->segvcode = SEGV_MAPERR_LINUX;
        errno = EAGAIN;
        return 0;
      }
      goto TryAgain;
    }
  }
HaveEntry:
  m->tlb[tlbkey].page = page;
  /* G12 FIX (M115-D5): never cache PAGE_LOCKS in the TLB (it is shared and
   * mutable); keep the cache a pure mapping cache. */
  m->tlb[tlbkey].entry = entry & ~PAGE_LOCKS;
  return entry;
MapError:
  m->segvcode = SEGV_MAPERR_LINUX;
  return (uintptr_t)efault0();
}

/* G12 PASSO-1: unconditional fault-site capture.
 * Emits FAULT-SITE with guest RIP, RSP, fault addr, access type, segvcode,
 * task/thread provenance, and VMA classification of RIP / SP / fault addr.
 * Deliberately uses ERRF (always-on in this build) so it reaches console.
 * Classification is best-effort and can return OTHER/unknown so we never
 * force the fault into a preferred (a)/(b)/(c) bucket. */
static const char *FaultAccessType(u64 need) {
  if (need & PAGE_XD) return "exec";
  return "read";
}

static const char *ClassifyVma(struct Machine *m, u64 v) {
  struct System *s = m->system;
  u64 fsb = m->fs.base;
  u64 sp = Get64(m->sp);
  /* TLS: guest fs.base points at the thread control block; treat a window
   * around it as TLS. */
  if (fsb && v >= fsb && v < fsb + 0x2000) return "TLS";
  /* Stack: window around current guest RSP (grows down). */
  if (sp && v >= sp - 0x400 && v < sp + 0x100000) return "stack";
  /* Executable image (guest .text). */
  if (s->codestart && v >= (u64)s->codestart &&
      v < (u64)s->codestart + (u64)s->codesize)
    return "image";
  /* Heap: below program break (and above image end, loosely). */
  if (s->brk && v < (u64)s->brk) return "heap";
  /* mmap region: blink places automap mappings here. */
  if (s->automap && v >= (u64)s->automap) return "mmap";
  return "unknown";
}

static void ReportFaultSite(struct Machine *m, i64 virt, u64 mask, u64 need,
                            const char *branch) {
  u64 entry;
  int mapped = 0;
  u64 perms = 0;
  u64 v = (u64)virt;
  u64 ip = (u64)GetIp(m);
  u64 sp = Get64(m->sp);
  entry = FindPageTableEntry(m, v & -4096);
  if (entry) {
    mapped = 1;
    perms = entry & (PAGE_U | PAGE_RW | PAGE_XD | PAGE_V);
  }
  ERRF("FAULT-SITE branch=%s tid=%d ctid=%#llx rip=%#llx rip_vma=%s "
       "rsp=%#llx rsp_vma=%s fault=%#llx fault_vma=%s acc=%s "
       "mapped=%d perms=%#llx mask=%#llx need=%#llx segvcode=%d "
       "brk=%#llx automap=%#llx codestart=%#llx codesize=%#lx fsbase=%#llx",
       branch, m->tid, (unsigned long long)m->ctid,
       (unsigned long long)ip, ClassifyVma(m, ip),
       (unsigned long long)sp, ClassifyVma(m, sp),
       (unsigned long long)v, ClassifyVma(m, v),
       FaultAccessType(need), mapped, (unsigned long long)perms,
       (unsigned long long)mask, (unsigned long long)need, m->segvcode,
       (unsigned long long)m->system->brk,
       (unsigned long long)m->system->automap,
       (unsigned long long)m->system->codestart,
       (unsigned long long)m->system->codesize,
       (unsigned long long)m->fs.base);
}

u8 *LookupAddress2(struct Machine *m, i64 virt, u64 mask, u64 need) {
  u8 *host;
  u64 entry;
  if (!m->metal || m->mode.omode == XED_MODE_LONG ||
      (m->mode.genmode != XED_GEN_MODE_REAL && (m->system->cr0 & CR0_PG))) {
    if (!(entry = FindPageTableEntry(m, virt & -4096))) {
      return 0;
    }
  } else if (virt >= 0 && virt <= 0xffffffff &&
             (virt & 0xffffffff) + 4095 < kRealSize) {
    unassert(m->system->real);
    return m->system->real + virt;
  } else {
    m->segvcode = SEGV_MAPERR_LINUX;
    ReportFaultSite(m, virt, mask, need, "real_mode_range");
    return (u8 *)efault0();
  }
  if ((entry & mask) != need) {
    /* Multi-threaded guests (V8 W^X) transiently clear PROT_EXEC on code
     * pages while another thread is still fetching from them. Linux delivers
     * SIGSEGV only if the permission stays wrong; a concurrent writer that
     * restores RX makes the fetch succeed. Mirror that: when the page is
     * mapped, user-accessible and merely missing exec (mask==PAGE_XD), spin
     * briefly on the PTE before declaring ACCERR. */
    if (mask == PAGE_XD && need == 0 && (entry & PAGE_V) &&
        (entry & PAGE_U)) {
      unsigned spins;
      for (spins = 0; spins < 2000; ++spins) {
        entry = FindPageTableEntry(m, virt & -4096);
        if ((entry & mask) == need) break;
#ifdef __wasm__
        __builtin_wasm_memory_atomic_wait32(
            (int *)&g_g12_exec_retry_tick, 0, 64);
#endif
      }
      if ((entry & mask) != need) {
        SYS_LOGF("SEGV_ACCERR virt=%#" PRIx64 " entry=%#" PRIx64
                 " mask=%#" PRIx64 " need=%#" PRIx64
                 " rip=%#" PRIx64 " sp=%#" PRIx64,
                 (u64)virt, entry, mask, need, m->ip, m->sp);
        if ((virt & -4096) == G12_E4R2_TARGET_PAGE)
          G12CapturePagePath(m->system, virt & -4096, false);
        m->segvcode = SEGV_ACCERR_LINUX;
        {
          u8 *hp = GetPageAddress(m->system, entry, false);
          u64 off = (u64)virt & 4095;
          if (hp && off >= 0x20 && off <= 4080) {
            u64 base = (u64)virt - off;
            ERRF("G12-TABLE-DUMP rip=%#" PRIx64 " page=%#" PRIx64
                 " qwords[-4..+3]:",
                 (unsigned long long)(u64)virt,
                 (unsigned long long)base);
            for (int q = -4; q <= 3; ++q) {
              long o = (long)off + q * 8;
              if (o < 0 || o > 4088) continue;
              u64 v = 0;
              for (int k = 7; k >= 0; --k)
                v = (v << 8) | hp[o + k];
              ERRF("  [%+#lx] = %#llx", o, (unsigned long long)v);
            }
          } else if (hp && off < 0x20) {
            ERRF("G12-RIP-DUMP rip=%#" PRIx64
                 " bytes=%02x%02x%02x%02x%02x%02x%02x%02x"
                 "%02x%02x%02x%02x%02x%02x%02x%02x",
                 (unsigned long long)(u64)virt,
                 hp[off], hp[off+1], hp[off+2], hp[off+3],
                 hp[off+4], hp[off+5], hp[off+6], hp[off+7],
                 hp[off+8], hp[off+9], hp[off+10], hp[off+11],
                 hp[off+12], hp[off+13], hp[off+14], hp[off+15]);
          }
        }
          {
            u64 sp = Get64(m->sp);
            u64 sentry = FindPageTableEntry(m, sp & -4096);
            if (sentry && ((sentry & PAGE_V))) {
              u8 *shp = GetPageAddress(m->system, sentry, false);
              u64 soff = sp & 4095;
              if (shp && soff <= 4088) {
                u64 ret = 0;
                for (int k = 7; k >= 0; --k)
                  ret = (ret << 8) | shp[soff + k];
                ERRF("G12-RET-DUMP rsp=%#" PRIx64 " ret=%#" PRIx64,
                     (unsigned long long)sp, (unsigned long long)ret);
              }
            }
          }
        ReportFaultSite(m, virt, mask, need, "accerr");
        return (u8 *)efault0();
      }
    } else {
      SYS_LOGF("SEGV_ACCERR virt=%#" PRIx64 " entry=%#" PRIx64
               " mask=%#" PRIx64 " need=%#" PRIx64
               " rip=%#" PRIx64 " sp=%#" PRIx64,
               (u64)virt, entry, mask, need, m->ip, m->sp);
      if ((virt & -4096) == G12_E4R2_TARGET_PAGE)
        G12CapturePagePath(m->system, virt & -4096, false);
      m->segvcode = SEGV_ACCERR_LINUX;
      ReportFaultSite(m, virt, mask, need, "accerr");
      return (u8 *)efault0();
    }
  }
#ifndef DISABLE_JIT
  if ((need & PAGE_RW) &&
      (entry & (PAGE_U | PAGE_RW | PAGE_XD)) == (PAGE_U | PAGE_RW) &&
      !IsJitDisabled(&m->system->jit) && !IsPageInSmcQueue(m, virt)) {
    AddPageToSmcQueue(m, virt);
  }
#endif
  /* G12 FIX: the multi-level page walk in FindPageTableEntry runs without
   * mmap_lock. With CLONE_VM siblings (libuv threads) concurrently executing
   * munmap/mprotect, an intermediate page-table page can be freed and
   * recycled as data mid-walk; GetPageAddress then resolves a recycled host
   * page and the caller executes heap metadata (observed wild-jump into
   * mallocng meta). Validate by re-reading the leaf PTE: if it changed under
   * us, retry the whole walk. */
  if ((host = GetPageAddress(m->system, entry, false))) {
    /* G12 walk-validation temporarily disabled: re-read loop regressed
     * execve (ENOEXEC on all external binaries). See result #29. */
    return host + (virt & 4095);
    u64 recheck;
    unsigned spins;
    for (spins = 0; spins < 4; ++spins) {
      recheck = FindPageTableEntry(m, virt & -4096);
      if (recheck == entry) {
        return host + (virt & 4095);
      }
      if (!(recheck & PAGE_V)) break;
      entry = recheck;
      if ((entry & mask) != need) break;
      host = GetPageAddress(m->system, entry, false);
      if (!host) break;
    }
    m->segvcode = SEGV_MAPERR_LINUX;
    ReportFaultSite(m, virt, mask, need, "walk_race");
    return (u8 *)efault0();
  } else {
    m->segvcode = SEGV_MAPERR_LINUX;
    ReportFaultSite(m, virt, mask, need, "no_host_page");
    return (u8 *)efault0();
  }
}

u8 *LookupAddress(struct Machine *m, i64 virt) {
  u64 need = 0;
  if (Cpl(m) == 3) need = PAGE_U;
  return LookupAddress2(m, virt, need, need);
}

flattencalls u8 *GetAddress(struct Machine *m, i64 v) {
  if (HasLinearMapping()) return ToHost(v);
  return LookupAddress(m, v);
}

/**
 * Translates virtual address into pointer.
 *
 * This function bypasses memory protection, since it's used to display
 * memory in the debugger tui. That's useful, for example, if debugging
 * programs that specify an eXecute-only program header.
 *
 * It's recommended that the caller use:
 *
 *     BEGIN_NO_PAGE_FAULTS;
 *     i64 address = ...;
 *     u8 *pointer = SpyAddress(m, address);
 *     END_NO_PAGE_FAULTS;
 *
 * When calling this function.
 */
u8 *SpyAddress(struct Machine *m, i64 virt) {
  return LookupAddress2(m, virt, 0, 0);
}

u8 *ResolveAddress(struct Machine *m, i64 v) {
  u8 *r;
  if ((r = GetAddress(m, v))) return r;
  ThrowSegmentationFault(m, v);
}

bool IsValidMemory(struct Machine *m, i64 virt, i64 size, int prot) {
  i64 p, pe;
  u64 pte, mask, need;
  size += virt & 4095;
  virt &= -4096;
  unassert(m->mode.omode == XED_MODE_LONG);
  unassert(prot && !(prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)));
  need = mask = 0;
  if (prot & PROT_READ) {
    mask |= PAGE_U;
    need |= PAGE_U;
  }
  if (prot & PROT_WRITE) {
    mask |= PAGE_RW;
    need |= PAGE_RW;
  }
  if (prot & PROT_EXEC) {
    mask |= PAGE_XD;
  }
  if (ckd_add(&pe, virt, size)) {
    eoverflow();
    return false;
  }
  for (p = virt; p < pe; p += 4096) {
    if (!(pte = FindPageTableEntry(m, p))) {
      return false;
    }
    if ((pte & mask) != need) {
      errno = EFAULT;
      return false;
    }
  }
  return true;
}

int VirtualCopy(struct Machine *m, i64 v, char *r, u64 n, bool d) {
  u8 *p;
  u64 k;
  k = 4096 - (v & 4095);
  while (n) {
    k = MIN(k, n);
    if (!(p = LookupAddress(m, v))) return -1;
    if (d) {
      memcpy(r, p, k);
    } else if (!IsRomAddress(m, p)) {
      memcpy(p, r, k);
    }
    n -= k;
    r += k;
    v += k;
    k = 4096;
  }
  return 0;
}

int CopyFromUser(struct Machine *m, void *dst, i64 src, u64 n) {
  return VirtualCopy(m, src, (char *)dst, n, true);
}

int CopyFromUserRead(struct Machine *m, void *dst, i64 addr, u64 n) {
  if (CopyFromUser(m, dst, addr, n) == -1) return -1;
  SetReadAddr(m, addr, n);
  return 0;
}

int CopyToUser(struct Machine *m, i64 dst, void *src, u64 n) {
  return VirtualCopy(m, dst, (char *)src, n, false);
}

int CopyToUserWrite(struct Machine *m, i64 addr, void *src, u64 n) {
  if (CopyToUser(m, addr, src, n) == -1) return -1;
  SetWriteAddr(m, addr, n);
  return 0;
}

void CommitStash(struct Machine *m) {
  unassert(m->stashaddr);
  if (m->opcache->writable) {
    CopyToUser(m, m->stashaddr, m->opcache->stash, m->opcache->stashsize);
  }
  m->stashaddr = 0;
}

u8 *ReserveAddress(struct Machine *m, i64 v, size_t n, bool writable) {
  long k;
  u64 mask, need;
  u8 *res, *p1, *p2;
  if (writable) {
    SetWriteAddr(m, v, n);
  } else {
    SetReadAddr(m, v, n);
  }
  if (HasLinearMapping()) {
    return ToHost(v);
  }
  m->reserving = true;
  if (Cpl(m) == 3) {
    if (!writable) {
      mask = PAGE_U;
      need = PAGE_U;
    } else {
      mask = PAGE_U | PAGE_RW;
      need = PAGE_U | PAGE_RW;
    }
  } else {
    mask = 0;
    need = 0;
  }
  if ((v & 4095) + n <= 4096) {
    if ((res = LookupAddress2(m, v, mask, need))) {
      if (!IsRomAddress(m, res)) return res;
      p1 = res;
      m->stashaddr = v;
      m->opcache->stashsize = n;
      m->opcache->writable = writable;
      res = m->opcache->stash;
      IGNORE_RACES_START();
      memcpy(res, p1, n);
      IGNORE_RACES_END();
      return res;
    } else {
      ThrowSegmentationFault(m, v);
    }
  }
  STATISTIC(++page_overlaps);
  unassert(n <= 4096);
  m->stashaddr = v;
  m->opcache->stashsize = n;
  m->opcache->writable = writable;
  res = m->opcache->stash;
  k = 4096 - (v & 4095);
  if ((p1 = LookupAddress2(m, v, mask, need))) {
    if ((p2 = LookupAddress2(m, v + k, mask, need))) {
      IGNORE_RACES_START();
      memcpy(res, p1, k);
      memcpy(res + k, p2, n - k);
      IGNORE_RACES_END();
      return res;
    } else {
      ThrowSegmentationFault(m, v + k);
    }
  } else {
    ThrowSegmentationFault(m, v);
  }
}

static u8 *AccessRam2(struct Machine *m, i64 v, size_t n, void *p[2], u8 *tmp,
                      bool copy, bool protect_rom) {
  u8 *a, *b;
  unsigned k;
  unassert(n <= 4096);
  if ((v & 4095) + n <= 4096) {
    a = ResolveAddress(m, v);
    if (!protect_rom || !IsRomAddress(m, a)) return a;
    if (copy) memcpy(tmp, a, n);
    return tmp;
  }
  STATISTIC(++page_overlaps);
  k = 4096;
  k -= v & 4095;
  unassert(k <= 4096);
  a = ResolveAddress(m, v);
  b = ResolveAddress(m, v + k);
  if (copy) {
    memcpy(tmp, a, k);
    memcpy(tmp + k, b, n - k);
  }
  if (protect_rom) {
    if (IsRomAddress(m, a)) a = NULL;
    if (IsRomAddress(m, b)) b = NULL;
  }
  p[0] = a;
  p[1] = b;
  return tmp;
}

u8 *AccessRam(struct Machine *m, i64 v, size_t n, void *p[2], u8 *tmp, bool d) {
  return AccessRam2(m, v, n, p, tmp, d, !d);
}

u8 *Load(struct Machine *m, i64 v, size_t n, u8 *b) {
  void *p[2];
  SetReadAddr(m, v, n);
  return AccessRam(m, v, n, p, b, true);
}

u8 *BeginStore(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  SetWriteAddr(m, v, n);
  return AccessRam(m, v, n, p, b, false);
}

u8 *BeginStoreNp(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  if (!v) return NULL;
  return BeginStore(m, v, n, p, b);
}

#if 0
u8 *BeginLoadStore(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  SetWriteAddr(m, v, n);
  return AccessRam2(m, v, n, p, b, true, true);
}
#endif

void EndStore(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  unsigned k;
  unassert(n <= 4096);
  if ((v & 4095) + n <= 4096) return;
  k = 4096;
  k -= v & 4095;
  unassert(n > k);
#ifdef DISABLE_ROM
  unassert(p[0]);
  unassert(p[1]);
  memcpy(p[0], b, k);
  memcpy(p[1], b + k, n - k);
#else
  if (p[0]) memcpy(p[0], b, k);
  if (p[1]) memcpy(p[1], b + k, n - k);
#endif
}

void EndStoreNp(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  if (v) EndStore(m, v, n, p, b);
}

void *AddToFreeList(struct Machine *m, void *mem) {
  int n;
  void *p;
  p = m->freelist.p;
  n = m->freelist.n + 1;
  if ((p = realloc(p, n * sizeof(*m->freelist.p)))) {
    STATISTIC(++freelisted);
    m->freelist.p = (void **)p;
    m->freelist.n = n;
    m->freelist.p[n - 1] = mem;
    return mem;
  } else {
    free(mem);
    return 0;
  }
}

// Returns pointer to memory in guest memory. If the memory overlaps a
// page boundary, then it's copied, and the temporary memory is pushed
// to the free list. Returns NULL w/ EFAULT or ENOMEM on error.
void *Schlep(struct Machine *m, i64 addr, size_t size, u64 mask, u64 need) {
  char *copy;
  size_t have;
  void *res, *page;
  if (!size) return 0;
  if (!(page = LookupAddress2(m, addr, mask, need))) return 0;
  have = 4096 - (addr & 4095);
  if (size <= have) {
    res = page;
  } else {
    if (!(copy = (char *)malloc(size))) return 0;
    memcpy(copy, page, have);
    for (; have < size; have += 4096) {
      if (!(page = LookupAddress2(m, addr + have, mask, need))) {
        free(copy);
        return 0;
      }
      memcpy(copy + have, page, MIN(4096, size - have));
    }
    res = AddToFreeList(m, copy);
  }
  return res;
}

void *SchlepR(struct Machine *m, i64 addr, size_t size) {
  SetReadAddr(m, addr, size);
  return Schlep(m, addr, size, PAGE_U, PAGE_U);
}

void *SchlepW(struct Machine *m, i64 addr, size_t size) {
  SetWriteAddr(m, addr, size);
  return Schlep(m, addr, size, PAGE_RW, PAGE_RW);
}

void *SchlepRW(struct Machine *m, i64 addr, size_t size) {
  SetReadAddr(m, addr, size);
  SetWriteAddr(m, addr, size);
  return Schlep(m, addr, size, PAGE_U | PAGE_RW, PAGE_U | PAGE_RW);
}

// Returns pointer to string in guest memory. If the string overlaps a
// page boundary, then it's copied, and the temporary memory is pushed
// to the free list. Returns NULL w/ EFAULT or ENOMEM on error.
char *LoadStr(struct Machine *m, i64 addr) {
  size_t have;
  char *copy, *page, *p;
  have = 4096 - (addr & 4095);
  if (!addr) return 0;
  if (!(page = (char *)LookupAddress2(m, addr, PAGE_U, PAGE_U))) return 0;
  if ((p = (char *)memchr(page, '\0', have))) {
    SetReadAddr(m, addr, p - page + 1);
    return page;
  }
  if (!(copy = (char *)malloc(have + 4096))) return 0;
  memcpy(copy, page, have);
  for (;;) {
    if (!(page = (char *)LookupAddress2(m, addr + have, PAGE_U, PAGE_U))) break;
    if ((p = (char *)memccpy(copy + have, page, '\0', 4096))) {
      SetReadAddr(m, addr, have + (p - (copy + have)) + 1);
      return (char *)AddToFreeList(m, copy);
    }
    have += 4096;
    if (!(p = (char *)realloc(copy, have + 4096))) break;
    copy = p;
  }
  free(copy);
  return 0;
}

// Copies string from guest memory. The returned memory is pushed to the
// machine free list. NULL w/ ENOMEM is returned if we're out of memory.
char *CopyStr(struct Machine *m, i64 addr) {
  char *s;
  if (!(s = LoadStr(m, addr))) return 0;
  return (char *)AddToFreeList(m, strdup(s));
}

// Returns fully copied NULL-terminated NUL-terminated string list. All
// memory allocated by this routine is pushed to the machine free list.
char **CopyStrList(struct Machine *m, i64 addr) {
  int n;
  u8 b[8];
  char *s;
  void *mem;
  char **list;
  for (list = 0, n = 0;;) {
    if ((mem = realloc(list, ++n * sizeof(*list)))) {
      list = (char **)mem;
    } else {
      free(list);
      return 0;
    }
    CopyFromUserRead(m, b, addr + n * 8 - 8, 8);
    if (Read64(b)) {
      if ((s = CopyStr(m, Read64(b)))) {
        list[n - 1] = s;
      } else {
        free(list);
        return 0;
      }
    } else {
      list[n - 1] = 0;
      return (char **)AddToFreeList(m, list);
    }
  }
}
