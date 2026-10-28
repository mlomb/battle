// Ultimate Tic-Tac-Toe (CodinGame, 2018-02, "tic-tac-toe") - headless C++ referee, port of
// https://github.com/CodinGame/game-ultimate-tictactoe (commit 30656ef, 2025-04-18, = HEAD; Maven project at
// the repository root, pom artifactId tictactoe; the game's rules last changed in 99229d6, 2018-03-13),
// CodinGame game engine 2.7, 2 players, the rules of the top league (league level 2: the 9x9 game).
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints the 2 scores, one per line, then "seed=N" to stdout like the jar. When the Java referee would
// have failed, prints nothing and exits with code 1, like the jar (engine 2.x has no FAIL command: its
// runner waits 150 s for the dead referee, then throws out of the game). Exit code 1 also when the jar's
// command line interface fails (bad -seed, a bot that cannot be started). -l writes the trace of
// common/engine.h (one line per bot execution, game summary and tooltip block, then "END OK|<why the
// game failed>"; the view frames are counted, not built, see below).
//
// Leagues: the referee tests getLeagueLevel() == 1 (Wood 1: one 3x3 board, 9 turns; a win scores 10) and
// == 2 (the small boards exist only then) and nothing else: the game has the two leagues of its
// config/level1 and config/level2, and CodinGame's leagues from Bronze up play level 2. Any other level
// crashes the referee's init (getValidActions reads the small boards, which were not created: a
// NullPointerException; the jar would print nothing after 150 s). So the jar is built for level 2
// (referees/cg-builder: `docker run -e LEAGUE_LEVEL=2 ...`, CommandLineInterface-v2.java reads it from the
// jar instead of the usual 19), and this port implements level 2 only.
//
// The game: a 9x9 grid of 9 small 3x3 boards; players alternate (turn % 2: the first player always starts,
// whatever the seed), one move per turn ("row col", 0-8). The first player's first input is "-1 -1",
// then every input is the opponent's last move, the number of valid moves and the valid moves, one per
// line, in the order of new java.util.Random(seed)'s Collections.shuffle of the list the referee builds:
// the empty cells of the small board at the position of the last move inside its own small board (row %
// 3, col % 3), or, when that board is won or full (and at the first turn), the empty cells of every board
// not won (boards row by row, cells row by row). Winning a small board (rows, columns, then diagonals)
// scores 1 (the board takes no more moves; a full board without a line is nobody's); winning three small
// boards in a line of the big board sets the winner's score to 10 and ends the game; with no valid move
// left the game ends and the scores are the counts of small boards won (the higher count wins, equal
// counts draw). The bots get 1000 ms at their first turn, 100 ms after. The 81-turn limit never ends a
// game (81 cells: at the latest, the 81st move leaves no valid move).
//
// Engine 2.7 (common/engine.h, cg::Options::engine25): engine 2.5's core and runner except (a) a 512 KB
// quota on one turn's game summary (unreachable here: two lines of < 80 chars per turn), (b) public
// CommandLinePlayerAgent and javadoc changes; module-entities 2.7 is 2.2's plus BufferedGroup (not used).
// So: the runner's split of the outputs (an empty line gives the referee no line: "Invalid output."), no
// summary quotas, turns numbered from 0 (the port passes turn - 1).
//
// The Java referee is followed closely. Notable behaviours reproduced here:
//   - the output line is matched (whole) against
//     (?<row>-?[0-9]{1})\s+(?<col>-?[0-9]{1})?(?:\s+(?<message>.+))? and both numbers parsed with
//     Integer.parseInt: a match with no col (e.g. "4 ", "4  hello") or no match at all is "Invalid
//     output."; the message is what follows the whitespace after col, or, when that whitespace ends the
//     line, its last char if it has 2+ chars ("4 4  " has the message " "; "4 4 " does not match:
//     invalid); -0 is 0; out of range single digits ("9 9", "-1 -1") and taken cells are "Invalid
//     action." (not in the valid list), after the "played" summary line. The message (" " without one)
//     goes to the summary as is when shorter than 20 UTF-16 chars, else cut to 17 + "...".
//   - the eliminated player (invalid output or action, timeout) scores -1, the other keeps its count of
//     small boards won; the win sets 10 without touching the other player's count.
//   - 3 or 4 players: the referee plays them in turn (turn % players); the third player's first valid
//     move crashes it (its symbol image, images[2] of 2): no output, exit code 1, like the jar (after
//     150 s); a third player eliminated before that ends the game normally.
// Not reproduced:
//   - the view (GraphicEntityModule 2.7 entities: the grids, the symbols, the HUD message of the bot): no
//     RNG, no crash path, and it cannot reach the engine's limits: bot text reaches it only in the HUD
//     message (20 chars at most); measured with the jar, games make 3-19 KB of view data (82 frames at
//     most) and the largest frame is the first one (1825 chars; 2545 with 4 players), against 1 MB and
//     30000 bytes (100000 for the first). So the -l trace has the frame count only.
//   - the jar's wall clock (it polls bot output one byte at a time, with 1 ms sleeps; a referee crash
//     costs it 150 s), and the game log.
// Validated (2026-10-25) with battle referee-diff against the jar on 4065 games (31 bots, every ordered
// pairing including self-play plus targeted pairs, seeds from 1, 1000001, 987654321 and 5000000000, and
// 153 games picked by rule coverage: 0-0 ties of nine drawn boards, 81-move games, rare parses): all
// identical, no mismatch, no flake; in direct mode (jar game log vs -l trace: every execution, frame
// count, summaries, tooltips, stdout, exit code) on 574 games, including raw line endings, invalid UTF-8,
// U+2028 / U+0085 / U+2029 (both fail), empty lines, outputs at the runner's 10000-byte limit, seeds 0,
// -7, 2^63 - 1, -2^63, -timeout, and 3- and 4-player games; the output parsing checked against
// java.util.regex on 2.8 million lines; every rule branch reached (instrumented copy).
#include "common/engine.h"

using java::String;

static String A(const std::string& s) { return java::ascii(s); }

struct Player;

struct TimeoutException {};
struct InvalidAction {
    String message;
};

struct Action {
    int row = 0, col = 0;
    Player* player = nullptr;
    String toString() const { return java::str(row) + u" " + java::str(col); }
    bool equals(const Action& o) const { return col == o.col && row == o.row; }
};

struct Player : cg::Player {
    String messageText = u" "; // the HUD message Text (created with " ")

    int getExpectedOutputLines() const override { return 1; }

    // PLAYER_ACTION_PATTERN = (?<row>-?[0-9]{1})\s+(?<col>-?[0-9]{1})?(?:\s+(?<message>.+))?, matches():
    // a line the referee reads has no line terminator ('.' matches every char of it), so the backtracking
    // finds a match with a col exactly when row, the longest whitespace run after it and col are there,
    // followed by the end or by whitespace then the message (any non-empty rest; or, when the whitespace
    // reaches the end, its last char if it has 2+ chars). Every other way of matching has no col: then
    // Integer.parseInt(null) throws, like no match at all: "Invalid output.".
    Action getAction() {
        if (timeout) throw TimeoutException{};
        if (outputs.empty()) throw InvalidAction{u"Invalid output."}; // getOutputs().get(0)
        const String& s = outputs[0];
        size_t n = s.size(), p = 0;
        auto digitAt = [&](size_t& q, int& v) {
            size_t k = q;
            bool neg = k < n && s[k] == u'-';
            if (neg) k++;
            if (k >= n || s[k] < u'0' || s[k] > u'9') return false;
            v = neg ? -(int)(s[k] - u'0') : (int)(s[k] - u'0');
            q = k + 1;
            return true;
        };
        int row, col;
        if (!digitAt(p, row)) throw InvalidAction{u"Invalid output."};
        size_t w = p;
        while (w < n && java::isRegexSpace(s[w])) w++;
        if (w == p) throw InvalidAction{u"Invalid output."};
        p = w;
        if (!digitAt(p, col)) throw InvalidAction{u"Invalid output."};
        bool hasMessage = false;
        String msg;
        if (p < n) {
            if (!java::isRegexSpace(s[p])) throw InvalidAction{u"Invalid output."};
            size_t q = p;
            while (q < n && java::isRegexSpace(s[q])) q++;
            if (q < n) {
                msg = s.substr(q);
            } else if (q - p >= 2) {
                msg = s.substr(n - 1);
            } else {
                throw InvalidAction{u"Invalid output."};
            }
            hasMessage = true;
        }
        if (!hasMessage) messageText = u" ";
        else if (msg.size() < 20) messageText = msg;
        else messageText = msg.substr(0, 17) + u"...";
        Action a;
        a.player = this;
        a.row = row;
        a.col = col;
        return a;
    }
};

static Player players[4];

// TicTacToe: one 3x3 board
struct TicTacToe {
    int grid[3][3] = {};
    int winner = 0;

    std::vector<Action> getValidActions() const {
        std::vector<Action> v;
        if (winner == 0)
            for (int x = 0; x < 3; x++)
                for (int y = 0; y < 3; y++)
                    if (grid[x][y] == 0) {
                        Action a;
                        a.row = x;
                        a.col = y;
                        v.push_back(a);
                    }
        return v;
    }

    int play(const Action& action) {
        if (action.row < 0 || action.row >= 3 || action.col < 0 || action.col >= 3 || grid[action.row][action.col] != 0)
            throw InvalidAction{u"Invalid move!"};
        grid[action.row][action.col] = action.player->getIndex() + 1;
        winner = checkWinner();
        return winner;
    }

    int checkWinner() const {
        for (int i = 0; i < 3; i++) {
            if (grid[i][0] > 0 && grid[i][0] == grid[i][1] && grid[i][0] == grid[i][2]) return grid[i][0];
            if (grid[0][i] > 0 && grid[0][i] == grid[1][i] && grid[0][i] == grid[2][i]) return grid[0][i];
        }
        if (grid[0][0] > 0 && grid[0][0] == grid[1][1] && grid[0][0] == grid[2][2]) return grid[0][0];
        if (grid[2][0] > 0 && grid[2][0] == grid[1][1] && grid[2][0] == grid[0][2]) return grid[2][0];
        return 0;
    }
};

// TicTacToeGrid: the board and its view (only the part of drawPlay that can throw)
struct TicTacToeGrid : TicTacToe {
    int play(const Action& action) {
        int w = TicTacToe::play(action);
        // drawPlay: images[action.player.getIndex()] of { "cross.png", "circle.png" }
        int index = action.player->getIndex();
        if (index >= 2) throw java::Exception{java::arrayIndexOutOfBoundsException(index, 2)};
        return w;
    }
};

struct UtttReferee : cg::Referee {
    TicTacToeGrid masterGrid;
    TicTacToeGrid smallGrids[3][3];
    bool hasLastAction = false;
    Action lastAction;
    std::vector<Action> validActions;
    java::Random random;

    void init() override {
        random = java::Random(cg::gm.getSeed());
        // drawBackground(), drawHud(), drawGrids(): the view (the small grids exist at level 2)
        cg::gm.setFrameDuration(600);
        cg::gm.setMaxTurns(9 * 9); // level 1: 9
        validActions = getValidActions();
    }

    void sendInputs(Player& player, const std::vector<Action>& actions) {
        if (hasLastAction) player.sendInputLine(lastAction.toString());
        else player.sendInputLine(std::string("-1 -1"));
        player.sendInputLine(std::to_string(actions.size()));
        for (const Action& a : actions) player.sendInputLine(a.toString());
    }

    void setWinner(Player& player) {
        cg::gm.addToGameSummary(cg::GameManager::formatSuccessMessage(player.getNicknameToken() + u" won!"));
        player.setScore(10);
        endGame();
    }

    // level 2 (the activate / deactivate / animate calls are the view's)
    std::vector<Action> getValidActions() {
        std::vector<Action> v;
        if (hasLastAction) {
            int br = lastAction.row % 3, bc = lastAction.col % 3;
            for (const Action& a : smallGrids[br][bc].getValidActions()) {
                Action b;
                b.row = br * 3 + a.row;
                b.col = bc * 3 + a.col;
                v.push_back(b);
            }
        }
        if (v.empty())
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++)
                    for (const Action& a : smallGrids[row][col].getValidActions()) {
                        Action b;
                        b.row = row * 3 + a.row;
                        b.col = col * 3 + a.col;
                        v.push_back(b);
                    }
        java::shuffle(v, random);
        return v;
    }

    void gameTurn(int engineTurn) override {
        int turn = engineTurn - 1; // engine 2.x numbers the turns from 0
        Player& player = *static_cast<Player*>(cg::gm.getPlayer(turn % cg::gm.getPlayerCount()));

        // "Temporary fix (will be fixed in the SDK)"
        if (turn == 0 || turn == 1) cg::gm.setTurnMaxTime(1000);
        else cg::gm.setTurnMaxTime(100);

        sendInputs(player, validActions);
        player.execute();

        try {
            Action action = player.getAction();
            cg::gm.addToGameSummary(A("Player ") + player.getNicknameToken() + A(" played (") + java::str(action.row) + u" " + java::str(action.col) + u") " +
                                    player.messageText);

            bool valid = false;
            for (const Action& a : validActions) valid = valid || a.equals(action);
            if (!valid) throw InvalidAction{u"Invalid action."};

            lastAction = action;
            hasLastAction = true;

            TicTacToeGrid& grid = smallGrids[action.row / 3][action.col / 3];
            Action local;
            local.player = action.player;
            local.row = action.row % 3;
            local.col = action.col % 3;
            if (grid.play(local) > 0) {
                player.setScore(player.getScore() + 1);
                cg::gm.addTooltip(player.getIndex(), player.getNicknameToken() + u" has won one cell");
                // grid.hide(): the view
                Action big;
                big.player = action.player;
                big.row = action.row / 3;
                big.col = action.col / 3;
                if (masterGrid.play(big) > 0) setWinner(player);
            }

            validActions = getValidActions();
            if (validActions.empty()) endGame();
        } catch (const TimeoutException&) {
            cg::gm.addToGameSummary(cg::GameManager::formatErrorMessage(player.getNicknameToken() + u" timeout!"));
            player.deactivate(player.getNicknameToken() + u" timeout!");
            player.setScore(-1);
            endGame();
        } catch (const InvalidAction& e) {
            cg::gm.addToGameSummary(cg::GameManager::formatErrorMessage(player.getNicknameToken() + u" eliminated: " + e.message));
            player.deactivate(e.message);
            player.setScore(-1);
            endGame();
        }
    }

    // gameManager.endGame(), then the losing player's HUD fades (the view)
    void endGame() { cg::gm.endGame(); }
};

static UtttReferee referee;

int main(int argc, char** argv) {
    cg::Options options;
    options.engine25 = true;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
