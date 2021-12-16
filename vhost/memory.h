#pragma once

#include "vhost_user_spec.h"
#include <stddef.h>

void init_hp_mem(void);

void free_hp_mem(void);

void *hp_mem_add_one(size_t size);

void get_memory_info(VhostUserMemory *memory);

int get_memory_fds(int *fds, int *size);

void *vhost_malloc(uint64_t size);

void vhost_free(void *ptr);