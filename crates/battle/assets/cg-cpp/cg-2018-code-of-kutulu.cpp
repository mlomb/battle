// Code of Kutulu (CodinGame, 2018-07) - headless C++ referee, port of
// https://github.com/JohnnyGuye/code-of-kutulu (commit 6b9b7c8, 2018-09-14, Maven project in codeOfKutulu/,
// pom artifactId codeOfKutulu), CodinGame game engine 2.2, exactly 4 players, the rules of the highest
// league. Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -p3 "<bot3 cmd>" -p4 "<bot4 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints the 4 scores, one per line, then "seed=N" to stdout like the jar. When the Java referee would
// have failed, prints nothing and exits with code 1, like the jar (engine 2.x has no FAIL command: its
// runner waits 150 s for the dead referee, then throws out of the game). Exit code 1 also when the jar's
// command line interface fails (bad -seed, a bot that cannot be started). -l writes the trace of
// common/engine.h (one line per bot execution, view frame (with CG_TRACE_VIEWS its text), game summary
// and tooltip block, then "END OK|<why the game failed>").
//
// Leagues: League.get(level) has WOOD_3..LEGEND for levels 1-7 and returns LEGEND for any other level;
// the jar's command line interface sets league.level 19, so LEGEND, whose rules are GOLD's = SILVER's =
// BRONZE's = WOOD_1's plus the bronze map list: every rule (wanderers, the isolated fear, PLAN, LIGHT,
// slashers, YELL, shelters, the 4 constants drawn from the seed) on the 16 bronze maps. CodinGame's
// Bronze to Legend leagues (levels 4-7) all play these rules, which is what this port implements; the
// wood leagues (fewer rules, 6 maps, fixed constants) are not.
//
// Build: the repository's HEAD does not compile as published (MazeFactory references BigPacMan, deleted
// in b7a0e1b and referenced again by the multiplayer commit 660a2c7): referees/cg-builder applies
// patches/cg-2018-code-of-kutulu.patch, which restores BigPacMan.java from the repository's own history. That
// maze is in no map list, so no game uses it. The jar is built with CommandLineInterface-v2.java.
//
// Engine 2.2 (common/engine.h, cg::Options::engine25): its core and runner are 2.5's (Maven Central
// sources diffed: only MultiplayerGameManager's handling of a non-numeric "seed" parameter differs, which
// the CLI never passes): the runner's split of the outputs (an empty line gives the referee no line at
// all: "Invalid Input: ''"), no frame for a turn without execution (never happens here), no summary or
// turn time quotas, 400 turns by default (the game ends itself after turn 200: 201 turns), turns numbered
// from 0 (the port passes turn - 1), 1000 ms for a bot's first turn and 50 ms after.
//
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption, texts.
// Notable behaviours reproduced here:
//   - new java.util.Random(seed): the 4 constants (randomBetween), the maze (Math.abs(nextInt()) % 16,
//     Integer.MIN_VALUE giving 0), each minion's unused dirOffset (nextInt(4) when created), and each
//     chase's Fisher-Yates order of the living explorers (the previous target first, kept on ties).
//   - the A* of PathFinder: the open list is stable-sorted by cumulative length + 1 after each expansion,
//     i.e. popped by (key, insertion order), cells re-added while not closed, the target accepted when
//     popped; minions weigh cells under LIGHT 5 and start their directions at (turn % 4), MOVE x y uses
//     weight 1 from UP; the first step becomes RIGHT/LEFT/UP/DOWN, anything else WAIT + "Invalid move".
//   - the commands: "^MOVE (-?\d+) (-?\d+)(?:\s+(.+))?\s*$" then "^(\w+)(?:\s+(.+))?\s*$", CASE_INSENSITIVE
//     with find(): ASCII \d \w \s, a single space between the MOVE tokens, the message (the rest after the
//     leading whitespace, or the last char of a whitespace-only rest of 2+ chars), Integer.parseInt
//     overflows, Action.valueOf of the upper-cased word (UP / DOWN / LEFT / RIGHT / WAIT / PLAN / LIGHT /
//     YELL); any failure kills the explorer (score -1, "Invalid Input: '<line>'").
//   - PlayerAction.WAIT is compared by identity: a stuck explorer that answered WAIT is still "taken
//     aback" in the summary.
//   - the turn: slashers (sanity < 200, once per explorer) then wanderers (every 5 turns at the spawn
//     points farthest from the living explorers, ties included) spawn; YELLs first (each pair once, range
//     1, the stuck explorer loses this turn and the next), moves and effects (one at a time per explorer:
//     PLAN 2 uses, 4 turns, heals in a radius of 2; LIGHT 3 uses, 2 turns, radius 5), effects, minions
//     (wanderers chase and spook, recalled on success or after their life time; slashers: spawn 6 turns,
//     rush, stunned 6, wander, stalk 2 turns when one explorer is in their line of sight (several at the
//     same distance: none)), the madness (alone 3-6, grouped within 2: 1-3, Kutulu's 50 when one explorer
//     is left), deaths (score = the turn), shelters every 50 turns from turn 50 (10 energy, 5 sanity per
//     explorer per turn); after turn 200 the survivors score 200 + sanity.
//   - the view, because the game can reach the engine's 1 MB view data quota: its own TooltipModule sends
//     all its registrations every frame (every explorer, minion, shelter: the view grows with the
//     minions alive); 200-turn games measured 790-850 KB. So the view is rebuilt: GraphicEntityModule 2.2
//     (common/gem.h, Module(true): sprite sheets, "C"/"U" commands), the game's TooltipModule and
//     EndScreenModule, and its view code (ViewController, MapNode, Ground/WallNode, ScoreNode with the
//     explorers' messages cut to 26 UTF-16 units, PlayerNode, Wanderer/SlasherMinionNode,
//     AnimationWithMask); the size of every frame was compared with the jar's.
//   - Engine (common/engine.h): the protocol between the runner and the referee (java.util.Scanner
//     lines: U+0085, U+2028 or U+2029 in a bot line makes the game fail), the runner's 10000 bytes per bot
//     output and 30000 bytes per referee command ("Invalid Input: '...'" of 9994 bytes 0xFF: the game
//     fails).
//   - 2 or 3 players: the first turn's inputs read getPlayer((i + index) % 4): IndexOutOfBoundsException,
//     the game fails (the jar after 150 s).
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time and checks the time
// limits on the wall clock: on a loaded machine it may time out a bot that answered in time); the order
// of the view's entity updates, group children and tooltip entries (identity hash codes and HashMaps in
// the jar: only the text order differs, not its size); toUpperCase in a Turkish default locale.
// Validated (2026-10-25) with battle referee-diff against the jar on 3136 games (30 bots; a covering
// design: every bot in every seat with 6 partner triples, every bot alone and 30 targeted quadruples, on
// seeds from 1, 1000001, 987654321 and 5000000000, plus 16 games that reach turn 200; the one mismatch
// was the loaded candidate timing out playback bots, identical when replayed alone) and in direct mode
// (jar game log vs -l trace: every execution, every view frame's size, summaries, tooltips, scores) on
// 450 games, including raw line endings, invalid UTF-8, U+2028 / U+0085 / U+2029, empty lines, 10000 /
// 10001-byte lines, a 30000 / 30003-byte tooltip, 2 and 3 players, edge seeds and -timeout.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/engine.h"
#include "common/gem.h"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <vector>

using java::String;

namespace {

String A(const char* s) { return java::ascii(s); }
String I(i64 v) { return java::str(v); }

// ----------------------------------------------------------------------------
// Constants, Rule (league 19 = LEGEND: every rule, the bronze maps)
// ----------------------------------------------------------------------------
const int MAX_PLAYERS = 4;
const int MAX_TURNS = 200;
const int MAX_DIRS = 4;
const int UP = 0, RIGHT = 1, DOWN = 2, LEFT = 3, STAY = 4;
const char* const DIRECTIONS[5] = {"up", "right", "down", "left", "wait"};

int SPREAD_MADNESS_PER_TURN_AMOUNT = 3;
int SPREAD_MADNESS_PLAYER_PROX_PER_TURN_AMOUNT = 1;
const int SPREAD_KUTULU_MADNESS_PER_TURN_AMOUNT = 50;
const int IM_NOT_ALONE_RANGE = 2;

const int PLAYER_TYPE = 0, WANDERER_TYPE = 1, SLASHER_TYPE = 2, EFFECT_SHELTER_TYPE = 3, EFFECT_PLAN_TYPE = 4, EFFECT_REPULSE_TYPE = 5,
          EFFECT_YELL_TYPE = 6;
const char* const UNIT_TYPES[7] = {"EXPLORER", "WANDERER", "SLASHER", "EFFECT_SHELTER", "EFFECT_PLAN", "EFFECT_LIGHT", "EFFECT_YELL"};

int WANDERER_SPAWN_TIME = 3;
const int WANDERER_SPAWN_INTERVAL = 5;
int WANDERER_LIFE_TIME = 40;

const int SLASHER_SPAWN_TIME = 6;
const int SLASHER_RUSH_PREP = 2;
const int SLASHER_RUSH_STUN = 6;
const int SLASHER_SANITY_SPAWN = 200;

const int MINION_SANITY_LOST = 20;

const int PLAYER_MAX_SANITY = 250;
const int PLAYER_MAX_PLAN_EFFECT = 2;
const int PLAYER_MAX_LIGHT_EFFECT = 3;

const int EFFECT_PLAN_MAX_DURATION = 4;
const int EFFECT_PLAN_HEAL_RADIUS = 2;
const int EFFECT_PLAN_HEAL = 3;

const int SHELTER_SPAWN_INTERVAL = 50;
const int EFFECT_MAX_SHELTER_ENERGY = 10;
const int EFFECT_SHELTER_HEAL = 5;

const int EFFECT_REPULSE_MAX_DURATION = 2;
const int EFFECT_REPULSE_RADIUS = 5;
const int EFFECT_REPULSE_WEIGHT = 4;

const int CELL_SIZE = 64;
const int SCREEN_W = 1920;

const int PLAYER_INITIAL_STUCK_ROUNDS = 2;
const int EFFECT_YELL_RADIUS = 1;

const bool HAS_WANDERER = true, HAS_SLASHER = true, HAS_SHELTER = true, HAS_PLAN = true, HAS_LIGHT = true, HAS_YELL = true,
           HAS_ISOLATED_FEAR = true;

java::Random random_; // Constants.random

// Constants.randomBetween
int randomBetween(int min, int max) {
    if (max <= min) return max;
    return min + random_.nextInt(max - min + 1);
}

// ----------------------------------------------------------------------------
// Vector2D
// ----------------------------------------------------------------------------
struct V {
    int x = 0, y = 0;
    bool operator==(const V& o) const { return x == o.x && y == o.y; }
    V operator+(const V& o) const { return V{x + o.x, y + o.y}; }
    int manhattan(const V& o) const { return java::abs(x - o.x) + java::abs(y - o.y); }
    int tchebychev(const V& o) const { return std::max(java::abs(x - o.x), java::abs(y - o.y)); }
};
const V VDIR[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}}; // UP, RIGHT, DOWN, LEFT

// Vector2D.matchDirection(new Vector2D(to).sub(from).normalize())
int directionTo(V from, V to) {
    int x = to.x - from.x, y = to.y - from.y;
    if ((x | y) != 0) {
        int ax = java::abs(x), ay = java::abs(y);
        if (ax > ay) {
            x /= ax;
            y = 0;
        } else if (ax < ay) {
            x = 0;
            y /= ay;
        } else {
            x /= ax;
            y /= ay;
        }
    }
    for (int d = 0; d < 4; d++)
        if (VDIR[d].x == x && VDIR[d].y == y) return d;
    return STAY;
}

// ----------------------------------------------------------------------------
// Mazes (MazeFactory.bronzeMaps, in order)
// ----------------------------------------------------------------------------
struct MazeDef {
    const char* name;
    std::vector<const char*> rows;
};
const MazeDef BRONZE_MAPS[16] = {
    {"Four of a kind", {"#########.#########", "#...#...#.#...#...#", "#.#...#.#.#.#...#.#", "#.##.##.###.##.##.#", "#...S.........S...#", "#.##.##.#.#.##.##.#", "#.##.#U.#.#.U#.##.#", "#......##.##......#", "####.###w.w###.####", "...#.....#.....#...", "####.###w.w###.####", "#......##.##......#", "#.##.#U.#.#.U#.##.#", "#.##.##.#.#.##.##.#", "#...S.........S...#", "#.##.##.###.##.##.#", "#.#...#.#.#.#...#.#", "#...#...#.#...#...#", "#########.#########"}},
    {"Hypersonic", {"###############", "#.............#", "#.#.#.#.#.#.#.#", "#..w.......w..#", "#.#.#.#.#.#.#.#", "#....S...S....#", "#.#.#.#.#.#.#.#", "#....S...S....#", "#.#.#.#.#.#.#.#", "#..w.......w..#", "#.#.#.#.#.#.#.#", "#.............#", "###############"}},
    {"Origin of symmetry", {"####################", "##................##", "#.w###.######.###w.#", "#.#....#....#....#.#", "#.#.##.#.##.#.##.#.#", "#...##...##...##...#", "#.#...U#....#U...#.#", "#.###.##S##S##.###.#", "#.###.##S##S##.###.#", "#.#...U#....#U...#.#", "#...##...##...##...#", "#.#.##.#.##.#.##.#.#", "#.#....#....#....#.#", "#.w###.######.###w.#", "##................##", "####################"}},
    {"Shelter me", {"####################", "#..#.#w..##..w#.#..#", "#.w#.#.#....#.#.#w.#", "#.#....##..##....#.#", "#...#U........U#...#", "###.##.##SS##.##.###", "#.......S..S.......#", "#.####.##..##.####.#", "#.#.....#..#.....#.#", "#.#.###.#..#.###.#.#", "#.#...#.#UU#.#...#.#", "#.###...#..#...###.#", "#.....###..###.....#", "#.###..U....U..###.#", "#.#...#.####.#...#.#", "#.#.#.#.####.#.#.#.#", "#U................U#", "####################"}},
    {"Slasher's hell", {"######################", "#.......######.......#", "#.###.#........#.###.#", "#.#S#.##########.#S#.#", "#.#.#.###w##w###.#.#.#", "#....................#", "#.#.#.##########.#.#.#", "#.#.#..U......U..#.#.#", "#.#.###.######.###.#.#", "#.#.###.######.###.#.#", "#.#.#..U......U..#.#.#", "#.#.#.##########.#.#.#", "#....................#", "#.#.#.###w##w###.#.#.#", "#.#S#.##########.#S#.#", "#.###.#........#.###.#", "#.......######.......#", "######################"}},
    {"Typhoon", {"####################", "###...S...##########", "###.###.#.#......###", "##....#.###.####...#", "##.##.....#.#U##.#.#", "##.##.#.#.#.#....#.#", "##.#U.#.#.#.###.##S#", "##.####.#..........#", "##......##w####.##.#", "#######.w###....#..#", "#..#....###w.#######", "#.##.####w##......##", "#..........#.####.##", "#S##.###.#.#.#.U#.##", "#.#....#.#.#.#.##.##", "#.#.##U#.#.....##.##", "#...####.###.#....##", "###......#.#.###.###", "##########...S...###", "####################"}},
    {"Oasis", {"###################", "#.................#", "#.#.##.##.##.##.#.#", "#..w#..##.##..#w..#", "#.###..##.##..###.#", "#.#..U###.###U..#.#", "#....####.####....#", "#.#####S...S#####.#", "#.#####.###.#####.#", "#.......#U#.......#", "#.#####.###.#####.#", "#.#####S...S#####.#", "#....####.####....#", "#.#..U###.###U..#.#", "#.###..##.##..###.#", "#..w#..##.##..#w..#", "#.#.##.##.##.##.#.#", "#.................#", "###################"}},
    {"Cross", {"#####################", "#S.................S#", "#...##..##.##..##...#", "#.#..##..###..##..#.#", "#.##..##..#..##..##.#", "#..##..##...##..##..#", "#.......##U##.......#", "#.#..##..###..##..#.#", "#.##..##.w#w.##..##.#", "#..##.U##...##U.##..#", "#..##.U##...##U.##..#", "#.##..##.w#w.##..##.#", "#.#..##..###..##..#.#", "#.......##U##.......#", "#..##..##...##..##..#", "#.##..##..#..##..##.#", "#.#..##..###..##..#.#", "#...##..##.##..##...#", "#S.................S#", "#####################"}},
    {"Shelter in Peril", {"#############", "#...........#", "#.#.##.##.#.#", "#..S##.##S..#", "#.###...###.#", "#.##.wUw.##.#", "#....U#U....#", "#.##.wUw.##.#", "#.###...###.#", "#..S##.##S..#", "#.#.##.##.#.#", "#...........#", "#############"}},
    {"Corridors", {"#################", "#U.............U#", "#.######.######.#", "#..#w.......w#..#", "##.#..##.##..#.##", "#..#.........#..#", "#.######.######.#", "#..#..S...S..#..#", "##.#..##.##..#.##", "#..#..S...S..#..#", "#.######.######.#", "#..#.........#..#", "##.#..##.##..#.##", "#..#w.......w#..#", "#.######.######.#", "#U.............U#", "#################"}},
    {"Roommates", {"####################", "#........##........#", "#.####.#.##.#.####.#", "#.#..S........S..#.#", "#...#.##.##.##.#...#", "#.#.#.##.##.##.#.#.#", "#.#.#....##....#.#.#", "#.....#w####w#.....#", "###.#####..#####.###", "###.#####..#####.###", "#.....#w####w#.....#", "#.#.#....##....#.#.#", "#.#.#.##.##.##.#.#.#", "#...#.##.##.##.#...#", "#.#..S........S..#.#", "#.####.#.##.#.####.#", "#........##........#", "####################"}},
    {"Pac man", {"###############", "#.....###.....#", "#.###.....###.#", "#.#...#.#...#.#", "#.#.#.#.#.#.#.#", "#....S...S....#", "##.####w####.##", "#....S...S....#", "#.#.#.#.#.#.#.#", "#.#...#.#...#.#", "#.###.....###.#", "#.....###.....#", "###############"}},
    {"The challenge from beyond", {"###############", "#w.....###...w#", "#.#.##.###.##.#", "#.#.....#.....#", "#...###...#.#.#", "###.....#.#.#.#", "####.##S#.#...#", "###...S.S...###", "#...#.#S##.####", "#.#.#.#.....###", "#.#.#...###...#", "#.....#.....#.#", "#.##.###.##.#.#", "#w...###.....w#", "###############"}},
    {"Cog", {"###############", "######.U.######", "###....#....###", "##.S##.#.##S.##", "##.##.....##.##", "##.#..#.#..#.##", "#....##.##....#", "#U##...w...##U#", "#....##.##....#", "##.#..#.#..#.##", "##.##.....##.##", "##.S##.#.##S.##", "###....#....###", "######.U.######", "###############"}},
    {"Hill climbing", {"#################", "#S.............S#", "#.######.######.#", "#.##.........##.#", "#.#.w###.###w.#.#", "#.#.##.....##.#.#", "#.#.#..#.#..#.#.#", "#.#.#.##U##.#.#.#", "#......U.U......#", "#.#.#.##U##.#.#.#", "#.#.#..#.#..#.#.#", "#.#.##.....##.#.#", "#.#.w###.###w.#.#", "#.##.........##.#", "#.######.######.#", "#S.............S#", "#################"}},
    {"Pixelated", {"####################", "#...#..........#...#", "#.#...#.#.#.##.#.#.#", "#...#..............#", "###...##.##.#w#.#.##", "#...#.#...w.#......#", "#.#.w...#.#.#.##.#.#", "#.#.###S....S..#...#", "#.......#.##.#...#.#", "#.#.#w#.#......#...#", "#...#......#.#w#.#.#", "#.#...#.##.#.......#", "#...#..S....S###.#.#", "#.#.##.#.#.#...w.#.#", "#......#.w...#.#...#", "##.#.#w#.##.##...###", "#..............#...#", "#.#.#.##.#.#.#...#.#", "#...#..........#...#", "####################"}},
};

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------
enum class CellType { NONE, EMPTY, WALL, SHELTER };

struct GroundNode;
struct PlayerNode;

// the events of the cells (Observable#notifyObservers to their CellNode; a WallNode ignores them)
enum class CellEvent { PLAN, REPULSE, SHELTER, SPAWN, SPOOK };

struct Cell {
    V pos;
    CellType type = CellType::NONE;
    bool spawnPoint = false;
    bool minionRepulse = false;
    bool invalid = false; // Cell.INVALID_CELL
    GroundNode* observer = nullptr;

    void emitEvent(CellEvent e, int energy = 0);
};

Cell INVALID_CELL;

struct Playfield {
    int W = 0, H = 0;
    std::vector<Cell> cells;
    std::vector<Cell*> shelterCells;

    bool inBounds(int x, int y) const { return !(x < 0 || x >= W || y < 0 || y >= H); }
    Cell* get(int x, int y) { return inBounds(x, y) ? &cells[y * W + x] : &INVALID_CELL; }
    Cell* get(V p) { return get(p.x, p.y); }
    bool isWalkable(const Cell* c) const { return c->type != CellType::WALL && !c->invalid && inBounds(c->pos.x, c->pos.y); }
};

const MazeDef* maze = nullptr;
Playfield playfield;

struct PlayerUnit;
enum class Action { WAIT, UP, DOWN, LEFT, RIGHT, PLAN, LIGHT, YELL };
const char* const ACTION_NAMES[8] = {"WAIT", "UP", "DOWN", "LEFT", "RIGHT", "PLAN", "LIGHT", "YELL"};
const V ACTION_MOVES[8] = {{0, 0}, {0, -1}, {0, 1}, {-1, 0}, {1, 0}, {0, 0}, {0, 0}, {0, 0}};

// PlayerAction: staticWait marks the PlayerAction.WAIT instance (the referee compares identities)
struct PlayerAction {
    Action action = Action::WAIT;
    String message;
    bool staticWait = true;
};

struct PlayerUnit {
    int id = 0;
    V pos;
    int orientation = DOWN;
    bool isIsolated = true;
    int sanity = PLAYER_MAX_SANITY;
    int remainingPlans = PLAYER_MAX_PLAN_EFFECT;
    int remainingLight = PLAYER_MAX_LIGHT_EFFECT;
    std::vector<int> yellsBy;
    bool performingInvalidMove = false;
    PlayerAction wantedAction;
    PlayerNode* observer = nullptr;

    // Unit#move: PlayerUnit#setOrientation keeps STAY
    void move(const Cell* c) {
        orientation = directionTo(pos, c->pos);
        pos = c->pos;
    }
    Cell* getCurrentCell() const { return playfield.get(pos); }
    bool isOnCell(const Cell* c) const { return playfield.isWalkable(c) && c->pos == pos; }
    bool isDead() const { return sanity <= 0; }
    void kill() { sanity = 0; }
    void madden(int amount) {
        sanity -= amount;
        if (sanity <= 0) kill();
    }
    void spook(int amount);
    void heal(int points) { sanity = std::min(PLAYER_MAX_SANITY, sanity + points); }
    bool hasBeenStuckedBy(int pid) const { return std::find(yellsBy.begin(), yellsBy.end(), pid) != yellsBy.end(); }
    String toOutput() const {
        return java::ascii(std::string(UNIT_TYPES[PLAYER_TYPE]) + " " + std::to_string(id) + " " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " +
                           std::to_string(sanity) + " " + std::to_string(remainingPlans) + " " + std::to_string(remainingLight));
    }
    void updateIsolation(const std::vector<PlayerUnit*>& players) {
        isIsolated = HAS_ISOLATED_FEAR;
        for (PlayerUnit* other : players) {
            if (other == this) continue;
            isIsolated &= other->pos.manhattan(pos) > IM_NOT_ALONE_RANGE;
        }
    }
    void spreadMadness() { madden(isIsolated ? SPREAD_MADNESS_PER_TURN_AMOUNT : SPREAD_MADNESS_PLAYER_PROX_PER_TURN_AMOUNT); }
    bool isStuck() const;
};

enum class MinionState { SPAWNING, WANDERING, STALKING, RUSH, STUNNED, RECALLING_SUCCESS, RECALLING_FAILURE };
const int MINION_STATE_ID[7] = {0, 1, 2, 3, 4, 10, 11};
const char* const MINION_STATE_NAMES[7] = {"SPAWNING", "WANDERING", "STALKING", "RUSH", "STUNNED", "RECALLING_SUCCESS", "RECALLING_FAILURE"};

struct Minion {
    int id = 0;
    V pos;
    int orientation = DOWN;
    bool slasher = false;
    int dirOffset = 0; // drawn, never used
    MinionState state = MinionState::SPAWNING;
    int spawnCountdown = 0;
    int lifeTimeCountdown = 0;
    PlayerUnit* target = nullptr;
    // MinionSlasher
    int stunCountdown = 0;
    Cell* targetPositionBeforePreparation = nullptr;

    // Unit#move: a minion keeps its orientation when it stays
    void move(const Cell* c) {
        int ori = directionTo(pos, c->pos);
        if (ori != STAY) orientation = ori;
        pos = c->pos;
    }
    Cell* getCurrentCell() const { return playfield.get(pos); }
    bool isSpawning() const { return state == MinionState::SPAWNING; }
    bool isBeingRecalled() const { return state == MinionState::RECALLING_FAILURE || state == MinionState::RECALLING_SUCCESS; }
    int getCountdown() const {
        if (slasher && (state == MinionState::STALKING || state == MinionState::STUNNED)) return stunCountdown;
        return state == MinionState::SPAWNING ? spawnCountdown : lifeTimeCountdown;
    }
    String toOutput() const {
        int t = target != nullptr && !target->isDead() ? target->id : -1;
        return java::ascii(std::string(UNIT_TYPES[slasher ? SLASHER_TYPE : WANDERER_TYPE]) + " " + std::to_string(id) + " " + std::to_string(pos.x) + " " +
                           std::to_string(pos.y) + " " + std::to_string(getCountdown()) + " " + std::to_string(MINION_STATE_ID[(int)state]) + " " +
                           std::to_string(t));
    }
    void update();
    // Minion
    void chaseClosestPlayers();
    bool spookPlayersInCell(Cell* cell);
    // MinionSlasher
    bool acquireTarget();
    bool isPlayerInLoS(const PlayerUnit* p);
    std::vector<PlayerUnit*> acquireClosestTargets();
    void rush();
};

struct Effect {
    int type = 0;
    PlayerUnit* caster = nullptr; // null for a shelter
    int tick = 0;
    Cell* cell = nullptr;           // ShelterEffect
    PlayerUnit* receiver = nullptr; // YellEffect

    bool isFinished() const { return tick <= 0; }
    bool isTimer() const { return type != EFFECT_SHELTER_TYPE; }
    const Cell* getCurrentCell() const {
        if (type == EFFECT_SHELTER_TYPE) return cell;
        if (type == EFFECT_YELL_TYPE) return &INVALID_CELL;
        return caster->getCurrentCell();
    }
    void doEffect();
    // nullptr: YellEffect of a dead receiver
    bool toOutput(String& out) const {
        if (type == EFFECT_YELL_TYPE && receiver->isDead()) return false;
        const Cell* c = getCurrentCell();
        std::string p1 = caster != nullptr ? std::to_string(caster->id) : "-1";
        std::string p2 = type == EFFECT_YELL_TYPE ? std::to_string(receiver->id) : "-1";
        out = java::ascii(std::string(UNIT_TYPES[type]) + " -1 " + std::to_string(c->pos.x) + " " + std::to_string(c->pos.y) + " " + std::to_string(tick) + " " +
                          p1 + " " + p2);
        return true;
    }
};

// Referee's static state
int NEXT_ID = 4;
int turn_ = 0; // Referee.turn
std::vector<std::unique_ptr<PlayerUnit>> allUnits;
std::vector<std::unique_ptr<Minion>> allMinions;
std::deque<Effect> allEffects;
std::vector<Minion*> minions;
std::vector<PlayerUnit*> livingPlayers;
std::vector<Effect*> effects;

bool contains(const std::vector<PlayerUnit*>& v, const PlayerUnit* p) { return std::find(v.begin(), v.end(), p) != v.end(); }

bool PlayerUnit::isStuck() const {
    for (const Effect* e : effects)
        if (e->type == EFFECT_YELL_TYPE && e->receiver == this) return true;
    return false;
}

bool isKutuluComing() { return livingPlayers.size() == 1; }

std::vector<PlayerUnit*> playersOnCell(const Cell* cell) {
    std::vector<PlayerUnit*> r;
    for (PlayerUnit* p : livingPlayers)
        if (p->isOnCell(cell)) r.push_back(p);
    return r;
}

// ----------------------------------------------------------------------------
// PathFinder / AStar: the open list is kept sorted (stable sort after each expansion) by
// cumulativeLength + 1, so it pops the lowest key, the earliest added first
// ----------------------------------------------------------------------------
struct PathResult {
    bool noPath = true;
    std::vector<Cell*> path;
    int weightedLength = -1;
    bool hasNextCell() const { return path.size() > 1; }
    Cell* nextCell() const { return path[1]; }
};

PathResult findPath(Cell* from, Cell* to, int dirOffset, bool minionWeights) {
    struct Item {
        Cell* cell;
        int cumulative;
        int precedent;
    };
    std::vector<Item> items;
    std::vector<char> closed(playfield.cells.size(), 0);
    using Key = std::tuple<int, int, int>; // totalPrevisionalLength, insertion order, item
    std::priority_queue<Key, std::vector<Key>, std::greater<Key>> open;
    int seq = 0;
    items.push_back({from, 0, -1});
    open.push(Key{0, seq++, 0});
    int found = -1;
    while (!open.empty()) {
        int vi = std::get<2>(open.top());
        open.pop();
        Cell* cell = items[vi].cell;
        if (cell == to) {
            found = vi;
            break;
        }
        size_t ci = (size_t)(cell->pos.y * playfield.W + cell->pos.x);
        if (closed[ci]) continue;
        closed[ci] = 1;
        for (int i = 0; i < MAX_DIRS; i++) {
            int index = (i + dirOffset) % MAX_DIRS;
            Cell* next = playfield.get(VDIR[index] + cell->pos);
            if (!playfield.isWalkable(next)) continue;
            if (closed[(size_t)(next->pos.y * playfield.W + next->pos.x)]) continue;
            int cumulative = items[vi].cumulative + (minionWeights ? 1 + EFFECT_REPULSE_WEIGHT * (next->minionRepulse ? 1 : 0) : 1);
            int total = cumulative + cell->pos.manhattan(next->pos);
            items.push_back({next, cumulative, vi});
            open.push(Key{total, seq++, (int)items.size() - 1});
        }
    }
    PathResult r;
    if (found < 0) return r; // NO_PATH
    for (int i = found; i >= 0; i = items[i].precedent) r.path.push_back(items[i].cell);
    std::reverse(r.path.begin(), r.path.end());
    r.noPath = false;
    r.weightedLength = items[found].cumulative;
    return r;
}

// Effect#doBFSEffect: every walkable cell within `depth` steps of start, each once (the order of the
// HashSets does not matter: the effects only heal or flag)
template <class F>
void doBFSEffect(Cell* start, int depth, F fn) {
    std::vector<char> seen(playfield.cells.size(), 0);
    std::vector<Cell*> level{start};
    seen[(size_t)(start->pos.y * playfield.W + start->pos.x)] = 1;
    for (int i = 0; i <= depth; i++) {
        std::vector<Cell*> next;
        for (Cell* cell : level) {
            fn(cell);
            for (int dir = 0; dir < 4; dir++) {
                Cell* n = playfield.get(cell->pos + VDIR[dir]);
                if (!playfield.isWalkable(n)) continue;
                size_t k = (size_t)(n->pos.y * playfield.W + n->pos.x);
                if (seen[k]) continue;
                seen[k] = 1;
                next.push_back(n);
            }
        }
        level = next;
    }
}

void Effect::doEffect() {
    if (!isFinished()) {
        if (type == EFFECT_PLAN_TYPE) {
            caster->heal(EFFECT_PLAN_HEAL);
            PlayerUnit* c = caster;
            doBFSEffect(playfield.get(c->pos), EFFECT_PLAN_HEAL_RADIUS, [c](Cell* cell) {
                cell->emitEvent(CellEvent::PLAN);
                for (PlayerUnit* player : livingPlayers)
                    if (player != c && player->isOnCell(cell)) {
                        player->heal(EFFECT_PLAN_HEAL);
                        c->heal(EFFECT_PLAN_HEAL);
                    }
            });
        } else if (type == EFFECT_REPULSE_TYPE) {
            doBFSEffect(caster->getCurrentCell(), EFFECT_REPULSE_RADIUS, [](Cell* cell) {
                cell->minionRepulse = true;
                cell->emitEvent(CellEvent::REPULSE);
            });
        } else if (type == EFFECT_SHELTER_TYPE) {
            cell->emitEvent(CellEvent::SHELTER, tick);
            int totalHeal = 0;
            for (PlayerUnit* p : livingPlayers)
                if (p->getCurrentCell() == cell) {
                    p->heal(EFFECT_SHELTER_HEAL);
                    totalHeal += 1;
                }
            tick = std::max(0, tick - totalHeal);
        }
        // YellEffect: nothing
    }
    if (isTimer()) tick--; // TimerEffect#doEffect
}

// ----------------------------------------------------------------------------
// Minions
// ----------------------------------------------------------------------------

// Minion#fisherYatesShuffle: a random order of livingPlayers' indices, the previous target first
std::vector<int> fisherYatesShuffle(PlayerUnit* target) {
    int n = (int)livingPlayers.size();
    std::vector<int> shuffle(n);
    for (int i = 0; i < n; i++) shuffle[i] = i;
    int start = 0;
    if (target != nullptr && contains(livingPlayers, target)) {
        int index = (int)(std::find(livingPlayers.begin(), livingPlayers.end(), target) - livingPlayers.begin());
        shuffle[index] = 0;
        shuffle[0] = index;
        start = 1;
    }
    for (int i = start; i < n - 1; i++) {
        int rnd = random_.nextInt(n - i) + i;
        std::swap(shuffle[i], shuffle[rnd]);
    }
    return shuffle;
}

void Minion::chaseClosestPlayers() {
    PathResult best;
    bool hasBest = false;
    PlayerUnit* newTarget = nullptr;
    std::vector<int> order = fisherYatesShuffle(target);
    for (int index : order) {
        PlayerUnit* player = livingPlayers[index];
        PathResult p = findPath(getCurrentCell(), player->getCurrentCell(), turn_, true);
        if (p.noPath) continue;
        if (!hasBest || p.weightedLength < best.weightedLength || (p.weightedLength == best.weightedLength && player == target)) {
            newTarget = player;
            best = p;
            hasBest = true;
        }
    }
    if (hasBest) {
        target = newTarget;
        if (best.hasNextCell()) move(best.nextCell());
    }
}

bool Minion::spookPlayersInCell(Cell* cell) {
    cell->emitEvent(CellEvent::SPOOK);
    std::vector<PlayerUnit*> frightened;
    for (PlayerUnit* p : livingPlayers)
        if (p->isOnCell(cell)) frightened.push_back(p);
    for (PlayerUnit* p : frightened) p->spook(MINION_SANITY_LOST);
    return !frightened.empty();
}

bool Minion::isPlayerInLoS(const PlayerUnit* player) {
    for (int i = 0; i < MAX_DIRS; i++)
        for (Cell* cell = playfield.get(pos); playfield.isWalkable(cell); cell = playfield.get(cell->pos + VDIR[i]))
            if (player->isOnCell(cell)) return true;
    return false;
}

std::vector<PlayerUnit*> Minion::acquireClosestTargets() {
    int minDepth = std::max(playfield.W, playfield.H); // maze.dim.tchebychevDistance()
    std::vector<PlayerUnit*> targets;
    for (int i = 0; i < MAX_DIRS; i++) {
        for (Cell* cell = playfield.get(pos); playfield.isWalkable(cell); cell = playfield.get(cell->pos + VDIR[i])) {
            std::vector<PlayerUnit*> possible = playersOnCell(cell);
            if (possible.empty()) continue;
            int depth = cell->pos.tchebychev(pos);
            if (depth < minDepth) {
                targets = possible;
                minDepth = depth;
            } else if (depth == minDepth) {
                targets.insert(targets.end(), possible.begin(), possible.end());
            }
            break;
        }
    }
    std::vector<PlayerUnit*> distinct;
    for (PlayerUnit* p : targets)
        if (!contains(distinct, p)) distinct.push_back(p);
    return distinct;
}

bool Minion::acquireTarget() {
    if (target != nullptr && target->isDead()) {
        target = nullptr;
        targetPositionBeforePreparation = nullptr;
    }
    if (target != nullptr && isPlayerInLoS(target)) {
        targetPositionBeforePreparation = target->getCurrentCell();
        return true;
    }
    std::vector<PlayerUnit*> targets = acquireClosestTargets();
    if (targets.empty()) {
        target = nullptr;
        return false;
    } else if (targets.size() == 1) {
        target = targets[0];
        targetPositionBeforePreparation = target->getCurrentCell();
        return true;
    } else {
        target = nullptr;
        targetPositionBeforePreparation = nullptr;
        return false;
    }
}

void Minion::rush() {
    if (acquireTarget()) move(target->getCurrentCell());
    else if (targetPositionBeforePreparation != nullptr) move(targetPositionBeforePreparation);
    spookPlayersInCell(getCurrentCell());
    target = nullptr;
    targetPositionBeforePreparation = nullptr;
}

void Minion::update() {
    if (!slasher) { // MinionWanderer#update
        switch (state) {
            case MinionState::SPAWNING:
                spawnCountdown--;
                if (spawnCountdown == 0) {
                    state = MinionState::WANDERING;
                    lifeTimeCountdown = WANDERER_LIFE_TIME;
                } else {
                    getCurrentCell()->emitEvent(CellEvent::SPAWN);
                }
                break;
            case MinionState::RECALLING_FAILURE:
            case MinionState::RECALLING_SUCCESS: break;
            default:
                chaseClosestPlayers();
                if (spookPlayersInCell(getCurrentCell())) {
                    state = MinionState::RECALLING_SUCCESS;
                } else {
                    lifeTimeCountdown--;
                    if (lifeTimeCountdown == 0) state = MinionState::RECALLING_FAILURE;
                }
        }
        return;
    }
    switch (state) { // MinionSlasher#update
        case MinionState::SPAWNING:
            spawnCountdown--;
            if (spawnCountdown == 0) state = MinionState::RUSH;
            break;
        case MinionState::STALKING:
            stunCountdown--;
            acquireTarget();
            if (stunCountdown <= 0) state = MinionState::RUSH;
            break;
        case MinionState::RUSH:
            rush();
            state = MinionState::STUNNED;
            stunCountdown = SLASHER_RUSH_STUN;
            break;
        case MinionState::STUNNED:
            stunCountdown--;
            if (stunCountdown == 0) state = MinionState::WANDERING;
            break;
        case MinionState::RECALLING_FAILURE:
        case MinionState::RECALLING_SUCCESS: break;
        default:
            if (acquireTarget()) {
                stunCountdown = SLASHER_RUSH_PREP;
                state = MinionState::STALKING;
            } else {
                chaseClosestPlayers();
            }
    }
}

// new MinionWanderer(id) / new MinionSlasher(id, player): the Minion constructor draws dirOffset
Minion* newMinion(int id, bool slasher) {
    allMinions.push_back(std::make_unique<Minion>());
    Minion* m = allMinions.back().get();
    m->id = id;
    m->slasher = slasher;
    m->state = MinionState::SPAWNING;
    m->spawnCountdown = WANDERER_SPAWN_TIME;
    m->lifeTimeCountdown = WANDERER_LIFE_TIME;
    m->dirOffset = random_.nextInt(MAX_DIRS);
    m->spawnCountdown = slasher ? SLASHER_SPAWN_TIME : WANDERER_SPAWN_TIME;
    return m;
}

// ----------------------------------------------------------------------------
// The view (GraphicEntityModule 2.2 through common/gem.h's commandList mode, the game's own
// TooltipModule and EndScreenModule): only its size matters (the engine's limits), so it is built as
// the Java code builds it
// ----------------------------------------------------------------------------
gem::Module M(true);
using gem::Entity;

// codeofkutulu.tooltipModule.TooltipModule: sends all its registrations every frame
struct TooltipModule {
    struct Value {
        enum Kind { INT, BOOL, STRING } kind = INT;
        i64 i = 0;
        String s;
    };
    using Params = std::vector<std::pair<std::string, Value>>; // a HashMap<String, Object> (insertion order here)
    std::map<int, Params> registrations;
    std::set<int> removals;

    Params& getParams(int id) { return registrations[id]; }
    static void put(Params& p, const std::string& key, const Value& v) {
        for (auto& kv : p)
            if (kv.first == key) {
                kv.second = v;
                return;
            }
        p.push_back({key, v});
    }
    static void putInt(Params& p, const std::string& key, i64 v) {
        Value x;
        x.i = v;
        put(p, key, x);
    }
    static void putBool(Params& p, const std::string& key, bool v) {
        Value x;
        x.kind = Value::BOOL;
        x.i = v;
        put(p, key, x);
    }
    static void putString(Params& p, const std::string& key, const String& v) {
        Value x;
        x.kind = Value::STRING;
        x.s = v;
        put(p, key, x);
    }
    static void remove(Params& p, const std::string& key) {
        for (size_t i = 0; i < p.size(); i++)
            if (p[i].first == key) {
                p.erase(p.begin() + (long)i);
                return;
            }
    }
    void removeEntity(int id) {
        removals.insert(id);
        registrations.erase(id);
    }
    // { registrations, newExtra (always empty here), removals } through Gson
    void sendFrameData() {
        std::string s = "[{";
        bool first = true;
        for (auto& [id, params] : registrations) {
            if (!first) s += ",";
            first = false;
            s += "\"" + std::to_string(id) + "\":{";
            for (size_t i = 0; i < params.size(); i++) {
                if (i) s += ",";
                s += java::jsonQuote(java::ascii(params[i].first)) + ":";
                const Value& v = params[i].second;
                if (v.kind == Value::INT) s += std::to_string(v.i);
                else if (v.kind == Value::BOOL) s += v.i ? "true" : "false";
                else s += java::jsonQuote(v.s);
            }
            s += "}";
        }
        s += "},{},[";
        first = true;
        for (int id : removals) {
            if (!first) s += ",";
            first = false;
            s += std::to_string(id);
        }
        s += "]]";
        cg::gm.setViewData("tooltips", cg::Json::raw(s));
        removals.clear();
    }
};
TooltipModule tooltips;

// com.codingame.view.EndScreenModule: [scores, sanities]
std::vector<int> endScores, endSanities;

// AnimationFactory / TileFactory
std::deque<gem::SpriteSheetLoader> loaders;
std::map<std::string, std::vector<std::string>> animations; // name -> images of its SpriteAnimation
std::vector<std::string> tileNames;

gem::SpriteSheetLoader* createSSL(const char* source, const char* name, int width, int height, int count, int perRow) {
    loaders.emplace_back();
    gem::SpriteSheetLoader* l = &loaders.back();
    l->sourceImage = source;
    l->name = name;
    l->width = width;
    l->height = height;
    l->imageCount = count;
    l->imagesPerRow = perRow;
    l->origCol = 0;
    l->origRow = 0;
    return l;
}
void createAnimation(const char* name, gem::SpriteSheetLoader* ssl, int row) {
    Entity* sa = M.createSpriteAnimation();
    ssl->origCol = 0;
    ssl->origRow = row;
    std::vector<std::string> images = M.load(ssl);
    M.setImages(sa, images);
    animations[name] = images;
}
void initAnimations() {
    createAnimation("Slasher_spawn", createSSL("flame_animation.png", "Flames_1", 48, 48, 8, 8), 1);
    createAnimation("Slasher_down", createSSL("slasher_scythe.png", "Slasher_0", 50, 48, 4, 4), 0);
    createAnimation("Slasher_left", createSSL("slasher_scythe.png", "Slasher_1", 50, 48, 4, 4), 1);
    createAnimation("Slasher_right", createSSL("slasher_scythe.png", "Slasher_2", 50, 48, 4, 4), 2);
    createAnimation("Slasher_up", createSSL("slasher_scythe.png", "Slasher_3", 50, 48, 4, 4), 3);

    createAnimation("Slasher_Mask_spawn", createSSL("flame_animation.png", "Flames_Mask_1", 48, 48, 8, 8), 1);
    createAnimation("Slasher_Mask_down", createSSL("slasher_scythe-tint.png", "Slasher_Mask_0", 50, 48, 4, 4), 0);
    createAnimation("Slasher_Mask_left", createSSL("slasher_scythe-tint.png", "Slasher_Mask_1", 50, 48, 4, 4), 1);
    createAnimation("Slasher_Mask_right", createSSL("slasher_scythe-tint.png", "Slasher_Mask_2", 50, 48, 4, 4), 2);
    createAnimation("Slasher_Mask_up", createSSL("slasher_scythe-tint.png", "Slasher_Mask_3", 50, 48, 4, 4), 3);

    createAnimation("Wanderer_spawn", createSSL("flame_animation.png", "Flames_0", 48, 48, 8, 8), 0);
    createAnimation("Wanderer_down", createSSL("wanderer.png", "Wanderer_0", 39, 48, 4, 4), 0);
    createAnimation("Wanderer_left", createSSL("wanderer.png", "Wanderer_1", 39, 48, 4, 4), 1);
    createAnimation("Wanderer_right", createSSL("wanderer.png", "Wanderer_2", 39, 48, 4, 4), 2);
    createAnimation("Wanderer_up", createSSL("wanderer.png", "Wanderer_3", 39, 48, 4, 4), 3);

    createAnimation("Wanderer_Mask_spawn", createSSL("flame_animation.png", "Flames_0", 48, 48, 8, 8), 0);
    createAnimation("Wanderer_Mask_down", createSSL("wanderer-tint.png", "Wanderer_Mask_0", 39, 48, 4, 4), 0);
    createAnimation("Wanderer_Mask_left", createSSL("wanderer-tint.png", "Wanderer_Mask_1", 39, 48, 4, 4), 1);
    createAnimation("Wanderer_Mask_right", createSSL("wanderer-tint.png", "Wanderer_Mask_2", 39, 48, 4, 4), 2);
    createAnimation("Wanderer_Mask_up", createSSL("wanderer-tint.png", "Wanderer_Mask_3", 39, 48, 4, 4), 3);

    createAnimation("Player_down", createSSL("player.png", "Player_0", 32, 48, 4, 4), 0);
    createAnimation("Player_left", createSSL("player.png", "Player_1", 32, 48, 4, 4), 1);
    createAnimation("Player_right", createSSL("player.png", "Player_2", 32, 48, 4, 4), 2);
    createAnimation("Player_up", createSSL("player.png", "Player_3", 32, 48, 4, 4), 3);
    createAnimation("Player_wait", createSSL("player.png", "Player_WAIT", 32, 48, 4, 4), 4);
    createAnimation("Player_stun", createSSL("player.png", "Player_STUN", 32, 48, 4, 4), 5);

    createAnimation("Player_Mask_down", createSSL("player-tint.png", "Player_Mask_0", 32, 48, 4, 4), 0);
    createAnimation("Player_Mask_left", createSSL("player-tint.png", "Player_Mask_1", 32, 48, 4, 4), 1);
    createAnimation("Player_Mask_right", createSSL("player-tint.png", "Player_Mask_2", 32, 48, 4, 4), 2);
    createAnimation("Player_Mask_up", createSSL("player-tint.png", "Player_Mask_3", 32, 48, 4, 4), 3);
    createAnimation("Player_Mask_wait", createSSL("player-tint.png", "Player_Mask_WAIT", 32, 48, 4, 4), 4);
    createAnimation("Player_Mask_stun", createSSL("player-tint.png", "Player_Mask_STUN", 32, 48, 4, 5), 5);
}
void initTiles() {
    gem::SpriteSheetLoader* l = createSSL("tiles.png", "TILE_", 48, 48, 8 * 8, 8);
    tileNames = M.load(l);
}
String tile(int i) { return java::decodeUtf8(tileNames[(size_t)i]); }

// CellProximityStyle tile indices
const int SOLO = 0, P_LEFT = 1, P_RIGHT = 2, P_UP = 3, P_DOWN = 4, HORIZONTAL = 5, VERTICAL = 6, CORNER_UP_LEFT = 7, CORNER_UP_RIGHT = 8,
          CORNER_DOWN_LEFT = 9, CORNER_DOWN_RIGHT = 10, T_UP = 11, T_RIGHT = 12, T_DOWN = 13, T_LEFT = 14, SQUARE = 15;

int colors[4] = {0, 0, 0, 0}; // ViewController.colors

// AnimationWithMask
struct AnimationWithMask {
    std::string baseName;
    Entity* animation = nullptr;
    Entity* mask = nullptr;
    int maskColor = 0xFFFFFF;

    explicit AnimationWithMask(const std::string& base) : baseName(base) {
        animation = M.createSpriteAnimation();
        M.setAnchorX(animation, .5);
        M.setX(animation, CELL_SIZE / 2);
        M.setY(animation, 0);
        M.setDuration(animation, 500);
        M.setVisible(animation, true);
        M.setZIndex(animation, 5);
        mask = M.createSpriteAnimation();
        M.setAnchorX(mask, .5);
        M.setX(mask, CELL_SIZE / 2);
        M.setY(mask, 0);
        M.setDuration(mask, 500);
        M.setVisible(mask, true);
        M.setZIndex(mask, 5);
        M.setAlpha(mask, 1.0);
    }
    void setScale(double s) {
        M.setScale(animation, s);
        M.setScale(mask, s);
    }
    void setVisible(bool b) {
        M.setVisible(animation, b);
        M.setVisible(mask, b);
    }
    void setMaskAlpha(double v) { M.setAlpha(mask, v); }
    void setAnimation(const char* index) {
        M.setImages(animation, animations.at(baseName + "_" + index));
        M.setImages(mask, animations.at(baseName + "_Mask_" + index));
        M.reset(animation);
        M.reset(mask);
        M.commitEntityState(0, {animation, mask});
    }
    void setTint(int color) { M.setTint(animation, color); }
    void setMaskTint(int color, gem::Curve curve) { M.setTint(mask, color, curve); }
    void setMaskTint(int color) {
        maskColor = color;
        M.setTint(mask, maskColor);
    }
    int getId() const { return animation->id; }
    void setZIndex(int z) {
        M.setZIndex(animation, z);
        M.setZIndex(mask, z);
    }
    void setAlpha(double d, gem::Curve curve) {
        M.setAlpha(animation, d, curve);
        M.setAlpha(mask, d, curve);
    }
    void setScaleX(int s, gem::Curve curve) {
        M.setScaleX(animation, s, curve);
        M.setScaleX(mask, s, curve);
    }
    double getScaleY() const { return animation->scaleY; }
    void setScaleY(double s, gem::Curve curve) {
        M.setScaleY(animation, s, curve);
        M.setScaleY(mask, s, curve);
    }
};

struct AbstractNode {
    bool toBeRemoved = false;
    virtual ~AbstractNode() {}
    virtual void updateView() = 0;
    virtual void teardown() {}
    virtual void updateToolTip() {}
    virtual int getTooltipEntityId() const { return -1; }
    void removeTooltip() { tooltips.removeEntity(getTooltipEntityId()); }
};

struct KPlayer;

// ScoreNode
const int GLOB_MARGIN_TOP = 20, GLOB_MARGIN_LEFT = 20, SCORE_HEIGHT = (1080 - GLOB_MARGIN_TOP) / 4, MARGIN_Y = 5;
const int NICKNAME_HEIGHT = SCORE_HEIGHT / 3, SANITY_WIDTH = 300, SANITY_HEIGHT = 40;
const int AVATAR_SIZE = NICKNAME_HEIGHT + SANITY_HEIGHT, SANITY_LEFT = AVATAR_SIZE + 2 * MARGIN_Y;

struct ScoreNode : AbstractNode {
    KPlayer* player;
    Entity *scoreBoard, *sanityGroup, *sanityView, *sanityViewFill, *sanityText, *sanityBackground, *isolatedStatus, *avatarView, *name, *message;
    Entity *planSprite, *planText, *lightSprite, *lightText, *sanityTextMask, *sanityForegroundText;
    Entity* yells[3] = {nullptr, nullptr, nullptr};
    explicit ScoreNode(KPlayer* p);
    void updateView() override;
    void updateToolTip() override {
        TooltipModule::Params& params = tooltips.getParams(isolatedStatus->id);
        TooltipModule::putString(params, "entity", A("message"));
        TooltipModule::putString(params, "message", A("Isolated"));
    }
};

struct PlayerNode : AbstractNode {
    KPlayer* cokPlayer;
    PlayerUnit* model;
    Entity *group, *rip;
    std::unique_ptr<AnimationWithMask> animation;
    bool spooked = false;
    PlayerNode(KPlayer* p, PlayerUnit* unit);
    void updateView() override;
    void teardown() override { spooked = false; }
    int getTooltipEntityId() const override { return group->id; }
    void updateToolTip() override;
};

struct GroundNode : AbstractNode {
    Cell* model;
    Entity *group, *background, *effectRectangle;
    int plan = 0, spook = 0, shelter = 0, spawn = 0, repulse = 0;
    explicit GroundNode(Cell* c) : model(c) {
        c->observer = this;
        group = M.createGroup();
        M.setX(group, c->pos.x * CELL_SIZE);
        M.setY(group, c->pos.y * CELL_SIZE);
        // createView
        background = M.createSprite();
        M.setImage(background, tile(24 + (c->pos.x + c->pos.y) % 12));
        M.setBaseHeight(background, CELL_SIZE);
        M.setBaseWidth(background, CELL_SIZE);
        M.setAlpha(background, 1.0);
        M.setZIndex(background, -1);
        effectRectangle = M.createRectangle();
        M.setAlpha(effectRectangle, 0.0);
        M.setX(effectRectangle, 0);
        M.setY(effectRectangle, 0);
        M.setWidth(effectRectangle, CELL_SIZE);
        M.setHeight(effectRectangle, CELL_SIZE);
        M.setZIndex(effectRectangle, 5);
        if (c->type != CellType::SHELTER && HAS_WANDERER && c->spawnPoint) M.setImage(background, tile(8 * 4 + 4));
        M.add(group, {effectRectangle});
        // CellNode
        M.add(group, {background});
    }
    void updateView() override {
        if (isKutuluComing()) M.setTint(background, 0x225522);
        M.setAlpha(effectRectangle, 0.0);
        if (HAS_SHELTER && model->type == CellType::SHELTER) M.setImage(background, tile(shelter == 0 ? 8 * 4 + 6 : 8 * 4 + 7));
        if (model->spawnPoint) M.setImage(background, tile(spawn == 0 ? 8 * 4 + 4 : 8 * 4 + 5));
        if (plan > 0) {
            M.setFillColor(effectRectangle, 0x0000FF);
            M.setAlpha(effectRectangle, java::min(1.0, 0.2 * plan));
        } else if (repulse > 0) {
            M.setFillColor(effectRectangle, 0xFFFF00);
            M.setAlpha(effectRectangle, java::min(1.0, 0.3 * repulse));
        }
    }
    void updateToolTip() override {
        if (HAS_SHELTER && model->type == CellType::SHELTER) {
            TooltipModule::Params& params = tooltips.getParams(getTooltipEntityId());
            TooltipModule::putString(params, "entity", A("shelter"));
            TooltipModule::putInt(params, "energy", shelter);
        }
    }
    void teardown() override { plan = spook = shelter = spawn = repulse = 0; }
    int getTooltipEntityId() const override { return group->id; }
    void onEvent(CellEvent e, int energy) {
        if (e == CellEvent::SHELTER) shelter += energy;
        if (e == CellEvent::PLAN) plan++;
        if (e == CellEvent::SPOOK) spook++;
        if (e == CellEvent::REPULSE) repulse++;
        if (e == CellEvent::SPAWN) spawn++;
    }
};

void Cell::emitEvent(CellEvent e, int energy) {
    if (observer) observer->onEvent(e, energy);
}

struct WallNode : AbstractNode {
    Cell* model;
    Entity *group, *background;
    explicit WallNode(Cell* c) : model(c) {
        group = M.createGroup();
        M.setX(group, c->pos.x * CELL_SIZE);
        M.setY(group, c->pos.y * CELL_SIZE);
        background = M.createSprite();
        M.setImage(background, tile(computeProximity()));
        M.setBaseHeight(background, CELL_SIZE);
        M.setBaseWidth(background, CELL_SIZE);
        M.setAlpha(background, 1.0);
        M.setTint(background, 0xA0A080);
        M.setZIndex(background, -1);
        M.add(group, {background});
    }
    void updateView() override {}
    int getTooltipEntityId() const override { return group->id; }
    int computeProximity() const {
        CellType stay = model->type;
        CellType up = playfield.get(model->pos + VDIR[UP])->type, down = playfield.get(model->pos + VDIR[DOWN])->type;
        CellType right = playfield.get(model->pos + VDIR[RIGHT])->type, left = playfield.get(model->pos + VDIR[LEFT])->type;
        bool u = stay == up, d = stay == down, r = stay == right, l = stay == left;
        if (u && d && r && l) return SQUARE;
        if (!u && !d && !r && !l) return SOLO;
        if (u && d && r && !l) return T_LEFT;
        if (!u && !d && !r && l) return P_LEFT;
        if (u && d && !r && l) return T_RIGHT;
        if (!u && !d && r && !l) return P_RIGHT;
        if (u && !d && r && l) return T_DOWN;
        if (!u && d && !r && !l) return P_DOWN;
        if (!u && d && r && l) return T_UP;
        if (u && !d && !r && !l) return P_UP;
        if (u && d && !r && !l) return VERTICAL;
        if (!u && !d && r && l) return HORIZONTAL;
        if (u && !d && !r && l) return CORNER_DOWN_RIGHT;
        if (u && !d && r && !l) return CORNER_DOWN_LEFT;
        if (!u && d && !r && l) return CORNER_UP_RIGHT;
        if (!u && d && r && !l) return CORNER_UP_LEFT;
        return SQUARE;
    }
};

struct MinionNode : AbstractNode {
    Minion* minion;
    Entity* baseNode;
    std::unique_ptr<AnimationWithMask> animation;
    Entity* statusMark = nullptr; // SlasherMinionNode
    explicit MinionNode(Minion* m) : minion(m) {
        animation = std::make_unique<AnimationWithMask>("Wanderer");
        animation->setScale(1.8);
        animation->setMaskAlpha(1.0);
        baseNode = M.createGroup({animation->animation, animation->mask});
        setPosition();
        M.commitEntityState(0, {baseNode});
        if (m->slasher) {
            animation->baseName = "Slasher";
            statusMark = M.createText(String());
            M.setAnchorX(statusMark, .5);
            M.setAnchorY(statusMark, 1);
            M.setX(statusMark, CELL_SIZE / 2);
            M.setFillColor(statusMark, 0xFFFFFF);
            M.add(baseNode, {statusMark});
        } else {
            animation->setZIndex(2);
        }
    }
    void setPosition() {
        M.setX(baseNode, minion->pos.x * CELL_SIZE + 0);
        M.setY(baseNode, minion->pos.y * CELL_SIZE + 0 - CELL_SIZE / 4);
    }
    int trackedPlayerColor() const { return minion->target != nullptr ? colors[minion->target->id] : 0xFFFFFF; }
    int getTooltipEntityId() const override { return animation->getId(); }
    void updateView() override {
        setPosition();
        if (minion->slasher) { // SlasherMinionNode
            animation->setVisible(true);
            if (minion->state == MinionState::SPAWNING) {
                animation->setAnimation("spawn");
            } else {
                animation->setAnimation(DIRECTIONS[minion->orientation]);
                if (minion->target != nullptr) animation->setMaskTint(trackedPlayerColor(), gem::EASE_IN_AND_OUT);
                else animation->setMaskTint(0xFFFFFF);
            }
            animation->setVisible(true);
            switch (minion->state) {
                case MinionState::SPAWNING: break;
                case MinionState::STALKING: M.setText(statusMark, A("!")); break;
                case MinionState::RUSH: M.setText(statusMark, A("Fwuush!")); break;
                case MinionState::STUNNED: {
                    int from = java::d2i((3 * minion->getCountdown() + 0.5) / SLASHER_RUSH_STUN);
                    String dots = A("....");
                    if (from < 0 || from > 4) throw java::Exception{"java.lang.StringIndexOutOfBoundsException: begin " + std::to_string(from) + ", end 4, length 4"};
                    M.setText(statusMark, dots.substr((size_t)from));
                    break;
                }
                default:
                    M.setText(statusMark, String());
                    animation->setVisible(true);
            }
            return;
        }
        // WandererMinionNode
        if (minion->state == MinionState::SPAWNING) animation->setAnimation("spawn");
        else animation->setAnimation(DIRECTIONS[minion->orientation]);
        animation->setVisible(true);
        if (minion->isBeingRecalled()) {
            animation->setAlpha(0.0, gem::LINEAR);
            animation->setScaleX(0, gem::LINEAR);
            animation->setScaleY(animation->getScaleY() * 2, gem::EASE_IN_AND_OUT);
            toBeRemoved = true;
        } else if (minion->target != nullptr) {
            animation->setMaskTint(trackedPlayerColor(), gem::EASE_IN_AND_OUT);
        }
    }
    void updateToolTip() override {
        TooltipModule::Params& params = tooltips.getParams(getTooltipEntityId());
        TooltipModule::putString(params, "entity", A("minion"));
        TooltipModule::putInt(params, "id", minion->id);
        TooltipModule::putString(params, "type", A(minion->slasher ? "SLASHER" : "WANDERER"));
        if (minion->isSpawning()) TooltipModule::putInt(params, "spawn", minion->spawnCountdown);
        else TooltipModule::remove(params, "spawn");
        if (minion->slasher) {
            TooltipModule::putString(params, "state", A(MINION_STATE_NAMES[(int)minion->state]));
        } else {
            TooltipModule::putInt(params, "life", minion->lifeTimeCountdown);
            if (minion->target != nullptr) TooltipModule::putInt(params, "target", minion->target->id);
            else TooltipModule::putString(params, "target", String());
        }
    }
};

// MapNode
struct MapNode {
    Entity* group = nullptr;
    double bestFitScale() const { return java::min((1080.0 / playfield.H) / (double)CELL_SIZE, (1350.0 / playfield.W) / (double)CELL_SIZE); }
    int left() const { return java::d2i(SCREEN_W - playfield.W * CELL_SIZE * bestFitScale()); }
    void create() {
        group = M.createGroup();
        M.setX(group, left());
        M.setY(group, 0);
        M.setVisible(group, true);
        M.setZIndex(group, 1);
        M.setScale(group, bestFitScale());
        // createMapName
        Entity* text = M.createText(java::decodeUtf8(maze->name));
        M.setFontSize(text, 30);
        M.setX(text, CELL_SIZE * playfield.W / 2);
        M.setY(text, 10);
        M.setFillColor(text, 0xF0EFEF);
        M.setAnchorX(text, .5);
        M.setVisible(text, true);
        M.setRotation(text, -.01);
        M.setStrokeColor(text, 0x601010);
        M.setStrokeThickness(text, 10);
        M.setZIndex(text, 2);
        M.add(group, {text});
        // createTentacles
        Entity* tentacle = M.createSprite();
        M.setImage(tentacle, "kutulu_tentacle.png");
        M.setX(tentacle, CELL_SIZE * playfield.W);
        M.setY(tentacle, CELL_SIZE * playfield.H);
        M.setBaseHeight(tentacle, CELL_SIZE * 4);
        M.setBaseWidth(tentacle, CELL_SIZE * 4);
        M.setScaleX(tentacle, -1);
        M.setScaleY(tentacle, -1);
        M.setZIndex(tentacle, 10);
        M.add(group, {tentacle});
    }
    void updateToolTip() {
        TooltipModule::Params& params = tooltips.getParams(group->id);
        TooltipModule::putString(params, "entity", A("map"));
        TooltipModule::putInt(params, "startx", left());
        TooltipModule::putInt(params, "starty", 0);
        TooltipModule::putString(params, "size", I(CELL_SIZE));
        TooltipModule::putString(params, "scale", java::ascii(java::doubleToString(bestFitScale())));
    }
};

// ViewController
struct ViewController {
    std::vector<std::unique_ptr<AbstractNode>> viewNodes;
    MapNode map;
    void update() {
        map.updateToolTip();
        for (auto& node : viewNodes) {
            node->updateView();
            node->updateToolTip();
            node->teardown();
        }
        for (auto& node : viewNodes)
            if (node->toBeRemoved) node->removeTooltip();
        viewNodes.erase(std::remove_if(viewNodes.begin(), viewNodes.end(), [](const std::unique_ptr<AbstractNode>& n) { return n->toBeRemoved; }),
                        viewNodes.end());
    }
    void createMinionView(Minion* m) {
        auto mn = std::make_unique<MinionNode>(m);
        M.setZIndex(mn->baseNode, 1);
        M.add(map.group, {mn->baseNode});
        M.commitEntityState(0, {map.group});
        viewNodes.push_back(std::move(mn));
    }
};
std::unique_ptr<ViewController> view;

// ----------------------------------------------------------------------------
// Player, Referee
// ----------------------------------------------------------------------------
struct KPlayer : cg::Player {
    PlayerUnit* unit = nullptr;
    String displayMessage;
    int getExpectedOutputLines() const override { return 1; }
    int getColorToken() const { return -(index + 1); }
    void setDisplayMessage(const String& m) {
        String message = m;
        if ((i64)message.size() > 26) message = message.substr(0, (size_t)std::max(0, 26 - 6)) + A(" (...)");
        displayMessage = message;
    }
};
KPlayer players[4];

void PlayerUnit::spook(int amount) {
    if (observer) observer->spooked = true; // SpookEvent
    madden(amount);
}

ScoreNode::ScoreNode(KPlayer* p) : player(p) {
    int color = p->getColorToken();
    // buildMetaInformationViewNodes
    avatarView = M.createSprite();
    M.setBaseWidth(avatarView, AVATAR_SIZE);
    M.setBaseHeight(avatarView, AVATAR_SIZE);
    M.setZIndex(avatarView, 20);
    M.setImage(avatarView, p->getNicknameToken()); // getAvatarToken: "$" + index too
    name = M.createText(p->getNicknameToken());
    M.setX(name, SANITY_LEFT);
    M.setFillColor(name, color);
    M.setScale(name, 1.5);
    M.setFontFamily(name, "Arial Black");
    message = M.createText(String());
    M.setX(message, SANITY_LEFT);
    M.setY(message, 2 * name->fontSize);
    M.setFillColor(message, color);
    M.setScale(message, 1.0);
    // buildSanityViewNodes
    sanityBackground = M.createSprite();
    M.setImage(sanityBackground, "sanity_background.png");
    M.setBaseWidth(sanityBackground, SANITY_WIDTH);
    M.setBaseHeight(sanityBackground, SANITY_HEIGHT);
    M.setAlpha(sanityBackground, 0.5);
    M.setZIndex(sanityBackground, 2);
    M.setVisible(sanityBackground, true);
    sanityView = M.createRectangle();
    M.setWidth(sanityView, SANITY_WIDTH);
    M.setHeight(sanityView, SANITY_HEIGHT);
    M.setFillColor(sanityView, color);
    M.setZIndex(sanityView, 1);
    sanityViewFill = M.createRectangle();
    M.setWidth(sanityViewFill, SANITY_WIDTH);
    M.setHeight(sanityViewFill, SANITY_HEIGHT);
    M.setFillColor(sanityViewFill, 0xD0D0D0);
    M.setAlpha(sanityViewFill, 0.5);
    M.setZIndex(sanityViewFill, 0);
    sanityText = M.createText(String());
    M.setX(sanityText, SANITY_WIDTH / 2);
    M.setScale(sanityText, 1.2);
    M.setAnchorX(sanityText, .5);
    M.setFontFamily(sanityText, "Arial Black");
    M.setStrokeThickness(sanityText, 1);
    M.setStrokeColor(sanityText, 0x000000);
    M.setFillColor(sanityText, color);
    M.setTint(sanityText, 0xFFFFFF);
    M.setZIndex(sanityText, 3);
    sanityTextMask = M.createRectangle();
    M.setWidth(sanityTextMask, sanityView->width);
    M.setHeight(sanityTextMask, sanityView->height);
    sanityForegroundText = M.createText(String());
    M.setX(sanityForegroundText, sanityText->x);
    M.setScale(sanityForegroundText, sanityText->scaleX);
    M.setAnchorX(sanityForegroundText, .5);              // sanityText.getAnchorX()
    M.setFontFamily(sanityForegroundText, "Arial Black"); // sanityText.getFontFamily()
    M.setStrokeThickness(sanityForegroundText, 1);
    M.setMask(sanityForegroundText, sanityTextMask);
    M.setStrokeColor(sanityForegroundText, color);
    M.setFillColor(sanityForegroundText, 0x000000);
    M.setZIndex(sanityForegroundText, 4);
    sanityGroup = M.createGroup({sanityBackground, sanityViewFill, sanityView, sanityText, sanityTextMask, sanityForegroundText});
    M.setX(sanityGroup, SANITY_LEFT);
    M.setY(sanityGroup, NICKNAME_HEIGHT);
    // buildStatusViewNodes
    isolatedStatus = M.createSprite();
    M.setImage(isolatedStatus, tile(8 * 7 + 4));
    M.setBaseHeight(isolatedStatus, SANITY_HEIGHT);
    M.setBaseWidth(isolatedStatus, SANITY_HEIGHT);
    M.setZIndex(isolatedStatus, 20);
    M.setX(isolatedStatus, SANITY_LEFT + SANITY_WIDTH + 2 * MARGIN_Y);
    M.setY(isolatedStatus, NICKNAME_HEIGHT);
    // buildItemsViewNodes
    planSprite = M.createSprite();
    M.setImage(planSprite, tile(8 * 7 + 2));
    M.setX(planSprite, SANITY_LEFT + MARGIN_Y);
    M.setY(planSprite, NICKNAME_HEIGHT + SANITY_HEIGHT + 16);
    M.setBaseHeight(planSprite, SANITY_HEIGHT);
    M.setBaseWidth(planSprite, SANITY_HEIGHT);
    planText = M.createText(String());
    M.setX(planText, planSprite->x + 4 + SANITY_HEIGHT);
    M.setY(planText, planSprite->y);
    M.setScale(planText, 1.2);
    M.setFillColor(planText, 0xFFFFFF);
    lightSprite = M.createSprite();
    M.setImage(lightSprite, tile(8 * 7 + 1));
    M.setX(lightSprite, SANITY_LEFT + MARGIN_Y + 64);
    M.setY(lightSprite, NICKNAME_HEIGHT + SANITY_HEIGHT + 16);
    M.setBaseHeight(lightSprite, SANITY_HEIGHT);
    M.setBaseWidth(lightSprite, SANITY_HEIGHT);
    lightText = M.createText(String());
    M.setX(lightText, lightSprite->x + 4 + SANITY_HEIGHT);
    M.setY(lightText, lightSprite->y);
    M.setScale(lightText, 1.2);
    M.setFillColor(lightText, 0xFFFFFF);
    // buildYellsNodes: yells[index++] = ...setX(... + index * 34): the index is incremented before the
    // right-hand side is evaluated
    int index = 0;
    for (int i = 0; i < 4; i++) {
        if (i == p->getIndex()) continue;
        int slot = index++;
        Entity* y = M.createSprite();
        M.setImage(y, tile(8 * 7 + 0));
        M.setX(y, SANITY_LEFT + MARGIN_Y + 2 * 64 + index * (32 + 2));
        M.setY(y, NICKNAME_HEIGHT + SANITY_HEIGHT + 16);
        M.setBaseWidth(y, 32);
        M.setBaseHeight(y, 32);
        M.setTint(y, colors[i]);
        M.setAlpha(y, 0.0);
        yells[slot] = y;
    }
    // createViewGroup
    scoreBoard = M.createGroup({avatarView, name, message, sanityGroup, planSprite, planText, lightSprite, lightText, isolatedStatus, yells[0], yells[1], yells[2]});
    M.setX(scoreBoard, GLOB_MARGIN_LEFT);
    M.setY(scoreBoard, GLOB_MARGIN_TOP + p->getIndex() * SCORE_HEIGHT);
}

void ScoreNode::updateView() {
    if (!player->isActive()) M.setTint(avatarView, 0x222222);
    M.setText(message, player->displayMessage);
    M.setWidth(sanityViewFill, sanityView->width);
    M.setWidth(sanityView, SANITY_WIDTH * player->unit->sanity / PLAYER_MAX_SANITY);
    int sanity = player->unit->sanity;
    M.setText(sanityText, sanity > 0 ? I(sanity) : String(u"✝"));
    M.setWidth(sanityTextMask, sanityView->width);
    M.setText(sanityForegroundText, sanityText->text);
    M.setVisible(isolatedStatus, player->unit->isIsolated);
    M.setText(planText, I(player->unit->remainingPlans));
    M.setText(lightText, I(player->unit->remainingLight));
    int index = 0;
    for (int i = 0; i < 4; i++) {
        if (i == player->getIndex()) continue;
        if (player->unit->hasBeenStuckedBy(i)) M.setAlpha(yells[index], 1.0);
        index++;
    }
}

PlayerNode::PlayerNode(KPlayer* p, PlayerUnit* unit) : cokPlayer(p), model(unit) {
    unit->observer = this;
    rip = M.createSprite();
    M.setImage(rip, tile(8 * 7 + 3));
    M.setAlpha(rip, .8);
    M.setZIndex(rip, 0);
    M.setTint(rip, p->getColorToken());
    M.setBaseWidth(rip, CELL_SIZE);
    M.setBaseHeight(rip, CELL_SIZE);
    M.setVisible(rip, false);
    animation = std::make_unique<AnimationWithMask>("Player");
    animation->setScale(1.6);
    animation->setMaskTint(p->getColorToken());
    group = M.createGroup({animation->animation, animation->mask, rip});
}

void PlayerNode::updateView() {
    M.setX(group, model->pos.x * CELL_SIZE + java::d2i(4 * (cokPlayer->getIndex() - 1.5)));
    M.setY(group, model->pos.y * CELL_SIZE - CELL_SIZE / 4);
    if (model->isDead()) {
        animation->setVisible(false);
        M.setVisible(rip, true);
        M.setZIndex(group, 1);
        toBeRemoved = true;
    } else {
        if (model->isStuck()) animation->setAnimation("stun");
        else animation->setAnimation(DIRECTIONS[model->orientation]);
        animation->setTint(spooked ? 0x222222 : 0xFFFFFF);
    }
}

void PlayerNode::updateToolTip() {
    if (model->isDead()) {
        tooltips.removeEntity(getTooltipEntityId());
        return;
    }
    TooltipModule::Params& params = tooltips.getParams(getTooltipEntityId());
    TooltipModule::putString(params, "entity", A("player"));
    TooltipModule::putInt(params, "id", model->id);
    TooltipModule::putInt(params, "sanity", model->sanity);
    if (HAS_ISOLATED_FEAR) TooltipModule::putBool(params, "isolated", model->isIsolated);
    String items;
    if (HAS_PLAN) items += A("plans: ") + I(model->remainingPlans);
    if (HAS_LIGHT) {
        if (!items.empty()) items += A(" / ");
        items += A("lights: ") + I(model->remainingLight);
    }
    TooltipModule::putString(params, "items", items);
}

// PlayerAction(player, line): the two patterns of the referee, MOVE first
//   ^MOVE (?<x>-?\d+) (?<y>-?\d+)(?:\s+(?<message>.+))?\s*$   and   ^(?<action>\w+)(?:\s+(?<message>.+))?\s*$
// both CASE_INSENSITIVE (ASCII), used with find(). A line here never holds a line terminator (the
// runner and the referee's Scanner split on them), so '.' matches anything and '$' is the end.
// The optional message group after the matched head `rest`: rest empty -> no match of the group;
// rest starting with \s: the leading \s run is skipped and the message is the rest of the line, or,
// when the rest is all \s, its last char (\s+ gives one back to .+), or nothing for a single \s.
// Returns false when the rest cannot end the pattern.
bool messageGroup(const String& line, size_t at, bool& has, String& message) {
    has = false;
    if (at == line.size()) return true;
    if (!java::isRegexSpace(line[at])) return false;
    size_t k = at;
    while (k < line.size() && java::isRegexSpace(line[k])) k++;
    if (k < line.size()) {
        has = true;
        message = line.substr(k);
    } else if (line.size() - at >= 2) {
        has = true;
        message = line.substr(line.size() - 1);
    }
    return true;
}

// parses -?\d+ at i, returns the end or npos
size_t signedDigits(const String& s, size_t i) {
    size_t j = i;
    if (j < s.size() && s[j] == u'-') j++;
    size_t d = j;
    while (j < s.size() && java::isRegexDigit(s[j])) j++;
    return j == d ? String::npos : j;
}

// throws (any Java exception the constructor throws: the referee catches them all)
struct InvalidInput {};

Action coordinatesToAction(PlayerUnit* player, int x, int y) {
    Cell* targetCell = playfield.get(x, y);
    if (targetCell == &INVALID_CELL || !playfield.isWalkable(targetCell)) return Action::WAIT;
    PathResult p = findPath(player->getCurrentCell(), targetCell, 0, false);
    if (p.noPath || !p.hasNextCell()) return Action::WAIT;
    Cell* next = p.nextCell();
    Cell* cur = player->getCurrentCell();
    if (next == playfield.get(cur->pos + V{1, 0})) return Action::RIGHT;
    if (next == playfield.get(cur->pos + V{-1, 0})) return Action::LEFT;
    if (next == playfield.get(cur->pos + V{0, -1})) return Action::UP;
    if (next == playfield.get(cur->pos + V{0, 1})) return Action::DOWN;
    return Action::WAIT;
}

PlayerAction parseAction(PlayerUnit* player, const String& line) {
    PlayerAction a;
    a.staticWait = false;
    player->performingInvalidMove = false;
    bool has = false;
    String message;
    bool moved = false;
    // pattern 1
    if (line.size() >= 5 && java::regexMatchesIgnoreCase(line.substr(0, 4), "MOVE") && line[4] == u' ') {
        size_t xe = signedDigits(line, 5);
        if (xe != String::npos && xe < line.size() && line[xe] == u' ') {
            size_t ye = signedDigits(line, xe + 1);
            if (ye != String::npos && messageGroup(line, ye, has, message)) {
                i32 x, y;
                if (!java::parseInt(line.substr(5, xe - 5), x)) throw InvalidInput{};
                if (!java::parseInt(line.substr(xe + 1, ye - xe - 1), y)) throw InvalidInput{};
                a.action = coordinatesToAction(player, x, y);
                if (a.action == Action::WAIT) player->performingInvalidMove = true;
                moved = true;
            }
        }
    }
    if (!moved) {
        // pattern 2: a no-match makes matcher.group throw IllegalStateException
        size_t e = 0;
        while (e < line.size() && java::isRegexWord(line[e])) e++;
        if (e == 0 || !messageGroup(line, e, has, message)) throw InvalidInput{};
        String name = line.substr(0, e); // toUpperCase().trim(): ASCII word chars
        for (char16_t& c : name)
            if (c >= u'a' && c <= u'z') c = (char16_t)(c - 32);
        int found = -1;
        for (int i = 0; i < 8; i++)
            if (name == A(ACTION_NAMES[i])) found = i;
        if (found < 0) throw InvalidInput{}; // Action.valueOf
        a.action = (Action)found;
    }
    // PLAN / LIGHT / YELL without their rule become WAIT: every rule is on in this league
    a.message = has ? message : String();
    return a;
}

struct KutuluReferee : cg::Referee {
    bool slasherSpawned[4] = {false, false, false, false};

    void init() override {
        turn_ = 0;
        random_ = java::Random(cg::gm.getSeed());
        // setupLeague(19): LEGEND -> ... -> WOOD_1 calculateParametrizedConstants, BRONZE's maps
        SPREAD_MADNESS_PER_TURN_AMOUNT = randomBetween(3, 6);
        SPREAD_MADNESS_PLAYER_PROX_PER_TURN_AMOUNT = randomBetween(1, 3);
        WANDERER_SPAWN_TIME = randomBetween(3, 6);
        WANDERER_LIFE_TIME = randomBetween(30, 40);
        cg::gm.setFrameDuration(500);
        // pickMaze: no "map" parameter
        i32 r = random_.nextInt();
        r = java::abs(r);
        maze = &BRONZE_MAPS[r % 16]; // Math.abs(MIN_VALUE) % 16 == 0
        int W = (int)strlen(maze->rows[0]), H = (int)maze->rows.size();
        playfield.W = W;
        playfield.H = H;
        playfield.cells.assign((size_t)(W * H), Cell());
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                Cell& c = playfield.cells[(size_t)(y * W + x)];
                c.pos = V{x, y};
                switch (maze->rows[y][x]) {
                    case 'w': c.spawnPoint = true; break;
                    case 'U':
                        c.type = CellType::SHELTER;
                        playfield.shelterCells.push_back(&c);
                        break;
                    case '#': c.type = CellType::WALL; break;
                    default: c.type = CellType::EMPTY;
                }
            }
        INVALID_CELL.pos = V{-1, -1};
        INVALID_CELL.invalid = true;

        initAnimations();
        initTiles();

        std::vector<V> starts;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (maze->rows[y][x] == 'S') starts.push_back(V{x, y});
        for (cg::Player* cp : cg::gm.getPlayers()) {
            KPlayer* p = static_cast<KPlayer*>(cp);
            allUnits.push_back(std::make_unique<PlayerUnit>());
            p->unit = allUnits.back().get();
            p->unit->id = p->getIndex();
            if (p->getIndex() >= (int)starts.size()) throw java::Exception{java::arrayIndexOutOfBoundsException(p->getIndex(), (i64)starts.size())};
            p->unit->move(playfield.get(starts[(size_t)p->getIndex()]));
            livingPlayers.push_back(p->unit);
            // sendInitData
            p->sendInputLine(std::to_string(W));
            p->sendInputLine(std::to_string(H));
            for (int y = 0; y < H; y++) {
                std::string row = maze->rows[y];
                std::replace(row.begin(), row.end(), 'S', '.');
                p->sendInputLine(row);
            }
            p->sendInputLine(std::to_string(SPREAD_MADNESS_PER_TURN_AMOUNT) + " " + std::to_string(SPREAD_MADNESS_PLAYER_PROX_PER_TURN_AMOUNT) + " " +
                             std::to_string(WANDERER_SPAWN_TIME) + " " + std::to_string(WANDERER_LIFE_TIME));
        }
        updateIsolation();
        createView();
    }

    void createView() {
        view = std::make_unique<ViewController>();
        view->map.create();
        // initView
        for (int i = 0; i < cg::gm.getPlayerCount(); i++) colors[i] = players[i].getColorToken();
        Entity* bg = M.createSprite();
        M.setImage(bg, "background.jpg");
        M.setScale(bg, 1);
        M.setAnchorX(bg, 0);
        M.setAnchorY(bg, 0);
        M.setZIndex(bg, -1);
        // createPlayersView
        for (cg::Player* cp : cg::gm.getPlayers()) {
            KPlayer* p = static_cast<KPlayer*>(cp);
            view->viewNodes.push_back(std::make_unique<ScoreNode>(p));
            auto pn = std::make_unique<PlayerNode>(p, p->unit);
            M.setZIndex(pn->group, 2);
            M.add(view->map.group, {pn->group});
            view->viewNodes.push_back(std::move(pn));
        }
        // createGrid
        for (int x = 0; x < playfield.W; x++)
            for (int y = 0; y < playfield.H; y++) {
                Cell* c = playfield.get(x, y);
                std::unique_ptr<AbstractNode> node;
                Entity* g;
                if (c->type == CellType::WALL) {
                    auto w = std::make_unique<WallNode>(c);
                    g = w->group;
                    node = std::move(w);
                } else {
                    auto gn = std::make_unique<GroundNode>(c);
                    g = gn->group;
                    node = std::move(gn);
                }
                M.setZIndex(g, 0);
                M.add(view->map.group, {g});
                view->viewNodes.push_back(std::move(node));
            }
        view->update();
    }

    void onGameInit() override {
        M.sendGlobalData();
        M.sendFrameData();
        tooltips.sendFrameData();
    }
    void onAfterGameTurn() override {
        M.sendFrameData();
        tooltips.sendFrameData();
    }
    void onAfterOnEnd() override {
        tooltips.sendFrameData();
        std::string s = "[[";
        for (size_t i = 0; i < endScores.size(); i++) s += (i ? "," : "") + std::to_string(endScores[i]);
        s += "],[";
        for (size_t i = 0; i < endSanities.size(); i++) s += (i ? "," : "") + std::to_string(endSanities[i]);
        cg::gm.setViewData("endScreen", cg::Json::raw(s + "]]"));
    }
    void onEnd() override {
        endScores.clear();
        endSanities.clear();
        for (cg::Player* cp : cg::gm.getPlayers()) {
            endScores.push_back(cp->getScore());
            endSanities.push_back(static_cast<KPlayer*>(cp)->unit->sanity);
        }
    }

    static std::vector<KPlayer*> active() {
        std::vector<KPlayer*> r;
        for (cg::Player* p : cg::gm.getActivePlayers()) r.push_back(static_cast<KPlayer*>(p));
        return r;
    }
    static std::vector<PlayerUnit*> activeUnits() {
        std::vector<PlayerUnit*> r;
        for (KPlayer* p : active()) r.push_back(p->unit);
        return r;
    }

    void gameTurn(int engineTurn) override {
        int turn = engineTurn - 1; // engine 2.2 numbers its turns from 0
        turn_ = turn;
        for (KPlayer* p : active()) sendInput(p);
        updatePlayerWishedMove();
        updateGame();
        view->update();
        checkGameEndConditions(turn);
    }

    void checkGameEndConditions(int turn) {
        if (turn >= MAX_TURNS) {
            for (KPlayer* p : active()) {
                p->setScore(MAX_TURNS + p->unit->sanity);
                p->deactivate();
            }
            cg::gm.endGame();
        }
        if (cg::gm.getActivePlayers().empty()) cg::gm.endGame();
    }

    void updatePlayerWishedMove() {
        for (KPlayer* p : active()) {
            p->setDisplayMessage(String());
            if (p->timeout) {
                killPlayer(p, A("$") + I(p->getIndex()) + A(" timeout!"));
                continue;
            }
            const std::vector<String>& outputs = p->outputs;
            try {
                if (outputs.empty()) throw InvalidInput{}; // outputs.get(0)
                PlayerAction wanted = parseAction(p->unit, outputs[0]);
                p->unit->wantedAction = wanted;
                p->setDisplayMessage(wanted.message);
            } catch (const InvalidInput&) {
                killPlayer(p, A("Invalid Input: '") + (outputs.empty() ? String() : outputs[0]) + A("'"));
                cg::gm.addToGameSummary(A("Invalid command by ") + p->getNicknameToken() + A("."));
            }
        }
    }

    void killPlayer(KPlayer* p, const String& message) {
        removeThePlayer(p->unit);
        p->setScore(-1);
        p->deactivate(message);
    }

    void sendInput(KPlayer* player) {
        std::vector<String> toSend;
        int idx = player->getIndex();
        for (int i = 0; i < MAX_PLAYERS; i++) {
            KPlayer* q = static_cast<KPlayer*>(cg::gm.getPlayer((i + idx) % MAX_PLAYERS));
            if (!q->isActive()) continue;
            toSend.push_back(q->unit->toOutput());
        }
        for (Minion* m : minions) toSend.push_back(m->toOutput());
        for (Effect* e : effects) {
            String out;
            if (e->toOutput(out)) toSend.push_back(out);
        }
        player->sendInputLine(std::to_string(toSend.size()));
        for (const String& s : toSend) player->sendInputLine(s);
        player->execute();
    }

    void updateGame() {
        for (Cell& c : playfield.cells) c.minionRepulse = false; // resetEffects
        spawnMinions();
        doPlayerAction();
        doEffects();
        for (Minion* m : minions) m->update();
        minions.erase(std::remove_if(minions.begin(), minions.end(), [](Minion* m) { return m->isBeingRecalled(); }), minions.end());
        spreadMadness();
        removeDeadPlayers();
        if (HAS_SHELTER) spawnShelters();
    }

    void spawnShelters() {
        if (turn_ != 0 && turn_ % SHELTER_SPAWN_INTERVAL == 0) {
            effects.erase(std::remove_if(effects.begin(), effects.end(), [](Effect* e) { return e->type == EFFECT_SHELTER_TYPE; }), effects.end());
            for (Cell* cell : playfield.shelterCells) {
                allEffects.emplace_back();
                Effect* e = &allEffects.back();
                e->type = EFFECT_SHELTER_TYPE;
                e->cell = cell;
                e->tick = EFFECT_MAX_SHELTER_ENERGY;
                effects.push_back(e);
            }
        }
    }

    static void doEffects() {
        for (Effect* e : effects) e->doEffect();
        effects.erase(std::remove_if(effects.begin(), effects.end(), [](Effect* e) { return e->isFinished(); }), effects.end());
    }

    void updateIsolation() {
        std::vector<PlayerUnit*> units = activeUnits();
        for (PlayerUnit* u : units) u->updateIsolation(units);
    }

    void spreadMadness() {
        updateIsolation();
        if (isKutuluComing()) {
            std::vector<KPlayer*> a = active();
            if (a.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
            a[0]->unit->madden(SPREAD_KUTULU_MADNESS_PER_TURN_AMOUNT);
        } else {
            for (KPlayer* p : active()) p->unit->spreadMadness();
        }
        removeDeadPlayers();
    }

    void doPlayerAction() {
        resetStuckedPlayerActions();
        handleYELLsFirst();
        resetStuckedPlayerActions();
        handleOtherCommands();
    }

    void handleOtherCommands() {
        for (KPlayer* p : active()) {
            PlayerUnit* u = p->unit;
            PlayerAction& wanted = u->wantedAction;
            Cell* cell = playfield.get(u->pos + ACTION_MOVES[(int)wanted.action]);
            if (u->isStuck()) {
                if (!wanted.staticWait) {
                    u->wantedAction = PlayerAction();
                    cg::gm.addToGameSummary(p->getNicknameToken() + A(" is taken aback and won't act this turn"));
                }
                continue;
            } else if (!playfield.isWalkable(cell) || u->performingInvalidMove) {
                cg::gm.addToGameSummary(A("Invalid move from ") + p->getNicknameToken());
                continue;
            } else {
                u->move(cell);
                if (wanted.action == Action::PLAN || wanted.action == Action::LIGHT) {
                    if (playerHasCurrentEffect(u)) {
                        cg::gm.addToGameSummary(A("Invalid effect from ") + p->getNicknameToken());
                        continue;
                    }
                    Effect* e = nullptr;
                    if (HAS_PLAN && wanted.action == Action::PLAN) {
                        if (u->remainingPlans > 0) {
                            u->remainingPlans--;
                            allEffects.emplace_back();
                            e = &allEffects.back();
                            e->type = EFFECT_PLAN_TYPE;
                            e->caster = u;
                            e->tick = EFFECT_PLAN_MAX_DURATION;
                        }
                    } else if (HAS_LIGHT && wanted.action == Action::LIGHT) {
                        if (u->remainingLight > 0) {
                            u->remainingLight--;
                            allEffects.emplace_back();
                            e = &allEffects.back();
                            e->type = EFFECT_REPULSE_TYPE;
                            e->caster = u;
                            e->tick = EFFECT_REPULSE_MAX_DURATION;
                        }
                    }
                    if (e != nullptr) effects.push_back(e);
                    else cg::gm.addToGameSummary(A("Invalid effect from ") + p->getNicknameToken());
                }
            }
        }
    }

    void handleYELLsFirst() {
        for (KPlayer* p : active()) {
            if (HAS_YELL && p->unit->wantedAction.action == Action::YELL) {
                for (KPlayer* o : active()) {
                    if (o->unit == p->unit) continue;
                    if (yell(p->unit, o->unit)) cg::gm.addToGameSummary(o->getNicknameToken() + A(" fooled by ") + p->getNicknameToken());
                }
            }
        }
    }

    // PlayerUnit#yell / #stuck
    static bool yell(PlayerUnit* self, PlayerUnit* opponent) {
        if (opponent == self) return false;
        if (opponent->pos.manhattan(self->pos) <= EFFECT_YELL_RADIUS) {
            if (opponent->hasBeenStuckedBy(self->id)) return false;
            opponent->yellsBy.push_back(self->id);
            allEffects.emplace_back();
            Effect* e = &allEffects.back();
            e->type = EFFECT_YELL_TYPE;
            e->caster = self;
            e->receiver = opponent;
            e->tick = PLAYER_INITIAL_STUCK_ROUNDS;
            effects.push_back(e);
            return true;
        }
        return false;
    }

    void resetStuckedPlayerActions() {
        for (KPlayer* p : active()) {
            if (p->unit->isStuck() && !p->unit->wantedAction.staticWait) {
                p->unit->wantedAction = PlayerAction();
                cg::gm.addToGameSummary(p->getNicknameToken() + A(" is taken aback and won't act this turn"));
            }
        }
    }

    static bool playerHasCurrentEffect(PlayerUnit* u) {
        for (Effect* e : effects)
            if (e->caster == u) return true;
        return false;
    }

    void removeDeadPlayers() {
        for (cg::Player* cp : cg::gm.getPlayers()) {
            KPlayer* p = static_cast<KPlayer*>(cp);
            if (p->isActive() && p->unit->isDead()) {
                removeThePlayer(p->unit);
                p->setScore(turn_);
                p->deactivate(A("$") + I(p->getIndex()) + A("  frightened to death"));
            }
        }
    }

    static void removeThePlayer(PlayerUnit* u) {
        if (contains(livingPlayers, u)) {
            effects.erase(std::remove_if(effects.begin(), effects.end(), [u](Effect* e) { return e->caster == u && e->type != EFFECT_YELL_TYPE; }), effects.end());
            livingPlayers.erase(std::find(livingPlayers.begin(), livingPlayers.end(), u));
            u->kill();
        }
    }

    void spawnMinions() {
        if (HAS_SLASHER) spawnPotentialSlashers();
        if (HAS_WANDERER) spawnWanderers();
    }

    void spawnWanderers() {
        if (turn_ % WANDERER_SPAWN_INTERVAL != 0) return;
        std::vector<V> spawns;
        for (int y = 0; y < playfield.H; y++)
            for (int x = 0; x < playfield.W; x++)
                if (maze->rows[y][x] == 'w') spawns.push_back(V{x, y});
        if (spawns.empty()) return;
        std::vector<int> mins(spawns.size(), playfield.W + playfield.H);
        for (size_t i = 0; i < spawns.size(); i++)
            for (PlayerUnit* p : livingPlayers) mins[i] = std::min(mins[i], p->pos.manhattan(spawns[i]));
        int maxOfMins = *std::max_element(mins.begin(), mins.end());
        for (size_t i = 0; i < spawns.size(); i++) {
            if (mins[i] != maxOfMins) continue;
            Minion* m = newMinion(NEXT_ID++, false);
            m->pos = spawns[i];
            minions.push_back(m);
            view->createMinionView(m);
        }
    }

    void spawnPotentialSlashers() {
        for (PlayerUnit* p : std::vector<PlayerUnit*>(livingPlayers)) {
            if (!slasherSpawned[p->id] && p->sanity < SLASHER_SANITY_SPAWN) {
                Minion* m = newMinion(NEXT_ID++, true);
                m->pos = p->pos;
                m->target = p;
                m->targetPositionBeforePreparation = p->getCurrentCell();
                slasherSpawned[p->id] = true;
                minions.push_back(m);
                view->createMinionView(m);
            }
        }
    }
};

KutuluReferee referee;

} // namespace

int main(int argc, char** argv) {
    cg::Options options;
    options.engine25 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
