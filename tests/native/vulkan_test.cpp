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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

#if defined(JSUI_VULKAN) && defined(__linux__)
#define JSUI_VULKAN_TESTS 1
#define VK_NO_PROTOTYPES 1
#include <dlfcn.h>

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
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return "no Vulkan loader (libvulkan.so.1)";
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
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

TEST(vulkan_capi_rejects_bad_input) {
  CHECK(jsui_gpu_create_vulkan(nullptr) == nullptr);
  JsuiVulkanDevice d{};
  CHECK(jsui_gpu_create_vulkan(&d) == nullptr);
  JsuiRuntime* rt = jsui_create(nullptr);
  CHECK_EQ(jsui_render_gpu_target(rt, nullptr, nullptr, nullptr), -1);
  CHECK_EQ(jsui_render_vulkan(rt, nullptr, nullptr, 16, 16, 0), -1);
  jsui_destroy(rt);
}
