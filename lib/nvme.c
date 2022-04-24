#include "nvme.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "nvme_spec.h"
#include "vfio-helper.h"

// common/utils.h

#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)

#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define le64_to_cpu(x) (x)

// log2
static inline int ctz32(uint32_t val) { return val ? __builtin_ctz(val) : 32; }

#if 0
static void print_cmd(struct NvmeCmd *cmd) {
  printf("[NVMe CMD] nsid: %d cid: %d opcode: %d\n", cmd->nsid, cmd->cid,
         cmd->opcode);
}
#endif

#define QEMU_HEXDUMP_LINE_BYTES 16 /* Number of bytes to dump */
#define QEMU_HEXDUMP_LINE_LEN 75   /* Number of characters in line */

void qemu_hexdump_line(char *line, unsigned int b, const void *bufptr,
                       unsigned int len, bool ascii) {
  const char *buf = bufptr;
  int i, c;

  if (len > QEMU_HEXDUMP_LINE_BYTES) {
    len = QEMU_HEXDUMP_LINE_BYTES;
  }

  line += snprintf(line, 6, "%04x:", b);
  for (i = 0; i < QEMU_HEXDUMP_LINE_BYTES; i++) {
    if ((i % 4) == 0) {
      *line++ = ' ';
    }
    if (i < len) {
      line += sprintf(line, " %02x", (unsigned char)buf[b + i]);
    } else {
      line += sprintf(line, "   ");
    }
  }
  if (ascii) {
    *line++ = ' ';
    for (i = 0; i < len; i++) {
      c = buf[b + i];
      if (c < ' ' || c > '~') {
      }
      *line++ = c;
    }
  }
  *line = '\0';
}

void qemu_hexdump(FILE *fp, const char *prefix, const void *bufptr,
                  size_t size) {
  unsigned int b, len;
  char line[QEMU_HEXDUMP_LINE_LEN];

  for (b = 0; b < size; b += QEMU_HEXDUMP_LINE_BYTES) {
    len = size - b;
    qemu_hexdump_line(line, b, bufptr, len, true);
    fprintf(fp, "%s: %s\n", prefix, line);
  }
}

////////////////////////////////////////////////////////////////

// static void nvme_inc_sq_head(struct nvme_queue *sq)
// {
//     sq->head = (sq->head + 1) % sq->size;
// }

// static uint8_t nvme_cq_full(NvmeCQueue *cq)
// {
//     return (cq->tail + 1) % cq->size == cq->head;
// }

// static uint8_t nvme_sq_empty(NvmeSQueue *sq)
// {
//     return sq->head == sq->tail;
// }

static int nvme_poll_cq(struct nvme_qp *qp);

static uint64_t get_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (unsigned long long)(tv.tv_sec) * 1000 +
         (unsigned long long)(tv.tv_usec) / 1000;
}

static inline int nvme_translate_error(const struct NvmeCqe *cqe) {
  uint16_t status = (le16_to_cpu(cqe->status) >> 1) & 0xFF;
  switch (status) {
  case 0:
    return 0;
  case 1:
    return -ENOSYS;
  case 2:
    return -EINVAL;
  default:
    return -EIO;
  }
  return 0;
}

// nvme://0000:44:00.0/1
static bool nvme_parse_filename(const char *filename, struct nvme_name *name) {
  int ret = 0;
  ret = sscanf(filename, "nvme://%12s/%d", name->bdf, &name->nsid);
  printf("NVME bdf: %s ns: %d \n", name->bdf, name->nsid);
  return ret == 2;
}

static bool nvme_init_queue(struct nvme_device *nd, struct nvme_queue *q,
                            unsigned nentries, size_t entry_bytes) {
  size_t bytes;
  int r;
  void *ptr = NULL;
  bytes = ALIGN_UP(nentries * entry_bytes, nd->page_size);
  q->head = q->tail = 0;
  if (posix_memalign(&ptr, nd->page_size, bytes)) {
    return false;
  }
  q->queue = ptr;
  memset((void *)q->queue, 0, bytes);
  // Use va as iova.
  q->iova = (uint64_t)q->queue;
  r = propig_vfio_dma_map(nd->vfio, (const void *)q->queue, bytes, q->iova);
  if (r) {
    fprintf(stderr, "Cannot map queue: %s", strerror(errno));
    return false;
  }
  return true;
}

static struct nvme_qp *nvme_create_queue_pair(struct nvme_device *nd, int idx) {
  int i, r;
  struct nvme_qp *qp;
  uint64_t prp_list_iova;
  size_t bytes;

  qp = calloc(1, sizeof(*qp));
  if (!qp) {
    return NULL;
  }

  bytes = ALIGN_UP(nd->page_size * NVME_NUM_REQS, PAGE_SIZE);
  qp->prp_list_pages = memalign(nd->page_size, bytes);
  if (!qp->prp_list_pages) {
    goto fail;
  }
  memset(qp->prp_list_pages, 0, bytes);
  qp->index = idx;

  // Use va as iova.
  prp_list_iova = (uint64_t)qp->prp_list_pages;

  r = propig_vfio_dma_map(nd->vfio, qp->prp_list_pages, bytes, prp_list_iova);
  if (r) {
    fprintf(stderr, "Cannot map queue: %s", strerror(errno));
    goto fail;
  }
  for (i = 0; i < NVME_NUM_REQS; i++) {
    struct nvme_req *req = &qp->reqs[i];
    req->cid = i + 1;
    req->prp_list_page = qp->prp_list_pages + i * nd->page_size;
    req->prp_list_iova = prp_list_iova + i * nd->page_size;
  }

  if (!nvme_init_queue(nd, &qp->sq, NVME_QUEUE_SIZE, NVME_SQ_ENTRY_BYTES)) {
    goto fail;
  }
  // memset(qp->sq.queue, idx+0xa0, NVME_QUEUE_SIZE * NVME_SQ_ENTRY_BYTES);
  // printf("%d: init memset(0x%lx 0xff)\n", qp->index, (void*)(qp->sq.queue +
  // 64)); memset(qp->sq.queue + 64, 0xff, 64);
  qp->sq.doorbell = &nd->doorbells[idx * nd->doorbell_scale].sq_tail;

  if (!nvme_init_queue(nd, &qp->cq, NVME_QUEUE_SIZE, NVME_CQ_ENTRY_BYTES)) {
    goto fail;
  }
  qp->cq.doorbell = &nd->doorbells[idx * nd->doorbell_scale].cq_head;
  nd->qps[nd->qp_cnt++] = qp;
  return qp;
fail:
  // TODO: free resource.
  return NULL;
}

// non zero means error.
static int wait_for_reset(uint64_t timeout_ms, volatile NvmeBar *regs) {
  /* Wait for CSTS.RDY = 0. */
  uint64_t deadline = get_time_ms() + timeout_ms;
  while (NVME_CSTS_RDY(le32_to_cpu(regs->csts))) {
    if (get_time_ms() > deadline) {
      fprintf(stderr,
              "Timeout while waiting for device to reset (%" PRId64 " ms)",
              timeout_ms);
      return -ETIMEDOUT;
    }
  }
  return 0;
}

static int wait_for_ready(uint64_t timeout_ms, volatile NvmeBar *regs) {
  /* Wait for CSTS.RDY = 1. */
  uint64_t deadline = get_time_ms() + timeout_ms;
  while (!NVME_CSTS_RDY(le32_to_cpu(regs->csts))) {
    if (get_time_ms() > deadline) {
      fprintf(stderr,
              "Timeout while waiting for device to reset (%" PRId64 " ms)",
              timeout_ms);
      return -ETIMEDOUT;
    }
  }
  return 0;
}

static struct nvme_req *nvme_get_free_req(struct nvme_qp *qp) {
  struct nvme_req *req;
  int i;
  for (i = 0; i < NVME_NUM_REQS; ++i) {
    req = &qp->reqs[(qp->req_next++) % NVME_NUM_REQS];
    if (req->is_used) {
      continue;
    }
    req->is_used = true;
    return req;
  }
  return NULL;
}

static void nvme_kick(struct nvme_qp *qp) {
  if (!qp->need_kick) {
    return;
  }
  assert(!(qp->sq.tail & 0xFF00));
  /* Fence the write to submission queue entry before notifying the device. */
  wmb();
  *qp->sq.doorbell = cpu_to_le32(qp->sq.tail);
  // printf("[QP %d] kick sq doorbell, qp: %p, head: %u, tail: %u doorbell:
  // %u\n",
  //        qp->index, qp, qp->sq.head, qp->sq.tail, *qp->sq.doorbell);
  qp->inflight += qp->need_kick;
  qp->need_kick = 0;
}

static void nvme_submit_command(struct nvme_qp *qp, NvmeCmd *cmd) {
  // Put the entry at tail, device will get entry from head.
  memcpy((void *)(qp->sq.queue + qp->sq.tail * NVME_SQ_ENTRY_BYTES), cmd,
         sizeof(*cmd));
  qp->sq.tail = (qp->sq.tail + 1) % NVME_QUEUE_SIZE;
  qp->need_kick++;
  nvme_kick(qp);
}

static void nvme_admin_sync_cb(void *priv, int rc) {
  int *ret = priv;
  if (rc != 0) {
    printf("nvme admin cmd failed, rc: %d\n", rc);
  }
  *ret = rc;
}

static int nvme_admin_cmd_sync(struct nvme_device *nd, NvmeCmd *cmd) {
  struct nvme_qp *qp = nd->qps[INDEX_ADMIN];
  struct nvme_req *req;
  int ret = -EINPROGRESS;
  req = nvme_get_free_req(qp);
  if (!req) {
    return -EBUSY;
  }
  req->cb = nvme_admin_sync_cb;
  req->opaque = &ret;
  cmd->cid = cpu_to_le16(req->cid);
  nvme_submit_command(qp, cmd);
  while (nvme_poll_cq(qp) != 1) {
  }
  return ret;
}

static bool nvme_identify(struct nvme_device *nd, uint32_t nsid) {
  bool ret = false;
  union {
    NvmeIdCtrl ctrl;
    NvmeIdNs ns;
  } * id;
  NvmeLBAF *lbaf;
  uint16_t oncs;
  int r;
  uint64_t iova;
  // SPEC 5.15.2.2 Identify Controller data structure (CNS 01h)
  NvmeCmd cmd = {
      .opcode = NVME_ADM_CMD_IDENTIFY,
      .cdw10 = cpu_to_le32(0x1U),
  };
  size_t id_size = ALIGN_UP(sizeof(*id), nd->page_size);
  id = memalign(nd->page_size, id_size);
  if (!id) {
    fprintf(stderr, "Cannot allocate buffer for identify response\n");
    goto out;
  }
  iova = (uint64_t)id;
  r = propig_vfio_dma_map(nd->vfio, id, id_size, iova);
  if (r) {
    fprintf(stderr, "Cannot map buffer for DMA: %s\n", strerror(errno));
    goto out;
  }
  memset(id, 0, id_size);
  cmd.dptr.prp1 = cpu_to_le64(iova);
  if (nvme_admin_cmd_sync(nd, &cmd)) {
    fprintf(stderr, "Failed to identify controller: %s\n", strerror(errno));
    goto out;
  }

  if (le32_to_cpu(id->ctrl.nn) < nsid) {
    fprintf(stderr, "Invalid namespace\n");
    goto out;
  }
  nd->write_cache_supported = le32_to_cpu((uint32_t)id->ctrl.vwc) & 0x1;
  nd->max_transfer = (id->ctrl.mdts ? 1 << id->ctrl.mdts : 0) * nd->page_size;
  /* For now the page list buffer per command is one page, to hold at most
   * s->page_size / sizeof(uint64_t) entries. */
  nd->max_transfer =
      MIN(nd->max_transfer, nd->page_size / sizeof(uint64_t) * nd->page_size);

  oncs = le16_to_cpu(id->ctrl.oncs);
  nd->supports_write_zeroes = !!(oncs & NVME_ONCS_WRITE_ZEROES);
  nd->supports_discard = !!(oncs & NVME_ONCS_DSM);

  memset(id, 0, id_size);
  // SPEC 5.15.2.1: Identify Namespace data structure (CNS 00h)
  cmd.cdw10 = 0;
  cmd.nsid = cpu_to_le32(nsid);
  if (nvme_admin_cmd_sync(nd, &cmd)) {
    fprintf(stderr, "Failed to identify namespace\n");
    goto out;
  }

  nd->nsze = le64_to_cpu(id->ns.nsze);
  lbaf = &id->ns.lbaf[NVME_ID_NS_FLBAS_INDEX(id->ns.flbas)];

  //   if (NVME_ID_NS_DLFEAT_WRITE_ZEROES(id->ns.dlfeat) &&
  //       NVME_ID_NS_DLFEAT_READ_BEHAVIOR(id->ns.dlfeat) ==
  //           NVME_ID_NS_DLFEAT_READ_BEHAVIOR_ZEROES) {
  //     nd->supported_write_flags |= BDRV_REQ_MAY_UNMAP;
  //   }

  if (lbaf->ms) {
    fprintf(stderr, "Namespaces with metadata are not yet supported\n");
    goto out;
  }

  ret = true;
  nd->blkshift = lbaf->ds;

out:
  return ret;
}

static bool nvme_add_io_queue(struct nvme_device *nvme_device) {
  struct nvme_qp *qp;
  NvmeCmd cmd;
  uint32_t queue_size = NVME_QUEUE_SIZE;
  uint32_t qpid = nvme_device->qp_cnt;

  printf("[IO Queue Pair] %d\n", qpid);
  qp = nvme_create_queue_pair(nvme_device, qpid);
  if (!qp) {
    return false;
  }
  cmd = (NvmeCmd){
      .opcode = NVME_ADM_CMD_CREATE_CQ,
      .dptr.prp1 = cpu_to_le64(qp->cq.iova),
      .cdw10 = cpu_to_le32((uint32_t)(((queue_size - 1) << 16) | qpid)),
      // Enable interrupt.
      .cdw11 = cpu_to_le32((uint32_t)(NVME_CQ_IEN | NVME_CQ_PC)),
      // .cdw11 = cpu_to_le32((uint32_t)(NVME_CQ_PC)),
  };
  printf("[IO Queue Pair] %d NVME_ADM_CMD_CREATE_CQ cq iova: 0x%lx\n", qpid,
         qp->cq.iova);
  if (nvme_admin_cmd_sync(nvme_device, &cmd)) {
    fprintf(stderr, "Failed to create CQ io queue [%u]\n", qpid);
    goto out_error;
  }
  cmd = (NvmeCmd){
      .opcode = NVME_ADM_CMD_CREATE_SQ,
      .dptr.prp1 = cpu_to_le64(qp->sq.iova),
      .cdw10 = cpu_to_le32(((queue_size - 1) << 16) | qpid),
      .cdw11 = cpu_to_le32(NVME_SQ_PC | (qpid << 16)),
  };
  printf("[IO Queue Pair] %d NVME_ADM_CMD_CREATE_SQ sq iova: 0x%lx\n", qpid,
         qp->sq.iova);
  if (nvme_admin_cmd_sync(nvme_device, &cmd)) {
    fprintf(stderr, "Failed to create SQ io queue [%u]\n", qpid);
    goto out_error;
  }
  return true;
out_error:
  //   nvme_free_queue_pair(qp);
  return false;
}

struct nvme_device *nvme_init(const char *filename) {
  struct nvme_device *nvme_device;
  volatile NvmeBar *regs = NULL;
  struct nvme_qp *admin_qp;
  int ret;
  uint64_t cap;
  uint64_t timeout_ms;
  nvme_device = calloc(sizeof(*nvme_device), 1);
  nvme_parse_filename(filename, &nvme_device->name);

  nvme_device->vfio = propig_vfio_init(nvme_device->name.bdf);
  if (!nvme_device->vfio) {
    fprintf(stderr, "init vfio pci failed: %s", strerror(errno));
    return NULL;
  }
  // bar 0, offset 0
  // void *test_vfio_pci_map_bar(struct test_vfio *s, int index, uint64_t
  // offset, uint64_t size, int prot);
  regs = propig_vfio_pci_map_bar(nvme_device->vfio, 0, 0, sizeof(NvmeBar),
                                 PROT_READ | PROT_WRITE);
  if (regs == NULL) {
    fprintf(stderr, "map nvme bar 0, offset 0 failed: %s", strerror(errno));
    return NULL;
  }

  cap = le64_to_cpu(regs->cap);
  if (!NVME_CAP_CSS(cap)) {
    fprintf(stderr, "Device doesn't support NVMe command set");
    goto out;
  }

  nvme_device->page_size = 1u << (12 + NVME_CAP_MPSMIN(cap));
  nvme_device->doorbell_scale = (4 << NVME_CAP_DSTRD(cap)) / sizeof(uint32_t);
  timeout_ms = MIN(500 * NVME_CAP_TO(cap), 30000);

  /* Reset device to get a clean state. */
  regs->cc = cpu_to_le32(le32_to_cpu(regs->cc) & 0xFE);
  printf("[Reset Device] cc: 0x%x\n", regs->cc);
  ret = wait_for_reset(timeout_ms, regs);
  if (ret != 0) {
    goto out;
  }
  // doorbells: bar 0, offset 4096(BAR SIZE);
  nvme_device->doorbells = propig_vfio_pci_map_bar(
      nvme_device->vfio, 0, sizeof(NvmeBar) /* offset */, NVME_DOORBELL_SIZE,
      PROT_WRITE);
  if (!nvme_device->doorbells) {
    ret = -EINVAL;
    goto out;
  }

  // create admin qp;
  admin_qp = nvme_create_queue_pair(nvme_device, 0);
  FAIL_Z(admin_qp);
  regs->aqa =
      cpu_to_le32((uint32_t)(((NVME_QUEUE_SIZE - 1) << AQA_ACQS_SHIFT) |
                             ((NVME_QUEUE_SIZE - 1) << AQA_ASQS_SHIFT)));
  regs->asq = cpu_to_le64(admin_qp->sq.iova);
  regs->acq = cpu_to_le64(admin_qp->cq.iova);
  printf("[Admin QP] sq iova: %lu cq iova: %lu\n", admin_qp->sq.iova,
         admin_qp->cq.iova);

  // enable the device.
  regs->cc =
      cpu_to_le32((uint32_t)(ctz32(NVME_CQ_ENTRY_BYTES) << CC_IOCQES_SHIFT) |
                  (ctz32(NVME_SQ_ENTRY_BYTES) << CC_IOSQES_SHIFT) | CC_EN_MASK);
  printf("[Enable Device] cc: 0x%x\n", regs->cc);
  ret = wait_for_ready(timeout_ms, regs);
  if (ret != 0) {
    goto out;
  }

  // init irq
  nvme_device->irqfd = eventfd(0, 0);
  ret = propig_vfio_pci_init_irq(nvme_device->vfio, nvme_device->irqfd,
                                 VFIO_PCI_MSIX_IRQ_INDEX);
  if (ret) {
    goto out;
  }
  if (!nvme_identify(nvme_device, nvme_device->name.nsid)) {
    ret = -EIO;
    goto out;
  }

  /* Set up command queues. */
  if (!nvme_add_io_queue(nvme_device)) {
    ret = -EIO;
  }
  // if (!nvme_add_io_queue(nvme_device)) {
  //   ret = -EIO;
  // }

  // if (!nvme_add_io_queue(nvme_device)) {
  //   ret = -EIO;
  // }
  nvme_device->pid = getpid();
  // TODO: unmap regs.
  return nvme_device;
out:
  return NULL;
}

size_t get_iovec_size(struct iovec *iov, int iovlen) {
  size_t size = 0;

  while (iovlen > 0) {
    size += iov->iov_len;
    iov++;
    iovlen--;
  }

  return size;
}

int nvme_submit_io(struct nvme_device *nvme_device, uint32_t qpid,
                   uint64_t offset, uint64_t bytes, struct iovec *iov,
                   size_t iovcnt, bool is_write, int flags, nvme_command_cb cb,
                   void *opaque) {
  struct nvme_qp *qp;
  struct nvme_req *req;
  uint64_t *pagelist;
  int entries = 0;
  int i;
  int j;
  size_t size;
  NvmeCmd *cmd;
  uint32_t blkshift = nvme_device->blkshift;
  uint32_t cdw12 = (((bytes >> blkshift) - 1) & 0xFFFF) |
                   (flags & NVME_IO_FLAGS_FUA ? 1 << 30 : 0);
  if (qpid >= nvme_device->qp_cnt || qpid == 0) {
    fprintf(stderr, "ERROR: bad qpid, should qpid(%d) < qp_cnt(%d)\n", qpid,
            nvme_device->qp_cnt);
    return -1;
  }
  if (nvme_device->pid != getpid()) {
    printf("[BUG] maybe fork(%d != %d)?\n", nvme_device->pid, getpid());
    exit(EXIT_FAILURE);
  }
  qp = nvme_device->qps[qpid];
  cmd = calloc(1, sizeof(*cmd));
  cmd->opcode = is_write ? NVME_CMD_WRITE : NVME_CMD_READ;
  cmd->nsid = cpu_to_le32(nvme_device->name.nsid);
  cmd->cdw10 = cpu_to_le32((uint32_t)((offset >> blkshift) & 0xFFFFFFFF));
  cmd->cdw11 =
      cpu_to_le32((uint32_t)(((offset >> blkshift) >> 32) & 0xFFFFFFFF));
  cmd->cdw12 = cpu_to_le32(cdw12);
  req = nvme_get_free_req(qp);
  assert(req);
  pagelist = req->prp_list_page;

  // printf("[Submit IO] nsid: %d qpid: %d req: %p pagelist: %lu\n", cmd->nsid,
  //        qpid, req, pagelist);
  size = get_iovec_size(iov, iovcnt);
  assert(IS_ALIGNED(size, nvme_device->page_size));
  // one prp page could save all io buffer, the max io buffer is 4k/8 * 4k
  // = 2MiB;
  assert(size / nvme_device->page_size <=
         nvme_device->page_size / sizeof(uint64_t));
  for (i = 0; i < iovcnt; ++i) {
    uint64_t iova = (uint64_t)iov[i].iov_base;
    // TODO: lookup the dma map.
    propig_vfio_dma_map(nvme_device->vfio, iov[i].iov_base, iov[i].iov_len,
                        iova);
    for (j = 0; j < iov[i].iov_len / nvme_device->page_size; j++) {
      pagelist[entries++] = cpu_to_le64(iova + j * nvme_device->page_size);
    }
  }

  // printf("entries: %d\n", entries);
  assert(entries <= nvme_device->page_size / sizeof(uint64_t));
  switch (entries) {
  case 0:
    abort();
  case 1:
    cmd->dptr.prp1 = pagelist[0];
    cmd->dptr.prp2 = 0;
    break;
  case 2:
    cmd->dptr.prp1 = pagelist[0];
    cmd->dptr.prp2 = pagelist[1];
    break;
  default:
    cmd->dptr.prp1 = pagelist[0];
    // Bigger than 2 pages, save the prp_list_page iova to the prp2.
    cmd->dptr.prp2 = cpu_to_le64(req->prp_list_iova + sizeof(uint64_t));
    break;
  }
  req->cb = cb;
  req->opaque = opaque;
  cmd->cid = cpu_to_le16(req->cid);
  nvme_submit_command(qp, cmd);

  // // memset(cmd, 0xfe, sizeof(*cmd));
  //   // printf("%d: memset(0x%lx 0xfe)\n", qp->index, (void*)(qp->sq.queue +
  //   64));

  // memset(qp->sq.queue + 64, 0xfe, 64);

  // qp->sq.tail = (qp->sq.tail + 1) % NVME_QUEUE_SIZE;
  // qp->need_kick++;
  // nvme_kick(qp);
  return 0;
}

#if 0
bool nvme_irq_trigger(struct nvme_device *nvme_device) {
    struct pollfd pfds[1];
    size_t nfds;
    nfds = ARRAY_SIZE(pfds);
    pfds[0].fd = nvme_device->irqfd;
    pfds[0].events = POLLIN;
    ready = poll(pfds, nfds, -1);
    for (int j = 0; j < nfds; j++) {
        char buf[10];

        if (pfds[j].revents != 0) {
            printf("  fd=%d; events: %s%s%s\n", pfds[j].fd,
                   (pfds[j].revents & POLLIN) ? "POLLIN " : "",
                   (pfds[j].revents & POLLHUP) ? "POLLHUP " : "",
                   (pfds[j].revents & POLLERR) ? "POLLERR " : "");

            if (pfds[j].revents & POLLIN) {
                ssize_t s = read(pfds[j].fd, buf, sizeof(buf));
                if (s == -1) return -1;
                printf("    read %zd bytes: %.*s\n", s, (int)s, buf);
            } else { /* POLLERR | POLLHUP */
                printf("    closing fd %d\n", pfds[j].fd);
                if (close(pfds[j].fd) == -1) return -1;
            }
        }
    }
    return 0;
}
#endif

int nvme_poll_cq(struct nvme_qp *qp) {
  // CQ: [head, tail), kernel put cq at tail.
  uint16_t cid;
  struct nvme_queue *cq = &qp->cq;
  struct nvme_req *req;
  int ret;
  struct NvmeCqe *cqe;

  // Device will change the cq queue, so add barrier here.
  rmb();
  cqe = (struct NvmeCqe *)(cq->queue + cq->head * NVME_CQ_ENTRY_BYTES);
  // Phase Tag (P):
  // Identifies whether a Completion Queue entry is new. The
  // Phase Tag values for all Completion Queue entries shall be initialized to
  // ‘0’ by host software prior to setting CC.EN to ‘1’. When the controller
  // places an entry in the Completion Queue, the controller shall invert the
  // Phase Tag to enable host software to discriminate a new entry.
  // Specifically, for the first set of completion queue entries after CC.EN is
  // set to ‘1’ all Phase Tags are set to ‘1’ when they are posted. For the
  // second set of completion queue entries, when the controller has wrapped
  // around to the top of the Completion Queue, all Phase Tags are cleared to
  // ‘0’ when they are posted. The value of the Phase Tag is inverted each pass
  // through the Completion Queue.
  // printf("[POLL CQ %d] cqe: cid: %d status: %d \n", qp->index, cqe->cid,
  // cqe->status);
  if ((le16_to_cpu(cqe->status) & 0x1) == qp->cq_phase) {
    // No new cqe.
    return 0;
  }
  cq->head = (cq->head + 1) % NVME_QUEUE_SIZE;
  if (!cq->head) {
    qp->cq_phase = !qp->cq_phase;
  }
  wmb();
  cid = le16_to_cpu(cqe->cid);
  if (cid == 0 || cid >= NVME_QUEUE_SIZE) {
    fprintf(stderr,
            "NVMe: Unexpected CID in completion queue: %" PRIu32 ", "
            "queue size: %u\n",
            cid, NVME_QUEUE_SIZE);
    return 0;
  }
  req = &qp->reqs[cid - 1];
  assert(req->cid == cid);
  qp->inflight--;
  ret = nvme_translate_error(cqe);
  req->cb(req->opaque, ret);
  req->is_used = false;
  // printf("req cid done: %d\n", cid);

  // Notify the device that it could put more cqes.
  *cq->doorbell = cpu_to_le32(cq->head);
  return 1;
}

int nvme_poll(struct nvme_device *nvme_device) {
  int i;
  int nr = 0;
  // Polling each qp.
  for (i = 1; i < nvme_device->qp_cnt; ++i) {
    struct nvme_qp *qp = nvme_device->qps[i];
    // printf("qpid: %d nr: %d\n", i, nr);
    // qemu_hexdump(stderr, "fuck", qp->sq.queue, 128);
    nr += nvme_poll_cq(qp);
  }
  return nr;
}

uint64_t nvme_get_blocksize(const struct nvme_device *nd) {
  return 1ULL << nd->blkshift;
}

uint64_t nvme_get_numblocks(const struct nvme_device *nd) { return nd->nsze; }
