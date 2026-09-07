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
#include "blink/assert.h"

#include <errno.h>
#include <stdio.h>

#include "blink/debug.h"
#include "blink/flag.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/thread.h"
#include "blink/util.h"

void AssertFailed(const char *file, int line, const char *msg) {
  _Thread_local static bool noreentry;
  _Thread_local static char bp[20000];
  struct Machine *m = g_machine;
  int lock_i = m ? m->pagelocks.i : -1;
  int last_depth =
      m && lock_i > 0 ? m->pagelocks.p[lock_i - 1].sysdepth : -1;
  u64 last_page =
      m && lock_i > 0 ? (u64)m->pagelocks.p[lock_i - 1].page : 0;
  void *last_pslot =
      m && lock_i > 0 ? (void *)m->pagelocks.p[lock_i - 1].pslot : 0;
  WriteErrorString("assertion failed\n");
  ERRF("G12-ASSERT file=%s line=%d tid=%d msg=%s ip=%p sp=%p fault=%p "
       "pagelocks_i=%d sysdepth=%d last_depth=%d nofault=%d insyscall=%d "
       "last_page=%#llx last_pslot=%p",
       file, line, m ? m->tid : -1, msg, m ? (void *)GetIp(m) : 0,
       m ? (void *)m->sp : 0, m ? (void *)m->faultaddr : 0, lock_i,
       m ? m->sysdepth : -1, last_depth, m ? m->nofault : -1,
       m ? m->insyscall : -1, (unsigned long long)last_page, last_pslot);
  if (!noreentry) {
    noreentry = true;
    FLAG_nologstderr = false;
    RestoreIp(g_machine);
    snprintf(bp, sizeof(bp),
             "%s:%d:%d assertion failed: %s (%s)\n"
             "\t%s\n"
             "\t%s\n",
             file, line, g_machine ? g_machine->tid : 666, msg,
             DescribeHostErrno(errno), GetBacktrace(g_machine),
             GetBlinkBacktrace());
    WriteErrorString(bp);
  }
  Abort();
}
