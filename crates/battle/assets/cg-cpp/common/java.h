// Java semantics for C++ ports of CodinGame referees: the bits of the JDK a referee observably depends
// on, reproduced exactly (checked against the JDK the jars run on). Shared by the referees of this
// folder: `#include "common/java.h"` (battle's bundler inlines it).
//
//   - numbers: (int)/(long) casts of doubles, Math.round, Double.compare, DoubleStream.sum
//   - strings (java::String is UTF-16 like java.lang.String): new String(bytes, UTF_8) with the
//     JDK's replacement of malformed input, String.getBytes(UTF_8), trim, replace,
//     Integer.parseInt / Long.parseLong
//   - JSON strings as Gson's JsonWriter writes them (JsonObject.toString, used by the view data)
//   - java.util.Random and SecureRandom("SHA1PRNG") (what MultiplayerGameManager.getRandom()
//     returns since engine 4.4), with the java.util.Random / RandomGenerator methods on top,
//     and Collections.shuffle
#pragma once

// Java never fuses a*b+c, so we must not either (zig c++ -march=native would emit FMAs)
#pragma STDC FP_CONTRACT OFF
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;
typedef uint8_t u8;

namespace java {

typedef std::u16string String; // java.lang.String: UTF-16 code units

// An exception thrown by the Java referee: `text` is e.toString(), e.g.
// "java.lang.RuntimeException: Invalid command: SET_PLAYER_TIMEOUT"
struct Exception {
    std::string text;
};

// ----------------------------------------------------------------------------
// Numbers
// ----------------------------------------------------------------------------

// (int) cast of a double: NaN gives 0, out of range values saturate
inline i32 d2i(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (i32)d;
}

// (long) cast of a double
inline i64 d2l(double d) {
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (i64)d;
}

// (int) cast of a long: keeps the low 32 bits
inline i32 l2i(i64 v) { return (i32)(u32)(u64)v; }

// Math.round(double): floor(a + 1/2) computed exactly
inline i64 round(double a) {
    if (a != a) return 0;
    if (std::fabs(a) >= 4503599627370496.0) return d2l(a); // already an integer
    double f = std::floor(a);
    return d2l((a - f >= 0.5) ? f + 1.0 : f); // a - f is exact here
}

// Double.compare
inline int compareDouble(double a, double b) {
    if (a < b) return -1;
    if (a > b) return 1;
    i64 x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    if (a != a) x = 0x7ff8000000000000LL; // doubleToLongBits collapses NaNs
    if (b != b) y = 0x7ff8000000000000LL;
    return x == y ? 0 : (x < y ? -1 : 1);
}

// Double.equals
inline bool equalsDouble(double a, double b) { return compareDouble(a, b) == 0; }

// DoubleStream.sum() of a sequential stream (Kahan summation of Collectors.sumWithCompensation)
inline double sum(const std::vector<double>& values) {
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

// ----------------------------------------------------------------------------
// Strings
// ----------------------------------------------------------------------------

inline bool isHighSurrogate(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
inline bool isLowSurrogate(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// a String from ASCII text
inline String ascii(const std::string& s) { return String(s.begin(), s.end()); }

// String.valueOf(long)
inline String str(i64 v) { return ascii(std::to_string(v)); }

// new String(bytes, UTF_8): String.decodeUTF8_UTF16 of the JDK (17+), malformed input replaced by
// U+FFFD exactly like the JDK does (how many bytes each replacement swallows matters)
inline String decodeUtf8(const char* bytes, size_t n) {
    const int8_t* src = (const int8_t*)bytes;
    int sp = 0, sl = (int)n;
    const char16_t REPL = 0xFFFD;
    String dst;
    dst.reserve(n);
    auto notCont = [](int b) { return (b & 0xc0) != 0x80; };
    while (sp < sl) {
        int b1 = src[sp++];
        if (b1 >= 0) {
            dst += (char16_t)b1;
        } else if ((b1 >> 5) == -2 && (b1 & 0x1e) != 0) {
            if (sp < sl) {
                int b2 = src[sp++];
                if (notCont(b2)) {
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
                if ((b1 == (int8_t)0xe0 && (b2 & 0xe0) == 0x80) || notCont(b2) || notCont(b3)) {
                    dst += REPL;
                    sp -= 3;
                    int c1 = src[sp], c2 = src[sp + 1]; // malformed3
                    sp += ((c1 == (int8_t)0xe0 && (c2 & 0xe0) == 0x80) || notCont(c2)) ? 1 : 2;
                } else {
                    char16_t c = (char16_t)(((b1 & 0x0f) << 12) | ((b2 & 0x3f) << 6) | (b3 & 0x3f));
                    dst += (c >= 0xD800 && c <= 0xDFFF) ? REPL : c;
                }
                continue;
            }
            if (sp < sl) {
                int b2 = src[sp];
                if ((b1 == (int8_t)0xe0 && (b2 & 0xe0) == 0x80) || notCont(b2)) {
                    dst += REPL;
                    continue;
                }
            }
            dst += REPL;
            break;
        } else if ((b1 >> 3) == -2) {
            if (sp + 2 < sl) {
                int b2 = src[sp++];
                int b3 = src[sp++];
                int b4 = src[sp++];
                int uc = ((b1 & 0x07) << 18) | ((b2 & 0x3f) << 12) | ((b3 & 0x3f) << 6) | (b4 & 0x3f);
                if (notCont(b2) || notCont(b3) || notCont(b4) || !(uc >= 0x10000 && uc <= 0x10FFFF)) {
                    dst += REPL;
                    sp -= 4;
                    int c1 = src[sp] & 0xff, c2 = src[sp + 1] & 0xff; // malformed4
                    if (c1 > 0xf4 || (c1 == 0xf0 && (c2 < 0x90 || c2 > 0xbf)) || (c1 == 0xf4 && (c2 & 0xf0) != 0x80) || notCont(c2))
                        sp += 1;
                    else if (notCont(src[sp + 2]))
                        sp += 2;
                    else
                        sp += 3;
                } else {
                    dst += (char16_t)(0xD800 + ((uc - 0x10000) >> 10));
                    dst += (char16_t)(0xDC00 + ((uc - 0x10000) & 0x3ff));
                }
                continue;
            }
            int ub1 = b1 & 0xff;
            if (ub1 > 0xf4) {
                dst += REPL;
                continue;
            }
            if (sp < sl) {
                int ub2 = src[sp] & 0xff;
                if ((ub1 == 0xf0 && (ub2 < 0x90 || ub2 > 0xbf)) || (ub1 == 0xf4 && (ub2 & 0xf0) != 0x80) || notCont(ub2)) {
                    dst += REPL;
                    continue;
                }
            }
            sp++;
            dst += REPL;
            if (sp < sl && notCont(src[sp])) continue;
            break;
        } else {
            dst += REPL;
        }
    }
    return dst;
}
inline String decodeUtf8(const std::string& bytes) { return decodeUtf8(bytes.data(), bytes.size()); }

// String.getBytes(UTF_8), also what a PrintStream writes: an unpaired surrogate becomes '?'
inline std::string encodeUtf8(const String& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (isHighSurrogate((char16_t)c) && i + 1 < s.size() && isLowSurrogate(s[i + 1])) {
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

// encodeUtf8(s).size() without building it
inline i64 utf8Length(const String& s) {
    i64 n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (isHighSurrogate(c) && i + 1 < s.size() && isLowSurrogate(s[i + 1])) {
            n += 4;
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) n += 1;
        else n += 3;
    }
    return n;
}

// String.trim(): removes the leading and trailing chars <= ' '
inline String trim(const String& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] <= u' ') a++;
    while (b > a && s[b - 1] <= u' ') b--;
    return s.substr(a, b - a);
}

// String.replace(CharSequence, CharSequence)
inline String replace(const String& s, const String& target, const String& repl) {
    String r;
    size_t off = 0, next;
    if (target.empty()) { // inserts repl around every char
        for (char16_t c : s) r += repl + c;
        return r + repl;
    }
    while ((next = s.find(target, off)) != String::npos) {
        r.append(s, off, next - off);
        r += repl;
        off = next + target.size();
    }
    r.append(s, off, String::npos);
    return r;
}

// line terminators of java.util.Scanner#nextLine ("\r\n|[\n\r  \u0085]"); also the chars
// '.' does not match in a regex (no DOTALL)
inline bool isLineSeparator(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029 || c == 0x0085; }

// \s in a java.util.regex Pattern (no UNICODE_CHARACTER_CLASS)
inline bool isRegexSpace(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == 0x0B || c == u'\f' || c == u'\r'; }

// \d in a java.util.regex Pattern (no UNICODE_CHARACTER_CLASS)
inline bool isRegexDigit(char16_t c) { return c >= u'0' && c <= u'9'; }

// Character.digit(c, 10): ASCII digits and the other Unicode decimal digits (Nd) of the BMP, each
// block being 10 consecutive chars (table generated with JDK 26)
inline int digit(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c < 0x660) return -1;
    static const char16_t ZEROS[] = {0x0660, 0x06F0, 0x07C0, 0x0966, 0x09E6, 0x0A66, 0x0AE6, 0x0B66, 0x0BE6, 0x0C66, 0x0CE6, 0x0D66,
                                     0x0DE6, 0x0E50, 0x0ED0, 0x0F20, 0x1040, 0x1090, 0x17E0, 0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90,
                                     0x1B50, 0x1BB0, 0x1C40, 0x1C50, 0xA620, 0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0, 0xFF10};
    for (char16_t z : ZEROS)
        if (c >= z && c < z + 10) return c - z;
    return -1;
}

// Long.parseLong(s) (radix 10, optional sign, any Unicode decimal digit like the JDK).
// Returns false where Java throws NumberFormatException; minV/maxV give Integer.parseInt.
inline bool parseLong(const String& s, i64& out, i64 minV = INT64_MIN, i64 maxV = INT64_MAX) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == u'-' || s[0] == u'+') {
        neg = s[0] == u'-';
        i = 1;
        if (s.size() == 1) return false;
    }
    i64 limit = neg ? minV : -maxV; // accumulates negatively like the JDK, to reach MIN_VALUE
    i64 multmin = limit / 10;
    i64 result = 0;
    for (; i < s.size(); i++) {
        int d = digit(s[i]);
        if (d < 0 || result < multmin) return false;
        result *= 10;
        if (result < limit + d) return false;
        result -= d;
    }
    out = neg ? result : -result;
    return true;
}
inline bool parseLong(const std::string& s, i64& out) { return parseLong(ascii(s), out); }

// Integer.parseInt(s) / Integer.valueOf(s)
inline bool parseInt(const String& s, i32& out) {
    i64 v;
    if (!parseLong(s, v, INT32_MIN, INT32_MAX)) return false;
    out = (i32)v;
    return true;
}

// ----------------------------------------------------------------------------
// JSON strings
// ----------------------------------------------------------------------------

// Length of a text as java.lang.String#length() (u16) and as the bytes a PrintStream writes (u8)
struct Size {
    i64 u16 = 0, u8 = 0;
    void ascii(i64 n) {
        u16 += n;
        u8 += n;
    }
    void ascii(const std::string& s) { ascii((i64)s.size()); }
    void add(const Size& o) {
        u16 += o.u16;
        u8 += o.u8;
    }
};

// The size of a UTF-8 text once decoded to a String (u16) and written back (u8)
inline Size sizeOf(const std::string& utf8) {
    Size r;
    r.u8 = (i64)utf8.size();
    for (size_t i = 0; i < utf8.size(); i++) {
        u8 b = (u8)utf8[i];
        if ((b & 0xc0) != 0x80) r.u16 += b >= 0xf0 ? 2 : 1;
    }
    return r;
}
inline Size sizeOf(const String& s) { return Size{(i64)s.size(), utf8Length(s)}; }

// A JSON string literal (quotes included) as Gson's JsonWriter writes it, returned as UTF-8.
// JsonElement#toString (what the engine sends for the view data) does not escape HTML chars;
// new Gson().toJson(...) does (htmlSafe).
inline std::string jsonQuote(const String& s, bool htmlSafe = false) {
    String out;
    out.reserve(s.size() + 2);
    out += u'"';
    char buf[8];
    for (char16_t c : s) {
        if (c == u'"' || c == u'\\') {
            out += u'\\';
            out += c;
        } else if (c < 0x20) {
            switch (c) {
                case u'\t': out += u"\\t"; break;
                case u'\b': out += u"\\b"; break;
                case u'\n': out += u"\\n"; break;
                case u'\r': out += u"\\r"; break;
                case u'\f': out += u"\\f"; break;
                default:
                    snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
                    out += ascii(buf);
            }
        } else if (c == 0x2028 || c == 0x2029 || (htmlSafe && (c == u'<' || c == u'>' || c == u'&' || c == u'=' || c == u'\''))) {
            snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
            out += ascii(buf);
        } else {
            out += c;
        }
    }
    out += u'"';
    return encodeUtf8(out);
}

// sizeOf(decodeUtf8(jsonQuote(s))) without building it
inline Size jsonQuoteSize(const String& s, bool htmlSafe = false) {
    Size r;
    r.ascii(2);
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c < 0x20) {
            r.ascii((c == u'\b' || c == u'\t' || c == u'\n' || c == u'\f' || c == u'\r') ? 2 : 6);
        } else if (c == u'"' || c == u'\\') {
            r.ascii(2);
        } else if (c == 0x2028 || c == 0x2029 || (htmlSafe && (c == u'<' || c == u'>' || c == u'&' || c == u'=' || c == u'\''))) {
            r.ascii(6);
        } else if (c < 0x80) {
            r.ascii(1);
        } else if (c < 0x800) {
            r.u16 += 1;
            r.u8 += 2;
        } else if (isHighSurrogate(c) && i + 1 < s.size() && isLowSurrogate(s[i + 1])) {
            r.u16 += 2;
            r.u8 += 4;
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            r.ascii(1); // written as '?'
        } else {
            r.u16 += 1;
            r.u8 += 3;
        }
    }
    return r;
}

// A JSON string in pure ASCII (every other char as \uXXXX): lossless, even for unpaired
// surrogates. Used by the referees' -l traces.
inline std::string traceJson(const String& s) {
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

// ----------------------------------------------------------------------------
// Random numbers
// ----------------------------------------------------------------------------

// Some entropy, where the jar uses a random seed (no -seed, or -seed 0 with SHA1PRNG)
inline u64 entropy() {
    static u64 counter = 0;
    u64 x = (u64)std::chrono::steady_clock::now().time_since_epoch().count();
    x ^= (u64)std::chrono::system_clock::now().time_since_epoch().count() * 0x9E3779B97F4A7C15ULL;
    x ^= (u64)(uintptr_t)&counter + (++counter) * 0xD1B54A32D192ED03ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// The java.util.Random methods (and the RandomGenerator defaults it inherits) on top of next(bits)
template <class Impl>
struct RandomMethods {
    i32 bits(int n) { return static_cast<Impl*>(this)->next(n); }

    i32 nextInt() { return bits(32); }

    // Random.nextInt(int bound)
    i32 nextInt(i32 bound) {
        i32 r = bits(31);
        i32 m = bound - 1;
        if ((bound & m) == 0) return (i32)(((i64)bound * (i64)r) >> 31);
        for (i32 u = r; (i32)((u32)u - (u32)(r = u % bound) + (u32)m) < 0; u = bits(31)) {
        }
        return r;
    }

    // RandomGenerator.nextInt(int origin, int bound) (RandomSupport.boundedNextInt)
    i32 nextInt(i32 origin, i32 bound) {
        i32 r = nextInt();
        if (origin < bound) {
            i32 n = (i32)((u32)bound - (u32)origin);
            i32 m = n - 1;
            if ((n & m) == 0) {
                r = (i32)((u32)(r & m) + (u32)origin);
            } else if (n > 0) {
                for (i32 u = (i32)((u32)r >> 1); (i32)((u32)u + (u32)m - (u32)(r = u % n)) < 0; u = (i32)((u32)nextInt() >> 1)) {
                }
                r = (i32)((u32)r + (u32)origin);
            } else {
                while (r < origin || r >= bound) r = nextInt();
            }
        }
        return r;
    }

    i64 nextLong() {
        u64 hi = (u64)(i64)bits(32);
        return (i64)((hi << 32) + (u64)(i64)bits(32));
    }

    bool nextBoolean() { return bits(1) != 0; }

    float nextFloat() { return (float)bits(24) / (float)(1 << 24); }

    double nextDouble() { return (double)(((i64)bits(26) << 27) + bits(27)) * 0x1.0p-53; }

    // RandomGenerator.nextDouble(double bound) (RandomSupport.boundedNextDouble)
    double nextDouble(double bound) {
        double r = nextDouble();
        r = r * bound;
        if (r >= bound) r = std::nextafter(bound, -INFINITY);
        return r;
    }

    // RandomGenerator.nextDouble(double origin, double bound)
    double nextDouble(double origin, double bound) {
        double r = nextDouble();
        r = r * (bound - origin) + origin;
        if (r >= bound) r = std::nextafter(bound, -INFINITY);
        return r;
    }
};

// java.util.Random
struct Random : RandomMethods<Random> {
    u64 seed = 0;
    Random() {}
    explicit Random(i64 s) { setSeed(s); }
    void setSeed(i64 s) { seed = ((u64)s ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1); }
    i32 next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return (i32)(u32)(seed >> (48 - bits));
    }
};

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
                w[i] = ((u32)buf[off + i * 4] << 24) | ((u32)buf[off + i * 4 + 1] << 16) | ((u32)buf[off + i * 4 + 2] << 8) | (u32)buf[off + i * 4 + 3];
            for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
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
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
        }
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = (u8)(h[i] >> (24 - 8 * j));
    }
};

// SecureRandom.getInstance("SHA1PRNG") (sun.security.provider.SecureRandom), used through the
// java.util.Random API: MultiplayerGameManager.getRandom() of engine 4.4+
struct SHA1PRNG : RandomMethods<SHA1PRNG> {
    static const int DIGEST_SIZE = 20;
    u8 state[DIGEST_SIZE];
    bool haveState = false;
    u8 remainder[DIGEST_SIZE] = {0};
    int remCount = 0;

    SHA1PRNG() {}
    explicit SHA1PRNG(i64 seed) { setSeed(seed); }

    void engineSetSeed(const u8* seed, size_t n) {
        std::vector<u8> msg;
        if (haveState) msg.insert(msg.end(), state, state + DIGEST_SIZE);
        msg.insert(msg.end(), seed, seed + n);
        SHA1::hash(msg.data(), msg.size(), state);
        haveState = true;
        remCount = 0;
    }

    // SecureRandom.setSeed(long): 0 is ignored, the engine then seeds itself from system entropy
    // on first use (not reproducible: -seed 0 plays a random game with the jar too)
    void setSeed(i64 seed) {
        if (seed == 0) return;
        u8 b[8];
        for (int i = 0; i < 8; i++) b[i] = (u8)(((u64)seed >> (8 * i)) & 0xff); // longToByteArray
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

    void nextBytes(u8* result, int len) {
        if (!haveState) {
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

    // SecureRandom.nextLong(): the low half is not sign-extended, unlike java.util.Random's
    i64 nextLong() {
        u64 hi = (u64)(u32)nextInt();
        return (i64)((hi << 32) | (u64)(u32)nextInt());
    }

    // SecureRandom.next(int)
    i32 next(int numBits) {
        int numBytes = (numBits + 7) / 8;
        u8 b[4];
        nextBytes(b, numBytes);
        u32 nxt = 0;
        for (int i = 0; i < numBytes; i++) nxt = (nxt << 8) + b[i];
        return (i32)(nxt >> (numBytes * 8 - numBits));
    }
};

// Collections.shuffle(list, rnd) for a RandomAccess list (ArrayList, Arrays.asList)
template <class T, class R>
inline void shuffle(std::vector<T>& list, R& rnd) {
    for (int i = (int)list.size(); i > 1; i--) std::swap(list[i - 1], list[rnd.nextInt(i)]);
}

} // namespace java
