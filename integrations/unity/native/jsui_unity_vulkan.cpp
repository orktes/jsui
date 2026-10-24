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
//     COLOR_ATTACHMENT_OPTIMAL + colour-attachment write access and
//     kUnityVulkanResourceAccess_PipelineBarrier. That records, into Unity's
//     current command buffer, the barrier and layout transition from however
//     Unity last used the texture (e.g. sampled it last frame) to what the
//     render needs, and makes Unity's tracking note that the image is in
//     COLOR_ATTACHMENT_OPTIMAL and was written as a colour attachment. It
//     returns the VkImage, its format, usage and layout. Then it asks for
//     queue access: AccessQueue(callback, flush = true), whose callback runs
//     once every command buffer Unity recorded so far (the barrier
//     included) has been submitted, while no Unity thread uses the queue.
//  2. the callback (render thread or Unity's submission thread) renders:
//     JSUI's submission ([full barrier] [Skia] [full barrier], see
//     src/render/gpu_graphite.cpp) goes onto the queue after Unity's work up
//     to this point and leaves the image in COLOR_ATTACHMENT_OPTIMAL, the
//     layout Unity's tracking expects. Whatever Unity records afterwards
//     (the canvas sampling the texture) is submitted later, so the queue
//     order and the trailing full barrier make it see the finished pixels;
//     Unity itself adds the COLOR_ATTACHMENT -> SHADER_READ_ONLY transition
//     it needs, from the access its tracking recorded in step 1.
//  3. the render event (the view's second plugin event) has nothing left
//     to do.
//
// The callback may run later than the event that asked for it (Unity says
// it "may or may not be called synchronously"); the job is done when it has
// run (jsuiu_job_done), which is what the main thread waits for before it
// touches the runtime again (jsui_unity.cpp). The Skia context is created
// in the first callback and only used from callbacks (serialised by a mutex
// in case Unity calls them on different threads); it goes with the device.
//
// Graphite renders only into images with COLOR_ATTACHMENT and
// INPUT_ATTACHMENT usage; Unity's RenderTextures have both (the job fails,
// and the view falls back to the CPU path, if one does not).
#define VK_NO_PROTOTYPES 1
#include "include/third_party/vulkan/vulkan/vulkan_core.h"

#define JSUI_UNITY_API_VULKAN 1
#include <mutex>

#include "jsui/jsui.h"
#include "jsui_unity_api.h"
#include "jsui_unity_gpu.h"

namespace {

IUnityGraphicsVulkanV2* g_vulkan = nullptr;  // or an IUnityGraphicsVulkan (same prefix)
UnityVulkanInstance g_instance{};
std::mutex g_mutex;  // the Skia context
JsuiGpuContext* g_gpu = nullptr;
bool g_createFailed = false;

constexpr VkImageLayout kLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

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
  t.texture = job->image;
  t.format = job->format;
  t.usage = job->usage;
  t.width = job->width;
  t.height = job->height;
  t.layout = job->layout;
  t.final_layout = kLayout;
  t.flags = job->incremental ? JSUI_GPU_INCREMENTAL : 0;
  JsuiRect dirty;
  const bool ok = jsui_render_gpu_target(job->rt, g_gpu, &t, &dirty) == 0;
  if (!ok) job->error = "Skia Graphite failed to render into the RenderTexture (Vulkan)";
  jsuiu_job_done(job, ok);
}

void prepare(JsuiUnityJob* job) {
  g_vulkan->EnsureOutsideRenderPass();
  UnityVulkanImage image{};
  if (!g_vulkan->AccessTexture(job->texture, nullptr, kLayout, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               kUnityVulkanResourceAccess_PipelineBarrier, &image) ||
      image.image == VK_NULL_HANDLE) {
    job->error = "IUnityGraphicsVulkan::AccessTexture rejected the RenderTexture";
    jsuiu_job_done(job, false);
    return;
  }
  constexpr VkImageUsageFlags kNeeded = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  if ((image.usage & kNeeded) != kNeeded) {
    job->error = "the RenderTexture's VkImage lacks COLOR_ATTACHMENT / INPUT_ATTACHMENT usage (Skia Graphite needs both)";
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

void shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_gpu) jsui_gpu_destroy(g_gpu);
  g_gpu = nullptr;
  g_createFailed = false;
  g_vulkan = nullptr;
}

const JsuiUnityBackend kVulkan = {"Skia Graphite (Vulkan)", prepare, render, shutdown};

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
