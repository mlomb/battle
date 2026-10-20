// Seabed Security (CodinGame Fall Challenge 2023) - headless C++ referee, port of
// https://github.com/CodinGame/FallChallenge2023-SeabedSecurity (commit d40cf97),
// CodinGame game engine 4.4.2, highest league (league.level 19: monsters, fleeing fish,
// 2 drones per player). Deterministic; reproduces the same game as the reference jar for a
// given -seed. 2 players only.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints "<p0score>\n<p1score>\n" to stdout (matches the jar). -l writes a trace: one line per
// player execution ("T<turn> P<player> OUT <json lines>" or "T<turn> P<player> TIMEOUT") and the
// viewer frame data of every turn ("V <json>", same text as the jar's game log).
//
// The Java referee is followed closely (same data structures, iteration orders and floating point
// operations in the same order). Notable Java behaviours reproduced here:
//   - SecureRandom("SHA1PRNG") seeded with the seed (gameManager.getRandom()), Random.nextInt
//     rejection loop, the int overflow of `random.nextInt() * 7` for the fish directions
//   - Math.round / (int) casts, Vector.epsilonRound, the collision code (FP contract off)
//   - drones that WAIT at the bottom keep their previous speed (updateDrones has no else branch)
//   - the light switch of a drone keeps its value when a turn's command is missing or invalid
//   - command regexes (ASCII case-insensitive, \d, \s, String.trim) and Integer.parseInt
//   - engine 4.4.2 I/O: Agent#getOutput ('\r' and '\n' terminate lines, "\r\n" counts once even
//     across turns, 10000 bytes max, 1000 ms for each player's first turn and 50 ms afterwards;
//     the jar's head start before the first input is approximated, see ChildProc::execute),
//     Java's UTF-8 decoder (U+FFFD), GameRunner's '\r' -> '\n' and line split; inputs are written
//     by a separate thread (AsynchronousWriter: an agent that never reads does not block us);
//     agent stderr goes to a pipe of the size Java uses (4096 + 24 bytes on Windows) drained by at
//     most 4096 bytes after each of the agent's turns (Agent#readError), so an agent writing more
//     than that per turn blocks on stderr and times out like with the jar
//   - the referee reads player lines with java.util.Scanner, which also ends lines at U+0085,
//     U+2028 and U+2029: such characters shift the referee/runner protocol by extra lines, which
//     is emulated with the referee's input queue (the Java referee usually dies on the next
//     header it reads; the runner then reports no scores: we print "null" like the jar's CLI)
//   - the runner reads referee commands into a 30000-byte buffer: an invalid command echoed in
//     the last frame's summary/tooltip can overflow it (invalid UTF-8 bytes become U+FFFD, 3 bytes
//     each), the jar then reports no scores ("null")
//   - CommandLineInterface: help text (no -p1/-p2 or -h), exit 1 on a bad -seed / option
// Not reproducible: -seed 0 (SecureRandom ignores setSeed(0) and seeds itself from the system,
// so the jar's map is random there). The HashMap iteration orders of the referee (Scan and
// FishType keys hash by identity) only change summary texts, which nobody can observe.

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
#include <climits>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <chrono>
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
typedef uint8_t u8;

// ----------------------------------------------------------------------------
// Java semantics
// ----------------------------------------------------------------------------
namespace java {

// (int) cast of a double
static inline i32 d2i(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (i32)d;
}

// (long) cast of a double
static inline i64 d2l(double d) {
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (i64)d;
}

// (int) cast of a long: keeps the low 32 bits
static inline i32 l2i(i64 v) { return (i32)(u32)(u64)v; }

// Math.round(double): floor(a + 1/2) computed exactly
static inline i64 round(double a) {
    if (a != a) return 0;
    double f = std::floor(a);
    if (std::fabs(a) >= 4503599627370496.0) return d2l(a); // already an integer
    double r = (a - f >= 0.5) ? f + 1.0 : f;               // a - f is exact here
    return d2l(r);
}

// SHA-1
struct SHA1 {
    static inline u32 rol(u32 v, int b) { return (v << b) | (v >> (32 - b)); }
    static void hash(const u8* msg, size_t n, u8 out[20]) {
        u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        size_t total = n + 1;
        while (total % 64 != 56) total++;
        total += 8;
        std::vector<u8> buf(total, 0);
        if (n) memcpy(buf.data(), msg, n);
        buf[n] = 0x80;
        u64 ml = (u64)n * 8;
        for (int i = 0; i < 8; i++) buf[total - 1 - i] = (u8)(ml >> (8 * i));
        for (size_t off = 0; off < total; off += 64) {
            u32 w[80];
            for (int i = 0; i < 16; i++)
                w[i] = ((u32)buf[off + i * 4] << 24) | ((u32)buf[off + i * 4 + 1] << 16) | ((u32)buf[off + i * 4 + 2] << 8) |
                       (u32)buf[off + i * 4 + 3];
            for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; i++) {
                u32 f, k;
                if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                u32 t = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = (u8)(h[i] >> (24 - 8 * j));
    }
};

// java.security.SecureRandom.getInstance("SHA1PRNG") + setSeed(long) (sun.security.provider.SecureRandom)
struct SHA1PRNG {
    u8 state[20];
    u8 remainder[20];
    int remCount = 0;

    void setSeed(i64 seed) {
        // SecureRandom.setSeed(long) ignores 0; the jar then seeds itself from the system
        u8 b[8];
        for (int i = 0; i < 8; i++) b[i] = (u8)((u64)seed >> (8 * i)); // longToByteArray: little endian
        SHA1::hash(b, 8, state);                                         // state = digest(seed)
        remCount = 0;
        memset(remainder, 0, sizeof(remainder));
    }
    static void updateState(u8* st, const u8* output) {
        int last = 1;
        bool zf = false;
        for (int i = 0; i < 20; i++) {
            int v = (int)(int8_t)st[i] + (int)(int8_t)output[i] + last;
            u8 t = (u8)v;
            zf = zf || (st[i] != t);
            st[i] = t;
            last = v >> 8;
        }
        if (!zf) st[0]++;
    }
    void nextBytes(u8* result, int len) {
        int index = 0;
        int r = remCount;
        if (r > 0) {
            int todo = std::min(len - index, 20 - r);
            for (int i = 0; i < todo; i++) {
                result[i] = remainder[r];
                remainder[r++] = 0;
            }
            remCount += todo;
            index += todo;
        }
        while (index < len) {
            u8 output[20];
            SHA1::hash(state, 20, output);
            updateState(state, output);
            int todo = std::min(len - index, 20);
            for (int i = 0; i < todo; i++) {
                result[index++] = output[i];
                output[i] = 0;
            }
            remCount += todo;
            memcpy(remainder, output, 20);
        }
        remCount %= 20;
    }
    // SecureRandom.next(numBits)
    i32 next(int numBits) {
        int numBytes = (numBits + 7) / 8;
        u8 b[4];
        nextBytes(b, numBytes);
        u32 nxt = 0;
        for (int i = 0; i < numBytes; i++) nxt = (nxt << 8) + b[i];
        return (i32)(nxt >> (numBytes * 8 - numBits));
    }
    i32 nextInt() { return next(32); }
    // Random.nextInt(bound)
    i32 nextInt(i32 bound) {
        i32 r = next(31);
        i32 m = bound - 1;
        if ((bound & m) == 0) return (i32)(((i64)bound * (i64)r) >> 31);
        for (i32 u = r; (i32)((u32)u - (u32)(r = u % bound) + (u32)m) < 0; u = next(31)) {
        }
        return r;
    }
};

typedef std::u16string ustr; // a java.lang.String (UTF-16 code units)

static ustr ascii(const std::string& s) { return ustr(s.begin(), s.end()); }

// new String(bytes, UTF_8) (String.decodeUTF8_UTF16 of JDK 26, malformed input -> U+FFFD)
static ustr decodeUTF8(const std::string& in) {
    const u8* src = (const u8*)in.data();
    int sl = (int)in.size(), sp = 0;
    ustr dst;
    const char16_t REPL = 0xFFFD;
    auto sb = [&](int i) -> int { return (int)(int8_t)src[i]; }; // signed byte
    auto notCont = [](int b) { return (b & 0xc0) != 0x80; };
    while (sp < sl) {
        int b1 = sb(sp++);
        if (b1 >= 0) {
            dst += (char16_t)b1;
        } else if ((b1 >> 5) == -2 && (b1 & 0x1e) != 0) {
            if (sp < sl) {
                int b2 = sb(sp++);
                if (notCont(b2)) {
                    dst += REPL;
                    sp--;
                } else {
                    dst += (char16_t)(((b1 << 6) ^ b2) ^ (((int)(int8_t)0xC0 << 6) ^ ((int)(int8_t)0x80 << 0)));
                }
                continue;
            }
            dst += REPL;
            break;
        } else if ((b1 >> 4) == -2) {
            if (sp + 1 < sl) {
                int b2 = sb(sp++);
                int b3 = sb(sp++);
                bool mal = (b1 == (int)(int8_t)0xe0 && (b2 & 0xe0) == 0x80) || (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80;
                if (mal) {
                    dst += REPL;
                    sp -= 3;
                    // malformed3
                    int m1 = sb(sp), m2 = sb(sp + 1);
                    sp += ((m1 == (int)(int8_t)0xe0 && (m2 & 0xe0) == 0x80) || notCont(m2)) ? 1 : 2;
                } else {
                    char16_t c = (char16_t)((b1 << 12) ^ (b2 << 6) ^
                                            (b3 ^ (((int)(int8_t)0xE0 << 12) ^ ((int)(int8_t)0x80 << 6) ^ ((int)(int8_t)0x80 << 0))));
                    if (c >= 0xD800 && c <= 0xDFFF) dst += REPL;
                    else dst += c;
                }
                continue;
            }
            if (sp < sl) {
                int b2 = sb(sp);
                if ((b1 == (int)(int8_t)0xe0 && (b2 & 0xe0) == 0x80) || (b2 & 0xc0) != 0x80) {
                    dst += REPL;
                    continue;
                }
            }
            dst += REPL;
            break;
        } else if ((b1 >> 3) == -2) {
            if (sp + 2 < sl) {
                int b2 = sb(sp++);
                int b3 = sb(sp++);
                int b4 = sb(sp++);
                int uc = ((b1 << 18) ^ (b2 << 12) ^ (b3 << 6) ^
                          (b4 ^ (((int)(int8_t)0xF0 << 18) ^ ((int)(int8_t)0x80 << 12) ^ ((int)(int8_t)0x80 << 6) ^
                                 ((int)(int8_t)0x80 << 0))));
                bool mal = (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80 || (b4 & 0xc0) != 0x80 || !(uc >= 0x10000 && uc <= 0x10FFFF);
                if (mal) {
                    dst += REPL;
                    sp -= 4;
                    // malformed4
                    int m1 = src[sp], m2 = src[sp + 1];
                    int n;
                    if (m1 > 0xf4 || (m1 == 0xf0 && (m2 < 0x90 || m2 > 0xbf)) || (m1 == 0xf4 && (m2 & 0xf0) != 0x80) || notCont(m2)) n = 1;
                    else if (notCont(sb(sp + 2))) n = 2;
                    else n = 3;
                    sp += n;
                } else {
                    dst += (char16_t)((((u32)uc - 0x10000) >> 10) + 0xD800);
                    dst += (char16_t)((((u32)uc - 0x10000) & 0x3FF) + 0xDC00);
                }
                continue;
            }
            b1 &= 0xff;
            if (b1 > 0xf4 || (sp < sl && [&] {
                    int b2 = src[sp];
                    return (b1 == 0xf0 && (b2 < 0x90 || b2 > 0xbf)) || (b1 == 0xf4 && (b2 & 0xf0) != 0x80) || (b2 & 0xc0) != 0x80;
                }())) {
                dst += REPL;
                continue;
            }
            sp++;
            dst += REPL;
            if (sp < sl && (sb(sp) & 0xc0) != 0x80) continue;
            break;
        } else {
            dst += REPL;
        }
    }
    return dst;
}

// length of String.getBytes(UTF_8) (lone surrogates are encoded as '?')
static size_t utf8Length(const ustr& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            n += 4;
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) n += 1;
        else n += 3;
    }
    return n;
}

// String.trim
static ustr trim(const ustr& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] <= u' ') a++;
    while (b > a && s[b - 1] <= u' ') b--;
    return s.substr(a, b - a);
}

// String.replace(CharSequence, CharSequence)
static ustr replace(const ustr& s, const ustr& from, const ustr& to) {
    ustr r;
    size_t off = 0, p;
    while ((p = s.find(from, off)) != ustr::npos) {
        r += s.substr(off, p - off);
        r += to;
        off = p + from.size();
    }
    r += s.substr(off);
    return r;
}

// Integer.parseInt of a string matching -?[0-9]+ ; false where Java throws NumberFormatException
static bool parseInt(const ustr& s, i32& out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && s[i] == u'-') { neg = true; i++; }
    if (i >= s.size()) return false;
    i64 v = 0;
    for (; i < s.size(); i++) {
        if (s[i] < u'0' || s[i] > u'9') return false;
        v = v * 10 + (s[i] - u'0');
        if (v > 2147483648LL) return false;
    }
    if (neg) v = -v;
    if (v > INT32_MAX || v < INT32_MIN) return false;
    out = (i32)v;
    return true;
}

// java.util.regex \s (no UNICODE_CHARACTER_CLASS)
static inline bool isSpace(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == 0x0B || c == u'\f' || c == u'\r'; }
// java.util.regex \d
static inline bool isDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
// CASE_INSENSITIVE without UNICODE_CASE: only ASCII letters fold
static inline bool eqIgnoreCaseAscii(char16_t c, char upper) {
    return c == (char16_t)upper || c == (char16_t)(upper + 32);
}

// command.toUpperCase().startsWith(prefix) for an upper case ASCII prefix (default locale en_US).
// Only these non-ASCII characters have an upper case form containing one of the letters of
// "MOVE" or "WAIT" (checked against the JDK on every code point).
static bool upperStartsWith(const ustr& s, const char* prefix) {
    ustr up;
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < s.size() && up.size() < plen; i++) {
        char16_t c = s[i];
        if (c >= u'a' && c <= u'z') up += (char16_t)(c - 32);
        else if (c < 0x80) up += c;
        else if (c == 0x0131) up += u"I";
        else if (c == 0x1E97) up += u"T\u0308";
        else if (c == 0x1E98) up += u"W\u030A";
        else if (c == 0x1E9A) up += u"A\u02BE";
        else if (c == 0xFB01) up += u"FI";
        else if (c == 0xFB03) up += u"FFI";
        else if (c == 0xFB05 || c == 0xFB06) up += u"ST";
        else up += (char16_t)0xFFFF; // upper case form without any letter of the prefixes
    }
    if (up.size() < plen) return false;
    for (size_t i = 0; i < plen; i++)
        if (up[i] != (char16_t)prefix[i]) return false;
    return true;
}

// Scanner line separators other than "\r\n"/'\n'/'\r'
static inline bool isScannerSeparator(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029 || c == 0x0085; }

} // namespace java

using java::ustr;

// ----------------------------------------------------------------------------
// Constants (Game.java, highest league)
// ----------------------------------------------------------------------------
namespace C {
    const int WIDTH = 10000;
    const int HEIGHT = 10000;
    const int DRONES_PER_PLAYER = 2;
    const int UGLY_UPPER_Y_LIMIT = 2500;
    const int DRONE_UPPER_Y_LIMIT = 0;
    const int DRONE_START_Y = 500;
    const int COLORS_PER_FISH = 4;
    const int DRONE_MAX_BATTERY = 30;
    const int LIGHT_BATTERY_COST = 5;
    const int DRONE_BATTERY_REGEN = 1;
    const int DARK_SCAN_RANGE = 800;
    const int LIGHT_SCAN_RANGE = 2000;
    const int UGLY_EAT_RANGE = 300;
    const int DRONE_HIT_RANGE = 200;
    const int FISH_HEARING_RANGE = (DARK_SCAN_RANGE + LIGHT_SCAN_RANGE) / 2;
    const int DRONE_MOVE_SPEED = 600;
    const int DRONE_SINK_SPEED = 300;
    const int DRONE_EMERGENCY_SPEED = 300;
    const double DRONE_MOVE_SPEED_LOSS_PER_SCAN = 0;
    const int FISH_SWIM_SPEED = 200;
    const int FISH_AVOID_RANGE = 600;
    const int FISH_FLEE_SPEED = 400;
    const int UGLY_ATTACK_SPEED = 540; // (int) (DRONE_MOVE_SPEED * 0.9)
    const int UGLY_SEARCH_SPEED = UGLY_ATTACK_SPEED / 2;
    const int FISH_X_SPAWN_LIMIT = 1000;
    const int FISH_SPAWN_MIN_SEP = 1000;
    const double CENTER_X = (WIDTH - 1) / 2.0;
    const int MAX_TURNS = 201;
    const int N_TYPES = 3; // FishType: JELLY, FISH, CRAB

    // GameManager / Referee settings (engine 4.4.2)
    const int FIRST_TURN_MAX_TIME = 1000;
    const int TURN_MAX_TIME = 50;
    const int FIRST_TURN_HEAD_START = 500; // see ChildProc::execute
    const size_t AGENT_MAX_BUFFER_SIZE = 10000;
    const size_t REFEREE_MAX_BUFFER_SIZE = 30000;
    const int GAME_SUMMARY_PER_TURN_HARD_QUOTA = 800;
    const int GAME_SUMMARY_TOTAL_HARD_QUOTA = 512 * 1024;
}

// ----------------------------------------------------------------------------
// Vector.java
// ----------------------------------------------------------------------------
struct Vec {
    double x, y;
};
static inline Vec between(Vec a, Vec b) { return {b.x - a.x, b.y - a.y}; } // new Vector(a, b)
static inline Vec vadd(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
static inline Vec vmult(Vec v, double f) { return {v.x * f, v.y * f}; }
static inline double vlength(Vec v) { return std::sqrt(v.x * v.x + v.y * v.y); }
static inline Vec vnormalize(Vec v) {
    double length = vlength(v);
    if (length == 0) return {0, 0};
    return {v.x / length, v.y / length};
}
static inline Vec vround(Vec v) { return {(double)java::l2i(java::round(v.x)), (double)java::l2i(java::round(v.y))}; }
static inline Vec epsilonRound(Vec v) {
    return {(double)java::round(v.x * 10000000.0) / 10000000.0, (double)java::round(v.y * 10000000.0) / 10000000.0};
}
static inline Vec hsymmetric(Vec v, double center) { return {2 * center - v.x, v.y}; }
static inline Vec hsymmetric(Vec v) { return {-v.x, v.y}; }
static inline Vec vsymmetric(Vec v) { return {v.x, -v.y}; }
static inline bool isZero(Vec v) { return v.x == 0 && v.y == 0; }
// self.inRange(v, range)
static inline bool inRange(Vec self, Vec v, double range) {
    return (v.x - self.x) * (v.x - self.x) + (v.y - self.y) * (v.y - self.y) <= range * range;
}
// self.sqrEuclideanTo(other): Math.pow(d, 2) is d * d for HotSpot (and every coordinate is an
// integer below 2^15 anyway, so the squares are exact)
static inline double sqrEuclideanTo(Vec self, Vec other) {
    double dx = other.x - self.x, dy = other.y - self.y;
    return dx * dx + dy * dy;
}
static inline std::string toIntString(Vec v) { return std::to_string(java::d2i(v.x)) + " " + std::to_string(java::d2i(v.y)); }

// ----------------------------------------------------------------------------
// Entities
// ----------------------------------------------------------------------------
struct Scan {
    int fishId;
    int type;
    int color;
};
static inline bool sameScan(const Scan& a, const Scan& b) { return a.color == b.color && a.type == b.type; }
static bool containsScan(const std::vector<Scan>& v, const Scan& s) {
    for (const Scan& o : v)
        if (sameScan(o, s)) return true;
    return false;
}

struct Fish {
    int type;
    Vec pos;
    int color;
    Vec speed;
    int id;
    int lowY, highY;
    bool isFleeing = false;
};

struct Ugly {
    Vec pos;
    Vec speed = {0, 0};
    bool hasTarget = false;
    Vec target = {0, 0};
    int id;
    bool foundTarget = false;
};

struct Drone {
    Vec pos;
    bool hasMove = false;
    Vec move = {0, 0};
    Vec speed = {0, 0};
    int battery = C::DRONE_MAX_BATTERY;
    std::vector<Scan> scans;
    std::vector<int> fishesScannedThisTurn;
    bool lightSwitch = false;
    bool lightOn = false;
    bool dying = false;
    bool dead = false;
    bool didReport = false;
    int id;
    double dieAt = 0;
    ustr message;
    int owner;

    bool isEngineOn() const { return hasMove; }
    bool isDeadOrDying() const { return dying || dead; }
};

struct Player {
    int index;
    std::vector<Drone*> drones;
    std::vector<Scan> scans;          // LinkedHashSet<Scan>
    std::vector<Fish*> visibleFishes; // LinkedHashSet<Fish> (identity)
    int points = 0;
    // AbstractPlayer / AbstractMultiplayerPlayer
    bool active = true;
    int score = 0;
    bool hasNeverBeenExecuted = true;
    bool timeout = false;
    std::vector<ustr> outputs;
    std::string inputs;

    int getExpectedOutputLines() const { return (int)drones.size(); }
};

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
static const int ABSENT = INT_MIN; // key not in the HashMap

struct Game {
    java::SHA1PRNG random;
    std::vector<Player*> players;
    std::vector<Fish*> fishes;
    std::vector<Ugly*> uglies;
    std::deque<Fish> fishPool;
    std::deque<Ugly> uglyPool;
    std::deque<Drone> dronePool;

    int firstToScan[C::COLORS_PER_FISH][C::N_TYPES];
    int firstToScanTemp[C::COLORS_PER_FISH][C::N_TYPES];
    int firstToScanAllFishOfColor[C::COLORS_PER_FISH];
    int firstToScanAllFishOfColorTemp[C::COLORS_PER_FISH];
    int firstToScanAllFishOfType[C::N_TYPES];
    int firstToScanAllFishOfTypeTemp[C::N_TYPES];

    int gameTurn = 1;
    int entityCount = 0;
};

static Game G;
static Player players[2];
static bool gameEnd = false;
static FILE* trace = nullptr;

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (auto& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

// Closest.java
template <class T>
struct Closest {
    std::vector<T*> list;
    double distance;
    Vec getMeanPos() const {
        if (list.size() == 1) return list[0]->pos;
        double x = 0, y = 0;
        for (T* e : list) {
            x += e->pos.x;
            y += e->pos.y;
        }
        return {x / (double)list.size(), y / (double)list.size()};
    }
};

template <class T>
static Closest<T> getClosestTo(Vec from, const std::vector<T*>& targets) {
    Closest<T> r;
    double minDist = 0;
    for (T* t : targets) {
        double dist = sqrEuclideanTo(t->pos, from);
        if (r.list.empty() || dist < minDist) {
            r.list.clear();
            r.list.push_back(t);
            minDist = dist;
        } else if (dist == minDist) {
            r.list.push_back(t);
        }
    }
    r.distance = std::sqrt(minDist);
    return r;
}

static void initPlayers() {
    int idxs[] = {0, 2, 1, 3};
    int idxIdx = 0;
    for (int i = 0; i < C::DRONES_PER_PLAYER; ++i) {
        double x = C::WIDTH / (C::DRONES_PER_PLAYER * 2 + 1) * (idxs[idxIdx] + 1);
        idxIdx++;
        for (Player* player : getActivePlayers()) {
            G.dronePool.emplace_back();
            Drone* d = &G.dronePool.back();
            d->id = G.entityCount++;
            d->owner = player->index;
            d->pos = {x, (double)C::DRONE_START_Y};
            if (player->index == 1) d->pos = hsymmetric(d->pos, C::CENTER_X);
            player->drones.push_back(d);
        }
    }
}

static void initFish() {
    for (int col = 0; col < C::COLORS_PER_FISH; col += 2) {
        for (int typeIdx = 0; typeIdx < C::N_TYPES; ++typeIdx) {
            bool positionFound = false;
            int iterations = 0;
            int x = 0, y = 0;
            int lowY = C::HEIGHT / 4;
            int highY = C::HEIGHT;
            while (!positionFound) {
                x = G.random.nextInt(C::WIDTH - C::FISH_X_SPAWN_LIMIT * 2) + C::FISH_X_SPAWN_LIMIT;
                if (typeIdx == 0) {
                    y = 1 * C::HEIGHT / 4 + C::FISH_SPAWN_MIN_SEP;
                    lowY = 1 * C::HEIGHT / 4;
                    highY = 2 * C::HEIGHT / 4;
                } else if (typeIdx == 1) {
                    y = 2 * C::HEIGHT / 4 + C::FISH_SPAWN_MIN_SEP;
                    lowY = 2 * C::HEIGHT / 4;
                    highY = 3 * C::HEIGHT / 4;
                } else {
                    y = 3 * C::HEIGHT / 4 + C::FISH_SPAWN_MIN_SEP;
                    lowY = 3 * C::HEIGHT / 4;
                    highY = 4 * C::HEIGHT / 4;
                }
                y += G.random.nextInt(C::HEIGHT / 4 - C::FISH_SPAWN_MIN_SEP * 2);

                bool tooClose = false;
                for (Fish* other : G.fishes) {
                    if (inRange(other->pos, {(double)x, (double)y}, C::FISH_SPAWN_MIN_SEP)) {
                        tooClose = true;
                        break;
                    }
                }
                bool tooCloseToCenter = std::fabs(C::CENTER_X - x) <= C::FISH_SPAWN_MIN_SEP;
                if ((!tooClose && !tooCloseToCenter) || iterations > 100) positionFound = true;
                iterations++;
            }
            G.fishPool.emplace_back();
            Fish* f = &G.fishPool.back();
            f->pos = {(double)x, (double)y};
            f->type = typeIdx;
            f->color = col;
            f->id = G.entityCount++;
            f->lowY = lowY;
            f->highY = highY;
            f->speed = {0, 0};

            // (random.nextInt() * 7) overflows as an int
            i32 r7 = (i32)((u32)G.random.nextInt() * 7u);
            double snapped = (double)r7 * 3.141592653589793 / 4; // Math.PI
            Vec direction = {std::cos(snapped), std::sin(snapped)};
            f->speed = vround(vmult(direction, C::FISH_SWIM_SPEED));
            G.fishes.push_back(f);

            Vec otherPos = hsymmetric(f->pos, C::CENTER_X);
            G.fishPool.emplace_back();
            Fish* o = &G.fishPool.back();
            o->pos = otherPos;
            o->type = typeIdx;
            o->color = col + 1;
            o->id = G.entityCount++;
            o->lowY = f->lowY;
            o->highY = f->highY;
            o->speed = hsymmetric(f->speed);
            G.fishes.push_back(o);
        }
    }
}

static void initUglies() {
    int uglyCount = 1 + G.random.nextInt(3);
    for (int i = 0; i < uglyCount; ++i) {
        int x = G.random.nextInt(C::WIDTH / 2);
        int y = C::HEIGHT / 2 + G.random.nextInt(C::HEIGHT / 2);
        for (int k = 0; k < 2; ++k) {
            G.uglyPool.emplace_back();
            Ugly* ugly = &G.uglyPool.back();
            ugly->pos = {(double)x, (double)y};
            ugly->id = G.entityCount++;
            if (k == 1) ugly->pos = hsymmetric(ugly->pos, C::CENTER_X);
            G.uglies.push_back(ugly);
        }
    }
}

static void gameInit(i64 seed) {
    G.entityCount = 0;
    for (auto& p : players) G.players.push_back(&p);
    G.random.setSeed(seed);
    G.gameTurn = 1;
    for (int c = 0; c < C::COLORS_PER_FISH; c++) {
        for (int t = 0; t < C::N_TYPES; t++) G.firstToScan[c][t] = G.firstToScanTemp[c][t] = ABSENT;
        G.firstToScanAllFishOfColor[c] = G.firstToScanAllFishOfColorTemp[c] = ABSENT;
    }
    for (int t = 0; t < C::N_TYPES; t++) G.firstToScanAllFishOfType[t] = G.firstToScanAllFishOfTypeTemp[t] = ABSENT;
    initPlayers();
    initFish();
    initUglies();
}

static std::vector<Drone*> allDrones() {
    std::vector<Drone*> r;
    for (Player* p : G.players)
        for (Drone* d : p->drones) r.push_back(d);
    return r;
}

static bool updateUglyTarget(Ugly* ugly) {
    std::vector<Drone*> targetableDrones;
    for (Drone* drone : allDrones()) {
        if (!inRange(drone->pos, ugly->pos, drone->lightOn ? C::LIGHT_SCAN_RANGE : C::DARK_SCAN_RANGE)) continue;
        if (drone->isDeadOrDying()) continue;
        targetableDrones.push_back(drone);
    }
    if (!targetableDrones.empty()) {
        Closest<Drone> closestTargets = getClosestTo(ugly->pos, targetableDrones);
        ugly->target = closestTargets.getMeanPos();
        ugly->hasTarget = true;
        return true;
    }
    ugly->hasTarget = false;
    return false;
}

// Game.getCollision: the collision time, or -1 (Collision.NONE)
static double getCollision(const Drone* drone, const Ugly* ugly) {
    // Check instant collision
    if (inRange(ugly->pos, drone->pos, C::DRONE_HIT_RANGE + C::UGLY_EAT_RANGE)) return 0.0;
    // Both units are motionless
    if (isZero(drone->speed) && isZero(ugly->speed)) return -1;
    double x = ugly->pos.x;
    double y = ugly->pos.y;
    double ux = drone->pos.x;
    double uy = drone->pos.y;
    double x2 = x - ux;
    double y2 = y - uy;
    double r2 = C::UGLY_EAT_RANGE + C::DRONE_HIT_RANGE;
    double vx2 = ugly->speed.x - drone->speed.x;
    double vy2 = ugly->speed.y - drone->speed.y;
    double a = vx2 * vx2 + vy2 * vy2;
    if (a <= 0.0) return -1;
    double b = 2.0 * (x2 * vx2 + y2 * vy2);
    double c = x2 * x2 + y2 * y2 - r2 * r2;
    double delta = b * b - 4.0 * a * c;
    if (delta < 0.0) return -1;
    double t = (-b - std::sqrt(delta)) / (2.0 * a);
    if (t <= 0.0) return -1;
    if (t > 1.0) return -1;
    return t;
}

static void snapToDroneZone(Drone* drone) {
    if (drone->pos.y > C::HEIGHT - 1) {
        drone->pos = {drone->pos.x, (double)(C::HEIGHT - 1)};
    } else if (drone->pos.y < C::DRONE_UPPER_Y_LIMIT) {
        drone->pos = {drone->pos.x, (double)C::DRONE_UPPER_Y_LIMIT};
    }
    if (drone->pos.x < 0) {
        drone->pos = {0, drone->pos.y};
    } else if (drone->pos.x >= C::WIDTH) {
        drone->pos = {(double)(C::WIDTH - 1), drone->pos.y};
    }
}

static void snapToFishZone(Fish* fish) {
    if (fish->pos.y > C::HEIGHT - 1) {
        fish->pos = {fish->pos.x, (double)(C::HEIGHT - 1)};
    } else if (fish->pos.y > fish->highY) {
        fish->pos = {fish->pos.x, (double)fish->highY};
    } else if (fish->pos.y < fish->lowY) {
        fish->pos = {fish->pos.x, (double)fish->lowY};
    }
}

static void snapToUglyZone(Ugly* ugly) {
    if (ugly->pos.y > C::HEIGHT - 1) {
        ugly->pos = {ugly->pos.x, (double)(C::HEIGHT - 1)};
    } else if (ugly->pos.y < C::UGLY_UPPER_Y_LIMIT) {
        ugly->pos = {ugly->pos.x, (double)C::UGLY_UPPER_Y_LIMIT};
    }
}

static void moveEntities() {
    for (Player* p : G.players) {
        for (Drone* drone : p->drones) {
            if (drone->dead) continue;
            // NOTE: the collision code does not take into account the snap to map borders
            for (Ugly* ugly : G.uglies) {
                double t = getCollision(drone, ugly);
                if (t >= 0) {
                    drone->dying = true;
                    drone->scans.clear();
                    drone->dieAt = t;
                    // If two uglies hit the drone, let's just keep the first collision, it matters not.
                    break;
                }
            }
        }
    }

    for (Player* p : G.players) {
        for (Drone* drone : p->drones) {
            drone->pos = vadd(drone->pos, drone->speed);
            snapToDroneZone(drone);
        }
    }

    for (Fish* f : G.fishes) {
        f->pos = vadd(f->pos, f->speed);
        snapToFishZone(f);
    }
    // fishes.removeAll(fish out of the map)
    std::vector<Fish*> kept;
    for (Fish* fish : G.fishes)
        if (!(fish->pos.x > C::WIDTH - 1 || fish->pos.x < 0)) kept.push_back(fish);
    G.fishes.swap(kept);

    for (Ugly* ugly : G.uglies) {
        ugly->pos = vadd(ugly->pos, ugly->speed);
        snapToUglyZone(ugly);
    }
}

static void updateUglySpeeds() {
    for (Ugly* ugly : G.uglies) {
        if (ugly->hasTarget) {
            Vec attackVec = between(ugly->pos, ugly->target);
            if (vlength(attackVec) > C::UGLY_ATTACK_SPEED) attackVec = vmult(vnormalize(attackVec), C::UGLY_ATTACK_SPEED);
            ugly->speed = vround(attackVec);
        } else {
            if (vlength(ugly->speed) > C::UGLY_SEARCH_SPEED)
                ugly->speed = vround(vmult(vnormalize(ugly->speed), C::UGLY_SEARCH_SPEED));

            if (!isZero(ugly->speed)) {
                std::vector<Ugly*> others;
                for (Ugly* u : G.uglies)
                    if (u != ugly) others.push_back(u);
                Closest<Ugly> closestUglies = getClosestTo(ugly->pos, others);
                if (!closestUglies.list.empty() && closestUglies.distance <= C::FISH_AVOID_RANGE) {
                    Vec avoid = closestUglies.getMeanPos();
                    Vec avoidDir = vnormalize(between(avoid, ugly->pos));
                    if (!isZero(avoidDir)) ugly->speed = vround(vmult(avoidDir, C::FISH_SWIM_SPEED));
                }
            }
            Vec nextPos = vadd(ugly->pos, ugly->speed);

            if ((nextPos.x < 0 && nextPos.x < ugly->pos.x) || (nextPos.x > C::WIDTH - 1 && nextPos.x > ugly->pos.x))
                ugly->speed = hsymmetric(ugly->speed);

            if ((nextPos.y < C::UGLY_UPPER_Y_LIMIT && nextPos.y < ugly->pos.y) || (nextPos.y > C::HEIGHT - 1 && nextPos.y > ugly->pos.y))
                ugly->speed = vsymmetric(ugly->speed);
        }
    }
}

static void updateUglyTargets() {
    for (Ugly* ugly : G.uglies) ugly->foundTarget = updateUglyTarget(ugly);
}

static void updateFish() {
    for (Fish* fish : G.fishes) {
        fish->isFleeing = false;

        bool flee = false;
        Vec fleeFrom = {0, 0};
        // FISH_WILL_FLEE
        std::vector<Drone*> engines;
        for (Drone* d : allDrones())
            if (d->isEngineOn() && !d->dead) engines.push_back(d);
        Closest<Drone> closestDrones = getClosestTo(fish->pos, engines);
        if (!closestDrones.list.empty() && closestDrones.distance <= C::FISH_HEARING_RANGE) {
            fleeFrom = closestDrones.getMeanPos();
            flee = true;
        }
        if (flee) {
            Vec fleeDir = vnormalize(between(fleeFrom, fish->pos));
            Vec fleeVec = vmult(fleeDir, C::FISH_FLEE_SPEED);
            fish->speed = vround(fleeVec);
            fish->isFleeing = true;
        } else {
            std::vector<Fish*> others;
            for (Fish* f : G.fishes)
                if (f != fish) others.push_back(f);
            Closest<Fish> closestFishes = getClosestTo(fish->pos, others);

            Vec swimVec = vmult(vnormalize(fish->speed), C::FISH_SWIM_SPEED);
            if (!closestFishes.list.empty() && closestFishes.distance <= C::FISH_AVOID_RANGE) {
                Vec avoid = closestFishes.getMeanPos();
                Vec avoidDir = vnormalize(between(avoid, fish->pos));
                swimVec = vmult(avoidDir, C::FISH_SWIM_SPEED);
            }

            Vec nextPos = vadd(fish->pos, swimVec);

            if ((nextPos.x < 0 && nextPos.x < fish->pos.x) || (nextPos.x > C::WIDTH - 1 && nextPos.x > fish->pos.x))
                swimVec = hsymmetric(swimVec);

            double yHighest = std::min(C::HEIGHT - 1, fish->highY);

            if ((nextPos.y < fish->lowY && nextPos.y < fish->pos.y) || (nextPos.y > yHighest && nextPos.y > fish->pos.y))
                swimVec = vsymmetric(swimVec);
            fish->speed = vround(epsilonRound(swimVec));
        }
    }
}

static void updateDrones() {
    for (Player* p : G.players) {
        for (Drone* drone : p->drones) {
            int moveSpeed = java::d2i(C::DRONE_MOVE_SPEED - C::DRONE_MOVE_SPEED * C::DRONE_MOVE_SPEED_LOSS_PER_SCAN * (double)drone->scans.size());
            if (drone->dead) {
                drone->speed = vmult({0, -1}, C::DRONE_EMERGENCY_SPEED);
            } else if (drone->hasMove) {
                Vec moveVec = between(drone->pos, drone->move);
                if (vlength(moveVec) > moveSpeed) moveVec = vmult(vnormalize(moveVec), moveSpeed);
                drone->speed = vround(moveVec);
            } else if (drone->pos.y < C::HEIGHT - 1) {
                drone->speed = vmult({0, 1}, C::DRONE_SINK_SPEED);
            }
            // else: the drone keeps its previous speed
        }
    }
}

static void upkeepDrones() {
    for (Player* p : G.players) {
        for (Drone* d : p->drones) {
            if (d->dying) {
                d->dead = true;
                d->dying = false;
            } else if (d->dead && d->pos.y == C::DRONE_UPPER_Y_LIMIT) {
                d->dead = false;
            }
        }
    }
}

static void doBatteries() {
    for (Player* player : G.players) {
        for (Drone* drone : player->drones) {
            if (drone->dead) {
                drone->lightOn = false;
                continue;
            }
            if (drone->lightSwitch && drone->battery >= C::LIGHT_BATTERY_COST && !drone->dead) {
                drone->lightOn = true;
            } else {
                drone->lightOn = false;
            }
            if (drone->lightOn) {
                drone->battery -= C::LIGHT_BATTERY_COST;
            } else if (drone->battery < C::DRONE_MAX_BATTERY) {
                drone->battery += C::DRONE_BATTERY_REGEN;
                if (drone->battery >= C::DRONE_MAX_BATTERY) drone->battery = C::DRONE_MAX_BATTERY;
            }
        }
    }
}

static void doScan() {
    for (Player* player : G.players) {
        for (Drone* drone : player->drones) {
            if (drone->isDeadOrDying()) continue;
            for (Fish* fish : G.fishes) {
                if (!inRange(fish->pos, drone->pos, drone->lightOn ? C::LIGHT_SCAN_RANGE : C::DARK_SCAN_RANGE)) continue;
                if (std::find(player->visibleFishes.begin(), player->visibleFishes.end(), fish) == player->visibleFishes.end())
                    player->visibleFishes.push_back(fish);
                // drone.scans.size() < DRONE_MAX_SCANS (Integer.MAX_VALUE)
                Scan scan = {fish->id, fish->type, fish->color};
                if (!containsScan(player->scans, scan)) {
                    if (!containsScan(drone->scans, scan)) {
                        drone->scans.push_back(scan);
                        drone->fishesScannedThisTurn.push_back(fish->id);
                    }
                }
            }
        }
    }
}

static bool playerScanned(const Player* player, const Scan& scan) { return containsScan(player->scans, scan); }

static bool playerScannedAllFishOfType(const Player* player, int type) {
    for (int color = 0; color < C::COLORS_PER_FISH; ++color)
        if (!playerScanned(player, {0, type, color})) return false;
    return true;
}

static bool playerScannedAllFishOfColor(const Player* player, int color) {
    for (int type = 0; type < C::N_TYPES; type++)
        if (!playerScanned(player, {0, type, color})) return false;
    return true;
}

static bool applyScansForReport(Player* player, Drone* drone) {
    bool pointsScored = false;
    for (const Scan& scan : drone->scans) {
        if (G.firstToScan[scan.color][scan.type] == ABSENT) {
            if (G.firstToScanTemp[scan.color][scan.type] != ABSENT) {
                // Opponent also completed this report this turn, nobody gets the bonus
                G.firstToScanTemp[scan.color][scan.type] = ABSENT;
            } else {
                G.firstToScanTemp[scan.color][scan.type] = player->index;
            }
        }
    }
    // player.scans.addAll(drone.scans)
    for (const Scan& scan : drone->scans) {
        if (!containsScan(player->scans, scan)) {
            player->scans.push_back(scan);
            pointsScored = true;
        }
    }
    for (Drone* other : player->drones) {
        if (drone != other) {
            // other.scans.removeAll(drone.scans)
            std::vector<Scan> kept;
            for (const Scan& s : other->scans)
                if (!containsScan(drone->scans, s)) kept.push_back(s);
            other->scans.swap(kept);
        }
    }
    drone->scans.clear();
    return pointsScored;
}

static void detectFirstToComboBonuses(Player* player) {
    for (int type = 0; type < C::N_TYPES; type++) {
        if (G.firstToScanAllFishOfType[type] == ABSENT) {
            if (playerScannedAllFishOfType(player, type)) {
                if (G.firstToScanAllFishOfTypeTemp[type] != ABSENT) {
                    // Opponent also completed this report this turn, nobody gets the bonus
                    G.firstToScanAllFishOfTypeTemp[type] = ABSENT;
                } else {
                    G.firstToScanAllFishOfTypeTemp[type] = player->index;
                }
            }
        }
    }
    for (int color = 0; color < C::COLORS_PER_FISH; ++color) {
        if (G.firstToScanAllFishOfColor[color] == ABSENT) {
            if (playerScannedAllFishOfColor(player, color)) {
                if (G.firstToScanAllFishOfColorTemp[color] != ABSENT) {
                    G.firstToScanAllFishOfColorTemp[color] = ABSENT;
                } else {
                    G.firstToScanAllFishOfColorTemp[color] = player->index;
                }
            }
        }
    }
}

static void persistFirstToScanBonuses() {
    for (int c = 0; c < C::COLORS_PER_FISH; c++) {
        for (int t = 0; t < C::N_TYPES; t++) {
            if (G.firstToScanTemp[c][t] != ABSENT && G.firstToScan[c][t] == ABSENT) G.firstToScan[c][t] = G.firstToScanTemp[c][t];
            G.firstToScanTemp[c][t] = ABSENT;
        }
    }
    for (int t = 0; t < C::N_TYPES; t++) {
        if (G.firstToScanAllFishOfTypeTemp[t] != ABSENT && G.firstToScanAllFishOfType[t] == ABSENT)
            G.firstToScanAllFishOfType[t] = G.firstToScanAllFishOfTypeTemp[t];
        G.firstToScanAllFishOfTypeTemp[t] = ABSENT;
    }
    for (int c = 0; c < C::COLORS_PER_FISH; c++) {
        if (G.firstToScanAllFishOfColorTemp[c] != ABSENT && G.firstToScanAllFishOfColor[c] == ABSENT)
            G.firstToScanAllFishOfColor[c] = G.firstToScanAllFishOfColorTemp[c];
        G.firstToScanAllFishOfColorTemp[c] = ABSENT;
    }
}

static int computePlayerScore(const Player* p) {
    int total = 0;
    for (const Scan& scan : p->scans) {
        total += scan.type + 1;
        if (G.firstToScan[scan.color][scan.type] == p->index) total += scan.type + 1;
    }
    for (int type = 0; type < C::N_TYPES; type++) {
        if (playerScannedAllFishOfType(p, type)) total += C::COLORS_PER_FISH;
        if (G.firstToScanAllFishOfType[type] == p->index) total += C::COLORS_PER_FISH;
    }
    for (int color = 0; color < C::COLORS_PER_FISH; ++color) {
        if (playerScannedAllFishOfColor(p, color)) total += C::N_TYPES;
        if (G.firstToScanAllFishOfColor[color] == p->index) total += C::N_TYPES;
    }
    return total;
}

static bool hasFishEscaped(const Scan& scan) {
    for (Fish* fish : G.fishes)
        if (fish->color == scan.color && fish->type == scan.type) return false;
    return true;
}

static bool isFishScannedByPlayerDrone(const Scan& scan, const Player* player) {
    for (Drone* d : player->drones)
        if (containsScan(d->scans, scan)) return true;
    return false;
}

static bool isTypeComboStillPossible(const Player* p, int type) {
    if (playerScannedAllFishOfType(p, type)) return false;
    for (int color = 0; color < C::COLORS_PER_FISH; color++) {
        Scan scan = {0, type, color};
        if (hasFishEscaped(scan) && !isFishScannedByPlayerDrone(scan, p) && !playerScanned(p, scan)) return false;
    }
    return true;
}

static bool isColorComboStillPossible(const Player* p, int color) {
    if (playerScannedAllFishOfColor(p, color)) return false;
    for (int type = 0; type < C::N_TYPES; type++) {
        Scan scan = {0, type, color};
        if (hasFishEscaped(scan) && !isFishScannedByPlayerDrone(scan, p) && !playerScanned(p, scan)) return false;
    }
    return true;
}

static int computeMaxPlayerScore(const Player* p) {
    int total = computePlayerScore(p);
    const Player* p2 = G.players[1 - p->index];

    for (int color = 0; color < C::COLORS_PER_FISH; color++) {
        for (int type = 0; type < C::N_TYPES; type++) {
            Scan scan = {0, type, color};
            if (!playerScanned(p, scan)) {
                if (isFishScannedByPlayerDrone(scan, p) || !hasFishEscaped(scan)) {
                    total += type + 1;
                    if (G.firstToScan[color][type] == ABSENT) total += type + 1;
                }
            }
        }
    }
    for (int type = 0; type < C::N_TYPES; type++) {
        if (isTypeComboStillPossible(p, type)) {
            total += C::COLORS_PER_FISH;
            if (G.firstToScanAllFishOfType[type] != p2->index) total += C::COLORS_PER_FISH;
        }
    }
    for (int color = 0; color < C::COLORS_PER_FISH; color++) {
        if (isColorComboStillPossible(p, color)) {
            total += C::N_TYPES;
            if (G.firstToScanAllFishOfColor[color] != p2->index) total += C::N_TYPES;
        }
    }
    return total;
}

static bool hasScannedAllRemainingFish(const Player* player) {
    for (Fish* fish : G.fishes)
        if (!playerScanned(player, {fish->id, fish->type, fish->color})) return false;
    return true;
}

static bool isGameOver() {
    bool all = true;
    for (Player* player : G.players)
        if (!hasScannedAllRemainingFish(player)) { all = false; break; }
    if (all) return true;
    return G.gameTurn >= 200 || computeMaxPlayerScore(G.players[0]) < G.players[1]->points ||
           computeMaxPlayerScore(G.players[1]) < G.players[0]->points;
}

static void computeScoreOnEnd() {
    for (Player* player : G.players) {
        for (Drone* drone : player->drones) applyScansForReport(player, drone);
        detectFirstToComboBonuses(player);
    }
    persistFirstToScanBonuses();
    for (Player* p : G.players) {
        if (p->active) {
            int score = computePlayerScore(p);
            p->score = score;
            p->points = score;
        } else {
            p->score = -1;
        }
    }
}

// ----------------------------------------------------------------------------
// Engine bookkeeping for the frame the runner reads last (GameManager game summary / tooltips)
// ----------------------------------------------------------------------------
struct SummaryLine {
    size_t chars;  // String.length()
    size_t bytes;  // UTF-8 bytes
    bool unknown;  // the GameSummaryManager text (small, ASCII, always the last line of a turn)
};
static std::vector<SummaryLine> currentGameSummary;
static std::vector<std::pair<size_t, int>> currentTooltips; // UTF-8 bytes of the message, player
static long long totalGameSummaryBytes = 0;

static void addToGameSummary(size_t chars, size_t bytes, bool unknown) {
    long long total = 0;
    for (auto& l : currentGameSummary) total += (long long)l.chars;
    if (total < C::GAME_SUMMARY_PER_TURN_HARD_QUOTA && total + totalGameSummaryBytes < C::GAME_SUMMARY_TOTAL_HARD_QUOTA) {
        currentGameSummary.push_back({chars, bytes, unknown});
        totalGameSummaryBytes += total;
    }
}
static void addToGameSummary(const ustr& s) { addToGameSummary(s.size(), java::utf8Length(s), false); }
static void addTooltip(int player, const ustr& message) { currentTooltips.push_back({java::utf8Length(message), player}); }

// The runner reads every referee command into a 30000-byte buffer (100000 for the first frame)
static bool lastFrameReadable() {
    bool unknown = false;
    size_t bytes = 0;
    for (size_t i = 0; i < currentGameSummary.size(); i++) {
        if (currentGameSummary[i].unknown) unknown = true;
        bytes += currentGameSummary[i].bytes + (i > 0 ? 1 : 0);
    }
    // the GameSummaryManager text is only added below 800 chars of summary, so it always fits
    if (!currentGameSummary.empty() && !unknown && bytes + 1 > C::REFEREE_MAX_BUFFER_SIZE) return false;
    if (!currentTooltips.empty()) {
        size_t tb = 0;
        for (auto& t : currentTooltips) tb += t.first + 1 + std::to_string(t.second).size() + 1;
        if (tb > C::REFEREE_MAX_BUFFER_SIZE) return false; // content + final line terminator
    }
    return true;
}

static void doReport() {
    for (Player* player : G.players) {
        bool pointsScored = false;
        for (Drone* drone : player->drones) {
            if (drone->isDeadOrDying()) continue;
            if (!drone->scans.empty() && drone->pos.y <= C::DRONE_START_Y) {
                bool droneScored = applyScansForReport(player, drone);
                pointsScored |= droneScored;
                if (droneScored) drone->didReport = true;
            }
        }
        if (pointsScored) addTooltip(player->index, java::ascii("$" + std::to_string(player->index) + " reports their findings."));
        detectFirstToComboBonuses(player);
    }
    persistFirstToScanBonuses();
    for (Player* p : G.players) p->points = computePlayerScore(p);
}

static void performGameUpdate(int frameIdx) {
    (void)frameIdx;
    for (Player* p : G.players) p->visibleFishes.clear(); // clearPlayerInfo
    doBatteries();

    // Update speeds
    updateDrones();
    // Move
    moveEntities();
    // Target
    updateUglyTargets();
    // Scans
    doScan();
    doReport();
    // Upkeep
    upkeepDrones();
    updateFish();
    updateUglySpeeds();

    if (isGameOver()) {
        computeScoreOnEnd();
        gameEnd = true;
    }
    G.gameTurn++;

    addToGameSummary(0, 0, true); // gameSummaryManager.toString()
}

static void onEnd() {
    for (Player* p : G.players) {
        if (p->active) {
            int score = computePlayerScore(p);
            p->score = score;
            p->points = score;
        } else {
            p->score = -1;
        }
    }
}

// ----------------------------------------------------------------------------
// Serializer.java
// ----------------------------------------------------------------------------
static void addLine(std::string& out, const std::string& line) {
    out += line;
    out += '\n';
}

static void serializeGlobalInfoFor(Player* player) {
    std::string& in = player->inputs;
    addLine(in, std::to_string(G.fishes.size() + G.uglies.size()));
    for (Fish* fish : G.fishes)
        addLine(in, std::to_string(fish->id) + " " + std::to_string(fish->color) + " " + std::to_string(fish->type));
    for (Ugly* ugly : G.uglies) addLine(in, std::to_string(ugly->id) + " -1 -1");
}

static void serializeFrameInfoFor(Player* player) {
    std::string& in = player->inputs;
    Player* other = G.players[0] != player ? G.players[0] : G.players[1];

    addLine(in, std::to_string(player->points));
    addLine(in, std::to_string(other->points));
    for (Player* p : {player, other}) {
        addLine(in, std::to_string(p->scans.size()));
        for (const Scan& scan : p->scans) addLine(in, std::to_string(scan.fishId));
    }
    for (Player* p : {player, other}) {
        addLine(in, std::to_string(p->drones.size()));
        for (Drone* drone : p->drones)
            addLine(in, std::to_string(drone->id) + " " + toIntString(drone->pos) + " " + (drone->dead ? "1" : "0") + " " +
                            std::to_string(drone->battery));
    }
    size_t n = 0;
    for (Player* p : {player, other})
        for (Drone* drone : p->drones) n += drone->scans.size();
    addLine(in, std::to_string(n));
    for (Player* p : {player, other})
        for (Drone* drone : p->drones)
            for (const Scan& scan : drone->scans) addLine(in, std::to_string(drone->id) + " " + std::to_string(scan.fishId));

    std::vector<Ugly*> visibleUglies;
    for (Ugly* ugly : G.uglies) {
        for (Drone* drone : player->drones) {
            if (inRange(ugly->pos, drone->pos, C::UGLY_EAT_RANGE + (drone->lightOn ? C::LIGHT_SCAN_RANGE : C::DARK_SCAN_RANGE))) {
                visibleUglies.push_back(ugly);
                break;
            }
        }
    }
    addLine(in, std::to_string(player->visibleFishes.size() + visibleUglies.size()));
    for (Fish* fish : player->visibleFishes)
        addLine(in, std::to_string(fish->id) + " " + toIntString(fish->pos) + " " + toIntString(fish->speed));
    for (Ugly* ugly : visibleUglies)
        addLine(in, std::to_string(ugly->id) + " " + toIntString(ugly->pos) + " " + toIntString(ugly->speed));

    addLine(in, std::to_string(C::DRONES_PER_PLAYER * (G.fishes.size() + G.uglies.size())));
    for (Drone* drone : player->drones) {
        auto blip = [&](int id, Vec pos) {
            std::string direction;
            direction += pos.y > drone->pos.y ? "B" : "T";
            direction += pos.x > drone->pos.x ? "R" : "L";
            addLine(in, std::to_string(drone->id) + " " + std::to_string(id) + " " + direction);
        };
        for (Fish* fish : G.fishes) blip(fish->id, fish->pos);
        for (Ugly* ugly : G.uglies) blip(ugly->id, ugly->pos);
    }
}

// ----------------------------------------------------------------------------
// Trace (-l): JSON strings, with lone surrogates written as '?' like Java's UTF-8 encoder
// ----------------------------------------------------------------------------
static std::string jsonString(const ustr& s) {
    std::string r = "\"";
    char buf[16];
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            snprintf(buf, sizeof(buf), "\\u%04x\\u%04x", (unsigned)c, (unsigned)s[i + 1]);
            r += buf;
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            r += '?';
        } else if (c == u'"' || c == u'\\') {
            r += '\\';
            r += (char)c;
        } else if (c < 0x20 || c >= 0x7F) {
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
            r += buf;
        } else {
            r += (char)c;
        }
    }
    return r + "\"";
}

static std::string scanSummary(const Player* player) {
    std::string s;
    auto add = [&](const std::string& t) {
        if (!s.empty()) s += ' ';
        s += t;
    };
    for (int col = 0; col < C::COLORS_PER_FISH; col++) {
        for (int type = 0; type < C::N_TYPES; type++) {
            Scan scan = {0, type, col};
            int code = playerScanned(player, scan) ? 2 : isFishScannedByPlayerDrone(scan, player) ? 1 : hasFishEscaped(scan) ? 3 : 0;
            add(std::to_string(code));
            add(G.firstToScan[col][type] == player->index ? "@" : "_");
        }
        add(G.firstToScanAllFishOfColor[col] == player->index ? "@" : "_");
    }
    for (int type = 0; type < C::N_TYPES; type++) add(G.firstToScanAllFishOfType[type] == player->index ? "@" : "_");
    return s;
}

// Serializer.serializeFrameData (the viewer data of a frame), for the trace only. The frame is
// written when the jar dumps it: in the first execution of the next turn, or at the end.
static std::string pendingFrame;
static bool newTurn = false;

static void flushFrame() {
    if (trace && !pendingFrame.empty()) fprintf(trace, "V %s\n", pendingFrame.c_str());
    pendingFrame.clear();
}

static void traceFrame() {
    if (!trace) return;
    ustr out;
    auto add = [&](const std::string& t) {
        if (!out.empty()) out += u';';
        out += java::ascii(t);
    };
    add(std::to_string(G.fishes.size()));
    for (Fish* fish : G.fishes)
        add(std::to_string(fish->id) + " " + toIntString(fish->pos) + " " + toIntString(fish->speed) + " " + (fish->isFleeing ? "1" : "0"));
    for (Ugly* ugly : G.uglies)
        add(std::to_string(ugly->id) + " " + toIntString(ugly->pos) + " " + toIntString(ugly->speed) + " " + (ugly->foundTarget ? "1" : "0"));
    for (Player* player : G.players) {
        add(std::to_string(player->points));
        for (Drone* drone : player->drones) {
            add(std::to_string(drone->id) + " " + toIntString(drone->pos) + " " + (drone->hasMove ? toIntString(drone->move) : "x x") + " " +
                std::to_string(drone->battery) + " " + (drone->lightOn ? "1" : "0") + " " + (drone->dead ? "1" : "0") + " " +
                std::to_string(java::d2i(drone->dieAt * 100)) + " " + (drone->didReport ? "1" : "0") + " ");
            out += drone->message;
            add(std::to_string(drone->fishesScannedThisTurn.size()));
            for (int id : drone->fishesScannedThisTurn) add(std::to_string(id));
            add(std::to_string(drone->scans.size()));
            for (const Scan& scan : drone->scans) add(std::to_string(scan.color) + " " + std::to_string(scan.type));
        }
        add(scanSummary(player));
    }
    pendingFrame = jsonString(out);
}

// ----------------------------------------------------------------------------
// CommandManager.java
// ----------------------------------------------------------------------------
struct InvalidInput {
    ustr message;
};

// Game.getExpected
static ustr getExpected(const ustr& playerOutput) {
    if (java::upperStartsWith(playerOutput, "MOVE")) return u"MOVE <x> <y> <light (1|0)>";
    if (java::upperStartsWith(playerOutput, "WAIT")) return u"WAIT <light (1|0)>";
    return u"MOVE | WAIT";
}

static InvalidInput invalidInput(const ustr& expected, const ustr& got) {
    return {u"Invalid Input: Expected " + expected + u" but got '" + got + u"'"};
}

static ustr numberFormatException(const ustr& s) { return u"java.lang.NumberFormatException: For input string: \"" + s + u"\""; }

struct Matched {
    int kind = 0; // 0: none, 1: MOVE, 2: WAIT
    ustr x, y;
    int light = 0;
    bool hasMessage = false;
    ustr message;
};

// (?:\s+(?<message>.+))?$ after the light switch; the line never contains a line terminator
static bool matchMessageTail(const ustr& s, size_t i, Matched& m) {
    if (i == s.size()) return true;
    if (!java::isSpace(s[i])) return false;
    size_t j = i;
    while (j < s.size() && java::isSpace(s[j])) j++;
    if (j == s.size()) return false; // .+ needs a character (impossible after trim)
    m.hasMessage = true;
    m.message = s.substr(j);
    return true;
}

static bool matchLiteral(const ustr& s, size_t& i, const char* lit) {
    for (const char* p = lit; *p; p++, i++) {
        if (i >= s.size()) return false;
        if (*p == ' ') {
            if (s[i] != u' ') return false;
        } else if (!java::eqIgnoreCaseAscii(s[i], *p)) {
            return false;
        }
    }
    return true;
}

// -?\d+ (greedy; the next pattern element is a space)
static bool matchNumber(const ustr& s, size_t& i, ustr& out) {
    size_t a = i;
    if (i < s.size() && s[i] == u'-') i++;
    size_t d = i;
    while (i < s.size() && java::isDigit(s[i])) i++;
    if (i == d) return false;
    out = s.substr(a, i - a);
    return true;
}

// ActionType patterns (CASE_INSENSITIVE), matches() on command.trim()
static Matched matchCommand(const ustr& s) {
    Matched m;
    {
        size_t i = 0;
        Matched t;
        if (matchLiteral(s, i, "MOVE ") && matchNumber(s, i, t.x) && matchLiteral(s, i, " ") && matchNumber(s, i, t.y) &&
            matchLiteral(s, i, " ") && i < s.size() && (s[i] == u'0' || s[i] == u'1')) {
            t.light = s[i] - u'0';
            i++;
            if (matchMessageTail(s, i, t)) {
                t.kind = 1;
                return t;
            }
        }
    }
    {
        size_t i = 0;
        Matched t;
        if (matchLiteral(s, i, "WAIT ") && i < s.size() && (s[i] == u'0' || s[i] == u'1')) {
            t.light = s[i] - u'0';
            i++;
            if (matchMessageTail(s, i, t)) {
                t.kind = 2;
                return t;
            }
        }
    }
    return m;
}

static void setMessage(Drone* drone, const ustr& message) {
    drone->message = message;
    if (message.size() > 48) drone->message = message.substr(0, 46) + u"...";
}

static void parseCommands(Player* player, const std::vector<ustr>& lines) {
    try {
        size_t droneIdx = 0;
        for (const ustr& command : lines) {
            Matched m = matchCommand(java::trim(command));
            if (m.kind == 0) throw invalidInput(getExpected(command), command);
            auto getDrone = [&]() -> Drone* {
                if (droneIdx >= player->drones.size())
                    throw invalidInput(getExpected(command), java::ascii("java.lang.IndexOutOfBoundsException: Index " + std::to_string(droneIdx) +
                                                                         " out of bounds for length " + std::to_string(player->drones.size())));
                return player->drones[droneIdx];
            };
            if (m.kind == 1) {
                i32 x, y;
                if (!java::parseInt(m.x, x)) throw invalidInput(getExpected(command), numberFormatException(m.x));
                if (!java::parseInt(m.y, y)) throw invalidInput(getExpected(command), numberFormatException(m.y));
                Drone* drone = getDrone();
                drone->hasMove = true;
                drone->move = {(double)x, (double)y};
                drone->lightSwitch = m.light == 1;
                if (m.hasMessage) setMessage(drone, m.message);
            } else {
                Drone* drone = getDrone();
                drone->lightSwitch = m.light == 1;
                if (m.hasMessage) setMessage(drone, m.message);
            }
            droneIdx++;
        }
    } catch (const InvalidInput& e) {
        // deactivatePlayer
        player->active = false;
        addTooltip(player->index, java::replace(java::replace(e.message, u"&lt;", u"<"), u"&gt;", u">"));
        player->score = -1;
        addToGameSummary(e.message);
        addToGameSummary(u"\u00A4RED\u00A4$" + java::ascii(std::to_string(player->index)) + u": disqualified!\u00A7RED\u00A7");
    }
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
    HANDLE hStderrR = nullptr;   // read child's stderr
    bool alive = false;
    std::string readBuf; // bytes read but not consumed yet
    size_t readPos = 0;

    bool start(const std::string& cmd) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;
        HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
        // stdin is plain synchronous, only the writer thread writes (blocking) to it.
        if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
        // stderr: the same pipe size as Java's ProcessImpl (PIPE_SIZE = 4096 + 24), see readError
        HANDLE errR = nullptr, errW = nullptr;
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
        hStdinW = inW;
        hStdoutR = outR;
        hStderrR = errR;
        alive = true;
        startTime = std::chrono::steady_clock::now();
        startWriter();
        return true;
    }

    typedef HANDLE Handle;
    Handle inputHandle() const { return hStdinW; }

    // Blocking write of all of s (called from the writer thread). False once the pipe is broken.
    static bool rawWrite(Handle h, const std::string& s) {
        size_t off = 0;
        DWORD wr;
        while (off < s.size()) {
            if (!WriteFile(h, s.data() + off, (DWORD)(s.size() - off), &wr, nullptr) || wr == 0) return false;
            off += wr;
        }
        return true;
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
                // the OVERLAPPED is freed. The game ends after this turn anyway.
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
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
            CloseHandle(hProcess);
        }
        stopWriter(); // hStdinW stays open: the writer thread may still use it until we exit
        if (hStdoutR) CloseHandle(hStdoutR);
        if (hReadEvent) CloseHandle(hReadEvent);
        if (hStderrR) CloseHandle(hStderrR);
        hProcess = nullptr;
        hStdoutR = nullptr;
        hReadEvent = nullptr;
        hStderrR = nullptr;
        alive = false;
    }

    // Agent#readError: if bytes are available, one read of at most 4096 bytes (a FileInputStream on
    // Windows). An agent writing more than that per turn eventually blocks on its stderr.
    void readError() {
        DWORD avail = 0;
        if (!hStderrR || !PeekNamedPipe(hStderrR, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
        char tmp[4096];
        DWORD rd = 0;
        ReadFile(hStderrR, tmp, avail < sizeof(tmp) ? avail : (DWORD)sizeof(tmp), &rd, nullptr);
    }
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <wordexp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <cerrno>

// ----------------------------------------------------------------------------
// Child process I/O (POSIX: macOS / Linux)
// ----------------------------------------------------------------------------
struct ChildProc {
    pid_t pid = -1;
    int fdStdinW = -1;
    int fdStdoutR = -1;
    int fdStderrR = -1;
    std::string errBuf; // java.io.BufferedInputStream buffer of the stderr stream (ProcessPipeInputStream)
    size_t errPos = 0;
    bool alive = false;
    std::string readBuf; // bytes read but not consumed yet
    size_t readPos = 0;

    bool start(const std::string& cmd) {
        int inPipe[2], outPipe[2], errPipe[2];
        if (pipe(inPipe) != 0) return false;
        if (pipe(outPipe) != 0) {
            close(inPipe[0]);
            close(inPipe[1]);
            return false;
        }
        if (pipe(errPipe) != 0) {
            close(inPipe[0]);
            close(inPipe[1]);
            close(outPipe[0]);
            close(outPipe[1]);
            return false;
        }
        int errFd = errPipe[1];

        pid_t c = fork();
        if (c < 0) {
            close(errFd);
            close(errPipe[0]);
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
            close(errPipe[0]);
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
        fdStderrR = errPipe[0];
        pid = c;
        alive = true;
        startTime = std::chrono::steady_clock::now();
        startWriter();
        return true;
    }

    typedef int Handle;
    Handle inputHandle() const { return fdStdinW; }

    // Blocking write of all of s (called from the writer thread). False once the pipe is broken.
    static bool rawWrite(Handle fd, const std::string& s) {
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
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
        stopWriter(); // fdStdinW stays open: the writer thread may still use it until we exit
        if (fdStdoutR >= 0) {
            close(fdStdoutR);
            fdStdoutR = -1;
        }
        if (fdStderrR >= 0) {
            close(fdStderrR);
            fdStderrR = -1;
        }
        alive = false;
    }

    int stderrAvailable() const {
        int n = 0;
        if (fdStderrR < 0 || ioctl(fdStderrR, FIONREAD, &n) < 0) return 0;
        return n;
    }

    // Agent#readError: if bytes are available, read(tmp, 0, 4096) on a BufferedInputStream(8192)
    // (ProcessPipeInputStream on Unix). An agent writing more than 4096 bytes per turn eventually
    // blocks on its stderr.
    void readError() {
        if ((errBuf.size() - errPos) + (size_t)stderrAvailable() == 0) return;
        size_t want = 4096, got = 0;
        while (true) {
            if (errPos >= errBuf.size()) { // fill()
                errBuf.assign(8192, 0);
                errPos = 0;
                ssize_t n;
                do {
                    n = read(fdStderrR, &errBuf[0], errBuf.size());
                } while (n < 0 && errno == EINTR);
                errBuf.resize(n > 0 ? (size_t)n : 0);
                if (n <= 0) break;
            }
            size_t take = std::min(errBuf.size() - errPos, want - got);
            errPos += take;
            got += take;
            if (got >= want || stderrAvailable() <= 0) break;
        }
    }
#endif

    // GameRunner hands each input to an AsynchronousWriter thread, so the referee never blocks on
    // an agent that does not read its input. A write error only stops the writer.
    struct Writer {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::string> q;
        bool stop = false;
    };
    Writer* writer = nullptr; // never freed: the detached thread may outlive the game

    void startWriter() {
        writer = new Writer();
        Writer* w = writer;
        Handle h = inputHandle();
        std::thread([w, h] {
            std::unique_lock<std::mutex> lk(w->m);
            while (true) {
                w->cv.wait(lk, [w] { return w->stop || !w->q.empty(); });
                if (w->stop) break;
                std::string s = std::move(w->q.front());
                w->q.pop_front();
                lk.unlock();
                bool ok = rawWrite(h, s);
                lk.lock();
                if (!ok) break;
            }
        }).detach();
    }

    void send(const std::string& s) {
        if (!writer) return;
        {
            std::lock_guard<std::mutex> g(writer->m);
            writer->q.push_back(s);
        }
        writer->cv.notify_one();
    }

    void stopWriter() {
        if (!writer) return;
        {
            std::lock_guard<std::mutex> g(writer->m);
            writer->stop = true;
            writer->q.clear();
        }
        writer->cv.notify_one();
    }

    bool lastCR = false;
    std::chrono::steady_clock::time_point startTime;

    // Agent#getOutput: reads until `nbLine` line terminators ('\n', '\r', "\r\n" counts once) or
    // AGENT_MAX_BUFFER_SIZE bytes, before `deadline`. Extra bytes are kept for the next turn.
    // Returns false when the runner reports a timeout (also on EOF and on too long outputs).
    bool getOutput(int nbLine, std::chrono::steady_clock::time_point deadline, std::string& out) {
        out.clear();
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

    // GameRunner#getNextPlayerOutput + sendPlayerOutput: sends the input and reads the expected
    // amount of lines. Returns false on timeout, otherwise the lines sent to the referee.
    // The jar launches the agents before the JVM sets up the referee, ~0.2 s to 1 s before their
    // first input, which absorbs a slow agent startup. We send the first input right away, so the
    // first turn ends at the latest of 1000 ms after the input and 1000 ms after
    // FIRST_TURN_HEAD_START past the launch of the agent.
    bool execute(const std::string& input, int nbLines, int timeoutMs, bool firstTurn, std::vector<ustr>& lines) {
        send(input);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        if (firstTurn) deadline = std::max(deadline, startTime + std::chrono::milliseconds(C::FIRST_TURN_HEAD_START + timeoutMs));
        std::string output;
        bool ok = getOutput(nbLines, deadline, output);
        readError();
        if (!ok) return false;
        ustr s = java::decodeUTF8(output);
        for (auto& ch : s)
            if (ch == u'\r') ch = u'\n';
        // split("(\n|\r\n)", -1), first nbLines pieces
        lines.clear();
        size_t off = 0, p;
        while ((int)lines.size() < nbLines && (p = s.find(u'\n', off)) != ustr::npos) {
            lines.push_back(s.substr(off, p - off));
            off = p + 1;
        }
        return true;
    }
};

// ----------------------------------------------------------------------------
// Engine 4.4.2 protocol between the runner and the referee (GameRunner / GameManager)
// ----------------------------------------------------------------------------
// The Java referee died: the runner stops and the CLI prints "null" scores
struct RefereeCrash {
    std::string what;
};

static std::deque<ustr> refereeIn; // the referee's stdin, as java.util.Scanner#nextLine returns it
static ChildProc procs[2];

// The runner writes lines to the referee: Scanner also ends lines at U+0085, U+2028 and U+2029
static void runnerSend(const ustr& line) {
    size_t off = 0;
    for (size_t i = 0; i < line.size(); i++) {
        if (java::isScannerSeparator(line[i])) {
            refereeIn.push_back(line.substr(off, i - off));
            off = i + 1;
        }
    }
    refereeIn.push_back(line.substr(off));
}

static ustr refereeReadLine() {
    if (refereeIn.empty()) {
        // the referee waits for a line the runner never sends; the runner gives up after 150 s
        throw RefereeCrash{"java.lang.RuntimeException: Invalid Referee command: "};
    }
    ustr s = refereeIn.front();
    refereeIn.pop_front();
    return s;
}

enum Cmd { INIT, GET_GAME_INFO, SET_PLAYER_OUTPUT, SET_PLAYER_TIMEOUT };

// InputCommand.parse: "\[\[(?<cmd>.+)\] ?(?<lineCount>[0-9]+)\]", Command.valueOf, Integer.parseInt
static Cmd parseHeader(const ustr& line, i32& lineCount) {
    const char* bad = "java.lang.RuntimeException: Error in data sent to referee";
    if (line.size() < 2 || line[0] != u'[' || line[1] != u'[' || line.back() != u']') throw RefereeCrash{bad};
    ustr body = line.substr(2, line.size() - 3);
    size_t j = body.rfind(u']');
    if (j == ustr::npos || j == 0) throw RefereeCrash{bad};
    size_t k = j + 1;
    if (k < body.size() && body[k] == u' ') k++;
    if (k >= body.size()) throw RefereeCrash{bad};
    for (size_t q = k; q < body.size(); q++)
        if (!java::isDigit(body[q])) throw RefereeCrash{bad};
    ustr cmd = body.substr(0, j);
    Cmd c;
    if (cmd == u"INIT") c = INIT;
    else if (cmd == u"GET_GAME_INFO") c = GET_GAME_INFO;
    else if (cmd == u"SET_PLAYER_OUTPUT") c = SET_PLAYER_OUTPUT;
    else if (cmd == u"SET_PLAYER_TIMEOUT") c = SET_PLAYER_TIMEOUT;
    else throw RefereeCrash{"java.lang.IllegalArgumentException: No enum constant com.codingame.gameengine.core.InputCommand.Command"};
    if (!java::parseInt(body.substr(k), lineCount)) throw RefereeCrash{"java.lang.NumberFormatException"};
    return c;
}

static const char* cmdName(Cmd c) {
    switch (c) {
        case INIT: return "INIT";
        case GET_GAME_INFO: return "GET_GAME_INFO";
        case SET_PLAYER_OUTPUT: return "SET_PLAYER_OUTPUT";
        default: return "SET_PLAYER_TIMEOUT";
    }
}

static int traceTurn = 0;

// GameManager.execute(player) and the runner's handling of the frame it produces
static void execute(Player* player) {
    player->timeout = false;
    i32 lineCount;
    Cmd c = parseHeader(refereeReadLine(), lineCount);
    if (c != GET_GAME_INFO) throw RefereeCrash{std::string("java.lang.RuntimeException: Invalid command: ") + cmdName(c)};
    if (newTurn) flushFrame(); // dumpView: KEY_FRAME

    // the runner gets the frame (input, NEXT_PLAYER_INFO) and runs the player
    int nbLines = player->getExpectedOutputLines();
    int timeoutMs = player->hasNeverBeenExecuted ? C::FIRST_TURN_MAX_TIME : C::TURN_MAX_TIME;
    std::vector<ustr> lines;
    bool ok = procs[player->index].execute(player->inputs, nbLines, timeoutMs, player->hasNeverBeenExecuted, lines);
    if (trace) {
        if (ok) {
            fprintf(trace, "T%d P%d OUT [", traceTurn, player->index);
            for (size_t i = 0; i < lines.size(); i++) fprintf(trace, "%s%s", i ? "," : "", jsonString(lines[i]).c_str());
            fprintf(trace, "]\n");
        } else {
            fprintf(trace, "T%d P%d TIMEOUT\n", traceTurn, player->index);
        }
    }
    if (ok) {
        runnerSend(java::ascii("[[SET_PLAYER_OUTPUT] " + std::to_string(nbLines) + "]"));
        for (const ustr& l : lines) runnerSend(l);
    } else {
        runnerSend(u"[[SET_PLAYER_TIMEOUT] 0]");
    }
    runnerSend(u"[[GET_GAME_INFO] 0]"); // next GameRunner.readGameInfo

    // READ PLAYER OUTPUTS
    c = parseHeader(refereeReadLine(), lineCount);
    if (c == SET_PLAYER_OUTPUT) {
        std::vector<ustr> output;
        for (i32 i = 0; i < lineCount; i++) output.push_back(refereeReadLine());
        player->outputs = output;
    } else if (c == SET_PLAYER_TIMEOUT) {
        player->timeout = true;
    } else {
        throw RefereeCrash{std::string("java.lang.RuntimeException: Invalid command: ") + cmdName(c)};
    }
    player->inputs.clear();
    player->hasNeverBeenExecuted = false;
    newTurn = false;
}

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
static void resetGameTurnData() {
    for (Player* p : G.players) {
        for (Drone* d : p->drones) {
            d->hasMove = false;
            d->fishesScannedThisTurn.clear();
            d->didReport = false;
            d->message.clear();
        }
    }
}

static void handlePlayerCommands() {
    for (Player* player : getActivePlayers()) {
        if (player->timeout) {
            player->active = false;
            addTooltip(player->index, u"Timeout!");
            addToGameSummary(java::ascii("$" + std::to_string(player->index) + " has not provided " +
                                         std::to_string(player->getExpectedOutputLines()) + " lines in time"));
        } else {
            parseCommands(player, player->outputs);
        }
    }
}

static void gameTurn(int turn) {
    resetGameTurnData();
    traceTurn = turn;
    for (Player* player : getActivePlayers()) {
        serializeFrameInfoFor(player);
        execute(player);
    }
    handlePlayerCommands();
    G.players[0]->outputs.clear();
    G.players[1]->outputs.clear();

    performGameUpdate(turn);

    if (getActivePlayers().size() < 2) gameEnd = true;
}

// GameManager.start
static bool runGame() {
    // Referee.init
    for (Player* player : getActivePlayers()) serializeGlobalInfoFor(player);
    traceFrame();

    runnerSend(u"[[GET_GAME_INFO] 0]");
    try {
        for (int turn = 1; turn <= C::MAX_TURNS && !gameEnd && !getActivePlayers().empty(); turn++) {
            currentGameSummary.clear();
            currentTooltips.clear();
            newTurn = true;
            gameTurn(turn);
            traceFrame();
        }
    } catch (const RefereeCrash& crash) {
        fprintf(stderr, "%s\n", crash.what.c_str());
        return false;
    }
    onEnd();
    if (!lastFrameReadable()) {
        fprintf(stderr, "java.lang.RuntimeException: Error reading Referee command. Buffer capacity: 30000 / 30000\n");
        return false;
    }
    flushFrame();
    return true;
}

// Long.parseLong (ASCII digits)
static bool parseLong(const char* s, i64& out) {
    size_t n = strlen(s), i = 0;
    if (n == 0) return false;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
        if (n == 1) return false;
    }
    u64 limit = neg ? (u64)INT64_MAX + 1 : (u64)INT64_MAX;
    u64 v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        u64 d = (u64)(s[i] - '0');
        if (v > (limit - d) / 10) return false;
        v = v * 10 + d;
    }
    out = neg ? (i64)(0 - v) : (i64)v;
    return true;
}

int main(int argc, char** argv) {
    // CommandLineInterface (commons-cli DefaultParser): -h, -p1..-p4, -l, -seed
    std::string cmds[4];
    bool has[4] = {false, false, false, false};
    bool help = false;
    const char* seedArg = nullptr;
    const char* tracePath = nullptr;
    auto isOption = [](const std::string& t) {
        return t == "-h" || t == "-l" || t == "-seed" || t == "-p1" || t == "-p2" || t == "-p3" || t == "-p4";
    };
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h") {
            help = true;
        } else if (isOption(a)) {
            if (i + 1 >= argc || isOption(argv[i + 1])) {
                fprintf(stderr, "org.apache.commons.cli.MissingArgumentException: Missing argument for option: %s\n", a.c_str() + 1);
                return 1;
            }
            const char* v = argv[++i];
            if (a == "-seed") seedArg = v;
            else if (a == "-l") tracePath = v;
            else {
                cmds[a[2] - '1'] = v;
                has[a[2] - '1'] = true;
            }
        } else if (a.size() > 1 && a[0] == '-') {
            fprintf(stderr, "org.apache.commons.cli.UnrecognizedOptionException: Unrecognized option: %s\n", a.c_str());
            return 1;
        }
    }
    if (help || !has[0] || !has[1]) {
        printf("usage: -p1 <player1 command line> -p2 <player2 command line> [-p3 <cmd>\n"
               "           -p4 <cmd> -l <log file> -seed <seed>]\n"
               " -h            Print the help\n"
               " -l <arg>      File output for logs\n"
               " -p1 <arg>     Required. Player 1 command line\n"
               " -p2 <arg>     Required. Player 2 command line\n"
               " -p3 <arg>     Player 3 command line (if applies)\n"
               " -p4 <arg>     Player 4 command line (if applies)\n"
               " -seed <arg>   Referee seed\n");
        return 0;
    }
    i64 seed = (i64)std::chrono::steady_clock::now().time_since_epoch().count();
    if (seedArg && !parseLong(seedArg, seed)) {
        fprintf(stderr, "java.lang.NumberFormatException: For input string: \"%s\"\n", seedArg);
        return 1;
    }
    if (has[2] || has[3]) {
        fprintf(stderr, "only 2 players are supported\n");
        return 1;
    }
    if (tracePath) trace = fopen(tracePath, "w");

    for (int i = 0; i < 2; i++) players[i].index = i;
    gameInit(seed);

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    for (int i = 0; i < 2; i++) {
        if (!procs[i].start(cmds[i])) {
            fprintf(stderr, "java.lang.RuntimeException: Failed to launch %s\n", cmds[i].c_str());
            procs[0].kill();
            return 1;
        }
    }
    for (int i = 0; i < 2; i++) procs[i].readError(); // GameRunner.readInitFrameErrors

    bool ok = runGame();
    if (trace) {
        fprintf(trace, "S %d %d\n", players[0].score, players[1].score);
        fclose(trace);
    }
    procs[0].kill();
    procs[1].kill();
    if (!ok) {
        // the runner gave up: GameResult.scores is empty and the CLI prints null for each player
        printf("null\nnull\n");
        return 0;
    }
    printf("%d\n%d\n", players[0].score, players[1].score);
    return 0;
}
