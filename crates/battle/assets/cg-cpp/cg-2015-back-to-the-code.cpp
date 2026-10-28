// Back to the Code (CodinGame, 2015, "back-to-the-code") - headless C++ referee, port of the CodinGame referee in
// https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the only commit of the file),
// "Referees/Back to the Code/Referee.java": the single-file referee `class Referee extends MultiReferee` with CodinGame's
// old AbstractReferee base of 2015 embedded (the same text as Coders Strike Back's base, whitespace aside: common/legacy.h,
// legacy::Base::codersStrikeBack2015(): getMillisTimeForRound 150, no "LostTooltip" message, no precomputed inputs, the
// frame data and the tooltips sent in every frame, no gameOver(); its SoloReferee class is unused). The file is the game
// CodinGame runs today: its classes reproduce CodinGame's own replays (views, infos, summaries, tooltips, scores).
// 2 to 4 players (CodinGame runs 2, 3 and 4; getMinimumPlayerCount() is 1: a player left alone plays on until the game's
// own end rules stop it). No leagues. Deterministic: reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 ... -p4 ...] -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): same_cell, seed, positions,
// symmetric, in the order of a JDK 9+ Properties) to stdout like the jar (referees/cg-builder/legacy: the referee runs
// unchanged in the runner of the SDK 4.5.0). "null" scores when the game fails (the Java referee crashed or the protocol
// broke; the jar after 150 s). -l writes the trace of common/legacy.h (bot executions, view sizes, summaries, tooltips,
// END; CG_TRACE_VIEWS=1 adds the text of every view). Time limits: the base's 1000 ms for the first turn, 150 ms after
// (the game overrides neither).
//
// Start positions: the referee reads the init properties seed, symmetric, same_cell and positions ("x,y" pairs). The
// CLI only passes the seed, so the players start on distinct random cells (java.util.Random(seed): nextInt(35),
// nextInt(20) per player, redrawn when taken). CodinGame's matches also set symmetric (positions rotated around the
// center from the first random one: Math.atan2, Math.cos / Math.sin rounded to 1e-9, floor / ceil away from the center)
// or same_cell (every player on the first random cell): this port implements both (checked against CodinGame's
// replays with the validation build below), but the CLI never selects them.
//
// The game: a 35 x 20 grid; each round every player answers "x y [message]" (one step towards (x, y): along x first,
// then along y) or "BACK n [message]" (1 <= n <= 25, once per game: the game goes back n rounds after this one, every
// player's moves and cells of those rounds undone, the round limit extended). A player alone on a free cell owns it;
// a player whose cell touches (Manhattan 1) one of its positions older than the previous round fills every free area
// 8-connected to its cell's neighbours that reaches neither the border nor another player's cell. Score: the cells
// owned. Out on an invalid line (x / y off the grid, n out of range, a second BACK, anything else) or a timeout: its
// cells stay. The game ends after 350 / 300 / 250 rounds (2 / 3 / 4 players) plus the rounds gone back, after 25
// rounds without a move or a BACK ("inactivityDetected"), one round after the ranking can no longer change even with
// every BACK left ("EndOfGameDetected"), or when every player is out.
//
// The Java referee is followed closely, notably: the regexes (Matcher#matches with Java's backtracking, see matchInt /
// matchTail: x and y of 1 to 9 digits after an optional '-', separated by spaces, "BACK" in ASCII case-insensitive form,
// the message after exactly one space made of printable ASCII \p{Print} only, cut to 100 chars), the order of the checks
// (x before y, a BACK line's stay recorded before its checks), every Stack of the players (positions, the cells filled
// each round, the cell counts) and their truncation going back in time, the 8-neighbour order of the zone fill
// (Stack-based search, an early stop at the border or at another player's cell), several players going back in one
// round (amounts added, capped at the in-game round - 1, a BACK in round 1 used up without effect), the inputs computed
// when each player plays (a player out earlier in the round shows "-1 -1 0"), the anticipated end (TimSort with the
// Potential comparator), the summaries / actions / view built in full (the view stays far below the runner's 30000-byte
// limit: at most the 700 cells and four 100-char messages).
// Identity hash order (class Cell has no hashCode): the Java referee keeps the cells filled in a round in a
// HashSet<Cell> and the players on a free cell in a HashMap<Cell, List<Player>>; their iteration orders depend on
// identity hash codes, which the jar does not reproduce from run to run. Every use but one is order-free: the map's
// entries each own a different cell; the zone search only asks the set's membership; the cells filled are all given
// the same owner and round, counted by size(); going back in time resets each cell on its own condition. The one
// order that shows is the view's list of a round's filled cells (frame data only: no bot input, score, summary or
// infos sees it): this port lists them in the order the search found them (the jar in its identity hash order;
// CodinGame's replays in yet another one). So games, inputs and scores are deterministic and the same as the jar's.
// Not reproduced / not modelled: the wall clock (see engine.h); that view order; the order of the tooltips of a frame
// (the base's HashSet of identity hash codes: legacy.h; this game never has two in one frame).
//
// Validation only: compiled with -DBACK_TO_THE_CODE_PROPS=1, initReferee first loads the text of the environment
// variable BTTC_PROPS ("key=value" lines, e.g. CodinGame's same_cell / symmetric) into the init properties, to replay
// CodinGame's games through the CLI. The default build has none of this.
//
// Validation (referees/README.md): referee-diff against the jar with 33 deterministic bots (every ordered pair on two
// seed bases, 3 and 4 player covering designs on two more: 3222 games, all identical), direct runs comparing the jar's
// game logs with this port's trace (views up to the filled-cell order, summaries, tooltips; raw bytes and line endings),
// the jar's own compiled classes driven by reflection over random games and states (448,200 cases), and 696 of
// CodinGame's replays (2, 3 and 4 players, all three start variants) reproduced through this port's CLI.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/legacy.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#ifndef BACK_TO_THE_CODE_PROPS
#define BACK_TO_THE_CODE_PROPS 0
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// --- constants ---
const int BACK_IN_TIME_COUNT = 1;
const int BACK_IN_TIME_MAX = 25;
const int MAX_INACTIVE_ROUND_COUNT = 25;
const int GRID_WIDTH = 35, GRID_HEIGHT = 20; // GRID_SIZE, a java.awt.Dimension
const double PI = 3.141592653589793;         // Math.PI

String S(i64 v) { return java::str(v); }

[[noreturn]] void arrayIndexOutOfBounds(i64 index, i64 length) { throw java::Exception{java::arrayIndexOutOfBoundsException(index, length)}; }

// Stack#get / Vector#get (an ArrayIndexOutOfBoundsException out of range) and Stack#peek (an EmptyStackException)
template <class T>
const T& at(const std::vector<T>& v, int index) {
    if (index < 0 || index >= (int)v.size()) arrayIndexOutOfBounds(index, (i64)v.size());
    return v[(size_t)index];
}
template <class T>
const T& peek(const std::vector<T>& v) {
    if (v.empty()) throw java::Exception{"java.util.EmptyStackException"};
    return v.back();
}

// --- java.awt.Point ---
struct Point {
    i32 x = 0, y = 0;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

struct Player;

// Referee.Cell (no equals / hashCode: identity)
struct Cell {
    Player* owner = nullptr;
    int ownershipRound = 0;
    Point position;
    void setOwner(Player* o, int round) {
        owner = o;
        ownershipRound = round;
    }
    int manhattanDistance(const Cell& b) const { return java::abs(position.x - b.position.x) + java::abs(position.y - b.position.y); }
};

// Referee.Vector (immutable: a value here; only the symmetric start positions use it)
struct Vector {
    double x = 0, y = 0;
    Vector() {}
    Vector(double x, double y) : x(x), y(y) {}
    // new Vector(a, b): b - a
    static Vector between(const Vector& a, const Vector& b) { return Vector(b.x - a.x, b.y - a.y); }
    Vector add(const Vector& v) const { return Vector(x + v.x, y + v.y); }
    Vector mult(double factor) const { return Vector(x * factor, y * factor); }
    double length() const { return std::sqrt(x * x + y * y); }
    double angle() const { return java::atan2(y, x); }
    // rotate(angle): Math.cos / Math.sin rounded to 1e-9 (Math.round, a long, divided by 1e9)
    Vector rotate(double angle) const {
        double cos = (double)java::round(java::cos(angle) * 1000000000.0) / 1000000000.0;
        double sin = (double)java::round(java::sin(angle) * 1000000000.0) / 1000000000.0;
        return Vector(x * cos - y * sin, x * sin + y * cos);
    }
};

// Referee.Player
struct Player {
    int id = 0;
    std::vector<Cell*> positions;               // Stack<Cell>: the position of every in-game round
    std::vector<std::vector<Cell*>> ownedCells; // Stack<Set<Cell>>: the cells filled each round (see the header: order)
    NString message;
    int backInTimeLeft = 0;
    bool lost = false;
    NString lostReason;
    int backInTimeCount = 0;
    std::vector<int> ownedCellCounts; // Stack<Integer>: the cell count of every in-game round
    int ownedCellCount = 0;

    Player(int id, Cell* p, int backInTimeLeft) : id(id), backInTimeLeft(backInTimeLeft) { positions.push_back(p); }

    void setLost(const String& reason) {
        lost = true;
        lostReason = reason;
        message.reset();
    }
    bool canBackInTime() {
        if (backInTimeLeft > 0) {
            backInTimeLeft--;
            return true;
        }
        return false;
    }
    // own(Set<Cell>)
    void own(const std::vector<Cell*>& cells) {
        ownedCells.push_back(cells);
        ownedCellCount += (int)cells.size();
        ownedCellCounts.push_back(ownedCellCount);
    }
    // own(Cell, round)
    void own(Cell* cell, int inGameRound) {
        cell->setOwner(this, inGameRound);
        ++ownedCellCount;
    }
    void dontMove() { positions.push_back(getLastPosition()); }
    void setMessage(const NString& m) {
        if (m && m->size() > 100) message = m->substr(0, 100);
        else message = m;
    }
    Cell* getLastPosition() const { return peek(positions); }
    Cell* getPositionAtRound(int round) const { return at(positions, round); }
    const std::vector<Cell*>& getOwnedCellsAtRound(int round) const { return at(ownedCells, round); }
    int getOwnedCellCountAtRound(int round) const { return at(ownedCellCounts, round); }
    int getLastOwnedCellCount() const { return peek(ownedCellCounts); }

    // backInTime(round): the rounds after `round` undone, each cell owned after it by this player freed
    void backInTime(int round) {
        auto release = [&](Cell* cell) {
            if (cell->ownershipRound > round && cell->owner == this) {
                cell->owner = nullptr;
                ownedCellCount--;
            }
        };
        if ((int)positions.size() > round) {
            for (size_t i = (size_t)round + 1; i < positions.size(); i++) release(positions[i]);
            positions.resize((size_t)round + 1);
        }
        if ((int)ownedCells.size() > round) {
            for (size_t i = (size_t)round + 1; i < ownedCells.size(); i++)
                for (Cell* cell : ownedCells[i]) release(cell);
            ownedCells.resize((size_t)round + 1);
        }
        if ((int)ownedCellCounts.size() > round) ownedCellCounts.resize((size_t)round + 1);
    }

    bool moved() const { return positions.size() >= 2 && positions[positions.size() - 2] != positions.back(); }
};

// the Java regexes, matched by hand (Matcher#matches: the whole line)
//   POSITION_PATTERN    (?<x>-?[0-9]{1,9}) +(?<y>-?[0-9]{1,9})(?: (?<message>\p{Print}+))?          CASE_INSENSITIVE
//   BACKINTIME_PATTERN  BACK +(?<rounds>-?[0-9]{1,9})(?: (?<message>\p{Print}+))?                    CASE_INSENSITIVE
// [0-9] and \p{Print} ([\x20-\x7E]) are ASCII, untouched by CASE_INSENSITIVE (without UNICODE_CASE only the ASCII letters
// of "BACK" fold). Backtracking never helps: a shorter digit run is followed by a digit, fewer spaces by a space.

// -?[0-9]{1,9} at i, followed by what the caller checks: the whole digit run, 1 to 9 digits
bool matchInt(const String& s, size_t& i) {
    size_t j = i;
    if (j < s.size() && s[j] == u'-') j++;
    size_t d = j;
    while (j < s.size() && java::isRegexDigit(s[j])) j++;
    if (j == d || j - d > 9) return false;
    i = j;
    return true;
}

// " +" at i: one space or more (U+0020 only), all of them
bool matchSpaces(const String& s, size_t& i) {
    size_t j = i;
    while (j < s.size() && s[j] == u' ') j++;
    if (j == i) return false;
    i = j;
    return true;
}

// (?: (?<message>\p{Print}+))? then the end of the line
bool matchTail(const String& s, size_t i, NString& message) {
    if (i == s.size()) {
        message.reset();
        return true;
    }
    if (s[i] != u' ' || i + 1 == s.size()) return false;
    for (size_t k = i + 1; k < s.size(); k++)
        if (s[k] < 0x20 || s[k] > 0x7E) return false;
    message = s.substr(i + 1);
    return true;
}

bool matchPosition(const String& s, String& xs, String& ys, NString& message) {
    size_t i = 0;
    if (!matchInt(s, i)) return false;
    xs = s.substr(0, i);
    if (!matchSpaces(s, i)) return false;
    size_t y0 = i;
    if (!matchInt(s, i)) return false;
    ys = s.substr(y0, i - y0);
    return matchTail(s, i, message);
}

bool matchBack(const String& s, String& rounds, NString& message) {
    if (s.size() < 4) return false;
    for (size_t k = 0; k < 4; k++)
        if (!java::regexCharMatchesIgnoreCase(s[k], "BACK"[k])) return false;
    size_t i = 4;
    if (!matchSpaces(s, i)) return false;
    size_t r0 = i;
    if (!matchInt(s, i)) return false;
    rounds = s.substr(r0, i - r0);
    return matchTail(s, i, message);
}

// Pattern "(?<x>-?[0-9]+),(?<y>-?[0-9]+)" with Matcher#find from `from` (the property "positions"): the first start where
// it matches; both digit runs whole (x's must be followed by ','). On a match, `from` moves past it.
bool findPosition(const String& s, size_t& from, String& xs, String& ys) {
    for (size_t st = from; st < s.size(); st++) {
        size_t i = st;
        if (s[i] == u'-') i++;
        size_t d = i;
        while (i < s.size() && java::isRegexDigit(s[i])) i++;
        if (i == d || i >= s.size() || s[i] != u',') continue;
        xs = s.substr(st, i - st);
        size_t y0 = ++i;
        if (i < s.size() && s[i] == u'-') i++;
        size_t e = i;
        while (i < s.size() && java::isRegexDigit(s[i])) i++;
        if (i == e) continue;
        ys = s.substr(y0, i - y0);
        from = i;
        return true;
    }
    return false;
}

// customRound(center, value): towards the center
int customRound(double center, double value) {
    if (value > center) return java::d2i(std::floor(value));
    return java::d2i(std::ceil(value));
}

struct Referee : legacy::MultiReferee {
    std::vector<std::unique_ptr<Player>> players; // Player[]: null until generatePlayer
    std::vector<Cell> grid;                       // Cell[GRID_HEIGHT][GRID_WIDTH]
    int inGameRound = 0;
    int backInTimeAmount = 0;
    int reality = 0;
    int additionnalRoundCount = 0;
    bool endOfGameDetected = false;
    i64 seed = 0;
    bool symmetric = false;
    bool sameCell = false;
    std::vector<Point> startPositions;
    int lastActionRound = 0;
    // the zone search's HashSet (membership only), its Stack
    std::vector<int> zoneMark;
    int zoneStamp = 0;
    std::vector<Cell*> zoneStack;

    Referee() { base = legacy::Base::codersStrikeBack2015(); }

    Cell* cellAt(int x, int y) { return &grid[(size_t)(y * GRID_WIDTH + x)]; }
    int index(const Cell* c) const { return (int)(c - grid.data()); }

    Player& player(int i) {
        if (!players[(size_t)i]) throw java::Exception{"java.lang.NullPointerException"};
        return *players[(size_t)i];
    }

    // Cell#getNeighBoringCells(): x then y, the cell itself left out
    template <class F>
    void forNeighbours(const Cell* c, F f) {
        int minX = std::max(0, c->position.x - 1), maxX = std::min(c->position.x + 1, GRID_WIDTH - 1);
        int minY = std::max(0, c->position.y - 1), maxY = std::min(c->position.y + 1, GRID_HEIGHT - 1);
        for (int x = minX; x <= maxX; ++x)
            for (int y = minY; y <= maxY; ++y)
                if (x != c->position.x || y != c->position.y) {
                    if (!f(cellAt(x, y))) return;
                }
    }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        additionnalRoundCount = 0;
        lastActionRound = 0;
        inGameRound = 0;
        endOfGameDetected = false;
        reality = 0;
        players.clear();
        players.resize((size_t)playerCount);
        grid.assign((size_t)(GRID_WIDTH * GRID_HEIGHT), Cell());
        for (int y = 0; y < GRID_HEIGHT; y++)
            for (int x = 0; x < GRID_WIDTH; x++) cellAt(x, y)->position = Point{x, y};
        zoneMark.assign(grid.size(), 0);
        zoneStamp = 0;
#if BACK_TO_THE_CODE_PROPS
        if (const char* extra = getenv("BTTC_PROPS")) prop.load(java::decodeUtf8(std::string(extra)));
#endif
        parseProperties(prop);
        // printError(prop), printError(playerCount): the referee's stderr
        generatePlayer(playerCount);
        checkOwnership();
    }

    void parseProperties(java::Properties& prop) {
        // Long.valueOf(prop.getProperty("seed", String.valueOf(new Random(System.currentTimeMillis()).nextLong())))
        if (const String* s = prop.find(u"seed")) {
            i64 v;
            if (!java::parseLong(*s, v)) throw legacy::InvalidFormatException{u"seed is not an integer"};
            seed = v;
        } else {
            java::Random r((i64)java::entropy());
            seed = r.nextLong();
        }
        symmetric = java::parseBoolean(prop.getProperty(u"symmetric", u"false"));
        sameCell = java::parseBoolean(prop.getProperty(u"same_cell", u"false"));
        // String.valueOf(prop.get("positions")): "null" without one (no match)
        const String* ps = prop.find(u"positions");
        String positionsString = ps ? *ps : String(u"null");
        startPositions.clear();
        size_t from = 0;
        String xs, ys;
        while (findPosition(positionsString, from, xs, ys)) {
            i32 x, y;
            if (!java::parseInt(xs, x)) throw legacy::InvalidFormatException{u"expected : " + xs + u" is not an integer"};
            if (!java::parseInt(ys, y)) throw legacy::InvalidFormatException{u"expected : " + ys + u" is not an integer"};
            if (x < 0 || x >= GRID_WIDTH) throw legacy::InvalidFormatException{u"expected : 0 <= x < " + S(GRID_WIDTH) + u" but found x=" + S(x)};
            if (y < 0 || y >= GRID_HEIGHT) throw legacy::InvalidFormatException{u"expected : 0 <= y < " + S(GRID_HEIGHT) + u" but found y=" + S(y)};
            startPositions.push_back(Point{x, y});
        }
    }

    int getMaxInGameRoundCount(int playerCount) const {
        switch (playerCount) {
        case 2: return 350;
        case 3: return 300;
        default: return 250;
        }
    }
    int getMaxRoundCount(int playerCount) override { return getMaxInGameRoundCount(playerCount) + additionnalRoundCount; }

    void generatePlayer(int playerCount) {
        java::Random random(seed);
        if (startPositions.empty()) {
            i32 x = random.nextInt(GRID_WIDTH);
            i32 y = random.nextInt(GRID_HEIGHT);
            startPositions.push_back(Point{x, y});
        }
        Point firstPoint = startPositions[0];
        if (sameCell) {
            for (int i = 0; i < playerCount; ++i) players[(size_t)i] = std::make_unique<Player>(i, cellAt(firstPoint.x, firstPoint.y), BACK_IN_TIME_COUNT);
        } else if (symmetric) {
            double interAngle = PI * 2 / (double)playerCount;
            Vector center((GRID_WIDTH - 1) / 2.0, (GRID_HEIGHT - 1) / 2.0);
            Vector dir = Vector::between(center, Vector(firstPoint.x, firstPoint.y));
            double startAngle = dir.angle();
            double maxLength = std::sqrt((double)(GRID_WIDTH * GRID_WIDTH / 4 + GRID_HEIGHT * GRID_HEIGHT / 4)); // int arithmetic
            for (int i = 0; i < playerCount; ++i) {
                double angle = startAngle + interAngle * i;
                maxLength = java::min(maxLength, std::fabs(GRID_WIDTH / 2 / java::cos(angle))); // 35 / 2 = 17 (int)
                maxLength = java::min(maxLength, std::fabs(GRID_HEIGHT / 2 / java::sin(angle)));
            }
            dir = dir.mult(java::min(1, maxLength / dir.length()));
            for (int i = 0; i < playerCount; ++i) {
                Vector pos = center.add(dir.rotate(interAngle * i));
                int y = std::min(GRID_HEIGHT - 1, std::max(0, customRound(center.y, pos.y)));
                int x = std::min(GRID_WIDTH - 1, std::max(0, customRound(center.x, pos.x)));
                players[(size_t)i] = std::make_unique<Player>(i, cellAt(x, y), BACK_IN_TIME_COUNT);
            }
        } else {
            while ((int)startPositions.size() < playerCount) {
                i32 x = random.nextInt(GRID_WIDTH);
                i32 y = random.nextInt(GRID_HEIGHT);
                Point p{x, y};
                bool contains = false;
                for (const Point& q : startPositions) contains = contains || q == p;
                if (!contains) startPositions.push_back(p);
            }
            for (int i = 0; i < playerCount; ++i) {
                const Point& p = startPositions[(size_t)i];
                players[(size_t)i] = std::make_unique<Player>(i, cellAt(p.x, p.y), BACK_IN_TIME_COUNT);
            }
        }
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        prop.setProperty(u"symmetric", symmetric ? u"true" : u"false");
        prop.setProperty(u"same_cell", sameCell ? u"true" : u"false");
        String str;
        for (size_t i = 0; i < players.size(); i++) {
            Cell* first = player((int)i).getPositionAtRound(0);
            if (i) str += u" ";
            str += S(first->position.x) + u"," + S(first->position.y);
        }
        prop.setProperty(u"positions", str);
        return prop;
    }

    void populateMessages(std::map<std::string, String>& p) override {
        p["noBackInTimeLeft"] = u"Failure: no remaining back in time";
        p["playerMove"] = u"$%d moved to cell %d,%d";
        p["playerBackInTime"] = u"$%d wanted to go back in time %d rounds";
        p["playerDoNothing"] = u"$%d stayed on cell %d,%d";
        p["gameGoBackInTime"] = u"Game round: %d. Game went back in time %d rounds. Next game round: %d";
        p["playerScore"] = u"$%d score: %d";
        p["gameGoForward"] = u"Game round: %d";
        p["EndOfGameDetected"] = u"Game was ended prematurely as ranking can no longer change";
        p["inactivityDetected"] = u"No action detected for " + S(MAX_INACTIVE_ROUND_COUNT) + u" rounds: game ended prematurely";
    }

    Lines getInitDataForView() override { return {S(GRID_WIDTH) + u" " + S(GRID_HEIGHT)}; }

    Lines getFrameDataForView(int, int, int, bool keyFrame) override {
        Lines data;
        data.push_back(S(keyFrame ? inGameRound : inGameRound - 1) + u" " + S(reality));
        for (const std::unique_ptr<Player>& pp : players) {
            if (!pp) {
                data.push_back(u"0 0 0");
                data.push_back(u"\"\"");
                continue;
            }
            Player& p = *pp;
            String joiner; // a StringJoiner(" ") of non-empty numbers
            auto add = [&](i64 v) {
                if (!joiner.empty()) joiner += u" ";
                joiner += S(v);
            };
            if (keyFrame) {
                add(p.backInTimeLeft);
                add(p.backInTimeCount);
                add(p.getOwnedCellCountAtRound(inGameRound));
                if (!p.lost) {
                    Cell* cell = p.getPositionAtRound(inGameRound);
                    add(cell->position.x);
                    add(cell->position.y);
                    if (cell->owner == &p && cell->ownershipRound >= inGameRound) {
                        add(cell->position.x);
                        add(cell->position.y);
                    }
                    for (Cell* c : p.getOwnedCellsAtRound(inGameRound)) {
                        add(c->position.x);
                        add(c->position.y);
                    }
                }
            }
            data.push_back(joiner);
            data.push_back(u"\"" + (p.message && keyFrame ? *p.message : String()) + u"\"");
        }
        return data;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 1; }
    String getGameName() override { return u"BackToTheCode"; }

    void handlePlayerOutput(int, int round, int playerIdx, const Lines& outputs) override {
        if (outputs.empty()) arrayIndexOutOfBounds(0, 0); // (never: one line expected)
        const String& output = outputs[0];
        NString message;
        Player& p = player(playerIdx);
        String xs, ys, rounds;
        if (matchPosition(output, xs, ys, message)) {
            i32 x, y;
            java::parseInt(xs, x); // 1 to 9 digits: never fails
            if (x < 0 || x >= GRID_WIDTH) {
                p.setLost(translate("InvalidInput", {Arg(u"0 <= x < " + S(GRID_WIDTH)), Arg(u"x = " + S(x))}));
                throw legacy::InvalidInputException(u"0 <= x < " + S(GRID_WIDTH), u"x = " + S(x));
            }
            java::parseInt(ys, y);
            if (y < 0 || y >= GRID_HEIGHT) {
                p.setLost(translate("InvalidInput", {Arg(u"0 <= y < " + S(GRID_HEIGHT)), Arg(u"y = " + S(y))}));
                throw legacy::InvalidInputException(u"0 <= y < " + S(GRID_HEIGHT), u"y = " + S(y));
            }
            moveTo(p, cellAt(x, y));
            if (p.moved()) lastActionRound = round;
        } else {
            p.dontMove();
            if (matchBack(output, rounds, message)) {
                i32 backRounds;
                java::parseInt(rounds, backRounds);
                if (backRounds <= 0 || backRounds > BACK_IN_TIME_MAX) {
                    String expected = u"0 < backRounds <= " + S(BACK_IN_TIME_MAX), found = u"backRounds = " + S(backRounds);
                    p.setLost(translate("InvalidInput", {Arg(expected), Arg(found)}));
                    throw legacy::InvalidInputException(expected, found);
                }
                if (!p.canBackInTime()) {
                    p.setLost(translate("noBackInTimeLeft"));
                    throw legacy::LostException(std::string("noBackInTimeLeft"));
                }
                lastActionRound = round;
                addToolTip(playerIdx, u"Back in time (" + S(backRounds) + u" rounds)");
                p.backInTimeCount = backRounds;
                backInTimeAmount = std::min(backInTimeAmount + backRounds, inGameRound - 1);
            } else {
                String expected = u"\"<x> <y>\" or \"BACK <amount>\"";
                p.setLost(translate("InvalidInput", {Arg(expected), Arg(output)}));
                throw legacy::InvalidInputException(expected, output);
            }
        }
        p.setMessage(message);
    }

    // Player#moveTo(c): one step along x, else along y, else stay
    void moveTo(Player& p, Cell* c) {
        Cell* currentCell = p.positions.back(); // positions.peek()
        if (currentCell->position.x != c->position.x) {
            p.positions.push_back(cellAt(currentCell->position.x + java::signum(c->position.x - currentCell->position.x), currentCell->position.y));
        } else if (currentCell->position.y != c->position.y) {
            p.positions.push_back(cellAt(currentCell->position.x, currentCell->position.y + java::signum(c->position.y - currentCell->position.y)));
        } else {
            p.positions.push_back(currentCell);
        }
    }

    // checkZoneFill: the cells this player's move closes, in the order found (Java: a HashSet<Cell>)
    std::vector<Cell*> checkZoneFill(Player& p) {
        std::vector<Cell*> playerCells;
        Cell* lastCell = p.getLastPosition();
        bool zoneClosed = false;
        for (int i = inGameRound - 2; i >= 0 && !zoneClosed; --i) {
            Cell* roundPos = p.getPositionAtRound(i);
            if (roundPos->manhattanDistance(*lastCell) == 1) zoneClosed = true;
        }
        if (zoneClosed) {
            std::vector<Cell*> zone;
            forNeighbours(lastCell, [&](Cell* cell) {
                if (cell->owner == nullptr && tryToFillZone(cell, &p, zone)) {
                    for (Cell* a : zone) a->setOwner(&p, inGameRound);
                    // (the zones found are disjoint: the cells of a filled one have an owner now)
                    playerCells.insert(playerCells.end(), zone.begin(), zone.end());
                }
                return true;
            });
        }
        return playerCells;
    }

    // tryToFillZone: the free cells 8-connected to fromCell (a Stack, the HashSet `zone`), or null (false) when the search
    // pops a border cell or meets a cell of another player
    bool tryToFillZone(Cell* fromCell, Player* p, std::vector<Cell*>& zone) {
        zone.clear();
        ++zoneStamp;
        std::vector<Cell*>& stack = zoneStack;
        stack.clear();
        stack.push_back(fromCell);
        while (!stack.empty()) {
            Cell* top = stack.back();
            stack.pop_back();
            if (zoneMark[(size_t)index(top)] != zoneStamp) {
                zoneMark[(size_t)index(top)] = zoneStamp;
                zone.push_back(top);
            }
            if (top->position.x == 0 || top->position.y == 0 || top->position.x == GRID_WIDTH - 1 || top->position.y == GRID_HEIGHT - 1) return false;
            bool foreign = false;
            forNeighbours(top, [&](Cell* cell) {
                if (cell->owner == nullptr) {
                    if (zoneMark[(size_t)index(cell)] != zoneStamp) stack.push_back(cell);
                } else if (cell->owner != p) {
                    foreign = true;
                    return false;
                }
                return true;
            });
            if (foreign) return false;
        }
        return true;
    }

    Lines getInitInputForPlayer(int) override { return {S((i64)players.size() - 1)}; }

    Lines getInputForPlayer(int, int playerIdx) override {
        Lines data;
        data.push_back(S(inGameRound));
        int n = (int)players.size();
        std::vector<int> mapper((size_t)n);
        for (int i = 0; i < n; ++i) mapper[(size_t)i] = i;
        mapper[(size_t)playerIdx] = 0;
        mapper[0] = playerIdx;
        for (int i = 0; i < n; ++i) {
            Player& p = player(mapper[(size_t)i]);
            if (p.lost) {
                data.push_back(u"-1 -1 0");
            } else {
                const Point& pos = p.getPositionAtRound(inGameRound - 1)->position;
                data.push_back(S(pos.x) + u" " + S(pos.y) + u" " + S(p.backInTimeLeft));
            }
        }
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            String str;
            for (int x = 0; x < GRID_WIDTH; ++x) {
                const Cell* c = cellAt(x, y);
                str += c->owner == nullptr ? String(u".") : S(mapper[(size_t)c->owner->id]);
            }
            data.push_back(str);
        }
        return data;
    }

    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 1; }
    bool showTooltips() override { return true; }

    void checkOwnership() {
        // HashMap<Cell, List<Player>>: the players on each free cell (the entries own different cells: their order,
        // identity hashes in Java, is not observable)
        std::vector<std::pair<Cell*, std::vector<Player*>>> ownership;
        for (size_t i = 0; i < players.size(); i++) {
            Player& p = player((int)i);
            if (!p.lost) {
                Cell* current = p.getLastPosition();
                if (current->owner == nullptr) {
                    std::vector<Player*>* list = nullptr;
                    for (auto& e : ownership)
                        if (e.first == current) list = &e.second;
                    if (!list) {
                        ownership.push_back({current, {}});
                        list = &ownership.back().second;
                    }
                    list->push_back(&p);
                }
            }
        }
        for (auto& e : ownership)
            if (e.second.size() == 1) e.second[0]->own(e.first, inGameRound);
        for (size_t i = 0; i < players.size(); i++) {
            Player& p = player((int)i);
            if (!p.lost) p.own(checkZoneFill(p));
            else p.own(std::vector<Cell*>());
        }
    }

    void prepare(int) override {
        inGameRound++;
        if (backInTimeAmount > 0) backInTimeAmount = 0;
        for (size_t i = 0; i < players.size(); i++) player((int)i).backInTimeCount = 0;
    }

    void updateGame(int round) override {
        checkOwnership();
        if (backInTimeAmount > 0) {
            additionnalRoundCount += backInTimeAmount + 1;
            inGameRound -= backInTimeAmount + 1;
            for (size_t i = 0; i < players.size(); i++) player((int)i).backInTime(inGameRound);
            reality++;
        }
        if (endOfGameDetected) throw legacy::GameOverException(std::string("EndOfGameDetected"));
        if (lastActionRound < round - MAX_INACTIVE_ROUND_COUNT) throw legacy::GameOverException(std::string("inactivityDetected"));
        if (checkAnticipatedGameOver()) endOfGameDetected = true;
    }

    Lines getPlayerActions(int playerIdx, int round) override {
        Player& p = player(playerIdx);
        if (!p.lost && round >= 0) {
            Cell* cell = p.getPositionAtRound(inGameRound);
            if (p.backInTimeCount > 0) return {translate("playerBackInTime", {playerIdx, p.backInTimeCount})};
            if (p.moved()) return {translate("playerMove", {playerIdx, cell->position.x, cell->position.y})};
            return {translate("playerDoNothing", {playerIdx, cell->position.x, cell->position.y})};
        }
        return {};
    }

    // checkAnticipatedGameOver: whether the ranking can no longer change, every BACK left used as far as it goes
    bool checkAnticipatedGameOver() {
        int n = (int)players.size();
        int maxBackInTime = 0;
        for (int i = 0; i < n; i++)
            if (!player(i).lost) maxBackInTime += player(i).backInTimeLeft * BACK_IN_TIME_MAX;
        if (maxBackInTime <= inGameRound) {
            int minReachableRound = inGameRound - maxBackInTime;
            int availableCellCountAtMinReachableRound = GRID_WIDTH * GRID_HEIGHT;
            for (int i = 0; i < n; i++) availableCellCountAtMinReachableRound -= player(i).getOwnedCellCountAtRound(minReachableRound);
            struct Potential {
                int min, max;
                int size() const { return max - min; }
                bool isEmpty() const { return size() <= 0; }
            };
            std::vector<Potential> playerPotentials;
            for (int i = 0; i < n; ++i) {
                int ownedCellCount = player(i).getOwnedCellCountAtRound(minReachableRound);
                int min = ownedCellCount, max;
                if (!player(i).lost) max = ownedCellCount + availableCellCountAtMinReachableRound;
                else max = player(i).getOwnedCellCountAtRound(inGameRound);
                playerPotentials.push_back(Potential{min, max});
            }
            // Arrays.sort with the referee's Comparator (TimSort); the counts are small: no overflow
            java::sort(playerPotentials, [](const Potential& o1, const Potential& o2) {
                int diff = o2.max - o1.max;
                if (diff == 0) diff = o2.min - o1.min;
                return diff;
            });
            bool finished = true;
            for (int i = 1; i < n && finished; ++i) {
                const Potential& a = playerPotentials[(size_t)i];
                const Potential& b = playerPotentials[(size_t)i - 1];
                if ((!a.isEmpty() || !b.isEmpty()) && a.max >= b.min) finished = false;
            }
            return finished;
        }
        return false;
    }

    bool isPlayerDead(int playerIdx) override { return player(playerIdx).lost; }
    NString getDeathReason(int playerIdx) override { return player(playerIdx).lostReason; }
    int getScore(int playerIdx) override { return player(playerIdx).getLastOwnedCellCount(); }

    Lines getGameSummary(int) override {
        Lines summary;
        if (backInTimeAmount > 0) {
            summary.push_back(translate("gameGoBackInTime", {inGameRound + backInTimeAmount + 1, backInTimeAmount, inGameRound + 1}));
        } else if (inGameRound > 0) {
            summary.push_back(translate("gameGoForward", {inGameRound}));
        }
        for (int playerId = 0; playerId < (int)players.size(); playerId++)
            summary.push_back(translate("playerScore", {playerId, player(playerId).getOwnedCellCountAtRound(inGameRound)}));
        return summary;
    }

    void setPlayerTimeout(int, int, int playerIdx) override { player(playerIdx).setLost(u"playerTimeoutMulti"); }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
