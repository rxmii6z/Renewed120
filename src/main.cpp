// Renewed120 - RefreshRateFix module.
//
// GTA SA asks Direct3D9 for a fullscreen mode at a fixed 60 Hz. This module hooks
// IDirect3D9::CreateDevice and IDirect3DDevice9::Reset (through their shared vtables, so
// it also works with a DXVK d3d9.dll) and replaces the requested refresh rate with the
// desktop one, after checking the mode really exists.
//
// Minimal D3D9 declarations are repeated here on purpose: no DirectX SDK is needed.
#include <windows.h>
#include <intrin.h>
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
int            g_fpsCap = 0;
void*          g_d3d = nullptr;       // our own IDirect3D9, used only to query modes
int            g_forceHz = 0;
int            g_windowMode = 1;   // 0 = exclusive fullscreen, 1 = borderless windowed fullscreen
HWND           g_gameWnd = nullptr;
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
unsigned g_ticks = 0, g_renderOnlyFrames = 0;

float TickAlpha() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double a = double(now.QuadPart - g_lastTickQpc) / double(g_tickPeriodQpc);
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
bool     g_waterSaved = false;
float    g_savedStep = 0;
uint32_t g_savedCtr = 0;
unsigned g_waterCalls = 0, g_waterLastTick = ~0u;
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


// ---- developer menu (F9) ---------------------------------------------------------------------------------
// Drawn with the game's own CFont after Render2dStuff. Keys: F9 open/close, PgUp/PgDn select, Home/End change,
// Enter on "Save" writes the current values into Renewed120.ini.
constexpr uintptr_t SITE_Render2d = 0x53EB12, EXPECT_Render2d = 0x53E230;
VoidFn g_origRender2d = nullptr;
bool   g_menuOpen = false;
int    g_menuSel = 0;
wchar_t g_iniPath[MAX_PATH];

extern bool g_probe;
constexpr int MenuItems = 9;

using FontF2 = void(__cdecl*)(float, float);
using FontB1 = void(__cdecl*)(uint8_t);
using FontB2 = void(__cdecl*)(uint8_t, uint8_t);
using FontU4 = void(__cdecl*)(uint32_t);
using FontF1 = void(__cdecl*)(float);
using FontPrint = void(__cdecl*)(float, float, const char*);

constexpr int FpsCaps[] = {0, 60, 90, 120, 144, 165, 180, 240};
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
    put(L"WaterFix", g_waterRedo); put(L"VehicleGuard", g_freezeVeh); put(L"FpsCap", g_fpsCap); put(L"LogicHz", g_logicHz);
}

void MenuChange(int dir) {   // dir: -1 / +1 (Enter = +1)
    switch (g_menuSel) {
    case 0: if (g_interpolate) Interp::Restore(); g_interpolate = !g_interpolate; break;
    case 1: Interp::Restore(); Interp::SetVehicleParts(!Interp::VehicleParts()); break;
    case 2: Interp::Restore(); Interp::SetPedSkeleton(!Interp::PedSkeleton()); break;
    case 3: g_waterRedo = !g_waterRedo; break;
    case 4: g_fpsCap = FpsCaps[CycleIndex(FpsCaps, g_fpsCap, dir)]; break;
    case 5: g_logicHz = LogicRates[CycleIndex(LogicRates, g_logicHz, dir)]; break;
    case 6: g_freezeVeh = !g_freezeVeh; break;
    case 7: SaveIni(); break;
    case 8: g_probe = !g_probe; break;
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
        switch (i) {
        case 0: name = "Interpolation"; strcpy_s(val, g_interpolate ? "ON" : "OFF"); break;
        case 1: name = "Vehicle parts (wheels/doors)"; strcpy_s(val, Interp::VehicleParts() ? "ON" : "OFF"); break;
        case 2: name = "Ped skeleton"; strcpy_s(val, Interp::PedSkeleton() ? "ON" : "OFF"); break;
        case 3: name = "Water fix"; strcpy_s(val, g_waterRedo ? "ON" : "OFF"); break;
        case 4: name = "FPS cap"; if (g_fpsCap > 0) sprintf_s(val, "%d", g_fpsCap); else strcpy_s(val, "off"); break;
        case 5: name = "Logic rate (Hz)"; sprintf_s(val, "%d", g_logicHz); break;
        case 6: name = "Physics guard (veh+peds)"; strcpy_s(val, g_freezeVeh ? "ON" : "OFF"); break;
        case 7: name = "Save to ini"; strcpy_s(val, "[Enter]"); break;
        case 8: name = "Draw probe (writes log)"; strcpy_s(val, g_probe ? "ON" : "OFF"); break;
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
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
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
        nextTick.QuadPart = (nextTick.QuadPart == 0 || menu || now.QuadPart > nextTick.QuadPart + 2 * period)
                                ? now.QuadPart + period : nextTick.QuadPart + period;
        g_renderOnly = false;
        ++g_ticks;
        const double t0 = Interp::NowUs();
        RestoreWaterTiming();
        FlushGuard();
        Interp::Restore();   // the simulation must see the real, latest state
        const double t1 = Interp::NowUs();
        g_lastTickQpc = now.QuadPart;
        g_tickPeriodQpc = period;
        g_origTimerUpdate();
        const double t2 = Interp::NowUs();
        Interp::g_timing.restoreUs += t1 - t0;
        Interp::g_timing.updateUs += t2 - t1;
        ++Interp::g_timing.ticks;
    } else {
        g_renderOnly = true;
        ++g_renderOnlyFrames;
    }
}

void __cdecl HookedGameProcess() {
    if (g_logicHz > 0 && g_renderOnly) return;
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

// Stored shadows (CShadows::asShadowsStored, 48 x 0x40 bytes at 0xC4A034) that the simulation registered during a tick are
// consumed when drawn. Re-insert them on the render-only frames of that tick so they are drawn every frame.
constexpr uintptr_t ADDR_StoredShadows = 0xC4A034;
constexpr int StoredShadowCount = 48, StoredShadowSize = 0x40;
uint8_t g_shadowSnap[StoredShadowCount * StoredShadowSize];
bool g_shadowKeep = false;   // experimental (crashed inside d3d9): re-insert stored shadows on render-only frames
void ShadowsAfterProcess(bool tickFrame) {
    if (!g_shadowKeep) return;
    __try {
        auto* cur = reinterpret_cast<uint8_t*>(ADDR_StoredShadows);
        if (tickFrame) {
            std::memcpy(g_shadowSnap, cur, sizeof(g_shadowSnap));
        } else {
            for (int i = 0; i < StoredShadowCount; ++i) {
                uint8_t* slot = cur + i * StoredShadowSize;
                const uint8_t* snap = g_shadowSnap + i * StoredShadowSize;
                if (*reinterpret_cast<const uint32_t*>(slot) == 0 && *reinterpret_cast<const uint32_t*>(snap) != 0)
                    std::memcpy(slot, snap, StoredShadowSize);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- blood pools ---------------------------------------------------------------------------------------------
// A dead ped's task registers the fresh blood pool as a stored shadow (CShadows::StoreShadowToBeRendered, 17 arguments, call
// at 0x630E63) once per simulated tick; stored shadows are consumed when drawn, so the pool only showed on tick frames.
// Remember the arguments (deep-copying the position vector) and replay the call on the render-only frames of the tick.
constexpr uintptr_t SITE_BloodStore = 0x630E63, EXPECT_BloodStore = 0x70BA00;
using StoreFn = void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
StoreFn g_origBloodStore = nullptr;
struct BloodCall { uint32_t a[17]; float pos[3]; };
constexpr int MaxBlood = 16;
BloodCall g_blood[MaxBlood];
int  g_bloodN = 0;
bool g_bloodReplay = true;
unsigned g_bloodTick = ~0u;

void __cdecl HookedBloodStore(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
                              uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13, uint32_t a14,
                              uint32_t a15, uint32_t a16) {
    if (g_logicHz > 0 && g_bloodReplay) {
        if (g_renderOnly) {
            // it runs on render frames by itself: nothing to replay
            g_bloodReplay = false;
            Log("blood pool store runs every frame, replay disabled");
        } else {
            if (g_bloodTick != g_ticks) { g_bloodTick = g_ticks; g_bloodN = 0; }
            if (g_bloodN < MaxBlood) {
                BloodCall& b = g_blood[g_bloodN];
                const uint32_t v[17] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16};
                std::memcpy(b.a, v, sizeof(v));
                bool ok = true;
                __try { std::memcpy(b.pos, reinterpret_cast<const void*>(a3), sizeof(b.pos)); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                if (ok) ++g_bloodN;
            }
        }
    }
    g_origBloodStore(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16);
}

void ReplayBlood() {
    if (!g_bloodReplay || g_bloodTick != g_ticks) return;   // only what this very tick registered
    for (int i = 0; i < g_bloodN; ++i) {
        BloodCall& b = g_blood[i];
        g_origBloodStore(b.a[0], b.a[1], b.a[2], reinterpret_cast<uint32_t>(b.pos), b.a[4], b.a[5], b.a[6], b.a[7], b.a[8], b.a[9],
                         b.a[10], b.a[11], b.a[12], b.a[13], b.a[14], b.a[15], b.a[16]);
    }
}

void __cdecl HookedConstructList() {
    if (g_logicHz > 0 && g_renderOnly) ReplayBlood();
    if (g_logicHz > 0) ShadowsAfterProcess(!g_renderOnly);
    // The sea fills draw buckets in this function once per simulated frame and the buckets are emptied when drawn,
    // so on render-only frames it has to run again or the water is missing.
    if (g_logicHz > 0 && g_renderOnly && g_waterRedo) reinterpret_cast<void(__cdecl*)()>(SITE_WaterFn)();
    const double t0 = Interp::NowUs();
    if (g_interpolate) Interp::ApplyEntities(TickAlpha());
    const double t1 = Interp::NowUs();
    g_origConstructList();
    const double t2 = Interp::NowUs();
    Interp::g_timing.applyEntUs += t1 - t0;
    Interp::g_timing.constructUs += t2 - t1;
    ++Interp::g_timing.frames;
}

char __cdecl HookedCameraBegin(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) {
    const double t0 = Interp::NowUs();
    if (g_interpolate) { Interp::ApplyHierarchies(!g_renderOnly, TickAlpha()); Interp::ApplyCamera(!g_renderOnly, TickAlpha()); }
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
}



// ---- draw probe (dev menu): lists draw calls that only happen on tick frames, with their callers -------------
bool g_probe = false;
struct DrawKey { unsigned api, a, b, c, d, caller[3]; unsigned long long tickCnt, rendCnt; bool used; };
DrawKey g_tbl[1024];
unsigned g_framesTick = 0, g_framesRend = 0;
unsigned g_drawCounter = 0;
double g_ssDraws[2];
void NoteDraw(unsigned api, unsigned a, unsigned b, unsigned c2, unsigned d, uintptr_t* sp) {
    ++g_drawCounter;
    unsigned h = (api * 2654435761u) ^ (a * 40503u) ^ (b * 69069u) ^ (c2 * 1664525u) ^ (d * 22695477u);
    for (int i = 0; i < 1024; ++i) {
        DrawKey& k = g_tbl[(h + i) & 1023];
        if (!k.used) {
            k = DrawKey{}; k.used = true; k.api = api; k.a = a; k.b = b; k.c = c2; k.d = d;
            int n = 0;
            __try { for (int j = 1; j < 80 && n < 3; ++j) if (sp[j] >= 0x401000 && sp[j] < 0x8E0000) k.caller[n++] = unsigned(sp[j]); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (k.api == api && k.a == a && k.b == b && k.c == c2 && k.d == d) { (g_renderOnly ? k.rendCnt : k.tickCnt)++; return; }
    }
}
// shadow system probe: number of static shadows at the moment they are drawn
VoidFn g_origStaticShadows1 = nullptr, g_origStaticShadows2 = nullptr;
double g_ssSum[2][2]; unsigned g_ssN[2][2];
void ReadStatic(int which) {
    __try { g_ssSum[g_renderOnly][which] += *reinterpret_cast<volatile uint16_t*>(0xC403DC); ++g_ssN[g_renderOnly][which]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void __cdecl HookedStatic1() { if (g_probe) ReadStatic(0); g_origStaticShadows1(); }
void __cdecl HookedStatic2() { if (g_probe) ReadStatic(1); const unsigned d0 = g_drawCounter; g_origStaticShadows2(); if (g_probe) g_ssDraws[g_renderOnly] += g_drawCounter - d0; }
void ProbeEndFrame() {
    if (!g_probe) return;
    (g_renderOnly ? g_framesRend : g_framesTick)++;
    if (g_framesTick + g_framesRend < 360) return;
    Log("probe: static shadows at draw time: site1 tick %.2f (n=%u) render %.2f (n=%u); site2 tick %.2f (n=%u) render %.2f (n=%u)", g_ssN[0][0] ? g_ssSum[0][0] / g_ssN[0][0] : -1.0, g_ssN[0][0], g_ssN[1][0] ? g_ssSum[1][0] / g_ssN[1][0] : -1.0, g_ssN[1][0], g_ssN[0][1] ? g_ssSum[0][1] / g_ssN[0][1] : -1.0, g_ssN[0][1], g_ssN[1][1] ? g_ssSum[1][1] / g_ssN[1][1] : -1.0, g_ssN[1][1]);
    Log("probe: draws inside RenderStaticShadows per frame: tick %.2f render %.2f", g_ssN[0][1] ? g_ssDraws[0] / g_ssN[0][1] : -1.0, g_ssN[1][1] ? g_ssDraws[1] / g_ssN[1][1] : -1.0);
    g_ssDraws[0] = g_ssDraws[1] = 0;
    memset(g_ssSum, 0, sizeof(g_ssSum)); memset(g_ssN, 0, sizeof(g_ssN));
    Log("probe: tick-only draws over %u tick / %u render frames:", g_framesTick, g_framesRend);
    for (auto& k : g_tbl) if (k.used && g_framesTick && g_framesRend) {
        const double t = double(k.tickCnt) / g_framesTick, r = double(k.rendCnt) / g_framesRend;
        if (t - r > 0.2) Log("  api=%u %u %u %u %u : tick %.2f render %.2f callers %08X %08X %08X", k.api, k.a, k.b, k.c, k.d, t, r, k.caller[0], k.caller[1], k.caller[2]);
    }
    memset(g_tbl, 0, sizeof(g_tbl)); g_framesTick = g_framesRend = 0;
}
using DP4 = HRESULT(WINAPI*)(void*, UINT, UINT, UINT);
using DP7 = HRESULT(WINAPI*)(void*, UINT, INT, UINT, UINT, UINT, UINT);
using DP5 = HRESULT(WINAPI*)(void*, UINT, UINT, const void*, UINT);
using DP9 = HRESULT(WINAPI*)(void*, UINT, UINT, UINT, UINT, const void*, UINT, const void*, UINT);
DP4 g_o81; DP7 g_o82; DP5 g_o83; DP9 g_o84;
HRESULT WINAPI H81(void* d, UINT a, UINT b, UINT c) { if (g_probe) NoteDraw(81, a, b, c, 0, static_cast<uintptr_t*>(_AddressOfReturnAddress())); return g_o81(d, a, b, c); }
HRESULT WINAPI H82(void* d, UINT a, INT b, UINT c, UINT e, UINT f, UINT g) { if (g_probe) NoteDraw(82, a, c, e, g, static_cast<uintptr_t*>(_AddressOfReturnAddress())); return g_o82(d, a, b, c, e, f, g); }
HRESULT WINAPI H83(void* d, UINT a, UINT b, const void* c, UINT e) { if (g_probe) NoteDraw(83, a, b, e, 0, static_cast<uintptr_t*>(_AddressOfReturnAddress())); return g_o83(d, a, b, c, e); }
HRESULT WINAPI H84(void* d, UINT a, UINT b, UINT c, UINT e, const void* f, UINT g, const void* h, UINT i) { if (g_probe) NoteDraw(84, a, b, c, e, static_cast<uintptr_t*>(_AddressOfReturnAddress())); return g_o84(d, a, b, c, e, f, g, h, i); }

HRESULT WINAPI HookedPresent(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    FlushGuard();
    ProbeEndFrame();
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
    HRESULT hr = g_origPresent(dev, src, dst, wnd, dirty);
    LimitFrame();
    return hr;
}

HRESULT WINAPI HookedCreateDevice(void* d3d, UINT adapter, UINT type, HWND wnd, DWORD flags,
                                  PresentParams* pp, void** outDev) {
    Fix(adapter, pp, "CreateDevice");
    if (wnd) g_gameWnd = wnd;
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, outDev);
    if (hr == 0 && pp) ApplyBorderless(pp->hDeviceWindow ? pp->hDeviceWindow : wnd, pp);
    if (hr == 0 && outDev && *outDev && !g_origReset) {
        if (!PatchVTable(*outDev, VT_Reset, reinterpret_cast<void*>(&HookedReset),
                         reinterpret_cast<void**>(&g_origReset)))
            Log("failed to hook Reset");
    }
    if (hr == 0 && outDev && *outDev && !g_o81) {
        PatchVTable(*outDev, 81, reinterpret_cast<void*>(&H81), reinterpret_cast<void**>(&g_o81));
        PatchVTable(*outDev, 82, reinterpret_cast<void*>(&H82), reinterpret_cast<void**>(&g_o82));
        PatchVTable(*outDev, 83, reinterpret_cast<void*>(&H83), reinterpret_cast<void**>(&g_o83));
        PatchVTable(*outDev, 84, reinterpret_cast<void*>(&H84), reinterpret_cast<void**>(&g_o84));
    }
    if (hr == 0 && outDev && *outDev && (g_telemetry || g_fpsCap > 0) && !g_origPresent) {
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
    g_fpsCap = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"FpsCap", 0, ini));
    g_logicHz = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"LogicHz", 0, ini));
    if (g_logicHz < 0 || g_logicHz > 60) g_logicHz = 0;
    if (!g_fixRefresh && !g_telemetry && g_fpsCap <= 0 && g_logicHz <= 0) { Log("all modules disabled"); return; }
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
        } else Log("interpolation hooks FAILED, running without interpolation");
    }
    if (g_logicHz > 0 && HookCall(SITE_PreRender, reinterpret_cast<void*>(&HookedPreRender), reinterpret_cast<void**>(&g_origPreRender), EXPECT_PreRender))
        Log("vehicle physics guard installed");
    HookCall(0x53DDF2, reinterpret_cast<void*>(&HookedStatic1), reinterpret_cast<void**>(&g_origStaticShadows1), 0x70A960);
    HookCall(0x53E0C8, reinterpret_cast<void*>(&HookedStatic2), reinterpret_cast<void**>(&g_origStaticShadows2), 0x70A960);
    if (g_logicHz > 0 && GetPrivateProfileIntW(L"Renewed120", L"BloodFix", 1, ini) != 0 &&
        HookCall(SITE_BloodStore, reinterpret_cast<void*>(&HookedBloodStore), reinterpret_cast<void**>(&g_origBloodStore), EXPECT_BloodStore))
        Log("blood pool fix installed");
    wcscpy_s(g_iniPath, ini);
    if (GetPrivateProfileIntW(L"Renewed120", L"DevMenu", 1, ini) != 0 &&
        HookCall(SITE_Render2d, reinterpret_cast<void*>(&HookedRender2d), reinterpret_cast<void**>(&g_origRender2d), EXPECT_Render2d))
        Log("dev menu active (F9)");
    if (g_telemetry) {
        wchar_t csv[MAX_PATH];
        swprintf_s(csv, L"%s.telemetry.csv", path);
        Telemetry::Start(csv);
        Interp::SetDebug(GetPrivateProfileIntW(L"Renewed120", L"DebugCamera", 0, ini) != 0);
    }
    g_forceHz = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"ForceHz", 0, ini));
    g_windowMode = static_cast<int>(GetPrivateProfileIntW(L"Renewed120", L"WindowMode", 1, ini));
    Interp::SetVehicleParts(GetPrivateProfileIntW(L"Renewed120", L"VehicleParts", 1, ini) != 0);
    Interp::SetPedSkeleton(GetPrivateProfileIntW(L"Renewed120", L"PedSkeleton", 1, ini) != 0);
    g_waterRedo = GetPrivateProfileIntW(L"Renewed120", L"WaterFix", 1, ini) != 0;
    g_freezeVeh = GetPrivateProfileIntW(L"Renewed120", L"VehicleGuard", 1, ini) != 0;
    g_shadowKeep = GetPrivateProfileIntW(L"Renewed120", L"ShadowKeep", 0, ini) != 0;

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
