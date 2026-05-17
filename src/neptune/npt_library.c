/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host-side D3D backend library loader.  Any library may be absent;
 * the corresponding top-level overrides return E_FAIL in that case.
 */

#include "npt_library.h"

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif

/* Defaults; override via NPT_*_LIBRARY_PATH env vars. */
#define NPT_D3D11_LIBRARY_DEFAULT "libd3d11.so"
#define NPT_DXGI_LIBRARY_DEFAULT  "libdxgi.so"
#define NPT_D3D12_LIBRARY_DEFAULT "libvkd3d-proton-d3d12.so"

#ifdef HAVE_DLFCN_H

static void *
npt_library_open(const char *env_var, const char *default_name)
{
   const char *path = getenv(env_var);
   if (!path)
      path = default_name;

   dlerror(); /* clear */
   void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!handle) {
      npt_log("failed to open %s (%s): %s", env_var, path, dlerror());
   }
   return handle;
}

static void *
npt_library_sym(void *handle, const char *name)
{
   dlerror(); /* clear */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   void *sym = dlsym(handle, name);
#pragma GCC diagnostic pop

   const char *error = dlerror();
   if (error) {
      npt_log("failed to load %s: %s", name, error);
      return NULL;
   }
   return sym;
}

#endif /* HAVE_DLFCN_H */

bool
npt_library_init(struct npt_d3d_library *lib)
{
   memset(lib, 0, sizeof(*lib));

#ifdef HAVE_DLFCN_H
   /* The swapchain wrapper requires the host D3D11/DXGI library's
    * dmabuf WSI backend, selected by DXVK_WSI_DRIVER=Dmabuf.  Flag 0
    * preserves a user-set value.  No-op for libraries that don't
    * honour this variable. */
   setenv("DXVK_WSI_DRIVER", "Dmabuf", 0);

   lib->d3d11_module = npt_library_open("NPT_D3D11_LIBRARY_PATH",
                                         NPT_D3D11_LIBRARY_DEFAULT);
   if (lib->d3d11_module) {
      lib->pfn_D3D11CreateDevice =
         ((union { void *p; PFN_D3D11CreateDevice f; }){
            .p = npt_library_sym(lib->d3d11_module, "D3D11CreateDevice")
         }).f;
      if (!lib->pfn_D3D11CreateDevice) {
         npt_log("D3D11 library loaded but D3D11CreateDevice not found");
         dlclose(lib->d3d11_module);
         lib->d3d11_module = NULL;
      } else {
         /* Optional. */
         lib->pfn_D3D11CreateDeviceAndSwapChain =
            ((union { void *p; PFN_D3D11CreateDeviceAndSwapChain f; }){
               .p = npt_library_sym(lib->d3d11_module,
                                    "D3D11CreateDeviceAndSwapChain")
            }).f;
         lib->pfn_D3D11On12CreateDevice =
            ((union { void *p; PFN_D3D11On12CreateDevice f; }){
               .p = npt_library_sym(lib->d3d11_module,
                                    "D3D11On12CreateDevice")
            }).f;
      }
   }

   lib->dxgi_module = npt_library_open("NPT_DXGI_LIBRARY_PATH",
                                        NPT_DXGI_LIBRARY_DEFAULT);
   if (lib->dxgi_module) {
      lib->pfn_CreateDXGIFactory1 =
         ((union { void *p; PFN_CreateDXGIFactory1 f; }){
            .p = npt_library_sym(lib->dxgi_module, "CreateDXGIFactory1")
         }).f;
      if (!lib->pfn_CreateDXGIFactory1) {
         npt_log("DXGI library loaded but CreateDXGIFactory1 not found");
         dlclose(lib->dxgi_module);
         lib->dxgi_module = NULL;
      }
   }

   lib->d3d12_module = npt_library_open("NPT_D3D12_LIBRARY_PATH",
                                         NPT_D3D12_LIBRARY_DEFAULT);
   if (lib->d3d12_module) {
      lib->pfn_D3D12CreateDevice =
         ((union { void *p; PFN_D3D12CreateDevice f; }){
            .p = npt_library_sym(lib->d3d12_module, "D3D12CreateDevice")
         }).f;
      if (!lib->pfn_D3D12CreateDevice) {
         npt_log("D3D12 library loaded but D3D12CreateDevice not found");
         dlclose(lib->d3d12_module);
         lib->d3d12_module = NULL;
      }
   }

   if (lib->d3d11_module) {
      const char *p = getenv("NPT_D3D11_LIBRARY_PATH");
      npt_log("loaded D3D11 library: %s", p ? p : NPT_D3D11_LIBRARY_DEFAULT);
   }
   if (lib->dxgi_module) {
      const char *p = getenv("NPT_DXGI_LIBRARY_PATH");
      npt_log("loaded DXGI library: %s", p ? p : NPT_DXGI_LIBRARY_DEFAULT);
   }
   if (lib->d3d12_module) {
      const char *p = getenv("NPT_D3D12_LIBRARY_PATH");
      npt_log("loaded D3D12 library: %s", p ? p : NPT_D3D12_LIBRARY_DEFAULT);
   }
#else
   npt_log("D3D library loading: dlopen not available");
#endif

   return true;
}

void
npt_library_fini(struct npt_d3d_library *lib)
{
#ifdef HAVE_DLFCN_H
   if (lib->d3d11_module)
      dlclose(lib->d3d11_module);
   if (lib->dxgi_module)
      dlclose(lib->dxgi_module);
   if (lib->d3d12_module)
      dlclose(lib->d3d12_module);
#endif

   memset(lib, 0, sizeof(*lib));
}
