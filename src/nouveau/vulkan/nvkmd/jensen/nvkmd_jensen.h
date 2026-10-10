/* SPDX-License-Identifier: MIT */

#ifndef NVKMD_JENSEN_H
#define NVKMD_JENSEN_H 1

#include "nvkmd/nvkmd.h"
#include "vk_drm_syncobj.h"
#include "util/vma.h"

#include <stdint.h>
#include <sys/types.h>

#define DRM_JENSEN_GEM_MAP_OFFSET 0x02
#define DRM_JENSEN_GET_DEVICE_INFO 0x03
#define DRM_JENSEN_GEM_CREATE 0x04
#define DRM_JENSEN_VM_BIND 0x05

#define DRM_JENSEN_GEM_VRAM (1u << 0)
#define DRM_JENSEN_GEM_GTT (1u << 1)

#define DRM_JENSEN_VM_BIND_OP_BIND 0
#define DRM_JENSEN_VM_BIND_OP_UNBIND 1

struct drm_jensen_device_info {
   uint16_t vendor_id;
   uint16_t device_id;
   uint16_t chipset;
   uint16_t sm;
   uint16_t cls_copy;
   uint16_t cls_eng2d;
   uint16_t cls_eng3d;
   uint16_t cls_m2mf;
   uint16_t cls_compute;
   uint16_t cls_gpfifo;
   uint32_t gpc_count;
   uint32_t tpc_count;
   uint64_t vram_size_B;
   uint64_t bar_size_B;
   char device_name[64];
};

struct drm_jensen_gem_create {
   uint64_t size;
   uint32_t flags;
   uint32_t handle;
   uint64_t offset;
};

struct drm_jensen_gem_map_offset {
   uint32_t handle;
   uint32_t pad;
   uint64_t offset;
};

struct drm_jensen_vm_bind_op {
   uint32_t op;
   uint32_t handle;
   uint64_t addr;
   uint64_t bo_offset;
   uint64_t range;
   uint32_t flags;
   uint32_t pad;
};

struct nvkmd_jensen_pdev {
   struct nvkmd_pdev base;

   int fd;
   int primary_fd;

   struct vk_sync_type syncobj_sync_type;
   const struct vk_sync_type *sync_types[2];
};

NVKMD_DECL_SUBCLASS(pdev, jensen);

VkResult nvkmd_jensen_try_create_pdev(struct _drmDevice *drm_device,
                                      struct vk_object_base *log_obj,
                                      enum nvk_debug debug_flags,
                                      struct nvkmd_pdev **pdev_out);

#define NVKMD_JENSEN_HEAP_START ((uint64_t)0x1000)
#define NVKMD_JENSEN_HEAP_END ((uint64_t)(1ull << 38))
#define NVKMD_JENSEN_REPLAY_HEAP_START NVKMD_JENSEN_HEAP_END
#define NVKMD_JENSEN_REPLAY_HEAP_END ((uint64_t)(1ull << 39))

struct nvkmd_jensen_dev {
   struct nvkmd_dev base;

   int fd;

   simple_mtx_t heap_mutex;
   struct util_vma_heap heap;
   struct util_vma_heap replay_heap;
};

NVKMD_DECL_SUBCLASS(dev, jensen);

VkResult nvkmd_jensen_create_dev(struct nvkmd_pdev *pdev,
                                 struct vk_object_base *log_obj,
                                 struct nvkmd_dev **dev_out);

struct nvkmd_jensen_mem {
   struct nvkmd_mem base;

   uint32_t handle;
   uint64_t map_handle;
};

NVKMD_DECL_SUBCLASS(mem, jensen);

VkResult nvkmd_jensen_alloc_mem(struct nvkmd_dev *dev,
                                struct vk_object_base *log_obj,
                                uint64_t size_B, uint64_t align_B,
                                enum nvkmd_mem_flags flags,
                                struct nvkmd_mem **mem_out);

VkResult nvkmd_jensen_alloc_tiled_mem(struct nvkmd_dev *dev,
                                      struct vk_object_base *log_obj,
                                      uint64_t size_B, uint64_t align_B,
                                      uint8_t pte_kind, uint16_t tile_mode,
                                      enum nvkmd_mem_flags flags,
                                      struct nvkmd_mem **mem_out);

VkResult nvkmd_jensen_import_dma_buf(struct nvkmd_dev *dev,
                                     struct vk_object_base *log_obj,
                                     int fd, struct nvkmd_mem **mem_out);

struct nvkmd_jensen_va {
   struct nvkmd_va base;
};

NVKMD_DECL_SUBCLASS(va, jensen);

VkResult nvkmd_jensen_alloc_va(struct nvkmd_dev *dev,
                               struct vk_object_base *log_obj,
                               enum nvkmd_va_flags flags, uint8_t pte_kind,
                               uint64_t size_B, uint64_t align_B,
                               uint64_t fixed_addr, struct nvkmd_va **va_out);

struct nvkmd_jensen_ctx {
   struct nvkmd_ctx base;
};

NVKMD_DECL_SUBCLASS(ctx, jensen);

VkResult nvkmd_jensen_create_ctx(struct nvkmd_dev *dev,
                                 struct vk_object_base *log_obj,
                                 enum nvkmd_engines engines,
                                 struct nvkmd_ctx **ctx_out);

#endif /* NVKMD_JENSEN_H */
