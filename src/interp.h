#pragma once
#include <cstdint>

// Render-time interpolation of entities and the camera (GTA SA 1.0 US).
// The game simulates at a fixed rate (see LogicHz); between two simulation ticks we show a blend of
// the previous and the current state so that 120+ FPS looks smooth.
namespace Interp {
// Microsecond accumulators for profiling (read and reset by the telemetry every second).
struct Timing {
    double restoreUs, updateUs, processUs, onTickUs, applyEntUs, applyCamUs, constructUs;
    int ticks, frames;
};
extern Timing g_timing;
double NowUs();
// A pool of the game holds more entities than the blend tables (a limit adjuster): those beyond are not blended.
bool PoolOverflow();

void SetVehicleParts(bool on);      // blend vehicle child frames (wheels, doors, forks)
bool VehicleParts();
void SetPedSkeleton(bool on);       // blend ped skeletons
bool PedSkeleton();
void SetCutsceneSkeleton(bool on);  // blend cutscene characters and the parts of cutscene objects
bool CutsceneSkeleton();

// After CGame::Process on a simulation tick: remember the new world state.
void OnTick();
// Before the render list is built: show the blended state of peds/vehicles/objects.
void ApplyEntities(float alpha);
// Show the blended camera. tickFrame = a simulation tick ran this frame (the new camera is read once per tick).
void ApplyCamera(bool tickFrame, float alpha);
// Right before CRenderer::PreRender: remember each ped's frame (ApplyHierarchies anchors the skeleton to it).
void CapturePedFrames(bool tickFrame);
// After PreRender, before the ped shadows and the scene are drawn: blend skeletons and vehicle/cutscene parts.
void ApplyHierarchies(bool tickFrame, float alpha);
// Around CRenderer::PreRender on render-only frames: keep the vehicles' and peds' physics state untouched.
void SaveVehicles();
void SavePeds();
void RestorePeds();
void RestoreVehicles();
// Before the next simulation tick: put the real (latest) state back.
void Restore();
// Menu open (a save may be loaded): forget every entity, skeleton and part kept for the blend.
void Reset();
// The camera is not blended any more (or is again): nothing kept from before.
void ForgetCamera();
void NoteShownByCamera(uintptr_t ent);   // the camera turned this ped: not another mod
void KeepShownPlayerSkeleton();   // before the tick: the player's skeleton as last shown
bool SwapShownPlayerSkeleton();   // swap it in / out (call twice)
}
