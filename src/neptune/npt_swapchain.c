/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Backend-neutral swapchain wrapper: slot tracking, wrapper-by-host-
 * pointer map, and the COM-level get/present/resize entry points.
 * Backend-specific code lives in npt_swapchain_<backend>.c.
 */

#include "npt_swapchain.h"
#include "npt_swapchain_dmabuf.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "npt_com.h"
#include "npt_context.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_directx_types.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "util/hash_table.h"

/* === Wrapper-by-host-pointer lookup map === */

/* The guest sees the real IDXGISwapChain pointer as the object_id;
 * overrides recover the wrapper through this map. */
static struct hash_table *npt_swapchain_map;
static mtx_t              npt_swapchain_map_mutex;
static pthread_once_t     npt_swapchain_map_once = PTHREAD_ONCE_INIT;

static void
npt_swapchain_map_init(void)
{
   mtx_init(&npt_swapchain_map_mutex, mtx_plain);
   npt_swapchain_map = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                               _mesa_key_pointer_equal);
}

bool
npt_swapchain_map_register(struct npt_swapchain *sc)
{
   pthread_once(&npt_swapchain_map_once, npt_swapchain_map_init);
   mtx_lock(&npt_swapchain_map_mutex);
   const bool ok = npt_swapchain_map &&
                   _mesa_hash_table_insert(npt_swapchain_map,
                                           sc->dxgi_swapchain, sc) != NULL;
   mtx_unlock(&npt_swapchain_map_mutex);
   if (!ok)
      npt_log("swapchain map insert failed; wrapper_for lookups will miss");
   return ok;
}

static void
npt_swapchain_map_unregister(struct npt_swapchain *sc)
{
   pthread_once(&npt_swapchain_map_once, npt_swapchain_map_init);
   mtx_lock(&npt_swapchain_map_mutex);
   if (npt_swapchain_map && sc->dxgi_swapchain)
      _mesa_hash_table_remove_key(npt_swapchain_map, sc->dxgi_swapchain);
   mtx_unlock(&npt_swapchain_map_mutex);
}

struct npt_swapchain *
npt_swapchain_wrapper_for(const void *dxgi_swapchain)
{
   if (!dxgi_swapchain)
      return NULL;
   pthread_once(&npt_swapchain_map_once, npt_swapchain_map_init);
   mtx_lock(&npt_swapchain_map_mutex);
   struct npt_swapchain *sc = NULL;
   if (npt_swapchain_map) {
      struct hash_entry *e =
         _mesa_hash_table_search(npt_swapchain_map, dxgi_swapchain);
      if (e)
         sc = e->data;
   }
   mtx_unlock(&npt_swapchain_map_mutex);
   return sc;
}

/* === Base init / fini === */

bool
npt_swapchain_base_init(struct npt_swapchain *sc,
                        const struct npt_swapchain_ops *ops,
                        struct npt_context *context,
                        void *device,
                        uint32_t buffer_count, uint32_t format)
{
   sc->ops = ops;
   sc->context = context;
   sc->device = device;
   sc->buffer_count = buffer_count;
   sc->format = format;
   sc->alive = true;

   if (mtx_init(&sc->slots_mutex, mtx_plain) != thrd_success)
      return false;
   if (cnd_init(&sc->slots_cond) != thrd_success) {
      mtx_destroy(&sc->slots_mutex);
      return false;
   }

   for (uint32_t i = 0; i < NPT_SWAPCHAIN_MAX_BUFFERS; i++)
      sc->slots[i].release_fd = -1;

   /* Strong device ref for the swapchain's lifetime. */
   npt_com_add_ref(device);
   return true;
}

void
npt_swapchain_base_fini(struct npt_swapchain *sc)
{
   if (sc->dxgi_swapchain)
      npt_com_release(sc->dxgi_swapchain);

   for (uint32_t i = 0; i < NPT_SWAPCHAIN_MAX_BUFFERS; i++) {
      if (sc->slots[i].release_fd >= 0)
         close(sc->slots[i].release_fd);
   }

   cnd_destroy(&sc->slots_cond);
   mtx_destroy(&sc->slots_mutex);

   if (sc->device)
      npt_com_release(sc->device);
   free(sc);
}

void *
npt_swapchain_derive_factory(void *device, void **out_owned_factory)
{
   *out_owned_factory = NULL;
   if (!device)
      return NULL;

   void *dxgi_device = NULL;
   HRESULT hr = npt_com_query_interface(device, &NPT_IID_IDXGIDevice, &dxgi_device);
   if (NPT_FAILED(hr) || !dxgi_device) {
      npt_log("swapchain: device->QI(IDXGIDevice) failed: 0x%x", hr);
      return NULL;
   }

   void *adapter = NULL;
   PFN_IDXGIDevice_GetAdapter get_adapter =
      NPT_COM_VTBL_FUNC(PFN_IDXGIDevice_GetAdapter,
                         npt_com_vtable(dxgi_device),
                         NPT_VTBL_IDXGIDevice_GetAdapter);
   hr = get_adapter(dxgi_device, (IDXGIAdapter **)&adapter);
   npt_com_release(dxgi_device);
   if (NPT_FAILED(hr) || !adapter) {
      npt_log("swapchain: IDXGIDevice::GetAdapter failed: 0x%x", hr);
      return NULL;
   }

   void *factory = NULL;
   PFN_IDXGIObject_GetParent get_parent =
      NPT_COM_VTBL_FUNC(PFN_IDXGIObject_GetParent,
                         npt_com_vtable(adapter),
                         NPT_VTBL_IDXGIObject_GetParent);
   hr = get_parent(adapter, &NPT_IID_IDXGIFactory2, &factory);
   npt_com_release(adapter);
   if (NPT_FAILED(hr) || !factory) {
      npt_log("swapchain: IDXGIAdapter::GetParent(IDXGIFactory2) failed: 0x%x", hr);
      return NULL;
   }
   *out_owned_factory = factory;
   return factory;
}

/* === Public API === */

struct npt_swapchain *
npt_swapchain_create(struct npt_context *context,
                     void *device, void *factory,
                     uint32_t width, uint32_t height,
                     uint32_t format, uint32_t buffer_count)
{
   return npt_swapchain_dmabuf_create(context, device, factory,
                                      width, height, format, buffer_count);
}

void
npt_swapchain_destroy(struct npt_swapchain *sc)
{
   if (!sc)
      return;

   /* Unregister first so concurrent lookups see NULL — prevents UAF. */
   npt_swapchain_map_unregister(sc);

   /* Break any pending acquire wait so destroy can proceed. */
   mtx_lock(&sc->slots_mutex);
   sc->alive = false;
   cnd_broadcast(&sc->slots_cond);
   mtx_unlock(&sc->slots_mutex);

   if (sc->ops && sc->ops->destroy)
      sc->ops->destroy(sc);

   npt_swapchain_base_fini(sc);
}

void *
npt_swapchain_get_buffer(struct npt_swapchain *sc, uint32_t index,
                         const GUID *riid)
{
   if (!sc || !sc->dxgi_swapchain || index >= sc->buffer_count || !riid)
      return NULL;

   void *out = NULL;
   PFN_IDXGISwapChain_GetBuffer get_buffer =
      NPT_COM_VTBL_FUNC(PFN_IDXGISwapChain_GetBuffer,
                         npt_com_vtable(sc->dxgi_swapchain),
                         NPT_VTBL_IDXGISwapChain_GetBuffer);
   HRESULT hr = get_buffer(sc->dxgi_swapchain, index, riid, &out);
   if (NPT_FAILED(hr)) {
      npt_log("swapchain: GetBuffer(%u) failed (hr=0x%x)", index, hr);
      return NULL;
   }
   return out;
}

void
npt_swapchain_present(struct npt_swapchain *sc, uint32_t sync_interval,
                      uint32_t flags)
{
   if (!sc || !sc->dxgi_swapchain)
      return;

   /* The host backend's post-Present completion callback hands us a
    * GPU-done sync_fd; the next submit_fence pairs it with the
    * guest fence, so no host-side wait stall is needed here.
    *
    * SyncInterval == 0 is async fire-and-forget; SyncInterval > 0
    * makes Present synchronous and relies on the host library to
    * wait on the present fence before returning.  Pass the value
    * through unchanged so the host sees the same hint a native
    * consumer would. */
   PFN_IDXGISwapChain_Present present =
      NPT_COM_VTBL_FUNC(PFN_IDXGISwapChain_Present,
                         npt_com_vtable(sc->dxgi_swapchain),
                         NPT_VTBL_IDXGISwapChain_Present);
   HRESULT hr = present(sc->dxgi_swapchain, sync_interval, flags);
   if (NPT_FAILED(hr))
      npt_log("swapchain: Present failed (hr=0x%x)", hr);
}

bool
npt_swapchain_resize(struct npt_swapchain *sc,
                     uint32_t width, uint32_t height,
                     uint32_t format, uint32_t buffer_count)
{
   if (!sc || !sc->dxgi_swapchain)
      return false;

   if (!buffer_count)
      buffer_count = sc->buffer_count;

   /* DXGI's ResizeBuffers(0, 0, ...) means "use the HWND client
    * area", but our HWND slot points at backend config rather than
    * a real window, so the host D3D11 layer would fall through to
    * a 1×1 backbuffer.  Substitute the dimensions of the most
    * recently published image set instead. */
   uint32_t w = width  ? width  : sc->current_width;
   uint32_t h = height ? height : sc->current_height;
   uint32_t fmt = format ? format : sc->format;
   if (!w || !h) {
      npt_log("swapchain: ResizeBuffers(0,0) with no current size; "
              "current=%ux%u — refusing to pass 0 to host", w, h);
      return false;
   }

   /* Drop stale per-image state: the backend is about to publish a
    * new image set.  Done under slots_mutex so an in-flight acquire
    * observes the reset before onImagesChanged lands. */
   mtx_lock(&sc->slots_mutex);
   for (uint32_t i = 0; i < NPT_SWAPCHAIN_MAX_BUFFERS; i++) {
      if (sc->slots[i].release_fd >= 0) {
         close(sc->slots[i].release_fd);
         sc->slots[i].release_fd = -1;
      }
      sc->slots[i].pending_presents = 0;
   }
   cnd_broadcast(&sc->slots_cond);
   mtx_unlock(&sc->slots_mutex);

   /* Let the backend update any config consumed by ResizeBuffers's
    * publish callback. */
   if (sc->ops && sc->ops->prepare_resize)
      sc->ops->prepare_resize(sc, w, h, fmt);

   PFN_IDXGISwapChain_ResizeBuffers resize =
      NPT_COM_VTBL_FUNC(PFN_IDXGISwapChain_ResizeBuffers,
                         npt_com_vtable(sc->dxgi_swapchain),
                         NPT_VTBL_IDXGISwapChain_ResizeBuffers);
   HRESULT hr = resize(sc->dxgi_swapchain, buffer_count, w, h, fmt, 0);
   if (NPT_FAILED(hr)) {
      npt_log("swapchain: ResizeBuffers failed (hr=0x%x)", hr);
      return false;
   }

   sc->buffer_count = buffer_count;
   sc->format       = fmt;
   return true;
}

bool
npt_swapchain_snapshot_info(struct npt_swapchain *sc,
                            struct npt_swapchain_images_info *out)
{
   if (!sc || !out || !sc->ops || !sc->ops->snapshot_info)
      return false;
   return sc->ops->snapshot_info(sc, out);
}

void
npt_swapchain_image_released(struct npt_swapchain *sc,
                             uint32_t image_index,
                             int release_fd)
{
   if (!sc || image_index >= NPT_SWAPCHAIN_MAX_BUFFERS) {
      if (release_fd >= 0)
         close(release_fd);
      return;
   }

   mtx_lock(&sc->slots_mutex);
   struct npt_swapchain_slot *slot = &sc->slots[image_index];

   /* pending_presents may go negative when an IMAGE_RELEASE arrives
    * before the corresponding present-submitted callback has run;
    * the next present-submitted callback rebalances.  See the slot
    * definition for the race rationale. */
   slot->pending_presents--;

   /* Latest fd replaces any stale one. */
   if (slot->release_fd >= 0)
      close(slot->release_fd);
   slot->release_fd = release_fd;
   cnd_broadcast(&sc->slots_cond);
   mtx_unlock(&sc->slots_mutex);
}

/* ====================================================================== */
/* Per-command helpers                                                    */
/* ====================================================================== */

static struct npt_swapchain *
npt_swapchain_lookup_by_guest_id(struct npt_context *ctx, uint64_t guest_id)
{
   void *dxgi_swapchain = npt_context_lookup_object(
      ctx, NULL, guest_id, NPT_OBJECT_TYPE_IDXGISWAPCHAIN);
   return dxgi_swapchain ? npt_swapchain_wrapper_for(dxgi_swapchain) : NULL;
}

bool
npt_swapchain_get_images(struct npt_context *ctx, uint64_t swapchain_guest_id,
                         uint32_t data_res_id, uint32_t data_off,
                         int32_t *ret)
{
   *ret = -1;

   struct npt_swapchain *sc =
      npt_swapchain_lookup_by_guest_id(ctx, swapchain_guest_id);
   if (!sc) {
      npt_log("GetSwapchainImages: unknown swapchain id=0x%016" PRIx64,
              swapchain_guest_id);
      return false;
   }

   /* Blob registration is done push-style by the backend's
    * onImagesChanged callback; this handler only snapshots state
    * already populated on the host. */
   struct npt_resource *res = npt_context_get_resource(ctx, data_res_id);
   if (!res || !res->u.data) {
      npt_log("GetSwapchainImages: data resource %u not found", data_res_id);
      return false;
   }

   struct npt_swapchain_images_info info;
   if (!npt_swapchain_snapshot_info(sc, &info)) {
      npt_log("GetSwapchainImages: no images published yet for sc=%p",
              (void *)sc);
      return false;
   }

   /* Validate that offset + size fit in the resource.  data_off is
    * guest-supplied: an out-of-range write would corrupt unrelated
    * pool tenants or fall off the mapping. */
   if ((uint64_t)data_off + sizeof(info) > res->size) {
      npt_log("GetSwapchainImages: data_off=%u + sizeof(info)=%zu exceeds "
              "res->size=%zu", data_off, sizeof(info), res->size);
      return false;
   }

   memcpy((uint8_t *)res->u.data + data_off, &info, sizeof(info));

   *ret = 0;
   npt_log("GetSwapchainImages: %u images, virgl_format=%u, %ux%u "
           "(deposited at off=%u)",
           info.num_images, info.virgl_format, info.width, info.height,
           data_off);
   return true;
}

void
npt_swapchain_image_release_by_id(struct npt_context *ctx,
                                  uint64_t swapchain_guest_id,
                                  uint32_t image_index)
{
   struct npt_swapchain *sc =
      npt_swapchain_lookup_by_guest_id(ctx, swapchain_guest_id);
   if (!sc) {
      npt_log("IMAGE_RELEASE: unknown swapchain id=0x%016" PRIx64,
              swapchain_guest_id);
      return;
   }

   /* The consumer has already finished with the image, so no fd is
    * needed.  -1 marks the slot unconditionally free. */
   npt_swapchain_image_released(sc, image_index, -1);
}

void
npt_swapchain_set_display_hints(struct npt_context *ctx,
                                uint32_t virgl_format,
                                uint32_t refresh_num,
                                uint32_t refresh_den,
                                const char *app_path,
                                uint32_t app_path_len)
{
   atomic_store_explicit(&ctx->preferred_virgl_format, virgl_format,
                         memory_order_relaxed);
   atomic_store_explicit(&ctx->preferred_refresh_num, refresh_num,
                         memory_order_relaxed);
   atomic_store_explicit(&ctx->preferred_refresh_den, refresh_den,
                         memory_order_relaxed);

   /* DXVK_APP_PATH gives the host library's per-app profile matcher
    * the guest exe name (our /proc/self/exe is the renderer binary).
    * Must be set before the host library constructs its instance,
    * which happens lazily on the first D3D11/DXGI create.  This
    * dispatcher runs ahead of those creates on the same ring.
    *
    * setenv is process-global, but the render-server fork model puts
    * each guest device in its own worker process so there's no
    * cross-app race.  app_path_len == 0 leaves the env var alone. */
   if (app_path_len > 0 && app_path_len <= NPT_APP_PATH_MAX) {
      char path[NPT_APP_PATH_MAX + 1];
      memcpy(path, app_path, app_path_len);
      path[app_path_len] = '\0';
      setenv("DXVK_APP_PATH", path, 1);
      npt_log("WSI hints: DXVK_APP_PATH=%s", path);
   }
}
