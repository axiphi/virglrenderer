/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_render_pass.h"

#include "vkr_render_pass_gen.h"

static void
vkr_dispatch_vkCreateRenderPass(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCreateRenderPass *args)
{
   vkr_render_pass_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateRenderPass2(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCreateRenderPass2 *args)
{
   vkr_render_pass_2_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyRenderPass(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkDestroyRenderPass *args)
{
   vkr_render_pass_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetRenderAreaGranularity(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkGetRenderAreaGranularity *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRenderAreaGranularity_args_handle(args);
   vk->GetRenderAreaGranularity(args->device, args->renderPass, args->pGranularity);
}

static void
vkr_dispatch_vkGetRenderingAreaGranularity(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRenderingAreaGranularity *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRenderingAreaGranularity_args_handle(args);
   vk->GetRenderingAreaGranularity(args->device, args->pRenderingAreaInfo,
                                   args->pGranularity);
}

static void
vkr_dispatch_vkCreateFramebuffer(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCreateFramebuffer *args)
{
   vkr_framebuffer_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyFramebuffer(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkDestroyFramebuffer *args)
{
   vkr_framebuffer_destroy_and_remove(dispatch->data, args);
}

void
vkr_context_init_render_pass_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateRenderPass = vkr_dispatch_vkCreateRenderPass;
   dispatch->dispatch_vkCreateRenderPass2 = vkr_dispatch_vkCreateRenderPass2;
   dispatch->dispatch_vkDestroyRenderPass = vkr_dispatch_vkDestroyRenderPass;
   dispatch->dispatch_vkGetRenderAreaGranularity =
      vkr_dispatch_vkGetRenderAreaGranularity;
   dispatch->dispatch_vkGetRenderingAreaGranularity =
      vkr_dispatch_vkGetRenderingAreaGranularity;
}

void
vkr_context_init_framebuffer_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateFramebuffer = vkr_dispatch_vkCreateFramebuffer;
   dispatch->dispatch_vkDestroyFramebuffer = vkr_dispatch_vkDestroyFramebuffer;
}
