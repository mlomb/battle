// Xmas Rush (CodinGame, 2018-12) - headless C++ referee, port of
// https://github.com/CodinGameCommunity/XmasRush (commit df8eb80, 2019-04-26, pom artifactId Xmas-Rush),
// CodinGame game engine 2.15, 2 players, the rules of the highest league level (the jar's command line
// interface sets league.level 19: the referee's default branch, 12 quest cards per player with 3
// visible at a time, every item on any tile, the tile set TILE_PATTERNS[3]). Deterministic; reproduces
// the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints "<p0score>\n<p1score>\nseed=<N>\n" to stdout like the jar, or "null" per player (exit code 0)
// when the Java referee would have failed (see below). Exit code 1 without output when the jar's command
// line interface fails (bad -seed, a bot that cannot be started).
// -l writes the trace of common/engine.h (one line per bot execution, game summary and tooltip block,
// then "END OK|<why the game failed>"; the view frames are counted, not built, see below).
//
// Leagues: the referee's switch on getLeagueLevel() has 1 (one quest card), 2 (6 cards, items only on
// 3- and 4-way tiles), 3 (12 cards, 3 visible, TILE_PATTERNS[1]), 4-5 (TILE_PATTERNS[2]) and the
// default (TILE_PATTERNS[3]), which is what the jar plays (league.level 19) and all this port
// implements. The game's config has the statements of 3 leagues only (level1-3), so on CodinGame the
// Bronze+ leagues may have played the level 3 tile set: the levels differ only in those parameters.
//
// The game: a 7x7 board of tiles with paths; turns alternate PUSH (each player slides the tile in its
// hand into a row or column, getting the tile that falls out) and MOVE (up to 20 steps along connected
// paths, or PASS), both players at once, 150 turns at most. A player scores 1 for each visible quest
// card whose item it stands on (cards are flipped up to 3 visible); 12 wins; 10 PUSH turns in a row
// where both players pushed the same line end the game ("deadlock").
//
// Engine 2.15 (common/engine.h, cg::Options::engine2): its runner is 3.4.5's (= 3.13.0's split of the
// bots' outputs: an empty line gives the referee no line at all, and the referee's outputs.get(0) then
// fails the game), its core is 3.4.5's except that a turn in which no player is executed makes no frame
// (the deadlock turn) and that the turn numbers start at 0 (unused by this referee). The jar is built
// with referees/cg-builder's CommandLineInterface-v2.java (2.15's runner has no setLeagueLevel and no
// addAgent(cmd, nickname)). The bots get 1000 ms on their first turn and 50 ms after.
//
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption, texts.
// Notable behaviours reproduced here:
//   - new java.util.Random(seed): the center tile (1111, 1010 or 0101), the tiles of the cells on and
//     above the anti-diagonal (a pattern taken out of the league's list, then 0-2 quarter turns; the
//     cell opposite each one gets the same pattern turned 180 degrees; the anti-diagonal's cells are
//     redone by their opposites, which changes nothing), the last pattern of the list for both players'
//     tiles (player 1's turned 180 degrees), the referee's biased shuffle of the 12 item names (nextInt(12)
//     at every step: the same card order for both players, flipped from the end), the items placed on
//     random cells (rejection sampling: not the center or a base, not a tile with an item; each item's
//     twin on the opposite cell), then Collections.shuffle of the tiles with items (the order of the item
//     lines, kept as items are taken).
//   - The view's createPlayersView flips the first 3 cards of each player during init.
//   - The commands: the Java regexes with matches() (exact text: "PUSH <id> <dir>" with id in [ 0-6],
//     "MOVE" and 1 to 20 " <dir>", "PASS"), the 124-char limit first, then the push, move, pass and
//     "max 20 steps" patterns; "PUSH   UP" (a space as the id) passes the regex and crashes the
//     referee in Integer.parseInt, an empty line crashes it in outputs.get(0): the game fails, null
//     scores. Any invalid command (or a command of the wrong turn type, or a timeout) deactivates the
//     player and ends the game: the deactivated player gets -1 (both -1 when both are deactivated in
//     the same turn, or by the two forceGameEnd calls), but the actions already read that turn are
//     still played: a push or the first step of a move can still complete quests, even for a
//     deactivated player (from -1 to 0).
//   - The turn: rows are pushed in a first frame and columns in a second one; two pushes of the same
//     line (whatever the directions) do nothing and count as a stale turn; players on a pushed line move
//     with it and wrap around; moves go one step per frame for both players, a blocked step ending that
//     player's move; quests are checked after each frame, and right after the inputs are sent (a flipped
//     card's item under the player); the game ends at the start of a turn (12 quests, 150 turns: "Max
//     turns reached!", 10 stale pushes) with an animation frame (an execution of the first active player
//     for 0 lines) except for the deadlock.
//   - Engine (common/engine.h): the protocol between the runner and the referee (java.util.Scanner
//     lines: U+0085, U+2028 or U+2029 in a bot line makes the game fail), the game summary quotas, the
//     runner's 10000 bytes per bot output.
// Not reproduced:
//   - the order of the game summary lines of simultaneous pushes and moves: the referee iterates a
//     HashMap<Player, ...> (identity hash codes: it varies from run to run in the jar), here player 0
//     first. The game itself does not depend on it (the pushes of one frame are on distinct parallel
//     lines, the steps of one frame are independent).
//   - the view (GraphicEntityModule 2.15 entities, the game's TooltipModule, EndScreenModule and
//     NicknamesHandlerModule): it cannot reach the engine's limits. The first frame is 28.6 KB against
//     the runner's 100000 bytes in its first round, the others at most ~1.3 KB against 30000; a game has
//     at most 1802 frames (1651 turns: 75 MOVE turns of 20 one-step frames, 75 PUSH turns of 2 frames,
//     the end; each turn's first frame shared by both executions), and two bots maximising it (no quest
//     ever taken, so the 24 card views commit every frame; 20 steps every MOVE turn; a row and a column
//     every PUSH turn) make ~890 KB of view data, against 1 MB. So the -l trace has the frame count only.
//   - the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps, so a bot answering
//     in ~50 ms may be timed out by one and not the other; on a loaded machine the jar times out bots
//     that answered in time).
// Validated (2026-10-25) with battle referee-diff against the jar on 3072 games (32 bots, every ordered
// pairing including self-play, seeds from 1, 1000001 and 987654321: all identical, no mismatch, no flake),
// in direct mode (jar game log vs -l trace: every execution, frame count, summaries, tooltips, scores) on
// 365 games, including raw line endings, invalid UTF-8, U+2028 / U+0085 / U+2029, outputs at the runner's
// 10000-byte limit, the 1802-frame games of the largest view data, seeds 5000000000+, -7 and 2^63-1,
// -timeout, and 3- and 4-player games (the jar fails in init: null scores, like here).
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/engine.h"

#include <deque>

using java::String;

static String A(const char* s) { return java::ascii(s); }
static String I(i64 v) { return java::str(v); }

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------
static const int MAP_SIZE = 7, MAP_WIDTH = MAP_SIZE, MAP_HEIGHT = MAP_SIZE;
static const int NUM_PLAYERS = 2, PLAYER_INDEX = 0, OPPONENT_INDEX = 1;
static const int MAX_GAME_TURNS = 150;
static const int MAX_MOVE_STEPS = 20;
static const int MAX_INPUT_LENGTH = 124;
static const int POINTS_PER_ITEM = 1;
// max move step frames per MOVE turn + (1 row frame + 1 push frame) per PUSH turn + 1
static const int MAX_NUM_TURNS = (MAX_MOVE_STEPS + 2) * MAX_GAME_TURNS / 2 + 1;
static const int MAX_STALE_TURNS = 10;

static const char* ITEM_NAMES[12] = {"ARROW", "BOOK", "CANE", "CANDY", "DIAMOND", "FISH", "MASK", "KEY", "POTION", "SCROLL", "SHIELD", "SWORD"};

// TILE_PATTERNS.get(3): the default branch of the league switch (league.level 19)
static const char* TILE_PATTERNS_3[] = {"0110", "0110", "0110", "0110", "0110", "0110", "0110", "0110", "0110", "0110", "1101", "1101",
                                        "1101", "0111", "0111", "1010", "1010", "1010", "1010", "1010", "1010", "1010", "1010", "1010"};
static const int NUM_CARDS_PER_PLAYER = 12;
static const int NUM_VISIBLE_CARDS = 3;
static const bool THREE_WAY_TILES = false;

enum Direction { UP = 0, RIGHT = 1, DOWN = 2, LEFT = 3 };
static const char* DIRECTION_NAMES[4] = {"UP", "RIGHT", "DOWN", "LEFT"};
static const int DIR_X[4] = {0, 1, 0, -1}, DIR_Y[4] = {-1, 0, 1, 0};
static Direction opposite(Direction d) { return (Direction)((d + 2) % 4); }

struct Vec {
    int x = 0, y = 0;
    bool operator==(const Vec& o) const { return x == o.x && y == o.y; }
    String toString() const { return I(x) + u" " + I(y); }
    void add(Direction d) { x += DIR_X[d], y += DIR_Y[d]; }
    void wrap(Direction d) {
        x = (x + DIR_X[d] + MAP_WIDTH) % MAP_WIDTH;
        y = (y + DIR_Y[d] + MAP_HEIGHT) % MAP_HEIGHT;
    }
};
static const Vec PLAYER_POSITIONS[2] = {{0, 0}, {MAP_WIDTH - 1, MAP_HEIGHT - 1}};
static const Vec TILE_MODEL_POSITIONS[2] = {{-1, -1}, {-2, -2}}; // MINUS_ONE, MINUS_TWO

enum ActionType { PUSH = 0, MOVE = 1, PASS = 2, NONE = -1 };
static const char* ACTION_TYPE_NAMES[3] = {"PUSH", "MOVE", "PASS"};

static java::Random random_;

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------
struct Item {
    int name = 0; // index in ITEM_NAMES
    int playerId = 0;
    bool equals(const Item& o) const { return name == o.name && playerId == o.playerId; }
    int opponentId() const { return playerId == PLAYER_INDEX ? OPPONENT_INDEX : PLAYER_INDEX; }
    String nameStr() const { return A(ITEM_NAMES[name]); }
};

// TileModel; tiles live in a pool and are referred to by index (Java object identity)
struct Tile {
    std::string pattern; // up, right, down, left
    int item = -1;       // index in items_, -1: none
    Vec pos;
    int playerId = -1;   // Integer playerId: -1 for null

    bool hasItem() const { return item >= 0; }
    bool hasDirection(Direction d) const { return pattern[d] == '1'; }
    void rotate(int numTimes) {
        int num = numTimes % 4;
        pattern = pattern.substr(4 - num) + pattern.substr(0, 4 - num);
    }
};
static std::vector<Tile> tiles_;
static std::vector<Item> items_;

static int newTile(const std::string& pattern, Vec pos) {
    Tile t;
    t.pattern = pattern;
    t.pos = pos;
    tiles_.push_back(t);
    return (int)tiles_.size() - 1;
}

static String tileToString(int ti) {
    const Tile& t = tiles_[ti];
    if (!t.hasItem()) return String();
    const Item& it = items_[t.item];
    Vec p = t.playerId >= 0 ? TILE_MODEL_POSITIONS[t.playerId] : t.pos;
    return it.nameStr() + u" " + p.toString() + u" " + I(it.playerId);
}
static String opponentTileToString(int ti) {
    const Tile& t = tiles_[ti];
    const Item& it = items_[t.item];
    Vec p = t.playerId >= 0 ? TILE_MODEL_POSITIONS[1 - t.playerId] : t.pos;
    return it.nameStr() + u" " + p.toString() + u" " + I(it.opponentId());
}

struct PlayerModel {
    int id = 0;
    Vec pos;
    std::vector<int> hiddenCards; // a Stack of items: pop() takes the last
    std::vector<int> visibleCards;
    int numVisibleCards = 0;
    int tile = -1;

    void setTile(int t) {
        if (tile >= 0) tiles_[tile].playerId = -1; // removeTile
        tile = t;
        tiles_[t].pos = TILE_MODEL_POSITIONS[id];
        tiles_[t].playerId = id;
    }
    int getNumCards() const { return (int)(hiddenCards.size() + visibleCards.size()); }
    bool removeCard(const Item& item) {
        for (size_t i = 0; i < visibleCards.size(); i++)
            if (item.equals(items_[visibleCards[i]])) {
                visibleCards.erase(visibleCards.begin() + (long)i);
                return true;
            }
        return false;
    }
    void flipCards() {
        if ((int)visibleCards.size() < numVisibleCards) {
            int available = std::min(numVisibleCards - (int)visibleCards.size(), (int)hiddenCards.size());
            for (int i = 0; i < available; i++) {
                if (!hiddenCards.empty()) {
                    int card = hiddenCards.back();
                    hiddenCards.pop_back();
                    visibleCards.push_back(card);
                }
            }
        }
    }
    String playerToString() const { return I(getNumCards()) + u" " + pos.toString() + u" " + A(tiles_[tile].pattern.c_str()); }
};

// ----------------------------------------------------------------------------
// GameBoard
// ----------------------------------------------------------------------------
struct GameBoard {
    const Vec centerTilePos{MAP_WIDTH / 2, MAP_HEIGHT / 2};
    int t[MAP_WIDTH][MAP_HEIGHT]; // [x][y], -1: null
    std::vector<int> tilesWithItems;
    std::vector<std::string>* availablePatterns = nullptr;

    void init(std::vector<std::string>* patterns) {
        for (auto& col : t)
            for (int& v : col) v = -1;
        availablePatterns = patterns;
        static const char* centerTilePatterns[3] = {"1111", "1010", "0101"};
        std::string centerTilePattern = centerTilePatterns[random_.nextInt(3)];
        setTile(centerTilePos, newTile(centerTilePattern, centerTilePos));
        setTile(PLAYER_POSITIONS[PLAYER_INDEX], newTile("0110", PLAYER_POSITIONS[PLAYER_INDEX]));
        fillEmptyTiles();
    }

    void fillEmptyTiles() {
        // the tiles above the secondary diagonal
        for (int y = 0; y < MAP_HEIGHT; y++) {
            for (int x = 0; x < MAP_WIDTH - y; x++) {
                Vec pos{x, y};
                if (getTile(pos) < 0) {
                    std::string pattern = getRandomAvailablePattern();
                    int tile = newTile(pattern, pos);
                    int rotTimes = random_.nextInt(3);
                    tiles_[tile].rotate(rotTimes);
                    setTile(pos, tile);
                }
                Vec oppositePos = getOppositeTilePos(pos);
                int oppositeTile = newTile(tiles_[getTile(pos)].pattern, oppositePos);
                tiles_[oppositeTile].rotate(2);
                setTile(oppositePos, oppositeTile);
            }
        }
    }

    std::string getRandomAvailablePattern() {
        int index = random_.nextInt((i32)availablePatterns->size());
        std::string pattern = (*availablePatterns)[index];
        availablePatterns->erase(availablePatterns->begin() + index);
        return pattern;
    }

    static Vec getOppositeTilePos(Vec pos) { return Vec{MAP_WIDTH - pos.x - 1, MAP_HEIGHT - pos.y - 1}; }

    int getRandomEmptyTile(bool threeWayTiles) {
        // the recursion of the Java code, as a loop
        for (;;) {
            int index = random_.nextInt(MAP_WIDTH * MAP_HEIGHT);
            Vec pos{index / MAP_WIDTH, index % MAP_HEIGHT};
            const Tile& tile = tiles_[getTile(pos)];
            if (isCenterOrPlayerBase(pos) || tile.hasItem() || (!isThreeWayPlus(tile) && threeWayTiles)) continue;
            return getTile(pos);
        }
    }
    static bool isThreeWayPlus(const Tile& tile) { return std::count(tile.pattern.begin(), tile.pattern.end(), '1') >= 3; }

    void placeItems(const std::vector<std::vector<int>>& itemList, bool threeWayTiles) {
        int numItems = (int)itemList[0].size();
        for (int i = 0; i < numItems; i++) {
            int playerTile = getRandomEmptyTile(threeWayTiles);
            int opponentTile = getTile(getOppositeTilePos(tiles_[playerTile].pos));
            tiles_[playerTile].item = itemList[PLAYER_INDEX][i];
            tiles_[opponentTile].item = itemList[OPPONENT_INDEX][i];
            tilesWithItems.push_back(playerTile);
            tilesWithItems.push_back(opponentTile);
        }
        java::shuffle(tilesWithItems, random_);
    }

    void removeItem(int tile) {
        tiles_[tile].item = -1;
        auto it = std::find(tilesWithItems.begin(), tilesWithItems.end(), tile);
        if (it != tilesWithItems.end()) tilesWithItems.erase(it);
    }

    void setTile(Vec pos, int tile) { t[pos.x][pos.y] = tile; }
    static bool isValidPos(Vec pos) { return pos.x >= 0 && pos.y >= 0 && pos.x < MAP_WIDTH && pos.y < MAP_HEIGHT; }
    int getTile(Vec pos) const { return isValidPos(pos) ? t[pos.x][pos.y] : -1; }

    void moveTile(int tile, Direction d) { tiles_[tile].pos.add(d); }
    void moveTile(int tile, Vec pos) { tiles_[tile].pos = pos; }

    // pushLine: returns the popped tile
    int pushLine(int pushedTile, int lineId, Direction dir) {
        int popped;
        if (dir == UP) popped = pushUp(pushedTile, lineId);
        else if (dir == RIGHT) popped = pushRight(pushedTile, lineId);
        else if (dir == DOWN) popped = pushDown(pushedTile, lineId);
        else popped = pushLeft(pushedTile, lineId);
        return popped;
    }
    int pushUp(int pushedTile, int col) {
        int maxRow = MAP_HEIGHT - 1;
        int poppedTile = t[col][0];
        for (int i = 0; i < maxRow; i++) {
            t[col][i] = t[col][i + 1];
            moveTile(t[col][i], UP);
        }
        t[col][maxRow] = pushedTile;
        moveTile(pushedTile, Vec{col, maxRow});
        return poppedTile;
    }
    int pushRight(int pushedTile, int row) {
        int maxCol = MAP_WIDTH - 1;
        int poppedTile = t[maxCol][row];
        for (int i = maxCol; i > 0; i--) {
            t[i][row] = t[i - 1][row];
            moveTile(t[i][row], RIGHT);
        }
        t[0][row] = pushedTile;
        moveTile(pushedTile, Vec{0, row});
        return poppedTile;
    }
    int pushDown(int pushedTile, int col) {
        int maxRow = MAP_HEIGHT - 1;
        int poppedTile = t[col][maxRow];
        for (int i = maxRow; i > 0; i--) {
            t[col][i] = t[col][i - 1];
            moveTile(t[col][i], DOWN);
        }
        t[col][0] = pushedTile;
        moveTile(pushedTile, Vec{col, 0});
        return poppedTile;
    }
    int pushLeft(int pushedTile, int row) {
        int maxCol = MAP_WIDTH - 1;
        int poppedTile = t[0][row];
        for (int i = 0; i < maxCol; i++) {
            t[i][row] = t[i + 1][row];
            moveTile(t[i][row], LEFT);
        }
        t[maxCol][row] = pushedTile;
        moveTile(pushedTile, Vec{maxCol, row});
        return poppedTile;
    }

    bool isValidMove(Vec pos, Direction direction) const {
        Vec newPos = pos;
        if (tiles_[getTile(newPos)].hasDirection(direction)) {
            newPos.add(direction);
            if (isValidPos(newPos) && tiles_[getTile(newPos)].hasDirection(opposite(direction))) return true;
        }
        return false;
    }

    bool isCenterOrPlayerBase(Vec pos) const { return pos == centerTilePos || pos == PLAYER_POSITIONS[0] || pos == PLAYER_POSITIONS[1]; }
};

// ----------------------------------------------------------------------------
// Players and actions
// ----------------------------------------------------------------------------
struct Player : cg::Player {
    int expectedOutputLines = 0;
    int getExpectedOutputLines() const override { return expectedOutputLines; }
};
static Player players[4];

struct InvalidAction {
    String message;
    bool fatal = true;
};
struct TimeoutException {}; // AbstractPlayer.TimeoutException

struct Action {
    ActionType type = NONE;
    int line = 0;
    Direction direction = UP;
    std::deque<Direction> steps; // MoveAction

    bool isLegalAction(ActionType turnType) const { return turnType == type || (turnType == MOVE && type == PASS); }
    bool isHorizontal() const { return direction == RIGHT || direction == LEFT; }
};

// one of UP / RIGHT / DOWN / LEFT, the whole of s[from, to)
static int directionAt(const String& s, size_t from, size_t to) {
    for (int d = 0; d < 4; d++) {
        size_t n = strlen(DIRECTION_NAMES[d]);
        if (to - from != n) continue;
        bool same = true;
        for (size_t i = 0; i < n; i++) same = same && s[from + i] == (char16_t)DIRECTION_NAMES[d][i];
        if (same) return d;
    }
    return -1;
}

// the steps of "MOVE( (UP|RIGHT|DOWN|LEFT))+" (the whole string), empty when it does not match
static std::vector<Direction> moveSteps(const String& s) {
    std::vector<Direction> steps;
    if (s.size() < 4 || s.compare(0, 4, u"MOVE") != 0) return {};
    size_t i = 4;
    while (i < s.size()) {
        if (s[i] != u' ') return {};
        size_t j = i + 1;
        while (j < s.size() && s[j] != u' ') j++;
        int d = directionAt(s, i + 1, j);
        if (d < 0) return {};
        steps.push_back((Direction)d);
        i = j;
    }
    return steps;
}

// Referee#parseAction: the java.util.regex patterns of Constants, with matches()
static Action parseAction(const String& action) {
    if ((int)action.size() > MAX_INPUT_LENGTH) throw InvalidAction{A("exceeded number of allowed characters")};
    // (?<pushAction>\bPUSH\b) (?<id>[ 0-6]) (?<direction>(\bUP\b|\bRIGHT\b|\bDOWN\b|\bLEFT\b))
    if (action.size() >= 9 && action.compare(0, 5, u"PUSH ") == 0 && (action[5] == u' ' || (action[5] >= u'0' && action[5] <= u'6')) &&
        action[6] == u' ') {
        int d = directionAt(action, 7, action.size());
        if (d >= 0) {
            // Integer.parseInt(matchPush.group("id")): " " throws NumberFormatException (not caught)
            i32 line;
            String id = action.substr(5, 1);
            if (!java::parseInt(id, line)) throw java::Exception{java::encodeUtf8(java::numberFormatException(id))};
            Action a;
            a.type = PUSH;
            a.line = line;
            a.direction = (Direction)d;
            return a;
        }
    }
    std::vector<Direction> steps = moveSteps(action);
    if (!steps.empty() && (int)steps.size() <= MAX_MOVE_STEPS) {
        Action a;
        a.type = MOVE;
        for (Direction d : steps) a.steps.push_back(d);
        return a;
    }
    if (action == u"PASS") {
        Action a;
        a.type = PASS;
        return a;
    }
    if ((int)steps.size() >= MAX_MOVE_STEPS) throw InvalidAction{A("max ") + I(MAX_MOVE_STEPS) + A(" steps - ") + action};
    throw InvalidAction{action};
}

// ----------------------------------------------------------------------------
// Referee
// ----------------------------------------------------------------------------
struct XmasReferee : cg::Referee {
    ActionType turnType = NONE;
    GameBoard gameBoard;
    std::vector<PlayerModel> models;
    std::vector<std::string> availablePatterns;
    // pushActions: the HashMaps of the rows (0) and of the columns (1), the empty ones removed; a
    // map's entries in player index order (the jar: identity hash order)
    std::vector<std::vector<std::pair<int, Action>>> pushActions;
    std::vector<std::pair<int, Action>> moveActions; // HashMap<Player, MoveAction>, player index order
    bool passActions = false;
    int gameTurnsLeft = 0;
    int numStalePushTurns = 0;

    cg::GameManager& gm = cg::gm;

    void init() override {
        random_ = java::Random(gm.getSeed());
        gameTurnsLeft = MAX_GAME_TURNS;
        gm.setMaxTurns(MAX_NUM_TURNS);
        availablePatterns.assign(std::begin(TILE_PATTERNS_3), std::end(TILE_PATTERNS_3));
        tiles_.clear();
        items_.clear();
        gameBoard.init(&availablePatterns);
        createPlayers();
        // createView: createPlayersView flips each player's first cards
        for (cg::Player* p : gm.getPlayers()) models[p->getIndex()].flipCards();
    }

    void createPlayers() {
        std::vector<std::vector<int>> itemList = getPlayerItems(NUM_CARDS_PER_PLAYER);
        std::vector<int> tileList = getPlayerTiles(availablePatterns.at(0));
        for (cg::Player* p : gm.getActivePlayers()) {
            int index = p->getIndex();
            // new PlayerModel(index): Constants.PLAYER_POSITIONS.get(index), an Arrays.asList of 2
            if (index >= NUM_PLAYERS) throw java::Exception{java::arrayIndexOutOfBoundsException(index, NUM_PLAYERS)};
            PlayerModel m;
            m.id = index;
            m.pos = PLAYER_POSITIONS[index];
            m.numVisibleCards = NUM_VISIBLE_CARDS <= 3 ? NUM_VISIBLE_CARDS : 3;
            m.hiddenCards = itemList[index];
            models.push_back(m);
            models.back().setTile(tileList[index]);
        }
        gameBoard.placeItems(itemList, THREE_WAY_TILES);
    }

    std::vector<std::vector<int>> getPlayerItems(int numCards) {
        std::vector<std::vector<int>> items;
        std::vector<int> shuffled = shuffleItemNames();
        for (int i = 0; i < NUM_PLAYERS; i++) {
            std::vector<int> list;
            for (int k = 0; k < numCards; k++) {
                Item it;
                it.name = shuffled[k];
                it.playerId = i;
                items_.push_back(it);
                list.push_back((int)items_.size() - 1);
            }
            items.push_back(list);
        }
        return items;
    }

    // the referee's own Fisher-Yates (nextInt over the whole list at every step)
    std::vector<int> shuffleItemNames() {
        std::vector<int> s;
        for (int i = 0; i < 12; i++) s.push_back(i);
        for (int i = (int)s.size() - 1; i > 0; i--) {
            int index = random_.nextInt((i32)s.size());
            std::swap(s[index], s[i]);
        }
        return s;
    }

    std::vector<int> getPlayerTiles(const std::string& pattern) {
        std::vector<int> list;
        for (int i = 0; i < NUM_PLAYERS; i++) {
            int tile = newTile(pattern, TILE_MODEL_POSITIONS[i]);
            tiles_[tile].rotate(i * 2);
            list.push_back(tile);
        }
        return list;
    }

    PlayerModel& model(cg::Player* p) { return models[p->getIndex()]; }

    // ---- game turn ----
    void gameTurn(int) override {
        if (hasActions()) {
            forceAnimationFrame();
        } else {
            if (gameTurnsLeft <= 0) {
                forceAnimationFrame();
                gm.addToGameSummary(cg::GameManager::formatErrorMessage(A("Max turns reached!")));
                forceGameEnd();
            }
            if (numStalePushTurns >= MAX_STALE_TURNS) {
                gm.addToGameSummary(cg::GameManager::formatErrorMessage(A("The game ended in a deadlock!")));
                forceGameEnd();
            }
            checkForWinner();
            if (gm.getActivePlayers().empty()) {
                gm.setFrameDuration(1);
                return;
            }
            turnType = turnType == PUSH ? MOVE : PUSH;
            gm.setFrameDuration(turnType == PUSH ? 2000 : 700);
            gameTurnsLeft--;
            forceGameFrame();
            flipCards();
            sendPlayerInputs();
            checkForFinishedItems();
            getPlayerActions();
        }
        if (hasActions()) {
            doPlayerActions();
            checkForFinishedItems();
        }
    }

    void flipCards() {
        for (cg::Player* p : gm.getActivePlayers()) model(p).flipCards();
    }

    void sendPlayerInputs() {
        for (cg::Player* p : gm.getActivePlayers()) {
            p->sendInputLine(I(turnType));
            for (int y = 0; y < MAP_HEIGHT; y++) {
                std::string sb = tiles_[gameBoard.getTile(Vec{0, y})].pattern;
                for (int x = 1; x < MAP_WIDTH; x++) sb += " " + tiles_[gameBoard.getTile(Vec{x, y})].pattern;
                p->sendInputLine(sb);
            }
            int first = p->getIndex(), second = 1 - first;
            for (int i = 0; i < NUM_PLAYERS; i++) p->sendInputLine(models[(i + first) % NUM_PLAYERS].playerToString());
            p->sendInputLine(I((i64)gameBoard.tilesWithItems.size()));
            for (int tile : gameBoard.tilesWithItems) p->sendInputLine(p->getIndex() == PLAYER_INDEX ? tileToString(tile) : opponentTileToString(tile));
            int numQuests = (int)(models[first].visibleCards.size() + models[second].visibleCards.size());
            p->sendInputLine(I(numQuests));
            for (int i = 0; i < NUM_PLAYERS; i++)
                for (int card : models[(i + first) % NUM_PLAYERS].visibleCards) {
                    const Item& it = items_[card];
                    p->sendInputLine(it.nameStr() + u" " + I(p->getIndex() == PLAYER_INDEX ? it.playerId : it.opponentId()));
                }
            p->execute();
        }
    }

    bool hasActions() const { return !pushActions.empty() || !moveActions.empty() || passActions; }

    static void putAction(std::vector<std::pair<int, Action>>& map, int player, const Action& a) {
        for (auto& e : map)
            if (e.first == player) {
                e.second = a;
                return;
            }
        map.push_back({player, a});
        std::sort(map.begin(), map.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    }

    void getPlayerActions() {
        for (cg::Player* p : gm.getActivePlayers()) {
            try {
                if (p->timeout) throw TimeoutException{};
                // outputs.get(0): an empty line gives no line at all (not caught: the game fails)
                if (p->outputs.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
                Action action = parseAction(p->outputs[0]);
                if (action.isLegalAction(turnType)) {
                    if (turnType == PUSH) {
                        if (pushActions.size() < 2) pushActions.resize(pushActions.size() + 2);
                        // 0 - for rows, 1 - for columns
                        putAction(pushActions[action.isHorizontal() ? 0 : 1], p->getIndex(), action);
                    } else {
                        if (action.type != PASS) {
                            putAction(moveActions, p->getIndex(), action);
                        } else {
                            passActions = true;
                            gm.addToGameSummary(p->getNicknameToken() + u" passed");
                        }
                    }
                } else {
                    throw InvalidAction{u"can't \"" + A(ACTION_TYPE_NAMES[action.type]) + u"\" while expecting a " + A(ACTION_TYPE_NAMES[turnType]) + u" action"};
                }
            } catch (const InvalidAction& e) {
                if (e.fatal) {
                    p->deactivate(p->getNicknameToken() + u": invalid input");
                    forceGameEnd();
                }
                gm.addToGameSummary(cg::GameManager::formatErrorMessage(p->getNicknameToken() + u": invalid input - " + e.message));
            } catch (const TimeoutException&) {
                gm.addToGameSummary(cg::GameManager::formatErrorMessage(p->getNicknameToken() + u": timeout - no input provided"));
                p->deactivate(p->getNicknameToken() + u": timeout");
                forceGameEnd();
            }
        }
        std::vector<std::vector<std::pair<int, Action>>> kept;
        for (auto& m : pushActions)
            if (!m.empty()) kept.push_back(m);
        pushActions = kept;
    }

    void doPlayerActions() {
        if (turnType == PUSH) {
            std::vector<std::pair<int, Action>> actions = pushActions.at(0);
            pushActions.erase(pushActions.begin());
            doPushAction(actions);
        } else {
            for (auto& e : moveActions) doMoveAction(e.first, e.second);
            std::vector<std::pair<int, Action>> kept;
            for (auto& e : moveActions)
                if (!e.second.steps.empty()) kept.push_back(e);
            moveActions = kept;
            passActions = false;
        }
    }

    void doMoveAction(int playerIndex, Action& moveAction) {
        cg::Player* player = gm.getPlayers()[playerIndex];
        PlayerModel& pm = models[playerIndex];
        Direction step = moveAction.steps.front(); // pollFirst
        moveAction.steps.pop_front();
        if (gameBoard.isValidMove(pm.pos, step)) {
            pm.pos.add(step);
            gm.addToGameSummary(player->getNicknameToken() + u" moved " + A(DIRECTION_NAMES[step]));
        } else {
            moveAction.steps.clear();
            gm.addToGameSummary(cg::GameManager::formatErrorMessage(player->getNicknameToken() + u": invalid input - MOVE " + A(DIRECTION_NAMES[step])));
        }
    }

    void doPushAction(const std::vector<std::pair<int, Action>>& actions) {
        // PushAction.pushSameLine of the two actions (both rows or both columns)
        if (actions.size() >= 2 && actions[0].second.line == actions[1].second.line) {
            gm.addToGameSummary(cg::GameManager::formatErrorMessage(A("Both players tried to push the same line. Nothing happens!")));
            numStalePushTurns++;
            return;
        }
        numStalePushTurns = 0;
        for (const auto& e : actions) {
            cg::Player* player = gm.getPlayers()[e.first];
            PlayerModel& pm = models[e.first];
            const Action& pushAction = e.second;
            int line = pushAction.line;
            Direction direction = pushAction.direction;
            int poppedTile = gameBoard.pushLine(pm.tile, line, direction);
            pm.setTile(poppedTile);
            updatePlayerPos(pushAction, pushAction.isHorizontal());
            if (pushAction.isHorizontal())
                gm.addToGameSummary(player->getNicknameToken() + u" pushed row " + I(line) + u" to the " + A(DIRECTION_NAMES[direction]));
            else
                gm.addToGameSummary(player->getNicknameToken() + u" pushed column " + I(line) + u" " + A(DIRECTION_NAMES[direction]));
        }
    }

    void updatePlayerPos(const Action& action, bool horizontal) {
        for (PlayerModel& pm : models) {
            int playerLine = horizontal ? pm.pos.y : pm.pos.x;
            if (playerLine == action.line) pm.pos.wrap(action.direction);
        }
    }

    // the first active player is executed for 0 lines (an animation frame)
    void forceAnimationFrame() {
        std::vector<cg::Player*> active = gm.getActivePlayers();
        if (active.empty()) return;
        Player* p = static_cast<Player*>(active[0]);
        p->expectedOutputLines = 0;
        p->execute();
        // getOutputs() in a try/catch: nothing
    }

    void forceGameFrame() {
        for (cg::Player* p : gm.getActivePlayers()) static_cast<Player*>(p)->expectedOutputLines = 1;
    }

    void forceGameEnd() {
        for (cg::Player* p : gm.getPlayers()) {
            if (p->isActive()) p->deactivate();
            else p->setScore(-1);
        }
        gm.endGame();
    }

    void checkForFinishedItems() {
        for (cg::Player* p : gm.getPlayers()) {
            PlayerModel& pm = models[p->getIndex()];
            int tile = gameBoard.getTile(pm.pos);
            if (tiles_[tile].hasItem() && pm.removeCard(items_[tiles_[tile].item])) {
                p->setScore(p->getScore() + POINTS_PER_ITEM);
                gm.addToGameSummary(cg::GameManager::formatSuccessMessage(p->getNicknameToken() + u" completed a quest"));
                gm.addTooltip(p->getIndex(), p->getNicknameToken() + u" got the " + items_[tiles_[tile].item].nameStr());
                gameBoard.removeItem(tile);
            }
        }
    }

    void checkForWinner() {
        bool win = false;
        for (cg::Player* p : gm.getActivePlayers()) win = win || p->getScore() == NUM_CARDS_PER_PLAYER;
        if (win) {
            forceAnimationFrame();
            forceGameEnd();
        }
    }

    void onEnd() override {
        // checkOutcome
        const std::vector<cg::Player*>& ps = gm.getPlayers();
        bool draw = true;
        for (cg::Player* p : ps) draw = draw && p->getScore() == ps[0]->getScore();
        if (draw) {
            gm.addToGameSummary(A("It's a draw!"));
        } else {
            cg::Player* best = ps[0]; // Stream.max keeps the first of equal ones
            for (cg::Player* p : ps)
                if (p->getScore() > best->getScore()) best = p;
            gm.addToGameSummary(cg::GameManager::formatSuccessMessage(best->getNicknameToken() + u" won!"));
        }
    }
};

static XmasReferee referee;

int main(int argc, char** argv) {
    cg::Options options;
    options.engine2 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
