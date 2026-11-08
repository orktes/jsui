// Turnip dmabuf facts for the compositor: supported DRM modifiers per format,
// and whether an exported image's memory is a real dma-buf fd.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <drm_fourcc.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s -> %d\n", #x, r_); exit(1); } } while (0)
static const char *modname(uint64_t m) {
  if (m == DRM_FORMAT_MOD_LINEAR) return "LINEAR";
  if (m == DRM_FORMAT_MOD_QCOM_COMPRESSED) return "QCOM_COMPRESSED(UBWC)";
  if (m == DRM_FORMAT_MOD_QCOM_TILED3) return "QCOM_TILED3";
  if (m == DRM_FORMAT_MOD_QCOM_TILED2) return "QCOM_TILED2";
  return "?";
}
int main(void) {
  VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
  VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t n = 1; VkPhysicalDevice pd; CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
  struct { VkFormat vk; const char *drm; } fmts[] = {
    { VK_FORMAT_B8G8R8A8_UNORM, "ARGB8888/XRGB8888 (VK B8G8R8A8)" },
    { VK_FORMAT_R8G8B8A8_UNORM, "ABGR8888/XBGR8888 (VK R8G8B8A8)" },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "ABGR2101010" },
    { VK_FORMAT_R16G16B16A16_SFLOAT, "ABGR16161616F" },
    { VK_FORMAT_R5G6B5_UNORM_PACK16, "RGB565" } };
  for (unsigned f = 0; f < sizeof fmts / sizeof *fmts; f++) {
    VkDrmFormatModifierPropertiesEXT mp[16];
    VkDrmFormatModifierPropertiesListEXT ml = { VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT, .drmFormatModifierCount = 16, .pDrmFormatModifierProperties = mp };
    VkFormatProperties2 fp = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml };
    vkGetPhysicalDeviceFormatProperties2(pd, fmts[f].vk, &fp);
    printf("%s:\n", fmts[f].drm);
    for (uint32_t i = 0; i < ml.drmFormatModifierCount; i++)
      printf("   modifier 0x%016llx %-22s planes=%u features=0x%x%s\n", (unsigned long long)mp[i].drmFormatModifier,
             modname(mp[i].drmFormatModifier), mp[i].drmFormatModifierPlaneCount, mp[i].drmFormatModifierTilingFeatures,
             (mp[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? " (render target)" : "");
  }
  float prio = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
  const char *ext[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier" };
  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 3, .ppEnabledExtensionNames = ext };
  VkDevice dev; CK(vkCreateDevice(pd, &dci, NULL, &dev));
  uint64_t mods[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_QCOM_COMPRESSED };
  for (int k = 0; k < 2; k++) {
    VkImageDrmFormatModifierListCreateInfoEXT ml = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, .drmFormatModifierCount = 1, .pDrmFormatModifiers = &mods[k] };
    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &ml, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { 1920, 1200, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    VkImage img; VkResult r = vkCreateImage(dev, &ii, NULL, &img);
    if (r) { printf("%s image: vkCreateImage -> %d\n", modname(mods[k]), r); continue; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkExportMemoryAllocateInfo ea = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryDedicatedAllocateInfo da = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &ea, .image = img };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &da, .allocationSize = mr.size, .memoryTypeIndex = __builtin_ctz(mr.memoryTypeBits) };
    VkDeviceMemory m; CK(vkAllocateMemory(dev, &mai, NULL, &m)); CK(vkBindImageMemory(dev, img, m, 0));
    VkMemoryGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = m, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    PFN_vkGetMemoryFdKHR getfd = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    int fd = -1; CK(getfd(dev, &gi, &fd));
    char p[64], l[256] = { 0 }; snprintf(p, sizeof p, "/proc/self/fd/%d", fd); readlink(p, l, sizeof l - 1);
    VkImageDrmFormatModifierPropertiesEXT ip = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    ((PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetImageDrmFormatModifierPropertiesEXT"))(dev, img, &ip);
    VkImageSubresource sr = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 }; VkSubresourceLayout sl; vkGetImageSubresourceLayout(dev, img, &sr, &sl);
    printf("%s 1920x1200 B8G8R8A8 exportable: fd %d -> %s, size %llu, modifier 0x%llx, plane0 offset %llu pitch %llu\n", modname(mods[k]), fd, l,
           (unsigned long long)mr.size, (unsigned long long)ip.drmFormatModifier, (unsigned long long)sl.offset, (unsigned long long)sl.rowPitch);
  }
  return 0;
}
