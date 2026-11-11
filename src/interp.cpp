#include "interp.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Interp {
namespace {

// ---- GTA SA 1.0 US layout ---------------------------------------------------------------
constexpr uintptr_t ADDR_PedPool = 0xB74490, ADDR_VehPool = 0xB74494, ADDR_ObjPool = 0xB7449C;
constexpr uintptr_t STRIDE_Ped = 0x7C4, STRIDE_Veh = 0xA18, STRIDE_Obj = 0x19C;
constexpr uintptr_t OFF_PoolObjects = 0, OFF_PoolFlags = 4, OFF_PoolSize = 8;   // CPool
constexpr uintptr_t OFF_EntMatrix = 0x14;       // CPlaceable::m_pMatrix (CMatrix*)
constexpr uintptr_t OFF_EntRwObject = 0x18;     // CEntity::m_pRwObject
constexpr uintptr_t OFF_FrameChild = 0x98, OFF_FrameNext = 0x9C;   // RwFrame::child / ::next
constexpr uintptr_t ADDR_SceneCam = 0xC1703C;   // RwCamera* Scene.m_pRwCamera
constexpr uintptr_t ADDR_CutsceneRunning = 0xB5F851;   // bool CCutsceneMgr::ms_running
constexpr uintptr_t VT_CutsceneObject = 0x868A64;      // CCutsceneObject vtable
constexpr uintptr_t OFF_CsAttachTo = 0x17C, OFF_CsAttachObj = 0x180;   // bone index (or RwFrame*) / holder object
constexpr uintptr_t ADDR_UpdateRwMatrix = 0x59AD70;      // void CMatrix::UpdateRwMatrix(RwMatrix*)  thiscall
constexpr uintptr_t ADDR_RwFrameUpdateObjects = 0x7F0910; // RwBool RwFrameUpdateObjects(RwFrame*)   cdecl
constexpr uintptr_t ADDR_RwFrameGetLTM = 0x7F0990;        // RwMatrix* RwFrameGetLTM(RwFrame*)        cdecl
constexpr uintptr_t ADDR_GetAnimHierarchyFromSkinClump = 0x734A40;   // RpHAnimHierarchy* (RpClump*)  cdecl
constexpr float TeleportDist = 20.0f;

using UpdateRwMatrixFn = void(__thiscall*)(void* cmatrix, void* rwMatrix);
using FrameUpdateFn = int(__cdecl*)(void* frame);
using FrameLtmFn = const uint8_t*(__cdecl*)(uintptr_t frame);

struct Pose { float r[3], u[3], a[3], p[3]; };   // right, up, at, pos

struct Slot {
    bool valid = false, applied = false;
    uintptr_t ent = 0;
    uint8_t gen = 0;
    Pose prev{}, curr{};
    Pose shown{};   // the blended pose last written: anything else found there was put by someone else
};

constexpr int MaxPed = 512, MaxVeh = 512, MaxObj = 1024;
Slot g_ped[MaxPed], g_veh[MaxVeh], g_obj[MaxObj];

// Child frames of a clump (wheels, doors, parts of cutscene objects): animated by the game once per tick.
constexpr int MaxBones = 160;   // cars have many parts (doors, bonnet, boot, bumpers, ok/damaged variants)
struct BoneSet {
    int n = 0;
    void* f[MaxBones];
    Pose prev[MaxBones], curr[MaxBones];
    bool moved = false, applied = false;
};
BoneSet g_vehBones[MaxVeh];
bool g_pedSkeleton = true;
bool g_vehParts = true;
bool g_csSkeleton = true;

struct CamState { bool valid = false, applied = false; Pose prev{}, curr{}; } g_cam;
bool g_camNeedCapture = false;   // a tick ran: read the game's new camera once (ApplyCamera runs twice per frame)

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
float AxisLen(const float* v) { return std::sqrt(Dot(v, v)); }
void Norm(float* v) {
    const float l = AxisLen(v);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

bool Teleported(const Pose& a, const Pose& b) {
    const float d[3] = {b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2]};
    return Dot(d, d) > TeleportDist * TeleportDist || Dot(a.a, b.a) < 0.5f || Dot(a.u, b.u) < 0.5f;
}

bool Same(const Pose& a, const Pose& b) { return std::memcmp(&a, &b, sizeof(Pose)) == 0; }

// Another mod (CheatMenu teleport, trainers...) may move an entity between two ticks, from its own drawing hook.
// The blend then wrote the old place back on the next frame, and the restore before the tick put back the tick's
// pose: the teleport lasted one frame. A pose that is not the one we wrote (moved > 0.5 m or turned) is taken as
// the new real state instead.
bool MovedByOther(const Pose& expected, const Pose& found) {
    const float d[3] = {found.p[0] - expected.p[0], found.p[1] - expected.p[1], found.p[2] - expected.p[2]};
    return Dot(d, d) > 0.25f || Dot(expected.a, found.a) < 0.95f || Dot(expected.u, found.u) < 0.95f;
}

// Entity / camera blend: linear, then re-orthonormalised (right-handed: right x up = at).
void Blend(const Pose& a, const Pose& b, float t, Pose& o) {
    for (int i = 0; i < 3; ++i) {
        o.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
        o.u[i] = a.u[i] + (b.u[i] - a.u[i]) * t;
        o.a[i] = a.a[i] + (b.a[i] - a.a[i]) * t;
    }
    Norm(o.a); Norm(o.u);
    Cross(o.u, o.a, o.r); Norm(o.r);
    Cross(o.a, o.r, o.u);
}

bool CutsceneRunning() { return *reinterpret_cast<const uint8_t*>(ADDR_CutsceneRunning) != 0; }

// ---- guarded game access -------------------------------------------------------------------
struct EntPtrs { uint8_t* cmat; void* rwMat; void* frame; };

bool GetEntity(uintptr_t ent, EntPtrs& e) {
    __try {
        e.cmat = *reinterpret_cast<uint8_t**>(ent + OFF_EntMatrix);
        void* rwObj = *reinterpret_cast<void**>(ent + OFF_EntRwObject);
        if (!e.cmat || !rwObj) return false;
        e.frame = *reinterpret_cast<void**>(static_cast<uint8_t*>(rwObj) + 4);   // RwObject::parent
        if (!e.frame) return false;
        // The entity's CMatrix is NOT attached to the frame: it is copied into the frame's own modelling matrix,
        // exactly like CEntity::UpdateRW does.
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

void UpdateFrame(void* frame) {
    __try { reinterpret_cast<FrameUpdateFn>(ADDR_RwFrameUpdateObjects)(frame); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool ReadEntityPose(uintptr_t ent, Pose& pose) {   // the CMatrix
    EntPtrs e{};
    if (!GetEntity(ent, e)) return false;
    __try { ReadPose(e.cmat, pose); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool EntityMatrix(uintptr_t ent, Pose& M) {   // the RwFrame's modelling matrix
    EntPtrs e{};
    if (!GetEntity(ent, e)) return false;
    __try { ReadPose(static_cast<const uint8_t*>(e.rwMat), M); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsCutsceneObject(uintptr_t ent) {
    __try { return *reinterpret_cast<const uintptr_t*>(ent) == VT_CutsceneObject; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- bones (child frames) ------------------------------------------------------------------
// Depth-first walk of the frame tree below `root` (the root itself is handled through the CMatrix).
bool CollectBones(void* root, void** frames, Pose* poses, int& n) {
    n = 0;
    __try {
        void* stack[64]; int sp = 0;
        uint8_t* f = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(root) + OFF_FrameChild);
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

// Rotation as a quaternion (x, y, z, w) from three orthonormal axes (rows r, u, a of an RW matrix).
struct Quat { float x, y, z, w; };
void AxesToQuat(const float* r, const float* u, const float* a, Quat& q) {
    // RW matrices are row vectors: m[i][j] with rows r, u, a. Standard conversion on the transpose.
    const float m00 = r[0], m01 = u[0], m02 = a[0], m10 = r[1], m11 = u[1], m12 = a[1], m20 = r[2], m21 = u[2], m22 = a[2];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
}
void QuatToAxes(const Quat& q, float* r, float* u, float* a) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    r[0] = 1 - 2 * (y * y + z * z); u[0] = 2 * (x * y - z * w);     a[0] = 2 * (x * z + y * w);
    r[1] = 2 * (x * y + z * w);     u[1] = 1 - 2 * (x * x + z * z); a[1] = 2 * (y * z - x * w);
    r[2] = 2 * (x * z - y * w);     u[2] = 2 * (y * z + x * w);     a[2] = 1 - 2 * (x * x + y * y);
}

// Blend of two bone poses: rotation by quaternion slerp (doors, fast limbs), axis lengths (bones may be scaled) and
// position linearly. Returns false when the rotation changed too much in one tick to tell its direction (> ~150
// degrees, fast spinning wheels...) or the axes are degenerate: the caller then keeps the real pose.
bool BlendBone(const Pose& a, const Pose& b, float t, Pose& o) {
    float la[3] = {AxisLen(a.r), AxisLen(a.u), AxisLen(a.a)}, lb[3] = {AxisLen(b.r), AxisLen(b.u), AxisLen(b.a)};
    // a bone shrunk to nothing (the head of a ped shot in the head with a shotgun, for example): it can't be turned
    // into a rotation, but it is invisible anyway. It takes its current state, only its place is blended, so the
    // rest of the skeleton keeps blending (before, the whole body fell back to the 30 fps pose).
    for (int i = 0; i < 3; ++i) if (la[i] < 1e-6f || lb[i] < 1e-6f) {
        o = b;
        for (int k = 0; k < 3; ++k) o.p[k] = a.p[k] + (b.p[k] - a.p[k]) * t;
        return true;
    }
    float ar[3], au[3], aa[3], br[3], bu[3], ba[3];
    for (int i = 0; i < 3; ++i) {
        ar[i] = a.r[i] / la[0]; au[i] = a.u[i] / la[1]; aa[i] = a.a[i] / la[2];
        br[i] = b.r[i] / lb[0]; bu[i] = b.u[i] / lb[1]; ba[i] = b.a[i] / lb[2];
    }
    // mirrored (scaled -1) matrices can't be expressed as a rotation: blend only if both have the same handedness
    float c[3]; Cross(ar, au, c); const bool ha = Dot(c, aa) > 0.0f;
    Cross(br, bu, c); const bool hb = Dot(c, ba) > 0.0f;
    if (ha != hb) return false;
    if (!ha) { for (int i = 0; i < 3; ++i) { aa[i] = -aa[i]; ba[i] = -ba[i]; } }
    Quat qa, qb; AxesToQuat(ar, au, aa, qa); AxesToQuat(br, bu, ba, qb);
    float d = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
    if (d < 0.0f) { qb = {-qb.x, -qb.y, -qb.z, -qb.w}; d = -d; }
    if (d < 0.26f) return false;                                        // > ~150 degrees per tick
    Quat q;
    if (d > 0.9995f) {
        q = {qa.x + (qb.x - qa.x) * t, qa.y + (qb.y - qa.y) * t, qa.z + (qb.z - qa.z) * t, qa.w + (qb.w - qa.w) * t};
    } else {
        const float th = std::acos(d), s = std::sin(th);
        const float wa = std::sin((1.0f - t) * th) / s, wb = std::sin(t * th) / s;
        q = {qa.x * wa + qb.x * wb, qa.y * wa + qb.y * wb, qa.z * wa + qb.z * wb, qa.w * wa + qb.w * wb};
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q = {q.x / n, q.y / n, q.z / n, q.w / n};
    QuatToAxes(q, o.r, o.u, o.a);
    if (!ha) for (int i = 0; i < 3; ++i) o.a[i] = -o.a[i];
    const float l0 = la[0] + (lb[0] - la[0]) * t, l1 = la[1] + (lb[1] - la[1]) * t, l2 = la[2] + (lb[2] - la[2]) * t;
    for (int i = 0; i < 3; ++i) {
        o.r[i] *= l0; o.u[i] *= l1; o.a[i] *= l2;
        o.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
    }
    return true;
}

bool WriteBone(void* frame, const Pose& p) {
    __try { WritePose(static_cast<uint8_t*>(frame) + 0x10, p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Tick frames: shift curr to prev and read the new poses. A different frame list (new model, part added or removed)
// starts again without blending.
void SnapBones(BoneSet& b, void* root, bool fresh) {
    void* f2[MaxBones]; Pose p2[MaxBones]; int n2 = 0;
    b.moved = false;
    if (!CollectBones(root, f2, p2, n2)) { b.n = 0; return; }
    const bool same = !fresh && n2 == b.n && std::memcmp(f2, b.f, n2 * sizeof(void*)) == 0;
    if (!same) {
        b.n = n2;
        std::memcpy(b.f, f2, n2 * sizeof(void*));
        for (int i = 0; i < n2; ++i) { b.prev[i] = p2[i]; b.curr[i] = p2[i]; }
        return;
    }
    for (int i = 0; i < n2; ++i) {
        b.prev[i] = b.curr[i];
        b.curr[i] = p2[i];
        if (!Same(b.prev[i], b.curr[i])) b.moved = true;
    }
}

void ApplyBones(BoneSet& b, float t) {
    for (int i = 0; i < b.n; ++i) {
        const Pose& p0 = b.prev[i];
        const Pose& p1 = b.curr[i];
        if (Same(p0, p1)) continue;
        Pose o{};
        if (!BlendBone(p0, p1, t, o)) o = p1;                   // keep the real pose
        WriteBone(b.f[i], o);
    }
    b.applied = true;
}

void RestoreBones(BoneSet& b) {
    if (!b.applied) return;
    for (int i = 0; i < b.n; ++i) WriteBone(b.f[i], b.curr[i]);
    b.applied = false;
}

// Tick frames capture the parts after PreRender (which rewrites some of them from the simulated state), every frame
// shows them blended, the tick frame included (it shows the start of the new interval, not the new pose).
void StepBones(BoneSet& b, void* root, bool tickFrame, bool fresh, float alpha) {
    if (tickFrame) { b.applied = false; SnapBones(b, root, fresh); }
    if (b.moved) { ApplyBones(b, alpha); UpdateFrame(root); }
}

// ---- entity pools ------------------------------------------------------------------------------
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

void SnapshotPool(uintptr_t poolAddr, uintptr_t stride, Slot* slots, int maxSlots) {
    PoolView v{};
    if (!GetPool(poolAddr, v)) { for (int i = 0; i < maxSlots; ++i) slots[i].valid = false; return; }
    const int n = v.size < maxSlots ? v.size : maxSlots;
    for (int i = 0; i < n; ++i) {
        Slot& s = slots[i];
        uint8_t flag = 0x80;
        __try { flag = v.flags[i]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        s.applied = false;
        if (flag & 0x80) { s.valid = false; continue; }          // free slot
        const uintptr_t ent = reinterpret_cast<uintptr_t>(v.base) + stride * i;
        Pose now{};
        if (!ReadEntityPose(ent, now)) { s.valid = false; continue; }
        const bool fresh = !s.valid || s.ent != ent || s.gen != flag;   // new entity: nothing to blend from
        s.prev = fresh ? now : s.curr;
        s.curr = now;
        if (Teleported(s.prev, s.curr)) s.prev = s.curr;      // teleport / camera cut: snap
        s.ent = ent; s.gen = flag; s.valid = true;
        s.shown = s.curr;
    }
    for (int i = n; i < maxSlots; ++i) slots[i].valid = false;
}

void ApplyPool(Slot* slots, int maxSlots, float alpha) {
    for (int i = 0; i < maxSlots; ++i) {
        Slot& s = slots[i];
        if (!s.valid) continue;
        Pose now{};
        if (ReadEntityPose(s.ent, now) && MovedByOther(s.shown, now)) {   // moved by another mod: keep it there
            s.prev = s.curr = s.shown = now;
            s.applied = false;
            continue;
        }
        if (Same(s.prev, s.curr)) continue;
        Pose p{}; Blend(s.prev, s.curr, alpha, p);
        s.applied = PushPose(s.ent, p);
        if (s.applied) ReadEntityPose(s.ent, s.shown);
    }
}

void RestorePool(Slot* slots, int maxSlots) {
    for (int i = 0; i < maxSlots; ++i) {
        Slot& s = slots[i];
        if (!s.applied) continue;
        s.applied = false;
        Pose now{};
        if (ReadEntityPose(s.ent, now) && MovedByOther(s.shown, now)) { s.prev = s.curr = s.shown = now; continue; }
        PushPose(s.ent, s.curr);
        s.shown = s.curr;
    }
}

// ---- skeletons: RpHAnim hierarchy matrices ---------------------------------------------------
// The bones of skinned models are not RwFrames: their pose lives in the hierarchy's pMatrixArray (numNodes x
// RwMatrix, world space), recomputed from the animation every time the game updates the hierarchy.
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
        // Only a clump has a skeleton. Most objects are a single atomic: the lookup then walked random memory and
        // could hand back a "hierarchy" the blend wrote into (heap corruption, crash after loading a save).
        if (!clump || *static_cast<const uint8_t*>(clump) != 2) return false;   // RwObject::type, rpCLUMP = 2
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

// The hierarchy matrices are rebuilt by the game from the entity frame, so they are kept relative to it:
// world = local * M.
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

// The ped frame as it was right before CRenderer::PreRender. PreRender shifts the frame of some peds (lying/dead
// ones) a bit more on every call, and only the next tick puts it back: on render-only frames the body slid and
// snapped back each tick (vibration). The skeleton is anchored to this pose instead, and the frame is put back.
Pose g_pedPre[MaxPed];
bool g_pedPreOk[MaxPed];
bool PedAnchor(int i, uintptr_t ent, Pose& M) {
    if (g_pedPreOk[i]) { M = g_pedPre[i]; return true; }
    return EntityMatrix(ent, M);
}
void ResetPedFrame(int i) {
    if (!g_pedPreOk[i] || !g_ped[i].valid) return;
    EntPtrs e{};
    if (!GetEntity(g_ped[i].ent, e)) return;
    __try {
        Pose now; ReadPose(static_cast<const uint8_t*>(e.rwMat), now);
        if (Same(now, g_pedPre[i])) return;
        WritePose(static_cast<uint8_t*>(e.rwMat), g_pedPre[i]);
        reinterpret_cast<FrameUpdateFn>(ADDR_RwFrameUpdateObjects)(e.frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// One skeleton: captured (relative to its anchor) on tick frames, blended on every frame. pedIdx < 0 = an object
// (cutscene characters are skinned objects, not peds), anchored to its frame.
void HierStep(Slot& s, HierSet& h, int pedIdx, bool tickFrame, float alpha) {
    auto anchor = [&](Pose& M) { return pedIdx >= 0 ? PedAnchor(pedIdx, s.ent, M) : EntityMatrix(s.ent, M); };
    void* hier = nullptr; uint8_t* mats = nullptr; int n = 0;
    if (!GetHier(s.ent, hier, mats, n)) { h.valid = false; h.applied = false; return; }
    const bool sameSet = h.valid && h.hier == hier && h.mats == mats && h.n == n;
    if (tickFrame || !sameSet) {
        Pose now[MaxHier], M;
        if (!ReadHier(mats, n, now) || !anchor(M)) { h.valid = false; return; }
        for (int k = 0; k < n; ++k) { Pose l; PoseToLocal(M, now[k], l); now[k] = l; }
        h.moved = false;
        for (int k = 0; k < n; ++k) {
            h.prev[k] = sameSet ? h.curr[k] : now[k];
            h.curr[k] = now[k];
            if (!Same(h.prev[k], h.curr[k])) h.moved = true;
        }
        h.valid = true; h.hier = hier; h.mats = mats; h.n = n;
    }
    // a still skeleton is left alone on tick frames; on render-only frames PreRender may have moved a ped with its frame
    if (!h.moved && (tickFrame || pedIdx < 0 || !g_pedPreOk[pedIdx])) return;
    Pose out[MaxHier], M;
    if (!anchor(M)) return;
    if (!tickFrame && pedIdx >= 0) ResetPedFrame(pedIdx);
    // one bone that can't be blended (turned too much this tick) would jump alone and stretch the skin:
    // then the whole skeleton shows the real pose
    bool all = true;
    for (int k = 0; k < n && all; ++k) {
        Pose b;
        if (!BlendBone(h.prev[k], h.curr[k], alpha, b)) all = false;
        else PoseToWorld(M, b, out[k]);
    }
    if (!all) for (int k = 0; k < n; ++k) PoseToWorld(M, h.curr[k], out[k]);
    if (WriteHier(mats, n, out)) h.applied = true;
}

void RestoreHier(HierSet& h, const Slot& s) {
    if (!h.applied) return;
    Pose M, out[MaxHier];
    // the skeleton may have been rebuilt or freed since it was blended (clothes change, loading a save): only write
    // into the very same one
    void* hier = nullptr; uint8_t* mats = nullptr; int n = 0;
    const bool same = s.valid && GetHier(s.ent, hier, mats, n) && hier == h.hier && mats == h.mats && n == h.n;
    if (same && EntityMatrix(s.ent, M)) {
        for (int k = 0; k < h.n; ++k) PoseToWorld(M, h.curr[k], out[k]);
        WriteHier(h.mats, h.n, out);
    }
    h.applied = false;
}

void UpdatePedHierarchies(bool tickFrame, float alpha) {
    if (!g_pedSkeleton) return;
    for (int i = 0; i < MaxPed; ++i) {
        Slot& s = g_ped[i];
        HierSet& h = g_pedHier[i];
        if (!s.valid) { h.valid = false; h.applied = false; continue; }
        HierStep(s, h, i, tickFrame, alpha);
    }
}

// ---- cutscenes ---------------------------------------------------------------------------------
// Objects with a skeleton (cutscene characters) and cutscene objects without one (props: their animation moves
// child frames of the clump): a few of them, found again on each tick.
constexpr int MaxCs = 48;
struct CsEntry { bool used; int slot; uintptr_t ent; bool skinned; HierSet h; BoneSet b; };   // all zero at start: kept out of the .asi file
CsEntry g_cs[MaxCs];

void ClearCutscene() {
    for (auto& c : g_cs) { c.used = false; c.h.valid = false; c.h.applied = false; c.b.n = 0; c.b.moved = false; c.b.applied = false; }
}

void FindCutsceneObjects() {
    bool taken[MaxObj] = {};
    for (auto& c : g_cs) {   // keep the ones still there
        if (!c.used) continue;
        if (!g_obj[c.slot].valid || g_obj[c.slot].ent != c.ent) { c.used = false; c.h.valid = false; c.b.n = 0; c.b.moved = false; continue; }
        taken[c.slot] = true;
    }
    for (int i = 0; i < MaxObj; ++i) {
        if (!g_obj[i].valid || taken[i]) continue;
        void* h = nullptr; uint8_t* m = nullptr; int n = 0;
        const bool skinned = GetHier(g_obj[i].ent, h, m, n);
        if (!skinned && !IsCutsceneObject(g_obj[i].ent)) continue;
        CsEntry* free = nullptr;
        for (auto& c : g_cs) if (!c.used) { free = &c; break; }
        if (!free) break;
        free->used = true; free->slot = i; free->ent = g_obj[i].ent; free->skinned = skinned;
        free->h.valid = free->h.applied = false;
        free->b.n = 0; free->b.moved = free->b.applied = false;
    }
}

// Props attached to a bone or a frame (CCutsceneObject::PreRender, 0x5B1E00): PreRender copies the bone's matrix
// before the skeletons are blended, so they are put on the blended bone again.
void FixAttachedProps() {
    for (const auto& c : g_cs) {
        if (!c.used) continue;
        __try {
            const uintptr_t to = *reinterpret_cast<uintptr_t*>(c.ent + OFF_CsAttachTo);
            if (!to) continue;
            const uintptr_t holder = *reinterpret_cast<uintptr_t*>(c.ent + OFF_CsAttachObj);
            const uint8_t* m = nullptr;
            if (holder) {
                void* h = nullptr; uint8_t* mats = nullptr; int n = 0;
                if (!GetHier(holder, h, mats, n) || to >= uintptr_t(n)) continue;
                m = mats + 0x40 * to;
            } else {
                m = reinterpret_cast<FrameLtmFn>(ADDR_RwFrameGetLTM)(to);
            }
            if (!m) continue;
            Pose p; ReadPose(m, p);
            PushPose(c.ent, p);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void UpdateCutscene(bool tickFrame, float alpha) {
    if (!g_csSkeleton) { ClearCutscene(); return; }
    if (tickFrame) FindCutsceneObjects();
    for (auto& c : g_cs) {
        if (!c.used) continue;
        if (c.skinned) { HierStep(g_obj[c.slot], c.h, -1, tickFrame, alpha); continue; }
        EntPtrs e{};
        if (GetEntity(c.ent, e)) StepBones(c.b, e.frame, tickFrame, c.b.n == 0, alpha);
    }
    FixAttachedProps();
}

void RestoreCutscene() {
    for (auto& c : g_cs) {
        if (!c.used) continue;
        RestoreHier(c.h, g_obj[c.slot]);
        RestoreBones(c.b);
    }
}

// ---- vehicle parts ---------------------------------------------------------------------------
// Wheels, doors, forks... are child frames that the vehicles' PreRender rewrites on every frame from the last
// simulated state, so they are handled after PreRender like the skeletons.
uintptr_t g_vehPartsEnt[MaxVeh];
void UpdateVehicleParts(bool tickFrame, float alpha) {
    for (int i = 0; i < MaxVeh; ++i) {
        Slot& s = g_veh[i];
        BoneSet& b = g_vehBones[i];
        if (!g_vehParts || !s.valid) { b.n = 0; b.moved = false; g_vehPartsEnt[i] = 0; continue; }
        EntPtrs e{};
        if (!GetEntity(s.ent, e)) { b.n = 0; b.moved = false; continue; }
        const bool fresh = g_vehPartsEnt[i] != s.ent;
        if (fresh && !tickFrame) continue;   // a new vehicle starts at its first tick
        StepBones(b, e.frame, tickFrame, fresh, alpha);
        g_vehPartsEnt[i] = s.ent;
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
float g_camStep = 0.0f, g_camPrevU[3] = {0, 0, 0};

// ---- physics guard around CRenderer::PreRender on render-only frames ----------------------------
// PreRender mutates vehicle and ped members (suspension, wheel state...) on every call; on render-only frames that
// would run it several times per simulated tick and change the physics (wobbling rear axle, no grip on sand).
// The raw objects are saved before and their plain data put back at the end of the frame.

// Roll back only plain data (floats, small ints). Pointer-like values (lists, sub-objects that the game may have
// created or freed in the meantime) are left alone: rolling those back crashes the game later.
bool LooksLikePointer(uint32_t v) { return v >= 0x10000u && v < 0x3A000000u; }
void RestoreScalars(void* dst, const uint8_t* saved, size_t size) {
    auto* d = static_cast<uint32_t*>(dst);
    const auto* s = reinterpret_cast<const uint32_t*>(saved);
    for (size_t i = 0; i < size / 4; ++i) {
        const uint32_t now = d[i], old = s[i];
        if (now == old || LooksLikePointer(now) || LooksLikePointer(old)) continue;
        d[i] = old;
    }
}

uint8_t g_vehSave[MaxVeh][STRIDE_Veh];
bool g_vehSaved[MaxVeh];
uint8_t g_pedSave[MaxPed][STRIDE_Ped];
bool g_pedSaved[MaxPed];
// The entity's CMatrix is a separate object (not inside the entity struct): PreRender can move it too (lying peds).
uint8_t g_pedMat[MaxPed][0x40], g_vehMat[MaxVeh][0x40];
uint8_t* g_pedMatPtr[MaxPed];
uint8_t* g_vehMatPtr[MaxVeh];
void SaveMat(uintptr_t ent, uint8_t* dst, uint8_t*& ptr) {
    ptr = nullptr;
    __try {
        uint8_t* m = *reinterpret_cast<uint8_t**>(ent + OFF_EntMatrix);
        if (m) { std::memcpy(dst, m, 0x40); ptr = m; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ptr = nullptr; }
}
bool MatMovedByOther(uintptr_t ent, const uint8_t* saved, uint8_t* ptr) {
    if (!ptr) return false;
    __try {
        uint8_t* m = *reinterpret_cast<uint8_t**>(ent + OFF_EntMatrix);
        if (m != ptr) return false;
        Pose a{}, b{}; ReadPose(saved, a); ReadPose(m, b);
        const float d[3] = {b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2]};
        return Dot(d, d) > 1.0f;   // PreRender itself only nudges things (lying peds): this is a teleport
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void RestoreMat(uintptr_t ent, const uint8_t* src, uint8_t* ptr) {
    if (!ptr) return;
    __try {
        uint8_t* m = *reinterpret_cast<uint8_t**>(ent + OFF_EntMatrix);
        if (m == ptr) std::memcpy(m, src, 0x40);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
template <size_t Size>
void SaveAll(const Slot* slots, int n, uint8_t (*save)[Size], bool* saved, uint8_t (*mat)[0x40], uint8_t** matPtr) {
    for (int i = 0; i < n; ++i) {
        saved[i] = false;
        if (!slots[i].valid) continue;
        SaveMat(slots[i].ent, mat[i], matPtr[i]);
        __try { std::memcpy(save[i], reinterpret_cast<void*>(slots[i].ent), Size); saved[i] = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
template <size_t Size>
void RestoreAll(const Slot* slots, int n, uint8_t (*save)[Size], bool* saved, uint8_t (*mat)[0x40], uint8_t** matPtr) {
    for (int i = 0; i < n; ++i) {
        if (!saved[i]) continue;
        if (MatMovedByOther(slots[i].ent, mat[i], matPtr[i])) { saved[i] = false; continue; }   // keep what the other mod did
        RestoreMat(slots[i].ent, mat[i], matPtr[i]);
        __try { RestoreScalars(reinterpret_cast<void*>(slots[i].ent), save[i], Size); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        saved[i] = false;
    }
}

}  // namespace

Timing g_timing{};
double NowUs() {
    static LARGE_INTEGER fq;
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return double(q.QuadPart) * 1e6 / double(fq.QuadPart);
}

void SetVehicleParts(bool on) { g_vehParts = on; }
bool VehicleParts() { return g_vehParts; }
void SetPedSkeleton(bool on) { g_pedSkeleton = on; }
bool PedSkeleton() { return g_pedSkeleton; }
void SetCutsceneSkeleton(bool on) { g_csSkeleton = on; }
bool CutsceneSkeleton() { return g_csSkeleton; }

// The game can delete entities outside the simulation tick: starting a new game or loading a save rebuilds the
// whole world from the menu. A slot whose entity is gone must be forgotten before anything is written back: its
// matrix, skeleton and part frames are freed memory (writing there corrupted the heap and crashed), and a new entity
// in the same slot must not get the old one's pose (the player stayed where the previous game left them).
void DropDeadPool(uintptr_t poolAddr, uintptr_t stride, Slot* slots, int maxSlots, HierSet* hier, BoneSet* bones) {
    PoolView v{};
    const bool ok = GetPool(poolAddr, v);
    for (int i = 0; i < maxSlots; ++i) {
        Slot& s = slots[i];
        if (!s.valid) continue;
        bool alive = false;
        if (ok && i < v.size) {
            __try { alive = v.flags[i] == s.gen && s.ent == reinterpret_cast<uintptr_t>(v.base) + stride * i; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (alive) continue;
        s.valid = false; s.applied = false;
        if (hier) { hier[i].applied = false; hier[i].valid = false; }
        if (bones) { bones[i].applied = false; bones[i].moved = false; bones[i].n = 0; }
    }
}
void DropDead() {
    DropDeadPool(ADDR_PedPool, STRIDE_Ped, g_ped, MaxPed, g_pedHier, nullptr);
    DropDeadPool(ADDR_VehPool, STRIDE_Veh, g_veh, MaxVeh, nullptr, g_vehBones);
    DropDeadPool(ADDR_ObjPool, STRIDE_Obj, g_obj, MaxObj, nullptr, nullptr);
    for (auto& c : g_cs) {
        if (c.used && !g_obj[c.slot].valid) { c.used = false; c.h.applied = false; c.b.applied = false; }
    }
}

void OnTick() {
    g_camNeedCapture = true;
    SnapshotPool(ADDR_PedPool, STRIDE_Ped, g_ped, MaxPed);
    SnapshotPool(ADDR_VehPool, STRIDE_Veh, g_veh, MaxVeh);   // parts: see UpdateVehicleParts
    SnapshotPool(ADDR_ObjPool, STRIDE_Obj, g_obj, MaxObj);
}

void ApplyEntities(float alpha) {
    DropDead();
    ApplyPool(g_ped, MaxPed, alpha);
    ApplyPool(g_veh, MaxVeh, alpha);
    ApplyPool(g_obj, MaxObj, alpha);
}

void ApplyHierarchies(bool tickFrame, float alpha) {
    DropDead();
    UpdateVehicleParts(tickFrame, alpha);
    UpdatePedHierarchies(tickFrame, alpha);
    UpdateCutscene(tickFrame, alpha);
    for (int i = 0; i < MaxPed; ++i) g_pedPreOk[i] = false;
}

// The ped frames as they are right before PreRender (see g_pedPre). On render-only frames a frame that a previous
// PreRender moved is put back first, so it does not drift further.
void CapturePedFrames(bool tickFrame) {
    DropDead();
    for (int i = 0; i < MaxPed; ++i) {
        g_pedPreOk[i] = g_ped[i].valid && ReadEntityPose(g_ped[i].ent, g_pedPre[i]);
        if (!g_pedPreOk[i] || tickFrame) continue;
        Pose f{};
        if (EntityMatrix(g_ped[i].ent, f) && !Same(f, g_pedPre[i])) PushPose(g_ped[i].ent, g_pedPre[i]);
    }
}

void ApplyCamera(bool tickFrame, float alpha) {
    uint8_t* f = CameraFrame();
    if (!f) return;
    __try {
        if ((tickFrame && g_camNeedCapture) || !g_cam.valid) {
            g_camNeedCapture = false;
            Pose now{}; ReadPose(f + 0x10, now);   // modelling matrix: what the game writes (the LTM at +0x50 is a lazily synced cache)
            if (!g_cam.valid) g_cam.prev = g_cam.curr = now;
            else {
                g_cam.prev = g_cam.curr; g_cam.curr = now;
                const float d[3] = {now.p[0] - g_cam.prev.p[0], now.p[1] - g_cam.prev.p[1], now.p[2] - g_cam.prev.p[2]};
                const float step = AxisLen(d);
                // cutscene shots: a cut can be only a few metres; a jump much bigger than the last step or a sharp turn
                const float la = AxisLen(now.u), lb = AxisLen(g_camPrevU);
                const bool turned = la > 1e-6f && lb > 1e-6f && Dot(now.u, g_camPrevU) / (la * lb) < 0.87f;   // > ~30 deg
                const bool cut = CutsceneRunning() && ((step > 1.0f && step > 4.0f * g_camStep + 0.25f) || turned);
                if (Teleported(g_cam.prev, g_cam.curr) || cut) g_cam.prev = g_cam.curr;
                g_camStep = step;
            }
            std::memcpy(g_camPrevU, now.u, 12);
            g_cam.valid = true;
        }
        if (Same(g_cam.prev, g_cam.curr)) { g_cam.applied = false; return; }   // unchanged: nothing to blend
        Pose p{}; Blend(g_cam.prev, g_cam.curr, alpha, p);
        WritePose(f + 0x10, p);   // modelling
        WritePose(f + 0x50, p);   // ltm (root frame: identical)
        g_cam.applied = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_cam.applied = false; }
}

void SavePeds() { SaveAll<STRIDE_Ped>(g_ped, MaxPed, g_pedSave, g_pedSaved, g_pedMat, g_pedMatPtr); }
void RestorePeds() { RestoreAll<STRIDE_Ped>(g_ped, MaxPed, g_pedSave, g_pedSaved, g_pedMat, g_pedMatPtr); }
void SaveVehicles() { SaveAll<STRIDE_Veh>(g_veh, MaxVeh, g_vehSave, g_vehSaved, g_vehMat, g_vehMatPtr); }
void RestoreVehicles() { RestoreAll<STRIDE_Veh>(g_veh, MaxVeh, g_vehSave, g_vehSaved, g_vehMat, g_vehMatPtr); }

// Loading a save (or starting a new game) from the menu frees and rebuilds every entity, skeleton and part frame,
// and the pools come back from the save with the same slot markers: nothing kept from before can be trusted. While
// the menu is open the world is not simulated nor blended, so all blend state is simply forgotten.
void Reset() {
    for (int i = 0; i < MaxPed; ++i) { g_ped[i].valid = g_ped[i].applied = false; g_pedHier[i].valid = g_pedHier[i].applied = false; g_pedSaved[i] = false; g_pedPreOk[i] = false; }
    for (int i = 0; i < MaxVeh; ++i) { g_veh[i].valid = g_veh[i].applied = false; g_vehBones[i].n = 0; g_vehBones[i].moved = g_vehBones[i].applied = false; g_vehSaved[i] = false; g_vehPartsEnt[i] = 0; }
    for (int i = 0; i < MaxObj; ++i) g_obj[i].valid = g_obj[i].applied = false;
    ClearCutscene();
    g_cam.valid = g_cam.applied = false;
}

void Restore() {
    DropDead();
    RestorePool(g_ped, MaxPed);
    for (int i = 0; i < MaxPed; ++i) RestoreHier(g_pedHier[i], g_ped[i]);
    for (auto& b : g_vehBones) RestoreBones(b);
    RestorePool(g_veh, MaxVeh);
    RestoreCutscene();
    RestorePool(g_obj, MaxObj);
    if (g_cam.applied) {
        uint8_t* f = CameraFrame();
        if (f) { __try { WritePose(f + 0x10, g_cam.curr); WritePose(f + 0x50, g_cam.curr); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        g_cam.applied = false;
    }
}

}  // namespace Interp

