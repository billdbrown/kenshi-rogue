// Kenshi Rogue: a roguelite run through Kenshi's combat.
//
// A run is a sequence of nodes: fights that start small (one starving bandit)
// and grow, with rests between. Loot is whatever Kenshi puts on the enemies,
// plus what an encounter adds. Limb loss carries through the run (decided
// 2026-11-23): rests heal wounds, never a lost limb. See docs/design.md.
//
// The first slice: one fixed-stat runner, created at an arena beside your
// first character; fight 1 (one starving bandit carrying a first aid kit),
// a rest, fight 2 (two starving bandits). A "Rogue" button on the HUD starts
// the run and moves it on.
//
// Everything is logged to RE_Kenshi_log.txt, prefixed "KenshiRogue:". A
// scenario in mods/KenshiRogue/autotest/scenario.txt drives it unattended
// (scripts/autotest.py; commands in scripts/autotest/README.md).

#include <Debug.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/Item.h>
#include <kenshi/CharStats.h>
#include <kenshi/Inventory.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/CharMovement.h>
#include <kenshi/SaveManager.h>
#include <kenshi/Enums.h>
#include <kenshi/SharedKing.h>
#include <kenshi/Town.h>
#include <kenshi/Building/Building.h>
#include <kenshi/CameraClass.h>
#include <kenshi/Faction.h>
#include <kenshi/util/hand.h>

#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Window.h>
#include <mygui/MyGUI_Delegate.h>
#include <ogre/OgreRoot.h>
#include <ogre/OgreRenderSystem.h>
#include <ogre/OgreRenderTarget.h>

#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // ==== Game data ===============================================================

    // Squads from KenshiRogue.mod (scripts/build_mods.py); must match it.
    const char* SQUAD_ONE_BANDIT = "1-KenshiRogue.mod";  // "Kenshi Rogue: one starving bandit"
    const char* SQUAD_TWO_BANDITS = "2-KenshiRogue.mod"; // "Kenshi Rogue: two starving bandits"
    const char* RUNNER_TEMPLATE = "2205-gamedata.base";  // Drifter
    const char* RUNNER_NAME = "Runner";

    // Starting weapon: a Horse Chopper, Rusted Junk grade.
    const char* START_WEAPON = "475-gamedata.base";      // Horse Chopper
    const char* GRADE_MAKER = "912-gamedata.base";       // "Unknown" maker, the one with Rusted Junk
    const char* GRADE_RUSTED_JUNK = "913-gamedata.base"; // MaterialSpecsWeapon "Rusted Junk"
    const char* FIRST_AID = "209-gamedata.base";         // Basic First Aid Kit
    const char* RAG_SHIRT = "2309-clothes_v1.mod";       // Rag Shirt
    const char* RAG_PANTS = "551-gamedata.base";         // Halfpants (ragged)
    const char* ANKH = "6-KenshiRogue.mod";              // "Kenshi Rogue: ankh"

    // After a node: bow at the ankh to move on (Bill, 2026-11-23). A soft
    // timer, generous, after which the ankh calls you on anyway.
    DWORD AFTER_NODE_SECONDS = 180;       // the harness's `ankh_timer` shortens it
    const float ANKH_DISTANCE_M = 4.0f;   // the ankh appears this far from the runner
    const float BOW_RADIUS_M = 1.5f;      // a runner this close to the ankh's origin is bowing
    const DWORD BOW_MS = 1500;            // for this long, and the run moves on
    // Cats after a won fight: a random amount in this range, times the fight's number.
    const int CATS_MIN = 60, CATS_MAX = 140;
    const size_t OWNERSHIPS_MONEY = 0x88; // Ownerships::money (as Kenshi Town reads it)

    const float METRE = 10.0f;               // world units per metre
    const float ARENA_OFFSET_M = 0.0f;       // the arena is where your first character stands (they stand aside)
    const float STAND_ASIDE_M = 200.0f;      // your own characters move this far west for the run
    const float SPAWN_DISTANCE_M = 6.0f;     // enemies appear this far from the runner (25 m put them up a cliff)
    const float ARENA_RADIUS_M = 150.0f;     // anyone not the run's inside this is removed while a run is on

    // The runner's fixed stats (everything else stays at the template's).
    struct FixedStat
    {
        StatsEnumerated stat;
        float value;
    };
    const FixedStat RUNNER_STATS[] = {
        {STAT_STRENGTH, 10}, {STAT_MELEE_ATTACK, 10}, {STAT_MELEE_DEFENCE, 10}, {STAT_TOUGHNESS, 10},
        {STAT_DEXTERITY, 10}, {STAT_ATHLETICS, 10}, {STAT_DODGE, 5},
        // Every melee weapon skill alike, so trials compare weapons, not skills.
        {STAT_HACKERS, 10}, {STAT_KATANAS, 10}, {STAT_SABRES, 10}, {STAT_BLUNT, 10}, {STAT_HEAVYWEAPONS, 10},
        {STAT_POLEARMS, 10},
    };

    typedef RootObject* (__fastcall* CreateCharFn)(RootObjectFactory* self, Faction* faction,
                                                   Ogre::Vector3 position, RootObjectContainer* owner,
                                                   GameData* characterTemplate, Building* home, float age);
    typedef Item* (__fastcall* CreateItemFn)(RootObjectFactory* self, GameData* gd, const hand* handle,
                                             GameData* weaponMesh, GameData* matData, int levelOverride,
                                             Faction* flagUniform);
    CreateCharFn g_createChar = NULL;
    CreateItemFn g_createItem = NULL;
    typedef Building* (__fastcall* CreateBuildingFn)(
        RootObjectFactory* self, GameData* data, Ogre::Vector3 position, TownBase* t, Faction* owner,
        Ogre::Quaternion rotation, FactoryCallbackInterface* cb, Layout* furnitureOf, Building* isDoorOf,
        GameSaveState* saveState, Building* isIndoorsOf, bool invisible, bool completed, bool isFoliage,
        int floorNumber, bool isOutsideFurniture);
    CreateBuildingFn g_createBuilding = NULL;

    std::string factionNameOf(Faction* f)
    {
        return f ? f->getName() : std::string("(none)");
    }

    int* factionMoney(Faction* f)
    {
        if (!f || !f->factionOwnerships)
            return NULL;
        return reinterpret_cast<int*>(reinterpret_cast<char*>(f->factionOwnerships) + OWNERSHIPS_MONEY);
    }

    int g_frame = 0;
    bool g_atActive = false; // a harness scenario is running (see Autotest, below)
    // Game time: what the ankh's timer counts, so pausing to loot doesn't eat
    // it and 5x makes it pass 5x as fast. The harness's `clock` checks.
    double g_gameClock = 0.0;
    std::string g_startWeapon = START_WEAPON; // the scenario's `weapon` changes it, for trials

    void logLine(const std::string& s)
    {
        DebugLog("KenshiRogue: " + s);
    }

    std::string toStr(float v)
    {
        std::ostringstream o;
        o << v;
        return o.str();
    }

    GameData* data(const char* sid)
    {
        return ou ? ou->gamedata.getData(sid) : NULL;
    }

    bool inGame()
    {
        return ou && ou->player && ou->player->getAllPlayerCharacters().size() > 0;
    }

    // Halt, then teleport to an exact spot: KenshiCoop's `park`, proven there.
    bool park(Character* c, const Ogre::Vector3& to)
    {
        CharMovement* mv = c ? c->movement : NULL;
        if (!mv)
            return false;
        mv->halt();
        mv->_setPositionDirectionAndTeleport(to, Ogre::Quaternion::IDENTITY);
        return true;
    }

    typedef hand* (__fastcall* HandCtorFn)(hand* self, unsigned int index, unsigned int serial, itemType type,
                                           unsigned int container, unsigned int containerSerial);
    HandCtorFn g_handCtor = NULL;

    // Items: createItem(template, blank hand, NULL, NULL, -1). Weapons are
    // different, as KenshiCoop found (its "spike 451"): the maker goes first,
    // the hand is built as NULL_ITEM, the weapon template third, the grade
    // fourth, level 0. Template-first returns null for every weapon (our first
    // three runs, 2026-11-23).
    Item* makeItem(GameData* d, GameData* maker, GameData* grade)
    {
        if (!d || !g_createItem || !ou->theFactory)
            return NULL;
        if (!maker)
        {
            hand none;
            return g_createItem(ou->theFactory, d, &none, NULL, NULL, -1, NULL);
        }
        if (!g_handCtor)
            return NULL;
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtor(h, 0, 0, NULL_ITEM, 0, 0);
        Item* it = g_createItem(ou->theFactory, maker, h, d, grade, 0, NULL);
        logLine(std::string("makeItem ") + d->name + ": " + (it ? it->getName() : std::string("NOTHING")));
        return it;
    }

    // Everything a character carries or wears, destroyed: the runner's
    // loadout is the run's, not the template's.
    int stripItems(Character* c)
    {
        Inventory* inv = c ? c->getInventory() : NULL;
        if (!inv)
            return 0;
        std::vector<Item*> all;
        lektor<InventorySection*>& secs = inv->getAllSections();
        for (uint32_t s = 0; s < secs.size(); ++s)
        {
            if (!secs[s])
                continue;
            const Ogre::vector<InventorySection::SectionItem>::type& items = secs[s]->getItems();
            for (uint32_t i = 0; i < items.size(); ++i)
                if (items[i].item)
                    all.push_back(items[i].item);
        }
        int n = 0;
        for (size_t i = 0; i < all.size(); ++i)
            if (inv->removeItemAutoDestroy(all[i], all[i]->quantity))
                ++n;
        return n;
    }

    // ==== The run =================================================================

    // A run is RUN_ROWS rows. Row 0 is always the single starving bandit;
    // after each row you choose the next node from two or three (Bill,
    // 2026-11-23: "back to our node map where they choose their next
    // engagement"); the last row is an elite.
    enum NodeKind { NODE_FIGHT, NODE_REST, NODE_ELITE, NODE_SHOP };
    struct Node
    {
        NodeKind kind;
        const char* squad;   // fights: the squad in KenshiRogue.mod
        const char* faction; // fights: whose they are
        int firstAidKits;    // fights: kits each enemy carries
        int enemyCount;      // fights: how many the squad has (must match KenshiRogue.mod)
        int minRow, maxRow;  // the rows it can be offered in
        const char* label;
    };
    const char* STARVING = "1305-gamedata.base"; // Starving Bandits
    const char* DUST = "200-gamedata.base";      // Dust Bandits
    const Node ENCOUNTERS[] = {
        {NODE_FIGHT, SQUAD_ONE_BANDIT, STARVING, 1, 1, 0, 0, "a starving bandit"},
        {NODE_FIGHT, "7-KenshiRogue.mod", STARVING, 1, 1, 1, 2, "a hungry bandit leader"},
        {NODE_FIGHT, "8-KenshiRogue.mod", DUST, 0, 1, 1, 3, "a dust bandit"},
        {NODE_FIGHT, SQUAD_TWO_BANDITS, STARVING, 0, 2, 2, 4, "two starving bandits"},
        {NODE_FIGHT, "9-KenshiRogue.mod", STARVING, 0, 3, 3, 5, "three starving bandits"},
        {NODE_FIGHT, "10-KenshiRogue.mod", DUST, 0, 2, 4, 5, "two dust bandits"},
        {NODE_ELITE, "11-KenshiRogue.mod", DUST, 2, 1, 3, 6, "a dust bandit boss"},
        {NODE_ELITE, "12-KenshiRogue.mod", DUST, 2, 2, 5, 6, "a dust bandit boss and his second"},
    };
    const int ENCOUNTER_COUNT = sizeof(ENCOUNTERS) / sizeof(ENCOUNTERS[0]);
    const Node REST_NODE = {NODE_REST, NULL, NULL, 0, 0, 1, 5, "rest"};
    // Shops are real towns (Bill, 2026-11-23): you're taken there to spend the
    // run's cats at Kenshi's own traders, with a longer timer and an ankh to
    // leave. The town is the node's label. Keeping you in town (the leash)
    // waits on Bill's answer.
    const char* SHOP_TOWNS[] = {"Stoat", "Squin", "The Hub", "Mongrel", "Shark", "Waystation"};
    const int SHOP_TOWN_COUNT = sizeof(SHOP_TOWNS) / sizeof(SHOP_TOWNS[0]);
    const DWORD SHOP_SECONDS = 600; // game seconds in town
    const int RUN_ROWS = 7;

    std::string nodeLabel(const Node& n)
    {
        if (n.kind == NODE_SHOP)
            return std::string("Shop: ") + n.label;
        return std::string(n.kind == NODE_REST ? "Rest" : n.kind == NODE_ELITE ? "Elite: " : "Fight: ") +
               (n.kind == NODE_REST ? "" : n.label);
    }

    enum RunState { RUN_NONE, RUN_FIGHTING, RUN_BETWEEN, RUN_WON, RUN_LOST, RUN_CHOOSING, RUN_PATHING };

    struct Run
    {
        RunState state;
        int node;                       // the row being played or just finished (-1 before the first)
        Node current;                   // what that row's node is
        std::vector<Node> choices;      // the next row's offer, while choosing
        Ogre::Vector3 arena;
        std::vector<Character*> runners;
        std::vector<Character*> enemies;    // the current (or last) fight's
        std::vector<Character*> allEnemies; // every fight's, so far: the downed stay as bodies to loot
        DWORD fightStart;
        Ogre::Vector3 spawnAt;         // where the current fight's squad was made
        int fightsWon;
        int strangersRemoved;
        std::string lastResult;
        Building* ankh;                // standing after a node, until bowed at
        double ankhUntil;              // the soft timer, in game seconds (g_gameClock): when the ankh calls you on
        DWORD bowSince;                // when a runner started bowing (0: nobody)
        int catsEarned;
        std::map<MedicalSystem::HealthPartStatus*, float> fleshCap; // no natural healing: flesh never rises past this
        Run() : state(RUN_NONE), node(-1), current(ENCOUNTERS[0]), fightStart(0), fightsWon(0), strangersRemoved(0), ankh(NULL), ankhUntil(0),
              bowSince(0), catsEarned(0) {}
    };
    Run g_run;

    bool contains(const std::vector<Character*>& v, Character* c)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == c)
                return true;
        return false;
    }

    bool out(Character* c) // down or dead
    {
        return !c || c->isDead() || c->isDown();
    }

    std::string health(Character* c)
    {
        if (!c)
            return "(gone)";
        std::ostringstream o;
        o << (c->isDead() ? "DEAD" : c->isDown() ? "DOWN" : "up");
        MedicalSystem* m = c->getMedical();
        if (!m)
            return o.str();
        // Hunger too: starving bandits can collapse from it, which would
        // look like a win (third run: fight 1 "won" with no fight seen).
        o << ", blood " << int(m->blood) << "/" << int(m->getMaxBlood()) << ", hunger " << int(m->hunger)
          << (m->isHungerKO() ? " (COLLAPSED FROM HUNGER)" : "") << ";";
        for (int i = 0; i < m->getPartCount(); ++i)
        {
            MedicalSystem::HealthPartStatus* p = m->getPart((unsigned __int64)i);
            if (!p || !p->getData())
                continue;
            o << " " << p->getData()->name << " " << int(p->flesh) << "/" << int(p->maxHealth());
            if (p->fleshStun > 0.5f) // Kenshi's panel shows flesh minus stun damage
                o << " (stun " << int(p->fleshStun) << ")";
        }
        const char* limbNames[] = {"left arm", "right arm", "left leg", "right leg"};
        const RobotLimbs::Limb limbs[] = {RobotLimbs::LEFT_ARM, RobotLimbs::RIGHT_ARM, RobotLimbs::LEFT_LEG,
                                          RobotLimbs::RIGHT_LEG};
        for (int k = 0; k < 4; ++k)
        {
            LimbState s = m->getLimbState(limbs[k]);
            if (s != LIMB_ORIGINAL)
                o << "; " << limbNames[k] << (s == LIMB_STUMP ? " LOST" : s == LIMB_REPLACED ? " robotic" : " crushed");
        }
        return o.str();
    }

    // No world spawns near a run (Bill, 2026-11-23, after a passing group of
    // starving bandits walked through the second test): for the whole run,
    // anyone near the arena who isn't the run's (or yours) is removed, hostile
    // or not, animals included. A hook on the spawner itself would be the
    // thorough version; this is the slice's.
    void keepArenaQuiet()
    {
        if ((g_run.state != RUN_FIGHTING && g_run.state != RUN_BETWEEN && g_run.state != RUN_CHOOSING &&
             g_run.state != RUN_PATHING) ||
            g_run.runners.empty() ||
            g_frame % 30 != 0)
            return;
        if (g_run.current.kind == NODE_SHOP)
            return; // a town's people are the point of a shop
        Character* r = g_run.runners[0];
        lektor<RootObject*> found;
        ou->getCharactersWithinSphere(found, g_run.arena, ARENA_RADIUS_M * METRE, ARENA_RADIUS_M * METRE, 0.0f,
                                      200, 200, NULL);
        for (uint32_t i = 0; i < found.size(); ++i)
        {
            Character* c = static_cast<Character*>(found[i]);
            if (!c || contains(g_run.runners, c) || contains(g_run.allEnemies, c))
                continue;
            if (c->getFaction() == r->getFaction())
                continue;
            logLine("arena: removed a stranger, " + c->getName() + " of " +
                    (c->getFaction() ? c->getFaction()->getName() : std::string("(none)")));
            ou->destroy(c, false, "Kenshi Rogue: arena kept quiet");
            ++g_run.strangersRemoved;
        }
    }

    Character* createRunner(const Ogre::Vector3& at)
    {
        GameData* tmpl = data(RUNNER_TEMPLATE);
        if (!tmpl || !g_createChar)
            return NULL;
        RootObject* o = g_createChar(ou->theFactory, ou->player->getFaction(), at, NULL, tmpl, NULL, 25.0f);
        if (!o)
            return NULL;
        Character* c = static_cast<Character*>(o);
        ou->player->recruit(c, false);
        c->separateIntoMyOwnSquad(true); // its own squad, not your first character's
        c->setName(RUNNER_NAME);
        if (CharStats* st = c->getStats())
            for (size_t i = 0; i < sizeof(RUNNER_STATS) / sizeof(RUNNER_STATS[0]); ++i)
                st->getStatRef(RUNNER_STATS[i].stat) = RUNNER_STATS[i].value;
        std::ostringstream note;
        note << "runner: template's " << stripItems(c) << " items removed";
        logLine(note.str());
        // Rags: stripping the template took its clothes too (the runner fought
        // in underwear, 2026-11-23).
        const char* rags[] = {RAG_SHIRT, RAG_PANTS};
        for (int k = 0; k < 2; ++k)
            if (Item* it = makeItem(data(rags[k]), NULL, NULL))
                if (c->giveItem(it, false, true) && c->getInventory())
                    c->getInventory()->equipItem(it);
        Item* w = makeItem(data(g_startWeapon.c_str()), data(GRADE_MAKER), data(GRADE_RUSTED_JUNK));
        if (!w)
            logLine("runner's weapon: not made");
        else if (!c->giveItem(w, false, true))
            logLine("runner's weapon: made (" + w->getName() + ") but giveItem refused it");
        else
        {
            bool equipped = c->getInventory() && c->getInventory()->equipItem(w);
            logLine("runner's weapon: " + w->getName() + (equipped ? ", equipped" : ", NOT equipped"));
        }
        return c;
    }

    // The squad's people: that faction, near where it was made, up, and not
    // from an earlier fight. Called again for the first seconds of a fight:
    // the squad isn't always all there at once (trials: 3 of 37 spawns found
    // fewer, two found none and counted as instant wins). New ones get their
    // kits and the order to attack.
    void collectEnemies(const Node& n)
    {
        Faction* them = ou->factionMgr->getFactionByStringID(n.faction);
        lektor<RootObject*> found;
        ou->getCharactersWithinSphere(found, g_run.spawnAt, 30.0f * METRE, 30.0f * METRE, 0.0f, 40, 40, NULL);
        GameData* kit = data(FIRST_AID);
        for (uint32_t i = 0; i < found.size(); ++i)
        {
            Character* c = static_cast<Character*>(found[i]);
            if (!c || c->getFaction() != them || out(c) || contains(g_run.allEnemies, c))
                continue;
            g_run.enemies.push_back(c);
            g_run.allEnemies.push_back(c);
            for (int k = 0; k < n.firstAidKits; ++k)
                if (Item* it = makeItem(kit, NULL, NULL))
                    c->giveItem(it, false, true);
            if (!g_run.runners.empty())
                c->attackTarget(g_run.runners[0]);
        }
    }

    void startFight(const Node& n)
    {
        g_run.enemies.clear();
        g_run.current = n;
        Faction* them = ou->factionMgr->getFactionByStringID(n.faction);
        GameData* squad = data(n.squad);
        if (!them || !squad)
        {
            logLine(std::string("fight: missing ") + (them ? "the squad " : "the faction ") + n.squad);
            g_run.state = RUN_LOST;
            g_run.lastResult = "the encounter's data is missing (is KenshiRogue.mod enabled?)";
            return;
        }
        Ogre::Vector3 at = g_run.arena; // not the runner's position: just back from a shop, it may not have landed yet
        at.x += SPAWN_DISTANCE_M * METRE;
        // The runner as the squad's AI target: spawned without one, the
        // bandit stood 25 m away and never came (first run, 2026-11-23).
        Platoon* made = ou->theFactory->createRandomSquad(them, at, NULL, 20, NULL, squad, NULL, NULL, NULL, false,
                                                          g_run.runners[0]->getHandle(), NULL, 1.0f, SQ_ROAMING,
                                                          false);
        g_run.spawnAt = at;
        collectEnemies(n);
        std::ostringstream o;
        o << n.label << ": squad " << (made ? "made" : "NOT made") << ", " << g_run.enemies.size() << " enemies";
        logLine(o.str());
        g_run.fightStart = GetTickCount();
        g_run.state = RUN_FIGHTING;
    }

    // Rest: every wound healed and the blood back. A lost limb stays lost.
    void rest()
    {
        for (size_t i = 0; i < g_run.runners.size(); ++i)
        {
            Character* c = g_run.runners[i];
            MedicalSystem* m = c && !c->isDead() ? c->getMedical() : NULL;
            if (!m)
                continue;
            for (int k = 0; k < m->getPartCount(); ++k)
            {
                MedicalSystem::HealthPartStatus* p = m->getPart((unsigned __int64)k);
                if (!p || (p->whatAmI != MedicalSystem::HealthPartStatus::PART_TORSO &&
                           p->whatAmI != MedicalSystem::HealthPartStatus::PART_HEAD &&
                           p->getRobotLimbState() == LIMB_STUMP))
                    continue;
                p->flesh = p->maxHealth();
                // fleshStun is stun *damage*, 0 when fine: setting it to the
                // maximum knocked the runner out (fourth run; the "7" on Kenshi's
                // panel in the second).
                p->fleshStun = 0.0f;
                p->updateDerivedHealths();
                g_run.fleshCap[p] = p->flesh;
            }
            m->blood = m->getMaxBlood();
            m->validateHealthValues();
            logLine("rest: " + c->getName() + " now " + health(c));
        }
    }

    // ---- The ankh: bow at it to leave a node --------------------------------

    void removeAnkh()
    {
        if (g_run.ankh)
            ou->dynamicDestroyBuilding(hand(g_run.ankh));
        g_run.ankh = NULL;
        g_run.bowSince = 0;
    }

    // Beside the runner, or beside the given spot: a teleport takes a frame to land, so
    // in a shop the runner's position is still the arena's (shop test,
    // 2026-11-23: the ankh went up 683 m from the runner).
    void spawnAnkh(const Ogre::Vector3* beside)
    {
        removeAnkh();
        GameData* d = data(ANKH);
        g_run.ankhUntil = g_gameClock + AFTER_NODE_SECONDS;
        if (!d || !g_createBuilding || g_run.runners.empty())
        {
            logLine("ankh: not made (no data or no runner); the timer still runs");
            return;
        }
        Ogre::Vector3 at = beside ? *beside : g_run.runners[0]->getPosition();
        Ogre::Vector3 pos(at.x, 0.0f, at.z + ANKH_DISTANCE_M * METRE); // y 0: createBuilding adds the ground's height
        g_run.ankh = g_createBuilding(ou->theFactory, d, pos, NULL, ou->player->getFaction(),
                                      Ogre::Quaternion::IDENTITY, NULL, NULL, NULL, NULL, NULL, false, true, false, 0,
                                      false);
        g_run.bowSince = 0;
        if (g_run.ankh)
        {
            Ogre::Vector3 p = g_run.ankh->getPosition();
            std::ostringstream o;
            o << "ankh: up at " << int(p.x) << "," << int(p.y) << "," << int(p.z) << "; " << AFTER_NODE_SECONDS
              << " s to bow";
            logLine(o.str());
        }
        else
            logLine("ankh: createBuilding returned nothing");
    }

    void payCats()
    {
        // More the deeper the row, and double for an elite.
        int cats = (CATS_MIN + rand() % (CATS_MAX - CATS_MIN + 1)) * (g_run.node + 1) *
                   (g_run.current.kind == NODE_ELITE ? 2 : 1);
        int* money = factionMoney(ou->player->getFaction());
        if (money)
            *money += cats;
        g_run.catsEarned += cats;
        std::ostringstream o;
        o << "paid " << cats << " cats" << (money ? "" : " (NO money record found)") << "; " << g_run.catsEarned
          << " this run";
        logLine(o.str());
    }

    void advance();

    // Bowing: a runner at the ankh's origin (where its use kneels you) for
    // BOW_MS. Or the timer runs out and the ankh calls you on.
    void watchAnkh()
    {
        if (g_run.state != RUN_BETWEEN || !g_run.ankhUntil)
            return;
        if (g_gameClock > g_run.ankhUntil)
        {
            logLine("ankh: time's up; the ankh calls you on");
            advance();
            return;
        }
        if (!g_run.ankh)
            return;
        Ogre::Vector3 a = g_run.ankh->getPosition();
        bool bowing = false;
        for (size_t i = 0; i < g_run.runners.size() && !bowing; ++i)
        {
            Ogre::Vector3 p = g_run.runners[i]->getPosition();
            float dx = p.x - a.x, dz = p.z - a.z;
            bowing = !out(g_run.runners[i]) && dx * dx + dz * dz < BOW_RADIUS_M * METRE * BOW_RADIUS_M * METRE;
        }
        if (!bowing)
            g_run.bowSince = 0;
        else if (!g_run.bowSince)
            g_run.bowSince = GetTickCount();
        else if (GetTickCount() - g_run.bowSince > BOW_MS)
        {
            logLine("ankh: bowed; moving on");
            advance();
        }
    }

    // The town called `name` with the most buildings (a name can have empty
    // twins: Kenshi Town found two Hubs).
    TownBase* findTown(const std::string& name)
    {
        if (!shou || !shou->townList)
            return NULL;
        lektor<RootObject*>& towns = shou->townList->getAllTowns();
        TownBase* best = NULL;
        size_t bestN = 0;
        for (uint32_t i = 0; i < towns.size(); ++i)
        {
            TownBase* t = static_cast<TownBase*>(towns[i]);
            if (!t || t->getName() != name)
                continue;
            TownBuildingsManager* mgr = t->getBuildingsManager();
            size_t n = mgr ? mgr->buildingEntities.size() : 0;
            if (!best || n > bestN)
            {
                best = t;
                bestN = n;
            }
        }
        return best;
    }

    // Runners to a spot, and kept trying until they land: a runner still
    // bowing at an ankh ignores a teleport (shop test, 2026-11-23: the runner
    // stayed in The Hub). Their ankh-use order is dropped first, and the
    // teleport repeats each frame (up to 4 s) until they're within 20 m.
    Ogre::Vector3 g_parkTo;
    int g_parkFrames = 0;

    void parkRunners(const Ogre::Vector3& at)
    {
        g_parkTo = at;
        g_parkFrames = 240;
        for (size_t i = 0; i < g_run.runners.size(); ++i)
        {
            g_run.runners[i]->removeJob(TaskType(87)); // the ankh's use (operate machinery)
            park(g_run.runners[i], Ogre::Vector3(at.x + float(i) * 2.0f * METRE, at.y, at.z));
        }
    }

    void keepParking()
    {
        if (g_parkFrames <= 0)
            return;
        --g_parkFrames;
        bool all = true;
        for (size_t i = 0; i < g_run.runners.size(); ++i)
        {
            Ogre::Vector3 want(g_parkTo.x + float(i) * 2.0f * METRE, g_parkTo.y, g_parkTo.z);
            Ogre::Vector3 p = g_run.runners[i]->getPosition();
            float dx = p.x - want.x, dz = p.z - want.z;
            if (dx * dx + dz * dz < 20.0f * METRE * 20.0f * METRE)
                continue;
            all = false;
            if (g_parkFrames % 10 == 0)
            {
                g_run.runners[i]->removeJob(TaskType(87));
                park(g_run.runners[i], want);
            }
        }
        if (all)
        {
            logLine("park: everyone landed");
            g_parkFrames = 0;
        }
        else if (g_parkFrames == 0)
            logLine("park: a runner didn't land");
    }

    void spawnAnkh(const Ogre::Vector3* beside = NULL);

    void goShopping(const Node& n)
    {
        TownBase* t = findTown(n.label);
        if (!t || g_run.runners.empty())
        {
            logLine(std::string("shop: no town called ") + n.label + "; the ankh is here instead");
            g_run.state = RUN_BETWEEN;
            spawnAnkh();
            return;
        }
        Ogre::Vector3 c = t->getPosition();
        parkRunners(c);
        ou->player->focusCamera(c);
        g_run.state = RUN_BETWEEN;
        DWORD normal = AFTER_NODE_SECONDS;
        AFTER_NODE_SECONDS = SHOP_SECONDS;
        spawnAnkh(&c);
        AFTER_NODE_SECONDS = normal;
        std::ostringstream o;
        o << "shop: in " << t->getName() << " at " << int(c.x) << "," << int(c.y) << "," << int(c.z) << " ("
          << factionNameOf(t->getFaction()) << ")";
        logLine(o.str());
    }

    void play(const Node& n)
    {
        g_run.current = n;
        if (n.kind == NODE_SHOP)
        {
            logLine("row " + toStr(float(g_run.node)) + ": " + nodeLabel(n));
            goShopping(n);
            return;
        }
        logLine("row " + toStr(float(g_run.node)) + ": " + nodeLabel(n));
        if (n.kind == NODE_REST)
        {
            rest();
            g_run.lastResult = "rested";
            g_run.state = RUN_BETWEEN;
            spawnAnkh();
            return;
        }
        startFight(n);
    }

    // ---- The path: the next row's choices -----------------------------------

    void showChoices(const char* title, const std::vector<std::string>& labels, void (*pick)(size_t));
    void goPath(size_t i);

    void offerPath()
    {
        int row = g_run.node;
        std::vector<Node> fights, elites;
        for (int i = 0; i < ENCOUNTER_COUNT; ++i)
            if (ENCOUNTERS[i].minRow <= row && row <= ENCOUNTERS[i].maxRow)
                (ENCOUNTERS[i].kind == NODE_ELITE ? elites : fights).push_back(ENCOUNTERS[i]);
        std::vector<Node> offer;
        if (row == RUN_ROWS - 1)
            offer = elites; // the last row: an elite
        else
        {
            for (int k = 0; k < 2 && !fights.empty(); ++k)
            {
                size_t j = size_t(rand()) % fights.size();
                offer.push_back(fights[j]);
                fights.erase(fights.begin() + j);
            }
            // The third: an elite, a shop or a rest, whichever this row allows.
            std::vector<Node> third;
            if (!elites.empty())
                third.push_back(elites[size_t(rand()) % elites.size()]);
            if (row >= 2 && row <= RUN_ROWS - 2 && g_run.current.kind != NODE_SHOP)
            {
                Node shop = {NODE_SHOP, NULL, NULL, 0, 0, 2, RUN_ROWS - 2, SHOP_TOWNS[rand() % SHOP_TOWN_COUNT]};
                third.push_back(shop);
            }
            if (g_run.current.kind != NODE_REST && REST_NODE.minRow <= row && row <= REST_NODE.maxRow)
                third.push_back(REST_NODE);
            if (!third.empty())
                offer.push_back(third[size_t(rand()) % third.size()]);
        }
        if (offer.empty())
            offer.push_back(ENCOUNTERS[0]);
        g_run.choices = offer;
        g_run.state = RUN_PATHING;
        std::vector<std::string> labels;
        std::ostringstream o;
        o << "row " << row << " of " << RUN_ROWS - 1 << ", offered:";
        for (size_t i = 0; i < offer.size(); ++i)
        {
            labels.push_back(nodeLabel(offer[i]));
            o << " [" << i << "] " << labels.back() << ";";
        }
        logLine(o.str());
        std::ostringstream title;
        title << "Choose your path (" << row << " of " << RUN_ROWS - 1 << ")";
        showChoices(title.str().c_str(), labels, goPath);
    }

    // Moves the run on: start it, or play the next node.
    void advance()
    {
        if (!inGame())
            return;
        if (g_run.state == RUN_FIGHTING)
            return;
        if (g_run.state == RUN_NONE || g_run.state == RUN_WON || g_run.state == RUN_LOST)
            return; // starting is runStart's job
        int next = g_run.node + 1;
        removeAnkh();
        g_run.ankhUntil = 0;
        if (next >= RUN_ROWS)
        {
            g_run.state = RUN_WON;
            g_run.lastResult = "run complete";
            logLine("run complete");
            return;
        }
        // The last fight's enemies leave once you move on (loot them first).
        // Knocked out isn't for good: fight 1's bandit got up again and
        // wandered into fight 2 (second run).
        int left = 0;
        for (size_t i = 0; i < g_run.enemies.size(); ++i)
            if (g_run.enemies[i])
            {
                ou->destroy(g_run.enemies[i], false, "Kenshi Rogue: the fight's over");
                ++left;
            }
        if (left)
            logLine("the last fight's " + toStr(float(left)) + " enemies left");
        g_run.enemies.clear();
        if (g_run.current.kind == NODE_SHOP && g_run.node >= 0)
        {
            parkRunners(g_run.arena);
            ou->player->focusCamera(g_run.arena);
            logLine("shop: back to the arena");
        }
        g_run.node = next;
        if (next == 0)
        {
            play(ENCOUNTERS[0]); // always the starving bandit
            return;
        }
        offerPath();
    }

    // ---- Boons: a run opens with a choice of three (Bill, 2026-11-23) ------

    enum BoonKind { BOON_STAT, BOON_WEAPON, BOON_KITS, BOON_CATS };
    struct Boon
    {
        BoonKind kind;
        StatsEnumerated stat;
        float amount;
        std::string weapon; // stringId, BOON_WEAPON
        std::string label;
    };
    std::vector<Boon> g_offer;
    bool g_atBoons = false; // scenarios skip the choice unless they ask for it (`boons on`)

    const char* MIDGRADE_MAKER = "917-gamedata.base"; // "Ancient": its grades are Mid-Grade Salvage, (Old) Refitted
    const char* GRADE_MIDGRADE = "918-gamedata.base"; // Mid-Grade Salvage

    Boon statBoon(StatsEnumerated st, float amount)
    {
        Boon b;
        b.kind = BOON_STAT;
        b.stat = st;
        b.amount = amount;
        std::ostringstream o;
        o << "+" << int(amount) << " " << CharStats::getStatName(st);
        b.label = o.str();
        return b;
    }

    std::vector<Boon> boonPool()
    {
        std::vector<Boon> pool;
        pool.push_back(statBoon(STAT_MELEE_ATTACK, 10));
        pool.push_back(statBoon(STAT_MELEE_DEFENCE, 10));
        pool.push_back(statBoon(STAT_STRENGTH, 10));
        pool.push_back(statBoon(STAT_TOUGHNESS, 10));
        const StatsEnumerated skills[] = {STAT_SABRES, STAT_KATANAS, STAT_HACKERS, STAT_BLUNT, STAT_HEAVYWEAPONS};
        pool.push_back(statBoon(skills[rand() % 5], 10));
        // A better weapon: a random type at Mid-Grade Salvage.
        const char* types[] = {"474-gamedata.base", "476-gamedata.base", "478-gamedata.base", "901-gamedata.base",
                               "903-gamedata.base", "1020-gamedata.base", "475-gamedata.base"};
        GameData* w = data(types[rand() % 7]);
        if (w)
        {
            Boon b;
            b.kind = BOON_WEAPON;
            b.weapon = w->stringID;
            b.amount = 0;
            b.stat = STAT_NONE;
            b.label = "A Mid-Grade Salvage " + w->name;
            pool.push_back(b);
        }
        Boon kits;
        kits.kind = BOON_KITS;
        kits.amount = 3;
        kits.stat = STAT_NONE;
        kits.label = "3 Basic First Aid Kits";
        pool.push_back(kits);
        Boon cats;
        cats.kind = BOON_CATS;
        cats.amount = 300;
        cats.stat = STAT_NONE;
        cats.label = "A purse of 300 cats";
        pool.push_back(cats);
        return pool;
    }

    std::string applyBoon(const Boon& b)
    {
        Character* r = g_run.runners.empty() ? NULL : g_run.runners[0];
        if (!r)
            return "no runner";
        if (b.kind == BOON_STAT)
        {
            if (CharStats* st = r->getStats())
                st->getStatRef(b.stat) += b.amount;
            return b.label;
        }
        if (b.kind == BOON_WEAPON)
        {
            Item* w = makeItem(data(b.weapon.c_str()), data(MIDGRADE_MAKER), data(GRADE_MIDGRADE));
            if (!w || !r->giveItem(w, false, true))
                return b.label + ": NOT made";
            bool equipped = r->getInventory() && r->getInventory()->equipItem(w);
            return b.label + " (" + w->getName() + (equipped ? ", equipped)" : ", in the pack)");
        }
        if (b.kind == BOON_KITS)
        {
            int n = 0;
            for (int i = 0; i < int(b.amount); ++i)
                if (Item* it = makeItem(data(FIRST_AID), NULL, NULL))
                    n += r->giveItem(it, false, true) ? 1 : 0;
            return b.label + " (" + toStr(float(n)) + " given)";
        }
        int* money = factionMoney(ou->player->getFaction());
        if (money)
            *money += int(b.amount);
        g_run.catsEarned += int(b.amount);
        return b.label;
    }

    void advance();
    void choose(size_t i);

    // One small Kenshi window of up to three buttons: boons, and the path.
    MyGUI::Window* g_choiceWindow = NULL;
    void (*g_choicePick)(size_t) = NULL;
    std::string g_choiceTitle;
    std::vector<std::string> g_choiceLabels;
    void onChoice0(MyGUI::WidgetPtr) { if (g_choicePick) g_choicePick(0); }
    void onChoice1(MyGUI::WidgetPtr) { if (g_choicePick) g_choicePick(1); }
    void onChoice2(MyGUI::WidgetPtr) { if (g_choicePick) g_choicePick(2); }

    void closeChoices()
    {
        if (g_choiceWindow && MyGUI::Gui::getInstancePtr())
            MyGUI::Gui::getInstancePtr()->destroyWidget(g_choiceWindow);
        g_choiceWindow = NULL;
    }

    void showChoices(const char* title, const std::vector<std::string>& labels, void (*pick)(size_t))
    {
        g_choiceTitle = title;
        g_choiceLabels = labels;
        g_choicePick = pick;
        closeChoices();
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui)
            return;
        g_choiceWindow = gui->createWidgetReal<MyGUI::Window>("Kenshi_WindowCX", 0.36f, 0.30f, 0.28f, 0.30f,
                                                              MyGUI::Align::Default, "Window", "KenshiRogueChoice");
        g_choiceWindow->setCaption(title);
        void (*clicks[])(MyGUI::WidgetPtr) = {onChoice0, onChoice1, onChoice2};
        for (size_t i = 0; i < labels.size() && i < 3; ++i)
        {
            MyGUI::Button* b = g_choiceWindow->getClientWidget()->createWidgetReal<MyGUI::Button>(
                "Kenshi_Button1", 0.05f, 0.06f + 0.31f * float(i), 0.90f, 0.25f, MyGUI::Align::Default);
            b->setCaption(labels[i]);
            b->eventMouseButtonClick += MyGUI::newDelegate(clicks[i]);
        }
    }

    void offerBoons()
    {
        std::vector<Boon> pool = boonPool();
        g_offer.clear();
        while (g_offer.size() < 3 && !pool.empty())
        {
            size_t k = size_t(rand()) % pool.size();
            g_offer.push_back(pool[k]);
            pool.erase(pool.begin() + k);
        }
        g_run.state = RUN_CHOOSING;
        std::ostringstream o;
        o << "boons offered:";
        for (size_t i = 0; i < g_offer.size(); ++i)
            o << " [" << i << "] " << g_offer[i].label << ";";
        logLine(o.str());
        std::vector<std::string> labels;
        for (size_t i = 0; i < g_offer.size(); ++i)
            labels.push_back(g_offer[i].label);
        showChoices("Choose a boon", labels, choose);
    }

    void choose(size_t i)
    {
        if (g_run.state != RUN_CHOOSING || i >= g_offer.size())
            return;
        logLine("boon chosen: " + applyBoon(g_offer[i]));
        closeChoices();
        g_run.state = RUN_BETWEEN; // row -1: advance() plays row 0
        advance();
    }

    void goPath(size_t i)
    {
        if (g_run.state != RUN_PATHING || i >= g_run.choices.size())
            return;
        closeChoices();
        Node n = g_run.choices[i];
        g_run.choices.clear();
        play(n);
    }

    bool runStart(float offsetX_m, float offsetZ_m)
    {
        if (!inGame() || !g_createChar)
            return false;
        Character* first = ou->player->getAllPlayerCharacters()[0];
        Ogre::Vector3 at = first->getPosition();
        at.x += offsetX_m * METRE;
        at.z += offsetZ_m * METRE;
        g_run = Run();
        g_run.arena = at;
        // Your own characters stand aside: the arena is where you are (ground
        // you chose), and the runner fights alone. Third run: 200 m away put
        // the runner on a mesa the bandit couldn't reach; second run: a
        // skeleton beside the runner fought fight 1 with them.
        const lektor<Character*>& yours = ou->player->getAllPlayerCharacters();
        std::vector<Character*> aside(yours.begin(), yours.end());
        for (size_t i = 0; i < aside.size(); ++i)
        {
            Ogre::Vector3 from = aside[i]->getPosition();
            Ogre::Vector3 to(from.x - STAND_ASIDE_M * METRE, from.y, from.z + float(i) * 3.0f * METRE);
            bool moved = park(aside[i], to);
            std::ostringstream o;
            o << "stand aside: " << aside[i]->getName() << (moved ? " moved" : " NOT moved") << " from "
              << int(from.x) << "," << int(from.z) << " to " << int(to.x) << "," << int(to.z);
            logLine(o.str());
        }
        Character* r = createRunner(at);
        if (!r)
        {
            logLine("run: could not create the runner");
            return false;
        }
        g_run.runners.push_back(r);
        g_run.arena = r->getPosition(); // where it actually landed
        std::ostringstream o;
        o << "run started: arena at " << int(g_run.arena.x) << "," << int(g_run.arena.z) << "; " << health(r);
        logLine(o.str());
        ou->player->focusCamera(g_run.arena);
        if (!g_atActive || g_atBoons)
        {
            offerBoons(); // fight 1 starts when one is chosen
            return true;
        }
        g_run.state = RUN_BETWEEN; // node -1: advance() plays node 0
        advance();
        return true;
    }

    // No natural healing during a run (Bill, 2026-11-23: waiting around after
    // a fight shouldn't heal you). Each runner body part's flesh may only go
    // down; a rise from Kenshi's healing is clipped back. Only rest raises the
    // cap. Bleeding, stun and knockouts are Kenshi's own, untouched (a hook on
    // HealthPartStatus::update's heal multipliers would also reach those).
    bool g_noRegen = true;
    float g_regenClipped = 0.0f; // flesh taken back, for the harness

    void holdWounds()
    {
        if (!g_noRegen || g_run.state == RUN_NONE)
            return;
        for (size_t i = 0; i < g_run.runners.size(); ++i)
        {
            Character* c = g_run.runners[i];
            MedicalSystem* m = c && !c->isDead() ? c->getMedical() : NULL;
            for (int k = 0; m && k < m->getPartCount(); ++k)
            {
                MedicalSystem::HealthPartStatus* p = m->getPart((unsigned __int64)k);
                if (!p)
                    continue;
                std::map<MedicalSystem::HealthPartStatus*, float>::iterator cap = g_run.fleshCap.find(p);
                if (cap == g_run.fleshCap.end())
                    g_run.fleshCap[p] = p->flesh;
                else if (p->flesh < cap->second)
                    cap->second = p->flesh;
                else if (p->flesh > cap->second)
                {
                    g_regenClipped += p->flesh - cap->second;
                    p->flesh = cap->second;
                }
            }
        }
    }

    void checkFight()
    {
        if (g_run.state != RUN_FIGHTING || g_frame % 15 != 0)
            return;
        // The order again every 10 s, for whoever is up (getting up from a
        // knockout, or distracted).
        static DWORD lastOrder = 0;
        if (GetTickCount() - lastOrder > 10000 && !g_run.runners.empty() && !out(g_run.runners[0]))
        {
            lastOrder = GetTickCount();
            for (size_t i = 0; i < g_run.enemies.size(); ++i)
                if (!out(g_run.enemies[i]))
                    g_run.enemies[i]->attackTarget(g_run.runners[0]);
        }
        const Node& n = g_run.current;
        DWORD since = GetTickCount() - g_run.fightStart;
        if (int(g_run.enemies.size()) < n.enemyCount)
        {
            if (since < 5000)
            {
                collectEnemies(n);
                return;
            }
            if (g_run.enemies.empty())
            {
                g_run.lastResult = std::string(n.label) + ": NO ENEMIES APPEARED";
                logLine(g_run.lastResult);
                g_run.state = RUN_BETWEEN; // not a win: fightsWon unchanged
                return;
            }
        }
        bool enemiesOut = true, runnersOut = true;
        for (size_t i = 0; i < g_run.enemies.size(); ++i)
            enemiesOut = enemiesOut && out(g_run.enemies[i]);
        for (size_t i = 0; i < g_run.runners.size(); ++i)
            runnersOut = runnersOut && out(g_run.runners[i]);
        if (!enemiesOut && !runnersOut)
            return;
        std::ostringstream o;
        o << "row " << g_run.node << ", " << nodeLabel(g_run.current) << ": " << (runnersOut ? "LOST" : "WON") << " in "
          << (GetTickCount() - g_run.fightStart) / 1000 << " s; runner " << health(g_run.runners[0]);
        g_run.lastResult = o.str();
        logLine(g_run.lastResult);
        if (runnersOut)
            g_run.state = RUN_LOST;
        else
        {
            ++g_run.fightsWon;
            g_run.state = RUN_BETWEEN;
            payCats();
            spawnAnkh();
        }
    }

    // ==== HUD =====================================================================

    MyGUI::Button* g_hud = NULL;
    const float HUD_X = 0.567f, HUD_Y = 0.747f, HUD_W = 0.14f, HUD_H = 0.035f; // right of Kenshi Town's

    std::string hudCaption()
    {
        switch (g_run.state)
        {
        case RUN_NONE:
            return "Rogue: start run";
        case RUN_FIGHTING:
            return "Rogue: " + nodeLabel(g_run.current);
        case RUN_BETWEEN:
            if (g_run.ankhUntil)
            {
                DWORD left = g_run.ankhUntil > g_gameClock ? DWORD(g_run.ankhUntil - g_gameClock) : 0;
                std::ostringstream o;
                o << "Rogue: " << (g_run.current.kind == NODE_SHOP ? std::string("in ") + g_run.current.label + ", " : "")
                  << "bow at the ankh (" << left / 60 << ":" << (left % 60 < 10 ? "0" : "") << left % 60 << ")";
                return o.str();
            }
            return g_run.node + 1 < RUN_ROWS ? "Rogue: move on" : "Rogue: finish run";
        case RUN_WON:
            return "Rogue: run complete";
        case RUN_LOST:
            return "Rogue: run lost";
        case RUN_CHOOSING:
            return "Rogue: choose a boon";
        case RUN_PATHING:
            return "Rogue: choose your path";
        }
        return "Rogue";
    }

    void onHudClick(MyGUI::WidgetPtr)
    {
        if (g_run.state == RUN_NONE || g_run.state == RUN_WON || g_run.state == RUN_LOST)
            runStart(ARENA_OFFSET_M, 0.0f);
        else if (g_run.state == RUN_FIGHTING && !g_run.runners.empty())
            ou->player->focusCamera(g_run.runners[0]->getPosition());
        else if (g_run.state == RUN_CHOOSING || g_run.state == RUN_PATHING)
            showChoices(g_choiceTitle.c_str(), g_choiceLabels, g_choicePick); // closed by mistake: back again
        else
            advance();
    }

    void updateHud()
    {
        bool game = inGame();
        if (!g_hud && game && MyGUI::Gui::getInstancePtr())
        {
            g_hud = MyGUI::Gui::getInstancePtr()->createWidgetReal<MyGUI::Button>(
                "Kenshi_Button1", HUD_X, HUD_Y, HUD_W, HUD_H, MyGUI::Align::Default, "Main", "KenshiRogueHud");
            g_hud->eventMouseButtonClick += MyGUI::newDelegate(onHudClick);
        }
        if (g_hud)
        {
            g_hud->setVisible(game);
            g_hud->setCaption(hudCaption());
        }
    }

    // ==== Autotest ================================================================
    //
    // Same shape as kenshi-town's: scripts/autotest.py writes a scenario,
    // picks the mods (data/mods.cfg) and launches Kenshi; the plugin runs it
    // one line at a time, writes autotest/out/report.txt and ends Kenshi.
    // Without the file nothing here runs.

    const char* AT_SCENARIO = "mods\\KenshiRogue\\autotest\\scenario.txt";
    const char* AT_REPORT = "mods\\KenshiRogue\\autotest\\out\\report.txt";
    const std::string AT_OUT = "mods\\KenshiRogue\\autotest\\out\\";

    bool g_atChecked = false;
    std::vector<std::string> g_atLines;
    size_t g_atPc = 0;
    DWORD g_atStepStart = 0;
    bool g_atStepBegun = false;
    int g_atPass = 0, g_atFail = 0;
    int g_atFramesSeen = 0;
    DWORD g_atReadySince = 0;

    void atWrite(const std::string& kind, const std::string& text)
    {
        FILE* f = fopen(AT_REPORT, "a");
        if (f)
        {
            fprintf(f, "%s\t%s\n", kind.c_str(), text.c_str());
            fclose(f);
        }
        logLine("[autotest] " + kind + " " + text);
    }

    void atCheck(bool ok, const std::string& what)
    {
        ++(ok ? g_atPass : g_atFail);
        atWrite(ok ? "PASS" : "FAIL", what);
    }

    void atFinish(const std::string& why)
    {
        std::ostringstream o;
        o << "pass=" << g_atPass << " fail=" << g_atFail << " (" << why << ")";
        atWrite("DONE", o.str());
        g_atActive = false;
        TerminateProcess(GetCurrentProcess(), 0); // nothing saved: the test save stays as it was
    }

    void atLoadScenario()
    {
        g_atChecked = true;
        FILE* f = fopen(AT_SCENARIO, "r");
        if (!f)
            return;
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            std::string l(line);
            while (!l.empty() && (l[l.size() - 1] == '\n' || l[l.size() - 1] == '\r' || l[l.size() - 1] == ' '))
                l.erase(l.size() - 1);
            size_t s = l.find_first_not_of(" \t");
            if (s == std::string::npos || l[s] == '#')
                continue;
            g_atLines.push_back(l.substr(s));
        }
        fclose(f);
        g_atActive = !g_atLines.empty();
        if (g_atActive)
            atWrite("INFO", "scenario loaded");
    }

    bool atScreenshot(const std::string& name)
    {
        Ogre::Root* root = Ogre::Root::getSingletonPtr();
        Ogre::RenderSystem* rs = root ? root->getRenderSystem() : NULL;
        if (!rs)
            return false;
        Ogre::RenderSystem::RenderTargetIterator it = rs->getRenderTargetIterator();
        while (it.hasMoreElements())
        {
            Ogre::RenderTarget* t = it.getNext();
            if (t && t->isPrimary())
            {
                t->writeContentsToFile(AT_OUT + name + ".png");
                return true;
            }
        }
        return false;
    }

    const char* stateName(RunState s)
    {
        const char* n[] = {"none", "fighting", "between", "won", "lost", "choosing", "pathing"};
        return n[s];
    }

    std::string inventoryOf(Character* c)
    {
        Inventory* inv = c ? c->getInventory() : NULL;
        if (!inv)
            return "(none)";
        std::map<std::string, int> counts;
        lektor<InventorySection*>& secs = inv->getAllSections();
        for (uint32_t s = 0; s < secs.size(); ++s)
        {
            if (!secs[s])
                continue;
            const Ogre::vector<InventorySection::SectionItem>::type& items = secs[s]->getItems();
            for (uint32_t i = 0; i < items.size(); ++i)
                if (items[i].item)
                    counts[items[i].item->getName()] += items[i].item->quantity;
        }
        std::ostringstream o;
        for (std::map<std::string, int>::iterator i = counts.begin(); i != counts.end(); ++i)
            o << (i == counts.begin() ? "" : "; ") << i->second << " x " << i->first;
        return counts.empty() ? "(empty)" : o.str();
    }

    // Looting by hand, done for the test: everything the downed enemies
    // carry goes into the runner's pack (what fits).
    std::string lootEnemies()
    {
        if (g_run.runners.empty())
            return "no runner";
        Character* r = g_run.runners[0];
        std::map<std::string, int> taken;
        for (size_t e = 0; e < g_run.enemies.size(); ++e)
        {
            Inventory* inv = g_run.enemies[e] ? g_run.enemies[e]->getInventory() : NULL;
            if (!inv)
                continue;
            std::vector<Item*> all;
            lektor<InventorySection*>& secs = inv->getAllSections();
            for (uint32_t s = 0; s < secs.size(); ++s)
            {
                if (!secs[s])
                    continue;
                const Ogre::vector<InventorySection::SectionItem>::type& items = secs[s]->getItems();
                for (uint32_t i = 0; i < items.size(); ++i)
                    if (items[i].item)
                        all.push_back(items[i].item);
            }
            for (size_t i = 0; i < all.size(); ++i)
            {
                std::string name = all[i]->getName();
                int q = all[i]->quantity;
                Item* moved = inv->removeItemDontDestroy_returnsItem(all[i], q, false);
                if (moved && r->giveItem(moved, true, false))
                    taken[name] += q;
            }
        }
        std::ostringstream o;
        for (std::map<std::string, int>::iterator i = taken.begin(); i != taken.end(); ++i)
            o << (i == taken.begin() ? "" : "; ") << i->second << " x " << i->first;
        return taken.empty() ? "nothing" : o.str();
    }

    bool atStep(const std::string& line)
    {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        std::string rest;
        std::getline(in, rest);
        size_t s = rest.find_first_not_of(" \t");
        rest = s == std::string::npos ? "" : rest.substr(s);
        DWORD elapsed = GetTickCount() - g_atStepStart;

        if (cmd == "load") // load <save>: from the main menu
        {
            if (g_atFramesSeen < 600)
                return false;
            SaveManager* sm = SaveManager::getSingleton();
            if (!sm)
                return elapsed > 120000 ? (atFinish("no SaveManager"), true) : false;
            atWrite("INFO", "loading save " + rest);
            sm->load(rest);
            return true;
        }
        if (cmd == "wait_ready") // wait_ready [secs]: in a game, settled 5 s
        {
            int limit = 240;
            std::istringstream(rest) >> limit;
            if (!inGame())
                g_atReadySince = 0;
            else if (!g_atReadySince)
                g_atReadySince = GetTickCount();
            if (g_atReadySince && GetTickCount() - g_atReadySince > 5000)
            {
                std::ostringstream o;
                o << "ready: " << ou->player->getAllPlayerCharacters().size() << " player characters";
                atWrite("INFO", o.str());
                return true;
            }
            if (elapsed > DWORD(limit) * 1000)
            {
                atCheck(false, "wait_ready timed out");
                atFinish("not ready");
            }
            return false;
        }
        if (cmd == "wait")
        {
            float secs = 1.0f;
            std::istringstream(rest) >> secs;
            return elapsed >= DWORD(secs * 1000.0f);
        }
        if (cmd == "note")
        {
            atWrite("NOTE", rest);
            return true;
        }
        if (cmd == "speed")
        {
            float x = 1.0f;
            std::istringstream(rest) >> x;
            ou->setGameSpeed(x, false);
            return true;
        }
        if (cmd == "screenshot")
        {
            atWrite(atScreenshot(rest) ? "SHOT" : "INFO", rest + ".png");
            return true;
        }
        if (cmd == "run_start") // run_start [east m] [south m]: the arena that far from your first character
        {
            float dx = ARENA_OFFSET_M, dz = 0.0f;
            std::istringstream(rest) >> dx >> dz;
            atCheck(runStart(dx, dz), "run started");
            return true;
        }
        if (cmd == "wait_fight") // wait_fight [secs]: until the current fight is won or lost
        {
            int limit = 300;
            std::istringstream(rest) >> limit;
            if (g_run.state == RUN_FIGHTING)
            {
                if (elapsed > DWORD(limit) * 1000)
                {
                    std::ostringstream o;
                    o << "fight undecided after " << limit << " s; runner " << health(g_run.runners[0]) << "; enemies:";
                    for (size_t i = 0; i < g_run.enemies.size(); ++i)
                        o << " [" << health(g_run.enemies[i]) << "]";
                    atCheck(false, o.str());
                    return true;
                }
                return false;
            }
            atWrite("INFO", g_run.lastResult);
            return true;
        }
        if (cmd == "next") // next: what the HUD button does between nodes
        {
            advance();
            atWrite("INFO", std::string("next: now ") + stateName(g_run.state) + ", node " +
                                (g_run.node >= 0 ? nodeLabel(g_run.current) : std::string("-")));
            return true;
        }
        if (cmd == "loot")
        {
            atWrite("INFO", "looted: " + lootEnemies());
            return true;
        }
        if (cmd == "runner") // runner: health, limbs, pack
        {
            Character* r = g_run.runners.empty() ? NULL : g_run.runners[0];
            atWrite("INFO", "runner: " + health(r));
            atWrite("INFO", "runner's pack: " + inventoryOf(r));
            return true;
        }
        if (cmd == "enemies")
        {
            std::ostringstream o;
            o << g_run.enemies.size() << " enemies:";
            for (size_t i = 0; i < g_run.enemies.size(); ++i)
                o << " [" << (g_run.enemies[i] ? g_run.enemies[i]->getName() : "?") << ": "
                  << health(g_run.enemies[i]) << "; carrying " << inventoryOf(g_run.enemies[i]) << "]";
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "select_runner") // the runner selected, so Kenshi's own panel shows their health
        {
            if (!g_run.runners.empty())
            {
                ou->player->unselectAll();
                ou->player->selectObject(g_run.runners[0], false);
                ou->player->objectSelected(g_run.runners[0], true);
                ou->player->focusCamera(g_run.runners[0]->getPosition());
            }
            return true;
        }
        if (cmd == "medraw") // medraw: every body part's fields as KenshiLib names them, to compare with Kenshi's panel
        {
            Character* r = g_run.runners.empty() ? NULL : g_run.runners[0];
            MedicalSystem* m = r ? r->getMedical() : NULL;
            for (int k = 0; m && k < m->getPartCount(); ++k)
            {
                MedicalSystem::HealthPartStatus* p = m->getPart((unsigned __int64)k);
                if (!p || !p->getData())
                    continue;
                std::ostringstream o;
                o << p->getData()->name << ": type " << int(p->whatAmI) << " flesh " << p->flesh << " stun "
                  << p->fleshStun << " bandaging " << p->bandaging << " max() " << p->maxHealth() << " _max "
                  << p->_maxHealth << " HPMult " << p->HPMult << " derived% " << p->derivedFleshHealthPercent
                  << " wear " << p->wearDamage << " raw:";
                const float* f = reinterpret_cast<const float*>(reinterpret_cast<const char*>(p) + 0x30);
                for (int j = 0; j < 14; ++j)
                    o << " " << f[j];
                atWrite("INFO", o.str());
            }
            return true;
        }
        if (cmd == "weapon") // weapon <stringId>: the starting weapon for runners made from now on
        {
            GameData* d = data(rest.c_str());
            atCheck(d != NULL, "starting weapon " + rest + (d ? " (" + d->name + ")" : " not found"));
            if (d)
                g_startWeapon = rest;
            return true;
        }
        if (cmd == "shop") // shop <town>: put that town on offer as the path's third choice (to test a shop)
        {
            static std::string town;
            town = rest;
            Node shop = {NODE_SHOP, NULL, NULL, 0, 0, 0, RUN_ROWS, town.c_str()};
            if (g_run.state == RUN_PATHING)
            {
                if (g_run.choices.size() >= 3)
                    g_run.choices.pop_back();
                g_run.choices.push_back(shop);
            }
            atCheck(g_run.state == RUN_PATHING && findTown(town) != NULL, "shop on offer: " + town);
            return true;
        }
        if (cmd == "towns") // towns: every town name and building count, for SHOP_TOWNS
        {
            std::ostringstream o;
            o << "towns:";
            if (shou && shou->townList)
            {
                lektor<RootObject*>& towns = shou->townList->getAllTowns();
                std::map<std::string, size_t> seen;
                for (uint32_t i = 0; i < towns.size(); ++i)
                {
                    TownBase* t = static_cast<TownBase*>(towns[i]);
                    TownBuildingsManager* mgr = t ? t->getBuildingsManager() : NULL;
                    size_t n = mgr ? mgr->buildingEntities.size() : 0;
                    if (t && n > seen[t->getName()])
                        seen[t->getName()] = n;
                }
                for (std::map<std::string, size_t>::iterator i = seen.begin(); i != seen.end(); ++i)
                    if (i->second >= 5)
                        o << " " << i->first << " (" << i->second << ");";
            }
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "path") // path: the choices on offer
        {
            std::ostringstream o;
            o << "row " << g_run.node << ", state " << stateName(g_run.state) << "; offered:";
            for (size_t i = 0; i < g_run.choices.size(); ++i)
                o << " [" << i << "] " << nodeLabel(g_run.choices[i]) << ";";
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "go") // go <i|fight|rest|elite>: what clicking that path choice does (a kind: the first of it, if offered)
        {
            size_t pickI = g_run.choices.size();
            if (rest == "fight" || rest == "rest" || rest == "elite" || rest == "shop")
            {
                NodeKind want = rest == "fight" ? NODE_FIGHT : rest == "rest" ? NODE_REST
                                : rest == "elite" ? NODE_ELITE : NODE_SHOP;
                for (size_t i = 0; i < g_run.choices.size() && pickI == g_run.choices.size(); ++i)
                    if (g_run.choices[i].kind == want)
                        pickI = i;
                if (pickI == g_run.choices.size())
                    pickI = 0; // not offered: the first choice
            }
            else
                std::istringstream(rest) >> pickI;
            bool ok = g_run.state == RUN_PATHING && pickI < g_run.choices.size();
            atCheck(ok, "go " + rest + (ok ? " -> " + nodeLabel(g_run.choices[pickI]) : std::string("")));
            if (ok)
                goPath(pickI);
            return true;
        }
        if (cmd == "trials") // trials <encounter> <times> [secs]: that fight again and again, a fresh runner each bout
        {
            static int node = 0, left = 0, won = 0, lost = 0, undecided = 0, secs = 180, wonBefore = 0;
            static bool running = false, inBout = false;
            static std::string times;
            if (!running)
            {
                int n = 5;
                node = 0;
                secs = 180;
                std::istringstream(rest) >> node >> n >> secs;
                if (node < 0 || node >= ENCOUNTER_COUNT || g_run.runners.empty())
                {
                    atCheck(false, "trials: node " + rest + " is not a fight, or no run is going");
                    return true;
                }
                left = n;
                won = lost = undecided = 0;
                times.clear();
                running = true;
                inBout = false;
            }
            if (inBout)
            {
                DWORD took = (GetTickCount() - g_run.fightStart) / 1000;
                if (g_run.state == RUN_FIGHTING)
                {
                    if (took < DWORD(secs))
                        return false;
                    ++undecided;
                    times += " " + toStr(float(took)) + "?";
                }
                else if (g_run.state == RUN_LOST)
                {
                    ++lost;
                    times += " " + toStr(float(took)) + "L";
                }
                else if (g_run.fightsWon > wonBefore)
                {
                    ++won;
                    times += " " + toStr(float(took)) + "W";
                }
                else
                {
                    ++undecided; // no enemies appeared
                    times += " " + toStr(float(took)) + "?";
                }
                inBout = false;
            }
            if (left <= 0)
            {
                running = false;
                std::ostringstream o;
                o << "trials of " << ENCOUNTERS[node].label << " with " << g_startWeapon << ": " << won << " won, " << lost
                  << " lost, " << undecided << " undecided (secs and result:" << times << ")";
                atWrite("INFO", o.str());
                return true;
            }
            // A fresh bout: the last one's people gone, a new runner at the arena.
            --left;
            for (size_t i = 0; i < g_run.enemies.size(); ++i)
                if (g_run.enemies[i])
                    ou->destroy(g_run.enemies[i], false, "Kenshi Rogue: trials");
            g_run.enemies.clear();
            for (size_t i = 0; i < g_run.runners.size(); ++i)
                if (g_run.runners[i])
                    ou->destroy(g_run.runners[i], false, "Kenshi Rogue: trials");
            g_run.runners.clear();
            g_run.fleshCap.clear(); // a new runner may land at an old one's addresses
            removeAnkh();
            g_run.ankhUntil = 0;
            Character* r = createRunner(g_run.arena);
            if (!r)
            {
                running = false;
                atCheck(false, "trials: could not make a runner");
                return true;
            }
            g_run.runners.push_back(r);
            wonBefore = g_run.fightsWon;
            startFight(ENCOUNTERS[node]);
            inBout = true;
            return false;
        }
        if (cmd == "regen") // regen on|off: natural healing for runners (off is the mode's rule); reports flesh clipped so far
        {
            if (rest == "on" || rest == "off")
                g_noRegen = rest == "off";
            atWrite("INFO", std::string("natural healing ") + (g_noRegen ? "OFF" : "on") + "; flesh clipped so far " +
                                toStr(g_regenClipped));
            return true;
        }
        if (cmd == "ankh") // ankh: is one up, where, how far the runner is, time left; cats so far
        {
            std::ostringstream o;
            if (g_run.ankh)
            {
                Ogre::Vector3 a = g_run.ankh->getPosition();
                o << "ankh up at " << int(a.x) << "," << int(a.y) << "," << int(a.z);
                if (!g_run.runners.empty())
                    o << ", runner " << g_run.runners[0]->getPosition().distance(a) / METRE << " m away";
            }
            else
                o << "no ankh";
            if (g_run.ankhUntil)
                o << "; " << (g_run.ankhUntil > g_gameClock ? int(g_run.ankhUntil - g_gameClock) : 0)
                  << " game s left";
            int* money = factionMoney(ou->player->getFaction());
            o << "; cats earned this run " << g_run.catsEarned << ", faction has " << (money ? *money : -1);
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "worship") // worship: the runner told to use the ankh, as a right-click Use would (job 87, as in Kenshi Town's test)
        {
            if (!g_run.ankh || g_run.runners.empty())
            {
                atCheck(false, "worship: no ankh or no runner");
                return true;
            }
            g_run.runners[0]->addJob(TaskType(87), g_run.ankh, false, true, g_run.ankh->getPosition());
            atWrite("INFO", "worship: runner given the ankh's use");
            return true;
        }
        if (cmd == "ankh_timer") // ankh_timer <secs>: the soft timer after a node, from the next ankh on
        {
            int secs = 180;
            std::istringstream(rest) >> secs;
            AFTER_NODE_SECONDS = DWORD(secs);
            atWrite("INFO", "ankh timer " + toStr(float(secs)) + " s");
            return true;
        }
        if (cmd == "bandage") // bandage: every wounded part of the runner treated, as first aid would
        {
            Character* r = g_run.runners.empty() ? NULL : g_run.runners[0];
            MedicalSystem* m = r ? r->getMedical() : NULL;
            for (int k = 0; m && k < m->getPartCount(); ++k)
                if (MedicalSystem::HealthPartStatus* p = m->getPart((unsigned __int64)k))
                    p->bandaging = p->maxHealth();
            atWrite("INFO", "bandaged: " + health(r));
            return true;
        }
        if (cmd == "look_ankh") // look_ankh: Kenshi's own framing of the ankh (focusCameraOnObject)
        {
            CameraClass* cam = ou->player->getCamera();
            if (g_run.ankh && cam)
                cam->focusCameraOnObject(g_run.ankh, Ogre::Vector3::ZERO, false);
            atWrite("INFO", g_run.ankh ? "camera on the ankh" : "look_ankh: no ankh");
            return true;
        }
        if (cmd == "pause") // pause on|off: Kenshi's own user pause
        {
            ou->userPause(rest != "off");
            return true;
        }
        if (cmd == "clock") // clock: game seconds counted so far, against real time
        {
            static double lastGame = 0.0;
            static DWORD lastReal = 0;
            std::ostringstream o;
            o << "game clock " << g_gameClock << " s (speed x" << ou->getFrameSpeedMultiplier()
              << (ou->isPaused() ? ", PAUSED" : "") << ")";
            if (lastReal)
                o << "; since last `clock`: " << g_gameClock - lastGame << " game s in "
                  << (GetTickCount() - lastReal) / 1000.0 << " real s";
            lastGame = g_gameClock;
            lastReal = GetTickCount();
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "boons") // boons on|off: whether run_start offers the choice (off by default in scenarios); with no argument, what's on offer
        {
            if (rest == "on" || rest == "off")
                g_atBoons = rest == "on";
            std::ostringstream o;
            o << "boons " << (g_atBoons ? "on" : "off") << "; offered:";
            for (size_t i = 0; i < g_offer.size(); ++i)
                o << " [" << i << "] " << g_offer[i].label << ";";
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "choose") // choose <i>: what clicking that boon does
        {
            size_t i = 0;
            std::istringstream(rest) >> i;
            atCheck(g_run.state == RUN_CHOOSING && i < g_offer.size(), "choose " + rest);
            choose(i);
            return true;
        }
        if (cmd == "stats") // stats: the runner's combat stats as stored
        {
            Character* r = g_run.runners.empty() ? NULL : g_run.runners[0];
            CharStats* st = r ? r->getStats() : NULL;
            std::ostringstream o;
            o << "runner's stats:";
            const StatsEnumerated show[] = {STAT_STRENGTH, STAT_MELEE_ATTACK, STAT_MELEE_DEFENCE, STAT_TOUGHNESS,
                                            STAT_DEXTERITY, STAT_SABRES, STAT_KATANAS, STAT_HACKERS, STAT_BLUNT,
                                            STAT_HEAVYWEAPONS};
            for (int k = 0; st && k < 10; ++k)
                o << " " << CharStats::getStatName(show[k]) << " " << st->getStatRef(show[k]) << ";";
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "people") // people: your characters, where they are, how far from the arena
        {
            const lektor<Character*>& ps = ou->player->getAllPlayerCharacters();
            std::ostringstream o;
            o << ps.size() << " player characters:";
            for (uint32_t i = 0; i < ps.size(); ++i)
            {
                Ogre::Vector3 p = ps[i]->getPosition();
                o << " [" << ps[i]->getName() << " at " << int(p.x) << "," << int(p.y) << "," << int(p.z) << ", "
                  << int(p.distance(g_run.arena) / METRE) << " m from the arena, " << (out(ps[i]) ? "out" : "up") << "]";
            }
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "focus") // camera on the runner
        {
            if (!g_run.runners.empty())
                ou->player->focusCamera(g_run.runners[0]->getPosition());
            return true;
        }
        if (cmd == "wound") // wound <amount>: take that much off every part of the runner (tests rest)
        {
            float amount = 30.0f;
            std::istringstream(rest) >> amount;
            Character* r = g_run.runners.empty() ? NULL : g_run.runners[0];
            MedicalSystem* m = r ? r->getMedical() : NULL;
            for (int k = 0; m && k < m->getPartCount(); ++k)
                if (MedicalSystem::HealthPartStatus* p = m->getPart((unsigned __int64)k))
                {
                    p->flesh -= amount;
                    p->updateDerivedHealths();
                }
            atWrite("INFO", "wounded: " + health(r));
            return true;
        }
        if (cmd == "strangers")
        {
            std::ostringstream o;
            o << g_run.strangersRemoved << " strangers removed from the arena";
            atWrite("INFO", o.str());
            return true;
        }
        if (cmd == "expect") // expect state <name> | fights_won <n> | enemies <n> | runner_up
        {
            std::istringstream e(rest);
            std::string what, want;
            e >> what >> want;
            if (what == "state")
                atCheck(want == stateName(g_run.state), "state is " + std::string(stateName(g_run.state)) +
                                                            " (want " + want + ")");
            else if (what == "fights_won")
                atCheck(atoi(want.c_str()) == g_run.fightsWon, "fights won " + toStr(float(g_run.fightsWon)) +
                                                                   " (want " + want + ")");
            else if (what == "enemies")
                atCheck(atoi(want.c_str()) == int(g_run.enemies.size()),
                        "enemies " + toStr(float(g_run.enemies.size())) + " (want " + want + ")");
            else if (what == "runner_up")
                atCheck(!g_run.runners.empty() && !out(g_run.runners[0]), "runner is up");
            else if (what == "carrying") // expect carrying <item name...>
            {
                std::string item;
                std::getline(e, item);
                item = want + item;
                std::string pack = g_run.runners.empty() ? "" : inventoryOf(g_run.runners[0]);
                atCheck(pack.find(item) != std::string::npos, "runner carries " + item);
            }
            else
                atCheck(false, "unknown expect: " + rest);
            return true;
        }
        if (cmd == "stop_if_lost") // stop_if_lost: end the scenario here if the run is lost
        {
            if (g_run.state == RUN_LOST)
                atFinish("run lost at row " + toStr(float(g_run.node)));
            return true;
        }
        if (cmd == "quit")
        {
            atFinish("quit");
            return true;
        }
        atCheck(false, "unknown command: " + line);
        return true;
    }

    bool atOnLoadStep()
    {
        return g_atPc < g_atLines.size() && g_atLines[g_atPc].compare(0, 5, "load ") == 0;
    }

    void atRunStep()
    {
        if (!g_atStepBegun)
        {
            g_atStepBegun = true;
            g_atStepStart = GetTickCount();
            atWrite("STEP", g_atLines[g_atPc]);
        }
        if (atStep(g_atLines[g_atPc]))
        {
            ++g_atPc;
            g_atStepBegun = false;
        }
    }

    void autotestTick()
    {
        if (!g_atChecked)
            atLoadScenario();
        if (!g_atActive || atOnLoadStep())
            return;
        if (SaveManager* sm = SaveManager::getSingleton())
            *reinterpret_cast<float*>(reinterpret_cast<char*>(sm) + 0x118) = 0.0f; // no autosaves during a test
        if (g_atPc >= g_atLines.size())
        {
            atFinish("end of scenario");
            return;
        }
        atRunStep();
    }

    // The game loop doesn't tick at the main menu, so `load` runs from MyGUI's
    // frame event (learned in kenshi-town, 2026-11-22).
    void atMenuFrame(float)
    {
        if (!g_atChecked)
            atLoadScenario();
        if (!g_atActive || !atOnLoadStep())
            return;
        ++g_atFramesSeen;
        atRunStep();
    }

    bool g_atFramesAdded = false;

    DWORD WINAPI atWaitForGui(LPVOID)
    {
        for (int i = 0; i < 1200 && !g_atFramesAdded; ++i)
        {
            if (MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr())
            {
                Sleep(500);
                gui->eventFrameStart += MyGUI::newDelegate(atMenuFrame);
                g_atFramesAdded = true;
                logLine("[autotest] MyGUI frame handler added");
                return 0;
            }
            Sleep(100);
        }
        ErrorLog("KenshiRogue: [autotest] MyGUI never started");
        return 0;
    }

    void addAutotestFrames()
    {
        FILE* f = fopen(AT_SCENARIO, "r");
        logLine(std::string("[autotest] ") + (f ? "scenario found" : "no scenario"));
        if (!f)
            return;
        fclose(f);
        CreateThread(NULL, 0, atWaitForGui, NULL, 0, NULL);
    }

    // ==== Main loop ===============================================================

    void (*mainLoop_orig)(GameWorld*, float) = NULL;

    void mainLoop_hook(GameWorld* world, float time)
    {
        mainLoop_orig(world, time);
        ++g_frame;
        // The loop's step is real time (paused, 1x and 5x all counted 5 s in 5
        // real seconds, 2026-11-23), so scale it by Kenshi's speed, and stop
        // while paused.
        if (time > 0.0f && time < 10.0f && !world->isPaused())
            g_gameClock += double(time) * world->getFrameSpeedMultiplier();
        if (!inGame())
        {
            if (g_hud)
                g_hud->setVisible(false);
            return;
        }
        keepParking();
        holdWounds();
        checkFight();
        watchAnkh();
        keepArenaQuiet();
        updateHud();
        autotestTick();
    }
}

__declspec(dllexport) void startPlugin()
{
    logLine("loading");
    addAutotestFrames();
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            mainLoop_hook, &mainLoop_orig))
    {
        ErrorLog("KenshiRogue: could not hook the main loop");
        return;
    }
    g_createChar = (CreateCharFn)KenshiLib::GetRealAddress(&RootObjectFactory::createRandomCharacter);
    g_createBuilding = (CreateBuildingFn)KenshiLib::GetRealAddress(&RootObjectFactory::createBuilding);
    g_handCtor = (HandCtorFn)KenshiLib::GetRealAddress(
        static_cast<hand* (hand::*)(unsigned int, unsigned int, itemType, unsigned int, unsigned int)>(
            &hand::_CONSTRUCTOR));
    g_createItem = (CreateItemFn)KenshiLib::GetRealAddress(
        static_cast<Item* (RootObjectFactory::*)(GameData*, const hand&, GameData*, GameData*, int, Faction*)>(
            &RootObjectFactory::createItem));
    logLine("ready");
}
