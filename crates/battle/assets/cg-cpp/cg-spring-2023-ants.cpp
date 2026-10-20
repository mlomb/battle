// Spring Challenge 2023 (Ants) - headless C++ referee, port of
// https://github.com/CodinGame/SpringChallenge2023 (commit dd151f9) on CodinGame game engine 4.4.2.
// The CLI runs league level 19, so the highest league rules apply: up to 2 anthills per player,
// eggs, scores in the turn input, ants cut off by stronger enemy chains can't carry.
// Deterministic; reproduces the same game as the reference jar for a given -seed (2 players).
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <trace file>]
// (common/engine.h: the engine, the command line and the trace; CG_TRACE_VIEWS=1 also traces the
// text of every view frame)
// Prints "<p0score>\n<p1score>\n" to stdout (matches the jar), or "null\nnull\n" when the Java
// game runner gives up on a failed referee (see below).
//
// The Java referee is followed closely: same data structures, iteration orders and library
// semantics. Notable Java behaviours reproduced here:
//   - gameManager.getRandom(): SecureRandom("SHA1PRNG") seeded with the seed; nextInt(bound),
//     nextDouble, nextBoolean and Collections.shuffle on top of it (seed 0 leaves the jar's
//     SecureRandom unseeded, i.e. not reproducible; we then use a time based seed too)
//   - Board.getBestPath: java.util.PriorityQueue (binary heap) whose comparator reads arrays that
//     are modified while the elements are queued; the heap is replicated operation by operation
//   - stable sorts (List.sort / stream().sorted()) and the BFS neighbour orders
//   - HashMap iteration orders (map.values() for food/egg cells, HashMap<AntMove,Integer> in
//     doMove, GameSummaryManager) only change the order of viewer events and summary lines: every
//     state update they drive commutes, so they are iterated in cell index order here
//   - engine I/O (Agent#getOutput): '\r' and '\n' terminate lines ("\r\n" counts once, even across
//     turns), 10000 bytes per turn max, 1000ms for the first turn of each player and 100ms after
//     (wall clock; the first turn also ends no earlier than 1.6 s after the agent was spawned, like
//     the jar whose JVM initializes the referee after spawning the agents); the output is decoded
//     like Java's new String(bytes, UTF_8) (invalid bytes -> U+FFFD)
//   - command parsing: String.split(";"), trim(), the ActionType regexes (matches(), ASCII \d,
//     '.' without line terminators), Integer.parseInt overflow -> disqualification, the
//     MESSAGE trimming/truncation (UTF-16 lengths), String.toUpperCase in the error message
//   - the referee reads its input with java.util.Scanner, whose nextLine() also ends lines at
//     U+0085, U+2028 and U+2029: a player line containing one is cut there, and the referee then
//     dies on the next input it reads ("Error in data sent to referee")
//   - the game runner gives up (no scores: the CLI prints "null" per player and exits 0) when the
//     referee fails: total view data above 1 MB (GameManager.dumpView, counted in UTF-16 units,
//     header included, line.separator is "\r\n" on Windows), or one referee command (a frame of
//     view data, the game summary or the tooltips of the last turn) above the runner's 30000
//     byte buffer. The view data is rebuilt here exactly (gson 2.8.2 escaping) to count it.
#include "common/engine.h"

#include <deque>

typedef std::u16string ustr; // java.lang.String (UTF-16 code units)

// ----------------------------------------------------------------------------
// Java semantics (java.h has the rest)
// ----------------------------------------------------------------------------
namespace java {

// Integer.compare
static inline int icmp(int a, int b) { return a < b ? -1 : (a == b ? 0 : 1); }

inline ustr num(i64 v) { return str(v); }
// UTF-8 encoding of a Java string as a PrintStream writes it (unpaired surrogates become '?')
inline void appendUtf8(std::string& out, const ustr& s) { out += encodeUtf8(s); }
inline std::string utf8(const ustr& s) { return encodeUtf8(s); }

// String.split(String) with a single, non special character (fast path, trailing empties removed)
static std::vector<ustr> split(const ustr& s, char16_t c) {
    std::vector<ustr> out;
    size_t off = 0, next;
    bool found = false;
    while ((next = s.find(c, off)) != ustr::npos) {
        found = true;
        out.push_back(s.substr(off, next - off));
        off = next + 1;
    }
    if (!found) return {s};
    out.push_back(s.substr(off));
    size_t n = out.size();
    while (n > 0 && out[n - 1].empty()) n--;
    out.resize(n);
    return out;
}

static bool startsWith(const ustr& s, const char* prefix) {
    size_t n = strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] != (char16_t)(unsigned char)prefix[i]) return false;
    return true;
}

// The first `n` UTF-16 units of s.toUpperCase() (locale en_US), enough for startsWith checks
// against ASCII words. Characters whose upper case contains no ASCII letter become U+FFFF.
static ustr upperPrefix(const ustr& s, size_t n) {
    static const struct {
        char16_t c;
        const char16_t* up;
    } special[] = {
        {0x00DF, u"SS"}, {0x0131, u"I"}, {0x0149, u"ʼN"}, {0x017F, u"S"}, {0x01F0, u"J̌"},
        {0x1E96, u"H̱"}, {0x1E97, u"T̈"}, {0x1E98, u"W̊"}, {0x1E99, u"Y̊"},
        {0x1E9A, u"Aʾ"}, {0xFB00, u"FF"}, {0xFB01, u"FI"}, {0xFB02, u"FL"}, {0xFB03, u"FFI"},
        {0xFB04, u"FFL"}, {0xFB05, u"ST"}, {0xFB06, u"ST"},
    };
    ustr out;
    for (size_t i = 0; i < s.size() && out.size() < n; i++) {
        char16_t c = s[i];
        if (c < 0x80) {
            out.push_back((c >= u'a' && c <= u'z') ? (char16_t)(c - 32) : c);
            continue;
        }
        const char16_t* up = nullptr;
        for (auto& sp : special)
            if (sp.c == c) up = sp.up;
        if (up) {
            while (*up) out.push_back(*up++);
        } else {
            out.push_back(0xFFFF);
        }
    }
    if (out.size() > n) out.resize(n);
    return out;
}

} // namespace java

// ----------------------------------------------------------------------------
// Game logic: Config.java, CubeCoord.java, Cell.java
// ----------------------------------------------------------------------------
namespace Config {
// league 19 (Referee.init): no league specific overrides apply below level 4 and SCORES_IN_IO is
// set; Config.takeFrom() finds none of its keys in the game parameters (only "seed")
const int MAP_RING_COUNT_MIN = 4;
const int MAP_RING_COUNT_MAX = 7;
const int MIN_EMPTY_CELLS_PERCENT = 10;
const int MAX_EMPTY_CELLS_PERCENT = 20;
const int MIN_EGG_CELLS_PERCENT = 10;
const int MAX_EGG_CELLS_PERCENT = 28;
const int MIN_FOOD_CELLS_PERCENT = 10;
const int MAX_FOOD_CELLS_PERCENT = 30;
const int STARTING_HILL_DISTANCE = 2;
const int MAX_ANT_LOSS = 3;
const int FRAMES_PER_TURN = 1;
const int MAX_TURNS = 100 * FRAMES_PER_TURN;
const bool FIGHTING_ANTS_KILL = false;
const bool LOSING_ANTS_CANT_CARRY = true;
const bool FORCE_SINGLE_HILL = false;
const bool ENABLE_EGGS = true;
const bool SCORES_IN_IO = true;
} // namespace Config

static const int FIRST_TURN_MAX_TIME = 1000;
static const int TURN_MAX_TIME = 100;
// The jar spawns the agents, then initializes the referee in its fresh JVM before it starts the
// first turn's clock: the agents get about 0.6 s of head start (measured on a quiet machine, from
// process creation to the first input). The first execution of a player therefore ends no earlier
// than its spawn time + FIRST_TURN_MAX_TIME + FIRST_TURN_HEAD_START here (cg::AgentOptions). It only
// matters for agents that need more than 1 s to start; fast agents are not slowed down.
static const int FIRST_TURN_HEAD_START = 600;

struct CubeCoord {
    int x, y, z;
    bool operator==(const CubeCoord& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const CubeCoord& o) const { return !(*this == o); }
};
static const int DIRECTIONS[6][3] = {{1, -1, 0}, {+1, 0, -1}, {0, +1, -1}, {-1, +1, 0}, {-1, 0, +1}, {0, -1, +1}};
static const CubeCoord CENTER = {0, 0, 0};
static inline CubeCoord neighbor(const CubeCoord& c, int orientation) {
    return {c.x + DIRECTIONS[orientation][0], c.y + DIRECTIONS[orientation][1], c.z + DIRECTIONS[orientation][2]};
}
static inline CubeCoord opposite(const CubeCoord& c) { return {-c.x, -c.y, -c.z}; }
static inline int distanceTo(const CubeCoord& a, const CubeCoord& b) {
    return (std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z)) / 2;
}
static std::vector<CubeCoord> neighbours(const CubeCoord& c) {
    std::vector<CubeCoord> r;
    for (int i = 0; i < 6; i++) r.push_back(neighbor(c, i));
    return r;
}

// coordinates never leave [-8, 8] (7 rings plus neighbours)
static const int LO = 10, LK = 21;
static inline bool inGrid(const CubeCoord& c) { return c.x >= -LO && c.x <= LO && c.z >= -LO && c.z <= LO; }

enum CellType { EMPTY, FOOD, EGG };

struct Cell {
    CellType type = EMPTY;
    int richness = 0;
    int index = 0;
    int anthill = -1; // player index, -1 = null
    int ants[2] = {0, 0};
    int beacons[2] = {0, 0};
    CubeCoord coord{0, 0, 0};
    void setFoodAmount(int r) {
        richness = r;
        type = FOOD;
    }
    void setSpawnPower(int r) {
        richness = r;
        type = EGG;
    }
    void placeAnts(int p, int amount) { ants[p] += amount; }
    void removeAnts(int p, int amount) { ants[p] -= std::min(amount, ants[p]); }
    void deplete(int amount) { richness -= std::min(amount, richness); }
    void removeBeacons() {
        beacons[0] = 0;
        beacons[1] = 0;
    }
};

struct LineAction {
    int from, to, ants;
};
struct BeaconAction {
    int cellIndex, power;
};

struct Player : cg::Player {
    bool hasMessage = false;
    ustr message;
    std::vector<int> anthills;
    std::vector<BeaconAction> beacons;
    std::vector<LineAction> lines;
    int points = 0;

    int getExpectedOutputLines() const override { return 1; }

    void setMessage(const ustr& m) {
        ustr trimmed = java::trim(m);
        if (trimmed.size() > 48) trimmed = trimmed.substr(0, 46) + u"...";
        if (trimmed.size() > 0) {
            message = trimmed;
            hasMessage = true;
        }
    }
    void reset() {
        hasMessage = false;
        message.clear();
        beacons.clear();
        lines.clear();
    }
};

// ----------------------------------------------------------------------------
// Board.java
// ----------------------------------------------------------------------------
struct Board {
    std::vector<Cell> cells;       // sorted by cell index
    std::vector<CubeCoord> coords; // sorted by cell index
    int ringCount = 0;
    int n = 0;
    int lookup[LK][LK];               // map: (x, z) -> cell index, -1 when absent
    std::vector<std::vector<int>> nb; // getNeighbours(i): existing neighbours in direction order
    std::vector<int> dist;            // getDistance(a, b) for all pairs (BFS length, -1 if no path)
    std::vector<int> attackCache[2];  // INT_MIN = not cached
    int initialFood = 0;

    int indexOf(const CubeCoord& c) const { return inGrid(c) ? lookup[c.x + LO][c.z + LO] : -1; }
    bool valid(int index) const { return index >= 0 && index < n; }

    void build(const std::vector<CubeCoord>& coordList, int rings) {
        ringCount = rings;
        n = (int)coordList.size();
        for (auto& row : lookup)
            for (int& v : row) v = -1;
        cells.assign(n, Cell());
        coords = coordList;
        for (int i = 0; i < n; i++) {
            cells[i].index = i;
            cells[i].coord = coordList[i];
            lookup[coordList[i].x + LO][coordList[i].z + LO] = i;
        }
        nb.assign(n, {});
        for (int i = 0; i < n; i++)
            for (int d = 0; d < 6; d++) {
                int j = indexOf(neighbor(coords[i], d));
                if (j >= 0) nb[i].push_back(j);
            }
    }

    void computeDistances() {
        dist.assign((size_t)n * n, -1);
        std::vector<int> q(n);
        for (int s = 0; s < n; s++) {
            int* d = &dist[(size_t)s * n];
            d[s] = 0;
            int qh = 0, qt = 0;
            q[qt++] = s;
            while (qh < qt) {
                int h = q[qh++];
                for (int x : nb[h])
                    if (d[x] < 0) {
                        d[x] = d[h] + 1;
                        q[qt++] = x;
                    }
            }
        }
        for (int p = 0; p < 2; p++) attackCache[p].assign(n, INT_MIN);
    }

    int getDistance(int a, int b) const { return dist[(size_t)a * n + b]; }

    void resetAttackCache() {
        for (int p = 0; p < 2; p++) std::fill(attackCache[p].begin(), attackCache[p].end(), INT_MIN);
    }

    // getEdges()
    std::vector<CubeCoord> getEdges() const {
        std::vector<CubeCoord> r;
        for (auto& c : coords)
            if (distanceTo(c, CENTER) == ringCount) r.push_back(c);
        return r;
    }

    // Board.isConnected(coords)
    bool isConnected() const {
        std::vector<char> visited(n, 0);
        std::vector<int> stack;
        stack.push_back(0);
        visited[0] = 1;
        int count = 1;
        while (!stack.empty()) {
            int c = stack.back();
            stack.pop_back();
            for (int x : nb[c])
                if (!visited[x]) {
                    visited[x] = 1;
                    count++;
                    stack.push_back(x);
                }
        }
        return count == n;
    }

    int getRemainingFood() const {
        int s = 0;
        for (auto& c : cells)
            if (c.type == FOOD) s += c.richness;
        return s;
    }
};

static Board board;
static Player players[2];
static java::SHA1PRNG& rnd = cg::gm.getRandom(); // seeded by the engine

// ----------------------------------------------------------------------------
// BoardGenerator.java
// ----------------------------------------------------------------------------
// ArrayList<CubeCoord> with a presence grid for contains()
struct CoordList {
    std::vector<CubeCoord> v;
    int cnt[LK][LK] = {};
    bool contains(const CubeCoord& c) const { return inGrid(c) && cnt[c.x + LO][c.z + LO] > 0; }
    void add(const CubeCoord& c) {
        v.push_back(c);
        cnt[c.x + LO][c.z + LO]++;
    }
    // remove(Object): first occurrence
    void remove(const CubeCoord& c) {
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] == c) {
                cnt[c.x + LO][c.z + LO]--;
                v.erase(v.begin() + i);
                return;
            }
    }
    // removeAll(set)
    template <class Set>
    void removeAll(const Set& s) {
        std::vector<CubeCoord> keep;
        for (auto& c : v)
            if (!s.contains(c)) keep.push_back(c);
            else cnt[c.x + LO][c.z + LO]--;
        v.swap(keep);
    }
};
// HashSet<CubeCoord>
struct CoordSet {
    bool in[LK][LK] = {};
    int size = 0;
    bool contains(const CubeCoord& c) const { return inGrid(c) && in[c.x + LO][c.z + LO]; }
    void add(const CubeCoord& c) {
        if (!in[c.x + LO][c.z + LO]) {
            in[c.x + LO][c.z + LO] = true;
            size++;
        }
    }
    void clear() {
        for (auto& row : in)
            for (bool& b : row) b = false;
        size = 0;
    }
};

static int randomPercentage(int min, int max, int total) {
    int percentage = min + rnd.nextInt((max + 1) - min);
    return java::d2i((double)(percentage * total) / 100.0);
}

static bool hasSixNeighbours(const CubeCoord& coord, const CoordList& list) {
    int count = 0;
    for (int i = 0; i < 6; i++)
        if (list.contains(neighbor(coord, i))) count++;
    return count == 6;
}

static const double VERTICAL_CUTOFF = 0.6;

static void generatePotentiallyUnconnectedGraph() {
    int ringCount = rnd.nextInt(Config::MAP_RING_COUNT_MAX - Config::MAP_RING_COUNT_MIN + 1) + Config::MAP_RING_COUNT_MIN;

    CoordList coordList;
    coordList.add(CENTER);
    CubeCoord cur = neighbor(CENTER, 0);
    int verticalLimit = java::d2i(std::ceil(ringCount * VERTICAL_CUTOFF));
    for (int distance = 1; distance <= ringCount; distance++) {
        for (int orientation = 0; orientation < 6; orientation++) {
            for (int count = 0; count < distance; count++) {
                if (cur.z > -verticalLimit && cur.z < verticalLimit) {
                    if (!coordList.contains(cur)) {
                        coordList.add(cur);
                        coordList.add(opposite(cur));
                    }
                }
                cur = neighbor(cur, (orientation + 2) % 6);
            }
        }
        cur = neighbor(cur, 0);
    }

    // Create holes
    int coordListSize = (int)coordList.v.size();
    int wantedEmptyCells = randomPercentage(Config::MIN_EMPTY_CELLS_PERCENT, Config::MAX_EMPTY_CELLS_PERCENT, (int)coordList.v.size());

    CoordSet toRemove;
    while (toRemove.size < wantedEmptyCells) {
        int randIndex = rnd.nextInt(coordListSize);
        CubeCoord randCoord = coordList.v[randIndex];
        toRemove.add(randCoord);
        toRemove.add(opposite(randCoord));
    }
    coordList.removeAll(toRemove);

    bool CORRIDOR_MODE = rnd.nextDouble() < 0.05;
    if (CORRIDOR_MODE) {
        toRemove.clear();
        for (auto& coord : coordList.v)
            if (hasSixNeighbours(coord, coordList)) toRemove.add(coord);
        coordList.removeAll(toRemove);
    }

    bool NO_BLOB_MODE = rnd.nextDouble() < 0.70;
    if (NO_BLOB_MODE) {
        bool changed = true;
        while (changed) {
            changed = false;
            int blobCenter = -1;
            for (size_t i = 0; i < coordList.v.size() && blobCenter < 0; i++)
                if (hasSixNeighbours(coordList.v[i], coordList)) blobCenter = (int)i;
            if (blobCenter >= 0) {
                std::vector<CubeCoord> nbs = neighbours(coordList.v[blobCenter]);
                java::shuffle(nbs, rnd);
                CubeCoord first = nbs[0];
                coordList.remove(first);
                coordList.remove(opposite(first));
                changed = true;
            } else {
                changed = false;
            }
        }
    }

    board.build(coordList.v, ringCount);
}

static void createEmptyBoard() {
    int iterations = 1000;
    do {
        generatePotentiallyUnconnectedGraph();
        iterations--;
    } while (!board.isConnected() && iterations > 0);
}

static int getSmallEggsAmount() { return 10 + rnd.nextInt(10); }
static int getLargeEggsAmount() { return 20 + rnd.nextInt(20); }
static int getSmallFoodAmount() { return 10 + rnd.nextInt(30); }
static int getLargeFoodAmount() { return 40 + rnd.nextInt(20); }

static std::vector<CubeCoord> trySelectAnthillCoords(int startingHillCount) {
    std::vector<CubeCoord> coordinates;
    std::vector<CubeCoord> availableCoords;
    for (auto& coord : board.coords) {
        const Cell& cell = board.cells[board.indexOf(coord)];
        if (cell.index == 0) continue;
        if (cell.richness > 0) continue;
        availableCoords.push_back(coord);
    }
    for (int i = 0; i < startingHillCount && !availableCoords.empty(); i++) {
        int r = rnd.nextInt((int)availableCoords.size());
        CubeCoord normalCoord = availableCoords[r];
        CubeCoord oppositeCoord = opposite(normalCoord);
        std::vector<CubeCoord> keep;
        for (auto& coord : availableCoords)
            if (!(distanceTo(coord, normalCoord) <= Config::STARTING_HILL_DISTANCE ||
                  distanceTo(coord, oppositeCoord) <= Config::STARTING_HILL_DISTANCE))
                keep.push_back(coord);
        availableCoords.swap(keep);
        coordinates.push_back(normalCoord);
    }
    return coordinates;
}

static std::vector<CubeCoord> selectAnthillCoords(int hillsPerPlayer) {
    std::vector<CubeCoord> validCoords;
    int iter = 1000;
    while ((int)validCoords.size() < hillsPerPlayer && iter > 0) {
        iter--;
        validCoords = trySelectAnthillCoords(hillsPerPlayer);
    }
    // Failsafes
    if ((int)validCoords.size() < hillsPerPlayer) validCoords = board.getEdges();
    if ((int)validCoords.size() < hillsPerPlayer) validCoords = board.coords;
    return validCoords;
}

static void addResourceCells() {
    // Fill center cell
    if (rnd.nextBoolean()) {
        int center = board.indexOf(CENTER);
        if (center >= 0) board.cells[center].setFoodAmount(getLargeFoodAmount());
    }

    // Place anthills
    int hillsPerPlayer = Config::FORCE_SINGLE_HILL ? 1 : (rnd.nextDouble() < .33 ? 2 : 1);
    std::vector<CubeCoord> validCoords = selectAnthillCoords(hillsPerPlayer);
    for (int i = 0; i < std::min(hillsPerPlayer, (int)validCoords.size()); i++) {
        CubeCoord coord = validCoords[i];
        Cell& cell1 = board.cells[board.indexOf(coord)];
        cell1.anthill = 0;
        players[0].anthills.push_back(cell1.index);
        Cell& cell2 = board.cells[board.indexOf(opposite(coord))];
        cell2.anthill = 1;
        players[1].anthills.push_back(cell2.index);
    }

    // Place food
    bool SURPLUS_MODE = Config::FORCE_SINGLE_HILL ? true : (rnd.nextDouble() < 0.1);
    bool HUNGRY_MODE = Config::FORCE_SINGLE_HILL ? false : (!SURPLUS_MODE && rnd.nextDouble() < 0.08);
    bool FAMINE_MODE = Config::FORCE_SINGLE_HILL ? false : (!SURPLUS_MODE && !HUNGRY_MODE && rnd.nextDouble() < 0.04);

    if (!FAMINE_MODE) {
        std::vector<CubeCoord> validFoodCoords;
        for (auto& coord : board.coords)
            if (board.cells[board.indexOf(coord)].anthill < 0) validFoodCoords.push_back(coord);
        int wantedFoodCells = randomPercentage(Config::MIN_FOOD_CELLS_PERCENT, Config::MAX_FOOD_CELLS_PERCENT, (int)validFoodCoords.size());
        wantedFoodCells = std::max(2, wantedFoodCells);

        java::shuffle(validFoodCoords, rnd);
        for (int i = 0; i < wantedFoodCells; i += 2) {
            CubeCoord coord = validFoodCoords.at(i);
            Cell& cell = board.cells[board.indexOf(coord)];
            double roll = rnd.nextDouble();
            if (roll < 0.65) {
                int amount = HUNGRY_MODE ? getSmallFoodAmount() : getLargeFoodAmount();
                if (SURPLUS_MODE) amount *= Config::FORCE_SINGLE_HILL ? 5 : 10;
                cell.setFoodAmount(amount);
                board.cells[board.indexOf(opposite(coord))].setFoodAmount(amount);
            } else {
                int amount = HUNGRY_MODE ? getSmallFoodAmount() : getSmallFoodAmount() / 2;
                if (SURPLUS_MODE) amount *= Config::FORCE_SINGLE_HILL ? 5 : 10;
                cell.setFoodAmount(amount);
                board.cells[board.indexOf(opposite(coord))].setFoodAmount(amount);
            }
        }
    }
    // Make sure there is food on the board
    bool boardHasFood = false;
    for (auto& c : board.cells)
        if (c.type == FOOD) boardHasFood = true;
    if (!boardHasFood) {
        std::vector<int> emptyCells;
        for (auto& c : board.cells)
            if (c.type == EMPTY && c.anthill < 0) emptyCells.push_back(c.index);
        int randomIndex = rnd.nextInt((int)emptyCells.size());
        Cell& emptyCell = board.cells[emptyCells[randomIndex]];
        int amount = getLargeFoodAmount();
        if (SURPLUS_MODE) amount *= 10;
        emptyCell.setFoodAmount(amount);
        CubeCoord oc = opposite(emptyCell.coord);
        if (oc != emptyCell.coord) board.cells[board.indexOf(oc)].setFoodAmount(amount);
    }

    int antPotential = 0;
    if (Config::ENABLE_EGGS) {
        // Place egg
        std::vector<CubeCoord> validEggCoords;
        for (auto& coord : board.coords) {
            const Cell& c = board.cells[board.indexOf(coord)];
            if (c.anthill < 0 && c.richness == 0) validEggCoords.push_back(coord);
        }
        int wantedEggCells = randomPercentage(Config::MIN_EGG_CELLS_PERCENT, Config::MAX_EGG_CELLS_PERCENT, (int)validEggCoords.size());

        java::shuffle(validEggCoords, rnd);
        for (int i = 0; i < wantedEggCells; i += 2) {
            CubeCoord coord = validEggCoords.at(i);
            Cell& cell = board.cells[board.indexOf(coord)];
            double roll = rnd.nextDouble();
            if (roll < 0.4) {
                int amount = getLargeEggsAmount();
                cell.setSpawnPower(amount);
                board.cells[board.indexOf(opposite(coord))].setSpawnPower(amount);
                antPotential += amount * 2;
            } else {
                int amount = getSmallEggsAmount();
                cell.setSpawnPower(amount);
                board.cells[board.indexOf(opposite(coord))].setSpawnPower(amount);
                antPotential += amount * 2;
            }
        }
    }

    // Place ants
    int antsPerHill = std::max(10, 60 - antPotential);
    for (auto& player : players)
        for (int idx : player.anthills) board.cells[idx].placeAnts(player.index, antsPerHill);

    board.initialFood = board.getRemainingFood();
}

static void generateBoard() {
    createEmptyBoard();
    board.computeDistances();
    addResourceCells();
}

// ----------------------------------------------------------------------------
// Board.java: paths
// ----------------------------------------------------------------------------
// java.util.PriorityQueue<Integer> with a comparator (offer/poll exactly as in JDK 26)
struct PriorityQueueI {
    std::vector<int> es;
    int size = 0;
    bool isEmpty() const { return size == 0; }
    template <class Cmp>
    void offer(int x, Cmp& cmp) {
        int k = size;
        if (k >= (int)es.size()) es.resize(k + 16);
        while (k > 0) { // siftUpUsingComparator
            int parent = (k - 1) >> 1;
            int e = es[parent];
            if (cmp(x, e) >= 0) break;
            es[k] = e;
            k = parent;
        }
        es[k] = x;
        size++;
    }
    template <class Cmp>
    int poll(Cmp& cmp) {
        int result = es[0];
        int n = --size;
        int x = es[n];
        if (n > 0) { // siftDownUsingComparator
            int k = 0;
            int half = n >> 1;
            while (k < half) {
                int child = (k << 1) + 1;
                int c = es[child];
                int right = child + 1;
                if (right < n && cmp(c, es[right]) > 0) c = es[child = right];
                if (cmp(x, c) <= 0) break;
                es[k] = c;
                k = child;
            }
            es[k] = x;
        }
        return result;
    }
};

// findShortestPath(a, b, playerIdx): BFS, neighbours ordered by friendly ants, then beacon
// strength, then cell id (playerIdx < 0: by cell id). The BFS stops as soon as b is reached,
// which does not change the parents of the cells reached before, so `prev` from a complete BFS
// gives the same path to every target.
static void bfsFrom(int a, int playerIdx, std::vector<int>& prev) {
    int n = board.n;
    prev.assign(n, -2); // -2: not in prev, -1: null
    std::vector<int> queue;
    queue.reserve(n);
    size_t qh = 0;
    prev[a] = -1;
    queue.push_back(a);
    std::vector<int> nbs;
    while (qh < queue.size()) {
        int head = queue[qh++];
        nbs = board.nb[head];
        if (playerIdx >= 0) {
            std::stable_sort(nbs.begin(), nbs.end(), [&](int i, int j) {
                const Cell& ci = board.cells[i];
                const Cell& cj = board.cells[j];
                if (ci.ants[playerIdx] != cj.ants[playerIdx]) return -ci.ants[playerIdx] < -cj.ants[playerIdx];
                if (ci.beacons[playerIdx] != cj.beacons[playerIdx]) return -ci.beacons[playerIdx] < -cj.beacons[playerIdx];
                return i < j;
            });
        } else {
            std::sort(nbs.begin(), nbs.end());
        }
        for (int x : nbs)
            if (prev[x] == -2) {
                prev[x] = head;
                queue.push_back(x);
            }
    }
}

static bool pathFromPrev(const std::vector<int>& prev, int b, std::vector<int>& path) {
    path.clear();
    if (prev[b] == -2) return false; // impossibru
    for (int current = b; current != -1; current = prev[current]) path.push_back(current);
    std::reverse(path.begin(), path.end());
    return true;
}

static int getAttackPower(int cellIdx, int playerIdx);

// getBestPath(start, end, playerIdx, interruptedByFight): Dijkstra on (maxValue, minDist)
static bool getBestPath(int start, int end, int playerIdx, bool interruptedByFight, std::vector<int>& path) {
    int n = board.n;
    std::vector<int> maxPathValues(n, INT_MIN), prev(n, -1), distanceFromStart(n, 0);
    std::vector<char> visited(n, 0);

    // valueComparator.reversed().thenComparing(distanceComparator)
    auto cmp = [&](int a, int b) -> int {
        int r = java::icmp(maxPathValues[b], maxPathValues[a]);
        if (r != 0) return r;
        return java::icmp(distanceFromStart[a] + board.getDistance(a, end), distanceFromStart[b] + board.getDistance(b, end));
    };
    PriorityQueueI queue;
    const Cell& startCell = board.cells[start];
    maxPathValues[start] = startCell.ants[playerIdx];
    distanceFromStart[start] = 0;
    int startAnts = startCell.ants[playerIdx];
    if (interruptedByFight) {
        int myForce = getAttackPower(start, playerIdx);
        int otherForce = getAttackPower(start, 1 - playerIdx);
        if (otherForce > myForce) startAnts = 0;
    }
    if (startAnts > 0) queue.offer(start, cmp);

    while (!queue.isEmpty() && !visited[end]) {
        int currentIndex = queue.poll(cmp);
        visited[currentIndex] = 1;

        // Update the max values of the neighbors
        for (int neighborIndex : board.nb[currentIndex]) {
            int neighborAnts = board.cells[neighborIndex].ants[playerIdx];
            if (neighborAnts > 0) {
                if (interruptedByFight) {
                    int myForce = getAttackPower(neighborIndex, playerIdx);
                    int otherForce = getAttackPower(neighborIndex, 1 - playerIdx);
                    if (otherForce > myForce) neighborAnts = 0;
                }
            }
            if (!visited[neighborIndex] && neighborAnts > 0) {
                int potentialMaxPathValue = std::min(maxPathValues[currentIndex], neighborAnts);
                if (potentialMaxPathValue > maxPathValues[neighborIndex]) {
                    maxPathValues[neighborIndex] = potentialMaxPathValue;
                    distanceFromStart[neighborIndex] = distanceFromStart[currentIndex] + 1;
                    prev[neighborIndex] = currentIndex;
                    queue.offer(neighborIndex, cmp);
                }
            }
        }
    }

    if (!visited[end]) return false; // No path from start to end

    path.clear();
    for (int currentIndex = end; currentIndex != -1; currentIndex = prev[currentIndex]) path.push_back(currentIndex);
    std::reverse(path.begin(), path.end());
    return true;
}

static int pathMin(const std::vector<int>& path, int playerIdx) {
    int m = INT_MAX;
    for (int c : path) m = std::min(m, board.cells[c].ants[playerIdx]);
    return path.empty() ? 0 : m;
}

static int getAttackPower(int cellIdx, int playerIdx) {
    int cached = board.attackCache[playerIdx][cellIdx];
    if (cached != INT_MIN) return cached;

    bool any = false;
    int maxMin = 0;
    std::vector<int> bestPath;
    for (int anthill : players[playerIdx].anthills) {
        if (getBestPath(cellIdx, anthill, playerIdx, false, bestPath)) {
            int m = pathMin(bestPath, playerIdx);
            if (!any || m > maxMin) maxMin = m;
            any = true;
        }
    }
    if (!any) maxMin = 0;
    board.attackCache[playerIdx][cellIdx] = maxMin;
    return maxMin;
}

// ----------------------------------------------------------------------------
// AntAllocater.java
// ----------------------------------------------------------------------------
struct AllocCellData {
    int cell;
    i64 ants, beacons, wiggleRoom;
};
struct AntAllocation {
    int antIndex, beaconIndex, amount;
};

static std::vector<AntAllocation> allocateAnts(const std::vector<int>& antCellIdx, const std::vector<int>& beaconCellIdx, int playerIdx) {
    std::deque<AllocCellData> antCells, beaconCells; // stable addresses
    for (int c : antCellIdx) antCells.push_back({c, board.cells[c].ants[playerIdx], board.cells[c].beacons[playerIdx], 0});
    for (int c : beaconCellIdx) beaconCells.push_back({c, board.cells[c].ants[playerIdx], board.cells[c].beacons[playerIdx], 0});

    std::vector<AntAllocation> allocations;
    i64 antSum = 0;
    for (auto& cell : antCells) antSum += cell.ants;
    i64 beaconSum = 0;
    for (auto& cell : beaconCells) beaconSum += cell.beacons;

    double scalingFactor = (double)antSum / (double)beaconSum;
    for (auto& cell : beaconCells) {
        i64 highBeaconValue = java::d2l(std::ceil((double)cell.beacons * scalingFactor));
        i64 lowBeaconValue = java::d2l((double)cell.beacons * scalingFactor);
        cell.beacons = std::max((i64)1, lowBeaconValue);
        cell.wiggleRoom = highBeaconValue - cell.beacons;
    }

    struct Pair {
        AllocCellData* ant;
        AllocCellData* beacon;
        int distance;
    };
    std::vector<Pair> allPairs;
    for (auto& antCell : antCells)
        for (auto& beaconCell : beaconCells) {
            int d = board.getDistance(antCell.cell, beaconCell.cell);
            if (d != -1) allPairs.push_back({&antCell, &beaconCell, d});
        }

    std::stable_sort(allPairs.begin(), allPairs.end(), [](const Pair& a, const Pair& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        if (a.ant->cell != b.ant->cell) return a.ant->cell < b.ant->cell;
        return a.beacon->cell < b.beacon->cell;
    });

    bool stragglers = false;
    while (!allPairs.empty()) {
        for (auto& pair : allPairs) {
            AllocCellData* antCell = pair.ant;
            i64 antCount = antCell->ants;
            AllocCellData* beaconCell = pair.beacon;
            i64 beaconCount = beaconCell->beacons;
            i64 wiggleRoom = beaconCell->wiggleRoom;

            int maxAlloc = java::l2i(stragglers ? std::min(antCount, beaconCount + wiggleRoom) : std::min(antCount, beaconCount));
            if (maxAlloc > 0) {
                allocations.push_back({antCell->cell, beaconCell->cell, maxAlloc});
                antCell->ants -= maxAlloc;
                if (!stragglers) {
                    beaconCell->beacons -= maxAlloc;
                } else {
                    beaconCell->beacons -= (maxAlloc - wiggleRoom);
                    beaconCell->wiggleRoom = 0;
                }
            }
        }
        std::vector<Pair> keep;
        for (auto& pair : allPairs)
            if (!(pair.ant->ants <= 0)) keep.push_back(pair);
        allPairs.swap(keep);
        stragglers = true;
    }
    return allocations;
}

// ----------------------------------------------------------------------------
// Animation.java, EventData.java
// ----------------------------------------------------------------------------
namespace Animation {
const int THIRD = 300;
const int HALF = 500;
static int frameTime = 0, endTime = 0;
static void reset() {
    frameTime = 0;
    endTime = 0;
}
static void wait(int time) { frameTime += time; }
static void catchUp() { frameTime = endTime; }
static int computeEvents() {
    int minTime = 1000;
    catchUp();
    return std::max(frameTime, minTime);
}
} // namespace Animation

struct EventData {
    enum { BUILD = 0, MOVE = 1, FOOD = 2 };
    int type = 0;
    int start = 0, end = 0; // animData.get(0)
    int playerIdx = 0, amount = 0;
    int cellIdx = -1, targetIdx = -1; // -1 = null
    bool hasPath = false;
    std::vector<int> path;
};
static std::vector<EventData> viewerEvents;

static void startAnim(EventData& e, int duration) {
    e.start = Animation::frameTime;
    e.end = Animation::frameTime + duration;
    Animation::endTime = std::max(Animation::endTime, Animation::frameTime + duration);
}

// ----------------------------------------------------------------------------
// Engine state used by the game (GameManager)
// ----------------------------------------------------------------------------
static void addToGameSummary(const ustr& summary) { cg::gm.addToGameSummary(summary); }
static void setFrameDuration(int duration) { cg::gm.setFrameDuration(duration); }
static void deactivate(Player& p, const ustr* reason) {
    if (reason) p.deactivate(*reason);
    else p.deactivate();
}

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (auto& p : players)
        if (p.active) r.push_back(&p);
    return r;
}


// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
struct AntConsumption {
    int player;
    int amount;
    int cell;
    std::vector<int> path;
};

static int gameTurn = 0;
static bool moveAnimatedThisTurn = false;

static bool isKeyFrame() { return gameTurn % Config::FRAMES_PER_TURN == 0; }

static void resetGameTurnData() {
    if (isKeyFrame())
        for (auto& cell : board.cells) cell.removeBeacons();
    viewerEvents.clear();
    Animation::reset();
    for (auto& p : players) p.reset();
    moveAnimatedThisTurn = false;
    board.resetAttackCache();
}

static std::string serializeList(const std::vector<int>& l) {
    std::string s;
    for (size_t i = 0; i < l.size(); i++) {
        if (i) s += ' ';
        s += std::to_string(l[i]);
    }
    return s;
}

static void getGlobalInfoFor(const Player& player, std::string& in) {
    in += std::to_string(board.n) + "\n";
    for (int i = 0; i < board.n; i++) {
        const Cell& cell = board.cells[i];
        in += std::to_string(cell.type == EGG ? 1 : cell.type == FOOD ? 2 : 0) + " " + std::to_string(cell.richness) + " ";
        for (int d = 0; d < 6; d++) {
            if (d) in += ' ';
            in += std::to_string(board.indexOf(neighbor(board.coords[i], d)));
        }
        in += "\n";
    }
    const Player& other = players[1 - player.index];
    in += std::to_string(player.anthills.size()) + "\n";
    in += serializeList(player.anthills) + "\n";
    in += serializeList(other.anthills) + "\n";
}

static void getCurrentFrameInfoFor(const Player& player, std::string& in) {
    const Player& other = players[1 - player.index];
    if (Config::SCORES_IN_IO) in += std::to_string(player.points) + " " + std::to_string(other.points) + "\n";
    for (int i = 0; i < board.n; i++) {
        const Cell& cell = board.cells[i];
        in += std::to_string(cell.richness) + " " + std::to_string(cell.ants[player.index]) + " " + std::to_string(cell.ants[other.index]) + "\n";
    }
}

static int pathValue(int playerIdx, const std::vector<int>& list) { return pathMin(list, playerIdx); }

static std::vector<AntConsumption> computeCellConsumption(const Player& player, const std::vector<int>& targetCells) {
    std::vector<AntConsumption> meals;
    for (int foodCell : targetCells) {
        std::vector<std::vector<int>> allPaths;
        for (int anthill : player.anthills) {
            // Dijkstra from food to anthill
            std::vector<int> bestPathToHill;
            if (getBestPath(foodCell, anthill, player.index, Config::LOSING_ANTS_CANT_CARRY, bestPathToHill))
                allPaths.push_back(bestPathToHill);
        }
        // sorted(byPathValue.reversed().thenComparing(size)).findFirst(): stable sort
        int best = -1;
        for (int i = 0; i < (int)allPaths.size(); i++) {
            if (best < 0) {
                best = i;
                continue;
            }
            int vi = pathValue(player.index, allPaths[i]), vb = pathValue(player.index, allPaths[best]);
            if (vi > vb || (vi == vb && allPaths[i].size() < allPaths[best].size())) best = i;
        }
        int maxMin = best >= 0 ? pathValue(player.index, allPaths[best]) : 0;
        int foodEaten = std::min(maxMin, board.cells[foodCell].richness);
        if (foodEaten > 0) meals.push_back({player.index, foodEaten, foodCell, allPaths[best]});
    }
    return meals;
}

// Board.getEggCells() / getFoodCells() iterate a HashMap; the order is not observable (see top)
static std::vector<int> getCells(CellType type) {
    std::vector<int> r;
    for (auto& c : board.cells)
        if (c.type == type && c.richness > 0) r.push_back(c.index);
    return r;
}

static void launchBuildEvent(int amount, int playerIdx, const std::vector<int>& path) {
    EventData e;
    e.type = EventData::BUILD;
    e.playerIdx = playerIdx;
    e.amount = amount;
    e.hasPath = true;
    e.path = path;
    startAnim(e, Animation::HALF);
    viewerEvents.push_back(e);
}

static void launchFoodEvent(const AntConsumption& meal) {
    EventData e;
    e.type = EventData::FOOD;
    e.playerIdx = meal.player;
    e.hasPath = true;
    e.path = meal.path;
    e.amount = meal.amount;
    startAnim(e, Animation::HALF);
    viewerEvents.push_back(e);
}

static void launchMoveEvent(int fromIdx, int toIdx, int amount, int playerIdx) {
    EventData e;
    e.type = EventData::MOVE;
    e.playerIdx = playerIdx;
    e.cellIdx = fromIdx;
    e.targetIdx = toIdx;
    e.amount = amount;
    startAnim(e, Animation::HALF);
    viewerEvents.push_back(e);
}

static void doBuild() {
    std::vector<int> eggCells = getCells(EGG);
    std::vector<AntConsumption> builds;
    for (auto& player : players) {
        auto b = computeCellConsumption(player, eggCells);
        builds.insert(builds.end(), b.begin(), b.end());
    }
    for (auto& build : builds) {
        for (int idx : players[build.player].anthills) board.cells[idx].placeAnts(build.player, build.amount);
        launchBuildEvent(build.amount, build.player, build.path);
        board.cells[build.cell].deplete(build.amount);
    }
}

static void doFights() {
    if (Config::FIGHTING_ANTS_KILL) {
        for (auto& cell : board.cells) {
            int ants0 = cell.ants[0];
            int ants1 = cell.ants[1];
            cell.removeAnts(0, std::min(Config::MAX_ANT_LOSS, ants1));
            cell.removeAnts(1, std::min(Config::MAX_ANT_LOSS, ants0));
        }
    }
}

static void doScore() {
    std::vector<int> foodCells = getCells(FOOD);
    std::vector<AntConsumption> meals;
    for (auto& player : players) {
        auto m = computeCellConsumption(player, foodCells);
        meals.insert(meals.end(), m.begin(), m.end());
    }
    for (auto& meal : meals) {
        launchFoodEvent(meal);
        players[meal.player].points += meal.amount;
        board.cells[meal.cell].deplete(meal.amount);
    }
}

static void applyMove(int fromIdx, int toIdx, int amount, int playerIdx) {
    Cell& source = board.cells[fromIdx];
    Cell& target = board.cells[toIdx];
    source.removeAnts(playerIdx, amount);
    target.placeAnts(playerIdx, amount);
    launchMoveEvent(source.index, target.index, amount, playerIdx);
    moveAnimatedThisTurn = true;
}

static void doMove() {
    for (auto& player : players) {
        std::vector<int> playerAntCells, playerBeaconCells;
        for (auto& cell : board.cells) {
            if (cell.ants[player.index] > 0) playerAntCells.push_back(cell.index);
        }
        for (auto& cell : board.cells) {
            if (cell.beacons[player.index] > 0) playerBeaconCells.push_back(cell.index);
        }
        std::vector<AntAllocation> allocations = allocateAnts(playerAntCells, playerBeaconCells, player.index);

        // HashMap<AntMove, Integer>: iteration order not observable, insertion order is used
        std::vector<std::pair<std::pair<int, int>, int>> moves;
        std::vector<std::vector<int>> bfsCache(board.n);
        std::vector<int> path;
        for (auto& alloc : allocations) {
            // Get next step in path
            auto& prev = bfsCache[alloc.antIndex];
            if (prev.empty()) bfsFrom(alloc.antIndex, player.index, prev);
            pathFromPrev(prev, alloc.beaconIndex, path); // always found: the pair is connected
            if (path.size() > 1) {
                int from = alloc.antIndex;
                int to = path[1];
                bool found = false;
                for (auto& m : moves)
                    if (m.first.first == from && m.first.second == to) {
                        m.second += alloc.amount;
                        found = true;
                    }
                if (!found) moves.push_back({{from, to}, alloc.amount});
            }
        }
        for (auto& m : moves) applyMove(m.first.first, m.first.second, m.second, player.index);
    }
}

struct NullPointerException {};

static void setBeaconPower(int cellIndex, Player& player, int power) {
    if (!board.valid(cellIndex)) {
        // gameSummaryManager.addError(player, "cannot find cell %d"): only in the summary
        return;
    }
    board.cells[cellIndex].beacons[player.index] = std::max(1, power);
}

static void doBeacons() {
    for (auto& player : players)
        for (auto& beacon : player.beacons) setBeaconPower(beacon.cellIndex, player, beacon.power);
}

static void doLines() {
    std::vector<int> prev, path;
    for (auto& player : players) {
        for (auto& line : player.lines) {
            int from = line.from, to = line.to, beaconPower = line.ants;
            if (!board.valid(from)) continue; // addError "cannot find cell %d"
            if (!board.valid(to)) continue;
            bfsFrom(from, -1, prev);
            if (!pathFromPrev(prev, to, path)) throw NullPointerException(); // disconnected board
            for (int cellIndex : path) setBeaconPower(cellIndex, player, beaconPower);
        }
    }
}

static bool checkGameOver() {
    int remainingFood = board.getRemainingFood();
    if (board.getRemainingFood() == 0) return true;
    if (players[0].points >= players[1].points + remainingFood) {
        return true;
    } else if (players[1].points >= players[0].points + remainingFood) {
        return true;
    }
    return false;
}

static void performGameUpdate() {
    doLines();
    doBeacons();
    doMove();
    Animation::catchUp();
    if (moveAnimatedThisTurn) Animation::wait(Animation::THIRD);
    doFights();
    doBuild();
    Animation::catchUp();
    board.resetAttackCache();
    doScore();
    Animation::catchUp();
    gameTurn++;
    if (checkGameOver()) cg::gm.endGame();

    // gameSummaryManager.toString(): not reproduced (summary text only; it is the last line of a
    // turn and only added below 800 chars, so it can't make the SUMMARY command overflow)
    addToGameSummary(u"");

    int frameTime = Animation::computeEvents();
    setFrameDuration(frameTime);
}

static int getAntTotal(const Player& p) {
    int s = 0;
    for (auto& cell : board.cells) s += cell.ants[p.index];
    return s;
}

// endScreenModule.setScores(...) data, serialized by gson
static std::string endScreenJson;

static void onEnd() {
    for (auto& p : players) p.score = p.active ? p.points : -1;
    if (players[0].score == players[1].score && players[0].score != -1) {
        for (auto& p : players) p.score = getAntTotal(p);
        endScreenJson = "[[" + std::to_string(players[0].score) + "," + std::to_string(players[1].score) + "],\"logo.png\",[";
        for (int i = 0; i < 2; i++) {
            if (i) endScreenJson += ",";
            endScreenJson += "\"" + std::to_string(players[i].points) + " points and " + std::to_string(players[i].score) + " ants\"";
        }
        endScreenJson += "]]";
    } else {
        endScreenJson = "[[" + std::to_string(players[0].score) + "," + std::to_string(players[1].score) + "],\"logo.png\",null]";
    }
}

// ----------------------------------------------------------------------------
// CommandManager.java
// ----------------------------------------------------------------------------
static ustr getExpected(const ustr& playerOutput) {
    ustr attempt = java::upperPrefix(playerOutput, 7);
    if (java::startsWith(attempt, "BEACON")) return u"BEACON <cell_index> <beacon_strength>";
    if (java::startsWith(attempt, "LINE")) return u"LINE <from_index> <to_index> <beacon_strength>";
    if (java::startsWith(attempt, "MESSAGE")) return u"MESSAGE <text>";
    if (java::startsWith(attempt, "WAIT")) return u"WAIT";
    return u"BEACON | LINE | MESSAGE | WAIT";
}

static ustr invalidInput(const ustr& expected, const ustr& got) { return u"Invalid Input: Expected " + expected + u" but got '" + got + u"'"; }

// matches "^<word>( \d+){count}" against the whole string (\d is ASCII only), fills the groups
static bool matchNumbers(const ustr& t, const char* word, int count, ustr* groups) {
    if (!java::startsWith(t, word)) return false;
    size_t pos = strlen(word);
    for (int g = 0; g < count; g++) {
        if (pos >= t.size() || t[pos] != u' ') return false;
        size_t b = ++pos;
        while (pos < t.size() && t[pos] >= u'0' && t[pos] <= u'9') pos++;
        if (pos == b) return false;
        groups[g] = t.substr(b, pos - b);
    }
    return pos == t.size();
}

// returns the exception message when the command is invalid
static bool parseCommand(Player& player, const ustr& command, ustr& error) {
    ustr t = java::trim(command);
    ustr g[3];
    auto parse = [&](const ustr& digits, int& out) {
        if (java::parseInt(digits, out)) return true;
        error = invalidInput(getExpected(command), u"java.lang.NumberFormatException: For input string: \"" + digits + u"\"");
        return false;
    };
    if (matchNumbers(t, "BEACON", 2, g)) {
        int index, power;
        if (!parse(g[0], index) || !parse(g[1], power)) return false;
        player.beacons.push_back({index, power});
        return true;
    }
    if (matchNumbers(t, "LINE", 3, g)) {
        int from, to, ants;
        if (!parse(g[0], from) || !parse(g[1], to) || !parse(g[2], ants)) return false;
        player.lines.push_back({from, to, ants});
        return true;
    }
    if (java::startsWith(t, "MESSAGE ")) {
        // '.' matches everything left: the referee's line holds no line terminator
        player.setMessage(t.substr(8));
        return true;
    }
    if (t == u"WAIT") return true;
    error = invalidInput(getExpected(command), command);
    return false;
}

using cg::trace;

static void parseCommands(Player& player, const ustr& line) {
    std::vector<ustr> commands = java::split(line, u';');
    for (auto& command : commands) {
        ustr error;
        if (!parseCommand(player, command, error)) {
            if (trace) fprintf(trace, "  P%d disqualified: %s\n", player.index, java::utf8(error).c_str());
            // deactivatePlayer(player, e.getMessage())
            ustr reason = java::replace(java::replace(error, u"&lt;", u"<"), u"&gt;", u">");
            deactivate(player, &reason);
            player.score = -1;
            addToGameSummary(error);
            addToGameSummary(u"¤RED¤$" + java::num(player.index) + u": disqualified!§RED§");
            return;
        }
    }
}


// gson JsonWriter.string() (htmlSafe = false) escaping of a Java string, UTF-8 encoded
static void appendJsonEscaped(std::string& out, const ustr& s) {
    ustr esc;
    for (char16_t c : s) {
        if (c < 0x20) {
            switch (c) {
            case u'\t': esc += u"\\t"; break;
            case u'\b': esc += u"\\b"; break;
            case u'\n': esc += u"\\n"; break;
            case u'\r': esc += u"\\r"; break;
            case u'\f': esc += u"\\f"; break;
            default: {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
                esc += java::ascii(buf);
            }
            }
        } else if (c == u'"') {
            esc += u"\\\"";
        } else if (c == u'\\') {
            esc += u"\\\\";
        } else if (c == 0x2028) {
            esc += u"\\u2028";
        } else if (c == 0x2029) {
            esc += u"\\u2029";
        } else {
            esc += c;
        }
    }
    java::appendUtf8(out, esc);
}

// GameDataProvider.getCurrentFrameData() + Serializer.serialize(FrameViewData)
static void sendFrameData() {
    std::string g;
    g += std::to_string(players[0].points) + ";" + std::to_string(players[1].points);
    for (auto& p : players) {
        g += ";";
        if (p.hasMessage) appendJsonEscaped(g, p.message);
    }
    for (auto& p : players) {
        g += ";";
        for (int i = 0; i < board.n; i++) {
            if (i) g += ' ';
            g += std::to_string(board.cells[i].beacons[p.index]);
        }
    }
    g += ";" + std::to_string(viewerEvents.size());
    for (auto& e : viewerEvents) {
        g += ";" + std::to_string(e.type) + ";" + std::to_string(e.start) + ";" + std::to_string(e.end) + ";" + std::to_string(e.playerIdx) +
             ";" + std::to_string(e.amount) + ";";
        if (e.cellIdx >= 0) g += std::to_string(e.cellIdx);
        g += ";";
        if (e.targetIdx >= 0) g += std::to_string(e.targetIdx);
        g += ";";
        if (e.hasPath) g += serializeList(e.path);
    }
    cg::gm.setViewData("graphics", cg::Json::raw("\"" + g + "\""));
}

// GameDataProvider.getGlobalData() + Serializer.serialize(GlobalViewData)
static void sendGlobalData() {
    std::string g = std::to_string(board.n);
    for (int i = 0; i < board.n; i++) {
        const Cell& cell = board.cells[i];
        g += ";" + std::to_string(cell.coord.x) + " " + std::to_string(cell.coord.z) + " " + std::to_string(cell.richness) + " " +
             std::to_string(cell.type == EGG ? 1 : 2) + " " + std::to_string(cell.ants[0]) + " " + std::to_string(cell.ants[1]) + " " +
             std::to_string(cell.anthill);
    }
    cg::gm.setViewGlobalData("graphics", cg::Json::raw("\"" + g + "\""));
}

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
// GameManager.execute input: one line per sendInputLine
static void sendInput(Player& player, const std::string& text) {
    size_t off = 0, nl;
    while ((nl = text.find('\n', off)) != std::string::npos) {
        player.sendInputLine(text.substr(off, nl - off));
        off = nl + 1;
    }
}

static void sendGlobalInfo() {
    for (Player* player : getActivePlayers()) {
        std::string in;
        getGlobalInfoFor(*player, in);
        sendInput(*player, in);
    }
}

static void handlePlayerCommands() {
    for (Player* player : getActivePlayers()) {
        if (player->timeout) {
            static const ustr reason = u"Timeout!";
            deactivate(*player, &reason);
            addToGameSummary(u"$" + java::num(player->index) + u" has not provided 1 lines in time");
        } else {
            parseCommands(*player, player->outputs[0]);
        }
    }
}

struct AntsReferee : cg::Referee {
    void init() override {
        generateBoard();
        sendGlobalInfo();
        setFrameDuration(500);
        cg::gm.setMaxTurns(Config::MAX_TURNS);
        cg::gm.setTurnMaxTime(TURN_MAX_TIME);
        cg::gm.setFirstTurnMaxTime(FIRST_TURN_MAX_TIME);
    }
    void gameTurn(int turn) override {
        resetGameTurnData();
        if (isKeyFrame()) {
            for (Player* player : getActivePlayers()) {
                std::string in;
                getCurrentFrameInfoFor(*player, in);
                sendInput(*player, in);
                player->execute();
            }
            handlePlayerCommands();
        }
        try {
            performGameUpdate();
        } catch (const NullPointerException&) {
            // Referee.gameTurn catches it: "Referee failed to compute turn", abort()
            fprintf(stderr, "java.lang.NullPointerException (turn %d)\n", turn);
            cg::gm.endGame();
            return;
        }
        if (getActivePlayers().size() < 2) cg::gm.endGame();
        if (trace) fprintf(trace, "  turn %d: points %d %d\n", turn, players[0].points, players[1].points);
    }
    void onEnd() override { ::onEnd(); }
    // ViewModule
    void onGameInit() override {
        sendGlobalData();
        sendFrameData();
    }
    void onAfterGameTurn() override { sendFrameData(); }
    // EndScreenModule
    void onAfterOnEnd() override { cg::gm.setViewData("endScreen", cg::Json::raw(endScreenJson)); }
};

int main(int argc, char** argv) {
    static AntsReferee referee;
    cg::Options options;
    options.maxPlayers = 2;
    options.agent.firstTurnHeadStartMs = FIRST_TURN_HEAD_START;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
