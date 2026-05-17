/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Backend-neutral swapchain wrapper.
 *
 * Wraps the host IDXGISwapChain so the guest sees an ordinary D3D11
 * swapchain while the host library is bound to a presentation
 * backend that exports the images for the guest VM's compositor.
 * Per-image slot state, guest-facing fields the real DXGI swapchain
 * can't round-trip, and backend dispatch all live here.
 *
 * Three image formats are visible at this layer:
 *
 *   1. sc->format — DXGI_FORMAT the application requested at
 *      CreateSwapChain time.  Round-trips through GetDesc.
 *
 *   2. ctx->preferred_virgl_format — guest hint from
 *      SET_PREFERRED_FORMAT derived from its consumer.  Backends
 *      translate to the host library's WSI surface format; honoured
 *      when compatible with (1), otherwise replaced by the backend's
 *      default.
 *
 *   3. The format the backend's presentation surface actually
 *      published.  Snapshot via snapshot_info, reported on the wire
 *      as virgl_format.  When (2) was honoured this matches it;
 *      otherwise (1) is preserved via an internal swizzle.
 *
 * The wire field in npt_swapchain_images_info carries (3) — the
 * surface's physical layout, which is what a downstream WSI consumer
 * needs to bind the image correctly.
 */

#ifndef NPT_SWAPCHAIN_H
#define NPT_SWAPCHAIN_H

#include "npt_common.h"

typedef struct _GUID GUID;

struct npt_context;
struct npt_swapchain;
struct npt_swapchain_images_info;

/* Hard cap on swapchain images.  Backends may publish fewer.  This
 * value sets the slot-table size and the wire reply array size;
 * raising it requires a wire bump. */
#define NPT_SWAPCHAIN_MAX_BUFFERS 4

/* Per-image state.  pending_presents = present-submits − releases.
 * The backend's present callback increments; IMAGE_RELEASE decrements;
 * the acquire callback waits while the count is > 0.
 *
 * The counter is signed because IMAGE_RELEASE can arrive before the
 * matching present-submitted callback has run on the backend submit
 * thread.  A bool would record "no pending" (no-op clear) and then
 * latch true on the late callback, with no future IMAGE_RELEASE
 * coming — the slot would wedge and the backend's frame worker
 * would deadlock on acquire.  Dipping to -1 and back to 0 keeps the
 * slot correctly recognized as free.  Initial value 0 = "no
 * presents, no releases yet — slot is free." */
struct npt_swapchain_slot {
   int32_t pending_presents;
   int     release_fd;
};

/* Backend dispatch table.  All ops receive the base pointer; a
 * backend casts it to its derived struct via container_of. */
struct npt_swapchain_ops {
   /* Backend-specific teardown, called from npt_swapchain_destroy
    * before the common cleanup runs.  May not free the wrapper. */
   void (*destroy)(struct npt_swapchain *sc);

   /* Refresh any backend-internal state (config, format hints) that
    * needs updating before the host's IDXGISwapChain::ResizeBuffers
    * is invoked.  The common code calls ResizeBuffers afterwards. */
   void (*prepare_resize)(struct npt_swapchain *sc,
                          uint32_t width, uint32_t height,
                          uint32_t format);

   /* Fill the wire reply describing the currently published image
    * set.  Returns false when no images have been published yet. */
   bool (*snapshot_info)(struct npt_swapchain *sc,
                         struct npt_swapchain_images_info *out);
};

/* Base swapchain struct.  Backends (e.g. struct npt_swapchain_dmabuf)
 * embed this as their first field. */
struct npt_swapchain {
   const struct npt_swapchain_ops *ops;

   /* Required: caller-managed lifetime; ctx outlives the swapchain. */
   struct npt_context *context;

   void *device;          /* host ID3D11Device                 */
   void *dxgi_swapchain;  /* host IDXGISwapChain1 (after create) */

   /* Guest-allocated id of the IDXGISwapChain wrapper that owns the
    * swapchain's lifetime.  COM_RELEASE on this id triggers destroy;
    * QI-derived alias ids (IDXGISwapChain1..4) just drop a host ref. */
   uint64_t primary_guest_id;

   /* Snapshot of the create-time / resize-time arguments so guest
    * replies can round-trip the desc cleanly.  The currently
    * published image set is whatever the backend's snapshot_info
    * reports. */
   uint32_t buffer_count;
   uint32_t format;

   /* Most recently published image dimensions, mirrored from the
    * backend's onImagesChanged callback.  Used as the resize
    * fallback when the guest calls ResizeBuffers(0, 0, ...): our
    * HWND slot is backend state, not a real window, so we substitute
    * the current dimensions to avoid the host layer collapsing to a
    * 1×1 backbuffer. */
   uint32_t current_width;
   uint32_t current_height;

   /* Generation last observed via onImagesChanged.  Backends bump
    * this on each publish so common code can drop stale per-image
    * bookkeeping. */
   uint64_t images_generation;

   /* Guest-facing fields the real DXGI swapchain can't round-trip
    * (its HWND slot holds backend state).  GetDesc / GetHwnd
    * overrides patch these back. */
   void    *guest_hwnd;
   uint32_t guest_flags;
   uint32_t guest_swap_effect;
   int32_t  guest_windowed;

   /* Per-image release tracking; all fields guarded by slots_mutex. */
   struct npt_swapchain_slot slots[NPT_SWAPCHAIN_MAX_BUFFERS];
   mtx_t                     slots_mutex;
   cnd_t                     slots_cond;
   bool                      alive; /* cleared by destroy to unblock acquire */
};

/* Public API — call from override sites in the dispatcher. */

/* Build a swapchain backed by the platform's presentation backend.
 * Returns NULL on failure. */
struct npt_swapchain *
npt_swapchain_create(struct npt_context *context,
                     void *device, void *factory,
                     uint32_t width, uint32_t height,
                     uint32_t format, uint32_t buffer_count);

void
npt_swapchain_destroy(struct npt_swapchain *sc);

/* The real host IDXGISwapChain pointer is the guest object_id;
 * recover the wrapper via this lookup.  NULL on miss. */
struct npt_swapchain *
npt_swapchain_wrapper_for(const void *dxgi_swapchain);

/* riid selects the COM interface the caller wants; the host library
 * returns the same backing object viewed through that vtable.  The
 * caller registers it under the matching NPT_OBJECT_TYPE_*. */
void *
npt_swapchain_get_buffer(struct npt_swapchain *sc, uint32_t index,
                         const GUID *riid);

void
npt_swapchain_present(struct npt_swapchain *sc, uint32_t sync_interval,
                      uint32_t flags);

bool
npt_swapchain_resize(struct npt_swapchain *sc,
                     uint32_t width, uint32_t height,
                     uint32_t format, uint32_t buffer_count);

/* Snapshot the current image set for a wire reply.  Fills
 * width/height/virgl_format/num_images and the per-image
 * (blob_id, stride) pairs.  Returns false when no images have been
 * published yet (caller should reply with num_images=0). */
bool
npt_swapchain_snapshot_info(struct npt_swapchain *sc,
                            struct npt_swapchain_images_info *out);

/* Takes ownership of \p release_fd (-1 = no sync, just mark free). */
void
npt_swapchain_image_released(struct npt_swapchain *sc,
                             uint32_t image_index,
                             int release_fd);

/* Helpers for backend implementations.  Not part of the consumer API. */

/* Init the base struct's slot tracking and ops vtable.  Backends
 * call this from their create function after allocating their
 * derived struct.  Returns false on mutex/cond init failure. */
bool
npt_swapchain_base_init(struct npt_swapchain *sc,
                        const struct npt_swapchain_ops *ops,
                        struct npt_context *context,
                        void *device,
                        uint32_t buffer_count, uint32_t format);

/* Tear down the base's slot tracking and COM refs.  Backends call
 * this AFTER releasing their own resources.  Frees `sc`. */
void
npt_swapchain_base_fini(struct npt_swapchain *sc);

/* Resolve a host IDXGIFactory2 from a host ID3D11Device when the
 * caller didn't provide one.  On success returns the factory and
 * stores it in *out_owned_factory (caller must npt_com_release). */
void *
npt_swapchain_derive_factory(void *device, void **out_owned_factory);

/* Register `sc` in the wrapper_for map.  Backends call this after a
 * successful CreateSwapChainForHwnd; destroy unregisters automatically. */
bool
npt_swapchain_map_register(struct npt_swapchain *sc);

/* ====================================================================== */
/* Per-command helpers used by the transport dispatcher.                  */
/* ====================================================================== */

/* GET_SWAPCHAIN_IMAGES: snapshot the current image set into the SHM
 * resource at (data_res_id, data_off) and write *ret.  data_off
 * is mandatory because the guest's info slots are bump-allocated
 * out of a shared pool. */
bool
npt_swapchain_get_images(struct npt_context *ctx,
                         uint64_t swapchain_guest_id,
                         uint32_t data_res_id,
                         uint32_t data_off,
                         int32_t *ret);

/* IMAGE_RELEASE: mark the given image slot free.  No-op (with log) on
 * unknown swapchain id. */
void
npt_swapchain_image_release_by_id(struct npt_context *ctx,
                                  uint64_t swapchain_guest_id,
                                  uint32_t image_index);

/* SET_PREFERRED_DISPLAY_INFO: store the guest's format / refresh
 * hints on the context and apply the optional exe path via
 * DXVK_APP_PATH.  app_path_len == 0 leaves DXVK_APP_PATH untouched. */
void
npt_swapchain_set_display_hints(struct npt_context *ctx,
                                uint32_t virgl_format,
                                uint32_t refresh_num,
                                uint32_t refresh_den,
                                const char *app_path,
                                uint32_t app_path_len);

#endif /* NPT_SWAPCHAIN_H */
