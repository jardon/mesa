/* SPDX-License-Identifier: MIT */

#include "nvkmd_jensen.h"

#include "util/u_memory.h"
#include "vk_log.h"

VkResult
nvkmd_jensen_create_dev(struct nvkmd_pdev *_pdev,
                        struct vk_object_base *log_obj,
                        struct nvkmd_dev **dev_out)
{
   struct nvkmd_jensen_pdev *pdev = nvkmd_jensen_pdev(_pdev);

   struct nvkmd_jensen_dev *dev = CALLOC_STRUCT(nvkmd_jensen_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   dev->base.ops = &nvkmd_jensen_dev_ops;
   dev->base.pdev = &pdev->base;
   dev->base.va_start = NVKMD_JENSEN_HEAP_START;
   dev->base.va_end = NVKMD_JENSEN_REPLAY_HEAP_END;

   dev->fd = pdev->fd;

   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);

   simple_mtx_init(&dev->heap_mutex, mtx_plain);

   STATIC_ASSERT(NVKMD_JENSEN_HEAP_START < NVKMD_JENSEN_HEAP_END);
   util_vma_heap_init(&dev->heap, NVKMD_JENSEN_HEAP_START,
                      NVKMD_JENSEN_HEAP_END - NVKMD_JENSEN_HEAP_START);

   STATIC_ASSERT(NVKMD_JENSEN_REPLAY_HEAP_START < NVKMD_JENSEN_REPLAY_HEAP_END);
   util_vma_heap_init(&dev->replay_heap, NVKMD_JENSEN_REPLAY_HEAP_START,
                      NVKMD_JENSEN_REPLAY_HEAP_END -
                      NVKMD_JENSEN_REPLAY_HEAP_START);

   *dev_out = &dev->base;

   return VK_SUCCESS;
}

static void
nvkmd_jensen_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_dev);

   util_vma_heap_finish(&dev->heap);
   util_vma_heap_finish(&dev->replay_heap);

   FREE(dev);
}

static uint64_t
nvkmd_jensen_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   return 0;
}

static int
nvkmd_jensen_dev_get_drm_fd(struct nvkmd_dev *_dev)
{
   struct nvkmd_jensen_dev *dev = nvkmd_jensen_dev(_dev);

   return dev->fd;
}

const struct nvkmd_dev_ops nvkmd_jensen_dev_ops = {
   .destroy = nvkmd_jensen_dev_destroy,
   .get_gpu_timestamp = nvkmd_jensen_dev_get_gpu_timestamp,
   .get_drm_fd = nvkmd_jensen_dev_get_drm_fd,
   .alloc_mem = nvkmd_jensen_alloc_mem,
   .alloc_tiled_mem = nvkmd_jensen_alloc_tiled_mem,
   .import_dma_buf = nvkmd_jensen_import_dma_buf,
   .alloc_va = nvkmd_jensen_alloc_va,
   .create_ctx = nvkmd_jensen_create_ctx,
};
