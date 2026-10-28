// Hypersonic (CodinGame, 2016-09, "hypersonic") - headless C++ referee, port of the CodinGame referee in
// https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the file
// "Referees/Hyper Sonic/Referee.java", unchanged since it was added in 92ac4d9, 2018-04-03): the
// single-file referee `class Referee extends MultiReferee` with CodinGame's old AbstractReferee base of
// 2016 (common/legacy.h, its default legacy::Base), 2 to 4 players, GAME_VERSION 4 (the top league's
// rules). Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 <cmd> -p4 <cmd>] -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): "boxes=N"
// and "seed=N", in the order of a JDK 9+ Properties) to stdout like the jar (referees/cg-builder/legacy:
// the referee runs unchanged in the runner of the SDK 4.5.0). "null" scores when the game fails (the
// Java referee crashed or the protocol broke; the jar after 150 s). -l writes the trace of
// common/legacy.h (bot executions, view sizes, summaries, tooltips, END).
//
// League: the referee takes its version from the init property GAME_VERSION, 4 by default (the CLI only
// passes the seed): GAME_VERSION >= 2 items (bomb and range upgrades in a third each of the boxes),
// >= 3 no practice mode (walls, boxes anywhere, deaths) and the game continues when the boxes are gone,
// >= 4 friendly fire (a player's own bombs kill it) and the game ends 20 rounds after the last box. 4
// is the highest version and CodinGame's rules from Bronze up; the lower ones (the wood leagues) are not
// implemented here, nor the other init properties (WIDTH, HEIGHT, boxes, BOMB_FUSE...: the CLI never
// passes them).
//
// The game: a 13x11 grid, walls on odd (x, y), 30 + nextInt(36) box draws mirrored in both axes (a
// java.util.Random(seed)), the first third of the boxes hold a range upgrade, the next third a bomb
// upgrade, the copies the same item. Players start in the corners (0,0), (12,10), (12,0), (0,10) with
// one bomb of range 3. Each round every player answers "MOVE x y [message]" or "BOMB x y [message]"
// (the referee's regexes, case insensitive: see matchCommand), the round's update: bombs tick (fuse 8),
// explode (chains, blocked by walls / boxes / items / other bombs, killing anyone on their cells, their
// owner included), bombs are placed (on a cell without bomb, ammo permitting), boxes and items blown,
// players move one step on the shortest path (BFS) toward the free cell closest to their target
// (Manhattan, ties in BFS order), pick items up, boxes pop their items. The score is the boxes blown
// (several players blowing a box all count it) + 1000000 alive, or + 100 x the round of death (invalid
// output: the round it was sent; timeout: likewise). The game ends after 200 rounds, 20 rounds without
// bomb nor move, 20 rounds after the last box, or when fewer than 2 players are left (at the end of the
// round: its last updateGame still runs).
//
// The Java referee is followed closely, notably: the regexes (\s and \d of java.util.regex, ASCII case
// folding, Integer.valueOf overflowing to an invalid action, the message = what follows the whitespace
// after y, cut to 17 chars + "..." past 19), int arithmetic of the Manhattan distances to far targets,
// the BFS (LinkedList queue, Direction order LEFT RIGHT UP DOWN, strict < for the closest cell), the
// HashSet<Cell> order of the boxes blown in a round (it orders the new items in the input), the item and
// bomb ids, a dead player whose bombs still score, two players picking the same item both getting it.
// Not reproduced (the jar is not deterministic there): the order of the bombs exploding in the same step
// (a HashSet<Bomb> of identity hash codes): this port takes them in the order they joined the set. It
// only shows when two boxes blown by different bombs of the same step, both holding an item, share a
// bucket of that HashSet<Cell>: their items' order in the input may differ from the jar's.
// The view (getInitDataForView / getFrameDataForView, the game summary, tooltips) is built for its size
// only: it never comes near the runner's 30000-byte limit (a few KB; bot text there: 20-char messages).
//
// Validation only: compiled with -DHYPERSONIC_HINTS, the referee takes the order of the bombs exploding
// in the same step from the file named by the environment variable HYPERSONIC_HINTS (lines
// "<round> <step> <x> <y> <bomb id>" in the jar's order, from the explosions of the jar's view), to
// replay a jar game exactly. The default build has none of this.
#include "common/legacy.h"

#include <algorithm>
#include <deque>

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// --- constants (GAME_VERSION 4) ---
const int DEFAULT_TIME_LIMIT = 200;
const int NO_PROGRESS_TIMEOUT = 20;
const int EXTRA_ROUNDS_AFTER_NO_BOXES = 20;
const bool ITEMS = true;
const bool VISIBLE_ITEMS = true;
const bool FRIENDLY_FIRE = true;
const bool END_ON_NO_BOXES = false;
const bool COUNTDOWN_ON_NO_BOXES = true;
const bool PRACTICE_MODE = false;
const int WIDTH = 13;
const int HEIGHT = 11;
const int DEFAULT_BOMB_POWER = 3;
const int DEFAULT_BOMBS_PER_PLAYER = 1;
const int BOMB_FUSE = 8;
const int SUDDEN_DEATH = -1; // no sudden death
const int SUDDEN_DEATH_SLOWNESS = 10;
const bool SYMMETRY_MAP_AND_ITEMS = true; // SYMMETRY

String S(i64 v) { return java::str(v); }
String A(const char* s) { return java::ascii(s); }

struct Coord {
    int x = 0, y = 0;
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    Coord add(int dx, int dy) const { return Coord{x + dx, y + dy}; }
    // Math.abs(x - this.x) + Math.abs(y - this.y), int arithmetic
    i32 manhattanTo(int tx, int ty) const { return java::add(java::abs(java::sub(tx, x)), java::abs(java::sub(ty, y))); }
    i32 manhattanTo(const Coord& t) const { return manhattanTo(t.x, t.y); }
    String toString() const { return S(x) + u" " + S(y); }
    i32 hashCode() const { return 31 * (31 * 1 + x) + y; }
};

enum Direction { LEFT, RIGHT, UP, DOWN };
const int DX[4] = {-1, 1, 0, 0};
const int DY[4] = {0, 0, -1, 1};
const char* DIRECTION_NAME[4] = {"LEFT", "RIGHT", "UP", "DOWN"};

const int ENTITY_PLAYER = 0, ENTITY_BOMB = 1, ENTITY_ITEM = 2;
const int FIRE_UP = 1, BOMB_UP = 2;
const int BLOCK = 0, FLOOR = 1, BOX = 2;

struct Man;
struct PlayerInfo;
struct Cell;

int ITEM_COUNT = 0;
int BOMB_COUNT = 0;

struct Item {
    int id;
    int itemType;
    Cell* position = nullptr; // set when its box blows
    std::vector<Man*> removedBy;
    explicit Item(int type) : id(++ITEM_COUNT), itemType(type) {}
};

struct Bomb {
    int id;
    Coord position;
    int power = 0, fuse = 0;
    Man* owner;
    explicit Bomb(Man* o) : id(++BOMB_COUNT), owner(o) {}
    void arm(Coord c, int p, int f) {
        power = p;
        fuse = f;
        position = c;
    }
};

struct Cell : Coord {
    int type = FLOOR;
    Item* item = nullptr;
    int boxId = 0;
    std::vector<Bomb*> bombs;
    bool hasItem() const { return item != nullptr; }
    bool isBox() const { return type == BOX; }
    bool hasBomb() const { return !bombs.empty(); }
    bool isFree() const { return type == FLOOR && !hasBomb(); }
    String toPlayerString(bool visibleItems) const {
        switch (type) {
        case BLOCK:
            return u"X";
        case FLOOR:
            return u".";
        case BOX:
            if (visibleItems && hasItem()) return S(item->itemType);
            return u"0";
        default:
            return u"X";
        }
    }
};

struct Explosion;

struct Man {
    int id; // the player's index
    Coord position;
    bool success = false, confused = false, dead = false, piercingBombs = false;
    Explosion* killedBy = nullptr;
    int nextMove = -1; // a Direction, -1: null
    enum Action { NONE, MOVE, BOMB } attempt = NONE;
    bool hasAttemptCoord = false;
    Coord attemptCoord;
    NString message;
    int direction = DOWN;
    int power, maxAmmo;
    PlayerInfo* player;
    std::vector<Bomb*> ammo; // a Stack
    Man(Coord c, int maxAmmo_, int power_, PlayerInfo* p, int index) : id(index), position(c), power(power_), maxAmmo(maxAmmo_), player(p) {
        for (int i = 0; i < maxAmmo_; ++i) ammo.push_back(new Bomb(this));
    }
    int getX() const { return position.x; }
    int getY() const { return position.y; }
    void reset() {
        message.reset();
        attempt = NONE;
        hasAttemptCoord = false;
        nextMove = -1;
        confused = false;
        success = false;
    }
};

struct PlayerInfo {
    bool invalidAction = false;
    bool dead = false;
    int index = 0, deadAt = 0;
    Man* man = nullptr;
    int boxesHit = 0;
};

struct Explosion {
    Bomb* source;
    std::vector<Cell*> flammables;
    std::vector<Bomb*> bombs;
    int span[4] = {0, 0, 0, 0};
    explicit Explosion(Bomb* s) : source(s) {}
    String toFrameString() const {
        return source->position.toString() + u" " + S(span[LEFT]) + u" " + S(span[RIGHT]) + u" " + S(span[UP]) + u" " + S(span[DOWN]) + u" " + S(source->id);
    }
};

#ifdef HYPERSONIC_HINTS
// hints[round][step]: the jar's explosions (x, y, bomb id) in its order
std::vector<std::vector<std::vector<std::vector<int>>>> hints;
void loadHints() {
    const char* p = getenv("HYPERSONIC_HINTS");
    if (!p) return;
    if (FILE* f = fopen(p, "r")) {
        int r, g, x, y, id;
        while (fscanf(f, "%d %d %d %d %d", &r, &g, &x, &y, &id) == 5) {
            if (r < 0 || r > 100000 || g < 0 || g > 1000) continue;
            if ((int)hints.size() <= r) hints.resize(r + 1);
            if ((int)hints[r].size() <= g) hints[r].resize(g + 1);
            hints[r][g].push_back({x, y, id});
        }
        fclose(f);
    }
}
// reorders the bombs of a step like the jar's (by position and id, then position), the others last
void orderByHint(int round, int step, std::vector<Bomb*>& bombs) {
    if (round >= (int)hints.size() || step >= (int)hints[round].size()) return;
    std::vector<Bomb*> rest = bombs, out;
    for (auto& h : hints[round][step]) {
        int found = -1;
        for (int pass = 0; pass < 2 && found < 0; pass++)
            for (size_t i = 0; i < rest.size() && found < 0; i++)
                if (rest[i] && rest[i]->position.x == h[0] && rest[i]->position.y == h[1] && (pass == 1 || rest[i]->id == h[2])) found = (int)i;
        if (found >= 0) {
            out.push_back(rest[found]);
            rest[found] = nullptr;
        }
    }
    for (Bomb* b : rest)
        if (b) out.push_back(b);
    bombs = out;
}
#endif

String entityString(int type, int owner, const Coord& position, int state, int value) {
    return S(type) + u" " + S(owner) + u" " + position.toString() + u" " + S(state) + u" " + S(value);
}

// Matcher#matches of
//   <word>\s+(?<x>-?\d+)\s+(?<y>-?\d+)(?:\s+)?(?:\s+(?<message>.+))?   (CASE_INSENSITIVE)
// <word> folds the ASCII letters only; the message is what follows the whitespace after y (none when
// only whitespace follows); '.' never meets a line terminator (the Scanner split the lines there).
bool matchCommand(const String& line, const char* word, String& gx, String& gy, NString& message) {
    size_t n = line.size(), i = 0, w = strlen(word);
    if (n < w) return false;
    for (; i < w; i++)
        if (!java::regexCharMatchesIgnoreCase(line[i], word[i])) return false;
    auto spaces = [&]() {
        size_t a = i;
        while (i < n && java::isRegexSpace(line[i])) i++;
        return i > a;
    };
    auto number = [&](String& g) {
        size_t a = i;
        if (i < n && line[i] == u'-') i++;
        size_t d = i;
        while (i < n && java::isRegexDigit(line[i])) i++;
        if (i == d) return false;
        g = line.substr(a, i - a);
        return true;
    };
    if (!spaces() || !number(gx) || !spaces() || !number(gy)) return false;
    if (i == n) {
        message.reset();
        return true;
    }
    if (!spaces()) return false;
    if (i == n) message.reset();
    else message = line.substr(i);
    return true;
}

struct Referee : legacy::MultiReferee {
    i64 seed = 0;
    int boxCount = 0;
    java::Random random;
    std::vector<Cell> grid; // HashMap<Coord, Cell>: lookups only
    std::vector<PlayerInfo*> players;
    std::vector<Bomb*> activeBombs;
    std::vector<Item*> activeItems;
    std::vector<Item*> removedItems; // a HashSet<Item> of identity hash codes (view only)
    std::vector<Cell*> boxes;
    java::HashMapOrder explodedBoxes; // HashSet<Cell>: key = cell index, hash = Coord#hashCode
    std::vector<Man*> activeMen;
    std::vector<std::vector<Explosion*>> explosionGroups;
    int countDown = 0, noProgressCountDown = 0, extraCountDown = 0;

    static int index(int x, int y) { return y * WIDTH + x; }
    Cell* get(int x, int y) {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return nullptr;
        return &grid[index(x, y)];
    }
    Cell* get(const Coord& c) { return get(c.x, c.y); }

    std::vector<Cell*> explodedBoxesInOrder() {
        std::vector<i32> keys;
        explodedBoxes.keys(keys);
        std::vector<Cell*> r;
        for (i32 k : keys) r.push_back(&grid[k]);
        return r;
    }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        // Long.valueOf(prop.getProperty("seed", ...)): the CLI always gives the seed
        String s = prop.getProperty(u"seed", u"0");
        if (!java::parseLong(s, seed)) throw java::Exception{java::encodeUtf8(java::numberFormatException(s))};
        random = java::Random(seed);

        Coord startingPositions[4] = {{0, 0}, {WIDTH - 1, HEIGHT - 1}, {WIDTH - 1, 0}, {0, HEIGHT - 1}};
        boxCount = 30 + random.nextInt(36);
        countDown = SUDDEN_DEATH;
        noProgressCountDown = NO_PROGRESS_TIMEOUT;
        extraCountDown = EXTRA_ROUNDS_AFTER_NO_BOXES;

        grid.assign(WIDTH * HEIGHT, Cell());
        for (int y = 0; y < HEIGHT; ++y)
            for (int x = 0; x < WIDTH; ++x) {
                Cell& c = grid[index(x, y)];
                c.x = x;
                c.y = y;
                c.type = (x % 2 == 1 && y % 2 == 1) ? BLOCK : FLOOR;
            }

        // LinkedHashMap: BOMB_UP then FIRE_UP; the last one with some left is dealt
        int toBombUp = ITEMS ? boxCount / 3 : 0;
        int toFireUp = ITEMS ? boxCount / 3 : 0;

        int iterations = 0;
        int boxId = 0;
        while (boxId < boxCount) {
            iterations++;
            if (iterations > 1000) break;
            int x = random.nextInt(WIDTH);
            int y = random.nextInt(HEIGHT);
            bool ok = true;
            for (const Coord& sp : startingPositions)
                if (sp.manhattanTo(x, y) < 2) {
                    ok = false;
                    break;
                }
            if (!ok) continue;
            Cell* cell = get(x, y);
            if (cell->type != FLOOR) continue;
            cell->type = BOX;
            int itemType = 0;
            if (toBombUp > 0) itemType = BOMB_UP;
            if (toFireUp > 0) itemType = FIRE_UP;
            if (itemType != 0) {
                (itemType == BOMB_UP ? toBombUp : toFireUp)--;
                cell->item = new Item(itemType);
            }
            cell->boxId = boxId++;
            boxes.push_back(cell);
            Cell* others[3] = {get(WIDTH - x - 1, y), get(WIDTH - x - 1, HEIGHT - y - 1), get(x, HEIGHT - y - 1)};
            for (Cell* c : others) {
                if (c->type != BOX) {
                    c->type = BOX;
                    if (SYMMETRY_MAP_AND_ITEMS && cell->hasItem()) {
                        c->item = new Item(cell->item->itemType); // copy()
                        (itemType == BOMB_UP ? toBombUp : toFireUp)--;
                    }
                    c->boxId = boxId++;
                    boxes.push_back(c);
                }
            }
        }
        boxCount = boxId;

        for (int i = 0; i < playerCount; ++i) {
            PlayerInfo* p = new PlayerInfo();
            p->index = i;
            p->man = new Man(startingPositions[i], DEFAULT_BOMBS_PER_PLAYER, DEFAULT_BOMB_POWER, p, i);
            players.push_back(p);
            activeMen.push_back(p->man);
        }
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        prop.setProperty(u"boxes", S(boxCount));
        return prop;
    }

    // --- AbstractReferee ---
    void prepare(int) override {
        for (PlayerInfo* p : players) p->man->reset();
    }

    Lines getInitInputForPlayer(int playerIdx) override { return {S(WIDTH) + u" " + S(HEIGHT) + u" " + S(playerIdx)}; }

    Lines getInputForPlayer(int, int) override {
        Lines lines;
        for (int y = 0; y < HEIGHT; ++y) {
            String row;
            for (int x = 0; x < WIDTH; ++x) row += get(x, y)->toPlayerString(VISIBLE_ITEMS);
            lines.push_back(row);
        }
        Lines entities;
        for (PlayerInfo* p : players)
            if (!p->dead) entities.push_back(entityString(ENTITY_PLAYER, p->index, p->man->position, (int)p->man->ammo.size(), p->man->power));
        for (Bomb* b : activeBombs) entities.push_back(entityString(ENTITY_BOMB, b->owner->player->index, b->position, b->fuse, b->power));
        for (Item* i : activeItems) entities.push_back(entityString(ENTITY_ITEM, 0, *i->position, i->itemType, i->itemType));
        lines.push_back(S((i64)entities.size()));
        lines.insert(lines.end(), entities.begin(), entities.end());
        return lines;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 1; }

    void handlePlayerOutput(int, int round, int playerIdx, const Lines& outputs) override {
        static const String EXPECTED = u"MOVE x y | BOMB x y";
        PlayerInfo* player = players[playerIdx];
        Man* man = player->man;
        const String& line = outputs.at(0);
        String gx, gy;
        NString message;
        bool valid = false;
        if (matchCommand(line, "MOVE", gx, gy, message)) {
            i32 x, y;
            if (java::parseInt(gx, x) && java::parseInt(gy, y)) {
                if (man->getX() != x || man->getY() != y) {
                    man->attempt = Man::MOVE;
                    man->hasAttemptCoord = true;
                    man->attemptCoord = Coord{x, y};
                }
                matchMessage(man, message);
                valid = true;
            }
        } else if (matchCommand(line, "BOMB", gx, gy, message)) {
            man->attempt = Man::BOMB; // before the coordinates are parsed
            i32 x, y;
            if (java::parseInt(gx, x) && java::parseInt(gy, y)) {
                if (man->getX() != x || man->getY() != y) {
                    man->hasAttemptCoord = true;
                    man->attemptCoord = Coord{x, y};
                }
                matchMessage(man, message);
                valid = true;
            }
        }
        if (!valid) { // no match, or Integer.valueOf overflowed
            player->dead = true;
            player->deadAt = round;
            player->invalidAction = true;
            throw legacy::InvalidInputException(EXPECTED, line);
        }
    }

    static void matchMessage(Man* man, const NString& message) {
        man->message = message;
        if (man->message && man->message->size() > 19) man->message = man->message->substr(0, 17) + u"...";
    }

    bool moveBFS(Man* man) {
        // fifo (LinkedList), route (HashMap<Coord, Coord>): parent cell indices, -2 for the start
        std::vector<int> route(WIDTH * HEIGHT, -1);
        std::deque<Coord> fifo;
        Coord start = man->position;
        route[index(start.x, start.y)] = -2;
        fifo.push_back(start);
        Coord closest = start;
        i32 by = start.manhattanTo(man->attemptCoord);
        bool foundTarget = false;
        while (!foundTarget && !fifo.empty()) {
            Coord e = fifo.front();
            fifo.pop_front();
            for (int d = 0; d < 4; d++) {
                Coord n = e.add(DX[d], DY[d]);
                Cell* c = get(n);
                if (c != nullptr && c->isFree() && route[index(c->x, c->y)] == -1) {
                    if (man->attemptCoord == n) foundTarget = true;
                    route[index(c->x, c->y)] = index(e.x, e.y);
                    fifo.push_back(n);
                    i32 distance = c->manhattanTo(man->attemptCoord);
                    if (distance < by) {
                        by = distance;
                        closest = n;
                    }
                }
            }
        }
        Coord cur = closest, prev = closest;
        while (cur != start) {
            prev = cur;
            int p = route[index(cur.x, cur.y)];
            cur = Coord{p % WIDTH, p / WIDTH};
        }
        if (man->getX() < prev.x) man->nextMove = RIGHT;
        else if (man->getX() > prev.x) man->nextMove = LEFT;
        else if (man->getY() < prev.y) man->nextMove = DOWN;
        else if (man->getY() > prev.y) man->nextMove = UP;
        return true;
    }

    bool move(Man* man) { return moveBFS(man); }

    std::vector<Man*> menOn(const Coord& c) {
        std::vector<Man*> res;
        for (Man* man : activeMen)
            if (!man->dead && man->position == c) res.push_back(man);
        return res;
    }

    void unspawnBomb(Bomb* bomb) {
        auto it = std::find(activeBombs.begin(), activeBombs.end(), bomb);
        if (it != activeBombs.end()) {
            activeBombs.erase(it);
            bomb->owner->ammo.push_back(bomb);
            std::vector<Bomb*>& bs = get(bomb->position)->bombs;
            bs.erase(std::find(bs.begin(), bs.end(), bomb));
        }
    }

    static bool removeItem(std::vector<Item*>& list, Item* item) {
        auto it = std::find(list.begin(), list.end(), item);
        if (it == list.end()) return false;
        list.erase(it);
        return true;
    }

    void updateGame(int round) override {
        bool progress = false;
        explosionGroups.clear();

        // Compute explosions. nextBombs is a HashSet<Bomb> (identity hash codes): taken here in the
        // order the bombs joined it
        std::vector<Bomb*> nextBombs;
        auto addNext = [&](Bomb* b) {
            if (std::find(nextBombs.begin(), nextBombs.end(), b) == nextBombs.end()) nextBombs.push_back(b);
        };
        for (Bomb* bomb : activeBombs) {
            progress = true;
            bomb->fuse--;
            if (bomb->fuse <= 0) addNext(bomb);
        }
        std::vector<bool> blownUp(WIDTH * HEIGHT, false); // HashSet<Coord> of bomb positions (all in the grid)
        auto isBlownUp = [&](const Coord& c) { return blownUp[index(c.x, c.y)]; };
#ifdef HYPERSONIC_HINTS
        int step = 0;
#endif
        while (!nextBombs.empty()) {
#ifdef HYPERSONIC_HINTS
            orderByHint(round, step++, nextBombs);
#endif
            std::vector<Explosion*> currentExplosions;
            for (Bomb* bomb : nextBombs) {
                Explosion* e = new Explosion(bomb);
                for (int d = 0; d < 4; d++) {
                    int distance = bomb->power;
                    Coord cur = bomb->position;
                    bool blocked = false;
                    while (!blocked && distance > 0) {
                        Cell* c = get(cur);
                        if (c != nullptr && ((c->type != BLOCK && !isBlownUp(*c)) || c->hasBomb())) {
                            if (cur != bomb->position && (c->isBox() || c->hasBomb() || c->hasItem())) {
                                if (!isBlownUp(*c)) {
                                    if (c->isBox() || c->hasItem()) {
                                        e->flammables.push_back(c);
                                    } else if (c->hasBomb()) {
                                        for (Bomb* b : c->bombs)
                                            if (b->fuse > 0) e->bombs.push_back(b);
                                    }
                                }
                                if (!bomb->owner->piercingBombs) blocked = true;
                            }
                            if (!PRACTICE_MODE) {
                                for (Man* man : menOn(*c)) {
                                    if (FRIENDLY_FIRE || bomb->owner != man) {
                                        man->dead = true;
                                        if (man->killedBy == nullptr) man->killedBy = e;
                                    }
                                }
                            }
                            cur = cur.add(DX[d], DY[d]);
                            distance--;
                        } else {
                            blocked = true;
                        }
                    }
                    e->span[d] = bomb->power - distance;
                }
                currentExplosions.push_back(e);
            }
            nextBombs.clear();
            if (!currentExplosions.empty()) {
                for (Explosion* e : currentExplosions) {
                    for (Bomb* b : e->bombs) blownUp[index(b->position.x, b->position.y)] = true;
                    blownUp[index(e->source->position.x, e->source->position.y)] = true;
                    for (Bomb* b : e->bombs) addNext(b);
                }
                explosionGroups.push_back(currentExplosions);
            }
        }

        // Sudden death: SUDDEN_DEATH < 0

        // Compute intentions
        struct BombPlacement {
            Man* owner;
            Cell* cell;
        };
        std::vector<BombPlacement> bombPlacements;
        for (PlayerInfo* player : players) {
            if (player->dead) continue;
            Man* man = player->man;
            if (man->dead) {
                player->dead = true;
                player->deadAt = round;
                continue;
            }
            if (man->attempt == Man::BOMB) {
                Cell* c = get(man->position);
                if (!man->ammo.empty() && c != nullptr && c->isFree()) {
                    bombPlacements.push_back({man, c});
                    man->success = true;
                    progress = true;
                } else {
                    man->confused = true;
                }
                if (man->hasAttemptCoord) move(man);
            } else if (man->attempt == Man::MOVE) {
                man->success = move(man);
            }
        }

        // Place bombs
        for (BombPlacement& bp : bombPlacements) {
            Bomb* b = bp.owner->ammo.back();
            bp.owner->ammo.pop_back();
            b->arm(bp.owner->position, bp.owner->power, BOMB_FUSE);
            bp.cell->bombs.push_back(b);
            activeBombs.push_back(b);
        }

        // Apply explosions (and retrieve ammo)
        std::vector<std::vector<bool>> boxHit(players.size(), std::vector<bool>(WIDTH * HEIGHT, false)); // HashSet<Cell> per player
        explodedBoxes.clear();
        for (auto& group : explosionGroups) {
            for (Explosion* explosion : group) {
                unspawnBomb(explosion->source);
                for (Bomb* bomb : explosion->bombs) unspawnBomb(bomb);
                for (Cell* cell : explosion->flammables) {
                    if (cell->isBox()) {
                        explodedBoxes.put(index(cell->x, cell->y), cell->hashCode());
                        auto it = std::find(boxes.begin(), boxes.end(), cell);
                        if (it != boxes.end()) boxes.erase(it);
                        boxHit[explosion->source->owner->player->index][index(cell->x, cell->y)] = true;
                    } else if (cell->hasItem()) {
                        if (removeItem(activeItems, cell->item)) removedItems.push_back(cell->item);
                        cell->item = nullptr;
                    }
                }
            }
        }
        for (PlayerInfo* player : players)
            for (bool hit : boxHit[player->index])
                if (hit) player->boxesHit++;

        // Perform actions
        std::vector<Item*> collectedItems; // a HashSet<Item>: only removed below, any order
        for (Man* man : activeMen) {
            if (man->nextMove >= 0) {
                progress = true;
                man->position = man->position.add(DX[man->nextMove], DY[man->nextMove]);
                man->direction = man->nextMove;
                Cell* location = get(man->position);
                if (location->item != nullptr) {
                    Item* item = location->item;
                    if (std::find(collectedItems.begin(), collectedItems.end(), item) == collectedItems.end()) collectedItems.push_back(item);
                    item->removedBy.push_back(man);
                    if (item->itemType == BOMB_UP) { // BombUpItem
                        man->ammo.push_back(new Bomb(man));
                        man->maxAmmo += 1;
                    } else { // FireUpItem
                        man->power++;
                    }
                }
            }
        }
        for (Item* item : collectedItems) {
            Cell* location = get(*item->position);
            if (removeItem(activeItems, location->item)) removedItems.push_back(location->item);
            location->item = nullptr;
        }

        // Pop items, in the HashSet<Cell>'s order
        for (Cell* box : explodedBoxesInOrder()) {
            Item* item = box->item;
            if (item != nullptr) {
                activeItems.push_back(item);
                item->position = box;
            }
            box->type = FLOOR;
        }

        if (progress) noProgressCountDown = NO_PROGRESS_TIMEOUT;
        else --noProgressCountDown;
        if (boxes.empty()) --extraCountDown;
    }

    int getDepth() const { return countDown < 0 ? -countDown / SUDDEN_DEATH_SLOWNESS : 0; }

    void populateMessages(std::map<std::string, String>& p) override {
        p["move"] = u"$%d moves to %d,%d";
        p["bomb"] = u"$%d places a bomb";
        p["bombError"] = u"$%d wants to place a bomb but cannot";
    }

    Lines getInitDataForView() override {
        Lines lines;
        lines.push_back(S(WIDTH) + u" " + S(HEIGHT) + u" " + S((i64)players.size()) + u" " + S(SUDDEN_DEATH < 0 ? DEFAULT_TIME_LIMIT : SUDDEN_DEATH) + u" " +
                        S(PRACTICE_MODE ? 1 : 0));
        lines.push_back(S((i64)boxes.size()));
        for (Cell* box : boxes) lines.push_back(S(box->boxId) + u" " + S(box->x) + u" " + S(box->y) + u" " + box->toPlayerString(true));
        lines.insert(lines.begin(), S((i64)lines.size() + 1));
        return lines;
    }

    Lines getFrameDataForView(int round, int, int, bool) override {
        Lines lines;
        lines.push_back(S(round));
        for (PlayerInfo* player : players) {
            Man* m = player->man;
            lines.push_back(S(m->getX()) + u" " + S(m->getY()) + u" " + A(DIRECTION_NAME[m->direction]) + u" " + S(player->boxesHit) + u" " + S(player->dead ? 0 : 1) +
                            u" " + S((i64)m->ammo.size()) + u" " + S(m->maxAmmo) + u" " + S(getScore(player->index)) + u" " + (m->message ? *m->message : String()));
        }
        lines.push_back(S((i64)activeBombs.size()));
        for (Bomb* b : activeBombs)
            lines.push_back(b->position.toString() + u" " + S(b->power) + u" " + S(b->fuse) + u" " + S(b->id) + u" " + S(b->owner->player->index));
        lines.push_back(S((i64)explosionGroups.size()));
        for (auto& group : explosionGroups) {
            lines.push_back(S((i64)group.size()));
            for (Explosion* e : group) lines.push_back(e->toFrameString());
        }
        std::vector<Cell*> exploded = explodedBoxesInOrder();
        lines.push_back(S((i64)exploded.size()));
        for (Cell* box : exploded) lines.push_back(S(box->boxId));
        lines.push_back(S((i64)activeItems.size()));
        for (Item* item : activeItems) lines.push_back(item->position->toString() + u" " + S(item->itemType) + u" " + S(item->id));
        lines.push_back(S((i64)removedItems.size()));
        for (Item* item : removedItems) lines.push_back(S(item->id) + u" 1"); // (removedBy is never null)
        lines.push_back(S(getDepth()));
        return lines;
    }

    String getGameName() override { return u"Hypersonic"; }
    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }
    Lines getPlayerActions(int, int) override { return {}; }
    bool isPlayerDead(int playerIdx) override { return players[playerIdx]->dead; }

    NString getDeathReason(int playerIdx) override {
        if (players[playerIdx]->man->killedBy != nullptr) return u"$" + S(playerIdx) + u": Disintegrated!";
        return u"$" + S(playerIdx) + u": Eliminated!";
    }

    int getScore(int playerIdx) override {
        PlayerInfo* player = players[playerIdx];
        return player->boxesHit + (player->dead ? (player->deadAt * 100) : 1000000);
    }

    Lines getGameSummary(int round) override {
        Lines lines;
        for (int i = 0; i < (int)players.size(); ++i) {
            PlayerInfo* player = players[i];
            Man* man = player->man;
            if (man->attempt == Man::MOVE && man->success) {
                lines.push_back(translate("move", {player->index, man->position.x, man->position.y}));
            } else if (man->attempt == Man::BOMB) {
                lines.push_back(translate(man->success ? "bomb" : "bombError", {player->index}));
                if (man->hasAttemptCoord && man->nextMove >= 0) lines.push_back(translate("move", {player->index, man->position.x, man->position.y}));
            }
            if (man->dead && man->player->deadAt == round) lines.push_back(*getDeathReason(i));
        }
        return lines;
    }

    void setPlayerTimeout(int, int round, int playerIdx) override {
        PlayerInfo* player = players[playerIdx];
        player->dead = true;
        player->deadAt = round;
        player->man->dead = true;
    }

    int getMaxRoundCount(int) override { return DEFAULT_TIME_LIMIT; } // SUDDEN_DEATH < 0

    bool gameOver() override {
        return noProgressCountDown <= 0 || (COUNTDOWN_ON_NO_BOXES && extraCountDown <= 0) || (END_ON_NO_BOXES && boxes.empty()) || legacy::MultiReferee::gameOver();
    }
};

} // namespace

int main(int argc, char** argv) {
#ifdef HYPERSONIC_HINTS
    loadHints();
#endif
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
