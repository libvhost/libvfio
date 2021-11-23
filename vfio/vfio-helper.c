#include "vfio-helper.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>   // PATH_MAX
#include <linux/pci_regs.h> // PCI_COMMAND
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct propig_vfio {
  int container;
  int group;
  int device;
  struct vfio_region_info config_region_info;
  struct vfio_region_info bar_region_info[6];
  struct vfio_iova_range *iova_range;
  int nr_iovas;
};

void rc_die(const char *reason) {
  fprintf(stderr, "%s errno: %s\n", reason, strerror(errno));
  exit(EXIT_FAILURE);
}

static int open_vfio_group(const char *bdf) {
  char link[PATH_MAX];
  char sysfs_group[PATH_MAX];
  char dev_group[PATH_MAX];
  int fd;
  char *p;
  int len;
  snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s/iommu_group", bdf);
  len = readlink(link, sysfs_group, PATH_MAX - 1);
  if (len == -1) {
    fprintf(stderr, "readlink %s failed, errno: %s\n", link, strerror(errno));
    exit(EXIT_FAILURE);
  }
  sysfs_group[len] = 0;
  p = strrchr(sysfs_group, '/');
  snprintf(dev_group, sizeof(dev_group), "/dev/vfio/%s", p + 1);
  fd = open(dev_group, O_RDWR);
  return fd;
}

static void parse_iova_ranges(struct propig_vfio *s,
                              struct vfio_iommu_type1_info *info) {
  struct vfio_info_cap_header *cap = (void *)info + info->cap_offset;
  struct vfio_iommu_type1_info_cap_iova_range *cap_iova_range;
  int i;

  while (cap->id != VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE) {
    if (!cap->next) {
      return;
    }
    cap = (struct vfio_info_cap_header *)((void *)info + cap->next);
  }

  cap_iova_range = (struct vfio_iommu_type1_info_cap_iova_range *)cap;

  s->nr_iovas = cap_iova_range->nr_iovas;

  s->iova_range = calloc(sizeof(struct vfio_iova_range), s->nr_iovas);
  for (i = 0; i < s->nr_iovas; i++) {
    s->iova_range[i].start = cap_iova_range->iova_ranges[i].start;
    s->iova_range[i].end = cap_iova_range->iova_ranges[i].end;
  }
}

static int vfio_pci_init_bar(struct propig_vfio *s, int index) {
  s->bar_region_info[index] = (struct vfio_region_info){
      .index = VFIO_PCI_BAR0_REGION_INDEX + index,
      .argsz = sizeof(struct vfio_region_info),
  };
  if (ioctl(s->device, VFIO_DEVICE_GET_REGION_INFO,
            &s->bar_region_info[index])) {
    fprintf(stderr, "Failed to get BAR region info: %s", strerror(errno));
    return -errno;
  }
  return 0;
}

int propig_vfio_pci_read_config(struct propig_vfio *s, void *buf, int size,
                                int offset) {
  int ret;
  assert(IS_ALIGNED(s->config_region_info.offset + offset, size));
  do {
    ret = pread(s->device, buf, size, s->config_region_info.offset + offset);
  } while (ret == -1 && errno == EINTR);
  return ret == size ? 0 : -errno;
}

int propig_vfio_pci_write_config(struct propig_vfio *s, void *buf, int size,
                                 int offset) {
  int ret;
  assert(IS_ALIGNED(s->config_region_info.offset + offset, size));
  do {
    ret = pwrite(s->device, buf, size, s->config_region_info.offset + offset);
  } while (ret == -1 && errno == EINTR);
  return ret == size ? 0 : -errno;
}

static int vfio_enable_bus_master(struct propig_vfio *s) {
  uint16_t pci_cmd;
  int ret;
  /* Enable bus master */
  ret = propig_vfio_pci_read_config(s, &pci_cmd, sizeof(pci_cmd), PCI_COMMAND);
  if (ret) {
    return ret;
  }
  pci_cmd |= PCI_COMMAND_MASTER;
  ret = propig_vfio_pci_write_config(s, &pci_cmd, sizeof(pci_cmd), PCI_COMMAND);
  return ret;
}

struct propig_vfio *propig_vfio_init(const char *device) {
  int ret;
  struct vfio_group_status group_status = {.argsz = sizeof(group_status)};
  struct vfio_iommu_type1_info *iommu_info = NULL;
  size_t iommu_info_size = sizeof(*iommu_info);
  struct vfio_device_info device_info = {.argsz = sizeof(device_info)};
  int i;
  struct propig_vfio *s;
  s = calloc(1, sizeof(*s));

  /* Create a new container */
  s->container = open("/dev/vfio/vfio", O_RDWR);
  if (s->container == -1) {
    perror("Failed to open /dev/vfio/vfio");
    free(s);
    return NULL;
  }

  if (ioctl(s->container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
    fprintf(stderr, "Invalid VFIO version: %s\n", strerror(errno));
    ret = -EINVAL;
    goto fail_container;
  }

  if (!ioctl(s->container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
    fprintf(stderr, "VFIO IOMMU Type1 is not supported: %s\n", strerror(errno));
    ret = -EINVAL;
    goto fail_container;
  }

  s->group = open_vfio_group(device);
  if (s->group == -1) {
    ret = -errno;
    goto fail_container;
  }

  /* Test the group is viable and available */
  if (ioctl(s->group, VFIO_GROUP_GET_STATUS, &group_status)) {
    fprintf(stderr, "Failed to get VFIO group status: %s\n", strerror(errno));
    ret = -errno;
    goto fail;
  }

  if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    fprintf(stderr, "VFIO group is not viable: %s\n", strerror(errno));
    ret = -EINVAL;
    goto fail;
  }

  /* Add the group to the container */
  if (ioctl(s->group, VFIO_GROUP_SET_CONTAINER, &s->container)) {
    fprintf(stderr, "Failed to add group to VFIO container: %s\n",
            strerror(errno));
    ret = -errno;
    goto fail;
  }

  /* Enable the IOMMU model we want */
  if (ioctl(s->container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) {
    fprintf(stderr, "Failed to set VFIO IOMMU type: %s\n", strerror(errno));
    ret = -errno;
    goto fail;
  }
  iommu_info = calloc(1, iommu_info_size);
  iommu_info->argsz = iommu_info_size;

  /* Get additional IOMMU info */
  if (ioctl(s->container, VFIO_IOMMU_GET_INFO, iommu_info)) {
    fprintf(stderr, "Failed to get IOMMU info: %s\n", strerror(errno));
    ret = -errno;
    goto fail;
  }

  if (iommu_info->argsz > iommu_info_size) {
    iommu_info_size = iommu_info->argsz;
    free(iommu_info);
    iommu_info = calloc(1, iommu_info_size);
    iommu_info->argsz = iommu_info_size;
    if (ioctl(s->container, VFIO_IOMMU_GET_INFO, iommu_info)) {
      ret = -errno;
      goto fail;
    }
  }

  parse_iova_ranges(s, iommu_info);
  free(iommu_info);

  s->device = ioctl(s->group, VFIO_GROUP_GET_DEVICE_FD, device);

  if (s->device < 0) {
    fprintf(stderr, "Failed to get device fd: %s\n", strerror(errno));
    ret = -errno;
    goto fail;
  }

  /* Test and setup the device */
  if (ioctl(s->device, VFIO_DEVICE_GET_INFO, &device_info)) {
    fprintf(stderr, "Failed to get device info: %s\n", strerror(errno));
    ret = -errno;
    goto fail;
  }

  if (device_info.num_regions < VFIO_PCI_CONFIG_REGION_INDEX) {
    fprintf(stderr, "Invalid device regions\n");
    ret = -EINVAL;
    goto fail;
  }

  s->config_region_info = (struct vfio_region_info){
      .index = VFIO_PCI_CONFIG_REGION_INDEX,
      .argsz = sizeof(struct vfio_region_info),
  };
  if (ioctl(s->device, VFIO_DEVICE_GET_REGION_INFO, &s->config_region_info)) {
    fprintf(stderr, "Failed to get config region info: %s\n", strerror(errno));
    ret = -errno;
    goto fail;
  }

  for (i = 0; i < ARRAY_SIZE(s->bar_region_info); i++) {
    ret = vfio_pci_init_bar(s, i);
    if (ret) {
      goto fail;
    }
  }

  ret = vfio_enable_bus_master(s);
  if (ret < 0) {
    fprintf(stderr, "Failed to enable bus master: %s\n", strerror(errno));
    ret = -errno;
    goto fail;
  }
  return s;

fail:
  free(iommu_info);
  close(s->group);

fail_container:
  close(s->container);
  return NULL;
}

void *propig_vfio_pci_map_bar(struct propig_vfio *s, int index, uint64_t offset,
                              uint64_t size, int prot) {
  void *p;
  assert(IS_ALIGNED(offset, 4096));
  p = mmap(NULL, MIN(size, s->bar_region_info[index].size - offset), prot,
           MAP_SHARED, s->device, s->bar_region_info[index].offset + offset);
  if (p == MAP_FAILED) {
    fprintf(stderr, "Failed to map BAR region: %s\n", strerror(errno));
    printf("[MAP BAR %d] offset: 0x%lx, size: 0x%lx, bar size: 0x%llx, bar "
           "offset: "
           "0x%llx\n",
           index, offset, size, s->bar_region_info[index].size,
           s->bar_region_info[index].offset);
    p = NULL;
  }
  return p;
}

int propig_vfio_pci_init_irq(struct propig_vfio *s, int efd, int irq_type) {
  int r;
  struct vfio_irq_set *irq_set;
  size_t irq_set_size;
  struct vfio_irq_info irq_info = {.argsz = sizeof(irq_info)};

  irq_info.index = irq_type;
  if (ioctl(s->device, VFIO_DEVICE_GET_IRQ_INFO, &irq_info)) {
    fprintf(stderr, "Failed to get device interrupt info: %s", strerror(errno));
    return -errno;
  }
  if (!(irq_info.flags & VFIO_IRQ_INFO_EVENTFD)) {
    fprintf(stderr, "Device interrupt doesn't support eventfd\n");
    return -EINVAL;
  }

  irq_set_size = sizeof(*irq_set) + sizeof(int);
  irq_set = malloc(irq_set_size);

  /* Get to a known IRQ state */
  *irq_set = (struct vfio_irq_set){
      .argsz = irq_set_size,
      .flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
      .index = irq_info.index,
      .start = 0,
      .count = 1,
  };

  *(int *)&irq_set->data = efd;
  r = ioctl(s->device, VFIO_DEVICE_SET_IRQS, irq_set);
  free(irq_set);
  if (r) {
    fprintf(stderr, "Failed to setup device interrupt: %s", strerror(errno));
    return -errno;
  }
  return 0;
}
// host, iova, size must align to 4k.
int propig_vfio_dma_map(struct propig_vfio *s, const void *host, uint64_t size,
                        uint64_t iova) {
  struct vfio_iommu_type1_dma_map dma_map = {
      .argsz = sizeof(dma_map),
      .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
      .iova = iova,
      .vaddr = (uintptr_t)host,
      .size = size,
  };

  if (ioctl(s->container, VFIO_IOMMU_MAP_DMA, &dma_map)) {
    // fprintf(stderr,
    //         "VFIO_MAP_DMA failed: %s, pid: %d, container fd: %d, iova: 0x%lx
    //         " "vaddr: %p " "size: 0x%lx\n", strerror(errno), getpid(),
    //         s->container, iova, host, size);
    return -errno;
  }
  return 0;
}
