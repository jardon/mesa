/* SPDX-License-Identifier: MIT */

#include "nvkmd_jensen.h"

#include "util/u_memory.h"
#include "vk_log.h"

#include <xf86drm.h>

VkResult
nvkmd_jensen_create_ctx(struct nvkmd_dev *_dev,
                        struct vk_object_base *log_obj,
                        enum nvkmd_engines engines,
                        struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_dev);
   struct nvkmd_jensen_ctx *ctx = CALLOC_STRUCT(nvkmd_jensen_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_jensen_ctx_ops;
   ctx->base.dev = _dev;

   if (engines == NVKMD_ENGINE_BIND) {
      ctx->is_bind = true;
      *ctx_out = &ctx->base;
      return VK_SUCCESS;
   }

   struct drm_jensen_channel_alloc chan = { 0 };
   if (drmCommandWriteRead(dev->fd, DRM_JENSEN_CHANNEL_ALLOC,
                           &chan, sizeof(chan)) != 0) {
      VkResult result = vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                                  "DRM_JENSEN_CHANNEL_ALLOC failed: %m");
      FREE(ctx);
      return result;
   }
   ctx->channel = chan.channel;

   if (drmSyncobjCreate(dev->fd, 0, &ctx->syncobj) != 0) {
      VkResult result = vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                                  "drmSyncobjCreate failed: %m");
      struct drm_jensen_channel_free args = { .channel = ctx->channel };
      drmCommandWriteRead(dev->fd, DRM_JENSEN_CHANNEL_FREE,
                          &args, sizeof(args));
      FREE(ctx);
      return result;
   }

   ctx->req = (struct drm_jensen_exec) {
      .channel = ctx->channel,
      .push_ptr = (uintptr_t)ctx->req_push,
      .wait_ptr = (uintptr_t)ctx->req_wait,
      .sig_ptr = (uintptr_t)ctx->req_sig,
   };

   *ctx_out = &ctx->base;

   return VK_SUCCESS;
}

static void
nvkmd_jensen_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_jensen_ctx *ctx = nvkmd_jensen_ctx(_ctx);
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_ctx->dev);

   if (!ctx->is_bind) {
      if (ctx->syncobj != 0)
         drmSyncobjDestroy(dev->fd, ctx->syncobj);

      struct drm_jensen_channel_free args = { .channel = ctx->channel };
      drmCommandWriteRead(dev->fd, DRM_JENSEN_CHANNEL_FREE,
                          &args, sizeof(args));
   }

   FREE(ctx);
}

static VkResult
nvkmd_jensen_ctx_flush(struct nvkmd_ctx *_ctx,
                       struct vk_object_base *log_obj);

static VkResult
nvkmd_jensen_ctx_add_sync(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t *req_count,
                          struct drm_jensen_sync *req_syncs,
                          struct vk_sync *sync,
                          uint64_t sync_value)
{
   struct nvkmd_jensen_ctx *ctx = nvkmd_jensen_ctx(_ctx);

   assert(!ctx->is_bind);

   if (unlikely(*req_count >= NVKMD_JENSEN_MAX_SYNCS)) {
      VkResult result = nvkmd_jensen_ctx_flush(_ctx, log_obj);
      if (result != VK_SUCCESS)
         return result;
   }

   struct vk_drm_syncobj *syncobj = vk_sync_as_drm_syncobj(sync);
   assert(syncobj != NULL);

   req_syncs[(*req_count)++] = (struct drm_jensen_sync) {
      .flags = sync_value ? DRM_JENSEN_SYNC_TIMELINE_SYNCOBJ
                          : DRM_JENSEN_SYNC_SYNCOBJ,
      .handle = syncobj->syncobj,
      .timeline_value = sync_value,
   };

   return VK_SUCCESS;
}

static VkResult
nvkmd_jensen_ctx_wait(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t wait_count,
                      const struct vk_sync_wait *waits)
{
   struct nvkmd_jensen_ctx *ctx = nvkmd_jensen_ctx(_ctx);

   if (ctx->is_bind)
      return VK_SUCCESS;

   for (uint32_t i = 0; i < wait_count; i++) {
      VkResult result =
         nvkmd_jensen_ctx_add_sync(_ctx, log_obj, &ctx->req.wait_count,
                                   ctx->req_wait, waits[i].sync,
                                   waits[i].wait_value);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_jensen_ctx_exec(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t exec_count,
                      const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_jensen_ctx *ctx = nvkmd_jensen_ctx(_ctx);

   for (uint32_t i = 0; i < exec_count; i++) {
      if (unlikely(ctx->req.push_count >= NVKMD_JENSEN_MAX_PUSHES)) {
         VkResult result = nvkmd_jensen_ctx_flush(_ctx, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }

      assert((execs[i].addr % 4) == 0);
      assert((execs[i].size_B % 4) == 0);
      assert(execs[i].size_B < (1u << 23));

      ctx->req_push[ctx->req.push_count++] = (struct drm_jensen_exec_push) {
         .va = execs[i].addr,
         .va_len = execs[i].size_B,
         .flags = execs[i].no_prefetch ? DRM_JENSEN_EXEC_PUSH_NO_PREFETCH : 0,
      };
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_jensen_ctx_bind(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t bind_count,
                      const struct nvkmd_ctx_bind *binds)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_ctx->dev);

   for (uint32_t i = 0; i < bind_count; i++) {
      const struct nvkmd_ctx_bind *bind = &binds[i];
      struct nvkmd_jensen_va *va = nvkmd_jensen_va(bind->va);
      struct drm_jensen_vm_bind_op op = {
         .addr = va->base.addr + bind->va_offset_B,
         .range = bind->range_B,
      };

      if (bind->op == NVKMD_BIND_OP_BIND) {
         struct nvkmd_jensen_mem *mem = nvkmd_jensen_mem(bind->mem);
         op.op = DRM_JENSEN_VM_BIND_OP_BIND;
         op.handle = mem->handle;
         op.bo_offset = bind->mem_offset_B;
         op.flags = va->base.pte_kind;
      } else {
         op.op = DRM_JENSEN_VM_BIND_OP_UNBIND;
      }

      if (drmCommandWriteRead(dev->fd, DRM_JENSEN_VM_BIND,
                              &op, sizeof(op)) != 0)
         return vk_errorf(log_obj, VK_ERROR_UNKNOWN, "vm_bind failed: %m");
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_jensen_ctx_signal(struct nvkmd_ctx *_ctx,
                        struct vk_object_base *log_obj,
                        uint32_t signal_count,
                        const struct vk_sync_signal *signals)
{
   struct nvkmd_jensen_ctx *ctx = nvkmd_jensen_ctx(_ctx);

   if (ctx->is_bind)
      return VK_SUCCESS;

   for (uint32_t i = 0; i < signal_count; i++) {
      VkResult result =
         nvkmd_jensen_ctx_add_sync(_ctx, log_obj, &ctx->req.sig_count,
                                   ctx->req_sig, signals[i].sync,
                                   signals[i].signal_value);
      if (result != VK_SUCCESS)
         return result;
   }

   return nvkmd_jensen_ctx_flush(_ctx, log_obj);
}

static VkResult
nvkmd_jensen_ctx_flush(struct nvkmd_ctx *_ctx,
                       struct vk_object_base *log_obj)
{
   struct nvkmd_jensen_ctx *ctx = nvkmd_jensen_ctx(_ctx);
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_ctx->dev);

   if (ctx->is_bind)
      return VK_SUCCESS;

   if (ctx->req.push_count == 0 && ctx->req.wait_count == 0 &&
       ctx->req.sig_count == 0)
      return VK_SUCCESS;

   if (drmCommandWriteRead(dev->fd, DRM_JENSEN_EXEC,
                           &ctx->req, sizeof(ctx->req)) != 0)
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                       "DRM_JENSEN_EXEC failed: %m");

   ctx->req.push_count = 0;
   ctx->req.wait_count = 0;
   ctx->req.sig_count = 0;

   return VK_SUCCESS;
}

static VkResult
nvkmd_jensen_ctx_sync(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj)
{
   struct nvkmd_jensen_ctx *ctx = nvkmd_jensen_ctx(_ctx);
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_ctx->dev);

   if (ctx->is_bind)
      return VK_SUCCESS;

   if (unlikely(ctx->req.sig_count >= NVKMD_JENSEN_MAX_SYNCS)) {
      VkResult result = nvkmd_jensen_ctx_flush(_ctx, log_obj);
      if (result != VK_SUCCESS)
         return result;
   }

   ctx->req_sig[ctx->req.sig_count++] = (struct drm_jensen_sync) {
      .flags = DRM_JENSEN_SYNC_SYNCOBJ,
      .handle = ctx->syncobj,
   };

   VkResult result = nvkmd_jensen_ctx_flush(_ctx, log_obj);
   if (result != VK_SUCCESS)
      return result;

   if (drmSyncobjWait(dev->fd, &ctx->syncobj, 1, INT64_MAX, 0, NULL) != 0)
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN, "drmSyncobjWait failed: %m");

   struct drm_jensen_exec empty = { .channel = ctx->channel };
   if (drmCommandWriteRead(dev->fd, DRM_JENSEN_EXEC,
                           &empty, sizeof(empty)) != 0)
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                       "post-submit DRM_JENSEN_EXEC failed: %m");

   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_jensen_ctx_ops = {
   .destroy = nvkmd_jensen_ctx_destroy,
   .wait = nvkmd_jensen_ctx_wait,
   .exec = nvkmd_jensen_ctx_exec,
   .bind = nvkmd_jensen_ctx_bind,
   .signal = nvkmd_jensen_ctx_signal,
   .flush = nvkmd_jensen_ctx_flush,
   .sync = nvkmd_jensen_ctx_sync,
};
