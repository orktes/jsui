// GBM allocation the way Chromium's Wayland GPU process does it: open the render node, create
// a bo (ABGR8888, scanout|rendering), export its dma-buf. Usage: gbmtest [/dev/dri/renderD128]
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <drm_fourcc.h>
int main(int argc, char **argv) {
  const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
  int fd = open(node, O_RDWR | O_CLOEXEC); if (fd < 0) { perror(node); return 1; }
  struct gbm_device *g = gbm_create_device(fd); if (!g) { printf("gbm_create_device failed\n"); return 1; }
  printf("backend %s\n", gbm_device_get_backend_name(g));
  uint32_t fmts[] = { GBM_FORMAT_ABGR8888, GBM_FORMAT_ARGB8888, GBM_FORMAT_XRGB8888 };
  for (int i = 0; i < 3; i++) {
    printf("format %.4s supported(scanout|render)=%d\n", (char *)&fmts[i],
           gbm_device_is_format_supported(g, fmts[i], GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING));
    struct gbm_bo *bo = gbm_bo_create(g, 1920, 1080, fmts[i], GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!bo) { printf("  gbm_bo_create failed\n"); continue; }
    int bfd = gbm_bo_get_fd(bo); char p[64], l[128] = { 0 }; snprintf(p, sizeof p, "/proc/self/fd/%d", bfd);
    if (readlink(p, l, sizeof l - 1) < 0) l[0] = 0;
    printf("  bo stride %u modifier 0x%llx planes %d fd %d -> %s\n", gbm_bo_get_stride(bo),
           (unsigned long long)gbm_bo_get_modifier(bo), gbm_bo_get_plane_count(bo), bfd, l);
    uint64_t lin = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo *bl = gbm_bo_create_with_modifiers2(g, 2000, 1130, fmts[i], &lin, 1, GBM_BO_USE_RENDERING);
    printf("  LINEAR 2000x1130: %s stride %u\n", bl ? "ok" : "failed", bl ? gbm_bo_get_stride(bl) : 0);
  }
  return 0;
}
