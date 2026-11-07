#pragma once
#include <windows.h>

// Frame/physics telemetry for GTA SA 1.0 US. Called once per presented frame.
namespace Telemetry {
void Start(const wchar_t* csvPath);
void OnFrame();

// Frame profiler: time stamps at fixed points of the main loop (Idle 0x53E920), in this order.
enum Mark { M_LogicStart, M_LogicEnd, M_AudioEnd, M_RwBeginEnd, M_ListStart, M_BlendEnd, M_ListEnd, M_PreStart, M_PreEnd,
            M_SceneStart, M_HudStart, M_HudEnd, M_PresentHook, M_PresentIn, M_WaitEnd, M_Count };
void Stamp(int mark);
// After Present returned. tick = this frame ran the game logic; playing = in game, no menu.
void EndFrame(bool tick, bool playing);
}
