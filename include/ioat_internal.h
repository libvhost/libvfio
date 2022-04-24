#ifndef __IOAT_INTERNAL_H__
#define __IOAT_INTERNAL_H__

#include "vfio-helper.h"
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/queue.h>

#include "ioat_spec.h"

/* Allocate 1 << 15 (32K) descriptors per channel by default. */
#define IOAT_DEFAULT_ORDER 15

struct ioat_descriptor {
  uint64_t phys_addr;
  propig_ioat_req_cb callback_fn;
  void *callback_arg;
};

struct ioat_name {
  char bdf[32];
};

/* One of these per allocated PCI device. */
struct propig_ioat_chan {
  struct ioat_name name;
  struct propig_vfio *vfio;

  uint64_t max_xfer_size;
  volatile struct propig_ioat_registers *regs;

  volatile uint64_t *comp_update;

  uint32_t head;
  uint32_t tail;

  uint32_t ring_size_order;
  uint64_t last_seen;

  struct ioat_descriptor *ring;
  union propig_ioat_hw_desc *hw_ring;
  uint32_t dma_capabilities;

  /* tailq entry for attached_chans */
  TAILQ_ENTRY(propig_ioat_chan) tailq;
};

static inline uint32_t is_ioat_active(uint64_t status) {
  return (status & PROPIG_IOAT_CHANSTS_STATUS) == PROPIG_IOAT_CHANSTS_ACTIVE;
}

static inline uint32_t is_ioat_idle(uint64_t status) {
  return (status & PROPIG_IOAT_CHANSTS_STATUS) == PROPIG_IOAT_CHANSTS_IDLE;
}

static inline uint32_t is_ioat_halted(uint64_t status) {
  return (status & PROPIG_IOAT_CHANSTS_STATUS) == PROPIG_IOAT_CHANSTS_HALTED;
}

static inline uint32_t is_ioat_suspended(uint64_t status) {
  return (status & PROPIG_IOAT_CHANSTS_STATUS) == PROPIG_IOAT_CHANSTS_SUSPENDED;
}

#endif /* __IOAT_INTERNAL_H__ */
