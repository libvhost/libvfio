
#include "conn.h"
#include "memory.h"
#include "utils.h"
#include "vhost_user.h"
#include "virtqueue.h"

#include <linux/virtio_blk.h>
#include <linux/virtio_ring.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/unistd.h>

static int vhost_conn_get_config(VhostConn *conn);

static const char *const vhost_msg_strings[VHOST_USER_MAX] = {
    [VHOST_USER_SET_OWNER] = "VHOST_SET_OWNER",
    [VHOST_USER_RESET_OWNER] = "VHOST_RESET_OWNER",
    [VHOST_USER_SET_FEATURES] = "VHOST_SET_FEATURES",
    [VHOST_USER_GET_FEATURES] = "VHOST_GET_FEATURES",
    [VHOST_USER_SET_VRING_CALL] = "VHOST_SET_VRING_CALL",
    [VHOST_USER_GET_PROTOCOL_FEATURES] = "VHOST_USER_GET_PROTOCOL_FEATURES",
    [VHOST_USER_SET_PROTOCOL_FEATURES] = "VHOST_USER_SET_PROTOCOL_FEATURES",
    [VHOST_USER_SET_VRING_NUM] = "VHOST_SET_VRING_NUM",
    [VHOST_USER_SET_VRING_BASE] = "VHOST_SET_VRING_BASE",
    [VHOST_USER_GET_VRING_BASE] = "VHOST_GET_VRING_BASE",
    [VHOST_USER_SET_VRING_ADDR] = "VHOST_SET_VRING_ADDR",
    [VHOST_USER_SET_VRING_KICK] = "VHOST_SET_VRING_KICK",
    [VHOST_USER_SET_MEM_TABLE] = "VHOST_SET_MEM_TABLE",
    [VHOST_USER_SET_VRING_ENABLE] = "VHOST_SET_VRING_ENABLE",
    [VHOST_USER_GET_QUEUE_NUM] = "VHOST_USER_GET_QUEUE_NUM",
    [VHOST_USER_GET_CONFIG] = "VHOST_USER_GET_CONFIG",
    [VHOST_USER_SET_CONFIG] = "VHOST_USER_SET_CONFIG",
};

/*
If VHOST_USER_F_PROTOCOL_FEATURES has not been negotiated, the ring is
initialized in an enabled state.

If VHOST_USER_F_PROTOCOL_FEATURES has been negotiated, the ring is initialized
in a disabled state. Client must not pass data to/from the backend until ring is
enabled by VHOST_USER_SET_VRING_ENABLE with parameter 1, or after it has been
disabled by VHOST_USER_SET_VRING_ENABLE with parameter 0.

Each ring is initialized in a stopped state, client must not process it until
ring is started, or after it has been stopped.

Client must start ring upon receiving a kick (that is, detecting that file
descriptor is readable) on the descriptor specified by VHOST_USER_SET_VRING_KICK
or receiving the in-band message VHOST_USER_VRING_KICK if negotiated, and stop
ring upon receiving VHOST_USER_GET_VRING_BASE.

*/
#define DEFUALT_VHOST_FEATURES                                                \
    ((1ULL << VHOST_F_LOG_ALL) | (1ULL << VIRTIO_F_VERSION_1) |               \
     (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) | (1ULL << VIRTIO_RING_F_EVENT_IDX) | \
     (1ULL << VIRTIO_RING_F_INDIRECT_DESC))
//  (1ULL << VIRTIO_F_RING_PACKED) | (1ULL << VHOST_USER_F_PROTOCOL_FEATURES)

VhostConn *create_vhost_conn(const char *path) {
    VhostConn *conn = calloc(1, sizeof(VhostConn));
    conn->sock_path = strdup(path);
    // Set default features.
    conn->features = DEFUALT_VHOST_FEATURES;
    return conn;
}

void destroy_vhost_conn(VhostConn *conn) {
    free(conn->sock_path);
    free(conn);
}

int vhost_connect(VhostConn *conn) {
    struct sockaddr_un un;
    size_t len;
    VhostUserMemory memory;

    conn->sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (conn->sock == -1) {
        perror("socket");
        return -1;
    }

    un.sun_family = AF_UNIX;
    strcpy(un.sun_path, conn->sock_path);
    len = sizeof(un.sun_family) + strlen(conn->sock_path);

    if (connect(conn->sock, (struct sockaddr *) &un, len) == -1) {
        close(conn->sock);
        perror("connect");
        return -1;
    }
    if (vhost_conn_get_config(conn) != 0) {
        close(conn->sock);
        return -1;
    }
    return 0;
}

static char *get_feature_name(int bit) {
    switch (bit) {
        case VIRTIO_F_NOTIFY_ON_EMPTY:
            return "VIRTIO_F_NOTIFY_ON_EMPTY";
        case VHOST_F_LOG_ALL:
            return "VHOST_F_LOG_ALL";
        case VIRTIO_RING_F_INDIRECT_DESC:
            return "VIRTIO_RING_F_INDIRECT_DESC";
        case VIRTIO_RING_F_EVENT_IDX:
            return "VIRTIO_RING_F_EVENT_IDX";
        case VHOST_USER_F_PROTOCOL_FEATURES:
            return "VHOST_USER_F_PROTOCOL_FEATURES";
        case VIRTIO_F_VERSION_1:
            return "VIRTIO_F_VERSION_1";
        case VIRTIO_F_RING_PACKED:
            return "VIRTIO_F_RING_PACKED";
        default:
            return "UNKNOWN";
    }
}

static int negotiate_features(VhostConn *conn) {
    uint64_t features;
    int i = 0;
    if (vhost_ioctl(conn->sock, VHOST_USER_GET_FEATURES, &features) != 0) {
        ERROR("Unable to get vhost features\n");
        return -1;
    }
    conn->features &= features;
    if (vhost_ioctl(conn->sock, VHOST_USER_SET_FEATURES, &conn->features) != 0) {
        ERROR("Unable to set vhost features\n");
        return -1;
    }
    INFO("Features:\n");
    for (; i < 64; ++i) {
        if (conn->features & (1ULL << i)) {
            INFO("    bit: %2d %s\n", i, get_feature_name(i));
        }
    }
    return 0;
}

#define TO_GB(x) ((x) / (1024 * 1024 * 1024))

static int vhost_conn_get_config(VhostConn *conn) {
    VhostUserConfig config = {.size = sizeof(config.region)};
    struct virtio_blk_config *cfg = &conn->cfg;

    if (vhost_ioctl(conn->sock, VHOST_USER_GET_CONFIG, &config) != 0) {
        ERROR("Unable to get vhost config\n");
        return -1;
    }
    memcpy(cfg, &config.region, sizeof(config.region));

    /* Capacity unit is sector, not block.*/
    DEBUG("[DEVICE INFO] capacity: %.3f GiB (%" PRIu64 ")\n",
          TO_GB(1.0 * cfg->capacity * 512), cfg->capacity * 512);
    DEBUG("[DEVICE INFO] size_max: %" PRIu32 "\n", cfg->size_max);
    DEBUG("[DEVICE INFO] seg_max: %" PRIu32 "\n", cfg->seg_max);
    // DEBUG("[DEVICE INFO] cylinders: %" PRIu16 "\n", cfg->cylinders);
    // DEBUG("[DEVICE INFO] heads: %" PRIu8 "\n", cfg->heads);
    // DEBUG("[DEVICE INFO] sectors: %" PRIu8 "\n", cfg->sectors);
    DEBUG("[DEVICE INFO] blk_size: %" PRIu32 "\n", cfg->blk_size);
    DEBUG("[DEVICE INFO] physical_block_exp: %" PRIu8 "\n",
          cfg->physical_block_exp);
    DEBUG("[DEVICE INFO] alignment_offset: %" PRIu8 "\n", cfg->alignment_offset);
    DEBUG("[DEVICE INFO] min_io_size: %" PRIu16 "\n", cfg->min_io_size);
    DEBUG("[DEVICE INFO] opt_io_size: %" PRIu32 "\n", cfg->opt_io_size);
    DEBUG("[DEVICE INFO] wce: %" PRIu8 "\n", cfg->wce);
    DEBUG("[DEVICE INFO] num_queues: %" PRIu16 "\n", cfg->num_queues);
    DEBUG("[DEVICE INFO] max_discard_sectors: %" PRIu32 "\n",
          cfg->max_discard_sectors);
    DEBUG("[DEVICE INFO] max_discard_seg: %" PRIu32 "\n", cfg->max_discard_seg);
    DEBUG("[DEVICE INFO] discard_sector_alignment: %" PRIu32 "\n",
          cfg->discard_sector_alignment);
    DEBUG("[DEVICE INFO] max_write_zeroes_sectors: %" PRIu32 "\n",
          cfg->max_write_zeroes_sectors);
    DEBUG("[DEVICE INFO] max_write_zeroes_seg: %" PRIu32 "\n",
          cfg->max_write_zeroes_seg);
    DEBUG("[DEVICE INFO] write_zeroes_may_unmap: %" PRIu8 "\n",
          cfg->write_zeroes_may_unmap);

    return 0;
}

uint64_t vhost_conn_get_blocksize(VhostConn *conn) { return conn->cfg.blk_size; }

int vhost_conn_get_numblocks(VhostConn *conn) {
    CHECK(conn->cfg.blk_size != 0);
    return conn->cfg.capacity * 512 / conn->cfg.blk_size;
}

int vhost_setup(VhostConn *conn) {
    VhostUserMemory memory;
    int ret;
    if (vhost_ioctl(conn->sock, VHOST_USER_SET_OWNER, 0) != 0) {
        goto fail;
    }
    if (negotiate_features(conn) != 0) {
        goto fail;
    }

    /* get mem regions info for passing it to the server */
    get_memory_info(&memory);
    if (vhost_ioctl(conn->sock, VHOST_USER_SET_MEM_TABLE, &memory) != 0) {
        goto fail;
    }
    return 0;
fail:
    close(conn->sock);
    return -1;
}

static int vhost_enable_vq(VhostConn *conn, VirtQueue *vq) {
    VhostVringState state;
    state.index = vq->idx;
    state.num = vq->size;
    INFO("Setup virtqueue \n");
    // Tell the backend that the virtqueue size.
    if (vhost_ioctl(conn->sock, VHOST_USER_SET_VRING_NUM, &state) != 0) {
        ERROR("Unable to set vring num\n");
        return -1;
    }
    INFO("  VHOST_USER_SET_VRING_NUM idx: %d num: %d\n", state.index, state.num);
    state.index = vq->idx;
    state.num = vq->last_used_idx;
    // Tell the backend that the available ring last used index.
    if (vhost_ioctl(conn->sock, VHOST_USER_SET_VRING_BASE, &state) != 0) {
        ERROR("Unable to set vring base\n");
        return -1;
    }
    INFO("  VHOST_USER_SET_VRING_BASE idx: %d num: %d\n", state.index, state.num);

    VhostVringAddr addr;
    addr.index = vq->idx;
    addr.desc_user_addr = (uint64_t) vq->vring.desc;
    addr.avail_user_addr = (uint64_t) vq->vring.avail;
    addr.used_user_addr = (uint64_t) vq->vring.used;

    addr.flags = 0;
    if (vhost_ioctl(conn->sock, VHOST_USER_SET_VRING_ADDR, &addr) != 0) {
        ERROR("Unable to set vring addr\n");
        return -1;
    }
    INFO("  VHOST_USER_SET_VRING_ADDR idx: %d desc_user_addr: %lx "
         "used_user_addr: %lx avail_user_addr: %lx\n",
         addr.index, addr.desc_user_addr, addr.used_user_addr,
         addr.avail_user_addr);

    VhostVringFile file;
    file.index = vq->idx;
    file.fd = vq->callfd;

    if (vhost_ioctl(conn->sock, VHOST_USER_SET_VRING_CALL, &file) != 0) {
        ERROR("Unable to set vring call\n");
        return -1;
    }
    INFO("  VHOST_USER_SET_VRING_CALL idx: %d fd: %d\n", file.index, file.fd);

    file.index = vq->idx;
    file.fd = vq->kickfd;

    if (vhost_ioctl(conn->sock, VHOST_USER_SET_VRING_KICK, &file) != 0) {
        ERROR("Unable to set vring kick\n");
        return -1;
    }
    INFO("  VHOST_USER_SET_VRING_KICK idx: %d fd: %d\n", file.index, file.fd);

    return 0;
}

int vhost_conn_add_vq(VhostConn *conn, int size) {
    VirtQueue *vq = &conn->vqs[conn->nr_vqs++];
    vq->idx = conn->nr_vqs - 1;
    vq->size = size;
    vhost_vq_init(vq);
    return vhost_enable_vq(conn, vq);
}
