// Compile-time check that jsui_unity_api.h matches Unity's real plugin API
// headers: interface GUIDs, enum values, and the size and member offsets of
// every struct JSUI calls through. Built (as an object library, nothing links
// it) only when JSUI_UNITY_PLUGIN_API_DIR points at Unity's PluginAPI.
#include <cstddef>
#include <type_traits>

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
#include "IUnityGraphicsMetal.h"

#define JSUI_UNITY_API_NAMESPACE jsui_api
#include "jsui_unity_api.h"

#define SAME_GUID(T)                                                 \
  static_assert(RealGUID<::T>::high == jsui_api::JsuiUnityGUID<jsui_api::T>::high && \
                    RealGUID<::T>::low == jsui_api::JsuiUnityGUID<jsui_api::T>::low, \
                #T " GUID")
#define SAME_OFFSET(T, M) static_assert(offsetof(::T, M) == offsetof(jsui_api::T, M), #T "::" #M " offset")
#define SAME_VALUE(V) static_assert(int(::V) == int(jsui_api::V), #V)

static_assert(sizeof(::UnityInterfaceGUID) == sizeof(jsui_api::UnityInterfaceGUID), "UnityInterfaceGUID size");
SAME_OFFSET(UnityInterfaceGUID, m_GUIDHigh);
SAME_OFFSET(UnityInterfaceGUID, m_GUIDLow);
static_assert(std::is_trivially_copyable<::UnityInterfaceGUID>::value ==
                  std::is_trivially_copyable<jsui_api::UnityInterfaceGUID>::value,
              "UnityInterfaceGUID passing convention");

static_assert(sizeof(::IUnityInterfaces) == sizeof(jsui_api::IUnityInterfaces), "IUnityInterfaces size");
SAME_OFFSET(IUnityInterfaces, GetInterface);
SAME_OFFSET(IUnityInterfaces, RegisterInterface);
SAME_OFFSET(IUnityInterfaces, GetInterfaceSplit);
SAME_OFFSET(IUnityInterfaces, RegisterInterfaceSplit);

SAME_GUID(IUnityGraphics);
static_assert(sizeof(::IUnityGraphics) == sizeof(jsui_api::IUnityGraphics), "IUnityGraphics size");
SAME_OFFSET(IUnityGraphics, GetRenderer);
SAME_OFFSET(IUnityGraphics, RegisterDeviceEventCallback);
SAME_OFFSET(IUnityGraphics, UnregisterDeviceEventCallback);
SAME_OFFSET(IUnityGraphics, ReserveEventIDRange);
SAME_VALUE(kUnityGfxRendererNull);
SAME_VALUE(kUnityGfxRendererMetal);
SAME_VALUE(kUnityGfxDeviceEventInitialize);
SAME_VALUE(kUnityGfxDeviceEventShutdown);
SAME_VALUE(kUnityGfxDeviceEventBeforeReset);
SAME_VALUE(kUnityGfxDeviceEventAfterReset);
static_assert(sizeof(::UnityGfxRenderer) == sizeof(jsui_api::UnityGfxRenderer), "UnityGfxRenderer size");
static_assert(sizeof(::UnityGfxDeviceEventType) == sizeof(jsui_api::UnityGfxDeviceEventType),
              "UnityGfxDeviceEventType size");

SAME_GUID(IUnityGraphicsMetalV2);
SAME_OFFSET(IUnityGraphicsMetalV2, CommitCurrentCommandBuffer);
SAME_OFFSET(IUnityGraphicsMetalV2, CommandQueue);
SAME_OFFSET(IUnityGraphicsMetalV2, MetalDevice);
SAME_OFFSET(IUnityGraphicsMetalV2, EndCurrentCommandEncoder);
// Ours is a prefix of Unity's (it stops at the last member JSUI calls).
static_assert(sizeof(jsui_api::IUnityGraphicsMetalV2) <= sizeof(::IUnityGraphicsMetalV2),
              "IUnityGraphicsMetalV2 prefix");
