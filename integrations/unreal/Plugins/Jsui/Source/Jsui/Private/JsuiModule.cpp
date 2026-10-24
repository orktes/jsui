#include "JsuiLog.h"
#include "HAL/PlatformProcess.h"
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
	}

	virtual void ShutdownModule() override
	{
		if (Handle) FPlatformProcess::FreeDllHandle(Handle);
		Handle = nullptr;
	}

private:
	void* Handle = nullptr;
};

IMPLEMENT_MODULE(FJsuiModule, Jsui)
