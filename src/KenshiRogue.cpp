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
#include <kenshi/SaveManager.h>
#include <kenshi/Enums.h>
#include <kenshi/util/hand.h>

#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_Button.h>
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
    const char* ENEMY_FACTION = "1305-gamedata.base";    // Starving Bandits

    // Starting weapon: a Horse Chopper, Rusted Junk grade.
    const char* START_WEAPON = "475-gamedata.base";      // Horse Chopper
    const char* GRADE_MAKER = "912-gamedata.base";       // "Unknown" maker, the one with Rusted Junk
    const char* GRADE_RUSTED_JUNK = "913-gamedata.base"; // MaterialSpecsWeapon "Rusted Junk"
    const char* FIRST_AID = "209-gamedata.base";         // Basic First Aid Kit

    const float METRE = 10.0f;               // world units per metre
    const float ARENA_OFFSET_M = 5.0f;       // the runner appears this far east of your first character
    const float SPAWN_DISTANCE_M = 25.0f;    // enemies appear this far from the runner
    const float ARENA_RADIUS_M = 120.0f;     // strangers inside this during a fight are removed

    // The runner's fixed stats (everything else stays at the template's).
    struct FixedStat
    {
        StatsEnumerated stat;
        float value;
    };
    const FixedStat RUNNER_STATS[] = {
        {STAT_STRENGTH, 10}, {STAT_MELEE_ATTACK, 10}, {STAT_MELEE_DEFENCE, 10}, {STAT_TOUGHNESS, 10},
        {STAT_DEXTERITY, 10}, {STAT_ATHLETICS, 10}, {STAT_HACKERS, 10}, {STAT_DODGE, 5},
    };

    typedef RootObject* (__fastcall* CreateCharFn)(RootObjectFactory* self, Faction* faction,
                                                   Ogre::Vector3 position, RootObjectContainer* owner,
                                                   GameData* characterTemplate, Building* home, float age);
    typedef Item* (__fastcall* CreateItemFn)(RootObjectFactory* self, GameData* gd, const hand* handle,
                                             GameData* weaponMesh, GameData* matData, int levelOverride,
                                             Faction* flagUniform);
    CreateCharFn g_createChar = NULL;
    CreateItemFn g_createItem = NULL;

    int g_frame = 0;

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

    Item* makeItem(GameData* d, GameData* maker, GameData* grade)
    {
        if (!d || !g_createItem || !ou->theFactory)
            return NULL;
        // Which of createItem's two GameData arguments takes the maker and
        // which the grade isn't documented; try both, then neither, and say
        // which worked (the first run, 2026-11-23, made nothing).
        hand none;
        Item* it = g_createItem(ou->theFactory, d, &none, maker, grade, -1, NULL);
        const char* how = "maker, grade";
        if (!it && (maker || grade))
        {
            it = g_createItem(ou->theFactory, d, &none, grade, maker, -1, NULL);
            how = "grade, maker";
        }
        if (!it && (maker || grade))
        {
            it = g_createItem(ou->theFactory, d, &none, NULL, NULL, -1, NULL);
            how = "no grade";
        }
        if (maker || grade)
            logLine(std::string("makeItem ") + d->name + ": " + (it ? it->getName() + " (" + how + ")" : "NOTHING"));
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

    enum NodeKind { NODE_FIGHT, NODE_REST };
    struct Node
    {
        NodeKind kind;
        const char* squad; // fights
        int firstAidKits;  // fights: kits each enemy carries
        const char* label;
    };
    // The first slice. Fight 1 is always the single starving bandit.
    const Node NODES[] = {
        {NODE_FIGHT, SQUAD_ONE_BANDIT, 1, "Fight 1: a starving bandit"},
        {NODE_REST, NULL, 0, "Rest"},
        {NODE_FIGHT, SQUAD_TWO_BANDITS, 0, "Fight 2: two starving bandits"},
    };
    const int NODE_COUNT = sizeof(NODES) / sizeof(NODES[0]);

    enum RunState { RUN_NONE, RUN_FIGHTING, RUN_BETWEEN, RUN_WON, RUN_LOST };

    struct Run
    {
        RunState state;
        int node;                       // the node being played or just finished
        Ogre::Vector3 arena;
        std::vector<Character*> runners;
        std::vector<Character*> enemies;
        DWORD fightStart;
        int fightsWon;
        int strangersRemoved;
        std::string lastResult;
        Run() : state(RUN_NONE), node(-1), fightStart(0), fightsWon(0), strangersRemoved(0) {}
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
        o << ", blood " << int(m->blood) << "/" << int(m->getMaxBlood()) << ";";
        for (int i = 0; i < m->getPartCount(); ++i)
        {
            MedicalSystem::HealthPartStatus* p = m->getPart((unsigned __int64)i);
            if (!p || !p->getData())
                continue;
            o << " " << p->getData()->name << " " << int(p->flesh) << "/" << int(p->maxHealth());
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

    // Arena hygiene: anyone hostile who isn't in the fight is removed while
    // it's on, so a passing patrol can't decide it (raids ended Kenshi Town
    // tests the same way). The design's "world spawns off nearby", for now.
    void keepArenaQuiet()
    {
        if (g_run.state != RUN_FIGHTING || g_run.runners.empty() || g_frame % 60 != 0)
            return;
        Character* r = g_run.runners[0];
        lektor<RootObject*> found;
        ou->getCharactersWithinSphere(found, g_run.arena, ARENA_RADIUS_M * METRE, ARENA_RADIUS_M * METRE, 0.0f,
                                      80, 80, NULL);
        for (uint32_t i = 0; i < found.size(); ++i)
        {
            Character* c = static_cast<Character*>(found[i]);
            if (!c || contains(g_run.runners, c) || contains(g_run.enemies, c) || c->isDead())
                continue;
            if (c->getFaction() == r->getFaction() || !c->isEnemy(r, false))
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
        Item* w = makeItem(data(START_WEAPON), data(GRADE_MAKER), data(GRADE_RUSTED_JUNK));
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

    void startFight(const Node& n)
    {
        g_run.enemies.clear();
        Faction* them = ou->factionMgr->getFactionByStringID(ENEMY_FACTION);
        GameData* squad = data(n.squad);
        if (!them || !squad)
        {
            logLine(std::string("fight: missing ") + (them ? "the squad " : "the faction ") + n.squad);
            g_run.state = RUN_LOST;
            g_run.lastResult = "the encounter's data is missing (is KenshiRogue.mod enabled?)";
            return;
        }
        Ogre::Vector3 at = g_run.runners[0]->getPosition();
        at.x += SPAWN_DISTANCE_M * METRE;
        // The runner as the squad's AI target: spawned without one, the
        // bandit stood 25 m away and never came (first run, 2026-11-23).
        Platoon* made = ou->theFactory->createRandomSquad(them, at, NULL, 20, NULL, squad, NULL, NULL, NULL, false,
                                                          g_run.runners[0]->getHandle(), NULL, 1.0f, SQ_ROAMING,
                                                          false);
        // The squad's people: that faction, right where it was made.
        lektor<RootObject*> found;
        ou->getCharactersWithinSphere(found, at, 30.0f * METRE, 30.0f * METRE, 0.0f, 40, 40, NULL);
        for (uint32_t i = 0; i < found.size(); ++i)
        {
            Character* c = static_cast<Character*>(found[i]);
            if (c && c->getFaction() == them && !c->isDead())
                g_run.enemies.push_back(c);
        }
        GameData* kit = data(FIRST_AID);
        for (size_t i = 0; i < g_run.enemies.size(); ++i)
            for (int k = 0; k < n.firstAidKits; ++k)
                if (Item* it = makeItem(kit, NULL, NULL))
                    g_run.enemies[i]->giveItem(it, false, true);
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
                p->fleshStun = p->maxHealth();
                p->updateDerivedHealths();
            }
            m->blood = m->getMaxBlood();
            m->validateHealthValues();
            logLine("rest: " + c->getName() + " now " + health(c));
        }
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
        if (next >= NODE_COUNT)
        {
            g_run.state = RUN_WON;
            g_run.lastResult = "run complete";
            logLine("run complete");
            return;
        }
        g_run.node = next;
        const Node& n = NODES[next];
        if (n.kind == NODE_REST)
        {
            rest();
            g_run.lastResult = "rested";
            g_run.state = RUN_BETWEEN;
            return;
        }
        startFight(n);
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
        g_run.state = RUN_BETWEEN; // node -1: advance() plays node 0
        ou->player->focusCamera(g_run.arena);
        advance();
        return true;
    }

    void checkFight()
    {
        if (g_run.state != RUN_FIGHTING || g_frame % 15 != 0)
            return;
        bool enemiesOut = true, runnersOut = true;
        for (size_t i = 0; i < g_run.enemies.size(); ++i)
            enemiesOut = enemiesOut && out(g_run.enemies[i]);
        for (size_t i = 0; i < g_run.runners.size(); ++i)
            runnersOut = runnersOut && out(g_run.runners[i]);
        if (!enemiesOut && !runnersOut)
            return;
        std::ostringstream o;
        o << NODES[g_run.node].label << ": " << (runnersOut ? "LOST" : "WON") << " in "
          << (GetTickCount() - g_run.fightStart) / 1000 << " s; runner " << health(g_run.runners[0]);
        g_run.lastResult = o.str();
        logLine(g_run.lastResult);
        if (runnersOut)
            g_run.state = RUN_LOST;
        else
        {
            ++g_run.fightsWon;
            g_run.state = RUN_BETWEEN;
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
            return std::string("Rogue: ") + NODES[g_run.node].label;
        case RUN_BETWEEN:
            return g_run.node + 1 < NODE_COUNT ? std::string("Rogue: next, ") + NODES[g_run.node + 1].label
                                               : "Rogue: finish run";
        case RUN_WON:
            return "Rogue: run complete";
        case RUN_LOST:
            return "Rogue: run lost";
        }
        return "Rogue";
    }

    void onHudClick(MyGUI::WidgetPtr)
    {
        if (g_run.state == RUN_NONE || g_run.state == RUN_WON || g_run.state == RUN_LOST)
            runStart(ARENA_OFFSET_M, 0.0f);
        else if (g_run.state == RUN_FIGHTING && !g_run.runners.empty())
            ou->player->focusCamera(g_run.runners[0]->getPosition());
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
    bool g_atActive = false;
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
        const char* n[] = {"none", "fighting", "between", "won", "lost"};
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
                                (g_run.node >= 0 && g_run.node < NODE_COUNT ? NODES[g_run.node].label : "-"));
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
        if (!inGame())
        {
            if (g_hud)
                g_hud->setVisible(false);
            return;
        }
        checkFight();
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
    g_createItem = (CreateItemFn)KenshiLib::GetRealAddress(
        static_cast<Item* (RootObjectFactory::*)(GameData*, const hand&, GameData*, GameData*, int, Faction*)>(
            &RootObjectFactory::createItem));
    logLine("ready");
}
