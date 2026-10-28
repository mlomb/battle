// Platinum Rift - Episode 2 (CodinGame, "platinum-rift-episode-2", API id "platinum-rift2") - headless C++ referee, port of
// the CodinGame referee in https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the only
// commit of the file), "Referees/Platinum Rift2/Referee.java": the single-file referee `class Referee extends MultiReferee`
// with CodinGame's old AbstractReferee base embedded (common/legacy.h, legacy::Base::platinumRift2_2015(): Platinum
// Rift's 2014 base but for its INFOS, which carry the actions of every frame and, on a new round, the game summary (no
// SUMMARY command); the scores updated at every step of nextPlayer(), the reason code "timeout", the texts "$N: invalid
// action" / "$N: timeout!" as tooltips, getMillisTimeForRound 150, no precomputed inputs, the frame data and the tooltips
// sent in every frame; its SoloReferee class is unused).
// Two presets. cg-2014-platinum-rift-episode-2 is the game CodinGame runs today (one division): the file with MAX_HEIGHT 12, i.e.
// random maps of 5 to 11 rows (referees/cg-builder/patches/cg-2014-platinum-rift-episode-2.patch, the jar built with it), the
// only difference CodinGame's replays show. cg-2014-platinum-rift-episode-2-original (cg-2014-platinum-rift-episode-2-original.cpp:
// this file with PLATINUM_RIFT2_ORIGINAL 1) is the file as published (MAX_HEIGHT 18: 5 to 17 rows). CodinGame's replays
// (625 games of the top 1000 players) give the referee's own configuration: procedural, distribution and the fixed map are
// what the seed draws, a random map's height is nextInt(7) + 5 where the file draws nextInt(13) + 5, and "symmetric" is
// CodinGame's own init property, not derived from the seed. The jar's CLI passes only the seed, so both presets draw
// "symmetric" from the seed (the file's default: half the games); the validation build -DPLATINUM_RIFT2_PROPS=1 takes more
// init properties from the environment variable PR2_PROPS ("symmetric=true") to replay CodinGame's games.
// 2 players (getMinimumPlayerCount() is 1, but the fog of war only exists for players 0 and 1: one player makes the init
// fail, a third player's first input makes the Java referee crash; both reproduced). No league rules in the referee.
// Deterministic: reproduces the same game as the reference jar for a given -seed (the order of one view line aside, below).
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): startUnits, seed, hq, procedural,
// distribution, symmetric, platinum, unitCost, [width,] startPlatinum, map, [height,] in the order of a JDK 9+ Properties)
// to stdout like the jar (referees/cg-builder/legacy: the referee runs unchanged in the runner of the SDK 4.5.0). "null"
// scores when the game fails (the Java referee crashed or the protocol broke; the jar after 150 s). -l writes the trace of
// common/legacy.h (bot executions, view sizes, infos, tooltips, END; CG_TRACE_VIEWS=1 adds the text of every view).
// Time limits: the base's 1000 ms for the first turn, 150 ms after (the game overrides neither).
//
// The game: a map of hexagonal zones drawn from the seed: half the time one of five fixed maps (120 platinum), else a
// random connected map of 5 to 11 rows (-original: 17) and 32/18 as many columns (a random share of the cells, up to half,
// are zones; as much platinum as zones), symmetric (point symmetry: zones, platinum and headquarters) half the time;
// the platinum is spread by one of four distributions, 1 to 6 per zone. Each player starts with 10 pods on its headquarters (a zone
// without platinum) and 0 platinum. Each round every player still in the game answers two lines: its moves ("n from to"
// triples, or WAIT; a '#' or '\'' starts a message of 30 chars shown in the view) and a second line that is ignored; at
// the end of its turn every 20 platinum buys a pod on its headquarters (wherever its owner). Invalid moves are skipped
// (listed as mistakes); a malformed line puts the player out. After the last player of a round: every move, then every
// purchase, then in each zone up to three fight rounds (every player there loses one pod per round), the last one
// standing owns the zone, then every player earns the platinum of its zones. Fog of war: a player sees the owner, the
// pods and the platinum of the zones it occupies, of their neighbours and of both headquarters (its own pods
// everywhere). A player with no pod, no income and no way to buy, or whose headquarters is taken, is eliminated when it
// next plays (its moves and purchase of that turn still count); a player out with no zone left raises everyone else's
// bonus by one. Score: zones owned + bonus while the player holds its headquarters, else 0. The game ends when a
// headquarters falls, after 250 rounds, when no island can change hands any more, when the ranking can no longer change
// (each player's zones against the others' potential), or when every player is out.
//
// The Java referee is followed closely, notably: the random map (union-find with path compression over a list of
// candidate cells, java.util.ArrayList removals by equals, the sorted zone list; its trailing space in "map"), the
// distribution of the platinum (its own java.util.Random, the symmetric pairing of zones i and n - 1 - i), the
// headquarters drawn among the zones without platinum, every draw of the referee's java.util.Random (eager defaults
// included), the map's HashMap<Coord, Tile> and every zone's HashSet<Integer> of neighbours (iteration orders by
// java::HashMapOrder: they decide the islands' search and the order of the links in the init input, a HashSet<Edge>),
// the depth-first island search (java.util.Stack) and its sort (TimSort, by size), the parsing of the first line
// (String.trim, the message cut at the first '#' else '\'', equalsIgnoreCase("wait"), split("\\s+"), Integer.valueOf, the
// "trailing" error, moves accepted before a malformed triple still played, the mistakes grouped by type in insertion
// order), the move and purchase merges, the fights in zone order, the elimination test at the start of a player's turn,
// the extinction bonus, the end tests, the INFOS / view texts in full (a frame's view: platinum, zones, score and message
// of every player, on a new round the round's purchases, moves and fights, then each player's visible zones), the game
// parameters' order (java::Properties: 12 keys resize the JDK's table).
// Math.pow: Distribution.value(p) = Math.pow(p, e) (e = 5, 1, 10, 0.4) only serves the platinum count
// 1 + (int)(value(p) * 5 + 0.5) of a zone, p = random.nextDouble() (a multiple of 2^-53). The jar's Math.pow is HotSpot's
// x86_64 intrinsic (Intel's LIBM stub), not StrictMath's fdlibm: they differ in the last bit for about 10% of these p, and
// the count of one p differs (MOSTLY_LOW, p = 7154672683828276 / 2^53). The count is a step function of p with five steps
// per distribution: this port compares p with the jar's own steps (PLATINUM_STEPS, the same formula and table as
// cg-2014-platinum-rift-episode-1, checked again on this jar's own Distribution.value), exact for every p the Random can draw.
// Collections: Coord and Edge hash by value (31 * (31 + a) + b); no two links or coordinates of a map drawn from a seed
// share a hash code (that needs a link spanning 33+ zone ids, i.e. 32+ rows, or a row 31+: init properties only), so
// their tree bins have no identity tie-break (with such init properties, the edge created first counts as the larger
// identity hash code; the validation build takes the jar's identity hash codes of tied links from the reflection check). The players' captures in the summary (HashMap<Integer, ...>: player order),
// the ranked players (a HashSet<Integer> asked contains / size) and the base's tooltips (one per frame at most here)
// show no order of their own.
// The occupied zones of each player are a HashSet<Tile>, and Tile has no hashCode(): its iteration order follows
// HotSpot's identity hash codes, which the jar draws from the referee thread's xor-shift state (the tiles get
// consecutive values in the first update; the thread's seed depends on how many threads the JVM created before it, so
// it changes from launch to launch even on an idle machine). That order only decides in which order the zones enter
// the HashSet<Integer> of visible zones, which shows only in the view's last line (the visible zones of both players, in
// the order of that set): this port iterates the occupied zones by id, so that line can list the zones of a bucket in
// another order than the jar (the same set; inputs, scores, infos and everything else are unaffected). The validation
// build -DPLATINUM_RIFT2_HINTS=1 takes the tiles' identity hash codes (PR2_IDH_TILES="h0 h1 ...", graph order) and then
// reproduces the jar's view exactly.
// Not reproduced / not modelled: the wall clock (see engine.h); the order of a frame's tooltips (never two here); the
// jar's identity hash order above; with maps or sizes given by init properties (the CLI never passes them), links or
// coordinates sharing a hash code in a tree bin (identity order in Java, see above), and the helpful messages of the jar's NullPointerExceptions (never shown: only in frames
// whose data the base drops, or in crashes); a jar on another CPU (whose rcpps may differ) might move a platinum step
// by one value of p.
//
// Validation (referees/README.md): referee-diff against the jars with 31 deterministic bots (every ordered pair on three
// seed bases plus targeted line-ups, all map variants: 2967 games; -original: 989 games on a fourth base; a 240-game spot
// check on a fifth: all identical, no replay needed), direct runs comparing the jars' game logs with this port's trace
// (views, infos, tooltips, stdout; raw bytes and line endings; the views exact with the jar's identity hash codes, read
// from a diagnostic build of the jar or found among HotSpot's few thread seeds for the real jar), the jar's own compiled
// classes driven by reflection over random properties and command sequences (300,000 cases, 42 of 46 mutations detected,
// the others equivalent or unreachable with 2 players), the platinum steps against the jar's Distribution.value, and all
// 625 CodinGame replays fetched reproduced through this port's CLI (with CodinGame's "symmetric").
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/legacy.h"

#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

#ifndef PLATINUM_RIFT2_ORIGINAL
#define PLATINUM_RIFT2_ORIGINAL 0
#endif
#ifndef PLATINUM_RIFT2_HINTS
#define PLATINUM_RIFT2_HINTS 0
#endif
#ifndef PLATINUM_RIFT2_PROPS
#define PLATINUM_RIFT2_PROPS 0
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

const int MAX_ROUNDS = 250;
const int NEUTRAL = -1;
const int NOBODY = -2;
const int COMBAT_STRIKES = 3;
const double MAX_PLATINUM = 6;
const int MAX_MESSAGE_LENGTH = 30;
const int MIN_HEIGHT = 5;
// CodinGame's game: 12 (referees/cg-builder/patches/cg-2014-platinum-rift-episode-2.patch); the file as published: 18
const int MAX_HEIGHT = PLATINUM_RIFT2_ORIGINAL ? 18 : 12;
const float MAP_RATIO = 32.0f / 18.0f;
const int HEIGHT_LOWEST = 1;
const int HEIGHT_HIGHEST = 40;
const int WIDTH_LOWEST = 2;
const int WIDTH_HIGHEST = 70;

// Referee#initReferee's maps (the fixed ones)
const char16_t* const MAPS[] = {
    u"1 6 1 7 1 8 1 9 1 10 1 11 1 12 1 13 1 14 1 17 1 18 1 19 1 20 1 21 2 6 2 7 2 8 2 9 2 10 2 11 2 12 2 13 2 14 2 15 2 17 2 22 3 5 3 6 3 14 3 15 "
    u"3 16 3 21 4 5 4 6 4 11 4 12 4 13 4 14 4 15 4 16 4 21 4 22 5 4 5 5 5 10 5 11 5 12 5 13 5 14 5 20 5 22 6 4 6 5 6 6 6 10 6 11 6 12 6 13 6 14 "
    u"6 17 6 18 6 19 6 20 6 23 7 3 7 4 7 5 7 6 7 7 7 8 7 9 7 10 7 11 7 12 7 13 7 16 7 19 7 22 8 3 8 4 8 12 8 13 8 16 8 19 8 22 9 2 9 3 9 8 9 9 "
    u"9 10 9 11 9 12 9 15 9 16 9 17 9 18 9 21 10 2 10 3 10 9 10 10 10 11 10 12 10 18 10 21 11 1 11 2 11 3 11 7 11 9 11 10 11 11 11 14 11 15 "
    u"11 16 11 17 11 20 12 1 12 2 12 3 12 4 12 5 12 6 12 7 12 8 12 10 12 11 12 14 12 17 12 20 13 0 13 1 13 3 13 4 13 8 13 9 13 10 13 13 13 16 "
    u"13 19 14 0 14 1 14 3 14 4 14 6 14 7 14 9 14 10 14 11 14 12 14 13 14 14 14 15 14 16 14 19 15 0 15 2 15 3 15 5 15 6 15 8 15 9 15 15 15 18 "
    u"16 0 16 1 16 5 16 6 16 8 16 9 16 10 16 12 16 13 16 14 16 15 16 18 17 0 17 1 17 2 17 3 17 4 17 5 17 6 17 7 17 8 17 14 17 17 18 1 18 2 18 3 "
    u"18 4 18 5 18 6 18 7 18 8 18 11 18 12 18 13 18 14 18 17 19 7 19 11 19 16 20 7 20 12 20 16 21 6 21 9 21 10 21 11 21 12 21 15 21 16 21 17 "
    u"21 18 21 19 21 20 21 21 21 22 22 6 22 9 22 15 22 16 22 17 22 18 22 19 22 20 22 21 22 22 22 23 23 5 23 8 23 9 23 10 23 11 23 13 23 14 "
    u"23 15 23 17 23 18 23 22 23 23 24 5 24 8 24 14 24 15 24 17 24 18 24 20 24 21 24 23 25 4 25 7 25 8 25 9 25 10 25 11 25 12 25 13 25 14 25 16 "
    u"25 17 25 19 25 20 25 22 25 23 26 4 26 7 26 10 26 13 26 14 26 15 26 19 26 20 26 22 26 23 27 3 27 6 27 9 27 12 27 13 27 15 27 16 27 17 27 18 "
    u"27 19 27 20 27 21 27 22 28 3 28 6 28 7 28 8 28 9 28 12 28 13 28 14 28 16 28 20 28 21 28 22 29 2 29 5 29 11 29 12 29 13 29 14 29 20 29 21 "
    u"30 2 30 5 30 6 30 7 30 8 30 11 30 12 30 13 30 14 30 15 30 20 30 21 31 1 31 4 31 7 31 10 31 11 31 19 31 20 32 1 32 4 32 7 32 10 32 11 32 12 "
    u"32 13 32 14 32 15 32 16 32 17 32 18 32 19 32 20 33 0 33 3 33 4 33 5 33 6 33 9 33 10 33 11 33 12 33 13 33 17 33 18 33 19 34 1 34 3 34 9 "
    u"34 10 34 11 34 12 34 13 34 18 34 19 35 1 35 2 35 7 35 8 35 9 35 10 35 11 35 12 35 17 35 18 36 2 36 7 36 8 36 9 36 17 36 18 37 1 37 6 37 8 "
    u"37 9 37 10 37 11 37 12 37 13 37 14 37 15 37 16 37 17 38 2 38 3 38 4 38 5 38 6 38 9 38 10 38 11 38 12 38 13 38 14 38 15 38 16 38 17",
    u"0 8 0 21 1 8 1 9 1 10 1 11 1 12 1 21 1 22 2 3 2 4 2 5 2 11 2 13 2 14 2 22 3 2 3 4 3 5 3 6 3 7 3 14 3 15 3 22 4 2 4 7 4 8 4 9 4 10 4 11 "
    u"4 16 4 17 4 23 5 2 5 9 5 10 5 11 5 12 5 17 5 19 5 22 6 2 6 5 6 6 6 12 6 13 6 18 6 19 6 22 6 23 7 2 7 6 7 9 7 13 7 14 7 18 7 19 7 22 8 2 "
    u"8 7 8 8 8 10 8 15 8 19 8 23 9 1 9 6 9 7 9 8 9 10 9 14 9 15 9 19 9 22 9 23 10 5 10 6 10 7 10 9 10 10 10 12 10 16 10 19 10 20 10 23 11 4 "
    u"11 5 11 9 11 10 11 12 11 16 11 19 11 22 12 3 12 4 12 7 12 8 12 10 12 11 12 13 12 16 12 17 12 20 12 22 12 23 13 3 13 8 13 10 13 11 13 12 "
    u"13 16 13 19 13 22 14 3 14 8 14 11 14 12 14 16 14 20 14 22 14 23 15 2 15 5 15 6 15 7 15 8 15 9 15 11 15 12 15 14 15 15 15 16 15 19 15 22 "
    u"16 2 16 5 16 6 16 7 16 9 16 10 16 11 16 12 16 13 16 14 16 15 16 19 16 22 17 1 17 4 17 9 17 10 17 11 17 12 17 13 17 14 17 18 17 21 17 22 "
    u"18 1 18 3 18 4 18 6 18 7 18 10 18 11 18 13 18 14 18 15 18 17 18 18 18 21 18 22 19 3 19 7 19 8 19 9 19 10 19 13 19 14 19 15 19 16 19 17 "
    u"19 20 19 21 20 2 20 3 20 6 20 7 20 8 20 9 20 10 20 13 20 14 20 15 20 16 20 20 21 1 21 2 21 5 21 6 21 8 21 9 21 10 21 12 21 13 21 16 21 17 "
    u"21 19 21 20 21 22 22 1 22 2 22 5 22 9 22 10 22 11 22 12 22 13 22 14 22 19 22 22 23 1 23 4 23 8 23 9 23 10 23 11 23 12 23 13 23 14 23 16 "
    u"23 17 23 18 23 21 24 1 24 4 24 7 24 8 24 9 24 11 24 12 24 14 24 15 24 16 24 17 24 18 24 21 25 0 25 1 25 3 25 7 25 11 25 12 25 15 25 20 "
    u"26 1 26 4 26 7 26 11 26 12 26 13 26 15 26 20 27 0 27 1 27 3 27 6 27 7 27 10 27 12 27 13 27 15 27 16 27 19 27 20 28 1 28 4 28 7 28 11 "
    u"28 13 28 14 28 18 28 19 29 0 29 3 29 4 29 7 29 11 29 13 29 14 29 16 29 17 29 18 30 0 30 1 30 4 30 8 30 9 30 13 30 15 30 16 30 17 30 22 "
    u"31 0 31 4 31 8 31 13 31 15 31 16 31 21 32 1 32 4 32 5 32 9 32 10 32 14 32 17 32 21 33 0 33 1 33 4 33 5 33 10 33 11 33 17 33 18 33 21 "
    u"34 1 34 4 34 6 34 11 34 12 34 13 34 14 34 21 35 0 35 6 35 7 35 12 35 13 35 14 35 15 35 16 35 21 36 1 36 8 36 9 36 16 36 17 36 18 36 19 "
    u"36 21 37 1 37 9 37 10 37 12 37 18 37 19 37 20 38 1 38 2 38 11 38 12 38 13 38 14 38 15 39 2 39 15",
    u"0 1 0 2 0 3 0 4 0 9 0 10 0 11 1 0 1 1 1 2 1 3 1 4 1 9 1 10 1 11 2 0 2 1 2 2 2 3 2 5 2 9 2 10 2 11 3 0 3 1 3 2 3 5 3 7 3 8 3 10 3 11 4 0 "
    u"4 1 4 2 4 6 4 7 4 9 4 11 5 0 5 1 5 2 5 4 5 6 5 9 5 11 6 0 6 1 6 3 6 4 6 5 6 7 6 8 6 10 7 0 7 3 7 4 7 5 7 7 7 9 8 0 8 1 8 3 8 4 8 5 8 6 "
    u"8 8 8 9 9 1 9 3 9 4 9 8 10 2 10 4 10 5 10 9 11 2 11 6 11 7 11 9 12 3 12 7 12 8 12 10 13 2 13 3 13 5 13 6 13 7 13 8 13 10 13 11 14 2 14 4 "
    u"14 6 14 7 14 8 14 11 15 1 15 3 15 4 15 6 15 7 15 8 15 10 15 11 16 0 16 2 16 5 16 7 16 9 16 10 16 11 17 0 17 2 17 4 17 5 17 9 17 10 17 11 "
    u"18 0 18 1 18 3 18 4 18 6 18 9 18 10 18 11 19 0 19 1 19 2 19 6 19 8 19 9 19 10 19 11 20 0 20 1 20 2 20 7 20 8 20 9 20 10 20 11 21 0 21 1 "
    u"21 2 21 7 21 8 21 9 21 10",
    u"1 10 1 11 2 2 2 3 2 4 2 5 2 6 2 7 2 8 2 9 2 10 2 12 3 1 3 2 3 3 3 4 3 5 3 6 3 7 3 9 3 10 3 11 4 1 4 2 4 3 4 5 4 8 4 9 4 11 4 12 5 0 5 1 "
    u"5 3 5 5 5 6 5 8 5 9 5 10 5 11 6 1 6 2 6 4 6 6 6 7 6 8 6 10 6 12 7 0 7 1 7 3 7 4 7 7 7 8 7 9 7 10 7 11 8 1 8 2 8 4 8 5 8 7 8 8 8 11 8 12 "
    u"9 0 9 1 9 5 9 6 9 7 9 10 9 11 10 1 10 2 10 3 10 6 10 7 10 8 10 9 10 11 10 12 11 0 11 1 11 3 11 4 11 5 11 6 11 9 11 10 11 11 12 1 12 2 "
    u"12 5 12 6 12 7 12 11 12 12 13 0 13 1 13 4 13 5 13 7 13 8 13 10 13 11 14 1 14 2 14 3 14 4 14 5 14 8 14 9 14 11 14 12 15 0 15 2 15 4 15 5 "
    u"15 6 15 8 15 10 15 11 16 1 16 2 16 3 16 4 16 6 16 7 16 9 16 11 16 12 17 0 17 1 17 3 17 4 17 7 17 9 17 10 17 11 18 1 18 2 18 3 18 5 18 6 "
    u"18 7 18 8 18 9 18 10 18 11 19 0 19 2 19 3 19 4 19 5 19 6 19 7 19 8 19 9 19 10 20 1 20 2",
    u"0 0 0 1 0 2 0 3 0 4 0 5 0 6 0 7 0 8 0 9 1 0 1 1 1 2 1 3 1 4 1 5 1 6 1 7 1 8 1 9 2 0 2 1 2 2 2 3 2 4 2 5 2 6 2 7 2 8 2 9 3 0 3 1 3 2 3 3 "
    u"3 4 3 5 3 6 3 7 3 8 3 9 4 0 4 1 4 2 4 3 4 4 4 5 4 6 4 7 4 8 4 9 5 0 5 1 5 2 5 3 5 4 5 5 5 6 5 7 5 8 5 9 6 0 6 1 6 2 6 3 6 4 6 5 6 6 6 7 "
    u"6 8 6 9 7 0 7 1 7 2 7 3 7 4 7 5 7 6 7 7 7 8 7 9 8 0 8 1 8 2 8 3 8 4 8 5 8 6 8 7 8 8 8 9 9 0 9 1 9 2 9 3 9 4 9 5 9 6 9 7 9 8 9 9 10 0 "
    u"10 1 10 2 10 3 10 4 10 5 10 6 10 7 10 8 10 9 11 0 11 1 11 2 11 3 11 4 11 5 11 6 11 7 11 8 11 9 12 0 12 1 12 2 12 3 12 4 12 5 12 6 12 7 "
    u"12 8 12 9 13 0 13 1 13 2 13 3 13 4 13 5 13 6 13 7 13 8 13 9 14 0 14 1 14 2 14 3 14 4 14 5 14 6 14 7 14 8 14 9 15 0 15 1 15 2 15 3 15 4 "
    u"15 5 15 6 15 7 15 8 15 9 16 0 16 1 16 2 16 3 16 4 16 5 16 6 16 7 16 8 16 9 17 0 17 1 17 2 17 3 17 4 17 5 17 6 17 7 17 8 17 9 18 0 18 1 "
    u"18 2 18 3 18 4 18 5 18 6 18 7 18 8 18 9 19 0 19 1 19 2 19 3 19 4 19 5 19 6 19 7 19 8 19 9",
};

String S(i64 v) { return java::str(v); }

[[noreturn]] void npe() { throw java::Exception{"java.lang.NullPointerException"}; }

// Integer.valueOf / Long.valueOf / Double.valueOf: a NumberFormatException (a RuntimeException) when it fails
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
double parseDoubleOrThrow(const String& s) {
    double v;
    if (java::parseDouble(s, v)) return v;
    // FloatingDecimal.readJavaFormatString: the message quotes the trimmed text ("empty String" when nothing is left)
    String t = java::trim(s);
    if (t.empty()) throw java::Exception{"java.lang.NumberFormatException: empty String"};
    throw java::Exception{java::encodeUtf8(java::numberFormatException(t))};
}

// Referee.Distribution (values() order) and the platinum count of distributePlatinum:
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
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    i32 hashCode() const { return java::add(java::mul(31, java::add(31, x)), y); }
    bool isEven() const { return x % 2 == 0; }
};

// A HashSet<Integer> of zone ids (a zone's neighbours, a player's visible zones): members, and the iteration order of the
// JDK's HashSet (clear() keeps the table's size)
struct IntSet {
    java::HashMapOrder order;
    std::vector<i32> members;
    bool contains(i32 v) const { return std::find(members.begin(), members.end(), v) != members.end(); }
    void add(i32 v) {
        if (contains(v)) return;
        members.push_back(v);
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

// A player's visible zones, a HashSet<Integer> of zone ids: the same as IntSet with a membership table (asked for every
// zone of every input)
struct VisibleSet {
    java::HashMapOrder order;
    std::vector<char> in; // by zone id (ids are >= 0)
    bool contains(i32 v) const { return v >= 0 && (size_t)v < in.size() && in[(size_t)v]; }
    void add(i32 v) {
        if (contains(v)) return;
        if ((size_t)v >= in.size()) in.resize((size_t)v + 1, 0);
        in[(size_t)v] = 1;
        order.put(v, v);
    }
    void clear() {
        std::fill(in.begin(), in.end(), 0);
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
// HashMap.tieBreakOrder, i.e. by their identity hash codes. No map drawn from a seed has two such links (see the
// header); for maps given by init properties, the edge created first counts as the larger identity hash code (as
// cg-2014-platinum-rift-episode-1 measured for its map). Keys: edge ids in creation order.
#if PLATINUM_RIFT2_HINTS
// Validation only: the identity hash codes of the jar's Edge objects that share a hash code (set by the reflection check);
// the tie-break then follows them like tieBreakOrder.
std::map<std::pair<i32, i32>, i64> edgeIdentity;
const std::vector<std::pair<i32, i32>>* edgeRegistry = nullptr;
#endif
struct EdgeTieBreak {
    static constexpr bool enabled = true;
    // tieBreakOrder(k, pk): -1 when identityHashCode(k) <= identityHashCode(pk), i.e. here when k was created after pk
    int operator()(i32 k, i32 pk) const {
#if PLATINUM_RIFT2_HINTS
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
#if PLATINUM_RIFT2_HINTS
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

// A player's occupied zones, HashSet<Tile> in Java (Tile has no hashCode(): the identity hash codes order it; see the
// header). Here they are iterated by zone id; the validation build iterates them like a HashSet of the given identity
// hash codes.
#if PLATINUM_RIFT2_HINTS
std::vector<i32> tileIdentity; // PR2_IDH_TILES, graph order
#endif
struct TileSet {
    std::vector<char> in; // by tile index
#if PLATINUM_RIFT2_HINTS
    java::HashMapOrder order;
#endif
    void add(int t) {
        if ((size_t)t >= in.size()) in.resize((size_t)t + 1, 0);
        in[(size_t)t] = 1;
#if PLATINUM_RIFT2_HINTS
        if ((size_t)t < tileIdentity.size()) order.put(t, tileIdentity[(size_t)t]);
#endif
    }
    void remove(int t) {
        if ((size_t)t < in.size()) in[(size_t)t] = 0;
#if PLATINUM_RIFT2_HINTS
        if ((size_t)t < tileIdentity.size()) order.remove(t, tileIdentity[(size_t)t]);
#endif
    }
    std::vector<int> iterate() {
        std::vector<int> r;
#if PLATINUM_RIFT2_HINTS
        if (!tileIdentity.empty()) {
            std::vector<i32> k;
            order.keys(k);
            for (i32 t : k) r.push_back(t);
            return r;
        }
#endif
        for (size_t t = 0; t < in.size(); t++)
            if (in[t]) r.push_back((int)t);
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

struct PlayerMoveSet;

// Referee.Tile
struct Tile {
    int platinum = 0, ownerId = -1, island = -1, id = 0, x = 0, y = 0;
    std::vector<int> units;
    bool contested = false;
    IntSet neighbours;

    Tile(int playerCount, int id_, const Coord& c) : id(id_), x(c.x), y(c.y) { units.assign((size_t)playerCount, 0); }

    int getPodCountFor(int playerId, const PlayerMoveSet& moves) const;
    int unit(int pid) const {
        if (pid < 0 || pid >= (int)units.size()) throw java::Exception{java::arrayIndexOutOfBoundsException(pid, (i64)units.size())};
        return units[(size_t)pid];
    }
    void remove(int amount, int pid) { units[(size_t)pid] = java::sub(unit(pid), amount); }
    void add(int amount, int pid) { units[(size_t)pid] = java::add(unit(pid), amount); }

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
                        if (ind >= 4) throw java::Exception{java::arrayIndexOutOfBoundsException(ind, 4)}; // (never: 2 players)
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
    int now = unit(playerId);
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
    int base = -1; // the headquarters (a tile index), -1 for null
    std::vector<String> lastActions;
    std::vector<std::pair<String, Mistake>> mistakes; // LinkedHashMap<String, Mistake>: insertion order

    void setDead(int round) {
        deadAt = round;
        dead = true;
    }
    bool hasSpeech() const { return speech.has_value(); }
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

// ArrayList.get(i) / List index checks
inline size_t listIndex(i64 i, size_t size) {
    if (i < 0 || i >= (i64)size) throw java::Exception{java::indexOutOfBoundsException(i, (i64)size)};
    return (size_t)i;
}

// Referee.HexGrid
struct HexGrid {
    int width = 0, height = 0;
    CoordMap map;
    int zones[5] = {0, 0, 0, 0, 0};
    int units[4] = {0, 0, 0, 0};
    int income[4] = {0, 0, 0, 0};
    TileSet occupiedTiles[2];
    VisibleSet visibleTiles[2];
    std::vector<std::vector<Coord>> islands;
    std::vector<Coord> aroundEven{{0, -1}, {1, 0}, {0, 1}, {-1, 0}, {1, -1}, {-1, -1}};
    std::vector<Coord> aroundOdd{{0, -1}, {1, 0}, {0, 1}, {-1, 0}, {-1, 1}, {1, 1}};
    std::vector<Tile> graph;
    std::optional<EdgeSet> edges;
    PlayerPlacementSet prePlacements;
    std::vector<Battle> preBattles;
    int hq0 = 0, hq1 = 0;

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

    void generate(int playerCount, const String& mapData, int totalPlatinum, Distribution distribution, bool symetric, i64 seed) {
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
        initIslands();
        distributePlatinum(totalPlatinum, allTiles, distribution, symetric, seed);
    }

    void distributePlatinum(int total, std::vector<int> tiles, Distribution method, bool symetric, i64 seed) {
        java::Random random(seed);
        std::vector<int> discardedTiles; // LinkedList<Tile>
        int platinum = total;
        auto removeTile = [](std::vector<int>& list, int t) { // LinkedList.remove(Object): the first one (identity)
            auto it = std::find(list.begin(), list.end(), t);
            if (it != list.end()) list.erase(it);
        };
        while (platinum > 0 && (i64)tiles.size() >= 3 + (symetric ? 1 : 0) && (platinum > 1 || !symetric)) {
            int rTile = random.nextInt((i32)tiles.size());
            if (symetric && rTile == (int)tiles.size() - 1 - rTile) continue;
            int t = tiles[(size_t)rTile];
            double percent = random.nextDouble();
            int count = platinumCount(method, percent);
            count = std::min(symetric ? java::div(platinum, 2) : platinum, count);
            count = std::min((int)MAX_PLATINUM, count);
            count = std::max(1, count);
            platinum = java::sub(platinum, count);
            graph[(size_t)t].platinum = count;
            if (symetric) {
                int t2 = tiles[tiles.size() - 1 - (size_t)rTile];
                if (graph[(size_t)t2].id != graph[(size_t)t].id) {
                    platinum = java::sub(platinum, count);
                    graph[(size_t)t2].platinum = count;
                    removeTile(tiles, t2);
                }
                discardedTiles.push_back(t2);
            }
            removeTile(tiles, t);
            discardedTiles.push_back(t);
        }
        while (platinum > 0 && !discardedTiles.empty() && ((platinum > 1 && discardedTiles.size() > 1) || !symetric)) {
            int rTile = random.nextInt((i32)discardedTiles.size());
            int t = discardedTiles[(size_t)rTile];
            if (graph[(size_t)t].platinum < 6) {
                graph[(size_t)t].platinum++;
                platinum--;
                if (symetric) {
                    int t2 = discardedTiles[listIndex(rTile + (rTile % 2 == 0 ? 1 : -1), discardedTiles.size())];
                    graph[(size_t)t2].platinum++;
                    platinum--;
                }
            } else {
                if (symetric) {
                    int t2 = discardedTiles[listIndex(rTile + (rTile % 2 == 0 ? 1 : -1), discardedTiles.size())];
                    removeTile(discardedTiles, t2);
                }
                removeTile(discardedTiles, t);
            }
        }
    }

    // initIslands(false) (the only call: the graph search is unused)
    void initIslands() {
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
                    const std::vector<Coord>& around = hex.isEven() ? aroundEven : aroundOdd;
                    for (const Coord& neigh : around) {
                        Coord next{java::add(hex.x, neigh.x), java::add(hex.y, neigh.y)};
                        Tile* nextTile = get(next);
                        if (nextTile) {
                            tile->neighbours.add(nextTile->id);
                            nextTile->neighbours.add(tile->id);
                            if (nextTile->island == -1) {
                                nextTile->island = currentIsland;
                                stack.push_back(next);
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
    }

    Lines toStringInitAsMap(const std::vector<Player>& players) {
        Lines data;
        String baseInfo;
        if (players.size() == 2) {
            if (players[0].base < 0 || players[1].base < 0) npe();
            baseInfo = S(graph[(size_t)players[0].base].id) + u" " + S(graph[(size_t)players[1].base].id);
        } else {
            baseInfo = u"0 0";
        }
        data.push_back(S(width) + u" " + S(height) + u" " + S(map.size()) + u" " + baseInfo + u" fog");
        for (const Tile& t : graph) data.push_back(S(t.x) + u" " + S(t.y) + u" " + S(t.platinum) + u" " + S(t.id));
        return data;
    }

    Lines toStringInitAsGraph(const std::vector<Player>& players) {
        if (!edges) getLinkCount();
        Lines data;
        for (const Tile& t : graph) {
            if (graph[(size_t)players[0].base].id == t.id || graph[(size_t)players[1].base].id == t.id) data.push_back(S(t.id) + u" " + S(t.platinum));
            else data.push_back(S(t.id) + u" 0");
        }
        for (const auto& e : edges->iterate()) data.push_back(S(e.first) + u" " + S(e.second));
        return data;
    }

    // adjacent(x1, y1, x2, y2)
    bool adjacent(i32 x1, i32 y1, i32 x2, i32 y2) const {
        if (y1 == y2 && x1 == x2) return false;
        if (java::abs(java::sub(y1, y2)) > 1 || java::abs(java::sub(x1, x2)) > 1) return false;
        const std::vector<Coord>& around = x1 % 2 == 0 ? aroundEven : aroundOdd;
        for (const Coord& c : around)
            if (java::add(c.x, x1) == x2 && java::add(c.y, y1) == y2) return true;
        return false;
    }

    std::vector<Battle> update(const std::vector<Player>& players) {
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
            if (t.unit(0) == 0) occupiedTiles[0].remove((int)i);
            else occupiedTiles[0].add((int)i);
            if (t.unit(1) == 0) occupiedTiles[1].remove((int)i);
            else occupiedTiles[1].add((int)i);
        }
        for (int i = 0; i < 2; i++) {
            VisibleSet& sight = visibleTiles[i];
            sight.clear();
            for (int ti : occupiedTiles[i].iterate()) {
                Tile& tile = graph[(size_t)ti];
                sight.add(tile.id);
                for (i32 neighbourId : tile.neighbours.iterate()) sight.add(neighbourId);
            }
            if (players.size() < 2) throw java::Exception{java::indexOutOfBoundsException((i64)players.size(), (i64)players.size())};
            if (players[0].base < 0 || players[1].base < 0) npe();
            sight.add(graph[(size_t)players[0].base].id);
            sight.add(graph[(size_t)players[1].base].id);
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

    Lines toStringForPlayer(int playerId) {
        if (playerId < 0 || playerId >= 2) throw java::Exception{java::indexOutOfBoundsException(playerId, 2)}; // visibleTiles.get(playerId)
        Lines data;
        VisibleSet& sight = visibleTiles[playerId];
        for (const Tile& t : graph) {
            bool visible = sight.contains(t.id);
            int platinum = visible ? t.platinum : 0;
            int pods0 = 0, pods1 = 0;
            if (playerId == 0 || (playerId == 1 && visible)) pods0 = t.unit(0);
            if (playerId == 1 || (playerId == 0 && visible)) pods1 = t.unit(1);
            data.push_back(S(t.id) + u" " + S(visible ? t.ownerId : -1) + u" " + S(pods0) + u" " + S(pods1) + u" " + (visible ? u"1" : u"0") + u" " + S(platinum));
        }
        return data;
    }

    int getIncomeFor(int playerId) { return income[arrayIndex(playerId, 4)]; }

    bool hasPlayerLost(const Player& player) {
        if (player.base < 0) npe();
        return graph[(size_t)player.base].ownerId != player.id;
    }
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

    // spawnRandom(playerCount, startUnits, players, symmetric, hq0, hq1)
    void spawnRandom(int playerCount, int startUnits, std::vector<Player>& players, bool symmetric, std::optional<i32> hq0p, std::optional<i32> hq1p,
                     java::Random& random) {
        std::vector<int> noPlatinumtiles;
        for (const Tile& t : graph)
            if (t.platinum == 0) noPlatinumtiles.push_back(t.id);
        if (symmetric && playerCount == 2 && noPlatinumtiles.size() >= 2) {
            i32 idRandom = random.nextInt((i32)(noPlatinumtiles.size() / 2));
            if (hq0p) idRandom = *hq0p;
            this->hq0 = idRandom;
            Tile& t = graph[(size_t)noPlatinumtiles[listIndex(idRandom, noPlatinumtiles.size())]];
            Tile& t2 = graph[(size_t)noPlatinumtiles[listIndex((i64)noPlatinumtiles.size() - 1 - idRandom, noPlatinumtiles.size())]];
            place(0, startUnits, t.x, t.y);
            prePlacements.add(Placement(0, startUnits, t));
            players[0].base = t.id;
            place(1, startUnits, t2.x, t2.y);
            prePlacements.add(Placement(1, startUnits, t2));
            players[1].base = t2.id;
        } else {
            for (int idPlayer = 0; idPlayer < playerCount; idPlayer++) {
                int ti;
                if (idPlayer == 0) {
                    this->hq0 = hq0p ? *hq0p : random.nextIntChecked((i32)noPlatinumtiles.size());
                    ti = noPlatinumtiles[listIndex(this->hq0, noPlatinumtiles.size())];
                } else {
                    this->hq1 = hq1p ? *hq1p : random.nextIntChecked((i32)noPlatinumtiles.size());
                    ti = noPlatinumtiles[listIndex(this->hq1, noPlatinumtiles.size())];
                }
                Tile& t = graph[(size_t)ti];
                place(idPlayer, startUnits, t.x, t.y);
                noPlatinumtiles.erase(std::find(noPlatinumtiles.begin(), noPlatinumtiles.end(), ti));
                prePlacements.add(Placement(idPlayer, startUnits, t));
                players[(size_t)idPlayer].base = t.id;
            }
        }
        preBattles = update(players);
    }

    // getRoot(parent, act): union-find with path compression (recursive, as in Java)
    static Coord getRoot(std::vector<std::vector<Coord>>& parent, const Coord& act) {
        if (!(act == parent[(size_t)act.x][(size_t)act.y])) {
            Coord copie = getRoot(parent, parent[(size_t)act.x][(size_t)act.y]);
            parent[(size_t)act.x][(size_t)act.y] = copie;
        }
        return parent[(size_t)act.x][(size_t)act.y];
    }

    String generateRandomMap(i32 w, i32 h, double proportion, bool symetric, java::Random& random) {
        std::vector<Coord> candidates; // ArrayList<Coord>
        for (i32 x = 0; x < w; x++)
            for (i32 y = 0; y < h; y++) candidates.push_back(Coord{x, y});
        int nbRoads = 0;
        const int dx[] = {0, 0, 1, 1, 1, -1, -1, -1};
        const int dy[] = {1, -1, 0, 1, -1, 0, 1, -1};
        std::vector<Coord> res;
        std::vector<std::vector<Coord>> parent((size_t)w, std::vector<Coord>((size_t)h, Coord{-1, -1}));
        std::vector<Coord> roots;
        auto removeFirst = [](std::vector<Coord>& list, const Coord& c) { // ArrayList.remove(Object): by equals
            auto it = std::find(list.begin(), list.end(), c);
            if (it != list.end()) list.erase(it);
        };
        while (proportion > (double)nbRoads / (double)(java::mul(w, h)) || roots.size() != 1 || nbRoads < 2) {
            Coord toCopy = candidates[(size_t)random.nextIntChecked((i32)candidates.size())];
            Coord toAdd = toCopy;
            for (int sym = 0; sym < (symetric ? 2 : 1); sym++) {
                removeFirst(candidates, toAdd);
                parent[(size_t)toAdd.x][(size_t)toAdd.y] = toAdd;
                for (int d = 0; d < 8; d++) {
                    Coord neighbour{toAdd.x + dx[d], toAdd.y + dy[d]};
                    if (neighbour.x >= w || neighbour.y >= h || neighbour.x < 0 || neighbour.y < 0 || !adjacent(neighbour.x, neighbour.y, toAdd.x, toAdd.y)) continue;
                    if (parent[(size_t)neighbour.x][(size_t)neighbour.y].x == -1) continue;
                    Coord r = getRoot(parent, neighbour);
                    if (!(r == toAdd)) {
                        parent[(size_t)r.x][(size_t)r.y] = toAdd;
                        removeFirst(roots, r);
                    }
                }
                roots.push_back(toAdd);
                res.push_back(toAdd);
                nbRoads++;
                if (toAdd.x == w - toAdd.x - 1 && toAdd.y == h - toAdd.y - 1) break;
                toAdd = Coord{w - toAdd.x - 1, h - toAdd.y - 1};
            }
        }
        java::sort(res, [](const Coord& a, const Coord& b) { return a.x != b.x ? java::compareInt(a.x, b.x) : java::compareInt(a.y, b.y); });
        String dataMap;
        for (const Coord& c : res) dataMap += S(c.x) + u" " + S(c.y) + u" ";
        return dataMap;
    }
};

struct Referee : legacy::MultiReferee {
    // the static fields of the Java referee (one game per JVM in the jar)
    i32 START_PLATINUM = 0;
    i32 START_UNITS = 10;
    i32 UNIT_COST = 20;
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
    bool symmetric = false;
    i32 width = 0, height = 0;
    double proportion = 0;
    bool procedural = false;

    Referee() { base = legacy::Base::platinumRift2_2015(); }

    String getGameName() override { return u"PlatinumRift"; }

    void initReferee(int playerCount, java::Properties& prop) override {
#if PLATINUM_RIFT2_PROPS
        // validation only: more init properties ("key=value" lines, e.g. CodinGame's "symmetric") to replay its games
        if (const char* extra = getenv("PR2_PROPS")) prop.load(java::decodeUtf8(std::string(extra)));
#endif
#if PLATINUM_RIFT2_HINTS
        if (const char* ids = getenv("PR2_IDH_TILES")) { // (else those set by the reflection check, if any)
            tileIdentity.clear();
            const char* p = ids;
            char* end;
            for (;;) {
                long long v = strtoll(p, &end, 10);
                if (end == p) break;
                tileIdentity.push_back((i32)v);
                p = end;
            }
        }
#endif
        // the default of "seed" (new Random(System.currentTimeMillis()).nextLong()) is computed whether it is given or not
        String defaultSeed = S(java::Random((i64)java::entropy()).nextLong());
        seed = parseLongOrThrow(prop.getProperty(u"seed", defaultSeed));
        random = java::Random(seed);
        i64 distributionSeed = random.nextLong();

        // the defaults are drawn whether the properties are given or not
        symmetric = java::parseBoolean(prop.getProperty(u"symmetric", random.nextBoolean() ? u"true" : u"false"));
        double drawn = random.nextDouble() / 2; // (Double.valueOf(String.valueOf(d)) is d)
        const String* proportionProp = prop.find(u"proportion");
        proportion = proportionProp ? parseDoubleOrThrow(*proportionProp) : drawn;
        String algoName = prop.getProperty(u"distribution", DISTRIBUTION_NAMES[random.nextInt(4)]);
        std::optional<i32> hq0, hq1;
        if (const String* hq = prop.find(u"hq")) {
            std::vector<String> hqs = java::split(*hq, u' ');
            i32 v;
            // Integer.valueOf(hqs[i]), any exception (a NumberFormatException, an ArrayIndexOutOfBoundsException) ignored
            if (hqs.size() > 0 && java::parseInt(hqs[0], v)) hq0 = v;
            if (hqs.size() > 1 && java::parseInt(hqs[1], v)) hq1 = v;
        }
        int d = -1;
        for (int k = 0; k < 4; k++)
            if (algoName == DISTRIBUTION_NAMES[k]) d = k;
        if (d < 0) d = random.nextInt(4); // Distribution.valueOf: an IllegalArgumentException, caught
        distribution = (Distribution)d;

        START_PLATINUM = parseIntOrThrow(prop.getProperty(u"startPlatinum", S(START_PLATINUM)));
        UNIT_COST = parseIntOrThrow(prop.getProperty(u"unitCost", S(UNIT_COST)));
        START_UNITS = parseIntOrThrow(prop.getProperty(u"startUnits", S(START_UNITS)));

        const String* mapProp = prop.find(u"map");
        map = HexGrid();
        i32 pushTotalPlatinum = 120;
        if (!mapProp) {
            const String* H = prop.find(u"height");
            const String* W = prop.find(u"width");
            bool pushProcedural = random.nextBoolean();
            if (W || H) pushProcedural = true;
            procedural = java::parseBoolean(prop.getProperty(u"procedural", pushProcedural ? u"true" : u"false"));
            if (procedural) {
                height = parseIntOrThrow(prop.getProperty(u"height", u"-1"));
                width = parseIntOrThrow(prop.getProperty(u"width", u"-1"));
                if (width == -1 && height == -1) height = random.nextInt(MAX_HEIGHT - MIN_HEIGHT) + MIN_HEIGHT;
                else if (width != -1) height = java::d2i((double)((float)width / MAP_RATIO));
                if (width == -1) width = java::d2i((double)((float)height * MAP_RATIO));
                height = std::min(HEIGHT_HIGHEST, std::max(HEIGHT_LOWEST, height));
                width = std::min(WIDTH_HIGHEST, std::max(WIDTH_LOWEST, width));
                if (symmetric) width += width % 2; // only maps with an even width can be symmetric
                mapData = map.generateRandomMap(width, height, proportion, symmetric, random);
                pushTotalPlatinum = (i32)(java::split(mapData, u' ').size() / 2);
            } else {
                mapData = MAPS[random.nextInt(5)];
            }
        } else {
            mapData = *mapProp;
            procedural = false;
        }

        totalPlatinum = parseIntOrThrow(prop.getProperty(u"platinum", S(pushTotalPlatinum)));
        map.generate(playerCount, mapData, totalPlatinum, distribution, symmetric, distributionSeed);
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
        map.spawnRandom(playerCount, START_UNITS, players, symmetric, hq0, hq1, random);
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        prop.setProperty(u"distribution", DISTRIBUTION_NAMES[distribution]);
        prop.setProperty(u"platinum", S(totalPlatinum));
        prop.setProperty(u"startPlatinum", S(START_PLATINUM));
        prop.setProperty(u"unitCost", S(UNIT_COST));
        prop.setProperty(u"startUnits", S(START_UNITS));
        prop.setProperty(u"map", mapData);
        prop.setProperty(u"symmetric", symmetric ? u"true" : u"false");
        prop.setProperty(u"procedural", procedural ? u"true" : u"false");
        prop.setProperty(u"hq", S(map.hq0) + u" " + S(map.hq1));
        if (procedural) {
            prop.setProperty(u"width", S(width));
            prop.setProperty(u"height", S(height));
        }
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
        Lines m = map.toStringInitAsMap(players);
        data.insert(data.end(), m.begin(), m.end());
        return data;
    }

    Lines getFrameDataForView(int round, int, int, bool keyFrame) override {
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
            int zones = getZones(p.id);
            int score = getScore(p.id);
            if (!t) npe();
            data.push_back(S(p.platinum) + u" " + S(zones) + u" " + S(score) + u" " + (p.hasSpeech() ? u"1" : u"0") + u" " + S(t->x) + u" " + S(t->y) + u" " + S(nx) +
                           u" " + S(ny));
            if (p.hasSpeech()) data.push_back(java::trim(*p.speech));
        }
        if (!keyFrame) {
            data.push_back(u"0");
            data.push_back(u"0");
        } else {
            if (round == 0) {
                data.push_back(S((i64)map.prePlacements.placements.size()));
                for (const Placement& placement : map.prePlacements.placements) data.push_back(placement.toString());
                data.push_back(u"0");
                data.push_back(S((i64)map.preBattles.size()));
                for (const Battle& battle : map.preBattles) data.push_back(battle.toString());
            } else {
                data.push_back(S((i64)playerPlacementsForView.size()));
                for (const Placement& placement : playerPlacementsForView) data.push_back(placement.toString());
                data.push_back(S((i64)playerMovesForView.size()));
                for (const Move& move : playerMovesForView) data.push_back(move.toString());
                data.push_back(S((i64)battles.size()));
                for (const Battle& battle : battles) data.push_back(battle.toString());
            }
            playerPlacementsForView.clear();
            playerMovesForView.clear();
        }
        String visibles;
        for (int p = 0; p < 2; p++) {
            for (i32 i : map.visibleTiles[p].iterate()) visibles += S(i) + u" ";
            if (p == 0) visibles += u"!";
        }
        data.push_back(visibles);
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
        if (map.isPlayerDead(player, UNIT_COST) || map.hasPlayerLost(player)) player.eliminate(turn); // (the turn goes on)

        std::vector<String> output = outputs;
        if (output.size() < 2) throw java::Exception{java::arrayIndexOutOfBoundsException((i64)output.size(), (i64)output.size())}; // (never: 2 lines)
        output[0] = java::trim(output[0]);
        output[1] = java::trim(output[1]); // (the second line is not used)
        NString moveMessage = parsePlayerMessage(output, 0);
        std::optional<std::vector<String>> moveData;
        if (!java::equalsIgnoreCase(output[0], "wait") && !output[0].empty()) moveData = java::splitRegexSpaces(output[0]);

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
                    if (!map.adjacent((i64)sid, (i64)did)) throw InvalidMoveOrPlacementException{u"Destination hex %d is not adjacent to source hex %d", {did, sid}};
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

        // the automatic purchase: every UNIT_COST platinum buys a pod on the player's headquarters
        if (player.platinum >= UNIT_COST && UNIT_COST != 0) {
            i32 podsToBuy = java::div(player.platinum, UNIT_COST);
            if (player.base < 0) npe();
            const Tile& t = map.graph[(size_t)player.base];
            playerPlacements.add(Placement(player.id, podsToBuy, t));
            framePlacements.add(Placement(player.id, podsToBuy, t));
            player.platinum = java::rem(player.platinum, UNIT_COST);
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
        Lines g = map.toStringInitAsGraph(players);
        data.insert(data.end(), g.begin(), g.end());
        return data;
    }

    Lines getInputForPlayer(int, int player) override {
        Lines data;
        data.push_back(S(players[(size_t)player].platinum));
        Lines m = map.toStringForPlayer(player);
        data.insert(data.end(), m.begin(), m.end());
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

        battles = map.update(players);

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

        int allMyBasesAreBelongToMe = 0;
        for (size_t playerId = 0; playerId < n; playerId++) {
            if (players[playerId].base < 0) npe();
            if (map.graph[(size_t)players[playerId].base].ownerId == (int)playerId) allMyBasesAreBelongToMe++;
        }
        if (allMyBasesAreBelongToMe <= 1) throw legacy::GameOverException(std::nullopt);

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

    int getScore(int player) override {
        const Player& p = players[(size_t)player];
        if (p.base < 0) npe();
        if (map.graph[(size_t)p.base].ownerId != p.id) return 0;
        return java::add(getZones(player), p.advantage);
    }
    int getZones(int player) { return map.getZones(player); }

    Lines getGameSummary(int round) override {
        Lines data;
        data.push_back(u"=== Summary for round " + S(round) + u" ===");
        // HashMap<Integer, List<Integer>> captures: player ids, iterated in ascending order
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
