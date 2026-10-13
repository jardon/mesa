/* SPDX-License-Identifier: MIT */

#include "nvkmd_jensen.h"

#include "util/cache_ops.h"
#include "util/libdrm.h"
#include "util/os_misc.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <xf86drm.h>

static bool
drm_fd_is_jensen(int fd)
{
   drmVersionPtr ver = drmGetVersion(fd);
   if (ver == NULL)
      return false;

   const bool is_jensen = ver->name_len == 6 &&
                          memcmp(ver->name, "jensen", 6) == 0;
   drmFreeVersion(ver);

   return is_jensen;
}

static uint8_t
jensen_max_warps_per_mp(uint8_t sm)
{
   switch (sm) {
   case 75:
      return 32;
   case 86:
   case 87:
   case 89:
      return 48;
   default:
      return 64;
   }
}

static uint8_t
jensen_max_blocks_per_mp(uint8_t sm)
{
   switch (sm) {
   case 75:
   case 86:
   case 87:
      return 16;
   default:
      return 24;
   }
}

static void
jensen_init_shared_mem_sizes(struct nv_device_info *info)
{
   if (info->sm >= 80) {
      static const uint16_t ampere_shared_mem[10] =
         { 0, 8, 16, 32, 64, 100, 132, 164, 196, 228 };

      if (info->sm >= 120)
         info->sm_smem_size_count = 6;
      else if (info->sm >= 90)
         info->sm_smem_size_count = 10;
      else if (info->sm == 80 || info->sm == 87)
         info->sm_smem_size_count = 8;
      else
         info->sm_smem_size_count = 6;

      memcpy(info->sm_smem_sizes_kB, ampere_shared_mem,
             info->sm_smem_size_count * sizeof(uint16_t));
   } else {
      info->sm_smem_sizes_kB[0] = 32;
      info->sm_smem_sizes_kB[1] = 64;
      info->sm_smem_size_count = 2;
   }

   info->max_smem_per_wg_kB =
      info->sm_smem_sizes_kB[info->sm_smem_size_count - 1];
}

VkResult
nvkmd_jensen_try_create_pdev(struct _drmDevice *drm_device,
                             struct vk_object_base *log_obj,
                             enum nvk_debug debug_flags,
                             struct nvkmd_pdev **pdev_out)
{
   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)))
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   const char *path = drm_device->nodes[DRM_NODE_RENDER];

   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   if (!drm_fd_is_jensen(fd)) {
      close(fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct drm_jensen_device_info info;
   memset(&info, 0, sizeof(info));
   if (drmCommandWriteRead(fd, DRM_JENSEN_GET_DEVICE_INFO, &info, sizeof(info)) != 0) {
      close(fd);
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "DRM_JENSEN_GET_DEVICE_INFO failed on %s: %m", path);
   }

   /* Verify execution readiness: if hardware channels or pushbuffer submissions
    * fail (e.g. GSP is offline or hardware hung), reject device enumeration
    * so Vulkan falls back to the integrated GPU instead of rendering black windows. */
   struct drm_jensen_channel_alloc chan_alloc = { 0 };
   if (drmCommandWriteRead(fd, DRM_JENSEN_CHANNEL_ALLOC, &chan_alloc, sizeof(chan_alloc)) == 0) {
      struct drm_jensen_exec exec_test = { .channel = chan_alloc.channel };
      int exec_ret = drmCommandWriteRead(fd, DRM_JENSEN_EXEC, &exec_test, sizeof(exec_test));
      struct drm_jensen_channel_free chan_free = { .channel = chan_alloc.channel };
      drmCommandWriteRead(fd, DRM_JENSEN_CHANNEL_FREE, &chan_free, sizeof(chan_free));
      if (exec_ret != 0) {
         close(fd);
         return VK_ERROR_INCOMPATIBLE_DRIVER;
      }
   } else {
      close(fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct stat st;
   if (stat(path, &st) != 0) {
      close(fd);
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "stat() failed on %s: %m", path);
   }
   const dev_t render_dev = st.st_rdev;

   struct nvkmd_jensen_pdev *pdev = CALLOC_STRUCT(nvkmd_jensen_pdev);
   if (pdev == NULL) {
      close(fd);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pdev->base.ops = &nvkmd_jensen_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   struct nv_device_info *dev_info = &pdev->base.dev_info;
   dev_info->type = NV_DEVICE_TYPE_DIS;
   dev_info->device_id = info.device_id;
   dev_info->chipset = info.chipset;
   dev_info->sm = info.sm;
   dev_info->gpc_count = info.gpc_count;
   dev_info->tpc_count = info.tpc_count;
   dev_info->cls_copy = info.cls_copy;
   dev_info->cls_eng2d = info.cls_eng2d;
   dev_info->cls_eng3d = info.cls_eng3d;
   dev_info->cls_m2mf = info.cls_m2mf;
   dev_info->cls_compute = info.cls_compute;
   dev_info->cls_gpfifo = info.cls_gpfifo;
   dev_info->vram_size_B = info.vram_size_B;
   dev_info->bar_size_B = info.bar_size_B;
   snprintf(dev_info->device_name, sizeof(dev_info->device_name),
            "%s", info.device_name);
   snprintf(dev_info->chipset_name, sizeof(dev_info->chipset_name),
            "%04x", info.chipset);

   if (drm_device->bustype == DRM_BUS_PCI) {
      dev_info->pci.domain = drm_device->businfo.pci->domain;
      dev_info->pci.bus = drm_device->businfo.pci->bus;
      dev_info->pci.dev = drm_device->businfo.pci->dev;
      dev_info->pci.func = drm_device->businfo.pci->func;
      dev_info->pci.revision_id = drm_device->deviceinfo.pci->revision_id;
   }

   dev_info->mp_per_tpc = dev_info->chipset >= 0x140 ? 2 : 1;
   dev_info->max_warps_per_mp = jensen_max_warps_per_mp(dev_info->sm);
   dev_info->max_blocks_per_mp = jensen_max_blocks_per_mp(dev_info->sm);
   jensen_init_shared_mem_sizes(dev_info);

   dev_info->nc_atom_size_B = util_cache_granularity();

   uint64_t os_page_size;
   os_get_page_size(&os_page_size);
   assert(os_page_size <= UINT32_MAX);
   pdev->base.bind_align_B = os_page_size;

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_dma_buf = true,
      .has_get_vram_used = true,
      .has_alloc_tiled = true,
      .has_map_fixed = true,
      .has_overmap = false,
      .has_compression = false,
   };

   pdev->base.drm.render_dev = render_dev;

   if ((drm_device->available_nodes & (1 << DRM_NODE_PRIMARY)) &&
       stat(drm_device->nodes[DRM_NODE_PRIMARY], &st) == 0)
      pdev->base.drm.primary_dev = st.st_rdev;

   pdev->fd = fd;
   pdev->primary_fd = -1;

   pdev->syncobj_sync_type = vk_drm_syncobj_get_type(fd);
   pdev->sync_types[0] = &pdev->syncobj_sync_type;
   pdev->sync_types[1] = NULL;
   pdev->base.sync_types = pdev->sync_types;

   *pdev_out = &pdev->base;

   return VK_SUCCESS;
}

static void
nvkmd_jensen_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_jensen_pdev *pdev = nvkmd_jensen_pdev(_pdev);

   if (pdev->primary_fd >= 0)
      close(pdev->primary_fd);

   if (pdev->fd >= 0)
      close(pdev->fd);

   FREE(pdev);
}

static uint64_t
nvkmd_jensen_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_jensen_pdev *pdev = nvkmd_jensen_pdev(_pdev);
   struct drm_jensen_vram_usage args = { 0 };

   if (drmCommandWriteRead(pdev->fd, DRM_JENSEN_GET_VRAM_USED,
                           &args, sizeof(args)) != 0)
      return 0;

   return args.used_B;
}

static int
nvkmd_jensen_pdev_get_drm_primary_fd(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_jensen_pdev *pdev = nvkmd_jensen_pdev(_pdev);

   if (pdev->primary_fd >= 0)
      return pdev->primary_fd;

   if (pdev->base.drm.primary_dev == 0)
      return -1;

   drmDevicePtr drm_device = NULL;
   if (drmGetDeviceFromDevId(pdev->base.drm.primary_dev, 0, &drm_device) != 0)
      return -1;

   int fd = open(drm_device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
   drmFreeDevice(&drm_device);

   pdev->primary_fd = fd;

   return pdev->primary_fd;
}

const struct nvkmd_pdev_ops nvkmd_jensen_pdev_ops = {
   .destroy = nvkmd_jensen_pdev_destroy,
   .get_vram_used = nvkmd_jensen_pdev_get_vram_used,
   .get_drm_primary_fd = nvkmd_jensen_pdev_get_drm_primary_fd,
   .create_dev = nvkmd_jensen_create_dev,
};
