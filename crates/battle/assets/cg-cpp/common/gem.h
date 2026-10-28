// The CodinGame SDK's view modules, for C++ ports of CodinGame referees whose bots can push the view
// data past the engine's limits (a view frame over the runner's 30000 bytes, or more than 1 MB in
// total, makes the jar fail and print null scores): such a port must build the jar's view data, or at
// least its exact size. Reproduced here (com.codingame.gameengine.module.*):
//   - GraphicEntityModule (module-entities): the entities and their setters (with the Java fields a
//     view reads back and the setters' argument checks), EntityState (a HashMap<String, Param> and
//     its iteration order), WorldState (the states committed at each frame instant),
//     commitEntityState / commitWorldState, the autocommit of every entity at instant 1, the diffs
//     against the current world state, and the Serializer: the "entitymodule" frame text (C line:
//     the created entities, U line: the updates), DecimalFormat("0.######") values and frame
//     instants, rotations in whole degrees, curves, the escaping of strings; the global data (World)
//   - TooltipModule, ToggleModule and EndScreenModule (module-tooltip, module-toggle,
//     module-endscreen): their view data
// A game's own view code (its view controller and parts, its own modules such as an event module)
// stays in its port. A game module that copies one of these uses it (Spring 2020's TooltipModule is
// the engine's 3.15.2+ one plus global data the port sends itself).
//
//   #include "common/gem.h"
//
//   static gem::Module gem_;                      // GraphicEntityModule
//   static gem::TooltipModule tooltips_(true);    // engine 3.2.0-3.15.1: sent every frame
//
//   gem::Entity* s = gem_.createSprite();
//   gem_.setImage(s, "sub.png");
//   gem_.setX(s, 300, gem::EASE_OUT);
//   gem_.commitEntityState(0.5, {s});
//   tooltips_.setTooltipText(s, u"Player 0");
//
//   void onGameInit() override { gem_.sendGlobalData(); gem_.sendFrameData(); tooltips_.sendFrameData(); }
//   void onAfterGameTurn() override { gem_.sendFrameData(); tooltips_.sendFrameData(); }
//
// The modules must send their data in the order the jar calls them (the order Guice registered them,
// usually the order of the @Inject fields): it is the order of the keys of a frame. Only the text
// depends on it, not the size.
//
// Checked against the engine sources (github.com/CodinGame/codingame-game-engine, tags v2.4 to v4.7.7;
// the tags v3.13.0 and v3.15.0 are identical to the Maven Central sources jars). module-entities,
// module-endscreen, module-tooltip and module-toggle are identical in 3.13.0, 3.13.1 and 3.15.0-3.15.1,
// so there is no 3.13/3.15 option. The code comes from the Ocean of Code (engine 3.13.0: every curve,
// TooltipModule every frame, EndScreenModule) and Spring 2020 (3.15.0: ToggleModule, EndScreenModule,
// a TooltipModule sending only new registrations) ports, validated against their jars in direct mode
// (the jar's game log vs the -l trace: every view frame's size and text, including games failing on
// the 1 MB quota and on 30000-byte frames); moving it here changed no byte of their traces.
//
// Not reproduced (only the text differs, never a size):
//   - the order of the entity updates in a frame and of the ids in a group's "children" (a
//     HashMap<Entity, EntityState> and a HashSet<Entity>: identity hash codes in the jar): ascending
//     ids here
//   - the order of the ids in the tooltips / toggles objects (HashMap<Integer, ...>: buckets of
//     id & (capacity - 1) in the jar): ascending ids here
// Not implemented: sprite sheets (SpriteSheetSplitter: an "L" line before the C line), the engine
// versions below.
//
// EntityState's order: a HashMap<String, Param> iterates its buckets (spread(hashCode) & (capacity -
// 1)), then the insertion order in a bucket; a copy (new HashMap(map), JDK 19+) is pre-sized to
// tableSizeFor(ceil(n / 0.75)). A bucket never gets 9 keys (which would resize or treeify it): at a
// capacity of 16 the 49 property names of every version (with 2.2's "started") fill 6 at most.
//
// Version differences found in the sources, for ports of other engine versions (module-entities is
// identical in 2.4-3.1.0, and in 3.13.0-4.4.3 except where noted):
//   - Serializer.escape quotes a string ('...') when it contains: ' ' (2.4-3.4.7); ' ' or ';'
//     (3.4.8-3.10.x); ' ', ';' or '\n' (3.11.0-4.4.3, implemented here); ' ', ';', '\n' or '\''
//     (4.4.4+). ' becomes \' in every version.
//   - Up to 3.4.8 the frame text ends with a world commit line: "\nW" + the instants of the frame's
//     world commits (always "1", the autocommit, plus those of commitWorldState) in frame order,
//     joined with ' ' (Serializer.serializeWorldCommits). Gone in 3.4.9.
//   - Up to 3.12.x ContainerBasedEntity#add never sets the child's parent: remove() does nothing and
//     an entity can be added to several containers. 3.13.0+ (implemented here) sets it, and add()
//     throws IllegalArgumentException for an entity that already has a parent.
//   - Properties and types appear with their versions: RoundedRectangle (K), Polygon (P, "points"
//     ps) 3.4.0; skewX/skewY (kx, ky) 3.5.0, in decimal radians in 3.5.0 and whole degrees (like
//     rotation) from 3.5.1; TilingSprite (D, tileX tx, tileY ty, tileScaleX tsx, tileScaleY tsy)
//     3.6.0; scaleMode (sm) 3.8.3; the EASE_IN / EASE_OUT curves 3.9.0; textAlign (ta) 3.11.0;
//     createText() without text and TextBasedEntity#setMaxWidth ("maxWidth", not minified) 3.12.0.
//     Shape#setLineWidth takes an int before 3.11.1 (an Integer: same text as the double here).
//     4.3.0: a BufferedGroup can be a mask; 4.4.1: setBlendMode(null).
//   - TooltipModule, ToggleModule and EndScreenModule are engine modules from 3.2.0; 2.x games have
//     their own copies in the game code (port them as game code, or use these when identical).
//     EndScreenModule: [scores, titleRankingsSprite] in 3.2.x, [scores, titleRankingsSprite,
//     displayedText] from 3.3.0 (implemented here). TooltipModule: "[{...}]" every frame up to
//     3.15.1, only when there are new registrations from 3.15.2 (both implemented: TooltipModule's
//     everyFrame). ToggleModule: "[{id: {"name": ..., "state": ...}}]" every frame up to 3.15.2
//     (implemented here); from 3.15.3 only when there are new registrations, as a HashMap<String,
//     String> {toggle name: the ids each followed by '+' (state true) or '-'}.
//   - Other engine modules are not modelled: the viewport module (3.x and 4.x), the community camera
//     (4.2+) and interactive display (4.7) modules.
//
// Engine 2.2 (Module(true), "commandList"; added for Code of Kutulu): the Maven Central sources of
// module-entities 2.2 to 2.11 (checked: 2.2, 2.4, 2.5, 2.7, 2.10, 2.11; 2.12 has the format above) send
// "entitymodule" as a JSON array of command strings instead of one text: "L <name> <image> <width>
// <height> <origRow> <origCol> <imageCount> <imagesPerRow>" for each SpriteSheetLoader#load since the
// last frame (the loader's values when the frame is sent), "C <type>" for each new entity, then "U <id>
// <instant> <key> <value>..." for each entity whose diff at a frame instant is not empty (never an
// empty one). What else differs from the implementation above, all handled by commandList:
//   - an entity is not made visible by its constructor (no initial "v 1")
//   - Group keeps its children in a HashSet ("children" minified "C"); add() never sets the parent, so
//     it never throws and remove() never removes anything
//   - SpriteAnimation has no playing / restarted properties: reset() increments START_INDEX and sets
//     "started" (minified "p") to it, stop() sets it to ""
//   - Serializer.escape: ' -> \', quoted ('...') when it contains a space (no ';' / '\n' rule)
//   - curves: no EASE_IN / EASE_OUT (DEFAULT is LINEAR, not written)
// 2.4-2.11 add the BufferedGroup ("B"); 2.10 moves Group's code to ContainerBasedEntity: not checked
// further, only 2.2 is validated (Code of Kutulu's view data sizes, frame by frame, against its jar).
#pragma once

#include "engine.h"
#include "java.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace gem {

using java::String;

// ----------------------------------------------------------------------------
// Properties, curves, types (Serializer.keys, Serializer.curves, Serializer.types)
// ----------------------------------------------------------------------------

enum Key {
    X, Y, ZINDEX, SCALEX, SCALEY, SKEWX, SKEWY, ALPHA, ROTATION, VISIBLE, MASK, CHILDREN, BLENDMODE, ANCHORX, ANCHORY, TINT, IMAGE,
    BASEWIDTH, BASEHEIGHT, SCALEMODE, TILEX, TILEY, TILESCALEX, TILESCALEY, PLAYING, RESTARTED, LOOP, DURATION, IMAGES, TEXT, TEXTALIGN,
    FONTSIZE, MAXWIDTH, FONTFAMILY, FONTWEIGHT, STROKECOLOR, STROKETHICKNESS, FILLCOLOR, FILLALPHA, LINECOLOR, LINEWIDTH, LINEALPHA,
    WIDTH, HEIGHT, RADIUS, X2, Y2, POINTS,
    STARTED, // engine 2.2 (commandList): SpriteAnimation's "started"
    NKEYS
};

// the property names and their minified forms (the name itself when Serializer.keys has none)
struct KeyName {
    const char* name;
    const char* min;
};
inline constexpr KeyName KEY_NAMES[NKEYS] = {
    {"x", "x"},
    {"y", "y"},
    {"zIndex", "z"},
    {"scaleX", "sx"},
    {"scaleY", "sy"},
    {"skewX", "kx"},
    {"skewY", "ky"},
    {"alpha", "a"},
    {"rotation", "r"},
    {"visible", "v"},
    {"mask", "mask"},
    {"children", "ch"},
    {"blendMode", "b"},
    {"anchorX", "ax"},
    {"anchorY", "ay"},
    {"tint", "t"},
    {"image", "i"},
    {"baseWidth", "bw"},
    {"baseHeight", "bh"},
    {"scaleMode", "sm"},
    {"tileX", "tx"},
    {"tileY", "ty"},
    {"tileScaleX", "tsx"},
    {"tileScaleY", "tsy"},
    {"playing", "p"},
    {"restarted", "rs"},
    {"loop", "l"},
    {"duration", "d"},
    {"images", "I"},
    {"text", "T"},
    {"textAlign", "ta"},
    {"fontSize", "s"},
    {"maxWidth", "maxWidth"},
    {"fontFamily", "ff"},
    {"fontWeight", "fw"},
    {"strokeColor", "sc"},
    {"strokeThickness", "S"},
    {"fillColor", "f"},
    {"fillAlpha", "F"},
    {"lineColor", "c"},
    {"lineWidth", "W"},
    {"lineAlpha", "A"},
    {"width", "w"},
    {"height", "h"},
    {"radius", "R"},
    {"x2", "X"},
    {"y2", "Y"},
    {"points", "ps"},
    {"started", "p"},
};

// HashMap's spread(String#hashCode()) of each property name
inline constexpr std::array<i32, NKEYS> KEY_HASH = [] {
    std::array<i32, NKEYS> r{};
    for (int k = 0; k < NKEYS; k++) {
        u32 h = 0;
        for (const char* c = KEY_NAMES[k].name; *c; c++) h = 31 * h + (u32)(u8)*c;
        r[k] = (i32)(h ^ (h >> 16));
    }
    return r;
}();

// Curve (DEFAULT is LINEAR, which the Serializer never writes)
enum Curve { LINEAR, NONE, IMMEDIATE, EASE_IN, EASE_OUT, EASE_IN_AND_OUT, ELASTIC };
inline const char16_t* const CURVE_SYMBOL[7] = {u"/", u"_", u"Γ", u"\U0001F656", u"\U0001F652", u"∫", u"~"};

// Entity.Type, valued by Serializer.types
enum class Type : char {
    CIRCLE = 'C',
    LINE = 'L',
    RECTANGLE = 'R',
    SPRITE = 'S',
    TEXT = 'T',
    BITMAPTEXT = 'X',
    GROUP = 'G',
    BUFFERED_GROUP = 'B',
    SPRITEANIMATION = 'A',
    ROUNDED_RECTANGLE = 'K',
    POLYGON = 'P',
    TILING_SPRITE = 'D'
};

enum class BlendMode { NORMAL, ADD, MULTIPLY, SCREEN };  // BlendableEntity.BlendMode (its value is the ordinal)
enum class TextAlign { LEFT, CENTER, RIGHT };            // TextBasedEntity.TextAlign (its value is the ordinal)
enum class FontWeight { NORMAL, BOLD, BOLDER, LIGHTER }; // Text.FontWeight (toString: lower case name)
enum class ScaleMode { LINEAR, NEAREST };                // ScaleMode (toString: name)

// ----------------------------------------------------------------------------
// EntityState, WorldState
// ----------------------------------------------------------------------------

// A property value: an Integer, Double, Boolean or String; == is Object#equals (Double#equals
// compares the bits: NaN equals NaN, 0.0 differs from -0.0)
struct Val {
    enum Kind : char { INT, DOUBLE, BOOL, STRING };
    Kind k = INT;
    i64 i = 0;
    double d = 0;
    String s;

    static Val ofInt(i64 v) {
        Val r;
        r.i = v;
        return r;
    }
    static Val ofDouble(double v) {
        Val r;
        r.k = DOUBLE;
        r.d = v;
        return r;
    }
    static Val ofBool(bool v) {
        Val r;
        r.k = BOOL;
        r.i = v;
        return r;
    }
    static Val ofString(const String& v) {
        Val r;
        r.k = STRING;
        r.s = v;
        return r;
    }
    bool operator==(const Val& o) const {
        if (k != o.k) return false;
        if (k == DOUBLE) return java::equalsDouble(d, o.d);
        if (k == STRING) return s == o.s;
        return i == o.i;
    }
};

// EntityState.Param (its equals compares the values only, not the curves)
struct Param {
    Val v;
    Curve curve = LINEAR;
};

// EntityState: a HashMap<String, Param> (see the order above)
struct EntityState {
    std::vector<std::pair<int, Param>> items; // (key, param) in insertion order
    int cap = 16;                             // the table's capacity

    bool empty() const { return items.empty(); }
    const Param* get(int key) const {
        for (auto& it : items)
            if (it.first == key) return &it.second;
        return nullptr;
    }
    // HashMap#put: a new key goes last; the table doubles past 3/4 of its capacity
    void put(int key, const Param& p) {
        for (auto& it : items)
            if (it.first == key) {
                it.second = p;
                return;
            }
        items.push_back({key, p});
        if ((int)items.size() > cap * 3 / 4) cap *= 2;
    }
    // the indices of items in iteration order
    std::vector<int> order() const {
        std::vector<int> idx(items.size());
        std::iota(idx.begin(), idx.end(), 0);
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return (KEY_HASH[items[a].first] & (cap - 1)) < (KEY_HASH[items[b].first] & (cap - 1)); });
        return idx;
    }
    // new EntityState(other): new HashMap<>(map), pre-sized to tableSizeFor(ceil(n / 0.75)) (JDK 19+)
    static EntityState copyOf(const EntityState& o) {
        EntityState r;
        int n = (int)o.items.size();
        if (n > 0) {
            int t = (int)std::ceil(n / 0.75);
            r.cap = 1;
            while (r.cap < t) r.cap <<= 1;
        }
        for (int i : o.order()) r.items.push_back(o.items[i]);
        return r;
    }
    // diffFromNonNullOtherState: the properties whose value differs from prev's, in this one's order
    EntityState diffFrom(const EntityState& prev) const {
        EntityState diff;
        for (int i : order()) {
            const Param* p = prev.get(items[i].first);
            if (!p || !(p->v == items[i].second.v)) diff.put(items[i].first, items[i].second);
        }
        return diff;
    }
    // every property of next put into this one (WorldState#updateStateMap of an existing state)
    void merge(const EntityState& next) {
        for (int i : next.order()) put(next.items[i].first, next.items[i].second);
    }
};

struct WorldState {
    std::map<int, EntityState> states; // entity id -> state (a HashMap<Entity, EntityState> in the jar)
    bool worldCommit = false;
};

// ----------------------------------------------------------------------------
// Entities
// ----------------------------------------------------------------------------

// An entity of any type: the Java fields of Entity and of the subclasses that a view may read back
struct Entity {
    int id = 0;
    Type type = Type::GROUP;
    EntityState state; // the changes since its last commit
    // Entity
    int x = 0, y = 0, zIndex = 0;
    double scaleX = 1, scaleY = 1, skewX = 0, skewY = 0, rotation = 0, alpha = 1;
    bool visible = false;
    Entity* parent = nullptr; // the ContainerBasedEntity it was added to
    Entity* mask = nullptr;
    // ContainerBasedEntity: its HashSet<Entity> (ids in insertion order)
    std::vector<int> children;
    // Rectangle / RoundedRectangle, Circle / RoundedRectangle
    int width = 0, height = 0, radius = 0;
    // SpriteAnimation
    int duration = 1000, restartIndex = 0;
    bool playing = true, loop = false;
    // TextBasedEntity
    String text;
    int fontSize = 26, maxWidth = 0;
    // Polygon
    std::vector<std::pair<int, int>> points;
};

// the IllegalArgumentException of the entities' checks
inline java::Exception illegalArgument(const std::string& message) { return java::Exception{"java.lang.IllegalArgumentException: " + message}; }

// SpriteSheetLoader (engine 2.2, commandList): a builder whose load() queues itself for the next frame
// (its values are read when the frame is sent) and returns the image names
struct SpriteSheetLoader {
    std::string name, sourceImage;
    int width = 0, height = 0, origRow = 0, origCol = 0, imageCount = 0, imagesPerRow = 0;
};

// ----------------------------------------------------------------------------
// GraphicEntityModule
// ----------------------------------------------------------------------------
struct Module {
    std::vector<std::unique_ptr<Entity>> entities; // creation order (ids from 1)
    std::vector<Entity*> newEntities;              // created since the last frame
    std::map<std::string, WorldState> worldStates; // by frame instant, sorted like the jar's String sort
    std::vector<EntityState> current;              // currentWorldState, by entity id (index 0 unused)
    std::vector<char> hasCurrent;
    int worldWidth = 1920, worldHeight = 1080; // World
    bool lockWorld = false;
    bool commandList = false;                      // engine 2.2 (see the header)
    std::vector<const SpriteSheetLoader*> newSpriteSheets; // commandList: loaded since the last frame

    explicit Module(bool commandList_ = false) : commandList(commandList_) {
        current.emplace_back();
        hasCurrent.push_back(0);
    }

    // Entity#set
    void set(Entity* e, int key, const Val& v, Curve curve = LINEAR) {
        Param p;
        p.v = v;
        p.curve = curve;
        e->state.put(key, p);
    }

    // --- World
    // createWorld(width, height)
    void createWorld(int width, int height) {
        if (lockWorld) throw java::Exception{"java.lang.IllegalStateException: World creation must be the first use of this module."};
        lockWorld = true;
        worldWidth = width;
        worldHeight = height;
    }

    // --- creation (the Entity constructor makes it visible: its first commit shows it)
    Entity* create(Type type) {
        auto e = std::make_unique<Entity>();
        e->id = (int)entities.size() + 1;
        e->type = type;
        if (type == Type::CIRCLE) e->radius = 100;
        if (type == Type::ROUNDED_RECTANGLE) {
            e->width = e->height = 100;
            e->radius = 20;
        }
        Entity* r = e.get();
        lockWorld = true;
        entities.push_back(std::move(e));
        newEntities.push_back(r);
        current.emplace_back();
        hasCurrent.push_back(0);
        if (commandList) r->visible = true; // the field only
        else setVisible(r, true);
        return r;
    }
    Entity* createCircle() { return create(Type::CIRCLE); }
    Entity* createSprite() { return create(Type::SPRITE); }
    Entity* createLine() { return create(Type::LINE); }
    Entity* createRectangle() { return create(Type::RECTANGLE); }
    Entity* createRoundedRectangle() { return create(Type::ROUNDED_RECTANGLE); }
    Entity* createPolygon() { return create(Type::POLYGON); }
    Entity* createText() { return create(Type::TEXT); }
    // createText(string): the text is set before the entity is registered (no difference here)
    Entity* createText(const String& text) {
        Entity* e = create(Type::TEXT);
        setText(e, text);
        return e;
    }
    Entity* createBitmapText() { return create(Type::BITMAPTEXT); }
    Entity* createGroup(std::initializer_list<Entity*> es = {}) {
        Entity* g = create(Type::GROUP);
        add(g, es);
        return g;
    }
    Entity* createBufferedGroup(std::initializer_list<Entity*> es = {}) {
        Entity* g = create(Type::BUFFERED_GROUP);
        add(g, es);
        return g;
    }
    Entity* createSpriteAnimation() { return create(Type::SPRITEANIMATION); }
    Entity* createTilingSprite() { return create(Type::TILING_SPRITE); }

    // --- Entity
    void setX(Entity* e, int v, Curve curve = LINEAR) {
        e->x = v;
        set(e, X, Val::ofInt(v), curve);
    }
    void setY(Entity* e, int v, Curve curve = LINEAR) {
        e->y = v;
        set(e, Y, Val::ofInt(v), curve);
    }
    void setZIndex(Entity* e, int v) {
        e->zIndex = v;
        set(e, ZINDEX, Val::ofInt(v));
    }
    void setScaleX(Entity* e, double v, Curve curve = LINEAR) {
        e->scaleX = v;
        set(e, SCALEX, Val::ofDouble(v), curve);
    }
    void setScaleY(Entity* e, double v, Curve curve = LINEAR) {
        e->scaleY = v;
        set(e, SCALEY, Val::ofDouble(v), curve);
    }
    void setScale(Entity* e, double v, Curve curve = LINEAR) {
        setScaleX(e, v, curve);
        setScaleY(e, v, curve);
    }
    void setSkewX(Entity* e, double v, Curve curve = LINEAR) {
        e->skewX = v;
        set(e, SKEWX, Val::ofDouble(v), curve);
    }
    void setSkewY(Entity* e, double v, Curve curve = LINEAR) {
        e->skewY = v;
        set(e, SKEWY, Val::ofDouble(v), curve);
    }
    void setAlpha(Entity* e, double v, Curve curve = LINEAR) {
        requireValidAlpha(v);
        e->alpha = v;
        set(e, ALPHA, Val::ofDouble(v), curve);
    }
    void setRotation(Entity* e, double v, Curve curve = LINEAR) {
        e->rotation = v;
        set(e, ROTATION, Val::ofDouble(v), curve);
    }
    void setVisible(Entity* e, bool v) {
        e->visible = v;
        set(e, VISIBLE, Val::ofBool(v));
    }
    // setMask(mask): its id, -1 for null
    void setMask(Entity* e, Entity* mask) {
        e->mask = mask;
        set(e, MASK, Val::ofInt(mask ? mask->id : -1));
    }

    // --- BlendableEntity
    void setBlendMode(Entity* e, BlendMode m) { set(e, BLENDMODE, Val::ofInt((int)m)); }

    // --- TextureBasedEntity
    void setAnchorX(Entity* e, double v, Curve curve = LINEAR) { set(e, ANCHORX, Val::ofDouble(v), curve); }
    void setAnchorY(Entity* e, double v, Curve curve = LINEAR) { set(e, ANCHORY, Val::ofDouble(v), curve); }
    void setAnchor(Entity* e, double v) {
        setAnchorX(e, v);
        setAnchorY(e, v);
    }
    void setTint(Entity* e, int color, Curve curve = LINEAR) {
        requireValidColor(color);
        set(e, TINT, Val::ofInt(color), curve);
    }

    // --- SpriteBasedEntity (Sprite, TilingSprite)
    void setImage(Entity* e, const String& v) { set(e, IMAGE, Val::ofString(v)); }
    void setImage(Entity* e, const char* utf8) { setImage(e, java::decodeUtf8(utf8)); }
    void setBaseWidth(Entity* e, int v) { set(e, BASEWIDTH, Val::ofInt(v)); }
    void setBaseHeight(Entity* e, int v) { set(e, BASEHEIGHT, Val::ofInt(v)); }
    void setScaleMode(Entity* e, ScaleMode m) { set(e, SCALEMODE, Val::ofString(m == ScaleMode::LINEAR ? u"LINEAR" : u"NEAREST")); }

    // --- TilingSprite
    void setTileX(Entity* e, int v, Curve curve = LINEAR) { set(e, TILEX, Val::ofInt(v), curve); }
    void setTileY(Entity* e, int v, Curve curve = LINEAR) { set(e, TILEY, Val::ofInt(v), curve); }
    void setTileScaleX(Entity* e, double v, Curve curve = LINEAR) { set(e, TILESCALEX, Val::ofDouble(v), curve); }
    void setTileScaleY(Entity* e, double v, Curve curve = LINEAR) { set(e, TILESCALEY, Val::ofDouble(v), curve); }
    void setTileScale(Entity* e, double v, Curve curve = LINEAR) {
        setTileScaleX(e, v, curve);
        setTileScaleY(e, v, curve);
    }

    // --- SpriteAnimation
    void setPlaying(Entity* e, bool v) {
        e->playing = v;
        set(e, PLAYING, Val::ofBool(v));
    }
    void play(Entity* e) { setPlaying(e, true); }
    void pause(Entity* e) { setPlaying(e, false); }
    void reset(Entity* e) {
        if (commandList) { // START_INDEX++, setStarted(true)
            e->restartIndex++;
            setStarted(e, true);
            return;
        }
        set(e, RESTARTED, Val::ofInt(e->restartIndex++));
    }
    // commandList: setStarted(started), start() / stop(): "started" is START_INDEX, or "" when stopped
    void setStarted(Entity* e, bool started) {
        e->playing = started;
        set(e, STARTED, started ? Val::ofInt(e->restartIndex) : Val::ofString(String()));
    }
    void setLoop(Entity* e, bool v) {
        e->loop = v;
        set(e, LOOP, Val::ofBool(v));
    }
    void setDuration(Entity* e, int v) {
        if (v < 0) throw illegalArgument("Invalid duration: " + std::to_string(v) + "ms");
        e->duration = v;
        set(e, DURATION, Val::ofInt(v));
    }
    // setImages(String...): joined with ','
    void setImages(Entity* e, const std::vector<std::string>& images) {
        if (images.empty()) throw illegalArgument("Animation must contain at least 1 image.");
        String s;
        for (size_t i = 0; i < images.size(); i++) {
            if (i) s += u',';
            s += java::decodeUtf8(images[i]);
        }
        set(e, IMAGES, Val::ofString(s));
    }

    // --- TextBasedEntity (Text, BitmapText)
    void setText(Entity* e, const String& v) {
        e->text = v;
        set(e, TEXT, Val::ofString(v));
    }
    void setTextAlign(Entity* e, TextAlign a) { set(e, TEXTALIGN, Val::ofInt((int)a)); }
    void setFontSize(Entity* e, int v, Curve curve = LINEAR) {
        e->fontSize = v;
        set(e, FONTSIZE, Val::ofInt(v), curve);
    }
    void setMaxWidth(Entity* e, int v) {
        if (v < 0) throw illegalArgument("Invalid max width: " + std::to_string(v));
        e->maxWidth = v;
        set(e, MAXWIDTH, Val::ofInt(v));
    }

    // --- Text (setFontFamily) / BitmapText (setFont): the same property
    void setFontFamily(Entity* e, const String& v) { set(e, FONTFAMILY, Val::ofString(v)); }
    void setFontFamily(Entity* e, const char* utf8) { setFontFamily(e, java::decodeUtf8(utf8)); }
    void setFont(Entity* e, const char* utf8) { setFontFamily(e, utf8); }
    void setFontWeight(Entity* e, FontWeight w) {
        static const char16_t* const NAMES[4] = {u"normal", u"bold", u"bolder", u"lighter"};
        set(e, FONTWEIGHT, Val::ofString(NAMES[(int)w]));
    }
    void setStrokeColor(Entity* e, int color, Curve curve = LINEAR) {
        requireValidColor(color);
        set(e, STROKECOLOR, Val::ofInt(color), curve);
    }
    void setStrokeThickness(Entity* e, double v, Curve curve = LINEAR) { set(e, STROKETHICKNESS, Val::ofDouble(v), curve); }

    // --- Shape (Circle, Line, Rectangle, RoundedRectangle, Polygon); Text#setFillColor has no check
    void setFillColor(Entity* e, int color, Curve curve = LINEAR) {
        if (e->type != Type::TEXT) requireValidColor(color);
        set(e, FILLCOLOR, Val::ofInt(color), curve);
    }
    void setFillAlpha(Entity* e, double v, Curve curve = LINEAR) {
        requireValidAlpha(v);
        set(e, FILLALPHA, Val::ofDouble(v), curve);
    }
    void setLineAlpha(Entity* e, double v, Curve curve = LINEAR) {
        requireValidAlpha(v);
        set(e, LINEALPHA, Val::ofDouble(v), curve);
    }
    void setLineWidth(Entity* e, double v, Curve curve = LINEAR) { set(e, LINEWIDTH, Val::ofDouble(v), curve); }
    void setLineColor(Entity* e, int color, Curve curve = LINEAR) {
        requireValidColor(color);
        set(e, LINECOLOR, Val::ofInt(color), curve);
    }

    // --- Rectangle, RoundedRectangle
    void setWidth(Entity* e, int v, Curve curve = LINEAR) {
        e->width = v;
        set(e, WIDTH, Val::ofInt(v), curve);
    }
    void setHeight(Entity* e, int v, Curve curve = LINEAR) {
        e->height = v;
        set(e, HEIGHT, Val::ofInt(v), curve);
    }
    // --- Circle (a negative radius throws), RoundedRectangle
    void setRadius(Entity* e, int v, Curve curve = LINEAR) {
        if (e->type == Type::CIRCLE && v < 0) throw illegalArgument("A Circle's radius may not be less than zero");
        e->radius = v;
        set(e, RADIUS, Val::ofInt(v), curve);
    }
    // --- Line
    void setX2(Entity* e, int v, Curve curve = LINEAR) { set(e, X2, Val::ofInt(v), curve); }
    void setY2(Entity* e, int v, Curve curve = LINEAR) { set(e, Y2, Val::ofInt(v), curve); }

    // --- ContainerBasedEntity (Group, BufferedGroup): "children" is the ids joined with ','
    void setChildren(Entity* c) {
        std::vector<int> ids = c->children;
        std::sort(ids.begin(), ids.end());
        String s;
        for (size_t i = 0; i < ids.size(); i++) {
            if (i) s += u',';
            s += java::str(ids[i]);
        }
        set(c, CHILDREN, Val::ofString(s));
    }
    // add: an entity that already has a parent throws (the ones before it stay added)
    void add(Entity* c, std::initializer_list<Entity*> es) {
        for (Entity* e : es) {
            if (commandList) { // a HashSet; the parent is never set
                if (std::find(c->children.begin(), c->children.end(), e->id) == c->children.end()) c->children.push_back(e->id);
                continue;
            }
            if (e->parent)
                throw illegalArgument("Cannot add entity " + std::to_string(e->id) + " to container " + std::to_string(c->id) + ": it is already in container " +
                                      std::to_string(e->parent->id));
            c->children.push_back(e->id);
            e->parent = c;
        }
        setChildren(c);
    }
    void remove(Entity* c, Entity* e) {
        if (!commandList && e->parent == c) { // commandList: the parent is never set
            e->parent = nullptr;
            c->children.erase(std::find(c->children.begin(), c->children.end(), e->id));
            setChildren(c);
        }
    }

    // --- Polygon: "points" is x,y,x,y...
    void addPoint(Entity* e, int x, int y) {
        e->points.push_back({x, y});
        setPoints(e, LINEAR);
    }
    void clearPoints(Entity* e) {
        e->points.clear();
        setPoints(e, LINEAR);
    }
    void setPointsInterpolationCurve(Entity* e, Curve curve) { setPoints(e, curve); }

    // --- commits
    // commitEntityState(t, entities...)
    void commitEntityState(double t, std::initializer_list<Entity*> es) {
        WorldState& ws = stateAt(t, es.size());
        for (Entity* e : es) flush(ws, e);
    }
    // commitWorldState(t): every entity, and the state at t becomes a world commit
    void commitWorldState(double t) {
        WorldState& ws = stateAt(t, entities.size());
        ws.worldCommit = true;
        for (auto& e : entities) flush(ws, e.get());
    }

    // GraphicEntityModule#sendFrameData: the value of "entitymodule" for this frame
    String frameData() {
        // autocommit: every entity missing from the state at 1 is flushed into it, its state possibly
        // empty (an empty state adds nothing to a world commit, but makes a new entity current)
        WorldState& w1 = worldStates["1"];
        w1.worldCommit = true;
        for (auto& e : entities) {
            if (w1.states.count(e->id)) continue;
            if (e->state.empty() && hasCurrent[e->id]) continue; // nothing would change
            w1.states.emplace(e->id, std::move(e->state));
            e->state = EntityState();
        }
        String create;
        for (size_t i = 0; i < newEntities.size(); i++) {
            create += i ? u";" : u"C";
            create += (char16_t)newEntities[i]->type;
        }
        newEntities.clear();
        String update;
        for (auto& [t, ws] : worldStates) {
            for (auto& [id, st] : ws.states) {
                EntityState diff = hasCurrent[id] ? st.diffFrom(current[id]) : EntityState::copyOf(st);
                // an entity is sent with an empty diff unless the state is a world commit
                if (!ws.worldCommit || !diff.empty()) {
                    update += update.empty() ? u"U" : u";";
                    serializeEntry(update, id, t, diff);
                }
            }
            for (auto& [id, st] : ws.states) {
                if (!hasCurrent[id]) {
                    current[id] = st;
                    hasCurrent[id] = 1;
                } else {
                    current[id].merge(st);
                }
            }
        }
        worldStates.clear();
        if (create.empty()) return update;
        if (update.empty()) return create;
        return create + u"\n" + update;
    }

    // commandList: SpriteSheetLoader#load(): its checks, then the loader is queued for the next frame
    // (the frame reads its values then) and the image names are returned: name + i for each of
    // imageCount > 1 images, else the name
    std::vector<std::string> load(const SpriteSheetLoader* l) {
        auto invalid = [](const char* what) { return java::Exception{std::string("java.lang.IllegalStateException: invalid ") + what}; };
        if (l->width <= 0) throw invalid("width");
        if (l->height <= 0) throw invalid("height");
        if (l->imageCount <= 0) throw invalid("imageCount");
        if (l->origRow < 0) throw invalid("origRow");
        if (l->origCol < 0) throw invalid("origCol");
        newSpriteSheets.push_back(l);
        std::vector<std::string> names;
        if (l->imageCount > 1)
            for (int i = 0; i < l->imageCount; i++) names.push_back(l->name + std::to_string(i));
        else
            names.push_back(l->name);
        return names;
    }

    // commandList: GraphicEntityModule#sendFrameData of engine 2.2, the value of "entitymodule": a
    // JSON array of the frame's commands
    cg::Json frameCommands() {
        WorldState& w1 = worldStates["1"]; // autocommit (flushMissingEntities)
        for (auto& e : entities) {
            if (w1.states.count(e->id)) continue;
            if (e->state.empty() && hasCurrent[e->id]) continue; // nothing would change
            w1.states.emplace(e->id, std::move(e->state));
            e->state = EntityState();
        }
        std::vector<String> commands;
        for (const SpriteSheetLoader* l : newSpriteSheets)
            commands.push_back(java::decodeUtf8("L " + l->name + " " + l->sourceImage + " " + std::to_string(l->width) + " " + std::to_string(l->height) + " " +
                                                std::to_string(l->origRow) + " " + std::to_string(l->origCol) + " " + std::to_string(l->imageCount) + " " +
                                                std::to_string(l->imagesPerRow)));
        newSpriteSheets.clear();
        for (Entity* e : newEntities) commands.push_back(String(u"C ") + (char16_t)e->type);
        newEntities.clear();
        for (auto& [t, ws] : worldStates) {
            for (auto& [id, st] : ws.states) {
                EntityState diff = hasCurrent[id] ? st.diffFrom(current[id]) : EntityState::copyOf(st);
                if (diff.empty()) continue; // never sent empty
                String u = u"U ";
                u += java::str(id);
                u += u" ";
                u += java::ascii(t);
                for (int i : diff.order()) {
                    int key = diff.items[i].first;
                    u += u" ";
                    u += java::ascii(key == CHILDREN ? "C" : KEY_NAMES[key].min);
                    u += u" ";
                    u += minifyParam(key, diff.items[i].second, true);
                }
                commands.push_back(u);
            }
            for (auto& [id, st] : ws.states) {
                if (!hasCurrent[id]) {
                    current[id] = st;
                    hasCurrent[id] = 1;
                } else {
                    current[id].merge(st);
                }
            }
        }
        worldStates.clear();
        std::string s = "[";
        for (size_t i = 0; i < commands.size(); i++) s += (i ? "," : "") + java::jsonQuote(commands[i]);
        return cg::Json::raw(s + "]");
    }

    // Module#onGameInit / onAfterGameTurn: the frame's data
    void sendFrameData() {
        if (commandList) cg::gm.setViewData("entitymodule", frameCommands());
        else cg::gm.setViewData("entitymodule", cg::Json::string(frameData()));
    }
    // onGameInit: the World (before the first frame's data in the jar)
    void sendGlobalData() {
        cg::gm.setViewGlobalData("entitymodule", cg::Json::raw("{\"width\":" + std::to_string(worldWidth) + ",\"height\":" + std::to_string(worldHeight) + "}"));
        lockWorld = true;
    }

    // --- Serializer
    // escape (3.11.0-4.4.3): ' -> \', quoted when it contains a space, ';' or '\n'
    static String escape(const String& text) {
        String r;
        bool quote = false;
        for (char16_t c : text) {
            if (c == u'\'') r += u'\\';
            if (c == u' ' || c == u';' || c == u'\n') quote = true;
            r += c;
        }
        return quote ? u"'" + r + u"'" : r;
    }
    // escape of engine 2.2 (commandList): text.replaceAll("\\'", "\\\\'"), quoted when it contains a space
    static String escape22(const String& text) {
        String r;
        bool quote = false;
        for (char16_t c : text) {
            if (c == u'\'') r += u'\\';
            if (c == u' ') quote = true;
            r += c;
        }
        return quote ? u"'" + r + u"'" : r;
    }
    static String minifyParam(int key, const Param& p, bool v22 = false) {
        String r;
        const Val& v = p.v;
        if (key == ROTATION || key == SKEWX || key == SKEWY) r = java::str(java::d2i(java::toDegrees(v.d)));
        else if (v.k == Val::DOUBLE) r = java::ascii(java::formatDecimal(v.d, 6));
        else if (v.k == Val::BOOL) r = v.i ? u"1" : u"0";
        else if (v.k == Val::INT) r = java::str(v.i);
        else r = v22 ? escape22(v.s) : escape(v.s);
        if (p.curve != LINEAR) {
            r += u" ";
            r += CURVE_SYMBOL[p.curve];
        }
        return r;
    }
    // "<id> <instant>[ <key> <value>]..."
    static void serializeEntry(String& out, int id, const std::string& t, const EntityState& diff) {
        out += java::str(id);
        out += u" ";
        out += java::ascii(t);
        for (int i : diff.order()) {
            out += u" ";
            out += java::ascii(KEY_NAMES[diff.items[i].first].min);
            out += u" ";
            out += minifyParam(diff.items[i].first, diff.items[i].second);
        }
    }

  private:
    static void requireValidAlpha(double alpha) {
        if (alpha > 1) throw illegalArgument("An alpha may not exceed 1");
        if (alpha < 0) throw illegalArgument("An alpha may not be less than 0");
    }
    static void requireValidColor(int color) {
        if (color > 0xFFFFFF) throw illegalArgument(std::to_string(color) + "is not a valid RGB integer."); // sic
    }
    void setPoints(Entity* e, Curve curve) {
        String s;
        for (auto& [x, y] : e->points) {
            if (!s.empty()) s += u',';
            s += java::str(x) + u"," + java::str(y);
        }
        set(e, POINTS, Val::ofString(s), curve);
    }
    // commitState: the checks, then the world state at Serializer.formatFrameTime(t)
    WorldState& stateAt(double t, size_t count) {
        if (t < 0 || t > 1) throw illegalArgument("Not a valid frame instant: " + java::doubleToString(t));
        if (count == 0) throw illegalArgument("Must not be an empty array");
        return worldStates[java::formatDecimal(t, 6)];
    }
    // WorldState#flushEntityState
    void flush(WorldState& ws, Entity* e) {
        auto it = ws.states.find(e->id);
        if (it == ws.states.end()) ws.states.emplace(e->id, std::move(e->state));
        else it->second.merge(e->state);
        e->state = EntityState();
    }
};

// ----------------------------------------------------------------------------
// TooltipModule (module-tooltip): "tooltips" = [{id: text, ...}] of the new registrations
// ----------------------------------------------------------------------------
struct TooltipModule {
    // send "[{...}]" every frame, even empty (3.2.0-3.15.1); else only when there are new
    // registrations (3.15.2+)
    bool everyFrame;
    std::map<int, String> registered;
    std::map<int, std::optional<String>> newRegistration; // a removed tooltip is null: Gson leaves it out

    explicit TooltipModule(bool everyFrame_) : everyFrame(everyFrame_) {}

    void setTooltipText(const Entity* e, const String& text) {
        auto it = registered.find(e->id);
        if (it == registered.end() || it->second != text) {
            newRegistration[e->id] = text;
            registered[e->id] = text;
        }
    }
    const String* getTooltipText(const Entity* e) const {
        auto it = registered.find(e->id);
        return it == registered.end() ? nullptr : &it->second;
    }
    void removeTooltipText(const Entity* e) {
        newRegistration[e->id] = std::nullopt;
        registered.erase(e->id);
    }
    void sendFrameData() {
        if (!everyFrame && newRegistration.empty()) return;
        std::string s = "[{";
        bool first = true;
        for (auto& [id, text] : newRegistration) {
            if (!text) continue;
            if (!first) s += ",";
            first = false;
            s += "\"" + std::to_string(id) + "\":" + java::jsonQuote(*text);
        }
        newRegistration.clear();
        cg::gm.setViewData("tooltips", cg::Json::raw(s + "}]"));
    }
};

// ----------------------------------------------------------------------------
// ToggleModule (module-toggle 3.2.0-3.15.2): "toggles" = [{id: {"name": ..., "state": ...}, ...}]
// of the new registrations, every frame
// ----------------------------------------------------------------------------
struct ToggleModule {
    struct Toggle {
        String name;
        bool state = true;
        bool operator==(const Toggle& o) const { return state == o.state && name == o.name; }
    };
    std::map<int, Toggle> registered, newRegistration;

    void displayOnToggleState(const Entity* e, const String& toggle, bool state) {
        Toggle t{toggle, state};
        auto it = registered.find(e->id);
        if (it == registered.end() || !(it->second == t)) {
            newRegistration[e->id] = t;
            registered[e->id] = t;
        }
    }
    void sendFrameData() {
        std::string s = "[{";
        bool first = true;
        for (auto& [id, t] : newRegistration) {
            if (!first) s += ",";
            first = false;
            s += "\"" + std::to_string(id) + "\":{\"name\":" + java::jsonQuote(t.name) + ",\"state\":" + (t.state ? "true" : "false") + "}";
        }
        newRegistration.clear();
        cg::gm.setViewData("toggles", cg::Json::raw(s + "}]"));
    }
};

// ----------------------------------------------------------------------------
// EndScreenModule (module-endscreen 3.3.0+): "endScreen" = [scores, titleRankingsSprite,
// displayedText], sent by onAfterOnEnd (null for what was never set)
// ----------------------------------------------------------------------------
struct EndScreenModule {
    std::optional<std::vector<int>> scores;
    std::optional<std::vector<String>> displayedText;
    String titleRankingsSprite = u"logo.png";

    void setScores(const std::vector<int>& s) { scores = s; }
    void setScores(const std::vector<int>& s, const std::vector<String>& texts) {
        scores = s;
        displayedText = texts;
    }
    void setTitleRankingsSprite(const String& sprite) { titleRankingsSprite = sprite; }

    void onAfterOnEnd() {
        std::string s = "[";
        if (scores) {
            s += "[";
            for (size_t i = 0; i < scores->size(); i++) s += (i ? "," : "") + std::to_string((*scores)[i]);
            s += "]";
        } else {
            s += "null";
        }
        s += "," + java::jsonQuote(titleRankingsSprite) + ",";
        if (displayedText) {
            s += "[";
            for (size_t i = 0; i < displayedText->size(); i++) s += (i ? "," : "") + java::jsonQuote((*displayedText)[i]);
            s += "]";
        } else {
            s += "null";
        }
        cg::gm.setViewData("endScreen", cg::Json::raw(s + "]"));
    }
};

} // namespace gem
