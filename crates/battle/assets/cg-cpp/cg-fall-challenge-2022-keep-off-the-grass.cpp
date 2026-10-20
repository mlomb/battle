// Keep Off the Grass (CodinGame Fall Challenge 2022) - headless C++ referee, port of
// https://github.com/CodinGame/FallChallenge2022-KeepOffTheGrass (commit 7694269), on the CodinGame
// game engine 4.4.0 (runner + CommandLineInterface-v4, league level 19), 2 players.
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints "<p0score>\n<p1score>\n" to stdout (matches the jar). When the Java referee crashes, the
// runner stops the game and the jar prints "null\nnull\n" and exits 0: so do we.
// -l writes a trace: one "T<turn> P<player> OUT <json array>" / "T<turn> P<player> TIMEOUT" line per
// player execution, plus view/score lines (KOTG_TRACE_VIEWS=1 adds the view JSON of every frame).
// With -p3/-p4 the Java referee fails during init (2 spawn points only): null scores, like the jar.
//
// The Java referee is followed very closely (same data structures where order matters, same
// floating point operations in the same order). Notable Java behaviours reproduced here:
//   - gameManager.getRandom(): SecureRandom("SHA1PRNG") seeded with setSeed(long), used through
//     java.util.Random's nextDouble / nextBoolean / nextInt(bound) and Collections.shuffle
//   - the map generation (Math.pow, island bridging that does not recompute islands after a bridge)
//   - the A* used by MOVE: every node has the same heuristic, so the java.util.PriorityQueue tie order
//     decides the path; int overflow of manhattan distances for huge MOVE targets
//   - a disqualified player's BUILDs that precede the invalid command are still applied (doBuilds
//     iterates all players), and recyclers of inactive players keep working
//   - engine I/O (Agent#getOutput): '\r', '\n' and "\r\n" (once, even across turns) end a line,
//     10000 bytes max, Java UTF-8 decoding (U+FFFD for malformed bytes), 1000 ms for the first turn
//     of each player and 50 ms afterwards, asynchronous writes to the agents' stdin
//   - the jar launches the agents, then starts its referee (JVM/Guice start-up, ~0.5 s) before their
//     first input, so an agent's start-up time hardly counts: the first turn's 1000 ms run from the
//     latest of the input being sent and 500 ms after the agent's launch (wall clock, approximate)
//   - command parsing: String.split(";"), trim(), the ActionType regexes, Integer.valueOf overflow
//   - the referee reads player lines with java.util.Scanner.nextLine(), which also splits at
//     U+0085, U+2028 and U+2029: the referee only sees the text before it, and the rest of the line
//     is read as the next engine command at the next player execution, which crashes the referee
//   - engine limits that make the Java referee crash (null scores): view data over 1 MiB in total
//     (GameManager.dumpView, counted in UTF-16 chars with the platform line separator), and a
//     referee command over 30000 bytes (RefereeAgent buffer: a view frame, the game summary or the
//     tooltips, which repeat a disqualified player's invalid command)
// Not reproduced:
//   - -seed 0: SecureRandom.setSeed(0) is ignored by Java, so the jar self-seeds (random map); we
//     use a random seed too
//   - Math.pow (HotSpot intrinsic) and the C library's pow differ by 1 ulp in ~0.05% of the calls of
//     the map generation; a map only changes if the value is within 1 ulp of a durability threshold
//     (about 1e-15 per cell; 340k maps compared with the jar's Grid were identical)
//   - agents' stderr is discarded (the jar reads at most 4 KB of it per turn, an agent writing much
//     more could block in the jar)
//   - protocol injection: a player line whose part after U+2028 (etc.) is a valid engine command

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
#include <charconv>
#include <chrono>
#include <random>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <system_error>

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

// int arithmetic (wraps around)
static inline i32 add(i32 a, i32 b) { return (i32)((u32)a + (u32)b); }
static inline i32 sub(i32 a, i32 b) { return (i32)((u32)a - (u32)b); }
static inline i32 abs(i32 a) { return a < 0 ? (i32)(0u - (u32)a) : a; } // Math.abs(MIN_VALUE) == MIN_VALUE

// SHA-1
static void sha1(const u8* msg, size_t n, u8 out[20]) {
    auto rol = [](u32 v, int b) { return (v << b) | (v >> (32 - b)); };
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

// java.security.SecureRandom("SHA1PRNG") (sun.security.provider.SecureRandom) used through the
// java.util.Random API, like MultiplayerGameManager.getRandom()
struct SecureRandom {
    u8 state[20];
    bool haveState = false;
    u8 remainder[20] = {0};
    int remCount = 0;

    void engineSetSeed(const u8* seed, size_t n) {
        std::vector<u8> msg;
        if (haveState) msg.insert(msg.end(), state, state + 20);
        msg.insert(msg.end(), seed, seed + n);
        sha1(msg.data(), msg.size(), state);
        haveState = true;
        remCount = 0;
    }
    // SecureRandom.setSeed(long): little-endian bytes; 0 is ignored (the object stays unseeded)
    void setSeed(i64 seed) {
        if (seed == 0) return;
        u8 b[8];
        for (int i = 0; i < 8; i++) b[i] = (u8)((u64)seed >> (8 * i));
        engineSetSeed(b, 8);
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
    void nextBytes(u8* result, int n) {
        if (!haveState) {
            // unseeded (seed 0): Java seeds itself from system entropy, so the game is random
            std::random_device rd;
            u8 seed[20];
            u64 t = (u64)std::chrono::high_resolution_clock::now().time_since_epoch().count();
            for (int i = 0; i < 20; i++) seed[i] = (u8)(rd() ^ (t >> (i % 8 * 8)));
            sha1(seed, 20, state);
            haveState = true;
        }
        int index = 0;
        u8 output[20];
        memcpy(output, remainder, 20);
        int r = remCount;
        if (r > 0) {
            int todo = std::min(n - index, 20 - r);
            for (int i = 0; i < todo; i++) {
                result[i] = output[r];
                output[r++] = 0;
            }
            remCount += todo;
            index += todo;
        }
        while (index < n) {
            sha1(state, 20, output);
            updateState(state, output);
            int todo = std::min(n - index, 20);
            for (int i = 0; i < todo; i++) {
                result[index++] = output[i];
                output[i] = 0;
            }
            remCount += todo;
        }
        memcpy(remainder, output, 20);
        remCount %= 20;
    }
    i32 next(int numBits) {
        int numBytes = (numBits + 7) / 8;
        u8 b[4];
        nextBytes(b, numBytes);
        u32 nxt = 0;
        for (int i = 0; i < numBytes; i++) nxt = (nxt << 8) + b[i];
        return (i32)(nxt >> (numBytes * 8 - numBits));
    }
    // Random.nextInt(bound)
    i32 nextInt(i32 bound) {
        i32 r = next(31);
        i32 m = bound - 1;
        if ((bound & m) == 0) return (i32)(((i64)bound * (i64)r) >> 31);
        for (i32 u = r; add(sub(u, r = u % bound), m) < 0; u = next(31)) {
        }
        return r;
    }
    // Random.nextDouble()
    double nextDouble() { return (double)(((i64)next(26) << 27) + next(27)) * 0x1.0p-53; }
    // Random.nextBoolean()
    bool nextBoolean() { return next(1) != 0; }
};

// java.lang.String (UTF-16 code units)
typedef std::u16string jstring;

static jstring J(const char* ascii) {
    jstring s;
    for (const char* p = ascii; *p; p++) s += (char16_t)(unsigned char)*p;
    return s;
}
static jstring J(const std::string& ascii) { return J(ascii.c_str()); }

// new String(bytes, UTF_8) (String.decodeUTF8_UTF16 of JDK 17+): malformed input becomes U+FFFD
static jstring decodeUtf8(const std::string& in) {
    const int sl = (int)in.size();
    auto src = [&](int i) -> int { return (int)(int8_t)in[i]; };
    auto isNotContinuation = [](int b) { return (b & 0xc0) != 0x80; };
    auto isMalformed3 = [](int b1, int b2, int b3) {
        return (b1 == (int)(int8_t)0xe0 && (b2 & 0xe0) == 0x80) || (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80;
    };
    auto isMalformed3_2 = [](int b1, int b2) { return (b1 == (int)(int8_t)0xe0 && (b2 & 0xe0) == 0x80) || (b2 & 0xc0) != 0x80; };
    auto isMalformed4 = [](int b2, int b3, int b4) { return (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80 || (b4 & 0xc0) != 0x80; };
    auto isMalformed4_2 = [](int b1, int b2) {
        return (b1 == 0xf0 && (b2 < 0x90 || b2 > 0xbf)) || (b1 == 0xf4 && (b2 & 0xf0) != 0x80) || (b2 & 0xc0) != 0x80;
    };
    auto isMalformed4_3 = [](int b3) { return (b3 & 0xc0) != 0x80; };
    auto malformed3 = [&](int sp) {
        int b1 = src(sp++);
        int b2 = src(sp);
        return ((b1 == (int)(int8_t)0xe0 && (b2 & 0xe0) == 0x80) || isNotContinuation(b2)) ? 1 : 2;
    };
    auto malformed4 = [&](int sp) {
        int b1 = src(sp++) & 0xff;
        int b2 = src(sp++) & 0xff;
        if (b1 > 0xf4 || (b1 == 0xf0 && (b2 < 0x90 || b2 > 0xbf)) || (b1 == 0xf4 && (b2 & 0xf0) != 0x80) || isNotContinuation(b2))
            return 1;
        if (isNotContinuation(src(sp))) return 2;
        return 3;
    };
    const char16_t REPL = 0xfffd;
    jstring dst;
    dst.reserve(sl);
    int sp = 0;
    while (sp < sl) {
        int b1 = src(sp++);
        if (b1 >= 0) {
            dst += (char16_t)b1;
        } else if ((b1 >> 5) == -2 && (b1 & 0x1e) != 0) {
            if (sp < sl) {
                int b2 = src(sp++);
                if (isNotContinuation(b2)) {
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
                int b2 = src(sp++);
                int b3 = src(sp++);
                if (isMalformed3(b1, b2, b3)) {
                    dst += REPL;
                    sp -= 3;
                    sp += malformed3(sp);
                } else {
                    char16_t c = (char16_t)((b1 << 12) ^ (b2 << 6) ^
                                            (b3 ^ (((int)(int8_t)0xE0 << 12) ^ ((int)(int8_t)0x80 << 6) ^ ((int)(int8_t)0x80 << 0))));
                    if (c >= 0xd800 && c <= 0xdfff) dst += REPL;
                    else dst += c;
                }
                continue;
            }
            if (sp < sl && isMalformed3_2(b1, src(sp))) {
                dst += REPL;
                continue;
            }
            dst += REPL;
            break;
        } else if ((b1 >> 3) == -2) {
            if (sp + 2 < sl) {
                int b2 = src(sp++);
                int b3 = src(sp++);
                int b4 = src(sp++);
                int uc = ((b1 << 18) ^ (b2 << 12) ^ (b3 << 6) ^
                          (b4 ^ (((int)(int8_t)0xF0 << 18) ^ ((int)(int8_t)0x80 << 12) ^ ((int)(int8_t)0x80 << 6) ^ ((int)(int8_t)0x80 << 0))));
                if (isMalformed4(b2, b3, b4) || !(uc >= 0x10000 && uc < 0x110000)) {
                    dst += REPL;
                    sp -= 4;
                    sp += malformed4(sp);
                } else {
                    dst += (char16_t)((uc >> 10) + (0xd800 - (0x10000 >> 10)));
                    dst += (char16_t)((uc & 0x3ff) + 0xdc00);
                }
                continue;
            }
            b1 &= 0xff;
            if (b1 > 0xf4 || (sp < sl && isMalformed4_2(b1, src(sp) & 0xff))) {
                dst += REPL;
                continue;
            }
            sp++;
            dst += REPL;
            if (sp < sl && isMalformed4_3(src(sp))) continue;
            break;
        } else {
            dst += REPL;
        }
    }
    return dst;
}

static inline bool isHighSurrogate(char16_t c) { return c >= 0xd800 && c <= 0xdbff; }
static inline bool isLowSurrogate(char16_t c) { return c >= 0xdc00 && c <= 0xdfff; }

// Bytes written for s by a UTF-8 encoder with replacement (PrintStream, String.getBytes): an unpaired
// surrogate becomes '?'
static i64 utf8Length(const jstring& s) {
    i64 n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (isHighSurrogate(c) && i + 1 < s.size() && isLowSurrogate(s[i + 1])) { n += 4; i++; }
        else if (isHighSurrogate(c) || isLowSurrogate(c)) n += 1;
        else n += 3;
    }
    return n;
}

// UTF-8 like Java's encoder with replacement (unpaired surrogates become '?'), for the trace
static std::string toUtf8(const jstring& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (isHighSurrogate((char16_t)c) && i + 1 < s.size() && isLowSurrogate(s[i + 1])) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            i++;
        } else if (isHighSurrogate((char16_t)c) || isLowSurrogate((char16_t)c)) {
            c = '?';
        }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xc0 | (c >> 6)); out += (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out += (char)(0xe0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3f)); out += (char)(0x80 | (c & 0x3f)); }
        else {
            out += (char)(0xf0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3f));
            out += (char)(0x80 | ((c >> 6) & 0x3f)); out += (char)(0x80 | (c & 0x3f));
        }
    }
    return out;
}

// String.trim(): removes code units <= ' ' at both ends
static jstring trim(const jstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] <= u' ') a++;
    while (b > a && s[b - 1] <= u' ') b--;
    return s.substr(a, b - a);
}

// String.split(c) for a single non-regex char: trailing empty strings are removed
static std::vector<jstring> split(const jstring& s, char16_t c) {
    std::vector<jstring> r;
    size_t off = 0, next;
    while ((next = s.find(c, off)) != jstring::npos) {
        r.push_back(s.substr(off, next - off));
        off = next + 1;
    }
    if (off == 0) return {s};
    r.push_back(s.substr(off));
    while (!r.empty() && r.back().empty()) r.pop_back();
    return r;
}

// String.replace(CharSequence, CharSequence)
static jstring replace(const jstring& s, const jstring& from, const jstring& to) {
    jstring out;
    size_t off = 0, pos;
    while ((pos = s.find(from, off)) != jstring::npos) {
        out.append(s, off, pos - off);
        out += to;
        off = pos + from.size();
    }
    out.append(s, off, jstring::npos);
    return out;
}

// Integer.parseInt for a string that matches -?[0-9]+. Returns false on overflow (NumberFormatException).
static bool parseInt(const jstring& s, i32& out) {
    size_t i = 0;
    bool neg = false;
    if (s[0] == u'-') { neg = true; i = 1; }
    i64 v = 0;
    for (; i < s.size(); i++) {
        v = v * 10 + (s[i] - u'0');
        if (v > 2147483648LL) return false;
    }
    if (neg) v = -v;
    if (v > INT32_MAX) return false;
    out = (i32)v;
    return true;
}

// String.toUpperCase() (default locale en_US) of the first code points of s, enough for startsWith
// checks of up to `n` chars. Only the non-ASCII chars whose upper case starts with an ASCII letter
// matter here (table generated with JDK 26).
static jstring upperPrefix(const jstring& s, size_t n) {
    static const struct { char16_t c; const char16_t* up; } TABLE[] = {
        {0x00df, u"SS"}, {0x0131, u"I"}, {0x017f, u"S"}, {0x01f0, u"J̌"}, {0x1e96, u"H̱"},
        {0x1e97, u"T̈"}, {0x1e98, u"W̊"}, {0x1e99, u"Y̊"}, {0x1e9a, u"Aʾ"},
        {0xfb00, u"FF"}, {0xfb01, u"FI"}, {0xfb02, u"FL"}, {0xfb03, u"FFI"}, {0xfb04, u"FFL"},
        {0xfb05, u"ST"}, {0xfb06, u"ST"},
    };
    jstring out;
    for (size_t i = 0; i < s.size() && out.size() < n; i++) {
        char16_t c = s[i];
        if (c >= u'a' && c <= u'z') { out += (char16_t)(c - 32); continue; }
        if (c < 0x80) { out += c; continue; }
        const char16_t* up = nullptr;
        for (auto& t : TABLE)
            if (t.c == c) up = t.up;
        if (up) out += up;
        else out += (char16_t)0xffff; // some non-ASCII char: no keyword can match from here
    }
    return out;
}

// Length (in UTF-16 chars, and in UTF-8 bytes) of a string once quoted by Gson 2.8.2's JsonWriter
// (not HTML-safe), without the quotes
static void jsonEscapedLength(const jstring& s, i64& chars, i64& bytes) {
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        int len;
        if (c < 0x20) len = (c == u'\t' || c == u'\b' || c == u'\n' || c == u'\r' || c == u'\f') ? 2 : 6;
        else if (c == u'"' || c == u'\\') len = 2;
        else if (c == 0x2028 || c == 0x2029) len = 6;
        else len = 1;
        if (len > 1 || c < 0x80) { chars += len; bytes += len; continue; }
        chars += 1;
        if (c < 0x800) bytes += 2;
        else if (isHighSurrogate(c) && i + 1 < s.size() && isLowSurrogate(s[i + 1])) { bytes += 4; chars += 1; i++; }
        else if (isHighSurrogate(c) || isLowSurrogate(c)) bytes += 1;
        else bytes += 3;
    }
}

// Gson 2.8.2 JsonWriter.string() escaping (not HTML-safe), without the quotes, as UTF-8
static std::string gsonEscape(const jstring& s) {
    std::string out;
    jstring plain;
    for (char16_t c : s) {
        const char* rep = nullptr;
        char buf[8];
        if (c == u'"') rep = "\\\"";
        else if (c == u'\\') rep = "\\\\";
        else if (c == u'\t') rep = "\\t";
        else if (c == u'\b') rep = "\\b";
        else if (c == u'\n') rep = "\\n";
        else if (c == u'\r') rep = "\\r";
        else if (c == u'\f') rep = "\\f";
        else if (c < 0x20 || c == 0x2028 || c == 0x2029) {
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
            rep = buf;
        }
        if (rep) {
            out += toUtf8(plain);
            plain.clear();
            out += rep;
        } else {
            plain += c;
        }
    }
    out += toUtf8(plain);
    return out;
}

// JSON string for the trace
static std::string jsonQuote(const jstring& s) { return "\"" + gsonEscape(s) + "\""; }

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
    u64 v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        if (v > (u64)INT64_MAX / 10 + 1) return false;
        v = v * 10 + (u64)(s[i] - '0');
        if (v > (u64)INT64_MAX + 1) return false;
    }
    if (!neg && v > (u64)INT64_MAX) return false;
    out = neg ? (i64)(0 - v) : (i64)v;
    return true;
}

} // namespace java

using java::jstring;

// ----------------------------------------------------------------------------
// Constants (Config.java, league level 19, and the engine)
// ----------------------------------------------------------------------------
namespace C {
const int MAP_MIN_WIDTH = 12;
const int MAP_MAX_WIDTH = 24; // 15 in league 1 only
const double MAP_ASPECT_RATIO = 1 / 2.0;
const int MIN_SPAWN_DISTANCE = 7;
const int CELL_MAX_DURABILITY = 10;
const int RECYCLER_INCOME = 1;
const int PLAYER_STARTING_MONEY = 10;
const int PLAYER_MINIMAL_INCOME = 10;
const int COST_UNIT = 10;
const int COST_RECYCLER = 10;
const int MAX_TURNS = 200;
const int EARLY_FINISH_TURNS = 20;

// engine
const int AGENT_MAX_BUFFER_SIZE = 10000;
const int FIRST_TURN_MAX_TIME = 1000;
const int TURN_MAX_TIME = 50;
const int AGENT_HEAD_START = 500; // time between launching the agents and their first input in the jar
const i64 VIEW_DATA_TOTAL_HARD_QUOTA = 1024 * 1024;
const i64 GAME_SUMMARY_TOTAL_HARD_QUOTA = 512 * 1024;
const i64 GAME_SUMMARY_PER_TURN_HARD_QUOTA = 800;
const i64 REFEREE_MAX_BUFFER_SIZE = 30000;
#ifdef _WIN32
const int LINE_SEPARATOR_LENGTH = 2; // System.lineSeparator() of the JVM, used by OutputData.toString
#else
const int LINE_SEPARATOR_LENGTH = 1;
#endif
} // namespace C

// ----------------------------------------------------------------------------
// Coord.java, Cell.java, Unit.java, Recycler.java
// ----------------------------------------------------------------------------
struct Coord {
    i32 x, y;
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    Coord add(const Coord& d) const { return {java::add(x, d.x), java::add(y, d.y)}; }
    i32 manhattanTo(i32 ox, i32 oy) const { return java::add(java::abs(java::sub(ox, x)), java::abs(java::sub(oy, y))); }
    i32 manhattanTo(const Coord& o) const { return manhattanTo(o.x, o.y); }
    double sqrEuclideanTo(double ox, double oy) const {
        // Math.pow(v, 2) is exact here (v is a small multiple of 0.5)
        double dx = ox - x, dy = oy - y;
        return dx * dx + dy * dy;
    }
};

static const Coord FOUR[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}; // Adjacency.FOUR

struct Cell {
    int durability = 0;
    int owner = -1; // player index, -1: none
    bool isHole() const { return durability == 0; }
    void garanteeNotHole() {
        if (isHole()) durability = 1;
    }
    bool damage() {
        durability--;
        if (durability == 0) {
            owner = -1;
            return true;
        }
        return false;
    }
};

struct Unit {
    bool valid = false; // an entry exists in Player.units (getUnitAt returns NO_UNIT otherwise)
    i32 availableCount = 0;
    i32 unavailableCount = 0;
};

struct Recycler {
    Coord coord;
    int owner;
};

// ----------------------------------------------------------------------------
// Grid.java
// ----------------------------------------------------------------------------
struct Grid {
    int width = 0, height = 0;
    bool ySymetry = false, snaky = false;
    std::vector<Cell> cells; // y * width + x, every coord of the map is in Grid.cells
    std::vector<std::vector<Coord>> spawns;
    java::SecureRandom* random = nullptr;

    bool isValid(const Coord& c) const { return c.x >= 0 && c.y >= 0 && c.x < width && c.y < height; }
    int idx(const Coord& c) const { return c.y * width + c.x; }
    // Grid.get: NO_CELL (invalid, durability 0, no owner) outside of the map. NO_CELL is never modified.
    Cell get(const Coord& c) const { return isValid(c) ? cells[idx(c)] : Cell(); }
    Cell& at(const Coord& c) { return cells[idx(c)]; }
    bool isHole(const Coord& c) const { return get(c).isHole(); }
    bool isOwner(const Coord& c, int player) const { return isValid(c) && cells[idx(c)].owner == player; }

    Coord opposite(const Coord& c) const { return {width - c.x - 1, ySymetry ? (height - c.y - 1) : c.y}; }

    int getNeighbours(const Coord& pos, Coord out[4]) const {
        int n = 0;
        for (const Coord& d : FOUR) {
            Coord c = pos.add(d);
            if (isValid(c)) out[n++] = c;
        }
        return n;
    }

    int randInt(int from, int to) { return random->nextInt(to - from) + from; }
    int randInt(int to) { return randInt(0, to); }

    int randomDurability(int x, int y, const Coord& centre) {
        double dist = Coord{x, y}.manhattanTo(centre);
        double maxDist = centre.manhattanTo(0, 0);
        double d = random->nextDouble();
        d *= d;
        double n = 1 / (1 + (1 - (dist / maxDist)) * 3);
        d = std::pow(d, n);
        if (d < .15) return 0;
        if (d < .35) return snaky ? 0 : 4;
        if (d < .5) return 6;
        if (d < .8) return 8;
        if (d < .9) return 9;
        return C::CELL_MAX_DURABILITY;
    }

    void generate(java::SecureRandom& rnd, int nPlayers) {
        random = &rnd;
        snaky = random->nextDouble() < 0.1;
        width = randInt(C::MAP_MIN_WIDTH, C::MAP_MAX_WIDTH + 1);
        height = (int)(width * C::MAP_ASPECT_RATIO);
        ySymetry = random->nextBoolean();

        cells.assign(width * height, Cell());
        std::vector<char> present(width * height, 0);
        std::vector<Coord> possibleSpawnPoints;
        std::vector<Coord> emptyCells; // Stack<Coord>

        Coord centre = {width / 2, height / 2};
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                Coord coord = {x, y};
                if (present[idx(coord)]) continue;
                int durability = randomDurability(x, y, centre);
                at(coord).durability = durability;
                present[idx(coord)] = 1;
                if (durability == 0) emptyCells.push_back(coord);
                Coord opp = opposite(coord);
                if (opp != coord) {
                    at(opp).durability = durability;
                    present[idx(opp)] = 1;
                    if (durability == C::CELL_MAX_DURABILITY && x > 1 && y > 1 && x < width - 1 && y < height - 1 &&
                        coord.manhattanTo(opp) >= C::MIN_SPAWN_DISTANCE && coord.manhattanTo(opp) >= (width * 0.5))
                        possibleSpawnPoints.push_back(coord);
                }
            }
        }

        spawns.clear();
        if (possibleSpawnPoints.empty()) possibleSpawnPoints.push_back({1, 1});
        int cellIdx = randInt((int)possibleSpawnPoints.size());
        Coord spawnPoint = possibleSpawnPoints[cellIdx];
        Coord opp = opposite(spawnPoint);
        Coord spawnPoints[2];
        if (random->nextBoolean()) { spawnPoints[0] = spawnPoint; spawnPoints[1] = opp; }
        else { spawnPoints[0] = opp; spawnPoints[1] = spawnPoint; }

        for (int playerIdx = 0; playerIdx < nPlayers; ++playerIdx) {
            std::vector<Coord> unitSpawns;
            Coord spawn = spawnPoints[playerIdx];
            Cell& spawnCentre = at(spawn);
            spawnCentre.owner = playerIdx;
            spawnCentre.garanteeNotHole();
            for (const Coord& d : FOUR) {
                Coord c = spawn.add(d);
                Cell& cell = at(c);
                cell.garanteeNotHole();
                cell.owner = playerIdx;
                unitSpawns.push_back(c);
            }
            spawns.push_back(unitSpawns);
        }

        // emptyCells.removeAll(all unit spawns)
        emptyCells.erase(std::remove_if(emptyCells.begin(), emptyCells.end(),
                                        [&](const Coord& c) {
                                            for (auto& l : spawns)
                                                for (auto& s : l)
                                                    if (s == c) return true;
                                            return false;
                                        }),
                         emptyCells.end());
        fixIslands(emptyCells);
    }

    // Islands are disjoint sets of coords: island[cell] is the id of the set containing the cell, -1 if
    // none. Islands are not recomputed after a bridge, so bridges belong to no island.
    std::vector<int> island;
    int islandCount = 0;
    int nextIslandId = 0;

    void detectIslands() {
        // the iteration order of cells.keySet() does not matter: islands are the connected components
        island.assign(width * height, -1);
        islandCount = 0;
        nextIslandId = 0;
        std::vector<Coord> fifo;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                Coord p = {x, y};
                if (get(p).isHole() || island[idx(p)] >= 0) continue;
                int id = nextIslandId++;
                islandCount++;
                fifo.clear();
                fifo.push_back(p);
                island[idx(p)] = id;
                for (size_t h = 0; h < fifo.size(); h++) {
                    Coord e = fifo[h];
                    for (const Coord& d : FOUR) {
                        Coord n = e.add(d);
                        if (isValid(n) && !get(n).isHole() && island[idx(n)] < 0) {
                            island[idx(n)] = id;
                            fifo.push_back(n);
                        }
                    }
                }
            }
        }
    }

    bool closeIslandGap(std::vector<Coord>& emptyCells) {
        std::vector<int> connecting;
        bool found = false;
        Coord bridge{0, 0};
        for (const Coord& coord : emptyCells) {
            Coord neighs[4];
            int nn = getNeighbours(coord, neighs);
            connecting.clear();
            for (int i = 0; i < nn; i++) {
                int id = island[idx(neighs[i])];
                if (id >= 0 && std::find(connecting.begin(), connecting.end(), id) == connecting.end()) connecting.push_back(id);
            }
            if (connecting.size() > 1) {
                bridge = coord;
                found = true;
                break;
            }
        }
        if (!found) return false;
        Coord coord = bridge;
        Coord opp = opposite(coord);
        at(coord).durability = 6;
        at(opp).durability = 6;
        auto removeFirst = [&](const Coord& c) {
            auto it = std::find(emptyCells.begin(), emptyCells.end(), c);
            if (it != emptyCells.end()) emptyCells.erase(it);
        };
        removeFirst(coord);
        removeFirst(opp);
        int id = nextIslandId++;
        for (int& v : island)
            if (v >= 0 && std::find(connecting.begin(), connecting.end(), v) != connecting.end()) v = id;
        islandCount = islandCount - (int)connecting.size() + 1;
        return true;
    }

    void fixIslands(std::vector<Coord>& emptyCells) {
        // Collections.shuffle(emptyCells, random)
        for (int i = (int)emptyCells.size(); i > 1; i--) std::swap(emptyCells[i - 1], emptyCells[random->nextInt(i)]);
        detectIslands();
        while (islandCount > 1) {
            if (!closeIslandGap(emptyCells)) break;
        }
    }
};

// ----------------------------------------------------------------------------
// AStar.java / PathFinder.java
// ----------------------------------------------------------------------------
struct PathItem {
    int cumulativeLength = 0;
    int totalPrevisionalLength = 0;
    int precedent = -1;
    Coord coord;
};

struct AStar {
    const Grid& grid;
    const std::vector<char>& restricted; // per cell: a recycler is there
    Coord from, target, nearest;
    double centreX, centreY;
    std::vector<PathItem> items;
    std::vector<int> openList; // java.util.PriorityQueue (binary heap) ordered by totalPrevisionalLength
    std::vector<char> closed;

    AStar(const Grid& g, Coord f, Coord t, const std::vector<char>& r) : grid(g), restricted(r), from(f), target(t), nearest(f) {
        centreX = grid.width / 2.0;
        centreY = grid.height / 2.0;
    }

    int cmp(int a, int b) const {
        int x = items[a].totalPrevisionalLength, y = items[b].totalPrevisionalLength;
        return x < y ? -1 : (x == y ? 0 : 1);
    }
    void offer(int e) {
        int k = (int)openList.size();
        openList.push_back(e);
        while (k > 0) {
            int parent = (k - 1) >> 1;
            int p = openList[parent];
            if (cmp(e, p) >= 0) break;
            openList[k] = p;
            k = parent;
        }
        openList[k] = e;
    }
    int poll() {
        int result = openList[0];
        int n = (int)openList.size() - 1;
        int x = openList[n];
        openList.pop_back();
        if (n > 0) {
            int k = 0, half = n >> 1;
            while (k < half) {
                int child = (k << 1) + 1;
                int c = openList[child];
                int right = child + 1;
                if (right < n && cmp(c, openList[right]) > 0) c = openList[child = right];
                if (cmp(x, c) <= 0) break;
                openList[k] = c;
                k = child;
            }
            openList[k] = x;
        }
        return result;
    }

    int compareByDistanceToCentre(const Coord& a, const Coord& b) const {
        double da = a.sqrEuclideanTo(centreX, centreY), db = b.sqrEuclideanTo(centreX, centreY);
        return da < db ? -1 : (da > db ? 1 : 0);
    }

    // returns the index of the item that reached the target, -1 if not found
    int getPathItemLinkedList() {
        closed.assign(grid.width * grid.height, 0);
        items.push_back(PathItem());
        items.back().coord = from;
        offer(0);
        while (!openList.empty()) {
            int visiting = poll();
            Coord visitingCoord = items[visiting].coord;
            if (visitingCoord == target) return visiting;
            if (closed[grid.idx(visitingCoord)]) continue;
            closed[grid.idx(visitingCoord)] = 1;

            Coord neighbors[4];
            int nn = grid.getNeighbours(visitingCoord, neighbors);
            // Collections.sort (stable) by distance to the centre
            std::stable_sort(neighbors, neighbors + nn, [&](const Coord& a, const Coord& b) { return compareByDistanceToCentre(a, b) < 0; });
            for (int i = 0; i < nn; i++) {
                const Coord& neighbor = neighbors[i];
                if (!grid.get(neighbor).isHole() && !restricted[grid.idx(neighbor)]) addToOpenList(visiting, visitingCoord, neighbor);
            }

            i32 visitingDist = visitingCoord.manhattanTo(target);
            i32 nearestDist = nearest.manhattanTo(target);
            if (visitingDist < nearestDist) {
                nearest = visitingCoord;
            } else if (visitingDist == nearestDist) {
                if (compareByDistanceToCentre(visitingCoord, nearest) < 0) nearest = visitingCoord;
            }
        }
        return -1;
    }

    void addToOpenList(int visiting, const Coord& fromCoord, const Coord& toCoord) {
        if (closed[grid.idx(toCoord)]) return;
        PathItem pi;
        pi.coord = toCoord;
        pi.cumulativeLength = items[visiting].cumulativeLength + 1; // default weight function
        int manh = fromCoord.manhattanTo(toCoord);
        pi.totalPrevisionalLength = pi.cumulativeLength + manh;
        pi.precedent = visiting;
        items.push_back(pi);
        offer((int)items.size() - 1);
    }

    std::vector<Coord> find() {
        int item = getPathItemLinkedList();
        std::vector<Coord> path;
        for (int i = item; i >= 0; i = items[i].precedent) path.push_back(items[i].coord);
        std::reverse(path.begin(), path.end());
        return path;
    }
};

// PathFinder.findPath(): the path to the target, or to the visited coord nearest to it
static std::vector<Coord> findPath(const Grid& grid, const std::vector<char>& restricted, Coord from, Coord to) {
    AStar a(grid, from, to, restricted);
    std::vector<Coord> path = a.find();
    if (path.empty()) {
        AStar b(grid, from, a.nearest, restricted);
        path = b.find();
    }
    return path;
}

// ----------------------------------------------------------------------------
// Player.java, Action.java
// ----------------------------------------------------------------------------
enum ActionType { A_MOVE, A_SPAWN, A_BUILD, A_MESSAGE, A_WAIT };

struct Action {
    ActionType type;
    i32 amount = 0;
    Coord coord{0, 0};
    Coord originCoord{0, 0};
};

struct Player {
    int index = 0;
    bool active = true;
    int score = 0;
    bool hasNeverBeenExecuted = true;
    int money = 0;
    int warpCooldown = 0;
    std::vector<Unit> units; // per cell
    bool hasMessage = false;
    jstring message;
    std::vector<Action> builds, spawns, moves;

    jstring nicknameToken() const { return java::J("$" + std::to_string(index)); }

    void setMessage(const jstring& msg) {
        jstring trimmed = java::trim(msg);
        if (trimmed.size() > 48) trimmed = trimmed.substr(0, 46) + u"...";
        if (trimmed.size() > 0) {
            message = trimmed;
            hasMessage = true;
        }
    }
    void reset() {
        hasMessage = false;
        message.clear();
        moves.clear();
        builds.clear();
        spawns.clear();
    }
    void addAction(const Action& a, const jstring& msg) {
        switch (a.type) {
        case A_BUILD: builds.push_back(a); break;
        case A_MOVE: moves.push_back(a); break;
        case A_SPAWN: spawns.push_back(a); break;
        case A_MESSAGE: setMessage(msg); break;
        default: break;
        }
    }
};

// ----------------------------------------------------------------------------
// View (ViewModule, Serializer, Animation, EventData) - only its size matters, for the engine limits
// ----------------------------------------------------------------------------
namespace Ev {
const int BUILD = 0, MOVE = 1, SPAWN = 3, FIGHT = 4, CELL_DAMAGE = 7, RECYCLER_FALL = 8, CELL_OWNER_SWAP = 9,
          UNIT_FALL = 10, MATTER_COLLECT = 11;
const int NONE = INT_MIN;
const int HUNDREDTH = 10, TWENTIETH = 50, TENTH = 100, THIRD = 300, HALF = 500, WHOLE = 1000;
} // namespace Ev

struct Animation {
    int frameTime = 0, endTime = 0;
    void reset() { frameTime = 0; endTime = 0; }
    void wait(int t) { frameTime += t; }
    void startAnim(int& start, int& end, int duration) {
        start = frameTime;
        end = frameTime + duration;
        endTime = std::max(endTime, frameTime + duration);
    }
    void setFrameTime(int t) { frameTime = t; }
    void catchUp() { frameTime = endTime; }
};

struct EventData {
    int type;
    int playerIndex = Ev::NONE, amount = Ev::NONE;
    bool hasCoord = false, hasTarget = false;
    Coord coord{0, 0}, target{0, 0};
    int start = 0, end = 0;
};

// The view data of a frame (GameManager's JsonObject for the frame)
struct FrameView {
    bool hasDuration = false;
    int duration = 0;
    bool hasGraphics = false;
    // Serializer.serialize(FrameViewData) is graphics[0] + message 0 + graphics[1] + message 1 + graphics[2]
    std::string graphics[3];
    bool hasMessage[2] = {false, false};
    jstring messages[2];
    bool hasEndScreen = false;
    int endScores[2] = {0, 0};

    // the frame JSON (JsonObject.toString), without the messages unless `withMessages`
    std::string json(bool withMessages) const {
        std::string s = "{";
        bool first = true;
        if (hasDuration) {
            s += "\"duration\":" + std::to_string(duration);
            first = false;
        }
        if (hasGraphics) {
            if (!first) s += ",";
            s += "\"graphics\":\"" + graphics[0];
            if (withMessages && hasMessage[0]) s += java::gsonEscape(messages[0]);
            s += graphics[1];
            if (withMessages && hasMessage[1]) s += java::gsonEscape(messages[1]);
            s += graphics[2] + "\"";
            first = false;
        }
        if (hasEndScreen) {
            if (!first) s += ",";
            s += "\"endScreen\":[[" + std::to_string(endScores[0]) + "," + std::to_string(endScores[1]) + "],\"logo.png\",null]";
        }
        s += "}";
        return s;
    }
    // size of the escaped messages in the JSON, in UTF-16 chars and in UTF-8 bytes
    void messageLength(i64& chars, i64& bytes) const {
        for (int i = 0; i < 2; i++)
            if (hasMessage[i]) java::jsonEscapedLength(messages[i], chars, bytes);
    }
};

// ----------------------------------------------------------------------------
// Game.java, GameSummaryManager.java
// ----------------------------------------------------------------------------
struct RefereeCrash {
    std::string what;
};

struct Engine;

struct Game {
    Engine* engine = nullptr;
    java::SecureRandom random;
    Grid grid;
    std::vector<Recycler> recyclers;
    std::vector<char> recyclerAt; // per cell: a recycler is there (getRecyclerAt(coord).isPresent())
    Player players[2];
    std::vector<Coord> fightLocations; // HashSet<Coord>: iteration order does not matter here
    std::vector<char> isFightLocation;
    std::vector<EventData> viewerEvents;
    Animation animation;
    int gameTurn = 0;
    int earlyFinishCounter = C::EARLY_FINISH_TURNS;
    // GameSummaryManager: first error and error count per player
    std::string firstError[2];
    int errorCount[2] = {0, 0};

    int cellCount() const { return grid.width * grid.height; }

    void init(i64 seed) {
        random.setSeed(seed);
        for (int i = 0; i < 2; i++) players[i].index = i;
        grid.generate(random, 2);
        recyclers.clear();
        recyclerAt.assign(cellCount(), 0);
        isFightLocation.assign(cellCount(), 0);
        gameTurn = 0;
        for (Player& player : players) {
            player.units.assign(cellCount(), Unit());
            player.money = C::PLAYER_STARTING_MONEY;
            for (const Coord& coord : grid.spawns[player.index]) {
                Unit& u = player.units[grid.idx(coord)];
                u.valid = true;
                u.availableCount = 1;
                u.unavailableCount = 0;
            }
        }
    }

    void resetEarlyTurnCounter() { earlyFinishCounter = C::EARLY_FINISH_TURNS; }

    void resetGameTurnData() {
        viewerEvents.clear();
        animation.reset();
        for (Player& p : players) p.reset();
        for (const Coord& c : fightLocations) isFightLocation[grid.idx(c)] = 0;
        fightLocations.clear();
    }

    void addFightLocation(const Coord& c) {
        if (!isFightLocation[grid.idx(c)]) {
            isFightLocation[grid.idx(c)] = 1;
            fightLocations.push_back(c);
        }
    }

    const Unit& getUnitAt(const Player& p, const Coord& c) const {
        static const Unit NO_UNIT;
        return grid.isValid(c) ? p.units[grid.idx(c)] : NO_UNIT;
    }

    bool hasRecyclerAt(const Coord& c) const { return grid.isValid(c) && recyclerAt[grid.idx(c)]; }

    void addError(int player, const std::string& error) {
        if (errorCount[player]++ == 0) firstError[player] = error;
    }

    std::string getGlobalInfoFor() const { return std::to_string(grid.width) + " " + std::to_string(grid.height) + "\n"; }

    std::string getCurrentFrameInfoFor(const Player& player) const {
        std::vector<char> inRange(cellCount(), 0);
        for (const Recycler& r : recyclers) {
            Coord n[4];
            int nn = grid.getNeighbours(r.coord, n);
            n[nn++] = r.coord;
            for (int i = 0; i < nn; i++)
                if (!grid.get(n[i]).isHole()) inRange[grid.idx(n[i])] = 1;
        }
        const Player& other = players[1 - player.index];
        std::string out;
        out.reserve(cellCount() * 16 + 16);
        out += std::to_string(player.money) + " " + std::to_string(other.money) + "\n";
        char buf[96];
        for (int y = 0; y < grid.height; ++y) {
            for (int x = 0; x < grid.width; ++x) {
                Coord coord = {x, y};
                const Cell& c = grid.cells[grid.idx(coord)];
                int ownerIdx = c.owner < 0 ? -1 : (c.owner == player.index ? 1 : 0);
                const Unit& myUnit = player.units[grid.idx(coord)];
                const Unit& foeUnit = other.units[grid.idx(coord)];
                bool excavator = recyclerAt[grid.idx(coord)];
                int unitStrength = ownerIdx == 1 ? myUnit.availableCount : foeUnit.availableCount;
                bool canBuildHere = ownerIdx == 1 && !excavator && unitStrength == 0;
                bool canSpawnHere = ownerIdx == 1 && !excavator;
                bool willGetDamaged = inRange[grid.idx(coord)];
                snprintf(buf, sizeof(buf), "%d %d %d %d %d %d %d\n", c.durability, ownerIdx, unitStrength, excavator ? 1 : 0,
                         canBuildHere ? 1 : 0, canSpawnHere ? 1 : 0, willGetDamaged ? 1 : 0);
                out += buf;
            }
        }
        return out;
    }

    void performGameUpdate(int frameIdx);

    int getOwnedCells(int player) const {
        int n = 0;
        for (const Cell& c : grid.cells)
            if (c.owner == player) n++;
        return n;
    }

    // events (only their size matters)
    void launchEvent(int type, int playerIndex, int amount, const Coord* coord, const Coord* target, int duration, int wait) {
        EventData e;
        e.type = type;
        e.playerIndex = playerIndex;
        e.amount = amount;
        if (coord) { e.hasCoord = true; e.coord = *coord; }
        if (target) { e.hasTarget = true; e.target = *target; }
        animation.startAnim(e.start, e.end, duration);
        if (wait) animation.wait(wait);
        viewerEvents.push_back(e);
    }

    void doBuilds() {
        int buildStartTime = animation.frameTime;
        for (Player& player : players) { // all players, also inactive ones
            animation.setFrameTime(buildStartTime);
            for (const Action& build : player.builds) {
                Coord t = build.coord;
                char buf[160];
                if (!grid.isOwner(t, player.index)) {
                    snprintf(buf, sizeof(buf), "tried to build a recycler at (%d, %d), which is not owned by the player", t.x, t.y);
                    addError(player.index, buf);
                } else if (hasRecyclerAt(t)) {
                    snprintf(buf, sizeof(buf), "tried to build a recycler at (%d, %d), into another recycler", t.x, t.y);
                    addError(player.index, buf);
                } else if (getUnitAt(players[0], t).valid || getUnitAt(players[1], t).valid) {
                    snprintf(buf, sizeof(buf), "tried to build a recycler at (%d, %d), where units are already present", t.x, t.y);
                    addError(player.index, buf);
                } else if (player.money < C::COST_RECYCLER) {
                    snprintf(buf, sizeof(buf), "tried to build a recycler at (%d, %d), but has not enough matter", t.x, t.y);
                    addError(player.index, buf);
                } else {
                    recyclers.push_back({t, player.index});
                    recyclerAt[grid.idx(t)] = 1;
                    player.money -= C::COST_RECYCLER;
                    launchEvent(Ev::BUILD, player.index, Ev::NONE, &build.coord, nullptr, 2 * Ev::THIRD, Ev::THIRD);
                }
            }
        }
        animation.catchUp();
    }

    void placeUnits(Player& p, const Coord& target, int amount) {
        Unit& u = p.units[grid.idx(target)];
        u.valid = true;
        u.unavailableCount += amount;
    }

    void doSpawn(const std::vector<int>& active) {
        int spawnStartTime = animation.frameTime;
        for (int pi : active) {
            Player& player = players[pi];
            animation.setFrameTime(spawnStartTime);
            std::vector<std::pair<Coord, int>> actualSpawns; // HashMap: iteration order does not matter here
            for (const Action& spawn : player.spawns) {
                double spawnCost = ((double)C::COST_UNIT) * spawn.amount;
                Coord t = spawn.coord;
                char buf[200];
                if (!grid.isOwner(t, player.index)) {
                    snprintf(buf, sizeof(buf), "tried to spawn %d units at (%d, %d), which is not owned by the player", spawn.amount, t.x, t.y);
                    addError(player.index, buf);
                } else if (hasRecyclerAt(t)) {
                    snprintf(buf, sizeof(buf), "tried to spawn %d units at (%d, %d), into a recycler", spawn.amount, t.x, t.y);
                    addError(player.index, buf);
                } else if (player.money < spawnCost) {
                    snprintf(buf, sizeof(buf), "tried to spawn %d units at (%d, %d), but has not enough matter", spawn.amount, t.x, t.y);
                    addError(player.index, buf);
                } else if (spawn.amount == 0) {
                    snprintf(buf, sizeof(buf), "tried to spawn a null amount of units at (%d, %d)", t.x, t.y);
                    addError(player.index, buf);
                } else {
                    placeUnits(player, t, spawn.amount);
                    player.money = (int)(player.money - spawnCost);
                    bool found = false;
                    for (auto& s : actualSpawns)
                        if (s.first == t) { s.second += spawn.amount; found = true; }
                    if (!found) actualSpawns.push_back({t, spawn.amount});
                }
            }
            for (auto& s : actualSpawns) {
                launchEvent(Ev::SPAWN, player.index, s.second, &s.first, nullptr, 2 * Ev::THIRD, Ev::TENTH);
                addFightLocation(s.first);
            }
        }
    }

    void doMove(const std::vector<int>& active) {
        // restricted: the coords with a recycler (no recycler is built or destroyed during doMove)
        int moveStartTime = animation.frameTime;
        for (int pi : active) {
            Player& player = players[pi];
            animation.setFrameTime(moveStartTime);
            std::vector<std::pair<std::pair<Coord, Coord>, int>> actualMoves; // HashMap<CoordTuple, Integer>
            for (const Action& move : player.moves) {
                Coord origin = move.originCoord;
                Coord target = move.coord;
                const Unit& originUnit = getUnitAt(player, origin);
                char buf[200];
                if (originUnit.availableCount < move.amount) {
                    snprintf(buf, sizeof(buf), "tried to move %d units from (%d, %d) where only %d were available", move.amount, origin.x, origin.y,
                             originUnit.availableCount);
                    addError(player.index, buf);
                } else if (origin == target) {
                    snprintf(buf, sizeof(buf), "tried to move %d units from (%d, %d) to the same tile", move.amount, origin.x, origin.y);
                    addError(player.index, buf);
                } else if (move.amount <= 0) {
                    snprintf(buf, sizeof(buf), "tried to move a null amount of units from (%d, %d)", origin.x, origin.y);
                    addError(player.index, buf);
                } else {
                    std::vector<Coord> wholePath = findPath(grid, recyclerAt, origin, target);
                    if (wholePath.size() > 1) {
                        Coord step = wholePath[1];
                        player.units[grid.idx(origin)].availableCount -= move.amount;
                        placeUnits(player, step, move.amount);
                        bool found = false;
                        for (auto& m : actualMoves)
                            if (m.first.first == origin && m.first.second == step) { m.second += move.amount; found = true; }
                        if (!found) actualMoves.push_back({{origin, step}, move.amount});
                    }
                }
            }
            for (auto& m : actualMoves) {
                launchEvent(Ev::MOVE, player.index, m.second, &m.first.first, &m.first.second, Ev::HALF, Ev::TWENTIETH);
                addFightLocation(m.first.second);
            }
        }
    }

    void removeUnits(Player& p, const Coord& coord, int n) {
        if (n == 0) return;
        Unit& u = p.units[grid.idx(coord)];
        if (!u.valid) return; // compute() on an absent key with a null result: nothing
        if (u.availableCount - n <= 0) u = Unit();
        else u.availableCount -= n;
    }

    void doFights() {
        for (const Coord& coord : fightLocations) {
            Player& player = players[0];
            Player& other = players[1];
            int playerUnitStrength = getUnitAt(player, coord).availableCount;
            int otherUnitStrength = getUnitAt(other, coord).availableCount;
            if (playerUnitStrength > 0 && otherUnitStrength > 0) {
                int winner = 2;
                if (playerUnitStrength > otherUnitStrength) winner = player.index;
                else if (otherUnitStrength > playerUnitStrength) winner = other.index;
                launchEvent(Ev::FIGHT, winner, std::abs(playerUnitStrength - otherUnitStrength), &coord, nullptr, Ev::THIRD, 0);
            }
            removeUnits(player, coord, otherUnitStrength);
            removeUnits(other, coord, playerUnitStrength);
            Cell& cell = grid.at(coord);
            if (playerUnitStrength > otherUnitStrength && cell.owner != player.index) {
                cell.owner = player.index;
                launchEvent(Ev::CELL_OWNER_SWAP, player.index, Ev::NONE, &coord, nullptr, Ev::THIRD, 0);
                resetEarlyTurnCounter();
            } else if (otherUnitStrength > playerUnitStrength && cell.owner != other.index) {
                cell.owner = other.index;
                launchEvent(Ev::CELL_OWNER_SWAP, other.index, Ev::NONE, &coord, nullptr, Ev::THIRD, 0);
                resetEarlyTurnCounter();
            }
        }
    }

    void resetUnits() {
        for (Player& player : players) {
            for (Unit& u : player.units) {
                if (!u.valid) continue;
                u.availableCount += u.unavailableCount;
                u.unavailableCount = 0;
                if (u.availableCount <= 0) u = Unit();
            }
        }
    }

    void doUnits(const std::vector<int>& active) {
        doSpawn(active);
        animation.catchUp();
        doMove(active);
        animation.catchUp();
        resetUnits();
        doFights();
    }

    void doRecycle() {
        std::vector<char> excavated[2] = {std::vector<char>(cellCount(), 0), std::vector<char>(cellCount(), 0)};
        std::vector<Coord> excavatedList[2];
        for (const Recycler& e : recyclers) {
            int nb = 0;
            Coord coords[5];
            int nc = grid.getNeighbours(e.coord, coords);
            coords[nc++] = e.coord;
            for (int i = 0; i < nc; i++) {
                const Coord& coord = coords[i];
                if (!grid.get(coord).isHole()) {
                    if (!excavated[e.owner][grid.idx(coord)]) {
                        nb++;
                        excavated[e.owner][grid.idx(coord)] = 1;
                        excavatedList[e.owner].push_back(coord);
                    }
                }
            }
            if (nb > 0) launchEvent(Ev::MATTER_COLLECT, e.owner, nb * C::RECYCLER_INCOME, &e.coord, nullptr, Ev::WHOLE, Ev::HUNDREDTH);
        }
        for (Player& player : players) player.money += (int)excavatedList[player.index].size() * C::RECYCLER_INCOME;

        std::vector<char> destroyed(cellCount(), 0);
        std::vector<Coord> destroyedCells;
        std::vector<char> done(cellCount(), 0); // distinct()
        for (int p = 0; p < 2; p++) {
            for (const Coord& coord : excavatedList[p]) {
                if (done[grid.idx(coord)]) continue;
                done[grid.idx(coord)] = 1;
                Cell& cell = grid.at(coord);
                bool broken = cell.damage();
                launchEvent(Ev::CELL_DAMAGE, Ev::NONE, cell.durability, &coord, nullptr, Ev::HALF, Ev::HUNDREDTH);
                if (broken) {
                    destroyedCells.push_back(coord);
                    destroyed[grid.idx(coord)] = 1;
                }
                resetEarlyTurnCounter();
            }
        }

        animation.wait(Ev::THIRD);
        animation.catchUp();

        for (const Recycler& r : recyclers)
            if (destroyed[grid.idx(r.coord)]) launchEvent(Ev::RECYCLER_FALL, r.owner, Ev::NONE, &r.coord, nullptr, Ev::THIRD, Ev::TWENTIETH);
        recyclers.erase(std::remove_if(recyclers.begin(), recyclers.end(), [&](const Recycler& r) { return destroyed[grid.idx(r.coord)] != 0; }),
                        recyclers.end());
        for (const Coord& c : destroyedCells) recyclerAt[grid.idx(c)] = 0;

        for (Player& p : players) {
            for (const Coord& coord : destroyedCells) {
                Unit& u = p.units[grid.idx(coord)];
                if (u.valid && u.availableCount > 0) {
                    int strength = u.availableCount;
                    u = Unit();
                    launchEvent(Ev::UNIT_FALL, p.index, strength, &coord, nullptr, Ev::THIRD, Ev::TWENTIETH);
                }
            }
        }
    }

    void doPassiveIncome() {
        for (Player& p : players) p.money += C::PLAYER_MINIMAL_INCOME;
    }

    bool checkGameOver();
    void computeEvents();

    void onEnd() {
        for (Player& p : players) p.score = p.active ? getOwnedCells(p.index) : -1;
    }

    // Serializer.serialize(GlobalViewData)
    std::string serializeGlobal() const {
        std::string s = std::to_string(grid.width) + ";" + std::to_string(grid.height);
        for (int x = 0; x < grid.width; x++)
            for (int y = 0; y < grid.height; y++) {
                const Cell& c = grid.cells[grid.idx({x, y})];
                s += ";" + std::to_string(c.durability) + " " + std::to_string(c.owner);
            }
        for (const Player& p : players) {
            std::string list;
            int n = 0;
            for (int y = 0; y < grid.height; y++)
                for (int x = 0; x < grid.width; x++) {
                    const Unit& u = p.units[grid.idx({x, y})];
                    if (!u.valid) continue;
                    n++;
                    list += ";" + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(u.availableCount);
                }
            s += ";" + std::to_string(n) + list;
        }
        return s;
    }

    // Serializer.serialize(FrameViewData), messages apart
    void serializeFrame(FrameView& f) const {
        for (int i = 0; i < 2; i++) {
            const Player& p = players[i];
            f.graphics[i] = (i ? ";" : "") + std::to_string(p.money) + " " + std::to_string(p.warpCooldown) + " ";
            f.hasMessage[i] = p.hasMessage;
            f.messages[i] = p.message;
        }
        std::string s = ";" + std::to_string(viewerEvents.size());
        for (const EventData& e : viewerEvents) {
            s += ";";
            if (e.playerIndex != Ev::NONE) s += std::to_string(e.playerIndex);
            s += ";";
            if (e.amount != Ev::NONE) s += std::to_string(e.amount);
            s += ";";
            if (e.hasCoord) s += std::to_string(e.coord.x) + " " + std::to_string(e.coord.y);
            s += ";";
            if (e.hasTarget) s += std::to_string(e.target.x) + " " + std::to_string(e.target.y);
            s += ";" + std::to_string(e.type) + ";" + std::to_string(e.start) + ";" + std::to_string(e.end);
        }
        f.graphics[2] = s;
        f.hasGraphics = true;
    }
};

// ----------------------------------------------------------------------------
// Child process I/O (Windows)
// ----------------------------------------------------------------------------
#ifdef _WIN32
struct ChildProc {
    HANDLE hProcess = nullptr;
    HANDLE hStdinW = nullptr;    // write to child's stdin
    HANDLE hStdoutR = nullptr;   // read child's stdout (overlapped)
    HANDLE hReadEvent = nullptr; // manual-reset event for overlapped reads on hStdoutR
    bool alive = false;
    std::string readBuf; // bytes read but not consumed yet
    size_t readPos = 0;

    bool start(const std::string& cmd, HANDLE stderrTarget) {
        spawnTime = std::chrono::steady_clock::now();
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;
        HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
        // stdin is plain synchronous, written by the writer thread.
        if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
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
        alive = true;
        startWriter();
        return true;
    }

    // blocking write, from the writer thread. Returns false when the pipe is broken.
    bool writeBlocking(const std::string& s) {
        size_t off = 0;
        DWORD wr;
        while (off < s.size()) {
            if (!WriteFile(hStdinW, s.data() + off, (DWORD)(s.size() - off), &wr, nullptr) || wr == 0) return false;
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

    void killProcess() {
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
        }
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
    bool alive = false;
    std::string readBuf; // bytes read but not consumed yet
    size_t readPos = 0;

    bool start(const std::string& cmd, const char* stderrPath) {
        spawnTime = std::chrono::steady_clock::now();
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
        pid = c;
        alive = true;
        startWriter();
        return true;
    }

    // blocking write, from the writer thread. Returns false when the pipe is broken.
    bool writeBlocking(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n;
            do {
                n = write(fdStdinW, s.data() + off, s.size() - off);
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

    void killProcess() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
    }
#endif

    // AsynchronousWriter: the engine queues the input of the agent and a thread writes it, so an agent
    // that does not read its input never blocks the referee
    std::thread writer;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool stopWriter = false;

    void startWriter() {
        writer = std::thread([this] {
            for (;;) {
                std::string item;
                {
                    std::unique_lock<std::mutex> lock(mu);
                    cv.wait(lock, [this] { return stopWriter || !queue.empty(); });
                    if (queue.empty()) return;
                    item = std::move(queue.front());
                    queue.pop_front();
                }
                if (!writeBlocking(item)) return; // IOException: the writer stops
            }
        });
    }

    void sendInput(const std::string& s) {
        std::lock_guard<std::mutex> lock(mu);
        queue.push_back(s);
        cv.notify_one();
    }

    void kill() {
        {
            std::lock_guard<std::mutex> lock(mu);
            stopWriter = true;
            queue.clear();
            cv.notify_one();
        }
        killProcess();
        // the writer may still be blocked on a pipe kept open by a grandchild: don't wait for it
        if (writer.joinable()) writer.detach();
#ifdef _WIN32
        if (hProcess) CloseHandle(hProcess);
        hProcess = nullptr;
        if (hStdoutR) CloseHandle(hStdoutR);
        hStdoutR = nullptr;
        if (hReadEvent) CloseHandle(hReadEvent);
        hReadEvent = nullptr;
#else
        if (fdStdoutR >= 0) close(fdStdoutR);
        fdStdoutR = -1;
#endif
        alive = false;
    }

    std::chrono::steady_clock::time_point spawnTime;
    bool lastCR = false;

    // Agent#getOutput: reads until `nbLine` line terminators ('\n', '\r', "\r\n" counts once) or
    // AGENT_MAX_BUFFER_SIZE bytes, within `timeoutMs`. Extra bytes are kept for the next turn.
    // Returns false when the engine would report a timeout (also on EOF and on too long outputs).
    bool getOutput(int nbLine, int timeoutMs, std::string& out) {
        out.clear();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        int nbOccurences = 0;
        while ((int)out.size() < C::AGENT_MAX_BUFFER_SIZE && nbOccurences < nbLine) {
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

    // AbstractPlayer#execute + GameRunner: sends the input and reads the expected amount of lines.
    // Returns false on timeout, otherwise the lines as received by the runner (decoded, '\r' -> '\n').
    bool execute(const std::string& input, int nbLines, int timeoutMs, std::vector<jstring>& outputs) {
        sendInput(input);
        std::string output;
        if (!getOutput(nbLines, timeoutMs, output)) return false;
        jstring s = java::decodeUtf8(output);
        for (auto& ch : s)
            if (ch == u'\r') ch = u'\n';
        outputs.clear();
        size_t off = 0, pos;
        while ((int)outputs.size() < nbLines && (pos = s.find(u'\n', off)) != jstring::npos) {
            outputs.push_back(s.substr(off, pos - off));
            off = pos + 1;
        }
        return true;
    }
};

// ----------------------------------------------------------------------------
// GameManager / MultiplayerGameManager / Referee.java / CommandManager.java
// ----------------------------------------------------------------------------
struct Engine {
    Game game;
    ChildProc procs[2];
    FILE* trace = nullptr;
    bool traceViews = false;

    bool gameEnd = false;
    int turn = 0;
    bool newTurn = false;
    int frame = 0;
    int frameDuration = 1000;
    i64 totalViewDataBytesSent = 0;
    std::string globalGraphics;
    FrameView currentView, prevView;
    std::vector<jstring> currentSummary, prevSummary;
    bool hasPrevSummary = false;
    i64 totalGameSummaryBytes = 0;
    struct Tooltip {
        jstring message;
        int player;
    };
    std::vector<Tooltip> currentTooltips, prevTooltips;
    bool scannerPoisoned = false; // the referee's Scanner holds the rest of a player line
    std::string inputs[2];        // AbstractPlayer.inputs

    std::vector<int> getActivePlayers() const {
        std::vector<int> r;
        for (const Player& p : game.players)
            if (p.active) r.push_back(p.index);
        return r;
    }

    void addToGameSummary(const jstring& summary) {
        i64 total = 0;
        for (const jstring& s : currentSummary) total += (i64)s.size();
        if (total < C::GAME_SUMMARY_PER_TURN_HARD_QUOTA && total + totalGameSummaryBytes < C::GAME_SUMMARY_TOTAL_HARD_QUOTA) {
            currentSummary.push_back(summary);
            totalGameSummaryBytes += total;
        }
    }

    void addTooltip(int player, const jstring& message) { currentTooltips.push_back({message, player}); }

    void deactivate(Player& p, const jstring* reason) {
        p.active = false;
        if (reason) addTooltip(p.index, *reason);
        if (trace) fprintf(trace, "DEACTIVATE P%d %s\n", p.index, reason ? java::toUtf8(*reason).c_str() : "");
    }

    void setFrameDuration(int d) {
        if (frameDuration != d) {
            frameDuration = d;
            currentView.hasDuration = true;
            currentView.duration = d;
        }
    }

    void swapInfoAndViewData() {
        prevView = currentView;
        currentView = FrameView();
        prevSummary = currentSummary;
        hasPrevSummary = true;
        currentSummary.clear();
        prevTooltips = currentTooltips;
        currentTooltips.clear();
    }

    // Bytes the runner reads for a referee command (its content and the line terminator), which must
    // fit in RefereeAgent's 30000 bytes buffer
    static i64 commandBytes(const std::vector<jstring>& lines) {
        jstring content;
        for (const jstring& l : lines) {
            content += l;
            content += u'\n';
        }
        content = java::trim(content);
        return java::utf8Length(content) + 1;
    }

    // GameManager.dumpView + dumpInfos, and the runner reading them
    void dumpViewAndInfos() {
        i64 contentChars, contentBytes;
        std::string head;
        if (newTurn) {
            head = "KEY_FRAME " + std::to_string(frame) + "\n";
            i64 msgChars = 0, msgBytes = 0;
            auto json = [&](bool withMessages) {
                if (turn == 1) return "{\"global\":{\"graphics\":\"" + globalGraphics + "\"},\"frame\":" + prevView.json(withMessages) + "}";
                return prevView.json(withMessages);
            };
            std::string skeleton = json(false);
            prevView.messageLength(msgChars, msgBytes);
            contentChars = (i64)(head.size() + skeleton.size()) + msgChars;
            contentBytes = (i64)(head.size() + skeleton.size()) + msgBytes;
            if (trace && traceViews) fprintf(trace, "VIEW %d %s\n", frame, json(true).c_str());
        } else {
            head = "INTERMEDIATE_FRAME " + std::to_string(frame);
            contentChars = contentBytes = (i64)head.size();
        }
        i64 viewDataLength = 10 /* "[[VIEW] n]" */ + C::LINE_SEPARATOR_LENGTH + contentChars;
        totalViewDataBytesSent += viewDataLength;
        if (trace) fprintf(trace, "V %d %lld %lld %lld\n", frame, (long long)contentChars, (long long)contentBytes, (long long)totalViewDataBytesSent);
        if (totalViewDataBytesSent > C::VIEW_DATA_TOTAL_HARD_QUOTA)
            throw RefereeCrash{"java.lang.RuntimeException: The amount of data sent to the viewer is too big!"};
        frame++;
        if (contentBytes + 1 > C::REFEREE_MAX_BUFFER_SIZE)
            throw RefereeCrash{"java.lang.RuntimeException: Error reading Referee command (VIEW). Buffer capacity: 30000"};
        if (newTurn && hasPrevSummary && commandBytes(prevSummary) > C::REFEREE_MAX_BUFFER_SIZE)
            throw RefereeCrash{"java.lang.RuntimeException: Error reading Referee command (SUMMARY). Buffer capacity: 30000"};
        if (newTurn && !prevTooltips.empty()) {
            std::vector<jstring> lines;
            for (const Tooltip& t : prevTooltips) {
                lines.push_back(t.message);
                lines.push_back(java::J(std::to_string(t.player)));
            }
            if (commandBytes(lines) > C::REFEREE_MAX_BUFFER_SIZE)
                throw RefereeCrash{"java.lang.RuntimeException: Error reading Referee command (TOOLTIP). Buffer capacity: 30000"};
        }
    }

    // GameManager.execute(player): returns false on timeout
    bool execute(Player& player, std::vector<jstring>& outputs) {
        if (scannerPoisoned) throw RefereeCrash{"java.lang.RuntimeException: Error in data sent to referee"};
        dumpViewAndInfos();
        int timeoutMs = player.hasNeverBeenExecuted ? C::FIRST_TURN_MAX_TIME : C::TURN_MAX_TIME;
        if (player.hasNeverBeenExecuted) {
            // the jar launches the agents before starting its referee (JVM and Guice start-up, ~0.5 s), so
            // an agent's own start-up time barely counts in its first turn: the first turn's limit runs
            // from the latest of now and the agent's launch + AGENT_HEAD_START
            auto ready = procs[player.index].spawnTime + std::chrono::milliseconds(C::AGENT_HEAD_START);
            auto now = std::chrono::steady_clock::now();
            if (ready > now) timeoutMs += (int)std::chrono::duration_cast<std::chrono::milliseconds>(ready - now).count();
        }
        bool ok = procs[player.index].execute(inputs[player.index], 1, timeoutMs, outputs);
        inputs[player.index].clear();
        player.hasNeverBeenExecuted = false;
        newTurn = false;
        if (trace) {
            if (ok) {
                fprintf(trace, "T%d P%d OUT [", turn, player.index);
                for (size_t i = 0; i < outputs.size(); i++) fprintf(trace, "%s%s", i ? "," : "", java::jsonQuote(outputs[i]).c_str());
                fprintf(trace, "]\n");
            } else {
                fprintf(trace, "T%d P%d TIMEOUT\n", turn, player.index);
            }
        }
        if (ok) {
            // the referee reads the line with Scanner.nextLine(), which also ends at U+0085, U+2028 and U+2029
            jstring& line = outputs[0];
            for (size_t i = 0; i < line.size(); i++) {
                if (line[i] == 0x0085 || line[i] == 0x2028 || line[i] == 0x2029) {
                    line.resize(i);
                    scannerPoisoned = true;
                    break;
                }
            }
        }
        return ok;
    }

    // Game.getExpected
    static jstring getExpected(const jstring& playerOutput) {
        jstring attempt = java::upperPrefix(playerOutput, 7);
        auto starts = [&](const char16_t* k) { return attempt.compare(0, std::char_traits<char16_t>::length(k), k) == 0; };
        if (starts(u"MOVE")) return u"MOVE <n> <x1> <y1> <x2> <y2>";
        if (starts(u"SPAWN")) return u"SPAWN <n> <x> <y>";
        if (starts(u"BUILD")) return u"BUILD <x> <y>";
        if (starts(u"MESSAGE")) return u"MESSAGE <text>";
        if (starts(u"WAIT")) return u"WAIT";
        return u"MOVE | SPAWN | BUILD | MESSAGE | WAIT";
    }

    // ActionType patterns with Matcher.matches(). Returns 0: no match, 1: action, 2: Integer.valueOf threw
    // (nfe = the string that failed)
    static int matchCommand(const jstring& s, Action& action, jstring& message, jstring& nfe) {
        size_t pos = 0;
        auto lit = [&](const char16_t* k) {
            size_t n = std::char_traits<char16_t>::length(k);
            if (s.compare(pos, n, k) != 0 || pos + n > s.size()) return false;
            pos += n;
            return true;
        };
        // group: (-)?\d+ followed by ' ' (or the end if last)
        auto number = [&](bool allowMinus, bool last, jstring& out) {
            size_t start = pos;
            if (allowMinus && pos < s.size() && s[pos] == u'-') pos++;
            size_t d = pos;
            while (pos < s.size() && s[pos] >= u'0' && s[pos] <= u'9') pos++;
            if (pos == d) return false;
            out = s.substr(start, pos - start);
            if (last) return pos == s.size();
            if (pos >= s.size() || s[pos] != u' ') return false;
            pos++;
            return true;
        };
        jstring g[5];
        int n = 0;
        ActionType type;
        if (lit(u"MOVE ") || (pos = 0, lit(u"WARP "))) {
            type = A_MOVE;
            if (!number(false, false, g[0]) || !number(false, false, g[1]) || !number(false, false, g[2]) || !number(true, false, g[3]) ||
                !number(true, true, g[4]))
                return 0;
            n = 5;
        } else if ((pos = 0, lit(u"SPAWN "))) {
            type = A_SPAWN;
            if (!number(false, false, g[0]) || !number(true, false, g[1]) || !number(true, true, g[2])) return 0;
            n = 3;
        } else if ((pos = 0, lit(u"BUILD "))) {
            type = A_BUILD;
            if (!number(true, false, g[0]) || !number(true, true, g[1])) return 0;
            n = 2;
        } else if ((pos = 0, lit(u"MESSAGE "))) {
            // '.' does not match line terminators, but the referee's line has none (see execute)
            action.type = A_MESSAGE;
            message = s.substr(pos);
            return 1;
        } else if (s == u"WAIT") {
            action.type = A_WAIT;
            return 1;
        } else {
            return 0;
        }
        i32 v[5];
        for (int i = 0; i < n; i++) {
            if (!java::parseInt(g[i], v[i])) {
                nfe = g[i];
                return 2;
            }
        }
        action.type = type;
        if (type == A_MOVE) {
            action.amount = v[0];
            action.originCoord = {v[1], v[2]};
            action.coord = {v[3], v[4]};
        } else if (type == A_SPAWN) {
            action.amount = v[0];
            action.coord = {v[1], v[2]};
        } else {
            action.coord = {v[0], v[1]};
        }
        return 1;
    }

    // CommandManager.parseCommands
    void parseCommands(Player& player, const jstring& line) {
        std::vector<jstring> commands = java::split(line, u';');
        for (const jstring& command : commands) {
            Action action;
            jstring message, nfe;
            int r = matchCommand(java::trim(command), action, message, nfe);
            if (r == 1) {
                player.addAction(action, message);
                continue;
            }
            jstring got = r == 2 ? u"java.lang.NumberFormatException: For input string: \"" + nfe + u"\"" : command;
            jstring msg = u"Invalid Input: Expected " + getExpected(command) + u" but got '" + got + u"'";
            // deactivatePlayer
            jstring reason = java::replace(java::replace(msg, u"&lt;", u"<"), u"&gt;", u">");
            deactivate(player, &reason);
            player.score = -1;
            addToGameSummary(msg);
            addToGameSummary(u"¤RED¤" + player.nicknameToken() + u": disqualified!§RED§");
            return;
        }
    }

    void handlePlayerCommands(const std::vector<int>& active, std::vector<jstring>* outputs, bool* ok) {
        for (int pi : active) {
            Player& player = game.players[pi];
            if (ok[pi]) {
                parseCommands(player, outputs[pi][0]);
            } else {
                jstring reason = u"Timeout!";
                deactivate(player, &reason);
                addToGameSummary(player.nicknameToken() + u" has not provided 1 lines in time");
            }
        }
    }

    // Referee.gameTurn
    void gameTurn() {
        game.resetGameTurnData();
        std::vector<int> active = getActivePlayers();
        std::vector<jstring> outputs[2];
        bool ok[2] = {false, false};
        for (int pi : active) {
            Player& player = game.players[pi];
            inputs[pi] += game.getCurrentFrameInfoFor(player);
            ok[pi] = execute(player, outputs[pi]);
        }
        handlePlayerCommands(active, outputs, ok);
        game.performGameUpdate(turn);
        if (getActivePlayers().size() < 2) gameEnd = true;
    }

    // GameManager.start: returns the exit code (0) after printing the scores, or after printing null
    // scores when the Java referee crashes
    int run(const std::string& bot1, const std::string& bot2, i64 seed) {
        game.engine = this;
        // Referee.init (league 19)
        game.init(seed);
        for (int pi : getActivePlayers()) inputs[pi] += game.getGlobalInfoFor();
        setFrameDuration(500);
        // ViewModule.onGameInit
        globalGraphics = game.serializeGlobal();
        game.serializeFrame(currentView);

#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE devnull = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
        if (!procs[0].start(bot1, devnull) || !procs[1].start(bot2, devnull)) {
            fprintf(stderr, "java.lang.RuntimeException: Failed to launch a player\n");
            procs[0].kill();
            procs[1].kill();
            return 1;
        }
#else
        signal(SIGPIPE, SIG_IGN);
        if (!procs[0].start(bot1, nullptr) || !procs[1].start(bot2, nullptr)) {
            fprintf(stderr, "java.lang.RuntimeException: Failed to launch a player\n");
            procs[0].kill();
            procs[1].kill();
            return 1;
        }
#endif

        bool crashed = false;
        try {
            for (turn = 1; turn <= C::MAX_TURNS && !gameEnd && !getActivePlayers().empty(); turn++) {
                swapInfoAndViewData();
                newTurn = true;
                gameTurn();
                // ViewModule.onAfterGameTurn
                game.serializeFrame(currentView);
                if (trace) {
                    fprintf(trace, "S %d money %d %d cells %d %d active %d %d\n", turn, game.players[0].money, game.players[1].money,
                            game.getOwnedCells(0), game.getOwnedCells(1), game.players[0].active, game.players[1].active);
                }
            }
            game.onEnd();
            // EndScreenModule.onAfterOnEnd
            currentView.hasEndScreen = true;
            currentView.endScores[0] = game.players[0].score;
            currentView.endScores[1] = game.players[1].score;
            swapInfoAndViewData();
            newTurn = true;
            dumpViewAndInfos();
        } catch (const RefereeCrash& crash) {
            fprintf(stderr, "%s\n", crash.what.c_str());
            if (trace) fprintf(trace, "CRASH %s\n", crash.what.c_str());
            crashed = true;
        }

        procs[0].kill();
        procs[1].kill();
#ifdef _WIN32
        if (devnull != INVALID_HANDLE_VALUE) CloseHandle(devnull);
#endif
        if (crashed) {
            // the runner stops at the referee failure: no scores
            printf("null\nnull\n");
        } else {
            if (trace) fprintf(trace, "SCORES %d %d\n", game.players[0].score, game.players[1].score);
            printf("%d\n%d\n", game.players[0].score, game.players[1].score);
        }
        return 0;
    }
};

// Game.performGameUpdate and the parts that talk to the engine
void Game::performGameUpdate(int) {
    std::vector<int> active = engine->getActivePlayers();
    doBuilds();
    doUnits(active);
    animation.catchUp();
    doRecycle();

    gameTurn++;
    earlyFinishCounter--;
    doPassiveIncome();
    if (checkGameOver()) engine->gameEnd = true;

    // gameSummaryManager.toString(): formatErrors() + "\n"
    std::string summary;
    for (int p = 0; p < 2; p++) {
        if (!errorCount[p]) continue;
        if (!summary.empty()) summary += "\n";
        summary += "$" + std::to_string(p) + ": " + firstError[p];
        if (errorCount[p] > 1) summary += " + " + std::to_string(errorCount[p] - 1) + " other error" + (errorCount[p] > 2 ? "s" : "");
    }
    summary += "\n";
    engine->addToGameSummary(java::J(summary));
    errorCount[0] = errorCount[1] = 0;

    computeEvents();
}

bool Game::checkGameOver() {
    auto winner = [&]() { return getOwnedCells(1) > getOwnedCells(0) ? 1 : 0; }; // Stream.max keeps the first on ties
    if (earlyFinishCounter <= 0) {
        int w = winner();
        engine->addTooltip(w, java::J("Field stable for 20 turns. $" + std::to_string(w) + " wins!"));
        return true;
    }
    for (Player& p : players) {
        if (getOwnedCells(p.index) == 0) {
            engine->addTooltip(winner(), java::J("$" + std::to_string(p.index) + " has no more cells and is disqualified"));
            return true;
        }
    }
    return false;
}

void Game::computeEvents() {
    int minTime = 1000;
    animation.catchUp();
    int frameTime = std::max(animation.frameTime, minTime);
    engine->setFrameDuration(frameTime);
}

#ifndef KOTG_NO_MAIN
int main(int argc, char** argv) {
    std::string bots[4];
    bool hasBot[4] = {false, false, false, false};
    bool hasSeed = false;
    const char* seedArg = nullptr;
    const char* tracePath = nullptr;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a.size() == 3 && a[0] == '-' && a[1] == 'p' && a[2] >= '1' && a[2] <= '4' && i + 1 < argc) {
            bots[a[2] - '1'] = argv[++i];
            hasBot[a[2] - '1'] = true;
        } else if (a == "-seed" && i + 1 < argc) {
            seedArg = argv[++i];
            hasSeed = true;
        } else if (a == "-l" && i + 1 < argc) {
            tracePath = argv[++i];
        }
    }
    if (!hasBot[0] || !hasBot[1]) {
        printf("usage: referee -p1 <player1 command line> -p2 <player2 command line> [-l <trace file> -seed <seed>]\n");
        return 0;
    }
    i64 seed;
    if (hasSeed) {
        if (!java::parseLong(seedArg, seed)) {
            // Long.parseLong in the CLI: caught, the jar exits 1
            fprintf(stderr, "java.lang.NumberFormatException: For input string: \"%s\"\n", seedArg);
            return 1;
        }
    } else {
        seed = (i64)std::chrono::steady_clock::now().time_since_epoch().count();
    }
    int nPlayers = 0;
    for (int i = 0; i < 4; i++) nPlayers += hasBot[i];
    if (nPlayers > 2) {
        // the Java referee fails in init (2 spawn points) and then in the view module: the players are
        // launched but never executed, and every score is null
        static ChildProc extra[4];
#ifdef _WIN32
        HANDLE devnull = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
#else
        const char* devnull = nullptr;
#endif
        bool launched = true;
        for (int i = 0; i < 4; i++)
            if (hasBot[i] && launched) launched = extra[i].start(bots[i], devnull);
        for (int i = 0; i < 4; i++)
            if (hasBot[i]) extra[i].kill();
        if (!launched) {
            fprintf(stderr, "java.lang.RuntimeException: Failed to launch a player\n");
            fflush(stderr);
            _Exit(1);
        }
        fprintf(stderr, "java.lang.ArrayIndexOutOfBoundsException: Index 2 out of bounds for length 2\n");
        for (int i = 0; i < nPlayers; i++) printf("null\n");
        fflush(stdout);
        _Exit(0);
    }
    static Engine engine; // big, keep it off the stack
    if (tracePath) {
        engine.trace = fopen(tracePath, "w");
        const char* tv = getenv("KOTG_TRACE_VIEWS");
        engine.traceViews = tv && *tv && strcmp(tv, "0") != 0;
    }
    int rc = engine.run(bots[0], bots[1], seed);
    if (engine.trace) fclose(engine.trace);
    fflush(stdout);
    // writer threads may still be blocked on pipes kept open by grandchildren
    _Exit(rc);
}
#endif
