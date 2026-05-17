/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_LIBRARY_H
#define NPT_LIBRARY_H

#include "npt_common.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

/*
 * Loads the host D3D backend libraries.  Path lookup: NPT_*_LIBRARY_PATH
 * env var if set, otherwise the built-in default name.
 */

struct npt_d3d_library {
   void *d3d11_module;
   void *dxgi_module;
   void *d3d12_module;

   PFN_D3D11CreateDevice pfn_D3D11CreateDevice;
   PFN_D3D11CreateDeviceAndSwapChain pfn_D3D11CreateDeviceAndSwapChain;
   PFN_D3D11On12CreateDevice pfn_D3D11On12CreateDevice;

   PFN_CreateDXGIFactory1 pfn_CreateDXGIFactory1;

   PFN_D3D12CreateDevice pfn_D3D12CreateDevice;
};

bool
npt_library_init(struct npt_d3d_library *lib);

void
npt_library_fini(struct npt_d3d_library *lib);

#endif /* NPT_LIBRARY_H */
