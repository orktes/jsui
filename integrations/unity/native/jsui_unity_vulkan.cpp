// jsui_unity's Vulkan backend (jsui_unity_gpu.h): Skia Graphite on Unity's
// own VkDevice and graphics VkQueue (IUnityGraphicsVulkanV2::Instance), into
// the view's RenderTexture.
//
// Ordering. Unity records a frame into command buffers that it submits
// later (at the end of the frame, or from its submission thread), while
// jsui_render_gpu_target submits its work to the queue before it returns.
// Rendering straight from a plugin event would therefore put JSUI's work on
// the queue *before* what Unity recorded earlier in the frame, and Unity may
// be using the queue from another thread at that moment. So a job is split:
//
//  1. prepare (the view's first plugin event, render thread): ends Unity's
//     render pass (EnsureOutsideRenderPass) and calls AccessTexture with
//     kUnityVulkanResourceAccess_PipelineBarrier and the layout / access the
//     render needs (below). That records, into Unity's current command
//     buffer, the barrier and layout transition from however Unity last used
//     the texture (e.g. sampled it last frame), and makes Unity's tracking
//     note the new layout and access. It returns the VkImage, its format,
//     usage and layout. Then it asks for queue access: AccessQueue(callback,
//     flush = true), whose callback runs once every command buffer Unity
//     recorded so far (the barrier included) has been submitted, while no
//     Unity thread uses the queue.
//  2. the callback (render thread or Unity's submission thread) renders:
//     JSUI's submission ([full barrier] [Skia] [full barrier], see
//     src/render/gpu_graphite.cpp) goes onto the queue after Unity's work up
//     to this point and leaves the image in the layout Unity's tracking
//     expects. Whatever Unity records afterwards (the canvas sampling the
//     texture) is submitted later, so the queue order and the trailing full
//     barrier make it see the finished pixels; Unity itself adds the
//     transition it needs, from the access its tracking recorded in step 1.
//  3. the render event (the view's second plugin event) has nothing left
//     to do.
//
// Two ways to draw, by the texture's usage:
//
//  - direct: Graphite renders only into images with COLOR_ATTACHMENT and
//    INPUT_ATTACHMENT usage. When Unity's image has both, AccessTexture asks
//    for COLOR_ATTACHMENT_OPTIMAL + colour-attachment access and Skia draws
//    into it, leaving it in that layout.
//  - copy: otherwise (Unity does not create every RenderTexture as an input
//    attachment) Skia draws into an image of the plugin's own (same format,
//    colour + input attachment + transfer src; one per view and size,
//    retained between frames so incremental repaints work) and, in the same
//    callback after the render, a command buffer of the plugin's copies the
//    repainted rect (the whole image when Unity's does not hold the last
//    frame) into Unity's image. AccessTexture asked for
//    TRANSFER_DST_OPTIMAL + transfer write, so that is the layout Unity's
//    tracking has and the copy leaves it in; the render's trailing full
//    barrier orders the copy after Skia's work. Plugin images are destroyed
//    (after their last copy's fence) when the view goes, changes size, or
//    with the device.
//
// The callback may run later than the event that asked for it (Unity says
// it "may or may not be called synchronously"); the job is done when it has
// run (jsuiu_job_done), which is what the main thread waits for before it
// touches the runtime again (jsui_unity.cpp). The Skia context is created
// in the first callback and only used from callbacks (serialised by a mutex
// in case Unity calls them on different threads); it goes with the device.
#define VK_NO_PROTOTYPES 1
#include "include/third_party/vulkan/vulkan/vulkan_core.h"

#define JSUI_UNITY_API_VULKAN 1
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "jsui/jsui.h"
#include "jsui_unity_api.h"
#include "jsui_unity_gpu.h"

namespace {

IUnityGraphicsVulkanV2* g_vulkan = nullptr;  // or an IUnityGraphicsVulkan (same prefix)
UnityVulkanInstance g_instance{};
std::mutex g_mutex;  // the Skia context, the plugin images, the copy pool
JsuiGpuContext* g_gpu = nullptr;
bool g_createFailed = false;

constexpr VkImageLayout kDirectLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
constexpr VkImageLayout kCopyLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
constexpr VkImageUsageFlags kGraphiteUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

// ------------------------------------------------- the copy path's Vulkan

struct Fns {
  PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties = nullptr;
#define JSUI_VK_DEVICE_FNS(X)       \
  X(vkCreateImage)                  \
  X(vkDestroyImage)                 \
  X(vkGetImageMemoryRequirements)   \
  X(vkAllocateMemory)               \
  X(vkFreeMemory)                   \
  X(vkBindImageMemory)              \
  X(vkCreateCommandPool)            \
  X(vkDestroyCommandPool)           \
  X(vkAllocateCommandBuffers)       \
  X(vkFreeCommandBuffers)           \
  X(vkResetCommandBuffer)           \
  X(vkBeginCommandBuffer)           \
  X(vkEndCommandBuffer)             \
  X(vkCmdPipelineBarrier)           \
  X(vkCmdCopyImage)                 \
  X(vkCreateFence)                  \
  X(vkDestroyFence)                 \
  X(vkWaitForFences)                \
  X(vkResetFences)                  \
  X(vkQueueSubmit)
#define X(f) PFN_##f f = nullptr;
  JSUI_VK_DEVICE_FNS(X)
#undef X
  bool ok = false;
} g_vk;

bool loadFns() {
  if (g_vk.ok) return true;
  PFN_vkGetInstanceProcAddr gipa = g_instance.getInstanceProcAddr;
  if (!gipa) return false;
  g_vk.getMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      gipa(g_instance.instance, "vkGetPhysicalDeviceMemoryProperties"));
  auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(g_instance.instance, "vkGetDeviceProcAddr"));
  if (!g_vk.getMemoryProperties || !gdpa) return false;
  bool all = true;
#define X(f)                                                              \
  g_vk.f = reinterpret_cast<PFN_##f>(gdpa(g_instance.device, #f)); \
  all = all && g_vk.f;
  JSUI_VK_DEVICE_FNS(X)
#undef X
  g_vk.ok = all;
  return all;
}

// A view's own render target on the copy path, and the command buffer that
// copies it into Unity's image.
struct Owned {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  int width = 0, height = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool holds = false;  // holds the view's last frame
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;  // the last copy's
  bool submitted = false;
};

VkCommandPool g_pool = VK_NULL_HANDLE;
std::unordered_map<void*, Owned> g_owned;  // by view
std::vector<Owned> g_retired;               // freed once their last copy is done

void destroy(Owned& o) {
  VkDevice d = g_instance.device;
  if (o.fence) {
    if (o.submitted) g_vk.vkWaitForFences(d, 1, &o.fence, VK_TRUE, UINT64_MAX);
    g_vk.vkDestroyFence(d, o.fence, nullptr);
  }
  if (o.cb) g_vk.vkFreeCommandBuffers(d, g_pool, 1, &o.cb);
  if (o.image) g_vk.vkDestroyImage(d, o.image, nullptr);
  if (o.memory) g_vk.vkFreeMemory(d, o.memory, nullptr);
  o = Owned{};
}

// Frees retired images (their copies were submitted before anything that is
// pending now, and nothing reads them afterwards).
void reap() {
  for (Owned& o : g_retired) destroy(o);
  g_retired.clear();
}

VkDeviceMemory allocate(const VkMemoryRequirements& mr) {
  VkPhysicalDeviceMemoryProperties mp;
  g_vk.getMemoryProperties(g_instance.physicalDevice, &mp);
  for (VkMemoryPropertyFlags want : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), VkMemoryPropertyFlags(0)})
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = i;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (g_vk.vkAllocateMemory(g_instance.device, &ai, nullptr, &m) == VK_SUCCESS) return m;
      }
  return VK_NULL_HANDLE;
}

// The view's plugin image of this size and format (a new one, not holding
// anything, when it changed). NULL on failure.
Owned* ownedFor(void* view, int w, int h, VkFormat format) {
  if (!loadFns()) return nullptr;
  VkDevice d = g_instance.device;
  if (!g_pool) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_instance.queueFamilyIndex;
    if (g_vk.vkCreateCommandPool(d, &pci, nullptr, &g_pool) != VK_SUCCESS) return nullptr;
  }
  Owned& o = g_owned[view];
  if (o.image && o.width == w && o.height == h && o.format == format) return &o;
  if (o.image) {
    g_retired.push_back(o);
    o = Owned{};
  }
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {uint32_t(w), uint32_t(h), 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = kGraphiteUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool ok = g_vk.vkCreateImage(d, &ii, nullptr, &o.image) == VK_SUCCESS;
  if (ok) {
    VkMemoryRequirements mr;
    g_vk.vkGetImageMemoryRequirements(d, o.image, &mr);
    o.memory = allocate(mr);
    ok = o.memory && g_vk.vkBindImageMemory(d, o.image, o.memory, 0) == VK_SUCCESS;
  }
  if (ok) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ok = g_vk.vkAllocateCommandBuffers(d, &ai, &o.cb) == VK_SUCCESS &&
         g_vk.vkCreateFence(d, &fi, nullptr, &o.fence) == VK_SUCCESS;
  }
  if (!ok) {
    destroy(o);
    g_owned.erase(view);
    return nullptr;
  }
  o.width = w;
  o.height = h;
  o.format = format;
  return &o;
}

// Copies `r` of the plugin image (in TRANSFER_SRC_OPTIMAL, where the render
// left it) into Unity's image (in TRANSFER_DST_OPTIMAL, from AccessTexture).
// The render's trailing full barrier orders this after Skia's work; what
// Unity submits later is ordered by Unity's own barrier from the transfer
// write its tracking recorded.
bool copyToUnity(Owned& o, VkImage dst, const JsuiRect& r) {
  VkDevice d = g_instance.device;
  if (o.submitted) {
    g_vk.vkWaitForFences(d, 1, &o.fence, VK_TRUE, UINT64_MAX);  // the last copy (long done)
    g_vk.vkResetFences(d, 1, &o.fence);
    o.submitted = false;
  }
  g_vk.vkResetCommandBuffer(o.cb, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (g_vk.vkBeginCommandBuffer(o.cb, &bi) != VK_SUCCESS) return false;
  VkImageCopy c{};
  c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.dstSubresource = c.srcSubresource;
  c.srcOffset = c.dstOffset = {r.x, r.y, 0};
  c.extent = {uint32_t(r.width), uint32_t(r.height), 1};
  g_vk.vkCmdCopyImage(o.cb, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, kCopyLayout, 1, &c);
  if (g_vk.vkEndCommandBuffer(o.cb) != VK_SUCCESS) return false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &o.cb;
  if (g_vk.vkQueueSubmit(g_instance.graphicsQueue, 1, &si, o.fence) != VK_SUCCESS) return false;
  o.submitted = true;
  return true;
}

// ------------------------------------------------------------------ jobs

void UNITY_INTERFACE_API renderOnQueue(int, void* data) {
  auto* job = static_cast<JsuiUnityJob*>(data);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_gpu && !g_createFailed) {
    JsuiVulkanDevice d{};
    d.instance = g_instance.instance;
    d.physical_device = g_instance.physicalDevice;
    d.device = g_instance.device;
    d.queue = g_instance.graphicsQueue;
    d.queue_family = g_instance.queueFamilyIndex;
    d.get_instance_proc_addr = reinterpret_cast<void*>(g_instance.getInstanceProcAddr);
    g_gpu = jsui_gpu_create_vulkan(&d);
    g_createFailed = !g_gpu;
  }
  if (!g_gpu) {
    job->error = "no Skia Graphite context on Unity's Vulkan device";
    jsuiu_gpu_disable();
    jsuiu_job_done(job, false);
    return;
  }
  JsuiGpuTarget t{};
  t.width = job->width;
  t.height = job->height;
  t.format = job->format;
  JsuiRect dirty{0, 0, 0, 0};
  bool ok;
  if (!job->copy) {
    t.texture = job->image;
    t.usage = job->usage;
    t.layout = job->layout;
    t.final_layout = kDirectLayout;
    t.flags = job->incremental ? JSUI_GPU_INCREMENTAL : 0;
    ok = jsui_render_gpu_target(job->rt, g_gpu, &t, &dirty) == 0;
  } else {
    reap();
    Owned* o = ownedFor(job->owner, job->width, job->height, VkFormat(job->format));
    if (!o) {
      job->error = "could not create the plugin's Vulkan render target (copy path)";
      jsuiu_job_done(job, false);
      return;
    }
    t.texture = o->image;
    t.usage = kGraphiteUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    t.layout = o->layout;
    t.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    t.flags = o->holds ? JSUI_GPU_INCREMENTAL : 0;
    ok = jsui_render_gpu_target(job->rt, g_gpu, &t, &dirty) == 0;
    if (ok) {
      o->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      const bool hadFrame = o->holds;
      o->holds = true;
      // Unity's image gets just the repainted rect when it holds the frame
      // before; the whole image otherwise (a new texture, the plugin
      // image's first frame).
      JsuiRect r = dirty;
      if (!job->incremental || !hadFrame) r = JsuiRect{0, 0, job->width, job->height};
      r.width = std::min(r.width, job->width - r.x);
      r.height = std::min(r.height, job->height - r.y);
      if (r.width > 0 && r.height > 0) ok = copyToUnity(*o, static_cast<VkImage>(job->image), r);
    } else {
      o->holds = false;
      o->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
  }
  if (!ok) job->error = "Skia Graphite failed to render into the RenderTexture (Vulkan)";
  jsuiu_job_done(job, ok);
}

void prepare(JsuiUnityJob* job) {
  g_vulkan->EnsureOutsideRenderPass();
  // What the texture can be: its usage, from an observe-only access first.
  UnityVulkanImage image{};
  if (!g_vulkan->AccessTexture(job->texture, nullptr, VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
                               kUnityVulkanResourceAccess_ObserveOnly, &image) ||
      image.image == VK_NULL_HANDLE) {
    job->error = "IUnityGraphicsVulkan::AccessTexture rejected the RenderTexture";
    jsuiu_job_done(job, false);
    return;
  }
  job->copy = (image.usage & kGraphiteUsage) != kGraphiteUsage;
  if (job->copy && !(image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    job->error = "the RenderTexture's VkImage is neither a colour + input attachment nor a transfer destination";
    jsuiu_job_done(job, false);
    return;
  }
  const bool barrier = job->copy
      ? g_vulkan->AccessTexture(job->texture, nullptr, kCopyLayout, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT, kUnityVulkanResourceAccess_PipelineBarrier, &image)
      : g_vulkan->AccessTexture(job->texture, nullptr, kDirectLayout, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                kUnityVulkanResourceAccess_PipelineBarrier, &image);
  if (!barrier || image.image == VK_NULL_HANDLE) {
    job->error = "IUnityGraphicsVulkan::AccessTexture rejected the RenderTexture";
    jsuiu_job_done(job, false);
    return;
  }
  job->image = image.image;
  job->format = unsigned(image.format);
  job->usage = unsigned(image.usage);
  job->layout = int(image.layout);
  job->prepared = true;
  g_vulkan->AccessQueue(renderOnQueue, job->eventId, job, true);
}

void render(JsuiUnityJob*) {}  // done in renderOnQueue

void forget(void* view) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_owned.find(view);
  if (it == g_owned.end()) return;
  g_retired.push_back(it->second);
  g_owned.erase(it);
}

void shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_vk.ok) {
    for (auto& kv : g_owned) g_retired.push_back(kv.second);
    g_owned.clear();
    reap();
    if (g_pool) g_vk.vkDestroyCommandPool(g_instance.device, g_pool, nullptr);
  }
  g_pool = VK_NULL_HANDLE;
  g_vk = Fns{};
  if (g_gpu) jsui_gpu_destroy(g_gpu);
  g_gpu = nullptr;
  g_createFailed = false;
  g_vulkan = nullptr;
}

const JsuiUnityBackend kVulkan = {"Skia Graphite (Vulkan)", prepare, render, shutdown, forget};

}  // namespace

const JsuiUnityBackend* jsuiu_vulkan_backend(IUnityInterfaces* unity) {
  g_vulkan = unity->Get<IUnityGraphicsVulkanV2>();
  if (!g_vulkan) g_vulkan = reinterpret_cast<IUnityGraphicsVulkanV2*>(unity->Get<IUnityGraphicsVulkan>());
  if (!g_vulkan) return nullptr;
  g_instance = g_vulkan->Instance();
  if (!g_instance.device || !g_instance.graphicsQueue) {
    g_vulkan = nullptr;
    return nullptr;
  }
  return &kVulkan;
}
