// libfpv.asi: a thin loader around libfpv/libfpv_gtaiv.dll. It owns the
// ScriptHook thread and the log, and reloads the core when a new build is
// deployed (the file's timestamp changes), so the game needn't restart.
// The core is loaded from a copy so the build can overwrite the original.
// It also owns ScriptHook's D3D hook and forwards each frame to the core.
#include <windows.h>

#include <d3d9.h>

#include <cstring>
#include <string>

#include "log.h"
#include "paths.h"
#include "scripthook.h"

namespace {

std::string core_path() { return paths::mod_file("libfpv_gtaiv.dll"); }
std::string live_copy_path() { return paths::mod_file("libfpv_gtaiv.live.dll"); }

// Set when an older install (GTAIV.FPV.asi) is loaded too: that one runs this
// session, this one stays idle so the two don't both fly a drone.
bool g_idle = false;

using CoreFn = void (*)();
using CoreRenderFn = void (*)(IDirect3DDevice9 *);

HMODULE g_core = nullptr;
CoreFn g_core_tick = nullptr;
CoreFn g_core_unload = nullptr;
CoreRenderFn g_core_render = nullptr;
CoreFn g_core_release_d3d = nullptr;
FILETIME g_core_time{};

// The render thread holds this shared while it calls into the core; a reload
// takes it exclusively, so the core is never unloaded mid-frame.
SRWLOCK g_core_lock = SRWLOCK_INIT;
// Before unloading, the core's D3D resources are released on the render thread.
volatile LONG g_release_request = 0;
volatile LONG g_render_calls = 0;  // OnRender count, to notice a dropped hook
HANDLE g_released = CreateEventA(nullptr, TRUE, FALSE, nullptr);

// ScriptHook's ID3DDeviceHook, built by hand (MSVC thiscall vtable):
// OnCreateDevice, OnResetDevice, OnLostDevice, OnDestroyDevice, OnRender.
void __attribute__((thiscall)) on_create(void *, IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *) {}
void __attribute__((thiscall)) on_reset(void *, IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *) {}

void __attribute__((thiscall)) on_lost(void *, IDirect3DDevice9 *) {
    AcquireSRWLockShared(&g_core_lock);
    if (g_core_release_d3d) g_core_release_d3d();
    ReleaseSRWLockShared(&g_core_lock);
}

void __attribute__((thiscall)) on_render(void *, IDirect3DDevice9 *dev) {
    InterlockedIncrement(&g_render_calls);
    AcquireSRWLockShared(&g_core_lock);
    if (InterlockedExchange(&g_release_request, 0)) {
        if (g_core_release_d3d) g_core_release_d3d();
        SetEvent(g_released);
    } else if (g_core_render) {
        g_core_render(dev);
    }
    ReleaseSRWLockShared(&g_core_lock);
}

void *g_hook_vtable[5] = {
    reinterpret_cast<void *>(&on_create), reinterpret_cast<void *>(&on_reset), reinterpret_cast<void *>(&on_lost),
    reinterpret_cast<void *>(&on_lost),  // OnDestroyDevice: same cleanup
    reinterpret_cast<void *>(&on_render),
};
struct {
    void **vtable;
} g_hook = {g_hook_vtable};

void install_d3d_hook() {
    using RequestFn = void *(__cdecl *)(const char *);
    auto request = reinterpret_cast<RequestFn>(reinterpret_cast<void *>(GetProcAddress(
        GetModuleHandleA("ScriptHook.dll"), "?RequestService@ScriptHookManager@@SAPAVIService@@PBD@Z")));
    void *service = request ? request("D3DHook") : nullptr;
    if (!service) {
        fpvlog::write("loader: ScriptHook D3DHook service unavailable, no OSD/overlay");
        return;
    }
    // IService: dtor, GetServiceName, GetDependencies, OnServiceStart, OnServiceStop;
    // ID3DHookService: [5] AddDeviceHook, [6] RemoveDeviceHook.
    using AddHookFn = void(__attribute__((thiscall)) *)(void *, void *);
    auto add = reinterpret_cast<AddHookFn>((*static_cast<void ***>(service))[5]);
    add(service, &g_hook);
    fpvlog::write("loader: D3D hook installed");
}

bool core_changed() {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(core_path().c_str(), GetFileExInfoStandard, &data)) return false;
    if (CompareFileTime(&data.ftLastWriteTime, &g_core_time) == 0) return false;
    g_core_time = data.ftLastWriteTime;
    return true;
}

void unload_core() {
    if (!g_core) return;
    if (g_core_release_d3d) {  // let the render thread free the core's textures
        ResetEvent(g_released);
        InterlockedExchange(&g_release_request, 1);
        if (WaitForSingleObject(g_released, 500) != WAIT_OBJECT_0) fpvlog::write("loader: render thread idle, D3D release skipped");
    }
    AcquireSRWLockExclusive(&g_core_lock);
    if (g_core_unload) g_core_unload();
    FreeLibrary(g_core);
    g_core = nullptr;
    g_core_tick = g_core_unload = g_core_release_d3d = nullptr;
    g_core_render = nullptr;
    ReleaseSRWLockExclusive(&g_core_lock);
}

void load_core() {
    unload_core();
    std::string core = core_path(), live = live_copy_path();
    if (!CopyFileA(core.c_str(), live.c_str(), FALSE)) {
        fpvlog::write("loader: cannot copy %s (error %lu)", core.c_str(), GetLastError());
        return;
    }
    g_core = LoadLibraryA(live.c_str());
    if (!g_core) {
        fpvlog::write("loader: cannot load %s (error %lu)", live.c_str(), GetLastError());
        return;
    }
    auto get = [](const char *name) { return reinterpret_cast<void *>(GetProcAddress(g_core, name)); };
    AcquireSRWLockExclusive(&g_core_lock);
    g_core_tick = reinterpret_cast<CoreFn>(get("libfpv_gtaiv_tick"));
    g_core_unload = reinterpret_cast<CoreFn>(get("libfpv_gtaiv_unload"));
    g_core_render = reinterpret_cast<CoreRenderFn>(get("libfpv_gtaiv_render"));
    g_core_release_d3d = reinterpret_cast<CoreFn>(get("libfpv_gtaiv_release_d3d"));
    ReleaseSRWLockExclusive(&g_core_lock);
    fpvlog::write("loader: core loaded (tick %p)", reinterpret_cast<void *>(g_core_tick));
}

// Loading a save restarts ScriptHook's threads and services, and our D3D
// hook stops being called (no OSD, lens or overlay). After a restart, add it
// again once a second of ticks went by without a single OnRender; checking
// first avoids registering it twice if it survived.
void keep_d3d_hook() {
    static bool hooked = false;
    static unsigned kills = 0;
    static int wait = -1;
    static LONG renders = 0;
    if (!hooked) {  // services are up once scripts tick
        hooked = true;
        kills = sh::kill_count();
        install_d3d_hook();
        return;
    }
    if (sh::kill_count() != kills) {
        kills = sh::kill_count();
        wait = 60;
        renders = g_render_calls;
        fpvlog::write("loader: script threads restarted (save loaded?), watching the D3D hook");
    }
    if (wait < 0 || --wait > 0) return;
    wait = -1;
    if (g_render_calls != renders) {
        fpvlog::write("loader: D3D hook still called after the restart");
        return;
    }
    fpvlog::write("loader: D3D hook dropped by the restart, adding it again");
    install_d3d_hook();
}

// Versions before the libfpv naming were GTAIV.FPV.asi, GTAIV.FPV.core.dll,
// GTAIV.FPV.log and libfpv.dll in the game folder. Move them into
// libfpv\old so the next start loads only this one (a loaded DLL can be
// renamed, not deleted). The settings in fpv-data are left for the core,
// which copies them into libfpv's shared folder.
void retire_old_install() {
    std::string game = paths::game_dir(), old_dir = paths::mod_file("old");
    const char *const kOld[] = {"GTAIV.FPV.asi", "GTAIV.FPV.core.dll", "GTAIV.FPV.core.live.dll", "GTAIV.FPV.log",
                                "libfpv.dll", "libfpv.LICENSE.txt"};
    for (const char *name : kOld) {
        std::string from = game + "\\" + name;
        if (GetFileAttributesA(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        CreateDirectoryA(old_dir.c_str(), nullptr);
        std::string to = old_dir + "\\" + name;
        if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
            fpvlog::write("loader: moved the old %s to libfpv\\old (delete that folder when you like)", name);
        else
            fpvlog::write("loader: cannot move the old %s (error %lu): delete it by hand", name, GetLastError());
    }
}

void tick() {
    static bool first = true;
    if (first) {  // every .asi is loaded by now
        first = false;
        g_idle = GetModuleHandleA("GTAIV.FPV.asi") != nullptr;
        retire_old_install();
        if (g_idle)
            fpvlog::write("loader: an older GTA IV FPV (GTAIV.FPV.asi) is loaded too and runs this session; "
                          "it was moved away, so the next start runs this version");
    }
    if (g_idle) return;
    keep_d3d_hook();
    static int poll = 0;
    if (poll++ % 30 == 0 && core_changed()) load_core();
    if (g_core_tick) g_core_tick();
}

// A crash's address in the log: which module and offset faulted, and the
// return addresses on the stack that point into a module. First-chance, so
// it sees the fault before the game's own handler; logs each address once.
const char *module_of(uintptr_t addr, uintptr_t *offset) {
    HMODULE m = nullptr;
    static char name[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &m) ||
        !m)
        return nullptr;
    DWORD n = GetModuleFileNameA(m, name, sizeof name);
    *offset = addr - reinterpret_cast<uintptr_t>(m);
    const char *base = name;
    for (DWORD i = 0; i < n; ++i)
        if (name[i] == '\\' || name[i] == '/') base = name + i + 1;
    return base;
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    static uintptr_t seen[32];
    static int count = 0;
    uintptr_t at = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    for (int i = 0; i < count; ++i)
        if (seen[i] == at) return EXCEPTION_CONTINUE_SEARCH;
    if (count < 32) seen[count++] = at;
    uintptr_t off = 0;
    const char *mod = module_of(at, &off);
    fpvlog::write("crash? exception %08lx at %p (%s+0x%x), %s %p, thread %lu", code, reinterpret_cast<void *>(at),
                  mod ? mod : "?", unsigned(off), e->ExceptionRecord->NumberParameters > 1
                      ? (e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading") : "",
                  e->ExceptionRecord->NumberParameters > 1
                      ? reinterpret_cast<void *>(e->ExceptionRecord->ExceptionInformation[1]) : nullptr,
                  GetCurrentThreadId());
    const CONTEXT *c = e->ContextRecord;
    fpvlog::write("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx,
                  c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    if (mod && std::strcmp(mod, "GTAIV.exe") == 0) {  // the game's own: the raw stack, for the objects on it
        const uint32_t *raw = reinterpret_cast<const uint32_t *>(c->Esp);
        for (int row = 0; row < 32; ++row) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(raw + row * 8, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            fpvlog::write("  [esp+%03x] %08x %08x %08x %08x %08x %08x %08x %08x", row * 32, raw[row * 8],
                          raw[row * 8 + 1], raw[row * 8 + 2], raw[row * 8 + 3], raw[row * 8 + 4], raw[row * 8 + 5],
                          raw[row * 8 + 6], raw[row * 8 + 7]);
        }
    }
    const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(e->ContextRecord->Esp);
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
        uintptr_t v = sp[i], o = 0;
        if (v < 0x10000) continue;
        if (!VirtualQuery(reinterpret_cast<void *>(v), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            continue;
        if (const char *m = module_of(v, &o)) {
            fpvlog::write("  stack +%03x: %s+0x%x", i * 4, m, unsigned(o));
            ++shown;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// libfpv's log callback lives here, not in the core, because libfpv keeps
// calling it from its own threads across core reloads.
extern "C" __declspec(dllexport) void libfpv_gtaiv_log(void *, int level, const char *message) {
    static const char *names[] = {"debug", "info", "warn", "error"};
    fpvlog::write("libfpv %s: %s", names[level & 3], message);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        fpvlog::write("libfpv.asi (GTA IV FPV) attached");
        AddVectoredExceptionHandler(1, &on_exception);
        sh::register_thread(&tick);
    }
    return TRUE;
}
