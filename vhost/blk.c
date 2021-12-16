#include "blk.h"
#include "memory.h"
#include "utils.h"
#include "virtqueue.h"
#include <linux/virtio_blk.h>
#include <linux/virtio_ring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int iovec_init(struct iovec *iov, void *buf, size_t len) {
    iov->iov_base = buf;
    iov->iov_len = len;
    return 0;
}

static int blk_add_req(VirtQueue *vq, VirtioBlkReq *vbr,
                       struct iovec *data_iovs, int data_iovcnt, void *data) {
    int num_out = 0;
    int num_in = 0;
    int i;
    struct vring_desc *desc;
    struct iovec iovs[128];
    if (data_iovcnt > 128 - 2) {
        printf("Too many iovec in req: %d\n", data_iovcnt);
        exit(EXIT_FAILURE);
        return -1;
    }
    // put header.
    iovec_init(&iovs[num_out++], &vbr->out_hdr, sizeof(vbr->out_hdr));
    // put iovs.
    for (i = 0; i < data_iovcnt; ++i) {
        if (vbr->out_hdr.type == VIRTIO_BLK_T_OUT) {
            iovs[num_out++] = data_iovs[i];
        } else {
            iovs[num_out + num_in++] = data_iovs[i];
        }
    }
    // put status.
    iovec_init(&iovs[num_out + num_in++], &vbr->status, sizeof(vbr->status));

    virtring_add(vq, iovs, num_out, num_in, data);
    return 0;
}

void blk_task_submit(VirtQueue *vq, VhostTask *task) {
    VirtioBlkReq *req = vhost_malloc(sizeof(VirtioBlkReq));
    task->priv = req;
    switch (task->type) {
        case VHOST_IO_WRITE:
            req->out_hdr.type = VIRTIO_BLK_T_OUT;
            break;
        case VHOST_IO_READ:
            req->out_hdr.type = VIRTIO_BLK_T_IN;
            break;
        default:
            printf("blk_task_submit unknow io type\n");
            exit(EXIT_FAILURE);
    }
    req->out_hdr.sector = task->offset >> 9;
    req->out_hdr.ioprio = 0;
    blk_add_req(vq, req, task->iovs, task->iovcnt, task);
}

int blk_task_getevents(VirtQueue *vq, VhostTask **out_task) {
    return virtqueue_get(vq, out_task);
}