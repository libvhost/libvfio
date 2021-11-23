#pragma once

#include "conn.h"
#include <sys/uio.h>

// typedef void (*vhost_cpl_t)(void *opaque, int status);
typedef struct VhostEvent {
  void *data;
  int res;
} VhostEvent;

int libvhost_read(VhostConn *conn, int q_idx, uint64_t offset, char *buf,
                  int len);
int libvhost_write(VhostConn *conn, int q_idx, uint64_t offset, char *buf,
                   int len);
int libvhost_readv(VhostConn *conn, int q_idx, uint64_t offset,
                   struct iovec *iov, int iovcnt);
int libvhost_writev(VhostConn *conn, int q_idx, uint64_t offset,
                    struct iovec *iov, int iovcnt);
int libvhost_submit(VhostConn *conn, int q_idx, uint64_t offset,
                    struct iovec *iov, int iovcnt, bool write, void *opaque);

int libvhost_getevents(VhostConn *conn, int q_idx, int nr, VhostEvent *events);
