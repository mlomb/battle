// Fall Challenge 2020 (the witches' potion brewing game) - headless C++ referee, port of
// https://github.com/CodinGame/FallChallenge2020 (commit 5c0cbf3, 2020-11-20, "Fixed ref presence
// check"), CodinGame game engine 3.15.4, 2 players, league 19 (the CLI sets the league to 19: the
// "Bronze+" rules, with the tome, the spells, the potion bonuses and the inventory bonus).
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints "<p0score>\n<p1score>\nseed=<N>\n" to stdout like the jar, or "null\nnull\n" (exit code 0)
// when the Java referee would have failed (see below). Exit code 1 without output when the jar's
// command line interface fails (bad -seed, a bot that cannot be started, more than 2 players).
// -l writes the trace of common/engine.h (one line per bot execution, view frame, game summary and
// tooltip block, then "END OK|<why the game failed>").
//
// Engine 3.15.4: its core and runner classes are those of 4.0.2-4.5.0 (what common/engine.h models
// by default) except that 3.x has no getRandom() (the game makes its own java.util.Random from the
// seed) and that the runner words one failure message differently ("Error reading Referee command.
// Buffer capacity: ..."): only the END line of the -l trace shows that one. The jar is compiled by a
// JDK 8 javac (Game#getSpellById does not compile with a recent javac), see referees/cg-builder.
//
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption,
// int semantics, error texts. Notable Java behaviours reproduced here:
//   - new java.util.Random(seed) shuffles the 42 tome spells, then the 36 potions
//     (Collections.shuffle of LinkedLists: the same permutation as for an ArrayList). Spell ids come
//     from a static counter in creation order: tome spells 0-41, potions 42-77, the 4 base spells of
//     player 0 then player 1 (78-85), then every learnt spell (86+) in the order they are learnt.
//   - Commands: the CASE_INSENSITIVE regexes of CommandManager, matched on the whole line (only ASCII
//     letters fold, \s is [ \t\n\x0B\f\r], \d is [0-9], '.' stops at line terminators), checked in the
//     order REST, CAST, LEARN, BREW, WAIT, with their backtracking ("CAST 1 2x" is CAST 1 with the
//     message "2x"; "WAIT " has the empty message), messages cut to 46 chars + "...". An unknown command
//     or an Integer.valueOf overflow disqualifies the bot (score -1, tooltip, summary).
//   - The rules in their order (both players' actions on the same state, the tome and potions
//     refilled afterwards, then the bonuses slide), with the referee's quirks: the tax paid when
//     learning goes to the tome positions as they were before the learnt spells are removed, the
//     learning summary is computed with the inventory after learning, two players brewing the same
//     potion in the same turn both get it (and its bonus, decremented twice), int overflows of
//     CAST <id> <huge repeat> (canAfford/enoughSpace, the inventory after repeats additions).
//     The HashSets of learnt spells and of deliveries (identity hash codes) are iterated in an
//     order that changes nothing.
//   - The view data (Serializer: events with their animation timings, spells, inventories, stocks
//     and bonuses in HashMap order, messages; the list of new tome spells of the viewer events is
//     never cleared and grows all game long) and the frame durations: only their size matters to the
//     engine, but they are produced in full and traced (CG_TRACE_VIEWS) for validation.
//   - Engine (common/engine.h): the protocol between the runner and the referee (java.util.Scanner
//     lines, 30000-byte commands: a long invalid command makes a too big summary or tooltip and the
//     game fails), the 1 MB view data quota, the game summary quotas; the bots get 1000 ms on their
//     first turn and 50 ms after.
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps, so a
// bot answering in ~50 ms may be timed out by one and not the other; on a loaded machine the jar
// times out bots that answered in time). A CAST whose repeat count makes an animation list of more
// than 100000 entries (int overflow of loss/gain * repeats: e.g. a learnt (-2, 0, 1, 0) cast with any
// repeat over 2^30 passes the checks and makes a list of that many entries) fails the game at once
// here: the jar runs out of memory (checked: OutOfMemoryError after 151 s, "null" scores) or sends a
// frame over the runner's buffer, so its game fails too. More than 2 players: refused here (exit code 1), the
// jar would start them and fail at the first turn.
// Validated (2026-10-25) with battle referee-diff against the jar on 3777 games (30 bots, every ordered
// pairing, seeds from 1, 1000001, 987654321 and 5000000000, plus the int-overflow bot against all on
// three more ranges; 2 games differed under load because the jar and then the candidate timed out bots
// that had answered, both identical when replayed alone) and in direct mode (jar game log vs -l trace,
// every view frame compared) on 685 games, including raw line endings, invalid UTF-8, U+2028/U+0085, an
// over-sized summary and a 2^31-entry animation list (the jar's OutOfMemoryError).

#include "common/engine.h"

#include <deque>

using java::String;

// ----------------------------------------------------------------------------
// Recipe.java
// ----------------------------------------------------------------------------
struct Recipe {
    i32 delta[4] = {0, 0, 0, 0};
    Recipe() {}
    Recipe(i32 a, i32 b, i32 c, i32 d) {
        delta[0] = a;
        delta[1] = b;
        delta[2] = c;
        delta[3] = d;
    }
    std::string toPlayerString() const {
        return std::to_string(delta[0]) + " " + std::to_string(delta[1]) + " " + std::to_string(delta[2]) + " " + std::to_string(delta[3]);
    }
    void add(int idx, i32 x) { delta[idx] = java::add(delta[idx], x); }
    // IntStream.sum: int arithmetic
    i32 getTotal() const {
        i32 s = 0;
        for (i32 v : delta) s = java::add(s, v);
        return s;
    }
    i32 getTotalLoss() const {
        i32 s = 0;
        for (i32 v : delta)
            if (v < 0) s = java::add(s, v);
        return java::sub(0, s);
    }
    i32 getTotalGain() const {
        i32 s = 0;
        for (i32 v : delta)
            if (v > 0) s = java::add(s, v);
        return s;
    }
};

// ----------------------------------------------------------------------------
// spell/Spell.java, TomeSpell.java, DeliverySpell.java, PlayerSpell.java
// ----------------------------------------------------------------------------
enum SpellClass { TOME_SPELL, DELIVERY_SPELL, PLAYER_SPELL };
enum SpellType { CAST, OPPONENT_CAST, LEARN, BREW };
static const char* SPELL_TYPE_NAME[4] = {"CAST", "OPPONENT_CAST", "LEARN", "BREW"};

struct Spell {
    i32 id = 0;
    SpellClass cls = TOME_SPELL;
    Recipe recipe;
    i32 stock = 0;           // TomeSpell
    bool repeatable = false; // TomeSpell, PlayerSpell
    i32 score = 0;           // DeliverySpell
    bool active = false;     // PlayerSpell
    int owner = -1;          // PlayerSpell: the owner's index

    i32 getId() const { return id; }
    i32 getScore() const { return cls == DELIVERY_SPELL ? score : 0; }
    i32 getStock() const { return cls == TOME_SPELL ? stock : -1; }
    bool isActive() const { return cls == PLAYER_SPELL && active; }
    bool isRepeatable() const { return cls == DELIVERY_SPELL ? false : repeatable; }
};

// Spell.INSTANCE_COUNT: every spell ever created, the id is the index
static std::vector<Spell> allSpells;

static int newSpell(SpellClass cls, const Recipe& recipe) {
    Spell s;
    s.id = (i32)allSpells.size();
    s.cls = cls;
    s.recipe = recipe;
    allSpells.push_back(s);
    return s.id;
}
static int newTomeSpell(const Recipe& recipe) {
    int id = newSpell(TOME_SPELL, recipe);
    Spell& s = allSpells[id];
    s.stock = 0;
    s.repeatable = false;
    for (i32 x : recipe.delta)
        if (x < 0) s.repeatable = true;
    return id;
}
static int newDeliverySpell(const Recipe& need, i32 score) {
    int id = newSpell(DELIVERY_SPELL, Recipe(-need.delta[0], -need.delta[1], -need.delta[2], -need.delta[3]));
    allSpells[id].score = score;
    return id;
}
static int newPlayerSpell(const Recipe& recipe, int owner, bool repeatable) {
    int id = newSpell(PLAYER_SPELL, recipe);
    Spell& s = allSpells[id];
    s.active = true;
    s.owner = owner;
    s.repeatable = repeatable;
    return id;
}

// ----------------------------------------------------------------------------
// action/Action.java, ResetAction.java, SpellAction.java, WaitAction.java
// ----------------------------------------------------------------------------
enum ActionKind { NO_ACTION, RESET_ACTION, SPELL_ACTION, WAIT_ACTION };

struct Action {
    ActionKind kind = NO_ACTION;
    std::string str = "NO_ACTION";
    i32 spellId = 0;
    bool hasParam = false; // SpellAction's Optional<Integer> param
    i32 param = 0;

    bool isSpell() const { return kind == SPELL_ACTION; }
    bool isReset() const { return kind == RESET_ACTION; }
    bool isWait() const { return kind == WAIT_ACTION; }
    i32 getRepeats() const { return kind == SPELL_ACTION && hasParam ? param : 1; }
};

// ----------------------------------------------------------------------------
// Player.java
// ----------------------------------------------------------------------------
namespace Game {
static const int READ_AHEAD_COST = 1;
static const int STARTING_SCORE = 0;
static const int MAX_SPACE = 10;
static const int INGREDIENT_TYPE_COUNT = 4;
static const int COUNTER_SIZE = 5;
static const int TOME_SIZE = 6;
static const int DELIVERY_GOAL = 6; // league 3+ ("Bronze+")
} // namespace Game

struct Player : cg::Player {
    Recipe inventory;
    std::vector<int> spells; // PlayerSpell ids
    Action action;
    i32 deliveriesCount = 0;
    bool hasMessage = false;
    String message;

    int getExpectedOutputLines() const override { return 1; }

    void initSpells() {
        spells.push_back(newPlayerSpell(Recipe(2, 0, 0, 0), index, false));
        spells.push_back(newPlayerSpell(Recipe(-1, 1, 0, 0), index, false));
        spells.push_back(newPlayerSpell(Recipe(0, -1, 1, 0), index, false));
        spells.push_back(newPlayerSpell(Recipe(0, 0, -1, 1), index, false));
    }
    void reset() {
        action = Action();
        hasMessage = false;
        message.clear();
    }
    bool canAfford(const Recipe& recipe, i32 repeats) const {
        for (int i = 0; i < Game::INGREDIENT_TYPE_COUNT; ++i)
            if (java::add(inventory.delta[i], java::mul(recipe.delta[i], repeats)) < 0) return false;
        return true;
    }
    bool enoughSpace(const Recipe& recipe, i32 repeats) const {
        return java::add(java::mul(recipe.getTotal(), repeats), inventory.getTotal()) <= Game::MAX_SPACE;
    }
    bool canDeliver(const Recipe& need) const {
        for (int i = 0; i < Game::INGREDIENT_TYPE_COUNT; ++i)
            if (java::add(inventory.delta[i], need.delta[i]) < 0) return false;
        return true;
    }
    void addScore(i32 n) { setScore(java::add(getScore(), n)); }
    void setMessage(const String& m) {
        hasMessage = true;
        message = m;
    }
};

static Player players[2];

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.isActive()) r.push_back(&p);
    return r;
}

// gameManager.getPlayer(i): IndexOutOfBoundsException outside the players
static Player& getPlayer(int i) { return static_cast<Player&>(*cg::gm.getPlayer(i)); }

// ----------------------------------------------------------------------------
// view/AnimationData.java, EventData.java, SpellData.java
// ----------------------------------------------------------------------------
namespace Anim {
static const int SHORT = 100;
static const int QUICK = 300;
static const int LONG = 500;
static const int STIR_DURATION = LONG;
static const int RESET_DURATION = QUICK;
static const int SHELF_TO_POT_DURATION = LONG;
static const int POT_TO_SHELF_DURATION = SHELF_TO_POT_DURATION;
static const int SPLASH_DURATION = LONG;
static const int SHELF_TO_POT_SEPERATION = SHORT;
static const int POT_TO_SHELF_SEPERATION = SHELF_TO_POT_SEPERATION;
static const int SHELF_TO_TOME_DURATION = LONG;
static const int SHELF_TO_TOME_SEPARATION = SHORT;
static const int SHELF_TO_STOCK_DURATION = QUICK;
static const int SHELF_TO_STOCK_SEPARATION = SHORT;
static const int TOME_TO_SHELF_DURATION = LONG;
static const int TOME_TO_LEARNT_DURATION = LONG;
static const int NEW_SPELL_DURATION = LONG;
static const int NEW_SPELL_SEPARATION = SHORT;
static const int POTION_SPAWN_DURATION = LONG;
static const int POTION_TO_DELIVERY_DURATION = LONG;
static const int DELIVERY_FADE_DURATION = LONG;
} // namespace Anim

// an Integer field (null when !set)
struct OptInt {
    bool set = false;
    i32 v = 0;
    OptInt() {}
    OptInt(i32 x) : set(true), v(x) {}
};

struct AnimationData {
    i32 start = 0, end = 0;
    OptInt trigger, triggerEnd;
};

// SpellData: repeatable (Boolean) or score (Integer), the other one null
struct SpellData {
    i32 id = 0;
    i32 delta[4] = {0, 0, 0, 0};
    OptInt repeatable; // 0/1
    OptInt score;
};

static SpellData spellDataRepeatable(const Spell& s) {
    SpellData d;
    d.id = s.id;
    for (int i = 0; i < 4; i++) d.delta[i] = s.recipe.delta[i];
    d.repeatable = OptInt(s.isRepeatable() ? 1 : 0);
    return d;
}
static SpellData spellDataScore(const Spell& s, i32 score) {
    SpellData d;
    d.id = s.id;
    for (int i = 0; i < 4; i++) d.delta[i] = s.recipe.delta[i];
    d.score = OptInt(score);
    return d;
}

namespace EventType {
static const int LEARN = 0;
static const int LEARN_PAY = 6;
static const int NEW_DELIVERIES = 1;
static const int NEW_TOME_SPELLs = 2;
static const int PLAYER_SPELL = 3;
static const int DELIVERY = 4;
static const int RESET = 5;
} // namespace EventType

struct EventData {
    OptInt type, playerIdx, spellId, resultId, tomeIdx, acquired, lost, repeats;
    bool hasSpells = false;
    std::vector<SpellData> spells;
    bool hasAnimData = false;
    std::vector<AnimationData> animData;
};

// ----------------------------------------------------------------------------
// GameSummaryManager.java
// ----------------------------------------------------------------------------
namespace Summary {
static std::vector<String> lines;

static String u(const std::string& utf8) { return java::decodeUtf8(utf8); }
static String nick(const Player& p) { return p.getNicknameToken(); }

static void clear() { lines.clear(); }
static String getSummary() {
    String r;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) r += u"\n";
        r += lines[i];
    }
    return r;
}
static void addPlayerDelivery(const Player& player, int deliveryId, i32 earned) {
    lines.push_back(nick(player) + u(" brewed potion " + std::to_string(deliveryId) + " and earned " + std::to_string(earned) +
                                     " rupees\nThey have now brewed " + std::to_string(player.deliveriesCount) + " potions."));
}
static void addPlayerSpellLearn(const Player& player, const Spell& spell) {
    i32 total = player.inventory.getTotal();
    if (spell.getStock() > 0) {
        if (total >= Game::MAX_SPACE) {
            lines.push_back(nick(player) + u(" learnt spell " + std::to_string(spell.id) + ", the taxed ingredients were lost because their inventory is full."));
        } else if (java::add(total, spell.getStock()) > Game::MAX_SPACE) {
            lines.push_back(nick(player) + u(" learnt spell " + std::to_string(spell.id) + " and gained " + std::to_string(java::sub(Game::MAX_SPACE, total)) +
                                             " taxed ingredients, the rest was lost because their inventory is full."));
        } else {
            lines.push_back(nick(player) + u(" learnt spell " + std::to_string(spell.id) + " and gained " + std::to_string(spell.getStock()) + " taxed ingredients."));
        }
    } else {
        lines.push_back(nick(player) + u(" learnt spell " + std::to_string(spell.id)));
    }
}
static void addPlayerSpellAction(const Player& player, const Spell& spell) { lines.push_back(nick(player) + u(" cast spell " + std::to_string(spell.id))); }
static void addPlayerResetAction(const Player& player) { lines.push_back(nick(player) + u" rested"); }
static void addPlayerWaitAction(const Player& player) { lines.push_back(nick(player) + u" bided their time"); }
static void addPlayerInvalidAction(const Player& player, const std::string& errorMessage) {
    if (player.action.isReset()) lines.push_back(nick(player) + u(" attempted an invalid REST action:\n" + errorMessage));
    else lines.push_back(nick(player) + u(" attempted an invalid action:\n" + errorMessage));
}
static void addPlayerBadCommand(const Player& player, const String& expected, const String& got) {
    lines.push_back(cg::GameManager::formatErrorMessage(nick(player) + u" provided invalid input. Expected '" + expected + u"'\nGot '" + got + u"'"));
}
static void addPlayerTimeout(const Player& player) {
    lines.push_back(cg::GameManager::formatErrorMessage(nick(player) + u(" has not provided " + std::to_string(player.getExpectedOutputLines()) + " lines in time.")));
}
static void addPlayerDisqualified(const Player& player) { lines.push_back(nick(player) + u" was disqualified."); }
} // namespace Summary

// ----------------------------------------------------------------------------
// Deck.java, Game.java
// ----------------------------------------------------------------------------
struct GameException {
    std::string message;
};

namespace Game {

static std::deque<int> deckTome, deckDeliveries; // Deck's LinkedLists
static std::vector<int> tome, deliveries, newDeliveries, newTomeSpells;
static java::Random random;
static i32 tomeStockGain[TOME_SIZE];
static i32 bonus[2];
static i32 bonusValue[2] = {3, 1};
static int frameDuration;
// HashSet<TomeSpell> learntSpells and HashSet<DeliveryCompletion> delivered (identity hash codes):
// what the referee does with them does not depend on their iteration order, insertion order here
static std::vector<int> learntSpells;
struct DeliveryCompletion {
    int delivery;
    i32 index, earned;
};
static std::vector<DeliveryCompletion> delivered;
static std::vector<EventData> viewerEvents;
static std::string endScreenJson; // EndScreenModule data, set by onEnd

static void makeDeck() {
    static const int TOME[42][4] = {{-3, 0, 0, 1}, {3, -1, 0, 0}, {1, 1, 0, 0},  {0, 0, 1, 0},  {3, 0, 0, 0},  {2, 3, -2, 0}, {2, 1, -2, 1},
                                    {3, 0, 1, -1}, {3, -2, 1, 0}, {2, -3, 2, 0}, {2, 2, 0, -1}, {-4, 0, 2, 0}, {2, 1, 0, 0},  {4, 0, 0, 0},
                                    {0, 0, 0, 1},  {0, 2, 0, 0},  {1, 0, 1, 0},  {-2, 0, 1, 0}, {-1, -1, 0, 1}, {0, 2, -1, 0}, {2, -2, 0, 1},
                                    {-3, 1, 1, 0}, {0, 2, -2, 1}, {1, -3, 1, 1}, {0, 3, 0, -1}, {0, -3, 0, 2}, {1, 1, 1, -1}, {1, 2, -1, 0},
                                    {4, 1, -1, 0}, {-5, 0, 0, 2}, {-4, 0, 1, 1}, {0, 3, 2, -2}, {1, 1, 3, -2}, {-5, 0, 3, 0}, {-2, 0, -1, 2},
                                    {0, 0, -3, 3}, {0, -3, 3, 0}, {-3, 3, 0, 0}, {-2, 2, 0, 0}, {0, 0, -2, 2}, {0, -2, 2, 0}, {0, 0, 2, -1}};
    static const int DELIVERIES[36][5] = {{2, 2, 0, 0, 6},  {3, 2, 0, 0, 7},  {0, 4, 0, 0, 8},  {2, 0, 2, 0, 8},  {2, 3, 0, 0, 8},  {3, 0, 2, 0, 9},
                                          {0, 2, 2, 0, 10}, {0, 5, 0, 0, 10}, {2, 0, 0, 2, 10}, {2, 0, 3, 0, 11}, {3, 0, 0, 2, 11}, {0, 0, 4, 0, 12},
                                          {0, 2, 0, 2, 12}, {0, 3, 2, 0, 12}, {0, 2, 3, 0, 13}, {0, 0, 2, 2, 14}, {0, 3, 0, 2, 14}, {2, 0, 0, 3, 14},
                                          {0, 0, 5, 0, 15}, {0, 0, 0, 4, 16}, {0, 2, 0, 3, 16}, {0, 0, 3, 2, 17}, {0, 0, 2, 3, 18}, {0, 0, 0, 5, 20},
                                          {2, 1, 0, 1, 9},  {0, 2, 1, 1, 12}, {1, 0, 2, 1, 12}, {2, 2, 2, 0, 13}, {2, 2, 0, 2, 15}, {2, 0, 2, 2, 17},
                                          {0, 2, 2, 2, 19}, {1, 1, 1, 1, 12}, {3, 1, 1, 1, 14}, {1, 3, 1, 1, 16}, {1, 1, 3, 1, 18}, {1, 1, 1, 3, 20}};
    deckTome.clear();
    deckDeliveries.clear();
    for (auto& t : TOME) deckTome.push_back(newTomeSpell(Recipe(t[0], t[1], t[2], t[3])));
    for (auto& d : DELIVERIES) deckDeliveries.push_back(newDeliverySpell(Recipe(d[0], d[1], d[2], d[3]), d[4]));
}

// List.indexOf (identity: no spell class overrides equals)
static int indexOf(const std::vector<int>& list, int id) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i] == id) return (int)i;
    return -1;
}
static void removeFrom(std::vector<int>& list, int id) {
    int i = indexOf(list, id);
    if (i >= 0) list.erase(list.begin() + i);
}

static void slideBonus() {
    if (bonus[0] <= 0) {
        bonus[0] = bonus[1];
        bonus[1] = 0;
        bonusValue[0] = 1;
        bonusValue[1] = 0;
    }
}

static i32 getScoreOfDelivery(int delivery) {
    int index = indexOf(deliveries, delivery);
    i32 bonusScore = 0;
    if (index < 2) {
        if (index < 0) throw java::Exception{java::arrayIndexOutOfBoundsException(index, 2)}; // unreachable
        if (bonus[index] > 0) bonusScore = bonusValue[index];
    }
    return allSpells[delivery].getScore() + bonusScore;
}

static void init() {
    makeDeck();
    // league 19: "Bronze+"
    static const int STARTING_INGREDIENT_COUNT[4] = {3, 0, 0, 0};
    bonus[0] = 4;
    bonus[1] = 4;
    bonusValue[0] = 3;
    bonusValue[1] = 1;
    viewerEvents.clear();
    random = java::Random(cg::gm.getSeed());

    // Collections.shuffle of the LinkedLists: toArray, the swaps of the RandomAccess version, set back
    std::vector<int> t(deckTome.begin(), deckTome.end()), d(deckDeliveries.begin(), deckDeliveries.end());
    java::shuffle(t, random);
    java::shuffle(d, random);
    deckTome.assign(t.begin(), t.end());
    deckDeliveries.assign(d.begin(), d.end());

    tome.clear();
    deliveries.clear();
    newDeliveries.clear();
    newTomeSpells.clear();
    for (int i = 0; i < TOME_SIZE; ++i) {
        tome.push_back(deckTome.front());
        deckTome.pop_front();
        tomeStockGain[i] = 0;
    }
    for (int i = 0; i < COUNTER_SIZE; ++i) {
        deliveries.push_back(deckDeliveries.front());
        deckDeliveries.pop_front();
    }
    for (Player& p : players) {
        for (int i = 0; i < INGREDIENT_TYPE_COUNT; ++i) p.inventory.delta[i] = STARTING_INGREDIENT_COUNT[i];
        p.setScore(STARTING_SCORE);
        p.initSpells();
    }
}

static i32 getBonusAmount(int delivery) {
    int index = indexOf(deliveries, delivery);
    if (index < 0 || index > 1) return 0;
    return bonus[index];
}
static i32 getBonusValue(int delivery) {
    if (getBonusAmount(delivery) == 0) return 0;
    int index = indexOf(deliveries, delivery);
    if (index < 0 || index > 1) return 0;
    return bonusValue[index];
}

static i32 getScoreOf(int spell) {
    if (allSpells[spell].cls == DELIVERY_SPELL) return getScoreOfDelivery(spell);
    return allSpells[spell].getScore();
}

static SpellType getSpellType(int spell, const Player& player) {
    const Spell& s = allSpells[spell];
    if (s.cls == TOME_SPELL) return LEARN;
    if (s.cls == DELIVERY_SPELL) return BREW;
    return s.owner == player.index ? CAST : OPPONENT_CAST;
}

static std::vector<std::string> getCurrentFrameInfoFor(Player& player) {
    Player& foe = getPlayer(1 - player.index);
    std::vector<std::string> lines;
    lines.push_back(std::to_string(deliveries.size() + tome.size() + player.spells.size() + foe.spells.size()));
    for (int id : deliveries) {
        const Spell& spell = allSpells[id];
        lines.push_back(std::to_string(id) + " BREW " + spell.recipe.toPlayerString() + " " + std::to_string(getScoreOfDelivery(id)) + " " +
                        std::to_string(getBonusValue(id)) + " " + std::to_string(getBonusAmount(id)) + " " + (spell.isActive() ? "1" : "0") + " " +
                        (spell.isRepeatable() ? "1" : "0"));
    }
    std::vector<int> all(tome);
    all.insert(all.end(), player.spells.begin(), player.spells.end());
    all.insert(all.end(), foe.spells.begin(), foe.spells.end());
    for (int id : all) {
        const Spell& spell = allSpells[id];
        lines.push_back(std::to_string(id) + " " + SPELL_TYPE_NAME[getSpellType(id, player)] + " " + spell.recipe.toPlayerString() + " " +
                        std::to_string(getScoreOf(id)) + " " + std::to_string(indexOf(tome, id)) + " " + std::to_string(spell.getStock()) + " " +
                        (spell.isActive() ? "1" : "0") + " " + (spell.isRepeatable() ? "1" : "0"));
    }
    for (Player* p : {&player, &foe}) lines.push_back(p->inventory.toPlayerString() + " " + std::to_string(p->getScore()));
    return lines;
}

static void checkSpellActionType(const Action& action, SpellType type) {
    std::string expectedStr;
    switch (type) {
        case LEARN: expectedStr = "LEARN"; break;
        case CAST: expectedStr = "CAST"; break;
        case BREW: expectedStr = "BREW"; break;
        case OPPONENT_CAST: throw GameException{"Tried to cast an opponent's spell..."};
    }
    if (action.str != expectedStr) throw GameException{"Command does not match action, expected '" + expectedStr + "' but got '" + action.str + "'"};
}

// getSpellById: deliveries, tome, the delivered potions, the players' spells; -1 for none
static int getSpellById(i32 id) {
    for (int s : deliveries)
        if (allSpells[s].id == id) return s;
    for (int s : tome)
        if (allSpells[s].id == id) return s;
    for (const DeliveryCompletion& d : delivered)
        if (allSpells[d.delivery].id == id) return d.delivery;
    for (Player& p : players)
        for (int s : p.spells)
            if (allSpells[s].id == id) return s;
    return -1;
}

static void doReset(Player& p) {
    bool allActive = true;
    for (int s : p.spells) allActive = allActive && allSpells[s].isActive();
    if (allActive) throw GameException{"All spells are already castable"};
    for (int s : p.spells) allSpells[s].active = true;
    EventData e;
    e.type = EventType::RESET;
    e.playerIdx = p.index;
    viewerEvents.push_back(e);
}

static DeliveryCompletion doDelivery(Player& p, int del) {
    const Spell& spell = allSpells[del];
    if (!p.canDeliver(spell.recipe)) throw GameException{"Not enough ingredients for order " + std::to_string(spell.id)};
    for (int i = 0; i < INGREDIENT_TYPE_COUNT; ++i) p.inventory.add(i, spell.recipe.delta[i]);
    DeliveryCompletion delCompletion{del, (i32)indexOf(deliveries, del), getScoreOfDelivery(del)};
    delivered.push_back(delCompletion);
    p.deliveriesCount++;
    p.addScore(getScoreOfDelivery(del));
    EventData e;
    e.type = EventType::DELIVERY;
    e.playerIdx = p.index;
    e.spellId = spell.id;
    viewerEvents.push_back(e);
    return delCompletion;
}

static void doLearn(Player& p, int spellIdx) {
    int index = indexOf(tome, spellIdx);
    if (p.inventory.delta[0] < index * READ_AHEAD_COST) throw GameException{"Not enough ingredients to learn " + std::to_string(allSpells[spellIdx].id)};
    for (int i = 0; i < index; ++i) {
        tomeStockGain[i] += READ_AHEAD_COST;
        p.inventory.delta[0] = java::sub(p.inventory.delta[0], READ_AHEAD_COST);
    }
    Recipe copy = allSpells[spellIdx].recipe;
    bool repeatable = allSpells[spellIdx].isRepeatable();
    int learnt = newPlayerSpell(copy, p.index, repeatable);
    p.spells.push_back(learnt);
    const Spell& spell = allSpells[spellIdx];

    i32 maxToGet = java::sub(MAX_SPACE, p.inventory.getTotal());
    i32 ingredientsGot = std::min(maxToGet, spell.stock);
    p.inventory.delta[0] = java::add(p.inventory.delta[0], ingredientsGot);

    bool known = false;
    for (int s : learntSpells) known = known || s == spellIdx;
    if (!known) learntSpells.push_back(spellIdx);

    EventData e;
    e.type = EventType::LEARN;
    e.playerIdx = p.index;
    e.spellId = spell.id;
    e.resultId = allSpells[learnt].id;
    e.tomeIdx = index;
    e.acquired = ingredientsGot;
    e.lost = java::sub(spell.stock, ingredientsGot);
    viewerEvents.push_back(e);
}

static void doPlayerSpell(Player& p, int spellIdx) {
    Spell& spell = allSpells[spellIdx];
    i32 repeats = p.action.getRepeats();
    if (repeats < 1) throw GameException{"Repeat can't be zero (on " + std::to_string(spell.id) + ")"};
    if (repeats > 1 && !spell.isRepeatable()) throw GameException{"Spell " + std::to_string(spell.id) + " is not repeatable"};
    if (!spell.isActive()) throw GameException{"Spell " + std::to_string(spell.id) + " is exhausted"};
    if (!p.canAfford(spell.recipe, repeats)) throw GameException{"Not enough ingredients for spell " + std::to_string(spell.id)};
    if (!p.enoughSpace(spell.recipe, repeats)) throw GameException{"Not enough space in inventory for spell " + std::to_string(spell.id)};
    // repeats times the deltas, in int arithmetic: the same as delta * repeats
    for (int i = 0; i < INGREDIENT_TYPE_COUNT; ++i) p.inventory.add(i, java::mul(spell.recipe.delta[i], repeats));
    spell.active = false;
    EventData e;
    e.type = EventType::PLAYER_SPELL;
    e.playerIdx = p.index;
    e.spellId = spell.id;
    e.repeats = repeats;
    viewerEvents.push_back(e);
}

// the most entries an animation list gets here: the jar builds lists of up to 2^31 - 1 entries (int
// overflow of a repeat count), runs out of memory or sends a frame over the runner's 30000-byte
// buffer (over ~3000 entries): its game fails either way
static const i32 MAX_ANIMS = 100000;

static void addAnim(std::vector<AnimationData>& animData, const AnimationData& a) {
    if ((i32)animData.size() >= MAX_ANIMS) throw java::Exception{"java.lang.OutOfMemoryError: Java heap space"};
    animData.push_back(a);
}

static void chainAnims(i32 count, std::vector<AnimationData>& animData, i32* timeArray, int timeIdx, i32 duration, i32 separation, i32 triggerAfter,
                       i32 triggerDuration) {
    for (i32 i = 0; i < count; ++i) {
        AnimationData a;
        a.start = timeArray[timeIdx];
        a.end = java::add(a.start, duration);
        a.trigger = java::add(a.start, triggerAfter);
        a.triggerEnd = java::add(java::add(a.start, triggerAfter), triggerDuration);
        addAnim(animData, a);
        if (i < count - 1) timeArray[timeIdx] = java::add(timeArray[timeIdx], separation);
    }
    if (count > 0) timeArray[timeIdx] = java::add(timeArray[timeIdx], duration);
}

static void chainAnims(i32 count, std::vector<AnimationData>& animData, i32* timeArray, int timeIdx, i32 duration, i32 separation) {
    for (i32 i = 0; i < count; ++i) {
        AnimationData a;
        a.start = timeArray[timeIdx];
        a.end = java::add(a.start, duration);
        addAnim(animData, a);
        if (i < count - 1) timeArray[timeIdx] = java::add(timeArray[timeIdx], separation);
    }
    if (count > 0) timeArray[timeIdx] = java::add(timeArray[timeIdx], duration);
}

static void waitForAnim(std::vector<AnimationData>& eventAnimData, i32* playerTime, int playerIdx, i32 duration) {
    AnimationData a;
    a.start = playerTime[playerIdx];
    a.end = java::add(a.start, duration);
    addAnim(eventAnimData, a);
    playerTime[playerIdx] = java::add(playerTime[playerIdx], duration);
}

// Optional<Spell>.get() of computeEvents: the spell is always there
static const Spell& spellById(i32 id) {
    int s = getSpellById(id);
    if (s < 0) throw java::Exception{"java.util.NoSuchElementException: No value present"};
    return allSpells[s];
}

static void computeEvents() {
    using namespace Anim;
    i32 playerTime[2] = {0, 0};
    bool hasLastTriggerEnd = false;
    i32 lastTriggerEnd = 0;
    std::vector<size_t> learnEvents; // indices in viewerEvents

    for (size_t k = 0; k < viewerEvents.size(); k++) {
        EventData& event = viewerEvents[k];
        int pi = event.playerIdx.v;
        if (event.type.v == EventType::PLAYER_SPELL) {
            const Spell& spell = spellById(event.spellId.v);
            event.hasAnimData = true;
            event.animData.clear();
            i32 loss = java::mul(spell.recipe.getTotalLoss(), event.repeats.v);
            i32 gain = java::mul(spell.recipe.getTotalGain(), event.repeats.v);
            chainAnims(loss, event.animData, playerTime, pi, SHELF_TO_POT_DURATION, SHELF_TO_POT_SEPERATION, SHELF_TO_POT_DURATION, SPLASH_DURATION);
            waitForAnim(event.animData, playerTime, pi, STIR_DURATION);
            chainAnims(gain, event.animData, playerTime, pi, POT_TO_SHELF_DURATION, POT_TO_SHELF_SEPERATION);
        } else if (event.type.v == EventType::DELIVERY) {
            const Spell& spell = spellById(event.spellId.v);
            event.hasAnimData = true;
            event.animData.clear();
            i32 loss = spell.recipe.getTotalLoss();
            chainAnims(loss, event.animData, playerTime, pi, SHELF_TO_POT_DURATION, SHELF_TO_POT_SEPERATION, SHELF_TO_POT_DURATION, SPLASH_DURATION);
            hasLastTriggerEnd = true;
            lastTriggerEnd = java::add(playerTime[pi], SPLASH_DURATION);
            waitForAnim(event.animData, playerTime, pi, STIR_DURATION);
            waitForAnim(event.animData, playerTime, pi, POTION_SPAWN_DURATION);
            waitForAnim(event.animData, playerTime, pi, POTION_TO_DELIVERY_DURATION);
            waitForAnim(event.animData, playerTime, pi, DELIVERY_FADE_DURATION);
        } else if (event.type.v == EventType::LEARN) {
            event.hasAnimData = true;
            event.animData.clear();
            chainAnims(event.tomeIdx.v, event.animData, playerTime, pi, SHELF_TO_STOCK_DURATION, SHELF_TO_STOCK_SEPARATION);
            waitForAnim(event.animData, playerTime, pi, TOME_TO_LEARNT_DURATION);
            chainAnims(java::add(event.acquired.v, event.lost.v), event.animData, playerTime, pi, TOME_TO_SHELF_DURATION, TOME_TO_SHELF_DURATION);
            learnEvents.push_back(k);
        } else if (event.type.v == EventType::RESET) {
            event.hasAnimData = true;
            event.animData.clear();
            waitForAnim(event.animData, playerTime, pi, RESET_DURATION);
        }
    }

    if (learnEvents.size() >= 2) {
        i32 currentTime = std::max(playerTime[0], playerTime[1]);
        playerTime[0] = playerTime[1] = currentTime;
    }

    if (!learnEvents.empty()) {
        EventData newTomeSpellsEvent;
        newTomeSpellsEvent.type = EventType::NEW_TOME_SPELLs;
        newTomeSpellsEvent.hasAnimData = true;
        newTomeSpellsEvent.hasSpells = true;
        for (int s : newTomeSpells) newTomeSpellsEvent.spells.push_back(spellDataRepeatable(allSpells[s]));
        for (size_t k : learnEvents)
            chainAnims(1, newTomeSpellsEvent.animData, playerTime, viewerEvents[k].playerIdx.v, NEW_SPELL_DURATION, NEW_SPELL_SEPARATION);
        viewerEvents.push_back(newTomeSpellsEvent);
    }

    // (learnEvents indices stay valid: events are only appended)
    std::vector<size_t> learns = learnEvents;
    for (size_t k : learns) {
        EventData newLearnPayEvent;
        newLearnPayEvent.type = EventType::LEARN_PAY;
        newLearnPayEvent.hasAnimData = true;
        newLearnPayEvent.playerIdx = viewerEvents[k].playerIdx;
        newLearnPayEvent.tomeIdx = viewerEvents[k].tomeIdx;
        chainAnims(viewerEvents[k].tomeIdx.v, newLearnPayEvent.animData, playerTime, viewerEvents[k].playerIdx.v, SHELF_TO_TOME_DURATION,
                   SHELF_TO_TOME_SEPARATION);
        viewerEvents.push_back(newLearnPayEvent);
    }

    i32 currentTime = std::max(playerTime[0], playerTime[1]);
    if (hasLastTriggerEnd && lastTriggerEnd > currentTime) currentTime = lastTriggerEnd;

    i32 postTurnEventsTime[2] = {currentTime, currentTime};
    int newDeliveriesTimeIdx = 0;
    if (!newDeliveries.empty()) {
        EventData newDeliveriesEvent;
        newDeliveriesEvent.type = EventType::NEW_DELIVERIES;
        newDeliveriesEvent.hasAnimData = true;
        newDeliveriesEvent.hasSpells = true;
        for (int s : newDeliveries) newDeliveriesEvent.spells.push_back(spellDataScore(allSpells[s], allSpells[s].getScore()));
        chainAnims((i32)newDeliveries.size(), newDeliveriesEvent.animData, postTurnEventsTime, newDeliveriesTimeIdx, NEW_SPELL_DURATION, NEW_SPELL_SEPARATION);
        viewerEvents.push_back(newDeliveriesEvent);
        currentTime = std::max(postTurnEventsTime[0], postTurnEventsTime[1]);
    }

    i32 minTime = 1000;
    frameDuration = std::max(currentTime, minTime);
    cg::gm.setFrameDuration(frameDuration);
}

static bool gameOver() {
    if (getActivePlayers().size() <= 1) return true;
    for (Player& player : players)
        if (player.deliveriesCount >= DELIVERY_GOAL) return true;
    return false;
}

static void performGameUpdate() {
    for (Player& player : players) {
        try {
            const Action& action = player.action;
            if (action.isSpell()) {
                i32 id = action.spellId;
                int spell = getSpellById(id);
                if (spell >= 0) {
                    checkSpellActionType(action, getSpellType(spell, player));
                    SpellClass cls = allSpells[spell].cls;
                    if (cls == TOME_SPELL) {
                        doLearn(player, spell);
                        Summary::addPlayerSpellLearn(player, allSpells[spell]);
                    } else if (cls == DELIVERY_SPELL) {
                        DeliveryCompletion delComplete = doDelivery(player, spell);
                        cg::gm.addTooltip(player.index, player.getNicknameToken() + u" made a delivery!");
                        Summary::addPlayerDelivery(player, allSpells[delComplete.delivery].id, delComplete.earned);
                    } else {
                        doPlayerSpell(player, spell);
                        Summary::addPlayerSpellAction(player, allSpells[spell]);
                    }
                } else {
                    throw GameException{"Spell with id " + std::to_string(id) + " does not exist"};
                }
            } else if (action.isReset()) {
                doReset(player);
                Summary::addPlayerResetAction(player);
            } else if (action.isWait()) {
                Summary::addPlayerWaitAction(player);
            }
        } catch (const GameException& gameException) {
            Summary::addPlayerInvalidAction(player, gameException.message);
        }
    }

    for (int spell : learntSpells) {
        removeFrom(tome, spell);
        if (!deckTome.empty()) {
            int newSpell = deckTome.front();
            deckTome.pop_front();
            tome.push_back(newSpell);
            newTomeSpells.push_back(newSpell);
        }
    }

    for (const DeliveryCompletion& delCompletion : delivered) {
        if (delCompletion.index < 2) {
            if (bonus[delCompletion.index] > 0) bonus[delCompletion.index]--;
        }
    }
    // delivered.stream().map(getDelivery).distinct()
    std::vector<int> distinct;
    for (const DeliveryCompletion& d : delivered) {
        bool seen = false;
        for (int x : distinct) seen = seen || x == d.delivery;
        if (!seen) distinct.push_back(d.delivery);
    }
    for (int delivery : distinct) {
        removeFrom(deliveries, delivery);
        if (!deckDeliveries.empty()) {
            int newDelivery = deckDeliveries.front();
            deckDeliveries.pop_front();
            deliveries.push_back(newDelivery);
            newDeliveries.push_back(newDelivery);
        }
    }

    slideBonus();

    for (size_t i = 0; i < tome.size(); i++) {
        allSpells[tome[i]].stock = java::add(allSpells[tome[i]].stock, tomeStockGain[i]);
        tomeStockGain[i] = 0;
    }

    computeEvents();

    if (gameOver()) cg::gm.endGame();

    cg::gm.addToGameSummary(Summary::getSummary());
}

static void resetGameTurnData() {
    for (Player& p : players) p.reset();
    Summary::clear();
    learntSpells.clear();
    delivered.clear();
    viewerEvents.clear();
    newDeliveries.clear();
}

static String getExpected() { return u"BREW <id> | CAST <id> [<repeat>] | LEARN <id> | REST | WAIT"; }

static void onEnd() {
    // INVENTORY_BONUS
    for (Player* player : getActivePlayers())
        for (int i = 1; i < INGREDIENT_TYPE_COUNT; i++) player->addScore(player->inventory.delta[i]);
}

} // namespace Game

// ----------------------------------------------------------------------------
// CommandManager.java
// ----------------------------------------------------------------------------
namespace Commands {

// The patterns are CASE_INSENSITIVE (ASCII letters only) and matched against the whole line:
//   REST   ^REST(?:\s+(?<message>.*))?
//   CAST   ^CAST\s+(?<id>\d+)(?:\s+(?<param>\d+))?(?:\s+(?<message>.*))?
//   LEARN  ^LEARN\s+(?<id>\d+)(?:\s+(?<message>.*))?
//   BREW   ^BREW\s+(?<id>\d+)(?:\s+(?<message>.*))?
//   WAIT   ^WAIT(?:\s+(?<message>.*))?
// Greedy \s+ and \d+ never gain by giving chars back here (what follows a run of spaces can't be a
// space, what follows a run of digits can't be a digit), so each group takes its whole run; '.*' takes
// the rest of the line if it has no line terminator.

struct Match {
    String id, param, message;
    bool hasParam = false, hasMessage = false;
};

static bool literalAt(const String& s, size_t& pos, const char* lit) {
    size_t n = strlen(lit);
    if (pos + n > s.size()) return false;
    for (size_t i = 0; i < n; i++)
        if (!java::regexCharMatchesIgnoreCase(s[pos + i], lit[i])) return false;
    pos += n;
    return true;
}
static size_t spacesAt(const String& s, size_t pos) {
    size_t p = pos;
    while (p < s.size() && java::isRegexSpace(s[p])) p++;
    return p - pos;
}
static size_t digitsAt(const String& s, size_t pos) {
    size_t p = pos;
    while (p < s.size() && java::isRegexDigit(s[p])) p++;
    return p - pos;
}
// (?:\s+(?<message>.*))? followed by the end of the line, from pos: false when it can't match
static bool messageTail(const String& s, size_t pos, Match& m) {
    if (pos == s.size()) return true; // the group is skipped
    size_t sp = spacesAt(s, pos);
    if (sp == 0) return false;
    for (size_t i = pos + sp; i < s.size(); i++)
        if (java::isLineSeparator(s[i])) return false;
    m.hasMessage = true;
    m.message = s.substr(pos + sp);
    return true;
}
// KEYWORD\s+(?<id>\d+), returns the position after the id or npos
static size_t keywordId(const String& s, const char* kw, Match& m) {
    size_t pos = 0;
    if (!literalAt(s, pos, kw)) return String::npos;
    size_t sp = spacesAt(s, pos);
    if (sp == 0) return String::npos;
    pos += sp;
    size_t d = digitsAt(s, pos);
    if (d == 0) return String::npos;
    m.id = s.substr(pos, d);
    return pos + d;
}

static bool matchKeyword(const String& s, const char* kw, Match& m) {
    size_t pos = 0;
    if (!literalAt(s, pos, kw)) return false;
    return messageTail(s, pos, m);
}
static bool matchIdCommand(const String& s, const char* kw, Match& m) {
    size_t pos = keywordId(s, kw, m);
    if (pos == String::npos) return false;
    return messageTail(s, pos, m);
}
static bool matchCast(const String& s, Match& m) {
    size_t pos = keywordId(s, "CAST", m);
    if (pos == String::npos) return false;
    // the (?:\s+(?<param>\d+))? group first
    size_t sp = spacesAt(s, pos);
    if (sp > 0) {
        size_t d = digitsAt(s, pos + sp);
        if (d > 0) {
            Match withParam = m;
            if (messageTail(s, pos + sp + d, withParam)) {
                withParam.hasParam = true;
                withParam.param = s.substr(pos + sp, d);
                m = withParam;
                return true;
            }
        }
    }
    // without it
    return messageTail(s, pos, m);
}

// Integer.valueOf: NumberFormatException on overflow (the regex lets only ASCII digits through)
struct NumberFormat {
    std::string text; // e.toString()
};
static i32 valueOf(const String& digits) {
    i32 v;
    if (!java::parseInt(digits, v)) throw NumberFormat{java::encodeUtf8(java::numberFormatException(digits))};
    return v;
}

// matchMessage: messages over 48 chars are cut to 46 + "..."
static void matchMessage(Player& player, const Match& m) {
    if (m.hasMessage) {
        String message = m.message;
        if (message.size() > 48) message = message.substr(0, 46) + u"...";
        player.setMessage(message);
    }
}

static void deactivatePlayer(Player& player, const String& message) {
    player.deactivate(java::replace(java::replace(message, u"&lt;", u"<"), u"&gt;", u">"));
    player.setScore(-1);
}

static void parseCommands(Player& player, const std::vector<String>& lines) {
    // lines.get(0): empty only when a bot line desynchronised the protocol (see common/engine.h)
    if (lines.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
    const String& command = lines[0];
    String expected = Game::getExpected();
    try {
        Match m;
        if (matchKeyword(command, "REST", m)) {
            player.action = Action();
            player.action.kind = RESET_ACTION;
            matchMessage(player, m);
            return;
        }
        m = Match();
        if (matchCast(command, m)) {
            i32 spellId = valueOf(m.id);
            Action a;
            a.kind = SPELL_ACTION;
            a.str = "CAST";
            a.spellId = spellId;
            if (m.hasParam) {
                a.hasParam = true;
                a.param = valueOf(m.param);
            }
            player.action = a;
            matchMessage(player, m);
            return;
        }
        m = Match();
        if (matchIdCommand(command, "LEARN", m)) {
            Action a;
            a.kind = SPELL_ACTION;
            a.str = "LEARN";
            a.spellId = valueOf(m.id);
            player.action = a;
            matchMessage(player, m);
            return;
        }
        m = Match();
        if (matchIdCommand(command, "BREW", m)) {
            Action a;
            a.kind = SPELL_ACTION;
            a.str = "BREW";
            a.spellId = valueOf(m.id);
            player.action = a;
            matchMessage(player, m);
            return;
        }
        m = Match();
        if (matchKeyword(command, "WAIT", m)) {
            player.action = Action();
            player.action.kind = WAIT_ACTION;
            matchMessage(player, m);
            return;
        }
        // InvalidInputException(Game.getExpected(), command)
        deactivatePlayer(player, u"Invalid Input: Expected " + expected + u" but got '" + command + u"'");
        Summary::addPlayerBadCommand(player, expected, command);
        Summary::addPlayerDisqualified(player);
    } catch (const NumberFormat& e) {
        // new InvalidInputException(Game.getExpected(), e.toString())
        String got = java::decodeUtf8(e.text);
        deactivatePlayer(player, u"Invalid Input: Expected " + expected + u" but got '" + got + u"'");
        Summary::addPlayerBadCommand(player, expected, got);
        Summary::addPlayerDisqualified(player);
    }
}

} // namespace Commands

// ----------------------------------------------------------------------------
// view/Serializer.java (Referee#getCurrentFrameData / getGlobalData)
// ----------------------------------------------------------------------------
namespace Serializer {

static String s(i64 v) { return java::str(v); }
static String opt(const OptInt& v) { return v.set ? s(v.v) : String(u"_"); }

static String joinInts(const std::vector<i32>& v) {
    String r;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) r += u" ";
        r += s(v[i]);
    }
    return r;
}
static String joinDelta(const i32* d) { return s(d[0]) + u" " + s(d[1]) + u" " + s(d[2]) + u" " + s(d[3]); }

static String animsSerialize(const EventData& e) {
    if (!e.hasAnimData || e.animData.empty()) return u"0";
    String r = s((i64)e.animData.size()) + u" ";
    for (size_t i = 0; i < e.animData.size(); i++) {
        const AnimationData& a = e.animData[i];
        if (i) r += u" ";
        r += s(a.start) + u" " + s(a.end) + u" " + opt(a.trigger) + u" " + opt(a.triggerEnd);
    }
    return r;
}

static String spellsSerialize(const std::vector<SpellData>& spells) {
    if (spells.empty()) return u"0";
    String r = s((i64)spells.size()) + u" ";
    for (size_t i = 0; i < spells.size(); i++) {
        const SpellData& d = spells[i];
        if (i) r += u" ";
        String rep = d.repeatable.set ? String(d.repeatable.v ? u"true" : u"false") : String(u"_");
        r += s(d.id) + u" " + joinDelta(d.delta) + u" " + rep + u" " + opt(d.score);
    }
    return r;
}

static String eventsSerialize(const std::vector<EventData>& events) {
    if (events.empty()) return u"0";
    String r = s((i64)events.size()) + u"\n";
    for (size_t i = 0; i < events.size(); i++) {
        const EventData& e = events[i];
        if (i) r += u"\n";
        r += opt(e.type) + u" " + opt(e.playerIdx) + u" " + opt(e.spellId) + u" " + opt(e.resultId) + u" " + opt(e.tomeIdx) + u" " + opt(e.acquired) + u" " +
             opt(e.lost);
        r += u"\n" + (e.hasSpells ? spellsSerialize(e.spells) : String(u"0"));
        r += u"\n" + animsSerialize(e);
    }
    return r;
}

static String frame() {
    using namespace Game;
    String r = eventsSerialize(viewerEvents);
    // scores
    r += u"\n" + s(players[0].getScore()) + u" " + s(players[1].getScore());
    // playerSpells
    r += u"\n2";
    for (Player& p : players) r += u"\n" + joinInts(std::vector<i32>(p.spells.begin(), p.spells.end()));
    // tomeSpells, deliveries
    r += u"\n" + joinInts(std::vector<i32>(tome.begin(), tome.end()));
    r += u"\n" + joinInts(std::vector<i32>(deliveries.begin(), deliveries.end()));
    // inventories
    r += u"\n2";
    for (Player& p : players) r += u"\n" + joinDelta(p.inventory.delta);
    // active
    std::vector<i32> active;
    for (Player& p : players)
        for (int sp : p.spells)
            if (allSpells[sp].isActive()) active.push_back(allSpells[sp].id);
    r += u"\n" + joinInts(active);
    // stock: a HashMap<Integer, Integer> (Collectors.toMap) of the tome spells with stock
    std::vector<i32> ids, stocks;
    for (int sp : tome)
        if (allSpells[sp].getStock() > 0) {
            ids.push_back(allSpells[sp].id);
            stocks.push_back(allSpells[sp].getStock());
        }
    r += u"\n";
    std::vector<int> order = java::hashMapOrder(ids);
    for (size_t i = 0; i < order.size(); i++) {
        if (i) r += u" ";
        r += s(ids[order[i]]) + u" " + s(stocks[order[i]]);
    }
    // bonus: HashMap<Integer, BonusData> (keys 0 and 1): "index value,amount"
    r += u"\n";
    bool first = true;
    for (int i = 0; i < 2; ++i)
        if (bonus[i] > 0) {
            if (!first) r += u" ";
            first = false;
            r += s(i) + u" " + s(bonusValue[i]) + u"," + s(bonus[i]);
        }
    // messages: HashMap<Integer, String> (player indices)
    r += u"\n";
    first = true;
    for (Player& p : players)
        if (p.hasMessage) {
            if (!first) r += u"\n";
            first = false;
            r += s(p.index) + u" " + p.message;
        }
    return r;
}

static String global() {
    using namespace Game;
    String r = u"2";
    for (Player& p : players) {
        std::vector<SpellData> spells;
        for (int sp : p.spells) spells.push_back(spellDataRepeatable(allSpells[sp]));
        r += u"\n" + spellsSerialize(spells);
    }
    std::vector<SpellData> t, d;
    for (int sp : tome) t.push_back(spellDataRepeatable(allSpells[sp]));
    for (int sp : deliveries) d.push_back(spellDataScore(allSpells[sp], allSpells[sp].getScore()));
    r += u"\n" + spellsSerialize(t);
    r += u"\n" + spellsSerialize(d);
    return r;
}

} // namespace Serializer

// ----------------------------------------------------------------------------
// Referee.java, ViewModule, EndScreenModule
// ----------------------------------------------------------------------------
struct FallReferee : cg::Referee {
    static const int MAX_TURNS = 100;

    void init() override {
        try {
            cg::gm.setFrameDuration(500);
            cg::gm.setMaxTurns(MAX_TURNS);
            cg::gm.setTurnMaxTime(50);
            Game::init();
            // sendGlobalInfo: no global info in this game
        } catch (const java::Exception&) {
            // "Referee failed to initialize" (unreachable)
            cg::gm.endGame();
        }
    }

    void gameTurn(int turn) override {
        (void)turn;
        Game::resetGameTurnData();
        for (Player* player : getActivePlayers()) {
            for (const std::string& line : Game::getCurrentFrameInfoFor(*player)) player->sendInputLine(line);
            player->execute();
        }
        // handlePlayerCommands
        for (Player* player : getActivePlayers()) {
            if (player->timeout) { // getOutputs() throws TimeoutException
                Commands::deactivatePlayer(*player, u"Timeout!");
                Summary::addPlayerTimeout(*player);
                Summary::addPlayerDisqualified(*player);
            } else {
                Commands::parseCommands(*player, player->outputs);
            }
        }
        Game::performGameUpdate();
    }

    void onEnd() override {
        Game::onEnd();
        // eem.setScores(scores), titleRankingsSprite "logo.png", no displayed texts
        Game::endScreenJson = "[[" + std::to_string(players[0].getScore()) + "," + std::to_string(players[1].getScore()) + "],\"logo.png\",null]";
    }

    // ViewModule
    void onGameInit() override {
        cg::gm.setViewGlobalData("graphics", cg::Json::string(Serializer::global()));
        cg::gm.setViewData("graphics", cg::Json::string(Serializer::frame()));
    }
    void onAfterGameTurn() override { cg::gm.setViewData("graphics", cg::Json::string(Serializer::frame())); }
    // EndScreenModule
    void onAfterOnEnd() override { cg::gm.setViewData("endScreen", cg::Json::raw(Game::endScreenJson)); }
};

int main(int argc, char** argv) {
    static FallReferee referee;
    cg::Options options;
    options.maxPlayers = 2;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
