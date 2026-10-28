// Mean Max (CodinGame, 2017-11, "mean-max") - headless C++ referee, port of the CodinGame referee published in
// https://github.com/CodinGame/MeanMax (commit ca1a77b, 2017-11-21, = HEAD "Fix missing round limit"; README: "The code
// used to play matches in the MeanMax contest"). The repository's history: 93dcc34 (2017-11-17, the referee as
// published after the contest), then three fixes for the multiplayer game: 8fb79dd and cb4abe0 (2017-11-20/21, the
// end rule of a lone survivor: isGameOver() first ended the game when another player had MORE points than the survivor,
// then compared the survivor with itself) and ca1a77b (the "expected" text per league, getMillisTimeForRound 50, the
// game summary commented out). This port follows HEAD, which CodinGame's own replays of today's game reproduce exactly
// (below). The file is the game's class only (`class Referee extends MultiReferee`, with its imports), without its base.
// 3 players (getMinimumPlayerCount() 3; the referee would run more, CodinGame's game has 3). Deterministic: reproduces
// the same game as the reference jar for a given -seed.
//
// The jar (referees/cg-builder/legacy: `legacy/build.sh ... cg-2017-mean-max bases/base-2017-mean-max.java`): the game's
// class compiled with CodinGame's old base of 2017 (the Coders of the Caribbean one from
// https://github.com/SpiritusSancti5/codinGame, 2017-04) with the two changes this game needs: its isGameOver() hook,
// called by nextRound() (instead of the alive player count test), and the OutputData#toString of CodinGame's SDK (its
// first commit, 2017-12-05), which counts the lines after trimming (this game's "" headline would otherwise
// desynchronise the protocol at the first frame). CodinGame's own base of 2017-11 is not public: see the base file's
// header for the evidence. CodinGame's replays (codingame.com's public gameResult/findByGameId, 2941 games of every
// league, 1.43M frames) are reproduced frame for frame (views, infos, summaries, tooltips, scores) by the jar's classes
// fed the recorded outputs, and by this port through its CLI (-seed only, bots printing the recorded lines) on the
// 1262 games of the top league's rules (Bronze to Legend). This port runs on common/legacy.h with
// legacy::Base::meanMax2017(). Time limits: 1000 ms for the first turn (the base's), 50 ms after (the game's).
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -p3 "<bot3 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): "seed=N") to stdout like
// the jar (the referee runs unchanged in the runner of the SDK 4.5.0). "null" scores when the game fails (the Java
// referee crashed or the protocol broke; the jar after 150 s). -l writes the trace of common/legacy.h (bot executions,
// view sizes, summaries, tooltips, END; CG_TRACE_VIEWS=1 adds the text of every view).
//
// League: the referee's `private static final int GAME_VERSION = 3` is the default branch of its league switch, the
// rules of Bronze and above (CodinGame's top leagues: 3 looters, the three skills); 0, 1 and 2 are the wood leagues
// (1 or 2 looters, no skills or only the destroyer's, wrecks spawned directly). The jar keeps the constant; the lower
// leagues are not implemented here.
//
// The game: a circular map of radius 6000 around Watertown (radius 3000); each player has 3 looters (reaper, destroyer,
// doof; mass 0.5 / 1.5 / 1.0, friction 0.2 / 0.3 / 0.25, radius 400) placed symmetrically (java.util.Random(seed):
// distance, angle; replaced while two looters touch); tankers (size 4-9, radius 400 + 50 size) come from beyond the map
// (500 spawns per player drawn at init, the same for every player, rotated by the player's third of the circle), fill
// up with water in Watertown and leave once full. Each round every player answers 3 lines, one per looter:
// `X Y POWER` (thrust towards the point, power 0-300), `SKILL X Y` (rage 30 / 60 / 30, range 2000: the reaper's tar
// pool (+10 mass, 3 rounds), the destroyer's grenade (a push of 1000, 1 round), the doof's oil (no friction, no harvest,
// 3 rounds)) or `WAIT`, each with an optional message. The round: skill effects apply, tankers and looters thrust, then
// the units move with exact collision times (map border, unit pairs: impulses with a minimum of 30, overlaps fixed; a
// destroyer hitting a tanker that is not in a tar pool destroys it into a wreck of its water), tankers fill or leave,
// dead tankers respawn, reapers inside wrecks harvest (1 point per water, not in oil), positions and speeds are rounded
// (friction applied), the doof's speed / 100 becomes rage (max 300). An invalid line or a timeout eliminates the player
// (its units stay on the map: its reaper can still harvest); the game ends when a player reaches 50 points, when a lone
// survivor has more points than every other player, when everyone is out, or after 200 rounds. The score is the points.
//
// The Java referee is followed closely, notably: the double precision physics in the same operation order (FP_CONTRACT
// off; Math.sqrt / floor exact; Math.sin / Math.cos of the spawns as HotSpot's x86_64 intrinsics compute them:
// java::sin / java::cos), its two roundings (Math.round for the view, the referee's own round() for the state and the
// inputs: halves away from zero, (int) of a long), the static id counter shared by units, wrecks and skill effects, the
// list orders (units, tankers, dead tankers, wrecks; the skill effects' TreeSet by (order, id)), the three regexes
// (hand matched: \s ASCII, the message after a whitespace run or the last character of a whitespace-only tail, SKILL /
// WAIT case insensitive in ASCII, numbers of 1-9 digits; a move whose power group is missing throws a
// NumberFormatException: an invalid input too), the lines of a player before its invalid line still applying that
// round (thrusts, skills: rage spent, effect cast), messages cut to 17 UTF-16 chars + "..." beyond 19, a dead player's
// units harvesting and filling its rage, the view's frame data (collision times with String.format("%.5f"): JDK 21+
// rounding) built in full, since a round with many collisions can exceed the runner's 30000-byte limit. The game's
// collections: the units, wrecks and players are lists; the skill effects a TreeSet; the HashSets of dead wrecks (used
// only by the wood leagues) and of finished effects (only removed from the TreeSet) never show their order; the base's
// tooltips (a HashSet of identity hash codes: several per frame when players are eliminated together) only count by
// their size. No identity hash order reaches the game.
// Not reproduced / not modelled: the wall clock (see engine.h). A referee looping forever in its collision loop (never
// seen; the jar would hang until the runner gives up after 150 s): cut after 10^6 collisions in a round, the game fails
// like the jar's ("null" scores; such a round's view would be far beyond the runner's limit anyway). The jar's Math.sin /
// Math.cos are those of x86_64 HotSpot: a jar run on another architecture may compute other last bits (which reach the
// game only through rounded positions and speeds).
#include "common/legacy.h"

#include <algorithm>
#include <memory>

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// --- constants (GAME_VERSION 3: the default branch of the league switch) ---
const int GAME_VERSION = 3;
static_assert(GAME_VERSION >= 3, "the wood leagues (GAME_VERSION 0-2) are not implemented");
const int LOOTER_COUNT = 3;
const bool REAPER_SKILL_ACTIVE = true;
const bool DESTROYER_SKILL_ACTIVE = true;
const bool DOOF_SKILL_ACTIVE = true;
const char16_t* EXPECTED = u"<x> <y> <power> | SKILL <x> <y> | WAIT";

const double MAP_RADIUS = 6000.0;
const int TANKERS_BY_PLAYER_MIN = 1;
const int TANKERS_BY_PLAYER_MAX = 3;

const double WATERTOWN_RADIUS = 3000.0;

const int TANKER_THRUST = 500;
const double TANKER_EMPTY_MASS = 2.5;
const double TANKER_MASS_BY_WATER = 0.5;
const double TANKER_FRICTION = 0.40;
const double TANKER_RADIUS_BASE = 400.0;
const double TANKER_RADIUS_BY_SIZE = 50.0;
const int TANKER_EMPTY_WATER = 1;
const int TANKER_MIN_SIZE = 4;
const int TANKER_MAX_SIZE = 10;
const double TANKER_MIN_RADIUS = TANKER_RADIUS_BASE + TANKER_RADIUS_BY_SIZE * TANKER_MIN_SIZE;
const double TANKER_SPAWN_RADIUS = 8000.0;
const int TANKER_START_THRUST = 2000;

const int MAX_THRUST = 300;
const int MAX_RAGE = 300;
const int WIN_SCORE = 50;

const double REAPER_MASS = 0.5;
const double REAPER_FRICTION = 0.20;
const int REAPER_SKILL_DURATION = 3;
const int REAPER_SKILL_COST = 30;
const int REAPER_SKILL_ORDER = 0;
const double REAPER_SKILL_RANGE = 2000.0;
const double REAPER_SKILL_RADIUS = 1000.0;
const double REAPER_SKILL_MASS_BONUS = 10.0;

const double DESTROYER_MASS = 1.5;
const double DESTROYER_FRICTION = 0.30;
const int DESTROYER_SKILL_DURATION = 1;
const int DESTROYER_SKILL_COST = 60;
const int DESTROYER_SKILL_ORDER = 2;
const double DESTROYER_SKILL_RANGE = 2000.0;
const double DESTROYER_SKILL_RADIUS = 1000.0;
const int DESTROYER_NITRO_GRENADE_POWER = 1000;

const double DOOF_MASS = 1.0;
const double DOOF_FRICTION = 0.25;
const double DOOF_RAGE_COEF = 1.0 / 100.0;
const int DOOF_SKILL_DURATION = 3;
const int DOOF_SKILL_COST = 30;
const int DOOF_SKILL_ORDER = 1;
const double DOOF_SKILL_RANGE = 2000.0;
const double DOOF_SKILL_RADIUS = 1000.0;

const double LOOTER_RADIUS = 400.0;
const int LOOTER_REAPER = 0;
const int LOOTER_DESTROYER = 1;
const int LOOTER_DOOF = 2;

const int TYPE_TANKER = 3;
const int TYPE_WRECK = 4;
const int TYPE_REAPER_SKILL_EFFECT = 5;
const int TYPE_DOOF_SKILL_EFFECT = 6;
const int TYPE_DESTROYER_SKILL_EFFECT = 7;

const double EPSILON = 0.00001;
const double MIN_IMPULSE = 30.0;
const double IMPULSE_COEFF = 0.5;

const double PI = 3.141592653589793; // Math.PI

// Global first free id for all elements on the map (a static of the Java referee: 0 in the jar's fresh JVM)
int GLOBAL_ID = 0;

String S(i64 v) { return java::str(v); }
String D(double v) { return java::ascii(java::doubleToString(v)); } // String.valueOf(double)

// join(Object... args): the values' String.valueOf joined with spaces
String join(std::initializer_list<String> parts) {
    String r;
    bool first = true;
    for (const String& p : parts) {
        if (!first) r += u' ';
        r += p;
        first = false;
    }
    return r;
}

// Referee.round(double): halves away from zero, (int) of Math.round's long
int refRound(double x) {
    int s = x < 0 ? -1 : 1;
    return s * java::l2i(java::round(s * x));
}

struct Point {
    double x;
    double y;

    Point(double x, double y) : x(x), y(y) {}
    virtual ~Point() {}

    double distance(const Point& p) const { return std::sqrt((this->x - p.x) * (this->x - p.x) + (this->y - p.y) * (this->y - p.y)); }

    // Move the point to x and y
    void move(double x, double y) {
        this->x = x;
        this->y = y;
    }

    // Move the point to an other point for a given distance
    void moveTo(const Point& p, double distance) {
        double d = this->distance(p);
        if (d < EPSILON) return;
        double dx = p.x - x;
        double dy = p.y - y;
        double coef = distance / d;
        this->x += dx * coef;
        this->y += dy * coef;
    }

    bool isInRange(const Point& p, double range) const { return &p != this && distance(p) <= range; }
};

const Point WATERTOWN(0, 0);

struct Player;
struct Unit;
struct SkillEffect;

// the skill effects: a TreeSet ordered by (order, id)
struct EffectOrder {
    bool operator()(const SkillEffect* a, const SkillEffect* b) const;
};
typedef std::vector<SkillEffect*> EffectSet; // kept sorted by EffectOrder (ids are unique: no ties)

struct Tanker;

struct Collision {
    double t;
    Unit* a = nullptr;
    Unit* b = nullptr;
    Collision(double t, Unit* a = nullptr, Unit* b = nullptr) : t(t), a(a), b(b) {}
    Tanker* dead() const;
};

// The null collision
const Collision NULL_COLLISION(1.0 + EPSILON);

struct Wreck : Point {
    int id;
    double radius;
    int water;
    bool known = false;
    Player* player = nullptr;

    Wreck(double x, double y, int water, double radius) : Point(x, y), id(GLOBAL_ID++), radius(radius), water(water) {}

    String getFrameId() const { return S(id) + u"@" + S(water); }

    String toFrameData() {
        if (known) return getFrameId();
        known = true;
        return join({getFrameId(), S(java::round(x)), S(java::round(y)), u"0", u"0", S(TYPE_WRECK), D(radius)});
    }

    // Reaper harvesting
    bool harvest(const std::vector<Player*>& players, const EffectSet& skillEffects);
};

struct Unit : Point {
    int type;
    int id;
    double vx = 0.0;
    double vy = 0.0;
    double radius = 0;
    double mass = 0;
    double friction = 0;
    bool known = false;

    Unit(int type, double x, double y) : Point(x, y), type(type), id(GLOBAL_ID++) {}

    void move(double t) {
        x += vx * t;
        y += vy * t;
    }
    using Point::move;

    double speed() const { return std::sqrt(vx * vx + vy * vy); }

    virtual String getFrameId() const { return S(id); }

    virtual String toFrameData() {
        if (known) return join({getFrameId(), S(java::round(x)), S(java::round(y)), S(java::round(vx)), S(java::round(vy))});
        known = true;
        return join({getFrameId(), S(java::round(x)), S(java::round(y)), S(java::round(vx)), S(java::round(vy)), S(type), S(java::round(radius))});
    }

    void thrust(const Point& p, int power) {
        double distance = this->distance(p);
        // Avoid a division by zero
        if (std::fabs(distance) <= EPSILON) return;
        double coef = (((double)power) / mass) / distance;
        vx += (p.x - this->x) * coef;
        vy += (p.y - this->y) * coef;
    }

    bool isInDoofSkill(const EffectSet& skillEffects) const;

    void adjust(const EffectSet& skillEffects) {
        x = refRound(x);
        y = refRound(y);
        if (isInDoofSkill(skillEffects)) {
            // No friction if we are in a doof skill effect
            vx = refRound(vx);
            vy = refRound(vy);
        } else {
            vx = refRound(vx * (1.0 - friction));
            vy = refRound(vy * (1.0 - friction));
        }
    }

    // Search the next collision with the map border
    virtual Collision getCollision() {
        // Check instant collision
        if (distance(WATERTOWN) + radius >= MAP_RADIUS) return Collision(0.0, this);
        // We are not moving, we can't reach the map border
        if (vx == 0.0 && vy == 0.0) return NULL_COLLISION;
        double a = vx * vx + vy * vy;
        if (a <= 0.0) return NULL_COLLISION;
        double b = 2.0 * (x * vx + y * vy);
        double c = x * x + y * y - (MAP_RADIUS - radius) * (MAP_RADIUS - radius);
        double delta = b * b - 4.0 * a * c;
        if (delta <= 0.0) return NULL_COLLISION;
        double t = (-b + std::sqrt(delta)) / (2.0 * a);
        if (t <= 0.0) return NULL_COLLISION;
        return Collision(t, this);
    }

    // Search the next collision with an other unit
    Collision getCollision(Unit* u) {
        // Check instant collision
        if (distance(*u) <= radius + u->radius) return Collision(0.0, this, u);
        // Both units are motionless
        if (vx == 0.0 && vy == 0.0 && u->vx == 0.0 && u->vy == 0.0) return NULL_COLLISION;
        // Change referencial
        double x2 = x - u->x;
        double y2 = y - u->y;
        double r2 = radius + u->radius;
        double vx2 = vx - u->vx;
        double vy2 = vy - u->vy;
        double a = vx2 * vx2 + vy2 * vy2;
        if (a <= 0.0) return NULL_COLLISION;
        double b = 2.0 * (x2 * vx2 + y2 * vy2);
        double c = x2 * x2 + y2 * y2 - r2 * r2;
        double delta = b * b - 4.0 * a * c;
        if (delta < 0.0) return NULL_COLLISION;
        double t = (-b - std::sqrt(delta)) / (2.0 * a);
        if (t <= 0.0) return NULL_COLLISION;
        return Collision(t, this, u);
    }

    // Bounce between 2 units
    void bounce(Unit* u) {
        double mcoeff = (mass + u->mass) / (mass * u->mass);
        double nx = x - u->x;
        double ny = y - u->y;
        double nxnysquare = nx * nx + ny * ny;
        double dvx = vx - u->vx;
        double dvy = vy - u->vy;
        double product = (nx * dvx + ny * dvy) / (nxnysquare * mcoeff);
        double fx = nx * product;
        double fy = ny * product;
        double m1c = 1.0 / mass;
        double m2c = 1.0 / u->mass;

        vx -= fx * m1c;
        vy -= fy * m1c;
        u->vx += fx * m2c;
        u->vy += fy * m2c;

        fx = fx * IMPULSE_COEFF;
        fy = fy * IMPULSE_COEFF;

        // Normalize vector at min or max impulse
        double impulse = std::sqrt(fx * fx + fy * fy);
        double coeff = 1.0;
        if (impulse > EPSILON && impulse < MIN_IMPULSE) coeff = MIN_IMPULSE / impulse;

        fx = fx * coeff;
        fy = fy * coeff;

        vx -= fx * m1c;
        vy -= fy * m1c;
        u->vx += fx * m2c;
        u->vy += fy * m2c;

        double diff = (distance(*u) - radius - u->radius) / 2.0;
        if (diff <= 0.0) {
            // Unit overlapping. Fix positions.
            moveTo(*u, diff - EPSILON);
            u->moveTo(*this, diff - EPSILON);
        }
    }

    // Bounce with the map border
    void bounce() {
        double mcoeff = 1.0 / mass;
        double nxnysquare = x * x + y * y;
        double product = (x * vx + y * vy) / (nxnysquare * mcoeff);
        double fx = x * product;
        double fy = y * product;

        vx -= fx * mcoeff;
        vy -= fy * mcoeff;

        fx = fx * IMPULSE_COEFF;
        fy = fy * IMPULSE_COEFF;

        // Normalize vector at min or max impulse
        double impulse = std::sqrt(fx * fx + fy * fy);
        double coeff = 1.0;
        if (impulse > EPSILON && impulse < MIN_IMPULSE) coeff = MIN_IMPULSE / impulse;

        fx = fx * coeff;
        fy = fy * coeff;
        vx -= fx * mcoeff;
        vy -= fy * mcoeff;

        double diff = distance(WATERTOWN) + radius - MAP_RADIUS;
        if (diff >= 0.0) {
            // Unit still outside of the map, reposition it
            moveTo(WATERTOWN, diff + EPSILON);
        }
    }

    virtual int getExtraInput() const { return -1; }
    virtual int getExtraInput2() const { return -1; }
    virtual int getPlayerIndex() const { return -1; }
};

struct Tanker : Unit {
    int water;
    int size;
    Player* player;
    bool killed = false;

    Tanker(int size, Player* player) : Unit(TYPE_TANKER, 0.0, 0.0), size(size), player(player) {
        water = TANKER_EMPTY_WATER;
        mass = TANKER_EMPTY_MASS + TANKER_MASS_BY_WATER * water;
        friction = TANKER_FRICTION;
        radius = TANKER_RADIUS_BASE + TANKER_RADIUS_BY_SIZE * size;
    }

    String getFrameId() const override { return S(id) + u"@" + S(water); }

    // a new Wreck (owned by the caller), or nullptr: no wreck if our center is outside of the map
    Wreck* die() {
        if (distance(WATERTOWN) >= MAP_RADIUS) return nullptr;
        return new Wreck(refRound(x), refRound(y), water, radius);
    }

    bool isFull() const { return water >= size; }

    void play() {
        if (isFull()) {
            // Try to leave the map
            thrust(WATERTOWN, -TANKER_THRUST);
        } else if (distance(WATERTOWN) > WATERTOWN_RADIUS) {
            // Try to reach watertown
            thrust(WATERTOWN, TANKER_THRUST);
        }
    }

    // Tankers can go outside of the map
    Collision getCollision() override { return NULL_COLLISION; }
    using Unit::getCollision;

    int getExtraInput() const override { return water; }
    int getExtraInput2() const override { return size; }
};

enum class Action { NONE, SKILL, MOVE, WAIT };

struct Looter : Unit {
    int skillCost = 0;
    double skillRange = 0;
    bool skillActive = false;

    Player* player;

    std::optional<Point> wantedThrustTarget;
    int wantedThrustPower = 0;

    NString message;
    Action attempt = Action::NONE;

    Looter(int type, Player* player, double x, double y) : Unit(type, x, y), player(player) { radius = LOOTER_RADIUS; }

    String toFrameData() override;
    int getPlayerIndex() const override;

    virtual SkillEffect* skillImpl(const Point& p) = 0;

    void setWantedThrust(const Point& target, int power) {
        if (power < 0) power = 0;
        wantedThrustTarget = target;
        wantedThrustPower = std::min(power, MAX_THRUST);
    }

    void reset() {
        message.reset();
        attempt = Action::NONE;
        wantedThrustTarget.reset();
    }
};

struct Player {
    int score = 0;
    int index;
    int rage = 0;
    Looter* looters[LOOTER_COUNT] = {};
    bool dead = false;
    struct TankerSpawn {
        int size;
        double angle;
    };
    std::vector<TankerSpawn> tankers; // a LinkedList used as a queue: remove() from the head
    size_t tankersHead = 0;

    explicit Player(int index) : index(index) {}

    void kill() { dead = true; }

    Looter* getReaper() const { return looters[LOOTER_REAPER]; }
    Looter* getDoof() const { return looters[LOOTER_DOOF]; }
};

String Looter::toFrameData() {
    if (known) return Unit::toFrameData();
    return join({Unit::toFrameData(), S(player->index)});
}
int Looter::getPlayerIndex() const { return player->index; }

struct SkillEffect : Point {
    int id;
    int type;
    double radius;
    int duration;
    int order;
    bool known = false;
    Looter* looter;

    SkillEffect(int type, double x, double y, double radius, int duration, int order, Looter* looter)
        : Point(x, y), id(GLOBAL_ID++), type(type), radius(radius), duration(duration), order(order), looter(looter) {}

    void apply(const std::vector<Unit*>& units) {
        duration -= 1;
        std::vector<Unit*> in;
        for (Unit* u : units)
            if (isInRange(*u, radius + u->radius)) in.push_back(u);
        applyImpl(in);
    }

    String toFrameData() {
        if (known) return S(id);
        known = true;
        return join({S(id), S(java::round(x)), S(java::round(y)), S(looter->id), u"0", S(type), S(java::round(radius))});
    }

    void applyImpl(const std::vector<Unit*>& units) {
        if (type == TYPE_REAPER_SKILL_EFFECT) {
            // Increase mass
            for (Unit* u : units) u->mass += REAPER_SKILL_MASS_BONUS;
        } else if (type == TYPE_DESTROYER_SKILL_EFFECT) {
            // Push units
            for (Unit* u : units) u->thrust(*this, -DESTROYER_NITRO_GRENADE_POWER);
        }
        // DoofSkillEffect: nothing to do now
    }
};

bool EffectOrder::operator()(const SkillEffect* a, const SkillEffect* b) const {
    // (a.order - b.order, then a.id - b.id): small ints, no overflow
    if (a->order != b->order) return a->order < b->order;
    return a->id < b->id;
}

void addEffect(EffectSet& set, SkillEffect* e) { set.insert(std::upper_bound(set.begin(), set.end(), e, EffectOrder()), e); }

bool Unit::isInDoofSkill(const EffectSet& skillEffects) const {
    for (SkillEffect* s : skillEffects)
        if (s->type == TYPE_DOOF_SKILL_EFFECT && isInRange(*s, s->radius + radius)) return true;
    return false;
}

bool Wreck::harvest(const std::vector<Player*>& players, const EffectSet& skillEffects) {
    for (Player* p : players) {
        if (isInRange(*p->getReaper(), radius) && !p->getReaper()->isInDoofSkill(skillEffects)) {
            p->score += 1;
            water -= 1;
        }
    }
    return water > 0;
}

struct Reaper : Looter {
    Reaper(Player* player, double x, double y) : Looter(LOOTER_REAPER, player, x, y) {
        mass = REAPER_MASS;
        friction = REAPER_FRICTION;
        skillCost = REAPER_SKILL_COST;
        skillRange = REAPER_SKILL_RANGE;
        skillActive = REAPER_SKILL_ACTIVE;
    }
    SkillEffect* skillImpl(const Point& p) override {
        return new SkillEffect(TYPE_REAPER_SKILL_EFFECT, p.x, p.y, REAPER_SKILL_RADIUS, REAPER_SKILL_DURATION, REAPER_SKILL_ORDER, this);
    }
};

struct Destroyer : Looter {
    Destroyer(Player* player, double x, double y) : Looter(LOOTER_DESTROYER, player, x, y) {
        mass = DESTROYER_MASS;
        friction = DESTROYER_FRICTION;
        skillCost = DESTROYER_SKILL_COST;
        skillRange = DESTROYER_SKILL_RANGE;
        skillActive = DESTROYER_SKILL_ACTIVE;
    }
    SkillEffect* skillImpl(const Point& p) override {
        return new SkillEffect(TYPE_DESTROYER_SKILL_EFFECT, p.x, p.y, DESTROYER_SKILL_RADIUS, DESTROYER_SKILL_DURATION, DESTROYER_SKILL_ORDER, this);
    }
};

struct Doof : Looter {
    Doof(Player* player, double x, double y) : Looter(LOOTER_DOOF, player, x, y) {
        mass = DOOF_MASS;
        friction = DOOF_FRICTION;
        skillCost = DOOF_SKILL_COST;
        skillRange = DOOF_SKILL_RANGE;
        skillActive = DOOF_SKILL_ACTIVE;
    }
    SkillEffect* skillImpl(const Point& p) override {
        return new SkillEffect(TYPE_DOOF_SKILL_EFFECT, p.x, p.y, DOOF_SKILL_RADIUS, DOOF_SKILL_DURATION, DOOF_SKILL_ORDER, this);
    }
    // With flame effects! Yeah!
    int sing() const { return java::d2i(std::floor(speed() * DOOF_RAGE_COEF)); }
};

// (a Looter is a Destroyer / a Unit is a Tanker: their types)
bool isDestroyer(const Unit* u) { return u->type == LOOTER_DESTROYER; }
bool isTanker(const Unit* u) { return u->type == TYPE_TANKER; }

Tanker* Collision::dead() const {
    if (a && b && isDestroyer(a) && isTanker(b) && b->mass < REAPER_SKILL_MASS_BONUS) return static_cast<Tanker*>(b);
    if (a && b && isDestroyer(b) && isTanker(a) && a->mass < REAPER_SKILL_MASS_BONUS) return static_cast<Tanker*>(a);
    return nullptr;
}

// --- the referee's regexes (Matcher#matches), hand matched. The lines never hold a line terminator (the base's
// Scanner split them there), so '.' takes any character. \s and the digits are ASCII; SKILL and WAIT are
// CASE_INSENSITIVE without UNICODE_CASE (ASCII folding only).

// \s+ at i: the whole whitespace run (a shorter one never helps: what follows starts with a non-space)
bool matchSpaces(const String& line, size_t& i) {
    size_t a = i;
    while (i < line.size() && java::isRegexSpace(line[i])) i++;
    return i > a;
}

// -?[0-9]{1,9} at i, followed by whitespace or the end: the whole digit run (10 digits or more never match)
bool matchNumber(const String& line, size_t& i, String& group) {
    size_t n = line.size(), a = i;
    if (i < n && line[i] == u'-') i++;
    size_t d = i;
    while (i < n && java::isRegexDigit(line[i])) i++;
    if (i - d < 1 || i - d > 9) return false;
    group = line.substr(a, i - a);
    return true;
}

// (?:\s+(?<message>.+))? then the end, from i: nothing left (no message), or whitespace then at least one character:
// the message is what follows the whitespace run (greedy \s+), or the last character when only whitespace is left
// (\s+ gives one back)
bool matchMessage(const String& line, size_t i, NString& message) {
    size_t n = line.size();
    if (i == n) {
        message.reset();
        return true;
    }
    if (!java::isRegexSpace(line[i])) return false;
    size_t j = i;
    while (j < n && java::isRegexSpace(line[j])) j++;
    if (j == n) {
        if (n - i < 2) return false;
        j = n - 1;
    }
    message = line.substr(j);
    return true;
}

bool matchWord(const String& line, const char* word, size_t& i) {
    size_t w = strlen(word);
    if (line.size() < w) return false;
    for (size_t k = 0; k < w; k++)
        if (!java::regexCharMatchesIgnoreCase(line[k], word[k])) return false;
    i = w;
    return true;
}

// WAIT(?:\s+(?<message>.+))?   (CASE_INSENSITIVE)
bool matchWait(const String& line, NString& message) {
    size_t i;
    return matchWord(line, "WAIT", i) && matchMessage(line, i, message);
}

// (?<x>-?[0-9]{1,9})\s+(?<y>-?[0-9]{1,9})\s+(?<power>([0-9]{1,9}))?(?:\s+(?<message>.+))?
// 1: a match with a power; 0: no match, or a match without the power group (the referee's Integer.valueOf(null)
// throws a NumberFormatException, caught: the same InvalidInputException). A match with a power always takes the
// whole whitespace run after y and the whole digit run after it (the regex tries those first; any other split leaves
// the power group empty).
int matchMove(const String& line, String& gx, String& gy, String& gpower, NString& message) {
    size_t i = 0;
    if (!matchNumber(line, i, gx) || !matchSpaces(line, i) || !matchNumber(line, i, gy) || !matchSpaces(line, i)) return 0;
    size_t a = i;
    while (i < line.size() && java::isRegexDigit(line[i])) i++;
    if (i == a || i - a > 9) return 0;
    gpower = line.substr(a, i - a);
    return matchMessage(line, i, message) ? 1 : 0;
}

// SKILL\s+(?<x>-?[0-9]{1,9})\s+(?<y>-?[0-9]{1,9})(?:\s+(?<message>.+))?   (CASE_INSENSITIVE)
bool matchSkill(const String& line, String& gx, String& gy, NString& message) {
    size_t i;
    if (!matchWord(line, "SKILL", i) || !matchSpaces(line, i) || !matchNumber(line, i, gx) || !matchSpaces(line, i) || !matchNumber(line, i, gy))
        return false;
    return matchMessage(line, i, message);
}

i32 parseInt(const String& s) {
    i32 v = 0;
    java::parseInt(s, v); // (1-9 digits: always an int)
    return v;
}

// the cut of matchMessage: more than 19 UTF-16 chars -> the first 17 + "..."
void matchMessage(Looter* looter, const NString& message) {
    looter->message = message;
    if (looter->message && looter->message->size() > 19) looter->message = looter->message->substr(0, 17) + u"...";
}

struct Referee : legacy::MultiReferee {
    i64 seed = 0;
    int playerCount = 0;
    std::vector<Unit*> units;
    std::vector<Looter*> looters;
    std::vector<Tanker*> tankers;
    std::vector<Tanker*> deadTankers;
    std::vector<Wreck*> wrecks;
    std::vector<Player*> players;
    Lines frameData;
    EffectSet skillEffects;
    // owners (the Java objects live as long as something references them: here until the end)
    std::vector<std::unique_ptr<Point>> owned;
    std::vector<std::unique_ptr<Player>> ownedPlayers;

    Referee() { base = legacy::Base::meanMax2017(); }

    template <class T>
    T* own(T* p) {
        owned.emplace_back(p);
        return p;
    }

    void spawnTanker(Player* player) {
        // player.tankers.remove(): NoSuchElementException once the 500 spawns are used (never: a few per round)
        if (player->tankersHead >= player->tankers.size()) throw java::Exception{"java.util.NoSuchElementException"};
        Player::TankerSpawn spawn = player->tankers[player->tankersHead++];

        double angle = (player->index + spawn.angle) * PI * 2.0 / ((double)playerCount);

        double cos = java::cos(angle);
        double sin = java::sin(angle);

        // (SPAWN_WRECK: the wood leagues only)
        Tanker* tanker = own(new Tanker(spawn.size, player));

        double distance = TANKER_SPAWN_RADIUS + tanker->radius;

        bool safe = false;
        while (!safe) {
            tanker->move(cos * distance, sin * distance);
            safe = true;
            for (Unit* u : units)
                if (!(tanker->distance(*u) > tanker->radius + u->radius)) {
                    safe = false;
                    break;
                }
            distance += TANKER_MIN_RADIUS;
        }

        tanker->thrust(WATERTOWN, TANKER_START_THRUST);

        units.push_back(tanker);
        tankers.push_back(tanker);
    }

    Looter* createLooter(int type, Player* player, double x, double y) {
        if (type == LOOTER_REAPER) return own(new Reaper(player, x, y));
        if (type == LOOTER_DESTROYER) return own(new Destroyer(player, x, y));
        return own(new Doof(player, x, y));
    }

    void newFrame(double t) { frameData.push_back(u"#" + java::ascii(java::formatFixed(t, 5))); }
    void addDeadToFrame(Unit* u) { frameData.push_back(join({u->toFrameData(), u"d"})); }
    void addDeadToFrame(Wreck* w) { frameData.push_back(join({w->toFrameData(), u"d"})); }
    void addDeadToFrame(SkillEffect* s) { frameData.push_back(join({s->toFrameData(), u"d"})); }

    void snapshot() {
        // unitsByType: looters, then tankers
        for (Looter* l : looters) frameData.push_back(l->toFrameData());
        for (Tanker* t : tankers) frameData.push_back(t->toFrameData());
        for (Wreck* w : wrecks) frameData.push_back(w->toFrameData());
        for (SkillEffect* s : skillEffects) frameData.push_back(s->toFrameData());
    }

    void initReferee(int playerCount, java::Properties& prop) override {
        this->playerCount = playerCount;

        // Long.valueOf(prop.getProperty("seed", <a random long>)); a NumberFormatException -> a random seed
        String s = prop.getProperty(u"seed", String());
        i64 v;
        if (prop.find(u"seed") && java::parseLong(s, v)) {
            seed = v;
        } else {
            java::Random r((i64)java::entropy());
            seed = r.nextLong();
        }

        java::Random random(seed);

        int TANKERS_BY_PLAYER = TANKERS_BY_PLAYER_MIN + random.nextInt(TANKERS_BY_PLAYER_MAX - TANKERS_BY_PLAYER_MIN + 1);

        // Create players
        for (int i = 0; i < playerCount; ++i) {
            ownedPlayers.emplace_back(new Player(i));
            players.push_back(ownedPlayers.back().get());
        }

        // Generate the map
        std::vector<Player::TankerSpawn> queue;
        for (int i = 0; i < 500; ++i) {
            int size = TANKER_MIN_SIZE + random.nextInt(TANKER_MAX_SIZE - TANKER_MIN_SIZE);
            double angle = random.nextDouble();
            queue.push_back({size, angle});
        }
        for (Player* p : players) p->tankers = queue;

        // Create looters
        for (Player* player : players) {
            for (int i = 0; i < LOOTER_COUNT; ++i) {
                Looter* looter = createLooter(i, player, 0, 0);
                player->looters[i] = looter;
                units.push_back(looter);
                looters.push_back(looter);
            }
        }

        // Random spawns for looters
        bool finished = false;
        while (!finished) {
            finished = true;

            for (int i = 0; i < LOOTER_COUNT && finished; ++i) {
                double distance = random.nextDouble() * (MAP_RADIUS - LOOTER_RADIUS);
                double angle = random.nextDouble();

                for (Player* player : players) {
                    double looterAngle = (player->index + angle) * (PI * 2.0 / ((double)playerCount));
                    double cos = java::cos(looterAngle);
                    double sin = java::sin(looterAngle);

                    Looter* looter = player->looters[i];
                    looter->move(cos * distance, sin * distance);

                    // If the looter touch a looter, reset everyone and try again
                    bool touch = false;
                    for (Unit* u : units)
                        if (u != looter && looter->distance(*u) <= looter->radius + u->radius) {
                            touch = true;
                            break;
                        }
                    if (touch) {
                        finished = false;
                        for (Looter* l : looters) l->move(0.0, 0.0);
                        break;
                    }
                }
            }
        }

        // Spawn start tankers
        for (int j = 0; j < TANKERS_BY_PLAYER; ++j)
            for (Player* player : players) spawnTanker(player);

        adjust();
        newFrame(1.0);
        snapshot();
    }

    java::Properties getConfiguration() override {
        java::Properties properties;
        properties.setProperty(u"seed", S(seed));
        return properties;
    }

    Lines getInitInputForPlayer(int) override { return {}; } // No init input

    void prepare(int) override {
        frameData.clear();
        for (Looter* l : looters) l->reset();
    }

    int getPlayerId(int id, int forId) const {
        // This method can be called with id=-1 because of the default player for units
        if (id < 0) return id;
        if (id == forId) return 0;
        if (id < forId) return id + 1;
        return id;
    }

    Lines getInputForPlayer(int, int playerIdx) override {
        Lines lines;
        // Scores (mine first)
        lines.push_back(S(players[playerIdx]->score));
        for (int i = 0; i < playerCount; ++i)
            if (i != playerIdx) lines.push_back(S(players[i]->score));
        // Rages (mine first)
        lines.push_back(S(players[playerIdx]->rage));
        for (int i = 0; i < playerCount; ++i)
            if (i != playerIdx) lines.push_back(S(players[i]->rage));

        Lines unitsLines;
        // Looters and tankers
        for (Unit* u : units)
            unitsLines.push_back(join({S(u->id), S(u->type), S(getPlayerId(u->getPlayerIndex(), playerIdx)), D(u->mass), S(refRound(u->radius)), S(refRound(u->x)),
                                       S(refRound(u->y)), S(refRound(u->vx)), S(refRound(u->vy)), S(u->getExtraInput()), S(u->getExtraInput2())}));
        // Wrecks
        for (Wreck* w : wrecks)
            unitsLines.push_back(join({S(w->id), S(TYPE_WRECK), u"-1", u"-1", S(refRound(w->radius)), S(refRound(w->x)), S(refRound(w->y)), u"0", u"0", S(w->water), u"-1"}));
        // Skill effects
        for (SkillEffect* s : skillEffects)
            unitsLines.push_back(join({S(s->id), S(s->type), u"-1", u"-1", S(refRound(s->radius)), S(refRound(s->x)), S(refRound(s->y)), u"0", u"0", S(s->duration), u"-1"}));

        lines.push_back(S((i64)unitsLines.size()));
        lines.insert(lines.end(), unitsLines.begin(), unitsLines.end());
        return lines;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 3; }

    void handlePlayerOutput(int, int, int playerIdx, const Lines& outputs) override {
        Player* player = players[playerIdx];
        String expected = EXPECTED;

        for (int i = 0; i < LOOTER_COUNT; ++i) {
            // outputs[i] (the runner sends 3 lines)
            if (i >= (int)outputs.size())
                throw java::Exception{java::arrayIndexOutOfBoundsException(i, (i64)outputs.size())};
            const String& line = outputs[i];
            Looter* looter = players[playerIdx]->looters[i];
            String gx, gy, gpower;
            NString message;

            if (matchWait(line, message)) {
                looter->attempt = Action::WAIT;
                matchMessage(looter, message);
                continue;
            }

            int move = matchMove(line, gx, gy, gpower, message);
            if (move == 1) {
                looter->attempt = Action::MOVE;
                looter->setWantedThrust(Point(parseInt(gx), parseInt(gy)), parseInt(gpower));
                matchMessage(looter, message);
                continue;
            }
            if (move == 0 && matchSkill(line, gx, gy, message)) {
                if (!looter->skillActive) {
                    // Don't kill the player for that. Just do a WAIT instead (the wood leagues)
                    looter->attempt = Action::WAIT;
                    matchMessage(looter, message);
                    continue;
                }

                looter->attempt = Action::SKILL;
                int x = parseInt(gx);
                int y = parseInt(gy);

                // looter.skill(new Point(x, y)): NoRageException, TooFarException leave no effect
                Point p(x, y);
                if (!(player->rage < looter->skillCost) && !(looter->distance(p) > looter->skillRange)) {
                    player->rage -= looter->skillCost;
                    addEffect(skillEffects, own(looter->skillImpl(p)));
                }
                matchMessage(looter, message);
                continue;
            }

            // no match (or a move without its power: a NumberFormatException, caught the same way)
            player->kill();
            throw legacy::InvalidInputException(expected, line);
        }
    }

    int getMillisTimeForRound() override { return 50; }

    // Get the next collision for the current round. All units are tested
    Collision getNextCollision() {
        Collision result = NULL_COLLISION;
        for (size_t i = 0; i < units.size(); ++i) {
            Unit* unit = units[i];
            // Test collision with map border first
            Collision collision = unit->getCollision();
            if (collision.t < result.t) result = collision;
            for (size_t j = i + 1; j < units.size(); ++j) {
                collision = unit->getCollision(units[j]);
                if (collision.t < result.t) result = collision;
            }
        }
        return result;
    }

    template <class T>
    static void removeObject(std::vector<T*>& list, T* o) { // List.remove(Object): the first equal one (the same object)
        auto it = std::find(list.begin(), list.end(), o);
        if (it != list.end()) list.erase(it);
    }

    // Play a collision
    void playCollision(const Collision& collision) {
        if (collision.b == nullptr) {
            // Bounce with border
            frameData.push_back(collision.a->toFrameData());
            collision.a->bounce();
        } else {
            Tanker* dead = collision.dead();
            if (dead != nullptr) {
                // A destroyer kill a tanker
                addDeadToFrame(dead);
                deadTankers.push_back(dead);
                removeObject(tankers, dead);
                removeObject<Unit>(units, dead);

                Wreck* wreck = dead->die();
                // If a tanker is too far away, there's no wreck
                if (wreck != nullptr) {
                    own(wreck);
                    wrecks.push_back(wreck);
                    frameData.push_back(wreck->toFrameData());
                }
            } else {
                // Bounce between two units
                frameData.push_back(collision.a->toFrameData());
                frameData.push_back(collision.b->toFrameData());
                collision.a->bounce(collision.b);
            }
        }
    }

    void updateGame(int) override {
        // Apply skill effects
        for (SkillEffect* effect : skillEffects) effect->apply(units);

        // Apply thrust for tankers
        for (Tanker* t : tankers) t->play();

        // Apply wanted thrust for looters
        for (Player* player : players)
            for (Looter* looter : player->looters)
                if (looter->wantedThrustTarget) looter->thrust(*looter->wantedThrustTarget, looter->wantedThrustPower);

        double t = 0.0;

        // Play the round. Stop at each collisions and play it. Reapeat until t > 1.0
        Collision collision = getNextCollision();
        long long collisions = 0;
        while (collision.t + t <= 1.0) {
            // (the Java referee would loop until the runner gives up: never seen)
            if (++collisions > 1000000) throw java::Exception{"cg-2017-mean-max: more than 10^6 collisions in a round"};
            double delta = collision.t;
            for (Unit* u : units) u->move(delta);
            t += collision.t;

            newFrame(t);

            playCollision(collision);

            collision = getNextCollision();
        }

        // No more collision. Move units until the end of the round
        double delta = 1.0 - t;
        for (Unit* u : units) u->move(delta);

        std::vector<Tanker*> tankersToRemove;

        for (Tanker* tanker : tankers) {
            double distance = tanker->distance(WATERTOWN);
            bool full = tanker->isFull();

            if (distance <= WATERTOWN_RADIUS && !full) {
                // A non full tanker in watertown collect some water
                tanker->water += 1;
                tanker->mass += TANKER_MASS_BY_WATER;
            } else if (distance >= TANKER_SPAWN_RADIUS + tanker->radius && full) {
                // Remove too far away and not full tankers from the game
                tankersToRemove.push_back(tanker);
            }
        }

        newFrame(1.0);
        snapshot();

        for (Tanker* tanker : tankersToRemove) addDeadToFrame(tanker);

        // removeAll: every element equal to one of them (by id: the same objects)
        auto removeAll = [&](auto& list) {
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](Unit* u) {
                                          for (Tanker* t : tankersToRemove)
                                              if ((Unit*)t == u) return true;
                                          return false;
                                      }),
                       list.end());
        };
        removeAll(units);
        removeAll(tankers);
        deadTankers.insert(deadTankers.end(), tankersToRemove.begin(), tankersToRemove.end());

        // Spawn new tankers for each dead tanker during the round
        for (Tanker* tanker : deadTankers) spawnTanker(tanker->player);
        deadTankers.clear();

        // Water collection for reapers (deadWrecks: a HashSet used by the wood leagues only)
        std::vector<Wreck*> alive;
        for (Wreck* w : wrecks) {
            bool a = w->harvest(players, skillEffects);
            if (!a) addDeadToFrame(w);
            else alive.push_back(w);
        }
        wrecks = alive;

        // Round values and apply friction
        adjust();

        // Generate rage
        if (LOOTER_COUNT >= 3)
            for (Player* p : players) p->rage = std::min(MAX_RAGE, p->rage + static_cast<Doof*>(p->getDoof())->sing());

        // Restore masses
        for (Unit* u : units)
            while (u->mass >= REAPER_SKILL_MASS_BONUS) u->mass -= REAPER_SKILL_MASS_BONUS;

        // Remove dead skill effects
        EffectSet left;
        for (SkillEffect* effect : skillEffects) {
            if (effect->duration <= 0) addDeadToFrame(effect);
            else left.push_back(effect);
        }
        skillEffects = left;
    }

    void adjust() {
        for (Unit* u : units) u->adjust(skillEffects);
    }

    void populateMessages(std::map<std::string, String>& p) override {
        p["Move"] = u"$%d moved looter %d towards (%d,%d) with power %d";
        p["SkillFailedTooFar"] = u"$%d %d %d %d";
        p["SkillFailedNoRage"] = u"$%d %d %d %d";
        p["SkillDestroyer"] = u"$%d %d %d %d";
        p["SkillRepear"] = u"$%d %d %d %d";
        p["SkillDoof"] = u"$%d %d %d %d";
    }

    // isGameOver() (@Override of the base's hook, called by nextRound(): legacy.h names it gameIsOverHook)
    bool gameIsOverHook() override {
        for (Player* p : players)
            if (p->score >= WIN_SCORE) return true; // We got a winner !
        std::vector<Player*> alive;
        for (Player* p : players)
            if (!p->dead) alive.push_back(p);
        if (alive.size() == 1) {
            Player* survivor = alive[0];
            // If only one player is alive and he got the highest score, end the game now.
            for (Player* p : players)
                if (p != survivor && !(p->score < survivor->score)) return false;
            return true;
        }
        // Everyone is dead. End of the game.
        return alive.empty();
    }

    Lines getInitDataForView() override {
        return {S(playerCount), S(java::round(MAP_RADIUS)), S(java::round(WATERTOWN_RADIUS)), S(java::round((double)LOOTER_COUNT))};
    }

    Lines getFrameDataForView(int, int, int, bool) override {
        Lines lines;
        for (Player* p : players) lines.push_back(S(p->score));
        for (Player* p : players) lines.push_back(S(p->rage));
        for (Looter* l : looters) lines.push_back(l->message ? *l->message : String());
        lines.insert(lines.end(), frameData.begin(), frameData.end());
        return lines;
    }

    String getGameName() override { return u"meanmax"; }
    NString getHeadlineAtGameStartForConsole() override { return String(); } // "" (not null)
    int getMinimumPlayerCount() override { return 3; }
    bool showTooltips() override { return true; }
    Lines getPlayerActions(int, int) override { return {}; }
    bool isPlayerDead(int playerIdx) override { return players[playerIdx]->dead; }
    NString getDeathReason(int playerIdx) override { return u"$" + S(playerIdx) + u": Eliminated!"; }
    int getScore(int playerIdx) override { return players[playerIdx]->score; }
    Lines getGameSummary(int) override { return {}; } // (its body is commented out)
    void setPlayerTimeout(int, int, int playerIdx) override { players[playerIdx]->kill(); }
    int getMaxRoundCount(int) override { return 200; }
    bool isTurnBasedGame() override { return false; }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
