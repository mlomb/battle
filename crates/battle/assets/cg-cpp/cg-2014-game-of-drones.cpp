// Game of Drones (CodinGame, 2014, "game-of-drones") - headless C++ referee, port of the CodinGame referee in
// https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD; the only commit of the file),
// "Referees/Game of Drones/Referee.java" + referees/cg-builder/patches/cg-2014-game-of-drones.patch (see below). The oldest of
// CodinGame's referees there: no base class (no MultiReferee / AbstractReferee), its `main` runs
// `new Referee().start(System.in)`, a protocol loop of its own that reads the runner's commands with a Scanner and prints
// its answers to System.out (the jar runs it through LegacyRefereeAgent's second entry point, see
// referees/cg-builder/legacy; here common/legacy.h's runStart with its pipe functions nextLine / nextInt / print /
// println, the runner of common/engine.h). 2 to 4 players (CodinGame runs all three counts; the input gives the player
// count). No league rules in the referee. Deterministic: reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 ...] -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's UINPUT: "nbZones=<2 x players>",
// "nbDrones=<3..11>", "gameSeed=N", then an empty line, as the jar's CLI prints that text) to stdout like the jar.
// "null" scores when the game fails (the Java referee thread died or the protocol broke; the jar after 150 s). -l writes
// the trace of common/legacy.h (bot executions, view sizes, summaries, END; CG_TRACE_VIEWS=1 adds the text of every
// view). Time limits: 1000 ms for each player's first turn, 150 ms after (the referee's NEXT_PLAYER_INFO; the statement
// says 100 ms).
//
// The seed and the version (patches/cg-2014-game-of-drones.patch, both changes inferred / checked on CodinGame's replays):
// with no init line the file draws its game from new Random(System.currentTimeMillis()); with init lines it expects the
// three of its UINPUT ("nbZones=", "nbDrones=", "gameSeed=") and fails on anything else, so the CLI's single "seed=N" line
// would end the game at once (an invalid input, every score 0). The patch makes "seed=N" the first path seeded with N:
// nbZones = 2 x players, nbDrones = 3 + 2 nextInt(5), then the zones and the drones. That is CodinGame's game: its
// replays' referee input is "nbZones=<2 x players>", "nbDrones=<drawn from the seed>", "gameSeed=<a clock time>", so
// "-seed <gameSeed>" plays CodinGame's game. The patch also gives the zones the rule of CodinGame's live referee: after
// 501 failed draws in a row for one zone (closer than 700 to a zone before it), the zones are drawn again from zone 1
// on (zone 0 kept). The file's own rule (redraw that zone forever) differs in 3 of 587 replays (4 players); with the rule
// the jar reproduces all 587 (views, infos, summaries, scores). The restart changes the zones of about 1.4% of the
// 4-player seeds (never with 2 or 3 players: 6 zones or fewer). Compiled with -DGAME_OF_DRONES_ORIGINAL=1 (the preset
// cg-2014-game-of-drones-original, jar from patches/cg-2014-game-of-drones-original.patch: the seed change only) this port plays
// the file's rule; a zone that can no longer be placed anywhere (every integer point of the zones' area is closer than
// 700 to a zone before it: possible with 8 zones) makes the Java referee loop forever, so the jar fails after 150 s
// ("null" scores; about 0.04% of the 4-player seeds, e.g. 2761): this port checks the whole area once a zone has failed
// 10^6 draws in a row and fails the same way at once. (Likewise getGameInfo's search for the next player, which never
// ends when every player is out while the round still counts some: only after commands out of the runner's order.)
//
// The game: a 4000 x 1800 field, nbZones zones (radius 100, centres at least 700 apart), nbDrones drones per player, all
// players' drones starting on the same points. One player plays at a time (each GET_GAME_INFO asks the next player still
// in the game; a round is every player that was in it at its start): it outputs one "x y" line per drone (Integer.parseInt
// of split(" ")'s first two parts, 0 <= x < 4000, 0 <= y < 1800, else the player is out: "Failure: invalid input" with the
// line's index; the drones before that line have moved); each drone moves at most 100 towards its target, in doubles,
// unless the target is its rounded position. After a round: the positions rounded ((int) (r + 0.5)), every drone within
// 100 of a zone's centre counts for it (the drones of players out too), a zone goes to the player with strictly the most
// drones in it (a tie keeps the owner), and every owned zone gives its owner 1 point (owners out too). The game ends after
// 200 rounds or when no player is left; a player alone plays on. Scores are the points (no ranking, no -1 for players out).
//
// The Java referee is followed closely, notably: its own command parser (indexOf(']'), substring, Integer.parseInt of the
// line count: a malformed line read as a command, e.g. after a bot line that the Scanner split at U+0085 / U+2028 /
// U+2029, kills the referee thread as in Java, or is executed when it happens to be a command), the views ("" + float
// angle: Float.toString of (float) Math.atan2, java::floatToString; the angle kept when a drone does not move),
// dumpResponse's line counts, the infos ("Game starting...", "$N moved their drones", the reasons with their ¤RED¤ /
// §RED§ markers), the summaries of key frames, NEXT_PLAYER_INPUT before NEXT_PLAYER_INFO in the first round only, the
// init input with every player's first turn, the UINPUT before the SCORES, the file's init error path (its three-line
// form: unreachable from the CLI, kept), Point#distance (java::pointDistance). No HashSet / HashMap in the referee: nothing
// depends on identity hash codes. The view stays far below the runner's 30000-byte limit (at most 44 drones and 8
// zones); no bot text reaches the referee's output.
// Not reproduced: the wall clock (see engine.h); the stderr of the jar (the referee's debug prints, the stack trace of its
// thread at the end).
//
// Validation (referees/README.md): referee-diff against the jar with 33 deterministic bots (every ordered pair on two
// seed bases, 3- and 4-player covering designs on two more, random line-ups, 4-player seeds where the zones restart:
// 3664 games, all identical; the original preset 274 games, all identical, with its looping seeds), direct runs
// comparing the jar's game logs with this port's trace (264 games of every bot, 196 with raw bytes and line endings, all
// identical), the jar's own Referee class driven by 30,000 random command streams against this port's Referee (every
// byte printed identical; 32 of 36 mutations detected, the others equivalent or exact float ties), and CodinGame's 587
// replays through this port's CLI from their gameSeed (every view, infos, summary, score; the original rule: 584).
#pragma STDC FP_CONTRACT OFF
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include "common/legacy.h"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace god {

using java::String;
using legacy::NString;

// a Java exception that leaves start(): the referee thread dies (no more answers, the runner fails)
[[noreturn]] inline void die(const std::string& what) { throw java::Exception{what}; }

inline String S(i64 v) { return java::str(v); }

struct PlayerData {
    int score = 0;
    bool lost = false;
    NString info;
    std::vector<i32> dx, dy;
    std::vector<float> angle;
    std::vector<double> rx, ry;
};

struct ZoneData {
    int playerId = -1;
    std::vector<i32> count;
    bool hasCenter = false; // center != null
    i32 cx = 0, cy = 0;     // center (a java.awt.Point)
    double distance(double px, double py) const {
        if (!hasCenter) die("java.lang.NullPointerException");
        return java::pointDistance(cx, cy, px, py);
    }
};

// Command_INIT (0) / Command_GET_GAME_INFO (1) / Command_SET_PLAYER_OUTPUT (2) / Command_SET_PLAYER_TIMEOUT (3)
struct Command {
    int id = -1;
    int nbPlayers = 0;
    std::vector<String> initLines;
    std::vector<String> output;
};

class Referee {
  public:
    void start();

  private:
    const int width = 4000;
    const int height = 1800;
    const int speed = 100;
    const int zoneRadius = 100;
    const int zoneUpMargin = 250;
    const int zoneRightMargin = 350;

    java::Random rand;

    // reason / reasonCode: null, or the constants below (compared by reference in Java)
    enum Reason { NONE, LOST_PARSING_REASON, OUT_OF_BOUNDS };
    bool end = false;
    Reason reason = NONE;
    bool reasonCodeInput = false; // reasonCode == LOST_PARSING_REASON_CODE ("INPUT")

    const int maxRounds = 200;
    int currentPlayerId = -1;
    int nbPlayers = 0;
    int nbZones = 0;
    int nbDronesPerPlayer = 0;
    String saveUserInput;

    std::vector<PlayerData> playersData;
    bool playersDataNull = true; // new PlayerData[n]: nulls until generateInput
    std::vector<ZoneData> zones;
    Command ci;

    int nbRounds = 0;
    int nbIntermediate = 0;
    int nbGlobalRounds = 0;
    int nbCurrentPlayers = 0;
    PlayerData* currentPlayer = nullptr;

    static String reasonText(Reason r) {
        if (r == LOST_PARSING_REASON) return u"Failure: invalid input";
        if (r == OUT_OF_BOUNDS) return u"Failure: an input value is out of bounds";
        return u"null";
    }

    Command parseCommand();
    void initGame(const Command& c);
    void getGameInfo();
    String buildInitView() const;
    String buildInitInput(int playerId) const;
    String buildPlayerInput() const;
    std::vector<String> buildPlayerInfo(int playerId);
    String buildViewParams() const;
    void dumpScores();
    void setPlayerOuput(const Command& c);
    void updateScores();
    void setPlayerTimeout();
    void generateInput();
    void dumpResponse(const char* id, const std::vector<String>& output);
    PlayerData& player(int p) {
        if (playersDataNull) die("java.lang.NullPointerException");
        return playersData[p];
    }
};

// private Command parseCommand(Scanner s)
inline Command Referee::parseCommand() {
    String line = legacy::nextLine();
    size_t fc = line.find(u']');
    i64 firstClose = fc == String::npos ? -1 : (i64)fc;
    i64 len = (i64)line.size();
    // line.substring(2, firstClose)
    if (firstClose < 2 || firstClose > len) die(java::indexOutOfBoundsException(2, len));
    String cmd = line.substr(2, (size_t)(firstClose - 2));
    // Integer.parseInt(line.substring(firstClose + 2, line.length() - 1))
    i64 b = firstClose + 2, e = len - 1;
    if (b > e) die(java::indexOutOfBoundsException(b, len));
    i32 nbLines;
    if (!java::parseInt(line.substr((size_t)b, (size_t)(e - b)), nbLines)) die(java::encodeUtf8(java::numberFormatException(line.substr((size_t)b, (size_t)(e - b)))));
    Command c;
    if (cmd == u"INIT") {
        // (System.err.print(nbLines): stderr only) new Command_INIT(s, nbLines)
        c.id = 0;
        c.nbPlayers = legacy::nextInt();
        legacy::nextLine();
        if (nbLines > 1) {
            for (i32 i = 0; i < nbLines - 1; i++) c.initLines.push_back(legacy::nextLine());
        }
    } else if (cmd == u"GET_GAME_INFO") {
        c.id = 1;
    } else if (cmd == u"SET_PLAYER_OUTPUT") {
        c.id = 2;
        if (nbLines < 0) die("java.lang.NegativeArraySizeException: " + std::to_string(nbLines));
        for (i32 i = 0; i < nbLines; i++) c.output.push_back(legacy::nextLine());
    } else if (cmd == u"SET_PLAYER_TIMEOUT") {
        c.id = 3;
    } else {
        die("java.lang.NullPointerException"); // parseCommand returns null: switch (c.id)
    }
    return c;
}

// public void start(InputStream is)
inline void Referee::start() {
    for (;;) {
        Command c = parseCommand();
        switch (c.id) {
        case 0:
            initGame(c);
            break;
        case 1:
            getGameInfo();
            break;
        case 2:
            setPlayerOuput(c);
            break;
        case 3:
            setPlayerTimeout();
            break;
        }
    }
}

inline void Referee::initGame(const Command& c) {
    nbPlayers = c.nbPlayers;
    nbCurrentPlayers = nbPlayers;
    if (nbPlayers < 0) die("java.lang.NegativeArraySizeException: " + std::to_string(nbPlayers));
    playersData.assign(nbPlayers, PlayerData());
    playersDataNull = true;
    // (the patch) no init line, or one "seed=N" line: the game drawn from that seed
    if (c.initLines.empty() || (c.initLines.size() == 1 && java::startsWith(c.initLines[0], u"seed="))) {
        i64 seed;
        if (c.initLines.empty()) {
            seed = (i64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        } else if (!java::parseLong(c.initLines[0].substr(5), seed)) {
            die(java::encodeUtf8(java::numberFormatException(c.initLines[0].substr(5))));
        }
        rand = java::Random(seed);
        nbZones = nbPlayers * 2;
        nbDronesPerPlayer = 3 + (2 * rand.nextInt(5));
        saveUserInput += u"nbZones=" + S(nbZones) + u"\n";
        saveUserInput += u"nbDrones=" + S(nbDronesPerPlayer) + u"\n";
        saveUserInput += u"gameSeed=" + S(seed) + u"\n";
        generateInput();
        return;
    }
    // the three-line form (unreachable from the CLI): any exception is an invalid input
    bool ok = [&]() {
        String parseInputArray[3], parsedInput[3];
        for (int line = 0; line < 3; line++) {
            if (line >= (int)c.initLines.size()) return false; // ArrayIndexOutOfBoundsException
            if (!java::scannerFirstLine(c.initLines[line], parseInputArray[line])) return false;
        }
        for (int i = 0; i != 3; i++) {
            std::vector<String> parts = java::split(parseInputArray[i], u'=');
            if (parts.size() < 2) return false;
            parsedInput[i] = parts[1];
        }
        i32 z, d;
        i64 seed;
        if (!java::parseInt(parsedInput[0], z)) return false;
        nbZones = z;
        if (!java::parseInt(parsedInput[1], d)) return false;
        nbDronesPerPlayer = d;
        if (!java::parseLong(parsedInput[2], seed)) return false;
        saveUserInput += u"nbZones=" + S(nbZones) + u"\n";
        saveUserInput += u"nbDrones=" + S(nbDronesPerPlayer) + u"\n";
        saveUserInput += u"gameSeed=" + S(seed) + u"\n";
        if (nbDronesPerPlayer > 20 || nbDronesPerPlayer < 1 || nbZones > 8 || nbZones < 1) {
            reason = OUT_OF_BOUNDS;
            return false;
        }
        rand = java::Random(seed);
        rand.nextInt(5);
        generateInput();
        return true;
    }();
    if (!ok) {
        end = true;
        if (reason != OUT_OF_BOUNDS) reason = LOST_PARSING_REASON;
        reasonCodeInput = true;
        ci = c;
    }
}

inline void Referee::getGameInfo() {
    if (end && reasonCodeInput) {
        String coloredReason = u"¤RED¤" + reasonText(reason) + u"§RED§";
        dumpResponse("VIEW", {u"KEY_FRAME -1", u"INPUT"});
        String inputError;
        for (const String& initLine : ci.initLines) inputError += initLine + u"\n";
        dumpResponse("INFOS", {coloredReason, inputError});
        dumpScores();
        return;
    }
    if (nbIntermediate == nbCurrentPlayers) {
        nbIntermediate = 0;
        updateScores();
        nbCurrentPlayers = 0;
        for (int p = 0; p < nbPlayers; p++)
            if (player(p).lost != true) nbCurrentPlayers++;
        nbGlobalRounds++;
    }

    bool key = nbIntermediate == 0;
    String frame = String(key ? u"KEY_FRAME" : u"INTERMEDIATE_FRAME") + u" " + S(nbRounds);
    bool scores = nbCurrentPlayers == 0 || nbGlobalRounds == maxRounds;
    int lastPlayerId = currentPlayerId;
    if (nbCurrentPlayers != 0) {
        // (every player out while the round still counts some: only after commands out of the runner's order, e.g. an
        // injected SET_PLAYER_TIMEOUT; the Java referee then loops forever, the jar fails after 150 s)
        bool someoneIn = false;
        for (int p = 0; p < nbPlayers && !someoneIn; p++) someoneIn = !player(p).lost;
        if (nbPlayers > 0 && !someoneIn) die("loops forever: no player left to play in getGameInfo()");
        do {
            currentPlayerId = java::rem(currentPlayerId + 1, nbPlayers);
        } while (player(currentPlayerId).lost);
        currentPlayer = &player(currentPlayerId);
    }

    String params = buildPlayerInput();
    String viewParams = buildViewParams();
    std::vector<String> infos = buildPlayerInfo(lastPlayerId);
    bool hasSummary = key;
    std::vector<String> summary;
    if (key) {
        for (int p = 0; p < nbPlayers; p++) {
            PlayerData& pd = player(p);
            if (pd.lost) summary.push_back(u"$" + S(p) + u" no longer plays in this game. (current Score = " + S(pd.score) + u")");
            else summary.push_back(u"$" + S(p) + u" score: " + S(pd.score));
        }
        for (int z = 0; z < nbZones; z++) {
            if (zones[z].playerId == -1) summary.push_back(u"Zone " + S(z) + u" is neutral");
            else summary.push_back(u"Zone " + S(z) + u" belongs to " + u"$" + S(zones[z].playerId));
        }
    }
    if (end) {
        // (unreachable: `end` only comes with the init error handled above)
        String coloredReason = u"¤RED¤" + reasonText(reason) + u"§RED§";
        if (reasonCodeInput) {
            dumpResponse("VIEW", {u"KEY_FRAME -1", u"INPUT"});
            String inputError;
            for (const String& initLine : ci.initLines) inputError += initLine + u"\n";
            dumpResponse("INFOS", {coloredReason, inputError});
            if (hasSummary) dumpResponse("SUMMARY", summary);
        } else {
            dumpResponse("VIEW", {frame, viewParams}); // (reasonCode null: not printed)
            dumpResponse("INFOS", {coloredReason});
            if (hasSummary) dumpResponse("SUMMARY", summary);
        }
        dumpScores();
    } else if (nbGlobalRounds == 0) {
        if (nbRounds == 0) dumpResponse("VIEW", {frame, u"GOD", buildInitView(), buildInitInput(-1), viewParams});
        else dumpResponse("VIEW", {frame, viewParams});
        dumpResponse("INFOS", infos);
        if (hasSummary) dumpResponse("SUMMARY", summary);
        dumpResponse("NEXT_PLAYER_INPUT", {buildInitInput(currentPlayerId), params});
        dumpResponse("NEXT_PLAYER_INFO", {S(currentPlayerId), S(nbDronesPerPlayer), u"1000"});
    } else if (scores) {
        dumpResponse("VIEW", {frame, viewParams});
        dumpResponse("INFOS", infos);
        if (hasSummary) dumpResponse("SUMMARY", summary);
        dumpScores();
    } else {
        // (key frame or not: the same commands)
        dumpResponse("VIEW", {frame, viewParams});
        dumpResponse("INFOS", infos);
        if (hasSummary) dumpResponse("SUMMARY", summary);
        dumpResponse("NEXT_PLAYER_INFO", {S(currentPlayerId), S(nbDronesPerPlayer), u"150"});
        dumpResponse("NEXT_PLAYER_INPUT", {params});
    }
    if (key) nbRounds++;
}

inline String Referee::buildInitView() const { return S(width) + u" " + S(height) + u" " + S(zoneRadius); }

inline String Referee::buildInitInput(int playerId) const {
    String sb = S(nbPlayers) + u" " + S(playerId) + u" " + S(nbDronesPerPlayer) + u" " + S(nbZones) + u"\n";
    for (const ZoneData& zoneData : zones) {
        if (!zoneData.hasCenter) die("java.lang.NullPointerException");
        sb += S(zoneData.cx) + u" " + S(zoneData.cy) + u"\n";
    }
    return sb;
}

inline String Referee::buildPlayerInput() const {
    String sb;
    for (const ZoneData& zoneData : zones) sb += S(zoneData.playerId) + u"\n";
    for (const PlayerData& p : playersData)
        for (int d = 0; d < nbDronesPerPlayer; d++) sb += S(p.dx[d]) + u" " + S(p.dy[d]) + u"\n";
    return sb;
}

inline std::vector<String> Referee::buildPlayerInfo(int playerId) {
    std::vector<String> infos;
    if (nbRounds == 0) {
        infos.push_back(u"Game starting...");
    } else {
        PlayerData& p = player(playerId);
        if (p.info) {
            infos.push_back(*p.info);
            p.info.reset();
        } else {
            infos.push_back(u"$" + S(playerId) + u" moved their drones");
        }
    }
    return infos;
}

inline String Referee::buildViewParams() const {
    String sb;
    for (int p = 0; p < nbPlayers; p++) sb += S(playersData[p].score) + u"\n";
    for (int z = 0; z < nbZones; z++) {
        sb += S(zones[z].playerId) + u"\n";
        for (int p = 0; p < nbPlayers; p++) sb += S(zones[z].count[p]) + u"\n";
    }
    for (int p = 0; p < nbPlayers; p++) {
        const PlayerData& pd = playersData[p];
        for (int d = 0; d < nbDronesPerPlayer; d++)
            sb += S(pd.dx[d]) + u" " + S(pd.dy[d]) + u" " + java::ascii(java::floatToString(pd.angle[d])) + u"\n";
    }
    return sb;
}

inline void Referee::dumpScores() {
    std::vector<String> playerScores;
    for (int i = 0; i < nbPlayers; i++) playerScores.push_back(S(i) + u" " + S(reasonCodeInput ? 0 : player(i).score));
    if (!reasonCodeInput) dumpResponse("UINPUT", {saveUserInput});
    dumpResponse("SCORES", playerScores);
}

inline void Referee::setPlayerOuput(const Command& c) {
    nbIntermediate++;
    // read the drones' requested positions and move the drones at the fixed speed
    for (int i = 0; i < nbDronesPerPlayer; i++) {
        if (i >= (int)c.output.size()) die(java::arrayIndexOutOfBoundsException(i, (i64)c.output.size()));
        std::vector<String> coords = java::split(c.output[i], u' ');
        i32 x = 0, y = 0;
        bool invalid = false;
        // Integer.parseInt(coords[0]), Integer.parseInt(coords[1]): any exception is an invalid line
        if (coords.size() < 1 || !java::parseInt(coords[0], x) || coords.size() < 2 || !java::parseInt(coords[1], y)) invalid = true;
        else if (x < 0 || x >= width || y < 0 || y >= height) invalid = true;
        if (currentPlayer == nullptr) die("java.lang.NullPointerException");
        if (invalid) {
            currentPlayer->lost = true;
            currentPlayer->info = u"¤RED¤Failure: invalid input§RED§. At line " + S(i) + u": expected two integers with values between [0 and " + S(width - 1) +
                                  u"] and [0 and " + S(height - 1) + u"]";
            return;
        }
        // compute the next position in doubles (rounded to int afterwards)
        if (x != currentPlayer->dx[i] || y != currentPlayer->dy[i]) {
            double curX = currentPlayer->rx[i];
            double curY = currentPlayer->ry[i];
            double diffX = x - curX;
            double diffY = y - curY;
            double square = (diffX * diffX) + (diffY * diffY);
            if (square <= (double)(speed * speed)) {
                currentPlayer->rx[i] = x;
                currentPlayer->ry[i] = y;
            } else {
                double norm = std::sqrt(square);
                currentPlayer->rx[i] = curX + ((speed * diffX) / norm);
                currentPlayer->ry[i] = curY + ((speed * diffY) / norm);
            }
            double w = currentPlayer->rx[i] - curX;
            double h = currentPlayer->ry[i] - curY;
            if (w != 0 || h != 0) currentPlayer->angle[i] = (float)java::atan2(h, w);
        }
    }
}

inline void Referee::updateScores() {
    // the zones' owners: step 1, every player's drones in each zone
    for (int p = 0; p < nbPlayers; p++) {
        for (int z = 0; z < nbZones; z++) zones[z].count[p] = 0;
        PlayerData& pd = player(p);
        for (int d = 0; d < nbDronesPerPlayer; d++) {
            pd.dx[d] = java::d2i(pd.rx[d] + 0.5);
            pd.dy[d] = java::d2i(pd.ry[d] + 0.5);
            for (int z = 0; z < nbZones; z++)
                if (zones[z].distance(pd.dx[d], pd.dy[d]) <= zoneRadius) zones[z].count[p]++;
        }
    }
    // each zone: a player strictly first takes it (a tie leaves the owner), the owner scores
    for (int z = 0; z < nbZones; z++) {
        int max = 0;
        int playerId = -1;
        const std::vector<i32>& count = zones[z].count;
        for (int i = 0; i < nbPlayers; i++) {
            if (count[i] > max) {
                max = count[i];
                playerId = i;
            } else if (max == count[i]) {
                playerId = -1;
            }
        }
        if (playerId != -1) zones[z].playerId = playerId;
        if (zones[z].playerId != -1) player(zones[z].playerId).score++;
    }
}

inline void Referee::setPlayerTimeout() {
    if (currentPlayer == nullptr) die("java.lang.NullPointerException");
    currentPlayer->lost = true;
    currentPlayer->info = u"¤RED¤Timeout: the program did not provide " + S(nbDronesPerPlayer) + u" input lines in due time...§RED§ $" + S(currentPlayerId) +
                          u" will no longer be active in this game.";
    nbIntermediate++;
}

inline void Referee::generateInput() {
    if (nbDronesPerPlayer < 0 || nbZones < 0) die("java.lang.NegativeArraySizeException");
    playersData.assign(nbPlayers, PlayerData());
    for (PlayerData& p : playersData) {
        p.dx.assign(nbDronesPerPlayer, 0);
        p.dy.assign(nbDronesPerPlayer, 0);
        p.angle.assign(nbDronesPerPlayer, 0.0f);
        p.rx.assign(nbDronesPerPlayer, 0.0);
        p.ry.assign(nbDronesPerPlayer, 0.0);
    }
    playersDataNull = false;
    zones.assign(nbZones, ZoneData());
    for (ZoneData& z : zones) z.count.assign(nbPlayers, 0);

    for (int i = 0; i < nbZones; i++) {
        bool intersect = false;
#ifndef GAME_OF_DRONES_ORIGINAL
        int failures = 0;
#else
        long long failures = 0;
#endif
        do {
            intersect = false;
            i32 x = rand.nextInt((width - zoneRightMargin) - (2 * zoneRadius)) + zoneRadius;
            i32 y = rand.nextInt((height - zoneUpMargin) - (2 * zoneRadius)) + zoneRadius + zoneUpMargin;
            for (int j = 0; j < i; j++) {
                if (zones[j].distance(x, y) < (zoneRadius * 7)) {
                    intersect = true;
                    break;
                }
            }
            if (!intersect) {
                zones[i].hasCenter = true;
                zones[i].cx = x;
                zones[i].cy = y;
            }
#ifdef GAME_OF_DRONES_ORIGINAL
            // the file's rule redraws forever: once a zone has failed 10^6 draws in a row, look for a free point in the
            // whole area; none means that the Java referee never ends this loop (the jar fails after 150 s)
            if (intersect && ++failures == 1000000) {
                bool free = false;
                for (i32 fx = zoneRadius; fx < width - zoneRightMargin - zoneRadius && !free; fx++)
                    for (i32 fy = zoneRadius + zoneUpMargin; fy < height - zoneRadius && !free; fy++) {
                        bool ok = true;
                        for (int j = 0; j < i && ok; j++)
                            if (zones[j].distance(fx, fy) < (zoneRadius * 7)) ok = false;
                        free = ok;
                    }
                if (!free) die("the zones leave no room for zone " + std::to_string(i) + ": the Java referee loops forever");
            }
        } while (intersect);
#else
        } while (intersect && ++failures <= 500);
        // (the live rule) 501 draws in a row too close to the zones before: the zones are drawn again from zone 1 on
        if (intersect) i = 0;
#endif
    }

    for (int i = 0; i < nbDronesPerPlayer; i++) {
        bool intersect = false;
        do {
            intersect = false;
            i32 x = rand.nextInt(width);
            i32 y = rand.nextInt(height);
            for (int j = 0; j < nbZones; j++) {
                if (zones[j].distance(x, y) <= zoneRadius) {
                    intersect = true;
                    break;
                }
            }
            if (!intersect) {
                // the same starting spot for every player
                for (int k = 0; k < nbPlayers; k++) {
                    playersData[k].dx[i] = x;
                    playersData[k].dy[i] = y;
                    playersData[k].rx[i] = x;
                    playersData[k].ry[i] = y;
                }
            }
        } while (intersect);
    }
}

// private void dumpResponse(String id, String... output): the header with the line count ('\n's, plus one for a string
// that does not end with one), println'd, then every string print'ed, with a '\n' after one that does not end with one
inline void Referee::dumpResponse(const char* id, const std::vector<String>& output) {
    int nbLines = 0;
    for (const String& s : output) {
        for (char16_t c : s)
            if (c == u'\n') nbLines++;
        if (s.empty()) die(java::indexOutOfBoundsException(-1, 0)); // s.charAt(s.length() - 1)
        if (s.back() != u'\n') nbLines++;
    }
    // (the -l trace: CG_TRACE_INFOS=1 adds "I <JSON string>", the text of every INFOS command, which the trace's S line
    // replaces by the SUMMARY in key frames)
    static const bool traceInfos = getenv("CG_TRACE_INFOS") != nullptr;
    if (traceInfos && cg::trace && std::string(id) == "INFOS") {
        String text;
        for (const String& s : output) text += s.back() == u'\n' ? s : s + u"\n";
        fprintf(cg::trace, "I %s\n", java::traceJson(text.substr(0, text.size() - 1)).c_str());
    }
    legacy::println(java::ascii(std::string("[[") + id + "] " + std::to_string(nbLines) + "]"));
    for (const String& s : output) {
        legacy::print(s);
        if (s.back() != u'\n') legacy::print(u"\n");
    }
}

} // namespace god

int main(int argc, char** argv) {
    static god::Referee referee;
    return legacy::runStart(argc, argv, [] { referee.start(); });
}
