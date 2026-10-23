// mods: optional cheats for LEGO Ninjago: Tournament. Only built into the
// "mods" variant of libGLESv2.so (prebuilt/armeabi-v7a-mods/, used by
// `prepare.py --mods`).
//
// TT left their developer cheat switches in the game: g_CheatOptions, three
// bytes that the unlock checks and SaveGame_DoCheats read. Turning them on is
// how TT's testers unlocked everything, so nothing in the game's code is
// patched. On top of that, studs and tokens are topped up on the game's own
// thread (from glClear, which runs every frame), and TT's
// SaveGame_CompleteEverything runs once per launch.
//
// Everything is looked up by symbol name; anything missing is skipped.

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>

#define DBG_FILE "/sdcard/Android/data/com.lego.ninjago.toe/files/mods.log"
#include "dbglog.h"

#define EXPORT __attribute__((visibility("default")))
#define TAG "mods"
#define LOGI(...) dbg_log(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) dbg_log(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

// g_CheatOptions bits, as read by the game (see docs/HOW-IT-WORKS.md).
#define CHEAT0_ALL_CHARACTERS 0x08   // IsCharUnlocked / IsCharBought
#define CHEAT0_ALL_SUPER_MOVES 0x10  // IsCharacterSuperMoveBought
#define CHEAT0_ALL_LEVELS 0x20       // UIFrontEnd_IsLevelUnlocked
#define CHEAT0_ALL_EXTRAS 0x40       // IsExtraUnlocked / IsExtraBought, extras menu
#define CHEAT1_STUDS 0x04            // SaveGame_DoCheats: studs = 10,000,000
#define CHEAT1_TOKENS 0x08           // SaveGame_DoCheats: tokens = 10,000,000
#define CHEAT1_ALL_RED_BRICKS 0x40   // red brick screens

// SAVEGAMEDATA fields written by SaveGame_DoCheats.
#define SAVE_STUDS 0x78    // uint64
#define SAVE_TOKENS 0x4a8  // uint32
#define TOP_UP_BELOW 5000000
#define TOP_UP_TO 10000000

#define COMPLETE_AFTER_FRAMES 600  // ~10 s: the save is loaded by then

static uint8_t* g_cheats;
static uint8_t* g_save;
static void (*g_completeEverything)(void);
static void (*real_glClear)(GLbitfield);
static unsigned g_frames;
static int g_completed;

static void set_cheats(void) {
    g_cheats[0] |= CHEAT0_ALL_CHARACTERS | CHEAT0_ALL_SUPER_MOVES | CHEAT0_ALL_LEVELS | CHEAT0_ALL_EXTRAS;
    g_cheats[1] |= CHEAT1_STUDS | CHEAT1_TOKENS | CHEAT1_ALL_RED_BRICKS;
}

// Runs from THUMB's constructor pass: the game is loaded and relocated, but
// none of its code has run yet.
__attribute__((constructor)) static void mods_init(void) {
    g_cheats = dlsym(RTLD_DEFAULT, "g_CheatOptions");
    g_save = dlsym(RTLD_DEFAULT, "SaveGame_Data");
    g_completeEverything = (void (*)(void))dlsym(RTLD_DEFAULT, "_Z27SaveGame_CompleteEverythingv");
    if (g_cheats) set_cheats();
    LOGI("cheats %s, studs/tokens top-up %s, complete everything %s", g_cheats ? "on" : "NOT FOUND",
         g_save ? "on" : "NOT FOUND", g_completeEverything ? "on" : "NOT FOUND");
}

static void mods_tick(void) {
    if (++g_frames % 30) return;
    if (g_cheats) set_cheats();
    if (g_save) {
        uint64_t* studs = (uint64_t*)(g_save + SAVE_STUDS);
        uint32_t* tokens = (uint32_t*)(g_save + SAVE_TOKENS);
        if (*studs < TOP_UP_BELOW) *studs = TOP_UP_TO;
        if (*tokens < TOP_UP_BELOW) *tokens = TOP_UP_TO;
    }
    if (g_completeEverything && !g_completed && g_frames >= COMPLETE_AFTER_FRAMES) {
        g_completed = 1;
        g_completeEverything();
        LOGI("SaveGame_CompleteEverything done");
    }
}

// The engine clears the screen every frame on its own thread, which makes
// this a safe place to touch game state.
EXPORT void glClear(GLbitfield mask) {
    if (!real_glClear) real_glClear = (void (*)(GLbitfield))eglGetProcAddress("glClear");
    real_glClear(mask);
    mods_tick();
}
