// Spring Challenge 2021 (Photosynthesis) - headless C++ referee, port of
// https://github.com/CodinGame/SpringChallenge2021 (commit 91bcff0), CodinGame game engine 4.0.2.
// 2 players, league 19 as set by the CLI, i.e. the default case of Game.init (Bronze and above:
// seeds, growth, shadows and holes enabled, 24 days).
// Deterministic; reproduces the same game as the reference jar (cg-2021-spring-photosynthesis-ref) for a -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints "<p0score>\n<p1score>\nseed=<N>\n" to stdout, like the jar's CLI, and exits 0. When the
// Java referee or the runner fails during the game (see below) the jar prints "null" for each
// score and still exits 0, so we do the same (and print the Java exception to stderr). Like the
// jar's CLI: a missing -p1/-p2 prints the usage (exit 0); an invalid -seed, a missing option value
// or an unknown option exits 1 without scores. -p3/-p4 are refused (exit 1).
// The engine and the command line are common/engine.h; -l writes its trace (see there) plus some
// informative lines.
//
// The Java referee is followed closely (same data structures and iteration orders). Notable Java
// behaviours reproduced here:
//   - java.util.Random(seed) and its consumers in order: BoardGenerator (holes are drawn from the
//     java.util.HashMap keySet order of the CubeCoord cells), the starting trees, and
//     Collections.shuffle of the COMPLETE, GROW and SEED possible moves of each input
//   - TreeMap order of the trees; a seed conflict makes every action of that turn free (the
//     available sun is not written back)
//   - engine 4.0.2 GameManager:
//       * a frame in which no player is executed still executes player 0 for 0 lines, so player 0
//         receives its pending input then (the global info at turn 1)
//       * every real execution adds turnMaxTime (100 ms) to a total: the 301st execution throws
//         "Total game duration too long (>30000ms)" and the game fails (scores "null")
//   - engine 4.0.2 runner (GameRunner, Agent):
//       * Agent#getOutput: '\r' and '\n' terminate lines ("\r\n" counts once, even across turns),
//         10000 bytes max, 1000 ms for the first execution of a player and 100 ms afterwards; the
//         bytes are decoded as UTF-8 exactly like java.lang.String does (malformed -> U+FFFD)
//       * the referee reads its stdin with a java.util.Scanner, which also ends lines at U+0085,
//         U+2028 and U+2029: the rest of such a line is read as the next engine command, so the
//         referee usually crashes at its next player execution
//       * the runner reads each referee command into a 30000-byte buffer: a turn summary or a
//         tooltip repeating a long invalid command (e.g. ~10000 invalid UTF-8 bytes, each one
//         becomes U+FFFD, 3 bytes) does not fit and the game fails (scores "null")
//       * the inputs are written by a thread per player (AsynchronousWriter): a bot that does not
//         read its stdin does not block the game, and a failed write (the bot exited) does not
//         prevent reading what the bot wrote before
//       * Agent#readError: a bot's stderr goes to a 4120-byte pipe (java.lang.ProcessImpl on
//         Windows) of which the runner reads at most 4096 bytes after each execution of that
//         player: a bot that writes more to stderr blocks and times out (the content is
//         discarded; on POSIX the runner reads through an 8192-byte BufferedInputStream)
//   - wall clock: the jar starts the bots and then spends 0.3-1.1 s (measured) initialising its
//     referee before the first input, so a slow bot startup rarely counts in its first turn. The
//     first turn of each player is timed here as if its input had been sent 500 ms after the bot
//     was started (BOT_STARTUP_GRACE_MS); later turns are timed exactly like the engine does.
//   - the view data quota (1 MB) cannot be reached: at most ~350 frames (the 30 s total turn time
//     above) of at most ~2 KB each; player messages only go to the view, so they are not kept.

#include "common/engine.h"

// ----------------------------------------------------------------------------
// Constants.java / Config.java
// ----------------------------------------------------------------------------
namespace C {
const int RICHNESS_NULL = 0;
const int RICHNESS_POOR = 1;
const int RICHNESS_OK = 2;
const int RICHNESS_LUSH = 3;
const int TREE_SEED = 0;
const int TREE_SMALL = 1;
const int TREE_TALL = 3;
const int TREE_BASE_COST[4] = {0, 1, 3, 7};
const int LIFECYCLE_END_COST = 4;
const int STARTING_TREE_COUNT = 2;
const int RICHNESS_BONUS_OK = 2;
const int RICHNESS_BONUS_LUSH = 4;
// Config defaults (the CLI passes no game parameter but the seed)
const int STARTING_SUN = 0;
const int MAP_RING_COUNT = 3;
const int STARTING_NUTRIENTS = 20;
const int MAX_ROUNDS = 24;
const int MAX_EMPTY_CELLS = 10;
// Game.init, default case (league 19)
const bool ENABLE_SEED = true;
const bool ENABLE_GROW = true;
const bool ENABLE_SHADOW = true;
const bool ENABLE_HOLES = true;
const int STARTING_TREE_SIZE = TREE_SMALL;
const int STARTING_TREE_DISTANCE = 2;
// Referee.init
const int FIRST_TURN_MAX_TIME = 1000;
const int TURN_MAX_TIME = 100;
// The jar starts the bots, then initialises its referee (0.3-1.1 s measured) before it sends the
// first input, so a bot's startup is mostly not counted in its first turn: the first turn of a
// player is timed as if its input had been sent BOT_STARTUP_GRACE_MS after the bot was started
// (cg::AgentOptions::firstTurnHeadStartMs)
const int BOT_STARTUP_GRACE_MS = 500;
const int NCELLS = 1 + 3 * MAP_RING_COUNT * (MAP_RING_COUNT + 1);
const char* const EXPECTED = "SEED <from> <to> | GROW <idx> | COMPLETE <idx> | WAIT"; // Game.getExpected()
} // namespace C

// ----------------------------------------------------------------------------
// CubeCoord.java, Cell.java, Board.java
// ----------------------------------------------------------------------------
static const int DIRECTIONS[6][3] = {{1, -1, 0}, {+1, 0, -1}, {0, +1, -1}, {-1, +1, 0}, {-1, 0, +1}, {0, -1, +1}};

struct CubeCoord {
    int x, y, z;
    i32 hashCode() const {
        i32 result = 1;
        result = 31 * result + x;
        result = 31 * result + y;
        result = 31 * result + z;
        return result;
    }
    bool operator==(const CubeCoord& o) const { return x == o.x && y == o.y && z == o.z; }
    CubeCoord neighbor(int orientation, int distance = 1) const {
        return {x + DIRECTIONS[orientation][0] * distance, y + DIRECTIONS[orientation][1] * distance,
                z + DIRECTIONS[orientation][2] * distance};
    }
    int distanceTo(const CubeCoord& d) const { return (std::abs(x - d.x) + std::abs(y - d.y) + std::abs(z - d.z)) / 2; }
    CubeCoord getOpposite() const { return {-x, -y, -z}; }
};

// Board.map (HashMap<CubeCoord, Cell>) lookups: every coord has x + y + z == 0, and the referee
// never looks further than 6 cells away from the centre (a tall tree's shadow from the edge)
struct Board {
    static const int R = 8;
    int cellAt[2 * R + 1][2 * R + 1]; // cell index, -1 when absent
    CubeCoord coords[C::NCELLS];      // Board.coords: sorted by cell index
    int richness[C::NCELLS];
    int cellIndex(const CubeCoord& c) const { // board.map.getOrDefault(c, Cell.NO_CELL).getIndex()
        if (c.x < -R || c.x > R || c.y < -R || c.y > R) return -1;
        return cellAt[c.x + R][c.y + R];
    }
    bool contains(const CubeCoord& c) const { return cellIndex(c) >= 0; }
};

// ----------------------------------------------------------------------------
// Player.java, Tree.java, Seed.java, actions
// ----------------------------------------------------------------------------
enum ActionType { NO_ACTION, WAIT, GROW, SEED, COMPLETE };
struct Action {
    ActionType type = NO_ACTION;
    int sourceId = 0, targetId = 0;
};

struct Player : cg::Player {
    // Player
    Action action;
    int sun = C::STARTING_SUN;
    bool waiting = false;
    int bonusScore = 0;
    std::string token() const { return "$" + std::to_string(index); } // getNicknameToken
    void addScore(int s) { score += s; }
    int getExpectedOutputLines() const override { return 1; }
};

struct Tree {
    int size = 0;
    Player* owner = nullptr;
    int fatherIndex = -1;
    bool isDormant = false;
};

struct Seed {
    int owner, sourceCell, targetCell;
};

struct GameException {
    std::string message;
};

// ----------------------------------------------------------------------------
// Engine state (GameManager, MultiplayerGameManager)
// ----------------------------------------------------------------------------
static Player players[2];
using cg::trace;

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

static void addTooltip(int player, const std::string& message) { cg::gm.addTooltip(player, message); }

// GameManager.formatErrorMessage: "¤RED¤%s§RED§"
static std::string formatErrorMessage(const std::string& m) { return "\xC2\xA4RED\xC2\xA4" + m + "\xC2\xA7RED\xC2\xA7"; }

// ----------------------------------------------------------------------------
// GameSummaryManager.java (lines are UTF-8; only their size matters, see checkRunnerBuffer)
// ----------------------------------------------------------------------------
struct GameSummaryManager {
    std::vector<std::string> lines;
    void clear() { lines.clear(); }
    void addPlayerBadCommand(const Player& player, const std::string& expected, const std::string& got) {
        lines.push_back(formatErrorMessage(player.token() + " provided invalid input. Expected '" + expected + "'\nGot '" + got + "'"));
    }
    void addPlayerTimeout(const Player& player) {
        lines.push_back(formatErrorMessage(player.token() + " has not provided an action in time."));
    }
    void addPlayerDisqualified(const Player& player) { lines.push_back(player.token() + " was disqualified."); }
    void addCutTree(const Player& player, int cell, int score) {
        lines.push_back(player.token() + " is ending their tree life on cell " + std::to_string(cell) + ", scoring " +
                        std::to_string(score) + " points");
    }
    void addGrowTree(const Player& player, int cell) {
        lines.push_back(player.token() + " is growing a tree on cell " + std::to_string(cell));
    }
    void addPlantSeed(const Player& player, int targetCell, int sourceCell) {
        lines.push_back(player.token() + " is planting a seed on cell " + std::to_string(targetCell) + " from cell " +
                        std::to_string(sourceCell));
    }
    void addWait(const Player& player) { lines.push_back(player.token() + " is waiting"); }
    void addRound(int round) {
        lines.push_back("Round " + std::to_string(round) + "/" + std::to_string(C::MAX_ROUNDS - 1));
    }
    void addError(const std::string& error) { lines.push_back(error); }
    void addSeedConflict(const Seed& seed) { lines.push_back("Seed conflict on cell " + std::to_string(seed.targetCell)); }
    void addRoundTransition(int round) {
        lines.push_back("Round " + std::to_string(round) + " ends");
        if (round + 1 < C::MAX_ROUNDS) {
            lines.push_back("The sun is now pointing towards direction " + std::to_string((round + 1) % 6));
            lines.push_back("Round " + std::to_string(round + 1) + " starts");
        }
    }
    void addGather(const Player& player, int given) {
        lines.push_back(player.token() + " has collected " + std::to_string(given) + " sun points");
    }
    std::string toString() const {
        std::string s;
        for (size_t i = 0; i < lines.size(); i++) {
            if (i) s += '\n';
            s += lines[i];
        }
        return s;
    }
};
static GameSummaryManager gameSummaryManager;

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
enum FrameType { GATHERING, ACTIONS, SUN_MOVE, INIT };

struct Game {
    int nutrients = C::STARTING_NUTRIENTS;
    Board board;
    Tree* trees[C::NCELLS] = {}; // TreeMap<Integer, Tree>: iterated by increasing cell index
    Tree treeStore[C::NCELLS];   // the Tree object on each cell (a cell holds at most one tree)
    std::vector<CubeCoord> dyingTrees;
    std::vector<int> availableSun;
    std::vector<Seed> sentSeeds;
    int sunOrientation = 0;
    int shadows[C::NCELLS]; // HashMap<Integer, Integer>, -1 = absent
    java::Random random;
    int round = 0;
    int turn = 0;
    FrameType currentFrameType = INIT;
    FrameType nextFrameType = GATHERING;

    int treeCount() const {
        int n = 0;
        for (Tree* t : trees)
            if (t) n++;
        return n;
    }

    // BoardGenerator.java
    void generateBoard() {
        std::vector<CubeCoord> keys; // insertion order of board.put
        std::vector<int> rich;
        for (auto& row : board.cellAt)
            for (int& c : row) c = -1;
        auto generateCell = [&](const CubeCoord& coord, int richness) {
            int index = (int)keys.size();
            keys.push_back(coord);
            rich.push_back(richness);
            board.cellAt[coord.x + Board::R][coord.y + Board::R] = index;
        };
        CubeCoord centre{0, 0, 0};
        generateCell(centre, C::RICHNESS_LUSH);
        CubeCoord coord = centre.neighbor(0);
        for (int distance = 1; distance <= C::MAP_RING_COUNT; distance++) {
            for (int orientation = 0; orientation < 6; orientation++) {
                for (int count = 0; count < distance; count++) {
                    if (distance == C::MAP_RING_COUNT) generateCell(coord, C::RICHNESS_POOR);
                    else if (distance == C::MAP_RING_COUNT - 1) generateCell(coord, C::RICHNESS_OK);
                    else generateCell(coord, C::RICHNESS_LUSH);
                    coord = coord.neighbor((orientation + 2) % 6);
                }
            }
            coord = coord.neighbor(0);
        }
        for (int i = 0; i < C::NCELLS; i++) {
            board.coords[i] = keys[i];
            board.richness[i] = rich[i];
        }

        // new ArrayList<>(board.keySet())
        std::vector<i32> hashes;
        for (auto& k : keys) hashes.push_back(k.hashCode());
        std::vector<CubeCoord> coordList;
        for (int i : java::hashMapOrder(hashes)) coordList.push_back(keys[i]);
        int coordListSize = (int)coordList.size();
        int wantedEmptyCells = C::ENABLE_HOLES ? random.nextInt(C::MAX_EMPTY_CELLS + 1) : 0;
        int actualEmptyCells = 0;
        while (actualEmptyCells < wantedEmptyCells - 1) {
            int randIndex = random.nextInt(coordListSize);
            CubeCoord randCoord = coordList[randIndex];
            int ci = board.cellIndex(randCoord);
            if (board.richness[ci] != C::RICHNESS_NULL) {
                board.richness[ci] = C::RICHNESS_NULL;
                actualEmptyCells++;
                if (!(randCoord == randCoord.getOpposite())) {
                    board.richness[board.cellIndex(randCoord.getOpposite())] = C::RICHNESS_NULL;
                    actualEmptyCells++;
                }
            }
        }
    }

    void init(i64 seed) {
        random.setSeed(seed);
        generateBoard();
        for (Tree*& t : trees) t = nullptr;
        dyingTrees.clear();
        availableSun.clear();
        sentSeeds.clear();
        initStartingTrees();
        for (int& s : shadows) s = -1;
        sunOrientation = 0;
        round = 0;
        if (C::ENABLE_SHADOW) calculateShadows();
    }

    void initStartingTrees() {
        // STARTING_TREES_ON_EDGES: getBoardEdges()
        std::vector<CubeCoord> startingCoords;
        CubeCoord centre{0, 0, 0};
        for (int i = 0; i < C::NCELLS; i++)
            if (board.coords[i].distanceTo(centre) == C::MAP_RING_COUNT) startingCoords.push_back(board.coords[i]);
        std::vector<CubeCoord> kept;
        for (auto& c : startingCoords)
            if (board.richness[board.cellIndex(c)] != C::RICHNESS_NULL) kept.push_back(c);
        startingCoords = kept;
        std::vector<CubeCoord> validCoords;
        while ((int)validCoords.size() < C::STARTING_TREE_COUNT * 2) validCoords = tryInitStartingTrees(startingCoords);
        for (int i = 0; i < C::STARTING_TREE_COUNT; i++) {
            placeTree(&players[0], board.cellIndex(validCoords[2 * i]), C::STARTING_TREE_SIZE);
            placeTree(&players[1], board.cellIndex(validCoords[2 * i + 1]), C::STARTING_TREE_SIZE);
        }
    }

    std::vector<CubeCoord> tryInitStartingTrees(const std::vector<CubeCoord>& startingCoords) {
        std::vector<CubeCoord> coordinates;
        std::vector<CubeCoord> availableCoords = startingCoords;
        for (int i = 0; i < C::STARTING_TREE_COUNT; i++) {
            if (availableCoords.empty()) return coordinates;
            int r = random.nextInt((i32)availableCoords.size());
            CubeCoord normalCoord = availableCoords[r];
            CubeCoord oppositeCoord = normalCoord.getOpposite();
            std::vector<CubeCoord> rest;
            for (auto& c : availableCoords)
                if (!(c.distanceTo(normalCoord) <= C::STARTING_TREE_DISTANCE ||
                      c.distanceTo(oppositeCoord) <= C::STARTING_TREE_DISTANCE))
                    rest.push_back(c);
            availableCoords = rest;
            coordinates.push_back(normalCoord);
            coordinates.push_back(oppositeCoord);
        }
        return coordinates;
    }

    void calculateShadows() {
        for (int& s : shadows) s = -1;
        for (int index = 0; index < C::NCELLS; index++) {
            Tree* tree = trees[index];
            if (!tree) continue;
            CubeCoord coord = board.coords[index];
            for (int i = 1; i <= tree->size; i++) {
                int ci = board.cellIndex(coord.neighbor(sunOrientation, i));
                if (ci >= 0) shadows[ci] = shadows[ci] < 0 ? tree->size : std::max(shadows[ci], tree->size);
            }
        }
    }

    // getGlobalInfoFor
    std::string getGlobalInfoFor(const Player&) const {
        std::string s = std::to_string(C::NCELLS) + "\n";
        for (int i = 0; i < C::NCELLS; i++) {
            const CubeCoord& coord = board.coords[i];
            s += std::to_string(i) + " " + std::to_string(board.richness[i]);
            for (int d = 0; d < 6; d++) s += " " + std::to_string(board.cellIndex(coord.neighbor(d)));
            s += "\n";
        }
        return s;
    }

    std::string getCurrentFrameInfoFor(Player& player) {
        std::string s;
        s += std::to_string(round) + "\n";
        s += std::to_string(nutrients) + "\n";
        Player& other = players[1 - player.index];
        s += std::to_string(player.sun) + " " + std::to_string(player.score) + "\n";
        s += std::to_string(other.sun) + " " + std::to_string(other.score) + " " + (other.waiting ? "1" : "0") + "\n";
        s += std::to_string(treeCount()) + "\n";
        for (int index = 0; index < C::NCELLS; index++) {
            Tree* tree = trees[index];
            if (!tree) continue;
            s += std::to_string(index) + " " + std::to_string(tree->size) + " " + (tree->owner == &player ? "1" : "0") + " " +
                 (tree->isDormant ? "1" : "0") + "\n";
        }
        std::vector<std::string> possibleMoves = getPossibleMoves(player);
        s += std::to_string(possibleMoves.size()) + "\n";
        for (auto& m : possibleMoves) s += m + "\n";
        return s;
    }

    std::vector<std::string> getPossibleMoves(Player& player) {
        std::vector<std::string> lines;
        lines.push_back("WAIT");
        std::vector<std::string> possibleSeeds, possibleGrows, possibleCompletes;
        if (player.waiting) return lines;
        int seedCost = getSeedCost(player);
        for (int index = 0; index < C::NCELLS; index++) {
            Tree* tree = trees[index];
            if (!tree || tree->owner != &player) continue;
            CubeCoord coord = board.coords[index];
            if (playerCanSeedFrom(player, *tree, seedCost)) {
                int N = tree->size;
                for (int x = -N; x <= +N; x++) {
                    for (int y = std::max(-N, -x - N); y <= std::min(+N, -x + N); y++) {
                        int z = -x - y;
                        CubeCoord targetCoord{coord.x + x, coord.y + y, coord.z + z};
                        int targetCell = board.cellIndex(targetCoord);
                        if (playerCanSeedTo(targetCell))
                            possibleSeeds.push_back("SEED " + std::to_string(index) + " " + std::to_string(targetCell));
                    }
                }
            }
            int growCost = getGrowthCost(*tree);
            if (growCost <= player.sun && !tree->isDormant) {
                if (tree->size == C::TREE_TALL) possibleCompletes.push_back("COMPLETE " + std::to_string(index));
                else if (C::ENABLE_GROW) possibleGrows.push_back("GROW " + std::to_string(index));
            }
        }
        for (auto* possibleList : {&possibleCompletes, &possibleGrows, &possibleSeeds}) {
            java::shuffle(*possibleList, random);
            lines.insert(lines.end(), possibleList->begin(), possibleList->end());
        }
        return lines;
    }

    bool playerCanSeedFrom(const Player& player, const Tree& tree, int seedCost) const {
        return C::ENABLE_SEED && seedCost <= player.sun && tree.size > C::TREE_SEED && !tree.isDormant;
    }

    bool playerCanSeedTo(int targetCell) const { // Cell.NO_CELL: index -1, not valid
        return targetCell >= 0 && board.richness[targetCell] != C::RICHNESS_NULL && !trees[targetCell];
    }

    void resetGameTurnData() {
        dyingTrees.clear();
        availableSun.clear();
        sentSeeds.clear();
        for (Player& p : players) {
            availableSun.push_back(p.sun);
            p.action = Action(); // Player.reset (message only goes to the view)
        }
        currentFrameType = nextFrameType;
    }

    int getGrowthCost(const Tree& targetTree) const {
        int targetSize = targetTree.size + 1;
        if (targetSize > C::TREE_TALL) return C::LIFECYCLE_END_COST;
        return getCostFor(targetSize, targetTree.owner);
    }

    int getSeedCost(const Player& player) const { return getCostFor(0, &player); }

    int getCostFor(int size, const Player* owner) const {
        int baseCost = C::TREE_BASE_COST[size];
        int sameTreeCount = 0;
        for (Tree* t : trees)
            if (t && t->size == size && t->owner == owner) sameTreeCount++;
        return baseCost + sameTreeCount;
    }

    // getCoordByIndex
    CubeCoord getCoordByIndex(int index) const {
        if (index < 0 || index >= C::NCELLS) throw GameException{"Cell " + std::to_string(index) + " not found"};
        return board.coords[index];
    }

    static std::string notEnoughSun(int cost, int sun) {
        return "Not enough sun. You need " + std::to_string(cost) + " but have " + std::to_string(sun);
    }

    void doGrow(Player& player, const Action& action) {
        CubeCoord coord = getCoordByIndex(action.targetId);
        int cell = board.cellIndex(coord);
        Tree* targetTree = trees[cell];
        if (!targetTree) throw GameException{"There is no tree on cell " + std::to_string(cell)};
        if (targetTree->owner != &player) throw GameException{"The tree on cell " + std::to_string(cell) + " is owned by opponent"};
        if (targetTree->isDormant)
            throw GameException{"Tree on cell " + std::to_string(cell) + " is dormant (has already been used this round)"};
        if (targetTree->size >= C::TREE_TALL)
            throw GameException{"Tree on cell " + std::to_string(cell) + " cannot grow more (max size is 3)."};
        int costOfGrowth = getGrowthCost(*targetTree);
        int currentSun = availableSun[player.index];
        if (currentSun < costOfGrowth) throw GameException{notEnoughSun(costOfGrowth, player.sun)};
        availableSun[player.index] = currentSun - costOfGrowth;
        targetTree->size++;
        gameSummaryManager.addGrowTree(player, cell);
        targetTree->isDormant = true;
    }

    void doComplete(Player& player, const Action& action) {
        CubeCoord coord = getCoordByIndex(action.targetId);
        int cell = board.cellIndex(coord);
        Tree* targetTree = trees[cell];
        if (!targetTree) throw GameException{"There is no tree on cell " + std::to_string(cell)};
        if (targetTree->owner != &player) throw GameException{"The tree on cell " + std::to_string(cell) + " is owned by opponent"};
        if (targetTree->size < C::TREE_TALL) throw GameException{"The tree on cell " + std::to_string(cell) + " is not large enough"};
        if (targetTree->isDormant)
            throw GameException{"Tree on cell " + std::to_string(cell) + " is dormant (has already been used this round)"};
        int costOfGrowth = getGrowthCost(*targetTree);
        int currentSun = availableSun[player.index];
        if (currentSun < costOfGrowth) throw GameException{notEnoughSun(costOfGrowth, player.sun)};
        availableSun[player.index] = currentSun - costOfGrowth;
        dyingTrees.push_back(coord);
        targetTree->isDormant = true;
    }

    void doSeed(Player& player, const Action& action) {
        CubeCoord targetCoord = getCoordByIndex(action.targetId);
        CubeCoord sourceCoord = getCoordByIndex(action.sourceId);
        int targetCell = board.cellIndex(targetCoord);
        int sourceCell = board.cellIndex(sourceCoord);
        if (trees[targetCell]) throw GameException{"There is already a tree on cell " + std::to_string(targetCell)};
        Tree* sourceTree = trees[sourceCell];
        if (!sourceTree) throw GameException{"There is no tree on cell " + std::to_string(sourceCell)};
        if (sourceTree->size == C::TREE_SEED) throw GameException{"The seed on " + std::to_string(sourceCell) + " cannot produce seeds"};
        if (sourceTree->owner != &player)
            throw GameException{"The tree on cell " + std::to_string(sourceCell) + " is owned by opponent"};
        if (sourceTree->isDormant)
            throw GameException{"Tree on cell " + std::to_string(sourceCell) + " is dormant (has already been used this round)"};
        int distance = sourceCoord.distanceTo(targetCoord);
        if (distance > sourceTree->size)
            throw GameException{"The tree on cell " + std::to_string(sourceCell) + " is too far from cell " +
                                std::to_string(targetCell) + " to plant a seed there"};
        if (board.richness[targetCell] == C::RICHNESS_NULL)
            throw GameException{"You can't plant a seed on cell " + std::to_string(targetCell)};
        int costOfSeed = getSeedCost(player);
        int currentSun = availableSun[player.index];
        if (currentSun < costOfSeed) throw GameException{notEnoughSun(costOfSeed, player.sun)};
        availableSun[player.index] = currentSun - costOfSeed;
        sourceTree->isDormant = true;
        sentSeeds.push_back({player.index, sourceCell, targetCell});
        gameSummaryManager.addPlantSeed(player, targetCell, sourceCell);
    }

    void giveSun() {
        int givenToPlayer[2] = {0, 0};
        for (int index = 0; index < C::NCELLS; index++) {
            Tree* tree = trees[index];
            if (!tree) continue;
            if (shadows[index] < 0 || shadows[index] < tree->size) {
                Player* owner = tree->owner;
                owner->sun += tree->size;
                givenToPlayer[owner->index] += tree->size;
            }
        }
        for (Player& player : players) {
            int given = givenToPlayer[player.index];
            if (given > 0) gameSummaryManager.addGather(player, given);
        }
    }

    void removeDyingTrees() {
        for (const CubeCoord& coord : dyingTrees) {
            int cell = board.cellIndex(coord);
            int points = nutrients;
            if (board.richness[cell] == C::RICHNESS_OK) points += C::RICHNESS_BONUS_OK;
            else if (board.richness[cell] == C::RICHNESS_LUSH) points += C::RICHNESS_BONUS_LUSH;
            Player* player = trees[cell]->owner;
            player->addScore(points);
            addTooltip(player->index, player->token() + " scores " + std::to_string(points) + " points");
            trees[cell] = nullptr;
            gameSummaryManager.addCutTree(*player, cell, points);
        }
    }

    void updateNutrients() {
        for (size_t i = 0; i < dyingTrees.size(); i++) nutrients = std::max(0, nutrients - 1);
    }

    void performGameUpdate() {
        turn++;
        switch (currentFrameType) {
        case GATHERING:
            gameSummaryManager.addRound(round);
            performSunGatheringUpdate();
            nextFrameType = ACTIONS;
            break;
        case ACTIONS:
            gameSummaryManager.addRound(round);
            performActionUpdate();
            if (allPlayersAreWaiting()) nextFrameType = SUN_MOVE;
            break;
        case SUN_MOVE:
            gameSummaryManager.addRoundTransition(round);
            performSunMoveUpdate();
            nextFrameType = GATHERING;
            break;
        default:
            break; // unreachable: currentFrameType is never INIT here
        }
        cg::gm.addToGameSummary(gameSummaryManager.toString());
        gameSummaryManager.clear();
        if (gameOver()) cg::gm.endGame();
        else cg::gm.setMaxTurns(turn + 1);
    }

    void performSunMoveUpdate() {
        round++;
        if (round < C::MAX_ROUNDS) {
            sunOrientation = (sunOrientation + 1) % 6;
            if (C::ENABLE_SHADOW) calculateShadows();
        }
    }

    void performSunGatheringUpdate() {
        for (Player& p : players) p.waiting = false;
        for (Tree* t : trees)
            if (t) t->isDormant = false;
        giveSun();
    }

    void performActionUpdate() {
        for (Player& player : players) {
            if (player.waiting) continue;
            try {
                const Action action = player.action;
                if (action.type == GROW) doGrow(player, action);
                else if (action.type == SEED) doSeed(player, action);
                else if (action.type == COMPLETE) doComplete(player, action);
                else {
                    player.waiting = true;
                    gameSummaryManager.addWait(player);
                }
            } catch (const GameException& e) {
                gameSummaryManager.addError(player.token() + ": " + e.message);
                player.waiting = true;
            }
        }
        if (seedsAreConflicting()) {
            gameSummaryManager.addSeedConflict(sentSeeds[0]);
        } else {
            for (const Seed& seed : sentSeeds) plantSeed(&players[seed.owner], seed.targetCell, seed.sourceCell);
            for (Player& player : players) player.sun = availableSun[player.index];
        }
        removeDyingTrees();
        updateNutrients();
    }

    bool seedsAreConflicting() const {
        std::vector<int> targets;
        for (auto& s : sentSeeds)
            if (std::find(targets.begin(), targets.end(), s.targetCell) == targets.end()) targets.push_back(s.targetCell);
        return sentSeeds.size() != targets.size();
    }

    bool allPlayersAreWaiting() const {
        int n = 0;
        for (const Player& p : players)
            if (p.waiting) n++;
        return n == 2;
    }

    void plantSeed(Player* player, int index, int fatherIndex) {
        Tree* seed = placeTree(player, index, C::TREE_SEED);
        seed->isDormant = true;
        seed->fatherIndex = fatherIndex;
    }

    Tree* placeTree(Player* player, int index, int size) {
        Tree* tree = &treeStore[index];
        *tree = Tree();
        tree->size = size;
        tree->owner = player;
        trees[index] = tree;
        return tree;
    }

    void onEnd() {
        std::vector<Player*> active = getActivePlayers();
        for (Player* player : active) player->addScore(player->sun / 3); // (int) Math.floor(sun / 3)
        std::vector<int> distinct;
        for (Player* player : getActivePlayers())
            if (std::find(distinct.begin(), distinct.end(), player->score) == distinct.end()) distinct.push_back(player->score);
        if (distinct.size() == 1) {
            for (Tree* tree : trees) {
                if (tree && tree->owner->active) {
                    tree->owner->bonusScore += 1;
                    tree->owner->addScore(1);
                }
            }
        }
    }

    bool gameOver() const { return getActivePlayers().size() <= 1 || round >= C::MAX_ROUNDS; }
};
static Game game;

// ----------------------------------------------------------------------------
// CommandManager.java
// ----------------------------------------------------------------------------
static void deactivatePlayer(Player& player, const std::string& message) {
    // escapeHTMLEntities, then AbstractMultiplayerPlayer#deactivate(reason) adds a tooltip
    player.deactivate(java::replace(java::replace(message, "&lt;", "<"), "&gt;", ">"));
    player.score = -1;
}

// "(?:\\s+(?<message>.*))?" then the end of the input (Matcher#matches)
static bool messageMatches(const std::u16string& s, size_t pos) {
    if (pos == s.size()) return true;
    if (!java::isRegexSpace(s[pos])) return false;
    while (pos < s.size() && java::isRegexSpace(s[pos])) pos++;
    for (; pos < s.size(); pos++)
        if (java::isLineSeparator(s[pos])) return false;
    return true;
}

// "\\d+" (ASCII digits); returns the end of the digit run
static size_t digits(const std::u16string& s, size_t pos) {
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
    return pos;
}

struct InvalidInput {
    std::string got; // UTF-8
};

// Integer.parseInt of the group s[a, b); InvalidInputException(Game.getExpected(), e.toString()) when it throws
static i32 parseInt(const std::u16string& s, size_t a, size_t b) {
    std::u16string group = s.substr(a, b - a);
    i32 v;
    if (!java::parseInt(group, v)) throw InvalidInput{java::numberFormatException(java::encodeUtf8(group))};
    return v;
}

static void parseCommands(Player& player, const std::u16string& command) {
    if (player.waiting) return;
    try {
        size_t p;
        // ^WAIT(?:\s+(?<message>.*))?
        if (java::startsWith(command, "WAIT") && messageMatches(command, 4)) {
            player.action.type = WAIT;
            return;
        }
        // ^GROW (?<targetId>\d+)(?:\s+(?<message>.*))?
        if (C::ENABLE_GROW && java::startsWith(command, "GROW ") && (p = digits(command, 5)) > 5 && messageMatches(command, p)) {
            i32 targetId = parseInt(command, 5, p);
            player.action = Action{GROW, 0, targetId};
            return;
        }
        // ^COMPLETE (?<targetId>\d+)(?:\s+(?<message>.*))?
        if (java::startsWith(command, "COMPLETE ") && (p = digits(command, 9)) > 9 && messageMatches(command, p)) {
            i32 targetId = parseInt(command, 9, p);
            player.action = Action{COMPLETE, 0, targetId};
            return;
        }
        // ^SEED (?<sourceId>\d+) (?<targetId>\d+)(?:\s+(?<message>.*))?
        if (C::ENABLE_SEED && java::startsWith(command, "SEED ")) {
            size_t a = digits(command, 5);
            if (a > 5 && a < command.size() && command[a] == ' ') {
                size_t b = digits(command, a + 1);
                if (b > a + 1 && messageMatches(command, b)) {
                    i32 sourceId = parseInt(command, 5, a);
                    i32 targetId = parseInt(command, a + 1, b);
                    player.action = Action{SEED, sourceId, targetId};
                    return;
                }
            }
        }
        throw InvalidInput{java::encodeUtf8(command)};
    } catch (const InvalidInput& e) {
        deactivatePlayer(player, std::string("Invalid Input: Expected ") + C::EXPECTED + " but got '" + e.got + "'");
        gameSummaryManager.addPlayerBadCommand(player, C::EXPECTED, e.got);
        gameSummaryManager.addPlayerDisqualified(player);
    }
}

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
static void handlePlayerCommands() {
    for (Player* player : getActivePlayers()) {
        if (player->waiting) continue;
        if (player->timeout) {
            // TimeoutException
            deactivatePlayer(*player, "Timeout!");
            gameSummaryManager.addPlayerTimeout(*player);
            gameSummaryManager.addPlayerDisqualified(*player);
        } else {
            parseCommands(*player, player->outputs[0]);
        }
    }
}

// player.sendInputLine for each line of a text
static void sendInput(Player& player, const std::string& text) {
    size_t off = 0, nl;
    while ((nl = text.find('\n', off)) != std::string::npos) {
        player.sendInputLine(text.substr(off, nl - off));
        off = nl + 1;
    }
}

static void gameTurn() {
    game.resetGameTurnData();
    if (game.currentFrameType == ACTIONS) {
        for (Player* player : getActivePlayers()) {
            if (!player->waiting) {
                sendInput(*player, game.getCurrentFrameInfoFor(*player));
                player->execute();
            }
        }
        handlePlayerCommands();
    }
    game.performGameUpdate();
}

struct Spring2021Referee : cg::Referee {
    void init() override {
        cg::gm.setFirstTurnMaxTime(C::FIRST_TURN_MAX_TIME);
        cg::gm.setTurnMaxTime(C::TURN_MAX_TIME);
        cg::gm.setFrameDuration(500);
        game.init(cg::gm.getSeed());
        for (Player* player : getActivePlayers()) sendInput(*player, game.getGlobalInfoFor(*player));
    }
    void gameTurn(int turn) override {
        ::gameTurn();
        if (trace)
            fprintf(trace, "  turn %d round %d frame %d sun %d %d score %d %d nutrients %d\n", turn, game.round, (int)game.currentFrameType, players[0].sun,
                    players[1].sun, players[0].score, players[1].score, game.nutrients);
    }
    void onEnd() override { game.onEnd(); }
};

int main(int argc, char** argv) {
    static Spring2021Referee referee;
    cg::Options options;
    options.maxPlayers = 2; // not reproduced: the jar would start a 3rd/4th player and fail at turn 2
    options.agent.stderrMode = cg::Stderr::Jar;
    options.agent.firstTurnHeadStartMs = C::BOT_STARTUP_GRACE_MS;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
