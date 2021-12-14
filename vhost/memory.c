
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>

#include "allocator/include/heap.h"
#include "heap.h"
#include "memory.h"
#include "utils.h"

struct hugepage_info {
  uint64_t addr;
  size_t size;
  char *path;
  int fd;
};

struct hp_mem {
  int nregions;
  struct hugepage_info hugepages[VHOST_MEMORY_MAX_NREGIONS];
  heap_t *heap;
};

static struct hp_mem g_mem;

void init_hp_mem(void) {
  memset(&g_mem, 0, sizeof(g_mem));
  hp_mem_add_one(HEAP_INIT_SIZE);
  g_mem.heap = create_heap(g_mem.hugepages[0].addr);
}

void free_hp_mem(void) {
  // printf("free_hp_mem regions: %d\n", g_mem.nregions);
  for (int i = 0; i < g_mem.nregions; i++) {
    if (g_mem.hugepages[i].fd >= 0) {
      close(g_mem.hugepages[i].fd);
    }
    if (g_mem.hugepages[i].path) {
      free(g_mem.hugepages[i].path);
      unlink(g_mem.hugepages[i].path);
    }
  }
  free_heap(g_mem.heap);
}

static int hugepage_info_alloc(struct hugepage_info *info) {
  asprintf(&info->path, "/dev/hugepages/libvhost.%d.XXXXXX", getpid());
  info->fd = mkstemp(info->path);
  if (info->fd < 0) {
    perror("mkstemp");
    free(info->path);
    return -1;
  }
  unlink(info->path);
  info->addr = (uint64_t)mmap(NULL, info->size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, info->fd, 0);
  if (info->addr == (uint64_t)MAP_FAILED) {
    perror("mmap");
    close(info->fd);
    free(info->path);
    exit(EXIT_FAILURE);
    return -1;
  }
  memset((void *)info->addr, 0, info->size);
  INFO("Add DIMM, addr: 0x%" PRIx64 "\n", info->addr);
  return 0;
}

void *hp_mem_add_one(size_t size) {
  struct hugepage_info *info;
  if (g_mem.nregions >= VHOST_MEMORY_MAX_NREGIONS) {
    fprintf(stderr, "Too many hugepages\n");
    return NULL;
  }
  info = &g_mem.hugepages[g_mem.nregions];
  g_mem.nregions++;
  info->size = size;
  if (hugepage_info_alloc(info) != 0) {
    return NULL;
  }
  return (void *)info->addr;
}

void get_memory_info(VhostUserMemory *memory) {
  memory->nregions = g_mem.nregions;

  for (int i = 0; i < g_mem.nregions; i++) {
    VhostUserMemoryRegion *reg = &memory->regions[i];
    /* UNUSED by backend, just fill the va */
    reg->guest_phys_addr = g_mem.hugepages[i].addr;
    reg->userspace_addr = g_mem.hugepages[i].addr;
    reg->memory_size = g_mem.hugepages[i].size;
    reg->mmap_offset = 0;
    INFO("memory region %d \n    size: %" PRIu64
         "\n    guest physical addr: 0x%" PRIx64 "\n    userspace "
         "addr: 0x%" PRIx64 "\n    mmap offset: 0x%" PRIx64 "\n",
         i, reg->memory_size, reg->guest_phys_addr, reg->userspace_addr,
         reg->mmap_offset);
  }
}

int get_memory_fds(int *fds, int *size) {
  unsigned int i;

  for (i = 0; i < g_mem.nregions; i++) {
    if (g_mem.hugepages[i].fd < 0) {
      fprintf(stderr, "Failed to open memory region\n");
      return -1;
    }
    fds[i] = g_mem.hugepages[i].fd;
  }
  *size = g_mem.nregions;
  return 0;
}

// TODO: only get memory from the first region
void *vhost_malloc(uint64_t size) { return heap_alloc(g_mem.heap, size); }

void vhost_free(void *ptr) {
  heap_free(g_mem.heap, ptr);
  // printf("DEBUG: free memory: %p\n", ptr);
}