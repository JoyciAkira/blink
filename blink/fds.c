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
#include "blink/fds.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "blink/assert.h"
#include "blink/atomic.h"
#include "blink/builtin.h"
#include "blink/errno.h"
#include "blink/log.h"
#include "blink/macros.h"
#include "blink/thread.h"
#include "blink/vfs.h"

void InitFds(struct Fds *fds) {
  fds->list = 0;
  unassert(!pthread_mutex_init(&fds->lock, 0));
}

struct Fd *AddFd(struct Fds *fds, int fildes, int oflags) {
  struct Fd *fd;
  if (fildes >= 0) {
    if ((fd = (struct Fd *)calloc(1, sizeof(*fd)))) {
      dll_init(&fd->elem);
      fd->cb = &kFdCbHost;
      fd->fildes = fildes;
      fd->oflags = oflags;
      fd->ofd_refcount = (int *)malloc(sizeof(int));
      if (fd->ofd_refcount) {
        *fd->ofd_refcount = 1;
      }
      unassert(!pthread_mutex_init(&fd->lock, 0));
      dll_make_first(&fds->list, &fd->elem);
    }
    return fd;
  } else {
    einval();
    return 0;
  }
}

struct Fd *ForkFd(struct Fds *fds, struct Fd *fd, int fildes, int oflags) {
  struct Fd *fd2;
  if ((fd2 = AddFd(fds, fildes, oflags))) {
    if (fd) {
      fd2->path = fd->path ? strdup(fd->path) : 0;
      fd2->socktype = fd->socktype;
      fd2->norestart = fd->norestart;
      memcpy(&fd2->saddr, &fd->saddr, sizeof(fd->saddr));
    }
  }
  return fd2;
}

struct Fd *GetFd(struct Fds *fds, int fildes) {
  bool lru;
  struct Dll *e;
  if (fildes >= 0) {
    lru = false;
    for (e = dll_first(fds->list); e; e = dll_next(fds->list, e)) {
      if (FD_CONTAINER(e)->fildes == fildes) {
        if (lru) {
          dll_remove(&fds->list, e);
          dll_make_first(&fds->list, e);
        }
        return FD_CONTAINER(e);
      }
      lru = true;
    }
  }
  ebadf();
  return 0;
}

void LockFd(struct Fd *fd) {
  LOCK(&fd->lock);
}

void UnlockFd(struct Fd *fd) {
  UNLOCK(&fd->lock);
}

int CountFds(struct Fds *fds) {
  int n = 0;
  struct Dll *e;
  for (e = dll_first(fds->list); e; e = dll_next(fds->list, e)) {
    ++n;
  }
  return n;
}

int FreeFd(struct Fd *fd) {
  int rc = 0;
  if (fd) {
    bool should_close_host = false;
    
    /* B6: Decrement OFD refcount; close host FD only when last reference */
    if (fd->ofd_refcount) {
      (*fd->ofd_refcount)--;
      if (*fd->ofd_refcount <= 0) {
        should_close_host = true;
        free(fd->ofd_refcount);
      }
    } else {
      /* No refcount tracking - always close (legacy path) */
      should_close_host = true;
    }
    
    /* Close host FD if this is the last reference */
    if (should_close_host && fd->cb) {
      if (fd->dirstream) {
        rc = VfsClosedir(fd->dirstream);
      } else {
        rc = fd->cb->close(fd->fildes);
      }
    }
    
    unassert(!pthread_mutex_destroy(&fd->lock));
    free(fd->path);
    free(fd);
  }
  return rc;
}

void DestroyFds(struct Fds *fds) {
  struct Dll *e, *e2;
  for (e = dll_first(fds->list); e; e = e2) {
    e2 = dll_next(fds->list, e);
    dll_remove(&fds->list, e);
    FreeFd(FD_CONTAINER(e));
  }
  unassert(!fds->list);
  unassert(!pthread_mutex_destroy(&fds->lock));
}

static int GetFdSocketType(int fildes, int *type) {
  socklen_t len = sizeof(*type);
  return VfsGetsockopt(fildes, SOL_SOCKET, SO_TYPE, type, &len);
}

static bool IsNoRestartSocket(int fildes) {
  struct timeval tv = {0};
  socklen_t len = sizeof(tv);
  VfsGetsockopt(fildes, SOL_SOCKET, SO_RCVTIMEO, &tv, &len);
  return tv.tv_sec || tv.tv_usec;
}

void InheritFd(struct Fd *fd) {
#ifndef DISABLE_SOCKETS
  socklen_t addrlen;
  if (!fd) return;
  if (!GetFdSocketType(fd->fildes, &fd->socktype)) {
    fd->norestart = IsNoRestartSocket(fd->fildes);
    addrlen = sizeof(fd->saddr);
    VfsGetsockname(fd->fildes, (struct sockaddr *)&fd->saddr, &addrlen);
  }
#endif
}

void AddStdFd(struct Fds *fds, int fildes) {
  int flags;
  if ((flags = VfsFcntl(fildes, F_GETFL, 0)) >= 0) {
    InheritFd(AddFd(fds, fildes, flags));
  }
}

/* B6: Clone parent FD table to child with shared OFD refcounts.
 * Each child Fd gets its own struct but shares ofd_refcount pointer with parent.
 * This implements Unix fork() semantics: independent FD tables referencing
 * the same underlying open file descriptions. */
void CloneFds(struct Fds *child, struct Fds *parent) {
  struct Dll *e;
  struct Fd *parent_fd, *child_fd;
  LOCK(&parent->lock);
  for (e = dll_first(parent->list); e; e = dll_next(parent->list, e)) {
    parent_fd = FD_CONTAINER(e);
    child_fd = AddFd(child, parent_fd->fildes, parent_fd->oflags);
    if (child_fd) {
      /* Share the OFD refcount: increment it and point child to same counter */
      if (parent_fd->ofd_refcount) {
        free(child_fd->ofd_refcount);  /* Free the fresh one from AddFd */
        child_fd->ofd_refcount = parent_fd->ofd_refcount;
        (*child_fd->ofd_refcount)++;
      }
      /* Copy metadata */
      if (parent_fd->path) {
        child_fd->path = strdup(parent_fd->path);
      }
      child_fd->socktype = parent_fd->socktype;
      child_fd->norestart = parent_fd->norestart;
      memcpy(&child_fd->saddr, &parent_fd->saddr, sizeof(child_fd->saddr));
    }
  }
  UNLOCK(&parent->lock);
}
