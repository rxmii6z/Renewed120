#include <windows.h>
#include <stdint.h>
#include <string.h>

#define HUD_CALL 0x53E298u
#define SCRIPT_CALL 0x46A21Bu
#define ACTIVE_SCRIPTS 0xA8B42Cu
#define PAUSE_FLAGS 0xB7CB48u
#define UI_RESUME_OFFSET 0x8F1u
#define SCRIPT_LOCALS 0x3Cu
#define UI_VISIBLE 3u
#define UI_PLAYER 5u
#define UI_WAS_OPEN 11u

typedef struct Script {
    struct Script *next;
    struct Script *previous;
    char name[8];
    const BYTE *base;
    const BYTE *ip;
} Script;

typedef void (__cdecl *HudFunction)(void);
typedef void (__fastcall *ScriptFunction)(Script *, void *);
typedef struct Wheel {
    Script *script;
    BOOL processed;
} Wheel;

static HudFunction draw_hud;
static ScriptFunction process_script;
static Wheel wheels[2];

static uintptr_t call_target(uintptr_t site)
{
    int32_t displacement;
    if (*(const BYTE *)site != 0xE8)
        return 0;
    memcpy(&displacement, (const void *)(site + 1), sizeof displacement);
    return site + 5 + displacement;
}

static BOOL replace_call(uintptr_t site, uintptr_t target)
{
    DWORD original_protection;
    DWORD ignored;
    uint32_t displacement = target - (site + 5);
    if (!VirtualProtect((void *)(site + 1), sizeof displacement,
                        PAGE_EXECUTE_READWRITE, &original_protection))
        return FALSE;
    memcpy((void *)(site + 1), &displacement, sizeof displacement);
    FlushInstructionCache(GetCurrentProcess(), (void *)site, 5);
    VirtualProtect((void *)(site + 1), sizeof displacement, original_protection, &ignored);
    return TRUE;
}

static BOOL is_wheel(const Script *script)
{
    static const BYTE signature[] = {0x01, 0x00, 0x04, 0x00, 0x85, 0x00,
                                     0x03, 0x00, 0x00, 0x03, 0x05, 0x00};
    return _strnicmp(script->name, "vwwui", 6) == 0 &&
           script->base && script->ip == script->base + UI_RESUME_OFFSET &&
           memcmp(script->ip - 4, signature, sizeof signature) == 0;
}

static uint32_t read_script_word(const Script *script, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const BYTE *)script + offset, sizeof value);
    return value;
}

static void __fastcall track_script(Script *script, void *unused)
{
    BOOL wheel_thread = _strnicmp(script->name, "vwwui", 6) == 0;
    process_script(script, unused);
    if (wheel_thread) {
        Script *active = *(Script **)ACTIVE_SCRIPTS;
        for (; active; active = active->next) {
            if (active == script && is_wheel(active)) {
                unsigned player = read_script_word(active, SCRIPT_LOCALS + UI_PLAYER * 4);
                if (player < 2) {
                    wheels[player].script = active;
                    wheels[player].processed = TRUE;
                }
                break;
            }
        }
    }
}

static BOOL install_script_hook(void)
{
    MEMORY_BASIC_INFORMATION target_memory;
    HMODULE cleo = GetModuleHandleA("CLEO.asi");
    uintptr_t target = call_target(SCRIPT_CALL);
    if (!cleo || !target ||
        !VirtualQuery((const void *)target, &target_memory, sizeof target_memory) ||
        target_memory.AllocationBase != (void *)cleo)
        return FALSE;

    process_script = (ScriptFunction)target;
    if (replace_call(SCRIPT_CALL, (uintptr_t)track_script))
        return TRUE;
    process_script = NULL;
    return FALSE;
}

static void replay_ui(Script *script)
{
    // Repeat the rendering pass with the last simulation state. Keep VM changes
    // such as animation progress and wake time on the simulation clock.
    BYTE saved[0xE0 - 0x14];
    BYTE *state = (BYTE *)script;
    uint32_t wake_now = 0;
    memcpy(saved, state + 0x14, sizeof saved);
    memcpy(state + 0xCC, &wake_now, sizeof wake_now);
    process_script(script, NULL);
    memcpy(state + 0x14, saved, sizeof saved);
}

static void __cdecl draw_hud_with_wheel(void)
{
    if (!process_script) {
        install_script_hook();
    } else if (!*(const uint16_t *)PAUSE_FLAGS &&
               ((wheels[0].script && !wheels[0].processed) ||
                (wheels[1].script && !wheels[1].processed))) {
        Script *script = *(Script **)ACTIVE_SCRIPTS;
        while (script) {
            Script *next = script->next;
            unsigned player;
            // Check membership in the live list before using retained pointers.
            for (player = 0; player < 2; ++player) {
                if (wheels[player].script == script && !wheels[player].processed &&
                    is_wheel(script) &&
                    read_script_word(script, SCRIPT_LOCALS + UI_VISIBLE * 4) == 1 &&
                    read_script_word(script, SCRIPT_LOCALS + UI_WAS_OPEN * 4) == 1) {
                    replay_ui(script);
                    break;
                }
            }
            script = next;
        }
    }
    wheels[0].processed = wheels[1].processed = FALSE;
    draw_hud();
}

void InstallWheelHudReplay()
{
    static const BYTE hud_suffix[] = {0xA0, 0x65, 0xF0, 0xB6, 0x00};
    static const BYTE script_suffix[] = {0x3B, 0xFB, 0x8B, 0xF7};
    uintptr_t original;

    // GTA San Andreas PC 1.0. Keep any already-installed HUD wrapper in the chain.
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000 ||
        memcmp((const void *)(HUD_CALL + 5), hud_suffix, sizeof hud_suffix) ||
        memcmp((const void *)(SCRIPT_CALL + 5), script_suffix, sizeof script_suffix))
        return;
    original = call_target(HUD_CALL);
    if (original) {
        draw_hud = (HudFunction)original;
        replace_call(HUD_CALL, (uintptr_t)draw_hud_with_wheel);
    }
    return;
}
