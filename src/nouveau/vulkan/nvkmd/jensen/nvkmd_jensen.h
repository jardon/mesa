/* SPDX-License-Identifier: MIT */

#ifndef NVKMD_JENSEN_H
#define NVKMD_JENSEN_H 1

#include "nvkmd/nvkmd.h"
#include "vk_drm_syncobj.h"

#include <stdint.h>
#include <sys/types.h>

#define DRM_JENSEN_GET_DEVICE_INFO 0x03

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

VkResult nvkmd_jensen_create_dev(struct nvkmd_pdev *pdev,
                                 struct vk_object_base *log_obj,
                                 struct nvkmd_dev **dev_out);

#endif /* NVKMD_JENSEN_H */
