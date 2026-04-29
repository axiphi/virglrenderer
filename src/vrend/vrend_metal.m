/*
 * Copyright 2025 Turing Software, LLC
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "virglrenderer.h"
#include "vrend_metal.h"
#include "pipe/p_state.h"
#include "util/u_math.h"
#include <Metal/Metal.h>

struct metal_format_conversion {
   uint32_t virgl_format;
   MTLPixelFormat metal_format;
};

static bool virgl_format_to_metal_format(uint32_t format, MTLPixelFormat *metal_format)
{
   static const struct metal_format_conversion conversions[] = {
      { VIRGL_FORMAT_R8G8B8A8_UNORM, MTLPixelFormatRGBA8Unorm },
      { VIRGL_FORMAT_R8G8B8X8_UNORM, MTLPixelFormatRGBA8Unorm },
      { VIRGL_FORMAT_R8G8B8A8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB },
      { VIRGL_FORMAT_R8G8B8X8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB },
      { VIRGL_FORMAT_B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm },
      { VIRGL_FORMAT_B8G8R8X8_UNORM, MTLPixelFormatBGRA8Unorm },
      { VIRGL_FORMAT_B8G8R8A8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB },
      { VIRGL_FORMAT_B8G8R8X8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB },
      { VIRGL_FORMAT_R16G16B16A16_FLOAT, MTLPixelFormatRGBA16Float },
      { VIRGL_FORMAT_R16G16B16X16_FLOAT, MTLPixelFormatRGBA16Float },
      { VIRGL_FORMAT_R16G16B16A16_UNORM, MTLPixelFormatRGBA16Unorm },
      { VIRGL_FORMAT_R16G16B16X16_UNORM, MTLPixelFormatRGBA16Unorm },
      { VIRGL_FORMAT_R32G32B32A32_FLOAT, MTLPixelFormatRGBA32Float },
      { VIRGL_FORMAT_R10G10B10A2_UNORM, MTLPixelFormatRGB10A2Unorm },
      { VIRGL_FORMAT_R10G10B10X2_UNORM, MTLPixelFormatRGB10A2Unorm },
      { VIRGL_FORMAT_B10G10R10A2_UNORM, MTLPixelFormatBGR10A2Unorm },
      { VIRGL_FORMAT_B10G10R10X2_UNORM, MTLPixelFormatBGR10A2Unorm },
      { VIRGL_FORMAT_R8_UNORM, MTLPixelFormatR8Unorm },
      { VIRGL_FORMAT_R16_UNORM, MTLPixelFormatR16Unorm },
      { VIRGL_FORMAT_R8G8_UNORM, MTLPixelFormatRG8Unorm },
      { VIRGL_FORMAT_R16G16_UNORM, MTLPixelFormatRG16Unorm },
      { VIRGL_FORMAT_B5G6R5_UNORM, MTLPixelFormatB5G6R5Unorm },
      // { VIRGL_FORMAT_NV12, }, /* Conversion not possible */
      // { VIRGL_FORMAT_YV12, }, /* Conversion not possible */
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(conversions); i++) {
      if (conversions[i].virgl_format == format) {
         *metal_format = conversions[i].metal_format;
         return true;
      }
   }

   return false;
}

static MTLTextureUsage virgl_bind_to_metal_usage_flags(uint32_t flags)
{
   MTLTextureUsage ret = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

   if (flags & PIPE_BIND_RENDER_TARGET)
      ret |= MTLTextureUsageRenderTarget;
   if (flags & PIPE_BIND_DEPTH_STENCIL)
      ret |= MTLTextureUsageRenderTarget;

   return ret;
}

static MTLResourceOptions virgl_usage_to_metal_resource_options(uint32_t usage)
{
   switch (usage) {
   case PIPE_USAGE_DEFAULT:
   case PIPE_USAGE_STAGING:
   default:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
   case PIPE_USAGE_IMMUTABLE:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined | MTLHazardTrackingModeUntracked;
   case PIPE_USAGE_DYNAMIC:
   case PIPE_USAGE_STREAM:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
   }
}

static MTLTextureDescriptor *new_descriptor(const struct vrend_metal_texture_description *desc)
{
   MTLPixelFormat pixel_format;

   if (!virgl_format_to_metal_format(desc->format, &pixel_format)) {
      return NULL;
   }

   MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
   descriptor.textureType = MTLTextureType2D;
   descriptor.pixelFormat = pixel_format;
   descriptor.width = desc->width;
   descriptor.height = desc->height;
   descriptor.resourceOptions = virgl_usage_to_metal_resource_options(desc->usage);
   descriptor.usage = virgl_bind_to_metal_usage_flags(desc->bind);
   if (desc->usage == PIPE_USAGE_IMMUTABLE) {
      descriptor.usage &= ~MTLTextureUsageShaderWrite;
   }

   return descriptor;
}

bool virgl_metal_create_texture(MTLDevice_id device,
                                const struct vrend_metal_texture_description *desc,
                                MTLTexture_id *tex)
{
   id<MTLDevice> mtl_device = (id<MTLDevice>)device;
   MTLTextureDescriptor *descriptor = new_descriptor(desc);
   if (descriptor) {
      *tex = [mtl_device newTextureWithDescriptor:descriptor];
      [descriptor release];
      return true;
   }

   return false;
}

bool virgl_metal_create_texture_from_heap(MTLHeap_id heap,
                                          const struct vrend_metal_texture_description *desc,
                                          MTLTexture_id *tex)
{
   id<MTLHeap> mtl_heap = (id<MTLHeap>)heap;
   id<MTLDevice> mtl_device = mtl_heap.device;
   MTLTextureDescriptor *descriptor = new_descriptor(desc);
   *tex = nil;
   if (descriptor) {
      NSUInteger deviceAlignment, bytesPerRow;
      /* swap B/R for existing texture */
      if (desc->format == VIRGL_FORMAT_B8G8R8X8_UNORM || desc->format == VIRGL_FORMAT_B8G8R8A8_UNORM) {
         descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
      }
      /* Regardless of what we want, we have to respect the heap's options */
      descriptor.resourceOptions = mtl_heap.resourceOptions;
      deviceAlignment = [mtl_device minimumLinearTextureAlignmentForPixelFormat:descriptor.pixelFormat];
      bytesPerRow = align(desc->stride, deviceAlignment);
      id<MTLBuffer> mtl_buffer = [mtl_heap newBufferWithLength:bytesPerRow * desc->height
                                                       options:mtl_heap.resourceOptions
                                                        offset:0];
      if (mtl_buffer) {
         *tex = [mtl_buffer newTextureWithDescriptor:descriptor
                                              offset:0
                                         bytesPerRow:bytesPerRow];
         [mtl_buffer release];
      }
      [descriptor release];
      return !!*tex;
   }

   return false;
}

void virgl_metal_release_texture(MTLTexture_id tex)
{
   id<MTLTexture> mtl_texture = (id<MTLTexture>)tex;

   [mtl_texture release];
}
