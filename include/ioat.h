#ifndef PROPIG_IOAT_H
#define PROPIG_IOAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define PAGE_SIZE (1 << 12)

struct propig_ioat_chan;
struct propig_vfio;

typedef void (*propig_ioat_req_cb)(void *arg, int status);

struct propig_ioat_chan **ioat_channel_load(int *cnt);

int ioat_channel_unload(struct propig_ioat_chan **ioats, int cnt);

struct propig_ioat_chan *ioat_channel_init();

struct propig_vfio *ioat_get_vfio(struct propig_ioat_chan *chan);

uint32_t propig_ioat_get_max_descriptors(struct propig_ioat_chan *chan);

int propig_ioat_build_copy(struct propig_ioat_chan *chan, void *cb_arg,
                           propig_ioat_req_cb cb_fn, void *dst, const void *src,
                           uint64_t nbytes);

int propig_ioat_submit_copy(struct propig_ioat_chan *chan, void *cb_arg,
                            propig_ioat_req_cb cb_fn, void *dst,
                            const void *src, uint64_t nbytes);

int propig_ioat_build_fill(struct propig_ioat_chan *chan, void *cb_arg,
                           propig_ioat_req_cb cb_fn, void *dst,
                           uint64_t fill_pattern, uint64_t nbytes);

int propig_ioat_submit_fill(struct propig_ioat_chan *chan, void *cb_arg,
                            propig_ioat_req_cb cb_fn, void *dst,
                            uint64_t fill_pattern, uint64_t nbytes);

void propig_ioat_flush(struct propig_ioat_chan *chan);

int propig_ioat_process_events(struct propig_ioat_chan *chan);

enum propig_ioat_dma_capability_flags {
  PROPIG_IOAT_ENGINE_COPY_SUPPORTED = 0x1, /**< The memory copy is supported */
  PROPIG_IOAT_ENGINE_FILL_SUPPORTED = 0x2, /**< The memory fill is supported */
};

uint32_t propig_ioat_get_dma_capabilities(struct propig_ioat_chan *chan);

#ifdef __cplusplus
}
#endif

#endif
