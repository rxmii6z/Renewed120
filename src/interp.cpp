#include "interp.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace Interp {
namespace {

// ---- GTA SA 1.0 US layout ---------------------------------------------------------------
constexpr uintptr_t ADDR_PedPool = 0xB74490, ADDR_VehPool = 0xB74494, ADDR_ObjPool = 0xB7449C;
constexpr uintptr_t STRIDE_Ped = 0x7C4, STRIDE_Veh = 0xA18, STRIDE_Obj = 0x19C;
constexpr uintptr_t OFF_PoolObjects = 0, OFF_PoolFlags = 4, OFF_PoolSize = 8;   // CPool
constexpr uintptr_t OFF_EntMatrix = 0x14;       // CPlaceable::m_pMatrix (CMatrix*)
constexpr uintptr_t OFF_EntRwObject = 0x18;     // CEntity::m_pRwObject
constexpr uintptr_t OFF_MatRwMatrix = 0x40;     // CMatrix::m_pRwMatrix (attached to the frame)
constexpr uintptr_t ADDR_SceneCam = 0xC1703C;   // RwCamera*
constexpr uintptr_t ADDR_UpdateRwMatrix = 0x59AD70;      // void CMatrix::UpdateRwMatrix(RwMatrix*)  thiscall
constexpr uintptr_t ADDR_RwFrameUpdateObjects = 0x7F0910; // RwBool RwFrameUpdateObjects(RwFrame*)   cdecl
constexpr float TeleportDist = 20.0f;

using UpdateRwMatrixFn = void(__thiscall*)(void* cmatrix, void* rwMatrix);
using FrameUpdateFn = int(__cdecl*)(void* frame);

struct Pose { float r[3], u[3], a[3], p[3]; };   // right, up, at, pos

struct Slot {
    bool valid = false, applied = false;
    uintptr_t ent = 0;
    uint8_t gen = 0;
    Pose prev{}, curr{};
};

constexpr int MaxPed = 256, MaxVeh = 256, MaxObj = 1024;
Slot g_ped[MaxPed], g_veh[MaxVeh], g_obj[MaxObj];

// Child frames of a ped/vehicle clump (bones, wheels, doors...): animated by the game once per tick.
constexpr int MaxBones = 48;
constexpr uintptr_t OFF_FrameChild = 0x98, OFF_FrameNext = 0x9C;   // RwFrame::child / ::next
struct BoneSet {
    int n = 0;
    void* f[MaxBones];
    Pose prev[MaxBones], curr[MaxBones];
    bool moved = false, applied = false;
};
BoneSet g_pedBones[MaxPed], g_vehBones[MaxVeh];
bool g_pedSkeleton = true;   // interpolate ped bones (RpHAnim)
bool g_vehParts = true;   // interpolate the child frames (wheels, doors...) of vehicles

Stats g_stats{};

struct DbgRec { char kind; bool tick; float alpha; double tMs; float raw[3], prev[3], curr[3], out[3], atPrev[3], atCurr[3], atOut[3]; };
constexpr int DbgN = 8000;
DbgRec g_dbg[DbgN];
unsigned g_dbgCount = 0;
bool g_dbgOn = false;
void DbgPush(char kind, bool tick, float alpha, const float* raw, const Pose& prev, const Pose& curr, const Pose& out) {
    if (!g_dbgOn) return;
    DbgRec& r = g_dbg[g_dbgCount++ % DbgN];
    LARGE_INTEGER q, fq; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&fq);
    r.tMs = double(q.QuadPart) * 1000.0 / double(fq.QuadPart);
    std::memcpy(r.atPrev, prev.a, 12); std::memcpy(r.atCurr, curr.a, 12); std::memcpy(r.atOut, out.a, 12);
    r.kind = kind; r.tick = tick; r.alpha = alpha;
    std::memcpy(r.raw, raw, 12); std::memcpy(r.prev, prev.p, 12); std::memcpy(r.curr, curr.p, 12); std::memcpy(r.out, out.p, 12);
}

struct CamState { bool valid = false, applied = false; Pose prev{}, curr{}; } g_cam;

// ---- helpers (no C++ objects with destructors: they use SEH) -----------------------------
void ReadPose(const uint8_t* m, Pose& o) {   // m points at a CMatrix or RwMatrix
    std::memcpy(o.r, m + 0x00, 12); std::memcpy(o.u, m + 0x10, 12);
    std::memcpy(o.a, m + 0x20, 12); std::memcpy(o.p, m + 0x30, 12);
}
void WritePose(uint8_t* m, const Pose& o) {
    std::memcpy(m + 0x00, o.r, 12); std::memcpy(m + 0x10, o.u, 12);
    std::memcpy(m + 0x20, o.a, 12); std::memcpy(m + 0x30, o.p, 12);
}

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Cross(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
void Norm(float* v) {
    const float l = std::sqrt(Dot(v, v));
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

bool Teleported(const Pose& a, const Pose& b) {
    const float d[3] = {b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2]};
    return Dot(d, d) > TeleportDist * TeleportDist || Dot(a.a, b.a) < 0.5f || Dot(a.u, b.u) < 0.5f;
}

bool Same(const Pose& a, const Pose& b) { return std::memcmp(&a, &b, sizeof(Pose)) == 0; }

void Blend(const Pose& a, const Pose& b, float t, Pose& o) {
    for (int i = 0; i < 3; ++i) {
        o.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
        o.u[i] = a.u[i] + (b.u[i] - a.u[i]) * t;
        o.a[i] = a.a[i] + (b.a[i] - a.a[i]) * t;
    }
    // Re-orthonormalise (right-handed: right x up = at).
    Norm(o.a); Norm(o.u);
    Cross(o.u, o.a, o.r); Norm(o.r);
    Cross(o.a, o.r, o.u);
}

// ---- guarded game access -------------------------------------------------------------------
struct EntPtrs { uint8_t* cmat; void* rwMat; void* frame; };

bool GetEntity(uintptr_t ent, EntPtrs& e) {
    __try {
        e.cmat = *reinterpret_cast<uint8_t**>(ent + OFF_EntMatrix);
        void* rwObj = *reinterpret_cast<void**>(ent + OFF_EntRwObject);
        if (!e.cmat) { ++g_stats.noMatrix; return false; }
        if (!rwObj) { ++g_stats.noRwObject; return false; }
        e.frame = *reinterpret_cast<void**>(static_cast<uint8_t*>(rwObj) + 4);   // RwObject::parent
        if (!e.frame) { ++g_stats.noFrame; return false; }
        // The entity's CMatrix is NOT attached to the frame (m_pRwMatrix is usually null): copy into the
        // frame's own modelling matrix, exactly like CEntity::UpdateRW does.
        e.rwMat = static_cast<uint8_t*>(e.frame) + 0x10;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool PushPose(uintptr_t ent, const Pose& pose) {   // write CMatrix, copy to the RwFrame, mark dirty
    EntPtrs e{};
    if (!GetEntity(ent, e)) return false;
    __try {
        WritePose(e.cmat, pose);
        reinterpret_cast<UpdateRwMatrixFn>(ADDR_UpdateRwMatrix)(e.cmat, e.rwMat);
        reinterpret_cast<FrameUpdateFn>(ADDR_RwFrameUpdateObjects)(e.frame);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadEntityPose(uintptr_t ent, Pose& pose) {
    EntPtrs e{};
    if (!GetEntity(ent, e)) return false;
    __try { ReadPose(e.cmat, pose); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- bones ---------------------------------------------------------------------------------
// Depth-first walk of the frame tree below `root` (the root itself is handled through the CMatrix).
bool CollectBones(void* root, void** frames, Pose* poses, int& n) {
    n = 0;
    __try {
        void* stack[64]; int sp = 0;
        auto* r = static_cast<uint8_t*>(root);
        uint8_t* f = *reinterpret_cast<uint8_t**>(r + OFF_FrameChild);
        while (f && n < MaxBones) {
            frames[n] = f; ReadPose(f + 0x10, poses[n]); ++n;
            uint8_t* c = *reinterpret_cast<uint8_t**>(f + OFF_FrameChild);
            uint8_t* nx = *reinterpret_cast<uint8_t**>(f + OFF_FrameNext);
            if (c) { if (nx && sp < 64) stack[sp++] = nx; f = c; }
            else if (nx) f = nx;
            else if (sp) f = static_cast<uint8_t*>(stack[--sp]);
            else f = nullptr;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; return false; }
}

float AxisLen(const float* v) { return std::sqrt(Dot(v, v)); }

// Linear blend that keeps each axis length (bones may be scaled). Returns false when the rotation changed
// too much for a linear blend (fast spinning wheels...): the caller then keeps the real pose.
bool BlendBone(const Pose& a, const Pose& b, float t, Pose& o) {
    auto axis = [&](const float* x, const float* y, float* out) -> bool {
        const float lx = AxisLen(x), ly = AxisLen(y);
        if (lx < 1e-6f || ly < 1e-6f) return false;
        float ux[3] = {x[0] / lx, x[1] / lx, x[2] / lx}, uy[3] = {y[0] / ly, y[1] / ly, y[2] / ly};
        if (Dot(ux, uy) < 0.9f) return false;                       // more than ~25 degrees per tick
        float v[3] = {ux[0] + (uy[0] - ux[0]) * t, ux[1] + (uy[1] - ux[1]) * t, ux[2] + (uy[2] - ux[2]) * t};
        Norm(v);
        const float l = lx + (ly - lx) * t;
        out[0] = v[0] * l; out[1] = v[1] * l; out[2] = v[2] * l;
        return true;
    };
    if (!axis(a.r, b.r, o.r) || !axis(a.u, b.u, o.u) || !axis(a.a, b.a, o.a)) return false;
    for (int i = 0; i < 3; ++i) o.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
    return true;
}

bool WriteBone(void* frame, const Pose& p) {
    __try { WritePose(static_cast<uint8_t*>(frame) + 0x10, p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SnapBones(BoneSet& b, void* root, bool fresh) {
    void* f2[MaxBones]; Pose p2[MaxBones]; int n2 = 0;
    if (!CollectBones(root, f2, p2, n2)) { b.n = 0; b.moved = false; return; }
    const bool same = !fresh && n2 == b.n && std::memcmp(f2, b.f, n2 * sizeof(void*)) == 0;
    if (!same) {
        b.n = n2;
        std::memcpy(b.f, f2, n2 * sizeof(void*));
        for (int i = 0; i < n2; ++i) { b.prev[i] = p2[i]; b.curr[i] = p2[i]; }
        b.moved = false;
    } else {
        b.moved = false;
        for (int i = 0; i < n2; ++i) {
            b.prev[i] = b.curr[i];
            b.curr[i] = p2[i];
            if (std::memcmp(&b.prev[i], &b.curr[i], sizeof(Pose)) != 0) b.moved = true;
        }
    }
    g_stats.bonesTracked += b.n;
}

void ApplyBones(BoneSet& b, float t) {
    for (int i = 0; i < b.n; ++i) {
        const Pose& p0 = b.prev[i];
        const Pose& p1 = b.curr[i];
        if (std::memcmp(&p0, &p1, sizeof(Pose)) == 0) continue;
        Pose o{};
        if (!BlendBone(p0, p1, t, o)) o = p1;                   // keep the real pose
        if (WriteBone(b.f[i], o)) ++g_stats.bonesApplied;
    }
    b.applied = true;
}

void RestoreBones(BoneSet& b) {
    if (!b.applied) return;
    for (int i = 0; i < b.n; ++i) WriteBone(b.f[i], b.curr[i]);
    b.applied = false;
}

struct PoolView { uint8_t* base; uint8_t* flags; int size; };
bool GetPool(uintptr_t poolAddr, PoolView& v) {
    __try {
        auto* pool = *reinterpret_cast<uint8_t**>(poolAddr);
        if (!pool) return false;
        v.base = *reinterpret_cast<uint8_t**>(pool + OFF_PoolObjects);
        v.flags = *reinterpret_cast<uint8_t**>(pool + OFF_PoolFlags);
        v.size = *reinterpret_cast<int*>(pool + OFF_PoolSize);
        return v.base && v.flags && v.size > 0 && v.size < 100000;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t g_playerPedForStats = 0;

void SnapshotPool(uintptr_t poolAddr, uintptr_t stride, Slot* slots, int maxSlots, BoneSet* bones) {
    PoolView v{};
    if (!GetPool(poolAddr, v)) { for (int i = 0; i < maxSlots; ++i) slots[i].valid = false; return; }
    const int n = v.size < maxSlots ? v.size : maxSlots;
    for (int i = 0; i < n; ++i) {
        Slot& s = slots[i];
        uint8_t flag = 0x80;
        __try { flag = v.flags[i]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        s.applied = false;
        if (bones) bones[i].applied = false;
        if (flag & 0x80) { s.valid = false; continue; }          // free slot
        const uintptr_t ent = reinterpret_cast<uintptr_t>(v.base) + stride * i;
        EntPtrs e{};
        Pose now{};
        if (!GetEntity(ent, e)) { s.valid = false; continue; }
        __try { ReadPose(e.cmat, now); } __except (EXCEPTION_EXECUTE_HANDLER) { s.valid = false; continue; }
        const bool fresh = !s.valid || s.ent != ent || s.gen != flag;
        if (fresh) {
            s.prev = s.curr = now;                                // new entity: nothing to blend from
        } else {
            s.prev = s.curr; s.curr = now;
            if (Teleported(s.prev, s.curr)) s.prev = s.curr;      // teleport / camera cut: snap
        }
        s.ent = ent; s.gen = flag; s.valid = true;
        if (bones) {
            SnapBones(bones[i], e.frame, fresh);
            if (g_dbgOn || true) {   // cheap: only counts bones that changed
                if (ent == g_playerPedForStats) {
                    int changed = 0;
                    for (int k = 0; k < bones[i].n; ++k) if (std::memcmp(&bones[i].prev[k], &bones[i].curr[k], sizeof(Pose)) != 0) ++changed;
                    g_stats.playerBones = bones[i].n; g_stats.playerBonesChanged = changed;
                }
            }
        }
        if (&slots[0] == g_ped) ++g_stats.pedsValid; else if (&slots[0] == g_veh) ++g_stats.vehsValid; else ++g_stats.objsValid;
    }
    for (int i = n; i < maxSlots; ++i) slots[i].valid = false;
}

// Index in the vehicle pool (veh=true: the vehicle the player is in) or ped pool (the player ped); -1 if none.
int PlayerSlot(bool veh) {
    __try {
        const uint8_t focus = *reinterpret_cast<uint8_t*>(0xB7CD74);
        const uintptr_t ped = *reinterpret_cast<uintptr_t*>(0xB7CD98 + 0x190 * focus);
        if (!ped) return -1;
        uintptr_t ent = ped, stride = STRIDE_Ped, poolAddr = ADDR_PedPool;
        if (veh) {
            ent = *reinterpret_cast<uintptr_t*>(ped + 0x58C);   // CPed::m_pVehicle
            if (!ent) return -1;
            stride = STRIDE_Veh; poolAddr = ADDR_VehPool;
        }
        PoolView v{};
        if (!GetPool(poolAddr, v)) return -1;
        const intptr_t idx = (static_cast<intptr_t>(ent) - reinterpret_cast<intptr_t>(v.base)) / static_cast<intptr_t>(stride);
        return (idx >= 0 && idx < v.size) ? static_cast<int>(idx) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void ApplyPool(Slot* slots, int maxSlots, float alpha, BoneSet* bones) {
    // Debug: remember the player's vehicle (if driving) or the player's ped.
    int dbgIdx = -1;
    char dbgKind = 'V';
    if (g_dbgOn) {
        if (slots == g_veh) dbgIdx = PlayerSlot(true);
        else if (slots == g_ped) { if (PlayerSlot(true) < 0) dbgIdx = PlayerSlot(false); dbgKind = 'P'; }
    }
    for (int i = 0; i < maxSlots; ++i) {
        Slot& s = slots[i];
        if (!s.valid) continue;
        const bool rootMoved = !Same(s.prev, s.curr);
        const bool bonesMoved = bones && bones[i].moved;
        if (!rootMoved && !bonesMoved) continue;
        ++g_stats.moved;
        Pose p = s.curr;
        if (rootMoved) Blend(s.prev, s.curr, alpha, p);
        if (i == dbgIdx) { Pose raw{}; ReadEntityPose(s.ent, raw); DbgPush(dbgKind, false, alpha, raw.p, s.prev, s.curr, p); }
        if (bonesMoved) ApplyBones(bones[i], alpha);              // children first, then the root marks the tree dirty
        s.applied = PushPose(s.ent, p);
        if (s.applied) ++g_stats.applied; else ++g_stats.applyFailed;
    }
}

void RestorePool(Slot* slots, int maxSlots, BoneSet* bones) {
    for (int i = 0; i < maxSlots; ++i) {
        Slot& s = slots[i];
        if (bones) RestoreBones(bones[i]);
        if (!s.applied) continue;
        PushPose(s.ent, s.curr);
        s.applied = false;
    }
}

// ---- ped skeleton: RpHAnim hierarchy matrices ----------------------------------------------------
// The ped bones are not RwFrames: their pose lives in the hierarchy's pMatrixArray (numNodes x RwMatrix),
// recomputed from the animation keyframes every time the game updates the hierarchy.
constexpr uintptr_t ADDR_GetAnimHierarchyFromSkinClump = 0x734A40;   // RpHAnimHierarchy* (RpClump*)  cdecl
constexpr int MaxHier = 64;
struct HierSet {
    bool valid = false, moved = false, applied = false;
    void* hier = nullptr;
    uint8_t* mats = nullptr;
    int n = 0;
    Pose prev[MaxHier], curr[MaxHier];
};
HierSet g_pedHier[MaxPed];

bool GetHier(uintptr_t ent, void*& hier, uint8_t*& mats, int& n) {
    __try {
        void* clump = *reinterpret_cast<void**>(ent + OFF_EntRwObject);
        if (!clump) return false;
        hier = reinterpret_cast<void*(__cdecl*)(void*)>(ADDR_GetAnimHierarchyFromSkinClump)(clump);
        if (!hier) return false;
        n = *reinterpret_cast<int*>(static_cast<uint8_t*>(hier) + 4);          // numNodes
        mats = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(hier) + 8);   // pMatrixArray
        return n > 0 && n <= MaxHier && mats;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadHier(uint8_t* mats, int n, Pose* out) {
    __try { for (int i = 0; i < n; ++i) ReadPose(mats + 0x40 * i, out[i]); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteHier(uint8_t* mats, int n, const Pose* in) {
    __try { for (int i = 0; i < n; ++i) WritePose(mats + 0x40 * i, in[i]); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Hierarchy matrices are world space and are rebuilt by the game from the (already blended) entity frame,
// so they are stored relative to that frame: world = local * M.
void AxisToLocal(const Pose& M, const float* w, float* l) {
    const float rr = Dot(M.r, M.r), uu = Dot(M.u, M.u), aa = Dot(M.a, M.a);
    l[0] = Dot(w, M.r) / (rr > 1e-9f ? rr : 1.f); l[1] = Dot(w, M.u) / (uu > 1e-9f ? uu : 1.f); l[2] = Dot(w, M.a) / (aa > 1e-9f ? aa : 1.f);
}
void AxisToWorld(const Pose& M, const float* l, float* w) {
    for (int i = 0; i < 3; ++i) w[i] = l[0] * M.r[i] + l[1] * M.u[i] + l[2] * M.a[i];
}
void PoseToLocal(const Pose& M, const Pose& w, Pose& l) {
    AxisToLocal(M, w.r, l.r); AxisToLocal(M, w.u, l.u); AxisToLocal(M, w.a, l.a);
    const float d[3] = {w.p[0] - M.p[0], w.p[1] - M.p[1], w.p[2] - M.p[2]};
    AxisToLocal(M, d, l.p);
}
void PoseToWorld(const Pose& M, const Pose& l, Pose& w) {
    AxisToWorld(M, l.r, w.r); AxisToWorld(M, l.u, w.u); AxisToWorld(M, l.a, w.a);
    AxisToWorld(M, l.p, w.p);
    for (int i = 0; i < 3; ++i) w.p[i] += M.p[i];
}
bool EntityMatrix(uintptr_t ent, Pose& M) {
    EntPtrs e{};
    if (!GetEntity(ent, e)) return false;
    __try { ReadPose(static_cast<const uint8_t*>(e.rwMat), M); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void UpdateHierarchies(bool tickFrame, float alpha) {
    if (!g_pedSkeleton) return;
    for (int i = 0; i < MaxPed; ++i) {
        Slot& s = g_ped[i];
        HierSet& h = g_pedHier[i];
        if (!s.valid) { h.valid = false; h.applied = false; continue; }
        void* hier = nullptr; uint8_t* mats = nullptr; int n = 0;
        if (!GetHier(s.ent, hier, mats, n)) { h.valid = false; h.applied = false; continue; }
        const bool sameSet = h.valid && h.hier == hier && h.mats == mats && h.n == n;
        if (tickFrame || !sameSet) {
            Pose now[MaxHier], M;
            if (!ReadHier(mats, n, now) || !EntityMatrix(s.ent, M)) { h.valid = false; continue; }
            for (int k = 0; k < n; ++k) { Pose l; PoseToLocal(M, now[k], l); now[k] = l; }
            h.moved = false;
            if (!sameSet) { for (int k = 0; k < n; ++k) { h.prev[k] = now[k]; h.curr[k] = now[k]; } }
            else {
                for (int k = 0; k < n; ++k) {
                    h.prev[k] = h.curr[k]; h.curr[k] = now[k];
                    if (std::memcmp(&h.prev[k], &h.curr[k], sizeof(Pose)) != 0) h.moved = true;
                }
            }
            h.valid = true; h.hier = hier; h.mats = mats; h.n = n;
            ++g_stats.hierPeds; g_stats.hierNodes += n;
            if (s.ent == g_playerPedForStats) g_stats.playerHierNodes = n;
        }
        if (!h.moved) continue;
        Pose out[MaxHier], M;
        if (!EntityMatrix(s.ent, M)) continue;
        for (int k = 0; k < n; ++k) {
            Pose b;
            if (!BlendBone(h.prev[k], h.curr[k], alpha, b)) b = h.curr[k];
            PoseToWorld(M, b, out[k]);
        }
        if (WriteHier(mats, n, out)) { h.applied = true; ++g_stats.hierApplied; }
    }
}

void RestoreHierarchies() {
    for (int i = 0; i < MaxPed; ++i) {
        HierSet& h = g_pedHier[i];
        if (!h.applied) continue;
        Pose M, out[MaxHier];
        if (g_ped[i].valid && EntityMatrix(g_ped[i].ent, M)) {
            for (int k = 0; k < h.n; ++k) PoseToWorld(M, h.curr[k], out[k]);
            WriteHier(h.mats, h.n, out);
        }
        h.applied = false;
    }
}

// ---- camera: the RwFrame of Scene.m_pRwCamera (modelling at +0x10, ltm at +0x50) ----------------
uint8_t* CameraFrame() {
    __try {
        auto* cam = *reinterpret_cast<uint8_t**>(ADDR_SceneCam);
        if (!cam) return nullptr;
        return *reinterpret_cast<uint8_t**>(cam + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

}  // namespace

Timing g_timing{};
double NowUs() {
    static LARGE_INTEGER fq;
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return double(q.QuadPart) * 1e6 / double(fq.QuadPart);
}

void SetDebug(bool on) { g_dbgOn = on; }
void SetVehicleParts(bool on) { g_vehParts = on; }
bool VehicleParts() { return g_vehParts; }
void SetPedSkeleton(bool on) { g_pedSkeleton = on; }
bool PedSkeleton() { return g_pedSkeleton; }

// Dump of the player ped's frame tree (raw dwords), to learn how its skeleton is linked.
void DumpPlayerFrames(const wchar_t* path) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"w") != 0 || !fp) return;
    __try {
        const uint8_t focus = *reinterpret_cast<uint8_t*>(0xB7CD74);
        const uintptr_t ped = *reinterpret_cast<uintptr_t*>(0xB7CD98 + 0x190 * focus);
        EntPtrs e{};
        if (!ped || !GetEntity(ped, e)) { std::fprintf(fp, "no player ped\n"); std::fclose(fp); return; }
        void* clump = *reinterpret_cast<void**>(ped + OFF_EntRwObject);
        std::fprintf(fp, "ped=%08X clump=%08X clumpFrame=%08X\n", unsigned(ped), unsigned(reinterpret_cast<uintptr_t>(clump)),
                     unsigned(reinterpret_cast<uintptr_t>(e.frame)));
        std::fprintf(fp, "clump dwords:");
        for (int i = 0; i < 12; ++i) std::fprintf(fp, " %08X", reinterpret_cast<uint32_t*>(clump)[i]);
        std::fprintf(fp, "\n");
        uint8_t* f = static_cast<uint8_t*>(e.frame);
        for (int n = 0; n < 40 && f; ++n) {
            std::fprintf(fp, "frame %2d @%08X obj:", n, unsigned(reinterpret_cast<uintptr_t>(f)));
            for (int i = 0; i < 3; ++i) std::fprintf(fp, " %08X", reinterpret_cast<uint32_t*>(f)[i]);
            std::fprintf(fp, " | pos %.2f %.2f %.2f | list %08X %08X child %08X next %08X root %08X\n",
                         *reinterpret_cast<float*>(f + 0x40), *reinterpret_cast<float*>(f + 0x44), *reinterpret_cast<float*>(f + 0x48),
                         *reinterpret_cast<uint32_t*>(f + 0x90), *reinterpret_cast<uint32_t*>(f + 0x94),
                         *reinterpret_cast<uint32_t*>(f + 0x98), *reinterpret_cast<uint32_t*>(f + 0x9C), *reinterpret_cast<uint32_t*>(f + 0xA0));
            uint8_t* child = *reinterpret_cast<uint8_t**>(f + 0x98);
            uint8_t* next = *reinterpret_cast<uint8_t**>(f + 0x9C);
            f = child ? child : next;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { std::fprintf(fp, "exception while dumping\n"); }
    std::fclose(fp);
}

void DumpDebug(const wchar_t* csvPath) {
    if (!g_dbgOn) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, csvPath, L"w") != 0 || !f) return;
    std::fputs("kind,tick,alpha,tMs,atPrevX,atPrevY,atCurrX,atCurrY,atOutX,atOutY,rawX,rawY,rawZ,prevX,prevY,prevZ,currX,currY,currZ,outX,outY,outZ\n", f);
    const unsigned n = g_dbgCount < DbgN ? g_dbgCount : DbgN, start = g_dbgCount - n;
    for (unsigned i = 0; i < n; ++i) {
        const DbgRec& r = g_dbg[(start + i) % DbgN];
        std::fprintf(f, "%c,%d,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", r.kind, r.tick ? 1 : 0, r.alpha,
                     r.tMs, r.atPrev[0], r.atPrev[1], r.atCurr[0], r.atCurr[1], r.atOut[0], r.atOut[1],
                     r.raw[0], r.raw[1], r.raw[2], r.prev[0], r.prev[1], r.prev[2], r.curr[0], r.curr[1], r.curr[2],
                     r.out[0], r.out[1], r.out[2]);
    }
    std::fclose(f);
}

Stats GetAndResetStats() {
    Stats s = g_stats;
    const float hand = g_stats.cameraHandedness;
    g_stats = Stats{};
    g_stats.cameraHandedness = hand;
    return s;
}

void OnTick() {
    __try {
        const uint8_t focus = *reinterpret_cast<uint8_t*>(0xB7CD74);
        g_playerPedForStats = *reinterpret_cast<uintptr_t*>(0xB7CD98 + 0x190 * focus);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_playerPedForStats = 0; }
    SnapshotPool(ADDR_PedPool, STRIDE_Ped, g_ped, MaxPed, nullptr);
    SnapshotPool(ADDR_VehPool, STRIDE_Veh, g_veh, MaxVeh, g_vehParts ? g_vehBones : nullptr);
    SnapshotPool(ADDR_ObjPool, STRIDE_Obj, g_obj, MaxObj, nullptr);
}

void ApplyEntities(float alpha) {
    ApplyPool(g_ped, MaxPed, alpha, nullptr);
    ApplyPool(g_veh, MaxVeh, alpha, g_vehParts ? g_vehBones : nullptr);
    ApplyPool(g_obj, MaxObj, alpha, nullptr);
}

void ApplyHierarchies(bool tickFrame, float alpha) { UpdateHierarchies(tickFrame, alpha); }

void ApplyCamera(bool tickFrame, float alpha) {
    uint8_t* f = CameraFrame();
    if (!f) return;
    __try {
        if (tickFrame || !g_cam.valid) {
            Pose now{}; ReadPose(f + 0x10, now);   // modelling matrix: what the game writes (the LTM at +0x50 is a lazily synced cache)
            if (!g_cam.valid) g_cam.prev = g_cam.curr = now;
            else { g_cam.prev = g_cam.curr; g_cam.curr = now; if (Teleported(g_cam.prev, g_cam.curr)) g_cam.prev = g_cam.curr; }
            g_cam.valid = true;
        }
        if (Same(g_cam.prev, g_cam.curr)) {
            DbgPush('c', tickFrame, alpha, g_cam.curr.p, g_cam.prev, g_cam.curr, g_cam.curr);   // unchanged: nothing to blend
            g_cam.applied = false;
            return;
        }
        Pose p{}; Blend(g_cam.prev, g_cam.curr, alpha, p);
        if (Dot(p.r, g_cam.curr.r) < 0.0f) ++g_stats.cameraFlipped;
        { float x[3]; Cross(g_cam.curr.r, g_cam.curr.u, x); g_stats.cameraHandedness = Dot(x, g_cam.curr.a); }
        { Pose raw{}; ReadPose(f + 0x10, raw); DbgPush('C', tickFrame, alpha, raw.p, g_cam.prev, g_cam.curr, p); }
        WritePose(f + 0x10, p);   // modelling
        WritePose(f + 0x50, p);   // ltm (root frame: identical)
        g_cam.applied = true; ++g_stats.cameraApplied;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_cam.applied = false; }
}

// ---- vehicle state guard around CRenderer::PreRender on render-only frames ------------------------------
// PreRender mutates vehicle members (suspension, wheel state...) every call; on render-only frames that would run it
// several times per simulated tick and change the physics (wobbling rear axle, no grip on sand). Save/restore the
// raw vehicle objects so only the tick frame's PreRender persists.

// Roll back only plain data (floats, small ints). Pointer-like values (lists, sub-objects that the game may have
// created or freed in the meantime) are left alone: rolling those back crashes the game later.
static bool LooksLikePointer(uint32_t v) { return v >= 0x10000u && v < 0x3A000000u; }
static void RestoreScalars(void* dst, const uint8_t* saved, size_t size) {
    auto* d = static_cast<uint32_t*>(dst);
    const auto* s = reinterpret_cast<const uint32_t*>(saved);
    for (size_t i = 0; i < size / 4; ++i) {
        const uint32_t now = d[i], old = s[i];
        if (now == old || LooksLikePointer(now) || LooksLikePointer(old)) continue;
        d[i] = old;
    }
}

static uint8_t g_vehSave[MaxVeh][STRIDE_Veh];
static bool g_vehSaved[MaxVeh];
static uint8_t g_pedSave[MaxPed][STRIDE_Ped];
static bool g_pedSaved[MaxPed];

void SavePeds() {
    for (int i = 0; i < MaxPed; ++i) {
        g_pedSaved[i] = false;
        if (!g_ped[i].valid) continue;
        __try { std::memcpy(g_pedSave[i], reinterpret_cast<void*>(g_ped[i].ent), STRIDE_Ped); g_pedSaved[i] = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void RestorePeds() {
    for (int i = 0; i < MaxPed; ++i) {
        if (!g_pedSaved[i]) continue;
        __try { RestoreScalars(reinterpret_cast<void*>(g_ped[i].ent), g_pedSave[i], STRIDE_Ped); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_pedSaved[i] = false;
    }
}

void SaveVehicles() {
    for (int i = 0; i < MaxVeh; ++i) {
        g_vehSaved[i] = false;
        if (!g_veh[i].valid) continue;
        __try { std::memcpy(g_vehSave[i], reinterpret_cast<void*>(g_veh[i].ent), STRIDE_Veh); g_vehSaved[i] = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void RestoreVehicles() {
    for (int i = 0; i < MaxVeh; ++i) {
        if (!g_vehSaved[i]) continue;
        __try { RestoreScalars(reinterpret_cast<void*>(g_veh[i].ent), g_vehSave[i], STRIDE_Veh); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_vehSaved[i] = false;
    }
}

void Restore() {
    RestorePool(g_ped, MaxPed, nullptr);
    RestoreHierarchies();
    RestorePool(g_veh, MaxVeh, g_vehBones);
    RestorePool(g_obj, MaxObj, nullptr);
    if (g_cam.applied) {
        uint8_t* f = CameraFrame();
        if (f) { __try { WritePose(f + 0x10, g_cam.curr); WritePose(f + 0x50, g_cam.curr); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        g_cam.applied = false;
    }
}

}  // namespace Interp
