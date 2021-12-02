
#pragma once
#include "vhost_user_spec.h"
#include "virtqueue.h"
#include "virtio_blk.h"

#include <linux/virtio_blk.h>

// typedef struct VhostDev {
// } VhostDev;
typedef struct VhostConn {
  char *sock_path;
  int status;
  int sock;
  uint64_t features;
  VirtQueue vqs[VHOST_USER_MAX_QUEUE_PAIRS];
  int nr_vqs;
  // VhostDev* dev;
  struct virtio_blk_config cfg;
} VhostConn;

VhostConn *create_vhost_conn(const char *path);
void destroy_vhost_conn(VhostConn *conn);
int vhost_connect(VhostConn *conn);
int vhost_setup(VhostConn *conn);

int vhost_conn_add_vq(VhostConn *conn, int size);
// int vhost_conn_del_vq(VhostConn* conn);

uint64_t vhost_conn_get_blocksize(VhostConn *conn);
int vhost_conn_get_numblocks(VhostConn *conn);