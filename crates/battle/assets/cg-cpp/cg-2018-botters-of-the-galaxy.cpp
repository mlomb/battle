// Botters of the Galaxy - headless C++ referee, port of https://github.com/Illedan/BOTG-Refree
// (CodinGame game engine 1.20, highest league: every rule enabled).
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints "<p0score>\n<p1score>\n" to stdout (matches the jar).
//
// The Java referee is followed very closely (same data structures and the same order of
// floating point operations), since tiny differences in the event simulation snowball quickly.
// Notable Java behaviours reproduced here:
//   - java.util.Random, (int) casts, Double.parseDouble, Integer.parseInt, String.split
//   - java.util.HashMap iteration order (items are sent in that order, item costs are summed in it)
//   - TimSort on the (inconsistent) comparator used by creatures to pick their target
//   - each bush consumes a random number for its sprite rotation (ViewController.addObstacle)
//   - Agent#getOutput from the engine: '\r' and '\n' terminate lines, 10000 bytes per turn max,
//     1000ms for the first turn of each player and 50ms afterwards
//   - the viewer crashes the Java referee in some cases (see ViewGroup), we then exit with code 1
//     and no scores, like the jar does

// Java never fuses a*b+c, so we must not either (zig c++ -march=native would emit FMAs)
#pragma STDC FP_CONTRACT OFF
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#include "common/agent.h"
#include "common/java.h"

#include <charconv>
#include <deque>
#include <limits>
#include <system_error>

// ----------------------------------------------------------------------------
// Constants (Const.java, highest league)
// ----------------------------------------------------------------------------
namespace C {
    const double EPSILON = 0.00001;
    const double ROUNDTIME = 1.0;
    const int ROUNDS = 250;
    const int MAPWIDTH = 1920;
    const int MAPHEIGHT = 780;
    const int HEROCOUNT = 2;
    const int MAXITEMCOUNT = 4;
    const int MELEE_UNIT_COUNT = 3;
    const int RANGED_UNIT_COUNT = 1;
    const double SELLITEMREFUND = 0.5;
    const int MAXMOVESPEED = 450;
    const int SPAWNRATE = 15;
    const int AGGROUNITRANGE = 300;
    const int AGGROUNITRANGE2 = AGGROUNITRANGE * AGGROUNITRANGE;
    const int AGGROUNITTIME = 3;
    const double DENYHEALTH = 0.4;
    const double BUSHRADIUS = 50;
    const int TOWERHEALTH = 3000;
    const double TOWERHEALTHSCALE = 1.0;
    const int TOWERDAMAGE = 190;
    const int NEUTRALSPAWNTIME = 4;
    const int NEUTRALSPAWNRATE = 40;
    const int NEUTRALGOLD = 100;
    const double EXPLOSIVESHIELDRANGE2 = 151 * 151;
    const int EXPLOSIVESHIELDDAMAGE = 50;
    const double POWERUPDAMAGEINCREASE = 0.3;
    const int MELEE_UNIT_GOLD_VALUE = 30;
    const int RANGER_UNIT_GOLD_VALUE = 50;
    const int HERO_GOLD_VALUE = 300;
    const int MINIMUM_STAT_COST = 30;
    const int NB_ITEMS_PER_LEVEL = 5;
    const int MAX_INT = INT32_MAX;
    const double MAX_DOUBLE = std::numeric_limits<double>::max(); // not DBL_MAX: undefined when zig builds on macOS
    const int LOST_SCORE = -100000000;

    // GameManager defaults (engine 1.20)
    const int FIRST_TURN_MAX_TIME = 1000;
    const int TURN_MAX_TIME = 50;

    // item stats, Const.STATS order
    enum Stat { DAMAGE, HEALTH, MAXHEALTH, MANA, MAXMANA, MOVESPEED, MANAREGEN, N_STATS };
    const char* const STAT_NAMES[N_STATS] = {"damage", "health", "maxHealth", "mana", "maxMana", "moveSpeed", "manaregeneration"};
    const Stat STATS[5] = {DAMAGE, HEALTH, MANA, MOVESPEED, MANAREGEN};
    const int STAT_LIMIT[N_STATS] = {100000000, 2500, 2500, 100, 100, 150, 50};
    const double STAT_PRICE[N_STATS] = {7.2, 0.7, 0, 0.5, 0, 3.6, 50};
    const int ITEM_LEVELS[4][2] = {{50, 200}, {200, 550}, {550, 1300}, {1300, 3000}};
}

struct Point {
    double x, y;
};

static inline double distance2(const Point& a, const Point& b) {
    return ((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}
static inline double distance(const Point& a, const Point& b) { return std::sqrt(distance2(a, b)); }

const Point TOWERTEAM0 = {100, 540};
const Point SPAWNTEAM0 = {TOWERTEAM0.x + 60, TOWERTEAM0.y - 50};
const Point HEROSPAWNTEAM0 = {TOWERTEAM0.x + 100, TOWERTEAM0.y + 50};
const Point HEROSPAWNTEAM0HERO2 = {HEROSPAWNTEAM0.x, TOWERTEAM0.y - 50};
const Point TOWERTEAM1 = {C::MAPWIDTH - TOWERTEAM0.x, TOWERTEAM0.y};
const Point SPAWNTEAM1 = {C::MAPWIDTH - SPAWNTEAM0.x, SPAWNTEAM0.y};
const Point HEROSPAWNTEAM1 = {C::MAPWIDTH - HEROSPAWNTEAM0.x, HEROSPAWNTEAM0.y};
const Point HEROSPAWNTEAM1HERO2 = {HEROSPAWNTEAM1.x, HEROSPAWNTEAM0HERO2.y};

struct MovingEntity : Point {
    double vx = 0, vy = 0;
    double forceVX = 0, forceVY = 0;

    void moveIgnoreEdges(double t) {
        x += vx * t;
        y += vy * t;
    }
    // MovingEntity.move(x, y)
    void moveTo(double nx, double ny) {
        x = nx;
        y = ny;
        if (x < 30) x = 30;
        if (y < 30) y = 30;
        if (x >= C::MAPWIDTH - 30) x = C::MAPWIDTH - 30;
        if (y >= C::MAPHEIGHT - 30) y = C::MAPHEIGHT - 30;
    }
};

// ----------------------------------------------------------------------------
// Entities
// ----------------------------------------------------------------------------
struct Player;
struct Unit;

struct Item {
    std::string name;
    int stats[C::N_STATS] = {0};
    int cost;
    bool isPotion;

    std::string getPlayerString() const {
        std::string s = name + " " + std::to_string(cost);
        for (int st = 0; st < C::N_STATS; st++) s += " " + std::to_string(stats[st]);
        return s + " " + (isPotion ? "1" : "0");
    }
};

enum SkillKind {
    SK_EMPTY, SK_BLINK, SK_JUMP, SK_FLIP, SK_POWERUP, SK_AOEHEAL, SK_BURNING, SK_SHIELD, SK_PULL,
    SK_CHARGE, SK_BASH, SK_EXPLOSIVE, SK_COUNTER, SK_STEALTH, SK_WIRE, SK_LINE
};
enum SkillType { SELF, UNIT, POSITION };

struct Skill {
    SkillKind kind;
    Unit* hero;
    int manaCost;
    std::string skillName;
    int range;
    int cooldown = 0;
    int initialCooldown;
    double duration = 1;
    // skill specific
    double radius = 0; // AOEHEAL/BURNING (int in Java), WIRE, FIREBALL
    double delay = 0;  // PULL
    int stunTime = 0;  // WIRE
    double flyTime = 0; // WIRE

    SkillType getTargetType() const {
        switch (kind) {
            case SK_BLINK: case SK_JUMP: case SK_AOEHEAL: case SK_BURNING: case SK_STEALTH: case SK_WIRE: case SK_LINE:
                return POSITION;
            case SK_FLIP: case SK_SHIELD: case SK_PULL: case SK_CHARGE: case SK_BASH:
                return UNIT;
            default:
                return SELF;
        }
    }
};

enum UnitKind { K_TOWER, K_LANE, K_CREATURE, K_HERO };
enum CreatureState { PEACEFULL, RUNNINGBACK, AGGRESSIVE };

struct ViewGroup;

struct Unit : MovingEntity {
    UnitKind kind;
    int id;
    int health;
    int stunTime = 0;
    bool visible = true;
    bool invisibleBySkill = false;
    int team;
    int damage = 0;
    int moveSpeed;
    int range = 0;
    bool isDead = false;
    double attackTime = 0.1;
    int goldValue = 0;
    int maxHealth;
    Player* player;
    int shield = 0;
    int explosiveShield = 0;
    bool moving = false;
    bool becomingInvis = false;

    // Hero
    Skill* skills[3] = {nullptr, nullptr, nullptr};
    std::string heroType;
    int mana = 0;
    int manaregeneration = 0;
    int maxMana = 0;
    int visibilityTimer = 0;
    std::vector<Item*> items;

    // LaneUnit / Tower
    Point targetPoint = {0, 0};
    Unit* aggroUnit = nullptr;
    int aggroTimeLeft = 0;
    double aggroTset = 0;

    // Creature
    Point camp = {0, 0};
    CreatureState state = PEACEFULL;

    // sprite group in the viewer, null once killed (ViewController.spriteMap)
    ViewGroup* view = nullptr;

    bool isHero() const { return kind == K_HERO; }
    bool isMelee() const { return range <= 150; }
    int getShield() const { return shield + explosiveShield; }

    const char* getType() const {
        switch (kind) {
            case K_TOWER: return "TOWER";
            case K_LANE: return "UNIT";
            case K_CREATURE: return "GROOT";
            default: return "HERO";
        }
    }

    void adjustShield(int val) {
        // remove min first
        if (shield < explosiveShield) {
            int toRemove = std::min(val, shield);
            val -= toRemove;
            shield -= toRemove;
            explosiveShield -= val;
        } else {
            int toRemove = std::min(val, explosiveShield);
            val -= toRemove;
            explosiveShield -= toRemove;
            shield -= val;
        }
    }

    // Unit.move(t)
    void move(double t) {
        if (isDead) return;
        moveTo(x + vx * t, y + vy * t);
    }

    std::string getPlayerString() const {
        std::string s = std::to_string(id) + " " + std::to_string(team) + " " + getType() + " " +
                        std::to_string(java::d2i(x)) + " " + std::to_string(java::d2i(y)) + " " +
                        std::to_string(range) + " " + std::to_string(health) + " " + std::to_string(maxHealth) + " " +
                        std::to_string(getShield()) + " " + std::to_string(damage) + " " + std::to_string(moveSpeed) + " " +
                        std::to_string(stunTime) + " " + std::to_string(goldValue) + " ";
        if (!isHero()) return s + "0 0 0 0 0 0 - 1 0";
        return s + std::to_string(skills[0]->cooldown) + " " + std::to_string(skills[1]->cooldown) + " " +
               std::to_string(skills[2]->cooldown) + " " + std::to_string(mana) + " " + std::to_string(maxMana) + " " +
               std::to_string(manaregeneration) + " " + heroType + " " + (visible ? "1" : "0") + " " +
               std::to_string(items.size());
    }
};

struct Player {
    int index;
    std::vector<Unit*> heroes;
    Unit* tower = nullptr;
    int gold = 0;
    int denies = 0;
    int unitKills = 0;
    // AbstractPlayer
    bool active = true;
    int score = 0;
    bool hasNeverBeenExecuted = true;

    int heroesAlive() const {
        int alive = 0;
        for (Unit* hero : heroes)
            if (!hero->isDead) alive++;
        return alive;
    }
    int getExpectedOutputLines() const {
        if ((int)heroes.size() < C::HEROCOUNT) return 1;
        return heroesAlive();
    }
};

struct Damage {
    Unit* target;
    Unit* attacker;
    int damage;
};

// ----------------------------------------------------------------------------
// Events (Event.java)
// ----------------------------------------------------------------------------
enum Outcome { NONE = 0, LIFECHANGED = 1, SPEEDCHANGED = 2, TELEPORTED = 4, STUNNED = 8 };

enum EventKind {
    EV_ON_LOCATION_REACHED, EV_SPEED_CHANGED_FORCE, EV_SPEED_CHANGED, EV_DELAYED_ATTACK, EV_DAMAGE, EV_COUNTER,
    EV_STUN, EV_BLINK, EV_ATTACK_NEAREST_DELAYED, EV_POWER_UP, EV_STEALTH, EV_HEALTH_CHANGE, EV_SHIELD,
    EV_EXPLOSIVE_SHIELD, EV_EXPLOSION, EV_DRAIN_MANA, EV_LINE_EFFECT, EV_WIRE, EV_ATTACK_MOVE, EV_ATTACK_MOVE_UNIT
};

struct Event {
    EventKind kind;
    double t;
    Unit* unit;

    double vx = 0, vy = 0;          // SpeedChanged, SpeedChangedForce
    Unit* attacker = nullptr;       // DelayedAttack, Damage, DrainMana, LineEffect, Wire
    int damage = 0;                 // Damage, PowerUp, LineEffect
    int health = 0;                 // Counter
    int range = 0;                  // Counter, PowerUp, HealthChange
    int stunTime = 0;               // Stun, Wire
    double x = 0, y = 0;            // Blink, Explosion
    int moveSpeed = 0;              // PowerUp
    int rounds = 0;                 // PowerUp, Shield, ExplosiveShield
    int dHealth = 0;                // HealthChange
    Point targetPos = {0, 0};       // HealthChange, AttackMove
    bool hitEnemies = false;        // HealthChange
    int manaToDrain = 0;            // DrainMana
    MovingEntity* movingSpell = nullptr; // LineEffect, Wire
    int damageByTime = 0;           // LineEffect
    double radius = 0, duration = 0, dmgMultiplier = 0; // LineEffect, Wire
    std::vector<Unit*> potentialTargets; // Wire
    Unit* nextTarget = nullptr;     // AttackMoveUnit
};

// Units affected by an event: either none or a single (possibly null) unit
struct Affected {
    bool any;
    Unit* unit;
};
static const Affected EMPTYLIST = {false, nullptr};

// ----------------------------------------------------------------------------
// Game state (Game.java + the statics of Const.java)
// ----------------------------------------------------------------------------
struct Bush : Point {
    double radius;
};

struct Game {
    java::Random random;
    int globalId = 1;

    std::vector<Unit*> allUnits;
    std::vector<Bush> bushes;
    std::vector<Event*> events;
    std::vector<Item*> items; // HashMap iteration order
    int round = 0;
    std::vector<Unit*> visibleHeroes;
    int forestCampCount = 0;
    // HashMap<Unit, List<Damage>>, iterated in insertion order (see handleDamage)
    std::vector<std::pair<Unit*, std::vector<Damage>>> damages;
    double t = 0;

    std::vector<Point> spawns;
    std::vector<Unit*> creatures;
    std::vector<double> amplitude;

    // object pools, Java objects are never freed while referenced
    std::deque<Unit> unitPool;
    std::deque<Event> eventPool;
    std::deque<Skill> skillPool;
    std::deque<Item> itemPool;
    std::deque<MovingEntity> entityPool;
};

static Game G;
static Player players[2];

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (auto& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

static Unit* getUnitOfId(int id) {
    for (Unit* unit : G.allUnits)
        if (unit->id == id) return unit;
    return nullptr;
}

static Unit* newUnit(UnitKind kind, double x, double y, int health, int team, int moveSpeed, Player* player) {
    G.unitPool.emplace_back();
    Unit* u = &G.unitPool.back();
    u->kind = kind;
    u->x = x;
    u->y = y;
    u->id = G.globalId++;
    u->team = team;
    u->health = u->maxHealth = health;
    u->moveSpeed = moveSpeed;
    u->player = player;
    return u;
}

static Event* newEvent(EventKind kind, Unit* unit, double t) {
    G.eventPool.emplace_back();
    Event* e = &G.eventPool.back();
    e->kind = kind;
    e->unit = unit;
    e->t = t;
    return e;
}

static void removeEvent(Event* e) {
    // ArrayList.remove(Object): first occurrence
    auto it = std::find(G.events.begin(), G.events.end(), e);
    if (it != G.events.end()) G.events.erase(it);
}

// ----------------------------------------------------------------------------
// Viewer state (ViewController.java, AnimationGroup.java, AnimationGroupFactory.java)
// ----------------------------------------------------------------------------
// Events are processed at t + EPSILON, so a unit can die slightly after t = 1.0. When that happens,
// the Java referee crashes with "IllegalArgumentException: Not a valid frame instant" if the sprite
// group of the unit has something to commit at that time: the outline of heroes, a stun indicator
// or the health bar of towers and groots. Sprite groups of lane units and groots are pooled and
// reused with the state they had, so we track just enough of them to crash at the same time.
struct ViewGroup {
    Unit* unit;
    bool withHealth; // AnimationGroupWithHealth (towers and groots)
    bool outline;    // heroes
    int prevStun = 0;
    bool stunIndicator = false;
    int prevHealth = 0;
    bool healthBar = false;
};

struct RefereeCrash {
    double t;
};

static std::deque<ViewGroup> viewGroupPool;
static std::vector<ViewGroup*> animationGroups; // never shrinks, killed groups stay
static std::deque<ViewGroup*> raccoonFactory[2];
static std::deque<ViewGroup*> grootFactory;

static void updateStunIndicator(ViewGroup* g) {
    if (g->unit->stunTime == 0 && g->stunIndicator) {
        g->stunIndicator = false;
        g->prevStun = 0;
        return;
    }
    if (g->unit->stunTime == g->prevStun || g->unit->isDead) return;
    g->prevStun = g->unit->stunTime;
    g->stunIndicator = true;
}

// ViewController.addSprite
static void viewAddSprite(Unit* unit) {
    ViewGroup* g;
    if (unit->kind == K_LANE && !raccoonFactory[unit->team].empty()) {
        g = raccoonFactory[unit->team].front();
        raccoonFactory[unit->team].pop_front();
        g->unit = unit;
        updateStunIndicator(g);
    } else if (unit->kind == K_CREATURE && !grootFactory.empty()) {
        g = grootFactory.front();
        grootFactory.pop_front();
        g->unit = unit;
    } else {
        viewGroupPool.emplace_back();
        g = &viewGroupPool.back();
        g->unit = unit;
        g->withHealth = unit->kind == K_TOWER || unit->kind == K_CREATURE;
        g->outline = unit->isHero();
        g->prevHealth = unit->health;
        animationGroups.push_back(g);
    }
    unit->view = g;
}

// ViewController.addEffect(unit, null, "stun", ...)
static void viewStun(Unit* unit) {
    if (unit->view) updateStunIndicator(unit->view);
}

// ViewController.killUnit
static void viewKill(Unit* unit, double t) {
    ViewGroup* g = unit->view;
    if (g == nullptr) return;
    // AnimationGroup.kill / AnimationGroupWithHealth.kill
    if (g->stunIndicator) {
        if (t > 1) throw RefereeCrash{t};
        g->stunIndicator = false;
    }
    if (g->outline && t > 1) throw RefereeCrash{t};
    if (g->withHealth && g->healthBar && t > 1) throw RefereeCrash{t};

    unit->view = nullptr;
    if (unit->kind == K_CREATURE) grootFactory.push_back(g);
    if (unit->kind == K_LANE) raccoonFactory[unit->team].push_back(g);
}

// ViewController.afterRound
static void viewAfterRound() {
    for (ViewGroup* g : animationGroups) {
        updateStunIndicator(g);
        if (g->withHealth && g->unit->health != g->prevHealth) {
            g->healthBar = true;
            if (!g->unit->isDead) g->prevHealth = g->unit->health;
        }
    }
}

static inline bool hasOutcome(int outcome, int expected) { return (outcome & expected) != 0; }
static inline bool hasAnyOutcome(int outcome, int e1, int e2) { return hasOutcome(outcome, e1) || hasOutcome(outcome, e2); }

// ----------------------------------------------------------------------------
// Utilities.java
// ----------------------------------------------------------------------------
static bool isNumber(const std::string& s) {
    double d;
    if (!java::parseDouble(s, d)) return false;
    if (std::isinf(d)) return false;
    if (std::isnan(d)) return false;
    return true;
}

static int rndInt(int min, int max) { return java::d2i(G.random.nextDouble() * (max - min) + min); }

static int roundJ(double x) {
    int s = x < 0 ? -1 : 1;
    return s * java::d2i((s * x) + 0.5);
}

static double timeToReachTarget(const Point& start, const Point& stop, double speed) {
    return distance(start, stop) / speed;
}

static double getCollisionTime(const MovingEntity& entity, const MovingEntity& entity1, double radius) {
    // Check instant collision
    if (distance(entity, entity1) <= radius) return 0.0;

    // Fixes rounding errors.
    radius -= C::EPSILON;

    // Both units are motionless
    if (entity.vx == 0.0 && entity.vy == 0.0 && entity1.vx == 0.0 && entity1.vy == 0.0) return -1;

    double x2 = entity.x - entity1.x;
    double y2 = entity.y - entity1.y;
    double r2 = radius;
    double vx2 = entity.vx - entity1.vx;
    double vy2 = entity.vy - entity1.vy;

    double a = vx2 * vx2 + vy2 * vy2;
    if (a <= 0.0) return -1;

    double b = 2.0 * (x2 * vx2 + y2 * vy2);
    double c = x2 * x2 + y2 * y2 - r2 * r2;
    double delta = b * b - 4.0 * a * c;
    if (delta < 0.0) return -1;

    double t = (-b - std::sqrt(delta)) / (2.0 * a);
    if (t <= 0.0) return -1;
    return t;
}

// ----------------------------------------------------------------------------
// Unit behaviour (Unit.java, LaneUnit.java, Tower.java, Creature.java)
// ----------------------------------------------------------------------------
static Event* newDamageEvent(Unit* unit, Unit* attacker, double t, int damage);
static Event* newOnLocationReachedEvent(Unit* unit, double t);
static Event* newAttackMoveEvent(Unit* unit, const Point& targetPos);
static Event* newAttackMoveUnitEvent(Unit* unit, Unit* targetUnit);

static bool allowedToTarget(const Unit* self, const Unit* unit) {
    if (unit == nullptr) return false;
    if (self->isDead) return false;
    if (self->stunTime > 0) return false;
    if (unit->team != self->team && !unit->visible && !(self->kind == K_TOWER || self->kind == K_CREATURE)) return false;
    return true;
}

static bool allowedToAttack(const Unit* self, const Unit* unit) {
    if (!allowedToTarget(self, unit)) return false;
    if (unit == self) return false;
    if (unit->isDead || unit->health <= 0) return false;
    return true;
}

static bool canAttack(const Unit* self, const Unit* unit) {
    if (!allowedToAttack(self, unit)) return false;
    if (unit->team == self->team && unit->health > unit->maxHealth * C::DENYHEALTH) return false;
    if (distance2(*self, *unit) > self->range * self->range) return false;
    return true;
}

static void fireAttack(Unit* self, Unit* unit) {
    if (!canAttack(self, unit)) return;

    double attackTravelTime = java::min(1.0, self->attackTime + (self->isMelee() ? 0 : self->attackTime * distance(*self, *unit) / self->range));
    if (G.t + attackTravelTime > 1.0) {
        return; // no attacks cross rounds.
    }

    G.events.push_back(newDamageEvent(unit, self, attackTravelTime, self->damage));
    // Creep aggro.
    if (self->isHero() && unit->isHero()) {
        for (Unit* u : G.allUnits) {
            if (u->team != 1 - self->team) continue;
            if ((u->kind == K_LANE || u->kind == K_TOWER) && distance2(*self, *u) < C::AGGROUNITRANGE2) {
                Unit* peasant = u;
                if (peasant->aggroTimeLeft < C::AGGROUNITTIME) {
                    peasant->aggroUnit = self;
                    peasant->aggroTset = G.t;
                } else if (peasant->aggroTset == G.t && peasant->aggroUnit != nullptr &&
                           distance2(*peasant->aggroUnit, *unit) > distance2(*self, *unit)) {
                    peasant->aggroUnit = self;
                }
                peasant->aggroTimeLeft = C::AGGROUNITTIME;
            }
        }
    }
}

static Unit* findClosestOnOtherTeam(const Unit* self, const char* filter) {
    Unit* closest = nullptr;
    bool useFilter = strcmp(filter, "none") != 0;
    bool groot = strcmp(filter, "GROOT") == 0;
    double minDist = C::MAX_DOUBLE;
    for (Unit* unit : G.allUnits) {
        if (useFilter && strcmp(unit->getType(), filter) != 0) continue;

        double dist = distance2(*self, *unit);

        // Closest on other team, if equal take lowest health and if equal highest y (to make equal matches)
        if ((unit->team == 1 - self->team || groot) && allowedToAttack(self, unit) &&
            (closest == nullptr || minDist > dist || (minDist == dist && (closest->health > unit->health || unit->y > closest->y)))) {
            minDist = dist;
            closest = unit;
        }
    }
    return closest;
}

static void moveAttackTowards(Unit* self, const Point& point) {
    Unit* closest = findClosestOnOtherTeam(self, "none");

    if (canAttack(self, closest)) {
        fireAttack(self, closest);
    } else if (closest != nullptr && distance2(*self, *closest) < C::AGGROUNITRANGE2) {
        Event* e = newAttackMoveUnitEvent(self, closest);
        G.events.push_back(e);
    } else {
        Event* e = newAttackMoveEvent(self, point);
        G.events.push_back(e);
    }
}

static void attackUnitOrMoveTowards(Unit* self, Unit* unit) {
    if (!allowedToAttack(self, unit)) return;
    if (distance2(*self, *unit) <= self->range * self->range) {
        fireAttack(self, unit);
    } else {
        Event* e = newAttackMoveUnitEvent(self, unit);
        G.events.push_back(e);
    }
}

static void runTowards(Unit* self, const Point& p, double speed) {
    double dist = distance(*self, p);

    // Avoid a division by zero
    if (std::abs(dist) <= C::EPSILON) return;

    double timeToLocation = timeToReachTarget(*self, p, speed);

    double coef = speed / dist;
    self->vx = (p.x - self->x) * coef;
    self->vy = (p.y - self->y) * coef;
    self->moving = true;
    if (speed > dist) {
        G.events.push_back(newOnLocationReachedEvent(self, timeToLocation));
    }
}

static void afterRound(Unit* u) {
    if (u->stunTime > 0) u->stunTime--;
    u->x = roundJ(u->x);
    u->y = roundJ(u->y);
    u->vx = 0;
    u->vy = 0;
    u->moving = false;

    if (u->kind == K_LANE || u->kind == K_TOWER) {
        u->aggroTimeLeft--;
    } else if (u->kind == K_HERO) {
        u->visibilityTimer = std::max(0, u->visibilityTimer - 1);
        u->mana += u->manaregeneration;
        if (u->mana > u->maxMana) u->mana = u->maxMana;
        for (Skill* skill : u->skills) {
            if (skill == nullptr) continue;
            skill->cooldown = std::max(0, skill->cooldown - 1);
        }
    }
}

static void laneFindAction(Unit* self) {
    if (self->isDead || self->stunTime > 0) return;
    if (self->aggroUnit != nullptr && self->aggroTimeLeft > 0 && distance(*self, *self->aggroUnit) < C::AGGROUNITRANGE && self->aggroUnit->visible) {
        attackUnitOrMoveTowards(self, self->aggroUnit);
        return;
    }

    self->aggroTset = 1.0;
    self->aggroUnit = nullptr;
    self->aggroTimeLeft = -1;

    Unit* closest = findClosestOnOtherTeam(self, "UNIT");
    if (canAttack(self, closest)) {
        fireAttack(self, closest);
    } else if (closest != nullptr && distance2(*self, *closest) < C::AGGROUNITRANGE2) {
        attackUnitOrMoveTowards(self, closest);
    } else {
        closest = findClosestOnOtherTeam(self, "HERO");
        if (canAttack(self, closest)) {
            fireAttack(self, closest);
        } else if (closest != nullptr && distance2(*self, *closest) < C::AGGROUNITRANGE2 && allowedToAttack(self, closest)) {
            attackUnitOrMoveTowards(self, closest);
        } else {
            moveAttackTowards(self, self->targetPoint);
        }
    }
}

static void towerFindAction(Unit* self) {
    if (self->aggroUnit != nullptr && self->aggroTimeLeft > 0 && canAttack(self, self->aggroUnit)) {
        self->aggroTimeLeft--;
        fireAttack(self, self->aggroUnit);
        return;
    }

    self->aggroTset = 1.0;
    self->aggroUnit = nullptr;
    self->aggroTimeLeft = -1;
    Unit* closest = findClosestOnOtherTeam(self, "UNIT");
    if (canAttack(self, closest)) {
        fireAttack(self, closest);
    } else {
        closest = findClosestOnOtherTeam(self, "HERO");
        if (canAttack(self, closest)) fireAttack(self, closest);
    }
}

static void creatureFindAction(Unit* self) {
    if (self->isDead || self->stunTime > 0) return;
    if (self->state == AGGRESSIVE) {
        std::vector<Unit*> heroes;
        for (Unit* u : G.allUnits)
            if (u->isHero()) heroes.push_back(u);
        java::sort(heroes, [&](Unit* u1, Unit* u2) { return distance2(*u1, *self) < distance2(*u2, *self) ? -1 : 1; });
        Unit* target = heroes.at(0);
        if (distance2(*self, self->camp) < C::AGGROUNITRANGE2) {
            attackUnitOrMoveTowards(self, target);
        } else {
            self->state = RUNNINGBACK;
            runTowards(self, self->camp, self->moveSpeed);
        }
    } else if (self->state == RUNNINGBACK) {
        runTowards(self, self->camp, self->moveSpeed);
        if (distance(*self, self->camp) < 2) {
            self->state = PEACEFULL;
            self->health = self->maxHealth;
        }
    }
}

// ----------------------------------------------------------------------------
// Event behaviour
// ----------------------------------------------------------------------------
static void doDamage(Unit* unit, int damage, Unit* attacker) {
    for (auto& entry : G.damages) {
        if (entry.first == unit) {
            entry.second.push_back({unit, attacker, damage});
            return;
        }
    }
    G.damages.push_back({unit, {{unit, attacker, damage}}});
}

static bool unitDead(const Unit* unit, const Unit* affected) {
    if (unit != affected) return false;
    return unit->isDead;
}

static bool unitStopped(const Unit* unit) {
    if (unit->isDead) return true;
    if (unit->stunTime > 0) return true;
    return false;
}

static bool unitStopped(const Unit* unit, const Unit* affected, int outcome) {
    if (unit != affected) return false;
    if (unitDead(unit, affected)) return true;
    if (hasOutcome(outcome, STUNNED) && unit->stunTime > 0) return true;
    return false;
}

static void runSilentlyTowards(Unit* unit, const Point& targetPoint) {
    if (std::abs(unit->forceVY) > C::EPSILON || std::abs(unit->forceVX) > C::EPSILON) return;
    double targetDist = distance(targetPoint, *unit);
    double coef = ((double)unit->moveSpeed) / targetDist;
    unit->vx = (targetPoint.x - unit->x) * coef;
    unit->vy = (targetPoint.y - unit->y) * coef;
    unit->moving = true;
}

static Unit* getClosestUnitInRange(const Point& root, double range, int team, bool allowInvis, const Unit* ignoredUnit) {
    double closestDist = C::MAX_DOUBLE;
    Unit* closest = nullptr;
    for (Unit* unit : G.allUnits) {
        if (unit == ignoredUnit || unit->team == team) continue;
        if (unit->kind == K_TOWER) continue;
        if (!unit->visible && !allowInvis) continue;
        double dist = distance2(*unit, root);
        if (dist < closestDist && dist <= range * range) {
            closestDist = dist;
            closest = unit;
        }
    }
    return closest;
}

static Event* newSpeedChangedEvent(Unit* unit, double t, double vx, double vy) {
    Event* e = newEvent(EV_SPEED_CHANGED, unit, t);
    e->vx = vx;
    e->vy = vy;
    return e;
}

static Event* newSpeedChangedForceEvent(Unit* unit, double t, double forcevx, double forcevy) {
    Event* e = newEvent(EV_SPEED_CHANGED_FORCE, unit, t);
    e->vx = forcevx;
    e->vy = forcevy;
    return e;
}

static Event* newOnLocationReachedEvent(Unit* unit, double t) { return newEvent(EV_ON_LOCATION_REACHED, unit, t); }

static Event* newDelayedAttackEvent(Unit* unit, Unit* attacker, double t) {
    Event* e = newEvent(EV_DELAYED_ATTACK, unit, t);
    e->attacker = attacker;
    return e;
}

static Event* newDamageEvent(Unit* unit, Unit* attacker, double t, int damage) {
    Event* e = newEvent(EV_DAMAGE, unit, t);
    e->attacker = attacker;
    e->damage = damage;
    return e;
}

static Event* newCounterEvent(Unit* defender, double t, int range) {
    Event* e = newEvent(EV_COUNTER, defender, t);
    e->health = defender->health;
    e->range = range;
    return e;
}

static Event* newStunEvent(Unit* unit, double t, int stunTime) {
    Event* e = newEvent(EV_STUN, unit, t);
    e->stunTime = stunTime;
    return e;
}

static Event* newBlinkEvent(Unit* unit, double t, double x, double y) {
    Event* e = newEvent(EV_BLINK, unit, t);
    e->x = x;
    e->y = y;
    return e;
}

static Event* newAttackNearestDelayed(Unit* hero, double t) { return newEvent(EV_ATTACK_NEAREST_DELAYED, hero, t); }

static Event* newPowerUpEvent(Unit* unit, int moveSpeed, int range, int damage, int rounds) {
    Event* e = newEvent(EV_POWER_UP, unit, C::MAX_INT);
    e->moveSpeed = moveSpeed;
    e->range = range;
    e->damage = damage;
    e->rounds = rounds;
    return e;
}

static Event* newStealthEvent(Unit* unit, double t) { return newEvent(EV_STEALTH, unit, t); }

static Event* newHealthChangeEvent(const Point& targetPos, double t, int range, int dHealth, bool hitEnemies, Unit* user) {
    Event* e = newEvent(EV_HEALTH_CHANGE, user, t);
    e->range = range;
    e->targetPos = targetPos;
    e->dHealth = dHealth;
    e->hitEnemies = hitEnemies;
    return e;
}

static Event* newShieldEvent(Unit* unit, int t) {
    Event* e = newEvent(EV_SHIELD, unit, t + 1); // avoid rounding errors.
    e->rounds = t;
    return e;
}

static Event* newExplosiveShieldEvent(Unit* unit, int t) {
    Event* e = newEvent(EV_EXPLOSIVE_SHIELD, unit, t + 1);
    e->rounds = t;
    return e;
}

static Event* newExplosionEvent(Unit* unit, double t, double x, double y) {
    Event* e = newEvent(EV_EXPLOSION, unit, t);
    e->x = x;
    e->y = y;
    return e;
}

static Event* newDrainManaEvent(Unit* unit, double t, int manaToDrain, Unit* attacker) {
    Event* e = newEvent(EV_DRAIN_MANA, unit, t);
    e->manaToDrain = manaToDrain;
    e->attacker = attacker;
    return e;
}

static Event* newLineEffectEvent(Unit* unit, double t, MovingEntity* movingSpell, int damage, Unit* attacker, double radius, double duration, int damageByTime) {
    Event* e = newEvent(EV_LINE_EFFECT, unit, t);
    e->attacker = attacker;
    e->movingSpell = movingSpell;
    e->damage = damage;
    e->radius = radius;
    e->duration = duration;
    e->damageByTime = damageByTime;
    return e;
}

static Event* newWireEvent(const std::vector<Unit*>& potentialTargets, double t, MovingEntity* movingSpell, int stunTime, Unit* attacker, double radius, double duration, double dmgMultiplier) {
    Event* e = newEvent(EV_WIRE, nullptr, t);
    e->dmgMultiplier = dmgMultiplier;
    e->attacker = attacker;
    e->movingSpell = movingSpell;
    e->stunTime = stunTime;
    e->radius = radius;
    e->duration = duration;
    e->potentialTargets = potentialTargets;
    return e;
}

static void attackMoveRecalculate(Event* e, Unit* changedUnit) {
    if (changedUnit == nullptr) {
        e->t = 1.0;
        for (Unit* unit : G.allUnits) {
            if (unit->team != 1 - e->unit->team || !allowedToAttack(e->unit, unit)) continue;
            double time = getCollisionTime(*unit, *e->unit, C::AGGROUNITRANGE - C::EPSILON);
            if (time < e->t && time >= 0) e->t = time;
        }
    } else {
        double time = getCollisionTime(*changedUnit, *e->unit, C::AGGROUNITRANGE - C::EPSILON);
        if (time < e->t && time >= 0) e->t = time;
    }
}

static Event* newAttackMoveEvent(Unit* unit, const Point& targetPos) {
    Event* e = newEvent(EV_ATTACK_MOVE, unit, 1.0);
    e->targetPos = targetPos;
    runSilentlyTowards(unit, targetPos);
    attackMoveRecalculate(e, nullptr);
    return e;
}

static void attackMoveUnitRecalculate(Event* e, bool run) {
    Unit* unit = e->unit;
    double prevVx = unit->vx;
    double prevVy = unit->vy;

    if (canAttack(unit, e->nextTarget)) {
        e->t = 0.0;
    } else {
        if (run) runSilentlyTowards(unit, *e->nextTarget);
        double timeToTarget = getCollisionTime(*unit, *e->nextTarget, unit->range - C::EPSILON);
        if (timeToTarget < 0)
            e->t = timeToReachTarget(*unit, *e->nextTarget, unit->moveSpeed);
        else
            e->t = timeToTarget;
    }

    if (std::abs(unit->vx - prevVx) > C::EPSILON || std::abs(unit->vy - prevVy) > C::EPSILON) {
        G.events.push_back(newSpeedChangedEvent(unit, 0.0, unit->vx, unit->vy)); // Alert speed changed
    }
}

static Event* newAttackMoveUnitEvent(Unit* unit, Unit* targetUnit) {
    Event* e = newEvent(EV_ATTACK_MOVE_UNIT, unit, timeToReachTarget(*unit, *targetUnit, unit->moveSpeed));
    e->nextTarget = targetUnit;
    attackMoveUnitRecalculate(e, true);
    return e;
}

static int getOutcome(const Event* e) {
    switch (e->kind) {
        case EV_ON_LOCATION_REACHED:
        case EV_SPEED_CHANGED_FORCE:
        case EV_SPEED_CHANGED:
        case EV_ATTACK_MOVE:
        case EV_ATTACK_MOVE_UNIT:
            return SPEEDCHANGED;
        case EV_STUN:
        case EV_WIRE:
            return STUNNED;
        case EV_BLINK:
            return TELEPORTED;
        default:
            return NONE;
    }
}

static Affected onEventTime(Event* e, double currentTime);

static bool useAcrossRounds(Event* e) {
    switch (e->kind) {
        case EV_STUN:
            return true;
        case EV_POWER_UP:
            e->rounds--;
            if (e->rounds <= 0) {
                onEventTime(e, 0);
                return false;
            }
            return true;
        case EV_STEALTH:
            return !e->unit->visible;
        case EV_SHIELD:
            e->rounds--;
            if (e->rounds <= 0) {
                e->unit->shield = 0;
                return false;
            }
            return true;
        case EV_EXPLOSIVE_SHIELD:
            e->rounds--;
            if (e->rounds <= 0) {
                e->unit->explosiveShield = 0;
                return false;
            }
            return true;
        default:
            return false;
    }
}

static Affected onEventTime(Event* e, double currentTime) {
    Unit* unit = e->unit;
    switch (e->kind) {
        case EV_ON_LOCATION_REACHED:
            unit->vx = unit->forceVX;
            unit->vy = unit->forceVY;
            unit->moving = false;
            return {true, unit};

        case EV_SPEED_CHANGED_FORCE:
            unit->forceVX += e->vx;
            unit->forceVY += e->vy;
            unit->vx = unit->forceVX;
            unit->vy = unit->forceVY;
            return {true, unit};

        case EV_SPEED_CHANGED:
            if (unit->stunTime > 0 || (std::abs(unit->forceVY) > C::EPSILON || std::abs(unit->forceVX) > C::EPSILON)) return EMPTYLIST;
            unit->vx = e->vx;
            unit->vy = e->vy;
            return {true, unit};

        case EV_DELAYED_ATTACK:
            fireAttack(e->attacker, unit);
            return EMPTYLIST;

        case EV_DAMAGE:
            doDamage(unit, e->damage, e->attacker);
            return {true, unit};

        case EV_COUNTER:
            if (e->health > unit->health) {
                Unit* closest = getClosestUnitInRange(*unit, e->range, unit->team, true, unit);
                int damage = java::d2i((e->health - unit->health) * 1.5);
                if (closest != nullptr) doDamage(closest, damage, unit);
                unit->health = e->health;
            }
            return EMPTYLIST;

        case EV_STUN:
            unit->stunTime = std::max(unit->stunTime, e->stunTime);
            viewStun(unit);
            unit->vx = unit->forceVX;
            unit->vy = unit->forceVY;
            return {true, unit};

        case EV_BLINK:
            unit->moveTo(e->x, e->y);
            return {true, unit};

        case EV_ATTACK_NEAREST_DELAYED: {
            Unit* toHit = getClosestUnitInRange(*unit, unit->range, unit->team, false, unit);
            if (toHit != nullptr) fireAttack(unit, toHit);
            return EMPTYLIST;
        }

        case EV_POWER_UP:
            unit->moveSpeed -= e->moveSpeed;
            unit->range -= e->range;
            unit->damage -= e->damage;
            return EMPTYLIST;

        case EV_STEALTH:
            unit->invisibleBySkill = false;
            unit->visible = true;
            return {true, unit};

        case EV_HEALTH_CHANGE:
            for (int i = (int)G.allUnits.size() - 1; i >= 0; i--) {
                Unit* target = G.allUnits[i];
                if (unit->team == target->team && e->hitEnemies) continue;
                if (unit->team != target->team && !e->hitEnemies) continue;
                if (target->kind == K_TOWER) continue;

                double dist2 = distance2(e->targetPos, *target);
                if (dist2 <= e->range * e->range) doDamage(target, -1 * e->dHealth, unit);
            }
            return EMPTYLIST;

        case EV_SHIELD:
            unit->shield = 0;
            return EMPTYLIST;

        case EV_EXPLOSIVE_SHIELD:
            unit->explosiveShield = 0;
            return EMPTYLIST;

        case EV_EXPLOSION:
            for (Unit* target : G.allUnits) {
                if (target->kind == K_TOWER || unit->team == target->team) continue;
                double dist2 = distance2(*unit, *target);
                if (dist2 <= C::EXPLOSIVESHIELDRANGE2) doDamage(target, C::EXPLOSIVESHIELDDAMAGE, unit);
            }
            return EMPTYLIST;

        case EV_DRAIN_MANA: {
            if (unit->isDead) return EMPTYLIST;
            // If attack is dead mana is still drained since spell would be in motion.
            Unit* target = unit;
            e->manaToDrain = std::min(target->mana, e->manaToDrain);
            target->mana -= e->manaToDrain;
            e->attacker->mana += e->manaToDrain;
            if (e->attacker->maxMana < e->attacker->mana) e->attacker->mana = e->attacker->maxMana;
            return EMPTYLIST;
        }

        case EV_LINE_EFFECT: {
            e->movingSpell->moveIgnoreEdges(currentTime);
            double dist2 = distance2(*e->movingSpell, *unit);
            double compareDist = e->radius * e->radius;
            if (currentTime <= e->duration && dist2 <= compareDist) {
                doDamage(unit, java::d2i(e->damage + e->damageByTime * currentTime), e->attacker);
            }
            e->movingSpell->moveIgnoreEdges(currentTime * -1);
            return EMPTYLIST;
        }

        case EV_WIRE:
            e->movingSpell->moveIgnoreEdges(currentTime);
            if (currentTime <= e->duration) {
                Unit* closest = nullptr;
                double minDist2 = 0.0;
                double maxDist2 = (e->radius + C::EPSILON) * (e->radius + C::EPSILON);
                for (Unit* potentialTarget : e->potentialTargets) {
                    double dist2 = distance2(*potentialTarget, *e->movingSpell);
                    if (dist2 <= maxDist2 && (closest == nullptr || dist2 < minDist2)) {
                        minDist2 = dist2;
                        closest = potentialTarget;
                    }
                }
                if (closest != nullptr) {
                    G.events.push_back(newStunEvent(closest, 0, 2));
                    doDamage(closest, java::d2i(closest->maxMana * e->dmgMultiplier), e->attacker);
                    return {true, unit}; // unit is null
                }
            }
            return EMPTYLIST;

        case EV_ATTACK_MOVE: {
            Unit* closest = findClosestOnOtherTeam(unit, "none");
            if (canAttack(unit, closest)) {
                unit->vx = unit->forceVX;
                unit->vy = unit->forceVY;
                fireAttack(unit, closest);
                return {true, unit};
            }
            if (currentTime <= 1.0 && closest != nullptr && !closest->isDead) {
                attackUnitOrMoveTowards(unit, closest);
            } else {
                unit->vx = unit->forceVX;
                unit->vy = unit->forceVY;
                return {true, unit};
            }
            return EMPTYLIST;
        }

        case EV_ATTACK_MOVE_UNIT:
            unit->vx = unit->forceVX;
            unit->vy = unit->forceVY;
            if (e->nextTarget->isDead || unitStopped(unit)) return {true, unit};
            fireAttack(unit, e->nextTarget);
            return {true, unit};
    }
    return EMPTYLIST;
}

static bool afterAnotherEvent(Event* e, Unit* affectedUnit, int outcome, double currentTime) {
    Unit* unit = e->unit;
    switch (e->kind) {
        case EV_ON_LOCATION_REACHED:
        case EV_ATTACK_NEAREST_DELAYED:
            return unitStopped(unit, affectedUnit, outcome);

        case EV_SPEED_CHANGED_FORCE:
        case EV_SPEED_CHANGED:
        case EV_COUNTER:
        case EV_STUN:
        case EV_STEALTH:
        case EV_BLINK:
        case EV_POWER_UP:
            return unitDead(unit, affectedUnit);

        case EV_DELAYED_ATTACK:
            return unitStopped(e->attacker) || unitDead(unit, affectedUnit) || (affectedUnit == unit && hasOutcome(outcome, TELEPORTED));

        case EV_DAMAGE:
            if (affectedUnit == unit && hasOutcome(outcome, TELEPORTED)) return true;
            return unitDead(unit, affectedUnit);

        case EV_HEALTH_CHANGE:
        case EV_EXPLOSION:
        case EV_DRAIN_MANA:
            return false;

        case EV_SHIELD:
            return affectedUnit == unit && affectedUnit->shield <= 0;

        case EV_EXPLOSIVE_SHIELD:
            if (affectedUnit == unit && unit->explosiveShield <= 0) {
                G.events.push_back(newExplosionEvent(affectedUnit, 0, unit->x, unit->y));
                return true;
            }
            return false;

        case EV_LINE_EFFECT:
            if (currentTime > e->duration || unitDead(unit, affectedUnit)) return true;

            if (affectedUnit == unit && hasAnyOutcome(outcome, SPEEDCHANGED, TELEPORTED)) {
                e->movingSpell->moveIgnoreEdges(currentTime);
                double colT = getCollisionTime(*e->movingSpell, *unit, e->radius - C::EPSILON);
                if (colT >= 0)
                    e->t = colT;
                else
                    e->t = 2;
                e->movingSpell->moveIgnoreEdges(currentTime * -1);
            }
            return false;

        case EV_WIRE:
            if (affectedUnit == nullptr) return false;
            if (affectedUnit->team != e->attacker->team && (affectedUnit->kind == K_HERO || affectedUnit->kind == K_CREATURE) &&
                hasAnyOutcome(outcome, SPEEDCHANGED, TELEPORTED)) {
                e->movingSpell->moveIgnoreEdges(currentTime);

                double affectedT = getCollisionTime(*e->movingSpell, *affectedUnit, e->radius);
                if (affectedT < e->t && !affectedUnit->isDead) {
                    e->t = affectedT;
                } else {
                    e->t = 2;
                    for (Unit* u : e->potentialTargets) {
                        if (u->isDead) continue;
                        double colT = getCollisionTime(*e->movingSpell, *u, e->radius);
                        if (colT >= 0 && colT < e->t) e->t = colT;
                    }
                }

                e->movingSpell->moveIgnoreEdges(currentTime * -1);
            }
            return false;

        case EV_ATTACK_MOVE:
            if (unitStopped(unit, affectedUnit, outcome)) return true;
            if (affectedUnit == unit || affectedUnit == nullptr || affectedUnit->team != 1 - unit->team) return false;
            if (distance2(*unit, *affectedUnit) <= C::AGGROUNITRANGE2) {
                e->t = 0.0;
            } else if (hasAnyOutcome(outcome, SPEEDCHANGED, TELEPORTED)) {
                attackMoveRecalculate(e, affectedUnit);
            }
            return false;

        case EV_ATTACK_MOVE_UNIT:
            if (unit != affectedUnit && e->nextTarget != affectedUnit) return false;
            if (unitStopped(unit, affectedUnit, outcome)) {
                e->t = 0.0;
            } else if (e->nextTarget == affectedUnit && hasOutcome(outcome, TELEPORTED)) {
                // when target teleports we lose track and just stops.
                G.events.push_back(newSpeedChangedEvent(unit, 0.0, 0, 0));
                return true;
            } else if (e->nextTarget == affectedUnit && hasOutcome(outcome, SPEEDCHANGED)) {
                attackMoveUnitRecalculate(e, false);
            }
            return false;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Skills (Skills.java)
// ----------------------------------------------------------------------------
static Skill* newSkill(SkillKind kind, Unit* hero, int manaCost, const char* skillName, int range, int cooldown) {
    G.skillPool.emplace_back();
    Skill* s = &G.skillPool.back();
    s->kind = kind;
    s->hero = hero;
    s->manaCost = manaCost;
    s->skillName = skillName;
    s->range = range;
    s->initialCooldown = cooldown;
    s->cooldown = 0;
    return s;
}

static Skill* emptySkill() { return newSkill(SK_EMPTY, nullptr, 100000, "NONE", 0, C::ROUNDS + 1); }

static void doSkill(Skill* skill, double x, double y, int unitId) {
    Unit* hero = skill->hero;
    switch (skill->kind) {
        case SK_EMPTY:
            break;

        case SK_BLINK: {
            Point target = {x, y};
            if (distance(target, *hero) > skill->range) {
                double dist = distance(target, *hero);
                target.x = hero->x + ((target.x - hero->x) / dist * skill->range);
                target.y = hero->y + ((target.y - hero->y) / dist * skill->range);
            }
            hero->mana = std::min(hero->mana + 20, hero->maxMana);
            G.events.push_back(newBlinkEvent(hero, skill->duration, roundJ(target.x), roundJ(target.y)));
            break;
        }

        case SK_JUMP: {
            Point target = {x, y};
            double dist = distance(target, *hero);
            if (dist > skill->range) {
                target.x = hero->x + ((target.x - hero->x) / dist * skill->range);
                target.y = hero->y + ((target.y - hero->y) / dist * skill->range);
            }
            G.events.push_back(newBlinkEvent(hero, skill->duration, target.x, target.y));
            G.events.push_back(newAttackNearestDelayed(hero, skill->duration + C::EPSILON));
            break;
        }

        case SK_FLIP: {
            Unit* target = getUnitOfId(unitId);
            if (distance(*target, *hero) <= skill->range && target->kind != K_TOWER) {
                G.events.push_back(newStunEvent(target, 0, 1));
                if (target->team != hero->team)
                    G.events.push_back(newDamageEvent(target, hero, skill->duration, java::d2i(hero->damage * 0.4)));
                double vx = (hero->x - target->x) * 2 / skill->duration;
                double vy = (hero->y - target->y) * 2 / skill->duration;
                G.events.push_back(newSpeedChangedForceEvent(target, C::EPSILON, vx, vy));
                G.events.push_back(newSpeedChangedForceEvent(target, skill->duration, vx * -1, vy * -1));
            }
            break;
        }

        case SK_POWERUP: {
            int dmgIncrease = java::d2i(hero->moveSpeed * C::POWERUPDAMAGEINCREASE);
            hero->moveSpeed += 0;
            hero->range += 10;
            hero->damage += dmgIncrease;
            G.events.push_back(newPowerUpEvent(hero, 0, 10, dmgIncrease, java::d2i(skill->duration)));
            break;
        }

        case SK_AOEHEAL: {
            Point target = {x, y};
            if (distance(target, *hero) <= skill->range) {
                G.events.push_back(newHealthChangeEvent(target, skill->duration, (int)skill->radius, java::d2i(0.2 * (hero->mana + skill->manaCost)), false, hero));
            }
            break;
        }

        case SK_BURNING: {
            Point target = {x, y};
            if (distance(target, *hero) <= skill->range + C::EPSILON) {
                G.events.push_back(newHealthChangeEvent(target, skill->duration, (int)skill->radius, -1 * (hero->manaregeneration * 5 + 30), true, hero));
            }
            break;
        }

        case SK_SHIELD: {
            Unit* target = getUnitOfId(unitId);
            if (distance(*target, *hero) <= skill->range) {
                target->shield = std::max(target->shield, java::d2i(0.5 * hero->maxMana + 50));
                G.events.push_back(newShieldEvent(target, java::d2i(skill->duration)));
            }
            break;
        }

        case SK_PULL: {
            Unit* unit = getUnitOfId(unitId);
            double dist = distance(*unit, *hero);
            if (dist <= skill->range && unit->kind != K_TOWER) {
                if (dist > C::EPSILON) {
                    double vx = (hero->x - unit->x) / dist * 200 / skill->duration;
                    double vy = (hero->y - unit->y) / dist * 200 / skill->duration;
                    G.events.push_back(newSpeedChangedForceEvent(unit, skill->delay, vx, vy));
                    G.events.push_back(newSpeedChangedForceEvent(unit, skill->delay + skill->duration, vx * -1, vy * -1));
                    G.events.push_back(newStunEvent(unit, skill->delay, 1));
                }

                if (unit->isHero() && unit->team != hero->team)
                    G.events.push_back(newDrainManaEvent(unit, skill->delay + skill->duration, unit->manaregeneration * 3 + 5, hero));
            }
            break;
        }

        case SK_CHARGE: {
            Unit* target = getUnitOfId(unitId);
            double dist = distance(*target, *hero);
            if (dist <= skill->range) {
                G.events.push_back(newBlinkEvent(hero, skill->duration, target->x, target->y));
                if (target->team != hero->team) {
                    // Reduce dmg on his attack on the delayed attack
                    int halfDmg = hero->damage / 2;
                    hero->damage -= halfDmg;
                    G.events.push_back(newPowerUpEvent(hero, 0, 0, -halfDmg, 0));
                    G.events.push_back(newDelayedAttackEvent(target, hero, skill->duration + C::EPSILON));

                    G.events.push_back(newPowerUpEvent(target, 150, 0, 0, 0));
                    G.events.push_back(newPowerUpEvent(target, -150, 0, 0, 3));
                }
            }
            break;
        }

        case SK_BASH: {
            Unit* target = getUnitOfId(unitId);
            if (distance(*target, *hero) <= skill->range && target->kind != K_TOWER) {
                G.events.push_back(newDamageEvent(target, hero, hero->attackTime, hero->damage));
                G.events.push_back(newStunEvent(target, hero->attackTime, java::d2i(skill->duration)));
            }
            break;
        }

        case SK_EXPLOSIVE:
            hero->explosiveShield = java::d2i(hero->maxHealth * 0.07 + 50);
            G.events.push_back(newExplosiveShieldEvent(hero, java::d2i(std::floor(skill->duration + 0.5))));
            break;

        case SK_COUNTER:
            G.events.push_back(newCounterEvent(hero, skill->duration - C::EPSILON, skill->range));
            break;

        case SK_STEALTH:
            hero->invisibleBySkill = true;
            G.events.push_back(newStealthEvent(hero, skill->duration));
            runTowards(hero, Point{x, y}, hero->moveSpeed);
            break;

        case SK_WIRE: {
            Point target = {x, y};
            double dist = distance(*hero, target);
            target.x = hero->x + (x - hero->x) / dist * skill->range;
            target.y = hero->y + (y - hero->y) / dist * skill->range;

            G.entityPool.emplace_back();
            MovingEntity* lineSpellUnit = &G.entityPool.back();
            lineSpellUnit->x = hero->x;
            lineSpellUnit->y = hero->y;
            lineSpellUnit->vx = (target.x - hero->x) / skill->flyTime;
            lineSpellUnit->vy = (target.y - hero->y) / skill->flyTime;

            double lowestT = 2;
            std::vector<Unit*> possibleTargets;
            for (int i = (int)G.allUnits.size() - 1; i >= 0; i--) {
                Unit* unit = G.allUnits[i];
                if (unit->team != hero->team && unit->isHero()) {
                    double collisionT = getCollisionTime(*lineSpellUnit, *unit, skill->radius);
                    possibleTargets.push_back(unit);
                    if (collisionT >= 0 && collisionT <= lowestT) lowestT = collisionT;
                }
            }

            G.events.push_back(newWireEvent(possibleTargets, lowestT, lineSpellUnit, skill->stunTime, hero, skill->radius, skill->duration, 0.5));
            break;
        }

        case SK_LINE: {
            Point target = {x, y};
            double dist = distance(*hero, target);
            double vx = (x - hero->x) / dist;
            double vy = (y - hero->y) / dist;

            target.x = hero->x + vx * skill->range;
            target.y = hero->y + vy * skill->range;

            G.entityPool.emplace_back();
            MovingEntity* lineSpellUnit = &G.entityPool.back();
            lineSpellUnit->x = hero->x;
            lineSpellUnit->y = hero->y;
            lineSpellUnit->vx = vx * skill->range / skill->duration;
            lineSpellUnit->vy = vy * skill->range / skill->duration;

            for (Unit* unit : G.allUnits) {
                if (unit->team != hero->team && (unit->isHero() || unit->kind == K_CREATURE)) {
                    double collisionT = getCollisionTime(*lineSpellUnit, *unit, skill->radius - C::EPSILON);
                    G.events.push_back(newLineEffectEvent(unit, collisionT < 0 ? skill->duration : collisionT, lineSpellUnit,
                                                          java::d2i(0.2 * (hero->mana + skill->manaCost)), hero, skill->radius, skill->duration, 55));
                }
            }
            break;
        }
    }
}

// ----------------------------------------------------------------------------
// Factories.java / MapFactory.java
// ----------------------------------------------------------------------------
static Unit* generateUnit(int type, int team, int number, Player* player) {
    Point spawn = team == 0 ? SPAWNTEAM0 : SPAWNTEAM1;
    Point target = team == 1 ? TOWERTEAM0 : TOWERTEAM1;
    Unit* unit = newUnit(K_LANE, spawn.x + 50 * type * (team * 2 - 1), spawn.y + number * 50, 1, team, 150, player);
    unit->targetPoint = Point{target.x, unit->y}; // Move in a straight line

    if (type == 0) {
        unit->health = unit->maxHealth = 400;
        unit->damage = 25;
        unit->range = 90;
        unit->goldValue = C::MELEE_UNIT_GOLD_VALUE;
    } else {
        unit->health = unit->maxHealth = 250;
        unit->damage = 35;
        unit->range = 300;
        unit->goldValue = C::RANGER_UNIT_GOLD_VALUE;
    }
    unit->attackTime = 0.2;

    viewAddSprite(unit);
    return unit;
}

// returns nullptr where Java throws an IllegalArgumentException (after allocating the hero id)
static Unit* generateHero(std::string type, Player* player, const Point& spawn) {
    int team = player->index;
    Unit* hero = newUnit(K_HERO, spawn.x, spawn.y, 1, team, 200, player);
    hero->goldValue = C::HERO_GOLD_VALUE;
    hero->heroType = type;
    hero->skills[0] = emptySkill();
    hero->skills[1] = emptySkill();
    hero->skills[2] = emptySkill();

    // Since stub doesn't support IFs, just take heroes
    if (java::startsWith(type, "WAIT")) {
        if (G.round == 0) type = "HULK";
        else if (G.round == 1 && player->heroes.at(0)->heroType == "IRONMAN") type = "DEADPOOL";
        else if (G.round == 1) type = "IRONMAN";
        hero->heroType = type;
    }

    if (type == "IRONMAN") {
        hero->health = hero->maxHealth = 820;
        hero->mana = hero->maxMana = 200;
        hero->damage = 60;
        hero->range = 270;
        hero->manaregeneration = 2;
        Skill* blink = newSkill(SK_BLINK, hero, 16, "BLINK", 200, 3);
        blink->duration = 0.05;
        Skill* fireball = newSkill(SK_LINE, hero, 60, "FIREBALL", 900, 6);
        fireball->radius = 50;
        fireball->duration = 0.9;
        Skill* burning = newSkill(SK_BURNING, hero, 50, "BURNING", 250, 5);
        burning->duration = 0.01;
        burning->radius = 100;
        hero->skills[0] = blink;
        hero->skills[1] = fireball;
        hero->skills[2] = burning;
    } else if (type == "VALKYRIE") {
        hero->health = hero->maxHealth = 1400;
        hero->mana = hero->maxMana = 155;
        hero->damage = 65;
        hero->range = 130;
        hero->manaregeneration = 2;
        Skill* flip = newSkill(SK_FLIP, hero, 20, "SPEARFLIP", 155, 3);
        flip->duration = 0.1;
        Skill* jump = newSkill(SK_JUMP, hero, 35, "JUMP", 250, 3);
        jump->duration = 0.15;
        Skill* powerup = newSkill(SK_POWERUP, hero, 50, "POWERUP", 0, 7);
        powerup->duration = 4;
        hero->skills[0] = flip;
        hero->skills[1] = jump;
        hero->skills[2] = powerup;
    } else if (type == "DEADPOOL") {
        hero->health = hero->maxHealth = 1380;
        hero->mana = hero->maxMana = 100;
        hero->damage = 80;
        hero->range = 110;
        hero->manaregeneration = 1;
        Skill* counter = newSkill(SK_COUNTER, hero, 40, "COUNTER", 350, 5);
        counter->duration = 1;
        Skill* wire = newSkill(SK_WIRE, hero, 50, "WIRE", 200, 9);
        wire->stunTime = 2;
        wire->radius = 25;
        wire->flyTime = 0.3;
        wire->duration = wire->stunTime;
        Skill* stealth = newSkill(SK_STEALTH, hero, 30, "STEALTH", 0, 6);
        stealth->duration = 5;
        hero->skills[0] = counter;
        hero->skills[1] = wire;
        hero->skills[2] = stealth;
    } else if (type == "DOCTOR_STRANGE") {
        hero->health = hero->maxHealth = 955;
        hero->mana = hero->maxMana = 300;
        hero->damage = 50;
        hero->range = 245;
        hero->manaregeneration = 2;
        Skill* aoeheal = newSkill(SK_AOEHEAL, hero, 50, "AOEHEAL", 250, 6);
        aoeheal->duration = 0.01;
        aoeheal->radius = 100;
        Skill* shield = newSkill(SK_SHIELD, hero, 40, "SHIELD", 500, 6);
        shield->duration = 3;
        Skill* pull = newSkill(SK_PULL, hero, 40, "PULL", 400, 5);
        pull->duration = 0.3;
        pull->delay = 0.1;
        hero->skills[0] = aoeheal;
        hero->skills[1] = shield;
        hero->skills[2] = pull;
    } else if (type == "HULK") {
        hero->health = hero->maxHealth = 1450;
        hero->mana = hero->maxMana = 90;
        hero->damage = 80;
        hero->range = 95;
        hero->manaregeneration = 1;
        Skill* charge = newSkill(SK_CHARGE, hero, 20, "CHARGE", 300, 4);
        charge->duration = 0.05;
        Skill* explosive = newSkill(SK_EXPLOSIVE, hero, 30, "EXPLOSIVESHIELD", 100, 8);
        explosive->duration = 4;
        Skill* bash = newSkill(SK_BASH, hero, 40, "BASH", 150, 10);
        bash->duration = 2;
        hero->skills[0] = charge;
        hero->skills[1] = explosive;
        hero->skills[2] = bash;
    } else {
        return nullptr; // Hero not supported
    }

    viewAddSprite(hero);
    return hero;
}

static Unit* generateTower(Player* player, int team) {
    Point spawn = team == 0 ? TOWERTEAM0 : TOWERTEAM1;
    Unit* tower = newUnit(K_TOWER, spawn.x, spawn.y, java::d2i(C::TOWERHEALTH * C::TOWERHEALTHSCALE), team, 0, player);
    tower->range = 400;
    tower->damage = C::TOWERDAMAGE;
    player->tower = tower;
    tower->attackTime = 0.2;

    viewAddSprite(tower);
    return tower;
}

static void generateBush(const Point& point, std::vector<Bush>& bushes) {
    bushes.push_back(Bush{{point.x, point.y}, C::BUSHRADIUS});
    // ViewController.addObstacle: random sprite rotation
    G.random.nextDouble();
}

static Unit* generateCreature(const Point& point, double amplitude) {
    Unit* creature = newUnit(K_CREATURE, point.x, point.y, 1, -1, 300, nullptr);
    creature->camp = point;
    creature->health = creature->maxHealth = java::d2i(400 * amplitude);
    creature->damage = java::d2i(35 * amplitude);
    creature->range = 150;
    creature->moveSpeed = 250;
    creature->goldValue = java::d2i(C::NEUTRALGOLD * amplitude);
    viewAddSprite(creature);
    creature->attackTime = 0.2;
    return creature;
}

static void spawnExtraBushes(int startX, int startY, int endX, int endY, std::vector<Bush>& bushes) {
    const int center = C::MAPWIDTH / 2;
    for (int i = 0; i < 10; i++) {
        int posX = java::d2i(G.random.nextDouble() * (endX - startX) + startX);
        int posY = java::d2i(G.random.nextDouble() * (endY - startY) + startY);
        if (posX < C::BUSHRADIUS || posY < C::BUSHRADIUS || posX > C::MAPWIDTH - C::BUSHRADIUS || posY > C::MAPHEIGHT - C::BUSHRADIUS) continue;
        Point newBush = {(double)posX, (double)posY};
        bool isOverlapping = false;
        for (const Bush& bush : bushes) {
            if (distance(bush, newBush) <= bush.radius * 2) isOverlapping = true;
        }

        if (!isOverlapping) {
            if (std::abs(posX - center) < C::BUSHRADIUS) {
                generateBush(Point{(double)center, (double)posY}, bushes);
            } else {
                generateBush(Point{(double)posX, (double)posY}, bushes);
                generateBush(Point{(double)(1920 - posX), (double)posY}, bushes);
            }
            break;
        }
    }
}

static std::vector<Bush> generateBushes(const std::vector<Point>& spawns) {
    std::vector<Bush> obstacles;
    // bushes below the lane
    int posX = 50;
    int bottomBushCount = java::d2i(G.random.nextDouble() * 4 + 1);
    for (int i = 0; i < bottomBushCount; i++) {
        posX = java::d2i(G.random.nextDouble() * ((910 - posX) / (bottomBushCount - i)) + posX);
        if (posX > 910) break;
        generateBush(Point{(double)posX, 720}, obstacles);
        generateBush(Point{(double)(1920 - posX), 720}, obstacles);
        posX += 100;
    }

    // bushes above the lane
    posX = 50;
    bottomBushCount = java::d2i(G.random.nextDouble() * 2 + 2);
    for (int i = 0; i < bottomBushCount; i++) {
        posX = java::d2i(G.random.nextDouble() * ((910 - posX) / (bottomBushCount - i)) + posX);
        if (posX > 910) break;
        generateBush(Point{(double)posX, 380}, obstacles);
        generateBush(Point{(double)(1920 - posX), 380}, obstacles);
        posX += 100;
    }

    for (const Point& spawn : spawns) {
        int posY = java::d2i(spawn.y);
        posX = java::d2i(spawn.x);
        spawnExtraBushes(posX - 150, posY - 150, posX + 150, posY + 150, obstacles);
    }

    int extraBushes = java::d2i(G.random.nextDouble() * 4);
    for (int i = 0; i < extraBushes; i++) {
        spawnExtraBushes(C::MAPWIDTH / 2 - 300, 50, C::MAPWIDTH / 2 + 300, 300, obstacles);
    }
    return obstacles;
}

static std::vector<Point> generateSpawns() {
    int forestCampCount = java::d2i(G.random.nextDouble() * 2 + 3);
    std::vector<Point> spawns(forestCampCount, Point{0, 0});
    int posX = 50;
    int campIter = forestCampCount / 2;
    int midBoundary = 850;
    if (forestCampCount % 2 > 0) {
        spawns[forestCampCount / 2] = Point{960, (double)java::d2i(G.random.nextDouble() * 230 + 50)};
        midBoundary = 750;
    }
    for (int i = 0; i < campIter; i++) {
        posX = java::d2i(G.random.nextDouble() * ((midBoundary - posX) / (campIter - i)) + posX);
        if (posX > midBoundary) break; // never happens, the referee would crash later on
        int posY = java::d2i(G.random.nextDouble() * 230 + 50);
        spawns[i] = Point{(double)posX, (double)posY};
        spawns[forestCampCount - i - 1] = Point{(double)(1920 - posX), (double)posY};
        posX += 150;
    }
    return spawns;
}

// HashMap<String, Integer> of item stats, remembers the insertion order of the keys
struct StatMap {
    std::vector<std::pair<C::Stat, int>> entries;

    int find(C::Stat st) const {
        for (int i = 0; i < (int)entries.size(); i++)
            if (entries[i].first == st) return i;
        return -1;
    }
    bool containsKey(C::Stat st) const { return find(st) >= 0; }
    int get(C::Stat st) const { return entries[find(st)].second; }
    void put(C::Stat st, int v) {
        int i = find(st);
        if (i >= 0) entries[i].second = v;
        else entries.push_back({st, v});
    }
};

static int findCost(const StatMap& stats) {
    std::vector<i32> keyHashes;
    for (auto& e : stats.entries) keyHashes.push_back(java::hashCode(java::ascii(C::STAT_NAMES[e.first])));
    double totalCost = 0.0;
    for (int k : java::hashMapOrder(keyHashes)) {
        int val = stats.entries[k].second;
        totalCost += val * C::STAT_PRICE[stats.entries[k].first];
    }
    return java::d2i(java::min(java::max(std::ceil(totalCost / 2.5), std::ceil(totalCost - (totalCost * totalCost / 5000))), 1200));
}

static Item* newItem(const std::string& name, const StatMap& stats, bool isPotion) {
    G.itemPool.emplace_back();
    Item* item = &G.itemPool.back();
    item->name = name;
    item->cost = findCost(stats); // evaluated before Item.fillEmptyStats
    for (auto& e : stats.entries) item->stats[e.first] = e.second;
    item->isPotion = isPotion;
    return item;
}

static Item* generateItem(int lowerLimit, int upperLimit, int nb) {
    StatMap stats;

    int spent = 0;
    std::string prefix = "";
    std::string suffix = "Gadget";
    int cost = rndInt(lowerLimit, upperLimit);

    while (spent + C::MINIMUM_STAT_COST < cost) {
        int investment = std::min(cost - spent, rndInt(C::MINIMUM_STAT_COST, cost));
        C::Stat statType = C::STATS[rndInt(0, 5)];
        double price = C::STAT_PRICE[statType];
        int limit = C::STAT_LIMIT[statType];

        if (stats.containsKey(statType)) {
            if (stats.get(statType) < limit) {
                int current = stats.get(statType);
                int extra = java::d2i(investment / price);
                int actual = std::min(limit, current + extra);
                stats.put(statType, actual);
                investment = java::d2i((actual - current) * price);
            } else {
                continue;
            }
        } else {
            int extra = java::d2i(investment / price);
            int actual = std::min(limit, extra);
            stats.put(statType, actual);
            investment = java::d2i((actual) * price);
        }

        spent += investment;
    }

    if (stats.containsKey(C::HEALTH)) stats.put(C::MAXHEALTH, stats.get(C::HEALTH));
    if (stats.containsKey(C::MANA)) stats.put(C::MAXMANA, stats.get(C::MANA));

    int dmg = stats.containsKey(C::DAMAGE) ? stats.get(C::DAMAGE) : 0;
    int speed = stats.containsKey(C::MOVESPEED) ? stats.get(C::MOVESPEED) : 0;

    if (dmg > 0 && dmg > speed) suffix = "Blade";
    else if (speed > 0) suffix = "Boots";

    if (lowerLimit == C::ITEM_LEVELS[0][0]) prefix += "Bronze";
    else if (lowerLimit == C::ITEM_LEVELS[1][0]) prefix += "Silver";
    else if (lowerLimit == C::ITEM_LEVELS[2][0]) prefix += "Golden";
    else if (lowerLimit == C::ITEM_LEVELS[3][0]) prefix += "Legendary";
    return newItem(prefix + "_" + suffix + "_" + std::to_string(nb), stats, false);
}

static Item* generatePotion(const char* name, C::Stat stat, int amount) {
    StatMap stats;
    stats.put(stat, amount);
    return newItem(name, stats, true);
}

static void generateItems() {
    std::vector<Item*> items;
    for (auto& level : C::ITEM_LEVELS) {
        for (int i = 0; i < C::NB_ITEMS_PER_LEVEL; i++) {
            items.push_back(generateItem(level[0], level[1], (int)items.size() + 1));
        }
    }
    items.push_back(generatePotion("mana_potion", C::MANA, 50));
    items.push_back(generatePotion("larger_potion", C::HEALTH, 100));
    items.push_back(generatePotion("xxl_potion", C::HEALTH, 500));

    std::vector<i32> nameHashes;
    for (Item* item : items) nameHashes.push_back(java::hashCode(java::ascii(item->name)));
    G.items.clear();
    for (int k : java::hashMapOrder(nameHashes)) G.items.push_back(items[k]);
}

static Item* findItem(const std::string& name) {
    for (Item* item : G.items)
        if (item->name == name) return item;
    return nullptr;
}

// ----------------------------------------------------------------------------
// Hero items (Hero.java)
// ----------------------------------------------------------------------------
static void addCharacteristics(Unit* hero, const Item* item, int amplitude) {
    const int* v = item->stats;
    hero->damage += v[C::DAMAGE] * amplitude;
    hero->health += v[C::HEALTH] * amplitude;
    hero->maxHealth += v[C::MAXHEALTH] * amplitude;
    hero->mana += v[C::MANA] * amplitude;
    hero->maxMana += v[C::MAXMANA] * amplitude;
    hero->moveSpeed += v[C::MOVESPEED] * amplitude;
    hero->manaregeneration += v[C::MANAREGEN] * amplitude;

    if (hero->mana > hero->maxMana) hero->mana = hero->maxMana;
    if (hero->health > hero->maxHealth) hero->health = hero->maxHealth;
    if (hero->mana < 0) hero->mana = 0;
    if (hero->health <= 0) hero->health = 1;
    if (hero->moveSpeed > C::MAXMOVESPEED) hero->moveSpeed = C::MAXMOVESPEED;
}

static void addItem(Unit* hero, Item* item) {
    addCharacteristics(hero, item, 1);
    if (!item->isPotion) hero->items.push_back(item);
}

static void removeItem(Unit* hero, Item* item) {
    auto it = std::find(hero->items.begin(), hero->items.end(), item);
    if (it == hero->items.end()) return;
    hero->items.erase(it);
    addCharacteristics(hero, item, -1);
}

// ----------------------------------------------------------------------------
// Player.java
// ----------------------------------------------------------------------------
static void doHeroCommand(Player* player, const std::string& roundOutput, Unit* hero) {
    // every exception thrown in here is caught and only reported in the game summary
    std::vector<std::string> roundOutputSplitted = java::split(roundOutput, ';', 2);

    std::vector<std::string> outputValues = java::split(roundOutputSplitted[0], ' ');
    if (outputValues.empty()) return; // ArrayIndexOutOfBoundsException
    const std::string& command = outputValues[0];
    int arguments = (int)outputValues.size() - 1;

    // Verification
    bool allNumbers = true;
    for (size_t num = 1; num < outputValues.size(); num++) {
        if (!isNumber(outputValues[num])) allNumbers = false;
    }

    if (command == "MOVE_ATTACK" && arguments == 3 && allNumbers) {
        double x, y;
        i32 id;
        java::parseDouble(outputValues[1], x);
        java::parseDouble(outputValues[2], y);
        if (!java::parseInt(outputValues[3], id)) return;
        Point target = {x, y};
        runTowards(hero, target, hero->moveSpeed);
        Unit* unit = getUnitOfId(id);
        if (unit != nullptr) {
            G.events.push_back(newDelayedAttackEvent(unit, hero, timeToReachTarget(*hero, target, hero->moveSpeed)));
        }
    } else if (command == "MOVE" && arguments == 2 && allNumbers) {
        double x, y;
        java::parseDouble(outputValues[1], x);
        java::parseDouble(outputValues[2], y);
        runTowards(hero, Point{x, y}, hero->moveSpeed);
    } else if (command == "ATTACK_NEAREST" && arguments == 1 && !allNumbers) {
        Unit* toHit = findClosestOnOtherTeam(hero, outputValues[1].c_str());
        if (toHit != nullptr) attackUnitOrMoveTowards(hero, toHit);
    } else if (command == "ATTACK" && arguments == 1 && allNumbers) {
        i32 id;
        if (!java::parseInt(outputValues[1], id)) return;
        Unit* unit = getUnitOfId(id);
        if (unit != nullptr && allowedToAttack(hero, unit)) attackUnitOrMoveTowards(hero, unit);
    } else if (command == "BUY" && arguments == 1 && !allNumbers) {
        Item* item = findItem(outputValues[1]);
        if (item == nullptr) {
            // item does not exist
        } else if (player->gold < item->cost) {
            // can't afford
        } else if ((int)hero->items.size() >= C::MAXITEMCOUNT) {
            // Can't have more than MAXITEMCOUNT items
        } else {
            addItem(hero, item);
            player->gold -= item->cost;
        }
    } else if (command == "SELL" && arguments == 1 && !allNumbers) {
        Item* foundItem = nullptr;
        for (Item* item : hero->items) {
            if (item->name == outputValues[1]) {
                foundItem = item;
                break;
            }
        }
        if (foundItem != nullptr) {
            removeItem(hero, foundItem);
            player->gold += roundJ(foundItem->cost * C::SELLITEMREFUND);
        }
    } else if (command == "WAIT" && arguments == 0) {
        return;
    } else if (allNumbers) { // skillz
        for (Skill* skill : hero->skills) {
            if (skill == nullptr) continue;
            if (command == skill->skillName) {
                if (skill->manaCost > hero->mana) {
                    // Not enough mana
                } else if (skill->cooldown > 0) {
                    // Skill on cooldown
                } else {
                    double x = -1;
                    double y = -1;
                    i32 unitId = -1;
                    if (outputValues.size() == 3 && skill->getTargetType() == POSITION) {
                        java::parseDouble(outputValues[1], x);
                        java::parseDouble(outputValues[2], y);
                    } else if (outputValues.size() == 2 && skill->getTargetType() == UNIT) {
                        if (!java::parseInt(outputValues[1], unitId)) return;
                        Unit* unit = getUnitOfId(unitId);
                        // the viewer throws a NullPointerException for missing units
                        if (unit == nullptr) return;
                        if (!allowedToTarget(hero, unit) || unit->kind == K_TOWER) return;
                    } else if (outputValues.size() == 1 && skill->getTargetType() == SELF) {
                    } else {
                        return; // invalid number of parameters on spell
                    }

                    hero->mana -= skill->manaCost;
                    skill->cooldown = skill->initialCooldown;
                    hero->invisibleBySkill = false;
                    doSkill(skill, x, y, unitId);
                }
                return;
            }
        }
        // spell not found
    }
    // else: invalid command
}

// returns false where Java throws an IndexOutOfBoundsException
static bool handlePlayerOutputs(Player* player, std::vector<std::string> outputs) {
    std::vector<Unit*> internalHeroes = player->heroes;
    if (outputs.size() == 2 && java::startsWith(outputs[1], "SELL") && player->heroesAlive() == 2) {
        // Flip direction to allow other way commands
        Unit* first = internalHeroes[0];
        internalHeroes.erase(internalHeroes.begin());
        internalHeroes.push_back(first);
        std::swap(outputs[0], outputs[1]);
    }

    size_t outputCounter = 0;
    for (Unit* hero : internalHeroes) {
        if (hero->isDead) continue;

        if (hero->stunTime > 0) {
            outputCounter++;
            continue;
        }

        if (outputCounter >= outputs.size()) return false;
        const std::string& roundOutput = outputs[outputCounter++];
        doHeroCommand(player, roundOutput, hero);
    }
    return true;
}

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
static void handleAfterEvents(const Affected& affected, int outcome) {
    for (int i = (int)G.events.size() - 1; i >= 0; i--) {
        Event* event = G.events[i];
        if (affected.any) {
            if (afterAnotherEvent(event, affected.unit, outcome, G.t) || event->t < 0) {
                removeEvent(event);
            }
        }
    }
}

static void handleDamage() {
    // Java iterates a HashMap keyed by identity (unspecified order). The outcome does not depend on it:
    // gold/kills are additive and event removals are order independent. The only exception is when
    // two explosive shields break at the same time (explosion events are queued in that order).
    for (size_t idx = 0; idx < G.damages.size(); idx++) {
        Unit* target = G.damages[idx].first;
        std::vector<Damage> dmgs = G.damages[idx].second;
        int totalDamage = 0;
        bool anyHero = target->isHero();

        Unit* highestDamageUnit = nullptr;

        // Used when 2 meele attacks groot
        Unit* otherAttacker = nullptr;

        int highestDmg = 0;

        for (const Damage& dmg : dmgs) {
            totalDamage += dmg.damage;
            anyHero = anyHero || dmg.attacker->isHero();

            // hero becomes visible if he damages any on enemy team
            if (dmg.attacker->isHero() && target->player != nullptr && target->player != dmg.attacker->player) {
                dmg.attacker->visibilityTimer = 2;
                dmg.attacker->visible = true;
                dmg.attacker->invisibleBySkill = false;
            } else if (dmg.attacker->isHero()) {
                dmg.attacker->invisibleBySkill = false;
            }

            // Highest damage or attackers advantage.
            if (dmg.damage > highestDmg || (dmg.damage == highestDmg && dmg.attacker->team == 1 - dmg.target->team)) {
                highestDamageUnit = dmg.attacker;
                highestDmg = dmg.damage;
                otherAttacker = nullptr;
            } else if (dmg.damage == highestDmg && dmg.target->team == -1 && dmg.attacker->team != highestDamageUnit->team) {
                otherAttacker = dmg.attacker;
            }
        }
        if (totalDamage > 0 && target->getShield() > 0) {
            int val = std::min(totalDamage, target->getShield());
            target->adjustShield(val);
            totalDamage -= val;
        }

        target->health = std::min(target->health - totalDamage, target->maxHealth);

        if (target->health <= 0) {
            target->isDead = true;

            if (highestDamageUnit->team == target->team) {
                target->goldValue = 0;
                if (highestDamageUnit->player != nullptr) highestDamageUnit->player->denies++;
            }

            if (target->isHero()) {
                if (highestDamageUnit->kind == K_LANE || highestDamageUnit->kind == K_TOWER) {
                    target->goldValue = target->goldValue / 2; // hero lost half of its value if killed by creep or tower
                } else if (highestDamageUnit->kind == K_CREATURE) {
                    target->goldValue = 0;
                }
            } else if (highestDamageUnit->isHero() && highestDamageUnit->team != target->team) {
                if (highestDamageUnit->player != nullptr) highestDamageUnit->player->unitKills++;
            } else {
                target->goldValue = 0;
            }

            if (otherAttacker != nullptr && otherAttacker->isHero()) {
                otherAttacker->player->unitKills++;
                otherAttacker->player->gold += target->goldValue;
            }

            if (highestDamageUnit->player != nullptr) highestDamageUnit->player->gold += target->goldValue;

            auto it = std::find(G.allUnits.begin(), G.allUnits.end(), target);
            if (it != G.allUnits.end()) G.allUnits.erase(it);
            viewKill(target, G.t);
        } else if (target->kind == K_CREATURE) {
            if (target->state == PEACEFULL) target->state = AGGRESSIVE;
        }

        handleAfterEvents({true, target}, LIFECHANGED);
    }

    G.damages.clear();
}

// updates visibility of heroes based on their positions in relation to bushes, trees and other heroes
static void updateVisibility(const std::vector<Player*>& players) {
    G.visibleHeroes.clear();
    for (Player* player : players) {
        for (Unit* hero : player->heroes) {
            if (hero->isDead) continue;
            G.visibleHeroes.push_back(hero);
            hero->becomingInvis = hero->visible;
            hero->visible = !hero->invisibleBySkill;
        }
    }

    std::vector<Unit*> hideout;
    std::vector<int> teams;
    for (const Bush& bush : G.bushes) {
        hideout.clear();
        teams.clear();
        if (G.visibleHeroes.empty()) break;
        for (Unit* hero : G.visibleHeroes) {
            if (distance(*hero, bush) <= bush.radius) {
                hideout.push_back(hero);
                if (std::find(teams.begin(), teams.end(), hero->team) == teams.end()) teams.push_back(hero->team);
            }
        }
        if (teams.size() == 1) {
            for (Unit* hero : hideout) {
                if (hero->visibilityTimer > 0) continue;
                hero->visible = false;
                auto it = std::find(G.visibleHeroes.begin(), G.visibleHeroes.end(), hero);
                if (it != G.visibleHeroes.end()) G.visibleHeroes.erase(it);
            }
        }
    }
}

static bool isGameOver(const std::vector<Player*>& players) {
    for (Player* player : players) {
        if (player->tower->isDead) return true;
        if (player->heroesAlive() == 0) return true;
    }
    return false;
}

static Event* findNextEvent() {
    double firstTime = C::ROUNDTIME;
    Event* nextEvent = nullptr;
    for (Event* event : G.events) {
        if (event->t < firstTime && event->t >= 0.0) {
            nextEvent = event;
            firstTime = event->t;
        }
    }
    return nextEvent;
}

static void handleTurn(const std::vector<Player*>& players) {
    // Make dummies do their actions
    for (size_t i = 0; i < G.allUnits.size(); i++) {
        Unit* unit = G.allUnits[i];
        if (unit->stunTime > 0) continue;
        if (unit->kind == K_LANE) laneFindAction(unit);
        else if (unit->kind == K_TOWER) towerFindAction(unit);
        else if (unit->kind == K_CREATURE) creatureFindAction(unit);
    }

    // Inform events about speed changes of moving units.
    for (Unit* unit : G.allUnits) {
        if (std::abs(unit->vx) > C::EPSILON || std::abs(unit->vy) > C::EPSILON) {
            for (int i = (int)G.events.size() - 1; i >= 0; i--) {
                Event* event = G.events[i];
                if (afterAnotherEvent(event, unit, SPEEDCHANGED, 0.0)) removeEvent(event);
            }
        }
    }

    std::vector<Event*> occuringEvents;
    while (G.t <= C::ROUNDTIME) {
        if (isGameOver(players)) {
            viewAfterRound();
            return;
        }

        Event* nextEvent = findNextEvent();
        if (nextEvent == nullptr || nextEvent->t + G.t > C::ROUNDTIME) break;

        removeEvent(nextEvent);
        double eventTime = nextEvent->t + C::EPSILON;

        for (Unit* unit : G.allUnits) unit->move(eventTime);

        for (Event* event : G.events) event->t -= eventTime;

        G.t += eventTime;

        occuringEvents.clear();
        occuringEvents.push_back(nextEvent);
        for (int i = (int)G.events.size() - 1; i >= 0; i--) {
            Event* event = G.events[i];
            if (event->t < 0 && event->t + G.t <= 1.0) {
                occuringEvents.push_back(event);
                G.events.erase(G.events.begin() + i);
            }
        }

        std::stable_sort(occuringEvents.begin(), occuringEvents.end(),
                         [](const Event* a, const Event* b) { return getOutcome(a) < getOutcome(b); });

        for (Event* currentEvent : occuringEvents) {
            Affected affectedUnits = onEventTime(currentEvent, G.t);
            handleAfterEvents(affectedUnits, getOutcome(currentEvent));
        }

        handleDamage();
    }

    for (Event* event : G.events) event->t -= 1.0 - G.t;

    for (Unit* unit : G.allUnits) unit->move(1.0 - G.t);

    for (int i = (int)G.events.size() - 1; i >= 0; i--) {
        Event* event = G.events[i];
        if (!useAcrossRounds(event)) removeEvent(event);
    }

    for (Unit* unit : G.allUnits) afterRound(unit);

    updateVisibility(players);
    viewAfterRound();
}

static void spawnUnits(const std::vector<Player*>& players) {
    for (size_t team = 0; team < players.size(); team++) {
        Player* player = players[team];
        for (int i = 0; i < C::MELEE_UNIT_COUNT; i++) {
            G.allUnits.push_back(generateUnit(0, (int)team, i, player));
        }
        for (int i = 0; i < C::RANGED_UNIT_COUNT; i++) {
            G.allUnits.push_back(generateUnit(1, (int)team, G.round / C::SPAWNRATE % 3, player));
        }
    }
}

static void spawnForestCreatures() {
    for (int i = 0; i < G.forestCampCount; i++) {
        if (G.creatures[i] == nullptr || G.creatures[i]->isDead) {
            G.allUnits.push_back(G.creatures[i] = generateCreature(G.spawns[i], G.amplitude[i]));
            G.amplitude[i] *= 1.2;
        }
    }
}

static void beforeTurn(int turn, const std::vector<Player*>& players) {
    G.t = 0.0;
    G.round = turn;

    if (turn % C::SPAWNRATE == C::HEROCOUNT) spawnUnits(players);

    if (turn % C::NEUTRALSPAWNRATE == C::NEUTRALSPAWNTIME + C::HEROCOUNT) spawnForestCreatures();
}

static void initialize() {
    // setupGame
    G.allUnits.push_back(generateTower(&players[0], 0));
    G.allUnits.push_back(generateTower(&players[1], 1));

    G.spawns = generateSpawns();
    G.forestCampCount = (int)G.spawns.size();
    G.creatures.assign(G.forestCampCount, nullptr);
    G.amplitude.assign(G.forestCampCount, 1.0);

    G.bushes = generateBushes(G.spawns);

    // setupGold
    int amount = java::d2i(G.random.nextDouble() * 551 + 100);
    for (Player& p : players) p.gold = amount;

    generateItems();
}

// ----------------------------------------------------------------------------
// The bots (common/agent.h: Agent#getOutput and AsynchronousWriter are the same in engine 1.20)
// ----------------------------------------------------------------------------
static cg::Agent agents[2];

// AbstractPlayer#execute: sends the input and reads the expected amount of lines.
// Returns false on timeout, otherwise the lines as received by the referee.
static bool execute(cg::Agent& agent, const std::string& input, int nbLines, int timeoutMs, std::vector<std::string>& outputs) {
    agent.sendInput(input);
    std::string output;
    if (!agent.getOutput(nbLines, timeoutMs, false, output)) return false;
    for (auto& ch : output)
        if (ch == '\r') ch = '\n';
    outputs = java::split(output, '\n');
    return true;
}

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
static bool gameEnd = false;
static FILE* trace = nullptr;
static i64 turnTimeout = 0; // -timeout, see cg::turnTimeoutMs

static void deactivate(Player* player) {
    player->score = C::LOST_SCORE;
    player->active = false;
}

static bool shouldSendToPlayer(const Player* player, const Unit* u) {
    return !u->isDead && (u->team == player->index || u->visible || u->becomingInvis || !u->isHero());
}

static void sendInitialData(std::string* inputs) {
    for (Player* player : getActivePlayers()) {
        std::string& in = inputs[player->index];
        in += std::to_string(player->index) + "\n";

        in += std::to_string(G.bushes.size() + G.spawns.size()) + "\n";
        for (const Bush& bush : G.bushes) {
            in += "BUSH " + std::to_string(java::d2i(bush.x)) + " " + std::to_string(java::d2i(bush.y)) + " " +
                  std::to_string(java::d2i(bush.radius)) + "\n";
        }
        for (const Point& spawn : G.spawns) {
            in += "SPAWN " + std::to_string(java::d2i(spawn.x)) + " " + std::to_string(java::d2i(spawn.y)) + " 0\n";
        }

        in += std::to_string(G.items.size()) + "\n";
        for (Item* item : G.items) in += item->getPlayerString() + "\n";
    }
}

static void pickHero(Player* player, bool timeout, const std::vector<std::string>& outputs) {
    if (timeout || outputs.empty()) { // TimeoutException / IndexOutOfBoundsException
        deactivate(player);
        return;
    }
    Point spawn = player->index == 0 ? (player->heroes.empty() ? HEROSPAWNTEAM0 : HEROSPAWNTEAM0HERO2)
                                     : (player->heroes.empty() ? HEROSPAWNTEAM1 : HEROSPAWNTEAM1HERO2);
    const std::string& output = outputs[0];
    if (!player->heroes.empty() && player->heroes[0]->heroType == output) {
        // tried to pick a hero already owned
        deactivate(player);
        return;
    }
    Unit* hero = generateHero(output, player, spawn);
    if (hero == nullptr) {
        // supplied an invalid hero name
        deactivate(player);
        return;
    }
    player->heroes.push_back(hero);
    G.allUnits.push_back(hero);
}

static void gameTurn(int turn) {
    beforeTurn(turn, getActivePlayers());

    std::string inputs[2];
    if (turn == 0) sendInitialData(inputs);

    for (Player& player : players) {
        std::string& in = inputs[player.index];
        Player* other = getActivePlayers().at(1 - player.index);
        in += std::to_string(player.gold) + "\n";
        in += std::to_string(other->gold) + "\n";
        in += std::to_string(turn >= C::HEROCOUNT ? player.heroesAlive() : (turn - C::HEROCOUNT)) + "\n";
        int count = 0;
        for (Unit* u : G.allUnits)
            if (shouldSendToPlayer(&player, u)) count++;
        in += std::to_string(count) + "\n";
        for (Unit* unit : G.allUnits) {
            if (shouldSendToPlayer(&player, unit)) in += unit->getPlayerString() + "\n";
        }
    }

    for (Player& player : players) {
        int timeoutMs = player.hasNeverBeenExecuted ? C::FIRST_TURN_MAX_TIME : C::TURN_MAX_TIME;
        timeoutMs = (int)cg::turnTimeoutMs(timeoutMs, player.hasNeverBeenExecuted, turnTimeout);
        std::vector<std::string> outputs;
        bool ok = execute(agents[player.index], inputs[player.index], player.getExpectedOutputLines(), timeoutMs, outputs);
        player.hasNeverBeenExecuted = false;

        if (trace) {
            fprintf(trace, "T%d P%d %s", turn, player.index, ok ? "OUT" : "TIMEOUT");
            for (auto& line : outputs) fprintf(trace, " [%s]", line.c_str());
            fprintf(trace, "\n");
        }

        if (turn < C::HEROCOUNT) {
            pickHero(&player, !ok, outputs);
        } else if (!ok || !handlePlayerOutputs(&player, outputs)) {
            // timeout / invalid input
            deactivate(&player);
        }
    }

    std::vector<Player*> active = getActivePlayers();
    if (active.size() == 2) handleTurn(active);

    for (Player* player : active) {
        player->score = 0;
        bool deadHeroes = true;
        for (Unit* hero : player->heroes) deadHeroes = deadHeroes && hero->isDead;
        if (deadHeroes || player->tower->isDead) {
            deactivate(player);
        } else {
            player->score = player->unitKills + player->denies;
        }
    }

    if (trace) fprintf(trace, "  scores: %d %d\n", players[0].score, players[1].score);

    if (getActivePlayers().size() < 2) gameEnd = true;
}

static int runGame(const std::string& bot1, const std::string& bot2, i64 seed) {
    G.random.setSeed(seed);
    for (int i = 0; i < 2; i++) players[i].index = i;

    initialize();

    if (!agents[0].start(bot1, cg::AgentOptions())) { fprintf(stderr, "failed to start bot1\n"); return 1; }
    if (!agents[1].start(bot2, cg::AgentOptions())) { fprintf(stderr, "failed to start bot2\n"); return 1; }

    int rc = 0;
    try {
        for (int turn = 0; turn < C::ROUNDS && !gameEnd; turn++) gameTurn(turn);
    } catch (const RefereeCrash& crash) {
        // the Java referee dies here, the runner gives up and no scores are reported
        fprintf(stderr, "java.lang.IllegalArgumentException: Not a valid frame instant: %.17g\n", crash.t);
        rc = 1;
    }

    agents[0].destroy();
    agents[1].destroy();
    return rc;
}

int main(int argc, char** argv) {
    std::string bot1, bot2;
    i64 seed = (i64)std::chrono::steady_clock::now().time_since_epoch().count();
    const char* tracePath = nullptr;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-p1" && i + 1 < argc) bot1 = argv[++i];
        else if (a == "-p2" && i + 1 < argc) bot2 = argv[++i];
        else if (a == "-seed" && i + 1 < argc) {
            const char* s = argv[++i];
            auto r = std::from_chars(s, s + strlen(s), seed);
            if (r.ec != std::errc() || *r.ptr != 0) { fprintf(stderr, "invalid seed: %s\n", s); return 1; }
        } else if (a == "-l" && i + 1 < argc) tracePath = argv[++i];
        else if (a == "-timeout" && i + 1 < argc) {
            const char* s = argv[++i];
            auto r = std::from_chars(s, s + strlen(s), turnTimeout);
            if (r.ec != std::errc() || *r.ptr != 0) { fprintf(stderr, "invalid timeout: %s\n", s); return 1; }
        }
    }
    if (bot1.empty() || bot2.empty()) {
        fprintf(stderr, "usage: referee -p1 <cmd> -p2 <cmd> -seed N [-l file] [-timeout ms]\n");
        return 2;
    }
    if (tracePath) trace = fopen(tracePath, "w");
    int rc = runGame(bot1, bot2, seed);
    if (trace) fclose(trace);
    if (rc) return rc;
    printf("%d\n%d\n", players[0].score, players[1].score);
    return 0;
}
