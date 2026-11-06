// Per-frame telemetry. Reads a few well known GTA SA 1.0 US globals (every read is SEH-guarded).
//
// Output (CSV, one line per second, plus event lines):
//   S,realSec,frames,fps,avgFrameMs,maxFrameMs,avgTimeStep,gameMsPerRealMs,px,py,pz,pedSpeed,vehPtr,vehSpeed,logicTicks
//   T,realSec,ticks,frames,time spent in the mod's steps per tick / per frame (microseconds)
//   E,realSec,frameCounter,what,details
#include "telemetry.h"
#include "interp.h"
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstdint>

namespace Telemetry {
namespace {

// GTA SA 1.0 US (HOODLUM / compact exe, image base 0x400000)
constexpr uintptr_t ADDR_TimeStep   = 0xB7CB5C;  // float CTimer::ms_fTimeStep (units of 1/50 s)
constexpr uintptr_t ADDR_TimeMs     = 0xB7CB84;  // uint32 CTimer::m_snTimeInMilliseconds (game time)
constexpr uintptr_t ADDR_FrameCtr   = 0xB7CB4C;  // uint32 CTimer::m_FrameCounter
constexpr uintptr_t ADDR_Players    = 0xB7CD98;  // CPlayerInfo[], stride 0x190, first field = CPed*
constexpr uintptr_t ADDR_PlayerFocus= 0xB7CD74;  // uint8 CWorld::PlayerInFocus
constexpr uintptr_t PLAYERINFO_SIZE = 0x190;
constexpr uintptr_t OFF_Matrix      = 0x14;      // CPlaceable::m_pMatrix
constexpr uintptr_t OFF_SimplePos   = 0x04;      // CPlaceable::m_transform.m_vPosn
constexpr uintptr_t OFF_MatrixPos   = 0x30;      // CMatrix::pos
constexpr uintptr_t OFF_MoveSpeed   = 0x44;      // CPhysical::m_vecMoveSpeed
constexpr uintptr_t OFF_PedVehicle  = 0x58C;     // CPed::m_pVehicle

struct Vec3 { float x, y, z; };

template <class T>
bool Read(uintptr_t addr, T& out) {
    __try { out = *reinterpret_cast<const T*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool EntityPos(uintptr_t e, Vec3& p) {
    uintptr_t m = 0;
    if (!Read(e + OFF_Matrix, m)) return false;
    return m ? Read(m + OFF_MatrixPos, p) : Read(e + OFF_SimplePos, p);
}

float Len(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

wchar_t g_path[MAX_PATH];
bool    g_on = false;
LARGE_INTEGER g_freq, g_t0, g_last, g_secStart;

// per-second accumulators
unsigned g_frames; double g_sumFt, g_maxFt, g_sumStep; uint32_t g_gameMsStart, g_ctrStart;
// previous frame
bool g_havePrev; Vec3 g_prevPos; uint32_t g_prevFrameCtr;

void Write(const char* fmt, ...) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path, L"a") != 0 || !f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fclose(f);
}

double Secs(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    return double(b.QuadPart - a.QuadPart) / double(g_freq.QuadPart);
}

}  // namespace

void Start(const wchar_t* csvPath) {
    wcscpy_s(g_path, csvPath);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path, L"w") != 0 || !f) return;
    fputs("# S,realSec,frames,fps,avgFrameMs,maxFrameMs,avgTimeStep,gameMsPerRealMs,px,py,pz,pedSpeed,vehPtr,vehSpeed,logicTicks\n"
          "# T,realSec,ticks,frames,perTick_us,perFrame_us\n"
          "# E,realSec,frameCounter,what,details\n", f);
    fclose(f);
    QueryPerformanceFrequency(&g_freq);
    g_on = true;
}

void OnFrame() {
    if (!g_on) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!g_t0.QuadPart) { g_t0 = g_last = g_secStart = now; return; }

    const double ftMs = Secs(g_last, now) * 1000.0;
    g_last = now;
    const double real = Secs(g_t0, now);

    float step = 0; uint32_t gameMs = 0, frameCtr = 0;
    Read(ADDR_TimeStep, step); Read(ADDR_TimeMs, gameMs); Read(ADDR_FrameCtr, frameCtr);
    if (!g_frames) { g_gameMsStart = gameMs; g_ctrStart = frameCtr; }

    ++g_frames; g_sumFt += ftMs; g_sumStep += step;
    if (ftMs > g_maxFt) g_maxFt = ftMs;

    // player
    uint8_t focus = 0; uintptr_t ped = 0; Vec3 pos{}, pedV{};
    const bool havePos = Read(ADDR_PlayerFocus, focus) && Read(ADDR_Players + PLAYERINFO_SIZE * focus, ped) && ped
                         && EntityPos(ped, pos) && Read(ped + OFF_MoveSpeed, pedV);

    if (havePos) {
        if (g_havePrev && ftMs < 100.0) {
            const Vec3 d{pos.x - g_prevPos.x, pos.y - g_prevPos.y, pos.z - g_prevPos.z};
            if (Len(d) > 20.0f)
                Write("E,%.3f,%u,jump,dist=%.1f ftMs=%.2f step=%.3f pos=%.1f;%.1f;%.1f\n",
                      real, frameCtr, Len(d), ftMs, step, pos.x, pos.y, pos.z);
        }
        g_prevPos = pos; g_havePrev = true;
    } else g_havePrev = false;

    if (g_prevFrameCtr && frameCtr > g_prevFrameCtr + 1)
        Write("E,%.3f,%u,frames_skipped,game counter jumped %u -> %u\n", real, frameCtr, g_prevFrameCtr, frameCtr);
    g_prevFrameCtr = frameCtr;

    if (Secs(g_secStart, now) < 1.0) return;
    const double elapsedMs = Secs(g_secStart, now) * 1000.0;
    uintptr_t veh = 0; float vehSpeed = 0;
    if (havePos && Read(ped + OFF_PedVehicle, veh) && veh) {
        Vec3 vp{}, vv{};
        // The pointer can be stale; accept it only if the vehicle really is next to the player.
        if (EntityPos(veh, vp) && Read(veh + OFF_MoveSpeed, vv) &&
            Len({vp.x - pos.x, vp.y - pos.y, vp.z - pos.z}) < 6.0f) vehSpeed = Len(vv); else veh = 0;
    }
    Write("S,%.2f,%u,%.1f,%.2f,%.2f,%.3f,%.3f,%.1f,%.1f,%.1f,%.4f,%08X,%.4f,%u\n",
          real, g_frames, g_frames * 1000.0 / elapsedMs, g_sumFt / g_frames, g_maxFt,
          g_sumStep / g_frames, double(gameMs - g_gameMsStart) / elapsedMs,
          pos.x, pos.y, pos.z, Len(pedV), unsigned(veh), vehSpeed, unsigned(frameCtr - g_ctrStart));
    const Interp::Timing tm = Interp::g_timing;
    Interp::g_timing = Interp::Timing{};
    const double tk = tm.ticks ? tm.ticks : 1, fr = tm.frames ? tm.frames : 1;
    Write("T,%.2f,ticks=%d,frames=%d,perTick_us: restore=%.0f update=%.0f process=%.0f onTick=%.0f,perFrame_us: applyEnt=%.0f applyCam=%.0f construct=%.0f\n",
          real, tm.ticks, tm.frames, tm.restoreUs / tk, tm.updateUs / tk, tm.processUs / tk, tm.onTickUs / tk,
          tm.applyEntUs / fr, tm.applyCamUs / fr, tm.constructUs / fr);
    g_frames = 0; g_sumFt = g_maxFt = g_sumStep = 0; g_secStart = now;
}

}  // namespace Telemetry
