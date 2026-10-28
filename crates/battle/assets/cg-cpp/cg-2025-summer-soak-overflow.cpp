// Summer Challenge 2025 "Soak Overflow" - headless C++ referee, port of
// https://github.com/CodinGame/SummerChallenge2025-SoakOverflow (commit 82bbb1b, 2025-08-13, "updated
// viewer"; the game code of commit 80196b2 "fixed ties", 2025-07-19), CodinGame game engine 4.5.0,
// 2 players, league 19 (the CLI sets the league to 19: the full game of the highest league, levels 1
// to 4 being tutorials). Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints "<p0score>\n<p1score>\nseed=<N>\n" to stdout like the jar, or "null\nnull\n" (exit code 0)
// when the Java referee would have failed (see below). Exit code 1 without output when the jar's
// command line interface fails (bad -seed, a bot that cannot be started, more than 2 players).
// -l writes the trace of common/engine.h (one line per bot execution, view frame, game summary and
// tooltip block, then "END OK|<why the game failed>").
//
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption,
// int/double semantics, error texts. Notable Java behaviours reproduced here:
//   - gameManager.getRandom() is SecureRandom("SHA1PRNG") seeded with the seed (common/java.h),
//     consumed by the map generator (GridMaker) only: RandomGenerator.nextInt(origin, bound),
//     nextInt(10), nextBoolean (with the short-circuits of `a || b` and `n == 5 && b`) and
//     Collections.shuffle. The island fixing works on the shuffled wall list and the stale island
//     sets of the Java code (a bridge cell joins no island until the islands are detected again).
//     (-seed 0 is ignored by SecureRandom.setSeed: the jar then plays a random map, and so do we.)
//   - Commands: one line per live agent; "<id>;" prefixes (Integer.parseInt: '+', Unicode digits),
//     String.split(";") (trailing empty parts dropped, so ";" is a valid empty line), trim(), the
//     CASE_INSENSITIVE regexes (only ASCII letters fold, \d is [0-9], WAIT is HUNKER_DOWN), the last
//     move / combat action / message of an agent wins, the rest of a line is ignored once an agent has
//     a move, a combat action and a message, Integer.valueOf overflows and unknown agent ids
//     disqualify, and every bad line disqualifies again (one tooltip and two summary lines each).
//   - Optional.orElse(player.agents.get(index)) is evaluated eagerly (an index past the agents would
//     make the referee crash).
//   - The rules in their order: moves (the A* path finder with its java.util.PriorityQueue
//     tie-breaking, neighbours sorted by "occupied" then farthest from the centre, the nearest
//     reachable cell when the target can't be reached; int overflows of manhattanTo with huge
//     targets), move cancellations (static agents, same target or swap, then chains; the duplicated
//     conflict events of the Java code), hunkering, shots (cooldown, range, cover, hunker bonus,
//     friendly fire), splash bombs, deaths, control zones (wet agents count double distance), points,
//     the end of the game (a lead of more than 600 points, a team without agents after one more
//     "cinematic" frame, 100 turns, fewer than 2 active players) and the scores (-1 for a
//     deactivated player or a team without agents unless both are).
//   - The view data (common/engine.h counts it against the 1 MB quota and the runner's 30000-byte
//     commands): events with their animation times (double arithmetic of the Java code), control
//     zones, points and the agents' messages ('|' replaced with U+2223), and the end screen.
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps, so
// a bot answering in ~50 ms may be timed out by one and not the other; on a loaded machine the jar
// times out bots that answered in time, most often bots writing ~10 KB lines). Unreachable Java paths
// are kept but untested: GridMaker's findWallAdjacentToFreeSpace fallback (no seed of 1 to 320000
// needs it), PathFinder's "cannot move to" error (a path always has a length) and the crash of
// player.agents.get(index) (the runner always gives as many lines as agents).
// Validated (2026-10-25) with battle referee-diff against the jar on 3917 games (30 bots, every ordered
// pairing on seeds from 1, 1000001, 987654321 and 5000000000, plus extra seeds and maps with many
// islands; 2 games differed because the jar timed out a bot that had answered, both identical when
// replayed alone) and in direct mode (jar game log vs -l trace, every view frame compared) on 870
// games, including raw line endings, invalid UTF-8, U+2028/U+0085, over-sized summaries and view
// frames, and the 1 MB view data quota.

#include "common/engine.h"

#include <cmath>
#include <memory>

using java::String;

// ----------------------------------------------------------------------------
// grid/Coord.java, Tile.java
// ----------------------------------------------------------------------------
struct Coord {
    i32 x = 0, y = 0;
    Coord() {}
    Coord(i32 x_, i32 y_) : x(x_), y(y_) {}
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    // manhattanTo(other): Math.abs(other.x - x) + Math.abs(other.y - y) in int arithmetic
    i32 manhattanTo(const Coord& o) const { return java::add(java::abs(java::sub(o.x, x)), java::abs(java::sub(o.y, y))); }
    i32 chebyshevTo(const Coord& o) const { return std::max(java::abs(java::sub(o.x, x)), java::abs(java::sub(o.y, y))); }
    // sqrEuclideanTo(double, double): Math.pow(d, 2) is exact here (small integers and halves)
    double sqrEuclideanTo(double px, double py) const {
        double dx = px - (double)x, dy = py - (double)y;
        return dx * dx + dy * dy;
    }
    double euclideanTo(const Coord& o) const { return std::sqrt(sqrEuclideanTo((double)o.x, (double)o.y)); }
    std::string toIntString() const { return std::to_string(x) + " " + std::to_string(y); }
};

const int TYPE_FLOOR = 0, TYPE_LOW_COVER = 1, TYPE_HIGH_COVER = 2, TYPE_NO_TILE = -1; // Tile.NO_TILE has type -1

// Grid.ADJACENCY (N, E, S, W) and ADJACENCY_8
static const Coord ADJACENCY[4] = {Coord(0, -1), Coord(1, 0), Coord(0, 1), Coord(-1, 0)};
static const Coord ADJACENCY_8[8] = {Coord(0, -1), Coord(1, 0), Coord(0, 1), Coord(-1, 0), Coord(-1, -1), Coord(1, 1), Coord(1, -1), Coord(-1, 1)};

// ----------------------------------------------------------------------------
// grid/Grid.java
// ----------------------------------------------------------------------------
struct Grid {
    int width = 0, height = 0;
    bool ySymetry = false;
    std::vector<int> types; // y * width + x (the LinkedHashMap's order)
    std::vector<Coord> spawns;

    void init(int w, int h, bool ySym) {
        width = w;
        height = h;
        ySymetry = ySym;
        types.assign((size_t)w * h, TYPE_FLOOR);
        spawns.clear();
    }
    bool valid(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    bool valid(const Coord& c) const { return valid(c.x, c.y); }
    int index(const Coord& c) const { return c.y * width + c.x; }
    // get(c).getType(): NO_TILE outside the grid
    int type(const Coord& c) const { return valid(c) ? types[(size_t)index(c)] : TYPE_NO_TILE; }
    bool isCover(const Coord& c) const { return type(c) != TYPE_FLOOR; } // NO_TILE too
    // Tile.getCoverModifier
    double coverModifier(const Coord& c) const {
        int t = type(c);
        if (t == TYPE_LOW_COVER) return 0.5;
        if (t == TYPE_HIGH_COVER) return 0.25;
        return 1;
    }
    // get(c).setType(t) / clear(): only ever called on cells of the grid
    void setType(const Coord& c, int t) {
        if (valid(c)) types[(size_t)index(c)] = t;
    }
    // getCoords(): the cells in the LinkedHashMap's order
    Coord coordAt(int i) const { return Coord(i % width, i / width); }
    int cellCount() const { return width * height; }

    std::vector<Coord> getNeighbours(const Coord& pos, const Coord* adjacency, int n) const {
        std::vector<Coord> r;
        for (int i = 0; i < n; i++) {
            Coord c(java::add(pos.x, adjacency[i].x), java::add(pos.y, adjacency[i].y));
            if (valid(c)) r.push_back(c);
        }
        return r;
    }
    std::vector<Coord> getNeighbours(const Coord& pos) const { return getNeighbours(pos, ADJACENCY, 4); }

    Coord opposite(const Coord& c) const {
        return Coord(java::sub(java::sub(width, c.x), 1), ySymetry ? java::sub(java::sub(height, c.y), 1) : c.y);
    }
};

static Grid grid;

// ----------------------------------------------------------------------------
// grid/GridMaker.java
// ----------------------------------------------------------------------------
namespace GridMaker {

const int GRID_W_RATIO = 2, MIN_SPAWN_COUNT = 3, MAX_SPAWN_COUNT = 5, MIN_HEIGHT = 6, MAX_HEIGHT = 10;

// The islands: island[cell] is the index of the HashSet holding the cell (-1: in none), `count` the
// size of the list. Only membership and the number of islands matter in the Java code.
struct Islands {
    std::vector<int> island;
    int count = 0;
};

static Islands detectIslands() {
    Islands r;
    r.island.assign((size_t)grid.cellCount(), -1);
    std::vector<bool> computed((size_t)grid.cellCount(), false);
    for (int i = 0; i < grid.cellCount(); i++) {
        Coord p = grid.coordAt(i);
        if (grid.isCover(p)) continue;
        if (computed[(size_t)i]) continue;
        std::deque<Coord> fifo;
        fifo.push_back(p);
        computed[(size_t)i] = true;
        while (!fifo.empty()) {
            Coord e = fifo.front();
            fifo.pop_front();
            for (const Coord& d : ADJACENCY) {
                Coord n(java::add(e.x, d.x), java::add(e.y, d.y));
                if (grid.valid(n) && !grid.isCover(n) && !computed[(size_t)grid.index(n)]) {
                    fifo.push_back(n);
                    computed[(size_t)grid.index(n)] = true;
                }
            }
            r.island[(size_t)grid.index(e)] = r.count;
        }
        r.count++;
    }
    return r;
}

static void removeFirst(std::vector<Coord>& list, const Coord& c) {
    auto it = std::find(list.begin(), list.end(), c);
    if (it != list.end()) list.erase(it);
}

static bool closeIslandGap(std::vector<Coord>& wallCoords, Islands& islands) {
    std::vector<int> connecting;
    bool found = false;
    Coord bridge;
    for (const Coord& coord : wallCoords) {
        connecting.clear();
        for (const Coord& n : grid.getNeighbours(coord)) {
            int k = islands.island[(size_t)grid.index(n)];
            if (k >= 0 && std::find(connecting.begin(), connecting.end(), k) == connecting.end()) connecting.push_back(k);
        }
        if (connecting.size() > 1) {
            bridge = coord;
            found = true;
            break;
        }
    }
    if (!found) return false;
    Coord opposite = grid.opposite(bridge);
    grid.setType(bridge, TYPE_FLOOR);
    grid.setType(opposite, TYPE_FLOOR);
    removeFirst(wallCoords, bridge);
    removeFirst(wallCoords, opposite);
    // the bridging islands become one (the bridge cells join no island)
    int merged = islands.count; // a fresh index
    for (int& k : islands.island)
        if (k >= 0 && std::find(connecting.begin(), connecting.end(), k) != connecting.end()) k = merged;
    // renumber so that indices stay below count (only membership matters)
    std::vector<int> remap((size_t)merged + 1, -1);
    int next = 0;
    for (int& k : islands.island) {
        if (k < 0) continue;
        if (remap[(size_t)k] < 0) remap[(size_t)k] = next++;
        k = remap[(size_t)k];
    }
    islands.count = islands.count - (int)connecting.size() + 1;
    return true;
}

static bool findWallAdjacentToFreeSpace(const std::vector<Coord>& wallCoords, Coord& out) {
    for (const Coord& c : wallCoords) {
        for (const Coord& n : grid.getNeighbours(c))
            if (!grid.isCover(n)) {
                out = c;
                return true;
            }
    }
    return false;
}

static void fixIslands(std::vector<Coord> wallCoords, java::SHA1PRNG& random) {
    java::shuffle(wallCoords, random);
    Islands islands = detectIslands();
    while (islands.count > 1) {
        bool closed = closeIslandGap(wallCoords, islands);
        if (!closed) {
            Coord c;
            if (findWallAdjacentToFreeSpace(wallCoords, c)) {
                Coord opposite = grid.opposite(c);
                grid.setType(c, TYPE_FLOOR);
                grid.setType(opposite, TYPE_FLOOR);
                removeFirst(wallCoords, c);
                removeFirst(wallCoords, opposite);
            }
            islands = detectIslands();
        }
    }
}

static void initGrid(java::SHA1PRNG& random) {
    int h = random.nextInt(MIN_HEIGHT, MAX_HEIGHT + 1);
    int w = h * GRID_W_RATIO;
    bool ySym = random.nextBoolean() || random.nextBoolean();
    grid.init(w, h, ySym);

    // Walls
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w / 2 - 1; ++x) {
            Coord coord(x, y);
            Coord opp = grid.opposite(coord);
            int n = random.nextInt(10);
            int type = TYPE_FLOOR;
            if (n == 0)
                type = TYPE_HIGH_COVER;
            else if (n == 1)
                type = TYPE_LOW_COVER;
            grid.setType(coord, type);
            grid.setType(opp, type);
        }
    }

    // Spawns
    std::vector<Coord> allLeftCoords;
    for (int i = 0; i < grid.cellCount(); i++)
        if (grid.coordAt(i).x == 0) allLeftCoords.push_back(grid.coordAt(i));
    int spawnCount = random.nextInt(MIN_SPAWN_COUNT, MAX_SPAWN_COUNT + 1);
    java::shuffle(allLeftCoords, random);
    if (spawnCount == 5 && random.nextBoolean()) spawnCount--;
    if (spawnCount == 4 && random.nextBoolean()) spawnCount--;
    for (int i = 0; i < spawnCount; i++) {
        Coord c = allLeftCoords.at((size_t)i);
        grid.spawns.push_back(c);
        grid.setType(c, TYPE_FLOOR);
        grid.setType(grid.opposite(c), TYPE_FLOOR);
    }

    std::vector<Coord> wallCoords;
    for (int i = 0; i < grid.cellCount(); i++)
        if (grid.isCover(grid.coordAt(i))) wallCoords.push_back(grid.coordAt(i));
    fixIslands(wallCoords, random);
}

} // namespace GridMaker

// ----------------------------------------------------------------------------
// action/Action.java, ActionType.java, AgentClass.java, Agent.java, Player.java
// ----------------------------------------------------------------------------
enum ActionType { MOVE, SHOOT, THROW, HUNKER_DOWN, MESSAGE };

struct Action {
    ActionType type = MOVE;
    Coord coord;
    i32 agentId = 0;
    String message;
};

// AgentClass: cooldown, soakingPower, optimalRange, balloons
struct AgentClass {
    int cooldown, soakingPower, optimalRange, balloons;
};
static const AgentClass AGENT_CLASSES[5] = {
    {1, 16, 4, 1}, // GUNNER
    {5, 24, 6, 0}, // SNIPER
    {2, 8, 2, 3},  // BOMBER
    {2, 16, 4, 2}, // ASSAULT
    {5, 32, 2, 1}, // BERSERKER
};

struct Player;

struct Agent {
    int maxCooldown = 0, soakingPower = 0, optimalRange = 0, balloons = 0;
    bool hunkered = false;
    Coord position;
    int wetness = 0;
    bool hasMove = false, hasCombat = false, hasMessage = false;
    Action moveAction, combatAction;
    String message;
    Player* owner = nullptr;
    int id = 0;
    int shootingId = -1;
    bool dying = false;
    int cooldown = 0;

    Agent(int id_, const AgentClass& c) : maxCooldown(c.cooldown), soakingPower(c.soakingPower), optimalRange(c.optimalRange), balloons(c.balloons), id(id_) {}

    void reset() {
        hasMessage = false;
        message.clear();
        hasMove = false;
        hasCombat = false;
        hunkered = false;
        shootingId = -1;
        cooldown = std::max(0, cooldown - 1);
    }
    int actionCount() const { return (hasMove ? 1 : 0) + (hasCombat ? 1 : 0); }
    bool hasThrowAction() const { return hasCombat && combatAction.type == THROW; }
    bool hasHunkerDownAction() const { return hasCombat && combatAction.type == HUNKER_DOWN; }
    bool hasShootAction() const { return hasCombat && combatAction.type == SHOOT; }
    // getDistanceMultiplier (WETNESS_AFFECTS_DISTANCE)
    double getDistanceMultiplier() const { return wetness >= 50 ? 2 : 1; }
};

static std::vector<std::unique_ptr<Agent>> agentStore;

struct Player : cg::Player {
    std::vector<Agent*> agents;
    int points = 0;

    int getLiveAgentCount() const {
        int n = 0;
        for (const Agent* a : agents)
            if (!a->dying) n++;
        return n;
    }
    int getExpectedOutputLines() const override { return getLiveAgentCount(); }
    void reset() {
        for (Agent* a : agents) a->reset();
    }
    void addScore(int p) {
        points = java::add(points, p);
        setScore(points);
    }
    Agent* getAgentById(i32 agentId) const {
        for (Agent* a : agents)
            if (a->id == agentId) return a;
        return nullptr;
    }
    std::string nick() const { return "$" + std::to_string(index); }
};

static Player players[2];

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.isActive()) r.push_back(&p);
    return r;
}

// ----------------------------------------------------------------------------
// event/Animation.java, AnimationData.java, EventData.java
// ----------------------------------------------------------------------------
const int EV_MOVE = 0, EV_HUNKER = 1, EV_SHOOT = 2, EV_DEATH = 3, EV_THROW = 4, EV_MOVE_CONFLICT = 5;
const int ANIM_THIRD = 300, ANIM_HALF = 500, ANIM_WHOLE = 1000;

struct EventData {
    int type = 0;
    int start = 0, end = 0;
    bool hasCoord = false, hasTarget = false;
    Coord coord, target;
    std::vector<int> params;
};

struct Animation {
    int frameTime = 0, endTime = 0;
    std::vector<EventData> viewerEvents;

    void reset() {
        viewerEvents.clear();
        frameTime = 0;
        endTime = 0;
    }
    void startAnim(EventData e, int duration) {
        e.start = frameTime;
        e.end = frameTime + duration;
        endTime = std::max(endTime, frameTime + duration);
        viewerEvents.push_back(e);
    }
    void catchUp() { frameTime = endTime; }
};

static Animation animation;

// ----------------------------------------------------------------------------
// pathfinding/AStar.java, PathFinder.java
// ----------------------------------------------------------------------------
namespace PathFinding {

struct PathItem {
    int cumulativeLength = 0;
    int totalPrevisionalLength = 0;
    int precedent = -1;
    Coord coord;
};

struct AStar {
    Coord from, target, nearest;
    const std::vector<Coord>& restricted;
    double centreX, centreY;
    std::vector<PathItem> items;
    std::vector<bool> closed; // closedList (in-grid coords only)

    AStar(const Coord& from_, const Coord& target_, const std::vector<Coord>& restricted_)
        : from(from_), target(target_), nearest(from_), restricted(restricted_), centreX(grid.width / 2.0), centreY(grid.height / 2.0) {}

    bool isRestricted(const Coord& c) const { return std::find(restricted.begin(), restricted.end(), c) != restricted.end(); }
    // byDistanceToCentre: Comparator.comparing(c -> -c.sqrEuclideanTo(centreX, centreY))
    int byDistanceToCentre(const Coord& a, const Coord& b) const {
        return java::compareDouble(-a.sqrEuclideanTo(centreX, centreY), -b.sqrEuclideanTo(centreX, centreY));
    }

    // getPathItemLinkedList: the item index of the target, or -1
    int search() {
        closed.assign((size_t)grid.cellCount(), false);
        auto cmp = [this](int a, int b) { return java::compareInt(items[(size_t)a].totalPrevisionalLength, items[(size_t)b].totalPrevisionalLength); };
        java::PriorityQueue<int, decltype(cmp)> openList(cmp);
        PathItem root;
        root.coord = from;
        items.push_back(root);
        openList.offer(0);

        while (openList.size() > 0) {
            int visiting = openList.poll();
            Coord visitingCoord = items[(size_t)visiting].coord;
            if (visitingCoord == target) return visiting;
            // the coords in the open list are always in the grid (the root is an agent's position)
            if (closed[(size_t)grid.index(visitingCoord)]) continue;
            closed[(size_t)grid.index(visitingCoord)] = true;

            std::vector<Coord> neighbors = grid.getNeighbours(visitingCoord);
            java::sort(neighbors, [this](const Coord& a, const Coord& b) {
                int c = java::compareInt(isRestricted(a) ? 1 : 0, isRestricted(b) ? 1 : 0);
                if (c != 0) return c;
                return byDistanceToCentre(a, b);
            });
            for (const Coord& neighbor : neighbors)
                if (!grid.isCover(neighbor)) addToOpenList(openList, visiting, visitingCoord, neighbor);

            i32 visitingDist = visitingCoord.manhattanTo(target);
            i32 nearestDist = nearest.manhattanTo(target);
            if (visitingDist < nearestDist) {
                nearest = visitingCoord;
            } else if (visitingDist == nearestDist) {
                if (byDistanceToCentre(visitingCoord, nearest) < 0) nearest = visitingCoord;
            }
        }
        return -1;
    }

    template <class Q>
    void addToOpenList(Q& openList, int visiting, const Coord& fromCoord, const Coord& toCoord) {
        if (closed[(size_t)grid.index(toCoord)]) return;
        PathItem pi;
        pi.coord = toCoord;
        pi.cumulativeLength = java::add(items[(size_t)visiting].cumulativeLength, 1); // weightFunction: 1
        pi.totalPrevisionalLength = java::add(pi.cumulativeLength, fromCoord.manhattanTo(toCoord));
        pi.precedent = visiting;
        items.push_back(pi);
        openList.offer((int)items.size() - 1);
    }

    // find(): the path items from the root to the target (empty when not found)
    std::vector<PathItem> find() {
        int item = search();
        std::vector<PathItem> path;
        for (int i = item; i >= 0; i = items[(size_t)i].precedent) path.insert(path.begin(), items[(size_t)i]);
        return path;
    }
};

struct PathFinderResult {
    std::vector<Coord> path;
    int weightedLength = -1;
    bool hasNextCoord() const { return path.size() > 1; }
    Coord getNextCoord() const { return path[1]; }
    bool hasNoPath() const { return weightedLength == -1; }
};

static PathFinderResult findPath(const Coord& from, const Coord& to, const std::vector<Coord>& restricted) {
    AStar a(from, to, restricted);
    std::vector<PathItem> pathItems = a.find();
    PathFinderResult pfr;
    if (pathItems.empty()) {
        AStar b(from, a.nearest, restricted);
        pathItems = b.find();
    }
    for (const PathItem& item : pathItems) pfr.path.push_back(item.coord);
    if (pathItems.empty()) throw java::Exception{java::indexOutOfBoundsException(-1, 0)}; // pathItems.get(size - 1)
    pfr.weightedLength = pathItems.back().cumulativeLength;
    return pfr;
}

} // namespace PathFinding

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
namespace Game {

const int THROW_DAMAGE = 30, THROW_DISTANCE_MAX = 4, MAX_POINT_DIFF = 600;

static int teamlessCount = 0;
static const int leagueLevel = 19;
static std::vector<int> zoneOwner; // per cell: 0, 1 (in that player's controlZone list) or -1
static int zoneSize[2] = {0, 0};
static std::string endScreenJson; // EndScreenModule.setScores(scores, scoreTexts)

// allAgentStream(): player 0's agents, then player 1's
static std::vector<Agent*> allAgents() {
    std::vector<Agent*> r;
    for (Player& p : players)
        for (Agent* a : p.agents) r.push_back(a);
    return r;
}

static Agent* getAgentById(i32 targetId) {
    for (Agent* a : allAgents())
        if (a->id == targetId) return a;
    return nullptr;
}

static void clearControlZones() {
    zoneOwner.assign((size_t)grid.cellCount(), -1);
    zoneSize[0] = zoneSize[1] = 0;
}

static void updateControlZones() {
    std::vector<Agent*> liveAgents;
    for (Agent* a : allAgents())
        if (!a->dying) liveAgents.push_back(a);
    for (int i = 0; i < grid.cellCount(); i++) {
        Coord c = grid.coordAt(i);
        // grid.getClosestTargets(c, liveAgents)
        std::vector<Agent*> closest;
        int closestBy = 0;
        for (Agent* targ : liveAgents) {
            int distance = java::d2i((double)c.manhattanTo(targ->position) * targ->getDistanceMultiplier());
            if (closest.empty() || closestBy > distance) {
                closest.clear();
                closest.push_back(targ);
                closestBy = distance;
            } else if (closestBy == distance) {
                closest.push_back(targ);
            }
        }
        if (closest.empty()) continue;
        bool one = true;
        for (Agent* a : closest)
            if (a->owner != closest[0]->owner) one = false;
        if (one) {
            int owner = closest[0]->owner->index;
            zoneOwner[(size_t)i] = owner;
            zoneSize[owner]++;
        }
    }
}

static void initPlayers() {
    int agentId = 1;
    for (Player& p : players) {
        int agentIdx = 0;
        for (Coord c : grid.spawns) {
            const AgentClass& agentClass = AGENT_CLASSES[agentIdx++];
            agentStore.push_back(std::make_unique<Agent>(agentId++, agentClass));
            Agent* a = agentStore.back().get();
            if (p.index == 1) c = grid.opposite(c);
            p.agents.push_back(a);
            a->owner = &p;
            a->position = c;
        }
    }
}

static void init() {
    teamlessCount = 0;
    // tutorialManager.initTutorial(random, 19) is false: no tutorial
    GridMaker::initGrid(cg::gm.getRandom());
    initPlayers();
    clearControlZones();
    updateControlZones();
}

static void launchDeathEvent(Agent* a) {
    EventData e;
    e.type = EV_DEATH;
    e.hasCoord = true;
    e.coord = a->position;
    e.params = {a->id};
    animation.startAnim(e, ANIM_WHOLE);
}

static void resetGameTurnData() {
    animation.reset();
    std::vector<Agent*> deadAgents;
    for (Agent* a : allAgents())
        if (a->dying) deadAgents.push_back(a);
    for (Agent* a : deadAgents) {
        std::vector<Agent*>& list = a->owner->agents;
        list.erase(std::find(list.begin(), list.end(), a));
        launchDeathEvent(a);
    }
    for (Player& p : players) p.reset();
    clearControlZones();
}

static void reportPlayerError(const std::string& message) {
    cg::gm.addToGameSummary(cg::GameManager::formatErrorMessage(java::decodeUtf8(message)));
}

// --- moves
struct Move {
    Agent* agent;
    Coord from, to;
    int uid; // record identity (each agent has at most one Move: equals() is identity here)
};

static void launchMoveEvent(Agent* a, const Coord& from, const Coord& to) {
    EventData e;
    e.type = EV_MOVE;
    e.hasCoord = e.hasTarget = true;
    e.coord = from;
    e.target = to;
    e.params = {a->id};
    animation.startAnim(e, ANIM_HALF);
}

static void launchMoveConflictEvent(const Move& move) {
    EventData e;
    e.type = EV_MOVE_CONFLICT;
    e.hasCoord = e.hasTarget = true;
    e.coord = move.from;
    e.target = move.to;
    e.params = {move.agent->id};
    animation.startAnim(e, ANIM_THIRD);
}

static bool containsMove(const std::vector<Move>& list, const Move& m) {
    for (const Move& o : list)
        if (o.uid == m.uid) return true;
    return false;
}

static void doMoves() {
    std::vector<Move> moves;
    std::vector<Agent*> all = allAgents();
    std::vector<bool> isStatic(all.size(), true); // staticAgents (a HashSet: membership only)
    std::vector<Coord> currentlyOccupied;
    for (Agent* a : all) currentlyOccupied.push_back(a->position);

    for (size_t i = 0; i < all.size(); i++) {
        Agent* a = all[i];
        if (!a->hasMove) continue;
        PathFinding::PathFinderResult res = PathFinding::findPath(a->position, a->moveAction.coord, currentlyOccupied);
        if (res.hasNextCoord()) {
            moves.push_back(Move{a, a->position, res.getNextCoord(), (int)moves.size()});
            isStatic[i] = false;
        } else if (res.hasNoPath()) { // unreachable: a path always has a length
            reportPlayerError(a->owner->nick() + " id=" + std::to_string(a->id) + " cannot move to " + a->moveAction.coord.toIntString());
        }
    }

    // COLLISIONS
    std::vector<Move> movesToCancel;
    for (const Move& m : moves) {
        bool collides = false; // collidesWithStaticAgent
        for (size_t i = 0; i < all.size(); i++)
            if (isStatic[i] && all[i]->position == m.to) collides = true;
        if (collides) movesToCancel.push_back(m);
    }
    for (const Move& m : movesToCancel) {
        for (size_t k = 0; k < moves.size(); k++)
            if (moves[k].uid == m.uid) {
                moves.erase(moves.begin() + (long)k);
                break;
            }
        launchMoveConflictEvent(m);
    }
    // Detect move conflicts
    for (const Move& m : moves)
        for (const Move& o : moves)
            if (o.uid != m.uid && (o.to == m.to || (o.to == m.from && m.to == o.from))) movesToCancel.push_back(o);
    while (!movesToCancel.empty()) {
        std::vector<Move> nextMovesToCancel;
        std::vector<Move> kept;
        for (const Move& m : moves)
            if (!containsMove(movesToCancel, m)) kept.push_back(m);
        moves = kept;
        for (const Move& cancelled : movesToCancel) {
            launchMoveConflictEvent(cancelled);
            for (const Move& o : moves)
                if (o.uid != cancelled.uid && o.to == cancelled.from) nextMovesToCancel.push_back(o);
        }
        movesToCancel = nextMovesToCancel;
    }
    for (const Move& m : moves) {
        m.agent->position = m.to;
        launchMoveEvent(m.agent, m.from, m.to);
    }
}

// --- hunkers
static void doHunkers() {
    for (Agent* a : allAgents()) {
        if (!a->hasHunkerDownAction()) continue;
        a->hunkered = true;
        EventData e;
        e.type = EV_HUNKER;
        e.params = {a->id};
        animation.startAnim(e, ANIM_THIRD);
    }
}

// --- shoots
static double getCoverModifier(const Agent* target, const Agent* shooter) {
    double dx = (double)target->position.x - (double)shooter->position.x;
    double dy = (double)target->position.y - (double)shooter->position.y;
    double bestModifier = 1;
    double ds[2][2] = {{dx, 0}, {0, dy}};
    for (auto& d : ds) {
        if (std::fabs(d[0]) > 1 || std::fabs(d[1]) > 1) {
            int adjX = (int)-(d[0] > 0 ? 1.0 : (d[0] < 0 ? -1.0 : 0.0)); // (int) -Math.signum(d)
            int adjY = (int)-(d[1] > 0 ? 1.0 : (d[1] < 0 ? -1.0 : 0.0));
            Coord coverPos(java::add(target->position.x, adjX), java::add(target->position.y, adjY));
            if (coverPos.chebyshevTo(shooter->position) > 1) bestModifier = java::min(bestModifier, grid.coverModifier(coverPos));
        }
    }
    return bestModifier;
}

static double getRangeModifier(const Agent* target, const Agent* shooter) {
    double distanceToTarget = (double)target->position.manhattanTo(shooter->position);
    if (distanceToTarget <= shooter->optimalRange) return 1;
    if (distanceToTarget <= shooter->optimalRange * 2) return 0.5;
    return 0;
}

static void launchShootEvent(const Agent* target, const Agent* shooter, double rangeModifier, int damage) {
    double distance = shooter->position.euclideanTo(target->position);
    int stretchTime = java::d2i(ANIM_HALF * distance / 7);
    int holdTime = ANIM_HALF;
    EventData e;
    e.type = EV_SHOOT;
    e.hasCoord = e.hasTarget = true;
    e.coord = shooter->position;
    e.target = target->position;
    e.params = {shooter->id, target->id, java::d2i(rangeModifier * 2), damage, java::d2i(((double)stretchTime / (stretchTime + holdTime)) * 10000)};
    animation.startAnim(e, stretchTime + holdTime);
}

static void doShoots() {
    for (Agent* a : allAgents()) {
        if (!a->hasShootAction()) continue;
        const Action& shootAction = a->combatAction;
        std::string nick = a->owner->nick();
        if (a->cooldown > 0) {
            reportPlayerError(nick + " agent id " + std::to_string(a->id) + " cannot shoot yet. Cooldown = " + std::to_string(a->cooldown));
            continue;
        }
        i32 targetId = shootAction.agentId;
        Agent* target = getAgentById(targetId);
        if (target == nullptr) {
            reportPlayerError(nick + " agent id " + std::to_string(a->id) + " cannot shoot agent id " + std::to_string(targetId) + ", they do not exist");
            continue;
        } else if (target == a) {
            reportPlayerError(nick + " agent id " + std::to_string(targetId) + " cannot shoot themselves");
            continue;
        }
        double rangeModifier = getRangeModifier(target, a);
        if (rangeModifier == 0) {
            reportPlayerError(nick + " agent id " + std::to_string(a->id) + " cannot shoot id " + std::to_string(targetId) + ", they are too far away");
            continue;
        }
        double coverModifier = getCoverModifier(target, a);
        double hunkerModifierBonus = target->hunkered ? 0.25 : 0;
        int damage = java::l2i(java::round((double)a->soakingPower * rangeModifier * (coverModifier - hunkerModifierBonus)));
        target->wetness = java::add(target->wetness, damage);
        a->shootingId = target->id;
        launchShootEvent(target, a, rangeModifier, damage);
        a->cooldown = a->maxCooldown + 1;
    }
}

// --- throws
static void launchThrowEvent(const Agent* agent, const Coord& target, const std::vector<Agent*>& gotWet) {
    double distance = agent->position.euclideanTo(target);
    EventData e;
    e.type = EV_THROW;
    e.hasCoord = e.hasTarget = true;
    e.coord = agent->position;
    e.target = target;
    int throwTime = java::d2i(ANIM_WHOLE * distance / 3);
    int boomTime = ANIM_THIRD * 2;
    e.params = {agent->id, THROW_DAMAGE, java::d2i(((double)throwTime / (throwTime + boomTime)) * 10000)};
    for (Agent* w : gotWet) e.params.push_back(w->id);
    animation.startAnim(e, throwTime + boomTime);
}

static void doThrows() {
    for (Agent* agent : allAgents()) {
        if (!agent->hasThrowAction()) continue;
        Coord target = agent->combatAction.coord;
        std::string nick = agent->owner->nick();
        if (!grid.valid(target)) {
            reportPlayerError(nick + " cannot throw splash bombs to " + target.toIntString() + ".");
            continue;
        }
        if (target.manhattanTo(agent->position) > THROW_DISTANCE_MAX) {
            reportPlayerError(nick + " cannot throw splash bomb to " + target.toIntString() + ", it is too far away from agent " + std::to_string(agent->id) + ".");
            continue;
        }
        if (agent->balloons <= 0) {
            reportPlayerError(nick + " agent " + std::to_string(agent->id) + " has no splash bomb to throw.");
            continue;
        }
        std::vector<Coord> affected{target};
        for (const Coord& n : grid.getNeighbours(target, ADJACENCY_8, 8)) affected.push_back(n);
        std::vector<Agent*> gotWet;
        for (Agent* other : allAgents())
            if (std::find(affected.begin(), affected.end(), other->position) != affected.end()) gotWet.push_back(other);
        for (Agent* other : gotWet) other->wetness = java::add(other->wetness, THROW_DAMAGE);
        launchThrowEvent(agent, target, gotWet);
        agent->balloons--;
    }
}

static void doDeaths() {
    for (Agent* a : allAgents())
        if (a->wetness >= 100) a->dying = true;
}

static void scorePoints() {
    for (Player& p : players) {
        int diff = zoneSize[p.index] - zoneSize[1 - p.index];
        if (diff > 0) p.addScore(diff);
    }
}

static bool isCinematicFrame() { return teamlessCount > 0; } // !ONE_PLAYER_MODE

static bool shouldSkipPlayerTurn(const Player&) { return isCinematicFrame(); }

static void computeEvents() {
    int minTime = 500;
    animation.catchUp();
    int frameTime = std::max(animation.frameTime, minTime);
    cg::gm.setFrameDuration(frameTime);
}

static bool isGameOver() {
    bool p0win = players[0].points > java::add(players[1].points, MAX_POINT_DIFF);
    bool p1win = players[1].points > java::add(players[0].points, MAX_POINT_DIFF);
    return p0win || p1win || teamlessCount > 0 || getActivePlayers().size() < 2;
}

static void countTeamless() {
    teamlessCount = 0;
    for (Player& p : players)
        if (p.getLiveAgentCount() == 0) teamlessCount++;
}

static void performGameUpdate() {
    if (!isCinematicFrame()) {
        doMoves();
        animation.catchUp();
        doHunkers();
        animation.catchUp();
        doShoots();
        doThrows();
        animation.catchUp();
        doDeaths();
        updateControlZones();
        scorePoints();
    }
    computeEvents();
    if (isGameOver()) cg::gm.endGame();
    countTeamless();
}

static void onEnd() {
    std::string scoreTexts[2];
    bool bothDead = teamlessCount == 2;
    for (Player& p : players) {
        if (!p.isActive() || (p.getLiveAgentCount() == 0 && !bothDead)) {
            p.setScore(-1);
            scoreTexts[p.index] = "-";
        } else {
            p.setScore(p.points);
            scoreTexts[p.index] = std::to_string(p.points) + " point" + (p.points > 1 ? "s" : "");
        }
    }
    endScreenJson = "[[" + std::to_string(players[0].getScore()) + "," + std::to_string(players[1].getScore()) + "],\"logo.png\",[" +
                    java::jsonQuote(java::ascii(scoreTexts[0])) + "," + java::jsonQuote(java::ascii(scoreTexts[1])) + "]]";
}

} // namespace Game

// ----------------------------------------------------------------------------
// Serializer.java
// ----------------------------------------------------------------------------
namespace Serializer {

static std::vector<std::string> serializeGlobalInfoFor(const Player& player) {
    std::vector<std::string> lines;
    lines.push_back(std::to_string(player.index));
    std::vector<Agent*> all = Game::allAgents();
    lines.push_back(std::to_string(all.size()));
    for (const Agent* a : all)
        lines.push_back(std::to_string(a->id) + " " + std::to_string(a->owner->index) + " " + std::to_string(a->maxCooldown) + " " +
                        std::to_string(a->optimalRange) + " " + std::to_string(a->soakingPower) + " " + std::to_string(a->balloons));
    lines.push_back(std::to_string(grid.width) + " " + std::to_string(grid.height));
    for (int y = 0; y < grid.height; ++y) {
        std::string row;
        for (int x = 0; x < grid.width; ++x) {
            if (x) row += " ";
            row += std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(grid.type(Coord(x, y)));
        }
        lines.push_back(row);
    }
    return lines;
}

static std::vector<std::string> serializeFrameInfoFor(const Player& player) {
    std::vector<std::string> lines;
    std::vector<Agent*> all = Game::allAgents();
    lines.push_back(std::to_string(all.size()));
    for (const Agent* a : all)
        lines.push_back(std::to_string(a->id) + " " + std::to_string(a->position.x) + " " + std::to_string(a->position.y) + " " + std::to_string(a->cooldown) +
                        " " + std::to_string(a->balloons) + " " + std::to_string(a->wetness));
    lines.push_back(std::to_string(player.agents.size()));
    return lines;
}

// the view's global data (ViewModule)
static String serializeGlobalData() {
    std::string s = std::to_string(Game::leagueLevel) + "|" + std::to_string(grid.width) + "|" + std::to_string(grid.height) + "|";
    for (int y = 0; y < grid.height; ++y)
        for (int x = 0; x < grid.width; ++x) s += std::to_string(grid.type(Coord(x, y)));
    std::vector<Agent*> all = Game::allAgents();
    s += "|" + std::to_string(all.size());
    for (const Agent* a : all)
        s += "|" + std::to_string(a->id) + "|" + std::to_string(a->position.x) + "|" + std::to_string(a->position.y) + "|" + std::to_string(a->maxCooldown) + "|" +
             std::to_string(a->optimalRange) + "|" + std::to_string(a->soakingPower) + "|" + std::to_string(a->owner->index) + "|" +
             std::to_string(a->balloons) + "|" + std::to_string(a->wetness);
    return java::ascii(s);
}

// the view's frame data: the events, the control zones, the points, the agents' messages ('|'
// replaced with U+2223)
static String serializeFrameData() {
    std::string s = std::to_string(animation.viewerEvents.size());
    for (const EventData& e : animation.viewerEvents) {
        s += "|" + std::to_string(e.type) + "|" + std::to_string(e.start) + "|" + std::to_string(e.end) + "|";
        if (e.hasCoord) s += e.coord.toIntString();
        s += "|";
        if (e.hasTarget) s += e.target.toIntString();
        s += "|";
        for (size_t i = 0; i < e.params.size(); i++) s += (i ? " " : "") + std::to_string(e.params[i]);
    }
    s += "|";
    for (int i = 0; i < grid.cellCount(); i++) s += (char)('0' + (Game::zoneOwner[(size_t)i] < 0 ? 2 : Game::zoneOwner[(size_t)i]));
    for (const Player& p : players) s += "|" + std::to_string(p.points);
    std::vector<Agent*> messagers;
    for (Agent* a : Game::allAgents())
        if (a->hasMessage) messagers.push_back(a);
    s += "|" + std::to_string(messagers.size());
    String r = java::ascii(s);
    for (const Agent* a : messagers) {
        r += java::ascii("|" + std::to_string(a->id) + "|");
        r += java::replace(a->message, u"|", u"∣");
    }
    return r;
}

} // namespace Serializer

// ----------------------------------------------------------------------------
// CommandManager.java, action/ActionType.java
// ----------------------------------------------------------------------------
namespace Commands {

// `s` matches the ASCII literal `lit` (CASE_INSENSITIVE, no UNICODE_CASE) at `pos`
static bool literalAt(const String& s, size_t& pos, const char* lit) {
    size_t n = strlen(lit);
    if (s.size() - pos < n) return false;
    for (size_t i = 0; i < n; i++)
        if (!java::regexCharMatchesIgnoreCase(s[pos + i], lit[i])) return false;
    pos += n;
    return true;
}

// (?<g>-?\d+) or (?<g>\d+): the group at `pos`
static bool digitsAt(const String& s, size_t& pos, String& group, bool minus) {
    size_t a = pos;
    if (minus && pos < s.size() && s[pos] == u'-') pos++;
    size_t d = pos;
    while (pos < s.size() && java::isRegexDigit(s[pos])) pos++;
    if (pos == d) {
        pos = a;
        return false;
    }
    group = s.substr(a, pos - a);
    return true;
}

struct BadCommand {}; // any Exception in the command loop: InvalidInputException(getExpected(command), command)

// Integer.valueOf of a group: a NumberFormatException is caught and disqualifies
static i32 valueOf(const String& g) {
    i32 v;
    if (!java::parseInt(g, v)) throw BadCommand{};
    return v;
}

// ActionType.values() in order, Pattern.matches(): true when `command` matches one of them
static bool parseAction(const String& command, Action& action) {
    {
        // MOVE: ^MOVE (?<x>\d+) (?<y>\d+)
        size_t pos = 0;
        String gx, gy;
        if (literalAt(command, pos, "MOVE ") && digitsAt(command, pos, gx, false) && literalAt(command, pos, " ") && digitsAt(command, pos, gy, false) &&
            pos == command.size()) {
            action.type = MOVE;
            i32 x = valueOf(gx);
            i32 y = valueOf(gy);
            action.coord = Coord(x, y);
            return true;
        }
    }
    {
        // SHOOT: ^SHOOT (?<id>\d+)
        size_t pos = 0;
        String gid;
        if (literalAt(command, pos, "SHOOT ") && digitsAt(command, pos, gid, false) && pos == command.size()) {
            action.type = SHOOT;
            action.agentId = valueOf(gid);
            return true;
        }
    }
    {
        // THROW: ^THROW (?<x>-?\d+) (?<y>-?\d+)
        size_t pos = 0;
        String gx, gy;
        if (literalAt(command, pos, "THROW ") && digitsAt(command, pos, gx, true) && literalAt(command, pos, " ") && digitsAt(command, pos, gy, true) &&
            pos == command.size()) {
            action.type = THROW;
            i32 x = valueOf(gx);
            i32 y = valueOf(gy);
            action.coord = Coord(x, y);
            return true;
        }
    }
    // HUNKER_DOWN: ^(WAIT|HUNKER_DOWN)
    if (java::regexMatchesIgnoreCase(command, "WAIT") || java::regexMatchesIgnoreCase(command, "HUNKER_DOWN")) {
        action.type = HUNKER_DOWN;
        return true;
    }
    {
        // MESSAGE: ^MESSAGE (?<message>[^;]*) (the command holds no ';')
        size_t pos = 0;
        if (literalAt(command, pos, "MESSAGE ")) {
            action.type = MESSAGE;
            action.message = command.substr(pos);
            return true;
        }
    }
    return false;
}

static void deactivatePlayer(Player& player, const String& message) {
    // escapeHTMLEntities
    player.deactivate(java::replace(java::replace(message, u"&lt;", u"<"), u"&gt;", u">"));
    player.setScore(-1);
}

static void parseCommands(Player& player, const std::vector<String>& lines) {
    for (size_t actionIdx = 0; actionIdx < lines.size(); ++actionIdx) {
        const String& line = lines[actionIdx];
        std::vector<String> commands = java::split(line, u';');
        // getAgentId: Integer.parseInt(tokens[0]) when there are 2+ tokens
        bool hasId = false;
        i32 explicitId = 0;
        if (commands.size() > 1 && java::parseInt(commands[0], explicitId)) hasId = true;
        size_t startCommandsIdx = hasId ? 1 : 0;
        // agentIdOpt.orElse(player.agents.get(actionIdx).getId()): the argument is always evaluated
        if (actionIdx >= player.agents.size()) throw java::Exception{java::indexOutOfBoundsException((i64)actionIdx, (i64)player.agents.size())};
        i32 agentId = hasId ? explicitId : player.agents[actionIdx]->id;

        String command;
        try {
            for (size_t i = startCommandsIdx; i < commands.size(); ++i) {
                command = java::trim(commands[i]);
                bool found = false;
                Agent* agent = player.getAgentById(agentId);
                if (agent == nullptr) throw BadCommand{};
                Action action;
                if (parseAction(command, action)) {
                    // Agent can have 1 move, 1 message and 1 other
                    if (action.type == MESSAGE) {
                        agent->hasMessage = true;
                        agent->message = java::trim(action.message);
                    } else if (action.type == MOVE) {
                        agent->hasMove = true;
                        agent->moveAction = action;
                    } else {
                        agent->hasCombat = true;
                        agent->combatAction = action;
                    }
                    found = true;
                }
                if (agent->actionCount() >= 2 && agent->hasMessage) break;
                if (!found) throw BadCommand{};
            }
        } catch (const BadCommand&) {
            String message = u"Invalid Input: Expected MOVE x y | SHOOT id | THROW x y | HUNKER_DOWN | MESSAGE text but got '" + command + u"'";
            deactivatePlayer(player, message);
            cg::gm.addToGameSummary(message);
            cg::gm.addToGameSummary(cg::GameManager::formatErrorMessage(java::ascii(player.nick() + ": disqualified!")));
        }
    }
}

} // namespace Commands

// ----------------------------------------------------------------------------
// Referee.java, ViewModule, EndScreenModule
// ----------------------------------------------------------------------------
struct SoakOverflowReferee : cg::Referee {
    void init() override {
        try {
            cg::gm.setMaxTurns(100);
            cg::gm.setFirstTurnMaxTime(1000);
            Game::init();
            // sendGlobalInfo
            for (Player* player : getActivePlayers())
                for (const std::string& line : Serializer::serializeGlobalInfoFor(*player)) player->sendInputLine(line);
            cg::gm.setFrameDuration(1000);
            cg::gm.setTurnMaxTime(50);
        } catch (const java::Exception&) {
            // "Referee failed to initialize" (unreachable: the map generator never throws)
            cg::gm.endGame();
        }
    }

    void gameTurn(int turn) override {
        (void)turn;
        Game::resetGameTurnData();
        for (Player* player : getActivePlayers()) {
            if (Game::shouldSkipPlayerTurn(*player)) continue;
            for (const std::string& line : Serializer::serializeFrameInfoFor(*player)) player->sendInputLine(line);
            player->execute();
        }
        // handlePlayerCommands
        for (Player* player : getActivePlayers()) {
            if (Game::shouldSkipPlayerTurn(*player)) continue;
            if (player->timeout) { // getOutputs() throws TimeoutException
                player->deactivate(u"Timeout!");
                cg::gm.addToGameSummary(player->nick() + " has not provided " + std::to_string(player->getExpectedOutputLines()) + " lines in time");
            } else {
                Commands::parseCommands(*player, player->outputs);
            }
        }
        Game::performGameUpdate();
        if (getActivePlayers().size() < 2) cg::gm.endGame();
    }

    void onEnd() override { Game::onEnd(); }

    // ViewModule
    void onGameInit() override {
        cg::gm.setViewGlobalData("graphics", cg::Json::string(Serializer::serializeGlobalData()));
        cg::gm.setViewData("graphics", cg::Json::string(Serializer::serializeFrameData()));
    }
    void onAfterGameTurn() override { cg::gm.setViewData("graphics", cg::Json::string(Serializer::serializeFrameData())); }
    // EndScreenModule
    void onAfterOnEnd() override { cg::gm.setViewData("endScreen", cg::Json::raw(Game::endScreenJson)); }
};

int main(int argc, char** argv) {
    static SoakOverflowReferee referee;
    cg::Options options;
    options.maxPlayers = 2;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
