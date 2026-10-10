/* SPDX-License-Identifier: MIT */

#include "nvkmd_jensen.h"

#include "util/bitscan.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <xf86drm.h>

static VkResult MUST_CHECK
alloc_heap_addr_locked(struct nvkmd_jensen_dev *dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_va_flags flags,
                       uint64_t size_B, uint64_t align_B,
                       uint64_t fixed_addr, uint64_t *addr_out)
{
   if (flags & NVKMD_VA_ALLOC_FIXED) {
      if (fixed_addr < NVKMD_JENSEN_REPLAY_HEAP_START ||
          fixed_addr >= NVKMD_JENSEN_REPLAY_HEAP_END) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Capture address 0x%" PRIx64 " not in the replay "
                          "heap address range [0x%" PRIx64 ", 0x%" PRIx64 ")",
                          fixed_addr, NVKMD_JENSEN_REPLAY_HEAP_START,
                          NVKMD_JENSEN_REPLAY_HEAP_END);
      }

      if (fixed_addr & (align_B - 1)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Unaligned capture address: 0x%" PRIx64, fixed_addr);
      }

      if (!util_vma_heap_alloc_addr(&dev->replay_heap, fixed_addr, size_B)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Replay address collision: 0x%" PRIx64, fixed_addr);
      }

      *addr_out = fixed_addr;
   } else if (flags & NVKMD_VA_REPLAY) {
      *addr_out = util_vma_heap_alloc(&dev->replay_heap, size_B, align_B);
      if (*addr_out == 0)
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address range");
   } else {
      *addr_out = util_vma_heap_alloc(&dev->heap, size_B, align_B);
      if (*addr_out == 0)
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address range");
   }

   return VK_SUCCESS;
}

static VkResult MUST_CHECK
alloc_heap_addr(struct nvkmd_jensen_dev *dev,
                struct vk_object_base *log_obj,
                enum nvkmd_va_flags flags,
                uint64_t size_B, uint64_t align_B,
                uint64_t fixed_addr, uint64_t *addr_out)
{
   simple_mtx_lock(&dev->heap_mutex);
   VkResult result = alloc_heap_addr_locked(dev, log_obj, flags,
                                            size_B, align_B,
                                            fixed_addr, addr_out);
   simple_mtx_unlock(&dev->heap_mutex);
   return result;
}

static void
free_heap_addr(struct nvkmd_jensen_dev *dev,
               enum nvkmd_va_flags flags,
               uint64_t addr, uint64_t size_B)
{
   simple_mtx_lock(&dev->heap_mutex);
   if (flags & (NVKMD_VA_REPLAY | NVKMD_VA_ALLOC_FIXED)) {
      util_vma_heap_free(&dev->replay_heap, addr, size_B);
   } else {
      util_vma_heap_free(&dev->heap, addr, size_B);
   }
   simple_mtx_unlock(&dev->heap_mutex);
}

static VkResult MUST_CHECK
jensen_vm_bind(struct nvkmd_jensen_dev *dev,
               struct vk_object_base *log_obj,
               struct drm_jensen_vm_bind_op *op)
{
   if (drmCommandWriteRead(dev->fd, DRM_JENSEN_VM_BIND, op, sizeof(*op)) != 0)
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN, "vm_bind failed: %m");

   return VK_SUCCESS;
}

VkResult
nvkmd_jensen_alloc_va(struct nvkmd_dev *_dev,
                      struct vk_object_base *log_obj,
                      enum nvkmd_va_flags flags, uint8_t pte_kind,
                      uint64_t size_B, uint64_t align_B,
                      uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_dev);

   struct nvkmd_jensen_va *va = CALLOC_STRUCT(nvkmd_jensen_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const uint32_t min_align_B = _dev->pdev->bind_align_B;
   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, min_align_B);
   size_B = align64(size_B, align_B);

   assert((fixed_addr == 0) == !(flags & NVKMD_VA_ALLOC_FIXED));

   VkResult result = alloc_heap_addr(dev, log_obj, flags, size_B, align_B,
                                     fixed_addr, &va->base.addr);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   va->base.ops = &nvkmd_jensen_va_ops;
   va->base.dev = &dev->base;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;

   *va_out = &va->base;

   return VK_SUCCESS;

fail_alloc:
   FREE(va);

   return result;
}

static void
nvkmd_jensen_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_va->dev);
   struct nvkmd_jensen_va *va = nvkmd_jensen_va(_va);

   struct drm_jensen_vm_bind_op bind = {
      .op = DRM_JENSEN_VM_BIND_OP_UNBIND,
      .addr = va->base.addr,
      .range = va->base.size_B,
   };
   drmCommandWriteRead(dev->fd, DRM_JENSEN_VM_BIND, &bind, sizeof(bind));

   free_heap_addr(dev, va->base.flags, va->base.addr, va->base.size_B);

   FREE(va);
}

static VkResult
nvkmd_jensen_va_bind_mem(struct nvkmd_va *_va,
                         struct vk_object_base *log_obj,
                         uint64_t va_offset_B,
                         struct nvkmd_mem *_mem,
                         uint64_t mem_offset_B,
                         uint64_t range_B)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_va->dev);
   struct nvkmd_jensen_va *va = nvkmd_jensen_va(_va);
   struct nvkmd_jensen_mem *mem = nvkmd_jensen_mem(_mem);

   assert(_mem->dev == _va->dev);

   struct drm_jensen_vm_bind_op bind = {
      .op = DRM_JENSEN_VM_BIND_OP_BIND,
      .handle = mem->handle,
      .addr = va->base.addr + va_offset_B,
      .range = range_B,
      .bo_offset = mem_offset_B,
      .flags = va->base.pte_kind,
      .tile_mode = mem->tile_mode,
   };
   return jensen_vm_bind(dev, log_obj, &bind);
}

static VkResult
nvkmd_jensen_va_unbind(struct nvkmd_va *_va,
                       struct vk_object_base *log_obj,
                       uint64_t va_offset_B,
                       uint64_t range_B)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_va->dev);
   struct nvkmd_jensen_va *va = nvkmd_jensen_va(_va);

   struct drm_jensen_vm_bind_op bind = {
      .op = DRM_JENSEN_VM_BIND_OP_UNBIND,
      .addr = va->base.addr + va_offset_B,
      .range = range_B,
   };
   return jensen_vm_bind(dev, log_obj, &bind);
}

const struct nvkmd_va_ops nvkmd_jensen_va_ops = {
   .free = nvkmd_jensen_va_free,
   .bind_mem = nvkmd_jensen_va_bind_mem,
   .unbind = nvkmd_jensen_va_unbind,
};
