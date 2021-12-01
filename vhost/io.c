
#include "io.h"
#include "blk.h"
#include "conn.h"
#include "utils.h"
#include "virtqueue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *get_virtio_task_status(int status) {
  switch (status) {
  case VIRTIO_BLK_S_OK:
    return "OK";
  case VIRTIO_BLK_S_IOERR:
    return "IOERR";
  case VIRTIO_BLK_S_UNSUPP:
    return "UNSUPP";
  default:
    return "UNKNOWN";
  }
}

static int task_done(void *opaque) {
  VhostTask *task = opaque;
  task->finished = true;

  VirtioBlkReq *req = task->priv;

  DEBUG("[TASK DONE] offset: 0x%" PRIx64
        ", type: %d, iovcnt: %d, queue index: %d, used: %d, priv: %p status: "
        "%s task: %p\n",
        task->offset, task->type, task->iovcnt, task->q_idx, task->used,
        task->priv, get_virtio_task_status(req->status), task);
  return 0;
}

static int libvhost_readwritev(VhostConn *conn, int q_idx, uint64_t offset,
                               struct iovec *iov, int iovcnt, bool write,
                               bool async, void *opaque) {
  VhostTask *task;
  VhostTask *out_task;
  VirtQueue *vq = &conn->vqs[q_idx];
  task = virtring_get_free_task(vq);
  if (!task) {
    printf("NO MORE TASK\n");
    exit(EXIT_FAILURE);
    return -1;
  }
  CHECK(task->used == true);
  CHECK(task->finished == false);
  task->cb = task_done;
  task->opaque = opaque;
  // TODO: check the offset, len that should be aligned to 512/4k block size.
  task->offset = offset;
  memcpy(task->iovs, iov, sizeof(struct iovec) * iovcnt);
  task->iovcnt = iovcnt;
  task->q_idx = q_idx;
  if (write) {
    task->type = VHOST_IO_WRITE;
  } else {
    task->type = VHOST_IO_READ;
  }
  blk_task_submit(vq, task);
  if (async) {
    return 0;
  }

  while (!task->finished) {
    if (blk_task_getevents(vq, &out_task) == 0) {
      continue;
    }
    if (task != out_task) {
      virtring_free_task(out_task);
      printf("[WARN] io is out-of-order, task: %p, out_task: %p\n", task,
             out_task);
    }
  }
  virtring_free_task(task);
  return 0;
}

int libvhost_read(VhostConn *conn, int q_idx, uint64_t offset, char *buf,
                  int len) {
  struct iovec iov = {.iov_base = buf, .iov_len = len};
  return libvhost_readwritev(conn, q_idx, offset, &iov, 1, false, false, NULL);
}

int libvhost_write(VhostConn *conn, int q_idx, uint64_t offset, char *buf,
                   int len) {
  struct iovec iov = {.iov_base = buf, .iov_len = len};
  return libvhost_readwritev(conn, q_idx, offset, &iov, 1, true, false, NULL);
}

int libvhost_readv(VhostConn *conn, int q_idx, uint64_t offset,
                   struct iovec *iov, int iovcnt) {
  return libvhost_readwritev(conn, q_idx, offset, iov, iovcnt, false, false,
                             NULL);
}

int libvhost_writev(VhostConn *conn, int q_idx, uint64_t offset,
                    struct iovec *iov, int iovcnt) {
  return libvhost_readwritev(conn, q_idx, offset, iov, iovcnt, true, false,
                             NULL);
}

int libvhost_submit(VhostConn *conn, int q_idx, uint64_t offset,
                    struct iovec *iov, int iovcnt, bool write, void *opaque) {
  return libvhost_readwritev(conn, q_idx, offset, iov, iovcnt, write, true,
                             opaque);
}

int libvhost_getevents(VhostConn *conn, int q_idx, int nr, VhostEvent *events) {
  int done = 0;
  int ret = 0;
  int i;
  VhostTask *done_tasks[VIRTIO_MAX_IODEPTH];
  while (done < nr) {
    ret = blk_task_getevents(&conn->vqs[q_idx], &done_tasks[done]);
    if (ret == 0) {
      continue;
    }
    done += ret;
    CHECK(done_tasks[done - 1]->used == true);
    CHECK(done_tasks[done - 1]->finished == true);
  }
  for (i = 0; i < done; i++) {
    events[i].data = done_tasks[i]->opaque;
    events[i].res = ((VirtioBlkReq *)done_tasks[i]->priv)->status;
    virtring_free_task(done_tasks[i]);
  }
  return done;
}