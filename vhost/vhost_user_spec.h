#pragma once
#include <inttypes.h>
#include <linux/limits.h>

#define VHOST_USER_MAX_QUEUE_PAIRS 32
typedef enum VhostUserRequest {
  VHOST_USER_NONE = 0,
  VHOST_USER_GET_FEATURES = 1,
  VHOST_USER_SET_FEATURES = 2,
  VHOST_USER_SET_OWNER = 3,
  VHOST_USER_RESET_OWNER = 4,
  VHOST_USER_SET_MEM_TABLE = 5,
  VHOST_USER_SET_LOG_BASE = 6,
  VHOST_USER_SET_LOG_FD = 7,
  VHOST_USER_SET_VRING_NUM = 8,
  VHOST_USER_SET_VRING_ADDR = 9,
  VHOST_USER_SET_VRING_BASE = 10,
  VHOST_USER_GET_VRING_BASE = 11,
  VHOST_USER_SET_VRING_KICK = 12,
  VHOST_USER_SET_VRING_CALL = 13,
  VHOST_USER_SET_VRING_ERR = 14,
  VHOST_USER_GET_PROTOCOL_FEATURES = 15,
  VHOST_USER_SET_PROTOCOL_FEATURES = 16,
  VHOST_USER_GET_QUEUE_NUM = 17,
  VHOST_USER_SET_VRING_ENABLE = 18,
  VHOST_USER_SEND_RARP = 19,
  VHOST_USER_NET_SET_MTU = 20,
  VHOST_USER_SET_SLAVE_REQ_FD = 21,
  VHOST_USER_IOTLB_MSG = 22,
  VHOST_USER_GET_CONFIG = 24,
  VHOST_USER_SET_CONFIG = 25,
  VHOST_USER_CRYPTO_CREATE_SESS = 26,
  VHOST_USER_CRYPTO_CLOSE_SESS = 27,
  VHOST_USER_POSTCOPY_ADVISE = 28,
  VHOST_USER_POSTCOPY_LISTEN = 29,
  VHOST_USER_POSTCOPY_END = 30,
  VHOST_USER_MAX
} VhostUserRequest;
typedef struct VhostUserMemoryRegion {
  uint64_t guest_phys_addr;
  uint64_t memory_size;
  uint64_t userspace_addr;
  uint64_t mmap_offset;
} VhostUserMemoryRegion;

#define VHOST_MEMORY_MAX_NREGIONS 8
#define VHOST_USER_MAX_CONFIG_SIZE 256

typedef struct VhostUserMemory {
  uint32_t nregions;
  uint32_t padding;
  VhostUserMemoryRegion regions[VHOST_MEMORY_MAX_NREGIONS];
} VhostUserMemory;

typedef struct VhostUserConfig {
  uint32_t offset;
  uint32_t size;
  uint32_t flags;
  uint8_t region[VHOST_USER_MAX_CONFIG_SIZE];
} VhostUserConfig;

typedef struct VhostVringState {
  unsigned int index;
  unsigned int num;
} VhostVringState;
typedef struct VhostVringFile {
  unsigned int index;
  int fd;
} VhostVringFile;

typedef struct VhostVringAddr {
  unsigned int index;
  unsigned int flags;
  uint64_t desc_user_addr;
  uint64_t used_user_addr;
  uint64_t avail_user_addr;
  uint64_t log_guest_addr;
} VhostVringAddr;

typedef struct VhostUserMsg {
  VhostUserRequest request;

#define VHOST_USER_VERSION_MASK (0x3)
#define VHOST_USER_REPLY_MASK (0x1 << 2)
  uint32_t flags;
  uint32_t size; /* payload size */
  union {
#define VHOST_USER_VRING_IDX_MASK (0xff)
#define VHOST_USER_VRING_NOFD_MASK (0x1 << 8)
    uint64_t u64;
    VhostVringState state;
    VhostVringAddr addr;
    VhostUserMemory memory;
    VhostUserConfig cfg;
  } payload;
} __attribute__((packed)) VhostUserMsg;

#define VHOST_USER_VERSION (0x1)
#define MEMB_SIZE(t, m) (sizeof(((t *)0)->m))
#define VHOST_USER_HDR_SIZE offsetof(struct VhostUserMsg, payload.u64)
#define VHOST_USER_PAYLOAD_SIZE                                                \
  (sizeof(struct VhostUserMsg) - VHOST_USER_HDR_SIZE)

// virtio
#define VIRTIO_F_NOTIFY_ON_EMPTY 24
#define VHOST_F_LOG_ALL 26
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX 29
#define VHOST_USER_F_PROTOCOL_FEATURES 30
#define VIRTIO_F_VERSION_1 32
#define VIRTIO_F_RING_PACKED 34
