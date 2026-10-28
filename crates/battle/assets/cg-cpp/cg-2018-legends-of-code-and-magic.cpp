// Legends of Code & Magic (CodinGame, 2018-09) - headless C++ referee, port of
// https://github.com/CodinGame/LegendsOfCodeAndMagic (commit 1aacfea, 2018-11-03, pom artifactId locam),
// CodinGame game engine 2.5, 2 players, the rules of the highest league (the jar's command line interface
// sets league.level 19: the referee's default branch, Difficulty.NORMAL, every one of the 160 cards in the
// draft). Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints "<p0score>\n<p1score>\n" and the game parameters ("draftChoicesSeed=..", "shufflePlayer0Seed=..",
// "seed=N", "shufflePlayer1Seed=..", the order the jar prints them) to stdout like the jar. When the Java
// referee would have failed, prints nothing and exits with code 1, like the jar (engine 2.5 has no FAIL
// command: its runner waits 150 s for the dead referee, then throws out of the game; see below). Exit code
// 1 also when the jar's command line interface fails (bad -seed, a bot that cannot be started).
// -l writes the trace of common/engine.h (one line per bot execution, game summary and tooltip block,
// then "END OK|<why the game failed>"; the view frames are counted, not built, see below).
//
// Leagues: EngineReferee's switch on getLeagueLevel() has 1 (VERY_EASY: creatures without keywords or
// summon effects), 2 (EASY: creatures without Drain, Lethal, Ward), 3 (LESS_EASY: every card but those
// with Drain, Lethal, Ward) and the default (NORMAL: every card): every level from 4 up, so whatever level
// CodinGame's upper leagues used, they played NORMAL, which level 19 gives; this port implements NORMAL
// only (the lower leagues differ only by the card pool).
//
// The game: a draft of 30 turns (both players at once: PICK 0-2 / PASS = the first card / CHOOSE <base
// id> among 3 cards drawn from a pool of 60), then a battle where the players alternate turns: each turn
// one line of actions separated by ';' (SUMMON id, ATTACK id target, USE id target, PASS; each with an
// optional text), played one by one, each in a frame of its own (an execution of that player for 0 lines).
// A player loses at 0 health; 30 health with runes at 25, 20, 15, 10, 5 (losing health past one draws an
// extra card next turn); an empty deck or the player's turn 50+ makes each draw burn a rune (health set to
// it) or kill. The second player starts with 5 cards and a mana bonus kept until a turn ends with 0 mana.
//
// Engine 2.5 (common/engine.h, cg::Options::engine25): engine 2.15's (engine2: the runner's split of the
// outputs, so an empty line gives the referee no line at all and its getOutputs().get(0) crashes the
// referee; no frame for a turn without execution, which never happens here), without game summary quotas
// or a total turn time quota (unreachable here anyway: 111 battle executions at most), and without a FAIL
// command. The jar is built with referees/cg-builder's CommandLineInterface-v2.java (which also discards
// the referee's own prints to System.out: this referee prints its turns there) and build.sh keeping
// src/main/resources/cardlist.txt. The bots get 1000 ms on their first draft turn and their first battle
// turn, 100 ms otherwise.
//
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption, texts.
// Notable behaviours reproduced here:
//   - the seeds: new java.util.Random(seed) gives the draft's seed and each player's deck shuffle seed
//     (three nextLong, printed with the game parameters); the draft pool is 60 distinct cards drawn by
//     rejection (nextInt(160)), each pick 3 distinct cards of the pool; the decks are shuffled with
//     Collections.shuffle and numbered 2i + player + 1.
//   - 5 frame-only turns before the draft (the showdraft layout of 60 cards in rows of 12, its view
//     removed upstream), and one before the battle (the initial board).
//   - the draft commands (String.split(" ", 3), exact names, Integer.parseInt: '+' and any Unicode
//     decimal digit accepted); its errors deactivate the player (score -1, the other keeps 0), with
//     CHOOSE's error listing the three base ids in HashSet order; when both players fail in one turn,
//     only the first one is handled.
//   - the battle line: split(";"), each action trimmed, parsed with split(" ", 2/3): any malformed action
//     (unknown name, missing or non-int argument, double spaces...) rejects the whole line and deactivates
//     the player; parsed actions are then played one per frame, those not in the referee's legal action
//     list at that point skipped with a summary warning (the first 3 of a frame, then a count), PASS
//     skipped silently.
//   - the rules as the referee codes them: Ward (removed by any non-zero attack, blocks a damaging item
//     instead of its damage), Lethal (any damage kills), Breakthrough (the excess damage, counted from
//     the defender's defense: negative health change), Drain (the attacker's attack, when it deals
//     damage), Guard (attacks must target guards), Charge (green items give it: canAttack = !hasAttacked),
//     items keeping attack >= 0, keyword removal before the Ward check; a creature copied by an attack or
//     an item loses its summon effects (myHealthChange, oppHealthChange, cardDraw read 0 in the inputs);
//     rune draws on every health loss crossing a rune (several at once), the second player's mana bonus,
//     the hand limit of 8 (a draw into a full hand is lost and leaves the deck as it is), the draws of
//     the turn start that can kill the current player (who is still executed: its illegal leading actions
//     get their warnings, then the game ends), the win check after each action ("first proper win", then
//     "second self-kill").
//   - Engine (common/engine.h): the protocol between the runner and the referee (java.util.Scanner
//     lines: U+0085, U+2028 or U+2029 in a bot line makes the game fail), the runner's 10000 bytes per
//     bot output and 30000 bytes per referee command (3 illegal-action warnings full of U+FFFD can pass
//     it: the game fails).
// Not reproduced:
//   - the view (GraphicEntityModule 2.5 entities of RefereeUI / PlayerUI / CardUI, the game's own
//     TooltipModule, FXModule and EndScreenModule): it cannot reach the engine's limits. Bot text reaches
//     it only in the talk bubbles (cut to 26 chars in the draft, 68 in the battle, sent when they change);
//     the largest frame is the battle's first (~19.5 KB, the 9 new cards of both hands) against 30000
//     bytes; whole games make 100-250 KB (view-maximising bots: 0-damage attacks every turn with changing
//     texts of 60 apostrophes, 8 JSON chars each) against 1 MB: a game has at most 111 battle executions
//     (both players are dead by their 55th turn) and 1 MB would take ~700 action frames with such texts.
//     So the -l trace has the frame count only.
//   - the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps; a referee crash
//     costs it 150 s), and the game log.
// Validated (2026-10-25) with battle referee-diff against the jar on 3600 games (30 bots, every ordered
// pairing including self-play, seeds from 1, 1000001, 987654321 and 3000000000: all identical, no mismatch,
// no flake), in direct mode (jar game log vs -l trace: every execution, frame count, summaries, tooltips,
// stdout with the game parameters, exit code) on 356 games, including raw line endings, invalid UTF-8,
// U+2028 / U+0085 / U+2029, empty lines (crashes), outputs at the runner's 10000-byte limit, a summary over
// the runner's 30000 bytes, seeds 0, -7, 2^63-1 and 5000000000+, -timeout, and 3- and 4-player games (the
// jar fails in init: no output, exit code 1, like here); every card's texts checked against the jar's own
// classes.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/engine.h"

using java::String;

static String A(const std::string& s) { return java::ascii(s); }
static std::string S(i64 v) { return std::to_string(v); }

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------
static const int CARDS_IN_DECK = 30;
static const int CARDS_IN_DRAFT = 60;
static const int INITIAL_HAND_SIZE = 4;
static const int MAX_CARDS_IN_HAND = 8;
static const int SECOND_PLAYER_CARD_BONUS = 1;
static const int SECOND_PLAYER_MAX_CARD_BONUS = 0;
static const int SECOND_PLAYER_MANA_BONUS_TURNS = 1;
static const int MAX_MANA = 12;
static const int INITIAL_HEALTH = 30;
static const int MAX_CREATURES_IN_LINE = 6;
static const int TIMELIMIT_FIRSTDRAFTTURN = 1000;
static const int TIMELIMIT_DRAFTTURN = 100;
static const int TIMELIMIT_FIRSTGAMETURN = 1000;
static const int TIMELIMIT_GAMETURN = 100;
static const int PLAYER_TURNLIMIT = 50;
static const int MAX_TURNS_HARDLIMIT = (2 * CARDS_IN_DECK + 2 * CARDS_IN_DECK + 2 * 10) * 10;
static const int FRAME_DURATION_SHOWDRAFT = 1;
static const int FRAME_DURATION_DRAFT = 500;
static const int FRAME_DURATION_BATTLE = 750;
static const int FRAME_DURATION_SUMMON = 600;
static const int ILLEGAL_ACTION_SUMMARY_LIMIT = 3;
// ConstantsUI: the number of frame-only turns before the draft comes from the showdraft layout
static const int SHOWDRAFT_SIZECHOICE[4] = {90, 60, 30, 15};
static const int SHOWDRAFT_ROWSIZE[4] = {15, 12, 10, 5};

enum CardType { CREATURE = 0, ITEM_GREEN = 1, ITEM_RED = 2, ITEM_BLUE = 3 };
static const char* TYPE_DESCRIPTION[4] = {"creature", "itemGreen", "itemRed", "itemBlue"};

// cardlist.txt, as Constants.LoadCardlist parses it: baseId, name, type, cost, attack, defense,
// keywords, myHealthChange, oppHealthChange, cardDraw (the tooltip text only serves the view)
struct CardData {
    int baseId;
    const char* name;
    int type, cost, attack, defense;
    const char* keywords;
    int myHealthChange, oppHealthChange, cardDraw;
};
static const CardData CARDLIST[] = {
    {1, "Slimer", 0, 1, 2, 1, "------", 1, 0, 0},
    {2, "Scuttler", 0, 1, 1, 2, "------", 0, -1, 0},
    {3, "Beavrat", 0, 1, 2, 2, "------", 0, 0, 0},
    {4, "Plated Toad", 0, 2, 1, 5, "------", 0, 0, 0},
    {5, "Grime Gnasher", 0, 2, 4, 1, "------", 0, 0, 0},
    {6, "Murgling", 0, 2, 3, 2, "------", 0, 0, 0},
    {7, "Rootkin Sapling", 0, 2, 2, 2, "-----W", 0, 0, 0},
    {8, "Psyshroom", 0, 2, 2, 3, "------", 0, 0, 0},
    {9, "Corrupted Beavrat", 0, 3, 3, 4, "------", 0, 0, 0},
    {10, "Carnivorous Bush", 0, 3, 3, 1, "--D---", 0, 0, 0},
    {11, "Snowsaur", 0, 3, 5, 2, "------", 0, 0, 0},
    {12, "Woodshroom", 0, 3, 2, 5, "------", 0, 0, 0},
    {13, "Swamp Terror", 0, 4, 5, 3, "------", 1, -1, 0},
    {14, "Fanged Lunger", 0, 4, 9, 1, "------", 0, 0, 0},
    {15, "Pouncing Flailmouth", 0, 4, 4, 5, "------", 0, 0, 0},
    {16, "Wrangler Fish", 0, 4, 6, 2, "------", 0, 0, 0},
    {17, "Ash Walker", 0, 4, 4, 5, "------", 0, 0, 0},
    {18, "Acid Golem", 0, 4, 7, 4, "------", 0, 0, 0},
    {19, "Foulbeast", 0, 5, 5, 6, "------", 0, 0, 0},
    {20, "Hedge Demon", 0, 5, 8, 2, "------", 0, 0, 0},
    {21, "Crested Scuttler", 0, 5, 6, 5, "------", 0, 0, 0},
    {22, "Sigbovak", 0, 6, 7, 5, "------", 0, 0, 0},
    {23, "Titan Cave Hog", 0, 7, 8, 8, "------", 0, 0, 0},
    {24, "Exploding Skitterbug", 0, 1, 1, 1, "------", 0, -1, 0},
    {25, "Spiney Chompleaf", 0, 2, 3, 1, "------", -2, -2, 0},
    {26, "Razor Crab", 0, 2, 3, 2, "------", 0, -1, 0},
    {27, "Nut Gatherer", 0, 2, 2, 2, "------", 2, 0, 0},
    {28, "Infested Toad", 0, 2, 1, 2, "------", 0, 0, 1},
    {29, "Steelplume Nestling", 0, 2, 2, 1, "------", 0, 0, 1},
    {30, "Venomous Bog Hopper", 0, 3, 4, 2, "------", 0, -2, 0},
    {31, "Woodland Hunter", 0, 3, 3, 1, "------", 0, -1, 0},
    {32, "Sandsplat", 0, 3, 3, 2, "------", 0, 0, 1},
    {33, "Chameleskulk", 0, 4, 4, 3, "------", 0, 0, 1},
    {34, "Eldritch Cyclops", 0, 5, 3, 5, "------", 0, 0, 1},
    {35, "Snail-eyed Hulker", 0, 6, 5, 2, "B-----", 0, 0, 1},
    {36, "Possessed Skull", 0, 6, 4, 4, "------", 0, 0, 2},
    {37, "Eldritch Multiclops", 0, 6, 5, 7, "------", 0, 0, 1},
    {38, "Imp", 0, 1, 1, 3, "--D---", 0, 0, 0},
    {39, "Voracious Imp", 0, 1, 2, 1, "--D---", 0, 0, 0},
    {40, "Rock Gobbler", 0, 3, 2, 3, "--DG--", 0, 0, 0},
    {41, "Blizzard Demon", 0, 3, 2, 2, "-CD---", 0, 0, 0},
    {42, "Flying Leech", 0, 4, 4, 2, "--D---", 0, 0, 0},
    {43, "Screeching Nightmare", 0, 6, 5, 5, "--D---", 0, 0, 0},
    {44, "Deathstalker", 0, 6, 3, 7, "--D-L-", 0, 0, 0},
    {45, "Night Howler", 0, 6, 6, 5, "B-D---", -3, 0, 0},
    {46, "Soul Devourer", 0, 9, 7, 7, "--D---", 0, 0, 0},
    {47, "Gnipper", 0, 2, 1, 5, "--D---", 0, 0, 0},
    {48, "Venom Hedgehog", 0, 1, 1, 1, "----L-", 0, 0, 0},
    {49, "Shiny Prowler", 0, 2, 1, 2, "---GL-", 0, 0, 0},
    {50, "Puff Biter", 0, 3, 3, 2, "----L-", 0, 0, 0},
    {51, "Elite Bilespitter", 0, 4, 3, 5, "----L-", 0, 0, 0},
    {52, "Bilespitter", 0, 4, 2, 4, "----L-", 0, 0, 0},
    {53, "Possessed Abomination", 0, 4, 1, 1, "-C--L-", 0, 0, 0},
    {54, "Shadow Biter", 0, 3, 2, 2, "----L-", 0, 0, 0},
    {55, "Hermit Slime", 0, 2, 0, 5, "---G--", 0, 0, 0},
    {56, "Giant Louse", 0, 4, 2, 7, "------", 0, 0, 0},
    {57, "Dream-Eater", 0, 4, 1, 8, "------", 0, 0, 0},
    {58, "Darkscale Predator", 0, 6, 5, 6, "B-----", 0, 0, 0},
    {59, "Sea Ghost", 0, 7, 7, 7, "------", 1, -1, 0},
    {60, "Gritsuck Troll", 0, 7, 4, 8, "------", 0, 0, 0},
    {61, "Alpha Troll", 0, 9, 10, 10, "------", 0, 0, 0},
    {62, "Mutant Troll", 0, 12, 12, 12, "B--G--", 0, 0, 0},
    {63, "Rootkin Drone", 0, 2, 0, 4, "---G-W", 0, 0, 0},
    {64, "Coppershell Tortoise", 0, 2, 1, 1, "---G-W", 0, 0, 0},
    {65, "Steelplume Defender", 0, 2, 2, 2, "-----W", 0, 0, 0},
    {66, "Staring Wickerbeast", 0, 5, 5, 1, "-----W", 0, 0, 0},
    {67, "Flailing Hammerhead", 0, 6, 5, 5, "-----W", 0, -2, 0},
    {68, "Giant Squid", 0, 6, 7, 5, "-----W", 0, 0, 0},
    {69, "Charging Boarhound", 0, 3, 4, 4, "B-----", 0, 0, 0},
    {70, "Murglord", 0, 4, 6, 3, "B-----", 0, 0, 0},
    {71, "Flying Murgling", 0, 4, 3, 2, "BC----", 0, 0, 0},
    {72, "Shuffling Nightmare", 0, 4, 5, 3, "B-----", 0, 0, 0},
    {73, "Bog Bounder", 0, 4, 4, 4, "B-----", 4, 0, 0},
    {74, "Crusher", 0, 5, 5, 4, "B--G--", 0, 0, 0},
    {75, "Titan Prowler", 0, 5, 6, 5, "B-----", 0, 0, 0},
    {76, "Crested Chomper", 0, 6, 5, 5, "B-D---", 0, 0, 0},
    {77, "Lumbering Giant", 0, 7, 7, 7, "B-----", 0, 0, 0},
    {78, "Shambler", 0, 8, 5, 5, "B-----", 0, -5, 0},
    {79, "Scarlet Colossus", 0, 8, 8, 8, "B-----", 0, 0, 0},
    {80, "Corpse Guzzler", 0, 8, 8, 8, "B--G--", 0, 0, 1},
    {81, "Flying Corpse Guzzler", 0, 9, 6, 6, "BC----", 0, 0, 0},
    {82, "Slithering Nightmare", 0, 7, 5, 5, "B-D--W", 0, 0, 0},
    {83, "Restless Owl", 0, 0, 1, 1, "-C----", 0, 0, 0},
    {84, "Fighter Tick", 0, 2, 1, 1, "-CD--W", 0, 0, 0},
    {85, "Heartless Crow", 0, 3, 2, 3, "-C----", 0, 0, 0},
    {86, "Crazed Nose-pincher", 0, 3, 1, 5, "-C----", 0, 0, 0},
    {87, "Bloat Demon", 0, 4, 2, 5, "-C-G--", 0, 0, 0},
    {88, "Abyss Nightmare", 0, 5, 4, 4, "-C----", 0, 0, 0},
    {89, "Boombeak", 0, 5, 4, 1, "-C----", 2, 0, 0},
    {90, "Eldritch Swooper", 0, 8, 5, 5, "-C----", 0, 0, 0},
    {91, "Flumpy", 0, 0, 1, 2, "---G--", 0, 1, 0},
    {92, "Wurm", 0, 1, 0, 1, "---G--", 2, 0, 0},
    {93, "Spinekid", 0, 1, 2, 1, "---G--", 0, 0, 0},
    {94, "Rootkin Defender", 0, 2, 1, 4, "---G--", 0, 0, 0},
    {95, "Wildum", 0, 2, 2, 3, "---G--", 0, 0, 0},
    {96, "Prairie Protector", 0, 2, 3, 2, "---G--", 0, 0, 0},
    {97, "Turta", 0, 3, 3, 3, "---G--", 0, 0, 0},
    {98, "Lilly Hopper", 0, 3, 2, 4, "---G--", 0, 0, 0},
    {99, "Cave Crab", 0, 3, 2, 5, "---G--", 0, 0, 0},
    {100, "Stalagopod", 0, 3, 1, 6, "---G--", 0, 0, 0},
    {101, "Engulfer", 0, 4, 3, 4, "---G--", 0, 0, 0},
    {102, "Mole Demon", 0, 4, 3, 3, "---G--", 0, -1, 0},
    {103, "Mutating Rootkin", 0, 4, 3, 6, "---G--", 0, 0, 0},
    {104, "Deepwater Shellcrab", 0, 4, 4, 4, "---G--", 0, 0, 0},
    {105, "King Shellcrab", 0, 5, 4, 6, "---G--", 0, 0, 0},
    {106, "Far-reaching Nightmare", 0, 5, 5, 5, "---G--", 0, 0, 0},
    {107, "Worker Shellcrab", 0, 5, 3, 3, "---G--", 3, 0, 0},
    {108, "Rootkin Elder", 0, 5, 2, 6, "---G--", 0, 0, 0},
    {109, "Elder Engulfer", 0, 5, 5, 6, "------", 0, 0, 0},
    {110, "Gargoyle", 0, 5, 0, 9, "---G--", 0, 0, 0},
    {111, "Turta Knight", 0, 6, 6, 6, "---G--", 0, 0, 0},
    {112, "Rootkin Leader", 0, 6, 4, 7, "---G--", 0, 0, 0},
    {113, "Tamed Bilespitter", 0, 6, 2, 4, "---G--", 4, 0, 0},
    {114, "Gargantua", 0, 7, 7, 7, "---G--", 0, 0, 0},
    {115, "Rootkin Warchief", 0, 8, 5, 5, "---G-W", 0, 0, 0},
    {116, "Emperor Nightmare", 0, 12, 8, 8, "BCDGLW", 0, 0, 0},
    {117, "Protein", 1, 1, 1, 1, "B-----", 0, 0, 0},
    {118, "Royal Helm", 1, 0, 0, 3, "------", 0, 0, 0},
    {119, "Serrated Shield", 1, 1, 1, 2, "------", 0, 0, 0},
    {120, "Venomfruit", 1, 2, 1, 0, "----L-", 0, 0, 0},
    {121, "Enchanted Hat", 1, 2, 0, 3, "------", 0, 0, 1},
    {122, "Bolstering Bread", 1, 2, 1, 3, "---G--", 0, 0, 0},
    {123, "Wristguards", 1, 2, 4, 0, "------", 0, 0, 0},
    {124, "Blood Grapes", 1, 3, 2, 1, "--D---", 0, 0, 0},
    {125, "Healthy Veggies", 1, 3, 1, 4, "------", 0, 0, 0},
    {126, "Heavy Shield", 1, 3, 2, 3, "------", 0, 0, 0},
    {127, "Imperial Helm", 1, 3, 0, 6, "------", 0, 0, 0},
    {128, "Enchanted Cloth", 1, 4, 4, 3, "------", 0, 0, 0},
    {129, "Enchanted Leather", 1, 4, 2, 5, "------", 0, 0, 0},
    {130, "Helm of Remedy", 1, 4, 0, 6, "------", 4, 0, 0},
    {131, "Heavy Gauntlet", 1, 4, 4, 1, "------", 0, 0, 0},
    {132, "High Protein", 1, 5, 3, 3, "B-----", 0, 0, 0},
    {133, "Pie of Power", 1, 5, 4, 0, "-----W", 0, 0, 0},
    {134, "Light The Way", 1, 4, 2, 2, "------", 0, 0, 1},
    {135, "Imperial Armour", 1, 6, 5, 5, "------", 0, 0, 0},
    {136, "Buckler", 1, 0, 1, 1, "------", 0, 0, 0},
    {137, "Ward", 1, 2, 0, 0, "-----W", 0, 0, 0},
    {138, "Grow Horns", 1, 2, 0, 0, "---G--", 0, 0, 1},
    {139, "Grow Stingers", 1, 4, 0, 0, "----LW", 0, 0, 0},
    {140, "Grow Wings", 1, 2, 0, 0, "-C----", 0, 0, 0},
    {141, "Throwing Knife", 2, 0, -1, -1, "------", 0, 0, 0},
    {142, "Staff of Suppression", 2, 0, 0, 0, "BCDGLW", 0, 0, 0},
    {143, "Pierce Armour", 2, 0, 0, 0, "---G--", 0, 0, 0},
    {144, "Rune Axe", 2, 1, 0, -2, "------", 0, 0, 0},
    {145, "Cursed Sword", 2, 3, -2, -2, "------", 0, 0, 0},
    {146, "Cursed Scimitar", 2, 4, -2, -2, "------", 0, -2, 0},
    {147, "Quick Shot", 2, 2, 0, -1, "------", 0, 0, 1},
    {148, "Helm Crusher", 2, 2, 0, -2, "BCDGLW", 0, 0, 0},
    {149, "Rootkin Ritual", 2, 3, 0, 0, "BCDGLW", 0, 0, 1},
    {150, "Throwing Axe", 2, 2, 0, -3, "------", 0, 0, 0},
    {151, "Decimate", 2, 5, 0, -99, "BCDGLW", 0, 0, 0},
    {152, "Mighty Throwing Axe", 2, 7, 0, -7, "------", 0, 0, 1},
    {153, "Healing Potion", 3, 2, 0, 0, "------", 5, 0, 0},
    {154, "Poison", 3, 2, 0, 0, "------", 0, -2, 1},
    {155, "Scroll of Firebolt", 3, 3, 0, -3, "------", 0, -1, 0},
    {156, "Major Life Steal Potion", 3, 3, 0, 0, "------", 3, -3, 0},
    {157, "Life Sap Drop", 3, 3, 0, -1, "------", 1, 0, 1},
    {158, "Tome of Thunder", 3, 3, 0, -4, "------", 0, 0, 0},
    {159, "Vial of Soul Drain", 3, 4, 0, -3, "------", 3, 0, 0},
    {160, "Minor Life Steal Potion", 3, 2, 0, 0, "------", 2, -2, 0},
};

// ----------------------------------------------------------------------------
// Cards
// ----------------------------------------------------------------------------
struct Keywords {
    bool hasBreakthrough = false, hasCharge = false, hasDrain = false, hasGuard = false, hasLethal = false, hasWard = false;

    Keywords() {}
    explicit Keywords(const char* data) {
        hasBreakthrough = data[0] == 'B';
        hasCharge = data[1] == 'C';
        hasDrain = data[2] == 'D';
        hasGuard = data[3] == 'G';
        hasLethal = data[4] == 'L';
        hasWard = data[5] == 'W';
    }
    bool hasAnyKeyword() const { return hasBreakthrough || hasCharge || hasDrain || hasGuard || hasLethal || hasWard; }
    std::vector<std::string> getListOfKeywords() const {
        std::vector<std::string> k;
        if (hasBreakthrough) k.push_back("Breakthrough");
        if (hasCharge) k.push_back("Charge");
        if (hasDrain) k.push_back("Drain");
        if (hasGuard) k.push_back("Guard");
        if (hasLethal) k.push_back("Lethal");
        if (hasWard) k.push_back("Ward");
        return k;
    }
    std::string toString() const {
        std::string s;
        s += hasBreakthrough ? 'B' : '-';
        s += hasCharge ? 'C' : '-';
        s += hasDrain ? 'D' : '-';
        s += hasGuard ? 'G' : '-';
        s += hasLethal ? 'L' : '-';
        s += hasWard ? 'W' : '-';
        return s;
    }
};

static std::string joinStrings(const std::vector<std::string>& v, const char* sep) {
    std::string r;
    for (size_t i = 0; i < v.size(); i++) r += (i ? sep : "") + v[i];
    return r;
}

// String.format("%+d", v)
static std::string signedInt(int v) { return (v >= 0 ? "+" : "") + S(v); }

struct Card {
    int id = -1;
    int baseId = 0;
    int type = CREATURE;
    int cost = 0, attack = 0, defense = 0;
    Keywords keywords;
    int myHealthChange = 0, oppHealthChange = 0, cardDraw = 0;
    std::string name, text;

    void generateText() {
        std::string sb;
        std::vector<std::string> kw = keywords.getListOfKeywords();
        std::vector<std::string> summon;
        if (myHealthChange > 0) summon.push_back("gain " + S(myHealthChange) + " health");
        if (myHealthChange < 0) summon.push_back("deal " + S(std::abs(myHealthChange)) + " damage to yourself");
        if (oppHealthChange < 0) summon.push_back("deal " + S(std::abs(oppHealthChange)) + " damage to your opponent");
        if (cardDraw > 0) summon.push_back("draw " + (cardDraw == 1 ? std::string("a card") : S(cardDraw) + " cards"));
        bool effects = myHealthChange != 0 || oppHealthChange != 0 || cardDraw != 0;
        if (type == CREATURE) {
            sb += joinStrings(kw, ", ");
            if (effects) {
                sb += kw.empty() ? "" : "; ";
                sb += "Summon: ";
                sb += joinStrings(summon, ", ") + ".";
            }
        } else if (type == ITEM_GREEN) {
            if (!kw.empty()) sb += "Give " + joinStrings(kw, ", ");
            if (effects) {
                sb += kw.empty() ? "" : "; ";
                sb += joinStrings(summon, ", ") + ".";
            }
        } else if (type == ITEM_RED) {
            if (!kw.empty()) sb += "Remove " + (kw.size() == 7 ? std::string("all keywords") : joinStrings(kw, ", "));
            if (effects) {
                sb += kw.empty() ? "" : "; ";
                sb += joinStrings(summon, ", ") + ".";
            }
        } else {
            sb += joinStrings(summon, ", ") + ".";
        }
        text = sb;
    }

    std::string toDescriptiveString() const {
        std::string sb;
        if (id >= 0) sb += "id:" + S(id) + ' ';
        if (name != "?") sb += name + ' ';
        sb += "(#" + S(baseId) + ")" + ' ';
        sb += std::string(TYPE_DESCRIPTION[type]) + ' ';
        sb += "COST:" + S(cost) + ' ';
        if (type == CREATURE) {
            sb += "ATT:" + S(attack) + ' ';
            sb += "DEF:" + S(defense) + ' ';
        } else {
            sb += "ATT:" + signedInt(attack) + ' ';
            sb += "DEF:" + signedInt(defense) + ' ';
        }
        sb += " " + text;
        return sb;
    }

    std::string getAsInput() const {
        return S(baseId) + " " + S(id) + " " + S(0) + " " + S(type) + " " + S(cost) + " " + S(attack) + " " + S(defense) + " " + keywords.toString() + " " +
               S(myHealthChange) + " " + S(oppHealthChange) + " " + S(cardDraw) + " ";
    }
};

// Constants.CARDSET (a HashMap<Integer, Card> of the ids 1-160: its values() in id order)
static std::vector<Card> CARDSET;

static void loadCardlist() {
    CARDSET.clear();
    for (const CardData& d : CARDLIST) {
        Card c;
        c.id = -1;
        c.baseId = d.baseId;
        c.name = d.name;
        c.type = d.type;
        c.cost = d.cost;
        c.attack = d.attack;
        c.defense = d.defense;
        c.keywords = Keywords(d.keywords);
        c.myHealthChange = d.myHealthChange;
        c.oppHealthChange = d.oppHealthChange;
        c.cardDraw = d.cardDraw;
        c.generateText();
        CARDSET.push_back(c);
    }
}

// CreatureOnBoard
struct Creature {
    int id = 0, baseId = 0, attack = 0, defense = 0, cost = 0;
    int myHealthChange = 0, oppHealthChange = 0, cardDraw = 0;
    Keywords keywords;
    bool canAttack = false, hasAttacked = false;
    int lastTurnDefense = 0;
    int lane = 0;

    // new CreatureOnBoard(Card card, int lane)
    static Creature fromCard(const Card& card, int lane) {
        Creature c;
        c.id = card.id;
        c.baseId = card.baseId;
        c.attack = card.attack;
        c.defense = card.defense;
        c.keywords = card.keywords;
        c.canAttack = c.keywords.hasCharge;
        c.lastTurnDefense = card.defense;
        c.cost = card.cost;
        c.myHealthChange = card.myHealthChange;
        c.oppHealthChange = card.oppHealthChange;
        c.cardDraw = card.cardDraw;
        c.lane = lane;
        return c;
    }
    // new CreatureOnBoard(CreatureOnBoard creature): myHealthChange, oppHealthChange and cardDraw are
    // not copied (0), which the players then see in their inputs
    Creature copy() const {
        Creature c;
        c.id = id;
        c.baseId = baseId;
        c.cost = cost;
        c.attack = attack;
        c.defense = defense;
        c.keywords = keywords;
        c.lastTurnDefense = lastTurnDefense;
        c.canAttack = canAttack;
        c.hasAttacked = hasAttacked;
        c.lane = lane;
        return c;
    }

    std::string getAsInput(bool isOpponentBoard) const {
        int position = isOpponentBoard ? -1 : 1;
        return S(baseId) + " " + S(id) + " " + S(position) + " " + S(CREATURE) + " " + S(cost) + " " + S(attack) + " " + S(defense) + " " + keywords.toString() + " " +
               S(myHealthChange) + " " + S(oppHealthChange) + " " + S(cardDraw) + " ";
    }
};

// ----------------------------------------------------------------------------
// Actions
// ----------------------------------------------------------------------------
struct InvalidActionHard {
    String message;
};
struct TimeoutException {}; // AbstractPlayer.TimeoutException

enum ActionType { SUMMON = 0, ATTACK = 1, USE = 2, PASS = 3 };
static const char* ACTION_TYPE_NAMES[4] = {"SUMMON", "ATTACK", "USE", "PASS"};

struct ActionResult {
    bool isValid = true;
    bool attackerDied = false, defenderDied = false;
    bool hasAttacker = false, hasDefender = false;
    Creature attacker, defender;
    int attackerHealthChange = 0, defenderHealthChange = 0;
};

struct Action {
    ActionType type = PASS;
    int arg1 = 0, arg2 = 0;
    String text;

    static Action make(ActionType t, int a1, int a2, const String& text) {
        Action a;
        a.type = t;
        a.arg1 = a1;
        a.arg2 = a2;
        a.text = text;
        return a;
    }

    std::string toStringNoText() const {
        switch (type) {
        case SUMMON: return "SUMMON " + S(arg1);
        case ATTACK: return "ATTACK " + S(arg1) + " " + S(arg2);
        case USE: return "USE " + S(arg1) + " " + S(arg2);
        default: return "PASS";
        }
    }
    String toString() const {
        switch (type) {
        case SUMMON: return A("SUMMON " + S(arg1) + " ") + text;
        case ATTACK: return A("ATTACK " + S(arg1) + " " + S(arg2) + " ") + text;
        case USE: return A("USE " + S(arg1) + " " + S(arg2) + " ") + text;
        default: return A("PASS");
        }
    }
    // Action#equals: the same toStringNoText()
    bool equals(const Action& o) const {
        if (type != o.type) return false;
        if (type == PASS) return true;
        if (type == SUMMON) return arg1 == o.arg1;
        return arg1 == o.arg1 && arg2 == o.arg2;
    }

    // Action.parse (Constants.LANES == 1)
    static Action parse(const String& data) {
        std::vector<String> str = java::split(data, u' ', 2);
        String name = java::trim(str[0]);
        ActionType type;
        if (name == u"SUMMON") type = SUMMON;
        else if (name == u"ATTACK") type = ATTACK;
        else if (name == u"USE") type = USE;
        else if (name == u"PASS") type = PASS;
        else throw InvalidActionHard{A("Invalid action name. Should be SUMMON, ATTACK, or USE.")};

        if (type == SUMMON) {
            // str[1] (ArrayIndexOutOfBounds) and parseInt failures are caught: one message
            if (str.size() < 2) throw InvalidActionHard{A("Invalid SUMMON argument. Expected integer (card id).")};
            std::vector<String> args = java::split(str[1], u' ', 2);
            i32 arg1;
            if (!java::parseInt(args[0], arg1)) throw InvalidActionHard{A("Invalid SUMMON argument. Expected integer (card id).")};
            String text = args.size() < 2 ? String() : java::trim(args[1]);
            return make(SUMMON, arg1, 0, text);
        } else if (type == PASS) {
            return make(PASS, 0, 0, String());
        } else {
            String err = A("Invalid ") + A(ACTION_TYPE_NAMES[type]) + A(" arguments. Expected two integers (card id and target id).");
            if (str.size() < 2) throw InvalidActionHard{err};
            std::vector<String> args = java::split(str[1], u' ', 3);
            i32 arg1, arg2;
            if (!java::parseInt(args[0], arg1)) throw InvalidActionHard{err};
            if (args.size() < 2 || !java::parseInt(args[1], arg2)) throw InvalidActionHard{err};
            String text = args.size() < 3 ? String() : java::trim(args[2]);
            return make(type, arg1, arg2, text);
        }
    }

    // Action.parseSequence
    static std::vector<Action> parseSequence(const String& data) {
        std::vector<Action> actions;
        for (const String& part : java::split(data, u';')) {
            String s = java::trim(part);
            if (s.empty()) continue; // empty action is a valid action
            actions.push_back(parse(s));
        }
        return actions;
    }
};

// ----------------------------------------------------------------------------
// Gamer
// ----------------------------------------------------------------------------
struct Gamer {
    int id = 0;
    int bonusManaTurns = 0;
    std::vector<Card> hand, deck;
    std::vector<Creature> board, graveyard;
    int health = 0, maxMana = 0, currentMana = 0, nextTurnDraw = 0, drawValueToShow = 0;
    std::vector<int> runes{5, 10, 15, 20, 25};
    std::vector<Action> performedActions;
    int handLimit = 0;

    void init(int id_, const std::vector<Card>& deck_) {
        id = id_;
        hand.clear();
        deck = deck_;
        board.clear();
        graveyard.clear();
        performedActions.clear();
        health = INITIAL_HEALTH;
        maxMana = id == 1 && SECOND_PLAYER_MANA_BONUS_TURNS > 0 ? 1 : 0;
        currentMana = maxMana;
        nextTurnDraw = 1;
        drawValueToShow = nextTurnDraw;
        bonusManaTurns = id == 1 ? SECOND_PLAYER_MANA_BONUS_TURNS : 0;
        handLimit = MAX_CARDS_IN_HAND + (id == 0 ? 0 : SECOND_PLAYER_MAX_CARD_BONUS);
        DrawCards(INITIAL_HAND_SIZE + (id == 0 ? 0 : SECOND_PLAYER_CARD_BONUS), 0);
    }

    void suicideRunes() {
        if (!runes.empty()) { // first rune gone
            health = runes.back();
            runes.pop_back();
        } else { // final run gone - suicide
            health = 0;
        }
    }

    void DrawCards(int n, int playerturn) {
        for (int i = 0; i < n; i++) {
            if (deck.empty() || playerturn >= PLAYER_TURNLIMIT) {
                suicideRunes();
                continue;
            }
            if ((int)hand.size() == handLimit) break; // additional draws are simply wasted
            hand.push_back(deck.front());
            deck.erase(deck.begin());
        }
    }

    void ModifyHealth(int mod) {
        health += mod;
        if (mod >= 0) return;
        for (int r = (int)runes.size() - 1; r >= 0; r--) { // rune checking
            if (health <= runes[r]) {
                nextTurnDraw += 1;
                runes.erase(runes.begin() + r);
            }
        }
    }

    int nextRune() const { return runes.empty() ? 0 : runes.back(); }

    void removeFromBoard(int creatureIndex) {
        graveyard.push_back(board[creatureIndex]);
        board.erase(board.begin() + creatureIndex);
    }

    std::string getPlayerInput() const { return S(health) + " " + S(maxMana) + " " + S((i64)deck.size()) + " " + S(nextRune()) + " " + S(drawValueToShow); }
};

// ----------------------------------------------------------------------------
// GameState
// ----------------------------------------------------------------------------
struct GameState {
    int turn = -1;
    int winner = -1;
    int currentPlayer = 1;
    Gamer players[2];
    std::map<int, Card> cardIdMap; // HashMap<Integer, Card>: lookups only

    void init(const std::vector<Card> decks[2]) {
        turn = -1;
        winner = -1;
        currentPlayer = 1;
        players[0].init(0, decks[0]);
        players[1].init(1, decks[1]);
        cardIdMap.clear();
        for (int i = 0; i < 2; i++)
            for (const Card& c : decks[i]) cardIdMap[c.id] = c;
    }

    std::vector<Action> computeLegalActions() const {
        std::vector<Action> legals;
        computeLegalSummons(legals);
        computeLegalAttacks(legals);
        computeLegalItems(legals);
        legals.push_back(Action::make(PASS, 0, 0, String()));
        return legals;
    }

    void computeLegalSummons(std::vector<Action>& actions) const {
        const Gamer& player = players[currentPlayer];
        if ((int)player.board.size() == MAX_CREATURES_IN_LINE) return;
        for (const Card& c : player.hand) {
            if (c.type != CREATURE || c.cost > player.currentMana) continue;
            actions.push_back(Action::make(SUMMON, c.id, 0, String()));
        }
    }

    std::vector<int> computeLegalTargets() const {
        const Gamer& enemyPlayer = players[1 - currentPlayer];
        std::vector<int> targets;
        for (const Creature& c : enemyPlayer.board) // First priority - guards
            if (c.keywords.hasGuard && c.lane == 0) targets.push_back(c.id);
        if (targets.empty()) { // if no guards we can freely attack any creature plus face
            targets.push_back(-1);
            for (const Creature& c : enemyPlayer.board)
                if (c.lane == 0) targets.push_back(c.id);
        }
        return targets;
    }

    void computeLegalAttacks(std::vector<Action>& actions) const {
        const Gamer& player = players[currentPlayer];
        std::vector<int> targets = computeLegalTargets();
        for (const Creature& c : player.board) {
            if (!c.canAttack) continue;
            for (int tid : targets) actions.push_back(Action::make(ATTACK, c.id, tid, String()));
        }
    }

    void computeLegalItems(std::vector<Action>& actions) const {
        const Gamer& player = players[currentPlayer];
        for (const Card& c : player.hand) {
            if (c.type == CREATURE || c.cost > player.currentMana) continue;
            if (c.type == ITEM_GREEN) { // on friendly creatures
                for (const Creature& cb : player.board) actions.push_back(Action::make(USE, c.id, cb.id, String()));
            } else { // red or blue item: on enemy creatures
                for (const Creature& cb : players[1 - currentPlayer].board) actions.push_back(Action::make(USE, c.id, cb.id, String()));
                if (c.type == ITEM_BLUE) actions.push_back(Action::make(USE, c.id, -1, String())); // blue also on the player
            }
        }
    }

    void AdvanceState() {
        CheckWinCondition();
        for (Creature& c : players[currentPlayer].board) {
            c.canAttack = false;
            c.hasAttacked = false;
        }
        players[currentPlayer].drawValueToShow = players[currentPlayer].nextTurnDraw;
        currentPlayer = 1 - currentPlayer;
        Gamer& player = players[currentPlayer];
        player.performedActions.clear();
        turn++;
        if (player.maxMana < MAX_MANA + (player.bonusManaTurns > 0 ? 1 : 0)) player.maxMana += 1;
        if (player.bonusManaTurns > 0 && player.currentMana == 0) {
            player.bonusManaTurns--;
            if (player.bonusManaTurns == 0) player.maxMana--;
        }
        player.currentMana = player.maxMana;
        for (Creature& c : player.board) {
            c.canAttack = true; // mark ALL creatures as ready to charge
            c.lastTurnDefense = c.defense;
        }
        player.DrawCards(player.nextTurnDraw, turn / 2);
        player.drawValueToShow = player.nextTurnDraw;
        player.nextTurnDraw = 1;
        CheckWinCondition();
    }

    static int indexOfId(const std::vector<Card>& v, int id) {
        for (size_t i = 0; i < v.size(); i++)
            if (v[i].id == id) return (int)i;
        return -1;
    }
    // the loops of AdvanceState(action): the last creature with that id (ids are unique)
    static int lastIndexOfId(const std::vector<Creature>& v, int id) {
        int index = -1;
        for (size_t i = 0; i < v.size(); i++)
            if (v[i].id == id) index = (int)i;
        return index;
    }
    static const Creature& at(const std::vector<Creature>& v, int i) {
        if (i < 0 || i >= (int)v.size()) throw java::Exception{java::indexOutOfBoundsException(i, (i64)v.size())};
        return v[i];
    }

    // ASSUMING THE ACTION IS LEGAL !
    void AdvanceState(Action& action) {
        Gamer& me = players[currentPlayer];
        Gamer& opp = players[1 - currentPlayer];
        if (action.type == SUMMON) {
            const Card c = cardIdMap.at(action.arg1);
            int hi = indexOfId(me.hand, c.id); // hand.remove(c)
            if (hi >= 0) me.hand.erase(me.hand.begin() + hi);
            me.currentMana -= c.cost;
            Creature creature = Creature::fromCard(c, 0);
            me.board.push_back(creature);
            me.ModifyHealth(c.myHealthChange);
            opp.ModifyHealth(c.oppHealthChange);
            me.nextTurnDraw += c.cardDraw;
        } else if (action.type == ATTACK) {
            int indexatt = lastIndexOfId(me.board, action.arg1);
            Creature att = at(me.board, indexatt);
            ActionResult result;
            if (action.arg2 == -1) { // attacking player
                result = ResolveAttack(att);
            } else {
                int indexdef = lastIndexOfId(opp.board, action.arg2);
                Creature def = at(opp.board, indexdef);
                result = ResolveAttack(att, def);
                if (result.defenderDied) opp.removeFromBoard(indexdef);
                else opp.board[indexdef] = result.defender;
            }
            if (result.attackerDied) me.removeFromBoard(indexatt);
            else me.board[indexatt] = result.attacker;
            me.ModifyHealth(result.attackerHealthChange);
            opp.ModifyHealth(result.defenderHealthChange);
        } else if (action.type == USE) {
            const Card item = cardIdMap.at(action.arg1);
            int hi = indexOfId(me.hand, item.id);
            if (hi >= 0) me.hand.erase(me.hand.begin() + hi);
            me.currentMana -= item.cost;
            if (item.type == ITEM_GREEN) { // here we assume that green cards never remove friendly creatures!
                int indextarg = lastIndexOfId(me.board, action.arg2);
                Creature targ = at(me.board, indextarg);
                ActionResult result = ResolveUse(item, targ);
                me.board[indextarg] = result.defender;
                me.ModifyHealth(result.attackerHealthChange);
                opp.ModifyHealth(result.defenderHealthChange);
                me.nextTurnDraw += item.cardDraw;
            } else { // red and blue cards
                ActionResult result;
                if (action.arg2 == -1) { // using on player
                    result = ResolveUse(item);
                } else { // using on creature
                    int indextarg = lastIndexOfId(opp.board, action.arg2);
                    Creature targ = at(opp.board, indextarg);
                    result = ResolveUse(item, targ);
                    if (result.defenderDied) opp.removeFromBoard(indextarg);
                    else opp.board[indextarg] = result.defender;
                }
                me.ModifyHealth(result.attackerHealthChange);
                opp.ModifyHealth(result.defenderHealthChange);
                me.nextTurnDraw += item.cardDraw;
            }
        }
        me.performedActions.push_back(action);
        CheckWinCondition();
    }

    void CheckWinCondition() {
        if (players[1 - currentPlayer].health <= 0) winner = currentPlayer; // first proper win
        else if (players[currentPlayer].health <= 0) winner = 1 - currentPlayer; // second self-kill
    }

    // when creature attacks creatures (legal actions only: the attacker can attack)
    static ActionResult ResolveAttack(const Creature& attacker, const Creature& defender) {
        Creature attackerAfter = attacker.copy();
        Creature defenderAfter = defender.copy();
        bool attackerAlive = true, defenderAlive = true; // attackerAfter / defenderAfter != null
        attackerAfter.canAttack = false;
        attackerAfter.hasAttacked = true;
        if (defender.keywords.hasWard) defenderAfter.keywords.hasWard = attacker.attack == 0;
        if (attacker.keywords.hasWard) attackerAfter.keywords.hasWard = defender.attack == 0;
        int damageGiven = defender.keywords.hasWard ? 0 : attacker.attack;
        int damageTaken = attacker.keywords.hasWard ? 0 : defender.attack;
        int healthGain = 0;
        int healthTaken = 0;
        // attacking
        if (damageGiven >= defender.defense) defenderAlive = false;
        if (attacker.keywords.hasBreakthrough && !defenderAlive) healthTaken = defender.defense - damageGiven;
        if (attacker.keywords.hasLethal && damageGiven > 0) defenderAlive = false;
        if (attacker.keywords.hasDrain && damageGiven > 0) healthGain = attacker.attack;
        if (defenderAlive) defenderAfter.defense -= damageGiven;
        // defending
        if (damageTaken >= attacker.defense) attackerAlive = false;
        if (defender.keywords.hasLethal && damageTaken > 0) attackerAlive = false;
        if (attackerAlive) attackerAfter.defense -= damageTaken;
        ActionResult result;
        result.hasAttacker = result.hasDefender = true;
        result.attacker = attackerAlive ? attackerAfter : attacker;
        result.defender = defenderAlive ? defenderAfter : defender;
        result.attackerDied = !attackerAlive;
        result.defenderDied = !defenderAlive;
        result.attackerHealthChange = healthGain;
        result.defenderHealthChange = healthTaken;
        return result;
    }

    // when creature attacks player
    static ActionResult ResolveAttack(const Creature& attacker) {
        Creature attackerAfter = attacker.copy();
        attackerAfter.canAttack = false;
        attackerAfter.hasAttacked = true;
        int healthGain = attacker.keywords.hasDrain ? attacker.attack : 0;
        int healthTaken = -attacker.attack;
        ActionResult result;
        result.hasAttacker = true;
        result.attacker = attackerAfter;
        result.attackerHealthChange = healthGain;
        result.defenderHealthChange = healthTaken;
        return result;
    }

    // when item is used on a creature
    static ActionResult ResolveUse(const Card& item, const Creature& target) {
        Creature targetAfter = target.copy();
        const Keywords& t = target.keywords;
        const Keywords& k = item.keywords;
        Keywords& a = targetAfter.keywords;
        if (item.type == ITEM_GREEN) { // add keywords
            a.hasCharge = t.hasCharge || k.hasCharge;
            if (k.hasCharge) targetAfter.canAttack = !targetAfter.hasAttacked; // No Swift Strike hack
            a.hasBreakthrough = t.hasBreakthrough || k.hasBreakthrough;
            a.hasDrain = t.hasDrain || k.hasDrain;
            a.hasGuard = t.hasGuard || k.hasGuard;
            a.hasLethal = t.hasLethal || k.hasLethal;
            a.hasWard = t.hasWard || k.hasWard;
        } else { // Assumming ITEM_BLUE or ITEM_RED - remove keywords
            a.hasCharge = t.hasCharge && !k.hasCharge;
            a.hasBreakthrough = t.hasBreakthrough && !k.hasBreakthrough;
            a.hasDrain = t.hasDrain && !k.hasDrain;
            a.hasGuard = t.hasGuard && !k.hasGuard;
            a.hasLethal = t.hasLethal && !k.hasLethal;
            a.hasWard = t.hasWard && !k.hasWard;
        }
        targetAfter.attack = std::max(0, target.attack + item.attack);
        if (targetAfter.keywords.hasWard && item.defense < 0) targetAfter.keywords.hasWard = false;
        else targetAfter.defense += item.defense;
        bool alive = targetAfter.defense > 0;
        ActionResult result;
        result.hasAttacker = result.hasDefender = true;
        result.defender = alive ? targetAfter : target;
        result.defenderDied = !alive;
        result.attackerHealthChange = item.myHealthChange;
        result.defenderHealthChange = item.oppHealthChange;
        return result;
    }

    // when item is used on a player
    static ActionResult ResolveUse(const Card& item) {
        ActionResult result;
        result.attackerHealthChange = item.myHealthChange;
        result.defenderHealthChange = item.defense + item.oppHealthChange;
        return result;
    }

    std::vector<std::string> getPlayersInput() const {
        return {players[currentPlayer].getPlayerInput(), players[1 - currentPlayer].getPlayerInput()};
    }

    std::vector<std::string> getCardsInput() const {
        std::vector<std::string> lines;
        const Gamer& player = players[currentPlayer];
        const Gamer& opponent = players[1 - currentPlayer];
        lines.push_back(S((i64)opponent.hand.size()) + " " + S((i64)opponent.performedActions.size()));
        for (const Action& a : opponent.performedActions) lines.push_back(S(cardIdMap.at(a.arg1).baseId) + " " + a.toStringNoText());
        lines.push_back(S((i64)(player.hand.size() + player.board.size() + opponent.board.size())));
        for (const Card& c : player.hand) lines.push_back(c.getAsInput());
        for (const Creature& b : player.board) lines.push_back(b.getAsInput(false));
        for (const Creature& b : opponent.board) lines.push_back(b.getAsInput(true));
        return lines;
    }
};

// ----------------------------------------------------------------------------
// RefereeParams and DraftPhase
// ----------------------------------------------------------------------------
struct RefereeParams {
    i64 draftChoicesSeed = 0, shufflePlayer0Seed = 0, shufflePlayer1Seed = 0;

    // RefereeParams(gameManager): the seeds from new Random(seed); the game parameters only hold the
    // "seed" the runner sent, which is the same number
    explicit RefereeParams(i64 mainSeed) {
        java::Random rng(mainSeed);
        draftChoicesSeed = rng.nextLong();
        shufflePlayer0Seed = rng.nextLong();
        shufflePlayer1Seed = rng.nextLong();
    }
};

struct DraftPhase {
    std::vector<int> allowedCards;  // indices in CARDSET
    std::vector<int> draftingCards; // indices in CARDSET
    int draft[CARDS_IN_DECK][3];    // indices in CARDSET
    std::vector<int> chosenCards[2]; // indices in CARDSET
    std::vector<Card> decks[2];      // after shuffle and assigning unique id's
    String text[2];
    java::Random choicesRNG;
    java::Random shufflesRNG[2];

    void init(const RefereeParams& params) {
        for (int p = 0; p < 2; p++) {
            chosenCards[p].clear();
            decks[p].clear();
            text[p].clear();
        }
        choicesRNG = java::Random(params.draftChoicesSeed);
        shufflesRNG[0] = java::Random(params.shufflePlayer0Seed);
        shufflesRNG[1] = java::Random(params.shufflePlayer1Seed);
    }

    // Difficulty.NORMAL: every card
    void prepareAllowedCards() {
        allowedCards.clear();
        for (int i = 0; i < (int)CARDSET.size(); i++) allowedCards.push_back(i);
    }

    void PrepareChoices() {
        prepareAllowedCards();
        std::vector<int> drafting;
        int n = std::min(CARDS_IN_DRAFT, (int)allowedCards.size());
        for (int pick = 0; pick < n; pick++) {
            int i;
            do {
                i = choicesRNG.nextInt((i32)allowedCards.size());
            } while (std::find(drafting.begin(), drafting.end(), i) != drafting.end());
            drafting.push_back(i);
        }
        draftingCards.clear();
        for (int i : drafting) draftingCards.push_back(allowedCards[i]);
        for (int pick = 0; pick < CARDS_IN_DECK; pick++) {
            int choice1 = drafting[choicesRNG.nextInt((i32)drafting.size())];
            int choice2;
            do {
                choice2 = drafting[choicesRNG.nextInt((i32)drafting.size())];
            } while (choice2 == choice1);
            int choice3;
            do {
                choice3 = drafting[choicesRNG.nextInt((i32)drafting.size())];
            } while (choice3 == choice1 || choice3 == choice2);
            draft[pick][0] = allowedCards[choice1];
            draft[pick][1] = allowedCards[choice2];
            draft[pick][2] = allowedCards[choice3];
        }
        // prepareShowdraft: the view's sorted copy of draftingCards
    }

    // PlayerChoice: the card (index in CARDSET) and the text
    std::pair<int, String> PlayerChoice(int pickNumber, const String& action, int player) {
        const int* cards = draft[pickNumber];
        int choice = -1;
        String txt;
        const String generic = A("Invalid action. Expected  \"PICK [0,1,2]\" or \"PASS\".");
        std::vector<String> command = java::split(action, u' ', 3);
        txt = command.size() < 3 ? String() : java::trim(command[2]);
        if (command[0] != u"PICK" && command[0] != u"CHOOSE" && command[0] != u"PASS") throw InvalidActionHard{generic};
        if (command[0] == u"PASS") {
            choice = cards[0];
        } else if (command[0] == u"PICK") {
            i32 value;
            if (command.size() < 2 || !java::parseInt(command[1], value)) throw InvalidActionHard{generic};
            if (value < 0 || value > 2) throw InvalidActionHard{A("Invalid action format. \"PICK\" argument should be 0, 1 or 2.")};
            choice = cards[value];
        } else { // "CHOOSE"
            // a HashSet<Integer> of the three base ids, printed in its iteration order
            std::vector<i32> ids;
            for (int k = 0; k < 3; k++) {
                i32 id = CARDSET[cards[k]].baseId;
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            }
            i32 value;
            if (command.size() < 2 || !java::parseInt(command[1], value)) throw InvalidActionHard{generic};
            if (std::find(ids.begin(), ids.end(), value) == ids.end()) {
                std::string set = "[";
                std::vector<int> order = java::hashMapOrder(ids);
                for (size_t k = 0; k < order.size(); k++) set += (k ? ", " : "") + S(ids[order[k]]);
                throw InvalidActionHard{A("Invalid action format. \"CHOOSE\" argument should be valid card's base id " + set + "].")};
            }
            choice = value - 1; // Constants.CARDSET.get(value)
        }
        chosenCards[player].push_back(choice);
        return {choice, txt};
    }

    void ShuffleDecks() {
        for (int player = 0; player < 2; player++) {
            for (int c : chosenCards[player]) decks[player].push_back(CARDSET[c]); // new Card(c)
            java::shuffle(decks[player], shufflesRNG[player]);
            for (int i = 0; i < (int)decks[player].size(); i++) decks[player][i].id = 2 * i + player + 1;
        }
    }

    std::vector<std::string> getMockPlayersInput(int player, int turn) const {
        return {S(INITIAL_HEALTH) + " 0 " + S(turn) + " 25 0", S(INITIAL_HEALTH) + " 0 " + S(player == 0 ? turn : turn + 1) + " 25 0", "0 0", "3"};
    }
};

// ----------------------------------------------------------------------------
// Players and referee (EngineReferee)
// ----------------------------------------------------------------------------
struct Player : cg::Player {
    int expectedOutputLines = 1;
    int getExpectedOutputLines() const override { return expectedOutputLines; }

    // getOutputs().get(0)
    String output0() const {
        if (timeout) throw TimeoutException{};
        if (outputs.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
        return outputs[0];
    }
};
static Player players[4];

struct EngineReferee {
    cg::GameManager& gm = cg::gm;
    DraftPhase draft;
    GameState state;
    bool hasState = false; // state != null
    int gamePlayer = 0;
    int gameTurn = 0;
    std::vector<Action> actionsToHandle;
    bool showBattleStart = true;
    bool showDraftStart = true;
    int showdraftSizechoice = 0;

    Player& player(int i) { return *static_cast<Player*>(gm.getPlayer(i)); }

    void refereeInit() {
        RefereeParams params(gm.getSeed());
        // the league switch: league.level 19 is the default branch, Difficulty.NORMAL
        loadCardlist();
        draft.init(params);
        draft.PrepareChoices();
        int showdraftCards = (int)draft.draftingCards.size();
        for (int i = 0; i < 4; i++) {
            if (showdraftCards > SHOWDRAFT_SIZECHOICE[i]) break;
            showdraftSizechoice = i;
        }
        int rowSize = SHOWDRAFT_ROWSIZE[showdraftSizechoice];
        gameTurn = -1 - (int)std::ceil(std::max(0, showdraftCards - rowSize) / (double)rowSize);
        gm.setMaxTurns(MAX_TURNS_HARDLIMIT);
        // the game parameters: the seeds RefereeParams adds to them, in the order the jar prints them
        // (java.util.Properties, a ConcurrentHashMap since JDK 9: fixed for these four keys)
        gm.setGameParameters({"draftChoicesSeed=" + S(params.draftChoicesSeed), "shufflePlayer0Seed=" + S(params.shufflePlayer0Seed), "seed=" + S(gm.getSeed()),
                              "shufflePlayer1Seed=" + S(params.shufflePlayer1Seed)});
    }

    void refereeGameTurn() {
        if (showDraftStart && gameTurn == 0) {
            showDraftStart = false;
            gm.addTooltip(0, A("Draft phase."));
        }
        if (showBattleStart && gameTurn == CARDS_IN_DECK) {
            showBattleStart = false;
            gm.addTooltip(0, A("Battle phase."));
        }
        if (gameTurn < 0) {
            player(0).expectedOutputLines = 0;
            player(0).execute();
            player(0).expectedOutputLines = 1;
            gameTurn++;
            return;
        }
        if (gameTurn < CARDS_IN_DECK) DraftTurn();
        else GameTurn();
    }

    void DraftTurn() {
        gm.setTurnMaxTime(gameTurn == 0 ? TIMELIMIT_FIRSTDRAFTTURN : TIMELIMIT_DRAFTTURN);
        for (int p = 0; p < 2; p++) {
            Player& sdkplayer = player(p);
            for (const std::string& line : draft.getMockPlayersInput(p, gameTurn)) sdkplayer.sendInputLine(line);
            for (int card = 0; card < 3; card++) sdkplayer.sendInputLine(CARDSET[draft.draft[gameTurn][card]].getAsInput());
            sdkplayer.execute();
        }
        for (int p = 0; p < 2; p++) {
            Player& sdkplayer = player(p);
            try {
                String output = sdkplayer.output0();
                std::pair<int, String> choice = draft.PlayerChoice(gameTurn, output, p);
                draft.text[p] = choice.second;
                gm.addToGameSummary(A("Player ") + sdkplayer.getNicknameToken() + A(" chose ") + A(CARDSET[choice.first].toDescriptiveString()));
            } catch (const InvalidActionHard& e) {
                HandleError(sdkplayer, sdkplayer.getNicknameToken() + u": " + e.message);
                return;
            } catch (const TimeoutException&) {
                HandleError(sdkplayer, sdkplayer.getNicknameToken() + u" timeout!");
                return;
            }
        }
        // render: ui.draft(gameTurn)
        gm.setFrameDuration(FRAME_DURATION_DRAFT);
        gameTurn++;
    }

    void GameTurn() {
        Player& sdkplayer = player(gamePlayer);
        gm.setFrameDuration(FRAME_DURATION_BATTLE);
        if (!hasState) { // frame-only turn for showing the initial state
            draft.ShuffleDecks();
            state.init(draft.decks);
            hasState = true;
            sdkplayer.expectedOutputLines = 0;
            sdkplayer.execute();
            sdkplayer.expectedOutputLines = 1;
            return; // after render
        }
        if (!actionsToHandle.empty()) { // there is a legal action on top of the list
            sdkplayer.expectedOutputLines = 0;
            sdkplayer.execute();
            sdkplayer.expectedOutputLines = 1;
            Action a = actionsToHandle.front();
            actionsToHandle.erase(actionsToHandle.begin());
            gm.addToGameSummary(A("Player ") + sdkplayer.getNicknameToken() + A(" performed action: " + a.toStringNoText()));
            state.AdvanceState(a);
            if (a.type == SUMMON) gm.setFrameDuration(FRAME_DURATION_SUMMON);
        } else { // it's time to actually call a player
            gm.setTurnMaxTime(gameTurn <= CARDS_IN_DECK + 1 ? TIMELIMIT_FIRSTGAMETURN : TIMELIMIT_GAMETURN);
            state.AdvanceState();
            for (const std::string& line : state.getPlayersInput()) sdkplayer.sendInputLine(line);
            for (const std::string& line : state.getCardsInput()) sdkplayer.sendInputLine(line);
            sdkplayer.execute();
            try {
                String output = sdkplayer.output0();
                actionsToHandle = Action::parseSequence(output);
            } catch (const InvalidActionHard& e) {
                HandleError(sdkplayer, sdkplayer.getNicknameToken() + u": " + e.message);
            } catch (const TimeoutException&) {
                HandleError(sdkplayer, sdkplayer.getNicknameToken() + u" timeout!");
            }
        }

        // now we roll-out actions until next legal is found
        std::vector<Action> legals = state.computeLegalActions();
        int illegalActions = 0;
        while (!actionsToHandle.empty()) {
            const Action& a = actionsToHandle.front();
            if (a.type == PASS) {
                actionsToHandle.erase(actionsToHandle.begin());
                continue;
            }
            bool legal = false;
            for (const Action& l : legals) legal = legal || a.equals(l);
            if (legal) break;
            String text = a.toString();
            actionsToHandle.erase(actionsToHandle.begin());
            illegalActions++;
            if (illegalActions <= ILLEGAL_ACTION_SUMMARY_LIMIT)
                gm.addToGameSummary(A("[Warning] ") + sdkplayer.getNicknameToken() + A(" Action is not legal: ") + text);
        }
        if (illegalActions > ILLEGAL_ACTION_SUMMARY_LIMIT)
            gm.addToGameSummary(A("[Warning] ") + sdkplayer.getNicknameToken() + A(" Performed another " + S(illegalActions - ILLEGAL_ACTION_SUMMARY_LIMIT) + " illegalActions"));

        // render: ui.battle(gameTurn)
        if (CheckAndHandleEndgame()) return;
        if (actionsToHandle.empty()) { // player change
            gameTurn++;
            gamePlayer = (gamePlayer + 1) % 2;
        }
    }

    void HandleError(Player& sdkplayer, const String& errmsg) {
        gm.addToGameSummary(cg::GameManager::formatErrorMessage(errmsg));
        sdkplayer.deactivate(errmsg);
        sdkplayer.setScore(-1);
        gm.endGame();
    }

    // returns true if the game ends
    bool CheckAndHandleEndgame() {
        if (state.winner == -1) return false;
        gm.addToGameSummary(cg::GameManager::formatSuccessMessage(player(state.winner).getNicknameToken() + u" won!"));
        player(state.winner).setScore(1);
        gm.endGame();
        return true;
    }
};

// Referee: the engine, then the view (RefereeUI, not built: see the header)
struct LocamReferee : cg::Referee {
    EngineReferee engine;

    void init() override {
        engine.refereeInit();
        // ui.init(): its static PlayerUI[2] takes two players at most
        cg::gm.setFrameDuration(FRAME_DURATION_SHOWDRAFT);
        for (cg::Player* p : cg::gm.getPlayers())
            if (p->getIndex() >= 2) throw java::Exception{java::arrayIndexOutOfBoundsException(p->getIndex(), 2)};
    }
    void gameTurn(int) override { engine.refereeGameTurn(); }
};

static LocamReferee referee;

int main(int argc, char** argv) {
    cg::Options options;
    options.engine25 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
