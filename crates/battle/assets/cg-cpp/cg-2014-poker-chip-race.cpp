// Poker Chip Race (CodinGame, 2014-2015, "poker-chip-race") - headless C++ referee, port of the CodinGame referee in
// https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the only commit of the file),
// "Referees/Poker Chip Race/Referee.java": the single-file referee `class Referee extends MultiReferee` with CodinGame's
// old AbstractReferee base embedded (common/legacy.h, legacy::Base::pokerChipRace2014(): the 2014 INFOS / SUMMARY, no
// tooltip for a loss by an invalid line, the timeout's info as its tooltip, the scores updated after each update,
// getMillisTimeForRound 150, no precomputed inputs, the frame data and the tooltips sent in every frame; its SoloReferee
// class is unused). The file is the game CodinGame runs today: its classes reproduce all 608 CodinGame replays fetched
// (the top 1000 players' last battles, 2 to 4 players: views, infos, summaries, tooltips, scores) from each replay's own
// init properties and the order of the shots its views show. 2 to 4 players (getMinimumPlayerCount() 2). No league rules
// in the referee (CodinGame's six divisions play the same file). Deterministic: one of the games the reference jar plays
// for a given -seed (the jar itself is not deterministic, see below).
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 <cmd> -p4 <cmd>] -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): seed, chips_count_per_player,
// max_neutral_count, player_initial_radius, symmetric, in the order of a JDK 9+ Properties) to stdout like the jar
// (referees/cg-builder/legacy: the referee runs unchanged in the runner of the SDK 4.5.0). "null" scores when the game
// fails (the Java referee crashed or the protocol broke; the jar after 150 s). -l writes the trace of common/legacy.h (bot
// executions, view sizes, summaries, tooltips, END; CG_TRACE_VIEWS=1 adds the text of every view). Time limits: the base's
// 1000 ms for the first turn, 150 ms after (the game overrides neither).
//
// Init properties: max_neutral_count (players + nextInt(50 - players)), chips_count_per_player (1 to 5) and
// player_initial_radius (18 to 25) are drawn from the seed unless given, symmetric is true for 4 players. CodinGame
// passes its own values of all four (its replays: none of them follows the seed's draws, and 2- and 3-player games are
// symmetric too in 10% and 19% of them); the jar's CLI passes only the seed, so the jar and this port play the seed's
// draws. The validation build -DPOKER_CHIP_RACE_PROPS=1 takes more init properties from the environment variable
// PCR_PROPS ("key=value" lines, e.g. CodinGame's) to replay CodinGame's games.
//
// The game: an 800 x 515 board; each player starts with chips_count_per_player chips of radius player_initial_radius at
// random places (or point / mirror symmetric ones), among max_neutral_count neutral chips of radius 10 to 25 (the smaller
// ones than the players' moving at up to 10 per round). Each round every player still in the game answers one line per
// chip it owns: WAIT, or "x y" (any Java double) to push the chip towards (x, y): the chip ejects 1/15 of its area as a
// new neutral chip behind it (an ejection speed of 200 shared by mass), unless its radius is 5 or less ("too small") or the
// point is closer than 0.5 ("too close"); either may be followed by spaces and a message (20 chars shown in the view).
// A line that does not match, an infinite or NaN coordinate, or an unparsable one (for a chip that can shoot) puts the
// player out (its chips become neutral; the shots of its earlier lines still happen). After the last player, the
// shots, then one round of continuous physics: chips bounce on the borders; touching chips of equal radius bounce, else
// the bigger eats the smaller (areas and momenta added). A player whose chips are all eaten is out. The game ends when
// fewer than 2 players are left or after 300 rounds. Scores: the players ranked by total area, the dead ones first by the
// time they died; a living player's score is its area (rounded) plus its rank, a dead one's its rank.
//
// The Java referee is followed closely, notably: the generation of the map (every draw of its java.util.Random, the eager
// defaults of the properties, the failed attempts redrawn up to 20 times with the global chip counter going on, an
// attempt failing 21 times caught and played with the last partial map), the regex of a line (Matcher#matches of
// (?<action>(?:WAIT|(?<x>\S+) (?<y>\S+)))( +(?<message>.+))? with CASE_INSENSITIVE: see matchLine), Double.parseDouble,
// the order of the checks, the physics (step2: the border and pair collision times in the referee's loop orders, the
// collisions sorted by time (TimSort, stable) and those within EPSILON of the first one applied together; the chips'
// parent links ignored until a waypoint clears them, which lets a chip eat its own ejected chip; a chip eaten twice in one
// update listed twice), every double operation in the Java order (no FMA), Math.sin / cos (HotSpot's intrinsics,
// java::sin / cos), Math.atan2 (fdlibm), the waypoints of the view (their cap of 8, a waypoint at the same time replacing
// the last one), the ranking (TimSort with Player#compareTo, its ties), the view / infos / summary / input texts in full
// (String.format %f / %.2f: java::formatFixed; trims).
//
// The order of the shots: updateGame runs the round's shots by iterating playersActions, a HashSet of anonymous
// Runnables, i.e. in the order of their identity hash codes (and a shot's new chip gets the next chip id: the ids every
// player sees and the order of the chips change with it, then the physics). HotSpot draws identity hash codes from a
// per-thread xor-shift generator (java::IdentityHashes) seeded with the n-th value of the JVM's os::random(), where n
// depends on how many threads the JVM created before the referee thread: it changes from launch to launch (seen on this
// machine: n = 19821 or 19822 for sequential launches, 19819 to 19824 under load), so the jar plays the same seed
// differently from run to run (e.g. a 2-player game of seed 11 gave three different outcomes in six runs). CodinGame's
// own referee did the same (its replays show the shots of 2-shot rounds in a random order). This port models the
// referee thread's generator: every hash the referee thread makes (K0 before the first shot: the runner's and the
// referee's start; each Runnable, each Chip first hashed by a bounce set (HashSet<Chip>: JDK 26's contains() hashes only
// once the set has a table), each Tooltip, and the hashes HotSpot makes when the referee thread first uses a class or a
// JDK path: see Cost, measured with diagnostic builds of the jar), and plays the order the jar plays with n = SEED_CALL
// (19822, the most frequent). The base's tooltips (a HashSet of identity hash codes too) are printed in the same model's
// order. The validation build -DPOKER_CHIP_RACE_HINTS=1 takes another n and K0 from PCR_IDH ("n k0"), or an explicit order
// of each round's shots from PCR_ORDER ("round:chipId,chipId,...;..."), and PCR_IDH_TRACE prints the model's events.
// Measured against the real jar: in the 3288 referee-diff games 1623 matched and all 1665 others were reproduced exactly
// by this port with the jar's own n (19821: 1081, 19820: 345, 19823: 190, 19819: 43, 19824: 6), every view, infos,
// summary and tooltip included; so did 454 direct games (raw bytes included). The model holds for JDK 26 on x86_64
// (another JDK makes other hashes before the first shot or at the first use of a class: the jar's order changes, this
// port keeps this one).
//
// Not reproduced / not modelled: the jar's own run-to-run variation (above); the wall clock (see engine.h); the hashes of
// an init that fails 21 times (its printStackTrace; init properties only: no seed of 2 million 4-player seeds does);
// helpful NullPointerException messages (only after an init error, never shown).
//
// Validation (referees/README.md): referee-diff against the jar with 31 deterministic bots (every ordered pair on two seed
// bases, 3- and 4-player covering designs on two more, 400 random 3- and 4-player line-ups on a fifth: 3288 games, the
// mismatches all explained as above), direct runs comparing the jar's game logs with this port's trace (454 games: every
// bot in 2-, 3- and 4-player games, raw bytes and line endings), the jar's own compiled classes driven by reflection over
// random states and commands (120,000 cases, 44 of 52 mutations detected, the others equivalent or needing exact float
// ties), the model of the jar's identity hash codes against diagnostic builds of the jar (every Runnable and Tooltip hash
// of 90 games), and all 608 CodinGame replays through this port's CLI (with CodinGame's properties and order).
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/legacy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#ifndef POKER_CHIP_RACE_HINTS
#define POKER_CHIP_RACE_HINTS 0
#endif
#ifndef POKER_CHIP_RACE_PROPS
#define POKER_CHIP_RACE_PROPS 0
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

String S(i64 v) { return java::str(v); }
String F(double v, int precision) { return java::ascii(java::formatFixed(v, precision)); }

// Math.PI
const double PI = 3.141592653589793;
const double EPSILON = 0.00001;
// SIZE = new Dimension(800, 515): getWidth() / getHeight() are doubles, width / height ints
const double WIDTH = 800, HEIGHT = 515;
const double CHIP_MIN_RADIUS = 5;
const int MAX_MESSAGE_LENGTH = 20;
// 10f*10f*Math.PI: a float product, then a double one
const double CHIP_MIN_START_AREA = (double)(10.0f * 10.0f) * PI;
const double CHIP_MAX_START_AREA = (double)(25.0f * 25.0f) * PI;
const double CHIP_MIN_START_RADIUS = std::sqrt(CHIP_MIN_START_AREA / PI);
const double CHIP_MAX_START_RADIUS = std::sqrt(CHIP_MAX_START_AREA / PI);
const double CHIP_EJECTION_SPEED = (double)200.0f;
const double CHIP_EJECTION_RATIO = (double)(1.0f / 15.0f);
const int MAX_ROUND_COUNT = 300;
const int MINIMUM_PLAYER_COUNT = 2;
const int MAX_DEFAULT_CHIP_COUNT = 50;
// the physics loop of one round: the Java referee loops forever when its time stops advancing (the jar fails after
// 150 s); the port stops after this many steps of a round and fails the same way
const long long MAX_PHYSICS_STEPS = 1000000;

// ----------------------------------------------------------------------------
// The identity hash codes of the jar's referee thread
// ----------------------------------------------------------------------------
// Referee#updateGame runs the players' shots of a round by iterating a HashSet<Runnable> of anonymous classes:
// identity hash codes, i.e. HotSpot's per-thread xor-shift (java::IdentityHashes). The referee thread hashes, in
// this order: K0 objects before the game's first Runnable (the runner's start, the referee's init and first frames:
// the JDK's own objects), then each Runnable when it is added to the set, each Chip the first time a bounce set
// (HashSet<Chip>) hashes it, each Tooltip when the base adds it to its HashSet<Tooltip>, and a few objects when a
// class or a JDK code path is used for the first time (see Cost). The thread's seed is os::random()'s n-th value,
// n = SEED_CALL by default: the value the jar's referee thread gets most often on an idle machine; the jar's own n
// varies from launch to launch (see the header).
enum Cost { ATAN2, RUNNABLE, WAYPOINT, COLISION, BORDER_COLISION, BORDER_REACT, CHIP_COLISION, TOOLTIP, INVALID_INPUT, BAD_NUMBER, NCOSTS };
const char* const COST_NAME[NCOSTS] = {"ATAN2", "RUNNABLE", "WAYPOINT", "COLISION", "BORDER_COLISION", "BORDER_REACT", "CHIP_COLISION", "TOOLTIP", "INVALID_INPUT", "BAD_NUMBER"};
const int SEED_CALL = 19822;
const int K0 = 86;
const int COST_DEFAULT[NCOSTS] = {3, 3, 3, 3, 6, 3, 3, 3, 2, 1};

struct IdentityModel {
    java::IdentityHashes stream;
    int costs[NCOSTS];
    bool used[NCOSTS] = {};
    bool trace = false;
    void init(int seedCall, int k0) {
        stream = java::IdentityHashes(java::IdentityHashes::osRandom(seedCall));
        stream.skip(k0);
        for (int i = 0; i < NCOSTS; i++) costs[i] = COST_DEFAULT[i];
    }
    i32 next() { return stream.next(); }
    // a first use: the hashes the JVM makes then (class loading, a JDK path's first run)
    void first(Cost c) {
        if (used[c]) return;
        if (c == BORDER_COLISION || c == CHIP_COLISION) first(COLISION); // (their superclass, loaded with the first of them)
        used[c] = true;
        if (trace) fprintf(stderr, "IDH FIRST %s %lld\n", COST_NAME[c], (long long)stream.count);
        stream.skip(costs[c]);
    }
};
IdentityModel idh;

// Referee.Vector (immutable)
struct Vec {
    double x = 0, y = 0;
    Vec() {}
    Vec(double x_, double y_) : x(x_), y(y_) {}
    // new Vector(a, b): b - a
    static Vec between(const Vec& a, const Vec& b) { return Vec(b.x - a.x, b.y - a.y); }
    double distance(const Vec& v) const { return std::sqrt((v.x - x) * (v.x - x) + (v.y - y) * (v.y - y)); }
    Vec add(const Vec& v) const { return Vec(x + v.x, y + v.y); }
    Vec mult(double f) const { return Vec(x * f, y * f); }
    Vec sub(const Vec& v) const { return Vec(x - v.x, y - v.y); }
    double length() const { return std::sqrt(x * x + y * y); }
    double lengthSquared() const { return x * x + y * y; }
    Vec normalize() const {
        double l = length();
        if (l == 0) return Vec(0, 0);
        return Vec(x / l, y / l);
    }
    double dot(const Vec& v) const { return x * v.x + y * v.y; }
    double angle() const {
        idh.first(ATAN2);
        return java::atan2(y, x);
    }
    Vec hsymmetric(double center) const { return Vec(2 * center - x, y); }
    Vec vsymmetric(double center) const { return Vec(x, 2 * center - y); }
    Vec vsymmetric() const { return Vec(x, -y); }
    Vec hsymmetric() const { return Vec(-x, y); }
    Vec symmetric(const Vec& c) const { return Vec(c.x * 2 - x, c.y * 2 - y); }
    Vec checkBounds(double minx, double miny, double maxx, double maxy) const {
        if (x < minx || x > maxx || y < miny || y > maxy) return Vec(java::min(maxx, java::max(minx, x)), java::min(maxy, java::max(miny, y)));
        return *this;
    }
};


struct Player;

// Referee.Chip
struct Chip {
    struct WayPoint {
        double time;
        Vec position;
        double radius;
        const Chip* eaten;
        double rotation;
        // ChipWayPoint#toString
        String toString() const {
            return F(time, 2) + u" " + F(position.x + WIDTH / 2, 2) + u" " + F(position.y + HEIGHT / 2, 2) + u" " + F(radius, 2) + u" " + F(rotation, 2) + u" " +
                   S(eaten ? eaten->id : -1);
        }
    };

    int id = 0;
    std::vector<WayPoint> wayPoints;
    Player* player = nullptr;
    const Chip* parent = nullptr;
    NString message;
    Vec position, velocity;
    double radius = 0, rotation = 0;
    // HashSet<Chip> bounce: only add / contains / clear
    std::vector<const Chip*> bounce;
    NString lastAction;
    // the identity hash code model (see IdentityModel): this chip's hash code was made; its bounce set has a table
    bool hashed = false, bounceTable = false;

    void checkBounds(double minx, double miny, double maxx, double maxy) {
        position = position.checkBounds(minx + radius + EPSILON, miny + radius + EPSILON, maxx - radius - EPSILON, maxy - radius - EPSILON);
    }
    double getArea() const { return radius * radius * PI; }
    double getMass() const { return getArea(); }
    // the private setArea(area)
    void setArea(double area) { radius = std::sqrt(area / PI); }
    void setRadius(double time, double r) {
        radius = r;
        addWayPoint(time);
    }
    void setLinearVelocity(double time, const Vec& v) {
        velocity = v;
        addWayPoint(time);
    }
    void setPosition(double time, const Vec& p) {
        position = p;
        addWayPoint(time);
    }
    void addWayPoint(double time);
    void reset() {
        wayPoints.clear();
        parent = nullptr;
        lastAction.reset();
        message.reset();
    }
    void setMessage(NString m) {
        if (m && (int)m->size() > MAX_MESSAGE_LENGTH) m = m->substr(0, MAX_MESSAGE_LENGTH);
        message = m;
    }
    void step(double step) {
        if (step > 0) {
            bounce.clear();
            position = position.add(velocity.mult(step));
        }
    }
    bool collide(const Chip& c2) const { return position.distance(c2.position) < radius + c2.radius - EPSILON && c2.parent != this && parent != &c2; }
    // the private applyForce(force)
    void applyForce(const Vec& force) { velocity = velocity.add(force.mult(1.0 / getMass())); }
    void applyForce(double time, const Vec& force) {
        applyForce(force);
        addWayPoint(time);
    }
    void eat(double time, Chip& c2) {
        idh.first(WAYPOINT);
        addWayPoint(time);
        rotation = Vec::between(position, c2.position).angle();
        position = position.mult(getMass()).add(c2.position.mult(c2.getMass())).mult(1.0 / (c2.getMass() + getMass()));
        velocity = velocity.mult(getMass()).add(c2.velocity.mult(c2.getMass())).mult(1.0 / (c2.getMass() + getMass()));
        setArea(getArea() + c2.getArea());
        wayPoints.push_back(WayPoint{time, position, radius, &c2, rotation});
        c2.setRadius(time, -1);
    }
    double getDeathTime() const {
        for (const WayPoint& p : wayPoints)
            if (p.radius < 0) return p.time;
        if (radius < 0) return 0;
        throw java::Exception{"java.lang.RuntimeException: Chip not dead"};
    }
    double contactPosition(const Chip& chip) const {
        if (chip.parent == this || parent == &chip) return INFINITY;
        Vec dv = velocity.sub(chip.velocity);
        Vec d = position.sub(chip.position);
        double a = dv.lengthSquared();
        double b = d.x * dv.x + d.y * dv.y;
        double dmin = radius + chip.radius;
        double c = d.lengthSquared() - dmin * dmin;
        double delta = b * b - a * c;
        if (delta <= 0 || a == 0) return INFINITY;
        double rd = std::sqrt(delta);
        return java::min((-b + rd) / a, (-b - rd) / a);
    }
    String toViewString() const;
    String toPlayerString() const;
};

void Chip::addWayPoint(double time) {
    parent = nullptr;
    bool force = false;
    if (!wayPoints.empty()) {
        const WayPoint& last = wayPoints.back();
        if (last.time == time) {
            wayPoints.pop_back();
        } else {
            force = last.radius != radius;
            if (last.radius == radius && last.position.distance(position) < 2.0) return;
        }
    }
    if (wayPoints.size() < 8 || force) {
        idh.first(WAYPOINT);
        wayPoints.push_back(WayPoint{time, position, radius, nullptr, rotation});
    }
}

// the identity hash code of a chip (made once)
void hashChip(Chip& c) {
    if (c.hashed) return;
    c.hashed = true;
    idh.next();
}
// HashSet<Chip>#contains: no hash code for a set that never had a table (JDK 26's HashMap#getNode)
bool bounceContains(Chip& set, Chip& c) {
    if (!set.bounceTable) return false;
    hashChip(c);
    for (const Chip* b : set.bounce)
        if (b == &c) return true;
    return false;
}
void bounceAdd(Chip& set, Chip& c) {
    hashChip(c);
    set.bounceTable = true;
    for (const Chip* b : set.bounce)
        if (b == &c) return;
    set.bounce.push_back(&c);
}

// Referee.Player
struct Player {
    int id = 0;
    int score = 0;
    double lostTime = -1;
    std::vector<Chip*> chips;

    bool isDead() const {
        for (const Chip* c : chips)
            if (c->radius >= 0) return false;
        return true;
    }
    double getTotalArea() const {
        double area = 0;
        for (const Chip* c : chips) area += c->getArea();
        return area;
    }
    // compareTo
    int compareTo(const Player& p2) const {
        double diff = getTotalArea() - p2.getTotalArea();
        if (diff == 0 && chips.empty() && p2.chips.empty()) diff = lostTime - p2.lostTime;
        return diff > 0 ? 1 : (diff < 0 ? -1 : 0);
    }
    void update() {
        for (size_t i = 0; i < chips.size();) {
            if (chips[i]->radius < 0) chips.erase(chips.begin() + (std::ptrdiff_t)i);
            else i++;
        }
    }
    void checkDeath(int round) {
        if (isDead() && lostTime < 0) {
            double maxTime = 0;
            for (const Chip* c : chips)
                if (c->radius < 0) maxTime = java::max(maxTime, c->getDeathTime());
            lostTime = round + maxTime;
        }
    }
    void setDead(int round) {
        for (Chip* c : chips) c->player = nullptr;
        chips.clear();
        lostTime = round;
    }
};

String Chip::toViewString() const {
    String buffer = S((i64)wayPoints.size());
    for (const WayPoint& p : wayPoints) buffer += u" " + p.toString();
    String s = S(id) + u" " + S(player ? player->id : -1) + u" " + F(radius, 2) + u" " + F(position.x + WIDTH / 2, 2) + u" " + F(position.y + HEIGHT / 2, 2) + u" " +
               F(rotation, 2) + u" " + buffer + u" " + (message ? *message : String());
    return java::trim(s);
}

String Chip::toPlayerString() const {
    return S(id) + u" " + S(player ? player->id : -1) + u" " + F(radius, 6) + u" " + F(position.x + WIDTH / 2, 6) + u" " + F(position.y + HEIGHT / 2, 6) + u" " +
           F(velocity.x, 6) + u" " + F(velocity.y, 6);
}

// One of the players' shots of a round (the anonymous Runnable: chips.add(chip.propulse(angle)))
struct Shot {
    Chip* chip;
    double angle;
};

struct Referee : legacy::MultiReferee {
    std::vector<std::unique_ptr<Chip>> allChips; // every Chip ever made (Java's objects)
    int CHIP_GLOBAL_ID = 0;
    std::vector<Chip*> chips, removedChips;
    std::vector<Player> players;
    bool playersCreated = false;
    i32 maxNeutralChipCount = 0, chipCountPerPlayer = 0;
    i64 seed = 0;
    double playerInitialRadius = 0;
    int playerCount = 0;
    bool symmetric = false;
    std::vector<Shot> shots;
    java::HashMapOrder playersActions; // HashSet<Runnable>: keys = indices of `shots`
    java::HashMapOrder tooltipSet; // the base's HashSet<Tooltip>: keys = a counter
    std::vector<i32> pendingTooltips; // the keys of the tooltips not printed yet, in the order the base added them
    i32 tooltipCounter = 0;
    long long physicsSteps = 0;
#if POKER_CHIP_RACE_HINTS
    std::map<int, std::vector<int>> orderHints; // PCR_ORDER: round -> chip ids in run order
    bool insertionOrder = false;
#endif

    Referee() { base = legacy::Base::pokerChipRace2014(); }

    Chip* newChip(Player* player, const Vec& position, const Vec& velocity, double radius) {
        allChips.push_back(std::make_unique<Chip>());
        Chip* c = allChips.back().get();
        c->id = CHIP_GLOBAL_ID++;
        c->velocity = velocity;
        c->position = position;
        c->radius = radius;
        c->player = player;
        return c;
    }

    // Chip#propulse(angle): returns the trash chip
    Chip* propulse(Chip& c, double angle) {
        c.rotation = angle;
        double area = c.getArea() * CHIP_EJECTION_RATIO;
        c.setArea(c.getArea() - area);
        Chip* trash = newChip(nullptr, Vec(0, 0), c.velocity, 0);
        trash->setArea(area);
        double radiusSum = c.radius - trash->radius;
        trash->position = Vec(c.position.x - java::cos(angle) * radiusSum, c.position.y - java::sin(angle) * radiusSum);
        Vec vector = Vec(java::cos(angle), java::sin(angle)).mult(CHIP_EJECTION_SPEED * trash->getMass());
        c.applyForce(vector);
        Vec inverse = vector.mult(-1);
        trash->applyForce(inverse);
        idh.first(WAYPOINT);
        trash->wayPoints.push_back(Chip::WayPoint{0, trash->position, trash->radius, &c, trash->rotation});
        trash->parent = &c;
        return trash;
    }

    String getGameName() override { return u"PokerChipRace"; }

    void populateMessages(std::map<std::string, String>& p) override {
        p["playerAction"] = u"$%s pushes chip %d toward %.2f %.2f (x y)"; // (formatted by handlePlayerOutput itself)
        p["playerDoNothing"] = u"$%d doesn't move chip %d";
        p["playerDeadStatus"] = u"$%d no longer plays in this game. (current score: %d)";
        p["playerAliveStatus"] = u"$%d score: %d";
        p["playerActionFail"] = u"$%s tried to move chip %d but gave a position too close to the chip";
        p["playerActionImpossible"] = u"$%s couldn't push chip %d because it's too small";
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"max_neutral_count", S(maxNeutralChipCount));
        prop.setProperty(u"chips_count_per_player", S(chipCountPerPlayer));
        prop.setProperty(u"seed", S(seed));
        prop.setProperty(u"player_initial_radius", java::ascii(java::doubleToString(playerInitialRadius)));
        prop.setProperty(u"symmetric", symmetric ? u"true" : u"false");
        return prop;
    }

    static i32 parseIntOrThrow(const String& s) {
        i32 v;
        if (!java::parseInt(s, v)) throw java::Exception{java::numberFormatException(java::encodeUtf8(s))};
        return v;
    }
    static i64 parseLongOrThrow(const String& s) {
        i64 v;
        if (!java::parseLong(s, v)) throw java::Exception{java::numberFormatException(java::encodeUtf8(s))};
        return v;
    }
    static double parseDoubleOrThrow(const String& s) {
        double v;
        if (!java::parseDouble(s, v)) throw java::Exception{java::numberFormatException(java::encodeUtf8(s))};
        return v;
    }

    void initReferee(int pc, java::Properties& prop) override {
#if POKER_CHIP_RACE_PROPS
        // validation only: more init properties ("key=value" lines: CodinGame's own configuration) to replay its games
        if (const char* extra = getenv("PCR_PROPS")) prop.load(java::decodeUtf8(std::string(extra)));
#endif
        int seedCall = SEED_CALL, k0 = K0;
        idh.init(seedCall, k0);
#if POKER_CHIP_RACE_HINTS
        // validation only: the jar's thread seed (os::random() call) and hashes before the first Runnable, the first-use
        // costs, an explicit order of the shots
        if (const char* v = getenv("PCR_IDH")) {
            int c = seedCall, k = k0;
            if (sscanf(v, "%d %d", &c, &k) >= 1) idh.init(c, k);
        }
        if (const char* v = getenv("PCR_IDH_COSTS")) {
            const char* p = v;
            for (int i = 0; i < NCOSTS; i++) {
                char* end;
                long x = strtol(p, &end, 10);
                if (end == p) break;
                idh.costs[i] = (int)x;
                p = end;
            }
        }
        idh.trace = getenv("PCR_IDH_TRACE") != nullptr;
        if (const char* v = getenv("PCR_ORDER")) {
            std::string s(v);
            size_t a = 0;
            while (a < s.size()) {
                size_t b = s.find(';', a);
                if (b == std::string::npos) b = s.size();
                std::string part = s.substr(a, b - a);
                size_t colon = part.find(':');
                if (colon != std::string::npos) {
                    int r = atoi(part.substr(0, colon).c_str());
                    std::vector<int>& o = orderHints[r];
                    std::string rest = part.substr(colon + 1);
                    size_t c = 0;
                    while (c < rest.size()) {
                        size_t d = rest.find(',', c);
                        if (d == std::string::npos) d = rest.size();
                        std::string tok = rest.substr(c, d - c);
                        o.push_back(tok == "None" ? -1 : atoi(tok.c_str()));
                        c = d + 1;
                    }
                }
                a = b + 1;
            }
        }
#endif
        // the whole method is in a try / catch (Exception e) { e.printStackTrace(); }: an exception leaves the fields
        // as they are
        try {
            // the default seed (new Random(System.currentTimeMillis()).nextLong()) is computed whether given or not
            String defaultSeed = S(java::Random((i64)java::entropy()).nextLong());
            seed = parseLongOrThrow(prop.getProperty(u"seed", defaultSeed));
            java::Random random(seed);
            playerCount = pc;
            // the defaults are drawn whether the properties are given or not
            maxNeutralChipCount = parseIntOrThrow(prop.getProperty(u"max_neutral_count", S(java::add(pc, random.nextIntChecked(MAX_DEFAULT_CHIP_COUNT - pc)))));
            chipCountPerPlayer = parseIntOrThrow(prop.getProperty(u"chips_count_per_player", S(random.nextInt(5) + 1)));
            double averageChipSize = (CHIP_MAX_START_RADIUS + CHIP_MIN_START_RADIUS) / 2;
            String defRadius = S(java::round(random.nextDouble() * (CHIP_MAX_START_RADIUS - averageChipSize) + averageChipSize));
            playerInitialRadius = parseDoubleOrThrow(prop.getProperty(u"player_initial_radius", defRadius));
            symmetric = java::parseBoolean(prop.getProperty(u"symmetric", pc >= 4 ? u"true" : u"false"));
            shots.clear();
            playersActions = java::HashMapOrder();

            int counter = 20;
            bool failed;
            while ((failed = !generateMap(random)) && counter-- > 0) {
            }
            if (failed) throw java::Exception{"java.lang.RuntimeException: Impossible to generate so much chips"};
        } catch (const java::Exception&) {
            // e.printStackTrace()
            initFailed = true;
        }
#if POKER_CHIP_RACE_HINTS
        if (getenv("PCR_INIT_ONLY")) { // validation only: the init's outcome, then stop
            fprintf(stdout, "INIT failed=%d chips=%d ids=%d players=%d per=%d neutral=%d radius=%s symmetric=%d\n", (int)initFailed, (int)chips.size(), CHIP_GLOBAL_ID, pc,
                    chipCountPerPlayer, maxNeutralChipCount, java::doubleToString(playerInitialRadius).c_str(), (int)symmetric);
            fflush(stdout);
            std::_Exit(0);
        }
#endif
    }
    bool initFailed = false;

    bool generateMap(java::Random& random) { return symmetric ? generateSymmetricMap(random) : generateRandomMap(random); }

    void newPlayers() {
        chips.clear();
        removedChips.clear();
        players.assign((size_t)playerCount, Player());
        for (int i = 0; i < playerCount; ++i) players[(size_t)i].id = i;
        playersCreated = true;
    }

    // a position drawn for a chip of this radius: x then y
    Vec drawPosition(java::Random& random, double radius) {
        double x = -WIDTH / 2 + radius + random.nextDouble() * (WIDTH - radius * 2);
        double y = -HEIGHT / 2 + radius + random.nextDouble() * (HEIGHT - radius * 2);
        return Vec(x, y);
    }

    void addPlayerChip(int p, const Vec& pos) {
        Chip* c = newChip(&players[(size_t)p], pos, Vec(0, 0), playerInitialRadius);
        chips.push_back(c);
        players[(size_t)p].chips.push_back(c);
    }

    bool generateSymmetricMap(java::Random& random) {
        newPlayers();
        Vec mapCenter(0, 0);
        if (playerCount % 2 == 1) {
            if (chipCountPerPlayer % 2 == 1) addPlayerChip(playerCount - 1, mapCenter);
            for (int j = 0; j < chipCountPerPlayer / 2; j++) {
                Vec pos;
                int counter = 50;
                do {
                    if (--counter < 0) return false;
                    pos = drawPosition(random, playerInitialRadius);
                } while ((std::fabs(pos.x - mapCenter.x) <= playerInitialRadius * 2) || (std::fabs(pos.y - mapCenter.y) <= playerInitialRadius * 2) ||
                         collide(pos, playerInitialRadius));
                addPlayerChip(playerCount - 1, pos);
                addPlayerChip(playerCount - 1, pos.symmetric(mapCenter));
            }
        }
        for (int j = 0; j < chipCountPerPlayer; ++j) {
            Vec pos;
            int counter = 50;
            do {
                if (--counter < 0) return false;
                pos = drawPosition(random, playerInitialRadius);
            } while ((std::fabs(pos.x - mapCenter.x) <= playerInitialRadius * 2) || (std::fabs(pos.y - mapCenter.y) <= playerInitialRadius * 2) ||
                     collide(pos, playerInitialRadius));
            addPlayerChip(0, pos);
            addPlayerChip(1, pos.symmetric(mapCenter));
            if (playerCount == 4) {
                addPlayerChip(2, pos.vsymmetric(mapCenter.y));
                addPlayerChip(3, pos.hsymmetric(mapCenter.x));
            }
        }
        int step = playerCount - playerCount % 2;
        for (int i = maxNeutralChipCount - 1; i >= 0; i -= step) {
            double radius = random.nextDouble() * (CHIP_MAX_START_RADIUS - CHIP_MIN_START_RADIUS) + CHIP_MIN_START_RADIUS;
            Vec pos;
            int counter = 50;
            bool placed = true;
            do {
                if (--counter < 0) {
                    placed = false;
                    break;
                }
                pos = drawPosition(random, radius);
            } while ((std::fabs(pos.x - mapCenter.x) <= radius * 2) || (std::fabs(pos.y - mapCenter.y) <= radius * 2) || collide(pos, radius));
            if (!placed) break; // break chipGeneration
            double speed = random.nextDouble() * 10;
            if (radius >= playerInitialRadius) speed = 0;
            double angle = random.nextDouble() * PI;
            Vec speedVector(java::cos(angle) * speed, java::sin(angle) * speed);
            chips.push_back(newChip(nullptr, pos, speedVector, radius));
            chips.push_back(newChip(nullptr, pos.symmetric(mapCenter), speedVector.mult(-1), radius));
            if (playerCount == 4) {
                chips.push_back(newChip(nullptr, pos.vsymmetric(mapCenter.y), speedVector.vsymmetric(), radius));
                chips.push_back(newChip(nullptr, pos.hsymmetric(mapCenter.x), speedVector.hsymmetric(), radius));
            }
        }
        return true;
    }

    bool generateRandomMap(java::Random& random) {
        newPlayers();
        for (int i = 0; i < playerCount; ++i) {
            for (int j = 0; j < chipCountPerPlayer; ++j) {
                Vec pos;
                int counter = 50;
                do {
                    if (--counter < 0) return false;
                    pos = drawPosition(random, playerInitialRadius);
                } while (collide(pos, playerInitialRadius));
                addPlayerChip(i, pos);
            }
        }
        for (int i = maxNeutralChipCount - 1; i >= 0; --i) {
            double radius = random.nextDouble() * (CHIP_MAX_START_RADIUS - CHIP_MIN_START_RADIUS) + CHIP_MIN_START_RADIUS;
            Vec pos;
            int counter = 50;
            bool placed = true;
            do {
                if (--counter < 0) {
                    placed = false;
                    break;
                }
                pos = drawPosition(random, radius);
            } while (collide(pos, radius));
            if (!placed) break; // break chipGeneration
            double speed = random.nextDouble() * 10;
            if (radius >= playerInitialRadius) speed = 0;
            double angle = random.nextDouble() * PI;
            Vec speedVector(java::cos(angle) * speed, java::sin(angle) * speed);
            chips.push_back(newChip(nullptr, pos, speedVector, radius));
        }
        return true;
    }

    bool collide(const Vec& pos, double radius) const {
        if (pos.x - radius < -WIDTH / 2) return true;
        if (pos.x + radius > WIDTH / 2) return true;
        if (pos.y - radius < -HEIGHT / 2) return true;
        if (pos.y + radius > HEIGHT / 2) return true;
        for (const Chip* c : chips) {
            double factor = 1.4;
            if (c->player != nullptr) factor = 2;
            double minDistance = (radius + c->radius) * factor;
            if (c->position.distance(pos) <= minDistance) return true;
        }
        return false;
    }

    // NullPointerException of a field the failed init left null (the referee thread dies)
    [[noreturn]] static void npe() { throw java::Exception{"java.lang.NullPointerException"}; }

    void prepare(int) override {
        if (!playersCreated) npe();
        for (Player& p : players) p.update();
        for (Chip* c : chips) c->reset();
    }

    // Referee.Colision and its two kinds
    struct Colision {
        double time;
        bool border;
        Chip* c1;
        Chip* c2;  // ChipColision
        int which; // BorderColision: TOP, LEFT, RIGHT, BOTTOM
    };
    enum { TOP, LEFT, RIGHT, BOTTOM };

    void react(const Colision& col) {
        if (col.border) {
            idh.first(BORDER_REACT);
            Chip& chip = *col.c1;
            Vec v = chip.velocity;
            switch (col.which) {
            case TOP: chip.setLinearVelocity(col.time, Vec(v.x, std::fabs(v.y))); break;
            case BOTTOM: chip.setLinearVelocity(col.time, Vec(v.x, -std::fabs(v.y))); break;
            case LEFT: chip.setLinearVelocity(col.time, Vec(std::fabs(v.x), v.y)); break;
            case RIGHT: chip.setLinearVelocity(col.time, Vec(-std::fabs(v.x), v.y)); break;
            }
            return;
        }
        Chip& c1 = *col.c1;
        Chip& c2 = *col.c2;
        double time = col.time;
        if (c1.radius == c2.radius) {
            Vec normal = c2.position.sub(c1.position).normalize();
            Vec relativeVelocity = c1.velocity.sub(c2.velocity);
            Vec impulse = normal.mult(-2 * normal.dot(relativeVelocity) / (1 / c1.getMass() + 1 / c2.getMass()));
            if (impulse.dot(normal) <= 0) {
                c1.applyForce(time, impulse);
                c2.applyForce(time, impulse.mult(-1));
            }
            double distance = c1.position.distance(c2.position);
            double diff = distance - c1.radius + c2.radius;
            if (diff < 0) {
                c1.setPosition(time, c1.position.add(normal.mult(-(diff / 2 + EPSILON))));
                c2.setPosition(time, c2.position.add(normal.mult((diff / 2 + EPSILON))));
            }
        } else {
            Chip& eater = c1.radius > c2.radius ? c1 : c2;
            Chip& eaten = c1.radius > c2.radius ? c2 : c1;
            eater.eat(time, eaten);
            for (size_t i = 0; i < chips.size(); i++)
                if (chips[i] == &eaten) {
                    chips.erase(chips.begin() + (std::ptrdiff_t)i);
                    break;
                }
            removedChips.push_back(&eaten);
        }
    }

    double step2(double beginTime) {
        double min = 1 - beginTime;
        std::vector<Colision> colisions;
        for (Chip* c : chips) {
            c->checkBounds(-WIDTH / 2, -HEIGHT / 2, WIDTH / 2, HEIGHT / 2);
            double verticalColision1 = ((HEIGHT / 2 - c->radius) - c->position.y) / c->velocity.y;
            double verticalColision2 = ((-HEIGHT / 2 + c->radius) - c->position.y) / c->velocity.y;
            if (verticalColision1 <= min && verticalColision1 > 0) addBorder(colisions, beginTime + verticalColision1, c, BOTTOM);
            if (verticalColision2 <= min && verticalColision2 > 0) addBorder(colisions, beginTime + verticalColision2, c, TOP);
            double horizontalColision1 = (WIDTH / 2 - c->radius - c->position.x) / c->velocity.x;
            double horizontalColision2 = (-WIDTH / 2 + c->radius - c->position.x) / c->velocity.x;
            if (horizontalColision1 <= min && horizontalColision1 > 0) addBorder(colisions, beginTime + horizontalColision1, c, RIGHT);
            if (horizontalColision2 <= min && horizontalColision2 > 0) addBorder(colisions, beginTime + horizontalColision2, c, LEFT);
        }
        for (int i = (int)chips.size() - 1; i >= 0; --i) {
            Chip* chip1 = chips[(size_t)i];
            if (chip1->radius >= 0) {
                for (int j = i - 1; j >= 0; --j) {
                    Chip* chip2 = chips[(size_t)j];
                    if (chip2->radius >= 0) {
                        if (chip2->collide(*chip1)) {
                            if (!bounceContains(*chip1, *chip2)) {
                                idh.first(CHIP_COLISION);
                                colisions.push_back(Colision{beginTime, false, chip1, chip2, 0});
                                bounceAdd(*chip1, *chip2);
                                bounceAdd(*chip2, *chip1);
                            }
                        } else {
                            double colision = chip1->contactPosition(*chip2);
                            if (colision <= min && colision > 0) {
                                idh.first(CHIP_COLISION);
                                colisions.push_back(Colision{beginTime + colision, false, chip1, chip2, 0});
                            }
                        }
                    }
                }
            }
        }
        if (!colisions.empty()) {
            // Collections.sort: stable, by Double.compare of the times
            java::sort(colisions, [](const Colision& a, const Colision& b) { return java::compareDouble(a.time, b.time); });
            double time = -1;
            for (const Colision& c : colisions) {
                if (time <= -1) {
                    time = c.time;
                    double step = time - beginTime;
                    for (Chip* ch : chips) ch->step(step);
                }
                if (c.time <= time + EPSILON) react(c);
                else break;
            }
            return time - beginTime;
        }
        double step = 1 - beginTime;
        for (Chip* ch : chips) ch->step(step);
        return step;
    }

    void addBorder(std::vector<Colision>& colisions, double time, Chip* c, int which) {
        idh.first(BORDER_COLISION);
        colisions.push_back(Colision{time, true, c, nullptr, which});
    }

    void updateGame(int round) override {
        std::vector<i32> order;
        playersActions.keys(order);
#if POKER_CHIP_RACE_HINTS
        auto h = orderHints.find(round);
        if (h != orderHints.end()) {
            // the listed chips first, in the hint's order, then the others in the set's order
            std::vector<i32> listed, rest;
            for (int id : h->second)
                for (i32 k : order)
                    if (shots[(size_t)k].chip->id == id && id >= 0 && std::find(listed.begin(), listed.end(), k) == listed.end()) listed.push_back(k);
            for (i32 k : order)
                if (std::find(listed.begin(), listed.end(), k) == listed.end()) rest.push_back(k);
            order = listed;
            order.insert(order.end(), rest.begin(), rest.end());
        }
        if (insertionOrder) { // the reflection check (jcheck/pcrcheck.cpp): the Java side runs a LinkedHashSet
            order.clear();
            for (i32 k = 0; k < (i32)shots.size(); k++) order.push_back(k);
        }
#endif
        if (idh.trace) {
            fprintf(stderr, "IDH RUNORDER %d", round - 1);
            for (i32 k : order) fprintf(stderr, " %d", shots[(size_t)k].chip->id);
            fprintf(stderr, "\n");
        }
        for (i32 k : order) chips.push_back(propulse(*shots[(size_t)k].chip, shots[(size_t)k].angle));
        shots.clear();
        playersActions.clear();

        double time = 0;
        physicsSteps = 0;
        while (time < 1) {
            if (++physicsSteps > MAX_PHYSICS_STEPS) throw java::Exception{"poker-chip-race: the physics loop does not end (the jar loops forever)"};
            double step = step2(time);
            time += step;
        }
        for (Player& p : players) p.checkDeath(round);
    }

    void establishRanking() {
        std::vector<Player*> ranking;
        for (Player& p : players) ranking.push_back(&p);
        java::sort(ranking, [](Player* a, Player* b) { return a->compareTo(*b); });
        int score = -1;
        double lastArea = -1;
        double lastLostTime = -1;
        for (Player* player : ranking) {
            if (player->isDead()) {
                if (player->lostTime > lastLostTime) {
                    lastLostTime = player->lostTime;
                    ++score;
                }
                player->score = score;
            } else {
                double totalArea = player->getTotalArea();
                if (totalArea > lastArea) {
                    ++score;
                    lastArea = totalArea;
                }
                player->score = java::add(java::d2i(totalArea + 0.5), score);
            }
        }
    }

    // the base's HashSet<Tooltip>: its iteration order, for the TOOLTIP command the base prints right after this view
    void orderTooltips() {
        if (pendingTooltips.empty()) return;
        std::vector<i32> keys;
        tooltipSet.keys(keys);
        if (keys.size() != tooltips.size()) return; // (never)
        std::vector<Tooltip> ordered;
        for (i32 k : keys) {
            size_t at = (size_t)(std::find(pendingTooltips.begin(), pendingTooltips.end(), k) - pendingTooltips.begin());
            ordered.push_back(tooltips[at]);
        }
        tooltips = ordered;
        pendingTooltips.clear();
        tooltipSet.clear();
    }
    void tooltipAdded() {
        idh.first(TOOLTIP);
        i32 h = idh.next();
        if (idh.trace) fprintf(stderr, "IDH T %lld\n", (long long)(idh.stream.count - 1));
        tooltipSet.put(tooltipCounter, h);
        pendingTooltips.push_back(tooltipCounter++);
    }

    Lines getInitDataForView() override { return {S(playerCount) + u" " + S(800) + u" " + S(515) + u" " + S(MAX_ROUND_COUNT)}; }

    Lines getFrameDataForView(int, int, int, bool) override {
        orderTooltips();
        if (!playersCreated) npe();
        establishRanking();
        Lines data;
        data.push_back(S((i64)(chips.size() + removedChips.size())));
        double totalArea = 0;
        std::vector<double> playersAreas((size_t)playerCount, 0.0);
        for (const Chip* chip : chips) {
            double area = chip->getArea();
            if (chip->player) playersAreas[(size_t)chip->player->id] += area;
            totalArea += area;
            data.push_back(chip->toViewString());
        }
        for (const Chip* chip : removedChips) data.push_back(chip->toViewString());
        removedChips.clear();
        String str;
        for (int i = 0; i < playerCount; ++i) str += F(playersAreas[(size_t)i], 6) + u" ";
        str += F(totalArea, 6);
        data.push_back(str);
        str.clear();
        for (int i = 0; i < playerCount; ++i) str += S(players[(size_t)i].score) + u" ";
        data.push_back(java::trim(str));
        return data;
    }

    int getExpectedOutputLineCountForPlayer(int playerId) override {
        if (!playersCreated) npe();
        return (int)players[(size_t)playerId].chips.size();
    }

    // PLAYER_INPUT_PATTERN, (?<action>(?:WAIT|(?<x>\S+) (?<y>\S+)))( +(?<message>.+))? with CASE_INSENSITIVE (ASCII
    // only), Matcher#matches: false when the line does not match
    static bool matchLine(const String& line, bool& isWait, String& x, String& y, NString& message) {
        size_t n = line.size();
        auto isS = [](char16_t c) { return !java::isRegexSpace(c); };
        // ( +(?<message>.+))? then the end: ' +' greedy, '.' any char but a line terminator
        auto tail = [&](size_t p, NString& msg) {
            if (p == n) {
                msg.reset();
                return true;
            }
            if (line[p] != u' ') return false;
            size_t k = p;
            while (k < n && line[k] == u' ') k++;
            for (size_t sp = k; sp > p; sp--) { // the spaces taken: k - p, then fewer
                if (sp >= n) continue;          // .+ needs one char
                bool ok = true;
                for (size_t i = sp; i < n; i++)
                    if (java::isLineSeparator(line[i])) ok = false;
                if (ok) {
                    msg = line.substr(sp);
                    return true;
                }
            }
            return false;
        };
        auto ci = [](char16_t c, char16_t lower) { return c == lower || c == (char16_t)(lower - 32); };
        if (n >= 4 && ci(line[0], u'w') && ci(line[1], u'a') && ci(line[2], u'i') && ci(line[3], u't') && tail(4, message)) {
            isWait = true;
            return true;
        }
        size_t a = 0;
        while (a < n && isS(line[a])) a++;
        if (a == 0 || a >= n || line[a] != u' ') return false;
        size_t b = a + 1;
        while (b < n && isS(line[b])) b++;
        if (b == a + 1) return false;
        if (!tail(b, message)) return false;
        isWait = false;
        x = line.substr(0, a);
        y = line.substr(a + 1, b - a - 1);
        return true;
    }

    void handlePlayerOutput(int, int round, int playerId, const Lines& output) override {
        if (!playersCreated) npe();
        Player& player = players[(size_t)playerId];
        if (player.chips.size() != output.size()) {
            player.setDead(round);
            invalidInput(S((i64)player.chips.size()) + u" lines", S((i64)output.size()) + u" lines");
        }
        size_t chipId = 0;
        for (const String& line : output) {
            Chip& chip = *player.chips[chipId++];
            bool isWait = false;
            String xs, ys;
            NString message;
            if (!matchLine(line, isWait, xs, ys, message)) {
                player.setDead(round);
                invalidInput(u"wait|x y", line);
            }
            chip.setMessage(message);
            if (isWait) {
                chip.lastAction = translate("playerDoNothing", {player.id, chip.id});
                continue;
            }
            if (chip.radius > CHIP_MIN_RADIUS) {
                double x, y;
                if (!java::parseDouble(xs, x) || !java::parseDouble(ys, y)) {
                    idh.first(BAD_NUMBER);
                    player.setDead(round);
                    invalidInput(u"wait|x y", line);
                }
                if (std::isnan(x) || std::isinf(x)) {
                    player.setDead(round);
                    invalidInput(u"x = An integer", u"x = NaN");
                }
                if (std::isnan(y) || std::isinf(y)) {
                    player.setDead(round);
                    invalidInput(u"y = An integer", u"y = NaN");
                }
                x -= WIDTH / 2;
                y -= HEIGHT / 2;
                double distance = std::sqrt((y - chip.position.y) * (y - chip.position.y) + (x - chip.position.x) * (x - chip.position.x));
                if (distance < 0.5) {
                    chip.lastAction = translate("playerActionFail", {S(player.id), chip.id});
                } else {
                    chip.lastAction = u"$" + S(player.id) + u" pushes chip " + S(chip.id) + u" toward " + F(x + WIDTH / 2, 2) + u" " + F(y + HEIGHT / 2, 2) + u" (x y)";
                    idh.first(ATAN2);
                    double angle = java::atan2(y - chip.position.y, x - chip.position.x);
                    idh.first(RUNNABLE);
                    i32 h = idh.next();
                    if (idh.trace) fprintf(stderr, "IDH R %d %d %lld\n", round, chip.id, (long long)(idh.stream.count - 1));
                    playersActions.put((i32)shots.size(), h);
                    shots.push_back(Shot{&chip, angle});
                }
            } else {
                chip.lastAction = translate("playerActionImpossible", {S(player.id), chip.id});
            }
        }
    }
    [[noreturn]] void invalidInput(const String& expected, const String& found) {
        idh.first(INVALID_INPUT);
        throw legacy::InvalidInputException(expected, found);
    }

    Lines getInitInputForPlayer(int player) override { return {S(player)}; }

    Lines getInputForPlayer(int, int playerId) override {
        if (!playersCreated) npe();
        const Player& player = players[(size_t)playerId];
        Lines str;
        str.push_back(S((i64)player.chips.size()));
        str.push_back(S((i64)chips.size()));
        for (const Chip* chip : player.chips) str.push_back(chip->toPlayerString());
        for (const Chip* chip : chips)
            if (std::find(player.chips.begin(), player.chips.end(), chip) == player.chips.end()) str.push_back(chip->toPlayerString());
        return str;
    }

    int getMaxRoundCount(int) override { return MAX_ROUND_COUNT; }
    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return MINIMUM_PLAYER_COUNT; }
    bool isPlayerDead(int player) override {
        if (!playersCreated) npe();
        return players[(size_t)player].isDead();
    }
    NString getDeathReason(int player) override {
        tooltipAdded(); // (the base adds the death tooltip right after)
        return u"$" + S(player) + u" got eaten";
    }
    int getScore(int player) override {
        if (!playersCreated) npe();
        return players[(size_t)player].score;
    }
    void setPlayerTimeout(int, int round, int player) override {
        if (!playersCreated) npe();
        players[(size_t)player].setDead(round);
        tooltipAdded(); // (the base adds the timeout tooltip right after)
    }
    Lines getGameSummary(int) override {
        Lines data;
        for (int i = 0; i < playerCount; ++i)
            data.push_back(translate(players[(size_t)i].isDead() ? "playerDeadStatus" : "playerAliveStatus", {i, players[(size_t)i].score}));
        return data;
    }
    Lines getPlayerActions(int player, int) override {
        Lines actions;
        for (const Chip* chip : players[(size_t)player].chips)
            if (chip->lastAction) actions.push_back(*chip->lastAction);
        return actions;
    }
    bool showTooltips() override { return true; }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
