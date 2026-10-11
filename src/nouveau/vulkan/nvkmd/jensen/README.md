# nvkmd_jensen — NVK kernel-mode driver backend for the Jensen GPU driver

This is Mesa's Vulkan (NVK) backend for the clean-room **Jensen** kernel driver
(`drivers/gpu/drm/jensen` in the jenson repository). It speaks to the driver
through private `DRM_IOCTL_JENSEN_*` commands on `/dev/dri/renderD*`:

- `nvkmd_jensen_pdev.c` — physical-device discovery (`DRM_JENSEN_GET_DEVICE_INFO`)
- `nvkmd_jensen_mem.c` — VRAM / GTT allocations (`DRM_JENSEN_GEM_CREATE`, tiled
  allocations with `pte_kind` / `tile_mode`)
- `nvkmd_jensen_va.c` — GPU virtual address space binding (`DRM_JENSEN_VM_BIND`)
- `nvkmd_jensen_ctx.c` — hardware channels, GPFIFO submission, syncobject
  fences (`DRM_JENSEN_CHANNEL_ALLOC/FREE`, `DRM_JENSEN_EXEC`)
- `nvkmd_jensen_dev.c` — per-device queries (`DRM_JENSEN_GET_TIMESTAMP`,
  `DRM_JENSEN_GET_VRAM_USED`)

## Phase 4: end-to-end display verification

Phase 4 (silicon display / presentation) is driven from the kernel side — see
`TESTING.md` in the jenson repo for the full runbook
(`tools/phase4_verify.sh`). The Mesa-side verification steps are:

```bash
# 1. Build + load the matching kernel module on the host
KSRC=/usr/src/linux-headers-$(uname -r) make -C $KSRC M=<jenson>/kernel modules
sudo modprobe jensen modeset=1 fbdev=1

# 2. Validate render node permissions (render group)
ls -l /dev/dri/renderD*
sudo usermod -aG render,video $USER

# 3. End-to-end Vulkan presentation (60 FPS)
DRI_PRIME=1 vkcube
```

The frame path exercised by `vkcube` is: device discovery
(`nvkmd_jensen_pdev`) → VRAM / page-table binding (`nvkmd_jensen_mem` / `va`)
→ GPFIFO submission and fence retirement (`nvkmd_jensen_ctx`) → presentation
via kernel DRM KMS with VBLANK-synchronized page flips and PRIME export for
Optimus/Hybrid setups.