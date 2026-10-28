// Smash The Code (CodinGame, 2016-04/05, "smash-the-code") - headless C++ referee, port of the CodinGame referee in
// https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD), Referees/SmashTheCode/Referee.java:
// the single-file referee `class Referee extends MultiReferee` with CodinGame's old AbstractReferee base of 2016 embedded
// (common/legacy.h, legacy::Base::smashTheCode2016(): Hypersonic's base but for getMillisTimeForRound 150, which the game
// overrides, and no gameOver() hook: nextRound() tests the alive count itself; its SoloReferee class is unused).
// The only version of the game in the sources; it is the one CodinGame runs today: CodinGame's replays (codingame.com's
// public gameResult/findByGameId: 717 games of 120 Gold and Legend players) are reproduced frame for frame (views, infos,
// summaries, tooltips, scores) by the jar's classes with the seed alone (their referee input) and by this port through
// its CLI: 717/717.
// 2 players (getMinimumPlayerCount() 2): each player's input holds the grid of players[1 - playerIdx], so with 3 or 4
// players the input of player 2 indexes -1 and the Java referee crashes at the first round (no scores, "null"), as
// this port does.
// Deterministic: reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): seed=N) to stdout like the jar
// (referees/cg-builder/legacy: the referee runs unchanged in the runner of the SDK 4.5.0). "null" scores when the game
// fails (the Java referee crashed or the protocol broke; the jar after 150 s). -l writes the trace of common/legacy.h
// (bot executions, view sizes, summaries, tooltips, END; CG_TRACE_VIEWS=1 adds the text of every view). Time limits: the
// game's 1000 ms for the first turn, 100 ms after; 200 rounds.
//
// League: none read from the init properties; the static constants ROTATION_ALLOWED (true) and BLOCK_PAIR_IDENTICAL
// (false) are the top league's. CodinGame's replays of each league are reproduced by this file with BLOCK_PAIR_IDENTICAL
// true and ROTATION_ALLOWED false (Wood 2), BLOCK_PAIR_IDENTICAL true (Wood 1), and as is (Bronze, Silver, Gold, Legend).
// Only the top league is implemented here (PLAYER_OUTPUT_PATTERN1, the pattern without rotation, is not).
//
// The game: each player has its own 6 x 12 grid. java.util.Random(seed) draws the pairs of blocks (two colours of 1-5,
// nextInt(5) + 1 each): 8 at the start, one more each round; both players get the same pairs, and see the next 8. Each
// round every player answers "<column> [<rotation>] [<message>]": the pair's first block goes in the column, the
// second to its right (rotation 0), above it (1), to its left (2) or below it (3, the default); a column or rotation
// leaving the grid is an invalid input, a block finding no space in its column kills the player. Groups of 4+ blocks
// of one colour (4-neighbours) disappear, with the skulls (0, nuisance blocks) next to them; the blocks above fall and
// may make more groups: a chain. Score of each step of a chain: (10 x blocks cleared) x clamp(chain power (0, 8, 16,
// 32... up to 999) + colour bonus (2^(colours - 1)) + group bonus (size - 4, 8 from 11), 1, 999); every 70 points
// add a skull to the opponent's nuisance (a Rational), whose whole lines of 6 drop on its grid at the start of the
// next round (lines beyond the grid's space are lost). The game ends when a player is out (no space, invalid input,
// timeout: the game ends at the next round, so the other plays its turn of that round) or after 200 rounds; the score
// is 0 for a dead player, else max(1, points).
//
// The Java referee is followed closely, notably: the regex of a line (Matcher#matches with Java's backtracking: a
// position of 1-8 digits, an optional rotation that only counts when a valid tail follows it ("3 12" is column 3,
// rotation 3, message "12"), \s and \d ASCII), Math.round(Math.cos / Math.sin(-rotation * PI / 2)) (java::cos / sin,
// HotSpot's intrinsics), the order in which a vertical pair is stacked (rotation 3 drops the second block first), the
// Rational class's int arithmetic (gcd, lcm, the unreduced sums of plus(), Java's / and %), the nuisance of each chain
// step summed for the view and of the whole turn for the opponent, the int overflow semantics of the score formula
// (4 << counter), the depth-first search of the groups, the HashSet<Point> iteration orders of the groups and of their
// broken skulls (java::HashMapOrder with java.awt.Point#hashCode: two pairs of grid points share a hash code, no bin
// is ever treeified, a bin of 9 resizes the table early), which only show in the view, the view's frame data ("%f",
// Double.toString) and the message cut to 40 UTF-16 units (a lone high surrogate written '?').
// Not reproduced / not modelled: the wall clock (see engine.h); the order of the tooltips of a frame (the base's HashSet
// of identity hash codes: legacy.h, only their count matters to the runner).
//
// Validation (referees/README.md): referee-diff against the jar with deterministic bots (every ordered pair, seed bases
// 1, 1000001, 987654321, 5000000000), direct runs comparing the jar's game log with this port's trace (views, summaries,
// tooltips; raw bytes and line endings), the jar's own compiled classes driven by reflection over random states (grids,
// chains, nuisance, commands, inputs, views), CodinGame's replays.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/legacy.h"

#include <optional>
#include <vector>

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// --- constants ---
const int WIDTH = 6, HEIGHT = 12; // GRID_SIZE (a java.awt.Dimension)
const int AHEAD_BLOCK = 8;
const int BLOCK_TYPE_COUNT = 5;
const int MINIMUM_ZONE_SIZE = 4;
const int DEFAULT_ROTATION = 3;
const bool ROTATION_ALLOWED = true;
const bool BLOCK_PAIR_IDENTICAL = false;
const int NUISANCE_FACTOR = 70;
const int MAX_MESSAGE_LENGTH = 40;
const double PI = 3.141592653589793; // Math.PI
const int NUL = -1;                  // a null Integer of the grid (the colours are 1-5, a nuisance block 0)

String S(i64 v) { return java::str(v); }

// --- Referee.Rational: int arithmetic (overflows wrap, / and % as Java's) ---
struct Rational {
    i32 num = 0; // the numerator
    i32 den = 1; // the denominator

    static i32 gcd(i32 m, i32 n) {
        if (m < 0) m = java::neg(m);
        if (n < 0) n = java::neg(n);
        if (0 == n) return m;
        return gcd(n, java::rem(m, n));
    }
    static i32 lcm(i32 m, i32 n) {
        if (m < 0) m = java::neg(m);
        if (n < 0) n = java::neg(n);
        return java::mul(m, java::div(n, gcd(m, n)));
    }

    Rational() {}
    Rational(i32 numerator, i32 denominator) {
        i32 g = gcd(numerator, denominator);
        num = java::div(numerator, g);
        den = java::div(denominator, g);
        if (den < 0) {
            den = java::neg(den);
            num = java::neg(num);
        }
    }

    double toDouble() const { return (double)num / den; }
    i32 floor() const { return java::div(num, den); }

    Rational times(const Rational& b) const {
        const Rational& a = *this;
        Rational c(a.num, b.den);
        Rational d(b.num, a.den);
        return Rational(java::mul(c.num, d.num), java::mul(c.den, d.den));
    }
    Rational plus(const Rational& b) const { return plus(b.num, b.den); }
    Rational plus(i32 v) const { return plus(v, 1); }
    Rational plus(i32 n, i32 d) const {
        const Rational& a = *this;
        if (a.num == 0) return Rational(n, d);
        if (n == 0) return a;
        i32 f = gcd(a.num, n);
        i32 g = gcd(a.den, d);
        Rational s(java::add(java::mul(java::div(a.num, f), java::div(d, g)), java::mul(java::div(n, f), java::div(a.den, g))), lcm(a.den, d));
        s.num = java::mul(s.num, f);
        return s;
    }
    Rational negate() const { return Rational(java::neg(num), den); }
    Rational minus(const Rational& b) const { return plus(b.negate()); }
    Rational minus(i32 i) const { return plus(java::neg(i)); }
    Rational reciprocal() const { return Rational(den, num); }
    Rational divides(const Rational& b) const { return times(b.reciprocal()); }
};

// --- java.awt.Point and a HashSet<Point> (the iteration order of java.util.HashSet with Point's hashCode) ---
struct Point {
    int x = 0, y = 0;
};

struct PointSet {
    java::HashMapOrder order; // keys x * HEIGHT + y (the points stay in the grid)
    bool add(Point p) {
        int before = order.size;
        order.put(p.x * HEIGHT + p.y, java::pointHashCode(p.x, p.y));
        return order.size != before;
    }
    void addAll(const std::vector<Point>& list) {
        for (Point p : list) add(p);
    }
    int size() const { return order.size; }
    bool isEmpty() const { return order.size == 0; }
    std::vector<Point> list() {
        std::vector<i32> keys;
        order.keys(keys);
        std::vector<Point> r;
        r.reserve(keys.size());
        for (i32 k : keys) r.push_back(Point{k / HEIGHT, k % HEIGHT});
        return r;
    }
};

String pointText(Point p) { return S(p.x) + u" " + S(p.y); }

struct PlacedBlockResult {
    Point position;
    int type = 0;
    String toString() const { return S(position.x) + u" " + S(position.y) + u" " + S(type); } // "%d %d %d"
};

struct Grid;

struct Combination {
    int color = 0;
    PointSet destroyedNuisance;
    PointSet points;
    explicit Combination(int color) : color(color) {}
    void apply(Grid& grid);
    bool isValid() const { return points.size() >= MINIMUM_ZONE_SIZE; }
    String toString() {
        String combination;
        bool first = true;
        for (Point point : points.list()) {
            if (!first) combination += u" ";
            first = false;
            combination += pointText(point);
        }
        if (!destroyedNuisance.isEmpty()) {
            String destroyed;
            first = true;
            for (Point point : destroyedNuisance.list()) {
                if (!first) destroyed += u" ";
                first = false;
                destroyed += pointText(point);
            }
            return combination + u"|" + destroyed;
        }
        return combination;
    }
};

struct Combo {
    std::vector<Combination> combinations;
    i32 points = 0;
    std::optional<Rational> nuisance; // null until updateGame's computePoints
    String toString() {
        String s;
        bool first = true;
        for (Combination& combination : combinations) {
            if (!first) s += u",";
            first = false;
            s += combination.toString();
        }
        if (!nuisance) throw java::Exception{"java.lang.NullPointerException"}; // (never: the view only shows updated combos)
        return s + u"/" + S(points) + u"/" + java::ascii(java::doubleToString(nuisance->toDouble()));
    }
};

struct Grid {
    int grid[WIDTH][HEIGHT]; // Integer[width][height]: column x, row y from the bottom; NUL = null

    Grid() {
        for (auto& column : grid)
            for (int& c : column) c = NUL;
    }

    std::optional<PlacedBlockResult> addBlock(int position, int type) {
        int* column = grid[position];
        int i;
        for (i = 0; i < HEIGHT && column[i] != NUL; i++) {
        }
        if (i >= HEIGHT) return std::nullopt;
        column[i] = type;
        return PlacedBlockResult{Point{position, i}, type};
    }

    std::vector<Point> breakNuisanceAround(Point p) {
        std::vector<Point> points;
        if (p.x > 0 && grid[p.x - 1][p.y] != NUL && grid[p.x - 1][p.y] == 0) {
            grid[p.x - 1][p.y] = NUL;
            points.push_back(Point{p.x - 1, p.y});
        }
        if (p.y > 0 && grid[p.x][p.y - 1] != NUL && grid[p.x][p.y - 1] == 0) {
            grid[p.x][p.y - 1] = NUL;
            points.push_back(Point{p.x, p.y - 1});
        }
        if (p.x < WIDTH - 1 && grid[p.x + 1][p.y] != NUL && grid[p.x + 1][p.y] == 0) {
            grid[p.x + 1][p.y] = NUL;
            points.push_back(Point{p.x + 1, p.y});
        }
        if (p.y < HEIGHT - 1 && grid[p.x][p.y + 1] != NUL && grid[p.x][p.y + 1] == 0) {
            grid[p.x][p.y + 1] = NUL;
            points.push_back(Point{p.x, p.y + 1});
        }
        return points;
    }

    void gravity() {
        for (int* column : grid) {
            for (int i = 0; i < HEIGHT; ++i) {
                int newPos = i - 1;
                while (newPos >= 0 && column[newPos] == NUL) newPos--;
                newPos++;
                if (newPos != i) {
                    column[newPos] = column[i];
                    column[i] = NUL;
                }
            }
        }
    }

    // alreadyChecked: a HashSet<Point> only added to and looked up (no iteration): a table here. cellType == currentCellType
    // compares Integer objects: the cached Integer.valueOf of the same small value, so the values.
    void checkCombinations(bool (&alreadyChecked)[WIDTH][HEIGHT], Combination& combination, Point cell, int cellType) {
        int currentCellType = grid[cell.x][cell.y];
        if (cellType == currentCellType) {
            combination.points.add(cell);
            if (!alreadyChecked[cell.x][cell.y]) {
                alreadyChecked[cell.x][cell.y] = true;
                if (cell.x > 0) checkCombinations(alreadyChecked, combination, Point{cell.x - 1, cell.y}, cellType);
                if (cell.x < WIDTH - 1) checkCombinations(alreadyChecked, combination, Point{cell.x + 1, cell.y}, cellType);
                if (cell.y > 0) checkCombinations(alreadyChecked, combination, Point{cell.x, cell.y - 1}, cellType);
                if (cell.y < HEIGHT - 1) checkCombinations(alreadyChecked, combination, Point{cell.x, cell.y + 1}, cellType);
            }
        }
    }

    Combo checkCombinations() {
        Combo combo;
        bool alreadyChecked[WIDTH][HEIGHT] = {};
        for (int i = 0; i < HEIGHT; ++i) {
            for (int j = 0; j < WIDTH; ++j) {
                int cellType = grid[j][i];
                if (cellType != NUL && cellType > 0) {
                    Point cell{j, i};
                    if (!alreadyChecked[j][i]) {
                        Combination combi(cellType);
                        checkCombinations(alreadyChecked, combi, cell, cellType);
                        if (combi.isValid()) {
                            combi.apply(*this);
                            combo.combinations.push_back(std::move(combi));
                        }
                    }
                }
            }
        }
        gravity();
        return combo;
    }

    // one row of the input / view, from the top (row HEIGHT - 1 - i)
    String row(int i) const {
        String s;
        for (int j = 0; j < WIDTH; ++j) {
            int cell = grid[j][HEIGHT - i - 1];
            s += cell == NUL ? String(u".") : S(cell);
        }
        return s;
    }
};

void Combination::apply(Grid& grid) {
    for (Point p : points.list()) {
        grid.grid[p.x][p.y] = NUL;
        destroyedNuisance.addAll(grid.breakNuisanceAround(p));
    }
}

enum DeathReason { NONE = -1, TIMEOUT, INVALID_INPUT, NO_MORE_SPACE };
const char16_t* DEATH_NAME[] = {u"TIMEOUT", u"INVALID_INPUT", u"NO_MORE_SPACE"}; // name()
const char16_t* DEATH_LABEL[] = {u"timeout", u"InvalidInput", u"noMoreSpace"};  // deathReasonLabel

struct PlayerAction {
    int position = 0;
    int rotation = 0;
};

struct BlockPair {
    int block1 = 0, block2 = 0;
};

struct Player {
    std::optional<PlayerAction> nextAction;
    Grid grid;
    std::vector<Combo> zonesCombo; // a Stack
    std::vector<PlacedBlockResult> newBlocks;
    std::vector<Point> newNuisanceBlocks;
    i32 points = 0;
    Rational nuisanceR = Rational(0, 1);
    Rational nuisanceAfterDrop = nuisanceR;
    NString message;
    int deathReason = NONE;

    bool isDead() const { return deathReason != NONE; }

    void reset() {
        nextAction.reset();
        zonesCombo.clear();
        newBlocks.clear();
        newNuisanceBlocks.clear();
        nuisanceAfterDrop = Rational(0, 1);
        message.reset();
    }

    void harm() {
        i32 nuisanceLines = nuisanceR.divides(Rational(WIDTH, 1)).floor();
        for (i32 line = 0; line < nuisanceLines; ++line) {
            for (int column = 0; column < WIDTH; ++column) {
                std::optional<PlacedBlockResult> result = grid.addBlock(column, 0);
                if (result) newNuisanceBlocks.push_back(result->position);
            }
        }
        nuisanceR = nuisanceR.minus(java::mul(nuisanceLines, WIDTH));
        nuisanceAfterDrop = nuisanceR;
    }

    PlacedBlockResult addBlock(int position, int type) {
        std::optional<PlacedBlockResult> result = grid.addBlock(position, type);
        if (!result) {
            deathReason = NO_MORE_SPACE;
            throw legacy::LostException(std::string("noMoreSpace"));
        }
        return *result;
    }

    int doAction(const BlockPair& pair) {
        if (nextAction) {
            const PlayerAction a = *nextAction;
            // (int) Math.round(Math.cos(-nextAction.rotation * Math.PI / 2)): an int negation, then doubles
            double angle = (double)java::neg(a.rotation) * PI / 2;
            int secondBlockXShift = java::l2i(java::round(java::cos(angle)));
            int secondBlockYShift = java::l2i(java::round(java::sin(angle)));
            int secondBlockX = java::add(secondBlockXShift, a.position);

            if (a.position < 0 || a.position >= WIDTH || secondBlockX < 0 || secondBlockX >= WIDTH) {
                deathReason = INVALID_INPUT;
                throw legacy::InvalidInputException(S(std::max(0, -secondBlockXShift)) + u" <= position < " + S(WIDTH + std::min(0, -secondBlockXShift)),
                                                    u"position = " + S(a.position));
            }

            if (secondBlockYShift > 0) {
                newBlocks.push_back(addBlock(secondBlockX, pair.block2));
                newBlocks.push_back(addBlock(a.position, pair.block1));
            } else {
                newBlocks.push_back(addBlock(a.position, pair.block1));
                newBlocks.push_back(addBlock(secondBlockX, pair.block2));
            }
        }

        for (;;) {
            Combo combo = grid.checkCombinations();
            if (combo.combinations.empty()) break;
            zonesCombo.push_back(std::move(combo));
        }
        return (int)zonesCombo.size();
    }

    i32 getScore() const { return isDead() ? 0 : points; }
};

// The player's line, PLAYER_OUTPUT_PATTERN2 (ROTATION_ALLOWED) with Matcher#matches():
//   (?<position>-?\d{1,8})(?:\s+(?<rotation>[0123]))?\s*(?:\s+(?<message>.+))?
// \s and \d are ASCII (java::isRegexSpace / isRegexDigit); '.' never meets a line terminator (the runner and the
// base's Scanner split the lines there). Java's backtracking gives:
//   - position: an optional '-' and the whole digit run, 1 to 8 digits (a 9th digit: no match);
//   - then (A) a whitespace run, one of 0-3 and a tail T, else (B) no rotation and the tail T of everything after
//     the position;
//   - T: only whitespace (no message), or a whitespace run then the message: all the rest (from its first
//     non-whitespace char, trailing whitespace included).
// So "3 12" is position 3, no rotation (the default 3), message "12"; "3 1 x" position 3, rotation 1, message "x".
bool matchTail(const String& line, size_t i, NString& message) {
    size_t a = i;
    while (i < line.size() && java::isRegexSpace(line[i])) i++;
    if (i == line.size()) {
        message.reset();
        return true;
    }
    if (i == a) return false; // \s+ needs one
    for (size_t k = i; k < line.size(); k++)
        if (java::isLineSeparator(line[k])) return false; // (never: see above)
    message = line.substr(i);
    return true;
}

bool matchOutput(const String& line, String& position, NString& rotation, NString& message) {
    size_t i = 0;
    if (i < line.size() && line[i] == u'-') i++;
    size_t d = i;
    while (i < line.size() && java::isRegexDigit(line[i])) i++;
    if (i == d || i - d > 8) return false;
    position = line.substr(0, i);
    // (A) \s+[0123] then the tail
    size_t k = i;
    while (k < line.size() && java::isRegexSpace(line[k])) k++;
    if (k > i && k < line.size() && line[k] >= u'0' && line[k] <= u'3' && matchTail(line, k + 1, message)) {
        rotation = line.substr(k, 1);
        return true;
    }
    // (B) no rotation
    rotation.reset();
    return matchTail(line, i, message);
}

struct Referee : legacy::MultiReferee {
    std::vector<BlockPair> blocks; // a LinkedList
    i64 seed = 0;
    java::Random random;
    std::vector<Player> players;

    Referee() { base = legacy::Base::smashTheCode2016(); }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        blocks.clear();
        // Long.parseLong(prop.getProperty("seed", String.valueOf(new Random().nextLong())))
        if (const String* s = prop.find(u"seed")) {
            i64 v;
            if (!java::parseLong(*s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(*s))};
            seed = v;
        } else {
            java::Random r((i64)java::entropy());
            seed = r.nextLong();
        }
        random = java::Random(seed);

        players.assign(playerCount, Player());
        generateNextBlocks();
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        return prop;
    }

    Lines getInitInputForPlayer(int) override { return {}; }

    void generateNextBlocks() {
        while (blocks.size() < (size_t)AHEAD_BLOCK) {
            if (BLOCK_PAIR_IDENTICAL) {
                int color = random.nextInt(BLOCK_TYPE_COUNT) + 1;
                blocks.push_back(BlockPair{color, color});
            } else {
                int block1 = random.nextInt(BLOCK_TYPE_COUNT) + 1;
                int block2 = random.nextInt(BLOCK_TYPE_COUNT) + 1;
                blocks.push_back(BlockPair{block1, block2});
            }
        }
    }

    void prepare(int round) override {
        if (round > 0 && !blocks.empty()) blocks.erase(blocks.begin());
        generateNextBlocks();
        for (Player& p : players) {
            p.reset();
            p.harm();
        }
    }

    Lines getInputForPlayer(int, int playerIdx) override {
        Lines out;
        for (int i = AHEAD_BLOCK; i > 0; --i) {
            const BlockPair& pair = blocks[blocks.size() - i];
            out.push_back(S(pair.block1) + u" " + S(pair.block2));
        }
        const Player& p = players[playerIdx];
        out.push_back(S(p.getScore()));
        for (int i = 0; i < HEIGHT; ++i) out.push_back(p.grid.row(i));
        // this.players[1 - playerIdx]: with 3 or 4 players, player 2's input indexes -1 (the referee crashes)
        int other = 1 - playerIdx;
        if (other < 0 || other >= (int)players.size()) throw java::Exception{java::arrayIndexOutOfBoundsException(other, (i64)players.size())};
        const Player& p2 = players[other];
        out.push_back(S(p2.getScore()));
        for (int i = 0; i < HEIGHT; ++i) out.push_back(p2.grid.row(i));
        return out;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 1; }

    void handlePlayerOutput(int, int, int playerIdx, const Lines& outputs) override {
        Player& player = players[playerIdx];
        String position;
        NString rotationGroup, messageGroup;
        if (matchOutput(outputs[0], position, rotationGroup, messageGroup)) {
            i32 pos;
            java::parseInt(position, pos); // (-?\d{1,8}: always an int)
            int rotation = DEFAULT_ROTATION;
            if (ROTATION_ALLOWED && rotationGroup) rotation = (*rotationGroup)[0] - u'0';

            player.nextAction = PlayerAction{pos, rotation};
            int comboCount = player.doAction(blocks[0]);
            if (comboCount > 1) addToolTip(playerIdx, translate("playerCombo", {comboCount}));
            player.message = messageGroup;
            if (player.message && player.message->size() > (size_t)MAX_MESSAGE_LENGTH) player.message = player.message->substr(0, MAX_MESSAGE_LENGTH);
        } else {
            player.deathReason = INVALID_INPUT;
            player.nextAction.reset();
            throw legacy::InvalidInputException(ROTATION_ALLOWED ? u"position rotation" : u"position", outputs[0]);
        }
    }

    static i32 computePoints(const Combo& combo, int counter) {
        // https://puyonexus.net/wiki/Scoring#Color_Bonus: score = (10 * PC) * (CP + CB + GB)
        i32 pc = 0; // PC: the blocks cleared
        for (const Combination& combi : combo.combinations) pc = java::add(pc, combi.points.size());
        // CP: the chain power (4 << counter: Java's shift takes counter & 31, and wraps)
        i32 cp = std::min(counter <= 0 ? 0 : (i32)((u32)4 << (counter & 31)), 999);
        // CB: the colour bonus (a HashSet<Integer>: only its size)
        unsigned colors = 0;
        for (const Combination& combi : combo.combinations) colors |= 1u << combi.color;
        int colorCount = __builtin_popcount(colors);
        i32 cb = colorCount <= 1 ? 0 : (1 << (colorCount - 1));
        // GB: the group bonus
        i32 gb = 0;
        for (const Combination& combi : combo.combinations) {
            i32 lgb = combi.points.size() - 4;
            if (combi.points.size() >= 11) lgb = 8;
            gb = java::add(gb, lgb);
        }
        return java::mul(java::mul(10, pc), std::max(1, std::min(999, java::add(java::add(cp, cb), gb))));
    }

    i32 computePoints(Player& p) {
        Player* other = nullptr;
        for (Player& p2 : players)
            if (&p != &p2) other = &p2;
        if (other == nullptr) throw java::Exception{"java.lang.NullPointerException"}; // (never: 2 players)
        Rational otherNuisance = other->nuisanceR;

        i32 pointsTotal = 0;
        for (size_t i = 0; i < p.zonesCombo.size(); ++i) {
            Combo& combo = p.zonesCombo[i];
            i32 points = computePoints(combo, (int)i);
            pointsTotal = java::add(pointsTotal, points);
            p.points = java::add(p.points, points);
            otherNuisance = otherNuisance.plus(Rational(points, NUISANCE_FACTOR));
            combo.points = p.points;
            combo.nuisance = otherNuisance;
        }
        Rational nuisance(pointsTotal, NUISANCE_FACTOR);
        other->nuisanceR = other->nuisanceR.plus(nuisance);
        return pointsTotal;
    }

    void updateGame(int) override {
        for (Player& p : players) computePoints(p);
    }

    void populateMessages(std::map<std::string, String>& p) override {
        p["noMoreSpace"] = u"No more space!";
        p["playerCombo"] = u"Chain of %d!";
    }

    Lines getInitDataForView() override { return {S(WIDTH) + u" " + S(HEIGHT) + u" " + S(AHEAD_BLOCK)}; }

    Lines getFrameDataForView(int, int, int, bool) override {
        Lines output;
        for (Player& p : players) {
            // String.format("%d %f %f %s", ...) (the default locale's '.': en_US)
            output.push_back(S(p.getScore()) + u" " + java::ascii(java::formatFixed(p.nuisanceR.toDouble(), 6)) + u" " +
                             java::ascii(java::formatFixed(p.nuisanceAfterDrop.toDouble(), 6)) + u" " + (p.isDead() ? String(DEATH_NAME[p.deathReason]) : String(u"-")));
            if (p.newNuisanceBlocks.empty()) {
                output.push_back(u"-");
            } else {
                String s;
                for (size_t i = 0; i < p.newNuisanceBlocks.size(); i++) s += (i ? u" " : u"") + pointText(p.newNuisanceBlocks[i]);
                output.push_back(s);
            }
            if (p.newBlocks.empty()) {
                output.push_back(u"-");
            } else {
                String s;
                for (size_t i = 0; i < p.newBlocks.size(); i++) s += (i ? u" " : u"") + p.newBlocks[i].toString();
                output.push_back(s);
            }
            if (!p.zonesCombo.empty()) {
                String s;
                for (size_t i = 0; i < p.zonesCombo.size(); i++) s += (i ? u";" : u"") + p.zonesCombo[i].toString();
                output.push_back(s);
            } else {
                output.push_back(u"-");
            }
            if (p.message) output.push_back(u"\"" + *p.message + u"\"");
            else output.push_back(u"\"\"");
        }
        String nextBlocks;
        for (size_t i = 0; i < blocks.size(); i++) nextBlocks += (i ? u"," : u"") + S(blocks[i].block1) + u" " + S(blocks[i].block2);
        output.push_back(nextBlocks);
        return output;
    }

    String getGameName() override { return u"SmashTheCode"; }
    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }
    Lines getPlayerActions(int, int) override { return {}; }
    bool isPlayerDead(int playerIdx) override { return players[playerIdx].deathReason != NONE; }
    NString getDeathReason(int playerIdx) override { return String(DEATH_LABEL[players[playerIdx].deathReason]); }
    int getScore(int playerIdx) override {
        const Player& player = players[playerIdx];
        if (player.isDead()) return 0;
        return std::max(1, player.getScore());
    }
    Lines getGameSummary(int) override { return {}; }
    void setPlayerTimeout(int, int, int playerIdx) override { players[playerIdx].deathReason = TIMEOUT; }
    int getMaxRoundCount(int) override { return 200; }
    int getMillisTimeForRound() override { return 100; }
    int getMillisTimeForFirstRound() override { return 1000; }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
