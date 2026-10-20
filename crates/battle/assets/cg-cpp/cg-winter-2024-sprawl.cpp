// Winter Challenge 2024 "Cellularena" - headless C++ referee, port of
// https://github.com/CGjupoulton/WinterChallenge2024-Cellularena (commit 3853e87, which includes
// 2778534 "fixed referee error messages"), CodinGame game engine 4.5.0, 2 players, league 19 (the
// CLI sets league.level=19, so the full game with the random map of GridMaker.initGrid is played).
// Deterministic; reproduces the same game as the reference jar for a given -seed.
//
// Usage:
//   referee -p1 "<bot1 cmd>" -p2 "<bot2 cmd>" -seed <N> [-l <file>]
// Prints "<p0score>\n<p1score>\nseed=<N>\n" to stdout like the jar, or "null\nnull\n" (exit code 0)
// when the Java referee would have crashed (see below). Exit code 1 without output when the jar's
// command line interface fails (bad -seed, a bot that cannot be started).
// -l writes a trace: one "T<turn> P<player> OUT <JSON array of lines>" or "T<turn> P<player> TIMEOUT"
// line per player execution, "V <frame> <UTF-16 length> <UTF-8 bytes>" per view frame, "S <JSON>" per
// game summary, "TT <JSON>" per tooltip block and a final "END OK|<Java exception>" line.
//
// The Java referee is followed very closely. Notable Java behaviours reproduced here:
//   - gameManager.getRandom() is SecureRandom("SHA1PRNG") seeded with the seed, consumed by
//     RandomGenerator.nextInt(origin, bound), nextDouble(bound), Random.nextInt(bound), nextBoolean
//     and Collections.shuffle. (-seed 0 is ignored by SecureRandom.setSeed: the jar then plays a
//     random map; we can't reproduce that.)
//   - java.util.HashMap iteration orders: the order of the growthsByCoord map (filled with compute(),
//     which inserts at the head of a bin and resizes lazily) decides the ids of the new organs;
//     organById/dyingIds orders decide the timing of view events. JHashMap models HashMap exactly,
//     including treeifyBin resizes and red-black tree bins.
//   - java.util.PriorityQueue tie order in the A* used to grow towards far targets.
//   - The engine 4.5.0 runner and referee are two threads talking through pipes, modelled here as a
//     line queue (the referee's java.util.Scanner, which also ends lines at U+0085, U+2028 and
//     U+2029) and a command queue (what the runner reads, each command limited to 30000 bytes, or
//     100000 in the first round). A player line containing such a separator desynchronises the
//     protocol and crashes the Java referee; an invalid command echoed into the game summary or a
//     tooltip can exceed the 30000 bytes; the view data can exceed its 1 MB total quota. In all these
//     cases the jar reports no scores ("null"), and so do we.
//   - Agent#getOutput: '\r' and '\n' end lines ("\r\n" counts once, also across turns), 10000 bytes
//     per turn at most, decoded as UTF-8 with Java's replacement rules; 1000 ms for the first
//     turn of each player, 50 ms afterwards. Input is written to the bots by a separate thread like
//     the engine's AsynchronousWriter. (The jar starts the bots, then initialises the referee before
//     it sends the first input, which gives slow-starting bots a head start of 0.1-1 s on their first
//     turn; here the 1000 ms count from the first input, like the jar counts them, without that
//     extra time: emulating it would change the result of bots that answer in 1.0-1.3 s.)
//   - Command regexes, String.trim, Integer.valueOf overflow, message truncation in UTF-16 units,
//     the per-turn (800 chars) and total game summary quotas, Gson string escaping.
//   - Not reproducible: the order of the HashSet<Harvest> in doHarvests (identity hash codes);
//     it only changes the order of harvest events in the view, not its size. The jar's wall clock:
//     it reads bot output one byte at a time, so a bot answering ~10 KB may time out in the jar
//     (50 ms) on a busy machine and not here. Metadata (game statistics) are not computed.

#include "common/engine.h"

#include <deque>
#include <map>
#include <set>

typedef std::u16string ustr;

// ----------------------------------------------------------------------------
// Java semantics (java.h has the rest)
// ----------------------------------------------------------------------------
namespace java {

inline ustr fromAscii(const std::string& s) { return ascii(s); }

// --- java.util.HashMap iteration order model ---
// Keys are ints with a given hashCode(); values live elsewhere. Mirrors HashMap.putVal (put / HashSet.add),
// HashMap.compute (inserting a new key), HashMap.remove, resize, treeifyBin and the TreeNode code
// (red-black bins keep a "next" order that iteration follows). Keys must have distinct hash codes,
// which holds for Integer and for in-grid Coord keys, so TreeNode tie-breaking never happens.
struct JHashMap {
    struct N {
        i32 hash, key;
        int next = -1, parent = -1, left = -1, right = -1, prev = -1;
        bool red = false, tree = false;
    };
    std::vector<N> pool;
    std::vector<int> tab;
    int size = 0, threshold = 0;

    static i32 spread(i32 h) { return h ^ (i32)((u32)h >> 16); }
    int newNode(i32 hash, i32 key, int next, bool tree) {
        N n;
        n.hash = hash;
        n.key = key;
        n.next = next;
        n.tree = tree;
        pool.push_back(n);
        return (int)pool.size() - 1;
    }
    N& P(int i) { return pool[i]; }

    void resize() {
        int oldCap = (int)tab.size();
        int oldThr = threshold;
        int newCap, newThr = 0;
        if (oldCap > 0) {
            newCap = oldCap << 1;
            if (oldCap >= 16) newThr = oldThr << 1;
        } else {
            newCap = 16;
            newThr = 12;
        }
        // (the maps here use the default initial capacity, so newThr is never 0)
        threshold = newThr;
        std::vector<int> oldTab;
        oldTab.swap(tab);
        tab.assign(newCap, -1);
        for (int j = 0; j < oldCap; ++j) {
            int e = oldTab[j];
            if (e < 0) continue;
            if (P(e).next < 0) {
                tab[P(e).hash & (newCap - 1)] = e;
            } else if (P(e).tree) {
                split(e, j, oldCap);
            } else {
                int loHead = -1, loTail = -1, hiHead = -1, hiTail = -1, next;
                do {
                    next = P(e).next;
                    if ((P(e).hash & oldCap) == 0) {
                        if (loTail < 0) loHead = e;
                        else P(loTail).next = e;
                        loTail = e;
                    } else {
                        if (hiTail < 0) hiHead = e;
                        else P(hiTail).next = e;
                        hiTail = e;
                    }
                } while ((e = next) >= 0);
                if (loTail >= 0) {
                    P(loTail).next = -1;
                    tab[j] = loHead;
                }
                if (hiTail >= 0) {
                    P(hiTail).next = -1;
                    tab[j + oldCap] = hiHead;
                }
            }
        }
    }

    int root(int r) {
        for (;;) {
            int p = P(r).parent;
            if (p < 0) return r;
            r = p;
        }
    }
    void moveRootToFront(int rt) {
        int n = (int)tab.size();
        if (rt < 0 || n == 0) return;
        int index = (n - 1) & P(rt).hash;
        int first = tab[index];
        if (rt != first) {
            tab[index] = rt;
            int rp = P(rt).prev;
            int rn = P(rt).next;
            if (rn >= 0) P(rn).prev = rp;
            if (rp >= 0) P(rp).next = rn;
            if (first >= 0) P(first).prev = rt;
            P(rt).next = first;
            P(rt).prev = -1;
        }
    }
    int find(int p, i32 h) {
        while (p >= 0) {
            i32 ph = P(p).hash;
            if (ph > h) p = P(p).left;
            else if (ph < h) p = P(p).right;
            else return p;
        }
        return -1;
    }
    int rotateLeft(int rt, int p) {
        int r, pp, rl;
        if (p >= 0 && (r = P(p).right) >= 0) {
            if ((rl = P(p).right = P(r).left) >= 0) P(rl).parent = p;
            if ((pp = P(r).parent = P(p).parent) < 0) {
                rt = r;
                P(r).red = false;
            } else if (P(pp).left == p) P(pp).left = r;
            else P(pp).right = r;
            P(r).left = p;
            P(p).parent = r;
        }
        return rt;
    }
    int rotateRight(int rt, int p) {
        int l, pp, lr;
        if (p >= 0 && (l = P(p).left) >= 0) {
            if ((lr = P(p).left = P(l).right) >= 0) P(lr).parent = p;
            if ((pp = P(l).parent = P(p).parent) < 0) {
                rt = l;
                P(l).red = false;
            } else if (P(pp).right == p) P(pp).right = l;
            else P(pp).left = l;
            P(l).right = p;
            P(p).parent = l;
        }
        return rt;
    }
    int balanceInsertion(int rt, int x) {
        P(x).red = true;
        for (int xp, xpp, xppl, xppr;;) {
            if ((xp = P(x).parent) < 0) {
                P(x).red = false;
                return x;
            } else if (!P(xp).red || (xpp = P(xp).parent) < 0)
                return rt;
            if (xp == (xppl = P(xpp).left)) {
                if ((xppr = P(xpp).right) >= 0 && P(xppr).red) {
                    P(xppr).red = false;
                    P(xp).red = false;
                    P(xpp).red = true;
                    x = xpp;
                } else {
                    if (x == P(xp).right) {
                        rt = rotateLeft(rt, x = xp);
                        xpp = (xp = P(x).parent) < 0 ? -1 : P(xp).parent;
                    }
                    if (xp >= 0) {
                        P(xp).red = false;
                        if (xpp >= 0) {
                            P(xpp).red = true;
                            rt = rotateRight(rt, xpp);
                        }
                    }
                }
            } else {
                if (xppl >= 0 && P(xppl).red) {
                    P(xppl).red = false;
                    P(xp).red = false;
                    P(xpp).red = true;
                    x = xpp;
                } else {
                    if (x == P(xp).left) {
                        rt = rotateRight(rt, x = xp);
                        xpp = (xp = P(x).parent) < 0 ? -1 : P(xp).parent;
                    }
                    if (xp >= 0) {
                        P(xp).red = false;
                        if (xpp >= 0) {
                            P(xpp).red = true;
                            rt = rotateLeft(rt, xpp);
                        }
                    }
                }
            }
        }
    }
    int balanceDeletion(int rt, int x) {
        for (int xp, xpl, xpr;;) {
            if (x < 0 || x == rt) return rt;
            else if ((xp = P(x).parent) < 0) {
                P(x).red = false;
                return x;
            } else if (P(x).red) {
                P(x).red = false;
                return rt;
            } else if ((xpl = P(xp).left) == x) {
                if ((xpr = P(xp).right) >= 0 && P(xpr).red) {
                    P(xpr).red = false;
                    P(xp).red = true;
                    rt = rotateLeft(rt, xp);
                    xpr = (xp = P(x).parent) < 0 ? -1 : P(xp).right;
                }
                if (xpr < 0) x = xp;
                else {
                    int sl = P(xpr).left, sr = P(xpr).right;
                    if ((sr < 0 || !P(sr).red) && (sl < 0 || !P(sl).red)) {
                        P(xpr).red = true;
                        x = xp;
                    } else {
                        if (sr < 0 || !P(sr).red) {
                            if (sl >= 0) P(sl).red = false;
                            P(xpr).red = true;
                            rt = rotateRight(rt, xpr);
                            xpr = (xp = P(x).parent) < 0 ? -1 : P(xp).right;
                        }
                        if (xpr >= 0) {
                            P(xpr).red = (xp < 0) ? false : P(xp).red;
                            if ((sr = P(xpr).right) >= 0) P(sr).red = false;
                        }
                        if (xp >= 0) {
                            P(xp).red = false;
                            rt = rotateLeft(rt, xp);
                        }
                        x = rt;
                    }
                }
            } else { // symmetric
                if (xpl >= 0 && P(xpl).red) {
                    P(xpl).red = false;
                    P(xp).red = true;
                    rt = rotateRight(rt, xp);
                    xpl = (xp = P(x).parent) < 0 ? -1 : P(xp).left;
                }
                if (xpl < 0) x = xp;
                else {
                    int sl = P(xpl).left, sr = P(xpl).right;
                    if ((sl < 0 || !P(sl).red) && (sr < 0 || !P(sr).red)) {
                        P(xpl).red = true;
                        x = xp;
                    } else {
                        if (sl < 0 || !P(sl).red) {
                            if (sr >= 0) P(sr).red = false;
                            P(xpl).red = true;
                            rt = rotateLeft(rt, xpl);
                            xpl = (xp = P(x).parent) < 0 ? -1 : P(xp).left;
                        }
                        if (xpl >= 0) {
                            P(xpl).red = (xp < 0) ? false : P(xp).red;
                            if ((sl = P(xpl).left) >= 0) P(sl).red = false;
                        }
                        if (xp >= 0) {
                            P(xp).red = false;
                            rt = rotateRight(rt, xp);
                        }
                        x = rt;
                    }
                }
            }
        }
    }
    void treeify(int head) {
        int rt = -1;
        for (int x = head, next; x >= 0; x = next) {
            next = P(x).next;
            P(x).left = P(x).right = -1;
            if (rt < 0) {
                P(x).parent = -1;
                P(x).red = false;
                rt = x;
            } else {
                i32 h = P(x).hash;
                for (int p = rt;;) {
                    i32 ph = P(p).hash;
                    int dir = ph > h ? -1 : 1; // hashes are distinct
                    int xp = p;
                    if ((p = (dir <= 0) ? P(p).left : P(p).right) < 0) {
                        P(x).parent = xp;
                        if (dir <= 0) P(xp).left = x;
                        else P(xp).right = x;
                        rt = balanceInsertion(rt, x);
                        break;
                    }
                }
            }
        }
        moveRootToFront(rt);
    }
    int untreeify(int head) {
        for (int q = head; q >= 0; q = P(q).next) {
            P(q).tree = false;
            P(q).prev = P(q).parent = P(q).left = P(q).right = -1;
            P(q).red = false;
        }
        return head;
    }
    void split(int b, int index, int bit) {
        int loHead = -1, loTail = -1, hiHead = -1, hiTail = -1;
        int lc = 0, hc = 0;
        for (int e = b, next; e >= 0; e = next) {
            next = P(e).next;
            P(e).next = -1;
            if ((P(e).hash & bit) == 0) {
                if ((P(e).prev = loTail) < 0) loHead = e;
                else P(loTail).next = e;
                loTail = e;
                ++lc;
            } else {
                if ((P(e).prev = hiTail) < 0) hiHead = e;
                else P(hiTail).next = e;
                hiTail = e;
                ++hc;
            }
        }
        if (loHead >= 0) {
            if (lc <= 6) tab[index] = untreeify(loHead);
            else {
                tab[index] = loHead;
                if (hiHead >= 0) treeify(loHead);
            }
        }
        if (hiHead >= 0) {
            if (hc <= 6) tab[index + bit] = untreeify(hiHead);
            else {
                tab[index + bit] = hiHead;
                if (loHead >= 0) treeify(hiHead);
            }
        }
    }
    void treeifyBin(i32 hash) {
        int n = (int)tab.size();
        if (n < 64) {
            resize();
            return;
        }
        int index = (n - 1) & hash;
        std::vector<int> chain;
        for (int e = tab[index]; e >= 0; e = P(e).next) chain.push_back(e);
        if (chain.empty()) return;
        for (size_t k = 0; k < chain.size(); k++) {
            N& c = P(chain[k]);
            c.tree = true;
            c.prev = k ? chain[k - 1] : -1;
            c.next = k + 1 < chain.size() ? chain[k + 1] : -1;
        }
        tab[index] = chain[0];
        treeify(chain[0]);
    }
    // returns the existing node, or -1 after inserting
    int putTreeVal(int first, i32 h, i32 k) {
        int rt = P(first).parent >= 0 ? root(first) : first;
        for (int p = rt;;) {
            int dir;
            i32 ph = P(p).hash;
            if (ph > h) dir = -1;
            else if (ph < h) dir = 1;
            else return p;
            int xp = p;
            if ((p = (dir <= 0) ? P(p).left : P(p).right) < 0) {
                int xpn = P(xp).next;
                int x = newNode(h, k, xpn, true);
                if (dir <= 0) P(xp).left = x;
                else P(xp).right = x;
                P(xp).next = x;
                P(x).parent = P(x).prev = xp;
                if (xpn >= 0) P(xpn).prev = x;
                moveRootToFront(balanceInsertion(rt, x));
                return -1;
            }
        }
    }
    void removeTreeNode(int node) {
        int n = (int)tab.size();
        int index = (n - 1) & P(node).hash;
        int first = tab[index], rt = first, rl;
        int succ = P(node).next, pred = P(node).prev;
        if (pred < 0) tab[index] = first = succ;
        else P(pred).next = succ;
        if (succ >= 0) P(succ).prev = pred;
        if (first < 0) return;
        if (P(rt).parent >= 0) rt = root(rt);
        if (rt < 0 || (P(rt).right < 0 || (rl = P(rt).left) < 0 || P(rl).left < 0)) {
            tab[index] = untreeify(first); // too small
            return;
        }
        int p = node, pl = P(node).left, pr = P(node).right, replacement;
        if (pl >= 0 && pr >= 0) {
            int s = pr, sl;
            while ((sl = P(s).left) >= 0) s = sl; // find successor
            bool c = P(s).red;
            P(s).red = P(p).red;
            P(p).red = c; // swap colors
            int sr = P(s).right;
            int pp = P(p).parent;
            if (s == pr) { // p was s's direct parent
                P(p).parent = s;
                P(s).right = p;
            } else {
                int sp = P(s).parent;
                if ((P(p).parent = sp) >= 0) {
                    if (s == P(sp).left) P(sp).left = p;
                    else P(sp).right = p;
                }
                if ((P(s).right = pr) >= 0) P(pr).parent = s;
            }
            P(p).left = -1;
            if ((P(p).right = sr) >= 0) P(sr).parent = p;
            if ((P(s).left = pl) >= 0) P(pl).parent = s;
            if ((P(s).parent = pp) < 0) rt = s;
            else if (p == P(pp).left) P(pp).left = s;
            else P(pp).right = s;
            if (sr >= 0) replacement = sr;
            else replacement = p;
        } else if (pl >= 0) replacement = pl;
        else if (pr >= 0) replacement = pr;
        else replacement = p;
        if (replacement != p) {
            int pp = P(replacement).parent = P(p).parent;
            if (pp < 0) {
                rt = replacement;
                P(replacement).red = false;
            } else if (p == P(pp).left) P(pp).left = replacement;
            else P(pp).right = replacement;
            P(p).left = P(p).right = P(p).parent = -1;
        }
        int r = P(p).red ? rt : balanceDeletion(rt, replacement);
        if (replacement == p) { // detach
            int pp = P(p).parent;
            P(p).parent = -1;
            if (pp >= 0) {
                if (p == P(pp).left) P(pp).left = -1;
                else if (p == P(pp).right) P(pp).right = -1;
            }
        }
        moveRootToFront(r);
    }

    // HashMap.put / HashSet.add
    void put(i32 key, i32 keyHash) {
        i32 hash = spread(keyHash);
        if (tab.empty()) resize();
        int n = (int)tab.size();
        int i = (n - 1) & hash;
        int p = tab[i];
        if (p < 0) {
            tab[i] = newNode(hash, key, -1, false);
        } else {
            int e;
            if (P(p).hash == hash && P(p).key == key) e = p;
            else if (P(p).tree) e = putTreeVal(p, hash, key);
            else {
                for (int binCount = 0;; ++binCount) {
                    if ((e = P(p).next) < 0) {
                        P(p).next = newNode(hash, key, -1, false);
                        if (binCount >= 7) treeifyBin(hash);
                        break;
                    }
                    if (P(e).hash == hash && P(e).key == key) break;
                    p = e;
                }
            }
            if (e >= 0) return; // existing mapping
        }
        if (++size > threshold) resize();
    }
    // HashMap.compute(key, f) where f returns a non-null value
    void compute(i32 key, i32 keyHash) {
        i32 hash = spread(keyHash);
        if (size > threshold || tab.empty()) resize();
        int n = (int)tab.size();
        int i = (n - 1) & hash;
        int first = tab[i];
        int binCount = 0, t = -1, old = -1;
        if (first >= 0) {
            if (P(first).tree) {
                t = first;
                old = find(P(first).parent >= 0 ? root(first) : first, hash);
            } else {
                int e = first;
                do {
                    if (P(e).hash == hash && P(e).key == key) {
                        old = e;
                        break;
                    }
                    ++binCount;
                } while ((e = P(e).next) >= 0);
            }
        }
        if (old >= 0) return;
        if (t >= 0) putTreeVal(t, hash, key);
        else {
            tab[i] = newNode(hash, key, first, false);
            if (binCount >= 7) treeifyBin(hash);
        }
        ++size;
    }
    // HashMap.remove
    void remove(i32 key, i32 keyHash) {
        i32 hash = spread(keyHash);
        int n = (int)tab.size();
        if (n == 0) return;
        int index = (n - 1) & hash;
        int p = tab[index];
        if (p < 0) return;
        int node = -1, e;
        if (P(p).hash == hash && P(p).key == key) node = p;
        else if ((e = P(p).next) >= 0) {
            if (P(p).tree) node = find(P(p).parent >= 0 ? root(p) : p, hash);
            else {
                do {
                    if (P(e).hash == hash && P(e).key == key) {
                        node = e;
                        break;
                    }
                    p = e;
                } while ((e = P(e).next) >= 0);
            }
        }
        if (node < 0) return;
        if (P(node).tree) removeTreeNode(node);
        else if (node == p) tab[index] = P(node).next;
        else P(p).next = P(node).next;
        --size;
    }
    // keys in iteration order
    void keys(std::vector<i32>& out) {
        out.clear();
        for (int b : tab)
            for (int e = b; e >= 0; e = P(e).next) out.push_back(P(e).key);
    }
};

// --- java.util.PriorityQueue (binary heap, siftUp/siftDown with a comparator) ---
template <class T, class Less>
struct PriorityQueue {
    std::vector<T> es;
    Less less; // less(a, b) <=> compare(a, b) < 0
    explicit PriorityQueue(Less l) : less(l) {}
    int size() const { return (int)es.size(); }
    void offer(const T& x) {
        es.push_back(x);
        int k = (int)es.size() - 1;
        while (k > 0) {
            int parent = (k - 1) >> 1;
            T e = es[parent];
            if (!less(x, e)) break; // compare(x, e) >= 0
            es[k] = e;
            k = parent;
        }
        es[k] = x;
    }
    T poll() {
        T result = es[0];
        int n = (int)es.size() - 1;
        T x = es[n];
        es.pop_back();
        if (n > 0) {
            int k = 0;
            int half = n >> 1;
            while (k < half) {
                int child = (k << 1) + 1;
                T c = es[child];
                int right = child + 1;
                if (right < n && less(es[right], c)) c = es[child = right]; // compare(c, right) > 0
                if (!less(c, x)) break;                                     // compare(x, c) <= 0
                es[k] = c;
                k = child;
            }
            es[k] = x;
        }
        return result;
    }
};

} // namespace java

using java::str;
using java::fromAscii;

// ----------------------------------------------------------------------------
// Grid (grid/Coord.java, Direction.java, Tile.java, Grid.java, Protein.java)
// ----------------------------------------------------------------------------
namespace C {
const int PROTEIN_COUNT = 4;
const int MIN_STARTING_PROTEINS = 3;
const int MAX_STARTING_PROTEINS = 10;
const int PROTEIN_PER_ABSORB = 3;
const int GRID_W_RATIO = 2;
const int MAX_SPAWN_DIST_FROM_CORNER = 3;
// Animation
const int TENTH = 100;
const int THIRD = 300;
const int WHOLE = 1000;
} // namespace C

struct Coord {
    i32 x = 0, y = 0;
    Coord() {}
    Coord(i32 x_, i32 y_) : x(x_), y(y_) {}
    Coord add(i32 dx, i32 dy) const { return Coord((i32)((u32)x + (u32)dx), (i32)((u32)y + (u32)dy)); }
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    i32 hashCode() const { return (i32)((u32)31 * ((u32)31 * 1u + (u32)x) + (u32)y); }
    i32 manhattanTo(const Coord& o) const {
        i32 dx = (i32)((u32)o.x - (u32)x), dy = (i32)((u32)o.y - (u32)y);
        return (i32)((u32)(dx < 0 ? (i32)(0u - (u32)dx) : dx) + (u32)(dy < 0 ? (i32)(0u - (u32)dy) : dy));
    }
    std::string toString() const { return "(" + std::to_string(x) + ", " + std::to_string(y) + ")"; }
    std::string toIntString() const { return std::to_string(x) + " " + std::to_string(y); }
};

enum Dir { NORTH, EAST, SOUTH, WEST, UNSET };
static const i32 DIR_X[5] = {0, 1, 0, -1, 0};
static const i32 DIR_Y[5] = {-1, 0, 1, 0, 0};
static const char* DIR_ALIAS[5] = {"N", "E", "S", "W", "X"};
static const int ADJACENCY[4] = {NORTH, EAST, SOUTH, WEST};

enum OrganType { ROOT, BASIC, TENTACLE, HARVESTER, SPORER, ORGAN_TYPES };
static const char* ORGAN_NAME[ORGAN_TYPES] = {"ROOT", "BASIC", "TENTACLE", "HARVESTER", "SPORER"};
static const int ORGAN_COST[ORGAN_TYPES][4] = {{1, 1, 1, 1}, {1, 0, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 1}, {0, 1, 0, 1}};
static const char* PROTEIN_NAME[4] = {"A", "B", "C", "D"};

struct Player;

struct Organ {
    int id;
    int type;
    Player* owner;
    Organ* parent = nullptr;
    std::vector<Organ*> children;
    int direction;
    Coord pos;
    int rootId;
    Coord getFacedCoord() const { return pos.add(DIR_X[direction], DIR_Y[direction]); }
    bool isHarvester() const { return type == HARVESTER; }
    bool isTentacle() const { return type == TENTACLE; }
    bool isNucleus() const { return type == ROOT; }
    int getParentId() const { return parent == nullptr ? 0 : parent->id; }
};

struct Tile {
    int protein = -1; // -1: null
    bool obstacle = false;
    Organ* organ = nullptr;
    bool valid = true;
    void setObstacle() {
        obstacle = true;
        protein = -1;
    }
    void setProtein(int idx) {
        obstacle = false;
        protein = idx;
    }
    void placeOrgan(Organ* o) {
        organ = o;
        obstacle = false;
        protein = -1;
    }
    void unsetObstacle() { obstacle = false; }
    void clear() {
        organ = nullptr;
        obstacle = false;
        protein = -1;
    }
    bool hasOrgan() const { return organ != nullptr; }
    bool hasProtein() const { return protein >= 0; }
    std::string getProteinString() const { return protein < 0 ? "X" : PROTEIN_NAME[protein]; }
    bool hasTentacle(const Player* owner, const Coord& facing) const {
        return organ != nullptr && organ->owner == owner && organ->isTentacle() && organ->getFacedCoord() == facing;
    }
};

struct Grid {
    int width = 0, height = 0;
    bool ySymetry = false;
    std::vector<Tile> cells; // LinkedHashMap order: y then x
    Coord spawn;
    Tile noTile; // Tile.NO_TILE, never modified

    void init(int w, int h, bool ySym) {
        width = w;
        height = h;
        ySymetry = ySym;
        spawn = Coord(0, 0);
        cells.assign(w * h, Tile());
        noTile = Tile();
        noTile.valid = false;
    }
    bool inside(const Coord& c) const { return c.x >= 0 && c.x < width && c.y >= 0 && c.y < height; }
    int index(const Coord& c) const { return c.y * width + c.x; }
    Tile& get(const Coord& c) { return inside(c) ? cells[index(c)] : noTile; }
    // neighbours inside the grid, in ADJACENCY order (N, E, S, W)
    int getNeighbours(const Coord& pos, Coord out[4]) const {
        int n = 0;
        for (int d : ADJACENCY) {
            Coord c = pos.add(DIR_X[d], DIR_Y[d]);
            if (inside(c)) out[n++] = c;
        }
        return n;
    }
    Coord coordAt(int i) const { return Coord(i % width, i / width); }
    Coord opposite(const Coord& c) const { return Coord(width - c.x - 1, ySymetry ? (height - c.y - 1) : c.y); }
};

// ----------------------------------------------------------------------------
// GridMaker.java
// ----------------------------------------------------------------------------
namespace GridMaker {

// detectIslands: island id per cell (-1 when the cell is in no island); returns the number of islands
static int detectIslands(Grid& grid, std::vector<int>& island) {
    int n = grid.width * grid.height;
    island.assign(n, -1);
    std::vector<char> computed(n, 0);
    int count = 0;
    std::deque<int> fifo;
    for (int p = 0; p < n; p++) {
        if (grid.cells[p].obstacle) continue;
        if (!computed[p]) {
            fifo.push_back(p);
            computed[p] = 1;
            while (!fifo.empty()) {
                int e = fifo.front();
                fifo.pop_front();
                Coord ec = grid.coordAt(e);
                for (int d : ADJACENCY) {
                    Coord nc = ec.add(DIR_X[d], DIR_Y[d]);
                    Tile& cell = grid.get(nc);
                    if (cell.valid && !cell.obstacle && !computed[grid.index(nc)]) {
                        fifo.push_back(grid.index(nc));
                        computed[grid.index(nc)] = 1;
                    }
                }
                island[e] = count;
            }
            count++;
        }
    }
    return count;
}

static void removeFirst(std::vector<Coord>& list, const Coord& c) {
    auto it = std::find(list.begin(), list.end(), c);
    if (it != list.end()) list.erase(it);
}

// The islands are disjoint sets, so only membership and their number matter. A bridge merges the
// islands it touches into a new one which, like in Java, contains neither the bridge nor its opposite.
static bool closeIslandGap(Grid& grid, std::vector<Coord>& wallCoords, std::vector<int>& island, int& islandCount, int& nextIslandId) {
    std::vector<int> connecting;
    bool found = false;
    Coord bridge;
    for (const Coord& coord : wallCoords) {
        Coord neighs[4];
        int nn = grid.getNeighbours(coord, neighs);
        connecting.clear();
        for (int k = 0; k < nn; k++) {
            int id = island[grid.index(neighs[k])];
            if (id >= 0 && std::find(connecting.begin(), connecting.end(), id) == connecting.end()) connecting.push_back(id);
        }
        if (connecting.size() > 1) {
            bridge = coord;
            found = true;
            break;
        }
    }
    if (!found) return false;
    Coord coord = bridge;
    Coord opposite = grid.opposite(coord);
    grid.get(coord).unsetObstacle();
    grid.get(opposite).unsetObstacle();
    removeFirst(wallCoords, coord);
    removeFirst(wallCoords, opposite);
    int merged = nextIslandId++;
    for (int& id : island)
        if (id >= 0 && std::find(connecting.begin(), connecting.end(), id) != connecting.end()) id = merged;
    islandCount = islandCount - (int)connecting.size() + 1;
    return true;
}

static bool findWallAdjecentToFreeSpace(std::vector<Coord>& wallCoords, Grid& grid, Coord& out) {
    for (const Coord& c : wallCoords) {
        Coord neighs[4];
        int nn = grid.getNeighbours(c, neighs);
        for (int k = 0; k < nn; k++) {
            if (!grid.get(neighs[k]).obstacle) {
                out = c;
                return true;
            }
        }
    }
    return false;
}

static void fixIslands(Grid& grid, std::vector<Coord>& wallCoords, java::SHA1PRNG& random) {
    // Collections.shuffle(wallCoords, random)
    for (int i = (int)wallCoords.size(); i > 1; i--) std::swap(wallCoords[i - 1], wallCoords[random.nextInt((i32)i)]);
    std::vector<int> island;
    int islandCount = detectIslands(grid, island);
    int nextIslandId = islandCount;
    while (islandCount > 1) {
        bool closed = closeIslandGap(grid, wallCoords, island, islandCount, nextIslandId);
        if (!closed) {
            Coord coord;
            if (findWallAdjecentToFreeSpace(wallCoords, grid, coord)) {
                Coord opposite = grid.opposite(coord);
                grid.get(coord).unsetObstacle();
                grid.get(opposite).unsetObstacle();
                removeFirst(wallCoords, coord);
                removeFirst(wallCoords, opposite);
            }
            islandCount = detectIslands(grid, island);
            nextIslandId = islandCount;
        }
    }
}

static void initGrid(Grid& grid, java::SHA1PRNG& random) {
    int h = random.nextInt(8, 13);
    int w = h * C::GRID_W_RATIO;

    bool ySym = random.nextBoolean() || random.nextBoolean();
    grid.init(w, h, ySym);

    double maxProteinRatio = 0.25;
    double maxObstacleRatio = 0.5;

    int proteinCount = java::d2i(random.nextDouble(maxProteinRatio) * h * w);
    int obstacleCount = java::d2i(random.nextDouble(maxObstacleRatio) * h * w);

    std::vector<Coord> allCoords;
    for (int i = 0; i < w * h; i++) allCoords.push_back(grid.coordAt(i));

    // Obstacles (HashSet<Coord>: only its size and contents matter)
    std::vector<char> obstacleSet(w * h, 0);
    int obstacleSetSize = 0;
    while (obstacleSetSize < obstacleCount && !allCoords.empty()) {
        int randIndex = random.nextInt((i32)allCoords.size());
        Coord randCoord = allCoords[randIndex];
        allCoords.erase(allCoords.begin() + randIndex);
        Coord symCoord = grid.opposite(randCoord);
        if (!obstacleSet[grid.index(randCoord)]) obstacleSet[grid.index(randCoord)] = 1, obstacleSetSize++;
        if (!obstacleSet[grid.index(symCoord)]) obstacleSet[grid.index(symCoord)] = 1, obstacleSetSize++;
    }
    for (int i = 0; i < w * h; i++)
        if (obstacleSet[i]) grid.cells[i].setObstacle();

    // Proteins (allCoords never runs out here: fewer than w*h/4 + 8 picks are left)
    int proteinIdx = 0;
    std::vector<char> proteinSet(w * h, 0);
    int proteinSetSize = 0;
    while ((proteinSetSize < proteinCount && !allCoords.empty()) || proteinIdx < C::PROTEIN_COUNT * 2) {
        int randIndex = random.nextInt((i32)allCoords.size());
        Coord randCoord = allCoords[randIndex];
        allCoords.erase(allCoords.begin() + randIndex);
        Coord symCoord = grid.opposite(randCoord);
        if (!proteinSet[grid.index(randCoord)]) proteinSet[grid.index(randCoord)] = 1, proteinSetSize++;
        if (!proteinSet[grid.index(symCoord)]) proteinSet[grid.index(symCoord)] = 1, proteinSetSize++;

        grid.get(randCoord).setProtein(proteinIdx % C::PROTEIN_COUNT);
        grid.get(symCoord).setProtein(proteinIdx % C::PROTEIN_COUNT);
        proteinIdx++;
    }

    // Spawns
    int sx = random.nextInt(0, C::MAX_SPAWN_DIST_FROM_CORNER + 1);
    int sy = random.nextInt(0, C::MAX_SPAWN_DIST_FROM_CORNER + 1);
    Coord spawn(sx, sy);
    grid.get(spawn).clear();
    grid.get(grid.opposite(spawn)).clear();

    std::vector<Coord> wallCoords;
    for (int i = 0; i < w * h; i++)
        if (grid.cells[i].obstacle) wallCoords.push_back(grid.coordAt(i));

    fixIslands(grid, wallCoords, random);

    grid.spawn = spawn;
}

} // namespace GridMaker

// ----------------------------------------------------------------------------
// pathfinding/AStar.java, PathFinder.java
// ----------------------------------------------------------------------------
namespace Path {

struct PathItem {
    int cumulativeLength = 0;
    int totalPrevisionalLength = 0;
    int precedent = -1;
    Coord coord;
};

struct ItemLess {
    const std::vector<PathItem>* items;
    bool operator()(int a, int b) const { return (*items)[a].totalPrevisionalLength < (*items)[b].totalPrevisionalLength; }
};

struct AStar {
    Grid& grid;
    Coord from, target, nearest;
    const std::vector<char>& restricted;
    double centreX, centreY;
    std::vector<PathItem> items;
    std::vector<char> closed; // closedList keys

    AStar(Grid& g, Coord f, Coord t, const std::vector<char>& r) : grid(g), from(f), target(t), nearest(f), restricted(r) {
        centreX = grid.width / 2.0;
        centreY = grid.height / 2.0;
    }
    // coord.sqrEuclideanTo(centreX, centreY): Math.pow(centreX - x, 2) + Math.pow(centreY - y, 2), exact
    // here since the differences are multiples of 0.5
    double distToCentre(const Coord& c) const {
        double dx = centreX - c.x, dy = centreY - c.y;
        return dx * dx + dy * dy;
    }
    std::vector<Coord> find() {
        int item = getPathItemLinkedList();
        std::vector<Coord> path;
        for (int i = item; i >= 0; i = items[i].precedent) path.push_back(items[i].coord);
        std::reverse(path.begin(), path.end());
        return path;
    }
    int getPathItemLinkedList() {
        java::PriorityQueue<int, ItemLess> openList(ItemLess{&items});
        closed.assign(grid.width * grid.height, 0);
        PathItem root;
        root.coord = from;
        items.push_back(root);
        openList.offer(0);

        while (openList.size() > 0) {
            int visiting = openList.poll();
            Coord visitingCoord = items[visiting].coord;
            if (visitingCoord == target) return visiting;
            if (closed[grid.index(visitingCoord)]) continue;
            closed[grid.index(visitingCoord)] = 1;

            Coord neighbors[4];
            int nn = grid.getNeighbours(visitingCoord, neighbors);
            // Collections.sort (stable) by distance to the centre
            std::stable_sort(neighbors, neighbors + nn, [this](const Coord& a, const Coord& b) { return distToCentre(a) < distToCentre(b); });
            for (int k = 0; k < nn; k++) {
                const Coord& neighbor = neighbors[k];
                if (!grid.get(neighbor).obstacle && !restricted[grid.index(neighbor)]) addToOpenList(openList, visiting, visitingCoord, neighbor);
            }

            int visitingDist = visitingCoord.manhattanTo(target);
            int nearestDist = nearest.manhattanTo(target);
            if (visitingDist < nearestDist) {
                nearest = visitingCoord;
            } else if (visitingDist == nearestDist) {
                if (distToCentre(visitingCoord) < distToCentre(nearest)) nearest = visitingCoord;
            }
        }
        return -1; // not found !
    }
    void addToOpenList(java::PriorityQueue<int, ItemLess>& openList, int visiting, const Coord& fromCoord, const Coord& toCoord) {
        if (closed[grid.index(toCoord)]) return;
        PathItem pi;
        pi.coord = toCoord;
        pi.cumulativeLength = items[visiting].cumulativeLength + 1;
        pi.totalPrevisionalLength = pi.cumulativeLength + fromCoord.manhattanTo(toCoord);
        pi.precedent = visiting;
        items.push_back(pi);
        openList.offer((int)items.size() - 1);
    }
};

// PathFinder.findPath (weight function: 1 per cell). The second search goes to a cell the first one
// visited, so it never comes back empty.
static std::vector<Coord> findPath(Grid& grid, const std::vector<char>& restricted, Coord from, Coord to) {
    AStar a(grid, from, to, restricted);
    std::vector<Coord> path = a.find();
    if (path.empty()) {
        AStar b(grid, from, a.nearest, restricted);
        path = b.find();
    }
    return path;
}

} // namespace Path

// ----------------------------------------------------------------------------
// action/Action.java, ActionType.java, Player.java (+ engine AbstractPlayer state)
// ----------------------------------------------------------------------------
struct Action {
    enum Type { GROWTH, SPORE, WAIT } type = WAIT;
    i32 id = 0;
    Coord to;
    int organType = BASIC;
    int direction = NORTH;
    bool hasMessage = false;
    ustr message;
};

struct Player : cg::Player {
    // Player.java
    std::vector<Action> actions;
    int storage[4] = {0, 0, 0, 0};
    std::vector<Organ*> organs;
    std::vector<Organ*> roots;
    std::set<int> actedOrganismRootIds;
    // ProteinSummary (a missing key and 0 are reported the same way)
    int fromGrowth[4] = {0, 0, 0, 0}, fromHarvest[4] = {0, 0, 0, 0}, fromAbsorb[4] = {0, 0, 0, 0};
    std::map<int, ustr> messages; // HashMap<Integer, String>: only its size and contents are used
    std::vector<ustr> unassignedMessages;

    std::string nick() const { return "$" + std::to_string(index); }
    int getExpectedOutputLines() const override { return (int)roots.size(); }
    void reset() {
        actions.clear();
        actedOrganismRootIds.clear();
        for (int i = 0; i < 4; i++) fromGrowth[i] = fromHarvest[i] = fromAbsorb[i] = 0;
        unassignedMessages.clear();
        messages.clear();
    }
    void addOrgan(Organ* organ) {
        if (organ->type == ROOT) roots.push_back(organ);
        organs.push_back(organ);
    }
    void absorb(int protein) {
        if (protein < 0) return;
        storage[protein] += C::PROTEIN_PER_ABSORB;
        fromAbsorb[protein] += C::PROTEIN_PER_ABSORB;
    }
    void harvestProtein(int protein) {
        storage[protein] += 1;
        fromHarvest[protein] += 1;
    }
    void payProtein(int protein, int cost) {
        storage[protein] -= cost;
        fromGrowth[protein] += cost;
    }
    static void removeFirst(std::vector<Organ*>& list, Organ* o) {
        auto it = std::find(list.begin(), list.end(), o);
        if (it != list.end()) list.erase(it);
    }
    void removeOrgan(Organ* organ) {
        removeFirst(organs, organ);
        if (organ->type == ROOT) removeFirst(roots, organ);
    }
    bool hasAlreadyActed(int rootId) const { return actedOrganismRootIds.count(rootId) > 0; }
    void setActed(int rootId) { actedOrganismRootIds.insert(rootId); }
    int getProteinTotal() const { return storage[0] + storage[1] + storage[2] + storage[3]; }
    static void addToReports(std::vector<std::string>& reports, const int* report, const char* before, const char* after) {
        std::string text;
        for (int p = 0; p < 4; p++) {
            int n = report[p];
            if (n > 0) {
                if (!text.empty()) text += ", ";
                text += std::to_string(n) + PROTEIN_NAME[p];
            }
        }
        if (!text.empty()) reports.push_back(std::string(before) + text + after);
    }
    std::string proteinSummaryString() const {
        std::vector<std::string> reports;
        addToReports(reports, fromGrowth, "consumed ", " for growth");
        addToReports(reports, fromAbsorb, "gained ", " from absorption");
        addToReports(reports, fromHarvest, "gained ", " from harvest");
        std::string r;
        for (size_t i = 0; i < reports.size(); i++) r += (i ? "\n- " : "- ") + reports[i];
        return r;
    }
    std::string getProteinSummary() const {
        std::string report = proteinSummaryString();
        if (report.empty()) return "";
        return nick() + ":\n" + report;
    }
    void assignMessages() {
        std::deque<Organ*> rts;
        for (Organ* r : roots)
            if (!messages.count(r->id)) rts.push_back(r);
        for (const ustr& m : unassignedMessages) {
            if (rts.empty()) return;
            Organ* root = rts.front();
            rts.pop_front();
            messages[root->id] = m;
        }
    }
};

// ----------------------------------------------------------------------------
// Engine state used by the referee (GameManager.java, MultiplayerGameManager.java)
// ----------------------------------------------------------------------------
namespace GM {
static void addToGameSummary(const ustr& summary) { cg::gm.addToGameSummary(summary); }
static void addToGameSummary(const std::string& summary) { addToGameSummary(fromAscii(summary)); }
static ustr formatErrorMessage(const ustr& m) { return cg::GameManager::formatErrorMessage(m); }
static ustr formatSuccessMessage(const ustr& m) { return cg::GameManager::formatSuccessMessage(m); }
static void setFrameDuration(int fd) { cg::gm.setFrameDuration(fd); }
static void endGame() { cg::gm.endGame(); }
static void deactivate(Player& p, const ustr* reason) {
    if (reason) p.deactivate(*reason);
    else p.deactivate();
}
} // namespace GM

static Player players[2];

static std::vector<Player*> getActivePlayers() {
    std::vector<Player*> r;
    for (Player& p : players)
        if (p.active) r.push_back(&p);
    return r;
}

// ----------------------------------------------------------------------------
// event/Animation.java, EventData.java
// ----------------------------------------------------------------------------
namespace Ev {
const int GROW = 0, SPORE = 1, ATTACK = 2, DEATH = 3, HARVEST = 5, ABSORB = 6, CRASH = 7, SPAWN_NUCLEUS = 8;
}

struct EventData {
    int type = 0;
    int start = 0, end = 0;
    int playerIdx = -1, id = -1, organType = -1, direction = -1; // -1: null
    bool hasCoord = false, hasTarget = false, hasGrowFrom = false;
    Coord coord, target;
    std::vector<Coord> growFrom;
};

struct Animation {
    int frameTime = 0;
    int endTime = 0;
    std::vector<EventData> viewerEvents;
    void reset() {
        viewerEvents.clear();
        frameTime = 0;
        endTime = 0;
    }
    void startAnim(EventData e, int duration) {
        e.start = frameTime;
        e.end = frameTime + duration;
        endTime = std::max(endTime, frameTime + duration);
        viewerEvents.push_back(e);
    }
    void setFrameTime(int t) { frameTime = t; }
    void catchUp() { frameTime = endTime; }
};

// ----------------------------------------------------------------------------
// Game.java
// ----------------------------------------------------------------------------
namespace Game {

static Grid grid;
static int ENTITY_COUNT = 0; // Organ.ENTITY_COUNT
static java::JHashMap organByIdMap; // HashMap<Integer, Organ> organById (iteration order)
static std::vector<Organ*> organById; // by id, nullptr when absent
static Animation animation;
static int turn = 0;
static java::SHA1PRNG& random = cg::gm.getRandom(); // seeded by the engine

static const int SPORE_OK = 0, SPORE_HIT_WALL = 1, SPORE_MISALIGNED = 2, SPORE_NOT_FOUND = 3;

static Organ* getOrganById(i32 id) { return (id >= 0 && id < (i32)organById.size()) ? organById[id] : nullptr; }

static Organ* newOrgan(Player* owner, int type, int direction) {
    Organ* o = new Organ();
    o->owner = owner;
    o->type = type;
    o->direction = direction;
    o->parent = nullptr;
    o->id = ++ENTITY_COUNT;
    o->rootId = o->id;
    return o;
}

static int getRootId(const Organ* organ) {
    const Organ* current = organ;
    while (current->parent != nullptr) current = current->parent;
    return current->id;
}

static Player* getOpponent(const Player* player) {
    for (Player& p : players)
        if (&p != player) return &p;
    return nullptr;
}

static void launchCrashEvent(const Coord& coord, const std::vector<Coord>& growFrom) {
    EventData e;
    e.type = Ev::CRASH;
    e.hasCoord = true;
    e.coord = coord;
    e.hasGrowFrom = true;
    e.growFrom = growFrom;
    animation.startAnim(e, C::THIRD);
}
static void launchAbsorbEvent(const Organ* organ) {
    EventData e;
    e.type = Ev::ABSORB;
    e.hasCoord = true;
    e.coord = organ->pos;
    e.id = organ->id;
    e.playerIdx = organ->owner->index;
    animation.startAnim(e, C::WHOLE);
}
static void launchGrowEvent(const Organ* parent, const Organ* child) {
    EventData e;
    e.type = Ev::GROW;
    e.hasCoord = true;
    e.coord = parent->pos;
    e.hasTarget = true;
    e.target = child->pos;
    e.organType = child->type;
    e.direction = child->direction;
    e.playerIdx = parent->owner->index;
    e.id = child->id;
    animation.startAnim(e, C::WHOLE);
}
static void launchSporeEvent(const Coord& target, const Organ* organ) {
    EventData e;
    e.type = Ev::SPORE;
    e.hasCoord = true;
    e.coord = organ->pos;
    e.hasTarget = true;
    e.target = target;
    e.id = organ->id;
    e.playerIdx = organ->owner->index;
    int dist = std::abs(organ->pos.manhattanTo(target));
    animation.startAnim(e, C::TENTH * dist);
}
static void launchAttackEvent(const Coord& target, const Organ* organ) {
    EventData e;
    e.type = Ev::ATTACK;
    e.hasCoord = true;
    e.coord = organ->pos;
    e.hasTarget = true;
    e.target = target;
    e.id = organ->id;
    e.playerIdx = organ->owner->index;
    animation.startAnim(e, C::THIRD);
}
static void launchDeathEvent(const Organ* organ) {
    EventData e;
    e.type = Ev::DEATH;
    e.hasCoord = true;
    e.coord = organ->pos;
    e.id = organ->id;
    e.organType = organ->type;
    e.direction = organ->direction;
    e.playerIdx = organ->owner->index;
    animation.startAnim(e, C::WHOLE);
}
static void launchHarvestEvent(const Coord& target, const Organ* organ) {
    EventData e;
    e.type = Ev::HARVEST;
    e.hasCoord = true;
    e.coord = organ->pos;
    e.hasTarget = true;
    e.target = target;
    e.id = organ->id;
    e.playerIdx = organ->owner->index;
    animation.startAnim(e, 400);
}
static void launchSpawnNucleusEvent(const Organ* organ) {
    EventData e;
    e.type = Ev::SPAWN_NUCLEUS;
    e.hasCoord = true;
    e.coord = organ->pos;
    e.direction = organ->direction;
    e.id = organ->id;
    e.playerIdx = organ->owner->index;
    animation.startAnim(e, C::WHOLE);
}

static void removeOrgan(Organ* organ, int frameTime) {
    Player* player = organ->owner;
    Tile& tile = grid.get(organ->pos);

    tile.clear();
    player->removeOrgan(organ);

    animation.setFrameTime(frameTime);
    launchDeathEvent(organ);

    organByIdMap.remove(organ->id, organ->id);
    organById[organ->id] = nullptr;

    std::vector<Organ*> removed = organ->children;
    for (Organ* o : removed) removeOrgan(o, frameTime + C::TENTH);

    if (organ->parent != nullptr) Player::removeFirst(organ->parent->children, organ);
}

static void placeOrgan(Organ* organ, const Coord& coord) {
    Player* player = organ->owner;
    Tile& tile = grid.get(coord);
    int protein = tile.protein;

    organ->pos = coord;
    tile.placeOrgan(organ);
    player->addOrgan(organ);
    player->absorb(protein);
    if (protein >= 0) launchAbsorbEvent(organ);
    organByIdMap.put(organ->id, organ->id);
    if ((int)organById.size() <= organ->id) organById.resize(organ->id + 1, nullptr);
    organById[organ->id] = organ;
}

static void initPlayers() {
    // Initial storage
    for (int i = 0; i < C::PROTEIN_COUNT; ++i) {
        int startingProteinCount = random.nextInt(C::MIN_STARTING_PROTEINS, C::MAX_STARTING_PROTEINS + 1);
        for (Player& player : players) player.storage[i] = startingProteinCount;
    }
    // Initial organism
    Coord spawn = grid.spawn;
    for (Player& player : players) {
        Organ* root = newOrgan(&player, ROOT, NORTH);
        if (player.index == 1) spawn = grid.opposite(spawn);
        placeOrgan(root, spawn);
    }
}

static void init(i64 seed) {
    (void)seed;
    turn = 0;
    GridMaker::initGrid(grid, random);
    initPlayers();
}

static void resetGameTurnData() {
    animation.reset();
    for (Player& p : players) p.reset();
}

static void reportPlayerError(const std::string& message) { GM::addToGameSummary(GM::formatErrorMessage(fromAscii(message))); }

static bool canBuy(const Player& player, int organType) {
    return player.storage[0] >= ORGAN_COST[organType][0] && player.storage[1] >= ORGAN_COST[organType][1] &&
           player.storage[2] >= ORGAN_COST[organType][2] && player.storage[3] >= ORGAN_COST[organType][3];
}

static void payForOrgan(Player& player, int type) {
    for (int i = 0; i < C::PROTEIN_COUNT; ++i) player.payProtein(i, ORGAN_COST[type][i]);
}

static int signum(i32 v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

static int withinSporerRange(const Coord& origin, int direction, const Coord& target) {
    int dx = DIR_X[direction];
    int dy = DIR_Y[direction];
    bool inDirection = dx == signum((i32)((u32)target.x - (u32)origin.x)) && dy == signum((i32)((u32)target.y - (u32)origin.y));
    // SPORE_OVER_OBSTACLES is false
    if (inDirection) {
        Coord cur = origin;
        while (cur != target) {
            cur = cur.add(DIR_X[direction], DIR_Y[direction]);
            if (cur == target) return SPORE_OK;
            Tile& t = grid.get(cur);
            if (t.obstacle || t.hasOrgan()) return SPORE_HIT_WALL;
        }
    }
    return inDirection ? SPORE_NOT_FOUND : SPORE_MISALIGNED;
}

static bool checkCanGrowWithPathfinding(Player& player, Coord target, int organType, i32 fromId, bool isSporeCommand, Action& action) {
    const std::string nick = player.nick();
    // Cost
    if (!canBuy(player, organType)) {
        reportPlayerError(nick + " cannot grow a " + ORGAN_NAME[organType] + ", not enough proteins");
        return false;
    }
    // Correct organ id
    Organ* fromOrgan = getOrganById(fromId);
    if (fromOrgan == nullptr) {
        reportPlayerError(nick + " cannot grow from id=" + std::to_string(fromId) + ", that id does not exist");
        return false;
    }
    int fromRootId = getRootId(fromOrgan);
    // Organism ownership
    if (fromOrgan->owner != &player) {
        reportPlayerError(nick + " cannot grow from id=" + std::to_string(fromId) + ", not part of owned organisms");
        return false;
    }
    // Create ROOT only with SPORE command
    if (organType == ROOT && !isSporeCommand) {
        reportPlayerError(nick + " cannot grow NUCLEUS");
        return false;
    }
    // Create ROOT only from SPORER
    if (organType == ROOT && fromOrgan->type != SPORER) {
        reportPlayerError(nick + " cannot spore NUCLEUS from an organ that is not a SPORER");
        return false;
    }
    // Target within map
    if (!grid.get(target).valid) {
        reportPlayerError(nick + " cannot grow outside of map at " + target.toString());
        return false;
    }
    // Target is not part of the organism
    if (grid.get(target).hasOrgan() && getRootId(grid.get(target).organ) == fromRootId) {
        reportPlayerError(nick + " cannot grow onto organ at " + target.toString());
        return false;
    }

    bool hasPathfindingSummary = false;
    std::string pathfindingSummaryText;

    // Parent location
    if (organType != ROOT && fromOrgan->pos.manhattanTo(target) != 1) {
        // Try pathfinding
        std::vector<char> restricted(grid.width * grid.height, 0);
        for (int i = 0; i < grid.width * grid.height; i++) {
            Tile& t = grid.cells[i];
            if (t.hasOrgan() && getRootId(t.organ) != fromRootId) restricted[i] = 1;
        }
        std::vector<Coord> wholePath = Path::findPath(grid, restricted, fromOrgan->pos, target);

        bool foundPath = false;
        if (wholePath.size() > 1) {
            for (int i = (int)wholePath.size() - 2; i >= 0; i--) {
                Tile& current = grid.get(wholePath[i]);
                if (current.hasOrgan() && getRootId(current.organ) == fromRootId) {
                    if (!grid.get(wholePath[i + 1]).hasOrgan()) {
                        hasPathfindingSummary = true;
                        pathfindingSummaryText = nick + " will grow from " + current.organ->pos.toString() + " to " + wholePath[i + 1].toString() +
                                                 " because " + target.toString() + " is too far from id=" + std::to_string(fromId);
                        action.to = wholePath[i + 1];
                        action.id = current.organ->id;
                        target = action.to;
                        fromId = action.id;
                        fromOrgan = getOrganById(fromId);
                        foundPath = true;
                        break;
                    }
                }
            }
        }
        if (!foundPath) {
            reportPlayerError(nick + " cannot grow in direction of " + target.toString() + " from " + fromOrgan->pos.toString());
            return false;
        }
    }

    // Sporer line-of-sight
    int withinSporeRangeCode = withinSporerRange(fromOrgan->pos, fromOrgan->direction, target);
    if (organType == ROOT && withinSporeRangeCode != SPORE_OK) {
        if (withinSporeRangeCode == SPORE_HIT_WALL) {
            reportPlayerError(nick + " cannot spore from " + fromOrgan->pos.toString() + " to " + target.toString() + " because an obstacle is in the way");
        } else if (withinSporeRangeCode == SPORE_MISALIGNED) {
            reportPlayerError(nick + " " + target.toString() + " is not to the " + DIR_ALIAS[fromOrgan->direction] + " of " + fromOrgan->pos.toString());
        } else {
            reportPlayerError(nick + " cannot spore from " + fromOrgan->pos.toString() + " to " + target.toString() + " because it is unreachable");
        }
        return false;
    }
    // Target is free of obstacles
    if (grid.get(target).obstacle) {
        reportPlayerError(nick + " cannot grow onto obstacle at " + target.toString());
        return false;
    }
    // Target is free of organs
    if (grid.get(target).hasOrgan()) {
        reportPlayerError(nick + " cannot grow onto organ at " + target.toString());
        return false;
    }
    // Target is free of attack
    Player* foe = getOpponent(&player);
    Coord neighs[4];
    int nn = grid.getNeighbours(target, neighs);
    for (int k = 0; k < nn; k++) {
        if (grid.get(neighs[k]).hasTentacle(foe, target)) {
            reportPlayerError(nick + " cannot grow in front of tentacle at " + target.toString());
            return false;
        }
    }
    // Organism already acted
    int rootId = getRootId(fromOrgan);
    if (fromOrgan->owner->hasAlreadyActed(rootId)) {
        reportPlayerError(nick + " cannot use same organism multiple times");
        return false;
    }
    if (hasPathfindingSummary) GM::addToGameSummary(pathfindingSummaryText);
    return true;
}

static void handleMessage(const Action& action, Player& player, int rootId) {
    if (action.hasMessage) {
        ustr trimmed = java::trim(action.message);
        if (trimmed.size() > 48) trimmed = trimmed.substr(0, 46) + u"...";
        if (trimmed.size() > 0) {
            if (action.type == Action::GROWTH || action.type == Action::SPORE) player.messages[rootId] = trimmed;
            else player.unassignedMessages.push_back(trimmed);
        }
    }
}

struct Growth {
    Player* player;
    Action* action;
    bool isSpore;
};

static void doActions() {
    java::JHashMap growthsMap; // HashMap<Coord, List<Growth>> growthsByCoord, filled with compute()
    std::map<i32, std::vector<Growth>> growthsByCoord; // key: Coord.hashCode() (unique for in-grid coords)

    for (Player& player : players) {
        for (Action& action : player.actions) {
            if (action.type == Action::GROWTH || action.type == Action::SPORE) {
                bool canGrow = checkCanGrowWithPathfinding(player, action.to, action.organType, action.id, action.type == Action::SPORE, action);
                if (canGrow) {
                    // because pathfinding can change from/target
                    Coord target = action.to;
                    int organType = action.organType;
                    i32 fromId = action.id;

                    int rootId = getRootId(getOrganById(fromId));
                    payForOrgan(player, organType);
                    player.setActed(rootId);
                    growthsMap.compute(target.hashCode(), target.hashCode());
                    growthsByCoord[target.hashCode()].push_back(Growth{&player, &action, action.type == Action::SPORE});
                    handleMessage(action, player, rootId);
                }
            } else {
                handleMessage(action, player, 0);
            }
        }
        player.assignMessages();
    }

    std::vector<i32> order;
    growthsMap.keys(order);

    int frameTimeBeforeSpores = animation.frameTime;
    for (i32 key : order) {
        for (Growth& growth : growthsByCoord[key]) {
            animation.setFrameTime(frameTimeBeforeSpores);
            if (growth.isSpore) {
                launchSporeEvent(growth.action->to, getOrganById(growth.action->id));
                animation.catchUp();
            }
        }
    }

    animation.catchUp();

    for (i32 key : order) {
        std::vector<Growth>& growths = growthsByCoord[key];
        Coord coord = growths[0].action->to;
        // Wall created by cell collision
        if (growths.size() > 1) {
            grid.get(coord).setObstacle();
            std::vector<Coord> wouldbeParents;
            for (Growth& g : growths)
                if (!g.isSpore) wouldbeParents.push_back(getOrganById(g.action->id)->pos);
            launchCrashEvent(coord, wouldbeParents);
            continue;
        }
        Growth& growth = growths[0];
        Organ* organ = newOrgan(growth.player, growth.action->organType, growth.action->direction);
        placeOrgan(organ, coord);
        if (!organ->isNucleus()) {
            Organ* parent = getOrganById(growth.action->id);
            parent->children.push_back(organ); // connect
            organ->parent = parent;
            organ->rootId = parent->type == ROOT ? parent->id : parent->rootId;
            launchGrowEvent(parent, organ);
        } else {
            launchSpawnNucleusEvent(organ);
        }
    }
    animation.catchUp();
}

static void doHarvests() {
    struct Harvest {
        Tile* tile;
        Organ* organ;
        Coord coord;
    };
    for (Player& player : players) {
        // HashSet<Harvest>: iterated here in insertion order, Java uses identity hash codes (see header)
        std::vector<Harvest> harvests;
        for (Organ* o : player.organs) {
            if (o->isHarvester()) {
                Coord harvesting = o->getFacedCoord();
                Tile& t = grid.get(harvesting);
                if (t.valid && t.protein >= 0) {
                    bool exists = false;
                    for (Harvest& h : harvests)
                        if (h.coord == harvesting) exists = true;
                    if (!exists) harvests.push_back(Harvest{&t, o, harvesting});
                }
            }
        }
        for (Harvest& h : harvests) {
            int protein = h.tile->protein;
            player.harvestProtein(protein);
            launchHarvestEvent(h.coord, h.organ);
        }
    }
    animation.catchUp();
}

static void doAttacks() {
    java::JHashMap dyingIds; // HashSet<Integer>
    std::vector<i32> ids;
    organByIdMap.keys(ids);
    for (i32 id : ids) {
        Organ* o = organById[id];
        if (o->isTentacle()) {
            Coord attacking = o->getFacedCoord();
            Organ* target = grid.get(attacking).organ; // organByCoord.get(attacking)
            if (target != nullptr && target->owner != o->owner) {
                dyingIds.put(target->id, target->id);
                launchAttackEvent(target->pos, o);
            }
        }
    }
    animation.catchUp();

    int frameTime = animation.frameTime;
    std::vector<i32> dying;
    dyingIds.keys(dying);
    for (i32 id : dying) {
        Organ* organ = getOrganById(id);
        if (organ != nullptr) removeOrgan(organ, frameTime);
    }
}

static bool playerIsHarvesting(Player& player) {
    for (Organ* o : player.organs) {
        if (o->type == HARVESTER) {
            if (grid.get(o->getFacedCoord()).hasProtein()) return true;
        }
    }
    return false;
}

static bool playerHasEnoughProteinsToProgress(Player& player) {
    if (playerIsHarvesting(player)) return true;
    for (int type = 0; type < ORGAN_TYPES; type++)
        if (canBuy(player, type)) return true;
    return false;
}

static int getScore(const Player& player) { return (int)player.organs.size(); }

static bool playerIsInLastPlace(Player& player) { return getScore(player) < getScore(*getOpponent(&player)); }

static bool playersHaveEnoughProteinsToProgress() {
    bool bothStopped = true;
    for (Player& p : players)
        if (playerHasEnoughProteinsToProgress(p)) bothStopped = false;
    if (bothStopped) return false;
    for (Player& player : players) {
        if (!playerHasEnoughProteinsToProgress(player) && playerIsInLastPlace(player)) return false;
    }
    return true;
}

static bool hasAPlayerDied() {
    for (Player& p : players)
        if (p.organs.empty()) return true;
    return false;
}

static bool isGameOver() {
    bool gridFull = true;
    for (Tile& t : grid.cells)
        if (!(t.hasOrgan() || t.obstacle)) gridFull = false;
    bool noProteinsLeft = !playersHaveEnoughProteinsToProgress();
    bool aPlayerHasDied = hasAPlayerDied();
    bool maxTurnsReached = turn >= 100;
    bool lessThan2 = getActivePlayers().size() < 2;

    if (aPlayerHasDied || lessThan2) {
        GM::addToGameSummary(GM::formatSuccessMessage(u"\nGame over: only 1 player remains."));
    } else if (noProteinsLeft) {
        GM::addToGameSummary(GM::formatSuccessMessage(u"\nGame over: not enough proteins to continue evolving."));
    } else if (gridFull) {
        GM::addToGameSummary(GM::formatSuccessMessage(u"\nGame over: not enough space to continue evolving."));
    } else if (maxTurnsReached) {
        GM::addToGameSummary(GM::formatSuccessMessage(u"\nGame over: max turns reached."));
    }
    return aPlayerHasDied || noProteinsLeft || gridFull || maxTurnsReached || lessThan2;
}

static void computeEvents() {
    int minTime = 500;
    animation.catchUp();
    int frameTime = std::max(animation.frameTime, minTime);
    GM::setFrameDuration(frameTime);
}

static void performGameUpdate(int t) {
    turn = t;
    doActions();
    doHarvests();
    for (Player& p : players) {
        std::string summary = p.getProteinSummary();
        if (!summary.empty()) GM::addToGameSummary(summary);
    }
    doAttacks();
    if (isGameOver()) GM::endGame();
    computeEvents();
}

static void onEnd() {
    for (Player& p : players) p.score = p.active ? (int)p.organs.size() : -1;

    std::string endScreen;
    if (players[0].score == players[1].score) {
        // Tie break
        std::string scoreTexts[2];
        int scores[2];
        for (Player& player : players) {
            int realScore = player.score;
            player.score = player.getProteinTotal();
            scores[player.index] = player.score;
            scoreTexts[player.index] = std::to_string(realScore) + " (and " + std::to_string(player.getProteinTotal()) + " proteins)";
        }
        endScreen = "[[" + std::to_string(scores[0]) + "," + std::to_string(scores[1]) + "],\"logo.png\",[\"" + scoreTexts[0] + "\",\"" +
                    scoreTexts[1] + "\"]]";
    } else {
        endScreen = "[[" + std::to_string(players[0].score) + "," + std::to_string(players[1].score) + "],\"logo.png\",null]";
    }
    // EndScreenModule.onAfterOnEnd
    cg::gm.setViewData("endScreen", cg::Json::raw(endScreen));

    Player* winner = nullptr;
    if (players[0].score > players[1].score) winner = &players[0];
    else if (players[0].score < players[1].score) winner = &players[1];
    if (winner != nullptr) cg::gm.addTooltip(winner->index, fromAscii(winner->nick() + " wins!"));
}

} // namespace Game

// ----------------------------------------------------------------------------
// Serializer.java
// ----------------------------------------------------------------------------
namespace Serializer {

static std::string serializeStorage(const Player& p) {
    return std::to_string(p.storage[0]) + " " + std::to_string(p.storage[1]) + " " + std::to_string(p.storage[2]) + " " + std::to_string(p.storage[3]);
}

// view global data (ViewModule.sendGlobalData)
static ustr serializeGlobalData() {
    Grid& grid = Game::grid;
    std::string s = std::to_string(grid.width) + " " + std::to_string(grid.height);
    for (Tile& t : grid.cells) s += std::string("\n") + (t.obstacle ? "1" : "0") + " " + t.getProteinString();
    for (Player& p : players) {
        s += "\n" + std::to_string(p.organs.size());
        for (Organ* o : p.organs)
            s += "\n" + std::to_string(o->id) + " " + std::to_string(o->pos.x) + " " + std::to_string(o->pos.y) + " " + ORGAN_NAME[o->type] + " " +
                 DIR_ALIAS[o->direction] + " " + std::to_string(o->getParentId());
    }
    return fromAscii(s);
}

static std::string serializeEventCoords(const EventData& e) {
    std::string s;
    auto add = [&](const Coord& c) {
        if (!s.empty()) s += "_";
        s += c.toIntString();
    };
    if (e.hasCoord) add(e.coord);
    if (e.hasTarget) add(e.target);
    if (e.hasGrowFrom)
        for (const Coord& c : e.growFrom) add(c);
    return s;
}

// view frame data (ViewModule.sendFrameData)
static ustr serializeFrameData() {
    ustr s;
    auto line = [&](const std::string& l) {
        if (!s.empty()) s += u"\n";
        s += fromAscii(l);
    };
    for (Player& p : players) {
        line(serializeStorage(p));
        line(std::to_string(p.messages.size()));
        for (auto& kv : p.messages) {
            line(std::to_string(kv.first) + " ");
            s += kv.second;
        }
    }
    std::vector<EventData>& events = Game::animation.viewerEvents;
    line(std::to_string(events.size()));
    for (const EventData& e : events) {
        line(std::to_string(e.type));
        line(std::to_string(e.start));
        line(std::to_string(e.end));
        line(e.playerIdx < 0 ? "" : std::to_string(e.playerIdx));
        line(e.id < 0 ? "" : std::to_string(e.id));
        line(e.organType < 0 ? "" : ORGAN_NAME[e.organType]);
        line(e.direction < 0 ? "" : DIR_ALIAS[e.direction]);
        line(serializeEventCoords(e));
    }
    return s;
}

static std::vector<std::string> serializeGlobalInfoFor(const Player&) {
    return {std::to_string(Game::grid.width) + " " + std::to_string(Game::grid.height)};
}

static std::vector<std::string> serializeFrameInfoFor(const Player& player) {
    Grid& grid = Game::grid;
    std::vector<std::string> lines;
    lines.push_back(""); // entity count, set below
    for (int i = 0; i < grid.width * grid.height; i++) {
        Tile& t = grid.cells[i];
        Organ* o = t.organ;
        std::string type;
        if (t.obstacle) type = "WALL";
        else if (t.hasProtein()) type = t.getProteinString();
        else if (o != nullptr) type = ORGAN_NAME[o->type];
        else continue;
        Coord coord = grid.coordAt(i);
        lines.push_back(coord.toIntString() + " " + type + " " + (o ? (o->owner == &player ? "1" : "0") : "-1") + " " + std::to_string(o ? o->id : 0) + " " +
                        (o ? DIR_ALIAS[o->direction] : "X") + " " + std::to_string(o ? o->getParentId() : 0) + " " + std::to_string(o ? o->rootId : 0));
    }
    lines[0] = std::to_string(lines.size() - 1);
    lines.push_back(serializeStorage(player));
    for (Player& p : players)
        if (&p != &player) lines.push_back(serializeStorage(p));
    lines.push_back(std::to_string(player.getExpectedOutputLines()));
    return lines;
}

} // namespace Serializer

// ----------------------------------------------------------------------------
// CommandManager.java (ActionType patterns)
// ----------------------------------------------------------------------------
namespace Commands {

static bool startsWith(const ustr& s, const char16_t* p) {
    size_t n = std::char_traits<char16_t>::length(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}
static bool isDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
static bool isWord(char16_t c) { return isDigit(c) || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'_'; }

// -?\d+ at s[i]
static bool matchInt(const ustr& s, size_t& i, ustr& out) {
    size_t st = i, j = i;
    if (j < s.size() && s[j] == u'-') j++;
    size_t ds = j;
    while (j < s.size() && isDigit(s[j])) j++;
    if (j == ds) return false;
    out = s.substr(st, j - st);
    i = j;
    return true;
}
static bool matchChar(const ustr& s, size_t& i, char16_t c) {
    if (i < s.size() && s[i] == c) {
        i++;
        return true;
    }
    return false;
}

static std::string asciiOf(const ustr& s) { return std::string(s.begin(), s.end()); } // for ASCII-only groups

// Matches one ActionType pattern on the trimmed command. Returns 0 when it does not match, 1 when it
// matches, 2 when the consumer threw (exText = e.toString()).
static int tryGrowth(const ustr& t, Action& a, std::string& exText) {
    size_t i;
    if (startsWith(t, u"GROWTH ")) i = 7;
    else if (startsWith(t, u"GROW ")) i = 5;
    else return 0;
    ustr fromId, tx, ty;
    if (!matchInt(t, i, fromId) || !matchChar(t, i, u' ')) return 0;
    if (!matchInt(t, i, tx) || !matchChar(t, i, u' ')) return 0;
    if (!matchInt(t, i, ty) || !matchChar(t, i, u' ')) return 0;
    size_t ts = i;
    while (i < t.size() && isWord(t[i])) i++;
    if (i == ts) return 0;
    ustr type = t.substr(ts, i - ts);
    ustr rest = t.substr(i);
    bool hasDir = false, hasMsg = false;
    char16_t dir = 0;
    ustr msg;
    auto isDir = [](char16_t c) { return c == u'N' || c == u'E' || c == u'W' || c == u'S' || c == u'n' || c == u'e' || c == u'w' || c == u's'; };
    if (rest.empty()) {
    } else if (rest.size() >= 2 && rest[0] == u' ' && isDir(rest[1]) && (rest.size() == 2 || rest[2] == u' ')) {
        hasDir = true;
        dir = rest[1];
        if (rest.size() > 2) {
            hasMsg = true;
            msg = rest.substr(3);
        }
    } else if (rest[0] == u' ') {
        hasMsg = true;
        msg = rest.substr(1);
    } else {
        return 0;
    }
    a = Action();
    a.type = Action::GROWTH;
    i32 v;
    if (!java::parseInt(fromId, v)) {
        exText = "java.lang.NumberFormatException: For input string: \"" + asciiOf(fromId) + "\"";
        return 2;
    }
    a.id = v;
    i32 x, y;
    if (!java::parseInt(tx, x)) {
        exText = "java.lang.NumberFormatException: For input string: \"" + asciiOf(tx) + "\"";
        return 2;
    }
    if (!java::parseInt(ty, y)) {
        exText = "java.lang.NumberFormatException: For input string: \"" + asciiOf(ty) + "\"";
        return 2;
    }
    a.to = Coord(x, y);
    std::string typeName = asciiOf(type);
    int organType = -1;
    for (int k = 0; k < ORGAN_TYPES; k++)
        if (typeName == ORGAN_NAME[k]) organType = k;
    if (organType < 0) {
        exText = "java.lang.RuntimeException: <type> must be a valid organ type. Recieved: " + typeName;
        return 2;
    }
    a.organType = organType;
    if (!hasDir) a.direction = NORTH;
    else {
        char16_t u = dir >= u'a' ? (char16_t)(dir - 32) : dir;
        a.direction = u == u'N' ? NORTH : u == u'E' ? EAST : u == u'S' ? SOUTH : WEST;
    }
    a.hasMessage = hasMsg;
    a.message = msg;
    return 1;
}

static int trySpore(const ustr& t, Action& a, std::string& exText) {
    size_t i;
    if (startsWith(t, u"SPORE ")) i = 6;
    else if (startsWith(t, u"SPOR ")) i = 5;
    else return 0;
    ustr fromId, tx, ty;
    if (!matchInt(t, i, fromId) || !matchChar(t, i, u' ')) return 0;
    if (!matchInt(t, i, tx) || !matchChar(t, i, u' ')) return 0;
    if (!matchInt(t, i, ty)) return 0;
    bool hasMsg = false;
    ustr msg;
    if (i < t.size()) {
        if (t[i] != u' ') return 0;
        hasMsg = true;
        msg = t.substr(i + 1);
    }
    a = Action();
    a.type = Action::SPORE;
    i32 v, x, y;
    if (!java::parseInt(fromId, v)) {
        exText = "java.lang.NumberFormatException: For input string: \"" + asciiOf(fromId) + "\"";
        return 2;
    }
    a.id = v;
    if (!java::parseInt(tx, x)) {
        exText = "java.lang.NumberFormatException: For input string: \"" + asciiOf(tx) + "\"";
        return 2;
    }
    if (!java::parseInt(ty, y)) {
        exText = "java.lang.NumberFormatException: For input string: \"" + asciiOf(ty) + "\"";
        return 2;
    }
    a.to = Coord(x, y);
    a.organType = ROOT;
    a.direction = NORTH;
    a.hasMessage = hasMsg;
    a.message = msg;
    return 1;
}

static int tryWait(const ustr& t, Action& a) {
    if (t == u"WAIT") {
        a = Action();
        a.type = Action::WAIT;
        return 1;
    }
    if (startsWith(t, u"WAIT ")) {
        a = Action();
        a.type = Action::WAIT;
        a.hasMessage = true;
        a.message = t.substr(5);
        return 1;
    }
    return 0;
}

// Game.getExpected: command.toLowerCase().startsWith(...); only ASCII letters lower-case to these
static ustr getExpected(const ustr& command) {
    auto ci = [&](const char* p) {
        size_t n = strlen(p);
        if (command.size() < n) return false;
        for (size_t k = 0; k < n; k++) {
            char16_t c = command[k];
            if (c >= u'A' && c <= u'Z') c = (char16_t)(c + 32);
            if (c != (char16_t)p[k]) return false;
        }
        return true;
    };
    if (ci("grow")) return u"GROW <id> <x> <y> <type> (<dir>)";
    if (ci("spore")) return u"SPORE <id> <x> <y>";
    return u"GROW | SPORE | WAIT";
}

static ustr replaceAll(const ustr& s, const ustr& from, const ustr& to) {
    ustr r;
    size_t i = 0;
    while (true) {
        size_t j = s.find(from, i);
        if (j == ustr::npos) break;
        r += s.substr(i, j - i);
        r += to;
        i = j + from.size();
    }
    r += s.substr(i);
    return r;
}

static void handleCommands(Player& player, const std::vector<ustr>& lines) {
    for (const ustr& command : lines) {
        ustr t = java::trim(command);
        Action action;
        std::string exText;
        int r = tryGrowth(t, action, exText);
        if (r == 0) r = trySpore(t, action, exText);
        if (r == 0) r = tryWait(t, action);
        if (r == 1) {
            player.actions.push_back(action);
            continue;
        }
        // InvalidInputException
        ustr got = r == 2 ? fromAscii(exText) : command;
        ustr message = u"Invalid Input: Expected " + getExpected(command) + u" but got '" + got + u"'";
        ustr escaped = replaceAll(replaceAll(message, u"&lt;", u"<"), u"&gt;", u">");
        GM::deactivate(player, &escaped);
        GM::addToGameSummary(message);
        GM::addToGameSummary(GM::formatErrorMessage(fromAscii(player.nick() + ": disqualified!")));
    }
}

} // namespace Commands

// ----------------------------------------------------------------------------
// Referee.java
// ----------------------------------------------------------------------------
namespace Referee {

static void init(i64 seed) {
    Game::init(seed);
    // sendGlobalInfo
    for (Player* player : getActivePlayers())
        for (const std::string& line : Serializer::serializeGlobalInfoFor(*player)) player->sendInputLine(line);
    GM::setFrameDuration(1000);
    cg::gm.setMaxTurns(101);
    cg::gm.setTurnMaxTime(50);
    cg::gm.setFirstTurnMaxTime(1000);
}

static void handlePlayerCommands() {
    for (Player* player : getActivePlayers()) {
        if (player->timeout) { // TimeoutException
            ustr reason = u"Timeout!";
            GM::deactivate(*player, &reason);
            GM::addToGameSummary(player->nick() + " has not provided " + std::to_string(player->getExpectedOutputLines()) + " lines in time");
        } else {
            Commands::handleCommands(*player, player->outputs);
        }
    }
}

static void gameTurn(int turn) {
    Game::resetGameTurnData();
    for (Player* player : getActivePlayers()) {
        for (const std::string& line : Serializer::serializeFrameInfoFor(*player)) player->sendInputLine(line);
        player->execute();
    }
    handlePlayerCommands();
    Game::performGameUpdate(turn);
    if (getActivePlayers().size() < 2) GM::endGame();
}

} // namespace Referee

// ViewModule (graphics) and the engine
struct SprawlReferee : cg::Referee {
    static cg::Json jsonString(const ustr& s) { return cg::Json::sized(java::jsonQuoteSize(s)); }
    void init() override { ::Referee::init(cg::gm.getSeed()); }
    void gameTurn(int turn) override { ::Referee::gameTurn(turn); }
    void onEnd() override { Game::onEnd(); }
    void onGameInit() override {
        cg::gm.setViewGlobalData("graphics", jsonString(Serializer::serializeGlobalData()));
        cg::gm.setViewData("graphics", jsonString(Serializer::serializeFrameData()));
    }
    void onAfterGameTurn() override { cg::gm.setViewData("graphics", jsonString(Serializer::serializeFrameData())); }
};

int main(int argc, char** argv) {
    static SprawlReferee referee;
    cg::Options options;
    options.maxPlayers = 2;
    return cg::run(argc, argv, referee, [](int i) -> cg::Player* { return &players[i]; }, options);
}
