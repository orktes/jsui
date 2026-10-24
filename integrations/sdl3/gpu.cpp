// The OpenGL and Vulkan GPU paths of the SDL3 example (see gpu.h) and the
// renderer choice.
//
// OpenGL (SDL's "opengl" driver, Skia Ganesh). The UI texture is an ordinary
// SDL texture; SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER is its GL_TEXTURE_2D,
// which jsui_render_gl_target draws into (Ganesh keeps the last frame in its
// own texture and repaints only the damage, so the target need not keep its
// contents). SDL_FlushRenderer first runs SDL's queued draws and marks its
// state cache dirty, so SDL sets its own state again afterwards. It does not
// re-bind everything it assumes, though: SDL's GL renderer draws from client
// vertex arrays into framebuffer 0, so after Skia the bindings it relies on
// are reset (restoreSdlState).
//
// Vulkan (SDL's "vulkan" driver, Skia Graphite). SDL creates its instance at
// API 1.0 and Graphite needs 1.1, so createRenderer makes the instance and
// SDL makes the device on it. The UI texture wraps a VkImage of our own
// (SDL_PROP_TEXTURE_CREATE_VULKAN_TEXTURE_NUMBER): SDL's images lack the
// input-attachment usage Graphite renders with. SDL records each image's
// layout and keeps sampled textures in SHADER_READ_ONLY_OPTIMAL; every render
// enters and leaves the image in that layout, so SDL's record stays right.
// The render goes to SDL's own graphics queue (vkGetDeviceQueue(family, 0)
// is the queue SDL took) after SDL_FlushRenderer submitted SDL's work, and
// jsui's full barriers order it after SDL's earlier frames (which may still be
// sampling the image) and before the frame that draws it. All on this
// thread, so the queue needs no extra locking.
#include "gpu.h"

#include <map>

#if defined(JSUI_VULKAN) && !defined(__APPLE__)
#define SDL3_GPU_VULKAN 1
#define VK_NO_PROTOTYPES
#include <SDL3/SDL_vulkan.h>

#include "vulkan/vulkan.h"
#endif

namespace sdl3_gpu {

namespace {

// ------------------------------------------------------------- OpenGL

#if defined(JSUI_GPU)

// The GL entry points restoreSdlState needs (SDL's GL context, 2.1+).
struct GlFns {
  void (*bindFramebuffer)(unsigned, unsigned) = nullptr;
  void (*bindBuffer)(unsigned, unsigned) = nullptr;
  void (*bindVertexArray)(unsigned) = nullptr;  // GL 3.0+ / ARB_vertex_array_object
  void (*useProgram)(unsigned) = nullptr;
  void (*disableVertexAttribArray)(unsigned) = nullptr;
  void (*activeTexture)(unsigned) = nullptr;
  void (*bindTexture)(unsigned, unsigned) = nullptr;
  void (*disable)(unsigned) = nullptr;
  void (*pixelStorei)(unsigned, int) = nullptr;
  const unsigned char* (*getString)(unsigned) = nullptr;

  bool load() {
    auto get = [](const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); };
    bindFramebuffer = reinterpret_cast<decltype(bindFramebuffer)>(get("glBindFramebuffer"));
    if (!bindFramebuffer) bindFramebuffer = reinterpret_cast<decltype(bindFramebuffer)>(get("glBindFramebufferEXT"));
    bindBuffer = reinterpret_cast<decltype(bindBuffer)>(get("glBindBuffer"));
    bindVertexArray = reinterpret_cast<decltype(bindVertexArray)>(get("glBindVertexArray"));
    useProgram = reinterpret_cast<decltype(useProgram)>(get("glUseProgram"));
    disableVertexAttribArray = reinterpret_cast<decltype(disableVertexAttribArray)>(get("glDisableVertexAttribArray"));
    activeTexture = reinterpret_cast<decltype(activeTexture)>(get("glActiveTexture"));
    bindTexture = reinterpret_cast<decltype(bindTexture)>(get("glBindTexture"));
    disable = reinterpret_cast<decltype(disable)>(get("glDisable"));
    pixelStorei = reinterpret_cast<decltype(pixelStorei)>(get("glPixelStorei"));
    getString = reinterpret_cast<decltype(getString)>(get("glGetString"));
    return bindFramebuffer && bindBuffer && useProgram && disableVertexAttribArray && activeTexture && bindTexture &&
           disable && pixelStorei && getString;
  }
};

constexpr unsigned kGlTexture2D = 0x0DE1, kGlRgba8 = 0x8058, kGlFramebuffer = 0x8D40, kGlArrayBuffer = 0x8892,
                   kGlElementArrayBuffer = 0x8893, kGlPixelUnpackBuffer = 0x88EC, kGlTexture0 = 0x84C0,
                   kGlScissorTest = 0x0C11, kGlStencilTest = 0x0B90, kGlDepthTest = 0x0B71, kGlCullFace = 0x0B44,
                   kGlUnpackRowLength = 0x0CF2, kGlUnpackAlignment = 0x0CF5, kGlRenderer = 0x1F01,
                   kGlVersion = 0x1F02;

class GlTarget : public UiTarget {
 public:
  GlTarget(SDL_Renderer* r, JsuiGpuContext* gpu, const GlFns& gl) : renderer_(r), gpu_(gpu), gl_(gl) {}
  ~GlTarget() override {
    if (texture_) SDL_DestroyTexture(texture_);
    SDL_FlushRenderer(renderer_);  // SDL's GL context current
    jsui_gpu_destroy(gpu_);
    restoreSdlState();
  }

  std::string describe() const override { return "Skia Ganesh GL"; }
  std::string device() const override {
    const unsigned char* r = gl_.getString(kGlRenderer);
    const unsigned char* v = gl_.getString(kGlVersion);
    return std::string(r ? (const char*)r : "?") + " / " + (v ? (const char*)v : "?");
  }

  bool resize(int width, int height) override {
    if (texture_) SDL_DestroyTexture(texture_);
    // RGBA8 (GL_RGBA8), premultiplied, as Skia paints it.
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture_) return false;
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
    SDL_PropertiesID p = SDL_GetTextureProperties(texture_);
    name_ = unsigned(SDL_GetNumberProperty(p, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0));
    // A GL_TEXTURE_2D of exactly this size (no power-of-two padding).
    if (!name_ || SDL_GetNumberProperty(p, SDL_PROP_TEXTURE_OPENGL_TEXTURE_TARGET_NUMBER, 0) != kGlTexture2D ||
        SDL_GetFloatProperty(p, SDL_PROP_TEXTURE_OPENGL_TEX_W_FLOAT, 1) != 1.f) {
      return false;
    }
    width_ = width;
    height_ = height;
    return true;
  }

  SDL_Texture* texture() const override { return texture_; }

  bool paint(JsuiRuntime* rt, bool force) override {
    if (!force && !jsui_needs_redraw(rt)) return true;
    SDL_FlushRenderer(renderer_);
    JsuiGLTarget t{};
    t.texture = name_;
    t.format = kGlRgba8;
    t.width = width_;
    t.height = height_;
    t.flags = JSUI_GL_TOP_LEFT_ORIGIN;  // SDL samples row 0 at the top
    int rc = jsui_render_gl_target(rt, gpu_, &t);
    restoreSdlState();
    return rc == 0;
  }

 private:
  // What SDL's GL renderer assumes between its own calls (it rebinds its
  // texture, shader, blend, viewport and scissor itself after the flush).
  void restoreSdlState() {
    gl_.bindFramebuffer(kGlFramebuffer, 0);
    if (gl_.bindVertexArray) gl_.bindVertexArray(0);
    gl_.bindBuffer(kGlArrayBuffer, 0);
    gl_.bindBuffer(kGlElementArrayBuffer, 0);
    gl_.bindBuffer(kGlPixelUnpackBuffer, 0);
    for (unsigned i = 0; i < 8; i++) gl_.disableVertexAttribArray(i);
    gl_.useProgram(0);
    gl_.activeTexture(kGlTexture0);
    gl_.bindTexture(kGlTexture2D, 0);
    gl_.disable(kGlScissorTest);
    gl_.disable(kGlStencilTest);
    gl_.disable(kGlDepthTest);
    gl_.disable(kGlCullFace);
    gl_.pixelStorei(kGlUnpackRowLength, 0);
    gl_.pixelStorei(kGlUnpackAlignment, 4);
  }

  SDL_Renderer* renderer_;
  JsuiGpuContext* gpu_;
  GlFns gl_;
  SDL_Texture* texture_ = nullptr;
  unsigned name_ = 0;
  int width_ = 0, height_ = 0;
};

std::unique_ptr<UiTarget> createGlTarget(SDL_Renderer* renderer) {
  SDL_FlushRenderer(renderer);  // makes SDL's GL context current
  if (!SDL_GL_GetCurrentContext()) return nullptr;
  GlFns gl;
  if (!gl.load()) return nullptr;
  JsuiGpuContext* gpu = jsui_gpu_create_gl(
      [](void*, const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }, nullptr);
  if (!gpu) return nullptr;
  return std::make_unique<GlTarget>(renderer, gpu, gl);
}

#endif  // JSUI_GPU

// ------------------------------------------------------------- Vulkan

#if SDL3_GPU_VULKAN

// Instances createRenderer made, destroyed after their renderer.
std::map<SDL_Renderer*, VkInstance> g_instances;

PFN_vkGetInstanceProcAddr instanceProcAddr() {
  return reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
}

VkInstance createInstance() {
  if (!SDL_Vulkan_LoadLibrary(nullptr)) return VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr gipa = instanceProcAddr();
  auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
  Uint32 count = 0;
  const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
  if (!create || !extensions) return VK_NULL_HANDLE;
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "jsui-sdl3";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = count;
  info.ppEnabledExtensionNames = extensions;
  VkInstance instance = VK_NULL_HANDLE;
  if (create(&info, nullptr, &instance) != VK_SUCCESS) return VK_NULL_HANDLE;
  return instance;
}

void destroyInstance(VkInstance instance) {
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(instanceProcAddr()(instance, "vkDestroyInstance"));
  if (destroy) destroy(instance, nullptr);
}

constexpr VkImageUsageFlags kImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;

class VulkanTarget : public UiTarget {
 public:
  VulkanTarget(SDL_Renderer* r, JsuiGpuContext* gpu, VkPhysicalDevice phys, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
               VkInstance instance)
      : renderer_(r), gpu_(gpu), phys_(phys), device_(device) {
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
#define SDL3_GPU_DEVICE_FN(name) name##_ = reinterpret_cast<PFN_##name>(gdpa(device, #name))
    SDL3_GPU_DEVICE_FN(vkCreateImage);
    SDL3_GPU_DEVICE_FN(vkDestroyImage);
    SDL3_GPU_DEVICE_FN(vkGetImageMemoryRequirements);
    SDL3_GPU_DEVICE_FN(vkAllocateMemory);
    SDL3_GPU_DEVICE_FN(vkFreeMemory);
    SDL3_GPU_DEVICE_FN(vkBindImageMemory);
    SDL3_GPU_DEVICE_FN(vkDeviceWaitIdle);
#undef SDL3_GPU_DEVICE_FN
    vkGetPhysicalDeviceMemoryProperties_ = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    VkPhysicalDeviceProperties p{};
    if (props) props(phys, &p);
    deviceName_ = p.deviceName;
  }
  ~VulkanTarget() override {
    releaseImage();
    jsui_gpu_destroy(gpu_);
  }

  bool ok() const {
    return vkCreateImage_ && vkDestroyImage_ && vkGetImageMemoryRequirements_ && vkAllocateMemory_ && vkFreeMemory_ &&
           vkBindImageMemory_ && vkDeviceWaitIdle_ && vkGetPhysicalDeviceMemoryProperties_;
  }

  std::string describe() const override { return "Skia Graphite Vulkan"; }
  std::string device() const override { return deviceName_; }

  bool resize(int width, int height) override {
    releaseImage();
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;  // what SDL uses for SDL_PIXELFORMAT_RGBA32 (sRGB output)
    ii.extent = {uint32_t(width), uint32_t(height), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = kImageUsage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage_(device_, &ii, nullptr, &image_) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements_(device_, image_, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties_(phys_, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; i++)
      if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        type = i;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory_(device_, &ai, nullptr, &memory_) != VK_SUCCESS ||
        vkBindImageMemory_(device_, image_, memory_, 0) != VK_SUCCESS) {
      return false;
    }
    // SDL wraps the image; given UNDEFINED it moves it to
    // SHADER_READ_ONLY_OPTIMAL in its next command buffer.
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_VULKAN_TEXTURE_NUMBER, Sint64(image_));
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_VULKAN_LAYOUT_NUMBER, VK_IMAGE_LAYOUT_UNDEFINED);
    texture_ = SDL_CreateTextureWithProperties(renderer_, p);
    SDL_DestroyProperties(p);
    if (!texture_) return false;
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
    width_ = width;
    height_ = height;
    holds_ = false;
    return true;
  }

  SDL_Texture* texture() const override { return texture_; }

  bool paint(JsuiRuntime* rt, bool force) override {
    if (!force && holds_ && !jsui_needs_redraw(rt)) return true;
    SDL_FlushRenderer(renderer_);  // submits SDL's work so far (and the image's first transition)
    JsuiGpuTarget t{};
    t.texture = reinterpret_cast<void*>(image_);
    t.format = VK_FORMAT_R8G8B8A8_UNORM;
    t.width = width_;
    t.height = height_;
    t.usage = kImageUsage;
    t.layout = t.final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t.flags = holds_ && !force ? JSUI_GPU_INCREMENTAL : 0;
    JsuiRect dirty;
    if (jsui_render_gpu_target(rt, gpu_, &t, &dirty) != 0) return false;
    holds_ = true;
    return true;
  }

 private:
  void releaseImage() {
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = nullptr;
    if (!image_ && !memory_) return;
    SDL_FlushRenderer(renderer_);
    vkDeviceWaitIdle_(device_);  // SDL's and Skia's frames done with the image
    if (image_) vkDestroyImage_(device_, image_, nullptr);
    if (memory_) vkFreeMemory_(device_, memory_, nullptr);
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
  }

  SDL_Renderer* renderer_;
  JsuiGpuContext* gpu_;
  VkPhysicalDevice phys_;
  VkDevice device_;
  std::string deviceName_;
  PFN_vkCreateImage vkCreateImage_ = nullptr;
  PFN_vkDestroyImage vkDestroyImage_ = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements_ = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory_ = nullptr;
  PFN_vkFreeMemory vkFreeMemory_ = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory_ = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle_ = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties_ = nullptr;
  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  SDL_Texture* texture_ = nullptr;
  int width_ = 0, height_ = 0;
  bool holds_ = false;  // the image holds the runtime's last frame
};

std::unique_ptr<UiTarget> createVulkanTarget(SDL_Renderer* renderer) {
  SDL_PropertiesID p = SDL_GetRendererProperties(renderer);
  auto instance = static_cast<VkInstance>(SDL_GetPointerProperty(p, SDL_PROP_RENDERER_VULKAN_INSTANCE_POINTER, nullptr));
  auto phys =
      static_cast<VkPhysicalDevice>(SDL_GetPointerProperty(p, SDL_PROP_RENDERER_VULKAN_PHYSICAL_DEVICE_POINTER, nullptr));
  auto device = static_cast<VkDevice>(SDL_GetPointerProperty(p, SDL_PROP_RENDERER_VULKAN_DEVICE_POINTER, nullptr));
  auto family = uint32_t(SDL_GetNumberProperty(p, SDL_PROP_RENDERER_VULKAN_GRAPHICS_QUEUE_FAMILY_INDEX_NUMBER, 0));
  // Graphite needs 1.1: only on the instance createRenderer made.
  if (!instance || !phys || !device || !g_instances.count(renderer)) return nullptr;
  PFN_vkGetInstanceProcAddr gipa = instanceProcAddr();
  auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
  auto getQueue = gdpa ? reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(device, "vkGetDeviceQueue")) : nullptr;
  if (!getQueue) return nullptr;
  VkQueue queue = VK_NULL_HANDLE;
  getQueue(device, family, 0, &queue);  // the queue SDL renders with
  JsuiVulkanDevice d{};
  d.instance = instance;
  d.physical_device = phys;
  d.device = device;
  d.queue = queue;
  d.queue_family = family;
  d.api_version = VK_API_VERSION_1_1;
  d.get_instance_proc_addr = reinterpret_cast<void*>(gipa);
  JsuiGpuContext* gpu = jsui_gpu_create_vulkan(&d);
  if (!gpu) return nullptr;
  auto t = std::make_unique<VulkanTarget>(renderer, gpu, phys, device, gipa, instance);
  if (!t->ok()) return nullptr;
  return t;
}

#endif  // SDL3_GPU_VULKAN

}  // namespace

const char* preferredDriver() {
#if defined(__APPLE__) && defined(JSUI_GRAPHITE_METAL)
  return "metal";
#elif SDL3_GPU_VULKAN
  return "vulkan";
#elif defined(JSUI_GPU)
  return "opengl";
#else
  return nullptr;
#endif
}

SDL_Renderer* createRenderer(SDL_Window* window, const char* driver) {
#if SDL3_GPU_VULKAN
  if (driver && SDL_strcmp(driver, "vulkan") == 0) {
    VkInstance instance = createInstance();
    if (!instance) return nullptr;
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_RENDERER_CREATE_NAME_STRING, "vulkan");
    SDL_SetPointerProperty(p, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetPointerProperty(p, SDL_PROP_RENDERER_CREATE_VULKAN_INSTANCE_POINTER, instance);
    SDL_Renderer* r = SDL_CreateRendererWithProperties(p);
    SDL_DestroyProperties(p);
    if (!r) {
      destroyInstance(instance);
      return nullptr;
    }
    g_instances[r] = instance;
    return r;
  }
#endif
  return SDL_CreateRenderer(window, driver);
}

void destroyRenderer(SDL_Renderer* renderer) {
  SDL_DestroyRenderer(renderer);
#if SDL3_GPU_VULKAN
  auto it = g_instances.find(renderer);
  if (it != g_instances.end()) {
    destroyInstance(it->second);  // SDL does not destroy an instance it was given
    g_instances.erase(it);
  }
#endif
}

std::unique_ptr<UiTarget> createTarget(SDL_Renderer* renderer) {
  const char* name = SDL_GetRendererName(renderer);
  if (!name) return nullptr;
#if defined(__APPLE__) && defined(JSUI_GRAPHITE_METAL)
  if (SDL_strcmp(name, "metal") == 0) return createMetalTarget(renderer);
#endif
#if SDL3_GPU_VULKAN
  if (SDL_strcmp(name, "vulkan") == 0) return createVulkanTarget(renderer);
#endif
#if defined(JSUI_GPU)
  if (SDL_strcmp(name, "opengl") == 0) return createGlTarget(renderer);
#endif
  return nullptr;
}

}  // namespace sdl3_gpu
