#include "ioat.h"
#include "ioat_internal.h"
#include "ioat_spec.h"
#include "mmio.h"
#include "vfio-helper.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <error.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define propig_min(a, b) (((a) < (b)) ? (a) : (b))

static uint64_t ioat_get_chansts(struct propig_ioat_chan *ioat) {
  return propig_mmio_read_8(&ioat->regs->chansts);
}

static void ioat_write_chancmp(struct propig_ioat_chan *ioat, uint64_t addr) {
  propig_mmio_write_8(&ioat->regs->chancmp, addr);
}

static void ioat_write_chainaddr(struct propig_ioat_chan *ioat, uint64_t addr) {
  propig_mmio_write_8(&ioat->regs->chainaddr, addr);
}

static inline void ioat_suspend(struct propig_ioat_chan *ioat) {
  ioat->regs->chancmd = PROPIG_IOAT_CHANCMD_SUSPEND;
}

static inline void ioat_reset(struct propig_ioat_chan *ioat) {
  ioat->regs->chancmd = PROPIG_IOAT_CHANCMD_RESET;
}

static inline uint32_t ioat_reset_pending(struct propig_ioat_chan *ioat) {
  uint8_t cmd;

  cmd = ioat->regs->chancmd;
  return (cmd & PROPIG_IOAT_CHANCMD_RESET) == PROPIG_IOAT_CHANCMD_RESET;
}

static inline uint32_t ioat_get_active(struct propig_ioat_chan *ioat) {
  return (ioat->head - ioat->tail) & ((1 << ioat->ring_size_order) - 1);
}

static inline uint32_t ioat_get_ring_space(struct propig_ioat_chan *ioat) {
  return (1 << ioat->ring_size_order) - ioat_get_active(ioat) - 1;
}

static uint32_t ioat_get_ring_index(struct propig_ioat_chan *ioat,
                                    uint32_t index) {
  return index & ((1 << ioat->ring_size_order) - 1);
}

static void ioat_get_ring_entry(struct propig_ioat_chan *ioat, uint32_t index,
                                struct ioat_descriptor **desc,
                                union propig_ioat_hw_desc **hw_desc) {
  uint32_t i = ioat_get_ring_index(ioat, index);

  *desc = &ioat->ring[i];
  *hw_desc = &ioat->hw_ring[i];
}

static void ioat_submit_single(struct propig_ioat_chan *ioat) { ioat->head++; }

void propig_ioat_flush(struct propig_ioat_chan *ioat) {
  uint32_t index = ioat_get_ring_index(ioat, ioat->head - 1);
  union propig_ioat_hw_desc *hw_desc;

  hw_desc = &ioat->hw_ring[index];
  hw_desc->dma.u.control.completion_update = 1;
  ioat->regs->dmacount = (uint16_t)ioat->head;
}

static struct ioat_descriptor *ioat_prep_null(struct propig_ioat_chan *ioat) {
  struct ioat_descriptor *desc;
  union propig_ioat_hw_desc *hw_desc;

  if (ioat_get_ring_space(ioat) < 1) {
    return NULL;
  }

  ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

  hw_desc->dma.u.control_raw = 0;
  hw_desc->dma.u.control.op = PROPIG_IOAT_OP_COPY;
  hw_desc->dma.u.control.null = 1;

  hw_desc->dma.size = 8;
  hw_desc->dma.src_addr = 0;
  hw_desc->dma.dest_addr = 0;

  desc->callback_fn = NULL;
  desc->callback_arg = NULL;

  ioat_submit_single(ioat);

  return desc;
}

static struct ioat_descriptor *ioat_prep_copy(struct propig_ioat_chan *ioat,
                                              uint64_t dst, uint64_t src,
                                              uint32_t len) {
  struct ioat_descriptor *desc;
  union propig_ioat_hw_desc *hw_desc;

  assert(len <= ioat->max_xfer_size);

  if (ioat_get_ring_space(ioat) < 1) {
    return NULL;
  }

  ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

  hw_desc->dma.u.control_raw = 0;
  hw_desc->dma.u.control.op = PROPIG_IOAT_OP_COPY;

  hw_desc->dma.size = len;
  hw_desc->dma.src_addr = src;
  hw_desc->dma.dest_addr = dst;

  desc->callback_fn = NULL;
  desc->callback_arg = NULL;

  ioat_submit_single(ioat);

  return desc;
}

static struct ioat_descriptor *ioat_prep_fill(struct propig_ioat_chan *ioat,
                                              uint64_t dst,
                                              uint64_t fill_pattern,
                                              uint32_t len) {
  struct ioat_descriptor *desc;
  union propig_ioat_hw_desc *hw_desc;

  assert(len <= ioat->max_xfer_size);

  if (ioat_get_ring_space(ioat) < 1) {
    return NULL;
  }

  ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

  hw_desc->fill.u.control_raw = 0;
  hw_desc->fill.u.control.op = PROPIG_IOAT_OP_FILL;

  hw_desc->fill.size = len;
  hw_desc->fill.src_data = fill_pattern;
  hw_desc->fill.dest_addr = dst;

  desc->callback_fn = NULL;
  desc->callback_arg = NULL;

  ioat_submit_single(ioat);

  return desc;
}

static int ioat_reset_hw(struct propig_ioat_chan *ioat) {
  int timeout;
  uint64_t status;
  uint32_t chanerr;
  int rc;

  status = ioat_get_chansts(ioat);
  if (is_ioat_active(status) || is_ioat_idle(status)) {
    ioat_suspend(ioat);
  }

  timeout = 20; /* in milliseconds */
  while (is_ioat_active(status) || is_ioat_idle(status)) {
    usleep(1000);
    timeout--;
    if (timeout == 0) {
      printf("timed out waiting for suspend\n");
      return -1;
    }
    status = ioat_get_chansts(ioat);
  }

  /*
   * Clear any outstanding errors.
   * CHANERR is write-1-to-clear, so write the current CHANERR bits back to
   * reset everything.
   */
  chanerr = ioat->regs->chanerr;
  ioat->regs->chanerr = chanerr;

  if (ioat->regs->cbver < PROPIG_IOAT_VER_3_3) {
    rc = propig_vfio_pci_read_config(ioat->vfio, &chanerr, 4,
                                     PROPIG_IOAT_PCI_CHANERR_INT_OFFSET);
    if (rc) {
      printf("failed to read the internal channel error register\n");
      return -1;
    }
    propig_vfio_pci_write_config(ioat->vfio, &chanerr, 4,
                                 PROPIG_IOAT_PCI_CHANERR_INT_OFFSET);
  }

  ioat_reset(ioat);

  timeout = 20;
  while (ioat_reset_pending(ioat)) {
    usleep(1000);
    timeout--;
    if (timeout == 0) {
      printf("timed out waiting for reset\n");
      return -1;
    }
  }

  return 0;
}

int propig_ioat_process_events(struct propig_ioat_chan *ioat) {
  struct ioat_descriptor *desc;
  uint64_t status, completed_descriptor, hw_desc_phys_addr, events_count = 0;
  uint32_t tail;

  if (ioat->head == ioat->tail) {
    return 0;
  }

  status = *ioat->comp_update;
  completed_descriptor = status & PROPIG_IOAT_CHANSTS_COMPLETED_DESCRIPTOR_MASK;

  if (is_ioat_halted(status)) {
    printf("Channel halted (%x)\n", ioat->regs->chanerr);
    return -1;
  }

  if (completed_descriptor == ioat->last_seen) {
    return 0;
  }

  do {
    tail = ioat_get_ring_index(ioat, ioat->tail);
    desc = &ioat->ring[tail];

    if (desc->callback_fn) {
      desc->callback_fn(desc->callback_arg, 0 /* ioat->regs->chanerr */);
    }

    hw_desc_phys_addr = desc->phys_addr;
    ioat->tail++;
    events_count++;
  } while (hw_desc_phys_addr != completed_descriptor);

  ioat->last_seen = hw_desc_phys_addr;

  return events_count;
}

uint32_t propig_ioat_get_max_descriptors(struct propig_ioat_chan *ioat) {
  return 1 << ioat->ring_size_order;
}

// ioat://0000:44:00.0
static bool ioat_parse_filename(const char *filename, struct ioat_name *name) {
  int ret = 0;
  ret = sscanf(filename, "ioat://%13s", name->bdf);
  // printf("IOAT BDF: %s\n", name->bdf);
  return ret == 1;
}

struct propig_ioat_chan *ioat_channel_init(const char *filename) {
  uint8_t xfercap, version;
  uint64_t status;
  int i, num_descriptors;
  uint64_t comp_update_bus_addr = 0;
  uint64_t hw_ring_iova;
  struct propig_ioat_chan *ioat;

  ioat = calloc(1, sizeof(struct propig_ioat_chan));
  if (ioat == NULL) {
    return NULL;
  }

  if (!ioat_parse_filename(filename, &ioat->name)) {
    free(ioat);
    goto failed;
  }

  ioat->vfio = propig_vfio_init(ioat->name.bdf);
  if (!ioat->vfio) {
    // fprintf(stderr, "init vfio pci failed: %s\n", strerror(errno));
    goto failed;
  }
  // bar 0, offset 0 (volatile struct propig_ioat_registers *)
  ioat->regs = propig_vfio_pci_map_bar(ioat->vfio, 0, 0,
                                       sizeof(struct propig_ioat_registers),
                                       PROT_READ | PROT_WRITE);
  if (ioat->regs == NULL) {
    fprintf(stderr, "map nvme bar 0, offset 0 failed: %s\n", strerror(errno));
    goto failed;
  }

  version = ioat->regs->cbver;
  if (version < PROPIG_IOAT_VER_3_0) {
    fprintf(stderr, "unsupported IOAT version %u.%u\n", version >> 4,
            version & 0xF);
    goto failed;
  }

  /* Always support DMA copy */
  ioat->dma_capabilities = PROPIG_IOAT_ENGINE_COPY_SUPPORTED;
  if (ioat->regs->dmacapability & PROPIG_IOAT_DMACAP_BFILL) {
    ioat->dma_capabilities |= PROPIG_IOAT_ENGINE_FILL_SUPPORTED;
  }
  xfercap = ioat->regs->xfercap;

  /* Only bits [4:0] are valid. */
  xfercap &= 0x1f;
  if (xfercap == 0) {
    /* 0 means 4 GB max transfer size. */
    ioat->max_xfer_size = 1ULL << 32;
  } else if (xfercap < 12) {
    /* XFERCAP must be at least 12 (4 KB) according to the spec. */
    fprintf(stderr, "invalid XFERCAP value %u\n", xfercap);
    goto failed;
  } else {
    ioat->max_xfer_size = 1U << xfercap;
  }

  if (posix_memalign((void **)&ioat->comp_update, PAGE_SIZE, PAGE_SIZE)) {
    goto failed;
  }

  // TODO: use va;
  comp_update_bus_addr = (uint64_t)ioat->comp_update;
  propig_vfio_dma_map(ioat->vfio, (void *)ioat->comp_update, PAGE_SIZE,
                      comp_update_bus_addr);

  ioat->ring_size_order = IOAT_DEFAULT_ORDER;

  num_descriptors = 1 << ioat->ring_size_order;

  ioat->ring = calloc(num_descriptors, sizeof(struct ioat_descriptor));
  if (!ioat->ring) {
    goto failed;
  }
  // TODO: Align to 64;
  ioat->hw_ring = calloc(num_descriptors, sizeof(union propig_ioat_hw_desc));
  if (!ioat->hw_ring) {
    goto failed;
  }
  hw_ring_iova = (uint64_t)ioat->hw_ring;
  propig_vfio_dma_map(ioat->vfio, (void *)ioat->hw_ring,
                      num_descriptors * sizeof(union propig_ioat_hw_desc),
                      hw_ring_iova);
  for (i = 0; i < num_descriptors; i++) {
    hw_ring_iova = (uint64_t)(&ioat->hw_ring[i]);
    ioat->ring[i].phys_addr = hw_ring_iova;
    ioat->hw_ring[ioat_get_ring_index(ioat, i - 1)].generic.next = hw_ring_iova;
  }

  ioat->head = 0;
  ioat->tail = 0;
  ioat->last_seen = 0;

  ioat_reset_hw(ioat);

  ioat->regs->chanctrl = PROPIG_IOAT_CHANCTRL_ANY_ERR_ABORT_EN;
  ioat_write_chancmp(ioat, comp_update_bus_addr);
  ioat_write_chainaddr(ioat, ioat->ring[0].phys_addr);

  ioat_prep_null(ioat);
  propig_ioat_flush(ioat);

  i = 100;
  while (i-- > 0) {
    usleep(100);
    status = ioat_get_chansts(ioat);
    if (is_ioat_idle(status)) {
      break;
    }
  }

  if (is_ioat_idle(status)) {
    propig_ioat_process_events(ioat);
  } else {
    fprintf(stderr, "could not start channel: status = %p\n error = %#x\n",
            (void *)status, ioat->regs->chanerr);
    goto failed;
  }
  return ioat;
failed:
  free(ioat);
  return NULL;
}

struct propig_pci_addr {
  uint32_t domain;
  uint8_t bus;
  uint8_t dev;
  uint8_t func;
};

int propig_pci_addr_parse(struct propig_pci_addr *addr, const char *bdf) {
  unsigned domain, bus, dev, func;

  if (addr == NULL || bdf == NULL) {
    return -EINVAL;
  }

  if ((sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &dev, &func) == 4) ||
      (sscanf(bdf, "%x.%x.%x.%x", &domain, &bus, &dev, &func) == 4)) {
    /* Matched a full address - all variables are initialized */
  } else if (sscanf(bdf, "%x:%x:%x", &domain, &bus, &dev) == 3) {
    func = 0;
  } else if ((sscanf(bdf, "%x:%x.%x", &bus, &dev, &func) == 3) ||
             (sscanf(bdf, "%x.%x.%x", &bus, &dev, &func) == 3)) {
    domain = 0;
  } else if ((sscanf(bdf, "%x:%x", &bus, &dev) == 2) ||
             (sscanf(bdf, "%x.%x", &bus, &dev) == 2)) {
    domain = 0;
    func = 0;
  } else {
    return -EINVAL;
  }

  if (bus > 0xFF || dev > 0x1F || func > 7) {
    return -EINVAL;
  }

  addr->domain = domain;
  addr->bus = bus;
  addr->dev = dev;
  addr->func = func;

  return 0;
}

static int parse_sysfs_value(const char *filename, unsigned long *val) {
  FILE *f;
  char buf[BUFSIZ];
  char *end = NULL;

  if ((f = fopen(filename, "r")) == NULL) {
    printf("%s(): cannot open sysfs value %s\n", __func__, filename);
    return -1;
  }

  if (fgets(buf, sizeof(buf), f) == NULL) {
    printf("%s(): cannot read sysfs value %s\n", __func__, filename);
    fclose(f);
    return -1;
  }
  *val = strtoul(buf, &end, 0);
  if ((buf[0] == '\0') || (end == NULL) || (*end != '\n')) {
    printf("%s(): cannot parse sysfs value %s\n", __func__, filename);
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int pci_get_kernel_driver_by_path(const char *filename, char *dri_name,
                                         size_t len) {
  int count;
  char path[PATH_MAX];
  char *name;

  if (!filename || !dri_name)
    return -1;

  count = readlink(filename, path, PATH_MAX);
  if (count >= PATH_MAX)
    return -1;

  /* For device does not have a driver */
  if (count < 0)
    return 1;

  path[count] = '\0';

  name = strrchr(path, '/');
  if (name) {
    snprintf(dri_name, len, name + 1);
    return 0;
  }

  return -1;
}

static int pci_get_vendor_device(const char *dirname, uint16_t *vendor_id,
                                 uint16_t *device_id) {
  char filename[PATH_MAX];
  unsigned long tmp;
  char driver[PATH_MAX];
  int ret;
  /* get vendor id */
  ret = snprintf(filename, sizeof(filename), "%s/vendor", dirname);
  if (ret < 0)
    return -1;
  if (parse_sysfs_value(filename, &tmp) < 0) {
    return -1;
  }
  *vendor_id = (uint16_t)tmp;

  /* get device id */
  ret = snprintf(filename, sizeof(filename), "%s/device", dirname);
  if (ret < 0)
    return -1;
  if (parse_sysfs_value(filename, &tmp) < 0) {
    return -1;
  }
  *device_id = (uint16_t)tmp;

  ret = snprintf(filename, sizeof(filename), "%s/driver", dirname);
  if (ret < 0)
    return -1;
  ret = pci_get_kernel_driver_by_path(filename, driver, sizeof(driver));
  if (ret < 0) {
    printf("Fail to get kernel driver\n");
    return -1;
  }

  if (!ret) {
    if (!strcmp(driver, "vfio-pci"))
      return 0;
    else if (!strcmp(driver, "igb_uio"))
      return 0;
    else if (!strcmp(driver, "uio_pci_generic"))
      return 0;
    else
      return -1;
  }
  return 0;
}

struct propig_ioat_chan **ioat_channel_load(int *cnt) {
  struct dirent *e;
  DIR *dir;
  char dirname[PATH_MAX];
  char filename[PATH_MAX];
  struct propig_pci_addr addr[128];
  int ioat_cnt = 0;
  uint16_t vendor_id;
  uint16_t device_id;
  // Only support Skylake Xeon.
  static uint16_t ioat_vendor_id = 0x8086;
  static uint16_t ioat_device_id = 0x2021;
  struct propig_ioat_chan **ioats;
  int i;
  int j;

#define SYSFS_PCI_DEVICES "/sys/bus/pci/devices"

  dir = opendir(SYSFS_PCI_DEVICES);
  if (dir == NULL) {
    printf("opendir failed: %s\n", strerror(errno));
    *cnt = 0;
    return NULL;
  }

  while ((e = readdir(dir)) != NULL) {
    if (e->d_name[0] == '.')
      continue;
    if (propig_pci_addr_parse(&addr[ioat_cnt], e->d_name) != 0)
      continue;
    snprintf(dirname, sizeof(dirname), "%s/%s", SYSFS_PCI_DEVICES, e->d_name);

    if (pci_get_vendor_device(dirname, &vendor_id, &device_id) < 0)
      continue;

    if (vendor_id == ioat_vendor_id && device_id == ioat_device_id) {
      // Match.
      // printf("Match %d ioat device %04x:%02x:%02x.%x\n", ioat_cnt,
      //        addr[ioat_cnt].domain, addr[ioat_cnt].bus, addr[ioat_cnt].dev,
      //        addr[ioat_cnt].func);
      ioat_cnt++;
    }
  }
  if (ioat_cnt == 0) {
    *cnt = 0;
    return NULL;
  }

  ioats = calloc(ioat_cnt, sizeof(*ioats));
  j = 0;
  for (i = 0; i < ioat_cnt; ++i) {
    snprintf(filename, sizeof(filename), "ioat://%04x:%02x:%02x.%x",
             addr[i].domain, addr[i].bus, addr[i].dev, addr[i].func);
    ioats[j] = ioat_channel_init(filename);
    if (ioats[j] == NULL) {
      // fprintf(stderr, "ioat channel init %s failed: %s \n", filename,
      // strerror(errno));
      continue;
    }
    printf("load %s\n", filename);
    j++;
    if (j == *cnt) {
      break;
    }
  }
  *cnt = j;
  closedir(dir);
  return ioats;
}

int ioat_channel_unload(struct propig_ioat_chan **ioats, int cnt) {
  int i;
  for (i = 0; i < cnt; ++i) {
    // free ioat;
  }
  free(ioats);
  return 0;
}

int propig_ioat_build_copy(struct propig_ioat_chan *ioat, void *cb_arg,
                           propig_ioat_req_cb cb_fn, void *dst, const void *src,
                           uint64_t nbytes) {
  struct ioat_descriptor *last_desc;
  uint64_t remaining, op_size;
  uint64_t vdst, vsrc;
  uint64_t pdst_addr, psrc_addr;
  uint32_t orig_head;

  if (!ioat) {
    return -EINVAL;
  }

  orig_head = ioat->head;

  vdst = (uint64_t)dst;
  vsrc = (uint64_t)src;

  remaining = nbytes;
  while (remaining) {
    op_size = propig_min(remaining, ioat->max_xfer_size);

    // TODO: use va;
    psrc_addr = vsrc;
    pdst_addr = vdst;
    last_desc = ioat_prep_copy(ioat, pdst_addr, psrc_addr, op_size);

    remaining -= op_size;
    if (remaining == 0 || last_desc == NULL) {
      break;
    }

    vsrc += op_size;
    vdst += op_size;
  }
  /* Issue null descriptor for null transfer */
  if (nbytes == 0) {
    last_desc = ioat_prep_null(ioat);
  }

  if (last_desc) {
    last_desc->callback_fn = cb_fn;
    last_desc->callback_arg = cb_arg;
  } else {
    /*
     * Ran out of descriptors in the ring - reset head to leave things as they
     * were in case we managed to fill out any descriptors.
     */
    ioat->head = orig_head;
    return -ENOMEM;
  }

  return 0;
}

int propig_ioat_submit_copy(struct propig_ioat_chan *ioat, void *cb_arg,
                            propig_ioat_req_cb cb_fn, void *dst,
                            const void *src, uint64_t nbytes) {
  int rc;

  rc = propig_ioat_build_copy(ioat, cb_arg, cb_fn, dst, src, nbytes);
  if (rc != 0) {
    return rc;
  }

  propig_ioat_flush(ioat);
  return 0;
}

int propig_ioat_build_fill(struct propig_ioat_chan *ioat, void *cb_arg,
                           propig_ioat_req_cb cb_fn, void *dst,
                           uint64_t fill_pattern, uint64_t nbytes) {
  struct ioat_descriptor *last_desc = NULL;
  uint64_t remaining, op_size;
  uint64_t vdst;
  uint64_t pdst_addr;
  uint32_t orig_head;

  if (!ioat) {
    return -EINVAL;
  }

  if (!(ioat->dma_capabilities & PROPIG_IOAT_ENGINE_FILL_SUPPORTED)) {
    printf("Channel does not support memory fill\n");
    return -1;
  }

  orig_head = ioat->head;

  vdst = (uint64_t)dst;
  remaining = nbytes;

  while (remaining) {
    op_size = propig_min(remaining, ioat->max_xfer_size);

    // TODO: use va;
    pdst_addr = vdst;
    // propig_vfio_dma_map(ioat->vfio, dst, op_size, pdst_addr);

    last_desc = ioat_prep_fill(ioat, pdst_addr, fill_pattern, op_size);

    remaining -= op_size;
    if (remaining == 0 || last_desc == NULL) {
      break;
    }

    vdst += op_size;
  }

  if (last_desc) {
    last_desc->callback_fn = cb_fn;
    last_desc->callback_arg = cb_arg;
  } else {
    /*
     * Ran out of descriptors in the ring - reset head to leave things as they
     * were in case we managed to fill out any descriptors.
     */
    ioat->head = orig_head;
    return -ENOMEM;
  }

  return 0;
}

int propig_ioat_submit_fill(struct propig_ioat_chan *ioat, void *cb_arg,
                            propig_ioat_req_cb cb_fn, void *dst,
                            uint64_t fill_pattern, uint64_t nbytes) {
  int rc;

  rc = propig_ioat_build_fill(ioat, cb_arg, cb_fn, dst, fill_pattern, nbytes);
  if (rc != 0) {
    return rc;
  }

  propig_ioat_flush(ioat);
  return 0;
}

uint32_t propig_ioat_get_dma_capabilities(struct propig_ioat_chan *ioat) {
  if (!ioat) {
    return 0;
  }
  return ioat->dma_capabilities;
}

struct propig_vfio *ioat_get_vfio(struct propig_ioat_chan *chan) {
  return chan->vfio;
}