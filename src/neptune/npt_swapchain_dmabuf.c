/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Dmabuf-WSI backend for the swapchain wrapper.  Implements the
 * DxvkDmabufConfig contract (callbacks, format mapping, image-fd
 * blob registration) and provides the npt_swapchain_ops vtable.
 */

#include "npt_swapchain_dmabuf.h"

#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "npt_com.h"
#include "npt_context.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_directx_types.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "npt_transport_defs.h"
#include "virgl_hw.h"

/* DRM fourccs for our presentable subset.  Defined locally to avoid
 * a <drm_fourcc.h> dependency. */
#define NPT_DRM_FORMAT_XRGB8888  0x34325258u  /* B,G,R,X memory order */
#define NPT_DRM_FORMAT_XBGR8888  0x34324258u  /* R,G,B,X memory order */
#define NPT_DRM_FORMAT_ARGB8888  0x34325241u  /* B,G,R,A memory order */
#define NPT_DRM_FORMAT_ABGR8888  0x34324241u  /* R,G,B,A memory order */

/* Translate a host-published DRM fourcc to virgl_format for the
 * wire reply.  Describes the dmabuf's physical layout, which can
 * differ from the app's DXGI_FORMAT request when the library was
 * free to honour preferredDrmFormat with a swizzle. */
static uint32_t
npt_drm_fourcc_to_virgl_format(uint32_t fourcc)
{
   switch (fourcc) {
   case NPT_DRM_FORMAT_ARGB8888:   return VIRGL_FORMAT_B8G8R8A8_UNORM;
   case NPT_DRM_FORMAT_XRGB8888:   return VIRGL_FORMAT_B8G8R8X8_UNORM;
   case NPT_DRM_FORMAT_ABGR8888:   return VIRGL_FORMAT_R8G8B8A8_UNORM;
   case NPT_DRM_FORMAT_XBGR8888:   return VIRGL_FORMAT_R8G8B8X8_UNORM;
   default:                        return VIRGL_FORMAT_NONE;
   }
}

/* Inverse: map a guest virgl_format hint to a DRM fourcc for the
 * library's preferredDrmFormat.  Limited to formats the library can
 * accept; everything else returns 0 (keeps the library on its
 * default). */
static uint32_t
npt_virgl_format_to_drm_fourcc(uint32_t virgl_format)
{
   switch (virgl_format) {
   case VIRGL_FORMAT_B8G8R8A8_UNORM:   return NPT_DRM_FORMAT_ARGB8888;
   case VIRGL_FORMAT_B8G8R8X8_UNORM:   return NPT_DRM_FORMAT_XRGB8888;
   case VIRGL_FORMAT_R8G8B8A8_UNORM:   return NPT_DRM_FORMAT_ABGR8888;
   case VIRGL_FORMAT_R8G8B8X8_UNORM:   return NPT_DRM_FORMAT_XBGR8888;
   default:                            return 0;
   }
}

/* DXGI_USAGE_RENDER_TARGET_OUTPUT is an SDK macro, not a registry enum. */
#ifndef DXGI_USAGE_RENDER_TARGET_OUTPUT
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x20u
#endif

static struct npt_swapchain_dmabuf *
sc_dmabuf(struct npt_swapchain *base)
{
   return (struct npt_swapchain_dmabuf *)base;
}

/* === Callbacks installed on DxvkDmabufConfig === */

/* Fires synchronously inside CreateSwapChainForHwnd and
 * ResizeBuffers.  Registers each dmabuf fd as a pending blob so the
 * guest's GET_SWAPCHAIN_IMAGES can hand back a blob_id without an
 * extra round-trip.  The host library retains ownership of the
 * original fd; we dup() before registering.
 *
 * Threading: DXGI dispatch thread (also the Neptune dispatcher).
 * Holds no Neptune locks the dispatcher needs. */
static void
npt_sc_on_images_changed(void *user, const struct DxvkDmabufImageSet *set)
{
   struct npt_swapchain_dmabuf *sc = user;
   if (!sc || !set)
      return;

   /* Snapshot the published set so width/height/numImages/strides
    * outlive the callback for snapshot_info readers.  Fds inside
    * images[] stay owned by the host library (valid until the next
    * onImagesChanged). */
   sc->dmabuf_current = *set;

   /* Mirror dimensions onto the base for the resize fallback. */
   sc->base.current_width  = set->width;
   sc->base.current_height = set->height;

   /* Drop blob ids from the previous generation.  An unclaimed
    * pending_blob entry leaks an fd-counted resource for the context
    * lifetime; in practice the guest claims them immediately on
    * CreateSwapChain reply. */
   for (uint32_t i = 0; i < NPT_SWAPCHAIN_MAX_BUFFERS; i++)
      sc->image_blob_ids[i] = 0;
   sc->base.images_generation = set->generation;

   /* Process-global counter so two contexts creating swapchains in
    * parallel can't collide. */
   static atomic_uint_least64_t next_dmabuf_blob_id = 0x80000000u;
   for (uint32_t i = 0; i < set->numImages && i < NPT_SWAPCHAIN_MAX_BUFFERS; i++) {
      const struct DxvkDmabufImage *img = &set->images[i];
      if (!img->numPlanes || img->planes[0].fd < 0)
         continue;

      uint64_t bid = atomic_fetch_add_explicit(&next_dmabuf_blob_id, 1,
                                               memory_order_relaxed);
      int fd = dup(img->planes[0].fd);
      if (fd < 0)
         continue;

      uint64_t size = (uint64_t)img->planes[0].stride * set->height;
      if (npt_context_register_pending_blob(sc->base.context, bid,
                                            VIRGL_RESOURCE_FD_DMABUF,
                                            fd, size)) {
         sc->image_blob_ids[i] = bid;
      } else {
         close(fd);
      }
   }

   npt_log("swapchain: onImagesChanged gen=%" PRIu64 " %ux%u drm=0x%x images=%u",
           set->generation, set->width, set->height,
           set->drmFormat, set->numImages);
}

/* Host library's frame worker asks whether image_index is free.
 * Returns the consumer's release sync_fd (library takes ownership)
 * or -1.  Called only on the frame worker. */
static int
npt_sc_on_acquire(void *user, uint32_t image_index)
{
   struct npt_swapchain_dmabuf *sc = user;
   if (!sc || image_index >= NPT_SWAPCHAIN_MAX_BUFFERS)
      return -1;

   mtx_lock(&sc->base.slots_mutex);
   struct npt_swapchain_slot *slot = &sc->base.slots[image_index];

   /* Wait while net pending > 0.  Gotchas:
    *  - Compute the deadline ONCE so spurious wakeups (slots_cond
    *    is broadcast from on_present and image_released across all
    *    slots) can't extend the 1 s safety bound by re-arming it.
    *  - Match thrd_busy rather than thrd_timeout: mesa's c11
    *    cnd_timedwait wrapper maps ETIMEDOUT to thrd_busy (deviates
    *    from C11), so a `rc == thrd_timeout` check is dead code. */
   struct timespec deadline;
   timespec_get(&deadline, TIME_UTC);
   deadline.tv_sec += 1;
   while (sc->base.alive && slot->pending_presents > 0) {
      int rc = cnd_timedwait(&sc->base.slots_cond, &sc->base.slots_mutex,
                             &deadline);
      if (rc != thrd_success)
         break;
   }

   /* Hand the latest release_fd to the library (ownership transfer). */
   int fd = slot->release_fd;
   slot->release_fd = -1;
   mtx_unlock(&sc->base.slots_mutex);
   return fd;
}

/* Host library has queued GPU work for a present.  gpu_done_fd
 * signals when the GPU has finished; pushed onto the present-done
 * FIFO so the next submit_fence can pair it with the guest fence.
 *
 * Threading: submit worker.  Not synchronous with the D3D11 Present
 * call — the host async chain can fire this after the guest has
 * already done Present-reply → consumer → IMAGE_RELEASE for the
 * same frame.  The signed pending counter handles that race. */
static void
npt_sc_on_present(void *user, uint32_t image_index,
                  uint64_t frame_id, int gpu_done_fd)
{
   struct npt_swapchain_dmabuf *sc = user;
   if (!sc) {
      if (gpu_done_fd >= 0)
         close(gpu_done_fd);
      return;
   }

   if (image_index < NPT_SWAPCHAIN_MAX_BUFFERS) {
      mtx_lock(&sc->base.slots_mutex);
      /* Bump pending and broadcast so an acquire waiter on this slot
       * can re-evaluate (e.g. an early IMAGE_RELEASE dropped pending
       * to -1 and this present brings it back to 0). */
      sc->base.slots[image_index].pending_presents++;
      cnd_broadcast(&sc->base.slots_cond);
      mtx_unlock(&sc->base.slots_mutex);
   }

   /* present-done FIFO: next submit_fence pops the fd as its wait
    * target.  retire_fence propagates through virgl core to the
    * guest's virtio-gpu fence machinery — no fd crosses virtio. */
   npt_context_push_present_done(sc->base.context, gpu_done_fd,
                                 image_index, frame_id);
}

/* === Ops vtable === */

static void
npt_sc_dmabuf_destroy(struct npt_swapchain *base)
{
   /* The host library closes the surface's dmabuf fds when its
    * presenter is torn down via the COM release cascade in
    * npt_swapchain_base_fini. */
   (void)base;
}

static void
npt_sc_dmabuf_prepare_resize(struct npt_swapchain *base,
                             uint32_t width, uint32_t height,
                             uint32_t format)
{
   struct npt_swapchain_dmabuf *sc = sc_dmabuf(base);

   /* The library rereads DxvkDmabufConfig on each (re)create.
    * Refresh defaultWidth/Height and preferredDrmFormat before
    * ResizeBuffers triggers the next onImagesChanged. */
   sc->dmabuf_config.defaultWidth  = width  ? width  : sc->dmabuf_current.width;
   sc->dmabuf_config.defaultHeight = height ? height : sc->dmabuf_current.height;
   sc->dmabuf_config.preferredDrmFormat = npt_virgl_format_to_drm_fourcc(
      atomic_load_explicit(&base->context->preferred_virgl_format,
                           memory_order_relaxed));
   (void)format;
}

static bool
npt_sc_dmabuf_snapshot_info(struct npt_swapchain *base,
                            struct npt_swapchain_images_info *out)
{
   struct npt_swapchain_dmabuf *sc = sc_dmabuf(base);
   const struct DxvkDmabufImageSet *cur = &sc->dmabuf_current;

   memset(out, 0, sizeof(*out));
   if (cur->numImages == 0)
      return false;

   out->num_images   = cur->numImages > NPT_SWAPCHAIN_MAX_IMAGES
                       ? NPT_SWAPCHAIN_MAX_IMAGES : cur->numImages;
   /* Derived from the host library's actual produced fourcc — the
    * dmabuf's physical layout, which is what a downstream WSI
    * consumer needs to bind the image correctly. */
   out->virgl_format = npt_drm_fourcc_to_virgl_format(cur->drmFormat);
   out->width        = cur->width;
   out->height       = cur->height;

   for (uint32_t i = 0; i < out->num_images; i++) {
      out->images[i].blob_id = sc->image_blob_ids[i];
      out->images[i].stride  = cur->images[i].numPlanes
                                ? cur->images[i].planes[0].stride : 0;
   }
   return true;
}

static const struct npt_swapchain_ops npt_swapchain_dmabuf_ops = {
   .destroy        = npt_sc_dmabuf_destroy,
   .prepare_resize = npt_sc_dmabuf_prepare_resize,
   .snapshot_info  = npt_sc_dmabuf_snapshot_info,
};

/* === Lifecycle === */

struct npt_swapchain *
npt_swapchain_dmabuf_create(struct npt_context *context,
                            void *device, void *factory,
                            uint32_t width, uint32_t height,
                            uint32_t format, uint32_t buffer_count)
{
   npt_log("swapchain_create: %ux%u fmt=%u buffers=%u device=%p factory=%p",
           width, height, format, buffer_count, device, factory);
   if (!context || !device || !width || !height || !buffer_count)
      return NULL;

   void *derived_factory = NULL;
   if (!factory)
      factory = npt_swapchain_derive_factory(device, &derived_factory);
   if (!factory)
      return NULL;

   /* Pass buffer_count through as DXGI's app-visible value.  Slot-
    * table safety is enforced separately by the image_index <
    * NPT_SWAPCHAIN_MAX_BUFFERS guards in the callbacks. */

   struct npt_swapchain_dmabuf *sc = calloc(1, sizeof(*sc));
   if (!sc) {
      if (derived_factory)
         npt_com_release(derived_factory);
      return NULL;
   }

   if (!npt_swapchain_base_init(&sc->base, &npt_swapchain_dmabuf_ops,
                                context, device, buffer_count, format)) {
      free(sc);
      if (derived_factory)
         npt_com_release(derived_factory);
      return NULL;
   }

   /* Fill the dmabuf config.  defaultWidth/Height are consulted by
    * the library only when DXGI desc Width/Height is 0; we always
    * pass non-zero values in desc1, but set them for contract clarity. */
   sc->dmabuf_config.structSize          = sizeof(struct DxvkDmabufConfig);
   sc->dmabuf_config.defaultWidth        = width;
   sc->dmabuf_config.defaultHeight       = height;
   /* preferredImageCount = 0: use the library's default.  DXGI's
    * BufferCount and the dmabuf swap-image count are independent
    * concepts; the dmabuf set needs enough images to keep the
    * consumer's frame queue from stalling the d3d11 thread, so we
    * don't propagate the DXGI count here. */
   sc->dmabuf_config.preferredImageCount = 0;
   /* Guest's visual hint translated to a DRM fourcc; 0 leaves the
    * library on its default. */
   sc->dmabuf_config.preferredDrmFormat  = npt_virgl_format_to_drm_fourcc(
      atomic_load_explicit(&context->preferred_virgl_format,
                           memory_order_relaxed));
   /* Refresh rate from the same hint.  Used by the library's WSI
    * display-mode query so per-Present refresh logic lands at the
    * actual host rate.  0/0 leaves the library on its fallback. */
   sc->dmabuf_config.refreshRateNumerator   = atomic_load_explicit(
      &context->preferred_refresh_num, memory_order_relaxed);
   sc->dmabuf_config.refreshRateDenominator = atomic_load_explicit(
      &context->preferred_refresh_den, memory_order_relaxed);
   sc->dmabuf_config.callbackUser        = sc;
   sc->dmabuf_config.onImagesChanged     = npt_sc_on_images_changed;
   sc->dmabuf_config.onAcquireImage      = npt_sc_on_acquire;
   sc->dmabuf_config.onPresentSubmitted  = npt_sc_on_present;

   void **vtable = npt_com_vtable(factory);

   DXGI_SWAP_CHAIN_DESC1 desc1;
   memset(&desc1, 0, sizeof(desc1));
   desc1.Width = width;
   desc1.Height = height;
   desc1.Format = (DXGI_FORMAT)format;
   desc1.SampleDesc.Count = 1;
   desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   desc1.BufferCount = buffer_count;
   desc1.Scaling = DXGI_SCALING_STRETCH;
   desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
   desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

   /* HWND = &dmabuf_config per the contract.  The host library
    * detects dmabuf mode (DXVK_WSI_DRIVER=Dmabuf, set at library
    * load), reinterprets the HWND, allocates the dmabuf-exported
    * images, and fires onImagesChanged synchronously before this
    * call returns. */
   PFN_IDXGIFactory2_CreateSwapChainForHwnd create_sc_hwnd =
      NPT_COM_VTBL_FUNC(PFN_IDXGIFactory2_CreateSwapChainForHwnd,
                         vtable,
                         NPT_VTBL_IDXGIFactory2_CreateSwapChainForHwnd);
   HRESULT hr = create_sc_hwnd(factory, device,
                               (HWND)(uintptr_t)&sc->dmabuf_config,
                               &desc1, NULL, NULL,
                               (IDXGISwapChain1 **)&sc->base.dxgi_swapchain);

   if (NPT_FAILED(hr)) {
      npt_log("swapchain: CreateSwapChainForHwnd failed (hr=0x%x)", hr);
      goto fail;
   }

   npt_swapchain_map_register(&sc->base);

   if (derived_factory)
      npt_com_release(derived_factory);
   return &sc->base;

fail:
   if (derived_factory)
      npt_com_release(derived_factory);
   npt_swapchain_base_fini(&sc->base);
   return NULL;
}
