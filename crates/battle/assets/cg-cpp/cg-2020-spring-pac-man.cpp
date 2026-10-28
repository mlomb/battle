// Spring Challenge 2020 (Pac-Man) - headless C++ referee, port of
// https://github.com/CodinGame/SpringChallenge2020 (commit 4525ae4, 2020-05-19, with the post-contest
// game updates), CodinGame game engine 3.15.0, 2 players, league 19 (the CLI sets the league to 19:
// the last league's rules, with the fog of war, the wrapping map, body blocking, SPEED, SWITCH, 4
// cherries, 2 to 5 pacs each and the dead pacs in the inputs).
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
// Engine 3.15.0: its core and runner classes are those of 4.0.2-4.5.0 (what common/engine.h models by
// default) except that 3.x has no getRandom() (the game makes its own java.util.Random from the seed)
// and that the runner words one failure message differently (only the END line of the -l trace shows
// it). The jar is built with referees/cg-builder's CommandLineInterface-v3.java (3.15.0's runner
// returns its older GameResult).
//
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption,
// int semantics, error texts. Notable Java behaviours reproduced here:
//   - new java.util.Random(seed): map size, the tetris-based map (Collections.shuffle of the 22 pieces
//     for every free cell, the second fitting piece), mirrored, islands other than the first largest
//     one walled up (neighbours wrap horizontally), a border with tunnels, then the number of pacs,
//     the shuffled free cells (pacs and cherries), and one nextInt(2) per pac pair for the sides.
//   - Commands: the CASE_INSENSITIVE regexes of CommandManager on each trimmed '|'-separated part (only
//     ASCII letters fold, \s is [ \t\n\x0B\f\r], \d is [0-9]): MOVE, SPEED, SWITCH and WAIT with
//     optional messages (cut to 46 chars + "..."), the checks in their order (unknown or dead pac,
//     commanded twice, out of the grid, Integer.parseInt/valueOf overflows), commands parsed before a
//     disqualifying one keep their effect.
//   - The rules in their order, with the referee's quirks: A* paths with java.util.PriorityQueue tie
//     orders, the nearest reachable cell for unreachable targets, body blocking resolved in passes
//     with the previous/current path steps (stale ones included), kills, pellets and cherries eaten
//     once per player, speed sub-turns (extra frames, the turn limit grows), the end when no player
//     can improve its rank, the remaining pellets to the last player standing.
//   - The view data: the jar fails when it passes 1 MB in total (reachable: e.g. messages full of
//     control chars that change every turn), so it is rebuilt here: the GraphicEntityModule of
//     engine 3.15.0, its ToggleModule (sends "[{}]" every frame) and EndScreenModule (common/gem.h),
//     the game's TooltipModule (the engine's 3.15.2+ one, sending only new registrations, plus
//     global data) and AnimatedEventModule (Double.toString coordinates, fdlibm Math.atan2 angles).
//   - Engine (common/engine.h): the protocol between the runner and the referee (java.util.Scanner
//     lines, 30000-byte commands: a long invalid command makes a too big summary or tooltip and the
//     game fails), the 1 MB view data quota, the game summary quotas; the bots get 1000 ms on their
//     first turn and 50 ms after.
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps, so a
// bot answering in ~50 ms may be timed out by one and not the other; on a loaded machine the jar
// times out bots that answered in time). The order of the entities in a frame and of the ids in a
// group's children (identity hash codes in the jar: only their size matters). When a player is the
// last one standing, the view sends the remaining pellets to its pacs picked with an unseeded
// java.util.Random: the size of that last frame can differ by a few bytes (a game whose view data
// ends within those bytes of the 1 MB quota could fail in one referee and not the other).
// Validated (2026-10-25) with battle referee-diff against the jar on 6334 games (32 bots, every ordered
// pairing, seeds from 1, 7, 1000001, 987654321, 3000000007 and 5000000000, plus a view-quota bot against
// all; every mismatch left was the loaded jar timing out a bot that had answered, identical when
// replayed alone) and in direct mode (jar game log vs -l trace, every view frame's size and text) on 720
// games, including raw line endings, invalid UTF-8, U+2028/U+0085, an over-sized summary and the 1 MB view
// data quota.
#include "common/engine.h"
#include "common/gem.h"

#include <cstdarg>
#include <map>
#include <memory>
#include <numeric>

using java::String;

// ----------------------------------------------------------------------------
// Config.java with LeagueRules.fromIndex(19)
// ----------------------------------------------------------------------------
namespace Config {
const int CHERRY_SCORE = 10;
const int MAP_MIN_WIDTH = 28, MAP_MAX_WIDTH = 33, MAP_MIN_HEIGHT = 10, MAP_MAX_HEIGHT = 15;
const int PACMAN_BASE_SPEED = 1, SPEED_BOOST = 2, ABILITY_DURATION = 6, ABILITY_COOLDOWN = 10;
const int NUMBER_OF_CHERRIES = 4;
const int MIN_PACS_PER_PLAYER = 2, MAX_PACS_PER_PLAYER = 5;
// FOG_OF_WAR, MAP_WRAPS, BODY_BLOCK, FRIENDLY_BODY_BLOCK, SPEED_ABILITY_AVAILABLE,
// SWITCH_ABILITY_AVAILABLE and PROVIDE_DEAD_PACS are all true
} // namespace Config

// Referee
const int MAX_TURNS = 200;
const char* const EXPECTED = "MOVE <id> <x> <y> or SPEED <id> or SWITCH <id> <type(ROCK|PAPER|SCISSORS)>";

// ----------------------------------------------------------------------------
// Coord.java, Cell.java, Grid.java
// ----------------------------------------------------------------------------
struct Coord {
    int x = 0, y = 0;
    Coord() {}
    Coord(int x_, int y_) : x(x_), y(y_) {}
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    bool operator<(const Coord& o) const { return x != o.x ? x < o.x : y < o.y; }
    i32 hashCode() const { return 31 * (31 + x) + y; }
    int manhattanTo(const Coord& o) const { return std::abs(o.x - x) + std::abs(o.y - y); }
    Coord add(const Coord& d) const { return Coord(x + d.x, y + d.y); }
    Coord subtract(const Coord& d) const { return Coord(x - d.x, y - d.y); }
    Coord getUnitVector() const { return Coord(x == 0 ? 0 : x / std::abs(x), y == 0 ? 0 : y / std::abs(y)); }
    // euclideanTo: Math.sqrt(Math.pow(dx, 2) + Math.pow(dy, 2))
    double euclideanTo(int ox, int oy) const {
        double dx = ox - x, dy = oy - y;
        return std::sqrt(dx * dx + dy * dy);
    }
};

static const Coord ADJACENCY[4] = {Coord(-1, 0), Coord(1, 0), Coord(0, -1), Coord(0, 1)};

enum CellType { NO_TYPE, WALL, FLOOR }; // a new Cell has no type (null)

struct Cell {
    int type = NO_TYPE;
    bool valid = true; // false for Cell.NO_CELL
    bool hasPellet = false, hasCherry = false;
    bool isFloor() const { return type == FLOOR; }
    bool isWall() const { return type == WALL; }
    void copy(const Cell& source) {
        type = source.type;
        hasPellet = source.hasPellet;
    }
};

struct Grid {
    int width = 0, height = 0;
    std::vector<Cell> cells; // the LinkedHashMap<Coord, Cell>: row by row
    static Cell NO_CELL;
    Grid() {}
    Grid(int w, int h) : width(w), height(h), cells((size_t)w * h) {}
    Cell& get(int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) return NO_CELL;
        return cells[(size_t)y * width + x];
    }
    Cell& get(const Coord& c) { return get(c.x, c.y); }
    Coord coordOf(size_t i) const { return Coord((int)(i % width), (int)(i / width)); }
    // getCoordNeighbour with MAP_WRAPS
    bool getCoordNeighbour(const Coord& pos, const Coord& delta, Coord& out) {
        Coord n = pos.add(delta);
        n = Coord((n.x + width) % width, n.y);
        if (&get(n) != &NO_CELL) {
            out = n;
            return true;
        }
        return false;
    }
    int getNeighbours(const Coord& pos, Coord* out) {
        int n = 0;
        for (const Coord& d : ADJACENCY)
            if (getCoordNeighbour(pos, d, out[n])) n++;
        return n;
    }
    int calculateDistance(const Coord& a, const Coord& b) const {
        int dv = std::abs(a.y - b.y);
        int dh = std::min(std::abs(a.x - b.x), std::min(a.x + width - b.x, b.x + width - a.x));
        return dv + dh;
    }
};
Cell Grid::NO_CELL = [] {
    Cell c;
    c.valid = false;
    return c;
}();

// ----------------------------------------------------------------------------
// maps/TetrisBasedMapGenerator.java
// ----------------------------------------------------------------------------
struct TetrisPiece {
    std::vector<Coord> blocks; // a HashSet<Coord>: only membership is used
    int maxX = 0, maxY = 0;
    explicit TetrisPiece(const std::vector<Coord>& cells) : blocks(cells) {
        maxX = maxY = INT32_MIN;
        for (const Coord& c : cells) {
            maxX = std::max(maxX, c.x);
            maxY = std::max(maxY, c.y);
        }
    }
    bool contains(const Coord& c) const { return std::find(blocks.begin(), blocks.end(), c) != blocks.end(); }
};

struct MapGenerator {
    std::vector<TetrisPiece> pieces;

    static TetrisPiece flipX(const TetrisPiece& t) {
        std::vector<Coord> r;
        for (const Coord& c : t.blocks) r.push_back(Coord(t.maxX - c.x, c.y));
        return TetrisPiece(r);
    }
    static TetrisPiece flipY(const TetrisPiece& t) {
        std::vector<Coord> r;
        for (const Coord& c : t.blocks) r.push_back(Coord(c.x, t.maxY - c.y));
        return TetrisPiece(r);
    }
    static TetrisPiece transpose(const TetrisPiece& t) {
        std::vector<Coord> r;
        for (const Coord& c : t.blocks) r.push_back(Coord(c.y, c.x));
        return TetrisPiece(r);
    }

    void init() {
        pieces.clear();
        pieces.push_back(TetrisPiece({Coord(0, 0), Coord(1, 0), Coord(0, 1), Coord(1, 1)}));
        TetrisPiece t({Coord(0, 0), Coord(0, 1), Coord(1, 1)});
        pieces.push_back(t);
        pieces.push_back(flipX(t));
        pieces.push_back(flipY(t));
        pieces.push_back(transpose(t));
        t = TetrisPiece({Coord(0, 0), Coord(0, 1), Coord(1, 1), Coord(0, 2)});
        pieces.push_back(t);
        pieces.push_back(flipX(t));
        pieces.push_back(transpose(t));
        pieces.push_back(flipY(transpose(t)));
        t = TetrisPiece({Coord(1, 0), Coord(1, 1), Coord(2, 1), Coord(1, 2), Coord(0, 1)});
        pieces.push_back(t);
        t = TetrisPiece({Coord(0, 0), Coord(0, 1), Coord(1, 1), Coord(2, 1)});
        pieces.push_back(t);
        pieces.push_back(flipX(t));
        pieces.push_back(flipY(t));
        pieces.push_back(flipX(flipY(t)));
        pieces.push_back(flipX(flipY(transpose(t))));
        pieces.push_back(transpose(t));
        pieces.push_back(flipY(transpose(t)));
        pieces.push_back(flipX(transpose(t)));
    }

    void generate(Grid& grid, java::Random& random) {
        int generateWidth = grid.width / 2 + 1;
        int generateHeight = grid.height / 2 + 1;
        std::map<Coord, TetrisPiece> generatedPieces;
        std::map<Coord, Coord> blockOrigin;
        auto occupied = [&](const Coord& c) { return blockOrigin.count(c) > 0; };
        for (int y = 0; y < generateHeight; ++y) {
            for (int x = 0; x < generateWidth; ++x) {
                Coord pos(x, y);
                if (!occupied(pos)) {
                    java::shuffle(pieces, random);
                    // Take the first available piece, unless it is the only one
                    int fits = 0;
                    const TetrisPiece* piece = nullptr;
                    for (const TetrisPiece& p : pieces) {
                        bool ok = true;
                        for (const Coord& c : p.blocks)
                            if (occupied(Coord(pos.x + c.x, pos.y + c.y))) ok = false;
                        if (ok && fits++ == 1) {
                            piece = &p;
                            break;
                        }
                    }
                    if (piece) {
                        generatedPieces.insert_or_assign(pos, *piece);
                        for (const Coord& c : piece->blocks) blockOrigin[Coord(pos.x + c.x, pos.y + c.y)] = pos;
                    }
                }
            }
        }
        for (Cell& c : grid.cells) c.type = WALL;
        for (int y = 1; y < generateHeight; ++y) {
            for (int x = 1; x < generateWidth; ++x) {
                Coord pos(x, y);
                auto o = blockOrigin.find(pos);
                Coord gridPos(x * 2 - 1, y * 2 - 1);
                if (o != blockOrigin.end()) {
                    const Coord origin = o->second;
                    const TetrisPiece& piece = generatedPieces.at(origin);
                    Coord block(pos.x - origin.x, pos.y - origin.y);
                    for (const Coord& delta : ADJACENCY) {
                        Coord adj(block.x + delta.x, block.y + delta.y);
                        if (!piece.contains(adj)) {
                            for (int i = 0; i < 3; ++i) {
                                Coord cellPos = delta.x == 0 ? Coord(gridPos.x - 1 + i, gridPos.y + delta.y) : Coord(gridPos.x + delta.x, gridPos.y - 1 + i);
                                Cell& cell = grid.get(cellPos);
                                if (cell.valid) cell.type = FLOOR;
                            }
                        }
                    }
                }
            }
        }
    }

    void generateWithHorizontalSymetry(Grid& grid, java::Random& random) {
        int w = grid.width / 2 + 1;
        int h = grid.height;
        Grid miniGrid(w, h);
        generate(miniGrid, random);
        for (size_t i = 0; i < miniGrid.cells.size(); i++) {
            Coord source = miniGrid.coordOf(i);
            grid.get(source).copy(miniGrid.cells[i]);
            grid.get(Coord(grid.width - source.x - 1, source.y)).copy(miniGrid.cells[i]);
        }
        // detectIslands (neighbours wrap): every island but the first largest one is walled up
        std::vector<int> islandOf(grid.cells.size(), -1);
        std::vector<std::vector<int>> islands;
        for (size_t i = 0; i < grid.cells.size(); i++) {
            if (!grid.cells[i].isFloor() || islandOf[i] >= 0) continue;
            int id = (int)islands.size();
            islands.push_back({(int)i});
            islandOf[i] = id;
            for (size_t k = 0; k < islands[id].size(); k++) {
                Coord e = grid.coordOf(islands[id][k]);
                Coord ns[4];
                int n = grid.getNeighbours(e, ns);
                for (int j = 0; j < n; j++) {
                    size_t ni = (size_t)ns[j].y * grid.width + ns[j].x;
                    if (islandOf[ni] < 0 && grid.cells[ni].isFloor()) {
                        islandOf[ni] = id;
                        islands[id].push_back((int)ni);
                    }
                }
            }
        }
        // sorted by decreasing size (stable): the first largest stays
        size_t keep = 0;
        for (size_t k = 1; k < islands.size(); k++)
            if (islands[k].size() > islands[keep].size()) keep = k;
        for (size_t k = 0; k < islands.size(); k++)
            if (k != keep)
                for (int c : islands[k]) grid.cells[c].type = WALL;
    }
};

// ----------------------------------------------------------------------------
// Player.java, Pacman.java, actions
// ----------------------------------------------------------------------------
enum PacmanType { ROCK, PAPER, SCISSORS, NEUTRAL };
static const char* const TYPE_NAME[4] = {"ROCK", "PAPER", "SCISSORS", "NEUTRAL"};
enum AbilityType { NO_ABILITY = -1, SPEED, SET_ROCK, SET_PAPER, SET_SCISSORS };
enum ActionType { AT_WAIT, AT_MOVE, AT_MSG, AT_SPEED, AT_SWITCH };
enum Intent { NO_ACTION, MOVE_INTENT, SPEED_INTENT, SWITCH_INTENT };

struct Player;

struct Pacman {
    Player* owner = nullptr;
    int id = 0, number = 0;
    bool hasMessage = false;
    String message;
    Coord position;
    int speed = 1, abilityDuration = 0, abilityCooldown = 0;
    bool endOfSpeed = false;
    Intent intent = NO_ACTION;
    Coord target;         // MoveAction
    int switchType = 0;   // SwitchAction
    int abilityToUse = NO_ABILITY;
    std::vector<Coord> intendedPath;
    bool hasWarning = false;
    std::string warning;
    int type = ROCK;
    int currentPathStep = 0, previousPathStep = 0;
    bool blocked = false, dead = false;
    std::vector<String> gameSummary;

    ActionType actionType() const { return intent == MOVE_INTENT ? AT_MOVE : intent == SPEED_INTENT ? AT_SPEED : intent == SWITCH_INTENT ? AT_SWITCH : AT_WAIT; }
    void setMessage(const String& m) {
        hasMessage = true;
        message = m;
        if (m.size() > 48) message = m.substr(0, 46) + u"...";
    }
    void tickAbilityDuration() {
        if (abilityDuration > 0) {
            abilityDuration--;
            endOfSpeed = abilityDuration == 0;
        } else {
            endOfSpeed = false;
        }
    }
    void tickAbilityCooldown() {
        if (abilityCooldown > 0) abilityCooldown--;
    }
    void setCurrentPathStep(int step) {
        previousPathStep = currentPathStep;
        currentPathStep = step;
    }
    void turnReset() {
        hasMessage = false;
        message.clear();
        if (!dead) {
            tickAbilityDuration();
            tickAbilityCooldown();
        }
        abilityToUse = NO_ABILITY;
        setCurrentPathStep(0);
        blocked = false;
        intent = NO_ACTION;
    }
    bool moveFinished() const { return currentPathStep == (int)intendedPath.size() - 1; }
    bool fastEnoughToMoveAt(int step) const { return speed > step; }
    bool isSpeeding() const { return speed == Config::SPEED_BOOST; }
    void addToGameSummary(const String& m) { gameSummary.push_back(m); }
    void addToGameSummary(const std::string& m) { gameSummary.push_back(java::ascii(m)); }
};

struct Player : cg::Player {
    std::vector<Pacman*> pacmen;
    int pellets = 0;
    bool timedOut = false;
    int getExpectedOutputLines() const override { return 1; }
    const char* getColor() const { return index == 0 ? "red" : "blue"; }
    bool hasAlivePacmen() const {
        for (Pacman* p : pacmen)
            if (!p->dead) return true;
        return false;
    }
    int getColorToken() const { return -(index + 1); }
};

static Player players[2];

static std::vector<Player*> activePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

static std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ----------------------------------------------------------------------------
// pathfinder/AStar.java, PathFinder.java
// ----------------------------------------------------------------------------
struct PathFinderResult {
    std::vector<Coord> path;
    bool isNearest = false;
};

struct AStar {
    struct PathItem {
        int cumulativeLength = 0, totalPrevisionalLength = 0, precedent = -1;
        Coord coord;
    };
    Grid& grid;
    Coord from, target, nearest;
    std::vector<PathItem> items;
    std::vector<char> closed;

    AStar(Grid& g, Coord f, Coord t) : grid(g), from(f), target(t), nearest(f), closed(g.cells.size(), 0) {}

    std::vector<Coord> find() {
        int item = getPathItemLinkedList();
        std::vector<Coord> path;
        for (int i = item; i >= 0; i = items[i].precedent) path.push_back(items[i].coord);
        std::reverse(path.begin(), path.end());
        return path;
    }

    int getPathItemLinkedList() {
        auto cmp = [this](int a, int b) { return java::compareInt(items[a].totalPrevisionalLength, items[b].totalPrevisionalLength); };
        java::PriorityQueue<int, decltype(cmp)> openList(cmp);
        PathItem root;
        root.coord = from;
        items.push_back(root);
        openList.offer(0);
        while (openList.size() > 0) {
            int visiting = openList.poll();
            Coord visitingCoord = items[visiting].coord;
            if (visitingCoord == target) return visiting;
            size_t vi = (size_t)visitingCoord.y * grid.width + visitingCoord.x;
            if (closed[vi]) continue;
            closed[vi] = 1;
            Coord ns[4];
            int n = grid.getNeighbours(visitingCoord, ns);
            for (int k = 0; k < n; k++) {
                if (grid.get(ns[k]).type != FLOOR) continue;
                // addToOpenList
                if (closed[(size_t)ns[k].y * grid.width + ns[k].x]) continue;
                PathItem pi;
                pi.coord = ns[k];
                pi.cumulativeLength = items[visiting].cumulativeLength + 1;
                pi.totalPrevisionalLength = pi.cumulativeLength + grid.calculateDistance(visitingCoord, ns[k]);
                pi.precedent = visiting;
                items.push_back(pi);
                openList.offer((int)items.size() - 1);
            }
            if (grid.calculateDistance(visitingCoord, target) < grid.calculateDistance(nearest, target)) nearest = visitingCoord;
        }
        return -1;
    }
};

static PathFinderResult findPath(Grid& grid, Coord from, Coord to) {
    AStar a(grid, from, to);
    std::vector<Coord> path = a.find();
    PathFinderResult pfr;
    if (path.empty()) {
        pfr.isNearest = true;
        AStar b(grid, from, a.nearest);
        path = b.find();
    }
    pfr.path = path;
    return pfr;
}

// ----------------------------------------------------------------------------
// The view (view/View.java, GridView.java, PacmanView.java, event/AnimatedEventModule.java,
// tooltip/TooltipModule.java; the engine's modules are in common/gem.h): only the size of its data
// matters
// ----------------------------------------------------------------------------
struct ViewerEvent {
    double t;
    std::string id;
    std::vector<std::pair<std::string, std::string>> params; // (key, String.valueOf(value))
    void put(const std::string& k, double v) { params.push_back({k, java::doubleToString(v)}); }
    void putInt(const std::string& k, i64 v) { params.push_back({k, std::to_string(v)}); }
    void putStr(const std::string& k, const std::string& v) { params.push_back({k, v}); }
    std::string toString() const {
        std::vector<std::pair<std::string, std::string>> p = params;
        std::stable_sort(p.begin(), p.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string s;
        for (size_t i = 0; i < p.size(); i++) s += (i ? " " : "") + p[i].second;
        return java::formatDecimal(t, 6) + " " + id + " " + s;
    }
};

struct PacmanView {
    Pacman* model = nullptr;
    gem::Entity *group = nullptr, *sprite = nullptr, *death = nullptr, *message = nullptr, *switchFX = nullptr, *deathFX = nullptr;
    gem::Entity *rotationWrapper = nullptr, *cooldownBar = nullptr, *cooldownBarBackground = nullptr, *plasmaBallContainer = nullptr;
    PacmanView* clone = nullptr; // View.pacmenWrapClones
};

static const char* folderNameFromType(int type) {
    switch (type) {
        case NEUTRAL: return "1";
        case SCISSORS: return "2";
        case PAPER: return "3";
        default: return "4";
    }
}
static std::vector<std::string> getDeathPacmanImages(int playerIndex, int type) {
    std::vector<std::string> r;
    for (int i = 1; i < 8; i++) r.push_back(fmt("mort_%s_%s_mort%04d", playerIndex == 0 ? "red" : "blue", folderNameFromType(type), i));
    return r;
}
static std::vector<std::string> getWalkPacmanImages(int playerIndex, int type, bool alive) {
    static const int WALK_CYCLE[6] = {1, 2, 3, 4, 3, 2};
    std::vector<std::string> r;
    for (int c : WALK_CYCLE)
        r.push_back(std::string("paku_") + (playerIndex == 0 ? "red" : "blue") + "_" + folderNameFromType(type) + "_walk000" + std::to_string(c) + (alive ? "" : "_2"));
    return r;
}
static std::vector<std::string> getFxImages(int startNumber, int endNumber, const char* mask) {
    std::vector<std::string> r;
    for (int i = startNumber; i < endNumber + 1; i++) r.push_back(fmt("%s%04d", mask, i));
    return r;
}

struct View {
    gem::Module gem;
    // modules
    std::vector<ViewerEvent> animEvents; // AnimatedEventModule
    double eventScale = 1;
    int speedboostGroupId = 0;
    std::vector<int> plasmaBallIds;
    gem::TooltipModule tooltips{false};           // the game's TooltipModule (3.15.2+ behaviour)
    int tooltipGlobal[5] = {0, 0, 0, 0, 0};      // width, height, x, y, cellSize
    double tooltipCoefficient = 0;
    gem::ToggleModule toggles;

    static const int CELL_SIZE = 40;
    gem::Entity *gameZone = nullptr, *background = nullptr, *pacMask = nullptr;
    std::vector<PacmanView> pacmen; // by pac id
    std::vector<PacmanView> clones; // View.pacmenWrapClones
    std::map<Coord, gem::Entity*> pellets;
    std::vector<gem::Entity*> scoreLabels, speedIndicators;
    Grid* gridModel = nullptr;
    int gridWidth = 0; // GridView.width

    std::vector<std::pair<Pacman*, int>> switchers; // (pac, original type), a HashMap
    struct Move {
        Pacman* pac;
        Coord from, to;
    };
    struct Bump {
        Pacman* pac;
        Coord from, to;
        int distanceToBlocker;
    };
    struct Flash {
        Pacman* pac;
        double time;
    };
    std::vector<Move> movers;
    std::vector<Pacman*> goners, cooldownLaunchers;
    std::vector<Bump> bumpers;
    std::vector<Flash> flashers;
    std::vector<PacmanView*> dying;

    ViewerEvent& createAnimationEvent(const std::string& id, double t) {
        animEvents.push_back(ViewerEvent{t, id, {}});
        return animEvents.back();
    }

    void setViewState(PacmanView& pv, int playerIndex, int type, bool alive) {
        gem.setImages(pv.sprite, getWalkPacmanImages(playerIndex, type, alive));
        if (!alive && pv.death) gem.setImages(pv.death, getDeathPacmanImages(playerIndex, type));
    }

    double gzScaleX() const { return gameZone->scaleX; }
    double gzScaleY() const { return gameZone->scaleY; }
    void setToGridCoordinates(gem::Entity* e, const Coord& p) {
        gem.setX(e, p.x * CELL_SIZE);
        gem.setY(e, p.y * CELL_SIZE);
    }
    void setToGridCenterCoordinates(gem::Entity* e, const Coord& p) {
        gem.setX(e, p.x * CELL_SIZE + CELL_SIZE / 2);
        gem.setY(e, p.y * CELL_SIZE + CELL_SIZE / 2);
    }
    void setToGridCenterCoordinates(gem::Entity* e, double px, double py) {
        gem.setX(e, java::d2i(px * CELL_SIZE + CELL_SIZE / 2));
        gem.setY(e, java::d2i(py * CELL_SIZE + CELL_SIZE / 2));
    }
    void setToAbsoluteCenterWithOffset(gem::Entity* e, double px, double py, double offsetX, double offsetY) {
        gem.setX(e, java::d2i((px + offsetX + 0.5) * (gzScaleX() * CELL_SIZE) + gameZone->x));
        gem.setY(e, java::d2i((py + offsetY + 0.5) * (gzScaleY() * CELL_SIZE) + gameZone->y));
    }
    double convertXFromGridToAbsoluteCenter(double x) const { return (x + 0.5) * (gzScaleX() * CELL_SIZE) + gameZone->x; }
    double convertYFromGridToAbsoluteCenter(double y) const { return (y + 0.5) * (gzScaleY() * CELL_SIZE) + gameZone->y; }

    std::string getTooltipText(const Pacman* m) const;
    void updateTooltipText(PacmanView& pv) { tooltips.setTooltipText(pv.sprite, java::ascii(getTooltipText(pv.model))); }

    gem::Entity* wrapGroupAround(gem::Entity* entity) {
        gem::Entity* wrapper = gem.createGroup();
        gem::Entity* parent = entity->parent;
        if (parent) gem.remove(parent, entity);
        gem.add(wrapper, {entity});
        if (parent) gem.add(parent, {wrapper});
        return wrapper;
    }

    PacmanView createPacmanGroup(Pacman* m, bool alive, bool clone, int frameDuration) {
        PacmanView pv;
        pv.model = m;
        pv.sprite = gem.createSpriteAnimation();
        gem.setDuration(pv.sprite, frameDuration);
        gem.setLoop(pv.sprite, true);
        gem.setVisible(pv.sprite, true);
        gem.setPlaying(pv.sprite, false);
        gem.setAnchor(pv.sprite, 0.5);
        gem.setMask(pv.sprite, pacMask);
        gem.setZIndex(pv.sprite, 0);

        pv.switchFX = gem.createSpriteAnimation();
        gem.setImages(pv.switchFX, getFxImages(1, 20, "transition"));
        gem.setVisible(pv.switchFX, false);
        gem.setZIndex(pv.switchFX, 1);
        gem.setAnchor(pv.switchFX, 0.5);
        gem.setBlendMode(pv.switchFX, gem::BlendMode::ADD);
        gem.setScale(pv.switchFX, 0.5);

        pv.message = gem.createText();
        gem.setFontSize(pv.message, 30);
        gem.setFontFamily(pv.message, "Arial");
        gem.setAnchor(pv.message, 0.5);
        gem.setFillColor(pv.message, m->owner->getColorToken());
        gem.setMaxWidth(pv.message, 136);
        gem.setStrokeThickness(pv.message, 4);
        gem.setZIndex(pv.message, 4);
        gem.setVisible(pv.message, true);
        setToAbsoluteCenterWithOffset(pv.message, m->position.x, m->position.y, 0, -0.82);

        pv.plasmaBallContainer = gem.createGroup();
        gem.setAlpha(pv.plasmaBallContainer, 0);
        gem.setZIndex(pv.plasmaBallContainer, 2);
        plasmaBallIds.push_back(pv.plasmaBallContainer->id);

        pv.group = gem.createGroup({pv.plasmaBallContainer, pv.switchFX, pv.sprite});
        gem.setZIndex(pv.group, 10);

        if (!clone) {
            pv.cooldownBarBackground = gem.createRectangle();
            gem.setScale(pv.cooldownBarBackground, 0.5);
            gem.setWidth(pv.cooldownBarBackground, 100);
            gem.setHeight(pv.cooldownBarBackground, 15);
            gem.setLineWidth(pv.cooldownBarBackground, 3);
            gem.setFillColor(pv.cooldownBarBackground, 0x000000);
            gem.setZIndex(pv.cooldownBarBackground, 2);
            gem.setVisible(pv.cooldownBarBackground, false);

            pv.cooldownBar = gem.createRectangle();
            gem.setScale(pv.cooldownBar, 0.5);
            gem.setWidth(pv.cooldownBar, 100);
            gem.setHeight(pv.cooldownBar, 15);
            gem.setLineColor(pv.cooldownBar, 0x000000);
            gem.setLineWidth(pv.cooldownBar, 3);
            gem.setFillColor(pv.cooldownBar, 0x90c43b);
            gem.setVisible(pv.cooldownBar, false);
            gem.setZIndex(pv.cooldownBar, 3);

            setToAbsoluteCenterWithOffset(pv.cooldownBar, m->position.x, m->position.y, -0.5, -0.6);
            setToAbsoluteCenterWithOffset(pv.cooldownBarBackground, m->position.x, m->position.y, -0.5, -0.6);
        }

        gem::Entity* baseScaleWrapper = wrapGroupAround(pv.sprite);
        gem.setScale(baseScaleWrapper, 0.67);
        pv.rotationWrapper = wrapGroupAround(pv.sprite);

        if (!clone && m->type != NEUTRAL) {
            pv.death = gem.createSpriteAnimation();
            gem.setImages(pv.death, getDeathPacmanImages(m->owner->index, m->type));
            gem.setDuration(pv.death, frameDuration / 2);
            gem.setLoop(pv.death, false);
            gem.setPlaying(pv.death, false);
            gem.setVisible(pv.death, false);
            gem.setAnchor(pv.death, 0.5);
            gem.setMask(pv.death, pacMask);

            pv.deathFX = gem.createSpriteAnimation();
            gem.setImages(pv.deathFX, getFxImages(28, 39, "mort_FX"));
            gem.setVisible(pv.deathFX, false);
            gem.setLoop(pv.deathFX, false);
            gem.setPlaying(pv.deathFX, false);
            gem.setZIndex(pv.deathFX, 1);
            gem.setAnchor(pv.deathFX, 0.5);
            gem.setBlendMode(pv.deathFX, gem::BlendMode::ADD);
            gem.setDuration(pv.deathFX, frameDuration / 2);

            if (pv.sprite->parent) gem.add(pv.sprite->parent, {pv.death, pv.deathFX});
        }
        setViewState(pv, m->owner->index, m->type, alive);
        gem.commitEntityState(0, {baseScaleWrapper});
        return pv;
    }

    void init(std::vector<Pacman*>& pacmenModel, Grid& grid, int frameDuration);
    void initGrid(Grid& grid);
    void initHud();
    void createTunnelMask(const Coord& coord, int cellSize, int padding, double cornerSize, gem::Entity*& mask);

    // --- the game's calls
    void eatPellet(const Coord& coord, int pelletValue) {
        gem::Entity* pellet = pellets.at(coord);
        gem.setVisible(pellet, false);
        ViewerEvent& ev = createAnimationEvent("splash", 1);
        ev.put("x", convertXFromGridToAbsoluteCenter(coord.x));
        ev.put("y", convertYFromGridToAbsoluteCenter(coord.y));
        ev.putInt("v", pelletValue);
    }
    void bumpPacman(Pacman* m, const Coord& from, const Coord& to, int d) { bumpers.push_back(Bump{m, from, to, d}); }
    void flashPacman(Pacman* m, double time) { flashers.push_back(Flash{m, time}); }
    void movePacman(Pacman* m, const Coord& from, const Coord& to) { movers.push_back(Move{m, from, to}); }
    void changePacmanType(Pacman* m, int originalType) {
        for (auto& s : switchers)
            if (s.first == m) {
                s.second = originalType;
                return;
            }
        switchers.push_back({m, originalType});
    }
    void addCooldownAnimation(Pacman* m) { cooldownLaunchers.push_back(m); }

    void launchBumpFx(const Coord& fromA, const Coord& fromB, const Coord& to, int) {
        double mid = 0.5, x, y;
        if (fromA.x == fromB.x || fromA.y == fromB.y) {
            if (std::abs(fromA.x - fromB.x) == gridWidth - 1) return; // both pacs on a border
            double meanX = ((fromA.x + fromB.x) / 2.0);
            double meanXWrap = std::fmod((fromA.x + fromB.x + gridWidth) / 2.0, (double)gridWidth);
            double meanY = ((fromA.y + fromB.y) / 2.0);
            bool wrap = std::abs(fromA.x - fromB.x) > gridWidth / 2.0;
            x = convertXFromGridToAbsoluteCenter(wrap ? meanXWrap : meanX);
            y = convertYFromGridToAbsoluteCenter(meanY);
        } else {
            x = convertXFromGridToAbsoluteCenter(to.x);
            y = convertYFromGridToAbsoluteCenter(to.y);
        }
        double angle = java::atan2(fromB.y - fromA.y, fromB.x - fromA.x);
        ViewerEvent& ev = createAnimationEvent("play", mid);
        ev.put("x", x);
        ev.put("y", y);
        ev.putStr("name", "choc");
        ev.put("a", angle);
    }

    void launchFlashFx(Pacman* m, double time) {
        PacmanView& pac = pacmen[m->id];
        createAnimationEvent("flash", time).putInt("id", pac.sprite->id);
        createAnimationEvent("flash", time).putInt("id", pac.clone->sprite->id);
    }

    void launchSpeedBoostFx(Pacman* m, double fx, double fy, double tx, double ty, double startTime, double endTime, bool flip) {
        PacmanView& pac = pacmen[m->id];
        ViewerEvent& ev = createAnimationEvent("speed", startTime);
        ev.putInt("color", m->owner->index);
        ev.putInt("id", pac.sprite->id);
        ev.put("toX", tx * CELL_SIZE + CELL_SIZE / 2);
        ev.put("toY", ty * CELL_SIZE + CELL_SIZE / 2);
        ev.put("fromX", fx * CELL_SIZE + CELL_SIZE / 2);
        ev.put("fromY", fy * CELL_SIZE + CELL_SIZE / 2);
        ev.put("walkEnd", endTime);
        ev.putInt("walkFlip", flip ? 1 : 0);
    }

    void rotatePacman(PacmanView& pac, const Coord& from, const Coord& to, double t) {
        const double PI = 3.141592653589793;
        if (from.y > to.y) {
            gem.setRotation(pac.rotationWrapper, PI / 2 * pac.rotationWrapper->scaleX, gem::NONE);
        } else if (from.y < to.y) {
            gem.setRotation(pac.rotationWrapper, -PI / 2 * pac.rotationWrapper->scaleX, gem::NONE);
        } else if (from.x > to.x) {
            gem.setRotation(pac.rotationWrapper, 0, gem::NONE);
            gem.setScaleX(pac.rotationWrapper, 1, gem::NONE);
        } else if (from.x < to.x) {
            gem.setRotation(pac.rotationWrapper, 0, gem::NONE);
            gem.setScaleX(pac.rotationWrapper, -1, gem::NONE);
        }
        gem.commitEntityState(t, {pac.rotationWrapper});
    }

    void translatePacman(PacmanView& pac, double fx, double fy, double tx, double ty, const Coord& rotateFrom, const Coord& rotateTo, double startTime,
                         double endTime, bool flip) {
        rotatePacman(pac, rotateFrom, rotateTo, startTime);
        gem.setPlaying(pac.sprite, true);
        setToAbsoluteCenterWithOffset(pac.message, fx, fy, 0, -0.82);
        setToGridCenterCoordinates(pac.group, fx, fy);
        if (pac.cooldownBar) {
            setToAbsoluteCenterWithOffset(pac.cooldownBar, fx, fy, -0.5, -0.6);
            setToAbsoluteCenterWithOffset(pac.cooldownBarBackground, fx, fy, -0.5, -0.6);
            gem.commitEntityState(startTime, {pac.cooldownBar, pac.cooldownBarBackground});
        }
        gem.commitEntityState(startTime, {pac.group, pac.sprite, pac.rotationWrapper, pac.message});
        setToAbsoluteCenterWithOffset(pac.message, tx, ty, 0, -0.82);
        setToGridCenterCoordinates(pac.group, tx, ty);
        if (pac.cooldownBar) {
            setToAbsoluteCenterWithOffset(pac.cooldownBar, tx, ty, -0.5, -0.6);
            setToAbsoluteCenterWithOffset(pac.cooldownBarBackground, tx, ty, -0.5, -0.6);
        }
        if (pac.model->isSpeeding()) launchSpeedBoostFx(pac.model, fx, fy, tx, ty, startTime, endTime, flip);
    }
    void translatePacman(PacmanView& pac, const Coord& from, const Coord& to, const Coord& rotateFrom, const Coord& rotateTo, double startTime, double endTime) {
        translatePacman(pac, from.x, from.y, to.x, to.y, rotateFrom, rotateTo, startTime, endTime, false);
    }

    bool isInPain(Pacman* m) const { return std::find(goners.begin(), goners.end(), m) != goners.end(); }

    PacmanView& summonClone(PacmanView& pac, Pacman* m, bool alive) {
        PacmanView& clone = *pac.clone;
        setViewState(clone, m->owner->index, m->type, alive);
        gem.setVisible(clone.group, true);
        gem.setDuration(clone.sprite, pac.sprite->duration);
        return clone;
    }

    void animateDeath(PacmanView& pac) {
        gem.setVisible(pac.sprite, false);
        gem.setVisible(pac.death, true);
        gem.setPlaying(pac.death, true);
        gem.setVisible(pac.plasmaBallContainer, false);
        dying.push_back(&pac);
    }

    static void partWay(const Coord& from, const Coord& to, double progress, double& x, double& y) {
        x = from.x + (to.x - from.x) * progress;
        y = from.y + (to.y - from.y) * progress;
    }

    void animateBump(Pacman* m, const Coord& from, const Coord& to, int distanceToBlocker) {
        PacmanView& pac = pacmen[m->id];
        double progress = distanceToBlocker / 4.0;
        double midTime = 0.5;
        bool alive = !isInPain(m);
        if (to.manhattanTo(from) > 1) {
            Coord unitDiff = to.subtract(from).getUnitVector();
            Coord exitingCoord = from.subtract(unitDiff);
            Coord enteringCoord = to.add(unitDiff);
            double pfx = from.x, pfy = from.y, ptx, pty, cfx = enteringCoord.x, cfy = enteringCoord.y, ctx, cty;
            partWay(from, exitingCoord, progress, ptx, pty);
            partWay(enteringCoord, to, progress, ctx, cty);
            PacmanView& clone = summonClone(pac, m, alive);
            translatePacman(pac, pfx, pfy, ptx, pty, to, from, 0, midTime, false);
            translatePacman(clone, cfx, cfy, ctx, cty, to, from, 0, midTime, false);
            translatePacman(pac, ptx, pty, pfx, pfy, to, from, midTime, 1, true);
            translatePacman(clone, ctx, cty, cfx, cfy, to, from, midTime, 1, true);
            gem.setVisible(clone.group, false);
            if (!alive) animateDeath(pac);
        } else {
            double fx = from.x, fy = from.y, px, py;
            partWay(from, to, progress, px, py);
            translatePacman(pac, fx, fy, px, py, from, to, 0, midTime, false);
            translatePacman(pac, px, py, fx, fy, from, to, midTime, 1, true);
        }
    }

    void animateMove(Pacman* m, const Coord& from, const Coord& to) {
        PacmanView& pac = pacmen[m->id];
        bool alive = !isInPain(m);
        setViewState(pac, m->owner->index, m->type, alive);
        gem.setPlaying(pac.sprite, true);
        gem.commitEntityState(0, {pac.sprite});
        if (from.manhattanTo(to) > 1) { // the pac wraps
            Coord unitDiff = to.subtract(from).getUnitVector();
            Coord exitingCoord = from.subtract(unitDiff);
            Coord enteringCoord = to.add(unitDiff);
            PacmanView& clone = summonClone(pac, m, alive);
            translatePacman(pac, enteringCoord, to, to, from, 0, 1);
            translatePacman(clone, from, exitingCoord, to, from, 0, 1);
            gem.setVisible(clone.group, false);
            if (!alive) animateDeath(pac);
        } else {
            translatePacman(pac, from, to, from, to, 0, 1);
            if (!alive) animateDeath(pac);
        }
    }

    void sendShake(PacmanView& pac, double startTime, double endTime) {
        ViewerEvent& ev = createAnimationEvent("shake", startTime);
        ev.putInt("id", pac.group->id);
        ev.put("length", endTime - startTime);
    }

    void killPacman(Pacman* m) {
        goners.push_back(m);
        PacmanView& pac = pacmen[m->id];
        PacmanView& clone = summonClone(pac, m, false);
        gem.setVisible(clone.group, false);
        sendShake(pac, 0, 1);
        sendShake(clone, 0, 1);
        auto it = std::find(cooldownLaunchers.begin(), cooldownLaunchers.end(), m);
        if (it != cooldownLaunchers.end()) cooldownLaunchers.erase(it);
    }

    void bouncePacman(PacmanView& pac) {
        gem.setScale(pac.sprite, 1.3);
        gem.commitEntityState(9 / 35.0, {pac.sprite});
        gem.commitEntityState(26 / 35.0, {pac.sprite});
        gem.setScale(pac.sprite, 0.9);
        gem.commitEntityState(31 / 35.0, {pac.sprite});
        gem.setScale(pac.sprite, 1.35);
        gem.commitEntityState(33 / 35.0, {pac.sprite});
        gem.setScale(pac.sprite, 1);
        gem.commitEntityState(35 / 35.0, {pac.sprite});
    }

    void animateSwitch(PacmanView& pac, int originalType, Pacman* m, bool alive, int frameDuration) {
        double switchFXStart = 7 / 35.0;
        double switchFXDuration = 20 / 35.0;
        setViewState(pac, m->owner->index, originalType, alive);
        gem.commitEntityState(0, {pac.sprite});
        setViewState(pac, m->owner->index, m->type, alive);
        gem.reset(pac.switchFX);
        gem.setDuration(pac.switchFX, java::d2i(frameDuration * switchFXDuration));
        gem.setVisible(pac.switchFX, true);
        gem.setPlaying(pac.switchFX, true);
        gem.commitEntityState(switchFXStart, {pac.switchFX});
        gem.setVisible(pac.switchFX, false);
        gem.commitEntityState(switchFXStart + switchFXDuration, {pac.switchFX});
        bouncePacman(pac);
        if (!alive) animateDeath(pac);
    }

    void performCooldownAnimation() {
        int totalCooldown = Config::ABILITY_COOLDOWN - 1;
        std::vector<Pacman*> toRemove;
        for (Pacman* m : cooldownLaunchers) {
            int currentCooldown = m->abilityCooldown - 1;
            PacmanView& pac = pacmen[m->id];
            int width = pac.cooldownBarBackground->width;
            if (currentCooldown <= 0) {
                gem.setVisible(pac.cooldownBar, false);
                gem.setVisible(pac.cooldownBarBackground, false);
                toRemove.push_back(m);
            } else {
                gem.setVisible(pac.cooldownBar, true);
                gem.setVisible(pac.cooldownBarBackground, true);
            }
            gem.setWidth(pac.cooldownBar, java::d2i(((double)(totalCooldown - currentCooldown) / totalCooldown) * width));
        }
        cooldownLaunchers.erase(std::remove_if(cooldownLaunchers.begin(), cooldownLaunchers.end(),
                                               [&](Pacman* m) { return std::find(toRemove.begin(), toRemove.end(), m) != toRemove.end(); }),
                                cooldownLaunchers.end());
    }

    void setPacMessage(Pacman* m) {
        PacmanView& pac = pacmen[m->id];
        if (m->hasMessage) {
            gem.setText(pac.message, m->message);
            gem.setVisible(pac.message, true);
        } else {
            gem.setVisible(pac.message, false);
        }
        gem.commitEntityState(0, {pac.message});
    }

    void updateScores() {
        for (Player& p : players) gem.setText(scoreLabels[p.index], java::str(p.pellets));
    }

    void scaleStackedPacmen() {
        std::vector<std::pair<Coord, std::vector<Pacman*>>> byPosition;
        for (Player& p : players)
            for (Pacman* m : p.pacmen) {
                if (m->dead) continue;
                bool found = false;
                for (auto& b : byPosition)
                    if (b.first == m->position) {
                        b.second.push_back(m);
                        found = true;
                    }
                if (!found) byPosition.push_back({m->position, {m}});
            }
        for (PacmanView& pac : pacmen) {
            gem.setScaleX(pac.group, 1);
            gem.setScaleY(pac.group, 1);
            gem.setZIndex(pac.group, 10);
        }
        for (auto& b : byPosition) {
            int idx = 0;
            for (Pacman* m : b.second) {
                PacmanView& pac = pacmen[m->id];
                double shrink = 1 - (idx * 0.1);
                gem.setZIndex(pac.group, 10 + idx);
                gem.setScaleX(pac.group, shrink);
                gem.setScaleY(pac.group, shrink);
                idx++;
            }
        }
    }

    void endOfTurn(int frameDuration) {
        for (Move& mv : movers) animateMove(mv.pac, mv.from, mv.to);
        for (Bump& b : bumpers) animateBump(b.pac, b.from, b.to, b.distanceToBlocker);
        for (auto& s : switchers) {
            PacmanView& pac = pacmen[s.first->id];
            animateSwitch(pac, s.second, s.first, !isInPain(s.first), frameDuration);
            updateTooltipText(pac);
        }
        performCooldownAnimation();
        std::vector<Flash> f = flashers;
        std::stable_sort(f.begin(), f.end(), [](const Flash& a, const Flash& b) { return a.time < b.time; });
        for (Flash& fl : f) launchFlashFx(fl.pac, fl.time);
        for (Pacman* g : goners) {
            bool switcher = false, mover = false;
            for (auto& s : switchers) switcher = switcher || s.first == g;
            for (Move& mv : movers) mover = mover || mv.pac == g;
            if (switcher || mover) continue;
            // animatePain
            PacmanView& pac = pacmen[g->id];
            setViewState(pac, g->owner->index, g->type, false);
            gem.commitEntityState(0, {pac.sprite});
            animateDeath(pac);
        }
        for (PacmanView& pac : pacmen)
            if (pac.model->isSpeeding() || pac.model->endOfSpeed) updateTooltipText(pac);
        scaleStackedPacmen();
    }

    void startOfTurn(const std::vector<Player*>& speeders) {
        for (gem::Entity* i : speedIndicators) gem.setVisible(i, false);
        for (Player* p : speeders) gem.setVisible(speedIndicators[p->index], true);
        gem.commitEntityState(0, {speedIndicators[0], speedIndicators[1]});
        switchers.clear();
        goners.clear();
        movers.clear();
        bumpers.clear();
        flashers.clear();
        for (PacmanView* pac : dying) {
            gem.setZIndex(pac->group, 9);
            gem.commitEntityState(0, {pac->group});
            gem.setVisible(pac->message, false);
            gem.setVisible(pac->cooldownBar, false);
            gem.setVisible(pac->cooldownBarBackground, false);
            gem.setVisible(pac->death, false);
            gem.setVisible(pac->deathFX, true);
            gem.setPlaying(pac->deathFX, true);
            gem.commitEntityState(0.5, {pac->deathFX, pac->death, pac->message, pac->cooldownBar, pac->cooldownBarBackground});
            gem.setVisible(pac->deathFX, false);
        }
        dying.clear();
        for (PacmanView& pac : pacmen) {
            gem.setPlaying(pac.sprite, false);
            gem.commitEntityState(0, {pac.sprite});
        }
    }

    void chargeSpeed(Pacman* m) {
        PacmanView& pac = pacmen[m->id];
        ViewerEvent& ev = createAnimationEvent("play", 0);
        ev.put("x", convertXFromGridToAbsoluteCenter(m->position.x));
        ev.put("y", convertYFromGridToAbsoluteCenter(m->position.y));
        ev.putStr("name", "charge");
        sendShake(pac, 0, 1);
        gem.setAlpha(pac.plasmaBallContainer, 1);
        gem.setAlpha(pac.clone->plasmaBallContainer, 1);
    }

    void endSpeed(Pacman* m) {
        PacmanView& pac = pacmen[m->id];
        gem.setAlpha(pac.plasmaBallContainer, 0);
        gem.setAlpha(pac.clone->plasmaBallContainer, 0);
    }

    // the remaining pellets fly to the pacs of the last player standing; the jar picks each pac with
    // an unseeded java.util.Random (not reproducible): a Random seeded with the game's seed here
    void getAllPellets(Player* player, java::Random& viewRandom) {
        std::vector<Pacman*> alive;
        for (Pacman* m : player->pacmen)
            if (!m->dead) alive.push_back(m);
        if (alive.empty()) return;
        double maxDistance = Coord(0, 0).euclideanTo(gridModel->width, gridModel->height);
        // HashMap<Coord, Entity> order
        std::vector<i32> hashes;
        std::vector<std::pair<Coord, gem::Entity*>> entries(pellets.begin(), pellets.end());
        for (auto& e : entries) hashes.push_back(e.first.hashCode());
        for (int i : java::hashMapOrder(hashes)) {
            const Coord& pos = entries[i].first;
            gem::Entity* pellet = entries[i].second;
            if (!pellet->visible) continue;
            Coord destination = alive[viewRandom.nextInt((int)alive.size())]->position;
            double dist = pos.euclideanTo(destination.x, destination.y);
            setToGridCenterCoordinates(pellet, destination);
            double t = java::max(0.0, java::min(1.0, dist / maxDistance));
            gem.commitEntityState(t, {pellet});
        }
    }

    // --- module data (Module#onGameInit / onAfterGameTurn)
    void sendFrameData(bool init) {
        gem.sendFrameData();
        tooltips.sendFrameData();
        if (!animEvents.empty()) {
            std::string s = "[";
            for (size_t i = 0; i < animEvents.size(); i++) s += (i ? "," : "") + java::jsonQuote(java::ascii(animEvents[i].toString()));
            cg::gm.setViewData("event", cg::Json::raw(s + "]"));
            animEvents.clear();
        }
        toggles.sendFrameData();
        if (init) {
            gem.sendGlobalData();
            cg::gm.setViewGlobalData("tooltips", cg::Json::raw("{\"width\":" + std::to_string(tooltipGlobal[0]) + ",\"height\":" + std::to_string(tooltipGlobal[1]) +
                                                               ",\"x\":" + std::to_string(tooltipGlobal[2]) + ",\"y\":" + std::to_string(tooltipGlobal[3]) +
                                                               ",\"cellSize\":" + std::to_string(tooltipGlobal[4]) +
                                                               ",\"coefficient\":" + java::doubleToString(tooltipCoefficient) + "}"));
            std::string ids;
            for (size_t i = 0; i < plasmaBallIds.size(); i++) ids += (i ? "," : "") + std::to_string(plasmaBallIds[i]);
            cg::gm.setViewGlobalData("event", cg::Json::raw("[" + java::doubleToString(eventScale) + "," + std::to_string(speedboostGroupId) + ",[" + ids + "]]"));
        }
    }
};

std::string View::getTooltipText(const Pacman* m) const {
    const char* color = m->owner->getColor();
    if (m->type == NEUTRAL) return fmt("id: %d (%s)", m->number, color);
    if (m->abilityDuration > 0) return fmt("id: %d (%s)\ntype: %s\nspeed turns left: %d", m->number, color, TYPE_NAME[m->type], m->abilityDuration - 1);
    return fmt("id: %d (%s)\ntype: %s", m->number, color, TYPE_NAME[m->type]);
}

void View::createTunnelMask(const Coord& coord, int cellSize, int padding, double cornerSize, gem::Entity*& mask) {
    mask = gem.createSprite();
    gem.setAnchorX(mask, 0.5);
    gem.setBaseWidth(mask, cellSize - padding);
    gem.setBaseHeight(mask, java::d2i(cellSize + 2 * cornerSize - padding));
    gem.setImage(mask, "tunnel_masque");
    if (coord.x == gridWidth - 1) gem.setScaleX(mask, -1);
    else if (coord.x == 0) gem.setScaleX(mask, 1);
}

void View::initGrid(Grid& grid) {
    // GridView.init
    gem::Entity* group = gem.createBufferedGroup();
    gem::Entity* groupTunnel = gem.createGroup();
    gridWidth = grid.width;
    for (size_t i = 0; i < grid.cells.size(); i++) {
        Coord coord = grid.coordOf(i);
        Cell& cell = grid.cells[i];
        int padding = 0;
        if (cell.type == WALL) {
            static const int NB[4][3] = {{0, -1, 2}, {-1, 0, 8}, {1, 0, 16}, {0, 1, 64}};
            static const int CORNER[4][4] = {{-1, -1, 1, 10}, {1, -1, 4, 18}, {-1, 1, 32, 72}, {1, 1, 128, 80}};
            int total = 0;
            for (auto& n : NB) {
                Cell& c = grid.get(coord.x + n[0], coord.y + n[1]);
                if (c.isWall() || !c.valid) total += n[2];
            }
            for (auto& n : CORNER) {
                if ((total & n[3]) == n[3]) {
                    Cell& c = grid.get(coord.x + n[0], coord.y + n[1]);
                    if (c.isWall() || !c.valid) total += n[2];
                }
            }
            if (total != 255) {
                gem::Entity* wall = gem.createSprite();
                gem.setBaseWidth(wall, CELL_SIZE - padding);
                gem.setBaseHeight(wall, CELL_SIZE - padding);
                gem.setImage(wall, java::str(total));
                setToGridCoordinates(wall, coord);
                gem.add(group, {wall});
            }
        } else if (cell.type == FLOOR) {
            gem::Entity* floor = gem.createSprite();
            gem.setBaseWidth(floor, CELL_SIZE - padding);
            gem.setBaseHeight(floor, CELL_SIZE - padding);
            gem.setImage(floor, "floor");
            if (coord.x == 0 || coord.x == gridWidth - 1) {
                double cornerSize = CELL_SIZE * 100.0 / 250.0;
                gem::Entity* tunnel;
                createTunnelMask(coord, CELL_SIZE, padding, cornerSize, tunnel);
                gem.add(groupTunnel, {tunnel});
                gem.setX(tunnel, coord.x * CELL_SIZE + CELL_SIZE / 2);
                gem.setY(tunnel, java::d2i(coord.y * CELL_SIZE - cornerSize));
            }
            setToGridCoordinates(floor, coord);
            gem.add(group, {floor});
        }
    }
    // View.initGrid
    gem.setZIndex(group, 0);
    gem.setZIndex(groupTunnel, 11);
    for (size_t i = 0; i < grid.cells.size(); i++) {
        Coord coord = grid.coordOf(i);
        Cell& cell = grid.cells[i];
        if (cell.hasPellet) {
            gem::Entity* pellet = gem.createSprite();
            gem.setBaseWidth(pellet, java::d2i(CELL_SIZE * 0.3));
            gem.setBaseHeight(pellet, java::d2i(CELL_SIZE * 0.3));
            gem.setAnchor(pellet, 0.5);
            gem.setZIndex(pellet, 5);
            gem.setImage(pellet, "Bonusx1");
            setToGridCenterCoordinates(pellet, coord);
            pellets[coord] = pellet;
            gem.add(gameZone, {pellet});
        }
        if (cell.hasCherry) {
            gem::Entity* cherry = gem.createSprite();
            gem.setBaseWidth(cherry, java::d2i(CELL_SIZE * 0.6));
            gem.setBaseHeight(cherry, java::d2i(CELL_SIZE * 0.6));
            gem.setAnchor(cherry, 0.5);
            gem.setImage(cherry, "Bonusx5");
            gem.setZIndex(cherry, 5);
            setToGridCenterCoordinates(cherry, coord);
            pellets[coord] = cherry;
            gem.add(gameZone, {cherry});
        }
    }
    gem.add(gameZone, {group, groupTunnel});
}

void View::initHud() {
    const int screenWidth = 1920, hudWidth = 530;
    gem::Entity* hudGroup = gem.createGroup();
    gem.setZIndex(hudGroup, 30);
    gem::Entity* hudRed = gem.createSprite();
    gem.setImage(hudRed, "HUD_Masque_RED");
    gem.setZIndex(hudRed, 1);
    gem::Entity* hudBlue = gem.createSprite();
    gem.setImage(hudBlue, "HUD_Masque_BLUE");
    gem.setX(hudBlue, screenWidth - hudWidth);
    gem.setZIndex(hudBlue, 1);
    for (Player& p : players) {
        int x = screenWidth / 2;
        int color;
        if (p.index == 0) {
            x -= 180;
            color = 0xb61e23;
        } else {
            x += 180;
            color = p.getColorToken();
        }
        gem::Entity* s = gem.createSprite();
        gem.setImage(s, "fast.png");
        gem.setY(s, 16);
        gem.setX(s, x);
        gem.setAnchorX(s, 0.5);
        gem.setVisible(s, false);
        gem.setZIndex(s, 3);
        gem.setScale(s, 1.2);
        gem.setTint(s, color);
        speedIndicators.push_back(s);
    }
    gem.add(hudGroup, {hudRed, hudBlue});
    int playerHudZoneWidth = screenWidth / 2;
    double avatarRotation = 0.08;
    for (Player& p : players) {
        int coefMirror = p.index == 0 ? -1 : 1;
        gem::Entity* nameLabel = gem.createBitmapText();
        gem.setFontFamily(nameLabel, "BRLNS_66");
        gem.setFontSize(nameLabel, 36);
        gem.setText(nameLabel, p.getNicknameToken());
        gem.setMaxWidth(nameLabel, 300);
        gem.setAnchorX(nameLabel, 0.5);
        gem.setX(nameLabel, playerHudZoneWidth + coefMirror * 630);
        gem.setY(nameLabel, 7);
        gem.setZIndex(nameLabel, 2);

        gem::Entity* scoreLabel = gem.createBitmapText();
        gem.setFontFamily(scoreLabel, "BRLNS_66");
        gem.setFontSize(scoreLabel, 72);
        gem.setText(scoreLabel, u"0");
        gem.setAnchorX(scoreLabel, 0.5);
        gem.setX(scoreLabel, playerHudZoneWidth + coefMirror * 320);
        gem.setY(scoreLabel, 10);
        gem.setZIndex(scoreLabel, 2);

        gem::Entity* avatar = gem.createSprite();
        gem.setImage(avatar, p.getNicknameToken()); // getAvatarToken() is "$" + index too
        gem.setAnchor(avatar, 0.5);
        gem.setX(avatar, playerHudZoneWidth + coefMirror * 882);
        gem.setY(avatar, 70);
        gem.setRotation(avatar, coefMirror * avatarRotation);
        gem.setBaseHeight(avatar, 130);
        gem.setBaseWidth(avatar, 130);
        gem.setZIndex(avatar, 0);

        gem.add(hudGroup, {nameLabel, scoreLabel, avatar});
        scoreLabels.push_back(scoreLabel);
    }
}

void View::init(std::vector<Pacman*>& pacmenModel, Grid& grid, int frameDuration) {
    gridModel = &grid;
    background = gem.createSprite();
    gem.setImage(background, "BACKGROUND_semi.jpg");
    gameZone = gem.createGroup();
    const int screenWidth = 1920, screenHeight = 1080, HUD_HEIGHT = 150;
    int gameZoneWidth = grid.width * CELL_SIZE;
    int gameZoneHeight = grid.height * CELL_SIZE;
    double coefficient = std::min((double)screenWidth / gameZoneWidth, (double)(screenHeight - HUD_HEIGHT) / gameZoneHeight);
    gem.setScale(gameZone, coefficient);
    // center(gameZone, ...)
    gem.setX(gameZone, java::d2i(screenWidth / 2 - (double)(gameZoneWidth * coefficient) / 2));
    gem.setY(gameZone, java::d2i(screenHeight / 2 - (double)(gameZoneHeight * coefficient) / 2));
    gem.setY(gameZone, gameZone->y + HUD_HEIGHT / 2);
    tooltipGlobal[0] = grid.width;
    tooltipGlobal[1] = grid.height;
    tooltipGlobal[2] = gameZone->x;
    tooltipGlobal[3] = gameZone->y;
    tooltipGlobal[4] = CELL_SIZE;
    tooltipCoefficient = coefficient;

    pacMask = gem.createRectangle();
    gem.setWidth(pacMask, gameZoneWidth);
    gem.setHeight(pacMask, gameZoneHeight);
    gem.setZIndex(pacMask, 20);
    gem.add(gameZone, {pacMask});

    gem::Entity* speedboostGroup = gem.createGroup();
    gem.setZIndex(speedboostGroup, 9);
    gem.setMask(speedboostGroup, pacMask);
    gem.add(gameZone, {speedboostGroup});

    eventScale = gameZone->scaleX;
    speedboostGroupId = speedboostGroup->id;

    // initPacmen (pacmen and clones never grow afterwards: pointers to them stay valid)
    pacmen.reserve(pacmenModel.size());
    clones.reserve(pacmenModel.size());
    for (Pacman* m : pacmenModel) {
        PacmanView pac = createPacmanGroup(m, true, false, frameDuration);
        // createClone
        PacmanView clone = createPacmanGroup(m, true, true, frameDuration);
        gem.add(gameZone, {clone.group});
        gem.commitEntityState(0, {gameZone});
        gem.setDuration(clone.sprite, pac.sprite->duration);
        gem.setVisible(clone.group, false);
        if (m->position.x * 2 < grid.width) gem.setScaleX(pac.rotationWrapper, -1, gem::NONE);
        setToGridCenterCoordinates(pac.group, m->position);
        gem.add(gameZone, {pac.group});
        pacmen.push_back(pac);
        clones.push_back(clone);
        toggles.displayOnToggleState(pac.message, u"messageToggle", true);
    }
    for (size_t i = 0; i < pacmen.size(); i++) pacmen[i].clone = &clones[i];
    for (PacmanView& pac : pacmen) updateTooltipText(pac);
    initGrid(grid);
    initHud();
    endOfTurn(frameDuration);
}

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
struct Game {
    std::vector<Pacman> allPacmen; // storage, by id
    std::vector<Pacman*> pacmen;   // the alive ones (Game.pacmen)
    java::Random random;
    java::Random viewRandom; // stands for View.random (unseeded in the jar)
    Grid grid;
    MapGenerator mapGenerator;
    View view;
    int totalPacmen = 0, pacmenPerPlayer = 0, currentStep = 0;
    static const int FRAME_DURATION = 500;

    int randomInt(int min, int max) { return min + random.nextInt(max - min + 1); }

    void generateGrid() {
        int width = randomInt(Config::MAP_MIN_WIDTH, Config::MAP_MAX_WIDTH);
        int height = randomInt(Config::MAP_MIN_HEIGHT, Config::MAP_MAX_HEIGHT);
        mapGenerator.init();
        if (width % 2 == 0) width++;
        Grid g(width, height);
        mapGenerator.generateWithHorizontalSymetry(g, random);
        // MAP_WRAPS: a border, with tunnel exits
        Grid big(width + 2, height + 2);
        auto isTunnelExit = [&](int x, int y) { return g.get(x, y).isFloor() && !g.get(x, y - 1).isFloor() && !g.get(x, y + 1).isFloor(); };
        for (int y = 0; y < height + 2; ++y) {
            for (int x = 0; x < width + 2; ++x) {
                Coord gridPos(x - 1, y - 1);
                if (x == 0 || y == 0 || x == width + 1 || y == height + 1) {
                    if (x == 0 && isTunnelExit(gridPos.x + 1, gridPos.y)) big.get(x, y).type = FLOOR;
                    else if (x == width + 1 && isTunnelExit(gridPos.x - 1, gridPos.y)) big.get(x, y).type = FLOOR;
                    else big.get(x, y).type = WALL;
                } else {
                    big.get(x, y).copy(g.get(x - 1, y - 1));
                }
            }
        }
        grid = big;
    }

    void init(i64 seed) {
        random = java::Random(seed);
        viewRandom = java::Random(seed);
        generateGrid();
        pacmenPerPlayer = randomInt(Config::MIN_PACS_PER_PLAYER, Config::MAX_PACS_PER_PLAYER);
        totalPacmen = pacmenPerPlayer * 2;
        // generatePacmen
        allPacmen.resize(totalPacmen);
        int pacmanIndex = 0, typeIndex = 0;
        while (pacmanIndex < totalPacmen) {
            for (Player& player : players) {
                if (pacmanIndex < totalPacmen) {
                    Pacman& pac = allPacmen[pacmanIndex];
                    pac.id = pacmanIndex;
                    pac.number = (int)player.pacmen.size();
                    pac.owner = &player;
                    pac.type = typeIndex % 3;
                    player.pacmen.push_back(&pac);
                    pacmen.push_back(&pac);
                    pacmanIndex++;
                }
            }
            typeIndex++;
        }
        std::vector<Coord> freeCells;
        for (size_t i = 0; i < grid.cells.size(); i++) {
            Coord c = grid.coordOf(i);
            if (grid.cells[i].type == FLOOR && c.x != grid.width / 2) freeCells.push_back(c);
        }
        java::shuffle(freeCells, random);
        std::vector<Coord> leftCells;
        for (const Coord& c : freeCells)
            if (c.x <= grid.width / 2) leftCells.push_back(c);
        int i;
        for (i = 0; i < pacmenPerPlayer; ++i) {
            if (i >= (int)leftCells.size()) throw java::Exception{java::indexOutOfBoundsException(i, leftCells.size())};
            Coord leftCell = leftCells[i];
            Coord rightCell(grid.width - 1 - leftCell.x, leftCell.y);
            int leftPlayer = random.nextInt(2);
            int rightPlayer = (leftPlayer + 1) % 2;
            players[leftPlayer].pacmen[i]->position = leftCell;
            players[rightPlayer].pacmen[i]->position = rightCell;
        }
        for (int j = 0; j < Config::NUMBER_OF_CHERRIES / 2; j++) {
            if (i + j >= (int)leftCells.size()) throw java::Exception{java::indexOutOfBoundsException(i + j, leftCells.size())};
            Coord leftCell = leftCells[i + j];
            Coord rightCell(grid.width - 1 - leftCell.x, leftCell.y);
            grid.get(leftCell).hasCherry = true;
            grid.get(rightCell).hasCherry = true;
        }
        for (size_t k = 0; k < grid.cells.size(); k++) {
            Cell& cell = grid.cells[k];
            Coord coord = grid.coordOf(k);
            bool occupied = false;
            for (Pacman* p : pacmen) occupied = occupied || p->position == coord;
            if (cell.isFloor() && !cell.hasCherry && !occupied) cell.hasPellet = true;
        }
        view.init(pacmen, grid, FRAME_DURATION);
    }

    static bool canEat(const Pacman* a, const Pacman* b) {
        if (a->owner == b->owner) return false;
        if (a->type == PAPER) return b->type == ROCK;
        if (a->type == ROCK) return b->type == SCISSORS;
        if (a->type == SCISSORS) return b->type == PAPER;
        return false;
    }

    std::vector<std::string> getGlobalInfoFor() {
        std::vector<std::string> lines;
        lines.push_back(std::to_string(grid.width) + " " + std::to_string(grid.height));
        for (int y = 0; y < grid.height; ++y) {
            std::string row;
            for (int x = 0; x < grid.width; ++x) row += grid.get(x, y).isWall() ? '#' : ' ';
            lines.push_back(row);
        }
        return lines;
    }

    // findVisibleItems: along the 4 lines of sight of the player's alive pacs (wrapping)
    template <class F>
    std::vector<Coord> findVisibleItems(Player* player, F hasItem) {
        std::vector<Coord> visible;
        for (Pacman* pac : player->pacmen) {
            if (pac->dead) continue;
            for (const Coord& unitMove : ADJACENCY) {
                Coord current = pac->position;
                while (grid.get(current).isFloor()) {
                    if (hasItem(current) && std::find(visible.begin(), visible.end(), current) == visible.end()) visible.push_back(current);
                    Coord next;
                    if (grid.getCoordNeighbour(current, unitMove, next)) current = next;
                    else break;
                    if (pac->position == current) break;
                }
            }
        }
        return visible;
    }

    std::vector<std::string> getCurrentFrameInfoFor(Player* player) {
        std::vector<Player*> active = activePlayers();
        size_t oi = (size_t)(player->index + 1) % 2;
        if (oi >= active.size()) throw java::Exception{java::indexOutOfBoundsException((i64)oi, (i64)active.size())};
        Player* opponent = active[oi];
        std::vector<std::string> lines;
        lines.push_back(std::to_string(player->pellets) + " " + std::to_string(opponent->pellets));
        std::vector<Coord> coords = findVisibleItems(player, [&](const Coord& c) {
            for (Pacman* p : pacmen)
                if (p->position == c) return true;
            return false;
        });
        std::vector<Pacman*> visiblePacmen;
        for (Pacman* p : pacmen)
            if (std::find(coords.begin(), coords.end(), p->position) != coords.end()) visiblePacmen.push_back(p);
        for (Pacman* p : player->pacmen)
            if (p->dead) visiblePacmen.push_back(p);
        for (Pacman* p : opponent->pacmen)
            if (p->dead) visiblePacmen.push_back(p);
        lines.push_back(std::to_string(visiblePacmen.size()));
        std::stable_sort(visiblePacmen.begin(), visiblePacmen.end(), [](const Pacman* a, const Pacman* b) { return a->id < b->id; });
        for (Pacman* pac : visiblePacmen)
            lines.push_back(fmt("%d %d %d %d %s %d %d", pac->number, pac->owner == player ? 1 : 0, pac->position.x, pac->position.y, pac->dead ? "DEAD" : TYPE_NAME[pac->type],
                                pac->abilityDuration, pac->abilityCooldown));
        std::vector<Coord> visiblePellets = findVisibleItems(player, [&](const Coord& c) { return grid.get(c).hasPellet; });
        std::vector<Coord> cherries;
        for (size_t i = 0; i < grid.cells.size(); i++)
            if (grid.cells[i].hasCherry) cherries.push_back(grid.coordOf(i));
        lines.push_back(std::to_string(visiblePellets.size() + cherries.size()));
        for (const Coord& c : visiblePellets) lines.push_back(fmt("%d %d %d", c.x, c.y, 1));
        for (const Coord& c : cherries) lines.push_back(fmt("%d %d %d", c.x, c.y, Config::CHERRY_SCORE));
        return lines;
    }

    void resetGameTurnData() {
        currentStep = 0;
        for (Player& p : players)
            for (Pacman* pac : p.pacmen) pac->turnReset();
    }

    void performGameUpdate() {
        view.startOfTurn({});
        executePacmenAbilities();
        updateAbilityModifiers();
        processPacmenIntent();
        resolveMovement();
        view.endOfTurn(FRAME_DURATION);
        printPacmenGameSummary();
    }

    void performGameSpeedUpdate() {
        std::vector<Player*> speeders;
        for (Pacman* p : pacmen)
            if (p->intendedPath.size() > 2 && std::find(speeders.begin(), speeders.end(), p->owner) == speeders.end()) speeders.push_back(p->owner);
        view.startOfTurn(speeders);
        cg::gm.addToGameSummary("Only pacs with the SPEED ability enabled can move:");
        for (Pacman* pac : pacmen) {
            if (pac->speed <= 1 || pac->actionType() != AT_MOVE) pac->intendedPath = {pac->position};
            pac->blocked = false;
        }
        resolveMovement();
        view.endOfTurn(FRAME_DURATION);
        printPacmenGameSummary();
    }

    static Coord getIntendedPositionAtStep(const Pacman* pac, int step) { return pac->intendedPath.at(std::min(step, (int)pac->intendedPath.size() - 1)); }

    bool pacmenHaveCollided(const Pacman* a, const Pacman* b) const {
        Coord fromA = getIntendedPositionAtStep(a, a->previousPathStep);
        Coord fromB = getIntendedPositionAtStep(b, b->previousPathStep);
        Coord toA = getIntendedPositionAtStep(a, a->currentPathStep);
        Coord toB = getIntendedPositionAtStep(b, b->currentPathStep);
        return toA == toB || (toA == fromB && toB == fromA);
    }

    void resolveMovement() {
        std::vector<Pacman*> blockedPacmen, blockedBy;
        resolvePacmenMovement(blockedPacmen, blockedBy);

        for (Pacman* pac : pacmen) {
            if (pac->hasWarning) {
                pac->addToGameSummary(fmt("Pac %d: %s", pac->number, pac->warning.c_str()));
                pac->hasWarning = false;
            }
        }

        std::vector<Pacman*> pacmenToKill; // a HashSet: the order of the kills changes nothing observable but the tooltips' order
        for (Pacman* pac : pacmen) {
            for (Pacman* other : pacmen) {
                if (other == pac) continue;
                if (canEat(pac, other) && pacmenHaveCollided(pac, other)) {
                    if (std::find(pacmenToKill.begin(), pacmenToKill.end(), other) == pacmenToKill.end()) {
                        pacmenToKill.push_back(other);
                        view.killPacman(other);
                    }
                }
            }
        }

        // view: bumps and flashes. Set<BumpCouple> (hashCode fromA + fromB, symmetric equals)
        struct BumpCouple {
            Coord from, fromBlocker, to;
            int distance;
        };
        std::vector<BumpCouple> bumps;
        std::vector<Pacman*> flashing; // pacmenToFlash.get(0.5): a HashSet<Pacman>
        for (size_t k = 0; k < blockedPacmen.size(); k++) {
            Pacman* pac = blockedPacmen[k];
            Pacman* blocker = blockedBy[k];
            Coord from = getIntendedPositionAtStep(pac, pac->currentPathStep);
            Coord blockerFrom = getIntendedPositionAtStep(blocker, pac->currentPathStep);
            Coord to = getIntendedPositionAtStep(pac, pac->currentPathStep + 1);
            int distanceToBlocker = grid.calculateDistance(pac->position, blocker->position);
            bool killed = std::find(pacmenToKill.begin(), pacmenToKill.end(), pac) != pacmenToKill.end();
            bool blockerBlocked = std::find(blockedPacmen.begin(), blockedPacmen.end(), blocker) != blockedPacmen.end();
            if (killed && !blockerBlocked) continue;
            view.bumpPacman(pac, from, to, distanceToBlocker);
            bool dup = false;
            for (const BumpCouple& b : bumps)
                if ((b.from == from && b.fromBlocker == blockerFrom) || (b.from == blockerFrom && b.fromBlocker == from)) dup = true;
            if (!dup) bumps.push_back(BumpCouple{from, blockerFrom, to, distanceToBlocker});
            for (Pacman* f : {pac, blocker})
                if (std::find(flashing.begin(), flashing.end(), f) == flashing.end()) flashing.push_back(f);
        }
        for (Pacman* f : flashing) view.flashPacman(f, 0.5);
        std::vector<i32> hashes;
        for (const BumpCouple& b : bumps) hashes.push_back(b.from.hashCode() + b.fromBlocker.hashCode());
        for (int k : java::hashMapOrder(hashes)) view.launchBumpFx(bumps[k].from, bumps[k].fromBlocker, bumps[k].to, bumps[k].distance);

        for (Pacman* pac : blockedPacmen) {
            Coord target = getIntendedPositionAtStep(pac, pac->currentPathStep + 1);
            pac->addToGameSummary(fmt("Pac %d is blocked from entering (%d, %d).", pac->number, target.x, target.y));
        }

        killPacmen(pacmenToKill);
        eatItem(true);
        eatItem(false);
        currentStep++;
    }

    bool pacmenWillShareSameCoord(const Pacman* a, const Pacman* b) const {
        Coord toA = getIntendedPositionAtStep(a, a->blocked ? a->currentPathStep : a->currentPathStep + 1);
        Coord toB = getIntendedPositionAtStep(b, b->blocked ? b->currentPathStep : b->currentPathStep + 1);
        return toA == toB;
    }
    bool pacmenWillCollide(const Pacman* a, const Pacman* b) const {
        Coord fromA = getIntendedPositionAtStep(a, a->currentPathStep);
        Coord fromB = getIntendedPositionAtStep(b, b->currentPathStep);
        Coord toA = a->blocked ? fromA : getIntendedPositionAtStep(a, a->currentPathStep + 1);
        Coord toB = b->blocked ? fromB : getIntendedPositionAtStep(b, b->currentPathStep + 1);
        return toA == toB || (toA == fromB && toB == fromA);
    }
    bool isBodyBlockedBy(const Pacman* pac, const Pacman* other) const {
        // BODY_BLOCK and FRIENDLY_BODY_BLOCK
        if (canEat(pac, other)) return false;
        if (canEat(other, pac) && pacmenWillShareSameCoord(pac, other)) return false;
        return pacmenWillCollide(pac, other);
    }

    void resolvePacmenMovement(std::vector<Pacman*>& blockedPacmen, std::vector<Pacman*>& blockedBy) {
        std::vector<Pacman*> toResolve;
        for (Pacman* pac : pacmen)
            if (pac->actionType() == AT_MOVE && !pac->moveFinished() && pac->fastEnoughToMoveAt(currentStep)) toResolve.push_back(pac);
        std::vector<Pacman*> resolved;
        do {
            resolved.clear();
            for (Pacman* pac : toResolve) {
                Pacman* best = nullptr;
                int bestD = 0;
                for (Pacman* other : pacmen) { // Stream.min(byDistanceTo): the first of the closest
                    if (other == pac || !isBodyBlockedBy(pac, other)) continue;
                    int d = other->position.manhattanTo(pac->position);
                    if (!best || d < bestD) {
                        best = other;
                        bestD = d;
                    }
                }
                if (best) { // a blocked pac leaves toResolve: blocked once at most
                    resolved.push_back(pac);
                    blockedPacmen.push_back(pac);
                    blockedBy.push_back(best);
                }
            }
            for (Pacman* pac : resolved) pac->blocked = true;
            toResolve.erase(std::remove_if(toResolve.begin(), toResolve.end(), [&](Pacman* p) { return std::find(resolved.begin(), resolved.end(), p) != resolved.end(); }),
                            toResolve.end());
        } while (!resolved.empty());
        for (Pacman* pac : toResolve) movePacman(pac);
    }

    void processPacmenIntent() {
        for (Pacman* pac : pacmen) {
            if (pac->actionType() == AT_MOVE) pac->intendedPath = computeIntendedPath(pac);
            else pac->intendedPath = {pac->position};
            view.setPacMessage(pac);
        }
    }

    void executePacmenAbilities() {
        for (Pacman* pac : pacmen) {
            if (pac->abilityToUse == NO_ABILITY) continue;
            int ability = pac->abilityToUse;
            if (pac->abilityCooldown != 0) {
                if (ability == SPEED) pac->addToGameSummary(fmt("Pac %d can't use a speed boost yet!", pac->number));
                else pac->addToGameSummary(fmt("Pac %d can't switch to %s form yet!", pac->number, TYPE_NAME[ability - SET_ROCK]));
                continue;
            }
            if (ability != SPEED) {
                view.changePacmanType(pac, pac->type);
                pac->type = ability - SET_ROCK;
            } else {
                pac->speed = Config::SPEED_BOOST;
                view.chargeSpeed(pac);
                pac->abilityDuration = Config::ABILITY_DURATION;
            }
            pac->abilityCooldown = Config::ABILITY_COOLDOWN;
            view.addCooldownAnimation(pac);
            if (ability == SPEED) pac->addToGameSummary(fmt("Pac %d used a speed boost.", pac->number));
            else pac->addToGameSummary(fmt("Pac %d switch to %s form.", pac->number, TYPE_NAME[pac->type]));
        }
    }

    void updateAbilityModifiers() {
        for (Pacman* pac : pacmen) {
            if (pac->speed > 1 && pac->abilityDuration == 0) {
                pac->speed = Config::PACMAN_BASE_SPEED;
                view.endSpeed(pac);
            }
        }
    }

    std::vector<Coord> computeIntendedPath(Pacman* pac) {
        PathFinderResult pfr = findPath(grid, pac->position, pac->target);
        const std::vector<Coord>& wholePath = pfr.path;
        if (pfr.isNearest) {
            Coord newTarget = pfr.path.back();
            pac->hasWarning = true;
            if (pfr.path.size() > 1)
                pac->warning = fmt("Warning: target (%d, %d) is unreachable, going to (%d, %d) instead.", pac->target.x, pac->target.y, newTarget.x, newTarget.y);
            else
                pac->warning = fmt("Warning: target (%d, %d) is unreachable. Staying here!", pac->target.x, pac->target.y);
        } else {
            pac->hasWarning = false;
        }
        if (wholePath.size() > 1) {
            int stepsThisTurn = std::min(pac->speed, (int)wholePath.size() - 1);
            return std::vector<Coord>(wholePath.begin(), wholePath.begin() + stepsThisTurn + 1);
        }
        return {pac->position};
    }

    void movePacman(Pacman* pac) {
        pac->setCurrentPathStep(pac->currentPathStep + 1);
        Coord from = getIntendedPositionAtStep(pac, pac->previousPathStep);
        Coord to = getIntendedPositionAtStep(pac, pac->currentPathStep);
        pac->position = to;
        view.movePacman(pac, from, to);
        std::string summary = fmt("Pac %d moved to (%d, %d).", pac->number, pac->position.x, pac->position.y);
        if (pac->hasWarning) {
            summary += " " + pac->warning;
            pac->hasWarning = false;
        }
        pac->addToGameSummary(summary);
    }

    void killPacmen(const std::vector<Pacman*>& toKill) {
        for (Pacman* pac : toKill) {
            pacmen.erase(std::find(pacmen.begin(), pacmen.end(), pac));
            pac->dead = true;
            Player* owner = pac->owner;
            if (!owner->hasAlivePacmen()) owner->deactivate();
            cg::gm.addTooltip(owner->index, fmt("Pac %d has died.", pac->number));
            pac->addToGameSummary(fmt("Pac %d has died.", pac->number));
        }
    }

    // eatPellets (pellets) / eatCherries
    void eatItem(bool pelletsNotCherries) {
        int pelletValue = pelletsNotCherries ? 1 : Config::CHERRY_SCORE;
        std::vector<std::pair<Coord, std::vector<Pacman*>>> eatenBy; // a HashMap<Coord, List<Pacman>>
        for (Pacman* pac : pacmen) {
            Cell& cell = grid.get(pac->position);
            if (pelletsNotCherries ? cell.hasPellet : cell.hasCherry) {
                bool found = false;
                for (auto& e : eatenBy)
                    if (e.first == pac->position) {
                        e.second.push_back(pac);
                        found = true;
                    }
                if (!found) eatenBy.push_back({pac->position, {pac}});
            }
        }
        std::vector<i32> hashes;
        for (auto& e : eatenBy) hashes.push_back(e.first.hashCode());
        for (int k : java::hashMapOrder(hashes)) {
            const Coord& coord = eatenBy[k].first;
            std::vector<Player*> owners;
            for (Pacman* p : eatenBy[k].second)
                if (std::find(owners.begin(), owners.end(), p->owner) == owners.end()) owners.push_back(p->owner);
            for (Player* p : owners) p->pellets += pelletValue;
            grid.get(coord).hasPellet = false;
            grid.get(coord).hasCherry = false;
            view.eatPellet(coord, pelletValue);
        }
        view.updateScores();
    }

    void printPacmenGameSummary() {
        for (Player& player : players) {
            bool any = false;
            for (Pacman* pac : player.pacmen) any = any || !pac->gameSummary.empty();
            if (!any) continue;
            cg::gm.addToGameSummary(player.getNicknameToken() + u":");
            std::vector<Pacman*> sorted = player.pacmen;
            std::stable_sort(sorted.begin(), sorted.end(), [](const Pacman* a, const Pacman* b) { return a->id < b->id; });
            for (Pacman* pac : sorted) {
                for (const String& line : pac->gameSummary) cg::gm.addToGameSummary(u"- " + line);
                pac->gameSummary.clear();
            }
        }
    }

    int getRemainingPellets() {
        int sum = 0;
        for (Cell& c : grid.cells)
            if (c.hasPellet || c.hasCherry) sum += c.hasPellet ? 1 : Config::CHERRY_SCORE;
        return sum;
    }

    bool canImproveRanking(Player* player) {
        int remaining = getRemainingPellets();
        if (remaining <= 0) return false;
        for (Player& p : players)
            if (&p != player && p.pellets >= player->pellets && player->pellets + remaining >= p.pellets) return true;
        return false;
    }

    bool isGameOver() {
        std::vector<Player*> active = activePlayers();
        if (active.size() <= 1) return true;
        for (Player* p : active)
            if (canImproveRanking(p)) return false;
        return true;
    }

    void performGameOver() {
        std::vector<Player*> active = activePlayers();
        if (active.size() <= 1) {
            if (active.size() == 1) {
                cg::gm.addToGameSummary(u"Only " + active[0]->getNicknameToken() + u" still has pacs standing!");
                active[0]->pellets += getRemainingPellets();
                view.updateScores();
                view.getAllPellets(active[0], viewRandom);
            } else {
                cg::gm.addToGameSummary("No pacs remaining!");
            }
            return;
        }
        int remaining = getRemainingPellets();
        for (Player& player : players) {
            bool all = true;
            for (Player* p : activePlayers())
                if (p != &player && !(p->pellets + remaining < player.pellets)) all = false;
            if (all) {
                cg::gm.addToGameSummary(u"Game Over: " + player.getNicknameToken() + u" has so many pellets that they can't be defeated!");
                return;
            }
        }
    }

    bool isSpeedTurn() {
        if (pacmen.empty()) throw java::Exception{"java.util.NoSuchElementException: No value present"};
        int numSteps = 0;
        for (size_t i = 0; i < pacmen.size(); i++) numSteps = i ? std::max(numSteps, (int)pacmen[i]->intendedPath.size()) : (int)pacmen[i]->intendedPath.size();
        numSteps -= 1;
        return currentStep > 0 && currentStep < numSteps;
    }
};

static Game game;

// ----------------------------------------------------------------------------
// CommandManager.java: the regexes, matched by hand
// ----------------------------------------------------------------------------
struct GameException {
    String message;
};
struct InvalidInputException {
    String message;
};

namespace parse {
// a run of [ \t\n\x0B\f\r] at i: its end
static size_t spaces(const String& s, size_t i) {
    while (i < s.size() && java::isRegexSpace(s[i])) i++;
    return i;
}
static size_t digits(const String& s, size_t i) {
    while (i < s.size() && java::isRegexDigit(s[i])) i++;
    return i;
}
// the word (ASCII case-insensitive) at i
static bool word(const String& s, size_t i, const char* w) {
    size_t n = strlen(w);
    if (i + n > s.size()) return false;
    for (size_t k = 0; k < n; k++)
        if (!java::regexCharMatchesIgnoreCase(s[i + k], w[k])) return false;
    return true;
}
// "." matches anything but the line terminators
static bool dots(const String& s, size_t i) {
    for (; i < s.size(); i++)
        if (java::isLineSeparator(s[i])) return false;
    return true;
}
// "(?:\s+(?<message>.+))?$" from i
static bool tail(const String& s, size_t i, bool& hasMessage, String& message) {
    hasMessage = false;
    if (i == s.size()) return true;
    size_t j = spaces(s, i);
    if (j == i) return false;
    // \s+ is greedy, then .+ needs a char: give back whitespace when the line ends with it
    if (j == s.size()) j = s.size() - 1;
    if (!dots(s, j)) return false;
    hasMessage = true;
    message = s.substr(j);
    return true;
}
// "<WORD>\s+(?<id>\d+)" from 0: the end of the id
static bool head(const String& s, const char* w, size_t& idStart, size_t& idEnd) {
    if (!word(s, 0, w)) return false;
    size_t i = strlen(w);
    size_t j = spaces(s, i);
    if (j == i) return false;
    size_t k = digits(s, j);
    if (k == j) return false;
    idStart = j;
    idEnd = k;
    return true;
}
} // namespace parse

static void deactivatePlayer(Player* player, const String& message) {
    String m = java::replace(message, u"&lt;", u"<");
    m = java::replace(m, u"&gt;", u">");
    player->deactivate(m);
}

static String invalidInput(const String& got) { return u"Invalid Input: Expected " + java::ascii(EXPECTED) + u" but got '" + got + u"'"; }

// Integer.parseInt / Integer.valueOf of a matched "-?\d+": the NumberFormatException text when it overflows
static bool parseInt(const String& s, i32& out) { return java::parseInt(s, out); }

struct NumberFormat {
    String input;
};

static void handlePacmanCommand(Pacman* pac, Intent intent, int switchType, bool hasMessage, const String& message) {
    if (hasMessage && !java::trim(message).empty()) pac->setMessage(message);
    pac->intent = intent;
    if (intent == SPEED_INTENT) {
        pac->abilityToUse = SPEED;
    } else if (intent == SWITCH_INTENT) {
        pac->switchType = switchType;
        if (pac->type != switchType) pac->abilityToUse = SET_ROCK + switchType;
        else cg::gm.addToGameSummary(fmt("Pac %d is already in %s form!", pac->number, TYPE_NAME[switchType]));
    }
}

static void parseCommands(Player* player, const std::vector<String>& lines) {
    std::vector<String> commands = java::split(lines.at(0), u'|');
    for (const String& command : commands) {
        String cStr = java::trim(command);
        if (cStr.empty()) continue;
        try {
            Pacman* pac = nullptr;
            // PLAYER_ACTION_PATTERN: ^(WAIT|MOVE|SWITCH|SPEED|MSG)\s+(?<id>\d+).*
            size_t idStart = 0, idEnd = 0;
            bool action = false;
            for (const char* w : {"WAIT", "MOVE", "SWITCH", "SPEED", "MSG"})
                if (!action && parse::head(cStr, w, idStart, idEnd) && parse::dots(cStr, idEnd)) action = true;
            if (!action) throw InvalidInputException{invalidInput(cStr)};
            i32 pacNumber;
            if (!parseInt(cStr.substr(idStart, idEnd - idStart), pacNumber)) throw NumberFormat{cStr.substr(idStart, idEnd - idStart)};
            for (Pacman* p : player->pacmen)
                if (!pac && p->number == pacNumber) pac = p;
            if (!pac) throw GameException{java::ascii(fmt("Pac %d doesn't exist", pacNumber))};
            if (pac->dead) {
                pac->addToGameSummary(fmt("Pac %d is dead! It cannot be commanded anymore!", pacNumber));
                continue;
            }
            if (pac->intent != NO_ACTION) throw GameException{java::ascii(fmt("Pac %d cannot be commanded twice!", pacNumber))};

            bool hasMessage;
            String message;
            // PLAYER_MOVE_PATTERN: ^MOVE\s+(?<id>\d+)\s+(?<x>-?\d+)\s+(?<y>-?\d+)(?:\s+(?<message>.+))?$
            if (parse::head(cStr, "MOVE", idStart, idEnd)) {
                size_t i = parse::spaces(cStr, idEnd);
                size_t xs = i, xe = 0, ys = 0, ye = 0;
                bool ok = i > idEnd;
                if (ok) {
                    if (i < cStr.size() && cStr[i] == u'-') i++;
                    xe = parse::digits(cStr, i);
                    ok = xe > i;
                }
                if (ok) {
                    i = parse::spaces(cStr, xe);
                    ok = i > xe;
                    ys = i;
                }
                if (ok) {
                    if (i < cStr.size() && cStr[i] == u'-') i++;
                    ye = parse::digits(cStr, i);
                    ok = ye > i;
                }
                if (ok && parse::tail(cStr, ye, hasMessage, message)) {
                    i32 x, y;
                    if (!parseInt(cStr.substr(xs, xe - xs), x)) throw NumberFormat{cStr.substr(xs, xe - xs)};
                    if (!parseInt(cStr.substr(ys, ye - ys), y)) throw NumberFormat{cStr.substr(ys, ye - ys)};
                    if (!(0 <= x && x < game.grid.width && 0 <= y && y < game.grid.height))
                        throw GameException{java::ascii(fmt("Pac %d (%s) cannot reach its target (%d, %d) because it is out of grid!", pac->number, pac->owner->getColor(), x, y))};
                    pac->target = Coord(x, y);
                    handlePacmanCommand(pac, MOVE_INTENT, 0, hasMessage, message);
                    continue;
                }
            }
            // PLAYER_SPEED_TYPE_PATTERN: ^SPEED\s+(?<id>\d+)(?:\s+(?<message>.+))?$
            if (parse::head(cStr, "SPEED", idStart, idEnd) && parse::tail(cStr, idEnd, hasMessage, message)) {
                handlePacmanCommand(pac, SPEED_INTENT, 0, hasMessage, message);
                continue;
            }
            // PLAYER_SWITCH_TYPE_PATTERN: ^SWITCH\s+(?<id>\d+)\s+(?<switchType>(ROCK|PAPER|SCISSORS))(?:\s+(?<message>.+))?$
            if (parse::head(cStr, "SWITCH", idStart, idEnd)) {
                size_t i = parse::spaces(cStr, idEnd);
                bool matched = false;
                if (i > idEnd) {
                    for (int t = 0; t < 3 && !matched; t++) {
                        if (parse::word(cStr, i, TYPE_NAME[t]) && parse::tail(cStr, i + strlen(TYPE_NAME[t]), hasMessage, message)) {
                            handlePacmanCommand(pac, SWITCH_INTENT, t, hasMessage, message);
                            matched = true;
                        }
                    }
                }
                if (matched) continue;
            }
            // PLAYER_WAIT_PATTERN: ^WAIT\s+(?<id>\d+)(?:\s+(?<message>.+))?$
            if (parse::head(cStr, "WAIT", idStart, idEnd) && parse::tail(cStr, idEnd, hasMessage, message)) continue;
            throw InvalidInputException{invalidInput(cStr)};
        } catch (const InvalidInputException& e) {
            deactivatePlayer(player, e.message);
            cg::gm.addToGameSummary(u"Bad command: " + e.message);
            return;
        } catch (const GameException& e) {
            deactivatePlayer(player, e.message);
            cg::gm.addToGameSummary(u"Bad command: " + e.message);
            return;
        } catch (const NumberFormat& e) {
            // catch (Exception e): new InvalidInputException(e.toString(), EXPECTED, cStr)
            String text = java::numberFormatException(e.input);
            deactivatePlayer(player, text + u": Expected " + java::ascii(EXPECTED) + u" but got '" + cStr + u"'");
            cg::gm.addToGameSummary(u"Bad command: For input string: \"" + e.input + u"\"");
            return;
        }
    }
    for (Pacman* pac : player->pacmen)
        if (!pac->dead && pac->intent == NO_ACTION) pac->addToGameSummary(fmt("Pac %d received no command.", pac->number));
}

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
struct SpringReferee : cg::Referee {
    gem::EndScreenModule endScreen;
    bool gameOverFrame = false;
    int maxFrames = MAX_TURNS;

    void init() override {
        gameOverFrame = false;
        maxFrames = MAX_TURNS;
        try {
            cg::gm.setFrameDuration(500);
            cg::gm.setMaxTurns(MAX_TURNS);
            cg::gm.setTurnMaxTime(50);
            game.init(cg::gm.getSeed());
            for (Player* p : activePlayers())
                for (const std::string& line : game.getGlobalInfoFor()) p->sendInputLine(line);
        } catch (const java::Exception&) {
            // "Referee failed to initialize", abort()
            cg::gm.endGame();
        }
    }

    void onGameInit() override { game.view.sendFrameData(true); }
    void onAfterGameTurn() override { game.view.sendFrameData(false); }

    void gameTurn(int) override {
        if (!gameOverFrame) {
            if (game.isSpeedTurn()) {
                game.performGameSpeedUpdate();
                maxFrames++;
                cg::gm.setMaxTurns(maxFrames);
            } else {
                game.resetGameTurnData();
                for (Player* p : activePlayers()) {
                    for (const std::string& line : game.getCurrentFrameInfoFor(p)) p->sendInputLine(line);
                    p->execute();
                }
                // handlePlayerCommands
                for (Player* p : activePlayers()) {
                    if (p->timeout) {
                        p->deactivate(u"Timeout!");
                        p->timedOut = true;
                        cg::gm.addToGameSummary(p->getNicknameToken() + u" has not provided 1 lines in time");
                    } else {
                        parseCommands(p, p->outputs);
                    }
                }
                game.performGameUpdate();
            }
            if (game.isGameOver()) gameOverFrame = true;
        } else {
            game.resetGameTurnData();
            game.performGameUpdate();
            game.performGameOver();
            cg::gm.endGame();
        }
    }

    void onEnd() override {
        for (Player& p : players) p.setScore(p.pellets);
        // printWinner
        int maxScore = std::max(players[0].pellets, players[1].pellets);
        std::vector<Player*> best;
        for (Player& p : players)
            if (p.pellets == maxScore) best.push_back(&p);
        if (best.size() > 1)
            cg::gm.addToGameSummary(fmt(maxScore == 1 ? "Game tied! Each player has %d pellet" : "Game tied! Each player has %d pellets", maxScore));
        else
            cg::gm.addToGameSummary(best[0]->getNicknameToken() + java::ascii(fmt(best[0]->pellets == 1 ? " wins with %d pellet" : " wins with %d pellets", best[0]->pellets)));
        std::vector<int> scores;
        std::vector<String> texts;
        for (Player& p : players) {
            scores.push_back(p.getScore());
            texts.push_back(java::ascii(fmt(p.timedOut ? "%d pellets (timed out)" : "%d pellets", p.pellets)));
        }
        endScreen.setScores(scores, texts);
    }

    void onAfterOnEnd() override { endScreen.onAfterOnEnd(); }
};

int main(int argc, char** argv) {
    static SpringReferee referee;
    cg::Options options;
    options.maxPlayers = 2;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
