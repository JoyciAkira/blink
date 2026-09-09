/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
╞══════════════════════════════════════════════════════════════════════════════╡
│ B7: Guest-side pipe implementation for wasm32.                             │
│                                                                            │
│ Host pipe() causes a fatal wasm trap because the wasm32 sandbox has no     │
│ kernel pipe primitive. This module implements an in-memory ring buffer     │
│ pipe entirely within the Blink guest, using FdCb callbacks so that         │
│ VfsRead/VfsWrite/VfsPoll work transparently on pipe fds.                   │
│                                                                            │
│ Ring buffer layout (allocated via malloc):                                  │
│   struct GuestPipe {                                                       │
│     u8 *buf;          // PIPE_BUF_SIZE bytes                               │
│     size_t head;      // next read position                                │
│     size_t tail;      // next write position                              │
│     size_t count;     // bytes currently buffered                          │
│     bool write_closed; // all writers closed → readers get EOF             │
│     bool read_closed;  // all readers closed → writers get EPIPE           │
│     int refcount;      // shared between read-end and write-end Fds        │
│   };                                                                       │
│                                                                            │
│ Thread safety: Blink wasm32 is single-threaded (DISABLE_THREADS),          │
│ so no locks needed on the ring buffer itself. The Fd->lock protects        │
│ Fd-level operations as usual.                                              │
╚─────────────────────────────────────────────────────────────────────────────*/

#include "blink/fds.h"
#include "blink/machine.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <poll.h>

#include "blink/assert.h"
#include "blink/errno.h"
#include "blink/log.h"
#include "blink/vfs.h"

#define GUEST_PIPE_BUF_SIZE 65536  /* 64KB ring buffer per pipe */

struct GuestPipe {
  u8 *buf;
  size_t head;
  size_t tail;
  size_t count;
  bool write_closed;
  bool read_closed;
  int refcount;
};

static struct GuestPipe *NewGuestPipe(void) {
  struct GuestPipe *gp = (struct GuestPipe *)calloc(1, sizeof(*gp));
  if (!gp) return NULL;
  gp->buf = (u8 *)malloc(GUEST_PIPE_BUF_SIZE);
  if (!gp->buf) {
    free(gp);
    return NULL;
  }
  gp->head = 0;
  gp->tail = 0;
  gp->count = 0;
  gp->write_closed = false;
  gp->read_closed = false;
  gp->refcount = 2;  /* one for read-end, one for write-end */
  return gp;
}
static void ReleaseGuestPipe(struct GuestPipe *gp) {
  if (!gp) return;
  gp->refcount--;
  if (gp->refcount <= 0) {
    free(gp->buf);
    free(gp);
  }
}

/* Called from CloneFds when a guest-pipe Fd is inherited across fork().
 * Increments the refcount so the pipe stays alive until all processes close it. */
void GuestPipeAcquireData(void *data) {
  struct GuestPipe *gp = (struct GuestPipe *)data;
  if (gp) {
    gp->refcount++;
  }
}

/* Called from FreeFd when a guest-pipe Fd is being destroyed. */
void GuestPipeReleaseData(void *data) {
  ReleaseGuestPipe((struct GuestPipe *)data);
}

/* ── FdCb callbacks for pipe read-end ─────────────────────────────────────── */

static int PipeClose(int fildes) {
  /* Called when this Fd is freed; release our reference to the GuestPipe.
   * The GuestPipe pointer is stored in the Fd's path field (repurposed). */
  (void)fildes;
  return 0;
}

static ssize_t PipeReadv(int fildes, const struct iovec *iov, int iovcnt) {
  struct Fd *fd;
  struct GuestPipe *gp;
  ssize_t total = 0;
  int i;

  LOCK(&g_machine->system->fds.lock);
  fd = GetFd(&g_machine->system->fds, fildes);
  UNLOCK(&g_machine->system->fds.lock);
  if (!fd) return -EBADF;

  gp = (struct GuestPipe *)fd->guest_data;
  if (!gp) return -EBADF;

  for (i = 0; i < iovcnt; i++) {
    size_t n = iov[i].iov_len;
    size_t avail;
    while (n > 0 && gp->count > 0) {
      size_t chunk;
      avail = GUEST_PIPE_BUF_SIZE - gp->head;
      chunk = n < gp->count ? n : gp->count;
      if (chunk > avail) chunk = avail;
      memcpy((u8 *)iov[i].iov_base + (total - (ssize_t)(iov[i].iov_len - n)),
             gp->buf + gp->head, chunk);
      gp->head = (gp->head + chunk) % GUEST_PIPE_BUF_SIZE;
      gp->count -= chunk;
      n -= chunk;
      total += chunk;
    }
    if (gp->count == 0) break;  /* no more data available */
  }

  if (total == 0 && gp->write_closed) return 0;  /* EOF */
  if (total == 0) return -EAGAIN;  /* would block */
  return total;
}

static ssize_t PipeWritev(int fildes, const struct iovec *iov, int iovcnt) {
  (void)fildes; (void)iov; (void)iovcnt;
  return -EBADF;  /* writing to read-end is invalid */
}

static int PipePollRead(struct pollfd *pfd, nfds_t nfds, int timeout) {
  (void)timeout;
  /* For simplicity, check first fd only */
  if (nfds < 1) return 0;
  struct Fd *fd;
  struct GuestPipe *gp;
  LOCK(&g_machine->system->fds.lock);
  fd = GetFd(&g_machine->system->fds, pfd[0].fd);
  UNLOCK(&g_machine->system->fds.lock);
  if (!fd) { pfd[0].revents = POLLNVAL; return 1; }
  gp = (struct GuestPipe *)fd->guest_data;
  if (!gp) { pfd[0].revents = POLLNVAL; return 1; }
  pfd[0].revents = 0;
  if (gp->count > 0) pfd[0].revents |= POLLIN;
  if (gp->write_closed) pfd[0].revents |= POLLHUP;
  return (pfd[0].revents != 0) ? 1 : 0;
}

static const struct FdCb kFdCbPipeRead = {
    .close = PipeClose,
    .readv = PipeReadv,
    .writev = PipeWritev,
    .poll = PipePollRead,
    .tcgetattr = NULL,
    .tcsetattr = NULL,
    .tcgetwinsize = NULL,
    .tcsetwinsize = NULL,
};

/* ── FdCb callbacks for pipe write-end ────────────────────────────────────── */

static ssize_t PipeWriteEndWritev(int fildes, const struct iovec *iov,
                                   int iovcnt) {
  struct Fd *fd;
  struct GuestPipe *gp;
  ssize_t total = 0;
  int i;

  LOCK(&g_machine->system->fds.lock);
  fd = GetFd(&g_machine->system->fds, fildes);
  UNLOCK(&g_machine->system->fds.lock);
  if (!fd) return -EBADF;

  gp = (struct GuestPipe *)fd->guest_data;
  if (!gp) return -EBADF;
  if (gp->read_closed) return -EPIPE;

  for (i = 0; i < iovcnt; i++) {
    size_t n = iov[i].iov_len;
    while (n > 0 && gp->count < GUEST_PIPE_BUF_SIZE) {
      size_t space = GUEST_PIPE_BUF_SIZE - gp->count;
      size_t avail = GUEST_PIPE_BUF_SIZE - gp->tail;
      size_t chunk = n < space ? n : space;
      if (chunk > avail) chunk = avail;
      memcpy(gp->buf + gp->tail,
             (const u8 *)iov[i].iov_base + (total - (ssize_t)(iov[i].iov_len - n)),
             chunk);
      gp->tail = (gp->tail + chunk) % GUEST_PIPE_BUF_SIZE;
      gp->count += chunk;
      n -= chunk;
      total += chunk;
    }
    if (gp->count >= GUEST_PIPE_BUF_SIZE) break;  /* buffer full */
  }

  if (total == 0) return -EAGAIN;  /* would block */
  return total;
}

static ssize_t PipeWriteEndReadv(int fildes, const struct iovec *iov,
                                  int iovcnt) {
  (void)fildes; (void)iov; (void)iovcnt;
  return -EBADF;  /* reading from write-end is invalid */
}

static int PipePollWrite(struct pollfd *pfd, nfds_t nfds, int timeout) {
  (void)timeout;
  if (nfds < 1) return 0;
  struct Fd *fd;
  struct GuestPipe *gp;
  LOCK(&g_machine->system->fds.lock);
  fd = GetFd(&g_machine->system->fds, pfd[0].fd);
  UNLOCK(&g_machine->system->fds.lock);
  if (!fd) { pfd[0].revents = POLLNVAL; return 1; }
  gp = (struct GuestPipe *)fd->guest_data;
  if (!gp) { pfd[0].revents = POLLNVAL; return 1; }
  pfd[0].revents = 0;
  if (gp->count < GUEST_PIPE_BUF_SIZE) pfd[0].revents |= POLLOUT;
  if (gp->read_closed) pfd[0].revents |= POLLERR;
  return (pfd[0].revents != 0) ? 1 : 0;
}

static const struct FdCb kFdCbPipeWrite = {
    .close = PipeClose,
    .readv = PipeWriteEndReadv,
    .writev = PipeWriteEndWritev,
    .poll = PipePollWrite,
    .tcgetattr = NULL,
    .tcsetattr = NULL,
    .tcgetwinsize = NULL,
    .tcsetwinsize = NULL,
};

/* ── Public API ───────────────────────────────────────────────────────────── */

int GuestPipeCreate(struct System *s, int fds[2]) {
  struct GuestPipe *gp;
  struct Fd *rdfd, *wrfd;

  if (!s || !fds) return efault();

  gp = NewGuestPipe();
  if (!gp) return enomem();

  LOCK(&s->fds.lock);

  /* Create read-end fd */
  rdfd = AddFdAuto(&s->fds, O_RDONLY);
  if (!rdfd) {
    UNLOCK(&s->fds.lock);
    ReleaseGuestPipe(gp);
    return emfile();
  }
  rdfd->cb = &kFdCbPipeRead;
  rdfd->guest_data = (void *)gp;

  /* Create write-end fd */
  wrfd = AddFdAuto(&s->fds, O_WRONLY);
  if (!wrfd) {
    /* Rollback read-end */
    dll_remove(&s->fds.list, &rdfd->elem);
    FreeFd(rdfd);
    UNLOCK(&s->fds.lock);
    ReleaseGuestPipe(gp);
    return emfile();
  }
  wrfd->cb = &kFdCbPipeWrite;
  wrfd->guest_data = (void *)gp;

  fds[0] = rdfd->fildes;
  fds[1] = wrfd->fildes;

  UNLOCK(&s->fds.lock);
  return 0;
}