#ifndef VFIO_HELPERS_H
#define VFIO_HELPERS_H

#include <inttypes.h>
#include <linux/vfio.h>

void rc_die(const char *reason);
#define FAIL_NZ(x)                                                             \
  do {                                                                         \
    if ((x))                                                                   \
      rc_die("error: " #x " failed (returned non-zero).");                     \
  } while (0)
#define FAIL_Z(x)                                                              \
  do {                                                                         \
    if (!(x))                                                                  \
      rc_die("error: " #x " failed (returned zero/null).");                    \
  } while (0)
#define FAIL_LZ(x)                                                             \
  do {                                                                         \
    if (x < 0)                                                                 \
      rc_die("error: " #x " failed (returned less zero).");                    \
  } while (0)

#define ARRAY_SIZE(arr) sizeof(arr) / sizeof(arr[0])
#define IS_ALIGNED(n, m) (((n) % (m)) == 0)
#define PTR_IS_ALIGNED(p, n) IS_ALIGNED((uintptr_t)(p), (n))
#define MIN(a, b) (a) > (b) ? (b) : (a)

#define ALIGN_UP(a, b) (((a + b - 1) / b) * b)
#define wmb()                                                                  \
  ({                                                                           \
    asm volatile("" ::: "memory");                                             \
    (void)0;                                                                   \
  })
#define rmb wmb

struct propig_vfio;

struct propig_vfio *propig_vfio_init(const char *device);

void *propig_vfio_pci_map_bar(struct propig_vfio *s, int index, uint64_t offset,
                              uint64_t size, int prot);

int propig_vfio_dma_map(struct propig_vfio *s, const void *host, uint64_t size,
                        uint64_t iova);
int propig_vfio_pci_init_irq(struct propig_vfio *s, int efd, int irq_type);

int propig_vfio_pci_read_config(struct propig_vfio *s, void *buf, int size,
                                int offset);

int propig_vfio_pci_write_config(struct propig_vfio *s, void *buf, int size,
                                 int offset);
void propig_vfio_free(struct propig_vfio *s);

#endif