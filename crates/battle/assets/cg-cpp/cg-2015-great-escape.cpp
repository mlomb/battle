// The Great Escape (CodinGame, 2014, "great-escape") - headless C++ referee, port of the CodinGame referee in
// https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the only commit of the file),
// "Referees/The Great Escape/Referee.java" + referees/cg-builder/patches/cg-2015-great-escape.patch (see below): the single-file
// referee `class Referee extends MultiReferee` with CodinGame's old AbstractReferee base of 2014 embedded
// (common/legacy.h, legacy::Base::greatEscape2014(): the 2014 INFOS / SUMMARY, a real win state, two tooltips per
// timeout, getMillisTimeForRound 150, no precomputed inputs, the frame data and the tooltips sent in every frame;
// its SoloReferee class is unused). The file is the game CodinGame runs today: its classes reproduce CodinGame's own
// replays (views, infos, summaries, tooltips, scores) from CodinGame's init properties. 2 or 3 players (CodinGame
// runs both; the referee would also seat a 4th). No league rules in the referee. Deterministic: reproduces the same
// game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 ...] -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): width, height, positions,
// symmetric, in the order of a JDK 9+ Properties) to stdout like the jar (referees/cg-builder/legacy: the referee runs
// in the runner of the SDK 4.5.0). "null" scores when the game fails (the Java referee crashed or the protocol broke;
// the jar after 150 s). -l writes the trace of common/legacy.h (bot executions, view sizes, summaries, tooltips, END;
// CG_TRACE_VIEWS=1 adds the text of every view). Time limits: the base's 1000 ms for the first turn, 150 ms after (the
// game overrides neither).
//
// Start positions and the seed: the published referee draws the start positions with an unseeded java.util.Random
// unless the init property "positions" is given, so the jar would play other positions on every run. The jar is built
// with referees/cg-builder/patches/cg-2015-great-escape.patch, which seeds that Random with the init property "seed" (the
// CLI's -seed; without one it stays unseeded): the four draws nextInt(width), nextInt(width), nextInt(height),
// nextInt(height) (made even when "positions" is given), the redraws of a short list or of a value out of range,
// all unchanged; this port makes the same draws from the same seed. CodinGame's matches give no seed (their init
// properties, as the replays show them, are width, height, positions and symmetric), so no mapping from a seed to
// CodinGame's positions exists: the seed here only makes the jar and this port deterministic. CodinGame also plays
// symmetric=true in some matches (about 9% of its replays: player 1 starts on the point reflection of player 0's
// cell); the CLI passes only the seed, so jar and port play symmetric=false (the referee's default), but this port
// implements it (validation build below).
//
// The game: a 9 x 9 board; player 0 starts on the left column (row positions[0]) and must reach the right one, player
// 1 the right column (row 8 - positions[1]) -> the left one, player 2 the top row (column 8 - positions[2]) -> the
// bottom one. Each turn a player outputs "UP" / "RIGHT" / "DOWN" / "LEFT" (one step) or "x y H|V" (a wall of length 2:
// H on the top edges of cells (x, y) and (x + 1, y), V on the left edges of (x, y) and (x, y + 1)), optionally followed
// by white space and a message (cut to 20 chars, shown in the view). 20 walls shared out (10 each with 2 players, 6
// with 3). A wall may not overlap or cross another, leave the board or cut any player still in the game from its goal
// side; a player loses on such a wall, on a move into a wall or off the board, on an invalid line, with no wall left,
// or by timing out. Reaching the goal side wins (a WinException: the player leaves the game). The game ends when fewer
// than 2 players are left in it (a win or a loss of the second-to-last one) or after 100 rounds. Scores are ranks
// (0 = last): winners above the players still in the game (an earlier win higher), those above the losers (a later
// loss higher); equal players share a rank.
//
// The Java referee is followed closely, notably: the regex (Matcher#matches of
// ((?<direction>UP|RIGHT|DOWN|LEFT)|((?<x>[0-9]+)\s+(?<y>[0-9]+)\s*(?<orientation>H|V)))(\s+(?<message>.*))? with
// Java's \s and '.', see matchOutput), Integer.parseInt of a coordinate that overflows (an uncaught
// NumberFormatException: the referee thread dies, "null" scores, as in the jar), the order of the checks (no wall
// left, x, y, the orientation's bounds, then the wall count spent before the overlap / crossing test, then the paths
// of every player still in the game), the path search (CustomPathfinding: a breadth-first search over a LinkedList
// with neighbours in the order left, up, right, down, cells closed when taken out; its duplicates never change the
// path it returns, so this port runs a plain BFS that marks cells when queued: the same first path to the first goal
// cell, see findWay), each player's shortest path recomputed after its own move or for everyone after a wall, the
// ranks (Arrays.sort of the players with their compareTo: java::sort, TimSort), the frame of a win / loss / timeout,
// a timed out player keeping its last action, path and message in the view, the walls "just placed" reset only when
// a player outputs something (a timeout's frame shows the previous frame's wall again), the view / infos / summary
// texts in full (the invalid input text has a no-break space U+00A0 after "or", as in the Java source). The view stays
// far below the runner's 30000-byte limit (three paths of at most 81 cells, messages of 20 chars); the INFOS carry an
// invalid line in full (legacy.h checks the limit). An init error (init properties only: a bad width / height /
// positions / seed, a zero-size board) leaves the players null as in Java: the start view data counts the seats and
// the frame data fails (a NullPointerException the base catches).
// Collections: the path search's HashSet<Cell> (Cell has equals / hashCode by value) only answers contains(); the
// HashMap<Player, List<Cell>> of new paths (identity hash order) gives each player its own path: no order shows.
// The base's tooltips (a HashSet of identity hash codes) never hold two different texts in one frame here (one
// player's "invalid action" / "victory!", or its two identical "timeout!").
// Not reproduced / not modelled: the wall clock (see engine.h); the order of a frame's tooltips (never observable here);
// the cost of the Java path search, which queues a cell once per shortest path reaching it: on the 9 x 9 board it stays
// small (the slowest Java step seen in 600,000 random cases took 156 ms; the worst wall layout an adversarial search
// found copies about 460,000 path cells), but on large boards (init properties only, e.g. 30 x 50, or 50 x 3 with a
// wall across two rows) the jar runs for minutes or exhausts its memory where this port answers at once.
//
// Validation only: compiled with -DGREAT_ESCAPE_PROPS=1, initReferee first loads the text of the environment variable
// GE_PROPS ("key=value" lines, e.g. CodinGame's positions / symmetric) into the init properties, to replay CodinGame's
// games through the CLI. The default build has none of this.
//
// Validation (referees/README.md): referee-diff against the jar with 32 deterministic bots (every ordered pair on two seed
// bases, 3-player covering designs on two more, a random spot check: 3144 games, all identical), direct runs comparing
// the jar's game logs with this port's trace (views, summaries, tooltips; raw bytes and line endings: 589 games), the
// jar's own compiled classes driven by reflection over random properties and command sequences (608,500 cases, 25/25
// mutations detected), and 368 of CodinGame's replays (2 and 3 players, symmetric starts included) reproduced through
// this port's CLI with CodinGame's init properties.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/legacy.h"

#include <cstdlib>
#include <optional>
#include <vector>

#ifndef GREAT_ESCAPE_PROPS
#define GREAT_ESCAPE_PROPS 0
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

const int DEFAULT_WIDTH = 9;
const int DEFAULT_HEIGHT = 9;
const int MAX_ROUND_COUNT = 100;

String S(i64 v) { return java::str(v); }

// Integer.parseInt: a NumberFormatException (a RuntimeException) when it fails
i32 parseIntOrThrow(const String& s) {
    i32 v;
    if (!java::parseInt(s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(s))};
    return v;
}

// Referee.Cell (equals / hashCode by value)
struct Cell {
    i32 x = 0, y = 0;
};

// Referee.Goal: VGoal (a column) or HGoal (a row)
struct Goal {
    bool vertical = true;
    i32 value = 0;
    bool isReached(const Cell& c) const { return vertical ? c.x == value : c.y == value; }
};

enum class Orientation { VERTICAL, HORIZONTAL };
const char16_t* label(Orientation o) { return o == Orientation::VERTICAL ? u"V" : u"H"; }

// Referee.Wall
struct Wall {
    bool justPlaced = true;
    Cell cell;
    Orientation dir = Orientation::VERTICAL;
    int player = 0;

    // block(from, to): whether the wall is between the two cells
    bool block(const Cell& from, const Cell& to) const {
        if (to.x == from.x) {
            if (dir == Orientation::HORIZONTAL && to.x >= cell.x && to.x <= cell.x + 1) return cell.y > std::min(from.y, to.y) && cell.y <= std::max(from.y, to.y);
            return false;
        } else if (to.y == from.y) {
            if (dir == Orientation::VERTICAL && to.y >= cell.y && to.y <= cell.y + 1) return cell.x > std::min(from.x, to.x) && cell.x <= std::max(from.x, to.x);
            return false;
        }
        throw java::Exception{"java.lang.RuntimeException: Illegal state"}; // (never: neighbouring cells only)
    }
    // conflict(newWall): overlapping or crossing
    bool conflict(const Wall& newWall) const {
        if (newWall.dir == Orientation::HORIZONTAL) {
            if (dir == Orientation::HORIZONTAL) return newWall.cell.y == cell.y && java::abs(newWall.cell.x - cell.x) <= 1;
            return newWall.cell.x == cell.x - 1 && newWall.cell.y == cell.y + 1;
        }
        if (dir == Orientation::HORIZONTAL) return cell.x == newWall.cell.x - 1 && cell.y == newWall.cell.y + 1;
        return newWall.cell.x == cell.x && java::abs(newWall.cell.y - cell.y) <= 1;
    }
    String toString() const { return S(cell.x) + u" " + S(cell.y) + u" " + label(dir); }
};

// Referee.Player
struct Player {
    int id = 0;
    int wallCount = 0;
    std::vector<Cell> way; // LinkedList<Cell>: every position, the last is the current one
    std::optional<std::vector<Cell>> minimalway;
    Goal goal;
    int score = 0;
    NString lastAction;
    std::optional<int> winTime, deathTime;
    NString message;

    Cell getPosition() const { return way.back(); }
    bool win() const { return goal.isReached(getPosition()); }
    void setDead(int frame) {
        deathTime = frame;
        lastAction.reset();
        minimalway.reset();
    }
    // compareTo: winners above the others (an earlier win higher), the players still in the game above the losers
    // (a later loss higher)
    int compareTo(const Player& other) const {
        if (!other.winTime || !winTime) {
            if (!other.winTime && !winTime) {
                if (other.deathTime && !deathTime) return 1;
                if (!other.deathTime && deathTime) return -1;
                if (!other.deathTime && !deathTime) return 0;
                return java::sub(*deathTime, *other.deathTime);
            }
            return !other.winTime ? 1 : -1;
        }
        return java::sub(*other.winTime, *winTime);
    }
};

// A player's line, matched like Matcher#matches of PLAYER_OUTPUT_PATTERN
//   ((?<direction>UP|RIGHT|DOWN|LEFT)|((?<x>[0-9]+)\s+(?<y>[0-9]+)\s*(?<orientation>H|V)))(\s+(?<message>.*))?
// [0-9] is ASCII, \s is [ \t\n\x0B\f\r], '.' anything but a line terminator (\n \r U+0085 U+2028 U+2029; the base's
// Scanner already split the line at those). Backtracking never helps: the directions start with distinct letters and
// a wall with a digit; a shorter digit run is followed by a digit, not by the space / orientation the regex needs; the
// greedy \s+ before the message leaves '.*' the rest, which it matches unless it holds a line terminator.
struct Output {
    bool wall = false;
    int direction = 0; // Direction: UP, LEFT, RIGHT, DOWN
    String xs, ys;
    Orientation orientation = Orientation::VERTICAL;
    NString message;
};

bool matchOutput(const String& s, Output& o) {
    static const char16_t* const NAMES[] = {u"UP", u"LEFT", u"RIGHT", u"DOWN"};
    size_t i = 0;
    bool direction = false;
    for (int d = 0; d < 4 && !direction; d++) {
        String name(NAMES[d]);
        if (s.compare(0, name.size(), name) == 0) {
            o.direction = d;
            i = name.size();
            direction = true;
        }
    }
    if (!direction) {
        size_t a = i;
        while (i < s.size() && java::isRegexDigit(s[i])) i++;
        if (i == a) return false;
        o.xs = s.substr(a, i - a);
        size_t sp = i;
        while (i < s.size() && java::isRegexSpace(s[i])) i++;
        if (i == sp) return false;
        size_t b = i;
        while (i < s.size() && java::isRegexDigit(s[i])) i++;
        if (i == b) return false;
        o.ys = s.substr(b, i - b);
        while (i < s.size() && java::isRegexSpace(s[i])) i++;
        if (i >= s.size() || (s[i] != u'H' && s[i] != u'V')) return false;
        o.orientation = s[i] == u'H' ? Orientation::HORIZONTAL : Orientation::VERTICAL;
        o.wall = true;
        i++;
    }
    if (i == s.size()) {
        o.message.reset();
        return true;
    }
    if (!java::isRegexSpace(s[i])) return false;
    while (i < s.size() && java::isRegexSpace(s[i])) i++;
    for (size_t k = i; k < s.size(); k++)
        if (java::isLineSeparator(s[k])) return false;
    o.message = s.substr(i);
    return true;
}

// Direction: UP(0,-1), LEFT(-1,0), RIGHT(1,0), DOWN(0,1)
const int DIRECTION_DX[] = {0, -1, 1, 0};
const int DIRECTION_DY[] = {-1, 0, 0, 1};

struct Referee : legacy::MultiReferee {
    int width = 0, height = 0;
    int slots = 0;               // players.length: the Player[] is allocated first, filled at the end of initReferee
    std::vector<Player> players; // the players once created (empty after an init error: a Player[] of nulls)
    std::vector<Wall> walls;
    bool symmetric = false;
    std::vector<i32> positions;
    // the path search's queue and marks
    std::vector<int> parent, queue;

    Referee() { base = legacy::Base::greatEscape2014(); }

    void populateMessages(std::map<std::string, String>& p) override {
        p["InvalidMove"] = u"$%d hit a wall";
        p["InvalidMoveBorder"] = u"$%d tried to move outside the game area";
        p["InvalidWall"] = u"$%d: invalid wall position";
        p["BlockForbidden"] = u"$%d's wall (%d,%d,%s) blocking $%d is forbidden";
        p["exitReached"] = u"$%d: exit reached";
        p["noWallLeft"] = u"$%d: no walls left";
        p["playerMove"] = u"$%d moved from (%d,%d) to (%d,%d)";
        p["playerPutWall"] = u"$%d placed a wall (%d,%d,%s)";
    }

    // --- the path search ---
    // CustomPathfinding#findWay: a breadth-first search over a LinkedList of paths (neighbours left, up, right, down;
    // a cell closed when its path is taken out; the first path taken out that ends on the goal). The Java search
    // queues a cell again for every path reaching it before it is closed; the first queued path of each cell is the
    // one of the first-closed neighbour that reaches it and the copies come later in the FIFO order, so the paths
    // taken out first per cell are exactly those of a plain BFS marking cells when queued (and the first one on the
    // goal is such a first path). null (nullopt) when no goal cell is reachable.
    bool isBlocked(const Cell& from, const Cell& to) const {
        for (const Wall& w : walls)
            if (w.block(from, to)) return true;
        return false;
    }
    std::optional<std::vector<Cell>> findWay(const Cell& start, const Goal& goal) {
        parent.assign((size_t)width * (size_t)height, -2); // -2: not queued, -1: the start
        queue.clear();
        auto index = [&](const Cell& c) { return (size_t)c.y * (size_t)width + (size_t)c.x; };
        parent[index(start)] = -1;
        queue.push_back((int)index(start));
        for (size_t head = 0; head < queue.size(); head++) {
            int c = queue[head];
            Cell last{c % width, c / width};
            if (goal.isReached(last)) {
                std::vector<Cell> path;
                for (int k = c; k != -1; k = parent[(size_t)k]) path.push_back(Cell{k % width, k / width});
                std::reverse(path.begin(), path.end());
                return path;
            }
            const Cell next[4] = {{last.x - 1, last.y}, {last.x, last.y - 1}, {last.x + 1, last.y}, {last.x, last.y + 1}};
            for (const Cell& t : next) {
                if (t.x >= 0 && t.x < width && t.y >= 0 && t.y < height) {
                    if (parent[index(t)] == -2 && !isBlocked(last, t)) {
                        parent[index(t)] = c;
                        queue.push_back((int)index(t));
                    }
                }
            }
        }
        return std::nullopt;
    }
    std::optional<std::vector<Cell>> computeMinimalWay(const Player& p) { return findWay(p.getPosition(), p.goal); }

    void recomputeMinimalWay() {
        for (Player& p : players) p.minimalway = computeMinimalWay(p);
    }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
#if GREAT_ESCAPE_PROPS
        if (const char* extra = getenv("GE_PROPS")) prop.load(java::decodeUtf8(std::string(extra)));
#endif
        slots = playerCount;
        players.clear();
        walls.clear();
        width = parseIntOrThrow(prop.getProperty(u"width", S(DEFAULT_WIDTH)));
        height = parseIntOrThrow(prop.getProperty(u"height", S(DEFAULT_HEIGHT)));
        symmetric = java::parseBoolean(prop.getProperty(u"symmetric", u"false"));
        width = std::min(50, std::max(0, width));
        height = std::min(50, std::max(0, height));

        // cg-2015-great-escape.patch: new Random(Long.parseLong(seed)) with the init property "seed", else new Random()
        java::Random r;
        if (const String* seed = prop.find(u"seed")) {
            i64 v;
            if (!java::parseLong(*seed, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(*seed))};
            r = java::Random(v);
        } else {
            r = java::Random((i64)java::entropy());
        }
        // the default value of "positions" is built (four draws) whether the property is given or not
        String drawn = S(r.nextIntChecked(width));
        drawn += u" " + S(r.nextIntChecked(width));
        drawn += u" " + S(r.nextIntChecked(height));
        drawn += u" " + S(r.nextIntChecked(height));
        std::vector<String> ps = java::split(prop.getProperty(u"positions", drawn), u' ');
        if ((int)ps.size() < playerCount) {
            String again = S(r.nextIntChecked(width));
            again += u" " + S(r.nextIntChecked(width));
            again += u" " + S(r.nextIntChecked(height));
            again += u" " + S(r.nextIntChecked(height));
            ps = java::split(again, u' ');
        }
        positions.assign(ps.size(), 0);
        for (size_t i = 0; i < ps.size(); ++i) {
            positions[i] = parseIntOrThrow(ps[i]);
            if (i <= 1 && (positions[i] < 0 || positions[i] >= height)) positions[i] = r.nextIntChecked(height);
            if (i >= 2 && (positions[i] < 0 || positions[i] >= width)) positions[i] = r.nextIntChecked(width);
        }
        if (symmetric) {
            if (positions.size() < 2) throw java::Exception{java::arrayIndexOutOfBoundsException(1, (i64)positions.size())};
            positions[1] = positions[0];
            if (playerCount >= 4) positions[3] = positions[2];
        }
        int wallperPlayer = 20 / playerCount;
        for (int i = 0; i < playerCount; ++i) {
            Player p;
            p.id = i;
            p.wallCount = wallperPlayer;
            i32 position = positions[(size_t)i];
            switch (i % 4) {
            case 0:
                p.way.push_back(Cell{0, position});
                p.goal = Goal{true, width - 1};
                break;
            case 1:
                p.way.push_back(Cell{width - 1, height - 1 - position});
                p.goal = Goal{true, 0};
                break;
            case 2:
                p.way.push_back(Cell{width - 1 - position, 0});
                p.goal = Goal{false, height - 1};
                break;
            default:
                p.way.push_back(Cell{position, height - 1});
                p.goal = Goal{false, 0};
                break;
            }
            players.push_back(p);
        }
        recomputeMinimalWay();
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"width", S(width));
        prop.setProperty(u"height", S(height));
        String ps;
        for (i32 v : positions) ps += S(v) + u" ";
        prop.setProperty(u"positions", java::trim(ps));
        prop.setProperty(u"symmetric", symmetric ? u"true" : u"false");
        return prop;
    }

    String getGameName() override { return u"TheGreatEscape"; }

    void handlePlayerOutput(int frame, int, int playerId, const Lines& outputs) override {
        Player& player = players[(size_t)playerId];
        try {
            play(frame, playerId, player, outputs);
        } catch (const legacy::LostException&) {
            player.setDead(frame);
            updateScores();
            throw;
        } catch (const legacy::InvalidInputException&) {
            player.setDead(frame);
            updateScores();
            throw;
        } catch (...) { // the WinException, or a NumberFormatException (the referee thread dies)
            updateScores();
            throw;
        }
        updateScores();
    }

    void play(int frame, int playerId, Player& player, const Lines& outputs) {
        for (Wall& wall : walls) wall.justPlaced = false;
        if (outputs.empty()) throw java::Exception{java::arrayIndexOutOfBoundsException(0, 0)}; // (never: one line)
        const String& output = outputs[0];
        player.message.reset();
        Output m;
        // (the Java text has a no-break space U+00A0 after "or")
        if (!matchOutput(output, m)) throw legacy::InvalidInputException(u"(x y H|V) or\u00A0(UP | RIGHT | DOWN | LEFT)", output);
        NString message = m.message;
        if (message && message->size() > 20) message = message->substr(0, 20);
        player.message = message;
        if (m.wall) {
            i32 x = parseIntOrThrow(m.xs);
            i32 y = parseIntOrThrow(m.ys);
            if (player.wallCount <= 0) throw legacy::LostException(std::string("noWallLeft"), {playerId});
            if (x >= width) throw legacy::InvalidInputException(u"0<=x<=" + S(width - 1), u"x=" + S(x));
            if (y >= height) throw legacy::InvalidInputException(u"0<=y<=" + S(height - 1), u"y=" + S(y));
            if (m.orientation == Orientation::HORIZONTAL) {
                if (y <= 0) throw legacy::InvalidInputException(u"0<y<=" + S(height - 1), u"y=" + S(y));
                if (x >= width - 1) throw legacy::InvalidInputException(u"0<=x<" + S(width - 1), u"x=" + S(x));
            } else {
                if (x <= 0) throw legacy::InvalidInputException(u"0<x<=" + S(width - 1), u"x=" + S(x));
                if (y >= height - 1) throw legacy::InvalidInputException(u"0<=y<" + S(height - 1), u"y=" + S(y));
            }
            player.wallCount--;
            Wall newWall;
            newWall.player = playerId;
            newWall.cell = Cell{x, y};
            newWall.dir = m.orientation;
            for (const Wall& wall : walls)
                if (wall.conflict(newWall)) throw legacy::LostException(std::string("InvalidWall"), {playerId});
            walls.push_back(newWall);
            player.lastAction = translate("playerPutWall", {playerId, x, y, String(label(m.orientation))});
            // HashMap<Player, List<Cell>> newWays: each player gets its own path (the order shows nowhere)
            std::vector<std::pair<Player*, std::vector<Cell>>> newWays;
            for (Player& p : players) {
                if (!p.winTime && !p.deathTime) {
                    std::optional<std::vector<Cell>> newWay = computeMinimalWay(p);
                    if (!newWay) {
                        walls.pop_back(); // walls.remove(newWall): the last one
                        throw legacy::LostException(std::string("BlockForbidden"), {playerId, x, y, String(label(m.orientation)), p.id});
                    }
                    newWays.push_back({&p, std::move(*newWay)});
                }
            }
            for (auto& e : newWays) e.first->minimalway = std::move(e.second);
        } else {
            Cell from = player.getPosition();
            Cell cell{from.x + DIRECTION_DX[m.direction], from.y + DIRECTION_DY[m.direction]};
            if (!canMoveTo(player, cell)) throw legacy::LostException(std::string("InvalidMoveBorder"), {playerId});
            if (isBlocked(from, cell)) throw legacy::LostException(std::string("InvalidMove"), {playerId});
            player.lastAction = translate("playerMove", {playerId, from.x, from.y, cell.x, cell.y});
            player.way.push_back(cell);
            player.minimalway = computeMinimalWay(player);
            if (player.win()) {
                legacy::WinException e(std::string("exitReached"), {playerId});
                player.lastAction = translate("exitReached", {playerId});
                player.winTime = frame;
                throw e;
            }
        }
    }

    bool canMoveTo(const Player& p, const Cell& cell) const {
        Cell pos = p.getPosition();
        if (cell.x == pos.x) return cell.y >= 0 && cell.y < height && java::abs(pos.y - cell.y) == 1;
        if (cell.y == pos.y) return cell.x >= 0 && cell.x < width && java::abs(pos.x - cell.x) == 1;
        return false;
    }

    // the game's updateScores: the ranks (Arrays.sort of a copy of the players: ComparableTimSort = java::sort)
    void updateScores() {
        std::vector<Player*> sorted;
        for (Player& p : players) sorted.push_back(&p);
        java::sort(sorted, [](const Player* a, const Player* b) { return a->compareTo(*b); });
        int score = 0;
        const Player* last = nullptr;
        for (Player* p : sorted) {
            if (last != nullptr && p->compareTo(*last) > 0) score++;
            p->score = score;
            last = p;
        }
    }

    Lines getInitDataForView() override { return {S(width) + u" " + S(height) + u" " + S(slots)}; }

    Lines getFrameDataForView(int, int, int, bool) override {
        // after an init error the players are null: a NullPointerException (the base catches it, the view has no frame data)
        if ((int)players.size() < slots) throw java::Exception{"java.lang.NullPointerException"};
        Lines list;
        for (const Player& p : players) {
            Cell pos = p.getPosition();
            String str = S(pos.x) + u" " + S(pos.y) + u" " + S(p.wallCount) + u" " + S(p.deathTime ? 1 : (p.winTime ? 2 : 0));
            if (p.minimalway)
                for (const Cell& cell : *p.minimalway) str += u" " + S(cell.x) + u" " + S(cell.y);
            list.push_back(str);
            list.push_back(p.message ? *p.message : String());
        }
        Lines wallsList;
        for (const Wall& wall : walls)
            if (wall.justPlaced) wallsList.push_back(S(wall.player) + u" " + S(wall.cell.x) + u" " + S(wall.cell.y) + u" " + label(wall.dir));
        list.push_back(S((i64)wallsList.size()));
        list.insert(list.end(), wallsList.begin(), wallsList.end());
        return list;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 1; }

    Lines getInitInputForPlayer(int playerIdx) override {
        return {S(width) + u" " + S(height) + u" " + S((i64)players.size()) + u" " + S(playerIdx)};
    }

    Lines getInputForPlayer(int, int) override {
        Lines list;
        for (const Player& p : players) {
            if (!p.deathTime && !p.winTime) list.push_back(S(p.getPosition().x) + u" " + S(p.getPosition().y) + u" " + S(p.wallCount));
            else list.push_back(u"-1 -1 -1");
        }
        list.push_back(S((i64)walls.size()));
        for (const Wall& wall : walls) list.push_back(wall.toString());
        return list;
    }

    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }
    void updateGame(int) override {}
    void prepare(int) override {}
    bool isPlayerDead(int) override { return false; }
    NString getDeathReason(int) override { throw java::Exception{"java.lang.RuntimeException"}; } // (never called)
    int getScore(int playerIdx) override { return players[(size_t)playerIdx].score; }

    Lines getGameSummary(int) override {
        Lines scores;
        scores.push_back(u"Scores:");
        int player = 0;
        for (const Player& p : players) scores.push_back(u"$" + S(player++) + u": " + S(p.score));
        return scores;
    }

    Lines getPlayerActions(int playerIdx, int) override {
        const Player& p = players[(size_t)playerIdx];
        if (!p.lastAction) return {};
        return {*p.lastAction};
    }

    void setPlayerTimeout(int frame, int, int playerIdx) override {
        players[(size_t)playerIdx].deathTime = frame;
        updateScores();
    }

    int getMaxRoundCount(int) override { return MAX_ROUND_COUNT; }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
