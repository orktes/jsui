// Skia Graphite on Vulkan (jsui_gpu_create_vulkan / jsui_render_gpu_target):
// a device of our own (Mesa lavapipe works without a GPU or display), frames
// rendered into a VkImage, read back and compared with the CPU raster of the
// same frame, including incremental frames (the image kept between them) with
// scattered damage.
//
// Needs JSUI_VULKAN (a Skia built with Graphite + Vulkan:
// scripts/build-skia-desktop.sh) and a Vulkan loader + ICD at run time;
// otherwise the test prints SKIP and passes. JSUI_VULKAN_TEST_REQUIRE=1
// turns that skip into a failure (CI jobs that install lavapipe set it).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "jsui/jsui.h"
#include "render/gpu.h"
#include "render/pipeline_cache.h"
#include "test.h"

#if defined(JSUI_VULKAN) && (defined(__linux__) || defined(_WIN32))
#define JSUI_VULKAN_TESTS 1
#define VK_NO_PROTOTYPES 1
#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "include/third_party/vulkan/vulkan/vulkan_core.h"
#endif

namespace {

bool requireVulkan() {
  const char* v = std::getenv("JSUI_VULKAN_TEST_REQUIRE");
  return v && *v && std::strcmp(v, "0") != 0;
}

void skip(const char* test, const char* why) {
  if (requireVulkan()) {
    std::fprintf(stderr, "  %s: %s but JSUI_VULKAN_TEST_REQUIRE is set\n", test, why);
    jsui_test::failures()++;
    return;
  }
  std::printf("SKIP %s: %s\n", test, why);
}

#if JSUI_VULKAN_TESTS

// A minimal Vulkan 1.1 device with one graphics queue, an RGBA8 image JSUI
// renders into and a host-visible buffer it is copied to for checking.
struct Vk {
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  std::string name;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory imageMem = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory bufferMem = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  int w = 0, h = 0;

#define VKFN(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
#define VKDFN(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))

  // Empty string on success, else why there is no device.
  std::string init(int width, int height) {
    w = width;
    h = height;
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) return "no Vulkan loader (vulkan-1.dll)";
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return "no Vulkan loader (libvulkan.so.1)";
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
    if (!gipa) return "no vkGetInstanceProcAddr";
    VKFN(vkCreateInstance);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "jsui-tests";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return "vkCreateInstance failed (no ICD?)";
    VKFN(vkEnumeratePhysicalDevices);
    VKFN(vkGetPhysicalDeviceProperties);
    VKFN(vkGetPhysicalDeviceQueueFamilyProperties);
    VKFN(vkCreateDevice);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);
      if (p.apiVersion < VK_API_VERSION_1_1) continue;
      uint32_t qn = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
      for (uint32_t i = 0; i < qn; i++)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          phys = d;
          family = i;
          name = p.deviceName;
          break;
        }
      if (phys) break;
    }
    if (!phys) return "no Vulkan 1.1 device with a graphics queue";
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) return "vkCreateDevice failed";
    VKFN(vkGetDeviceProcAddr);
    VKDFN(vkGetDeviceQueue);
    vkGetDeviceQueue(device, family, 0, &queue);

    VKDFN(vkCreateImage);
    VKDFN(vkGetImageMemoryRequirements);
    VKDFN(vkBindImageMemory);
    VKDFN(vkCreateBuffer);
    VKDFN(vkGetBufferMemoryRequirements);
    VKDFN(vkBindBufferMemory);
    VKDFN(vkCreateCommandPool);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {uint32_t(w), uint32_t(h), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ii, nullptr, &image) != VK_SUCCESS) return "vkCreateImage failed";
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, image, &mr);
    if (!(imageMem = alloc(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) return "no image memory";
    vkBindImageMemory(device, image, imageMem, 0);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(w) * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device, &bi, nullptr, &buffer) != VK_SUCCESS) return "vkCreateBuffer failed";
    vkGetBufferMemoryRequirements(device, buffer, &mr);
    if (!(bufferMem = alloc(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
      return "no host-visible memory";
    vkBindBufferMemory(device, buffer, bufferMem, 0);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) return "vkCreateCommandPool failed";
    return "";
  }

  VkDeviceMemory alloc(const VkMemoryRequirements& mr, VkMemoryPropertyFlags want) {
    VKFN(vkGetPhysicalDeviceMemoryProperties);
    VKFN(vkGetDeviceProcAddr);
    VKDFN(vkAllocateMemory);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = i;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vkAllocateMemory(device, &ai, nullptr, &m) == VK_SUCCESS) return m;
      }
    return VK_NULL_HANDLE;
  }

  JsuiVulkanDevice desc() const {
    JsuiVulkanDevice d{};
    d.instance = instance;
    d.physical_device = phys;
    d.device = device;
    d.queue = queue;
    d.queue_family = family;
    d.api_version = VK_API_VERSION_1_1;
    d.get_instance_proc_addr = reinterpret_cast<void*>(gipa);
    return d;
  }

  // Copies the image (in TRANSFER_SRC_OPTIMAL, as JSUI left it) to the
  // buffer on the same queue (JSUI's trailing barrier orders it after the
  // render) and returns its premultiplied RGBA pixels.
  std::vector<uint32_t> read() {
    VKFN(vkGetDeviceProcAddr);
    VKDFN(vkAllocateCommandBuffers);
    VKDFN(vkBeginCommandBuffer);
    VKDFN(vkCmdCopyImageToBuffer);
    VKDFN(vkEndCommandBuffer);
    VKDFN(vkQueueSubmit);
    VKDFN(vkQueueWaitIdle);
    VKDFN(vkFreeCommandBuffers);
    VKDFN(vkMapMemory);
    VKDFN(vkUnmapMemory);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &r);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cb);
    std::vector<uint32_t> px(size_t(w) * h);
    void* p = nullptr;
    vkMapMemory(device, bufferMem, 0, VK_WHOLE_SIZE, 0, &p);
    std::memcpy(px.data(), p, px.size() * 4);
    vkUnmapMemory(device, bufferMem);
    return px;
  }

  // A SAMPLED | TRANSFER_DST image holding `px` (w x h RGBA8), left in
  // SHADER_READ_ONLY_OPTIMAL like an engine's texture. Freed with the Vk.
  VkImage makeTexture(const std::vector<uint32_t>& px, int tw, int th) {
    VKFN(vkGetDeviceProcAddr);
    VKDFN(vkCreateImage);
    VKDFN(vkGetImageMemoryRequirements);
    VKDFN(vkBindImageMemory);
    VKDFN(vkCreateBuffer);
    VKDFN(vkGetBufferMemoryRequirements);
    VKDFN(vkBindBufferMemory);
    VKDFN(vkMapMemory);
    VKDFN(vkUnmapMemory);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {uint32_t(tw), uint32_t(th), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Owned o;
    if (vkCreateImage(device, &ii, nullptr, &o.image) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, o.image, &mr);
    o.imageMem = alloc(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(device, o.image, o.imageMem, 0);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(px.size()) * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device, &bi, nullptr, &o.buffer);
    vkGetBufferMemoryRequirements(device, o.buffer, &mr);
    o.bufferMem = alloc(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkBindBufferMemory(device, o.buffer, o.bufferMem, 0);
    void* p = nullptr;
    vkMapMemory(device, o.bufferMem, 0, VK_WHOLE_SIZE, 0, &p);
    std::memcpy(p, px.data(), px.size() * 4);
    vkUnmapMemory(device, o.bufferMem);
    owned.push_back(o);
    upload(o.image, o.buffer, tw, th, VK_IMAGE_LAYOUT_UNDEFINED);
    return o.image;
  }

  // Copies `buffer` into `img` (from layout `from`) and leaves it in
  // SHADER_READ_ONLY_OPTIMAL; waits for the queue.
  void upload(VkImage img, VkBuffer buf, int tw, int th, VkImageLayout from) {
    VKFN(vkGetDeviceProcAddr);
    VKDFN(vkAllocateCommandBuffers);
    VKDFN(vkBeginCommandBuffer);
    VKDFN(vkCmdPipelineBarrier);
    VKDFN(vkCmdCopyBufferToImage);
    VKDFN(vkEndCommandBuffer);
    VKDFN(vkQueueSubmit);
    VKDFN(vkQueueWaitIdle);
    VKDFN(vkFreeCommandBuffers);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device, &ai, &cb);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {uint32_t(tw), uint32_t(th), 1};
    vkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cb);
  }
  // New contents for a makeTexture() image (same size).
  void rewrite(VkImage img, const std::vector<uint32_t>& px, int tw, int th) {
    VKFN(vkGetDeviceProcAddr);
    VKDFN(vkMapMemory);
    VKDFN(vkUnmapMemory);
    for (auto& o : owned)
      if (o.image == img) {
        void* p = nullptr;
        vkMapMemory(device, o.bufferMem, 0, VK_WHOLE_SIZE, 0, &p);
        std::memcpy(p, px.data(), px.size() * 4);
        vkUnmapMemory(device, o.bufferMem);
        upload(img, o.buffer, tw, th, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      }
  }
  struct Owned {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMem = VK_NULL_HANDLE;
  };
  std::vector<Owned> owned;

  ~Vk() {
    if (!instance) return;
    if (device) {
      VKFN(vkGetDeviceProcAddr);
      VKDFN(vkDeviceWaitIdle);
      VKDFN(vkDestroyCommandPool);
      VKDFN(vkDestroyImage);
      VKDFN(vkDestroyBuffer);
      VKDFN(vkFreeMemory);
      VKDFN(vkDestroyDevice);
      vkDeviceWaitIdle(device);
      if (pool) vkDestroyCommandPool(device, pool, nullptr);
      for (auto& o : owned) {
        vkDestroyImage(device, o.image, nullptr);
        vkDestroyBuffer(device, o.buffer, nullptr);
        vkFreeMemory(device, o.imageMem, nullptr);
        vkFreeMemory(device, o.bufferMem, nullptr);
      }
      if (image) vkDestroyImage(device, image, nullptr);
      if (buffer) vkDestroyBuffer(device, buffer, nullptr);
      if (imageMem) vkFreeMemory(device, imageMem, nullptr);
      if (bufferMem) vkFreeMemory(device, bufferMem, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    VKFN(vkDestroyInstance);
    vkDestroyInstance(instance, nullptr);
  }
#undef VKFN
#undef VKDFN
};

// Mean absolute channel difference and the share of pixels off by > tol.
void compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int tol, double* mean, double* over) {
  uint64_t sum = 0, n = 0;
  for (size_t i = 0; i < a.size(); i++) {
    int m = 0;
    for (int c = 0; c < 4; c++) {
      int x = int((a[i] >> (c * 8)) & 255), y = int((b[i] >> (c * 8)) & 255);
      int v = x > y ? x - y : y - x;
      sum += uint64_t(v);
      if (v > m) m = v;
    }
    if (m > tol) n++;
  }
  *mean = double(sum) / double(a.size() * 4);
  *over = double(n) / double(a.size());
}

// Two boxes in opposite corners that move every 16 ms (scattered damage)
// over a gradient backdrop, a rounded border and text.
const char* kBundle =
    "const n = __jsui_native;"
    "const bg = n.createNode('View');"
    "n.setProps(bg, {style: {position: 'absolute', left: 0, top: 0, width: 240, height: 160,"
    "  backgroundImage: 'linear-gradient(135deg, #203040, #402030)'}});"
    "n.appendChild(n.rootId, bg);"
    "const card = n.createNode('View');"
    "n.setProps(card, {style: {position: 'absolute', left: 70, top: 50, width: 100, height: 60, borderRadius: 12,"
    "  borderWidth: 2, borderColor: '#ffffff', backgroundColor: 'rgba(255,255,255,0.15)',"
    "  boxShadow: '0 4px 10px rgba(0,0,0,0.5)'}});"
    "n.appendChild(n.rootId, card);"
    "const t = n.createNode('Text'); const s = n.createNode('RawText'); n.setText(s, 'Graphite');"
    "n.appendChild(t, s); n.setProps(t, {style: {color: '#ffcc00', fontSize: 16}}); n.appendChild(card, t);"
    "const a = n.createNode('View'), b = n.createNode('View');"
    "n.appendChild(n.rootId, a); n.appendChild(n.rootId, b);"
    "let f = 0;"
    "function step() {"
    "  n.setProps(a, {style: {position: 'absolute', left: 4 + f * 2, top: 4, width: 16, height: 16,"
    "    borderRadius: 8, backgroundColor: '#00ff80'}});"
    "  n.setProps(b, {style: {position: 'absolute', left: 216 - f * 2, top: 140, width: 16, height: 16,"
    "    backgroundColor: '#ff4060', opacity: 0.8}});"
    "  f++; if (f < 6) setTimeout(step, 16);"
    "}"
    "step();";

JsuiRuntime* makeRuntime(int w, int h) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = w;
  cfg.height = h;
  JsuiRuntime* rt = jsui_create(&cfg);
  CHECK_EQ(jsui_load_bundle_source(rt, kBundle, std::strlen(kBundle), "vulkan.js"), 0);
  return rt;
}

// kBundle's six frames, by a new runtime, into vk.image. Returns the
// pipelines its renders compiled on demand (JsuiPerf::pipeline_compiles).
unsigned long long renderFrames(Vk& vk, JsuiGpuContext* gpu) {
  JsuiRuntime* rt = makeRuntime(vk.w, vk.h);
  int layout = VK_IMAGE_LAYOUT_UNDEFINED;
  for (int frame = 0; frame < 6; frame++) {
    jsui_tick(rt, frame * 16.0 + 1);
    JsuiGpuTarget t{};
    t.texture = vk.image;
    t.width = vk.w;
    t.height = vk.h;
    t.layout = layout;
    t.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    t.flags = frame > 0 ? JSUI_GPU_INCREMENTAL : 0;
    CHECK_EQ(jsui_render_gpu_target(rt, gpu, &t, nullptr), 0);
    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  }
  vk.read();  // waits for the queue
  JsuiPerf p;
  jsui_get_perf(rt, &p, 0);
  jsui_destroy(rt);
  return p.pipeline_compiles;
}

JsuiPipelineStats pipelineStats(JsuiGpuContext* gpu) {
  JsuiPipelineStats s;
  jsui_gpu_get_pipeline_stats(gpu, &s);
  return s;
}

// Until the worker thread precompiled everything queued (lavapipe is slow).
bool waitPrecompiled(JsuiGpuContext* gpu) {
  for (int i = 0; i < 6000; i++) {
    if (pipelineStats(gpu).queued == 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// ShaderViews (a fill with an image child, and one post-processing its
// content, inside a rounded overflow clip) mounted at 600 ms over a page
// that preloads their shaders when `preload` (warmed over the frames before).
// Returns the pipelines the frames from the mount on compiled.
unsigned long long shaderFrames(Vk& vk, JsuiGpuContext* gpu, bool preload) {
  static const char* kShaders =
      "const FILL = 'uniform shader img; uniform float glow; half4 main(float2 p) {"
      "  return half4(half(fract(p.x * 0.05)), half(glow), 0.5, 1) + img.eval(p); }';"
      "const CONTENT = 'uniform shader content; uniform float2 resolution; half4 main(float2 p) {"
      "  return content.eval(float2(resolution.x - p.x, p.y)).bgra; }';"
      "const IMAGES = {img: {uri: 'missing.png', fit: 'cover'}};";
  std::string bundle = std::string(kShaders) +
                       "const n = __jsui_native;"
                       "function view(type, parent, style, props) {"
                       "  const v = n.createNode(type); n.setProps(v, Object.assign({style}, props || {}));"
                       "  n.appendChild(parent, v); return v; }"
                       "view('View', n.rootId, {position: 'absolute', left: 0, top: 0, width: 240, height: 160,"
                       "  backgroundColor: '#203040'});"
                       "view('View', n.rootId, {position: 'absolute', left: 4, top: 120, width: 20, height: 20,"
                       "  backgroundColor: '#ff0000'});" +
                       (preload ? "if (n.shaderPreload(FILL, IMAGES).error || n.shaderPreload(CONTENT).error) throw new Error('compile');"
                                  "if (!(n.shadersWarming() > 2)) throw new Error('queued ' + n.shadersWarming());"
                                : "") +
                       "setTimeout(() => {"
                       "  if (n.shadersWarming() !== 0) throw new Error('still warming');"
                       "  const clip = view('View', n.rootId, {position: 'absolute', left: 20.5, top: 5, width: 80,"
                       "    height: 50, borderRadius: 12, overflow: 'hidden'});"
                       "  view('ShaderView', clip, {position: 'absolute', left: 9.5, top: 5, width: 60, height: 40},"
                       "    {source: FILL, uniforms: {glow: 0.5}, images: IMAGES});"
                       "  const c = view('ShaderView', n.rootId, {position: 'absolute', left: 100, top: 10, width: 60,"
                       "    height: 40}, {source: CONTENT});"
                       "  view('View', c, {position: 'absolute', left: 4, top: 4, width: 20, height: 20,"
                       "    backgroundColor: '#ff0000'});"
                       "}, 600);";
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = vk.w;
  cfg.height = vk.h;
  JsuiRuntime* rt = jsui_create(&cfg);
  CHECK_EQ(jsui_load_bundle_source(rt, bundle.data(), bundle.size(), "shaders.js"), 0);
  int layout = VK_IMAGE_LAYOUT_UNDEFINED;
  unsigned long long compiles[2] = {0, 0};  // before / from the mount on
  for (int frame = 0; frame < 40; frame++) {
    const double now = frame * 16.0 + 1;
    jsui_tick(rt, now);
    JsuiGpuTarget t{};
    t.texture = vk.image;
    t.width = vk.w;
    t.height = vk.h;
    t.layout = layout;
    t.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    t.flags = frame > 0 ? JSUI_GPU_INCREMENTAL : 0;
    CHECK_EQ(jsui_render_gpu_target(rt, gpu, &t, nullptr), 0);
    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    JsuiPerf p;
    jsui_get_perf(rt, &p, 1);
    compiles[now >= 600] += p.pipeline_compiles;
  }
  CHECK(jsui_last_error(rt) == nullptr || !*jsui_last_error(rt));
  std::vector<uint32_t> out = vk.read();
  // The views drew: the fill's blue, the mirrored red box (bgra: blue).
  CHECK_EQ(out[size_t(30) * vk.w + 60] >> 16 & 255, 128u);  // inside the clip
  CHECK_EQ(out[size_t(20) * vk.w + 100 + 60 - 14], 0xffff0000u);
  std::printf("  %s: %llu pipelines compiled before the ShaderViews, %llu with them\n",
              preload ? "preloaded" : "cold", compiles[0], compiles[1]);
  jsui_destroy(rt);
  return compiles[1];
}

#endif  // JSUI_VULKAN_TESTS

}  // namespace

TEST(vulkan_graphite_matches_cpu) {
#if !JSUI_VULKAN_TESTS
  skip("vulkan_graphite_matches_cpu", "built without JSUI_VULKAN");
#else
  const int W = 240, H = 160;
  Vk vk;
  std::string why = vk.init(W, H);
  if (!why.empty()) {
    skip("vulkan_graphite_matches_cpu", why.c_str());
    return;
  }
  JsuiVulkanDevice d = vk.desc();
  JsuiGpuContext* gpu = jsui_gpu_create_vulkan(&d);
  CHECK(gpu != nullptr);
  if (!gpu) return;
  std::printf("  Vulkan device: %s\n", vk.name.c_str());
  // The same bundle in two runtimes on the same clock: one on the GPU, one
  // as the CPU reference (a CPU render must not reset the GPU one's damage).
  JsuiRuntime* g = makeRuntime(W, H);
  JsuiRuntime* c = makeRuntime(W, H);
  std::vector<uint32_t> cpu(size_t(W) * H);
  int layout = VK_IMAGE_LAYOUT_UNDEFINED;
  int incrementalFrames = 0;
  for (int frame = 0; frame < 6; frame++) {
    double now = frame * 16.0 + 1;
    jsui_tick(g, now);
    jsui_tick(c, now);
    JsuiGpuTarget t{};
    t.texture = vk.image;
    t.width = W;
    t.height = H;
    t.layout = layout;
    t.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    t.flags = frame > 0 ? JSUI_GPU_INCREMENTAL : 0;
    JsuiRect dirty{};
    CHECK_EQ(jsui_render_gpu_target(g, gpu, &t, &dirty), 0);
    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (frame > 0 && dirty.width * dirty.height < W * H) incrementalFrames++;
    jsui_render_rgba(c, cpu.data(), W, H, W * 4);
    std::vector<uint32_t> out = vk.read();
    double mean = 0, over = 0;
    compare(out, cpu, 24, &mean, &over);
    if (mean > 1.0 || over > 0.01) {
      std::fprintf(stderr, "  frame %d: GPU differs from CPU: mean %.3f, %.2f%% of pixels off by > 24 (dirty %d,%d %dx%d)\n",
                   frame, mean, over * 100, dirty.x, dirty.y, dirty.width, dirty.height);
      jsui_test::failures()++;
    }
    // Inside the moved (translucent) box: the same colour within rounding.
    if (frame == 5) {
      uint32_t o = out[size_t(140 + 8) * W + (216 - 10 + 8)], r = cpu[size_t(140 + 8) * W + (216 - 10 + 8)];
      for (int ch = 0; ch < 4; ch++) CHECK(std::abs(int((o >> (ch * 8)) & 255) - int((r >> (ch * 8)) & 255)) <= 2);
    }
    std::printf("  frame %d: dirty %d,%d %dx%d, mean diff %.3f\n", frame, dirty.x, dirty.y, dirty.width, dirty.height, mean);
  }
  CHECK(incrementalFrames >= 4);
  // Nothing changed: no GPU work, nothing reported dirty.
  jsui_tick(g, 200);
  JsuiGpuTarget t{};
  t.texture = vk.image;
  t.width = W;
  t.height = H;
  t.layout = t.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  t.flags = JSUI_GPU_INCREMENTAL;
  JsuiRect dirty{1, 1, 1, 1};
  CHECK_EQ(jsui_render_gpu_target(g, gpu, &t, &dirty), 0);
  CHECK_EQ(dirty.width * dirty.height, 0);
  jsui_destroy(g);
  jsui_destroy(c);
  jsui_gpu_destroy(gpu);
#endif
}

// The pipeline cache (jsui_gpu_set_pipeline_cache): a first context compiles
// every pipeline in its frames and records them; the next one precompiles
// them on its worker thread and the same frames compile nothing. A list
// saved in memory warms a context that precompiles in host-driven slices,
// and one from a device of other formats a context of this device.
TEST(vulkan_pipeline_cache_warms_the_next_context) {
#if !JSUI_VULKAN_TESTS
  skip("vulkan_pipeline_cache_warms_the_next_context", "built without JSUI_VULKAN");
#else
  Vk vk;
  std::string why = vk.init(240, 160);
  if (!why.empty()) {
    skip("vulkan_pipeline_cache_warms_the_next_context", why.c_str());
    return;
  }
  JsuiVulkanDevice d = vk.desc();
  const std::string path = (std::filesystem::temp_directory_path() / "jsui-test-vulkan-pipelines.bin").string();
  std::remove(path.c_str());

  JsuiGpuContext* gpu = jsui_gpu_create_vulkan(&d);
  CHECK(gpu != nullptr);
  if (!gpu) return;
  CHECK_EQ(jsui_gpu_set_pipeline_cache(gpu, path.c_str()), 0);  // a new file
  const unsigned long long cold = renderFrames(vk, gpu);
  JsuiPipelineStats s = pipelineStats(gpu);
  std::printf("  cold: %llu pipelines compiled in frames, %llu recorded, %llu not serializable\n", cold, s.added,
              s.unserializable);
  CHECK(cold > 0);
  CHECK_EQ(s.mode, int(JSUI_PRECOMPILE_THREAD));
  CHECK_EQ(s.compiled, cold);
  CHECK_EQ(s.unserializable, 0ull);
  CHECK_EQ(s.added, cold);
  jsui_gpu_destroy(gpu);

  gpu = jsui_gpu_create_vulkan(&d);
  const int n = jsui_gpu_set_pipeline_cache(gpu, path.c_str());
  CHECK_EQ(n, int(cold));
  CHECK(waitPrecompiled(gpu));
  s = pipelineStats(gpu);
  CHECK_EQ(s.precompiled, (unsigned long long)n);
  CHECK_EQ(s.failed, 0ull);
  CHECK_EQ(renderFrames(vk, gpu), 0ull);
  CHECK_EQ(pipelineStats(gpu).compiled, 0ull);
  void* list = nullptr;
  size_t size = 0;
  CHECK_EQ(jsui_gpu_save_pipelines(gpu, &list, &size), 0);
  const std::string saved(static_cast<const char*>(list), size);
  jsui_free(list);
  jsui_gpu_destroy(gpu);

  // Destroyed while its worker is precompiling: stops after the pipeline it
  // is on, before the device goes.
  gpu = jsui_gpu_create_vulkan(&d);
  CHECK_EQ(jsui_gpu_set_pipeline_cache(gpu, path.c_str()), n);
  jsui_gpu_destroy(gpu);

  jsui::pipeline_cache::setForceSliced(true);
  gpu = jsui_gpu_create_vulkan(&d);
  jsui::pipeline_cache::setForceSliced(false);
  CHECK_EQ(pipelineStats(gpu).mode, int(JSUI_PRECOMPILE_SLICED));
  CHECK_EQ(jsui_gpu_precompile_pipelines(gpu, saved.data(), saved.size()), n);
  CHECK_EQ(pipelineStats(gpu).queued, n);
  CHECK_EQ(jsui_gpu_precompile_step(gpu, 0), n - 1);  // at least one per step
  CHECK_EQ(jsui_gpu_precompile_step(gpu, 1e6), 0);
  CHECK_EQ(pipelineStats(gpu).precompiled, (unsigned long long)n);
  CHECK_EQ(renderFrames(vk, gpu), 0ull);
  jsui_gpu_destroy(gpu);

  // A list recorded on a device whose Graphite picks other depth/stencil
  // formats (as a list shipped from one vendor's GPU meets another's): its
  // keys are fitted to this device and still warm the frames.
  using namespace jsui::pipeline_cache;
  gpu = jsui_gpu_create_vulkan(&d);
  std::string other = header(Backend::Vulkan);
  int swapped = 0;
  for (const Entry& e : parse(saved.data(), saved.size(), Backend::Vulkan).entries) {
    const std::string k = gpu->pipelines->swapDepthStencilForTest(e.data);
    swapped += !k.empty();
    other += k.empty() ? record(e.data.data(), e.data.size()) : record(k.data(), k.size());
  }
  std::printf("  %d of %d keys with the other depth/stencil format\n", swapped, n);
  CHECK(swapped > 0);
  CHECK_EQ(jsui_gpu_precompile_pipelines(gpu, other.data(), other.size()), n);
  CHECK(waitPrecompiled(gpu));
  CHECK_EQ(pipelineStats(gpu).failed, 0ull);
  CHECK_EQ(renderFrames(vk, gpu), 0ull);
  jsui_gpu_destroy(gpu);
  std::remove(path.c_str());
#endif
}

// Host textures (JSUI_TEXTURE_VULKAN): an engine's sampled image drawn in
// place by Graphite, new contents shown after jsui_texture_updated, the
// image still usable by the host (SHADER_READ_ONLY_OPTIMAL) afterwards.
// Preloaded shaders (shaderPreload / precompileShaders): the frame that
// mounts the views compiles no pipeline; without the preload it does.
TEST(vulkan_shader_preload_warms_pipelines) {
#if !JSUI_VULKAN_TESTS
  skip("vulkan_shader_preload_warms_pipelines", "built without JSUI_VULKAN");
#else
  Vk vk;
  std::string why = vk.init(240, 160);
  if (!why.empty()) {
    skip("vulkan_shader_preload_warms_pipelines", why.c_str());
    return;
  }
  JsuiVulkanDevice d = vk.desc();
  JsuiGpuContext* gpu = jsui_gpu_create_vulkan(&d);
  CHECK(gpu != nullptr);
  if (!gpu) return;
  CHECK(shaderFrames(vk, gpu, false) > 0);
  jsui_gpu_destroy(gpu);
  gpu = jsui_gpu_create_vulkan(&d);
  CHECK_EQ(shaderFrames(vk, gpu, true), 0ull);
  jsui_gpu_destroy(gpu);
#endif
}

TEST(vulkan_host_texture) {
#if !JSUI_VULKAN_TESTS
  skip("vulkan_host_texture", "built without JSUI_VULKAN");
#else
  const int W = 64, H = 48;
  Vk vk;
  std::string why = vk.init(W, H);
  if (!why.empty()) {
    skip("vulkan_host_texture", why.c_str());
    return;
  }
  JsuiVulkanDevice d = vk.desc();
  JsuiGpuContext* gpu = jsui_gpu_create_vulkan(&d);
  CHECK(gpu != nullptr);
  if (!gpu) return;
  std::vector<uint32_t> px(8);
  for (int x = 0; x < 4; x++) {
    px[x] = 0xff0000ffu;      // top row red
    px[4 + x] = 0xffff0000u;  // bottom row blue
  }
  VkImage tex = vk.makeTexture(px, 4, 2);
  CHECK(tex != VK_NULL_HANDLE);
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = W;
  cfg.height = H;
  JsuiRuntime* rt = jsui_create(&cfg);
  const char* src =
      "const n = __jsui_native; const v = n.createNode('Image');"
      "n.setProps(v, {texture: 'engine', resizeMode: 'stretch',"
      " style: {position: 'absolute', left: 8, top: 8, width: 40, height: 32}});"
      "n.appendChild(n.rootId, v);";
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "vktex.js"), 0);
  JsuiTexture t{};
  t.kind = JSUI_TEXTURE_VULKAN;
  t.handle = tex;
  t.width = 4;
  t.height = 2;
  t.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  CHECK_EQ(jsui_set_texture(rt, "engine", &t), 1);
  auto render = [&](int layout, int flags) {
    jsui_tick(rt, 16);
    JsuiGpuTarget gt{};
    gt.texture = vk.image;
    gt.width = W;
    gt.height = H;
    gt.layout = layout;
    gt.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    gt.flags = flags;
    CHECK_EQ(jsui_render_gpu_target(rt, gpu, &gt, nullptr), 0);
    return vk.read();
  };
  std::vector<uint32_t> out = render(VK_IMAGE_LAYOUT_UNDEFINED, 0);
  auto at = [&](int x, int y) { return out[size_t(y) * W + x]; };
  CHECK_EQ(at(28, 12), 0xff0000ffu);
  CHECK_EQ(at(28, 36), 0xffff0000u);
  CHECK_EQ(at(2, 2), 0u);
  // The host draws the next "frame" into the same image.
  std::vector<uint32_t> green(8, 0xff00ff00u);
  vk.rewrite(tex, green, 4, 2);
  CHECK_EQ(jsui_texture_updated(rt, "engine"), 1);
  CHECK(jsui_needs_redraw(rt));
  out = render(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, JSUI_GPU_INCREMENTAL);
  CHECK_EQ(at(28, 12), 0xff00ff00u);
  CHECK_EQ(at(28, 36), 0xff00ff00u);
  // A format Graphite cannot sample here: the view is left empty.
  t.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  CHECK_EQ(jsui_set_texture(rt, "engine", &t), 1);
  out = render(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);
  CHECK_EQ(at(28, 12), 0u);
  jsui_destroy(rt);
  jsui_gpu_destroy(gpu);
#endif
}

TEST(vulkan_capi_rejects_bad_input) {
  CHECK(jsui_gpu_create_vulkan(nullptr) == nullptr);
  JsuiVulkanDevice d{};
  CHECK(jsui_gpu_create_vulkan(&d) == nullptr);
  JsuiRuntime* rt = jsui_create(nullptr);
  CHECK_EQ(jsui_render_gpu_target(rt, nullptr, nullptr, nullptr), -1);
  CHECK_EQ(jsui_render_vulkan(rt, nullptr, nullptr, 16, 16, 0), -1);
  jsui_destroy(rt);
}
