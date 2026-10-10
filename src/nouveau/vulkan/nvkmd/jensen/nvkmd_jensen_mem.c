/* SPDX-License-Identifier: MIT */

#include "nvkmd_jensen.h"

#include "util/u_memory.h"
#include "vk_log.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>

static void
jensen_gem_close(struct nvkmd_jensen_dev *dev, uint32_t handle)
{
   if (handle != 0)
      drmCloseBufferHandle(dev->fd, handle);
}

static VkResult
create_mem_or_close_gem(struct nvkmd_jensen_dev *dev,
                        struct vk_object_base *log_obj,
                        enum nvkmd_mem_flags mem_flags,
                        uint32_t handle, uint64_t map_handle, uint64_t size_B,
                        uint8_t pte_kind, uint16_t tile_mode,
                        enum nvkmd_va_flags va_flags,
                        uint64_t va_align_B,
                        struct nvkmd_mem **mem_out)
{
   VkResult result;

   struct nvkmd_jensen_mem *mem = CALLOC_STRUCT(nvkmd_jensen_mem);
   if (mem == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_gem;
   }

   va_align_B = MAX2(dev->base.pdev->bind_align_B, va_align_B);
   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_jensen_mem_ops,
                  mem_flags, size_B, va_align_B);
   mem->handle = handle;
   mem->map_handle = map_handle;
   mem->pte_kind = pte_kind;
   mem->tile_mode = tile_mode;

   result = nvkmd_dev_alloc_va(&dev->base, log_obj,
                               va_flags, pte_kind,
                               size_B, va_align_B,
                               0 /* fixed_addr */,
                               &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_mem;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0 /* va_offset_B */,
                              &mem->base, 0 /* mem_offset_B */, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;

   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_mem:
   FREE(mem);
fail_gem:
   jensen_gem_close(dev, handle);

   return result;
}

VkResult
nvkmd_jensen_alloc_mem(struct nvkmd_dev *dev,
                       struct vk_object_base *log_obj,
                       uint64_t size_B, uint64_t align_B,
                       enum nvkmd_mem_flags flags,
                       struct nvkmd_mem **mem_out)
{
   return nvkmd_jensen_alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                       0 /* pte_kind */, 0 /* tile_mode */,
                                       flags, mem_out);
}

VkResult
nvkmd_jensen_alloc_tiled_mem(struct nvkmd_dev *_dev,
                             struct vk_object_base *log_obj,
                             uint64_t size_B, uint64_t align_B,
                             uint8_t pte_kind, uint16_t tile_mode,
                             enum nvkmd_mem_flags flags,
                             struct nvkmd_mem **mem_out)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_dev);
   const struct nv_device_info *dev_info = &dev->base.pdev->dev_info;

   assert(util_bitcount(flags & (NVKMD_MEM_LOCAL |
                                 NVKMD_MEM_GART |
                                 NVKMD_MEM_VRAM)) == 1);
   uint32_t gem_flags = 0;
   if (flags & NVKMD_MEM_LOCAL) {
      gem_flags |= DRM_JENSEN_GEM_GTT;
      if (dev_info->vram_size_B > 0)
         gem_flags |= DRM_JENSEN_GEM_VRAM;
   } else if (flags & NVKMD_MEM_GART) {
      gem_flags |= DRM_JENSEN_GEM_GTT;
   } else if (flags & NVKMD_MEM_VRAM) {
      gem_flags |= DRM_JENSEN_GEM_VRAM;
   }

   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, dev->base.pdev->bind_align_B);
   size_B = align64(size_B, align_B);

   struct drm_jensen_gem_create req = {
      .size = size_B,
      .flags = gem_flags,
      .pte_kind = pte_kind,
      .tile_mode = tile_mode,
   };
   if (drmCommandWriteRead(dev->fd, DRM_JENSEN_GEM_CREATE,
                           &req, sizeof(req)) != 0)
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "DRM_JENSEN_GEM_CREATE failed: %m");

   enum nvkmd_va_flags va_flags = 0;
   if (gem_flags == DRM_JENSEN_GEM_GTT)
      va_flags |= NVKMD_VA_GART;

   return create_mem_or_close_gem(dev, log_obj, flags, req.handle, req.offset,
                                  size_B, pte_kind, tile_mode,
                                  va_flags, align_B, mem_out);
}

VkResult
nvkmd_jensen_import_dma_buf(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            int fd, struct nvkmd_mem **mem_out)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_dev);
   uint32_t handle = 0;
   int err;

   err = drmPrimeFDToHandle(dev->fd, fd, &handle);
   if (err != 0)
      return vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE, "%m");

   uint64_t size_B = lseek(fd, 0, SEEK_END);
   if (size_B == 0) {
      vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                "Failed to query dma-buf size: %m");
      goto fail_size;
   }

   struct drm_jensen_gem_map_offset off = {
      .handle = handle,
   };
   if (drmCommandWriteRead(dev->fd, DRM_JENSEN_GEM_MAP_OFFSET,
                           &off, sizeof(off)) != 0) {
      vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                "Failed to map imported dma-buf: %m");
      goto fail_size;
   }

   enum nvkmd_mem_flags flags = NVKMD_MEM_SHARED | NVKMD_MEM_LOCAL |
                                NVKMD_MEM_CAN_MAP | NVKMD_MEM_COHERENT;

   return create_mem_or_close_gem(dev, log_obj, flags, handle, off.offset,
                                  size_B, 0 /* pte_kind */, 0 /* tile_mode */,
                                  0 /* va_flags */,
                                  0 /* va_align_B */, mem_out);

fail_size:
   jensen_gem_close(dev, handle);
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static void
nvkmd_jensen_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_jensen_mem *mem = nvkmd_jensen_mem(_mem);
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_mem->dev);

   nvkmd_va_free(mem->base.va);
   jensen_gem_close(dev, mem->handle);
   FREE(mem);
}

static VkResult
nvkmd_jensen_mem_map(struct nvkmd_mem *_mem,
                     struct vk_object_base *log_obj,
                     enum nvkmd_mem_map_flags map_flags,
                     void *fixed_addr,
                     void **map_out)
{
   struct nvkmd_jensen_mem *mem = nvkmd_jensen_mem(_mem);
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_mem->dev);

   int prot = 0;
   if (map_flags & NVKMD_MEM_MAP_RD)
      prot |= PROT_READ;
   if (map_flags & NVKMD_MEM_MAP_WR)
      prot |= PROT_WRITE;

   int flags = MAP_SHARED;
   if (map_flags & NVKMD_MEM_MAP_FIXED)
      flags |= MAP_FIXED;

   void *map = mmap(fixed_addr, mem->base.size_B, prot, flags,
                    dev->fd, mem->map_handle);
   if (map == MAP_FAILED)
      return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);

   *map_out = map;

   return VK_SUCCESS;
}

static void
nvkmd_jensen_mem_unmap(struct nvkmd_mem *_mem,
                       enum nvkmd_mem_map_flags flags,
                       void *map)
{
   struct nvkmd_jensen_mem *mem = nvkmd_jensen_mem(_mem);

   munmap(map, mem->base.size_B);
}

static VkResult
nvkmd_jensen_mem_overmap(struct nvkmd_mem *_mem,
                         struct vk_object_base *log_obj,
                         enum nvkmd_mem_map_flags flags,
                         void *map)
{
   struct nvkmd_jensen_mem *mem = nvkmd_jensen_mem(_mem);

   void *new_map = mmap(map, mem->base.size_B, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
   if (new_map == MAP_FAILED) {
      return vk_errorf(log_obj, VK_ERROR_MEMORY_MAP_FAILED,
                       "Failed to map over original mapping");
   }

   assert(new_map == map);

   return VK_SUCCESS;
}

static VkResult
nvkmd_jensen_mem_export_dma_buf(struct nvkmd_mem *_mem,
                                struct vk_object_base *log_obj,
                                int *fd_out)
{
   struct nvkmd_jensen_mem *mem = nvkmd_jensen_mem(_mem);
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_mem->dev);

   int err = drmPrimeHandleToFD(dev->fd, mem->handle, DRM_CLOEXEC, fd_out);
   if (err != 0)
      return vk_errorf(log_obj, VK_ERROR_TOO_MANY_OBJECTS,
                       "Failed to export dma-buf: %m");

   return VK_SUCCESS;
}

static uint32_t
nvkmd_jensen_mem_log_handle(struct nvkmd_mem *_mem)
{
   return nvkmd_jensen_mem(_mem)->handle;
}

const struct nvkmd_mem_ops nvkmd_jensen_mem_ops = {
   .free = nvkmd_jensen_mem_free,
   .map = nvkmd_jensen_mem_map,
   .unmap = nvkmd_jensen_mem_unmap,
   .overmap = nvkmd_jensen_mem_overmap,
   .export_dma_buf = nvkmd_jensen_mem_export_dma_buf,
   .log_handle = nvkmd_jensen_mem_log_handle,
};
