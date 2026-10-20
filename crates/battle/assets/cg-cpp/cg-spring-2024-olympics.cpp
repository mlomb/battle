// Summer Challenge 2024 "Olymbits" - headless C++ referee, port of
// https://github.com/CodinGame/SummerChallenge2024Olymbits (commit 6529670) running on the
// CodinGame game engine 4.5.0 (core + runner), highest league (the 4 mini games), 3 players.
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -p3 "<bot3 cmd>" [-p4 "<cmd>"] -seed <N> [-l <file>]
// Prints one score per player, one per line, then "seed=<N>" (like the jar). When the Java referee
// fails (see below) it prints "null" for every player and exits with code 0, like the jar does.
// -l writes a trace with one line per player execution:
//   T<turn> P<player> OUT ["<line>"]      (JSON, the line as the runner received it)
//   T<turn> P<player> TIMEOUT
//
// The Java code is followed closely: same data structures, iteration orders and Java library
// semantics. Notable Java behaviours reproduced here:
//   - gameManager.getRandom() is SecureRandom("SHA1PRNG") seeded with setSeed(long). SecureRandom
//     ignores setSeed(0): with -seed 0 the jar self-seeds from system entropy, and so do we.
//     Consumers: Random.nextInt(bound) (rejection loop), nextBoolean, nextDouble,
//     Collections.shuffle; DoubleStream.sum is the compensated (Kahan) sum.
//   - rankings: stable sort on Double.compare, ties detected with Double.equals
//   - Agent#getOutput: '\r' and '\n' end a line ("\r\n" counts once, even across turns), at most
//     10000 bytes per turn, 1000 ms for the first turn of each player and 50 ms afterwards; the
//     bytes are decoded like new String(bytes, UTF_8) in JDK 26 (same U+FFFD replacements)
//   - the referee runs behind the runner's text protocol and reads its commands with a
//     java.util.Scanner, whose nextLine() also ends lines at U+0085, U+2028 and U+2029. A player
//     line containing one of them desynchronizes the protocol: the rest of the line is read as the
//     next command (usually "Error in data sent to referee", a crash; a crafted line can even
//     inject commands). The protocol is emulated line by line, so all of this is reproduced.
//   - the runner reads every referee command into a 30000-byte buffer (100000 on its first read).
//     A SUMMARY or TOOLTIP command holding long invalid player lines (U+FFFD takes 3 bytes in
//     UTF-8) overflows it and the game fails.
//   - when the referee fails or the protocol deadlocks, the runner stops: no scores ("null")
//   - 2 players crash the Java referee in the viewer module during init, 4 players in the first
//     turn (MiniGame.fillRegisters overflows); both are reproduced
// Not reproduced (documented): a Turkish default locale (toUpperCase of "right"), and agents
// flooding stderr (the jar reads at most 4 KB of agent stderr per turn; we discard it).

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
#include <cmath>
#include <cfloat>
#include <climits>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;
typedef uint8_t u8;

// ----------------------------------------------------------------------------
// Java semantics
// ----------------------------------------------------------------------------
namespace java {

typedef std::u16string String;

struct Exception {
    std::string text; // what printStackTrace would print first
};

static String str(const std::string& ascii) { return String(ascii.begin(), ascii.end()); }

static String str(long long v) { return str(std::to_string(v)); }

// new String(bytes, UTF_8) of JDK 26 (String.utf8 / decodeUTF8_UTF16 with replacement)
static String decodeUtf8(const std::string& bytes) {
    const signed char* src = (const signed char*)bytes.data();
    int sp = 0, sl = (int)bytes.size();
    const char16_t REPL = 0xFFFD;
    String dst;
    dst.reserve(bytes.size());
    auto isNotContinuation = [](int b) { return (b & 0xc0) != 0x80; };
    auto isMalformed3 = [](int b1, int b2, int b3) {
        return (b1 == (signed char)0xe0 && (b2 & 0xe0) == 0x80) || (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80;
    };
    auto isMalformed3_2 = [](int b1, int b2) { return (b1 == (signed char)0xe0 && (b2 & 0xe0) == 0x80) || (b2 & 0xc0) != 0x80; };
    auto isMalformed4 = [](int b2, int b3, int b4) { return (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80 || (b4 & 0xc0) != 0x80; };
    auto isMalformed4_2 = [](int b1, int b2) {
        return (b1 == 0xf0 && (b2 < 0x90 || b2 > 0xbf)) || (b1 == 0xf4 && (b2 & 0xf0) != 0x80) || (b2 & 0xc0) != 0x80;
    };
    auto isMalformed4_3 = [](int b3) { return (b3 & 0xc0) != 0x80; };
    auto malformed3 = [&](int p) {
        int b1 = src[p++];
        int b2 = src[p];
        return ((b1 == (signed char)0xe0 && (b2 & 0xe0) == 0x80) || isNotContinuation(b2)) ? 1 : 2;
    };
    auto malformed4 = [&](int p) {
        int b1 = src[p++] & 0xff;
        int b2 = src[p++] & 0xff;
        if (b1 > 0xf4 || (b1 == 0xf0 && (b2 < 0x90 || b2 > 0xbf)) || (b1 == 0xf4 && (b2 & 0xf0) != 0x80) || isNotContinuation(b2))
            return 1;
        if (isNotContinuation(src[p])) return 2;
        return 3;
    };
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
                    dst += (char16_t)(((b1 & 0x1f) << 6) | (b2 & 0x3f));
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
                    sp += malformed3(sp);
                } else {
                    char16_t c = (char16_t)(((b1 & 0x0f) << 12) | ((b2 & 0x3f) << 6) | (b3 & 0x3f));
                    dst += (c >= 0xD800 && c <= 0xDFFF) ? REPL : c;
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
                int uc = ((b1 & 0x07) << 18) | ((b2 & 0x3f) << 12) | ((b3 & 0x3f) << 6) | (b4 & 0x3f);
                if (isMalformed4(b2, b3, b4) || !(uc >= 0x10000 && uc <= 0x10FFFF)) {
                    dst += REPL;
                    sp -= 4;
                    sp += malformed4(sp);
                } else {
                    dst += (char16_t)(0xD800 + ((uc - 0x10000) >> 10));
                    dst += (char16_t)(0xDC00 + ((uc - 0x10000) & 0x3ff));
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

// String.getBytes(UTF_8) (an unpaired surrogate becomes '?')
static std::string encodeUtf8(const String& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = '?';
        }
        if (c < 0x80) {
            r += (char)c;
        } else if (c < 0x800) {
            r += (char)(0xC0 | (c >> 6));
            r += (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            r += (char)(0xE0 | (c >> 12));
            r += (char)(0x80 | ((c >> 6) & 0x3f));
            r += (char)(0x80 | (c & 0x3f));
        } else {
            r += (char)(0xF0 | (c >> 18));
            r += (char)(0x80 | ((c >> 12) & 0x3f));
            r += (char)(0x80 | ((c >> 6) & 0x3f));
            r += (char)(0x80 | (c & 0x3f));
        }
    }
    return r;
}

// String.trim
static String trim(const String& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] <= u' ') a++;
    while (b > a && s[b - 1] <= u' ') b--;
    return s.substr(a, b - a);
}

// String.replace(CharSequence, CharSequence)
static String replace(const String& s, const String& target, const String& repl) {
    String r;
    size_t off = 0, next;
    while ((next = s.find(target, off)) != String::npos) {
        r.append(s, off, next - off);
        r += repl;
        off = next + target.size();
    }
    r.append(s, off, String::npos);
    return r;
}

// String.split(regex) where the regex matches single chars of `seps` (and "\r\n" when '\r' is
// one of them): trailing empty strings are removed
static std::vector<String> splitLines(const String& s, const char16_t* seps) {
    std::vector<String> r;
    String cur;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (std::char_traits<char16_t>::find(seps, std::char_traits<char16_t>::length(seps), c)) {
            if (c == u'\r' && i + 1 < s.size() && s[i + 1] == u'\n') i++;
            r.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    r.push_back(cur);
    while (!r.empty() && r.back().empty()) r.pop_back();
    return r;
}

// Integer.parseInt / Long.parseLong (radix 10, ASCII digits, optional sign)
static bool parseLong(const String& s, i64 minV, i64 maxV, i64& out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == u'-' || s[0] == u'+') {
        neg = s[0] == u'-';
        i = 1;
        if (s.size() == 1) return false;
    }
    // accumulate negatively like Java to cover MIN_VALUE
    i64 limit = neg ? minV : -maxV;
    i64 multmin = limit / 10;
    i64 result = 0;
    for (; i < s.size(); i++) {
        char16_t c = s[i];
        int d = (c >= u'0' && c <= u'9') ? c - u'0' : -1;
        if (d < 0 || result < multmin) return false;
        result *= 10;
        if (result < limit + d) return false;
        result -= d;
    }
    out = neg ? result : -result;
    return true;
}

// Double.compare
static int compareDouble(double a, double b) {
    if (a < b) return -1;
    if (a > b) return 1;
    i64 x, y;
    double na = a != a ? NAN : a, nb = b != b ? NAN : b; // doubleToLongBits collapses NaNs
    memcpy(&x, &na, 8);
    memcpy(&y, &nb, 8);
    if (a != a) x = 0x7ff8000000000000LL;
    if (b != b) y = 0x7ff8000000000000LL;
    return x == y ? 0 : (x < y ? -1 : 1);
}

// Double.equals
static bool equalsDouble(double a, double b) { return compareDouble(a, b) == 0; }

// JSON string (ASCII only, \uXXXX escapes), for the trace
static std::string json(const String& s) {
    std::string r = "\"";
    char buf[8];
    for (char16_t c : s) {
        if (c == u'"') r += "\\\"";
        else if (c == u'\\') r += "\\\\";
        else if (c >= 0x20 && c < 0x7f) r += (char)c;
        else {
            snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
            r += buf;
        }
    }
    return r + "\"";
}

// ---------------------------------------------------------------------------
// SHA-1 and SecureRandom("SHA1PRNG") (sun.security.provider.SecureRandom)
// ---------------------------------------------------------------------------
struct SHA1 {
    static inline u32 rol(u32 v, int b) { return (v << b) | (v >> (32 - b)); }
    static void hash(const u8* msg, size_t n, u8 out[20]) {
        u32 h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
        u64 ml = (u64)n * 8;
        size_t total = n + 1;
        while (total % 64 != 56) total++;
        total += 8;
        std::vector<u8> buf(total, 0);
        if (n) memcpy(buf.data(), msg, n);
        buf[n] = 0x80;
        for (int i = 0; i < 8; i++) buf[total - 1 - i] = (u8)((ml >> (8 * i)) & 0xff);
        for (size_t off = 0; off < total; off += 64) {
            u32 w[80];
            for (int i = 0; i < 16; i++)
                w[i] = ((u32)buf[off + i * 4] << 24) | ((u32)buf[off + i * 4 + 1] << 16) | ((u32)buf[off + i * 4 + 2] << 8) |
                       (u32)buf[off + i * 4 + 3];
            for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            u32 a = h0, b = h1, c = h2, d = h3, e = h4;
            for (int i = 0; i < 80; i++) {
                u32 f, k;
                if (i < 20) {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999u;
                } else if (i < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1u;
                } else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDCu;
                } else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6u;
                }
                u32 t = rol(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = rol(b, 30);
                b = a;
                a = t;
            }
            h0 += a;
            h1 += b;
            h2 += c;
            h3 += d;
            h4 += e;
        }
        u32 hh[5] = {h0, h1, h2, h3, h4};
        for (int i = 0; i < 5; i++) {
            out[i * 4] = (u8)(hh[i] >> 24);
            out[i * 4 + 1] = (u8)(hh[i] >> 16);
            out[i * 4 + 2] = (u8)(hh[i] >> 8);
            out[i * 4 + 3] = (u8)(hh[i]);
        }
    }
};

// some entropy, where the jar uses a random seed (no -seed, or -seed 0)
static u64 entropy() {
    static u64 counter = 0;
    u64 x = (u64)std::chrono::steady_clock::now().time_since_epoch().count();
    x ^= (u64)std::chrono::system_clock::now().time_since_epoch().count() * 0x9E3779B97F4A7C15ULL;
    x ^= (u64)(uintptr_t)&counter + (++counter) * 0xD1B54A32D192ED03ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// java.security.SecureRandom with the SHA1PRNG engine, used through the java.util.Random API
struct SecureRandom {
    static const int DIGEST_SIZE = 20;
    u8 state[DIGEST_SIZE];
    bool haveState = false;
    u8 remainder[DIGEST_SIZE] = {0};
    int remCount = 0;

    void engineSetSeed(const u8* seed, size_t n) {
        std::vector<u8> msg;
        if (haveState) msg.insert(msg.end(), state, state + DIGEST_SIZE);
        msg.insert(msg.end(), seed, seed + n);
        SHA1::hash(msg.data(), msg.size(), state);
        haveState = true;
        remCount = 0;
    }

    // SecureRandom.setSeed(long): 0 is ignored (the engine then seeds itself from system entropy)
    void setSeed(i64 seed) {
        if (seed == 0) return;
        u8 b[8];
        for (int i = 0; i < 8; i++) b[i] = (u8)(((u64)seed >> (8 * i)) & 0xff);
        engineSetSeed(b, 8);
    }

    static void updateState(u8* st, const u8* output) {
        int last = 1;
        bool zf = false;
        for (int i = 0; i < DIGEST_SIZE; i++) {
            int v = (int)(int8_t)st[i] + (int)(int8_t)output[i] + last;
            u8 t = (u8)v;
            zf = zf || (st[i] != t);
            st[i] = t;
            last = v >> 8;
        }
        if (!zf) st[0]++;
    }

    void engineNextBytes(u8* result, int len) {
        if (!haveState) {
            // unseeded (setSeed(0)): the jar seeds from system entropy, so does this port
            u8 seed[DIGEST_SIZE];
            for (int i = 0; i < DIGEST_SIZE; i++) seed[i] = (u8)entropy();
            SHA1::hash(seed, DIGEST_SIZE, state);
            haveState = true;
        }
        int index = 0;
        int r = remCount;
        if (r > 0) {
            int todo = std::min(len - index, DIGEST_SIZE - r);
            for (int i = 0; i < todo; i++) {
                result[i] = remainder[r];
                remainder[r++] = 0;
            }
            remCount += todo;
            index += todo;
        }
        while (index < len) {
            u8 output[DIGEST_SIZE];
            SHA1::hash(state, DIGEST_SIZE, output);
            updateState(state, output);
            int todo = std::min(len - index, DIGEST_SIZE);
            for (int i = 0; i < todo; i++) {
                result[index++] = output[i];
                output[i] = 0;
            }
            remCount += todo;
            memcpy(remainder, output, DIGEST_SIZE);
        }
        remCount %= DIGEST_SIZE;
    }

    // SecureRandom.next(int)
    i32 next(int numBits) {
        int numBytes = (numBits + 7) / 8;
        u8 b[4];
        engineNextBytes(b, numBytes);
        u32 nxt = 0;
        for (int i = 0; i < numBytes; i++) nxt = (nxt << 8) + b[i];
        return (i32)(nxt >> (numBytes * 8 - numBits));
    }

    // Random.nextInt(int bound)
    i32 nextInt(i32 bound) {
        i32 r = next(31);
        i32 m = bound - 1;
        if ((bound & m) == 0) {
            r = (i32)(((i64)bound * (i64)r) >> 31);
        } else {
            for (i32 u = r; (i32)((u32)u - (u32)(r = u % bound) + (u32)m) < 0; u = next(31))
                ;
        }
        return r;
    }

    bool nextBoolean() { return next(1) != 0; }

    double nextDouble() { return (double)(((i64)next(26) << 27) + next(27)) * 0x1.0p-53; }
};

// Collections.shuffle(list, rnd) for a RandomAccess list
template <class T>
static void shuffle(std::vector<T>& list, SecureRandom& rnd) {
    for (int i = (int)list.size(); i > 1; i--) std::swap(list[i - 1], list[rnd.nextInt(i)]);
}

// DoubleStream.sum() of a sequential stream (Collectors.sumWithCompensation/computeFinalSum)
static double sum(const std::vector<double>& values) {
    double s[3] = {0, 0, 0};
    for (double d : values) {
        double tmp = d - s[1];
        double sm = s[0];
        double velvel = sm + tmp;
        s[1] = (velvel - sm) - tmp;
        s[0] = velvel;
        s[2] += d;
    }
    double tmp = s[0] - s[1];
    if (tmp != tmp && std::isinf(s[2])) return s[2];
    return tmp;
}

} // namespace java

using java::String;

// ----------------------------------------------------------------------------
// Engine constants
// ----------------------------------------------------------------------------
namespace C {
static const int AGENT_MAX_BUFFER_SIZE = 10000;            // Agent
static const int REFEREE_MAX_BUFFER_SIZE = 30000;          // RefereeAgent
static const int REFEREE_MAX_BUFFER_SIZE_EXTRA = 100000;   // RefereeAgent, first read
static const int GAME_SUMMARY_TOTAL_HARD_QUOTA = 512 * 1024;
static const int GAME_SUMMARY_PER_TURN_HARD_QUOTA = 800;
static const int GAME_DURATION_HARD_QUOTA = 30000;
static const int LEAGUE_LEVEL = 19; // CommandLineInterface: setLeagueLevel(19)
} // namespace C

// ----------------------------------------------------------------------------
// Action.java, Player.java
// ----------------------------------------------------------------------------
enum Action { UP = 0, DOWN = 1, LEFT = 2, RIGHT = 3 }; // Action.values() order
static const int NO_ACTION = -1;                        // null
static const char* const ACTION_NAMES[4] = {"UP", "DOWN", "LEFT", "RIGHT"};

struct Tooltip {
    int player;
    String message;
};

struct Player {
    int index = 0;
    // AbstractPlayer / AbstractMultiplayerPlayer
    std::vector<String> inputs;
    std::vector<String> outputs;
    bool timeout = false;
    int score = 0;
    bool hasBeenExecuted = false;
    bool hasNeverBeenExecuted = true;
    bool active = true;
    // Player (the message of a command is stored by the Java referee but never used)
    int action = NO_ACTION;
    std::vector<std::array<int, 3>> medals;

    void init(int gameCount) { medals.assign(gameCount, {0, 0, 0}); }

    int getPoints() const {
        int p = 1;
        for (size_t i = 0; i < medals.size(); ++i) p *= (3 * medals[i][0] + medals[i][1]);
        return p;
    }

    int getExpectedOutputLines() const { return 1; }

    void reset() { action = NO_ACTION; }
    // getMedalsTotal/getScoreText only feed the end screen and the metadata
};

static std::vector<Player> players; // gameManager.getPlayers()

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

static void addTooltip(int player, const String& message);

// AbstractMultiplayerPlayer.deactivate(String)
static void deactivate(Player& p, const String& reason) {
    p.active = false;
    addTooltip(p.index, reason);
}

// ----------------------------------------------------------------------------
// MiniGame.java and the four mini games
// ----------------------------------------------------------------------------
static int PLAYER_COUNT = 3;          // Game.PLAYER_COUNT
static const int REGISTER_COUNT = 7;  // Game.REGISTER_COUNT
static const bool EARLY_RACE_END = true;

static java::Exception indexOutOfBounds(int index, int length) {
    return {"java.lang.IndexOutOfBoundsException: Index " + std::to_string(index) + " out of bounds for length " +
            std::to_string(length)};
}

// MiniGame.fillRegisters
static std::vector<int> fillRegisters(std::initializer_list<const std::vector<int>*> arrays) {
    std::vector<int> registers(REGISTER_COUNT, 0);
    int idx = 0;
    for (const std::vector<int>* array : arrays) {
        for (int integer : *array) {
            if (idx >= REGISTER_COUNT)
                throw java::Exception{"java.lang.ArrayIndexOutOfBoundsException: Index " + std::to_string(idx) +
                                      " out of bounds for length " + std::to_string(REGISTER_COUNT)};
            registers[idx++] = integer;
        }
    }
    return registers;
}

// MiniGame.createRankings: entries in the map's iteration order (TreeMap and HashMap<Integer> with
// keys 0..n-1 both iterate by increasing key)
static std::vector<int> createRankings(const std::vector<std::pair<int, double>>& scoreByPlayer) {
    std::vector<int> rankings(PLAYER_COUNT, 0);
    std::vector<std::pair<int, double>> sorted = scoreByPlayer;
    // stream().sorted(comparingDouble(getValue).reversed()): a stable sort (TimSort uses a binary
    // insertion sort below 32 elements); a plain stable insertion sort gives the same order
    for (size_t i = 1; i < sorted.size(); i++) {
        std::pair<int, double> x = sorted[i];
        size_t j = i;
        while (j > 0 && java::compareDouble(sorted[j - 1].second, x.second) < 0) { // compare(x, a[j-1]) < 0
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = x;
    }
    int rank = 0;
    int lastRank = 0;
    bool hasLastScore = false;
    double lastScore = 0;
    for (auto& e : sorted) {
        int currentRank = rank;
        if (hasLastScore && java::equalsDouble(lastScore, e.second)) currentRank = lastRank;
        lastRank = currentRank;
        rankings[e.first] = currentRank;
        rank++;
        hasLastScore = true;
        lastScore = e.second;
    }
    return rankings;
}

struct MiniGame {
    std::string type;
    bool shouldReset = false, resetting = false;
    virtual ~MiniGame() {}
    virtual void reset(java::SecureRandom& random) = 0;
    virtual std::string getGPU() = 0;
    virtual std::vector<int> getRegisters() = 0;
    virtual void tick(const std::vector<int>& actions) = 0;
    virtual bool isGameOver() = 0;
    virtual std::vector<int> getRankings() = 0;
    virtual std::string getName() = 0;
    // the accesses of Serializer.serializeFrameData that can throw (the view data itself is not
    // needed: it never reaches the players and stays far below the engine quotas)
    virtual void checkViewSerialization() {}
};

struct HurdleRace : MiniGame {
    static const int STUN_DURATION = 2;
    std::string map;
    std::vector<int> positions, stunTimers;
    std::vector<bool> dead, jumped;
    std::vector<int> finished;
    int rank = 0;

    HurdleRace()
        : positions(PLAYER_COUNT, 0), stunTimers(PLAYER_COUNT, 0), dead(PLAYER_COUNT, false), jumped(PLAYER_COUNT, false),
          finished(PLAYER_COUNT, 0) {
        type = "hurdles";
    }

    void reset(java::SecureRandom& random) override {
        int startStretch = 3 + random.nextInt(5);
        int hurdles = 3 + random.nextInt(4);
        int length = 30;
        std::string sb;
        for (int i = 0; i < startStretch; ++i) sb += '.';
        for (int i = 0; i < hurdles; ++i) {
            if (random.nextBoolean()) sb += "#....";
            else sb += "#...";
        }
        while ((int)sb.size() < length) sb += ".";
        map = sb.substr(0, length - 1) + ".";
        for (int i = 0; i < PLAYER_COUNT; ++i) {
            stunTimers[i] = 0;
            positions[i] = 0;
            finished[i] = -1;
            rank = 0;
            jumped[i] = false;
        }
    }

    std::string getGPU() override { return map; }

    std::vector<int> getRegisters() override { return fillRegisters({&positions, &stunTimers}); }

    void tick(const std::vector<int>& actions) override {
        int maxX = (int)map.size() - 1;
        int countFinishes = 0;
        for (int i = 0; i < (int)actions.size(); ++i) {
            jumped[i] = false;
            int a = actions[i];
            if (a == NO_ACTION) {
                dead[i] = true;
                continue;
            }
            if (stunTimers[i] > 0) {
                stunTimers[i] -= 1;
                continue;
            }
            if (finished[i] > -1) {
                continue;
            }
            int moveBy = 0;
            bool jump = false;
            switch (a) {
            case DOWN:
                moveBy = 2;
                break;
            case LEFT:
                moveBy = 1;
                break;
            case RIGHT:
                moveBy = 3;
                break;
            case UP:
                moveBy = 2;
                jump = true;
                jumped[i] = true;
                break;
            }
            for (int x = 0; x < moveBy; ++x) {
                positions[i] = std::min(maxX, positions[i] + 1);
                if (map[positions[i]] == '#' && !jump) {
                    stunTimers[i] = STUN_DURATION;
                    break;
                }
                if (positions[i] == maxX && finished[i] == -1) {
                    finished[i] = rank;
                    countFinishes++;
                    break;
                }
                jump = false;
            }
        }
        rank += countFinishes;
    }

    bool isGameOver() override {
        int count = 0;
        for (int i = 0; i < (int)finished.size(); ++i) {
            if (finished[i] > -1 && EARLY_RACE_END) {
                return true;
            }
            if (finished[i] > -1 || dead[i]) {
                count++;
            }
        }
        return count >= 2;
    }

    std::vector<int> getRankings() override {
        if (EARLY_RACE_END) {
            std::vector<std::pair<int, double>> scoreByPlayer; // TreeMap
            for (int i = 0; i < (int)positions.size(); ++i) scoreByPlayer.push_back({i, (double)(dead[i] ? -1 : positions[i])});
            return createRankings(scoreByPlayer);
        }
        std::vector<int> rankings(PLAYER_COUNT, 0);
        for (int i = 0; i < (int)finished.size(); ++i) rankings[i] = finished[i] == -1 ? rank : finished[i];
        return rankings;
    }

    std::string getName() override { return "Hurdle Race"; }
};

struct Archery : MiniGame {
    std::vector<std::vector<int>> cursors; // List<int[2]>
    std::vector<int> wind;                 // LinkedList<Integer>
    bool arrows = false;
    std::vector<bool> dead;

    Archery() {
        for (int i = 0; i < PLAYER_COUNT; ++i) cursors.push_back(std::vector<int>(2, 0));
        dead.assign(PLAYER_COUNT, false);
        type = "archery";
    }

    std::vector<int>& cursor(int i) {
        if (i >= (int)cursors.size()) throw indexOutOfBounds(i, (int)cursors.size()); // List.get
        return cursors[i];
    }

    int randomChoice(java::SecureRandom& random, const std::vector<double>& coeffs) {
        double rand = random.nextDouble();
        double total = java::sum(coeffs);
        double b = 1 / total;
        std::vector<double> weights;
        for (double v : coeffs) weights.push_back(v * b);
        double cur = 0;
        for (int i = 0; i < (int)weights.size(); ++i) {
            cur += weights[i];
            if (cur >= rand) {
                return i;
            }
        }
        return 0;
    }

    void reset(java::SecureRandom& random) override {
        int x = 5 + random.nextInt(5);
        x *= random.nextBoolean() ? 1 : -1;
        int y = 5 + random.nextInt(5);
        y *= random.nextBoolean() ? 1 : -1;
        for (auto& c : cursors) {
            c[0] = x;
            c[1] = y;
        }
        wind.clear();
        int rounds = 12 + random.nextInt(4);
        static const std::vector<double> weights = {0, 2, 2, 2, 0.5, 0.5, 0.25, 0.25, 0.25, 0.2};
        for (int i = 0; i < rounds; ++i) wind.push_back(randomChoice(random, weights));
        arrows = false;
    }

    std::string getGPU() override {
        std::string r;
        for (int w : wind) r += std::to_string(w);
        return r;
    }

    std::vector<int> getRegisters() override { return fillRegisters({&cursor(0), &cursor(1), &cursor(2)}); }

    void tick(const std::vector<int>& actions) override {
        for (int i = 0; i < (int)actions.size(); ++i) {
            int a = actions[i];
            if (a == NO_ACTION) {
                dead[i] = true;
                continue;
            }
            int offset = wind.at(0);
            int dx = 0;
            int dy = 0;
            if (a == DOWN) {
                dy = offset;
            } else if (a == LEFT) {
                dx = -offset;
            } else if (a == RIGHT) {
                dx = offset;
            } else {
                dy = -offset;
            }
            std::vector<int>& c = cursor(i);
            c[0] += dx;
            c[1] += dy;
            int maxDist = 20;
            if (c[0] > maxDist) c[0] = maxDist;
            if (c[1] > maxDist) c[1] = maxDist;
            if (c[0] < -maxDist) c[0] = -maxDist;
            if (c[1] < -maxDist) c[1] = -maxDist;
        }
        wind.erase(wind.begin()); // wind.remove(0)
        arrows = isGameOver();
    }

    bool isGameOver() override { return wind.empty(); }

    std::vector<int> getRankings() override {
        std::vector<std::pair<int, double>> pointsByPlayer; // TreeMap
        for (int i = 0; i < (int)cursors.size(); ++i) {
            std::vector<int>& c = cursors[i];
            // Math.pow(int, 2) is exact for these small values
            double distance = (double)c[0] * (double)c[0] + (double)c[1] * (double)c[1];
            if (dead[i]) pointsByPlayer.push_back({i, -DBL_MAX});
            else pointsByPlayer.push_back({i, -distance});
        }
        return createRankings(pointsByPlayer);
    }

    std::string getName() override { return "Archery"; }

    void checkViewSerialization() override {
        cursor(0);
        cursor(1);
        cursor(2);
    }
};

struct RollerSpeedSkating : MiniGame {
    java::SecureRandom* random = nullptr;
    std::vector<int> positions, risk;
    std::vector<bool> dead;
    std::vector<int> directions; // Arrays.asList(Action.values()), shuffled in place
    int length = 0, timer = 0;

    RollerSpeedSkating() : positions(PLAYER_COUNT, 0), risk(PLAYER_COUNT, 0), dead(PLAYER_COUNT, false) {
        directions = {UP, DOWN, LEFT, RIGHT};
        type = "skating";
    }

    void reset(java::SecureRandom& rnd) override {
        random = &rnd;
        for (int i = 0; i < (int)positions.size(); ++i) {
            positions[i] = 0;
            risk[i] = 0;
        }
        java::shuffle(directions, rnd);
        length = 10;
        timer = 15;
    }

    std::string getGPU() override {
        std::string r;
        for (int a : directions) r += ACTION_NAMES[a][0];
        return r;
    }

    std::vector<int> getRegisters() override {
        std::vector<int> registers = fillRegisters({&positions, &risk});
        registers[6] = timer;
        return registers;
    }

    void tick(const std::vector<int>& actions) override {
        for (int i = 0; i < (int)actions.size(); ++i) {
            int action = actions[i];
            if (action == NO_ACTION) {
                dead[i] = true;
                continue;
            }
            if (risk[i] < 0) {
                risk[i]++;
                continue;
            }
            int idx = (int)(std::find(directions.begin(), directions.end(), action) - directions.begin());
            int dx = idx == 0 ? 1 : idx == 3 ? 3 : 2;
            positions[i] = positions[i] + dx;
            int riskValue = -1 + idx;
            risk[i] = std::max(0, risk[i] + riskValue);
        }
        for (int i = 0; i < (int)positions.size(); ++i) {
            if (risk[i] < 0) {
                continue;
            }
            bool clash = false;
            for (int k = 0; k < (int)positions.size(); ++k) {
                if (k == i) {
                    continue;
                }
                if (positions[k] % length == positions[i] % length) {
                    clash = true;
                    break;
                }
            }
            if (clash) {
                risk[i] += 2;
            }
            if (risk[i] >= 5) {
                risk[i] = -2; // stun
            }
        }
        java::shuffle(directions, *random);
        timer--;
    }

    bool isGameOver() override { return timer <= 0; }

    std::vector<int> getRankings() override {
        std::vector<std::pair<int, double>> scoreByPlayer; // TreeMap
        for (int i = 0; i < (int)positions.size(); ++i) scoreByPlayer.push_back({i, (double)(dead[i] ? -1 : positions[i])});
        return createRankings(scoreByPlayer);
    }

    std::string getName() override { return "Roller Speed Skating"; }
};

struct Diving : MiniGame {
    std::string goal;                          // LinkedList<Character>
    std::vector<std::string> playerInputs;     // HashMap<Integer, List<Character>> (view only)
    int turnsRemaining = 0;
    std::vector<int> points, combo;
    std::vector<bool> dead;

    Diving() : points(PLAYER_COUNT, 0), combo(PLAYER_COUNT, 0), dead(PLAYER_COUNT, false) { type = "diving"; }

    void reset(java::SecureRandom& random) override {
        goal.clear();
        playerInputs.clear();
        int length = 12 + random.nextInt(4);
        for (int i = 0; i < length; ++i) {
            char c = ACTION_NAMES[random.nextInt(4)][0];
            goal.push_back(c);
        }
        for (int i = 0; i < (int)points.size(); ++i) {
            points[i] = 0;
            combo[i] = 0;
            playerInputs.push_back("");
        }
        turnsRemaining = length + 1;
    }

    std::string getGPU() override { return goal; }

    std::vector<int> getRegisters() override { return fillRegisters({&points, &combo}); }

    void tick(const std::vector<int>& actions) override {
        for (int i = 0; i < (int)actions.size(); ++i) {
            int action = actions[i];
            if (action == NO_ACTION) {
                dead[i] = true;
                continue;
            }
            if (goal.at(0) == ACTION_NAMES[action][0]) {
                combo[i]++;
                points[i] += combo[i];
            } else {
                combo[i] = 0;
            }
            playerInputs[i] += ACTION_NAMES[action][0];
        }
        turnsRemaining = (int)goal.size();
        goal.erase(0, 1); // goal.remove(0)
    }

    bool isGameOver() override { return goal.empty(); }

    std::vector<int> getRankings() override {
        std::vector<std::pair<int, double>> pointsByPlayer; // HashMap<Integer, Double>, keys 0..n-1
        for (int i = 0; i < (int)points.size(); ++i) pointsByPlayer.push_back({i, (double)(dead[i] ? -1 : points[i])});
        return createRankings(pointsByPlayer);
    }

    std::string getName() override { return "Diving"; }

    void checkViewSerialization() override {
        // serialize(playerInputs.get(0..2)): a missing key gives null and a NullPointerException
        for (int i = 0; i < 3; i++)
            if (i >= (int)playerInputs.size())
                throw java::Exception{"java.lang.NullPointerException: Cannot invoke \"java.util.List.stream()\" because \"list\" is null"};
    }
};

// ----------------------------------------------------------------------------
// GameManager (core) and GameRunner (runner): the referee/runner text protocol
// ----------------------------------------------------------------------------
// The jar runs the referee in its own thread; it talks to the runner through pipes:
//   runner -> referee: "[[GET_GAME_INFO] 0]", "[[SET_PLAYER_OUTPUT] n]" + n lines,
//                      "[[SET_PLAYER_TIMEOUT] 0]", read by the referee with Scanner.nextLine()
//   referee -> runner: "[[VIEW] n]", "[[INFOS] n]", "[[SUMMARY] n]", "[[TOOLTIP] n]",
//                      "[[NEXT_PLAYER_INPUT] n]", "[[NEXT_PLAYER_INFO] n]", "[[UINPUT] n]",
//                      "[[METADATA] n]", "[[SCORES] n]", "[[FAIL] n]" + n lines
// Here the referee code runs normally and calls the runner synchronously for every command it
// prints; the runner does the player I/O as soon as it has a complete turn and appends its answer
// to the referee's input. The order of the lines on both sides is the same as in the jar.
enum OutCmd { VIEW, INFOS, NEXT_PLAYER_INPUT, NEXT_PLAYER_INFO, SCORES, UINPUT, TOOLTIP, SUMMARY, METADATA, FAIL, OUTCMD_COUNT };

struct RunnerStopped {}; // the runner left its loop: nothing else is observable

struct ChildProc;
static std::vector<ChildProc*> agents;
static FILE* trace = nullptr;

// --- GameManager state
static int maxTurns = 200;
static int turnMaxTime = 50;
static int firstTurnMaxTime = 1000;
static int turn = 0;
static bool gameEnd = false;
static bool newTurn = false;
static bool initDone = false;
static std::vector<String> currentGameSummary, prevGameSummary;
static bool prevGameSummaryNull = true;
static std::vector<Tooltip> currentTooltips, prevTooltips;
static int totalGameSummaryBytes = 0;
static int totalTurnTime = 0;
// lines not read yet by the referee's Scanner
static struct LineQueue {
    std::vector<String> lines;
    size_t head = 0;
    bool empty() const { return head == lines.size(); }
    void push_back(const String& l) { lines.push_back(l); }
    String pop_front() { return lines[head++]; }
    void clear() { lines.clear(), head = 0; }
    std::vector<String>::const_iterator begin() const { return lines.begin() + (long)head; }
    std::vector<String>::const_iterator end() const { return lines.end(); }
} refereeInput;
static i64 seedParam = 0;

// --- GameRunner state
static int runnerRound = 0;
static bool runnerStopped = false;
static bool runnerHasScores = false;
static std::vector<long long> runnerScores; // gameResult.scores (Integer.decode)
static std::vector<bool> runnerScoreSet;
static bool turnInfoHas[OUTCMD_COUNT];
static String turnInfo[OUTCMD_COUNT];
static std::string failCause;

// referee <- runner: the Scanner splits on "\r\n", '\n', '\r', U+2028, U+2029 and U+0085
static void sendToReferee(const String& text) {
    String cur;
    for (size_t i = 0; i < text.size(); i++) {
        char16_t c = text[i];
        if (c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029 || c == 0x0085) {
            if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') i++;
            refereeInput.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) refereeInput.push_back(cur); // never happens: the runner ends every command with '\n'
}

static void runnerFail(const std::string& cause) {
    failCause = cause;
    runnerStopped = true;
    throw RunnerStopped{};
}

static void runnerBeginRound() {
    for (int i = 0; i < OUTCMD_COUNT; i++) {
        turnInfoHas[i] = false;
        turnInfo[i].clear();
    }
    sendToReferee(java::str("[[GET_GAME_INFO] 0]\n"));
}

static bool isCompleteNormalTurn() {
    return turnInfoHas[NEXT_PLAYER_INPUT] && turnInfoHas[VIEW] && turnInfoHas[NEXT_PLAYER_INFO] && turnInfoHas[INFOS];
}
static bool isCompleteEndTurn() { return turnInfoHas[SCORES] && turnInfoHas[VIEW] && turnInfoHas[INFOS]; }

static void runnerPlayerTurn();

// GameRunner.readCommand + the loops of readGameInfo/runAgents, for one command printed by the referee
static void runnerReceive(OutCmd cmd, const String& content, int lineCount) {
    if (runnerStopped) throw RunnerStopped{};
    // RefereeAgent.getOutput: the lines and their terminators must fit in the buffer
    size_t maxBufferSize = runnerRound == 0 ? C::REFEREE_MAX_BUFFER_SIZE_EXTRA : C::REFEREE_MAX_BUFFER_SIZE;
    String received;
    if (lineCount > 0) {
        size_t bytes = java::encodeUtf8(content).size() + 1;
        if (bytes > maxBufferSize) runnerFail("java.lang.RuntimeException: Error reading Referee command (buffer capacity)");
        received = content + u'\n';
    }
    turnInfoHas[cmd] = true;
    turnInfo[cmd] = received;
    if (cmd == FAIL) failCause = java::encodeUtf8(received);
    if (!(isCompleteNormalTurn() || isCompleteEndTurn()) && cmd != FAIL) return; // keep reading

    // runAgents loop body
    bool validTurn = isCompleteNormalTurn() || isCompleteEndTurn();
    if (validTurn && !turnInfoHas[SCORES]) runnerPlayerTurn();
    if (validTurn && turnInfoHas[SCORES]) {
        for (const String& line : java::splitLines(turnInfo[SCORES], u"\n")) {
            std::vector<String> parts;
            String cur;
            for (char16_t c : line) {
                if (c == u' ') {
                    parts.push_back(cur);
                    cur.clear();
                } else cur += c;
            }
            parts.push_back(cur);
            while (!parts.empty() && parts.back().empty()) parts.pop_back();
            if (parts.size() > 1) {
                i64 p, s;
                if (java::parseLong(parts[0], INT32_MIN, INT32_MAX, p) && java::parseLong(parts[1], INT32_MIN, INT32_MAX, s) && p >= 0 &&
                    p < (i64)runnerScores.size()) {
                    runnerScores[p] = s;
                    runnerScoreSet[p] = true;
                }
            }
        }
        runnerHasScores = true;
    }
    runnerRound++;
    if (!validTurn || isCompleteEndTurn()) {
        runnerStopped = true;
        throw RunnerStopped{};
    }
    runnerBeginRound();
}

// --- referee side helpers (GameManager)
static void printCommand(OutCmd cmd, const std::vector<String>& lines) {
    // OutputData.toString: lines joined with '\n', trimmed; the line count splits on \r\n|\r|\n
    String sb;
    for (const String& l : lines) {
        sb += l;
        sb += u'\n';
    }
    String content = java::trim(sb);
    int length = content.empty() ? 0 : (int)java::splitLines(content, u"\r\n").size();
    runnerReceive(cmd, content, length);
}

// InputCommand.parse: \[\[(?<cmd>.+)\] ?(?<lineCount>[0-9]+)\] (matches()), then
// Command.valueOf and Integer.parseInt
enum InCmd { INIT, GET_GAME_INFO, SET_PLAYER_OUTPUT, SET_PLAYER_TIMEOUT };
static const char* const IN_CMD_NAMES[4] = {"INIT", "GET_GAME_INFO", "SET_PLAYER_OUTPUT", "SET_PLAYER_TIMEOUT"};

static InCmd parseInputCommand(const String& line, int& lineCount) {
    const java::Exception error{"java.lang.RuntimeException: Error in data sent to referee"};
    size_t n = line.size();
    if (n < 6 || line[0] != u'[' || line[1] != u'[' || line[n - 1] != u']') throw error;
    size_t d = n - 1; // digits are line[d0 .. n-2]
    while (d > 0 && line[d - 1] >= u'0' && line[d - 1] <= u'9') d--;
    if (d == n - 1) throw error;
    size_t close = d - 1; // ']' or ' '
    if (line[close] == u' ') {
        if (close == 0 || line[close - 1] != u']') throw error;
        close--;
    } else if (line[close] != u']') {
        throw error;
    }
    if (close < 3) throw error; // .+ needs one char
    String cmd = line.substr(2, close - 2);
    String digits = line.substr(d, n - 1 - d);
    int found = -1;
    for (int i = 0; i < 4; i++)
        if (cmd == java::str(IN_CMD_NAMES[i])) found = i;
    if (found < 0)
        throw java::Exception{"java.lang.IllegalArgumentException: No enum constant com.codingame.gameengine.core.InputCommand.Command." +
                              java::encodeUtf8(cmd)};
    i64 v;
    if (!java::parseLong(digits, INT32_MIN, INT32_MAX, v))
        throw java::Exception{"java.lang.NumberFormatException: For input string: \"" + java::encodeUtf8(digits) + "\""};
    lineCount = (int)v;
    return (InCmd)found;
}

// Scanner.nextLine() of the referee
static String nextLine() {
    if (refereeInput.empty()) {
        // The referee waits for the runner while the runner waits for a referee command: after
        // 150 s the runner gives up ("Invalid Referee command") and the game fails.
        runnerFail("java.lang.RuntimeException: Invalid Referee command: ");
    }
    return refereeInput.pop_front();
}

static void dumpFail(const java::Exception& e) {
    fprintf(stderr, "Exception in thread \"Thread-0\" %s\n", e.text.c_str());
    printCommand(FAIL, {java::str(e.text)});
}

static void dumpView() {
    // KEY_FRAME/INTERMEDIATE_FRAME + view data: a few KB at most, far from the 30000-byte command
    // buffer and the 1 MB total quota, and never seen by the players
    printCommand(VIEW, {java::str(newTurn ? "KEY_FRAME" : "INTERMEDIATE_FRAME")});
}

static void dumpInfos() {
    printCommand(INFOS, {});
    if (newTurn && !prevGameSummaryNull) printCommand(SUMMARY, prevGameSummary);
    if (newTurn && !prevTooltips.empty()) {
        std::vector<String> data;
        for (const Tooltip& t : prevTooltips) {
            data.push_back(t.message);
            data.push_back(java::str(t.player));
        }
        printCommand(TOOLTIP, data);
    }
}

static void addTooltip(int player, const String& message) { currentTooltips.push_back({player, message}); }

// GameManager.addToGameSummary
static void addToGameSummary(const String& summary) {
    int total = 0;
    for (const String& s : currentGameSummary) total += (int)s.size();
    if (total < C::GAME_SUMMARY_PER_TURN_HARD_QUOTA && total + totalGameSummaryBytes < C::GAME_SUMMARY_TOTAL_HARD_QUOTA) {
        currentGameSummary.push_back(summary);
        totalGameSummaryBytes += total;
    }
}

static void swapInfoAndViewData() {
    prevGameSummary = currentGameSummary;
    prevGameSummaryNull = false;
    currentGameSummary.clear();
    prevTooltips = currentTooltips;
    currentTooltips.clear();
}

// GameManager.execute(player, nbrOutputLines)
static void execute(Player& player, int nbrOutputLines) {
    try {
        if (!initDone) throw java::Exception{"java.lang.RuntimeException: Impossible to execute a player during init phase."};
        player.timeout = false;
        int lineCount;
        InCmd iCmd = parseInputCommand(nextLine(), lineCount);
        if (iCmd != GET_GAME_INFO) throw java::Exception{std::string("java.lang.RuntimeException: Invalid command: ") + IN_CMD_NAMES[iCmd]};
        dumpView();
        dumpInfos();
        printCommand(NEXT_PLAYER_INPUT, player.inputs);
        if (nbrOutputLines > 0) {
            totalTurnTime += turnMaxTime;
            if (totalTurnTime > C::GAME_DURATION_HARD_QUOTA)
                throw java::Exception{"java.lang.RuntimeException: Total game duration too long (>30000ms)"};
        }
        printCommand(NEXT_PLAYER_INFO, {java::str(player.index), java::str(nbrOutputLines),
                                        java::str(player.hasNeverBeenExecuted ? firstTurnMaxTime : turnMaxTime)});
        iCmd = parseInputCommand(nextLine(), lineCount);
        if (iCmd == SET_PLAYER_OUTPUT) {
            std::vector<String> output;
            for (int i = 0; i < lineCount; i++) output.push_back(nextLine());
            player.outputs = output;
        } else if (iCmd == SET_PLAYER_TIMEOUT) {
            player.timeout = true;
        } else {
            throw java::Exception{std::string("java.lang.RuntimeException: Invalid command: ") + IN_CMD_NAMES[iCmd]};
        }
        player.inputs.clear();
        newTurn = false;
    } catch (const java::Exception& e) {
        dumpFail(e);
        throw;
    }
}

// AbstractPlayer.execute
static void playerExecute(Player& p) {
    execute(p, p.getExpectedOutputLines());
    p.hasBeenExecuted = true;
    p.hasNeverBeenExecuted = false;
}

// ----------------------------------------------------------------------------
// Child process I/O (Windows)
// ----------------------------------------------------------------------------
#ifdef _WIN32
struct ChildProc {
    HANDLE hProcess = nullptr;
    HANDLE hStdinW = nullptr;    // write to child's stdin
    HANDLE hStdoutR = nullptr;   // read child's stdout (overlapped)
    HANDLE hReadEvent = nullptr; // manual-reset event for overlapped reads on hStdoutR
    bool inAlive = false;        // we can still write its stdin
    bool outAlive = false;       // its stdout is not at EOF
    std::string readBuf;         // bytes read but not consumed yet
    size_t readPos = 0;

    bool start(const std::string& cmd, HANDLE stderrTarget) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;
        HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
        // stdin: plain synchronous pipe. The jar writes the inputs from another thread, so a bot
        // that stops reading never blocks it: a 1 MB buffer holds far more than a whole game of
        // inputs, so our writes never block either.
        if (!CreatePipe(&inR, &inW, &sa, 1 << 20)) return false;
        // stdout must be overlapped on our (read) side so reads can wait with a precise
        // millisecond timeout. CreatePipe doesn't support FILE_FLAG_OVERLAPPED, so build the pair
        // manually with a unique-named pipe.
        char pipeName[96];
        static volatile LONG s_pipeCounter = 0;
        LONG pipeId = InterlockedIncrement(&s_pipeCounter);
        std::snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\battle_referee_%lu_%ld", (unsigned long)GetCurrentProcessId(), (long)pipeId);
        outR = CreateNamedPipeA(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, &sa);
        if (outR == INVALID_HANDLE_VALUE) {
            CloseHandle(inR);
            CloseHandle(inW);
            return false;
        }
        outW = CreateFileA(pipeName, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outW == INVALID_HANDLE_VALUE) {
            CloseHandle(inR);
            CloseHandle(inW);
            CloseHandle(outR);
            return false;
        }
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inR;
        si.hStdOutput = outW;
        si.hStdError = stderrTarget ? stderrTarget : GetStdHandle(STD_ERROR_HANDLE);
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
        if (!ok) {
            CloseHandle(inW);
            CloseHandle(outR);
            return false;
        }
        CloseHandle(pi.hThread);
        hReadEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!hReadEvent) {
            CloseHandle(inW);
            CloseHandle(outR);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            return false;
        }
        hProcess = pi.hProcess;
        hStdinW = inW;
        hStdoutR = outR;
        inAlive = true;
        outAlive = true;
        return true;
    }

    void writeAll(const std::string& s) {
        if (!inAlive) return;
        size_t off = 0;
        DWORD wr;
        while (off < s.size()) {
            if (!WriteFile(hStdinW, s.data() + off, (DWORD)(s.size() - off), &wr, nullptr) || wr == 0) {
                inAlive = false; // like the jar's AsynchronousWriter: stop writing, keep reading
                return;
            }
            off += wr;
        }
    }

    // Reads more bytes into readBuf. Returns false on timeout, EOF or error.
    bool fill(std::chrono::steady_clock::time_point deadline) {
        auto now = std::chrono::steady_clock::now();
        if (!outAlive || now >= deadline) return false;
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
                outAlive = false;
                return false;
            }
            DWORD wr = WaitForSingleObject(hReadEvent, ms);
            if (wr == WAIT_TIMEOUT) {
                // Time is up: cancel the pending read. Bytes it got before the cancellation stay
                // buffered for a later read, as they would in the jar's pipe.
                CancelIoEx(hStdoutR, &ov);
                DWORD got = 0;
                if (GetOverlappedResult(hStdoutR, &ov, &got, TRUE) && got > 0) readBuf.append(tmp, got);
                return false;
            }
            if (wr != WAIT_OBJECT_0 || !GetOverlappedResult(hStdoutR, &ov, &rd, FALSE)) {
                outAlive = false;
                return false;
            }
        }
        if (rd == 0) {
            outAlive = false;
            return false;
        }
        readBuf.append(tmp, rd);
        return true;
    }

    void kill() {
        if (hStdinW) CloseHandle(hStdinW);
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
            CloseHandle(hProcess);
        }
        if (hStdoutR) CloseHandle(hStdoutR);
        if (hReadEvent) CloseHandle(hReadEvent);
        hProcess = nullptr;
        hStdinW = nullptr;
        hStdoutR = nullptr;
        hReadEvent = nullptr;
        inAlive = outAlive = false;
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
struct ChildProc {
    pid_t pid = -1;
    int fdStdinW = -1;
    int fdStdoutR = -1;
    bool inAlive = false;
    bool outAlive = false;
    std::string pendingIn; // input not accepted by the pipe yet (the jar writes from another thread)
    std::string readBuf;   // bytes read but not consumed yet
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
        fdStdinW = inPipe[1];
        fdStdoutR = outPipe[0];
        fcntl(fdStdinW, F_SETFL, fcntl(fdStdinW, F_GETFL) | O_NONBLOCK);
        pid = c;
        inAlive = true;
        outAlive = true;
        return true;
    }

    void flushInput() {
        while (inAlive && !pendingIn.empty()) {
            ssize_t n = write(fdStdinW, pendingIn.data(), pendingIn.size());
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (n <= 0) {
                inAlive = false;
                pendingIn.clear();
                return;
            }
            pendingIn.erase(0, (size_t)n);
        }
    }

    void writeAll(const std::string& s) {
        if (!inAlive) return;
        pendingIn += s;
        flushInput();
    }

    // Reads more bytes into readBuf. Returns false on timeout, EOF or error.
    bool fill(std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (!outAlive || now >= deadline) return false;
            int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (ms < 1) ms = 1;
            struct pollfd pfd[2];
            pfd[0].fd = fdStdoutR;
            pfd[0].events = POLLIN;
            pfd[0].revents = 0;
            int nfds = 1;
            if (inAlive && !pendingIn.empty()) {
                pfd[1].fd = fdStdinW;
                pfd[1].events = POLLOUT;
                pfd[1].revents = 0;
                nfds = 2;
            }
            int pr = poll(pfd, nfds, ms);
            if (pr < 0 && errno == EINTR) continue;
            if (pr == 0) return false;
            if (pr < 0) {
                outAlive = false;
                return false;
            }
            if (nfds == 2 && pfd[1].revents) flushInput();
            if (!pfd[0].revents) continue;
            char tmp[4096];
            ssize_t rd;
            do {
                rd = read(fdStdoutR, tmp, sizeof(tmp));
            } while (rd < 0 && errno == EINTR);
            if (rd <= 0) {
                outAlive = false;
                return false;
            }
            readBuf.append(tmp, (size_t)rd);
            return true;
        }
    }

    void kill() {
        if (fdStdinW >= 0) {
            close(fdStdinW);
            fdStdinW = -1;
        }
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
        inAlive = outAlive = false;
    }
#endif

    bool lastCR = false;

    // Agent#getOutput: reads until `nbLine` line terminators ('\n', '\r', "\r\n" counts once) or
    // AGENT_MAX_BUFFER_SIZE bytes, within `timeoutMs`. Extra bytes are kept for the next turn.
    std::string getOutput(int nbLine, int timeoutMs) {
        std::string out;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        int nbOccurences = 0;
        while ((int)out.size() < C::AGENT_MAX_BUFFER_SIZE && nbOccurences < nbLine) {
            if (readPos >= readBuf.size()) {
                readBuf.clear();
                readPos = 0;
                if (!fill(deadline)) break;
                continue;
            }
            char curByte = readBuf[readPos++];
            if (!(curByte == '\n' && lastCR)) {
                out += curByte;
                if (curByte == '\n' || curByte == '\r') ++nbOccurences;
            }
            lastCR = curByte == '\r';
        }
        return out;
    }
};

// GameRunner.getNextPlayerOutput + sendPlayerOutput/sendTimeOut, with the turn info just read
static void runnerPlayerTurn() {
    std::vector<String> info = java::splitLines(turnInfo[NEXT_PLAYER_INFO], u"\n");
    i64 nextPlayer = 0, nbLines = 0, timeout = 0;
    java::parseLong(info.at(0), INT32_MIN, INT32_MAX, nextPlayer);
    java::parseLong(info.at(1), INT32_MIN, INT32_MAX, nbLines);
    java::parseLong(info.at(2), INT64_MIN, INT64_MAX, timeout);
    ChildProc* agent = agents.at(nextPlayer);
    agent->writeAll(java::encodeUtf8(turnInfo[NEXT_PLAYER_INPUT]));
    String output = java::decodeUtf8(agent->getOutput((int)nbLines, (int)timeout));
    for (char16_t& c : output)
        if (c == u'\r') c = u'\n';
    // checkOutput
    bool ok;
    if (output.empty()) {
        ok = nbLines <= 0;
    } else {
        i64 nb = std::count(output.begin(), output.end(), u'\n');
        ok = nb == nbLines;
    }
    if (!ok) {
        // (the jar sleeps for the timeout here to let the agent crash cleanly)
        if (trace) fprintf(trace, "T%d P%d TIMEOUT\n", turn, (int)nextPlayer);
        sendToReferee(java::str("[[SET_PLAYER_TIMEOUT] 0]\n"));
        return;
    }
    if (output.empty() && nbLines == 1) output = u"\n";
    if (!output.empty() && output.back() != u'\n') output += u'\n';
    // sendPlayerOutput: output.split("(\\n|\\r\\n)", -1), first nbLines pieces ('\r' is gone)
    std::vector<String> pieces;
    String cur;
    for (char16_t c : output) {
        if (c == u'\n') {
            pieces.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    pieces.push_back(cur);
    String command = java::str("[[SET_PLAYER_OUTPUT] " + std::to_string(nbLines) + "]\n");
    if (trace) fprintf(trace, "T%d P%d OUT [", turn, (int)nextPlayer);
    for (i64 i = 0; i < nbLines; i++) {
        const String piece = i < (i64)pieces.size() ? pieces[i] : java::str("null");
        command += piece;
        command += u'\n';
        if (trace) fprintf(trace, "%s%s", i ? "," : "", java::json(piece).c_str());
    }
    if (trace) fprintf(trace, "]\n");
    sendToReferee(command);
}

// ----------------------------------------------------------------------------
// Game.java, Serializer.java, CommandManager.java, Referee.java
// ----------------------------------------------------------------------------
struct Game {
    std::vector<MiniGame*> minigames;
    java::SecureRandom random;
    int resets[4] = {0, 0, 0, 0};

    void init() {
        PLAYER_COUNT = (int)players.size();
        random.setSeed(seedParam);
        int leagueLevel = C::LEAGUE_LEVEL;
        minigames.push_back(new HurdleRace());
        if (leagueLevel == 2) {
            minigames.push_back(new HurdleRace());
            minigames.push_back(new HurdleRace());
            minigames.push_back(new HurdleRace());
        } else if (leagueLevel > 2) {
            minigames.push_back(new Archery());
            minigames.push_back(new RollerSpeedSkating());
            minigames.push_back(new Diving());
        }
        for (MiniGame* minigame : minigames) minigame->reset(random);
        for (Player& p : players) p.init((int)minigames.size());
    }

    void resetGameTurnData() {
        for (Player& p : players) p.reset();
    }

    static const char* getMedalName(int rank) {
        if (rank == 0) return "GOLD medal";
        if (rank == 1) return "SILVER medal";
        if (rank == 2) return "BRONZE medal";
        return "pat on the back";
    }

    std::vector<int> getRankings(MiniGame* minigame) {
        std::vector<int> rankings = minigame->getRankings();
        for (int i = 0; i < (int)rankings.size(); ++i) {
            if (!players.at(i).active) rankings[i] = 2;
        }
        return rankings;
    }

    void performGameUpdate(int) {
        for (int gameIdx = 0; gameIdx < (int)minigames.size(); gameIdx++) {
            MiniGame* minigame = minigames[gameIdx];
            if (minigame->shouldReset) {
                minigame->resetting = true;
                minigame->reset(random);
                resets[gameIdx]++;
                minigame->shouldReset = false;
            } else {
                minigame->resetting = false;
                std::vector<int> actions;
                for (Player& p : players) actions.push_back(p.action);
                minigame->tick(actions);
                if (minigame->isGameOver()) {
                    minigame->shouldReset = true;
                    std::vector<int> rankings = getRankings(minigame);
                    for (int i = 0; i < (int)rankings.size(); ++i) players.at(i).medals.at(gameIdx).at(rankings[i])++;
                    addToGameSummary(java::str("Game over: " + minigame->getName()));
                    for (int i = 0; i < (int)rankings.size(); ++i)
                        addToGameSummary(java::str("$" + std::to_string(players.at(i).index) + " earns a " + getMedalName(rankings[i])));
                }
            }
        }
    }

    void onEnd() {
        for (Player& p : players) {
            if (!p.active) p.score = -1;
            else p.score = p.getPoints();
        }
        // EndScreenModule data and the gold_medals metadata: view only
    }
};

static Game game;

// Serializer.serializeFrameInfoFor
static std::vector<String> serializeFrameInfoFor(const Player&) {
    std::vector<String> lines;
    for (const Player& p : players) {
        std::string s = std::to_string(p.getPoints());
        for (size_t gameIdx = 0; gameIdx < game.minigames.size(); gameIdx++)
            for (int k = 0; k < 3; k++) s += " " + std::to_string(p.medals[gameIdx][k]);
        lines.push_back(java::str(s));
    }
    for (MiniGame* minigame : game.minigames) {
        std::vector<int> reg = minigame->getRegisters();
        std::string registers;
        for (size_t i = 0; i < reg.size(); i++) registers += (i ? " " : "") + std::to_string(reg[i]);
        std::string gpu = minigame->getGPU();
        lines.push_back(java::str((minigame->shouldReset ? std::string("GAME_OVER") : gpu) + " " + registers));
    }
    return lines;
}

// ViewModule.sendFrameData (Serializer.serializeFrameData): only its exceptions matter
static void viewSendFrameData() {
    for (MiniGame* minigame : game.minigames) {
        minigame->checkViewSerialization();
        if (minigame->shouldReset) game.getRankings(minigame);
    }
}

// CommandManager: ^(?<button>UP|DOWN|RIGHT|LEFT)(?:\s+(?<message>.+))?\s*$, CASE_INSENSITIVE
// (ASCII only), with matches(). The line holds no line terminator (the Scanner split on them),
// so it matches iff it starts with a button and the rest is empty or starts with [ \t\n\x0B\f\r].
static int matchPlayerPattern(const String& line) {
    static const char* const BUTTONS[4] = {"UP", "DOWN", "RIGHT", "LEFT"};
    static const int BUTTON_ACTIONS[4] = {UP, DOWN, RIGHT, LEFT};
    for (int b = 0; b < 4; b++) {
        size_t n = strlen(BUTTONS[b]);
        if (line.size() < n) continue;
        bool ok = true;
        for (size_t i = 0; i < n && ok; i++) {
            char16_t c = line[i];
            if (c >= u'a' && c <= u'z') c = (char16_t)(c - 32);
            ok = c == (char16_t)BUTTONS[b][i];
        }
        if (!ok) continue;
        if (line.size() == n) return BUTTON_ACTIONS[b];
        char16_t c = line[n];
        if (c == u' ' || c == u'\t' || c == u'\n' || c == 0x0B || c == u'\f' || c == u'\r') return BUTTON_ACTIONS[b];
        return NO_ACTION;
    }
    return NO_ACTION;
}

static void handleCommands(Player& player, const std::vector<String>& lines) {
    if (lines.empty()) throw indexOutOfBounds(0, 0); // lines.get(0)
    const String& line = lines[0];
    int button = matchPlayerPattern(line);
    if (button != NO_ACTION) {
        player.action = button; // Action.valueOf(button.toUpperCase())
        // the message (group "message") is stored but never used
        return;
    }
    String message = java::str("Invalid Input: Expected UP | DOWN | LEFT | RIGHT but got '") + line + u'\'';
    // deactivatePlayer: escapeHTMLEntities
    deactivate(player, java::replace(java::replace(message, u"&lt;", u"<"), u"&gt;", u">"));
    addToGameSummary(java::str("Bad command: ") + message);
}

// Referee.java
static void refereeInit() {
    game.init();
    // sendGlobalInfo
    for (Player* player : getActivePlayers()) {
        player->inputs.push_back(java::str(player->index));
        player->inputs.push_back(java::str((long long)game.minigames.size()));
    }
    maxTurns = 100;
    turnMaxTime = 50;
    firstTurnMaxTime = 1000;
}

static void refereeGameTurn(int t) {
    game.resetGameTurnData();
    for (Player* player : getActivePlayers()) {
        for (const String& line : serializeFrameInfoFor(*player)) player->inputs.push_back(line);
        playerExecute(*player);
    }
    // handlePlayerCommands
    for (Player* player : getActivePlayers()) {
        if (player->timeout) { // getOutputs() throws TimeoutException
            deactivate(*player, java::str("Timeout!"));
            addToGameSummary(java::str("$" + std::to_string(player->index) + " has not provided " +
                                       std::to_string(player->getExpectedOutputLines()) + " lines in time"));
        } else {
            handleCommands(*player, player->outputs);
        }
    }
    game.performGameUpdate(t);
    if (getActivePlayers().size() < 2) gameEnd = true; // abort()
}

// GameManager.start (after the INIT command), until the runner stops reading
static void gameManagerStart() {
    try {
        refereeInit();
        viewSendFrameData(); // ViewModule.onGameInit
        initDone = true;
        for (turn = 1; turn <= maxTurns && !gameEnd && !getActivePlayers().empty(); turn++) {
            swapInfoAndViewData();
            newTurn = true;
            refereeGameTurn(turn);
            viewSendFrameData(); // ViewModule.onAfterGameTurn
            bool anyExecuted = false;
            for (Player& p : players) anyExecuted = anyExecuted || p.hasBeenExecuted;
            if (!players.empty() && !anyExecuted) execute(players[0], 0);
            for (Player& p : players) {
                p.outputs.clear();
                p.hasBeenExecuted = false;
            }
        }
        if (trace) {
            fprintf(trace, "END turn=%d", turn);
            for (Player& p : players) fprintf(trace, " %d", p.active ? p.getPoints() : -1);
            fprintf(trace, "\n");
        }
        game.onEnd();
        swapInfoAndViewData();
        newTurn = true;
        dumpView();
        dumpInfos();
        printCommand(UINPUT, {java::str("seed=" + std::to_string(seedParam))});
        printCommand(METADATA, {java::str("{}")});
        std::vector<String> scores;
        for (Player& p : players) scores.push_back(java::str(std::to_string(p.index) + " " + std::to_string(p.score)));
        printCommand(SCORES, scores);
    } catch (const java::Exception& e) {
        dumpFail(e);
    }
}

// ----------------------------------------------------------------------------
// CommandLineInterface
// ----------------------------------------------------------------------------
static const char* HELP =
    "usage: -p1 <player1 command line> -p2 <player2 command line> [-p3 <cmd>\n"
    "           -p4 <cmd> -l <log file> -seed <seed>]\n"
    " -h            Print the help\n"
    " -l <arg>      File output for logs\n"
    " -p1 <arg>     Required. Player 1 command line\n"
    " -p2 <arg>     Required. Player 2 command line\n"
    " -p3 <arg>     Player 3 command line (if applies)\n"
    " -p4 <arg>     Player 4 command line (if applies)\n"
    " -seed <arg>   Referee seed\n";

int main(int argc, char** argv) {
    const char* names[7] = {"p1", "p2", "p3", "p4", "l", "seed", "h"};
    std::string values[7];
    bool has[7] = {false, false, false, false, false, false, false};
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a.size() < 2 || a[0] != '-') continue; // a plain argument: ignored
        int opt = -1;
        for (int k = 0; k < 7; k++)
            if (a.substr(1) == names[k]) opt = k;
        if (opt < 0) {
            fprintf(stderr, "org.apache.commons.cli.UnrecognizedOptionException: Unrecognized option: %s\n", a.c_str());
            return 1;
        }
        if (opt == 6) {
            has[opt] = true;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "org.apache.commons.cli.MissingArgumentException: Missing argument for option: %s\n", names[opt]);
            return 1;
        }
        if (!has[opt]) values[opt] = argv[i + 1]; // getOptionValue returns the first value
        has[opt] = true;
        i++;
    }
    if (has[6] || !has[0] || !has[1]) {
        fputs(HELP, stdout);
        return 0;
    }
    if (has[5]) {
        i64 s;
        if (!java::parseLong(java::str(values[5]), INT64_MIN, INT64_MAX, s)) {
            fprintf(stderr, "java.lang.NumberFormatException: For input string: \"%s\"\n", values[5].c_str());
            return 1;
        }
        seedParam = s;
    } else {
        seedParam = (i64)java::entropy(); // System.nanoTime() + new Object().hashCode()
    }
    std::vector<std::string> cmds;
    for (int k = 0; k < 4; k++)
        if (has[k]) cmds.push_back(values[k]);
    if (has[4]) trace = fopen(values[4].c_str(), "w");

    int n = (int)cmds.size();
    std::vector<ChildProc> procs(n);
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE devnull = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    for (int i = 0; i < n; i++) {
#ifdef _WIN32
        bool started = procs[i].start(cmds[i], devnull);
#else
        bool started = procs[i].start(cmds[i], nullptr);
#endif
        if (!started) {
            fprintf(stderr, "java.lang.RuntimeException: Failed to launch %s\n", cmds[i].c_str());
            for (int k = 0; k < i; k++) procs[k].kill();
            if (trace) fclose(trace);
            return 1;
        }
        agents.push_back(&procs[i]);
    }

    players.resize(n);
    for (int i = 0; i < n; i++) players[i].index = i;
    runnerScores.assign(n, 0);
    runnerScoreSet.assign(n, false);

    runnerBeginRound();
    try {
        gameManagerStart();
    } catch (const RunnerStopped&) {
    }

    for (auto& p : procs) p.kill();
#ifdef _WIN32
    if (devnull != INVALID_HANDLE_VALUE) CloseHandle(devnull);
#endif
    if (trace) {
        if (!runnerHasScores) fprintf(trace, "FAIL %s\n", failCause.c_str());
        fclose(trace);
    }
    // CommandLineInterface: result.scores.get(i) ("null" when the referee did not finish), then
    // the game parameters
    for (int i = 0; i < n; i++) {
        if (runnerScoreSet[i]) printf("%lld\n", (long long)runnerScores[i]);
        else printf("null\n");
    }
    if (runnerHasScores) printf("seed=%lld\n\n", (long long)seedParam);
    return 0;
}
