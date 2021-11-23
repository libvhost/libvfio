#pragma once
#include <inttypes.h>
#include <linux/virtio_blk.h>
#include <sys/uio.h>

#include "virtqueue.h"
typedef struct VirtioBlkReq {
  struct virtio_blk_outhdr out_hdr;
  uint8_t status;
} VirtioBlkReq;

void blk_task_submit(VirtQueue *vq, VhostTask *task);
int blk_task_getevents(VirtQueue *vq, VhostTask **out_task);