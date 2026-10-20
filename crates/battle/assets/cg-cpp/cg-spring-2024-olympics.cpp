// Summer Challenge 2024 "Olymbits" - headless C++ referee, port of
// https://github.com/CodinGame/SummerChallenge2024Olymbits (commit 6529670) running on the
// CodinGame game engine 4.5.0 (core + runner), highest league (the 4 mini games), 3 players.
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -p3 "<bot3 cmd>" [-p4 "<cmd>"] -seed <N> [-l <file>]
// Prints one score per player, one per line, then "seed=<N>" (like the jar). When the Java referee
// fails (see below) it prints "null" for every player and exits with code 0, like the jar does.
// -l writes a trace with one line per player execution:
//   T<turn> P<player> OUT ["<line>"]      (JSON, the line as the runner received it)
//   T<turn> P<player> TIMEOUT
//
// The Java code is followed closely: same data structures, iteration orders and Java library
// semantics. Notable Java behaviours reproduced here:
//   - gameManager.getRandom() is SecureRandom("SHA1PRNG") seeded with setSeed(long). SecureRandom
//     ignores setSeed(0): with -seed 0 the jar self-seeds from system entropy, and so do we.
//     Consumers: Random.nextInt(bound) (rejection loop), nextBoolean, nextDouble,
//     Collections.shuffle; DoubleStream.sum is the compensated (Kahan) sum.
//   - rankings: stable sort on Double.compare, ties detected with Double.equals
//   - Agent#getOutput: '\r' and '\n' end a line ("\r\n" counts once, even across turns), at most
//     10000 bytes per turn, 1000 ms for the first turn of each player and 50 ms afterwards; the
//     bytes are decoded like new String(bytes, UTF_8) in JDK 26 (same U+FFFD replacements)
//   - the referee runs behind the runner's text protocol and reads its commands with a
//     java.util.Scanner, whose nextLine() also ends lines at U+0085, U+2028 and U+2029. A player
//     line containing one of them desynchronizes the protocol: the rest of the line is read as the
//     next command (usually "Error in data sent to referee", a crash; a crafted line can even
//     inject commands). The protocol is emulated line by line, so all of this is reproduced.
//   - the runner reads every referee command into a 30000-byte buffer (100000 on its first read).
//     A SUMMARY or TOOLTIP command holding long invalid player lines (U+FFFD takes 3 bytes in
//     UTF-8) overflows it and the game fails.
//   - when the referee fails or the protocol deadlocks, the runner stops: no scores ("null")
//   - 2 players crash the Java referee in the viewer module during init, 4 players in the first
//     turn (MiniGame.fillRegisters overflows); both are reproduced
// Not reproduced (documented): a Turkish default locale (toUpperCase of "right"), and agents
// flooding stderr (the jar reads at most 4 KB of agent stderr per turn; we discard it).

#include "common/engine.h"

#include <array>
#include <cfloat>

using java::String;

// ----------------------------------------------------------------------------
// Action.java, Player.java
// ----------------------------------------------------------------------------
enum Action { UP = 0, DOWN = 1, LEFT = 2, RIGHT = 3 }; // Action.values() order
static const int NO_ACTION = -1;                        // null
static const char* const ACTION_NAMES[4] = {"UP", "DOWN", "LEFT", "RIGHT"};

struct Player : cg::Player {
    // Player (the message of a command is stored by the Java referee but never used)
    int action = NO_ACTION;
    std::vector<std::array<int, 3>> medals;

    void init(int gameCount) { medals.assign(gameCount, {0, 0, 0}); }

    int getPoints() const {
        int p = 1;
        for (size_t i = 0; i < medals.size(); ++i) p *= (3 * medals[i][0] + medals[i][1]);
        return p;
    }

    int getExpectedOutputLines() const override { return 1; }

    void reset() { action = NO_ACTION; }
    // getMedalsTotal/getScoreText only feed the end screen and the metadata
};

static std::deque<Player> players; // gameManager.getPlayers()

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

static void addToGameSummary(const String& s) { cg::gm.addToGameSummary(s); }

// AbstractMultiplayerPlayer.deactivate(String)
static void deactivate(Player& p, const String& reason) { p.deactivate(reason); }

// ----------------------------------------------------------------------------
// MiniGame.java and the four mini games
// ----------------------------------------------------------------------------
static int PLAYER_COUNT = 3;          // Game.PLAYER_COUNT
static const int REGISTER_COUNT = 7;  // Game.REGISTER_COUNT
static const bool EARLY_RACE_END = true;

// MiniGame.fillRegisters
static std::vector<int> fillRegisters(std::initializer_list<const std::vector<int>*> arrays) {
    std::vector<int> registers(REGISTER_COUNT, 0);
    int idx = 0;
    for (const std::vector<int>* array : arrays) {
        for (int integer : *array) {
            if (idx >= REGISTER_COUNT)
                throw java::Exception{java::arrayIndexOutOfBoundsException(idx, REGISTER_COUNT)};
            registers[idx++] = integer;
        }
    }
    return registers;
}

// MiniGame.createRankings: entries in the map's iteration order (TreeMap and HashMap<Integer> with
// keys 0..n-1 both iterate by increasing key)
static std::vector<int> createRankings(const std::vector<std::pair<int, double>>& scoreByPlayer) {
    std::vector<int> rankings(PLAYER_COUNT, 0);
    std::vector<std::pair<int, double>> sorted = scoreByPlayer;
    // stream().sorted(comparingDouble(getValue).reversed())
    java::sort(sorted, [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return java::compareDouble(b.second, a.second); });
    int rank = 0;
    int lastRank = 0;
    bool hasLastScore = false;
    double lastScore = 0;
    for (auto& e : sorted) {
        int currentRank = rank;
        if (hasLastScore && java::equalsDouble(lastScore, e.second)) currentRank = lastRank;
        lastRank = currentRank;
        rankings[e.first] = currentRank;
        rank++;
        hasLastScore = true;
        lastScore = e.second;
    }
    return rankings;
}

struct MiniGame {
    std::string type;
    bool shouldReset = false, resetting = false;
    virtual ~MiniGame() {}
    virtual void reset(java::SHA1PRNG& random) = 0;
    virtual std::string getGPU() = 0;
    virtual std::vector<int> getRegisters() = 0;
    virtual void tick(const std::vector<int>& actions) = 0;
    virtual bool isGameOver() = 0;
    virtual std::vector<int> getRankings() = 0;
    virtual std::string getName() = 0;
    // the accesses of Serializer.serializeFrameData that can throw (the view data itself is not
    // needed: it never reaches the players and stays far below the engine quotas)
    virtual void checkViewSerialization() {}
};

struct HurdleRace : MiniGame {
    static const int STUN_DURATION = 2;
    std::string map;
    std::vector<int> positions, stunTimers;
    std::vector<bool> dead, jumped;
    std::vector<int> finished;
    int rank = 0;

    HurdleRace()
        : positions(PLAYER_COUNT, 0), stunTimers(PLAYER_COUNT, 0), dead(PLAYER_COUNT, false), jumped(PLAYER_COUNT, false),
          finished(PLAYER_COUNT, 0) {
        type = "hurdles";
    }

    void reset(java::SHA1PRNG& random) override {
        int startStretch = 3 + random.nextInt(5);
        int hurdles = 3 + random.nextInt(4);
        int length = 30;
        std::string sb;
        for (int i = 0; i < startStretch; ++i) sb += '.';
        for (int i = 0; i < hurdles; ++i) {
            if (random.nextBoolean()) sb += "#....";
            else sb += "#...";
        }
        while ((int)sb.size() < length) sb += ".";
        map = sb.substr(0, length - 1) + ".";
        for (int i = 0; i < PLAYER_COUNT; ++i) {
            stunTimers[i] = 0;
            positions[i] = 0;
            finished[i] = -1;
            rank = 0;
            jumped[i] = false;
        }
    }

    std::string getGPU() override { return map; }

    std::vector<int> getRegisters() override { return fillRegisters({&positions, &stunTimers}); }

    void tick(const std::vector<int>& actions) override {
        int maxX = (int)map.size() - 1;
        int countFinishes = 0;
        for (int i = 0; i < (int)actions.size(); ++i) {
            jumped[i] = false;
            int a = actions[i];
            if (a == NO_ACTION) {
                dead[i] = true;
                continue;
            }
            if (stunTimers[i] > 0) {
                stunTimers[i] -= 1;
                continue;
            }
            if (finished[i] > -1) {
                continue;
            }
            int moveBy = 0;
            bool jump = false;
            switch (a) {
            case DOWN:
                moveBy = 2;
                break;
            case LEFT:
                moveBy = 1;
                break;
            case RIGHT:
                moveBy = 3;
                break;
            case UP:
                moveBy = 2;
                jump = true;
                jumped[i] = true;
                break;
            }
            for (int x = 0; x < moveBy; ++x) {
                positions[i] = std::min(maxX, positions[i] + 1);
                if (map[positions[i]] == '#' && !jump) {
                    stunTimers[i] = STUN_DURATION;
                    break;
                }
                if (positions[i] == maxX && finished[i] == -1) {
                    finished[i] = rank;
                    countFinishes++;
                    break;
                }
                jump = false;
            }
        }
        rank += countFinishes;
    }

    bool isGameOver() override {
        int count = 0;
        for (int i = 0; i < (int)finished.size(); ++i) {
            if (finished[i] > -1 && EARLY_RACE_END) {
                return true;
            }
            if (finished[i] > -1 || dead[i]) {
                count++;
            }
        }
        return count >= 2;
    }

    std::vector<int> getRankings() override {
        if (EARLY_RACE_END) {
            std::vector<std::pair<int, double>> scoreByPlayer; // TreeMap
            for (int i = 0; i < (int)positions.size(); ++i) scoreByPlayer.push_back({i, (double)(dead[i] ? -1 : positions[i])});
            return createRankings(scoreByPlayer);
        }
        std::vector<int> rankings(PLAYER_COUNT, 0);
        for (int i = 0; i < (int)finished.size(); ++i) rankings[i] = finished[i] == -1 ? rank : finished[i];
        return rankings;
    }

    std::string getName() override { return "Hurdle Race"; }
};

struct Archery : MiniGame {
    std::vector<std::vector<int>> cursors; // List<int[2]>
    std::vector<int> wind;                 // LinkedList<Integer>
    bool arrows = false;
    std::vector<bool> dead;

    Archery() {
        for (int i = 0; i < PLAYER_COUNT; ++i) cursors.push_back(std::vector<int>(2, 0));
        dead.assign(PLAYER_COUNT, false);
        type = "archery";
    }

    std::vector<int>& cursor(int i) {
        if (i >= (int)cursors.size()) throw java::Exception{java::indexOutOfBoundsException(i, cursors.size())}; // List.get
        return cursors[i];
    }

    int randomChoice(java::SHA1PRNG& random, const std::vector<double>& coeffs) {
        double rand = random.nextDouble();
        double total = java::sum(coeffs);
        double b = 1 / total;
        std::vector<double> weights;
        for (double v : coeffs) weights.push_back(v * b);
        double cur = 0;
        for (int i = 0; i < (int)weights.size(); ++i) {
            cur += weights[i];
            if (cur >= rand) {
                return i;
            }
        }
        return 0;
    }

    void reset(java::SHA1PRNG& random) override {
        int x = 5 + random.nextInt(5);
        x *= random.nextBoolean() ? 1 : -1;
        int y = 5 + random.nextInt(5);
        y *= random.nextBoolean() ? 1 : -1;
        for (auto& c : cursors) {
            c[0] = x;
            c[1] = y;
        }
        wind.clear();
        int rounds = 12 + random.nextInt(4);
        static const std::vector<double> weights = {0, 2, 2, 2, 0.5, 0.5, 0.25, 0.25, 0.25, 0.2};
        for (int i = 0; i < rounds; ++i) wind.push_back(randomChoice(random, weights));
        arrows = false;
    }

    std::string getGPU() override {
        std::string r;
        for (int w : wind) r += std::to_string(w);
        return r;
    }

    std::vector<int> getRegisters() override { return fillRegisters({&cursor(0), &cursor(1), &cursor(2)}); }

    void tick(const std::vector<int>& actions) override {
        for (int i = 0; i < (int)actions.size(); ++i) {
            int a = actions[i];
            if (a == NO_ACTION) {
                dead[i] = true;
                continue;
            }
            int offset = wind.at(0);
            int dx = 0;
            int dy = 0;
            if (a == DOWN) {
                dy = offset;
            } else if (a == LEFT) {
                dx = -offset;
            } else if (a == RIGHT) {
                dx = offset;
            } else {
                dy = -offset;
            }
            std::vector<int>& c = cursor(i);
            c[0] += dx;
            c[1] += dy;
            int maxDist = 20;
            if (c[0] > maxDist) c[0] = maxDist;
            if (c[1] > maxDist) c[1] = maxDist;
            if (c[0] < -maxDist) c[0] = -maxDist;
            if (c[1] < -maxDist) c[1] = -maxDist;
        }
        wind.erase(wind.begin()); // wind.remove(0)
        arrows = isGameOver();
    }

    bool isGameOver() override { return wind.empty(); }

    std::vector<int> getRankings() override {
        std::vector<std::pair<int, double>> pointsByPlayer; // TreeMap
        for (int i = 0; i < (int)cursors.size(); ++i) {
            std::vector<int>& c = cursors[i];
            // Math.pow(int, 2) is exact for these small values
            double distance = (double)c[0] * (double)c[0] + (double)c[1] * (double)c[1];
            if (dead[i]) pointsByPlayer.push_back({i, -DBL_MAX});
            else pointsByPlayer.push_back({i, -distance});
        }
        return createRankings(pointsByPlayer);
    }

    std::string getName() override { return "Archery"; }

    void checkViewSerialization() override {
        cursor(0);
        cursor(1);
        cursor(2);
    }
};

struct RollerSpeedSkating : MiniGame {
    java::SHA1PRNG* random = nullptr;
    std::vector<int> positions, risk;
    std::vector<bool> dead;
    std::vector<int> directions; // Arrays.asList(Action.values()), shuffled in place
    int length = 0, timer = 0;

    RollerSpeedSkating() : positions(PLAYER_COUNT, 0), risk(PLAYER_COUNT, 0), dead(PLAYER_COUNT, false) {
        directions = {UP, DOWN, LEFT, RIGHT};
        type = "skating";
    }

    void reset(java::SHA1PRNG& rnd) override {
        random = &rnd;
        for (int i = 0; i < (int)positions.size(); ++i) {
            positions[i] = 0;
            risk[i] = 0;
        }
        java::shuffle(directions, rnd);
        length = 10;
        timer = 15;
    }

    std::string getGPU() override {
        std::string r;
        for (int a : directions) r += ACTION_NAMES[a][0];
        return r;
    }

    std::vector<int> getRegisters() override {
        std::vector<int> registers = fillRegisters({&positions, &risk});
        registers[6] = timer;
        return registers;
    }

    void tick(const std::vector<int>& actions) override {
        for (int i = 0; i < (int)actions.size(); ++i) {
            int action = actions[i];
            if (action == NO_ACTION) {
                dead[i] = true;
                continue;
            }
            if (risk[i] < 0) {
                risk[i]++;
                continue;
            }
            int idx = (int)(std::find(directions.begin(), directions.end(), action) - directions.begin());
            int dx = idx == 0 ? 1 : idx == 3 ? 3 : 2;
            positions[i] = positions[i] + dx;
            int riskValue = -1 + idx;
            risk[i] = std::max(0, risk[i] + riskValue);
        }
        for (int i = 0; i < (int)positions.size(); ++i) {
            if (risk[i] < 0) {
                continue;
            }
            bool clash = false;
            for (int k = 0; k < (int)positions.size(); ++k) {
                if (k == i) {
                    continue;
                }
                if (positions[k] % length == positions[i] % length) {
                    clash = true;
                    break;
                }
            }
            if (clash) {
                risk[i] += 2;
            }
            if (risk[i] >= 5) {
                risk[i] = -2; // stun
            }
        }
        java::shuffle(directions, *random);
        timer--;
    }

    bool isGameOver() override { return timer <= 0; }

    std::vector<int> getRankings() override {
        std::vector<std::pair<int, double>> scoreByPlayer; // TreeMap
        for (int i = 0; i < (int)positions.size(); ++i) scoreByPlayer.push_back({i, (double)(dead[i] ? -1 : positions[i])});
        return createRankings(scoreByPlayer);
    }

    std::string getName() override { return "Roller Speed Skating"; }
};

struct Diving : MiniGame {
    std::string goal;                          // LinkedList<Character>
    std::vector<std::string> playerInputs;     // HashMap<Integer, List<Character>> (view only)
    int turnsRemaining = 0;
    std::vector<int> points, combo;
    std::vector<bool> dead;

    Diving() : points(PLAYER_COUNT, 0), combo(PLAYER_COUNT, 0), dead(PLAYER_COUNT, false) { type = "diving"; }

    void reset(java::SHA1PRNG& random) override {
        goal.clear();
        playerInputs.clear();
        int length = 12 + random.nextInt(4);
        for (int i = 0; i < length; ++i) {
            char c = ACTION_NAMES[random.nextInt(4)][0];
            goal.push_back(c);
        }
        for (int i = 0; i < (int)points.size(); ++i) {
            points[i] = 0;
            combo[i] = 0;
            playerInputs.push_back("");
        }
        turnsRemaining = length + 1;
    }

    std::string getGPU() override { return goal; }

    std::vector<int> getRegisters() override { return fillRegisters({&points, &combo}); }

    void tick(const std::vector<int>& actions) override {
        for (int i = 0; i < (int)actions.size(); ++i) {
            int action = actions[i];
            if (action == NO_ACTION) {
                dead[i] = true;
                continue;
            }
            if (goal.at(0) == ACTION_NAMES[action][0]) {
                combo[i]++;
                points[i] += combo[i];
            } else {
                combo[i] = 0;
            }
            playerInputs[i] += ACTION_NAMES[action][0];
        }
        turnsRemaining = (int)goal.size();
        goal.erase(0, 1); // goal.remove(0)
    }

    bool isGameOver() override { return goal.empty(); }

    std::vector<int> getRankings() override {
        std::vector<std::pair<int, double>> pointsByPlayer; // HashMap<Integer, Double>, keys 0..n-1
        for (int i = 0; i < (int)points.size(); ++i) pointsByPlayer.push_back({i, (double)(dead[i] ? -1 : points[i])});
        return createRankings(pointsByPlayer);
    }

    std::string getName() override { return "Diving"; }

    void checkViewSerialization() override {
        // serialize(playerInputs.get(0..2)): a missing key gives null and a NullPointerException
        for (int i = 0; i < 3; i++)
            if (i >= (int)playerInputs.size())
                throw java::Exception{"java.lang.NullPointerException: Cannot invoke \"java.util.List.stream()\" because \"list\" is null"};
    }
};

// ----------------------------------------------------------------------------
// Game.java, Serializer.java, CommandManager.java, Referee.java
// ----------------------------------------------------------------------------
struct Game {
    std::vector<MiniGame*> minigames;
    java::SHA1PRNG& random = cg::gm.getRandom();
    int resets[4] = {0, 0, 0, 0};

    void init() {
        PLAYER_COUNT = (int)players.size();
        int leagueLevel = cg::gm.getLeagueLevel();
        minigames.push_back(new HurdleRace());
        if (leagueLevel == 2) {
            minigames.push_back(new HurdleRace());
            minigames.push_back(new HurdleRace());
            minigames.push_back(new HurdleRace());
        } else if (leagueLevel > 2) {
            minigames.push_back(new Archery());
            minigames.push_back(new RollerSpeedSkating());
            minigames.push_back(new Diving());
        }
        for (MiniGame* minigame : minigames) minigame->reset(random);
        for (Player& p : players) p.init((int)minigames.size());
    }

    void resetGameTurnData() {
        for (Player& p : players) p.reset();
    }

    static const char* getMedalName(int rank) {
        if (rank == 0) return "GOLD medal";
        if (rank == 1) return "SILVER medal";
        if (rank == 2) return "BRONZE medal";
        return "pat on the back";
    }

    std::vector<int> getRankings(MiniGame* minigame) {
        std::vector<int> rankings = minigame->getRankings();
        for (int i = 0; i < (int)rankings.size(); ++i) {
            if (!players.at(i).active) rankings[i] = 2;
        }
        return rankings;
    }

    void performGameUpdate(int) {
        for (int gameIdx = 0; gameIdx < (int)minigames.size(); gameIdx++) {
            MiniGame* minigame = minigames[gameIdx];
            if (minigame->shouldReset) {
                minigame->resetting = true;
                minigame->reset(random);
                resets[gameIdx]++;
                minigame->shouldReset = false;
            } else {
                minigame->resetting = false;
                std::vector<int> actions;
                for (Player& p : players) actions.push_back(p.action);
                minigame->tick(actions);
                if (minigame->isGameOver()) {
                    minigame->shouldReset = true;
                    std::vector<int> rankings = getRankings(minigame);
                    for (int i = 0; i < (int)rankings.size(); ++i) players.at(i).medals.at(gameIdx).at(rankings[i])++;
                    addToGameSummary(java::ascii("Game over: " + minigame->getName()));
                    for (int i = 0; i < (int)rankings.size(); ++i)
                        addToGameSummary(java::ascii("$" + std::to_string(players.at(i).index) + " earns a " + getMedalName(rankings[i])));
                }
            }
        }
    }

    void onEnd() {
        for (Player& p : players) {
            if (!p.active) p.score = -1;
            else p.score = p.getPoints();
        }
        // EndScreenModule data and the gold_medals metadata: view only
    }
};

static Game game;

// Serializer.serializeFrameInfoFor
static std::vector<String> serializeFrameInfoFor(const Player&) {
    std::vector<String> lines;
    for (const Player& p : players) {
        std::string s = std::to_string(p.getPoints());
        for (size_t gameIdx = 0; gameIdx < game.minigames.size(); gameIdx++)
            for (int k = 0; k < 3; k++) s += " " + std::to_string(p.medals[gameIdx][k]);
        lines.push_back(java::ascii(s));
    }
    for (MiniGame* minigame : game.minigames) {
        std::vector<int> reg = minigame->getRegisters();
        std::string registers;
        for (size_t i = 0; i < reg.size(); i++) registers += (i ? " " : "") + std::to_string(reg[i]);
        std::string gpu = minigame->getGPU();
        lines.push_back(java::ascii((minigame->shouldReset ? std::string("GAME_OVER") : gpu) + " " + registers));
    }
    return lines;
}

// ViewModule.sendFrameData (Serializer.serializeFrameData): only its exceptions matter
static void viewSendFrameData() {
    for (MiniGame* minigame : game.minigames) {
        minigame->checkViewSerialization();
        if (minigame->shouldReset) game.getRankings(minigame);
    }
}

// CommandManager: ^(?<button>UP|DOWN|RIGHT|LEFT)(?:\s+(?<message>.+))?\s*$, CASE_INSENSITIVE
// (ASCII only), with matches(). The line holds no line terminator (the Scanner split on them),
// so it matches iff it starts with a button and the rest is empty or starts with [ \t\n\x0B\f\r].
static int matchPlayerPattern(const String& line) {
    static const char* const BUTTONS[4] = {"UP", "DOWN", "RIGHT", "LEFT"};
    static const int BUTTON_ACTIONS[4] = {UP, DOWN, RIGHT, LEFT};
    for (int b = 0; b < 4; b++) {
        size_t n = strlen(BUTTONS[b]);
        if (line.size() < n) continue;
        bool ok = true;
        for (size_t i = 0; i < n && ok; i++) {
            char16_t c = line[i];
            if (c >= u'a' && c <= u'z') c = (char16_t)(c - 32);
            ok = c == (char16_t)BUTTONS[b][i];
        }
        if (!ok) continue;
        if (line.size() == n) return BUTTON_ACTIONS[b];
        char16_t c = line[n];
        if (c == u' ' || c == u'\t' || c == u'\n' || c == 0x0B || c == u'\f' || c == u'\r') return BUTTON_ACTIONS[b];
        return NO_ACTION;
    }
    return NO_ACTION;
}

static void handleCommands(Player& player, const std::vector<String>& lines) {
    if (lines.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)}; // lines.get(0)
    const String& line = lines[0];
    int button = matchPlayerPattern(line);
    if (button != NO_ACTION) {
        player.action = button; // Action.valueOf(button.toUpperCase())
        // the message (group "message") is stored but never used
        return;
    }
    String message = java::ascii("Invalid Input: Expected UP | DOWN | LEFT | RIGHT but got '") + line + u'\'';
    // deactivatePlayer: escapeHTMLEntities
    deactivate(player, java::replace(java::replace(message, u"&lt;", u"<"), u"&gt;", u">"));
    addToGameSummary(java::ascii("Bad command: ") + message);
}

// Referee.java
static void refereeInit() {
    game.init();
    // sendGlobalInfo
    for (Player* player : getActivePlayers()) {
        player->sendInputLine(java::str(player->index));
        player->sendInputLine(java::str((long long)game.minigames.size()));
    }
    cg::gm.setMaxTurns(100);
    cg::gm.setTurnMaxTime(50);
    cg::gm.setFirstTurnMaxTime(1000);
}

static void refereeGameTurn(int t) {
    game.resetGameTurnData();
    for (Player* player : getActivePlayers()) {
        for (const String& line : serializeFrameInfoFor(*player)) player->sendInputLine(line);
        player->execute();
    }
    // handlePlayerCommands
    for (Player* player : getActivePlayers()) {
        if (player->timeout) { // getOutputs() throws TimeoutException
            deactivate(*player, java::ascii("Timeout!"));
            addToGameSummary(java::ascii("$" + std::to_string(player->index) + " has not provided " +
                                       std::to_string(player->getExpectedOutputLines()) + " lines in time"));
        } else {
            handleCommands(*player, player->outputs);
        }
    }
    game.performGameUpdate(t);
    if (getActivePlayers().size() < 2) cg::gm.endGame(); // abort()
}

struct Referee : cg::Referee {
    void init() override { refereeInit(); }
    void gameTurn(int turn) override { refereeGameTurn(turn); }
    void onEnd() override { game.onEnd(); }
    void onGameInit() override { viewSendFrameData(); }      // ViewModule
    void onAfterGameTurn() override { viewSendFrameData(); } // ViewModule
};

int main(int argc, char** argv) {
    static Referee referee;
    return cg::run(argc, argv, referee, [](int) -> cg::Player* {
        players.emplace_back();
        return &players.back();
    });
}
