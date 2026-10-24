// The slice of Unity's native plugin API that jsui_unity uses, so the plugin's
// GPU path builds without a Unity install (CI, release builds).
//
// This declares only the ABI JSUI calls: the interface registry
// (IUnityInterfaces), IUnityGraphics (renderer + device events) and, per
// graphics API, the interface of that API:
//
//   Objective-C++ (__OBJC__)        IUnityGraphicsMetalV2
//   JSUI_UNITY_API_VULKAN           IUnityGraphicsVulkanV2 (and V1, the same
//                                   prefix); include vulkan_core.h first
//   JSUI_UNITY_API_D3D (Windows)    IUnityGraphicsD3D11, IUnityGraphicsD3D12v8;
//                                   include d3d11.h / d3d12.h first
//
// Layouts, enum values and interface GUIDs follow Unity 6000.3's PluginAPI
// headers; check_unity_api.cpp (.mm on Apple) static_asserts them against
// the real headers when JSUI_UNITY_PLUGIN_API_DIR is set. Members JSUI never
// calls are kept only as opaque placeholders, so the ones it does call sit at
// the right offsets; structs Unity fills in (UnityVulkanInstance,
// UnityVulkanImage) are declared whole.
//
// With JSUI_UNITY_SDK_HEADERS=1 this includes Unity's own headers instead.
// With JSUI_UNITY_API_NAMESPACE set, the declarations go into that namespace
// (for the check TU, which sees both).
#pragma once

#if defined(JSUI_UNITY_SDK_HEADERS) && JSUI_UNITY_SDK_HEADERS

#include "IUnityGraphics.h"
#include "IUnityInterface.h"
#ifdef __OBJC__
#include "IUnityGraphicsMetal.h"
#endif
#ifdef JSUI_UNITY_API_VULKAN
#ifndef UNITY_VULKAN_HEADER
#define UNITY_VULKAN_HEADER "include/third_party/vulkan/vulkan/vulkan_core.h"
#endif
#include "IUnityGraphicsVulkan.h"
#endif
#ifdef JSUI_UNITY_API_D3D
#include "IUnityGraphicsD3D11.h"
#include "IUnityGraphicsD3D12.h"
#endif

#else

#ifndef UNITY_INTERFACE_API
#if defined(_WIN32)
#define UNITY_INTERFACE_API __stdcall
#define UNITY_INTERFACE_EXPORT __declspec(dllexport)
#else
#define UNITY_INTERFACE_API
#define UNITY_INTERFACE_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
@protocol MTLCommandBuffer;
#endif

#ifdef JSUI_UNITY_API_NAMESPACE
namespace JSUI_UNITY_API_NAMESPACE {
#endif

// Two 64-bit halves. Unity's has a user-provided copy constructor, which makes
// it non-trivially copyable (passed by reference in the C++ ABI); this keeps
// that, though JSUI only looks interfaces up through GetInterfaceSplit.
struct UnityInterfaceGUID {
  constexpr UnityInterfaceGUID(unsigned long long high, unsigned long long low) : m_GUIDHigh(high), m_GUIDLow(low) {}
  constexpr UnityInterfaceGUID(const UnityInterfaceGUID& o) : m_GUIDHigh(o.m_GUIDHigh), m_GUIDLow(o.m_GUIDLow) {}
  unsigned long long m_GUIDHigh;
  unsigned long long m_GUIDLow;
};

struct IUnityInterface {};

template <typename T>
struct JsuiUnityGUID;  // specialized per interface: high / low

struct IUnityInterfaces {
  IUnityInterface*(UNITY_INTERFACE_API* GetInterface)(UnityInterfaceGUID guid);
  void(UNITY_INTERFACE_API* RegisterInterface)(UnityInterfaceGUID guid, IUnityInterface* ptr);
  IUnityInterface*(UNITY_INTERFACE_API* GetInterfaceSplit)(unsigned long long high, unsigned long long low);
  void(UNITY_INTERFACE_API* RegisterInterfaceSplit)(unsigned long long high, unsigned long long low,
                                                    IUnityInterface* ptr);

  template <typename T>
  T* Get() {
    return static_cast<T*>(GetInterfaceSplit(JsuiUnityGUID<T>::high, JsuiUnityGUID<T>::low));
  }
};

enum UnityGfxRenderer {
  kUnityGfxRendererD3D11 = 2,
  kUnityGfxRendererNull = 4,
  kUnityGfxRendererOpenGLES30 = 11,
  kUnityGfxRendererMetal = 16,
  kUnityGfxRendererOpenGLCore = 17,
  kUnityGfxRendererD3D12 = 18,
  kUnityGfxRendererVulkan = 21,
};

enum UnityGfxDeviceEventType {
  kUnityGfxDeviceEventInitialize = 0,
  kUnityGfxDeviceEventShutdown = 1,
  kUnityGfxDeviceEventBeforeReset = 2,
  kUnityGfxDeviceEventAfterReset = 3,
};

typedef void(UNITY_INTERFACE_API* IUnityGraphicsDeviceEventCallback)(UnityGfxDeviceEventType type);
typedef void(UNITY_INTERFACE_API* UnityRenderingEvent)(int eventId);
typedef void(UNITY_INTERFACE_API* UnityRenderingEventAndData)(int eventId, void* data);

struct IUnityGraphics : IUnityInterface {
  UnityGfxRenderer(UNITY_INTERFACE_API* GetRenderer)();
  void(UNITY_INTERFACE_API* RegisterDeviceEventCallback)(IUnityGraphicsDeviceEventCallback callback);
  void(UNITY_INTERFACE_API* UnregisterDeviceEventCallback)(IUnityGraphicsDeviceEventCallback callback);
  int(UNITY_INTERFACE_API* ReserveEventIDRange)(int count);
};
template <>
struct JsuiUnityGUID<IUnityGraphics> {
  static constexpr unsigned long long high = 0x7CBA0A9CA4DDB544ULL, low = 0x8C5AD4926EB17B11ULL;
};

#ifdef __OBJC__
struct IUnityGraphicsMetalV2 : IUnityInterface {
  id<MTLCommandBuffer>(UNITY_INTERFACE_API* CommitCurrentCommandBuffer)();
  id<MTLCommandQueue>(UNITY_INTERFACE_API* CommandQueue)();
  void* metalBundle_;
  id<MTLDevice>(UNITY_INTERFACE_API* MetalDevice)();
  void* currentCommandBuffer_;
  void* currentCommandEncoder_;
  void(UNITY_INTERFACE_API* EndCurrentCommandEncoder)();
  // Unity's has five more members after these; JSUI never touches them.
};
template <>
struct JsuiUnityGUID<IUnityGraphicsMetalV2> {
  static constexpr unsigned long long high = 0xF58857784FEF46ECULL, low = 0x9DB7A8803B87DA3DULL;
};
#endif

#ifdef JSUI_UNITY_API_VULKAN
// IUnityGraphicsVulkan.h. Unity fills these in, so they are declared whole.
struct UnityVulkanInstance {
  VkPipelineCache pipelineCache;
  VkInstance instance;
  VkPhysicalDevice physicalDevice;
  VkDevice device;
  VkQueue graphicsQueue;
  PFN_vkGetInstanceProcAddr getInstanceProcAddr;
  unsigned int queueFamilyIndex;
  void* reserved[8];
};

struct UnityVulkanMemory {
  VkDeviceMemory memory;
  VkDeviceSize offset;
  VkDeviceSize size;
  void* mapped;
  VkMemoryPropertyFlags flags;
  unsigned int memoryTypeIndex;
  void* reserved[4];
};

enum UnityVulkanResourceAccessMode {
  kUnityVulkanResourceAccess_ObserveOnly,
  kUnityVulkanResourceAccess_PipelineBarrier,
  kUnityVulkanResourceAccess_Recreate,
};

struct UnityVulkanImage {
  UnityVulkanMemory memory;
  VkImage image;
  VkImageLayout layout;
  VkImageAspectFlags aspect;
  VkImageUsageFlags usage;
  VkFormat format;
  VkExtent3D extent;
  VkImageTiling tiling;
  VkImageType type;
  VkSampleCountFlagBits samples;
  int layers;
  int mipCount;
  void* reserved[4];
};

enum UnityVulkanGraphicsQueueAccess {
  kUnityVulkanGraphicsQueueAccess_DontCare,
  kUnityVulkanGraphicsQueueAccess_Allow,
};

// IUnityGraphicsVulkanV2 and IUnityGraphicsVulkan share this prefix.
struct IUnityGraphicsVulkanV2 : IUnityInterface {
  void* InterceptInitialization_;
  void* InterceptVulkanAPI_;
  void* ConfigureEvent_;
  UnityVulkanInstance(UNITY_INTERFACE_API* Instance)();
  void* CommandRecordingState_;
  bool(UNITY_INTERFACE_API* AccessTexture)(void* nativeTexture, const VkImageSubresource* subResource,
                                           VkImageLayout layout, VkPipelineStageFlags pipelineStageFlags,
                                           VkAccessFlags accessFlags, UnityVulkanResourceAccessMode accessMode,
                                           UnityVulkanImage* outImage);
  void* AccessRenderBufferTexture_;
  void* AccessRenderBufferResolveTexture_;
  void* AccessBuffer_;
  void(UNITY_INTERFACE_API* EnsureOutsideRenderPass)();
  void* EnsureInsideRenderPass_;
  void(UNITY_INTERFACE_API* AccessQueue)(UnityRenderingEventAndData callback, int eventId, void* userData, bool flush);
  // Unity's has more members after these; JSUI never touches them.
};
template <>
struct JsuiUnityGUID<IUnityGraphicsVulkanV2> {
  static constexpr unsigned long long high = 0x329334C09DCA4787ULL, low = 0xB347DD92A0097FFCULL;
};
struct IUnityGraphicsVulkan : IUnityGraphicsVulkanV2 {};
template <>
struct JsuiUnityGUID<IUnityGraphicsVulkan> {
  static constexpr unsigned long long high = 0x95355348d4ef4e11ULL, low = 0x9789313dfcffcc87ULL;
};
#endif  // JSUI_UNITY_API_VULKAN

#ifdef JSUI_UNITY_API_D3D
struct IUnityGraphicsD3D11 : IUnityInterface {
  ID3D11Device*(UNITY_INTERFACE_API* GetDevice)();
  // Unity's has seven more members after this one; JSUI never touches them.
};
template <>
struct JsuiUnityGUID<IUnityGraphicsD3D11> {
  static constexpr unsigned long long high = 0xAAB37EF87A87D748ULL, low = 0xBF76967F07EFB177ULL;
};

enum UnityD3D12GraphicsQueueAccess {
  kUnityD3D12GraphicsQueueAccess_DontCare,
  kUnityD3D12GraphicsQueueAccess_Allow,
};
enum UnityD3D12EventConfigFlagBits {
  kUnityD3D12EventConfigFlag_EnsurePreviousFrameSubmission = (1 << 0),
  kUnityD3D12EventConfigFlag_FlushCommandBuffers = (1 << 1),
  kUnityD3D12EventConfigFlag_SyncWorkerThreads = (1 << 2),
  kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState = (1 << 3),
};
struct UnityD3D12PluginEventConfig {
  UnityD3D12GraphicsQueueAccess graphicsQueueAccess;
  UINT32 flags;
  bool ensureActiveRenderTextureIsBound;
};

// Unity 6.3's (RequestResourceState is new in v8).
struct IUnityGraphicsD3D12v8 : IUnityInterface {
  ID3D12Device*(UNITY_INTERFACE_API* GetDevice)();
  void* GetSwapChain_;
  void* GetSyncInterval_;
  void* GetPresentFlags_;
  void* GetFrameFence_;
  void* GetNextFrameFenceValue_;
  void* ExecuteCommandList_;
  void* SetPhysicalVideoMemoryControlValues_;
  ID3D12CommandQueue*(UNITY_INTERFACE_API* GetCommandQueue)();
  void* TextureFromRenderBuffer_;
  void* TextureFromNativeTexture_;
  void(UNITY_INTERFACE_API* ConfigureEvent)(int eventID, const UnityD3D12PluginEventConfig* pluginEventConfig);
  void* CommandRecordingState_;
  void(UNITY_INTERFACE_API* RequestResourceState)(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);
  // Unity's has one more member after these; JSUI never touches it.
};
template <>
struct JsuiUnityGUID<IUnityGraphicsD3D12v8> {
  static constexpr unsigned long long high = 0x9d303045d00d4cfdULL, low = 0x8febb42968b423b6ULL;
};
#endif  // JSUI_UNITY_API_D3D

#ifdef JSUI_UNITY_API_NAMESPACE
}  // namespace JSUI_UNITY_API_NAMESPACE
#endif

#endif  // JSUI_UNITY_SDK_HEADERS
