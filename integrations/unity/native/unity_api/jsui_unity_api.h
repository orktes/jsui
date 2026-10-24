// The slice of Unity's native plugin API that jsui_unity uses, so the plugin's
// GPU path builds without a Unity install (CI, release builds).
//
// This declares only the ABI JSUI calls: the interface registry
// (IUnityInterfaces), IUnityGraphics (renderer + device events) and, for
// Objective-C++, IUnityGraphicsMetalV2. Layouts, enum values and interface
// GUIDs follow Unity 6000.3's PluginAPI headers; check_unity_api.mm
// static_asserts them against the real headers when JSUI_UNITY_PLUGIN_API_DIR
// is set. Members JSUI never calls are kept only as opaque placeholders, so
// the ones it does call sit at the right offsets.
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
  kUnityGfxRendererNull = 4,
  kUnityGfxRendererMetal = 16,
};

enum UnityGfxDeviceEventType {
  kUnityGfxDeviceEventInitialize = 0,
  kUnityGfxDeviceEventShutdown = 1,
  kUnityGfxDeviceEventBeforeReset = 2,
  kUnityGfxDeviceEventAfterReset = 3,
};

typedef void(UNITY_INTERFACE_API* IUnityGraphicsDeviceEventCallback)(UnityGfxDeviceEventType type);
typedef void(UNITY_INTERFACE_API* UnityRenderingEvent)(int eventId);

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

#ifdef JSUI_UNITY_API_NAMESPACE
}  // namespace JSUI_UNITY_API_NAMESPACE
#endif

#endif  // JSUI_UNITY_SDK_HEADERS
