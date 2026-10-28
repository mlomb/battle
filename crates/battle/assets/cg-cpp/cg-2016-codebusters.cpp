// Code Busters (CodinGame, 2016-06, "codebusters") - headless C++ referee, port of the CodinGame referee in
// https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe, 2018-07-23, = HEAD): the single-file referee
// `class Referee extends MultiReferee` with CodinGame's old AbstractReferee base of 2016 embedded (common/legacy.h,
// legacy::Base::codeBusters2016()). The folder "Referees/Code Busters" holds two versions of the game, both
// `class Referee` with the same base:
//   - puzzle/Referee.java, the multiplayer game CodinGame runs today (this file, preset cg-2016-codebusters): GAME_VERSION 5,
//     the top league's rules (stuns, ghosts with stamina, RADAR, EJECT), 250 rounds;
//   - challenge/Referee.java, the contest's referee (cg-2016-codebusters-contest.cpp: this file with CODEBUSTERS_CONTEST 1):
//     GAME_VERSION 3 (stuns, ghosts with stamina; no RADAR, no EJECT), 200 rounds, and another resolution of the
//     actions: RELEASE taking effect while the command is read, a carried ghost following its buster's grab target
//     (a BUST while carrying drops it), a MOVE while busting forgetting the target, no "escaping" round after a ghost
//     is dropped, a bust of a carried ghost refused.
//   CodinGame's replays of today's game (codingame.com's public gameResult/findByGameId: 720 games of the top 1000
//   players, Gold and Legend) are reproduced frame for frame (views, infos, summaries, tooltips, scores) by
//   puzzle/Referee.java (its jar and this port through their CLIs: 720/720), not by challenge/Referee.java (0/720).
// 2 players (getMinimumPlayerCount() 2). The referee has corners and spawn directions for 4 players, but its end
// rules read the scores of players 0 and 1 only (with 3 or 4 players a death makes it index past them: the Java
// referee crashes, as this port does); CodinGame runs it with 2.
// Deterministic: reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): seed, busters and ghosts, in
// the order of a JDK 9+ Properties) to stdout like the jar (referees/cg-builder/legacy: the referee runs unchanged in
// the runner of the SDK 4.5.0). "null" scores when the game fails (the Java referee crashed or the protocol broke; the
// jar after 150 s). -l writes the trace of common/legacy.h (bot executions, view sizes, summaries, tooltips, END;
// CG_TRACE_VIEWS=1 adds the text of every view). Time limits: the base's 1000 ms for the first turn, the game's 100 ms
// after.
//
// League: the referee's static GAME_VERSION (puzzle 5, challenge 3) is the top league's, not read from the init
// properties: GAME_VERSION >= 2 stuns, >= 3 ghosts of stamina 3 / 15 / 40 and the buster state 3 (busting) in the
// input, >= 4 RADAR, >= 5 EJECT (the puzzle's ladder; the challenge has 1 = no stun and 0-stamina ghosts, 2 = 0-stamina
// ghosts, 3 = all). Only the top version is implemented here. CodinGame's replays of each league are reproduced by
// puzzle/Referee.java with GAME_VERSION 1 (Wood 2), 2 (Wood 1), 3 (Bronze), 4 (Silver) and 5 (Gold, Legend); their
// referee input is the seed and the busters / ghosts counts the seed itself gives. The other init properties (busters,
// ghosts, speeds, ranges, CENTRAL_GHOST, map...) keep their defaults: the CLI only passes the seed.
//
// The game: a 16001 x 9001 map, player 0's base at (0, 0), player 1's at (16000, 9000) (radius 1600). Each player has
// 2-4 busters (java.util.Random(seed): 2 + nextInt(3)) spread 600 apart across its corner's diagonal, 1600 from the
// corner; the ghosts: one in the centre and point-symmetric pairs (4 x busters + nextInt(9) ghosts wanted, rounded up
// to an odd count; 200 from the edges, more than 1200 apart, more than 3800 from the bases), stamina 3, 15 or 40 (one
// draw per pair). Each round every player answers one line per buster: `MOVE x y` (800 a round), `BUST id` (a ghost
// at 900-1760: its stamina drops by the number of busters busting it; at 0 the team with the most busters on it gets
// it, carried by its closest buster; a tie keeps them busting), `RELEASE` (a ghost released in a base is captured
// there), `STUN id` (any buster within 1760, itself included, stunned for 10 rounds from the next one, carried ghost
// dropped; 20 rounds of recharge), `RADAR` (once per buster: 4400 of vision for the next round's input) and
// `EJECT x y` (throws the carried ghost up to 1760 away: an ejected ghost is never captured where it lands), each with
// an optional message (cut to 17 chars + "..." past 19). Ghosts flee 400 a round from the closest busters seen within
// 2200 (the mean of the equally closest ones), unless busted. Inputs: the own busters, the enemies and free ghosts
// within the vision of any own buster (2200, 4400 with a radar). The score: the ghosts captured in the own base + the
// ghosts the own busters carry. An invalid line or a timeout kills the player (its busters stay on the map, idle;
// the lines before an invalid one still act that round); the game ends when every ghost is captured, when a player
// has captured more than half of them, when every player is dead or the last alive player has more than the dead
// ones, or after 250 rounds (the challenge: 200).
//
// The Java referee is followed closely, notably: the regexes (\s and \d of java.util.regex, ASCII case folding,
// Integer.valueOf overflowing to an invalid line, an EJECT whose number overflows keeping its attempt), the double
// precision vectors (FP_CONTRACT off; Math.sqrt and Math.atan2 (fdlibm) the only Math functions; Math.round),
// snapToGameZone, the ghosts' flight and the order of every list (LinkedLists, the busters by id), Java's null
// Integers where they matter (a buster keeps its last target id), the dead player's busters still scaring ghosts,
// being stunned and robbed, a stunned buster's invalid line ending its stun, the summary's per-buster lines and the
// view's frame data (with the bots' messages; views stay small, a few KB: far from the runner's 30000-byte limit; an
// invalid line is quoted whole in the INFOS, where a long one can pass the limit and make the runner fail like the
// jar: lines of invalid UTF-8, whose bytes become 3-byte U+FFFD). The game's HashMaps are only looked up (grab
// demands, team counts) but one: the stuns of a round (a HashMap<Buster, Buster> of identity hash codes, an order
// not even the jar reproduces) are taken here in the busters' order; nothing seen by the bots, the scores or the view
// depends on that order (each stun's effects are independent; k stuns of one carrier print k - 1 "Error: letting go
// of nothing" to stderr whatever the order). The base's tooltips (a HashSet) only count by their size.
// Not reproduced / not modelled: the wall clock (see engine.h).
//
// Validation (referees/README.md): referee-diff against the jars with deterministic bots (every ordered pair, seed
// bases 1, 1000001, 987654321, 5000000000), direct runs comparing the jar's game log with this port's trace (views,
// summaries, tooltips; raw bytes and line endings), the jar's own compiled classes driven by reflection over random
// states (commands, updates, inputs, views, summaries), CodinGame's replays.
#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)
#include "common/legacy.h"

#include <cmath>
#include <memory>

#ifndef CODEBUSTERS_CONTEST
#define CODEBUSTERS_CONTEST 0
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// puzzle/Referee.java (the multiplayer game) unless CODEBUSTERS_CONTEST (challenge/Referee.java, the contest)
const bool CONTEST = CODEBUSTERS_CONTEST != 0;

// --- constants (the defaults: the CLI passes no other init property than the seed) ---
const int GAME_VERSION = CONTEST ? 3 : 5;
const int WIDTH = 16001;
const int HEIGHT = 9001;
const int GHOST_STAMINA[] = {3, 15, 40}; // (GAME_VERSION >= 3)
const int MIN_BUSTERS = 2;
const int MAX_BUSTERS = 5;
const int SPAWN_RADIUS = 1600;
const int HQ_RADIUS = 1600;
const int SPACE_BETWEEN_BUSTERS = 600;
const int MAX_EXTRA_GHOSTS = 8;
const int MAX_GHOSTS = 30;
const int BUSTER_SPEED = 800;
const int BUSTER_SPEED_WHILE_CARRYING = 800;
const int VIEW_RANGE = 2200;
const int RADAR_RANGE = 4400;
const int EJECT_RANGE = 1760;
const int GHOST_VIEW_RANGE = 2200;
const int GRAB_RANGE = 1760;
const int GRAB_SAFETY_DISTANCE = 900;
const int STUN_RANGE = 1760;
const int GHOST_MIN_SEPERATION = 1200;
const int GHOST_MARGIN = 200;
const int STUN_DURATION = 10;
const int STUN_COOLDOWN = 20;
const bool CAN_GRAB_GHOSTS_IN_ENEMY_SPAWN = false;
const bool CAN_STUN = GAME_VERSION >= 2;
const bool CAN_USE_RADAR = !CONTEST && GAME_VERSION >= 4;
const bool CAN_EJECT = !CONTEST && GAME_VERSION >= 5;
const int GHOST_SPEED = 400;
const bool CENTRAL_GHOST = true;
const int RADARS_PER_BUSTER = 1;
// (OBSTACLE_MODE 0: no obstacle, the map is empty: the obstacles' code never runs, the contest's collision steps
// neither)

const double PI = 3.141592653589793; // Math.PI

String S(i64 v) { return java::str(v); }

// AbstractReferee#printError: the referee's System.err (the jar's stderr)
void printError(const char* message) { fprintf(stderr, "%s\n", message); }

// --- Vector (immutable in Java: values) ---
struct Vec {
    double x, y;
};
Vec between(const Vec& a, const Vec& b) { return {b.x - a.x, b.y - a.y}; } // new Vector(a, b)
bool equals(const Vec& a, const Vec& v) { return v.x == a.x && v.y == a.y; }
// round(): (int) Math.round of each
Vec roundV(const Vec& a) { return {(double)java::l2i(java::round(a.x)), (double)java::l2i(java::round(a.y))}; }
double distance(const Vec& a, const Vec& v) { return std::sqrt((v.x - a.x) * (v.x - a.x) + (v.y - a.y) * (v.y - a.y)); }
Vec add(const Vec& a, const Vec& v) { return {a.x + v.x, a.y + v.y}; }
Vec mult(const Vec& a, double factor) { return {a.x * factor, a.y * factor}; }
double length(const Vec& a) { return std::sqrt(a.x * a.x + a.y * a.y); }
double lengthSquared(const Vec& a) { return a.x * a.x + a.y * a.y; }
Vec normalize(const Vec& a) {
    double l = length(a);
    if (l == 0) return {0, 0};
    return {a.x / l, a.y / l};
}
double angle(const Vec& a) { return java::atan2(a.y, a.x); }
Vec translateV(const Vec& a, const Vec& v) { return {v.x + a.x, v.y + a.y}; }
String toIntString(const Vec& a) { return S(java::d2i(a.x)) + u" " + S(java::d2i(a.y)); }

Vec snapToGameZone(const Vec& v) {
    double snapX = v.x, snapY = v.y;
    if (snapX < 0) snapX = 0;
    if (snapX >= WIDTH) snapX = WIDTH - 1;
    if (snapY < 0) snapY = 0;
    if (snapY >= HEIGHT) snapY = HEIGHT - 1;
    return {snapX, snapY};
}

const Vec corners[4] = {{0, 0}, {WIDTH - 1, HEIGHT - 1}, {WIDTH - 1, 0}, {0, HEIGHT - 1}};

// --- the entities ---
const int GHOST_NORMAL = -1;

const int IDLE = 0;
const int CARRYING = 1;
const int STUNNED = 2;
const int GRABBING = 3;

// Buster.Action (the puzzle; NONE = null)
enum class Action { NONE, MOVE, GRAB, STUN, RADAR, RELEASE, EJECT };

struct PlayerInfo;
struct Buster;

struct GameEntity {
    int id;
    Vec position;
    int type;
    int state;
    GameEntity(int id, const Vec& position, int type, int state) : id(id), position(position), type(type), state(state) {}
};

struct Ghost : GameEntity {
    bool frightened = false; // frightenedBy != null
    Vec frightenedBy{0, 0};
    Buster* grabbedBy = nullptr;
    bool moved = false;
    bool inHq = false, suffering = false;
    bool wasSuffering = false, escaping = false; // (escaping: the puzzle only)
    int attemptedGrabs = 0;

    Ghost(int id, const Vec& position, int type, int stamina) : GameEntity(id, position, type, stamina) {}

    bool isTrappedBy(const PlayerInfo* player) const;
    int getStamina() const { return state; }
    bool isGrabbed() const { return grabbedBy != nullptr; }
    String toInitFrameString() const { return S(getStamina()); }
    bool hasMoved(int round) const { return round == 0 || moved; }
    String toFrameString() const;
    void flagUpdated() { moved = true; }
    void decreaseStamina(int amount) {
        state = std::max(0, getStamina() - amount);
        attemptedGrabs = amount;
    }
    bool isCaptured() const { return !isGrabbed() && inHq; }
    int getValue() const { return suffering ? attemptedGrabs : 0; }
    String toPlayerString() const { return S(id) + u" " + toIntString(position) + u" " + S(type) + u" " + S(state) + u" " + S(getValue()); }
};

struct PlayerInfo {
    std::vector<Buster*> busters;
    bool dead = false;
    int index;
    int lastCaptureFrame = 0;
    explicit PlayerInfo(int index) : index(index) {}
};

// an Integer that may be null
using NInt = std::optional<int>;

struct Buster : GameEntity {
    int index, stunCountDown = 0, stunRecharge = 0, radars = 0, radarTimeout = 0;
    PlayerInfo* owner;
    NString message;
    bool hasNextPosition = false;
    Vec nextPosition{0, 0};
    double rotation;
    bool confused = false, finishGrab = false, tooClose = false;
    bool invalidMove = false;
    bool doStun = false;
    // the puzzle
    bool hasAttemptedVector = false;
    Vec attemptedVector{0, 0};
    Action attempt = Action::NONE;
    bool success = false;
    NInt attemptTargetId;
    Ghost* ghost = nullptr;
    // the challenge
    bool hasMovement = false;
    Vec movement{0, 0};
    Vec prevPosition{0, 0};
    NInt grabTarget, stunTarget, release;
    bool attemptGrab = false, stunSuccess = false;

    Buster(int id, int index, const Vec& position, PlayerInfo* owner, double rotation)
        : GameEntity(id, position, owner->index, IDLE), index(index), owner(owner), rotation(rotation) {
        if (!CONTEST) radars = RADARS_PER_BUSTER;
    }

    bool attemptedMove() const { return attempt == Action::MOVE; }
    bool radarSucceeded() const { return attempt == Action::RADAR && success; }
    bool attemptedGrab() const { return attempt == Action::GRAB; }
    bool stunSucceeded() const { return attempt == Action::STUN && success; }

    void reset() {
        if (CONTEST) {
            hasNextPosition = false;
            hasMovement = false;
            invalidMove = false;
            message.reset();
            attemptGrab = false;
            release.reset();
            confused = false;
            stunTarget.reset();
            stunSuccess = false;
            finishGrab = false;
            tooClose = false;
        } else {
            attempt = Action::NONE;
            success = false;
            hasNextPosition = false;
            hasAttemptedVector = false;
            invalidMove = false;
            message.reset();
            confused = false;
            finishGrab = false;
            tooClose = false;
            if (radarTimeout > 0) radarTimeout--;
        }
        if (doStun) applyStun();
    }

    // an Integer unboxed (a null one: NullPointerException)
    static int unbox(const NInt& v) {
        if (!v) throw java::Exception{"java.lang.NullPointerException"};
        return *v;
    }

    String toFrameString() const {
        String m = message ? *message : String();
        String head = toIntString(position) + u" " + S(state) + u" " + S(java::d2i(rotation * 180 / PI)) + u" " + S(confused ? 1 : 0) + u" ";
        if (CONTEST)
            return head + S(stunSuccess ? unbox(stunTarget) : -1) + u" " + S((state == GRABBING || finishGrab) ? unbox(grabTarget) : -1) + u" " + m;
        return head + S(stunSucceeded() ? unbox(attemptTargetId) : -1) + u" " + S((state == GRABBING || finishGrab) ? ghostId() : -1) + u" " +
               S(radarSucceeded() ? 1 : 0) + u" " + m;
    }
    int ghostId() const {
        if (!ghost) throw java::Exception{"java.lang.NullPointerException"};
        return ghost->id;
    }

    void applyStun() {
        state = STUNNED;
        doStun = false;
    }
    void stun(int duration) {
        doStun = true;
        stunCountDown = duration;
    }
    // the puzzle's trappedGhost(), the challenge's grabSuccess()
    bool trappedGhost() const { return (CONTEST ? attemptGrab : attemptedGrab()) && state == CARRYING; }

    void letGo() {
        if (state == CARRYING) {
            state = IDLE;
            if (!CONTEST && ghost != nullptr && ghost->grabbedBy == this) {
                ghost->grabbedBy = nullptr;
                ghost->flagUpdated();
                ghost->escaping = true;
            }
        }
    }
    int getValue() const {
        if (state == CARRYING || state == GRABBING) return CONTEST ? unbox(grabTarget) : ghostId();
        if (state == STUNNED) return stunCountDown;
        return -1;
    }
    // (GAME_VERSION >= 3: GameEntity's; the lower versions show GRABBING as 0)
    String toPlayerString() const { return S(id) + u" " + toIntString(position) + u" " + S(type) + u" " + S(state) + u" " + S(getValue()); }
};

bool Ghost::isTrappedBy(const PlayerInfo* player) const {
    if (CONTEST)
        return isGrabbed() && grabbedBy->grabTarget && *grabbedBy->grabTarget == id && grabbedBy->state == CARRYING && grabbedBy->owner == player;
    return isGrabbed() && grabbedBy->ghost == this && grabbedBy->state == CARRYING && grabbedBy->owner == player;
}

String Ghost::toFrameString() const {
    return S(id) + u" " + toIntString(position) + u" " + S(isGrabbed() ? grabbedBy->id : -1) + u" " + S(suffering ? 1 : 0) + u" " + S(getStamina()) + u" " +
           S(isCaptured() ? 1 : 0);
}

// --- the referee's regexes (Matcher#matches, CASE_INSENSITIVE), hand matched:
//   MOVE\s+(?<x>-?\d+)\s+(?<y>-?\d+)(?:\s+)?(?:\s+(?<message>.+))?     (EJECT likewise)
//   BUST\s+(?<id>\d+)(?:\s+)?(?:\s+(?<message>.+))?                    (STUN likewise)
//   RELEASE(?:\s+)?(?:\s+(?<message>.+))?                               (RADAR likewise)
// The word folds the ASCII letters only; \s and \d are ASCII; the message is what follows the whitespace run after
// the last number (or the word), none when only whitespace follows; '.' never meets a line terminator (the base's
// Scanner split the lines there).
struct Matcher {
    const String& line;
    size_t i = 0;
    explicit Matcher(const String& line) : line(line) {}
    bool word(const char* w) {
        size_t n = strlen(w);
        if (line.size() < n) return false;
        for (size_t k = 0; k < n; k++)
            if (!java::regexCharMatchesIgnoreCase(line[k], w[k])) return false;
        i = n;
        return true;
    }
    // \s+: the whole whitespace run (what follows is never whitespace)
    bool spaces() {
        size_t a = i;
        while (i < line.size() && java::isRegexSpace(line[i])) i++;
        return i > a;
    }
    // -?\d+ (or \d+): the whole digit run (what follows is never a digit)
    bool number(String& group, bool sign) {
        size_t a = i;
        if (sign && i < line.size() && line[i] == u'-') i++;
        size_t d = i;
        while (i < line.size() && java::isRegexDigit(line[i])) i++;
        if (i == d) return false;
        group = line.substr(a, i - a);
        return true;
    }
    // (?:\s+)?(?:\s+(?<message>.+))? then the end
    bool tail(NString& message) {
        if (i == line.size()) {
            message.reset();
            return true;
        }
        if (!spaces()) return false;
        if (i == line.size()) message.reset();
        else message = line.substr(i);
        return true;
    }
};

bool matchXY(const String& line, const char* w, String& gx, String& gy, NString& message) {
    Matcher m(line);
    return m.word(w) && m.spaces() && m.number(gx, true) && m.spaces() && m.number(gy, true) && m.tail(message);
}
bool matchId(const String& line, const char* w, String& gid, NString& message) {
    Matcher m(line);
    return m.word(w) && m.spaces() && m.number(gid, false) && m.tail(message);
}
bool matchWordOnly(const String& line, const char* w, NString& message) {
    Matcher m(line);
    return m.word(w) && m.tail(message);
}

// an Integer argument of String.format (a null one prints "null")
Arg argOf(const std::optional<int>& v) { return v ? Arg(*v) : Arg(); }

// Integer.valueOf(group): false where it throws (a NumberFormatException: the number overflows an int)
bool valueOf(const String& s, int& v) {
    i32 r;
    if (!java::parseInt(s, r)) return false;
    v = r;
    return true;
}

struct Referee : legacy::MultiReferee {
    std::vector<std::unique_ptr<PlayerInfo>> ownedPlayers;
    std::vector<PlayerInfo*> players;
    std::vector<std::unique_ptr<Buster>> ownedBusters;
    std::vector<Buster*> allBusters;
    std::vector<std::unique_ptr<Ghost>> ownedGhosts;
    std::vector<Ghost*> ghosts; // a LinkedList: ids = indices
    int bustersPerPlayer = 0, ghostCount = 0;
    i64 seed = 0;
    java::Random random;
    bool gameOverFlag = false; // the referee's gameOver field

    Referee() { base = legacy::Base::codeBusters2016(); }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        // Long.valueOf(prop.getProperty("seed", <a random long>))
        if (const String* s = prop.find(u"seed")) {
            i64 v;
            if (!java::parseLong(*s, v)) throw java::Exception{java::encodeUtf8(java::numberFormatException(*s))};
            seed = v;
        } else {
            java::Random r((i64)java::entropy());
            seed = r.nextLong();
        }
        random = java::Random(seed);

        // (GAME_VERSION >= 2: CAN_STUN, >= 3: the ghosts' stamina, >= 4: radar, >= 5: eject; constants here)
        // prop "busters" / "ghosts" / speeds / ranges / CENTRAL_GHOST: their defaults (the default values are computed
        // whatever the properties: the random draws happen)
        bustersPerPlayer = std::max(MIN_BUSTERS, std::min(MAX_BUSTERS, random.nextInt(MAX_BUSTERS - MIN_BUSTERS) + MIN_BUSTERS));
        ghostCount = std::max(1, std::min(MAX_GHOSTS, bustersPerPlayer * 4 + random.nextInt(MAX_EXTRA_GHOSTS + 1)));
        // map: chooseMap() with OBSTACLE_MODE 0 = no obstacle (no random draw)

        // Generate busters
        int busterId = 0;
        Vec directions[4] = {{1, 1}, {-1, -1}, {-1, 1}, {1, -1}};
        int spawnOffset = 2 * SPAWN_RADIUS / 2;
        for (int i = 0; i < playerCount; ++i) {
            if (i >= 4) throw java::Exception{java::arrayIndexOutOfBoundsException(i, 4)}; // corners[i] (the CLI: 4 at most)
            ownedPlayers.emplace_back(new PlayerInfo(i));
            PlayerInfo* playerInfo = ownedPlayers.back().get();
            players.push_back(playerInfo);

            Vec vector = normalize(i < 2 ? Vec{1, -1} : Vec{1, 1});
            if (i % 2 == 1) vector = mult(vector, -1);

            Vec startPoint = corners[i];
            for (int j = 0; j < bustersPerPlayer; ++j) {
                double offset = (double)((j % 2 == 0 ? -1 : 1) * (j / 2 * 2 + 1) + bustersPerPlayer % 2);
                Vec position = roundV(translateV(translateV(mult(vector, offset * (SPACE_BETWEEN_BUSTERS)), startPoint), mult(directions[i], spawnOffset)));
                // (snapToGameZone(position): its result is dropped)
                ownedBusters.emplace_back(new Buster(busterId++, j, position, playerInfo, angle(directions[i])));
                Buster* b = ownedBusters.back().get();
                playerInfo->busters.push_back(b);
                allBusters.push_back(b);
            }
        }

        // Generate ghosts
        int iterations = 0;
        int ghostId = 0;
        if (CENTRAL_GHOST) {
            int stamina = GHOST_STAMINA[random.nextInt(3)];
            newGhost(ghostId++, Vec{(WIDTH - 1) / 2, (HEIGHT - 1) / 2}, stamina);
        }
        while (ghostId < ghostCount) {
            iterations++;
            if (iterations > 1000) break;
            int x = GHOST_MARGIN + random.nextInt(WIDTH - GHOST_MARGIN * 2);
            int y = GHOST_MARGIN + random.nextInt(HEIGHT - GHOST_MARGIN * 2);
            Vec pos{(double)x, (double)y};

            bool ok = true;
            for (Ghost* ghost : ghosts)
                if (distance(ghost->position, pos) <= GHOST_MIN_SEPERATION) {
                    ok = false;
                    break;
                }
            if (!ok) continue;
            for (int k = 0; k < playerCount; ++k)
                if (distance(corners[k], pos) <= SPAWN_RADIUS + 2200) {
                    ok = false;
                    break;
                }
            if (!ok) continue;
            // (no obstacle)
            int stamina = GHOST_STAMINA[random.nextInt(3)];
            newGhost(ghostId++, pos, stamina);
            newGhost(ghostId++, Vec{WIDTH - 1 - pos.x, HEIGHT - 1 - pos.y}, stamina);
        }
        ghostCount = ghostId;
    }

    void newGhost(int id, const Vec& position, int stamina) {
        ownedGhosts.emplace_back(new Ghost(id, position, GHOST_NORMAL, stamina));
        ghosts.push_back(ownedGhosts.back().get());
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        prop.setProperty(u"busters", S(bustersPerPlayer));
        prop.setProperty(u"ghosts", S(ghostCount));
        return prop;
    }

    // --- AbstractReferee ---
    Lines getInitInputForPlayer(int playerIdx) override { return {S(bustersPerPlayer), S(ghostCount), S(playerIdx)}; }

    void prepare(int) override {
        for (PlayerInfo* player : players)
            for (Buster* buster : player->busters) buster->reset();
    }

    int range(const Buster* b) const { return b->radarTimeout > 0 ? RADAR_RANGE : VIEW_RANGE; }

    Lines getInputForPlayer(int, int playerIdx) override {
        PlayerInfo* player = players[playerIdx];
        std::vector<Buster*> visibleEnemies;
        std::vector<Ghost*> visibleGhosts;
        for (int j = 0; j < (int)players.size(); ++j) {
            if (j == playerIdx) continue;
            for (Buster* enemy : players[j]->busters)
                for (Buster* b : player->busters)
                    if (distance(enemy->position, b->position) <= range(b)) {
                        visibleEnemies.push_back(enemy);
                        break;
                    }
        }
        for (Ghost* ghost : ghosts)
            for (Buster* b : player->busters)
                if (!ghost->inHq && !ghost->isGrabbed() && distance(ghost->position, b->position) <= range(b)) {
                    visibleGhosts.push_back(ghost);
                    break;
                }
        // (no obstacle)
        Lines lines;
        lines.push_back(S(bustersPerPlayer + (int)visibleEnemies.size() + (int)visibleGhosts.size()));
        for (Buster* b : player->busters) lines.push_back(b->toPlayerString());
        for (Buster* enemy : visibleEnemies) lines.push_back(enemy->toPlayerString());
        for (Ghost* ghost : visibleGhosts) lines.push_back(ghost->toPlayerString());
        return lines;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return bustersPerPlayer; }

    static String expected() {
        String e = u"MOVE x y | BUST id | RELEASE"; // (a no-break space in the Java source)
        if (CAN_STUN) e += u" | STUN id";
        if (CAN_USE_RADAR) e += u" | RADAR";
        if (CAN_EJECT) e += u" | EJECT";
        return e;
    }

    static void matchMessage(Buster* buster, const NString& message) {
        buster->message = message;
        if (buster->message && buster->message->size() > 19) buster->message = buster->message->substr(0, 17) + u"...";
    }

    // the puzzle's parse of one line: false where the Java code throws (an invalid line, an Integer.valueOf overflow)
    bool parsePuzzle(Buster* buster, const String& line) {
        String gx, gy, gid;
        NString message;
        if (matchXY(line, "MOVE", gx, gy, message)) {
            int x, y;
            if (!valueOf(gx, x) || !valueOf(gy, y)) return false;
            if (buster->position.x != x || buster->position.y != y) {
                int speed = buster->state == CARRYING ? BUSTER_SPEED_WHILE_CARRYING : BUSTER_SPEED;
                Vec v = between(buster->position, Vec{(double)x, (double)y});
                buster->attemptedVector = lengthSquared(v) <= speed * speed ? v : mult(normalize(v), speed);
                buster->hasAttemptedVector = true;
                buster->attempt = Action::MOVE;
            }
            matchMessage(buster, message);
        } else if (matchId(line, "BUST", gid, message)) {
            int id;
            if (!valueOf(gid, id)) return false;
            buster->attemptTargetId = id;
            buster->attempt = Action::GRAB;
            matchMessage(buster, message);
        } else if (matchWordOnly(line, "RELEASE", message)) {
            buster->attempt = Action::RELEASE;
            matchMessage(buster, message);
        } else if (CAN_STUN && matchId(line, "STUN", gid, message)) {
            int id;
            if (!valueOf(gid, id)) return false;
            buster->attempt = Action::STUN;
            buster->attemptTargetId = id;
            matchMessage(buster, message);
        } else if (CAN_USE_RADAR && matchWordOnly(line, "RADAR", message)) {
            buster->attempt = Action::RADAR;
            matchMessage(buster, message);
        } else if (CAN_EJECT && matchXY(line, "EJECT", gx, gy, message)) {
            buster->attempt = Action::EJECT; // (set before the numbers: an overflow keeps it)
            int x, y;
            if (!valueOf(gx, x) || !valueOf(gy, y)) return false;
            Vec v = between(buster->position, Vec{(double)x, (double)y});
            buster->attemptedVector = lengthSquared(v) <= EJECT_RANGE * EJECT_RANGE ? v : mult(normalize(v), EJECT_RANGE);
            buster->hasAttemptedVector = true;
            matchMessage(buster, message);
        } else {
            return false;
        }
        return true;
    }

    // the challenge's parse of one line
    bool parseContest(Buster* buster, const String& line) {
        String gx, gy, gid;
        NString message;
        if (matchXY(line, "MOVE", gx, gy, message)) {
            int x, y;
            if (!valueOf(gx, x) || !valueOf(gy, y)) return false;
            if (buster->position.x != x || buster->position.y != y) {
                int speed = buster->state == CARRYING ? BUSTER_SPEED_WHILE_CARRYING : BUSTER_SPEED;
                Vec v = between(buster->position, Vec{(double)x, (double)y});
                buster->movement = lengthSquared(v) <= speed * speed ? v : mult(normalize(v), speed);
                buster->hasMovement = true;
            }
            matchMessage(buster, message);
        } else if (matchId(line, "BUST", gid, message)) {
            int id;
            if (!valueOf(gid, id)) return false;
            buster->grabTarget = id;
            buster->attemptGrab = true;
            matchMessage(buster, message);
        } else if (matchWordOnly(line, "RELEASE", message)) {
            if (buster->state == CARRYING) {
                buster->release = buster->grabTarget;
                buster->state = IDLE;
                buster->grabTarget.reset();
            } else {
                buster->confused = true;
            }
            matchMessage(buster, message);
        } else if (CAN_STUN && matchId(line, "STUN", gid, message)) {
            int id;
            if (!valueOf(gid, id)) return false;
            buster->stunTarget = id;
            matchMessage(buster, message);
        } else {
            return false;
        }
        return true;
    }

    void handlePlayerOutput(int, int, int playerIdx, const Lines& outputs) override {
        size_t i = 0;
        PlayerInfo* player = players[playerIdx];
        for (const String& line : outputs) {
            // player.busters.get(i) (the runner sends bustersPerPlayer lines)
            if (i >= player->busters.size()) throw java::Exception{java::indexOutOfBoundsException((i64)i, (i64)player->busters.size())};
            Buster* buster = player->busters[i];
            if (!(CONTEST ? parseContest(buster, line) : parsePuzzle(buster, line))) {
                // catch (Exception e)
                if (!CONTEST) buster->letGo();
                buster->invalidMove = true;
                buster->confused = true;
                buster->state = IDLE;
                if (CONTEST) buster->grabTarget.reset();
                player->dead = true;
                throw legacy::InvalidInputException(expected(), line);
            }
            ++i;
        }
    }

    // the grabbers of a ghost reaching stamina 0 or not (the same in both versions)
    void resolveGrab(Ghost* ghost, const std::vector<Buster*>& grabbers) {
        ghost->decreaseStamina((int)grabbers.size());
        ghost->attemptedGrabs = (int)grabbers.size();
        ghost->suffering = true;
        if (ghost->getStamina() == 0) {
            int winningTeam = -1; // (an Integer: -1 = null)
            int max = 0;
            std::vector<int> teamGrabs(players.size(), 0);
            for (Buster* b : grabbers) {
                int x = teamGrabs[b->owner->index] + 1;
                teamGrabs[b->owner->index] = x;
                if (x > max) {
                    max = x;
                    winningTeam = b->owner->index;
                } else if (x == max) {
                    winningTeam = -1;
                }
            }
            double min = std::numeric_limits<double>::max();
            Buster* closest = nullptr;
            if (winningTeam != -1) {
                for (Buster* b : grabbers) {
                    if (b->owner->index != winningTeam) continue;
                    double dist = distance(b->position, ghost->position);
                    if (dist < min) {
                        min = dist;
                        closest = b;
                    }
                }
                for (Buster* b : grabbers) {
                    b->state = IDLE;
                    b->finishGrab = true;
                }
                closest->state = CARRYING;
                ghost->grabbedBy = closest;
            } else {
                for (Buster* b : grabbers) b->state = GRABBING;
            }
        } else {
            for (Buster* b : grabbers) b->state = GRABBING;
        }
    }

    // the ghost follows its carrier; whether it is in a base
    void carried(Ghost* ghost) {
        ghost->position = ghost->grabbedBy->position;
        bool inHqRange = false;
        for (int i = 0; i < (int)players.size(); ++i)
            if (distance(ghost->position, corners[i]) <= HQ_RADIUS) {
                inHqRange = true;
                break;
            }
        ghost->inHq = inHqRange;
        ghost->flagUpdated();
    }

    // Frightened ghosts
    void frighten() {
        for (Ghost* ghost : ghosts) {
            if (!CONTEST) ghost->escaping = false;
            if (!ghost->isGrabbed() && !ghost->isCaptured() && !ghost->suffering) {
                if (ghost->frightened) {
                    Vec move = mult(normalize(between(ghost->frightenedBy, ghost->position)), GHOST_SPEED);
                    ghost->position = roundV(snapToGameZone(add(ghost->position, move)));
                    ghost->flagUpdated();
                }
                std::vector<Buster*> closest;
                double minDist = 0;
                for (Buster* b : allBusters) {
                    double dist = distance(b->position, ghost->position);
                    if (closest.empty()) {
                        closest.push_back(b);
                        minDist = dist;
                    } else if (dist == minDist) {
                        closest.push_back(b);
                    } else if (dist < minDist) {
                        closest.clear();
                        closest.push_back(b);
                        minDist = dist;
                    }
                }
                if (minDist <= GHOST_VIEW_RANGE) {
                    double x = 0, y = 0;
                    for (Buster* b : closest) {
                        x += b->position.x;
                        y += b->position.y;
                    }
                    x /= (double)closest.size();
                    y /= (double)closest.size();
                    ghost->frightened = true;
                    ghost->frightenedBy = {x, y};
                    if (equals(roundV(ghost->position), roundV(Vec{x, y}))) ghost->frightened = false;
                } else {
                    ghost->frightened = false;
                }
            } else {
                ghost->frightened = false;
            }
        }
    }

    // the end rules of updateGame (scores and captures of players 0 and 1)
    void checkEnd() {
        if (!CAN_GRAB_GHOSTS_IN_ENEMY_SPAWN && allGhostsCaptured()) gameOverFlag = true;

        int scores[2] = {getScore(0), getScore(1)};
        int captures[2] = {getGhostCount(0, false), getGhostCount(1, false)};
        auto at = [](int* a, int i) -> int& {
            if (i < 0 || i >= 2) throw java::Exception{java::arrayIndexOutOfBoundsException(i, 2)};
            return a[i];
        };
        int deads = 0;
        PlayerInfo* alive = nullptr;
        for (PlayerInfo* player : players) {
            if (player->dead) deads++;
            else alive = player;
        }
        if (deads == (int)players.size() - 1) {
            int capturedByDead = 0;
            for (PlayerInfo* other : players) {
                if (alive == other) continue;
                capturedByDead += at(scores, other->index);
            }
            if (at(scores, alive->index) > capturedByDead) gameOverFlag = true;
        } else if (deads == (int)players.size()) {
            gameOverFlag = true;
        }
        // Arret anticipé
        int g = (int)ghosts.size();
        if (captures[0] > g - captures[0] || captures[1] > g - captures[1]) gameOverFlag = true;
    }

    void updateGame(int round) override {
        if (gameOverFlag) throw legacy::GameOverException(std::string("end"));
        if (CONTEST) updateContest(round);
        else updatePuzzle();
        checkEnd();
    }

    void updatePuzzle() {
        std::vector<std::vector<Buster*>> grabDemand(ghosts.size()); // HashMap<Ghost, List<Buster>>: lookups only
        std::vector<std::pair<Buster*, Buster*>> stunMap;            // HashMap<Buster, Buster>: see the header

        for (Buster* buster : allBusters) {
            if (buster->state == STUNNED) continue;
            switch (buster->attempt) {
            case Action::GRAB: {
                buster->letGo();
                Ghost* target = nullptr;
                int id = *buster->attemptTargetId;
                if (id >= 0 && id < (int)ghosts.size()) {
                    target = ghosts[id];
                    double dist = distance(target->position, buster->position);
                    if (dist < GRAB_SAFETY_DISTANCE || dist > GRAB_RANGE) {
                        if (GRAB_SAFETY_DISTANCE > dist) buster->tooClose = true;
                        target = nullptr;
                    } else if (!CAN_GRAB_GHOSTS_IN_ENEMY_SPAWN) {
                        if (target->isCaptured()) target = nullptr;
                    }
                }
                if (target == nullptr) {
                    buster->confused = true;
                } else {
                    grabDemand[target->id].push_back(buster);
                    buster->rotation = angle(between(buster->position, target->position));
                    buster->success = true;
                    buster->ghost = target;
                }
                break;
            }
            case Action::MOVE: {
                Vec newPos = roundV(snapToGameZone(add(buster->position, buster->attemptedVector)));
                buster->nextPosition = newPos;
                buster->hasNextPosition = true;
                break;
            }
            case Action::EJECT: {
                if (buster->state == CARRYING) {
                    if (!buster->hasAttemptedVector) throw java::Exception{"java.lang.NullPointerException"};
                    Vec target = roundV(snapToGameZone(add(buster->position, buster->attemptedVector)));
                    buster->success = true;
                    if (!buster->ghost) throw java::Exception{"java.lang.NullPointerException"};
                    buster->ghost->position = target;
                    buster->ghost->inHq = false;
                    buster->letGo();
                } else {
                    buster->confused = true;
                }
                break;
            }
            case Action::RADAR:
                if (buster->radars > 0) {
                    buster->success = true;
                    buster->radars--;
                    buster->radarTimeout = 2;
                } else {
                    buster->confused = true;
                }
                break;
            case Action::RELEASE:
                if (buster->state == CARRYING) {
                    buster->success = true;
                    buster->letGo();
                } else {
                    buster->confused = true;
                }
                break;
            case Action::STUN: {
                if (buster->state == GRABBING) buster->state = IDLE;
                Buster* target = nullptr;
                int id = *buster->attemptTargetId;
                if (buster->stunRecharge == 0 && id >= 0 && id < (int)allBusters.size()) {
                    target = allBusters[id];
                    if (distance(target->position, buster->position) > STUN_RANGE) target = nullptr;
                }
                if (target == nullptr) {
                    buster->confused = true;
                } else {
                    buster->success = true;
                    buster->stunRecharge = STUN_COOLDOWN;
                    buster->letGo();
                    stunMap.push_back({buster, target});
                }
                break;
            }
            default: break;
            }
        }
        for (Buster* buster : allBusters) {
            if (buster->hasNextPosition) {
                buster->rotation = angle(between(buster->position, buster->nextPosition));
                buster->position = buster->nextPosition;
            }
            if (buster->stunCountDown > 0) {
                buster->stunCountDown--;
                if (buster->stunCountDown == 0) buster->state = IDLE;
            }
            if (buster->stunRecharge > 0) buster->stunRecharge--;
            if (buster->state == GRABBING && buster->attempt != Action::GRAB) buster->state = IDLE;
        }

        for (Ghost* ghost : ghosts) {
            ghost->wasSuffering = ghost->suffering;
            ghost->suffering = false;
            ghost->attemptedGrabs = 0;
            if (!ghost->isGrabbed() && !ghost->escaping) {
                std::vector<Buster*>& grabbers = grabDemand[ghost->id];
                if (!grabbers.empty()) resolveGrab(ghost, grabbers);
            }
            if (ghost->isGrabbed()) carried(ghost);
            else if (ghost->suffering != ghost->wasSuffering) ghost->flagUpdated();
        }

        for (auto& stunEntry : stunMap) {
            Buster* stunned = stunEntry.second;
            if (stunned->state == CARRYING) {
                if (stunned->ghost != nullptr && stunned->ghost->grabbedBy == stunned) {
                    stunned->ghost->grabbedBy = nullptr;
                    stunned->ghost->flagUpdated();
                } else {
                    printError("Error: letting go of nothing");
                }
            }
            stunned->stun(STUN_DURATION);
            stunEntry.first->rotation = angle(between(stunEntry.first->position, stunEntry.second->position));
        }

        frighten();
    }

    void updateContest(int round) {
        std::vector<std::vector<Buster*>> grabDemand(ghosts.size());
        std::vector<std::pair<Buster*, Buster*>> stunMap; // HashMap<Buster, Buster>: no order shows here

        for (Buster* buster : allBusters) {
            if (buster->state == STUNNED) continue;
            if (buster->hasMovement) {
                if (buster->state == GRABBING) {
                    buster->state = IDLE;
                    buster->grabTarget.reset();
                }
                Vec newPos = roundV(snapToGameZone(add(buster->position, buster->movement)));
                // (no obstacle to collide with)
                buster->prevPosition = buster->position;
                buster->nextPosition = newPos;
                buster->hasNextPosition = true;
            } else if (buster->attemptGrab && buster->grabTarget) {
                Ghost* target = nullptr;
                int id = *buster->grabTarget;
                if (id >= 0 && id < (int)ghosts.size()) {
                    target = ghosts[id];
                    double dist = distance(target->position, buster->position);
                    if (dist < GRAB_SAFETY_DISTANCE || dist > GRAB_RANGE || target->isGrabbed()) {
                        if (GRAB_SAFETY_DISTANCE > dist) buster->tooClose = true;
                        target = nullptr;
                    } else if (!CAN_GRAB_GHOSTS_IN_ENEMY_SPAWN) {
                        if (target->isCaptured()) target = nullptr;
                    }
                }
                if (target == nullptr) {
                    if (buster->state == GRABBING) buster->state = IDLE;
                    buster->confused = true;
                } else {
                    grabDemand[target->id].push_back(buster);
                    buster->rotation = angle(between(buster->position, target->position));
                }
            } else if (buster->stunTarget) {
                if (buster->state == GRABBING) buster->state = IDLE;
                Buster* target = nullptr;
                int id = *buster->stunTarget;
                if (buster->stunRecharge == 0 && id >= 0 && id < (int)allBusters.size()) {
                    target = allBusters[id];
                    if (distance(target->position, buster->position) > STUN_RANGE) target = nullptr;
                }
                if (target == nullptr) {
                    buster->confused = true;
                } else {
                    buster->stunSuccess = true;
                    buster->stunRecharge = STUN_COOLDOWN;
                    buster->letGo();
                    stunMap.push_back({buster, target});
                }
            } else if (buster->state == GRABBING) {
                buster->state = IDLE;
                buster->grabTarget.reset();
            }
        }
        for (Buster* buster : allBusters) {
            if (buster->hasNextPosition) {
                buster->position = buster->nextPosition;
                buster->rotation = angle(between(buster->prevPosition, buster->position));
            }
            if (buster->stunCountDown > 0) {
                buster->stunCountDown--;
                if (buster->stunCountDown == 0) buster->state = IDLE;
            }
            if (buster->stunRecharge > 0) buster->stunRecharge--;
        }

        for (Ghost* ghost : ghosts) {
            ghost->wasSuffering = ghost->suffering;
            ghost->suffering = false;
            ghost->attemptedGrabs = 0;
            if (ghost->isGrabbed()) {
                Buster* g = ghost->grabbedBy;
                if (g->state != CARRYING || Buster::unbox(g->grabTarget) != ghost->id) {
                    if (ghost->inHq) g->owner->lastCaptureFrame = round;
                    ghost->grabbedBy = nullptr;
                    ghost->flagUpdated();
                }
            }
            if (!ghost->isGrabbed()) {
                std::vector<Buster*>& grabbers = grabDemand[ghost->id];
                if (!grabbers.empty()) resolveGrab(ghost, grabbers);
            }
            if (ghost->isGrabbed()) carried(ghost);
            else if (ghost->suffering != ghost->wasSuffering) ghost->flagUpdated();
        }

        for (auto& stunEntry : stunMap) {
            Buster* stunned = stunEntry.second;
            if (stunned->state == CARRYING) {
                int id = Buster::unbox(stunned->grabTarget);
                if (id >= 0 && id < (int)ghosts.size()) {
                    Ghost* ghost = ghosts[id];
                    if (ghost->grabbedBy == stunned) {
                        ghost->grabbedBy = nullptr;
                        ghost->flagUpdated();
                    }
                }
            }
            stunned->stun(STUN_DURATION);
            stunEntry.first->rotation = angle(between(stunEntry.first->position, stunEntry.second->position));
        }

        frighten();
    }

    bool allGhostsCaptured() const {
        for (Ghost* g : ghosts)
            if (!g->isCaptured()) return false;
        return true;
    }

    void populateMessages(std::map<std::string, String>& p) override {
        p["invalid"] = u"Buster %d of player $%d was given an invalid command";
        p["stunned"] = u"Buster %d of player $%d is stunned";
        p["move"] = u"Buster %d of player $%d moves to (%d,%d)";
        p["moveAndBust"] = u"Buster %d of player $%d moves to (%d,%d) while carrying ghost %d";
        p["bustError"] = u"Buster %d of player $%d wants to bust ghost %d but can't see it.";
        p["bustTooClose"] = u"Buster %d of player $%d wants to bust ghost %d but is too close.";
        p["bust"] = u"Buster %d of player $%d trapped ghost %d.";
        p["busting"] = u"Buster %d of player $%d is busting ghost %d.";
        p["stun"] = u"Buster %d of player $%d stuns buster %d of player $%d.";
        p["autostun"] = u"Buster %d of player $%d stuns himself! What a clutz!.";
        p["stunError"] = u"Buster %d of player $%d wants to stun buster %d but cannot.";
        p["stunErrorRecharge"] = u"Buster %d of player $%d wants to stun buster %d but needs more time.";
        p["release"] = u"Buster %d of player $%d released ghost %d.";
        if (!CONTEST) p["radar"] = u"Buster %d of player $%d used his radar.";
    }

    Lines getInitDataForView() override {
        Lines lines;
        lines.push_back(S(WIDTH) + u" " + S(HEIGHT) + u" " + S(HQ_RADIUS) + u" " + S(bustersPerPlayer) + u" " + S(ghostCount) + u" " + S(VIEW_RANGE) + u" " +
                        S(GRAB_RANGE) + u" " + S(0));
        for (Ghost* g : ghosts) lines.push_back(g->toInitFrameString());
        // (no obstacle)
        lines.insert(lines.begin(), S((i64)lines.size() + 1));
        return lines;
    }

    Lines getFrameDataForView(int round, int, int, bool keyFrame) override {
        Lines lines;
        if (keyFrame) {
            for (PlayerInfo* player : players) {
                lines.push_back(S(getGhostCount(player->index, false)));
                lines.push_back(S(getScore(player->index)));
                for (Buster* b : player->busters) lines.push_back(b->toFrameString());
            }
            std::vector<Ghost*> updatedGhosts;
            for (Ghost* ghost : ghosts)
                if (ghost->hasMoved(round)) updatedGhosts.push_back(ghost);
            lines.push_back(S((i64)updatedGhosts.size()));
            for (Ghost* g : updatedGhosts) lines.push_back(g->toFrameString());
        } else {
            lines.push_back(u"X"); // (never: the frame data is only asked for on new rounds)
        }
        return lines;
    }

    String getGameName() override { return u"CodeBusters"; }
    NString getHeadlineAtGameStartForConsole() override { return String(u"Who you gonna call... ?"); }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }
    Lines getPlayerActions(int, int) override { return {}; } // (null)

    Lines getPlayerSummary(int playerIdx) {
        Lines lines;
        PlayerInfo* player = players[playerIdx];
        int p = player->index;
        for (Buster* b : player->busters) {
            if (CONTEST) {
                if (b->invalidMove) {
                    lines.push_back(translate("invalid", {b->id, p}));
                } else if (b->state == STUNNED) {
                    lines.push_back(translate("stunned", {b->id, p}));
                } else if (b->hasMovement) {
                    if (b->state == CARRYING)
                        lines.push_back(translate("moveAndBust", {b->id, p, java::d2i(b->position.x), java::d2i(b->position.y), argOf(b->grabTarget)}));
                    else
                        lines.push_back(translate("move", {b->id, p, java::d2i(b->position.x), java::d2i(b->position.y)}));
                } else if (b->stunTarget) {
                    int t = *b->stunTarget;
                    if (b->stunSuccess) {
                        if (t == b->id) lines.push_back(translate("autostun", {b->id, p}));
                        else lines.push_back(translate("stun", {b->id, p, t, allBusters[t]->owner->index}));
                    } else if (b->stunRecharge > 0) {
                        lines.push_back(translate("stunErrorRecharge", {b->id, p, t}));
                    } else {
                        lines.push_back(translate("stunError", {b->id, p, t}));
                    }
                } else if (b->state == GRABBING) {
                    lines.push_back(translate("busting", {b->id, p, argOf(b->grabTarget)}));
                } else if (b->trappedGhost()) {
                    lines.push_back(translate("bust", {b->id, p, argOf(b->grabTarget)}));
                } else if (b->release) {
                    lines.push_back(translate("release", {b->id, p, *b->release}));
                } else if (b->attemptGrab && b->grabTarget) {
                    if (b->tooClose) lines.push_back(translate("bustTooClose", {b->id, p, *b->grabTarget}));
                    else lines.push_back(translate("bustError", {b->id, p, *b->grabTarget}));
                }
                continue;
            }
            if (b->invalidMove) {
                lines.push_back(translate("invalid", {b->id, p}));
            } else if (b->state == STUNNED) {
                lines.push_back(translate("stunned", {b->id, p}));
            } else if (b->attemptedMove() && b->hasAttemptedVector) {
                if (b->state == CARRYING) {
                    if (b->ghost != nullptr)
                        lines.push_back(translate("moveAndBust", {b->id, p, java::d2i(b->position.x), java::d2i(b->position.y), b->ghost->id}));
                    else printError("Error: carrying no ghost?");
                } else {
                    lines.push_back(translate("move", {b->id, p, java::d2i(b->position.x), java::d2i(b->position.y)}));
                }
            } else if (b->attempt == Action::STUN) {
                int t = Buster::unbox(b->attemptTargetId);
                if (b->success) {
                    if (t == b->id) lines.push_back(translate("autostun", {b->id, p}));
                    else lines.push_back(translate("stun", {b->id, p, t, allBusters[t]->owner->index}));
                } else if (b->stunRecharge > 0) {
                    lines.push_back(translate("stunErrorRecharge", {b->id, p, t}));
                } else {
                    lines.push_back(translate("stunError", {b->id, p, t}));
                }
            } else if (b->state == GRABBING) {
                lines.push_back(translate("busting", {b->id, p, b->ghostId()}));
            } else if (b->trappedGhost()) {
                lines.push_back(translate("bust", {b->id, p, b->ghostId()}));
            } else if (b->attempt == Action::RELEASE && b->success) {
                lines.push_back(translate("release", {b->id, p, b->ghostId()}));
            } else if (b->attemptedGrab() && !b->success && b->attemptTargetId) {
                if (b->tooClose) lines.push_back(translate("bustTooClose", {b->id, p, *b->attemptTargetId}));
                else lines.push_back(translate("bustError", {b->id, p, *b->attemptTargetId}));
            } else if (b->radarSucceeded()) {
                lines.push_back(translate("radar", {b->id, p}));
            }
        }
        return lines;
    }

    bool isPlayerDead(int playerIdx) override { return players[playerIdx]->dead; }
    NString getDeathReason(int) override { return String(u"Timeout!"); }

    int getGhostCount(int playerIdx, bool countTrapped) {
        int count = 0;
        for (Ghost* g : ghosts) {
            if (distance(g->position, corners[playerIdx]) <= HQ_RADIUS && g->isCaptured()) {
                count++;
            } else if (countTrapped) {
                // players.get(playerIdx)
                if (playerIdx >= (int)players.size()) throw java::Exception{java::indexOutOfBoundsException(playerIdx, (i64)players.size())};
                if (g->isTrappedBy(players[playerIdx])) count++;
            }
        }
        return count;
    }

    int getScore(int playerIdx) override { return getGhostCount(playerIdx, true); }

    Lines getGameSummary(int) override {
        Lines lines;
        for (int i = 0; i < (int)players.size(); ++i) {
            Lines l = getPlayerSummary(i);
            lines.insert(lines.end(), l.begin(), l.end());
        }
        return lines;
    }

    void setPlayerTimeout(int, int, int playerIdx) override { players[playerIdx]->dead = true; }
    int getMillisTimeForRound() override { return 100; }
    int getMaxRoundCount(int) override { return CONTEST ? 200 : 250; }
    // the game's gameOver() (abstract in this base; nextRound() calls it after every update)
    bool gameOver() override { return gameOverFlag; }
};

} // namespace

int main(int argc, char** argv) {
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
