/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 resource manipulation for the RESOURCE_{UPDATE, MAP, UNMAP}
 * transport commands.  Resolves host resources via the object table,
 * walks the COM vtable for D3D11 Map/Unmap/Update, and (for MAP)
 * stashes per-resource map_state on the SHM blob so the matching
 * UNMAP can write back.
 */

#include "npt_resource.h"

#include <string.h>

#include "npt_com.h"
#include "npt_context.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

void
npt_resource_update(struct npt_context *ctx,
                    uint64_t resource_id, uint32_t subresource,
                    uint32_t row_pitch, uint32_t depth_pitch,
                    UNUSED uint32_t byte_size,
                    bool has_box,
                    uint32_t box_left, uint32_t box_top, uint32_t box_front,
                    uint32_t box_right, uint32_t box_bottom, uint32_t box_back,
                    const void *payload)
{
   /* Recover the device and immediate context from the resource.
    * Both calls add refs that we release after UpdateSubresource. */
   void *resource = npt_context_lookup_object(ctx, NULL, resource_id,
                                              NPT_OBJECT_TYPE_IUNKNOWN);
   if (!resource) {
      npt_log("resource_update: NULL resource");
      return;
   }

   ID3D11Device *device = NULL;
   PFN_ID3D11DeviceChild_GetDevice get_dev =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceChild_GetDevice,
                        npt_com_vtable(resource),
                        NPT_VTBL_ID3D11DeviceChild_GetDevice);
   get_dev(resource, &device);
   if (!device) {
      npt_log("resource_update: GetDevice returned NULL");
      return;
   }

   ID3D11DeviceContext *imm_ctx = NULL;
   PFN_ID3D11Device_GetImmediateContext get_ctx =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Device_GetImmediateContext,
                        npt_com_vtable(device),
                        NPT_VTBL_ID3D11Device_GetImmediateContext);
   get_ctx(device, &imm_ctx);
   if (!imm_ctx) {
      npt_log("resource_update: GetImmediateContext returned NULL");
      npt_com_release(device);
      return;
   }

   D3D11_BOX box;
   const D3D11_BOX *box_arg = NULL;
   if (has_box) {
      box.left   = box_left;
      box.top    = box_top;
      box.front  = box_front;
      box.right  = box_right;
      box.bottom = box_bottom;
      box.back   = box_back;
      box_arg = &box;
   }

   PFN_ID3D11DeviceContext_UpdateSubresource update =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_UpdateSubresource,
                        npt_com_vtable(imm_ctx),
                        NPT_VTBL_ID3D11DeviceContext_UpdateSubresource);
   update(imm_ctx, resource, subresource, box_arg, payload,
          row_pitch, depth_pitch);

   npt_com_release(imm_ctx);
   npt_com_release(device);
}

/* Returns 0 on invalid flags (causes D3D11 Map to fail). */
static D3D11_MAP
npt_access_flags_to_d3d11_map(uint32_t access_flags)
{
   const bool read = access_flags & NPT_MAP_ACCESS_READ;
   const bool write = access_flags & NPT_MAP_ACCESS_WRITE;

   if (read && write)
      return D3D11_MAP_READ_WRITE;
   if (read)
      return D3D11_MAP_READ;
   if (access_flags & NPT_MAP_ACCESS_DISCARD)
      return D3D11_MAP_WRITE_DISCARD;
   if (access_flags & NPT_MAP_ACCESS_NO_OVERWRITE)
      return D3D11_MAP_WRITE_NO_OVERWRITE;
   if (write)
      return D3D11_MAP_WRITE;

   return (D3D11_MAP)0;
}

HRESULT
npt_resource_map(struct npt_context *ctx,
                 uint64_t context_id, uint64_t resource_id,
                 uint32_t subresource, uint32_t access_flags,
                 uint32_t api_map_flags, uint32_t shmem_res_id,
                 UNUSED uint64_t read_range_begin,
                 UNUSED uint64_t read_range_end,
                 uint64_t byte_size,
                 uint32_t mip_height, uint32_t mip_depth,
                 uint32_t *out_row_pitch, uint32_t *out_depth_pitch,
                 uint32_t *out_mapped_size)
{
   *out_row_pitch = 0;
   *out_depth_pitch = 0;
   *out_mapped_size = 0;

   void *resource = npt_context_lookup_object(ctx, NULL, resource_id,
                                              NPT_OBJECT_TYPE_IUNKNOWN);
   if (!resource) {
      npt_log("map_resource: NULL resource");
      return NPT_E_FAIL;
   }

   struct npt_resource *shmem_res =
      npt_context_get_resource(ctx, shmem_res_id);
   if (!shmem_res || shmem_res->fd_type != VIRGL_RESOURCE_FD_SHM ||
       !shmem_res->u.data) {
      npt_log("map_resource: invalid SHM resource %u", shmem_res_id);
      return NPT_E_FAIL;
   }

   if (!context_id) {
      /* TODO: D3D12: call ID3D12Resource::Map(subresource, pReadRange, &pData)
       *               using read_range_{begin,end}. */
      npt_log("map_resource: D3D12 path not implemented");
      return NPT_E_FAIL;
   }

   void *imm_ctx = npt_context_lookup_object(ctx, NULL, context_id,
                                             NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT);
   if (!imm_ctx) {
      npt_log("map_resource: NULL immediate context");
      return NPT_E_FAIL;
   }

   D3D11_MAP d3d11_map_type = npt_access_flags_to_d3d11_map(access_flags);

   /* May block on GPU sync; the guest's spin on the ring head
    * propagates the stall. */
   PFN_ID3D11DeviceContext_Map map_fn =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Map,
                        npt_com_vtable(imm_ctx),
                        NPT_VTBL_ID3D11DeviceContext_Map);
   D3D11_MAPPED_SUBRESOURCE mapped;
   memset(&mapped, 0, sizeof(mapped));
   HRESULT hr = map_fn(imm_ctx, resource, subresource,
                       d3d11_map_type, api_map_flags, &mapped);

   if (NPT_FAILED(hr))
      return hr;

   /* Memcpy bound for READ (and matching WRITE on the Unmap path):
    *   1. (mip_height, mip_depth) for textures — tightest, required
    *      since the guest can't know RowPitch ahead of Map.
    *   2. byte_size for buffers.
    *   3. shmem size as fallback.
    * Clamped to shmem_res->size so an oversized blob can't over-read
    * past D3D's mapped region into unrelated host memory. */
   uint64_t bound;
   if (mip_height && mip_depth) {
      bound = (uint64_t)mapped.RowPitch *
              (uint64_t)mip_height *
              (uint64_t)mip_depth;
   } else {
      bound = byte_size ? byte_size : shmem_res->size;
   }
   if (bound > shmem_res->size)
      bound = shmem_res->size;
   const uint32_t mapped_size = (uint32_t)bound;

   if (access_flags & NPT_MAP_ACCESS_READ)
      memcpy(shmem_res->u.data, mapped.pData, mapped_size);

   shmem_res->map_state = (struct npt_resource_map_state){
      .mapped_data  = mapped.pData,
      .row_pitch    = mapped.RowPitch,
      .depth_pitch  = mapped.DepthPitch,
      .mapped_size  = mapped_size,
      .access_flags = access_flags,
      .is_mapped    = true,
      .persistent   = !!(access_flags & NPT_MAP_ACCESS_PERSISTENT),
   };

   *out_row_pitch = mapped.RowPitch;
   *out_depth_pitch = mapped.DepthPitch;
   *out_mapped_size = mapped_size;
   return hr;
}

HRESULT
npt_resource_unmap(struct npt_context *ctx,
                   uint64_t context_id, uint64_t resource_id,
                   uint32_t subresource, uint32_t shmem_res_id,
                   uint32_t shmem_offset, uint64_t byte_size,
                   uint32_t access_flags,
                   UNUSED uint64_t written_range_begin,
                   UNUSED uint64_t written_range_end)
{
   void *resource = npt_context_lookup_object(ctx, NULL, resource_id,
                                              NPT_OBJECT_TYPE_IUNKNOWN);
   if (!resource) {
      npt_log("unmap_resource: NULL resource");
      return NPT_E_FAIL;
   }

   struct npt_resource *shmem_res =
      npt_context_get_resource(ctx, shmem_res_id);
   if (!shmem_res || shmem_res->fd_type != VIRGL_RESOURCE_FD_SHM ||
       !shmem_res->u.data) {
      npt_log("unmap_resource: invalid SHM resource %u", shmem_res_id);
      return NPT_E_FAIL;
   }

   /* Bounds check the slot window. */
   if ((uint64_t)shmem_offset + byte_size > shmem_res->size) {
      npt_log("unmap_resource: slot window [%u, %u+%" PRIu64 ") "
              "exceeds shmem size %zu",
              shmem_offset, shmem_offset, byte_size,
              shmem_res->size);
      return NPT_E_FAIL;
   }
   const uint8_t *slot_src =
      (const uint8_t *)shmem_res->u.data + shmem_offset;

   if (access_flags) {
      /* Rename-ring path: no prior MAP_RESOURCE; replay the full
       * Map + memcpy + Unmap cycle here. */
      if (!context_id) {
         npt_log("unmap_resource: access_flags path requires context_id");
         return NPT_E_FAIL;
      }

      void *imm_ctx = npt_context_lookup_object(ctx, NULL, context_id,
                                                 NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT);
      if (!imm_ctx) {
         npt_log("unmap_resource: NULL immediate context");
         return NPT_E_FAIL;
      }

      D3D11_MAP d3d11_map_type =
         npt_access_flags_to_d3d11_map(access_flags);

      PFN_ID3D11DeviceContext_Map map_fn =
         NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Map,
                           npt_com_vtable(imm_ctx),
                           NPT_VTBL_ID3D11DeviceContext_Map);
      PFN_ID3D11DeviceContext_Unmap unmap_fn =
         NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Unmap,
                           npt_com_vtable(imm_ctx),
                           NPT_VTBL_ID3D11DeviceContext_Unmap);

      D3D11_MAPPED_SUBRESOURCE mapped;
      memset(&mapped, 0, sizeof(mapped));
      HRESULT hr = map_fn(imm_ctx, resource, subresource,
                          d3d11_map_type, 0, &mapped);
      if (NPT_FAILED(hr)) {
         npt_log("unmap_resource: rename-ring Map failed 0x%08x", hr);
         return NPT_E_FAIL;
      }

      if ((access_flags & NPT_MAP_ACCESS_WRITE) && mapped.pData) {
         uint64_t bound = byte_size;
         if ((uint64_t)shmem_offset + bound > shmem_res->size)
            bound = shmem_res->size - shmem_offset;
         memcpy(mapped.pData, slot_src, (size_t)bound);
      }

      unmap_fn(imm_ctx, resource, subresource);
      return NPT_S_OK;
   }

   /* Paired with a prior MAP_RESOURCE. */
   if (!shmem_res->map_state.is_mapped) {
      npt_log("unmap_resource: SHM resource %u not mapped",
              shmem_res_id);
      return NPT_E_FAIL;
   }

   if (shmem_res->map_state.persistent) {
      npt_log("unmap_resource: SHM resource %u is persistent, "
              "ignoring unmap", shmem_res_id);
      return NPT_E_FAIL;
   }

   if (shmem_res->map_state.access_flags & NPT_MAP_ACCESS_WRITE) {
      /* MIN(guest byte_size, recorded mapped_size) so we don't
       * overrun the D3D region with stale/padded SHM bytes.  0 means
       * "use the full mapped_size". */
      uint64_t bound = byte_size ? byte_size
                                 : shmem_res->map_state.mapped_size;
      if (bound > shmem_res->map_state.mapped_size)
         bound = shmem_res->map_state.mapped_size;
      memcpy(shmem_res->map_state.mapped_data, slot_src, (size_t)bound);
   }

   if (!context_id) {
      /* TODO: D3D12: call ID3D12Resource::Unmap(subresource, pWrittenRange) */
      npt_log("unmap_resource: D3D12 path not implemented");
      return NPT_E_FAIL;
   }

   void *imm_ctx = npt_context_lookup_object(ctx, NULL, context_id,
                                              NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT);
   PFN_ID3D11DeviceContext_Unmap unmap_fn =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Unmap,
                        npt_com_vtable(imm_ctx),
                        NPT_VTBL_ID3D11DeviceContext_Unmap);
   unmap_fn(imm_ctx, resource, subresource);

   shmem_res->map_state.is_mapped = false;
   shmem_res->map_state.mapped_data = NULL;
   return NPT_S_OK;
}
