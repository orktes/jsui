// The Unity plugin's Vulkan backend (native/jsui_unity_vulkan.cpp) without
// Unity: this loads the built plugin (libjsui_unity.so) the way Unity does
// and plays Unity's part over a Vulkan device of its own (Mesa lavapipe works
// without a GPU or display):
//
//  - IUnityInterfaces with IUnityGraphics (renderer: Vulkan, device events,
//    event id reservation) and IUnityGraphicsVulkanV2 (Instance,
//    AccessTexture with Unity-style layout / access tracking and barriers
//    recorded into "Unity's" current command buffer, EnsureOutsideRenderPass,
//    AccessQueue).
//  - a frame like Unity's: work recorded into a command buffer that is not
//    submitted yet (frame 0: a magenta clear of the view's texture, which
//    JSUI's frame must end up over), the view's two plugin events (what
//    JsuiView.PaintGpu issues), then Unity's own use of the texture after
//    them (a copy to a buffer, standing in for the canvas sampling it), all
//    submitted at the end of the frame.
//  - AccessQueue's callback either synchronously (flushing what was recorded
//    so far first) or deferred to a "submission thread" at the end of the
//    frame, in submission order, while the main thread waits for the job
//    (jsuiu_wait_render) as JsuiView does before touching the runtime.
//
// Every frame's pixels are compared with the CPU raster of the same frame
// (the plugin's own jsui_render_rgba in a second runtime), including
// incremental frames, both for a texture Graphite draws into directly
// (colour + input attachment) and for one it cannot (no INPUT_ATTACHMENT
// usage, as many of Unity's RenderTextures), where the plugin draws into an
// image of its own and copies the repainted rect into Unity's. Then a
// texture that is neither (no TRANSFER_DST either) must turn the view to the
// CPU path with a reason.
//
//   jsui-unity-vulkan-test [<libjsui_unity.so>]
//
// Exit 0: pass; 1: failure; 77: skipped (no Vulkan device), which
// JSUI_VULKAN_TEST_REQUIRE=1 turns into a failure.
#define VK_NO_PROTOTYPES 1
#include "include/third_party/vulkan/vulkan/vulkan_core.h"

#define JSUI_UNITY_API_VULKAN 1
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "jsui/jsui.h"
#include "jsui_unity_api.h"

namespace {

int g_failures = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      g_failures++;                                                           \
    }                                                                         \
  } while (0)

// ------------------------------------------------------------------ Vulkan

struct Vk {
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  std::string name;
  VkCommandPool pool = VK_NULL_HANDLE;

#define VK_FNS(X)                     \
  X(vkGetDeviceQueue)                 \
  X(vkCreateImage)                    \
  X(vkDestroyImage)                   \
  X(vkGetImageMemoryRequirements)     \
  X(vkBindImageMemory)                \
  X(vkCreateBuffer)                   \
  X(vkDestroyBuffer)                  \
  X(vkGetBufferMemoryRequirements)    \
  X(vkBindBufferMemory)               \
  X(vkAllocateMemory)                 \
  X(vkFreeMemory)                     \
  X(vkMapMemory)                      \
  X(vkUnmapMemory)                    \
  X(vkCreateCommandPool)              \
  X(vkDestroyCommandPool)             \
  X(vkAllocateCommandBuffers)         \
  X(vkFreeCommandBuffers)             \
  X(vkBeginCommandBuffer)             \
  X(vkEndCommandBuffer)               \
  X(vkCmdPipelineBarrier)             \
  X(vkCmdClearColorImage)             \
  X(vkCmdCopyImageToBuffer)           \
  X(vkQueueSubmit)                    \
  X(vkQueueWaitIdle)                  \
  X(vkDeviceWaitIdle)                 \
  X(vkDestroyDevice)
#define X(f) PFN_##f f = nullptr;
  VK_FNS(X)
#undef X

  // Empty on success, else why there is no device.
  std::string init() {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return "no Vulkan loader (libvulkan.so.1)";
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) return "no vkGetInstanceProcAddr";
    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "jsui-unity-vulkan-test";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return "vkCreateInstance failed (no ICD?)";
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto qprops = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
    uint32_t n = 0;
    enumerate(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    enumerate(instance, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
      VkPhysicalDeviceProperties p;
      props(d, &p);
      if (p.apiVersion < VK_API_VERSION_1_1) continue;
      uint32_t qn = 0;
      qprops(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qn);
      qprops(d, &qn, qs.data());
      for (uint32_t i = 0; i < qn && !phys; i++)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          phys = d;
          family = i;
          name = p.deviceName;
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
    if (createDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) return "vkCreateDevice failed";
#define X(f) f = reinterpret_cast<PFN_##f>(gdpa(device, #f));
    VK_FNS(X)
#undef X
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) return "vkCreateCommandPool failed";
    return "";
  }

  VkDeviceMemory alloc(const VkMemoryRequirements& mr, VkMemoryPropertyFlags want) {
    auto memProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
    VkPhysicalDeviceMemoryProperties mp;
    memProps(phys, &mp);
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

  ~Vk() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (pool) vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance) reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, nullptr);
  }
};

// A texture as Unity's Vulkan backend keeps one: the image plus the layout
// and last access its tracking has for it. The plugin only ever sees a
// pointer to this (RenderTexture.GetNativeTexturePtr), which it hands back
// to AccessTexture.
struct FakeTexture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  VkImageUsageFlags usage = 0;
  int width = 0, height = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  VkAccessFlags access = 0;
};

// ------------------------------------------------------------------- Unity

struct FakeUnity;
FakeUnity* g_unity = nullptr;

struct FakeUnity {
  Vk& vk;
  IUnityInterfaces interfaces{};
  IUnityGraphics graphics{};
  IUnityGraphicsVulkanV2 vulkan{};
  IUnityGraphicsDeviceEventCallback deviceCallback = nullptr;
  int nextEventId = 100;  // other plugins' ids before ours

  // Unity's frame: the command buffer being recorded, and what is waiting
  // for submission in order (command buffers and queue-access callbacks).
  VkCommandBuffer current = VK_NULL_HANDLE;
  struct Item {
    VkCommandBuffer cb;
    UnityRenderingEventAndData callback;
    int eventId;
    void* data;
  };
  std::vector<Item> pending;
  std::vector<VkCommandBuffer> submitted;  // freed after the frame
  bool deferQueueAccess = false;
  bool inQueueAccess = false;
  bool misuse = false;  // the plugin broke one of Unity's rules
  int queueAccesses = 0;

  explicit FakeUnity(Vk& v) : vk(v) {
    g_unity = this;
    interfaces.GetInterfaceSplit = [](unsigned long long high, unsigned long long low) -> IUnityInterface* {
      if (high == JsuiUnityGUID<IUnityGraphics>::high && low == JsuiUnityGUID<IUnityGraphics>::low)
        return &g_unity->graphics;
      if (high == JsuiUnityGUID<IUnityGraphicsVulkanV2>::high && low == JsuiUnityGUID<IUnityGraphicsVulkanV2>::low)
        return &g_unity->vulkan;
      return nullptr;
    };
    graphics.GetRenderer = []() { return kUnityGfxRendererVulkan; };
    graphics.RegisterDeviceEventCallback = [](IUnityGraphicsDeviceEventCallback cb) { g_unity->deviceCallback = cb; };
    graphics.UnregisterDeviceEventCallback = [](IUnityGraphicsDeviceEventCallback cb) {
      if (g_unity->deviceCallback == cb) g_unity->deviceCallback = nullptr;
    };
    graphics.ReserveEventIDRange = [](int count) {
      int base = g_unity->nextEventId;
      g_unity->nextEventId += count;
      return base;
    };
    vulkan.Instance = []() {
      UnityVulkanInstance i{};
      i.instance = g_unity->vk.instance;
      i.physicalDevice = g_unity->vk.phys;
      i.device = g_unity->vk.device;
      i.graphicsQueue = g_unity->vk.queue;
      i.getInstanceProcAddr = g_unity->vk.gipa;
      i.queueFamilyIndex = g_unity->vk.family;
      return i;
    };
    vulkan.EnsureOutsideRenderPass = []() {
      if (g_unity->inQueueAccess) g_unity->misuse = true;
    };
    vulkan.AccessTexture = [](void* native, const VkImageSubresource*, VkImageLayout layout, VkPipelineStageFlags stage,
                              VkAccessFlags access, UnityVulkanResourceAccessMode mode, UnityVulkanImage* out) {
      FakeUnity& u = *g_unity;
      if (u.inQueueAccess) u.misuse = true;  // not allowed with queue access
      auto* t = static_cast<FakeTexture*>(native);
      if (mode == kUnityVulkanResourceAccess_PipelineBarrier) u.barrier(*t, layout, stage, access);
      *out = UnityVulkanImage{};
      out->image = t->image;
      out->layout = t->layout;
      out->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      out->usage = t->usage;
      out->format = t->format;
      out->extent = {uint32_t(t->width), uint32_t(t->height), 1};
      out->tiling = VK_IMAGE_TILING_OPTIMAL;
      out->type = VK_IMAGE_TYPE_2D;
      out->samples = VK_SAMPLE_COUNT_1_BIT;
      out->layers = 1;
      out->mipCount = 1;
      return true;
    };
    vulkan.AccessQueue = [](UnityRenderingEventAndData cb, int eventId, void* data, bool flush) {
      FakeUnity& u = *g_unity;
      if (u.inQueueAccess) u.misuse = true;
      if (flush) u.closeCurrent();
      u.pending.push_back({VK_NULL_HANDLE, cb, eventId, data});
      if (!u.deferQueueAccess) u.process();  // "synchronously"
    };
  }

  VkCommandBuffer cmd() {
    if (!current) {
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ai.commandPool = vk.pool;
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      vk.vkAllocateCommandBuffers(vk.device, &ai, &current);
      VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vk.vkBeginCommandBuffer(current, &bi);
    }
    return current;
  }
  void closeCurrent() {
    if (!current) return;
    vk.vkEndCommandBuffer(current);
    pending.push_back({current, nullptr, 0, nullptr});
    current = VK_NULL_HANDLE;
  }
  // Submits / runs what is pending, in order (Unity's submission).
  void process() {
    std::vector<Item> items;
    items.swap(pending);
    for (const Item& it : items) {
      if (it.cb) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &it.cb;
        vk.vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
        submitted.push_back(it.cb);
      } else {
        inQueueAccess = true;
        queueAccesses++;
        it.callback(it.eventId, it.data);
        inQueueAccess = false;
      }
    }
  }
  // Unity's tracking: a barrier from the texture's last use to the next.
  void barrier(FakeTexture& t, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = t.access;
    b.dstAccessMask = access;
    b.oldLayout = t.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdPipelineBarrier(cmd(), t.stage, stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    t.layout = layout;
    t.stage = stage;
    t.access = access;
  }
  // Unity's own work on the texture, recorded into its current command buffer.
  void clear(FakeTexture& t, float r, float g, float b, float a) {
    barrier(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue c{};
    c.float32[0] = r;
    c.float32[1] = g;
    c.float32[2] = b;
    c.float32[3] = a;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdClearColorImage(cmd(), t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
  }
  void copyOut(FakeTexture& t, VkBuffer buffer) {
    barrier(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {uint32_t(t.width), uint32_t(t.height), 1};
    vk.vkCmdCopyImageToBuffer(cmd(), t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &r);
    // Then "samples" it, as the canvas would.
    barrier(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
  }
  // The end of the frame: everything goes to the queue.
  void endFrame() {
    closeCurrent();
    process();
  }
  void afterFrame() {
    vk.vkQueueWaitIdle(vk.queue);
    if (!submitted.empty()) vk.vkFreeCommandBuffers(vk.device, vk.pool, uint32_t(submitted.size()), submitted.data());
    submitted.clear();
  }

  bool makeTexture(FakeTexture& t, int w, int h, VkImageUsageFlags usage) {
    t.width = w;
    t.height = h;
    t.usage = usage;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = t.format;
    ii.extent = {uint32_t(w), uint32_t(h), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(vk.device, &ii, nullptr, &t.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vk.vkGetImageMemoryRequirements(vk.device, t.image, &mr);
    t.memory = vk.alloc(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    return t.memory && vk.vkBindImageMemory(vk.device, t.image, t.memory, 0) == VK_SUCCESS;
  }
  void freeTexture(FakeTexture& t) {
    if (t.image) vk.vkDestroyImage(vk.device, t.image, nullptr);
    if (t.memory) vk.vkFreeMemory(vk.device, t.memory, nullptr);
    t = FakeTexture{};
  }
};

// --------------------------------------------------------------- the plugin

struct Plugin {
  void* lib = nullptr;
  void(UNITY_INTERFACE_API* UnityPluginLoad)(IUnityInterfaces*) = nullptr;
  void(UNITY_INTERFACE_API* UnityPluginUnload)() = nullptr;
  void* (*jsuiu_create)(int, int, float, int, int) = nullptr;
  JsuiRuntime* (*jsuiu_runtime)(void*) = nullptr;
  void (*jsuiu_destroy)(void*) = nullptr;
  int (*jsuiu_gpu_available)() = nullptr;
  const char* (*jsuiu_gpu_backend_name)() = nullptr;
  int (*jsuiu_gpu_failed)(void*) = nullptr;
  const char* (*jsuiu_gpu_error)(void*) = nullptr;
  void (*jsuiu_set_target)(void*, void*, int, int) = nullptr;
  int (*jsuiu_queue_render)(void*) = nullptr;
  void (*jsuiu_wait_render)(void*) = nullptr;
  void* (*jsuiu_render_event_func)() = nullptr;
  void (*jsui_config_init)(JsuiConfig*) = nullptr;
  JsuiRuntime* (*jsui_create)(const JsuiConfig*) = nullptr;
  void (*jsui_destroy)(JsuiRuntime*) = nullptr;
  int (*jsui_load_bundle_source)(JsuiRuntime*, const char*, size_t, const char*) = nullptr;
  void (*jsui_tick)(JsuiRuntime*, double) = nullptr;
  int (*jsui_needs_redraw)(JsuiRuntime*) = nullptr;
  void (*jsui_render_rgba)(JsuiRuntime*, void*, int, int, size_t) = nullptr;

  std::string load(const char* path) {
    lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) return std::string("dlopen: ") + dlerror();
    std::string missing;
    auto sym = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
      if (!fn) missing += std::string(" ") + name;
    };
    sym(UnityPluginLoad, "UnityPluginLoad");
    sym(UnityPluginUnload, "UnityPluginUnload");
    sym(jsuiu_create, "jsuiu_create");
    sym(jsuiu_runtime, "jsuiu_runtime");
    sym(jsuiu_destroy, "jsuiu_destroy");
    sym(jsuiu_gpu_available, "jsuiu_gpu_available");
    sym(jsuiu_gpu_backend_name, "jsuiu_gpu_backend_name");
    sym(jsuiu_gpu_failed, "jsuiu_gpu_failed");
    sym(jsuiu_gpu_error, "jsuiu_gpu_error");
    sym(jsuiu_set_target, "jsuiu_set_target");
    sym(jsuiu_queue_render, "jsuiu_queue_render");
    sym(jsuiu_wait_render, "jsuiu_wait_render");
    sym(jsuiu_render_event_func, "jsuiu_render_event_func");
    sym(jsui_config_init, "jsui_config_init");
    sym(jsui_create, "jsui_create");
    sym(jsui_destroy, "jsui_destroy");
    sym(jsui_load_bundle_source, "jsui_load_bundle_source");
    sym(jsui_tick, "jsui_tick");
    sym(jsui_needs_redraw, "jsui_needs_redraw");
    sym(jsui_render_rgba, "jsui_render_rgba");
    return missing.empty() ? "" : "missing exports:" + missing;
  }
};

// Two boxes in opposite corners that move every 16 ms (scattered damage)
// over a gradient backdrop, a rounded border and text (as vulkan_test.cpp).
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
    "const t = n.createNode('Text'); const s = n.createNode('RawText'); n.setText(s, 'Unity');"
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

struct Readback {
  Vk& vk;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  size_t pixels = 0;
  Readback(Vk& v, int w, int h) : vk(v), pixels(size_t(w) * h) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(pixels) * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vk.vkCreateBuffer(vk.device, &bi, nullptr, &buffer);
    VkMemoryRequirements mr;
    vk.vkGetBufferMemoryRequirements(vk.device, buffer, &mr);
    memory = vk.alloc(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk.vkBindBufferMemory(vk.device, buffer, memory, 0);
  }
  std::vector<uint32_t> read() {
    std::vector<uint32_t> px(pixels);
    void* p = nullptr;
    vk.vkMapMemory(vk.device, memory, 0, VK_WHOLE_SIZE, 0, &p);
    std::memcpy(px.data(), p, px.size() * 4);
    vk.vkUnmapMemory(vk.device, memory);
    return px;
  }
  ~Readback() {
    vk.vkDestroyBuffer(vk.device, buffer, nullptr);
    vk.vkFreeMemory(vk.device, memory, nullptr);
  }
};

// Six frames of kBundle through the plugin's GPU path, each compared with
// the CPU raster. `deferred`: AccessQueue callbacks run at the end of the
// frame on another thread. `inputAttachment`: the texture has
// INPUT_ATTACHMENT usage (Skia draws into it directly; else the copy path).
void runFrames(Plugin& p, FakeUnity& unity, bool deferred, bool inputAttachment) {
  const int W = 240, H = 160;
  std::printf("%s, %s queue access\n", inputAttachment ? "direct (input attachment)" : "copy (no input attachment)",
              deferred ? "deferred" : "synchronous");
  unity.deferQueueAccess = deferred;
  FakeTexture tex;
  CHECK(unity.makeTexture(tex, W, H,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (inputAttachment ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0)));
  Readback rb(unity.vk, W, H);

  void* view = p.jsuiu_create(W, H, 1, 0, 0);
  CHECK(view != nullptr);
  JsuiRuntime* rt = p.jsuiu_runtime(view);
  CHECK(p.jsui_load_bundle_source(rt, kBundle, std::strlen(kBundle), "unity.js") == 0);
  JsuiConfig cfg;
  p.jsui_config_init(&cfg);
  cfg.width = W;
  cfg.height = H;
  JsuiRuntime* cpu = p.jsui_create(&cfg);
  CHECK(p.jsui_load_bundle_source(cpu, kBundle, std::strlen(kBundle), "unity.js") == 0);
  std::vector<uint32_t> ref(size_t(W) * H);
  auto event = reinterpret_cast<UnityRenderingEvent>(p.jsuiu_render_event_func());
  p.jsuiu_set_target(view, &tex, W, H);

  const int accessesBefore = unity.queueAccesses;
  for (int frame = 0; frame < 6; frame++) {
    const double now = frame * 16.0 + 1;
    // Main thread (JsuiView.LateUpdate): wait for the last job, tick, queue.
    p.jsuiu_wait_render(view);
    p.jsui_tick(rt, now);
    p.jsui_tick(cpu, now);
    CHECK(p.jsuiu_gpu_failed(view) == 0);
    if (p.jsuiu_gpu_failed(view)) std::fprintf(stderr, "  GPU path failed: %s\n", p.jsuiu_gpu_error(view));
    // Unity's work earlier in the frame, still unsubmitted: JSUI's frame
    // must come after it (a full repaint covers it).
    if (frame == 0) unity.clear(tex, 1, 0, 1, 1);
    int id = p.jsuiu_queue_render(view);
    CHECK(id != 0);
    // Render thread: the two events, then Unity's use of the texture.
    event(id);
    event(id + 1);
    unity.copyOut(tex, rb.buffer);
    if (deferred) {
      // The submission thread, while the main thread waits for the job.
      std::thread submit([&] { unity.endFrame(); });
      p.jsuiu_wait_render(view);
      submit.join();
    } else {
      unity.endFrame();
    }
    unity.afterFrame();
    CHECK(!unity.misuse);
    p.jsui_render_rgba(cpu, ref.data(), W, H, size_t(W) * 4);
    std::vector<uint32_t> out = rb.read();
    double mean = 0, over = 0;
    compare(out, ref, 24, &mean, &over);
    std::printf("  frame %d: mean diff %.3f, %.2f%% of pixels off by > 24, pixel (0,0) %08x\n", frame, mean,
                over * 100, out[0]);
    if (mean > 1.0 || over > 0.01) {
      std::fprintf(stderr, "  frame %d: the plugin's frame differs from the CPU raster\n", frame);
      g_failures++;
    }
  }
  CHECK(unity.queueAccesses - accessesBefore == 6);  // one AccessQueue per job
  CHECK(tex.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  p.jsuiu_destroy(view);
  p.jsui_destroy(cpu);
  unity.freeTexture(tex);
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : JSUI_UNITY_PLUGIN_PATH;
  const char* req = std::getenv("JSUI_VULKAN_TEST_REQUIRE");
  const bool require = req && *req && std::strcmp(req, "0") != 0;
  Vk vk;
  std::string why = vk.init();
  if (!why.empty()) {
    std::printf("SKIP jsui-unity-vulkan-test: %s\n", why.c_str());
    return require ? 1 : 77;
  }
  std::printf("Vulkan device: %s\n", vk.name.c_str());
  Plugin p;
  why = p.load(path);
  if (!why.empty()) {
    std::fprintf(stderr, "%s: %s\n", path, why.c_str());
    return 1;
  }
  FakeUnity unity(vk);
  // Unity loads the plugin, which finds the device already up.
  p.UnityPluginLoad(&unity.interfaces);
  CHECK(unity.deviceCallback != nullptr);
  CHECK(p.jsuiu_gpu_available() == 1);
  const char* backend = p.jsuiu_gpu_backend_name();
  std::printf("backend: %s\n", backend ? backend : "(none)");
  CHECK(backend && std::strstr(backend, "Vulkan"));

  for (bool inputAttachment : {true, false})
    for (bool deferred : {false, true}) runFrames(p, unity, deferred, inputAttachment);

  // A texture the plugin can neither draw nor copy into: the view must fall
  // back, with a reason.
  {
    FakeTexture tex;
    CHECK(unity.makeTexture(tex, 64, 64, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
    void* view = p.jsuiu_create(64, 64, 1, 0, 0);
    p.jsui_load_bundle_source(p.jsuiu_runtime(view), kBundle, std::strlen(kBundle), "unity.js");
    p.jsuiu_set_target(view, &tex, 64, 64);
    auto event = reinterpret_cast<UnityRenderingEvent>(p.jsuiu_render_event_func());
    int id = p.jsuiu_queue_render(view);
    event(id);
    event(id + 1);
    unity.endFrame();
    unity.afterFrame();
    CHECK(p.jsuiu_gpu_failed(view) == 1);
    const char* err = p.jsuiu_gpu_error(view);
    std::printf("no INPUT_ATTACHMENT / TRANSFER_DST usage: %s\n", err ? err : "(no reason)");
    CHECK(err && std::strstr(err, "transfer destination"));
    CHECK(p.jsuiu_queue_render(view) == 0);
    p.jsuiu_destroy(view);
    unity.freeTexture(tex);
  }

  // Device shutdown frees the Skia context; then the plugin unloads.
  if (unity.deviceCallback) unity.deviceCallback(kUnityGfxDeviceEventShutdown);
  CHECK(p.jsuiu_gpu_available() == 0);
  p.UnityPluginUnload();
  CHECK(unity.deviceCallback == nullptr);
  vk.vkDeviceWaitIdle(vk.device);
  if (g_failures) {
    std::fprintf(stderr, "jsui-unity-vulkan-test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("jsui-unity-vulkan-test: ok\n");
  return 0;
}
