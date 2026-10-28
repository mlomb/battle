// Fantastic Bits (CodinGame, 2016-11, "fantastic-bits") - headless C++ referee, port of the CodinGame referee in
// https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the folder "Referees/Fantastic
// Bits" was added in aeefee6, 2018-03-31, removed the same day and added again unchanged in fc160b7, 2018-04-03, never
// changed since): the single-file referee `class Referee extends MultiReferee` with CodinGame's old AbstractReferee
// base of 2016 embedded (common/legacy.h, legacy::Base::fantasticBits2016()). The folder holds two versions of the
// game, both `class Referee`:
//   - RefereePuzzle.java, the multiplayer game CodinGame runs today (this file, preset cg-2016-fantastic-bits): Obliviate
//     lasts 4 rounds, Accio costs 15 magic, the input starts with "myScore myMagic" and "opponentScore opponentMagic",
//     a snaffle's state is 1 while a wizard holds it, a bludger's the id of its last victim (-1);
//   - Referee.java, the contest's referee (cg-2016-fantastic-bits-contest.cpp: this file with FANTASTIC_BITS_CONTEST 1):
//     Obliviate 3 rounds, Accio 20 magic, each turn's input is the entity count then the entities (snaffles and
//     bludgers with the state 0).
//   Nothing else differs (the base, the physics, the parsing are the same text). CodinGame's replays of today's game
//   (codingame.com's public gameResult/findByGameId: 720 games of the top 1000, Gold and Legend) are reproduced frame
//   for frame by RefereePuzzle.java (its jar and this port's default build through their CLIs: views, infos,
//   summaries, tooltips, scores), not by Referee.java (52 of the 720: Accio's cost and Obliviate's duration differ).
// Exactly 2 players (getMinimumPlayerCount() 2; the goals, the end and the input are written for players 0 and 1).
// Deterministic: reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): "seed=N") to stdout like
// the jar (referees/cg-builder/legacy: the referee runs unchanged in the runner of the SDK 4.5.0). "null" scores when
// the game fails (the Java referee crashed, looped forever or the protocol broke; the jar after 150 s). -l writes the
// trace of common/legacy.h (bot executions, view sizes, summaries, tooltips, END; CG_TRACE_VIEWS=1 adds the text of
// every view). Time limits: the base's 1000 ms for the first turn, 100 ms after.
//
// League: the referee's static WITH_BLUDGERS and WITH_SPELLS are true unless the init properties with_bludgers /
// with_spells say otherwise; the CLI only passes the seed, so the jar plays the top rules (bludgers and the four
// spells), the only ones implemented here. CodinGame's replays of every league have the seed alone as their input, and
// are reproduced with WITH_BLUDGERS and WITH_SPELLS false (Wood 2), WITH_SPELLS false (Wood 1), the defaults (Bronze
// to Legend). The other init properties (friction, masses, radii, durations...) keep their defaults the same way.
//
// The game: a 16000 x 7500 field, a goal 4000 wide in the middle of each side (posts of radius 300 at y 1750 and
// 5750), player 0 scores at the right, player 1 at the left. Each player has 2 wizards (radius 400, mass 1, friction
// 0.25); 5 or 7 snaffles (java.util.Random(seed): 2 + nextInt(2) random pairs, point-symmetric, 1250 apart, plus one
// in the centre; radius 150, mass 0.5, friction 0.25), 2 bludgers (radius 200, mass 8, friction 0.1) chasing the
// closest wizard other than their last victim. Each round every player answers 2 lines, one per wizard: `MOVE x y
// thrust` (0-150), `THROW x y power` (0-500, while holding a snaffle) or a spell on an entity id, `OBLIVIATE` (a
// bludger forgets the team, 5 magic), `PETRIFICUS` (the target stops, 10), `ACCIO` (pulled towards the wizard, 20 /
// 15), `FLIPENDO` (pushed away, 20), each with an optional message; magic +1 per round (max 100). A spell is paid
// when cast and acts from the update after the next one (it starts in the next prepare()). The round: thrusts,
// spells, then the movement with exact collision times (walls but the goal mouths for snaffles, goal posts, entity
// pairs: impulses with a minimum of 100, overlaps fixed; a wizard within 399 of a free snaffle catches it unless
// another free wizard is as close; a caught snaffle follows its wizard until the next update releases it; a snaffle
// crossing a goal line scores), friction and rounding. An invalid line or a timeout eliminates the player (score -1,
// the game ends after that round's update); the game ends after the update that started without any snaffle left, in
// which a player reached the majority (pairs + 1 goals) or after which a player is eliminated, or after 200 rounds.
// The score is the goals.
//
// The Java referee is followed closely, notably: the double precision physics in the same operation order
// (FP_CONTRACT off; Math.sqrt is the only Math function), Java's object references for the positions and speeds (a
// caught snaffle shares its wizard's Vector objects, and the collisions write into the snaffle's ones in place:
// whatever else still refers to them sees it), the collision lists (created in the referee's order, a stable sort on
// Double.compare of the times, every collision within 1e-5 of the first one reacting), the static id counters
// (entities, collisions: their ids show in the view), Math.round of the view's int targets as Math.round(float)
// (java.awt.Point coordinates: 99999999 shows 100000000), symmetricRound, the regexes (hand matched: literal "MOVE "
// etc., \s ASCII, 1-8 / 1-3 digit numbers, the message after the whitespace run or the last character of a
// whitespace-only tail; "MOVE 1 2 3 " is invalid), the lines of a player before its invalid line still applying that
// round, messages cut to 26 UTF-16 chars + "..." beyond 28, a dead snaffle still moving (and colliding with bludgers)
// after it scored, the view's frame data built in full (collision texts with Double.toString; views stay around 1 KB,
// far from the runner's 30000-byte limit). The game's collections are lists (no hash order); the base's tooltips (a
// HashSet of identity hash codes: several per frame) only count by their size.
// Reproduced but never seen (not reachable in a game as far as the validation went): a snaffle's wall collision that
// reacts once the snaffle is in a goal (it keeps no positions: its view text throws a NullPointerException, the
// referee dies, "null" scores), a capture blocked by a closer wizard or two wizards catching one snaffle (wizards stay
// 800 apart), and the Java referee's endless loop when a step's collisions are only failed captures at its start (the
// time never moves on; the jar's runner gives up after 150 s, "null" scores): this port detects that step and fails
// the same way at once (any other round is cut after 10^6 steps).
// Not reproduced / not modelled: the wall clock (see engine.h).
//
// Validation (referees/README.md): referee-diff against the jars with 34 deterministic bots (every ordered pair, seed
// bases 1, 1000001, 987654321, 5000000000 and more), direct runs comparing the jar's game log with this port's trace
// (views, summaries, tooltips; raw bytes and line endings), the jar's own compiled classes driven by reflection over
// random states (physics, spells, captures, views: bit for bit), CodinGame's replays.
#include "common/legacy.h"

#include <cmath>
#include <memory>

#ifndef FANTASTIC_BITS_CONTEST
#define FANTASTIC_BITS_CONTEST 0
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// RefereePuzzle.java (the multiplayer game) unless FANTASTIC_BITS_CONTEST (Referee.java, the contest)
const bool PUZZLE = FANTASTIC_BITS_CONTEST == 0;

// --- constants (the defaults: the CLI passes no other init property than the seed) ---
const bool WITH_BLUDGERS = true;
const bool WITH_SPELLS = true;
const double EPSILON = 0.00001;
const double MIN_IMPULSE = 100;
const int MAX_POWER = 150;
const int MAX_POWER_SNAFFLE = 500;
const double FRICTION = 0.25;
const double FRICTION_BLUDGER = 0.1;
const double FRICTION_SNAFFLE = 0.25;
const double BLUDGER_MASS = 8;
const double SNAFFLE_MASS = 0.5;
const int POD_RADIUS = 400;
const int SNAFFLE_RADIUS = 150;
const int BLUDGER_RADIUS = 200;
const int GOAL_RADIUS = 300;
const int GOAL_SIZE = 4000;
const int PODS_PER_PLAYER = 2;
const int SPACE_BETWEEN_POD = 3000;
const int MIN_SPACE_BETWEEN_SNAFFLES = 1250;
const int WIDTH = 16000;
const int HEIGHT = 7500;
const int POINTS_GOAL = 1;
const int TIME_BEFORE_CAPTURE_AGAIN = 3;
int SCORE_TO_WIN = 0;

const int OBLIVIATE_DURATION = PUZZLE ? 4 : 3;
const int ACCIO_DURATION = 6;
const int FLIPENDO_DURATION = 3;
const int PETRIFICUS_DURATION = 1;

const int MAX_MAGIC = 100;
const int ACCIO_MAGIC = PUZZLE ? 15 : 20;
const int PETRIFICUS_MAGIC = 10;
const int OBLIVIATE_MAGIC = 5;
const int FLIPENDO_MAGIC = 20;

String S(i64 v) { return java::str(v); }
String D(double v) { return java::ascii(java::doubleToString(v)); } // String.valueOf(double)

// Referee.symmetricRound(double): halves away from zero, (int) of Math.round's long
int symmetricRound(double x) {
    int s = x < 0 ? -1 : 1;
    return java::mul(s, java::l2i(java::round(s * x)));
}

// --- Vector: the arithmetic on values; the entities' positions and speeds are references to shared, mutable
// Vector objects (VP), like the Java objects ---
struct Vec {
    double x, y;
};
using VP = std::shared_ptr<Vec>;
VP vnew(const Vec& v) { return std::make_shared<Vec>(v); }

Vec between(const Vec& a, const Vec& b) { return {b.x - a.x, b.y - a.y}; } // new Vector(a, b)
double distance(const Vec& a, const Vec& v) { return std::sqrt((v.x - a.x) * (v.x - a.x) + (v.y - a.y) * (v.y - a.y)); }
double distance2(const Vec& a, const Vec& v) { return (v.x - a.x) * (v.x - a.x) + (v.y - a.y) * (v.y - a.y); }
Vec add(const Vec& a, const Vec& v) { return {a.x + v.x, a.y + v.y}; }
Vec mult(const Vec& a, double factor) { return {a.x * factor, a.y * factor}; }
Vec sub(const Vec& a, const Vec& v) { return {a.x - v.x, a.y - v.y}; }
double length(const Vec& a) { return std::sqrt(a.x * a.x + a.y * a.y); }
double lengthSquared(const Vec& a) { return a.x * a.x + a.y * a.y; }
Vec normalize(const Vec& a) {
    double l = length(a);
    if (l == 0) return {0, 0};
    return {a.x / l, a.y / l};
}
double dot(const Vec& a, const Vec& v) { return a.x * v.x + a.y * v.y; }
Vec symmetricRound(const Vec& a) { return {(double)symmetricRound(a.x), (double)symmetricRound(a.y)}; }

enum class Side { LEFT, RIGHT, BOTTOM, TOP };
const char16_t* sideName(Side s) {
    switch (s) {
    case Side::LEFT: return u"LEFT";
    case Side::RIGHT: return u"RIGHT";
    case Side::BOTTOM: return u"BOTTOM";
    default: return u"TOP";
    }
}

struct CollisionSide {
    Side side;
    double timeDist;
};

enum class Kind { GOALPOST, POD, SNAFFLE, BLUDGER };

struct Collision;
using CP = std::shared_ptr<Collision>;
struct Pod;
struct Player;

// Entity.UNIQUE_ENTITY_ID (a static: 0 in the jar's fresh JVM)
int UNIQUE_ENTITY_ID = 0;

struct Entity {
    Kind kind;
    int id;
    VP position;
    int radius;
    const char16_t* type;

    Entity(Kind kind, const char16_t* type, const Vec& position, int radius) : kind(kind), id(UNIQUE_ENTITY_ID++), position(vnew(position)), radius(radius), type(type) {}
    virtual ~Entity() {}

    bool collidesWith(const Entity& other) const {
        return distance2(*this->position, *other.position) <= (double)((this->radius + other.radius) * (this->radius + other.radius));
    }
};

struct GoalPost : Entity {
    GoalPost(int x, int y) : Entity(Kind::GOALPOST, u"GOALPOST", Vec{(double)x, (double)y}, GOAL_RADIUS) {}
};

struct DynamicEntity : Entity {
    VP speed;
    Vec acceleration{0, 0}; // (only ever replaced: a value)
    bool alive = true;      // not alive: not passed to the players, no more interactions (but it still moves)

    DynamicEntity(Kind kind, const char16_t* type, const Vec& position, int radius) : Entity(kind, type, position, radius), speed(vnew({0, 0})) {}

    virtual void prepareRound() = 0;
    virtual String toViewString() = 0;

    // (Referee.java: DynamicEntity#toPlayerString with the state 0; RefereePuzzle.java: Snaffle's and Bludger's own)
    virtual String toPlayerString() {
        return S(id) + u" " + type + u" " + S(java::round(position->x)) + u" " + S(java::round(position->y)) + u" " + S(symmetricRound(speed->x)) + u" " +
               S(symmetricRound(speed->y)) + u" " + S(0);
    }

    void step(double step) {
        if (step > 0) this->position = vnew(add(*this->position, mult(*this->speed, step)));
    }

    virtual double getMass() { return 1; }

    virtual void reset() {}

    // applyForce(time, force): the way points (never shown anywhere) left out
    void applyForce(double, const Vec& force) { applyForce(force); }
    void applyForce(const Vec& force) { this->speed = vnew(add(*this->speed, mult(force, 1.0 / getMass()))); }

    static double contactPosition(const Vec& dv, const Vec& d, double dmin) {
        double a = lengthSquared(dv);
        double b = d.x * dv.x + d.y * dv.y;
        double c = lengthSquared(d) - dmin * dmin;
        double delta = b * b - a * c;
        if (delta <= 0 || a == 0) return INFINITY;
        double rd = std::sqrt(delta);
        return java::min((-b + rd) / a, (-b - rd) / a);
    }
    double contactPosition(const Entity& o) { return contactPosition(*this->speed, between(*o.position, *this->position), radius + o.radius); }
    double contactPosition(const DynamicEntity& o) { return contactPosition(sub(*this->speed, *o.speed), between(*o.position, *this->position), radius + o.radius); }

    // fixPosition: setPosition (the way point's copy left out)
    void fixPosition(const Vec& position) { this->position = vnew(position); }

    void checkEntityCollision(DynamicEntity* other, std::vector<CP>& collisions, double beginTime, double min);
    void checkEntityCollision(Entity* other, std::vector<CP>& collisions, double beginTime, double min);
    virtual void checkWallCollisions(std::vector<CP>& collisions, double beginTime, double min);

    virtual bool contactPositionWall(CollisionSide& res) {
        bool found = false;
        if (this->speed->x > 0) {
            double right = (WIDTH - radius - this->position->x) / this->speed->x;
            if (right >= 0) {
                res = {Side::RIGHT, right};
                found = true;
            }
        }
        if (this->speed->x < 0) {
            double left = (radius - this->position->x) / this->speed->x;
            if (left >= 0 && (!found || res.timeDist > left)) {
                res = {Side::LEFT, left};
                found = true;
            }
        }
        if (this->speed->y > 0) {
            double bottom = (HEIGHT - radius - this->position->y) / this->speed->y;
            if (bottom >= 0 && (!found || res.timeDist > bottom)) {
                res = {Side::BOTTOM, bottom};
                found = true;
            }
        }
        if (this->speed->y < 0) {
            double top = (radius - this->position->y) / this->speed->y;
            if (top >= 0 && (!found || res.timeDist > top)) {
                res = {Side::TOP, top};
                found = true;
            }
        }
        return found;
    }

    virtual void hasCollidedWith(DynamicEntity*) {}

    virtual void endRound() {
        this->position = vnew(symmetricRound(*this->position));
        this->speed = vnew(symmetricRound(*this->speed));
    }
};

struct Bludger;
struct Snaffle;

// --- the spells' count downs ---
struct CountDown {
    int value = 0;
    void dec() {
        if (value > 0) value--;
    }
    void setValue(int v) { value = v; }
    int getValue() const { return value; }
};

enum class Action { NONE, MOVE, ACCIO, OBLIVIATE, PETRIFICUS, FLIPENDO, IDLE, THROW };

// the referee's addToolTip, for the game's classes
void addToolTip(int player, const String& message);

struct Player {
    int id;
    std::vector<Pod*> pods;
    int score = 0;
    int magic = 0;
    bool dead = false;

    explicit Player(int id) : id(id) {}

    void setDead(bool b) {
        this->score = -1;
        this->dead = b;
    }

    void addScore(int points) {
        if (points != 0 && !this->dead) {
            this->score += points;
            addToolTip(this->id, u"$" + S(this->id) + u" scored");
        }
    }

    int getMagic() const { return magic; }

    void increaseMagic() {
        if (magic < MAX_MAGIC && WITH_SPELLS) magic++;
    }

    void decreaseMagic(int value) { magic -= value; }
};

struct Pod : DynamicEntity {
    Action lastAction = Action::NONE; // (null)
    int timeBeforeCanCaptureAgain = 0;
    Snaffle* snaffle = nullptr;
    // java.awt.Point (ints), null when absent
    bool hasTarget = false;
    int targetX = 0, targetY = 0;

    Player* player;
    NString message;
    int power = 0;
    int throwPower = 0;

    CountDown cloakCountDown; // (the cloak spell is commented out: always 0)
    CountDown obliviateCountDown;
    Bludger* obliviateTarget = nullptr;
    CountDown accioCountDown;
    DynamicEntity* accioTarget = nullptr;
    CountDown flipendoCountDown;
    DynamicEntity* flipendoTarget = nullptr;
    CountDown petrificusCountDown;
    DynamicEntity* petrificusTarget = nullptr;

    bool doAccio = false;
    bool doObliviate = false;
    bool doFlipendo = false;
    bool doPetrificus = false;

    DynamicEntity* spellTarget = nullptr;

    // Boolean spellSuccess: 0 null, 1 true, 2 false
    int spellSuccess = 0;

    Pod(const Vec& position, Player* player, int startX, int startY) : DynamicEntity(Kind::POD, u"POD", position, POD_RADIUS), player(player) {
        hasTarget = true;
        targetX = startX;
        targetY = startY;
    }

    String toPlayerString(bool myPlayer) {
        i64 positionX = (myPlayer || cloakCountDown.getValue() == 0) ? java::round(position->x) : 0;
        i64 positionY = (myPlayer || cloakCountDown.getValue() == 0) ? java::round(position->y) : 0;
        const char16_t* t = myPlayer ? u"WIZARD" : u"OPPONENT_WIZARD";
        return S(id) + u" " + t + u" " + S(positionX) + u" " + S(positionY) + u" " + S(symmetricRound(speed->x)) + u" " + S(symmetricRound(speed->y)) + u" " +
               S(this->snaffle != nullptr ? 1 : 0);
    }
    using DynamicEntity::toPlayerString;

    String toViewString() override;

    void reset() override {
        this->message.reset();
        this->hasTarget = false;
        this->power = 0;
        this->throwPower = 0;
        this->spellSuccess = 0;
    }

    void prepareRound() override;

    void endRound() override {
        this->speed = vnew(mult(*this->speed, 1 - FRICTION));
        DynamicEntity::endRound();
    }

    void releaseSnaffle();

    double contactPosition(const Snaffle& o);
    using DynamicEntity::contactPosition;

    void checkCaptureSnaffleCollision(std::vector<CP>& collisions, double beginTime, double min, const std::vector<Pod*>& pods);

    void applyAccio() {
        if (accioTarget != nullptr) {
            Vec d = sub(*position, *accioTarget->position);
            double power = java::min(3000.0 * (1000 * 1000 / lengthSquared(d)), 1000); // power = 3000 / (dist/1000)**2
            Vec force = mult(normalize(d), power);
            accioTarget->applyForce(force);
        }
    }

    void applyFlipendo() {
        if (flipendoTarget != nullptr && (flipendoTarget->position->x != position->x || flipendoTarget->position->y != position->y)) {
            Vec d = sub(*flipendoTarget->position, *position);
            double power = java::min(6000.0 * (1000 * 1000 / lengthSquared(d)), 1000); // power = 6000 / (dist/1000)**2
            Vec force = mult(normalize(d), power);
            flipendoTarget->applyForce(force);
        }
    }

    void applyPetrificus() {
        if (petrificusTarget != nullptr) petrificusTarget->speed = vnew({0, 0});
    }

    void setMessage(const NString& msg) {
        this->message = msg;
        if (this->message && this->message->size() > 28) this->message = this->message->substr(0, 26) + u"...";
    }
};

struct Bludger : DynamicEntity {
    Pod* lastVictim = nullptr;

    Bludger(int x, int y) : DynamicEntity(Kind::BLUDGER, u"BLUDGER", Vec{(double)x, (double)y}, BLUDGER_RADIUS) {}

    double getMass() override { return BLUDGER_MASS; }

    void hasCollidedWith(DynamicEntity* c2) override {
        if (c2->kind == Kind::POD) lastVictim = static_cast<Pod*>(c2);
    }

    void prepareRound() override { this->acceleration = {0, 0}; }

    void endRound() override {
        this->speed = vnew(mult(*this->speed, 1 - FRICTION_BLUDGER));
        DynamicEntity::endRound();
    }

    bool canTarget(Pod* pod, const std::vector<Pod*>& pods) {
        // Obliviate spell: if one pod of the same team as `pod` sent an obliviate on this bludger, ignore player
        for (Pod* p : pods)
            if (p->obliviateCountDown.getValue() > 0 && p->obliviateTarget == this && p->player->id == pod->player->id) return false;
        return pod != lastVictim && pod->cloakCountDown.getValue() == 0;
    }

    void updateAcceleration(const std::vector<Pod*>& pods) {
        Pod* closestPod = nullptr;
        for (Pod* p : pods)
            if (canTarget(p, pods) && (closestPod == nullptr || distance(*position, *p->position) < distance(*position, *closestPod->position))) closestPod = p;
        if (closestPod != nullptr) this->acceleration = mult(normalize(sub(*closestPod->position, *this->position)), 1000);
    }

    String toPlayerString() override {
        if (!PUZZLE) return DynamicEntity::toPlayerString();
        return S(id) + u" " + type + u" " + S(java::round(position->x)) + u" " + S(java::round(position->y)) + u" " + S(symmetricRound(speed->x)) + u" " +
               S(symmetricRound(speed->y)) + u" " + S(lastVictim != nullptr ? lastVictim->id : -1);
    }

    String toViewString() override { return S(id) + u" " + S(java::round(position->x)) + u" " + S(java::round(position->y)); }
};

struct Snaffle : DynamicEntity {
    bool captured = false;
    std::vector<Snaffle*> ignoreCollisionList;

    Snaffle(int x, int y, int vx, int vy) : DynamicEntity(Kind::SNAFFLE, u"SNAFFLE", Vec{(double)x, (double)y}, SNAFFLE_RADIUS) {
        this->speed = vnew({(double)vx, (double)vy});
    }

    double getMass() override { return SNAFFLE_MASS; }

    void prepareRound() override { this->acceleration = {0, 0}; }

    void endRound() override {
        this->speed = vnew(mult(*this->speed, 1 - FRICTION_SNAFFLE));
        DynamicEntity::endRound();
    }

    void setCapturedByPod(Pod* pod) {
        this->captured = true;
        this->position = pod->position;
        this->speed = pod->speed;
    }

    void scored() { this->alive = false; }

    bool canBeCapturedByPod(const Pod* p) const { return this->alive && p->timeBeforeCanCaptureAgain <= 0 && this->captured == false; }

    bool contactPositionWall(CollisionSide& res) override {
        bool found = false;
        if (this->speed->x > 0) {
            double right = (WIDTH - radius - position->x) / speed->x;
            double collisionY = position->y + right * speed->y;
            if (right >= 0 && (collisionY < (HEIGHT - GOAL_SIZE) / 2 || collisionY > (HEIGHT + GOAL_SIZE) / 2)) {
                res = {Side::RIGHT, right};
                found = true;
            }
        } else if (this->speed->x < 0) {
            double left = (radius - this->position->x) / this->speed->x;
            double collisionY = position->y + left * speed->y;
            if (left >= 0 && (collisionY < (HEIGHT - GOAL_SIZE) / 2 || collisionY > (HEIGHT + GOAL_SIZE) / 2) && (!found || res.timeDist > left)) {
                res = {Side::LEFT, left};
                found = true;
            }
        }
        if (this->speed->y > 0) {
            double bottom = (HEIGHT - radius - this->position->y) / this->speed->y;
            if (bottom >= 0 && (!found || res.timeDist > bottom)) {
                res = {Side::BOTTOM, bottom};
                found = true;
            }
        } else if (this->speed->y < 0) {
            double top = (radius - this->position->y) / this->speed->y;
            if (top >= 0 && (!found || res.timeDist > top)) {
                res = {Side::TOP, top};
                found = true;
            }
        }
        return found;
    }

    bool inGoal() const {
        return (this->position->x <= this->radius || this->position->x >= WIDTH - this->radius) && this->position->y >= (HEIGHT - GOAL_SIZE) / 2 &&
               this->position->y <= (HEIGHT + GOAL_SIZE) / 2;
    }

    void checkWallCollisions(std::vector<CP>& collisions, double beginTime, double min) override {
        if (!inGoal()) DynamicEntity::checkWallCollisions(collisions, beginTime, min);
    }

    String toPlayerString() override {
        if (!PUZZLE) return DynamicEntity::toPlayerString();
        return S(id) + u" " + type + u" " + S(java::round(position->x)) + u" " + S(java::round(position->y)) + u" " + S(symmetricRound(speed->x)) + u" " +
               S(symmetricRound(speed->y)) + u" " + S(this->captured ? 1 : 0);
    }

    String toViewString() override { return S(id) + u" " + S(java::round(position->x)) + u" " + S(java::round(position->y)) + u" " + S(alive ? 1 : 0); }

    void checkGoalCollisions(std::vector<CP>& collisions, double beginTime, double min, Player* const* players);

    void ignoreCollisionWith(Snaffle* snaffle) { this->ignoreCollisionList.push_back(snaffle); }
    bool ignores(const Snaffle* snaffle) const {
        for (const Snaffle* s : ignoreCollisionList)
            if (s == snaffle) return true;
        return false;
    }
};

void Pod::releaseSnaffle() {
    this->snaffle->captured = false;
    this->snaffle = nullptr;
}

double Pod::contactPosition(const Snaffle& o) { return DynamicEntity::contactPosition(sub(*this->speed, *o.speed), between(*o.position, *this->position), radius - 1); }

// --- the collisions ---

// Collision.COLLISION_UNIQUE_ID (a static: 0 in the jar's fresh JVM)
int COLLISION_UNIQUE_ID = 0;

i64 roundOf(const VP& v, bool y) {
    if (!v) throw java::Exception{"java.lang.NullPointerException"};
    return java::round(y ? v->y : v->x);
}

struct Collision {
    int uniqueId;
    double time;

    explicit Collision(double time) : uniqueId(COLLISION_UNIQUE_ID++), time(time) {}
    virtual ~Collision() {}
    virtual bool react() = 0;
    virtual String toViewString() = 0;
};

struct SnaffleScoreCollision : Collision {
    Snaffle* snaffle;
    Player* player;

    SnaffleScoreCollision(Snaffle* snaffle, double time, Player* player) : Collision(time), snaffle(snaffle), player(player) {}

    bool react() override {
        this->snaffle->scored();
        this->player->addScore(POINTS_GOAL);
        return true;
    }

    String toViewString() override {
        return u"SNAFFLESCORE " + S(uniqueId) + u" " + D(this->time) + u" " + S(snaffle->id) + u" " + S(player->id) + u" " + S(java::round(snaffle->position->x)) + u" " +
               S(java::round(snaffle->position->y));
    }
};

struct SnaffleCollision : Collision {
    Pod* c1;
    VP c1Position;
    VP snafflePosition;
    Snaffle* snaffle;
    const std::vector<Pod*>* pods;

    SnaffleCollision(Snaffle* snaffle, double time, Pod* c1, const std::vector<Pod*>* pods) : Collision(time), c1(c1), snaffle(snaffle), pods(pods) {}

    bool react() override {
        if (c1->snaffle != nullptr) return false;
        double distSnaffle2 = distance2(*snaffle->position, *c1->position) + EPSILON;
        for (Pod* p : *pods)
            if (p != c1 && p->snaffle == nullptr && distSnaffle2 >= distance2(*snaffle->position, *p->position)) return false;
        this->c1Position = c1->position;
        this->snafflePosition = snaffle->position;
        c1->snaffle = snaffle;
        snaffle->setCapturedByPod(c1);
        return true;
    }

    String toViewString() override {
        return u"SNAFFLE " + S(uniqueId) + u" " + D(this->time) + u" " + S(snaffle->id) + u" " + S(c1->id) + u" " + S(roundOf(c1Position, false)) + u" " +
               S(roundOf(c1Position, true)) + u" " + S(roundOf(snafflePosition, false)) + u" " + S(roundOf(snafflePosition, true));
    }
};

// a wizard's caught snaffle follows it: the snaffle's own Vector objects are written in place
void syncSnaffle(DynamicEntity* c) {
    if (c->kind == Kind::POD && static_cast<Pod*>(c)->snaffle != nullptr) {
        Snaffle* s = static_cast<Pod*>(c)->snaffle;
        s->position->x = c->position->x;
        s->position->y = c->position->y;
        s->speed->x = c->speed->x;
        s->speed->y = c->speed->y;
    }
}

struct WallCollision : Collision {
    DynamicEntity* c1;
    Side side;
    VP c1Position;
    double forceTotal = 0;
    VP speed;

    WallCollision(double time, DynamicEntity* a, Side side) : Collision(time), c1(a), side(side) {}

    bool react() override {
        if (c1->kind == Kind::SNAFFLE && static_cast<Snaffle*>(c1)->inGoal()) return true; // (without its positions)

        this->c1Position = c1->position;
        this->speed = c1->speed;

        Vec normal;
        switch (side) {
        case Side::BOTTOM:
            normal = {0, -1};
            forceTotal = 2 * c1->speed->y * c1->getMass();
            break;
        case Side::LEFT:
            normal = {-1, 0};
            forceTotal = 2 * c1->speed->x * c1->getMass();
            break;
        case Side::RIGHT:
            normal = {-1, 0};
            forceTotal = 2 * c1->speed->x * c1->getMass();
            break;
        default: // TOP
            normal = {0, -1};
            forceTotal = 2 * c1->speed->y * c1->getMass();
            break;
        }

        Vec impulse = mult(normal, forceTotal);
        c1->applyForce(time, impulse);

        double error = 0.0;
        if (c1->position->x > WIDTH - c1->radius) {
            error = c1->position->x - (WIDTH - c1->radius);
            c1->fixPosition(add(*c1->position, Vec{-2 * error, 0}));
        }
        if (c1->position->x < c1->radius) {
            error = c1->position->x - c1->radius;
            c1->fixPosition(add(*c1->position, Vec{-2 * error, 0}));
        }
        if (c1->position->y > HEIGHT - c1->radius) {
            error = c1->position->y - (HEIGHT - c1->radius);
            c1->fixPosition(add(*c1->position, Vec{0, -2 * error}));
        }
        if (c1->position->y < c1->radius) {
            error = c1->position->y - c1->radius;
            c1->fixPosition(add(*c1->position, Vec{0, -2 * error}));
        }

        syncSnaffle(c1);
        return true;
    }

    String toViewString() override {
        return u"WALL " + S(uniqueId) + u" " + D(this->time) + u" " + S(c1->id) + u" " + S(roundOf(c1Position, false)) + u" " + S(roundOf(c1Position, true)) + u" " + u"0" +
               u" " + D(std::fabs(forceTotal)) + u" " + S(roundOf(this->speed, false)) + u" " + S(roundOf(this->speed, true)) + u" " + sideName(side);
    }
};

struct EntityCollision : Collision {
    DynamicEntity* c1;
    Entity* c2;
    double forceTotal = 0;
    VP c1Position, c2Position;
    VP speed;

    EntityCollision(double time, DynamicEntity* c1, Entity* c2) : Collision(time), c1(c1), c2(c2) {}

    bool react() override {
        this->c1Position = c1->position;
        this->c2Position = c2->position;

        this->speed = c1->speed;

        Vec normal = normalize(between(*c1->position, *c2->position));
        if (normal.x == 0 && normal.y == 0) normal = {1, 0};
        Vec relativeVelocity = *c1->speed;

        double force = dot(normal, relativeVelocity) * c1->getMass();
        double repulseForce = java::max(MIN_IMPULSE, force);
        this->forceTotal = force + repulseForce;

        Vec impulse = mult(normal, -1 * this->forceTotal);
        c1->applyForce(time, impulse);

        double dist = distance(*c1->position, *c2->position);
        double diff = dist - c1->radius - c2->radius;
        if (diff <= 0) c1->fixPosition(add(*c1->position, mult(normal, -(-diff / 2 + EPSILON))));

        syncSnaffle(c1);
        return true;
    }

    String toViewString() override {
        return u"ENTITY " + S(uniqueId) + u" " + D(this->time) + u" " + S(c1->id) + u" " + S(roundOf(c1Position, false)) + u" " + S(roundOf(c1Position, true)) + u" " +
               S(c2->id) + u" " + S(roundOf(c2Position, false)) + u" " + S(roundOf(c2Position, true)) + u" " + D(forceTotal) + u" " + S(roundOf(this->speed, false)) +
               u" " + S(roundOf(this->speed, true));
    }
};

struct DynamicEntityCollision : EntityCollision {
    DynamicEntityCollision(double time, DynamicEntity* c1, DynamicEntity* c2) : EntityCollision(time, c1, c2) {}

    bool react() override {
        DynamicEntity* c2 = static_cast<DynamicEntity*>(this->c2);

        this->c1Position = c1->position;
        this->c2Position = c2->position;

        this->speed = vnew(mult(add(*c1->speed, *c2->speed), 0.5));

        Vec normal = normalize(between(*c1->position, *c2->position));
        if (normal.x == 0 && normal.y == 0) normal = {1, 0};
        Vec relativeVelocity = sub(*c1->speed, *c2->speed);

        double force = dot(normal, relativeVelocity) / (1 / c1->getMass() + 1 / c2->getMass());
        double repulseForce = java::max(MIN_IMPULSE, force);
        this->forceTotal = force + repulseForce;

        Vec impulse = mult(normal, -1 * this->forceTotal);
        c1->applyForce(time, impulse);
        c2->applyForce(time, mult(impulse, -1));

        double dist = distance(*c1->position, *c2->position);
        double diff = dist - c1->radius - c2->radius;
        if (diff <= 0) {
            c1->fixPosition(add(*c1->position, mult(normal, -(-diff / 2 + EPSILON))));
            c2->fixPosition(add(*c2->position, mult(normal, -diff / 2 + EPSILON)));
        }

        c1->hasCollidedWith(c2);
        c2->hasCollidedWith(c1);

        syncSnaffle(c1);
        syncSnaffle(c2);
        return true;
    }
};

void DynamicEntity::checkEntityCollision(DynamicEntity* other, std::vector<CP>& collisions, double beginTime, double min) {
    if (this->collidesWith(*other)) {
        collisions.push_back(std::make_shared<DynamicEntityCollision>(beginTime, this, other));
    } else {
        double colision = this->contactPosition(*other);
        if (colision <= min && colision > 0) collisions.push_back(std::make_shared<DynamicEntityCollision>(beginTime + colision, this, other));
    }
}

void DynamicEntity::checkEntityCollision(Entity* other, std::vector<CP>& collisions, double beginTime, double min) {
    if (this->collidesWith(*other)) {
        collisions.push_back(std::make_shared<EntityCollision>(beginTime, this, other));
    } else {
        double colision = this->contactPosition(*other);
        if (colision <= min && colision > 0) collisions.push_back(std::make_shared<EntityCollision>(beginTime + colision, this, other));
    }
}

void DynamicEntity::checkWallCollisions(std::vector<CP>& collisions, double beginTime, double min) {
    if (this->position->x < this->radius) {
        collisions.push_back(std::make_shared<WallCollision>(beginTime, this, Side::LEFT));
    } else if (this->position->x > WIDTH - this->radius) {
        collisions.push_back(std::make_shared<WallCollision>(beginTime, this, Side::RIGHT));
    } else if (this->position->y < this->radius) {
        collisions.push_back(std::make_shared<WallCollision>(beginTime, this, Side::TOP));
    } else if (this->position->y > HEIGHT - this->radius) {
        collisions.push_back(std::make_shared<WallCollision>(beginTime, this, Side::BOTTOM));
    } else {
        CollisionSide collision;
        if (this->contactPositionWall(collision) && collision.timeDist <= min)
            collisions.push_back(std::make_shared<WallCollision>(beginTime + collision.timeDist, this, collision.side));
    }
}

void Snaffle::checkGoalCollisions(std::vector<CP>& collisions, double beginTime, double min, Player* const* players) {
    if (this->position->x >= WIDTH) {
        collisions.push_back(std::make_shared<SnaffleScoreCollision>(this, beginTime, players[0]));
    } else if (this->position->x <= 0) {
        collisions.push_back(std::make_shared<SnaffleScoreCollision>(this, beginTime, players[1]));
    } else {
        if (std::fabs(this->speed->x) > 0) {
            double collisionRight = (WIDTH - this->position->x) / this->speed->x;
            double collisionLeft = -this->position->x / this->speed->x;
            if (collisionRight <= min && collisionRight > 0) {
                collisions.push_back(std::make_shared<SnaffleScoreCollision>(this, beginTime + collisionRight, players[0]));
            } else if (collisionLeft <= min && collisionLeft > 0) {
                collisions.push_back(std::make_shared<SnaffleScoreCollision>(this, beginTime + collisionLeft, players[1]));
            }
        }
    }
}

// the snaffles of the referee, for Pod#checkCaptureSnaffleCollision (an inner class)
std::vector<Snaffle*>* SNAFFLES = nullptr;

void Pod::checkCaptureSnaffleCollision(std::vector<CP>& collisions, double beginTime, double min, const std::vector<Pod*>& pods) {
    if (snaffle == nullptr) {
        // Find closest snaffle
        Snaffle* closestSnaffle = nullptr;
        double distMin = 0;
        for (Snaffle* snaffle : *SNAFFLES) {
            if (snaffle->canBeCapturedByPod(this)) {
                double dist2 = distance2(*this->position, *snaffle->position);
                if (dist2 <= (double)((this->radius - 1) * (this->radius - 1)) && (closestSnaffle == nullptr || dist2 < distMin)) {
                    closestSnaffle = snaffle;
                    distMin = dist2;
                }
            }
        }

        // Check collision with closest snaffle first
        if (closestSnaffle != nullptr) collisions.push_back(std::make_shared<SnaffleCollision>(closestSnaffle, beginTime, this, &pods));

        // Snaffle collision
        for (Snaffle* snaffle : *SNAFFLES) {
            if (snaffle != closestSnaffle && snaffle->canBeCapturedByPod(this)) {
                double collision = this->contactPosition(*snaffle);
                if (collision <= min && collision > 0) collisions.push_back(std::make_shared<SnaffleCollision>(snaffle, beginTime + collision, this, &pods));
            }
        }
    }
}

// --- the referee ---

// Pod#toViewString: the target is a java.awt.Point: Math.round(int) is Math.round(float)
String Pod::toViewString() {
    String pods = S(java::round(position->x)) + u" " + S(java::round(position->y)) + u" " + S(symmetricRound(speed->x)) + u" " + S(symmetricRound(speed->y)) + u" " +
                  S(power);
    if (hasTarget) {
        pods += u" " + S(java::roundFloat((float)targetX)) + u" " + S(java::roundFloat((float)targetY));
    } else if (spellTarget != nullptr) {
        pods += u" " + S(java::round(spellTarget->position->x)) + u" " + S(java::round(spellTarget->position->y));
    } else {
        pods += u" null null";
    }
    if (message) pods += u" " + *message;
    return pods;
}

void Pod::prepareRound() {
    this->acceleration = {0, 0};

    if (WITH_SPELLS) {
        cloakCountDown.dec();
        obliviateCountDown.dec();
        accioCountDown.dec();
        flipendoCountDown.dec();
        petrificusCountDown.dec();

        if (doAccio) {
            accioCountDown.setValue(ACCIO_DURATION);
            accioTarget = spellTarget;
            addToolTip(this->player->id, u"$" + S(this->player->id) + u" Accio!");
            doAccio = false;
        }
        if (doFlipendo) {
            flipendoCountDown.setValue(FLIPENDO_DURATION);
            flipendoTarget = spellTarget;
            addToolTip(this->player->id, u"$" + S(this->player->id) + u" Flipendo!");
            doFlipendo = false;
        }
        if (doPetrificus) {
            petrificusCountDown.setValue(PETRIFICUS_DURATION);
            petrificusTarget = spellTarget;
            addToolTip(this->player->id, u"$" + S(this->player->id) + u" Petrificus!");
            doPetrificus = false;
        }
        if (doObliviate) {
            obliviateCountDown.setValue(OBLIVIATE_DURATION);
            obliviateTarget = static_cast<Bludger*>(spellTarget); // (a Bludger: handlePlayerObliviate checked it)
            addToolTip(this->player->id, u"$" + S(this->player->id) + u" Obliviate!");
            doObliviate = false;
        }

        if (accioCountDown.getValue() == 0) accioTarget = nullptr;
        if (flipendoCountDown.getValue() == 0) flipendoTarget = nullptr;
        if (petrificusCountDown.getValue() == 0) petrificusTarget = nullptr;
        if (obliviateCountDown.getValue() == 0) obliviateTarget = nullptr;
    }
}

// --- the referee's regexes (Matcher#matches), hand matched. The lines never hold a line terminator (the base's
// Scanner split them there), so '.' takes any character. \s and [0-9] are ASCII; the command words are case
// sensitive and followed by exactly one ' '.

// \s+ at i: the whole whitespace run (a shorter one never helps: what follows starts with a non-space)
bool matchSpaces(const String& line, size_t& i) {
    size_t a = i;
    while (i < line.size() && java::isRegexSpace(line[i])) i++;
    return i > a;
}

// -?[0-9]{1,max} at i: the whole digit run (a longer run never matches: what the regex expects next is never a
// digit)
bool matchNumber(const String& line, size_t& i, String& group, size_t max, bool sign = true) {
    size_t n = line.size(), a = i;
    if (sign && i < n && line[i] == u'-') i++;
    size_t d = i;
    while (i < n && java::isRegexDigit(line[i])) i++;
    if (i - d < 1 || i - d > max) return false;
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
        if (line[k] != (char16_t)word[k]) return false;
    i = w;
    return true;
}

// MOVE (?<x>-?[0-9]{1,8})\s+(?<y>-?[0-9]{1,8})\s+(?<power>([0-9]{1,8}))(?:\s+(?<message>.+))?   (THROW likewise)
bool matchMoveLike(const String& line, const char* word, String& gx, String& gy, String& gpower, NString& message) {
    size_t i;
    if (!matchWord(line, word, i) || !matchNumber(line, i, gx, 8) || !matchSpaces(line, i) || !matchNumber(line, i, gy, 8) || !matchSpaces(line, i) ||
        !matchNumber(line, i, gpower, 8, false))
        return false;
    return matchMessage(line, i, message);
}

// FLIPENDO (?<id>-?[0-9]{1,3})(?:\s+(?<message>.+))?   (PETRIFICUS, ACCIO, OBLIVIATE likewise)
bool matchSpell(const String& line, const char* word, String& gid, NString& message) {
    size_t i;
    if (!matchWord(line, word, i) || !matchNumber(line, i, gid, 3)) return false;
    return matchMessage(line, i, message);
}

i32 parseInt(const String& s) {
    i32 v = 0;
    java::parseInt(s, v); // (1-8 digits: always an int)
    return v;
}

// the Java referee would loop forever here (its runner gives up after 150 s): the referee "crashes" (a note on stderr)
[[noreturn]] void loopsForever(const char* why) {
    fprintf(stderr, "cg-2016-fantastic-bits: %s: the Java referee loops forever\n", why);
    throw java::Exception{std::string("cg-2016-fantastic-bits: ") + why};
}

struct Referee;
Referee* REFEREE = nullptr;

struct Referee : legacy::MultiReferee {
    std::vector<std::unique_ptr<GoalPost>> goalPosts;
    std::vector<std::unique_ptr<Bludger>> ownedBludgers;
    std::vector<Bludger*> bludgers;
    std::vector<std::unique_ptr<Snaffle>> ownedSnaffles;
    std::vector<Snaffle*> snaffles;
    std::vector<std::unique_ptr<Pod>> ownedPods;
    std::vector<Pod*> pods;
    std::vector<std::unique_ptr<Player>> ownedPlayers;
    std::vector<Player*> players;
    i64 seed = 0;
    std::vector<CP> collisions; // the round's collisions that reacted (the view)
    java::Random random;

    Referee() {
        base = legacy::Base::fantasticBits2016();
        REFEREE = this;
        SNAFFLES = &snaffles;
    }

    void initReferee(int playerCount, java::Properties& prop) override {
        // Long.valueOf(prop.getProperty("seed", <a random long>))
        if (const String* s = prop.find(u"seed")) {
            i64 v;
            if (!java::parseLong(*s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(*s))};
            seed = v;
        } else {
            java::Random r((i64)java::entropy());
            seed = r.nextLong();
        }
        // (FRICTION... WITH_SPELLS: Double/Integer/Boolean.valueOf of their own defaults: unchanged)

        collisions.clear();

        random = java::Random(seed);
        generatePlayers(playerCount);
        generateSnaffles();
        generateBludgers();
        generateGoals();
    }

    void generateGoals() {
        goalPosts.emplace_back(new GoalPost(0, (HEIGHT - GOAL_SIZE) / 2));
        goalPosts.emplace_back(new GoalPost(0, (HEIGHT + GOAL_SIZE) / 2));
        goalPosts.emplace_back(new GoalPost(WIDTH, (HEIGHT - GOAL_SIZE) / 2));
        goalPosts.emplace_back(new GoalPost(WIDTH, (HEIGHT + GOAL_SIZE) / 2));
    }

    void generatePlayers(int playerCount) {
        int startX = WIDTH / 2, startY = HEIGHT / 2;
        for (int i = 0; i < playerCount; ++i) {
            ownedPlayers.emplace_back(new Player(i));
            players.push_back(ownedPlayers.back().get());
        }
        for (int j = 0; j < playerCount; j++) {
            Player* player = this->players[j];
            for (int i = 0; i < PODS_PER_PLAYER; ++i) {
                Vec pos{(double)(j * (WIDTH - 2000) + 1000),
                        (double)(HEIGHT / 2 + ((j % 2 == 0) ? -1 : 1) * (SPACE_BETWEEN_POD * i - (SPACE_BETWEEN_POD * (PODS_PER_PLAYER - 1)) / 2))};
                ownedPods.emplace_back(new Pod(pos, player, startX, startY));
                Pod* pod = ownedPods.back().get();
                player->pods.push_back(pod);
                pods.push_back(pod);
            }
        }
    }

    Snaffle* newSnaffle(int x, int y) {
        ownedSnaffles.emplace_back(new Snaffle(x, y, 0, 0));
        return ownedSnaffles.back().get();
    }

    void generateSnaffles() {
        int nbSnaffles = 2 + random.nextInt(2);
        SCORE_TO_WIN = nbSnaffles + 1;

        int i = 0;
        while (i < nbSnaffles) {
            int x = random.nextInt(WIDTH / 2 - 3000) + 2000;
            int y = random.nextInt(HEIGHT - 1000) + 500;

            Vec position{(double)x, (double)y};

            bool valid = true;
            for (Snaffle* f : snaffles) {
                if (distance(position, *f->position) < MIN_SPACE_BETWEEN_SNAFFLES) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                snaffles.push_back(newSnaffle(x, y));
                snaffles.push_back(newSnaffle(WIDTH - x, HEIGHT - y));
                i++;
            }
        }

        snaffles.push_back(newSnaffle(WIDTH / 2, HEIGHT / 2));
    }

    void generateBludgers() {
        if (WITH_BLUDGERS) {
            ownedBludgers.emplace_back(new Bludger(WIDTH / 2 - SNAFFLE_RADIUS - 2 * BLUDGER_RADIUS, HEIGHT / 2));
            ownedBludgers.emplace_back(new Bludger(WIDTH / 2 + SNAFFLE_RADIUS + 2 * BLUDGER_RADIUS, HEIGHT / 2));
            for (auto& b : ownedBludgers) bludgers.push_back(b.get());
        }
    }

    int getMaxRoundCount(int) override { return 200; }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        return prop;
    }

    void populateMessages(std::map<std::string, String>& p) override {
        p["endReached"] = u"End reached";
        p["podActionMove"] = u"$%d: Wizard %d moves towards coordinate (%d, %d) with thrust %d";
        p["podActionThrow"] = u"$%d: Wizard %d throws the snaffle to coordinate (%d, %d) with power %d";
        p["podActionAccio"] = u"$%d: Wizard %d casts Accio on entity %d";
        p["podActionFlipendo"] = u"$%d: Wizard %d casts Flipendo on entity %d";
        p["podActionPetrificus"] = u"$%d: Wizard %d casts Petrificus on entity %d";
        p["podActionObliviate"] = u"$%d: Wizard %d casts Obliviate on bludger %d";
    }

    Lines getInitDataForView() override {
        Lines list;
        list.push_back(S(WIDTH) + u" " + S(HEIGHT) + u" " + S(POD_RADIUS) + u" " + S((i64)this->players.size() * PODS_PER_PLAYER) + u" " + S(SNAFFLE_RADIUS) + u" " +
                       S(GOAL_SIZE));

        String podsB;
        for (Pod* p : this->pods) {
            if (p != this->pods[0]) podsB += u" ";
            podsB += S(p->player->id) + u" " + S(java::round(p->position->x)) + u" " + S(java::round(p->position->y));
        }
        list.push_back(podsB);

        String snafflesB;
        for (Snaffle* f : this->snaffles) {
            if (f != this->snaffles[0]) snafflesB += u" ";
            snafflesB += S(f->id) + u" " + S(java::round(f->position->x)) + u" " + S(java::round(f->position->y));
        }
        list.push_back(snafflesB);

        String bludgerB;
        for (Bludger* f : this->bludgers) {
            if (f != this->bludgers[0]) bludgerB += u" ";
            bludgerB += S(f->id) + u" " + S(java::round(f->position->x)) + u" " + S(java::round(f->position->y));
        }
        list.push_back(bludgerB);
        return list;
    }

    Lines getFrameDataForView(int, int, int, bool keyFrame) override {
        if (!keyFrame) return {};
        Lines list;
        for (Pod* p : this->pods) list.push_back(p->toViewString());

        Lines spells;
        auto b = [](bool v) { return String(v ? u"true" : u"false"); };
        for (Pod* p : this->pods) {
            if (p->accioCountDown.getValue() > 0)
                spells.push_back(u"ACCIO " + S(p->id) + u" " + S(p->accioTarget->id) + u" " + S(p->accioCountDown.getValue()) + u" " +
                                 b(p->accioCountDown.getValue() == ACCIO_DURATION));
            if (p->flipendoCountDown.getValue() > 0)
                spells.push_back(u"FLIPENDO " + S(p->id) + u" " + S(p->flipendoTarget->id) + u" " + S(p->flipendoCountDown.getValue()) + u" " +
                                 b(p->flipendoCountDown.getValue() == FLIPENDO_DURATION));
            if (p->obliviateCountDown.getValue() > 0)
                spells.push_back(u"OBLIVIATE " + S(p->id) + u" " + S(p->obliviateTarget->id) + u" " + S(p->obliviateCountDown.getValue()) + u" " +
                                 b(p->obliviateCountDown.getValue() == OBLIVIATE_DURATION));
            if (p->petrificusCountDown.getValue() > 0)
                spells.push_back(u"PETRIFICUS " + S(p->id) + u" " + S(p->petrificusTarget->id) + u" " + S(p->petrificusCountDown.getValue()) + u" " +
                                 b(p->petrificusCountDown.getValue() == PETRIFICUS_DURATION));
        }
        list.push_back(S((i64)spells.size()));
        list.insert(list.end(), spells.begin(), spells.end());

        list.push_back(S((i64)this->snaffles.size()));
        for (Snaffle* g : this->snaffles) list.push_back(g->toViewString());
        list.push_back(S((i64)this->bludgers.size()));
        for (Bludger* g : this->bludgers) list.push_back(g->toViewString());

        String scores;
        for (Player* p : players) {
            if (p != players[0]) scores += u" ";
            scores += S(p->score);
        }
        list.push_back(scores);

        String magic;
        for (Player* p : players) {
            if (p != players[0]) magic += u" ";
            magic += S(p->getMagic());
        }
        list.push_back(magic);

        for (const CP& c : this->collisions) list.push_back(c->toViewString());
        return list;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return PODS_PER_PLAYER; }

    String getGameName() override { return u"Fantastic Bits"; }

    Snaffle* findSnaffleById(int id) {
        for (Snaffle* snaffle : snaffles)
            if (snaffle->alive && snaffle->id == id) return snaffle;
        return nullptr;
    }

    Bludger* findBludgerById(int id) {
        for (Bludger* bludger : bludgers)
            if (bludger->id == id) return bludger;
        return nullptr;
    }

    Pod* findPodById(int id) {
        for (Pod* pod : pods)
            if (pod->id == id) return pod;
        return nullptr;
    }

    DynamicEntity* findEntityById(int id) {
        if (Snaffle* snaffle = findSnaffleById(id)) return snaffle;
        if (Bludger* bludger = findBludgerById(id)) return bludger;
        if (Pod* pod = findPodById(id)) return pod;
        return nullptr;
    }

    void handlePlayerThrow(const String& gx, const String& gy, const String& gpower, const NString& message, Pod* pod) {
        if (pod->snaffle != nullptr) {
            int x = parseInt(gx);
            int y = parseInt(gy);

            int power = parseInt(gpower);
            if (power < 0 || power > MAX_POWER_SNAFFLE) throw legacy::InvalidInputException(u"0 <= throw power <= " + S(MAX_POWER_SNAFFLE), u"power = " + S(power));
            pod->throwPower = power;

            Snaffle* snaffle = pod->snaffle;
            snaffle->position = pod->position;
            pod->hasTarget = true;
            pod->targetX = x;
            pod->targetY = y;
            if (snaffle->position->x == x && snaffle->position->y == y) {
                snaffle->acceleration = {0, 0};
            } else {
                int snafflePower = power;
                Vec direction = normalize(between(*snaffle->position, Vec{(double)x, (double)y}));
                snaffle->acceleration = mult(normalize(direction), snafflePower);
            }
            pod->lastAction = Action::THROW;
            pod->setMessage(message);
        } else {
            throw legacy::InvalidInputException(u"Impossible to throw a snaffle without holding one.", u"id = " + S(pod->id));
        }
    }

    void handlePlayerMove(const String& gx, const String& gy, const String& gpower, const NString& message, Pod* pod) {
        int x = parseInt(gx);
        int y = parseInt(gy);

        int power = parseInt(gpower);
        if (power < 0 || power > MAX_POWER) throw legacy::InvalidInputException(u"0 <= move power <= " + S(MAX_POWER), u"power = " + S(power));

        pod->hasTarget = true;
        pod->targetX = x;
        pod->targetY = y;
        pod->power = power;
        if (pod->position->x == x && pod->position->y == y) {
            pod->acceleration = {0, 0};
        } else {
            Vec direction = normalize(between(*pod->position, Vec{(double)x, (double)y}));
            pod->acceleration = mult(normalize(direction), power);
        }

        pod->lastAction = Action::MOVE;
        pod->setMessage(message);
    }

    // the four spells: the target's checks, then the cost (a failed spell costs nothing and does nothing)
    enum Spell { OBLIVIATE, ACCIO, FLIPENDO, PETRIFICUS };
    void handlePlayerSpell(Spell spell, const String& gid, const NString& message, Pod* pod) {
        int id = parseInt(gid);
        DynamicEntity* entity = findEntityById(id);
        bool* flag;
        int cost;
        Action action;
        if (spell == OBLIVIATE) {
            if (entity == nullptr) throw legacy::InvalidInputException(u"Target not found", u"id = " + S(id));
            if (entity->kind != Kind::BLUDGER) throw legacy::InvalidInputException(u"Target is not a bludger", u"id = " + S(id));
            flag = &pod->doObliviate;
            cost = OBLIVIATE_MAGIC;
            action = Action::OBLIVIATE;
        } else {
            if (entity == nullptr || entity->alive == false) throw legacy::InvalidInputException(u"Target not found", u"id = " + S(id));
            if (spell == ACCIO) {
                if (entity->kind == Kind::POD) throw legacy::InvalidInputException(u"Target can't be a player", u"id = " + S(id));
                flag = &pod->doAccio;
                cost = ACCIO_MAGIC;
                action = Action::ACCIO;
            } else {
                if (entity->kind == Kind::POD && static_cast<Pod*>(entity)->player->id == pod->player->id)
                    throw legacy::InvalidInputException(u"Target can't be yourself", u"id = " + S(id));
                flag = spell == FLIPENDO ? &pod->doFlipendo : &pod->doPetrificus;
                cost = spell == FLIPENDO ? FLIPENDO_MAGIC : PETRIFICUS_MAGIC;
                action = spell == FLIPENDO ? Action::FLIPENDO : Action::PETRIFICUS;
            }
        }
        if (pod->player->getMagic() >= cost && WITH_SPELLS) {
            pod->player->decreaseMagic(cost);
            *flag = true;
            pod->spellSuccess = 1;
        } else {
            pod->spellSuccess = 2;
        }
        pod->spellTarget = entity;
        pod->lastAction = action;
        pod->setMessage(message);
    }

    void handlePlayerOutput(int, int, int playerIdx, const Lines& outputs) override {
        size_t i = 0;
        Player* player = this->players[playerIdx];
        for (const String& line : outputs) {
            // player.pods.get(i) (the runner sends 2 lines)
            if (i >= player->pods.size()) throw java::Exception{java::indexOutOfBoundsException((i64)i, (i64)player->pods.size())};
            Pod* pod = player->pods[i];
            String gx, gy, gpower, gid;
            NString message;
            try {
                pod->lastAction = Action::IDLE;
                pod->hasTarget = false;

                if (matchMoveLike(line, "MOVE ", gx, gy, gpower, message)) {
                    handlePlayerMove(gx, gy, gpower, message, pod);
                } else if (matchMoveLike(line, "THROW ", gx, gy, gpower, message)) {
                    handlePlayerThrow(gx, gy, gpower, message, pod);
                } else if (matchSpell(line, "ACCIO ", gid, message)) {
                    handlePlayerSpell(ACCIO, gid, message, pod);
                } else if (matchSpell(line, "OBLIVIATE ", gid, message)) {
                    handlePlayerSpell(OBLIVIATE, gid, message, pod);
                } else if (matchSpell(line, "FLIPENDO ", gid, message)) {
                    handlePlayerSpell(FLIPENDO, gid, message, pod);
                } else if (matchSpell(line, "PETRIFICUS ", gid, message)) {
                    handlePlayerSpell(PETRIFICUS, gid, message, pod);
                } else {
                    throw legacy::InvalidInputException(u"MOVE x y thrust | THROW x y power | FLIPENDO id | ACCIO id | OBLIVIATE id | PETRIFICUS id", line);
                }

                if (pod->snaffle != nullptr) {
                    Snaffle* snaffle = pod->snaffle;
                    snaffle->speed = pod->speed;
                    snaffle->position = pod->position;
                    pod->timeBeforeCanCaptureAgain = TIME_BEFORE_CAPTURE_AGAIN;
                }
            } catch (const legacy::InvalidInputException&) {
                player->setDead(true);
                throw;
            }
            ++i;
        }
    }

    Lines getInitInputForPlayer(int playerIdx) override { return {S(playerIdx)}; }

    Lines getInputForPlayer(int, int playerIdx) override {
        Lines data;
        Player* player = this->players[playerIdx];

        for (Pod* p : this->pods)
            if (p->player == player) data.push_back(p->toPlayerString(true));
        for (Pod* p : this->pods)
            if (p->player != player) data.push_back(p->toPlayerString(false));
        for (Snaffle* snaffle : this->snaffles)
            if (snaffle->alive) data.push_back(snaffle->toPlayerString());
        for (Bludger* bludger : this->bludgers) data.push_back(bludger->toPlayerString());

        data.insert(data.begin(), S((i64)data.size()));
        if (PUZZLE) {
            Player* opponent = this->players[(playerIdx + 1) % 2];
            data.insert(data.begin(), {S(player->score) + u" " + S(player->magic), S(opponent->score) + u" " + S(opponent->magic)});
        }
        return data;
    }

    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }

    void prepare(int) override {
        for (Player* p : players)
            for (Pod* pod : p->pods) {
                pod->reset();
                pod->prepareRound();
            }
        for (Snaffle* f : snaffles) {
            f->reset();
            f->prepareRound();
        }
        for (Bludger* b : bludgers) {
            b->reset();
            b->prepareRound();
        }
        this->collisions.clear();
    }

    void updateGame(int) override {
        std::vector<Pod*> pods;
        for (Player* p : players) pods.insert(pods.end(), p->pods.begin(), p->pods.end());

        for (Snaffle* snaffle : snaffles) {
            std::vector<Snaffle*>& l = snaffle->ignoreCollisionList;
            for (size_t k = 0; k < l.size();) {
                if (!l[k]->collidesWith(*snaffle)) l.erase(l.begin() + k);
                else k++;
            }
        }
        for (Pod* p : pods) {
            p->applyForce(p->acceleration);
            if (p->snaffle != nullptr) {
                if (p->accioTarget == p->snaffle) {
                    p->accioCountDown.setValue(0);
                    p->accioTarget = nullptr;
                }
                for (Snaffle* snaffle : snaffles) {
                    if (snaffle != p->snaffle && p->snaffle->collidesWith(*snaffle)) {
                        snaffle->ignoreCollisionWith(p->snaffle);
                        p->snaffle->ignoreCollisionWith(snaffle);
                    }
                }
                p->releaseSnaffle();
            }
            if (p->accioTarget != nullptr && !p->accioTarget->alive) {
                p->accioCountDown.setValue(0);
                p->accioTarget = nullptr;
            }
            if (p->timeBeforeCanCaptureAgain > 0) p->timeBeforeCanCaptureAgain--;
        }
        int aliveSnaffles = 0;
        for (Snaffle* f : snaffles) {
            if (f->alive) aliveSnaffles++;
            f->applyForce(f->acceleration);
        }
        for (Bludger* b : bludgers) {
            b->updateAcceleration(pods);
            b->applyForce(b->acceleration);
        }

        // Apply spells (order is important):
        if (WITH_SPELLS) {
            for (Pod* pod : pods) pod->applyPetrificus();
            for (Pod* pod : pods) pod->applyAccio();
            for (Pod* pod : pods) pod->applyFlipendo();
        }

        for (Player* player : players) player->increaseMagic();

        double time = 0;
        long long steps = 0;
        while (time < 1) {
            // (the Java referee would loop until the runner gives up: never seen)
            if (++steps > 1000000) loopsForever("more than 10^6 collision steps in a round");
            double step = this->step(pods, time);
            time += step;
        }

        for (Pod* p : pods) p->endRound();
        for (Snaffle* f : snaffles) f->endRound();
        for (Bludger* b : bludgers) b->endRound();

        bool ended = false;
        if (aliveSnaffles == 0 || (players[0]->score >= SCORE_TO_WIN || players[1]->score >= SCORE_TO_WIN) || players[0]->dead || players[1]->dead) ended = true;
        if (ended) throw legacy::GameOverException(std::string("endReached"));
    }

    void stepAllEntities(double step) {
        for (Pod* ch : pods) ch->step(step);
        for (Snaffle* f : snaffles) f->step(step);
        for (Bludger* b : bludgers) b->step(step);
    }

    double step(const std::vector<Pod*>& pods, double beginTime) {
        std::vector<CP> collisions;
        double min = 1 - beginTime;

        // physics loop with walls and other pods
        for (int i = (int)pods.size() - 1; i >= 0; --i) {
            Pod* pod = pods[i];
            pod->checkWallCollisions(collisions, beginTime, min);
            for (auto& goalPost : goalPosts) pod->checkEntityCollision(static_cast<Entity*>(goalPost.get()), collisions, beginTime, min);
            // Collision with other pods
            for (int j = i - 1; j >= 0; --j) pod->checkEntityCollision(static_cast<DynamicEntity*>(pods[j]), collisions, beginTime, min);
            pod->checkCaptureSnaffleCollision(collisions, beginTime, min, pods);
        }

        for (int i = (int)snaffles.size() - 1; i >= 0; --i) {
            Snaffle* snaffle = snaffles[i];
            if (snaffle->alive && !snaffle->captured) {
                for (auto& goalPost : goalPosts) snaffle->checkEntityCollision(static_cast<Entity*>(goalPost.get()), collisions, beginTime, min);
                snaffle->checkWallCollisions(collisions, beginTime, min);
                // Collision with other snaffles
                for (int j = i - 1; j >= 0; --j) {
                    Snaffle* snaffle2 = snaffles[j];
                    if (!snaffle2->alive || snaffle2->captured || snaffle->ignores(snaffle2)) continue;
                    snaffle->checkEntityCollision(static_cast<DynamicEntity*>(snaffle2), collisions, beginTime, min);
                }
                snaffle->checkGoalCollisions(collisions, beginTime, min, players.data());
            }
        }

        for (int i = (int)bludgers.size() - 1; i >= 0; --i) {
            Bludger* bludger = bludgers[i];
            bludger->checkWallCollisions(collisions, beginTime, min);
            for (auto& goalPost : goalPosts) bludger->checkEntityCollision(static_cast<Entity*>(goalPost.get()), collisions, beginTime, min);
            for (int j = i - 1; j >= 0; --j) bludger->checkEntityCollision(static_cast<DynamicEntity*>(bludgers[j]), collisions, beginTime, min);
            for (Pod* pod : pods) bludger->checkEntityCollision(static_cast<DynamicEntity*>(pod), collisions, beginTime, min);
            for (Snaffle* snaffle : snaffles) bludger->checkEntityCollision(static_cast<DynamicEntity*>(snaffle), collisions, beginTime, min);
        }

        if (!collisions.empty()) {
            java::sort(collisions, [](const CP& a, const CP& b) { return java::compareDouble(a->time, b->time); });
            double time = -1;
            bool reacted = false;
            for (const CP& c : collisions) {
                if (time <= -1) {
                    time = c->time;
                    double step = time - beginTime;
                    stepAllEntities(step);
                }
                if (c->time <= time + EPSILON) {
                    if (c->react()) {
                        this->collisions.push_back(c);
                        reacted = true;
                    }
                } else {
                    break;
                }
            }
            // nothing moved and nothing reacted (only failed captures): the next step is this one again, forever
            if (!(time - beginTime > 0) && !reacted) loopsForever("the collision loop does not move on");
            return time - beginTime;
        }

        double step = 1 - beginTime;
        stepAllEntities(step);
        return step;
    }

    Lines getPlayerActions(int playerIdx, int) override {
        Lines action;
        for (Pod* p : this->players[playerIdx]->pods) {
            switch (p->lastAction) {
            case Action::MOVE:
                action.push_back(translate("podActionMove", {p->player->id, p->id, p->targetX, p->targetY, p->power}));
                break;
            case Action::THROW:
                action.push_back(translate("podActionThrow", {p->player->id, p->id, p->targetX, p->targetY, p->throwPower}));
                break;
            case Action::ACCIO: action.push_back(translate("podActionAccio", {p->player->id, p->id, p->spellTarget->id})); break;
            case Action::FLIPENDO: action.push_back(translate("podActionFlipendo", {p->player->id, p->id, p->spellTarget->id})); break;
            case Action::OBLIVIATE: action.push_back(translate("podActionObliviate", {p->player->id, p->id, p->spellTarget->id})); break;
            case Action::PETRIFICUS: action.push_back(translate("podActionPetrificus", {p->player->id, p->id, p->spellTarget->id})); break;
            case Action::NONE: throw java::Exception{"java.lang.NullPointerException"}; // (switch on a null enum)
            default: break;
            }
        }
        return action;
    }

    bool isPlayerDead(int playerIdx) override { return this->players[playerIdx]->dead; }
    NString getDeathReason(int) override { return String(u"Eliminated!"); }
    int getScore(int playerIdx) override { return players[playerIdx]->score; }

    Lines getGameSummary(int) override {
        Lines summary;
        for (Player* p : players) {
            String sum = u"$" + S(p->id) + u" Score: " + S(p->score);
            if (WITH_SPELLS) sum += u" | Magic: " + S(p->magic);
            summary.push_back(sum);
            for (Pod* pod : p->pods) {
                if (pod->snaffle != nullptr) summary.push_back(u"$" + S(p->id) + u" " + S(pod->id) + u" caught snaffle " + S(pod->snaffle->id));
                if (pod->spellSuccess == 2) summary.push_back(u"$" + S(p->id) + u": Wizard " + S(pod->id) + u" failed his spell!");
            }
        }
        return summary;
    }

    // the game's gameOver() (abstract in this base, never called: Base::gameOverHook false)
    bool gameOver() override { return false; }

    void setPlayerTimeout(int, int, int playerIdx) override { this->players[playerIdx]->setDead(true); }
};

void addToolTip(int player, const String& message) { REFEREE->addToolTip(player, message); }

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
