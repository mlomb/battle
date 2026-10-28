// Coders Strike Back / Mad Pod Racing (CodinGame, 2015-11, "coders-strike-back") - headless C++ referee, port of the
// CodinGame referee in https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the only
// commit of the file), "Referees/Coders Strike Back/Referee.java": the single-file referee `class Referee extends
// MultiReferee` with CodinGame's old AbstractReferee base of 2015 embedded (common/legacy.h,
// legacy::Base::codersStrikeBack2015(): Hypersonic's base but for getMillisTimeForRound 150, no "LostTooltip" message,
// no precomputed inputs, the frame data and the tooltips sent in every frame, no gameOver(); its SoloReferee class is
// unused). Two presets:
//   - cg-2016-coders-strike-back (this file): the game CodinGame runs today, in its top league (Legend). The file in the
//     sources is an older version of it: CodinGame's replays (codingame.com's public gameResult/findByGameId: 364 Legend
//     games, 300 Gold, 180 of the lower leagues) show seven differences, applied here (CODERS_STRIKE_BACK_ORIGINAL 0) and
//     in the jar by referees/cg-builder/patches/cg-2016-coders-strike-back.patch: BOOST (the first BOOST of a pod is a
//     thrust of 650, "boosts towards (x, y)" in the infos, used up even during a shield cooldown where it gives 0;
//     later ones are the maximum thrust, 200), "its" for "it's" in two action texts, 3 laps whatever the checkpoint
//     count (the file: (int) (15.0 / count)), the checkpoints' jitter of 30
//     instead of 100 (CHECKPOINT_GENERATION_MAX_GAP), the time of a checkpoint is the round plus the fraction of the
//     movement where the pod enters its circle (the file: the round; so a player is out one round later, except
//     without any checkpoint), a pod's next checkpoint stays 0 once it finished, and two pods on their last leg or
//     finished with as many checkpoints rank by the time of their last checkpoint (earlier first; the file: the
//     distance to the next checkpoint, kept for the other legs). The jar built with the patch and this port reproduce
//     every Legend replay from its seed alone. How the live referee codes these rules is inferred (the replays show what
//     they do, not their code): see the patch's header; the edges no replay shows (the fraction's formula beyond its
//     integer part, two pods finishing in one round with as many checkpoints) follow the patch.
//   - cg-2016-coders-strike-back-original (cg-2016-coders-strike-back-original.cpp: this file with CODERS_STRIKE_BACK_ORIGINAL
//     1): the file as published, no BOOST (an invalid input), (int) (15.0 / count) laps, jitter 100, integer
//     checkpoint times.
// 2 to 4 players (getMinimumPlayerCount() 2; every rule and input is written for any number of players: a player's
// input has its own pods first, then the others' in player order; CodinGame runs it with 2 players).
// Deterministic: reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 ... -p4 ...] -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): seed, map, pod_timeout,
// pod_per_player) to stdout like the jar (referees/cg-builder/legacy: the referee runs unchanged in the runner of the
// SDK 4.5.0). "null" scores when the game fails (the Java referee crashed, looped forever or the protocol broke; the
// jar after 150 s). -l writes the trace of common/legacy.h (bot executions, view sizes, summaries, tooltips, END;
// CG_TRACE_VIEWS=1 adds the text of every view). Time limits: the base's 1000 ms for the first turn, 150 ms after
// (the game overrides neither; CodinGame's statement says 75 ms after the first turn, which no replay can show).
//
// League: none in the file (its constants are the rules of its time); the CLI only passes the seed. CodinGame's leagues,
// from its replays: Legend = this preset; Gold = the same with a maximum thrust of 100 (also for a later BOOST; 300/300
// Gold replays with that one constant changed); Silver, Bronze and Wood 1-3 = one pod per player, other maps (given in
// their game parameters), thrust 100, checkpoint times in whole rounds, Wood without pod collisions (36/36 replays of
// each league with those constants). Only the top league is implemented here.
//
// The game: a 16000 x 9000 race track of 3 to 6 checkpoints (radius 600), one of 13 hard-coded maps chosen by
// java.util.Random(seed), rotated, each checkpoint moved by a random jitter. Each player has 2 pods (radius 400),
// side by side on the first checkpoint, facing the second; each round every player answers one line per pod:
// "x y power [message]" (0-200), "x y SHIELD" (mass 10 for that round, no thrust for the 3 next rounds) or, live,
// "x y BOOST". A pod turns towards its target by at most 18 degrees (any angle in its first round), accelerates by its
// thrust, moves with exact collision times (elastic pod collisions with a minimum impulse of 120, overlaps fixed),
// then friction 0.15, its position rounded and its speed truncated. A pod passing within 600 of its next checkpoint
// (along its whole path, collisions included) validates it. The race ends when a pod has done all its laps; a player
// is out after pod_timeout (100) rounds without a checkpoint for any of its pods, on an invalid line or a timeout; or
// after 500 rounds. Ranks: by checkpoints passed, then by the distance to the next checkpoint (live: the time on the
// last leg); the score is players - rank + 1, 0 when out.
//
// The Java referee is followed closely, notably: the double precision physics in the same operation order
// (FP_CONTRACT off) with Math.atan2 (java::atan2, fdlibm), Math.cos / Math.sin (java::cos / sin, HotSpot's intrinsics),
// Math.sqrt, Math.round, Math.signum and Java's double %, the angles of the pods unbounded (only the turn is limited),
// the collision lists (created in the referee's order, every candidate collision numbered by a static counter that
// the view shows, a stable sort on Double.compare of the times, every collision within 1e-5 of the first one
// reacting in that order), the pods of a player who is out still moving and colliding, the waypoints of a pod (the
// positions of its collisions, the last one of a time replaced) and java.awt.geom.Line2D#ptSegDist on every segment of
// the path for the checkpoints (java::ptSegDistSq; several checkpoints in one round, each search from the first
// segment), the int cast of Math.round (positions), the ranks (TimSort with the referee's compareTo, equal pods sharing
// a rank, the players' ranks then renumbered by the referee's loop), the regex of a line (Matcher#matches with Java's
// backtracking, see matchLine: "x y  word" is a thrust of 200 with the message "word"), the lines before an invalid
// one still applying that round, messages cut to 50 UTF-16 units + "...", the view's frame data built in full
// (Double.toString; views stay far below the runner's 30000-byte limit).
// Not reproduced / not modelled: the wall clock (see engine.h); the order of the tooltips of a frame (the base's
// HashSet of identity hash codes: legacy.h, only their count reaches the runner).
// Reproduced but never seen in a game: the Java referee's endless collision loop when pods are at the same point (no
// normal, no impulse, no separation: every step takes no time; reached by the reflection check's random states; the
// jar's runner gives up after 150 s, "null" scores): this port detects a step of no time that changed nothing and fails
// the same way at once (any other round is cut after 10^5 steps).
//
// Validation (referees/README.md): referee-diff against the jars with deterministic bots (every ordered pair, 3 and 4
// players, seed bases covering the 13 maps), direct runs comparing the jars' game logs with this port's trace (views,
// summaries, tooltips; raw bytes and line endings), the jars' own compiled classes driven by reflection over random
// physics states, CodinGame's replays (the patched jar and this port).
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/legacy.h"

#include <cmath>
#include <optional>
#include <vector>

#ifndef CODERS_STRIKE_BACK_ORIGINAL
#define CODERS_STRIKE_BACK_ORIGINAL 0
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// the rules of CodinGame's live game (patches/cg-2016-coders-strike-back.patch), or the file's
const bool LIVE = !CODERS_STRIKE_BACK_ORIGINAL;

// --- constants ---
const double EPSILON = 0.00001;
const double MIN_IMPULSE = 120;
const int MAP_WIDTH = 16000, MAP_HEIGHT = 9000; // SIZE, a java.awt.Dimension
const int POD_RADIUS = 400;
const int CHECKPOINT_RADIUS = 600;
const int SPACE_BETWEEN_POD = 100;
const int DEFAULT_POD_PER_PLAYER = 2;
const int MAX_POWER = 200;
const int BOOST_POWER = 650; // live
const double FRICTION = 0.15;
const int DEFAULT_POD_TIMEOUT = 100;
const int MIN_CHECKPOINT_COUNT = 15;
const int LAPS = 3; // live
const int CHECKPOINT_GENERATION_MAX_GAP = LIVE ? 30 : 100;
const double PI = 3.141592653589793;                       // Math.PI
const double MAX_ROTATION_PER_TURN = 0.3141592653589793;   // Math.PI / 10 (folded by javac)
const double INACTIVE_ROUND_AFTER_SHIELD = 3;
const double SHIELD_MASS = 10;
const long long MAX_STEPS = 100000; // the steps of a round's collision loop, cut (never reached in a game)

String S(i64 v) { return java::str(v); }
String D(double v) { return java::ascii(java::doubleToString(v)); } // String.valueOf(double)

// --- java.awt.Point ---
struct Point {
    i32 x = 0, y = 0;
};

// --- Referee.Vector (immutable: a value here) ---
struct Vector {
    double x = 0, y = 0;
    Vector() {}
    Vector(double x, double y) : x(x), y(y) {}
    // new Vector(a, b): b - a
    static Vector between(const Vector& a, const Vector& b) { return Vector(b.x - a.x, b.y - a.y); }
    static Vector between(const Point& a, const Point& b) { return Vector((double)(b.x - a.x), (double)(b.y - a.y)); }
    // (int) Math.round(x): the long's low 32 bits
    Vector round() const { return Vector((double)java::l2i(java::round(x)), (double)java::l2i(java::round(y))); }
    Vector truncate() const { return Vector((double)java::d2i(x), (double)java::d2i(y)); }
    double distance(const Vector& v) const { return std::sqrt((v.x - x) * (v.x - x) + (v.y - y) * (v.y - y)); }
    Vector add(const Vector& v) const { return Vector(x + v.x, y + v.y); }
    Vector mult(double factor) const { return Vector(x * factor, y * factor); }
    Vector sub(const Vector& v) const { return Vector(x - v.x, y - v.y); }
    double length() const { return std::sqrt(x * x + y * y); }
    double lengthSquared() const { return x * x + y * y; }
    Vector normalize() const {
        double l = length();
        if (l == 0) return Vector(0, 0);
        return Vector(x / l, y / l);
    }
    double dot(const Vector& v) const { return x * v.x + y * v.y; }
    double angle() const { return java::atan2(y, x); }
    Vector cross(double s) const { return Vector(-s * y, s * x); }
    Vector translate(const Point& p) const { return Vector(p.x + x, p.y + y); }
};

struct CheckPoint {
    Point position;
};

struct PodWayPoint {
    double time;
    Vector position;
};

struct Player;

struct Pod {
    int id = 0;
    std::vector<double> checkPointTimes;
    std::optional<Point> targetPosition;
    Vector lastPosition, position, speed, acceleration;
    Player* player = nullptr;
    std::vector<PodWayPoint> wayPoints;
    NString message;
    int rank = 1, power = 0, wantedPower = 0;
    std::optional<double> lastAngle; // a Double
    bool shieldMode = false;
    std::optional<int> lastShieldModeRound; // an Integer
    bool boostUsed = false;                  // live

    Pod(int id, Vector p) : id(id), lastPosition(p), position(p), speed(0, 0) { checkPointTimes.push_back(0.0); }

    double getLastCheckPointTime() const { return checkPointTimes.back(); }
    double getMass() const { return shieldMode ? SHIELD_MASS : 1; }
    bool collide(const Pod& c2) const { return position.distance(c2.position) <= POD_RADIUS * 2; }

    void addWayPoint(double time) {
        if (!wayPoints.empty() && wayPoints.back().time == time) wayPoints.pop_back();
        wayPoints.push_back(PodWayPoint{time, position});
    }
    void applyForce(const Vector& force) { speed = speed.add(force.mult(1.0 / getMass())); }
    void applyForce(double time, const Vector& force) {
        applyForce(force);
        addWayPoint(time); // (force.length(), its other argument, is unused)
    }
    void fixPosition(const Vector& p) {
        position = p;
        if (!wayPoints.empty()) wayPoints.back().position = p;
    }
    double contactPosition(const Pod& pod) const {
        Vector dv = speed.sub(pod.speed);
        Vector d = Vector::between(pod.position, position);
        double a = dv.lengthSquared();
        double b = d.x * dv.x + d.y * dv.y;
        double dmin = POD_RADIUS * 2;
        double c = d.lengthSquared() - dmin * dmin;
        double delta = b * b - a * c;
        if (delta <= 0 || a == 0) return INFINITY;
        double rd = std::sqrt(delta);
        return java::min((-b + rd) / a, (-b - rd) / a);
    }
    void step(double step) {
        if (step > 0) position = position.add(speed.mult(step));
    }
    void endRound() {
        position = position.round();
        speed = speed.truncate();
    }
    void prepareRound() { acceleration = Vector(0, 0); }
    void applyFriction() { speed = speed.mult(1 - FRICTION); }
    void reset() {
        wayPoints.clear();
        message.reset();
        targetPosition.reset();
        power = 0;
    }
};

struct Player {
    int id = 0;
    std::vector<Pod*> pods;
    int rank = 1;
    NString deathReason;
    Lines nextInputs;
    bool isDead() const { return deathReason.has_value(); }
};

// Referee.PodCollision (the only Collision)
struct PodCollision {
    double time = 0;
    int uniqueId = 0;
    Pod* c1 = nullptr;
    Pod* c2 = nullptr;
    double forceTotal = 0;
    Vector c1Position, c2Position, speed;

    PodCollision() {}
    PodCollision(double time, int uniqueId, Pod* a, Pod* b) : time(time), uniqueId(uniqueId), c1(a), c2(b) {}

    void react() {
        c1Position = c1->position;
        c2Position = c2->position;
        speed = c1->speed.add(c2->speed).mult(0.5);
        Vector normal = Vector::between(c1->position, c2->position).normalize();
        Vector relativeVelocity = c1->speed.sub(c2->speed);
        double force = normal.dot(relativeVelocity) / (1 / c1->getMass() + 1 / c2->getMass());
        double repulseForce = java::max(MIN_IMPULSE, force);
        forceTotal = force + repulseForce;
        Vector impulse = normal.mult(-1 * forceTotal);
        c1->applyForce(time, impulse);
        c2->applyForce(time, impulse.mult(-1));
        double distance = c1->position.distance(c2->position);
        double diff = distance - POD_RADIUS * 2;
        if (diff <= 0) {
            c1->fixPosition(c1->position.add(normal.mult(-(-diff / 2 + EPSILON))));
            c2->fixPosition(c2->position.add(normal.mult(-diff / 2 + EPSILON)));
        }
    }
    String toViewString() const {
        return S(uniqueId) + u" " + D(time) + u" " + S(c1->id) + u" " + S(java::round(c1Position.x)) + u" " + S(java::round(c1Position.y)) + u" " + S(c2->id) + u" " +
               S(java::round(c2Position.x)) + u" " + S(java::round(c2Position.y)) + u" " + D(forceTotal) + u" " + S(java::round(speed.x)) + u" " + S(java::round(speed.y));
    }
};

// shortAngleDist(a0, a1)
double shortAngleDist(double a0, double a1) {
    double max = PI * 2;
    double da = std::fmod(a1 - a0, max);
    return std::fmod(2 * da, max) - da;
}

// PLAYER_INPUT_PATTERN with Matcher#matches():
//   (?<x>-?[0-9]{1,9})\s+(?<y>-?[0-9]{1,9})\s+(?<power>([0-9]{1,9}|SHIELD))?(?:\s+(?<message>.+))?   (live: |SHIELD|BOOST)
// \s is ASCII ([ \t\n\x0B\f\r]); '.' never meets a line terminator (the base's Scanner split the lines there). Java's
// backtracking gives:
//   - x and y: an optional '-' and the whole digit run, 1 to 9 digits, each followed by a whitespace run W1 / W2;
//   - (A) all of W2, then a power P (1-9 leading digits of the rest, or SHIELD / BOOST) with a tail, else no power and
//     nothing after W2; the tail after P: nothing, or a whitespace run then the message: all the rest from its first
//     non-whitespace char, or, a whitespace-only tail of 2+ chars, its last char (a digit run shorter than 9 is tried
//     shorter too, which never helps: a digit then follows);
//   - (B) else, when W2 has 2+ chars: no power and the message = everything after W2 (non-empty: e.g. "x y  150 "
//     or "x y  shield" is a thrust of 200 with that message);
//   - else no match.
bool matchTail(const String& line, size_t i, NString& message) {
    if (i == line.size()) {
        message.reset();
        return true;
    }
    if (!java::isRegexSpace(line[i])) return false;
    size_t k = i;
    while (k < line.size() && java::isRegexSpace(line[k])) k++;
    if (k < line.size()) {
        message = line.substr(k);
        return true;
    }
    if (k - i >= 2) { // \s+ gives its last char back to .+
        message = line.substr(k - 1);
        return true;
    }
    return false;
}

bool matchNumber(const String& line, size_t& i) {
    if (i < line.size() && line[i] == u'-') i++;
    size_t d = i;
    while (i < line.size() && java::isRegexDigit(line[i])) i++;
    return i > d && i - d <= 9;
}

bool matchLine(const String& line, String& xs, String& ys, NString& power, NString& message) {
    size_t i = 0;
    if (!matchNumber(line, i)) return false;
    xs = line.substr(0, i);
    size_t w = i;
    while (i < line.size() && java::isRegexSpace(line[i])) i++;
    if (i == w) return false;
    size_t y0 = i;
    if (!matchNumber(line, i)) return false;
    ys = line.substr(y0, i - y0);
    size_t w2 = i;
    while (i < line.size() && java::isRegexSpace(line[i])) i++;
    if (i == w2) return false;
    // (A) all of W2: a power then its tail
    size_t d = i;
    while (d < line.size() && java::isRegexDigit(line[d])) d++;
    if (d > i && matchTail(line, std::min(d, i + 9), message)) {
        power = line.substr(i, std::min(d, i + 9) - i);
        return true;
    }
    for (const char16_t* word : {u"SHIELD", u"BOOST"}) {
        if (!LIVE && word[0] == u'B') continue;
        String wd(word);
        if (line.compare(i, wd.size(), wd) == 0 && matchTail(line, i + wd.size(), message)) {
            power = wd;
            return true;
        }
    }
    power.reset();
    if (i == line.size()) { // no power, no message
        message.reset();
        return true;
    }
    // (B) W2 minus its last char, no power, the message after W2
    if (i - w2 >= 2) {
        message = line.substr(i);
        return true;
    }
    return false;
}

struct Referee : legacy::MultiReferee {
    std::vector<CheckPoint> checkPoints;
    std::vector<Pod> pods;
    std::vector<Player> players;
    i64 seed = 0;
    std::vector<PodCollision> collisions; // the round's collisions (the view)
    int podTimeout = DEFAULT_POD_TIMEOUT;
    int podPerPlayer = DEFAULT_POD_PER_PLAYER;
    std::optional<i32> simulateSoloForAgent;
    int collisionUniqueId = 0; // PodCollision.COLLISION_UNIQUE_ID (static: one game per JVM)

    Referee() { base = legacy::Base::codersStrikeBack2015(); }

    static i32 parseIntOrThrow(const String& s) {
        i32 v;
        if (!java::parseInt(s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(s))};
        return v;
    }

    int lapCount() const { return LIVE ? LAPS : (int)(std::ceil((double)MIN_CHECKPOINT_COUNT) / (double)checkPoints.size()); }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        // Long.valueOf(prop.getProperty("seed", String.valueOf(new Random(System.currentTimeMillis()).nextLong())))
        if (const String* s = prop.find(u"seed")) {
            i64 v;
            if (!java::parseLong(*s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(*s))};
            seed = v;
        } else {
            java::Random r((i64)java::entropy());
            seed = r.nextLong();
        }
        java::Random r(seed);
        static const std::vector<std::vector<Point>> MAPS = {
            {{12460, 1350}, {10540, 5980}, {3580, 5180}, {13580, 7600}},
            {{3600, 5280}, {13840, 5080}, {10680, 2280}, {8700, 7460}, {7200, 2160}},
            {{4560, 2180}, {7350, 4940}, {3320, 7230}, {14580, 7700}, {10560, 5060}, {13100, 2320}},
            {{5010, 5260}, {11480, 6080}, {9100, 1840}},
            {{14660, 1410}, {3450, 7220}, {9420, 7240}, {5970, 4240}},
            {{3640, 4420}, {8000, 7900}, {13300, 5540}, {9560, 1400}},
            {{4100, 7420}, {13500, 2340}, {12940, 7220}, {5640, 2580}},
            {{14520, 7780}, {6320, 4290}, {7800, 860}, {7660, 5970}, {3140, 7540}, {9520, 4380}},
            {{10040, 5970}, {13920, 1940}, {8020, 3260}, {2670, 7020}},
            {{7500, 6940}, {6000, 5360}, {11300, 2820}},
            {{4060, 4660}, {13040, 1900}, {6560, 7840}, {7480, 1360}, {12700, 7100}},
            {{3020, 5190}, {6280, 7760}, {14100, 7760}, {13880, 1220}, {10240, 4920}, {6100, 2200}},
            {{10323, 3366}, {11203, 5425}, {7259, 6656}, {5425, 2838}},
        };
        collisions.clear();
        simulateSoloForAgent.reset();
        if (const String* s = prop.find(u"simulate_solo_for_agent")) simulateSoloForAgent = parseIntOrThrow(*s);
        podTimeout = parseIntOrThrow(prop.getProperty(u"pod_timeout", S(DEFAULT_POD_TIMEOUT)));
        podPerPlayer = parseIntOrThrow(prop.getProperty(u"pod_per_player", S(DEFAULT_POD_PER_PLAYER)));

        if (const String* mapProperty = prop.find(u"map")) {
            std::vector<String> tab = java::split(*mapProperty, u' ');
            if (tab.size() % 2 != 0) throw legacy::InvalidFormatException{u"Map must contain at least 2 checkpoints"};
            if (tab.size() < 4) throw legacy::InvalidFormatException{u"Map must contain at least 2 checkpoints"};
            checkPoints.clear();
            for (size_t i = 0; i < tab.size(); i += 2) checkPoints.push_back(CheckPoint{Point{parseIntOrThrow(tab[i]), parseIntOrThrow(tab[i + 1])}});
        } else {
            int map = r.nextInt((i32)MAPS.size()); // Integer.valueOf(String.valueOf(r.nextInt(maps.size())))
            generateMap(r, MAPS[std::min((int)MAPS.size() - 1, std::max(0, map))]);
        }
        if (checkPoints.size() < 2) throw java::Exception{java::arrayIndexOutOfBoundsException((i64)checkPoints.size(), (i64)checkPoints.size())}; // (never)
        generatePlayers(playerCount, checkPoints[0].position, Vector::between(checkPoints[0].position, checkPoints[1].position));
    }

    void generateMap(java::Random& r, const std::vector<Point>& map) {
        // Collections.rotate(points, d): the element i goes to (i + d) % n
        int n = (int)map.size();
        int d = r.nextInt(n);
        std::vector<Point> points(n);
        for (int i = 0; i < n; i++) points[(i + d) % n] = map[i];
        checkPoints.clear();
        for (const Point& p : points) {
            i32 x = p.x + r.nextInt(CHECKPOINT_GENERATION_MAX_GAP * 2 + 1) - CHECKPOINT_GENERATION_MAX_GAP;
            i32 y = p.y + r.nextInt(CHECKPOINT_GENERATION_MAX_GAP * 2 - 1) - CHECKPOINT_GENERATION_MAX_GAP;
            checkPoints.push_back(CheckPoint{Point{x, y}});
        }
    }

    void generatePlayers(int playerCount, const Point& startPoint, const Vector& direction) {
        players.assign(playerCount, Player());
        for (int i = 0; i < playerCount; ++i) players[i].id = i;
        int podCount = podPerPlayer * playerCount;
        Vector vector = direction.normalize().cross(1);
        pods.clear();
        pods.reserve(podCount);
        for (int i = 0; i < podCount; ++i) {
            double offset = ((i % 2 == 0 ? -1 : 1) * (i / 2 * 2 + 1) + podCount % 2);
            pods.emplace_back(i, vector.mult(offset * (POD_RADIUS + SPACE_BETWEEN_POD)).translate(startPoint).round());
        }
        for (int i = 0; i < podCount; ++i) {
            Player& player = players[i / podPerPlayer];
            pods[i].player = &player;
            player.pods.push_back(&pods[i]);
        }
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        String map;
        for (const CheckPoint& p : checkPoints) map += S(p.position.x) + u" " + S(p.position.y) + u" ";
        prop.setProperty(u"map", java::trim(map));
        prop.setProperty(u"pod_timeout", S(podTimeout));
        prop.setProperty(u"pod_per_player", S(podPerPlayer));
        if (simulateSoloForAgent) prop.setProperty(u"simulate_solo_for_agent", S(*simulateSoloForAgent));
        return prop;
    }

    int getMaxRoundCount(int) override { return 500; }

    void populateMessages(std::map<std::string, String>& p) override {
        p["endReached"] = u"End reached";
        p["podDead"] = u"Pod %d of player $%d is eliminated";
        p["podAction"] = u"Pod %d of player $%d moves towards (%d, %d) at power %d";
        p["podReloadShield"] = LIVE ? u"Pod %d of player $%d waits for its engine to reload" : u"Pod %d of player $%d waits for it's engine to reload";
        p["podActionShield"] = LIVE ? u"Pod %d of player $%d enabled its shield" : u"Pod %d of player $%d enabled it's shield";
        p["podBoost"] = u"Pod %d of player $%d boosts towards (%d, %d)";
        p["checkPointTimeout"] = u"$%d did not reach the next checkpoint in time";
    }

    // --- the pods ---
    int getNextCheckpointId(const Pod& p) const {
        int n = (int)checkPoints.size();
        int size = (int)p.checkPointTimes.size();
        // live: a pod that finished keeps 0 (its count capped at the race's)
        if (LIVE) size = std::min(size, LAPS * n);
        return size % n;
    }

    // the live referee's time of the segment before -> after entering the checkpoint's circle (the first point of
    // the segment within its radius): the patch's crossTime
    static double crossTime(const PodWayPoint& before, const PodWayPoint& after, const CheckPoint& cp) {
        double dx = before.position.x - cp.position.x, dy = before.position.y - cp.position.y;
        double ex = after.position.x - before.position.x, ey = after.position.y - before.position.y;
        double c = dx * dx + dy * dy - CHECKPOINT_RADIUS * CHECKPOINT_RADIUS;
        double s = 0;
        if (c > 0) {
            double a = ex * ex + ey * ey, b = dx * ex + dy * ey;
            double delta = b * b - a * c;
            s = (-b - std::sqrt(java::max(0, delta))) / a;
            s = java::max(0, java::min(1, s));
        }
        return before.time + s * (after.time - before.time);
    }

    void checkCheckPoints(Pod& pod, int round) {
        std::vector<PodWayPoint> positions;
        positions.push_back(PodWayPoint{0, pod.lastPosition});
        for (const PodWayPoint& pwp : pod.wayPoints) positions.push_back(pwp);
        positions.push_back(PodWayPoint{1, pod.position});
        bool found = true;
        size_t start = pod.checkPointTimes.size();
        while (found) {
            found = false;
            if (pod.checkPointTimes.size() - start > (size_t)MAX_STEPS) throw java::Exception{"cg-2016-coders-strike-back: the checkpoint loop does not end"};
            const CheckPoint& cp = checkPoints[pod.checkPointTimes.size() % checkPoints.size()];
            for (size_t i = 0; i + 1 < positions.size(); ++i) {
                const PodWayPoint& before = positions[i];
                const PodWayPoint& after = positions[i + 1];
                if (java::ptSegDist(before.position.x, before.position.y, after.position.x, after.position.y, cp.position.x, cp.position.y) <= CHECKPOINT_RADIUS) {
                    pod.checkPointTimes.push_back(LIVE ? round + crossTime(before, after, cp) : (double)round);
                    found = true;
                    break;
                }
            }
        }
    }

    String toPlayerString(const Pod& p) const {
        i64 angle = -1;
        if (p.lastAngle) angle = java::round(std::fmod(std::fmod(*p.lastAngle, PI * 2) + PI * 2, PI * 2) * 180 / PI);
        return S(java::round(p.position.x)) + u" " + S(java::round(p.position.y)) + u" " + S(java::round(p.speed.x)) + u" " + S(java::round(p.speed.y)) + u" " + S(angle) +
               u" " + S(p.player->isDead() ? -1 : getNextCheckpointId(p));
    }

    Lines podViewStrings(const Pod& p) const {
        String pods = D(p.position.x) + u" " + D(p.position.y) + u" " + S(java::round(p.speed.x)) + u" " + S(java::round(p.speed.y)) + u" " + S(p.power) + u" " +
                      S(p.player->isDead() ? 1 : 0);
        if (p.targetPosition) pods += u" " + S(p.targetPosition->x) + u" " + S(p.targetPosition->y);
        else pods += u" null null";
        pods += u" " + (p.lastAngle ? D(*p.lastAngle) : String(u"null"));
        pods += u" " + S(p.shieldMode ? 1 : 0);
        pods += u" " + S(LIVE ? getNextCheckpointId(p) : (int)(p.checkPointTimes.size() % checkPoints.size()));
        pods += u" " + S(p.rank);
        return {pods, u"\"" + (p.message ? *p.message : String()) + u"\""};
    }

    // Pod#compareTo
    int compare(const Pod& a, const Pod& b) const {
        if (a.player->isDead() && !b.player->isDead()) return 1;
        if (!a.player->isDead() && b.player->isDead()) return -1;
        if (a.checkPointTimes.size() > b.checkPointTimes.size()) return -1;
        if (a.checkPointTimes.size() < b.checkPointTimes.size()) return 1;
        if (LIVE && (int)a.checkPointTimes.size() >= LAPS * (int)checkPoints.size()) {
            // live: on the last leg, the earlier last checkpoint first
            double time1 = a.checkPointTimes.back(), time2 = b.checkPointTimes.back();
            return time1 < time2 ? -1 : (time1 > time2 ? 1 : 0);
        }
        const CheckPoint& nextCheckpoint = checkPoints[getNextCheckpointId(a)];
        const CheckPoint& nextCheckpoint2 = checkPoints[getNextCheckpointId(b)];
        Vector next(nextCheckpoint.position.x, nextCheckpoint.position.y);
        Vector next2(nextCheckpoint2.position.x, nextCheckpoint2.position.y);
        double diff = a.position.distance(next) - b.position.distance(next2);
        if (diff < 0) return -1;
        if (diff > 0) return 1;
        return 0;
    }

    // --- the base's methods ---
    Lines getInitDataForView() override {
        Lines list;
        list.push_back(S(MAP_WIDTH) + u" " + S(MAP_HEIGHT) + u" " + S(POD_RADIUS) + u" " + S(CHECKPOINT_RADIUS) + u" " + S((i64)players.size() * podPerPlayer) + u" " +
                       D(MAX_ROTATION_PER_TURN));
        String cps;
        for (size_t i = 0; i < checkPoints.size(); i++) cps += (i ? u" " : u"") + S(checkPoints[i].position.x) + u" " + S(checkPoints[i].position.y);
        list.push_back(cps);
        String ps;
        for (size_t i = 0; i < pods.size(); i++) ps += (i ? u" " : u"") + S(pods[i].player->id) + u" " + D(pods[i].position.x) + u" " + D(pods[i].position.y);
        list.push_back(ps);
        return list;
    }

    Lines getFrameDataForView(int round, int, int, bool keyFrame) override {
        if (!keyFrame) return {};
        Lines list;
        for (const Pod& p : pods) {
            Lines v = podViewStrings(p);
            list.insert(list.end(), v.begin(), v.end());
        }
        String ranks;
        for (size_t i = 0; i < players.size(); i++) {
            const Player& p = players[i];
            if (i) ranks += u" ";
            i32 lastCheckPointTime = 0;
            for (const Pod* pod : p.pods) lastCheckPointTime = std::max(lastCheckPointTime, java::d2i(pod->getLastCheckPointTime()));
            i32 timeout = java::add(java::sub(podTimeout, round), lastCheckPointTime);
            ranks += S(p.rank) + u":" + S(timeout);
        }
        list.push_back(ranks);
        for (const PodCollision& c : collisions) list.push_back(c.toViewString());
        return list;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return podPerPlayer; }
    String getGameName() override { return u"CodersStrikeBack"; }

    void handlePlayerOutput(int, int round, int playerIdx, const Lines& outputs) override {
        Player& player = players[playerIdx];
        size_t i = 0;
        for (const String& line : outputs) {
            String xs, ys;
            NString powerStr, message;
            if (!matchLine(line, xs, ys, powerStr, message)) {
                player.deathReason = String(u"invalidInput");
                throw legacy::InvalidInputException(u"x y power", line);
            }
            if (i >= player.pods.size()) throw java::Exception{java::indexOutOfBoundsException((i64)i, (i64)player.pods.size())}; // (never)
            Pod& pod = *player.pods[i];
            if (!pod.player->isDead()) {
                i32 x = parseIntOrThrow(xs), y = parseIntOrThrow(ys);
                pod.shieldMode = powerStr && *powerStr == u"SHIELD";
                if (pod.shieldMode) {
                    pod.wantedPower = 0;
                    pod.lastShieldModeRound = round;
                } else if (LIVE && powerStr && *powerStr == u"BOOST") {
                    pod.wantedPower = pod.boostUsed ? MAX_POWER : BOOST_POWER;
                    pod.boostUsed = true;
                } else {
                    pod.wantedPower = powerStr ? parseIntOrThrow(*powerStr) : MAX_POWER;
                    if (pod.wantedPower < 0 || pod.wantedPower > MAX_POWER) {
                        player.deathReason = String(u"invalidInput");
                        throw legacy::InvalidInputException(u"0 <= power <= " + S(MAX_POWER), u"power = " + S(pod.wantedPower));
                    }
                }
                pod.power = pod.wantedPower;
                if (pod.lastShieldModeRound && round <= *pod.lastShieldModeRound + INACTIVE_ROUND_AFTER_SHIELD) pod.power = 0;
                pod.targetPosition = Point{x, y};
                if (pod.position.x != x || pod.position.y != y) {
                    double angle = Vector::between(pod.position, Vector(x, y)).angle();
                    if (pod.lastAngle) {
                        double relativeAngle = shortAngleDist(*pod.lastAngle, angle);
                        if (std::fabs(relativeAngle) >= MAX_ROTATION_PER_TURN) angle = *pod.lastAngle + MAX_ROTATION_PER_TURN * java::signum(relativeAngle);
                    }
                    pod.lastAngle = angle;
                    Vector direction(java::cos(angle), java::sin(angle));
                    pod.acceleration = direction.normalize().mult(pod.power);
                } else {
                    pod.acceleration = Vector(0, 0);
                }
            }
            pod.message = message;
            if (pod.message && pod.message->size() > 50) pod.message = pod.message->substr(0, 50) + u"...";
            ++i;
        }
    }

    Lines getInitInputForPlayer(int playerIdx) override {
        Lines data;
        if (!simulateSoloForAgent || *simulateSoloForAgent != playerIdx) {
            data.push_back(S(lapCount()));
            data.push_back(S((i64)checkPoints.size()));
            for (const CheckPoint& cp : checkPoints) data.push_back(S(cp.position.x) + u" " + S(cp.position.y));
        }
        return data;
    }

    Lines getInputForPlayer(int, int playerIdx) override { return players[playerIdx].nextInputs; }

    Lines preGetInputForPlayer(int playerIdx) const {
        Lines data;
        const Player& player = players[playerIdx];
        if (!simulateSoloForAgent || *simulateSoloForAgent != playerIdx) {
            for (const Pod& p : pods)
                if (p.player == &player) data.push_back(toPlayerString(p));
            for (const Pod& p : pods)
                if (p.player != &player) data.push_back(toPlayerString(p));
        } else {
            for (const Pod& p : pods)
                if (p.player == &player) {
                    const CheckPoint& next = checkPoints[getNextCheckpointId(p)];
                    data.push_back(S(java::round(p.position.x)) + u" " + S(java::round(p.position.y)) + u" " + S(next.position.x) + u" " + S(next.position.y));
                }
        }
        return data;
    }

    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return players.size() == 1 ? 1 : 2; }
    bool showTooltips() override { return true; }

    void prepare(int) override {
        for (Player& p : players) {
            if (!p.isDead()) p.nextInputs = preGetInputForPlayer(p.id);
            else p.nextInputs.clear();
            for (Pod* pod : p.pods) {
                pod->reset();
                pod->prepareRound();
            }
        }
        collisions.clear();
    }

    void updateGame(int round) override {
        std::vector<Pod*> ps;
        for (Player& p : players) ps.insert(ps.end(), p.pods.begin(), p.pods.end());
        for (Pod* p : ps) {
            p->lastPosition = p->position;
            p->applyForce(p->acceleration);
        }
        double time = 0;
        long long steps = 0;
        std::vector<Vector> before(ps.size() * 2);
        while (time < 1) {
            if (++steps > MAX_STEPS) throw java::Exception{"cg-2016-coders-strike-back: more than 10^5 steps in a round"};
            for (size_t i = 0; i < ps.size(); i++) before[i * 2] = ps[i]->position, before[i * 2 + 1] = ps[i]->speed;
            double s = step(ps, time);
            // a step of no time that changed nothing repeats forever (pods at the same point: no normal, no impulse,
            // no separation): the Java referee loops (the jar's runner gives up after 150 s)
            bool same = s == 0;
            for (size_t i = 0; same && i < ps.size(); i++)
                same = before[i * 2].x == ps[i]->position.x && before[i * 2].y == ps[i]->position.y && before[i * 2 + 1].x == ps[i]->speed.x &&
                       before[i * 2 + 1].y == ps[i]->speed.y;
            if (same) throw java::Exception{"cg-2016-coders-strike-back: the collision loop does not end"};
            time += s;
        }
        for (Pod* p : ps) {
            p->applyFriction();
            p->endRound();
        }
        size_t lastCheckPoint = (size_t)lapCount() * checkPoints.size();
        bool ended = false;
        for (Pod* p : ps) {
            if (!p->player->isDead()) {
                checkCheckPoints(*p, round);
                if (p->checkPointTimes.size() > lastCheckPoint) ended = true;
            }
        }
        for (Player& p : players) {
            bool dead = true;
            for (Pod* pod : p.pods)
                if (round - pod->getLastCheckPointTime() < podTimeout) dead = false;
            if (dead) p.deathReason = translate("checkPointTimeout", {p.id});
        }
        std::vector<Pod*> podRanking = ps;
        java::sort(podRanking, [&](Pod* a, Pod* b) { return compare(*a, *b); });
        for (Player& p : players) p.rank = (int)ps.size();
        int rank = 0;
        for (size_t i = 0; i < podRanking.size(); ++i) {
            if (i == 0 || compare(*podRanking[i], *podRanking[i - 1]) > 0) rank++;
            podRanking[i]->rank = rank;
            Player* player = podRanking[i]->player;
            if (rank < player->rank) player->rank = rank;
        }
        for (int i = 1; i <= (int)ps.size(); ++i) {
            int _rank = i;
            bool found = false;
            while (!found && _rank <= (int)ps.size()) {
                for (Player& p : players) {
                    if (p.rank == _rank) {
                        p.rank = i;
                        found = true;
                    }
                }
                _rank++;
            }
        }
        if (ended) throw legacy::GameOverException(std::string("endReached"));
    }

    double step(std::vector<Pod*>& ps, double beginTime) {
        std::vector<PodCollision> found;
        double min = 1 - beginTime;
        for (int i = (int)ps.size() - 1; i >= 0; --i) {
            Pod* pod1 = ps[i];
            for (int j = i - 1; j >= 0; --j) {
                Pod* pod2 = ps[j];
                if (pod2->collide(*pod1)) {
                    found.push_back(PodCollision(beginTime, collisionUniqueId++, pod1, pod2));
                } else {
                    double colision = pod1->contactPosition(*pod2);
                    if (colision <= min && colision > 0) found.push_back(PodCollision(beginTime + colision, collisionUniqueId++, pod1, pod2));
                }
            }
        }
        if (!found.empty()) {
            java::sort(found, [](const PodCollision& a, const PodCollision& b) { return java::compareDouble(a.time, b.time); });
            double time = -1;
            for (PodCollision& c : found) {
                if (time <= -1) {
                    time = c.time;
                    double s = time - beginTime;
                    for (Pod* ch : ps) ch->step(s);
                }
                if (c.time <= time + EPSILON) {
                    c.react();
                    collisions.push_back(c);
                } else {
                    break;
                }
            }
            return time - beginTime;
        }
        double s = 1 - beginTime;
        for (Pod* ch : ps) ch->step(s);
        return s;
    }

    Lines getPlayerActions(int playerIdx, int round) override {
        Lines action;
        int podId = 1;
        for (Pod* p : players[playerIdx].pods) {
            if (p->player->isDead()) {
                action.push_back(translate("podDead", {podId, p->player->id}));
            } else if (p->shieldMode) {
                action.push_back(translate("podActionShield", {podId, p->player->id}));
            } else if (p->lastShieldModeRound && round <= *p->lastShieldModeRound + INACTIVE_ROUND_AFTER_SHIELD) {
                action.push_back(translate("podReloadShield", {podId, p->player->id}));
            } else {
                if (!p->targetPosition) throw java::Exception{"java.lang.NullPointerException"}; // (never: the pod played)
                if (LIVE && p->power == BOOST_POWER) action.push_back(translate("podBoost", {podId, p->player->id, p->targetPosition->x, p->targetPosition->y}));
                else action.push_back(translate("podAction", {podId, p->player->id, p->targetPosition->x, p->targetPosition->y, p->power}));
            }
            podId++;
        }
        return action;
    }

    bool isPlayerDead(int playerIdx) override { return players[playerIdx].isDead(); }
    NString getDeathReason(int playerIdx) override { return players[playerIdx].deathReason; }
    int getScore(int playerIdx) override {
        if (players[playerIdx].isDead()) return 0;
        return (int)players.size() - players[playerIdx].rank + 1;
    }
    Lines getGameSummary(int) override {
        Lines summary;
        for (const Player& p : players) {
            if (p.isDead()) summary.push_back(u"$" + S(p.id) + u" eliminated! rank: " + S(p.rank));
            else summary.push_back(u"$" + S(p.id) + u" rank: " + S(p.rank));
        }
        return summary;
    }
    void setPlayerTimeout(int, int, int playerIdx) override { players[playerIdx].deathReason = String(u"playerTimeout"); }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
