// Renewed120 - high frame rate for GTA San Andreas 1.0 US.
//
// - Direct3D9 (works with a DXVK d3d9.dll too): the monitor's refresh rate instead of the fixed 60 Hz, borderless
//   windowed fullscreen, precise frame cap.
// - Logic/render split: the game is simulated LogicHz times per second, the frames drawn in between show a blend
//   of the last two simulated states (interp.cpp), with fixes for the game systems that assumed one drawn frame
//   per simulated one (water, shadows, particles, physics state touched while drawing...).
// - Developer menu (F9) and optional telemetry.
//
// Minimal D3D9 declarations are repeated here on purpose: no DirectX SDK is needed.
#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <mmsystem.h>
#include "telemetry.h"
#include "interp.h"
#pragma comment(lib, "winmm.lib")

namespace {

// ---- minimal D3D9 ABI (x86, all fields are 4 bytes) -------------------------------------
struct PresentParams {
    UINT BackBufferWidth, BackBufferHeight;
    UINT BackBufferFormat;
    UINT BackBufferCount;
    UINT MultiSampleType;
    DWORD MultiSampleQuality;
    UINT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    UINT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT PresentationInterval;
};
static_assert(sizeof(PresentParams) == 56, "D3DPRESENT_PARAMETERS must be 56 bytes on x86");

struct DisplayMode { UINT Width, Height, RefreshRate, Format; };

constexpr UINT D3DFMT_X8R8G8B8 = 22;
constexpr UINT D3D_SDK_VERSION = 32;
constexpr int VT_GetAdapterModeCount = 6;
constexpr int VT_EnumAdapterModes    = 7;
constexpr int VT_GetAdapterDisplayMode = 8;
constexpr int VT_CreateDevice = 16;  // IDirect3D9
constexpr int VT_Reset        = 16;  // IDirect3DDevice9
constexpr int VT_Present      = 17;  // IDirect3DDevice9

using GetModeCountFn = UINT(WINAPI*)(void*, UINT, UINT);
using EnumModesFn    = HRESULT(WINAPI*)(void*, UINT, UINT, UINT, DisplayMode*);
using GetDispModeFn  = HRESULT(WINAPI*)(void*, UINT, DisplayMode*);
using CreateDeviceFn = HRESULT(WINAPI*)(void*, UINT, UINT, HWND, DWORD, PresentParams*, void**);
using ResetFn        = HRESULT(WINAPI*)(void*, PresentParams*);
using PresentFn      = HRESULT(WINAPI*)(void*, const RECT*, const RECT*, HWND, const void*);

CreateDeviceFn g_origCreateDevice = nullptr;
ResetFn        g_origReset = nullptr;
PresentFn      g_origPresent = nullptr;
bool           g_fixRefresh = true;
bool           g_telemetry = false;
int            g_fpsCap = 0;      // effective render cap (0 = none)
int            g_fpsCapSetting = 0;   // ini/menu value: -1 = off, 0 = auto (monitor refresh rate), > 0 = fixed
void*          g_d3d = nullptr;       // our own IDirect3D9, used only to query modes
int            g_forceHz = 0;
bool           g_vsync = false;    // wait for the monitor's refresh at each Present (no tearing)
int            g_monitorHz = 60;
int            g_windowMode = 1;   // 0 = exclusive fullscreen, 1 = borderless windowed fullscreen
int            g_backW = 1920, g_backH = 1080;
unsigned       g_presentCount = 0;
bool           g_log = true;
wchar_t        g_logPath[MAX_PATH];
HMODULE        g_self;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath, L"a") != 0 || !f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

// Current refresh rate of the monitor showing the game window (primary monitor if none yet).
int MonitorHz(HWND wnd) {
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
    DEVMODEW dev{}; dev.dmSize = sizeof(dev);
    const wchar_t* name = (mon && GetMonitorInfoW(mon, &mi)) ? mi.szDevice : nullptr;
    if (!EnumDisplaySettingsW(name, ENUM_CURRENT_SETTINGS, &dev) || dev.dmDisplayFrequency <= 1) return 60;   // 0/1 = "hardware default"
    return int(dev.dmDisplayFrequency);
}
HWND g_gameWnd = nullptr;
void ApplyFpsCap(HWND wnd) {
    const int before = g_fpsCap;
    g_monitorHz = MonitorHz(wnd);
    g_fpsCap = g_fpsCapSetting > 0 ? g_fpsCapSetting : (g_fpsCapSetting == 0 && !g_vsync ? g_monitorHz : 0);   // auto + VSync: VSync paces
    if (g_vsync && g_fpsCap >= g_monitorHz) g_fpsCap = 0;   // VSync already holds that rate: two pacers would fight
    if (g_fpsCap != before) Log("render cap: %d fps (setting %d)", g_fpsCap, g_fpsCapSetting);
}

void** VTable(void* obj) { return *reinterpret_cast<void***>(obj); }

bool PatchVTable(void* obj, int index, void* hook, void** orig) {
    void** slot = &VTable(obj)[index];
    if (*slot == hook) return true;  // already hooked (shared vtable)
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Pick the refresh rate to request for this fullscreen mode. Returns 0 to leave it untouched.
UINT ChooseHz(UINT adapter, const PresentParams* pp) {
    UINT want = static_cast<UINT>(g_forceHz);
    if (!want) {
        DisplayMode dm{};
        if (reinterpret_cast<GetDispModeFn>(VTable(g_d3d)[VT_GetAdapterDisplayMode])(g_d3d, adapter, &dm) == 0)
            want = dm.RefreshRate;
        if (!want) {
            DEVMODEW dev{}; dev.dmSize = sizeof(dev);
            if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dev)) want = dev.dmDisplayFrequency;
        }
    }
    if (want <= 1) return 0;  // 0/1 = "hardware default"

    // Make sure the adapter exposes this exact resolution+rate, otherwise CreateDevice fails.
    const UINT fmt = pp->BackBufferFormat ? pp->BackBufferFormat : D3DFMT_X8R8G8B8;
    const UINT count = reinterpret_cast<GetModeCountFn>(VTable(g_d3d)[VT_GetAdapterModeCount])(g_d3d, adapter, fmt);
    auto enumModes = reinterpret_cast<EnumModesFn>(VTable(g_d3d)[VT_EnumAdapterModes]);
    UINT best = 0;
    for (UINT i = 0; i < count; ++i) {
        DisplayMode m{};
        if (enumModes(g_d3d, adapter, fmt, i, &m) != 0) continue;
        if (m.Width != pp->BackBufferWidth || m.Height != pp->BackBufferHeight) continue;
        if (m.RefreshRate == want) return want;
        if (m.RefreshRate < want && m.RefreshRate > best) best = m.RefreshRate;
    }
    Log("  %ux%u@%uHz not offered by adapter, falling back to %uHz",
        pp->BackBufferWidth, pp->BackBufferHeight, want, best);
    return best;  // 0 = no usable mode found, keep the game's value
}

// Borderless windowed fullscreen: the game asks for exclusive fullscreen, we turn it into a windowed
// device and make the window a borderless popup covering the monitor (no mode switch, no black flash).
bool g_borderless = false;
void Fix(UINT adapter, PresentParams* pp, const char* where) {
    if (!pp || !g_d3d) return;
    if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0) { g_backW = int(pp->BackBufferWidth); g_backH = int(pp->BackBufferHeight); }
    g_borderless = false;
    if (g_windowMode == 1) {   // also when the game itself picked its "windowed" video mode (desktop resolution)
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        if (!pp->BackBufferFormat) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
        if (pp->hDeviceWindow) g_gameWnd = pp->hDeviceWindow;
        g_borderless = true;
        Log("%s: %ux%u -> borderless windowed", where, pp->BackBufferWidth, pp->BackBufferHeight);
        return;
    }
    if (!g_fixRefresh || pp->Windowed) return;
    const UINT before = pp->FullScreen_RefreshRateInHz;
    const UINT hz = ChooseHz(adapter, pp);
    if (hz) pp->FullScreen_RefreshRateInHz = hz;
    Log("%s: %ux%u fullscreen, refresh %u -> %u Hz", where,
        pp->BackBufferWidth, pp->BackBufferHeight, before, pp->FullScreen_RefreshRateInHz);
}

// After the device exists/was reset: restyle the window (must run after the D3D call, which may touch it).
int g_bx, g_by, g_bw, g_bh; HWND g_bwnd = nullptr;
void ApplyBorderless(HWND wnd, const PresentParams* pp) {
    if (!g_borderless || !wnd) return;
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int w = int(pp->BackBufferWidth), h = int(pp->BackBufferHeight);
    SetWindowLongW(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongW(wnd, GWL_EXSTYLE, GetWindowLongW(wnd, GWL_EXSTYLE) & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
    // Window = client = backbuffer size, centred on the monitor (identical to the monitor at native resolution).
    const int x = mi.rcMonitor.left + (mw - w) / 2, y = mi.rcMonitor.top + (mh - h) / 2;
    SetWindowPos(wnd, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g_bx = x; g_by = y; g_bw = w; g_bh = h; g_bwnd = wnd;
    SetForegroundWindow(wnd); SetActiveWindow(wnd); SetFocus(wnd);   // a windowed GTA pauses itself when it is not the active window
    Log("borderless window %dx%d at %d,%d (monitor %dx%d)", w, h, x, y, mw, mh);
    ApplyFpsCap(wnd);
}

HRESULT WINAPI HookedReset(void* dev, PresentParams* pp) {
    Fix(0, pp, "Reset");
    const HRESULT hr = g_origReset(dev, pp);
    if (hr == 0 && pp) ApplyBorderless(pp->hDeviceWindow ? pp->hDeviceWindow : g_gameWnd, pp);
    return hr;
}

// ---- logic / render split ----------------------------------------------------------------
// GTA SA 1.0 US, inside Idle (0x53E920):
//   0x53E968  call CTimer::Update   (0x561B10)  - advances game time + timestep
//   0x53E981  call CGame::Process   (0x53BEE0)  - pads, world, AI, physics, scripts
// Everything after is rendering. On "render only" frames we skip both calls, so the world is
// only simulated LogicHz times per second while the screen is redrawn at the render cap.
constexpr uintptr_t SITE_TimerUpdate = 0x53E968, EXPECT_TimerUpdate = 0x561B10;
constexpr uintptr_t SITE_GameProcess = 0x53E981, EXPECT_GameProcess = 0x53BEE0;
constexpr uintptr_t ADDR_MenuActive  = 0xBA67A4;   // byte, non-zero while the frontend menu is up

// Interpolation hook sites (also in Idle):
//   0x53E9F9  call CRenderer::ConstructRenderList (0x5556E0) - entities blended just before
//   0x53EA7E  call 0x53D7A0 (clear + RwCameraBeginUpdate)   - camera blended just before
constexpr uintptr_t SITE_ConstructList = 0x53E9F9, EXPECT_ConstructList = 0x5556E0;
constexpr uintptr_t SITE_CameraBegin   = 0x53EA7E, EXPECT_CameraBegin   = 0x53D7A0;

using VoidFn = void(__cdecl*)();
using CameraBeginFn = char(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
VoidFn g_origTimerUpdate = nullptr;
VoidFn g_origGameProcess = nullptr;
VoidFn g_origConstructList = nullptr;
CameraBeginFn g_origCameraBegin = nullptr;
int    g_logicHz = 0;
bool   g_interpolate = false;
bool   g_renderOnly = false;
LONGLONG g_lastTickQpc = 0, g_tickPeriodQpc = 1;
unsigned g_ticks = 0;

// The moment the frame being built will be shown. With the frame cap, Present is held until a fixed schedule
// (see LimitFrame), so the scheduled time is known in advance: blending for it instead of "now" removes the
// uneven steps of tick frames (they start later because the simulation runs first, but are shown on schedule).
LONGLONG g_nextPresentQpc = 0;
LONGLONG FrameQpcNow() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return ((g_fpsCap > 0 || g_vsync) && g_nextPresentQpc > now.QuadPart - g_tickPeriodQpc) ? g_nextPresentQpc : now.QuadPart;
}
// One time for the whole frame, taken when it starts (HookedTimerUpdate): without a frame cap or VSync the time was
// read again at every use, so the entities, the camera, the skeletons and the effects of one frame were each blended
// for a slightly later moment (the car shook against the camera when the frame took longer to build).
LONGLONG g_frameQpc = 0;
LONGLONG FrameQpc() { return g_frameQpc ? g_frameQpc : FrameQpcNow(); }
float TickAlpha() {
    const double a = double(FrameQpc() - g_lastTickQpc) / double(g_tickPeriodQpc);
    return a < 0.0 ? 0.0f : (a > 1.0 ? 1.0f : float(a));
}

bool HookCall(uintptr_t site, void* hook, void** origOut, uintptr_t expected) {
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (p[0] != 0xE8) { Log("HookCall %08X: not a call (0x%02X), SilentPatch or another mod changed it", unsigned(site), p[0]); return false; }
    const int32_t rel = *reinterpret_cast<int32_t*>(p + 1);
    const uintptr_t target = site + 5 + rel;
    if (target != expected) Log("HookCall %08X: target is %08X, expected %08X (another mod hooked it, chaining)", unsigned(site), unsigned(target), unsigned(expected));
    *origOut = reinterpret_cast<void*>(target);
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

// ---- water: CWaterLevel scrolls with ms_fTimeStep on every call and runs a periodic update on frame counter
// 0x1d (mod 32). Both assume one call per simulated frame, so on render-only frames spread them out.
constexpr uintptr_t SITE_WaterFn   = 0x6EB710;   // first instruction is "call 0x53C4B0"
constexpr uintptr_t EXPECT_WaterFn = 0x53C4B0;
constexpr uintptr_t ADDR_TimeStepF = 0xB7CB5C;
constexpr uintptr_t ADDR_FrameCtrW = 0xB7CB4C;
using BoolFn = char(__cdecl*)();
BoolFn g_origWaterGate = nullptr;
bool g_waterRedo = true;
bool g_freezeVeh = true;   // vehicle physics guard around PreRender
bool g_fixedStep = true;   // simulate exactly one schedule period per tick
bool     g_waterSaved = false;
float    g_savedStep = 0;
uint32_t g_savedCtr = 0;
unsigned g_waterLastTick = ~0u;
LONGLONG g_waterLastQpc = 0;

void RestoreWaterTiming() {
    if (!g_waterSaved) return;
    *reinterpret_cast<float*>(ADDR_TimeStepF) = g_savedStep;
    *reinterpret_cast<uint32_t*>(ADDR_FrameCtrW) = g_savedCtr;
    g_waterSaved = false;
}

// The +1 above must only be seen by the water code: the HUD is hidden for one frame by stamping the frame counter
// (0xB6EC40 == counter, idle camera, cutscenes...), so a counter left at +1 showed the HUD on the drawn frames of
// one tick every 32 (a flicker every second). Called right after the water function returns.
void RestoreWaterCounter() {
    if (g_waterSaved) *reinterpret_cast<uint32_t*>(ADDR_FrameCtrW) = g_savedCtr;
}

char __cdecl HookedWaterGate() {
    if (g_logicHz > 0) {
        LARGE_INTEGER now; now.QuadPart = FrameQpc();   // shown time of this frame, see SplitFrac
        if (!g_waterSaved) {
            g_savedStep = *reinterpret_cast<float*>(ADDR_TimeStepF);
            g_savedCtr = *reinterpret_cast<uint32_t*>(ADDR_FrameCtrW);
            g_waterSaved = true;
        }
        double frac = g_waterLastQpc ? double(now.QuadPart - g_waterLastQpc) / double(g_tickPeriodQpc) : 1.0;
        if (frac > 1.0) frac = 1.0;
        g_waterLastQpc = now.QuadPart;
        *reinterpret_cast<float*>(ADDR_TimeStepF) = g_savedStep * float(frac);
        // the periodic update must run once per tick, not on every render frame of that tick
        if (g_waterLastTick == g_ticks && (g_savedCtr & 0x1f) == 0x1d) *reinterpret_cast<uint32_t*>(ADDR_FrameCtrW) = g_savedCtr + 1;
        else *reinterpret_cast<uint32_t*>(ADDR_FrameCtrW) = g_savedCtr;
        g_waterLastTick = g_ticks;
    }
    return g_origWaterGate();
}

// ---- game clock while drawing -------------------------------------------------------------------------------
// Several things animate from CTimer::m_snTimeInMilliseconds, which only moves on simulated ticks: the height of the
// waves (CWaterLevel, sin of the clock) and the spin of pickups (weapons, health, money...). On render-only frames
// the clock is moved ahead by the share of the tick that elapsed, only around the draw code of these systems (nothing
// stores the shifted value: they only read it), then put back.
constexpr uintptr_t ADDR_TimeMs = 0xB7CB84;
bool     g_smoothClock = true;
bool     g_smoothFog = true;     // see HookedMovingFog
uint32_t g_tickMs = 0, g_lastTickTimeMs = 0;   // game ms the last tick advanced the clock by
uint32_t ClockAheadMs() {
    // on the tick frame too: it is shown a little after the tick like the others (it stayed at the tick's time: one
    // short step then one long step of the waves, pickups and wings, 30 times a second)
    if (!g_smoothClock || g_logicHz <= 0 || *reinterpret_cast<const uint8_t*>(ADDR_MenuActive)) return 0;
    const float a = TickAlpha();
    return uint32_t(float(g_tickMs) * a);
}
struct ClockShift {
    uint32_t saved; bool on;
    ClockShift() : saved(*reinterpret_cast<volatile uint32_t*>(ADDR_TimeMs)), on(false) {
        const uint32_t ahead = ClockAheadMs();
        if (ahead) { *reinterpret_cast<volatile uint32_t*>(ADDR_TimeMs) = saved + ahead; on = true; }
    }
    ~ClockShift() { if (on) *reinterpret_cast<volatile uint32_t*>(ADDR_TimeMs) = saved; }
};
// CWaterLevel::RenderWater (0x6EF650, called at the three sites below) and the pickups' spin / glow, both called from
// CObject's pre-render with the object: 0x455720 for every usual pickup (weapons, health, armour, money...) at
// 0x536541, 0x4560E0 for a few special models at 0x536961.
constexpr uintptr_t SITE_RenderWater1 = 0x53DD31, SITE_RenderWater2 = 0x53E004, SITE_RenderWater3 = 0x53E142, EXPECT_RenderWater = 0x6EF650;
constexpr uintptr_t SITE_PickupFx = 0x536541, EXPECT_PickupFx = 0x455720;
constexpr uintptr_t SITE_PickupFx2 = 0x536961, EXPECT_PickupFx2 = 0x4560E0;
VoidFn g_origRenderWater = nullptr;
void (__cdecl* g_origPickupFx)(void*) = nullptr;
void (__cdecl* g_origPickupFx2)(void*) = nullptr;
void __cdecl HookedRenderWater() { ClockShift shift; g_origRenderWater(); }
void __cdecl HookedPickupFx(void* ent) { ClockShift shift; g_origPickupFx(ent); }
void __cdecl HookedPickupFx2(void* ent) { ClockShift shift; g_origPickupFx2(ent); }
// Money dropped by peds (0x454E80, at 0x5361BE) and collectables: tags, horseshoes, oysters, snapshots (0x455E20, at
// 0x536187) are turned by the same pre-render from the game clock, which only moves on ticks.
constexpr uintptr_t SITE_MoneyFx = 0x5361BE, EXPECT_MoneyFx = 0x454E80;
constexpr uintptr_t SITE_CollectFx = 0x536187, EXPECT_CollectFx = 0x455E20;
void (__cdecl* g_origMoneyFx)(void*) = nullptr;
void (__cdecl* g_origCollectFx)(void*) = nullptr;
void __cdecl HookedMoneyFx(void* ent) { ClockShift shift; g_origMoneyFx(ent); }
void __cdecl HookedCollectFx(void* ent) { ClockShift shift; g_origCollectFx(ent); }

// ---- car headlight patches on the ground ------------------------------------------------------------------
// CShadows::StoreCarLightShadow picks the headlight patch of AI cars from "speed x ms_fTimeStep" (fmul at 0x70C679).
// The time step is scaled down on render-only frames (water, PreRender guard), so the choice flipped every tick and
// the patch jerked. That instruction reads this copy of the tick's step instead.
float g_tickStep = 50.0f / 30.0f;
bool PatchCarLightStep() {
    auto* p = reinterpret_cast<uint8_t*>(0x70C679);
    const uint8_t expect[6] = {0xD8, 0x0D, 0x5C, 0xCB, 0xB7, 0x00};   // fmul dword ptr [0xB7CB5C]
    if (std::memcmp(p, expect, 6) != 0) { Log("car light step patch: unexpected code, skipped"); return false; }
    DWORD old;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<float**>(p + 2) = &g_tickStep;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return true;
}
// Slow AI cars get their headlight patch as a cached static shadow (CShadows::StoreStaticShadow at 0x70C6E6), only
// recomputed after the car moved a few cm: it crawled in jumps. The player's car always takes the per-frame path
// (0x70C6F4); make every car take it ("je" at 0x70C68A -> "jmp").
// Same for the car shadows (CShadows::StoreShadowForVehicle, 0x70BDA0): an AI car whose speed * CTimer::ms_fTimeStep
// is small gets a cached static shadow (0x70C33F), only recomputed after it moved a few cm. On render-only frames the
// time step is only the share of the tick (PreRender guard), so every AI car looked parked and its shadow stayed at
// the tick position: 30 Hz. Make every car take the per-frame path like the player's car ("je" at 0x70C2C1 -> "jmp").
bool g_carShadowPatched = false, g_smoothCarShadows = true;
void SetVehShadowPatch(bool on) {
    if (!g_carShadowPatched) return;
    auto* p = reinterpret_cast<uint8_t*>(0x70C2C1);
    const uint8_t je[6] = {0x0F, 0x84, 0xC4, 0x00, 0x00, 0x00}, jmp[6] = {0xE9, 0xC5, 0x00, 0x00, 0x00, 0x90};
    DWORD old;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(p, on ? jmp : je, 6);
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    g_smoothCarShadows = on;
}
bool PatchVehShadowAlwaysMoving() {
    auto* p = reinterpret_cast<uint8_t*>(0x70C2C1);
    const uint8_t expect[6] = {0x0F, 0x84, 0xC4, 0x00, 0x00, 0x00};   // je 0x70C38B
    if (std::memcmp(p, expect, 6) != 0) { Log("car shadow path patch: unexpected code, skipped"); return false; }
    DWORD old;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    const uint8_t jmp[6] = {0xE9, 0xC5, 0x00, 0x00, 0x00, 0x90};    // jmp 0x70C38B; nop
    std::memcpy(p, jmp, 6);
    g_carShadowPatched = true;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return true;
}
bool PatchCarLightAlwaysMoving() {
    auto* p = reinterpret_cast<uint8_t*>(0x70C68A);
    if (p[0] != 0x74 || p[1] != 0x68) { Log("car light path patch: unexpected code, skipped"); return false; }
    DWORD old;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xEB;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    return true;
}

// ---- developer menu (F9) ---------------------------------------------------------------------------------
// Drawn with the game's own CFont after Render2dStuff. Keys: F9 open/close, PgUp/PgDn select, Home/End change,
// Enter on "Save" writes the current values into Renewed120.ini.
constexpr uintptr_t SITE_Render2d = 0x53EB12, EXPECT_Render2d = 0x53E230;
VoidFn g_origRender2d = nullptr;
bool   g_menuOpen = false;
int    g_menuSel = 0;
wchar_t g_iniPath[MAX_PATH];

extern bool g_splitFx;
extern bool g_camFrame;
extern bool g_streamEveryFrame;
extern bool g_staticFix;

using FontF2 = void(__cdecl*)(float, float);
using FontB1 = void(__cdecl*)(uint8_t);
using FontB2 = void(__cdecl*)(uint8_t, uint8_t);
using FontU4 = void(__cdecl*)(uint32_t);
using FontF1 = void(__cdecl*)(float);
using FontPrint = void(__cdecl*)(float, float, const char*);

constexpr int FpsCaps[] = {0, -1, 60, 90, 120, 144, 165, 180, 240};   // 0 = auto, -1 = off
constexpr int LogicRates[] = {15, 20, 25, 30, 45, 60};
template <size_t N> int CycleIndex(const int (&a)[N], int v, int dir) {
    int i = 0;
    for (size_t k = 0; k < N; ++k) if (a[k] == v) i = int(k);
    return (i + dir + int(N)) % int(N);
}

bool KeyPressed(int vk) {   // edge-triggered
    static bool was[256];
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool hit = down && !was[vk];
    was[vk] = down;
    return hit;
}

bool g_smoothSun = true;   // see "sun direction and sun shadows"
void RestoreSun();
extern bool g_smoothCarShadows;
extern bool g_stencilFrame;
extern bool g_smoothSparks;
void SetVehShadowPatch(bool on);
void SaveIni() {
    auto put = [](const wchar_t* k, int v) { wchar_t b[16]; swprintf_s(b, L"%d", v); WritePrivateProfileStringW(L"Renewed120", k, b, g_iniPath); };
    put(L"Interpolate", g_interpolate); put(L"VehicleParts", Interp::VehicleParts()); put(L"PedSkeleton", Interp::PedSkeleton());
    put(L"CutsceneSkeleton", Interp::CutsceneSkeleton()); put(L"WaterFix", g_waterRedo); put(L"StaticShadowFix", g_staticFix);
    put(L"SmoothFx", g_splitFx); put(L"StreamEveryFrame", g_streamEveryFrame); put(L"VehicleGuard", g_freezeVeh); put(L"SmoothSun", g_smoothSun); put(L"SmoothCarShadows", g_smoothCarShadows);
    put(L"SmoothClock", g_smoothClock); put(L"SmoothFog", g_smoothFog); put(L"CarStencilEachFrame", g_stencilFrame); put(L"SmoothSparks", g_smoothSparks);
    put(L"FpsCap", g_fpsCapSetting); put(L"LogicHz", g_logicHz); put(L"CameraEveryFrame", g_camFrame);
}

// Menu lines in display order (the ini keys are written by SaveIni).
enum MenuId {
    M_Interp, M_CamFrame, M_VehParts, M_PedSkel, M_CutSkel, M_Guard,
    M_Water, M_Clock, M_Fx, M_Sparks,
    M_StaticShadows, M_Sun, M_CarBlob, M_CarStencil,
    M_Stream, M_FpsCap, M_LogicHz, M_Save, MenuItems
};
void MenuChange(int dir) {   // dir: -1 / +1 (Enter = +1)
    switch (g_menuSel) {
    case M_Interp: if (g_interpolate) Interp::Restore(); g_interpolate = !g_interpolate; break;
    case M_CamFrame: g_camFrame = !g_camFrame; break;
    case M_VehParts: Interp::Restore(); Interp::SetVehicleParts(!Interp::VehicleParts()); break;
    case M_PedSkel: Interp::Restore(); Interp::SetPedSkeleton(!Interp::PedSkeleton()); break;
    case M_CutSkel: Interp::Restore(); Interp::SetCutsceneSkeleton(!Interp::CutsceneSkeleton()); break;
    case M_Guard: g_freezeVeh = !g_freezeVeh; break;
    case M_Water: g_waterRedo = !g_waterRedo; break;
    case M_Clock: g_smoothClock = !g_smoothClock; break;
    case M_Fx: g_splitFx = !g_splitFx; break;
    case M_Sparks: g_smoothSparks = !g_smoothSparks; break;
    case M_StaticShadows: g_staticFix = !g_staticFix; break;
    case M_Sun: RestoreSun(); g_smoothSun = !g_smoothSun; break;
    case M_CarBlob: SetVehShadowPatch(!g_smoothCarShadows); break;
    case M_CarStencil: g_stencilFrame = !g_stencilFrame; break;
    case M_Stream: g_streamEveryFrame = !g_streamEveryFrame; break;
    case M_FpsCap: g_fpsCapSetting = FpsCaps[CycleIndex(FpsCaps, g_fpsCapSetting, dir)]; ApplyFpsCap(g_gameWnd); break;
    case M_LogicHz: g_logicHz = LogicRates[CycleIndex(LogicRates, g_logicHz, dir)]; break;
    case M_Save: SaveIni(); break;
    }
}

void DrawMenu() {
    static bool first = true;
    static unsigned lastPresents = 0, lastTicks = 0;
    static LARGE_INTEGER freq, lastQpc;
    static double fps = 0, tps = 0;
    if (first) { first = false; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&lastQpc); lastPresents = g_presentCount; lastTicks = g_ticks; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double dt = double(now.QuadPart - lastQpc.QuadPart) / double(freq.QuadPart);
    if (dt >= 0.5) { fps = (g_presentCount - lastPresents) / dt; tps = (g_ticks - lastTicks) / dt; lastPresents = g_presentCount; lastTicks = g_ticks; lastQpc = now; }

    const bool focus = !g_gameWnd || GetForegroundWindow() == g_gameWnd;
    if (focus) {
        if (KeyPressed(VK_F9)) g_menuOpen = !g_menuOpen;
        if (g_menuOpen) {
            if (KeyPressed(VK_NEXT)) g_menuSel = (g_menuSel + 1) % MenuItems;
            if (KeyPressed(VK_PRIOR)) g_menuSel = (g_menuSel + MenuItems - 1) % MenuItems;
            if (KeyPressed(VK_END)) MenuChange(+1);
            if (KeyPressed(VK_HOME)) MenuChange(-1);
            if (KeyPressed(VK_RETURN)) MenuChange(+1);
        }
    }
    if (!g_menuOpen) return;

    const float sx = float(g_backW) / 640.0f, sy = float(g_backH) / 448.0f;
    reinterpret_cast<FontB1>(0x719490)(1);               // SetFontStyle
    reinterpret_cast<FontB1>(0x7195B0)(1);               // SetProportional
    reinterpret_cast<FontB2>(0x7195C0)(0, 0);            // SetBackground off
    reinterpret_cast<FontB1>(0x719600)(0);               // SetJustify off
    reinterpret_cast<FontB1>(0x719610)(1);               // SetOrientation left
    reinterpret_cast<FontF1>(0x7194F0)(100000.0f);       // SetWrapx: never wrap
    reinterpret_cast<FontF1>(0x719420)(0.0f);            // SetSlant 0
    reinterpret_cast<FontB1>(0x719570)(1);               // SetDropShadowPosition 1
    reinterpret_cast<FontF2>(0x719380)(0.34f * sx, 0.85f * sy);   // SetScale
    char line[160];
    float y = 40.0f * sy;
    const float x = 24.0f * sx, step = 12.5f * sy;
    auto print = [&](const char* s, uint32_t rgba) {
        reinterpret_cast<FontU4>(0x719430)(rgba);        // SetColor (R in the low byte)
        reinterpret_cast<FontPrint>(0x71A700)(x, y, s);
        y += step;
    };
    constexpr uint32_t WHITE = 0xFFFFFFFFu, YELLOW = 0xFF40E6FFu, GREY = 0xFFC8C8C8u;   // 0xAABBGGRR
    snprintf(line, sizeof(line), "RENEWED120 DEV  %.0f fps  %.0f ticks/s", fps, tps);
    print(line, YELLOW);
    print("F9 close | PgUp/PgDn select | Home/End change | Enter toggle", GREY);
    char val[48];
    for (int i = 0; i < MenuItems; ++i) {
        const char* name = ""; val[0] = 0;
        auto onOff = [&](bool v) { strcpy_s(val, v ? "ON" : "OFF"); };
        switch (i) {
        case M_Interp: name = "Interpolation"; onOff(g_interpolate); break;
        case M_CamFrame: name = "Camera on every frame (no lag)"; onOff(g_camFrame); break;
        case M_VehParts: name = "Vehicle parts (wheels/doors)"; onOff(Interp::VehicleParts()); break;
        case M_PedSkel: name = "Ped skeletons"; onOff(Interp::PedSkeleton()); break;
        case M_CutSkel: name = "Cutscene skeletons"; onOff(Interp::CutsceneSkeleton()); break;
        case M_Guard: name = "Physics guard (vehicles/peds)"; onOff(g_freezeVeh); break;
        case M_Water: name = "Water fix"; onOff(g_waterRedo); break;
        case M_Clock: name = "Smooth waves/pickups"; onOff(g_smoothClock); break;
        case M_Fx: name = "Smooth particles/debris"; onOff(g_splitFx); break;
        case M_Sparks: name = "Smooth sparks/dust emission"; onOff(g_smoothSparks); break;
        case M_StaticShadows: name = "Static shadows (blood pools)"; onOff(g_staticFix); break;
        case M_Sun: name = "Smooth sun shadows"; onOff(g_smoothSun); break;
        case M_CarBlob: name = "Car shadows, low FX (blob)"; onOff(g_smoothCarShadows); break;
        case M_CarStencil: name = "Car shadows, medium+ FX (stencil)"; onOff(g_stencilFrame); break;
        case M_Stream: name = "Load models every frame"; onOff(g_streamEveryFrame); break;
        case M_FpsCap: name = "FPS cap";
            if (g_fpsCapSetting == 0) sprintf_s(val, "auto (%d)", g_fpsCap); else if (g_fpsCap > 0) sprintf_s(val, "%d", g_fpsCap); else strcpy_s(val, "off");
            break;
        case M_LogicHz: name = "Logic rate (Hz)"; sprintf_s(val, "%d", g_logicHz); break;
        case M_Save: name = "Save to ini"; strcpy_s(val, "[Enter]"); break;
        }
        snprintf(line, sizeof(line), "%s %s: %s", i == g_menuSel ? ">" : " ", name, val);
        print(line, i == g_menuSel ? YELLOW : WHITE);
    }
    reinterpret_cast<VoidFn>(0x719840)();                // RenderFontBuffer: draw now
}

void __cdecl HookedRender2d() {
    Telemetry::Stamp(Telemetry::M_HudStart);
    g_origRender2d();
    DrawMenu();
    Telemetry::Stamp(Telemetry::M_HudEnd);
}

void FlushGuard();
void RestoreSun();
void RestoreFov();
void RestoreTheCamera();
void __cdecl HookedTimerUpdate() {
    static LARGE_INTEGER freq, nextTick;
    g_frameQpc = FrameQpcNow();   // the time this frame is blended for, everywhere
    if (g_logicHz <= 0) { g_origTimerUpdate(); return; }   // split disabled after a partial hook failure
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    const uint8_t menu = *reinterpret_cast<volatile uint8_t*>(ADDR_MenuActive);
    LARGE_INTEGER now; now.QuadPart = FrameQpc();   // the time this frame will be shown
    const LONGLONG period = freq.QuadPart / g_logicHz;

    // Menus/loading always run the full original path.
    bool tick = menu != 0 || nextTick.QuadPart == 0;
    // Otherwise tick when the next logic step is due at the time this frame is shown. (No tolerance: a tick run up
    // to half a frame early showed its frame at "alpha 0" while it stood a little before the tick: one step of 1.5
    // frames then one of 0.5, for as long as the frame and tick schedules stayed that close.)
    if (!tick) tick = now.QuadPart >= nextTick.QuadPart;
    if (tick) {
        // Keep a fixed schedule; resync after hitches instead of fast-forwarding.
        const LONGLONG sched = (nextTick.QuadPart == 0 || menu || now.QuadPart > nextTick.QuadPart + 2 * period)
                                   ? now.QuadPart : nextTick.QuadPart;
        nextTick.QuadPart = sched + period;
        g_renderOnly = false;
        ++g_ticks;
        const double t0 = Interp::NowUs();
        RestoreWaterTiming();
        FlushGuard();
        Interp::Restore();   // the simulation must see the real, latest state
        if (menu) Interp::Reset();   // a save may be loaded from the menu: see Interp::Reset
        RestoreSun();
        RestoreFov();
        RestoreTheCamera();
        const double t1 = Interp::NowUs();
        g_lastTickQpc = sched;   // on the fixed schedule: blend steps stay even
        g_tickPeriodQpc = period;
        g_origTimerUpdate();
        {
            const uint32_t now32 = *reinterpret_cast<const uint32_t*>(ADDR_TimeMs);
            g_tickMs = (g_lastTickTimeMs && now32 >= g_lastTickTimeMs && now32 - g_lastTickTimeMs < 200) ? now32 - g_lastTickTimeMs : 0;
            g_lastTickTimeMs = now32;
        }
        // CTimer measures the real time since the previous tick, which wobbles by a few ms around the schedule;
        // the blend assumes exactly one period per tick, so a wobbling step made moving things speed up and slow
        // down every tick. Simulate exactly one period (keeping the game's time scale; pauses keep their 0 step).
        if (g_fixedStep && !menu) {
            float& step = *reinterpret_cast<float*>(ADDR_TimeStepF);
            const float scale = *reinterpret_cast<const float*>(0xB7CB64);   // CTimer::ms_fTimeScale
            const float want = (50.0f / float(g_logicHz)) * scale;
            // only correct a small wobble: anything else (loading hitch, unknown time scale) stays as the game made it
            if (step > 0.0f && scale > 0.0f && step > want * 0.6f && step < want * 1.4f) step = want;
        }
        g_tickStep = *reinterpret_cast<const float*>(ADDR_TimeStepF);
        const double t2 = Interp::NowUs();
        Interp::g_timing.restoreUs += t1 - t0;
        Interp::g_timing.updateUs += t2 - t1;
        ++Interp::g_timing.ticks;
    } else {
        g_renderOnly = true;
    }
}

// CStreaming::LoadRequestedModels: takes the finished disk reads and loads the models, one read per channel per call.
// The game calls it from CStreaming::Update in CGame::Process, i.e. once per tick: at 30 ticks/s the map, LODs and
// cars loaded several times slower than in the original game running at a high frame rate. It is called alone on
// the frames without a tick (what the original game does on every frame; nothing else of Update runs).
// ---- sun direction and sun shadows ------------------------------------------------------------------------------
// CTimeCycle::Update (CGame::Process, once per tick) moves the sun with the game clock: it writes the direction to the
// sun (CTimeCycle::m_VectorToSun, vec3[16]) and the shadow vectors derived from it (CShadows::CalcPedShadowValues:
// front, side and displacement, float[16] each) in slot m_CurrentStoredValue. The shadows of peds, vehicles and
// objects are oriented with them, so they turned by small steps at 30 Hz while time passes. Drawn frames get the
// blend of the last two ticks' values (like the entities); the real values are put back before each tick.
constexpr uintptr_t ADDR_SunIndex = 0xB79FD0, ADDR_VectorToSun = 0xB7CA50;
constexpr uintptr_t ADDR_SunShadow[6] = { 0xB79F90, 0xB79F50, 0xB79F10, 0xB79ED0, 0xB79E90, 0xB79E50 };
struct SunState { int idx = -1; float v[9]{}; };   // vector to the sun, then the 6 shadow values
SunState g_sunPrev, g_sunCur;
bool g_sunApplied = false;
void ReadSun(SunState& st) {
    st.idx = *reinterpret_cast<const int*>(ADDR_SunIndex) & 15;
    const float* sun = reinterpret_cast<const float*>(ADDR_VectorToSun) + 3 * st.idx;
    for (int k = 0; k < 3; ++k) st.v[k] = sun[k];
    for (int k = 0; k < 6; ++k) st.v[3 + k] = reinterpret_cast<const float*>(ADDR_SunShadow[k])[st.idx];
}
void WriteSun(int idx, const float* v) {
    float* sun = reinterpret_cast<float*>(ADDR_VectorToSun) + 3 * idx;
    for (int k = 0; k < 3; ++k) sun[k] = v[k];
    for (int k = 0; k < 6; ++k) reinterpret_cast<float*>(ADDR_SunShadow[k])[idx] = v[3 + k];
}
void RestoreSun() {
    if (g_sunApplied) WriteSun(g_sunCur.idx, g_sunCur.v);
    g_sunApplied = false;
}
void OnTickSun() {
    SunState now; ReadSun(now);
    if (now.idx == g_sunCur.idx) return;   // CTimeCycle::Update did not run (menu, loading)
    g_sunPrev = g_sunCur;
    g_sunCur = now;
    // a jump of the clock (load, sleep, cheat, cutscene time): no blend
    const float* a = g_sunPrev.v; const float* b = g_sunCur.v;
    if (g_sunPrev.idx < 0 || a[0] * b[0] + a[1] * b[1] + a[2] * b[2] < 0.999f) g_sunPrev = g_sunCur;
}
void ApplySun(float alpha) {
    if (!g_smoothSun || g_sunCur.idx < 0 || g_sunPrev.idx < 0) return;
    if ((*reinterpret_cast<const int*>(ADDR_SunIndex) & 15) != g_sunCur.idx) return;
    float v[9];
    for (int k = 0; k < 9; ++k) v[k] = g_sunPrev.v[k] + (g_sunCur.v[k] - g_sunPrev.v[k]) * alpha;
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) for (int k = 0; k < 3; ++k) v[k] /= len;
    WriteSun(g_sunCur.idx, v);
    g_sunApplied = true;
}

// ---- sparks (and the other effects the simulation emits) ---------------------------------------------------------
// Collisions, scraping, bullets and wheels add their particles to the permanent effect systems of Fx_c (g_fx 0xA9AE00:
// +0x24 exhaust smoke, +0x28 spark, +0x2C spark with blur, dust, debris, collision smoke, blood...) through
// FxSystem_c::AddParticle (0x4AA440, thiscall: pos, vel, timeSince, FxPrtMult_c*, zRot, light, lightLimit, local;
// ret 0x20). The simulation only runs on tick frames, so every tick dropped its whole burst at once: 30 bursts per
// second however smooth the particles then move. The particles added during a tick are queued and released over the
// following drawn frames (ReleaseParticles).
constexpr uintptr_t ADDR_AddParticle = 0x4AA440, ADDR_FxC = 0xA9AE00;
constexpr int FxCSystems = 18;   // FxSystem_c* fields of Fx_c (+0x00..+0x44)
struct FxMult { float f[7]; };   // FxPrtMult_c: colour (rgba), size, size variation, life
using AddParticleFn = void(__thiscall*)(void*, float*, float*, float, FxMult*, float, float, float, uint32_t);
struct QueuedPrt {
    void* sys; float pos[3], vel[3], timeSince; FxMult mult; bool hasMult;
    float zRot, light, lightLimit; uint32_t local;
    float due;               // release time in ticks (g_prtTick + share of the tick), given at the first release
    bool spark, assigned;
    uint8_t* veh; float rel[3];   // spark on a car: place in the car's frame, put back on the car as shown when released
};
AddParticleFn g_origAddParticle = nullptr;   // trampoline (the first 5 bytes of the function, then a jump back)
bool g_smoothSparks = true, g_inTick = false;
bool IsSparkSystem(const void* sys) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(sys);
    return v && (v == *reinterpret_cast<const uintptr_t*>(0xA9AE28) || v == *reinterpret_cast<const uintptr_t*>(0xA9AE2C));
}
bool IsFxCSystem(const void* sys) {
    const uintptr_t* f = reinterpret_cast<const uintptr_t*>(ADDR_FxC);
    for (int i = 0; i < FxCSystems; ++i) if (f[i] == reinterpret_cast<uintptr_t>(sys)) return true;
    return false;
}

// Fx_c::AddSparks (0x49F040, thiscall pos, dir, force, count, across xyz, blur, spread, life; ret 0x28) is the only
// user of the spark systems. Its entry is already hooked by another mod, so its 8 call sites are hooked instead.
// - It spreads the sparks of one call along "across" x step, but that is not always the car's move (scraping, CPhysical
//   friction at 0x545953, passes another direction): the sparks did not leave from the car as shown. The car is found
//   instead (nearest vehicle < 8 m), AddSparks gets no "across", and each spark is kept in that car's frame.
// - CVehicle::AddSingleWheelParticles (0x6DE880, rims on the road) calls it from the vehicles' PreRender (0x6AB136..,
//   CBike 0x6BD38C, CBmx 0x6C0AFD), which runs on every drawn frame here with the wheel contact of the last tick: 6
//   times the sparks, piled on one spot. Sparks from PreRender only count on the tick frame (g_inPreRender).
using AddSparksFn = void(__thiscall*)(void*, float*, float*, float, int, float, float, float, uint32_t, float, float);
AddSparksFn g_origAddSparks = nullptr;
constexpr uintptr_t EXPECT_AddSparks = 0x49F040;
constexpr uintptr_t SITES_AddSparks[] = { 0x475D67, 0x49F498, 0x545953, 0x59E1DB, 0x61D1A9, 0x6A6E8E, 0x6DB6C1, 0x6DEB5A };
bool g_inSparks = false, g_inPreRender = false;
uint8_t* g_sparkVeh = nullptr;   // the car the sparks of the current AddSparks call are on
const uint8_t* VehiclePool(const uint8_t** objs, const int8_t** flags, int* size) {
    const uint8_t* pool = *reinterpret_cast<uint8_t* const*>(0xB74494);   // CPools::ms_pVehiclePool
    if (!pool) return nullptr;
    *objs = *reinterpret_cast<uint8_t* const*>(pool);
    *flags = *reinterpret_cast<const int8_t* const*>(pool + 4);
    *size = *reinterpret_cast<const int*>(pool + 8);
    return pool;
}
bool VehicleAlive(const uint8_t* veh) {
    __try {
        const uint8_t* objs; const int8_t* flags; int size;
        if (!veh || !VehiclePool(&objs, &flags, &size)) return false;
        const intptr_t off = veh - objs;
        if (off < 0 || off % 0xA18) return false;
        const int i = int(off / 0xA18);
        return i < size && flags[i] >= 0 && *reinterpret_cast<uint8_t* const*>(veh + 0x14) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint8_t* NearestVehicle(const float* pos) {
    __try {
        const uint8_t* objs; const int8_t* flags; int size;
        if (!VehiclePool(&objs, &flags, &size)) return nullptr;
        float best = 8.0f * 8.0f;
        const uint8_t* found = nullptr;
        for (int i = 0; i < size; ++i) {
            if (flags[i] < 0) continue;   // free slot
            const uint8_t* v = objs + 0xA18 * i;
            const uint8_t* m = *reinterpret_cast<uint8_t* const*>(v + 0x14);
            if (!m) continue;
            const float* p = reinterpret_cast<const float*>(m + 0x30);
            const float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best) { best = d; found = v; }
        }
        return const_cast<uint8_t*>(found);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void __fastcall HookedAddSparks(void* fx, void*, float* pos, float* dir, float force, int count, float ax, float ay, float az,
                                uint32_t blur, float spread, float life) {
    const bool was = g_inSparks;
    g_sparkVeh = (g_smoothSparks && g_logicHz > 0 && pos) ? NearestVehicle(pos) : nullptr;
    if (g_sparkVeh) ax = ay = az = 0.0f;
    g_inSparks = true;
    g_origAddSparks(fx, pos, dir, force, count, ax, ay, az, blur, spread, life);
    g_inSparks = was;
}

// The motion streak of a spark (FxPrimBP render 0x4A2C40, mode 2) is its move relative to the camera over one step:
// (pos - camera now) - (pos - velocity x CTimer::ms_fTimeStep (0x4A30F9) - camera before), "camera before" being
// TheCamera.m_mCameraMatrixOld's position (0xB6FA14, read at 0x4A318E/9B/A8). Both were per tick: the time step is
// only a share of it on render-only frames (water, PreRender guard), and the old camera only changed per tick while the
// shown camera moves every frame, so turning the camera fast stretched every spark more and more until the next tick.
// Both now cover one drawn frame: g_streakStep, and the camera of the previous drawn frame (g_prevCam).
float g_streakStep = 50.0f / 30.0f;
float g_prevCam[3] = {};
void UpdateStreakStep() {
    if (g_logicHz <= 0) { g_streakStep = g_tickStep; return; }
    static LONGLONG last = 0;
    const LONGLONG now = FrameQpc();
    double frac = g_fpsCap > 0 ? double(g_logicHz) / double(g_fpsCap) : (last ? double(now - last) / double(g_tickPeriodQpc) : 1.0);
    last = now;
    if (frac > 1.0) frac = 1.0;
    if (frac < 0.0) frac = 0.0;
    g_streakStep = g_tickStep * float(frac);
}
void UpdatePrevCam() {   // at the end of a frame: the camera it was drawn with
    __try {
        const uint8_t* cam = *reinterpret_cast<uint8_t* const*>(0xC1703C);        // Scene.m_pRwCamera
        const uint8_t* frame = cam ? *reinterpret_cast<uint8_t* const*>(cam + 4) : nullptr;
        if (!frame) return;
        const float* p = reinterpret_cast<const float*>(frame + 0x10 + 0x30);     // modelling matrix pos, as the render reads
        for (int k = 0; k < 3; ++k) g_prevCam[k] = p[k];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
bool RedirectFloat(uintptr_t at, const uint8_t (&expect)[6], float* to) {   // an x87 "op dword ptr [abs]" reads *to instead
    auto* p = reinterpret_cast<uint8_t*>(at);
    if (std::memcmp(p, expect, 6) != 0) return false;
    DWORD old;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<float**>(p + 2) = to;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return true;
}
bool PatchSparkStreak() {
    const uint8_t step[6] = {0xD9, 0x05, 0x5C, 0xCB, 0xB7, 0x00};   // fld dword ptr [0xB7CB5C]
    const uintptr_t camSites[3] = { 0x4A318E, 0x4A319B, 0x4A31A8 };
    for (int k = 0; k < 3; ++k) {
        const uint8_t e[6] = { 0xD8, 0x25, uint8_t(0x14 + 4 * k), 0xFA, 0xB6, 0x00 };   // fsub dword ptr [0xB6FA14 + 4k]
        if (std::memcmp(reinterpret_cast<void*>(camSites[k]), e, 6) != 0) { Log("spark streak patch: unexpected code, skipped"); return false; }
    }
    if (!RedirectFloat(0x4A30F9, step, &g_streakStep)) { Log("spark streak patch: unexpected code, skipped"); return false; }
    UpdatePrevCam();
    for (int k = 0; k < 3; ++k) {
        const uint8_t e[6] = { 0xD8, 0x25, uint8_t(0x14 + 4 * k), 0xFA, 0xB6, 0x00 };
        RedirectFloat(camSites[k], e, &g_prevCam[k]);
    }
    return true;
}

constexpr int MaxQueuedPrt = 4096;
QueuedPrt g_prtQueue[MaxQueuedPrt];
int g_prtCount = 0;
uint32_t g_prtRand = 0x2545F491u;
int g_prtTick = 0;            // ticks run so far: release times are g_prtTick + the share of the next tick shown
float g_sparkSpread = 2.0f;   // ticks over which the sparks of one tick come out
void __fastcall HookedAddParticle(void* sys, void*, float* pos, float* vel, float timeSince, FxMult* mult,
                                  float zRot, float light, float lightLimit, uint32_t local) {
    const bool preRenderSpark = g_inPreRender && IsSparkSystem(sys);
    if (preRenderSpark && g_renderOnly && g_smoothSparks && g_logicHz > 0) return;   // only once per tick, see g_inPreRender
    if (!g_smoothSparks || !(g_inTick || preRenderSpark) || g_logicHz <= 0 || g_prtCount >= MaxQueuedPrt || !pos || !vel ||
        *reinterpret_cast<const uint8_t*>(ADDR_MenuActive) || !IsFxCSystem(sys)) {
        g_origAddParticle(sys, pos, vel, timeSince, mult, zRot, light, lightLimit, local);
        return;
    }
    QueuedPrt& q = g_prtQueue[g_prtCount++];
    q.sys = sys;
    for (int k = 0; k < 3; ++k) { q.pos[k] = pos[k]; q.vel[k] = vel[k]; }
    q.timeSince = timeSince;
    q.hasMult = mult != nullptr;
    if (mult) q.mult = *mult;
    q.zRot = zRot; q.light = light; q.lightLimit = lightLimit; q.local = local;
    q.spark = g_inSparks;
    q.assigned = false;
    q.due = 0.0f;
    q.veh = nullptr;
    if (q.spark && VehicleAlive(g_sparkVeh)) {
        const float* m = reinterpret_cast<const float*>(*reinterpret_cast<uint8_t* const*>(g_sparkVeh + 0x14));
        const float d[3] = { pos[0] - m[12], pos[1] - m[13], pos[2] - m[14] };
        for (int k = 0; k < 3; ++k) q.rel[k] = m[4 * k] * d[0] + m[4 * k + 1] * d[1] + m[4 * k + 2] * d[2];   // right, forward, up
        q.veh = g_sparkVeh;
    }
}
// Called on every drawn frame after the entities were blended (alpha = share of the next tick shown).
// - Scraping gives its sparks in lumps (seen in a trace: 8 in one tick, 16 in another, none for 2 ticks): the sparks of
//   a tick get evenly spaced release times over g_sparkSpread ticks, which also fills the ticks without contact.
//   Other particles get a random time within the next tick.
// - timeSince moves a new particle along its way at once (CreateParticle 0x4A2580 runs the update 0x4A21D0 with it).
//   AddSparks gives (1 - i/count) x 0.05 s, made for a burst of all sparks at once: on top of the delay it made the
//   first sparks of each tick jump ahead. A spark gets only the time it waited past its release time.
void ReleaseParticles(float alpha, bool all = false) {
    if (!g_prtCount || !g_origAddParticle) return;
    const float a = alpha > 1.0f ? 1.0f : (alpha < 0.0f ? 0.0f : alpha);
    const float now = float(g_prtTick) + a;
    const float tickSec = g_logicHz > 0 ? 1.0f / float(g_logicHz) : 0.0f;
    int newSparks = 0;
    for (int i = 0; i < g_prtCount; ++i) if (!g_prtQueue[i].assigned && g_prtQueue[i].spark) ++newSparks;
    int j = 0;
    for (int i = 0; i < g_prtCount; ++i) {
        QueuedPrt& q = g_prtQueue[i];
        if (q.assigned) continue;
        q.assigned = true;
        if (q.spark) {
            q.due = now + (float(j++) + 0.5f) / float(newSparks) * g_sparkSpread;
        } else {
            g_prtRand = g_prtRand * 1664525u + 1013904223u;
            q.due = now + float(g_prtRand >> 8) / 16777216.0f;
        }
    }
    int keep = 0;
    for (int i = 0; i < g_prtCount; ++i) {
        QueuedPrt& q = g_prtQueue[i];
        if (!all && q.due > now) { if (keep != i) g_prtQueue[keep] = q; ++keep; continue; }
        float pos[3] = { q.pos[0], q.pos[1], q.pos[2] };
        if (q.veh && VehicleAlive(q.veh)) {
            const float* m = reinterpret_cast<const float*>(*reinterpret_cast<uint8_t* const*>(q.veh + 0x14));
            for (int k = 0; k < 3; ++k) pos[k] = m[12 + k] + m[k] * q.rel[0] + m[4 + k] * q.rel[1] + m[8 + k] * q.rel[2];
        }
        float late = (now - q.due) * tickSec;
        if (late < 0.0f) late = 0.0f;
        const float age = q.spark ? late : q.timeSince + late;
        g_origAddParticle(q.sys, pos, q.vel, age, q.hasMult ? &q.mult : nullptr, q.zRot, q.light, q.lightLimit, q.local);
    }
    g_prtCount = keep;
}
bool HookAddParticle() {
    auto* p = reinterpret_cast<uint8_t*>(ADDR_AddParticle);
    const uint8_t expect[5] = {0x83, 0xEC, 0x08, 0x56, 0x57};   // sub esp, 8; push esi; push edi
    if (std::memcmp(p, expect, 5) != 0) { Log("smooth sparks: FxSystem_c::AddParticle already changed by another mod, skipped"); return false; }
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, expect, 5);
    tramp[5] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + 6) = int32_t(ADDR_AddParticle + 5 - reinterpret_cast<uintptr_t>(tramp + 10));
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    g_origAddParticle = reinterpret_cast<AddParticleFn>(tramp);
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) = int32_t(reinterpret_cast<uintptr_t>(&HookedAddParticle) - (ADDR_AddParticle + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

constexpr uintptr_t ADDR_LoadRequestedModels = 0x40E3A0, ADDR_GameState = 0xC8D4C0;   // gGameState: 9 = playing
bool g_streamEveryFrame = true;
extern bool g_camOwed, g_camOwedPrev;

// ---- lights and shadows added by the simulation -------------------------------------------------------------------
// Idle empties the list of point lights on every drawn frame (CPointLights::NumLights = 0 at 0x53E977) and the shadow
// renderer empties the list of shadows to draw (CShadows::ShadowsStoredToBeRendered) after drawing it. What the
// simulation adds to them (CPointLights::AddLight 0x7000E0 from fires, explosions, gunshots and scripts;
// CShadows::StoreShadowToBeRendered 0x707390 from scripts) was only there on the tick frame: one drawn frame out of
// six at 180 fps. The entries added during a tick are kept and put back at the start of each frame drawn before the
// next one (those added from PreRender are added again on every frame by the game itself).
constexpr uintptr_t ADDR_NumPointLights = 0xC3F0D0, ADDR_PointLights = 0xC3F0E0;       // uint32; 32 x 0x30
constexpr uintptr_t ADDR_NumStoredShadows = 0xC403DC, ADDR_StoredShadows = 0xC40430;   // uint16; 48 x 0x34
constexpr uint32_t MaxPointLights = 32, PointLightSize = 0x30, MaxStoredShadows = 48, StoredShadowSize = 0x34;
bool g_tickLights = true;
uint8_t g_keptLights[MaxPointLights * PointLightSize], g_keptShadows[MaxStoredShadows * StoredShadowSize];
uint32_t g_keptLightCount = 0, g_keptShadowCount = 0, g_lightsBefore = 0, g_shadowsBefore = 0;
bool TickLightsOn() {
    return g_tickLights && *reinterpret_cast<const int*>(ADDR_GameState) == 9 && !*reinterpret_cast<const uint8_t*>(ADDR_MenuActive);
}
void BeginTickLights() {
    g_lightsBefore = *reinterpret_cast<const uint32_t*>(ADDR_NumPointLights);
    g_shadowsBefore = *reinterpret_cast<const uint16_t*>(ADDR_NumStoredShadows);
}
void CaptureTickLights() {
    g_keptLightCount = g_keptShadowCount = 0;
    if (!TickLightsOn()) return;
    const uint32_t lights = *reinterpret_cast<const uint32_t*>(ADDR_NumPointLights);
    const uint32_t shadows = *reinterpret_cast<const uint16_t*>(ADDR_NumStoredShadows);
    if (lights <= MaxPointLights && lights > g_lightsBefore) {
        g_keptLightCount = lights - g_lightsBefore;
        std::memcpy(g_keptLights, reinterpret_cast<const void*>(ADDR_PointLights + g_lightsBefore * PointLightSize), g_keptLightCount * PointLightSize);
    }
    if (shadows <= MaxStoredShadows && shadows > g_shadowsBefore) {
        g_keptShadowCount = shadows - g_shadowsBefore;
        std::memcpy(g_keptShadows, reinterpret_cast<const void*>(ADDR_StoredShadows + g_shadowsBefore * StoredShadowSize), g_keptShadowCount * StoredShadowSize);
    }
}
void ReplayTickLights() {
    if (!TickLightsOn()) { g_keptLightCount = g_keptShadowCount = 0; return; }
    uint32_t& lights = *reinterpret_cast<uint32_t*>(ADDR_NumPointLights);
    if (g_keptLightCount && lights <= MaxPointLights) {
        const uint32_t n = g_keptLightCount < MaxPointLights - lights ? g_keptLightCount : MaxPointLights - lights;
        std::memcpy(reinterpret_cast<void*>(ADDR_PointLights + lights * PointLightSize), g_keptLights, n * PointLightSize);
        lights += n;
    }
    uint16_t& shadows = *reinterpret_cast<uint16_t*>(ADDR_NumStoredShadows);
    if (g_keptShadowCount && shadows <= MaxStoredShadows) {
        const uint32_t n = g_keptShadowCount < MaxStoredShadows - shadows ? g_keptShadowCount : MaxStoredShadows - shadows;
        std::memcpy(reinterpret_cast<void*>(ADDR_StoredShadows + shadows * StoredShadowSize), g_keptShadows, n * StoredShadowSize);
        shadows = static_cast<uint16_t>(shadows + n);
    }
}

void __cdecl HookedGameProcess() {
    if (g_logicHz > 0 && g_renderOnly) {
        // only in the running game (gGameState 9), never in the pause/main menu (FrontEndMenuManager.m_bMenuActive)
        Telemetry::Stamp(Telemetry::M_LogicStart);
        if (g_streamEveryFrame && *reinterpret_cast<const int*>(ADDR_GameState) == 9 && !*reinterpret_cast<const uint8_t*>(ADDR_MenuActive))
            reinterpret_cast<VoidFn>(ADDR_LoadRequestedModels)();
        ReplayTickLights();
        Telemetry::Stamp(Telemetry::M_LogicEnd);
        return;
    }
    Telemetry::Stamp(Telemetry::M_LogicStart);
    const double t0 = Interp::NowUs();
    if (!g_smoothSparks) ReleaseParticles(1.0f, true);   // switched off: nothing left waiting
    g_inTick = true;
    BeginTickLights();
    g_camOwedPrev = g_camOwed;
    g_camOwed = false;   // set again by the tick if it skips its camera update (see CameraEveryFrame)
    g_origGameProcess();
    RestoreWaterCounter();   // CGame::Process ends with a jump to the water function
    CaptureTickLights();
    g_inTick = false;
    ++g_prtTick;
    OnTickSun();
    const double t1 = Interp::NowUs();
    if (g_interpolate) Interp::OnTick();
    if (Interp::PoolOverflow()) {
        static bool warned = false;
        if (!warned) { warned = true; Log("a ped, vehicle or object pool is larger than the blend tables (limit adjuster): the entities beyond them move at the logic rate"); }
    }
    const double t2 = Interp::NowUs();
    Interp::g_timing.processUs += t1 - t0;
    Interp::g_timing.onTickUs += t2 - t1;
    Telemetry::Stamp(Telemetry::M_LogicEnd);
}

// CRenderer::PreRender (called right after ConstructRenderList): see Interp::SaveVehicles.
constexpr uintptr_t SITE_PreRender = 0x53E9FE, EXPECT_PreRender = 0x553910;
VoidFn g_origPreRender = nullptr;
bool   g_guardPending = false;
void FlushGuard() {
    if (!g_guardPending) return;
    Interp::RestorePeds();
    Interp::RestoreVehicles();
    g_guardPending = false;
}

void PreRenderBody();
void __cdecl HookedPreRender() {
    Telemetry::Stamp(Telemetry::M_PreStart);
    PreRenderBody();
    Telemetry::Stamp(Telemetry::M_PreEnd);
}
// Police helicopter searchlights: CHeli::PreRender (vtable 0x8716C4) adds its light to a list (count 0xC1C96C) that
// is drawn every frame, but only CHeli::UpdateHelis, in the tick, empties it. Every drawn frame between two ticks
// added one more copy at another place (until the list was full): flicker and jerks. Emptied before each PreRender,
// the list holds exactly this frame's lights.
constexpr uintptr_t ADDR_NumHeliSearchLights = 0xC1C96C;
// The beam goes from the (blended) helicopter to the point it searches, CHeli +0x9F4 (x, y, z), which the tick moves:
// the beam swept at 30 fps. Around PreRender that point is blended between its last two tick values.
constexpr uintptr_t VT_Heli = 0x871680;
constexpr int OFF_HeliTarget = 0x9F4, MaxHeliSlots = 256;
struct HeliTarget { const uint8_t* veh; float prev[3], curr[3], real[3]; bool set; };
HeliTarget g_heliTgt[MaxHeliSlots];
unsigned g_heliTick = ~0u;
void BlendHeliTargets() {
    const uint8_t* objs; const int8_t* flags; int size;
    if (!g_interpolate || !VehiclePool(&objs, &flags, &size)) return;
    if (size > MaxHeliSlots) size = MaxHeliSlots;
    const bool newTick = g_heliTick != g_ticks;
    g_heliTick = g_ticks;
    const float t = TickAlpha();
    for (int i = 0; i < size; ++i) {
        HeliTarget& h = g_heliTgt[i];
        h.set = false;
        uint8_t* v = const_cast<uint8_t*>(objs) + 0xA18 * i;
        if (flags[i] < 0 || *reinterpret_cast<const uintptr_t*>(v) != VT_Heli) { h.veh = nullptr; continue; }
        float* tg = reinterpret_cast<float*>(v + OFF_HeliTarget);
        if (newTick) {
            const float d[3] = {tg[0] - h.curr[0], tg[1] - h.curr[1], tg[2] - h.curr[2]};
            const bool fresh = h.veh != v || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 400.0f;
            std::memcpy(h.prev, fresh ? tg : h.curr, 12);
            std::memcpy(h.curr, tg, 12);
            h.veh = v;
        }
        if (h.veh != v) continue;
        std::memcpy(h.real, tg, 12);
        for (int k = 0; k < 3; ++k) tg[k] = h.prev[k] + (h.curr[k] - h.prev[k]) * t;
        h.set = true;
    }
}
void RestoreHeliTargets() {
    for (auto& h : g_heliTgt) {
        if (!h.set) continue;
        std::memcpy(const_cast<uint8_t*>(h.veh) + OFF_HeliTarget, h.real, 12);
        h.set = false;
    }
}
void PreRenderBody() {
    if (g_logicHz > 0) *reinterpret_cast<int*>(ADDR_NumHeliSearchLights) = 0;
    if (g_interpolate) Interp::CapturePedFrames(!g_renderOnly);
    if (g_logicHz <= 0 || !g_freezeVeh) { g_origPreRender(); return; }
    float* ts = reinterpret_cast<float*>(ADDR_TimeStepF);
    const float before = *ts;
    const float real = g_waterSaved ? g_savedStep : before;
    if (g_renderOnly) {
        // show the wheels/suspension a fraction of a tick ahead of the last simulated state, then forget it
        *ts = real * TickAlpha();
        Interp::SaveVehicles();
        Interp::SavePeds();
        BlendHeliTargets();
        g_inPreRender = true;
        g_origPreRender();
        g_inPreRender = false;
        RestoreHeliTargets();
        g_guardPending = true;   // rendering must see PreRender's result; the state is put back at the end of the frame
    } else {
        *ts = real;   // the tick frame advances the state by one full step
        BlendHeliTargets();
        g_inPreRender = true;
        g_origPreRender();
        g_inPreRender = false;
        RestoreHeliTargets();
    }
    *ts = before;
}

// ---- static shadows (blood pools, permanent shadows...) --------------------------------------------------------
// CShadows::UpdateStaticShadows (0x707F40) runs right before the shadows are drawn, every drawn frame, and frees each
// static shadow that was not stored again since its previous call. The game re-stores them once per simulated tick
// (UpdatePermanentShadows in CGame::Process, ped tasks), so on render-only frames they were deleted: blinking pools,
// worse with more frames per tick. Only run it on tick frames.
constexpr uintptr_t SITE_UpdStatic1 = 0x53DDD6, SITE_UpdStatic2 = 0x53E0BE, EXPECT_UpdStatic = 0x707F40;
VoidFn g_origUpdStatic1 = nullptr, g_origUpdStatic2 = nullptr;
bool g_staticFix = true;
void __cdecl HookedUpdStatic1() { if (g_logicHz > 0 && g_renderOnly && g_staticFix) return; g_origUpdStatic1(); }
void __cdecl HookedUpdStatic2() { if (g_logicHz > 0 && g_renderOnly && g_staticFix) return; g_origUpdStatic2(); }

// ---- 3D markers (mission cylinders, arrows) -----------------------------------------------------------------
// Scripts place their markers once per tick (C3dMarkers::PlaceMarker sets "used" + "render this frame"). The markers
// renderer (0x725040, called at 0x726AE4 every drawn frame) clears both flags after drawing, and frees the markers not
// placed since: on render-only frames every script marker was freed, then re-created (from size 0) on the next tick,
// so they were invisible. The flags left by the tick are put back before each render-only frame.
constexpr uintptr_t SITE_Markers = 0x726AE4, EXPECT_Markers = 0x725040;
constexpr uintptr_t ADDR_MarkerFlags = 0xC7DD88 + 0x22;   // C3dMarkers::m_aMarkerArray[32], stride 0xA0
VoidFn g_origMarkers = nullptr;
uint8_t g_markerFlags[32][2];
// A marker moves, turns and pulses in C3dMarkers::PlaceMarker (0x725120), i.e. when a script or the game places it, once
// per tick: its matrix (C3dMarker +0, CMatrix: the cone bobbing in front of a door, the turning arrows) and its size
// (+0x6C, the pulse of the rings). Around the render each marker placed on the last two ticks is put at the blend of
// these two states, then put back. Markers: 32 at 0xC7DD58, stride 0xA0; in use +0x52, identifier +0x54.
constexpr uintptr_t ADDR_Markers = 0xC7DD58;
constexpr int NumMarkers = 32, MarkerStride = 0xA0;
struct MarkerState { float m[16]; float size; uint32_t id; bool used; };
MarkerState g_markerPrev[NumMarkers], g_markerCurr[NumMarkers];
unsigned g_markerTick = ~0u;
void BlendAxis(const float* a, const float* b, float t, float* o);
void ReadMarker(int i, MarkerState& st) {
    const uint8_t* mk = reinterpret_cast<const uint8_t*>(ADDR_Markers + MarkerStride * i);
    std::memcpy(st.m, mk, sizeof st.m);
    st.size = *reinterpret_cast<const float*>(mk + 0x6C);
    st.id = *reinterpret_cast<const uint32_t*>(mk + 0x54);
    st.used = mk[0x52] != 0;
}
void __cdecl HookedMarkers() {
    uint8_t* f = reinterpret_cast<uint8_t*>(ADDR_MarkerFlags);
    for (int k = 0; k < 32; ++k, f += 0xA0) {
        if (!g_renderOnly) { g_markerFlags[k][0] = f[0]; g_markerFlags[k][1] = f[1]; }
        else { f[0] |= g_markerFlags[k][0]; f[1] |= g_markerFlags[k][1]; }
    }
    if (!g_interpolate || g_logicHz <= 0) { g_origMarkers(); return; }
    if (g_markerTick != g_ticks) {   // first draw after a tick: the markers hold the new tick's state
        g_markerTick = g_ticks;
        for (int i = 0; i < NumMarkers; ++i) {
            MarkerState now; ReadMarker(i, now);
            const MarkerState& c = g_markerCurr[i];
            const float d[3] = {now.m[12] - c.m[12], now.m[13] - c.m[13], now.m[14] - c.m[14]};
            const bool fresh = !c.used || !now.used || c.id != now.id || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 25.0f;
            g_markerPrev[i] = fresh ? now : c;
            g_markerCurr[i] = now;
        }
    }
    const float t = TickAlpha();
    static float savedM[NumMarkers][16], savedSize[NumMarkers];
    bool set[NumMarkers];
    for (int i = 0; i < NumMarkers; ++i) {
        uint8_t* mk = reinterpret_cast<uint8_t*>(ADDR_Markers + MarkerStride * i);
        const MarkerState& a = g_markerPrev[i]; const MarkerState& b = g_markerCurr[i];
        set[i] = mk[0x52] != 0 && b.used && *reinterpret_cast<const uint32_t*>(mk + 0x54) == b.id &&
                 std::memcmp(mk, b.m, sizeof b.m) == 0 && (std::memcmp(a.m, b.m, sizeof b.m) != 0 || a.size != b.size);
        if (!set[i]) continue;
        float* m = reinterpret_cast<float*>(mk);
        std::memcpy(savedM[i], m, sizeof savedM[i]);
        savedSize[i] = *reinterpret_cast<const float*>(mk + 0x6C);
        for (int r = 0; r < 3; ++r) BlendAxis(a.m + r * 4, b.m + r * 4, t, m + r * 4);
        for (int k = 0; k < 3; ++k) m[12 + k] = a.m[12 + k] + (b.m[12 + k] - a.m[12 + k]) * t;
        *reinterpret_cast<float*>(mk + 0x6C) = a.size + (b.size - a.size) * t;
    }
    g_origMarkers();
    for (int i = 0; i < NumMarkers; ++i) {
        if (!set[i]) continue;
        uint8_t* mk = reinterpret_cast<uint8_t*>(ADDR_Markers + MarkerStride * i);
        std::memcpy(mk, savedM[i], sizeof savedM[i]);
        *reinterpret_cast<float*>(mk + 0x6C) = savedSize[i];
    }
}

// ---- ropes (crane, magnet, helicopter winch) ---------------------------------------------------------------------
// CRopes::Update (0x558D70, in the tick) moves the 32 points of each rope (8 at 0xB768B8, stride 0x328: points at +0,
// in use +0x325); CRopes::Render (0x556AE0, called at 0x53DE30 and 0x53E17A) draws them on every frame where the last
// tick left them, while what they hang from is shown blended. Around the render each point is put at the blend of its
// last two tick places.
constexpr uintptr_t SITE_Ropes1 = 0x53DE30, SITE_Ropes2 = 0x53E17A, EXPECT_Ropes = 0x556AE0, ADDR_Ropes = 0xB768B8;
constexpr int NumRopes = 8, RopeStride = 0x328, RopePoints = 32;
VoidFn g_origRopes1 = nullptr, g_origRopes2 = nullptr;
struct RopeState { float p[RopePoints][3]; bool used; };
RopeState g_ropePrev[NumRopes], g_ropeCurr[NumRopes];
unsigned g_ropeTick = ~0u;
uint8_t* Rope(int i) { return reinterpret_cast<uint8_t*>(ADDR_Ropes + RopeStride * i); }
void RenderRopes(VoidFn orig) {
    if (!g_interpolate || g_logicHz <= 0) { orig(); return; }
    if (g_ropeTick != g_ticks) {
        g_ropeTick = g_ticks;
        for (int i = 0; i < NumRopes; ++i) {
            RopeState now; std::memcpy(now.p, Rope(i), sizeof now.p); now.used = Rope(i)[0x325] != 0;
            const RopeState& c = g_ropeCurr[i];
            const float d[3] = {now.p[0][0] - c.p[0][0], now.p[0][1] - c.p[0][1], now.p[0][2] - c.p[0][2]};
            const bool fresh = !c.used || !now.used || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 400.0f;
            g_ropePrev[i] = fresh ? now : c;
            g_ropeCurr[i] = now;
        }
    }
    const float t = TickAlpha();
    static float saved[NumRopes][RopePoints][3];
    bool set[NumRopes];
    for (int i = 0; i < NumRopes; ++i) {
        float* pts = reinterpret_cast<float*>(Rope(i));
        set[i] = Rope(i)[0x325] != 0 && g_ropeCurr[i].used && std::memcmp(pts, g_ropeCurr[i].p, sizeof g_ropeCurr[i].p) == 0;
        if (!set[i]) continue;
        std::memcpy(saved[i], pts, sizeof saved[i]);
        const float* a = &g_ropePrev[i].p[0][0]; const float* b = &g_ropeCurr[i].p[0][0];
        for (int k = 0; k < RopePoints * 3; ++k) pts[k] = a[k] + (b[k] - a[k]) * t;
    }
    orig();
    for (int i = 0; i < NumRopes; ++i) if (set[i]) std::memcpy(Rope(i), saved[i], sizeof saved[i]);
}
void __cdecl HookedRopes1() { RenderRopes(g_origRopes1); }
void __cdecl HookedRopes2() { RenderRopes(g_origRopes2); }

// ---- particles and broken-object pieces ---------------------------------------------------------------------
// CGame::Process advances the effect system (Fx_c::Update, call at 0x53C1F0, dt in seconds) and the pieces of broken
// objects (BreakManager_c::Update, call at 0x59E670 site 0x53C201, raw timestep) once per tick, so they moved at 30 fps.
// Like the water: each call (tick frame in Process, render-only frames from ConstructList) advances them by the share
// of the tick that elapsed since the previous call.
constexpr uintptr_t SITE_FxUpdate = 0x53C1F0, EXPECT_FxUpdate = 0x49E640;
constexpr uintptr_t SITE_BreakUpdate = 0x53C201, EXPECT_BreakUpdate = 0x59E670;
using FxUpdateFn = void(__thiscall*)(void*, void*, float);
using BreakUpdateFn = void(__thiscall*)(void*, float);
FxUpdateFn g_origFx = nullptr;
BreakUpdateFn g_origBreak = nullptr;
bool g_splitFx = true;
struct SplitState { void* self = nullptr; void* cam = nullptr; float step = 0; LONGLONG last = 0; bool have = false; };
SplitState g_fx, g_break;
// Measured between the times the frames are SHOWN (FrameQpc), not the times of the calls: on tick frames the effects are
// updated early, in the middle of the simulation, and the time spent there varies (more with many particles), so the
// real call times made the particles advance by uneven steps.
float SplitFrac(SplitState& s) {
    LARGE_INTEGER now; now.QuadPart = FrameQpc();
    double f = s.last ? double(now.QuadPart - s.last) / double(g_tickPeriodQpc) : 1.0;
    s.last = now.QuadPart;
    return f < 0.0 ? 0.0f : (f > 1.0 ? 1.0f : float(f));
}
void __fastcall HookedFx(void* self, void*, void* cam, float dt) {
    if (g_logicHz <= 0 || !g_splitFx) { g_fx.have = false; g_origFx(self, cam, dt); return; }
    g_fx.self = self; g_fx.cam = cam; g_fx.step = dt; g_fx.have = true;
    g_origFx(self, cam, dt * SplitFrac(g_fx));
}
void __fastcall HookedBreak(void* self, void*, float ts) {
    if (g_logicHz <= 0 || !g_splitFx) { g_break.have = false; g_origBreak(self, ts); return; }
    g_break.self = self; g_break.step = ts; g_break.have = true;
    g_origBreak(self, ts * SplitFrac(g_break));
}
void SplitRenderOnly() {
    if (!g_splitFx) return;
    if (g_fx.have && g_origFx) {
        g_origFx(g_fx.self, g_fx.cam, g_fx.step * SplitFrac(g_fx));
    }
    if (g_break.have && g_origBreak) {
        // BreakObject_c::Update also counts calls: the object's frame counter (+0xC) goes up and each piece's life
        // (+0x70, pieces of 0x74 bytes at +8, count at +4) goes down by one per call. Extra calls must not age them.
        constexpr int MaxObjs = 64, MaxPieces = 128;
        static uint32_t ctr[MaxObjs], life[MaxObjs][MaxPieces];
        static uint8_t* pieces[MaxObjs];
        static int count[MaxObjs];
        auto* objs = static_cast<uint8_t*>(g_break.self);
        __try {
            for (int i = 0; i < MaxObjs; ++i) {
                uint8_t* o = objs + 0x20 * i;
                pieces[i] = nullptr;
                if (!o[1]) continue;
                pieces[i] = *reinterpret_cast<uint8_t**>(o + 8);
                count[i] = *reinterpret_cast<int*>(o + 4);
                ctr[i] = *reinterpret_cast<uint32_t*>(o + 0xC);
                for (int k = 0; k < count[i] && k < MaxPieces; ++k) life[i][k] = *reinterpret_cast<uint32_t*>(pieces[i] + 0x74 * k + 0x70);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        g_origBreak(g_break.self, g_break.step * SplitFrac(g_break));
        __try {
            for (int i = 0; i < MaxObjs; ++i) {
                uint8_t* o = objs + 0x20 * i;
                // only if the object is still the same one (it may have been freed during the call)
                if (!pieces[i] || !o[1] || *reinterpret_cast<uint8_t**>(o + 8) != pieces[i] || *reinterpret_cast<int*>(o + 4) != count[i]) continue;
                *reinterpret_cast<uint32_t*>(o + 0xC) = ctr[i];
                for (int k = 0; k < count[i] && k < MaxPieces; ++k) {
                    uint32_t& l = *reinterpret_cast<uint32_t*>(pieces[i] + 0x74 * k + 0x70);
                    if (l != 0) l = life[i][k];   // a piece that just expired stays expired
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ---- car shadows at medium+ visual FX (stencil shadow volumes) ----------------------------------------------------
// With Visual FX quality >= medium and a stencil buffer, the cars' shadows are volumes built from the shadow mesh of
// their collision model by CStencilShadows::Process (0x711D90, call at 0x53C1AB in CGame::Process, i.e. once per
// tick) and drawn every frame by CStencilShadows::Render. The volumes stayed at the tick position of the cars: 30 Hz
// shadows. Build them again on every drawn frame, after the cars were blended (the call in Process is kept).
constexpr uintptr_t ADDR_StencilShadowsProcess = 0x711D90;
bool g_stencilFrame = true;   // rebuild the stencil volumes on every drawn frame (split from the blob-shadow patch)
// Bikes and BMX (CVehicle::m_nVehicleType +0x590 == 9) build their shadow volume from CBike::m_mLeanMatrix (+0x5CC),
// not from their matrix (0x70FB80). It is only recomputed by CBike::CalculateLeanMatrix (0x6B7150) when the flag at
// +0x5C8 is 0, and the flag is cleared in the bike's PreRender (0x6BD3A1, 0x6C0B12), which runs after this rebuild: the
// volume used the lean matrix of an earlier frame (put back at the end of the frame by the physics guard), so the
// shadow jumped between two positions, further apart the faster the bike went (two faint shadows). Clear the flag
// first: the lean matrix is computed from the blended matrix of this frame. Afterwards the old lean matrix and flag
// are put back, so the rest of the game sees exactly what it saw before (PreRender recomputes it for drawing anyway).
struct BikeLean { uint8_t* veh; uint8_t flag; float m[16]; };
constexpr int MaxBikes = 64;
BikeLean g_bikeLean[MaxBikes];
int g_bikeLeanCount = 0;
void ResetBikeLean() {
    g_bikeLeanCount = 0;
    __try {
        const uint8_t* pool = *reinterpret_cast<uint8_t* const*>(0xB74494);   // CPools::ms_pVehiclePool
        if (!pool) return;
        uint8_t* objs = *reinterpret_cast<uint8_t* const*>(pool);
        const int8_t* flags = *reinterpret_cast<const int8_t* const*>(pool + 4);
        const int size = *reinterpret_cast<const int*>(pool + 8);
        for (int i = 0; i < size && g_bikeLeanCount < MaxBikes; ++i) {
            if (flags[i] < 0) continue;   // free slot
            uint8_t* v = objs + 0xA18 * i;
            if (*reinterpret_cast<const int*>(v + 0x590) != 9) continue;
            BikeLean& b = g_bikeLean[g_bikeLeanCount++];
            b.veh = v; b.flag = v[0x5C8];
            std::memcpy(b.m, v + 0x5CC, sizeof(b.m));   // CMatrix right/forward/up/pos (the RwMatrix pointer after it is left alone)
            v[0x5C8] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void RestoreBikeLean() {
    __try {
        for (int i = 0; i < g_bikeLeanCount; ++i) {
            BikeLean& b = g_bikeLean[i];
            std::memcpy(b.veh + 0x5CC, b.m, sizeof(b.m));
            b.veh[0x5C8] = b.flag;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_bikeLeanCount = 0;
}
void UpdateStencilShadows() {
    if (!g_stencilFrame || *reinterpret_cast<const int*>(ADDR_GameState) != 9 || *reinterpret_cast<const uint8_t*>(ADDR_MenuActive)) return;
    ResetBikeLean();
    const uint8_t* camMat = *reinterpret_cast<uint8_t* const*>(0xB6F03C);   // TheCamera.m_matrix (or its placement)
    const float* camPos = camMat ? reinterpret_cast<const float*>(camMat + 0x30) : reinterpret_cast<const float*>(0xB6F02C);
    reinterpret_cast<bool(__cdecl*)(const float*)>(ADDR_StencilShadowsProcess)(camPos);
    RestoreBikeLean();
}

// CAudioEngine::Service (0x507750, thiscall on AudioEngine 0xB6BC90) updates every sound entity (each car, ped...) and
// costs 1.3-2 ms with dense traffic. The original game ran it once per logic frame (30 per second); with 180 drawn
// frames it ran 6 times as often for nothing. AudioAtLogicRate keeps it on tick frames only.
// The RW call after it is hooked for the profiler only (Telemetry=1).
constexpr uintptr_t SITE_Audio = 0x53E98B, EXPECT_Audio = 0x507750, SITE_RwBegin = 0x53E997, EXPECT_RwBegin = 0x7354E0;
using ThisFn = void(__fastcall*)(void*, void*);
using RwBeginFn = int(__cdecl*)(void*);
ThisFn g_origAudio = nullptr;
RwBeginFn g_origRwBegin = nullptr;
// It runs on the drawn frame right after each tick, not in the tick frame itself: the tick frame already carries the
// whole simulation, so the two costs land on two frames (the timer does not move in between, so the sound sees the
// same time step). Two ticks in a row (menus, hitches): the pending update runs at once.
bool g_audioTickRate = true, g_audioPending = false;
void __fastcall HookedAudio(void* self, void* edx) {
    if (!(g_audioTickRate && g_logicHz > 0)) g_origAudio(self, edx);
    else if (!g_renderOnly) {
        if (g_audioPending) g_origAudio(self, edx);
        g_audioPending = true;
    } else if (g_audioPending) {
        g_origAudio(self, edx);
        g_audioPending = false;
    }
    Telemetry::Stamp(Telemetry::M_AudioEnd);
}
int __cdecl HookedRwBegin(void* p) { const int r = g_origRwBegin(p); Telemetry::Stamp(Telemetry::M_RwBeginEnd); return r; }

// ---- birds ----------------------------------------------------------------------------------------------------
// CBirds::Update (in the tick) moves the 6 birds (array 0xC6A8B0, stride 0x44: position +0, heading +0x1C, in use
// +0x37); CBirds::Render (0x712810, called at 0x53DE1C and 0x53E170) draws them where the last tick left them and
// flaps their wings from the game clock. Around the render they are put at the blend of their last two tick states
// and the clock is moved ahead like for the waves.
constexpr uintptr_t SITE_Birds1 = 0x53DE1C, SITE_Birds2 = 0x53E170, EXPECT_Birds = 0x712810, ADDR_Birds = 0xC6A8B0;
constexpr int NumBirds = 6, BirdStride = 0x44;
VoidFn g_origBirds1 = nullptr, g_origBirds2 = nullptr;
struct BirdState { float p[3]; float h; bool used; };
BirdState g_birdPrev[NumBirds], g_birdCurr[NumBirds];
unsigned g_birdTick = ~0u;
uint8_t* Bird(int i) { return reinterpret_cast<uint8_t*>(ADDR_Birds + BirdStride * i); }
void ReadBird(int i, BirdState& b) {
    std::memcpy(b.p, Bird(i), 12);
    b.h = *reinterpret_cast<const float*>(Bird(i) + 0x1C);
    b.used = Bird(i)[0x37] != 0;
}
void RenderBirds(VoidFn orig) {
    if (!g_interpolate || g_logicHz <= 0) { orig(); return; }
    if (g_birdTick != g_ticks) {   // first draw after a tick: the array holds the new tick's state
        const bool first = g_birdTick == ~0u;
        g_birdTick = g_ticks;
        for (int i = 0; i < NumBirds; ++i) {
            BirdState now; ReadBird(i, now);
            const float d[3] = {now.p[0] - g_birdCurr[i].p[0], now.p[1] - g_birdCurr[i].p[1], now.p[2] - g_birdCurr[i].p[2]};
            const bool fresh = first || !g_birdCurr[i].used || !now.used || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 100.0f;
            g_birdPrev[i] = fresh ? now : g_birdCurr[i];
            g_birdCurr[i] = now;
        }
    }
    const float t = TickAlpha();
    BirdState saved[NumBirds];
    for (int i = 0; i < NumBirds; ++i) {
        ReadBird(i, saved[i]);
        const BirdState& a = g_birdPrev[i]; const BirdState& b = g_birdCurr[i];
        if (!saved[i].used || !b.used) continue;
        float* pos = reinterpret_cast<float*>(Bird(i));
        for (int k = 0; k < 3; ++k) pos[k] = a.p[k] + (b.p[k] - a.p[k]) * t;
        float dh = b.h - a.h;
        while (dh > 3.14159265f) dh -= 6.2831853f;
        while (dh < -3.14159265f) dh += 6.2831853f;
        *reinterpret_cast<float*>(Bird(i) + 0x1C) = a.h + dh * t;
    }
    { ClockShift shift; orig(); }
    for (int i = 0; i < NumBirds; ++i) {
        if (!saved[i].used) continue;
        std::memcpy(Bird(i), saved[i].p, 12);
        *reinterpret_cast<float*>(Bird(i) + 0x1C) = saved[i].h;
    }
}
void __cdecl HookedBirds1() { RenderBirds(g_origBirds1); }
void __cdecl HookedBirds2() { RenderBirds(g_origBirds2); }

// ---- broken glass ---------------------------------------------------------------------------------------------
// The falling pieces of broken windscreens and windows (CFallingGlassPane, 45 at 0xC71BF8, stride 0x70: CMatrix at
// +0, in use +0x6D) are moved and spun by CGlass::Update in the tick and drawn by CGlass::Render (0x71CE20, called
// at 0x53DE21 and 0x53E17F) on every frame. Around the render each piece is put at the blend of its last two ticks.
constexpr uintptr_t SITE_Glass1 = 0x53DE21, SITE_Glass2 = 0x53E17F, EXPECT_Glass = 0x71CE20, ADDR_Panes = 0xC71BF8;
constexpr int NumPanes = 45, PaneStride = 0x70;
VoidFn g_origGlass1 = nullptr, g_origGlass2 = nullptr;
struct PaneState { float m[16]; bool used; };
PaneState g_panePrev[NumPanes], g_paneCurr[NumPanes];
unsigned g_paneTick = ~0u;
float* Pane(int i) { return reinterpret_cast<float*>(ADDR_Panes + PaneStride * i); }
bool PaneUsed(int i) { return reinterpret_cast<const uint8_t*>(Pane(i))[0x6D] != 0; }
void BlendAxis(const float* a, const float* b, float t, float* o) {
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    for (int k = 0; k < 3; ++k) o[k] = a[k] + (b[k] - a[k]) * t;
    const float l = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    if (l > 1e-6f) { const float want = la + (lb - la) * t; for (int k = 0; k < 3; ++k) o[k] *= want / l; }
}
void RenderGlass(VoidFn orig) {
    if (!g_interpolate || g_logicHz <= 0) { orig(); return; }
    if (g_paneTick != g_ticks) {
        g_paneTick = g_ticks;
        for (int i = 0; i < NumPanes; ++i) {
            PaneState now; std::memcpy(now.m, Pane(i), sizeof now.m); now.used = PaneUsed(i);
            const float* c = g_paneCurr[i].m;
            const float d[3] = {now.m[12] - c[12], now.m[13] - c[13], now.m[14] - c[14]};
            const bool fresh = !g_paneCurr[i].used || !now.used || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 25.0f;
            g_panePrev[i] = fresh ? now : g_paneCurr[i];
            g_paneCurr[i] = now;
        }
    }
    const float t = TickAlpha();
    float saved[NumPanes][16]; bool set[NumPanes];
    for (int i = 0; i < NumPanes; ++i) {
        set[i] = PaneUsed(i) && g_paneCurr[i].used;
        if (!set[i]) continue;
        float* m = Pane(i);
        std::memcpy(saved[i], m, sizeof saved[i]);
        const float* a = g_panePrev[i].m; const float* b = g_paneCurr[i].m;
        for (int r = 0; r < 3; ++r) BlendAxis(a + r * 4, b + r * 4, t, m + r * 4);
        for (int k = 0; k < 3; ++k) m[12 + k] = a[12 + k] + (b[12 + k] - a[12 + k]) * t;
    }
    orig();
    for (int i = 0; i < NumPanes; ++i) if (set[i]) std::memcpy(Pane(i), saved[i], sizeof saved[i]);
}
void __cdecl HookedGlass1() { RenderGlass(g_origGlass1); }
void __cdecl HookedGlass2() { RenderGlass(g_origGlass2); }

// ---- water cannons (fire truck, SWAT tank) ----------------------------------------------------------------------
// CWaterCannon (3 at 0xC80740, stride 0x3CC, in use if +0 != 0): 32 points of the jet at +0x0C, their speeds at
// +0x18C, point alive at +0x30C, newest point at +4. On each tick the oldest point is freed, every point does
// speed.z -= g * step then point += speed * step, and a new point is added at the nozzle. CWaterCannons::Render
// (0x729B30, called at 0x53DE26 and 0x53E1A5) draws the jet on every frame as three ribbons through the points: one
// turned to the camera, two in random directions, wider and more faded toward the end. Around it:
//   - each point is put at the blend, point - speed * step * (1 - alpha); the newest one follows the nozzle and the
//     oldest one slides onto the next point, so the jet neither stretches nor snaps back at each tick;
//   - the camera matrix is blended (the first ribbon is turned to it);
//   - the width and fade of a segment use its place along the jet plus alpha instead of a whole number (cave);
//   - the two random directions are picked once per tick instead of on every frame (rand hook);
//   - segments up to 10 m are drawn instead of 5 m: on a moving truck they are near that length and the blend made
//     them appear and disappear from frame to frame.
constexpr uintptr_t SITE_Cannons1 = 0x53DE26, SITE_Cannons2 = 0x53E1A5, EXPECT_Cannons = 0x729B30, ADDR_Cannons = 0xC80740;
constexpr int NumCannons = 3, CannonStride = 0x3CC, CannonPoints = 32;
VoidFn g_origCannons1 = nullptr, g_origCannons2 = nullptr;
bool BeginCamMatrixBlend();
void EndCamMatrixBlend();

// Segment j (from the nozzle, [esp+0x2C]) gets the width j*j/32 + 3 and the alpha 64 * (1 - j/32). The cave at
// 0x7290C2 uses j + alpha: just before a tick that is j + 1, just after it the same segment is j + 1 + 0.
constexpr uintptr_t SITE_CannonFade = 0x7290C2, SITE_CannonMaxSeg = 0x728F04;
float g_cannonSegOff = 0.0f;
float g_cannonMaxSeg2 = 100.0f;   // squared length; the game uses 25 (0x858FE8)
__declspec(naked) void CannonFadeCave() {
    __asm {
        movsx eax, word ptr [esp + 0x2C]
        mov [esp + 0x18], eax
        mov ecx, eax
        imul ecx, eax
        mov [esp + 0x10], ecx
        fild dword ptr [esp + 0x18]
        fadd dword ptr [g_cannonSegOff]
        fld st(0)
        fmul st(0), st(0)
        fmul dword ptr ds:[0x864E10]      ; 1/32
        fadd dword ptr ds:[0x858B3C]      ; 3
        fxch st(1)
        fmul dword ptr ds:[0x864E10]
        fmul dword ptr ds:[0x859A44]      ; 64
        fsubr dword ptr ds:[0x859A44]
        push 0x7290FA
        ret
    }
}
bool PatchCannonRender() {
    uint8_t* fade = reinterpret_cast<uint8_t*>(SITE_CannonFade);
    uint8_t* maxSeg = reinterpret_cast<uint8_t*>(SITE_CannonMaxSeg);
    static const uint8_t expectFade[] = {0x0F, 0xBF, 0x44, 0x24, 0x2C, 0x8B, 0xC8, 0x0F, 0xAF, 0xC8};
    static const uint8_t expectMaxSeg[] = {0xD8, 0x1D, 0xE8, 0x8F, 0x85, 0x00};
    if (std::memcmp(fade, expectFade, sizeof expectFade) != 0 || std::memcmp(maxSeg, expectMaxSeg, sizeof expectMaxSeg) != 0)
        return false;
    DWORD old;
    if (!VirtualProtect(fade, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    fade[0] = 0xE9;
    *reinterpret_cast<int32_t*>(fade + 1) = int32_t(reinterpret_cast<uintptr_t>(&CannonFadeCave) - (SITE_CannonFade + 5));
    VirtualProtect(fade, 5, old, &old);
    if (!VirtualProtect(maxSeg, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<float**>(maxSeg + 2) = &g_cannonMaxSeg2;
    VirtualProtect(maxSeg, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// The two random directions: six rand() calls of the first drawn segment. CWaterCannon* is at [esp+0x30] of the
// caller (stored at 0x728DB5).
constexpr uintptr_t SITE_CannonRand[6] = {0x728FD2, 0x728FE9, 0x729000, 0x72904A, 0x729061, 0x729078};
constexpr uintptr_t EXPECT_Rand = 0x821B1E;
using RandFn = int(__cdecl*)();
RandFn g_origCannonRand = nullptr;
bool g_cannonRandOn = false;
int g_cannonRand[NumCannons][6];
unsigned g_cannonRandSeed = 0x2545F491u;
int CannonRandNext() {
    g_cannonRandSeed = g_cannonRandSeed * 214013u + 2531011u;
    return int((g_cannonRandSeed >> 16) & 0x7FFF);
}
int __cdecl HookedCannonRand() {
    if (!g_cannonRandOn) return g_origCannonRand();
    const uintptr_t site = reinterpret_cast<uintptr_t>(_ReturnAddress()) - 5;
    int k = 0;
    while (k < 6 && SITE_CannonRand[k] != site) ++k;
    const uintptr_t frame = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) + 4;
    const int c = int((*reinterpret_cast<const uintptr_t*>(frame + 0x30) - ADDR_Cannons) / CannonStride);
    if (k >= 6 || c < 0 || c >= NumCannons) return g_origCannonRand();
    return g_cannonRand[c][k];
}
bool HookCannonRand() {
    for (uintptr_t site : SITE_CannonRand)
        if (!HookCall(site, reinterpret_cast<void*>(&HookedCannonRand), reinterpret_cast<void**>(&g_origCannonRand), EXPECT_Rand))
            return false;
    return true;
}

void RenderCannons(VoidFn orig) {
    if (!g_interpolate || g_logicHz <= 0 || g_tickStep <= 0.0f) { orig(); return; }
    static float saved[NumCannons][CannonPoints][3];
    static float emitPrev[NumCannons][3], emitCurr[NumCannons][3];   // nozzle at the last two ticks
    static int lastCur[NumCannons];
    static bool emitOk[NumCannons];
    static unsigned cannonTick = ~0u;
    const bool newTick = cannonTick != g_ticks;
    cannonTick = g_ticks;
    const float alpha = TickAlpha(), back = g_tickStep * (1.0f - alpha);
    for (int c = 0; c < NumCannons; ++c) {
        uint8_t* w = reinterpret_cast<uint8_t*>(ADDR_Cannons + CannonStride * c);
        if (!*reinterpret_cast<const int*>(w)) { emitOk[c] = false; continue; }
        float* pts = reinterpret_cast<float*>(w + 0x0C);
        const float* spd = reinterpret_cast<const float*>(w + 0x18C);
        const uint8_t* alive = w + 0x30C;
        const int cur = *reinterpret_cast<const int16_t*>(w + 4) & 31;
        if (newTick) {
            for (int k = 0; k < 6; ++k) g_cannonRand[c][k] = CannonRandNext();
            const bool added = emitOk[c] && cur != lastCur[c] && alive[cur];
            std::memcpy(emitPrev[c], added ? emitCurr[c] : pts + cur * 3, 12);
            std::memcpy(emitCurr[c], pts + cur * 3, 12);
            emitOk[c] = alive[cur] != 0;
            lastCur[c] = cur;
        }
        std::memcpy(saved[c], pts, sizeof saved[c]);
        for (int i = 0; i < CannonPoints; ++i) {
            if (!alive[i]) continue;
            float* p = pts + i * 3;
            if (i == cur && emitOk[c])
                for (int k = 0; k < 3; ++k) p[k] = emitPrev[c][k] + (emitCurr[c][k] - emitPrev[c][k]) * alpha;
            else
                for (int k = 0; k < 3; ++k) p[k] -= spd[i * 3 + k] * back;
        }
        if (emitOk[c]) {   // the oldest point (o) slides onto the next one (n): it is freed at the next tick
            int o = -1, n = -1;
            for (int k = 1; k < CannonPoints && n < 0; ++k) {
                const int i = (cur + k) & 31;
                if (!alive[i]) continue;
                if (o < 0) o = i; else n = i;
            }
            if (o >= 0 && n >= 0 && n != cur)
                for (int k = 0; k < 3; ++k) pts[o * 3 + k] += (pts[n * 3 + k] - pts[o * 3 + k]) * alpha;
        }
    }
    const bool cam = BeginCamMatrixBlend();
    g_cannonSegOff = alpha;
    g_cannonRandOn = true;
    orig();
    g_cannonRandOn = false;
    g_cannonSegOff = 0.0f;
    if (cam) EndCamMatrixBlend();
    for (int c = 0; c < NumCannons; ++c) {
        uint8_t* w = reinterpret_cast<uint8_t*>(ADDR_Cannons + CannonStride * c);
        if (*reinterpret_cast<const int*>(w)) std::memcpy(w + 0x0C, saved[c], sizeof saved[c]);
    }
}
void __cdecl HookedCannons1() { RenderCannons(g_origCannons1); }
void __cdecl HookedCannons2() { RenderCannons(g_origCannons2); }

// ---- moving fog -------------------------------------------------------------------------------------------------
// CClouds::MovingFogRender (0x716C90, called on every drawn frame at 0x53E1AF) moves each fog patch by the fog wind
// vector (0xC6E388, a constant set once in CClouds::Init) times its own speed, once per call and not per unit of
// time: drawn at 300 fps the fog flew 10 times too fast. Around the call the wind is scaled by the share of a tick
// this frame stands for (ms_fTimeStep is already that share on drawn frames, see the water gate).
constexpr uintptr_t SITE_MovingFog = 0x53E1AF, EXPECT_MovingFog = 0x716C90, ADDR_FogWind = 0xC6E388;
VoidFn g_origMovingFog = nullptr;
void __cdecl HookedMovingFog() {
    if (!g_smoothFog || g_logicHz <= 0 || g_tickStep <= 0.0f) { g_origMovingFog(); return; }
    float* wind = reinterpret_cast<float*>(ADDR_FogWind);
    float share = *reinterpret_cast<const float*>(ADDR_TimeStepF) / g_tickStep;
    if (!(share >= 0.0f)) share = 0.0f; else if (share > 1.0f) share = 1.0f;
    const float saved[3] = {wind[0], wind[1], wind[2]};
    for (int i = 0; i < 3; ++i) wind[i] = saved[i] * share;
    g_origMovingFog();
    for (int i = 0; i < 3; ++i) wind[i] = saved[i];
}

// ---- mirrors and reflective floors (interiors) ----------------------------------------------------------------
// CMirrors::BeforeConstructRenderList (0x726DF0, called from ConstructRenderList at 0x555854) builds the reflected
// camera from TheCamera.m_mCameraMatrix (0xB6F99C), which the game only updates on simulation ticks: the reflection
// moved at 30 fps while the scene follows the blended camera. Around that call the matrix is blended between its
// last two tick values, then put back.
constexpr uintptr_t SITE_Mirrors = 0x555854, EXPECT_Mirrors = 0x726DF0, ADDR_CamMatrix = 0xB6F99C;
VoidFn g_origMirrors = nullptr;
float g_camMatPrev[16], g_camMatCurr[16];
unsigned g_camMatTick = ~0u;
bool g_camMatValid = false;
void MirrorNorm(float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
// Puts the blended camera matrix in place (returns false if nothing to do); EndCamMatrixBlend puts the real one back.
float g_camMatSaved[16];
extern bool g_camLive;
bool BeginCamMatrixBlend() {
    if (g_camLive) return false;   // the camera is run on every frame: the matrix is this frame's already
    float* m = reinterpret_cast<float*>(ADDR_CamMatrix);
    if (g_ticks != g_camMatTick) {   // first use after a tick: the matrix holds the new tick's camera
        g_camMatTick = g_ticks;
        std::memcpy(g_camMatPrev, g_camMatValid ? g_camMatCurr : m, sizeof g_camMatPrev);
        std::memcpy(g_camMatCurr, m, sizeof g_camMatCurr);
        g_camMatValid = true;
    }
    const float* a = g_camMatPrev; const float* b = g_camMatCurr;
    const float d[3] = {b[12] - a[12], b[13] - a[13], b[14] - a[14]};
    const bool cut = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 25.0f || a[8] * b[8] + a[9] * b[9] + a[10] * b[10] < 0.8f;
    if (!g_interpolate || g_logicHz <= 0 || cut || std::memcmp(m, b, sizeof g_camMatCurr) != 0) return false;
    std::memcpy(g_camMatSaved, m, sizeof g_camMatSaved);
    const float t = TickAlpha();
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 3; ++k) m[r * 4 + k] = a[r * 4 + k] + (b[r * 4 + k] - a[r * 4 + k]) * t;
        if (r < 3) MirrorNorm(m + r * 4);
    }
    return true;
}
void EndCamMatrixBlend() { std::memcpy(reinterpret_cast<float*>(ADDR_CamMatrix), g_camMatSaved, sizeof g_camMatSaved); }
void __cdecl HookedMirrors() {
    const bool on = BeginCamMatrixBlend();
    g_origMirrors();
    if (on) EndCamMatrixBlend();
}

// TheCamera's own placement (CPlaceable matrix, CMatrix* at 0xB6F03C): position and axes of the camera as the game
// code sees them (TheCamera.GetPosition / GetForward). Set on each tick only, while the scene is drawn with the
// blended RW camera. The sky reads it: its gradient was placed from the camera of the last tick, and the horizon
// jumped 30 times a second when the camera moved fast (falling under the map). Blended for the whole drawn frame,
// put back before the next tick.
float g_tcPrev[16], g_tcCurr[16], g_tcSaved[16];
float* g_tcPtr = nullptr;
unsigned g_tcTick = ~0u;
bool g_tcApplied = false;
void RestoreTheCamera() {
    if (g_tcApplied && g_tcPtr && *reinterpret_cast<float**>(0xB6F03C) == g_tcPtr) std::memcpy(g_tcPtr, g_tcSaved, sizeof g_tcSaved);
    g_tcApplied = false;
}
void ApplyTheCamera(float t) {
    RestoreTheCamera();
    float* m = *reinterpret_cast<float**>(0xB6F03C);
    if (!m) { g_tcPtr = nullptr; return; }
    if (g_ticks != g_tcTick || m != g_tcPtr) {   // first frame after a tick: the matrix holds the new tick's camera
        std::memcpy(g_tcPrev, m == g_tcPtr ? g_tcCurr : m, sizeof g_tcPrev);
        std::memcpy(g_tcCurr, m, sizeof g_tcCurr);
        g_tcTick = g_ticks;
        g_tcPtr = m;
    }
    const float* a = g_tcPrev; const float* b = g_tcCurr;
    const float d[3] = {b[12] - a[12], b[13] - a[13], b[14] - a[14]};
    const bool cut = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 25.0f || a[4] * b[4] + a[5] * b[5] + a[6] * b[6] < 0.8f;
    if (cut || std::memcmp(m, b, sizeof g_tcCurr) != 0) return;
    std::memcpy(g_tcSaved, m, sizeof g_tcSaved);
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 3; ++k) m[r * 4 + k] = a[r * 4 + k] + (b[r * 4 + k] - a[r * 4 + k]) * t;
        if (r < 3) MirrorNorm(m + r * 4);
    }
    g_tcApplied = true;
}

// ---- field of view ----------------------------------------------------------------------------------------------
// CDraw::ms_fFOV (0x8D5038) is set by the camera on each tick (zoom of the idle camera, aiming, vehicle camera...)
// and given to the render camera on every drawn frame (0x53EB20): blended between its last two tick values like the
// camera itself, put back before the next tick.
constexpr uintptr_t ADDR_Fov = 0x8D5038;
float g_fovPrev = 0, g_fovCurr = 0;
unsigned g_fovTick = ~0u;
bool g_fovApplied = false;
void ApplyFov(float alpha) {
    float& fov = *reinterpret_cast<float*>(ADDR_Fov);
    if (g_ticks != g_fovTick) {
        g_fovPrev = g_fovTick == ~0u ? fov : g_fovCurr;
        g_fovCurr = fov;
        g_fovTick = g_ticks;
        if (std::fabs(g_fovCurr - g_fovPrev) > 20.0f) g_fovPrev = g_fovCurr;   // camera cut / mode switch: snap
    }
    if (g_fovPrev == g_fovCurr) return;
    fov = g_fovPrev + (g_fovCurr - g_fovPrev) * alpha;
    g_fovApplied = true;
}
void RestoreFov() {
    if (g_fovApplied) *reinterpret_cast<float*>(ADDR_Fov) = g_fovCurr;
    g_fovApplied = false;
}

// ---- camera on every drawn frame ---------------------------------------------------------------------------------
// The camera was simulated with the rest of the game (CCamera::Process 0x52B730, called at 0x53C104 in CGame::Process)
// and shown blended between its last two ticks: what the mouse or the stick did was seen one tick late (33 ms at 30 Hz)
// on top of the wait for the next tick. Here the tick skips its camera update and every drawn frame runs it instead,
// after the entities were blended (the camera follows the player as shown), with the share of the tick's time step
// that elapsed and the mouse moves made since the previous frame: the view answers on the next drawn frame, as in the
// original game running at that frame rate. The camera blends (RW camera, TheCamera, FOV, mirrors) are then off.
// Not during cutscenes (their camera follows the cutscene clock, which only moves on ticks: the blend stays), replays
// or menus.
//
// The mouse: CPad::UpdateMouse (0x53F3C0, called at 0x541DD7 in CPad::UpdatePads once per tick) reads the moves since
// its previous call into NewMouseControllerState (0xB73418: buttons, wheel up/down flags, then z, x, y floats; the
// previous state goes to 0xB7342C). It is also run on the frames between two ticks, for the camera; what those reads
// took is given back to the tick's own read (moves and wheel summed, previous state = the previous tick's), so the
// simulation still sees everything since its last tick (weapon wheel, mouse steering).
constexpr uintptr_t SITE_CameraProcess = 0x53C104, EXPECT_CameraProcess = 0x52B730, ADDR_TheCamera = 0xB6F028;
constexpr uintptr_t SITE_UpdateMouse = 0x541DD7, EXPECT_UpdateMouse = 0x53F3C0;
constexpr uintptr_t ADDR_NewMouse = 0xB73418, ADDR_OldMouse = 0xB7342C, ADDR_Pad0 = 0xB73458;
struct MouseState { uint8_t lmb, rmb, mmb, wheelUp, wheelDown, b1, b2, pad; float z, x, y; };
static_assert(sizeof(MouseState) == 0x14, "CMouseControllerState");
ThisFn g_origCameraProcess = nullptr, g_origUpdateMouse = nullptr;
bool g_camFrame = false;      // ini CameraEveryFrame
bool g_camHooks = false;
bool g_camOwed = false;       // the last tick skipped its camera update: the drawn frames run it
bool g_camLive = false;       // the camera is run on every frame now (no camera blend)
MouseState g_mouseFresh{}, g_mouseTickPrev{};
float g_mouseAcc[3] = {};     // z, x, y taken by the reads between two ticks
bool g_mouseAccUp = false, g_mouseAccDown = false, g_mouseSub = false;
SplitState g_camSplit;
// Only the views the player steers: on foot, behind a vehicle, aiming. Every other camera (scripted and fixed mission
// cameras, the cinematic vehicle camera, wasted / busted, first person...) keeps the tick camera and its blend, as
// before: some of them move by a fixed amount per call (they shook six times too fast when run on every frame), or
// follow something that is not blended yet when the camera runs. Mode of the active camera: TheCamera.Cams[ActiveCam]
// (ActiveCam: byte at +0x59; Cams at +0x174, 0x238 bytes each; Mode: uint16 at +0xC).
bool CamModeSteered() {
    const uint8_t active = *reinterpret_cast<const uint8_t*>(ADDR_TheCamera + 0x59);
    if (active > 2) return false;
    switch (*reinterpret_cast<const uint16_t*>(ADDR_TheCamera + 0x174 + 0x238 * active + 0xC)) {
    case 3: case 4: case 18: case 22:              // behind car, follow ped, cam on a string (vehicles), behind boat
    case 7: case 8: case 46: case 51:              // sniper, rocket launcher, M16 first person, heat seeking launcher
    case 53: case 55: case 65: return true;        // aim weapon: on foot, from a car, attached
    default: return false;
    }
}
// The idle camera (the view that wanders to passers-by when nothing is pressed for a while) hides the HUD by stamping
// the frame counter into 0xB6EC40 each time it runs (0x522D25): stamped on the last tick or since, it is on. It shakes
// by a fixed amount per call: it stays on the tick camera too.
bool IdleCamOn() {
    const uint32_t stamp = *reinterpret_cast<const uint32_t*>(0xB6EC40), ctr = *reinterpret_cast<const uint32_t*>(ADDR_FrameCtrW);
    return stamp == ctr || stamp + 1 == ctr;
}
bool CamFrameWanted() {
    return g_camFrame && g_camHooks && g_logicHz > 0 && g_interpolate && *reinterpret_cast<const int*>(ADDR_GameState) == 9 &&
           !*reinterpret_cast<const uint8_t*>(ADDR_MenuActive) && !*reinterpret_cast<const uint8_t*>(0xB5F851) &&   // CCutsceneMgr::ms_running
           CamModeSteered() && !IdleCamOn();
}
// Idle does not build nor draw the scene while the screen is fully faded out (CCamera::GetScreenFadeStatus() == 2 at
// 0x53E9BE: fade alpha, TheCamera +0xBFC, at 255), so the drawn frames do not reach CameraEveryFrame then. The fade
// itself is run by the camera update: left to the frames, it stayed black for ever after walking through a door. The
// tick keeps its own camera update while the screen is black, and also whenever the frames since the previous tick
// did not run the one they owed (anything else that skips the scene).
bool g_camOwedPrev = false, g_camRan = false;
void __fastcall HookedCameraProcess(void* self, void* edx) {
    const bool black = *reinterpret_cast<const float*>(ADDR_TheCamera + 0xBFC) >= 254.5f;
    const bool missed = g_camOwedPrev && !g_camRan;
    if (CamFrameWanted() && !black && !missed) { g_camOwed = true; g_camRan = false; return; }   // run by the drawn frames
    g_origCameraProcess(self, edx);
}
void __fastcall HookedUpdateMouse(void* self, void* edx) {
    g_origUpdateMouse(self, edx);
    MouseState& n = *reinterpret_cast<MouseState*>(ADDR_NewMouse);
    g_mouseFresh = n;
    if (g_mouseSub) {
        n.z += g_mouseAcc[0]; n.x += g_mouseAcc[1]; n.y += g_mouseAcc[2];
        n.wheelUp |= uint8_t(g_mouseAccUp); n.wheelDown |= uint8_t(g_mouseAccDown);
        *reinterpret_cast<MouseState*>(ADDR_OldMouse) = g_mouseTickPrev;
    }
    g_mouseAcc[0] = g_mouseAcc[1] = g_mouseAcc[2] = 0.0f;
    g_mouseAccUp = g_mouseAccDown = g_mouseSub = false;
    g_mouseTickPrev = n;
}
void CameraEveryFrame() {
    const bool live = g_camOwed && CamFrameWanted();
    if (live != g_camLive) {   // from the blend to the live camera or back: nothing kept from the other way
        g_camLive = live;
        Interp::ForgetCamera();
        RestoreFov();
        RestoreTheCamera();
        g_camMatValid = false;
        g_camSplit.last = 0;
    }
    if (!live) return;
    MouseState& n = *reinterpret_cast<MouseState*>(ADDR_NewMouse);
    uint8_t* pad = reinterpret_cast<uint8_t*>(ADDR_Pad0);   // CPad: NewState (0x30 bytes), then OldState
    const bool tickFrame = !g_renderOnly;
    const MouseState tickMouse = n;
    uint8_t oldState[0x30];
    if (tickFrame) n = g_mouseFresh;   // the tick's own read: what was not shown yet
    else {
        n.z = n.x = n.y = 0.0f; n.wheelUp = n.wheelDown = 0;   // stays so if the mouse can't be read (window not active)
        g_origUpdateMouse(pad, nullptr);
        g_mouseAcc[0] += n.z; g_mouseAcc[1] += n.x; g_mouseAcc[2] += n.y;
        g_mouseAccUp |= n.wheelUp != 0; g_mouseAccDown |= n.wheelDown != 0;
        g_mouseSub = true;
        // a button pressed at the last tick is "just pressed" until the next one: only the tick frame may see that
        // (change of view: once per press, not once per drawn frame)
        std::memcpy(oldState, pad + 0x30, 0x30);
        std::memcpy(pad + 0x30, pad, 0x30);
    }
    float& step = *reinterpret_cast<float*>(ADDR_TimeStepF);
    const float savedStep = step;
    step = g_tickStep * SplitFrac(g_camSplit);
    { ClockShift shift; g_origCameraProcess(reinterpret_cast<void*>(ADDR_TheCamera), nullptr); }
    g_camRan = true;
    step = savedStep;
    if (tickFrame) n = tickMouse;
    else std::memcpy(pad + 0x30, oldState, 0x30);
}

void __cdecl HookedConstructList() {
    Telemetry::Stamp(Telemetry::M_ListStart);
    // The sea fills draw buckets in this function once per simulated frame and the buckets are emptied when drawn,
    // so on render-only frames it has to run again or the water is missing.
    if (g_logicHz > 0 && g_renderOnly && g_waterRedo) { reinterpret_cast<void(__cdecl*)()>(SITE_WaterFn)(); RestoreWaterCounter(); }
    const double t0 = Interp::NowUs();
    if (g_interpolate) Interp::ApplyEntities(TickAlpha());
    CameraEveryFrame();   // after the blend: the camera follows the shown player
    if (g_logicHz > 0 && g_renderOnly) SplitRenderOnly();   // after the blend: effects follow the shown entities
    if (g_logicHz > 0) ReleaseParticles(TickAlpha());   // what the simulation emitted, spread over the drawn frames
    UpdateStreakStep();
    // the camera too (blended again before RwCameraBeginUpdate): PreRender, shadows and effects see the shown one
    if (g_interpolate && !g_camLive) Interp::ApplyCamera(!g_renderOnly, TickAlpha());
    if (g_interpolate && !g_camLive) ApplyFov(TickAlpha()); else RestoreFov();
    RestoreTheCamera();   // the render list is built with the tick camera (blending it there made objects blink)
    if (g_interpolate && g_logicHz > 0) ApplySun(TickAlpha()); else RestoreSun();
    if (g_interpolate && g_logicHz > 0) UpdateStencilShadows();
    Telemetry::Stamp(Telemetry::M_BlendEnd);
    const double t1 = Interp::NowUs();
    g_origConstructList();
    if (g_interpolate && g_logicHz > 0 && !g_camLive) ApplyTheCamera(TickAlpha());   // PreRender, sky, effects: the shown camera
    const double t2 = Interp::NowUs();
    Interp::g_timing.applyEntUs += t1 - t0;
    Interp::g_timing.constructUs += t2 - t1;
    ++Interp::g_timing.frames;
    Telemetry::Stamp(Telemetry::M_ListEnd);
}

// CRealTimeShadowManager::Update (0x706AB0, thiscall on 0xC40350) draws the ped shadows from their skeletons right
// after PreRender: the skeletons are blended just before it, or the shadows keep the 30 Hz pose.
constexpr uintptr_t SITE_RtShadows = 0x53EA0D, EXPECT_RtShadows = 0x706AB0;
using RtShadowsFn = void(__fastcall*)(void*, void*);
RtShadowsFn g_origRtShadows = nullptr;
bool g_hierDone = false;
void __fastcall HookedRtShadows(void* self, void* edx) {
    if (g_interpolate && !g_hierDone) { Interp::ApplyHierarchies(!g_renderOnly, TickAlpha()); g_hierDone = true; }
    g_origRtShadows(self, edx);
}
// Props held by peds (bottle, cigarette, box...): just before the shadows, 0x563430 runs
// CPedIntelligence::ProcessAfterPreRender (0x6019B0) for every ped, which puts the held object (task
// TASK_SIMPLE_HOLD_ENTITY, 0x133) on the hand bone. The skeletons were blended only after it: the prop followed the
// 30 Hz hand. They are blended before it now.
constexpr uintptr_t SITE_PedsAfterPreRender = 0x53EA03, EXPECT_PedsAfterPreRender = 0x563430;
VoidFn g_origPedsAfterPreRender = nullptr;
void __cdecl HookedPedsAfterPreRender() {
    if (g_interpolate && !g_hierDone) { Interp::ApplyHierarchies(!g_renderOnly, TickAlpha()); g_hierDone = true; }
    g_origPedsAfterPreRender();
}

char __cdecl HookedCameraBegin(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) {
    const double t0 = Interp::NowUs();
    if (g_interpolate) {
        if (!g_hierDone) Interp::ApplyHierarchies(!g_renderOnly, TickAlpha());
        if (!g_camLive) Interp::ApplyCamera(!g_renderOnly, TickAlpha());
    }
    g_hierDone = false;
    Interp::g_timing.applyCamUs += Interp::NowUs() - t0;
    Telemetry::Stamp(Telemetry::M_SceneStart);
    return g_origCameraBegin(a, b, c, d, e, f, g);
}

// Precise frame cap: sleep for most of the wait, spin for the last ~1.5 ms.
void LimitFrame() {
    if (g_fpsCap <= 0) return;
    static LARGE_INTEGER freq, next;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); timeBeginPeriod(1); }
    const LONGLONG period = freq.QuadPart / g_fpsCap;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (next.QuadPart && now.QuadPart < next.QuadPart) {
        for (;;) {
            QueryPerformanceCounter(&now);
            const LONGLONG left = next.QuadPart - now.QuadPart;
            if (left <= 0) break;
            if (left * 1000 / freq.QuadPart >= 2) Sleep(1); else YieldProcessor();
        }
    }
    // If we fell behind (loading screen, hitch), restart the schedule instead of catching up.
    next.QuadPart = (now.QuadPart > next.QuadPart + period) ? now.QuadPart + period : next.QuadPart + period;
    g_nextPresentQpc = next.QuadPart;   // when the next frame will be shown
}

// ---- startup video mode and frame limiter ------------------------------------------------------------------
// psSelectDevice (0x746190) takes the video mode saved in gta_sa.set (CMenuManager::m_nPrefsVideoMode) if the list of
// usable modes (0x745AF0) accepts it. Right after that list is built, the mode matching the desktop is put there.
constexpr uintptr_t SITE_VideoModes = 0x74629C, EXPECT_VideoModes = 0x745AF0;
constexpr uintptr_t ADDR_PrefsVideoMode = 0xBA6820;
constexpr uintptr_t ADDR_RwEngineGetNumVideoModes = 0x7F2CC0, ADDR_RwEngineGetVideoModeInfo = 0x7F2CF0;
constexpr uintptr_t ADDR_FrameLimiter = 0xBA6794;   // CMenuManager::m_bFrameLimiterOn (30 fps cap of the main loop)
struct RwVideoMode { int width, height, depth, flags, refRate, format, pad[4]; };   // flags & 1 = exclusive (fullscreen)
using VideoModesFn = const int*(__cdecl*)();
VideoModesFn g_origVideoModes = nullptr;
bool g_noFrameLimiter = true;

const int* __cdecl HookedVideoModes() {
    const int* usable = g_origVideoModes();
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (!usable || !EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return usable;
    const int n = reinterpret_cast<int(__cdecl*)()>(ADDR_RwEngineGetNumVideoModes)();
    int best = -1, bestDepth = 0;
    for (int i = 1; i < n; ++i) {   // mode 0 is the windowed one
        RwVideoMode m{};
        reinterpret_cast<void*(__cdecl*)(RwVideoMode*, int)>(ADDR_RwEngineGetVideoModeInfo)(&m, i);
        if (!usable[i] || !(m.flags & 1)) continue;
        if (m.width == int(dm.dmPelsWidth) && m.height == int(dm.dmPelsHeight) && m.depth > bestDepth) { best = i; bestDepth = m.depth; }
    }
    if (best > 0) {
        *reinterpret_cast<int*>(ADDR_PrefsVideoMode) = best;
        Log("desktop resolution %lux%lu: video mode %d", dm.dmPelsWidth, dm.dmPelsHeight, best);
    } else Log("desktop resolution %lux%lu: no matching video mode, keeping the saved one", dm.dmPelsWidth, dm.dmPelsHeight);
    if (g_noFrameLimiter) *reinterpret_cast<uint8_t*>(ADDR_FrameLimiter) = 0;
    return usable;
}

HRESULT WINAPI HookedPresent(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    Telemetry::Stamp(Telemetry::M_PresentHook);
    if (g_noFrameLimiter) *reinterpret_cast<volatile uint8_t*>(ADDR_FrameLimiter) = 0;   // the menu or gta_sa.set may turn it on
    FlushGuard();
    UpdatePrevCam();   // this frame's camera, for the next frame's spark streaks
    ++g_presentCount;
    Telemetry::OnFrame();
    if (g_borderless && g_bwnd) {   // the game restyles its window at times (caption/border): keep it borderless
        static unsigned n = 0;
        if ((++n & 15) == 0) {
            const LONG st = GetWindowLongW(g_bwnd, GWL_STYLE);
            RECT r{}; GetWindowRect(g_bwnd, &r);
            if ((st & (WS_CAPTION | WS_THICKFRAME)) || r.left != g_bx || r.top != g_by || r.right - r.left != g_bw || r.bottom - r.top != g_bh) {
                SetWindowLongW(g_bwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(g_bwnd, HWND_TOP, g_bx, g_by, g_bw, g_bh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                Log("borderless window re-applied (style %08lX rect %ld,%ld %ldx%ld)", st, r.left, r.top, r.right - r.left, r.bottom - r.top);
            }
        }
    }
    Telemetry::Stamp(Telemetry::M_PresentIn);
    LimitFrame();   // before Present: the frame is shown at its scheduled time (see FrameQpc)
    Telemetry::Stamp(Telemetry::M_WaitEnd);
    const HRESULT hr = g_origPresent(dev, src, dst, wnd, dirty);
    if (g_vsync && g_fpsCap <= 0) {
        // VSync paces the frames: each one is shown one refresh after the previous. The next frame is blended for that
        // moment on an even grid of refreshes (not "now" when it starts: tick frames start later, the simulation runs
        // first, but are shown on the same beat), pulled gently towards the real Present times so it never drifts.
        static LARGE_INTEGER freq;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const LONGLONG period = freq.QuadPart / (g_monitorHz > 0 ? g_monitorHz : 60);
        LONGLONG next = g_nextPresentQpc + period;
        const LONGLONG target = now.QuadPart + period;
        if (next < now.QuadPart || next > target + 2 * period) next = target;   // fell behind (hitch, loading) or ran ahead
        else next += (target - next) / 32;
        g_nextPresentQpc = next;
    }
    Telemetry::EndFrame(!g_renderOnly, *reinterpret_cast<const int*>(ADDR_GameState) == 9 &&
                                           !*reinterpret_cast<const volatile uint8_t*>(ADDR_MenuActive));
    return hr;
}

// The game's window (RsGlobal.ps->window: [[0xC17054]]), null before the game made it.
HWND GameWindow() {
    const uintptr_t ps = *reinterpret_cast<const uintptr_t*>(0xC17054);
    return ps ? *reinterpret_cast<const HWND*>(ps) : nullptr;
}

HRESULT WINAPI HookedCreateDevice(void* d3d, UINT adapter, UINT type, HWND wnd, DWORD flags,
                                  PresentParams* pp, void** outDev) {
    // another mod's own device (e.g. a dummy one on a hidden window to find the D3D9 functions): left as it is,
    // and its window must not become "the game window" (F9 menu focus, borderless style, refresh rate)
    const HWND devWnd = wnd ? wnd : (pp ? pp->hDeviceWindow : nullptr), game = GameWindow();
    const bool ours = !game || devWnd == game;
    if (ours) {
        Fix(adapter, pp, "CreateDevice");
        if (wnd) { g_gameWnd = wnd; ApplyFpsCap(wnd); }
    } else Log("CreateDevice on another window (%p, not the game's %p): left as it is", devWnd, game);
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, outDev);
    if (ours && hr == 0 && pp) ApplyBorderless(pp->hDeviceWindow ? pp->hDeviceWindow : wnd, pp);
    if (hr == 0 && outDev && *outDev && !g_origReset) {
        if (!PatchVTable(*outDev, VT_Reset, reinterpret_cast<void*>(&HookedReset),
                         reinterpret_cast<void**>(&g_origReset)))
            Log("failed to hook Reset");
    }
    if (hr == 0 && outDev && *outDev && !g_origPresent) {
        if (!PatchVTable(*outDev, VT_Present, reinterpret_cast<void*>(&HookedPresent),
                         reinterpret_cast<void**>(&g_origPresent)))
            Log("failed to hook Present");
    }
    return hr;
}

void Init(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* dot = wcsrchr(path, L'.');
    if (dot) *dot = 0;
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s.ini", path);
    swprintf_s(g_logPath, L"%s.log", path);

    g_log = GetPrivateProfileIntW(L"Renewed120", L"Log", 1, ini) != 0;
    if (g_log) { FILE* f = nullptr; if (_wfopen_s(&f, g_logPath, L"w") == 0 && f) fclose(f); }  // truncate
    g_fixRefresh = GetPrivateProfileIntW(L"Renewed120", L"RefreshRateFix", 1, ini) != 0;
    g_telemetry = GetPrivateProfileIntW(L"Renewed120", L"Telemetry", 0, ini) != 0;
    g_fpsCapSetting = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"FpsCap", 0, ini));
    g_vsync = GetPrivateProfileIntW(L"Renewed120", L"VSync", 0, ini) != 0;
    if (g_vsync) {
        // psCameraShowRaster (0x745240) asks RwCameraShowRaster for rwRASTERFLIPWAITVSYNC only when the frame limiter
        // (0xBA6794) or 0xBAB318 is on: "push 0" at 0x745252 -> "push 1", so RenderWare itself sets the D3D present
        // interval to one (setting it behind its back makes it reset the device on every frame)
        uint8_t* p = reinterpret_cast<uint8_t*>(0x745252);
        DWORD old;
        if (p[0] == 0x6A && p[1] == 0x00 && VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) {
            p[1] = 0x01;
            VirtualProtect(p, 2, old, &old);
            Log("VSync: on");
        } else { Log("VSync: unexpected code at 0x745252, off"); g_vsync = false; }
    } else Log("VSync: off");
    ApplyFpsCap(nullptr);
    g_logicHz = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"LogicHz", 30, ini));
    if (g_logicHz < 0 || g_logicHz > 60) g_logicHz = 0;
    if (!g_fixRefresh && !g_telemetry && g_fpsCapSetting < 0 && g_logicHz <= 0) { Log("all modules disabled"); return; }
    if (g_logicHz > 0) {
        if (HookCall(SITE_TimerUpdate, reinterpret_cast<void*>(&HookedTimerUpdate),
                     reinterpret_cast<void**>(&g_origTimerUpdate), EXPECT_TimerUpdate) &&
            HookCall(SITE_GameProcess, reinterpret_cast<void*>(&HookedGameProcess),
                     reinterpret_cast<void**>(&g_origGameProcess), EXPECT_GameProcess))
            Log("logic/render split active: LogicHz=%d", g_logicHz);
        else { Log("logic/render split FAILED, running without it"); g_logicHz = 0; }
    }
    if (g_logicHz > 0) {
        if (HookCall(SITE_WaterFn, reinterpret_cast<void*>(&HookedWaterGate), reinterpret_cast<void**>(&g_origWaterGate), EXPECT_WaterFn))
            Log("water timing fix active");
    }
    if (g_logicHz > 0) {
        if (HookCall(SITE_ConstructList, reinterpret_cast<void*>(&HookedConstructList),
                     reinterpret_cast<void**>(&g_origConstructList), EXPECT_ConstructList) &&
            HookCall(SITE_CameraBegin, reinterpret_cast<void*>(&HookedCameraBegin),
                     reinterpret_cast<void**>(&g_origCameraBegin), EXPECT_CameraBegin)) {
            g_interpolate = GetPrivateProfileIntW(L"Renewed120", L"Interpolate", 1, ini) != 0;
            Log("interpolation hooks installed (enabled=%d)", int(g_interpolate));
            if (HookCall(SITE_RtShadows, reinterpret_cast<void*>(&HookedRtShadows), reinterpret_cast<void**>(&g_origRtShadows), EXPECT_RtShadows))
                Log("skeletons blended before the ped shadows");
            if (HookCall(SITE_PedsAfterPreRender, reinterpret_cast<void*>(&HookedPedsAfterPreRender),
                         reinterpret_cast<void**>(&g_origPedsAfterPreRender), EXPECT_PedsAfterPreRender))
                Log("skeletons blended before the held props are placed");
            if (HookCall(SITE_Mirrors, reinterpret_cast<void*>(&HookedMirrors), reinterpret_cast<void**>(&g_origMirrors), EXPECT_Mirrors))
                Log("mirror camera blended");
            g_smoothFog = GetPrivateProfileIntW(L"Renewed120", L"SmoothFog", 1, ini) != 0;
            if (HookCall(SITE_MovingFog, reinterpret_cast<void*>(&HookedMovingFog), reinterpret_cast<void**>(&g_origMovingFog), EXPECT_MovingFog))
                Log("moving fog speed fix installed (enabled=%d)", int(g_smoothFog));
            if (HookCall(SITE_Birds1, reinterpret_cast<void*>(&HookedBirds1), reinterpret_cast<void**>(&g_origBirds1), EXPECT_Birds) &&
                HookCall(SITE_Birds2, reinterpret_cast<void*>(&HookedBirds2), reinterpret_cast<void**>(&g_origBirds2), EXPECT_Birds))
                Log("birds blended");
            if (HookCall(SITE_Glass1, reinterpret_cast<void*>(&HookedGlass1), reinterpret_cast<void**>(&g_origGlass1), EXPECT_Glass) &&
                HookCall(SITE_Glass2, reinterpret_cast<void*>(&HookedGlass2), reinterpret_cast<void**>(&g_origGlass2), EXPECT_Glass))
                Log("broken glass blended");
            if (HookCall(SITE_Cannons1, reinterpret_cast<void*>(&HookedCannons1), reinterpret_cast<void**>(&g_origCannons1), EXPECT_Cannons) &&
                HookCall(SITE_Cannons2, reinterpret_cast<void*>(&HookedCannons2), reinterpret_cast<void**>(&g_origCannons2), EXPECT_Cannons))
                Log("water cannons blended (render patch=%d, rand hook=%d)", int(PatchCannonRender()), int(HookCannonRand()));
        } else Log("interpolation hooks FAILED, running without interpolation");
    }
    if (g_logicHz > 0 && HookCall(SITE_PreRender, reinterpret_cast<void*>(&HookedPreRender), reinterpret_cast<void**>(&g_origPreRender), EXPECT_PreRender))
        Log("vehicle physics guard installed");
    if (g_logicHz > 0 && PatchCarLightStep()) Log("car light step patch installed");
    g_smoothSun = GetPrivateProfileIntW(L"Renewed120", L"SmoothSun", 1, ini) != 0;
    if (GetPrivateProfileIntW(L"Renewed120", L"SmoothCarLights", 1, ini) != 0 && PatchCarLightAlwaysMoving()) Log("car light per-frame patch installed");
    if (PatchVehShadowAlwaysMoving()) {
        SetVehShadowPatch(GetPrivateProfileIntW(L"Renewed120", L"SmoothCarShadows", 1, ini) != 0);
        Log("car shadow per-frame patch installed (enabled=%d)", int(g_smoothCarShadows));
    }
    g_camFrame = GetPrivateProfileIntW(L"Renewed120", L"CameraEveryFrame", 0, ini) != 0;
    if (g_logicHz > 0 &&
        HookCall(SITE_CameraProcess, reinterpret_cast<void*>(&HookedCameraProcess), reinterpret_cast<void**>(&g_origCameraProcess), EXPECT_CameraProcess) &&
        HookCall(SITE_UpdateMouse, reinterpret_cast<void*>(&HookedUpdateMouse), reinterpret_cast<void**>(&g_origUpdateMouse), EXPECT_UpdateMouse)) {
        g_camHooks = true;
        Log("camera on every frame installed (enabled=%d)", int(g_camFrame));
    }
    g_staticFix = GetPrivateProfileIntW(L"Renewed120", L"StaticShadowFix", 1, ini) != 0;
    g_tickLights = GetPrivateProfileIntW(L"Renewed120", L"LightsEveryFrame", 1, ini) != 0;
    Log("lights and shadows of the simulation kept on every frame: %d", int(g_tickLights));
    if (g_logicHz > 0 &&
        HookCall(SITE_UpdStatic1, reinterpret_cast<void*>(&HookedUpdStatic1), reinterpret_cast<void**>(&g_origUpdStatic1), EXPECT_UpdStatic) &&
        HookCall(SITE_UpdStatic2, reinterpret_cast<void*>(&HookedUpdStatic2), reinterpret_cast<void**>(&g_origUpdStatic2), EXPECT_UpdStatic))
        Log("static shadow fix installed (enabled=%d)", int(g_staticFix));
    if (g_logicHz > 0 && HookCall(SITE_Markers, reinterpret_cast<void*>(&HookedMarkers), reinterpret_cast<void**>(&g_origMarkers), EXPECT_Markers))
        Log("3D markers fix installed");
    if (g_logicHz > 0 &&
        HookCall(SITE_Ropes1, reinterpret_cast<void*>(&HookedRopes1), reinterpret_cast<void**>(&g_origRopes1), EXPECT_Ropes) &&
        HookCall(SITE_Ropes2, reinterpret_cast<void*>(&HookedRopes2), reinterpret_cast<void**>(&g_origRopes2), EXPECT_Ropes))
        Log("ropes blended");
    g_smoothClock = GetPrivateProfileIntW(L"Renewed120", L"SmoothClock", 1, ini) != 0;
    g_stencilFrame = GetPrivateProfileIntW(L"Renewed120", L"CarStencilEachFrame", 1, ini) != 0;
    if (g_logicHz > 0 &&
        HookCall(SITE_RenderWater1, reinterpret_cast<void*>(&HookedRenderWater), reinterpret_cast<void**>(&g_origRenderWater), EXPECT_RenderWater) &&
        HookCall(SITE_RenderWater2, reinterpret_cast<void*>(&HookedRenderWater), reinterpret_cast<void**>(&g_origRenderWater), EXPECT_RenderWater) &&
        HookCall(SITE_RenderWater3, reinterpret_cast<void*>(&HookedRenderWater), reinterpret_cast<void**>(&g_origRenderWater), EXPECT_RenderWater))
        Log("smooth waves installed (enabled=%d)", int(g_smoothClock));
    if (g_logicHz > 0 && HookCall(SITE_PickupFx, reinterpret_cast<void*>(&HookedPickupFx), reinterpret_cast<void**>(&g_origPickupFx), EXPECT_PickupFx))
        Log("smooth pickups installed (enabled=%d)", int(g_smoothClock));
    if (g_logicHz > 0 && HookCall(SITE_PickupFx2, reinterpret_cast<void*>(&HookedPickupFx2), reinterpret_cast<void**>(&g_origPickupFx2), EXPECT_PickupFx2))
        Log("smooth special pickups installed");
    if (g_logicHz > 0 && HookCall(SITE_MoneyFx, reinterpret_cast<void*>(&HookedMoneyFx), reinterpret_cast<void**>(&g_origMoneyFx), EXPECT_MoneyFx) &&
        HookCall(SITE_CollectFx, reinterpret_cast<void*>(&HookedCollectFx), reinterpret_cast<void**>(&g_origCollectFx), EXPECT_CollectFx))
        Log("money and collectables spin smoothly");
    g_smoothSparks = GetPrivateProfileIntW(L"Renewed120", L"SmoothSparks", 1, ini) != 0;
    if (g_logicHz > 0 && HookAddParticle()) {
        Log("smooth sparks/dust emission installed (enabled=%d)", int(g_smoothSparks));
        int n = 0;
        for (uintptr_t site : SITES_AddSparks)
            n += HookCall(site, reinterpret_cast<void*>(&HookedAddSparks), reinterpret_cast<void**>(&g_origAddSparks), EXPECT_AddSparks) ? 1 : 0;
        Log("spark calls hooked: %d of %d", n, int(sizeof(SITES_AddSparks) / sizeof(SITES_AddSparks[0])));
    }
    if (g_logicHz > 0 && PatchSparkStreak()) Log("spark streaks over one drawn frame");
    g_splitFx = GetPrivateProfileIntW(L"Renewed120", L"SmoothFx", 1, ini) != 0;
    if (g_logicHz > 0 &&
        HookCall(SITE_FxUpdate, reinterpret_cast<void*>(&HookedFx), reinterpret_cast<void**>(&g_origFx), EXPECT_FxUpdate) &&
        HookCall(SITE_BreakUpdate, reinterpret_cast<void*>(&HookedBreak), reinterpret_cast<void**>(&g_origBreak), EXPECT_BreakUpdate))
        Log("smooth particles/debris installed (enabled=%d)", int(g_splitFx));
    wcscpy_s(g_iniPath, ini);
    if (GetPrivateProfileIntW(L"Renewed120", L"DevMenu", 1, ini) != 0 &&
        HookCall(SITE_Render2d, reinterpret_cast<void*>(&HookedRender2d), reinterpret_cast<void**>(&g_origRender2d), EXPECT_Render2d))
        Log("dev menu active (F9)");
    if (g_telemetry) {
        wchar_t csv[MAX_PATH];
        swprintf_s(csv, L"%s.telemetry.csv", path);
        Telemetry::Start(csv);
    }
    g_audioTickRate = GetPrivateProfileIntW(L"Renewed120", L"AudioAtLogicRate", 1, ini) != 0;
    if ((g_telemetry || (g_audioTickRate && g_logicHz > 0)) &&
        HookCall(SITE_Audio, reinterpret_cast<void*>(&HookedAudio), reinterpret_cast<void**>(&g_origAudio), EXPECT_Audio))
        Log("audio service: %s", g_audioTickRate && g_logicHz > 0 ? "logic rate" : "every frame");
    if (g_telemetry) {
        HookCall(SITE_RwBegin, reinterpret_cast<void*>(&HookedRwBegin), reinterpret_cast<void**>(&g_origRwBegin), EXPECT_RwBegin);
    }
    g_noFrameLimiter = GetPrivateProfileIntW(L"Renewed120", L"NoFrameLimiter", 1, ini) != 0;
    if (GetPrivateProfileIntW(L"Renewed120", L"DesktopResolution", 1, ini) != 0 &&
        HookCall(SITE_VideoModes, reinterpret_cast<void*>(&HookedVideoModes), reinterpret_cast<void**>(&g_origVideoModes), EXPECT_VideoModes))
        Log("desktop resolution at startup");
    g_forceHz = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"ForceHz", 0, ini));
    g_windowMode = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"WindowMode", 1, ini));
    Interp::SetVehicleParts(GetPrivateProfileIntW(L"Renewed120", L"VehicleParts", 1, ini) != 0);
    Interp::SetPedSkeleton(GetPrivateProfileIntW(L"Renewed120", L"PedSkeleton", 1, ini) != 0);
    Interp::SetCutsceneSkeleton(GetPrivateProfileIntW(L"Renewed120", L"CutsceneSkeleton", 1, ini) != 0);
    g_streamEveryFrame = GetPrivateProfileIntW(L"Renewed120", L"StreamEveryFrame", 1, ini) != 0;
    g_waterRedo = GetPrivateProfileIntW(L"Renewed120", L"WaterFix", 1, ini) != 0;
    g_freezeVeh = GetPrivateProfileIntW(L"Renewed120", L"VehicleGuard", 1, ini) != 0;
    g_fixedStep = GetPrivateProfileIntW(L"Renewed120", L"FixedStep", 1, ini) != 0;

    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    using CreateFn = void*(WINAPI*)(UINT);
    auto create = d3d9 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    g_d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!g_d3d) { Log("Direct3DCreate9 failed"); return; }
    if (!PatchVTable(g_d3d, VT_CreateDevice, reinterpret_cast<void*>(&HookedCreateDevice),
                     reinterpret_cast<void**>(&g_origCreateDevice))) { Log("failed to hook CreateDevice"); return; }
    Log("Renewed120 ready (RefreshRateFix=%d ForceHz=%d Telemetry=%d)", g_fixRefresh, g_forceHz, g_telemetry);
}

}  // namespace

// Called by Ultimate ASI Loader outside the loader lock.
extern "C" __declspec(dllexport) void InitializeASI() {
    static bool done = false;
    if (done) return;
    done = true;
    Init(g_self);
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = mod; DisableThreadLibraryCalls(mod); }
    return TRUE;
}
