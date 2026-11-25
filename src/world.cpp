#include "world.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstring>

#include "log.h"
#include "scripthook.h"

namespace world {
namespace {

// GTA IV 1.2.0.59 addresses as RVAs (taken from a dump loaded at 0x700000,
// see MODLOG). GET_GROUND_Z_FOR_3D_COORD -> 0xe962d0 ->
// 0xd523f0 -> vertical probe 0xd54510, which calls the level's segment test.
constexpr uint32_t kProbeRva = 0xa38880 - 0x700000;        // phLevel::TestProbe-like, thiscall, ret 0x20
constexpr uint32_t kVerticalProbeRva = 0xd54510 - 0x700000; // reads the level pointer at +0x60
constexpr uint32_t kLevelPtrRva = 0x15b9c78 - 0x700000;

// Entity pools, each loaded by a `mov ecx, [pool]` we check before trusting it:
// peds in GET_PLAYER_CHAR's helper, vehicles in GET_CAR_CHAR_IS_USING's,
// objects in GET_OBJECT_COORDINATES'.
struct PoolRef {
    Kind kind;
    uint32_t mov_rva;   // address of the `8b 0d <abs32>` instruction
    uint32_t pool_rva;  // the pool pointer it loads
};
const PoolRef kPools[] = {
    {Kind::Ped, 0xeb37b9 - 0x700000, 0x1bb6f1c - 0x700000},
    {Kind::Vehicle, 0xea653c - 0x700000, 0x15e22a4 - 0x700000},
    {Kind::Object, 0xd04664 - 0x700000, 0x1932c60 - 0x700000},
};

// rage pool header; handle = index << 8 | flags[index] (0x7b7020 in the game).
struct Pool {
    uint8_t *objects;
    uint8_t *flags;
    uint32_t count;
    uint32_t entry_size;
};

const uint8_t kProbePrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0xb8, 0xcc, 0x49, 0x00, 0x00};

struct alignas(16) Vec4 {
    float x, y, z, w;
};

struct alignas(16) Segment {
    Vec4 a, b;
};

// rage intersection record, 0x54 bytes (layout matches IV-SDK's tLineOfSightResults).
struct alignas(16) Intersection {
    void *inst;        // 0x00 phInst of what was hit
    uint8_t pad[0xc];  // 0x04
    Vec4 position;     // 0x10
    Vec4 normal_a;     // 0x20
    Vec4 normal_b;     // 0x30
    float t;           // 0x40
    float depth;       // 0x44
    uint32_t unk48;    // 0x48
    uint32_t part;     // 0x4c, 0xffff when unset
    uint8_t material;  // 0x50
    uint8_t pad2[0xf];
};
static_assert(offsetof(Intersection, position) == 0x10, "layout");
static_assert(offsetof(Intersection, t) == 0x40, "layout");
static_assert(offsetof(Intersection, material) == 0x50, "layout");

using ProbeFn = int(__attribute__((thiscall)) *)(void *level, const Segment *seg, Intersection *out, void *exclude,
                                                 uint32_t type_flags, int32_t include_flags, uint32_t unk7,
                                                 uint32_t max_hits, uint32_t unk0);

ProbeFn g_probe = nullptr;
void **g_level = nullptr;
bool g_checked = false;
Pool **g_pools[3] = {};
void *g_exclude = nullptr;         // phInst the probes ignore
void *g_exclude_entity = nullptr;  // and anything else of this entity (peds have more than one inst)
void *g_exclude_entity2 = nullptr; // a second ped (the stand-in at home), every inst of it
bool g_near_now = false, g_near_last = false;  // moving_thing_near()

bool resolve() {
    if (g_checked) return g_probe != nullptr;
    g_checked = true;
    auto base = reinterpret_cast<uint8_t *>(GetModuleHandleA(nullptr));
    uint8_t *probe = base + kProbeRva;
    uint8_t *vprobe = base + kVerticalProbeRva;
    // The vertical probe loads the level with `mov ecx, [abs]` at +0x60.
    uint8_t *mov = vprobe + 0x60;
    uint32_t level_abs = 0;
    std::memcpy(&level_abs, mov + 2, 4);
    if (std::memcmp(probe, kProbePrologue, sizeof kProbePrologue) != 0 || mov[0] != 0x8b || mov[1] != 0x0d ||
        level_abs != uint32_t(reinterpret_cast<uintptr_t>(base + kLevelPtrRva))) {
        fpvlog::write("world: probe signature mismatch (not GTA IV 1.2.0.59?), using ground-only collision");
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        uint8_t *ins = base + kPools[i].mov_rva;
        uint32_t abs = 0;
        std::memcpy(&abs, ins + 2, 4);
        if (ins[0] == 0x8b && ins[1] == 0x0d && abs == uint32_t(reinterpret_cast<uintptr_t>(base + kPools[i].pool_rva)))
            g_pools[i] = reinterpret_cast<Pool **>(base + kPools[i].pool_rva);
        else
            fpvlog::write("world: pool %d signature mismatch, impacts on it won't affect the entity", i);
    }
    g_probe = reinterpret_cast<ProbeFn>(probe);
    g_level = reinterpret_cast<void **>(base + kLevelPtrRva);
    fpvlog::write("world: engine probe at %p, level %p", static_cast<void *>(probe), *g_level);
    return true;
}

int ground_only(const float origin[3], const float dir[3], float max_distance, fpv_raycast_hit *hit) {
    if (dir[2] > -0.5f) return 0;
    float ground = 0;
    sh::call(N("GET_GROUND_Z_FOR_3D_COORD"), origin[0], origin[1], origin[2], &ground);
    if (ground == 0 || ground > origin[2]) return 0;
    float distance = (origin[2] - ground) / -dir[2];
    if (distance > max_distance) return 0;
    hit->distance = distance;
    hit->normal[0] = 0;
    hit->normal[1] = 0;
    hit->normal[2] = 1;
    hit->material = kMaterialGround;
    hit->object_id = 0;
    return 1;
}

// phInst::m_pEntity (+0xC, IV-SDK), or null when the pointer looks wrong.
void *entity_of(void *inst) {
    if (!inst) return nullptr;
    void *entity = *reinterpret_cast<void **>(static_cast<uint8_t *>(inst) + 0xc);
    MEMORY_BASIC_INFORMATION mbi;
    if (!entity || !VirtualQuery(entity, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return nullptr;
    return entity;
}

// Kind and script handle of an entity, packed as an fpv object_id.
uint32_t classify(void *entity) {
    if (!entity) return 0;
    auto p = static_cast<uint8_t *>(entity);
    for (int i = 0; i < 3; ++i) {
        Pool *pool = g_pools[i] ? *g_pools[i] : nullptr;
        if (!pool || !pool->entry_size || p < pool->objects || p >= pool->objects + pool->count * pool->entry_size)
            continue;
        uint32_t off = uint32_t(p - pool->objects);
        if (off % pool->entry_size) continue;
        uint32_t index = off / pool->entry_size;
        uint8_t flag = pool->flags[index];
        if (flag & 0x80) return 0;  // free slot
        return uint32_t(kPools[i].kind) << 28 | (index << 8 | flag);
    }
    return uint32_t(Kind::Other) << 28;
}

// Entity behind a script handle, or null (the pool's handle -> pointer, 0x799d20).
void *entity_from_handle(Kind kind, uint32_t handle) {
    int i = int(kind) - 1;
    Pool *pool = (i >= 0 && i < 3 && g_pools[i]) ? *g_pools[i] : nullptr;
    uint32_t index = handle >> 8;
    if (!pool || index >= pool->count || pool->flags[index] != (handle & 0xff)) return nullptr;
    return pool->objects + index * pool->entry_size;
}

bool usable_normal(const Vec4 &n, const float dir[3]) {
    float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
    return len2 > 0.81f && len2 < 1.21f && n.x * dir[0] + n.y * dir[1] + n.z * dir[2] < 0;
}

}  // namespace

int raycast(void *, const float origin[3], const float dir[3], float max_distance, fpv_raycast_hit *hit) {
    if (!resolve() || !*g_level) return ground_only(origin, dir, max_distance, hit);

    Segment seg{};
    seg.a = {origin[0], origin[1], origin[2], 0};
    seg.b = {origin[0] + dir[0] * max_distance, origin[1] + dir[1] * max_distance,
             origin[2] + dir[2] * max_distance, 0};
    Intersection is{};
    void *entity = nullptr;
    for (int attempt = 0;; ++attempt) {
        is = Intersection{};
        is.part = 0xffff;
        // Same trailing arguments as the game's own ground probe; types: static,
        // buildings, vehicles, peds, objects.
        if (!g_probe(*g_level, &seg, &is, g_exclude, kTypeAll, -1, 7, 1, 0)) return 0;
        entity = entity_of(is.inst);
        bool ignored = entity && (entity == g_exclude_entity || entity == g_exclude_entity2);
        if (!ignored || attempt == 8) break;
        // Hit an ignored ped (through another of its instances): continue past it.
        seg.a = {is.position.x + dir[0] * 0.25f, is.position.y + dir[1] * 0.25f, is.position.z + dir[2] * 0.25f, 0};
    }
    if (entity && (entity == g_exclude_entity || entity == g_exclude_entity2)) return 0;

    float dx = is.position.x - origin[0], dy = is.position.y - origin[1], dz = is.position.z - origin[2];
    float distance = dx * dir[0] + dy * dir[1] + dz * dir[2];
    if (!std::isfinite(distance)) {  // a hit without a position: libfpv would turn it into NaN wind
        static int reported = 0;
        if (reported++ < 5)
            fpvlog::write("world: probe hit with no usable position (%g %g %g), ignored", is.position.x,
                          is.position.y, is.position.z);
        return 0;
    }
    if (distance < 0 || distance > max_distance) return 0;

    const Vec4 &n = usable_normal(is.normal_a, dir) ? is.normal_a : is.normal_b;
    uint32_t object_id = classify(entity);
    static void *last_entity = nullptr;
    static int traced = 0;
    if (traced < 10 || (entity != last_entity && traced < 60)) {
        ++traced;
        last_entity = entity;
        fpvlog::write("world: hit d %.2f entity %p kind %u handle %u pos %.2f %.2f %.2f n %.2f %.2f %.2f", distance,
                      entity, uint32_t(kind_of(object_id)), handle_of(object_id), is.position.x, is.position.y,
                      is.position.z, n.x, n.y, n.z);
    }
    if (!usable_normal(n, dir)) {
        // No trustworthy normal: treat it as facing straight back at the ray.
        hit->normal[0] = -dir[0];
        hit->normal[1] = -dir[1];
        hit->normal[2] = -dir[2];
    } else {
        hit->normal[0] = n.x;
        hit->normal[1] = n.y;
        hit->normal[2] = n.z;
    }
    hit->distance = distance;
    if (distance < 3.0f && (kind_of(object_id) == Kind::Vehicle || kind_of(object_id) == Kind::Ped))
        g_near_now = true;
    hit->material = kind_of(object_id) == Kind::Vehicle ? kMaterialVehicle : kMaterialGround;
    hit->object_id = object_id;
    return 1;
}

namespace {

// Poses handed out this tick, by object_id: libfpv wants the same one for
// every ray that hits the same thing in a tick.
struct Pose {
    uint32_t id;
    float pos[3], rot[4];
};
std::vector<Pose> g_poses;

// The pose of a vehicle, ped or object as the game has it now. Only the
// heading goes into the rotation: its convention is certain (degrees,
// counter-clockwise about +Z), and turning is what carries a perched drone.
bool pose_of(uint32_t object_id, float pos[3], float rot[4]) {
    uint32_t h = handle_of(object_id);
    float heading = 0;
    switch (kind_of(object_id)) {
        case Kind::Vehicle:
            if (!h || !sh::call<bool>(N("DOES_VEHICLE_EXIST"), h)) return false;
            sh::call(N("GET_CAR_COORDINATES"), h, &pos[0], &pos[1], &pos[2]);
            sh::call(N("GET_CAR_HEADING"), h, &heading);
            break;
        case Kind::Ped:
            if (!h || !sh::call<bool>(N("DOES_CHAR_EXIST"), h)) return false;
            sh::call(N("GET_CHAR_COORDINATES"), h, &pos[0], &pos[1], &pos[2]);
            sh::call(N("GET_CHAR_HEADING"), h, &heading);
            break;
        case Kind::Object:
            if (!h || !sh::call<bool>(N("DOES_OBJECT_EXIST"), h)) return false;
            sh::call(N("GET_OBJECT_COORDINATES"), h, &pos[0], &pos[1], &pos[2]);
            sh::call(N("GET_OBJECT_HEADING"), h, &heading);
            break;
        default:
            return false;
    }
    float a = heading * 0.5f / 57.2957795f;
    rot[0] = 0, rot[1] = 0, rot[2] = std::sin(a), rot[3] = std::cos(a);
    return true;
}

}  // namespace

void new_tick() {
    g_poses.clear();
    g_near_last = g_near_now;
    g_near_now = false;
}

bool moving_thing_near() { return g_near_last || g_near_now; }

int raycast_ex(void *user, const float origin[3], const float dir[3], float max_distance, fpv_raycast_hit_ex *hit) {
    // libfpv sets struct_size: write no field past it
    if (!raycast(user, origin, dir, max_distance, &hit->hit)) return 0;
    if (hit->struct_size < offsetof(fpv_raycast_hit_ex, object_rotation) + sizeof hit->object_rotation) return 1;
    hit->flags = 0;
    uint32_t id = hit->hit.object_id;
    if (!handle_of(id)) return 1;
    for (const Pose &p : g_poses)
        if (p.id == id) {
            std::memcpy(hit->object_position, p.pos, sizeof p.pos);
            std::memcpy(hit->object_rotation, p.rot, sizeof p.rot);
            hit->flags = FPV_RAYCAST_HIT_POSE;
            return 1;
        }
    Pose p{id, {}, {}};
    if (!pose_of(id, p.pos, p.rot)) return 1;
    g_poses.push_back(p);
    std::memcpy(hit->object_position, p.pos, sizeof p.pos);
    std::memcpy(hit->object_rotation, p.rot, sizeof p.rot);
    hit->flags = FPV_RAYCAST_HIT_POSE;
    return 1;
}

std::vector<uint32_t> vehicles() {
    std::vector<uint32_t> out;
    if (!resolve()) return out;
    Pool *pool = g_pools[1] ? *g_pools[1] : nullptr;  // kPools[1]: vehicles
    if (!pool || !pool->flags) return out;
    for (uint32_t i = 0; i < pool->count; ++i) {
        uint8_t flag = pool->flags[i];
        if (flag & 0x80) continue;  // free slot
        out.push_back(i << 8 | flag);
    }
    return out;
}

void *ped_entity(uint32_t ped_handle) {
    return ped_handle && resolve() ? entity_from_handle(Kind::Ped, ped_handle) : nullptr;
}

void ignore_second_ped(uint32_t ped_handle) {
    g_exclude_entity2 = ped_handle && resolve() ? entity_from_handle(Kind::Ped, ped_handle) : nullptr;
    if (ped_handle) fpvlog::write("world: probes also ignore ped %u (entity %p)", ped_handle, g_exclude_entity2);
}

void ignore_ped(uint32_t ped_handle) {
    g_exclude = g_exclude_entity = nullptr;
    if (!ped_handle || !resolve()) return;
    auto entity = static_cast<uint8_t *>(entity_from_handle(Kind::Ped, ped_handle));
    // CEntity+0x38 = phInst (IV-SDK); trust it only if the instance points back.
    void *inst = entity ? *reinterpret_cast<void **>(entity + 0x38) : nullptr;
    g_exclude_entity = entity;
    if (inst && entity_of(inst) == entity) {
        g_exclude = inst;
        fpvlog::write("world: probes ignore ped %u (entity %p inst %p)", ped_handle, static_cast<void *>(entity), inst);
    } else {
        fpvlog::write("world: cannot resolve ped %u's phInst (entity %p inst %p)", ped_handle,
                      static_cast<void *>(entity), inst);
    }
}

}  // namespace world
