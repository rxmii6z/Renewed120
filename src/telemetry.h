#pragma once
#include <windows.h>

// Frame/physics telemetry for GTA SA 1.0 US. Called once per presented frame.
namespace Telemetry {
void Start(const wchar_t* csvPath);
void OnFrame();
}
