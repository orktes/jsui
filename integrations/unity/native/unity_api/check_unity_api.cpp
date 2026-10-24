// Compile-time check that jsui_unity_api.h matches Unity's real plugin API
// headers: interface GUIDs, enum values, and the size and member offsets of
// every struct JSUI calls through or Unity fills in. Built (as an object
// library, nothing links it) only when JSUI_UNITY_PLUGIN_API_DIR points at
// Unity's PluginAPI folder; per graphics API with JSUI_UNITY_CHECK_VULKAN /
// JSUI_UNITY_CHECK_D3D (Windows) / JSUI_UNITY_CHECK_METAL (check_unity_api.mm,
// which includes this file).
#include <cstddef>
#include <type_traits>

#ifdef JSUI_UNITY_CHECK_VULKAN
#define VK_NO_PROTOTYPES 1
#include "include/third_party/vulkan/vulkan/vulkan_core.h"
#define UNITY_VULKAN_HEADER "include/third_party/vulkan/vulkan/vulkan_core.h"
#endif
#ifdef JSUI_UNITY_CHECK_D3D
#include <d3d11.h>
#include <d3d12.h>
#endif

#include "IUnityInterface.h"

// Record each real GUID as constants too: Unity's registration macro only
// emits a (non-constexpr) function.
template <typename T>
struct RealGUID;
#undef UNITY_REGISTER_INTERFACE_GUID
#define UNITY_REGISTER_INTERFACE_GUID(HASHH, HASHL, TYPE)                                       \
  template <>                                                                                   \
  inline const UnityInterfaceGUID GetUnityInterfaceGUID<TYPE>() { return UnityInterfaceGUID(HASHH, HASHL); } \
  template <>                                                                                   \
  struct RealGUID<TYPE> {                                                                       \
    static constexpr unsigned long long high = HASHH, low = HASHL;                              \
  };

#include "IUnityGraphics.h"
#ifdef JSUI_UNITY_CHECK_METAL
#include "IUnityGraphicsMetal.h"
#endif
#ifdef JSUI_UNITY_CHECK_VULKAN
#include "IUnityGraphicsVulkan.h"
#define JSUI_UNITY_API_VULKAN 1
#endif
#ifdef JSUI_UNITY_CHECK_D3D
#include "IUnityGraphicsD3D11.h"
#include "IUnityGraphicsD3D12.h"
#define JSUI_UNITY_API_D3D 1
#endif

#define JSUI_UNITY_API_NAMESPACE jsui_api
#include "jsui_unity_api.h"

#define SAME_GUID(T)                                                 \
  static_assert(RealGUID<::T>::high == jsui_api::JsuiUnityGUID<jsui_api::T>::high && \
                    RealGUID<::T>::low == jsui_api::JsuiUnityGUID<jsui_api::T>::low, \
                #T " GUID")
#define SAME_OFFSET(T, M) static_assert(offsetof(::T, M) == offsetof(jsui_api::T, M), #T "::" #M " offset")
#define SAME_SIZE(T) static_assert(sizeof(::T) == sizeof(jsui_api::T), #T " size")
#define SAME_VALUE(V) static_assert(int(::V) == int(jsui_api::V), #V)
// Ours is a prefix of Unity's (it stops at the last member JSUI calls).
#define PREFIX_OF(T) static_assert(sizeof(jsui_api::T) <= sizeof(::T), #T " prefix")

SAME_SIZE(UnityInterfaceGUID);
SAME_OFFSET(UnityInterfaceGUID, m_GUIDHigh);
SAME_OFFSET(UnityInterfaceGUID, m_GUIDLow);
static_assert(std::is_trivially_copyable<::UnityInterfaceGUID>::value ==
                  std::is_trivially_copyable<jsui_api::UnityInterfaceGUID>::value,
              "UnityInterfaceGUID passing convention");

SAME_SIZE(IUnityInterfaces);
SAME_OFFSET(IUnityInterfaces, GetInterface);
SAME_OFFSET(IUnityInterfaces, RegisterInterface);
SAME_OFFSET(IUnityInterfaces, GetInterfaceSplit);
SAME_OFFSET(IUnityInterfaces, RegisterInterfaceSplit);

SAME_GUID(IUnityGraphics);
SAME_SIZE(IUnityGraphics);
SAME_OFFSET(IUnityGraphics, GetRenderer);
SAME_OFFSET(IUnityGraphics, RegisterDeviceEventCallback);
SAME_OFFSET(IUnityGraphics, UnregisterDeviceEventCallback);
SAME_OFFSET(IUnityGraphics, ReserveEventIDRange);
SAME_VALUE(kUnityGfxRendererD3D11);
SAME_VALUE(kUnityGfxRendererNull);
SAME_VALUE(kUnityGfxRendererOpenGLES30);
SAME_VALUE(kUnityGfxRendererMetal);
SAME_VALUE(kUnityGfxRendererOpenGLCore);
SAME_VALUE(kUnityGfxRendererD3D12);
SAME_VALUE(kUnityGfxRendererVulkan);
SAME_VALUE(kUnityGfxDeviceEventInitialize);
SAME_VALUE(kUnityGfxDeviceEventShutdown);
SAME_VALUE(kUnityGfxDeviceEventBeforeReset);
SAME_VALUE(kUnityGfxDeviceEventAfterReset);
SAME_SIZE(UnityGfxRenderer);
SAME_SIZE(UnityGfxDeviceEventType);
static_assert(std::is_same<::UnityRenderingEvent, jsui_api::UnityRenderingEvent>::value, "UnityRenderingEvent");
static_assert(std::is_same<::UnityRenderingEventAndData, jsui_api::UnityRenderingEventAndData>::value,
              "UnityRenderingEventAndData");

#ifdef JSUI_UNITY_CHECK_METAL
SAME_GUID(IUnityGraphicsMetalV2);
SAME_OFFSET(IUnityGraphicsMetalV2, CommitCurrentCommandBuffer);
SAME_OFFSET(IUnityGraphicsMetalV2, CommandQueue);
SAME_OFFSET(IUnityGraphicsMetalV2, MetalDevice);
SAME_OFFSET(IUnityGraphicsMetalV2, EndCurrentCommandEncoder);
PREFIX_OF(IUnityGraphicsMetalV2);
#endif

#ifdef JSUI_UNITY_CHECK_VULKAN
SAME_SIZE(UnityVulkanInstance);
SAME_OFFSET(UnityVulkanInstance, instance);
SAME_OFFSET(UnityVulkanInstance, physicalDevice);
SAME_OFFSET(UnityVulkanInstance, device);
SAME_OFFSET(UnityVulkanInstance, graphicsQueue);
SAME_OFFSET(UnityVulkanInstance, getInstanceProcAddr);
SAME_OFFSET(UnityVulkanInstance, queueFamilyIndex);
SAME_SIZE(UnityVulkanMemory);
SAME_SIZE(UnityVulkanImage);
SAME_OFFSET(UnityVulkanImage, image);
SAME_OFFSET(UnityVulkanImage, layout);
SAME_OFFSET(UnityVulkanImage, usage);
SAME_OFFSET(UnityVulkanImage, format);
SAME_OFFSET(UnityVulkanImage, extent);
SAME_SIZE(UnityVulkanResourceAccessMode);
SAME_VALUE(kUnityVulkanResourceAccess_ObserveOnly);
SAME_VALUE(kUnityVulkanResourceAccess_PipelineBarrier);
SAME_VALUE(kUnityVulkanResourceAccess_Recreate);
SAME_VALUE(kUnityVulkanGraphicsQueueAccess_DontCare);
SAME_VALUE(kUnityVulkanGraphicsQueueAccess_Allow);
SAME_GUID(IUnityGraphicsVulkanV2);
SAME_OFFSET(IUnityGraphicsVulkanV2, Instance);
SAME_OFFSET(IUnityGraphicsVulkanV2, AccessTexture);
SAME_OFFSET(IUnityGraphicsVulkanV2, EnsureOutsideRenderPass);
SAME_OFFSET(IUnityGraphicsVulkanV2, AccessQueue);
PREFIX_OF(IUnityGraphicsVulkanV2);
// The older interface: the same members at the same offsets.
SAME_GUID(IUnityGraphicsVulkan);
SAME_OFFSET(IUnityGraphicsVulkan, Instance);
SAME_OFFSET(IUnityGraphicsVulkan, AccessTexture);
SAME_OFFSET(IUnityGraphicsVulkan, EnsureOutsideRenderPass);
SAME_OFFSET(IUnityGraphicsVulkan, AccessQueue);
PREFIX_OF(IUnityGraphicsVulkan);
#endif

#ifdef JSUI_UNITY_CHECK_D3D
SAME_GUID(IUnityGraphicsD3D11);
SAME_OFFSET(IUnityGraphicsD3D11, GetDevice);
PREFIX_OF(IUnityGraphicsD3D11);
SAME_GUID(IUnityGraphicsD3D12v8);
SAME_OFFSET(IUnityGraphicsD3D12v8, GetDevice);
SAME_OFFSET(IUnityGraphicsD3D12v8, GetCommandQueue);
SAME_OFFSET(IUnityGraphicsD3D12v8, ConfigureEvent);
SAME_OFFSET(IUnityGraphicsD3D12v8, RequestResourceState);
PREFIX_OF(IUnityGraphicsD3D12v8);
SAME_SIZE(UnityD3D12PluginEventConfig);
SAME_OFFSET(UnityD3D12PluginEventConfig, graphicsQueueAccess);
SAME_OFFSET(UnityD3D12PluginEventConfig, flags);
SAME_OFFSET(UnityD3D12PluginEventConfig, ensureActiveRenderTextureIsBound);
SAME_SIZE(UnityD3D12GraphicsQueueAccess);
SAME_VALUE(kUnityD3D12GraphicsQueueAccess_DontCare);
SAME_VALUE(kUnityD3D12GraphicsQueueAccess_Allow);
SAME_VALUE(kUnityD3D12EventConfigFlag_EnsurePreviousFrameSubmission);
SAME_VALUE(kUnityD3D12EventConfigFlag_FlushCommandBuffers);
SAME_VALUE(kUnityD3D12EventConfigFlag_SyncWorkerThreads);
SAME_VALUE(kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState);
#endif
