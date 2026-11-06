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
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
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
    g_fpsCap = g_fpsCapSetting > 0 ? g_fpsCapSetting : (g_fpsCapSetting == 0 ? MonitorHz(wnd) : 0);
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
LONGLONG FrameQpc() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (g_fpsCap > 0 && g_nextPresentQpc > now.QuadPart - g_tickPeriodQpc) ? g_nextPresentQpc : now.QuadPart;
}
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

char __cdecl HookedWaterGate() {
    if (g_logicHz > 0) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
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

void SaveIni() {
    auto put = [](const wchar_t* k, int v) { wchar_t b[16]; swprintf_s(b, L"%d", v); WritePrivateProfileStringW(L"Renewed120", k, b, g_iniPath); };
    put(L"Interpolate", g_interpolate); put(L"VehicleParts", Interp::VehicleParts()); put(L"PedSkeleton", Interp::PedSkeleton());
    put(L"CutsceneSkeleton", Interp::CutsceneSkeleton()); put(L"WaterFix", g_waterRedo); put(L"StaticShadowFix", g_staticFix);
    put(L"SmoothFx", g_splitFx); put(L"StreamEveryFrame", g_streamEveryFrame); put(L"VehicleGuard", g_freezeVeh);
    put(L"FpsCap", g_fpsCapSetting); put(L"LogicHz", g_logicHz);
}

constexpr int MenuItems = 12;
void MenuChange(int dir) {   // dir: -1 / +1 (Enter = +1)
    switch (g_menuSel) {
    case 0: if (g_interpolate) Interp::Restore(); g_interpolate = !g_interpolate; break;
    case 1: Interp::Restore(); Interp::SetVehicleParts(!Interp::VehicleParts()); break;
    case 2: Interp::Restore(); Interp::SetPedSkeleton(!Interp::PedSkeleton()); break;
    case 3: Interp::Restore(); Interp::SetCutsceneSkeleton(!Interp::CutsceneSkeleton()); break;
    case 4: g_waterRedo = !g_waterRedo; break;
    case 5: g_staticFix = !g_staticFix; break;
    case 6: g_splitFx = !g_splitFx; break;
    case 7: g_streamEveryFrame = !g_streamEveryFrame; break;
    case 8: g_freezeVeh = !g_freezeVeh; break;
    case 9: g_fpsCapSetting = FpsCaps[CycleIndex(FpsCaps, g_fpsCapSetting, dir)]; ApplyFpsCap(g_gameWnd); break;
    case 10: g_logicHz = LogicRates[CycleIndex(LogicRates, g_logicHz, dir)]; break;
    case 11: SaveIni(); break;
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
        case 0: name = "Interpolation"; onOff(g_interpolate); break;
        case 1: name = "Vehicle parts (wheels/doors)"; onOff(Interp::VehicleParts()); break;
        case 2: name = "Ped skeleton"; onOff(Interp::PedSkeleton()); break;
        case 3: name = "Cutscene skeletons"; onOff(Interp::CutsceneSkeleton()); break;
        case 4: name = "Water fix"; onOff(g_waterRedo); break;
        case 5: name = "Static shadow fix (blood)"; onOff(g_staticFix); break;
        case 6: name = "Smooth particles/debris"; onOff(g_splitFx); break;
        case 7: name = "Load models every frame"; onOff(g_streamEveryFrame); break;
        case 8: name = "Physics guard (veh+peds)"; onOff(g_freezeVeh); break;
        case 9: name = "FPS cap";
            if (g_fpsCapSetting == 0) sprintf_s(val, "auto (%d)", g_fpsCap); else if (g_fpsCap > 0) sprintf_s(val, "%d", g_fpsCap); else strcpy_s(val, "off");
            break;
        case 10: name = "Logic rate (Hz)"; sprintf_s(val, "%d", g_logicHz); break;
        case 11: name = "Save to ini"; strcpy_s(val, "[Enter]"); break;
        }
        snprintf(line, sizeof(line), "%s %s: %s", i == g_menuSel ? ">" : " ", name, val);
        print(line, i == g_menuSel ? YELLOW : WHITE);
    }
    reinterpret_cast<VoidFn>(0x719840)();                // RenderFontBuffer: draw now
}

void __cdecl HookedRender2d() {
    g_origRender2d();
    DrawMenu();
}

void FlushGuard();
void __cdecl HookedTimerUpdate() {
    static LARGE_INTEGER freq, nextTick;
    if (g_logicHz <= 0) { g_origTimerUpdate(); return; }   // split disabled after a partial hook failure
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    const uint8_t menu = *reinterpret_cast<volatile uint8_t*>(ADDR_MenuActive);
    LARGE_INTEGER now; now.QuadPart = FrameQpc();   // the time this frame will be shown
    const LONGLONG period = freq.QuadPart / g_logicHz;

    // Menus/loading always run the full original path.
    bool tick = menu != 0 || nextTick.QuadPart == 0;
    // Otherwise tick when the next logic step is due (half a render frame of tolerance).
    if (!tick) {
        const LONGLONG tol = g_fpsCap > 0 ? freq.QuadPart / g_fpsCap / 2 : 0;
        tick = now.QuadPart + tol >= nextTick.QuadPart;
    }
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
        const double t1 = Interp::NowUs();
        g_lastTickQpc = sched;   // on the fixed schedule: blend steps stay even
        g_tickPeriodQpc = period;
        g_origTimerUpdate();
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
constexpr uintptr_t ADDR_LoadRequestedModels = 0x40E3A0, ADDR_GameState = 0xC8D4C0;   // gGameState: 9 = playing
bool g_streamEveryFrame = true;
void __cdecl HookedGameProcess() {
    if (g_logicHz > 0 && g_renderOnly) {
        // only in the running game (gGameState 9), never in the pause/main menu (FrontEndMenuManager.m_bMenuActive)
        if (g_streamEveryFrame && *reinterpret_cast<const int*>(ADDR_GameState) == 9 && !*reinterpret_cast<const uint8_t*>(ADDR_MenuActive))
            reinterpret_cast<VoidFn>(ADDR_LoadRequestedModels)();
        return;
    }
    const double t0 = Interp::NowUs();
    g_origGameProcess();
    const double t1 = Interp::NowUs();
    if (g_interpolate) Interp::OnTick();
    const double t2 = Interp::NowUs();
    Interp::g_timing.processUs += t1 - t0;
    Interp::g_timing.onTickUs += t2 - t1;
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

void __cdecl HookedPreRender() {
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
        g_origPreRender();
        g_guardPending = true;   // rendering must see PreRender's result; the state is put back at the end of the frame
    } else {
        *ts = real;   // the tick frame advances the state by one full step
        g_origPreRender();
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
float SplitFrac(SplitState& s) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
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
    if (g_fx.have && g_origFx) g_origFx(g_fx.self, g_fx.cam, g_fx.step * SplitFrac(g_fx));
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

void __cdecl HookedConstructList() {
    // The sea fills draw buckets in this function once per simulated frame and the buckets are emptied when drawn,
    // so on render-only frames it has to run again or the water is missing.
    if (g_logicHz > 0 && g_renderOnly && g_waterRedo) reinterpret_cast<void(__cdecl*)()>(SITE_WaterFn)();
    const double t0 = Interp::NowUs();
    if (g_interpolate) Interp::ApplyEntities(TickAlpha());
    if (g_logicHz > 0 && g_renderOnly) SplitRenderOnly();   // after the blend: effects follow the shown entities
    // the camera too (blended again before RwCameraBeginUpdate): PreRender, shadows and effects see the shown one
    if (g_interpolate) Interp::ApplyCamera(!g_renderOnly, TickAlpha());
    const double t1 = Interp::NowUs();
    g_origConstructList();
    const double t2 = Interp::NowUs();
    Interp::g_timing.applyEntUs += t1 - t0;
    Interp::g_timing.constructUs += t2 - t1;
    ++Interp::g_timing.frames;
}

// CRealTimeShadowManager::Update (0x706AB0, thiscall on 0xC40350) draws the ped shadows from their skeletons right
// after PreRender: the skeletons are blended just before it, or the shadows keep the 30 Hz pose.
constexpr uintptr_t SITE_RtShadows = 0x53EA0D, EXPECT_RtShadows = 0x706AB0;
using RtShadowsFn = void(__fastcall*)(void*, void*);
RtShadowsFn g_origRtShadows = nullptr;
bool g_hierDone = false;
void __fastcall HookedRtShadows(void* self, void* edx) {
    if (g_interpolate) { Interp::ApplyHierarchies(!g_renderOnly, TickAlpha()); g_hierDone = true; }
    g_origRtShadows(self, edx);
}

char __cdecl HookedCameraBegin(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) {
    const double t0 = Interp::NowUs();
    if (g_interpolate) {
        if (!g_hierDone) Interp::ApplyHierarchies(!g_renderOnly, TickAlpha());
        Interp::ApplyCamera(!g_renderOnly, TickAlpha());
    }
    g_hierDone = false;
    Interp::g_timing.applyCamUs += Interp::NowUs() - t0;
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

HRESULT WINAPI HookedPresent(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    FlushGuard();
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
    LimitFrame();   // before Present: the frame is shown at its scheduled time (see FrameQpc)
    return g_origPresent(dev, src, dst, wnd, dirty);
}

HRESULT WINAPI HookedCreateDevice(void* d3d, UINT adapter, UINT type, HWND wnd, DWORD flags,
                                  PresentParams* pp, void** outDev) {
    Fix(adapter, pp, "CreateDevice");
    if (wnd) { g_gameWnd = wnd; ApplyFpsCap(wnd); }
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, outDev);
    if (hr == 0 && pp) ApplyBorderless(pp->hDeviceWindow ? pp->hDeviceWindow : wnd, pp);
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
    ApplyFpsCap(nullptr);
    g_logicHz = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"LogicHz", 0, ini));
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
        } else Log("interpolation hooks FAILED, running without interpolation");
    }
    if (g_logicHz > 0 && HookCall(SITE_PreRender, reinterpret_cast<void*>(&HookedPreRender), reinterpret_cast<void**>(&g_origPreRender), EXPECT_PreRender))
        Log("vehicle physics guard installed");
    if (g_logicHz > 0 && PatchCarLightStep()) Log("car light step patch installed");
    if (GetPrivateProfileIntW(L"Renewed120", L"SmoothCarLights", 1, ini) != 0 && PatchCarLightAlwaysMoving()) Log("car light per-frame patch installed");
    g_staticFix = GetPrivateProfileIntW(L"Renewed120", L"StaticShadowFix", 1, ini) != 0;
    if (g_logicHz > 0 &&
        HookCall(SITE_UpdStatic1, reinterpret_cast<void*>(&HookedUpdStatic1), reinterpret_cast<void**>(&g_origUpdStatic1), EXPECT_UpdStatic) &&
        HookCall(SITE_UpdStatic2, reinterpret_cast<void*>(&HookedUpdStatic2), reinterpret_cast<void**>(&g_origUpdStatic2), EXPECT_UpdStatic))
        Log("static shadow fix installed (enabled=%d)", int(g_staticFix));
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
