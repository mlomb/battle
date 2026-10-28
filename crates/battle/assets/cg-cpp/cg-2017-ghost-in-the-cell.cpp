// Ghost in the Cell (CodinGame, 2017-02/03, "ghost-in-the-cell") - headless C++ referee, port of the CodinGame
// referee published in https://github.com/CodinGame/ghost-in-the-cell (commit a0a2cec, 2017-03-21, = HEAD and
// the repository's only commit: "Fix end conditions (thanks Agade)."; the game's class only, with its imports,
// `class Referee extends MultiReferee`). That is the multiplayer version after the contest (2017-02-25..03-06):
// it has the fixed end condition (a player without units nor production ends the game only when no player with
// no units still has production; the contest's version, e.g. the one in dreignier's brutaltester referee, ended
// the game as soon as one player had neither). 2 players. Deterministic; reproduces the same game as the
// reference jar for a given -seed.
//
// The base: no public copy of Ghost in the Cell's own base exists. The jar compiles the game's class with the
// closest public one, CodinGame's old AbstractReferee base of 2017 embedded in the Coders of the Caribbean
// referee of https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2017-04, two months later:
// referees/cg-builder/legacy/bases/base-2017-cotc.java, unchanged), and this port runs on common/legacy.h with
// legacy::Base::cotc2017(). The game calls nothing that base lacks. What depends on the base's behaviour and
// may have differed in CodinGame's own base of 2017-02: translate() of a message code the game does not define
// (the game's populateMessages only has "endReached", but it throws LostException("MoveFromNotControlledFactory",
// ...) and adds the tooltips translate("BombAction", ...) / translate("IncAction", ...)): this base returns the
// code itself (so the tooltips read "BombAction", "$0 lost: MoveFromNotControlledFactory"; with Hypersonic's 2016
// base a NullPointerException would end the game without scores); and the loss of a player (LostException,
// InvalidInputException, timeout) ending the game only at the next round's update (not turn based).
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): "seed=N") to stdout
// like the jar (referees/cg-builder/legacy: the referee runs unchanged in the runner of the SDK 4.5.0). "null"
// scores when the game fails (the Java referee crashed or the protocol broke; the jar after 150 s). -l writes
// the trace of common/legacy.h (bot executions, view sizes, summaries, tooltips, END).
//
// League: the referee's constant LEAGUE_LEVEL = 3 picks the default branch of its league switch ("Other
// leagues: add increase action"; levels 0, 1, 2 = Wood 3, 2, 1): up to 15 factories, several actions per turn,
// 2 bombs per player and the INC action = the rules of Bronze and above, CodinGame's top league. The lower
// leagues are not implemented, nor the init properties factory_count / initial_unit_count beyond what the
// referee does with them (the CLI only passes the seed).
//
// The game: 7-15 factories (an odd count; java.util.Random(seed)), one at the center, the others in pairs
// mirrored through the center, at least 2 * (radius + 300) apart (radius 600 above 10 factories, else 700);
// the distance between two factories in turns = Math.round((distance - both radii) / 800.) (double precision).
// Each player starts with one factory of 15-30 units; production 0-3 per factory. Each round every player
// answers one line of actions separated by ';' (the separator regex \s*;\s*(?=WAIT|MOVE|BOMB|INC|MSG), case
// insensitive): MOVE src dst units, BOMB src dst, INC src, WAIT, MSG text. An action from a factory the player
// does not own, or with source = destination, loses the game (LostException); an index out of range or an
// unknown action is an invalid input (the player loses). The update: troops and bombs move, the disabled
// countdowns decrease, then every player's orders run in player order (bombs: 2 per game, one per route and
// turn; troops: up to the factory's units, merged per route and turn, not on a bomb's route; INC: 10 units, up
// to production 3), owned factories produce (unless disabled), arrived troops fight (both players' first, the
// rest against the factory), bombs explode (half the units, at least 10, and 5 turns without production); the
// score is the units of a player's factories and troops; the game ends when the players without units have no
// production either (see above), or after 200 rounds.
//
// The Java referee is followed closely, notably: the regexes (\s of java.util.regex, ASCII case folding, 1-8
// digits) and Pattern.split (the whitespace before a ';' is the separator's: "MSG ;WAIT" leaves "MSG", invalid),
// the message (trimmed, cut to 100 UTF-16 chars), the double precision distances (no FMA: java.h's pragmas;
// checked against the jar's own Factory#computeDistances for every offset of 0..16000 x 0..6500, both radii),
// the entity ids (a static counter: a troop or bomb whose order is refused still takes one), the iteration
// orders (arrays and lists only: the game's own HashSet import is unused, its LinkedHashMap of distances is only
// read by key, java.awt.Point is only used for distances), the actions of a line before the action that makes
// the player lose still running in that round's update (from factories that turned neutral), and the last
// orders of a timed out player running again in that update (the game never clears them: troops and bombs
// without owner, INC on neutral factories), the NullPointerExceptions such orders could cause.
// Not reproduced: the tooltips' order inside one frame (the base's HashSet of identity hash codes: their size
// only counts, see legacy.h). The view (getInitDataForView / getFrameDataForView) matters only for its size:
// a few KB at most, far below the runner's 30000-byte limit (an invalid action's info, which carries the bot's
// text, can reach it only with invalid UTF-8: reproduced like the other ports on legacy.h).
#include "common/legacy.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// --- constants (the static block's league switch; LEAGUE_LEVEL 3: its default branch) ---
const int LEAGUE_LEVEL = 3;
const int MIN_FACTORY_COUNT = 7;
const int MAX_FACTORY_COUNT = LEAGUE_LEVEL == 0 ? 9 : 15;
const int MIN_PRODUCTION_RATE = 0;
const int MAX_PRODUCTION_RATE = 3;
const int MIN_TOTAL_PRODUCTION_RATE = 4;
const int BOMBS_PER_PLAYER = LEAGUE_LEVEL == 0 || LEAGUE_LEVEL == 1 ? 0 : 2;
const int PLAYER_INIT_UNITS_MIN = 15;
const int PLAYER_INIT_UNITS_MAX = 30;
const int WIDTH = 16000;
const int HEIGHT = 6500;
const int EXTRA_SPACE_BETWEEN_FACTORIES = 300;
const int COST_INCREASE_PRODUCTION = 10;
const int DAMAGE_DURATION = 5;
const bool MOVE_RESTRICTION_ENABLED = LEAGUE_LEVEL == 0;
const bool INCREASE_ACTION_ENABLED = LEAGUE_LEVEL != 0 && LEAGUE_LEVEL != 1 && LEAGUE_LEVEL != 2;
int FACTORY_RADIUS = 0; // static, set by generateFactories

String S(i64 v) { return java::str(v); }

[[noreturn]] void npe() { throw java::Exception{"java.lang.NullPointerException"}; }

// java.awt.Point#distance (Point2D.distance): Math.sqrt(dx * dx + dy * dy) in double precision
double pointDistance(int x1, int y1, int x2, int y2) {
    double px = (double)x2 - (double)x1;
    double py = (double)y2 - (double)y1;
    return std::sqrt(px * px + py * py);
}

// Factory#computeDistances: (int) Math.round((distance - radius - radius) / 800.)
int factoryDistance(int x1, int y1, int x2, int y2, int r1, int r2) {
    return (int)java::round((pointDistance(x1, y1, x2, y2) - r1 - r2) / 800.);
}

int UNIQUE_ENTITY_ID = 0; // Entity.UNIQUE_ENTITY_ID (static: from 0 in the jar's fresh JVM)

enum EntityType { FACTORY, TROOP, BOMB };
const char16_t* ENTITY_TYPE_NAME[] = {u"FACTORY", u"TROOP", u"BOMB"};

struct Player;
struct TroopList;

struct Entity {
    int id;
    EntityType type;
    explicit Entity(EntityType t) : id(UNIQUE_ENTITY_ID++), type(t) {}
    String toPlayerString(int arg1, int arg2, int arg3, int arg4, int arg5) const {
        return S(id) + u" " + ENTITY_TYPE_NAME[type] + u" " + S(arg1) + u" " + S(arg2) + u" " + S(arg3) + u" " + S(arg4) + u" " + S(arg5);
    }
};

struct Factory : Entity {
    Player* owner;
    int x, y; // position
    int unitCount;
    int productionRate;
    int disabled = 0;
    std::vector<int> distances; // the LinkedHashMap by factory id (ids 0..n-1 = the indices); -1: no entry
    int unitsReadyToFight[2] = {0, 0};

    Factory(Player* owner_, int x_, int y_, int unitCount_, int productionRate_)
        : Entity(FACTORY), owner(owner_), x(x_), y(y_), unitCount(unitCount_), productionRate(productionRate_) {}

    void computeDistances(const std::vector<Factory*>& factories) {
        distances.assign(factories.size(), -1);
        for (Factory* factory : factories)
            if (this != factory) distances[factory->id] = factoryDistance(x, y, factory->x, factory->y, getRadius(), factory->getRadius());
    }

    // distances.get(factory.id), unboxed: a NullPointerException for a factory without entry (itself)
    int getDistanceTo(const Factory* factory) const {
        if (factory->id < 0 || factory->id >= (int)distances.size() || distances[factory->id] < 0) npe();
        return distances[factory->id];
    }

    int getRadius() const { return FACTORY_RADIUS; }

    int getCurrentProductionRate() const { return (disabled == 0) ? this->productionRate : 0; }

    String toPlayerString(int playerIdx) const;
    String toViewStringInit() const { return S(id) + u" " + S(productionRate) + u" " + S(x) + u" " + S(y) + u" " + S(getRadius()); }
    String toViewString() const;
};

struct Player {
    int id;
    struct MoveAction {
        Factory* src;
        Factory* dst;
        int units;
    };
    struct BombAction {
        Factory* src;
        Factory* dst;
    };
    struct IncAction {
        Factory* src;
    };
    std::vector<MoveAction> lastMoveActions;
    std::vector<BombAction> lastBombActions;
    std::vector<IncAction> lastIncActions;
    NString message;
    int score = 0;
    std::vector<Factory*>* factories = nullptr;
    TroopList* troops = nullptr;
    int remainingBombs = BOMBS_PER_PLAYER;

    explicit Player(int id_) : id(id_) {}

    void setDead();
};

String Factory::toPlayerString(int playerIdx) const {
    int ownerShip = 0;
    if (owner != nullptr) ownerShip = (playerIdx == owner->id) ? 1 : -1;
    return Entity::toPlayerString(ownerShip, unitCount, this->productionRate, disabled, 0);
}

String Factory::toViewString() const {
    return (owner == nullptr ? String(u"-1") : S(owner->id)) + u" " + S(unitCount) + u" " + S(productionRate) + u" " + S(disabled);
}

struct MovingEntity : Entity {
    Player* owner;
    int remainingTurns;
    Factory* source;
    Factory* destination;

    MovingEntity(EntityType type, Factory* source_, Factory* destination_)
        : Entity(type), owner(source_->owner), remainingTurns(0), source(source_), destination(destination_) {
        remainingTurns = source->getDistanceTo(destination);
    }

    void move() { this->remainingTurns--; }

    template <class A>
    A* findWithSameRouteInList(const std::vector<A*>& list) const {
        for (A* other : list)
            if (other->source == this->source && other->destination == this->destination) return other;
        return nullptr;
    }
};

struct Bomb : MovingEntity {
    Bomb(Factory* source_, Factory* destination_) : MovingEntity(BOMB, source_, destination_) {}

    String toPlayerString(int playerIdx) const {
        if (owner == nullptr) npe();
        if (owner->id == playerIdx) return Entity::toPlayerString(1, source->id, destination->id, remainingTurns, 0);
        return Entity::toPlayerString(-1, source->id, -1, -1, 0);
    }

    String toViewString() const {
        return S(id) + u" " + S(owner == nullptr ? 0 : owner->id) + u" " + S(source->id) + u" " + S(destination->id) + u" " + S(remainingTurns);
    }

    void explode() {
        int damage = std::min(destination->unitCount, std::max(10, destination->unitCount / 2));
        destination->unitCount -= damage;
        destination->disabled = DAMAGE_DURATION;
    }
};

struct Troop : MovingEntity {
    int unitCount;

    Troop(Factory* source_, Factory* destination_, int unitCount_) : MovingEntity(TROOP, source_, destination_), unitCount(unitCount_) {}

    String toPlayerString(int playerIdx) const {
        int ownerShip = 0;
        if (owner != nullptr) ownerShip = (playerIdx == owner->id) ? 1 : -1;
        return Entity::toPlayerString(ownerShip, source->id, destination->id, unitCount, remainingTurns);
    }

    String toViewString() const {
        return S(id) + u" " + S(owner == nullptr ? 0 : owner->id) + u" " + S(source->id) + u" " + S(destination->id) + u" " + S(unitCount) + u" " +
               S(remainingTurns);
    }
};

// the LinkedLists of troops / bombs: insertion order, removal while iterating
struct TroopList {
    std::vector<Troop*> items;
};

void Player::setDead() {
    // When a player is dead, it loses its factories and troops
    for (Factory* factory : *factories)
        if (factory->owner == this) factory->owner = nullptr;
    std::vector<Troop*>& list = troops->items;
    list.erase(std::remove_if(list.begin(), list.end(), [&](Troop* troop) { return troop->owner == this; }), list.end());
    this->score = 0;
}

// ----------------------------------------------------------------------------
// The referee's regexes (CASE_INSENSITIVE, without UNICODE_CASE: ASCII folding only). The lines never hold a
// line terminator (the base's Scanner split them there).
// ----------------------------------------------------------------------------

// the literal (case insensitive) text `word` at s[i..]
bool hasWordAt(const String& s, size_t i, const char* word) {
    size_t w = strlen(word);
    if (i > s.size() || s.size() - i < w) return false;
    for (size_t k = 0; k < w; k++)
        if (!java::regexCharMatchesIgnoreCase(s[i + k], word[k])) return false;
    return true;
}

// PLAYER_INPUT_ACTION_SEPARATOR_PATTERN.split(line): \s*;\s*(?=WAIT|MOVE|BOMB|INC|MSG), limit 0.
// A match at p: the whitespace run from p ends on a ';' at q, and the whitespace run after it ends on one of
// the words (the greedy \s* can only backtrack onto whitespace, which starts no word). Pattern#split: the
// parts between the matches (an empty first part included: the matches are never empty), the rest after the
// last one, the trailing empty parts removed; no match: the line itself.
std::vector<String> splitActions(const String& s) {
    size_t n = s.size();
    std::vector<size_t> wsEnd(n + 1);
    wsEnd[n] = n;
    for (size_t i = n; i-- > 0;) wsEnd[i] = java::isRegexSpace(s[i]) ? wsEnd[i + 1] : i;
    auto lookahead = [&](size_t r) {
        return hasWordAt(s, r, "WAIT") || hasWordAt(s, r, "MOVE") || hasWordAt(s, r, "BOMB") || hasWordAt(s, r, "INC") || hasWordAt(s, r, "MSG");
    };
    std::vector<String> list;
    size_t index = 0;
    for (size_t p = 0; p < n; p++) {
        size_t q = wsEnd[p];
        if (q >= n || s[q] != u';') continue;
        size_t r = wsEnd[q + 1];
        if (!lookahead(r)) continue;
        list.push_back(s.substr(index, p - index));
        index = r;
        p = r - 1; // the next search starts at the end of this match
    }
    if (index == 0) return {s};
    list.push_back(s.substr(index));
    size_t size = list.size();
    while (size > 0 && list[size - 1].empty()) size--;
    list.resize(size);
    return list;
}

// [0-9]{1,8} from i, followed by something that is not a digit: false for no digit or more than 8 (the
// greedy count can only give back digits, which the next token (\s or the end) never takes)
bool matchDigits(const String& s, size_t& i, String& group) {
    size_t a = i;
    while (i < s.size() && java::isRegexDigit(s[i])) i++;
    if (i - a < 1 || i - a > 8) return false;
    group = s.substr(a, i - a);
    return true;
}

// \s+ from i (all of the whitespace run: the next token is a digit)
bool matchSpaces(const String& s, size_t& i) {
    size_t a = i;
    while (i < s.size() && java::isRegexSpace(s[i])) i++;
    return i > a;
}

// MOVE (?<src>[0-9]{1,8})\s+(?<dst>[0-9]{1,8})\s+(?<units>([0-9]{1,8}))
bool matchMove(const String& s, String& src, String& dst, String& units) {
    size_t i = 5;
    return hasWordAt(s, 0, "MOVE ") && matchDigits(s, i, src) && matchSpaces(s, i) && matchDigits(s, i, dst) && matchSpaces(s, i) &&
           matchDigits(s, i, units) && i == s.size();
}

// BOMB (?<src>[0-9]{1,8})\s+(?<dst>[0-9]{1,8})
bool matchBomb(const String& s, String& src, String& dst) {
    size_t i = 5;
    return hasWordAt(s, 0, "BOMB ") && matchDigits(s, i, src) && matchSpaces(s, i) && matchDigits(s, i, dst) && i == s.size();
}

// INC (?<src>[0-9]{1,8})
bool matchInc(const String& s, String& src) {
    size_t i = 4;
    return hasWordAt(s, 0, "INC ") && matchDigits(s, i, src) && i == s.size();
}

// WAIT
bool matchWait(const String& s) { return java::regexMatchesIgnoreCase(s, "WAIT"); }

// MSG (?<message>.*)   ('.': no line terminator)
bool matchMsg(const String& s, String& message) {
    if (!hasWordAt(s, 0, "MSG ")) return false;
    for (size_t k = 4; k < s.size(); k++)
        if (java::isLineSeparator(s[k])) return false;
    message = s.substr(4);
    return true;
}

i32 parseInt(const String& digits) {
    i32 v = 0;
    java::parseInt(digits, v); // 1-8 ASCII digits: always an int
    return v;
}

struct Referee : legacy::MultiReferee {
    std::vector<Player*> players;
    std::vector<Factory*> factories;
    TroopList troops;
    std::vector<Troop*> newTroops;
    std::vector<Bomb*> bombs;
    std::vector<Bomb*> newBombs;
    java::Random random;

    // Properties
    i64 seed = 0;
    std::optional<i32> customFactoryCount;
    std::optional<i32> customInitialUnitCount;

    Referee() { base = legacy::Base::cotc2017(); }

    // Integer.valueOf / Long.valueOf: a NumberFormatException (a RuntimeException: an init error)
    static i64 valueOf(const String& v, bool isLong) {
        i64 r;
        bool ok;
        if (isLong) {
            ok = java::parseLong(v, r);
        } else {
            i32 x;
            ok = java::parseInt(v, x);
            r = x;
        }
        if (!ok) throw java::Exception{java::numberFormatException(java::encodeUtf8(v))};
        return r;
    }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        // new Random(System.currentTimeMillis()).nextLong(): evaluated, unused (the CLI always gives the seed)
        i64 now = (i64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        this->seed = valueOf(prop.getProperty(u"seed", S(java::Random(now).nextLong())), true);
        if (const String* factoryCount = prop.find(u"factory_count")) this->customFactoryCount = (i32)valueOf(*factoryCount, false);
        if (const String* initialUnitCount = prop.find(u"initial_unit_count")) this->customInitialUnitCount = (i32)valueOf(*initialUnitCount, false);

        newTroops.clear();
        newBombs.clear();

        this->random = java::Random(seed);
        generatePlayers(playerCount);
        generateFactories();

        troops.items.clear();
        bombs.clear();

        for (Player* player : players) {
            player->troops = &troops;
            player->factories = &factories;
        }
    }

    void generatePlayers(int playerCount) {
        players.clear();
        for (int i = 0; i < playerCount; i++) players.push_back(new Player(i));
    }

    // Generate the factory objects
    void generateFactories() {
        int factoryCount;
        if (customFactoryCount && *customFactoryCount >= MIN_FACTORY_COUNT && *customFactoryCount <= MAX_FACTORY_COUNT) {
            factoryCount = *customFactoryCount;
        } else {
            factoryCount = MIN_FACTORY_COUNT + random.nextInt(MAX_FACTORY_COUNT - MIN_FACTORY_COUNT + 1);
        }

        if (factoryCount % 2 == 0) factoryCount++; // factoryCount must be odd
        FACTORY_RADIUS = factoryCount > 10 ? 600 : 700;

        int minSpaceBetweenFactories = 2 * (FACTORY_RADIUS + EXTRA_SPACE_BETWEEN_FACTORIES);

        factories.assign(factoryCount, nullptr);

        int i = 0;

        // Add one factory at the center of the map
        factories[i++] = new Factory(nullptr, WIDTH / 2, HEIGHT / 2, 0, 0);

        while (i < factoryCount - 1) {
            int x = random.nextInt(WIDTH / 2 - 2 * FACTORY_RADIUS) + FACTORY_RADIUS + EXTRA_SPACE_BETWEEN_FACTORIES;
            int y = random.nextInt(HEIGHT - 2 * FACTORY_RADIUS) + FACTORY_RADIUS + EXTRA_SPACE_BETWEEN_FACTORIES;

            bool valid = true;
            for (int j = 0; j < i; j++) {
                Factory* factory = factories[j];
                if (pointDistance(factory->x, factory->y, x, y) < minSpaceBetweenFactories) {
                    valid = false;
                    break;
                }
            }

            if (valid) {
                int productionRate = MIN_PRODUCTION_RATE + random.nextInt(MAX_PRODUCTION_RATE - MIN_PRODUCTION_RATE + 1);

                if (i == 1) {
                    int unitCount;
                    if (customInitialUnitCount && *customInitialUnitCount >= PLAYER_INIT_UNITS_MIN && *customInitialUnitCount <= PLAYER_INIT_UNITS_MAX) {
                        unitCount = *customInitialUnitCount;
                    } else {
                        unitCount = PLAYER_INIT_UNITS_MIN + random.nextInt(PLAYER_INIT_UNITS_MAX - PLAYER_INIT_UNITS_MIN + 1);
                    }
                    if (players.size() < 2) // players[1]
                        throw java::Exception{java::arrayIndexOutOfBoundsException(1, (i64)players.size())};
                    factories[i++] = new Factory(players[0], x, y, unitCount, productionRate);
                    factories[i++] = new Factory(players[1], WIDTH - x, HEIGHT - y, unitCount, productionRate);
                } else {
                    int unitCount = random.nextInt(5 * productionRate + 1);
                    factories[i++] = new Factory(nullptr, x, y, unitCount, productionRate);
                    factories[i++] = new Factory(nullptr, WIDTH - x, HEIGHT - y, unitCount, productionRate);
                }
            }
        }

        int totalProductionRate = 0;
        for (Factory* factory : factories) {
            factory->computeDistances(this->factories);
            totalProductionRate += factory->productionRate;
        }

        // Make sure that the initial accumulated production rate for all the factories is at least MIN_TOTAL_PRODUCTION_RATE
        for (int j = 1; totalProductionRate < MIN_TOTAL_PRODUCTION_RATE && j < (int)factories.size(); j++) {
            if (factories[j]->productionRate < MAX_PRODUCTION_RATE) {
                factories[j]->productionRate++;
                totalProductionRate++;
            }
        }
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(this->seed));
        if (customFactoryCount) prop.setProperty(u"factory_count", S(*customFactoryCount));
        if (customInitialUnitCount) prop.setProperty(u"initial_unit_count", S(*customInitialUnitCount));
        return prop;
    }

    // --- AbstractReferee ---
    Lines getInitInputForPlayer(int) override {
        Lines data;
        data.push_back(S((i64)factories.size()));

        // Factory distances
        Lines links;
        for (size_t i = 0; i < factories.size(); i++)
            for (size_t j = i + 1; j < factories.size(); j++)
                links.push_back(S(factories[i]->id) + u" " + S(factories[j]->id) + u" " + S(factories[i]->getDistanceTo(factories[j])));
        data.push_back(S((i64)links.size()));
        data.insert(data.end(), links.begin(), links.end());
        return data;
    }

    void prepare(int) override {}

    Lines getInputForPlayer(int, int playerIdx) override {
        Lines data;
        Lines entities;
        for (Factory* factory : factories) entities.push_back(factory->toPlayerString(playerIdx));
        for (Troop* troop : troops.items) entities.push_back(troop->toPlayerString(playerIdx));
        for (Bomb* bomb : bombs) entities.push_back(bomb->toPlayerString(playerIdx));
        data.push_back(S((i64)entities.size()));
        data.insert(data.end(), entities.begin(), entities.end());
        return data;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 1; }

    void handlePlayerOutput(int, int, int playerIdx, const Lines& outputs) override {
        Player* player = players[playerIdx];
        player->lastBombActions.clear();
        player->lastIncActions.clear();
        player->lastMoveActions.clear();
        player->message.reset();
        try {
            int n = (int)factories.size();
            for (const String& line : outputs) {
                for (const String& action : splitActions(line)) {
                    String g1, g2, g3;
                    if (matchMove(action, g1, g2, g3)) {
                        if (MOVE_RESTRICTION_ENABLED && !player->lastMoveActions.empty()) continue; // Silently ignore multiple moves

                        int src = parseInt(g1);
                        int dst = parseInt(g2);
                        int units = parseInt(g3);

                        if (src >= n) throw legacy::InvalidInputException(u"0 <= source < " + S(n), S(src));
                        if (dst >= n) throw legacy::InvalidInputException(u"0 <= destination < " + S(n), S(dst));
                        if (factories[src]->owner != player) throw legacy::LostException(std::string("MoveFromNotControlledFactory"), {Arg(src)});
                        if (src == dst) throw legacy::LostException(std::string("MoveSameSourceDestination"), {Arg(src)});

                        player->lastMoveActions.push_back({factories[src], factories[dst], units});
                    } else if (matchBomb(action, g1, g2)) {
                        int src = parseInt(g1);
                        int dst = parseInt(g2);
                        if (src >= n) throw legacy::InvalidInputException(u"0 <= source < " + S(n), S(src));
                        if (dst >= n) throw legacy::InvalidInputException(u"0 <= destination < " + S(n), S(dst));
                        if (factories[src]->owner != player) throw legacy::LostException(std::string("BombFromNotControlledFactory"), {Arg(src)});
                        if (src == dst) throw legacy::LostException(std::string("BombSameSourceDestination"), {Arg(src)});

                        player->lastBombActions.push_back({factories[src], factories[dst]});
                    } else if (matchInc(action, g1)) {
                        if (!INCREASE_ACTION_ENABLED) continue; // Silently ignore increase actions

                        int src = parseInt(g1);

                        if (src >= n) throw legacy::InvalidInputException(u"0 <= source < " + S(n), S(src));
                        if (factories[src]->owner != player) throw legacy::LostException(std::string("IncFromNotControlledFactory"), {Arg(src)});

                        player->lastIncActions.push_back({factories[src]});
                    } else if (matchWait(action)) {
                        // do nothing.
                    } else if (matchMsg(action, g1)) {
                        String message = java::trim(g1);
                        if (message.size() > 100) message = message.substr(0, 100);
                        player->message = message;
                    } else {
                        throw legacy::InvalidInputException(u"A valid action", action);
                    }
                }
            }
        } catch (const legacy::InvalidInputException&) {
            player->setDead();
            throw;
        } catch (const legacy::LostException&) {
            player->setDead();
            throw;
        }
    }

    void updateGame(int) override {
        newTroops.clear();
        newBombs.clear();

        // Move troops and bombs
        for (Troop* troop : troops.items) troop->move();
        for (Bomb* bomb : bombs) bomb->move();

        // Decrease disabled countdown
        for (Factory* factory : factories)
            if (factory->disabled > 0) factory->disabled--;

        // Execute orders
        for (Player* player : players) {
            // Send bombs
            for (const Player::BombAction& bombAction : player->lastBombActions) {
                Bomb* bomb = new Bomb(bombAction.src, bombAction.dst);
                if (player->remainingBombs > 0 && bomb->findWithSameRouteInList(newBombs) == nullptr) {
                    newBombs.push_back(bomb);
                    bombs.push_back(bomb);
                    player->remainingBombs--;
                    addToolTip(player->id, translate("BombAction", {player->id, bombAction.src->id, bombAction.dst->id}));
                }
            }

            // Send troops
            for (const Player::MoveAction& moveAction : player->lastMoveActions) {
                int unitsToMove = std::min(moveAction.src->unitCount, moveAction.units);
                Troop* troop = new Troop(moveAction.src, moveAction.dst, unitsToMove);

                if (unitsToMove > 0 && troop->findWithSameRouteInList(newBombs) == nullptr) { // Forbid sending units with the same source and destination as a bomb
                    moveAction.src->unitCount -= unitsToMove;

                    Troop* other = troop->findWithSameRouteInList(newTroops);
                    if (other != nullptr) {
                        other->unitCount += unitsToMove;
                    } else {
                        troops.items.push_back(troop);
                        newTroops.push_back(troop);
                    }
                }
            }

            // Increase
            for (const Player::IncAction& incAction : player->lastIncActions) {
                if (incAction.src->unitCount >= COST_INCREASE_PRODUCTION && incAction.src->productionRate < MAX_PRODUCTION_RATE) {
                    incAction.src->productionRate++;
                    incAction.src->unitCount -= COST_INCREASE_PRODUCTION;
                    addToolTip(player->id, translate("IncAction", {player->id, incAction.src->id}));
                }
            }
        }

        // Create new units
        for (Factory* factory : factories)
            if (factory->owner != nullptr) factory->unitCount += factory->getCurrentProductionRate();

        // Solve battles
        for (Factory* factory : factories) factory->unitsReadyToFight[0] = factory->unitsReadyToFight[1] = 0;
        {
            std::vector<Troop*>& list = troops.items;
            size_t w = 0;
            for (size_t k = 0; k < list.size(); k++) {
                Troop* troop = list[k];
                if (troop->remainingTurns <= 0) {
                    if (troop->owner == nullptr) npe(); // troop.owner.id
                    troop->destination->unitsReadyToFight[troop->owner->id] += troop->unitCount;
                } else {
                    list[w++] = troop;
                }
            }
            list.resize(w);
        }
        for (Factory* factory : factories) {
            // Units from both players fight first
            int units = std::min(factory->unitsReadyToFight[0], factory->unitsReadyToFight[1]);
            factory->unitsReadyToFight[0] -= units;
            factory->unitsReadyToFight[1] -= units;

            // Remaining units fight on the factory
            for (Player* player : players) {
                if (factory->owner == player) { // Allied
                    factory->unitCount += factory->unitsReadyToFight[player->id];
                } else { // Opponent
                    if (factory->unitsReadyToFight[player->id] > factory->unitCount) {
                        factory->owner = player;
                        factory->unitCount = factory->unitsReadyToFight[player->id] - factory->unitCount;
                    } else {
                        factory->unitCount -= factory->unitsReadyToFight[player->id];
                    }
                }
            }
        }

        // Solve bombs
        {
            size_t w = 0;
            for (size_t k = 0; k < bombs.size(); k++) {
                Bomb* bomb = bombs[k];
                if (bomb->remainingTurns <= 0) bomb->explode();
                else bombs[w++] = bomb;
            }
            bombs.resize(w);
        }

        // Update score
        for (Player* player : players) player->score = 0;
        for (Factory* factory : factories)
            if (factory->owner != nullptr) factory->owner->score += factory->unitCount;
        for (Troop* troop : troops.items)
            if (troop->owner != nullptr) troop->owner->score += troop->unitCount;

        // Check end conditions
        bool gameOver = false;
        for (Player* player : players) {
            if (player->score == 0) {
                int production = 0;
                for (Factory* factory : factories)
                    if (factory->owner == player) production += factory->productionRate;
                if (production == 0) {
                    gameOver = true;
                } else {
                    // Keep playing until this player has produced some units
                    gameOver = false;
                    break;
                }
            }
        }

        if (gameOver) throw legacy::GameOverException(std::string("endReached"));
    }

    void populateMessages(std::map<std::string, String>& p) override { p["endReached"] = u"End reached"; }

    Lines getInitDataForView() override {
        Lines data;
        data.push_back(S(WIDTH) + u" " + S(HEIGHT) + u" " + S((i64)factories.size()) + u" " + S(BOMBS_PER_PLAYER));
        for (Factory* factory : factories) data.push_back(factory->toViewStringInit());
        data.insert(data.begin(), S((i64)data.size() + 1));
        return data;
    }

    Lines getFrameDataForView(int, int, int, bool) override {
        Lines data;
        // Pass the scores and messages
        for (size_t playerIdx = 0; playerIdx < players.size(); ++playerIdx) {
            String playerInfo = S(getScore((int)playerIdx)) + u" " + S(players[playerIdx]->remainingBombs);
            if (players[playerIdx]->message) playerInfo += u" " + *players[playerIdx]->message;
            data.push_back(playerInfo);
        }

        // Pass the troops
        data.push_back(S((i64)newTroops.size()));
        for (Troop* troop : newTroops) data.push_back(troop->toViewString());

        // Pass the bombs
        data.push_back(S((i64)newBombs.size()));
        for (Bomb* bomb : newBombs) data.push_back(bomb->toViewString());

        // Pass the factories
        for (Factory* factory : factories) data.push_back(factory->toViewString());
        return data;
    }

    String getGameName() override { return u"GhostInTheCell"; }
    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }
    Lines getPlayerActions(int, int) override { return {}; }
    bool isPlayerDead(int) override { return false; }
    NString getDeathReason(int playerIdx) override { return u"$" + S(playerIdx) + u": Eliminated!"; }
    int getScore(int playerIdx) override { return players[playerIdx]->score; }
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
