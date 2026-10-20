// The CodinGame game engine 4.x (com.codingame.gameengine core + runner, 4.0.2 to 4.5.0, which only
// differ by getRandom()), and the command line interface of the jars in referees/cg-jar, for C++
// ports of CodinGame multiplayer referees. A port only writes the game: its Referee and Player.
//
//   #include "common/engine.h"
//
//   struct Player : cg::Player {
//       int getExpectedOutputLines() const override { return 1; }
//   };
//   static Player players[4];
//
//   struct Referee : cg::Referee {
//       void init() override { cg::gm.setMaxTurns(100); }
//       void gameTurn(int turn) override {
//           for (cg::Player* p : cg::gm.getActivePlayers()) {
//               p->sendInputLine("42");
//               p->execute();
//           }
//           for (cg::Player* p : cg::gm.getActivePlayers()) {
//               if (p->timeout) p->deactivate(u"Timeout!");
//               else { /* p->outputs */ }
//           }
//       }
//       void onEnd() override { players[0].setScore(1); }
//   };
//
//   int main(int argc, char** argv) {
//       static Referee referee;
//       return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; });
//   }
//
// Usage of the referee (CommandLineInterface of the jars):
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-p3 <cmd> -p4 <cmd>] [-seed <N>] [-l <file>] [-timeout <ms>]
// Prints one score per player ("null" when the game failed) and the game parameters ("seed=N"),
// exit code 0; exit code 1 when the command line is wrong or a bot can't be launched.
// -l writes a trace (the jar writes its JSON game log there):
//   T<turn> P<player> OUT <JSON array of the lines the referee received>   one per execution
//   T<turn> P<player> TIMEOUT
//   V <frame> <UTF-16 length> <UTF-8 bytes>                                 one per view frame
//   VIEW <frame> <JSON string of the frame> (with the environment variable CG_TRACE_VIEWS set)
//   S [<JSON string>] / TT [<JSON string>]: the content of each SUMMARY / TOOLTIP command
//   END OK | END <why the game failed>
// plus anything the port writes to cg::trace.
//
// What is reproduced (everything a referee or a bot can observe):
//   - the referee and the runner are two threads of the jar talking through pipes: the referee
//     reads the runner's commands with java.util.Scanner, which also ends lines at U+0085,
//     U+2028 and U+2029, so a bot line containing one desynchronises the protocol (usually the
//     referee then crashes); the runner reads each referee command into 30000 bytes (100000 in the
//     first round), so a huge game summary, tooltip or view frame makes the game fail
//   - GameManager: the turn loop, frames, the "no player executed" frame, the 800 chars per turn
//     and 512 KB game summary quotas, the 1 MB view data quota (counted in UTF-16 chars, with the
//     platform line separator), the 30 s total turn time, OutputData#toString trimming the inputs
//   - GameRunner: Agent#getOutput (see agent.h), checkOutput, the '\r' -> '\n' replacement and the
//     split of the output in lines, Agent#readError, AsynchronousWriter
//   - when the Java referee throws, or the runner gives up, the game ends without scores: the CLI
//     then prints "null" for every player, like the jar
// Not reproduced: the wall clock of the jar (it reads bot output one byte at a time, slowly, and
// sleeps for the turn time after a timeout), the game log, metadata contents (only their size).
#pragma once

#include "agent.h"
#include "java.h"

#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cg {

using java::String;

// The -l trace (nullptr without -l). Ports may write their own lines to it.
inline FILE* trace = nullptr;

// The -timeout override in ms (0 without -timeout), see turnTimeoutMs in agent.h
inline i64 turnTimeout = 0;

// ----------------------------------------------------------------------------
// View data
// ----------------------------------------------------------------------------

// A JSON value of the view data. Only its size matters to the engine (the jar fails when the view
// data gets too big), so a port that only knows the size can give that.
struct Json {
    std::string text; // UTF-8 (empty when only the size is known)
    java::Size size;  // String#length() and UTF-8 bytes of the JSON text

    static Json raw(const std::string& utf8) {
        Json j;
        j.text = utf8;
        j.size = java::sizeOf(utf8);
        return j;
    }
    static Json sized(java::Size s) {
        Json j;
        j.size = s;
        return j;
    }
    static Json string(const String& s) { return raw(java::jsonQuote(s)); }
    static Json number(i64 v) { return raw(std::to_string(v)); }
};

// A Gson JsonObject: properties in insertion order, setting an existing property keeps its place
struct JsonObject {
    std::vector<std::pair<std::string, Json>> props;

    void set(const std::string& key, const Json& value) {
        for (auto& p : props)
            if (p.first == key) {
                p.second = value;
                return;
            }
        props.push_back({key, value});
    }
    // toString()
    java::Size size() const {
        java::Size s;
        s.ascii(2);
        for (size_t i = 0; i < props.size(); i++) {
            s.add(java::jsonQuoteSize(java::ascii(props[i].first)));
            s.ascii(1); // ':'
            s.add(props[i].second.size);
            if (i) s.ascii(1); // ','
        }
        return s;
    }
    Json toJson() const {
        std::string t = "{";
        for (size_t i = 0; i < props.size(); i++) {
            if (i) t += ",";
            t += java::jsonQuote(java::ascii(props[i].first)) + ":" + props[i].second.text;
        }
        return Json{t + "}", size()};
    }
};

// ----------------------------------------------------------------------------
// Players and referee (what a port implements)
// ----------------------------------------------------------------------------

struct Player; // AbstractMultiplayerPlayer

// A port's referee (AbstractReferee) and its modules (ViewModule, EndScreenModule... their hooks)
struct Referee {
    virtual ~Referee() {}
    virtual void init() = 0;
    virtual void gameTurn(int turn) = 0;
    virtual void onEnd() {}
    virtual void onGameInit() {}      // Module#onGameInit, after init()
    virtual void onAfterGameTurn() {} // Module#onAfterGameTurn, after each gameTurn()
    virtual void onAfterOnEnd() {}    // Module#onAfterOnEnd, after onEnd()
};

// What a port chooses
struct Options {
    AgentOptions agent;
    // More players than this on the command line: exit code 1 before anything is launched (the jars
    // accept up to 4 and leave the game to fail as it does)
    int maxPlayers = 4;
};

struct RunnerStopped {}; // the runner gave up: the Java referee thread is abandoned

// ----------------------------------------------------------------------------
// GameRunner (the runner side of the jar)
// ----------------------------------------------------------------------------
namespace detail {

enum Key { VIEW, INFOS, NEXT_PLAYER_INPUT, NEXT_PLAYER_INFO, SCORES, UINPUT, TOOLTIP, SUMMARY, METADATA, FAIL, NKEYS };
inline const char* KEY_NAME[NKEYS] = {"VIEW", "INFOS", "NEXT_PLAYER_INPUT", "NEXT_PLAYER_INFO", "SCORES", "UINPUT", "TOOLTIP", "SUMMARY", "METADATA", "FAIL"};

const int REFEREE_MAX_BUFFER_SIZE_EXTRA = 100000;
const int REFEREE_MAX_BUFFER_SIZE = 30000;

// A command the referee printed, as the runner reads it: `bytes` is what its content takes in the
// runner's buffer, `text` the content (lines ended by '\n', '\r' replaced by '\n'), when needed
struct OutCmd {
    int key;
    i64 bytes;
    std::string text;
};

struct Runner {
    std::vector<Agent*> agents;
    std::deque<String> inbox;     // lines the referee's Scanner#nextLine will return
    std::deque<OutCmd> outbox;    // commands printed by the referee, not read by the runner yet
    int round = 0;
    bool done = false;            // the runner left its loop
    std::string failCause;        // gameResult.failCause
    OutCmd turnInfo[NKEYS];
    bool has[NKEYS] = {};
    std::map<int, i64> scores;    // gameResult.scores
    std::vector<std::string> uinput;
    std::vector<bool> executedOnce;
    int traceTurn = 0;

    // Command#toString sent to the referee: java.util.Scanner splits it in lines
    void send(const std::string& name, const std::vector<String>& lines) {
        String text = java::ascii("[[" + name + "] " + std::to_string(lines.size()) + "]\n");
        for (const String& l : lines) text += l + u"\n";
        String cur;
        for (size_t i = 0; i < text.size(); i++) {
            char16_t c = text[i];
            if (java::isLineSeparator(c)) {
                if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') i++;
                inbox.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    }

    void startRound() {
        for (bool& h : has) h = false;
        send("GET_GAME_INFO", {});
    }

    void fail(const std::string& cause) {
        if (failCause.empty()) failCause = cause;
        done = true;
    }

    // GameRunner#getNextPlayerOutput + sendPlayerOutput / sendTimeOut
    void runPlayer() {
        std::vector<i64> info;
        for (const String& l : lines(turnInfo[NEXT_PLAYER_INFO].text)) {
            i64 v = 0;
            java::parseLong(l, v);
            info.push_back(v);
        }
        int nextPlayer = (int)info.at(0), nbLines = (int)info.at(1);
        i64 timeout = info.at(2);
        Agent& agent = *agents.at(nextPlayer);
        agent.sendInput(turnInfo[NEXT_PLAYER_INPUT].text);
        std::string raw;
        bool first = nbLines > 0 && !executedOnce[nextPlayer];
        if (nbLines > 0) executedOnce[nextPlayer] = true;
        timeout = turnTimeoutMs(timeout, first, turnTimeout);
        bool ok = agent.getOutput(nbLines, timeout, first, raw);
        agent.readError();
        if (!ok) {
            if (trace) fprintf(trace, "T%d P%d TIMEOUT\n", traceTurn, nextPlayer);
            send("SET_PLAYER_TIMEOUT", {});
            return;
        }
        String output = java::decodeUtf8(raw);
        for (char16_t& c : output)
            if (c == u'\r') c = u'\n';
        // output.split("(\\n|\\r\\n)", -1), the first nbLines pieces
        std::vector<String> pieces;
        size_t off = 0, p;
        while ((int)pieces.size() < nbLines && (p = output.find(u'\n', off)) != String::npos) {
            pieces.push_back(output.substr(off, p - off));
            off = p + 1;
        }
        if (trace) {
            // the frame GameManager creates when no player was executed reads 0 lines: the jar's game
            // log records an empty output for it, traced as [""]
            fprintf(trace, "T%d P%d OUT [%s", traceTurn, nextPlayer, nbLines == 0 ? "\"\"" : "");
            for (size_t i = 0; i < pieces.size(); i++) fprintf(trace, "%s%s", i ? "," : "", java::traceJson(pieces[i]).c_str());
            fprintf(trace, "]\n");
        }
        send("SET_PLAYER_OUTPUT", pieces);
    }

    static std::vector<String> lines(const std::string& text) {
        std::vector<String> r;
        size_t off = 0, p;
        while ((p = text.find('\n', off)) != std::string::npos) {
            r.push_back(java::decodeUtf8(text.substr(off, p - off)));
            off = p + 1;
        }
        if (off < text.size()) r.push_back(java::decodeUtf8(text.substr(off)));
        return r;
    }

    // GameRunner#readCommand + the body of the runAgents loop, for the next command of the referee
    void consume(const OutCmd& c) {
        if (c.key != FAIL) {
            i64 limit = round == 0 ? REFEREE_MAX_BUFFER_SIZE_EXTRA : REFEREE_MAX_BUFFER_SIZE;
            if (c.bytes > limit) {
                fail(std::string("java.lang.RuntimeException: Error reading Referee ") + KEY_NAME[c.key] + " command. Buffer capacity: " + std::to_string(limit) + " / " +
                     std::to_string(limit));
                return;
            }
        }
        turnInfo[c.key] = c;
        has[c.key] = true;
        if (has[FAIL]) {
            fail(c.text.empty() ? "FAIL" : c.text);
            return;
        }
        bool normalTurn = has[NEXT_PLAYER_INPUT] && has[VIEW] && has[NEXT_PLAYER_INFO] && has[INFOS];
        bool endTurn = has[SCORES] && has[VIEW] && has[INFOS];
        if (!normalTurn && !endTurn) return; // keeps reading
        if (!has[SCORES]) runPlayer();
        if (has[UINPUT]) uinput.push_back(turnInfo[UINPUT].text);
        if (has[SCORES]) {
            for (const String& line : lines(turnInfo[SCORES].text)) {
                size_t sp = line.find(u' ');
                if (sp == String::npos) continue;
                String a = line.substr(0, sp), b = line.substr(sp + 1);
                size_t sp2 = b.find(u' ');
                if (sp2 != String::npos) b = b.substr(0, sp2);
                i64 player, score;
                if (java::parseLong(a, player, INT32_MIN, INT32_MAX) && java::parseLong(b, score, INT32_MIN, INT32_MAX)) scores[(int)player] = score;
            }
        }
        round++;
        if (endTurn) {
            done = true;
            return;
        }
        startRound();
    }

    // lets the runner read the next command the referee printed; false when there is none
    bool step() {
        if (done || outbox.empty()) return false;
        OutCmd c = outbox.front();
        outbox.pop_front();
        consume(c);
        return true;
    }

    // the referee printed a command: the runner reads it at once (it is always waiting for one)
    void receive(const OutCmd& c) {
        outbox.push_back(c);
        while (step()) {
        }
    }
};

inline Runner runner;

// How the runner reads the content of a command printed with println: `content` is what OutputData
// wrote after the header line (lines separated by '\n'). Returns the bytes it takes in the runner's
// buffer and, if wanted, the text the runner gets ('\r' -> '\n', every line ended by '\n').
inline i64 readAsRunner(const std::string& content, int lineCount, std::string* text) {
    if (lineCount == 0) {
        if (text) text->clear();
        return 0;
    }
    i64 bytes = 1; // the line separator of println ("\r\n" counts once)
    size_t crlf = 0;
    for (size_t i = 0; i + 1 < content.size(); i++)
        if (content[i] == '\r' && content[i + 1] == '\n') crlf++;
    bytes += (i64)content.size() - (i64)crlf;
    if (text) {
        std::string t;
        t.reserve(content.size() + 1);
        for (size_t i = 0; i < content.size(); i++) {
            char c = content[i];
            if (c == '\r') {
                t += '\n';
                if (i + 1 < content.size() && content[i + 1] == '\n') i++;
            } else {
                t += c;
            }
        }
        *text = t + "\n";
    }
    return bytes;
}

// OutputData#toString: the lines joined with '\n' and trimmed; the line count splits on
// "\r\n|\r|\n" (on UTF-8 bytes: the trimmed chars are all single bytes)
inline std::string outputContent(const std::vector<std::string>& lines, int& lineCount) {
    std::string sb;
    for (const std::string& l : lines) {
        sb += l;
        sb += '\n';
    }
    size_t a = 0, b = sb.size();
    while (a < b && (u8)sb[a] <= ' ') a++;
    while (b > a && (u8)sb[b - 1] <= ' ') b--;
    std::string content = sb.substr(a, b - a);
    lineCount = 0;
    if (!content.empty()) {
        lineCount = 1;
        for (size_t i = 0; i < content.size(); i++) {
            if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') i++;
            if (content[i] == '\r' || content[i] == '\n') lineCount++;
        }
        // String#split drops trailing empty strings: never happens after trim()
    }
    return content;
}

} // namespace detail

// ----------------------------------------------------------------------------
// GameManager / MultiplayerGameManager (the referee side of the jar)
// ----------------------------------------------------------------------------
class GameManager {
  public:
    static const int VIEW_DATA_TOTAL_HARD_QUOTA = 1024 * 1024;
    static const int GAME_SUMMARY_TOTAL_HARD_QUOTA = 512 * 1024;
    static const int GAME_SUMMARY_PER_TURN_HARD_QUOTA = 800;
    static const int GAME_DURATION_HARD_QUOTA = 30000;
    static const int MAX_TURN_TIME = 25000;
    static const int MIN_TURN_TIME = 50;
#ifdef _WIN32
    static const int LINE_SEPARATOR_LENGTH = 2; // System.lineSeparator(), written by println
#else
    static const int LINE_SEPARATOR_LENGTH = 1;
#endif

    std::vector<Player*> players;
    int turn = 0;  // the current turn (1 for the first)
    int frame = 0; // the next view frame

    // --- MultiplayerGameManager API ---
    const std::vector<Player*>& getPlayers() const { return players; }
    std::vector<Player*> getActivePlayers() const;
    int getPlayerCount() const { return (int)players.size(); }
    Player* getPlayer(int i) const {
        if (i < 0 || i >= (int)players.size())
            throw java::Exception{"java.lang.IndexOutOfBoundsException: Index " + std::to_string(i) + " out of bounds for length " + std::to_string(players.size())};
        return players[i];
    }
    i64 getSeed() const { return seed; }
    // the SecureRandom("SHA1PRNG") seeded with the seed (engine 4.4+)
    java::SHA1PRNG& getRandom() { return random; }
    int getLeagueLevel() const { return 19; } // the CLI sets the highest league

    void endGame() { gameEnd = true; }
    bool isGameEnd() const { return gameEnd; }
    void setMaxTurns(int n) {
        if (n <= 0) throw java::Exception{"java.lang.IllegalArgumentException: Invalid maximum number of turns"};
        maxTurns = n;
    }
    int getMaxTurns() const { return maxTurns; }
    void setTurnMaxTime(int t) {
        checkTurnTime(t);
        turnMaxTime = t;
    }
    void setFirstTurnMaxTime(int t) {
        checkTurnTime(t);
        firstTurnMaxTime = t;
    }
    int getTurnMaxTime() const { return turnMaxTime; }
    int getFirstTurnMaxTime() const { return firstTurnMaxTime; }
    void setFrameDuration(int d) {
        if (d <= 0) throw java::Exception{"java.lang.IllegalArgumentException: Invalid frame duration: only positive frame duration is supported"};
        if (frameDuration != d) {
            frameDuration = d;
            currentViewData.set("duration", Json::number(d));
        }
    }
    int getFrameDuration() const { return frameDuration; }

    void addToGameSummary(const String& summary) {
        i64 total = 0;
        for (const String& s : currentGameSummary) total += (i64)s.size();
        if (total < GAME_SUMMARY_PER_TURN_HARD_QUOTA && total + totalGameSummaryBytes < GAME_SUMMARY_TOTAL_HARD_QUOTA) {
            currentGameSummary.push_back(summary);
            totalGameSummaryBytes += total; // sic: the size before this summary
        }
    }
    void addToGameSummary(const std::string& utf8) { addToGameSummary(java::decodeUtf8(utf8)); }
    void addTooltip(int player, const String& message) { currentTooltips.push_back({player, message}); }
    void addTooltip(int player, const std::string& utf8) { addTooltip(player, java::decodeUtf8(utf8)); }
    void putMetadata(const std::string& key, const std::string& value) { metadata[key] = value; }

    // ViewModule & co: setViewData(moduleName, data) / setViewGlobalData(moduleName, data)
    void setViewData(const std::string& module, const Json& data) { currentViewData.set(module, data); }
    void setViewGlobalData(const std::string& module, const Json& data) {
        if (initDone) throw java::Exception{"java.lang.IllegalStateException: Impossible to send global data to view outside of init phase"};
        globalViewData.set(module, data);
    }

    static String formatSuccessMessage(const String& m) { return u"¤GREEN¤" + m + u"§GREEN§"; }
    static String formatErrorMessage(const String& m) { return u"¤RED¤" + m + u"§RED§"; }

    // GameManager#execute(player, nbrOutputLines): AbstractPlayer#execute uses getExpectedOutputLines()
    void execute(Player& player, int nbrOutputLines);

    // --- the engine ---
    // GameManager#start, after the runner sent INIT. Ends when the game is over or the runner gave up.
    void start(Referee& referee);
    void setup(i64 s, const std::vector<Player*>& ps) {
        seed = s;
        players = ps;
        random = java::SHA1PRNG();
        random.setSeed(s);
    }

  private:
    i64 seed = 0;
    java::SHA1PRNG random;
    int maxTurns = 200;
    int turnMaxTime = 50;
    int firstTurnMaxTime = 1000;
    bool gameEnd = false;
    bool newTurn = false;
    bool initDone = false;
    int frameDuration = 1000;
    JsonObject currentViewData, prevViewData, globalViewData;
    std::vector<String> currentGameSummary, prevGameSummary;
    bool hasPrevGameSummary = false;
    std::vector<std::pair<int, String>> currentTooltips, prevTooltips;
    std::map<std::string, std::string> metadata;
    i64 totalViewDataBytesSent = 0;
    i64 totalGameSummaryBytes = 0;
    i64 totalTurnTime = 0;
    bool traceViews = getenv("CG_TRACE_VIEWS") != nullptr; // -l also gets the text of every view frame

    static void checkTurnTime(int t) {
        if (t < MIN_TURN_TIME) throw java::Exception{"java.lang.IllegalArgumentException: Invalid turn max time : stay above 50ms"};
        if (t > MAX_TURN_TIME) throw java::Exception{"java.lang.IllegalArgumentException: Invalid turn max time : stay under 25s"};
    }

    // out.println(OutputData): the runner reads it
    static void print(int key, const std::vector<std::string>& lines, bool keepText) {
        int lineCount;
        std::string content = detail::outputContent(lines, lineCount);
        detail::OutCmd c{key, 0, ""};
        c.bytes = detail::readAsRunner(content, lineCount, keepText ? &c.text : nullptr);
        detail::runner.receive(c);
    }
    static void print(int key, const std::vector<String>& lines, bool keepText) {
        std::vector<std::string> u;
        for (const String& l : lines) u.push_back(java::encodeUtf8(l));
        print(key, u, keepText);
    }

    // Scanner#nextLine of the referee: waits for the runner
    static String nextLine() {
        detail::Runner& r = detail::runner;
        if (r.done) throw RunnerStopped();
        if (r.inbox.empty()) {
            // each side waits for the other: after 150 s the runner gives up
            r.fail("java.lang.RuntimeException: Invalid Referee command: ");
            throw RunnerStopped();
        }
        String l = r.inbox.front();
        r.inbox.pop_front();
        return l;
    }

    enum InCmd { INIT, GET_GAME_INFO, SET_PLAYER_OUTPUT, SET_PLAYER_TIMEOUT };

    // InputCommand.parse: "\[\[(?<cmd>.+)\] ?(?<lineCount>[0-9]+)\]" (matches), Command.valueOf,
    // Integer.parseInt
    static InCmd parseCommand(const String& line, int& lineCount) {
        static const char* NAMES[4] = {"INIT", "GET_GAME_INFO", "SET_PLAYER_OUTPUT", "SET_PLAYER_TIMEOUT"};
        size_t n = line.size();
        bool matched = false;
        String name, digits;
        if (n >= 6 && line[0] == u'[' && line[1] == u'[' && line[n - 1] == u']') {
            for (size_t k = n - 2; k >= 3 && !matched; k--) { // greedy .+: the longest name first
                if (line[k] != u']') continue;
                size_t a = k + 1, b = n - 1;
                if (a < b && line[a] == u' ') a++;
                if (a >= b) continue;
                bool allDigits = true;
                for (size_t i = a; i < b; i++)
                    if (!java::isRegexDigit(line[i])) allDigits = false;
                bool dotOk = true; // '.' does not match line terminators
                for (size_t i = 2; i < k; i++)
                    if (java::isLineSeparator(line[i])) dotOk = false;
                if (!allDigits || !dotOk) continue;
                matched = true;
                name = line.substr(2, k - 2);
                digits = line.substr(a, b - a);
            }
        }
        if (!matched) throw java::Exception{"java.lang.RuntimeException: Error in data sent to referee"};
        int found = -1;
        for (int i = 0; i < 4; i++)
            if (name == java::ascii(NAMES[i])) found = i;
        if (found < 0)
            throw java::Exception{"java.lang.IllegalArgumentException: No enum constant com.codingame.gameengine.core.InputCommand.Command." + java::encodeUtf8(name)};
        i32 v;
        if (!java::parseInt(digits, v)) throw java::Exception{"java.lang.NumberFormatException: For input string: \"" + java::encodeUtf8(digits) + "\""};
        lineCount = v;
        return (InCmd)found;
    }
    static const char* inCmdName(InCmd c) {
        static const char* NAMES[4] = {"INIT", "GET_GAME_INFO", "SET_PLAYER_OUTPUT", "SET_PLAYER_TIMEOUT"};
        return NAMES[c];
    }

    void dumpFail(const java::Exception& e) {
        detail::runner.receive(detail::OutCmd{detail::FAIL, 0, e.text});
    }

    void dumpView() {
        java::Size content;
        if (newTurn) {
            content.ascii("KEY_FRAME " + std::to_string(frame) + "\n");
            if (turn == 1) {
                JsonObject init;
                init.set("global", Json::sized(globalViewData.size()));
                init.set("frame", Json::sized(prevViewData.size()));
                content.add(init.size());
            } else {
                content.add(prevViewData.size());
            }
        } else {
            content.ascii("INTERMEDIATE_FRAME " + std::to_string(frame));
        }
        int lineCount = newTurn ? 2 : 1;
        // viewData = OutputData#toString: "[[VIEW] n]" + line separator + content
        totalViewDataBytesSent += (i64)(9 + std::to_string(lineCount).size()) + LINE_SEPARATOR_LENGTH + content.u16;
        if (totalViewDataBytesSent > VIEW_DATA_TOTAL_HARD_QUOTA)
            throw java::Exception{"java.lang.RuntimeException: The amount of data sent to the viewer is too big!"};
        if (trace) {
            fprintf(trace, "V %d %lld %lld\n", frame, (long long)content.u16, (long long)content.u8);
            if (traceViews) {
                // the frame as the jar sends it to the viewer (JSON values known only by their size are empty)
                std::string text;
                if (newTurn) {
                    text = "KEY_FRAME " + std::to_string(frame) + "\n";
                    if (turn == 1) text += "{\"global\":" + globalViewData.toJson().text + ",\"frame\":" + prevViewData.toJson().text + "}";
                    else text += prevViewData.toJson().text;
                } else {
                    text = "INTERMEDIATE_FRAME " + std::to_string(frame);
                }
                fprintf(trace, "VIEW %d %s\n", frame, java::traceJson(java::decodeUtf8(text)).c_str());
            }
        }
        detail::runner.receive(detail::OutCmd{detail::VIEW, content.u8 + 1, ""});
        frame++;
    }

    void dumpInfos() {
        print(detail::INFOS, std::vector<std::string>{}, false);
        if (newTurn && hasPrevGameSummary) {
            if (trace) traceLines("S", prevGameSummary);
            print(detail::SUMMARY, prevGameSummary, false);
        }
        if (newTurn && !prevTooltips.empty()) {
            std::vector<String> data;
            for (auto& t : prevTooltips) {
                data.push_back(t.second);
                data.push_back(java::str(t.first));
            }
            if (trace) traceLines("TT", data);
            print(detail::TOOLTIP, data, false);
        }
    }

    // the content of a SUMMARY / TOOLTIP command: the lines joined with '\n', trimmed
    static void traceLines(const char* tag, const std::vector<String>& lines) {
        String content;
        for (const String& l : lines) content += l + u"\n";
        fprintf(trace, "%s [%s]\n", tag, java::traceJson(java::trim(content)).c_str());
    }

    void swapInfoAndViewData() {
        prevViewData = currentViewData;
        currentViewData = JsonObject();
        prevGameSummary.swap(currentGameSummary);
        currentGameSummary.clear();
        hasPrevGameSummary = true;
        prevTooltips.swap(currentTooltips);
        currentTooltips.clear();
    }

    void dumpEnd() {
        // dumpGameProperties: "[[UINPUT] 1]" then "seed=N", printed as is
        std::string params = "seed=" + std::to_string(seed);
        detail::OutCmd u{detail::UINPUT, 0, ""};
        u.bytes = detail::readAsRunner(params, 1, &u.text);
        detail::runner.receive(u);
        // dumpMetadata: the JSON of a HashMap<String, String> (only its size matters)
        std::string json = "{";
        for (auto& kv : metadata) {
            if (json.size() > 1) json += ",";
            json += java::jsonQuote(java::decodeUtf8(kv.first)) + ":" + java::jsonQuote(java::decodeUtf8(kv.second));
        }
        print(detail::METADATA, std::vector<std::string>{json + "}"}, false);
        std::vector<std::string> scores;
        for (Player* p : players) scores.push_back(std::to_string(playerIndex(p)) + " " + std::to_string(playerScore(p)));
        print(detail::SCORES, scores, true);
    }

    static int playerIndex(const Player* p);
    static int playerScore(const Player* p);
};

inline GameManager gm;

// AbstractMultiplayerPlayer
struct Player {
    int index = 0;
    bool active = true;
    int score = 0;
    std::vector<std::string> inputs; // sendInputLine since the last execution (UTF-8)
    std::vector<String> outputs;     // what the bot answered at its last execution (getOutputs)
    bool timeout = false;            // its last execution timed out (getOutputs throws TimeoutException)
    bool hasBeenExecuted = false;
    bool hasNeverBeenExecuted = true;

    virtual ~Player() {}
    virtual int getExpectedOutputLines() const = 0;

    void sendInputLine(const std::string& utf8) {
        if (hasBeenExecuted) throw java::Exception{"java.lang.RuntimeException: Impossible to send new inputs after calling execute"};
        inputs.push_back(utf8);
    }
    void sendInputLine(const String& line) { sendInputLine(java::encodeUtf8(line)); }
    // AbstractPlayer#execute
    void execute() {
        gm.execute(*this, getExpectedOutputLines());
        hasBeenExecuted = true;
        hasNeverBeenExecuted = false;
    }
    void deactivate() { active = false; }
    void deactivate(const String& reason) {
        active = false;
        gm.addTooltip(index, reason);
    }
    void deactivate(const std::string& utf8) { deactivate(java::decodeUtf8(utf8)); }
    bool isActive() const { return active; }
    int getIndex() const { return index; }
    int getScore() const { return score; }
    void setScore(int s) { score = s; }
    String getNicknameToken() const { return u"$" + java::str(index); }
};

inline int GameManager::playerIndex(const Player* p) { return p->index; }
inline int GameManager::playerScore(const Player* p) { return p->score; }

inline std::vector<Player*> GameManager::getActivePlayers() const {
    std::vector<Player*> r;
    for (Player* p : players)
        if (p->active) r.push_back(p);
    return r;
}

inline void GameManager::execute(Player& player, int nbrOutputLines) {
    try {
        if (!initDone) throw java::Exception{"java.lang.RuntimeException: Impossible to execute a player during init phase."};
        player.timeout = false;
        int lineCount;
        InCmd cmd = parseCommand(nextLine(), lineCount);
        if (cmd != GET_GAME_INFO) throw java::Exception{std::string("java.lang.RuntimeException: Invalid command: ") + inCmdName(cmd)};
        dumpView();
        dumpInfos();
        print(detail::NEXT_PLAYER_INPUT, player.inputs, true);
        if (nbrOutputLines > 0) {
            totalTurnTime += turnMaxTime;
            if (totalTurnTime > GAME_DURATION_HARD_QUOTA) throw java::Exception{"java.lang.RuntimeException: Total game duration too long (>30000ms)"};
        }
        int timeout = player.hasNeverBeenExecuted ? firstTurnMaxTime : turnMaxTime;
        print(detail::NEXT_PLAYER_INFO, std::vector<std::string>{std::to_string(player.index), std::to_string(nbrOutputLines), std::to_string(timeout)}, true);

        // READ PLAYER OUTPUTS
        cmd = parseCommand(nextLine(), lineCount);
        if (cmd == SET_PLAYER_OUTPUT) {
            std::vector<String> output;
            for (int i = 0; i < lineCount; i++) output.push_back(nextLine());
            player.outputs = output;
        } else if (cmd == SET_PLAYER_TIMEOUT) {
            player.timeout = true;
        } else {
            throw java::Exception{std::string("java.lang.RuntimeException: Invalid command: ") + inCmdName(cmd)};
        }
        player.inputs.clear();
        newTurn = false;
    } catch (const java::Exception& e) {
        dumpFail(e);
        throw;
    }
}

inline void GameManager::start(Referee& referee) {
    try {
        referee.init();
        referee.onGameInit();
        initDone = true;
        for (turn = 1; (detail::runner.traceTurn = turn) <= maxTurns && !gameEnd && !getActivePlayers().empty(); turn++) {
            swapInfoAndViewData();
            newTurn = true;
            referee.gameTurn(turn);
            referee.onAfterGameTurn();
            // a frame is created if no player has been executed
            bool executed = false;
            for (Player* p : players) executed = executed || p->hasBeenExecuted;
            if (!players.empty() && !executed) execute(*players[0], 0);
            for (Player* p : players) {
                p->outputs.clear();
                p->hasBeenExecuted = false;
            }
        }
        referee.onEnd();
        referee.onAfterOnEnd();
        // the last frame
        swapInfoAndViewData();
        newTurn = true;
        dumpView();
        dumpInfos();
        dumpEnd();
    } catch (const java::Exception& e) {
        dumpFail(e);
    }
}

// ----------------------------------------------------------------------------
// CommandLineInterface + GameRunner#runGame
// ----------------------------------------------------------------------------
inline const char* CLI_HELP =
    "usage: -p1 <player1 command line> -p2 <player2 command line> [-p3 <cmd>\n"
    "           -p4 <cmd> -l <log file> -seed <seed> -timeout <ms>]\n"
    " -h               Print the help\n"
    " -l <arg>         File output for logs\n"
    " -p1 <arg>        Required. Player 1 command line\n"
    " -p2 <arg>        Required. Player 2 command line\n"
    " -p3 <arg>        Player 3 command line (if applies)\n"
    " -p4 <arg>        Player 4 command line (if applies)\n"
    " -seed <arg>      Referee seed\n"
    " -timeout <arg>   Turn time limit in ms, replacing the game's (first turn:\n"
    "                  the longest of both)\n";

// Plays one game like the jar's CommandLineInterface: returns the exit code. `player(i)` gives the
// port's player object of index i (0 to 3).
inline int run(int argc, char** argv, Referee& referee, const std::function<Player*(int)>& player, const Options& options = Options()) {
    // org.apache.commons.cli DefaultParser: -h, -p1..-p4, -l, -seed, -timeout
    const char* names[8] = {"p1", "p2", "p3", "p4", "l", "seed", "h", "timeout"};
    std::string values[8];
    bool has[8] = {false, false, false, false, false, false, false, false};
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a.size() < 2 || a[0] != '-') continue; // a plain argument: ignored
        int opt = -1;
        for (int k = 0; k < 8; k++)
            if (a.substr(1) == names[k]) opt = k;
        if (opt < 0) {
            fprintf(stderr, "org.apache.commons.cli.UnrecognizedOptionException: Unrecognized option: %s\n", a.c_str());
            return 1;
        }
        if (opt == 6) {
            has[opt] = true;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "org.apache.commons.cli.MissingArgumentException: Missing argument for option: %s\n", names[opt]);
            return 1;
        }
        if (!has[opt]) values[opt] = argv[i + 1]; // getOptionValue returns the first value
        has[opt] = true;
        i++;
    }
    if (has[6] || !has[0] || !has[1]) {
        fputs(CLI_HELP, stdout);
        return 0;
    }
    i64 seed;
    if (has[5]) {
        if (!java::parseLong(java::decodeUtf8(values[5]), seed)) {
            fprintf(stderr, "java.lang.NumberFormatException: For input string: \"%s\"\n", values[5].c_str());
            return 1;
        }
    } else {
        seed = (i64)java::entropy(); // System.nanoTime() + new Object().hashCode()
    }
    if (has[7]) {
        if (!java::parseLong(java::decodeUtf8(values[7]), turnTimeout)) {
            fprintf(stderr, "java.lang.NumberFormatException: For input string: \"%s\"\n", values[7].c_str());
            return 1;
        }
    }
    std::vector<std::string> cmds;
    for (int k = 0; k < 4; k++)
        if (has[k]) cmds.push_back(values[k]);
    if ((int)cmds.size() > options.maxPlayers) {
        fprintf(stderr, "this referee supports %d players at most\n", options.maxPlayers);
        return 1;
    }
    if (has[4]) trace = fopen(values[4].c_str(), "w");

    // GameRunner#initialize: launches the bots
    int n = (int)cmds.size();
    std::vector<Agent> agents(n);
    for (int i = 0; i < n; i++) {
        if (!agents[i].start(cmds[i], options.agent)) {
            fprintf(stderr, "java.lang.RuntimeException: Failed to launch %s\n", cmds[i].c_str());
            for (int k = 0; k < i; k++) agents[k].destroy();
            if (trace) fclose(trace);
            return 1;
        }
    }
    std::vector<Player*> players;
    for (int i = 0; i < n; i++) {
        Player* p = player(i);
        p->index = i;
        players.push_back(p);
    }
    detail::Runner& r = detail::runner;
    for (Agent& a : agents) r.agents.push_back(&a);
    r.executedOnce.assign(n, false);
    gm.setup(seed, players);

    // runAgents: the bots' stderr is read once after their launch, the referee gets INIT then
    // GET_GAME_INFO
    for (Agent& a : agents) a.readError();
    r.startRound();
    try {
        gm.start(referee);
    } catch (const RunnerStopped&) {
    }
    while (r.step()) {
    }
    if (!r.done) r.fail("java.lang.RuntimeException: Invalid Referee command: ");

    for (Agent& a : agents) a.destroy();
    if (trace) {
        fprintf(trace, "END %s\n", r.scores.empty() ? r.failCause.c_str() : "OK");
        fclose(trace);
    }
    // result.scores.get(i), then the game parameters
    std::string out;
    for (int i = 0; i < n; i++) {
        auto it = r.scores.find(i);
        out += (it == r.scores.end() ? std::string("null") : std::to_string(it->second)) + "\n";
    }
    for (const std::string& u : r.uinput) out += u + "\n";
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
    return 0;
}

} // namespace cg
