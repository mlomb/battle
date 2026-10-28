// Summer Challenge 2026 "Back Track King" - headless C++ referee, port of
// https://github.com/CGjupoulton/SummerChallenge2026 (commit 23b2a02, "statement/stub fixes + updated
// game engine"), CodinGame game engine 4.7.8, 2 players, league 19 (the CLI sets the league to 19: no
// tutorial objective, the full game of the top league).
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
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption,
// int/float semantics, error texts. Notable Java behaviours reproduced here:
//   - gameManager.getRandom() is SecureRandom("SHA1PRNG") seeded with the seed (common/java.h),
//     consumed by RandomGenerator.nextInt(origin, bound), nextFloat(bound), Random.nextInt(bound),
//     nextFloat, nextBoolean and Collections.shuffle, all in the map generator (GridMaker). Its float
//     arithmetic (river weights, Math.round(float)) is reproduced bit for bit. (-seed 0 is ignored
//     by SecureRandom.setSeed: the jar then plays a random map, and so do we.)
//   - java.util.HashSet<Coord> iteration orders where the map generator draws a random element of
//     a set turned into a list (mountain growth, region growth): java::HashMapOrder.
//   - java.util.PriorityQueue tie order of the AUTOPLACE A* (a Dijkstra on tracks to buy, ties broken
//     by direction then by heap order), including its stale nodes and its path reconstruction through
//     LinkedHashMaps keyed by (cursor, state); TrainBFS's neighbour "sort" (its comparator always
//     returns 4: TimSort keeps the N, E, S, W order).
//   - Commands: String.trim/split(";"), the CASE_INSENSITIVE regexes (only ASCII letters fold),
//     Integer.parseInt overflow (the player is disqualified with the NumberFormatException text, the
//     actions parsed before the bad one still play that turn), the summary/tooltip texts.
//   - Engine 4.7.8 (common/engine.h, cg::Options::engine47): the bots get 50 ms + 50 ms per turn
//     (1000 ms + 50 ms on their first turn), and a bot over 50 ms (1000 ms on its first turn) in 3
//     turns, by the runner's clock, times out (soft time limit). -timeout replaces the runner's limit
//     only: like with the jar, a bot slower than 50 ms is still timed out on its third slow turn.
//     The protocol between the runner and the referee (java.util.Scanner lines, 30000-byte
//     commands), the 1 MB view data quota (MESSAGE texts go to the view: long messages make the jar
//     fail, and the port too), the game summary quotas.
//   - Game end: 100 game turns, no desired connection possible any more (a BFS: TerrainAStar only
//     answers whether a path exists), or fewer than 2 active players. Scores: the points of the
//     active players, -1 for deactivated ones.
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps,
// and its soft time limit counts its own measures, so a bot answering in ~50 ms may be timed out by
// one and not the other; on a loaded machine the jar times out bots that answered in time: during
// validation 21 of 3600 referee-diff games differed that way, all identical when replayed alone);
// the game's metadata contents (statistics, only their size matters to the engine and it stays far
// below its limit); the viewer's map "making of" snapshots (Grid.clone, never sent anywhere).
// Validated (2026-10-24) with battle referee-diff against the jar on 3600 games (30 bots, every
// ordered pairing, seeds from 1, 1000001, 987654321 and 5000000000) and in direct mode (jar game
// log vs -l trace, every view frame compared) on 163 games, including raw line endings, invalid
// UTF-8, U+2028 and soft time limit bots.

#include "common/engine.h"

#include <deque>
#include <map>

using java::String;

// ----------------------------------------------------------------------------
// Game.java constants
// ----------------------------------------------------------------------------
namespace C {
const int MIN_GRID_HEIGHT = 14;
const int MAX_GRID_HEIGHT = 20;
const float ASPECT_RATIO = 1.5f;
const int MIN_TOWN_DISTANCE = 4;
const int AVERAGE_TILES_PER_ZONE_COEFF_TO_GRID_HEIGHT = 2;
const int AVERAGE_TILES_PER_TOWN = 50;
const float RIVER_SPLIT_PROBA = 0.055f;
const float RIVER_TO_LAND_MIN_RATIO = 0.07f;
const int MIN_MOUNTAINS = 2;
const float MOUNTAIN_TO_CELL_RATIO = 0.04f;
const int MIN_RIVER_LENGTH = 3;
const int BASE_RAIL_COST = 1;
const int PASSIVE_INCOME = 3;
const int GRASS_COST_MULTIPLIER = 1;
const int RIVER_COST_MULTIPLIER = 2;
const int MOUNTAIN_COST_MULTIPLIER = 3;
const int POI_COST_MULTIPLIER = 3;
const int STARTING_DOSH = 0;
const int BLOT_POINTS_PER_TURN = 1;
const int MAX_TURNS = 100;
const int INSTABILITY_THRESHOLD_BASE = 4;
const int INSTABILITY_THRESHOLD_INCREASE = 0;
// Animation (in ms)
const int TENTH = 100;
const int THIRD = 300;
const int HALF = 500;
const int WHOLE = 1000;
} // namespace C

// ----------------------------------------------------------------------------
// grid/Coord.java, Direction.java, Tile.java, Zone.java, Town.java, Grid.java
// ----------------------------------------------------------------------------
struct Coord {
    i32 x = 0, y = 0;
    Coord() {}
    Coord(i32 x_, i32 y_) : x(x_), y(y_) {}
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    // compareTo: x, then y (TreeMap<Coord, ...>)
    bool operator<(const Coord& o) const { return x != o.x ? x < o.x : y < o.y; }
    Coord sub(const Coord& o) const { return Coord(java::sub(x, o.x), java::sub(y, o.y)); }
    i32 hashCode() const { return java::add(java::mul(31, java::add(31, x)), y); }
    i32 manhattanTo(const Coord& o) const { return java::add(java::abs(java::sub(o.x, x)), java::abs(java::sub(o.y, y))); }
    std::string toString() const { return "(" + std::to_string(x) + ", " + std::to_string(y) + ")"; }
};

enum Dir { NORTH, EAST, SOUTH, WEST, UNSET };
static const int DIR_X[5] = {0, 1, 0, -1, 0};
static const int DIR_Y[5] = {-1, 0, 1, 0, 0};

// Direction.fromCoord(c).ordinal()
static int dirFromCoord(const Coord& c) {
    for (int d = 0; d < 4; d++)
        if (c.x == DIR_X[d] && c.y == DIR_Y[d]) return d;
    return UNSET;
}
// Direction.opposite()
static int dirOpposite(int d) {
    static const int OPP[5] = {SOUTH, WEST, NORTH, EAST, UNSET};
    return OPP[d];
}

const int TYPE_GRASS = 0, TYPE_WATER = 1, TYPE_MOUNTAIN = 2, TYPE_POI = 3;
const int TRACK_NONE = -1, TRACK_NEUTRAL = 2, TOWN_NONE = -1;

struct Tile {
    bool valid = true; // false: Tile.NO_TILE
    int type = 0;
    int zoneId = -1;
    int track = TRACK_NONE;
    int townId = TOWN_NONE;
    Coord coord;
    std::vector<std::pair<int, int>> activeConnections; // ScheduleStep(fromTownId, toTownId)

    bool isWater() const { return type == TYPE_WATER; }
    bool isMountain() const { return type == TYPE_MOUNTAIN; }
    bool isPlains() const { return type == TYPE_GRASS; }
    bool isTown() const { return townId != TOWN_NONE; }
    bool isTrack() const { return track != TRACK_NONE; }
    bool isTrackOrTown() const { return isTown() || isTrack(); }
};

static Tile makeNoTile() {
    Tile t;
    t.valid = false;
    t.type = -1;
    t.coord = Coord(-1, -1);
    return t;
}
static Tile NO_TILE = makeNoTile();

struct Zone {
    int id = 0;
    std::vector<Coord> coords;
    std::vector<int> neighbours; // zone ids (a HashSet turned into a list: only used for membership)
    std::vector<int> containedTowns;
    int instability = 0;
    bool inked = false;
};

struct Town {
    int id = 0;
    Coord coord;
    std::vector<int> desiredConnections; // town ids (= indices in Grid::towns once the map is made)
    std::vector<int> activeConnections;
    std::map<int, std::vector<Coord>> paths; // TreeMap<Integer, List<Coord>>
};

struct Grid {
    int width = 0, height = 0;
    std::vector<Tile> cells; // y * width + x (the LinkedHashMap's order)
    std::vector<Town> towns;
    std::vector<Zone> zones;

    void init(int w, int h) {
        width = w;
        height = h;
        cells.assign((size_t)w * h, Tile());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) cells[(size_t)y * w + x].coord = Coord(x, y);
    }
    bool inside(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    int index(const Coord& c) const { return c.y * width + c.x; }
    Tile& get(i32 x, i32 y) { return inside(x, y) ? cells[(size_t)y * width + x] : NO_TILE; }
    Tile& get(const Coord& c) { return get(c.x, c.y); }
    // getNeighbours(pos): the in-grid N, E, S, W neighbours
    int neighbours(const Coord& pos, Coord out[4]) const {
        int n = 0;
        for (int d = 0; d < 4; d++) {
            Coord c(java::add(pos.x, DIR_X[d]), java::add(pos.y, DIR_Y[d]));
            if (inside(c.x, c.y)) out[n++] = c;
        }
        return n;
    }
    bool canTrainPass(const Coord& c) {
        Tile& t = get(c);
        if (!t.valid) return false;
        return t.isTown() || t.track != TRACK_NONE;
    }
};

static Grid grid;

// Game.getRailCost(Tile)
static int getRailCost(const Tile& t) {
    int cost = C::BASE_RAIL_COST;
    if (t.isMountain()) return cost * C::MOUNTAIN_COST_MULTIPLIER;
    if (t.isWater()) return cost * C::RIVER_COST_MULTIPLIER;
    if (t.type == TYPE_POI) return cost * C::POI_COST_MULTIPLIER;
    return cost * C::GRASS_COST_MULTIPLIER;
}
static int getRailCost(const Coord& c) { return getRailCost(grid.get(c)); }

// ----------------------------------------------------------------------------
// grid/GridMaker.java
// ----------------------------------------------------------------------------
namespace GridMaker {

struct River {
    Coord current; // equal to the last item of history
    std::vector<Coord> history;
    int preferredDirection;
    River(const Coord& c, const std::vector<Coord>& hist, int dir) : current(c), history(hist), preferredDirection(dir) { history.push_back(c); }
    bool isStart() const { return history.size() == 1; }
};

static java::SHA1PRNG* rng;
static int h, w;
static std::vector<Coord> freeBorders; // LinkedLists
static std::vector<Coord> freeCoords;

static bool isCorner(int x, int y) { return (x == 0 && y == 0) || (x == 0 && y == h - 1) || (x == w - 1 && y == 0) || (x == w - 1 && y == h - 1); }
static bool isEdge(int x, int y) { return x == 0 || y == 0 || x == w - 1 || y == h - 1; }
static bool isAccessible(const Tile& t) { return !t.isWater() && t.type != TYPE_MOUNTAIN; }

// LinkedList.remove(Object): the first occurrence
static void removeFirst(std::vector<Coord>& list, const Coord& c) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i] == c) {
            list.erase(list.begin() + i);
            return;
        }
}
static bool contains(const std::vector<Coord>& list, const Coord& c) {
    for (const Coord& e : list)
        if (e == c) return true;
    return false;
}
// LinkedList.poll() (the list is not empty)
static Coord poll(std::vector<Coord>& list) {
    Coord c = list.front();
    list.erase(list.begin());
    return c;
}

static bool hasWaterNearby(const Coord& tile, const std::vector<Coord>& riverHistory) {
    size_t from = riverHistory.size() >= 2 ? riverHistory.size() - 2 : 0; // the last 2 are ignored
    static const int ADJ8_X[8] = {0, 1, 0, -1, -1, 1, 1, -1};
    static const int ADJ8_Y[8] = {-1, 0, 1, 0, -1, 1, -1, 1};
    for (int k = 0; k < 8; k++) {
        Coord n(tile.x + ADJ8_X[k], tile.y + ADJ8_Y[k]);
        if (!grid.inside(n.x, n.y)) continue;
        bool ignored = false;
        for (size_t i = from; i < riverHistory.size(); i++)
            if (riverHistory[i] == n) ignored = true;
        if (!ignored && grid.get(n).isWater()) return true;
    }
    return false;
}

// getAvailableNeighbours: the neighbours passing the test, in the iteration order of the HashSet<Coord>
// they are collected in
template <class Pred>
static std::vector<Coord> getAvailableNeighbours(const std::vector<Coord>& coords, Pred checkAccessible) {
    std::vector<char> seen((size_t)w * h, 0);
    std::vector<Coord> added;
    std::vector<i32> hashes;
    Coord ns[4];
    for (const Coord& coord : coords) {
        int n = grid.neighbours(coord, ns);
        for (int k = 0; k < n; k++) {
            if (!checkAccessible(grid.get(ns[k]))) continue;
            int i = grid.index(ns[k]);
            if (seen[i]) continue;
            seen[i] = 1;
            added.push_back(ns[k]);
            hashes.push_back(ns[k].hashCode());
        }
    }
    std::vector<Coord> r;
    for (int k : java::hashMapOrder(hashes)) r.push_back(added[k]);
    return r;
}

static void init(java::SHA1PRNG& rnd) {
    rng = &rnd;
    h = rng->nextInt(C::MIN_GRID_HEIGHT, C::MAX_GRID_HEIGHT + 1);
    w = java::roundFloat((float)h * C::ASPECT_RATIO);
}

static void initializeGrid() {
    grid = Grid();
    grid.init(w, h);
    freeBorders.clear();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            grid.get(x, y).type = TYPE_GRASS;
            if (isEdge(x, y) && !isCorner(x, y)) freeBorders.push_back(Coord(x, y));
        }
    freeCoords.clear();
    for (const Tile& t : grid.cells) freeCoords.push_back(t.coord);
    java::shuffle(freeBorders, *rng);
    java::shuffle(freeCoords, *rng);
}

static void makeMountains() {
    int nMountains = std::max(C::MIN_MOUNTAINS, java::roundFloat(rng->nextFloat((float)(w * h) * C::MOUNTAIN_TO_CELL_RATIO)));
    if (w * h < 10) nMountains = 0;
    for (int i = 0; i < nMountains; ++i) {
        int mountainSize = rng->nextInt(2, 8);
        if (freeCoords.empty()) return;
        Coord mountainBase = poll(freeCoords);
        std::vector<Coord> mountainCoords;
        grid.get(mountainBase).type = TYPE_MOUNTAIN;
        mountainCoords.push_back(mountainBase);
        removeFirst(freeBorders, mountainBase);
        for (int j = 0; j < mountainSize; j++) {
            std::vector<Coord> neighs = getAvailableNeighbours(mountainCoords, isAccessible);
            if (neighs.empty()) break;
            Coord newMountain = neighs[rng->nextInt((i32)neighs.size())];
            grid.get(newMountain).type = TYPE_MOUNTAIN;
            mountainCoords.push_back(newMountain);
            removeFirst(freeBorders, newMountain);
            removeFirst(freeCoords, newMountain);
        }
    }
}

static void createWater(std::deque<River>& riversToExpand, const River& riverToAdd) {
    riversToExpand.push_back(riverToAdd);
    grid.get(riverToAdd.current).type = TYPE_WATER;
    removeFirst(freeCoords, riverToAdd.current);
}

static std::vector<Coord> getAvailableNeighboursForRiverToFlow(const River& river) {
    std::vector<Coord> r;
    Coord ns[4];
    int n = grid.neighbours(river.current, ns);
    for (int k = 0; k < n; k++) {
        const Coord& c = ns[k];
        if (!hasWaterNearby(c, river.history) && !grid.get(c).isWater() && (!river.isStart() || !isEdge(c.x, c.y)) && isAccessible(grid.get(c)))
            r.push_back(c);
    }
    return r;
}

static int getDirectionFromRiverStart(const Coord& s) {
    return s.x == 0 ? EAST : (s.y == 0 ? SOUTH : (s.x == w - 1 ? WEST : (s.y == h - 1 ? NORTH : UNSET)));
}

static float getWeight(const Coord& neig, const Coord& current, int preferredDirection) {
    Coord d = neig.sub(current);
    if (d.x == DIR_X[preferredDirection] && d.y == DIR_Y[preferredDirection]) return 1.75f;
    int opp = dirOpposite(preferredDirection);
    if (d.x == DIR_X[opp] && d.y == DIR_Y[opp]) return 0.25f;
    return 1.0f;
}

// getRandomCoord over the LinkedHashMap<Coord, Float> of createRiverFlowWeights
static Coord getRandomCoord(const std::vector<std::pair<Coord, float>>& weights) {
    float totalWeight = 0.0f;
    for (auto& e : weights) totalWeight += e.second;
    float rand = rng->nextFloat() * totalWeight;
    float cumulative = 0.0f;
    for (auto& e : weights) {
        cumulative += e.second;
        if (rand < cumulative) return e.first;
    }
    return weights.front().first; // fallback
}

static void deleteShortRivers(const std::vector<River>& generatedRivers) {
    for (const River& river : generatedRivers) {
        if ((int)river.history.size() < C::MIN_RIVER_LENGTH) {
            for (const Coord& coord : river.history) {
                grid.get(coord).type = TYPE_GRASS;
                if (!contains(freeCoords, coord)) freeCoords.push_back(coord);
            }
        }
    }
}

static void makeRivers() {
    int nRiverCells = java::roundFloat((float)(w * h) * C::RIVER_TO_LAND_MIN_RATIO);
    std::deque<Coord> availableRiverSources(freeBorders.begin(), freeBorders.end());
    if (rng->nextBoolean()) availableRiverSources.push_front(Coord(w / 2, h / 2));
    else availableRiverSources.push_front(Coord(w / 2, h / 2 + 1));
    bool initialRiver = true;
    std::vector<River> generatedRivers;

    while (nRiverCells > 0 && !availableRiverSources.empty()) {
        Coord riverStart = availableRiverSources.front();
        availableRiverSources.pop_front();
        removeFirst(freeBorders, riverStart);
        int direction = getDirectionFromRiverStart(riverStart);
        if (hasWaterNearby(riverStart, std::vector<Coord>())) continue;
        std::deque<River> riversToExpand;
        createWater(riversToExpand, River(riverStart, std::vector<Coord>(), direction));
        nRiverCells--;

        while (!riversToExpand.empty()) {
            River river = riversToExpand.front();
            riversToExpand.pop_front();
            Coord current = river.current;
            if (isEdge(current.x, current.y) && !river.isStart()) {
                generatedRivers.push_back(river);
                continue;
            }
            std::vector<Coord> neighs = getAvailableNeighboursForRiverToFlow(river);
            if (neighs.empty()) {
                generatedRivers.push_back(river);
                continue;
            }
            bool goingToSplit = nRiverCells > 0 && neighs.size() >= 2 && rng->nextFloat() <= C::RIVER_SPLIT_PROBA;
            goingToSplit |= initialRiver;
            initialRiver = false;

            std::vector<std::pair<Coord, float>> weights;
            for (const Coord& n : neighs) weights.push_back({n, getWeight(n, river.current, river.preferredDirection)});
            Coord nextCoord = getRandomCoord(weights);
            River nextRiver(nextCoord, river.history, goingToSplit ? dirFromCoord(nextCoord.sub(river.current)) : river.preferredDirection);
            createWater(riversToExpand, nextRiver);
            nRiverCells--;
            if (!goingToSplit) continue;

            std::vector<Coord> remainingNeighs;
            for (const Coord& n : neighs)
                if (n != nextCoord) remainingNeighs.push_back(n);
            java::shuffle(remainingNeighs, *rng);
            if (remainingNeighs.empty()) continue;
            Coord splitCoord = remainingNeighs[0];
            createWater(riversToExpand, River(splitCoord, river.history, dirFromCoord(splitCoord.sub(river.current))));
            nRiverCells--;
        }
    }
    deleteShortRivers(generatedRivers);
}

static void makeZones(int nZones) {
    int cols = (int)std::ceil(std::sqrt((double)nZones));
    int rows = (int)std::ceil((double)nZones / cols);
    double cellH = (double)h / rows;
    std::vector<Zone>& zones = grid.zones;
    zones.clear();
    for (int i = 0; i < nZones; i++) {
        int row = i / cols;
        int col = i % cols;
        double x, y;
        if (row == rows - 1 && nZones % cols != 0) {
            int lastRowPoints = nZones % cols;
            double cellW = (double)w / lastRowPoints;
            x = (col + 0.5) * cellW;
        } else {
            double cellW = (double)w / cols;
            x = (col + 0.5) * cellW;
        }
        y = (row + 0.5) * cellH;
        Coord zoneCenter(java::d2i(x), java::d2i(y));
        Zone z;
        z.id = i;
        z.coords.push_back(zoneCenter);
        grid.get(zoneCenter).zoneId = i;
        zones.push_back(z);
    }

    int zoneId = 0;
    std::vector<char> blocked(nZones, 0);
    int blockedCount = 0;
    while (true) {
        if (!blocked[zoneId]) {
            Zone& zone = zones[zoneId];
            std::vector<Coord> neighs = getAvailableNeighbours(zone.coords, [](const Tile& t) { return t.zoneId == -1; });
            if (neighs.empty()) {
                blocked[zoneId] = 1;
                if (++blockedCount == nZones) break;
            } else {
                Coord neighToAdd = neighs[rng->nextInt((i32)neighs.size())];
                grid.get(neighToAdd).zoneId = zoneId;
                zone.coords.push_back(neighToAdd);
            }
        }
        zoneId = (zoneId + 1) % nZones;
    }

    // Cache neighbours
    Coord ns[4];
    for (Zone& zone : zones) {
        std::vector<int> all;
        for (const Coord& coord : zone.coords) {
            int n = grid.neighbours(coord, ns);
            for (int k = 0; k < n; k++) {
                int otherId = grid.get(ns[k]).zoneId;
                if (otherId != zone.id && std::find(all.begin(), all.end(), otherId) == all.end()) all.push_back(otherId);
            }
        }
        zone.neighbours = all;
    }
}

static std::vector<Town> makeTowns(int nTowns, int averageTilesPerZone) {
    std::vector<Zone>& zones = grid.zones;
    std::deque<int> availableZones; // zone indices (LinkedList<Zone>)
    for (int i = 0; i < (int)zones.size(); i++) availableZones.push_back(i);
    std::vector<char> blacklist(zones.size(), 0);
    std::vector<Town> towns;

    auto farFromTowns = [&](const Coord& c) {
        for (const Town& t : towns)
            if (c.manhattanTo(t.coord) < C::MIN_TOWN_DISTANCE) return false;
        return true;
    };

    int step = std::max(1, C::AVERAGE_TILES_PER_TOWN / averageTilesPerZone);
    int retries = 100;
    if (availableZones.empty()) return towns;
    for (int i = 0; i < nTowns; i++) {
        int index = rng->nextInt(i * step, (i + 1) * step);
        int zone = availableZones[(size_t)(index % (int)availableZones.size())];
        bool found = false;
        if (!blacklist[zone]) {
            std::vector<Coord> townCoords = zones[zone].coords;
            java::shuffle(townCoords, *rng);
            for (const Coord& townCoord : townCoords) {
                if (isAccessible(grid.get(townCoord)) && !isEdge(townCoord.x, townCoord.y) && farFromTowns(townCoord)) {
                    Town t;
                    t.id = i;
                    t.coord = townCoord;
                    towns.push_back(t);
                    found = true;
                    blacklist[zone] = 1;
                    for (int zid : zones[zone].neighbours) blacklist[zid] = 1;
                    break;
                }
            }
        }
        if (!found && retries > 0) {
            i--;
            retries--;
        }
    }

    int townsLeftToPlace = nTowns - (int)towns.size();
    std::deque<int> remaining;
    for (int z : availableZones)
        if (!blacklist[z]) remaining.push_back(z);
    std::vector<int> shuffled(remaining.begin(), remaining.end());
    java::shuffle(shuffled, *rng);
    availableZones.assign(shuffled.begin(), shuffled.end());

    for (int i = 0; i < townsLeftToPlace; ++i) {
        if (availableZones.empty()) break;
        int z = availableZones.front();
        availableZones.pop_front();
        std::vector<Coord> townCoords = zones[z].coords;
        java::shuffle(townCoords, *rng);
        for (const Coord& townCoord : townCoords) {
            if (farFromTowns(townCoord)) {
                Town t;
                t.id = i;
                t.coord = townCoord;
                towns.push_back(t);
                break;
            }
        }
    }

    int idx = 0;
    for (Town& town : towns) town.id = idx++;
    for (Town& town : towns) {
        Tile& tile = grid.get(town.coord);
        zones[tile.zoneId].containedTowns.push_back(town.id);
        tile.townId = town.id;
        tile.type = TYPE_GRASS;
    }
    return towns;
}

static void makeTownConnections(std::vector<Town>& towns) {
    for (Town& t : towns) {
        std::vector<int> otherTowns;
        for (const Town& ot : towns)
            if (ot.id != t.id) otherTowns.push_back(ot.id);
        java::shuffle(otherTowns, *rng);
        int atLeast = std::min(3, (int)otherTowns.size());
        int atMost = std::max(atLeast, (int)otherTowns.size() - 4);
        int n = rng->nextInt(atLeast, atMost + 1);
        t.desiredConnections.assign(otherTowns.begin(), otherTowns.begin() + n);
        std::sort(t.desiredConnections.begin(), t.desiredConnections.end()); // distinct ids
    }
    // Remove reciprocal connections
    for (Town& t : towns) {
        std::vector<int> kept;
        for (int ot : t.desiredConnections) {
            const std::vector<int>& d = towns[ot].desiredConnections;
            if (std::find(d.begin(), d.end(), t.id) == d.end()) kept.push_back(ot);
        }
        t.desiredConnections = kept;
    }
}

static void make() {
    initializeGrid();
    makeMountains();
    makeRivers();
    int averageTilesPerZone = h / C::AVERAGE_TILES_PER_ZONE_COEFF_TO_GRID_HEIGHT;
    int nZones = std::max(1, (h * w) / averageTilesPerZone);
    makeZones(nZones);
    int nTowns = std::max(4, (h * w) / C::AVERAGE_TILES_PER_TOWN);
    std::vector<Town> towns = makeTowns(nTowns, averageTilesPerZone);
    makeTownConnections(towns);
    grid.towns = towns;
}

} // namespace GridMaker

// ----------------------------------------------------------------------------
// action/ActionType.java, Action.java, Player.java
// ----------------------------------------------------------------------------
enum ActionType { AUTOPLACE, PLACE_TRACK, DISRUPT, DISRUPT_ALT, MESSAGE, WAIT };

struct Action {
    int type = WAIT;
    Coord coord;
    i32 zoneId = 0;
    Coord from, to;
    bool generatedByAutobuild = false;
    String message;
};

struct Player : cg::Player {
    std::vector<Action> intents;
    int dosh = 0;
    int blotPoints = 0;
    bool hasMessage = false; // message != null
    String message;

    int getExpectedOutputLines() const override { return 1; }
    void init() {
        intents.clear();
        dosh = C::STARTING_DOSH;
    }
    void reset() {
        intents.clear();
        hasMessage = false;
        message.clear();
    }
    std::string nick() const { return "$" + std::to_string(index); }
};

static Player players[2];

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

// ----------------------------------------------------------------------------
// event/Animation.java, EventData.java
// ----------------------------------------------------------------------------
namespace Ev {
const int BUILD = 1, DISRUPT = 2, EARN_POINTS = 3, INK = 5, CONNECTION_LOST = 6, CONNECTION_GAINED = 7, BUMP = 8;
}

struct EventData {
    int type = 0;
    int start = 0, end = 0;
    std::vector<int> params;
};

struct Animation {
    int frameTime = 0;
    int endTime = 0;
    std::vector<EventData> viewerEvents;

    void reset() {
        viewerEvents.clear();
        frameTime = 0;
        endTime = 0;
    }
    void wait(int time) { frameTime += time; }
    int getFrameTime() const { return frameTime; }
    void startAnim(EventData e, int duration) {
        e.start = frameTime;
        e.end = frameTime + duration;
        endTime = std::max(endTime, frameTime + duration);
        viewerEvents.push_back(e);
    }
    void setFrameTime(int t) { frameTime = t; }
    void catchUp() { frameTime = endTime; }
};

static Animation animation;

static EventData event(int type, std::vector<int> params) {
    EventData e;
    e.type = type;
    e.params = std::move(params);
    return e;
}

// ----------------------------------------------------------------------------
// grid/pathfinding: TrainBFS, AutobuildAStar (AbstractAStar), TerrainAStar
// ----------------------------------------------------------------------------
namespace Path {

// TrainBFS.search(): the path from `from` to `to` over towns and tracks (empty if none). The
// neighbours are "sorted" with a comparator that always returns 4 (two neighbours of a cell are never
// one step apart): TimSort leaves them in N, E, S, W order.
static std::vector<Coord> trainBFS(const Coord& from, const Coord& to) {
    int n = grid.width * grid.height;
    std::vector<int> prev(n, -2); // -2: not visited, -1: the start
    std::vector<Coord> fifo;
    fifo.push_back(from);
    prev[grid.index(from)] = -1;
    Coord ns[4];
    for (size_t head = 0; head < fifo.size(); head++) {
        Coord current = fifo[head];
        if (current == to) {
            std::vector<Coord> path;
            for (int i = grid.index(current); i != -1; i = prev[i]) path.push_back(grid.cells[i].coord);
            std::reverse(path.begin(), path.end());
            return path;
        }
        int k = grid.neighbours(current, ns);
        for (int j = 0; j < k; j++) {
            if (grid.canTrainPass(ns[j]) && prev[grid.index(ns[j])] == -2) {
                prev[grid.index(ns[j])] = grid.index(current);
                fifo.push_back(ns[j]);
            }
        }
    }
    return std::vector<Coord>();
}

// TerrainAStar(from, to).search().isPresent(): an A* explores every reachable cell before giving up,
// so it finds a path exactly when a BFS over the cells of regions that are not inked does
static bool terrainPathExists(const Coord& from, const Coord& to) {
    int n = grid.width * grid.height;
    std::vector<char> seen(n, 0);
    std::vector<Coord> fifo;
    fifo.push_back(from);
    seen[grid.index(from)] = 1;
    Coord ns[4];
    for (size_t head = 0; head < fifo.size(); head++) {
        Coord current = fifo[head];
        if (current == to) return true;
        int k = grid.neighbours(current, ns);
        for (int j = 0; j < k; j++) {
            int i = grid.index(ns[j]);
            if (seen[i] || grid.zones[grid.get(ns[j]).zoneId].inked) continue;
            seen[i] = 1;
            fifo.push_back(ns[j]);
        }
    }
    return false;
}

// AutobuildAStar
enum CursorState { UNBOUGHT, BUILDABLE, BUILT, BROKEN };

struct AState {
    bool hasAction = false; // action = PLACE_TRACK at cursor (null otherwise)
    int tracks = 0;         // the Set<Coord> of planned tracks (index in trackSets, shared by moves)
    int moneySpent = 0;
    Coord cursor;
    int cursorState = BUILT;
};

struct AutobuildAStar {
    Coord from, to;
    int cells;
    std::vector<AState> states;
    std::vector<std::vector<char>> trackSets;

    AutobuildAStar(const Coord& f, const Coord& t) : from(f), to(t), cells(grid.width * grid.height) {}

    bool hasTrack(const AState& s, const Coord& c) const { return grid.inside(c.x, c.y) && trackSets[s.tracks][grid.index(c)]; }

    // the key of the LinkedHashMaps: (cursor, cursorState); a BROKEN cursor is outside the grid
    int key(const AState& s) const { return s.cursorState == BROKEN ? 4 * cells : s.cursorState * cells + grid.index(s.cursor); }

    AState initialState() {
        AState s;
        s.hasAction = false;
        s.moneySpent = 0;
        trackSets.push_back(std::vector<char>(cells, 0));
        s.tracks = 0;
        s.cursor = from;
        Tile& t = grid.get(from);
        if (!t.valid) {
            s.cursorState = BROKEN;
            return s;
        }
        s.cursorState = t.isTrackOrTown() ? BUILT : BUILDABLE;
        return s;
    }

    bool isPartOfRailBlock(const Coord& coord, const Coord& goal) {
        std::vector<char> visited(cells, 0);
        std::vector<Coord> fifo;
        fifo.push_back(coord);
        visited[grid.index(coord)] = 1;
        Coord ns[4];
        for (size_t head = 0; head < fifo.size(); head++) {
            Coord current = fifo[head];
            if (current == goal) return true;
            int k = grid.neighbours(current, ns);
            for (int j = 0; j < k; j++) {
                int i = grid.index(ns[j]);
                if (!visited[i] && grid.get(ns[j]).isTrackOrTown()) {
                    fifo.push_back(ns[j]);
                    visited[i] = 1;
                }
            }
        }
        return false;
    }

    bool isGoal(const AState& s) {
        if (s.cursor == to) return true;
        Tile& t = grid.get(s.cursor);
        if (t.isTrackOrTown()) return isPartOfRailBlock(s.cursor, to);
        return false;
    }

    std::vector<Coord> getAllNeighboursOfRailBlock(const Coord& cursor) {
        std::vector<Coord> neighs;
        std::vector<char> visited(cells, 0);
        std::vector<Coord> fifo;
        fifo.push_back(cursor);
        visited[grid.index(cursor)] = 1;
        Coord ns[4];
        for (size_t head = 0; head < fifo.size(); head++) {
            Coord current = fifo[head];
            if (!grid.get(current).isTrackOrTown()) {
                neighs.push_back(current);
                continue; // not part of the rail block
            }
            int k = grid.neighbours(current, ns);
            for (int j = 0; j < k; j++) {
                int i = grid.index(ns[j]);
                if (!visited[i]) {
                    fifo.push_back(ns[j]);
                    visited[i] = 1;
                }
            }
        }
        return neighs;
    }

    // getSuccessors: indices of new states
    std::vector<int> getSuccessors(int si) {
        std::vector<int> result;
        if (states[si].cursorState == BROKEN) return result;
        if (!grid.get(to).valid) return result;
        AState state = states[si];
        std::vector<Coord> scope;
        Tile& currentTile = grid.get(state.cursor);
        bool usableTrack = hasTrack(state, state.cursor) || currentTile.isTrackOrTown();
        if (usableTrack) {
            if (hasTrack(state, state.cursor)) {
                Coord ns[4];
                int k = grid.neighbours(state.cursor, ns);
                scope.assign(ns, ns + k);
            } else {
                scope = getAllNeighboursOfRailBlock(state.cursor);
            }
        } else {
            scope.push_back(state.cursor);
        }
        std::vector<int> keys; // the LinkedHashSet<AutobuildState> (all BUILT: keyed by cursor)
        for (const Coord& neigh : scope) {
            Tile& t = grid.get(neigh);
            bool isTown = t.townId != TOWN_NONE;
            bool buildableZone = !grid.zones[t.zoneId].inked;
            if (!(buildableZone || isTown)) continue; // ownedZones is always empty
            if (hasTrack(state, neigh)) continue;
            AState next;
            next.cursor = neigh;
            next.cursorState = BUILT;
            if (t.isTrackOrTown()) {
                if (neigh == state.cursor) continue;
                next.hasAction = false;
                next.moneySpent = state.moneySpent;
                next.tracks = state.tracks;
            } else {
                next.hasAction = true;
                next.moneySpent = state.moneySpent + getRailCost(t);
                std::vector<char> tracks = trackSets[state.tracks];
                tracks[grid.index(neigh)] = 1;
                trackSets.push_back(std::move(tracks));
                next.tracks = (int)trackSets.size() - 1;
            }
            int k = key(next);
            if (std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
            keys.push_back(k);
            states.push_back(next);
            result.push_back((int)states.size() - 1);
        }
        return result;
    }

    struct Node {
        int state;
        double f;
        int tieBreaker;
    };
    struct NodeCmp {
        int operator()(const Node& a, const Node& b) const {
            int c = java::compareDouble(a.f, b.f);
            return c != 0 ? c : java::compareInt(a.tieBreaker, b.tieBreaker);
        }
    };

    // search().map(path -> the actions of its states)
    std::vector<Coord> search() {
        java::PriorityQueue<Node, NodeCmp> open{NodeCmp()};
        std::vector<double> gScores(4 * cells + 1, INFINITY);
        std::vector<int> cameFrom(4 * cells + 1, -1);
        states.push_back(initialState());
        gScores[key(states[0])] = 0.0;
        open.offer(Node{0, 0.0, 0}); // heuristic: cursor.manhattanTo(cursor) = 0
        while (!open.isEmpty()) {
            Node current = open.poll();
            if (isGoal(states[current.state])) {
                std::vector<int> path;
                for (int s = current.state; s != -1; s = cameFrom[key(states[s])]) path.push_back(s);
                std::reverse(path.begin(), path.end());
                std::vector<Coord> actions;
                for (int s : path)
                    if (states[s].hasAction) actions.push_back(states[s].cursor);
                return actions;
            }
            std::vector<int> succ = getSuccessors(current.state);
            for (int nb : succ) {
                const AState& cur = states[current.state];
                const AState& next = states[nb];
                double tentativeG = gScores[key(cur)] + (double)(next.moneySpent - cur.moneySpent);
                if (tentativeG < gScores[key(next)]) {
                    cameFrom[key(next)] = current.state;
                    gScores[key(next)] = tentativeG;
                    int tieBreakerValue = dirFromCoord(next.cursor.sub(cur.cursor));
                    open.offer(Node{nb, tentativeG + 0.0, tieBreakerValue});
                }
            }
        }
        return std::vector<Coord>();
    }
};

} // namespace Path

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
namespace Game {

static int turn = 0;
static int instabilityThreshold = C::INSTABILITY_THRESHOLD_BASE;
static std::map<int, int> succesfulBlotsThisTurn; // TreeMap, never cleared (sic)

// statistics (metadata)
static int placedTracks[2], zonesInked[2], extraTilesInConnection[2], autobuildCalled[2], tracksPlacedOnPlains[2], tracksPlacedOnRiver[2],
    tracksPlacedOnMountains[2], ownTracksInkedOut[2], enemyTracksInkedOut[2], trackOwnershipPercentagePerActiveConnectionTotal[2];
static float trackOwnershipPercentagePerActiveConnection[2];
static i64 executionTimeMs[2];

static std::string endScreenJson; // EndScreenModule data, set by onEnd

static void init() {
    succesfulBlotsThisTurn.clear();
    for (Player& p : players) p.init();
    GridMaker::init(cg::gm.getRandom());
    GridMaker::make();
    // tutorialManager.initTutorial(): league 19 is not a tutorial league
    turn = 0;
    instabilityThreshold = C::INSTABILITY_THRESHOLD_BASE;
}

static void resetGameTurnData() {
    for (Player& p : players) p.reset();
    animation.reset();
}

static void reportPlayerError(Player& player, const std::string& message) {
    cg::gm.addToGameSummary(cg::GameManager::formatErrorMessage(java::ascii(player.nick() + " " + message)));
}

static void doIncome() {
    for (Player& player : players) {
        player.dosh = C::PASSIVE_INCOME;
        player.blotPoints = C::BLOT_POINTS_PER_TURN;
    }
}

static void computeAutobuilds() {
    for (Player& player : players) {
        bool autoBuildUsed = false;
        std::vector<Action> resolvedIntents;
        for (const Action& intent : player.intents) {
            if (intent.type == AUTOPLACE) {
                if (autoBuildUsed) {
                    reportPlayerError(player, "Only one autobuild action allowed per turn.");
                    continue;
                }
                Path::AutobuildAStar planner(intent.from, intent.to);
                for (const Coord& c : planner.search()) {
                    Action a;
                    a.type = PLACE_TRACK;
                    a.coord = c;
                    a.generatedByAutobuild = true;
                    resolvedIntents.push_back(a);
                }
                autoBuildUsed = true;
                autobuildCalled[player.index]++;
            } else {
                resolvedIntents.push_back(intent);
            }
        }
        player.intents = resolvedIntents;
    }
}

static void doActions() {
    std::map<Coord, std::vector<int>> tracksPlaced; // TreeMap<Coord, List<Integer>>
    std::vector<Coord> placementOrder;

    for (Player& player : players) {
        bool interruptAutobuild = false;
        for (const Action& action : player.intents) {
            if (interruptAutobuild && action.generatedByAutobuild) continue;
            if (action.type != PLACE_TRACK) continue;
            const Coord& coord = action.coord;
            Tile& tile = grid.get(coord);
            if (!tile.valid) {
                reportPlayerError(player, "Not part of grid: " + coord.toString() + "");
                continue;
            }
            if (tile.isTown()) {
                reportPlayerError(player, "Cannot place tracks on a town at " + coord.toString());
                continue;
            }
            Zone& zone = grid.zones[tile.zoneId];
            if (zone.inked) { // canPlaceTrackInZone
                reportPlayerError(player, "Cannot build in region " + std::to_string(zone.id));
                continue;
            }
            auto it = tracksPlaced.find(coord);
            bool placedByPlayer = it != tracksPlaced.end() && std::find(it->second.begin(), it->second.end(), player.index) != it->second.end();
            if (!(tile.track == TRACK_NONE && !placedByPlayer)) { // isFreeOfTracks
                reportPlayerError(player, "Cannot place tracks on existing tracks at " + coord.toString());
                continue;
            }
            int railCost = getRailCost(tile);
            if (player.dosh < railCost) {
                if (action.generatedByAutobuild) {
                    reportPlayerError(player, "Autobuild interrupted: not enough track points to build a track at " + coord.toString() + ".");
                    interruptAutobuild = true;
                } else {
                    reportPlayerError(player, "Not enough track points to build a track at " + coord.toString() + ".");
                }
                continue;
            }
            tracksPlaced[coord].push_back(player.index);
            for (size_t i = 0; i < placementOrder.size(); i++)
                if (placementOrder[i] == coord) {
                    placementOrder.erase(placementOrder.begin() + i);
                    break;
                }
            placementOrder.push_back(coord);
            player.dosh -= railCost;
            placedTracks[player.index]++;
            if (tile.isMountain()) tracksPlacedOnMountains[player.index]++;
            else if (tile.isWater()) tracksPlacedOnRiver[player.index]++;
            else if (tile.isPlains()) tracksPlacedOnPlains[player.index]++;
        }
    }

    int animTimes[2] = {animation.getFrameTime(), animation.getFrameTime()};
    for (const Coord& coord : placementOrder) {
        const std::vector<int>& playerIdxs = tracksPlaced[coord];
        Tile& tile = grid.get(coord);
        if (playerIdxs.size() == 1) {
            int playerIndex = playerIdxs[0];
            tile.track = playerIndex;
            animation.setFrameTime(animTimes[playerIndex]);
            animation.startAnim(event(Ev::BUILD, {playerIndex, coord.x, coord.y, getRailCost(coord)}), C::HALF);
            animation.wait(C::THIRD);
            animTimes[playerIndex] = animation.getFrameTime();
        } else {
            tile.track = TRACK_NEUTRAL;
            animation.setFrameTime(std::max(animTimes[0], animTimes[1]));
            animation.startAnim(event(Ev::BUILD, {TRACK_NEUTRAL, coord.x, coord.y, getRailCost(coord)}), C::HALF);
            animation.wait(C::THIRD);
            animTimes[0] = animation.getFrameTime();
            animTimes[1] = animation.getFrameTime();
        }
    }
    animation.catchUp();

    // Disruptions
    for (Player& player : players) {
        for (Action& action : player.intents) {
            if (action.type == DISRUPT_ALT) {
                Tile& tile = grid.get(action.coord);
                if (!tile.valid) {
                    reportPlayerError(player, "Not part of grid: " + action.coord.toString() + "");
                    continue;
                }
                action.zoneId = tile.zoneId;
                action.type = DISRUPT;
            }
            if (action.type == DISRUPT) {
                if (player.blotPoints <= 0) {
                    reportPlayerError(player, "Not enough disruption points.");
                    continue;
                }
                if (action.zoneId < 0 || action.zoneId >= (i32)grid.zones.size()) {
                    reportPlayerError(player, "Invalid region id: " + std::to_string(action.zoneId));
                    continue;
                }
                Zone& zone = grid.zones[action.zoneId];
                if (zone.inked) {
                    reportPlayerError(player, "Cannot disrupt region" + std::to_string(zone.id) + ". Already inked out.");
                    continue;
                }
                if (!zone.containedTowns.empty()) {
                    reportPlayerError(player, "Cannot disrupt region" + std::to_string(zone.id) + ". It contains a town.");
                    continue;
                }
                player.blotPoints--;
                zone.instability++;
                animation.startAnim(event(Ev::DISRUPT, {player.index, zone.id}), C::HALF + C::TENTH);
                succesfulBlotsThisTurn[player.index] = zone.id;
            }
        }
    }
    animation.catchUp();
}

static void doInstabilityCheck() {
    std::vector<int> toInk;
    for (Zone& zone : grid.zones) {
        if (zone.inked) continue;
        if (!zone.containedTowns.empty()) continue;
        if (zone.instability >= instabilityThreshold) toInk.push_back(zone.id);
    }
    for (int zi : toInk) {
        Zone& zone = grid.zones[zi];
        instabilityThreshold += C::INSTABILITY_THRESHOLD_INCREASE;
        zone.inked = true;
        std::vector<Coord> coordsWithTrackPiece;
        for (const Coord& c : zone.coords)
            if (grid.get(c).isTrack()) coordsWithTrackPiece.push_back(c);
        int rekt[3] = {0, 0, 0};
        for (const Coord& coord : coordsWithTrackPiece) {
            int trackOwner = grid.get(coord).track;
            if (trackOwner > -1) rekt[trackOwner]++;
            grid.get(coord).track = TRACK_NONE;
        }
        std::vector<int> params;
        params.push_back(zone.id);
        for (const Coord& c : coordsWithTrackPiece) {
            params.push_back(c.x);
            params.push_back(c.y);
        }
        animation.startAnim(event(Ev::INK, params), C::WHOLE);
        // (tutorial: setPlayerOneHasInkedEnemyTrack, unused in league 19)
        for (Player& p : players) {
            auto it = succesfulBlotsThisTurn.find(p.index);
            if ((it == succesfulBlotsThisTurn.end() ? -1 : it->second) == zone.id) {
                zonesInked[p.index]++;
                ownTracksInkedOut[p.index] += rekt[p.index];
                enemyTracksInkedOut[p.index] += rekt[1 - p.index];
            }
        }
    }
    animation.catchUp();
}

static void launchConnectionGainedEvent(const Town& town, const Town& other, const std::vector<Coord>& path, bool skipBumpAnimation) {
    int t = animation.getFrameTime();
    std::vector<int> params = {town.id, other.id};
    for (const Coord& c : path) {
        params.push_back(c.x);
        params.push_back(c.y);
    }
    animation.startAnim(event(Ev::CONNECTION_GAINED, params), C::THIRD);
    if (skipBumpAnimation) return;
    for (const Coord& step : path) {
        if (grid.get(step).isTown()) continue;
        animation.startAnim(event(Ev::BUMP, {step.x, step.y}), C::THIRD);
        animation.wait(C::TENTH);
    }
    animation.setFrameTime(t);
}

static void moveTrains() {
    std::vector<std::pair<int, int>> newConnections; // LinkedHashSet<ScheduleStep>
    for (Town& town : grid.towns) {
        std::vector<int> previousConnections = town.activeConnections;
        std::map<int, std::vector<Coord>> prevTownPaths;
        prevTownPaths.swap(town.paths);
        town.activeConnections.clear();
        for (int otherId : town.desiredConnections) {
            Town& other = grid.towns[otherId];
            std::vector<Coord> path = Path::trainBFS(town.coord, other.coord);
            if (path.empty()) continue;
            town.activeConnections.push_back(otherId);
            town.paths[other.id] = path;
            int pointsPerPlayer[2] = {0, 0};
            for (const Coord& coord : path) {
                Tile& tile = grid.get(coord);
                for (Player& p : players)
                    if (tile.track == p.index) pointsPerPlayer[p.index]++;
            }
            for (Player& p : players) {
                if (pointsPerPlayer[p.index] == 0) continue;
                int points = pointsPerPlayer[p.index];
                p.setScore(p.getScore() + points);
                animation.startAnim(event(Ev::EARN_POINTS, {p.index, points, town.id, other.id}), C::HALF);
                extraTilesInConnection[p.index] += (int)path.size() - town.coord.manhattanTo(other.coord);
                trackOwnershipPercentagePerActiveConnection[p.index] += (float)points / (float)path.size();
                trackOwnershipPercentagePerActiveConnectionTotal[p.index]++;
            }
            bool wasActive = std::find(previousConnections.begin(), previousConnections.end(), otherId) != previousConnections.end();
            bool pathHasChanged = false;
            if (wasActive) pathHasChanged = prevTownPaths[other.id] != path;
            if (!wasActive || pathHasChanged) {
                if (pathHasChanged) animation.startAnim(event(Ev::CONNECTION_LOST, {town.id, other.id}), C::THIRD);
                std::pair<int, int> ss(town.id, other.id);
                if (std::find(newConnections.begin(), newConnections.end(), ss) == newConnections.end()) newConnections.push_back(ss);
            }
        }
        // Removals
        for (int otherId : previousConnections)
            if (std::find(town.activeConnections.begin(), town.activeConnections.end(), otherId) == town.activeConnections.end())
                animation.startAnim(event(Ev::CONNECTION_LOST, {town.id, otherId}), C::THIRD);
    }
    animation.catchUp();

    int total = 0;
    for (auto& ss : newConnections) total += (int)grid.towns[ss.first].paths[ss.second].size();
    bool skipBumpAnimation = total > 100;
    for (auto& ss : newConnections) {
        Town& from = grid.towns[ss.first];
        Town& to = grid.towns[ss.second];
        launchConnectionGainedEvent(from, to, from.paths[ss.second], skipBumpAnimation);
        animation.catchUp();
    }
}

static void computeTileStates() {
    for (Tile& tile : grid.cells) tile.activeConnections.clear();
    for (Town& t : grid.towns)
        for (auto& kv : t.paths)
            for (const Coord& c : kv.second) grid.get(c).activeConnections.push_back({t.id, kv.first});
}

static bool isAnyConnectionStillPossible() {
    for (Town& town : grid.towns)
        for (int other : town.desiredConnections)
            if (Path::terrainPathExists(town.coord, grid.towns[other].coord)) return true;
    return false;
}

static bool isGameOver() { return !isAnyConnectionStillPossible() || turn >= C::MAX_TURNS; }

static void computeEvents() {
    int minTime = 500;
    animation.catchUp();
    int frameTime = std::max(animation.getFrameTime(), minTime);
    cg::gm.setFrameDuration(frameTime);
}

static void performGameUpdate() {
    turn++;
    for (Player& player : players) executionTimeMs[player.index] += player.getLastExectionTimeMs();
    doIncome();
    computeAutobuilds();
    doActions();
    doInstabilityCheck();
    moveTrains();
    computeTileStates();
    if (isGameOver()) cg::gm.endGame();
    computeEvents();
}

// String.valueOf(double) of the metadata values (ints: "N.0"; the float average only approximately)
static std::string metaInt(i64 v) { return std::to_string(v) + ".0"; }

static void onEnd() {
    std::string scoreTexts[2];
    for (Player& p : players) {
        if (!p.isActive()) {
            p.setScore(-1);
            scoreTexts[p.index] = "-";
        } else {
            scoreTexts[p.index] = std::to_string(p.getScore()) + " point" + (p.getScore() > 1 ? "s" : "");
        }
    }
    // EndScreenModule.setScores(scores, scoreTexts): sent by onAfterOnEnd
    for (Player& p : players) {
        std::string i = std::to_string(p.index);
        cg::gm.putMetadata("tracksPlaced_" + i, metaInt(placedTracks[p.index]));
        cg::gm.putMetadata("tracksPlacedOnPlains_" + i, metaInt(tracksPlacedOnPlains[p.index]));
        cg::gm.putMetadata("tracksPlacedOnRiver_" + i, metaInt(tracksPlacedOnRiver[p.index]));
        cg::gm.putMetadata("tracksPlacedOnMountains_" + i, metaInt(tracksPlacedOnMountains[p.index]));
        cg::gm.putMetadata("zonesInked_" + i, metaInt(zonesInked[p.index]));
        cg::gm.putMetadata("ownTracksInkedOut_" + i, metaInt(ownTracksInkedOut[p.index]));
        cg::gm.putMetadata("enemyTracksInkedOut_" + i, metaInt(enemyTracksInkedOut[p.index]));
        cg::gm.putMetadata("extraTilesInConnection_" + i, metaInt(extraTilesInConnection[p.index]));
        cg::gm.putMetadata("autobuildCalled_" + i, metaInt(autobuildCalled[p.index]));
        int total = trackOwnershipPercentagePerActiveConnectionTotal[p.index];
        double avg = total == 0 ? 0.0 : (double)(trackOwnershipPercentagePerActiveConnection[p.index] / (float)total);
        char buf[32];
        snprintf(buf, sizeof buf, "%.17g", avg);
        cg::gm.putMetadata("averageTrackOwnershipPercentagePerActiveConnection_" + i, total == 0 ? std::string("0.0") : std::string(buf));
        cg::gm.putMetadata("executionTimeMs_" + i, metaInt(executionTimeMs[p.index]));
        cg::gm.putMetadata("poisServed_" + i, "0.0"); // poisConnected is never filled
    }
    cg::gm.putMetadata("totalPois", "0.0"); // the map has no POI
    endScreenJson = "[[" + std::to_string(players[0].getScore()) + "," + std::to_string(players[1].getScore()) + "],\"logo.png\",[" +
                    java::jsonQuote(java::ascii(scoreTexts[0])) + "," + java::jsonQuote(java::ascii(scoreTexts[1])) + "]]";
}

} // namespace Game

// ----------------------------------------------------------------------------
// Serializer.java
// ----------------------------------------------------------------------------
namespace Serializer {

static std::string serializeTowns(const std::vector<int>& towns) {
    if (towns.empty()) return "x";
    std::string s;
    for (size_t i = 0; i < towns.size(); i++) s += (i ? "," : "") + std::to_string(towns[i]);
    return s;
}

static std::string townLine(const Town& t) {
    return std::to_string(t.id) + " " + std::to_string(t.coord.x) + " " + std::to_string(t.coord.y) + " " + serializeTowns(t.desiredConnections);
}

static std::vector<std::string> serializeGlobalInfoFor(const Player& player) {
    std::vector<std::string> lines;
    lines.push_back(std::to_string(player.index));
    lines.push_back(std::to_string(grid.width));
    lines.push_back(std::to_string(grid.height));
    for (int y = 0; y < grid.height; ++y)
        for (int x = 0; x < grid.width; ++x) {
            Tile& tile = grid.get(x, y);
            lines.push_back(std::to_string(tile.zoneId) + " " + std::to_string(tile.type));
        }
    lines.push_back(std::to_string(grid.towns.size()));
    for (const Town& t : grid.towns) lines.push_back(townLine(t));
    return lines;
}

static std::string serializeLiveConnections(const std::vector<std::pair<int, int>>& connections) {
    if (connections.empty()) return "x";
    std::vector<std::string> steps;
    for (auto& ss : connections) steps.push_back(std::to_string(ss.first) + "-" + std::to_string(ss.second));
    std::sort(steps.begin(), steps.end()); // String.compareTo on ASCII
    std::string s;
    for (size_t i = 0; i < steps.size(); i++) s += (i ? "," : "") + steps[i];
    return s;
}

static std::vector<std::string> serializeFrameInfoFor(const Player& player) {
    std::vector<std::string> lines;
    lines.push_back(std::to_string(player.getScore()));
    lines.push_back(std::to_string(players[1 - player.index].getScore()));
    for (int y = 0; y < grid.height; ++y)
        for (int x = 0; x < grid.width; ++x) {
            Tile& tile = grid.get(x, y);
            const Zone& zone = grid.zones[tile.zoneId];
            lines.push_back(std::to_string(tile.track) + " " + std::to_string(zone.instability) + " " + (zone.inked ? "1" : "0") + " " +
                            serializeLiveConnections(tile.activeConnections));
        }
    return lines;
}

// the view's global data (ViewModule)
static String serializeGlobalData() {
    std::string s = std::to_string(grid.width) + "|" + std::to_string(grid.height) + "|" + std::to_string(C::PASSIVE_INCOME);
    for (int y = 0; y < grid.height; ++y)
        for (int x = 0; x < grid.width; ++x) {
            Tile& tile = grid.get(x, y);
            s += "|" + std::to_string(tile.zoneId) + " " + std::to_string(tile.type);
        }
    s += "|" + std::to_string(grid.towns.size());
    for (const Town& t : grid.towns) s += "|" + townLine(t);
    return java::ascii(s);
}

// the view's frame data: the events, then the players' messages ('|' replaced with U+2223)
static String serializeFrameData() {
    std::string s = std::to_string(animation.viewerEvents.size());
    for (const EventData& e : animation.viewerEvents) {
        s += "|" + std::to_string(e.type) + "|" + std::to_string(e.start) + "|" + std::to_string(e.end) + "|";
        for (size_t i = 0; i < e.params.size(); i++) s += (i ? " " : "") + std::to_string(e.params[i]);
    }
    String r = java::ascii(s);
    for (const Player& p : players) {
        r += u"|";
        if (p.hasMessage) r += java::replace(p.message, u"|", u"\u2223");
    }
    return r;
}

} // namespace Serializer

// ----------------------------------------------------------------------------
// CommandManager.java
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

// (?<g>\d+): the digits at `pos` (at least one)
static bool digitsAt(const String& s, size_t& pos, String& group) {
    size_t a = pos;
    while (pos < s.size() && java::isRegexDigit(s[pos])) pos++;
    if (pos == a) return false;
    group = s.substr(a, pos - a);
    return true;
}

// Pattern.matcher(command).matches() for "^<literal>(\d+)( (\d+))*" with `count` groups
static bool matchNumbers(const String& s, const char* lit, int count, String groups[4]) {
    size_t pos = 0;
    if (!literalAt(s, pos, lit)) return false;
    for (int i = 0; i < count; i++) {
        if (i > 0 && !literalAt(s, pos, " ")) return false;
        if (!digitsAt(s, pos, groups[i])) return false;
    }
    return pos == s.size();
}

struct InvalidInput {
    String message;
};

// Integer.parseInt of a group
static i32 parseInt(const String& g) {
    i32 v;
    if (!java::parseInt(g, v)) throw InvalidInput{u"For input string: \"" + g + u"\""};
    return v;
}

// ActionType patterns in their order; true when `command` is one of them
static bool parseAction(const String& command, Action& action) {
    String g[4];
    if (matchNumbers(command, "AUTOPLACE ", 4, g)) {
        action.type = AUTOPLACE;
        i32 fromX = parseInt(g[0]);
        i32 toX = parseInt(g[2]);
        i32 fromY = parseInt(g[1]);
        i32 toY = parseInt(g[3]);
        action.from = Coord(fromX, fromY);
        action.to = Coord(toX, toY);
        return true;
    }
    if (matchNumbers(command, "PLACE_TRACKS ", 2, g)) {
        action.type = PLACE_TRACK;
        i32 x = parseInt(g[0]);
        i32 y = parseInt(g[1]);
        action.coord = Coord(x, y);
        return true;
    }
    if (matchNumbers(command, "DISRUPT ", 1, g)) {
        action.type = DISRUPT;
        action.zoneId = parseInt(g[0]);
        return true;
    }
    if (matchNumbers(command, "DISRUPT ", 2, g)) {
        action.type = DISRUPT_ALT;
        i32 x = parseInt(g[0]);
        i32 y = parseInt(g[1]);
        action.coord = Coord(x, y);
        return true;
    }
    size_t pos = 0;
    if (literalAt(command, pos, "MESSAGE ")) { // (?<message>[^;]*): the command holds no ';'
        action.type = MESSAGE;
        action.message = command.substr(pos);
        return true;
    }
    pos = 0;
    if (literalAt(command, pos, "WAIT") && pos == command.size()) {
        action.type = WAIT;
        return true;
    }
    return false;
}

// Game.getExpected
static String getExpected(const String& command) {
    if (java::startsWith(command, "AUTOPLACE")) return u"AUTOPLACE x1 y1 x2 y2";
    if (java::startsWith(command, "PLACE_TRACK")) return u"PLACE_TRACK x y";
    if (java::startsWith(command, "DISRUPT")) return u"DISRUPT zoneId";
    if (java::startsWith(command, "MESSAGE")) return u"MESSAGE text";
    if (java::startsWith(command, "WAIT")) return u"WAIT";
    return u"AUTOPLACE | PLACE_TRACK | DISRUPT | MESSAGE | WAIT";
}

static void parseCommands(Player& player, const std::vector<String>& lines) {
    // lines.get(0): empty only when a bot line desynchronised the protocol (see common/engine.h)
    if (lines.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
    const String& line = lines[0];
    std::vector<String> commands = java::split(java::trim(line), u';');
    try {
        for (const String& c : commands) {
            String command = java::trim(c);
            Action action;
            if (!parseAction(command, action)) throw InvalidInput{u"Invalid Input: Expected " + getExpected(command) + u" but got '" + command + u"'"};
            if (action.type == MESSAGE) {
                player.hasMessage = true;
                player.message = action.message;
            } else {
                player.intents.push_back(action);
            }
        }
    } catch (const InvalidInput& e) {
        // deactivatePlayer: escapeHTMLEntities, score -1
        player.deactivate(java::replace(java::replace(e.message, u"&lt;", u"<"), u"&gt;", u">"));
        player.setScore(-1);
        cg::gm.addToGameSummary(e.message);
        cg::gm.addToGameSummary(cg::GameManager::formatErrorMessage(java::ascii(player.nick() + ": disqualified!")));
    }
}

} // namespace Commands

// ----------------------------------------------------------------------------
// Referee.java, ViewModule, EndScreenModule
// ----------------------------------------------------------------------------
struct BackTrackKingReferee : cg::Referee {
    void init() override {
        cg::gm.setMaxTurns(400);
        cg::gm.setFirstTurnMaxTime(1000);
        Game::init();
        // sendGlobalInfo
        for (Player* player : getActivePlayers())
            for (const std::string& line : Serializer::serializeGlobalInfoFor(*player)) player->sendInputLine(line);
        cg::gm.setFrameDuration(1000);
        cg::gm.setTurnMaxTime(50);
    }

    void gameTurn(int turn) override {
        Game::resetGameTurnData();
        for (Player* player : getActivePlayers()) {
            for (const std::string& line : Serializer::serializeFrameInfoFor(*player)) player->sendInputLine(line);
            player->execute();
        }
        // handlePlayerCommands
        for (Player* player : getActivePlayers()) {
            if (player->timeout) { // getOutputs() throws TimeoutException
                player->deactivate(u"Timeout!");
                cg::gm.addToGameSummary(player->nick() + " has not provided " + std::to_string(player->getExpectedOutputLines()) + " lines in time");
            } else {
                Commands::parseCommands(*player, player->outputs);
            }
        }
        Game::performGameUpdate();
        (void)turn;
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
    static BackTrackKingReferee referee;
    cg::Options options;
    options.maxPlayers = 2;
    options.engine47 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
