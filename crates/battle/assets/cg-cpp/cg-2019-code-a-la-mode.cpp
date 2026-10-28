// Code a la Mode (CodinGame, 2019-03) - headless C++ referee, port of
// https://github.com/csj/code-a-la-mode (commit 5fbd06c, 2020-08-07, pom artifactId code-a-la-mode;
// the game is written in Kotlin 1.3.11), CodinGame game engine 3.4.5, 3 players, league "All" (the
// highest, level 4+: the jar's CommandLineInterface sets 19): every crate, the chopping board, the oven,
// croissants and tarts. Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -p3 "<bot3 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints "<p0score>\n<p1score>\n<p2score>\nseed=<N>\n" to stdout like the jar, or "null" per player
// (exit code 0) when the Java referee would have failed (see below). Exit code 1 without output when
// the jar's command line interface fails (bad -seed, a bot that cannot be started).
// -l writes the trace of common/engine.h (one line per bot execution, game summary and tooltip block,
// then "END OK|<why the game failed>"; the view frames are not built, see below).
//
// The game: three rounds of 200 turns (one execution of one player per turn); each round two of the
// three players cook together (seats 0+1, then 2+0, then 1+2) and both get the round's score (the sum
// of the awards of the customers they served); a player's score is the sum of its two rounds, -1 once
// it has been deactivated (invalid command, an exception of the game, a timeout).
//
// Engine 3.4.5 (common/engine.h, cg::Options::engine313): its core is 3.8.2's except that the turn
// numbers given to the referee start at 0 (here gameTurn's turn - 1) and that only RuntimeExceptions
// fail the game; its runner is 3.8.2's (= 3.13.0's) except for timing (a 300 ms sleep after launching
// the bots, no sleep after a bad output). So the runner sends the referee every line of a bot's
// output split with String#split: an empty line gives the referee no line at all, and the player is
// then deactivated by the IndexOutOfBoundsException of outputs[0]. The jar is built with
// referees/cg-builder's CommandLineInterface-v3.java (3.4.5's runner returns the older GameResult).
// The bots get 1000 ms on their first execution (the third player's comes in the second round) and
// 50 ms after; the 600 executions with output reach the engine's 30 s total exactly, without passing it.
//
// The Kotlin referee is followed closely: same data structures, iteration orders, RNG consumption,
// error texts. Notable behaviours reproduced here:
//   - new java.util.Random(seed): Collections.shuffle (Kotlin's shuffled(java.util.Random)) of the 30
//     spawn cells (the two first are the spawns of every round), of the 45 equipment cells (the oven
//     takes the next one that is an oven cell, those skipped are lost), then the 20 customers of the
//     looping queue, each a shuffle of the 5 dishes (LinkedHashMap order) and a nextDouble() for how
//     many it wants (Kotlin `when` ranges: [0, .25] -> 4, [.25, .5] -> 3, else 2); the customer queue is
//     the same 20 objects in every round, an award reset when a customer comes back, so a customer
//     still waiting when the queue wraps around is in the queue twice (and decremented twice a turn).
//   - The moves: Cell#buildDistanceMap (a java.util.PriorityQueue by distance, LinkedHashMaps: visit
//     order and the last expanded neighbour as trace-back), the partner's cell blocked when the
//     partner is active, a table target replaced by its closest reachable floor neighbour (first in
//     straight then diagonal neighbour order), unreachable targets (cut off by the partner in a dead
//     end) sending the player next to its partner (with an inactive partner nothing is blocked, so the
//     KotlinNullPointerException of `blockedCell!!` cannot happen: every table has a floor neighbour),
//     moves longer than 9 stopping at the cell closest to the target (first in visit order), the path
//     traced back from the new location.
//   - USE: reach 3 from the table (adjacent cells), farther tables are moved to; every equipment and
//     item combination with the jar's exceptions: LogicExceptions only add "$N: <message>" to the game
//     summary, the others (a USE of a floor cell, invalid commands) deactivate the player; the texts
//     use Kotlin's data class toString for dishes and shells, e.g. "Cannot take Dish(contents=
//     [ICE_CREAM]) while holding STRAWBERRIES!".
//   - The commands: Kotlin's String.trim() (Unicode spaces, not the control chars), the message after
//     the first ';' (view only), Kotlin's split(" ") keeping empty tokens (two spaces: invalid),
//     Integer.parseInt, coordinates checked by the arrays, the command checked after its coordinates.
//   - The turn order of the jar: oven tick, skipped turns of a deactivated player (each still ticks the
//     oven and counts in the round's 200 turns, so rounds end sooner, and a game whose rounds all
//     ended stops at the start of a 4th round), new customers, inputs, execution, action, award decay.
//   - A dish held by a deactivated player goes back to the dishwasher (one more dish); every dish given
//     to the window goes back too, served or not.
//   - The view code where it changes the game (it runs inside the referee's try/catch): a player path
//     of 6+ cells would deactivate the player (GraphicEntityModule refuses frame instants above 1;
//     never happens: paths are at most 5 cells); with 4 players the 4th one has no character sprite
//     and the game fails when it should first appear (start of the second round), with 2 players the
//     referee fails in init: null scores, like the jar.
//   - Engine (common/engine.h): the protocol between the runner and the referee (java.util.Scanner
//     lines: U+0085, U+2028 or U+2029 in a bot line makes the game fail; 30000-byte commands: a game
//     summary or tooltip quoting a long invalid command full of U+FFFD fails the game), the game summary
//     quotas.
// Not reproduced: the view (GraphicEntityModule 3.4.5 entities, the game's TooltipModule and
// TextLimitModule, EndScreenModule): no bot can bring it near the engine's limits (the first frame is
// ~8.6 KB against the runner's 100000 bytes, the others under 1 KB against 30000; a whole game stays
// under 200 KB against 1 MB, even for a bot writing an 18-char message of escaped chars every turn and
// moving 5 cells every other turn), so the -l trace has no view frame sizes; what the referee prints to
// its stdout (println of every move's path, teed by the jar's runner to the real stdout: no number) and
// stderr; the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps, so a bot
// answering in ~50 ms may be timed out by one and not the other; on a loaded machine the jar times out
// bots that answered in time).
// Validated (2026-10-25) with battle referee-diff against the jar on 3388 games (33 bots: every bot in
// every seat with 8 fixed random partner pairs, every bot alone, 22 targeted triples; seeds from 1,
// 1000001, 987654321 and 5000000000: all identical, no mismatch, no flake), in direct mode (jar game
// log vs -l trace: every execution, frame count, summaries, tooltips, scores) on 568 games, including
// raw line endings, invalid UTF-8, U+2028 / U+0085 / U+2029, a summary at the runner's 30000-byte limit,
// outputs at its 10000-byte one, 2- and 4-player games (2 replays of jar wall-clock timeouts matched);
// Kotlin's trim / split(" ") and Integer.parseInt against the jar's kotlin-stdlib on 331072 strings.
// Not exercised by the bots: a customer queued twice (it takes 18 served customers in a round) and the
// unreachable paths above (KotlinNullPointerException, 6-cell paths), which follow the Kotlin code.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/engine.h"

#include <optional>

using java::String;

static String A(const char* s) { return java::ascii(s); }
static String I(i64 v) { return java::str(v); }

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------
static const int WIDTH = 11, HEIGHT = 7, NCELLS = WIDTH * HEIGHT;
static const int REACH_DISTANCE = 3;
static const int WALK_DISTANCE = 9;
static const int OVEN_COOKTIME = 10;
static const int OVEN_BURNTIME = 10;
static const int TIP = 200;
static const int ROUND_TURNS = 200;

static const char* BOARD_LAYOUT[HEIGHT] = {
    "***********", //
    "*.........*", //
    "*.****.**.*", //
    "*.*..*..*.*", //
    "*.**.****.*", //
    "*.........*", //
    "***********", //
};
static const char* EQUIPMENT_LOCS[] = {"A0", "B0", "C0", "D0", "E0", "G0", "H0", "I0", "J0", "K0", "A1", "A2", "A3", "A4", "A5",
                                       "K1", "K2", "K3", "K4", "K5", "C2", "D2", "E2", "F2", "H2", "I2", "C3", "F3", "I3", "C4",
                                       "D4", "F4", "G4", "H4", "I4", "A6", "B6", "C6", "D6", "E6", "G6", "H6", "I6", "J6", "K6"};
static const char* OVEN_LOCS[] = {"B0", "C0", "D0", "E0", "G0", "H0", "I0", "J0", "A1", "A2", "A3", "A4", "A5", "K1", "K2", "K3", "K4", "K5"};
static const char* SPAWN_LOCS = "B1 C1 D1 E1 F1 G1 H1 I1 J1 B5 C5 D5 E5 F5 G5 H5 I5 J5 B2 B3 B4 J2 J3 J4 D3 E3 E4 G2 G3 H3";

// ----------------------------------------------------------------------------
// Items (Items.kt, Dishes.kt, Containers.kt)
// ----------------------------------------------------------------------------
// The singletons (Kotlin objects) and the two mutable kinds (Shell, Dish); a dish or a shell is never
// in two places at once, so they are values here.
enum Kind { ICE_CREAM, BLUEBERRIES, CHOPPED_STRAWBERRIES, CROISSANT, TART, STRAWBERRIES, DOUGH, SHELL, DISH };
static const char* KIND_NAME[] = {"ICE_CREAM", "BLUEBERRIES", "CHOPPED_STRAWBERRIES", "CROISSANT", "TART", "STRAWBERRIES", "DOUGH"};
static bool isEdible(Kind k) { return k <= TART; } // EdibleItem

struct Item {
    Kind kind = ICE_CREAM;
    bool hasBlueberry = false;  // Shell
    std::vector<Kind> contents; // Dish: a LinkedHashSet<EdibleItem>, insertion order
};
static Item item(Kind k) {
    Item i;
    i.kind = k;
    return i;
}
static Item dish(std::vector<Kind> contents = {}) {
    Item i;
    i.kind = DISH;
    i.contents = contents;
    return i;
}

// Item#describe
static std::string describe(const Item& it) {
    if (it.kind == DISH) {
        std::string s = "DISH";
        for (Kind k : it.contents) s += std::string("-") + KIND_NAME[k];
        return s;
    }
    if (it.kind == SHELL) return it.hasBlueberry ? "RAW_TART" : "CHOPPED_DOUGH";
    return KIND_NAME[it.kind];
}
// toString(): describe(), except the data classes Dish and Shell
static std::string toString(const Item& it) {
    if (it.kind == DISH) {
        std::string s = "Dish(contents=[";
        for (size_t i = 0; i < it.contents.size(); i++) s += std::string(i ? ", " : "") + KIND_NAME[it.contents[i]];
        return s + "])";
    }
    if (it.kind == SHELL) return std::string("Shell(hasBlueberry=") + (it.hasBlueberry ? "true" : "false") + ")";
    return describe(it);
}
static bool contains(const std::vector<Kind>& v, Kind k) { return std::find(v.begin(), v.end(), k) != v.end(); }
// Dish#equals (data class: contents, a Set)
static bool sameContents(const std::vector<Kind>& a, const std::vector<Kind>& b) {
    if (a.size() != b.size()) return false;
    for (Kind k : a)
        if (!contains(b, k)) return false;
    return true;
}

// Exceptions of the game: LogicException (game summary only) and the other ones (deactivation);
// AbstractPlayer.TimeoutException
struct LogicException {
    std::string message;
};
struct GameException {
    String message;
    bool isNull = false; // ex.message == null (KotlinNullPointerException)
};
struct TimeoutException {};

static LogicException logic(const std::string& m) { return LogicException{m}; }

// ----------------------------------------------------------------------------
// Equipment (ItemsAndEquipment.kt, Crates.kt, ChoppingBoard.kt, Dishes.kt, Customers.kt, Oven.kt)
// ----------------------------------------------------------------------------
enum Equip { NO_EQUIPMENT, ICE_CREAM_CRATE, BLUEBERRY_CRATE, STRAWBERRY_CRATE, CHOPPING_BOARD, DOUGH_CRATE, OVEN, DISH_WASHER, WINDOW };
static const char* EQUIP_TOOLTIP[] = {"", "Ice cream crate", "Blueberry crate", "Strawberry crate", "Chopping board", "Dough crate", "Oven", "Dish washer", "Window"};
static const char EQUIP_CHAR[] = {'#', 'I', 'B', 'S', 'C', 'H', 'O', 'D', 'W'};

enum OvenStateKind { OVEN_EMPTY, OVEN_BAKING, OVEN_READY, OVEN_BURNING };
struct OvenState {
    OvenStateKind kind = OVEN_EMPTY;
    Item contents;     // Baking: Dough or a Shell; Ready: Croissant or Tart
    int timer = 0;     // timeUntilCooked / timeUntilBurnt
    std::string text;  // contentsStr, describe() of the contents when the state was made
    std::string toString() const {
        if (kind == OVEN_EMPTY || kind == OVEN_BURNING) return "NONE 0";
        return text + " " + std::to_string(timer);
    }
};
static OvenState baking(const Item& contents, int t) { return OvenState{OVEN_BAKING, contents, t, describe(contents)}; }
static OvenState ready(Kind k, int t) { return OvenState{OVEN_READY, item(k), t, KIND_NAME[k]}; }

// ----------------------------------------------------------------------------
// Board (BoardFeatures.kt, BoardBuilding.kt)
// ----------------------------------------------------------------------------
struct Cell {
    int x = 0, y = 0;
    bool isTable = true;
    int playerId = -1; // spawn 0 / 1
    Equip equipment = NO_EQUIPMENT;
    std::optional<Item> item;
    std::vector<std::pair<int, int>> neighbours; // (cell, 2 straight / 3 diagonal)
    std::string toString() const { return "(" + std::to_string(x) + ", " + std::to_string(y) + ")"; }
};

// allCells = cells.flatten(): x-major
static int cellIndex(int x, int y) { return x * HEIGHT + y; }

static Cell cells[NCELLS];
static int spawnLocations[2];
static int dishes = 3;        // the DishWasher (F0)
static OvenState ovenState;   // the Oven
static int ovenCell = -1;
static java::Random rnd;      // `rand`

// Board#get(cellName)
static int cellNamed(const std::string& name) { return cellIndex(name[0] - 'A', std::atoi(name.c_str() + 1)); }

static void buildBoard() {
    // Board(layout, allSpawns): spawnLocations, cells, connections
    std::vector<std::string> spawns = java::split(std::string(SPAWN_LOCS), ' ');
    java::shuffle(spawns, rnd);
    for (int i = 0; i < 2; i++) spawnLocations[i] = cellNamed(spawns[i]);
    for (int x = 0; x < WIDTH; x++)
        for (int y = 0; y < HEIGHT; y++) {
            Cell& c = cells[cellIndex(x, y)];
            c.x = x;
            c.y = y;
            c.isTable = BOARD_LAYOUT[y][x] != '.';
            c.playerId = -1;
            for (int i = 0; i < 2; i++)
                if (spawnLocations[i] == cellIndex(x, y)) {
                    c.playerId = i;
                    break;
                }
        }
    std::vector<int> straight[NCELLS], diagonal[NCELLS];
    for (int x = 0; x < WIDTH; x++)
        for (int y = 0; y < HEIGHT; y++)
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    int x2 = x + dx, y2 = y + dy;
                    if (x2 < 0 || x2 >= WIDTH || y2 < 0 || y2 >= HEIGHT) continue;
                    (dx * dy == 0 ? straight : diagonal)[cellIndex(x, y)].push_back(cellIndex(x2, y2));
                }
    // neighbours: straight ones (2), then the diagonal ones touching a table (3)
    for (int i = 0; i < NCELLS; i++) {
        for (int n : straight[i]) cells[i].neighbours.push_back({n, 2});
        for (int n : diagonal[i])
            if (cells[n].isTable || cells[i].isTable) cells[i].neighbours.push_back({n, 3});
    }

    cells[cellNamed("F0")].equipment = DISH_WASHER;
    cells[cellNamed("F6")].equipment = WINDOW;
    std::vector<std::string> locs(std::begin(EQUIPMENT_LOCS), std::end(EQUIPMENT_LOCS));
    java::shuffle(locs, rnd);
    size_t it = 0;
    const Equip order[6] = {ICE_CREAM_CRATE, BLUEBERRY_CRATE, STRAWBERRY_CRATE, CHOPPING_BOARD, DOUGH_CRATE, OVEN};
    for (Equip eq : order) {
        // equipmentLocs.findNext { eq !is Oven || it in ovenLocs } (never exhausted)
        std::string loc;
        while (true) {
            loc = locs.at(it++);
            if (eq != OVEN || std::find(std::begin(OVEN_LOCS), std::end(OVEN_LOCS), loc) != std::end(OVEN_LOCS)) break;
        }
        cells[cellNamed(loc)].equipment = eq;
        if (eq == OVEN) ovenCell = cellNamed(loc);
    }
}

// Board#reset
static void resetBoard() {
    for (Cell& c : cells) {
        if (c.equipment == DISH_WASHER) dishes = 3;
        if (c.equipment == OVEN) ovenState = OvenState();
        c.item.reset();
    }
}

// Board#tick: the oven
static void tick() {
    OvenState& s = ovenState;
    switch (s.kind) {
    case OVEN_EMPTY:
        return;
    case OVEN_BURNING:
        s = OvenState();
        return;
    case OVEN_BAKING:
        if (s.timer == 1) {
            if (s.contents.kind == DOUGH) s = ready(CROISSANT, OVEN_BURNTIME);
            else if (s.contents.kind == SHELL) s = ready(TART, OVEN_BURNTIME);
            else throw java::Exception{"java.lang.Exception: Wasn't expecting oven contents: " + toString(s.contents) + "!"};
        } else {
            s = baking(s.contents, s.timer - 1);
        }
        return;
    case OVEN_READY:
        if (s.timer == 1) s = OvenState{OVEN_BURNING, Item(), 0, ""};
        else s = ready(s.contents.kind, s.timer - 1);
        return;
    }
}

// Cell#buildDistanceMap: distances (-1: not visited), the visit order, the trace-back (-1: none)
struct DistanceMap {
    int dist[NCELLS];
    int traceBack[NCELLS];
    std::vector<int> order;
};
static DistanceMap buildDistanceMap(int from, int blockedCell) {
    DistanceMap m;
    for (int i = 0; i < NCELLS; i++) m.dist[i] = m.traceBack[i] = -1;
    auto cmp = [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return java::compareInt(a.second, b.second); };
    java::PriorityQueue<std::pair<int, int>, decltype(cmp)> flooded(cmp);
    flooded.offer({from, 0});
    bool isFirst = true;
    while (!flooded.isEmpty()) {
        std::pair<int, int> e = flooded.poll();
        int cell = e.first, dist = e.second;
        if (m.dist[cell] >= 0) continue;
        m.dist[cell] = dist;
        m.order.push_back(cell);
        if ((!cells[cell].isTable && cell != blockedCell) || isFirst) {
            std::vector<std::pair<int, int>> add;
            for (auto& nb : cells[cell].neighbours) {
                if (nb.first == blockedCell) continue;
                if (m.dist[nb.first] >= 0) continue;
                add.push_back(nb);
            }
            for (auto& nb : add) {
                m.traceBack[nb.first] = cell;
                flooded.offer({nb.first, dist + nb.second});
            }
        }
        isFirst = false;
    }
    return m;
}

// ----------------------------------------------------------------------------
// Customers (Customers.kt)
// ----------------------------------------------------------------------------
static const Kind PLATE_KINDS[5] = {ICE_CREAM, BLUEBERRIES, CHOPPED_STRAWBERRIES, CROISSANT, TART};
static int plateValue(Kind k) {
    switch (k) {
    case ICE_CREAM: return 200;
    case BLUEBERRIES: return 250;
    case CHOPPED_STRAWBERRIES: return 400;
    case CROISSANT: return 650;
    case TART: return 1000;
    default: return 0;
    }
}

struct Customer {
    std::vector<Kind> dish;
    int award = 0;
    int originalAward = 0;
    bool satisfied = false;
};
static std::vector<Customer> originalQueue; // the same 20 objects in every round

static Customer randomCustomer() {
    std::vector<Kind> keys(std::begin(PLATE_KINDS), std::end(PLATE_KINDS));
    java::shuffle(keys, rnd);
    double d = rnd.nextDouble();
    int n = (0.0 <= d && d <= 0.25) ? 4 : (0.25 <= d && d <= 0.5) ? 3 : 2;
    Customer c;
    c.dish.assign(keys.begin(), keys.begin() + n);
    int price = TIP;
    for (Kind k : c.dish) price += plateValue(k);
    c.award = c.originalAward = price;
    return c;
}

struct CustomerQueue {
    size_t next = 0;         // the looping iterator over originalQueue
    std::vector<int> active; // activeCustomers (indices in originalQueue)

    void getNewCustomers() {
        active.erase(std::remove_if(active.begin(), active.end(), [](int c) { return originalQueue[c].satisfied; }), active.end());
        while (active.size() < 3) {
            int c = (int)(next++ % originalQueue.size());
            originalQueue[c].award = originalQueue[c].originalAward;
            originalQueue[c].satisfied = false;
            active.push_back(c);
        }
    }
    void updateRemainingCustomers() {
        std::vector<int> waiting;
        for (int c : active)
            if (!originalQueue[c].satisfied) waiting.push_back(c);
        for (int c : waiting) originalQueue[c].award -= 1;
    }
};

// ----------------------------------------------------------------------------
// Players
// ----------------------------------------------------------------------------
struct Player : cg::Player {
    int location = -1;
    std::optional<Item> heldItem;
    int partner = -1;
    bool crashed = false;
    bool hasSprite = false; // characterSprite.image != null
    int getExpectedOutputLines() const override { return 1; }
};
static Player players[4];

static String token(const Player& p) { return p.getNicknameToken(); }

// ----------------------------------------------------------------------------
// The referee (Referee.kt, Player.kt)
// ----------------------------------------------------------------------------
struct ScoreEntry {
    int roundScores[3];
    bool has[3];
    int total() const {
        int s = 0;
        for (int i = 0; i < 3; i++)
            if (has[i]) s += roundScores[i];
        return s;
    }
};

struct CalmReferee : cg::Referee {
    std::vector<int> matchPlayers;
    ScoreEntry scoreBoard[3]; // of players 0, 1, 2
    CustomerQueue queue;
    bool hasRound = false;
    int roundNumber = -1;
    // RoundReferee
    int roundPlayers[2] = {0, 0};
    int roundScore = 0;
    int turn = 0;
    int nextPlayerId = 0;

    Player& P(int i) { return players[i]; }

    void init() override {
        rnd = java::Random(cg::gm.getSeed());
        int n = cg::gm.getPlayerCount();
        for (int i = 0; i < n; i++) matchPlayers.push_back(i);
        if (n < 3) throw java::Exception{java::indexOutOfBoundsException(2, n)}; // matchPlayers[2]
        const bool has[3][3] = {{true, true, false}, {true, false, true}, {false, true, true}};
        for (int i = 0; i < 3; i++)
            for (int r = 0; r < 3; r++) {
                scoreBoard[i].roundScores[r] = 0;
                scoreBoard[i].has[r] = has[i][r];
            }
        cg::gm.setMaxTurns(606);
        cg::gm.setFrameDuration(400);
        buildBoard();
        for (int i : matchPlayers) {
            Player& p = P(i);
            if (originalQueue.empty()) // CustomersKt's static initialiser, at the first access
                for (int k = 0; k < 20; k++) originalQueue.push_back(randomCustomer());
            describeCustomers(p, true);
            for (int y = 0; y < HEIGHT; y++) {
                std::string row;
                for (int x = 0; x < WIDTH; x++) {
                    const Cell& c = cells[cellIndex(x, y)];
                    if (c.playerId >= 0) row += (char)('0' + (p.index == 1 ? 1 - c.playerId : c.playerId));
                    else if (!c.isTable) row += '.';
                    else row += EQUIP_CHAR[c.equipment];
                }
                p.sendInputLine(row);
            }
        }
    }

    void describeCustomers(Player& p, bool all) {
        std::vector<int> list;
        if (all)
            for (int i = 0; i < (int)originalQueue.size(); i++) list.push_back(i);
        else
            list = queue.active;
        p.sendInputLine(std::to_string(list.size()));
        for (int c : list) {
            Item d = dish(originalQueue[c].dish);
            p.sendInputLine(describe(d) + " " + std::to_string(originalQueue[c].award));
        }
    }

    void nextRound() {
        int a = matchPlayers[0], b = matchPlayers[1];
        std::rotate(matchPlayers.rbegin(), matchPlayers.rbegin() + 1, matchPlayers.rend()); // Collections.rotate(list, 1)
        resetBoard();
        queue = CustomerQueue();
        queue.getNewCustomers();
        P(a).location = spawnLocations[0];
        P(a).heldItem.reset();
        P(b).location = spawnLocations[1];
        P(b).heldItem.reset();
        roundNumber++;
        // RoundReferee(roundPlayers, roundNumber)
        roundPlayers[0] = a;
        roundPlayers[1] = b;
        roundScore = 0;
        turn = 0;
        nextPlayerId = 0;
        P(a).partner = b;
        P(b).partner = a;
        hasRound = true;
    }

    void gameTurn(int engineTurn) override {
        int matchTurn = engineTurn - 1; // engine 3.4.5 counts from 0
        if (!hasRound || turn > ROUND_TURNS) {
            nextRound();
            if (roundNumber >= 3) {
                cg::gm.endGame();
                return;
            }
            for (int p : roundPlayers) cg::gm.addTooltip(p, A("Round ") + I(roundNumber + 1) + A(" starting!"));
            for (int p : roundPlayers) updatePlayer(P(p), nullptr); // view.boardView.resetPlayers()
        } else {
            roundTurn(matchTurn);
        }
    }

    // RoundReferee#gameTurn
    void roundTurn(int matchTurn) {
        turn++;
        if (turn > ROUND_TURNS) return;
        tick();
        Player& p = P(roundPlayers[nextPlayerId]);
        nextPlayerId = 1 - nextPlayerId;
        if (!p.isActive()) {
            if (p.score == 0) p.score = -1000 + matchTurn;
            p.crashed = true;
            roundTurn(matchTurn);
            return;
        }
        queue.getNewCustomers();
        sendGameState(p);
        p.execute();
        try {
            processPlayerActions(p);
        } catch (const LogicException& e) {
            cg::gm.addToGameSummary(token(p) + A(": ") + A(e.message.c_str()));
        } catch (const TimeoutException&) {
            disablePlayer(p, A("timeout"));
        } catch (const GameException& e) {
            disablePlayer(p, e.isNull ? A("null") : e.message);
        }
        queue.updateRemainingCustomers();
    }

    void sendGameState(Player& p) {
        p.sendInputLine(std::to_string(ROUND_TURNS - turn));
        Player& partner = P(p.partner);
        for (Player* q : {&p, &partner}) {
            if (q->isActive())
                p.sendInputLine(std::to_string(cells[q->location].x) + " " + std::to_string(cells[q->location].y) + " " +
                                (q->heldItem ? describe(*q->heldItem) : "NONE"));
            else
                p.sendInputLine(std::string("-1 -1 NONE"));
        }
        std::vector<int> withItems;
        for (int i = 0; i < NCELLS; i++)
            if (cells[i].isTable && cells[i].item) withItems.push_back(i);
        p.sendInputLine(std::to_string(withItems.size()));
        for (int i : withItems) p.sendInputLine(std::to_string(cells[i].x) + " " + std::to_string(cells[i].y) + " " + describe(*cells[i].item));
        p.sendInputLine(ovenCell >= 0 ? ovenState.toString() : std::string("NONE 0"));
        describeCustomers(p, false);
    }

    void processPlayerActions(Player& p) {
        if (p.timeout) throw TimeoutException(); // getOutputs()
        if (p.outputs.empty()) throw GameException{A("Index 0 out of bounds for length 0")};
        String line = java::kotlinTrim(p.outputs[0]);
        if (line.empty()) line = A("WAIT");
        size_t semicolon = line.find(u';');
        String fullCommand = semicolon != String::npos ? line.substr(0, semicolon) : line; // the rest: a message (view)
        std::vector<String> toks = java::kotlinSplit(fullCommand, String(u" "));
        size_t next = 0;
        String command = toks[next++];
        std::vector<int> path;
        bool hasPath = false;
        if (command != A("WAIT")) {
            i32 x = 0, y = 0;
            bool ok = next < toks.size() && java::parseInt(toks[next++], x);
            ok = ok && next < toks.size() && java::parseInt(toks[next++], y);
            ok = ok && x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT;
            if (!ok) throw GameException{A("Invalid command: ") + fullCommand};
            int target = cellIndex(x, y);
            if (command == A("MOVE")) path = moveTo(p, target);
            else if (command == A("USE")) path = use(p, target);
            else throw GameException{A("Invalid command: ") + fullCommand};
            hasPath = true;
        }
        updatePlayer(p, hasPath ? &path : nullptr);
    }

    // BoardView#updatePlayer, where it can throw: the 4th player has no sprite, frame instants above 1
    void updatePlayer(Player& p, const std::vector<int>* path) {
        if (p.crashed) return;
        auto sprites = [&]() {
            if (p.index >= 3) throw java::Exception{java::indexOutOfBoundsException(p.index, 3)}; // playerSprites[index]
        };
        if (!p.hasSprite) {
            sprites();
            p.hasSprite = true;
        }
        if (path) {
            const std::vector<int>& pp = *path;
            for (size_t index = 0; index < pp.size(); index++) {
                double t = 0.2 * (double)index + 0.1;
                if (pp.size() > 1 && pp.front() != pp.back() && index + 1 < pp.size()) {
                    sprites();
                    commit(t);
                }
                commit(t);
            }
        }
    }
    static void commit(double t) {
        if (t < 0 || t > 1) throw GameException{A("Not a valid frame instant: ") + java::ascii(java::doubleToString(t))};
    }

    void disablePlayer(Player& p, const String& message) {
        p.crashed = true;
        cg::gm.addToGameSummary(token(p) + A(": ") + message + A(" (deactivating!)"));
        p.deactivate(token(p) + A(": ") + message);
        if (p.heldItem && p.heldItem->kind == DISH) dishes++;
    }

    // Player#moveTo
    std::vector<int> moveTo(Player& p, int cell) {
        Player& partner = P(p.partner);
        int blockedCell = partner.isActive() ? partner.location : -1;
        DistanceMap fromSource = buildDistanceMap(p.location, blockedCell);
        int target;
        if (!cells[cell].isTable && cell != blockedCell) {
            target = cell;
        } else {
            target = -1;
            for (auto& nb : cells[cell].neighbours) {
                int c = nb.first;
                if (cells[c].isTable || fromSource.dist[c] < 0) continue;
                if (target < 0 || fromSource.dist[c] < fromSource.dist[target]) target = c;
            }
            if (target < 0) {
                if (blockedCell < 0) throw GameException{String(), true};
                return moveTo(p, blockedCell);
            }
        }
        if (fromSource.dist[target] < 0) {
            // "Warning: cannot move! Moving to partner location instead"
            if (blockedCell < 0) throw GameException{String(), true};
            return moveTo(p, blockedCell);
        }
        if (fromSource.dist[target] <= WALK_DISTANCE) { // location.distanceTo(target, blockedCell): the same map
            p.location = target;
            return trace(cell, p.location, fromSource);
        }
        DistanceMap fromTarget = buildDistanceMap(target, blockedCell);
        int best = -1;
        for (int c : fromSource.order) {
            if (fromSource.dist[c] > WALK_DISTANCE || cells[c].isTable) continue;
            if (fromTarget.dist[c] < 0) throw GameException{String(), true};
            if (best < 0 || fromTarget.dist[c] < fromTarget.dist[best]) best = c;
        }
        p.location = best;
        return trace(cell, p.location, fromSource);
    }

    static std::vector<int> trace(int source, int target, const DistanceMap& m) {
        std::vector<int> r{target};
        int cur = target;
        while (cur != source) {
            int prev = m.traceBack[cur];
            if (prev < 0) break;
            cur = prev;
            r.push_back(cur);
        }
        std::reverse(r.begin(), r.end());
        return r;
    }

    // Player#use
    std::vector<int> use(Player& p, int cellIdx) {
        Cell& cell = cells[cellIdx];
        std::vector<int> useCellPath{p.location, cellIdx, cellIdx, p.location};
        if (!cell.isTable) throw GameException{A(("Cannot use " + cell.toString() + ": not a table!").c_str())};
        int d = buildDistanceMap(cellIdx, -1).dist[p.location];
        if (d < 0 || d > REACH_DISTANCE) return moveTo(p, cellIdx);
        if (cell.equipment != NO_EQUIPMENT) {
            useEquipment(p, cell.equipment);
            return useCellPath;
        }
        if (p.heldItem) {
            if (cell.item) {
                Item held = *p.heldItem;
                receiveItem(p, *cell.item, held, &cell);
                return useCellPath;
            }
            cell.item = p.heldItem;
            p.heldItem.reset();
            return useCellPath;
        }
        if (cell.item) { // Item#take
            p.heldItem = cell.item;
            cell.item.reset();
            return useCellPath;
        }
        throw logic("Cannot USE this table now, nothing to do!");
    }

    // `self`.receiveItem(player, item, cell) of an Item (the one on a table, or the held one when a
    // crate gives it something: cell null). `self` is changed in place.
    void receiveItem(Player& p, Item& self, const Item& item, Cell* cell) {
        auto base = [&]() { throw logic("Cannot take " + toString(self) + " while holding " + toString(item) + "!"); };
        if (self.kind == BLUEBERRIES && item.kind == SHELL) {
            if (item.hasBlueberry) throw logic("This already has blueberries!");
            Item shell = item;
            shell.hasBlueberry = true;
            p.heldItem = shell;
            if (cell) cell->item.reset();
            return;
        }
        if (isEdible(self.kind)) { // EdibleItem
            if (item.kind == DISH) {
                Item d = item;
                plusAssign(d, self.kind);
                p.heldItem = d; // the held dish (item) itself
                if (!cell) throw GameException{String(), true}; // cell!! (never: crates give no dish)
                cell->item.reset();
                return;
            }
            base();
        }
        if (self.kind == SHELL) {
            if (item.kind != BLUEBERRIES) throw logic("Cannot add " + toString(item) + " to " + toString(self) + "!");
            if (self.hasBlueberry) throw logic("This already has blueberries!");
            self.hasBlueberry = true;
            Item shell = self;
            p.heldItem = shell;
            if (cell) cell->item.reset();
            return;
        }
        if (self.kind == DISH) {
            if (isEdible(item.kind)) {
                plusAssign(self, item.kind);
                p.heldItem.reset();
                return;
            }
            base();
        }
        base(); // Strawberries, Dough
    }

    // Container.plusAssign
    static void plusAssign(Item& d, Kind k) {
        if (contains(d.contents, k)) throw logic("Can't add: " + toString(d) + " already contains " + KIND_NAME[k]);
        d.contents.push_back(k);
    }

    // Equipment#use
    void useEquipment(Player& p, Equip eq) {
        if (p.heldItem) {
            Item held = *p.heldItem;
            equipmentReceive(p, eq, held);
            return;
        }
        p.heldItem = takeFrom(eq);
    }

    Item takeFrom(Equip eq) {
        switch (eq) {
        case ICE_CREAM_CRATE: return item(ICE_CREAM);
        case BLUEBERRY_CRATE: return item(BLUEBERRIES);
        case STRAWBERRY_CRATE: return item(STRAWBERRIES);
        case DOUGH_CRATE: return item(DOUGH);
        case DISH_WASHER:
            removeDish();
            return dish();
        case OVEN: {
            switch (ovenState.kind) {
            case OVEN_EMPTY:
            case OVEN_BURNING: throw logic("Cannot take from Oven: nothing inside!");
            case OVEN_BAKING: throw logic("Cannot take from Oven: food is baking!");
            case OVEN_READY: {
                Item r = ovenState.contents;
                ovenState = OvenState();
                return r;
            }
            }
            return Item();
        }
        default: throw logic(std::string("Cannot USE ") + EQUIP_TOOLTIP[eq] + " now, not holding anything!");
        }
    }

    static void removeDish() {
        if (dishes <= 0) throw logic("Dishwasher is empty!");
        dishes--;
    }

    void equipmentReceive(Player& p, Equip eq, const Item& held) {
        auto base = [&]() { throw logic("Cannot drop " + toString(held) + " onto " + EQUIP_TOOLTIP[eq] + "!"); };
        switch (eq) {
        case ICE_CREAM_CRATE:
        case BLUEBERRY_CRATE:
        case STRAWBERRY_CRATE:
        case DOUGH_CRATE: {
            // GeneralCrate: item.receiveItem(player, newItem, null); player.heldItem = item (the held
            // dish or shell, as its receiveItem changed it)
            Item self = held;
            receiveItem(p, self, takeFrom(eq), nullptr);
            p.heldItem = self;
            return;
        }
        case CHOPPING_BOARD:
            if (held.kind == STRAWBERRIES) {
                p.heldItem = item(CHOPPED_STRAWBERRIES);
                return;
            }
            if (held.kind == DOUGH) {
                p.heldItem = item(SHELL);
                return;
            }
            base();
            return;
        case DISH_WASHER:
            if (held.kind == DISH) {
                p.heldItem = dish();
                return;
            }
            if (isEdible(held.kind)) {
                removeDish();
                p.heldItem = dish({held.kind});
                return;
            }
            base();
            return;
        case WINDOW:
            if (held.kind == DISH) {
                deliver(held);
                dishes++; // dishWasher?.addDish()
                p.heldItem.reset();
                return;
            }
            base();
            return;
        case OVEN:
            if (ovenState.kind == OVEN_READY && held.kind == DISH) {
                Item d = held;
                plusAssign(d, ovenState.contents.kind); // Dish#receiveItem
                p.heldItem = d;
                ovenState = OvenState();
                return;
            }
            if (ovenState.kind != OVEN_EMPTY && ovenState.kind != OVEN_BURNING) throw logic("Cannot insert: oven not empty!");
            if (held.kind == SHELL && held.hasBlueberry) {
                ovenState = baking(held, OVEN_COOKTIME);
                p.heldItem.reset();
                return;
            }
            if (held.kind == DOUGH) {
                ovenState = baking(held, OVEN_COOKTIME);
                p.heldItem.reset();
                return;
            }
            base();
            return;
        default:
            base();
        }
    }

    // CustomerQueue#delivery (+ RoundReferee's onPointsAwarded)
    void deliver(const Item& d) {
        int best = -1;
        for (int c : queue.active)
            if (sameContents(originalQueue[c].dish, d.contents) && (best < 0 || originalQueue[c].award > originalQueue[best].award)) best = c;
        if (best < 0) return; // onFailure: the view
        roundScore += originalQueue[best].award;
        for (ScoreEntry& e : scoreBoard)
            if (e.has[roundNumber]) e.roundScores[roundNumber] = roundScore;
        originalQueue[best].satisfied = true;
    }

    void onEnd() override {
        for (int i = 0; i < 3; i++) P(i).setScore(P(i).crashed ? -1 : scoreBoard[i].total());
    }
};

static CalmReferee referee;

int main(int argc, char** argv) {
    cg::Options options;
    options.engine313 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
