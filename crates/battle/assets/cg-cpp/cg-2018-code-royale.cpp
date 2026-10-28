// Code Royale (CodinGame, 2018-04) - headless C++ referee, port of https://github.com/csj/code-royale
// (commit 349924a, 2019-03-16, = HEAD; Maven project at the repository root, pom artifactId
// code-royale; the CodinGame page links its older name csj/team-2), CodinGame game engine 1.30, game
// written in Kotlin 1.2.10, exactly 2 players, the rules of the highest league. Deterministic;
// reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints the 2 scores, one per line, then "seed=N" to stdout like the jar. When the Java referee would
// have failed, prints nothing and exits with code 1, like the jar (engine 1.x has no FAIL command: its
// runner waits 150 s for the dead referee, then throws out of the game). Exit code 1 also when the jar's
// command line interface fails (bad -seed, a bot that cannot be started). -l writes the trace of
// common/engine.h (one line per bot execution, view frame (with CG_TRACE_VIEWS its text), game summary
// and tooltip block, then "END OK|<why the game failed>").
//
// Leagues: Referee.init switches on the league level: 1 (Wood 3: no mines, towers or giants, 10 gold per
// turn, 6-12 site pairs), 2 (Wood 2: no mines, fixed income, 6-12 pairs), 3 (Wood 1: every rule, queens
// with 100 HP), anything else: every rule and the queens' HP drawn from the seed (25-100 by 5), 12 site
// pairs. The jar's command line interface sets league.level 19: that last branch, which CodinGame's
// Bronze, Silver, Gold and Legend leagues (levels 4-7) all play; it is what this port implements. The
// wood leagues are not implemented.
//
// Build: the pom's Kotlin 1.2.10 compiler does not run on JDK 17 (it cannot read the JDK's modules), so
// the jar is built with the builder's JDK 8 image, and with CommandLineInterface-v1.java (engine 1.x).
// The repository's last two game commits (2018-04-28, after the contest) are included: the collision
// order (creeps of both players, then both queens, then the sites) and the creeps of the second player
// spawning at barracks + (-i, -i) instead of + (i, i).
//
// Engine 1.30 (common/engine.h, cg::Options::engine25): its runner (Agent, CommandLinePlayerAgent,
// RefereeAgent, GameRunner's loop) is 2.2's; its GameManager is 2.2's except that (a) the turn loop does
// not stop when every player is inactive (here the referee always ends the game itself first), (b) the
// frame data a module sends in onAfterOnEnd is never sent (this port sends none, which gives the same
// last frame), (c) no METADATA command (the runner only stores it), (d) the summary needs 2+ players
// (always). So: the runner's split of the outputs (an empty second line gives the referee one line:
// "Index 1 out of bounds for length 1"), no summary quotas, turns numbered from 0 (the port passes
// turn - 1), 1000 ms for a bot's first turn and 50 ms after, 200 turns (set by the game).
//
// The Java (Kotlin) referee is followed closely: same data structures, iteration orders, RNG
// consumption, floating point operations, texts. Notable behaviours reproduced here:
//   - new java.util.Random(seed): the queens' HP (nextInt(16) + 5, times 5), then for each of the 12 site
//     pairs: max mine size, gold, radius, x, y (IntRange.sample = nextInt(last - first + 1) + first); 100
//     rounds of pair symmetrisation + collisionCheck with a 90 gap (whatever its outcome), snapped to
//     integers, +1 mine size / +50 gold within 500 and 200 of (960, 500).
//   - Vector2 (a data class: equals with Double.compare), Distance comparisons on squared lengths,
//     normalized (length below 1e-6: (1, 0)), towards, clampWithin, roundToInt (Math.round; NaN throws),
//     Math.atan2 (fdlibm) for the view's rotations; no fused multiply-add (FP_CONTRACT off).
//   - collisionCheck: every ordered pair (u1, u2) of the list, u1 clamped first, positions updated as it
//     goes; masses (sites 0: immovable against units, 0.5/0.5 + 20 between sites), the +1 gap; a unit
//     ignores a new location equal to (0, 0) (Unit.location's setter). allEntities = the creeps of both
//     players, both queens, the sites.
//   - the turn: TRAIN line first (Kotlin split(" "), eager map/map/map: every token parsed (toIntOrNull),
//     then every site looked up, then the barracks checks; warnings skip the rest of the line), then the
//     queen's line (Kotlin trim, split(" ")): WAIT / MOVE x y (Integer.parseInt) / BUILD id type (moves
//     towards the site when not touching it; the structure type is only checked when the building
//     happens, after both players' commands, and a building scheduled by a player killed for "Too many
//     tokens" still happens); two builds on the same site: the one of player (turn % 2) is dropped.
//     Creeps sorted by type (stable), 5 sub-steps of move + one collision round, damage, creeps tearing
//     down enemy mines, aging, queens tearing down enemy mines and barracks (the closest site only);
//     then every site acts in id order (mines, towers firing at the closest enemy creep, dead or alive,
//     else the queen, melting and recomputing their range, barracks training and spawning with 999
//     collision rounds); dead creeps removed; queens' health checked (both dead: a draw at 0); every
//     entity snapped to integers.
//   - scores: the queen's health (Player.health's delegate sets the score unless it is -1), -1 for a
//     player killed (invalid output, timeout).
//   - the view, because a game can reach the engine's 1 MB view data quota (measured: 520 KB for a
//     200-turn game of two moderate bots, 3 KB per turn with creeps): GraphicEntityModule 1.30 (= 2.2's
//     format: common/gem.h, Module(true)), the game's TooltipModule (every site's extra text every turn,
//     every damaged unit) and AnimModule (Gson: doubles with Double.toString), and the game's view code
//     (Unit/Queen/Creep sprites, structures, the HUD, the background). The per-frame runner buffer
//     (30000 bytes, 100000 for the first) is checked on the exact sizes too.
//   - Kotlin's top level vals: the background and HUD background sprites are created when MapBuildingKt
//     is first used (the HP draw), before the sites.
//   - 3 or 4 players: allEntities reads the third player's queen (lateinit, never set): the game fails
//     (the jar after 150 s).
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time and checks the time
// limits on the wall clock: on a loaded machine it may time out a bot that answered in time); each
// site's image "LC_<1-10>" drawn from an unseeded java.util.Random (the first frame's size varies by one
// char per "LC_10" in the jar; here a java.util.Random seeded with the game seed picks them); the order
// of the view's entity updates, group children and JSON object keys (identity hash codes and HashMaps in
// the jar: only the text order differs, not its size).
// Validated (2026-10-25) with battle referee-diff against the jar on 3759 games (35 bots: every ordered
// pair, every bot against itself and 28 targeted pairs, on seeds from 1, 1000001 and 987654321): 3759
// MATCH, no mismatch and no flake; and in direct mode (jar game log vs -l trace: every execution, every
// view frame's size, summaries, tooltips, scores) on 416 games, including raw output bytes (CRLF, CR,
// invalid UTF-8, Scanner line separators, a 29998 / 30001-byte summary, 10000 / 10001-byte outputs), 3
// and 4 players, extreme seeds and -timeout. The largest view data measured: 682 KB (no game reached
// the 1 MB quota).
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)

#include "common/engine.h"
#include "common/gem.h"

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using java::String;
using gem::Entity;

namespace {

// ----------------------------------------------------------------------------
// Constants (Constants, Leagues, CreepType)
// ----------------------------------------------------------------------------
const int STARTING_GOLD = 100;
const int QUEEN_SPEED = 60;
const int TOWER_HP_INITIAL = 200;
const int TOWER_HP_INCREMENT = 100;
const int TOWER_HP_MAXIMUM = 800;
const int TOWER_CREEP_DAMAGE_MIN = 3;
const int TOWER_CREEP_DAMAGE_CLIMB_DISTANCE = 200;
const int TOWER_QUEEN_DAMAGE_MIN = 1;
const int TOWER_QUEEN_DAMAGE_CLIMB_DISTANCE = 200;
const int TOWER_MELT_RATE = 4;
const int TOWER_COVERAGE_PER_HP = 1000;
const int GIANT_BUST_RATE = 80;
const int OBSTACLE_GAP = 90;
const int OBSTACLE_GOLD_INCREASE = 50;
const int OBSTACLE_GOLD_INCREASE_DISTANCE_1 = 500;
const int OBSTACLE_GOLD_INCREASE_DISTANCE_2 = 200;
const int KNIGHT_DAMAGE = 1;
const int ARCHER_DAMAGE = 2;
const int ARCHER_DAMAGE_TO_GIANTS = 10;
const int QUEEN_RADIUS = 30;
const int QUEEN_MASS = 10000;
const int QUEEN_HP_FIRST = 5, QUEEN_HP_LAST = 20; // QUEEN_HP = 5..20
const int QUEEN_HP_MULT = 5;
const int QUEEN_VISION = 300;
const int VIEWPORT_X_FIRST = 0, VIEWPORT_X_LAST = 1920; // viewportX = 0..1920
const int VIEWPORT_Y_FIRST = 0, VIEWPORT_Y_LAST = 1000; // viewportY = 0..1000
const int WORLD_WIDTH = VIEWPORT_X_LAST - VIEWPORT_X_FIRST;
const int WORLD_HEIGHT = VIEWPORT_Y_LAST - VIEWPORT_Y_FIRST;
const int TOUCHING_DELTA = 5;
const int OBSTACLE_GOLD_RANGE_LAST = 250;
const double PI = 3.141592653589793; // Math.PI

// Leagues (the "else" branch of Referee.init: towers, giants, mines, no fixed income, 12 pairs)
const int LEAGUE_OBSTACLES = 12; // OBSTACLE_PAIRS.last
int leagueQueenHp = 100;

enum CreepType { KNIGHT, ARCHER, GIANT };
struct CreepTypeInfo {
    int count, cost, speed, range, radius, mass, hp, buildTime;
    const char* assetName;
    const char* name;
};
const CreepTypeInfo CREEP_TYPES[3] = {
    {4, 80, 100, 0, 20, 400, 30, 5, "Unite_Fantassin", "KNIGHT"},
    {2, 100, 75, 200, 25, 900, 45, 8, "Unite_Archer", "ARCHER"},
    {1, 140, 50, 0, 40, 2000, 200, 10, "Unite_Siege", "GIANT"},
};

// ----------------------------------------------------------------------------
// Vector2 / Distance
// ----------------------------------------------------------------------------
struct V2 {
    double x = 0, y = 0;
    V2() {}
    V2(double x_, double y_) : x(x_), y(y_) {}
    static V2 ofInt(int x, int y) { return V2((double)x, (double)y); }

    V2 operator-(const V2& o) const { return V2(x - o.x, y - o.y); }
    V2 operator+(const V2& o) const { return V2(x + o.x, y + o.y); }
    V2 operator*(double k) const { return V2(x * k, y * k); }
    V2 div(double k) const {
        if (k == 0.0) throw java::Exception{"java.lang.IllegalArgumentException: Division by zero"};
        return V2(x / k, y / k);
    }
    V2 neg() const { return V2(-x, -y); }
    double lengthSquared() const { return x * x + y * y; }
    // Distance.toDouble
    double length() const { return std::sqrt(lengthSquared()); }
    V2 normalized() const {
        double ls = lengthSquared();
        if (ls < 1e-6 * 1e-6) return V2(1.0, 0.0); // len < 1e-6
        double l = std::sqrt(ls);
        return V2(x / l, y / l);
    }
    V2 resizedTo(double newLength) const { return normalized() * newLength; }
    // distanceTo(other): the squared distance (Distance compares squares)
    double sqDist(const V2& o) const { return (*this - o).lengthSquared(); }
    double dist(const V2& o) const { return (*this - o).length(); }
    V2 towards(const V2& other, double maxDistance) const {
        if (sqDist(other) < maxDistance * maxDistance) return other;
        return *this + (other - *this).resizedTo(maxDistance);
    }
    V2 clampWithin(double minX, double maxX, double minY, double maxY) const {
        double nx = x < minX ? minX : (x > maxX ? maxX : x);
        double ny = y < minY ? minY : (y > maxY ? maxY : y);
        return V2(nx, ny);
    }
    double angle() const { return java::atan2(y, x); }
    V2 snapToIntegers() const;
    // data class equals (Double.compare)
    bool equals(const V2& o) const { return java::compareDouble(x, o.x) == 0 && java::compareDouble(y, o.y) == 0; }
};

// Distance < / > / >= an Int: squareDistance compared with the square (an int product)
bool lessThan(double sq, int d) { return sq < (double)(d * d); }
bool greaterThan(double sq, int d) { return sq > (double)(d * d); }

// kotlin.math.roundToInt(Double)
int roundToInt(double v) {
    if (v != v) throw java::Exception{"java.lang.IllegalArgumentException: Cannot round NaN value."};
    if (v > (double)INT32_MAX) return INT32_MAX;
    if (v < (double)INT32_MIN) return INT32_MIN;
    return java::l2i(java::round(v));
}

V2 V2::snapToIntegers() const { return V2::ofInt(roundToInt(x), roundToInt(y)); }

const V2 ZERO(0.0, 0.0);

// ----------------------------------------------------------------------------
// View: GraphicEntityModule, the game's TooltipModule and AnimModule
// ----------------------------------------------------------------------------
gem::Module gem_(true); // engine 1.30 = 2.2's command list

String S(const std::string& ascii) { return java::ascii(ascii); }
String I(i64 v) { return java::str(v); }

// tooltipModule.TooltipModule: "tooltips" = [newRegistrations, newExtra], every frame
struct TooltipModule {
    std::map<int, std::string> registrations, newRegistrations; // id -> params as JSON
    std::map<int, std::vector<String>> newExtra;

    void registerEntity(const Entity* e, const std::string& paramsJson) {
        auto it = registrations.find(e->id);
        if (it == registrations.end() || it->second != paramsJson) {
            newRegistrations[e->id] = paramsJson;
            registrations[e->id] = paramsJson;
        }
    }
    // lines.equals(extra.get(id)) compares the arrays by identity: always a new entry
    void updateExtraTooltipText(const Entity* e, const std::vector<String>& lines) { newExtra[e->id] = lines; }

    void sendFrameData() {
        std::string s = "[{";
        bool first = true;
        for (auto& [id, params] : newRegistrations) {
            if (!first) s += ",";
            first = false;
            s += "\"" + std::to_string(id) + "\":" + params;
        }
        s += "},{";
        first = true;
        for (auto& [id, lines] : newExtra) {
            if (!first) s += ",";
            first = false;
            s += "\"" + std::to_string(id) + "\":[";
            for (size_t i = 0; i < lines.size(); i++) s += (i ? "," : "") + java::jsonQuote(lines[i]);
            s += "]";
        }
        s += "}]";
        cg::gm.setViewData("tooltips", cg::Json::raw(s));
        newRegistrations.clear();
        newExtra.clear();
    }
};
TooltipModule tooltips;

// anims.AnimModule: "anims" = the Anim events of the frame (Gson: t, id, params {x, y} as doubles)
struct AnimModule {
    std::vector<std::string> events;
    void createAnimationEvent(const char* id, double t, const V2& location) {
        events.push_back("{\"t\":" + java::doubleToString(t) + ",\"id\":\"" + id + "\",\"params\":{\"x\":" + java::doubleToString(location.x) + ",\"y\":" +
                         java::doubleToString(location.y) + "}}");
    }
    void sendFrameData() {
        std::string s = "[";
        for (size_t i = 0; i < events.size(); i++) s += (i ? "," : "") + events[i];
        cg::gm.setViewData("anims", cg::Json::raw(s + "]"));
        events.clear();
    }
};
AnimModule anims;

// Entity<T>.location's setter (Characters.kt): x = (value.x + viewportX.first).toInt()
void entSetLocation(Entity* e, const V2& v) {
    gem_.setX(e, java::d2i(v.x + VIEWPORT_X_FIRST));
    gem_.setY(e, java::d2i(v.y + VIEWPORT_Y_FIRST));
}
// its getter
V2 getLocation(const Entity* e) { return V2::ofInt(e->x - VIEWPORT_X_FIRST, e->y - VIEWPORT_Y_FIRST); }

// ----------------------------------------------------------------------------
// Field objects
// ----------------------------------------------------------------------------
struct Player;
struct Obstacle;
struct Creep;

struct FieldObject {
    virtual ~FieldObject() {}
    virtual V2 location() const = 0;
    virtual void setLocation(const V2& v) = 0;
    virtual int radius() const = 0;
    virtual int mass() const = 0; // 0: immovable
};

struct Unit : FieldObject {
    Player* owner;
    Entity* tokenCircle;
    Entity* characterSprite;
    Entity* tokenGroup;
    V2 loc = ZERO;
    int healthField = 0;

    explicit Unit(Player* owner_);
    V2 location() const override { return loc; }
    void setLocation(const V2& v) override {
        if (v.equals(ZERO)) return;
        loc = v;
        entSetLocation(tokenGroup, v);
    }
    virtual int health() const { return healthField; }
    virtual void setHealth(int v) { unitSetHealth(v); }
    void unitSetHealth(int v) {
        healthField = v;
        if (v < 0) healthField = 0;
        tooltips.updateExtraTooltipText(tokenCircle, {S("Health: ") + I(health())});
    }
    virtual void damage(int damageAmount) = 0;
    virtual bool isGiant() const { return false; }
    void commitState(double t) { gem_.commitEntityState(t, {tokenCircle, characterSprite, tokenGroup}); }
};

struct Queen : Unit {
    explicit Queen(Player* owner_) : Unit(owner_) {
        gem_.setImage(characterSprite, "Unite_Reine");
        tooltips.registerEntity(tokenGroup, "{\"type\":\"Queen\"}");
        gem_.setBaseWidth(tokenCircle, QUEEN_RADIUS * 2);
        gem_.setBaseHeight(tokenCircle, QUEEN_RADIUS * 2);
        gem_.setBaseWidth(characterSprite, QUEEN_RADIUS * 2);
        gem_.setBaseHeight(characterSprite, QUEEN_RADIUS * 2);
    }
    int radius() const override { return QUEEN_RADIUS; }
    int mass() const override { return QUEEN_MASS; }
    void moveTowards(const V2& target) { setLocation(loc.towards(target, (double)QUEEN_SPEED)); }
    void damage(int damageAmount) override;
};

// ----------------------------------------------------------------------------
// Structures
// ----------------------------------------------------------------------------
enum StructureKind { MINE, TOWER, BARRACKS };

struct Structure {
    Player* owner;
    Obstacle* obstacle;
    StructureKind kind;
    Structure(Obstacle* o, Player* p, StructureKind k) : owner(p), obstacle(o), kind(k) {}
    virtual ~Structure() {}
    virtual void updateEntities() = 0;
    virtual void hideEntities() = 0;
    virtual std::vector<String> extraTooltipLines() = 0;
    virtual bool act() = 0; // true: the structure is destroyed
};

struct Obstacle : FieldObject {
    int obstacleId;
    int maxMineSize;
    int goldField;
    Entity* obstacleImage;
    int radiusField = 0;
    V2 loc;
    double area;
    Structure* structureField = nullptr;

    Obstacle(int id, int maxMineSize_, int initialGold, int initialRadius, const V2& initialLocation, int imageNumber)
        : obstacleId(id), maxMineSize(maxMineSize_), goldField(initialGold), loc(initialLocation) {
        obstacleImage = gem_.createSprite();
        gem_.setImage(obstacleImage, S("LC_") + I(imageNumber));
        gem_.setZIndex(obstacleImage, 20);
        gem_.setAnchor(obstacleImage, 0.5);
        setRadius(initialRadius);
        setLocation(initialLocation);
        tooltips.registerEntity(obstacleImage, "{\"id\":" + std::to_string(obstacleId) + ",\"type\":\"Site\"}");
        area = PI * radiusField * radiusField;
    }
    int gold() const { return goldField; }
    void setGold(int v) { goldField = v < 0 ? 0 : v; } // nonNegative
    int radius() const override { return radiusField; }
    int mass() const override { return 0; }
    void setRadius(int v) {
        radiusField = v;
        gem_.setScale(obstacleImage, (v * 2) / 220.0);
    }
    V2 location() const override { return loc; }
    void setLocation(const V2& v) override {
        loc = v;
        entSetLocation(obstacleImage, loc);
    }
    Structure* structure() const { return structureField; }
    void setStructure(Structure* value) {
        if (value != nullptr && structureField != value) anims.createAnimationEvent("construction", 0.0, loc); // includes replacing
        if (value == nullptr && structureField != nullptr) anims.createAnimationEvent("destruction", 0.0, loc);
        if (structureField) structureField->hideEntities();
        structureField = value;
        if (value) value->updateEntities();
        if (value == nullptr) {
            gem_.setAlpha(obstacleImage, 1.0);
        } else {
            gem_.setAlpha(obstacleImage, 0.0);
            gem_.setImage(obstacleImage, "LieuDetruit");
        }
        gem_.setAlpha(obstacleImage, value == nullptr ? 1.0 : 0.0);
        gem_.commitEntityState(0.0, {obstacleImage});
    }
    void updateEntities() {
        if (structureField) structureField->updateEntities();
        std::vector<String> lines = {S("Radius: ") + I(radiusField)};
        lines.push_back(S("Remaining gold: ") + I(goldField)); // Leagues.mines
        if (structureField)
            for (const String& l : structureField->extraTooltipLines()) lines.push_back(l);
        tooltips.updateExtraTooltipText(obstacleImage, lines);
    }
    void act() {
        if (structureField && structureField->act()) setStructure(nullptr);
        updateEntities();
    }
    void setMine(Player* owner);
    void setTower(Player* owner, int health);
    void setBarracks(Player* owner, CreepType creepType);
};

std::vector<Obstacle*> obstacles;

// ----------------------------------------------------------------------------
// Players and their HUD
// ----------------------------------------------------------------------------
struct PlayerHUD;

struct Player : cg::Player {
    Queen* queenUnit = nullptr;
    Player* enemyPlayer = nullptr;
    bool isSecondPlayer = false;
    std::vector<Creep*> activeCreeps;
    int healthField = 100; // nonNegative(100).andAlso { if (score >= 0) score = it }
    int gold = STARTING_GOLD;
    int goldPerTurn = 0;
    PlayerHUD* hudField = nullptr;

    int getExpectedOutputLines() const override { return 2; }
    int health() const { return healthField; }
    void setHealth(int v) {
        healthField = v < 0 ? 0 : v;
        if (score >= 0) score = healthField;
    }
    int fixOwner(const Player* p) const { return p == nullptr ? -1 : (p == this ? 0 : 1); }
    String avatarToken() const { return S("$") + I(index); }
    int colorToken() const { return -(index + 1); }
    PlayerHUD& hud();
    void checkQueenHealth();
    void kill(const String& reason) {
        score = -1;
        deactivate(reason);
    }
    void printObstacleInit(const Obstacle* o) {
        sendInputLine(std::to_string(o->obstacleId) + " " + std::to_string(roundToInt(o->loc.x)) + " " + std::to_string(roundToInt(o->loc.y)) + " " +
                      std::to_string(o->radiusField));
    }
    void printObstaclePerTurn(const Obstacle* o);
};

Player players[4];

void Queen::damage(int damageAmount) {
    if (damageAmount <= 0) return;
    owner->setHealth(owner->health() - damageAmount);
}

Unit::Unit(Player* owner_) : owner(owner_) {
    tokenCircle = gem_.createSprite();
    gem_.setImage(tokenCircle, owner->isSecondPlayer ? "Unite_Base_Bleu" : "Unite_Base_Rouge");
    gem_.setAnchor(tokenCircle, 0.5);
    gem_.setZIndex(tokenCircle, 40);
    characterSprite = gem_.createSprite();
    gem_.setZIndex(characterSprite, 41);
    gem_.setScale(characterSprite, 1.2);
    gem_.setAnchor(characterSprite, 0.5);
    tokenGroup = gem_.createGroup({tokenCircle, characterSprite});
}

struct PlayerHUD {
    Player* player;
    int top = VIEWPORT_Y_LAST, bottom = 1080;
    int healthBarWidth = 420;
    Entity* avatar;
    Entity* healthBarFillMask;
    Entity* healthBarFill;
    Entity* playerName;
    Entity* healthText;
    Entity* moneyText;
    Entity* moneyIncText;

    PlayerHUD(Player* p, bool isSecondPlayer) : player(p) {
        avatar = gem_.createSprite();
        gem_.setImage(avatar, player->avatarToken());
        gem_.setY(avatar, bottom);
        gem_.setAnchorY(avatar, 1.0);
        if (isSecondPlayer) {
            gem_.setX(avatar, 1920);
            gem_.setAnchorX(avatar, 1.0);
        } else {
            gem_.setX(avatar, 0);
        }
        gem_.setBaseWidth(avatar, 140);
        gem_.setBaseHeight(avatar, 140);
        gem_.setZIndex(avatar, 4003);
        // init
        Entity* maskCircle = gem_.createCircle();
        gem_.setRadius(maskCircle, 64);
        gem_.setX(maskCircle, isSecondPlayer ? 1920 - 69 : 69);
        gem_.setY(maskCircle, bottom - 68);
        gem_.setMask(avatar, maskCircle);

        healthBarFillMask = gem_.createRectangle();
        gem_.setLineAlpha(healthBarFillMask, 0.0);
        gem_.setY(healthBarFillMask, top + 39);
        gem_.setX(healthBarFillMask, isSecondPlayer ? 1920 - 139 - healthBarWidth : 143);
        gem_.setWidth(healthBarFillMask, healthBarWidth);
        gem_.setHeight(healthBarFillMask, 28);
        gem_.setFillColor(healthBarFillMask, player->colorToken());
        gem_.setFillAlpha(healthBarFillMask, 0.0);
        gem_.setLineWidth(healthBarFillMask, 0);
        gem_.setZIndex(healthBarFillMask, 4002);

        healthBarFill = gem_.createSprite();
        gem_.setImage(healthBarFill, isSecondPlayer ? "Life-Bleu" : "Life-Rouge");
        gem_.setX(healthBarFill, isSecondPlayer ? 1920 - 139 - healthBarWidth : 143);
        gem_.setY(healthBarFill, top + 39);
        gem_.setZIndex(healthBarFill, 4002);
        gem_.setMask(healthBarFill, healthBarFillMask);

        playerName = gem_.createText(player->getNicknameToken());
        gem_.setY(playerName, bottom - 45);
        gem_.setAnchorY(playerName, 1.0);
        if (isSecondPlayer) {
            gem_.setX(playerName, 1770);
            gem_.setAnchorX(playerName, 1.0);
        } else {
            gem_.setX(playerName, 150);
        }
        gem_.setFillColor(playerName, 0xffffff);
        gem_.setScale(playerName, 1.8);
        gem_.setFontFamily(playerName, "Arial Black");
        gem_.setZIndex(playerName, 4003);

        healthText = gem_.createText(I(player->health()));
        gem_.setX(healthText, healthBarFillMask->x + healthBarFillMask->width - 10);
        gem_.setY(healthText, healthBarFillMask->y + healthBarFillMask->height / 2);
        gem_.setAnchorX(healthText, 1.0);
        gem_.setAnchorY(healthText, 0.5);
        gem_.setScale(healthText, 1.3);
        gem_.setFontFamily(healthText, "Arial Black");
        gem_.setFillColor(healthText, 0xffffff);
        gem_.setZIndex(healthText, 4003);

        moneyText = gem_.createText(u"0");
        gem_.setY(moneyText, bottom - 40);
        gem_.setAnchorY(moneyText, 0.5);
        gem_.setX(moneyText, isSecondPlayer ? 1020 : 700);
        gem_.setFillColor(moneyText, 0xffffff);
        gem_.setScale(moneyText, 1.8);
        gem_.setFontFamily(moneyText, "Arial Black");
        gem_.setZIndex(moneyText, 4002);

        moneyIncText = gem_.createText(u"");
        gem_.setY(moneyIncText, bottom - 40);
        gem_.setAnchorY(moneyIncText, 0.5);
        gem_.setX(moneyIncText, isSecondPlayer ? 1220 : 910);
        gem_.setAnchorX(moneyIncText, 1.0);
        gem_.setFillColor(moneyIncText, 0xffffff);
        gem_.setScale(moneyIncText, 1.2);
        gem_.setFontFamily(moneyIncText, "Arial Black");
        gem_.setZIndex(moneyIncText, 4002);
    }

    void update() {
        gem_.setWidth(healthBarFillMask, healthBarWidth * player->health() / (QUEEN_HP_LAST * QUEEN_HP_MULT));
        gem_.setText(healthText, I(player->health()));
        gem_.setText(moneyText, I(player->gold));
        gem_.setText(moneyIncText, player->goldPerTurn == 0 ? String() : S("+") + I(player->goldPerTurn));
        gem_.commitEntityState(0.0, {moneyText, moneyIncText});
    }
};

PlayerHUD& Player::hud() {
    if (!hudField) hudField = new PlayerHUD(this, isSecondPlayer);
    return *hudField;
}

void Player::checkQueenHealth() {
    queenUnit->setHealth(health());
    if (health() == 0) deactivate(u"Dead queen");
    hud().update();
}

// ----------------------------------------------------------------------------
// Creeps
// ----------------------------------------------------------------------------
struct Creep : Unit {
    CreepType creepType;
    int speed, attackRange, massField, maxHealth, radiusField;

    Creep(Player* owner_, CreepType t) : Unit(owner_), creepType(t) {
        const CreepTypeInfo& c = CREEP_TYPES[t];
        speed = c.speed;
        attackRange = c.range;
        massField = c.mass;
        maxHealth = c.hp;
        radiusField = c.radius;
        // init
        Creep::setHealth(c.hp);
        gem_.setBaseWidth(tokenCircle, radiusField * 2);
        gem_.setBaseHeight(tokenCircle, radiusField * 2);
        gem_.setImage(characterSprite, c.assetName);
        gem_.setBaseWidth(characterSprite, radiusField * 2);
        gem_.setBaseHeight(characterSprite, radiusField * 2);
        tooltips.registerEntity(tokenGroup, std::string("{\"type\":\"") + c.name + "\"}");
    }
    int radius() const override { return radiusField; }
    int mass() const override { return massField; }
    virtual void finalizeFrame() {}
    void damage(int damageAmount) override {
        if (damageAmount <= 0) return; // no accidental healing!
        setHealth(health() - damageAmount);
        tooltips.updateExtraTooltipText(tokenCircle, {S("Health: ") + I(health())});
    }
    virtual void dealDamage() = 0;
    virtual void move(double frames) = 0;
    int health() const override { return healthField; }
    void setHealth(int v) override {
        unitSetHealth(v);
        if (healthField == 0) {
            gem_.setAlpha(characterSprite, 0.0);
            gem_.setAlpha(tokenCircle, 0.0);
        } else {
            gem_.setAlpha(tokenCircle, 0.8 * health() / maxHealth + 0.2);
        }
    }
};

// activeCreeps.minBy { it.location.distanceTo(location) }
Creep* closestCreep(const std::vector<Creep*>& creeps, const V2& to) {
    Creep* best = nullptr;
    double bd = 0;
    for (Creep* c : creeps) {
        double d = c->location().sqDist(to);
        if (best == nullptr || d < bd) {
            best = c;
            bd = d;
        }
    }
    return best;
}

struct Tower;

struct GiantCreep : Creep {
    GiantCreep(Player* owner_, CreepType t) : Creep(owner_, t) {}
    bool isGiant() const override { return true; }
    void move(double frames) override;
    void dealDamage() override;
};

struct KnightCreep : Creep {
    std::optional<V2> lastLocation;
    bool attacksThisTurn = false;
    KnightCreep(Player* owner_, CreepType t) : Creep(owner_, t) {}
    void finalizeFrame() override;
    void move(double frames) override;
    void dealDamage() override;
};

struct ArcherCreep : Creep {
    std::optional<V2> lastLocation;
    Creep* attackTarget = nullptr;
    Entity* projectile;
    ArcherCreep(Player* owner_, CreepType t) : Creep(owner_, t) {
        projectile = gem_.createSprite();
        gem_.setZIndex(projectile, 60);
        gem_.setImage(projectile, owner->isSecondPlayer ? "Fleche_Bleu" : "Fleche_Rouge");
        gem_.setVisible(projectile, false);
        gem_.setAnchorX(projectile, 1.0);
        gem_.setAnchorY(projectile, 0.5);
    }
    Creep* findTarget() const;
    void finalizeFrame() override;
    void move(double frames) override;
    void dealDamage() override;
};

// ----------------------------------------------------------------------------
// Structures (Mine, Tower, Barracks)
// ----------------------------------------------------------------------------
struct Mine : Structure {
    Entity* mineImage;
    Entity* pickaxeSprite;
    Entity* text;
    Entity* mineralBarOutline;
    Entity* mineralBarFill;
    int incomeRateField;

    Mine(Obstacle* o, Player* p, int incomeRate) : Structure(o, p, MINE), incomeRateField(incomeRate) {
        mineImage = gem_.createSprite();
        gem_.setImage(mineImage, "Mine");
        gem_.setZIndex(mineImage, 40);
        gem_.setAnchor(mineImage, 0.5);
        entSetLocation(mineImage, o->loc);
        gem_.setScale(mineImage, (o->radiusField * 2) / 220.0);

        pickaxeSprite = gem_.createSprite();
        gem_.setImage(pickaxeSprite, p->isSecondPlayer ? "Mine_Bleu" : "Mine_Rouge");
        gem_.setZIndex(pickaxeSprite, 41);
        entSetLocation(pickaxeSprite, o->loc + V2::ofInt(0, -40));
        gem_.setAnchor(pickaxeSprite, 0.5);

        text = gem_.createText(S("+") + I(incomeRate));
        gem_.setFillColor(text, 0xffffff);
        gem_.setZIndex(text, 42);
        gem_.setFontFamily(text, "Arial Black");
        gem_.setAnchorY(text, 0.5);
        entSetLocation(text, o->loc + V2::ofInt(0, -40));

        mineralBarOutline = gem_.createRectangle();
        entSetLocation(mineralBarOutline, o->loc + V2::ofInt(-40, -90));
        gem_.setHeight(mineralBarOutline, 15);
        gem_.setWidth(mineralBarOutline, 80);
        gem_.setLineColor(mineralBarOutline, 0);
        gem_.setLineWidth(mineralBarOutline, 1);
        gem_.setFillAlpha(mineralBarOutline, 0.0);
        gem_.setZIndex(mineralBarOutline, 401);

        mineralBarFill = gem_.createRectangle();
        entSetLocation(mineralBarFill, o->loc + V2::ofInt(-40, -90));
        gem_.setHeight(mineralBarFill, 15);
        gem_.setWidth(mineralBarFill, 80);
        gem_.setFillColor(mineralBarFill, 0xffbf00);
        gem_.setLineAlpha(mineralBarFill, 0.0);
        gem_.setZIndex(mineralBarFill, 400);
    }
    int incomeRate() const { return incomeRateField; }
    void setIncomeRate(int v) {
        incomeRateField = v;
        gem_.setText(text, S("+") + I(incomeRateField));
    }
    std::vector<String> extraTooltipLines() override { return {S("MINE (+") + I(incomeRateField) + u")"}; }
    void hideEntities() override {
        gem_.setVisible(text, false);
        gem_.setVisible(pickaxeSprite, false);
        gem_.setVisible(mineImage, false);
        gem_.setVisible(mineralBarOutline, false);
        gem_.setVisible(mineralBarFill, false);
        gem_.commitEntityState(0.51, {text, pickaxeSprite, mineImage, mineralBarOutline, mineralBarFill});
    }
    void updateEntities() override {
        gem_.setVisible(text, true);
        gem_.setVisible(pickaxeSprite, true);
        gem_.setVisible(mineImage, true);
        gem_.setVisible(mineralBarOutline, true);
        gem_.setVisible(mineralBarFill, true);
        gem_.setWidth(mineralBarFill, 80 * obstacle->gold() / (OBSTACLE_GOLD_RANGE_LAST + 2 * OBSTACLE_GOLD_INCREASE));
        gem_.commitEntityState(0.5, {text, pickaxeSprite, mineImage, mineralBarOutline, mineralBarFill});
    }
    bool act() override;
};

struct Tower : Structure {
    int attackRadius;
    int health;
    Entity* towerRangeCircle;
    Entity* sprite;
    Entity* projectile;
    FieldObject* attackTarget = nullptr;

    Tower(Obstacle* o, Player* p, int attackRadius_, int health_) : Structure(o, p, TOWER), attackRadius(attackRadius_), health(health_) {
        towerRangeCircle = gem_.createCircle();
        gem_.setFillAlpha(towerRangeCircle, 0.0);
        gem_.setAlpha(towerRangeCircle, 0.2);
        gem_.setLineWidth(towerRangeCircle, 10);
        gem_.setZIndex(towerRangeCircle, 10);
        entSetLocation(towerRangeCircle, o->loc);
        gem_.setRadius(towerRangeCircle, o->radiusField);
        gem_.commitEntityState(0.0, {towerRangeCircle});

        sprite = gem_.createSpriteAnimation();
        std::vector<std::string> images;
        const char* color = p->isSecondPlayer ? "B" : "R";
        for (int i = 1; i <= 15; i++) images.push_back(std::string("T") + color + (i < 10 ? "0" : "") + std::to_string(i));
        gem_.setImages(sprite, images);
        gem_.setZIndex(sprite, 40);
        entSetLocation(sprite, o->loc);
        gem_.setAnchorX(sprite, 0.5);
        gem_.setAnchorY(sprite, 1 - (220.0 / 238.0 * 0.5));
        gem_.setStarted(sprite, true);
        gem_.setLoop(sprite, true);
        gem_.setScale(sprite, (o->radiusField * 2) / 220.0);

        projectile = gem_.createSprite();
        gem_.setImage(projectile, p->isSecondPlayer ? "Eclair_Bleu" : "Eclair_Rouge");
        gem_.setZIndex(projectile, 50);
        gem_.setVisible(projectile, false);
        gem_.setAnchorX(projectile, 0.5);
        gem_.setAnchorY(projectile, 0.5);
    }
    std::vector<String> extraTooltipLines() override { return {u"TOWER", S("Range: ") + I(attackRadius), S("Health: ") + I(health)}; }
    void hideEntities() override {
        gem_.setRadius(towerRangeCircle, obstacle->radiusField);
        gem_.setVisible(towerRangeCircle, false);
        gem_.setVisible(sprite, false);
        gem_.commitEntityState(0.51, {towerRangeCircle, sprite});
    }
    void updateEntities() override {
        gem_.setVisible(towerRangeCircle, true);
        gem_.setLineColor(towerRangeCircle, owner->isSecondPlayer ? 0x8844ff : 0xff4444);
        gem_.setVisible(sprite, true);
        gem_.commitEntityState(0.5, {towerRangeCircle, sprite});
        gem_.setRadius(towerRangeCircle, attackRadius);
        gem_.commitEntityState(1.0, {towerRangeCircle});

        if (attackTarget != nullptr) {
            gem_.setVisible(projectile, true);
            V2 projectileSource = obstacle->loc - V2(0.0, obstacle->radiusField * 0.6);
            V2 obsToTarget = attackTarget->location() - projectileSource;
            entSetLocation(projectile, (projectileSource + attackTarget->location()).div(2.0));
            gem_.setScaleX(projectile, obsToTarget.length() / 200.0);
            gem_.setScaleY(projectile, 1.0);
            gem_.setRotation(projectile, obsToTarget.angle(), gem::IMMEDIATE);
            gem_.commitEntityState(0.0, {projectile});
            gem_.setRotation(projectile, obsToTarget.neg().angle(), gem::IMMEDIATE);
            gem_.setScaleY(projectile, 2.0);
            gem_.commitEntityState(0.2, {projectile});
            gem_.setRotation(projectile, obsToTarget.angle(), gem::IMMEDIATE);
            gem_.commitEntityState(0.4, {projectile});
            gem_.setRotation(projectile, obsToTarget.neg().angle(), gem::IMMEDIATE);
            gem_.setScaleY(projectile, 1.0);
            gem_.commitEntityState(0.6, {projectile});
            gem_.setRotation(projectile, obsToTarget.angle(), gem::IMMEDIATE);
            gem_.commitEntityState(0.8, {projectile});
            gem_.setRotation(projectile, obsToTarget.neg().angle(), gem::IMMEDIATE);
            gem_.commitEntityState(0.99, {projectile});
            gem_.setVisible(projectile, false);
            gem_.commitEntityState(1.0, {projectile});
        }
    }
    void damageCreep(Creep* target) {
        double shotDistance = target->location().dist(obstacle->loc) - obstacle->radiusField;
        double differenceFromMax = attackRadius - shotDistance;
        int damage = TOWER_CREEP_DAMAGE_MIN + java::d2i(differenceFromMax / TOWER_CREEP_DAMAGE_CLIMB_DISTANCE);
        target->damage(damage);
    }
    void damageQueen(Queen* target) {
        double shotDistance = target->location().dist(obstacle->loc) - obstacle->radiusField;
        double differenceFromMax = attackRadius - shotDistance;
        int damage = TOWER_QUEEN_DAMAGE_MIN + java::d2i(differenceFromMax / TOWER_QUEEN_DAMAGE_CLIMB_DISTANCE);
        target->damage(damage);
    }
    bool act() override;
};

struct Barracks : Structure {
    CreepType creepType;
    int progressFillMaxWidth;
    Entity* progressFillMask;
    int progressMax;
    int progress = 0;
    bool isTraining = false;
    Player* onCompletePlayer = nullptr; // onComplete: { } until a TRAIN sets it
    Entity* barracksImage;
    Entity* progressFill;
    Entity* creepToken;
    Entity* creepSprite;

    Barracks(Obstacle* o, Player* p, CreepType t) : Structure(o, p, BARRACKS), creepType(t) {
        int r = o->radiusField;
        progressFillMaxWidth = java::d2i(r * 1.05);
        progressFillMask = gem_.createRectangle();
        entSetLocation(progressFillMask, o->loc + V2(-0.51, 0.72) * (double)r);
        gem_.setHeight(progressFillMask, 8);
        gem_.setWidth(progressFillMask, progressFillMaxWidth);
        progressMax = CREEP_TYPES[t].buildTime;

        barracksImage = gem_.createSprite();
        gem_.setAnchor(barracksImage, 0.5);
        gem_.setImage(barracksImage, p->isSecondPlayer ? "Caserne_Bleu" : "Caserne_Rouge");
        gem_.setZIndex(barracksImage, 40);
        entSetLocation(barracksImage, o->loc);
        gem_.setBaseHeight(barracksImage, r * 2);
        gem_.setBaseWidth(barracksImage, r * 2);

        progressFill = gem_.createSprite();
        gem_.setAnchor(progressFill, 0.5);
        gem_.setImage(progressFill, p->isSecondPlayer ? "Caserne_Bleu_Jauge" : "Caserne_Rouge_Jauge");
        gem_.setZIndex(progressFill, 400);
        entSetLocation(progressFill, o->loc + V2::ofInt(0, r * 53 / 68));
        gem_.setBaseWidth(progressFill, r * 2 * 70 / 138);
        gem_.setBaseHeight(progressFill, java::d2i((r * 2 * 70 / 138) * 10.0 / 110.0));
        gem_.setMask(progressFill, progressFillMask);

        int tokenSize = java::d2i((r * 2) * 0.32); // barracksImage.baseHeight * 0.32 (and baseWidth)
        creepToken = gem_.createSprite();
        gem_.setAnchor(creepToken, 0.5);
        gem_.setImage(creepToken, p->isSecondPlayer ? "Unite_Base_Bleu" : "Unite_Base_Rouge");
        gem_.setZIndex(creepToken, 41);
        entSetLocation(creepToken, o->loc + V2(-0.1, -0.45) * (double)r);
        gem_.setBaseHeight(creepToken, tokenSize);
        gem_.setBaseWidth(creepToken, tokenSize);

        creepSprite = gem_.createSprite();
        gem_.setAnchor(creepSprite, 0.5);
        gem_.setZIndex(creepSprite, 42);
        entSetLocation(creepSprite, getLocation(creepToken));
        gem_.setBaseHeight(creepSprite, tokenSize);
        gem_.setBaseWidth(creepSprite, tokenSize);
    }
    std::vector<String> extraTooltipLines() override { return {S("BARRACKS (") + S(CREEP_TYPES[creepType].name) + u")"}; }
    void updateEntities() override {
        gem_.setVisible(barracksImage, true);
        gem_.setVisible(creepToken, true);
        gem_.setVisible(creepSprite, true);
        gem_.setImage(creepSprite, CREEP_TYPES[creepType].assetName);
        gem_.commitEntityState(0.5, {barracksImage, creepToken, creepSprite});

        gem_.setVisible(progressFill, isTraining);
        gem_.commitEntityState(0.0, {progressFill, progressFillMask});
        gem_.setWidth(progressFillMask, progressFillMaxWidth * progress / (progressMax - 1));
        gem_.commitEntityState(1.0, {progressFillMask});
    }
    void hideEntities() override {
        gem_.setVisible(barracksImage, false);
        gem_.setVisible(creepToken, false);
        gem_.setVisible(creepSprite, false);
        gem_.setVisible(progressFill, false);
        gem_.commitEntityState(0.51, {barracksImage, creepToken, creepSprite, progressFill, progressFillMask});
    }
    void onComplete();
    bool act() override {
        if (isTraining) {
            progress++;
            if (progress == progressMax) {
                progress = 0;
                isTraining = false;
                onComplete();
            }
        }
        updateEntities();
        return false;
    }
};

void Obstacle::setMine(Player* owner) { setStructure(new Mine(this, owner, 1)); }
void Obstacle::setTower(Player* owner, int health) { setStructure(new Tower(this, owner, 0, health)); }
void Obstacle::setBarracks(Player* owner, CreepType creepType) { setStructure(new Barracks(this, owner, creepType)); }

void Player::printObstaclePerTurn(const Obstacle* o) {
    Structure* struc = o->structureField;
    bool visible = (struc != nullptr && struc->owner == this) || lessThan(o->loc.sqDist(queenUnit->loc), QUEEN_VISION);
    std::vector<int> toks = {o->obstacleId, visible ? o->goldField : -1, visible ? o->maxMineSize : -1};
    if (struc == nullptr) {
        toks.insert(toks.end(), {-1, -1, -1, -1});
    } else if (struc->kind == MINE) {
        toks.insert(toks.end(), {0, fixOwner(struc->owner), visible ? static_cast<Mine*>(struc)->incomeRateField : -1, -1});
    } else if (struc->kind == TOWER) {
        Tower* t = static_cast<Tower*>(struc);
        toks.insert(toks.end(), {1, fixOwner(struc->owner), t->health, t->attackRadius});
    } else {
        Barracks* b = static_cast<Barracks*>(struc);
        toks.insert(toks.end(), {2, fixOwner(struc->owner), !b->isTraining ? 0 : b->progressMax - b->progress, (int)b->creepType});
    }
    std::string line;
    for (size_t i = 0; i < toks.size(); i++) line += (i ? " " : "") + std::to_string(toks[i]);
    sendInputLine(line);
}

bool Mine::act() {
    int cash = std::min(incomeRateField, obstacle->gold());
    owner->goldPerTurn += cash;
    owner->gold += cash;
    obstacle->setGold(obstacle->gold() - cash);
    if (obstacle->gold() <= 0) {
        hideEntities();
        return true;
    }
    return false;
}

bool Tower::act() {
    Creep* closestEnemy = closestCreep(owner->enemyPlayer->activeCreeps, obstacle->loc);
    Queen* enemyQueen = owner->enemyPlayer->queenUnit;
    if (closestEnemy != nullptr && lessThan(closestEnemy->location().sqDist(obstacle->loc), attackRadius)) {
        damageCreep(closestEnemy);
        attackTarget = closestEnemy;
    } else if (lessThan(enemyQueen->location().sqDist(obstacle->loc), attackRadius)) {
        damageQueen(enemyQueen);
        attackTarget = enemyQueen;
    } else {
        attackTarget = nullptr;
    }
    health -= TOWER_MELT_RATE;
    attackRadius = java::d2i(std::sqrt((health * TOWER_COVERAGE_PER_HP + obstacle->area) / PI));
    if (health <= 0) {
        hideEntities();
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Collisions (MapBuilding.kt)
// ----------------------------------------------------------------------------

// collisionCheck: false if everything is ok; true if there was a correction
bool collisionCheck(const std::vector<FieldObject*>& entities, double acceptableGap = 0.0) {
    bool any = false;
    for (size_t i = 0; i < entities.size(); i++) {
        FieldObject* u1 = entities[i];
        double rad = (double)u1->radius();
        double clampDist = u1->mass() == 0 ? OBSTACLE_GAP + rad : rad;
        u1->setLocation(u1->location().clampWithin(clampDist, WORLD_WIDTH - clampDist, clampDist, WORLD_HEIGHT - clampDist));
        for (size_t j = 0; j < entities.size(); j++) {
            if (j == i) continue; // entities - u1 (the first occurrence; each object is there once)
            FieldObject* u2 = entities[j];
            double overlap = (double)(u1->radius() + u2->radius()) + acceptableGap - u1->location().dist(u2->location());
            if (overlap <= 1e-6) continue;
            double d1, d2;
            int m1 = u1->mass(), m2 = u2->mass();
            if (m1 == 0 && m2 == 0) d1 = 0.5, d2 = 0.5;
            else if (m1 == 0) d1 = 0.0, d2 = 1.0;
            else if (m2 == 0) d1 = 1.0, d2 = 0.0;
            else d1 = (double)m2 / (m1 + m2), d2 = (double)m1 / (m1 + m2);
            V2 u1tou2 = u2->location() - u1->location();
            double gap = (m1 == 0 && m2 == 0) ? 20.0 : 1.0;
            u1->setLocation(u1->location() - u1tou2.resizedTo(d1 * overlap + ((m1 == 0 && m2 > 0) ? 0.0 : gap)));
            u2->setLocation(u2->location() + u1tou2.resizedTo(d2 * overlap + ((m2 == 0 && m1 > 0) ? 0.0 : gap)));
            any = true;
        }
    }
    return any;
}

void fixCollisions(const std::vector<FieldObject*>& entities, int maxIterations = 999) {
    for (int i = 0; i < maxIterations; i++)
        if (!collisionCheck(entities)) return;
}

// Referee.allEntities: the creeps of every player, every queen, the sites
std::vector<FieldObject*> allEntities() {
    std::vector<FieldObject*> r;
    for (cg::Player* p : cg::gm.getPlayers())
        for (Creep* c : static_cast<Player*>(p)->activeCreeps) r.push_back(c);
    for (cg::Player* p : cg::gm.getPlayers()) {
        Queen* q = static_cast<Player*>(p)->queenUnit;
        if (q == nullptr) // lateinit
            throw java::Exception{"kotlin.UninitializedPropertyAccessException: lateinit property queenUnit has not been initialized"};
        r.push_back(q);
    }
    for (Obstacle* o : obstacles) r.push_back(o);
    return r;
}

// ----------------------------------------------------------------------------
// Creeps' behaviours
// ----------------------------------------------------------------------------
void GiantCreep::move(double frames) {
    Obstacle* best = nullptr;
    double bd = 0;
    for (Obstacle* o : obstacles) {
        Structure* s = o->structureField;
        if (s == nullptr || s->kind != TOWER || s->owner != owner->enemyPlayer) continue;
        double d = o->loc.sqDist(loc);
        if (best == nullptr || d < bd) {
            best = o;
            bd = d;
        }
    }
    if (best) setLocation(loc.towards(best->loc, (double)speed * frames));
}

void GiantCreep::dealDamage() {
    for (Obstacle* o : obstacles) {
        Structure* s = o->structureField;
        if (s != nullptr && s->kind == TOWER && s->owner == owner->enemyPlayer && lessThan(o->loc.sqDist(loc), radiusField + o->radiusField + TOUCHING_DELTA)) {
            static_cast<Tower*>(s)->health -= GIANT_BUST_RATE;
            V2 creepToTower = o->loc - loc;
            entSetLocation(characterSprite, creepToTower.resizedTo((double)radiusField));
            gem_.commitEntityState(0.2, {characterSprite});
            entSetLocation(characterSprite, V2::ofInt(0, 0));
            gem_.commitEntityState(1.0, {characterSprite});
            return;
        }
    }
}

void KnightCreep::finalizeFrame() {
    if (lastLocation) {
        V2 last = *lastLocation;
        V2 movementVector = (greaterThan(last.sqDist(loc), 30) && !attacksThisTurn) ? loc - last : owner->enemyPlayer->queenUnit->loc - loc;
        gem_.setRotation(characterSprite, movementVector.angle());
    }
    lastLocation = loc;
}

void KnightCreep::move(double frames) {
    Queen* enemyQueen = owner->enemyPlayer->queenUnit;
    // move toward enemy queen, if not yet in range
    if (greaterThan(loc.sqDist(enemyQueen->loc), radiusField + enemyQueen->radius() + attackRange))
        setLocation(loc.towards(enemyQueen->loc + (loc - enemyQueen->loc).resizedTo(3.0), (double)speed * frames));
}

void KnightCreep::dealDamage() {
    attacksThisTurn = false;
    Queen* enemyQueen = owner->enemyPlayer->queenUnit;
    if (lessThan(loc.sqDist(enemyQueen->loc), radiusField + enemyQueen->radius() + attackRange + TOUCHING_DELTA)) {
        attacksThisTurn = true;
        gem_.setAnchorX(characterSprite, 0.5, gem::IMMEDIATE);
        gem_.commitEntityState(0.4, {characterSprite});
        gem_.setAnchorX(characterSprite, 0.2);
        gem_.commitEntityState(0.7, {characterSprite});
        gem_.setAnchorX(characterSprite, 0.5);
        gem_.commitEntityState(1.0, {characterSprite});
        owner->enemyPlayer->setHealth(owner->enemyPlayer->health() - KNIGHT_DAMAGE);
    }
}

Creep* ArcherCreep::findTarget() const { return closestCreep(owner->enemyPlayer->activeCreeps, loc); }

void ArcherCreep::finalizeFrame() {
    Creep* t = findTarget();
    const Unit* target = t ? static_cast<const Unit*>(t) : owner->enemyPlayer->queenUnit;
    if (lastLocation) {
        V2 last = *lastLocation;
        V2 movementVector = greaterThan(last.sqDist(loc), 30) ? loc - last : target->loc - loc;
        gem_.setRotation(characterSprite, java::atan2(movementVector.y, movementVector.x));
    }
    lastLocation = loc;
    Creep* localAttackTarget = attackTarget;
    if (localAttackTarget != nullptr) {
        gem_.setAnchorX(characterSprite, 0.8);
        gem_.commitEntityState(0.3, {characterSprite});
        gem_.setAnchorX(characterSprite, 0.5);
        gem_.commitEntityState(0.4, {characterSprite});

        gem_.setRotation(projectile, (localAttackTarget->loc - loc).angle(), gem::IMMEDIATE);
        gem_.setVisible(projectile, true);
        gem_.setX(projectile, java::d2i(loc.x) + VIEWPORT_X_FIRST, gem::NONE);
        gem_.setY(projectile, java::d2i(loc.y) + VIEWPORT_Y_FIRST, gem::NONE);
        gem_.commitEntityState(0.4, {projectile});
        gem_.setX(projectile, java::d2i(localAttackTarget->loc.x) + VIEWPORT_X_FIRST, gem::EASE_IN_AND_OUT);
        gem_.setY(projectile, java::d2i(localAttackTarget->loc.y) + VIEWPORT_Y_FIRST, gem::EASE_IN_AND_OUT);
        gem_.commitEntityState(0.99, {projectile});
        gem_.setVisible(projectile, false);
        gem_.commitEntityState(1.0, {projectile});
    }
}

void ArcherCreep::move(double frames) {
    Creep* t = findTarget();
    const Unit* target = t ? static_cast<const Unit*>(t) : owner->queenUnit;
    // move toward target, if not yet in range
    if (greaterThan(loc.sqDist(target->loc), radiusField + target->radius() + attackRange))
        setLocation(loc.towards(target->loc + (loc - target->loc).resizedTo(3.0), (double)speed * frames));
}

void ArcherCreep::dealDamage() {
    attackTarget = nullptr;
    Creep* target = findTarget();
    if (target == nullptr) return;
    if (lessThan(loc.sqDist(target->loc), radiusField + target->radius() + attackRange + TOUCHING_DELTA)) {
        target->damage(target->isGiant() ? ARCHER_DAMAGE_TO_GIANTS : ARCHER_DAMAGE);
        attackTarget = target;
    }
}

// the onComplete closure of a TRAIN: { repeat(count) { spawn }; fixCollisions(allEntities()) }
void Barracks::onComplete() {
    Player* player = onCompletePlayer;
    if (player == nullptr) return; // { }
    for (int iter = 0; iter < CREEP_TYPES[creepType].count; iter++) {
        Creep* it;
        if (creepType == KNIGHT) it = new KnightCreep(owner, creepType);
        else if (creepType == ARCHER) it = new ArcherCreep(owner, creepType);
        else it = new GiantCreep(owner, creepType);
        int c = owner->isSecondPlayer ? -1 : 1;
        it->setLocation(obstacle->loc + V2::ofInt(c * iter, c * iter));
        it->finalizeFrame();
        it->setLocation(it->location().towards(owner->enemyPlayer->queenUnit->loc, 30.0));
        it->finalizeFrame();
        it->commitState(0.0);
        player->activeCreeps.push_back(it);
    }
    fixCollisions(allEntities());
}

// ----------------------------------------------------------------------------
// Map (MapBuilding.kt)
// ----------------------------------------------------------------------------
java::Random theRandom;
java::Random rando; // the sites' images (an unseeded java.util.Random in the jar)
int nextObstacleId = 0;

// IntRange.sample()
int sample(int first, int last) { return theRandom.nextInt(last - first + 1) + first; }

Entity* background = nullptr;
Entity* hudBackground = nullptr;

// MapBuildingKt's static initializer: its top level vals
void mapBuildingInit() {
    if (background) return;
    background = gem_.createSprite();
    gem_.setImage(background, "Background.jpg");
    gem_.setBaseWidth(background, WORLD_WIDTH);
    gem_.setBaseHeight(background, WORLD_HEIGHT);
    gem_.setX(background, VIEWPORT_X_FIRST);
    gem_.setY(background, VIEWPORT_Y_FIRST);
    gem_.setZIndex(background, 0);
    hudBackground = gem_.createSprite();
    gem_.setImage(hudBackground, "Hud.png");
    gem_.setBaseWidth(hudBackground, 1920);
    gem_.setX(hudBackground, 0);
    gem_.setY(hudBackground, 1080);
    gem_.setAnchorY(hudBackground, 1.0);
    gem_.setZIndex(hudBackground, 4000);
}

std::vector<Obstacle*> buildMap() {
    mapBuildingInit();
    nextObstacleId = 0;
    std::vector<std::pair<Obstacle*, Obstacle*>> obstaclePairs;
    for (int k = 1; k <= LEAGUE_OBSTACLES; k++) {
        int rate = sample(1, 3);
        int gold = sample(200, 250);
        int radius = sample(60, 90);
        int lx = theRandom.nextInt(WORLD_WIDTH);
        int ly = theRandom.nextInt(WORLD_HEIGHT);
        V2 l1 = V2::ofInt(lx, ly);
        V2 l2 = V2::ofInt(WORLD_WIDTH, WORLD_HEIGHT) - l1;
        Obstacle* o1 = new Obstacle(nextObstacleId++, rate, gold, radius, l1, rando.nextInt(10) + 1);
        Obstacle* o2 = new Obstacle(nextObstacleId++, rate, gold, radius, l2, rando.nextInt(10) + 1);
        obstaclePairs.push_back({o1, o2});
    }
    std::vector<Obstacle*> obs;
    for (auto& p : obstaclePairs) {
        obs.push_back(p.first);
        obs.push_back(p.second);
    }
    std::vector<FieldObject*> fos(obs.begin(), obs.end());
    // (1..100).all { ... collisionCheck } (the result does not matter: the obstacles are returned either way)
    for (int k = 1; k <= 100; k++) {
        for (auto& [o1, o2] : obstaclePairs) {
            V2 mid = (o1->loc + V2(WORLD_WIDTH - o2->loc.x, WORLD_HEIGHT - o2->loc.y)).div(2.0);
            o1->setLocation(mid);
            o2->setLocation(V2(WORLD_WIDTH - mid.x, WORLD_HEIGHT - mid.y));
        }
        if (!collisionCheck(fos, (double)OBSTACLE_GAP)) break;
    }
    V2 mapCenter = V2::ofInt((VIEWPORT_X_LAST - VIEWPORT_X_FIRST) / 2, (VIEWPORT_Y_LAST - VIEWPORT_Y_FIRST) / 2);
    for (Obstacle* o : obs) {
        o->setLocation(o->loc.snapToIntegers());
        if (lessThan(o->loc.sqDist(mapCenter), OBSTACLE_GOLD_INCREASE_DISTANCE_1)) {
            o->maxMineSize++;
            o->setGold(o->gold() + OBSTACLE_GOLD_INCREASE);
        }
        if (lessThan(o->loc.sqDist(mapCenter), OBSTACLE_GOLD_INCREASE_DISTANCE_2)) {
            o->maxMineSize++;
            o->setGold(o->gold() + OBSTACLE_GOLD_INCREASE);
        }
        o->updateEntities();
    }
    return obs;
}

// ----------------------------------------------------------------------------
// Referee
// ----------------------------------------------------------------------------
struct PlayerInputException {
    String message;
};
struct PlayerInputWarning {
    String message;
};
struct TimeoutException {};
struct OtherException { // any other Exception: its message
    String message;
};

std::vector<Player*> activePlayers() {
    std::vector<Player*> r;
    for (cg::Player* p : cg::gm.getActivePlayers()) r.push_back(static_cast<Player*>(p));
    return r;
}

Obstacle* findObstacle(int id) {
    for (Obstacle* o : obstacles)
        if (o->obstacleId == id) return o;
    return nullptr;
}

struct Referee : cg::Referee {
    void init() override {
        cg::gm.setMaxTurns(200);
        theRandom = java::Random(cg::gm.getSeed());
        rando = java::Random(cg::gm.getSeed());
        // league level 19: the else branch (QUEEN_HP.sample() first initializes MapBuildingKt)
        mapBuildingInit();
        leagueQueenHp = sample(QUEEN_HP_FIRST, QUEEN_HP_LAST) * QUEEN_HP_MULT;

        cg::gm.setFrameDuration(750);

        Player* p0 = static_cast<Player*>(cg::gm.getPlayer(0));
        Player* p1 = static_cast<Player*>(cg::gm.getPlayer(1));
        p0->enemyPlayer = p1;
        p1->enemyPlayer = p0;
        p1->isSecondPlayer = true;
        for (cg::Player* p : cg::gm.getPlayers()) static_cast<Player*>(p)->setHealth(leagueQueenHp);

        obstacles = buildMap();

        std::vector<Player*> act = activePlayers();
        for (size_t i = 0; i < act.size() && i < 2; i++) {
            bool invert = i == 1;
            int spawnDistance = 200;
            V2 corner = invert ? V2::ofInt(WORLD_WIDTH - spawnDistance, WORLD_HEIGHT - spawnDistance) : V2::ofInt(spawnDistance, spawnDistance);
            act[i]->queenUnit = new Queen(act[i]);
            act[i]->queenUnit->setLocation(corner);
        }

        fixCollisions(allEntities());

        for (Player* p : activePlayers()) p->hud().update();

        for (Player* p : activePlayers()) {
            p->sendInputLine(std::to_string(obstacles.size()));
            for (Obstacle* o : obstacles) p->printObstacleInit(o);
        }
    }

    void sendGameStates() {
        for (Player* activePlayer : activePlayers()) {
            int touchedObstacle = -1, touching = 0;
            for (Obstacle* o : obstacles)
                if (lessThan(o->loc.sqDist(activePlayer->queenUnit->loc), o->radiusField + QUEEN_RADIUS + TOUCHING_DELTA)) {
                    touching++;
                    touchedObstacle = o->obstacleId;
                }
            if (touching != 1) touchedObstacle = -1; // singleOrNull
            activePlayer->sendInputLine(std::to_string(activePlayer->gold) + " " + std::to_string(touchedObstacle));
            for (Obstacle* o : obstacles) activePlayer->printObstaclePerTurn(o);

            std::vector<Unit*> units;
            for (Player* p : activePlayers()) {
                for (Creep* c : p->activeCreeps) units.push_back(c);
                units.push_back(p->queenUnit);
            }
            activePlayer->sendInputLine(std::to_string(units.size()));
            for (Unit* u : units) {
                std::string line = std::to_string(roundToInt(u->loc.x)) + " " + std::to_string(roundToInt(u->loc.y)) + " " + (u->owner == activePlayer ? "0" : "1");
                if (Queen* q = dynamic_cast<Queen*>(u)) line += " -1 " + std::to_string(q->owner->health());
                else {
                    Creep* c = static_cast<Creep*>(u);
                    line += " " + std::to_string((int)c->creepType) + " " + std::to_string(c->health());
                }
                activePlayer->sendInputLine(line);
            }
            activePlayer->execute();
        }
    }

    // a scheduled building: the closure of scheduleBuilding
    struct Scheduled {
        Player* player;
        Obstacle* obs;
        Structure* struc; // the site's structure when the command was read
        std::vector<String> toks;
    };

    static void runScheduled(const Scheduled& s) {
        size_t k = 0;
        if (k >= s.toks.size()) throw PlayerInputException{u"Structure type must be specified"};
        const String& firstToken = s.toks[k++];
        if (firstToken == u"MINE") {
            if (s.struc != nullptr && s.struc->kind == MINE) {
                Mine* m = static_cast<Mine*>(s.struc);
                m->setIncomeRate(m->incomeRate() + 1);
                if (m->incomeRate() > s.obs->maxMineSize) m->setIncomeRate(s.obs->maxMineSize);
            } else {
                s.obs->setMine(s.player);
            }
        } else if (firstToken == u"TOWER") {
            if (s.struc != nullptr && s.struc->kind == TOWER) {
                Tower* t = static_cast<Tower*>(s.struc);
                t->health += TOWER_HP_INCREMENT;
                if (t->health > TOWER_HP_MAXIMUM) t->health = TOWER_HP_MAXIMUM;
            } else {
                s.obs->setTower(s.player, TOWER_HP_INITIAL);
            }
        } else if (firstToken == u"BARRACKS") {
            if (k >= s.toks.size()) throw PlayerInputException{u"BARRACKS type must be specified"};
            const String& creepInputType = s.toks[k++];
            int creepType = -1;
            for (int t = 0; t < 3; t++)
                if (creepInputType == java::ascii(CREEP_TYPES[t].name)) creepType = t; // CreepType.valueOf
            if (creepType < 0) throw PlayerInputException{u"Invalid BARRACKS type: " + creepInputType};
            s.obs->setBarracks(s.player, (CreepType)creepType);
        } else {
            throw PlayerInputException{u"Invalid structure type: " + firstToken};
        }
    }

    void processPlayerActions(int turn) {
        std::vector<Obstacle*> obstaclesAttemptedToBuildUpon;
        std::vector<Scheduled> scheduledBuildings;

        auto scheduleBuilding = [&](Player* player, Obstacle* obs, const String& strucType) {
            Structure* struc = obs->structureField;
            if (struc != nullptr && struc->owner == player->enemyPlayer) throw PlayerInputWarning{u"Cannot build: owned by enemy player"};
            if (struc != nullptr && struc->kind == BARRACKS && struc->owner == player && static_cast<Barracks*>(struc)->isTraining)
                throw PlayerInputWarning{u"Cannot rebuild: training is in progress"};
            obstaclesAttemptedToBuildUpon.push_back(obs);
            scheduledBuildings.push_back(Scheduled{player, obs, struc, java::kotlinSplit(strucType, String(u"-"))});
        };

        for (Player* player : activePlayers()) {
            Queen* queen = player->queenUnit;
            try {
                // player.outputs: getOutputs() throws TimeoutException after a timeout
                if (player->timeout) throw TimeoutException{};
                const std::vector<String>& outputs = player->outputs;
                try {
                    if (outputs.size() < 2) throw OtherException{S("Index 1 out of bounds for length ") + I((i64)outputs.size())};
                    std::vector<String> toks = java::kotlinSplit(outputs[1], String(u" "));
                    if (toks[0] != u"TRAIN") throw PlayerInputException{u"Expected TRAIN on the second line"};

                    // Process building creeps
                    std::vector<int> ids;
                    for (size_t i = 1; i < toks.size(); i++) {
                        i32 v;
                        if (!java::parseInt(toks[i], v)) throw PlayerInputException{u"Couldn't process siteId: " + toks[i]}; // toIntOrNull
                        ids.push_back(v);
                    }
                    std::vector<Obstacle*> obss;
                    for (int id : ids) {
                        Obstacle* o = findObstacle(id);
                        if (o == nullptr) throw PlayerInputException{S("No site with id = ") + I(id)};
                        obss.push_back(o);
                    }
                    std::vector<Barracks*> buildingBarracks;
                    for (Obstacle* obs : obss) {
                        Structure* s = obs->structureField;
                        if (s == nullptr || s->kind != BARRACKS) throw PlayerInputWarning{S("Cannot spawn from ") + I(obs->obstacleId) + u": not a barracks"};
                        Barracks* struc = static_cast<Barracks*>(s);
                        if (struc->owner != player) throw PlayerInputWarning{S("Cannot spawn from ") + I(obs->obstacleId) + u": not owned"};
                        if (struc->isTraining) throw PlayerInputWarning{S("Barracks ") + I(obs->obstacleId) + u" is training"};
                        buildingBarracks.push_back(struc);
                    }
                    std::vector<Barracks*> distinct;
                    for (Barracks* b : buildingBarracks)
                        if (std::find(distinct.begin(), distinct.end(), b) == distinct.end()) distinct.push_back(b);
                    if (buildingBarracks.size() > distinct.size()) throw PlayerInputWarning{u"Training from some barracks more than once"};

                    int sum = 0;
                    for (Barracks* b : buildingBarracks) sum += CREEP_TYPES[b->creepType].cost;
                    if (sum > player->gold) throw PlayerInputWarning{S("Training too many creeps (") + I(sum) + u" total gold requested)"};

                    player->gold -= sum;
                    for (Barracks* barracks : buildingBarracks) {
                        barracks->progress = 0;
                        barracks->isTraining = true;
                        barracks->onCompletePlayer = player;
                    }
                } catch (const PlayerInputWarning& e) {
                    cg::gm.addToGameSummary(player->getNicknameToken() + u": [WARNING] " + e.message);
                }

                // Process queen command
                try {
                    String line = java::kotlinTrim(outputs[0]);
                    std::vector<String> toks = java::kotlinSplit(line, String(u" "));
                    size_t k = 0;
                    const String command = toks[k++];
                    if (command == u"WAIT") {
                    } else if (command == u"MOVE") {
                        i32 x, y;
                        if (k >= toks.size() || !java::parseInt(toks[k++], x)) throw PlayerInputException{u"In MOVE command, x and y must be integers"};
                        if (k >= toks.size() || !java::parseInt(toks[k++], y)) throw PlayerInputException{u"In MOVE command, x and y must be integers"};
                        queen->moveTowards(V2::ofInt(x, y));
                    } else if (command == u"BUILD") {
                        i32 obsId;
                        if (k >= toks.size() || !java::parseInt(toks[k++], obsId)) throw PlayerInputException{u"Could not parse siteId"};
                        Obstacle* obs = findObstacle(obsId);
                        if (obs == nullptr) throw PlayerInputException{S("Site id ") + I(obsId) + u" does not exist"};
                        if (k >= toks.size()) throw PlayerInputException{u"Missing structure type in " + command + u" command"};
                        const String strucType = toks[k++];
                        if (lessThan(obs->loc.sqDist(queen->loc), queen->radius() + obs->radiusField + TOUCHING_DELTA)) {
                            scheduleBuilding(player, obs, strucType);
                        } else {
                            queen->moveTowards(obs->loc);
                        }
                    } else {
                        throw PlayerInputException{u"Didn't understand command: " + command};
                    }
                    if (k < toks.size()) throw PlayerInputException{u"Too many tokens after " + command + u" command"};
                } catch (const PlayerInputWarning& e) {
                    cg::gm.addToGameSummary(player->getNicknameToken() + u": [WARNING] " + e.message);
                }
            } catch (const TimeoutException&) {
                player->kill(u"Timeout!");
                cg::gm.addToGameSummary(player->getNicknameToken() + u" failed to provide " + I(player->getExpectedOutputLines()) + u" lines of output in time.");
            } catch (const PlayerInputException& e) {
                player->kill(e.message);
                cg::gm.addToGameSummary(player->getNicknameToken() + u": " + e.message);
            } catch (const OtherException& e) {
                player->kill(e.message);
                cg::gm.addToGameSummary(player->getNicknameToken() + u": " + e.message);
            }
        }

        // If they're both building onto the same one, then actually build only one: depending on parity of the turn number
        if (obstaclesAttemptedToBuildUpon.size() == 2 && obstaclesAttemptedToBuildUpon[0] == obstaclesAttemptedToBuildUpon[1])
            scheduledBuildings.erase(scheduledBuildings.begin() + (turn % 2));

        // Execute builds that remain
        for (const Scheduled& s : scheduledBuildings) {
            try {
                runScheduled(s);
            } catch (const PlayerInputException& e) {
                s.player->kill(e.message);
                cg::gm.addToGameSummary(s.player->getNicknameToken() + u": " + e.message);
            }
        }
    }

    void processCreeps() {
        std::vector<Creep*> allCreeps;
        for (Player* p : activePlayers())
            for (Creep* c : p->activeCreeps) allCreeps.push_back(c);
        std::stable_sort(allCreeps.begin(), allCreeps.end(), [](const Creep* a, const Creep* b) { return a->creepType < b->creepType; });
        for (int k = 0; k < 5; k++) {
            for (Creep* c : allCreeps) c->move(1.0 / 5);
            fixCollisions(allEntities(), 1);
        }
        for (Creep* c : allCreeps) c->dealDamage();

        // Tear down enemy mines
        for (Creep* creep : allCreeps) {
            Obstacle* closest = nullptr;
            double bd = 0;
            for (Obstacle* o : obstacles) {
                double d = o->loc.sqDist(creep->loc);
                if (closest == nullptr || d < bd) {
                    closest = o;
                    bd = d;
                }
            }
            if (!lessThan(closest->loc.sqDist(creep->loc), closest->radiusField + creep->radius() + TOUCHING_DELTA)) continue;
            Structure* struc = closest->structureField;
            if (struc != nullptr && struc->kind == MINE && struc->owner != creep->owner) closest->setStructure(nullptr);
        }

        for (Creep* c : allCreeps) c->damage(1);
        for (Creep* c : allCreeps) c->finalizeFrame();

        // Queens tear down enemy structures (not TOWERs)
        for (Player* p : activePlayers()) {
            Queen* queen = p->queenUnit;
            Obstacle* closest = nullptr;
            double bd = 0;
            for (Obstacle* o : obstacles) {
                double d = o->loc.sqDist(queen->loc);
                if (closest == nullptr || d < bd) {
                    closest = o;
                    bd = d;
                }
            }
            if (!lessThan(closest->loc.sqDist(queen->loc), closest->radiusField + queen->radius() + TOUCHING_DELTA)) continue;
            Structure* struc = closest->structureField;
            if (struc != nullptr && (struc->kind == MINE || struc->kind == BARRACKS) && struc->owner != queen->owner) closest->setStructure(nullptr);
        }
    }

    void gameTurn(int turn1) override {
        int turn = turn1 - 1; // engine 1.30 numbers the turns from 0
        for (Player* p : activePlayers()) p->goldPerTurn = 0;

        sendGameStates();
        processPlayerActions(turn);
        processCreeps();

        // Process structures
        for (Obstacle* o : obstacles) o->act();

        // Remove dead creeps
        for (Player* player : activePlayers()) {
            std::vector<Creep*> dead;
            for (Creep* c : player->activeCreeps)
                if (c->health() == 0) dead.push_back(c);
            for (Creep* c : dead) {
                player->activeCreeps.erase(std::find(player->activeCreeps.begin(), player->activeCreeps.end(), c));
                anims.createAnimationEvent("death", 1.0, c->loc);
            }
        }

        // Check end game
        for (Player* p : activePlayers()) p->checkQueenHealth();
        if (activePlayers().size() < 2) cg::gm.endGame();

        // Snap entities to integer coordinates
        for (FieldObject* e : allEntities()) e->setLocation(e->location().snapToIntegers());
    }

    // the modules in the order Guice registered them: GraphicEntityModule, AnimModule, TooltipModule
    void onGameInit() override {
        gem_.sendGlobalData();
        gem_.sendFrameData();
        anims.sendFrameData();
        tooltips.sendFrameData();
    }
    void onAfterGameTurn() override {
        gem_.sendFrameData();
        anims.sendFrameData();
        tooltips.sendFrameData();
    }
    // onAfterOnEnd: the modules' data goes nowhere in engine 1.30
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    cg::Options options;
    options.engine25 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
