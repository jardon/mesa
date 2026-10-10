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
   struct nvkmd_jensen_ctx *ctx = CALLOC_STRUCT(nvkmd_jensen_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_jensen_ctx_ops;
   ctx->base.dev = _dev;

   *ctx_out = &ctx->base;

   return VK_SUCCESS;
}

static void
nvkmd_jensen_ctx_destroy(struct nvkmd_ctx *ctx)
{
   FREE(nvkmd_jensen_ctx(ctx));
}

static VkResult
nvkmd_jensen_ctx_wait(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t wait_count,
                      const struct vk_sync_wait *waits)
{
   return VK_SUCCESS;
}

static VkResult
nvkmd_jensen_ctx_exec(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t exec_count,
                      const struct nvkmd_ctx_exec *execs)
{
   return vk_error(log_obj, VK_ERROR_UNKNOWN);
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
   return vk_error(log_obj, VK_ERROR_UNKNOWN);
}

static VkResult
nvkmd_jensen_ctx_flush(struct nvkmd_ctx *_ctx,
                       struct vk_object_base *log_obj)
{
   return VK_SUCCESS;
}

static VkResult
nvkmd_jensen_ctx_sync(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj)
{
   return vk_error(log_obj, VK_ERROR_UNKNOWN);
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
