// Tron Battle (CodinGame, 2014, "tron-battle") - headless C++ referee. CodinGame never published its referee: the rules
// are those of the unofficial C# referee Tron/TronReferee.cs of https://github.com/eulerscheZahl/RefereeCollection
// (commit 9c31fb8), with CodinGame's statement and its live behaviour, as its replays show it, where they differ. 2 to 4
// players. Deterministic: the seed decides the start cells.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 <cmd> -p4 <cmd>] -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player (the number of players ranked below it), then "seed=N". -l writes a trace: per turn
// "T<k> P<id> IN [<the input lines>]" then "T<k> P<id> OUT [<the line>]" or "T<k> P<id> TIMEOUT"; at the end
// "RANKING <ids, the winner first>" and "END OK".
//
// The game: a 30 x 20 grid. Each player starts on a cell drawn with java.util.Random(seed), player 0 first: nextInt(600)
// = x + 30 y, drawn again while the cell is taken. The players move in turn, in the order of their index, one cell per
// round, each seeing the moves made before it. A player's input: "N P" (the number of players, its index), then one
// line per player "X0 Y0 X1 Y1", its start cell and its head, or "-1 -1 -1 -1" for a player out. Its output: one line
// whose first word (words are separated by spaces) is UP, DOWN, LEFT or RIGHT in any ASCII case; the words after it do
// not matter. A player loses on any other line (an empty or blank one too), when it times out, or when its move leaves
// the grid or enters a trail (its own included); its trail is then removed. The game ends as soon as one player is left,
// even in the middle of a round. The ranking: the last player left, then the others in the reverse order of their
// losses.
//
// Where the rules come from: the C# referee, except where CodinGame differs. CodinGame's statement and its example put
// the start cell in X0 Y0 (the C# referee sends the previous cell). CodinGame's replays (692 games from every league,
// with 2, 3 and 4 players) show that an empty line loses (the C# referee crashes) and that the game ends as soon as one
// player is left (the C# referee finishes the round); they show no other difference. CodinGame does not seed its start
// cells (they are often symmetric), and its replays only show exact words. The start cells are drawn with one
// nextInt(600) each because x = nextInt(30), y = nextInt(20) gives player 0 too few rows over consecutive seeds.
//
// The runner is CodinGame's (common/agent.h): a line ends at '\n' or '\r', at most 10000 bytes; no complete line in time
// is a timeout. 100 ms per turn (CodinGame's statement), 1000 ms for a player's first turn; -timeout <ms> replaces them.
//
// Validation: CodinGame's 692 replays played through this CLI with their start cells (built with -DTRON_BATTLE_STARTS=1,
// which reads them from the environment variable TRON_STARTS, "x0 y0 x1 y1 ..."): every outcome, trail, ranking and
// score identical. The previous port of the C# referee (itself checked against the C# program), given the same start
// cells, on 4426 games of 2 to 4 bots that ignore X0 Y0 (odd bytes and line ends included): the same games, but for
// words with U+0131 (dotless i), which the C# referee takes for RIGHT; X0 Y0 always the start cell. The start cells:
// checked against java.util.Random.
#include "common/engine.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#ifndef TRON_BATTLE_STARTS
#define TRON_BATTLE_STARTS 0
#endif

namespace tron {

const int WIDTH = 30, HEIGHT = 20;
const int FIRST_TURN_MS = 1000, TURN_MS = 100;
const char* const MOVES[4] = {"UP", "DOWN", "LEFT", "RIGHT"};
const int DX[4] = {0, 0, -1, 1}, DY[4] = {-1, 1, 0, 0};

struct Player {
    int startX = 0, startY = 0, x = 0, y = 0;
    bool alive = true;
};

// The move of a line (an index of MOVES), -1 for none: its first word, compared ignoring ASCII case only
inline int parseMove(const std::string& line) {
    for (const std::string& word : java::split(line, ' ')) {
        if (word.empty()) continue;
        for (int d = 0; d < 4; d++)
            if (java::regexMatchesIgnoreCase(word, MOVES[d])) return d;
        return -1;
    }
    return -1;
}

inline void play(i64 seed, int n) {
    cg::detail::Runner& r = cg::detail::runner;
    int grid[WIDTH][HEIGHT] = {}; // 0, or the index + 1 of the player whose trail is there
    std::vector<Player> players(n);
#if TRON_BATTLE_STARTS
    std::istringstream starts(getenv("TRON_STARTS")); // validation only
#endif
    java::Random random(seed);
    for (int id = 0; id < n; id++) {
        Player& p = players[id];
#if TRON_BATTLE_STARTS
        starts >> p.x >> p.y;
#else
        do {
            int cell = random.nextInt(WIDTH * HEIGHT);
            p.x = cell % WIDTH;
            p.y = cell / WIDTH;
        } while (grid[p.x][p.y] != 0);
#endif
        p.startX = p.x;
        p.startY = p.y;
        grid[p.x][p.y] = id + 1;
    }

    std::vector<int> losses; // in the order of the losses
    int alive = n;
    // round after round, the players in the order of their index; all of them play in the first round, so turns 1 to n
    // are their first turns
    for (int id = 0, turn = 1; alive > 1; id = (id + 1) % n) {
        Player& p = players[id];
        if (!p.alive) continue;
        std::vector<std::string> lines = {std::to_string(n) + " " + std::to_string(id)};
        for (const Player& q : players)
            lines.push_back(q.alive ? std::to_string(q.startX) + " " + std::to_string(q.startY) + " " + std::to_string(q.x) + " " + std::to_string(q.y)
                                    : "-1 -1 -1 -1");
        std::string input;
        for (const std::string& l : lines) input += l + "\n";
        if (cg::trace) {
            fprintf(cg::trace, "T%d P%d IN [", turn, id);
            for (size_t i = 0; i < lines.size(); i++) fprintf(cg::trace, "%s\"%s\"", i ? "," : "", lines[i].c_str());
            fprintf(cg::trace, "]\n");
        }
        r.agents[id]->sendInput(input);

        bool first = turn <= n;
        std::string line;
        int d = -1;
        if (r.agents[id]->getOutput(1, cg::turnTimeoutMs(first ? FIRST_TURN_MS : TURN_MS, first, cg::turnTimeout), first, line)) {
            line.pop_back(); // its line end
            if (cg::trace) fprintf(cg::trace, "T%d P%d OUT [%s]\n", turn, id, java::traceJson(java::decodeUtf8(line)).c_str());
            d = parseMove(line);
        } else if (cg::trace) {
            fprintf(cg::trace, "T%d P%d TIMEOUT\n", turn, id);
        }
        turn++;
        int x = d < 0 ? -1 : p.x + DX[d], y = d < 0 ? -1 : p.y + DY[d];
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT && grid[x][y] == 0) {
            grid[x][y] = id + 1;
            p.x = x;
            p.y = y;
            continue;
        }
        p.alive = false;
        alive--;
        losses.push_back(id);
        for (auto& column : grid)
            for (int& cell : column)
                if (cell == id + 1) cell = 0;
    }

    std::vector<int> ranking;
    for (int id = 0; id < n; id++)
        if (players[id].alive) ranking.push_back(id);
    ranking.insert(ranking.end(), losses.rbegin(), losses.rend());
    if (cg::trace) {
        fprintf(cg::trace, "RANKING");
        for (int id : ranking) fprintf(cg::trace, " %d", id);
        fprintf(cg::trace, "\n");
    }
    for (int place = 0; place < n; place++) r.scores[ranking[place]] = n - 1 - place;
    r.uinput.push_back("seed=" + std::to_string(seed));
    r.done = true;
}

} // namespace tron

int main(int argc, char** argv) { return cg::runCli(argc, argv, cg::Options(), tron::play); }
