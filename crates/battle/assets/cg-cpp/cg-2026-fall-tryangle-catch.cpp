// Fall Challenge 2026 "TryAngle Catch" - headless C++ referee, port of
// https://github.com/eulerscheZahl/TryAngle-Catch (commit b691312) running on the CodinGame game
// engine 4.1.6 (core + runner), highest league (all commands), 2 players.
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" [-seed <N>] [-l <file>] [-timeout <ms>]
// Prints one score per player ("null" when the Java referee fails), then the game parameters like
// the jar: the generated map ("nodes", "edges", "units") and the seed.
// See common/engine.h for the -l trace and what the engine reproduces.
//
// The Java code is followed closely: same data structures, iteration orders and Java library
// semantics. Notable behaviours of the Java referee reproduced here:
//   - the map comes from java.util.Random(seed); the angle check uses Math.acos (fdlibm, see
//     java::acos) and Math.toDegrees. The edges are sorted with TimSort on the length.
//   - map generation throws (NullPointerException, no scores) when it removes an edge whose mirror
//     edge was never added (an edge and its mirror can cross each other at the center)
//   - triangles keep their identity (owner, capture rights) across updateTriangles, and tasks hold
//     the triangle they were parsed with, even once it no longer exists on the board
//   - ADD_PATH with an invalid house crashes the referee while parsing (canApply evaluates
//     board.canConnect(null, ...) even though the task already failed)
//   - MOVE towards a house that can't be reached crashes the referee in the view (TaskModule
//     serializes the move with a null target) at the end of that frame
//   - REMOVE_PATH destroying a triangle that was never drawn (no endMove since the start of the
//     game) crashes the referee in the view (TriangleView.hide on a null polygon)
//   - commands: the patterns are matched like java.util.regex (ASCII case folding, \s, \d), "MSG "
//     and "WAIT" use String.toUpperCase, numbers go through Integer.parseInt (an overflow is an
//     "Unknown command", which eliminates the player); ADD_PATH can add a self loop
//   - errors are grouped by type (at most 3 per type and turn), and the turn summary is built from a
//     HashMap<String, Integer> (its iteration order decides which lines fit the summary quota)
// Not reproduced: the view (only the crashes above matter), and maps given as game parameters (the
// CLI only passes the seed).

#include "common/engine.h"

#include <array>
#include <deque>
#include <unordered_set>

using java::String;

static String S(const std::string& ascii) { return java::ascii(ascii); }
static String S(i64 v) { return java::str(v); }

[[noreturn]] static void npe(const char* what) { throw java::Exception{std::string("java.lang.NullPointerException: ") + what}; }

// ----------------------------------------------------------------------------
// Board constants
// ----------------------------------------------------------------------------
namespace C {
const int TOP_BORDER = 70;
const int BOTTOM_BORDER = 190;
const int SIDE_BORDER = 80;
const int WIDTH = 1920 - 2 * SIDE_BORDER;
const int HEIGHT = 1080 - TOP_BORDER - BOTTOM_BORDER;
const int NODE_COUNT = 50;
const int NODE_MIN_DIST = 140;
const double SPARSE_MIN = 0.1;
const double SPARSE_MAX = 0.5;
const int MIN_TRIANGLE_COUNT = 20;
const int MAX_PATH_LENGTH = 400;
const int MAX_ANGLE = 160;
} // namespace C

// ----------------------------------------------------------------------------
// Player.java
// ----------------------------------------------------------------------------
namespace Err {
const int UNKNOWN_COMMAND = 1;
const int INVALID_NODE = 3;
const int INVALID_TRIANGLE = 4;
const int CANT_BE_APPLIED = 5;
const int CANT_ADD_PATH = 6;
const int POSITIVE_AMOUNT = 7;
const int NOT_NEAR_TRIANGLE = 8;
} // namespace Err

struct InputError {
    String message;
    int code;
    bool critical;
};

struct Player : cg::Player {
    int units = 0, triangles = 0, nodes = 0;
    std::vector<InputError> errors;

    int getExpectedOutputLines() const override { return 1; }
    Player* getOpponent();

    std::vector<InputError> popErrors() {
        std::vector<InputError> result;
        while (!errors.empty()) {
            InputError error = errors[0];
            std::vector<InputError> ofType, rest;
            for (const InputError& e : errors) (e.code == error.code ? ofType : rest).push_back(e);
            errors = rest;
            if (ofType.size() <= 3) {
                result.insert(result.end(), ofType.begin(), ofType.end());
            } else {
                result.push_back(ofType[0]);
                result.push_back(ofType[1]);
                result.push_back({S((i64)ofType.size() - 2) + u" more errors of that type", error.code, error.critical});
            }
        }
        return result;
    }
    void addError(const InputError& e) { errors.push_back(e); }
};
static Player players[2];
Player* Player::getOpponent() { return &players[(1 + index) % 2]; }

// ----------------------------------------------------------------------------
// Node.java, Edge.java, Triangle.java
// ----------------------------------------------------------------------------
struct Node {
    int id, x, y;
    std::vector<Node*> neighbors; // sorted by id (a self loop included)
    int units[2] = {0, 0};
    int remainingUnits[2] = {0, 0};

    Node(int id, int x, int y) : id(id), x(x), y(y) {}

    Node mirror() const {
        return Node(id + 1, C::WIDTH - 1 - (x - C::SIDE_BORDER) + C::SIDE_BORDER, C::HEIGHT - 1 - (y - C::TOP_BORDER) + C::TOP_BORDER);
    }
    int dist2(const Node* n) const {
        int dx = x - n->x, dy = y - n->y;
        return dx * dx + dy * dy;
    }
    double dist(const Node* n) const { return std::sqrt((double)dist2(n)); }

    bool hasNeighbor(const Node* n) const {
        for (Node* m : neighbors)
            if (m == n) return true;
        return false;
    }

    void finalizeTurn() {
        remainingUnits[0] = units[0];
        remainingUnits[1] = units[1];
    }

    // bfs(blocked): distances indexed by id, NODE_COUNT when unreachable
    std::array<int, C::NODE_COUNT> bfs(const Node* blocked = nullptr) const {
        std::array<int, C::NODE_COUNT> dist;
        dist.fill(C::NODE_COUNT);
        dist[id] = 0;
        std::vector<const Node*> queue{this};
        for (size_t q = 0; q < queue.size(); q++) {
            const Node* current = queue[q];
            for (Node* next : current->neighbors) {
                if (dist[next->id] < C::NODE_COUNT || next == blocked) continue;
                dist[next->id] = 1 + dist[current->id];
                queue.push_back(next);
            }
        }
        return dist;
    }

    bool canMoveTo(int pid, const Node* target) const {
        if (this == target) return false;
        if (remainingUnits[pid] <= 0) return false;
        return target->bfs()[id] > 0;
    }
    int getMoveAmount(int pid, int planned) const { return std::min(planned, remainingUnits[pid]); }

    Node* moveTo(int pid, Node* target, int amount) {
        if (this == target) return nullptr;
        amount = getMoveAmount(pid, amount);
        if (amount <= 0) return nullptr;
        auto dist = target->bfs();
        for (Node* next : neighbors) {
            if (dist[next->id] < dist[id]) {
                units[pid] -= amount;
                remainingUnits[pid] -= amount;
                next->units[pid] += amount;
                return next;
            }
        }
        return nullptr;
    }

    // -1 when nobody owns it
    int getOwner() const {
        if (!neighbors.empty()) {
            bool all0 = true, all1 = true;
            for (Node* n : neighbors) {
                all0 = all0 && n->units[0] > n->units[1];
                all1 = all1 && n->units[1] > n->units[0];
            }
            if (all0) return 0;
            if (all1) return 1;
        }
        if (units[0] > units[1]) return 0;
        if (units[1] > units[0]) return 1;
        return -1;
    }

    void addNeighbor(Node* n) {
        if (hasNeighbor(n)) return;
        neighbors.push_back(n);
        std::stable_sort(neighbors.begin(), neighbors.end(), [](Node* a, Node* b) { return a->id < b->id; });
        n->addNeighbor(this);
    }
    // List.remove(Object): the first occurrence
    static void removeFirst(std::vector<Node*>& list, Node* n) {
        for (size_t i = 0; i < list.size(); i++)
            if (list[i] == n) {
                list.erase(list.begin() + i);
                return;
            }
    }
    void removeNeighbor(Node* n) {
        removeFirst(neighbors, n);
        removeFirst(n->neighbors, this);
    }
};

static bool ccw(const Node* a, const Node* b, const Node* c) {
    return (c->y - a->y) * (b->x - a->x) > (b->y - a->y) * (c->x - a->x);
}

struct Edge {
    Node* n1;
    Node* n2;
    void makeNeighbors() { n1->addNeighbor(n2); }
    void unmakeNeighbors() { n1->removeNeighbor(n2); }
    bool isCrossing(const Edge& e) const {
        if (n1 == n2) return false;
        if (n1 == e.n1 || n2 == e.n1 || n1 == e.n2 || n2 == e.n2) return false;
        return ccw(n1, e.n1, e.n2) != ccw(n2, e.n1, e.n2) && ccw(n1, n2, e.n1) != ccw(n1, n2, e.n2);
    }
    double length() const {
        if (!n1 || !n2) npe("Cannot read field \"x\" because \"node\" is null");
        return n1->dist(n2);
    }
    template <class List>
    bool isBlocked(const List& edges) const {
        if (length() > C::MAX_PATH_LENGTH) return true;
        for (const auto& e : edges)
            if (isCrossing(*e)) return true;
        return false;
    }
};

struct Triangle {
    Node* node1;
    Node* node2;
    Node* node3;
    int owner = -1, initialOwner = -1;
    bool canCapture[2] = {true, true};
    bool drawn = false; // TriangleView.region != null

    bool hasNode(const Node* n) const { return node1 == n || node2 == n || node3 == n; }
    bool equals(const Triangle* t) const { return hasNode(t->node1) && hasNode(t->node2) && hasNode(t->node3); }
    bool hasNeighbor(const Node* n) const { return node1->hasNeighbor(n) || node2->hasNeighbor(n) || node3->hasNeighbor(n); }

    void finalizeTurn() { initialOwner = owner; }

    bool capture() {
        int oldOwner = owner;
        if (canCapture[0] && node1->getOwner() == 0 && node2->getOwner() == 0 && node3->getOwner() == 0) doCapture(0);
        if (canCapture[1] && node1->getOwner() == 1 && node2->getOwner() == 1 && node3->getOwner() == 1) doCapture(1);
        return owner != oldOwner;
    }
    void doCapture(int id) {
        owner = id;
        drawn = true; // view.update()
    }

    void use(int units) {
        if (owner < 0) npe("Cannot invoke \"com.codingame.game.Player.getIndex()\" because \"this.owner\" is null");
        for (Node* node : {node1, node2, node3}) {
            int take = std::min(units, node->units[owner]);
            node->units[owner] -= take;
            node->remainingUnits[owner] -= take;
        }
        canCapture[owner] = false;
        owner = -1;
        drawn = true; // view.update()
    }

    bool canUse(int pid, int units) const {
        return owner == pid && initialOwner == pid && node1->remainingUnits[pid] >= units &&
               node2->remainingUnits[pid] >= units && node3->remainingUnits[pid] >= units;
    }

    void updateAllowedCaptures() {
        for (int i = 0; i < 2; i++) {
            if (owner >= 0) canCapture[i] = owner != i;
            else canCapture[i] = canCapture[i] || (node1->units[i] == 0 && node2->units[i] == 0 && node3->units[i] == 0);
        }
    }

    std::string getInput(int pid) const {
        int ownerId = -1;
        if (owner == pid) ownerId = 0;
        else if (owner >= 0) ownerId = 1;
        return std::to_string(node1->id) + " " + std::to_string(node2->id) + " " + std::to_string(node3->id) + " " +
               std::to_string(ownerId) + " " + (canCapture[pid] ? "1" : "0") + " " + (canCapture[(pid + 1) % 2] ? "1" : "0");
    }

    void del() {
        if (!drawn) npe("Cannot invoke \"com.codingame.gameengine.module.entities.Polygon.setVisible(boolean)\" because \"this.region\" is null");
    }
};

// ----------------------------------------------------------------------------
// Tasks (engine/task)
// ----------------------------------------------------------------------------
enum TaskType { MOVE, SPAWN, ATTACK, ADD_PATH, REMOVE_PATH, DEBUG };

struct Board;
static Board* board = nullptr;
static bool canConnect(const Node* from, const Node* to);

struct Task {
    TaskType type;
    Player* player;
    bool failedParsing = false;
    Node* from = nullptr; // MOVE from, SPAWN spawn, ADD/REMOVE_PATH from
    Node* to = nullptr;   // MOVE to, ATTACK target, ADD/REMOVE_PATH to
    Triangle* triangle = nullptr;
    int amount = 0;

    int pid() const { return player->index; }

    bool allowMultiplePerFrame() const { return type == MOVE || type == SPAWN || type == DEBUG; }
    int getTaskPriority() const {
        switch (type) {
        case ATTACK: return 0;
        case MOVE: return 1;
        case SPAWN: return 2;
        case ADD_PATH: return 3;
        case REMOVE_PATH: return 4;
        default: return -1;
        }
    }
    int getTaskCost() const { return type == ADD_PATH ? from->dist2(to) : 0; }

    bool canApply(bool strict) const {
        switch (type) {
        case MOVE:
            if (!strict) return true;
            return !failedParsing && from->canMoveTo(pid(), to);
        case SPAWN:
            return !failedParsing && triangle->canUse(pid(), 0);
        case ATTACK:
            if (!strict) return true;
            return !failedParsing && triangle->canUse(pid(), 1) && triangle->hasNeighbor(to);
        case ADD_PATH: {
            bool valid = !failedParsing && triangle->canUse(pid(), 1) && !from->hasNeighbor(to);
            if (!strict) valid = true;
            valid &= canConnect(from, to); // evaluated even when the task failed parsing
            return valid;
        }
        case REMOVE_PATH:
            if (!strict) return true;
            return !failedParsing && triangle->canUse(pid(), 1) && from->hasNeighbor(to) && !triangle->hasNode(to);
        default:
            return true;
        }
    }

    void addParsingError(const String& message, int code, bool critical) {
        if (failedParsing) return;
        failedParsing = true;
        player->addError({message, code, critical});
    }
};

// ----------------------------------------------------------------------------
// Board.java
// ----------------------------------------------------------------------------
struct Board {
    std::deque<Node> nodePool;
    std::deque<Triangle> trianglePool;
    std::vector<Node*> nodes;
    std::vector<Triangle*> triangles;
    bool view = false; // BoardView created

    bool killedSurrounded = false;
    std::vector<std::pair<std::string, int>> turnSummary; // HashMap<String, Integer>, insertion order

    // TaskModule: a move animated this frame has a null target (serialize throws)
    bool moveViewCrash = false;

    Node* newNode(const Node& n) {
        nodePool.push_back(n);
        return &nodePool.back();
    }

    Node* getMirror(const Node* node) const { return node->id % 2 == 0 ? nodes.at(node->id + 1) : nodes.at(node->id - 1); }

    explicit Board(i64 seed) {
        java::Random random(seed);
        std::vector<Edge*> exsitingEdges;
        std::deque<Edge> edgePool;
        while (true) {
            // place nodes on map, ensure minimum distance and unique distances
            int tries = 0;
            nodes.clear();
            std::unordered_set<int> nodeDistances;
            while ((int)nodes.size() < C::NODE_COUNT && tries++ < 10000) {
                int x = C::SIDE_BORDER + random.nextInt(C::WIDTH);
                int y = C::TOP_BORDER + random.nextInt(C::HEIGHT);
                Node n1((int)nodes.size(), x, y);
                Node n2 = n1.mirror();
                bool reject = false;
                for (Node* n : nodes)
                    if (n->dist(&n1) < C::NODE_MIN_DIST) reject = true;
                if (!reject) reject = n1.dist(&n2) < C::NODE_MIN_DIST || nodeDistances.count(n1.dist2(&n2));
                if (!reject)
                    for (Node* n : nodes)
                        if (nodeDistances.count(n->dist2(&n1))) reject = true;
                if (reject) continue;
                if (obtuseAngle(n1, n2)) continue;
                nodeDistances.insert(n1.dist2(&n2));
                for (Node* n : nodes) nodeDistances.insert(n->dist2(&n1));
                nodes.push_back(newNode(n1));
                nodes.push_back(newNode(n2));
            }

            // add edges to create triangles
            std::vector<Edge*> edges;
            for (size_t i = 0; i < nodes.size(); i++)
                for (size_t j = i + 1; j < nodes.size(); j++) {
                    edgePool.push_back(Edge{nodes[i], nodes[j]});
                    edges.push_back(&edgePool.back());
                }
            java::sort(edges, [](Edge* a, Edge* b) {
                double d = a->length() - b->length();
                return d > 0 ? 1 : (d < 0 ? -1 : 0); // (int) Math.signum
            });
            exsitingEdges.clear();
            for (Edge* e : edges) {
                if (e->isBlocked(exsitingEdges)) continue;
                e->makeNeighbors();
                exsitingEdges.push_back(e);
            }

            updateTriangles();
            if (isStronglyConnected() && (int)triangles.size() >= C::MIN_TRIANGLE_COUNT) break;
        }

        // randomly remove some edges again
        std::unordered_set<Edge*> testedEdges;
        int removeEdgeCount = java::d2i((double)exsitingEdges.size() * (random.nextDouble() * (C::SPARSE_MAX - C::SPARSE_MIN) + C::SPARSE_MIN));
        while (removeEdgeCount > 0) {
            removeEdgeCount -= 2;
            if (exsitingEdges.empty()) throw java::Exception{"java.lang.IllegalArgumentException: bound must be positive"};
            Edge* toDisconnect = exsitingEdges[random.nextInt((i32)exsitingEdges.size())];
            if (testedEdges.count(toDisconnect)) continue;
            Node* mirror1 = getMirror(toDisconnect->n1);
            Node* mirror2 = getMirror(toDisconnect->n2);
            Edge* partner = nullptr;
            for (Edge* edge : exsitingEdges)
                if ((edge->n1 == mirror1 && edge->n2 == mirror2) || (edge->n1 == mirror2 && edge->n2 == mirror1)) partner = edge;
            testedEdges.insert(toDisconnect);
            testedEdges.insert(partner);

            updateTriangles();
            toDisconnect->unmakeNeighbors();
            if (!partner) npe("Cannot invoke \"engine.Edge.unmakeNeighbors()\" because \"partner\" is null");
            partner->unmakeNeighbors();
            updateTriangles();
            if ((int)triangles.size() >= C::MIN_TRIANGLE_COUNT && isStronglyConnected()) {
                for (Edge* r : {toDisconnect, partner})
                    for (size_t i = 0; i < exsitingEdges.size(); i++)
                        if (exsitingEdges[i] == r) {
                            exsitingEdges.erase(exsitingEdges.begin() + i);
                            break;
                        }
            } else {
                toDisconnect->makeNeighbors();
                partner->makeNeighbors();
            }
        }

        std::vector<Node*> nodesByX = nodes;
        java::sort(nodesByX, [](Node* a, Node* b) { return java::compareInt(a->x, b->x); });
        for (int i = 0; i < 3; i++) {
            Node* node = nodesByX.at(i);
            node->units[0] = 1;
            getMirror(node)->units[1] = 1;
        }

        // the game parameters, in the order the jar prints them (java.util.Properties: fixed for these keys)
        std::string nodeParam, edgeParam, unitParam;
        for (Node* n : nodes) nodeParam += (nodeParam.empty() ? "" : "_") + std::to_string(n->x) + "," + std::to_string(n->y);
        for (Node* n1 : nodes)
            for (Node* n2 : n1->neighbors)
                if (n2->id > n1->id) edgeParam += (edgeParam.empty() ? "" : "_") + std::to_string(n1->id) + "," + std::to_string(n2->id);
        for (Node* n : nodes) unitParam += (unitParam.empty() ? "" : "_") + std::to_string(n->units[0]) + "," + std::to_string(n->units[1]);
        cg::gm.setGameParameters({"nodes=" + nodeParam, "seed=" + std::to_string(seed), "edges=" + edgeParam, "units=" + unitParam});

        updateTriangles();
        view = true; // new BoardView(...)
        while (finalizeTurn(false)) {
        }
    }

    bool isStronglyConnected() const {
        for (Node* blocked : nodes) {
            Node* start = nodes.at(0);
            if (start == blocked) start = nodes.at(1);
            auto dist = start->bfs(blocked);
            int unreachable = 0;
            for (size_t i = 0; i < nodes.size(); i++)
                if (dist[i] == C::NODE_COUNT) unreachable++;
            if (unreachable > 1) return false;
        }
        return true;
    }

    bool obtuseAngle(const Node& n1, const Node& n2) const {
        std::vector<const Node*> list(nodes.begin(), nodes.end());
        list.push_back(&n1);
        list.push_back(&n2);
        for (size_t i1 = 0; i1 < list.size(); i1++)
            for (size_t i2 = i1 + 1; i2 < list.size(); i2++)
                for (size_t i3 = i2 + 1; i3 < list.size(); i3++) {
                    double a = list[i1]->dist(list[i2]);
                    double b = list[i1]->dist(list[i3]);
                    double c = list[i2]->dist(list[i3]);
                    if (a > C::MAX_PATH_LENGTH || b > C::MAX_PATH_LENGTH || c > C::MAX_PATH_LENGTH) continue;
                    double alpha = java::toDegrees(java::acos((b * b + c * c - a * a) / (2 * b * c)));
                    double beta = java::toDegrees(java::acos((a * a + c * c - b * b) / (2 * a * c)));
                    double gamma = java::toDegrees(java::acos((a * a + b * b - c * c) / (2 * a * b)));
                    if (alpha > C::MAX_ANGLE || beta > C::MAX_ANGLE || gamma > C::MAX_ANGLE) return true;
                }
        return false;
    }

    std::vector<Edge> existingEdges() const {
        std::vector<Edge> r;
        for (Node* n1 : nodes)
            for (Node* n2 : n1->neighbors)
                if (n2->id > n1->id) r.push_back(Edge{n1, n2});
        return r;
    }

    static bool canConnect(const std::vector<Edge>& existing, const Node* from, const Node* to) {
        Edge connect{const_cast<Node*>(from), const_cast<Node*>(to)};
        if (connect.length() > C::MAX_PATH_LENGTH) return false;
        for (const Edge& e : existing)
            if (connect.isCrossing(e)) return false;
        return true;
    }

    void updateTriangles() {
        std::vector<Triangle*> oldTriangles = triangles;
        std::vector<Triangle*> newTriangles = findTriangles();
        triangles.clear();
        for (Triangle* t : newTriangles) {
            Triangle* old = nullptr;
            for (Triangle* t2 : oldTriangles)
                if (t->equals(t2)) old = t2;
            triangles.push_back(old ? old : t); // keep triangle stats like the owner
        }
        if (view) {
            for (Triangle* old : oldTriangles) {
                bool contained = false;
                for (Triangle* t : triangles)
                    if (t->equals(old)) contained = true;
                if (!contained) old->del();
            }
        }
    }

    bool isInside(const Node* n, const Node* a, const Node* b, const Node* c) const {
        if (n == a || n == b || n == c) return false;
        bool ccw1 = ccw(n, a, b), ccw2 = ccw(n, b, c), ccw3 = ccw(n, c, a);
        bool has_neg = !ccw1 || !ccw2 || !ccw3;
        bool has_pos = ccw1 || ccw2 || ccw3;
        return !(has_neg && has_pos);
    }

    std::vector<Triangle*> findTriangles() {
        std::vector<Triangle*> result;
        for (Node* node1 : nodes)
            for (Node* node2 : node1->neighbors)
                for (Node* node3 : node2->neighbors) {
                    if (!(node1->id < node2->id && node2->id < node3->id && node1->hasNeighbor(node3))) continue;
                    bool inside = false;
                    for (Node* n : nodes)
                        if (isInside(n, node1, node2, node3)) {
                            inside = true;
                            break;
                        }
                    if (inside) continue;
                    trianglePool.push_back(Triangle{node1, node2, node3});
                    result.push_back(&trianglePool.back());
                }
        return result;
    }

    void addSummary(const std::string& message, int amount) {
        for (auto& e : turnSummary)
            if (e.first == message) {
                e.second += amount;
                return;
            }
        turnSummary.push_back({message, amount});
    }

    void summarizeTurn(bool gameManager) {
        if (gameManager) {
            std::vector<i32> hashes;
            for (auto& e : turnSummary) hashes.push_back(java::hashCode(S(e.first)));
            for (int k : java::hashMapOrder(hashes)) {
                const std::string& key = turnSummary[k].first;
                int count = turnSummary[k].second;
                size_t p = key.find("%d");
                std::string message = key.substr(0, p) + std::to_string(count) + key.substr(p + 2);
                if (count != 1) message += "s";
                cg::gm.addToGameSummary(S(message));
            }
        }
        turnSummary.clear();
    }

    void makeStats() {
        for (int pi = 0; pi < 2; pi++) {
            Player& player = players[pi];
            player.triangles = 0;
            player.units = 0;
            player.nodes = 0;
            for (Node* node : nodes) {
                player.units += node->units[pi];
                if (node->getOwner() == pi) player.nodes++;
            }
            for (Triangle* t : triangles)
                if (t->owner == pi) player.triangles++;
        }
    }

    bool surroundNodes() {
        if (killedSurrounded) return false;
        killedSurrounded = true;
        std::vector<bool> advantage[2];
        for (int i = 0; i < 2; i++) {
            advantage[i].assign(C::NODE_COUNT, false);
            for (Node* node : nodes) advantage[i][node->id] = node->units[i] > node->units[(i + 1) % 2];
        }
        bool surrounded = false;
        for (int i = 0; i < 2; i++) {
            int opponent = (i + 1) % 2;
            for (Node* node : nodes) {
                if (node->units[i] <= 0 || node->neighbors.empty()) continue;
                bool all = true;
                for (Node* n : node->neighbors) all = all && advantage[opponent][n->id];
                if (!all) continue;
                addSummary("$" + std::to_string(opponent) + " surrounded %d unit", node->units[i]);
                node->units[i] = 0;
                surrounded = true;
            }
        }
        return surrounded;
    }

    bool captureTriangles() {
        bool result = false;
        for (Triangle* t : triangles) {
            bool capture = t->capture();
            result |= capture;
            if (capture) addSummary("$" + std::to_string(t->owner) + " captured %d triangle", 1);
        }
        return result;
    }

    bool finalizeTurn(bool gameManager) {
        for (Node* node : nodes) node->finalizeTurn();
        for (Triangle* t : triangles) t->updateAllowedCaptures();
        if (surroundNodes() || captureTriangles()) {
            makeStats();
            summarizeTurn(gameManager);
            return true;
        }
        for (Triangle* t : triangles) {
            if (t->owner >= 0) players[t->owner].setScore(players[t->owner].getScore() + 1);
            t->finalizeTurn();
        }
        makeStats();
        summarizeTurn(gameManager);
        return false;
    }

    std::string getInput(bool initial, const Player& player, bool allowNewEdges) const {
        int pid = player.index, oid = (pid + 1) % 2;
        std::string sb;
        if (initial) {
            sb += std::to_string(nodes.size()) + "\n";
            for (Node* node : nodes) sb += std::to_string(node->id) + " " + std::to_string(node->x) + " " + std::to_string(node->y) + "\n";
        }
        std::vector<std::string> links;
        for (Node* n1 : nodes)
            for (Node* n2 : n1->neighbors)
                if (n2->id > n1->id) links.push_back(std::to_string(n1->id) + " " + std::to_string(n2->id));
        std::vector<std::string> canLink;
        if (allowNewEdges) {
            std::vector<Edge> existing = existingEdges();
            for (Node* n1 : nodes)
                for (Node* n2 : nodes)
                    if (n1->id < n2->id && !n1->hasNeighbor(n2) && canConnect(existing, n1, n2))
                        canLink.push_back(std::to_string(n1->id) + " " + std::to_string(n2->id));
        }
        sb += std::to_string(player.getScore()) + " " + std::to_string(players[oid].getScore()) + "\n";
        for (Node* node : nodes) sb += std::to_string(node->id) + " " + std::to_string(node->units[pid]) + " " + std::to_string(node->units[oid]) + "\n";
        sb += std::to_string(links.size()) + "\n";
        for (auto& l : links) sb += l + "\n";
        sb += std::to_string(triangles.size()) + "\n";
        for (Triangle* t : triangles) sb += t->getInput(pid) + "\n";
        sb += std::to_string(canLink.size()) + "\n";
        for (auto& l : canLink) sb += l + "\n";
        return sb;
    }

    void connect(Node* from, Node* to) {
        from->addNeighbor(to);
        updateTriangles();
    }
    void disconnect(Node* from, Node* to) {
        from->removeNeighbor(to);
        updateTriangles();
    }

    void endMove() {
        for (Triangle* t : triangles) t->drawn = true; // updateOwnerView
    }
};

static bool canConnect(const Node* from, const Node* to) { return Board::canConnect(board->existingEdges(), from, to); }

// ----------------------------------------------------------------------------
// TaskManager.java, Task.parseTask and the task constructors
// ----------------------------------------------------------------------------
namespace parse {

// ^\s*KEYWORD(\s+(-?)\d+)*\s*$ with CASE_INSENSITIVE; `sign[i]` allows the minus of group i
static bool match(const String& s, const char* keyword, int groups, const bool* sign, std::vector<String>& out, size_t* restAt = nullptr) {
    size_t n = s.size(), i = 0;
    while (i < n && java::isRegexSpace(s[i])) i++;
    for (const char* k = keyword; *k; k++, i++)
        if (i >= n || !java::regexCharMatchesIgnoreCase(s[i], *k)) return false;
    out.clear();
    for (int g = 0; g < groups; g++) {
        size_t ws = i;
        while (i < n && java::isRegexSpace(s[i])) i++;
        if (i == ws) return false;
        size_t start = i;
        if (sign[g] && i < n && s[i] == u'-') i++;
        size_t digits = i;
        while (i < n && java::isRegexDigit(s[i])) i++;
        if (i == digits) return false;
        out.push_back(s.substr(start, i - start));
    }
    if (restAt) {
        *restAt = i;
        return true;
    }
    while (i < n && java::isRegexSpace(s[i])) i++;
    return i == n;
}

static const bool U[4] = {false, false, false, false};
static const bool MOVE_SIGN[3] = {false, false, true};
static const bool SGN[4] = {true, true, true, true};

// TEXT: ^\s*TEXT\s+(-?\d+)\s+(-?\d+)\s+(.+)\s*$ ('.' never meets a line terminator here: the
// referee's Scanner splits lines at all of them)
static bool matchText(const String& s, std::vector<String>& out) {
    size_t rest;
    if (!match(s, "TEXT", 2, SGN, out, &rest)) return false;
    return s.size() - rest >= 2 && java::isRegexSpace(s[rest]);
}

struct ParseException {}; // NumberFormatException in a task constructor

static int parseInt(const String& s) {
    i32 v;
    if (!java::parseInt(s, v)) throw ParseException{};
    return v;
}

static Node* getNode(Task& task, const String& node) {
    int id = parseInt(node);
    if (id < 0 || id >= (int)board->nodes.size()) {
        task.addParsingError(u"Invalid node: " + node, Err::INVALID_NODE, false);
        return nullptr;
    }
    return board->nodes[id];
}

static Triangle* getTriangle(Task& task, Node* n1, Node* n2, Node* n3) {
    if (!n1 || !n2 || !n3) return nullptr;
    for (Triangle* t : board->triangles)
        if (t->hasNode(n1) && t->hasNode(n2) && t->hasNode(n3)) return t;
    task.addParsingError(S("Invalid triangle: " + std::to_string(n1->id) + " " + std::to_string(n2->id) + " " + std::to_string(n3->id)), Err::INVALID_TRIANGLE, false);
    return nullptr;
}

static std::deque<Task> taskPool;

// Task.parseTask: nullptr when no task
static Task* parseTask(Player& player, const String& command) {
    String trimmed = java::trim(command);
    if (trimmed.empty()) return nullptr;
    if (java::upperCaseEquals(trimmed, "WAIT")) return nullptr;
    Task* task = nullptr;
    std::vector<String> g;
    taskPool.push_back(Task{DEBUG, &player});
    Task& t = taskPool.back();
    try {
        if (match(command, "MOVE", 3, MOVE_SIGN, g)) {
            t.type = MOVE;
            t.from = getNode(t, g[0]);
            t.to = getNode(t, g[1]);
            t.amount = parseInt(g[2]);
            if (t.amount <= 0) t.addParsingError(u"Amount of units to move must be positive", Err::POSITIVE_AMOUNT, false);
            task = &t;
        } else if (match(command, "SPAWN", 3, U, g)) {
            t.type = SPAWN;
            t.from = getNode(t, g[0]);
            Node* n2 = getNode(t, g[1]);
            Node* n3 = getNode(t, g[2]);
            t.triangle = getTriangle(t, t.from, n2, n3);
            task = &t;
        } else if (match(command, "ATTACK", 4, U, g)) {
            t.type = ATTACK;
            Node* n1 = getNode(t, g[0]);
            Node* n2 = getNode(t, g[1]);
            Node* n3 = getNode(t, g[2]);
            t.to = getNode(t, g[3]);
            t.triangle = getTriangle(t, n1, n2, n3);
            if (!t.failedParsing && !t.triangle->hasNeighbor(t.to))
                t.addParsingError(S("Can't attack house " + std::to_string(t.to->id) + " from given triangle"), Err::NOT_NEAR_TRIANGLE, false);
            task = &t;
        } else if (match(command, "ADD_PATH", 4, U, g)) {
            t.type = ADD_PATH;
            t.from = getNode(t, g[0]);
            Node* n2 = getNode(t, g[1]);
            Node* n3 = getNode(t, g[2]);
            t.to = getNode(t, g[3]);
            t.triangle = getTriangle(t, t.from, n2, n3);
            if (!t.failedParsing && !canConnect(t.from, t.to))
                t.addParsingError(S("Can't connect houses " + std::to_string(t.from->id) + " and " + std::to_string(t.to->id)), Err::CANT_ADD_PATH, false);
            task = &t;
        } else if (match(command, "REMOVE_PATH", 4, U, g)) {
            t.type = REMOVE_PATH;
            t.from = getNode(t, g[0]);
            Node* n2 = getNode(t, g[1]);
            Node* n3 = getNode(t, g[2]);
            t.to = getNode(t, g[3]);
            t.triangle = getTriangle(t, t.from, n2, n3);
            if (!t.failedParsing && !t.triangle->hasNeighbor(t.to))
                t.addParsingError(S("House " + std::to_string(t.to->id) + " isn't connected to given triangle"), Err::NOT_NEAR_TRIANGLE, false);
            task = &t;
        } else if (match(command, "LINE", 4, SGN, g) || match(command, "RECT", 4, SGN, g)) {
            for (const String& v : g) parseInt(v);
            task = &t;
        } else if (match(command, "CIRCLE", 3, SGN, g)) {
            for (const String& v : g) parseInt(v);
            task = &t;
        } else if (matchText(command, g)) {
            for (const String& v : g) parseInt(v);
            task = &t;
        }
    } catch (const ParseException&) {
        player.addError({u"Unknown command: " + command, Err::UNKNOWN_COMMAND, true});
        return nullptr;
    }
    if (!task) {
        player.addError({u"Unknown command: " + command, Err::UNKNOWN_COMMAND, true});
        return nullptr;
    }
    // every command is available in the highest league
    if (!task->canApply(true)) task->addParsingError(u"Task can't be applied: " + command, Err::CANT_BE_APPLIED, false);
    return task;
}

} // namespace parse

struct TaskManager {
    std::vector<Task*> tasks;

    void parseTasks(Player& player, const String& command) {
        for (const String& comm : java::split(command, u';')) {
            if (java::upperCaseStartsWith(comm, "MSG ")) continue; // player.setMessage (view only)
            Task* task = parse::parseTask(player, comm);
            if (task) tasks.push_back(task);
        }
    }

    std::vector<Task*> peekTasks() {
        for (int i = (int)tasks.size() - 1; i >= 0; i--)
            if (!tasks[i]->canApply(true)) tasks.erase(tasks.begin() + i);
        std::vector<Task*> result;
        for (Task* task : tasks) {
            if (!result.empty() && result[0]->getTaskPriority() > task->getTaskPriority()) result.clear();
            if (result.empty() || task->getTaskPriority() == result[0]->getTaskPriority()) result.push_back(task);
        }
        java::sort(result, [](Task* a, Task* b) { return java::compareInt(a->getTaskCost(), b->getTaskCost()); });
        return result;
    }

    bool hasTasks() { return !peekTasks().empty(); }
    bool hasDebug() { return hasTasks() && peekTasks()[0]->getTaskPriority() == -1; }

    std::array<std::vector<Task*>, 2> popTasks() {
        std::vector<Task*> pop = peekTasks();
        for (Task* task : pop)
            for (size_t i = 0; i < tasks.size(); i++)
                if (tasks[i] == task) {
                    tasks.erase(tasks.begin() + i);
                    break;
                }
        std::array<std::vector<Task*>, 2> result;
        for (Task* task : pop) result[task->pid()].push_back(task);
        return result;
    }

    void returnTasks(const std::array<std::vector<Task*>, 2>& rest) {
        for (const auto& playerTasks : rest) tasks.insert(tasks.end(), playerTasks.begin(), playerTasks.end());
    }
};
static TaskManager taskManager;

// Board.applyDebug / applyActions
static void applyDebug() {
    if (taskManager.hasDebug()) taskManager.popTasks(); // drawn by the DebugModule
}

static void applyActions() {
    Board& b = *board;
    b.killedSurrounded = false;
    b.moveViewCrash = false;
    auto tasks = taskManager.popTasks();
    std::vector<Task*> toApply;
    for (int p = 0; p < 2; p++) {
        while (!tasks[p].empty()) {
            Task* task = tasks[p].front();
            tasks[p].erase(tasks[p].begin());
            if (!task->canApply(true)) continue; // double check because of multiple actions per turn
            toApply.push_back(task);
            if (!task->allowMultiplePerFrame()) break;
        }
    }
    java::sort(toApply, [](Task* x, Task* y) { return java::compareInt(x->getTaskCost(), y->getTaskCost()); });
    for (Task* task : toApply) {
        if (!task->canApply(false)) continue;
        if (task->allowMultiplePerFrame() && !task->canApply(true)) continue;
        std::string token = "$" + std::to_string(task->pid());
        int pid = task->pid();
        switch (task->type) {
        case SPAWN:
            b.addSummary(token + " spawned %d unit", 1);
            task->triangle->use(0);
            task->from->units[pid]++;
            break;
        case ADD_PATH:
            b.addSummary(token + " added %d path", 1);
            task->triangle->use(1);
            b.connect(task->from, task->to);
            break;
        case REMOVE_PATH:
            b.addSummary(token + " removed %d path", 1);
            task->triangle->use(1);
            b.disconnect(task->from, task->to);
            break;
        case ATTACK: {
            task->triangle->use(1);
            int opp = (pid + 1) % 2;
            task->amount = task->to->units[opp];
            task->to->units[opp] = 0;
            task->to->remainingUnits[opp] = 0;
            b.addSummary(token + " attacked %d unit", task->amount);
            break;
        }
        case MOVE:
            task->amount = task->from->getMoveAmount(pid, task->amount);
            task->to = task->from->moveTo(pid, task->to, task->amount);
            if (!task->to) b.moveViewCrash = true; // BoardView.cacheMove with a null target
            break;
        default:
            break;
        }
    }
    taskManager.returnTasks(tasks);
    b.makeStats();
    b.endMove();
    if (!taskManager.hasTasks()) b.summarizeTurn(true);
}

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
static void killPlayer(Player& player, const String& message) {
    if (!player.isActive()) return;
    cg::gm.addToGameSummary(player.getNicknameToken() + u": " + message);
    player.deactivate(u"$" + java::str(player.index) + u" " + message + u"!");
    player.setScore(-1);
    cg::gm.endGame();
}

struct Referee : cg::Referee {
    void init() override {
        static Board b(cg::gm.getSeed());
        board = &b;
        cg::gm.setMaxTurns(200);
    }

    void gameTurn(int turn) override {
        if (!taskManager.hasTasks()) {
            if (board->finalizeTurn(true)) {
                cg::gm.setMaxTurns(cg::gm.getMaxTurns() + 1);
                return;
            }
            for (cg::Player* cp : cg::gm.getActivePlayers()) {
                Player& player = *static_cast<Player*>(cp);
                Player& opponent = *player.getOpponent();
                if (player.units == 0 && player.triangles == 0) {
                    killPlayer(player, u"no units or triangles left");
                    continue;
                }
                if (player.units == 1 && player.triangles == 0 && player.getScore() < opponent.getScore()) {
                    killPlayer(player, u"can't score any more points");
                    continue;
                }
                if (opponent.triangles >= board->triangles.size() * 0.8) {
                    killPlayer(player, u"is dominated");
                    continue;
                }
                player.sendInputLine(board->getInput(turn == 1, player, cg::gm.getLeagueLevel() >= 3));
                player.execute();
            }

            for (cg::Player* cp : cg::gm.getActivePlayers()) {
                Player& player = *static_cast<Player*>(cp);
                if (player.timeout) {
                    killPlayer(player, u"timeout");
                } else {
                    if (player.outputs.empty()) throw java::Exception{java::indexOutOfBoundsException(0, 0)};
                    taskManager.parseTasks(player, player.outputs[0]);
                }
                for (const InputError& error : player.popErrors()) {
                    if (error.critical) killPlayer(player, error.message);
                    else cg::gm.addToGameSummary(player.getNicknameToken() + u": " + error.message);
                }
            }
            applyDebug();
        } else {
            cg::gm.setMaxTurns(cg::gm.getMaxTurns() + 1);
        }
        applyActions();
    }

    // TaskModule.sendFrameData: MoveTask.serialize of a move without target
    void onAfterGameTurn() override {
        if (board->moveViewCrash)
            npe("Cannot invoke \"engine.Node.getId()\" because \"this.nodeTo\" is null");
    }
};

int main(int argc, char** argv) {
    static Referee referee;
    cg::Options options;
    options.maxPlayers = 2;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
