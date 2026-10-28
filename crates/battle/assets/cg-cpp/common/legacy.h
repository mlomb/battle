// The pre-SDK CodinGame referees (2014-2017): a single Java file `class Referee extends MultiReferee`,
// whose base `MultiReferee extends AbstractReferee` (CodinGame's old referee base, embedded at the end
// of the file) runs the whole game in its constructor, speaking the text protocol of the runner. The
// jars of these games (referees/cg-builder/legacy) run that referee unchanged in the runner of the SDK
// 4.5.0, which still speaks the same protocol: so the runner side here is common/engine.h's (agents,
// time limits, output lines, buffers: cg::detail::Runner), and this header is the referee side: the
// old AbstractReferee + MultiReferee, with options for the differences between the versions of the
// base the games embed. A port only writes the game (the `Referee extends MultiReferee` class).
//
//   #include "common/legacy.h"
//
//   struct Referee : legacy::MultiReferee {
//       void initReferee(int playerCount, java::Properties& prop) override { ... }
//       java::Properties getConfiguration() override { ... }
//       void handlePlayerOutput(int frame, int round, int playerIdx, const std::vector<String>& outputs) override {
//           ... throw legacy::InvalidInputException(u"MOVE x y", outputs[0]); ...
//       }
//       ... (every abstract method of the base, same names)
//   };
//
//   int main(int argc, char** argv) {
//       static Referee referee;
//       return legacy::run(argc, argv, referee);
//   }
//
// The protocol (the referee reads the runner's commands with a java.util.Scanner, prints its own with
// println, as OutputData: "[[NAME] n]" then n lines, trimmed):
//   runner -> referee: [[INIT] n] + the player count + n-1 lines "key=value" (the CLI's "seed=N"),
//                      [[GET_GAME_INFO] 0], [[SET_PLAYER_OUTPUT] n] + the bot's lines, [[SET_PLAYER_TIMEOUT] 0]
//   referee -> runner: VIEW, INFOS, SUMMARY, TOOLTIP (text and player pairs), NEXT_PLAYER_INPUT,
//                      NEXT_PLAYER_INFO (player, expected lines, time limit ms), at the end UINPUT
//                      (MultiReferee: getConfiguration() as "key=value" lines) and SCORES ("i score")
// What the old base does (AbstractReferee#start): INIT reads the player count and the properties
// (MultiReferee: initReferee, then getConfiguration() replaces them for the end). Each GET_GAME_INFO
// moves to the next player that has neither lost nor won (nextPlayer); past the last one a new round
// starts (nextRound): round 0 has no update, then updateGame(round), updateScores (isPlayerDead ->
// lost + getDeathReason tooltip, every score = getScore) and gameOver(); then the max rounds check,
// the view (KEY_FRAME on a new round, INTERMEDIATE_FRAME otherwise), the infos / summary / tooltips,
// on a new round prepare(round) and (not turn based) every player's input of the round, then the
// player's input (with the init input in round 0) and info (getMillisTimeForFirstRound in round 0,
// getMillisTimeForRound after). SET_PLAYER_OUTPUT: handlePlayerOutput; a LostException or
// InvalidInputException makes the player lose (score = getScore, info = the reason, a tooltip), a
// WinException win; SET_PLAYER_TIMEOUT: setPlayerTimeout and the player loses. Losing or winning
// ends the game at once only below getMinimumPlayerCount() players left (and, for a loss, in a turn
// based game): otherwise at the next new round (gameOver()). (Wondev Woman's base, Base::wondevWoman2017():
// a loss never ends the game at once; nextRound() and, in a turn based game, every GET_GAME_INFO after
// nextPlayer() end it when the game's gameIsOver(); every turn of a turn based game is a new round for
// the view, the summary and prepare().) GameOverException (thrown by the base or
// the game): the last view and infos, prepare, updateScores, then UINPUT and SCORES: the scores are
// those of the last updateScores, whatever the players' state (a dead player's score is getScore,
// ties are the game's). An init error (handleInitInputForReferee throws): an error view, then UINPUT
// (the init properties) and SCORES of 0. Any other exception (Java's Throwable) ends the referee
// thread without scores: the runner waits 150 s for its next command and fails (the CLI prints "null"
// for every player; here at once).
//
// Reproduced, besides the game's own logic: the bytes of every command as the runner reads them
// (OutputData#toString counts its lines before trimming: a last line that trims away desynchronises
// the protocol and the game fails like it does in the jar; println's platform line separator;
// unpaired surrogates written as '?'; the runner's 30000-byte buffer per command, 100000 in its
// first round), the runner reading command by command (cg::detail::Runner), a runner that waits for
// a command the referee will never print (the jar fails after 150 s, here at once), the runner's
// crash on a tooltip whose player is not a number (a null tooltip text is not printed: exit code 1,
// nothing on stdout), the Scanner splitting the bots' lines at U+0085 / U+2028 / U+2029, the order of
// the game parameters (java::Properties, a ConcurrentHashMap since JDK 9).
// Not reproduced: the order of the tooltips of a frame (a HashSet of identity hash codes: only their
// size counts), the wall clock (see engine.h), a race of the jar's first round (a command of more
// than 30000 bytes in the runner's first round may be cut short when the runner reads faster than
// the referee writes), a referee looping forever (cut after 10^6 rounds without a player to play:
// the referee "crashes").
//
// The versions of the base (legacy::Base, the AbstractReferee embedded in each game's file; the
// SpiritusSancti5/codinGame referees, 2014-2017). The default is Hypersonic's (2016) base; validated so
// far: Hypersonic (the default Base, cg-2016-hypersonic), Coders of the Caribbean (Base::cotc2017(),
// cg-2017-coders-of-the-caribbean), Ghost in the Cell (Base::cotc2017(), cg-2017-ghost-in-the-cell: also its
// LostException path with the "LostTooltip", translate() of codes without a message, the game's own
// tooltips, many per frame), Code4Life (Base::code4life2017(), cg-2017-code4life: also the tooltip code of
// its LostExceptions, the game summaries of every round on the 2017 base (SUMMARY, the colored reason of
// the end), two players lost in one round) and Wondev Woman (Base::wondevWoman2017(), cg-2017-wondev-woman:
// the first turn based game: per-turn inputs, frame data / tooltips / summaries every turn, the reason
// in INFOS at the end, a player out skipped by nextPlayer() for the rest of the game, the gameIsOver()
// hook; also against CodinGame's own replays) and Mean Max (Base::meanMax2017(), cg-2017-mean-max: 3 players,
// the end rule of a non turn based game in nextRound() (its isGameOver()), several players out in one
// round, a lone survivor playing on, everyone out, the SDK's OutputData#toString (a "" headline); also
// against CodinGame's own replays) and Fantastic Bits (Base::fantasticBits2016(), cg-2016-fantastic-bits and
// cg-2016-fantastic-bits-contest: also a non-empty getPlayerActions in the INFOS of every turn, two
// players out in one round; against CodinGame's own replays too) and Code Busters (Base::codeBusters2016(),
// cg-2016-codebusters and cg-2016-codebusters-contest: the game's own gameOver() called by nextRound(), a lone
// survivor playing on until the game's rule ends it, both players out; against CodinGame's own replays
// too) and Smash The Code (Base::smashTheCode2016(), cg-2016-smash-the-code: the alive count test of nextRound()
// without a hook, LostExceptions with the "LostTooltip", both players out in one round, the game's
// tooltips; against CodinGame's own replays too) and Coders Strike Back (Base::codersStrikeBack2015(),
// cg-2016-coders-strike-back and cg-2016-coders-strike-back-original: inputs asked when each player plays, the frame
// data and tooltips of every frame, the "InvalidActionTooltip" of every loss, non-empty getPlayerActions,
// 2 to 4 players, deaths by isPlayerDead in the update; against CodinGame's own replays too) and Back to the
// Code (Base::codersStrikeBack2015() as is, cg-2015-back-to-the-code: getMinimumPlayerCount() 1 (a player left alone
// plays on until the game's own end rules or everyone out), a getMaxRoundCount that grows during the game, the
// game's own GameOverExceptions from updateGame with their reason code in the last view and summary, the
// "InvalidActionTooltip" of a LostException, 2 to 4 players; against CodinGame's own replays too) and The Great
// Escape (Base::greatEscape2014(), cg-2015-great-escape: the first 2014 base; WinExceptions with the win state and the
// "WinTooltip", a winner skipped by nextPlayer() while the others play on (3 players), the two tooltips of a timeout,
// the 2014 INFOS / SUMMARY, the game's getMaxRoundCount() 100; against CodinGame's own replays too) and Platinum Rift
// (Base::platinumRift2014(), cg-2014-platinum-rift-episode-1: the scores updated at every step of nextPlayer(), a player out by
// isPlayerDead found at the next step, the reason code "timeout" of the last view, getMinimumPlayerCount() 1 with 2 to 4
// players, the game's GameOverExceptions from updateGame, "$N: invalid action" / "$N: timeout!" / the death reason as
// tooltips; against CodinGame's own replays too) and Platinum Rift 2 (Base::platinumRift2_2015(), cg-2014-platinum-rift-episode-2
// and cg-2014-platinum-rift-episode-2-original: the INFOS_ONLY infos, i.e. the actions of every frame and the summary of every new
// round in the INFOS, after the reason of the end too; 2 players, the rest as Platinum Rift; against CodinGame's own
// replays too) and Poker Chip Race (Base::pokerChipRace2014(), cg-2014-poker-chip-race: no tooltip for a loss by an exception,
// the timeout's info as its tooltip, the scores updated after updateGame only, 2 to 4 players with players out by
// isPlayerDead, timeouts and invalid lines while the others play on, the game's getMaxRoundCount() 300; against
// CodinGame's own replays too); the other columns are read from the sources and still to be validated by their own ports:
//   - Hypersonic (2016): the defaults.
//   - Smash The Code (2016-05, validated): Base::smashTheCode2016() = the defaults except getMillisTimeForRound
//     150 (the game overrides it: 100) and gameOverHook = false (the base has no gameOver(): nextRound()
//     tests the alive count itself, which is what the default hook does too); its SoloReferee class is
//     unused. A line-by-line diff with Hypersonic's base shows nothing else (formatting aside). No command
//     of the game can end with a line that trims away (no headline, no summary or action lines, the view's
//     last line is the next pairs), so the outputDataRecount question does not arise.
//   - Code Busters (2016-06, validated; the same base in its challenge/ and puzzle/ files):
//     Base::codeBusters2016() = the defaults except getMillisTimeForRound 150 (the game overrides it: 100);
//     gameOver() is abstract and the game's (its gameOver flag) is called by nextRound() after every
//     update, as in Hypersonic's base (gameOverHook = true). A line-by-line diff with Hypersonic's base
//     shows nothing else (formatting and comments aside). No command of the game ends with a line that
//     trims away (the headline "Who you gonna call... ?", summary lines ending in '.' / ')', the view's
//     last line a ghost or the ghost count), so the outputDataRecount question does not arise.
//   - Fantastic Bits (2016-11, validated): Base::fantasticBits2016() = the defaults except getMaxRoundCount
//     200 (the game overrides it with 200 too) and gameOverHook = false (nextRound()'s gameOver() call is
//     commented out, replaced by the alive count test; gameOver() is abstract and the game's, returning
//     false, is never called); its SoloReferee class is unused. A line-by-line diff with Hypersonic's base
//     shows nothing else (formatting and comments aside). No command of the game can end with a line that
//     trims away (no headline, every summary / action / tooltip line ends with visible text), so the
//     outputDataRecount question does not arise.
//   - Coders of the Caribbean (2017-04, validated): Base::cotc2017() = the defaults except
//     getMillisTimeForRound 150 (the game overrides it: 50), translateMissingReturnsCode, updateScores =
//     GAME_END; the same gameOver() hook as Hypersonic's (the alive count check, not overridden by the
//     game); its SoloReferee class is unused. A line-by-line diff with Hypersonic's base shows nothing
//     else (formatting aside). The same base, as a Java file, builds the jars of the CodinGame/*
//     repositories that only publish the game's class (referees/cg-builder/legacy/bases/).
//   - Code4Life (2017-05, validated): Base::code4life2017() = cotc2017 + tooltipCode (GameException's
//     setTooltipCode / getTooltipCode, the "LostTooltip" translating the tooltip code), the base of
//     referees/cg-builder/legacy/bases/base-2017-code4life.java (reconstructed: CG's own is not public).
//   - Wondev Woman (2017-06, validated, turn based): Base::wondevWoman2017() = cotc2017 + nextRoundGameIsOver
//     (nextRound() calls the base's gameIsOver() hook, by default gameOver(), which the game overrides:
//     the virtual gameIsOverHook() here),
//     lossEndsGame = false, turnGameIsOverCheck, turnIsNewRound, lostTooltipSuffix "Tooltip" (the
//     "LostTooltip" translates "<code>Tooltip"): referees/cg-builder/legacy/bases/base-2017-wondev-woman.java
//     (reconstructed: the hook from kevinsandow's brutaltester base, the rest inferred from CodinGame's
//     replays of the game, which that base and this preset reproduce exactly; see the base file's header).
//     CodinGame's own replays (codingame.com's public gameResult/findByGameId: views, infos, summaries,
//     tooltips, scores and every player's output per frame) can check any reconstructed base this way.
//   - Mean Max (2017-11, validated, 3 players): Base::meanMax2017() = cotc2017 + nextRoundGameIsOver (the
//     game's isGameOver(): gameIsOverHook() here) + outputDataRecount (OutputData#toString counting the
//     lines after trimming, as CodinGame's SDK does from its first commit: the game's "" headline would
//     otherwise desynchronise the protocol at the first frame): referees/cg-builder/legacy/bases/
//     base-2017-mean-max.java (reconstructed: inferred from CodinGame's replays of the game, which that
//     base and this preset reproduce exactly; see the base file's header).
//   - Coders Strike Back (2015-11, validated): Base::codersStrikeBack2015() = the defaults except
//     precomputeInputs = false (dumpNextPlayerInput calls getInputForPlayer; the game computes its inputs
//     in prepare() itself), invalidTooltip = INVALID_ACTION (no "LostTooltip" message), frameDataEveryFrame,
//     tooltipsEveryFrame, getMillisTimeForRound 150 (not overridden by the game) and gameOverHook = false
//     (no gameOver(): nextRound() tests the alive count itself); its SoloReferee class is unused. A
//     line-by-line diff with Smash The Code's base shows nothing else (formatting aside). No command of the
//     game can end with a line that trims away (the actions, the summary, the view's last line: the ranks
//     or a collision), so the outputDataRecount question does not arise.
//   - Back to the Code (2015, validated): the same base text as Coders Strike Back (identical once all
//     whitespace is removed), so Base::codersStrikeBack2015() as is. No command of the game can end with a
//     line that trims away (the view's last line is a quoted message, the summary's a score, the actions end
//     with coordinates or a count), so the outputDataRecount question does not arise.
//   - The Great Escape (2014, validated): Base::greatEscape2014() = era2014 (read from Platinum Rift's base) but for
//     the messages (the 2015 set: "Max rounds reached", "InvalidActionTooltip", "TimeoutTooltip", "WinTooltip"), the
//     "InvalidActionTooltip" message of a loss (era2014's text "$N: invalid action" written by translate()), the two
//     tooltips of a timeout (TIMEOUT_TWICE), a real win state (winEndsGame = false) and no gameOver() (gameOverHook =
//     false; the game does not override it, so the same test). Its base is Coders Strike Back's 2015 base minus:
//     the "Init error" reason, GameOverException's reason code (set by the base before throwing instead), a loss
//     ending the game at once whatever isTurnBasedGame() (no such method), the "TimeoutTooltip" twice, the scores
//     updated before the last view, getFrameDataForView(round) errors caught, the 2014 INFOS (the reason, the
//     headline, the actions of getPlayerActions(player)) and SUMMARY (from round 0, no reason), getMaxRoundCount()
//     500 without argument (the game: 100). Its SoloReferee class is unused. No command of the game can end with a
//     line that trims away (the view's last line is a wall or the wall count, the summary's a score, the actions and
//     reasons end with visible text, an empty message line is never last), so the outputDataRecount question does
//     not arise.
//   - Platinum Rift (2014, validated): Base::platinumRift2014() = era2014 (read from this base) + the three behaviours
//     era2014 leaves out: timeoutReasonCode (a timed out player's reason code "timeout": in the last view when the game
//     ends in that frame or right after it, as nextPlayer() throws before playerStatus moves on), updateScores =
//     NEXT_PLAYER (nextPlayer() calls updateScores() after each step of its loop; nextRound() does not) and gameOverHook =
//     false (no gameOver(): nextRound() tests the alive count itself). A field-by-field reading of the base finds
//     nothing else; its SoloReferee class is unused. No command of the game can end with a line that trims away (the
//     view's last line is a count or a fight, the actions and the summary end with visible text, a message is never
//     last), so the outputDataRecount question does not arise.
//   - Platinum Rift 2 (validated): Base::platinumRift2_2015() = platinumRift2014() + infos = INFOS_ONLY. Its base text is
//     Platinum Rift's but for its INFOS (no SUMMARY command: the actions of frames > 0 and, on new rounds, the summary are
//     added to the INFOS from round 0, also after the reason of the end) and an extra printStackTrace of an init error
//     (stderr only); the game overrides getMaxRoundCount() (250), not the time limits. No command of the game can end with
//     a line that trims away (the view's last line, the visible zones, always lists both headquarters; the actions and
//     the summary end with visible text), so the outputDataRecount question does not arise.
//   - Poker Chip Race (2014-2015, validated): Base::pokerChipRace2014() = era2014 but for its messages ("playerTimeout"
//     with (count, player) instead of playerTimeoutMulti / Solo, "maxRoundsCountReached" = "Max rounds count reached"), no
//     tooltip for a player lost by an exception (the 2014 "$N: invalid action" line is not in this base), the timeout's info
//     as its tooltip (not "$N: timeout!"), no updateScores() before the last view, and no gameOver() (nextRound() tests the
//     alive count itself, after updateGame and updateScores: the default NEXT_ROUND). A field-by-field reading against
//     Platinum Rift's base finds nothing else (its getFrameDataForView and getInputForPlayer take no round / frame, which
//     the ports ignore anyway); its SoloReferee class is unused. The game overrides getMaxRoundCount() (300), not the time
//     limits (1000 / 150 ms). Its tooltips are a HashSet of identity hash codes like every base's: the port orders them
//     itself (it models the jar's identity hash codes, see cg-2014-poker-chip-race.cpp), here they keep insertion order. No
//     command of the game can end with a line that trims away (the view's last line is the scores, the summary's a score,
//     the actions end with "(x y)" or a chip id, an empty message only shows inside a chip's line, which toViewString
//     trims), so the outputDataRecount question does not arise.
// Without the base: Game of Drones (validated, cg-2014-game-of-drones and cg-2014-game-of-drones-original): no AbstractReferee at
// all, its own protocol loop (`new Referee().start(System.in)` printing to System.out; the jar runs it through
// LegacyRefereeAgent's second entry point). Its port uses only the layer 1 here: runStart() with its own loop, which
// reads with nextLine() / nextInt() and prints with print() / println(). The CodinGame/* repositories (only the game class:
// Coders of the Caribbean, Ghost in the Cell, Code4Life, Wondev Woman, Mean Max) run on the 2017 base
// (Base::cotc2017(), or a variant of it for what their game class needs: Ghost in the Cell needs none,
// Code4Life the tooltip code, Wondev Woman the turn based paths and the gameIsOver() hook, Mean Max the
// isGameOver() hook and the SDK's OutputData).
// Still not exercised by a validated port: winEndsGame (a WinException ending the game at once: The Great Escape's
// base has the win state, Platinum Rift never throws one), a non-empty headline (Mean Max's is "", which
// only the outputDataRecount version survives; non-empty getPlayerActions: Fantastic Bits), a loss
// ending a turn based game at once (lossEndsGame with isTurnBasedGame: no turn based game on such a base
// yet); with the 2017 base (updateScores = GAME_END) also isPlayerDead deaths (Code4Life's and Wondev
// Woman's isPlayerDead never fire; Hypersonic validated them on its own base).
#pragma once

#include "engine.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace legacy {

using java::String;
using Arg = java::FormatArg;
using Lines = std::vector<String>;     // a String[]: null is an empty array (OutputData#addAll(null) adds nothing)
using NString = std::optional<String>; // a String that may be null

// ----------------------------------------------------------------------------
// The referee thread's streams: its stdin (the runner's commands, read with a Scanner) and its stdout
// (println to a PrintStream on the pipe the runner reads, RefereeAgent#getOutput)
// ----------------------------------------------------------------------------
namespace detail {

using cg::detail::OutCmd;
using cg::detail::Runner;

struct Pipe {
    std::string bytes;    // what the referee printed (UTF-8)
    size_t pos = 0;       // what the runner has read
    bool lastCR = false;  // RefereeAgent#lastRefereeByteIsCarriageReturn
    String scanned;       // Scanner: the rest of a line nextInt() read from
    bool hasScanned = false;
    // the commands of the runner's current turn (GameTurnInfo), for the -l trace
    bool got[cg::detail::NKEYS] = {};
    String last[cg::detail::NKEYS];
    bool traceViews = getenv("CG_TRACE_VIEWS") != nullptr;
};
inline Pipe pipe;

inline String replaceCR(String s) {
    for (char16_t& c : s)
        if (c == u'\r') c = u'\n';
    return s;
}

// RefereeAgent#getOutput(nbLine, timeout, extraBufferSpace) on the bytes printed so far, from `pos`:
// false when it would wait for more (fewer than nbLine line ends in fewer than `max` bytes)
inline bool readLines(int nbLine, int max, size_t& pos, bool& lastCR, std::string& out, int& occurrences) {
    out.clear();
    occurrences = 0;
    while ((int)out.size() < max && occurrences < nbLine) {
        if (pos >= pipe.bytes.size()) return false;
        char b = pipe.bytes[pos++];
        if (!(b == '\n' && lastCR)) {
            out += b;
            if (b == '\n' || b == '\r') occurrences++;
        }
        lastCR = b == '\r';
    }
    return true;
}

inline int keyOf(const String& name) {
    // Command.InputCommand of the runner 4.5.0 (no MONITORING)
    for (int k = 0; k < cg::detail::NKEYS; k++)
        if (k != cg::detail::MONITORING && name == java::ascii(cg::detail::KEY_NAME[k])) return k;
    return -1;
}

// The end of one of the runner's turns (the body of GameRunner#runAgents after readGameInfo, as far
// as it is observable): the -l trace of its view, summary and tooltips, the tooltips' parse
inline void endTurn(int round) {
    Runner& r = cg::detail::runner;
    using namespace cg::detail;
    auto content = [](const String& s) { return !s.empty() && s.back() == u'\n' ? s.substr(0, s.size() - 1) : s; };
    if (cg::trace) {
        if (pipe.got[VIEW]) {
            String v = content(pipe.last[VIEW]);
            fprintf(cg::trace, "V %d %lld %lld\n", round, (long long)v.size(), (long long)java::utf8Length(v));
            if (pipe.traceViews) fprintf(cg::trace, "VIEW %d %s\n", round, java::traceJson(v).c_str());
        }
        // gameResult.summaries: the SUMMARY, else the INFOS
        int s = pipe.got[SUMMARY] ? SUMMARY : INFOS;
        fprintf(cg::trace, "S [%s]\n", java::traceJson(content(pipe.last[s])).c_str());
        if (pipe.got[TOOLTIP]) fprintf(cg::trace, "TT [%s]\n", java::traceJson(content(pipe.last[TOOLTIP])).c_str());
    }
    if (pipe.got[TOOLTIP]) {
        // line.split("\n"), then Integer.valueOf of every second line: a NumberFormatException leaves
        // GameRunner#simulate
        std::vector<String> parts = java::split(pipe.last[TOOLTIP], u'\n');
        for (size_t i = 0; i < parts.size() / 2; i++) {
            i32 v;
            if (!java::parseInt(parts[i * 2 + 1], v)) {
                r.crash(java::numberFormatException(java::encodeUtf8(parts[i * 2 + 1])));
                break;
            }
        }
    }
    for (bool& g : pipe.got) g = false;
}

// The runner reads every command the referee has printed in full (GameRunner#readCommand), and acts
// on it (cg::detail::Runner::consume: runs a bot, sends the referee its next commands...)
inline void pump() {
    Runner& r = cg::detail::runner;
    while (!r.done) {
        size_t pos = pipe.pos;
        bool cr = pipe.lastCR;
        std::string raw;
        int occ;
        // the header: getOutput(1, 150 s), a buffer of 30000 bytes
        if (!readLines(1, cg::detail::REFEREE_MAX_BUFFER_SIZE, pos, cr, raw, occ)) return;
        String header = replaceCR(java::decodeUtf8(raw));
        auto commit = [&]() {
            pipe.pos = pos;
            pipe.lastCR = cr;
        };
        if (occ < 1) { // checkOutput: no line end in the whole buffer
            commit();
            r.fail("java.lang.RuntimeException: Invalid Referee command: " + java::encodeUtf8(header));
            return;
        }
        String name, digits;
        if (!cg::detail::matchCommandHeader(java::trim(header), name, digits)) {
            commit();
            r.fail("java.lang.RuntimeException: Invalid referee command: " + java::encodeUtf8(header));
            return;
        }
        i32 n;
        if (!java::parseInt(digits, n)) {
            commit();
            r.fail(java::numberFormatException(java::encodeUtf8(digits)));
            return;
        }
        // the lines: getOutput(n, 150 s, round == 0)
        int limit = r.round == 0 ? cg::detail::REFEREE_MAX_BUFFER_SIZE_EXTRA : cg::detail::REFEREE_MAX_BUFFER_SIZE;
        std::string body;
        if (!readLines(n, limit, pos, cr, body, occ)) return;
        commit();
        String output = replaceCR(java::decodeUtf8(body));
        if (occ < n) {
            r.fail("java.lang.RuntimeException: Error reading Referee " + java::encodeUtf8(name) + " command. Buffer capacity: " + std::to_string(output.size()) + " / " +
                   std::to_string(limit));
            return;
        }
        int key = keyOf(name);
        if (key < 0) {
            r.fail("java.lang.IllegalArgumentException: No enum constant com.codingame.gameengine.runner.Command.InputCommand." + java::encodeUtf8(name));
            return;
        }
        pipe.got[key] = true;
        pipe.last[key] = output;
        int round = r.round;
        r.traceTurn = round;
        r.consume(OutCmd{key, (i64)body.size(), java::encodeUtf8(output)});
        // a turn ended: complete (the runner went on, or has the scores), not failed
        if (r.round != round) endTurn(round); // a complete turn
    }
}

// Why the runner fails when it waits for a command the referee will never print: after 150 s,
// getOutput returns what it has read
inline std::string stallCause() {
    size_t pos = pipe.pos;
    bool cr = pipe.lastCR;
    std::string raw;
    int occ;
    readLines(1, cg::detail::REFEREE_MAX_BUFFER_SIZE, pos, cr, raw, occ);
    String header = replaceCR(java::decodeUtf8(raw));
    String name, digits;
    i32 n;
    if (occ < 1 || !cg::detail::matchCommandHeader(java::trim(header), name, digits) || !java::parseInt(digits, n))
        return "java.lang.RuntimeException: Invalid Referee command: " + java::encodeUtf8(header);
    int limit = cg::detail::runner.round == 0 ? cg::detail::REFEREE_MAX_BUFFER_SIZE_EXTRA : cg::detail::REFEREE_MAX_BUFFER_SIZE;
    readLines(n, limit, pos, cr, raw, occ);
    return "java.lang.RuntimeException: Error reading Referee " + java::encodeUtf8(name) + " command. Buffer capacity: " +
           std::to_string(replaceCR(java::decodeUtf8(raw)).size()) + " / " + std::to_string(limit);
}

} // namespace detail

// out.print(text) of the referee: PrintStream (UTF-8, an unpaired surrogate as '?') on the pipe
inline void print(const String& text) {
    detail::Pipe& p = detail::pipe;
    if (p.pos > (1 << 20)) { // forget what the runner has read
        p.bytes.erase(0, p.pos);
        p.pos = 0;
    }
    p.bytes += java::encodeUtf8(text);
    detail::pump();
}
// out.println(text)
inline void println(const String& text) { print(text + java::LINE_SEPARATOR); }

// Scanner#nextLine on the referee's stdin: the next line of the runner's commands. Throws
// cg::RunnerStopped when the runner has stopped, or waits for the referee (it fails then).
inline String nextLine() {
    detail::Pipe& p = detail::pipe;
    cg::detail::Runner& r = cg::detail::runner;
    if (p.hasScanned) {
        p.hasScanned = false;
        return p.scanned;
    }
    if (r.done) throw cg::RunnerStopped();
    if (r.inbox.empty()) {
        r.fail(detail::stallCause());
        throw cg::RunnerStopped();
    }
    String l = r.inbox.front();
    r.inbox.pop_front();
    return l;
}

// Scanner#nextInt: the next token (the runner's commands only have the player count there)
inline i32 nextInt() {
    detail::Pipe& p = detail::pipe;
    String line;
    for (;;) {
        line = nextLine();
        size_t a = 0;
        while (a < line.size() && java::isRegexSpace(line[a])) a++;
        if (a < line.size()) {
            line = line.substr(a);
            break;
        }
    }
    size_t b = 0;
    while (b < line.size() && !java::isRegexSpace(line[b])) b++;
    i32 v;
    if (!java::parseInt(line.substr(0, b), v)) throw java::Exception{"java.util.InputMismatchException: For input string: \"" + java::encodeUtf8(line.substr(0, b)) + "\""};
    p.scanned = line.substr(b);
    p.hasScanned = true;
    return v;
}

// AbstractReferee.OutputCommand
enum class Out { VIEW, INFOS, NEXT_PLAYER_INPUT, NEXT_PLAYER_INFO, SCORES, UINPUT, TOOLTIP, SUMMARY };
inline const char* outName(Out c) {
    static const char* N[] = {"VIEW", "INFOS", "NEXT_PLAYER_INPUT", "NEXT_PLAYER_INFO", "SCORES", "UINPUT", "TOOLTIP", "SUMMARY"};
    return N[(int)c];
}
// OutputCommand#format(lineCount): "[[NAME] n]"
inline String format(Out c, size_t lineCount) { return java::ascii(std::string("[[") + outName(c) + "] " + std::to_string(lineCount) + "]"); }

// AbstractReferee.OutputData: lines of a command; add(null) adds nothing, addAll adds every line
struct OutputData {
    Out command;
    Lines lines;
    explicit OutputData(Out c) : command(c) {}
    void add(const String& s) { lines.push_back(s); }
    void add(const char16_t* s) { lines.push_back(s); }
    void add(const NString& s) {
        if (s) lines.push_back(*s);
    }
    void addAll(const Lines& data) { lines.insert(lines.end(), data.begin(), data.end()); }
    // toString(): the header with size() lines, every line println'd, the whole trimmed. recount
    // (Base::outputDataRecount): the SDK's version (codingame-game-engine's OutputData since its first commit): the
    // lines joined with '\n' and trimmed, the header counting the lines of that content (split on "\r\n|\r|\n"),
    // the content print()ed after it, the whole trimmed
    String toString(bool recount = false) const {
        if (recount) {
            String sb;
            for (const String& l : lines) sb += l + u'\n';
            String content = java::trim(sb);
            size_t length = 0;
            if (!content.empty()) {
                length = 1; // (split drops trailing empty strings: none after trim())
                for (size_t i = 0; i < content.size(); i++) {
                    if (content[i] == u'\r' && i + 1 < content.size() && content[i + 1] == u'\n') i++;
                    if (content[i] == u'\r' || content[i] == u'\n') length++;
                }
            }
            String w = format(command, length);
            if (!content.empty()) w += java::LINE_SEPARATOR + content;
            return w;
        }
        String w = format(command, lines.size()) + java::LINE_SEPARATOR;
        for (const String& l : lines) w += l + java::LINE_SEPARATOR;
        return java::trim(w);
    }
};
inline void println(const OutputData& d, bool recount = false) { println(d.toString(recount)); }

// ----------------------------------------------------------------------------
// The exceptions of the base
// ----------------------------------------------------------------------------

// GameException: a reason code (a key of the messages, may be null) and the values of its format; the
// bases with Base::tooltipCode also have a tooltip code (setTooltipCode)
struct GameException {
    std::optional<std::string> reasonCode;
    std::vector<Arg> values;
    std::optional<std::string> tooltipCode;
    GameException() {}
    GameException(std::optional<std::string> code, std::vector<Arg> v = {}) : reasonCode(std::move(code)), values(std::move(v)) {}
    void setTooltipCode(const std::string& code) { tooltipCode = code; }
    // getTooltipCode(): the tooltip code if one was set, else the reason code
    const std::optional<std::string>& getTooltipCode() const { return tooltipCode ? tooltipCode : reasonCode; }
};
struct LostException : GameException {
    using GameException::GameException;
};
struct WinException : GameException {
    using GameException::GameException;
};
struct InvalidInputException : GameException {
    InvalidInputException(const String& expected, const String& found) : GameException(std::string("InvalidInput"), {Arg(expected), Arg(found)}) {}
};
struct GameOverException : GameException {
    using GameException::GameException;
};
struct InvalidFormatException {
    String message;
};
struct GameErrorException {};

// ----------------------------------------------------------------------------
// The versions of the base (see the table at the top)
// ----------------------------------------------------------------------------
struct Base {
    // the messages start() puts before populateMessages (key -> format)
    std::vector<std::pair<std::string, String>> messages = {
        {"InvalidInput", u"invalid input. Expected '%s' but found '%s'"},
        {"playerTimeoutMulti", u"Timeout: the program did not provide %d input lines in due time... $%d will no longer be active in this game."},
        {"playerTimeoutSolo", u"Timeout: the program did not provide %d input lines in due time..."},
        {"maxRoundsCountReached", u"Max rounds reached"},
        {"notEnoughPlayers", u"Not enough players (expected > %d, found %d)"},
        {"InvalidActionTooltip", u"$%d: invalid action"},
        {"TimeoutTooltip", u"$%d: timeout!"},
        {"WinTooltip", u"$%d: victory!"},
        {"LostTooltip", u"$%d lost: %s"},
    };
    // the defaults of getMillisTimeForFirstRound, getMillisTimeForRound, getMaxRoundCount
    int millisTimeForFirstRound = 1000;
    int millisTimeForRound = 100;
    int maxRoundCount = 400;
    // an init error sets the reason "Init error: <message>" (shown in the summary)
    bool initErrorReason = true;
    // not turn based: on a new round, GET_GAME_INFO computes the input of every player still in the
    // game (else each player's input is computed when it plays)
    bool precomputeInputs = true;
    // the tooltip of a player who lost by an exception: INVALID_OR_LOST "InvalidActionTooltip" for an
    // InvalidInputException, "LostTooltip" (with the translated reason) for a LostException;
    // INVALID_ACTION: "InvalidActionTooltip" for both; INVALID_ACTION_TEXT: the text "$N: invalid
    // action" for both (2014: no message); NONE
    enum InvalidTooltip { INVALID_OR_LOST, INVALID_ACTION, INVALID_ACTION_TEXT, NONE } invalidTooltip = INVALID_OR_LOST;
    // a player who loses (exception or timeout) ends the game at once, when fewer than
    // getMinimumPlayerCount() are left, only in a turn based game (else at the next round)
    bool lossEndsGameOnlyIfTurnBased = true;
    // a WinException ends the game at once (no "win" state, no tooltip)
    bool winEndsGame = false;
    // the reason code of a timed out player is "timeout" (shown in the view)
    bool timeoutReasonCode = false;
    // the info of a timed out player: the message `timeoutInfo` with (count, player), else
    // "playerTimeoutSolo" (count) / "playerTimeoutMulti" (count, player)
    std::string timeoutInfo;
    // the tooltips of a timed out player: TIMEOUT "TimeoutTooltip"; TIMEOUT_TEXT the text "$N:
    // timeout!" (2014: no message); INFO its info; TIMEOUT_TWICE "TimeoutTooltip" and "$N: timeout!"
    enum TimeoutTooltip { TIMEOUT, TIMEOUT_TEXT, INFO, TIMEOUT_TWICE } timeoutTooltip = TIMEOUT;
    // when updateScores runs: NEXT_ROUND (after updateGame), NEXT_PLAYER (after every step of
    // nextPlayer's loop), GAME_END (only at the end)
    enum ScoresUpdate { NEXT_ROUND, NEXT_PLAYER, GAME_END } updateScores = NEXT_ROUND;
    // the game over also runs updateScores before its last view
    bool updateScoresBeforeLastView = false;
    // getFrameDataForView in every view (else only on new rounds, or every frame of a turn based game)
    bool frameDataEveryFrame = false;
    // an exception of getFrameDataForView is caught (printed to stderr)
    bool catchFrameDataErrors = false;
    // tooltips are sent in every frame (else only on new rounds, or every frame of a turn based game)
    bool tooltipsEveryFrame = false;
    // the infos: INFOS_2016 (the reason only in a turn based game, the headline and the game summary
    // in a SUMMARY command of new rounds); INFOS_2014 (the reason, the headline in INFOS, a SUMMARY of
    // new rounds from round 0); INFOS_ONLY (Platinum Rift 2: the actions and the summary in INFOS, no
    // SUMMARY command)
    enum Infos { INFOS_2016, INFOS_2014, INFOS_ONLY } infos = INFOS_2016;
    // translate() of a code without message returns the code (else a NullPointerException)
    bool translateMissingReturnsCode = false;
    // GameException#setTooltipCode / getTooltipCode exist, and the "LostTooltip" of a LostException
    // translates e.getTooltipCode() (the tooltip code if the game set one, else the reason code) instead
    // of e.getReasonCode(): referees/cg-builder/legacy/bases/base-2017-code4life.java (Code4Life)
    bool tooltipCode = false;
    // the "LostTooltip" of a LostException translates the reason code + this suffix (Wondev Woman's base:
    // "Tooltip", the game's "<code>Tooltip" messages; a null code gives "null" + suffix)
    std::string lostTooltipSuffix;
    // nextRound() ends the game when the base's gameIsOver() hook says so (gameIsOverHook() here, a hook the
    // game may override, by default gameOver()) instead of gameOver()
    bool nextRoundGameIsOver = false;
    // a player who loses (exception or timeout) can end the game at once (lossEndsGameOnlyIfTurnBased);
    // false: never, only decremented alive count (Wondev Woman's base)
    bool lossEndsGame = true;
    // a turn based game also ends when gameIsOverHook() in GET_GAME_INFO, after nextPlayer() and the max
    // rounds test
    bool turnGameIsOverCheck = false;
    // in a turn based game every turn is a new round for the view (KEY_FRAME), the summary and prepare():
    // nextPlayer() sets newRound = isTurnBasedGame() (the round counter still counts the passes over the
    // players)
    bool turnIsNewRound = false;
    // OutputData#toString counts the lines after trimming (the SDK's version, see OutputData): a last line that
    // trims away (Mean Max's "" headline in the first SUMMARY) no longer desynchronises the protocol
    bool outputDataRecount = false;
    // nextRound() ends the game when gameOver() (the hook, by default the alive count test, which the game may
    // override); false: the hook's call is commented out and nextRound() tests the alive count itself, the game's
    // gameOver() (abstract in that base) is never called (Fantastic Bits)
    bool gameOverHook = true;

    // the 2014-2015 bases (Platinum Rift, Poker Chip Race, The Great Escape, Platinum Rift 2): their
    // common points, as read from Platinum Rift's base; each game adjusts the rest (messages, winEndsGame,
    // timeout*, updateScores...: see greatEscape2014(), platinumRift2014())
    static Base era2014() {
        Base b;
        b.messages = {
            {"InvalidInput", u"invalid input. Expected '%s' but found '%s'"},
            {"playerTimeoutMulti", u"Timeout: the program did not provide %d input lines in due time... $%d will no longer be active in this game."},
            {"playerTimeoutSolo", u"Timeout: the program did not provide %d input lines in due time..."},
            {"maxRoundsCountReached", u"Maximum amount of rounds reached..."},
            {"notEnoughPlayers", u"Not enough players (expected > %d, found %d)"},
        };
        b.millisTimeForRound = 150;
        b.maxRoundCount = 500;
        b.initErrorReason = false;
        b.precomputeInputs = false;
        b.invalidTooltip = INVALID_ACTION_TEXT;
        b.timeoutTooltip = TIMEOUT_TEXT;
        b.lossEndsGameOnlyIfTurnBased = false;
        b.winEndsGame = true;
        b.updateScoresBeforeLastView = true;
        b.frameDataEveryFrame = true;
        b.catchFrameDataErrors = true;
        b.tooltipsEveryFrame = true;
        b.infos = INFOS_2014;
        return b;
    }

    // The Great Escape's 2014 base (validated by cg-2015-great-escape and against CodinGame's own replays): era2014 but for
    // the messages (the 2015 bases' set: "Max rounds reached", "InvalidActionTooltip", "TimeoutTooltip", "WinTooltip"),
    // the "InvalidActionTooltip" message of a loss (the same text as era2014's), the two tooltips of a timeout
    // ("TimeoutTooltip" and the text "$N: timeout!"), a real win state (a WinException: the "WinTooltip", the player
    // skipped afterwards, the game over only below getMinimumPlayerCount() players left) and no gameOver() (nextRound()
    // tests the alive count itself)
    static Base greatEscape2014() {
        Base b = era2014();
        b.messages = {
            {"InvalidInput", u"invalid input. Expected '%s' but found '%s'"},
            {"playerTimeoutMulti", u"Timeout: the program did not provide %d input lines in due time... $%d will no longer be active in this game."},
            {"playerTimeoutSolo", u"Timeout: the program did not provide %d input lines in due time..."},
            {"maxRoundsCountReached", u"Max rounds reached"},
            {"notEnoughPlayers", u"Not enough players (expected > %d, found %d)"},
            {"InvalidActionTooltip", u"$%d: invalid action"},
            {"TimeoutTooltip", u"$%d: timeout!"},
            {"WinTooltip", u"$%d: victory!"},
        };
        b.invalidTooltip = INVALID_ACTION;
        b.timeoutTooltip = TIMEOUT_TWICE;
        b.winEndsGame = false;
        b.gameOverHook = false;
        return b;
    }

    // Platinum Rift's 2014 base (validated by cg-2014-platinum-rift-episode-1 and against CodinGame's own replays; era2014
    // was read from it but leaves these out): era2014 + the reason code "timeout" of a timed out player (in the view
    // when the game ends in the same frame or right after it), updateScores after every step of nextPlayer()'s loop
    // instead of after updateGame, and no gameOver() (nextRound() tests the alive count itself; the game does not
    // override gameOver(), so the same test)
    static Base platinumRift2014() {
        Base b = era2014();
        b.timeoutReasonCode = true;
        b.updateScores = NEXT_PLAYER;
        b.gameOverHook = false;
        return b;
    }

    // Platinum Rift 2's base (validated by cg-2014-platinum-rift-episode-2 and against CodinGame's own replays): Platinum
    // Rift's text but for its INFOS: no SUMMARY command, the actions of every frame > 0 and, on a new round, the game
    // summary go into the INFOS from round 0 (also after the reason of the end)
    static Base platinumRift2_2015() {
        Base b = platinumRift2014();
        b.infos = INFOS_ONLY;
        return b;
    }

    // Poker Chip Race's 2014 base (validated by cg-2014-poker-chip-race and against CodinGame's own replays): era2014 but for
    // its messages (one "playerTimeout" with (count, player), "Max rounds count reached"), no tooltip for a loss by an
    // exception, the timeout's info as its tooltip, no updateScores before the last view, and no gameOver() (nextRound()
    // tests the alive count itself; updateScores after updateGame, as the default)
    static Base pokerChipRace2014() {
        Base b = era2014();
        b.messages = {
            {"InvalidInput", u"invalid input. Expected '%s' but found '%s'"},
            {"playerTimeout", u"Timeout: the program did not provide %d input lines in due time... $%d will no longer be active in this game."},
            {"maxRoundsCountReached", u"Max rounds count reached"},
            {"notEnoughPlayers", u"Not enough players (expected > %d, found %d)"},
        };
        b.invalidTooltip = NONE;
        b.timeoutInfo = "playerTimeout";
        b.timeoutTooltip = INFO;
        b.updateScoresBeforeLastView = false;
        b.gameOverHook = false;
        return b;
    }

    // the 2017 base (Coders of the Caribbean's, validated by cg-2017-coders-of-the-caribbean; also the base
    // of referees/cg-builder/legacy/bases/base-2017-cotc.java, which the jars of the CodinGame/*
    // repositories that only publish the game's class compile with): Hypersonic's but for these three
    static Base cotc2017() {
        Base b;
        b.millisTimeForRound = 150;
        b.translateMissingReturnsCode = true;
        b.updateScores = GAME_END;
        return b;
    }

    // the 2017 base with the tooltip code of GameException (referees/cg-builder/legacy/bases/
    // base-2017-code4life.java, validated by cg-2017-code4life): cotc2017 + tooltipCode
    static Base code4life2017() {
        Base b = cotc2017();
        b.tooltipCode = true;
        return b;
    }

    // the 2017 base as Wondev Woman runs on it (referees/cg-builder/legacy/bases/base-2017-wondev-woman.java,
    // validated by cg-2017-wondev-woman and against CodinGame's own replays): cotc2017 + the gameIsOver() hook
    // (nextRound and, turn based, GET_GAME_INFO), no end of the game at a loss, every turn of a turn based
    // game a new round, the "<code>Tooltip" of a lost player
    static Base wondevWoman2017() {
        Base b = cotc2017();
        b.lostTooltipSuffix = "Tooltip";
        b.nextRoundGameIsOver = true;
        b.lossEndsGame = false;
        b.turnGameIsOverCheck = true;
        b.turnIsNewRound = true;
        return b;
    }

    // the 2017 base as Mean Max runs on it (referees/cg-builder/legacy/bases/base-2017-mean-max.java, validated by
    // cg-2017-mean-max and against CodinGame's own replays): cotc2017 + the game's end rule in nextRound() (its
    // isGameOver(): gameIsOverHook() here) and the SDK's OutputData#toString
    static Base meanMax2017() {
        Base b = cotc2017();
        b.nextRoundGameIsOver = true;
        b.outputDataRecount = true;
        return b;
    }

    // Fantastic Bits' 2016 base (validated by cg-2016-fantastic-bits): Hypersonic's but for getMaxRoundCount 200 and the
    // gameOver() hook commented out in nextRound() (the alive count test only)
    static Base fantasticBits2016() {
        Base b;
        b.maxRoundCount = 200;
        b.gameOverHook = false;
        return b;
    }

    // Code Busters' 2016 base (validated by cg-2016-codebusters and cg-2016-codebusters-contest): Hypersonic's but for
    // getMillisTimeForRound 150 (the game overrides it with 100) and an abstract gameOver() (the game's, called by
    // nextRound() after every update as in Hypersonic's)
    static Base codeBusters2016() {
        Base b;
        b.millisTimeForRound = 150;
        return b;
    }

    // Smash The Code's 2016 base (validated by cg-2016-smash-the-code): Hypersonic's but for getMillisTimeForRound 150 (the
    // game overrides it with 100) and no gameOver() at all (nextRound() tests the alive count itself)
    static Base smashTheCode2016() {
        Base b;
        b.millisTimeForRound = 150;
        b.gameOverHook = false;
        return b;
    }

    // Coders Strike Back's 2015 base (validated by cg-2016-coders-strike-back and cg-2016-coders-strike-back-original; Back to the
    // Code embeds the same text): Hypersonic's but for getMillisTimeForRound 150, no "LostTooltip" message (any loss by an
    // exception gets the "InvalidActionTooltip"), no precomputed inputs (each player's input is asked when it plays),
    // the frame data and the tooltips sent in every frame, and no gameOver() (nextRound() tests the alive count itself)
    static Base codersStrikeBack2015() {
        Base b;
        b.messages.erase(std::remove_if(b.messages.begin(), b.messages.end(), [](const std::pair<std::string, String>& m) { return m.first == "LostTooltip"; }),
                         b.messages.end());
        b.millisTimeForRound = 150;
        b.precomputeInputs = false;
        b.invalidTooltip = INVALID_ACTION;
        b.frameDataEveryFrame = true;
        b.tooltipsEveryFrame = true;
        b.gameOverHook = false;
        return b;
    }
};

// ----------------------------------------------------------------------------
// AbstractReferee
// ----------------------------------------------------------------------------
class AbstractReferee {
  public:
    Base base;

    virtual ~AbstractReferee() {}

    // --- the game: the abstract methods of the base (superset signatures: the older bases pass
    // fewer arguments, their ports ignore the others) ---
    virtual void populateMessages(std::map<std::string, String>& messages) = 0;
    virtual void handleInitInputForReferee(int playerCount, const Lines& init) = 0;
    virtual Lines getInitDataForView() = 0; // 2014: getAdditionalFrameDataAtGameStartForView
    virtual Lines getFrameDataForView(int round, int frame, int currentPlayer, bool newRound) = 0;
    virtual int getExpectedOutputLineCountForPlayer(int playerIdx) = 0;
    virtual String getGameName() = 0;
    virtual void appendDataToEnd() = 0;
    virtual void handlePlayerOutput(int frame, int round, int playerIdx, const Lines& outputs) = 0;
    virtual Lines getInitInputForPlayer(int playerIdx) = 0;
    virtual Lines getInputForPlayer(int round, int playerIdx) = 0;
    virtual NString getHeadlineAtGameStartForConsole() = 0;
    virtual int getMinimumPlayerCount() = 0;
    virtual bool showTooltips() = 0; // 2014: showDeathTooltips (Poker Chip Race: always true)
    virtual void updateGame(int round) = 0;
    virtual void prepare(int round) = 0;
    virtual bool isPlayerDead(int playerIdx) = 0;
    virtual NString getDeathReason(int playerIdx) = 0;
    virtual int getScore(int playerIdx) = 0;
    virtual Lines getGameSummary(int round) = 0;
    virtual Lines getPlayerActions(int playerIdx, int round) = 0;
    virtual void setPlayerTimeout(int frame, int round, int playerIdx) = 0;
    // overridable, with the base's defaults
    virtual int getMillisTimeForFirstRound() { return base.millisTimeForFirstRound; }
    virtual int getMillisTimeForRound() { return base.millisTimeForRound; }
    virtual int getMaxRoundCount(int playerCount) {
        (void)playerCount;
        return base.maxRoundCount;
    }
    // the bases without the hook test the alive count only: their ports keep this one
    virtual bool gameOver() { return alivePlayerCount < getMinimumPlayerCount(); }
    // the bases with the gameIsOver() hook (Base::nextRoundGameIsOver, turnGameIsOverCheck: Wondev Woman's):
    // the game's end rule, by default gameOver(). Named gameIsOverHook here, not gameIsOver: Coders of the
    // Caribbean's game class has its own (private, unrelated) gameIsOver(), which must not override it.
    virtual bool gameIsOverHook() { return gameOver(); }
    virtual bool isTurnBasedGame() { return false; }

    // --- what the game uses ---
    // translate(code, values): String.format(messages.get(code), values)
    String translate(const std::string& code, const std::vector<Arg>& values = {}) {
        auto it = messages.find(code);
        if (it == messages.end()) {
            if (base.translateMissingReturnsCode) return java::ascii(code);
            throw java::Exception{"java.lang.NullPointerException"};
        }
        return java::format(it->second, values);
    }
    // GameException#getReason
    NString getReason(const GameException& e) {
        if (!e.reasonCode) return std::nullopt;
        return translate(*e.reasonCode, e.values);
    }
    static String getColoredReason(bool error, const NString& reason) {
        String r = reason ? *reason : String(u"null");
        return error ? u"¤RED¤" + r + u"§RED§" : u"¤GREEN¤" + r + u"§GREEN§";
    }
    void addToolTip(int player, const NString& message) {
        if (showTooltips()) tooltips.push_back({player, message});
    }
    int getPlayerCount() const { return playerCount; }
    int getAlivePlayerCount() const { return alivePlayerCount; }

    // the protocol loop (the constructor of the Java referee)
    void start();

  protected:
    struct PlayerStatus {
        int id = 0;
        int score = 0;
        bool lost = false, win = false;
        NString info;
        std::optional<std::string> reasonCode;
        Lines nextInput;
    };
    struct Tooltip {
        int player;
        NString message;
    };
    std::map<std::string, String> messages;
    std::vector<Tooltip> tooltips; // a HashSet of identity hash codes in Java: the order is unknown
    int playerCount = 0, alivePlayerCount = 0;
    int currentPlayer = 0, nextPlayerIdx = 0;
    PlayerStatus* lastPlayer = nullptr;
    PlayerStatus* playerStatus = nullptr;
    int frame = 0, round = 0;
    std::vector<PlayerStatus> players;
    Lines initLines;
    bool newRound = false;
    std::optional<std::string> reasonCode;
    NString reason;

  private:
    PlayerStatus* nextPlayer();
    void nextRound();
    void updateScores();
    void dumpView();
    void dumpInfos();
    void dumpNextPlayerInfos();
    void dumpNextPlayerInput();
    void onLoss(int player, bool invalidInput, const GameException* e);
    void gameOverDump(const GameOverException& e);
};

inline AbstractReferee::PlayerStatus* AbstractReferee::nextPlayer() {
    currentPlayer = nextPlayerIdx;
    newRound = base.turnIsNewRound && isTurnBasedGame();
    long long emptyRounds = 0;
    for (;;) {
        ++nextPlayerIdx;
        if (nextPlayerIdx >= playerCount) {
            nextRound();
            nextPlayerIdx = 0;
            // nobody left to play and gameOver() false: the Java referee loops (until gameOver())
            if (++emptyRounds > 1000000) throw java::Exception{"legacy: the referee loops forever in nextPlayer()"};
        }
        if (base.updateScores == Base::NEXT_PLAYER) updateScores();
        if (!(players[nextPlayerIdx].lost || players[nextPlayerIdx].win)) break;
    }
    return &players[nextPlayerIdx];
}

inline void AbstractReferee::nextRound() {
    newRound = true;
    if (++round > 0) {
        updateGame(round);
        if (base.updateScores == Base::NEXT_ROUND) updateScores();
    }
    if (base.nextRoundGameIsOver ? gameIsOverHook() : base.gameOverHook ? gameOver() : alivePlayerCount < getMinimumPlayerCount())
        throw GameOverException(std::nullopt);
}

inline void AbstractReferee::updateScores() {
    for (int i = 0; i < playerCount; ++i) {
        if (!players[i].lost && isPlayerDead(i)) {
            alivePlayerCount--;
            players[i].lost = true;
            players[i].info = getDeathReason(i);
            addToolTip(i, players[i].info);
        }
        players[i].score = getScore(i);
    }
}

inline void AbstractReferee::dumpView() {
    OutputData data(Out::VIEW);
    std::optional<std::string> rc = this->reasonCode;
    if (!rc && playerStatus != nullptr) rc = playerStatus->reasonCode;
    std::string f = std::to_string(frame);
    if (newRound) {
        data.add(java::ascii(rc ? "KEY_FRAME " + f + " " + *rc : "KEY_FRAME " + f));
        if (frame == 0) {
            data.add(getGameName());
            data.addAll(getInitDataForView());
        }
    } else {
        data.add(java::ascii(rc ? "INTERMEDIATE_FRAME " + f + " " + *rc : "INTERMEDIATE_FRAME " + f));
    }
    if (newRound || isTurnBasedGame() || base.frameDataEveryFrame) {
        if (base.catchFrameDataErrors) {
            try {
                data.addAll(getFrameDataForView(round, frame, currentPlayer, newRound));
            } catch (const java::Exception&) {
            }
        } else {
            data.addAll(getFrameDataForView(round, frame, currentPlayer, newRound));
        }
    }
    println(data, base.outputDataRecount);
}

inline void AbstractReferee::dumpInfos() {
    OutputData data(Out::INFOS);
    bool turnBased = isTurnBasedGame();
    if (base.infos == Base::INFOS_2016) {
        if (reason && turnBased) {
            data.add(getColoredReason(true, reason));
        } else if (lastPlayer != nullptr) {
            if (lastPlayer->info) data.add(getColoredReason(lastPlayer->lost, lastPlayer->info));
            else if (frame > 0) data.addAll(getPlayerActions(currentPlayer, newRound ? round - 1 : round));
        }
        println(data, base.outputDataRecount);
        if (newRound && round >= -1 && playerCount > 1) {
            OutputData summary(Out::SUMMARY);
            if (frame == 0) summary.add(getHeadlineAtGameStartForConsole());
            if (round >= 0) summary.addAll(getGameSummary(round));
            if (!turnBased && reason) summary.add(getColoredReason(true, reason));
            println(summary, base.outputDataRecount);
        }
    } else {
        if (reason) {
            data.add(getColoredReason(true, reason));
        } else {
            if (frame == 0) data.add(getHeadlineAtGameStartForConsole());
            if (lastPlayer != nullptr) {
                if (lastPlayer->info) data.add(getColoredReason(lastPlayer->lost, lastPlayer->info));
                else if (frame > 0 && base.infos == Base::INFOS_2014) data.addAll(getPlayerActions(currentPlayer, round));
            }
        }
        if (base.infos == Base::INFOS_ONLY && round >= 0) {
            if (frame > 0) data.addAll(getPlayerActions(currentPlayer, round));
            if (newRound) data.addAll(getGameSummary(round));
        }
        println(data, base.outputDataRecount);
        if (base.infos == Base::INFOS_2014 && newRound && round >= 0 && playerCount > 1) {
            OutputData summary(Out::SUMMARY);
            summary.addAll(getGameSummary(round));
            println(summary, base.outputDataRecount);
        }
    }
    if (!tooltips.empty() && (newRound || turnBased || base.tooltipsEveryFrame)) {
        OutputData t(Out::TOOLTIP);
        for (const Tooltip& tt : tooltips) {
            t.add(tt.message);
            t.add(java::str(tt.player));
        }
        tooltips.clear();
        println(t, base.outputDataRecount);
    }
}

inline void AbstractReferee::dumpNextPlayerInfos() {
    OutputData data(Out::NEXT_PLAYER_INFO);
    data.add(java::str(nextPlayerIdx));
    data.add(java::str(getExpectedOutputLineCountForPlayer(nextPlayerIdx)));
    data.add(java::str(round == 0 ? getMillisTimeForFirstRound() : getMillisTimeForRound()));
    println(data, base.outputDataRecount);
}

inline void AbstractReferee::dumpNextPlayerInput() {
    OutputData data(Out::NEXT_PLAYER_INPUT);
    if (round == 0) data.addAll(getInitInputForPlayer(nextPlayerIdx));
    if (!base.precomputeInputs || isTurnBasedGame()) players[nextPlayerIdx].nextInput = getInputForPlayer(round, nextPlayerIdx);
    data.addAll(players[nextPlayerIdx].nextInput);
    println(data, base.outputDataRecount);
}

// a player lost by an exception of handlePlayerOutput (e) or by a timeout (e == nullptr)
inline void AbstractReferee::onLoss(int player, bool invalidInput, const GameException* e) {
    PlayerStatus& ps = *playerStatus;
    if (e) {
        ps.score = getScore(player);
        ps.lost = true;
        ps.info = getReason(*e);
        ps.reasonCode = e->reasonCode;
        if (base.invalidTooltip == Base::INVALID_ACTION || (base.invalidTooltip == Base::INVALID_OR_LOST && invalidInput)) {
            addToolTip(player, translate("InvalidActionTooltip", {player}));
        } else if (base.invalidTooltip == Base::INVALID_ACTION_TEXT) {
            addToolTip(player, u"$" + java::str(player) + u": invalid action");
        } else if (base.invalidTooltip == Base::INVALID_OR_LOST) {
            // translate(e.getReasonCode(), e.values) (Base::tooltipCode: e.getTooltipCode()):
            // messages.get(null) throws a NullPointerException
            const std::optional<std::string>& code = base.tooltipCode ? e->getTooltipCode() : e->reasonCode;
            Arg r;
            if (!base.lostTooltipSuffix.empty()) r = Arg(translate((code ? *code : std::string("null")) + base.lostTooltipSuffix, e->values));
            else if (code) r = Arg(translate(*code, e->values));
            else if (!base.translateMissingReturnsCode) throw java::Exception{"java.lang.NullPointerException"};
            addToolTip(player, translate("LostTooltip", {player, r}));
        }
    } else {
        int count = getExpectedOutputLineCountForPlayer(player);
        setPlayerTimeout(frame, round, player);
        ps.lost = true;
        if (base.timeoutReasonCode) ps.reasonCode = std::string("timeout");
        if (!base.timeoutInfo.empty()) ps.info = translate(base.timeoutInfo, {count, player});
        else if (playerCount <= 1) ps.info = translate("playerTimeoutSolo", {count});
        else ps.info = translate("playerTimeoutMulti", {count, player});
        if (base.timeoutTooltip == Base::INFO) {
            addToolTip(player, ps.info);
        } else if (base.timeoutTooltip == Base::TIMEOUT_TEXT) {
            addToolTip(player, u"$" + java::str(player) + u": timeout!");
        } else {
            addToolTip(player, translate("TimeoutTooltip", {player}));
            if (base.timeoutTooltip == Base::TIMEOUT_TWICE) addToolTip(player, u"$" + java::str(player) + u": timeout!");
        }
    }
    if (--alivePlayerCount < getMinimumPlayerCount() && base.lossEndsGame && (isTurnBasedGame() || !base.lossEndsGameOnlyIfTurnBased)) {
        lastPlayer = playerStatus;
        throw GameOverException(std::nullopt);
    }
}

inline void AbstractReferee::gameOverDump(const GameOverException& e) {
    newRound = true;
    if (e.reasonCode) { // (the 2014 bases set them before throwing: same thing)
        reasonCode = e.reasonCode;
        reason = getReason(e);
    }
    if (base.updateScoresBeforeLastView) updateScores();
    dumpView();
    dumpInfos();
    prepare(round);
    updateScores();
}

inline void AbstractReferee::start() {
    for (auto& m : base.messages) messages[m.first] = m.second;
    populateMessages(messages);
    try {
        for (;;) {
            String line = nextLine();
            String cmd, digits;
            if (!cg::detail::matchCommandHeader(line, cmd, digits)) throw java::Exception{"java.lang.RuntimeException: Error in data sent to referee"};
            i32 lineCount;
            if (!java::parseInt(digits, lineCount)) throw java::Exception{java::encodeUtf8(java::numberFormatException(digits))};
            if (cmd == u"INIT") {
                playerCount = alivePlayerCount = nextInt();
                if (playerCount <= 0) throw java::Exception{"java.lang.ArrayIndexOutOfBoundsException"}; // (never: 2+ players)
                players.assign(playerCount, PlayerStatus());
                for (int i = 0; i < playerCount; ++i) players[i].id = i;
                playerStatus = &players[0];
                currentPlayer = nextPlayerIdx = playerCount - 1;
                frame = 0;
                round = -1;
                newRound = true;
                nextLine();
                initLines.clear();
                for (int i = 0; i < lineCount - 1; i++) initLines.push_back(nextLine());
                bool failed = false;
                String message;
                bool hasMessage = true;
                try {
                    handleInitInputForReferee(playerCount, initLines);
                } catch (const InvalidFormatException& e) {
                    failed = true;
                    message = e.message;
                } catch (const java::Exception& e) {
                    // RuntimeException#getMessage: after "class: ", null without one
                    failed = true;
                    size_t c = e.text.find(": ");
                    hasMessage = c != std::string::npos;
                    if (hasMessage) message = java::decodeUtf8(e.text.substr(c + 2));
                }
                if (failed) {
                    NString m = hasMessage ? NString(message) : std::nullopt;
                    if (base.initErrorReason) reason = u"Init error: " + (m ? *m : String(u"null"));
                    OutputData viewData(Out::VIEW);
                    viewData.add(u"-1");
                    viewData.add(u"INPUT");
                    println(viewData, base.outputDataRecount);
                    OutputData infoData(Out::INFOS);
                    infoData.add(getColoredReason(true, String(u"Failure: invalid input")));
                    infoData.add(getColoredReason(true, m));
                    infoData.addAll(initLines);
                    println(infoData, base.outputDataRecount);
                    dumpView();
                    dumpInfos();
                    throw GameErrorException();
                }
                if (playerCount < getMinimumPlayerCount()) throw GameOverException(std::string("notEnoughPlayers"), {getMinimumPlayerCount(), playerCount});
            } else if (cmd == u"GET_GAME_INFO") {
                lastPlayer = playerStatus;
                playerStatus = nextPlayer();
                if (round >= getMaxRoundCount(playerCount)) throw GameOverException(std::string("maxRoundsCountReached"));
                if (base.turnGameIsOverCheck && isTurnBasedGame() && gameIsOverHook()) throw GameOverException(std::nullopt);
                dumpView();
                dumpInfos();
                if (newRound) {
                    prepare(round);
                    if (base.precomputeInputs && !isTurnBasedGame())
                        for (PlayerStatus& p : players) p.nextInput = !p.lost ? getInputForPlayer(round, p.id) : Lines();
                }
                dumpNextPlayerInput();
                dumpNextPlayerInfos();
            } else if (cmd == u"SET_PLAYER_OUTPUT") {
                ++frame;
                Lines output;
                for (int i = 0; i < lineCount; i++) output.push_back(nextLine());
                try {
                    handlePlayerOutput(frame, round, nextPlayerIdx, output);
                } catch (const InvalidInputException& e) {
                    onLoss(nextPlayerIdx, true, &e);
                } catch (const LostException& e) {
                    onLoss(nextPlayerIdx, false, &e);
                } catch (const WinException& e) {
                    PlayerStatus& ps = *playerStatus;
                    ps.score = getScore(nextPlayerIdx);
                    ps.info = getReason(e);
                    ps.reasonCode = e.reasonCode;
                    if (base.winEndsGame) {
                        lastPlayer = playerStatus;
                        throw GameOverException(std::nullopt);
                    }
                    ps.win = true;
                    addToolTip(nextPlayerIdx, translate("WinTooltip", {nextPlayerIdx}));
                    if (--alivePlayerCount < getMinimumPlayerCount()) {
                        lastPlayer = playerStatus;
                        throw GameOverException(std::nullopt);
                    }
                }
            } else if (cmd == u"SET_PLAYER_TIMEOUT") {
                ++frame;
                onLoss(nextPlayerIdx, false, nullptr);
            } else {
                throw java::Exception{"java.lang.IllegalArgumentException: No enum constant AbstractReferee.InputCommand." + java::encodeUtf8(cmd)};
            }
        }
    } catch (const GameOverException& e) {
        gameOverDump(e);
    } catch (const GameErrorException&) {
    } catch (const java::Exception&) {
        // t.printStackTrace(); s.close(); return: no scores
        return;
    }
    OutputData scores(Out::SCORES);
    for (int i = 0; i < playerCount; ++i) scores.add(java::str(i) + u" " + java::str(players[i].score));
    appendDataToEnd();
    println(scores, base.outputDataRecount);
}

// ----------------------------------------------------------------------------
// MultiReferee
// ----------------------------------------------------------------------------
class MultiReferee : public AbstractReferee {
  public:
    virtual void initReferee(int playerCount, java::Properties& prop) = 0;
    virtual java::Properties getConfiguration() = 0;

    void handleInitInputForReferee(int playerCount, const Lines& init) override {
        properties = java::Properties();
        for (const String& s : init) properties.load(s);
        initReferee(playerCount, properties);
        properties = getConfiguration();
    }
    // UINPUT: the properties as "key=value", in the iteration order of the Properties
    void appendDataToEnd() override {
        println(format(Out::UINPUT, properties.size()));
        for (auto& e : properties.entrySet()) println(e.first + u"=" + e.second);
    }

  private:
    java::Properties properties;
};

// ----------------------------------------------------------------------------
// The jar's CommandLineInterface (cg::runCli) with the old referee in its runner
// ----------------------------------------------------------------------------
struct Options : cg::Options {};

// Plays one game like the jar of referees/cg-builder/legacy, the referee thread running `start()`: returns the exit
// code. run() below passes the base's protocol loop (LegacyRefereeAgent's `new Referee(stdin, stdout, stderr)`); a
// referee without the base (LegacyRefereeAgent#runStart: Game of Drones' `new Referee().start(System.in)`, printing
// to System.out) passes its own loop, which reads with nextLine() / nextInt() and prints with print() / println().
template <class Start>
inline int runStart(int argc, char** argv, Start start, const Options& options = Options()) {
    return cg::runCli(argc, argv, options, [start](i64 seed, int n) {
        cg::detail::Runner& r = cg::detail::runner;
        // GameRunner#runAgents: INIT (the player count, "seed=N"), then GET_GAME_INFO
        r.send("INIT", {java::str(n), u"seed=" + java::str(seed)});
        r.startRound();
        try {
            start();
        } catch (const cg::RunnerStopped&) {
        } catch (const java::Exception&) {
            // thrown by the game over's own dumps (or any Java exception of a referee without the base): the
            // referee thread dies without scores
        }
        detail::pump();
        // the referee thread is over: a runner still waiting for a command fails after 150 s
        if (!r.done) r.fail(detail::stallCause());
    });
}

// Plays one game like the jar of referees/cg-builder/legacy: returns the exit code
inline int run(int argc, char** argv, AbstractReferee& referee, const Options& options = Options()) {
    return runStart(argc, argv, [&referee] { referee.start(); }, options);
}

} // namespace legacy
