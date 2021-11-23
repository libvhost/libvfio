#ifndef NVME_H
#define NVME_H
#include <stdbool.h>
#include <sys/uio.h>

#include "lib/vfio-helper.h"

#define NVME_SQ_ENTRY_BYTES 64
#define NVME_CQ_ENTRY_BYTES 16
#define NVME_QUEUE_SIZE 128
#define NVME_DOORBELL_SIZE 4096
#define NVME_QP_SIZE 128

#define PAGE_SIZE 4096
/*
 * We have to leave one slot empty as that is the full queue case where
 * head == tail + 1.
 */
#define NVME_NUM_REQS (NVME_QUEUE_SIZE - 1)

/* Same index is used for queues and IRQs */
#define INDEX_ADMIN 0
#define INDEX_IO(n) (1 + n)

/* This driver shares a single MSIX IRQ for the admin and I/O queues */
enum { MSIX_SHARED_IRQ_IDX = 0, MSIX_IRQ_COUNT = 1 };

enum {
  NVME_IO_FLAGS_FUA = 0x1,
};

struct nvme_device;

typedef void (*nvme_command_cb)(void *priv, int rc);
struct nvme_name {
  char bdf[32];
  uint32_t nsid;
};

struct nvme_doorbell {
  uint32_t sq_tail;
  uint32_t cq_head;
};

struct nvme_queue {
  uint32_t head;
  uint32_t tail;
  // Hardware/device share memory, use volatile.
  volatile uint8_t *queue;
  uint64_t iova;
  /* Hardware MMIO register */
  volatile uint32_t *doorbell;
};

struct nvme_req {
  nvme_command_cb cb;
  void *opaque;
  uint16_t cid;
  void *prp_list_page;
  uint64_t prp_list_iova;
  bool is_used;
};
struct nvme_qp {
  int index;
  uint8_t *prp_list_pages;
  struct nvme_queue sq;
  struct nvme_queue cq;
  int cq_phase;
  int req_next;
  struct nvme_req reqs[NVME_NUM_REQS];
  int need_kick;
  int inflight;
};

struct nvme_device {
  struct nvme_name name;
  struct propig_vfio *vfio;
  uint32_t qp_cnt;
  uint32_t page_size;
  uint32_t doorbell_scale;
  volatile struct nvme_doorbell *doorbells;
  struct nvme_qp *qps[NVME_QP_SIZE];
  bool write_cache_supported;
  uint32_t max_transfer;
  bool supports_write_zeroes;
  bool supports_discard;
  uint64_t nsze;     // nvme namespace size
  uint32_t blkshift; // nvme namespace block size

  int irqfd;
  pid_t pid;
};

struct nvme_device *nvme_init(const char *filename);
uint64_t nvme_get_blocksize(const struct nvme_device *nd);
uint64_t nvme_get_numblocks(const struct nvme_device *nd);

int nvme_submit_io(struct nvme_device *nvme_device, uint32_t qpid,
                   uint64_t offset, uint64_t bytes, struct iovec *iov,
                   size_t iovcnt, bool is_write, int flags, nvme_command_cb cb,
                   void *opaque);
int nvme_poll(struct nvme_device *nvme_device);

#endif
