#ifndef BLINK_GUEST_PIPE_H_
#define BLINK_GUEST_PIPE_H_

struct System;

/* B7: Guest-side pipe for wasm32.
 * Creates an in-memory ring buffer pipe that does not require host OS
 * pipe() support, avoiding fatal wasm traps. */
int GuestPipeCreate(struct System *s, int fds[2]);

/* Called from FreeFd to release the GuestPipe reference when a pipe fd
 * is closed. */
void GuestPipeReleaseData(void *data);

/* Called from CloneFds to increment the GuestPipe refcount when a pipe fd
 * is inherited across fork(). */
void GuestPipeAcquireData(void *data);

#endif /* BLINK_GUEST_PIPE_H_ */