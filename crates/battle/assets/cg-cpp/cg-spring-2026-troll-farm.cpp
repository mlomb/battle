// Troll Farm - headless C++ referee, port of JavaReferee/troll-farm.jar (league 10).
// Deterministic; reproduces the same game result as the reference jar for a given -seed.
// Engine/map validation matches upstream Troll-Farm through tag v1.0.3.
//
// Usage:
//   referee.exe -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>] [-test]
// Prints "<p0score>\n<p1score>\n" to stdout (matches the jar).
//
// Build (MSYS2 g++):  g++ -O3 -std=c++17 -march=native -o referee.exe referee.cpp
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdarg>
#include <cctype>
#include <functional>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;
typedef uint8_t u8;

// ----------------------------------------------------------------------------
// SHA-1 (minimal)
// ----------------------------------------------------------------------------
struct SHA1 {
    static inline u32 rol(u32 v, int b) { return (v << b) | (v >> (32 - b)); }
    // hash of msg (length n bytes) -> 20-byte digest
    static void hash(const u8* msg, size_t n, u8 out[20]) {
        u32 h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
        u64 ml = (u64)n * 8;
        // process in 64-byte blocks; we build a padded buffer
        size_t total = n + 1;
        while (total % 64 != 56) total++;
        total += 8;
        std::vector<u8> buf(total, 0);
        memcpy(buf.data(), msg, n);
        buf[n] = 0x80;
        for (int i = 0; i < 8; i++) buf[total - 1 - i] = (u8)((ml >> (8 * i)) & 0xff);
        for (size_t off = 0; off < total; off += 64) {
            u32 w[80];
            for (int i = 0; i < 16; i++)
                w[i] = ((u32)buf[off + i*4] << 24) | ((u32)buf[off + i*4+1] << 16) | ((u32)buf[off + i*4+2] << 8) | (u32)buf[off + i*4+3];
            for (int i = 16; i < 80; i++) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
            u32 a=h0,b=h1,c=h2,d=h3,e=h4;
            for (int i = 0; i < 80; i++) {
                u32 f, k;
                if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                u32 t = rol(a,5) + f + e + k + w[i];
                e = d; d = c; c = rol(b,30); b = a; a = t;
            }
            h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
        }
        u32 hh[5] = {h0,h1,h2,h3,h4};
        for (int i = 0; i < 5; i++) {
            out[i*4]   = (u8)(hh[i] >> 24);
            out[i*4+1] = (u8)(hh[i] >> 16);
            out[i*4+2] = (u8)(hh[i] >> 8);
            out[i*4+3] = (u8)(hh[i]);
        }
    }
};

// ----------------------------------------------------------------------------
// Java SecureRandom("SHA1PRNG") replica
// ----------------------------------------------------------------------------
struct SHA1PRNG {
    static const int DIGEST = 20;
    u8 state[20];
    bool haveState = false;
    u8 remainder[20] = {0};
    int remCount = 0;
    std::vector<u8> mdData; // pending digest update bytes

    void mdUpdate(const u8* b, size_t n) { mdData.insert(mdData.end(), b, b + n); }
    void mdDigest(const u8* extra, size_t extraN, u8 out[20]) {
        if (extraN) mdData.insert(mdData.end(), extra, extra + extraN);
        SHA1::hash(mdData.data(), mdData.size(), out);
        mdData.clear();
    }

    void setSeedLong(i64 seed) {
        u8 b[8];
        for (int i = 0; i < 8; i++) b[i] = (u8)(((u64)seed >> (8*i)) & 0xff);
        engineSetSeed(b, 8);
    }
    void engineSetSeed(const u8* seed, size_t n) {
        if (haveState) mdUpdate(state, 20);
        u8 out[20];
        mdDigest(seed, n, out);
        memcpy(state, out, 20);
        haveState = true;
        remCount = 0;
    }
    static void updateState(u8* state, const u8* output) {
        int last = 1;
        bool zf = false;
        for (int i = 0; i < 20; i++) {
            int si = (int8_t)state[i];
            int oi = (int8_t)output[i];
            int v = si + oi + last;
            u8 t = (u8)v;
            if (state[i] != t) zf = true;
            state[i] = t;
            last = v >> 8; // arithmetic
        }
        if (!zf) state[0] = (u8)(state[0] + 1);
    }
    void engineNextBytes(u8* result, int n) {
        int index = 0;
        if (remCount > 0) {
            int todo = n - index; if (todo > DIGEST - remCount) todo = DIGEST - remCount;
            for (int i = 0; i < todo; i++) {
                result[i] = remainder[remCount];
                remainder[remCount] = 0;
                remCount++;
            }
            index += todo;
        }
        u8 output[20];
        bool gotOutput = false;
        while (index < n) {
            mdUpdate(state, 20);
            mdDigest(nullptr, 0, output);
            gotOutput = true;
            updateState(state, output);
            int todo = n - index; if (todo > DIGEST) todo = DIGEST;
            for (int i = 0; i < todo; i++) result[index++] = output[i];
            remCount += todo;
        }
        if (gotOutput) memcpy(remainder, output, 20);
        // else: remainder unchanged (matches python: output stays = old remainder; remainder = output)
        else memcpy(remainder, remainder, 20); // no-op; python sets self.remainder = output where output==remainder
        remCount %= DIGEST;
    }
    u32 next(int numBits) {
        int numBytes = (numBits + 7) / 8;
        u8 b[4];
        engineNextBytes(b, numBytes);
        u32 nxt = 0;
        for (int i = 0; i < numBytes; i++) nxt = (nxt << 8) + b[i];
        return nxt >> (numBytes * 8 - numBits);
    }
    i32 nextInt() { return (i32)next(32); }
    i32 nextIntBound(i32 bound) {
        i32 r = (i32)next(31);
        i32 m = bound - 1;
        if ((bound & m) == 0) return (i32)(((i64)bound * (i64)r) >> 31);
        for (i32 u = r; (i32)(u - (r = u % bound) + m) < 0; ) u = (i32)next(31);
        return r;
    }
    i32 nextIntOriginBound(i32 origin, i32 bound) {
        i32 r = nextInt();
        if (origin < bound) {
            i32 n = bound - origin;
            i32 m = n - 1;
            if ((n & m) == 0) {
                r = (r & m) + origin;
            } else if (n > 0) {
                for (u32 u = ((u32)r) >> 1; (i32)(u + (u32)m - (u32)(r = (i32)(u % (u32)n))) < 0; )
                    u = ((u32)nextInt()) >> 1;
                r += origin;
            } else {
                while (r < origin || r >= bound) r = nextInt();
            }
        }
        return r;
    }
    double nextDouble() {
        i64 hi = (i64)next(26);
        i64 lo = (i64)next(27);
        return (double)((hi << 27) + lo) * 0x1.0p-53;
    }
};

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------
namespace C {
    const int GAME_TURNS = 300;
    const int STALL_LIMIT = 10;
    const int PLANT_MAX_SIZE = 4;
    const int PLANT_MAX_RESOURCES = 3;
    const int PLANT_COOLDOWN[4] = {8,8,9,6};
    const int PLANT_WATER_BOOST[4] = {5,5,7,2};
    const int PLANT_FINAL_HEALTH[4] = {12,12,20,6};
    const int PLANT_DELTA_HEALTH[4] = {2,2,3,1};
    const int MAP_MIN_HEIGHT = 8, MAP_MAX_HEIGHT = 11;
    const int MAP_MIN_RIVER = 2, MAP_MAX_RIVER = 3;
    const int MAP_MIN_IRON = 1, MAP_MAX_IRON = 2;
    const int MAP_MIN_ROCK = 1, MAP_MAX_ROCK = 10;
    const int MAP_MIN_TREE = 1, MAP_MAX_TREE = 3;
    const int MAP_MAX_OPP_DIST = 16;
    const int MIN_START = 2, MAX_START = 10;
    const int WOOD_POINTS = 4;
    const int LEAGUE = 10;
    /// Wall time for a bot to produce one line of output after stdin for this turn is written.
    const int PLAYER_RESPONSE_MS = 50;
    /// Same as RULES.md: heavier first-turn input/output allowed for warmup.
    const int PLAYER_FIRST_RESPONSE_MS = 1000;
    // items: 0 PLUM 1 LEMON 2 APPLE 3 BANANA 4 IRON 5 WOOD
    const int N_ITEMS = 6;
    const int IRON = 4, WOOD = 5, BANANA = 3, APPLE = 2;
}

// cell types
enum CellType { GRASS=0, WATER=1, ROCK=2, IRON_C=3, SHACK=4 };
// neighbor dirs: 0=(0,+1) 1=(+1,0) 2=(0,-1) 3=(-1,0)
static const int DX[4] = {0,1,0,-1};
static const int DY[4] = {1,0,-1,0};

// ----------------------------------------------------------------------------
// Engine state
// ----------------------------------------------------------------------------
struct Plant {
    int x, y;
    int type;       // 0..3
    int size, health, resources, cooldown;
    bool dead() const { return health <= 0; }
};

struct Unit {
    int id;
    int player;     // 0 or 1
    int x, y;
    int ms, cc, hp, cp;   // movementSpeed, carryCapacity, harvestPower, chopPower
    int carry[C::N_ITEMS] = {0,0,0,0,0,0};
    int total() const { int s=0; for(int k=0;k<C::N_ITEMS;k++) s+=carry[k]; return s; }
    int freeCap() const { return cc - total(); }
};

struct Board {
    int W = 0, H = 0;
    std::vector<int8_t> type;        // [x*H + y]
    std::vector<int>    plantIdx;    // index into plants[], or -1, by [x*H+y]; only "alive on grid" plants
    std::vector<Plant>  plants;      // insertion order; dead ones removed at end of tick
    std::vector<Unit>   units;       // insertion order
    int shackX[2], shackY[2];
    int inv[2][C::N_ITEMS] = {{0}};  // player inventories
    int score[2] = {0,0};
    bool active[2] = {true,true};
    int unitIdCounter = 0;
    int noTreeCounter = 0;
    SHA1PRNG* rng = nullptr;

    inline int idx(int x, int y) const { return x*H + y; }
    inline int8_t ttype(int x, int y) const { return type[idx(x,y)]; }
    inline bool inB(int x, int y) const { return x>=0 && x<W && y>=0 && y<H; }
    inline bool walkable(int x, int y) const { return type[idx(x,y)] == GRASS; }
    inline int neighbor(int x, int y, int d, int& nx, int& ny) const {
        nx = x+DX[d]; ny = y+DY[d];
        return inB(nx,ny) ? 1 : 0;
    }
    bool nearType(int x, int y, int t) const {
        for (int d=0; d<4; d++) { int nx,ny; if (neighbor(x,y,d,nx,ny) && ttype(nx,ny)==t) return true; }
        return false;
    }
    bool nearEdge(int x, int y) const {
        for (int d=0; d<4; d++) { int nx,ny; if (!neighbor(x,y,d,nx,ny)) return true; }
        return false;
    }
    bool nearWater(int x, int y) const { return nearType(x,y,WATER); }
    bool nearIron(int x, int y) const { return nearType(x,y,IRON_C); }

    void resize(int w, int h) {
        W=w; H=h;
        type.assign(w*h, GRASS);
        plantIdx.assign(w*h, -1);
        plants.clear(); units.clear();
        for (int i=0;i<2;i++){ for(int k=0;k<C::N_ITEMS;k++) inv[i][k]=0; score[i]=0; active[i]=true; }
        unitIdCounter=0; noTreeCounter=0;
    }
    void setCellTypeMirror(int x, int y, int8_t t) {
        type[idx(x,y)] = t;
        type[idx(W-1-x, H-1-y)] = t;
    }

    // growth cooldown for a plant of `type` at cell (x,y) with the CURRENT board terrain
    int growthCooldown(int x, int y, int t) const {
        int cd = C::PLANT_COOLDOWN[t];
        if (nearWater(x,y)) cd -= C::PLANT_WATER_BOOST[t];
        return cd;
    }
    static int initialHealth(int t) {
        return C::PLANT_FINAL_HEALTH[t] - C::PLANT_DELTA_HEALTH[t] * C::PLANT_MAX_SIZE;
    }
    // tick a plant in-place (matches Plant.tick(true))
    void tickPlant(Plant& p) {
        if (p.cooldown > 0) p.cooldown--;
        if (p.cooldown == 0 && p.health > 0) {
            if (p.size < C::PLANT_MAX_SIZE) {
                p.size++;
                p.health += C::PLANT_DELTA_HEALTH[p.type];
                p.cooldown = growthCooldown(p.x, p.y, p.type);
            } else if (p.resources < C::PLANT_MAX_RESOURCES) {
                p.resources++;
                p.cooldown = growthCooldown(p.x, p.y, p.type);
            }
        }
    }

    // ---- BFS distances ----
    // result.assign(W*H,-1); BFS FIFO; neighbours expanded in dir order 0..3 over walkable cells.
    void getDistances(const std::vector<std::pair<int,int>>& starts, std::vector<int>& result) const {
        result.assign(W*H, -1);
        static thread_local std::vector<int> q;
        q.clear(); q.reserve(W*H);
        for (auto& s : starts) { result[idx(s.first,s.second)] = 0; q.push_back(idx(s.first,s.second)); }
        size_t head = 0;
        while (head < q.size()) {
            int ci = q[head++];
            int cx = ci / H, cy = ci % H;
            int cd = result[ci];
            for (int d=0; d<4; d++) {
                int nx,ny; if (!neighbor(cx,cy,d,nx,ny)) continue;
                int ni = idx(nx,ny);
                if (type[ni]==GRASS && result[ni]==-1) { result[ni] = cd+1; q.push_back(ni); }
            }
        }
    }
    void getDistances1(int sx, int sy, std::vector<int>& result) const {
        std::vector<std::pair<int,int>> s{{sx,sy}};
        getDistances(s, result);
    }

    // getNextCell(current, target, speed) - consumes rng->nextInt(closest.size()) when tie-break needed.
    // returns cell index.
    int getNextCell(int curx, int cury, int tgtx, int tgty, int speed) {
        static thread_local std::vector<int> targetDist, sourceDist;
        getDistances1(tgtx, tgty, targetDist);
        getDistances1(curx, cury, sourceDist);
        int tIdx = idx(tgtx,tgty);
        if (sourceDist[tIdx] >= 0 && sourceDist[tIdx] <= speed) return tIdx;
        if (sourceDist[tIdx] == -1) {
            // closest reachable cells (from current) with min manhattan to target
            std::vector<std::pair<int,int>> closest;
            int best = W*H;
            for (int x=0;x<W;x++) for (int y=0;y<H;y++) {
                if (sourceDist[idx(x,y)] == -1) continue;
                int d = std::abs(x-tgtx) + std::abs(y-tgty);
                if (d < best) { best = d; closest.clear(); }
                if (d == best) closest.push_back({x,y});
            }
            getDistances(closest, targetDist);
        }
        std::vector<int> closest; // cell indices, in column-major (x outer, y inner) iteration order
        int best = W*H;
        for (int x=0;x<W;x++) for (int y=0;y<H;y++) {
            int sd = sourceDist[idx(x,y)];
            if (sd > speed || sd == -1) continue;
            int d = targetDist[idx(x,y)];
            if (d >= 0 && d < best) { best = d; closest.clear(); }
            if (d == best) closest.push_back(idx(x,y));
        }
        int pick = rng->nextIntBound((i32)closest.size());
        return closest[pick];
    }

    // recompute scores (only for active players)
    void recomputeScore(int p) {
        if (active[p])
            score[p] = inv[p][0] + inv[p][1] + inv[p][2] + inv[p][3] + C::WOOD_POINTS * inv[p][C::WOOD];
    }
};

// ----------------------------------------------------------------------------
// BirdView replica (state + random consumption only)
// ----------------------------------------------------------------------------
struct BirdView {
    bool exists = false;
    bool birdDied = false;
    bool isFlying = false;
    bool isStanding = false;
    int cellx, celly;        // bird position
    int treeIdx;             // index into board.plants of the target tree (we track by pointer-equivalent;
                             // but plants get filtered each turn => track by (x,y) of plant and re-find)
    int treex, treey;        // target tree cell
    // creatorsTexts lengths: [0]=4 frog, [1]=5 fish, [2]=4 bird, [3]=3 cat, [4]=1 turtle

    // Find the current Plant* for (treex,treey); returns index in board.plants or -1
    int findTree(const Board& b) const {
        for (int i = 0; i < (int)b.plants.size(); i++)
            if (b.plants[i].x == treex && b.plants[i].y == treey) return i;
        return -1;
    }
    // update() - called once at construction and once at the end of each board.tick (after plants filtered).
    void update(Board& b) {
        if (birdDied) return;
        // tree may be dead (removed from plants). In Java, `tree` is a Plant ref; isDead() checks health<=0.
        // The plant object still exists in Java even after removal from plants list; we approximate:
        // if we can't find it in plants, it was removed => dead.
        int ti = findTree(b);
        bool treeDead = (ti < 0) || b.plants[ti].dead();
        if (treeDead) {
            // pick the closest max-size plant via BFS from (cellx,celly)
            std::vector<int> dist;
            b.getDistances1(cellx, celly, dist);
            int closest = b.W * b.H;
            int newti = -1;
            for (int i = 0; i < (int)b.plants.size(); i++) {
                int tmp = dist[b.idx(b.plants[i].x, b.plants[i].y)];
                if (tmp < closest && b.plants[i].size == C::PLANT_MAX_SIZE) { closest = tmp; newti = i; }
            }
            // NOTE: Java's `if (tmp < closest ...)` — if dist is -1 it's < W*H, so it can pick unreachable.
            // But also tmp could be -1 which is < closest always initially. We mirror that exactly above.
            if (newti >= 0) { treex = b.plants[newti].x; treey = b.plants[newti].y; ti = newti; }
            // recompute treeDead w.r.t. (possibly new) tree
            ti = findTree(b);
            treeDead = (ti < 0) || b.plants[ti].dead();
        }
        if (cellx == treex && celly == treey) {
            (void)b.rng->nextIntOriginBound(1, 5);
            (void)b.rng->nextIntOriginBound(1, 3);
            // unit detection: no random
        } else {
            int nxi = b.getNextCell(cellx, celly, treex, treey, 1);
            int nx = nxi / b.H, ny = nxi % b.H;
            // direction/animation logic: no random
            // (java: if !isFlying ... else if next==treeCell ... else ...)
            bool nextIsTree = (nx == treex && ny == treey);
            if (!isFlying) isFlying = true;
            else if (nextIsTree) isFlying = false;
            if (!isStanding) isStanding = true;
            else if (nextIsTree) isStanding = false;
            cellx = nx; celly = ny;
        }
        // re-check tree dead after move
        ti = findTree(b);
        treeDead = (ti < 0) || b.plants[ti].dead();
        if (treeDead) birdDied = true;
    }
};

// ----------------------------------------------------------------------------
// BoardView replica (random consumption only)
// ----------------------------------------------------------------------------
static void runBoardViewRandom(Board& b, BirdView& bird) {
    SHA1PRNG& r = *b.rng;
    (void)r.nextIntBound(1840);
    (void)r.nextIntOriginBound(120, 1000);
    bool creatorsPlaced[5] = {false,false,false,false,false}; // frog, fish, bird, cat, turtle
    const int creatorsTextsLen[5] = {4,5,4,3,1};
    for (int x = 0; x < b.W; x++) {
        for (int y = 0; y < b.H; y++) {
            int8_t t = b.ttype(x,y);
            if (t != WATER) (void)r.nextIntBound(8);
            if (t == ROCK) { (void)r.nextIntBound(5); (void)r.nextIntBound(6); (void)r.nextIntBound(6); }
            if (t == GRASS) {
                int pi = b.plantIdx[b.idx(x,y)];
                bool condBird = !creatorsPlaced[1] && !creatorsPlaced[2] && pi >= 0 && b.plants[pi].size == C::PLANT_MAX_SIZE;
                if (condBird) {
                    double d1 = r.nextDouble();
                    if (d1 < 0.0005) {
                        // place bird
                        bird.exists = true;
                        bird.cellx = x; bird.celly = y;
                        bird.treex = x; bird.treey = y;
                        creatorsPlaced[2] = true; creatorsPlaced[3] = true;
                        // BirdView ctor: nextInt(creatorsTexts[2].length), nextInt(creatorsTexts[3].length)
                        (void)r.nextIntBound(creatorsTextsLen[2]);
                        (void)r.nextIntBound(creatorsTextsLen[3]);
                        // then update() once
                        bird.update(b);
                    } else {
                        double d2 = r.nextDouble();
                        if (d2 < 0.3) { (void)r.nextIntBound(16); (void)r.nextIntBound(56); (void)r.nextIntBound(56); }
                    }
                } else {
                    double d = r.nextDouble();
                    if (d < 0.3) { (void)r.nextIntBound(16); (void)r.nextIntBound(56); (void)r.nextIntBound(56); }
                }
            }
            if (t == WATER) {
                int index = 0;
                { int nx,ny; if (b.neighbor(x,y,0,nx,ny) && b.ttype(nx,ny)!=WATER) index += 4; }
                { int nx,ny; if (b.neighbor(x,y,1,nx,ny) && b.ttype(nx,ny)!=WATER) index += 1; }
                { int nx,ny; if (b.neighbor(x,y,2,nx,ny) && b.ttype(nx,ny)!=WATER) index += 8; }
                { int nx,ny; if (b.neighbor(x,y,3,nx,ny) && b.ttype(nx,ny)!=WATER) index += 2; }
                bool empty = (index == 0);
                if (index == 0) { int v = r.nextIntBound(5); static const int arr[5]={0,20,21,22,23}; index = arr[v]; }
                // 4 conditional decorative sprites: no random
                if (empty) {
                    double dW = r.nextDouble();
                    if (dW < 0.3) {
                        double dW2 = r.nextDouble();
                        if (dW2 < 0.001) {
                            int creatorIndex = r.nextIntBound(2);
                            if ((creatorIndex == 1 && creatorsPlaced[2]) || creatorsPlaced[creatorIndex])
                                creatorIndex = 1 - creatorIndex;
                            if (!creatorsPlaced[creatorIndex] && (creatorIndex == 0 || !creatorsPlaced[2])) {
                                // no random in the sprite-sheet bits; one nextInt for tooltip text:
                                (void)r.nextIntBound(creatorsTextsLen[creatorIndex]);
                                creatorsPlaced[creatorIndex] = true;
                            }
                        } else {
                            (void)r.nextIntBound(4);
                        }
                    }
                }
                if (index == 8 && !creatorsPlaced[4]) {
                    double dT = r.nextDouble();
                    if (dT < 0.0005) {
                        (void)r.nextIntBound(creatorsTextsLen[4]); // nextInt(1) - still consumes
                        creatorsPlaced[4] = true;
                    }
                }
            }
            // IRON / SHACK: no random
        }
    }
    // after the loop: grid lines / SpritePool.initPool: no random
}

// ----------------------------------------------------------------------------
// Map generation (Board.createMap, league 10)
// ----------------------------------------------------------------------------
static void createMap(Board& b, SHA1PRNG& rng, BirdView& bird) {
    b.rng = &rng;
    auto getRandomCell = [&](int& rx, int& ry) {
        while (true) {
            int x = rng.nextIntBound(b.W);
            int y = rng.nextIntBound(b.H);
            if (b.ttype(x,y)==GRASS && b.plantIdx[b.idx(x,y)]==-1) { rx=x; ry=y; return; }
        }
    };
    while (true) {
        int H = rng.nextIntBound(C::MAP_MAX_HEIGHT - C::MAP_MIN_HEIGHT + 1) + C::MAP_MIN_HEIGHT;
        int W = 2*H;
        b.resize(W, H);

        // rivers (league > 2)
        {
            int maxTotalRiver = W*H - 2*(C::MAP_MAX_ROCK + C::PLANT_MAX_SIZE*4 + C::MAP_MAX_IRON + 1)*4/5;
            int riversCount = rng.nextIntBound(C::MAP_MAX_RIVER - C::MAP_MIN_RIVER + 1) + C::MAP_MIN_RIVER;
            for (int i=0;i<riversCount;i++) {
                int rx,ry; getRandomCell(rx,ry);
                for (int j=0; j<10 && b.nearEdge(rx,ry); j++) getRandomCell(rx,ry);
                int cx=rx, cy=ry; bool valid=true;
                while (valid && maxTotalRiver > 0) {
                    b.setCellTypeMirror(cx,cy,WATER);
                    int d = rng.nextIntBound(4);
                    int nx=cx+DX[d], ny=cy+DY[d];
                    if (b.inB(nx,ny)) { cx=nx; cy=ny; } else valid=false;
                    maxTotalRiver -= 2;
                }
            }
        }

        // inventory (league > 1), iron stays (league >= 3 keeps it)
        int inventory[5];
        for (int i=0;i<5;i++) inventory[i] = C::MIN_START + rng.nextIntBound(C::MAX_START - C::MIN_START + 1);
        // shack
        int sx = rng.nextIntBound(W/2), sy = rng.nextIntBound(H);
        while (b.ttype(sx,sy)==WATER) { sx = rng.nextIntBound(W/2); sy = rng.nextIntBound(H); }
        b.setCellTypeMirror(sx,sy,SHACK);
        b.unitIdCounter = 0;
        b.shackX[0]=sx; b.shackY[0]=sy;
        b.shackX[1]=W-1-sx; b.shackY[1]=H-1-sy;
        // players init: each gets a Unit with talents {1,1,1,1} (league>=3), id = idCounter++
        for (int p=0;p<2;p++) {
            Unit u;
            u.id = b.unitIdCounter++;
            u.player = p;
            u.x = b.shackX[p]; u.y = b.shackY[p];
            u.ms=1; u.cc=1; u.hp=1; u.cp=1;
            // ctor deducts training costs from (empty) inventory then setInventory overwrites -> ignore.
            b.units.push_back(u);
            for (int k=0;k<5;k++) b.inv[p][k] = inventory[k];
            b.inv[p][C::WOOD] = 0;
            b.recomputeScore(p);
        }

        // resources (league > 2)
        auto placeTerrain = [&](int8_t t, int mn, int mx) {
            int cnt = rng.nextIntBound(mx-mn+1) + mn;
            for (int i=0;i<cnt;i++){ int x,y; getRandomCell(x,y); b.setCellTypeMirror(x,y,t); }
        };
        placeTerrain(IRON_C, C::MAP_MIN_IRON, C::MAP_MAX_IRON);
        placeTerrain(ROCK, C::MAP_MIN_ROCK, C::MAP_MAX_ROCK);

        // trees
        auto placeTree = [&](int type, int mn, int mx) -> void {
            int cnt = rng.nextIntBound(mx-mn+1) + mn;
            for (int i=0;i<cnt;i++) {
                int x,y; getRandomCell(x,y);
                Plant p; p.x=x; p.y=y; p.type=type; p.size=0; p.resources=0; p.cooldown=0;
                p.health = Board::initialHealth(type);
                int ticks = rng.nextIntOriginBound(1, b.growthCooldown(x,y,type) * (C::PLANT_MAX_SIZE + C::PLANT_MAX_RESOURCES));
                for (int t=0;t<ticks;t++) b.tickPlant(p);
                b.plants.push_back(p);
                b.plantIdx[b.idx(x,y)] = (int)b.plants.size()-1;
                int mx2 = b.W-1-x, my2 = b.H-1-y;
                if (mx2==x && my2==y) return;
                Plant p2; p2.x=mx2; p2.y=my2; p2.type=type; p2.size=0; p2.resources=0; p2.cooldown=0;
                p2.health = Board::initialHealth(type);
                for (int t=0;t<ticks;t++) b.tickPlant(p2);
                b.plants.push_back(p2);
                b.plantIdx[b.idx(mx2,my2)] = (int)b.plants.size()-1;
            }
        };
        placeTree(0, C::MAP_MIN_TREE, C::MAP_MAX_TREE);
        placeTree(1, C::MAP_MIN_TREE, C::MAP_MAX_TREE);
        placeTree(2, C::MAP_MIN_TREE, C::MAP_MAX_TREE);
        placeTree(3, C::MAP_MIN_TREE, C::MAP_MAX_TREE);

        // isValid(league 10)
        bool valid = true;
        if (b.nearIron(sx,sy)) valid = false;
        if (valid) {
            bool shackReachable = false;
            for (int d=0;d<4;d++){ int nx,ny; if (b.neighbor(sx,sy,d,nx,ny) && b.walkable(nx,ny)) shackReachable=true; }
            if (!shackReachable) valid=false;
        }
        std::vector<std::pair<int,int>> walkables, irons;
        for (int x=0;x<W;x++) for (int y=0;y<H;y++) {
            if (b.walkable(x,y)) walkables.push_back({x,y});
            if (b.ttype(x,y)==IRON_C) irons.push_back({x,y});
        }
        if (valid) {
            bool canReachIron = false;
            for (auto& ic : irons) for (int d=0;d<4;d++){ int nx,ny; if (b.neighbor(ic.first,ic.second,d,nx,ny) && b.walkable(nx,ny)) canReachIron=true; }
            if (!canReachIron) valid=false; // league > 2
        }
        if (valid) {
            std::vector<int> dist;
            b.getDistances1(walkables[0].first, walkables[0].second, dist);
            for (auto& w : walkables) if (dist[b.idx(w.first,w.second)] == -1) { valid=false; break; }
        }
        if (valid) {
            // Board.isValid: min over walkable neighbors of P1 shack of (dist from P0 shack) + 1
            // (upstream v1.0.3 / commit 293d185)
            std::vector<int> sd;
            b.getDistances1(sx, sy, sd);
            int opponentDist = INT_MAX;
            int s1x=b.shackX[1], s1y=b.shackY[1];
            for (int d=0;d<4;d++) {
                int nx, ny;
                if (!b.neighbor(s1x, s1y, d, nx, ny) || !b.walkable(nx, ny)) continue;
                int dd = sd[b.idx(nx, ny)];
                opponentDist = std::min(opponentDist, dd + 1);
            }
            if (opponentDist > C::MAP_MAX_OPP_DIST) valid = false;
        }
        // league<3 plant check skipped at league 10
        if (valid) {
            // initView -> BoardView ctor consumes random (and possibly BirdView)
            runBoardViewRandom(b, bird);
            return;
        }
        // else: loop again (next iteration calls resize anyway)
    }
}

// ----------------------------------------------------------------------------
// Child process I/O (Windows)
// ----------------------------------------------------------------------------
#ifdef _WIN32
struct ChildProc {
    HANDLE hProcess = nullptr;
    HANDLE hStdinW = nullptr;   // write to child's stdin
    HANDLE hStdoutR = nullptr;  // read child's stdout (overlapped)
    HANDLE hStderrW = nullptr;  // child's stderr (we keep a file or our stderr)
    HANDLE hReadEvent = nullptr; // manual-reset event for overlapped reads on hStdoutR
    bool alive = false;
    std::string readBuf; // leftover

    bool start(const std::string& cmd, HANDLE stderrTarget) {
        SECURITY_ATTRIBUTES sa{}; sa.nLength=sizeof(sa); sa.bInheritHandle=TRUE; sa.lpSecurityDescriptor=nullptr;
        HANDLE inR=nullptr, inW=nullptr, outR=nullptr, outW=nullptr;
        // stdin is plain synchronous — we only do blocking writes from the referee.
        if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
        // stdout must be overlapped on our (read) side so readLine() can wait with a precise
        // millisecond timeout via WaitForSingleObject and wake immediately when bytes arrive.
        // CreatePipe doesn't support FILE_FLAG_OVERLAPPED, so build the pair manually with a
        // unique-named pipe (name only needs to be unique per pipe, never advertised externally).
        char pipeName[96];
        static volatile LONG s_pipeCounter = 0;
        LONG pipeId = InterlockedIncrement(&s_pipeCounter);
        std::snprintf(pipeName, sizeof(pipeName),
                      "\\\\.\\pipe\\battle_referee_%lu_%ld",
                      (unsigned long)GetCurrentProcessId(), (long)pipeId);
        outR = CreateNamedPipeA(pipeName,
                                PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_BYTE | PIPE_WAIT,
                                1, 65536, 65536, 0, &sa);
        if (outR == INVALID_HANDLE_VALUE) {
            CloseHandle(inR); CloseHandle(inW);
            return false;
        }
        outW = CreateFileA(pipeName, GENERIC_WRITE, 0, &sa, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outW == INVALID_HANDLE_VALUE) {
            CloseHandle(inR); CloseHandle(inW); CloseHandle(outR);
            return false;
        }
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{}; si.cb=sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inR;
        si.hStdOutput = outW;
        si.hStdError = stderrTarget ? stderrTarget : GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        // If the command contains shell metacharacters, run it through cmd /c (with '/'->'\' so cmd
        // doesn't mistake path separators for option prefixes). Otherwise run it directly via CreateProcess.
        bool needShell = cmd.find_first_of("&|<>^()") != std::string::npos;
        std::string full;
        if (needShell) {
            std::string c2 = cmd;
            for (auto& ch : c2) if (ch=='/') ch='\\';
            full = "cmd /c \"" + c2 + "\"";
        } else {
            full = cmd;
            for (auto& ch : full) if (ch=='/') ch='\\';
        }
        std::vector<char> cmdline(full.begin(), full.end()); cmdline.push_back(0);
        BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inR); CloseHandle(outW);
        if (!ok) { CloseHandle(inW); CloseHandle(outR); return false; }
        CloseHandle(pi.hThread);
        // Manual-reset event for overlapped reads. Manual-reset matches GetOverlappedResult's
        // expectations and lets us reuse the OVERLAPPED across reads.
        hReadEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!hReadEvent) { CloseHandle(inW); CloseHandle(outR); TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); return false; }
        hProcess = pi.hProcess; hStdinW = inW; hStdoutR = outR; alive=true;
        return true;
    }
    void writeAll(const std::string& s) {
        if (!alive) return;
        size_t off=0; DWORD wr;
        while (off < s.size()) {
            if (!WriteFile(hStdinW, s.data()+off, (DWORD)(s.size()-off), &wr, nullptr) || wr==0) { alive=false; return; }
            off += wr;
        }
    }
    // read one line (without trailing \n or \r). Returns false on EOF/error/timeout with no line.
    // On timeout, sets timedOut=true and alive=false (incomplete line does not count).
    bool readLine(std::string& line, int timeoutMs, bool& timedOut) {
        using clock = std::chrono::steady_clock;
        timedOut = false;
        line.clear();
        auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
        char tmp[4096];
        while (true) {
            size_t nl = readBuf.find('\n');
            if (nl != std::string::npos) {
                line = readBuf.substr(0, nl);
                readBuf.erase(0, nl+1);
                if (!line.empty() && line.back()=='\r') line.pop_back();
                return true;
            }
            auto now = clock::now();
            if (now >= deadline) {
                timedOut = true;
                alive = false;
                return false;
            }
            DWORD ms = (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (ms < 1) ms = 1;
            // Issue an overlapped read; it either completes synchronously (data already buffered),
            // returns ERROR_IO_PENDING (we wait on the event), or fails (EOF / broken pipe).
            OVERLAPPED ov{}; ov.hEvent = hReadEvent;
            ResetEvent(hReadEvent);
            DWORD rd = 0;
            BOOL ok = ReadFile(hStdoutR, tmp, (DWORD)sizeof(tmp), &rd, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    alive=false;
                    if (!readBuf.empty()) { line = readBuf; readBuf.clear(); if(!line.empty()&&line.back()=='\r') line.pop_back(); return true; }
                    return false;
                }
                DWORD wr = WaitForSingleObject(hReadEvent, ms);
                if (wr == WAIT_TIMEOUT) {
                    // Bot blew its budget: cancel the pending read so it doesn't smear into a
                    // future turn, then drain the cancellation result so the OVERLAPPED is freed.
                    CancelIoEx(hStdoutR, &ov);
                    DWORD ignored = 0;
                    GetOverlappedResult(hStdoutR, &ov, &ignored, TRUE);
                    timedOut = true;
                    alive = false;
                    return false;
                }
                if (wr != WAIT_OBJECT_0) {
                    alive = false;
                    if (!readBuf.empty()) { line = readBuf; readBuf.clear(); if(!line.empty()&&line.back()=='\r') line.pop_back(); return true; }
                    return false;
                }
                if (!GetOverlappedResult(hStdoutR, &ov, &rd, FALSE)) {
                    alive=false;
                    if (!readBuf.empty()) { line = readBuf; readBuf.clear(); if(!line.empty()&&line.back()=='\r') line.pop_back(); return true; }
                    return false;
                }
            }
            if (rd == 0) {
                alive=false;
                if (!readBuf.empty()) { line = readBuf; readBuf.clear(); if(!line.empty()&&line.back()=='\r') line.pop_back(); return true; }
                return false;
            }
            readBuf.append(tmp, rd);
        }
    }
    void kill() {
        if (hStdinW) CloseHandle(hStdinW);
        if (hProcess) { TerminateProcess(hProcess, 0); WaitForSingleObject(hProcess, 2000); CloseHandle(hProcess); }
        if (hStdoutR) CloseHandle(hStdoutR);
        if (hReadEvent) CloseHandle(hReadEvent);
        hProcess=nullptr; hStdinW=nullptr; hStdoutR=nullptr; hReadEvent=nullptr; alive=false;
    }
};
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <wordexp.h>
#include <poll.h>
#include <cerrno>

// Child process I/O (POSIX — macOS / Linux)
struct ChildProc {
    pid_t pid = -1;
    int fdStdinW = -1;
    int fdStdoutR = -1;
    bool alive = false;
    std::string readBuf;

    bool start(const std::string& cmd, const char* stderrPath) {
        int inPipe[2], outPipe[2];
        if (pipe(inPipe) != 0) return false;
        if (pipe(outPipe) != 0) { close(inPipe[0]); close(inPipe[1]); return false; }

        int errFd;
        if (stderrPath) {
            errFd = open(stderrPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (errFd < 0) {
                close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
                return false;
            }
        } else {
            errFd = open("/dev/null", O_WRONLY);
            if (errFd < 0) {
                close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
                return false;
            }
        }

        pid_t c = fork();
        if (c < 0) {
            close(errFd);
            close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
            return false;
        }
        if (c == 0) {
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errFd, STDERR_FILENO);
            close(errFd);
            close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);

            bool needShell = cmd.find_first_of("&|<>^()") != std::string::npos;
            if (needShell) {
                execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            } else {
                wordexp_t w{};
                int we = wordexp(cmd.c_str(), &w, WRDE_NOCMD);
                if (we != 0 || w.we_wordc == 0) _exit(127);
                execvp(w.we_wordv[0], w.we_wordv);
                wordfree(&w);
                _exit(127);
            }
            _exit(127);
        }

        close(errFd);
        close(inPipe[0]);
        close(outPipe[1]);
        fdStdinW = inPipe[1];
        fdStdoutR = outPipe[0];
        pid = c;
        alive = true;
        return true;
    }
    void writeAll(const std::string& s) {
        if (!alive) return;
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n;
            do {
                n = write(fdStdinW, s.data() + off, s.size() - off);
            } while (n < 0 && errno == EINTR);
            if (n <= 0) { alive = false; return; }
            off += (size_t)n;
        }
    }
    bool readLine(std::string& line, int timeoutMs, bool& timedOut) {
        using clock = std::chrono::steady_clock;
        timedOut = false;
        line.clear();
        auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            size_t nl = readBuf.find('\n');
            if (nl != std::string::npos) {
                line = readBuf.substr(0, nl);
                readBuf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            auto now = clock::now();
            if (now >= deadline) {
                timedOut = true;
                alive = false;
                return false;
            }
            int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (ms < 1) ms = 1;
            struct pollfd pfd;
            pfd.fd = fdStdoutR;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr;
            do {
                pr = poll(&pfd, 1, ms);
            } while (pr < 0 && errno == EINTR);
            if (pr == 0) {
                timedOut = true;
                alive = false;
                return false;
            }
            if (pr < 0) {
                alive = false;
                if (!readBuf.empty()) {
                    line = readBuf;
                    readBuf.clear();
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
                return false;
            }
            if (pfd.revents & (POLLERR | POLLNVAL)) {
                alive = false;
                return false;
            }
            char tmp[4096];
            ssize_t rd;
            do {
                rd = read(fdStdoutR, tmp, sizeof(tmp));
            } while (rd < 0 && errno == EINTR);
            if (rd < 0) {
                alive = false;
                if (!readBuf.empty()) {
                    line = readBuf;
                    readBuf.clear();
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
                return false;
            }
            if (rd == 0) {
                alive = false;
                if (!readBuf.empty()) {
                    line = readBuf;
                    readBuf.clear();
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
                return false;
            }
            readBuf.append(tmp, (size_t)rd);
        }
    }
    void kill() {
        if (fdStdinW >= 0) { close(fdStdinW); fdStdinW = -1; }
        if (fdStdoutR >= 0) { close(fdStdoutR); fdStdoutR = -1; }
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            pid = -1;
        }
        alive = false;
    }
};
#endif

// ----------------------------------------------------------------------------
// Input building
// ----------------------------------------------------------------------------
static void buildInitialInput(const Board& b, int id, std::string& out) {
    out += std::to_string(b.W); out += ' '; out += std::to_string(b.H); out += '\n';
    for (int y=0;y<b.H;y++) {
        for (int x=0;x<b.W;x++) {
            int8_t t = b.ttype(x,y);
            char c = '.';
            if (t==GRASS) c='.';
            else if (t==WATER) c='~';
            else if (t==IRON_C) c='+';
            else if (t==ROCK) c='#';
            else if (t==SHACK) {
                int owner = (b.shackX[0]==x && b.shackY[0]==y) ? 0 : 1;
                c = (char)('0' + ((owner - id + 2) % 2));
            }
            out += c;
        }
        out += '\n';
    }
}
static const char* ITEM_NAME[6] = {"PLUM","LEMON","APPLE","BANANA","IRON","WOOD"};
static void appendInv(const int* inv, std::string& out) {
    for (int i=0;i<6;i++){ if(i) out+=' '; out+=std::to_string(inv[i]); }
}
static void buildTurnInput(const Board& b, int id, std::string& out) {
    // own inventory first
    appendInv(b.inv[(0+id)%2], out); out += '\n';
    appendInv(b.inv[(1+id)%2], out); out += '\n';
    out += std::to_string(b.plants.size()); out += '\n';
    for (auto& p : b.plants) {
        out += ITEM_NAME[p.type]; out+=' ';
        out += std::to_string(p.x); out+=' ';
        out += std::to_string(p.y); out+=' ';
        out += std::to_string(p.size); out+=' ';
        out += std::to_string(p.health); out+=' ';
        out += std::to_string(p.resources); out+=' ';
        out += std::to_string(p.cooldown); out += '\n';
    }
    out += std::to_string(b.units.size()); out += '\n';
    for (auto& u : b.units) {
        int outId = (id + u.player) % 2;
        out += std::to_string(u.id); out+=' ';
        out += std::to_string(outId); out+=' ';
        out += std::to_string(u.x); out+=' ';
        out += std::to_string(u.y); out+=' ';
        out += std::to_string(u.ms); out+=' ';
        out += std::to_string(u.cc); out+=' ';
        out += std::to_string(u.hp); out+=' ';
        out += std::to_string(u.cp); out+=' ';
        for (int k=0;k<6;k++){ out += std::to_string(u.carry[k]); if(k<5) out+=' '; }
        out += '\n';
    }
}

// ----------------------------------------------------------------------------
// Task system
// ----------------------------------------------------------------------------
enum TaskKind { TK_MOVE, TK_HARVEST, TK_PLANT, TK_CHOP, TK_PICK, TK_TRAIN, TK_DROP, TK_MINE };
struct Task {
    TaskKind kind;
    int player;
    int unitId;       // -1 if none (TRAIN)
    int unitInitialCarry = 0;
    bool failedParsing = false;
    bool applied = false;
    // type-specific
    int targetX=0, targetY=0;   // MOVE: resolved target cell
    int itemType=0;             // PLANT/PICK
    int talents[4]={0,0,0,0};   // TRAIN
    int priority() const {
        switch(kind){case TK_MOVE:return 1;case TK_HARVEST:return 2;case TK_PLANT:return 3;case TK_CHOP:return 4;case TK_PICK:return 5;case TK_TRAIN:return 6;case TK_DROP:return 7;case TK_MINE:return 8;}
        return 99;
    }
    int requiredLeague() const {
        switch(kind){case TK_MOVE:return 1;case TK_HARVEST:return 1;case TK_PLANT:return 2;case TK_CHOP:return 3;case TK_PICK:return 2;case TK_TRAIN:return 2;case TK_DROP:return 1;case TK_MINE:return 3;}
        return 1;
    }
};

struct Game {
    Board board;
    BirdView bird;
    bool gameOver = false;
    bool playerHasCriticalError[2] = {false,false};
    // trace
    FILE* trace = nullptr;
    void tracef(const char* fmt, ...) {
        if (!trace) return;
        va_list ap; va_start(ap, fmt); vfprintf(trace, fmt, ap); va_end(ap);
    }

    Unit* findUnit(int id) {
        for (auto& u : board.units) if (u.id == id) return &u;
        return nullptr;
    }
    int unitIndex(int id) {
        for (int i=0;i<(int)board.units.size();i++) if (board.units[i].id==id) return i;
        return -1;
    }

    // ----- parsing helpers -----
    // Parse a non-negative integer from str; return false if not a valid int that fits in int32.
    static bool parseInt(const std::string& s, i64& out) {
        if (s.empty()) return false;
        size_t i=0; bool neg=false;
        if (s[0]=='-'){ neg=true; i=1; if(s.size()==1) return false; }
        else if (s[0]=='+'){ i=1; if(s.size()==1) return false; }
        i64 v=0;
        for (; i<s.size(); i++) { if (s[i]<'0'||s[i]>'9') return false; v = v*10 + (s[i]-'0'); if (v > 5000000000LL) return false; }
        if (neg) v = -v;
        if (v > 2147483647LL || v < -2147483648LL) return false;
        out = v;
        return true;
    }
    static std::string upper(std::string s){ for(auto&c:s) c=(char)toupper((unsigned char)c); return s; }
    static std::string trim(const std::string& s){
        size_t a=0,b=s.size();
        while(a<b && isspace((unsigned char)s[a])) a++;
        while(b>a && isspace((unsigned char)s[b-1])) b--;
        return s.substr(a,b-a);
    }
    // tokenize on whitespace
    static std::vector<std::string> tokens(const std::string& s){
        std::vector<std::string> r; size_t i=0;
        while(i<s.size()){
            while(i<s.size() && isspace((unsigned char)s[i])) i++;
            if(i>=s.size()) break;
            size_t j=i; while(j<s.size() && !isspace((unsigned char)s[j])) j++;
            r.push_back(s.substr(i,j-i)); i=j;
        }
        return r;
    }
    static bool isAllDigits(const std::string& s){ if(s.empty())return false; for(char c:s) if(c<'0'||c>'9') return false; return true; }
    static bool isIntTok(const std::string& s){ // -?\d+
        if(s.empty())return false; size_t i=0; if(s[0]=='-'){i=1; if(s.size()==1)return false;} for(;i<s.size();i++) if(s[i]<'0'||s[i]>'9')return false; return true; }
    static bool isWord(const std::string& s){ if(s.empty())return false; for(char c:s) if(!(isalnum((unsigned char)c)||c=='_'))return false; return true; }

    // returns item type for a "type" token (PLANT/PICK): 0..5 ; -1 if word not recognized and not numeric -> caller treats as throw.
    // out param `threw` set true if numeric parse failed (Integer.parseInt throw -> UNKNOWN_COMMAND critical)
    int parseItemType(const std::string& tok, bool& threw){
        threw=false;
        std::string u = upper(tok);
        if(u=="PLUM")return 0; if(u=="LEMON")return 1; if(u=="APPLE")return 2; if(u=="BANANA")return 3;
        if(u=="IRON")return 4; if(u=="WOOD")return 5;
        i64 v;
        if(!parseInt(tok, v)){ threw=true; return -999; }
        return (int)v;
    }

    // Try to match command to a task; mirrors Task.parseTask. Returns:
    //  - parsed task in `t` and returns 1 if a (possibly fail-parsed) task object was created
    //  - returns 0 if no pattern matched -> caller: UNKNOWN_COMMAND critical
    //  - returns -1 if a ctor "threw" -> caller: UNKNOWN_COMMAND critical
    // The task's failedParsing / errors are recorded.
    // `usedUnits` semantics handled by caller.
    int matchTask(int player, const std::string& cmd, Task& t, bool& sawWaitOrEmpty) {
        sawWaitOrEmpty=false;
        std::string ct = trim(cmd);
        if (ct.empty()) { sawWaitOrEmpty=true; return 0; } // handled as null (ignored) by caller
        if (upper(ct)=="WAIT") { sawWaitOrEmpty=true; return 0; }
        auto tk = tokens(ct);
        if (tk.empty()) { sawWaitOrEmpty=true; return 0; }
        std::string act = upper(tk[0]);
        // Patterns require exact token counts.
        // MOVE id x y :  ^\s*MOVE\s+\d+\s+-?\d+\s+-?\d+\s*$
        if (act=="MOVE" && tk.size()==4 && isAllDigits(tk[1]) && isIntTok(tk[2]) && isIntTok(tk[3])) {
            t = Task{}; t.kind=TK_MOVE; t.player=player;
            i64 id; if(!parseInt(tk[1],id)) return -1; // \d+ but Integer.parseInt may overflow
            t.unitId=(int)id;
            // parseUnit
            Unit* u = findUnit((int)id);
            if (!u) { t.unitId=-1; t.failedParsing=true; addErr(player,false); return 1; } // UNIT_NOT_FOUND non-crit
            t.unitInitialCarry = u->total();
            if (u->player != player) { t.failedParsing=true; addErr(player,false); return 1; } // not owned
            // x,y
            i64 x,y; if(!parseInt(tk[2],x)||!parseInt(tk[3],y)) return -1;
            if (x<0||x>=board.W||y<0||y>=board.H) { t.failedParsing=true; addErr(player,false); return 1; } // OUT_OF_BOARD
            int nxi = board.getNextCell(u->x,u->y,(int)x,(int)y,u->ms);
            t.targetX = nxi/board.H; t.targetY = nxi%board.H;
            return 1;
        }
        // HARVEST id : ^\s*HARVEST\s+\d+\s*$
        if (act=="HARVEST" && tk.size()==2 && isAllDigits(tk[1])) {
            t=Task{}; t.kind=TK_HARVEST; t.player=player;
            i64 id; if(!parseInt(tk[1],id)) return -1; t.unitId=(int)id;
            Unit* u=findUnit((int)id);
            if(!u){ t.unitId=-1; t.failedParsing=true; addErr(player,false); return 1; }
            t.unitInitialCarry=u->total();
            if(u->player!=player){ t.failedParsing=true; addErr(player,false); return 1; }
            int pi = board.plantIdx[board.idx(u->x,u->y)];
            if(pi<0){ t.failedParsing=true; addErr(player,false); return 1; }
            else if(board.plants[pi].resources==0){ t.failedParsing=true; addErr(player,false); return 1; }
            if(u->freeCap()==0){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} return 1; }
            if(u->hp==0){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} return 1; }
            return 1;
        }
        // PLANT id type : ^\s*PLANT\s+\d+\s+\w+\s*$
        if (act=="PLANT" && tk.size()==3 && isAllDigits(tk[1]) && isWord(tk[2])) {
            t=Task{}; t.kind=TK_PLANT; t.player=player;
            i64 id; if(!parseInt(tk[1],id)) return -1; t.unitId=(int)id;
            Unit* u=findUnit((int)id);
            if(!u){ t.unitId=-1; t.failedParsing=true; addErr(player,false); return 1; }
            t.unitInitialCarry=u->total();
            if(u->player!=player){ t.failedParsing=true; addErr(player,false); return 1; }
            bool threw; int type = parseItemType(tk[2], threw);
            if (threw) return -1;
            t.itemType=type;
            bool isPlantItem = (type>=0 && type<6 && type<=C::BANANA);
            if (type<0 || type>=6 || !isPlantItem) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if (board.ttype(u->x,u->y)!=GRASS) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if (board.plantIdx[board.idx(u->x,u->y)]>=0) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            // unit inventory count of `type`: if type out of range this would AIOOBE in java... but earlier check
            // would have set failedParsing for invalid type; java still executes `unit.getInventory().getItemCount(type)`
            // unconditionally though! If type is e.g. 6 or negative -> ArrayIndexOutOfBounds -> caught? No: the ctor
            // is wrapped in try/catch in parseTask, which catches Exception => UNKNOWN_COMMAND critical.
            // So an out-of-range numeric type in PLANT => critical kill.
            if (type<0 || type>=6) return -1;
            if (u->carry[type]==0) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            return 1;
        }
        // CHOP id : ^\s*CHOP\s+\d+\s*$
        if (act=="CHOP" && tk.size()==2 && isAllDigits(tk[1])) {
            t=Task{}; t.kind=TK_CHOP; t.player=player;
            i64 id; if(!parseInt(tk[1],id)) return -1; t.unitId=(int)id;
            Unit* u=findUnit((int)id);
            if(!u){ t.unitId=-1; t.failedParsing=true; addErr(player,false); return 1; }
            t.unitInitialCarry=u->total();
            if(u->player!=player){ t.failedParsing=true; addErr(player,false); return 1; }
            if(board.plantIdx[board.idx(u->x,u->y)]<0){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if(u->cp==0){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            return 1;
        }
        // PICK id type : ^\s*PICK\s+\d+\s+\w+\s*$
        if (act=="PICK" && tk.size()==3 && isAllDigits(tk[1]) && isWord(tk[2])) {
            t=Task{}; t.kind=TK_PICK; t.player=player;
            i64 id; if(!parseInt(tk[1],id)) return -1; t.unitId=(int)id;
            Unit* u=findUnit((int)id);
            if(!u){ t.unitId=-1; t.failedParsing=true; addErr(player,false); return 1; }
            t.unitInitialCarry=u->total();
            if(u->player!=player){ t.failedParsing=true; addErr(player,false); return 1; }
            if(u->freeCap()==0){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            bool threw; int type = parseItemType(tk[2], threw);
            if (threw) return -1;
            t.itemType=type;
            bool isPlantItem = (type>=0 && type<6 && type<=C::BANANA);
            if (type<0 || type>=6 || !isPlantItem) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            else {
                // player.getInventory().getItemCount(type) - type valid here (0..3)
                if (board.inv[player][type]==0) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            }
            // Java still calls unit.isNearShack() unconditionally
            if (!isNearShack(*u)) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            return 1;
        }
        // TRAIN ms cc hp cp : ^\s*TRAIN\s+\d+\s+\d+\s+\d+\s+\d+\s*$
        if (act=="TRAIN" && tk.size()==5 && isAllDigits(tk[1]) && isAllDigits(tk[2]) && isAllDigits(tk[3]) && isAllDigits(tk[4])) {
            t=Task{}; t.kind=TK_TRAIN; t.player=player; t.unitId=-1;
            i64 ms,cc,hp,cp;
            if(!parseInt(tk[1],ms)||!parseInt(tk[2],cc)||!parseInt(tk[3],hp)||!parseInt(tk[4],cp)) return -1;
            t.talents[0]=(int)ms; t.talents[1]=(int)cc; t.talents[2]=(int)hp; t.talents[3]=(int)cp;
            int maxFH = 20; // max(PLANT_FINAL_HEALTH)
            if (ms<1 || ms>(i64)board.W*board.H) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if (cc<0 || cc>1000) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if (hp<0 || hp>C::PLANT_MAX_RESOURCES) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            // league 10 >= 3, so the "chop power not available" check never fires
            if (cp<0 || cp>maxFH) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if (!canTrain(player, t.talents)) { if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            return 1;
        }
        // DROP id : ^\s*DROP\s+\d+\s*$
        if (act=="DROP" && tk.size()==2 && isAllDigits(tk[1])) {
            t=Task{}; t.kind=TK_DROP; t.player=player;
            i64 id; if(!parseInt(tk[1],id)) return -1; t.unitId=(int)id;
            Unit* u=findUnit((int)id);
            if(!u){ t.unitId=-1; t.failedParsing=true; addErr(player,false); return 1; }
            t.unitInitialCarry=u->total();
            if(u->player!=player){ t.failedParsing=true; addErr(player,false); return 1; }
            if(u->total()==0){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if(!isNearShack(*u)){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            return 1;
        }
        // MINE id : ^\s*MINE\s+\d+\s*$
        if (act=="MINE" && tk.size()==2 && isAllDigits(tk[1])) {
            t=Task{}; t.kind=TK_MINE; t.player=player;
            i64 id; if(!parseInt(tk[1],id)) return -1; t.unitId=(int)id;
            Unit* u=findUnit((int)id);
            if(!u){ t.unitId=-1; t.failedParsing=true; addErr(player,false); return 1; }
            t.unitInitialCarry=u->total();
            if(u->player!=player){ t.failedParsing=true; addErr(player,false); return 1; }
            if(!board.nearIron(u->x,u->y)){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if(u->freeCap()==0){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            if(u->cp==0){ if(!t.failedParsing){t.failedParsing=true; addErr(player,false);} }
            return 1;
        }
        // WAIT pattern handled above (returns 0). No pattern matched:
        return 0;
    }

    bool isNearShack(const Unit& u) {
        int sx=board.shackX[u.player], sy=board.shackY[u.player];
        if (u.x==sx && u.y==sy) return true;
        for (int d=0;d<4;d++){ int nx,ny; if(board.neighbor(u.x,u.y,d,nx,ny) && nx==sx && ny==sy) return true; }
        return false;
    }
    void getTrainingCosts(int player, const int* talents, int* costs) {
        int base = 0; for (auto& u : board.units) if (u.player==player) base++;
        for (int i=0;i<6;i++) costs[i]=0;
        for (int i=0;i<=C::APPLE;i++) costs[i] = base + talents[i]*talents[i];
        costs[C::IRON] = base + talents[3]*talents[3]; // league>=3
    }
    bool canTrain(int player, const int* talents) {
        int costs[6]; getTrainingCosts(player, talents, costs);
        for (int i=0;i<6;i++) if (costs[i] > board.inv[player][i]) return false;
        return true;
    }
    void addErr(int player, bool critical) {
        if (critical) playerHasCriticalError[player]=true;
    }
    void killPlayer(int player) {
        if (!board.active[player]) return;
        board.active[player]=false;
        board.score[player]=-2;
        gameOver=true;
    }

    // ----- apply tasks -----
    void applyMove(std::vector<Task*>& group); // forward
    void applyHarvest(std::vector<Task*>& group);
    void applyChop(std::vector<Task*>& group);
    void applyPlant(std::vector<Task*>& group);
    void applyPick(Task& t);
    void applyTrain(Task& t);
    void applyDrop(Task& t);
    void applyMine(Task& t);

    void tick(std::vector<Task>& tasks, int turn) {
        // process priority groups
        // tasks vector preserved (player0 then player1, in command order). We pop min-priority groups.
        std::vector<bool> done(tasks.size(), false);
        while (true) {
            int minPrio = 1<<30;
            for (size_t i=0;i<tasks.size();i++) if (!done[i]) minPrio = std::min(minPrio, tasks[i].priority());
            if (minPrio == (1<<30)) break;
            std::vector<Task*> group;
            for (size_t i=0;i<tasks.size();i++) if (!done[i] && tasks[i].priority()==minPrio) { group.push_back(&tasks[i]); done[i]=true; }
            // dispatch
            switch (group[0]->kind) {
                case TK_MOVE: applyMove(group); break;
                case TK_HARVEST: applyHarvest(group); break;
                case TK_PLANT: applyPlant(group); break;
                case TK_CHOP: applyChop(group); break;
                case TK_PICK: for (auto* t:group) applyPick(*t); break;
                case TK_TRAIN: for (auto* t:group) applyTrain(*t); break;
                case TK_DROP: for (auto* t:group) applyDrop(*t); break;
                case TK_MINE: for (auto* t:group) applyMine(*t); break;
            }
            if (trace) {
                for (auto* t : group) traceSummary(*t);
            }
        }
        // plant tick
        for (auto& p : board.plants) board.tickPlant(p);
        // remove dead plants (keep order), rebuild plantIdx
        {
            std::vector<Plant> kept; kept.reserve(board.plants.size());
            for (auto& p : board.plants) if (!p.dead()) kept.push_back(p);
            board.plants.swap(kept);
            std::fill(board.plantIdx.begin(), board.plantIdx.end(), -1);
            for (int i=0;i<(int)board.plants.size();i++) board.plantIdx[board.idx(board.plants[i].x, board.plants[i].y)] = i;
        }
        // recompute scores
        board.recomputeScore(0); board.recomputeScore(1);
        // view.update -> birdView.update
        if (bird.exists) bird.update(board);
    }

    void traceSummary(const Task& t) {
        // mimic the per-task summaries (best-effort, used for trace only)
        if (!trace) return;
        // not exhaustive; main verification uses input traces, not summaries.
    }

    // helper: harvest one unit (Unit.harvest(power))
    void unitHarvest(Unit& u, int power, Plant& p) {
        if (power > u.hp || u.total() >= u.cc) return;
        u.carry[p.type]++;
        if (p.resources > 0) p.resources--;
    }
    void unitMine(Unit& u) {
        for (int i=0;i<u.cp && u.total()<u.cc; i++) u.carry[C::IRON]++;
    }
    void plantDamage(Plant& p, int dmg) {
        p.health = std::max(p.health - dmg, 0);
        if (p.dead()) board.plantIdx[board.idx(p.x,p.y)] = -1;
    }

    // group helpers by cell, sorted by cell id (x + (y<<16)), preserving task order within a cell
    std::vector<std::vector<Task*>> groupByCell(std::vector<Task*>& group) {
        // collect distinct cells in (cellId) order
        std::vector<std::pair<int, std::vector<Task*>>> byCell;
        auto cellOf = [&](Task* t)->std::pair<int,int>{
            Unit* u = findUnit(t->unitId); return {u->x, u->y};
        };
        for (auto* t : group) {
            auto c = cellOf(t);
            int cid = c.first + (c.second << 16);
            bool found=false;
            for (auto& e : byCell) if (e.first==cid) { e.second.push_back(t); found=true; break; }
            if (!found) byCell.push_back({cid, {t}});
        }
        std::sort(byCell.begin(), byCell.end(), [](const auto&a,const auto&b){return a.first<b.first;});
        std::vector<std::vector<Task*>> res;
        for (auto& e : byCell) res.push_back(std::move(e.second));
        return res;
    }
};

// ----- MoveTask.apply -----
void Game::applyMove(std::vector<Task*>& group) {
    // only first works
    // group is all MOVE tasks in command order. Resolve per player independently.
    for (int player=0; player<2; player++) {
        // tasks of this player in the group
        std::vector<Task*> moves;
        for (auto* t : group) if (t->player==player) moves.push_back(t);
        if (moves.empty()) continue;
        // units of this player in insertion order
        std::vector<int> uidx; // indices into board.units
        for (int i=0;i<(int)board.units.size();i++) if (board.units[i].player==player) uidx.push_back(i);
        int n = (int)uidx.size();
        // targets[i] initially = current cell of units[uidx[i]]
        std::vector<std::pair<int,int>> targets(n);
        for (int i=0;i<n;i++) targets[i] = {board.units[uidx[i]].x, board.units[uidx[i]].y};
        // apply move targets: targets.set(units.indexOf(m.unit), m.target)
        for (auto* m : moves) {
            int pos=-1; for (int i=0;i<n;i++) if (board.units[uidx[i]].id==m->unitId) { pos=i; break; }
            if (pos>=0) targets[pos] = {m->targetX, m->targetY};
        }
        // working lists: units (indices into board.units) and targets, both mutable, indices kept in sync
        std::vector<int> U; std::vector<std::pair<int,int>> T;
        for (int i=0;i<n;i++){ U.push_back(uidx[i]); T.push_back(targets[i]); }
        // occupied grid
        std::vector<char> occ(board.W*board.H, 0);
        // remove non-movers (iterate from end), keep their cells occupied
        for (int i=(int)U.size()-1;i>=0;i--) {
            int ux=board.units[U[i]].x, uy=board.units[U[i]].y;
            occ[board.idx(ux,uy)] = 1;
            if (ux==T[i].first && uy==T[i].second) { U.erase(U.begin()+i); T.erase(T.begin()+i); }
        }
        bool madeMove=true, resolveBlocking=false;
        while (madeMove) {
            madeMove=false;
            // single target a->b
            std::vector<int> freq(board.W*board.H, 0);
            for (auto& c : T) freq[board.idx(c.first,c.second)]++;
            for (int i=(int)U.size()-1;i>=0;i--) {
                int ci=board.idx(T[i].first,T[i].second);
                if ((resolveBlocking || freq[ci]==1) && !occ[ci]) {
                    occ[ci]=1;
                    int ux=board.units[U[i]].x, uy=board.units[U[i]].y;
                    occ[board.idx(ux,uy)]=0;
                    board.units[U[i]].x=T[i].first; board.units[U[i]].y=T[i].second;
                    U.erase(U.begin()+i); T.erase(T.begin()+i);
                    madeMove=true; resolveBlocking=false;
                    // NOTE: freq array now stale w.r.t. removed entry, but Java also doesn't recompute
                    // freq within the inner loop; it does recompute next outer iteration. But Java's
                    // freq array is per outer iteration and the inner loop uses the SAME (stale) freq.
                    // We replicate: don't recompute freq here. (We changed T but freq still old; that's fine
                    // because Java's freq counts the ORIGINAL T at start of this outer iteration.)
                }
            }
            if (madeMove) continue;
            // circular swaps
            for (int startIndex=0; startIndex<(int)U.size(); startIndex++) {
                std::vector<int> path; path.push_back(startIndex);
                bool looped=false;
                for (int i=0;i<(int)U.size()+1;i++) {
                    auto tgt = T[path.back()];
                    // find first unit (in U order) whose current cell == tgt
                    int found=-1;
                    for (int k=0;k<(int)U.size();k++) if (board.units[U[k]].x==tgt.first && board.units[U[k]].y==tgt.second) { found=k; break; }
                    if (found<0) break;
                    if (found==path[0]) { looped=true; break; }
                    path.push_back(found);
                }
                if (looped) {
                    std::sort(path.begin(), path.end());
                    for (int i=(int)path.size()-1;i>=0;i--) {
                        int idx=path[i];
                        board.units[U[idx]].x=T[idx].first; board.units[U[idx]].y=T[idx].second;
                        U.erase(U.begin()+idx); T.erase(T.begin()+idx);
                        madeMove=true;
                    }
                }
            }
            if (!madeMove && !resolveBlocking) { resolveBlocking=true; madeMove=true; }
        }
        // mark applied: applied if unit not still in U
        for (auto* t : moves) {
            bool stillThere=false;
            for (int k : U) if (board.units[k].id==t->unitId) { stillThere=true; break; }
            t->applied = !stillThere;
            if (!t->applied) addErr(player,false); // MOVE_BLOCKED non-crit
        }
    }
}

// ----- HarvestTask.apply -----
void Game::applyHarvest(std::vector<Task*>& group) {
    for (auto& byCell : groupByCell(group)) {
        // plant at cell of first task's unit
        Unit* u0 = findUnit(byCell[0]->unitId);
        int pi = board.plantIdx[board.idx(u0->x,u0->y)];
        Plant& p = board.plants[pi];
        for (int i=1; i<=C::PLANT_MAX_RESOURCES; i++) {
            if (p.resources == 0) break;
            for (auto* t : byCell) {
                Unit* u = findUnit(t->unitId);
                unitHarvest(*u, i, p);
                t->applied=true;
            }
        }
    }
}
// ----- ChopTask.apply -----
void Game::applyChop(std::vector<Task*>& group) {
    for (auto& byCell : groupByCell(group)) {
        Unit* u0 = findUnit(byCell[0]->unitId);
        int pi = board.plantIdx[board.idx(u0->x,u0->y)];
        Plant& p = board.plants[pi];
        for (auto* t : byCell) {
            Unit* u = findUnit(t->unitId);
            plantDamage(p, u->cp);
            t->applied=true;
        }
        if (p.dead()) {
            int remainingWood = p.size;
            for (int i=0;i<p.size && remainingWood>0;i++) {
                for (auto* t : byCell) {
                    Unit* u = findUnit(t->unitId);
                    if (u->freeCap()>0) { u->carry[C::WOOD]++; remainingWood--; }
                }
            }
        }
    }
}
// ----- PlantTask.apply -----
void Game::applyPlant(std::vector<Task*>& group) {
    for (auto& byCell : groupByCell(group)) {
        // distinct types in this cell group
        bool sameType=true; int t0 = byCell[0]->itemType;
        for (auto* t : byCell) if (t->itemType != t0) sameType=false;
        if (sameType) {
            for (auto* t : byCell) {
                Unit* u = findUnit(t->unitId);
                int type = t->itemType;
                if (u->carry[type]>0) u->carry[type]--; // decrementItem only if >0
                // (Java decrementItem checks >0; if 0 nothing happens — but parse would have failed for 0 seeds,
                //  so all surviving tasks have >0. Still safe.)
                int ux=u->x, uy=u->y;
                if (board.plantIdx[board.idx(ux,uy)] < 0) {
                    Plant np; np.x=ux; np.y=uy; np.type=type; np.size=0; np.resources=0; np.cooldown=0;
                    np.health = Board::initialHealth(type);
                    board.plants.push_back(np);
                    board.plantIdx[board.idx(ux,uy)] = (int)board.plants.size()-1;
                }
                t->applied=true;
            }
        } else {
            for (auto* t : byCell) { (void)t; addErr(byCell[0]->player /*irrelevant*/, false); }
        }
    }
}
void Game::applyPick(Task& t) {
    Unit* u = findUnit(t.unitId);
    int type = t.itemType;
    if (board.inv[t.player][type] > 0) {
        board.inv[t.player][type]--;
        u->carry[type]++;
        t.applied=true;
    } else addErr(t.player,false);
}
void Game::applyTrain(Task& t) {
    if (!canTrain(t.player, t.talents)) { addErr(t.player,false); return; }
    // cell blocked?
    int sx=board.shackX[t.player], sy=board.shackY[t.player];
    for (auto& u : board.units) if (u.x==sx && u.y==sy) { addErr(t.player,false); return; }
    // new Unit
    int costs[6]; getTrainingCosts(t.player, t.talents, costs);
    Unit nu;
    nu.id = board.unitIdCounter++;
    nu.player = t.player;
    nu.x=sx; nu.y=sy;
    nu.ms=t.talents[0]; nu.cc=t.talents[1]; nu.hp=t.talents[2]; nu.cp=t.talents[3];
    for (int i=0;i<6;i++) board.inv[t.player][i] -= costs[i];
    board.units.push_back(nu);
    t.applied=true;
}
void Game::applyDrop(Task& t) {
    Unit* u = findUnit(t.unitId);
    for (int i=0;i<6;i++){ board.inv[t.player][i] += u->carry[i]; u->carry[i]=0; }
    t.applied=true;
}
void Game::applyMine(Task& t) {
    Unit* u = findUnit(t.unitId);
    unitMine(*u);
    t.applied=true;
}

// ----------------------------------------------------------------------------
// hasStalled
// ----------------------------------------------------------------------------
static bool hasStalled(Board& b) {
    if (!b.plants.empty()) { b.noTreeCounter=0; return false; }
    b.noTreeCounter++;
    if (b.noTreeCounter == C::STALL_LIMIT) return true;
    bool stuck[2] = {true,true};
    for (auto& u : b.units) {
        if (u.total() > u.carry[C::IRON]) stuck[u.player]=false;
    }
    for (int p=0;p<2;p++) for (int i=0;i<=C::BANANA;i++) if (b.inv[p][i] > 0) stuck[p]=false;
    if (stuck[0] && stuck[1]) return true;
    if (stuck[0] && b.score[0] < b.score[1]) return true;
    if (stuck[1] && b.score[1] < b.score[0]) return true;
    return false;
}

// ----------------------------------------------------------------------------
// Game runner
// ----------------------------------------------------------------------------
static void parsePlayerCommands(Game& g, int player, const std::string& line, std::vector<Task>& outTasks) {
    // split on ';'
    std::vector<std::string> segs;
    {
        std::string cur;
        for (char c : line) { if (c==';'){ segs.push_back(cur); cur.clear(); } else cur+=c; }
        segs.push_back(cur);
    }
    // usedUnits
    std::vector<int> usedUnits;
    auto isUsed = [&](int id){ for (int u:usedUnits) if(u==id) return true; return false; };
    for (auto& seg : segs) {
        // MSG check (untrimmed, toUpper startsWith "MSG ")
        std::string su = Game::upper(seg);
        if (su.size()>=4 && su.compare(0,4,"MSG ")==0) {
            // setMessage(seg.substring(4).trim()) -- irrelevant for logic
            continue;
        }
        Task t; bool waitOrEmpty=false;
        int res = g.matchTask(player, seg, t, waitOrEmpty);
        if (waitOrEmpty) continue; // null, ignored
        if (res == 0 || res == -1) {
            // UNKNOWN_COMMAND, critical
            g.playerHasCriticalError[player] = true;
            continue;
        }
        // res == 1: a task object exists
        if (t.unitId != -1) {
            if (isUsed(t.unitId)) {
                // ALREADY_USED non-crit; return null
                continue;
            }
            usedUnits.push_back(t.unitId);
        }
        if (t.requiredLeague() > C::LEAGUE) {
            // NOT_AVAILABLE non-crit; return null
            continue;
        }
        // TaskManager: add only if !failedParsing
        if (!t.failedParsing) outTasks.push_back(t);
    }
}

int runGame(const std::string& bot1, const std::string& bot2, i64 seed, FILE* trace, int& s0, int& s1,
            const char* err1Path, const char* err2Path) {
    Game g; g.trace = trace;
    SHA1PRNG rng; rng.setSeedLong(seed);
    createMap(g.board, rng, g.bird);

    ChildProc proc[2];
#ifdef _WIN32
    auto mkErr = [&](const char* path)->HANDLE{
        SECURITY_ATTRIBUTES sa{}; sa.nLength=sizeof(sa); sa.bInheritHandle=TRUE;
        if (path) {
            HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            return h;
        }
        HANDLE devnull = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
        return devnull;
    };
    HANDLE e0 = mkErr(err1Path), e1 = mkErr(err2Path);
    if (!proc[0].start(bot1, e0)) { fprintf(stderr,"failed to start bot1\n"); return 1; }
    if (!proc[1].start(bot2, e1)) { fprintf(stderr,"failed to start bot2\n"); return 1; }
#else
    if (!proc[0].start(bot1, err1Path)) { fprintf(stderr,"failed to start bot1\n"); return 1; }
    if (!proc[1].start(bot2, err2Path)) { fprintf(stderr,"failed to start bot2\n"); return 1; }
#endif

    std::string out[2], inbuf;
    inbuf.reserve(8192);
    std::vector<Task> tasks; tasks.reserve(64);
    for (int turn=1; turn<=C::GAME_TURNS && !g.gameOver; turn++) {
        // send inputs + read outputs
        for (int p=0;p<2;p++) {
            if (!g.board.active[p]) continue;
            std::string& in = inbuf; in.clear();
            if (turn==1) buildInitialInput(g.board, p, in);
            buildTurnInput(g.board, p, in);
            proc[p].writeAll(in);
            std::string line;
            bool timedOut = false;
            const int responseMs =
                (turn == 1) ? C::PLAYER_FIRST_RESPONSE_MS : C::PLAYER_RESPONSE_MS;
            if (!proc[p].readLine(line, responseMs, timedOut)) {
                line.clear(); // EOF / error / timeout: treat as empty command line
                if (timedOut) g.killPlayer(p);
            }
            out[p] = line;
            if (trace) fprintf(trace, "T%d P%d IN_HASH=%016llx OUT=%s\n", turn, p,
                (unsigned long long)std::hash<std::string>{}(in), out[p].c_str());
        }
        // parse tasks
        tasks.clear();
        for (int p=0;p<2;p++) {
            if (!g.board.active[p]) continue;
            parsePlayerCommands(g, p, out[p], tasks);
        }
        // tick
        g.tick(tasks, turn);
        // process critical errors -> kill
        for (int p=0;p<2;p++) {
            if (g.playerHasCriticalError[p]) {
                g.killPlayer(p);
            }
        }
        // reset per-turn critical flags? In Java, popErrors clears errors each turn. Reset.
        g.playerHasCriticalError[0]=false; g.playerHasCriticalError[1]=false;
        if (hasStalled(g.board)) g.gameOver=true;
        if (trace) fprintf(trace, "  scores: %d %d  plants=%zu units=%zu\n", g.board.score[0], g.board.score[1], g.board.plants.size(), g.board.units.size());
    }

    proc[0].kill(); proc[1].kill();
#ifdef _WIN32
    if (e0) CloseHandle(e0);
    if (e1) CloseHandle(e1);
#endif
    s0 = g.board.score[0]; s1 = g.board.score[1];
    return 0;
}

// ----------------------------------------------------------------------------
// PRNG / mapgen self-test
// ----------------------------------------------------------------------------
static int selfTest() {
    {
        SHA1PRNG r; r.setSeedLong(12345);
        printf("nextInt(100) x10:");
        for (int i=0;i<10;i++) printf(" %d", r.nextIntBound(100));
        printf("\n");
    }
    {
        SHA1PRNG r; r.setSeedLong(12345);
        printf("nextInt() x5:");
        for (int i=0;i<5;i++) printf(" %d", r.nextInt());
        printf("\n");
    }
    {
        SHA1PRNG r; r.setSeedLong(12345);
        u8 b[20]; r.engineNextBytes(b,20);
        printf("nextBytes(20):");
        for (int i=0;i<20;i++) printf(" %d", b[i]);
        printf("\n");
    }
    {
        Board board; BirdView bird; SHA1PRNG rng; rng.setSeedLong(12345);
        createMap(board, rng, bird);
        std::string s; buildInitialInput(board, 0, s);
        printf("--- map seed 12345 ---\n%s", s.c_str());
        printf("inventory: %d %d %d %d %d %d\n", board.inv[0][0],board.inv[0][1],board.inv[0][2],board.inv[0][3],board.inv[0][4],board.inv[0][5]);
        printf("plants:\n");
        for (auto& p : board.plants) printf("  %s %d %d %d %d %d %d\n", ITEM_NAME[p.type],p.x,p.y,p.size,p.health,p.resources,p.cooldown);
        printf("bird.exists=%d\n", (int)bird.exists);
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string bot1, bot2; i64 seed = 0; const char* tracePath=nullptr; bool test=false;
    const char* err1=nullptr; const char* err2=nullptr;
    for (int i=1;i<argc;i++) {
        std::string a = argv[i];
        if (a=="-p1" && i+1<argc) bot1 = argv[++i];
        else if (a=="-p2" && i+1<argc) bot2 = argv[++i];
        else if (a=="-seed" && i+1<argc) seed = atoll(argv[++i]);
        else if (a=="-l" && i+1<argc) tracePath = argv[++i];
        else if (a=="-err1" && i+1<argc) err1 = argv[++i];
        else if (a=="-err2" && i+1<argc) err2 = argv[++i];
        else if (a=="-test") test=true;
    }
    if (test) return selfTest();
    // -scanbird N : print seeds in [1,N] that place a bird (for verification of the rare bird path)
    for (int i=1;i<argc;i++) if (std::string(argv[i])=="-scanbird" && i+1<argc) {
        long N = atol(argv[i+1]);
        for (long s=1;s<=N;s++) { Board b; BirdView bird; SHA1PRNG r; r.setSeedLong(s); createMap(b,r,bird); if (bird.exists) printf("%ld\n", s); }
        return 0;
    }
    if (bot1.empty() || bot2.empty()) { fprintf(stderr,"usage: referee -p1 <cmd> -p2 <cmd> -seed N [-l file] [-err1 f] [-err2 f] [-test]\n"); return 2; }
    FILE* trace = nullptr;
    if (tracePath) trace = fopen(tracePath, "w");
    int s0=0,s1=0;
    int rc = runGame(bot1, bot2, seed, trace, s0, s1, err1, err2);
    if (trace) fclose(trace);
    if (rc) return rc;
    printf("%d\n%d\n", s0, s1);
    return 0;
}
