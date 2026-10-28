// Crystal Rush (CodinGame "Unleash The Geek" 2019, a multiplayer game since 2020-03) - headless C++
// referee, port of https://github.com/CodinGameCommunity/UnleashTheGeek (commit b8b955e, 2019-10-03,
// pom artifactId unleash-the-geek-2019-gold), CodinGame game engine 3.8.2, 2 players, no leagues.
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
// Engine 3.8.2 (common/engine.h, cg::Options::engine313): its core and its runner's GameRunner / Agent
// are those of engine 3.13.0 (only the Renderer differs), so the runner sends the referee every line of
// a bot's output split with String#split: trailing empty lines are removed, so a bot ending its 5 lines
// with empty ones gives the referee fewer lines (the robots without a line then do nothing). The jar is
// built with referees/cg-builder's CommandLineInterface-v3.java (3.8.2's runner returns its older
// GameResult). The game parameters of the jar's command line are only the seed: the game's Config
// keeps its defaults (4-adjacency, 5 robots, 30x15 map, radar range 4, trap range 1 with chain reactions,
// cooldowns 5, robots respawning after 999 turns, i.e. never), which is all this port implements.
//
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption, int
// semantics, error texts. Notable Java behaviours reproduced here:
//   - new java.util.Random(seed) shared by the map generation (14 to 28 ore clusters placed with
//     Math.pow(nextFloat(), 0.55), each cell of a 5x5 cluster drawn; std::pow is not bit-identical to
//     Math.pow but the cluster column (int)(3 + pow * 25) is: every float nextFloat() can return gives a
//     pow at least 1.4 million ulps away from a column boundary), Collections.shuffle of the robots'
//     rows, and the game: the random pick among the closest neighbours of a far DIG target, the
//     (single) closest HQ cell of a REQUEST away from the HQ (one draw even then), and the random pick
//     among the closest cells reached when a MOVE target is more than 4 steps away (greedy best-first
//     search with a java.util.PriorityQueue: ties come out in the JDK's heap order).
//   - The commands: the four java.util.regex patterns (CASE_INSENSITIVE, ASCII \s and \d, the greedy
//     optional message group, e.g. "WAIT  " keeps one space as its message), the order they are tried
//     in, Integer.valueOf overflows (NumberFormatException texts), the first invalid line disqualifying
//     the player after the robots before it got their actions, lines of dead robots skipped unparsed.
//   - The turn: DIG intents converted to MOVE (invalid or HQ target, or too far), the "stays in place"
//     check made on the original intent, traps triggered by any DIG on them (also by robots of a player
//     disqualified this turn) with chain reactions, the digs by position in HashMap<Coord, ...> order and
//     by team index (ore duplicated when several robots dig the last ore, buried ore and items put back
//     after), item ids taken even when the item is not added (same cell), requests sorted with a stable
//     sort, cooldowns; digs and moves of the active players only, while intent conversions, traps,
//     requests and deliveries are those of every player's robots; the end conditions (no ore left,
//     counting ore carried by dead robots; the leader alone with live robots; every robot dead) and the
//     scores (-1 for a disqualified player).
//   - The HashSet<ItemCoord> orders of the radars and traps sent to the players (java::HashMapOrder).
//   - A turn where both players are disqualified while a robot still digs next to it crashes the Java
//     referee (getLargetSize: Optional.get() on no team): the game fails, null scores.
//   - Engine (common/engine.h): the protocol between the runner and the referee (java.util.Scanner
//     lines: U+0085, U+2028 or U+2029 in a bot line makes the game fail, 30000-byte commands: e.g. a
//     tooltip quoting a long invalid line full of U+FFFD), the game summary quotas; the bots get
//     1000 ms on their first turn and 50 ms after.
//   - The view data (the jar's own ViewModule text and EndScreenModule): it cannot reach the engine's
//     limits (a frame is at most ~3.5 KB: 10 robots, messages of at most 48 chars; at most ~750 KB over
//     the 201 frames, against 1 MB), so it only matters for the -l trace; it is rebuilt exactly, except the message filter
//     [^\p{L}\p{M}\p{N}\p{P}\p{Z}\p{Cf}\p{Cs}\s] which is exact for U+0000-U+00FF only: other chars are
//     kept here, where the jar removes the symbols, private use and unassigned ones (Unicode general
//     categories, not modelled), so frames with such messages differ in size from the jar's.
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps, so a
// bot answering in ~50 ms may be timed out by one and not the other; on a loaded machine the jar times
// out bots that answered in time); the message filter's Unicode categories above U+00FF (view only).
// Validated (2026-10-25) with battle referee-diff against the jar on 5250 games (33 bots, every ordered
// pairing, seeds from 1, 7, 1000001, 987654321 and 5000000000: all identical, no mismatch), in direct
// mode (jar game log vs -l trace: every execution, every view frame's size and text, summaries,
// tooltips, scores) on 352 games, including raw line endings, invalid UTF-8, U+2028 / U+0085, summaries
// at the runner's 30000-byte limit and outputs at its 10000-byte one; the command parsing against the
// four Java patterns on 200000 random lines. The "no ore left" end is not reached by these bots (maps
// hold 104 to 308 ore); the other ends are.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/engine.h"

#include <cmath>
#include <deque>
#include <map>
#include <set>

using java::String;

static String A(const char* s) { return java::ascii(s); }
static String I(i64 v) { return java::str(v); }

// ----------------------------------------------------------------------------
// Config (the defaults: the command line passes no game parameter)
// ----------------------------------------------------------------------------
static const int AGENTS_MOVE_DISTANCE = 4;
static const int AGENTS_PER_PLAYER = 5;
static const int AGENT_INTERACT_RADIUS = 1;
static const int AGENT_RESPAWN_TIME = 999;
static const int MAP_CLUSTER_SIZE = 5;
static const double MAP_ORE_COEFF_X = 0.55;
static const int MAP_HEIGHT = 15;
static const int MAP_WIDTH = 30;
static const double MAP_CLUSTER_DISTRIBUTION_MAX = 0.064;
static const double MAP_CLUSTER_DISTRIBUTION_MIN = 0.032;
static const int MAP_ORE_IN_CELL_MAX = 3;
static const int MAP_ORE_IN_CELL_MIN = 1;
static const int RADAR_COOLDOWN = 5;
static const int RADAR_RANGE = 4;
static const int TRAP_COOLDOWN = 5;
static const int TRAP_RANGE = 1;
static const int TYPE_MY_AGENT = 0;
static const int TYPE_FOE_AGENT = 1;

// Item, in ordinal order (EnumMap order)
enum Item { TRAP, RADAR, ORE, NOTHING };
static int typeId(Item i) {
    static const int T[4] = {3, 2, 4, -1};
    return T[i];
}
static const char* itemName(Item i) {
    static const char* N[4] = {"TRAP", "RADAR", "ORE", "NOTHING"};
    return N[i];
}

struct Coord {
    i32 x = 0, y = 0;
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    i32 hashCode() const { return java::add(java::mul(31, java::add(31, x)), y); }
    i32 manhattanTo(i32 ox, i32 oy) const { return java::add(java::abs(java::sub(ox, x)), java::abs(java::sub(oy, y))); }
    // FOUR_ADJACENCY: the Manhattan distance (int overflow included)
    i32 gameDistanceTo(const Coord& c) const { return manhattanTo(c.x, c.y); }
    String toString() const { return u"(" + I(x) + u", " + I(y) + u")"; }
};
static const Coord DELTAS[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

static bool inGrid(const Coord& c) { return c.x >= 0 && c.x < MAP_WIDTH && c.y >= 0 && c.y < MAP_HEIGHT; }
static i32 keyOf(const Coord& c) { return c.y * MAP_WIDTH + c.x; } // in-grid coordinates only

// A HashSet<Coord> (or HashSet<ItemCoord>) of in-grid coordinates, with its iteration order
struct CoordSet {
    java::HashMapOrder order;
    std::map<i32, int> ids; // key -> the ItemCoord id
    bool contains(const Coord& c) const { return inGrid(c) && ids.count(keyOf(c)); }
    bool add(const Coord& c, int id = 0) {
        if (contains(c)) return false;
        order.put(keyOf(c), c.hashCode());
        ids[keyOf(c)] = id;
        return true;
    }
    bool remove(const Coord& c) {
        if (!contains(c)) return false;
        order.remove(keyOf(c), c.hashCode());
        ids.erase(keyOf(c));
        return true;
    }
    // the elements in iteration order, with their ids
    std::vector<std::pair<Coord, int>> elements() {
        std::vector<i32> keys;
        order.keys(keys);
        std::vector<std::pair<Coord, int>> r;
        for (i32 k : keys) r.push_back({Coord{k % MAP_WIDTH, k / MAP_WIDTH}, ids[k]});
        return r;
    }
};

// ----------------------------------------------------------------------------
// Actions, agents, players
// ----------------------------------------------------------------------------
enum ActionKind { NO_ACTION, MOVE, DIG, REQUEST };
struct Action {
    ActionKind kind = NO_ACTION;
    Coord target; // MOVE, DIG (getTarget() is null otherwise)
    Item item = NOTHING;
    bool isMove() const { return kind == MOVE; }
    bool isDig() const { return kind == DIG; }
    bool isRequest() const { return kind == REQUEST; }
    bool hasTarget() const { return kind == MOVE || kind == DIG; }
    static Action move(const Coord& c) {
        Action a;
        a.kind = MOVE;
        a.target = c;
        return a;
    }
    static Action dig(const Coord& c) {
        Action a;
        a.kind = DIG;
        a.target = c;
        return a;
    }
    static Action request(Item i) {
        Action a;
        a.kind = REQUEST;
        a.item = i;
        return a;
    }
};

struct Agent {
    int owner = 0;
    int id = 0;
    bool dead = false;
    int respawnIn = 0;
    bool hasMessage = false; // message != null
    String message;
    Coord position, initialPosition;
    Item inventory = NOTHING;
    Action intent;

    void setMessage(const String& m) {
        message = m.size() > 48 ? m.substr(0, 46) + u"..." : m;
        hasMessage = true;
    }
    void reset() {
        intent = Action();
        hasMessage = false;
        message.clear();
    }
    void factoryReset() {
        reset();
        position = initialPosition;
        inventory = NOTHING;
        dead = false;
    }
    void die() {
        dead = true;
        respawnIn = AGENT_RESPAWN_TIME;
        intent = Action();
    }
    bool shouldRespawn() const { return dead && respawnIn <= 0; }
};

static Agent agents_[2 * AGENTS_PER_PLAYER]; // by id

struct Player : cg::Player {
    std::vector<int> agents; // ids
    int ore = 0;
    int cooldowns[2] = {0, 0};     // EnumMap<Item, Integer>: TRAP, RADAR
    int cooldownTimes[2] = {TRAP_COOLDOWN, RADAR_COOLDOWN};

    int getExpectedOutputLines() const override { return (int)agents.size(); }
    void decrementCooldowns() {
        for (int& c : cooldowns)
            if (c > 0) c--;
    }
    int getCooldown(Item item) const { return cooldowns[item]; }
    void startCooldown(Item item) { cooldowns[item] = cooldownTimes[item]; }
    void scoreOre() {
        ore++;
        setScore(ore);
    }
    void reset() {
        for (int a : agents) agents_[a].reset();
    }
};
static Player players[2];

static std::vector<Player*> activePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.isActive()) r.push_back(&p);
    return r;
}

// allAgents: the agents of every player (active or not), player by player
static std::vector<Agent*> allAgents() {
    std::vector<Agent*> r;
    for (Player& p : players)
        for (int a : p.agents) r.push_back(&agents_[a]);
    return r;
}

// ----------------------------------------------------------------------------
// Grid
// ----------------------------------------------------------------------------
struct Cell {
    bool accessToHQ = false, hole = false;
    int ore = 0;
};

static java::Random random_;
static int randomInt(int high) { return random_.nextInt(high); }
static int randomInt(int low, int high) { return low + random_.nextInt(high - low); }

static int itemInstanceCount = 0; // ItemCoord.INSTANCE_COUNT

struct Grid {
    Cell cells[MAP_WIDTH][MAP_HEIGHT];
    CoordSet radars[2], traps[2];

    // get(x, y): nullptr for Cell.NO_CELL
    Cell* get(const Coord& c) { return inGrid(c) ? &cells[c.x][c.y] : nullptr; }
    int oreAt(const Coord& c) {
        Cell* cell = get(c);
        return cell ? cell->ore : 0;
    }

    std::vector<Coord> getNeighbours(const Coord& pos) {
        std::vector<Coord> neighs;
        for (const Coord& d : DELTAS) {
            Coord n{java::add(pos.x, d.x), java::add(pos.y, d.y)};
            if (get(n)) neighs.push_back(n);
        }
        return neighs;
    }

    // the greedy best-first search: the path to the target, or to a random one of the closest cells
    // reached within AGENTS_MOVE_DISTANCE steps (no obstacles: robots can share cells)
    std::vector<Coord> findPath(const Coord& start, const Coord& target) {
        struct PathNode {
            Coord coord;
            int previous;
            int steps;
        };
        std::vector<PathNode> nodes;
        auto unrollPath = [&](int current) {
            std::vector<Coord> path;
            while (nodes[current].previous >= 0) {
                path.insert(path.begin(), nodes[current].coord);
                current = nodes[current].previous;
            }
            return path;
        };
        auto byDistanceTo = [&](int a, int b) { return java::compareInt(nodes[a].coord.gameDistanceTo(target), nodes[b].coord.gameDistanceTo(target)); };
        java::PriorityQueue<int, decltype(byDistanceTo)> queue(byDistanceTo);
        bool computed[MAP_WIDTH][MAP_HEIGHT] = {};
        std::vector<int> closest;
        int closestBy = 0;

        nodes.push_back({start, -1, 0});
        queue.offer(0);
        computed[start.x][start.y] = true;
        while (!queue.isEmpty()) {
            int current = queue.poll();
            if (nodes[current].coord == target) return unrollPath(current);
            int distance = nodes[current].coord.gameDistanceTo(target);
            if (closest.empty() || closestBy > distance) {
                closest.clear();
                closest.push_back(current);
                closestBy = distance;
            } else if (!closest.empty() && closestBy == distance) {
                closest.push_back(current);
            }
            if (nodes[current].steps < AGENTS_MOVE_DISTANCE) {
                for (const Coord& neigh : getNeighbours(nodes[current].coord)) {
                    if (!computed[neigh.x][neigh.y]) {
                        nodes.push_back({neigh, current, nodes[current].steps + 1});
                        queue.offer((int)nodes.size() - 1);
                        computed[neigh.x][neigh.y] = true;
                    }
                }
            }
        }
        if (closest.empty()) return {};
        return unrollPath(closest[random_.nextInt((int)closest.size())]);
    }

    static std::vector<Coord> getClosestTarget(const Coord& from, const std::vector<Coord>& targets) {
        std::vector<Coord> closest;
        int closestBy = 0;
        for (const Coord& neigh : targets) {
            int distance = from.gameDistanceTo(neigh);
            if (closest.empty() || closestBy > distance) {
                closest.clear();
                closest.push_back(neigh);
                closestBy = distance;
            } else if (!closest.empty() && closestBy == distance) {
                closest.push_back(neigh);
            }
        }
        return closest;
    }

    bool hasTrap(const Coord& pos) { return traps[0].contains(pos) || traps[1].contains(pos); }
    void removeTrap(const Coord& pos) {
        for (CoordSet& s : traps) s.remove(pos);
    }
    bool destroyRadar(const Coord& pos, int destroyer) {
        bool destroyed = false;
        for (int i = 0; i < 2; ++i)
            if (i != destroyer) destroyed |= radars[i].remove(pos);
        return destroyed;
    }
    CoordSet& getItems(Item item, int player) { return item == RADAR ? radars[player] : traps[player]; }
    void insertItem(const Coord& pos, Item item, int owner) {
        int id = itemInstanceCount++ + AGENTS_PER_PLAYER * 2; // new ItemCoord(x, y)
        getItems(item, owner).add(pos, id);
    }
    std::vector<Coord> getHQAccesses() {
        std::vector<Coord> coords;
        for (int y = 0; y < MAP_HEIGHT; ++y) coords.push_back(Coord{0, y});
        return coords;
    }
    // isOreVisibleTo(x, y, player) for every cell: [x][y]
    void oreVisibility(int player, bool visible[MAP_WIDTH][MAP_HEIGHT]) {
        std::vector<std::pair<Coord, int>> rs = radars[player].elements();
        for (int x = 0; x < MAP_WIDTH; ++x)
            for (int y = 0; y < MAP_HEIGHT; ++y) {
                visible[x][y] = false;
                for (auto& e : rs)
                    if (e.first.manhattanTo(x, y) <= RADAR_RANGE) {
                        visible[x][y] = true;
                        break;
                    }
            }
    }
    // the cells within range steps (a HashSet<Coord>, in its iteration order)
    std::vector<Coord> getCellsInRange(const Coord& coord, int range) {
        CoordSet result;
        struct Node {
            Coord c;
            int steps;
        };
        std::deque<Node> queue;
        queue.push_back({coord, 0});
        while (!queue.empty()) {
            Node e = queue.front();
            queue.pop_front();
            result.add(e.c);
            if (e.steps < range)
                for (const Coord& neigh : getNeighbours(e.c))
                    if (!result.contains(neigh)) queue.push_back({neigh, e.steps + 1});
        }
        std::vector<Coord> r;
        for (auto& e : result.elements()) r.push_back(e.first);
        return r;
    }
};
static Grid grid;

// the coordinates of the grid (HashMap<Coord, Cell>) in iteration order
static std::vector<Coord> cellOrder() {
    std::vector<Coord> coords;
    std::vector<i32> hashes;
    for (int y = 0; y < MAP_HEIGHT; ++y)
        for (int x = 0; x < MAP_WIDTH; ++x) {
            coords.push_back(Coord{x, y});
            hashes.push_back(coords.back().hashCode());
        }
    std::vector<Coord> r;
    for (int k : java::hashMapOrder(hashes)) r.push_back(coords[k]);
    return r;
}

// ----------------------------------------------------------------------------
// Game
// ----------------------------------------------------------------------------
struct EventData {
    enum { REQUEST = 0, RADAR_DESTRUCTION = 1, BURY = 2, GET_ORE = 3, GIVE_ORE = 4, EXPLOSION = 5, AGENT_DEATH = 6, RESPAWN = 7 };
    int type = 0;
    bool hasItem = false, hasAgent = false, hasX = false, hasY = false;
    int item = 0, agent = 0, x = 0, y = 0;
    EventData(int t) : type(t) {}
    void setItem(int v) {
        item = v;
        hasItem = true;
    }
    void setAgent(int v) {
        agent = v;
        hasAgent = true;
    }
    void setPos(const Coord& c) {
        x = c.x;
        y = c.y;
        hasX = hasY = true;
    }
};

static std::vector<int> deadAgents;
static std::vector<EventData> viewerEvents;
static int trapsPlaced = 0, robotsDestroyed = 0, oreDelivered = 0;

static void destroyAgent(Agent* agent) {
    agent->die();
    deadAgents.push_back(agent->id);
    EventData event(EventData::AGENT_DEATH);
    event.setAgent(agent->id);
    viewerEvents.push_back(event);
    robotsDestroyed++;
    cg::gm.addTooltip(agent->owner, u"Robot " + I(agent->id) + u" destroyed!");
}

static void generateMap() {
    // newMap: the first column has access to the HQ
    for (int y = 0; y < MAP_HEIGHT; ++y) grid.cells[0][y].accessToHQ = true;

    int cellCount = MAP_WIDTH * MAP_HEIGHT;
    int clustersMin = std::max(1, java::d2i(cellCount * MAP_CLUSTER_DISTRIBUTION_MIN));
    int clustersMax = std::max(clustersMin, java::d2i(cellCount * MAP_CLUSTER_DISTRIBUTION_MAX));
    int oreClusterCount = randomInt(clustersMin, clustersMax + 1);

    const int padLeft = 3, padRight = 2, padTop = 2, padBottom = 2;
    int tries = 0;
    while (oreClusterCount > 0 && tries < 1000) {
        double factor = std::pow((double)random_.nextFloat(), MAP_ORE_COEFF_X);
        int x = java::d2i(padLeft + factor * (MAP_WIDTH - padRight - padLeft)); // interpolate
        int y = randomInt(padTop, MAP_HEIGHT - padBottom);
        if (grid.oreAt(Coord{x, y}) == 0) {
            Coord clusterCenter{x, y};
            for (int i = 0; i < MAP_CLUSTER_SIZE; ++i) {
                for (int j = 0; j < MAP_CLUSTER_SIZE; ++j) {
                    x = clusterCenter.x + (i - MAP_CLUSTER_SIZE / 2);
                    y = clusterCenter.y + (j - MAP_CLUSTER_SIZE / 2);
                    int chances = clusterCenter.manhattanTo(x, y) * 2 + 1;
                    int hit = randomInt(chances);
                    if (hit == 0) {
                        int amount = randomInt(MAP_ORE_IN_CELL_MIN, MAP_ORE_IN_CELL_MAX + 1);
                        Cell* cell = grid.get(Coord{x, y});
                        if (cell) cell->ore = amount; // (always in the grid)
                    }
                }
            }
            oreClusterCount--;
        }
        tries++;
    }
}

static void initPlayers() {
    int spaces = MAP_HEIGHT / 2;
    std::vector<int> available; // a LinkedList: Collections.shuffle swaps like on an array
    for (int i = 0; i < spaces; ++i) available.push_back(i);
    java::shuffle(available, random_);
    size_t next = 0;
    for (int i = 0; i < AGENTS_PER_PLAYER; ++i) {
        int y = next < available.size() ? available[next++] : randomInt(spaces);
        for (Player& player : players) {
            Coord pos{0, y * 2}; // AGENTS_START_PACKED
            Agent& agent = agents_[AGENTS_PER_PLAYER * player.getIndex() + i];
            agent = Agent();
            agent.id = AGENTS_PER_PLAYER * player.getIndex() + i;
            agent.owner = player.getIndex();
            agent.position = agent.initialPosition = pos;
            player.agents.push_back(agent.id);
        }
    }
}

static void convertIntents() {
    for (Agent* agent : allAgents()) {
        Action intent = agent->intent;
        if (intent.isDig()) {
            Cell* cell = grid.get(intent.target);
            if (!cell || cell->accessToHQ) {
                agent->intent = Action::move(intent.target);
            } else if (agent->position.gameDistanceTo(intent.target) > AGENT_INTERACT_RADIUS) {
                std::vector<Coord> closest = Grid::getClosestTarget(agent->position, grid.getNeighbours(intent.target));
                Coord target = closest[randomInt((int)closest.size())];
                agent->intent = Action::move(target);
            }
        } else if (intent.isRequest()) {
            Cell* cell = grid.get(agent->position);
            if (!cell || !cell->accessToHQ) {
                std::vector<Coord> closest = Grid::getClosestTarget(agent->position, grid.getHQAccesses());
                Coord target = closest[randomInt((int)closest.size())];
                agent->intent = Action::move(target);
                cg::gm.addToGameSummary(I(agent->id) + u" wanted to request but instead moves to " + target.toString());
            }
        }
        // the original intent
        if (intent.isMove()) {
            if (agent->position.x == intent.target.x && agent->position.y == intent.target.y) {
                agent->intent = Action();
                cg::gm.addToGameSummary(I(agent->id) + u" stays in place");
            }
        }
    }
}

static void resolveTraps() {
    CoordSet triggeredTraps; // Collectors.toSet()
    for (Agent* agent : allAgents()) {
        const Action& intent = agent->intent;
        if (intent.isDig() && grid.hasTrap(intent.target)) triggeredTraps.add(intent.target);
    }
    // TRAP_CHAIN_REACTION
    std::set<std::pair<int, int>> deathZone;
    std::deque<Coord> exploding;
    for (auto& e : triggeredTraps.elements()) {
        deathZone.insert({e.first.x, e.first.y});
        exploding.push_back(e.first);
    }
    while (!exploding.empty()) {
        Coord trap = exploding.front();
        exploding.pop_front();
        for (const Coord& c : grid.getCellsInRange(trap, TRAP_RANGE)) {
            deathZone.insert({c.x, c.y});
            if (grid.hasTrap(c) && !triggeredTraps.contains(c)) {
                exploding.push_back(c);
                triggeredTraps.add(c);
            }
        }
    }
    for (const auto& c : deathZone) grid.removeTrap(Coord{c.first, c.second});

    std::vector<Agent*> killed;
    for (Agent* agent : allAgents())
        if (!agent->dead && deathZone.count({agent->position.x, agent->position.y})) killed.push_back(agent);
    for (Agent* agent : killed) destroyAgent(agent);

    for (auto& e : triggeredTraps.elements()) {
        EventData event(EventData::EXPLOSION);
        event.setPos(e.first);
        viewerEvents.push_back(event);
    }
}

static void resolveDigs() {
    std::vector<Player*> active = activePlayers(); // getTeamList()
    // Collectors.toMap(...) of the distinct dig targets: a HashMap<Coord, List<List<Agent>>>
    std::vector<Coord> keys;
    std::vector<std::vector<std::vector<Agent*>>> values;
    for (Agent* agent : allAgents()) {
        if (!agent->intent.isDig()) continue;
        const Coord& c = agent->intent.target;
        bool seen = false;
        for (const Coord& k : keys) seen = seen || k == c;
        if (seen) continue;
        keys.push_back(c);
        std::vector<std::vector<Agent*>> teams;
        for (Player* p : active) {
            std::vector<Agent*> team;
            for (int a : p->agents)
                if (agents_[a].intent.isDig() && agents_[a].intent.target == c) team.push_back(&agents_[a]);
            teams.push_back(team);
        }
        values.push_back(teams);
    }
    std::vector<i32> hashes;
    for (const Coord& k : keys) hashes.push_back(k.hashCode());

    for (int k : java::hashMapOrder(hashes)) {
        const Coord pos = keys[k];
        std::vector<std::vector<Agent*>>& teams = values[k];
        Cell* cell = grid.get(pos);
        // getLargetSize: max() of no team at all (no active player) is an empty Optional
        if (teams.empty()) throw java::Exception{"java.util.NoSuchElementException: No value present"};
        size_t maxAgentsInTeam = 0;
        for (auto& team : teams) maxAgentsInTeam = std::max(maxAgentsInTeam, team.size());

        std::map<int, std::vector<Item>> buried; // HashMap<Integer, List<Item>>: player 0 then 1

        // Radar destruction
        for (auto& team : teams) {
            if (team.empty()) continue;
            Agent* agent = team[0];
            if (grid.destroyRadar(pos, agent->owner)) {
                EventData event(EventData::RADAR_DESTRUCTION);
                event.setPos(pos);
                event.setAgent(agent->id);
                viewerEvents.push_back(event);
            }
        }

        // Ore collection
        for (size_t idx = 0; idx < maxAgentsInTeam; ++idx) {
            std::vector<Agent*> oreCollectors;
            for (auto& team : teams) {
                if (team.size() <= idx) continue;
                Agent* agent = team[idx];
                EventData event(EventData::BURY);
                event.setPos(pos);
                event.setAgent(agent->id);
                event.setItem(typeId(agent->inventory));
                viewerEvents.push_back(event);
                if (agent->inventory != ORE) oreCollectors.push_back(agent);
                if (agent->inventory != NOTHING) {
                    buried[agent->owner].push_back(agent->inventory);
                    agent->inventory = NOTHING;
                }
                if (event.item == typeId(TRAP)) trapsPlaced++;
            }
            // Drill hole
            if (!cell->hole) cell->hole = true;
            if (cell->ore > 0) {
                cell->ore = std::max(0, cell->ore - (int)oreCollectors.size());
                for (Agent* agent : oreCollectors) {
                    agent->inventory = ORE;
                    EventData event(EventData::GET_ORE);
                    event.setPos(pos);
                    event.setAgent(agent->id);
                    viewerEvents.push_back(event);
                }
            }
        }

        // Item insertion
        for (auto& kv : buried)
            for (Item item : kv.second) {
                if (item == ORE) cell->ore++;
                else grid.insertItem(pos, item, kv.first);
            }
    }
}

static void resolveRequests() {
    std::vector<Agent*> requesters;
    for (Agent* agent : allAgents())
        if (agent->intent.isRequest()) requesters.push_back(agent);
    java::sort(requesters, [](Agent* a, Agent* b) { // compareByInventorySpace
        bool hasItemA = a->inventory != NOTHING;
        bool hasItemB = b->inventory != NOTHING;
        if (hasItemA && !hasItemB) return 1;
        if (!hasItemA && hasItemB) return -1;
        return 0;
    });
    for (Agent* agent : requesters) {
        Item item = agent->intent.item;
        Player& owner = players[agent->owner];
        cg::gm.addToGameSummary(I(agent->id) + u" requested a " + A(itemName(item)));
        if (owner.getCooldown(item) == 0) {
            owner.startCooldown(item);
            agent->inventory = item;
            cg::gm.addToGameSummary(A("and was given one\n"));
            EventData event(EventData::REQUEST);
            event.setItem(typeId(item));
            event.setAgent(agent->id);
            viewerEvents.push_back(event);
        } else {
            cg::gm.addToGameSummary(u"but cooldown is " + I(owner.getCooldown(item)) + u"\n");
        }
    }
}

static void decrementCooldowns() {
    for (int a : deadAgents) agents_[a].respawnIn--;
    for (Player& p : players) p.decrementCooldowns();
}

static void resolveMoves() {
    // getMoversByTeam: the active players' robots with a MOVE intent
    for (Player* p : activePlayers()) {
        std::vector<Agent*> team;
        for (int a : p->agents)
            if (agents_[a].intent.isMove()) team.push_back(&agents_[a]);
        for (Agent* agent : team) {
            std::vector<Coord> path = grid.findPath(agent->position, agent->intent.target);
            if (!path.empty()) agent->position = path.back();
        }
    }
}

static void resolveDelivers() {
    for (Agent* agent : allAgents()) {
        Cell* cell = grid.get(agent->position);
        if (agent->dead || !cell || !cell->accessToHQ) continue;
        if (agent->inventory == ORE) {
            players[agent->owner].scoreOre();
            agent->inventory = NOTHING;
            EventData event(EventData::GIVE_ORE);
            event.setAgent(agent->id);
            viewerEvents.push_back(event);
            oreDelivered++;
        }
    }
}

static void respawnDeadAgents() {
    std::vector<int> respawning;
    for (int a : deadAgents)
        if (agents_[a].shouldRespawn()) respawning.push_back(a);
    for (int a : respawning) {
        deadAgents.erase(std::find(deadAgents.begin(), deadAgents.end(), a));
        agents_[a].factoryReset();
        EventData event(EventData::RESPAWN);
        event.setAgent(a);
        viewerEvents.push_back(event);
    }
}

static void performGameUpdate() {
    convertIntents();
    resolveTraps();
    resolveDigs();
    resolveRequests();
    decrementCooldowns();
    resolveMoves();
    resolveDelivers();
    respawnDeadAgents();
}

static int getRemainingOre() {
    int remainingOre = 0;
    for (int x = 0; x < MAP_WIDTH; ++x)
        for (int y = 0; y < MAP_HEIGHT; ++y) remainingOre += grid.cells[x][y].ore;
    for (Player& p : players)
        for (int a : p.agents)
            if (agents_[a].inventory == ORE) remainingOre += 1;
    return remainingOre;
}

static Player* getPlayerWithMostOre() {
    int most = 0;
    Player* result = nullptr;
    for (Player& player : players) {
        if (result == nullptr || player.ore > most) {
            most = player.ore;
            result = &player;
        } else if (player.ore == most) {
            return nullptr;
        }
    }
    return result;
}

static bool gameOver() {
    if (getRemainingOre() == 0) return true;
    // the player with most ore is the only player with live robots
    Player* playerWithMostOre = getPlayerWithMostOre();
    if (playerWithMostOre != nullptr) {
        for (Player& player : players)
            if (&player != playerWithMostOre)
                for (int a : player.agents)
                    if (!agents_[a].dead) return false;
        return true;
    }
    // no robot left
    for (Player& player : players)
        for (int a : player.agents)
            if (!agents_[a].dead) return false;
    return true;
}

// The input of a player for this turn
static std::vector<std::string> getCurrentFrameInfoFor(Player& player) {
    std::vector<std::string> lines;
    Player& opponent = &player != &players[0] ? players[0] : players[1];
    lines.push_back(std::to_string(player.ore) + " " + std::to_string(opponent.ore));
    bool visible[MAP_WIDTH][MAP_HEIGHT];
    grid.oreVisibility(player.getIndex(), visible);
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        std::string row;
        for (int x = 0; x < MAP_WIDTH; ++x) {
            const Cell& c = grid.cells[x][y];
            if (x) row += ' ';
            row += visible[x][y] ? std::to_string(c.ore) : std::string("?");
            row += c.hole ? " 1" : " 0";
        }
        lines.push_back(row);
    }
    std::vector<std::string> entities;
    for (Agent* agent : allAgents()) {
        bool mine = agent->owner == player.getIndex();
        entities.push_back(std::to_string(agent->id) + " " + std::to_string(mine ? TYPE_MY_AGENT : TYPE_FOE_AGENT) + " " +
                           std::to_string(agent->dead ? -1 : agent->position.x) + " " + std::to_string(agent->dead ? -1 : agent->position.y) + " " +
                           std::to_string(mine ? typeId(agent->inventory) : typeId(NOTHING)));
    }
    for (Item item : {RADAR, TRAP}) {
        std::string t = std::to_string(typeId(item));
        for (auto& e : grid.getItems(item, player.getIndex()).elements())
            entities.push_back(std::to_string(e.second) + " " + t + " " + std::to_string(e.first.x) + " " + std::to_string(e.first.y) + " " + t);
    }
    lines.push_back(std::to_string(entities.size()) + " " + std::to_string(player.getCooldown(RADAR)) + " " + std::to_string(player.getCooldown(TRAP)));
    lines.insert(lines.end(), entities.begin(), entities.end());
    return lines;
}

// ----------------------------------------------------------------------------
// CommandManager
// ----------------------------------------------------------------------------
static const char* EXPECTED = "DIG <x> <y> | REQUEST <item> | MOVE <x> <y> | WAIT";

// The parts of a line matched by one of the patterns
struct Matched {
    String x, y, item;
    bool hasMessage = false;
    String message;
};

// a literal of a CASE_INSENSITIVE pattern at pos: the position after it, or npos
static size_t matchLiteral(const String& s, size_t pos, const char* lit) {
    size_t n = strlen(lit);
    if (s.size() - pos < n) return String::npos;
    for (size_t i = 0; i < n; i++)
        if (!java::regexCharMatchesIgnoreCase(s[pos + i], lit[i])) return String::npos;
    return pos + n;
}
// \s+: the position after the (greedy) run, or npos
static size_t matchSpaces(const String& s, size_t pos) {
    size_t p = pos;
    while (p < s.size() && java::isRegexSpace(s[p])) p++;
    return p == pos ? String::npos : p;
}
// -?\d+ (greedy): the position after it, or npos
static size_t matchNumber(const String& s, size_t pos) {
    size_t p = pos;
    if (p < s.size() && s[p] == u'-') p++;
    size_t d = p;
    while (p < s.size() && java::isRegexDigit(s[p])) p++;
    return p == d ? String::npos : p;
}
// (?:\s+(?<message>.+))?\s*$ from pos (a line has no line terminator: '.' matches every char, $ is the
// end): the greedy optional group takes everything after the spaces, or, when only spaces are left, the
// last of at least two of them
static bool matchTail(const String& s, size_t pos, Matched& m) {
    size_t n = s.size();
    if (pos == n) return true;
    if (!java::isRegexSpace(s[pos])) return false;
    size_t w = matchSpaces(s, pos);
    if (w < n) {
        m.hasMessage = true;
        m.message = s.substr(w);
    } else if (n - pos >= 2) {
        m.hasMessage = true;
        m.message = s.substr(n - 1);
    }
    return true;
}
// ^MOVE\s+(?<x>-?\d+)\s+(?<y>-?\d+)<tail>, ^(INTERACT|DIG)... (coords), ^REQUEST\s+(?<item>(?:TRAP|RADAR))<tail>,
// ^WAIT<tail>
static bool matchCoords(const String& s, size_t p, Matched& m) {
    if (p == String::npos || (p = matchSpaces(s, p)) == String::npos) return false;
    size_t e = matchNumber(s, p);
    if (e == String::npos) return false;
    m.x = s.substr(p, e - p);
    if ((p = matchSpaces(s, e)) == String::npos) return false;
    if ((e = matchNumber(s, p)) == String::npos) return false;
    m.y = s.substr(p, e - p);
    return matchTail(s, e, m);
}
static bool matchWait(const String& s, Matched& m) {
    size_t p = matchLiteral(s, 0, "WAIT");
    return p != String::npos && matchTail(s, p, m);
}
static bool matchMove(const String& s, Matched& m) { return matchCoords(s, matchLiteral(s, 0, "MOVE"), m); }
static bool matchInteract(const String& s, Matched& m) {
    size_t p = matchLiteral(s, 0, "INTERACT");
    if (p == String::npos) p = matchLiteral(s, 0, "DIG");
    return matchCoords(s, p, m);
}
static bool matchRequest(const String& s, Matched& m) {
    size_t p = matchLiteral(s, 0, "REQUEST");
    if (p == String::npos || (p = matchSpaces(s, p)) == String::npos) return false;
    size_t e = matchLiteral(s, p, "TRAP");
    if (e == String::npos) e = matchLiteral(s, p, "RADAR");
    if (e == String::npos) return false;
    m.item = s.substr(p, e - p);
    return matchTail(s, e, m);
}

// message.replaceAll("[^\\p{L}\\p{M}\\p{N}\\p{P}\\p{Z}\\p{Cf}\\p{Cs}\\s]", ""): exact for U+0000-U+00FF
// (checked with JDK 26), every other char kept (see the header)
static bool keptByMessageFilter(char16_t c) {
    if (c < 0x20) return java::isRegexSpace(c);
    if (c >= 0x7F && c <= 0x9F) return false; // Cc
    switch (c) {
        case u'$': case u'+': case u'<': case u'=': case u'>': case u'^': case u'`': case u'|': case u'~': // Sm Sc Sk
        case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA8: case 0xA9: case 0xAC: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB4: case 0xB8: case 0xD7: case 0xF7:
            return false;
        default: return true;
    }
}

struct InvalidInput {
    String message;
};

static String invalidInputMessage(const String& line) { return u"Invalid Input: Expected " + A(EXPECTED) + u" but got '" + line + u"'"; }

static void deactivatePlayer(Player& player, const String& message) {
    // escapeHTMLEntities
    player.deactivate(java::replace(java::replace(message, u"&lt;", u"<"), u"&gt;", u">"));
}

static void matchMessage(Agent& agent, const Matched& m) {
    if (!m.hasMessage) return;
    String filtered;
    for (char16_t c : m.message)
        if (keptByMessageFilter(c)) filtered += c;
    agent.setMessage(filtered);
}

static i32 integerValueOf(const String& s) {
    i32 v;
    if (!java::parseInt(s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(s))};
    return v;
}

static void handleCommands(Player& player, const std::vector<String>& lines) {
    size_t i = 0;
    for (const String& line : lines) {
        Agent& agent = agents_[player.agents.at(i++)];
        if (agent.dead) continue;
        try {
            Matched m;
            if (matchWait(line, m)) {
                matchMessage(agent, m);
                continue;
            }
            m = Matched();
            if (matchMove(line, m)) {
                i32 x = integerValueOf(m.x);
                i32 y = integerValueOf(m.y);
                agent.intent = Action::move(Coord{x, y});
                matchMessage(agent, m);
                continue;
            }
            m = Matched();
            if (matchInteract(line, m)) {
                i32 x = integerValueOf(m.x);
                i32 y = integerValueOf(m.y);
                agent.intent = Action::dig(Coord{x, y});
                matchMessage(agent, m);
                continue;
            }
            m = Matched();
            if (matchRequest(line, m)) {
                // Item.valueOf(item.toUpperCase()): an ASCII match of TRAP or RADAR
                Item item = (m.item[0] == u't' || m.item[0] == u'T') ? TRAP : RADAR;
                agent.intent = Action::request(item);
                matchMessage(agent, m);
                continue;
            }
            throw InvalidInput{invalidInputMessage(line)};
        } catch (const InvalidInput& e) {
            deactivatePlayer(player, e.message);
            cg::gm.addToGameSummary(u"Bad command: " + e.message);
            return;
        } catch (const java::Exception& e) {
            // a NumberFormatException: new InvalidInputException(e.toString(), EXPECTED, line), and
            // e.getMessage() in the summary
            String text = java::decodeUtf8(e.text);
            deactivatePlayer(player, text + u": Expected " + A(EXPECTED) + u" but got '" + line + u"'");
            size_t colon = e.text.find(": ");
            cg::gm.addToGameSummary(u"Bad command: " + java::decodeUtf8(e.text.substr(colon + 2)));
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// View (ViewModule, EndScreenModule): text sent to the viewer
// ----------------------------------------------------------------------------
static String serializeGlobal() {
    std::vector<Coord> ore;
    for (const Coord& c : cellOrder())
        if (grid.cells[c.x][c.y].ore > 0) ore.push_back(c);
    String r = I(MAP_WIDTH) + u" " + I(MAP_HEIGHT) + u" " + I(AGENTS_PER_PLAYER) + u" " + I((i64)ore.size());
    for (const Coord& c : ore) r += u"\n" + I(c.x) + u" " + I(c.y) + u" " + I(grid.cells[c.x][c.y].ore);
    return r;
}

static String serializeFrame() {
    String r = I(players[0].getScore()) + u" " + I(players[1].getScore()) + u" " + I((i64)viewerEvents.size());
    for (Agent* agent : allAgents()) { // sorted by id
        r += u"\n" + I(agent->id) + u" " + I(agent->position.x) + u" " + I(agent->position.y) + u" 0 " + (agent->dead ? u"1" : u"0");
        r += u"\n" + (agent->hasMessage ? agent->message : String());
        if (agent->intent.hasTarget()) r += u"\n" + I(agent->intent.target.x) + u" " + I(agent->intent.target.y);
    }
    auto coalesce = [](bool has, int v) { return has ? I(v) : String(u"_"); };
    for (const EventData& e : viewerEvents)
        r += u"\n" + I(e.type) + u" " + coalesce(e.hasItem, e.item) + u" " + coalesce(e.hasAgent, e.agent) + u" " + coalesce(e.hasX, e.x) + u" " +
             coalesce(e.hasY, e.y);
    return r;
}

static void sendFrameData() { cg::gm.setViewData("graphics", cg::Json::string(serializeFrame())); }

// ----------------------------------------------------------------------------
// Referee
// ----------------------------------------------------------------------------
struct Referee : cg::Referee {
    int endScores[2] = {0, 0};
    bool tie = false;

    void init() override {
        random_ = java::Random(cg::gm.getSeed());
        generateMap();
        initPlayers();
        for (Player* p : activePlayers()) p->sendInputLine(std::to_string(MAP_WIDTH) + " " + std::to_string(MAP_HEIGHT));
        cg::gm.setFrameDuration(1000);
        cg::gm.setMaxTurns(200);
        cg::gm.setTurnMaxTime(50);
    }

    void onGameInit() override {
        cg::gm.setViewGlobalData("graphics", cg::Json::string(serializeGlobal()));
        sendFrameData();
    }

    void gameTurn(int) override {
        // resetGameTurnData
        for (Player& p : players) p.reset();
        viewerEvents.clear();

        for (Player* player : activePlayers()) {
            for (const std::string& line : getCurrentFrameInfoFor(*player)) player->sendInputLine(line);
            player->execute();
        }
        // handlePlayerCommands
        for (Player* player : activePlayers()) {
            if (player->timeout) {
                player->deactivate(A("Timeout!"));
                cg::gm.addToGameSummary(player->getNicknameToken() + u" has not provided " + I(player->getExpectedOutputLines()) + u" lines in time");
            } else {
                handleCommands(*player, player->outputs);
            }
        }

        performGameUpdate();

        if (gameOver() || activePlayers().size() < 2) cg::gm.endGame();
    }

    void onAfterGameTurn() override { sendFrameData(); }

    void onEnd() override {
        for (Player& p : players) p.setScore(p.isActive() ? p.ore : -1);
        endScores[0] = players[0].getScore();
        endScores[1] = players[1].getScore();
        tie = endScores[1] == endScores[0];
        cg::gm.putMetadata("robotsDestroyed", std::to_string(robotsDestroyed));
        cg::gm.putMetadata("trapsPlaced", std::to_string(trapsPlaced));
        cg::gm.putMetadata("oreDelivered", std::to_string(oreDelivered));
    }

    void onAfterOnEnd() override {
        sendFrameData();
        cg::gm.setViewData("endScreen", cg::Json::raw("[[" + std::to_string(endScores[0]) + "," + std::to_string(endScores[1]) + "]," + (tie ? "true" : "false") + "]"));
    }
};
static Referee referee;

int main(int argc, char** argv) {
    cg::Options options;
    options.maxPlayers = 2;
    options.engine313 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
