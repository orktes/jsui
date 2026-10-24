#include "JsuiHost.h"
#include "JsuiLog.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogJsui);

class FJsuiModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if PLATFORM_WINDOWS
		// jsui.dll is delay-loaded (Jsui.Build.cs) and staged next to the
		// game binary; load it from there before the first jsui_* call.
		const FString Dll = FPaths::Combine(FPlatformProcess::GetModulesDirectory(), TEXT("jsui.dll"));
		Handle = FPlatformProcess::GetDllHandle(*Dll);
		if (!Handle)
		{
			UE_LOG(LogJsui, Error, TEXT("could not load %s"), *Dll);
		}
#endif
		// The GPU context lives on the RHI's device: free it while the RHI
		// is still up (modules are unloaded after it is gone).
		PreExit = FCoreDelegates::OnEnginePreExit.AddLambda([] { FJsuiHost::ReleaseGpu(); });
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnEnginePreExit.Remove(PreExit);
		if (Handle) FPlatformProcess::FreeDllHandle(Handle);
		Handle = nullptr;
	}

private:
	void* Handle = nullptr;
	FDelegateHandle PreExit;
};

IMPLEMENT_MODULE(FJsuiModule, Jsui)
