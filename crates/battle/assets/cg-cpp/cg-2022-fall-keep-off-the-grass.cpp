// Keep Off the Grass (CodinGame Fall Challenge 2022) - headless C++ referee, port of
// https://github.com/CodinGame/FallChallenge2022-KeepOffTheGrass (commit 7694269), on the CodinGame
// game engine 4.4.0 (runner + CommandLineInterface-v4, league level 19), 2 players.
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints "<p0score>\n<p1score>\n" to stdout (matches the jar). When the Java referee crashes, the
// runner stops the game and the jar prints "null\nnull\n" and exits 0: so do we.
// The engine and the command line are common/engine.h; -l writes its trace (see there) plus a
// "STATE" line per turn.
// With -p3/-p4 the Java referee fails during init (2 spawn points only): null scores, like the jar.
//
// The Java referee is followed very closely (same data structures where order matters, same
// floating point operations in the same order). Notable Java behaviours reproduced here:
//   - gameManager.getRandom(): SecureRandom("SHA1PRNG") seeded with setSeed(long), used through
//     java.util.Random's nextDouble / nextBoolean / nextInt(bound) and Collections.shuffle
//   - the map generation (Math.pow, island bridging that does not recompute islands after a bridge)
//   - the A* used by MOVE: every node has the same heuristic, so the java.util.PriorityQueue tie order
//     decides the path; int overflow of manhattan distances for huge MOVE targets
//   - a disqualified player's BUILDs that precede the invalid command are still applied (doBuilds
//     iterates all players), and recyclers of inactive players keep working
//   - engine I/O (Agent#getOutput): '\r', '\n' and "\r\n" (once, even across turns) end a line,
//     10000 bytes max, Java UTF-8 decoding (U+FFFD for malformed bytes), 1000 ms for the first turn
//     of each player and 50 ms afterwards, asynchronous writes to the agents' stdin
//   - the jar launches the agents, then starts its referee (JVM/Guice start-up, ~0.5 s) before their
//     first input, so an agent's start-up time hardly counts: the first turn's 1000 ms run from the
//     latest of the input being sent and 500 ms after the agent's launch (wall clock, approximate)
//   - command parsing: String.split(";"), trim(), the ActionType regexes, Integer.valueOf overflow
//   - the referee reads player lines with java.util.Scanner.nextLine(), which also splits at
//     U+0085, U+2028 and U+2029: the referee only sees the text before it, and the rest of the line
//     is read as the next engine command at the next player execution, which crashes the referee
//   - engine limits that make the Java referee crash (null scores): view data over 1 MiB in total
//     (GameManager.dumpView, counted in UTF-16 chars with the platform line separator), and a
//     referee command over 30000 bytes (RefereeAgent buffer: a view frame, the game summary or the
//     tooltips, which repeat a disqualified player's invalid command)
// Not reproduced:
//   - -seed 0: SecureRandom.setSeed(0) is ignored by Java, so the jar self-seeds (random map); we
//     use a random seed too
//   - Math.pow (HotSpot intrinsic) and the C library's pow differ by 1 ulp in ~0.05% of the calls of
//     the map generation; a map only changes if the value is within 1 ulp of a durability threshold
//     (about 1e-15 per cell; 340k maps compared with the jar's Grid were identical)
//   - agents' stderr is discarded (the jar reads at most 4 KB of it per turn, an agent writing much
//     more could block in the jar)

#include "common/engine.h"

using jstring = java::String;

// ----------------------------------------------------------------------------
// Constants (Config.java, league level 19, and the engine)
// ----------------------------------------------------------------------------
namespace C {
const int MAP_MIN_WIDTH = 12;
const int MAP_MAX_WIDTH = 24; // 15 in league 1 only
const double MAP_ASPECT_RATIO = 1 / 2.0;
const int MIN_SPAWN_DISTANCE = 7;
const int CELL_MAX_DURABILITY = 10;
const int RECYCLER_INCOME = 1;
const int PLAYER_STARTING_MONEY = 10;
const int PLAYER_MINIMAL_INCOME = 10;
const int COST_UNIT = 10;
const int COST_RECYCLER = 10;
const int MAX_TURNS = 200;
const int EARLY_FINISH_TURNS = 20;

// engine
const int FIRST_TURN_MAX_TIME = 1000;
const int TURN_MAX_TIME = 50;
const int AGENT_HEAD_START = 500; // time between launching the agents and their first input in the jar
} // namespace C

// ----------------------------------------------------------------------------
// Coord.java, Cell.java, Unit.java, Recycler.java
// ----------------------------------------------------------------------------
struct Coord {
    i32 x, y;
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    Coord add(const Coord& d) const { return {java::add(x, d.x), java::add(y, d.y)}; }
    i32 manhattanTo(i32 ox, i32 oy) const { return java::add(java::abs(java::sub(ox, x)), java::abs(java::sub(oy, y))); }
    i32 manhattanTo(const Coord& o) const { return manhattanTo(o.x, o.y); }
    double sqrEuclideanTo(double ox, double oy) const {
        // Math.pow(v, 2) is exact here (v is a small multiple of 0.5)
        double dx = ox - x, dy = oy - y;
        return dx * dx + dy * dy;
    }
};

static const Coord FOUR[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}; // Adjacency.FOUR

struct Cell {
    int durability = 0;
    int owner = -1; // player index, -1: none
    bool isHole() const { return durability == 0; }
    void garanteeNotHole() {
        if (isHole()) durability = 1;
    }
    bool damage() {
        durability--;
        if (durability == 0) {
            owner = -1;
            return true;
        }
        return false;
    }
};

struct Unit {
    bool valid = false; // an entry exists in Player.units (getUnitAt returns NO_UNIT otherwise)
    i32 availableCount = 0;
    i32 unavailableCount = 0;
};

struct Recycler {
    Coord coord;
    int owner;
};

// ----------------------------------------------------------------------------
// Grid.java
// ----------------------------------------------------------------------------
struct Grid {
    int width = 0, height = 0;
    bool ySymetry = false, snaky = false;
    std::vector<Cell> cells; // y * width + x, every coord of the map is in Grid.cells
    std::vector<std::vector<Coord>> spawns;
    java::SHA1PRNG* random = nullptr;

    bool isValid(const Coord& c) const { return c.x >= 0 && c.y >= 0 && c.x < width && c.y < height; }
    int idx(const Coord& c) const { return c.y * width + c.x; }
    // Grid.get: NO_CELL (invalid, durability 0, no owner) outside of the map. NO_CELL is never modified.
    Cell get(const Coord& c) const { return isValid(c) ? cells[idx(c)] : Cell(); }
    Cell& at(const Coord& c) { return cells[idx(c)]; }
    bool isHole(const Coord& c) const { return get(c).isHole(); }
    bool isOwner(const Coord& c, int player) const { return isValid(c) && cells[idx(c)].owner == player; }

    Coord opposite(const Coord& c) const { return {width - c.x - 1, ySymetry ? (height - c.y - 1) : c.y}; }

    int getNeighbours(const Coord& pos, Coord out[4]) const {
        int n = 0;
        for (const Coord& d : FOUR) {
            Coord c = pos.add(d);
            if (isValid(c)) out[n++] = c;
        }
        return n;
    }

    int randInt(int from, int to) { return random->nextInt(to - from) + from; }
    int randInt(int to) { return randInt(0, to); }

    int randomDurability(int x, int y, const Coord& centre) {
        double dist = Coord{x, y}.manhattanTo(centre);
        double maxDist = centre.manhattanTo(0, 0);
        double d = random->nextDouble();
        d *= d;
        double n = 1 / (1 + (1 - (dist / maxDist)) * 3);
        d = std::pow(d, n);
        if (d < .15) return 0;
        if (d < .35) return snaky ? 0 : 4;
        if (d < .5) return 6;
        if (d < .8) return 8;
        if (d < .9) return 9;
        return C::CELL_MAX_DURABILITY;
    }

    void generate(java::SHA1PRNG& rnd, int nPlayers) {
        random = &rnd;
        snaky = random->nextDouble() < 0.1;
        width = randInt(C::MAP_MIN_WIDTH, C::MAP_MAX_WIDTH + 1);
        height = (int)(width * C::MAP_ASPECT_RATIO);
        ySymetry = random->nextBoolean();

        cells.assign(width * height, Cell());
        std::vector<char> present(width * height, 0);
        std::vector<Coord> possibleSpawnPoints;
        std::vector<Coord> emptyCells; // Stack<Coord>

        Coord centre = {width / 2, height / 2};
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                Coord coord = {x, y};
                if (present[idx(coord)]) continue;
                int durability = randomDurability(x, y, centre);
                at(coord).durability = durability;
                present[idx(coord)] = 1;
                if (durability == 0) emptyCells.push_back(coord);
                Coord opp = opposite(coord);
                if (opp != coord) {
                    at(opp).durability = durability;
                    present[idx(opp)] = 1;
                    if (durability == C::CELL_MAX_DURABILITY && x > 1 && y > 1 && x < width - 1 && y < height - 1 &&
                        coord.manhattanTo(opp) >= C::MIN_SPAWN_DISTANCE && coord.manhattanTo(opp) >= (width * 0.5))
                        possibleSpawnPoints.push_back(coord);
                }
            }
        }

        spawns.clear();
        if (possibleSpawnPoints.empty()) possibleSpawnPoints.push_back({1, 1});
        int cellIdx = randInt((int)possibleSpawnPoints.size());
        Coord spawnPoint = possibleSpawnPoints[cellIdx];
        Coord opp = opposite(spawnPoint);
        Coord spawnPoints[2];
        if (random->nextBoolean()) { spawnPoints[0] = spawnPoint; spawnPoints[1] = opp; }
        else { spawnPoints[0] = opp; spawnPoints[1] = spawnPoint; }

        for (int playerIdx = 0; playerIdx < nPlayers; ++playerIdx) {
            std::vector<Coord> unitSpawns;
            Coord spawn = spawnPoints[playerIdx];
            Cell& spawnCentre = at(spawn);
            spawnCentre.owner = playerIdx;
            spawnCentre.garanteeNotHole();
            for (const Coord& d : FOUR) {
                Coord c = spawn.add(d);
                Cell& cell = at(c);
                cell.garanteeNotHole();
                cell.owner = playerIdx;
                unitSpawns.push_back(c);
            }
            spawns.push_back(unitSpawns);
        }

        // emptyCells.removeAll(all unit spawns)
        emptyCells.erase(std::remove_if(emptyCells.begin(), emptyCells.end(),
                                        [&](const Coord& c) {
                                            for (auto& l : spawns)
                                                for (auto& s : l)
                                                    if (s == c) return true;
                                            return false;
                                        }),
                         emptyCells.end());
        fixIslands(emptyCells);
    }

    // Islands are disjoint sets of coords: island[cell] is the id of the set containing the cell, -1 if
    // none. Islands are not recomputed after a bridge, so bridges belong to no island.
    std::vector<int> island;
    int islandCount = 0;
    int nextIslandId = 0;

    void detectIslands() {
        // the iteration order of cells.keySet() does not matter: islands are the connected components
        island.assign(width * height, -1);
        islandCount = 0;
        nextIslandId = 0;
        std::vector<Coord> fifo;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                Coord p = {x, y};
                if (get(p).isHole() || island[idx(p)] >= 0) continue;
                int id = nextIslandId++;
                islandCount++;
                fifo.clear();
                fifo.push_back(p);
                island[idx(p)] = id;
                for (size_t h = 0; h < fifo.size(); h++) {
                    Coord e = fifo[h];
                    for (const Coord& d : FOUR) {
                        Coord n = e.add(d);
                        if (isValid(n) && !get(n).isHole() && island[idx(n)] < 0) {
                            island[idx(n)] = id;
                            fifo.push_back(n);
                        }
                    }
                }
            }
        }
    }

    bool closeIslandGap(std::vector<Coord>& emptyCells) {
        std::vector<int> connecting;
        bool found = false;
        Coord bridge{0, 0};
        for (const Coord& coord : emptyCells) {
            Coord neighs[4];
            int nn = getNeighbours(coord, neighs);
            connecting.clear();
            for (int i = 0; i < nn; i++) {
                int id = island[idx(neighs[i])];
                if (id >= 0 && std::find(connecting.begin(), connecting.end(), id) == connecting.end()) connecting.push_back(id);
            }
            if (connecting.size() > 1) {
                bridge = coord;
                found = true;
                break;
            }
        }
        if (!found) return false;
        Coord coord = bridge;
        Coord opp = opposite(coord);
        at(coord).durability = 6;
        at(opp).durability = 6;
        auto removeFirst = [&](const Coord& c) {
            auto it = std::find(emptyCells.begin(), emptyCells.end(), c);
            if (it != emptyCells.end()) emptyCells.erase(it);
        };
        removeFirst(coord);
        removeFirst(opp);
        int id = nextIslandId++;
        for (int& v : island)
            if (v >= 0 && std::find(connecting.begin(), connecting.end(), v) != connecting.end()) v = id;
        islandCount = islandCount - (int)connecting.size() + 1;
        return true;
    }

    void fixIslands(std::vector<Coord>& emptyCells) {
        // Collections.shuffle(emptyCells, random)
        for (int i = (int)emptyCells.size(); i > 1; i--) std::swap(emptyCells[i - 1], emptyCells[random->nextInt(i)]);
        detectIslands();
        while (islandCount > 1) {
            if (!closeIslandGap(emptyCells)) break;
        }
    }
};

// ----------------------------------------------------------------------------
// AStar.java / PathFinder.java
// ----------------------------------------------------------------------------
struct PathItem {
    int cumulativeLength = 0;
    int totalPrevisionalLength = 0;
    int precedent = -1;
    Coord coord;
};

struct AStar {
    const Grid& grid;
    const std::vector<char>& restricted; // per cell: a recycler is there
    Coord from, target, nearest;
    double centreX, centreY;
    std::vector<PathItem> items;
    struct ByTotalPrevisionalLength {
        const std::vector<PathItem>* items;
        int operator()(int a, int b) const { return java::compareInt((*items)[a].totalPrevisionalLength, (*items)[b].totalPrevisionalLength); }
    };
    java::PriorityQueue<int, ByTotalPrevisionalLength> openList{ByTotalPrevisionalLength{&items}};
    std::vector<char> closed;

    AStar(const Grid& g, Coord f, Coord t, const std::vector<char>& r) : grid(g), restricted(r), from(f), target(t), nearest(f) {
        centreX = grid.width / 2.0;
        centreY = grid.height / 2.0;
    }

    int compareByDistanceToCentre(const Coord& a, const Coord& b) const {
        double da = a.sqrEuclideanTo(centreX, centreY), db = b.sqrEuclideanTo(centreX, centreY);
        return da < db ? -1 : (da > db ? 1 : 0);
    }

    // returns the index of the item that reached the target, -1 if not found
    int getPathItemLinkedList() {
        closed.assign(grid.width * grid.height, 0);
        items.push_back(PathItem());
        items.back().coord = from;
        openList.offer(0);
        while (!openList.isEmpty()) {
            int visiting = openList.poll();
            Coord visitingCoord = items[visiting].coord;
            if (visitingCoord == target) return visiting;
            if (closed[grid.idx(visitingCoord)]) continue;
            closed[grid.idx(visitingCoord)] = 1;

            Coord neighbors[4];
            int nn = grid.getNeighbours(visitingCoord, neighbors);
            // Collections.sort (stable) by distance to the centre
            std::stable_sort(neighbors, neighbors + nn, [&](const Coord& a, const Coord& b) { return compareByDistanceToCentre(a, b) < 0; });
            for (int i = 0; i < nn; i++) {
                const Coord& neighbor = neighbors[i];
                if (!grid.get(neighbor).isHole() && !restricted[grid.idx(neighbor)]) addToOpenList(visiting, visitingCoord, neighbor);
            }

            i32 visitingDist = visitingCoord.manhattanTo(target);
            i32 nearestDist = nearest.manhattanTo(target);
            if (visitingDist < nearestDist) {
                nearest = visitingCoord;
            } else if (visitingDist == nearestDist) {
                if (compareByDistanceToCentre(visitingCoord, nearest) < 0) nearest = visitingCoord;
            }
        }
        return -1;
    }

    void addToOpenList(int visiting, const Coord& fromCoord, const Coord& toCoord) {
        if (closed[grid.idx(toCoord)]) return;
        PathItem pi;
        pi.coord = toCoord;
        pi.cumulativeLength = items[visiting].cumulativeLength + 1; // default weight function
        int manh = fromCoord.manhattanTo(toCoord);
        pi.totalPrevisionalLength = pi.cumulativeLength + manh;
        pi.precedent = visiting;
        items.push_back(pi);
        openList.offer((int)items.size() - 1);
    }

    std::vector<Coord> find() {
        int item = getPathItemLinkedList();
        std::vector<Coord> path;
        for (int i = item; i >= 0; i = items[i].precedent) path.push_back(items[i].coord);
        std::reverse(path.begin(), path.end());
        return path;
    }
};

// PathFinder.findPath(): the path to the target, or to the visited coord nearest to it
static std::vector<Coord> findPath(const Grid& grid, const std::vector<char>& restricted, Coord from, Coord to) {
    AStar a(grid, from, to, restricted);
    std::vector<Coord> path = a.find();
    if (path.empty()) {
        AStar b(grid, from, a.nearest, restricted);
        path = b.find();
    }
    return path;
}

// ----------------------------------------------------------------------------
// Player.java, Action.java
// ----------------------------------------------------------------------------
enum ActionType { A_MOVE, A_SPAWN, A_BUILD, A_MESSAGE, A_WAIT };

struct Action {
    ActionType type;
    i32 amount = 0;
    Coord coord{0, 0};
    Coord originCoord{0, 0};
};

struct Player : cg::Player {
    int money = 0;
    int warpCooldown = 0;
    std::vector<Unit> units; // per cell
    bool hasMessage = false;
    jstring message;
    std::vector<Action> builds, spawns, moves;

    jstring nicknameToken() const { return java::ascii("$" + std::to_string(index)); }
    int getExpectedOutputLines() const override { return 1; }

    void setMessage(const jstring& msg) {
        jstring trimmed = java::trim(msg);
        if (trimmed.size() > 48) trimmed = trimmed.substr(0, 46) + u"...";
        if (trimmed.size() > 0) {
            message = trimmed;
            hasMessage = true;
        }
    }
    void reset() {
        hasMessage = false;
        message.clear();
        moves.clear();
        builds.clear();
        spawns.clear();
    }
    void addAction(const Action& a, const jstring& msg) {
        switch (a.type) {
        case A_BUILD: builds.push_back(a); break;
        case A_MOVE: moves.push_back(a); break;
        case A_SPAWN: spawns.push_back(a); break;
        case A_MESSAGE: setMessage(msg); break;
        default: break;
        }
    }
};

// ----------------------------------------------------------------------------
// View (ViewModule, Serializer, Animation, EventData) - only its size matters, for the engine limits
// ----------------------------------------------------------------------------
namespace Ev {
const int BUILD = 0, MOVE = 1, SPAWN = 3, FIGHT = 4, CELL_DAMAGE = 7, RECYCLER_FALL = 8, CELL_OWNER_SWAP = 9,
          UNIT_FALL = 10, MATTER_COLLECT = 11;
const int NONE = INT_MIN;
const int HUNDREDTH = 10, TWENTIETH = 50, TENTH = 100, THIRD = 300, HALF = 500, WHOLE = 1000;
} // namespace Ev

struct Animation {
    int frameTime = 0, endTime = 0;
    void reset() { frameTime = 0; endTime = 0; }
    void wait(int t) { frameTime += t; }
    void startAnim(int& start, int& end, int duration) {
        start = frameTime;
        end = frameTime + duration;
        endTime = std::max(endTime, frameTime + duration);
    }
    void setFrameTime(int t) { frameTime = t; }
    void catchUp() { frameTime = endTime; }
};

struct EventData {
    int type;
    int playerIndex = Ev::NONE, amount = Ev::NONE;
    bool hasCoord = false, hasTarget = false;
    Coord coord{0, 0}, target{0, 0};
    int start = 0, end = 0;
};

// The view data of a frame (GameManager's JsonObject for the frame)
struct FrameView {
    bool hasDuration = false;
    int duration = 0;
    bool hasGraphics = false;
    // Serializer.serialize(FrameViewData) is graphics[0] + message 0 + graphics[1] + message 1 + graphics[2]
    std::string graphics[3];
    bool hasMessage[2] = {false, false};
    jstring messages[2];
    bool hasEndScreen = false;
    int endScores[2] = {0, 0};

    // the frame JSON (JsonObject.toString), without the messages unless `withMessages`
    std::string json(bool withMessages) const {
        std::string s = "{";
        bool first = true;
        if (hasDuration) {
            s += "\"duration\":" + std::to_string(duration);
            first = false;
        }
        if (hasGraphics) {
            if (!first) s += ",";
            s += "\"graphics\":\"" + graphics[0];
            if (withMessages && hasMessage[0]) s += java::jsonEscape(messages[0]);
            s += graphics[1];
            if (withMessages && hasMessage[1]) s += java::jsonEscape(messages[1]);
            s += graphics[2] + "\"";
            first = false;
        }
        if (hasEndScreen) {
            if (!first) s += ",";
            s += "\"endScreen\":[[" + std::to_string(endScores[0]) + "," + std::to_string(endScores[1]) + "],\"logo.png\",null]";
        }
        s += "}";
        return s;
    }
    // size of the escaped messages in the JSON, in UTF-16 chars and in UTF-8 bytes
    void messageLength(i64& chars, i64& bytes) const {
        for (int i = 0; i < 2; i++)
            if (hasMessage[i]) {
                java::Size size = java::jsonEscapeSize(messages[i]);
                chars += size.u16;
                bytes += size.u8;
            }
    }
};

// ----------------------------------------------------------------------------
// Game.java, GameSummaryManager.java
// ----------------------------------------------------------------------------
struct Game {
    java::SHA1PRNG& random = cg::gm.getRandom(); // seeded by the engine
    Grid grid;
    std::vector<Recycler> recyclers;
    std::vector<char> recyclerAt; // per cell: a recycler is there (getRecyclerAt(coord).isPresent())
    Player players[2];
    std::vector<Coord> fightLocations; // HashSet<Coord>: iteration order does not matter here
    std::vector<char> isFightLocation;
    std::vector<EventData> viewerEvents;
    Animation animation;
    int gameTurn = 0;
    int earlyFinishCounter = C::EARLY_FINISH_TURNS;
    // GameSummaryManager: first error and error count per player
    std::string firstError[2];
    int errorCount[2] = {0, 0};

    int cellCount() const { return grid.width * grid.height; }

    void init() {
        grid.generate(random, 2);
        recyclers.clear();
        recyclerAt.assign(cellCount(), 0);
        isFightLocation.assign(cellCount(), 0);
        gameTurn = 0;
        for (Player& player : players) {
            player.units.assign(cellCount(), Unit());
            player.money = C::PLAYER_STARTING_MONEY;
            for (const Coord& coord : grid.spawns[player.index]) {
                Unit& u = player.units[grid.idx(coord)];
                u.valid = true;
                u.availableCount = 1;
                u.unavailableCount = 0;
            }
        }
    }

    void resetEarlyTurnCounter() { earlyFinishCounter = C::EARLY_FINISH_TURNS; }

    void resetGameTurnData() {
        viewerEvents.clear();
        animation.reset();
        for (Player& p : players) p.reset();
        for (const Coord& c : fightLocations) isFightLocation[grid.idx(c)] = 0;
        fightLocations.clear();
    }

    void addFightLocation(const Coord& c) {
        if (!isFightLocation[grid.idx(c)]) {
            isFightLocation[grid.idx(c)] = 1;
            fightLocations.push_back(c);
        }
    }

    const Unit& getUnitAt(const Player& p, const Coord& c) const {
        static const Unit NO_UNIT;
        return grid.isValid(c) ? p.units[grid.idx(c)] : NO_UNIT;
    }

    bool hasRecyclerAt(const Coord& c) const { return grid.isValid(c) && recyclerAt[grid.idx(c)]; }

    void addError(int player, const std::string& error) {
        if (errorCount[player]++ == 0) firstError[player] = error;
    }

    std::string getGlobalInfoFor() const { return std::to_string(grid.width) + " " + std::to_string(grid.height) + "\n"; }

    std::string getCurrentFrameInfoFor(const Player& player) const {
        std::vector<char> inRange(cellCount(), 0);
        for (const Recycler& r : recyclers) {
            Coord n[4];
            int nn = grid.getNeighbours(r.coord, n);
            n[nn++] = r.coord;
            for (int i = 0; i < nn; i++)
                if (!grid.get(n[i]).isHole()) inRange[grid.idx(n[i])] = 1;
        }
        const Player& other = players[1 - player.index];
        std::string out;
        out.reserve(cellCount() * 16 + 16);
        out += std::to_string(player.money) + " " + std::to_string(other.money) + "\n";
        char buf[96];
        for (int y = 0; y < grid.height; ++y) {
            for (int x = 0; x < grid.width; ++x) {
                Coord coord = {x, y};
                const Cell& c = grid.cells[grid.idx(coord)];
                int ownerIdx = c.owner < 0 ? -1 : (c.owner == player.index ? 1 : 0);
                const Unit& myUnit = player.units[grid.idx(coord)];
                const Unit& foeUnit = other.units[grid.idx(coord)];
                bool excavator = recyclerAt[grid.idx(coord)];
                int unitStrength = ownerIdx == 1 ? myUnit.availableCount : foeUnit.availableCount;
                bool canBuildHere = ownerIdx == 1 && !excavator && unitStrength == 0;
                bool canSpawnHere = ownerIdx == 1 && !excavator;
                bool willGetDamaged = inRange[grid.idx(coord)];
                snprintf(buf, sizeof(buf), "%d %d %d %d %d %d %d\n", c.durability, ownerIdx, unitStrength, excavator ? 1 : 0,
                         canBuildHere ? 1 : 0, canSpawnHere ? 1 : 0, willGetDamaged ? 1 : 0);
                out += buf;
            }
        }
        return out;
    }

    void performGameUpdate(int frameIdx);

    int getOwnedCells(int player) const {
        int n = 0;
        for (const Cell& c : grid.cells)
            if (c.owner == player) n++;
        return n;
    }

    // events (only their size matters)
    void launchEvent(int type, int playerIndex, int amount, const Coord* coord, const Coord* target, int duration, int wait) {
        EventData e;
        e.type = type;
        e.playerIndex = playerIndex;
        e.amount = amount;
        if (coord) { e.hasCoord = true; e.coord = *coord; }
        if (target) { e.hasTarget = true; e.target = *target; }
        animation.startAnim(e.start, e.end, duration);
        if (wait) animation.wait(wait);
        viewerEvents.push_back(e);
    }

    void doBuilds() {
        int buildStartTime = animation.frameTime;
        for (Player& player : players) { // all players, also inactive ones
            animation.setFrameTime(buildStartTime);
            for (const Action& build : player.builds) {
                Coord t = build.coord;
                char buf[160];
                if (!grid.isOwner(t, player.index)) {
                    snprintf(buf, sizeof(buf), "tried to build a recycler at (%d, %d), which is not owned by the player", t.x, t.y);
                    addError(player.index, buf);
                } else if (hasRecyclerAt(t)) {
                    snprintf(buf, sizeof(buf), "tried to build a recycler at (%d, %d), into another recycler", t.x, t.y);
                    addError(player.index, buf);
                } else if (getUnitAt(players[0], t).valid || getUnitAt(players[1], t).valid) {
                    snprintf(buf, sizeof(buf), "tried to build a recycler at (%d, %d), where units are already present", t.x, t.y);
                    addError(player.index, buf);
                } else if (player.money < C::COST_RECYCLER) {
                    snprintf(buf, sizeof(buf), "tried to build a recycler at (%d, %d), but has not enough matter", t.x, t.y);
                    addError(player.index, buf);
                } else {
                    recyclers.push_back({t, player.index});
                    recyclerAt[grid.idx(t)] = 1;
                    player.money -= C::COST_RECYCLER;
                    launchEvent(Ev::BUILD, player.index, Ev::NONE, &build.coord, nullptr, 2 * Ev::THIRD, Ev::THIRD);
                }
            }
        }
        animation.catchUp();
    }

    void placeUnits(Player& p, const Coord& target, int amount) {
        Unit& u = p.units[grid.idx(target)];
        u.valid = true;
        u.unavailableCount += amount;
    }

    void doSpawn(const std::vector<int>& active) {
        int spawnStartTime = animation.frameTime;
        for (int pi : active) {
            Player& player = players[pi];
            animation.setFrameTime(spawnStartTime);
            std::vector<std::pair<Coord, int>> actualSpawns; // HashMap: iteration order does not matter here
            for (const Action& spawn : player.spawns) {
                double spawnCost = ((double)C::COST_UNIT) * spawn.amount;
                Coord t = spawn.coord;
                char buf[200];
                if (!grid.isOwner(t, player.index)) {
                    snprintf(buf, sizeof(buf), "tried to spawn %d units at (%d, %d), which is not owned by the player", spawn.amount, t.x, t.y);
                    addError(player.index, buf);
                } else if (hasRecyclerAt(t)) {
                    snprintf(buf, sizeof(buf), "tried to spawn %d units at (%d, %d), into a recycler", spawn.amount, t.x, t.y);
                    addError(player.index, buf);
                } else if (player.money < spawnCost) {
                    snprintf(buf, sizeof(buf), "tried to spawn %d units at (%d, %d), but has not enough matter", spawn.amount, t.x, t.y);
                    addError(player.index, buf);
                } else if (spawn.amount == 0) {
                    snprintf(buf, sizeof(buf), "tried to spawn a null amount of units at (%d, %d)", t.x, t.y);
                    addError(player.index, buf);
                } else {
                    placeUnits(player, t, spawn.amount);
                    player.money = (int)(player.money - spawnCost);
                    bool found = false;
                    for (auto& s : actualSpawns)
                        if (s.first == t) { s.second += spawn.amount; found = true; }
                    if (!found) actualSpawns.push_back({t, spawn.amount});
                }
            }
            for (auto& s : actualSpawns) {
                launchEvent(Ev::SPAWN, player.index, s.second, &s.first, nullptr, 2 * Ev::THIRD, Ev::TENTH);
                addFightLocation(s.first);
            }
        }
    }

    void doMove(const std::vector<int>& active) {
        // restricted: the coords with a recycler (no recycler is built or destroyed during doMove)
        int moveStartTime = animation.frameTime;
        for (int pi : active) {
            Player& player = players[pi];
            animation.setFrameTime(moveStartTime);
            std::vector<std::pair<std::pair<Coord, Coord>, int>> actualMoves; // HashMap<CoordTuple, Integer>
            for (const Action& move : player.moves) {
                Coord origin = move.originCoord;
                Coord target = move.coord;
                const Unit& originUnit = getUnitAt(player, origin);
                char buf[200];
                if (originUnit.availableCount < move.amount) {
                    snprintf(buf, sizeof(buf), "tried to move %d units from (%d, %d) where only %d were available", move.amount, origin.x, origin.y,
                             originUnit.availableCount);
                    addError(player.index, buf);
                } else if (origin == target) {
                    snprintf(buf, sizeof(buf), "tried to move %d units from (%d, %d) to the same tile", move.amount, origin.x, origin.y);
                    addError(player.index, buf);
                } else if (move.amount <= 0) {
                    snprintf(buf, sizeof(buf), "tried to move a null amount of units from (%d, %d)", origin.x, origin.y);
                    addError(player.index, buf);
                } else {
                    std::vector<Coord> wholePath = findPath(grid, recyclerAt, origin, target);
                    if (wholePath.size() > 1) {
                        Coord step = wholePath[1];
                        player.units[grid.idx(origin)].availableCount -= move.amount;
                        placeUnits(player, step, move.amount);
                        bool found = false;
                        for (auto& m : actualMoves)
                            if (m.first.first == origin && m.first.second == step) { m.second += move.amount; found = true; }
                        if (!found) actualMoves.push_back({{origin, step}, move.amount});
                    }
                }
            }
            for (auto& m : actualMoves) {
                launchEvent(Ev::MOVE, player.index, m.second, &m.first.first, &m.first.second, Ev::HALF, Ev::TWENTIETH);
                addFightLocation(m.first.second);
            }
        }
    }

    void removeUnits(Player& p, const Coord& coord, int n) {
        if (n == 0) return;
        Unit& u = p.units[grid.idx(coord)];
        if (!u.valid) return; // compute() on an absent key with a null result: nothing
        if (u.availableCount - n <= 0) u = Unit();
        else u.availableCount -= n;
    }

    void doFights() {
        for (const Coord& coord : fightLocations) {
            Player& player = players[0];
            Player& other = players[1];
            int playerUnitStrength = getUnitAt(player, coord).availableCount;
            int otherUnitStrength = getUnitAt(other, coord).availableCount;
            if (playerUnitStrength > 0 && otherUnitStrength > 0) {
                int winner = 2;
                if (playerUnitStrength > otherUnitStrength) winner = player.index;
                else if (otherUnitStrength > playerUnitStrength) winner = other.index;
                launchEvent(Ev::FIGHT, winner, std::abs(playerUnitStrength - otherUnitStrength), &coord, nullptr, Ev::THIRD, 0);
            }
            removeUnits(player, coord, otherUnitStrength);
            removeUnits(other, coord, playerUnitStrength);
            Cell& cell = grid.at(coord);
            if (playerUnitStrength > otherUnitStrength && cell.owner != player.index) {
                cell.owner = player.index;
                launchEvent(Ev::CELL_OWNER_SWAP, player.index, Ev::NONE, &coord, nullptr, Ev::THIRD, 0);
                resetEarlyTurnCounter();
            } else if (otherUnitStrength > playerUnitStrength && cell.owner != other.index) {
                cell.owner = other.index;
                launchEvent(Ev::CELL_OWNER_SWAP, other.index, Ev::NONE, &coord, nullptr, Ev::THIRD, 0);
                resetEarlyTurnCounter();
            }
        }
    }

    void resetUnits() {
        for (Player& player : players) {
            for (Unit& u : player.units) {
                if (!u.valid) continue;
                u.availableCount += u.unavailableCount;
                u.unavailableCount = 0;
                if (u.availableCount <= 0) u = Unit();
            }
        }
    }

    void doUnits(const std::vector<int>& active) {
        doSpawn(active);
        animation.catchUp();
        doMove(active);
        animation.catchUp();
        resetUnits();
        doFights();
    }

    void doRecycle() {
        std::vector<char> excavated[2] = {std::vector<char>(cellCount(), 0), std::vector<char>(cellCount(), 0)};
        std::vector<Coord> excavatedList[2];
        for (const Recycler& e : recyclers) {
            int nb = 0;
            Coord coords[5];
            int nc = grid.getNeighbours(e.coord, coords);
            coords[nc++] = e.coord;
            for (int i = 0; i < nc; i++) {
                const Coord& coord = coords[i];
                if (!grid.get(coord).isHole()) {
                    if (!excavated[e.owner][grid.idx(coord)]) {
                        nb++;
                        excavated[e.owner][grid.idx(coord)] = 1;
                        excavatedList[e.owner].push_back(coord);
                    }
                }
            }
            if (nb > 0) launchEvent(Ev::MATTER_COLLECT, e.owner, nb * C::RECYCLER_INCOME, &e.coord, nullptr, Ev::WHOLE, Ev::HUNDREDTH);
        }
        for (Player& player : players) player.money += (int)excavatedList[player.index].size() * C::RECYCLER_INCOME;

        std::vector<char> destroyed(cellCount(), 0);
        std::vector<Coord> destroyedCells;
        std::vector<char> done(cellCount(), 0); // distinct()
        for (int p = 0; p < 2; p++) {
            for (const Coord& coord : excavatedList[p]) {
                if (done[grid.idx(coord)]) continue;
                done[grid.idx(coord)] = 1;
                Cell& cell = grid.at(coord);
                bool broken = cell.damage();
                launchEvent(Ev::CELL_DAMAGE, Ev::NONE, cell.durability, &coord, nullptr, Ev::HALF, Ev::HUNDREDTH);
                if (broken) {
                    destroyedCells.push_back(coord);
                    destroyed[grid.idx(coord)] = 1;
                }
                resetEarlyTurnCounter();
            }
        }

        animation.wait(Ev::THIRD);
        animation.catchUp();

        for (const Recycler& r : recyclers)
            if (destroyed[grid.idx(r.coord)]) launchEvent(Ev::RECYCLER_FALL, r.owner, Ev::NONE, &r.coord, nullptr, Ev::THIRD, Ev::TWENTIETH);
        recyclers.erase(std::remove_if(recyclers.begin(), recyclers.end(), [&](const Recycler& r) { return destroyed[grid.idx(r.coord)] != 0; }),
                        recyclers.end());
        for (const Coord& c : destroyedCells) recyclerAt[grid.idx(c)] = 0;

        for (Player& p : players) {
            for (const Coord& coord : destroyedCells) {
                Unit& u = p.units[grid.idx(coord)];
                if (u.valid && u.availableCount > 0) {
                    int strength = u.availableCount;
                    u = Unit();
                    launchEvent(Ev::UNIT_FALL, p.index, strength, &coord, nullptr, Ev::THIRD, Ev::TWENTIETH);
                }
            }
        }
    }

    void doPassiveIncome() {
        for (Player& p : players) p.money += C::PLAYER_MINIMAL_INCOME;
    }

    bool checkGameOver();
    void computeEvents();

    std::vector<int> getActivePlayers() const {
        std::vector<int> r;
        for (const Player& p : players)
            if (p.active) r.push_back(p.index);
        return r;
    }

    void onEnd() {
        for (Player& p : players) p.score = p.active ? getOwnedCells(p.index) : -1;
    }

    // Serializer.serialize(GlobalViewData)
    std::string serializeGlobal() const {
        std::string s = std::to_string(grid.width) + ";" + std::to_string(grid.height);
        for (int x = 0; x < grid.width; x++)
            for (int y = 0; y < grid.height; y++) {
                const Cell& c = grid.cells[grid.idx({x, y})];
                s += ";" + std::to_string(c.durability) + " " + std::to_string(c.owner);
            }
        for (const Player& p : players) {
            std::string list;
            int n = 0;
            for (int y = 0; y < grid.height; y++)
                for (int x = 0; x < grid.width; x++) {
                    const Unit& u = p.units[grid.idx({x, y})];
                    if (!u.valid) continue;
                    n++;
                    list += ";" + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(u.availableCount);
                }
            s += ";" + std::to_string(n) + list;
        }
        return s;
    }

    // Serializer.serialize(FrameViewData), messages apart
    void serializeFrame(FrameView& f) const {
        for (int i = 0; i < 2; i++) {
            const Player& p = players[i];
            f.graphics[i] = (i ? ";" : "") + std::to_string(p.money) + " " + std::to_string(p.warpCooldown) + " ";
            f.hasMessage[i] = p.hasMessage;
            f.messages[i] = p.message;
        }
        std::string s = ";" + std::to_string(viewerEvents.size());
        for (const EventData& e : viewerEvents) {
            s += ";";
            if (e.playerIndex != Ev::NONE) s += std::to_string(e.playerIndex);
            s += ";";
            if (e.amount != Ev::NONE) s += std::to_string(e.amount);
            s += ";";
            if (e.hasCoord) s += std::to_string(e.coord.x) + " " + std::to_string(e.coord.y);
            s += ";";
            if (e.hasTarget) s += std::to_string(e.target.x) + " " + std::to_string(e.target.y);
            s += ";" + std::to_string(e.type) + ";" + std::to_string(e.start) + ";" + std::to_string(e.end);
        }
        f.graphics[2] = s;
        f.hasGraphics = true;
    }
};

static Game game;
static Player extraPlayers[2]; // -p3 / -p4: the Java referee fails in init

// The view data of a frame for the engine: Serializer.serialize(FrameViewData) as a JSON string
static cg::Json graphicsJson(const FrameView& f) {
    java::Size size;
    size.ascii(2 + (i64)(f.graphics[0].size() + f.graphics[1].size() + f.graphics[2].size()));
    f.messageLength(size.u16, size.u8);
    return cg::Json::sized(size);
}

// GameManager.execute input: one line per sendInputLine
static void sendInput(Player& player, const std::string& text) {
    size_t off = 0, nl;
    while ((nl = text.find('\n', off)) != std::string::npos) {
        player.sendInputLine(text.substr(off, nl - off));
        off = nl + 1;
    }
}

// ----------------------------------------------------------------------------
// Referee.java / CommandManager.java
// ----------------------------------------------------------------------------
// Game.getExpected
static jstring getExpected(const jstring& playerOutput) {
    auto starts = [&](const char* k) { return java::upperCaseStartsWith(playerOutput, k); };
    if (starts("MOVE")) return u"MOVE <n> <x1> <y1> <x2> <y2>";
    if (starts("SPAWN")) return u"SPAWN <n> <x> <y>";
    if (starts("BUILD")) return u"BUILD <x> <y>";
    if (starts("MESSAGE")) return u"MESSAGE <text>";
    if (starts("WAIT")) return u"WAIT";
    return u"MOVE | SPAWN | BUILD | MESSAGE | WAIT";
}

// ActionType patterns with Matcher.matches(). Returns 0: no match, 1: action, 2: Integer.valueOf threw
// (nfe = the string that failed)
static int matchCommand(const jstring& s, Action& action, jstring& message, jstring& nfe) {
    size_t pos = 0;
    auto lit = [&](const char16_t* k) {
        size_t n = std::char_traits<char16_t>::length(k);
        if (s.compare(pos, n, k) != 0 || pos + n > s.size()) return false;
        pos += n;
        return true;
    };
    // group: (-)?\d+ followed by ' ' (or the end if last)
    auto number = [&](bool allowMinus, bool last, jstring& out) {
        size_t start = pos;
        if (allowMinus && pos < s.size() && s[pos] == u'-') pos++;
        size_t d = pos;
        while (pos < s.size() && s[pos] >= u'0' && s[pos] <= u'9') pos++;
        if (pos == d) return false;
        out = s.substr(start, pos - start);
        if (last) return pos == s.size();
        if (pos >= s.size() || s[pos] != u' ') return false;
        pos++;
        return true;
    };
    jstring g[5];
    int n = 0;
    ActionType type;
    if (lit(u"MOVE ") || (pos = 0, lit(u"WARP "))) {
        type = A_MOVE;
        if (!number(false, false, g[0]) || !number(false, false, g[1]) || !number(false, false, g[2]) || !number(true, false, g[3]) ||
            !number(true, true, g[4]))
            return 0;
        n = 5;
    } else if ((pos = 0, lit(u"SPAWN "))) {
        type = A_SPAWN;
        if (!number(false, false, g[0]) || !number(true, false, g[1]) || !number(true, true, g[2])) return 0;
        n = 3;
    } else if ((pos = 0, lit(u"BUILD "))) {
        type = A_BUILD;
        if (!number(true, false, g[0]) || !number(true, true, g[1])) return 0;
        n = 2;
    } else if ((pos = 0, lit(u"MESSAGE "))) {
        // '.' does not match line terminators, but the referee's line has none (see execute)
        action.type = A_MESSAGE;
        message = s.substr(pos);
        return 1;
    } else if (s == u"WAIT") {
        action.type = A_WAIT;
        return 1;
    } else {
        return 0;
    }
    i32 v[5];
    for (int i = 0; i < n; i++) {
        if (!java::parseInt(g[i], v[i])) {
            nfe = g[i];
            return 2;
        }
    }
    action.type = type;
    if (type == A_MOVE) {
        action.amount = v[0];
        action.originCoord = {v[1], v[2]};
        action.coord = {v[3], v[4]};
    } else if (type == A_SPAWN) {
        action.amount = v[0];
        action.coord = {v[1], v[2]};
    } else {
        action.coord = {v[0], v[1]};
    }
    return 1;
}

// CommandManager.parseCommands
static void parseCommands(Player& player, const jstring& line) {
    std::vector<jstring> commands = java::split(line, u';');
    for (const jstring& command : commands) {
        Action action;
        jstring message, nfe;
        int r = matchCommand(java::trim(command), action, message, nfe);
        if (r == 1) {
            player.addAction(action, message);
            continue;
        }
        jstring got = r == 2 ? java::numberFormatException(nfe) : command;
        jstring msg = u"Invalid Input: Expected " + getExpected(command) + u" but got '" + got + u"'";
        // deactivatePlayer
        player.deactivate(java::replace(java::replace(msg, u"&lt;", u"<"), u"&gt;", u">"));
        player.score = -1;
        cg::gm.addToGameSummary(msg);
        cg::gm.addToGameSummary(u"¤RED¤" + player.nicknameToken() + u": disqualified!§RED§");
        return;
    }
}

static void handlePlayerCommands(const std::vector<int>& active) {
    for (int pi : active) {
        Player& player = game.players[pi];
        if (!player.timeout) {
            parseCommands(player, player.outputs[0]);
        } else {
            player.deactivate(u"Timeout!");
            cg::gm.addToGameSummary(player.nicknameToken() + u" has not provided 1 lines in time");
        }
    }
}

struct KotgReferee : cg::Referee {
    void init() override {
        if (cg::gm.getPlayerCount() > 2) throw java::Exception{java::arrayIndexOutOfBoundsException(2, 2)};
        game.init();
        for (int pi : game.getActivePlayers()) sendInput(game.players[pi], game.getGlobalInfoFor());
        cg::gm.setFrameDuration(500);
        cg::gm.setTurnMaxTime(C::TURN_MAX_TIME);
        cg::gm.setFirstTurnMaxTime(C::FIRST_TURN_MAX_TIME);
        cg::gm.setMaxTurns(C::MAX_TURNS);
    }
    void gameTurn(int turn) override {
        game.resetGameTurnData();
        std::vector<int> active = game.getActivePlayers();
        for (int pi : active) {
            Player& player = game.players[pi];
            sendInput(player, game.getCurrentFrameInfoFor(player));
            player.execute();
            if (!player.timeout) {
                // the referee reads the line with Scanner.nextLine(), which also ends at U+0085, U+2028 and U+2029
                // (the engine already cut it there)
            }
        }
        handlePlayerCommands(active);
        game.performGameUpdate(turn);
        if (game.getActivePlayers().size() < 2) cg::gm.endGame();
        if (cg::trace)
            fprintf(cg::trace, "STATE %d money %d %d cells %d %d active %d %d\n", turn, game.players[0].money, game.players[1].money, game.getOwnedCells(0),
                    game.getOwnedCells(1), game.players[0].active, game.players[1].active);
    }
    void onEnd() override { game.onEnd(); }
    // ViewModule
    void onGameInit() override {
        cg::gm.setViewGlobalData("graphics", cg::Json::raw("\"" + game.serializeGlobal() + "\""));
        FrameView f;
        game.serializeFrame(f);
        cg::gm.setViewData("graphics", graphicsJson(f));
    }
    void onAfterGameTurn() override {
        FrameView f;
        game.serializeFrame(f);
        cg::gm.setViewData("graphics", graphicsJson(f));
    }
    // EndScreenModule
    void onAfterOnEnd() override {
        cg::gm.setViewData("endScreen", cg::Json::raw("[[" + std::to_string(game.players[0].score) + "," + std::to_string(game.players[1].score) +
                                                      "],\"logo.png\",null]"));
    }
};

// Game.performGameUpdate and the parts that talk to the engine
void Game::performGameUpdate(int) {
    std::vector<int> active = getActivePlayers();
    doBuilds();
    doUnits(active);
    animation.catchUp();
    doRecycle();

    gameTurn++;
    earlyFinishCounter--;
    doPassiveIncome();
    if (checkGameOver()) cg::gm.endGame();

    // gameSummaryManager.toString(): formatErrors() + "\n"
    std::string summary;
    for (int p = 0; p < 2; p++) {
        if (!errorCount[p]) continue;
        if (!summary.empty()) summary += "\n";
        summary += "$" + std::to_string(p) + ": " + firstError[p];
        if (errorCount[p] > 1) summary += " + " + std::to_string(errorCount[p] - 1) + " other error" + (errorCount[p] > 2 ? "s" : "");
    }
    summary += "\n";
    cg::gm.addToGameSummary(java::ascii(summary));
    errorCount[0] = errorCount[1] = 0;

    computeEvents();
}

bool Game::checkGameOver() {
    auto winner = [&]() { return getOwnedCells(1) > getOwnedCells(0) ? 1 : 0; }; // Stream.max keeps the first on ties
    if (earlyFinishCounter <= 0) {
        int w = winner();
        cg::gm.addTooltip(w, java::ascii("Field stable for 20 turns. $" + std::to_string(w) + " wins!"));
        return true;
    }
    for (Player& p : players) {
        if (getOwnedCells(p.index) == 0) {
            cg::gm.addTooltip(winner(), java::ascii("$" + std::to_string(p.index) + " has no more cells and is disqualified"));
            return true;
        }
    }
    return false;
}

void Game::computeEvents() {
    int minTime = 1000;
    animation.catchUp();
    int frameTime = std::max(animation.frameTime, minTime);
    cg::gm.setFrameDuration(frameTime);
}

int main(int argc, char** argv) {
    static KotgReferee referee;
    cg::Options options;
    options.agent.firstTurnHeadStartMs = C::AGENT_HEAD_START;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return i < 2 ? (cg::Player*)&game.players[i] : &extraPlayers[i - 2]; }, options);
}
