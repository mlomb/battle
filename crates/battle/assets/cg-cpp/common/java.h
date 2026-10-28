// Java semantics for C++ ports of CodinGame referees: the bits of the JDK a referee observably depends
// on, reproduced exactly (checked against the JDK the jars run on). Shared by the referees of this
// folder: `#include "common/java.h"` (battle's bundler inlines it). A referee keeps no JDK behaviour
// of its own: what it needs and is missing here belongs here, with a check against the JDK.
//
//   - numbers: (int)/(long) casts of doubles, int arithmetic (/ and % included), Math.round (double, float)/abs/min/max,
//     Integer.compare/signum, Double.compare, DoubleStream.sum, Double.parseDouble, Double.toString, Float.toString,
//     DecimalFormat("0.##"), String.format("%.Nf"), Math.atan2 (fdlibm), Math.toDegrees, Math.sin / Math.cos
//     (HotSpot's x86_64 intrinsics), Math.signum(double), java.awt.geom.Line2D.ptSegDistSq / ptSegDist, Point2D#distance
//   - strings (java::String is UTF-16 like java.lang.String; a std::string is UTF-8 text):
//     new String(bytes, UTF_8) with the JDK's replacement of malformed input, String.getBytes(UTF_8),
//     trim (and Kotlin's), replace, split (and split("\\s+")), startsWith, hashCode (and java.awt.Point's),
//     toUpperCase/toLowerCase compared with ASCII text, equalsIgnoreCase with ASCII text,
//     Integer.parseInt / Long.parseLong, Boolean.parseBoolean, the regex classes \s \d \w, the texts of the JDK's exceptions
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
inline i32 neg(i32 a) { return (i32)(0u - (u32)a); }
// int / and %: truncated toward zero, MIN_VALUE / -1 wraps to MIN_VALUE (remainder 0), a zero divisor throws
inline i32 div(i32 a, i32 b) {
    if (b == 0) throw Exception{"java.lang.ArithmeticException: / by zero"};
    return b == -1 ? neg(a) : a / b;
}
inline i32 rem(i32 a, i32 b) {
    if (b == 0) throw Exception{"java.lang.ArithmeticException: / by zero"};
    return b == -1 ? 0 : a % b;
}

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

// Math.round(float): floor(a + 1/2) computed exactly, saturated to the int range (a separate name, so
// that a float argument never picks it instead of round(double) by accident)
inline i32 roundFloat(float a) {
    if (a != a) return 0;
    double d = a; // exact
    if (std::fabs(d) >= 8388608.0) return d2i(d); // already an integer
    double f = std::floor(d);
    return d2i((d - f >= 0.5) ? f + 1.0 : f); // d - f is exact here
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

// Double.toString(double) (JDK 19+): the shortest decimal that rounds to the double (the closest one,
// then the even one, if several; when that has 1 digit, a closer 2-digit decimal wins), written as
// integer.fraction when 10^-3 <= |d| < 10^7 (at least one fraction digit), else as d.dddE<n>.
// Float.toString(float) (JDK 19+) follows the same rules with the decimals that round to the float.
namespace detail {
// The significant digits of a > 0 (finite, a double or a float) in scientific notation as std::to_chars
// writes them: a = d.ddd x 10^exp. precision < 0: the shortest that round-trips (to a's type); else that
// many digits after the first one, correctly rounded.
template <class T>
inline void sciDigits(T a, int precision, std::string& digits, int& exp) {
    char buf[1100];
    char* end = precision < 0 ? std::to_chars(buf, buf + sizeof buf, a, std::chars_format::scientific).ptr
                              : std::to_chars(buf, buf + sizeof buf, a, std::chars_format::scientific, precision).ptr;
    *end = 0;
    digits.clear();
    const char* p = buf;
    for (; p < end && *p != 'e'; p++)
        if (*p != '.') digits += *p;
    exp = atoi(p + 1);
}

// Double.toString (T = double) / Float.toString (T = float)
template <class T>
inline std::string fpToString(T v) {
    if (v != v) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
    T a = std::fabs(v);
    std::string digits;
    int exp;
    detail::sciDigits(a, -1, digits, exp);
    if (digits.size() == 1) {
        std::string d2;
        int e2;
        detail::sciDigits(a, 1, d2, e2); // the closest 2-digit decimal
        std::string text = d2.substr(0, 1) + "." + d2.substr(1) + "e" + std::to_string(e2);
        T back = 0;
        std::from_chars(text.data(), text.data() + text.size(), back);
        if (d2[1] != '0' && back == a) {
            digits = d2;
            exp = e2;
        }
    }
    std::string r = v < 0 ? "-" : "";
    int n = (int)digits.size();
    if (exp >= -3 && exp < 7) {
        if (exp >= 0) {
            for (int i = 0; i <= exp; i++) r += i < n ? digits[i] : '0';
            r += '.';
            r += exp + 1 < n ? digits.substr(exp + 1) : "0";
        } else {
            r += "0.";
            r.append(-exp - 1, '0');
            r += digits;
        }
    } else {
        r += digits[0];
        r += '.';
        r += n > 1 ? digits.substr(1) : "0";
        r += "E" + std::to_string(exp);
    }
    return r;
}
} // namespace detail

inline std::string doubleToString(double v) { return detail::fpToString(v); }

// Float.toString(float), e.g. `"" + f` of a float field (Game of Drones' (float) Math.atan2 angles). Checked against
// JDK 26 on every float (all 2^32 bit patterns).
inline std::string floatToString(float v) { return detail::fpToString(v); }

// new DecimalFormat("0.#...#") (maxFraction '#'s, '.' as the decimal separator, no grouping) .format(v)
// for a finite v. java.text.DigitList rounds the digits of the shortest decimal (not the exact binary
// value: 1e23 gives 100000000000000000000000) half-even, and when they end exactly on the tie it
// looks at whether they are the exact value or were rounded up or down. Trailing zeros are dropped,
// and "-" is kept for negative values (and -0.0) that round to 0 ("-0").
inline std::string formatDecimal(double v, int maxFraction) {
    double a = std::fabs(v);
    std::string d; // the significant digits: a = 0.d1d2... x 10^decimalAt
    int decimalAt = 0;
    if (a != 0) {
        int exp;
        detail::sciDigits(a, -1, d, exp);
        while (d.size() > 1 && d.back() == '0') d.pop_back();
        decimalAt = exp + 1;
        int maxDigits = decimalAt + maxFraction;
        if (maxDigits < 0) {
            d.clear();
        } else if (maxDigits < (int)d.size()) {
            bool up;
            char c = d[maxDigits];
            if (c != '5') {
                up = c > '5';
            } else if (maxDigits < (int)d.size() - 1) {
                up = true; // a non-zero digit follows
            } else {
                // the digits end on the tie: is the exact value below, above or on it?
                std::string x;
                int ex, cmp = 0;
                detail::sciDigits(a, 767, x, ex);
                if (exp != ex) {
                    cmp = exp > ex ? 1 : -1;
                } else {
                    for (size_t i = 0; i < std::max(d.size(), x.size()) && cmp == 0; i++) {
                        char p = i < d.size() ? d[i] : '0', q = i < x.size() ? x[i] : '0';
                        if (p != q) cmp = p > q ? 1 : -1;
                    }
                }
                if (cmp > 0) up = false;     // the digits were rounded up
                else if (cmp < 0) up = true; // truncated
                else up = maxDigits > 0 && (d[maxDigits - 1] - '0') % 2 != 0;
            }
            d.resize(maxDigits);
            if (up) {
                int i = maxDigits - 1;
                while (i >= 0 && d[i] == '9') d.pop_back(), i--;
                if (i < 0) {
                    d = "1";
                    decimalAt++;
                } else {
                    d[i]++;
                }
            }
            while (!d.empty() && d.back() == '0') d.pop_back();
        }
    }
    std::string r = std::signbit(v) ? "-" : "";
    if (d.empty()) return r + "0";
    int n = (int)d.size();
    if (decimalAt <= 0) {
        r += "0";
    } else {
        for (int i = 0; i < decimalAt; i++) r += i < n ? d[i] : '0';
    }
    if (n > decimalAt) {
        r += ".";
        if (decimalAt < 0) r.append(-decimalAt, '0');
        r += d.substr(std::max(decimalAt, 0));
    }
    return r;
}

// String.format(Locale.US, "%.<precision>f", v) for a finite v, as JDK 21+ writes it (java.util.Formatter on
// jdk.internal.math.FormattedFPDecimal): the decimal digits of Double.toString(v) (the shortest decimal, as
// doubleToString picks it) rounded half up to `precision` fraction digits, no grouping, "-" for a v below +0.0
// (Double.compare: -0.0 and negative values rounding to 0 included). Checked against JDK 26 on 50M values (unit
// interval, exact ties of the shortest digits, tiny and wide magnitudes, both signs, precisions 0 to 5).
inline std::string formatFixed(double v, int precision) {
    std::string r = (v < 0 || (v == 0 && std::signbit(v))) ? "-" : "";
    double a = std::fabs(v);
    std::string d; // a = 0.d1d2... x 10^decimalAt
    int decimalAt = 0;
    if (a != 0) {
        int exp;
        detail::sciDigits(a, -1, d, exp);
        if (d.size() == 1) { // doubleToString's rule: a closer 2-digit decimal wins
            std::string d2;
            int e2;
            detail::sciDigits(a, 1, d2, e2);
            std::string text = d2.substr(0, 1) + "." + d2.substr(1) + "e" + std::to_string(e2);
            double back = 0;
            std::from_chars(text.data(), text.data() + text.size(), back);
            if (d2[1] != '0' && back == a) {
                d = d2;
                exp = e2;
            }
        }
        while (d.size() > 1 && d.back() == '0') d.pop_back();
        decimalAt = exp + 1;
        long long keep = (long long)decimalAt + precision; // the digits kept
        if (keep < 0) {
            d.clear();
        } else if (keep < (long long)d.size()) {
            bool up = d[(size_t)keep] >= '5';
            d.resize((size_t)keep);
            if (up) {
                int i = (int)keep - 1;
                while (i >= 0 && d[i] == '9') d[i--] = '0';
                if (i < 0) {
                    d.insert(d.begin(), '1');
                    decimalAt++;
                } else {
                    d[i]++;
                }
            }
        }
        if (d.empty() || d.find_first_not_of('0') == std::string::npos) {
            d.clear();
            decimalAt = 0;
        }
    }
    int n = (int)d.size();
    if (decimalAt <= 0) {
        r += "0";
    } else {
        for (int i = 0; i < decimalAt; i++) r += i < n ? d[i] : '0';
    }
    if (precision > 0) {
        r += ".";
        for (int k = 0; k < precision; k++) {
            int i = decimalAt + k; // the digit at 10^-(k+1)
            r += (i >= 0 && i < n) ? d[i] : '0';
        }
    }
    return r;
}

// Math.atan2 (StrictMath.atan2: fdlibm's e_atan2.c and s_atan.c, as the JDK's FdLibm)
namespace detail {
inline i32 hiWord(double x) {
    u64 b;
    memcpy(&b, &x, 8);
    return (i32)(u32)(b >> 32);
}
inline u32 loWord(double x) {
    u64 b;
    memcpy(&b, &x, 8);
    return (u32)b;
}
inline double fdlibmAtan(double x) {
    static const double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01, 1.57079632679489655800e+00};
    static const double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17, 6.12323399573676603587e-17};
    static const double aT[] = {3.33333333333329318027e-01,  -1.99999999998764832476e-01, 1.42857142725034663711e-01,  -1.11111104054623557880e-01,
                                9.09088713343650656196e-02,  -7.69187620504482999495e-02, 6.66107313738753120669e-02,  -5.83357013379057348645e-02,
                                4.97687799461593236017e-02,  -3.65315727442169155270e-02, 1.62858201153657823623e-02};
    const double one = 1.0, huge = 1.0e300;
    double w, s1, s2, z;
    int id;
    i32 hx = hiWord(x);
    i32 ix = hx & 0x7fffffff;
    if (ix >= 0x44100000) { // |x| >= 2^66
        if (ix > 0x7ff00000 || (ix == 0x7ff00000 && loWord(x) != 0)) return x + x; // NaN
        if (hx > 0) return atanhi[3] + atanlo[3];
        return -atanhi[3] - atanlo[3];
    }
    if (ix < 0x3fdc0000) { // |x| < 0.4375
        if (ix < 0x3e400000) { // |x| < 2^-27
            if (huge + x > one) return x;
        }
        id = -1;
    } else {
        x = std::fabs(x);
        if (ix < 0x3ff30000) { // |x| < 1.1875
            if (ix < 0x3fe60000) { // 7/16 <= |x| < 11/16
                id = 0;
                x = (2.0 * x - one) / (2.0 + x);
            } else { // 11/16 <= |x| < 19/16
                id = 1;
                x = (x - one) / (x + one);
            }
        } else {
            if (ix < 0x40038000) { // |x| < 2.4375
                id = 2;
                x = (x - 1.5) / (one + 1.5 * x);
            } else { // 2.4375 <= |x| < 2^66
                id = 3;
                x = -1.0 / x;
            }
        }
    }
    z = x * x;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return x - x * (s1 + s2);
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return (hx < 0) ? -z : z;
}
} // namespace detail

inline double atan2(double y, double x) {
    const double tiny = 1.0e-300, zero = 0.0, pi_o_4 = 7.8539816339744827900E-01, pi_o_2 = 1.5707963267948965580E+00,
                 pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
    double z;
    i32 hx = detail::hiWord(x), ix = hx & 0x7fffffff;
    u32 lx = detail::loWord(x);
    i32 hy = detail::hiWord(y), iy = hy & 0x7fffffff;
    u32 ly = detail::loWord(y);
    if ((u32)(ix | (i32)((lx | (0u - lx)) >> 31)) > 0x7ff00000u || (u32)(iy | (i32)((ly | (0u - ly)) >> 31)) > 0x7ff00000u) return x + y; // a NaN
    if ((((u32)hx - 0x3ff00000u) | lx) == 0) return detail::fdlibmAtan(y); // x = 1.0
    int m = ((hy >> 31) & 1) | ((hx >> 30) & 2); // 2 * sign(x) + sign(y)
    if ((iy | (i32)ly) == 0) { // y = 0
        switch (m) {
            case 0:
            case 1: return y;
            case 2: return pi + tiny;
            default: return -pi - tiny;
        }
    }
    if ((ix | (i32)lx) == 0) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny; // x = 0
    if (ix == 0x7ff00000) { // x is INF
        if (iy == 0x7ff00000) {
            switch (m) {
                case 0: return pi_o_4 + tiny;
                case 1: return -pi_o_4 - tiny;
                case 2: return 3.0 * pi_o_4 + tiny;
                default: return -3.0 * pi_o_4 - tiny;
            }
        } else {
            switch (m) {
                case 0: return zero;
                case 1: return -zero;
                case 2: return pi + tiny;
                default: return -pi - tiny;
            }
        }
    }
    if (iy == 0x7ff00000) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny; // y is INF
    int k = (iy - ix) >> 20;
    if (k > 60) z = pi_o_2 + 0.5 * pi_lo;       // |y/x| > 2^60
    else if (hx < 0 && k < -60) z = 0.0;        // |y|/x < -2^60
    else z = detail::fdlibmAtan(std::fabs(y / x));
    switch (m) {
        case 0: return z;
        case 1: return -z;
        case 2: return pi - (z - pi_lo);
        default: return (z - pi_lo) - pi;
    }
}

// Math.toDegrees (JDK 9+: angrad * RADIANS_TO_DEGREES)
inline double toDegrees(double angrad) { return angrad * 57.29577951308232; }

// Math.signum(double): the argument itself for +-0.0 and NaN, else +-1.0 (HotSpot's intrinsic computes the same)
inline double signum(double d) { return (d == 0.0 || d != d) ? d : std::copysign(1.0, d); }

// java.awt.geom.Line2D.ptSegDistSq(x1, y1, x2, y2, px, py): the square of the distance from (px, py) to the segment,
// in the JDK's operation order (also Line2D.Double#ptSegDistSq(Point2D)); ptSegDist is Math.sqrt of it
inline double ptSegDistSq(double x1, double y1, double x2, double y2, double px, double py) {
    x2 -= x1;
    y2 -= y1;
    px -= x1;
    py -= y1;
    double dotprod = px * x2 + py * y2;
    double projlenSq;
    if (dotprod <= 0.0) {
        projlenSq = 0.0;
    } else {
        px = x2 - px;
        py = y2 - py;
        dotprod = px * x2 + py * y2;
        if (dotprod <= 0.0) projlenSq = 0.0;
        else projlenSq = dotprod * dotprod / (x2 * x2 + y2 * y2);
    }
    double lenSq = px * px + py * py - projlenSq;
    if (lenSq < 0) lenSq = 0;
    return lenSq;
}
inline double ptSegDist(double x1, double y1, double x2, double y2, double px, double py) { return std::sqrt(ptSegDistSq(x1, y1, x2, y2, px, py)); }

// java.awt.geom.Point2D#distance(px, py) of the point (x, y) (e.g. a java.awt.Point's, x and y its int coordinates):
// px -= x, py -= y, Math.sqrt(px * px + py * py), in the JDK's operation order
inline double pointDistance(double x, double y, double px, double py) {
    px -= x;
    py -= y;
    return std::sqrt(px * px + py * py);
}

// Math.sin / Math.cos as HotSpot computes them on x86_64 (JDK 9+): not StrictMath's fdlibm (they differ in the last
// bit for ~3% of arguments) but the stubs of Intel's LIBM (StubGenerator::generate_libmSin / generate_libmCos, used
// by the interpreter and both compilers alike), translated here instruction by instruction (xmm registers as their
// two lanes; the same IEEE operations in the same order, FP_CONTRACT off). Checked bit for bit against JDK 26's
// Math.sin / Math.cos on 900M arguments (cg-2017-mean-max's validation: its spawn angles, |x| < 8, |x| < 90000, random bit
// patterns with tiny and subnormal values, the edges of the pi/32 table). Only their main path, |x| < 90112 (+-0, tiny and subnormal
// arguments, Inf and NaN included): beyond, the stubs reduce the argument with a multi-word 2/pi table, not modelled
// (the port aborts). Note: a jar on another architecture (e.g. aarch64's own stubs) may compute other last bits.
namespace detail {
struct Xmm {
    double lo, hi;
};
inline double bitsToDouble(u64 b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}
inline u64 doubleToBits(double d) {
    u64 b;
    memcpy(&b, &d, 8);
    return b;
}
inline double juints(u32 lo, u32 hi) { return bitsToDouble(((u64)hi << 32) | lo); }
// StubGenerator's Ctable: 64 entries of 4 doubles (each a pair of juints, low word first)
inline const u32 LIBM_CTABLE[] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3ff00000, 0x176d6d31, 0xbf73b92e,
    0xbc29b42c, 0x3fb917a6, 0xe0000000, 0xbc3e2718, 0x00000000, 0x3ff00000, 0x011469fb, 0xbf93ad06, 0x3c69a60b, 0x3fc8f8b8,
    0xc0000000, 0xbc626d19, 0x00000000, 0x3ff00000, 0x939d225a, 0xbfa60bea, 0x2ed59f06, 0x3fd29406, 0xa0000000, 0xbc75d28d,
    0x00000000, 0x3ff00000, 0x866b95cf, 0xbfb37ca1, 0xa6aea963, 0x3fd87de2, 0xe0000000, 0xbc672ced, 0x00000000, 0x3ff00000,
    0x73fa1279, 0xbfbe3a68, 0x3806f63b, 0x3fde2b5d, 0x20000000, 0x3c5e0d89, 0x00000000, 0x3ff00000, 0x5bc57974, 0xbfc59267,
    0x39ae68c8, 0x3fe1c73b, 0x20000000, 0x3c8b25dd, 0x00000000, 0x3ff00000, 0x53aba2fd, 0xbfcd0dfe, 0x25091dd6, 0x3fe44cf3,
    0x20000000, 0x3c68076a, 0x00000000, 0x3ff00000, 0x99fcef32, 0x3fca8279, 0x667f3bcd, 0x3fe6a09e, 0x20000000, 0xbc8bdd34,
    0x00000000, 0x3fe00000, 0x94247758, 0x3fc133cc, 0x6b151741, 0x3fe8bc80, 0x20000000, 0xbc82c5e1, 0x00000000, 0x3fe00000,
    0x9ae68c87, 0x3fac73b3, 0x290ea1a3, 0x3fea9b66, 0xe0000000, 0x3c39f630, 0x00000000, 0x3fe00000, 0x7f909c4e, 0xbf9d4a2c,
    0xf180bdb1, 0x3fec38b2, 0x80000000, 0xbc76e0b1, 0x00000000, 0x3fe00000, 0x65455a75, 0xbfbe0875, 0xcf328d46, 0x3fed906b,
    0x20000000, 0x3c7457e6, 0x00000000, 0x3fe00000, 0x76acf82d, 0x3fa4a031, 0x56c62dda, 0x3fee9f41, 0xe0000000, 0x3c8760b1,
    0x00000000, 0x3fd00000, 0x0e5967d5, 0xbfac1d1f, 0xcff75cb0, 0x3fef6297, 0x20000000, 0x3c756217, 0x00000000, 0x3fd00000,
    0x0f592f50, 0xbf9ba165, 0xa3d12526, 0x3fefd88d, 0x40000000, 0xbc887df6, 0x00000000, 0x3fc00000, 0x00000000, 0x00000000,
    0x00000000, 0x3ff00000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0f592f50, 0x3f9ba165, 0xa3d12526, 0x3fefd88d,
    0x40000000, 0xbc887df6, 0x00000000, 0xbfc00000, 0x0e5967d5, 0x3fac1d1f, 0xcff75cb0, 0x3fef6297, 0x20000000, 0x3c756217,
    0x00000000, 0xbfd00000, 0x76acf82d, 0xbfa4a031, 0x56c62dda, 0x3fee9f41, 0xe0000000, 0x3c8760b1, 0x00000000, 0xbfd00000,
    0x65455a75, 0x3fbe0875, 0xcf328d46, 0x3fed906b, 0x20000000, 0x3c7457e6, 0x00000000, 0xbfe00000, 0x7f909c4e, 0x3f9d4a2c,
    0xf180bdb1, 0x3fec38b2, 0x80000000, 0xbc76e0b1, 0x00000000, 0xbfe00000, 0x9ae68c87, 0xbfac73b3, 0x290ea1a3, 0x3fea9b66,
    0xe0000000, 0x3c39f630, 0x00000000, 0xbfe00000, 0x94247758, 0xbfc133cc, 0x6b151741, 0x3fe8bc80, 0x20000000, 0xbc82c5e1,
    0x00000000, 0xbfe00000, 0x99fcef32, 0xbfca8279, 0x667f3bcd, 0x3fe6a09e, 0x20000000, 0xbc8bdd34, 0x00000000, 0xbfe00000,
    0x53aba2fd, 0x3fcd0dfe, 0x25091dd6, 0x3fe44cf3, 0x20000000, 0x3c68076a, 0x00000000, 0xbff00000, 0x5bc57974, 0x3fc59267,
    0x39ae68c8, 0x3fe1c73b, 0x20000000, 0x3c8b25dd, 0x00000000, 0xbff00000, 0x73fa1279, 0x3fbe3a68, 0x3806f63b, 0x3fde2b5d,
    0x20000000, 0x3c5e0d89, 0x00000000, 0xbff00000, 0x866b95cf, 0x3fb37ca1, 0xa6aea963, 0x3fd87de2, 0xe0000000, 0xbc672ced,
    0x00000000, 0xbff00000, 0x939d225a, 0x3fa60bea, 0x2ed59f06, 0x3fd29406, 0xa0000000, 0xbc75d28d, 0x00000000, 0xbff00000,
    0x011469fb, 0x3f93ad06, 0x3c69a60b, 0x3fc8f8b8, 0xc0000000, 0xbc626d19, 0x00000000, 0xbff00000, 0x176d6d31, 0x3f73b92e,
    0xbc29b42c, 0x3fb917a6, 0xe0000000, 0xbc3e2718, 0x00000000, 0xbff00000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0xbff00000, 0x176d6d31, 0x3f73b92e, 0xbc29b42c, 0xbfb917a6, 0xe0000000, 0x3c3e2718,
    0x00000000, 0xbff00000, 0x011469fb, 0x3f93ad06, 0x3c69a60b, 0xbfc8f8b8, 0xc0000000, 0x3c626d19, 0x00000000, 0xbff00000,
    0x939d225a, 0x3fa60bea, 0x2ed59f06, 0xbfd29406, 0xa0000000, 0x3c75d28d, 0x00000000, 0xbff00000, 0x866b95cf, 0x3fb37ca1,
    0xa6aea963, 0xbfd87de2, 0xe0000000, 0x3c672ced, 0x00000000, 0xbff00000, 0x73fa1279, 0x3fbe3a68, 0x3806f63b, 0xbfde2b5d,
    0x20000000, 0xbc5e0d89, 0x00000000, 0xbff00000, 0x5bc57974, 0x3fc59267, 0x39ae68c8, 0xbfe1c73b, 0x20000000, 0xbc8b25dd,
    0x00000000, 0xbff00000, 0x53aba2fd, 0x3fcd0dfe, 0x25091dd6, 0xbfe44cf3, 0x20000000, 0xbc68076a, 0x00000000, 0xbff00000,
    0x99fcef32, 0xbfca8279, 0x667f3bcd, 0xbfe6a09e, 0x20000000, 0x3c8bdd34, 0x00000000, 0xbfe00000, 0x94247758, 0xbfc133cc,
    0x6b151741, 0xbfe8bc80, 0x20000000, 0x3c82c5e1, 0x00000000, 0xbfe00000, 0x9ae68c87, 0xbfac73b3, 0x290ea1a3, 0xbfea9b66,
    0xe0000000, 0xbc39f630, 0x00000000, 0xbfe00000, 0x7f909c4e, 0x3f9d4a2c, 0xf180bdb1, 0xbfec38b2, 0x80000000, 0x3c76e0b1,
    0x00000000, 0xbfe00000, 0x65455a75, 0x3fbe0875, 0xcf328d46, 0xbfed906b, 0x20000000, 0xbc7457e6, 0x00000000, 0xbfe00000,
    0x76acf82d, 0xbfa4a031, 0x56c62dda, 0xbfee9f41, 0xe0000000, 0xbc8760b1, 0x00000000, 0xbfd00000, 0x0e5967d5, 0x3fac1d1f,
    0xcff75cb0, 0xbfef6297, 0x20000000, 0xbc756217, 0x00000000, 0xbfd00000, 0x0f592f50, 0x3f9ba165, 0xa3d12526, 0xbfefd88d,
    0x40000000, 0x3c887df6, 0x00000000, 0xbfc00000, 0x00000000, 0x00000000, 0x00000000, 0xbff00000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x0f592f50, 0xbf9ba165, 0xa3d12526, 0xbfefd88d, 0x40000000, 0x3c887df6, 0x00000000, 0x3fc00000,
    0x0e5967d5, 0xbfac1d1f, 0xcff75cb0, 0xbfef6297, 0x20000000, 0xbc756217, 0x00000000, 0x3fd00000, 0x76acf82d, 0x3fa4a031,
    0x56c62dda, 0xbfee9f41, 0xe0000000, 0xbc8760b1, 0x00000000, 0x3fd00000, 0x65455a75, 0xbfbe0875, 0xcf328d46, 0xbfed906b,
    0x20000000, 0xbc7457e6, 0x00000000, 0x3fe00000, 0x7f909c4e, 0xbf9d4a2c, 0xf180bdb1, 0xbfec38b2, 0x80000000, 0x3c76e0b1,
    0x00000000, 0x3fe00000, 0x9ae68c87, 0x3fac73b3, 0x290ea1a3, 0xbfea9b66, 0xe0000000, 0xbc39f630, 0x00000000, 0x3fe00000,
    0x94247758, 0x3fc133cc, 0x6b151741, 0xbfe8bc80, 0x20000000, 0x3c82c5e1, 0x00000000, 0x3fe00000, 0x99fcef32, 0x3fca8279,
    0x667f3bcd, 0xbfe6a09e, 0x20000000, 0x3c8bdd34, 0x00000000, 0x3fe00000, 0x53aba2fd, 0xbfcd0dfe, 0x25091dd6, 0xbfe44cf3,
    0x20000000, 0xbc68076a, 0x00000000, 0x3ff00000, 0x5bc57974, 0xbfc59267, 0x39ae68c8, 0xbfe1c73b, 0x20000000, 0xbc8b25dd,
    0x00000000, 0x3ff00000, 0x73fa1279, 0xbfbe3a68, 0x3806f63b, 0xbfde2b5d, 0x20000000, 0xbc5e0d89, 0x00000000, 0x3ff00000,
    0x866b95cf, 0xbfb37ca1, 0xa6aea963, 0xbfd87de2, 0xe0000000, 0x3c672ced, 0x00000000, 0x3ff00000, 0x939d225a, 0xbfa60bea,
    0x2ed59f06, 0xbfd29406, 0xa0000000, 0x3c75d28d, 0x00000000, 0x3ff00000, 0x011469fb, 0xbf93ad06, 0x3c69a60b, 0xbfc8f8b8,
    0xc0000000, 0x3c626d19, 0x00000000, 0x3ff00000, 0x176d6d31, 0xbf73b92e, 0xbc29b42c, 0xbfb917a6, 0xe0000000, 0x3c3e2718,
    0x00000000, 0x3ff00000,
};
// the double at a byte offset of Ctable ([rax + k] with rax = Ctable + (index << 5))
inline double libmCtable(int byteOffset) { return juints(LIBM_CTABLE[byteOffset / 4], LIBM_CTABLE[byteOffset / 4 + 1]); }

// The main path shared by both stubs, from the reduction (x in xmm0, PI32INV * x + 0.5 * sign(x) in xmm1, N its
// truncation) to the result: the two stubs only differ in a few register moves and the table offset (cos: N + 16)
inline double libmSinCos(double x, bool cosine) {
    const double PI32INV = juints(0x6dc9c883, 0x40245f30), ONEHALF = juints(0x00000000, 0x3fe00000);
    const double P_1 = juints(0x54400000, 0x3fb921fb), P_2 = juints(0x1a600000, 0x3d90b461), P_3 = juints(0x2e037073, 0x3b63198a);
    const Xmm SC_1 = {juints(0x55555555, 0xbfc55555), juints(0x00000000, 0xbfe00000)};
    const Xmm SC_2 = {juints(0x11111111, 0x3f811111), juints(0x55555555, 0x3fa55555)};
    const Xmm SC_3 = {juints(0x1a01a01a, 0xbf2a01a0), juints(0x16c16c17, 0xbf56c16c)};
    const Xmm SC_4 = {juints(0xa556c734, 0x3ec71de3), juints(0x1a01a01a, 0x3efa01a0)};
    Xmm x0{x, 0}, x1, x2, x3, x4, x5, x6, x7;
    x1 = {PI32INV, 0};
    x1.lo = x1.lo * x0.lo;                                                // mulsd(xmm1, xmm0)
    x5 = {ONEHALF, ONEHALF};                                              // movdqu(xmm5, ONEHALF)
    x5.lo = bitsToDouble(doubleToBits(x5.lo) | (doubleToBits(x0.lo) & 0x8000000000000000ull)); // movq(xmm4, SIGN_MASK); pand; por
    x1.lo = x1.lo + x5.lo;                                                // addpd(xmm1, xmm5)
    x1.hi = x1.hi + x5.hi;
    i32 rdx = (i32)x1.lo;                                                 // cvttsd2sil (|x1| < 2^31 here)
    x1.lo = (double)rdx;                                                  // cvtsi2sdl
    int off;
    if (!cosine) {
        x6 = {P_2, P_2};                                                  // movdqu(xmm6, P_2)
        x3 = {P_1, 0};                                                    // mov64(r8, P_1); movdq(xmm3, r8)
        x5 = SC_4;                                                        // movdqu(xmm5, SC_4)
        x4 = {x0.lo, x0.lo};                                              // pshufd(xmm4, xmm0, 68)
        x3.lo = x3.lo * x1.lo;                                            // mulsd(xmm3, xmm1)
        x1 = {x1.lo, x1.lo};                                              // movddup(xmm1, xmm1)
        off = (int)(((u32)rdx & 63) << 5);                                // andl(rdx, 63); shll(rdx, 5)
        x6.lo = x6.lo * x1.lo;                                            // mulpd(xmm6, xmm1)
        x6.hi = x6.hi * x1.hi;
        x1.lo = x1.lo * P_3;                                              // mulsd(xmm1, P_3)
        x4.lo = x4.lo - x3.lo;                                            // subsd(xmm4, xmm3)
        x7 = {libmCtable(off + 8), 0};                                    // movq(xmm7, [rax + 8])
        x0.lo = x0.lo - x3.lo;                                            // subsd(xmm0, xmm3)
        x3 = {x4.lo, x4.lo};                                              // movddup(xmm3, xmm4)
        x4.lo = x4.lo - x6.lo;                                            // subsd(xmm4, xmm6)
        x0 = {x0.lo, x0.lo};                                              // pshufd(xmm0, xmm0, 68)
        x2 = {libmCtable(off), libmCtable(off + 8)};                      // movdqu(xmm2, [rax])
        x5.lo = x5.lo * x0.lo;                                            // mulpd(xmm5, xmm0)
        x5.hi = x5.hi * x0.hi;
        x0.lo = x0.lo - x6.lo;                                            // subpd(xmm0, xmm6)
        x0.hi = x0.hi - x6.hi;
        x7.lo = x7.lo * x4.lo;                                            // mulsd(xmm7, xmm4)
        x3.lo = x3.lo - x4.lo;                                            // subsd(xmm3, xmm4)
        x5.lo = x5.lo * x0.lo;                                            // mulpd(xmm5, xmm0)
        x5.hi = x5.hi * x0.hi;
        x0.lo = x0.lo * x0.lo;                                            // mulpd(xmm0, xmm0)
        x0.hi = x0.hi * x0.hi;
        x3.lo = x3.lo - x6.lo;                                            // subsd(xmm3, xmm6)
        x6 = SC_2;                                                        // movdqu(xmm6, SC_2)
        x1.lo = x1.lo - x3.lo;                                            // subsd(xmm1, xmm3)
    } else {
        x2 = {P_2, P_2};                                                  // movdqu(xmm2, P_2)
        x3 = {P_1, 0};                                                    // movq(xmm3, P_1)
        x3.lo = x3.lo * x1.lo;                                            // mulsd(xmm3, xmm1)
        x1 = {x1.lo, x1.lo};                                              // unpcklpd(xmm1, xmm1)
        off = (int)((((u64)(u32)rdx + 1865232u) & 63) << 5);              // addq(rdx, 1865232); andq(rdx, 63); shlq(rdx, 5)
        x4 = x0;                                                          // movdqu(xmm4, xmm0)
        x5 = SC_4;                                                        // movdqu(xmm5, SC_4)
        x2.lo = x2.lo * x1.lo;                                            // mulpd(xmm2, xmm1)
        x2.hi = x2.hi * x1.hi;
        x0.lo = x0.lo - x3.lo;                                            // subsd(xmm0, xmm3)
        x1.lo = x1.lo * P_3;                                              // mulsd(xmm1, P_3)
        x4.lo = x4.lo - x3.lo;                                            // subsd(xmm4, xmm3)
        x7 = {libmCtable(off + 8), 0};                                    // movq(xmm7, [rax + 8])
        x0 = {x0.lo, x0.lo};                                              // unpcklpd(xmm0, xmm0)
        x3 = x4;                                                          // movdqu(xmm3, xmm4)
        x4.lo = x4.lo - x2.lo;                                            // subsd(xmm4, xmm2)
        x5.lo = x5.lo * x0.lo;                                            // mulpd(xmm5, xmm0)
        x5.hi = x5.hi * x0.hi;
        x0.lo = x0.lo - x2.lo;                                            // subpd(xmm0, xmm2)
        x0.hi = x0.hi - x2.hi;
        x6 = SC_2;                                                        // movdqu(xmm6, SC_2)
        x7.lo = x7.lo * x4.lo;                                            // mulsd(xmm7, xmm4)
        x3.lo = x3.lo - x4.lo;                                            // subsd(xmm3, xmm4)
        x5.lo = x5.lo * x0.lo;                                            // mulpd(xmm5, xmm0)
        x5.hi = x5.hi * x0.hi;
        x0.lo = x0.lo * x0.lo;                                            // mulpd(xmm0, xmm0)
        x0.hi = x0.hi * x0.hi;
        x3.lo = x3.lo - x2.lo;                                            // subsd(xmm3, xmm2)
        x2 = {libmCtable(off), libmCtable(off + 8)};                      // movdqu(xmm2, [rax])
        x1.lo = x1.lo - x3.lo;                                            // subsd(xmm1, xmm3)
    }
    x3 = {libmCtable(off + 24), 0};                                       // movq(xmm3, [rax + 24])
    x2.lo = x2.lo + x3.lo;                                                // addsd(xmm2, xmm3)
    x7.lo = x7.lo - x2.lo;                                                // subsd(xmm7, xmm2)
    x2.lo = x2.lo * x4.lo;                                                // mulsd(xmm2, xmm4)
    x6.lo = x6.lo * x0.lo;                                                // mulpd(xmm6, xmm0)
    x6.hi = x6.hi * x0.hi;
    x3.lo = x3.lo * x4.lo;                                                // mulsd(xmm3, xmm4)
    x2.lo = x2.lo * x0.lo;                                                // mulpd(xmm2, xmm0)
    x2.hi = x2.hi * x0.hi;
    x0.lo = x0.lo * x0.lo;                                                // mulpd(xmm0, xmm0)
    x0.hi = x0.hi * x0.hi;
    x5.lo = x5.lo + SC_3.lo;                                              // addpd(xmm5, SC_3)
    x5.hi = x5.hi + SC_3.hi;
    x4.lo = x4.lo * libmCtable(off);                                      // mulsd(xmm4, [rax])
    x6.lo = x6.lo + SC_1.lo;                                              // addpd(xmm6, SC_1)
    x6.hi = x6.hi + SC_1.hi;
    x5.lo = x5.lo * x0.lo;                                                // mulpd(xmm5, xmm0)
    x5.hi = x5.hi * x0.hi;
    x0 = x3;                                                              // movdqu(xmm0, xmm3)
    x3.lo = x3.lo + libmCtable(off + 8);                                  // addsd(xmm3, [rax + 8])
    x1.lo = x1.lo * x7.lo;                                                // mulpd(xmm1, xmm7)
    x1.hi = x1.hi * x7.hi;
    x7 = x4;                                                              // movdqu(xmm7, xmm4)
    x4.lo = x4.lo + x3.lo;                                                // addsd(xmm4, xmm3)
    x6.lo = x6.lo + x5.lo;                                                // addpd(xmm6, xmm5)
    x6.hi = x6.hi + x5.hi;
    x5 = {libmCtable(off + 8), 0};                                        // movq(xmm5, [rax + 8])
    x5.lo = x5.lo - x3.lo;                                                // subsd(xmm5, xmm3)
    x3.lo = x3.lo - x4.lo;                                                // subsd(xmm3, xmm4)
    x1.lo = x1.lo + libmCtable(off + 16);                                 // addsd(xmm1, [rax + 16])
    x6.lo = x6.lo * x2.lo;                                                // mulpd(xmm6, xmm2)
    x6.hi = x6.hi * x2.hi;
    if (!cosine) {
        x5.lo = x5.lo + x0.lo;                                            // addsd(xmm5, xmm0)
        x3.lo = x3.lo + x7.lo;                                            // addsd(xmm3, xmm7)
        x1.lo = x1.lo + x5.lo;                                            // addsd(xmm1, xmm5)
        x1.lo = x1.lo + x3.lo;                                            // addsd(xmm1, xmm3)
        x1.lo = x1.lo + x6.lo;                                            // addsd(xmm1, xmm6)
        x6 = {x6.hi, x6.hi};                                              // unpckhpd(xmm6, xmm6)
        x0 = x4;                                                          // movdqu(xmm0, xmm4)
        x1.lo = x1.lo + x6.lo;                                            // addsd(xmm1, xmm6)
        x0.lo = x0.lo + x1.lo;                                            // addsd(xmm0, xmm1)
    } else {
        x0.lo = x0.lo + x5.lo;                                            // addsd(xmm0, xmm5)
        x3.lo = x3.lo + x7.lo;                                            // addsd(xmm3, xmm7)
        x0.lo = x0.lo + x1.lo;                                            // addsd(xmm0, xmm1)
        x0.lo = x0.lo + x3.lo;                                            // addsd(xmm0, xmm3)
        x0.lo = x0.lo + x6.lo;                                            // addsd(xmm0, xmm6)
        x6 = {x6.hi, x6.hi};                                              // unpckhpd(xmm6, xmm6)
        x0.lo = x0.lo + x6.lo;                                            // addsd(xmm0, xmm6)
        x0.lo = x0.lo + x4.lo;                                            // addsd(xmm0, xmm4)
    }
    return x0.lo;
}

// the stubs' range test: (high word & 0x7fff0000) - 0x30300000 above 0x10c50000 (unsigned: tiny, signed: large)
inline int libmRange(double x, u32& rax) {
    rax = ((u32)(doubleToBits(x) >> 32) & 2147418112u) - 808452096u;
    if (rax <= 281346048u) return 0;          // the main path
    return (i32)rax > 281346048 ? 1 : -1;     // |x| >= 90112, Inf, NaN : tiny (or 0)
}

[[noreturn]] inline void libmUnsupported(const char* f, double x) {
    fprintf(stderr, "java::%s: argument %.17g beyond the modelled range (|x| < 90112)\n", f, x);
    abort();
}
} // namespace detail

inline double sin(double x) {
    u32 rax;
    int r = detail::libmRange(x, rax);
    if (r == 0) return detail::libmSinCos(x, false);
    if (r > 0) {
        if (((u32)(detail::doubleToBits(x) >> 32) & 0x7ff00000u) == 0x7ff00000u) return x * -0.0; // Inf, NaN: NaN
        detail::libmUnsupported("sin", x);
    }
    if ((rax >> 20) == 3325u) return x * detail::juints(0xffffffff, 0x3fefffff); // +-0, subnormal: x * (1 - 2^-53)
    double t = detail::juints(0x00000000, 0x43600000);                            // 2^-55 * (2^55 * x - x)
    t = t * x;
    t = t - x;
    return t * detail::juints(0x00000000, 0x3c800000);
}

inline double cos(double x) {
    u32 rax;
    int r = detail::libmRange(x, rax);
    if (r == 0) return detail::libmSinCos(x, true);
    if (r > 0) {
        if (((u32)(detail::doubleToBits(x) >> 32) & 0x7ff00000u) == 0x7ff00000u) return x * -0.0; // Inf, NaN: NaN
        detail::libmUnsupported("cos", x);
    }
    return 1.0 - std::fabs(x); // tiny: 1 - |x|
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

// Kotlin's Char.isWhitespace() on the JVM: Character.isWhitespace(c) || Character.isSpaceChar(c)
// (the chars below with JDK 26: Unicode Zs, Zl, Zp, \t-\r and U+001C-U+001F)
inline bool isKotlinWhitespace(char16_t c) {
    switch (c) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20:
    case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

// Kotlin's String.trim() / CharSequence.trim() (kotlin-stdlib, JVM): removes the leading and
// trailing chars for which isKotlinWhitespace holds; unlike String.trim() it keeps the other
// control chars (U+0000-U+0008...) and removes U+00A0, U+3000...
inline String kotlinTrim(const String& s) {
    size_t a = 0, b = s.size();
    while (a < b && isKotlinWhitespace(s[a])) a++;
    while (b > a && isKotlinWhitespace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Kotlin's CharSequence.split(delimiter) with one non-empty String delimiter, no limit, case
// sensitive (kotlin-stdlib): every part, the empty ones included (unlike String.split)
template <class S>
inline std::vector<S> kotlinSplit(const S& s, const S& delim) {
    std::vector<S> r;
    size_t off = 0, next;
    while ((next = s.find(delim, off)) != S::npos) {
        r.push_back(s.substr(off, next - off));
        off = next + delim.size();
    }
    r.push_back(s.substr(off));
    return r;
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

// java.awt.Point#hashCode (Point2D's: Point does not override it): the long bits of the double x, xor those of the
// double y times 31, folded to an int
inline i32 pointHashCode(i32 x, i32 y) {
    u64 bits = detail::doubleToBits((double)x);
    bits ^= detail::doubleToBits((double)y) * 31u;
    return (i32)(u32)bits ^ (i32)(u32)(bits >> 32);
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

// s.equalsIgnoreCase(t) for an ASCII text t: the same length, and each char of s is t's char, or for a letter its
// other ASCII case or one of the four non-ASCII chars that regionMatches folds to it (Character.toUpperCase /
// toLowerCase per char: U+0130 and U+0131 to i, U+017F to s, U+212A to k). Every BMP char against every ASCII
// char checked with JDK 26, in Latin-1 and UTF-16 strings, both ways round.
inline bool equalsIgnoreCase(const String& s, const char* t) {
    size_t n = strlen(t);
    if (s.size() != n) return false;
    for (size_t i = 0; i < n; i++) {
        char16_t c = s[i];
        char16_t a = (char16_t)(unsigned char)t[i];
        if (c == a) continue;
        if (a >= u'A' && a <= u'Z') a = (char16_t)(a + 32);
        if (a < u'a' || a > u'z') return false; // not a letter: only itself
        if (c >= u'A' && c <= u'Z') c = (char16_t)(c + 32);
        if (c == a) continue;
        if ((c == 0x0130 || c == 0x0131) && a == u'i') continue;
        if (c == 0x017F && a == u's') continue;
        if (c == 0x212A && a == u'k') continue;
        return false;
    }
    return true;
}

// line terminators of java.util.Scanner#nextLine ("\r\n|[\n\r  \u0085]"); also the chars
// '.' does not match in a regex (no DOTALL)
inline bool isLineSeparator(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029 || c == 0x0085; }

// new Scanner(s).nextLine(): the text of s up to its first line terminator; false where Java throws
// NoSuchElementException ("No line found": s is empty)
inline bool scannerFirstLine(const String& s, String& line) {
    if (s.empty()) return false;
    size_t i = 0;
    while (i < s.size() && !isLineSeparator(s[i])) i++;
    line = s.substr(0, i);
    return true;
}

// \s in a java.util.regex Pattern (no UNICODE_CHARACTER_CLASS)
inline bool isRegexSpace(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == 0x0B || c == u'\f' || c == u'\r'; }

// String.split("\\s+") (a regex: Pattern.split with limit 0): the parts between the runs of \s; a run at the start
// gives an empty first part, the trailing empty parts are removed (a string of white space only gives no part), a
// string without white space is returned whole ("" gives [""])
inline std::vector<String> splitRegexSpaces(const String& s) {
    std::vector<String> list;
    size_t index = 0, i = 0, n = s.size();
    bool matched = false;
    while (i < n) {
        if (!isRegexSpace(s[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && isRegexSpace(s[j])) j++;
        list.push_back(s.substr(index, i - index));
        index = i = j;
        matched = true;
    }
    if (!matched) return {s};
    list.push_back(s.substr(index));
    size_t k = list.size();
    while (k > 0 && list[k - 1].empty()) k--;
    list.resize(k);
    return list;
}

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

// Boolean.parseBoolean(s) / Boolean.valueOf(s): "true".equalsIgnoreCase(s). No char but the ASCII ones folds to t, r, u
// or e (Character.toUpperCase / toLowerCase; every char at every place checked with JDK 26), so an ASCII comparison
inline bool parseBoolean(const String& s) {
    const char16_t* t = u"true";
    if (s.size() != 4) return false;
    for (size_t i = 0; i < 4; i++) {
        char16_t c = s[i];
        if (c >= u'A' && c <= u'Z') c = (char16_t)(c + 32);
        if (c != t[i]) return false;
    }
    return true;
}

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
// compareTo or identity hash codes. Integer keys and in-grid coordinates never do. When they can
// (Platinum Rift's HashSet<Edge>, Edge hashing to 31 * (31 + a) + b), use HashMapOrderT with a
// tie-break policy: TieBreak::enabled, and tieBreak(k, pk) gives the direction Java's TreeNode code
// takes for the key k against the tree's key pk (< 0 left, > 0 right: compareComparables for
// Comparable keys, else tieBreakOrder, i.e. System.identityHashCode(k) <= System.identityHashCode(pk)
// ? -1 : 1, which a port can only model on the jar's usual outcome); keys are then found by hash and
// key like TreeNode.find. java::HashMapOrder (NoTieBreak) is the code without it.
struct NoTieBreak {
    static constexpr bool enabled = false;
    int operator()(i32, i32) const { return 1; }
};
template <class TieBreak = NoTieBreak>
struct HashMapOrderT {
    [[no_unique_address]] TieBreak tieBreak;
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
    // TreeNode.find(h, k, null) for keys that are not Comparable (with a tie-break policy): equal hash codes searched
    // on both sides, the right subtree first
    int findKey(int p, i32 h, i32 k) {
        while (p >= 0) {
            int pl = P(p).left, pr = P(p).right, q;
            i32 ph = P(p).hash;
            if (ph > h) p = pl;
            else if (ph < h) p = pr;
            else if (P(p).key == k) return p;
            else if (pl < 0) p = pr;
            else if (pr < 0) p = pl;
            else if ((q = findKey(pr, h, k)) >= 0) return q;
            else p = pl;
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
                    int dir = ph > h ? -1 : 1; // hashes are distinct (without a tie-break policy)
                    if constexpr (TieBreak::enabled)
                        if (ph == h) dir = tieBreak(P(x).key, P(p).key);
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
        [[maybe_unused]] bool searched = false;
        for (int p = rt;;) {
            int dir;
            i32 ph = P(p).hash;
            if (ph > h) dir = -1;
            else if (ph < h) dir = 1;
            else if constexpr (!TieBreak::enabled) return p;
            else {
                if (P(p).key == k) return p;
                if (!searched) { // the key may be further down, on either side
                    searched = true;
                    int q = -1;
                    if ((P(p).left >= 0 && (q = findKey(P(p).left, h, k)) >= 0) || (P(p).right >= 0 && (q = findKey(P(p).right, h, k)) >= 0)) return q;
                }
                dir = tieBreak(k, P(p).key);
            }
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
                if constexpr (TieBreak::enabled) old = findKey(P(first).parent >= 0 ? root(first) : first, hash, key);
                else old = find(P(first).parent >= 0 ? root(first) : first, hash);
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
            if (P(p).tree) {
                if constexpr (TieBreak::enabled) node = findKey(P(p).parent >= 0 ? root(p) : p, hash, key);
                else node = find(P(p).parent >= 0 ? root(p) : p, hash);
            } else {
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
    // HashMap.clear / HashSet.clear: empties the bins, the table keeps its size (and threshold)
    void clear() {
        for (int& b : tab) b = -1;
        pool.clear();
        size = 0;
    }
};
using HashMapOrder = HashMapOrderT<>;

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

    // Random.nextInt(int bound) with its argument check: an IllegalArgumentException for a bound below 1
    // (nextInt(bound) above assumes a positive bound)
    i32 nextIntChecked(i32 bound) {
        if (bound <= 0) throw Exception{"java.lang.IllegalArgumentException: bound must be positive"};
        return nextInt(bound);
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

    // RandomGenerator.nextFloat(float bound) (RandomSupport.boundedNextFloat); `bound` must be finite
    // and positive (Java throws IllegalArgumentException otherwise)
    float nextFloat(float bound) {
        float r = nextFloat();
        r = r * bound;
        if (r >= bound) r = std::nextafter(bound, -INFINITY);
        return r;
    }

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

// HotSpot's identity hash codes (Object#hashCode / System.identityHashCode of an object whose class does not
// override hashCode; -XX:hashCode=5, the default since JDK 8): every thread has its own Marsaglia xor-shift
// generator, seeded when the thread is created with X = os::random(), Y = 842502087, Z = 0x8767, W = 273326509;
// an object gets the next value of the thread that first asks for its hash code (the new W, 31 bits, 0 becoming
// 0xBAD), and keeps it. os::random() is the JVM's global Park-Miller generator (16807, 2^31 - 1, from 1234567),
// called once per thread created and a few other times: a thread's seed is os::random()'s n-th value, n depending on
// how many threads the JVM created before (JIT compiler and GC threads come and go with the load, so the same
// program's thread can get another n from one launch to the next). Checked against JDK 26 (x86_64) with diagnostic
// builds of cg-2014-poker-chip-race's jar (every identity hash code of a game explained by one thread seed).
struct IdentityHashes {
    u32 x = 0, y = 842502087u, z = 0x8767u, w = 273326509u;
    i64 count = 0; // the values drawn so far
    IdentityHashes() {}
    explicit IdentityHashes(u32 threadSeed) : x(threadSeed) {}
    // os::random()'s n-th value (n >= 1)
    static u32 osRandom(int n) {
        u64 s = 1234567;
        for (int i = 0; i < n; i++) s = s * 16807 % 2147483647;
        return (u32)s;
    }
    // the identity hash code of the next object hashed by the thread
    i32 next() {
        u32 t = x;
        t ^= t << 11;
        x = y;
        y = z;
        z = w;
        u32 v = w;
        v = (v ^ (v >> 19)) ^ (t ^ (t >> 8));
        w = v;
        count++;
        u32 value = v & 0x7FFFFFFFu;
        return (i32)(value == 0 ? 0xBAD : value);
    }
    void skip(i64 n) {
        for (i64 i = 0; i < n; i++) next();
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

// ----------------------------------------------------------------------------
// String.format, java.util.Properties
// ----------------------------------------------------------------------------

// An argument of String.format: an Integer / Long, a String, or null
struct FormatArg {
    enum Kind { NUL, INT, STR } kind = NUL;
    i64 i = 0;
    String s;
    FormatArg() {}
    FormatArg(int v) : kind(INT), i(v) {}
    FormatArg(i64 v) : kind(INT), i(v) {}
    FormatArg(const String& v) : kind(STR), s(v) {}
    FormatArg(const char16_t* v) : kind(STR), s(v) {}
    // String.valueOf(arg)
    String toString() const { return kind == INT ? str(i) : (kind == STR ? s : String(u"null")); }
};

// The platform's line separator (System.lineSeparator(), what println and %n write)
#ifdef _WIN32
inline const char16_t* LINE_SEPARATOR = u"\r\n";
#else
inline const char16_t* LINE_SEPARATOR = u"\n";
#endif

// String.format(format, args) (an English default locale) for the conversions %s, %d, %% and %n
// with an optional argument index, '-' flag, width and (%s) precision: the formats of the messages
// of CodinGame's old referees. The JDK's exceptions for a missing argument, a %d of a String, an
// unknown conversion; other flags and conversions are not supported (the port aborts).
inline String format(const String& fmt, const std::vector<FormatArg>& args) {
    String out;
    size_t ordinary = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != u'%') {
            out += fmt[i];
            continue;
        }
        size_t start = i++;
        // %[argument_index$][flags][width][.precision]conversion
        auto digits = [&](size_t& k) {
            size_t a = k;
            while (k < fmt.size() && fmt[k] >= u'0' && fmt[k] <= u'9') k++;
            return a < k;
        };
        auto number = [&](size_t a, size_t b) {
            i64 v = 0;
            for (size_t k = a; k < b; k++) v = std::min<i64>(v * 10 + (fmt[k] - u'0'), INT32_MAX);
            return v;
        };
        i64 index = -1, width = -1, precision = -1;
        size_t k = i;
        if (digits(k) && k < fmt.size() && fmt[k] == u'$') {
            index = number(i, k);
            i = k + 1;
        }
        bool left = false;
        String flags;
        while (i < fmt.size() && String(u"-#+ 0,(<").find(fmt[i]) != String::npos) {
            if (fmt[i] == u'-') left = true;
            flags += fmt[i++];
        }
        k = i;
        if (digits(k)) {
            width = number(i, k);
            i = k;
        }
        if (i < fmt.size() && fmt[i] == u'.') {
            k = i + 1;
            if (digits(k)) {
                precision = number(i + 1, k);
                i = k;
            }
        }
        if (i >= fmt.size()) throw Exception{"java.util.UnknownFormatConversionException: Conversion = '%'"};
        char16_t conv = fmt[i];
        String spec = fmt.substr(start, i + 1 - start);
        if (conv != u's' && conv != u'd' && conv != u'%' && conv != u'n') {
            bool known = String(u"bBhHsScCdoxXeEfgGaAtT%n").find(conv) != String::npos;
            if (!known) throw Exception{"java.util.UnknownFormatConversionException: Conversion = '" + encodeUtf8(String(1, conv)) + "'"};
            fprintf(stderr, "java::format: unsupported conversion %s\n", encodeUtf8(spec).c_str());
            abort();
        }
        if (flags.size() > (left ? 1u : 0u) || (left && width < 0)) {
            fprintf(stderr, "java::format: unsupported flags %s\n", encodeUtf8(spec).c_str());
            abort();
        }
        String text;
        if (conv == u'n') {
            out += LINE_SEPARATOR;
            continue;
        } else if (conv == u'%') {
            text = u"%";
        } else {
            size_t a = index >= 1 ? (size_t)(index - 1) : ordinary++;
            if (a >= args.size()) throw Exception{"java.util.MissingFormatArgumentException: Format specifier '" + encodeUtf8(spec) + "'"};
            const FormatArg& arg = args[a];
            if (conv == u'd') {
                if (arg.kind == FormatArg::STR) throw Exception{"java.util.IllegalFormatConversionException: d != java.lang.String"};
                if (precision >= 0) throw Exception{"java.util.IllegalFormatPrecisionException: " + std::to_string(precision)};
            }
            text = arg.toString();
            if (conv == u's' && precision >= 0 && (i64)text.size() > precision) text = text.substr(0, (size_t)precision);
        }
        while (width > (i64)text.size()) text = left ? text + u" " : u" " + text;
        out += text;
    }
    return out;
}

// java.util.Properties: getProperty / setProperty / load, and the iteration order of entrySet():
// since JDK 9 a Properties is a ConcurrentHashMap of initial capacity 8, i.e. a table of 16 bins, in
// which each bin keeps its keys in insertion order (putVal appends a new key to its bin); the 12th key
// doubles the table (and the 24th, the 48th...: count >= 0.75 n), which moves each bin's keys to bins i
// and i + n (ConcurrentHashMap#transfer: the keys after the last change of the new bit stay in order, the
// ones before it are put in front of them one by one, i.e. reversed). A bin of 8 keys that gets a 9th
// (treeifyBin: presized or turned into a tree) is not modelled: the port aborts.
struct Properties {
    std::vector<std::pair<String, String>> entries; // insertion order

    const String* find(const String& key) const {
        for (auto& e : entries)
            if (e.first == key) return &e.second;
        return nullptr;
    }
    // setProperty / put: a new key goes last, an existing one keeps its place
    void setProperty(const String& key, const String& value) {
        for (auto& e : entries)
            if (e.first == key) {
                e.second = value;
                return;
            }
        entries.push_back({key, value});
    }
    String getProperty(const String& key, const String& defaultValue) const {
        const String* v = find(key);
        return v ? *v : defaultValue;
    }
    size_t size() const { return entries.size(); }
    // entrySet() in iteration order: the keys put again in their insertion order into the table
    // (spread(hashCode) & (n - 1), n = 16, doubled at count >= 0.75 n), then the bins in order
    std::vector<std::pair<String, String>> entrySet() const {
        size_t n = 16;
        std::vector<std::vector<size_t>> bins(n);
        for (size_t k = 0; k < entries.size(); k++) {
            i32 h = hashCode(entries[k].first);
            u32 spread = (u32)(h ^ (i32)((u32)h >> 16)) & 0x7fffffffu;
            std::vector<size_t>& bin = bins[spread & (n - 1)];
            if (bin.size() >= 8) {
                fprintf(stderr, "java::Properties: a bin of more than 8 keys (treeifyBin) is not supported\n");
                abort();
            }
            bin.push_back(k);
            if (k + 1 >= n - (n >> 2)) { // addCount: count >= sizeCtl, transfer to a table of 2n
                std::vector<std::vector<size_t>> next(n * 2);
                for (size_t i = 0; i < n; i++) {
                    const std::vector<size_t>& f = bins[i];
                    if (f.empty()) continue;
                    auto bit = [&](size_t key) {
                        i32 kh = hashCode(entries[key].first);
                        return ((u32)(kh ^ (i32)((u32)kh >> 16)) & (u32)n) != 0;
                    };
                    size_t lastRun = 0; // the last run of keys with the same bit
                    for (size_t p = 1; p < f.size(); p++)
                        if (bit(f[p]) != bit(f[lastRun])) lastRun = p;
                    std::vector<size_t>& run = next[bit(f[lastRun]) ? i + n : i];
                    run.assign(f.begin() + (std::ptrdiff_t)lastRun, f.end());
                    for (size_t p = 0; p < lastRun; p++) {
                        std::vector<size_t>& to = next[bit(f[p]) ? i + n : i];
                        to.insert(to.begin(), f[p]);
                    }
                }
                bins.swap(next);
                n *= 2;
            }
        }
        std::vector<std::pair<String, String>> r;
        for (const std::vector<size_t>& bin : bins)
            for (size_t k : bin) r.push_back(entries[k]);
        return r;
    }
    // load(new StringReader(text)): natural lines ended by \n, \r or \r\n, leading white space
    // (' ', '\t', '\f') skipped, blank and comment (# !) lines skipped, a line ending with an odd
    // number of backslashes continued on the next one, key = up to the first unescaped '=', ':' or
    // white space, then white space and one '=' or ':' skipped, escapes \t \n \r \f \uXXXX \x
    void load(const String& text) {
        auto isWs = [](char16_t c) { return c == u' ' || c == u'\t' || c == u'\f'; };
        size_t off = 0, n = text.size();
        bool skipLF = false;
        // LineReader.readLine: the next logical line, false at the end
        auto readLine = [&](String& line) {
            line.clear();
            bool skipWhiteSpace = true, appendedLineBegin = false, precedingBackslash = false;
            for (;;) {
                if (off >= n) {
                    if (line.empty()) return false;
                    if (precedingBackslash) line.pop_back();
                    return true;
                }
                char16_t c = text[off++];
                if (skipLF) {
                    skipLF = false;
                    if (c == u'\n') continue;
                }
                if (skipWhiteSpace) {
                    if (isWs(c)) continue;
                    if (!appendedLineBegin && (c == u'\r' || c == u'\n')) continue;
                    skipWhiteSpace = false;
                    appendedLineBegin = false;
                }
                if (line.empty() && (c == u'#' || c == u'!')) { // a comment: up to the end of its line
                    for (;;) {
                        if (off >= n) return false;
                        char16_t b = text[off++];
                        if (b == u'\r' || b == u'\n') break;
                    }
                    skipWhiteSpace = true;
                    continue;
                }
                if (c != u'\n' && c != u'\r') {
                    line += c;
                    precedingBackslash = (c == u'\\') ? !precedingBackslash : false;
                } else {
                    if (line.empty()) {
                        skipWhiteSpace = true;
                        continue;
                    }
                    if (off >= n) { // the end of the input
                        if (precedingBackslash) line.pop_back();
                        return true;
                    }
                    if (precedingBackslash) { // continued on the next line, without its leading white space
                        line.pop_back();
                        skipWhiteSpace = true;
                        appendedLineBegin = true;
                        precedingBackslash = false;
                        if (c == u'\r') skipLF = true;
                    } else {
                        return true;
                    }
                }
            }
        };
        String line;
        while (readLine(line)) {
            // load0
            size_t limit = line.size(), keyLen = 0, valueStart = limit;
            bool hasSep = false, precedingBackslash = false;
            while (keyLen < limit) {
                char16_t c = line[keyLen];
                if ((c == u'=' || c == u':') && !precedingBackslash) {
                    valueStart = keyLen + 1;
                    hasSep = true;
                    break;
                } else if (isWs(c) && !precedingBackslash) {
                    valueStart = keyLen + 1;
                    break;
                }
                precedingBackslash = c == u'\\' ? !precedingBackslash : false;
                keyLen++;
            }
            while (valueStart < limit) {
                char16_t c = line[valueStart];
                if (!isWs(c)) {
                    if (!hasSep && (c == u'=' || c == u':')) hasSep = true;
                    else break;
                }
                valueStart++;
            }
            setProperty(loadConvert(line.substr(0, keyLen)), loadConvert(line.substr(valueStart)));
        }
    }
    // Properties.loadConvert: the escapes
    static String loadConvert(const String& in) {
        String out;
        for (size_t i = 0; i < in.size();) {
            char16_t c = in[i++];
            if (c != u'\\') {
                out += c;
                continue;
            }
            if (i >= in.size()) break; // (a final lone backslash is dropped by readLine)
            c = in[i++];
            if (c == u'u') {
                u32 v = 0;
                for (int k = 0; k < 4; k++) {
                    char16_t d = i < in.size() ? in[i++] : 0;
                    int x = d >= u'0' && d <= u'9' ? d - u'0' : (d >= u'a' && d <= u'f' ? d - u'a' + 10 : (d >= u'A' && d <= u'F' ? d - u'A' + 10 : -1));
                    if (x < 0) throw Exception{"java.lang.IllegalArgumentException: Malformed \\uxxxx encoding."};
                    v = (v << 4) + x;
                }
                out += (char16_t)v;
            } else {
                out += c == u't' ? u'\t' : c == u'r' ? u'\r' : c == u'n' ? u'\n' : c == u'f' ? u'\f' : c;
            }
        }
        return out;
    }
};

} // namespace java
