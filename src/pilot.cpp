#include "pilot.h"

#include "log.h"
#include "scripthook.h"

namespace pilot {
namespace {

// GTA IV has no anim of anyone holding an RC transmitter. Niko reading a
// text (head down, the phone held at the chest in both hands' reach) looks
// like a pilot watching the goggles' feed. Both names were checked in
// pc/anim/anim.img (cellphone.wad has cellphone_text).
const char *const kSet = "cellphone";
const char *const kAnim = "cellphone_text";
constexpr float kBlend = 4.0f;  // TASK_PLAY_ANIM speed: a soft blend in

uint32_t g_ped = 0;      // the posed ped
int g_retry = 0;         // ticks until the anim is started again if the game dropped it
float g_pending = -1;    // phase to set once the anim plays (hold_at)
bool g_reported = false;

bool ready() {
    if (sh::call<bool>(N("HAVE_ANIMS_LOADED"), kSet)) return true;
    sh::call(N("REQUEST_ANIMS"), kSet);
    return false;
}

bool playing(uint32_t ped) {
    return ped && sh::call<bool>(N("DOES_CHAR_EXIST"), ped) && sh::call<bool>(N("IS_CHAR_PLAYING_ANIM"), ped, kSet, kAnim);
}

void start(uint32_t ped, float speed) {
    // ped, anim, set, blend speed, loop, extract back / side velocity, hold last frame, time (-1 = forever)
    sh::call(N("TASK_PLAY_ANIM"), ped, kAnim, kSet, speed, 1u, 0u, 0u, 0u, -1);
    g_retry = 30;
}

void stop(uint32_t ped) {
    if (ped && sh::call<bool>(N("DOES_CHAR_EXIST"), ped) && playing(ped)) sh::call(N("CLEAR_CHAR_TASKS"), ped);
}

}  // namespace

void hold(uint32_t ped, bool on) {
    if (!on) {
        if (g_ped && ped == g_ped) {
            stop(g_ped);
            fpvlog::write("pilot: pose off (ped %u)", g_ped);
            g_ped = 0;
        }
        return;
    }
    if (!ped || !sh::call<bool>(N("DOES_CHAR_EXIST"), ped)) return;
    if (g_ped && g_ped != ped) {
        stop(g_ped);
        g_ped = 0;
    }
    if (!ready()) return;
    if (g_ped != ped) {
        g_ped = ped;
        g_pending = -1;
        if (playing(ped)) {  // still posing (Niko kept it, hidden at the drone): carry on, no restart
            g_retry = 30;
            return;
        }
        start(ped, kBlend);
        fpvlog::write("pilot: pose on (ped %u, %s/%s)", ped, kSet, kAnim);
        return;
    }
    if (playing(ped)) {
        if (g_pending >= 0) {
            sh::call(N("SET_CHAR_ANIM_CURRENT_TIME"), ped, kSet, kAnim, g_pending);
            g_pending = -1;
        }
        if (!g_reported) {
            g_reported = true;
            fpvlog::write("pilot: the pose anim plays");
        }
        return;
    }
    if (--g_retry <= 0) {  // dropped by the game (a cutscene, a shove): again
        static int restarts = 0;
        if (restarts++ < 5) fpvlog::write("pilot: pose not playing on ped %u, started again", ped);
        start(ped, kBlend);
    }
}

float phase(uint32_t ped) {
    if (!playing(ped)) return -1;
    float t = -1;
    sh::call(N("GET_CHAR_ANIM_CURRENT_TIME"), ped, kSet, kAnim, &t);
    return t;
}

void hold_at(uint32_t ped, float at) {
    if (!ped || !ready()) return;
    if (g_ped && g_ped != ped) g_ped = 0;  // the other ped is about to go (invisible or deleted)
    g_ped = ped;
    if (playing(ped)) {  // already in the pose: only move it to the other ped's point in it
        sh::call(N("SET_CHAR_ANIM_CURRENT_TIME"), ped, kSet, kAnim, at);
        g_pending = -1;
        g_retry = 30;
        return;
    }
    start(ped, 8.0f);  // a quick blend: the phase is set once it plays
    g_pending = at;
}

void forget(uint32_t ped) {
    if (ped == g_ped) g_ped = 0;
}

}  // namespace pilot
