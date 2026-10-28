// Platinum Rift - Episode 1 (CodinGame, 2014, "platinum-rift-episode-1") - headless C++ referee, port of the CodinGame
// referee in https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the only commit of the
// file), "Referees/Platinum Rift/Referee.java": the single-file referee `class Referee extends MultiReferee` with
// CodinGame's old AbstractReferee base of 2014 embedded (common/legacy.h, legacy::Base::platinumRift2014(): the 2014
// INFOS / SUMMARY, the scores updated at every step of nextPlayer(), the reason code "timeout", the texts "$N: invalid
// action" / "$N: timeout!" as tooltips, a WinException ending the game (never thrown here), getMillisTimeForRound 150,
// no precomputed inputs, the frame data and the tooltips sent in every frame; its SoloReferee class is unused). The file
// is the game CodinGame runs today, in every league: CodinGame's replays give the referee's own configuration (default
// map, platinum 120, startPlatinum 200, unitCost 20, finale=true and the noEmptyIslands / richAmericas / distribution
// that the seed draws), and the jar's classes and this port reproduce them from the seed alone. 2 to 4 players
// (CodinGame runs all three; getMinimumPlayerCount() is 1: a player left alone plays on until the game's own end rules
// stop it). No league rules in the referee. Deterministic: reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 ... -p4 ...] -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): richAmericas, platinum, seed,
// noEmptyIslands, finale, unitCost, distribution, startPlatinum, map, in the order of a JDK 9+ Properties) to stdout like
// the jar (referees/cg-builder/legacy: the referee runs unchanged in the runner of the SDK 4.5.0). "null" scores when the
// game fails (the Java referee crashed or the protocol broke; the jar after 150 s). -l writes the trace of
// common/legacy.h (bot executions, view sizes, summaries, tooltips, END; CG_TRACE_VIEWS=1 adds the text of every view).
// Time limits: the base's 1000 ms for the first turn, 150 ms after (the game overrides neither).
//
// The game: a world map of 154 hexagonal zones (6 continents and islands; the referee shifts Africa down one row, cuts
// the Suez links and moves Panama: "finale"), 120 platinum spread over the zones from the seed (1 to 6 per zone; every
// island one zone at least when noEmptyIslands, 55% of the rest on the second largest landmass when richAmericas; the
// amounts drawn by one of four distributions). Every player starts with 200 platinum and no pod. Each round every player
// still in the game answers two lines: its moves ("n from to" triples, or WAIT) and its purchases ("n zone" pairs at 20
// platinum a pod, on a free or own zone that is not a battlefield, or WAIT); a '#' or '\'' starts a message (30 chars,
// shown in the view). Invalid moves and purchases are skipped (listed as mistakes); a malformed line puts the player out.
// After the last player of a round: every move, then every purchase, then in each zone up to three fight rounds (every
// player there loses one pod per round), the last one standing owns the zone, then every player earns the platinum of
// its zones. A player with no pod, no income and no way to buy is eliminated when it next plays; a player out with no zone
// left raises everyone else's bonus by one. Score: zones owned + bonus. The game ends after 200 rounds, when no landmass
// can change hands any more, when the ranking can no longer change (each player's zones against the others' potential),
// or when every player is out.
//
// The Java referee is followed closely, notably: the map's HashMap<Coord, Tile> and every zone's HashSet<Integer> of
// neighbours (iteration orders by java::HashMapOrder, with the removals and re-insertions of the finale: they decide the
// islands' order and contents, hence where the platinum goes, and the order of the links in the init input, a
// HashSet<Edge>), the depth-first island search (java.util.Stack) and its sort (TimSort, by size), every draw of the
// java.util.Random (eager defaults included), the parsing of the two lines (String.trim, the message cut at the first '#'
// else '\'' and only on the second line when the first has none, equalsIgnoreCase("wait"), split("\\s+"),
// Integer.valueOf, the "trailing" errors, the purchases' error quoting the first line, moves accepted before a malformed
// triple still played, the mistakes grouped by type in insertion order), the move and purchase merges, the fights in
// zone order, the elimination test at the start of a player's turn, the extinction bonus, the end tests (islands that
// can change, ranks), the INFOS / SUMMARY / view texts in full (a frame's view: platinum, zones, score and message of
// every player, then on a new round the round's purchases, moves and fights).
// Math.pow: Distribution.value(p) = Math.pow(p, e) (e = 5, 1, 10, 0.4) only serves the platinum count
// 1 + (int)(value(p) * 5 + 0.5) of a zone, p = random.nextDouble() (a multiple of 2^-53). The jar's Math.pow is HotSpot's
// x86_64 intrinsic (Intel's LIBM stub, built on the CPU's rcpps approximation), not StrictMath's fdlibm: they differ in the
// last bit for about 10% of these p, and the count of one p differs (MOSTLY_LOW, p = 7154672683828276 / 2^53: 2 with
// Math.pow, 1 with StrictMath.pow). The count is a step function of p with five steps per distribution: this port
// compares p with the jar's own steps (PLATINUM_STEPS, found by a binary search over Math.pow on JDK 26 / Intel and
// checked on every p within 2^16 grid values of each step, also through the jar's own Distribution.value; further away
// no last-bit difference can change the count), which is exact for every p the Random can draw and the same on every
// platform.
// Collections: Coord and Edge hash by value (31 * (31 + a) + b). The map's HashMap<Coord, Tile> (the default map's
// coordinates have distinct hash codes) and every zone's HashSet<Integer> of neighbours are emulated with
// java::HashMapOrder. The links' HashSet<Edge> holds two links with the same hash code, 18-51 and 19-20 (1570), that
// share a tree bin while the set grows: Java orders them by their identity hash codes (HashMap.tieBreakOrder), which in
// general differ from run to run (the jar's own classes, built 2000 times in one JVM: either order, 1000 / 1000); the
// jar put 18-51 first at every launch measured (40, sequential and in parallel) and in every game validated, and so does
// this port (EdgeTieBreak, java::HashMapOrderT: the edge created first counts as the larger identity hash code). Should a
// loaded jar ever draw the other order, the init input's link list differs in those two lines (the validation build
// -DPLATINUM_RIFT_HINTS=1 takes the jar's identity hash codes from PR_EDGE_IDS="18 51 h1;19 20 h2", or from the
// reflection check). The players' captures in the summary (HashMap<Integer, ...>: player order), the ranked players (a
// HashSet<Integer> asked contains / size) and the base's tooltips (one per frame at most here) show no order of their own.
// Not reproduced / not modelled: the wall clock (see engine.h); the order of a frame's tooltips (never two here); the
// jar's identity hash order of the two links above if it ever changes; with maps given by the init property "map" (the
// CLI never passes one), coordinates or links colliding in a tree bin of their HashMap (identity hash order in Java;
// the edges follow the model above), and the helpful messages of the jar's NullPointerExceptions but the one an init
// error can show (a finale shift putting a zone on another one's coordinates); a jar on another CPU (whose rcpps may
// differ) might move a platinum step by one value of p.
//
// Validation (referees/README.md): referee-diff against the jar with 33 deterministic bots (2-, 3- and 4-player covering
// designs on four seed bases, every map variant: 3164 games, all identical, no replay needed; a 240-game spot check on a
// fifth base), direct runs comparing the jar's game logs with this port's trace (views, summaries, tooltips; raw bytes and
// line endings), the jar's own compiled classes driven by reflection over random properties and command sequences
// (242,000 cases, 31 of 32 mutations detected, the 32nd reached by the bots' games), the platinum steps against the jar's
// Distribution.value (2.6M values around the steps, 50M random), and 653 of CodinGame's replays (2, 3 and 4 players, every
// league) reproduced through this port's CLI from their seed alone.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/legacy.h"

#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

#ifndef PLATINUM_RIFT_HINTS
#define PLATINUM_RIFT_HINTS 0
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

const int MAX_ROUNDS = 200;
const int NEUTRAL = -1;
const int NOBODY = -2;
const int COMBAT_STRIKES = 3;
const double MAX_PLATINUM = 6;
const double RICH_AMERICA_PERCENTAGE = .55;
const int MAX_MESSAGE_LENGTH = 30;

const char16_t* const DEFAULT_MAP =
    u"0 2 1 1 2 1 2 2 2 3 2 4 2 5 3 1 3 2 3 3 3 4 3 5 3 6 4 1 4 2 4 3 4 4 4 5 4 7 5 0 5 1 5 2 5 3 5 4 5 7 5 8 5 9 6 1 6 2 6 3 "
    u"6 8 6 9 6 10 6 11 6 12 6 13 6 14 7 0 7 2 7 8 7 9 7 10 7 11 8 0 8 9 8 10 9 0 9 1 10 0 10 1 10 6 10 7 11 2 11 3 11 5 11 6 "
    u"11 7 11 14 12 1 12 3 12 5 12 6 12 7 12 8 12 9 12 10 12 11 12 14 13 1 13 2 13 3 13 5 13 6 13 7 13 8 13 9 13 10 13 11 "
    u"13 14 14 2 14 3 14 4 14 5 14 6 14 7 14 8 14 9 14 10 14 11 14 14 15 1 15 2 15 3 15 4 15 5 15 6 15 7 15 14 16 1 16 2 16 3 "
    u"16 4 16 5 16 6 16 14 17 0 17 1 17 2 17 3 17 4 17 5 17 6 17 7 17 14 18 1 18 2 18 3 18 4 18 5 18 6 19 1 19 2 19 3 19 4 "
    u"19 5 19 6 19 7 19 8 20 1 20 2 20 3 20 4 20 5 20 6 20 9 20 11 20 12 21 1 21 9 21 10 21 11 22 1 22 2 22 5 22 9 22 11 "
    u"22 12 22 13 23 1 23 3 23 4 23 9 23 13 24 13";

String S(i64 v) { return java::str(v); }

[[noreturn]] void npe() { throw java::Exception{"java.lang.NullPointerException"}; }

// Integer.valueOf / Long.valueOf: a NumberFormatException (a RuntimeException) when it fails
i32 parseIntOrThrow(const String& s) {
    i32 v;
    if (!java::parseInt(s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(s))};
    return v;
}
i64 parseLongOrThrow(const String& s) {
    i64 v;
    if (!java::parseLong(s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(s))};
    return v;
}

// Referee.Distribution (values() order) and the platinum count of distributePlatinum / generate:
// 1 + (int)(value(p) * (MAX_PLATINUM - 1) + 0.5) with value(p) = Math.pow(p, e), e = 5, 1, 10, 0.40
enum Distribution { LOWISH, HIGHISH, MOSTLY_LOW, MOSTLY_HIGH };
const char16_t* const DISTRIBUTION_NAMES[] = {u"LOWISH", u"HIGHISH", u"MOSTLY_LOW", u"MOSTLY_HIGH"};
// For p = i / 2^53 (every value nextDouble() returns), (int)(Math.pow(p, e) * 5 + 0.5) >= k exactly when i >= step k
// (k = 1..5), with the jar's Math.pow (see the header: HotSpot's intrinsic, JDK 26 on x86_64)
const i64 PLATINUM_STEPS[4][5] = {
    {5683158522975352LL, 7079686406810021LL, 7841222384935201LL, 8387052821294360LL, 8819384399738653LL}, // LOWISH, p^5
    {900719925474100LL, 2702159776422298LL, 4503599627370496LL, 6305039478318694LL, 8106479329266892LL},  // HIGHISH, p^1
    {7154672683828276LL, 7985495985048090LL, 8404014066019091LL, 8691596856817273LL, 8912797125066874LL}, // MOSTLY_LOW, p^10
    {28483264983953LL, 444010159058883LL, 1592262918131444LL, 3692622148059958LL, 6921433391100507LL},    // MOSTLY_HIGH, p^0.4
};
int platinumCount(Distribution d, double p) {
    i64 i = (i64)(p * 0x1.0p53); // nextDouble(): exactly i / 2^53
    int count = 1;
    for (i64 step : PLATINUM_STEPS[d])
        if (i >= step) count++;
    return count;
}

// Referee.Coord (equals / hashCode by value)
struct Coord {
    i32 x = 0, y = 0;
    bool operator<(const Coord& o) const { return x != o.x ? x < o.x : y < o.y; }
    i32 hashCode() const { return java::add(java::mul(31, java::add(31, x)), y); }
    bool isEven() const { return x % 2 == 0; }
    String toString() const { return S(x) + u" " + S(y); }
};

// A HashSet<Integer> (the neighbours of a zone): members, and the iteration order of the JDK's HashSet
struct IntSet {
    java::HashMapOrder order;
    std::vector<i32> members;
    bool contains(i32 v) const { return std::find(members.begin(), members.end(), v) != members.end(); }
    void add(i32 v) {
        if (!contains(v)) members.push_back(v);
        order.put(v, v);
    }
    void remove(i32 v) {
        auto it = std::find(members.begin(), members.end(), v);
        if (it == members.end()) return;
        members.erase(it);
        order.remove(v, v);
    }
    void clear() {
        members.clear();
        order.clear();
    }
    std::vector<i32> iterate() {
        std::vector<i32> k;
        order.keys(k);
        return k;
    }
};

// A HashMap<Coord, V>: the coordinates met get ids (the keys of a java::HashMapOrder), the values (tile indices) aside
struct CoordMap {
    java::HashMapOrder order;
    std::map<Coord, int> ids;
    std::vector<Coord> coords;
    std::vector<int> values; // -1: no mapping
    int idOf(const Coord& c) {
        auto it = ids.find(c);
        if (it != ids.end()) return it->second;
        int id = (int)coords.size();
        ids[c] = id;
        coords.push_back(c);
        values.push_back(-1);
        return id;
    }
    void put(const Coord& c, int tile) {
        int id = idOf(c);
        order.put(id, c.hashCode());
        values[(size_t)id] = tile;
    }
    // get(c): the tile index, -1 for null
    int get(const Coord& c) const {
        auto it = ids.find(c);
        return it == ids.end() ? -1 : values[(size_t)it->second];
    }
    void remove(const Coord& c) {
        auto it = ids.find(c);
        if (it == ids.end()) return;
        order.remove(it->second, c.hashCode());
        values[(size_t)it->second] = -1;
    }
    int size() const { return order.size; }
    std::vector<Coord> keySet() {
        std::vector<i32> k;
        order.keys(k);
        std::vector<Coord> r;
        for (i32 id : k) r.push_back(coords[(size_t)id]);
        return r;
    }
};

// Edge is not Comparable: two edges with the same hash code in a tree bin of the HashSet are ordered by
// HashMap.tieBreakOrder, i.e. by their identity hash codes (not reproducible in general). The default map has one such
// pair, the links 18-51 and 19-20 (hash code 1570): in the jar the edge created first (18-51) has the larger identity
// hash code at every launch measured (40 launches, sequential and in parallel; the jar's own classes driven by
// reflection in one JVM give either order), which puts 18-51 first in the init input. Keys: edge ids in creation order.
#if PLATINUM_RIFT_HINTS
// Validation only: the identity hash codes of the jar's Edge objects (System.identityHashCode, "a b hash;..." in the
// environment variable PR_EDGE_IDS, or set by the reflection check); the tie-break then follows them like tieBreakOrder.
std::map<std::pair<i32, i32>, i64> edgeIdentity;
const std::vector<std::pair<i32, i32>>* edgeRegistry = nullptr;
#endif
struct EdgeTieBreak {
    static constexpr bool enabled = true;
    // tieBreakOrder(k, pk): -1 when identityHashCode(k) <= identityHashCode(pk), i.e. here when k was created after pk
    int operator()(i32 k, i32 pk) const {
#if PLATINUM_RIFT_HINTS
        if (edgeRegistry) {
            auto a = edgeIdentity.find((*edgeRegistry)[(size_t)k]), b = edgeIdentity.find((*edgeRegistry)[(size_t)pk]);
            if (a != edgeIdentity.end() && b != edgeIdentity.end()) return a->second <= b->second ? -1 : 1;
        }
#endif
        return k > pk ? -1 : 1;
    }
};

// Referee.HexGrid.Edge (a <= b, equals / hashCode by value) in a HashSet<Edge>
struct EdgeSet {
    java::HashMapOrderT<EdgeTieBreak> order;
    std::map<std::pair<i32, i32>, int> ids;
    std::vector<std::pair<i32, i32>> edges;
    void add(i32 a, i32 b) {
#if PLATINUM_RIFT_HINTS
        edgeRegistry = &edges;
#endif
        std::pair<i32, i32> e{std::min(a, b), std::max(a, b)};
        auto it = ids.find(e);
        int id;
        if (it == ids.end()) {
            id = (int)edges.size();
            ids[e] = id;
            edges.push_back(e);
        } else {
            id = it->second;
        }
        order.put(id, java::add(java::mul(31, java::add(31, e.first)), e.second));
    }
    int size() const { return order.size; }
    std::vector<std::pair<i32, i32>> iterate() {
        std::vector<i32> k;
        order.keys(k);
        std::vector<std::pair<i32, i32>> r;
        for (i32 id : k) r.push_back(edges[(size_t)id]);
        return r;
    }
};

// Referee.Battle
struct Battle {
    Coord coord;
    std::optional<std::vector<int>> contenders; // null: no fight
    int capturer = -1, oldOwner = -1, hexId = 0, strikes = 0;
    int deaths[4] = {0, 0, 0, 0};
    bool hasFight() const { return contenders.has_value(); }
    bool hasCapturer() const { return capturer != -1; }
    bool hasActivity() const { return hasFight() || hasCapturer(); }
    String toString() const {
        String deathInfo;
        if (hasFight())
            for (int d : deaths) deathInfo += S(d) + u" ";
        return S(coord.x) + u" " + S(coord.y) + u" " + S(capturer) + u" " + (hasFight() ? u"1" : u"0") + u" " + java::trim(deathInfo);
    }
};

struct Move;
struct PlayerMoveSet;

// Referee.Tile
struct Tile {
    int platinum = 0, ownerId = -1, island = -1, id = 0, x = 0, y = 0;
    std::vector<int> units;
    bool contested = false;
    IntSet neighbours;

    Tile(int playerCount, int id_, const Coord& c) : id(id_), x(c.x), y(c.y) { units.assign((size_t)playerCount, 0); }

    String toStringInit() const { return S(platinum); }
    int getPodCountFor(int playerId, const PlayerMoveSet& moves) const;
    void remove(int amount, int pid) { units[(size_t)pid] = java::sub(units[(size_t)pid], amount); }
    void add(int amount, int pid) { units[(size_t)pid] = java::add(units[(size_t)pid], amount); }

    Battle update() {
        Battle battle;
        int newOwner = ownerId;
        std::vector<int> contenders;
        for (size_t i = 0; i < units.size(); i++)
            if (units[i] > 0) contenders.push_back((int)i);
        int contendersLeft = (int)contenders.size();
        if (contendersLeft > 1) {
            battle.contenders = contenders;
            int strikes = 0;
            int deaths[4] = {0, 0, 0, 0};
            bool resolved = false;
            while (!resolved) {
                for (int ind : contenders) {
                    if (units[(size_t)ind] > 0) {
                        if (ind >= 4) throw java::Exception{java::arrayIndexOutOfBoundsException(ind, 4)}; // (never: 4 players at most)
                        deaths[ind]++;
                        units[(size_t)ind] = std::max(0, units[(size_t)ind] - 1);
                        if (units[(size_t)ind] == 0) contendersLeft--;
                    }
                }
                strikes++;
                if (contendersLeft <= 1 || strikes == COMBAT_STRIKES) resolved = true;
            }
            battle.strikes = strikes;
            for (int k = 0; k < 4; k++) battle.deaths[k] = deaths[k];
        }
        if (contendersLeft == 1) {
            for (int ind : contenders) {
                if (units[(size_t)ind] != 0) {
                    newOwner = ind;
                    break;
                }
            }
        }
        if (contendersLeft == 1) {
            if (ownerId != newOwner) {
                battle.oldOwner = ownerId;
                battle.capturer = newOwner;
            }
            ownerId = newOwner;
            contested = false;
        } else if (contendersLeft > 1) {
            contested = true;
        } else {
            contested = false;
        }
        return battle;
    }

    int getEnemyCountFor(int playerId) const {
        int sum = 0;
        for (size_t i = 0; i < units.size(); i++)
            if ((int)i != playerId) sum = java::add(sum, units[i]);
        return sum;
    }

    String allUnits() const {
        String s;
        for (size_t i = 0; i < 4; ++i) s += units.size() > i ? S(units[i]) + u" " : String(u"0 ");
        return java::trim(s);
    }
};

// Referee.Move: a player's move of `amount` pods (the zones' coordinates and ids when it was made)
struct Move {
    int player = 0;
    int fromX = 0, fromY = 0, toX = 0, toY = 0, amount = 0, fromId = 0, toId = 0;
    Move(int p, const Tile& from, const Tile& to, int n) : player(p), fromX(from.x), fromY(from.y), toX(to.x), toY(to.y), amount(n), fromId(from.id), toId(to.id) {}
    String toString() const { return S(player) + u" " + S(amount) + u" " + S(fromX) + u" " + S(fromY) + u" " + S(toX) + u" " + S(toY); }
};

// Referee.Placement
struct Placement {
    int player = 0, amount = 0, x = 0, y = 0, id = 0;
    Placement(int p, int n, const Tile& t) : player(p), amount(n), x(t.x), y(t.y), id(t.id) {}
    String toString() const { return S(player) + u" " + S(amount) + u" " + S(x) + u" " + S(y); }
};

// Referee.PlayerMoveSet: a move with the same zones (coordinates) and player adds its amount to the one already there
struct PlayerMoveSet {
    std::vector<Move> moves;
    void add(const Move& m) {
        for (Move& move : moves) {
            if (move.fromX == m.fromX && move.fromY == m.fromY && move.toX == m.toX && move.toY == m.toY && move.player == m.player) {
                move.amount = java::add(move.amount, m.amount);
                return;
            }
        }
        moves.push_back(m);
    }
    void clear() { moves.clear(); }
};

// Referee.PlayerPlacementSet
struct PlayerPlacementSet {
    std::vector<Placement> placements;
    void add(const Placement& p) {
        for (Placement& placement : placements) {
            if (placement.x == p.x && placement.y == p.y && placement.player == p.player) {
                placement.amount = java::add(placement.amount, p.amount);
                return;
            }
        }
        placements.push_back(p);
    }
    void clear() { placements.clear(); }
};

int Tile::getPodCountFor(int playerId, const PlayerMoveSet& moves) const {
    int now = units[(size_t)playerId];
    int moveAways = 0;
    for (const Move& m : moves.moves)
        if (m.fromId == id) moveAways = java::add(moveAways, m.amount);
    return java::sub(now, moveAways);
}

// Referee.Mistake
struct Mistake {
    String message;
    int count = 0;
};

// Referee.Player
struct Player {
    int platinum = 0, id = 0, deadAt = 0, speechHex = 0, speechHexNext = -1, expenses = 0, advantage = 0;
    bool dead = false, timeout = false, eliminated = false, extinct = false;
    NString speech;
    int lastMoves = 0, lastPlaces = 0;
    std::vector<String> lastActions;
    std::vector<std::pair<String, Mistake>> mistakes; // LinkedHashMap<String, Mistake>: insertion order

    void setDead(int round) {
        deadAt = round;
        dead = true;
    }
    bool hasSpeech() const { return speech.has_value(); }
    void setSpeech(const String& m, const Tile& t) {
        speech = m;
        speechHex = t.id;
        speechHexNext = -1;
    }
    void setSpeech(const String& message, const Tile& from, const Tile& to) {
        speech = message;
        speechHex = from.id;
        speechHexNext = to.id;
    }
    void reset() {
        speech.reset();
        lastActions.clear();
        lastMoves = 0;
        lastPlaces = 0;
        mistakes.clear();
        speechHexNext = -1;
        expenses = 0;
    }
    void setTimeout(int turn) {
        setDead(turn);
        timeout = true;
    }
    void addMistake(const String& type, const String& message) {
        for (auto& m : mistakes) {
            if (m.first == type) {
                m.second.count++;
                return;
            }
        }
        mistakes.push_back({type, Mistake{message, 0}});
    }
    void eliminate(int turn) {
        eliminated = true;
        setDead(turn);
    }
};

// Referee.InvalidMoveOrPlacementException: a type (a format) and its replacements; toString() quotes the action
struct InvalidMoveOrPlacementException {
    String type;
    std::vector<Arg> replacements;
};

// Referee.HexGrid
struct HexGrid {
    int width = 0, height = 0;
    CoordMap map;
    int zones[5] = {0, 0, 0, 0, 0};
    int units[4] = {0, 0, 0, 0};
    int income[4] = {0, 0, 0, 0};
    std::vector<std::vector<Coord>> islands;
    std::vector<Coord> aroundEven{{0, -1}, {1, 0}, {0, 1}, {-1, 0}, {1, -1}, {-1, -1}};
    std::vector<Coord> aroundOdd{{0, -1}, {1, 0}, {0, 1}, {-1, 0}, {-1, 1}, {1, 1}};
    std::vector<Tile> graph;
    std::optional<std::vector<Coord>> america;
    std::optional<EdgeSet> edges;

    // graph.get(i) (an ArrayList)
    Tile& at(i64 i) {
        if (i < 0 || i >= (i64)graph.size()) throw java::Exception{java::indexOutOfBoundsException(i, (i64)graph.size())};
        return graph[(size_t)i];
    }
    // get(x, y) / get(coord): the zone at these coordinates, nullptr for null
    Tile* get(const Coord& c) {
        int t = map.get(c);
        return t < 0 ? nullptr : &graph[(size_t)t];
    }
    // get(id): the zone of this id, nullptr for null
    Tile* get(i64 id) { return id >= 0 && id < (i64)graph.size() ? &graph[(size_t)id] : nullptr; }

    static int arrayIndex(int i, int length) {
        if (i < 0 || i >= length) throw java::Exception{java::arrayIndexOutOfBoundsException(i, length)};
        return i;
    }

    void generate(int playerCount, const String& mapData, int totalPlatinum, Distribution distribution, bool noEmptyIslands, bool richAmericas,
                  java::Random& random, bool finale) {
        int platinum = totalPlatinum;
        std::vector<int> allTiles; // LinkedList<Tile>
        std::vector<String> tileData = java::split(mapData, u' ');
        size_t tileCount = tileData.size() / 2;
        for (size_t i = 0; i < tileCount; i++) {
            i32 x = parseIntOrThrow(tileData[i * 2]);
            i32 y = parseIntOrThrow(tileData[i * 2 + 1]);
            if (java::add(x, 1) > width) width = java::add(x, 1);
            if (java::add(y, 1) > height) height = java::add(y, 1);
            Coord c{x, y};
            graph.emplace_back(playerCount, (int)i, c);
            map.put(c, (int)i);
            allTiles.push_back((int)i);
            zones[0]++;
        }

        initIslands(false);
        if (noEmptyIslands) {
            for (const std::vector<Coord>& island : islands) {
                Tile* t = get(island[(size_t)random.nextInt((i32)island.size())]);
                double percent = random.nextDouble();
                int count = platinumCount(distribution, percent);
                count = std::min(platinum, count);
                count = std::min((int)MAX_PLATINUM, count);
                count = std::max(1, count);
                platinum -= count;
                if (!t) npe();
                t->platinum = count;
            }
        }

        if (richAmericas && america) {
            std::vector<int> americanTiles;
            int americanPlatinum = java::d2i(platinum * RICH_AMERICA_PERCENTAGE);
            platinum -= americanPlatinum;
            for (const Coord& c : *america) {
                int t = map.get(c);
                if (t >= 0) {
                    auto it = std::find(allTiles.begin(), allTiles.end(), t);
                    if (it != allTiles.end()) allTiles.erase(it);
                }
                americanTiles.push_back(t);
            }
            distributePlatinum(americanPlatinum, americanTiles, distribution, random);
        }
        distributePlatinum(platinum, allTiles, distribution, random);
        if (finale) {
            shiftPlates();
            islands.clear();
            for (Tile& t : graph) t.island = -1;
            initIslands(true);
        }
    }

    void shiftPlates() {
        const int suezAfrica[] = {71, 83, 95};
        const int suezAsia[] = {82, 94, 103};
        for (int i : suezAfrica) {
            for (int j : suezAsia) {
                Tile& t1 = at(i);
                Tile& t2 = at(j);
                if (t1.neighbours.contains(t2.id)) {
                    t1.neighbours.remove(t2.id);
                    t2.neighbours.remove(t1.id);
                }
            }
        }

        const int africa[] = {50, 51, 54, 55, 56, 60, 61, 62, 63, 64, 65, 66, 71, 72, 73, 74, 75, 76, 77, 83, 84, 85, 86, 87, 88, 95, 96};
        CoordMap newMap;
        for (int id : africa) {
            Tile& t = at(id);
            map.remove(Coord{t.x, t.y});
            t.y = java::add(t.y, 1);
            newMap.put(Coord{t.x, t.y}, id);
        }
        for (const Coord& c : newMap.keySet()) map.put(c, newMap.get(c));

        Tile& panama = at(18);
        map.remove(Coord{panama.x, panama.y});
        panama.x = 9;
        panama.y = 8;
        map.put(Coord{panama.x, panama.y}, 18);
        for (i32 i : panama.neighbours.iterate()) at(i).neighbours.remove(panama.id);
        panama.neighbours.clear();
        panama.neighbours.add(44);
        panama.neighbours.add(51);
        at(44).neighbours.add(18);
        at(51).neighbours.add(18);
    }

    void distributePlatinum(int total, std::vector<int> tiles, Distribution method, java::Random& random) {
        std::vector<int> discardedTiles;
        int platinum = total;
        while (platinum > 0 && !tiles.empty()) {
            int t = tiles[(size_t)random.nextInt((i32)tiles.size())];
            double percent = random.nextDouble();
            int count = platinumCount(method, percent);
            count = std::min(platinum, count);
            count = std::min((int)MAX_PLATINUM, count);
            count = std::max(1, count);
            platinum -= count;
            if (t < 0) npe();
            graph[(size_t)t].platinum = count;
            tiles.erase(std::find(tiles.begin(), tiles.end(), t));
            discardedTiles.push_back(t);
        }
        while (platinum > 0 && !discardedTiles.empty()) {
            int t = discardedTiles[(size_t)random.nextInt((i32)discardedTiles.size())];
            if (graph[(size_t)t].platinum < 6) {
                graph[(size_t)t].platinum++;
                platinum--;
            } else {
                discardedTiles.erase(std::find(discardedTiles.begin(), discardedTiles.end(), t));
            }
        }
    }

    void initIslands(bool asGraph) {
        int currentIsland = 0;
        std::vector<Coord> islandData;
        for (const Coord& c : map.keySet()) {
            Tile* t = get(c);
            if (t->island == -1) {
                std::vector<Coord> stack; // java.util.Stack
                stack.push_back(c);
                get(c)->island = currentIsland;
                while (!stack.empty()) {
                    Coord hex = stack.back();
                    stack.pop_back();
                    Tile* tile = get(hex);
                    islandData.push_back(hex);
                    if (asGraph) {
                        // (a custom map whose finale shift put a zone on another one's coordinates: the JDK's helpful
                        // message, which the init error shows; the jar is compiled with its local variable names)
                        if (!tile) throw java::Exception{"java.lang.NullPointerException: Cannot read field \"neighbours\" because \"tile\" is null"};
                        for (i32 i : tile->neighbours.iterate()) {
                            Tile& nextTile = at(i);
                            Coord next{nextTile.x, nextTile.y};
                            if (nextTile.island == -1) {
                                nextTile.island = currentIsland;
                                stack.push_back(next);
                            }
                        }
                    } else {
                        const std::vector<Coord>& around = hex.isEven() ? aroundEven : aroundOdd;
                        for (const Coord& neigh : around) {
                            Coord next{java::add(hex.x, neigh.x), java::add(hex.y, neigh.y)};
                            Tile* nextTile = get(next);
                            if (nextTile) {
                                if (!tile) npe();
                                tile->neighbours.add(nextTile->id);
                                nextTile->neighbours.add(tile->id);
                                if (nextTile->island == -1) {
                                    nextTile->island = currentIsland;
                                    stack.push_back(next);
                                }
                            }
                        }
                    }
                }
                islands.push_back(islandData);
                islandData.clear();
                currentIsland++;
            }
        }
        java::sort(islands, [](const std::vector<Coord>& a, const std::vector<Coord>& b) { return (int)b.size() - (int)a.size(); });
        if (islands.size() > 1) america = islands[1];
        else if (islands.size() > 0) america = islands[0];
    }

    Lines toStringInitAsMap() {
        Lines data;
        data.push_back(S(width) + u" " + S(height) + u" " + S(map.size()));
        for (const Coord& c : map.keySet()) {
            Tile* t = get(c);
            data.push_back(c.toString() + u" " + t->toStringInit() + u" " + S(t->id));
        }
        return data;
    }

    Lines toStringInitAsGraph() {
        if (!edges) getLinkCount();
        Lines data;
        for (const Tile& t : graph) data.push_back(S(t.id) + u" " + S(t.platinum));
        for (const auto& e : edges->iterate()) data.push_back(S(e.first) + u" " + S(e.second));
        return data;
    }

    std::vector<Battle> update() {
        std::vector<Battle> battleInfos;
        for (size_t i = 0; i < graph.size(); i++) {
            Tile& t = graph[i];
            Battle battleInfo = t.update();
            if (battleInfo.hasActivity()) {
                if (battleInfo.hasCapturer()) {
                    incZones(battleInfo.capturer);
                    decZones(battleInfo.oldOwner);
                    income[arrayIndex(battleInfo.capturer, 4)] = java::add(income[battleInfo.capturer], t.platinum);
                    if (battleInfo.oldOwner >= 0) income[arrayIndex(battleInfo.oldOwner, 4)] = java::sub(income[battleInfo.oldOwner], t.platinum);
                }
                if (battleInfo.hasFight())
                    for (int ind : *battleInfo.contenders) units[arrayIndex(ind, 4)] = java::sub(units[ind], battleInfo.deaths[ind]);
                battleInfo.coord = Coord{t.x, t.y};
                battleInfo.hexId = (int)i;
                battleInfos.push_back(battleInfo);
            }
        }
        return battleInfos;
    }

    void place(int id, int bought, int x, int y) {
        Tile* t = get(Coord{x, y});
        if (!t) npe();
        t->add(bought, id);
        units[arrayIndex(id, 4)] = java::add(units[id], bought);
    }

    void decZones(int id) { zones[arrayIndex(id + 1, 5)]--; }
    void incZones(int id) { zones[arrayIndex(id + 1, 5)]++; }
    int getZones(int id) { return zones[arrayIndex(id + 1, 5)]; }

    int getIncomeFor(int playerId) { return income[arrayIndex(playerId, 4)]; }

    bool isPlayerDead(const Player& player, int unitCost) {
        int id = player.id;
        if (units[arrayIndex(id, 4)] > 0) return false;
        if (player.platinum >= unitCost && (getZones(NEUTRAL) > 0 || getZones(id) > 0)) return false;
        if (income[id] > 0) return false;
        return true;
    }

    bool adjacent(i64 sid, i64 did) {
        Tile* a = get(sid);
        Tile* b = get(did);
        if (!a || !b) return false;
        return a->neighbours.contains((i32)did);
    }

    int getLinkCount() {
        if (edges) return edges->size();
        edges.emplace();
        for (Tile& t : graph)
            for (i32 i : t.neighbours.iterate()) edges->add(t.id, i);
        return edges->size();
    }

    int getZonesIn(int playerId, const std::vector<Coord>& island) {
        int count = 0;
        for (const Coord& hex : island) {
            Tile* t = get(hex);
            if (!t) npe();
            if (t->ownerId == playerId) count++;
        }
        return count;
    }
};

struct Referee : legacy::MultiReferee {
    // the static fields of the Java referee (one game per JVM in the jar)
    i32 START_PLATINUM = 200;
    i32 UNIT_COST = 20;
    bool finale = true;
    java::Random random;

    HexGrid map;
    i64 seed = 0;
    std::vector<Player> players;
    bool playersCreated = false; // players is null until the end of initReferee
    PlayerMoveSet playerMoves;
    PlayerPlacementSet playerPlacements;
    std::vector<Battle> battles;
    std::vector<Placement> playerPlacementsForView;
    std::vector<Move> playerMovesForView;
    i32 totalPlatinum = 0;
    String mapData;
    Distribution distribution = LOWISH;
    bool noEmptyIslands = false, richAmericas = false;

    Referee() { base = legacy::Base::platinumRift2014(); }

    String getGameName() override { return u"PlatinumRift"; }

    void initReferee(int playerCount, java::Properties& prop) override {
#if PLATINUM_RIFT_HINTS
        if (const char* ids = getenv("PR_EDGE_IDS")) {
            edgeIdentity.clear();
            std::string text(ids);
            for (size_t at = 0; at < text.size();) {
                size_t end = text.find(';', at);
                if (end == std::string::npos) end = text.size();
                long long a = 0, b = 0, h = 0;
                if (sscanf(text.substr(at, end - at).c_str(), "%lld %lld %lld", &a, &b, &h) == 3) edgeIdentity[{(i32)a, (i32)b}] = h;
                at = end + 1;
            }
        }
#endif
        // the default of "seed" (new Random(System.currentTimeMillis()).nextLong()) is computed whether it is given or not
        String defaultSeed = S(java::Random((i64)java::entropy()).nextLong());
        seed = parseLongOrThrow(prop.getProperty(u"seed", defaultSeed));
        random = java::Random(seed);

        // the defaults are drawn whether the properties are given or not
        noEmptyIslands = java::parseBoolean(prop.getProperty(u"noEmptyIslands", random.nextBoolean() ? u"true" : u"false"));
        richAmericas = java::parseBoolean(prop.getProperty(u"richAmericas", random.nextBoolean() ? u"true" : u"false"));
        String algoName = prop.getProperty(u"distribution", DISTRIBUTION_NAMES[random.nextInt(4)]);
        int d = -1;
        for (int k = 0; k < 4; k++)
            if (algoName == DISTRIBUTION_NAMES[k]) d = k;
        if (d < 0) d = random.nextInt(4); // Distribution.valueOf: an IllegalArgumentException, caught
        distribution = (Distribution)d;
        finale = java::parseBoolean(prop.getProperty(u"finale", u"true"));
        totalPlatinum = parseIntOrThrow(prop.getProperty(u"platinum", u"120"));
        START_PLATINUM = parseIntOrThrow(prop.getProperty(u"startPlatinum", S(START_PLATINUM)));
        UNIT_COST = parseIntOrThrow(prop.getProperty(u"unitCost", S(UNIT_COST)));

        mapData = prop.getProperty(u"map", DEFAULT_MAP);

        map = HexGrid();
        map.generate(playerCount, mapData, totalPlatinum, distribution, noEmptyIslands, richAmericas, random, finale);

        playerMoves.clear();
        playerPlacements.clear();
        battles.clear();
        playerMovesForView.clear();
        playerPlacementsForView.clear();

        players.clear();
        for (int i = 0; i < playerCount; i++) {
            Player p;
            p.id = i;
            p.platinum = START_PLATINUM;
            players.push_back(p);
        }
        playersCreated = true;
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        prop.setProperty(u"distribution", DISTRIBUTION_NAMES[distribution]);
        prop.setProperty(u"richAmericas", richAmericas ? u"true" : u"false");
        prop.setProperty(u"noEmptyIslands", noEmptyIslands ? u"true" : u"false");
        prop.setProperty(u"finale", finale ? u"true" : u"false");
        prop.setProperty(u"platinum", S(totalPlatinum));
        prop.setProperty(u"startPlatinum", S(START_PLATINUM));
        prop.setProperty(u"unitCost", S(UNIT_COST));
        prop.setProperty(u"map", mapData);
        return prop;
    }

    void populateMessages(std::map<std::string, String>& p) override {
        p["wait"] = u"$%d bides their time.";
        p["dead"] = u"$%d no longer plays in this game.";
        p["move"] = u"$%d moved %d pod%s from %d to %d.";
        p["groupMove"] = u"$%d moved %d pod%s.";
        p["groupPlace"] = u"$%d spawned %d pod%s.";
        p["place"] = u"$%d spawned %d pod%s at %d.";
        p["status"] = u"$%d score: %d.";
        p["battle"] = u"The battle at %d has cost %s up to three pods each.";
        p["capture"] = u"$%d has captured %s!";
    }

    Lines getInitDataForView() override {
        if (!playersCreated) npe(); // players.size() of a null list: not caught by the base, the referee thread dies
        String sb = S((i64)players.size()) + u" ";
        for (const Player& p : players) sb += S(p.platinum) + u" ";
        Lines data{java::trim(sb)};
        Lines m = map.toStringInitAsMap();
        data.insert(data.end(), m.begin(), m.end());
        return data;
    }

    Lines getFrameDataForView(int, int, int, bool keyFrame) override {
        if (!playersCreated) npe();
        Lines data;
        for (const Player& p : players) {
            Tile* t = map.get((i64)p.speechHex);
            int nx = -1, ny = -1;
            if (p.speechHexNext >= 0) {
                Tile* next = map.get((i64)p.speechHexNext);
                if (!next) npe();
                nx = next->x;
                ny = next->y;
            }
            if (!t) npe();
            data.push_back(S(p.platinum) + u" " + S(getZones(p.id)) + u" " + S(getScore(p.id)) + u" " + (p.hasSpeech() ? u"1" : u"0") + u" " + S(t->x) + u" " +
                           S(t->y) + u" " + S(nx) + u" " + S(ny));
            if (p.hasSpeech()) data.push_back(java::trim(*p.speech));
        }
        if (!keyFrame) {
            data.push_back(u"0");
            data.push_back(u"0");
        } else {
            data.push_back(S((i64)playerPlacementsForView.size()));
            for (const Placement& placement : playerPlacementsForView) data.push_back(placement.toString());
            data.push_back(S((i64)playerMovesForView.size()));
            for (const Move& move : playerMovesForView) data.push_back(move.toString());
            data.push_back(S((i64)battles.size()));
            for (const Battle& battle : battles) data.push_back(battle.toString());
            playerPlacementsForView.clear();
            playerMovesForView.clear();
        }
        return data;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 2; }

    // parsePlayerMessage(output, line): the text after the first '#' (else the first '\''), cut to 30 chars and trimmed
    // (null when empty), the line cut before it and trimmed
    static NString parsePlayerMessage(std::vector<String>& output, size_t line) {
        NString userMessage;
        size_t quoteIdx = output[line].find(u'#');
        if (quoteIdx == String::npos) quoteIdx = output[line].find(u'\'');
        if (quoteIdx != String::npos) {
            size_t lastQuoteIndex = output[line].size();
            lastQuoteIndex = std::min(quoteIdx + 1 + MAX_MESSAGE_LENGTH, lastQuoteIndex);
            if (quoteIdx + 1 < lastQuoteIndex) {
                userMessage = java::trim(output[line].substr(quoteIdx + 1, lastQuoteIndex - (quoteIdx + 1)));
                if (userMessage->empty()) userMessage.reset();
            }
            output[line] = java::trim(output[line].substr(0, quoteIdx));
        }
        return userMessage;
    }

    // InvalidMoveOrPlacementException#toString with its action
    static String mistakeText(const String& action, const InvalidMoveOrPlacementException& e) {
        return java::format(u"\"%s\" is an invalid action: %s", {Arg(action), Arg(java::format(e.type, e.replacements))});
    }

    void handlePlayerOutput(int turn, int, int playerId, const Lines& outputs) override {
        Player& player = players[(size_t)playerId];
        if (map.isPlayerDead(player, UNIT_COST)) player.eliminate(turn); // (the turn goes on)

        std::vector<String> output = outputs;
        if (output.size() < 2) throw java::Exception{java::arrayIndexOutOfBoundsException((i64)output.size(), (i64)output.size())}; // (never: 2 lines)
        output[0] = java::trim(output[0]);
        output[1] = java::trim(output[1]);
        NString moveMessage = parsePlayerMessage(output, 0);
        NString placeMessage;
        if (!moveMessage) placeMessage = parsePlayerMessage(output, 1);
        std::optional<std::vector<String>> moveData;
        if (!java::equalsIgnoreCase(output[0], "wait") && !output[0].empty()) moveData = java::splitRegexSpaces(output[0]);
        std::optional<std::vector<String>> placeData;
        if (!java::equalsIgnoreCase(output[1], "wait") && !output[1].empty()) placeData = java::splitRegexSpaces(output[1]);

        PlayerMoveSet frameMoves;
        PlayerPlacementSet framePlacements;

        if (moveData) {
            const std::vector<String>& md = *moveData;
            size_t movel = md.size() / 3 * 3;
            if (md.size() < 3) {
                player.setDead(turn);
                throw legacy::InvalidInputException(u"x y z | \"WAIT\"", output[0]);
            }
            if (md.size() % 3 != 0) {
                String extra;
                for (size_t i = movel; i < md.size(); i++) extra += md[i] + u' ';
                extra.pop_back();
                player.setDead(turn);
                throw legacy::InvalidInputException(u"multiple of 3 integers | \"WAIT\"", u"trailing " + extra);
            }

            Tile* msgFrom = nullptr;
            Tile* msgTo = nullptr;
            for (size_t move = 0; move < movel;) {
                const String& n1 = md[move++];
                const String& n2 = md[move++];
                const String& n3 = md[move++];
                i32 n, sid, did;
                if (!java::parseInt(n1, n) || !java::parseInt(n2, sid) || !java::parseInt(n3, did)) {
                    player.setDead(turn);
                    throw legacy::InvalidInputException(u"x y z | \"WAIT\"", n1 + u" " + n2 + u" " + n3);
                }
                try {
                    if (n <= 0) throw InvalidMoveOrPlacementException{u"You can't move zero pods", {}};
                    Tile* from = map.get((i64)sid);
                    if (!from) throw InvalidMoveOrPlacementException{u"Source hex %d is not part of the map", {sid}};
                    int podCount = from->getPodCountFor(playerId, frameMoves);
                    if (podCount < n) throw InvalidMoveOrPlacementException{u"There aren't enough pods (%d instead of %d) on hex %d", {podCount, n, sid}};
                    Tile* to = map.get((i64)did);
                    if (!to) throw InvalidMoveOrPlacementException{u"Destination hex %d is not part of the map", {did}};
                    if (!map.adjacent(sid, did)) throw InvalidMoveOrPlacementException{u"Destination hex %d is not adjacent to source hex %d", {did, sid}};
                    if (from->contested && to->ownerId != player.id && to->ownerId != -1)
                        throw InvalidMoveOrPlacementException{u"Cannot retreat from  %d to %d as destination is controlled by enemy", {sid, did}};
                    msgFrom = from;
                    msgTo = to;
                    playerMoves.add(Move(playerId, *from, *to, n));
                    frameMoves.add(Move(playerId, *from, *to, n));
                } catch (const InvalidMoveOrPlacementException& e) {
                    player.addMistake(e.type, mistakeText(n1 + u" " + n2 + u" " + n3, e));
                    continue;
                }
            }
            if (moveMessage && msgFrom && msgTo) player.setSpeech(*moveMessage, *msgFrom, *msgTo);
        }

        for (const Move& move : frameMoves.moves) {
            playerMovesForView.push_back(move);
            player.lastMoves = java::add(player.lastMoves, move.amount);
            player.lastActions.push_back(translate("move", {playerId, move.amount, String(move.amount > 1 ? u"s" : u""), move.fromId, move.toId}));
        }
        frameMoves.clear();

        if (placeData) {
            const std::vector<String>& pd = *placeData;
            Tile* msgFrom = nullptr;
            size_t placel = pd.size();
            if (pd.size() < 2) {
                player.setDead(turn);
                throw legacy::InvalidInputException(u"x y | \"WAIT\"", output[0]); // (sic: the first line)
            }
            if (pd.size() % 2 != 0) {
                player.setDead(turn);
                throw legacy::InvalidInputException(u"multiple of 2 integers | \"WAIT\"", u"trailing " + pd[placel - 1]);
            }
            for (size_t place = 0; place < placel;) {
                const String& n1 = pd[place++];
                const String& n2 = pd[place++];
                i32 n, id;
                if (!java::parseInt(n1, n) || !java::parseInt(n2, id)) {
                    player.setDead(turn);
                    throw legacy::InvalidInputException(u"x y | \"WAIT\"", n1 + u" " + n2);
                }
                try {
                    if (n <= 0) throw InvalidMoveOrPlacementException{u"Expected n > 0 but got n = %d", {n}};
                    Tile* t = map.get((i64)id);
                    if (!t) throw InvalidMoveOrPlacementException{u"Hex %d is not part of the map", {id}};
                    if (t->ownerId != playerId && t->ownerId != -1) throw InvalidMoveOrPlacementException{u"Hex %d is owned by another player", {id}};
                    if (t->ownerId == -1 && t->contested) throw InvalidMoveOrPlacementException{u"Hex %d is a battlefield", {id}};
                    // how many pods the player can afford (int arithmetic, like the Java loop)
                    bool buyNextPod = true;
                    i32 expenses = 0;
                    i32 bought = 0;
                    while (buyNextPod) {
                        if (java::sub(java::sub(player.platinum, player.expenses), expenses) >= UNIT_COST) {
                            bought++;
                            expenses = java::add(expenses, UNIT_COST);
                            if (bought >= n) buyNextPod = false;
                        } else {
                            buyNextPod = false;
                        }
                    }
                    player.expenses = java::add(player.expenses, expenses);
                    if (bought > 0) {
                        playerPlacements.add(Placement(playerId, bought, *t));
                        framePlacements.add(Placement(playerId, bought, *t));
                        msgFrom = t;
                    } else {
                        throw InvalidMoveOrPlacementException{u"Buyer can't afford reinforcements", {}};
                    }
                } catch (const InvalidMoveOrPlacementException& e) {
                    player.addMistake(e.type, mistakeText(n1 + u" " + n2, e));
                    continue;
                }
            }
            if (placeMessage && msgFrom) player.setSpeech(*placeMessage, *msgFrom);
        }

        for (const Placement& place : framePlacements.placements) {
            playerPlacementsForView.push_back(place);
            player.lastPlaces = java::add(player.lastPlaces, place.amount);
            player.lastActions.push_back(translate("place", {playerId, place.amount, String(place.amount > 1 ? u"s" : u""), place.id}));
        }
        framePlacements.clear();
    }

    Lines getInitInputForPlayer(int player) override {
        Lines data;
        data.push_back(S((i64)players.size()) + u" " + S(player) + u" " + S((i64)map.graph.size()) + u" " + S(map.getLinkCount()));
        Lines g = map.toStringInitAsGraph();
        data.insert(data.end(), g.begin(), g.end());
        return data;
    }

    Lines getInputForPlayer(int, int player) override {
        Lines data;
        data.push_back(S(players[(size_t)player].platinum));
        for (const Tile& t : map.graph) data.push_back(S(t.id) + u" " + S(t.ownerId) + u" " + t.allUnits());
        return data;
    }

    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 1; }

    void updateGame(int round) override {
        for (const Move& move : playerMoves.moves) {
            Tile* from = map.get(Coord{move.fromX, move.fromY});
            Tile* to = map.get(Coord{move.toX, move.toY});
            if (!from) npe();
            from->remove(move.amount, move.player);
            if (!to) npe();
            to->add(move.amount, move.player);
        }
        playerMoves.clear();

        for (const Placement& move : playerPlacements.placements) map.place(move.player, move.amount, move.x, move.y);
        playerPlacements.clear();

        battles = map.update();

        for (Player& p : players) {
            p.platinum = java::sub(p.platinum, p.expenses);
            p.platinum = java::add(p.platinum, map.getIncomeFor(p.id));
        }

        detectExtinction(round);
        detectEndOfGame();
    }

    void detectExtinction(int) {
        bool needUpdate = false;
        for (size_t i = 0; i < players.size(); ++i) {
            int zones = map.getZones((int)i);
            Player& p = players[i];
            if (zones == 0 && p.dead && !p.extinct) {
                p.extinct = true;
                needUpdate = true;
            }
        }
        if (needUpdate)
            for (Player& player : players)
                if (!player.extinct && !player.timeout) player.advantage++;
    }

    void detectEndOfGame() {
        size_t n = players.size();
        std::vector<int> owned(n, 0), potential(n, 0);
        std::vector<bool> canChange(map.islands.size(), false);

        for (size_t i = 0, il = map.islands.size(); i < il; i++) {
            const std::vector<Coord>& island = map.islands[i];
            int islandSize = (int)island.size();
            Tile* first = map.get(island[0]);
            if (!first) npe();
            int playerId = first->ownerId;
            bool isOwner = playerId != NEUTRAL;
            std::vector<bool> possibles(n, false);
            canChange[i] = true;
            for (const Coord& c : island) {
                Tile* t = map.get(c);
                if (!t) npe();
                if (isOwner && (t->ownerId != playerId || t->getEnemyCountFor(playerId) > 0)) isOwner = false;
                if (t->ownerId != NEUTRAL) {
                    possibles[(size_t)t->ownerId] = true;
                } else {
                    for (size_t p = 0; p < n; p++) possibles[p] = true;
                    break;
                }
                if (t->contested)
                    for (size_t p = 0, pl = t->units.size(); p < pl; p++)
                        if (t->units[p] > 0) possibles[p] = true;
            }
            int aliveContesters = 0;
            int deadContesters = 0;
            for (size_t j = 0; j < n; j++) {
                if (possibles[j]) {
                    if (!players[j].dead) aliveContesters++;
                    else deadContesters++;
                }
            }
            if (aliveContesters == 0 || aliveContesters + deadContesters <= 1) canChange[i] = false;

            int soloPlayer = NOBODY;
            for (const Player& p : players) {
                if (possibles[(size_t)p.id] && !p.dead) {
                    if (soloPlayer == NOBODY) {
                        soloPlayer = p.id;
                    } else {
                        soloPlayer = NOBODY;
                        break;
                    }
                }
            }

            for (size_t p = 0; p < n; p++) {
                if (possibles[p]) {
                    if (players[p].dead) potential[p] += map.getZonesIn((int)p, island);
                    else potential[p] += islandSize;
                }
            }

            if (isOwner) owned[(size_t)playerId] += islandSize;
            else if (soloPlayer != NOBODY) owned[(size_t)soloPlayer] += map.getZonesIn(soloPlayer, island);
        }

        bool gameFrozen = true;
        for (bool b : canChange) {
            if (b) {
                gameFrozen = false;
                break;
            }
        }
        if (gameFrozen) throw legacy::GameOverException(std::nullopt);

        int sizeToRank = (int)n - 1;
        for (size_t p = 0; p < n; p++)
            if (potential[p] == 0) sizeToRank--;
        if (sizeToRank == 0) throw legacy::GameOverException(std::nullopt);

        // Set<Integer> ranked: only contains / add / size
        std::vector<bool> ranked(n, false);
        int rankedSize = 0;
        bool redo = false;
        do {
            redo = false;
            for (size_t p = 0; p < n; p++) {
                if (ranked[p]) continue;
                bool master = true;
                for (size_t enemy = 0; enemy < n; enemy++) {
                    if (p == enemy || ranked[enemy]) continue;
                    if (owned[p] <= potential[enemy]) master = false;
                }
                if (master) {
                    redo = true;
                    ranked[p] = true;
                    rankedSize++;
                    break;
                }
            }
            if (rankedSize == sizeToRank) break;
        } while (redo);

        if (rankedSize == sizeToRank) throw legacy::GameOverException(std::nullopt);
    }

    void prepare(int) override {
        for (Player& p : players) p.reset();
    }

    bool isPlayerDead(int player) override { return players[(size_t)player].dead; }

    int getScore(int player) override { return java::add(getZones(player), players[(size_t)player].advantage); }
    int getZones(int player) { return map.getZones(player); }

    Lines getGameSummary(int) override {
        Lines data;
        // HashMap<Integer, List<Integer>> captures: player ids 0..3, iterated in ascending order
        std::map<int, std::vector<int>> captures;
        for (const Battle& battle : battles)
            if (battle.hasCapturer()) captures[battle.capturer].push_back(battle.hexId);
        for (const auto& c : captures) {
            const std::vector<int>& captured = c.second;
            String sb;
            size_t size = captured.size();
            for (size_t i = 0; i < size; i++) {
                sb += S(captured[i]);
                if (size > 1) {
                    if (i + 2 < size) sb += u", ";
                    else if (i + 1 < size) sb += u" and ";
                }
            }
            data.push_back(translate("capture", {c.first, sb}));
        }
        for (const Player& player : players) {
            if (player.dead) data.push_back(translate("dead", {player.id}));
            else data.push_back(translate("status", {player.id, getScore(player.id)}));
        }
        return data;
    }

    Lines getPlayerActions(int playerId, int) override {
        Lines data;
        const Player& player = players[(size_t)playerId];
        if (player.lastActions.empty()) {
            if (!player.dead) data.push_back(translate("wait", {playerId}));
        } else {
            if (player.lastMoves > 0) data.push_back(translate("groupMove", {playerId, player.lastMoves, String(player.lastMoves > 1 ? u"s" : u"")}));
            if (player.lastPlaces > 0) data.push_back(translate("groupPlace", {playerId, player.lastPlaces, String(player.lastPlaces > 1 ? u"s" : u"")}));
        }
        for (const auto& m : player.mistakes) {
            const Mistake& mistake = m.second;
            if (mistake.count == 0) data.push_back(mistake.message);
            else data.push_back(java::format(u"%s (and %d additional errors of the same type)", {Arg(mistake.message), Arg(mistake.count)}));
        }
        return data;
    }

    void setPlayerTimeout(int, int round, int player) override { players[(size_t)player].setTimeout(round); }
    int getMaxRoundCount(int) override { return MAX_ROUNDS; }
    NString getDeathReason(int player) override { return u"$" + S(player) + u" eliminated"; }
    bool showTooltips() override { return true; }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
