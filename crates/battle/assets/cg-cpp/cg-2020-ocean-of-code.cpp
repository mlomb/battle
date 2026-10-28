// Ocean of Code (CodinGame, 2020-04) - headless C++ referee, port of
// https://github.com/CodinGameCommunity/ocean-of-code (commit a84dcfb, 2020-04-18), CodinGame game engine
// 3.13.0, 2 players, league 19 (the CLI sets the league to 19: MOVE, SURFACE, TORPEDO, SILENCE, SONAR,
// MINE and TRIGGER).
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints "<p0score>\n<p1score>\nseed=<N>\n" to stdout like the jar, or "null\nnull\n" (exit code 0)
// when the Java referee would have failed (see below). Exit code 1 without output when the jar's
// command line interface fails (bad -seed, a bot that cannot be started, more than 2 players).
// -l writes the trace of common/engine.h (one line per bot execution, view frame, game summary and
// tooltip block, then "END OK|<why the game failed>").
//
// Engine 3.13.0 (common/engine.h, cg::Options::engine313): its core is 3.15.0's, which is what engine.h
// models by default (4.0.2-4.5.0 without getRandom(); the game makes its own java.util.Random from the
// seed), but its runner sends the referee every line of a bot's output split with String#split (trailing
// empty lines removed): a bot answering an empty line gives the referee no line at all, and the
// referee's getOutputs().get(0) then throws ("provided invalid input"). The jar is built with
// referees/cg-builder's CommandLineInterface-v3.java (3.13.0's runner returns its older GameResult).
//
// The Java referee is followed closely: same data structures, iteration orders, RNG consumption, int
// semantics, error texts. Notable Java behaviours reproduced here:
//   - new java.util.Random(seed): 5 + nextInt(26) islands, the bound drawn again at every iteration of
//     the loop, each island grown to the cells above and to the right, then the tile check that fills
//     the 8 neighbours of an island cell no sprite fits, until every island cell fits one.
//   - One engine turn per action: a player's line is split on '|' (String#split, trimmed parts, empty
//     ones skipped, the same command twice disqualifies except MSG), MSG parts executed first, then
//     each valid action executed in its own turn (the others get "frames" without any execution), the
//     invalid ones dropped as the Java code drops them (canExecute in the order of the actions, with
//     its game summary errors and its disqualifying GameExceptions, after the actions before it ran),
//     the SURFACE fallback, the 598 rounds and the engine's 1000 turns.
//   - The checks and texts of every command (String#split(" "), Integer#parseInt with any Unicode
//     digit, the TORPEDO range by BFS through the islands, mines active from the owner's next round,
//     explosions hurting both submarines), the exceptions and the scores: the lives, -1 for a
//     disqualified player (timeout, GameException message, "provided invalid input" for anything else).
//   - The view data: the jar fails when one frame passes the runner's 30000 bytes or when the total
//     passes 1 MB (both reachable: every action and message of a bot goes into the view as text), so
//     it is rebuilt here: the GraphicEntityModule of engine 3.13.0, its TooltipModule (sends "[{...}]"
//     every frame) and EndScreenModule (common/gem.h), and the view code with its quirks (an explosion
//     drawn again on the game-over frame, the partial view of a game whose first turn disqualified a
//     player, ...).
//   - Engine (common/engine.h): the protocol between the runner and the referee (java.util.Scanner
//     lines, 30000-byte commands: e.g. a tooltip with a huge TORPEDO command makes the game fail), the
//     game summary quotas; the bots get 1000 ms on their first turn and 50 ms after.
// Not reproduced: the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps, so a
// bot answering in ~50 ms may be timed out by one and not the other; on a loaded machine the jar times
// out bots that answered in time). The order of the entities in a frame and of the ids in a group's
// children (identity hash codes in the jar: only their size matters). The ids given to the island
// sprites: GridModel's HashMap<java.awt.Point, String> has crowded, treeified buckets where two islands
// with the same Point hash code ((1,1) and (4,4), (1,4) and (4,1)) are ordered by identity hash codes;
// the frame sizes do not depend on that order.
// Validated (2026-10-25) with battle referee-diff against the jar on 5892 games (32 bots, every ordered
// pairing, seeds from 1, 7, 1000001, 987654321, 3000000007 and 5000000000; the 6 mismatches were the
// loaded jar or candidate timing out a bot that had answered, identical when replayed alone) and in
// direct mode (jar game log vs -l trace: every execution, every view frame's size and text, summaries,
// tooltips) on 306 games, including raw line endings, invalid UTF-8, U+2028 / U+0085, empty lines, view
// frames at the runner's 30000-byte limit, the 1 MB view data quota and the engine's 1000 turns.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/engine.h"
#include "common/gem.h"

#include <cmath>
#include <map>
#include <memory>
#include <numeric>

using java::String;

static String A(const char* s) { return java::ascii(s); }
static String I(i64 v) { return java::str(v); }

static String join(const std::vector<String>& parts, const char16_t* sep) {
    String r;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) r += sep;
        r += parts[i];
    }
    return r;
}

// the engine 3.13.0 modules (GraphicEntityModule, TooltipModule, EndScreenModule)
static gem::Module gem_;
static gem::TooltipModule tooltips_(true);
static gem::EndScreenModule endScreen_;

// ----------------------------------------------------------------------------
// The model: Point, Direction, GridModel, Mine, PlayerAction, PlayerModel, commands, Game
// ----------------------------------------------------------------------------
struct GameException {
    String message;
};
struct TimeoutException {}; // AbstractPlayer.TimeoutException

static java::Exception npe() { return java::Exception{"java.lang.NullPointerException"}; }

struct Point {
    int x = 0, y = 0;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
    // Point2D#hashCode
    i32 hashCode() const {
        double dx = x, dy = y;
        u64 bx, by;
        memcpy(&bx, &dx, 8);
        memcpy(&by, &dy, 8);
        u64 bits = bx ^ (by * 31);
        return (i32)(u32)bits ^ (i32)(u32)(bits >> 32);
    }
};

static const int GRID_SIZE = 15;

struct Direction {
    int dx, dy;
    const char* name;
};
static const Direction DIRECTIONS[4] = {{-1, 0, "W"}, {1, 0, "E"}, {0, -1, "N"}, {0, 1, "S"}};

static const Direction& getDir(const String& dir) {
    for (const Direction& d : DIRECTIONS)
        if (dir == A(d.name)) return d;
    throw GameException{u"Invalid direction: " + dir + u". Legal values are: N, S, E or W."};
}

struct GridModel {
    bool grid[GRID_SIZE][GRID_SIZE] = {};
    std::vector<std::pair<Point, String>> tiles; // the HashMap<Point, String>, in its iteration order

    bool isEmpty(const Point& p) const { return p.x >= 0 && p.x < GRID_SIZE && p.y >= 0 && p.y < GRID_SIZE && !grid[p.x][p.y]; }

    void initialize(java::Random& rand) {
        bool result[GRID_SIZE][GRID_SIZE] = {};
        // create random islands (the bound is drawn again at each iteration)
        for (int i = 0; i < rand.nextInt(26) + 5; i++) {
            int x = rand.nextInt(15);
            int y = rand.nextInt(15);
            result[x][y] = true;
        }
        memcpy(grid, result, sizeof grid);
        // increase the island tiles
        for (int x = 0; x < GRID_SIZE; x++)
            for (int y = 0; y < GRID_SIZE; y++)
                if (result[x][y]) {
                    if (y - 1 >= 0) grid[x][y - 1] = true;
                    if (x + 1 < GRID_SIZE) {
                        grid[x + 1][y] = true;
                        if (y - 1 >= 0) grid[x + 1][y - 1] = true;
                    }
                }
        while (!checkError()) {
        }
    }

    // checkError: false (null) after fixing the first island cell no tile fits
    bool checkError() {
        static const char* const TILES[13] = {"11.0.111", ".11111.0", ".0.0.111", "11.0.0.1", ".111.0.0", ".0.111.0", "1111.0.1",
                                              ".0.11111", "11111111", "11111101", "11110111", "11011111", "01111111"};
        std::vector<std::pair<Point, String>> sprites; // put order
        for (int y = 0; y < GRID_SIZE; y++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                if (!grid[x][y]) continue;
                char map[9];
                map[0] = y - 1 < 0 || x - 1 < 0 || grid[x - 1][y - 1] ? '1' : '0';
                map[1] = y - 1 < 0 || grid[x][y - 1] ? '1' : '0';
                map[2] = y - 1 < 0 || x + 1 >= GRID_SIZE || grid[x + 1][y - 1] ? '1' : '0';
                map[3] = x + 1 >= GRID_SIZE || grid[x + 1][y] ? '1' : '0';
                map[4] = y + 1 >= GRID_SIZE || x + 1 >= GRID_SIZE || grid[x + 1][y + 1] ? '1' : '0';
                map[5] = y + 1 >= GRID_SIZE || grid[x][y + 1] ? '1' : '0';
                map[6] = y + 1 >= GRID_SIZE || x - 1 < 0 || grid[x - 1][y + 1] ? '1' : '0';
                map[7] = x - 1 < 0 || grid[x - 1][y] ? '1' : '0';
                map[8] = 0;
                int found = -1;
                for (int i = 0; i < 13 && found < 0; i++) { // map.matches(tile): '.' is any char
                    bool ok = true;
                    for (int k = 0; k < 8; k++)
                        if (TILES[i][k] != '.' && TILES[i][k] != map[k]) ok = false;
                    if (ok) found = i;
                }
                if (found < 0) {
                    // fix error
                    grid[std::max(x - 1, 0)][std::max(0, y - 1)] = true;
                    grid[x][std::max(0, y - 1)] = true;
                    grid[std::min(x + 1, GRID_SIZE - 1)][std::max(0, y - 1)] = true;
                    grid[std::max(x - 1, 0)][std::max(0, y)] = true;
                    grid[std::min(x + 1, GRID_SIZE - 1)][std::max(0, y)] = true;
                    grid[std::max(x - 1, 0)][std::min(y + 1, GRID_SIZE - 1)] = true;
                    grid[x][std::min(y + 1, GRID_SIZE - 1)] = true;
                    grid[std::min(x + 1, GRID_SIZE - 1)][std::min(y + 1, GRID_SIZE - 1)] = true;
                    return false;
                }
                std::string name = TILES[found];
                std::replace(name.begin(), name.end(), '.', 'x');
                sprites.push_back({Point{x, y}, A(("island/" + name + ".png").c_str())});
            }
        }
        // HashMap<Point, String> iteration order
        std::vector<i32> hashes;
        for (auto& s : sprites) hashes.push_back(s.first.hashCode());
        tiles.clear();
        for (int k : java::hashMapOrder(hashes)) tiles.push_back(sprites[k]);
        return true;
    }
};

struct Mine {
    Point point;
    bool isActive = false, isBlown = false;
};

struct PlayerAction {
    String activator;
    String command;
};

enum CommandType { MOVE, SURFACE, TORPEDO, SILENCE, SONAR, MINE, TRIGGER };
static const char* const COMMAND_NAME[7] = {"MOVE", "SURFACE", "TORPEDO", "SILENCE", "SONAR", "MINE", "TRIGGER"};

struct Game;

struct PlayerModel {
    std::vector<PlayerAction> actions;
    std::vector<int> powers;   // TORPEDO, SILENCE, SONAR, MINE (the IPower objects)
    std::vector<int> commands; // MOVE, SURFACE, then the powers, TRIGGER
    int charges[7] = {};       // by command type, for the powers
    Game* game = nullptr;
    GridModel* gridModel = nullptr;
    int life = 6;
    int index = 0;
    bool hasPosition = false; // position == null before initialize
    Point position;
    std::vector<std::shared_ptr<Mine>> mines;
    String message;
    bool grid[GRID_SIZE][GRID_SIZE] = {};
    std::vector<String> summaries;
    String sonarResult = u"NA";
    bool hasLastCommand = false;
    PlayerAction lastCommand;
    String actionMessage;

    static int maxValue(int power) { return power == TORPEDO ? 3 : power == SILENCE ? 6 : power == SONAR ? 4 : 3; }
    static int powerIndex(int power) { return power == TORPEDO ? 0 : power == SONAR ? 1 : power == SILENCE ? 2 : 3; }

    // Math.ceil((x + 1) / 5.0) + Math.floor(y / 5.0) * 3
    int getSector() const { return (int)(std::ceil((position.x + 1) / 5.0) + std::floor(position.y / 5.0) * 3); }

    PlayerModel& getOpponent();
    void initialize(const String& output, int league);
    bool hasMoreActions() const { return !actions.empty(); }
    void handleRoundInput(const String& input);
    void performRoundAction();
    bool hasValidCommandAndRemoveInvalid();
    void surface();
    void reduceLife(int amount);
    void move(const Direction& d);
    String getCooldowns() const {
        return I(getCooldown(TORPEDO)) + u" " + I(getCooldown(SONAR)) + u" " + I(getCooldown(SILENCE)) + u" " + I(getCooldown(MINE));
    }
    int getCooldown(int power) const {
        for (int p : powers)
            if (p == power) return maxValue(p) - charges[p];
        return -1;
    }
    bool hasPower(const String& name) const {
        for (int p : powers)
            if (name == A(COMMAND_NAME[p])) return true;
        return false;
    }
    void chargePower(const String& name) {
        for (int p : powers)
            if (name == A(COMMAND_NAME[p])) {
                if (maxValue(p) > charges[p]) charges[p] = std::min(maxValue(p), charges[p] + 1); // increaseCharges
                return;
            }
    }
    void updateMines() {
        for (int i = (int)mines.size() - 1; i >= 0; i--)
            if (mines[i]->isBlown) mines.erase(mines.begin() + i);
        for (auto& m : mines) m->isActive = true;
    }
    String getSummaries() const { return summaries.empty() ? String(u"NA") : join(summaries, u"|"); }

    // the commands (ICommand#canExecute / #execute)
    bool canExecute(int type, const PlayerAction& action);
    void execute(int type, const PlayerAction& action);
    void messageExecute(const PlayerAction& action) {
        if (action.command.size() < 5) return;
        message = action.command.substr(4);
    }
};

struct Referee;

struct Game {
    PlayerModel players[2];
    GridModel gridModel;
    PlayerModel* activePlayer = nullptr;
    int turn = 0;
    static const int maxTurns = 598;
    Referee* playerManager = nullptr;

    void init(Referee* manager, java::Random& rand) {
        playerManager = manager;
        for (int i = 0; i < 2; i++) {
            players[i] = PlayerModel();
            players[i].index = i;
            players[i].game = this;
            players[i].gridModel = &gridModel;
        }
        gridModel.initialize(rand);
    }

    void initialize(int league);
    void onRound();
    void handleNewRound(PlayerModel& p);
    std::vector<String> getRoundInput(PlayerModel& p);
    std::vector<String> getInitialInput(PlayerModel& p);
    void initializePlayer(PlayerModel& p, int league);
    void addTooltip(PlayerModel& p, const String& tooltip);
    void addError(PlayerModel& p, const String& error);
    void updateScore();
    bool isGameOver() const { return players[0].life <= 0 || players[1].life <= 0 || turn > maxTurns; }
    void explode(const Point& target) {
        for (PlayerModel& p : players) {
            if (p.position == target) {
                p.reduceLife(2);
                continue;
            }
            if (std::abs(p.position.x - target.x) <= 1 && std::abs(p.position.y - target.y) <= 1) p.reduceLife(1);
        }
    }
};

PlayerModel& PlayerModel::getOpponent() { return game->players[1 - index]; }

// BFS.getPathBFS(gridModel.grid, src, dst).getPathLength(), -1 for null
static int pathLength(const GridModel& gm, const Point& src, const Point& dst) {
    auto isFree = [&](int x, int y) { return x >= 0 && x < GRID_SIZE && y >= 0 && y < GRID_SIZE && !gm.grid[x][y]; };
    if (!isFree(dst.x, dst.y)) return -1;
    bool seen[GRID_SIZE][GRID_SIZE];
    memcpy(seen, gm.grid, sizeof seen);
    int dist[GRID_SIZE][GRID_SIZE];
    seen[src.x][src.y] = true;
    dist[src.x][src.y] = 0;
    std::vector<Point> q{src};
    for (size_t i = 0; i < q.size(); i++) {
        Point p = q[i];
        if (p == dst) return dist[p.x][p.y];
        static const int DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int nx = p.x + DX[k], ny = p.y + DY[k];
            if (isFree(nx, ny) && !seen[nx][ny]) {
                seen[nx][ny] = true;
                dist[nx][ny] = dist[p.x][p.y] + 1;
                q.push_back(Point{nx, ny});
            }
        }
    }
    return -1;
}

// ----------------------------------------------------------------------------
// Player.java and Referee.java (IPlayerManager)
// ----------------------------------------------------------------------------
struct Player : cg::Player {
    int getExpectedOutputLines() const override { return 1; }
};
static Player players[2];

static Game game;

struct Referee : cg::Referee {
    java::Random random;

    void init() override;
    void gameTurn(int turn) override;
    void onEnd() override;
    void onGameInit() override {
        gem_.sendGlobalData();
        gem_.sendFrameData();
        tooltips_.sendFrameData();
    }
    void onAfterGameTurn() override {
        gem_.sendFrameData();
        tooltips_.sendFrameData();
    }
    void onAfterOnEnd() override { endScreen_.onAfterOnEnd(); }

    int getLeague() const { return cg::gm.getLeagueLevel(); }

    // IPlayerManager
    String getOutputs(int playerId) {
        Player& p = players[playerId];
        if (p.timeout) throw TimeoutException{};
        if (p.outputs.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)}; // getOutputs().get(0)
        return p.outputs[0];
    }
    void sendData(const std::vector<String>& lines, int playerId) {
        for (const String& line : lines) players[playerId].sendInputLine(line);
    }
    void execute(int playerId) { players[playerId].execute(); }
    void updateScore(int playerId, int score) { players[playerId].setScore(score); }
    void disablePlayer(int playerId, const String& reason) {
        players[playerId].deactivate(reason);
        players[playerId].setScore(-1);
        cg::gm.endGame();
    }
    void endGame() { cg::gm.endGame(); }
    void addTooltip(int playerId, const String& message) { cg::gm.addTooltip(playerId, message); }
    void addGameSummary(int, const String& message) { cg::gm.addToGameSummary(message); }

    void addFrame(int x, int y, int width, int height, int z) {
        gem::Module& m = gem_;
        gem::Entity* r = m.createRectangle();
        m.setLineColor(r, 0xefefef);
        m.setLineWidth(r, 10);
        m.setZIndex(r, z);
        m.setX(r, x);
        m.setY(r, y);
        m.setWidth(r, width);
        m.setHeight(r, height);
        m.setFillAlpha(r, 0);
    }
};
static Referee referee;

void Game::addTooltip(PlayerModel& p, const String& tooltip) { playerManager->addTooltip(p.index, tooltip); }
void Game::addError(PlayerModel& p, const String& error) { playerManager->addGameSummary(p.index, error); }
void Game::updateScore() {
    playerManager->updateScore(0, players[0].life);
    playerManager->updateScore(1, players[1].life);
}

// ----------------------------------------------------------------------------
// PlayerModel.java, PlayerAction.java and the commands
// ----------------------------------------------------------------------------
void PlayerModel::initialize(const String& output, int league) {
    // output.matches("^[0-9]{1,2} [0-9]{1,2}$")
    size_t sp = output.find(u' ');
    auto digits = [&](size_t a, size_t b) {
        if (b - a < 1 || b - a > 2) return false;
        for (size_t i = a; i < b; i++)
            if (!java::isRegexDigit(output[i])) return false;
        return true;
    };
    if (sp == String::npos || !digits(0, sp) || !digits(sp + 1, output.size()))
        throw GameException{u"Cannot read initial coordinates. Expected format: 'X Y'"};
    i32 x = 0, y = 0;
    java::parseInt(output.substr(0, sp), x);
    java::parseInt(output.substr(sp + 1), y);
    position = Point{x, y};
    hasPosition = true;
    if (!gridModel->isEmpty(position)) throw GameException{u"Invalid coordinates!"};
    grid[position.x][position.y] = true;

    commands.push_back(MOVE);
    commands.push_back(SURFACE);
    commands.push_back(TORPEDO);
    powers.push_back(TORPEDO);
    if (league >= 2) {
        commands.push_back(SILENCE);
        powers.push_back(SILENCE);
        commands.push_back(SONAR);
        powers.push_back(SONAR);
    }
    if (league >= 3) {
        commands.push_back(MINE);
        powers.push_back(MINE);
        commands.push_back(TRIGGER);
    }
}

// PlayerAction.cleanActions
static std::vector<PlayerAction> cleanActions(const String& command) {
    std::vector<String> commandStr = java::split(command, u'|');
    std::vector<String> keys; // the HashSet<String> (only membership matters)
    std::vector<PlayerAction> actions;
    for (const String& part : commandStr) {
        String cStr = java::trim(part);
        if (cStr.empty()) continue;
        size_t activatorSize = cStr.find(u' ');
        if (activatorSize == String::npos) activatorSize = cStr.size();
        String activator = cStr.substr(0, activatorSize);
        if (std::find(keys.begin(), keys.end(), activator) != keys.end()) {
            if (activator != u"MSG") throw GameException{u"Two equal commands executed."};
        } else {
            keys.push_back(activator);
        }
        actions.push_back(PlayerAction{activator, cStr});
    }
    return actions;
}

void PlayerModel::handleRoundInput(const String& input) {
    updateMines();
    hasLastCommand = false;
    sonarResult = u"NA";
    summaries.clear();
    actions = cleanActions(input);
    std::vector<String> actionMessages;
    for (const PlayerAction& action : actions) {
        if (action.activator == u"MSG") messageExecute(action);
        else actionMessages.push_back(action.command);
    }
    actionMessage = join(actionMessages, u" > ");

    if (!hasValidCommandAndRemoveInvalid()) actions.push_back(PlayerAction{u"SURFACE", u"SURFACE"});

    performRoundAction();
}

void PlayerModel::performRoundAction() {
    PlayerAction action = actions[0];
    lastCommand = action;
    hasLastCommand = true;
    actions.erase(actions.begin());
    for (int c : commands) {
        if (action.activator != A(COMMAND_NAME[c])) continue;
        execute(c, action);
        break;
    }
    hasValidCommandAndRemoveInvalid(); // clear trailing invalid commands on the right player if at the end.
}

bool PlayerModel::hasValidCommandAndRemoveInvalid() {
    while (!actions.empty()) {
        PlayerAction action = actions[0];
        bool removed = false;
        for (int c : commands) {
            if (action.activator != A(COMMAND_NAME[c])) continue;
            if (!canExecute(c, action)) {
                actions.erase(actions.begin());
                removed = true;
                break;
            } else {
                return true;
            }
        }
        if (action.activator == u"MSG") {
            messageExecute(action);
            actions.erase(actions.begin());
        } else if (!removed) { // actions.contains(action)
            throw GameException{u"Unknown command: " + action.command};
        }
    }
    return false;
}

void PlayerModel::surface() {
    memset(grid, 0, sizeof grid);
    grid[position.x][position.y] = true;
    reduceLife(1);
}

void PlayerModel::reduceLife(int amount) {
    life -= amount;
    if (life < 0) life = 0;
    if (amount > 1) game->addTooltip(*this, u" lost " + I(amount) + u" lives");
    else game->addTooltip(*this, u" lost a life");
}

void PlayerModel::move(const Direction& d) {
    int x = position.x + d.dx;
    int y = position.y + d.dy;
    if (!gridModel->isEmpty(Point{x, y})) throw GameException{u"Invalid move. Tried to move outside map or onto island"};
    if (grid[x][y]) throw GameException{u"Can't visit same location twice. Use surface."};
    position = Point{x, y};
    grid[x][y] = true;
}

// --- MoveCommand, SilencePower, MinePower: getDirection / getTarget (a GameException keeps its message,
// anything else becomes "Invalid ... command: <command>")
static const Direction& directionOf(const std::vector<String>& commands, const String& command, const char16_t* what) {
    if (commands.size() < 2) throw GameException{String(what) + command};
    return getDir(commands[1]);
}

// MoveCommand#getPower
static String getPower(PlayerModel& pm, const String& command) {
    std::vector<String> commands = java::split(command, u' ');
    if (commands.size() != 3) return String();
    String power = java::trim(commands[2]);
    if (!pm.hasPower(power)) throw GameException{u"Invalid power to charge: " + power};
    return power;
}

// TorpedoPower#getTarget / TriggerCommand#getTarget
static Point getTarget(const String& command, const char16_t* error) {
    std::vector<String> commands = java::split(command, u' ');
    i32 x, y;
    if (commands.size() < 3 || !java::parseInt(commands[1], x) || !java::parseInt(commands[2], y)) throw GameException{error};
    return Point{x, y};
}

// SilencePower#getDistance
static int getDistance(const String& command) {
    std::vector<String> commands = java::split(command, u' ');
    i32 distance;
    if (commands.size() < 3 || !java::parseInt(commands[2], distance) || distance < 0 || distance > 4)
        throw GameException{u"Invalid distance on SILENCE: " + command};
    return distance;
}

// SonarPower#getSector
static int getSonarSector(const String& command) {
    std::vector<String> commands = java::split(command, u' ');
    i32 sector;
    if (commands.size() < 2 || !java::parseInt(commands[1], sector) || sector < 1 || sector > 9)
        throw GameException{u"Invalid sector on SONAR: " + command};
    return sector;
}

// MinePower#getTarget
static Point getMineTarget(PlayerModel& pm, const String& command) {
    std::vector<String> commands = java::split(command, u' ');
    const Direction& dir = directionOf(commands, command, u"Invalid mine command: ");
    return Point{pm.position.x + dir.dx, pm.position.y + dir.dy};
}

bool PlayerModel::canExecute(int type, const PlayerAction& action) {
    const String& command = action.command;
    switch (type) {
        case MOVE: {
            std::vector<String> commands = java::split(command, u' ');
            if (commands.size() > 3) throw GameException{u"Invalid amount of parameters to MOVE. Should be MOVE direction power"};
            directionOf(commands, command, u"Invalid move command: ");
            getPower(*this, command);
            return true;
        }
        case SURFACE: return true;
        case TORPEDO: {
            if (maxValue(TORPEDO) > charges[TORPEDO]) {
                game->addError(*this, u"Not enough charges of TORPEDO");
                return false;
            }
            std::vector<String> commands = java::split(command, u' ');
            if (commands.size() != 3) throw GameException{u"Invalid amount of params for TORPEDO, should be TORPEDO x y"};
            Point target = getTarget(command, u"Invalid params to Torpedo. Should be TORPEDO x y");
            int len = pathLength(*gridModel, position, target);
            if (len < 0 || len > 4) {
                game->addError(*this, u"Tried to fire out of range");
                return false;
            }
            return true;
        }
        case SILENCE: {
            if (maxValue(SILENCE) > charges[SILENCE]) {
                game->addError(*this, u"Not enough charges of SILENCE");
                return false;
            }
            std::vector<String> commands = java::split(command, u' ');
            if (commands.size() != 3) throw GameException{u"Invalid amount of params for Silence, should be: SILENCE direction steps"};
            directionOf(commands, command, u"Invalid move command: ");
            getDistance(command);
            return true;
        }
        case SONAR: {
            if (maxValue(SONAR) > charges[SONAR]) {
                game->addError(*this, u"Not enough charges of SONAR");
                return false;
            }
            std::vector<String> commands = java::split(command, u' ');
            if (commands.size() != 2) throw GameException{u"Invalid amount of params for SONAR, should be: SONAR sectorId"};
            getSonarSector(command);
            return true;
        }
        case MINE: {
            if (maxValue(MINE) > charges[MINE]) {
                game->addError(*this, u"Not enough charges of MINE");
                return false;
            }
            std::vector<String> commands = java::split(command, u' ');
            if (commands.size() > 2) throw GameException{u"Invalid amount of params for MINE, should be MINE direction"};
            Point target = getMineTarget(*this, command);
            if (!gridModel->isEmpty(target)) {
                game->addError(*this, u"Mine target outside map or on obstacle");
                return false;
            }
            for (auto& mine : mines)
                if (!mine->isBlown && mine->point == target) {
                    game->addError(*this, u"Two mines on the same location is not allowed.");
                    return false;
                }
            return true;
        }
        case TRIGGER: {
            std::vector<String> commands = java::split(command, u' ');
            if (commands.size() > 3) throw GameException{u"Invalid amount of params for TRIGGER, should be TRIGGER x y"};
            Point target = getTarget(command, u"Invalid params to Trigger. Should be Trigger x y");
            for (auto& mine : mines) {
                if (mine->isBlown) continue;
                if (!mine->isActive) continue;
                if (mine->point == target) return true;
            }
            return false;
        }
    }
    return false;
}

void PlayerModel::execute(int type, const PlayerAction& action) {
    const String& command = action.command;
    switch (type) {
        case MOVE: {
            std::vector<String> commands = java::split(command, u' ');
            const Direction& direction = directionOf(commands, command, u"Invalid move command: ");
            String power = getPower(*this, command);
            move(direction);
            chargePower(power);
            summaries.push_back(u"MOVE " + A(direction.name));
            break;
        }
        case SURFACE:
            game->addTooltip(*this, u"SURFACE");
            surface();
            summaries.push_back(u"SURFACE " + I(getSector()));
            break;
        case TORPEDO: {
            game->addTooltip(*this, command);
            Point target = getTarget(command, u"Invalid params to Torpedo. Should be TORPEDO x y");
            game->explode(target);
            summaries.push_back(u"TORPEDO " + I(target.x) + u" " + I(target.y));
            charges[TORPEDO] = 0;
            break;
        }
        case SILENCE: {
            std::vector<String> commands = java::split(command, u' ');
            const Direction& dir = directionOf(commands, command, u"Invalid move command: ");
            int dist = getDistance(command);
            for (int i = 0; i < dist; i++) move(dir);
            summaries.push_back(u"SILENCE");
            charges[SILENCE] = 0;
            break;
        }
        case SONAR: {
            int sector = getSonarSector(command);
            sonarResult = getOpponent().getSector() == sector ? u"Y" : u"N";
            summaries.push_back(u"SONAR " + I(sector));
            game->addTooltip(*this, u"SONAR " + I(sector));
            charges[SONAR] = 0;
            break;
        }
        case MINE: {
            Point target = getMineTarget(*this, command);
            auto m = std::make_shared<Mine>();
            m->point = target;
            mines.push_back(m);
            summaries.push_back(u"MINE");
            charges[MINE] = 0;
            break;
        }
        case TRIGGER: {
            game->addTooltip(*this, command);
            Point target = getTarget(command, u"Invalid params to Trigger. Should be Trigger x y");
            for (auto& mine : mines) {
                if (mine->isBlown) continue;
                if (!mine->isActive) continue;
                if (!(mine->point == target)) continue;
                mine->isBlown = true;
                game->explode(target);
                summaries.push_back(u"TRIGGER " + I(target.x) + u" " + I(target.y));
                return;
            }
            break;
        }
    }
}

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
void Game::initialize(int league) {
    updateScore();
    playerManager->sendData(getInitialInput(players[0]), 0);
    playerManager->sendData(getInitialInput(players[1]), 1);
    initializePlayer(players[0], league);
    initializePlayer(players[1], league);
}

void Game::onRound() {
    if (isGameOver()) {
        playerManager->endGame();
        return;
    }
    try {
        if (turn == 0) {
            turn++;
            activePlayer = &players[0];
            handleNewRound(*activePlayer);
            updateScore();
            return;
        }
        if (activePlayer->hasMoreActions()) {
            activePlayer->performRoundAction();
            updateScore();
            return;
        }
        turn++;
        if (turn > maxTurns) {
            playerManager->endGame();
            return;
        }
        activePlayer = &players[1 - activePlayer->index];
        handleNewRound(*activePlayer);
        updateScore();
    } catch (const TimeoutException&) {
        playerManager->disablePlayer(activePlayer->index, u"Timeout!");
        activePlayer->life = -1;
    } catch (const GameException& e) {
        playerManager->disablePlayer(activePlayer->index, e.message);
        activePlayer->life = -1;
    } catch (const java::Exception&) {
        playerManager->disablePlayer(activePlayer->index, u"provided invalid input");
        activePlayer->life = -1;
    }
}

void Game::handleNewRound(PlayerModel& p) {
    playerManager->sendData(getRoundInput(p), p.index);
    playerManager->execute(p.index);
    p.handleRoundInput(playerManager->getOutputs(p.index));
}

std::vector<String> Game::getRoundInput(PlayerModel& p) {
    PlayerModel& opponent = p.getOpponent();
    std::vector<String> input(3);
    input[0] = I(p.position.x) + u" " + I(p.position.y) + u" " + I(p.life) + u" " + I(opponent.life) + u" " + p.getCooldowns();
    input[1] = p.sonarResult;
    input[2] = opponent.getSummaries();
    return input;
}

std::vector<String> Game::getInitialInput(PlayerModel& p) {
    std::vector<String> input;
    input.push_back(I(GRID_SIZE) + u" " + I(GRID_SIZE) + u" " + I(p.index));
    for (int y = 0; y < GRID_SIZE; y++) {
        String line;
        for (int x = 0; x < GRID_SIZE; x++) line += gridModel.grid[x][y] ? u'x' : u'.';
        input.push_back(line);
    }
    return input;
}

void Game::initializePlayer(PlayerModel& p, int league) {
    try {
        playerManager->execute(p.index);
        p.initialize(playerManager->getOutputs(p.index), league);
    } catch (const TimeoutException&) {
        playerManager->disablePlayer(p.index, u"Timeout!");
        p.life = -1;
    } catch (const GameException& e) {
        playerManager->disablePlayer(p.index, e.message);
        p.life = -1;
    } catch (const java::Exception&) {
        playerManager->disablePlayer(p.index, u"provided invalid input");
        p.life = -1;
    }
}

// ----------------------------------------------------------------------------
// ViewController.java
// ----------------------------------------------------------------------------
namespace view {

using gem::Entity;
static const int CELL_WIDTH = 60;
static const int GRID_ORIGIN_Y = 90;  // (int) Math.round(1080 / 2 - CELL_WIDTH * GRID_SIZE / 2)
static const int GRID_ORIGIN_X = 510; // (int) Math.round(1920 / 2 - CELL_WIDTH * GRID_SIZE / 2)
static const double PI = 3.141592653589793;

static int convertX(double unit) { return java::d2i(GRID_ORIGIN_X + unit * CELL_WIDTH); }
static int convertY(double unit) { return java::d2i(GRID_ORIGIN_Y + unit * CELL_WIDTH); }
static int colorToken(int index) { return -(index + 1); }

static gem::Module& M() { return gem_; }

static Entity* createCircle(const Point& point, int color) {
    Entity* c = M().createCircle();
    M().setX(c, convertX(point.x) + CELL_WIDTH / 2);
    M().setY(c, convertY(point.y) + CELL_WIDTH / 2);
    M().setRadius(c, CELL_WIDTH / 3);
    M().setFillColor(c, color);
    M().setLineColor(c, color);
    return c;
}

static Entity* createLine(const Point& p1, const Point& p2, int color) {
    Entity* l = M().createLine();
    M().setX(l, convertX(p1.x) + CELL_WIDTH / 2);
    M().setY(l, convertY(p1.y) + CELL_WIDTH / 2);
    M().setX2(l, convertX(p2.x) + CELL_WIDTH / 2);
    M().setY2(l, convertY(p2.y) + CELL_WIDTH / 2);
    M().setLineColor(l, color);
    M().setLineWidth(l, 3);
    return l;
}

static void addExplosion(const Point& position, const PlayerModel& playerModel) {
    Entity* rect = M().createRectangle();
    M().setWidth(rect, CELL_WIDTH * 3);
    M().setHeight(rect, CELL_WIDTH * 3);
    M().setX(rect, convertX(position.x) - CELL_WIDTH);
    M().setY(rect, convertY(position.y) - CELL_WIDTH);
    M().setAlpha(rect, 0);
    M().setFillColor(rect, colorToken(playerModel.index));
    M().setLineAlpha(rect, 0);
    M().commitEntityState(0, {rect});
    M().setAlpha(rect, 0.3, gem::ELASTIC);
    M().commitEntityState(0.2, {rect});
    M().setAlpha(rect, 0, gem::ELASTIC);

    Entity* entity = M().createSprite();
    M().setImage(entity, u"explosion/0.png");
    M().setX(entity, convertX(position.x) + CELL_WIDTH / 2);
    M().setY(entity, convertY(position.y) + CELL_WIDTH / 2);
    M().setScale(entity, 1.5);
    M().setAnchor(entity, 0.5);
    const int animation = 5;
    for (int i = 0; i < animation; i++) {
        M().setImage(entity, u"explosion/" + I(i) + u".png");
        M().commitEntityState(i / (animation - 1.0), {entity});
    }
    M().setScale(entity, 0.5);
}

struct ViewPart {
    virtual ~ViewPart() {}
    virtual void update() = 0;
};

struct ExplosionPart : ViewPart {
    void update() override {
        PlayerModel* ap = game.activePlayer;
        if (ap != nullptr && ap->hasLastCommand && ap->lastCommand.activator == u"TORPEDO") {
            std::vector<String> coords = java::split(ap->lastCommand.command, u' ');
            i32 x = 0, y = 0;
            java::parseInt(coords.at(1), x); // checked by TorpedoPower#canExecute
            java::parseInt(coords.at(2), y);
            explode(Point{x, y}, *ap);
        }
    }
    void explode(const Point& point, const PlayerModel& playerModel) {
        Entity* line = createLine(playerModel.position, point, colorToken(playerModel.index));
        M().commitEntityState(0.2, {line});
        M().setAlpha(line, 0);
        M().commitEntityState(1, {line});
        addExplosion(point, playerModel);
    }
};

struct MinePart : ViewPart {
    PlayerModel* playerModel;
    std::map<const Mine*, Entity*> mines; // a HashMap<Mine, SpriteAnimation> (never iterated)
    explicit MinePart(PlayerModel* pm) : playerModel(pm) {}
    void update() override {
        if (game.activePlayer != playerModel) return;
        for (size_t i = 0; i < playerModel->mines.size(); i++) {
            std::shared_ptr<Mine> mine = playerModel->mines[i];
            if (!mine->isActive && !mines.count(mine.get())) {
                addMine(mine.get());
            } else if (mine->isBlown && mines.count(mine.get())) {
                Entity* e = mines[mine.get()];
                M().setAlpha(e, 0);
                tooltips_.setTooltipText(e, String());
                M().commitEntityState(0.4, {e});
                mines.erase(mine.get());
                addExplosion(mine->point, *playerModel);
            }
        }
    }
    void addMine(const Mine* mine) {
        int index = playerModel->index;
        Entity* a = M().createSpriteAnimation();
        M().setImages(a, {"mine0.png", "mine1.png", "mine0.png", "mine2.png"});
        M().setZIndex(a, 5);
        M().setAnchor(a, 0.5);
        M().setTint(a, colorToken(index));
        M().setX(a, convertX(playerModel->position.x) + CELL_WIDTH / 2);
        M().setY(a, convertY(playerModel->position.y) + CELL_WIDTH / 2);
        M().setDuration(a, 1000);
        M().setLoop(a, true);
        mines[mine] = a;
        tooltips_.setTooltipText(a, u"Mine\nOwner = " + I(index));
        M().commitEntityState(0, {a});
        M().setX(a, convertX(mine->point.x) + CELL_WIDTH / 2 - 10 + index * 20);
        M().setY(a, convertY(mine->point.y) + CELL_WIDTH / 2 - 10 + index * 20);
        M().setZIndex(a, 5);
        M().setPlaying(a, true);
    }
};

static Entity* fillCell(const Point& p, int index) {
    Entity* r = M().createRectangle();
    M().setX(r, convertX(p.x));
    M().setY(r, convertY(p.y));
    M().setWidth(r, CELL_WIDTH);
    M().setHeight(r, CELL_WIDTH);
    M().setAlpha(r, 0.5);
    M().setFillColor(r, colorToken(index));
    M().setLineAlpha(r, 0);
    M().setZIndex(r, index * -1 + 1);
    return r;
}

struct PathMapPart : ViewPart {
    PlayerModel* playerModel;
    Entity* grid[GRID_SIZE][GRID_SIZE] = {};
    explicit PathMapPart(PlayerModel* pm) : playerModel(pm) {
        // grid[position.x][position.y] = fillCell(...): grid[position.x] first, then the cell, then the store
        if (!pm->hasPosition) throw npe();
        Point p = pm->position;
        if (p.x < 0 || p.x >= GRID_SIZE) throw java::Exception{java::arrayIndexOutOfBoundsException(p.x, GRID_SIZE)};
        Entity* cell = fillCell(p, pm->index);
        if (p.y < 0 || p.y >= GRID_SIZE) throw java::Exception{java::arrayIndexOutOfBoundsException(p.y, GRID_SIZE)};
        grid[p.x][p.y] = cell;
    }
    void update() override {
        if (game.activePlayer != playerModel) return;
        for (int i = 0; i < GRID_SIZE; i++) {
            for (int j = 0; j < GRID_SIZE; j++) {
                Entity* path = grid[i][j];
                if (path == nullptr && playerModel->grid[i][j]) {
                    grid[i][j] = fillCell(Point{i, j}, playerModel->index);
                    continue;
                } else if (path != nullptr && playerModel->grid[i][j]) {
                    continue;
                }
                if (path != nullptr && !playerModel->grid[i][j]) M().setAlpha(path, 0);
                grid[i][j] = nullptr;
            }
        }
    }
};

struct SubmarinePart : ViewPart {
    PlayerModel* playerModel;
    Entity* group = nullptr;
    Entity* sub = nullptr;
    explicit SubmarinePart(PlayerModel* pm) : playerModel(pm) {
        int index = pm->index;
        sub = M().createSprite();
        M().setImage(sub, u"sub.png");
        M().setTint(sub, colorToken(index));
        M().setScale(sub, 1);
        M().setScaleY(sub, 1.5);
        M().setAnchor(sub, 0.5);
        group = M().createGroup({sub});
        M().setZIndex(group, 100);
        if (!pm->hasPosition) throw npe();
        M().setX(group, convertX(pm->position.x) + CELL_WIDTH / 2);
        M().setY(group, convertY(pm->position.y) + CELL_WIDTH / 2);
        tooltips_.setTooltipText(sub, u"Player " + I(index));
        M().commitEntityState(0, {group, sub});
    }
    void update() override {
        if (game.activePlayer != playerModel) return;
        Entity* image = sub;
        handleSonar();
        handleSurface();

        int newX = convertX(playerModel->position.x) + CELL_WIDTH / 2;
        int newY = convertY(playerModel->position.y) + CELL_WIDTH / 2;

        double rot = image->rotation;
        if (newX - group->x < 0) {
            M().setRotation(image, PI);
        } else if (newX - group->x > 0) {
            M().setRotation(image, 0);
        } else if (newY - group->y < 0) {
            M().setRotation(image, PI / 2 * 3);
        } else if (newY - group->y > 0) {
            M().setRotation(image, PI / 2);
        } else {
            handleSilence(); // 0 dist
            return;
        }
        if (std::abs(rot - image->rotation) > 0.0001) M().commitEntityState(0, {image});
        handleSilence();
        M().setX(group, newX);
        M().setY(group, newY);
    }
    void handleSilence() {
        if (game.activePlayer == playerModel && playerModel->hasLastCommand && playerModel->lastCommand.activator == u"SILENCE") {
            M().setAlpha(sub, 0.7, gem::LINEAR);
            M().commitEntityState(0.5, {sub});
            M().setAlpha(sub, 1.0, gem::LINEAR);
        }
    }
    void handleSonar() {
        if (game.activePlayer == playerModel && playerModel->hasLastCommand && playerModel->lastCommand.activator == u"SONAR") {
            i32 g = 0;
            java::parseInt(java::split(playerModel->lastCommand.command, u' ').at(1), g); // checked by SonarPower#canExecute
            g--;
            int x = g % 3 * 5 + 2;
            int y = g / 3 * 5 + 2;
            int color = colorToken(playerModel->index);
            Entity* sonar = M().createGroup();
            M().setX(sonar, convertX(x) + CELL_WIDTH / 2);
            M().setY(sonar, convertY(y) + CELL_WIDTH / 2);
            M().setZIndex(sonar, 1000);

            Entity* c1 = M().createCircle();
            M().setFillAlpha(c1, 0);
            M().setLineAlpha(c1, 1);
            M().setRadius(c1, CELL_WIDTH * 3);
            M().setLineColor(c1, color);
            M().setLineWidth(c1, 6);
            M().add(sonar, {c1});

            Entity* c2 = M().createCircle();
            M().setFillAlpha(c2, 0);
            M().setLineAlpha(c2, 1);
            M().setRadius(c2, CELL_WIDTH * 2);
            M().setLineColor(c2, color);
            M().setLineWidth(c2, 6);
            M().add(sonar, {c2});

            M().setScale(c2, 0);
            M().setScale(c1, 0);
            M().commitEntityState(0, {c1, c2, sonar});
            M().setScale(c2, 1, gem::EASE_OUT);
            M().setScale(c1, 1, gem::EASE_OUT);
            M().commitEntityState(0.3, {c1, c2});
            M().setScale(c2, 0.7, gem::EASE_IN);
            M().setScale(c1, 0.7, gem::EASE_IN);
            M().commitEntityState(0.6, {c1, c2});
            M().setScale(c2, 1.3, gem::EASE_OUT);
            M().setScale(c1, 1.3, gem::EASE_OUT);
            M().commitEntityState(0.8, {c1, c2, sonar});
            M().setAlpha(sonar, 0, gem::ELASTIC);
            M().setScale(c2, 0, gem::EASE_IN_AND_OUT);
            M().setScale(c1, 0, gem::EASE_IN_AND_OUT);
        }
    }
    void handleSurface() {
        if (game.activePlayer == playerModel && playerModel->hasLastCommand && playerModel->lastCommand.command == u"SURFACE") {
            Entity* circle = createCircle(playerModel->position, colorToken(playerModel->index));
            M().setFillAlpha(circle, 0);
            M().setScale(circle, 0);

            M().commitEntityState(0, {circle});
            M().setFillAlpha(circle, 0.5);
            M().setScale(circle, 1, gem::ELASTIC);
            M().commitEntityState(0.2, {circle});

            M().setScale(circle, 10);
            M().setAlpha(circle, 0);
            M().commitEntityState(1, {circle});

            M().setScale(group, 2);
            M().commitEntityState(0.5, {group});
            M().setScale(group, 1);
            M().commitEntityState(1, {group});
        }
    }
};

struct LifePart : ViewPart {
    int lastLife;
    PlayerModel* playerModel;
    std::vector<Entity*> graphicLife;
    explicit LifePart(PlayerModel* pm) : lastLife(pm->life), playerModel(pm) {
        for (int i = 0; i < pm->life; i++) {
            int x = 1080 / 4 + 1080 / 6 - 10;
            int y = 35 + (pm->life - i - 1) * 40;
            if (pm->index == 1) x = 1920 - x;
            Entity* c = M().createCircle();
            M().setX(c, x);
            M().setY(c, y);
            M().setFillColor(c, colorToken(pm->index));
            M().setRadius(c, 12);
            M().setAlpha(c, 0.8);
            M().setLineColor(c, 0x000000);
            M().setLineWidth(c, 4);
            graphicLife.push_back(c);
            M().commitEntityState(0, {c});
        }
    }
    void update() override {
        for (int i = lastLife - 1; i >= std::max(0, playerModel->life); i--) {
            Entity* c = graphicLife.at(i);
            M().setFillColor(c, 0x555555);
            M().setScale(c, 0.5);
        }
        lastLife = playerModel->life;
    }
};

struct PowerPart : ViewPart {
    int charges = 0;
    PlayerModel* playerModel;
    int power;
    std::vector<Entity*> graphicCharges;
    PowerPart(PlayerModel* pm, int i) : playerModel(pm) {
        if (i >= (int)pm->powers.size()) throw java::Exception{java::indexOutOfBoundsException(i, (i64)pm->powers.size())};
        power = pm->powers[i];
        int index = pm->index;
        int y = 550 + 120 * PlayerModel::powerIndex(power);

        Entity* t = M().createText(A(COMMAND_NAME[power]));
        M().setFontFamily(t, "Arial");
        M().setX(t, index == 1 ? 30 : 1890);
        M().setY(t, y - 50);
        M().setAnchorX(t, index == 1 ? 0 : 1);
        M().setFontSize(t, 30);
        M().setAnchorY(t, 0.5);
        M().setZIndex(t, 5);
        M().setFontWeight(t, gem::FontWeight::BOLD);
        M().setFillColor(t, 0xefefef);
        M().commitEntityState(0, {t});

        for (int k = 0; k < PlayerModel::maxValue(power); k++) {
            int x = 50 + 55 * k;
            if (index == 1) x = 1920 - (50 + 55 * k);
            Entity* c = M().createCircle();
            M().setX(c, x);
            M().setY(c, y);
            M().setFillColor(c, 0x555555);
            M().setAlpha(c, 0.8);
            M().setRadius(c, 20);
            M().setScale(c, 0.5);
            M().setLineWidth(c, 5);
            M().setLineColor(c, 0x000000);
            graphicCharges.push_back(c);
            M().commitEntityState(0, {c});
        }
    }
    void update() override {
        if (game.activePlayer != playerModel) return;
        int now = playerModel->charges[power];
        if (charges < now) {
            for (int i = charges; i < now; i++) {
                M().setScale(graphicCharges.at(i), 1.0, gem::ELASTIC);
                M().setFillColor(graphicCharges.at(i), colorToken(playerModel->index));
            }
        } else if (charges > now) {
            for (int i = charges - 1; i >= now; i--) {
                M().setScale(graphicCharges.at(i), 0.5, gem::ELASTIC);
                M().setFillColor(graphicCharges.at(i), 0x555555);
            }
        }
        charges = now;
    }
};

struct MessagePart : ViewPart {
    PlayerModel* playerModel;
    Entity* messageText;
    explicit MessagePart(PlayerModel* pm) : playerModel(pm) {
        messageText = M().createText(String());
        M().setFontFamily(messageText, "Arial");
        M().setFillColor(messageText, 0xffffff);
        M().setX(messageText, pm->index == 0 ? 30 : 1890);
        M().setMaxWidth(messageText, 400);
        M().setY(messageText, 370);
        M().setFontSize(messageText, 30);
        M().setZIndex(messageText, 100);
        if (pm->index == 1) M().setAnchorX(messageText, 1);
    }
    void update() override {
        if (messageText->text != playerModel->message) {
            M().setText(messageText, playerModel->message);
            M().commitEntityState(0, {messageText});
        }
    }
};

struct ActionMessagePart : ViewPart {
    Entity* actionText;
    ActionMessagePart() {
        actionText = M().createText(String());
        M().setX(actionText, 1920 / 2);
        M().setAnchor(actionText, 0.5);
        M().setStrokeColor(actionText, 0x000000);
        M().setStrokeThickness(actionText, 2);
        M().setY(actionText, 1040);
        M().setFontFamily(actionText, "Arial");
        M().setFontSize(actionText, 40);
    }
    void update() override {
        M().setText(actionText, game.activePlayer->actionMessage);
        M().setFillColor(actionText, colorToken(game.activePlayer->index));
        M().commitEntityState(0, {actionText});
    }
};

struct ViewController {
    std::vector<std::unique_ptr<ViewPart>> parts;

    void preInitialize() {
        setupMap();
        parts.push_back(std::make_unique<ActionMessagePart>());
        parts.push_back(std::make_unique<ExplosionPart>());
        setupPlayer(game.players[0]);
        setupPlayer(game.players[1]);
    }

    void initialize() {
        parts.push_back(std::make_unique<SubmarinePart>(&game.players[0]));
        parts.push_back(std::make_unique<SubmarinePart>(&game.players[1]));
        parts.push_back(std::make_unique<PathMapPart>(&game.players[0]));
        parts.push_back(std::make_unique<PathMapPart>(&game.players[1]));
        for (size_t i = 0; i < game.players[0].powers.size(); i++) {
            parts.push_back(std::make_unique<PowerPart>(&game.players[0], (int)i));
            parts.push_back(std::make_unique<PowerPart>(&game.players[1], (int)i));
        }
    }

    void onRound() {
        for (auto& part : parts) part->update();
    }

    void setupMap() {
        for (auto& [point, image] : game.gridModel.tiles) {
            Entity* sprite = M().createSprite();
            M().setImage(sprite, image);
            M().setBaseWidth(sprite, 60);
            M().setBaseHeight(sprite, 60);
            M().setX(sprite, convertX(point.x));
            M().setY(sprite, convertY(point.y));
            M().commitEntityState(0, {sprite});
            tooltips_.setTooltipText(sprite, u"Island\nx = " + I(point.x) + u"\ny = " + I(point.y));
        }
        // set tooltips for water tiles
        for (int x = 0; x < GRID_SIZE; x++) {
            for (int y = 0; y < GRID_SIZE; y++) {
                if (!game.gridModel.grid[x][y]) {
                    Entity* r = M().createRectangle();
                    M().setX(r, convertX(x));
                    M().setY(r, convertY(y));
                    M().setWidth(r, CELL_WIDTH);
                    M().setHeight(r, CELL_WIDTH);
                    M().setAlpha(r, 0);
                    tooltips_.setTooltipText(r, u"Water\nx = " + I(x) + u"\ny = " + I(y));
                }
            }
        }
    }

    void setupPlayer(PlayerModel& pm) {
        parts.push_back(std::make_unique<MessagePart>(&pm));
        parts.push_back(std::make_unique<LifePart>(&pm));
        parts.push_back(std::make_unique<MinePart>(&pm));
        createNickname(pm.index);
        createPlayerFrame(pm.index);
    }

    void createPlayerFrame(int index) {
        Entity* sprite = M().createSprite();
        M().setImage(sprite, u"$" + I(index)); // getAvatarToken()
        M().setAnchorX(sprite, 0.5);
        M().setBaseHeight(sprite, 200);
        M().setBaseWidth(sprite, 200);
        M().setY(sprite, 50);
        M().setX(sprite, 1080 / 4);
        addFrame(1080 / 4 - 5 - 100, 45, 205, 205, 1, index);
        if (index == 1) M().setX(sprite, 1920 - sprite->x);
        M().commitEntityState(0, {sprite});
    }

    void addFrame(int x, int y, int width, int height, int z, int index) {
        Entity* rect = M().createRectangle();
        M().setLineColor(rect, colorToken(index));
        M().setLineWidth(rect, 10);
        M().setZIndex(rect, z);
        M().setX(rect, x);
        M().setY(rect, y);
        M().setWidth(rect, width);
        M().setHeight(rect, height);
        M().setFillAlpha(rect, 0);
        if (index == 1) M().setX(rect, 1920 - rect->x - rect->width);
        M().commitEntityState(0, {rect});
    }

    void createNickname(int index) {
        Entity* nickname = M().createText(u"$" + I(index)); // getNicknameToken()
        M().setFontFamily(nickname, "Arial");
        M().setX(nickname, 1080 / 4);
        M().setY(nickname, 270);
        M().setZIndex(nickname, 20);
        M().setAnchorX(nickname, 0.5);
        M().setMaxWidth(nickname, 400);
        M().setFontSize(nickname, 60);
        M().setFillColor(nickname, colorToken(index));
        if (index == 1) M().setX(nickname, 1920 - nickname->x);
        M().commitEntityState(0, {nickname});
    }
};

} // namespace view

static view::ViewController viewController;

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
void Referee::init() {
    cg::gm.setTurnMaxTime(50);
    cg::gm.setMaxTurns(1000);
    random = java::Random(cg::gm.getSeed());
    gem::Entity* background = gem_.createSprite();
    gem_.setImage(background, u"background.jpg");
    gem_.setAnchor(background, 0);

    int diff = 87;
    addFrame(1920 / 2 - 1080 / 2 + diff, diff, 1080 - diff * 2, 1080 - diff * 2, 99);
    game.init(this, random);
    viewController.preInitialize();
}

static String messageOf(const java::Exception& e) {
    // Throwable#getMessage: the text after "<class>: " (null when there is none)
    size_t p = e.text.find(": ");
    return p == std::string::npos ? String(u"null") : java::decodeUtf8(e.text.substr(p + 2));
}

void Referee::gameTurn(int turn) {
    try {
        if (turn == 1) {
            cg::gm.setFrameDuration(10);
            game.initialize(getLeague()); // firstTurn
            viewController.initialize();
        } else {
            cg::gm.setFrameDuration(500);
            game.onRound(); // classicTurn
            viewController.onRound();
        }
    } catch (const java::Exception& e) {
        // in the first turn (the only place where the view throws: a player without a valid position)
        // there is no active player yet
        if (game.activePlayer != nullptr) {
            String m = messageOf(e);
            disablePlayer(game.activePlayer->index, m);
            cg::gm.addToGameSummary(m);
            cg::gm.addTooltip(game.activePlayer->index, m);
        }
    }
}

void Referee::onEnd() {
    auto lifeString = [](int amount) { return std::abs(amount) == 1 ? u" life" : u" lives"; };
    int s0 = players[0].getScore(), s1 = players[1].getScore();
    endScreen_.setScores({s0, s1}, {I(s0) + lifeString(s0), I(s1) + lifeString(s1)});
    endScreen_.setTitleRankingsSprite(u"logo.png");
}

int main(int argc, char** argv) {
    cg::Options options;
    options.maxPlayers = 2;
    options.engine313 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
