# MESA_WSI_DMABUF_CHECK=N: every Nth presented dmabuf frame, mmap the LINEAR buffer and log
# its centre pixel and how much of it is non-zero, so a blank window can be pinned on the
# client or the compositor. Debug only; it waits a little for the GPU before reading.
import sys
p = sys.argv[1] + "/src/vulkan/wsi/wsi_common_wayland.c"; s = open(p).read()
if "MESA_WSI_DMABUF_CHECK" not in s:
    old = "   VkResult ret = wsi_wl_swapchain_update_colorspace(chain);"
    new = '''   if (chain->buffer_type == WSI_WL_BUFFER_NATIVE) {
      static int check_every = -1, frame;
      if (check_every < 0) {
         const char *e = getenv("MESA_WSI_DMABUF_CHECK");
         check_every = e ? atoi(e) : 0;
      }
      struct wsi_wl_image *ci = &chain->images[image_index];
      if (check_every > 0 && frame++ % check_every == 0 && ci->base.dma_buf_fd >= 0) {
         size_t sz = (size_t)ci->base.row_pitches[0] * chain->extent.height;
         usleep(30000);
         uint8_t *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, ci->base.dma_buf_fd, ci->base.offsets[0]);
         if (m != MAP_FAILED) {
            size_t nz = 0, n = 0;
            for (size_t o = 0; o < sz; o += 4093, n++) nz += m[o] != 0;
            uint32_t c = *(uint32_t *)(m + (size_t)ci->base.row_pitches[0] * (chain->extent.height / 2) + (chain->extent.width / 2) * 4);
            fprintf(stderr, "WSI-CHECK frame %d %ux%u pitch %u mod 0x%llx centre 0x%08x nonzero %zu%%\\n", frame - 1,
                    chain->extent.width, chain->extent.height, ci->base.row_pitches[0],
                    (unsigned long long)ci->base.drm_modifier, c, n ? nz * 100 / n : 0);
            munmap(m, sz);
         } else {
            fprintf(stderr, "WSI-CHECK mmap failed: %s\\n", strerror(errno));
         }
      }
   }

   VkResult ret = wsi_wl_swapchain_update_colorspace(chain);'''
    assert old in s; s = s.replace(old, new, 1)
    if "#include <sys/mman.h>" not in s: s = s.replace("#include <wayland-client.h>", "#include <wayland-client.h>\n#include <sys/mman.h>", 1)
    open(p, "w").write(s)
print("patched dmabuf check")
