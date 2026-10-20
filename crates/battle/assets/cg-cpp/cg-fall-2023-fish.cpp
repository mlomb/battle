// Seabed Security (CodinGame Fall Challenge 2023) - headless C++ referee, port of
// https://github.com/CodinGame/FallChallenge2023-SeabedSecurity (commit d40cf97),
// CodinGame game engine 4.4.2, highest league (league.level 19: monsters, fleeing fish,
// 2 drones per player). Deterministic; reproduces the same game as the reference jar for a
// given -seed. 2 players only.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints the scores and "seed=<N>" to stdout like the jar. The engine and the command line are
// common/engine.h; -l writes its trace (see there) plus the viewer frame data of every turn
// ("FV <json>", same text as the jar's game log).
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
//     the jar's head start before the first input is approximated, see FIRST_TURN_HEAD_START),
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

#include "common/engine.h"

#include <deque>

typedef std::u16string ustr; // a java.lang.String (UTF-16 code units)

// ----------------------------------------------------------------------------
// Java semantics (java.h has the rest)
// ----------------------------------------------------------------------------
namespace java {

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


} // namespace java

using java::String;

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

    // Referee.init
    const int FIRST_TURN_MAX_TIME = 1000;
    const int TURN_MAX_TIME = 50;
    // the jar's head start before the first input (cg::AgentOptions::firstTurnHeadStartMs)
    const int FIRST_TURN_HEAD_START = 500;
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

struct Player : cg::Player {
    std::vector<Drone*> drones;
    std::vector<Scan> scans;          // LinkedHashSet<Scan>
    std::vector<Fish*> visibleFishes; // LinkedHashSet<Fish> (identity)
    int points = 0;

    int getExpectedOutputLines() const override { return (int)drones.size(); }
};

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
static const int ABSENT = INT_MIN; // key not in the HashMap

struct Game {
    java::SHA1PRNG& random = cg::gm.getRandom(); // seeded by the engine
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
using cg::trace;

static void addToGameSummary(const ustr& s) { cg::gm.addToGameSummary(s); }
static void addTooltip(int player, const ustr& message) { cg::gm.addTooltip(player, message); }

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

static void gameInit() {
    G.entityCount = 0;
    for (auto& p : players) G.players.push_back(&p);
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
        cg::gm.endGame();
    }
    G.gameTurn++;

    // gameSummaryManager.toString(): not reproduced (summary text only; it is the last line of a
    // turn and only added below 800 chars, so it can't make the SUMMARY command overflow)
    addToGameSummary(u"");
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
static void addLine(Player* player, const std::string& line) { player->sendInputLine(line); }

static void serializeGlobalInfoFor(Player* player) {
    Player* in = player;
    addLine(in, std::to_string(G.fishes.size() + G.uglies.size()));
    for (Fish* fish : G.fishes)
        addLine(in, std::to_string(fish->id) + " " + std::to_string(fish->color) + " " + std::to_string(fish->type));
    for (Ugly* ugly : G.uglies) addLine(in, std::to_string(ugly->id) + " -1 -1");
}

static void serializeFrameInfoFor(Player* player) {
    Player* in = player;
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
    if (trace && !pendingFrame.empty()) fprintf(trace, "FV %s\n", pendingFrame.c_str());
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
        player->deactivate(java::replace(java::replace(e.message, u"&lt;", u"<"), u"&gt;", u">"));
        player->score = -1;
        addToGameSummary(e.message);
        addToGameSummary(u"\u00A4RED\u00A4$" + java::ascii(std::to_string(player->index)) + u": disqualified!\u00A7RED\u00A7");
    }
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
            player->deactivate(u"Timeout!");
            addToGameSummary(java::ascii("$" + std::to_string(player->index) + " has not provided " +
                                         std::to_string(player->getExpectedOutputLines()) + " lines in time"));
        } else {
            parseCommands(player, player->outputs);
        }
    }
}

struct FishReferee : cg::Referee {
    void init() override {
        gameInit();
        for (Player* player : getActivePlayers()) serializeGlobalInfoFor(player);
        cg::gm.setMaxTurns(C::MAX_TURNS);
        cg::gm.setTurnMaxTime(C::TURN_MAX_TIME);
        cg::gm.setFirstTurnMaxTime(C::FIRST_TURN_MAX_TIME);
        traceFrame();
    }
    void gameTurn(int turn) override {
        resetGameTurnData();
        newTurn = true;
        for (Player* player : getActivePlayers()) {
            serializeFrameInfoFor(player);
            if (newTurn) flushFrame(); // the jar dumps the previous frame at the first execution
            newTurn = false;
            player->execute();
        }
        handlePlayerCommands();
        performGameUpdate(turn);
        if (getActivePlayers().size() < 2) cg::gm.endGame();
        traceFrame();
    }
    void onEnd() override {
        ::onEnd();
        flushFrame();
    }
};

int main(int argc, char** argv) {
    static FishReferee referee;
    cg::Options options;
    options.maxPlayers = 2;
    options.agent.stderrMode = cg::Stderr::Jar;
    options.agent.firstTurnHeadStartMs = C::FIRST_TURN_HEAD_START;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
