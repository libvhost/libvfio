#pragma once

#include <inttypes.h>
#include <linux/virtio_ring.h>
#include <stdbool.h>
#include <sys/uio.h>

#define rmb() __asm volatile("" :: \
                                 : "memory")
#define wmb() __asm volatile("" :: \
                                 : "memory")

#define VIRTIO_MAX_IODEPTH 256

typedef enum VhostIOType {
    VHOST_IO_READ,
    VHOST_IO_WRITE,
    VHOST_IO_FLUSH,
} VhostIOType;

typedef int (*VhostIOCB)(void *task);

typedef struct VhostTask {
    uint64_t offset;// align to sector size.
    VhostIOType type;
    struct iovec iovs[128];
    int iovcnt;
    int q_idx;
    bool used;
    VhostIOCB cb;

    // VirtioBlkReq or SCSIReq;
    void *priv;
    bool finished;

    // user data.
    void *opaque;
} VhostTask;

typedef struct VirtQueue {
    int idx;
    int size;
    /* Must be [0, 2^16 - 1] */
    uint16_t last_used_idx;
    int kickfd;
    int callfd;

    struct vring vring;
    /* next free head in desc table */
    uint16_t free_head;
    uint16_t num_free;

    VhostTask tasks[VIRTIO_MAX_IODEPTH];
    void *desc_state[VIRTIO_MAX_IODEPTH];
} VirtQueue;

void vhost_vq_init(VirtQueue *vq);

void virtring_add(VirtQueue *vq, struct iovec *iovec, int num_out, int num_in,
                  void *data);

VhostTask *virtring_get_free_task(VirtQueue *vq);

void virtring_free_task(VhostTask *task);

void virtqueue_kick(VirtQueue *vq);

int virtqueue_get(VirtQueue *vq, VhostTask **out_task);