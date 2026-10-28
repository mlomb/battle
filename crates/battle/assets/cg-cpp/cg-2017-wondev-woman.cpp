// Wondev Woman (CodinGame, 2017-06, "wondev-woman") - headless C++ referee, port of the CodinGame referee published in
// https://github.com/CodinGame/WondevWoman (commit 7b2ac87, 2017-06-23, = HEAD, "included the referee code"; the
// README: "The code used to play matches in the WondevWoman contest"; the repository has only that commit and its
// initial commit, so this is the contest's referee, also the multiplayer game's as published; CodinGame's live game
// has at least one later fix, below). The file is the game's class only
// (`class Referee extends MultiReferee`), without imports nor base. 2 players. Deterministic: reproduces the same game
// as the reference jar for a given -seed.
//
// The jar (referees/cg-builder/legacy: `legacy/build.sh ... cg-2017-wondev-woman imports/java-2017.imports
// imports/java-2017-static.imports bases/base-2017-wondev-woman.java GAME_VERSION=3`): the game's class with import
// blocks prepended (the game calls Stream.of / Collectors.joining unqualified; its own nested Point shadows the
// java.awt.Point import), its GAME_VERSION set to 3 (below), compiled with CodinGame's old base of 2017 (the Coders of
// the Caribbean one from https://github.com/SpiritusSancti5/codinGame, 2017-04) changed as this game needs: the
// gameIsOver() hook the game overrides (nextRound, and every turn of a turn based game), a loss that no longer ends a
// turn based game at once, every turn of a turn based game a new round for the view / summary / prepare(), the
// "<code>Tooltip" message of a lost player. CodinGame's own base of 2017-06 is not public: these changes are inferred
// from CodinGame's own replays of the game (codingame.com's public gameResult/findByGameId; see the base file's
// header). Checked on 2563 replays (Legend to Bronze, 2017 and today's; every player's recorded output fed back):
// the jar's referee gives the same views, infos, summaries, tooltips and scores in 2562; this port, through its CLI
// with bots printing the recorded lines, in 2183 of the 2184 whose map the seed alone gives (the others were played
// with CodinGame's symmetric=true or a fixed mapIndex, which the CLI does not pass). The one difference is a change of
// CodinGame's live referee, not in the repository: its "PlaceTooHigh" loss reads "Cannot build at height 4 at (0, 2)."
// with the tooltip "$1 lost: Invalid build (0,2)" (game 910879129), where the published referee has "Cannot build at
// height %d." with the move target's height and fails (below). This port follows the published referee. This port
// runs on common/legacy.h with legacy::Base::wondevWoman2017().
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): "seed=N", "mapIndex=M")
// to stdout like the jar (the referee runs unchanged in the runner of the SDK 4.5.0). "null" scores when the game
// fails (the Java referee crashed or the protocol broke; the jar after 150 s). -l writes the trace of common/legacy.h
// (bot executions, view sizes, summaries, tooltips, END).
//
// League: the referee's `public static int GAME_VERSION = 0` is the Wood 3 rules and nothing assigns it (initReferee
// reads only seed, mapIndex and symmetric from the properties); 1 = Wood 2 (points instead of winning at height 3), 2
// = Wood 1 (2 units, PUSH&BUILD), 3 = Bronze and above (fog of war: an opponent's unit is seen only next to one of
// yours). CodinGame's multiplayer game plays 3 in its top leagues (the view's init data of its Legend replays says 3;
// kevinsandow's brutaltester referee sets 3 too): the jar is built with GAME_VERSION=3 (legacy/build.sh's static field
// override) and this port's GAME_VERSION is 3; the lower rules are kept as code (the Wood 3 / Wood 1 replays are
// reproduced with 0 / 2), never run.
//
// The game: a square grid of 5-7 cells (one of three maps drawn from the seed: a 5x5 square, a diamond of size 7, or
// a generated left-right symmetric map on a 6x6 grid: cell pairs drawn until it has 25 + nextInt(10) cells or more in
// one 8-connected piece, 1000 draws at most); each player has 2 units on random
// cells (java.util.Random(seed): the sorted cells shuffled). Turn based: the players play one after the other, one
// line each: MOVE&BUILD <unit> <dir> <dir> (move to a neighbouring cell at most one level higher, not a hole, below
// height 4, not onto a unit; then build +1 on a neighbouring cell of the new position, not a hole nor height 4 nor a
// unit; moving onto height 3 scores a point), PUSH&BUILD <unit> <dir> <dir> (push an adjacent enemy unit one cell
// further in the same or a 45-degree direction, onto a cell at most one level above its own, then build on the cell
// it left), ACCEPT-DEFEAT; each with an optional message. A move or push onto, or a build on, a cell holding a unit
// the player does not see is not a loss: the build (or the whole push) is cancelled. Each turn's input lists every
// legal action. An invalid or illegal line (12 kinds of LostException, InvalidCommand for a PUSH&BUILD not in upper
// case, a unit index out of range: the referee's catch (Exception)), ACCEPT-DEFEAT or a timeout puts the player out
// (the game's dead flag; the base's lost status); the other player plays on alone while it does not have more points
// (gameIsOver); the game ends when it does, when both are out, or after 200 rounds (400 turns). The score is the
// points.
//
// The Java referee is followed closely, notably: the two regexes hand-matched (CASE_INSENSITIVE without UNICODE_CASE:
// ASCII folding; \s and \d ASCII; the alternations of 1- and 2-letter directions; the message is what follows the
// whitespace after the last direction, cut to 46 UTF-16 chars + "..." beyond 48), MOVE&BUILD compared ignoring case
// but PUSH&BUILD with equals, the order of the checks of a move and of a push (the reason's values: e.g.
// "PlaceTooHigh" with the height of the move's target), the visibility (Chebyshev distance 1 from any of the player's
// units, before the move), the legal actions computed with the same checks and sorted as strings, the units of an out
// player staying on the grid, the summaries (MoveValid / PlaceValid / CancelledPlace / Scores, PushToPlaceOn /
// CancelledPush) and view lines of every turn. The generated map: the referee's HashSet<String> of cells and
// HashMap<Point, Integer> grid are only counted (the islands) or sorted (the start cells): their iteration orders never
// show. The base's tooltips HashSet: at most one tooltip per frame here (one player acts per frame).
// Odd Java behaviour reproduced: a "PlaceTooHigh" loss (a build on a height 4 cell) makes the base's "LostTooltip"
// format "PlaceTooHighTooltip" ("Invalid build (%d,%d)") with that reason's single value: a
// MissingFormatArgumentException ends the referee thread, the game fails ("null" scores) like the jar's. CodinGame's
// live referee does not (it has a fixed "PlaceTooHigh", above).
// Not reproduced / not modelled: the wall clock (see engine.h). The time limits are the base's (1000 ms for the first
// turn, 150 ms after: the game does not override getMillisTimeForRound, unlike Coders of the Caribbean, Ghost in the
// Cell, Code4Life and Mean Max which set 50 ms; the statement says 50 ms). The view is built as text (a few hundred
// bytes per frame, far below the runner's limits). No floating point.
#include "common/legacy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <set>

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// --- constants ---
// The league: Referee.GAME_VERSION (public static int, 0 in the repository = Wood 3; nothing assigns it); the jar is
// built with GAME_VERSION=3 (legacy/build.sh's override), the rules of Bronze and above.
const int GAME_VERSION = 3;

const int GOT_PUSHED = 2;
const int DID_PUSH = 1;
const int NO_PUSH = 0;
const int FINAL_HEIGHT = 4;
const int VIEW_DISTANCE = 1;
const int GENERATED_MAP_SIZE = 6;

String S(i64 v) { return java::str(v); }

struct Point {
    int x = 0, y = 0;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
    bool operator<(const Point& o) const { return x != o.x ? x < o.x : y < o.y; }
    int distance(const Point& other) const { return std::max(std::abs(x - other.x), std::abs(y - other.y)); }
};

// enum Direction { NW, N, NE, W, E, SW, S, SE } (values() order)
const int DIRECTION_COUNT = 8;
const char16_t* DIRECTION_NAME[] = {u"NW", u"N", u"NE", u"W", u"E", u"SW", u"S", u"SE"};

bool contains(const String& s, char16_t c) { return s.find(c) != String::npos; }

// getNeighbor(direction, position[, size]): the direction is a String (a Direction's name, or the player's uppercased
// text, one of the same eight)
Point getNeighbor(const String& direction, const Point& position) {
    int x = position.x;
    int y = position.y;
    if (contains(direction, u'E')) {
        x++;
    } else if (contains(direction, u'W')) {
        x--;
    }
    if (contains(direction, u'S')) {
        y++;
    } else if (contains(direction, u'N')) {
        y--;
    }
    return {x, y};
}

// Grid: a HashMap<Point, Integer> of the playable cells (Point has hashCode / equals: only get / put, and keySet()
// either sorted or for an order-free count); get() of any other point is null (-1 here)
struct Grid {
    static const int OFF = 2, DIM = 12; // the points looked up stay within -1..8
    int level[DIM][DIM];
    int size = 0;
    Grid() {
        for (auto& r : level)
            for (int& v : r) v = -1;
    }
    int get(int x, int y) const {
        if (x < -OFF || y < -OFF || x >= DIM - OFF || y >= DIM - OFF) return -1;
        return level[x + OFF][y + OFF];
    }
    int get(const Point& p) const { return get(p.x, p.y); }
    void create(const Point& point) {
        level[point.x + OFF][point.y + OFF] = 0;
        int necessarySize = std::max(point.x, point.y) + 1;
        if (necessarySize > size) size = necessarySize;
    }
    void place(const Point& placeAt) { level[placeAt.x + OFF][placeAt.y + OFF]++; }
    std::vector<Point> keys() const {
        std::vector<Point> r;
        for (int x = 0; x < DIM; x++)
            for (int y = 0; y < DIM; y++)
                if (level[x][y] >= 0) r.push_back({x - OFF, y - OFF});
        return r;
    }
};

struct Player;

enum ActionType { MOVE, PUSH };
const char16_t* ACTION_NAME[] = {u"MOVE&BUILD", u"PUSH&BUILD"};

struct Unit;

struct ActionResult {
    Point moveTarget, placeTarget;
    bool placeValid = false, moveValid = false, scorePoint = false;
    ActionType type;
    Unit* unit = nullptr;
    explicit ActionResult(ActionType t) : type(t) {}
};

struct Unit {
    int index;
    Player* player;
    Point position;
    bool gotPushed = false;
    std::optional<ActionResult> did;

    Unit(Player* p, int i) : index(i), player(p) {}
    void reset() {
        gotPushed = false;
        did.reset();
    }
    bool pushed() const { return did && did->type == PUSH; }
    bool moved() const { return did && did->type == MOVE; }
};

struct Action {
    int index;
    int move, place; // Direction
    ActionType command;
    String toPlayerString() const {
        return String(ACTION_NAME[command]) + u" " + S(index) + u" " + DIRECTION_NAME[move] + u" " + DIRECTION_NAME[place];
    }
};

struct Player {
    int index, score = 0;
    bool dead = false, won = false;
    std::vector<Unit*> units;
    NString message;

    explicit Player(int i) : index(i) {}
    void win() { won = true; }
    void die(int) { dead = true; }
    void reset() {
        message.reset();
        for (Unit* u : units) u->reset();
    }
    void setMessage(const NString& m) {
        message = m;
        if (m && m->size() > 48) message = m->substr(0, 46) + u"...";
    }
};

// ----------------------------------------------------------------------------
// The referee's regexes (Pattern.CASE_INSENSITIVE without UNICODE_CASE: ASCII folding only; \s = [ \t\n\x0B\f\r],
// \d = [0-9]), matched on the whole line (Matcher#matches). The lines never hold a line terminator (the base's
// Scanner split them there), so '.' matches every char.
// ----------------------------------------------------------------------------

// the literal (case insensitive) text `word` at s[i..]
bool hasWordAt(const String& s, size_t i, const char* word) {
    size_t w = strlen(word);
    if (i > s.size() || s.size() - i < w) return false;
    for (size_t k = 0; k < w; k++)
        if (!java::regexCharMatchesIgnoreCase(s[i + k], word[k])) return false;
    return true;
}

size_t skipSpaces(const String& s, size_t i) {
    while (i < s.size() && java::isRegexSpace(s[i])) i++;
    return i;
}

// (?:\s+)?(?:\s+(?<message>.+))?$ on s[i..]: the end, or a whitespace run then the end (no message), or a
// whitespace run then the message = all the rest (the greedy runs give back one whitespace char to the message's
// \s+; '.+' takes the rest); false when s[i] is not whitespace
bool matchTail(const String& s, size_t i, NString& message) {
    message.reset();
    if (i == s.size()) return true;
    size_t j = skipSpaces(s, i);
    if (j == i) return false;
    if (j < s.size()) message = s.substr(j);
    return true;
}

// ^ACCEPT-DEFEAT(?:\s+)?(?:\s+(?<message>.+))?
bool matchAcceptDefeat(const String& s, NString& message) { return hasWordAt(s, 0, "ACCEPT-DEFEAT") && matchTail(s, 13, message); }

// (?<dir>N|S|W|E|NW|NE|SW|SE) at s[i..]: the 1- or 2-letter direction (case insensitive) followed by what `next`
// accepts (one choice at most can be followed by whitespace or the end: a 2-letter one has a letter where a 1-letter
// one needs whitespace); -1 when none
template <class Next>
int matchDirection(const String& s, size_t i, size_t& end, Next next) {
    // alternation order: N, S, W, E, NW, NE, SW, SE
    static const char* ALTS[] = {"N", "S", "W", "E", "NW", "NE", "SW", "SE"};
    static const int DIR[] = {1, 6, 3, 4, 0, 2, 5, 7};
    for (int a = 0; a < 8; a++) {
        if (!hasWordAt(s, i, ALTS[a])) continue;
        size_t e = i + strlen(ALTS[a]);
        if (next(e)) {
            end = e;
            return DIR[a];
        }
    }
    return -1;
}

// ^(?<action>MOVE\&BUILD|PUSH\&BUILD)\s+(?<index>\d)\s+(?<move>N|S|W|E|NW|NE|SW|SE)\s+(?<place>N|S|W|E|NW|NE|SW|SE)
// (?:\s+)?(?:\s+(?<message>.+))?
struct PlayerMatch {
    String action;
    int index = 0;
    int move = 0, place = 0;
    NString message;
};
bool matchPlayer(const String& s, PlayerMatch& m) {
    if (!hasWordAt(s, 0, "MOVE&BUILD") && !hasWordAt(s, 0, "PUSH&BUILD")) return false;
    m.action = s.substr(0, 10);
    size_t j = skipSpaces(s, 10);
    if (j == 10 || j >= s.size() || !java::isRegexDigit(s[j])) return false;
    m.index = s[j] - u'0';
    size_t k = skipSpaces(s, j + 1);
    if (k == j + 1) return false;
    size_t moveEnd = 0;
    // the move direction is followed by \s+ (greedy: the whole run, then the place direction)
    size_t placeStart = 0;
    m.move = matchDirection(s, k, moveEnd, [&](size_t e) {
        size_t p = skipSpaces(s, e);
        if (p == e) return false;
        placeStart = p;
        return true;
    });
    if (m.move < 0) return false;
    size_t placeEnd = 0;
    NString message;
    m.place = matchDirection(s, placeStart, placeEnd, [&](size_t e) { return matchTail(s, e, message); });
    if (m.place < 0) return false;
    m.message = message;
    return true;
}

struct Referee : legacy::MultiReferee {
    // the rules (static fields set by initReferee from GAME_VERSION)
    bool WIN_ON_MAX_HEIGHT = true;
    bool FOG_OF_WAR = false;
    bool CAN_PUSH = false;
    int UNITS_PER_PLAYER = 1;

    bool symmetric = false;
    i64 seed = 0;
    java::Random random;
    Grid grid;
    std::vector<Player*> players;
    std::vector<Unit*> units;
    int mapIndex = -1;
    String expected;

    Referee() { base = legacy::Base::wondevWoman2017(); }

    bool isTurnBasedGame() override { return true; }

    // gameIsOver() (@Override of the base's hook: legacy.h names it gameIsOverHook)
    bool gameIsOverHook() override {
        if (WIN_ON_MAX_HEIGHT) {
            if (legacy::MultiReferee::gameIsOverHook()) return true;
            for (Player* p : players)
                if (p->won || p->dead) return true;
            return false;
        }

        bool oneDead = players[0]->dead;
        bool twoDead = players[1]->dead;
        if (oneDead && twoDead) {
            return true;
        } else if (oneDead && !twoDead) {
            return players[1]->score > players[0]->score;
        } else if (!oneDead && twoDead) {
            return players[0]->score > players[1]->score;
        } else {
            return false;
        }
    }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        // new Random(System.currentTimeMillis()).nextLong(): evaluated, unused (the CLI always gives the seed)
        i64 now = (i64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        String seedText = prop.getProperty(u"seed", S(java::Random(now).nextLong()));
        String mapIndexText = prop.getProperty(u"mapIndex", u"-1");
        String symmetricText = prop.getProperty(u"symmetric", u"false");

        if (GAME_VERSION >= 1) WIN_ON_MAX_HEIGHT = false;
        if (GAME_VERSION >= 2) {
            UNITS_PER_PLAYER = 2;
            CAN_PUSH = true;
        }
        if (GAME_VERSION >= 3) FOG_OF_WAR = true;

        expected = u"MOVE&BUILD";
        if (CAN_PUSH) expected += u" | PUSH&BUILD";
        expected += u" <index> <direction> <direction>";

        // Integer.valueOf, a NumberFormatException -> -1
        i32 mi;
        this->mapIndex = java::parseInt(mapIndexText, mi) ? mi : -1;
        // Long.valueOf, a NumberFormatException -> a time based seed
        this->seed = java::Random(now).nextLong();
        i64 s;
        if (java::parseLong(seedText, s)) this->seed = s;
        // Boolean.valueOf: "true" ignoring case
        this->symmetric = symmetricText.size() == 4 && java::regexMatchesIgnoreCase(symmetricText, "true");

        random = java::Random(this->seed);
        grid = initGrid();
        players.clear();
        units.clear();
        for (int idx = 0; idx < playerCount; ++idx) {
            Player* player = new Player(idx);
            for (int i = 0; i < UNITS_PER_PLAYER; ++i) {
                Unit* u = new Unit(player, i);
                player->units.push_back(u);
                units.push_back(u);
            }
            players.push_back(player);
        }

        // new LinkedList<>(grid.map.keySet()), sorted by x then y (the keys are distinct: the order of the HashMap
        // does not matter), then Collections.shuffle (a LinkedList of 5+ elements: through an array, the same swaps)
        std::vector<Point> points = grid.keys();
        std::sort(points.begin(), points.end());
        java::shuffle(points, random);
        std::deque<Point> queue(points.begin(), points.end());

        if (this->symmetric) {
            Player* one = players[0];
            Player* two = players[1];
            for (Unit* u : one->units) {
                bool okay = false;
                while (!okay) {
                    if (queue.empty()) throw java::Exception{"java.lang.NullPointerException"}; // poll() of an empty queue: null
                    Point a = queue.front();
                    queue.pop_front();
                    Point b{grid.size - 1 - a.x, a.y};
                    auto it = std::find(queue.begin(), queue.end(), b);
                    bool removed = it != queue.end();
                    if (removed) {
                        queue.erase(it);
                        okay = true;
                        u->position = a;
                        two->units[u->index]->position = b;
                    }
                }
            }
        } else {
            for (Unit* u : units) {
                if (queue.empty()) throw java::Exception{"java.lang.NullPointerException"};
                u->position = queue.front();
                queue.pop_front();
            }
        }
    }

    Grid initGrid() {
        Grid grid;
        std::vector<String> maps;
        maps.push_back(u"0 0;1 0;2 0;3 0;4 0;0 1;1 1;2 1;3 1;4 1;0 2;1 2;2 2;3 2;4 2;0 3;1 3;2 3;3 3;4 3;0 4;1 4;2 4;3 4;4 4"); // Square
        maps.push_back(u"3 0;2 1;3 1;4 1;1 2;2 2;3 2;4 2;5 2;0 3;1 3;2 3;3 3;4 3;5 3;6 3;1 4;2 4;3 4;4 4;5 4;2 5;3 5;4 5;3 6"); // Diamond
        std::vector<Point> generated = generateRandomMap();

        int randomMapIndex = random.nextInt(3);
        if (mapIndex < 0 || mapIndex >= 3) mapIndex = randomMapIndex;
        if (mapIndex == 2) {
            // the generated map's "x y;x y..." (a HashSet's order: the same cells whatever it is)
            for (const Point& p : generated) grid.create(p);
        } else {
            for (const String& coord : java::split(maps[mapIndex], u';')) {
                std::vector<String> xy = java::split(coord, u' ');
                i32 x, y;
                java::parseInt(xy[0], x);
                java::parseInt(xy[1], y);
                grid.create({x, y});
            }
        }
        return grid;
    }

    std::vector<Point> generateRandomMap() {
        std::set<Point> coords; // HashSet<String> of "x y": only its size and its cells count
        int size = GENERATED_MAP_SIZE;
        int iterations = 0;
        int cells = 25 + random.nextInt(10);
        int islands = 0;
        Grid g;
        while (((int)coords.size() < cells || islands > 1) && iterations < 1000) {
            int x = random.nextInt(size);
            int y = random.nextInt(size);
            Point point{x, y};
            Point mirror{size - 1 - x, y};

            coords.insert(point);
            coords.insert(mirror);
            g.create(point);
            g.create(mirror);

            islands = countIslands(g);
            iterations++;
        }
        return std::vector<Point>(coords.begin(), coords.end());
    }

    // the number of 8-connected groups of cells (the referee's BFS over the HashMap's keys: the count does not depend
    // on their order)
    static int countIslands(const Grid& grid) {
        std::vector<Point> keys = grid.keys();
        std::set<Point> computed;
        int total = 0;
        for (const Point& p : keys) {
            if (computed.count(p)) continue;
            total++;
            std::deque<Point> fifo{p};
            computed.insert(p);
            while (!fifo.empty()) {
                Point e = fifo.front();
                fifo.pop_front();
                for (int d = 0; d < DIRECTION_COUNT; d++) {
                    Point n = getNeighbor(DIRECTION_NAME[d], e);
                    if (!computed.count(n) && grid.get(n) >= 0) {
                        computed.insert(n);
                        fifo.push_back(n);
                    }
                }
            }
        }
        return total;
    }

    java::Properties getConfiguration() override {
        java::Properties p;
        p.setProperty(u"seed", S(seed));
        p.setProperty(u"mapIndex", S(mapIndex));
        if (symmetric) p.setProperty(u"symmetric", u"true");
        return p;
    }

    // --- AbstractReferee ---
    Lines getInitInputForPlayer(int) override {
        Lines lines;
        lines.push_back(S(grid.size));
        lines.push_back(S(UNITS_PER_PLAYER));
        return lines;
    }

    String gridRow(int y) const {
        String row;
        for (int x = 0; x < grid.size; ++x) {
            int height = grid.get(x, y);
            if (height < 0) row += u".";
            else row += S(height);
        }
        return row;
    }

    Lines getInputForPlayer(int, int playerIdx) override {
        Lines lines;
        Player* self = players[playerIdx];
        Player* other = players[(playerIdx + 1) % 2];

        for (int y = 0; y < grid.size; ++y) lines.push_back(gridRow(y));

        for (Player* p : {self, other}) {
            for (Unit* u : p->units) {
                if (p == other && !unitVisibleToPlayer(u, self)) lines.push_back(u"-1 -1");
                else lines.push_back(S(u->position.x) + u" " + S(u->position.y));
            }
        }

        std::vector<Action> legalActions = getLegalActions(self);
        lines.push_back(S((i64)legalActions.size()));
        for (const Action& action : legalActions) lines.push_back(action.toPlayerString());
        return lines;
    }

    bool unitVisibleToPlayer(const Unit* unit, const Player* player) const {
        if (!FOG_OF_WAR) return true;
        for (const Unit* u : player->units)
            if (u->position.distance(unit->position) <= VIEW_DISTANCE) return true;
        return false;
    }

    Unit* getUnitOnPoint(const Point& target) const {
        for (Unit* u : units)
            if (u->position == target) return u;
        return nullptr;
    }

    ActionResult computeMove(Unit* unit, const String& dir1, const String& dir2) const {
        Point target = getNeighbor(dir1, unit->position);
        int targetHeight = grid.get(target);
        if (targetHeight < 0) throw legacy::LostException(std::string("BadCoords"), {target.x, target.y});
        int currentHeight = grid.get(unit->position);
        if (targetHeight > currentHeight + 1) throw legacy::LostException(std::string("InvalidMove"), {currentHeight, targetHeight});
        if (targetHeight >= FINAL_HEIGHT) throw legacy::LostException(std::string("MoveTooHigh"), {target.x, target.y});
        if (getUnitOnPoint(target) != nullptr) throw legacy::LostException(std::string("MoveOnUnit"), {target.x, target.y});

        Point placeTarget = getNeighbor(dir2, target);
        int placeTargetHeight = grid.get(placeTarget);
        if (placeTargetHeight < 0) throw legacy::LostException(std::string("InvalidPlace"), {placeTarget.x, placeTarget.y});
        if (placeTargetHeight >= FINAL_HEIGHT) throw legacy::LostException(std::string("PlaceTooHigh"), {targetHeight});

        ActionResult result(MOVE);
        result.moveTarget = target;
        result.placeTarget = placeTarget;

        // getUnitOnPoint(placeTarget).filter(u -> !u.equals(unit)): the first unit there, unless it is this one
        Unit* possibleUnit = getUnitOnPoint(placeTarget);
        if (possibleUnit == unit) possibleUnit = nullptr;
        if (possibleUnit == nullptr) {
            result.placeValid = true;
            result.moveValid = true;
        } else if (FOG_OF_WAR && !unitVisibleToPlayer(possibleUnit, unit->player)) {
            result.placeValid = false;
            result.moveValid = true;
        } else {
            throw legacy::LostException(std::string("PlaceOnUnit"), {placeTarget.x, placeTarget.y});
        }

        if (targetHeight == FINAL_HEIGHT - 1) result.scorePoint = true;
        result.unit = unit;
        return result;
    }

    ActionResult computePush(Unit* unit, const String& dir1, const String& dir2) const {
        if (!validPushDirection(dir1, dir2)) throw legacy::LostException(std::string("PushInvalid"), {dir1, dir2});
        Point target = getNeighbor(dir1, unit->position);
        Unit* pushed = getUnitOnPoint(target);
        if (pushed == nullptr) throw legacy::LostException(std::string("PushVoid"), {target.x, target.y});

        if (pushed->player == unit->player) throw legacy::LostException(std::string("FriendlyFire"), {unit->index, pushed->index});

        Point pushTo = getNeighbor(dir2, pushed->position);
        int toHeight = grid.get(pushTo);
        int fromHeight = grid.get(target);

        if (toHeight < 0 || toHeight >= FINAL_HEIGHT || toHeight > fromHeight + 1) throw legacy::LostException(std::string("PushInvalid"), {dir1, dir2});

        ActionResult result(PUSH);
        result.moveTarget = pushTo;
        result.placeTarget = target;

        Unit* possibleUnit = getUnitOnPoint(pushTo);
        if (possibleUnit == nullptr) {
            result.placeValid = true;
            result.moveValid = true;
        } else if (FOG_OF_WAR && !unitVisibleToPlayer(possibleUnit, unit->player)) {
            result.placeValid = false;
            result.moveValid = false;
        } else {
            throw legacy::LostException(std::string("PushOnUnit"), {dir1, dir2});
        }

        result.unit = pushed;
        return result;
    }

    // command: the text of the action group (MOVE: equalsIgnoreCase, PUSH: equals)
    ActionResult computeAction(const String& command, Unit* unit, const String& dir1, const String& dir2) const {
        if (command.size() == 10 && java::regexMatchesIgnoreCase(command, "MOVE&BUILD")) {
            return computeMove(unit, dir1, dir2);
        } else if (CAN_PUSH && command == u"PUSH&BUILD") {
            return computePush(unit, dir1, dir2);
        } else {
            throw legacy::LostException(std::string("InvalidCommand"), {command});
        }
    }

    std::vector<Action> getLegalActions(Player* player) const {
        std::vector<Action> actions;
        for (Unit* unit : player->units) {
            for (int dir1 = 0; dir1 < DIRECTION_COUNT; dir1++) {
                for (int dir2 = 0; dir2 < DIRECTION_COUNT; dir2++) {
                    try {
                        computeAction(ACTION_NAME[MOVE], unit, DIRECTION_NAME[dir1], DIRECTION_NAME[dir2]);
                        actions.push_back({unit->index, dir1, dir2, MOVE});
                    } catch (const legacy::LostException&) {
                    }
                    if (CAN_PUSH) {
                        try {
                            computeAction(ACTION_NAME[PUSH], unit, DIRECTION_NAME[dir1], DIRECTION_NAME[dir2]);
                            actions.push_back({unit->index, dir1, dir2, PUSH});
                        } catch (const legacy::LostException&) {
                        }
                    }
                }
            }
        }
        // sort by toPlayerString (distinct strings: a stable sort gives the plain order)
        std::sort(actions.begin(), actions.end(), [](const Action& a, const Action& b) { return a.toPlayerString() < b.toPlayerString(); });
        return actions;
    }

    void prepare(int) override {
        for (Player* p : players) p->reset();
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 1; }

    void handlePlayerOutput(int, int round, int playerIdx, const Lines& outputs) override {
        const String& line = outputs[0];
        Player* player = players[playerIdx];

        try {
            NString message;
            if (matchAcceptDefeat(line, message)) {
                player->die(round);
                // Message
                player->setMessage(message);
                throw legacy::LostException(std::string("selfDestruct"), {player->index});
            }
            PlayerMatch match;
            if (matchPlayer(line, match)) {
                String dir1 = DIRECTION_NAME[match.move]; // match.group("move").toUpperCase()
                String dir2 = DIRECTION_NAME[match.place];
                int index = match.index;
                if (index >= (int)player->units.size()) {
                    // player.units.get(index): an IndexOutOfBoundsException, caught by catch (Exception e) (stack
                    // trace to stderr), the player dies, an invalid input
                    player->die(round);
                    throw legacy::InvalidInputException(expected, line);
                }
                Unit* unit = player->units[index];

                ActionResult ar = computeAction(match.action, unit, dir1, dir2);
                unit->did = ar;
                if (ar.moveValid) ar.unit->position = ar.moveTarget;
                if (ar.placeValid) grid.place(ar.placeTarget);
                if (ar.scorePoint) player->score++;
                if (ar.type == PUSH) ar.unit->gotPushed = true;

                // Message
                player->setMessage(match.message);
                return;
            }

            throw legacy::InvalidInputException(expected, line);

        } catch (const legacy::LostException&) {
            player->die(round);
            throw;
        } catch (const legacy::InvalidInputException&) {
            player->die(round);
            throw;
        }
    }

    static bool validPushDirection(const String& target, const String& push) {
        if (target.size() == 2) return push == target || push == target.substr(0, 1) || push == target.substr(1, 1);
        return push.find(target) != String::npos;
    }

    void updateGame(int) override {
        for (Unit* unit : units)
            if (WIN_ON_MAX_HEIGHT && grid.get(unit->position) == FINAL_HEIGHT - 1) unit->player->win();
    }

    void populateMessages(std::map<std::string, String>& p) override {
        // Error messages
        p["PlaceOnUnit"] = u"Trying to build on a unit at position (%d,%d).";
        p["PushVoid"] = u"Nobody to push at position (%d, %d).";
        p["PlaceTooHigh"] = u"Cannot build at height %d.";
        p["InvalidMove"] = u"Cannot move from height %d to %d.";
        p["MoveOnUnit"] = u"Trying to move onto an occupied cell at (%d,%d).";
        p["BadCoords"] = u"Cannot move to position (%d,%d).";
        p["MoveTooHigh"] = u"Cannot move to position (%d,%d).";
        p["InvalidPlace"] = u"Cannot build on position (%d,%d).";
        p["PushInvalid"] = u"Not a valid push: %s + %s.";
        p["FriendlyFire"] = u"Unit %d is tried to push friendly unit %d.";
        p["PushOnUnit"] = u"Trying to push onto another unit: %s + %s.";
        p["selfDestruct"] = u"$%d accepts defeat!";

        // Tooltip error messages
        p["PlaceOnUnitTooltip"] = u"Invalid build (%d,%d)";
        p["PushVoidTooltip"] = u"Invalid push (%d, %d)";
        p["PlaceTooHighTooltip"] = u"Invalid build (%d,%d)";
        p["InvalidMoveTooltip"] = u"Invalid move (%d to %d)";
        p["MoveOnUnitTooltip"] = u"Invalid move (%d,%d)";
        p["BadCoordsTooltip"] = u"Invalid move (%d,%d)";
        p["MoveTooHighTooltip"] = u"Invalid move (%d,%d)";
        p["InvalidPlaceTooltip"] = u"Invalid build (%d,%d)";
        p["PushInvalidTooltip"] = u"Invalid push (%s + %s)";
        p["FriendlyFireTooltip"] = u"Friendly fire!";
        p["PushOnUnitTooltip"] = u"Invalid push (%s + %s)";
        p["selfDestructTooltip"] = u"accepted defeat!";

        // Status messages
        p["MoveValid"] = u"$%d moved unit %d to (%d,%d).";
        p["PlaceValid"] = u"...and builds on (%d,%d).";
        p["CancelledPlace"] = u"...and cannot build on (%d,%d)!";
        p["PushToPlaceOn"] = u"$%d made unit %d push a unit to (%d,%d) and builds on (%d,%d).";
        p["CancelledPush"] = u"$%d attempted to make unit %d push the unit on (%d, %d), but could not.";
        p["Wins"] = u"...and wins the game!";
        p["Scores"] = u"...and scores a point!";
    }

    Lines getInitDataForView() override {
        Lines lines;
        lines.push_back(S(grid.size));
        lines.push_back(S(GAME_VERSION));
        lines.push_back(S(UNITS_PER_PLAYER));
        lines.insert(lines.begin(), S((i64)lines.size() + 1));
        return lines;
    }

    Lines getFrameDataForView(int, int, int, bool) override {
        Lines lines;
        for (Unit* u : units) {
            int pushCode = NO_PUSH;
            if (u->pushed()) {
                pushCode = DID_PUSH;
            } else if (u->gotPushed) {
                pushCode = GOT_PUSHED;
            }
            lines.push_back(S(u->position.x) + u" " + S(u->position.y) + u" " + S(pushCode));
        }
        for (int y = 0; y < grid.size; ++y) lines.push_back(gridRow(y));
        for (Player* p : players) lines.push_back(S(getScore(p->index)) + u" " + (p->dead ? u"0" : u"1") + u";" + (p->message ? *p->message : String()));
        lines.push_back(S(whoJustPlayed()));
        return lines;
    }

    int whoJustPlayed() const {
        for (Unit* u : units)
            if (u->did) return u->player->index;
        return players[0]->index;
    }

    String getGameName() override { return u"WondevWoman"; }
    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }
    Lines getPlayerActions(int, int) override { return {}; }

    bool isPlayerDead(int playerIdx) override {
        if (GAME_VERSION == 0) return players[playerIdx]->dead;
        return false;
    }

    NString getDeathReason(int playerIdx) override { return u"$" + S(playerIdx) + u": Eliminated!"; }

    int getScore(int playerIdx) override {
        Player* p = players[playerIdx];
        if (WIN_ON_MAX_HEIGHT) {
            if (p->dead) return -1;
            if (p->won) return 1;
            return 0;
        }
        return p->score;
    }

    Lines getGameSummary(int) override {
        Lines lines;
        for (Unit* u : units) {
            if (u->moved()) {
                // (a MOVE always has moveValid: the "impossible" error is never printed)
                if (u->did->moveValid) lines.push_back(translate("MoveValid", {u->player->index, u->index, u->did->moveTarget.x, u->did->moveTarget.y}));
                if (u->did->placeValid) {
                    lines.push_back(translate("PlaceValid", {u->did->placeTarget.x, u->did->placeTarget.y}));
                } else {
                    lines.push_back(translate("CancelledPlace", {u->did->placeTarget.x, u->did->placeTarget.y}));
                }
                if (u->did->scorePoint) {
                    if (WIN_ON_MAX_HEIGHT) {
                        lines.push_back(translate("Wins"));
                    } else {
                        lines.push_back(translate("Scores"));
                    }
                }
            } else if (u->pushed()) {
                if (u->did->moveValid && u->did->placeValid) {
                    lines.push_back(translate("PushToPlaceOn", {u->player->index, u->index, u->did->moveTarget.x, u->did->moveTarget.y, u->did->placeTarget.x,
                                                                u->did->placeTarget.y}));
                } else if (!u->did->moveValid && !u->did->placeValid) {
                    lines.push_back(translate("CancelledPush", {u->player->index, u->index, u->did->placeTarget.x, u->did->placeTarget.y}));
                }
                // (else the "also impossible" error to stderr: never)
            }
        }
        return lines;
    }

    void setPlayerTimeout(int, int round, int playerIdx) override { players[playerIdx]->die(round); }
    int getMaxRoundCount(int) override { return 200; }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
