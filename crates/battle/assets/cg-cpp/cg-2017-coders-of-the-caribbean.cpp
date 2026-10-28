// Coders of the Caribbean (CodinGame, 2017-04, "coders-of-the-caribbean") - headless C++ referee, port of
// the CodinGame referee published in https://github.com/CodinGame/coders-of-the-caribbean (commit 0478489,
// 2017-04-19, = HEAD: "Catch NumberFormatException when reading properties"; the game's class only,
// `class Referee extends MultiReferee`) run on CodinGame's old AbstractReferee base of 2017 (common/legacy.h,
// legacy::Base::cotc2017(): the base embedded in the same game's referee of
// https://github.com/SpiritusSancti5/codinGame, commit c0cc6fe, "Referees/Coders of the Caribbean/Referee.java",
// which the jar compiles as referees/cg-builder/legacy/bases/base-2017-cotc.java). 2 players. Deterministic;
// reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): "seed=N",
// "mineCount=N", "barrelCount=N", "shipsPerPlayer=N", in the order of a JDK 9+ Properties) to stdout like
// the jar (referees/cg-builder/legacy: the referee runs unchanged in the runner of the SDK 4.5.0). "null"
// scores when the game fails (the Java referee crashed or the protocol broke; the jar after 150 s). -l
// writes the trace of common/legacy.h (bot executions, view sizes, summaries, tooltips, END).
//
// The two game classes: SpiritusSancti5's file has the same game class as the CodinGame repository's HEAD
// (all 14 commits of fixes of 2017-04-14..19 included), except that its getInitDataForView() also gives the
// seed ("23 21 <ships> 5 <seed>"): a view-only difference (a few bytes of the first frame, never near the
// runner's limits). This port follows the CodinGame repository (the class CodinGame links from the game).
//
// League: the referee's constant LEAGUE_LEVEL = 3 picks the default branch of its league switch (levels 0, 1,
// 2 = the wood leagues: 1 ship / no mines / speed 1, then mines, then 3 ships): up to 3 ships per player,
// cannons and mines, speed up to 2 = the rules of Bronze and above, CodinGame's top league. The lower
// leagues are not implemented, nor the init properties shipsPerPlayer / mineCount / barrelCount beyond
// what the referee does with them (the CLI only passes the seed).
//
// The game: a 23x21 hex grid (odd rows shifted right), 1-3 ships per player (java.util.Random(seed): ship
// count, mine count 5-9, barrel count 10-25, then the ships, mines and barrels mirrored top/bottom), each
// ship 3 cells (stern, center, bow), 100 rum. Each round every ship answers one line: MOVE x y, FASTER,
// SLOWER, PORT, STARBOARD, WAIT, MINE or FIRE x y (+ an optional message; the referee's regexes, case
// insensitive, coordinates of 1-8 digits); MOVE picks one of the other actions toward the target (the
// referee's autopilot, angles in double precision). The round: cannonballs fly (and land), every ship loses
// 1 rum, actions apply (speed, rotation, mines dropped behind the stern every 5 turns, cannonballs fired from
// the bow up to distance 10 every 2 turns), ships move 1 or 2 cells (bow collisions stop them), rotate
// (collisions cancel the rotation), pick up barrels and hit mines after each step; landing cannonballs hit
// ships (50 center, 25 bow/stern), mines (25 + 10 around) or barrels; a sunk ship leaves a barrel of its rum
// (up to 30). Invalid output or a timeout sinks all the player's ships (health 0; a ship of that player can
// still drift onto a barrel that round and float on). The score is the total rum of the player's ships;
// the game ends when a player has no ship left, or after 200 rounds.
//
// The Java referee is followed closely, notably: the regexes (\s of java.util.regex, ASCII case folding,
// the message = what follows the whitespace run, or its last character when only whitespace follows,
// cut to 50 UTF-16 chars + "..."), the double precision angles of the MOVE autopilot (Math.atan2 = fdlibm,
// exact comparisons, no FMA: java.h's pragmas), the entity ids (a static counter, rejected map candidates
// included), the iteration orders (lists everywhere: the game has no HashSet/HashMap), the actions of a
// player's valid lines before an invalid one still applying in that round's update.
// Not reproduced: the tooltips' order inside one frame (the base's HashSet of identity hash codes: their
// size only counts, see legacy.h). The view (getInitDataForView / getFrameDataForView) is built for its
// size only: it stays far below the runner's 30000-byte limit (messages are cut to 53 chars).
#include "common/legacy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// --- constants (LEAGUE_LEVEL 3: the default branch) ---
const int LEAGUE_LEVEL = 3;
const int MAP_WIDTH = 23;
const int MAP_HEIGHT = 21;
const int COOLDOWN_CANNON = 2;
const int COOLDOWN_MINE = 5;
const int INITIAL_SHIP_HEALTH = 100;
const int MAX_SHIP_HEALTH = 100;
const int MAX_SHIP_SPEED = 2;
const int MIN_SHIPS = 1;
const int MAX_SHIPS = 3;
const int MIN_MINES = 5;
const int MAX_MINES = 10;
const int MIN_RUM_BARRELS = 10;
const int MAX_RUM_BARRELS = 26;
const int MIN_RUM_BARREL_VALUE = 10;
const int MAX_RUM_BARREL_VALUE = 20;
const int REWARD_RUM_BARREL_VALUE = 30;
const int MINE_VISIBILITY_RANGE = 5;
const int FIRE_DISTANCE_MAX = 10;
const int LOW_DAMAGE = 25;
const int HIGH_DAMAGE = 50;
const int MINE_DAMAGE = 25;
const int NEAR_MINE_DAMAGE = 10;
const bool CANNONS_ENABLED = true;
const bool MINES_ENABLED = true;
const double PI = 3.141592653589793; // Math.PI

String S(i64 v) { return java::str(v); }

// Referee.join: String.valueOf of each value, joined by " "
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

int clamp(int val, int min, int max) { return std::max(min, std::min(max, val)); }

const int DIRECTIONS_EVEN[6][2] = {{1, 0}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}, {0, 1}};
const int DIRECTIONS_ODD[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {0, 1}, {1, 1}};

// (coordinates stay within +-10^8 + 1: the regexes take at most 8 digits, so no int arithmetic here overflows)
struct CubeCoordinate {
    int x, y, z;
    int distanceTo(const CubeCoordinate& dst) const { return (std::abs(x - dst.x) + std::abs(y - dst.y) + std::abs(z - dst.z)) / 2; }
};

struct Coord {
    int x = 0, y = 0;

    double angle(const Coord& targetPosition) const {
        double dy = (double)(targetPosition.y - this->y) * std::sqrt(3.0) / 2;
        double dx = (double)(targetPosition.x - this->x) + (double)((this->y - targetPosition.y) & 1) * 0.5;
        double angle = -java::atan2(dy, dx) * 3 / PI;
        if (angle < 0) {
            angle += 6;
        } else if (angle >= 6) {
            angle -= 6;
        }
        return angle;
    }

    CubeCoordinate toCubeCoordinate() const {
        int xp = x - (y - (y & 1)) / 2;
        int zp = y;
        int yp = -(xp + zp);
        return CubeCoordinate{xp, yp, zp};
    }

    Coord neighbor(int orientation) const {
        if (this->y % 2 == 1) return Coord{x + DIRECTIONS_ODD[orientation][0], y + DIRECTIONS_ODD[orientation][1]};
        return Coord{x + DIRECTIONS_EVEN[orientation][0], y + DIRECTIONS_EVEN[orientation][1]};
    }

    bool isInsideMap() const { return x >= 0 && x < MAP_WIDTH && y >= 0 && y < MAP_HEIGHT; }

    int distanceTo(const Coord& dst) const { return toCubeCoordinate().distanceTo(dst.toCubeCoordinate()); }

    bool operator==(const Coord& o) const { return y == o.y && x == o.x; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
};

enum EntityType { SHIP, BARREL, MINE, CANNONBALL };
const char* ENTITY_TYPE_NAME[] = {"SHIP", "BARREL", "MINE", "CANNONBALL"};

int UNIQUE_ENTITY_ID = 0; // Entity.UNIQUE_ENTITY_ID (static: from 0 in the jar's fresh JVM)

struct Entity {
    int id;
    EntityType type;
    Coord position;
    Entity(EntityType t, int x, int y) : id(UNIQUE_ENTITY_ID++), type(t), position{x, y} {}
    String toViewString() const { return join({S(id), S(position.y), S(position.x)}); }
    String toPlayerString(int arg1, int arg2, int arg3, int arg4) const {
        return join({S(id), java::ascii(ENTITY_TYPE_NAME[type]), S(position.x), S(position.y), S(arg1), S(arg2), S(arg3), S(arg4)});
    }
};

struct Damage {
    Coord position;
    int health;
    bool hit;
    String toViewString() const { return join({S(position.y), S(position.x), S(health), S(hit ? 1 : 0)}); }
};

// Referee.Action (FASTER..MINE), NONE = null
enum class Action { NONE, FASTER, SLOWER, PORT, STARBOARD, FIRE, MINE };
const char* ACTION_NAME[] = {"WAIT", "FASTER", "SLOWER", "PORT", "STARBOARD", "FIRE", "MINE"};

struct Ship : Entity {
    int orientation;
    int speed = 0;
    int health = INITIAL_SHIP_HEALTH;
    int initialHealth = 0;
    int owner;
    NString message;
    Action action = Action::NONE;
    int mineCooldown = 0;
    int cannonCooldown = 0;
    Coord target; // set by fire() before any FIRE action
    // (null in Java until the first moveShips: every ship of `ships` gets them before they are read)
    int newOrientation = 0;
    Coord newPosition, newBowCoordinate, newSternCoordinate;

    Ship(int x, int y, int orientation_, int owner_) : Entity(SHIP, x, y), orientation(orientation_), owner(owner_) {}

    String toViewString() const {
        Coord b = bow(), s = stern();
        return join({S(id), S(position.y), S(position.x), S(orientation), S(health), S(speed), java::ascii(ACTION_NAME[(int)action]), S(b.y), S(b.x), S(s.y),
                     S(s.x), u" ;" + (message ? *message : String())});
    }

    String toPlayerString(int playerIdx) const { return Entity::toPlayerString(orientation, speed, health, owner == playerIdx ? 1 : 0); }

    void setMessage(NString m) {
        if (m && m->size() > 50) m = m->substr(0, 50) + u"...";
        this->message = m;
    }

    void moveTo(int x, int y) {
        Coord currentPosition = this->position;
        Coord targetPosition{x, y};

        if (currentPosition == targetPosition) {
            this->action = Action::SLOWER;
            return;
        }

        double targetAngle, angleStraight, anglePort, angleStarboard, centerAngle, anglePortCenter, angleStarboardCenter;
        const Coord center{MAP_WIDTH / 2, MAP_HEIGHT / 2};

        switch (speed) {
        case 2:
            this->action = Action::SLOWER;
            break;
        case 1: {
            // Suppose we've moved first
            currentPosition = currentPosition.neighbor(orientation);
            if (!currentPosition.isInsideMap()) {
                this->action = Action::SLOWER;
                break;
            }

            // Target reached at next turn
            if (currentPosition == targetPosition) {
                this->action = Action::NONE;
                break;
            }

            // For each neighbor cell, find the closest to target
            targetAngle = currentPosition.angle(targetPosition);
            angleStraight = java::min(std::fabs(orientation - targetAngle), 6 - std::fabs(orientation - targetAngle));
            anglePort = java::min(std::fabs((orientation + 1) - targetAngle), std::fabs((orientation - 5) - targetAngle));
            angleStarboard = java::min(std::fabs((orientation + 5) - targetAngle), std::fabs((orientation - 1) - targetAngle));

            centerAngle = currentPosition.angle(center);
            anglePortCenter = java::min(std::fabs((orientation + 1) - centerAngle), std::fabs((orientation - 5) - centerAngle));
            angleStarboardCenter = java::min(std::fabs((orientation + 5) - centerAngle), std::fabs((orientation - 1) - centerAngle));

            // Next to target with bad angle, slow down then rotate (avoid to turn around the target!)
            if (currentPosition.distanceTo(targetPosition) == 1 && angleStraight > 1.5) {
                this->action = Action::SLOWER;
                break;
            }

            bool hasMin = false; // Integer distanceMin = null
            int distanceMin = 0;

            // Test forward
            Coord nextPosition = currentPosition.neighbor(orientation);
            if (nextPosition.isInsideMap()) {
                distanceMin = nextPosition.distanceTo(targetPosition);
                hasMin = true;
                this->action = Action::NONE;
            }

            // Test port
            nextPosition = currentPosition.neighbor((orientation + 1) % 6);
            if (nextPosition.isInsideMap()) {
                int distance = nextPosition.distanceTo(targetPosition);
                if (!hasMin || distance < distanceMin || (distance == distanceMin && anglePort < angleStraight - 0.5)) {
                    distanceMin = distance;
                    hasMin = true;
                    this->action = Action::PORT;
                }
            }

            // Test starboard
            nextPosition = currentPosition.neighbor((orientation + 5) % 6);
            if (nextPosition.isInsideMap()) {
                int distance = nextPosition.distanceTo(targetPosition);
                if (!hasMin || distance < distanceMin || (distance == distanceMin && angleStarboard < anglePort - 0.5 && this->action == Action::PORT) ||
                    (distance == distanceMin && angleStarboard < angleStraight - 0.5 && this->action == Action::NONE) ||
                    (distance == distanceMin && this->action == Action::PORT && angleStarboard == anglePort && angleStarboardCenter < anglePortCenter) ||
                    (distance == distanceMin && this->action == Action::PORT && angleStarboard == anglePort && angleStarboardCenter == anglePortCenter &&
                     (orientation == 1 || orientation == 4))) {
                    distanceMin = distance;
                    hasMin = true;
                    this->action = Action::STARBOARD;
                }
            }
            break;
        }
        case 0: {
            // Rotate ship towards target
            targetAngle = currentPosition.angle(targetPosition);
            angleStraight = java::min(std::fabs(orientation - targetAngle), 6 - std::fabs(orientation - targetAngle));
            anglePort = java::min(std::fabs((orientation + 1) - targetAngle), std::fabs((orientation - 5) - targetAngle));
            angleStarboard = java::min(std::fabs((orientation + 5) - targetAngle), std::fabs((orientation - 1) - targetAngle));

            centerAngle = currentPosition.angle(center);
            anglePortCenter = java::min(std::fabs((orientation + 1) - centerAngle), std::fabs((orientation - 5) - centerAngle));
            angleStarboardCenter = java::min(std::fabs((orientation + 5) - centerAngle), std::fabs((orientation - 1) - centerAngle));

            Coord forwardPosition = currentPosition.neighbor(orientation);

            this->action = Action::NONE;

            if (anglePort <= angleStarboard) {
                this->action = Action::PORT;
            }

            if (angleStarboard < anglePort || (angleStarboard == anglePort && angleStarboardCenter < anglePortCenter) ||
                (angleStarboard == anglePort && angleStarboardCenter == anglePortCenter && (orientation == 1 || orientation == 4))) {
                this->action = Action::STARBOARD;
            }

            if (forwardPosition.isInsideMap() && angleStraight <= anglePort && angleStraight <= angleStarboard) {
                this->action = Action::FASTER;
            }
            break;
        }
        }
    }

    void faster() { this->action = Action::FASTER; }
    void slower() { this->action = Action::SLOWER; }
    void port() { this->action = Action::PORT; }
    void starboard() { this->action = Action::STARBOARD; }
    void placeMine() {
        if (MINES_ENABLED) this->action = Action::MINE;
    }

    Coord stern() const { return position.neighbor((orientation + 3) % 6); }
    Coord bow() const { return position.neighbor(orientation); }
    Coord newStern() const { return position.neighbor((newOrientation + 3) % 6); }
    Coord newBow() const { return position.neighbor(newOrientation); }

    bool at(const Coord& coord) const { return stern() == coord || bow() == coord || position == coord; }

    bool newBowIntersect(const Ship& other) const {
        return newBowCoordinate == other.newBowCoordinate || newBowCoordinate == other.newPosition || newBowCoordinate == other.newSternCoordinate;
    }

    bool newBowIntersect(const std::vector<Ship*>& ships) const {
        for (Ship* other : ships)
            if (this != other && newBowIntersect(*other)) return true;
        return false;
    }

    bool newPositionsIntersect(const Ship& other) const {
        bool sternCollision = newSternCoordinate == other.newBowCoordinate || newSternCoordinate == other.newPosition || newSternCoordinate == other.newSternCoordinate;
        bool centerCollision = newPosition == other.newBowCoordinate || newPosition == other.newPosition || newPosition == other.newSternCoordinate;
        return newBowIntersect(other) || sternCollision || centerCollision;
    }

    bool newPositionsIntersect(const std::vector<Ship*>& ships) const {
        for (Ship* other : ships)
            if (this != other && newPositionsIntersect(*other)) return true;
        return false;
    }

    void damage(int h) {
        this->health -= h;
        if (this->health <= 0) this->health = 0;
    }

    void heal(int h) {
        this->health += h;
        if (this->health > MAX_SHIP_HEALTH) this->health = MAX_SHIP_HEALTH;
    }

    void fire(int x, int y) {
        if (CANNONS_ENABLED) {
            this->target = Coord{x, y};
            this->action = Action::FIRE;
        }
    }
};

struct Mine : Entity {
    Mine(int x, int y) : Entity(MINE, x, y) {}

    String toPlayerString(int) const { return Entity::toPlayerString(0, 0, 0, 0); }

    std::vector<Damage> explode(const std::vector<Ship*>& ships, bool force) const {
        std::vector<Damage> damage;
        Ship* victim = nullptr;

        for (Ship* ship : ships) {
            if (position == ship->bow() || position == ship->stern() || position == ship->position) {
                damage.push_back(Damage{this->position, MINE_DAMAGE, true});
                ship->damage(MINE_DAMAGE);
                victim = ship;
            }
        }

        if (force || victim != nullptr) {
            if (victim == nullptr) damage.push_back(Damage{this->position, MINE_DAMAGE, true});

            for (Ship* ship : ships) {
                if (ship != victim) {
                    bool impact = false;
                    Coord impactPosition;
                    if (ship->stern().distanceTo(position) <= 1) {
                        impactPosition = ship->stern();
                        impact = true;
                    }
                    if (ship->bow().distanceTo(position) <= 1) {
                        impactPosition = ship->bow();
                        impact = true;
                    }
                    if (ship->position.distanceTo(position) <= 1) {
                        impactPosition = ship->position;
                        impact = true;
                    }

                    if (impact) {
                        ship->damage(NEAR_MINE_DAMAGE);
                        damage.push_back(Damage{impactPosition, NEAR_MINE_DAMAGE, true});
                    }
                }
            }
        }

        return damage;
    }
};

struct Cannonball : Entity {
    int ownerEntityId;
    int srcX, srcY;
    int initialRemainingTurns;
    int remainingTurns;

    Cannonball(int row, int col, int ownerEntityId_, int srcX_, int srcY_, int remainingTurns_)
        : Entity(CANNONBALL, row, col), ownerEntityId(ownerEntityId_), srcX(srcX_), srcY(srcY_), initialRemainingTurns(remainingTurns_),
          remainingTurns(remainingTurns_) {}

    String toViewString() const {
        return join({S(id), S(position.y), S(position.x), S(srcY), S(srcX), S(initialRemainingTurns), S(remainingTurns), S(ownerEntityId)});
    }

    String toPlayerString(int) const { return Entity::toPlayerString(ownerEntityId, remainingTurns, 0, 0); }
};

struct RumBarrel : Entity {
    int health;
    RumBarrel(int x, int y, int health_) : Entity(BARREL, x, y), health(health_) {}
    String toViewString() const { return join({S(id), S(position.y), S(position.x), S(health)}); }
    String toPlayerString(int) const { return Entity::toPlayerString(health, 0, 0, 0); }
};

struct Player {
    int id;
    std::vector<Ship*> ships;
    std::vector<Ship*> shipsAlive;

    explicit Player(int id_) : id(id_) {}

    void setDead() {
        for (Ship* ship : ships) ship->health = 0;
    }

    int getScore() const {
        int score = 0;
        for (Ship* ship : ships) score += ship->health;
        return score;
    }

    Lines toViewString() const {
        Lines data;
        data.push_back(S(this->id));
        for (Ship* ship : ships) data.push_back(ship->toViewString());
        return data;
    }
};

template <class T>
void removeObject(std::vector<T*>& list, T* o) { // List.remove(Object): the first one (identity: no equals)
    auto it = std::find(list.begin(), list.end(), o);
    if (it != list.end()) list.erase(it);
}

// Matcher#matches of the referee's patterns (CASE_INSENSITIVE, without UNICODE_CASE: ASCII folding only).
// The lines never hold a line terminator (the referee's Scanner split them there), so '.' takes anything.

// the literal start of a pattern ("MOVE ", "WAIT"...): i = the index after it
bool matchWord(const String& line, const char* word, size_t& i) {
    size_t w = strlen(word);
    if (line.size() < w) return false;
    for (size_t k = 0; k < w; k++)
        if (!java::regexCharMatchesIgnoreCase(line[k], word[k])) return false;
    i = w;
    return true;
}

// -?[0-9]{1,8}, greedy: more than 8 digits never match (a digit would then have to match \s or the end)
bool matchNumber(const String& line, size_t& i, String& group) {
    size_t n = line.size(), a = i;
    if (i < n && line[i] == u'-') i++;
    size_t d = i;
    while (i < n && java::isRegexDigit(line[i])) i++;
    if (i - d < 1 || i - d > 8) return false;
    group = line.substr(a, i - a);
    return true;
}

// (?:\s+(?<message>.+))? then the end, from i: nothing left (no message), or whitespace then at least one
// character: the message is what follows the whitespace run (greedy \s+), or the last character when only
// whitespace is left (\s+ backtracks one)
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
    for (size_t k = j; k < n; k++)
        if (java::isLineSeparator(line[k])) return false; // '.' (never: see above)
    message = line.substr(j);
    return true;
}

// WORD(?:\s+(?<message>.+))?
bool matchSimple(const String& line, const char* word, NString& message) {
    size_t i;
    return matchWord(line, word, i) && matchMessage(line, i, message);
}

// WORD (?<x>-?[0-9]{1,8})\s+(?<y>-?[0-9]{1,8})(?:\s+(?<message>.+))?   (WORD then one space)
bool matchTarget(const String& line, const char* word, String& gx, String& gy, NString& message) {
    size_t i;
    if (!matchWord(line, word, i) || !matchNumber(line, i, gx)) return false;
    size_t a = i;
    while (i < line.size() && java::isRegexSpace(line[i])) i++;
    if (i == a) return false;
    return matchNumber(line, i, gy) && matchMessage(line, i, message);
}

struct Referee : legacy::MultiReferee {
    i64 seed = 0;
    std::vector<Cannonball*> cannonballs;
    std::vector<Mine*> mines;
    std::vector<RumBarrel*> barrels;
    std::vector<Player*> players;
    std::vector<Ship*> ships;
    std::vector<Damage> damage;
    std::vector<Coord> cannonBallExplosions;
    int shipsPerPlayer = 0;
    int mineCount = 0;
    int barrelCount = 0;
    java::Random random;

    Referee() { base = legacy::Base::cotc2017(); }

    // Long.valueOf(prop.getProperty(key)), the default on a NumberFormatException (a missing key included)
    static i64 parseProperty(java::Properties& prop, const String& key, i64 defaultValue) {
        const String* v = prop.find(key);
        i64 r;
        if (v != nullptr && java::parseLong(*v, r)) return r;
        return defaultValue;
    }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        // new Random(System.currentTimeMillis()).nextLong(): evaluated, unused (the CLI always gives the seed)
        i64 now = (i64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        seed = parseProperty(prop, u"seed", java::Random(now).nextLong());

        random = java::Random(this->seed);

        i64 d = random.nextInt(1 + MAX_SHIPS - MIN_SHIPS) + MIN_SHIPS;
        shipsPerPlayer = clamp((i32)parseProperty(prop, u"shipsPerPlayer", d), MIN_SHIPS, MAX_SHIPS);

        if (MAX_MINES > MIN_MINES) {
            d = random.nextInt(MAX_MINES - MIN_MINES) + MIN_MINES;
            mineCount = clamp((i32)parseProperty(prop, u"mineCount", d), MIN_MINES, MAX_MINES);
        } else {
            mineCount = MIN_MINES;
        }

        d = random.nextInt(MAX_RUM_BARRELS - MIN_RUM_BARRELS) + MIN_RUM_BARRELS;
        barrelCount = clamp((i32)parseProperty(prop, u"barrelCount", d), MIN_RUM_BARRELS, MAX_RUM_BARRELS);

        cannonballs.clear();
        cannonBallExplosions.clear();
        damage.clear();

        // Generate Players
        for (int i = 0; i < playerCount; i++) players.push_back(new Player(i));
        // Generate Ships
        for (int j = 0; j < shipsPerPlayer; j++) {
            int xMin = 1 + j * MAP_WIDTH / shipsPerPlayer;
            int xMax = (j + 1) * MAP_WIDTH / shipsPerPlayer - 2;

            int y = 1 + random.nextInt(MAP_HEIGHT / 2 - 2);
            int x = xMin + random.nextInt(1 + xMax - xMin);
            int orientation = random.nextInt(6);

            Ship* ship0 = new Ship(x, y, orientation, 0);
            Ship* ship1 = new Ship(x, MAP_HEIGHT - 1 - y, (6 - orientation) % 6, 1);

            if (playerCount < 2) // this.players.get(1)
                throw java::Exception{"java.lang.IndexOutOfBoundsException: Index 1 out of bounds for length " + std::to_string(playerCount)};
            players[0]->ships.push_back(ship0);
            players[1]->ships.push_back(ship1);
            players[0]->shipsAlive.push_back(ship0);
            players[1]->shipsAlive.push_back(ship1);
        }

        ships.clear();
        for (Player* p : players) ships.insert(ships.end(), p->ships.begin(), p->ships.end());

        // Generate mines
        mines.clear();
        while ((int)mines.size() < mineCount) {
            int x = 1 + random.nextInt(MAP_WIDTH - 2);
            int y = 1 + random.nextInt(MAP_HEIGHT / 2);

            Mine* m = new Mine(x, y);

            bool cellIsFreeOfMines = std::none_of(mines.begin(), mines.end(), [&](Mine* mine) { return mine->position == m->position; });
            bool cellIsFreeOfShips = std::none_of(ships.begin(), ships.end(), [&](Ship* ship) { return ship->at(m->position); });

            if (cellIsFreeOfShips && cellIsFreeOfMines) {
                if (y != MAP_HEIGHT - 1 - y) mines.push_back(new Mine(x, MAP_HEIGHT - 1 - y));
                mines.push_back(m);
            }
        }
        mineCount = (int)mines.size();

        // Generate supplies
        barrels.clear();
        while ((int)barrels.size() < barrelCount) {
            int x = 1 + random.nextInt(MAP_WIDTH - 2);
            int y = 1 + random.nextInt(MAP_HEIGHT / 2);
            int h = MIN_RUM_BARREL_VALUE + random.nextInt(1 + MAX_RUM_BARREL_VALUE - MIN_RUM_BARREL_VALUE);

            RumBarrel* m = new RumBarrel(x, y, h);

            bool cellIsFreeOfBarrels = std::none_of(barrels.begin(), barrels.end(), [&](RumBarrel* barrel) { return barrel->position == m->position; });
            bool cellIsFreeOfMines = std::none_of(mines.begin(), mines.end(), [&](Mine* mine) { return mine->position == m->position; });
            bool cellIsFreeOfShips = std::none_of(ships.begin(), ships.end(), [&](Ship* ship) { return ship->at(m->position); });

            if (cellIsFreeOfShips && cellIsFreeOfMines && cellIsFreeOfBarrels) {
                if (y != MAP_HEIGHT - 1 - y) barrels.push_back(new RumBarrel(x, MAP_HEIGHT - 1 - y, h));
                barrels.push_back(m);
            }
        }
        barrelCount = (int)barrels.size();
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        prop.setProperty(u"shipsPerPlayer", S(shipsPerPlayer));
        prop.setProperty(u"barrelCount", S(barrelCount));
        prop.setProperty(u"mineCount", S(mineCount));
        return prop;
    }

    // --- AbstractReferee ---
    void prepare(int) override {
        for (Player* player : players)
            for (Ship* ship : player->ships) {
                ship->action = Action::NONE;
                ship->message.reset();
            }
        cannonBallExplosions.clear();
        damage.clear();
    }

    int getExpectedOutputLineCountForPlayer(int playerIdx) override { return (int)players[playerIdx]->shipsAlive.size(); }

    void handlePlayerOutput(int, int, int playerIdx, const Lines& outputs) override {
        Player* player = players[playerIdx];
        size_t i = 0;
        for (const String& line : outputs) {
            if (i >= player->shipsAlive.size()) // shipsAlive.get(i++) (never: the runner sends that many lines)
                throw java::Exception{"java.lang.IndexOutOfBoundsException: Index " + std::to_string(i) + " out of bounds for length " +
                                      std::to_string(player->shipsAlive.size())};
            Ship* ship = player->shipsAlive[i++];
            String gx, gy;
            NString message;

            if (matchTarget(line, "MOVE ", gx, gy, message)) {
                i32 x, y;
                java::parseInt(gx, x);
                java::parseInt(gy, y);
                ship->setMessage(message);
                ship->moveTo(x, y);
            } else if (matchSimple(line, "FASTER", message)) {
                ship->setMessage(message);
                ship->faster();
            } else if (matchSimple(line, "SLOWER", message)) {
                ship->setMessage(message);
                ship->slower();
            } else if (matchSimple(line, "PORT", message)) {
                ship->setMessage(message);
                ship->port();
            } else if (matchSimple(line, "STARBOARD", message)) {
                ship->setMessage(message);
                ship->starboard();
            } else if (matchSimple(line, "WAIT", message)) {
                ship->setMessage(message);
            } else if (matchSimple(line, "MINE", message)) {
                ship->setMessage(message);
                ship->placeMine();
            } else if (matchTarget(line, "FIRE ", gx, gy, message)) {
                i32 x, y;
                java::parseInt(gx, x);
                java::parseInt(gy, y);
                ship->setMessage(message);
                ship->fire(x, y);
            } else {
                player->setDead();
                throw legacy::InvalidInputException(u"A valid action", line);
            }
        }
    }

    void decrementRum() {
        for (Ship* ship : ships) ship->damage(1);
    }

    void updateInitialRum() {
        for (Ship* ship : ships) ship->initialHealth = ship->health;
    }

    void moveCannonballs() {
        for (size_t k = 0; k < cannonballs.size();) {
            Cannonball* ball = cannonballs[k];
            if (ball->remainingTurns == 0) {
                cannonballs.erase(cannonballs.begin() + k);
                continue;
            } else if (ball->remainingTurns > 0) {
                ball->remainingTurns--;
            }

            if (ball->remainingTurns == 0) cannonBallExplosions.push_back(ball->position);
            k++;
        }
    }

    void applyActions() {
        for (Player* player : players) {
            for (Ship* ship : player->shipsAlive) {
                if (ship->mineCooldown > 0) ship->mineCooldown--;
                if (ship->cannonCooldown > 0) ship->cannonCooldown--;

                ship->newOrientation = ship->orientation;

                switch (ship->action) {
                case Action::FASTER:
                    if (ship->speed < MAX_SHIP_SPEED) ship->speed++;
                    break;
                case Action::SLOWER:
                    if (ship->speed > 0) ship->speed--;
                    break;
                case Action::PORT:
                    ship->newOrientation = (ship->orientation + 1) % 6;
                    break;
                case Action::STARBOARD:
                    ship->newOrientation = (ship->orientation + 5) % 6;
                    break;
                case Action::MINE:
                    if (ship->mineCooldown == 0) {
                        Coord target = ship->stern().neighbor((ship->orientation + 3) % 6);

                        if (target.isInsideMap()) {
                            bool cellIsFreeOfBarrels = std::none_of(barrels.begin(), barrels.end(), [&](RumBarrel* barrel) { return barrel->position == target; });
                            bool cellIsFreeOfMines = std::none_of(mines.begin(), mines.end(), [&](Mine* mine) { return mine->position == target; });
                            bool cellIsFreeOfShips = std::none_of(ships.begin(), ships.end(), [&](Ship* b) { return b != ship && b->at(target); });

                            if (cellIsFreeOfBarrels && cellIsFreeOfShips && cellIsFreeOfMines) {
                                ship->mineCooldown = COOLDOWN_MINE;
                                mines.push_back(new Mine(target.x, target.y));
                            }
                        }
                    }
                    break;
                case Action::FIRE: {
                    int distance = ship->bow().distanceTo(ship->target);
                    if (ship->target.isInsideMap() && distance <= FIRE_DISTANCE_MAX && ship->cannonCooldown == 0) {
                        int travelTime = (int)(1 + java::round(ship->bow().distanceTo(ship->target) / 3.0));
                        cannonballs.push_back(new Cannonball(ship->target.x, ship->target.y, ship->id, ship->bow().x, ship->bow().y, travelTime));
                        ship->cannonCooldown = COOLDOWN_CANNON;
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    void checkBarrelCollisions(Ship* ship) {
        Coord bow = ship->bow();
        Coord stern = ship->stern();
        Coord center = ship->position;

        for (size_t k = 0; k < barrels.size();) {
            RumBarrel* barrel = barrels[k];
            if (barrel->position == bow || barrel->position == stern || barrel->position == center) {
                ship->heal(barrel->health);
                barrels.erase(barrels.begin() + k);
            } else {
                k++;
            }
        }
    }

    void checkMineCollisions() {
        for (size_t k = 0; k < mines.size();) {
            Mine* mine = mines[k];
            std::vector<Damage> mineDamage = mine->explode(ships, false);

            if (!mineDamage.empty()) {
                damage.insert(damage.end(), mineDamage.begin(), mineDamage.end());
                mines.erase(mines.begin() + k);
            } else {
                k++;
            }
        }
    }

    void checkCollisions() {
        // Check collisions with Barrels
        for (Ship* ship : this->ships) checkBarrelCollisions(ship);

        // Check collisions with Mines
        checkMineCollisions();
    }

    // The collision loops end once no ship collides: the reverted ships are back on their cells, which
    // never overlap (by induction from the start). A pass that changes nothing would loop forever in Java
    // (the jar's runner fails after 150 s): never seen, guarded here as a crash of the referee.
    static void guardLoop(int& passes) {
        if (++passes > 1000) throw java::Exception{"legacy: the referee loops forever in its collision checks"};
    }

    void moveShips() {
        // Go forward
        for (int i = 1; i <= MAX_SHIP_SPEED; i++) {
            for (Player* player : players) {
                for (Ship* ship : player->shipsAlive) {
                    ship->newPosition = ship->position;
                    ship->newBowCoordinate = ship->bow();
                    ship->newSternCoordinate = ship->stern();

                    if (i > ship->speed) continue;

                    Coord newCoordinate = ship->position.neighbor(ship->orientation);

                    if (newCoordinate.isInsideMap()) {
                        // Set new coordinate.
                        ship->newPosition = newCoordinate;
                        ship->newBowCoordinate = newCoordinate.neighbor(ship->orientation);
                        ship->newSternCoordinate = newCoordinate.neighbor((ship->orientation + 3) % 6);
                    } else {
                        // Stop ship!
                        ship->speed = 0;
                    }
                }
            }

            // Check ship and obstacles collisions
            std::vector<Ship*> collisions;
            bool collisionDetected = true;
            int passes = 0;
            while (collisionDetected) {
                guardLoop(passes);
                collisionDetected = false;

                for (Ship* ship : this->ships)
                    if (ship->newBowIntersect(ships)) collisions.push_back(ship);

                for (Ship* ship : collisions) {
                    // Revert last move
                    ship->newPosition = ship->position;
                    ship->newBowCoordinate = ship->bow();
                    ship->newSternCoordinate = ship->stern();

                    // Stop ships
                    ship->speed = 0;

                    collisionDetected = true;
                }
                collisions.clear();
            }

            // Move ships to their new location
            for (Ship* ship : this->ships) ship->position = ship->newPosition;

            checkCollisions();
        }
    }

    void rotateShips() {
        // Rotate
        for (Player* player : players) {
            for (Ship* ship : player->shipsAlive) {
                ship->newPosition = ship->position;
                ship->newBowCoordinate = ship->newBow();
                ship->newSternCoordinate = ship->newStern();
            }
        }

        // Check collisions
        bool collisionDetected = true;
        std::vector<Ship*> collisions;
        int passes = 0;
        while (collisionDetected) {
            guardLoop(passes);
            collisionDetected = false;

            for (Ship* ship : this->ships)
                if (ship->newPositionsIntersect(ships)) collisions.push_back(ship);

            for (Ship* ship : collisions) {
                ship->newOrientation = ship->orientation;
                ship->newBowCoordinate = ship->newBow();
                ship->newSternCoordinate = ship->newStern();
                ship->speed = 0;
                collisionDetected = true;
            }

            collisions.clear();
        }

        // Apply rotation
        for (Ship* ship : this->ships) ship->orientation = ship->newOrientation;

        checkCollisions();
    }

    bool gameIsOver() {
        for (Player* player : players)
            if (player->shipsAlive.empty()) return true;
        return barrels.empty() && LEAGUE_LEVEL == 0;
    }

    void explodeShips() {
        for (size_t k = 0; k < cannonBallExplosions.size();) {
            Coord position = cannonBallExplosions[k];
            bool removed = false;
            for (Ship* ship : ships) {
                if (position == ship->bow() || position == ship->stern()) {
                    damage.push_back(Damage{position, LOW_DAMAGE, true});
                    ship->damage(LOW_DAMAGE);
                    removed = true;
                    break;
                } else if (position == ship->position) {
                    damage.push_back(Damage{position, HIGH_DAMAGE, true});
                    ship->damage(HIGH_DAMAGE);
                    removed = true;
                    break;
                }
            }
            if (removed) cannonBallExplosions.erase(cannonBallExplosions.begin() + k);
            else k++;
        }
    }

    void explodeMines() {
        for (size_t b = 0; b < cannonBallExplosions.size();) {
            Coord position = cannonBallExplosions[b];
            bool removed = false;
            for (size_t k = 0; k < mines.size(); k++) {
                Mine* mine = mines[k];
                if (mine->position == position) {
                    std::vector<Damage> d = mine->explode(ships, true);
                    damage.insert(damage.end(), d.begin(), d.end());
                    mines.erase(mines.begin() + k);
                    removed = true;
                    break;
                }
            }
            if (removed) cannonBallExplosions.erase(cannonBallExplosions.begin() + b);
            else b++;
        }
    }

    void explodeBarrels() {
        for (size_t b = 0; b < cannonBallExplosions.size();) {
            Coord position = cannonBallExplosions[b];
            bool removed = false;
            for (size_t k = 0; k < barrels.size(); k++) {
                if (barrels[k]->position == position) {
                    damage.push_back(Damage{position, 0, true});
                    barrels.erase(barrels.begin() + k);
                    removed = true;
                    break;
                }
            }
            if (removed) cannonBallExplosions.erase(cannonBallExplosions.begin() + b);
            else b++;
        }
    }

    void updateGame(int) override {
        moveCannonballs();
        decrementRum();
        updateInitialRum();

        applyActions();
        moveShips();
        rotateShips();

        explodeShips();
        explodeMines();
        explodeBarrels();

        // For each sunk ship, create a new rum barrel with the amount of rum the ship had at the begin of the turn (up to 30).
        for (Ship* ship : ships) {
            if (ship->health <= 0) {
                int reward = std::min(REWARD_RUM_BARREL_VALUE, ship->initialHealth);
                if (reward > 0) barrels.push_back(new RumBarrel(ship->position.x, ship->position.y, reward));
            }
        }

        for (const Coord& position : cannonBallExplosions) damage.push_back(Damage{position, 0, false});

        for (size_t k = 0; k < ships.size();) {
            Ship* ship = ships[k];
            if (ship->health <= 0) {
                removeObject(players[ship->owner]->shipsAlive, ship);
                ships.erase(ships.begin() + k);
            } else {
                k++;
            }
        }

        if (gameIsOver()) throw legacy::GameOverException(std::string("endReached"));
    }

    void populateMessages(std::map<std::string, String>& p) override { p["endReached"] = u"End reached"; }

    Lines getInitInputForPlayer(int) override { return {}; }

    Lines getInputForPlayer(int, int playerIdx) override {
        Lines data;

        // Player's ships first
        for (Ship* ship : players[playerIdx]->shipsAlive) data.push_back(ship->toPlayerString(playerIdx));

        // Number of ships
        data.insert(data.begin(), S((i64)data.size()));

        // Opponent's ships
        for (Ship* ship : players[(playerIdx + 1) % 2]->shipsAlive) data.push_back(ship->toPlayerString(playerIdx));

        // Visible mines
        for (Mine* mine : mines) {
            bool visible = false;
            for (Ship* ship : players[playerIdx]->shipsAlive) {
                if (ship->position.distanceTo(mine->position) <= MINE_VISIBILITY_RANGE) {
                    visible = true;
                    break;
                }
            }
            if (visible) data.push_back(mine->toPlayerString(playerIdx));
        }

        for (Cannonball* ball : cannonballs) data.push_back(ball->toPlayerString(playerIdx));

        for (RumBarrel* barrel : barrels) data.push_back(barrel->toPlayerString(playerIdx));

        data.insert(data.begin() + 1, S((i64)data.size() - 1));

        return data;
    }

    Lines getInitDataForView() override {
        Lines data;
        data.push_back(join({S(MAP_WIDTH), S(MAP_HEIGHT), S((i64)players[0]->ships.size()), S(MINE_VISIBILITY_RANGE)}));
        data.insert(data.begin(), S((i64)data.size() + 1));
        return data;
    }

    Lines getFrameDataForView(int, int, int, bool) override {
        Lines data;

        for (Player* player : players) {
            Lines v = player->toViewString();
            data.insert(data.end(), v.begin(), v.end());
        }
        data.push_back(S((i64)cannonballs.size()));
        for (Cannonball* ball : cannonballs) data.push_back(ball->toViewString());
        data.push_back(S((i64)mines.size()));
        for (Mine* mine : mines) data.push_back(mine->toViewString());
        data.push_back(S((i64)barrels.size()));
        for (RumBarrel* barrel : barrels) data.push_back(barrel->toViewString());
        data.push_back(S((i64)damage.size()));
        for (const Damage& d : damage) data.push_back(d.toViewString());

        return data;
    }

    String getGameName() override { return u"CodersOfTheCaribbean"; }
    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }
    Lines getPlayerActions(int, int) override { return {}; }
    bool isPlayerDead(int) override { return false; }
    NString getDeathReason(int playerIdx) override { return u"$" + S(playerIdx) + u": Eliminated!"; }
    int getScore(int playerIdx) override { return players[playerIdx]->getScore(); }
    Lines getGameSummary(int) override { return {}; }
    void setPlayerTimeout(int, int, int playerIdx) override { players[playerIdx]->setDead(); }
    int getMaxRoundCount(int) override { return 200; }
    int getMillisTimeForRound() override { return 50; }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
