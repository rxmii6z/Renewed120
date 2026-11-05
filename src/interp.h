#pragma once

// Render-time interpolation of entities and the camera (GTA SA 1.0 US).
// The game simulates at a fixed rate (see LogicHz); between two simulation ticks we show a blend of
// the previous and the current state so that 120+ FPS looks smooth.
namespace Interp {
// Microsecond accumulators for profiling (reset by the telemetry every second).
struct Timing {
    double restoreUs, updateUs, processUs, onTickUs, applyEntUs, applyCamUs, constructUs;
    int ticks, frames;
};
extern Timing g_timing;
double NowUs();
struct Stats {
    int pedsValid, vehsValid, objsValid;          // entities tracked at the last tick
    int moved;                                     // of those, how many differ between prev and curr
    int applied, applyFailed;                      // blends written this second / failures
    int noMatrix, noRwObject, noRwMatrix, noFrame; // why an entity could not be handled
    int cameraApplied, cameraTicks;
    int playerBones, playerBonesChanged;           // bones of the player ped, and how many changed in the last tick
    int hierPeds, hierNodes, hierApplied, playerHierNodes;   // ped skeleton hierarchies tracked / nodes / blended
    int bonesTracked, bonesApplied;                // child frames (bones, wheels, doors) tracked / blended
    int cameraFlipped;      // blended right axis points away from the real one (mirrored)
    float cameraHandedness; // (right x up) . at of the last real camera matrix: +1 right-handed, -1 left-handed
};
Stats GetAndResetStats();
// Debug ring buffer of the last camera/vehicle blends; written to csvPath (overwritten).
void DumpDebug(const wchar_t* csvPath);
void DumpPlayerFrames(const wchar_t* path);   // raw dump of the player ped's frame tree
void SetVehicleParts(bool on);
bool VehicleParts();
void SetPedSkeleton(bool on);   // blend ped skeleton bones
bool PedSkeleton();   // blend vehicle child frames (wheels, doors) too
void SetDebug(bool on);   // record the ring buffer (off by default: costs CPU and disk)
// After CGame::Process on a simulation tick: remember the new world state.
void OnTick();
// Before the render list is built: show the blended state of peds/vehicles/objects.
void ApplyEntities(float alpha);
// Before RwCameraBeginUpdate: show the blended camera. tickFrame = a simulation tick ran this frame.
void ApplyCamera(bool tickFrame, float alpha);
// Right after the game finished updating the ped skeletons, before the scene is drawn.
void ApplyHierarchies(bool tickFrame, float alpha);
// Around CRenderer::PreRender on render-only frames: keep the vehicles' physics state untouched.
void SaveVehicles();
void SavePeds();
void RestorePeds();
void RestoreVehicles();
// Before the next simulation tick: put the real (latest) state back.
void Restore();
}
