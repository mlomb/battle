// Spider Attack (CodinGame Spring Challenge 2022) - headless C++ referee, port of
// https://github.com/CodinGame/SpringChallenge2022 (commit 760e720), CodinGame game engine 4.1.6,
// highest league (league.level 19: fog, WIND, SHIELD, CONTROL and the tie-break are all enabled).
// 2 players.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints one score per player and "seed=<N>" on stdout, like the jar (exit 1 and no score when the
// jar's CLI dies: bad -seed, agent that cannot be launched).
// -l writes a trace: "T<turn> P<player> OUT [<lines received>]" / "T<turn> P<player> TIMEOUT", the
// viewer frame after each turn ("V<turn> <graphics>", hero messages left out) and the turns with an
// ambiguous push ("AMBIGUOUS T<turn> <mobs>", see below).
//
// The Java referee is followed very closely (same data structures, same iteration orders and the
// same floating point operations in the same order). Notable Java behaviours reproduced here:
//   - java.util.Random (nextDouble, nextInt(bound) with its rejection loop, nextBoolean), (int)
//     casts, Math.round, Integer.valueOf, Long.parseLong (seed), the command regexes of
//     Referee.handleCommands (\s is [ \t\n\x0B\f\r], \d is [0-9], int overflow = bad command)
//   - engine 4.1.6 I/O: Agent#getOutput ('\r' and '\n' end lines, "\r\n" counts once even across
//     turns, 10000 bytes max, UTF-8 decoding with U+FFFD replacement like String(bytes, UTF_8)),
//     1000ms for the first execution of each player and 50ms afterwards, extra lines kept for the
//     next turn, input written asynchronously (a bot that never reads its stdin does not block the
//     referee), output of a dead bot still read from the pipe
//   - the referee reads the runner's commands with java.util.Scanner, which also ends lines at
//     U+2028, U+2029 and U+0085: a player line containing one of them desynchronizes the engine
//     protocol. This is emulated with a line queue between "runner" and "GameManager" (refIn): the
//     referee then misreads the following lines (forged headers included) and usually dies with
//     "Error in data sent to referee"; the jar then prints "null" as every score and exits 0.
//   - the game summary and the tooltips: a bad command is repeated in the deactivation tooltip, so
//     ~10 KB of invalid UTF-8 (U+FFFD, 3 bytes each) overflows the runner's 30000-byte buffer and
//     the game fails ("null" scores)
//   - a controlled hero's command line is not parsed at all; a bad command deactivates the player
//     but the intents of its previous heroes are still played that turn; mana is spent by a
//     SHIELD/CONTROL on an already shielded target
//   - 3 or 4 players: Referee.init throws (2 corners only), the game ends before turn 1 and every
//     player scores 3 (no player is ever executed)
// Java behaviours that cannot be reproduced:
//   - Referee.doPush iterates a HashMap<GameEntity, List<Vector>> keyed by identity hash codes.
//     When two or more mobs are pushed out of a base attraction zone in the same turn, each one
//     draws its new random direction in that (run-dependent) order, so the jar itself is not
//     deterministic there (the same game played twice by the jar diverges). We use the insertion
//     order (the most likely order: ~53% for two mobs).
//   - Math.sin/Math.cos are HotSpot intrinsics (not fdlibm) that differ from the C library in the
//     last bit for ~6% of the game's angles. They only feed values truncated to integers (mob
//     speeds and positions): over 9M game angles, no spawn speed, no (int) speed and no mob
//     trajectory (1.5e9 steps) differed.
// Engine limits that cannot be reached here: the 1MB view quota (the worst case is below 970KB:
// 220 turns of <= 182 entities and <= 6 hero messages of <= 48 chars), the game summary quotas
// (no exception) and the 30s total turn time (440 executions x 50ms).
//
// Validation only: compiled with -DSPIDER_HINTS, the referee reads the jar's mob positions
// (SPIDER_HINTS=<file>) to follow the jar's identity hash order in doPush, and SPIDER_FORCE can
// force an order. The default build has none of this.

#include "common/engine.h"

#include <deque>

typedef std::u16string jstring; // a java.lang.String (UTF-16 code units)

// ----------------------------------------------------------------------------
// Java semantics (java.h has the rest)
// ----------------------------------------------------------------------------
namespace java {

// Math.min / Math.max for doubles (NaN and -0.0 aware)
static inline double min(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && std::signbit(b)) return b;
    return (a <= b) ? a : b;
}
static inline double max(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && std::signbit(a)) return b;
    return (a >= b) ? a : b;
}

static inline bool isSpace(char16_t c) { return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r'; }
static inline bool isDigit(char16_t c) { return c >= '0' && c <= '9'; }

// Integer.valueOf / Integer.parseInt of a string matching -?[0-9]+ ; false on overflow
// (NumberFormatException)
static bool parseInt(const jstring& s, size_t a, size_t b, i32& out) {
    bool neg = s[a] == '-';
    if (neg) a++;
    i64 v = 0;
    for (size_t i = a; i < b; i++) {
        v = v * 10 + (s[i] - '0');
        if (v > 2147483648LL) return false;
    }
    if (!neg && v > 2147483647LL) return false;
    out = (i32)(neg ? -v : v);
    return true;
}

static jstring fromAscii(const std::string& s) { return jstring(s.begin(), s.end()); }

static bool startsWith(const jstring& s, size_t pos, const char* lit) {
    size_t n = strlen(lit);
    if (pos + n > s.size()) return false;
    for (size_t i = 0; i < n; i++)
        if (s[pos + i] != (char16_t)lit[i]) return false;
    return true;
}

// Iteration order of a java.util.HashMap<Integer, ?> (default capacity) after putting the given
// distinct keys in order. Returns the indices of the keys in iteration order.
static std::vector<int> hashMapOrder(const std::vector<i32>& keys) {
    auto spread = [](i32 h) { return (u32)h ^ ((u32)h >> 16); };
    std::vector<u32> hashes;
    for (i32 k : keys) hashes.push_back(spread(k));
    size_t cap = 16;
    std::vector<std::vector<int>> tab(cap);
    auto resize = [&]() {
        // splitting a bin preserves the relative order of its nodes
        std::vector<std::vector<int>> nt(cap * 2);
        for (auto& bin : tab)
            for (int k : bin) nt[hashes[k] & (cap * 2 - 1)].push_back(k);
        tab.swap(nt);
        cap *= 2;
    };
    size_t size = 0;
    for (int k = 0; k < (int)keys.size(); k++) {
        auto& bin = tab[hashes[k] & (cap - 1)];
        bin.push_back(k);
        // treeifyBin resizes small tables instead (bins are never treeified at these sizes)
        if (bin.size() >= 9 && cap < 64) resize();
        if (++size > cap * 3 / 4) resize();
    }
    std::vector<int> order;
    for (auto& bin : tab)
        for (int k : bin) order.push_back(k);
    return order;
}

} // namespace java
// ----------------------------------------------------------------------------
// Configuration.java (defaults: the CLI passes no game parameter but the seed)
// ----------------------------------------------------------------------------
namespace C {
const int MAP_WIDTH = 17630;
const int MAP_HEIGHT = 9000;
const int MAP_LIMIT = 800;
const int BASE_ATTRACTION_RADIUS = 5000;
const int BASE_VIEW_RADIUS = 6000;
const int BASE_RADIUS = 300;
const int HERO_MOVE_SPEED = 800;
const int HEROES_PER_PLAYER = 3;
const int HERO_VIEW_RADIUS = 2200;
const int HERO_ATTACK_RANGE = 800;
const int HERO_ATTACK_DAMAGE = 2;
const int MAX_MANA = -1;
const int STARTING_MANA = 0;
const int STARTING_BASE_HEALTH = 3;
const int MOB_MOVE_SPEED = 400;
const double PI = 3.141592653589793;
const double MOB_SPAWN_MAX_DIRECTION_DELTA = 5 * PI / 12;
const int MOB_SPAWN_RATE = 5;
const int MOB_STARTING_MAX_ENERGY = 10;
const double MOB_GROWTH_MAX_ENERGY = 0.5;
const int SPELL_WIND_COST = 10;
const int SPELL_CONTROL_COST = 10;
const int SPELL_PROTECT_COST = 10;
const int SPELL_PROTECT_DURATION = 12;
const int SPELL_WIND_DISTANCE = 2200;
const int SPELL_WIND_RADIUS = 1280;
const bool ENABLE_FOG = true;
const bool ENABLE_WIND = true;
const bool ENABLE_CONTROL = true;
const bool ENABLE_SHIELD = true;
const bool ENABLE_TIE_BREAK = true;

// Referee.init
const int MAX_TURNS = 220;
const int TURN_MAX_TIME = 50;
} // namespace C

// ----------------------------------------------------------------------------
// Vector.java
// ----------------------------------------------------------------------------
struct Vector {
    double x = 0, y = 0;
    Vector() {}
    Vector(double x_, double y_) : x(x_), y(y_) {}
    // new Vector(a, b)
    static Vector between(const Vector& a, const Vector& b) { return Vector(b.x - a.x, b.y - a.y); }
    // new Vector(angle)
    static Vector fromAngle(double angle) { return Vector(std::cos(angle), std::sin(angle)); }
    Vector rotate(double angle) const {
        double nx = (x * std::cos(angle)) - (y * std::sin(angle));
        double ny = (x * std::sin(angle)) + (y * std::cos(angle));
        return Vector(nx, ny);
    }
    Vector round() const { return Vector((double)(i32)java::round(x), (double)(i32)java::round(y)); }
    Vector truncate() const { return Vector((double)java::d2i(x), (double)java::d2i(y)); }
    bool inRange(const Vector& v, double range) const { return (v.x - x) * (v.x - x) + (v.y - y) * (v.y - y) <= range * range; }
    Vector add(const Vector& v) const { return Vector(x + v.x, y + v.y); }
    Vector mult(double factor) const { return Vector(x * factor, y * factor); }
    Vector sub(const Vector& v) const { return Vector(x - v.x, y - v.y); }
    double length() const { return std::sqrt(x * x + y * y); }
    double lengthSquared() const { return x * x + y * y; }
    Vector normalize() const {
        double len = length();
        if (len == 0) return Vector(0, 0);
        return Vector(x / len, y / len);
    }
    std::string toIntString() const { return std::to_string(java::d2i(x)) + " " + std::to_string(java::d2i(y)); }
    Vector symmetric() const { return symmetric(Vector(0, 0)); }
    Vector symmetric(const Vector& center) const { return Vector(center.x * 2 - x, center.y * 2 - y); }
    bool withinBounds(double minx, double miny, double maxx, double maxy) const { return x >= minx && x <= maxx && y >= miny && y <= maxy; }
    bool isZero() const { return x == 0 && y == 0; }
    Vector symmetricTruncate(const Vector& origin) const { return sub(origin).truncate().add(origin); }
    // Vector.equals(Object): Double.doubleToLongBits of both coordinates
    bool sameBits(const Vector& o) const { return bitsX() == o.bitsX() && bitsY() == o.bitsY(); }
    u64 bitsX() const { return bits(x); }
    u64 bitsY() const { return bits(y); }
    static u64 bits(double d) {
        if (d != d) return 0x7ff8000000000000ULL;
        u64 b;
        memcpy(&b, &d, 8);
        return b;
    }
};

// ----------------------------------------------------------------------------
// Game entities (GameEntity.java, Hero.java, Mob.java, Player.java, ...)
// ----------------------------------------------------------------------------
enum ActionType { A_MOVE, A_WIND, A_SHIELD, A_CONTROL, A_IDLE };
static const char* ACTION_NAMES[] = {"MOVE", "WIND", "SHIELD", "CONTROL", "IDLE"};

struct Action {
    ActionType type = A_IDLE;
    bool hasDestination = false;
    Vector destination;
    int target = 0;
    bool forced = false;
};

struct Player;

struct MobStatus {
    int turns = 0;
    int target = -1; // player index, -1 = null
    int state = 0;
};

// Referee.TYPE_*: a hero's type is its owner's index (0 or 1)
const int TYPE_MOB = 2;
const int INPUT_TYPE_MOB = 0;
const int INPUT_TYPE_MY_HERO = 1;
const int INPUT_TYPE_ENEMY_HERO = 2;

struct Entity {
    // GameEntity
    int id = 0;
    Vector position;
    int type = 0;
    bool isHero = false;
    std::vector<Vector> activeControls;
    int shieldDuration = 0;
    bool pushed = false;
    // Hero
    int index = 0;
    Player* owner = nullptr;
    Action intent;
    // Mob
    Vector speed;
    int health = 0;
    bool healthChanged = true;
    bool hasStatus = false;
    MobStatus status;
    std::vector<Vector> nextControls;
    bool removed = false; // member of Referee.mobRemovals

    Player* getOwner() const { return isHero ? owner : nullptr; }
    void applyControl(const Vector& destination) {
        if (isHero) activeControls.push_back(destination);
        else nextControls.push_back(destination);
    }
    void applyShield() { shieldDuration = C::SPELL_PROTECT_DURATION + 1; }
    bool hasActiveShield() const { return shieldDuration > 0 && shieldDuration < C::SPELL_PROTECT_DURATION + 1; }
    bool hadActiveShield() const { return shieldDuration > 0 && shieldDuration < C::SPELL_PROTECT_DURATION; }
    bool gotPushed() const { return pushed; }
    bool isControlled() const { return !activeControls.empty(); }
    void pushTo(const Vector& p) {
        position = p;
        if (!isHero) {
            hasStatus = false;
            pushed = true;
        }
    }
    // Mob
    bool isAlive() const { return health > 0; }
    void hit(int damage) {
        health -= damage;
        healthChanged = true;
    }
    bool moveCancelled() const { return !isAlive() || pushed; }
    void setSpeed(const Vector& s) {
        speed = s;
        hasStatus = false;
    }
    void reset() {
        pushed = false;
        healthChanged = false;
        activeControls = nextControls;
        nextControls.clear();
    }
};

struct Player : cg::Player {
    std::vector<Entity*> heroes;
    int mana = C::STARTING_MANA;
    bool manaChanged = true;
    bool baseHealthChanged = true;
    int baseHealth = C::STARTING_BASE_HEALTH;
    int manaGainedOutsideOfBase = 0;

    int getExpectedOutputLines() const override { return (int)heroes.size(); }
    void gainMana(const int amount[2]) {
        mana += amount[0];
        manaGainedOutsideOfBase += amount[1];
        if (C::MAX_MANA > 0) {
            if (mana > C::MAX_MANA) mana = C::MAX_MANA;
            if (manaGainedOutsideOfBase > C::MAX_MANA) manaGainedOutsideOfBase = C::MAX_MANA;
        }
        manaChanged = true;
    }
    void spendMana(int amount) {
        mana -= amount;
        manaChanged = true;
    }
    void damageBase() {
        baseHealth--;
        if (baseHealth < 0) baseHealth = 0;
        baseHealthChanged = true;
    }
    void resetViewData() {
        manaChanged = false;
        baseHealthChanged = false;
    }
};

using cg::trace;

static std::string jsonString(const jstring& s) {
    std::string r = "\"";
    for (char16_t c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c >= 0x20 && c < 0x7f) r += (char)c;
        else {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
            r += b;
        }
    }
    return r + "\"";
}

static int currentTurn = 0;

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
static std::deque<Entity> entityPool;
static int ENTITY_COUNT = 0;
static Player players[4];
static int playerCount = 2;
static java::Random random_; // Referee.random (named random_: POSIX stdlib has a random())

static std::vector<Entity*> allHeroes;
static std::vector<Entity*> allMobs;
static std::vector<Entity*> mobRemovals;
static std::vector<Entity*> intentMap[5];
static Vector basePositions[2];
static int baseCount = 0; // basePositions.size()
static const Vector symmetryOrigin(C::MAP_WIDTH / 2, C::MAP_HEIGHT / 2);
static const Vector corners[2] = {Vector(0, 0), Vector(C::MAP_WIDTH, C::MAP_HEIGHT)};
static Vector startDirections[2];

// view data (only used for the trace)
struct SpellUse {
    int hero;
    ActionType spell;
    int target;
    bool hasDestination;
    int dx, dy;
};
static std::vector<Entity*> newEntities;
static std::vector<std::pair<int, std::vector<int>>> attacks;
static std::vector<SpellUse> spellUses;
static std::vector<std::pair<int, int>> baseAttacks;

#ifdef SPIDER_HINTS
// Validation only (compile with -DSPIDER_HINTS): the jar's mob positions at the end of each turn
// (env SPIDER_HINTS=<file> with lines "<turn> <id> <x> <y>"), used to follow the jar's identity
// hash order in doPush. SPIDER_FORCE="<turn>:<k>,..." forces the k-th permutation (in
// std::next_permutation order) of the random draws of that turn, for the cases the positions
// cannot decide.
static std::vector<std::vector<std::pair<int, std::pair<int, int>>>> hints;
static std::vector<std::pair<int, int>> forcedOrders;
static void loadHints() {
    const char* p = getenv("SPIDER_HINTS");
    if (p) {
        if (FILE* f = fopen(p, "r")) {
            int t, id, x, y;
            while (fscanf(f, "%d %d %d %d", &t, &id, &x, &y) == 4) {
                if (t < 0 || t > 1000) continue;
                if ((int)hints.size() <= t) hints.resize(t + 1);
                hints[t].push_back({id, {x, y}});
            }
            fclose(f);
        }
    }
    if (const char* s = getenv("SPIDER_FORCE")) {
        int t, k, n;
        while (sscanf(s, "%d:%d%n", &t, &k, &n) == 2) {
            forcedOrders.push_back({t, k});
            s += n;
            if (*s == ',') s++;
        }
    }
}
static bool hintPosition(int turn, int id, int& x, int& y) {
    if (turn < 0 || turn >= (int)hints.size()) return false;
    for (auto& h : hints[turn])
        if (h.first == id) {
            x = h.second.first;
            y = h.second.second;
            return true;
        }
    return false;
}
#endif

static Entity* newEntity(const Vector& position, int type) {
    entityPool.emplace_back();
    Entity* e = &entityPool.back();
    e->id = ENTITY_COUNT++;
    e->position = position;
    e->type = type;
    e->shieldDuration = 0;
    return e;
}

static Entity* newHero(int index, const Vector& position, Player* owner) {
    Entity* h = newEntity(position, owner->index);
    h->isHero = true;
    h->index = index;
    h->owner = owner;
    return h;
}

static Entity* newMob(const Vector& position, int health) {
    Entity* m = newEntity(position, TYPE_MOB);
    m->isHero = false;
    m->speed = Vector(0, 0);
    m->health = health;
    m->pushed = false;
    return m;
}

static std::vector<Entity*> allEntities() {
    std::vector<Entity*> r = allHeroes;
    r.insert(r.end(), allMobs.begin(), allMobs.end());
    return r;
}

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (int i = 0; i < playerCount; i++)
        if (players[i].active) r.push_back(&players[i]);
    return r;
}

// MobSpawner.java
struct SpawnLocation {
    Vector position, symetry, direction;
    SpawnLocation(int x, int y) {
        position = Vector(x, y);
        symetry = Vector(C::MAP_WIDTH - x, C::MAP_HEIGHT - y);
        direction = Vector(0, position.y <= C::MAP_HEIGHT / 2 ? 1 : -1);
    }
};

struct MobSpawner {
    std::vector<SpawnLocation> spawnLocations;
    double maxDirectionDelta = C::MOB_SPAWN_MAX_DIRECTION_DELTA;
    int spawnRate = C::MOB_SPAWN_RATE;
    int lastSpawn = -C::MOB_SPAWN_RATE;
    double currentMaxHealth = C::MOB_STARTING_MAX_ENERGY;

    MobSpawner() {
        spawnLocations.emplace_back(C::MAP_WIDTH / 2, -C::MAP_LIMIT + 1);
        spawnLocations.emplace_back(C::MAP_WIDTH / 2 + 4000, -C::MAP_LIMIT + 1);
    }

    std::vector<Entity*> update(int turn) {
        bool suddenDeath = turn >= 200;
        if (turn - lastSpawn >= spawnRate) {
            lastSpawn = turn;
            return spawn(suddenDeath);
        }
        return {};
    }

    static Vector opposite(const Vector& v) { return Vector(C::MAP_WIDTH - v.x, C::MAP_HEIGHT - v.y); }

    std::vector<Entity*> spawn(bool suddenDeath) {
        std::vector<Entity*> newMobs;
        for (const SpawnLocation& pairToUse : spawnLocations) {
            Vector suddenDeathTarget;
            if (suddenDeath) {
                int tx = random_.nextInt(C::BASE_ATTRACTION_RADIUS);
                int ty = random_.nextInt(C::BASE_ATTRACTION_RADIUS);
                if (random_.nextBoolean()) {
                    tx = C::MAP_WIDTH - tx;
                    ty = C::MAP_HEIGHT - ty;
                }
                suddenDeathTarget = Vector(tx, ty);
            }
            double directionDelta = random_.nextDouble() * maxDirectionDelta * 2 - maxDirectionDelta;
            for (int i = 0; i < 2; ++i) {
                Vector location = i == 0 ? pairToUse.position : pairToUse.symetry;
                Vector direction = i == 0 ? pairToUse.direction : pairToUse.direction.symmetric();
                Entity* mob = newMob(location, java::d2i(currentMaxHealth));
                if (suddenDeath) {
                    Vector v = Vector::between(location, i == 0 ? suddenDeathTarget : opposite(suddenDeathTarget))
                                   .normalize()
                                   .mult(C::MOB_MOVE_SPEED)
                                   .truncate();
                    mob->setSpeed(v);
                } else {
                    mob->setSpeed(direction.rotate(directionDelta).normalize().mult(C::MOB_MOVE_SPEED).truncate());
                }
                newMobs.push_back(mob);
            }
        }
        currentMaxHealth += C::MOB_GROWTH_MAX_ENERGY;
        return newMobs;
    }
};
static MobSpawner mobSpawner;

static Vector snapToGameZone(const Vector& v) {
    double snapX = v.x;
    double snapY = v.y;
    if (snapX < 0) snapX = 0;
    if (snapX > C::MAP_WIDTH) snapX = C::MAP_WIDTH;
    if (snapY < 0) snapY = 0;
    if (snapY > C::MAP_HEIGHT) snapY = C::MAP_HEIGHT;
    return Vector(snapX, snapY);
}

// Referee.intersection: null -> false
static bool intersection(double x1, double y1, double x2, double y2, double x3, double y3, double x4, double y4, Vector& p) {
    double d = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
    if (d == 0) return false;
    double xi = ((x3 - x4) * (x1 * y2 - y1 * x2) - (x1 - x2) * (x3 * y4 - y3 * x4)) / d;
    double yi = ((y3 - y4) * (x1 * y2 - y1 * x2) - (y1 - y2) * (x3 * y4 - y3 * x4)) / d;
    p = Vector(xi, yi);
    if (xi < java::min(x1, x2) || xi > java::max(x1, x2)) return false;
    if (xi < java::min(x3, x4) || xi > java::max(x3, x4)) return false;
    if (yi < java::min(y1, y2) || yi > java::max(y1, y2)) return false;
    if (yi < java::min(y3, y4) || yi > java::max(y3, y4)) return false;
    return true;
}

static void initPlayers() {
    // Generate heroes
    int spawnOffset = 1600;
    int spaceBetweenHeroes = 400;
    for (int i = 0; i < playerCount; ++i) {
        Player* player = &players[i];
        Vector vector = (i < 2 ? Vector(1, -1) : Vector(1, 1)).normalize();
        if (i % 2 == 1) vector = vector.mult(-1);
        // corners has 2 entries: a third player makes Referee.init throw (caught in init)
        if (i >= 2) throw java::Exception{"java.lang.ArrayIndexOutOfBoundsException: Index 2 out of bounds for length 2"};
        Vector startPoint = corners[i];
        basePositions[i] = startPoint;
        baseCount = i + 1;
        static const double offsets[] = {0, 1, -1, 2, -2, 3, -3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int j = 0; j < C::HEROES_PER_PLAYER; ++j) {
            double offset = offsets[j + (1 - C::HEROES_PER_PLAYER % 2)];
            Vector position = vector.mult(offset * (spaceBetweenHeroes)).add(startPoint).add(startDirections[i].mult(spawnOffset)).round();
            position = snapToGameZone(position);
            Entity* hero = newHero(j, position, player);
            player->heroes.push_back(hero);
            allHeroes.push_back(hero);
            newEntities.push_back(hero);
        }
    }
    // sendGlobalInfo
    for (Player* player : getActivePlayers()) {
        player->sendInputLine(basePositions[player->index].toIntString());
        player->sendInputLine(std::to_string(C::HEROES_PER_PLAYER));
    }
}

static bool insideMap(const Vector& p) {
    return p.withinBounds(-C::MAP_LIMIT, -C::MAP_LIMIT, C::MAP_WIDTH + C::MAP_LIMIT, C::MAP_HEIGHT + C::MAP_LIMIT);
}

static bool insideVisibleMap(const Vector& p) { return p.withinBounds(0, 0, C::MAP_WIDTH, C::MAP_HEIGHT); }

static Vector stepTo(const Vector& position, const Vector& destination, int speed) {
    Vector v = Vector::between(position, destination);
    Vector target;
    if (v.lengthSquared() <= speed * speed) target = v;
    else target = v.normalize().mult(speed);
    return position.add(target);
}

static Vector computeControlResult(const Entity* e, int moveSpeed) {
    Vector sum;
    for (size_t i = 0; i < e->activeControls.size(); i++) {
        Vector s = stepTo(e->position, e->activeControls[i], moveSpeed);
        sum = i == 0 ? s : sum.add(s);
    }
    return sum.mult(1.0 / e->activeControls.size());
}

static bool canSee(const Entity* hero, const Entity* entity) {
    if (C::ENABLE_FOG) return hero->position.inRange(entity->position, C::HERO_VIEW_RADIUS);
    return true;
}

static bool canSee(const Player* player, const Entity* entity) {
    if (!insideVisibleMap(entity->position)) return false;
    if (entity->getOwner() == player) return true;
    if (entity->position.inRange(basePositions[player->index], C::BASE_VIEW_RADIUS)) return true;
    for (const Entity* hero : player->heroes)
        if (canSee(hero, entity)) return true;
    return false;
}

static Vector baseWallIntersectionRaw(const Vector& from, const Vector& to, bool& found) {
    int w = C::MAP_WIDTH;
    int h = C::MAP_HEIGHT;
    int baseRadius = C::BASE_ATTRACTION_RADIUS;
    Vector p;
    found = false;
    if (to.y > h) found = intersection(from.x, from.y, to.x, to.y, w - baseRadius, h, w, h, p);
    else if (to.y < 0) found = intersection(from.x, from.y, to.x, to.y, 0, 0, baseRadius, 0, p);
    if (!found) {
        if (to.x > w) found = intersection(from.x, from.y, to.x, to.y, w, h - baseRadius, w, h, p);
        else if (to.x < 0) found = intersection(from.x, from.y, to.x, to.y, 0, 0, 0, baseRadius, p);
    }
    return found ? p.symmetricTruncate(symmetryOrigin) : Vector();
}

static bool isInBaseAttractionZone(const Vector& v) {
    for (int i = 0; i < baseCount; i++)
        if (v.inRange(basePositions[i], C::BASE_ATTRACTION_RADIUS)) return true;
    return false;
}

static void recordSpellUse(Entity* hero) {
    const Action& push = hero->intent;
    SpellUse su{};
    su.hero = hero->id;
    su.spell = push.type;
    su.target = push.target;
    su.hasDestination = push.hasDestination;
    if (push.hasDestination) {
        su.dx = java::d2i(push.destination.x);
        su.dy = java::d2i(push.destination.y);
    }
    spellUses.push_back(su);
}

// gameManager.addToGameSummary(hero.owner.getNicknameToken() + " failed a <spell>: " + e.getMessage())
static void spellFailed(Entity* hero, const char* spell, const std::string& message) {
    cg::gm.addToGameSummary("$" + std::to_string(hero->owner->index) + " failed a " + spell + ": " + message);
}
static std::string notVisible(Entity* hero, Entity* target) {
    if (canSee(hero->owner, target)) return "Entity " + std::to_string(target->id) + " is not within range of Hero " + std::to_string(hero->id);
    return "Hero " + std::to_string(hero->id) + " doesn't know where entity " + std::to_string(target->id) + " is";
}

static Entity* findVisibleEntity(int id) {
    for (Entity* e : allEntities())
        if (insideVisibleMap(e->position) && e->id == id) return e;
    return nullptr;
}

static void doControl() {
    // Incept next action into victim's mind
    for (Entity* hero : intentMap[A_CONTROL]) {
        const Action& control = hero->intent;
        if (hero->owner->mana < C::SPELL_CONTROL_COST) {
            spellFailed(hero, "CONTROL", "Not enough mana");
            continue;
        }
        Entity* victim = findVisibleEntity(control.target);
        if (!victim) {
            spellFailed(hero, "CONTROL", "Could not find entity " + std::to_string(control.target));
            continue;
        }
        if (canSee(hero, victim)) {
            hero->owner->spendMana(C::SPELL_CONTROL_COST);
            recordSpellUse(hero);
            if (!victim->hasActiveShield()) {
                victim->applyControl(control.destination);
            } else {
                spellFailed(hero, "CONTROL", "Entity " + std::to_string(victim->id) + " has a shield up"); // the mana is spent
            }
        } else {
            spellFailed(hero, "CONTROL", notVisible(hero, victim));
        }
    }
}

static void doShield() {
    // A protective bubble will appear around target on next turn
    for (Entity* hero : intentMap[A_SHIELD]) {
        const Action& control = hero->intent;
        if (hero->owner->mana < C::SPELL_PROTECT_COST) {
            spellFailed(hero, "SHIELD", "Not enough mana");
            continue;
        }
        Entity* entity = findVisibleEntity(control.target);
        if (!entity) {
            spellFailed(hero, "SHIELD", "Could not find entity " + std::to_string(control.target));
            continue;
        }
        if (canSee(hero, entity)) {
            hero->owner->spendMana(C::SPELL_PROTECT_COST);
            recordSpellUse(hero);
            if (!entity->hasActiveShield()) {
                entity->applyShield();
            } else {
                spellFailed(hero, "SHIELD", "Entity " + std::to_string(entity->id) + " already has a shield up"); // the mana is spent
            }
        } else {
            spellFailed(hero, "SHIELD", notVisible(hero, entity));
        }
    }
}

static void moveHeroes() {
    for (Entity* h : intentMap[A_MOVE]) h->position = snapToGameZone(h->intent.destination);
}

static void removeMob(Entity* mob) {
    if (!mob->removed) {
        mob->removed = true;
        mobRemovals.push_back(mob);
    }
}

// returns, per player: present, {mana, mana outside of base}
struct ManaGain {
    bool present[2] = {false, false};
    int amount[2][2] = {{0, 0}, {0, 0}};
};

static ManaGain performCombat() {
    std::vector<Entity*> killedMobs;
    ManaGain manaGain;
    // Deal hero damage to mobs
    for (Entity* h : allHeroes) {
        std::vector<int> mobsHit;
        bool isOutsideBaseRadius = !h->position.inRange(basePositions[h->owner->index], C::BASE_ATTRACTION_RADIUS);
        for (Entity* mob : allMobs) {
            if (!insideVisibleMap(mob->position)) continue;
            if (!mob->position.inRange(h->position, C::HERO_ATTACK_RANGE)) continue;
            mob->hit(C::HERO_ATTACK_DAMAGE);
            int o = h->owner->index;
            if (!manaGain.present[o]) {
                manaGain.present[o] = true;
                manaGain.amount[o][0] = C::HERO_ATTACK_DAMAGE;
                manaGain.amount[o][1] = isOutsideBaseRadius ? C::HERO_ATTACK_DAMAGE : 0;
            } else {
                manaGain.amount[o][0] += C::HERO_ATTACK_DAMAGE;
                manaGain.amount[o][1] += isOutsideBaseRadius ? C::HERO_ATTACK_DAMAGE : 0;
            }
            if (!mob->isAlive()) killedMobs.push_back(mob);
            mobsHit.push_back(mob->id);
        }
        if (!mobsHit.empty()) attacks.push_back({h->id, mobsHit});
    }
    for (Entity* m : killedMobs) removeMob(m);
    return manaGain;
}

static int ambiguousPushes = 0; // turns where the jar's order of random draws is not reproducible

static void doPush(int turn) {
    (void)turn;
    // HashMap<GameEntity, List<Vector>> directionMap, in insertion order
    std::vector<std::pair<Entity*, std::vector<Vector>>> directionMap;
    for (Entity* hero : intentMap[A_WIND]) {
        if (hero->owner->mana < C::SPELL_WIND_COST) {
            spellFailed(hero, "WIND", "Not enough mana");
            continue;
        }
        hero->owner->spendMana(C::SPELL_WIND_COST);
        const Action& push = hero->intent;
        recordSpellUse(hero);
        Vector dir = Vector::between(hero->position, push.destination).normalize().mult(C::SPELL_WIND_DISTANCE);
        for (Entity* e : allEntities()) {
            if (e->getOwner() == hero->owner) continue;
            if (!insideVisibleMap(e->position)) continue;
            if (!e->position.inRange(hero->position, C::SPELL_WIND_RADIUS)) continue;
            if (e->hasActiveShield()) continue;
            auto it = std::find_if(directionMap.begin(), directionMap.end(), [e](const auto& kv) { return kv.first == e; });
            if (it == directionMap.end()) directionMap.push_back({e, {dir}});
            else it->second.push_back(dir);
        }
    }

    // Calculate sum of pushes. Each entity only reads and writes its own position, so the
    // predicted positions can be computed first; only the random draws depend on the order.
    struct Push {
        Entity* entity;
        Vector predicted;
        bool randomDirection;
    };
    std::vector<Push> pushes;
    for (auto& kv : directionMap) {
        Entity* entity = kv.first;
        Vector sum = kv.second[0];
        for (size_t i = 1; i < kv.second.size(); i++) sum = sum.add(kv.second[i]);
        Vector predictedPosition = entity->position.add(sum).symmetricTruncate(symmetryOrigin);
        bool found;
        Vector bwi = baseWallIntersectionRaw(entity->position, predictedPosition, found);
        bool rnd = false;
        if (entity->isHero || found) {
            predictedPosition = snapToGameZone(found ? bwi : predictedPosition);
        } else if (entity->type == TYPE_MOB && isInBaseAttractionZone(entity->position) && !isInBaseAttractionZone(predictedPosition)) {
            rnd = true;
        } else {
            // a mob pushed without crossing a base wall nor leaving a base zone keeps its speed
        }
        pushes.push_back({entity, predictedPosition, rnd});
    }

    std::vector<int> order; // indices of the pushes that draw a random direction, in draw order
    for (int i = 0; i < (int)pushes.size(); i++)
        if (pushes[i].randomDirection) order.push_back(i);
    if (order.size() >= 2) {
        ambiguousPushes++;
        if (trace) fprintf(trace, "AMBIGUOUS T%d %d\n", currentTurn, (int)order.size());
    }

    // positionKeyMap: HashMap<Set<Vector>, Double>, a set {p, symmetric(p)} as a sorted pair
    struct Key {
        u64 a[2], b[2];
    };
    auto makeKey = [](const Vector& p) {
        Vector s = p.symmetric(symmetryOrigin);
        Key k;
        bool swap = std::make_pair(p.bitsX(), p.bitsY()) > std::make_pair(s.bitsX(), s.bitsY());
        const Vector& lo = swap ? s : p;
        const Vector& hi = swap ? p : s;
        k.a[0] = lo.bitsX();
        k.a[1] = lo.bitsY();
        k.b[0] = hi.bitsX();
        k.b[1] = hi.bitsY();
        return k;
    };
    auto applyRandom = [&](const std::vector<int>& ord, java::Random& rng, std::vector<Vector>* speeds) {
        std::vector<std::pair<Key, double>> positionKeyMap;
        for (int i : ord) {
            Key k = makeKey(pushes[i].predicted);
            double randomDouble;
            bool existing = false;
            for (auto& kv : positionKeyMap)
                if (!memcmp(&kv.first, &k, sizeof(Key))) {
                    randomDouble = kv.second;
                    existing = true;
                    break;
                }
            if (!existing) {
                randomDouble = rng.nextDouble();
                positionKeyMap.push_back({k, randomDouble});
            }
            double randomDirection = randomDouble * C::PI * 2;
            if (existing) randomDirection += C::PI;
            speeds->push_back(Vector::fromAngle(randomDirection).normalize().mult(C::MOB_MOVE_SPEED));
        }
    };

#ifdef SPIDER_HINTS
    if (order.size() > 8) {
        if (trace) fprintf(trace, "HINT T%d skipped (%d mobs)\n", currentTurn, (int)order.size());
    } else if (order.size() >= 2) {
        // pick the order whose speeds explain the jar's mob positions: a free move of a mob during
        // one of the next 3 turns (from its position at the end of the previous one) reveals its speed
        std::vector<int> perm = order, best = order;
        std::vector<std::vector<int>> perms;
        std::vector<int> scores;
        int ties = 0;
        int bestScore = -1;
        do {
            java::Random rng = random_;
            std::vector<Vector> speeds;
            applyRandom(perm, rng, &speeds);
            int score = 0;
            for (size_t j = 0; j < perm.size(); j++) {
                Entity* m = pushes[perm[j]].entity;
                Vector prev = pushes[perm[j]].predicted;
                for (int dt = 1; dt <= 3; dt++) {
                    int hx, hy;
                    if (!hintPosition(turn + dt, m->id, hx, hy)) break;
                    Vector next = prev.add(speeds[j]).symmetricTruncate(symmetryOrigin);
                    if (hx == java::d2i(next.x) && hy == java::d2i(next.y)) {
                        score++;
                        break;
                    }
                    prev = Vector(hx, hy);
                }
            }
            if (score > bestScore) {
                bestScore = score;
                best = perm;
                ties = 1;
            } else if (score == bestScore) {
                ties++;
            }
            perms.push_back(perm);
            scores.push_back(score);
        } while (std::next_permutation(perm.begin(), perm.end()));
        int forced = -1;
        for (auto& f : forcedOrders)
            if (f.first == turn) forced = f.second % (int)perms.size();
        if (forced >= 0) best = perms[forced];
        if (trace) {
            // the orders that explain the positions equally well (candidates for SPIDER_FORCE)
            std::string cand;
            for (size_t i = 0; i < perms.size(); i++)
                if (scores[i] == bestScore) cand += (cand.empty() ? "" : ",") + std::to_string(i);
            fprintf(trace, "HINT T%d score %d ties %d perms %d forced %d cand %s\n", currentTurn, bestScore, ties, (int)perms.size(), forced,
                    cand.c_str());
        }
        order = best;
    }
#endif

    std::vector<Vector> speeds;
    applyRandom(order, random_, &speeds);
    for (size_t j = 0; j < order.size(); j++) pushes[order[j]].entity->setSpeed(speeds[j]);
    for (auto& p : pushes) p.entity->pushTo(p.predicted);
}

static void moveMobs() {
    for (Entity* mob : allMobs) {
        if (!insideMap(mob->position)) {
            removeMob(mob);
            continue;
        }
        if (!mob->moveCancelled()) {
            if (!mob->activeControls.empty()) {
                Vector computedDestination = computeControlResult(mob, C::MOB_MOVE_SPEED);
                Vector newSpeed = Vector::between(mob->position, computedDestination);
                bool found;
                Vector bwi = baseWallIntersectionRaw(mob->position, computedDestination, found);
                computedDestination = snapToGameZone(found ? bwi : computedDestination);
                mob->position = computedDestination.symmetricTruncate(symmetryOrigin);
                if (!newSpeed.isZero()) {
                    mob->setSpeed(newSpeed.normalize().mult(C::MOB_MOVE_SPEED).truncate());
                } else {
                    // controlled towards its own position: the speed is kept
                }
            } else {
                mob->position = mob->position.add(mob->speed).symmetricTruncate(symmetryOrigin);
            }
        }
        for (int idx = 0; idx < baseCount; ++idx) {
            const Vector& base = basePositions[idx];
            if (mob->position.inRange(base, C::BASE_RADIUS)) {
                if (mob->health > 0) {
                    removeMob(mob);
                    Player& p = players[idx];
                    p.damageBase();
                    if (p.baseHealth > 0) cg::gm.addTooltip(p.index, "Base attacked!");
                    baseAttacks.push_back({idx, mob->id});
                    continue;
                } else {
                    // a mob killed this turn does not damage the base
                }
            }
            if (insideVisibleMap(mob->position) && mob->position.inRange(base, C::BASE_ATTRACTION_RADIUS)) { // mobCanDetectBase
                Vector v = Vector::between(mob->position, base);
                int distanceToStep = java::d2i(java::min(v.length(), (double)C::MOB_MOVE_SPEED));
                mob->setSpeed(base.sub(mob->position).normalize().mult(distanceToStep).truncate());
            } else if (mob->position.inRange(base, C::BASE_ATTRACTION_RADIUS)) {
                Vector objective(1, 1);
                if (mob->position.x < 0 || mob->position.y < 0) objective = objective.mult(-1);
                mob->setSpeed(objective.normalize().mult(C::MOB_MOVE_SPEED).truncate());
            }
        }
    }
}

static void shieldDecay() {
    for (Entity* e : allEntities())
        if (e->shieldDuration > 0) e->shieldDuration--;
}

static void spawnNewMobs(int turn) {
    std::vector<Entity*> newMobs = mobSpawner.update(turn);
    allMobs.insert(allMobs.end(), newMobs.begin(), newMobs.end());
    newEntities.insert(newEntities.end(), newMobs.begin(), newMobs.end());
}

static void performGameUpdate(int turn) {
    doControl();
    doShield();
    moveHeroes();
    ManaGain manaGain = performCombat();
    doPush(turn);
    moveMobs();
    shieldDecay();
    spawnNewMobs(turn);
    for (int i = 0; i < 2; i++)
        if (manaGain.present[i]) players[i].gainMana(manaGain.amount[i]);
}

static void recordIntention(Entity* hero, const Action& intent) {
    hero->intent = intent;
    intentMap[intent.type].push_back(hero);
}

// Referee.handleCommands, with the regexes of PLAYER_*_PATTERN matched by hand. A line never
// contains a line terminator (Scanner split it), so '.' matches any char. Every pattern ends with
// "(?:\s+(?<message>.+))?\s*$", which matches iff the rest is empty or starts with whitespace.
// The message only goes to the viewer.
struct Tokenizer {
    const jstring& s;
    size_t p = 0;
    explicit Tokenizer(const jstring& line) : s(line) {}
    bool lit(const char* w) {
        if (!java::startsWith(s, p, w)) return false;
        p += strlen(w);
        return true;
    }
    bool spaces() { // \s+
        size_t q = p;
        while (q < s.size() && java::isSpace(s[q])) q++;
        if (q == p) return false;
        p = q;
        return true;
    }
    bool number(size_t& a, size_t& b) { // -?\d+ (greedy)
        size_t q = p;
        if (q < s.size() && s[q] == '-') q++;
        size_t d = q;
        while (q < s.size() && java::isDigit(s[q])) q++;
        if (q == d) return false;
        a = p;
        b = q;
        p = q;
        return true;
    }
    bool rest() const { return p == s.size() || java::isSpace(s[p]); }
};

enum ParseResult { PR_OK, PR_INVALID };

// Integer.valueOf of each group in turn; on overflow `nfe` gets the group that threw
static bool parseInts(const jstring&, jstring&) { return true; }
template <class... Rest>
static bool parseInts(const jstring& line, jstring& nfe, size_t a, size_t b, i32& out, Rest&... rest) {
    if (!java::parseInt(line, a, b, out)) {
        nfe = line.substr(a, b - a);
        return false;
    }
    return parseInts(line, nfe, rest...);
}

static const char* EXPECTED = "MOVE <x> <y> | SPELL <spell_command> | WAIT";

static ParseResult handleLine(Entity* hero, const jstring& line, jstring& nfe) {
    size_t a1, b1, a2, b2, a3, b3;
    {
        Tokenizer t(line);
        if (t.lit("WAIT") && t.rest()) return PR_OK;
    }
    {
        Tokenizer t(line);
        if (t.lit("MOVE") && t.spaces() && t.number(a1, b1) && t.spaces() && t.number(a2, b2) && t.rest()) {
            i32 x, y;
            if (!parseInts(line, nfe, a1, b1, x, a2, b2, y)) return PR_INVALID;
            if (hero->position.x != x || hero->position.y != y) {
                Action intent;
                intent.type = A_MOVE;
                Vector target = stepTo(hero->position, Vector(x, y), C::HERO_MOVE_SPEED);
                intent.hasDestination = true;
                intent.destination = target.symmetricTruncate(symmetryOrigin);
                recordIntention(hero, intent);
            } else {
                // already there: no intent (the hero stays IDLE)
            }
            return PR_OK;
        }
    }
    if (C::ENABLE_WIND) {
        Tokenizer t(line);
        if (t.lit("SPELL") && t.spaces() && t.lit("WIND") && t.spaces() && t.number(a1, b1) && t.spaces() && t.number(a2, b2) && t.rest()) {
            i32 x, y;
            if (!parseInts(line, nfe, a1, b1, x, a2, b2, y)) return PR_INVALID;
            Action intent;
            intent.type = A_WIND;
            intent.hasDestination = true;
            intent.destination = Vector(x, y);
            recordIntention(hero, intent);
            return PR_OK;
        }
    }
    if (C::ENABLE_SHIELD) {
        Tokenizer t(line);
        if (t.lit("SPELL") && t.spaces() && t.lit("SHIELD") && t.spaces() && t.number(a1, b1) && t.rest()) {
            i32 entityId;
            if (!parseInts(line, nfe, a1, b1, entityId)) return PR_INVALID;
            Action intent;
            intent.type = A_SHIELD;
            intent.target = entityId;
            recordIntention(hero, intent);
            return PR_OK;
        }
    }
    if (C::ENABLE_CONTROL) {
        Tokenizer t(line);
        if (t.lit("SPELL") && t.spaces() && t.lit("CONTROL") && t.spaces() && t.number(a1, b1) && t.spaces() && t.number(a2, b2) &&
            t.spaces() && t.number(a3, b3) && t.rest()) {
            i32 entityId, x, y;
            if (!parseInts(line, nfe, a1, b1, entityId, a2, b2, x, a3, b3, y)) return PR_INVALID;
            Action intent;
            intent.type = A_CONTROL;
            intent.target = entityId;
            intent.hasDestination = true;
            intent.destination = Vector(x, y);
            recordIntention(hero, intent);
            return PR_OK;
        }
    }
    return PR_INVALID; // InvalidInputException
}

static void handleCommands(Player* player, const std::vector<jstring>& lines) {
    size_t i = 0;
    for (const jstring& line : lines) {
        // player.heroes.get(i++) is outside the try: more lines than heroes (only possible through
        // the Scanner desync) kills the referee
        if (i >= player->heroes.size())
            throw java::Exception{"java.lang.IndexOutOfBoundsException: Index " + std::to_string(i) + " out of bounds for length " + std::to_string(player->heroes.size())};
        Entity* hero = player->heroes[i++];
        if (!hero->activeControls.empty()) {
            Vector computedDestination = computeControlResult(hero, C::HERO_MOVE_SPEED);
            Action intent;
            intent.type = A_MOVE;
            intent.forced = true;
            intent.hasDestination = true;
            intent.destination = computedDestination.symmetricTruncate(symmetryOrigin);
            hero->activeControls.clear();
            recordIntention(hero, intent);
            continue;
        }
        jstring nfe;
        if (handleLine(hero, line, nfe) == PR_INVALID) {
            // InvalidInputException, or another exception wrapped in one
            jstring head = nfe.empty() ? jstring(u"Invalid Input") : u"java.lang.NumberFormatException: For input string: \"" + nfe + u"\"";
            player->deactivate(head + u": Expected " + java::fromAscii(EXPECTED) + u" but got '" + line + u"'");
            cg::gm.addToGameSummary("Bad command");
            return;
        }
    }
}

static void handlePlayerCommands() {
    for (Player* player : getActivePlayers()) {
        if (player->timeout) {
            player->deactivate("Timeout!");
            cg::gm.addToGameSummary("$" + std::to_string(player->index) + " has not provided " + std::to_string(player->getExpectedOutputLines()) +
                                    " lines in time");
            continue;
        }
        handleCommands(player, player->outputs);
    }
}

static void resetGameTurnData() {
    // Reset intentions
    for (Entity* h : allHeroes) h->intent = Action();
    for (auto& l : intentMap) l.clear();
    // Remove dead mobs
    if (!mobRemovals.empty()) {
        allMobs.erase(std::remove_if(allMobs.begin(), allMobs.end(), [](Entity* m) { return m->removed; }), allMobs.end());
        mobRemovals.clear();
    }
    // Reset mobs
    for (Entity* mob : allMobs) {
        mob->reset();
        if (!mob->activeControls.empty()) {
            Vector computedDestination = computeControlResult(mob, C::MOB_MOVE_SPEED);
            Vector newSpeed = Vector::between(mob->position, computedDestination);
            if (!newSpeed.isZero()) {
                mob->setSpeed(newSpeed.normalize().mult(C::MOB_MOVE_SPEED).truncate());
            } else {
                // controlled towards its own position: the speed is kept
            }
        }
    }
    // Reset view info
    newEntities.clear();
    attacks.clear();
    spellUses.clear();
    baseAttacks.clear();
    for (int i = 0; i < playerCount; i++) players[i].resetViewData();
}

static const int WANDERING = 0;
static const int ATTACKING = 1;

static const MobStatus& getMobStatus(Entity* mob) {
    if (!mob->hasStatus || !mob->activeControls.empty()) {
        Vector mobSpeed;
        if (!mob->activeControls.empty()) {
            Vector computedDestination = computeControlResult(mob, C::MOB_MOVE_SPEED);
            mobSpeed = Vector::between(mob->position, computedDestination);
        } else {
            mobSpeed = mob->speed;
        }
        if (mobSpeed.isZero()) {
            mob->status = MobStatus{0, -1, WANDERING};
        } else {
            Vector cur = mob->position;
            bool stop = false;
            int turns = 0;
            while (!stop && turns < 8000) {
                // Am I inside an attraction zone?
                for (int idx = 0; idx < baseCount; ++idx) {
                    if (cur.inRange(basePositions[idx], C::BASE_ATTRACTION_RADIUS)) {
                        mob->status = MobStatus{turns, idx, turns == 0 ? ATTACKING : WANDERING};
                        stop = true;
                        break;
                    }
                }
                // Am I outside the map?
                if (!insideVisibleMap(cur)) {
                    mob->status = MobStatus{turns, -1, WANDERING};
                    stop = true;
                }
                turns++;
                cur = cur.add(mobSpeed).symmetricTruncate(symmetryOrigin);
            }
            if (!stop) mob->status = MobStatus{0, -1, WANDERING}; // Failsafe
        }
        mob->hasStatus = true;
    }
    return mob->status;
}

static void sendGameStateFor(Player* player) {
    std::vector<std::string> entityLines;
    for (Entity* hero : allHeroes) {
        if (!canSee(player, hero)) continue;
        entityLines.push_back(std::to_string(hero->id) + " " +
                              std::to_string(hero->type == player->index ? INPUT_TYPE_MY_HERO : INPUT_TYPE_ENEMY_HERO) + " " +
                              hero->position.toIntString() + " " + std::to_string(hero->shieldDuration) + " " +
                              (hero->isControlled() ? "1" : "0") + " -1 -1 -1 -1 -1");
    }
    for (Entity* mob : allMobs) {
        if (!canSee(player, mob)) continue;
        const MobStatus& st = getMobStatus(mob);
        int threat = st.target < 0 ? 0 : (st.target == player->index ? 1 : 2);
        entityLines.push_back(std::to_string(mob->id) + " " + std::to_string(INPUT_TYPE_MOB) + " " + mob->position.toIntString() + " " +
                              std::to_string(mob->shieldDuration) + " " + (mob->isControlled() ? "1" : "0") + " " +
                              std::to_string(mob->health) + " " + mob->speed.toIntString() + " " + std::to_string(st.state) + " " +
                              std::to_string(threat));
    }
    // <health> <mana>
    player->sendInputLine(std::to_string(player->baseHealth) + " " + std::to_string(player->mana));
    for (int i = 0; i < playerCount; i++)
        if (&players[i] != player) player->sendInputLine(std::to_string(players[i].baseHealth) + " " + std::to_string(players[i].mana));
    // <entityCount>
    player->sendInputLine(std::to_string(entityLines.size()));
    for (auto& line : entityLines) player->sendInputLine(line);
}

// Referee.getCurrentFrameData + ViewModule.serialize, for the trace. Hero messages are left out
// (they only go to the viewer).
static std::string frameData() {
    std::string out;
    auto intMap = [](const std::vector<i32>& keys, const std::vector<std::string>& values) {
        std::string r;
        for (int k : java::hashMapOrder(keys)) r += (r.empty() ? "" : " ") + std::to_string(keys[k]) + " " + values[k];
        return r;
    };
    std::vector<Entity*> all = allEntities();
    {
        std::vector<i32> keys;
        std::vector<std::string> values;
        for (Entity* e : all) {
            keys.push_back(e->id);
            values.push_back(std::to_string(java::d2i(e->position.x)) + " " + std::to_string(java::d2i(e->position.y)));
        }
        out += intMap(keys, values) + "\n";
    }
    out += "<messages>\n";
    auto idList = [&](auto pred) {
        std::string r;
        for (Entity* e : all)
            if (pred(e)) r += (r.empty() ? "" : " ") + std::to_string(e->id);
        return r;
    };
    out += idList([](Entity* e) { return e->isControlled(); }) + "\n";
    out += idList([](Entity* e) { return e->gotPushed(); }) + "\n";
    out += idList([](Entity* e) { return e->hadActiveShield(); }) + "\n";
    {
        std::vector<i32> keys;
        std::vector<std::string> values;
        for (int i = 0; i < playerCount; i++)
            if (players[i].manaChanged) {
                keys.push_back(i);
                values.push_back(std::to_string(players[i].mana));
            }
        out += intMap(keys, values) + "\n";
        keys.clear();
        values.clear();
        for (int i = 0; i < playerCount; i++)
            if (players[i].baseHealthChanged) {
                keys.push_back(i);
                values.push_back(std::to_string(players[i].baseHealth));
            }
        out += intMap(keys, values) + "\n";
        keys.clear();
        values.clear();
        for (Entity* m : allMobs)
            if (m->healthChanged) {
                keys.push_back(m->id);
                values.push_back(std::to_string(m->health));
            }
        out += intMap(keys, values) + "\n";
    }
    {
        std::string r;
        for (Entity* e : newEntities)
            r += (r.empty() ? "" : " ") + std::to_string(e->id) + " " + std::to_string(e->type) + " " +
                 (e->isHero ? std::string("null") : std::to_string(e->health));
        out += r + "\n";
    }
    {
        std::string r;
        for (auto& a : attacks) {
            std::string ids;
            for (int id : a.second) ids += (ids.empty() ? "" : ";") + std::to_string(id);
            r += (r.empty() ? "" : " ") + std::to_string(a.first) + " " + ids;
        }
        out += r + "\n";
    }
    {
        std::string r;
        for (auto& b : baseAttacks) r += (r.empty() ? "" : " ") + std::to_string(b.first) + " " + std::to_string(b.second);
        out += r + "\n";
    }
    {
        std::string r;
        for (auto& s : spellUses)
            r += (r.empty() ? "" : " ") + std::to_string(s.hero) + " " + ACTION_NAMES[s.spell] + " " + std::to_string(s.target) + " " +
                 (s.hasDestination ? std::to_string(s.dx) + " " + std::to_string(s.dy) : std::string("0 0"));
        out += r;
    }
    if (spellUses.empty()) out += "\nX";
    return out;
}

static void traceFrame(int turn) {
    if (!trace) return;
    std::string f = frameData();
    fprintf(trace, "V%d %s\n", turn, jsonString(java::fromAscii(f)).c_str());
}

static void gameTurn(int turn) {
    currentTurn = turn;
    resetGameTurnData();
    // Give input to players
    for (Player* player : getActivePlayers()) {
        sendGameStateFor(player);
        player->execute();
    }
    // Get output from players
    handlePlayerCommands();
    performGameUpdate(turn);
    for (int i = 0; i < playerCount; i++)
        if (players[i].baseHealth == 0) players[i].deactivate("Base destroyed!");
    if (getActivePlayers().size() < 2) cg::gm.endGame();
}

static void onEnd() {
    if (playerCount == 2) {
        Player& a = players[0];
        Player& b = players[1];
        if (a.active && !b.active) {
            a.score = 1;
            b.score = 0;
        } else if (!a.active && b.active) {
            a.score = 0;
            b.score = 1;
        } else if (a.baseHealth != b.baseHealth || !C::ENABLE_TIE_BREAK) {
            a.score = a.baseHealth;
            b.score = b.baseHealth;
        } else {
            // Tie breaker
            a.score = a.manaGainedOutsideOfBase;
            b.score = b.manaGainedOutsideOfBase;
            if (a.manaGainedOutsideOfBase == b.manaGainedOutsideOfBase) {
                cg::gm.addToGameSummary("Tie!");
            } else {
                Player& winner = a.manaGainedOutsideOfBase > b.manaGainedOutsideOfBase ? a : b;
                cg::gm.addToGameSummary("$" + std::to_string(winner.index) + " won the game because their heroes gained more mana outside of their base:");
                cg::gm.addToGameSummary("$" + std::to_string(a.index) + ": " + std::to_string(a.manaGainedOutsideOfBase) + " mana");
                cg::gm.addToGameSummary("$" + std::to_string(b.index) + ": " + std::to_string(b.manaGainedOutsideOfBase) + " mana");
            }
        }
    } else {
        for (int i = 0; i < playerCount; i++) players[i].score = players[i].baseHealth;
    }
}

struct SpiderReferee : cg::Referee {
    void init() override {
        random_.setSeed(cg::gm.getSeed());
        startDirections[0] = Vector(1, 1).normalize();
        startDirections[1] = Vector(-1, -1).normalize();
        try {
            playerCount = cg::gm.getPlayerCount();
            initPlayers();
            cg::gm.setFrameDuration(500);
            cg::gm.setMaxTurns(C::MAX_TURNS);
            cg::gm.setTurnMaxTime(C::TURN_MAX_TIME);
        } catch (const java::Exception& e) {
            // "Referee failed to initialize", abort(): with 3 or 4 players (2 corners only) the game
            // ends before the first turn, nobody plays and every base keeps its STARTING_BASE_HEALTH
            fprintf(stderr, "%s\nReferee failed to initialize\n", e.text.c_str());
            cg::gm.endGame();
        }
        traceFrame(0);
    }
    void gameTurn(int turn) override {
        ::gameTurn(turn);
        traceFrame(turn);
    }
    void onEnd() override { ::onEnd(); }
};

int main(int argc, char** argv) {
#ifdef SPIDER_HINTS
    loadHints();
#endif
    static SpiderReferee referee;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; });
}
