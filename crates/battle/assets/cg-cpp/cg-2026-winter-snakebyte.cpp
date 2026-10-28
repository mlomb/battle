// Winter Challenge 2026 "SnakeByte" - headless C++ referee, port of
// https://github.com/CodinGame/WinterChallenge2026-Exotec (commit 0bec8c0, 2026-09-09, "Merge pull
// request #1", the game code of commit b8493d0 "remove debug map"), CodinGame game engine 4.6.0,
// 2 players, league 19 (the CLI sets the league to 19: the map generator's "legend" skew).
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
//     consumed by the map generator (GridMaker) only: nextDouble, RandomGenerator.nextInt(origin,
//     bound) and Collections.shuffle (of ArrayLists and of a LinkedList, which shuffles the same way).
//     Its float arithmetic (Math.round(height * 1.8f), the wall chances) is reproduced bit for bit.
//     (-seed 0 is ignored by SecureRandom.setSeed: the jar then plays a random map, and so do we.)
//   - Commands: String.split(";") (no trim), at most 30 commands per turn (a 31st makes the referee
//     return at once: the errors of that turn are then never summarised), the CASE_INSENSITIVE
//     regexes (only ASCII letters fold, \d is [0-9]), MOVE messages (cut to 46 chars + "..."), the
//     GameException texts (at most 4 of them + "...and N more errors."), the disqualification of an
//     unknown command, and Integer.valueOf/parseInt overflows (a NumberFormatException the referee
//     does not catch: the game fails, "null" scores).
//   - The rules in their order: moves, eats, beheadings, falls (the fixpoint of grounded birds, then
//     every airborne bird falls one row, until none falls; a bird entirely below the grid dies), the
//     end of the game (no power source left, a player without birds, 200 turns, fewer than 2 active
//     players), the scores (the lengths of the live birds, -1 for a deactivated player; a tie is
//     broken by the lost body parts, which are subtracted from both scores).
//   - Engine 4.6.0 (common/engine.h, cg::Options::engine46): the referee asks the runner for the
//     bots' execution times (MONITORING) and puts them in the view data; the bots get 1000 ms on
//     their first turn and 50 ms after (no extra 50 ms and no soft time limit before engine 4.7).
//     The protocol between the runner and the referee (java.util.Scanner lines, 30000-byte
//     commands), the 1 MB view data quota (bird messages go to the view), the game summary quotas.
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps, so
// a bot answering in ~50 ms may be timed out by one and not the other; on a loaded machine the jar
// times out bots that answered in time), the execution times in the view data (measured, so they
// differ between any two runs of the jar too), the order of the death events of birds that fall out
// of the grid at the same step (a HashSet of Birds with identity hash codes in the jar, view only),
// Math.pow (StrictMath-like intrinsic in the JVM, the C library's pow here: they could only
// disagree by one ulp, which would change the map height only if pow(r, 0.3) * 14 fell within one
// ulp of a half integer).
// Validated (2026-10-24) with battle referee-diff against the jar on 3600 games (30 bots, every ordered
// pairing, seeds from 1, 1000001, 987654321 and 5000000000; 8 games differed under load because the
// jar timed out a bot that had answered, all identical when replayed alone) and in direct mode (jar
// game log vs -l trace, every view frame compared with the execution times masked) on 602 games,
// including raw line endings, invalid UTF-8, U+2028/U+0085 and over-sized summaries.

#include "common/engine.h"

#include <cmath>
#include <deque>
#include <map>

using java::String;

// ----------------------------------------------------------------------------
// grid/Coord.java, Direction.java, Tile.java
// ----------------------------------------------------------------------------
struct Coord {
    i32 x = 0, y = 0;
    Coord() {}
    Coord(i32 x_, i32 y_) : x(x_), y(y_) {}
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    // compareTo: x, then y
    bool operator<(const Coord& o) const { return x != o.x ? x < o.x : y < o.y; }
    Coord add(i32 dx, i32 dy) const { return Coord(java::add(x, dx), java::add(y, dy)); }
    i32 manhattanTo(const Coord& o) const { return java::add(java::abs(java::sub(o.x, x)), java::abs(java::sub(o.y, y))); }
};

// Direction (ordinal order); NONE stands for a null Direction
enum Dir { NORTH, EAST, SOUTH, WEST, UNSET, NONE = -1 };
static const int DIR_X[5] = {0, 1, 0, -1, 0};
static const int DIR_Y[5] = {-1, 0, 1, 0, 0};

// Direction.fromCoord: the first of NORTH, EAST, SOUTH, WEST, UNSET with that coord, else UNSET
static int dirFromCoord(const Coord& c) {
    for (int d = 0; d < 5; d++)
        if (c.x == DIR_X[d] && c.y == DIR_Y[d]) return d;
    return UNSET;
}
// Direction.opposite()
static int dirOpposite(int d) {
    static const int OPP[5] = {SOUTH, WEST, NORTH, EAST, UNSET};
    return OPP[d];
}

const int TYPE_EMPTY = 0, TYPE_WALL = 1, TYPE_NO_TILE = -1; // Tile.NO_TILE has type -1

// Grid.ADJACENCY (N, E, S, W) and ADJACENCY_8
static const Coord ADJACENCY[4] = {Coord(0, -1), Coord(1, 0), Coord(0, 1), Coord(-1, 0)};
static const Coord ADJACENCY_8[8] = {Coord(0, -1), Coord(1, 0), Coord(0, 1), Coord(-1, 0), Coord(-1, -1), Coord(1, 1), Coord(1, -1), Coord(-1, 1)};

// ----------------------------------------------------------------------------
// grid/Grid.java
// ----------------------------------------------------------------------------
struct Grid {
    int width = 0, height = 0;
    std::vector<int> types; // y * width + x (the LinkedHashMap's order)
    std::vector<Coord> spawns;
    std::vector<Coord> apples;

    void init(int w, int h) {
        width = w;
        height = h;
        types.assign((size_t)w * h, TYPE_EMPTY);
        spawns.clear();
        apples.clear();
    }
    bool valid(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    bool valid(const Coord& c) const { return valid(c.x, c.y); }
    // get(c).getType(): NO_TILE outside the grid
    int type(i32 x, i32 y) const { return valid(x, y) ? types[(size_t)y * width + x] : TYPE_NO_TILE; }
    int type(const Coord& c) const { return type(c.x, c.y); }
    // get(c).setType(t): only ever called on cells of the grid (NO_TILE.setType would print "NOPE")
    void setType(const Coord& c, int t) {
        if (valid(c)) types[(size_t)c.y * width + c.x] = t;
    }
    // the cells in the LinkedHashMap's order
    Coord coordAt(int i) const { return Coord(i % width, i / width); }
    int cellCount() const { return width * height; }

    // getNeighbours(pos, adjacency): the neighbours inside the grid, in the adjacency order
    std::vector<Coord> getNeighbours(const Coord& pos, const Coord* adjacency, int n) const {
        std::vector<Coord> r;
        for (int i = 0; i < n; i++) {
            Coord c(java::add(pos.x, adjacency[i].x), java::add(pos.y, adjacency[i].y));
            if (valid(c)) r.push_back(c);
        }
        return r;
    }

    Coord opposite(const Coord& c) const { return Coord(java::sub(java::sub(width, c.x), 1), c.y); } // ySymetry is false

    bool hasApple(const Coord& c) const { return std::find(apples.begin(), apples.end(), c) != apples.end(); }
    bool isSpawn(const Coord& c) const { return std::find(spawns.begin(), spawns.end(), c) != spawns.end(); }

    // detectAirPockets: the connected (N, E, S, W) groups of non-wall cells
    std::vector<std::vector<Coord>> detectAirPockets() const {
        std::vector<std::vector<Coord>> islands;
        std::vector<char> computed(types.size(), 0);
        for (int i = 0; i < cellCount(); i++) {
            Coord p = coordAt(i);
            if (type(p) == TYPE_WALL) {
                computed[i] = 1;
                continue;
            }
            if (computed[i]) continue;
            std::vector<Coord> current;
            std::deque<Coord> fifo;
            fifo.push_back(p);
            computed[i] = 1;
            while (!fifo.empty()) {
                Coord e = fifo.front();
                fifo.pop_front();
                for (const Coord& d : ADJACENCY) {
                    Coord n = e.add(d.x, d.y);
                    if (valid(n) && !computed[(size_t)n.y * width + n.x] && type(n) != TYPE_WALL) {
                        fifo.push_back(n);
                        computed[(size_t)n.y * width + n.x] = 1;
                    }
                }
                current.push_back(e);
            }
            islands.push_back(current);
        }
        return islands;
    }

    // detectSpawnIslands: the connected groups of spawn cells, in the order of their first spawn
    std::vector<std::vector<Coord>> detectSpawnIslands() const {
        std::vector<std::vector<Coord>> islands;
        std::vector<Coord> computed;
        auto isComputed = [&](const Coord& c) { return std::find(computed.begin(), computed.end(), c) != computed.end(); };
        for (const Coord& p : spawns) {
            if (isComputed(p)) continue;
            std::vector<Coord> current;
            std::deque<Coord> fifo;
            fifo.push_back(p);
            computed.push_back(p);
            while (!fifo.empty()) {
                Coord e = fifo.front();
                fifo.pop_front();
                for (const Coord& d : ADJACENCY) {
                    Coord n = e.add(d.x, d.y);
                    if (valid(n) && !isComputed(n) && isSpawn(n)) {
                        fifo.push_back(n);
                        computed.push_back(n);
                    }
                }
                if (std::find(current.begin(), current.end(), e) == current.end()) current.push_back(e); // a HashSet
            }
            islands.push_back(current);
        }
        return islands;
    }

    // detectLowestIsland: the walls connected to the bottom left corner, in BFS order
    std::vector<Coord> detectLowestIsland() const {
        Coord start(0, height - 1);
        if (type(start) != TYPE_WALL) return {};
        std::vector<char> computed(types.size(), 0);
        std::deque<Coord> fifo;
        std::vector<Coord> lowest;
        fifo.push_back(start);
        computed[(size_t)start.y * width + start.x] = 1;
        lowest.push_back(start);
        while (!fifo.empty()) {
            Coord e = fifo.front();
            fifo.pop_front();
            for (const Coord& d : ADJACENCY) {
                Coord n = e.add(d.x, d.y);
                if (valid(n) && !computed[(size_t)n.y * width + n.x] && type(n) == TYPE_WALL) {
                    fifo.push_back(n);
                    computed[(size_t)n.y * width + n.x] = 1;
                    lowest.push_back(n);
                }
            }
        }
        return lowest;
    }
};

static Grid grid;

// ----------------------------------------------------------------------------
// grid/GridMaker.java
// ----------------------------------------------------------------------------
namespace GridMaker {

const int MIN_GRID_HEIGHT = 10;
const int MAX_GRID_HEIGHT = 24;
const float ASPECT_RATIO = 1.8f;
const int SPAWN_HEIGHT = 3;
const int DESIRED_SPAWNS = 4;

// getFreeAbove(c, by)
static std::vector<Coord> getFreeAbove(const Coord& c, int by) {
    std::vector<Coord> result;
    for (int i = 1; i <= by; i++) {
        Coord above(c.x, java::sub(c.y, i));
        if (grid.valid(above) && grid.type(above) == TYPE_EMPTY) result.push_back(above);
        else break;
    }
    return result;
}

static void checkGrid() {
    for (const Coord& c : grid.apples)
        if (grid.type(c) != TYPE_EMPTY)
            throw java::Exception{"java.lang.RuntimeException: Apple on wall at (" + std::to_string(c.x) + ", " + std::to_string(c.y) + ")"};
    for (size_t i = 0; i < grid.apples.size(); i++)
        for (size_t j = i + 1; j < grid.apples.size(); j++)
            if (grid.apples[i] == grid.apples[j]) throw java::Exception{"java.lang.RuntimeException: Duplicate apples"};
}

static void make(java::SHA1PRNG& random, int leagueLevel) {
    double skew;
    if (leagueLevel == 1) skew = 2;
    else if (leagueLevel == 2) skew = 1;
    else if (leagueLevel == 3) skew = 0.8;
    else skew = 0.3;

    double rand = random.nextDouble();
    int height = MIN_GRID_HEIGHT + java::l2i(java::round(std::pow(rand, skew) * (MAX_GRID_HEIGHT - MIN_GRID_HEIGHT)));
    int width = java::roundFloat((float)height * ASPECT_RATIO);
    if (width % 2 != 0) width += 1;
    grid.init(width, height);

    double b = 5 + random.nextDouble() * 10;

    for (int x = 0; x < width; x++) grid.setType(Coord(x, height - 1), TYPE_WALL);

    for (int y = height - 2; y >= 0; y--) {
        double yNorm = (double)(height - 1 - y) / (height - 1);
        double blockChanceEl = 1 / (yNorm + 0.1) / b;
        for (int x = 0; x < width; x++)
            if (random.nextDouble() < blockChanceEl) grid.setType(Coord(x, y), TYPE_WALL);
    }

    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            Coord c(x, y);
            grid.setType(grid.opposite(c), grid.type(c));
        }

    // Fill islands
    for (const std::vector<Coord>& island : grid.detectAirPockets())
        if (island.size() < 10)
            for (const Coord& c : island) grid.setType(c, TYPE_WALL);

    bool somethingDestroyed = true;
    while (somethingDestroyed) {
        somethingDestroyed = false;
        for (int i = 0; i < grid.cellCount(); i++) {
            Coord c = grid.coordAt(i);
            if (grid.type(c) == TYPE_WALL) continue;
            std::vector<Coord> neighbourWalls;
            for (const Coord& n : grid.getNeighbours(c, ADJACENCY, 4))
                if (grid.type(n) == TYPE_WALL) neighbourWalls.push_back(n);
            if (neighbourWalls.size() >= 3) {
                std::vector<Coord> destroyable;
                for (const Coord& n : neighbourWalls)
                    if (n.y <= c.y) destroyable.push_back(n);
                java::shuffle(destroyable, random);
                if (destroyable.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
                grid.setType(destroyable[0], TYPE_EMPTY);
                grid.setType(grid.opposite(destroyable[0]), TYPE_EMPTY);
                somethingDestroyed = true;
            }
        }
    }

    // Sink lowest island
    std::vector<Coord> island = grid.detectLowestIsland();
    std::vector<char> inIsland(grid.types.size(), 0);
    for (const Coord& c : island) inIsland[(size_t)c.y * width + c.x] = 1;
    auto islandContains = [&](const Coord& c) { return grid.valid(c) && inIsland[(size_t)c.y * width + c.x]; };
    int lowerBy = 0;
    bool canLower = true;
    while (canLower) {
        for (int x = 0; x < width; x++) {
            Coord c(x, height - 1 - (lowerBy + 1));
            if (!islandContains(c)) {
                canLower = false;
                break;
            }
        }
        if (canLower) lowerBy++;
    }
    if (lowerBy >= 2) lowerBy = random.nextInt(2, lowerBy + 1);
    for (const Coord& c : island) {
        grid.setType(c, TYPE_EMPTY);
        grid.setType(grid.opposite(c), TYPE_EMPTY);
    }
    for (const Coord& c : island) {
        Coord lowered(c.x, c.y + lowerBy);
        if (grid.valid(lowered)) {
            grid.setType(lowered, TYPE_WALL);
            grid.setType(grid.opposite(lowered), TYPE_WALL);
        }
    }

    // Spawn a few apples
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width / 2; ++x) {
            Coord c(x, y);
            if (grid.type(c) == TYPE_EMPTY && random.nextDouble() < 0.025) {
                grid.apples.push_back(c);
                grid.apples.push_back(grid.opposite(c));
            }
        }

    // the "better generation method" when the first one gave fewer than 8
    if (grid.apples.size() < 8) {
        grid.apples.clear();
        std::vector<Coord> freeTiles; // a LinkedList: Collections.shuffle shuffles it like an array
        for (int i = 0; i < grid.cellCount(); i++)
            if (grid.type(grid.coordAt(i)) == TYPE_EMPTY) freeTiles.push_back(grid.coordAt(i));
        java::shuffle(freeTiles, random);
        int minApplesCoords = std::max(4, java::d2i(0.025 * (double)freeTiles.size()));
        size_t head = 0; // freeTiles.poll() takes freeTiles[head]
        while ((int)grid.apples.size() < minApplesCoords * 2 && head < freeTiles.size()) {
            Coord c = freeTiles[head++];
            grid.apples.push_back(c);
            grid.apples.push_back(grid.opposite(c));
            auto it = std::find(freeTiles.begin() + head, freeTiles.end(), grid.opposite(c));
            if (it != freeTiles.end()) freeTiles.erase(it);
        }
    }

    // Convert lone walls to apples
    for (int i = 0; i < grid.cellCount(); i++) {
        Coord c = grid.coordAt(i);
        if (grid.type(c) == TYPE_EMPTY) continue;
        int neighbourWallCount = 0;
        for (const Coord& n : grid.getNeighbours(c, ADJACENCY_8, 8))
            if (grid.type(n) == TYPE_WALL) neighbourWallCount++;
        if (neighbourWallCount == 0) {
            grid.setType(c, TYPE_EMPTY);
            grid.setType(grid.opposite(c), TYPE_EMPTY);
            grid.apples.push_back(c);
            grid.apples.push_back(grid.opposite(c));
        }
    }

    std::vector<Coord> potentialSpawns;
    for (int i = 0; i < grid.cellCount(); i++) {
        Coord c = grid.coordAt(i);
        if (grid.type(c) != TYPE_WALL) continue;
        if ((int)getFreeAbove(c, SPAWN_HEIGHT).size() < SPAWN_HEIGHT) continue;
        potentialSpawns.push_back(c);
    }
    java::shuffle(potentialSpawns, random);
    int desiredSpawns = DESIRED_SPAWNS;
    if (height <= 15) desiredSpawns--;
    if (height <= 10) desiredSpawns--;
    size_t next = 0; // potentialSpawns.remove(0) takes potentialSpawns[next]
    while (desiredSpawns > 0 && next < potentialSpawns.size()) {
        Coord spawn = potentialSpawns[next++];
        std::vector<Coord> spawnLoc = getFreeAbove(spawn, SPAWN_HEIGHT);
        // Check not neighbouring other spawn (or center line)
        bool tooClose = false;
        for (const Coord& c : spawnLoc) {
            if (c.x == width / 2 - 1 || c.x == width / 2) {
                tooClose = true;
                break;
            }
            for (const Coord& n : grid.getNeighbours(c, ADJACENCY_8, 8)) {
                if (grid.isSpawn(n) || grid.isSpawn(grid.opposite(n))) {
                    tooClose = true;
                    break;
                }
            }
            if (tooClose) break;
        }
        if (tooClose) continue;
        for (const Coord& ac : spawnLoc) {
            grid.spawns.push_back(ac);
            auto it = std::find(grid.apples.begin(), grid.apples.end(), ac);
            if (it != grid.apples.end()) grid.apples.erase(it);
            it = std::find(grid.apples.begin(), grid.apples.end(), grid.opposite(ac));
            if (it != grid.apples.end()) grid.apples.erase(it);
        }
        desiredSpawns--;
    }

    // Assert every apple is unique and not on a wall
    checkGrid();
}

} // namespace GridMaker

// ----------------------------------------------------------------------------
// event/Animation.java, EventData.java
// ----------------------------------------------------------------------------
namespace Event {
const int MOVE = 0, FALL = 1, EAT = 2, DEATH = 3, BEHEAD = 4;
}
const int HALF = 500;

struct EventData {
    int type = 0;
    int start = 0, end = 0; // animData
    std::vector<i32> params;
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
// Bird.java, Player.java
// ----------------------------------------------------------------------------
struct Bird {
    int id = 0;
    int owner = 0; // the owner's index
    bool alive = true;
    std::deque<Coord> body; // a LinkedList
    int direction = NONE;
    bool hasMessage = false; // message != null
    String message;

    const Coord& getHeadPos() const {
        if (body.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
        return body[0];
    }
    int getFacing() const {
        if (body.size() < 2) return UNSET;
        return dirFromCoord(Coord(java::sub(body[0].x, body[1].x), java::sub(body[0].y, body[1].y)));
    }
    bool contains(const Coord& c) const { return std::find(body.begin(), body.end(), c) != body.end(); }
    void setMessage(const String& m) {
        hasMessage = true;
        message = m;
        if (m.size() > 48) message = m.substr(0, 46) + u"...";
    }
};

static std::vector<Bird> allBirds; // players.stream().flatMap(p -> p.birds.stream()): player 0's then player 1's

struct Player : cg::Player {
    std::vector<int> birds; // indices in allBirds
    std::vector<Coord> marks;

    int getExpectedOutputLines() const override { return 1; }
    std::string nick() const { return "$" + std::to_string(index); }

    void reset() {
        for (int b : birds) {
            allBirds[b].direction = NONE;
            allBirds[b].hasMessage = false;
            allBirds[b].message.clear();
        }
        marks.clear();
    }
    Bird* getBirdById(i32 id) {
        for (int b : birds)
            if (allBirds[b].id == id) return &allBirds[b];
        return nullptr;
    }
    bool addMark(const Coord& c) {
        if (marks.size() < 4) {
            marks.push_back(c);
            return true;
        }
        return false;
    }
};

static Player players[2];

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.isActive()) r.push_back(&p);
    return r;
}

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
namespace Game {

static int losses[2] = {0, 0};
static std::string endScreenJson; // EndScreenModule.setScores(scores, scoreTexts)

static std::vector<int> getLiveBirds() {
    std::vector<int> r;
    for (size_t i = 0; i < allBirds.size(); i++)
        if (allBirds[i].alive) r.push_back((int)i);
    return r;
}

static void startEvent(int type, std::vector<i32> params, int duration) {
    EventData e;
    e.type = type;
    e.params = std::move(params);
    animation.startAnim(e, duration);
}
static void launchMoveEvent(const Bird& bird, bool willEatApple) {
    startEvent(Event::MOVE, {bird.id, bird.owner, bird.getHeadPos().x, bird.getHeadPos().y, willEatApple ? 1 : 0}, HALF);
}
static void launchBeheadEvent(const Bird& bird) { startEvent(Event::BEHEAD, {bird.id, bird.owner}, HALF); }
static void launchEatEvent(const Bird& bird, const Coord& coord, int growth) {
    startEvent(Event::EAT, {bird.id, bird.owner, growth, coord.x, coord.y}, HALF);
}
static void launchFallEvent(const Bird& bird, int numberOfCells) { startEvent(Event::FALL, {bird.id, bird.owner, numberOfCells}, 200); }
static void launchDeathEvent(const Bird& bird) { startEvent(Event::DEATH, {bird.id, bird.owner}, HALF); }

// findSpawnLocations: each spawn island sorted (Coord.compareTo)
static std::vector<std::vector<Coord>> findSpawnLocations() {
    std::vector<std::vector<Coord>> r = grid.detectSpawnIslands();
    for (std::vector<Coord>& s : r) std::sort(s.begin(), s.end());
    return r;
}

static void initPlayers() {
    int birdId = 0;
    std::vector<std::vector<Coord>> spawnLocations = findSpawnLocations();
    allBirds.clear();
    for (Player& p : players) {
        p.birds.clear();
        p.marks.clear();
        for (const std::vector<Coord>& spawn : spawnLocations) {
            Bird bird;
            bird.id = birdId++;
            bird.owner = p.index;
            for (Coord c : spawn) {
                if (p.index == 1) c = grid.opposite(c);
                bird.body.push_back(c);
                if (bird.body.size() == 1) {
                    // Is the head enclosed?
                    Coord left = c.add(-1, 0), right = c.add(1, 0);
                    if (grid.type(left) == TYPE_WALL && grid.type(right) == TYPE_WALL) {
                        grid.setType(left, TYPE_EMPTY);
                        grid.setType(grid.opposite(left), TYPE_EMPTY);
                    }
                }
            }
            p.birds.push_back((int)allBirds.size());
            allBirds.push_back(bird);
        }
    }
}

static void init() {
    GridMaker::make(cg::gm.getRandom(), cg::gm.getLeagueLevel());
    initPlayers();
}

static void resetGameTurnData() {
    animation.reset();
    for (Player& p : players) p.reset();
}

static void doMoves() {
    for (Player& p : players)
        for (int bi : p.birds) {
            Bird& bird = allBirds[bi];
            if (!bird.alive) continue;
            if (bird.direction == NONE || bird.direction == UNSET) bird.direction = bird.getFacing();
            Coord newHead = bird.getHeadPos().add(DIR_X[bird.direction], DIR_Y[bird.direction]);
            bool willEatApple = grid.hasApple(newHead);
            if (!willEatApple) bird.body.pop_back();
            bird.body.push_front(newHead);
            launchMoveEvent(bird, willEatApple);
        }
}

static void doBeheadings() {
    std::vector<int> birdsToBehead;
    for (int bi : getLiveBirds()) {
        Bird& bird = allBirds[bi];
        Coord head = bird.getHeadPos();
        bool isInWall = grid.type(head) == TYPE_WALL;
        bool isInBird = false;
        for (const Bird& b : allBirds) {
            if (!b.alive || !b.contains(head)) continue;
            // head intersects with another bird, or with the same bird on a pos that is not its head
            if (b.id != bird.id || std::find(b.body.begin() + 1, b.body.end(), b.getHeadPos()) != b.body.end()) {
                isInBird = true;
                break;
            }
        }
        if (isInWall || isInBird) birdsToBehead.push_back(bi);
    }
    for (int bi : birdsToBehead) {
        Bird& b = allBirds[bi];
        if (b.body.size() <= 3) {
            launchDeathEvent(b);
            b.alive = false;
            losses[b.owner] += (int)b.body.size();
        } else {
            b.body.pop_front();
            launchBeheadEvent(b);
            losses[b.owner]++;
        }
    }
}

static void doEats() {
    std::vector<Coord> applesEatenThisTurn;
    for (Player& p : players)
        for (int bi : p.birds) {
            Bird& bird = allBirds[bi];
            if (bird.alive && grid.hasApple(bird.getHeadPos())) {
                launchEatEvent(bird, bird.getHeadPos(), 1);
                applesEatenThisTurn.push_back(bird.getHeadPos());
            }
        }
    // apples.removeAll(applesEatenThisTurn)
    std::vector<Coord> kept;
    for (const Coord& a : grid.apples)
        if (std::find(applesEatenThisTurn.begin(), applesEatenThisTurn.end(), a) == applesEatenThisTurn.end()) kept.push_back(a);
    grid.apples = kept;
}

static bool hasTileOrAppleUnder(const Coord& c) {
    Coord below = c.add(0, 1);
    if (grid.type(below) == TYPE_WALL) return true;
    return grid.hasApple(below);
}

// isGrounded(c, frozenBirds)
static bool isGrounded(const Coord& c, const std::vector<char>& frozen) {
    if (hasTileOrAppleUnder(c)) return true;
    Coord under = c.add(0, 1);
    for (size_t i = 0; i < allBirds.size(); i++)
        if (frozen[i] && allBirds[i].contains(under)) return true;
    return false;
}

static void doFalls() {
    bool somethingFell = true;
    std::map<int, int> fallDistances; // TreeMap<Integer, Integer>
    std::vector<int> outOfBounds;
    // airborneBirds / groundedBirds: HashSets of Birds (identity hash codes). Only the order of the
    // death events of birds leaving the grid at the same step depends on their order (view only):
    // here the order of getLiveBirds().
    std::vector<int> airborne = getLiveBirds();
    std::vector<char> grounded(allBirds.size(), 0);

    while (somethingFell) {
        somethingFell = false;
        bool somethingGotGrounded = true;
        while (somethingGotGrounded) {
            somethingGotGrounded = false;
            for (int bi : airborne) {
                bool g = false;
                for (const Coord& c : allBirds[bi].body)
                    if (isGrounded(c, grounded)) {
                        g = true;
                        break;
                    }
                if (g) {
                    grounded[bi] = 1;
                    somethingGotGrounded = true;
                }
            }
            std::vector<int> rest;
            for (int bi : airborne)
                if (!grounded[bi]) rest.push_back(bi);
            airborne = rest;
        }

        for (int bi : airborne) {
            Bird& bird = allBirds[bi];
            somethingFell = true;
            for (Coord& c : bird.body) c = c.add(0, 1);
            fallDistances[bird.id]++;
            // check out of bounds
            bool allBelow = true;
            for (const Coord& part : bird.body)
                if (!(part.y >= grid.height + 1)) allBelow = false;
            if (allBelow) {
                bird.alive = false;
                outOfBounds.push_back(bi);
            }
        }
        std::vector<int> rest;
        for (int bi : airborne)
            if (std::find(outOfBounds.begin(), outOfBounds.end(), bi) == outOfBounds.end()) rest.push_back(bi);
        airborne = rest;
    }

    for (auto& fd : fallDistances) {
        // getBirdById
        const Bird* bird = nullptr;
        for (const Bird& b : allBirds)
            if (b.id == fd.first) {
                bird = &b;
                break;
            }
        if (fd.second > 0) launchFallEvent(*bird, fd.second);
    }
    for (int bi : outOfBounds) launchDeathEvent(allBirds[bi]);
}

static bool isGameOver() {
    bool noApples = grid.apples.empty();
    bool playerDed = false;
    for (Player& p : players) {
        bool anyAlive = false;
        for (int bi : p.birds)
            if (allBirds[bi].alive) anyAlive = true;
        if (!anyAlive) playerDed = true;
    }
    return noApples || playerDed;
}

static void computeEvents() {
    int minTime = 500;
    animation.catchUp();
    int frameTime = std::max(animation.frameTime, minTime);
    cg::gm.setFrameDuration(frameTime);
}

static void performGameUpdate() {
    int currentTime = animation.frameTime;
    doMoves();
    animation.frameTime = currentTime;
    doEats();
    animation.catchUp();
    doBeheadings();
    animation.catchUp();
    doFalls();
    animation.catchUp();
    computeEvents();
    if (isGameOver()) cg::gm.endGame();
}

// String.format("%s point%s", ...)
static std::string points(int score) { return std::to_string(score) + " point" + (score != 1 ? "s" : ""); }

static void onEnd() {
    std::string scoreTexts[2];
    for (Player& p : players) {
        if (!p.isActive()) {
            p.setScore(-1);
            scoreTexts[p.index] = "-";
        } else {
            int sum = 0;
            for (int bi : p.birds)
                if (allBirds[bi].alive) sum += (int)allBirds[bi].body.size();
            p.setScore(sum);
        }
    }
    if (players[0].getScore() == players[1].getScore() && players[0].getScore() != -1) {
        // tie breaker: least losses
        for (Player& p : players)
            scoreTexts[p.index] = points(p.getScore()) + " (" + (losses[0] == losses[1] ? "and" : "but") + " " + std::to_string(losses[p.index]) + " loss" +
                                  (losses[p.index] != 1 ? "es" : "") + ")";
        for (Player& p : players) p.setScore(java::sub(p.getScore(), losses[p.index]));
    } else {
        for (Player& p : players) scoreTexts[p.index] = p.getScore() > -1 ? points(p.getScore()) : "-";
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
    lines.push_back(std::to_string(grid.width));
    lines.push_back(std::to_string(grid.height));
    for (int y = 0; y < grid.height; ++y) {
        std::string row;
        for (int x = 0; x < grid.width; ++x) row += grid.type(x, y) == TYPE_WALL ? '#' : '.';
        lines.push_back(row);
    }
    // birds per player
    lines.push_back(std::to_string(players[0].birds.size()));
    for (int bi : player.birds) lines.push_back(std::to_string(allBirds[bi].id));
    for (int bi : players[1 - player.index].birds) lines.push_back(std::to_string(allBirds[bi].id));
    return lines;
}

static std::vector<std::string> serializeFrameInfoFor(const Player&) {
    std::vector<std::string> lines;
    lines.push_back(std::to_string(grid.apples.size()));
    for (const Coord& c : grid.apples) lines.push_back(std::to_string(c.x) + " " + std::to_string(c.y));
    std::vector<int> liveBirds = Game::getLiveBirds();
    lines.push_back(std::to_string(liveBirds.size()));
    for (int bi : liveBirds) {
        const Bird& b = allBirds[bi];
        std::string body;
        for (size_t i = 0; i < b.body.size(); i++) body += (i ? ":" : "") + std::to_string(b.body[i].x) + "," + std::to_string(b.body[i].y);
        lines.push_back(std::to_string(b.id) + " " + body);
    }
    return lines;
}

// the view's global data (ViewModule)
static String serializeGlobalData() {
    std::string s = std::to_string(grid.width) + "|" + std::to_string(grid.height);
    for (int y = 0; y < grid.height; ++y)
        for (int x = 0; x < grid.width; ++x) s += "|" + std::to_string(grid.type(x, y));
    s += "|" + std::to_string(grid.apples.size());
    for (const Coord& c : grid.apples) s += "|" + std::to_string(c.x) + "|" + std::to_string(c.y);
    for (const Player& p : players) {
        s += "|" + std::to_string(p.birds.size());
        for (int bi : p.birds) {
            const Bird& b = allBirds[bi];
            s += "|" + std::to_string(b.id) + "|" + std::to_string(b.body.size());
            for (const Coord& c : b.body) s += "|" + std::to_string(c.x) + "|" + std::to_string(c.y);
        }
    }
    return java::ascii(s);
}

// the view's frame data: the events, the players' execution times and marks, the birds' messages
// ('|' replaced with U+2223)
static String serializeFrameData() {
    std::string s = std::to_string(animation.viewerEvents.size());
    for (const EventData& e : animation.viewerEvents) {
        s += "|" + std::to_string(e.type) + "|" + std::to_string(e.start) + "|" + std::to_string(e.end) + "|";
        for (size_t i = 0; i < e.params.size(); i++) s += (i ? " " : "") + std::to_string(e.params[i]);
    }
    for (const Player& p : players) {
        s += "|" + std::to_string(p.getLastExectionTimeMs()) + "|" + std::to_string(p.marks.size());
        for (const Coord& c : p.marks) s += "|" + std::to_string(c.x) + " " + std::to_string(c.y);
    }
    int withMessage = 0;
    for (const Bird& b : allBirds) withMessage += b.hasMessage;
    s += "|" + std::to_string(withMessage);
    String r = java::ascii(s);
    for (const Bird& b : allBirds) {
        if (!b.hasMessage) continue;
        r += java::ascii("|" + std::to_string(b.id) + "|" + std::to_string(b.owner) + "|");
        r += java::replace(b.message, u"|", u"∣");
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

// (?<g>\d+): the digits at `pos` (at least one)
static bool digitsAt(const String& s, size_t& pos, String& group) {
    size_t a = pos;
    while (pos < s.size() && java::isRegexDigit(s[pos])) pos++;
    if (pos == a) return false;
    group = s.substr(a, pos - a);
    return true;
}

// Integer.valueOf / Integer.parseInt of a group: an overflow throws a NumberFormatException that
// nothing catches (the game fails)
static i32 parseInt(const String& g) {
    i32 v;
    if (!java::parseInt(g, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(g))};
    return v;
}

enum ActionKind { MOVE, MARK, WAIT };
struct Action {
    ActionKind kind = WAIT;
    i32 birdId = 0;
    int direction = NONE;
    bool hasMessage = false; // the message group matched
    String message;
    Coord coord;
};

// MOVE_UP/DOWN/LEFT/RIGHT: "^(?<birdId>\d+) <DIR>( (?<message>[^;]*))?" (the command holds no ';')
static bool matchMove(const String& s, const char* word, int dir, Action& action) {
    size_t pos = 0;
    String id;
    if (!digitsAt(s, pos, id)) return false;
    if (!literalAt(s, pos, " ")) return false;
    if (!literalAt(s, pos, word)) return false;
    bool hasMessage = false;
    if (pos != s.size()) {
        if (s[pos] != u' ') return false;
        hasMessage = true;
        pos++;
    }
    action.kind = MOVE;
    action.birdId = parseInt(id);
    action.direction = dir;
    action.hasMessage = hasMessage;
    action.message = hasMessage ? s.substr(pos) : String();
    return true;
}

// ActionType.values() in order; true when `command` matches one of them
static bool parseAction(const String& command, Action& action) {
    if (matchMove(command, "UP", NORTH, action)) return true;
    if (matchMove(command, "DOWN", SOUTH, action)) return true;
    if (matchMove(command, "LEFT", WEST, action)) return true;
    if (matchMove(command, "RIGHT", EAST, action)) return true;
    {
        // MARK (?<x>\d+) (?<y>\d+)
        size_t pos = 0;
        String gx, gy;
        if (literalAt(command, pos, "MARK ") && digitsAt(command, pos, gx) && literalAt(command, pos, " ") && digitsAt(command, pos, gy) &&
            pos == command.size()) {
            action.kind = MARK;
            i32 x = parseInt(gx);
            i32 y = parseInt(gy);
            action.coord = Coord(x, y);
            return true;
        }
    }
    size_t pos = 0;
    if (literalAt(command, pos, "WAIT") && pos == command.size()) {
        action.kind = WAIT;
        return true;
    }
    return false;
}

struct GameError {
    std::string message;
};

static void parseCommands(Player& player, const std::vector<String>& lines) {
    std::vector<String> errors;
    // lines.get(0): empty only when a bot line desynchronised the protocol (see common/engine.h)
    if (lines.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
    const String& line = lines[0];
    std::vector<String> commands = java::split(line, u';');

    int reasonableLimitForActions = 30;
    for (const String& command : commands) {
        if (reasonableLimitForActions-- <= 0) return;
        Action action;
        bool found;
        try {
            found = parseAction(command, action);
            if (found && action.kind == MOVE) {
                i32 birdId = action.birdId;
                Bird* bird = player.getBirdById(birdId);
                if (bird == nullptr) throw GameError{"Bird not found for id " + std::to_string(birdId)};
                if (!bird->alive) throw GameError{"Bird with id " + std::to_string(birdId) + " is dead"};
                if (bird->direction != NONE) throw GameError{"Bird id " + std::to_string(birdId) + " has already been given a move"};
                if (dirOpposite(bird->getFacing()) == action.direction)
                    throw GameError{"Bird id " + std::to_string(birdId) + " cannot move backwards"};
                bird->direction = action.direction;
                if (action.hasMessage) bird->setMessage(action.message);
            } else if (found && action.kind == MARK) {
                if (!player.addMark(action.coord)) throw GameError{"Too many MARK actions this turn"};
            }
        } catch (const GameError& e) {
            errors.push_back(cg::GameManager::formatErrorMessage(java::ascii(player.nick() + ": " + e.message)));
            continue;
        }
        if (!found) {
            // InvalidInputException(Game.getExpected(command), command)
            String message = u"Invalid Input: Expected MESSAGE text but got '" + command + u"'";
            // deactivatePlayer: escapeHTMLEntities, score -1
            player.deactivate(java::replace(java::replace(message, u"&lt;", u"<"), u"&gt;", u">"));
            player.setScore(-1);
            cg::gm.addToGameSummary(message);
            cg::gm.addToGameSummary(cg::GameManager::formatErrorMessage(java::ascii(player.nick() + ": disqualified!")));
            break;
        }
    }

    const size_t maxErrs = 4;
    if (errors.size() <= maxErrs + 1) {
        for (const String& err : errors) cg::gm.addToGameSummary(err);
    } else {
        for (size_t i = 0; i < maxErrs; i++) cg::gm.addToGameSummary(errors[i]);
        cg::gm.addToGameSummary(cg::GameManager::formatErrorMessage(java::ascii("...and " + std::to_string(errors.size() - maxErrs) + " more errors.")));
    }
}

} // namespace Commands

// ----------------------------------------------------------------------------
// Referee.java, ViewModule, EndScreenModule
// ----------------------------------------------------------------------------
struct SnakeByteReferee : cg::Referee {
    void init() override {
        try {
            cg::gm.setMaxTurns(200);
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
    static SnakeByteReferee referee;
    cg::Options options;
    options.maxPlayers = 2;
    options.engine46 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
