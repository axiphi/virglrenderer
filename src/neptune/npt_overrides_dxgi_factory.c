/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGIFactory CreateSwapChain* overrides.  Each entry point routes
 * through npt_swapchain so the host library never sees the guest's
 * HWND directly.
 */

#include "npt_context.h"
#include "npt_overrides.h"
#include "npt_swapchain.h"

#include "neptune-protocol/npt_protocol_host_idxgifactory.h"

static HRESULT
npt_override_IDXGIFactory_CreateSwapChain(
   struct npt_dispatch_context *dctx,
   struct npt_command_IDXGIFactory_CreateSwapChain *args,
   UNUSED PFN_IDXGIFactory_CreateSwapChain original)
{
   if (!args->pDevice || !args->pDesc) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   const DXGI_SWAP_CHAIN_DESC *desc = args->pDesc;

   struct npt_context *ctx = npt_context_from_dispatch(dctx);
   struct npt_swapchain *sc =
      npt_swapchain_create(ctx, args->pDevice, args->_self,
                            desc->BufferDesc.Width,
                            desc->BufferDesc.Height,
                            desc->BufferDesc.Format,
                            desc->BufferCount);
   if (!sc) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }

   sc->primary_guest_id = args->_guest_id_ppSwapChain;

   /* Cache guest-facing fields the host library's swapchain can't
    * round-trip (its HWND slot holds our dmabuf_config pointer). */
   sc->guest_hwnd = (void *)(uintptr_t)desc->OutputWindow;
   sc->guest_flags = desc->Flags;
   sc->guest_swap_effect = desc->SwapEffect;
   sc->guest_windowed = desc->Windowed;

   /* Expose the host-library swapchain directly so the protocol's
    * default COM dispatch invokes the host methods. */
   if (args->ppSwapChain)
      *args->ppSwapChain = (IDXGISwapChain *)sc->dxgi_swapchain;
   args->ret = NPT_S_OK;
   return args->ret;
}

/* Captures the guest hWnd separately so IDXGISwapChain1::GetHwnd can
 * round-trip it; the host library receives our dmabuf-WSI surface
 * pointer in the HWND slot. */
static HRESULT
npt_override_IDXGIFactory2_CreateSwapChainForHwnd(
   struct npt_dispatch_context *dctx,
   struct npt_command_IDXGIFactory2_CreateSwapChainForHwnd *args,
   UNUSED PFN_IDXGIFactory2_CreateSwapChainForHwnd original)
{
   if (!args->pDevice || !args->pDesc) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   const DXGI_SWAP_CHAIN_DESC1 *desc = args->pDesc;

   struct npt_context *ctx = npt_context_from_dispatch(dctx);
   struct npt_swapchain *sc =
      npt_swapchain_create(ctx, args->pDevice, args->_self,
                            desc->Width, desc->Height,
                            desc->Format, desc->BufferCount);
   if (!sc) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }

   sc->primary_guest_id  = args->_guest_id_ppSwapChain;
   sc->guest_hwnd        = (void *)(uintptr_t)args->hWnd;
   sc->guest_flags       = desc->Flags;
   sc->guest_swap_effect = desc->SwapEffect;
   /* Windowed lives on a separate DXGI_SWAP_CHAIN_FULLSCREEN_DESC;
    * NULL means windowed. */
   sc->guest_windowed    = args->pFullscreenDesc
                              ? (int32_t)args->pFullscreenDesc->Windowed
                              : 1;

   if (args->ppSwapChain)
      *args->ppSwapChain = (IDXGISwapChain1 *)sc->dxgi_swapchain;
   args->ret = NPT_S_OK;
   return args->ret;
}

struct npt_dispatch_idxgifactory_overrides npt_dxgifactory_overrides = {
   .CreateSwapChain = npt_override_IDXGIFactory_CreateSwapChain,
};

struct npt_dispatch_idxgifactory2_overrides npt_dxgifactory2_overrides = {
   .CreateSwapChainForHwnd = npt_override_IDXGIFactory2_CreateSwapChainForHwnd,
};
