// jsui-wl --mir: Skia Graphite on Vulkan, presented through the Mir window.
//
// Halium's libhybris implements libvulkan.so.1 on the Android vendor driver
// (vulkan.<platform>.so, which its linker finds once /vendor/lib64/hw is on
// HYBRIS_LD_LIBRARY_PATH). With HYBRIS_VULKANPLATFORM=null it offers
// VK_KHR_android_surface, and the ANativeWindow of a Mir 1 hardware window is
// a valid surface for it. Both are read when libvulkan is loaded, which is at
// startup (wlroots links it), so jsui-session sets them. The desktop then
// goes through a Vulkan swapchain the way an Android app's does. Like the Android host (integrations/android):
// JSUI renders the desktop into a retained landscape image (only the damage is
// repainted), and one present pass (present_spv.h) draws it into the panel's
// swapchain image, rotated. Two frames are in flight: the CPU records the next
// frame while the GPU still runs the last one.
#pragma once

#ifdef JSUI_WL_MIR_VK

#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "../../android/present_spv.h"
#include "jsui/jsui.h"

// Entry points, resolved from libvulkan at run time.
#define JSUI_VK_INSTANCE_FNS(X)                                                                                  \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                           \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFeatures2)                                   \
  X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkCreateAndroidSurfaceKHR) \
  X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)   \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define JSUI_VK_DEVICE_FNS(X)                                                                                    \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueWaitIdle) X(vkCreateSwapchainKHR)          \
  X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)            \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)       \
  X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer)          \
  X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory)                        \
  X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)            \
  X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
  X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateRenderPass) X(vkDestroyRenderPass)                       \
  X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule)               \
  X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)        \
  X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)                       \
  X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateSampler)           \
  X(vkDestroySampler) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                       \
  X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw)          \
  X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer)

class MirVulkan {
 public:
  ~MirVulkan() { destroy(); }

  // `window`: the Mir buffer stream's ANativeWindow, `panelW` x `panelH` its
  // size; the desktop (`deskW` x `deskH`) is drawn into it turned by `rot`
  // degrees clockwise. False (with the reason) when any step fails; the
  // caller then destroys this and falls back.
  bool create(void* window, int panelW, int panelH, int deskW, int deskH, int rot, std::string* why) {
    window_ = window;
    panelW_ = panelW;
    panelH_ = panelH;
    rotation_ = rot / 90;
    if (!initDevice(why)) return false;
    JsuiVulkanDevice d{};
    d.instance = instance_;
    d.physical_device = physical_;
    d.device = device_;
    d.queue = queue_;
    d.queue_family = family_;
    d.api_version = VK_API_VERSION_1_1;
    d.get_instance_proc_addr = reinterpret_cast<void*>(getInstanceProcAddr_);
    d.instance_extensions = instanceExtensions_.data();
    d.instance_extension_count = int(instanceExtensions_.size());
    d.device_extensions = deviceExtensions_.data();
    d.device_extension_count = int(deviceExtensions_.size());
    d.device_features2 = &features_;
    gpu_ = jsui_gpu_create_vulkan(&d);
    if (!gpu_) {
      *why = "Skia Graphite rejected the Vulkan device";
      return false;
    }
    jsui_gpu_prepare_target(gpu_, kFrameFormat);
    VkAndroidSurfaceCreateInfoKHR asci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    asci.window = static_cast<ANativeWindow*>(window_);
    VkBool32 present = VK_FALSE;
    if (vkCreateAndroidSurfaceKHR(instance_, &asci, nullptr, &surface_) != VK_SUCCESS ||
        (vkGetPhysicalDeviceSurfaceSupportKHR(physical_, family_, surface_, &present), !present)) {
      *why = "no presentable Vulkan surface on the Mir window";
      return false;
    }
    if (!createSwapchain() || !createFrameImage(deskW, deskH)) {
      *why = "Vulkan swapchain / frame image failed";
      return false;
    }
    return true;
  }

  JsuiGpuContext* gpu() const { return gpu_; }
  const std::string& deviceName() const { return deviceName_; }
  double lastRenderMs() const { return renderMs_; }
  double lastPassMs() const { return passMs_; }
  double lastWaitMs() const { return waitMs_; }

  // Renders the frame (only the damage once the retained image holds the last
  // one) and presents it. False when the device is gone.
  bool frame(JsuiRuntime* rt) {
    if (outOfDate_ && !createSwapchain()) return false;
    const double t0 = nowMs();
    Slot& s = slots_[slot_];
    if (s.inFlight) {
      vkWaitForFences(device_, 1, &s.fence, VK_TRUE, UINT64_MAX);
      vkResetFences(device_, 1, &s.fence);
      s.inFlight = false;
    }
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, s.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      outOfDate_ = true;
      return true;  // nothing presented: the next frame recreates the swapchain
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    const double t1 = nowMs();

    JsuiGpuTarget t{};
    t.texture = frame_;
    t.format = kFrameFormat;
    t.width = frameW_;
    t.height = frameH_;
    t.flags = frameValid_ ? JSUI_GPU_INCREMENTAL : 0;
    t.usage = kFrameUsage;
    t.layout = frameValid_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    t.final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Submitted between full barriers before it returns: the pass below reads
    // the finished frame.
    frameValid_ = jsui_render_gpu_target(rt, gpu_, &t, nullptr) == 0;
    const double t2 = nowMs();

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = renderPass_;
    rp.framebuffer = framebuffers_[index];
    rp.renderArea = {{0, 0}, extent_};
    vkCmdBeginRenderPass(s.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(s.cmd, 0, 1, &viewport);
    vkCmdSetScissor(s.cmd, 0, 1, &scissor);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set_, 0, nullptr);
    int32_t rotation = rotation_;
    vkCmdPushConstants(s.cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rotation, &rotation);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    vkEndCommandBuffer(s.cmd);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &s.acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &drawn_[index];
    if (vkQueueSubmit(queue_, 1, &si, s.fence) != VK_SUCCESS) return false;
    s.inFlight = true;
    slot_ = (slot_ + 1) % kSlots;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &drawn_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    const double t3 = nowMs();
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) outOfDate_ = true;
    else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    renderMs_ = t2 - t1;
    passMs_ = t3 - t2;
    waitMs_ = (t1 - t0) + (nowMs() - t3);
    return true;
  }

  // The last frame as top-down RGBA (screenshots).
  bool readDesktop(std::vector<uint8_t>* out) {
    if (!frameValid_) return false;
    vkDeviceWaitIdle(device_);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VkDeviceSize(frameW_) * VkDeviceSize(frameH_) * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    bool ok = mai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(device_, &mai, nullptr, &mem) == VK_SUCCESS &&
              vkBindBufferMemory(device_, buf, mem, 0) == VK_SUCCESS;
    if (ok) {
      VkCommandBuffer cmd = slots_[slot_].cmd;  // idle after the wait above
      VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(cmd, &bi);
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = frame_;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {uint32_t(frameW_), uint32_t(frameH_), 1};
      vkCmdCopyImageToBuffer(cmd, frame_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
      std::swap(b.oldLayout, b.newLayout);
      std::swap(b.srcAccessMask, b.dstAccessMask);
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
      vkEndCommandBuffer(cmd);
      VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cmd;
      ok = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue_) == VK_SUCCESS;
      void* p = nullptr;
      if (ok && vkMapMemory(device_, mem, 0, bci.size, 0, &p) == VK_SUCCESS) {
        out->assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + bci.size);
        vkUnmapMemory(device_, mem);
      } else {
        ok = false;
      }
    }
    if (buf) vkDestroyBuffer(device_, buf, nullptr);
    if (mem) vkFreeMemory(device_, mem, nullptr);
    return ok;
  }

  void destroy() {
    if (device_) vkDeviceWaitIdle(device_);
    if (gpu_) jsui_gpu_destroy(gpu_);
    gpu_ = nullptr;
    if (device_) {
      destroySwapchain();
      if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
      if (frameView_) vkDestroyImageView(device_, frameView_, nullptr);
      if (frame_) vkDestroyImage(device_, frame_, nullptr);
      if (frameMemory_) vkFreeMemory(device_, frameMemory_, nullptr);
      if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
      if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
      if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
      if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
      if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
      if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
      for (Slot& s : slots_) {
        if (s.acquired) vkDestroySemaphore(device_, s.acquired, nullptr);
        if (s.fence) vkDestroyFence(device_, s.fence, nullptr);
      }
      if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
      vkDestroyDevice(device_, nullptr);
    } else if (surface_ && vkDestroySurfaceKHR) {
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_ && vkDestroyInstance) vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    // libvulkan stays loaded (the driver may keep references into it).
  }

 private:
  static constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;
  static constexpr VkImageUsageFlags kFrameUsage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  static constexpr int kSlots = 2;  // frames in flight

  static double nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
  }

  bool initDevice(std::string* why) {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      *why = "no libvulkan.so.1";
      return false;
    }
    getInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!getInstanceProcAddr_) {
      *why = "libvulkan.so.1 has no vkGetInstanceProcAddr";
      return false;
    }
    // libhybris' own exports first: they apply its window platform (the
    // driver's entry points behind vkGetInstanceProcAddr may not).
    lib_ = lib;
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(dlsym(lib, "vkCreateInstance"));
    if (!createInstance)
      createInstance = reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr_(VK_NULL_HANDLE, "vkCreateInstance"));
    instanceExtensions_ = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "jsui-wl";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(instanceExtensions_.size());
    ici.ppEnabledExtensionNames = instanceExtensions_.data();
    VkResult r = createInstance ? createInstance(&ici, nullptr, &instance_) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS) {
      *why = "vkCreateInstance failed (" + std::to_string(int(r)) +
             "; VK_KHR_android_surface needs HYBRIS_VULKANPLATFORM=null)";
      return false;
    }
#define JSUI_VK_LOAD(name)                                          \
  name = reinterpret_cast<PFN_##name>(dlsym(lib_, #name));          \
  if (!name) name = reinterpret_cast<PFN_##name>(getInstanceProcAddr_(instance_, #name));
    JSUI_VK_INSTANCE_FNS(JSUI_VK_LOAD)
#undef JSUI_VK_LOAD
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    for (VkPhysicalDevice d : devices) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(d, &props);
      if (props.apiVersion < VK_API_VERSION_1_1) continue;
      uint32_t qn = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> families(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, families.data());
      for (uint32_t i = 0; i < qn && !physical_; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          physical_ = d;
          family_ = i;
          deviceName_ = props.deviceName;
        }
      if (physical_) break;
    }
    if (!physical_) {
      *why = "no Vulkan 1.1 device with a graphics queue (the vendor driver: HYBRIS_LD_LIBRARY_PATH)";
      return false;
    }
    // Every feature the device has but robust buffer access, handed to Skia
    // as enabled.
    vkGetPhysicalDeviceFeatures2(physical_, &features_);
    features_.features.robustBufferAccess = VK_FALSE;
    deviceExtensions_ = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features_;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(deviceExtensions_.size());
    dci.ppEnabledExtensionNames = deviceExtensions_.data();
    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
      *why = "vkCreateDevice failed";
      return false;
    }
#define JSUI_VK_LOAD(name)                                          \
  name = reinterpret_cast<PFN_##name>(dlsym(lib_, #name));          \
  if (!name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name));
    JSUI_VK_DEVICE_FNS(JSUI_VK_LOAD)
#undef JSUI_VK_LOAD
    vkGetDeviceQueue(device_, family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &pool_) != VK_SUCCESS) return false;
    for (Slot& s : slots_) {
      VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool = pool_;
      cai.commandBufferCount = 1;
      cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      if (vkAllocateCommandBuffers(device_, &cai, &s.cmd) != VK_SUCCESS ||
          vkCreateFence(device_, &fci, nullptr, &s.fence) != VK_SUCCESS ||
          vkCreateSemaphore(device_, &sci, nullptr, &s.acquired) != VK_SUCCESS)
        return false;
    }

    // The present pass: sampler (texelFetch), descriptor set, layouts.
    VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.magFilter = smp.minFilter = VK_FILTER_NEAREST;
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 1;
    dsl.pBindings = &binding;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t)};
    if (vkCreateSampler(device_, &smp, nullptr, &sampler_) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(device_, &dsl, nullptr, &setLayout_) != VK_SUCCESS ||
        vkCreateDescriptorPool(device_, &dpi, nullptr, &descriptorPool_) != VK_SUCCESS)
      return false;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = descriptorPool_;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &setLayout_;
    return vkCreatePipelineLayout(device_, &pli, nullptr, &pipelineLayout_) == VK_SUCCESS &&
           vkAllocateDescriptorSets(device_, &dsa, &set_) == VK_SUCCESS;
  }

  uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
  }

  VkShaderModule shaderModule(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &ci, nullptr, &m);
    return m;
  }

  bool createPresentPass(VkFormat format) {
    if (passFormat_ == format && pipeline_) return true;
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if (vkCreateRenderPass(device_, &rpi, nullptr, &renderPass_) != VK_SUCCESS) return false;
    VkShaderModule vert = shaderModule(kPresentVert, sizeof kPresentVert);
    VkShaderModule frag = shaderModule(kPresentFrag, sizeof kPresentFrag);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = pipelineLayout_;
    gp.renderPass = renderPass_;
    VkResult r = vert && frag ? vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_)
                              : VK_ERROR_INITIALIZATION_FAILED;
    if (vert) vkDestroyShaderModule(device_, vert, nullptr);
    if (frag) vkDestroyShaderModule(device_, frag, nullptr);
    if (r != VK_SUCCESS) return false;
    passFormat_ = format;
    return true;
  }

  void destroySwapchain() {
    for (Slot& s : slots_)
      if (s.inFlight) {
        vkWaitForFences(device_, 1, &s.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &s.fence);
        s.inFlight = false;
      }
    vkDeviceWaitIdle(device_);
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(device_, f, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : drawn_) vkDestroySemaphore(device_, s, nullptr);
    framebuffers_.clear();
    views_.clear();
    drawn_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }

  // The panel's swapchain (not pre-rotated: the present pass turns the
  // desktop), its views, framebuffers and per-image "drawn" semaphores.
  bool createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS) return false;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
    VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const VkSurfaceFormatKHR& f : formats)
      if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
          (format.format == VK_FORMAT_UNDEFINED || f.format == kFrameFormat))
        format = f;
    if (format.format == VK_FORMAT_UNDEFINED || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) extent = {uint32_t(panelW_), uint32_t(panelH_)};
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    // Three images: one shown, one queued, one being drawn.
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = format.format;
    sci.imageColorSpace = format.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                             ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                             : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain);
    destroySwapchain();
    if (r != VK_SUCCESS || !createPresentPass(format.format)) {
      if (swapchain) vkDestroySwapchainKHR(device_, swapchain, nullptr);
      return false;
    }
    swapchain_ = swapchain;
    extent_ = extent;
    outOfDate_ = false;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images.data());
    for (VkImage image : images) {
      VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vci.image = image;
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.format = format.format;
      vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view = VK_NULL_HANDLE;
      if (vkCreateImageView(device_, &vci, nullptr, &view) != VK_SUCCESS) return false;
      views_.push_back(view);
      VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fci.renderPass = renderPass_;
      fci.attachmentCount = 1;
      fci.pAttachments = &view;
      fci.width = extent.width;
      fci.height = extent.height;
      fci.layers = 1;
      VkFramebuffer fb = VK_NULL_HANDLE;
      if (vkCreateFramebuffer(device_, &fci, nullptr, &fb) != VK_SUCCESS) return false;
      framebuffers_.push_back(fb);
      VkSemaphoreCreateInfo ssi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore drawn = VK_NULL_HANDLE;
      if (vkCreateSemaphore(device_, &ssi, nullptr, &drawn) != VK_SUCCESS) return false;
      drawn_.push_back(drawn);
    }
    return true;
  }

  // The retained landscape frame JSUI renders into, and its view in the
  // present pass' descriptor set.
  bool createFrameImage(int w, int h) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kFrameFormat;
    ici.extent = {uint32_t(w), uint32_t(h), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = kFrameUsage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &frame_) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, frame_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = kFrameFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &frameMemory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, frame_, frameMemory_, 0) != VK_SUCCESS ||
        (vci.image = frame_, vkCreateImageView(device_, &vci, nullptr, &frameView_)) != VK_SUCCESS)
      return false;
    frameW_ = w;
    frameH_ = h;
    frameValid_ = false;
    VkDescriptorImageInfo info{sampler_, frameView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
  }

#define JSUI_VK_DECLARE(name) PFN_##name name = nullptr;
  JSUI_VK_INSTANCE_FNS(JSUI_VK_DECLARE)
  JSUI_VK_DEVICE_FNS(JSUI_VK_DECLARE)
#undef JSUI_VK_DECLARE

  struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    bool inFlight = false;
  };

  void* window_ = nullptr;
  int panelW_ = 0, panelH_ = 0, rotation_ = 0;
  void* lib_ = nullptr;  // libvulkan.so.1
  PFN_vkGetInstanceProcAddr getInstanceProcAddr_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  std::string deviceName_;
  VkPhysicalDeviceFeatures2 features_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  std::vector<const char*> instanceExtensions_, deviceExtensions_;
  JsuiGpuContext* gpu_ = nullptr;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  Slot slots_[kSlots];
  int slot_ = 0;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkFormat passFormat_ = VK_FORMAT_UNDEFINED;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
  std::vector<VkSemaphore> drawn_;  // per swapchain image
  bool outOfDate_ = false;
  VkImage frame_ = VK_NULL_HANDLE;
  VkDeviceMemory frameMemory_ = VK_NULL_HANDLE;
  VkImageView frameView_ = VK_NULL_HANDLE;
  int frameW_ = 0, frameH_ = 0;
  bool frameValid_ = false;
  double renderMs_ = 0, passMs_ = 0, waitMs_ = 0;
};

#endif  // JSUI_WL_MIR_VK
