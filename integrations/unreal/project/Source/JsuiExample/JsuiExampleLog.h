#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogJsuiExample, Log, All);

// The lines a test run looks for (selftest, screenshot, renderer), at Display
// level: a packaged game prints those to stdout too.
void JsuiExampleSay(const TCHAR* Line, bool bError = false);
