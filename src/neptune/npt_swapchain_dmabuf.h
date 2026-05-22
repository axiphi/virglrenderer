/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Dmabuf-WSI backend for the swapchain wrapper.  We fill a
 * DxvkDmabufConfig with three callbacks (onImagesChanged,
 * onAcquireImage, onPresentSubmitted), pass the config pointer in
 * the HWND slot of CreateSwapChainForHwnd, and the host library
 * drives presentation through those callbacks.  The published image
 * set is snapshotted into our own storage in onImagesChanged so the
 * wire reply can be served without re-entering the surface.
 */

#ifndef NPT_SWAPCHAIN_DMABUF_H
#define NPT_SWAPCHAIN_DMABUF_H

#include "npt_swapchain.h"

#include <dxvk_dmabuf.h>

/* The dmabuf contract's per-surface image cap must fit the base
 * wrapper's slot table.  If the contract grows, bump
 * NPT_SWAPCHAIN_MAX_BUFFERS. */
_Static_assert(DXVK_DMABUF_MAX_IMAGES <= NPT_SWAPCHAIN_MAX_BUFFERS,
               "dmabuf max images exceeds base slot table");

struct npt_swapchain_dmabuf {
   struct npt_swapchain base;

   /* Handed to the host library as (HWND)&dmabuf_config. */
   struct DxvkDmabufConfig dmabuf_config;

   /* Snapshot of the latest image set from onImagesChanged.  We
    * don't dup() the fds — they stay owned by the host library
    * until the next onImagesChanged or destroy.  The struct copy
    * keeps width/height/numImages/strides alive for snapshot_info
    * readers after the callback returns. */
   struct DxvkDmabufImageSet dmabuf_current;

   /* Blob ids registered for each image fd in dmabuf_current,
    * indexed by image position.  0 = unused slot. */
   uint64_t image_blob_ids[NPT_SWAPCHAIN_MAX_BUFFERS];
};

/* Called by npt_swapchain_create when dmabuf is the selected backend. */
struct npt_swapchain *
npt_swapchain_dmabuf_create(struct npt_context *context,
                            void *device, void *factory,
                            uint32_t width, uint32_t height,
                            uint32_t format, uint32_t buffer_count);

#endif /* NPT_SWAPCHAIN_DMABUF_H */
