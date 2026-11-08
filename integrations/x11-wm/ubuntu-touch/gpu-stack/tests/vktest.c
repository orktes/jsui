// Offscreen Vulkan smoke test: render a clear + full-screen triangle-free fill via
// vkCmdClearColorImage on a 256x256 image, copy to a host buffer, check pixels,
// then time N submit+wait round trips.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s -> %d\n", #x, r_); exit(1); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static uint32_t memtype(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags f) {
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
  fprintf(stderr, "no memtype\n"); exit(1);
}
int main(void) {
  VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
  VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t n = 1; VkPhysicalDevice pd; CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
  if (!n) { fprintf(stderr, "no physical device\n"); return 1; }
  VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
  printf("device: %s  api %u.%u.%u  driver 0x%x\n", p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
         VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
  float prio = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
  VkDevice dev; CK(vkCreateDevice(pd, &dci, NULL, &dev));
  VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
  const int W = 256, H = 256;
  VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
    .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
  VkImage img; CK(vkCreateImage(dev, &ii, NULL, &img));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
  VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
    .memoryTypeIndex = memtype(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
  VkDeviceMemory im; CK(vkAllocateMemory(dev, &mai, NULL, &im)); CK(vkBindImageMemory(dev, img, im, 0));
  VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
  VkBuffer buf; CK(vkCreateBuffer(dev, &bci, NULL, &buf));
  vkGetBufferMemoryRequirements(dev, buf, &mr);
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = memtype(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bm; CK(vkAllocateMemory(dev, &mai, NULL, &bm)); CK(vkBindBufferMemory(dev, buf, bm, 0));
  VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
  VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
  VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  CK(vkBeginCommandBuffer(cb, &bi));
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image = img,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
  VkClearColorValue c = { .float32 = { 1.0f, 0.5f, 0.25f, 1.0f } };
  vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &b.subresourceRange);
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
  VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
  vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
  CK(vkEndCommandBuffer(cb));
  VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
  VkFence f; CK(vkCreateFence(dev, &fci, NULL, &f));
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
  CK(vkQueueSubmit(q, 1, &si, f)); CK(vkWaitForFences(dev, 1, &f, 1, ~0ull));
  uint8_t *px; CK(vkMapMemory(dev, bm, 0, W * H * 4, 0, (void **)&px));
  printf("pixel[0]=%u,%u,%u,%u pixel[last]=%u,%u,%u,%u\n", px[0], px[1], px[2], px[3],
         px[W * H * 4 - 4], px[W * H * 4 - 3], px[W * H * 4 - 2], px[W * H * 4 - 1]);
  int ok = px[0] == 255 && (px[1] == 127 || px[1] == 128) && px[W * H * 4 - 1] == 255;
  const int N = 200; double t0 = now();
  for (int i = 0; i < N; i++) { CK(vkResetFences(dev, 1, &f)); CK(vkQueueSubmit(q, 1, &si, f)); CK(vkWaitForFences(dev, 1, &f, 1, ~0ull)); }
  printf("submit+wait round trip: %.3f ms\n", (now() - t0) * 1000 / N);
  puts(ok ? "VK OFFSCREEN OK" : "VK OFFSCREEN WRONG PIXELS");
  return !ok;
}
