// Code4Life (CodinGame, 2017-05, "code4life") - headless C++ referee, port of the CodinGame referee published in
// https://github.com/CodinGame/Code4Life (commit d8abcf5, 2017-05-16, = HEAD, "Update Referee.java - Fixed a few
// bugs"; the README: "The code used to play matches in the Code4Life contest"). The repository's history: the
// referee added 2017-05-12, then the 2017-05-15 fixes "fixed end-game bug" (the score limit of 170 ends the game in
// the lower leagues only) and "Corrected molecule balance" (one rank 2 sample's cost), then d8abcf5 (LEAGUE_LEVEL 0
// -> 3, the 4th EXPECTED_BY_LEAGUE_LEVEL entry, the "ProjectTooltip" format): fixes made during the contest (May
// 2017); the repository was never updated after it, so this is also the multiplayer game's referee as published.
// The file is the game's class only (`class Referee extends MultiReferee`), without imports nor base. 2 players.
// Deterministic but for one HashMap order (below); reproduces the same game as the reference jar for a given -seed.
//
// The jar (referees/cg-builder/legacy, `legacy/build.sh ... cg-2017-code4life imports/java-2017.imports
// bases/base-2017-code4life.java`): the game's class with an import block prepended (imports/java-2017.imports:
// only imports, no game code), compiled with CodinGame's old base of 2017 (the Coders of the Caribbean one from
// https://github.com/SpiritusSancti5/codinGame, 2017-04) plus the GameException tooltip code the game calls
// (LostException#setTooltipCode: setTooltipCode / getTooltipCode from kevinsandow's brutaltester referee, and the
// "LostTooltip" of a LostException translating getTooltipCode(); see that base file's header). CodinGame's own
// base of 2017-05 is not public. This port runs on common/legacy.h with legacy::Base::code4life2017() (= cotc2017
// + tooltipCode). What depends on the base: the tooltip of a lost player ("$0 lost: Invalid CONNECT" for every
// LostException of a CONNECT, the game's tooltip code; the info keeps the whole reason), the loss of a player
// (LostException, InvalidInputException, timeout) ending the game only at the next round, after that round's
// update (not turn based), the game summaries (SUMMARY: every player's transfers, diagnoses, travels and science
// projects of the round; the colored reason of the game's end).
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-timeout <ms>]
// Prints one score per player, then the game parameters (the referee's getConfiguration(): "seed=N") to stdout
// like the jar (the referee runs unchanged in the runner of the SDK 4.5.0). "null" scores when the game fails
// (the Java referee crashed or the protocol broke; the jar after 150 s). -l writes the trace of common/legacy.h
// (bot executions, view sizes, summaries, tooltips, END).
//
// League: the referee's public static LEAGUE_LEVEL = 3 (non-final, "0, 1 or 2. 3 is for silver+"; nothing sets it:
// no init property, the CLI only passes the seed): 5 molecules of each type, 3 science projects worth 50 health
// points, no diagnosed sample at the start, the module distances 2-4, no score limit (the game ends after 200
// rounds or when a player is out) = the rules of Silver and above, CodinGame's top league (Legend). The lower
// leagues' branches (cloned cloud samples of level 0, samples without expertise gain of levels 0-1, distances of 1,
// the 170 score limit, 99 molecules) are kept as code but never run.
//
// The game: each round every player answers one line: GOTO <module> (SAMPLES, DIAGNOSIS, MOLECULES, LABORATORY;
// the robot travels 2-4 rounds, during which its lines are only messages), CONNECT [data] at the module it stands
// at (SAMPLES: take an undiagnosed sample of rank 1-3; DIAGNOSIS: diagnose a carried sample, store a diagnosed one
// in the cloud, or take one from the cloud; MOLECULES: take one molecule A-E (5 each, shared); LABORATORY: research
// a carried sample whose cost the player's molecules and expertise cover: its health points, one expertise in its
// gain molecule, the molecules return to the pool) or WAIT, each with an optional message. Samples come from three
// shuffled decks (java.util.Random(seed); a drawn sample's copy goes back at the bottom of its deck); science
// projects (3 of 10, shuffled) give 50 points to every player whose expertise reaches their cost in a round (both
// players in the same round: both). A failed CONNECT (tray of 3 or 10 molecules full, no molecule left, unknown
// sample or molecule, unaffordable research, nothing to connect to) is a LostException; an unparsable line, a rank
// outside 1-3, a sample id that is not an int at DIAGNOSIS / LABORATORY an invalid input (the game's
// catch (Exception): any RuntimeException of the parse becomes one); the player is out with a score of -1. The
// score is the health points.
//
// The Java referee is followed closely, notably: the three regexes hand-matched (CASE_INSENSITIVE without
// UNICODE_CASE: ASCII folding; \s and \d ASCII; the optional data of CONNECT is a single letter A-E or -?digits
// followed by whitespace or the end, else the whole rest is the message and the data null; the message is what
// follows the whitespace after the command), Integer.valueOf of the data and its NumberFormatException (null, a
// letter, beyond int), the order of the checks of each module, the messages (19 UTF-16 chars kept, else 17 +
// "..."; any line while travelling), the static sample id counter (from 0 in the jar's fresh JVM), the shuffles
// (Collections.shuffle of LinkedLists = the array swap loop), the transfers applied in the order of the requests,
// two players taking the last molecule of a type in the same round (the pool goes negative, the input shows 0),
// a sample researched without being diagnosed, the projects removed only after every player was checked.
// The jar's NONDETERMINISM: the cloud requests of a round are a HashMap<Sample, SampleTransfer> (Sample has no
// hashCode: identity hash codes) whose values() are appended to the transfers in its iteration order. With
// requests of different players on different samples (a sample is requested once: a discoverer's request replaces
// another player's), the transfers commute (each adds to its own player's tray and removes its own sample from the
// cloud): the order shows only in the order of the view's event lines of that round, never in the players' inputs,
// the summaries (per player, one transfer each), the scores or any size. This port takes the requests in the order
// they were made (the most likely order: a 16-bin table, the same bin keeps the insertion order); the jar's order
// varies from run to run under load (HotSpot's identity hash codes).
// Validation only: compiled with -DCODE4LIFE_HINTS, the referee takes that order from the file named by the
// environment variable CODE4LIFE_HINTS (one line per round, "<round> <sample id>...": the ids of the jar's
// EV_TAKE_SAMPLE view events of that round, in its order), to replay a jar game's views exactly. The default build
// has none of this.
// Not reproduced: the tooltips' order inside one frame (the base's HashSet of identity hash codes: only their size
// counts, see legacy.h). The view (getInitDataForView / getFrameDataForView) is built as text (only its size can
// matter: a few hundred bytes per frame, far below the runner's 30000-byte limit; the messages are cut to 20
// chars). An invalid input's info carries the bot's line: it reaches the runner's 30000-byte limit only with invalid
// UTF-8 (U+FFFD), reproduced like the other ports on legacy.h. A JVM with a Turkish default locale (the jar's
// String.toUpperCase of "diagnosis" would not give DIAGNOSIS: an invalid input) is not reproduced. The game's
// distance(Point, Point) (Math.pow/sqrt) is never called: no floating point.
#include "common/legacy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>

#ifdef CODE4LIFE_HINTS
#include <cstdio>
#include <fstream>
#include <sstream>
#endif

using java::String;
using legacy::Arg;
using legacy::Lines;
using legacy::NString;

namespace {

// --- constants ---
const int LEAGUE_LEVEL = 3; // 0, 1 or 2. 3 is for silver+.

const int EV_NEW_SAMPLE = 0;
const int EV_TAKE_SAMPLE = 1;
const int EV_STORE_SAMPLE = 2;
const int EV_TAKE_RESOURCE = 3;
const int EV_DIAGNOSE = 4;
const int EV_PRODUCE = 5;
const int EV_CLONE_INITAL_SAMPLE = 6;
const int RESOURCE_PER_TYPE_BY_LEAGUE_LEVEL[] = {99, 99, 6, 5};
const int SCIENCE_PROJECTS_BY_LEAGUE_LEVEL[] = {0, 0, 3, 3};
const int INIT_DIAGNOSED_SAMPLES_BY_LEAGUE_LEVEL[] = {50, 0, 0, 0};
const int MAX_STORAGE = 10;
const int MAX_TRAY = 3;
const int SAMPLE_RANK_COUNT = 3;
const int SCIENCE_PROJECT_VALUE = LEAGUE_LEVEL < 3 ? 30 : 50;
const int MAX_SCORE = 170;

String S(i64 v) { return java::str(v); }

// enum MoleculeType { A(0), B(1), C(2), D(3), E(4) } (index = ordinal); -1: null
const int MOLECULE_COUNT = 5;
const char16_t* MOLECULE_NAME[] = {u"A", u"B", u"C", u"D", u"E"};
String moleculeName(int type) { return type < 0 ? String(u"null") : String(MOLECULE_NAME[type]); }

enum Bound { TO_DIAGNOSIS, FROM_SAMPLES, FROM_DIAGNOSIS };

enum Module { SAMPLES, DIAGNOSIS, MOLECULES, LABORATORY, START_POS };
const char16_t* MODULE_NAME[] = {u"SAMPLES", u"DIAGNOSIS", u"MOLECULES", u"LABORATORY", u"START_POS"};

// distances: a HashMap<ModulePair, Integer> (ModulePair = the HashSet of the two modules); get() of a pair of
// equal modules (a set of one) finds nothing: null
std::optional<int> distance(Module a, Module b) {
    if (a == b) return std::nullopt;
    if (LEAGUE_LEVEL >= 2) {
        if (a == START_POS || b == START_POS) return 2;
        if ((a == DIAGNOSIS && b == LABORATORY) || (a == LABORATORY && b == DIAGNOSIS)) return 4;
        return 3;
    }
    return 1;
}

struct PlayerData;

int ENTITY_COUNT = 0; // Sample.ENTITY_COUNT (static: from 0 in the jar's fresh JVM)

struct Sample {
    int expertise; // MoleculeType, -1 = null
    int life;
    std::array<int, MOLECULE_COUNT> cost;
    int id = 0, rank = 0;
    bool discovered = false;
    PlayerData* discoveredBy = nullptr;

    Sample(std::array<int, MOLECULE_COUNT> cost_, int life_, int gain) : expertise(gain), life(life_), cost(cost_) {}
    Sample* clone() const { return new Sample(cost, life, expertise); }
    String getGainChar() const { return expertise < 0 ? String(u"0") : String(MOLECULE_NAME[expertise]); }
};

struct ScienceProject {
    std::array<int, MOLECULE_COUNT> cost;
    int index = 0;
    explicit ScienceProject(std::array<int, MOLECULE_COUNT> cost_) : cost(cost_) {}
};

struct PlayerData {
    int storage[MOLECULE_COUNT] = {}, expertise[MOLECULE_COUNT] = {};
    bool dead = false, attemptConnection = false, moved = false;
    int eta = 0, score = 0, deadAt = 0, index;
    NString message, connectionData;
    std::vector<Sample*> tray; // an ArrayList: remove(Object) removes the first (identical) one
    Module from = START_POS, target = START_POS;

    explicit PlayerData(int index_) : index(index_) {}

    void die(int round) {
        if (!dead) {
            dead = true;
            deadAt = round;
            score = -1;
        }
    }

    void reset() {
        message.reset();
        attemptConnection = false;
        moved = false;
        connectionData.reset();
    }

    void setMessage(const NString& m) {
        this->message = m;
        if (m && m->size() > 19) this->message = m->substr(0, 17) + u"...";
    }

    bool isMoving() const { return eta > 0; }
};

template <class T>
void removeFirst(std::vector<T*>& list, const T* item) {
    auto it = std::find(list.begin(), list.end(), item);
    if (it != list.end()) list.erase(it);
}

struct Translatable {
    std::string code;
    std::vector<Arg> values;
};

struct Referee;

// Transfer: ProductionTransfer, SampleTransfer, ResourceTransfer
struct Transfer {
    enum Kind { PRODUCTION, SAMPLE, RESOURCE } kind;
    PlayerData* player;
    Sample* sample = nullptr;
    Sample* clone = nullptr; // SampleTransfer
    Bound bound = TO_DIAGNOSIS;
    int resourceType = 0; // ResourceTransfer

    Transfer(Kind k, PlayerData* p) : kind(k), player(p) {}
    void apply(Referee& referee);
    Translatable getSummary() const {
        switch (kind) {
        case PRODUCTION:
            if (sample->expertise < 0) return {"productionNoGain", {player->index, sample->id, sample->life}};
            return {"production", {player->index, sample->id, sample->life, String(MOLECULE_NAME[sample->expertise])}};
        case SAMPLE:
            if (bound == TO_DIAGNOSIS) return {"upload", {player->index, sample->id}};
            if (bound == FROM_SAMPLES) return {"newSample", {player->index, sample->id}};
            return {"download", {player->index, sample->id}};
        default:
            return {"takeMolecule", {player->index, String(MOLECULE_NAME[resourceType])}};
        }
    }
};

struct Diagnosis {
    PlayerData* player;
    Sample* sample;
};

struct ProjectCompletion {
    PlayerData* player;
    ScienceProject* project;
};

// ----------------------------------------------------------------------------
// The referee's regexes (Pattern.CASE_INSENSITIVE without UNICODE_CASE: ASCII folding only; \s = [ \t\n\x0B\f\r],
// \d = [0-9]), matched on the whole line (Matcher#matches). The lines never hold a line terminator (the base's
// Scanner split them there), so '.' matches every char.
// ----------------------------------------------------------------------------

// the literal (case insensitive) text `word` at s[i..]
bool hasWordAt(const String& s, size_t i, const char* word) {
    size_t w = strlen(word);
    if (i > s.size() || s.size() - i < w) return false;
    for (size_t k = 0; k < w; k++)
        if (!java::regexCharMatchesIgnoreCase(s[i + k], word[k])) return false;
    return true;
}

size_t skipSpaces(const String& s, size_t i) {
    while (i < s.size() && java::isRegexSpace(s[i])) i++;
    return i;
}

// (?:\s+)?(?:\s+(?<message>.+))?$ on s[i..]: the end, or a whitespace run then the end (no message), or a
// whitespace run then the message = all the rest (the greedy runs give back one whitespace char to the message's
// \s+; '.+' takes the rest); false when s[i] is not whitespace
bool matchTail(const String& s, size_t i, NString& message) {
    message.reset();
    if (i == s.size()) return true;
    size_t j = skipSpaces(s, i);
    if (j == i) return false;
    if (j < s.size()) message = s.substr(j);
    return true;
}

// ^GOTO\s+(?<module>LABORATORY|DIAGNOSIS|MOLECULES|SAMPLES)(?:\s+)?(?:\s+(?<message>.+))?
bool matchMove(const String& s, Module& module, NString& message) {
    if (!hasWordAt(s, 0, "GOTO")) return false;
    size_t j = skipSpaces(s, 4);
    if (j == 4) return false;
    static const struct {
        const char* word;
        Module module;
    } WORDS[] = {{"LABORATORY", LABORATORY}, {"DIAGNOSIS", DIAGNOSIS}, {"MOLECULES", MOLECULES}, {"SAMPLES", SAMPLES}};
    for (const auto& w : WORDS) {
        if (hasWordAt(s, j, w.word)) { // (the words start with different letters: one candidate)
            module = w.module;
            return matchTail(s, j + strlen(w.word), message);
        }
    }
    return false;
}

// ^WAIT(?:\s+)?(?:\s+(?<message>.+))?
bool matchWait(const String& s, NString& message) { return hasWordAt(s, 0, "WAIT") && matchTail(s, 4, message); }

// ^CONNECT(?:\s+(?<data>[ABCDE]|(?:-?\d+)))?(?:\s+)?(?:\s+(?<message>.+))?$
// The optional data group first: the whitespace run after CONNECT, then a letter A-E (either case) or -?digits (all
// of them: fewer digits leave a digit, which the tail never takes), followed by a tail that matches; else the
// group matches nothing and the tail starts right after CONNECT (then the data is null and the message, if any, is
// everything after the whitespace: "CONNECT 12x" -> message "12x").
bool matchUse(const String& s, NString& data, NString& message) {
    if (!hasWordAt(s, 0, "CONNECT")) return false;
    data.reset();
    size_t p = skipSpaces(s, 7);
    if (p > 7 && p < s.size()) {
        size_t q = p;
        if (java::regexCharMatchesIgnoreCase(s[p], 'A') || java::regexCharMatchesIgnoreCase(s[p], 'B') || java::regexCharMatchesIgnoreCase(s[p], 'C') ||
            java::regexCharMatchesIgnoreCase(s[p], 'D') || java::regexCharMatchesIgnoreCase(s[p], 'E')) {
            q = p + 1;
            if (matchTail(s, q, message)) {
                data = s.substr(p, 1);
                return true;
            }
            q = p; // [ABCDE] failed: the other alternative cannot start with a letter
        } else {
            size_t d = p;
            if (s[d] == u'-') d++;
            size_t e = d;
            while (e < s.size() && java::isRegexDigit(s[e])) e++;
            if (e > d && matchTail(s, e, message)) {
                data = s.substr(p, e - p);
                return true;
            }
        }
    }
    return matchTail(s, 7, message);
}

// Integer.valueOf(data): a NumberFormatException (a RuntimeException) for null or a non-int
struct NumberFormat {};
int integerValueOf(const NString& data) {
    i32 v;
    if (!data || !java::parseInt(*data, v)) throw NumberFormat();
    return v;
}

String resourceArrayToString(const int* a, int n = MOLECULE_COUNT) {
    String r;
    for (int i = 0; i < n; i++) {
        if (i) r += u" ";
        r += S(a[i]);
    }
    return r;
}
String resourceArrayToString(const std::array<int, MOLECULE_COUNT>& a) { return resourceArrayToString(a.data()); }

#ifdef CODE4LIFE_HINTS
// hints[round]: the sample ids of the jar's cloud transfers (its EV_TAKE_SAMPLE events) of that round, in its order
std::map<int, std::vector<int>> hints;
void loadHints() {
    const char* p = getenv("CODE4LIFE_HINTS");
    if (!p) return;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        int round, id;
        if (!(in >> round)) continue;
        std::vector<int>& h = hints[round];
        while (in >> id) h.push_back(id);
    }
}
#endif

struct Referee : legacy::MultiReferee {
    std::vector<PlayerData*> players;
    std::vector<Transfer*> transfers;
    std::vector<std::pair<Sample*, Transfer*>> cloudRequests; // HashMap<Sample, SampleTransfer>: see the header
    std::vector<Diagnosis> diagnoses;
    std::vector<ProjectCompletion> projectCompletions;
    java::Random random;
    std::vector<std::deque<Sample*>> samplePool; // LinkedLists: pop() the first, add() at the end
    std::vector<Sample*> storedSamples;          // a LinkedList
    std::vector<ScienceProject*> scienceProjects;
    i64 seed = 0;
    int molecules[MOLECULE_COUNT] = {}; // HashMap<MoleculeType, Integer>: only get / put

    Referee() { base = legacy::Base::code4life2017(); }

    // Long.valueOf: a NumberFormatException (a RuntimeException: an init error)
    static i64 longValueOf(const String& v) {
        i64 r;
        if (!java::parseLong(v, r)) throw java::Exception{java::numberFormatException(java::encodeUtf8(v))};
        return r;
    }

    Transfer* cloudGet(const Sample* sample) const {
        for (auto& e : cloudRequests)
            if (e.first == sample) return e.second;
        return nullptr;
    }
    void cloudPut(Sample* sample, Transfer* transfer) {
        for (auto& e : cloudRequests)
            if (e.first == sample) {
                e.second = transfer; // an existing key keeps its place
                return;
            }
        cloudRequests.push_back({sample, transfer});
    }

    // --- MultiReferee ---
    void initReferee(int playerCount, java::Properties& prop) override {
        // new Random(System.currentTimeMillis()).nextLong(): evaluated, unused (the CLI always gives the seed)
        i64 now = (i64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        seed = longValueOf(prop.getProperty(u"seed", S(java::Random(now).nextLong())));

        random = java::Random(seed);

        // map: the distances (distance())

        // players
        players.clear();
        for (int i = 0; i < playerCount; ++i) players.push_back(new PlayerData(i));

        // resources
        for (int type = 0; type < MOLECULE_COUNT; type++) molecules[type] = RESOURCE_PER_TYPE_BY_LEAGUE_LEVEL[LEAGUE_LEVEL];

        // samples
        initSamplePool();
        storedSamples.clear();

        // science
        initScienceProjects();

        // diagnosis
        initDiagnonisModule();

        transfers.clear();
        cloudRequests.clear();
        diagnoses.clear();
        projectCompletions.clear();
    }

    void initScienceProjects() {
        std::vector<ScienceProject*> pool;
        for (const auto& c : std::vector<std::array<int, MOLECULE_COUNT>>{{3, 3, 0, 0, 3},
                                                                          {0, 3, 3, 3, 0},
                                                                          {3, 0, 0, 3, 3},
                                                                          {0, 0, 4, 4, 0},
                                                                          {0, 4, 4, 0, 0},
                                                                          {0, 0, 0, 4, 4},
                                                                          {4, 0, 0, 0, 4},
                                                                          {3, 3, 3, 0, 0},
                                                                          {0, 0, 3, 3, 3},
                                                                          {4, 4, 0, 0, 0}})
            pool.push_back(new ScienceProject(c));
        // Collections.shuffle of a LinkedList (>= 5 elements: through an array, the same swaps)
        java::shuffle(pool, random);

        scienceProjects.clear();
        for (int i = 0; i < SCIENCE_PROJECTS_BY_LEAGUE_LEVEL[LEAGUE_LEVEL]; ++i) {
            ScienceProject* project = pool[i]; // pop()
            project->index = i;
            scienceProjects.push_back(project);
        }
    }

    void initSamplePool() {
        struct Def {
            int rank;
            std::array<int, MOLECULE_COUNT> cost;
            int life, gain;
        };
        enum { A, B, C, D, E };
        static const Def DEFS[] = {
            {0, {0, 3, 0, 0, 0}, 1, A},  {0, {0, 0, 0, 2, 1}, 1, A},  {0, {0, 1, 1, 1, 1}, 1, A},  {0, {0, 2, 0, 0, 2}, 1, A},
            {0, {0, 0, 4, 0, 0}, 10, A}, {0, {0, 1, 2, 1, 1}, 1, A},  {0, {0, 2, 2, 0, 1}, 1, A},  {0, {3, 1, 0, 0, 1}, 1, A},
            {0, {1, 0, 0, 0, 2}, 1, B},  {0, {0, 0, 0, 0, 3}, 1, B},  {0, {1, 0, 1, 1, 1}, 1, B},  {0, {0, 0, 2, 0, 2}, 1, B},
            {0, {0, 0, 0, 4, 0}, 10, B}, {0, {1, 0, 1, 2, 1}, 1, B},  {0, {1, 0, 2, 2, 0}, 1, B},  {0, {0, 1, 3, 1, 0}, 1, B},
            {0, {2, 1, 0, 0, 0}, 1, C},  {0, {0, 0, 0, 3, 0}, 1, C},  {0, {1, 1, 0, 1, 1}, 1, C},  {0, {0, 2, 0, 2, 0}, 1, C},
            {0, {0, 0, 0, 0, 4}, 10, C}, {0, {1, 1, 0, 1, 2}, 1, C},  {0, {0, 1, 0, 2, 2}, 1, C},  {0, {1, 3, 1, 0, 0}, 1, C},
            {0, {0, 2, 1, 0, 0}, 1, D},  {0, {3, 0, 0, 0, 0}, 1, D},  {0, {1, 1, 1, 0, 1}, 1, D},  {0, {2, 0, 0, 2, 0}, 1, D},
            {0, {4, 0, 0, 0, 0}, 10, D}, {0, {2, 1, 1, 0, 1}, 1, D},  {0, {2, 0, 1, 0, 2}, 1, D},  {0, {1, 0, 0, 1, 3}, 1, D},
            {0, {0, 0, 2, 1, 0}, 1, E},  {0, {0, 0, 3, 0, 0}, 1, E},  {0, {1, 1, 1, 1, 0}, 1, E},  {0, {2, 0, 2, 0, 0}, 1, E},
            {0, {0, 4, 0, 0, 0}, 10, E}, {0, {1, 2, 1, 1, 0}, 1, E},  {0, {2, 2, 0, 1, 0}, 1, E},  {0, {0, 0, 1, 3, 1}, 1, E},
            {1, {0, 0, 0, 5, 0}, 20, A}, {1, {6, 0, 0, 0, 0}, 30, A}, {1, {0, 0, 3, 2, 2}, 10, A}, {1, {0, 0, 1, 4, 2}, 20, A},
            {1, {2, 3, 0, 3, 0}, 10, A}, {1, {0, 0, 0, 5, 3}, 20, A}, {1, {0, 5, 0, 0, 0}, 20, B}, {1, {0, 6, 0, 0, 0}, 30, B},
            {1, {0, 2, 2, 3, 0}, 10, B}, {1, {2, 0, 0, 1, 4}, 20, B}, {1, {0, 2, 3, 0, 3}, 20, B}, {1, {5, 3, 0, 0, 0}, 20, B},
            {1, {0, 0, 5, 0, 0}, 20, C}, {1, {0, 0, 6, 0, 0}, 30, C}, {1, {2, 3, 0, 0, 2}, 10, C}, {1, {3, 0, 2, 3, 0}, 10, C},
            {1, {4, 2, 0, 0, 1}, 20, C}, {1, {0, 5, 3, 0, 0}, 20, C}, {1, {5, 0, 0, 0, 0}, 20, D}, {1, {0, 0, 0, 6, 0}, 30, D},
            {1, {2, 0, 0, 2, 3}, 10, D}, {1, {1, 4, 2, 0, 0}, 20, D}, {1, {0, 3, 0, 2, 3}, 10, D}, {1, {3, 0, 0, 0, 5}, 20, D},
            {1, {0, 0, 0, 0, 5}, 20, E}, {1, {0, 0, 0, 0, 6}, 30, E}, {1, {3, 2, 2, 0, 0}, 10, E}, {1, {0, 1, 4, 2, 0}, 20, E},
            {1, {3, 0, 3, 0, 2}, 10, E}, {1, {0, 0, 5, 3, 0}, 20, E}, {2, {0, 0, 0, 0, 7}, 40, A}, {2, {3, 0, 0, 0, 7}, 50, A},
            {2, {3, 0, 0, 3, 6}, 40, A}, {2, {0, 3, 3, 5, 3}, 30, A}, {2, {7, 0, 0, 0, 0}, 40, B}, {2, {7, 3, 0, 0, 0}, 50, B},
            {2, {6, 3, 0, 0, 3}, 40, B}, {2, {3, 0, 3, 3, 5}, 30, B}, {2, {0, 7, 0, 0, 0}, 40, C}, {2, {0, 7, 3, 0, 0}, 50, C},
            {2, {3, 6, 3, 0, 0}, 40, C}, {2, {5, 3, 0, 3, 3}, 30, C}, {2, {0, 0, 7, 0, 0}, 40, D}, {2, {0, 0, 7, 3, 0}, 50, D},
            {2, {0, 3, 6, 3, 0}, 40, D}, {2, {3, 5, 3, 0, 3}, 30, D}, {2, {0, 0, 0, 7, 0}, 40, E}, {2, {0, 0, 0, 7, 3}, 50, E},
            {2, {0, 0, 3, 6, 3}, 40, E}, {2, {3, 3, 5, 3, 0}, 30, E},
        };
        samplePool.assign(SAMPLE_RANK_COUNT, {});
        for (const Def& d : DEFS) samplePool[d.rank].push_back(new Sample(d.cost, d.life, d.gain));
        for (int rank = 0; rank < SAMPLE_RANK_COUNT; ++rank) {
            // Collections.shuffle of a LinkedList: through an array, the same swaps
            std::vector<Sample*> a(samplePool[rank].begin(), samplePool[rank].end());
            java::shuffle(a, random);
            samplePool[rank].assign(a.begin(), a.end());
        }
    }

    void initDiagnonisModule() {
        for (int i = 0; i < INIT_DIAGNOSED_SAMPLES_BY_LEAGUE_LEVEL[LEAGUE_LEVEL]; i++) {
            int rank = 0;
            Sample* sample = samplePool[rank].front();
            samplePool[rank].pop_front();
            samplePool[rank].push_back(sample->clone());

            sample->id = ENTITY_COUNT++;
            sample->rank = rank;
            sample->discovered = true;
            if (LEAGUE_LEVEL <= 1) sample->expertise = -1;
            storedSamples.push_back(sample);
        }
    }

    java::Properties getConfiguration() override {
        java::Properties prop;
        prop.setProperty(u"seed", S(seed));
        return prop;
    }

    // --- AbstractReferee ---
    Lines getInitInputForPlayer(int) override {
        Lines lines;
        lines.push_back(S((i64)scienceProjects.size()));
        for (ScienceProject* project : scienceProjects) lines.push_back(resourceArrayToString(project->cost));
        return lines;
    }

    void prepare(int) override {
        transfers.clear();
        diagnoses.clear();
        cloudRequests.clear();
        projectCompletions.clear();
        for (PlayerData* player : players) player->reset();
    }

    Lines getInputForPlayer(int, int playerIdx) override {
        Lines lines;
        Lines sampleLines;

        // the player first, then the others in their order
        std::vector<PlayerData*> reordered;
        for (PlayerData* p : players)
            if (p->index == playerIdx) reordered.push_back(p);
        for (PlayerData* p : players)
            if (p->index != playerIdx) reordered.push_back(p);
        for (PlayerData* player : reordered) {
            String sj = String(MODULE_NAME[player->target]) + u" " + S(player->eta) + u" " + S(player->score) + u" " + resourceArrayToString(player->storage) +
                        u" " + resourceArrayToString(player->expertise);
            for (Sample* sample : player->tray) {
                int carrier = player->index == playerIdx ? 0 : 1;
                if (sample->discovered) {
                    sampleLines.push_back(S(sample->id) + u" " + S(carrier) + u" " + S(sample->rank + 1) + u" " + sample->getGainChar() + u" " + S(sample->life) +
                                          u" " + resourceArrayToString(sample->cost));
                } else {
                    sampleLines.push_back(S(sample->id) + u" " + S(carrier) + u" " + S(sample->rank + 1) + u" 0 -1 -1 -1 -1 -1 -1");
                }
            }
            lines.push_back(sj);
        }

        for (Sample* sample : storedSamples)
            sampleLines.push_back(S(sample->id) + u" -1 " + S(sample->rank + 1) + u" " + sample->getGainChar() + u" " + S(sample->life) + u" " +
                                  resourceArrayToString(sample->cost));

        String available;
        for (int type = 0; type < MOLECULE_COUNT; type++) {
            if (type) available += u" ";
            available += S(std::max(0, molecules[type]));
        }
        lines.push_back(available);
        lines.push_back(S((i64)sampleLines.size()));
        lines.insert(lines.end(), sampleLines.begin(), sampleLines.end());
        return lines;
    }

    int getExpectedOutputLineCountForPlayer(int) override { return 1; }

    // EXPECTED_BY_LEAGUE_LEVEL: the source has a no-break space (U+00A0) between "|" and "CONNECT" in each
    String expected() const {
        static const char16_t* EXPECTED_BY_LEAGUE_LEVEL[] = {
            u"GOTO LABORATORY|DIAGNOSIS|MOLECULES | CONNECT data", u"GOTO LABORATORY|DIAGNOSIS|MOLECULES|SAMPLES | CONNECT data",
            u"GOTO LABORATORY|DIAGNOSIS|MOLECULES|SAMPLES | CONNECT data", u"GOTO LABORATORY|DIAGNOSIS|MOLECULES|SAMPLES | CONNECT data"};
        return EXPECTED_BY_LEAGUE_LEVEL[LEAGUE_LEVEL];
    }

    void handlePlayerOutput(int, int round, int playerIdx, const Lines& outputs) override {
        const String& line = outputs[0];
        PlayerData* player = players[playerIdx];

        try {
            if (player->isMoving()) {
                player->setMessage(line);
                return;
            }

            Module target;
            NString message, data;
            if (matchMove(line, target, message)) {
                // Movement
                if (target == SAMPLES && LEAGUE_LEVEL == 0) throw legacy::InvalidInputException(expected(), line);

                if (player->target != target) {
                    player->from = player->target;
                    player->target = target;
                    player->eta = *distance(player->target, player->from); // (two different modules: always in the map)
                }

                // Message
                player->setMessage(message);
                return;
            }

            if (matchUse(line, data, message)) {
                // Connect to machine
                player->attemptConnection = true;
                player->connectionData = data;

                connectToMachine(player, data);

                // Message
                player->setMessage(message);
                return;
            }

            if (matchWait(line, message)) {
                // Message
                player->setMessage(message);
                return;
            }

            throw legacy::InvalidInputException(expected(), line);

        } catch (const legacy::LostException&) {
            player->die(round);
            throw;
        } catch (const legacy::InvalidInputException&) {
            player->die(round);
            throw;
        } catch (const NumberFormat&) {
            // catch (Exception e): Integer.valueOf's NumberFormatException at DIAGNOSIS / LABORATORY
            player->die(round);
            throw legacy::InvalidInputException(expected(), line);
        } catch (const java::Exception&) {
            // catch (Exception e): the IndexOutOfBoundsException of a sample rank outside 1-3
            player->die(round);
            throw legacy::InvalidInputException(expected(), line);
        }
    }

    void connectToMachine(PlayerData* player, const NString& data) {
        try {
            switch (player->target) {
            case SAMPLES: {
                int rank;
                try {
                    rank = integerValueOf(data);
                } catch (const NumberFormat&) {
                    // new LostException("badSampleRank", data): a null data formats as "null"
                    throw legacy::LostException(std::string("badSampleRank"), {data ? Arg(*data) : Arg()});
                }
                requestSample(player, rank);
                break;
            }
            case MOLECULES: {
                int molecule = -1;
                // MoleculeType.valueOf(data.toUpperCase()): a NullPointerException for null, an
                // IllegalArgumentException for anything but A-E (the data is a letter A-E or digits)
                if (data && data->size() == 1) {
                    for (int t = 0; t < MOLECULE_COUNT; t++)
                        if (java::regexCharMatchesIgnoreCase((*data)[0], (char)('A' + t))) molecule = t;
                }
                if (molecule < 0) throw legacy::LostException(std::string("unknownMoleculeType"), {data ? Arg(*data) : Arg(u"")});
                requestMolecule(player, molecule);
                break;
            }
            case DIAGNOSIS:
                requestDiagnosis(player, integerValueOf(data));
                break;
            case LABORATORY:
                requestProduction(player, integerValueOf(data));
                break;
            case START_POS:
                throw legacy::LostException(std::string("connectToNothing"));
            default:
                break;
            }
        } catch (legacy::LostException& le) {
            le.setTooltipCode("InvalidConnect");
            throw;
        }
    }

    void requestProduction(PlayerData* player, int data) {
        for (Sample* sample : player->tray) {
            if (sample->id == data) {
                if (canAfford(player, sample->cost)) {
                    Transfer* t = new Transfer(Transfer::PRODUCTION, player);
                    t->sample = sample;
                    transfers.push_back(t);
                    return;
                }
                throw legacy::LostException(std::string("cannotAffordSample"), {data});
            }
        }
        throw legacy::LostException(std::string("sampleNotInTray"), {data});
    }

    bool canAfford(const PlayerData* player, const std::array<int, MOLECULE_COUNT>& cost) const {
        for (int i = 0; i < MOLECULE_COUNT; ++i)
            if (player->expertise[i] + player->storage[i] < cost[i]) return false;
        return true;
    }

    void requestDiagnosis(PlayerData* player, int data) {
        Sample* target = nullptr;
        for (Sample* sample : player->tray)
            if (sample->id == data) {
                target = sample;
                break;
            }

        if (target != nullptr) {
            Sample* sample = target;
            if (sample->discovered) {
                Transfer* t = new Transfer(Transfer::SAMPLE, player);
                t->sample = sample;
                t->bound = TO_DIAGNOSIS;
                transfers.push_back(t);
                return;
            } else {
                // Diagnose
                diagnoses.push_back({player, sample});
                sample->discovered = true;
                sample->discoveredBy = player;
                return;
            }
        } else {
            for (Sample* sample : storedSamples)
                if (sample->id == data) {
                    target = sample;
                    break;
                }
            if (target == nullptr) throw legacy::LostException(std::string("sampleNotFound"), {data});
            if ((int)player->tray.size() >= MAX_TRAY) throw legacy::LostException(std::string("trayIsFull"));
            Sample* sample = target;
            Transfer* transfer = new Transfer(Transfer::SAMPLE, player);
            transfer->sample = sample;
            transfer->bound = FROM_DIAGNOSIS;

            if (cloudGet(sample) == nullptr || transfer->player == sample->discoveredBy) {
                cloudPut(sample, transfer);
            } else if (LEAGUE_LEVEL == 0) {
                Sample* clonedSample = sample->clone();
                clonedSample->id = ENTITY_COUNT++;
                clonedSample->discovered = true;
                transfer->clone = clonedSample;
                cloudPut(clonedSample, transfer);
            }
        }
    }

    void requestMolecule(PlayerData* player, int type) {
        if (molecules[type] <= 0) throw legacy::LostException(std::string("notEnoughMolecules"), {String(MOLECULE_NAME[type])});
        int sum = 0;
        for (int v : player->storage) sum += v;
        if (sum >= MAX_STORAGE) throw legacy::LostException(std::string("storageIsFull"));
        Transfer* t = new Transfer(Transfer::RESOURCE, player);
        t->resourceType = type;
        transfers.push_back(t);
    }

    void requestSample(PlayerData* player, int rank) {
        if ((int)player->tray.size() >= MAX_TRAY) throw legacy::LostException(std::string("trayIsFull"));

        // samplePool.get(rank - 1): an IndexOutOfBoundsException outside 1-3 (the rank check is commented out)
        if (rank < 1 || rank > SAMPLE_RANK_COUNT) throw java::Exception{java::indexOutOfBoundsException((i64)rank - 1, SAMPLE_RANK_COUNT)};

        std::deque<Sample*>& pool = samplePool[rank - 1];
        Sample* sample = pool.front();
        pool.pop_front();
        // Just recycle it right back in there.
        pool.push_back(sample->clone());

        sample->id = ENTITY_COUNT++;
        sample->rank = rank - 1;
        sample->discovered = false;

        if (LEAGUE_LEVEL <= 1) sample->expertise = -1;

        Transfer* t = new Transfer(Transfer::SAMPLE, player);
        t->sample = sample;
        t->bound = FROM_SAMPLES;
        transfers.push_back(t);
    }

    void updateGame(int round) override {
        (void)round;
        // Move players
        for (PlayerData* player : players) {
            if (player->eta != 0) {
                player->eta--;
                player->moved = true;
            }
        }

        // Perform transfers (cloudRequests.values(): see the header)
#ifdef CODE4LIFE_HINTS
        auto h = hints.find(round);
        if (h != hints.end() && cloudRequests.size() > 1) {
            auto pos = [&](Sample* s) {
                auto it = std::find(h->second.begin(), h->second.end(), s->id);
                return it - h->second.begin();
            };
            std::stable_sort(cloudRequests.begin(), cloudRequests.end(), [&](const auto& a, const auto& b) { return pos(a.first) < pos(b.first); });
        }
#endif
        for (auto& e : cloudRequests) transfers.push_back(e.second);
        for (Transfer* transfer : transfers) transfer->apply(*this);

        // Check for science projects
        std::vector<ScienceProject*> removes;
        for (PlayerData* player : players) {
            for (ScienceProject* project : scienceProjects) {
                if (completedProject(player, project)) {
                    removes.push_back(project);
                    player->score += SCIENCE_PROJECT_VALUE;
                    projectCompletions.push_back({player, project});
                    addToolTip(player->index, translate("ProjectTooltip", {player->index}));
                }
            }
        }
        for (ScienceProject* project : removes) removeFirst(scienceProjects, project);
    }

    static bool completedProject(const PlayerData* player, const ScienceProject* project) {
        for (int i = 0; i < MOLECULE_COUNT; ++i)
            if (player->expertise[i] < project->cost[i]) return false;
        return true;
    }

    void populateMessages(std::map<std::string, String>& p) override {
        p["notEnoughMolecules"] = u"Invalid CONNECT: there are no %s type molecules left";
        p["trayIsFull"] = u"Invalid CONNECT: your robot may not carry data for more than " + S(MAX_TRAY) + u" samples";
        p["storageIsFull"] = u"Invalid CONNECT: your robot may not carry more than " + S(MAX_STORAGE) + u" molecules";
        p["nullIsInvalidSample"] = u"Invalid CONNECT: you must specify a Sample ID to connect to this module";
        p["sampleNotFound"] = u"Invalid CONNECT: the sample %d is not available";
        p["badSampleRank"] = u"Invalid CONNECT: there is no sample with rank %s";
        p["sampleNotInTray"] = u"Invalid CONNECT: you are not carrying sample %d";
        p["unknownMoleculeType"] = u"Invalid CONNECT: invalid molecule %s";
        p["cannotAffordSample"] = u"Invalid CONNECT: you do not have enough molecules/expertise to launch research on sample %d";
        p["connectToNothing"] = u"Invalid CONNECT: you must go to a module before using the connect command";
        p["InvalidConnect"] = u"Invalid CONNECT";
        p["ProjectTooltip"] = u"$%d completes a science project!";
        p["production"] = u"$%d researched medicine for sample %d, scored %d health points and gained expertise in molecule %s";
        p["productionNoGain"] = u"$%d researched medicine for sample %d, scored %d health points";
        p["upload"] = u"$%d stores sample %d on the cloud.";
        p["newSample"] = u"$%d receives sample %d.";
        p["download"] = u"$%d downloads sample %d from the cloud.";
        p["takeMolecule"] = u"$%d receives a %s molecule.";
        p["etaSAMPLES"] = u"$%d will arrive at the samples module in %d turns";
        p["etaDIAGNOSIS"] = u"$%d will arrive at the diagnosis module in %d turns";
        p["etaMOLECULES"] = u"$%d will arrive at the molecules module in %d turns";
        p["etaLABORATORY"] = u"$%d will arrive at the laboratory module in %d turns";
        p["etaSAMPLESsingular"] = u"$%d will arrive at the samples module in %d turn";
        p["etaDIAGNOSISsingular"] = u"$%d will arrive at the diagnosis module in %d turn";
        p["etaMOLECULESsingular"] = u"$%d will arrive at the molecules module in %d turn";
        p["etaLABORATORYsingular"] = u"$%d will arrive at the laboratory module in %d turn";
        p["diagnosis"] = u"$%d has diagnosed sample %d";
        p["projectCompletion"] = u"$%d has completed the science project %d and scores " + S(SCIENCE_PROJECT_VALUE) + u" health points.";
    }

    Lines getInitDataForView() override {
        Lines lines;
        lines.push_back(S(SCIENCE_PROJECT_VALUE) + u" " + S(LEAGUE_LEVEL));

        lines.push_back(S((i64)scienceProjects.size()));
        for (ScienceProject* project : scienceProjects) lines.push_back(resourceArrayToString(project->cost));

        lines.push_back(S((i64)storedSamples.size()));
        for (Sample* sample : storedSamples)
            lines.push_back(S(sample->id) + u" " + resourceArrayToString(sample->cost) + u" " + S(sample->rank) + u" " + S(sample->life) + u" " +
                            moleculeName(sample->expertise));

        lines.insert(lines.begin(), S((i64)lines.size() + 1));
        return lines;
    }

    Lines getFrameDataForView(int, int, int, bool) override {
        Lines lines;

        // Players
        for (PlayerData* player : players) {
            std::optional<int> total = distance(player->target, player->from);
            String j = String(MODULE_NAME[player->target]) + u" " + MODULE_NAME[player->from] + u" " + S(player->eta) + u" " + (player->moved ? u"1" : u"0") +
                       u" " + (total ? S(*total) : String(u"0")) + u" " + resourceArrayToString(player->storage) + u" " +
                       resourceArrayToString(player->expertise) + u" " + S(player->score) + u" " + (player->dead ? u"1" : u"0") + u" ;" +
                       (player->message ? *player->message : String());
            lines.push_back(j);
        }

        // Resources
        String res;
        for (int type = 0; type < MOLECULE_COUNT; type++) {
            if (type) res += u" ";
            res += S(molecules[type]);
        }
        lines.push_back(res);

        // Events
        Lines eventLines;
        for (Transfer* transfer : transfers) {
            if (transfer->kind == Transfer::SAMPLE) {
                Sample* s = transfer->sample;
                if (transfer->bound == FROM_SAMPLES) {
                    eventLines.push_back(S(EV_NEW_SAMPLE) + u" " + S(s->id) + u" " + resourceArrayToString(s->cost) + u" " + S(s->rank) + u" " + S(s->life) +
                                         u" " + moleculeName(s->expertise) + u" " + S(transfer->player->index));
                } else if (transfer->bound == FROM_DIAGNOSIS) {
                    if (transfer->clone != nullptr) {
                        Sample* c = transfer->clone;
                        eventLines.push_back(S(EV_CLONE_INITAL_SAMPLE) + u" " + S(c->id) + u" " + resourceArrayToString(c->cost) + u" " + S(c->rank) + u" " +
                                             S(c->life) + u" " + moleculeName(c->expertise));
                        eventLines.push_back(S(EV_TAKE_SAMPLE) + u" " + S(c->id) + u" " + S(transfer->player->index));
                    } else {
                        eventLines.push_back(S(EV_TAKE_SAMPLE) + u" " + S(s->id) + u" " + S(transfer->player->index));
                    }
                } else {
                    eventLines.push_back(S(EV_STORE_SAMPLE) + u" " + S(s->id) + u" " + S(transfer->player->index));
                }
            } else if (transfer->kind == Transfer::RESOURCE) {
                eventLines.push_back(S(EV_TAKE_RESOURCE) + u" " + MOLECULE_NAME[transfer->resourceType] + u" " + S(transfer->player->index));
            } else {
                eventLines.push_back(S(EV_PRODUCE) + u" " + S(transfer->sample->id));
            }
        }
        for (const Diagnosis& diagnosis : diagnoses) eventLines.push_back(S(EV_DIAGNOSE) + u" " + S(diagnosis.sample->id));
        lines.push_back(S((i64)eventLines.size()));
        lines.insert(lines.end(), eventLines.begin(), eventLines.end());
        return lines;
    }

    String getGameName() override { return u"Roche"; }
    NString getHeadlineAtGameStartForConsole() override { return std::nullopt; }
    int getMinimumPlayerCount() override { return 2; }
    bool showTooltips() override { return true; }
    Lines getPlayerActions(int, int) override { return {}; }
    bool isPlayerDead(int playerIdx) override { return players[playerIdx]->dead; }
    NString getDeathReason(int playerIdx) override { return u"$" + S(playerIdx) + u": Eliminated!"; }
    int getMillisTimeForRound() override { return 50; }
    int getScore(int playerIdx) override { return players[playerIdx]->score; }

    Lines getGameSummary(int round) override {
        Lines lines;
        for (int i = 0; i < (int)players.size(); ++i) {
            Lines p = getPlayerSummary(i, round);
            lines.insert(lines.end(), p.begin(), p.end());
        }
        return lines;
    }

    Lines getPlayerSummary(int playerIdx, int round) {
        Lines lines;
        PlayerData* player = players[playerIdx];

        for (Transfer* t : transfers) {
            if (t->player == player) {
                Translatable summary = t->getSummary();
                lines.push_back(translate(summary.code, summary.values));
            }
        }

        for (const Diagnosis& d : diagnoses)
            if (d.player == player) lines.push_back(translate("diagnosis", {playerIdx, d.sample->id}));

        if (player->isMoving())
            lines.push_back(translate("eta" + java::encodeUtf8(MODULE_NAME[player->target]) + (player->eta == 1 ? "singular" : ""), {playerIdx, player->eta}));

        for (const ProjectCompletion& projectCompletion : projectCompletions)
            if (projectCompletion.player == player) lines.push_back(translate("projectCompletion", {playerIdx, projectCompletion.project->index}));

        if (player->dead) {
            if (player->deadAt == round) lines.push_back(*getDeathReason(playerIdx));
        }
        return lines;
    }

    void setPlayerTimeout(int, int round, int playerIdx) override { players[playerIdx]->die(round); }
    int getMaxRoundCount(int) override { return 200; }

    bool gameOver() override {
        if (LEAGUE_LEVEL >= 3) return legacy::MultiReferee::gameOver();
        if (legacy::MultiReferee::gameOver()) return true;
        for (PlayerData* p : players)
            if (p->score >= MAX_SCORE) return true;
        return false;
    }
};

void Transfer::apply(Referee& referee) {
    switch (kind) {
    case PRODUCTION:
        removeFirst(player->tray, sample);
        for (int i = 0; i < MOLECULE_COUNT; ++i) {
            int toPay = std::max(0, sample->cost[i] - player->expertise[i]);
            player->storage[i] -= toPay;
            referee.molecules[i] += toPay;
        }
        player->score += sample->life;
        if (sample->expertise >= 0) player->expertise[sample->expertise]++;
        break;
    case SAMPLE:
        if (bound == TO_DIAGNOSIS) {
            removeFirst(player->tray, sample);
            if (std::find(referee.storedSamples.begin(), referee.storedSamples.end(), sample) == referee.storedSamples.end())
                referee.storedSamples.push_back(sample);
        } else if (bound == FROM_SAMPLES) {
            player->tray.push_back(sample);
        } else if (bound == FROM_DIAGNOSIS) {
            if (clone == nullptr) {
                player->tray.push_back(sample);
                removeFirst(referee.storedSamples, sample);
            } else {
                player->tray.push_back(clone);
            }
        }
        break;
    case RESOURCE:
        player->storage[resourceType]++;
        referee.molecules[resourceType]--;
        break;
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef CODE4LIFE_HINTS
    loadHints();
#endif
    static Referee referee;
    return legacy::run(argc, argv, referee);
}
