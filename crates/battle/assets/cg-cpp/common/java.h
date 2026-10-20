// Java semantics for C++ ports of CodinGame referees: the bits of the JDK a referee observably depends
// on, reproduced exactly (checked against the JDK the jars run on). Shared by the referees of this
// folder: `#include "common/java.h"` (battle's bundler inlines it). A referee keeps no JDK behaviour
// of its own: what it needs and is missing here belongs here, with a check against the JDK.
//
//   - numbers: (int)/(long) casts of doubles, int arithmetic, Math.round/abs/min/max,
//     Integer.compare/signum, Double.compare, DoubleStream.sum, Double.parseDouble
//   - strings (java::String is UTF-16 like java.lang.String; a std::string is UTF-8 text):
//     new String(bytes, UTF_8) with the JDK's replacement of malformed input, String.getBytes(UTF_8),
//     trim, replace, split, startsWith, hashCode, toUpperCase/toLowerCase compared with ASCII text,
//     Integer.parseInt / Long.parseLong, the regex classes \s \d \w, the texts of the JDK's exceptions
//   - JSON strings as Gson's JsonWriter writes them (JsonObject.toString, used by the view data)
//   - collections: the iteration order of HashMap/HashSet, PriorityQueue, TimSort (List.sort,
//     Collections.sort, Arrays.sort with a comparator, Stream.sorted)
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
#include <system_error>
#include <type_traits>
#include <charconv>
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

// int arithmetic: overflows wrap around
inline i32 add(i32 a, i32 b) { return (i32)((u32)a + (u32)b); }
inline i32 sub(i32 a, i32 b) { return (i32)((u32)a - (u32)b); }
inline i32 mul(i32 a, i32 b) { return (i32)((u32)a * (u32)b); }

// Math.abs(int) / Math.abs(long): MIN_VALUE stays negative
inline i32 abs(i32 a) { return a < 0 ? (i32)(0u - (u32)a) : a; }
inline i64 abs(i64 a) { return a < 0 ? (i64)(0ull - (u64)a) : a; }

// Integer.signum
inline int signum(i32 v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

// Integer.compare
inline int compareInt(i32 a, i32 b) { return a < b ? -1 : (a == b ? 0 : 1); }

// Math.min(double, double): NaN if either is, -0.0 below 0.0
inline double min(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && std::signbit(b)) return b;
    return a <= b ? a : b;
}

// Math.max(double, double)
inline double max(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && std::signbit(a)) return b;
    return a >= b ? a : b;
}

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

// The string functions below that take any string type work on a String, or on a std::string of
// UTF-8 text where the result is the same (ASCII separators, targets and prefixes).
namespace detail {
template <class C>
inline u32 unit(C c) { return (u32)(typename std::make_unsigned<C>::type)c; }
} // namespace detail

// String.trim(): removes the leading and trailing chars <= ' '
template <class S>
inline S trim(const S& s) {
    size_t a = 0, b = s.size();
    while (a < b && detail::unit(s[a]) <= 0x20) a++;
    while (b > a && detail::unit(s[b - 1]) <= 0x20) b--;
    return s.substr(a, b - a);
}

// String.replace(CharSequence, CharSequence)
template <class S>
inline S replace(const S& s, const S& target, const S& repl) {
    S r;
    size_t off = 0, next;
    if (target.empty()) { // inserts repl around every char
        for (auto c : s) r += repl + c;
        return r + repl;
    }
    while ((next = s.find(target, off)) != S::npos) {
        r.append(s, off, next - off);
        r += repl;
        off = next + target.size();
    }
    r.append(s, off, S::npos);
    return r;
}
inline String replace(const String& s, const char16_t* target, const char16_t* repl) { return replace(s, String(target), String(repl)); }
inline std::string replace(const std::string& s, const char* target, const char* repl) {
    return replace(s, std::string(target), std::string(repl));
}

// String.split(regex, limit) for a regex of one char other than .$|()[{^?*+\ (the JDK's fast path,
// no regex involved): limit > 0 gives at most `limit` parts, 0 removes the trailing empty ones
template <class S>
inline std::vector<S> split(const S& s, typename S::value_type ch, int limit = 0) {
    std::vector<S> list;
    size_t off = 0, next;
    bool limited = limit > 0;
    while ((next = s.find(ch, off)) != S::npos) {
        if (!limited || (int)list.size() < limit - 1) {
            list.push_back(s.substr(off, next - off));
            off = next + 1;
        } else { // last one
            list.push_back(s.substr(off));
            off = s.size();
            break;
        }
    }
    if (off == 0) return {s}; // no match
    if (!limited || (int)list.size() < limit) list.push_back(s.substr(off));
    size_t n = list.size();
    if (limit == 0)
        while (n > 0 && list[n - 1].empty()) n--;
    list.resize(n);
    return list;
}

// String.startsWith(prefix, offset)
template <class S, class C>
inline bool startsWith(const S& s, const C* prefix, size_t offset = 0) {
    size_t n = 0;
    while (prefix[n]) n++;
    if (offset > s.size() || s.size() - offset < n) return false;
    for (size_t i = 0; i < n; i++)
        if (detail::unit(s[offset + i]) != detail::unit(prefix[i])) return false;
    return true;
}

// String.hashCode
inline i32 hashCode(const String& s) {
    u32 h = 0;
    for (char16_t c : s) h = 31 * h + c;
    return (i32)h;
}

// s.toUpperCase().startsWith(t), s.toUpperCase().equals(t), and the same with toLowerCase(), for
// an ASCII text t (default locale en_US, which maps like Locale.ROOT). Only these non-ASCII code
// points have a mapping that starts with an ASCII char (every code point checked with JDK 26):
// `ascii` is that start, `more` tells that non-ASCII chars follow it.
namespace detail {
struct CaseMapping {
    u32 cp;
    const char* ascii;
    bool more;
};
inline const CaseMapping* caseMapping(u32 cp, bool upper) {
    static const CaseMapping UPPER[] = {
        {0x00DF, "SS", false}, {0x0131, "I", false},  {0x017F, "S", false},  {0x01F0, "J", true},
        {0x1E96, "H", true},   {0x1E97, "T", true},   {0x1E98, "W", true},   {0x1E99, "Y", true},
        {0x1E9A, "A", true},   {0xFB00, "FF", false}, {0xFB01, "FI", false}, {0xFB02, "FL", false},
        {0xFB03, "FFI", false}, {0xFB04, "FFL", false}, {0xFB05, "ST", false}, {0xFB06, "ST", false},
    };
    static const CaseMapping LOWER[] = {{0x0130, "i", true}, {0x212A, "k", false}};
    if (upper) {
        for (const CaseMapping& m : UPPER)
            if (m.cp == cp) return &m;
    } else {
        for (const CaseMapping& m : LOWER)
            if (m.cp == cp) return &m;
    }
    return nullptr;
}
inline bool caseMappingMatches(const String& s, const char* t, bool upper, bool prefix) {
    size_t k = 0, n = strlen(t);
    for (size_t i = 0; i < s.size();) {
        if (prefix && k == n) return true;
        u32 cp = s[i++];
        if (isHighSurrogate((char16_t)cp) && i < s.size() && isLowSurrogate(s[i])) cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i++] - 0xDC00);
        if (cp < 0x80) {
            if (upper && cp >= 'a' && cp <= 'z') cp -= 32;
            if (!upper && cp >= 'A' && cp <= 'Z') cp += 32;
            if (k == n || cp != (u32)(unsigned char)t[k]) return false;
            k++;
            continue;
        }
        const CaseMapping* m = caseMapping(cp, upper);
        if (!m) return false; // non-ASCII text
        for (const char* a = m->ascii; *a; a++) {
            if (k == n) return prefix;
            if (*a != t[k]) return false;
            k++;
        }
        if (m->more) return prefix && k == n;
    }
    return k == n;
}
} // namespace detail
inline bool upperCaseStartsWith(const String& s, const char* t) { return detail::caseMappingMatches(s, t, true, true); }
inline bool upperCaseEquals(const String& s, const char* t) { return detail::caseMappingMatches(s, t, true, false); }
inline bool lowerCaseStartsWith(const String& s, const char* t) { return detail::caseMappingMatches(s, t, false, true); }
inline bool lowerCaseEquals(const String& s, const char* t) { return detail::caseMappingMatches(s, t, false, false); }

// line terminators of java.util.Scanner#nextLine ("\r\n|[\n\r  \u0085]"); also the chars
// '.' does not match in a regex (no DOTALL)
inline bool isLineSeparator(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029 || c == 0x0085; }

// \s in a java.util.regex Pattern (no UNICODE_CHARACTER_CLASS)
inline bool isRegexSpace(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == 0x0B || c == u'\f' || c == u'\r'; }

// \d in a java.util.regex Pattern (no UNICODE_CHARACTER_CLASS)
inline bool isRegexDigit(char16_t c) { return c >= u'0' && c <= u'9'; }

// \w in a java.util.regex Pattern (no UNICODE_CHARACTER_CLASS)
inline bool isRegexWord(char16_t c) { return isRegexDigit(c) || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'_'; }

// Whether c matches the literal pattern char p (ASCII) of a CASE_INSENSITIVE Pattern without
// UNICODE_CASE: only the ASCII letters fold
inline bool regexCharMatchesIgnoreCase(char16_t c, char p) {
    char16_t lo = (char16_t)(unsigned char)p, up = lo;
    if (p >= 'a' && p <= 'z') up = (char16_t)(p - 32);
    if (p >= 'A' && p <= 'Z') lo = (char16_t)(p + 32);
    return c == lo || c == up;
}

// Whether all of s matches the literal ASCII text t in such a Pattern
template <class S>
inline bool regexMatchesIgnoreCase(const S& s, const char* t) {
    size_t n = strlen(t);
    if (s.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (detail::unit(s[i]) > 0xFFFF || !regexCharMatchesIgnoreCase((char16_t)detail::unit(s[i]), t[i])) return false;
    return true;
}

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
inline bool parseLong(const std::string& utf8, i64& out) { return parseLong(decodeUtf8(utf8), out); }

// Integer.parseInt(s) / Integer.valueOf(s)
inline bool parseInt(const String& s, i32& out) {
    i64 v;
    if (!parseLong(s, v, INT32_MIN, INT32_MAX)) return false;
    out = (i32)v;
    return true;
}
inline bool parseInt(const std::string& utf8, i32& out) { return parseInt(decodeUtf8(utf8), out); }

// Double.parseDouble(s) (FloatingDecimal.readJavaFormatString): trims, then an optional sign and
// "NaN", "Infinity", or a decimal or hexadecimal literal with an optional [fFdD] suffix. Returns
// false where Java throws NumberFormatException. The conversion is correctly rounded, as the JDK's.
inline bool parseDouble(const String& str, double& out) {
    String trimmed = trim(str);
    std::string in; // a valid literal is ASCII
    for (char16_t c : trimmed) {
        if (c >= 0x80) return false;
        in += (char)c;
    }
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    auto isHexDigit = [&](char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    size_t len = in.size(), i = 0;
    if (len == 0) return false;
    bool neg = false;
    if (in[i] == '-' || in[i] == '+') neg = in[i++] == '-';
    if (i >= len) return false;
    if (in[i] == 'N' || in[i] == 'I') {
        if (in.compare(i, std::string::npos, "NaN") == 0) out = NAN;
        else if (in.compare(i, std::string::npos, "Infinity") == 0) out = neg ? -INFINITY : INFINITY;
        else return false;
        return true;
    }
    bool hex = in[i] == '0' && i + 1 < len && (in[i + 1] == 'x' || in[i + 1] == 'X');
    if (hex) i += 2;
    // mantissa: digits with at most one '.', at least one digit
    size_t mant = i, point = std::string::npos, firstSig = std::string::npos, nDigits = 0;
    while (i < len && ((hex ? isHexDigit(in[i]) : isDigit(in[i])) || (in[i] == '.' && point == std::string::npos))) {
        if (in[i] == '.') point = i;
        else {
            nDigits++;
            if (firstSig == std::string::npos && in[i] != '0') firstSig = i;
        }
        i++;
    }
    if (nDigits == 0) return false;
    size_t mantEnd = i;
    // exponent: required for hex (p), optional for decimal (e)
    bool hasExp = i < len && (hex ? (in[i] == 'p' || in[i] == 'P') : (in[i] == 'e' || in[i] == 'E'));
    if (hex && !hasExp) return false;
    i64 exponent = 0;
    size_t expStart = i;
    if (hasExp) {
        i++;
        bool expNeg = false;
        if (i < len && (in[i] == '+' || in[i] == '-')) expNeg = in[i++] == '-';
        size_t expDigits = i;
        while (i < len && isDigit(in[i])) {
            if (exponent < 1000000000) exponent = exponent * 10 + (in[i] - '0');
            i++;
        }
        if (i == expDigits) return false;
        if (expNeg) exponent = -exponent;
    }
    size_t numEnd = i;
    if (i < len && (i != len - 1 || (in[i] != 'f' && in[i] != 'F' && in[i] != 'd' && in[i] != 'D'))) return false;
    std::string lit = in.substr(mant, mantEnd - mant);
    if (hex) lit += "p" + std::to_string(exponent);
    else lit += in.substr(expStart, numEnd - expStart);
    double v = 0;
    auto r = std::from_chars(lit.data(), lit.data() + lit.size(), v, hex ? std::chars_format::hex : std::chars_format::general);
    if (r.ec == std::errc::result_out_of_range) {
        // overflow or underflow: the magnitude of the leading digit tells which
        size_t intEnd = point != std::string::npos ? point : mantEnd;
        i64 mag = firstSig == std::string::npos ? -1 : firstSig < intEnd ? (i64)(intEnd - firstSig) : -(i64)(firstSig - intEnd - 1);
        v = (hex ? mag * 4 : mag) + exponent > 0 ? INFINITY : 0.0;
    } else if (r.ec != std::errc() || r.ptr != lit.data() + lit.size()) {
        return false;
    }
    out = neg ? -v : v;
    return true;
}
inline bool parseDouble(const std::string& utf8, double& out) { return parseDouble(decodeUtf8(utf8), out); }

// e.toString() of the exceptions the JDK throws
inline String numberFormatException(const String& input) { return u"java.lang.NumberFormatException: For input string: \"" + input + u"\""; }
inline std::string numberFormatException(const std::string& utf8) { return "java.lang.NumberFormatException: For input string: \"" + utf8 + "\""; }
inline std::string indexOutOfBoundsException(i64 index, i64 length) {
    return "java.lang.IndexOutOfBoundsException: Index " + std::to_string(index) + " out of bounds for length " + std::to_string(length);
}
inline std::string arrayIndexOutOfBoundsException(i64 index, i64 length) {
    return "java.lang.ArrayIndexOutOfBoundsException: Index " + std::to_string(index) + " out of bounds for length " + std::to_string(length);
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

// A JSON string as Gson's JsonWriter writes it, returned as UTF-8: jsonQuote with the quotes,
// jsonEscape without them. JsonElement#toString (what the engine sends for the view data) does not
// escape HTML chars; new Gson().toJson(...) does (htmlSafe).
inline std::string jsonEscape(const String& s, bool htmlSafe = false) {
    String out;
    out.reserve(s.size());
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
    return encodeUtf8(out);
}
inline std::string jsonQuote(const String& s, bool htmlSafe = false) { return "\"" + jsonEscape(s, htmlSafe) + "\""; }

// sizeOf(decodeUtf8(jsonEscape(s))) and sizeOf(decodeUtf8(jsonQuote(s))) without building them
inline Size jsonEscapeSize(const String& s, bool htmlSafe = false) {
    Size r;
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
inline Size jsonQuoteSize(const String& s, bool htmlSafe = false) {
    Size r = jsonEscapeSize(s, htmlSafe);
    r.ascii(2);
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
// Collections
// ----------------------------------------------------------------------------

// The iteration order of a java.util.HashMap / HashSet: the keys are ints (an index or an id) with a
// given hashCode(); the values live elsewhere. Mirrors HashMap.putVal (put / HashSet.add),
// HashMap.compute (inserting a new key), HashMap.remove, resize, treeifyBin and the TreeNode code
// (red-black bins keep a "next" order that iteration follows). Two keys in the same tree bin (8+
// keys in one bucket of a table of 64+) must not share a hash code: Java would order them with
// compareTo or identity hash codes. Integer keys and in-grid coordinates never do.
struct HashMapOrder {
    struct N {
        i32 hash, key;
        int next = -1, parent = -1, left = -1, right = -1, prev = -1;
        bool red = false, tree = false;
    };
    std::vector<N> pool;
    std::vector<int> tab;
    int size = 0, threshold = 0;

    static i32 spread(i32 h) { return h ^ (i32)((u32)h >> 16); }
    int newNode(i32 hash, i32 key, int next, bool tree) {
        N n;
        n.hash = hash;
        n.key = key;
        n.next = next;
        n.tree = tree;
        pool.push_back(n);
        return (int)pool.size() - 1;
    }
    N& P(int i) { return pool[i]; }

    void resize() {
        int oldCap = (int)tab.size();
        int oldThr = threshold;
        int newCap, newThr = 0;
        if (oldCap > 0) {
            newCap = oldCap << 1;
            if (oldCap >= 16) newThr = oldThr << 1;
        } else {
            newCap = 16;
            newThr = 12;
        }
        // (the maps here use the default initial capacity, so newThr is never 0)
        threshold = newThr;
        std::vector<int> oldTab;
        oldTab.swap(tab);
        tab.assign(newCap, -1);
        for (int j = 0; j < oldCap; ++j) {
            int e = oldTab[j];
            if (e < 0) continue;
            if (P(e).next < 0) {
                tab[P(e).hash & (newCap - 1)] = e;
            } else if (P(e).tree) {
                split(e, j, oldCap);
            } else {
                int loHead = -1, loTail = -1, hiHead = -1, hiTail = -1, next;
                do {
                    next = P(e).next;
                    if ((P(e).hash & oldCap) == 0) {
                        if (loTail < 0) loHead = e;
                        else P(loTail).next = e;
                        loTail = e;
                    } else {
                        if (hiTail < 0) hiHead = e;
                        else P(hiTail).next = e;
                        hiTail = e;
                    }
                } while ((e = next) >= 0);
                if (loTail >= 0) {
                    P(loTail).next = -1;
                    tab[j] = loHead;
                }
                if (hiTail >= 0) {
                    P(hiTail).next = -1;
                    tab[j + oldCap] = hiHead;
                }
            }
        }
    }

    int root(int r) {
        for (;;) {
            int p = P(r).parent;
            if (p < 0) return r;
            r = p;
        }
    }
    void moveRootToFront(int rt) {
        int n = (int)tab.size();
        if (rt < 0 || n == 0) return;
        int index = (n - 1) & P(rt).hash;
        int first = tab[index];
        if (rt != first) {
            tab[index] = rt;
            int rp = P(rt).prev;
            int rn = P(rt).next;
            if (rn >= 0) P(rn).prev = rp;
            if (rp >= 0) P(rp).next = rn;
            if (first >= 0) P(first).prev = rt;
            P(rt).next = first;
            P(rt).prev = -1;
        }
    }
    int find(int p, i32 h) {
        while (p >= 0) {
            i32 ph = P(p).hash;
            if (ph > h) p = P(p).left;
            else if (ph < h) p = P(p).right;
            else return p;
        }
        return -1;
    }
    int rotateLeft(int rt, int p) {
        int r, pp, rl;
        if (p >= 0 && (r = P(p).right) >= 0) {
            if ((rl = P(p).right = P(r).left) >= 0) P(rl).parent = p;
            if ((pp = P(r).parent = P(p).parent) < 0) {
                rt = r;
                P(r).red = false;
            } else if (P(pp).left == p) P(pp).left = r;
            else P(pp).right = r;
            P(r).left = p;
            P(p).parent = r;
        }
        return rt;
    }
    int rotateRight(int rt, int p) {
        int l, pp, lr;
        if (p >= 0 && (l = P(p).left) >= 0) {
            if ((lr = P(p).left = P(l).right) >= 0) P(lr).parent = p;
            if ((pp = P(l).parent = P(p).parent) < 0) {
                rt = l;
                P(l).red = false;
            } else if (P(pp).right == p) P(pp).right = l;
            else P(pp).left = l;
            P(l).right = p;
            P(p).parent = l;
        }
        return rt;
    }
    int balanceInsertion(int rt, int x) {
        P(x).red = true;
        for (int xp, xpp, xppl, xppr;;) {
            if ((xp = P(x).parent) < 0) {
                P(x).red = false;
                return x;
            } else if (!P(xp).red || (xpp = P(xp).parent) < 0)
                return rt;
            if (xp == (xppl = P(xpp).left)) {
                if ((xppr = P(xpp).right) >= 0 && P(xppr).red) {
                    P(xppr).red = false;
                    P(xp).red = false;
                    P(xpp).red = true;
                    x = xpp;
                } else {
                    if (x == P(xp).right) {
                        rt = rotateLeft(rt, x = xp);
                        xpp = (xp = P(x).parent) < 0 ? -1 : P(xp).parent;
                    }
                    if (xp >= 0) {
                        P(xp).red = false;
                        if (xpp >= 0) {
                            P(xpp).red = true;
                            rt = rotateRight(rt, xpp);
                        }
                    }
                }
            } else {
                if (xppl >= 0 && P(xppl).red) {
                    P(xppl).red = false;
                    P(xp).red = false;
                    P(xpp).red = true;
                    x = xpp;
                } else {
                    if (x == P(xp).left) {
                        rt = rotateRight(rt, x = xp);
                        xpp = (xp = P(x).parent) < 0 ? -1 : P(xp).parent;
                    }
                    if (xp >= 0) {
                        P(xp).red = false;
                        if (xpp >= 0) {
                            P(xpp).red = true;
                            rt = rotateLeft(rt, xpp);
                        }
                    }
                }
            }
        }
    }
    int balanceDeletion(int rt, int x) {
        for (int xp, xpl, xpr;;) {
            if (x < 0 || x == rt) return rt;
            else if ((xp = P(x).parent) < 0) {
                P(x).red = false;
                return x;
            } else if (P(x).red) {
                P(x).red = false;
                return rt;
            } else if ((xpl = P(xp).left) == x) {
                if ((xpr = P(xp).right) >= 0 && P(xpr).red) {
                    P(xpr).red = false;
                    P(xp).red = true;
                    rt = rotateLeft(rt, xp);
                    xpr = (xp = P(x).parent) < 0 ? -1 : P(xp).right;
                }
                if (xpr < 0) x = xp;
                else {
                    int sl = P(xpr).left, sr = P(xpr).right;
                    if ((sr < 0 || !P(sr).red) && (sl < 0 || !P(sl).red)) {
                        P(xpr).red = true;
                        x = xp;
                    } else {
                        if (sr < 0 || !P(sr).red) {
                            if (sl >= 0) P(sl).red = false;
                            P(xpr).red = true;
                            rt = rotateRight(rt, xpr);
                            xpr = (xp = P(x).parent) < 0 ? -1 : P(xp).right;
                        }
                        if (xpr >= 0) {
                            P(xpr).red = (xp < 0) ? false : P(xp).red;
                            if ((sr = P(xpr).right) >= 0) P(sr).red = false;
                        }
                        if (xp >= 0) {
                            P(xp).red = false;
                            rt = rotateLeft(rt, xp);
                        }
                        x = rt;
                    }
                }
            } else { // symmetric
                if (xpl >= 0 && P(xpl).red) {
                    P(xpl).red = false;
                    P(xp).red = true;
                    rt = rotateRight(rt, xp);
                    xpl = (xp = P(x).parent) < 0 ? -1 : P(xp).left;
                }
                if (xpl < 0) x = xp;
                else {
                    int sl = P(xpl).left, sr = P(xpl).right;
                    if ((sl < 0 || !P(sl).red) && (sr < 0 || !P(sr).red)) {
                        P(xpl).red = true;
                        x = xp;
                    } else {
                        if (sl < 0 || !P(sl).red) {
                            if (sr >= 0) P(sr).red = false;
                            P(xpl).red = true;
                            rt = rotateLeft(rt, xpl);
                            xpl = (xp = P(x).parent) < 0 ? -1 : P(xp).left;
                        }
                        if (xpl >= 0) {
                            P(xpl).red = (xp < 0) ? false : P(xp).red;
                            if ((sl = P(xpl).left) >= 0) P(sl).red = false;
                        }
                        if (xp >= 0) {
                            P(xp).red = false;
                            rt = rotateRight(rt, xp);
                        }
                        x = rt;
                    }
                }
            }
        }
    }
    void treeify(int head) {
        int rt = -1;
        for (int x = head, next; x >= 0; x = next) {
            next = P(x).next;
            P(x).left = P(x).right = -1;
            if (rt < 0) {
                P(x).parent = -1;
                P(x).red = false;
                rt = x;
            } else {
                i32 h = P(x).hash;
                for (int p = rt;;) {
                    i32 ph = P(p).hash;
                    int dir = ph > h ? -1 : 1; // hashes are distinct
                    int xp = p;
                    if ((p = (dir <= 0) ? P(p).left : P(p).right) < 0) {
                        P(x).parent = xp;
                        if (dir <= 0) P(xp).left = x;
                        else P(xp).right = x;
                        rt = balanceInsertion(rt, x);
                        break;
                    }
                }
            }
        }
        moveRootToFront(rt);
    }
    int untreeify(int head) {
        for (int q = head; q >= 0; q = P(q).next) {
            P(q).tree = false;
            P(q).prev = P(q).parent = P(q).left = P(q).right = -1;
            P(q).red = false;
        }
        return head;
    }
    void split(int b, int index, int bit) {
        int loHead = -1, loTail = -1, hiHead = -1, hiTail = -1;
        int lc = 0, hc = 0;
        for (int e = b, next; e >= 0; e = next) {
            next = P(e).next;
            P(e).next = -1;
            if ((P(e).hash & bit) == 0) {
                if ((P(e).prev = loTail) < 0) loHead = e;
                else P(loTail).next = e;
                loTail = e;
                ++lc;
            } else {
                if ((P(e).prev = hiTail) < 0) hiHead = e;
                else P(hiTail).next = e;
                hiTail = e;
                ++hc;
            }
        }
        if (loHead >= 0) {
            if (lc <= 6) tab[index] = untreeify(loHead);
            else {
                tab[index] = loHead;
                if (hiHead >= 0) treeify(loHead);
            }
        }
        if (hiHead >= 0) {
            if (hc <= 6) tab[index + bit] = untreeify(hiHead);
            else {
                tab[index + bit] = hiHead;
                if (loHead >= 0) treeify(hiHead);
            }
        }
    }
    void treeifyBin(i32 hash) {
        int n = (int)tab.size();
        if (n < 64) {
            resize();
            return;
        }
        int index = (n - 1) & hash;
        std::vector<int> chain;
        for (int e = tab[index]; e >= 0; e = P(e).next) chain.push_back(e);
        if (chain.empty()) return;
        for (size_t k = 0; k < chain.size(); k++) {
            N& c = P(chain[k]);
            c.tree = true;
            c.prev = k ? chain[k - 1] : -1;
            c.next = k + 1 < chain.size() ? chain[k + 1] : -1;
        }
        tab[index] = chain[0];
        treeify(chain[0]);
    }
    // returns the existing node, or -1 after inserting
    int putTreeVal(int first, i32 h, i32 k) {
        int rt = P(first).parent >= 0 ? root(first) : first;
        for (int p = rt;;) {
            int dir;
            i32 ph = P(p).hash;
            if (ph > h) dir = -1;
            else if (ph < h) dir = 1;
            else return p;
            int xp = p;
            if ((p = (dir <= 0) ? P(p).left : P(p).right) < 0) {
                int xpn = P(xp).next;
                int x = newNode(h, k, xpn, true);
                if (dir <= 0) P(xp).left = x;
                else P(xp).right = x;
                P(xp).next = x;
                P(x).parent = P(x).prev = xp;
                if (xpn >= 0) P(xpn).prev = x;
                moveRootToFront(balanceInsertion(rt, x));
                return -1;
            }
        }
    }
    void removeTreeNode(int node) {
        int n = (int)tab.size();
        int index = (n - 1) & P(node).hash;
        int first = tab[index], rt = first, rl;
        int succ = P(node).next, pred = P(node).prev;
        if (pred < 0) tab[index] = first = succ;
        else P(pred).next = succ;
        if (succ >= 0) P(succ).prev = pred;
        if (first < 0) return;
        if (P(rt).parent >= 0) rt = root(rt);
        if (rt < 0 || (P(rt).right < 0 || (rl = P(rt).left) < 0 || P(rl).left < 0)) {
            tab[index] = untreeify(first); // too small
            return;
        }
        int p = node, pl = P(node).left, pr = P(node).right, replacement;
        if (pl >= 0 && pr >= 0) {
            int s = pr, sl;
            while ((sl = P(s).left) >= 0) s = sl; // find successor
            bool c = P(s).red;
            P(s).red = P(p).red;
            P(p).red = c; // swap colors
            int sr = P(s).right;
            int pp = P(p).parent;
            if (s == pr) { // p was s's direct parent
                P(p).parent = s;
                P(s).right = p;
            } else {
                int sp = P(s).parent;
                if ((P(p).parent = sp) >= 0) {
                    if (s == P(sp).left) P(sp).left = p;
                    else P(sp).right = p;
                }
                if ((P(s).right = pr) >= 0) P(pr).parent = s;
            }
            P(p).left = -1;
            if ((P(p).right = sr) >= 0) P(sr).parent = p;
            if ((P(s).left = pl) >= 0) P(pl).parent = s;
            if ((P(s).parent = pp) < 0) rt = s;
            else if (p == P(pp).left) P(pp).left = s;
            else P(pp).right = s;
            if (sr >= 0) replacement = sr;
            else replacement = p;
        } else if (pl >= 0) replacement = pl;
        else if (pr >= 0) replacement = pr;
        else replacement = p;
        if (replacement != p) {
            int pp = P(replacement).parent = P(p).parent;
            if (pp < 0) {
                rt = replacement;
                P(replacement).red = false;
            } else if (p == P(pp).left) P(pp).left = replacement;
            else P(pp).right = replacement;
            P(p).left = P(p).right = P(p).parent = -1;
        }
        int r = P(p).red ? rt : balanceDeletion(rt, replacement);
        if (replacement == p) { // detach
            int pp = P(p).parent;
            P(p).parent = -1;
            if (pp >= 0) {
                if (p == P(pp).left) P(pp).left = -1;
                else if (p == P(pp).right) P(pp).right = -1;
            }
        }
        moveRootToFront(r);
    }

    // HashMap.put / HashSet.add
    void put(i32 key, i32 keyHash) {
        i32 hash = spread(keyHash);
        if (tab.empty()) resize();
        int n = (int)tab.size();
        int i = (n - 1) & hash;
        int p = tab[i];
        if (p < 0) {
            tab[i] = newNode(hash, key, -1, false);
        } else {
            int e;
            if (P(p).hash == hash && P(p).key == key) e = p;
            else if (P(p).tree) e = putTreeVal(p, hash, key);
            else {
                for (int binCount = 0;; ++binCount) {
                    if ((e = P(p).next) < 0) {
                        P(p).next = newNode(hash, key, -1, false);
                        if (binCount >= 7) treeifyBin(hash);
                        break;
                    }
                    if (P(e).hash == hash && P(e).key == key) break;
                    p = e;
                }
            }
            if (e >= 0) return; // existing mapping
        }
        if (++size > threshold) resize();
    }
    // HashMap.compute(key, f) where f returns a non-null value
    void compute(i32 key, i32 keyHash) {
        i32 hash = spread(keyHash);
        if (size > threshold || tab.empty()) resize();
        int n = (int)tab.size();
        int i = (n - 1) & hash;
        int first = tab[i];
        int binCount = 0, t = -1, old = -1;
        if (first >= 0) {
            if (P(first).tree) {
                t = first;
                old = find(P(first).parent >= 0 ? root(first) : first, hash);
            } else {
                int e = first;
                do {
                    if (P(e).hash == hash && P(e).key == key) {
                        old = e;
                        break;
                    }
                    ++binCount;
                } while ((e = P(e).next) >= 0);
            }
        }
        if (old >= 0) return;
        if (t >= 0) putTreeVal(t, hash, key);
        else {
            tab[i] = newNode(hash, key, first, false);
            if (binCount >= 7) treeifyBin(hash);
        }
        ++size;
    }
    // HashMap.remove
    void remove(i32 key, i32 keyHash) {
        i32 hash = spread(keyHash);
        int n = (int)tab.size();
        if (n == 0) return;
        int index = (n - 1) & hash;
        int p = tab[index];
        if (p < 0) return;
        int node = -1, e;
        if (P(p).hash == hash && P(p).key == key) node = p;
        else if ((e = P(p).next) >= 0) {
            if (P(p).tree) node = find(P(p).parent >= 0 ? root(p) : p, hash);
            else {
                do {
                    if (P(e).hash == hash && P(e).key == key) {
                        node = e;
                        break;
                    }
                    p = e;
                } while ((e = P(e).next) >= 0);
            }
        }
        if (node < 0) return;
        if (P(node).tree) removeTreeNode(node);
        else if (node == p) tab[index] = P(node).next;
        else P(p).next = P(node).next;
        --size;
    }
    // keys in iteration order
    void keys(std::vector<i32>& out) {
        out.clear();
        for (int b : tab)
            for (int e = b; e >= 0; e = P(e).next) out.push_back(P(e).key);
    }
};

// The iteration order of a HashMap / HashSet (default capacity) after putting distinct keys with
// the given hashCode()s in order: the indices of the keys
inline std::vector<int> hashMapOrder(const std::vector<i32>& hashCodes) {
    HashMapOrder map;
    for (int k = 0; k < (int)hashCodes.size(); k++) map.put(k, hashCodes[k]);
    std::vector<i32> keys;
    map.keys(keys);
    return std::vector<int>(keys.begin(), keys.end());
}

// java.util.PriorityQueue with a comparator (cmp(a, b) returns an int like Comparator#compare). offer
// and poll sift exactly like the JDK, so equal or inconsistently compared elements come out in the
// jar's order. poll() and peek() need a non-empty queue.
template <class T, class Cmp>
struct PriorityQueue {
    std::vector<T> es; // the heap array, in the order iteration and toArray() see it
    Cmp cmp;
    explicit PriorityQueue(Cmp c) : cmp(c) {}
    int size() const { return (int)es.size(); }
    bool isEmpty() const { return es.empty(); }
    void clear() { es.clear(); }
    const T& peek() const { return es[0]; }
    void offer(const T& value) {
        T x = value;
        int k = (int)es.size();
        es.push_back(x);
        while (k > 0) { // siftUpUsingComparator
            int parent = (k - 1) >> 1;
            T e = es[parent];
            if (cmp(x, e) >= 0) break;
            es[k] = e;
            k = parent;
        }
        es[k] = x;
    }
    T poll() {
        T result = es[0];
        int n = (int)es.size() - 1;
        T x = es[n];
        es.pop_back();
        if (n > 0) { // siftDownUsingComparator
            int k = 0;
            int half = n >> 1;
            while (k < half) {
                int child = (k << 1) + 1;
                T c = es[child];
                int right = child + 1;
                if (right < n && cmp(c, es[right]) > 0) c = es[child = right];
                if (cmp(x, c) <= 0) break;
                es[k] = c;
                k = child;
            }
            es[k] = x;
        }
        return result;
    }
};

namespace detail {
// java.util.TimSort
template <class T, class Cmp>
struct TimSort {
    static const int MIN_MERGE = 32;
    static const int MIN_GALLOP = 7;
    std::vector<T>& a;
    Cmp& c;
    int minGallop = MIN_GALLOP;
    std::vector<T> tmp;
    int stackSize = 0;
    int runBase[49], runLen[49];

    TimSort(std::vector<T>& array, Cmp& cmp) : a(array), c(cmp) {}

    static void sort(std::vector<T>& a, int lo, int hi, Cmp& c) {
        int nRemaining = hi - lo;
        if (nRemaining < 2) return;
        if (nRemaining < MIN_MERGE) {
            int initRunLen = countRunAndMakeAscending(a, lo, hi, c);
            binarySort(a, lo, hi, lo + initRunLen, c);
            return;
        }
        TimSort ts(a, c);
        int minRun = minRunLength(nRemaining);
        do {
            int runLen = countRunAndMakeAscending(a, lo, hi, c);
            if (runLen < minRun) {
                int force = nRemaining <= minRun ? nRemaining : minRun;
                binarySort(a, lo, lo + force, lo + runLen, c);
                runLen = force;
            }
            ts.pushRun(lo, runLen);
            ts.mergeCollapse();
            lo += runLen;
            nRemaining -= runLen;
        } while (nRemaining != 0);
        ts.mergeForceCollapse();
    }

    // System.arraycopy
    static void copy(const std::vector<T>& src, int srcPos, std::vector<T>& dst, int dstPos, int n) {
        if (&src == &dst && dstPos > srcPos) {
            for (int k = n - 1; k >= 0; k--) dst[dstPos + k] = src[srcPos + k];
        } else {
            for (int k = 0; k < n; k++) dst[dstPos + k] = src[srcPos + k];
        }
    }

    static void binarySort(std::vector<T>& a, int lo, int hi, int start, Cmp& c) {
        if (start == lo) start++;
        for (; start < hi; start++) {
            T pivot = a[start];
            int left = lo;
            int right = start;
            while (left < right) {
                int mid = (int)((u32)(left + right) >> 1);
                if (c(pivot, a[mid]) < 0) right = mid;
                else left = mid + 1;
            }
            for (int k = start; k > left; k--) a[k] = a[k - 1];
            a[left] = pivot;
        }
    }

    static int countRunAndMakeAscending(std::vector<T>& a, int lo, int hi, Cmp& c) {
        int runHi = lo + 1;
        if (runHi == hi) return 1;
        if (c(a[runHi++], a[lo]) < 0) { // descending
            while (runHi < hi && c(a[runHi], a[runHi - 1]) < 0) runHi++;
            std::reverse(a.begin() + lo, a.begin() + runHi);
        } else {
            while (runHi < hi && c(a[runHi], a[runHi - 1]) >= 0) runHi++;
        }
        return runHi - lo;
    }

    static int minRunLength(int n) {
        int r = 0;
        while (n >= MIN_MERGE) {
            r |= (n & 1);
            n >>= 1;
        }
        return n + r;
    }

    void pushRun(int base, int len) {
        runBase[stackSize] = base;
        runLen[stackSize] = len;
        stackSize++;
    }

    void mergeCollapse() {
        while (stackSize > 1) {
            int n = stackSize - 2;
            if ((n > 0 && runLen[n - 1] <= runLen[n] + runLen[n + 1]) || (n > 1 && runLen[n - 2] <= runLen[n] + runLen[n - 1])) {
                if (runLen[n - 1] < runLen[n + 1]) n--;
            } else if (n < 0 || runLen[n] > runLen[n + 1]) {
                break;
            }
            mergeAt(n);
        }
    }

    void mergeForceCollapse() {
        while (stackSize > 1) {
            int n = stackSize - 2;
            if (n > 0 && runLen[n - 1] < runLen[n + 1]) n--;
            mergeAt(n);
        }
    }

    void mergeAt(int i) {
        int base1 = runBase[i];
        int len1 = runLen[i];
        int base2 = runBase[i + 1];
        int len2 = runLen[i + 1];
        runLen[i] = len1 + len2;
        if (i == stackSize - 3) {
            runBase[i + 1] = runBase[i + 2];
            runLen[i + 1] = runLen[i + 2];
        }
        stackSize--;
        int k = gallopRight(a[base2], a, base1, len1, 0, c);
        base1 += k;
        len1 -= k;
        if (len1 == 0) return;
        len2 = gallopLeft(a[base1 + len1 - 1], a, base2, len2, len2 - 1, c);
        if (len2 == 0) return;
        if (len1 <= len2) mergeLo(base1, len1, base2, len2);
        else mergeHi(base1, len1, base2, len2);
    }

    static int grow(int ofs) { return (int)(((u32)ofs << 1) + 1); } // (ofs << 1) + 1, wrapping

    static int gallopLeft(const T& key, const std::vector<T>& a, int base, int len, int hint, Cmp& c) {
        int lastOfs = 0;
        int ofs = 1;
        if (c(key, a[base + hint]) > 0) {
            int maxOfs = len - hint;
            while (ofs < maxOfs && c(key, a[base + hint + ofs]) > 0) {
                lastOfs = ofs;
                ofs = grow(ofs);
                if (ofs <= 0) ofs = maxOfs;
            }
            if (ofs > maxOfs) ofs = maxOfs;
            lastOfs += hint;
            ofs += hint;
        } else {
            int maxOfs = hint + 1;
            while (ofs < maxOfs && c(key, a[base + hint - ofs]) <= 0) {
                lastOfs = ofs;
                ofs = grow(ofs);
                if (ofs <= 0) ofs = maxOfs;
            }
            if (ofs > maxOfs) ofs = maxOfs;
            int t = lastOfs;
            lastOfs = hint - ofs;
            ofs = hint - t;
        }
        lastOfs++;
        while (lastOfs < ofs) {
            int m = lastOfs + (int)((u32)(ofs - lastOfs) >> 1);
            if (c(key, a[base + m]) > 0) lastOfs = m + 1;
            else ofs = m;
        }
        return ofs;
    }

    static int gallopRight(const T& key, const std::vector<T>& a, int base, int len, int hint, Cmp& c) {
        int ofs = 1;
        int lastOfs = 0;
        if (c(key, a[base + hint]) < 0) {
            int maxOfs = hint + 1;
            while (ofs < maxOfs && c(key, a[base + hint - ofs]) < 0) {
                lastOfs = ofs;
                ofs = grow(ofs);
                if (ofs <= 0) ofs = maxOfs;
            }
            if (ofs > maxOfs) ofs = maxOfs;
            int t = lastOfs;
            lastOfs = hint - ofs;
            ofs = hint - t;
        } else {
            int maxOfs = len - hint;
            while (ofs < maxOfs && c(key, a[base + hint + ofs]) >= 0) {
                lastOfs = ofs;
                ofs = grow(ofs);
                if (ofs <= 0) ofs = maxOfs;
            }
            if (ofs > maxOfs) ofs = maxOfs;
            lastOfs += hint;
            ofs += hint;
        }
        lastOfs++;
        while (lastOfs < ofs) {
            int m = lastOfs + (int)((u32)(ofs - lastOfs) >> 1);
            if (c(key, a[base + m]) < 0) ofs = m;
            else lastOfs = m + 1;
        }
        return ofs;
    }

    static Exception contractViolated() { return Exception{"java.lang.IllegalArgumentException: Comparison method violates its general contract!"}; }

    void mergeLo(int base1, int len1, int base2, int len2) {
        if ((int)tmp.size() < len1) tmp.resize(len1);
        int cursor1 = 0;
        int cursor2 = base2;
        int dest = base1;
        copy(a, base1, tmp, cursor1, len1);
        a[dest++] = a[cursor2++];
        if (--len2 == 0) {
            copy(tmp, cursor1, a, dest, len1);
            return;
        }
        if (len1 == 1) {
            copy(a, cursor2, a, dest, len2);
            a[dest + len2] = tmp[cursor1];
            return;
        }
        int mg = minGallop;
        while (true) {
            int count1 = 0;
            int count2 = 0;
            do {
                if (c(a[cursor2], tmp[cursor1]) < 0) {
                    a[dest++] = a[cursor2++];
                    count2++;
                    count1 = 0;
                    if (--len2 == 0) goto done;
                } else {
                    a[dest++] = tmp[cursor1++];
                    count1++;
                    count2 = 0;
                    if (--len1 == 1) goto done;
                }
            } while ((count1 | count2) < mg);
            do {
                count1 = gallopRight(a[cursor2], tmp, cursor1, len1, 0, c);
                if (count1 != 0) {
                    copy(tmp, cursor1, a, dest, count1);
                    dest += count1;
                    cursor1 += count1;
                    len1 -= count1;
                    if (len1 <= 1) goto done;
                }
                a[dest++] = a[cursor2++];
                if (--len2 == 0) goto done;
                count2 = gallopLeft(tmp[cursor1], a, cursor2, len2, 0, c);
                if (count2 != 0) {
                    copy(a, cursor2, a, dest, count2);
                    dest += count2;
                    cursor2 += count2;
                    len2 -= count2;
                    if (len2 == 0) goto done;
                }
                a[dest++] = tmp[cursor1++];
                if (--len1 == 1) goto done;
                mg--;
            } while ((count1 >= MIN_GALLOP) | (count2 >= MIN_GALLOP));
            if (mg < 0) mg = 0;
            mg += 2;
        }
    done:
        minGallop = mg < 1 ? 1 : mg;
        if (len1 == 1) {
            copy(a, cursor2, a, dest, len2);
            a[dest + len2] = tmp[cursor1];
        } else if (len1 == 0) {
            throw contractViolated();
        } else {
            copy(tmp, cursor1, a, dest, len1);
        }
    }

    void mergeHi(int base1, int len1, int base2, int len2) {
        if ((int)tmp.size() < len2) tmp.resize(len2);
        int tmpBase = 0;
        copy(a, base2, tmp, tmpBase, len2);
        int cursor1 = base1 + len1 - 1;
        int cursor2 = tmpBase + len2 - 1;
        int dest = base2 + len2 - 1;
        a[dest--] = a[cursor1--];
        if (--len1 == 0) {
            copy(tmp, tmpBase, a, dest - (len2 - 1), len2);
            return;
        }
        if (len2 == 1) {
            dest -= len1;
            cursor1 -= len1;
            copy(a, cursor1 + 1, a, dest + 1, len1);
            a[dest] = tmp[cursor2];
            return;
        }
        int mg = minGallop;
        while (true) {
            int count1 = 0;
            int count2 = 0;
            do {
                if (c(tmp[cursor2], a[cursor1]) < 0) {
                    a[dest--] = a[cursor1--];
                    count1++;
                    count2 = 0;
                    if (--len1 == 0) goto done;
                } else {
                    a[dest--] = tmp[cursor2--];
                    count2++;
                    count1 = 0;
                    if (--len2 == 1) goto done;
                }
            } while ((count1 | count2) < mg);
            do {
                count1 = len1 - gallopRight(tmp[cursor2], a, base1, len1, len1 - 1, c);
                if (count1 != 0) {
                    dest -= count1;
                    cursor1 -= count1;
                    len1 -= count1;
                    copy(a, cursor1 + 1, a, dest + 1, count1);
                    if (len1 == 0) goto done;
                }
                a[dest--] = tmp[cursor2--];
                if (--len2 == 1) goto done;
                count2 = len2 - gallopLeft(a[cursor1], tmp, tmpBase, len2, len2 - 1, c);
                if (count2 != 0) {
                    dest -= count2;
                    cursor2 -= count2;
                    len2 -= count2;
                    copy(tmp, cursor2 + 1, a, dest + 1, count2);
                    if (len2 <= 1) goto done;
                }
                a[dest--] = a[cursor1--];
                if (--len1 == 0) goto done;
                mg--;
            } while ((count1 >= MIN_GALLOP) | (count2 >= MIN_GALLOP));
            if (mg < 0) mg = 0;
            mg += 2;
        }
    done:
        minGallop = mg < 1 ? 1 : mg;
        if (len2 == 1) {
            dest -= len1;
            cursor1 -= len1;
            copy(a, cursor1 + 1, a, dest + 1, len1);
            a[dest] = tmp[cursor2];
        } else if (len2 == 0) {
            throw contractViolated();
        } else {
            copy(tmp, tmpBase, a, dest - (len2 - 1), len2);
        }
    }
};
} // namespace detail

// List.sort / Collections.sort / Arrays.sort(a, comparator) / Stream.sorted (java.util.TimSort): cmp(a, b)
// returns an int like Comparator#compare. Stable; with a comparator that breaks its contract the result
// is still the jar's, including the java.lang.IllegalArgumentException TimSort may throw (thrown as a
// java::Exception, the list then partly merged like in Java).
template <class T, class Cmp>
inline void sort(std::vector<T>& list, Cmp cmp) {
    detail::TimSort<T, Cmp>::sort(list, 0, (int)list.size(), cmp);
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

    // SecureRandom.setSeed(byte[])
    void engineSetSeed(const u8* seed, size_t n) {
        std::vector<u8> msg;
        if (haveState) msg.insert(msg.end(), state, state + DIGEST_SIZE);
        msg.insert(msg.end(), seed, seed + n);
        SHA1::hash(msg.data(), msg.size(), state);
        haveState = true;
        remCount = 0;
    }

    // SecureRandom.longToByteArray: the 8 bytes setSeed(long) seeds with, low byte first
    static void longToByteArray(i64 v, u8 out[8]) {
        for (int i = 0; i < 8; i++) out[i] = (u8)(((u64)v >> (8 * i)) & 0xff);
    }

    // SecureRandom.setSeed(long): 0 is ignored, the engine then seeds itself from system entropy
    // on first use (not reproducible: -seed 0 plays a random game with the jar too)
    void setSeed(i64 seed) {
        if (seed == 0) return;
        u8 b[8];
        longToByteArray(seed, b);
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
