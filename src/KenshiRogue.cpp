// Kenshi Rogue: a roguelite run through Kenshi's combat.
//
// A run is a sequence of encounters that start small (one starving bandit)
// and grow with the squad. Between fights: loot drawn from Kenshi's own item
// grades, rest, recruits. Limb loss carries through the run.
// See docs/design.md.
//
// This file is the scaffold: it proves the plugin loads and the main loop
// hook ticks. Everything is logged to RE_Kenshi_log.txt, prefixed
// "KenshiRogue:".

#include <Debug.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>

#include <core/Functions.h>

namespace
{
    void (*mainLoop_orig)(GameWorld*, float) = NULL;
    bool g_saidHello = false;

    void mainLoop_hook(GameWorld* world, float time)
    {
        mainLoop_orig(world, time);
        if (!g_saidHello)
        {
            g_saidHello = true;
            DebugLog("KenshiRogue: main loop is ticking");
        }
    }
}

__declspec(dllexport) void startPlugin()
{
    DebugLog("KenshiRogue: loading");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            mainLoop_hook, &mainLoop_orig))
    {
        ErrorLog("KenshiRogue: could not hook the main loop");
        return;
    }
    DebugLog("KenshiRogue: ready");
}
