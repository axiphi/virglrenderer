/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGISwapChain dispatch overrides for the methods that need to
 * route through npt_swapchain (Present, GetBuffer, ResizeBuffers,
 * GetDesc, GetHwnd).  Other methods fall through to the generated
 * default dispatcher.
 */

#include "npt_overrides.h"
#include "npt_swapchain.h"

#include "neptune-protocol/npt_protocol_host_idxgiswapchain.h"

/* args->_self is the real host IDXGISwapChain pointer (the
 * object_id the guest got from CreateSwapChain); the wrapper lives
 * in a side map keyed on that pointer. */
static struct npt_swapchain *
sc_wrapper(void *self)
{
   return npt_swapchain_wrapper_for(self);
}

static HRESULT
npt_override_IDXGISwapChain_Present(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_IDXGISwapChain_Present *args,
   UNUSED PFN_IDXGISwapChain_Present original)
{
   struct npt_swapchain *sc = sc_wrapper(args->_self);
   if (!sc) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }
   npt_swapchain_present(sc, args->SyncInterval, args->Flags);
   args->ret = NPT_S_OK;
   return args->ret;
}

/* One backing object answers to several interfaces (Texture2D,
 * IDXGISurface, IDXGIResource, ...).  Pass the guest's riid through
 * so the host library returns the pointer via the matching vtable;
 * the generated post-dispatch register tags it accordingly. */
static HRESULT
npt_override_IDXGISwapChain_GetBuffer(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_IDXGISwapChain_GetBuffer *args,
   UNUSED PFN_IDXGISwapChain_GetBuffer original)
{
   struct npt_swapchain *sc = sc_wrapper(args->_self);
   if (!sc) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }
   if (!args->ppSurface || !args->riid) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }
   void *obj = npt_swapchain_get_buffer(sc, args->Buffer, args->riid);
   if (!obj) {
      *args->ppSurface = NULL;
      args->ret = NPT_E_FAIL;
      return args->ret;
   }
   *args->ppSurface = obj;
   args->ret = NPT_S_OK;
   return args->ret;
}

static HRESULT
npt_override_IDXGISwapChain_ResizeBuffers(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_IDXGISwapChain_ResizeBuffers *args,
   UNUSED PFN_IDXGISwapChain_ResizeBuffers original)
{
   struct npt_swapchain *sc = sc_wrapper(args->_self);
   if (!sc) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }
   if (!npt_swapchain_resize(sc, args->Width, args->Height,
                              args->NewFormat, args->BufferCount)) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }
   args->ret = NPT_S_OK;
   return args->ret;
}

/* Patches OutputWindow / SwapEffect / Flags / Windowed back to what
 * the guest passed in: the host library's swapchain stores our
 * dmabuf-WSI surface in the HWND slot and leaves the rest unused,
 * so without this the guest would see garbage. */
static HRESULT
npt_override_IDXGISwapChain_GetDesc(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_IDXGISwapChain_GetDesc *args,
   PFN_IDXGISwapChain_GetDesc original)
{
   args->ret = original(args->_self, args->pDesc);
   if (NPT_FAILED(args->ret) || !args->pDesc)
      return args->ret;

   struct npt_swapchain *sc = sc_wrapper(args->_self);
   if (sc) {
      args->pDesc->OutputWindow = (HWND)(uintptr_t)sc->guest_hwnd;
      args->pDesc->Flags        = sc->guest_flags;
      args->pDesc->SwapEffect   = (DXGI_SWAP_EFFECT)sc->guest_swap_effect;
      args->pDesc->Windowed     = (BOOL)sc->guest_windowed;
   }
   return args->ret;
}

/* Returns the guest HWND captured at create time.  The host
 * library's GetHwnd would return our dmabuf-WSI surface pointer
 * (what CreateSwapChainForHwnd received), which the guest can't
 * use — the app expects the HWND it originally passed. */
static HRESULT
npt_override_IDXGISwapChain1_GetHwnd(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_IDXGISwapChain1_GetHwnd *args,
   UNUSED PFN_IDXGISwapChain1_GetHwnd original)
{
   if (!args->pHwnd) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }
   struct npt_swapchain *sc = sc_wrapper(args->_self);
   *args->pHwnd = (HWND)(uintptr_t)(sc ? sc->guest_hwnd : NULL);
   args->ret = sc ? NPT_S_OK : NPT_E_INVALIDARG;
   return args->ret;
}

struct npt_dispatch_idxgiswapchain_overrides npt_dxgiswapchain_overrides = {
   .Present       = npt_override_IDXGISwapChain_Present,
   .GetBuffer     = npt_override_IDXGISwapChain_GetBuffer,
   .ResizeBuffers = npt_override_IDXGISwapChain_ResizeBuffers,
   .GetDesc       = npt_override_IDXGISwapChain_GetDesc,
};

struct npt_dispatch_idxgiswapchain1_overrides npt_dxgiswapchain1_overrides = {
   .GetHwnd       = npt_override_IDXGISwapChain1_GetHwnd,
};
