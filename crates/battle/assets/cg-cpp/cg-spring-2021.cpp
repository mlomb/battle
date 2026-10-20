// Spring Challenge 2021 (Photosynthesis) - headless C++ referee, port of
// https://github.com/CodinGame/SpringChallenge2021 (commit 91bcff0), CodinGame game engine 4.0.2.
// 2 players, league 19 as set by the CLI, i.e. the default case of Game.init (Bronze and above:
// seeds, growth, shadows and holes enabled, 24 days).
// Deterministic; reproduces the same game as the reference jar (cg-spring-2021-ref) for a -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints "<p0score>\n<p1score>\nseed=<N>\n" to stdout, like the jar's CLI, and exits 0. When the
// Java referee or the runner fails during the game (see below) the jar prints "null" for each
// score and still exits 0, so we do the same (and print the Java exception to stderr). Like the
// jar's CLI: a missing -p1/-p2 prints the usage (exit 0); an invalid -seed, a missing option value
// or an unknown option exits 1 without scores. -p3/-p4 are refused (exit 1).
// -l writes a trace with one "T<turn> P<player> OUT <JSON array of the lines>" or
// "T<turn> P<player> TIMEOUT" line per player execution, the turn summaries and tooltips as the
// runner records them ("SUM <json>", "TIP <player> <json>"), and some informative lines.
//
// The Java referee is followed closely (same data structures and iteration orders). Notable Java
// behaviours reproduced here:
//   - java.util.Random(seed) and its consumers in order: BoardGenerator (holes are drawn from the
//     java.util.HashMap keySet order of the CubeCoord cells), the starting trees, and
//     Collections.shuffle of the COMPLETE, GROW and SEED possible moves of each input
//   - TreeMap order of the trees; a seed conflict makes every action of that turn free (the
//     available sun is not written back)
//   - engine 4.0.2 GameManager:
//       * a frame in which no player is executed still executes player 0 for 0 lines, so player 0
//         receives its pending input then (the global info at turn 1)
//       * every real execution adds turnMaxTime (100 ms) to a total: the 301st execution throws
//         "Total game duration too long (>30000ms)" and the game fails (scores "null")
//   - engine 4.0.2 runner (GameRunner, Agent):
//       * Agent#getOutput: '\r' and '\n' terminate lines ("\r\n" counts once, even across turns),
//         10000 bytes max, 1000 ms for the first execution of a player and 100 ms afterwards; the
//         bytes are decoded as UTF-8 exactly like java.lang.String does (malformed -> U+FFFD)
//       * the referee reads its stdin with a java.util.Scanner, which also ends lines at U+0085,
//         U+2028 and U+2029: the rest of such a line is read as the next engine command, so the
//         referee crashes at its next player execution (not reproduced: a rest that is itself a
//         valid engine command header, see execute())
//       * the runner reads each referee command into a 30000-byte buffer: a turn summary or a
//         tooltip repeating a long invalid command (e.g. ~10000 invalid UTF-8 bytes, each one
//         becomes U+FFFD, 3 bytes) does not fit and the game fails (scores "null")
//       * the inputs are written by a thread per player (AsynchronousWriter): a bot that does not
//         read its stdin does not block the game, and a failed write (the bot exited) does not
//         prevent reading what the bot wrote before
//       * Agent#readError: a bot's stderr goes to a 4120-byte pipe (java.lang.ProcessImpl on
//         Windows) of which the runner reads at most 4096 bytes after each execution of that
//         player: a bot that writes more to stderr blocks and times out. Reproduced on Windows
//         (the content is discarded); on POSIX agents' stderr goes to /dev/null instead.
//   - wall clock: the jar starts the bots and then spends 0.3-1.1 s (measured) initialising its
//     referee before the first input, so a slow bot startup rarely counts in its first turn. The
//     first turn of each player is timed here as if its input had been sent 500 ms after the bot
//     was started (BOT_STARTUP_GRACE_MS); later turns are timed exactly like the engine does.
//   - the view data quota (1 MB) cannot be reached: at most ~350 frames (the 30 s total turn time
//     above) of at most ~2 KB each; player messages only go to the view, so they are not kept.

// Java never fuses a*b+c, so we must not either (zig c++ -march=native would emit FMAs)
#pragma STDC FP_CONTRACT OFF
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;

// ----------------------------------------------------------------------------
// Java semantics
// ----------------------------------------------------------------------------
namespace java {

// java.util.Random
struct Random {
    u64 seed = 0;
    void setSeed(i64 s) { seed = ((u64)s ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1); }
    i32 next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return (i32)(u32)(seed >> (48 - bits));
    }
    i32 nextInt(i32 bound) {
        i32 r = next(31);
        i32 m = bound - 1;
        if ((bound & m) == 0) return (i32)(((i64)bound * (i64)r) >> 31);
        for (i32 u = r; (i32)((u32)u - (u32)(r = u % bound) + (u32)m) < 0; u = next(31)) {
        }
        return r;
    }
};

// Collections.shuffle(list, rnd) on an ArrayList (RandomAccess)
template <class T>
static void shuffle(std::vector<T>& list, Random& rnd) {
    for (int i = (int)list.size(); i > 1; i--) std::swap(list[i - 1], list[rnd.nextInt(i)]);
}

// Iteration order of a java.util.HashMap (default capacity) after putting distinct keys with the
// given hashCode()s in order. Returns the indices of the keys in iteration order.
static std::vector<int> hashMapOrder(const std::vector<i32>& hashCodes) {
    std::vector<u32> hashes;
    for (i32 h : hashCodes) hashes.push_back((u32)h ^ ((u32)h >> 16));
    size_t cap = 16;
    std::vector<std::vector<int>> tab(cap);
    auto resize = [&]() {
        // splitting a bin preserves the relative order of its nodes
        std::vector<std::vector<int>> nt(cap * 2);
        for (auto& bin : tab)
            for (int k : bin) nt[hashes[k] & (cap * 2 - 1)].push_back(k);
        tab.swap(nt);
        cap *= 2;
    };
    size_t size = 0;
    for (int k = 0; k < (int)hashCodes.size(); k++) {
        auto& bin = tab[hashes[k] & (cap - 1)];
        bin.push_back(k);
        // treeifyBin resizes small tables instead (bins are never treeified at these sizes)
        if (bin.size() >= 9 && cap < 64) resize();
        if (++size > cap * 3 / 4) resize();
    }
    std::vector<int> order;
    for (auto& bin : tab)
        for (int k : bin) order.push_back(k);
    return order;
}

// new String(bytes, UTF_8): port of java.lang.String.decodeUTF8_UTF16 (JDK 17+), malformed input
// is replaced by U+FFFD
static const char16_t REPL = 0xFFFD;
static inline bool isNotContinuation(int b) { return (b & 0xc0) != 0x80; }
static inline bool isMalformed3(int b1, int b2, int b3) {
    return (b1 == (int8_t)0xe0 && (b2 & 0xe0) == 0x80) || (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80;
}
static inline bool isMalformed3_2(int b1, int b2) {
    return (b1 == (int8_t)0xe0 && (b2 & 0xe0) == 0x80) || (b2 & 0xc0) != 0x80;
}
static inline bool isMalformed4(int b2, int b3, int b4) {
    return (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80 || (b4 & 0xc0) != 0x80;
}
static inline bool isMalformed4_2(int b1, int b2) {
    return (b1 == 0xf0 && (b2 < 0x90 || b2 > 0xbf)) || (b1 == 0xf4 && (b2 & 0xf0) != 0x80) || (b2 & 0xc0) != 0x80;
}
static inline bool isMalformed4_3(int b3) { return (b3 & 0xc0) != 0x80; }
static inline char16_t decode2(int b1, int b2) {
    return (char16_t)((i32)(((u32)b1 << 6) ^ (u32)b2) ^ (i32)(((u32)(i32)(int8_t)0xC0 << 6) ^ (u32)(i32)(int8_t)0x80));
}
static inline char16_t decode3(int b1, int b2, int b3) {
    u32 k = ((u32)(i32)(int8_t)0xE0 << 12) ^ ((u32)(i32)(int8_t)0x80 << 6) ^ (u32)(i32)(int8_t)0x80;
    return (char16_t)(((u32)b1 << 12) ^ ((u32)b2 << 6) ^ ((u32)b3 ^ k));
}
static inline i32 decode4(int b1, int b2, int b3, int b4) {
    u32 k = ((u32)(i32)(int8_t)0xF0 << 18) ^ ((u32)(i32)(int8_t)0x80 << 12) ^ ((u32)(i32)(int8_t)0x80 << 6) ^
            (u32)(i32)(int8_t)0x80;
    return (i32)(((u32)b1 << 18) ^ ((u32)b2 << 12) ^ ((u32)b3 << 6) ^ ((u32)b4 ^ k));
}
static int malformed3(const int8_t* src, int sp) {
    int b1 = src[sp++];
    int b2 = src[sp];
    return ((b1 == (int8_t)0xe0 && (b2 & 0xe0) == 0x80) || isNotContinuation(b2)) ? 1 : 2;
}
static int malformed4(const int8_t* src, int sp) {
    int b1 = src[sp++] & 0xff;
    int b2 = src[sp++] & 0xff;
    if (b1 > 0xf4 || (b1 == 0xf0 && (b2 < 0x90 || b2 > 0xbf)) || (b1 == 0xf4 && (b2 & 0xf0) != 0x80) ||
        isNotContinuation(b2))
        return 1;
    if (isNotContinuation(src[sp])) return 2;
    return 3;
}
static std::u16string decodeUtf8(const std::string& bytes) {
    const int8_t* src = (const int8_t*)bytes.data();
    int sp = 0, sl = (int)bytes.size();
    std::u16string dst;
    dst.reserve(bytes.size());
    while (sp < sl) {
        int b1 = src[sp++];
        if (b1 >= 0) {
            dst += (char16_t)b1;
        } else if ((b1 >> 5) == -2 && (b1 & 0x1e) != 0) {
            if (sp < sl) {
                int b2 = src[sp++];
                if (isNotContinuation(b2)) {
                    dst += REPL;
                    sp--;
                } else {
                    dst += decode2(b1, b2);
                }
                continue;
            }
            dst += REPL;
            break;
        } else if ((b1 >> 4) == -2) {
            if (sp + 1 < sl) {
                int b2 = src[sp++];
                int b3 = src[sp++];
                if (isMalformed3(b1, b2, b3)) {
                    dst += REPL;
                    sp -= 3;
                    sp += malformed3(src, sp);
                } else {
                    char16_t c = decode3(b1, b2, b3);
                    if (c >= 0xD800 && c <= 0xDFFF) dst += REPL;
                    else dst += c;
                }
                continue;
            }
            if (sp < sl && isMalformed3_2(b1, src[sp])) {
                dst += REPL;
                continue;
            }
            dst += REPL;
            break;
        } else if ((b1 >> 3) == -2) {
            if (sp + 2 < sl) {
                int b2 = src[sp++];
                int b3 = src[sp++];
                int b4 = src[sp++];
                i32 uc = decode4(b1, b2, b3, b4);
                if (isMalformed4(b2, b3, b4) || !(uc >= 0x10000 && uc < 0x110000)) {
                    dst += REPL;
                    sp -= 4;
                    sp += malformed4(src, sp);
                } else {
                    dst += (char16_t)((uc >> 10) + (0xD800 - (0x10000 >> 10)));
                    dst += (char16_t)((uc & 0x3ff) + 0xDC00);
                }
                continue;
            }
            b1 &= 0xff;
            if (b1 > 0xf4 || (sp < sl && isMalformed4_2(b1, src[sp] & 0xff))) {
                dst += REPL;
                continue;
            }
            sp++;
            dst += REPL;
            if (sp < sl && isMalformed4_3(src[sp])) continue;
            break;
        } else {
            dst += REPL;
        }
    }
    return dst;
}

// String#getBytes(UTF_8) / PrintStream output of a string without lone surrogates
static std::string toUtf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = '?'; // unmappable for the encoder (never produced by decodeUtf8)
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// String.trim on UTF-8 bytes (chars <= ' ' are single bytes in UTF-8)
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

// String.replace(CharSequence, CharSequence)
static std::string replace(const std::string& s, const std::string& from, const std::string& to) {
    std::string out;
    size_t pos = 0;
    for (;;) {
        size_t f = s.find(from, pos);
        if (f == std::string::npos) break;
        out.append(s, pos, f - pos);
        out += to;
        pos = f + from.size();
    }
    out.append(s, pos, std::string::npos);
    return out;
}

// \s without UNICODE_CHARACTER_CLASS
static inline bool isRegexSpace(char16_t c) { return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r'; }
// '.' does not match line terminators (no DOTALL, no UNIX_LINES)
static inline bool isLineTerminator(char16_t c) { return c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029; }
// line separators of java.util.Scanner#nextLine ("\r\n|[\n\r  \u0085]")
static inline bool isScannerLineSeparator(char16_t c) { return isLineTerminator(c); }

} // namespace java

// ----------------------------------------------------------------------------
// Constants.java / Config.java
// ----------------------------------------------------------------------------
namespace C {
const int RICHNESS_NULL = 0;
const int RICHNESS_POOR = 1;
const int RICHNESS_OK = 2;
const int RICHNESS_LUSH = 3;
const int TREE_SEED = 0;
const int TREE_SMALL = 1;
const int TREE_TALL = 3;
const int TREE_BASE_COST[4] = {0, 1, 3, 7};
const int LIFECYCLE_END_COST = 4;
const int STARTING_TREE_COUNT = 2;
const int RICHNESS_BONUS_OK = 2;
const int RICHNESS_BONUS_LUSH = 4;
// Config defaults (the CLI passes no game parameter but the seed)
const int STARTING_SUN = 0;
const int MAP_RING_COUNT = 3;
const int STARTING_NUTRIENTS = 20;
const int MAX_ROUNDS = 24;
const int MAX_EMPTY_CELLS = 10;
// Game.init, default case (league 19)
const bool ENABLE_SEED = true;
const bool ENABLE_GROW = true;
const bool ENABLE_SHADOW = true;
const bool ENABLE_HOLES = true;
const int STARTING_TREE_SIZE = TREE_SMALL;
const int STARTING_TREE_DISTANCE = 2;
// Referee.init
const int FIRST_TURN_MAX_TIME = 1000;
const int TURN_MAX_TIME = 100;
// see execute(): grace for the bots' startup in their first turn (the jar's referee initialisation)
const int BOT_STARTUP_GRACE_MS = 500;
// engine
const int GAME_DURATION_HARD_QUOTA = 30000;
const size_t AGENT_MAX_BUFFER_SIZE = 10000;
const size_t REFEREE_MAX_BUFFER_SIZE = 30000;
const int NCELLS = 1 + 3 * MAP_RING_COUNT * (MAP_RING_COUNT + 1);
const char* const EXPECTED = "SEED <from> <to> | GROW <idx> | COMPLETE <idx> | WAIT"; // Game.getExpected()
} // namespace C

// ----------------------------------------------------------------------------
// CubeCoord.java, Cell.java, Board.java
// ----------------------------------------------------------------------------
static const int DIRECTIONS[6][3] = {{1, -1, 0}, {+1, 0, -1}, {0, +1, -1}, {-1, +1, 0}, {-1, 0, +1}, {0, -1, +1}};

struct CubeCoord {
    int x, y, z;
    i32 hashCode() const {
        i32 result = 1;
        result = 31 * result + x;
        result = 31 * result + y;
        result = 31 * result + z;
        return result;
    }
    bool operator==(const CubeCoord& o) const { return x == o.x && y == o.y && z == o.z; }
    CubeCoord neighbor(int orientation, int distance = 1) const {
        return {x + DIRECTIONS[orientation][0] * distance, y + DIRECTIONS[orientation][1] * distance,
                z + DIRECTIONS[orientation][2] * distance};
    }
    int distanceTo(const CubeCoord& d) const { return (std::abs(x - d.x) + std::abs(y - d.y) + std::abs(z - d.z)) / 2; }
    CubeCoord getOpposite() const { return {-x, -y, -z}; }
};

// Board.map (HashMap<CubeCoord, Cell>) lookups: every coord has x + y + z == 0, and the referee
// never looks further than 6 cells away from the centre (a tall tree's shadow from the edge)
struct Board {
    static const int R = 8;
    int cellAt[2 * R + 1][2 * R + 1]; // cell index, -1 when absent
    CubeCoord coords[C::NCELLS];      // Board.coords: sorted by cell index
    int richness[C::NCELLS];
    int cellIndex(const CubeCoord& c) const { // board.map.getOrDefault(c, Cell.NO_CELL).getIndex()
        if (c.x < -R || c.x > R || c.y < -R || c.y > R) return -1;
        return cellAt[c.x + R][c.y + R];
    }
    bool contains(const CubeCoord& c) const { return cellIndex(c) >= 0; }
};

// ----------------------------------------------------------------------------
// Player.java, Tree.java, Seed.java, actions
// ----------------------------------------------------------------------------
enum ActionType { NO_ACTION, WAIT, GROW, SEED, COMPLETE };
struct Action {
    ActionType type = NO_ACTION;
    int sourceId = 0, targetId = 0;
};

struct Tooltip {
    int player;
    std::string message; // UTF-8
};

struct Player {
    int index = 0;
    // AbstractPlayer / AbstractMultiplayerPlayer
    int score = 0;
    bool active = true;
    bool hasBeenExecuted = false;
    bool hasNeverBeenExecuted = true;
    bool timeout = false;
    std::string inputs; // pending input lines
    std::u16string output; // the line received by the referee
    // Player
    Action action;
    int sun = C::STARTING_SUN;
    bool waiting = false;
    int bonusScore = 0;
    std::string token() const { return "$" + std::to_string(index); } // getNicknameToken
    void addScore(int s) { score += s; }
};

struct Tree {
    int size = 0;
    Player* owner = nullptr;
    int fatherIndex = -1;
    bool isDormant = false;
};

struct Seed {
    int owner, sourceCell, targetCell;
};

struct GameException {
    std::string message;
};

struct RefereeCrash {
    std::string what; // Java exception, for stderr
};

// ----------------------------------------------------------------------------
// Engine state (GameManager, MultiplayerGameManager)
// ----------------------------------------------------------------------------
static Player players[2];
static bool gameEnd = false;
static int totalTurnTime = 0;
static bool newTurn = false;
static std::vector<std::string> currentGameSummary, prevGameSummary;
static std::vector<Tooltip> currentTooltips, prevTooltips;
static int scannerPendingLines = 0; // lines left in the referee's Scanner by U+0085/U+2028/U+2029
static FILE* trace = nullptr;
static int engineTurn = 0;

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

static void addTooltip(int player, const std::string& message) { currentTooltips.push_back({player, message}); }

// GameManager.formatErrorMessage: "¤RED¤%s§RED§"
static std::string formatErrorMessage(const std::string& m) { return "\xC2\xA4RED\xC2\xA4" + m + "\xC2\xA7RED\xC2\xA7"; }

// ----------------------------------------------------------------------------
// GameSummaryManager.java (lines are UTF-8; only their size matters, see checkRunnerBuffer)
// ----------------------------------------------------------------------------
struct GameSummaryManager {
    std::vector<std::string> lines;
    void clear() { lines.clear(); }
    void addPlayerBadCommand(const Player& player, const std::string& expected, const std::string& got) {
        lines.push_back(formatErrorMessage(player.token() + " provided invalid input. Expected '" + expected + "'\nGot '" + got + "'"));
    }
    void addPlayerTimeout(const Player& player) {
        lines.push_back(formatErrorMessage(player.token() + " has not provided an action in time."));
    }
    void addPlayerDisqualified(const Player& player) { lines.push_back(player.token() + " was disqualified."); }
    void addCutTree(const Player& player, int cell, int score) {
        lines.push_back(player.token() + " is ending their tree life on cell " + std::to_string(cell) + ", scoring " +
                        std::to_string(score) + " points");
    }
    void addGrowTree(const Player& player, int cell) {
        lines.push_back(player.token() + " is growing a tree on cell " + std::to_string(cell));
    }
    void addPlantSeed(const Player& player, int targetCell, int sourceCell) {
        lines.push_back(player.token() + " is planting a seed on cell " + std::to_string(targetCell) + " from cell " +
                        std::to_string(sourceCell));
    }
    void addWait(const Player& player) { lines.push_back(player.token() + " is waiting"); }
    void addRound(int round) {
        lines.push_back("Round " + std::to_string(round) + "/" + std::to_string(C::MAX_ROUNDS - 1));
    }
    void addError(const std::string& error) { lines.push_back(error); }
    void addSeedConflict(const Seed& seed) { lines.push_back("Seed conflict on cell " + std::to_string(seed.targetCell)); }
    void addRoundTransition(int round) {
        lines.push_back("Round " + std::to_string(round) + " ends");
        if (round + 1 < C::MAX_ROUNDS) {
            lines.push_back("The sun is now pointing towards direction " + std::to_string((round + 1) % 6));
            lines.push_back("Round " + std::to_string(round + 1) + " starts");
        }
    }
    void addGather(const Player& player, int given) {
        lines.push_back(player.token() + " has collected " + std::to_string(given) + " sun points");
    }
    std::string toString() const {
        std::string s;
        for (size_t i = 0; i < lines.size(); i++) {
            if (i) s += '\n';
            s += lines[i];
        }
        return s;
    }
};
static GameSummaryManager gameSummaryManager;

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
enum FrameType { GATHERING, ACTIONS, SUN_MOVE, INIT };

struct Game {
    int nutrients = C::STARTING_NUTRIENTS;
    Board board;
    Tree* trees[C::NCELLS] = {}; // TreeMap<Integer, Tree>: iterated by increasing cell index
    Tree treeStore[C::NCELLS];   // the Tree object on each cell (a cell holds at most one tree)
    std::vector<CubeCoord> dyingTrees;
    std::vector<int> availableSun;
    std::vector<Seed> sentSeeds;
    int sunOrientation = 0;
    int shadows[C::NCELLS]; // HashMap<Integer, Integer>, -1 = absent
    java::Random random;
    int round = 0;
    int turn = 0;
    FrameType currentFrameType = INIT;
    FrameType nextFrameType = GATHERING;

    int treeCount() const {
        int n = 0;
        for (Tree* t : trees)
            if (t) n++;
        return n;
    }

    // BoardGenerator.java
    void generateBoard() {
        std::vector<CubeCoord> keys; // insertion order of board.put
        std::vector<int> rich;
        for (auto& row : board.cellAt)
            for (int& c : row) c = -1;
        auto generateCell = [&](const CubeCoord& coord, int richness) {
            int index = (int)keys.size();
            keys.push_back(coord);
            rich.push_back(richness);
            board.cellAt[coord.x + Board::R][coord.y + Board::R] = index;
        };
        CubeCoord centre{0, 0, 0};
        generateCell(centre, C::RICHNESS_LUSH);
        CubeCoord coord = centre.neighbor(0);
        for (int distance = 1; distance <= C::MAP_RING_COUNT; distance++) {
            for (int orientation = 0; orientation < 6; orientation++) {
                for (int count = 0; count < distance; count++) {
                    if (distance == C::MAP_RING_COUNT) generateCell(coord, C::RICHNESS_POOR);
                    else if (distance == C::MAP_RING_COUNT - 1) generateCell(coord, C::RICHNESS_OK);
                    else generateCell(coord, C::RICHNESS_LUSH);
                    coord = coord.neighbor((orientation + 2) % 6);
                }
            }
            coord = coord.neighbor(0);
        }
        for (int i = 0; i < C::NCELLS; i++) {
            board.coords[i] = keys[i];
            board.richness[i] = rich[i];
        }

        // new ArrayList<>(board.keySet())
        std::vector<i32> hashes;
        for (auto& k : keys) hashes.push_back(k.hashCode());
        std::vector<CubeCoord> coordList;
        for (int i : java::hashMapOrder(hashes)) coordList.push_back(keys[i]);
        int coordListSize = (int)coordList.size();
        int wantedEmptyCells = C::ENABLE_HOLES ? random.nextInt(C::MAX_EMPTY_CELLS + 1) : 0;
        int actualEmptyCells = 0;
        while (actualEmptyCells < wantedEmptyCells - 1) {
            int randIndex = random.nextInt(coordListSize);
            CubeCoord randCoord = coordList[randIndex];
            int ci = board.cellIndex(randCoord);
            if (board.richness[ci] != C::RICHNESS_NULL) {
                board.richness[ci] = C::RICHNESS_NULL;
                actualEmptyCells++;
                if (!(randCoord == randCoord.getOpposite())) {
                    board.richness[board.cellIndex(randCoord.getOpposite())] = C::RICHNESS_NULL;
                    actualEmptyCells++;
                }
            }
        }
    }

    void init(i64 seed) {
        random.setSeed(seed);
        generateBoard();
        for (Tree*& t : trees) t = nullptr;
        dyingTrees.clear();
        availableSun.clear();
        sentSeeds.clear();
        initStartingTrees();
        for (int& s : shadows) s = -1;
        sunOrientation = 0;
        round = 0;
        if (C::ENABLE_SHADOW) calculateShadows();
    }

    void initStartingTrees() {
        // STARTING_TREES_ON_EDGES: getBoardEdges()
        std::vector<CubeCoord> startingCoords;
        CubeCoord centre{0, 0, 0};
        for (int i = 0; i < C::NCELLS; i++)
            if (board.coords[i].distanceTo(centre) == C::MAP_RING_COUNT) startingCoords.push_back(board.coords[i]);
        std::vector<CubeCoord> kept;
        for (auto& c : startingCoords)
            if (board.richness[board.cellIndex(c)] != C::RICHNESS_NULL) kept.push_back(c);
        startingCoords = kept;
        std::vector<CubeCoord> validCoords;
        while ((int)validCoords.size() < C::STARTING_TREE_COUNT * 2) validCoords = tryInitStartingTrees(startingCoords);
        for (int i = 0; i < C::STARTING_TREE_COUNT; i++) {
            placeTree(&players[0], board.cellIndex(validCoords[2 * i]), C::STARTING_TREE_SIZE);
            placeTree(&players[1], board.cellIndex(validCoords[2 * i + 1]), C::STARTING_TREE_SIZE);
        }
    }

    std::vector<CubeCoord> tryInitStartingTrees(const std::vector<CubeCoord>& startingCoords) {
        std::vector<CubeCoord> coordinates;
        std::vector<CubeCoord> availableCoords = startingCoords;
        for (int i = 0; i < C::STARTING_TREE_COUNT; i++) {
            if (availableCoords.empty()) return coordinates;
            int r = random.nextInt((i32)availableCoords.size());
            CubeCoord normalCoord = availableCoords[r];
            CubeCoord oppositeCoord = normalCoord.getOpposite();
            std::vector<CubeCoord> rest;
            for (auto& c : availableCoords)
                if (!(c.distanceTo(normalCoord) <= C::STARTING_TREE_DISTANCE ||
                      c.distanceTo(oppositeCoord) <= C::STARTING_TREE_DISTANCE))
                    rest.push_back(c);
            availableCoords = rest;
            coordinates.push_back(normalCoord);
            coordinates.push_back(oppositeCoord);
        }
        return coordinates;
    }

    void calculateShadows() {
        for (int& s : shadows) s = -1;
        for (int index = 0; index < C::NCELLS; index++) {
            Tree* tree = trees[index];
            if (!tree) continue;
            CubeCoord coord = board.coords[index];
            for (int i = 1; i <= tree->size; i++) {
                int ci = board.cellIndex(coord.neighbor(sunOrientation, i));
                if (ci >= 0) shadows[ci] = shadows[ci] < 0 ? tree->size : std::max(shadows[ci], tree->size);
            }
        }
    }

    // getGlobalInfoFor
    std::string getGlobalInfoFor(const Player&) const {
        std::string s = std::to_string(C::NCELLS) + "\n";
        for (int i = 0; i < C::NCELLS; i++) {
            const CubeCoord& coord = board.coords[i];
            s += std::to_string(i) + " " + std::to_string(board.richness[i]);
            for (int d = 0; d < 6; d++) s += " " + std::to_string(board.cellIndex(coord.neighbor(d)));
            s += "\n";
        }
        return s;
    }

    std::string getCurrentFrameInfoFor(Player& player) {
        std::string s;
        s += std::to_string(round) + "\n";
        s += std::to_string(nutrients) + "\n";
        Player& other = players[1 - player.index];
        s += std::to_string(player.sun) + " " + std::to_string(player.score) + "\n";
        s += std::to_string(other.sun) + " " + std::to_string(other.score) + " " + (other.waiting ? "1" : "0") + "\n";
        s += std::to_string(treeCount()) + "\n";
        for (int index = 0; index < C::NCELLS; index++) {
            Tree* tree = trees[index];
            if (!tree) continue;
            s += std::to_string(index) + " " + std::to_string(tree->size) + " " + (tree->owner == &player ? "1" : "0") + " " +
                 (tree->isDormant ? "1" : "0") + "\n";
        }
        std::vector<std::string> possibleMoves = getPossibleMoves(player);
        s += std::to_string(possibleMoves.size()) + "\n";
        for (auto& m : possibleMoves) s += m + "\n";
        return s;
    }

    std::vector<std::string> getPossibleMoves(Player& player) {
        std::vector<std::string> lines;
        lines.push_back("WAIT");
        std::vector<std::string> possibleSeeds, possibleGrows, possibleCompletes;
        if (player.waiting) return lines;
        int seedCost = getSeedCost(player);
        for (int index = 0; index < C::NCELLS; index++) {
            Tree* tree = trees[index];
            if (!tree || tree->owner != &player) continue;
            CubeCoord coord = board.coords[index];
            if (playerCanSeedFrom(player, *tree, seedCost)) {
                int N = tree->size;
                for (int x = -N; x <= +N; x++) {
                    for (int y = std::max(-N, -x - N); y <= std::min(+N, -x + N); y++) {
                        int z = -x - y;
                        CubeCoord targetCoord{coord.x + x, coord.y + y, coord.z + z};
                        int targetCell = board.cellIndex(targetCoord);
                        if (playerCanSeedTo(targetCell))
                            possibleSeeds.push_back("SEED " + std::to_string(index) + " " + std::to_string(targetCell));
                    }
                }
            }
            int growCost = getGrowthCost(*tree);
            if (growCost <= player.sun && !tree->isDormant) {
                if (tree->size == C::TREE_TALL) possibleCompletes.push_back("COMPLETE " + std::to_string(index));
                else if (C::ENABLE_GROW) possibleGrows.push_back("GROW " + std::to_string(index));
            }
        }
        for (auto* possibleList : {&possibleCompletes, &possibleGrows, &possibleSeeds}) {
            java::shuffle(*possibleList, random);
            lines.insert(lines.end(), possibleList->begin(), possibleList->end());
        }
        return lines;
    }

    bool playerCanSeedFrom(const Player& player, const Tree& tree, int seedCost) const {
        return C::ENABLE_SEED && seedCost <= player.sun && tree.size > C::TREE_SEED && !tree.isDormant;
    }

    bool playerCanSeedTo(int targetCell) const { // Cell.NO_CELL: index -1, not valid
        return targetCell >= 0 && board.richness[targetCell] != C::RICHNESS_NULL && !trees[targetCell];
    }

    void resetGameTurnData() {
        dyingTrees.clear();
        availableSun.clear();
        sentSeeds.clear();
        for (Player& p : players) {
            availableSun.push_back(p.sun);
            p.action = Action(); // Player.reset (message only goes to the view)
        }
        currentFrameType = nextFrameType;
    }

    int getGrowthCost(const Tree& targetTree) const {
        int targetSize = targetTree.size + 1;
        if (targetSize > C::TREE_TALL) return C::LIFECYCLE_END_COST;
        return getCostFor(targetSize, targetTree.owner);
    }

    int getSeedCost(const Player& player) const { return getCostFor(0, &player); }

    int getCostFor(int size, const Player* owner) const {
        int baseCost = C::TREE_BASE_COST[size];
        int sameTreeCount = 0;
        for (Tree* t : trees)
            if (t && t->size == size && t->owner == owner) sameTreeCount++;
        return baseCost + sameTreeCount;
    }

    // getCoordByIndex
    CubeCoord getCoordByIndex(int index) const {
        if (index < 0 || index >= C::NCELLS) throw GameException{"Cell " + std::to_string(index) + " not found"};
        return board.coords[index];
    }

    static std::string notEnoughSun(int cost, int sun) {
        return "Not enough sun. You need " + std::to_string(cost) + " but have " + std::to_string(sun);
    }

    void doGrow(Player& player, const Action& action) {
        CubeCoord coord = getCoordByIndex(action.targetId);
        int cell = board.cellIndex(coord);
        Tree* targetTree = trees[cell];
        if (!targetTree) throw GameException{"There is no tree on cell " + std::to_string(cell)};
        if (targetTree->owner != &player) throw GameException{"The tree on cell " + std::to_string(cell) + " is owned by opponent"};
        if (targetTree->isDormant)
            throw GameException{"Tree on cell " + std::to_string(cell) + " is dormant (has already been used this round)"};
        if (targetTree->size >= C::TREE_TALL)
            throw GameException{"Tree on cell " + std::to_string(cell) + " cannot grow more (max size is 3)."};
        int costOfGrowth = getGrowthCost(*targetTree);
        int currentSun = availableSun[player.index];
        if (currentSun < costOfGrowth) throw GameException{notEnoughSun(costOfGrowth, player.sun)};
        availableSun[player.index] = currentSun - costOfGrowth;
        targetTree->size++;
        gameSummaryManager.addGrowTree(player, cell);
        targetTree->isDormant = true;
    }

    void doComplete(Player& player, const Action& action) {
        CubeCoord coord = getCoordByIndex(action.targetId);
        int cell = board.cellIndex(coord);
        Tree* targetTree = trees[cell];
        if (!targetTree) throw GameException{"There is no tree on cell " + std::to_string(cell)};
        if (targetTree->owner != &player) throw GameException{"The tree on cell " + std::to_string(cell) + " is owned by opponent"};
        if (targetTree->size < C::TREE_TALL) throw GameException{"The tree on cell " + std::to_string(cell) + " is not large enough"};
        if (targetTree->isDormant)
            throw GameException{"Tree on cell " + std::to_string(cell) + " is dormant (has already been used this round)"};
        int costOfGrowth = getGrowthCost(*targetTree);
        int currentSun = availableSun[player.index];
        if (currentSun < costOfGrowth) throw GameException{notEnoughSun(costOfGrowth, player.sun)};
        availableSun[player.index] = currentSun - costOfGrowth;
        dyingTrees.push_back(coord);
        targetTree->isDormant = true;
    }

    void doSeed(Player& player, const Action& action) {
        CubeCoord targetCoord = getCoordByIndex(action.targetId);
        CubeCoord sourceCoord = getCoordByIndex(action.sourceId);
        int targetCell = board.cellIndex(targetCoord);
        int sourceCell = board.cellIndex(sourceCoord);
        if (trees[targetCell]) throw GameException{"There is already a tree on cell " + std::to_string(targetCell)};
        Tree* sourceTree = trees[sourceCell];
        if (!sourceTree) throw GameException{"There is no tree on cell " + std::to_string(sourceCell)};
        if (sourceTree->size == C::TREE_SEED) throw GameException{"The seed on " + std::to_string(sourceCell) + " cannot produce seeds"};
        if (sourceTree->owner != &player)
            throw GameException{"The tree on cell " + std::to_string(sourceCell) + " is owned by opponent"};
        if (sourceTree->isDormant)
            throw GameException{"Tree on cell " + std::to_string(sourceCell) + " is dormant (has already been used this round)"};
        int distance = sourceCoord.distanceTo(targetCoord);
        if (distance > sourceTree->size)
            throw GameException{"The tree on cell " + std::to_string(sourceCell) + " is too far from cell " +
                                std::to_string(targetCell) + " to plant a seed there"};
        if (board.richness[targetCell] == C::RICHNESS_NULL)
            throw GameException{"You can't plant a seed on cell " + std::to_string(targetCell)};
        int costOfSeed = getSeedCost(player);
        int currentSun = availableSun[player.index];
        if (currentSun < costOfSeed) throw GameException{notEnoughSun(costOfSeed, player.sun)};
        availableSun[player.index] = currentSun - costOfSeed;
        sourceTree->isDormant = true;
        sentSeeds.push_back({player.index, sourceCell, targetCell});
        gameSummaryManager.addPlantSeed(player, targetCell, sourceCell);
    }

    void giveSun() {
        int givenToPlayer[2] = {0, 0};
        for (int index = 0; index < C::NCELLS; index++) {
            Tree* tree = trees[index];
            if (!tree) continue;
            if (shadows[index] < 0 || shadows[index] < tree->size) {
                Player* owner = tree->owner;
                owner->sun += tree->size;
                givenToPlayer[owner->index] += tree->size;
            }
        }
        for (Player& player : players) {
            int given = givenToPlayer[player.index];
            if (given > 0) gameSummaryManager.addGather(player, given);
        }
    }

    void removeDyingTrees() {
        for (const CubeCoord& coord : dyingTrees) {
            int cell = board.cellIndex(coord);
            int points = nutrients;
            if (board.richness[cell] == C::RICHNESS_OK) points += C::RICHNESS_BONUS_OK;
            else if (board.richness[cell] == C::RICHNESS_LUSH) points += C::RICHNESS_BONUS_LUSH;
            Player* player = trees[cell]->owner;
            player->addScore(points);
            addTooltip(player->index, player->token() + " scores " + std::to_string(points) + " points");
            trees[cell] = nullptr;
            gameSummaryManager.addCutTree(*player, cell, points);
        }
    }

    void updateNutrients() {
        for (size_t i = 0; i < dyingTrees.size(); i++) nutrients = std::max(0, nutrients - 1);
    }

    void performGameUpdate() {
        turn++;
        switch (currentFrameType) {
        case GATHERING:
            gameSummaryManager.addRound(round);
            performSunGatheringUpdate();
            nextFrameType = ACTIONS;
            break;
        case ACTIONS:
            gameSummaryManager.addRound(round);
            performActionUpdate();
            if (allPlayersAreWaiting()) nextFrameType = SUN_MOVE;
            break;
        case SUN_MOVE:
            gameSummaryManager.addRoundTransition(round);
            performSunMoveUpdate();
            nextFrameType = GATHERING;
            break;
        default:
            break; // unreachable: currentFrameType is never INIT here
        }
        currentGameSummary.push_back(gameSummaryManager.toString()); // addToGameSummary (never over quota)
        gameSummaryManager.clear();
        if (gameOver()) gameEnd = true; // else setMaxTurns(turn + 1): the engine loop never stops on it
    }

    void performSunMoveUpdate() {
        round++;
        if (round < C::MAX_ROUNDS) {
            sunOrientation = (sunOrientation + 1) % 6;
            if (C::ENABLE_SHADOW) calculateShadows();
        }
    }

    void performSunGatheringUpdate() {
        for (Player& p : players) p.waiting = false;
        for (Tree* t : trees)
            if (t) t->isDormant = false;
        giveSun();
    }

    void performActionUpdate() {
        for (Player& player : players) {
            if (player.waiting) continue;
            try {
                const Action action = player.action;
                if (action.type == GROW) doGrow(player, action);
                else if (action.type == SEED) doSeed(player, action);
                else if (action.type == COMPLETE) doComplete(player, action);
                else {
                    player.waiting = true;
                    gameSummaryManager.addWait(player);
                }
            } catch (const GameException& e) {
                gameSummaryManager.addError(player.token() + ": " + e.message);
                player.waiting = true;
            }
        }
        if (seedsAreConflicting()) {
            gameSummaryManager.addSeedConflict(sentSeeds[0]);
        } else {
            for (const Seed& seed : sentSeeds) plantSeed(&players[seed.owner], seed.targetCell, seed.sourceCell);
            for (Player& player : players) player.sun = availableSun[player.index];
        }
        removeDyingTrees();
        updateNutrients();
    }

    bool seedsAreConflicting() const {
        std::vector<int> targets;
        for (auto& s : sentSeeds)
            if (std::find(targets.begin(), targets.end(), s.targetCell) == targets.end()) targets.push_back(s.targetCell);
        return sentSeeds.size() != targets.size();
    }

    bool allPlayersAreWaiting() const {
        int n = 0;
        for (const Player& p : players)
            if (p.waiting) n++;
        return n == 2;
    }

    void plantSeed(Player* player, int index, int fatherIndex) {
        Tree* seed = placeTree(player, index, C::TREE_SEED);
        seed->isDormant = true;
        seed->fatherIndex = fatherIndex;
    }

    Tree* placeTree(Player* player, int index, int size) {
        Tree* tree = &treeStore[index];
        *tree = Tree();
        tree->size = size;
        tree->owner = player;
        trees[index] = tree;
        return tree;
    }

    void onEnd() {
        std::vector<Player*> active = getActivePlayers();
        for (Player* player : active) player->addScore(player->sun / 3); // (int) Math.floor(sun / 3)
        std::vector<int> distinct;
        for (Player* player : getActivePlayers())
            if (std::find(distinct.begin(), distinct.end(), player->score) == distinct.end()) distinct.push_back(player->score);
        if (distinct.size() == 1) {
            for (Tree* tree : trees) {
                if (tree && tree->owner->active) {
                    tree->owner->bonusScore += 1;
                    tree->owner->addScore(1);
                }
            }
        }
    }

    bool gameOver() const { return getActivePlayers().size() <= 1 || round >= C::MAX_ROUNDS; }
};
static Game game;

// ----------------------------------------------------------------------------
// Child process input: GameRunner's AsynchronousWriter. The runner queues each player input and a
// thread writes it, so a bot that does not read its stdin never blocks the referee; a write error
// (e.g. the bot exited) only stops the writing, the output already sent can still be read.
// ----------------------------------------------------------------------------
template <class Impl, class H>
struct InputWriter {
    struct State {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::string> q;
        bool stop = false;
    };
    State* ws = nullptr; // never freed: a writer blocked on a full pipe may outlive kill()

    void startWriter(H h) {
        ws = new State();
        State* st = ws;
        std::thread([st, h]() {
            bool ok = true;
            std::unique_lock<std::mutex> lk(st->m);
            for (;;) {
                st->cv.wait(lk, [st] { return st->stop || !st->q.empty(); });
                if (st->stop) break;
                std::string s = std::move(st->q.front());
                st->q.pop_front();
                lk.unlock();
                if (ok) ok = Impl::writeAll(h, s);
                lk.lock();
            }
            lk.unlock();
            Impl::closeInput(h);
        }).detach();
    }
    // the runner offers the input to the writer's queue (capacity 1024, never reached: a player is
    // executed at most ~350 times)
    void send(const std::string& s) {
        if (!ws || s.empty()) return;
        std::lock_guard<std::mutex> g(ws->m);
        ws->q.push_back(s);
        ws->cv.notify_one();
    }
    void stopWriter() {
        if (!ws) return;
        std::lock_guard<std::mutex> g(ws->m);
        ws->stop = true;
        ws->cv.notify_one();
    }
};

// ----------------------------------------------------------------------------
// Child process I/O (Windows)
// ----------------------------------------------------------------------------
#ifdef _WIN32
struct ChildProc : InputWriter<ChildProc, HANDLE> {
    HANDLE hProcess = nullptr;
    HANDLE hStdoutR = nullptr;   // read child's stdout (overlapped)
    HANDLE hReadEvent = nullptr; // manual-reset event for overlapped reads on hStdoutR
    HANDLE hStderrR = nullptr;   // read child's stderr (Agent#readError)
    bool alive = false;
    std::string readBuf; // bytes read but not consumed yet
    size_t readPos = 0;

    bool start(const std::string& cmd) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;
        HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
        // stdin is plain synchronous, we only do blocking writes from the referee.
        if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
        // stderr: a pipe of the size java.lang.ProcessImpl uses on Windows (4096 + 24 bytes, measured
        // with the jar), drained like Agent#readError (see readError)
        if (!CreatePipe(&errR, &errW, &sa, 4096 + 24)) {
            CloseHandle(inR);
            CloseHandle(inW);
            return false;
        }
        SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
        // stdout must be overlapped on our (read) side so reads can wait with a precise
        // millisecond timeout. CreatePipe doesn't support FILE_FLAG_OVERLAPPED, so build the pair
        // manually with a unique-named pipe.
        char pipeName[96];
        static volatile LONG s_pipeCounter = 0;
        LONG pipeId = InterlockedIncrement(&s_pipeCounter);
        std::snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\battle_referee_%lu_%ld",
                      (unsigned long)GetCurrentProcessId(), (long)pipeId);
        outR = CreateNamedPipeA(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT,
                                1, 65536, 65536, 0, &sa);
        if (outR == INVALID_HANDLE_VALUE) {
            CloseHandle(inR);
            CloseHandle(inW);
            CloseHandle(errR);
            CloseHandle(errW);
            return false;
        }
        outW = CreateFileA(pipeName, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outW == INVALID_HANDLE_VALUE) {
            CloseHandle(inR);
            CloseHandle(inW);
            CloseHandle(outR);
            CloseHandle(errR);
            CloseHandle(errW);
            return false;
        }
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inR;
        si.hStdOutput = outW;
        si.hStdError = errW;
        PROCESS_INFORMATION pi{};
        // If the command contains shell metacharacters, run it through cmd /c (with '/'->'\' so cmd
        // doesn't mistake path separators for option prefixes). Otherwise run it directly via CreateProcess.
        bool needShell = cmd.find_first_of("&|<>^()") != std::string::npos;
        std::string full;
        if (needShell) {
            std::string c2 = cmd;
            for (auto& ch : c2)
                if (ch == '/') ch = '\\';
            full = "cmd /c \"" + c2 + "\"";
        } else {
            full = cmd;
            for (auto& ch : full)
                if (ch == '/') ch = '\\';
        }
        std::vector<char> cmdline(full.begin(), full.end());
        cmdline.push_back(0);
        BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inR);
        CloseHandle(outW);
        CloseHandle(errW);
        if (!ok) {
            CloseHandle(inW);
            CloseHandle(outR);
            CloseHandle(errR);
            return false;
        }
        CloseHandle(pi.hThread);
        hReadEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!hReadEvent) {
            CloseHandle(inW);
            CloseHandle(outR);
            CloseHandle(errR);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            return false;
        }
        hProcess = pi.hProcess;
        hStdoutR = outR;
        hStderrR = errR;
        alive = true;
        started = std::chrono::steady_clock::now();
        startWriter(inW);
        return true;
    }

    static bool writeAll(HANDLE h, const std::string& s) {
        size_t off = 0;
        DWORD wr;
        while (off < s.size()) {
            if (!WriteFile(h, s.data() + off, (DWORD)(s.size() - off), &wr, nullptr) || wr == 0) return false;
            off += wr;
        }
        return true;
    }
    static void closeInput(HANDLE h) { CloseHandle(h); }

    // Agent#readError: when bytes are available, reads (and here discards) at most 4096 of them.
    // The runner does it once per player at the start and after each execution of that player, so
    // a bot that writes more than that to stderr fills the pipe and blocks (then times out), as
    // with the jar.
    void readError() {
        DWORD avail = 0;
        if (!hStderrR || !PeekNamedPipe(hStderrR, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
        char tmp[4096];
        DWORD rd = 0;
        ReadFile(hStderrR, tmp, (DWORD)sizeof(tmp), &rd, nullptr);
    }

    // Reads more bytes into readBuf. Returns false on timeout, EOF or error.
    bool fill(std::chrono::steady_clock::time_point deadline) {
        auto now = std::chrono::steady_clock::now();
        if (!alive || now >= deadline) return false;
        DWORD ms = (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (ms < 1) ms = 1;
        char tmp[4096];
        // Issue an overlapped read; it either completes synchronously (data already buffered),
        // returns ERROR_IO_PENDING (we wait on the event), or fails (EOF / broken pipe).
        OVERLAPPED ov{};
        ov.hEvent = hReadEvent;
        ResetEvent(hReadEvent);
        DWORD rd = 0;
        BOOL ok = ReadFile(hStdoutR, tmp, (DWORD)sizeof(tmp), &rd, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                alive = false;
                return false;
            }
            DWORD wr = WaitForSingleObject(hReadEvent, ms);
            if (wr == WAIT_TIMEOUT) {
                // Bot blew its budget: cancel the pending read, then drain the cancellation result so
                // the OVERLAPPED is freed. The player is deactivated anyway.
                CancelIoEx(hStdoutR, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(hStdoutR, &ov, &ignored, TRUE);
                alive = false;
                return false;
            }
            if (wr != WAIT_OBJECT_0 || !GetOverlappedResult(hStdoutR, &ov, &rd, FALSE)) {
                alive = false;
                return false;
            }
        }
        if (rd == 0) {
            alive = false;
            return false;
        }
        readBuf.append(tmp, rd);
        return true;
    }

    void kill() {
        stopWriter();
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
            CloseHandle(hProcess);
        }
        if (hStdoutR) CloseHandle(hStdoutR);
        if (hReadEvent) CloseHandle(hReadEvent);
        if (hStderrR) CloseHandle(hStderrR);
        hProcess = nullptr;
        hStdoutR = nullptr;
        hReadEvent = nullptr;
        hStderrR = nullptr;
        alive = false;
    }
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <wordexp.h>
#include <poll.h>
#include <cerrno>

// ----------------------------------------------------------------------------
// Child process I/O (POSIX: macOS / Linux)
// ----------------------------------------------------------------------------
struct ChildProc : InputWriter<ChildProc, int> {
    pid_t pid = -1;
    int fdStdoutR = -1;
    bool alive = false;
    std::string readBuf; // bytes read but not consumed yet
    size_t readPos = 0;

    bool start(const std::string& cmd, const char* stderrPath) {
        int inPipe[2], outPipe[2];
        if (pipe(inPipe) != 0) return false;
        if (pipe(outPipe) != 0) {
            close(inPipe[0]);
            close(inPipe[1]);
            return false;
        }

        int errFd = open(stderrPath ? stderrPath : "/dev/null", stderrPath ? (O_WRONLY | O_CREAT | O_TRUNC) : O_WRONLY, 0666);
        if (errFd < 0) {
            close(inPipe[0]);
            close(inPipe[1]);
            close(outPipe[0]);
            close(outPipe[1]);
            return false;
        }

        pid_t c = fork();
        if (c < 0) {
            close(errFd);
            close(inPipe[0]);
            close(inPipe[1]);
            close(outPipe[0]);
            close(outPipe[1]);
            return false;
        }
        if (c == 0) {
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errFd, STDERR_FILENO);
            close(errFd);
            close(inPipe[0]);
            close(inPipe[1]);
            close(outPipe[0]);
            close(outPipe[1]);

            bool needShell = cmd.find_first_of("&|<>^()") != std::string::npos;
            if (needShell) {
                execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            } else {
                wordexp_t w{};
                int we = wordexp(cmd.c_str(), &w, WRDE_NOCMD);
                if (we != 0 || w.we_wordc == 0) _exit(127);
                execvp(w.we_wordv[0], w.we_wordv);
                wordfree(&w);
                _exit(127);
            }
            _exit(127);
        }

        close(errFd);
        close(inPipe[0]);
        close(outPipe[1]);
        fdStdoutR = outPipe[0];
        pid = c;
        alive = true;
        started = std::chrono::steady_clock::now();
        startWriter(inPipe[1]);
        return true;
    }

    static bool writeAll(int fd, const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n;
            do {
                n = write(fd, s.data() + off, s.size() - off);
            } while (n < 0 && errno == EINTR);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }
    static void closeInput(int fd) { close(fd); }

    // Not reproduced on POSIX: agents' stderr goes to /dev/null. (The Linux jar drains it like the
    // Windows one, but through a 64 KB pipe and a BufferedInputStream; see the Windows readError.)
    void readError() {}

    // Reads more bytes into readBuf. Returns false on timeout, EOF or error.
    bool fill(std::chrono::steady_clock::time_point deadline) {
        auto now = std::chrono::steady_clock::now();
        if (!alive || now >= deadline) return false;
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (ms < 1) ms = 1;
        struct pollfd pfd;
        pfd.fd = fdStdoutR;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr;
        do {
            pr = poll(&pfd, 1, ms);
        } while (pr < 0 && errno == EINTR);
        if (pr <= 0) {
            alive = false;
            return false;
        }
        char tmp[4096];
        ssize_t rd;
        do {
            rd = read(fdStdoutR, tmp, sizeof(tmp));
        } while (rd < 0 && errno == EINTR);
        if (rd <= 0) {
            alive = false;
            return false;
        }
        readBuf.append(tmp, (size_t)rd);
        return true;
    }

    void kill() {
        stopWriter();
        if (fdStdoutR >= 0) {
            close(fdStdoutR);
            fdStdoutR = -1;
        }
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
        alive = false;
    }
#endif

    bool lastCR = false;
    std::chrono::steady_clock::time_point started; // when the bot process was created

    // Agent#getOutput: reads until `nbLine` line terminators ('\n', '\r', "\r\n" counts once) or
    // AGENT_MAX_BUFFER_SIZE bytes, within `timeoutMs` counted from `clockStart` (or from now if
    // later). Extra bytes are kept for the next turn. Returns false when the engine would report a
    // timeout (also on EOF and on too long outputs).
    bool getOutput(int nbLine, int timeoutMs, std::string& out, std::chrono::steady_clock::time_point clockStart) {
        out.clear();
        auto deadline = std::max(std::chrono::steady_clock::now(), clockStart) + std::chrono::milliseconds(timeoutMs);
        int nbOccurences = 0;
        while (out.size() < C::AGENT_MAX_BUFFER_SIZE && nbOccurences < nbLine) {
            if (readPos >= readBuf.size()) {
                readBuf.clear();
                readPos = 0;
                if (!fill(deadline)) return false;
                continue;
            }
            char curByte = readBuf[readPos++];
            if (!(curByte == '\n' && lastCR)) {
                out += curByte;
                if (curByte == '\n' || curByte == '\r') ++nbOccurences;
            }
            lastCR = curByte == '\r';
        }
        return nbOccurences == nbLine;
    }
};

// ----------------------------------------------------------------------------
// Engine: GameManager / GameRunner
// ----------------------------------------------------------------------------
static ChildProc procs[2];

// The runner reads each referee command into a 30000-byte buffer (GameRunner#readCommand,
// RefereeAgent#getOutput). The referee prints the content and a line separator; '\r' of "\r\n"
// takes one byte of the buffer and a lone '\n' too.
static void checkRunnerBuffer(const std::string& content) {
    if (content.size() + 1 > C::REFEREE_MAX_BUFFER_SIZE)
        throw RefereeCrash{"java.lang.RuntimeException: Error reading Referee command. Buffer capacity: " +
                           std::to_string(C::REFEREE_MAX_BUFFER_SIZE) + " / " + std::to_string(C::REFEREE_MAX_BUFFER_SIZE)};
}

static std::string jsonString(const std::u16string& s) {
    std::string out = "\"";
    char buf[8];
    for (char16_t c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c >= 0x20 && c < 0x7F) {
            out += (char)c;
        } else {
            std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
            out += buf;
        }
    }
    return out + "\"";
}

// GameManager#dumpInfos (the size of the commands matters, see checkRunnerBuffer). Returns, for
// the trace, the summary and the tooltips as the runner records them for this round.
static std::string dumpInfos() {
    std::string traced;
    if (newTurn) {
        std::string content;
        for (auto& s : prevGameSummary) content += s + "\n";
        content = java::trim(content);
        checkRunnerBuffer(content);
        if (trace) traced += "SUM " + jsonString(java::decodeUtf8(content.empty() ? content : content + "\n")) + "\n";
    } else if (trace) {
        traced += "SUM \"\"\n";
    }
    if (newTurn && !prevTooltips.empty()) {
        std::string content;
        for (auto& t : prevTooltips) content += t.message + "\n" + std::to_string(t.player) + "\n";
        checkRunnerBuffer(java::trim(content));
        if (trace)
            for (auto& t : prevTooltips) traced += "TIP " + std::to_string(t.player) + " " + jsonString(java::decodeUtf8(t.message)) + "\n";
    }
    return traced;
}

// GameManager#execute + the runner's side of the turn (GameRunner#getNextPlayerOutput,
// #sendPlayerOutput) + the referee's Scanner reading SET_PLAYER_OUTPUT
static void execute(Player& player, int nbrOutputLines) {
    player.timeout = false;
    if (scannerPendingLines > 0) {
        // The referee's Scanner reads the rest of a previous player line as the GET_GAME_INFO command
        // header. (Not reproduced: a rest that is a valid "[[GET_GAME_INFO] 0]" header, which would
        // shift the protocol instead.)
        throw RefereeCrash{"java.lang.RuntimeException: Error in data sent to referee"};
    }
    std::string traced = dumpInfos();
    if (nbrOutputLines > 0) {
        totalTurnTime += C::TURN_MAX_TIME; // addTurnTime
        if (totalTurnTime > C::GAME_DURATION_HARD_QUOTA)
            throw RefereeCrash{"java.lang.RuntimeException: Total game duration too long (>30000ms)"};
    }
    int timeoutMs = player.hasNeverBeenExecuted ? C::FIRST_TURN_MAX_TIME : C::TURN_MAX_TIME;

    if (trace) fputs(traced.c_str(), trace);
    ChildProc& proc = procs[player.index];
    proc.send(player.inputs);
    player.inputs.clear();
    if (nbrOutputLines == 0) {
        // getOutput(0, ...) reads nothing and the runner records "" for this player
        if (trace) fprintf(trace, "T%d P%d OUT [\"\"]\n", engineTurn, player.index);
        proc.readError();
    } else {
        std::string raw;
        // The jar starts the bots, then initialises its referee (0.3-1.1 s measured) before it sends
        // the first input, so a bot's startup is mostly not counted in its first turn; here the
        // first input comes right after the start. The first turn of a player is therefore timed
        // as if its input had been sent BOT_STARTUP_GRACE_MS after the bot was started.
        auto clockStart = player.hasNeverBeenExecuted ? proc.started + std::chrono::milliseconds(C::BOT_STARTUP_GRACE_MS)
                                                      : std::chrono::steady_clock::time_point();
        bool ok = proc.getOutput(nbrOutputLines, timeoutMs, raw, clockStart);
        std::u16string out;
        if (ok) {
            out = java::decodeUtf8(raw);
            for (auto& ch : out)
                if (ch == '\r') ch = '\n';
            // checkOutput: the number of '\n' equals nbLines when getOutput stopped on a terminator
        }
        proc.readError(); // (after a timeout the runner first sleeps for the timeout: no matter here)
        if (!ok) {
            player.timeout = true;
            if (trace) fprintf(trace, "T%d P%d TIMEOUT\n", engineTurn, player.index);
        } else {
            // sendPlayerOutput: split("(\\n|\\r\\n)") and keep the first line (nbLines == 1)
            std::u16string line = out.substr(0, out.find(u'\n'));
            if (trace) fprintf(trace, "T%d P%d OUT [%s]\n", engineTurn, player.index, jsonString(line).c_str());
            // the referee's Scanner#nextLine ends the line at the first line separator; every
            // separator leaves one more line in the Scanner
            size_t cut = line.size();
            for (size_t i = 0; i < line.size(); i++) {
                if (java::isScannerLineSeparator(line[i])) {
                    if (cut == line.size()) cut = i;
                    scannerPendingLines++;
                }
            }
            player.output = line.substr(0, cut);
        }
    }
    newTurn = false;
}

// AbstractPlayer#execute
static void playerExecute(Player& player) {
    execute(player, 1);
    player.hasBeenExecuted = true;
    player.hasNeverBeenExecuted = false;
}

// ----------------------------------------------------------------------------
// CommandManager.java
// ----------------------------------------------------------------------------
static void deactivatePlayer(Player& player, const std::string& message) {
    // escapeHTMLEntities, then AbstractMultiplayerPlayer#deactivate(reason) adds a tooltip
    std::string m = java::replace(java::replace(message, "&lt;", "<"), "&gt;", ">");
    player.active = false;
    addTooltip(player.index, m);
    player.score = -1;
}

// "(?:\\s+(?<message>.*))?" then the end of the input (Matcher#matches)
static bool messageMatches(const std::u16string& s, size_t pos) {
    if (pos == s.size()) return true;
    if (!java::isRegexSpace(s[pos])) return false;
    while (pos < s.size() && java::isRegexSpace(s[pos])) pos++;
    for (; pos < s.size(); pos++)
        if (java::isLineTerminator(s[pos])) return false;
    return true;
}

static bool startsWith(const std::u16string& s, size_t pos, const char* lit) {
    for (size_t i = 0; lit[i]; i++)
        if (pos + i >= s.size() || s[pos + i] != (char16_t)(unsigned char)lit[i]) return false;
    return true;
}

// "\\d+" (ASCII digits); returns the end of the digit run
static size_t digits(const std::u16string& s, size_t pos) {
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
    return pos;
}

struct InvalidInput {
    std::string got; // UTF-8
};

// Integer.parseInt on a run of ASCII digits
static i32 parseInt(const std::u16string& s, size_t a, size_t b) {
    i64 v = 0;
    for (size_t i = a; i < b; i++) {
        v = v * 10 + (s[i] - '0');
        if (v > INT32_MAX) {
            std::string str;
            for (size_t j = a; j < b; j++) str += (char)s[j];
            // InvalidInputException(Game.getExpected(), e.toString())
            throw InvalidInput{"java.lang.NumberFormatException: For input string: \"" + str + "\""};
        }
    }
    return (i32)v;
}

static void parseCommands(Player& player, const std::u16string& command) {
    if (player.waiting) return;
    try {
        size_t p;
        // ^WAIT(?:\s+(?<message>.*))?
        if (startsWith(command, 0, "WAIT") && messageMatches(command, 4)) {
            player.action.type = WAIT;
            return;
        }
        // ^GROW (?<targetId>\d+)(?:\s+(?<message>.*))?
        if (C::ENABLE_GROW && startsWith(command, 0, "GROW ") && (p = digits(command, 5)) > 5 && messageMatches(command, p)) {
            i32 targetId = parseInt(command, 5, p);
            player.action = Action{GROW, 0, targetId};
            return;
        }
        // ^COMPLETE (?<targetId>\d+)(?:\s+(?<message>.*))?
        if (startsWith(command, 0, "COMPLETE ") && (p = digits(command, 9)) > 9 && messageMatches(command, p)) {
            i32 targetId = parseInt(command, 9, p);
            player.action = Action{COMPLETE, 0, targetId};
            return;
        }
        // ^SEED (?<sourceId>\d+) (?<targetId>\d+)(?:\s+(?<message>.*))?
        if (C::ENABLE_SEED && startsWith(command, 0, "SEED ")) {
            size_t a = digits(command, 5);
            if (a > 5 && a < command.size() && command[a] == ' ') {
                size_t b = digits(command, a + 1);
                if (b > a + 1 && messageMatches(command, b)) {
                    i32 sourceId = parseInt(command, 5, a);
                    i32 targetId = parseInt(command, a + 1, b);
                    player.action = Action{SEED, sourceId, targetId};
                    return;
                }
            }
        }
        throw InvalidInput{java::toUtf8(command)};
    } catch (const InvalidInput& e) {
        deactivatePlayer(player, std::string("Invalid Input: Expected ") + C::EXPECTED + " but got '" + e.got + "'");
        gameSummaryManager.addPlayerBadCommand(player, C::EXPECTED, e.got);
        gameSummaryManager.addPlayerDisqualified(player);
    }
}

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
static void handlePlayerCommands() {
    for (Player* player : getActivePlayers()) {
        if (player->waiting) continue;
        if (player->timeout) {
            // TimeoutException
            deactivatePlayer(*player, "Timeout!");
            gameSummaryManager.addPlayerTimeout(*player);
            gameSummaryManager.addPlayerDisqualified(*player);
        } else {
            parseCommands(*player, player->output);
        }
    }
}

static void gameTurn() {
    game.resetGameTurnData();
    if (game.currentFrameType == ACTIONS) {
        for (Player* player : getActivePlayers()) {
            if (!player->waiting) {
                player->inputs += game.getCurrentFrameInfoFor(*player);
                playerExecute(*player);
            }
        }
        handlePlayerCommands();
    }
    game.performGameUpdate();
}

static void swapInfoAndViewData() {
    prevGameSummary = currentGameSummary;
    currentGameSummary.clear();
    prevTooltips = currentTooltips;
    currentTooltips.clear();
}

// GameManager#start
static void runGame(i64 seed) {
    for (int i = 0; i < 2; i++) players[i].index = i;
    // Referee#init
    game.init(seed);
    for (Player* player : getActivePlayers()) player->inputs += game.getGlobalInfoFor(*player);
    for (engineTurn = 1; !gameEnd && !getActivePlayers().empty(); engineTurn++) {
        swapInfoAndViewData();
        newTurn = true;
        gameTurn();
        // Create a frame if no player has been executed
        if (!players[0].hasBeenExecuted && !players[1].hasBeenExecuted) execute(players[0], 0);
        for (Player& p : players) {
            p.output.clear();
            p.hasBeenExecuted = false;
        }
        if (trace)
            fprintf(trace, "  turn %d round %d frame %d sun %d %d score %d %d nutrients %d\n", engineTurn, game.round,
                    (int)game.currentFrameType, players[0].sun, players[1].sun, players[0].score, players[1].score,
                    game.nutrients);
    }
    game.onEnd();
    swapInfoAndViewData();
    newTurn = true;
    std::string traced = dumpInfos(); // final frame: summary and tooltips of the last turn
    if (trace) fputs(traced.c_str(), trace);
}

int main(int argc, char** argv) {
    // CommandLineInterface (commons-cli): -p1 -p2 [-p3 -p4] -l -seed take a value, the first
    // occurrence wins; a missing value or an unknown option is an error (exit 1)
    const char* opt[6] = {"-p1", "-p2", "-p3", "-p4", "-l", "-seed"};
    const char* val[6] = {};
    bool help = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        int o = -1;
        for (int k = 0; k < 6; k++)
            if (a == opt[k]) o = k;
        if (a == "-h") {
            help = true;
        } else if (o >= 0) {
            bool isOpt = i + 1 < argc && (std::string(argv[i + 1]) == "-h" ||
                                          std::find_if(opt, opt + 6, [&](const char* x) { return std::string(argv[i + 1]) == x; }) != opt + 6);
            if (i + 1 >= argc || isOpt) {
                fprintf(stderr, "org.apache.commons.cli.MissingArgumentException: Missing argument for option: %s\n", a.c_str() + 1);
                return 1;
            }
            if (!val[o]) val[o] = argv[i + 1];
            i++;
        } else if (a.size() > 1 && a[0] == '-') {
            fprintf(stderr, "org.apache.commons.cli.UnrecognizedOptionException: Unrecognized option: %s\n", a.c_str());
            return 1;
        }
    }
    if (help || !val[0] || !val[1]) {
        printf("usage: -p1 <player1 command line> -p2 <player2 command line> [-p3 <cmd> -p4 <cmd> -l <log file> -seed <seed>]\n");
        return 0;
    }
    if (val[2] || val[3]) {
        // not reproduced: the jar would start a 3rd/4th player and fail at turn 2 (null scores)
        fprintf(stderr, "Spring Challenge 2021 is a 2-player game\n");
        return 1;
    }
    std::string bot1 = val[0], bot2 = val[1];
    const char* tracePath = val[4];
    i64 seed = (i64)std::chrono::steady_clock::now().time_since_epoch().count();
    if (val[5]) {
        // Long.parseLong (ASCII digits)
        const char* s = val[5];
        size_t k = 0, n = strlen(s);
        bool neg = false;
        if (n > 0 && (s[0] == '-' || s[0] == '+')) neg = s[k++] == '-';
        u64 v = 0, limit = (u64)INT64_MAX + (neg ? 1 : 0);
        bool okSeed = k < n;
        for (; k < n && okSeed; k++) {
            u64 d = (u64)(s[k] - '0');
            if (s[k] < '0' || s[k] > '9' || v > (limit - d) / 10) okSeed = false;
            else v = v * 10 + d;
        }
        if (!okSeed) {
            fprintf(stderr, "java.lang.NumberFormatException: For input string: \"%s\"\n", s);
            return 1;
        }
        seed = neg ? (i64)(0 - v) : (i64)v;
    }
    if (tracePath) trace = fopen(tracePath, "w");

#ifdef _WIN32
    if (!procs[0].start(bot1) || !procs[1].start(bot2)) {
        fprintf(stderr, "java.lang.RuntimeException: Failed to launch a player\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
    if (!procs[0].start(bot1, nullptr) || !procs[1].start(bot2, nullptr)) {
        fprintf(stderr, "java.lang.RuntimeException: Failed to launch a player\n");
        return 1;
    }
#endif
    // GameRunner#readInitFrameErrors
    procs[0].readError();
    procs[1].readError();

    bool crashed = false;
    try {
        runGame(seed);
    } catch (const RefereeCrash& crash) {
        // the runner gets a FAIL instead of the scores: the CLI prints null scores and exits 0
        fprintf(stderr, "%s\n", crash.what.c_str());
        if (trace) fprintf(trace, "CRASH %s\n", crash.what.c_str());
        crashed = true;
    }

    procs[0].kill();
    procs[1].kill();
    if (crashed) {
        printf("null\nnull\n");
    } else {
        if (trace) fprintf(trace, "SCORES %d %d\n", players[0].score, players[1].score);
        printf("%d\n%d\nseed=%lld\n", players[0].score, players[1].score, (long long)seed);
    }
    if (trace) fclose(trace);
    return 0;
}
