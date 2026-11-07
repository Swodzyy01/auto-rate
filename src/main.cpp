// Auto Rate - Geode mod for Geometry Dash
//
// Decorates the blocks and hazards of a level (or only the objects you selected):
// edge glow + light bloom on all four sides, inset panels, emblems and
// per-style extras (flames, rays, sparkles, circuits, gears, waves ...).
// It finds FREE color channel ids first and never reuses one the level uses.
//
// Everything it adds is tagged with groups 9998 + 9999, so a later run or the
// Remove button can delete exactly its own output. Work is time-sliced
// (about 10 ms per frame) so the editor never freezes.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {

using Cell = std::pair<int, int>;  // (column, row) in 30-unit grid cells

constexpr int GROUP_A = 9998;
constexpr int GROUP_B = 9999;

char const* const STYLE_NAMES[] = {"Hell", "Heaven", "Green", "Yellow", "Aqua"};
char const* const QUALITY_NAMES[] = {"Fast", "Normal", "Ultra"};
char const* const BG_NAMES[] = {"Off", "Colors", "Full"};

template <size_t N>
std::string stepName(char const* const (&arr)[N], std::string const& cur, int dir) {
    size_t idx = 0;
    for (size_t i = 0; i < N; ++i) {
        if (cur == arr[i]) idx = i;
    }
    return arr[dir > 0 ? (idx + 1) % N : (idx + N - 1) % N];
}

// ------------------------------------------------------------------ styles
struct StyleDef {
    char const* name;
    ccColor3B bg, dark, mid, bright, glow;
    bool flames;    // tall thin spikes on top faces
    bool embers;    // floating glowing bits above platforms
    bool rays;      // light rays going up
    bool sparkles;  // little plus-shaped stars
    bool circuits;  // circuit traces on block faces
    bool gears;     // rotating gear emblem (saw) instead of a diamond
    bool waves;     // dashed wave lines on block faces
    bool icicles;   // spikes hanging under platforms
};

const StyleDef STYLES[] = {
    //  name      bg                dark              mid               bright            glow
    //            flames embers rays   sparkles circuits gears waves icicles
    {"Hell",   {28, 4, 4},      {110, 14, 8},    {210, 70, 12},   {255, 150, 25},  {255, 215, 110},
               true,  true,  false, false,   false,    false, false, true},
    {"Heaven", {140, 195, 255}, {175, 205, 240}, {235, 242, 255}, {255, 255, 255},  {255, 238, 170},
               false, false, true,  true,    false,    false, false, false},
    {"Green",  {3, 16, 9},      {8, 72, 32},     {20, 175, 75},   {85, 255, 125},   {200, 255, 210},
               false, false, false, true,    true,     false, false, false},
    {"Yellow", {34, 22, 0},     {120, 85, 0},    {230, 175, 10},  {255, 232, 70},   {255, 255, 190},
               false, false, true,  true,    false,    true,  false, false},
    {"Aqua",   {2, 30, 38},     {5, 90, 105},    {10, 185, 200},  {70, 240, 245},   {200, 255, 255},
               false, true,  false, false,   false,    false, true,  true},
};

StyleDef const& findStyle(std::string n) {
    // names from older versions
    if (n == "Gold") n = "Yellow";
    if (n == "Neon") n = "Green";
    if (n == "Blue") n = "Aqua";
    for (auto& s : STYLES) {
        if (n == s.name) return s;
    }
    return STYLES[0];
}

// ------------------------------------------------------------ object string
struct Obj {
    int id = 1;
    float x = 0, y = 0, rot = 0, sx = 1, sy = 1;
    int base = 0, detail = 0;  // color channel ids (keys 21 / 22)
    int z = 0, zo = 0;         // z layer (24) and z order (25)
    bool fx = false, fy = false;
    bool noTouch = true;       // key 121: decoration never kills / collides
};

std::string toStr(Obj const& o) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(2);
    s << "1," << o.id << ",2," << o.x << ",3," << o.y;
    if (o.fx) s << ",4,1";
    if (o.fy) s << ",5,1";
    if (o.rot != 0.f) s << ",6," << o.rot;
    if (o.base) s << ",21," << o.base;
    if (o.detail) s << ",22," << o.detail;
    if (o.z) s << ",24," << o.z;
    if (o.zo) s << ",25," << o.zo;
    if (o.sx != 1.f) s << ",128," << o.sx;
    if (o.sy != 1.f) s << ",129," << o.sy;
    if (o.noTouch) s << ",121,1";
    s << ",57," << GROUP_A << '.' << GROUP_B << ';';
    return s.str();
}

std::string colorTrig(float x, float y, int channel, ccColor3B c, float opacity, bool blend) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(2);
    s << "1,899,2," << x << ",3," << y
      << ",7," << (int)c.r << ",8," << (int)c.g << ",9," << (int)c.b
      << ",10,0,35," << opacity << ",23," << channel;
    if (blend) s << ",17,1";
    s << ",57," << GROUP_A << '.' << GROUP_B << ';';
    return s.str();
}

std::string pulseTrig(float x, float y, int channel, ccColor3B c) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(2);
    s << "1,1006,2," << x << ",3," << y
      << ",7," << (int)c.r << ",8," << (int)c.g << ",9," << (int)c.b
      << ",45,0.00,46,0.05,47,0.30,48,0,52,0,51," << channel
      << ",57," << GROUP_A << '.' << GROUP_B << ';';
    return s.str();
}

std::map<int, std::string> parseKV(std::string const& str) {
    std::map<int, std::string> out;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= str.size()) {
        size_t e = str.find(',', start);
        if (e == std::string::npos) e = str.size();
        parts.push_back(str.substr(start, e - start));
        start = e + 1;
    }
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        out[std::atoi(parts[i].c_str())] = parts[i + 1];
    }
    return out;
}

// True if the object's group list (key 57) contains both of our marker groups.
bool isOurs(std::map<int, std::string> const& kv) {
    auto it = kv.find(57);
    if (it == kv.end()) return false;
    auto const& str = it->second;
    bool a = false, b = false;
    size_t start = 0;
    while (start <= str.size()) {
        size_t e = str.find('.', start);
        if (e == std::string::npos) e = str.size();
        int g = std::atoi(str.substr(start, e - start).c_str());
        if (g == GROUP_A) a = true;
        if (g == GROUP_B) b = true;
        start = e + 1;
    }
    return a && b;
}

// ------------------------------------------------------------------ runner
class AutoRateRunner : public CCNode {
public:
    static void start(LevelEditorLayer* lel, bool removeOnly = false) {
        if (s_busy) {
            Notification::create("Auto Rate is already running", NotificationIcon::Warning)->show();
            return;
        }
        auto r = new AutoRateRunner();
        if (r->init(lel, removeOnly)) {
            s_busy = true;
            r->autorelease();
            lel->addChild(r);
        } else {
            delete r;
        }
    }

private:
    static inline bool s_busy = false;
    enum class Phase { Scan, Clean, Faces, Panels, Hazards, BgShapes, Triggers, Wait, Place, Finish };
    struct Hz { int id; float x, y, rot; bool fx, fy; };
    struct Edge { int kind, line, a, b; };  // kind: 0 top, 1 bottom, 2 left, 3 right
    struct Rect { int x0, x1, y0, y1; };

    // channel slots
    enum { CH_DARK = 0, CH_MID = 1, CH_BRIGHT = 2, CH_GLOW = 3, CH_HALO = 4, CH_COUNT = 5 };

    LevelEditorLayer* m_lel = nullptr;
    StyleDef const* m_style = &STYLES[0];
    float m_q = 0.7f;
    int m_bgStep = 9;
    float m_pulseStep = 240.f;
    int m_bgMode = 1;  // 0 off, 1 colors only, 2 colors + background shapes
    bool m_crown = true, m_pulses = true;
    bool m_removeOnly = false, m_replace = true;
    float m_yOff = 0.f;
    std::string m_qName;
    float m_minSeconds = 0.f;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();

    Ref<Notification> m_notif;
    Phase m_phase = Phase::Scan;
    bool m_abort = false;
    size_t m_i = 0;

    // selection mode
    std::unordered_set<GameObject*> m_sel;
    bool m_selMode = false;

    // analysis results
    std::vector<Ref<GameObject>> m_objs;
    std::vector<Ref<GameObject>> m_old;  // objects from an earlier Auto Rate run
    std::set<Cell> m_solid, m_occ;       // all solids / everything (for neighbour checks)
    std::vector<Cell> m_tgtCells;        // solid cells we decorate
    std::vector<Cell> m_rows, m_cols;    // (row, col) and (col, row), sorted
    std::vector<Edge> m_edges;
    std::vector<Rect> m_rects;
    std::vector<Hz> m_hazards;
    std::map<int, int> m_decoChunk;
    std::set<int> m_usedColors;
    bool m_hasBounds = false;
    int m_minCx = 0, m_maxCx = 0, m_minCy = 0, m_maxCy = 0, m_baseCy = 0;
    int m_nSolid = 0, m_nHaz = 0, m_nDeco = 0, m_nAdded = 0, m_nRemoved = 0;
    int m_ids[CH_COUNT] = {0, 0, 0, 0, 0};

    // generation state
    std::mt19937 m_rng;
    int m_bx = 0;
    std::vector<std::string> m_out;
    size_t m_placed = 0;

    ~AutoRateRunner() override { s_busy = false; }

    bool init(LevelEditorLayer* lel, bool removeOnly) {
        if (!CCNode::init()) return false;
        m_lel = lel;
        m_removeOnly = removeOnly;

        auto mod = Mod::get();
        m_style = &findStyle(mod->getSettingValue<std::string>("style"));
        m_qName = mod->getSettingValue<std::string>("quality");
        m_q = m_qName == "Fast" ? 0.4f : (m_qName == "Ultra" ? 1.0f : 0.7f);
        m_bgStep = (int)(14.f - 9.f * m_q);
        m_pulseStep = m_qName == "Fast" ? 360.f : (m_qName == "Ultra" ? 150.f : 240.f);
        auto bg = mod->getSettingValue<std::string>("bg-mode");
        m_bgMode = bg == "Off" ? 0 : (bg == "Full" ? 2 : 1);
        m_crown = mod->getSettingValue<bool>("crown");
        m_pulses = mod->getSettingValue<bool>("pulses");
        m_replace = mod->getSettingValue<bool>("replace-previous") || removeOnly;
        m_minSeconds = removeOnly ? 0.f : (float)mod->getSettingValue<int64_t>("analysis-time");
        m_yOff = (float)mod->getSettingValue<double>("y-offset");
        m_start = std::chrono::steady_clock::now();

        // Objects selected in the editor: decorate only those.
        if (!removeOnly && lel->m_editorUI) {
            if (auto arr = lel->m_editorUI->getSelectedObjects()) {
                for (auto* o : CCArrayExt<GameObject*>(arr)) m_sel.insert(o);
            }
        }
        m_selMode = !m_sel.empty();
        if (m_selMode) m_bgMode = 0;  // never touch the level's own background

        for (auto* o : CCArrayExt<GameObject*>(lel->m_objects)) {
            m_objs.emplace_back(o);
        }
        m_rng.seed((unsigned)m_objs.size() * 2654435761u + 12345u);

        m_notif = Notification::create("Auto Rate: analysing level...", NotificationIcon::Loading, 0.f);
        m_notif->show();
        this->schedule(schedule_selector(AutoRateRunner::tick));
        return true;
    }

    // ---------------------------------------------------------------- utils
    float rnd() { return std::uniform_real_distribution<float>(0.f, 1.f)(m_rng); }

    float dens(int cx) {
        auto it = m_decoChunk.find((int)std::floor(cx / 10.f));
        if (it == m_decoChunk.end()) return 1.f;
        if (it->second > 60) return 0.3f;  // chunk already heavily decorated
        if (it->second > 25) return 0.6f;
        return 1.f;
    }
    bool roll(float p, int cx) { return rnd() < p * dens(cx); }

    void add(Obj o) {
        o.y += m_yOff;
        m_out.push_back(toStr(o));
        m_nAdded++;
    }

    // block-shaped rectangle (sx, sy are in block units: 1 = 30 units)
    void strip(float x, float y, float sx, float sy, int slot, int z, int zo) {
        Obj o;
        o.id = 1;
        o.x = x;
        o.y = y;
        o.sx = sx;
        o.sy = sy;
        o.base = m_ids[slot];
        o.z = z;
        o.zo = zo;
        add(o);
    }
    void diamond(float x, float y, float s, int slot, int z, int zo) {
        Obj o;
        o.id = 1;
        o.x = x;
        o.y = y;
        o.rot = 45.f;
        o.sx = o.sy = s;
        o.base = m_ids[slot];
        o.z = z;
        o.zo = zo;
        add(o);
    }
    void spike(float x, float y, float sx, float sy, int slot, bool flip, int z, int zo) {
        Obj o;
        o.id = 8;
        o.x = x;
        o.y = y;
        o.sx = sx;
        o.sy = sy;
        o.fy = flip;
        o.base = m_ids[slot];
        o.z = z;
        o.zo = zo;
        add(o);
    }

    float elapsed() {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_start).count();
    }

    std::string status(char const* name) {
        if (m_minSeconds <= 0.f || m_phase >= Phase::Place) {
            return fmt::format("Auto Rate: {} ({} objects queued)", name, m_out.size());
        }
        float left = std::max(0.f, m_minSeconds - elapsed());
        int pct = (int)std::min(99.f, 100.f * elapsed() / m_minSeconds);
        return fmt::format("Auto Rate: analysing level... {}%  ({}s left)", pct, (int)std::ceil(left));
    }

    // The decoration is computed quickly; this holds the result back until the
    // chosen analysis time (mod setting) has passed. Set the time to 0 for instant.
    bool doWait() { return elapsed() >= m_minSeconds; }

    // ----------------------------------------------------------------- tick
    void tick(float) {
        auto t0 = std::chrono::steady_clock::now();
        std::function<bool()> over = [&] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 10.0;
        };

        while (true) {
            bool done = false;
            char const* name = "";
            switch (m_phase) {
                case Phase::Scan:     name = "analysing level";   done = doScan(over); break;
                case Phase::Clean:    name = "removing old deco"; done = doClean(over); break;
                case Phase::Faces:    name = "edges & light";     done = doFaces(over); break;
                case Phase::Panels:   name = "panels & emblems";  done = doPanels(over); break;
                case Phase::Hazards:  name = "hazard glow";       done = doHazards(over); break;
                case Phase::BgShapes: name = "background shapes"; done = doBgShapes(over); break;
                case Phase::Triggers: name = "colors & pulses";   done = doTriggers(); break;
                case Phase::Wait:     name = "analysing level";   done = doWait(); break;
                case Phase::Place:    name = "placing objects";   done = doPlace(over); break;
                case Phase::Finish:   finish(); return;
            }
            if (!done) {
                m_notif->setString(status(name));
                return;
            }
            m_phase = (Phase)((int)m_phase + 1);
            if ((m_abort || m_removeOnly) && m_phase > Phase::Clean) m_phase = Phase::Finish;
        }
    }

    // ----------------------------------------------------------------- scan
    bool doScan(std::function<bool()> const& over) {
        while (m_i < m_objs.size()) {
            scanOne(m_objs[m_i++].data());
            if (over()) return false;
        }
        m_objs.clear();
        m_objs.shrink_to_fit();
        prepare();
        m_i = 0;
        return true;
    }

    bool doClean(std::function<bool()> const& over) {
        while (m_i < m_old.size()) {
            m_lel->removeObject(m_old[m_i++].data(), true);
            m_nRemoved++;
            if (over()) return false;
        }
        m_old.clear();
        m_i = 0;
        return true;
    }

    void scanOne(GameObject* o) {
        auto type = o->m_objectType;
        auto p = o->getPosition();
        int cx = (int)std::floor(p.x / 30.f);
        int cy = (int)std::floor(p.y / 30.f);

        // The save string lists every color channel the object uses.
        auto kv = parseKV(std::string(o->getSaveString(m_lel).c_str()));
        if (m_replace && isOurs(kv)) {
            m_old.emplace_back(o);  // our own earlier output: delete, don't analyse
            return;
        }
        for (int k : {21, 22, 23, 50, 51}) {
            auto it = kv.find(k);
            if (it != kv.end()) m_usedColors.insert(std::atoi(it->second.c_str()));
        }

        m_occ.insert({cx, cy});
        bool target = !m_selMode || m_sel.count(o) > 0;

        if (type == GameObjectType::Decoration) {
            m_nDeco++;
            m_decoChunk[(int)std::floor(cx / 10.f)]++;
            return;
        }

        bool solid = type == GameObjectType::Solid;
        bool haz = type == GameObjectType::Hazard;
        bool slope = type == GameObjectType::Slope;

        if (solid) {
            m_solid.insert({cx, cy});
            if (target) {
                m_nSolid++;
                m_tgtCells.push_back({cx, cy});
            }
        }
        if (haz && target) {
            m_nHaz++;
            float rot = kv.count(6) ? (float)std::atof(kv[6].c_str()) : 0.f;
            bool fx = kv.count(4) && kv[4] == "1";
            bool fy = kv.count(5) && kv[5] == "1";
            m_hazards.push_back({o->m_objectID, p.x, p.y, rot, fx, fy});
        }
        if (target && (solid || haz || slope)) {
            if (!m_hasBounds) {
                m_minCx = m_maxCx = cx;
                m_minCy = m_maxCy = cy;
                m_hasBounds = true;
            } else {
                m_minCx = std::min(m_minCx, cx);
                m_maxCx = std::max(m_maxCx, cx);
                m_minCy = std::min(m_minCy, cy);
                m_maxCy = std::max(m_maxCy, cy);
            }
        }
    }

    void prepare() {
        if (!m_hasBounds) {
            m_abort = true;
            return;
        }
        // GD's ground top is at y=90 (row 3). Never start lower or higher than that.
        m_baseCy = std::min(std::max(0, m_minCy), 3);

        // Find FREE color channels (custom channels are 1..999).
        int found = 0;
        for (int id = 999; id >= 1 && found < CH_COUNT; --id) {
            if (!m_usedColors.count(id)) m_ids[found++] = id;
        }

        for (auto& c : m_tgtCells) {
            m_rows.push_back({c.second, c.first});
            m_cols.push_back({c.first, c.second});
        }
        std::sort(m_rows.begin(), m_rows.end());
        m_rows.erase(std::unique(m_rows.begin(), m_rows.end()), m_rows.end());
        std::sort(m_cols.begin(), m_cols.end());
        m_cols.erase(std::unique(m_cols.begin(), m_cols.end()), m_cols.end());

        buildEdges();
        buildRects();
        m_bx = m_minCx;
    }

    // Merge exposed block faces into long runs (one strip per run, not per block).
    void buildEdges() {
        for (int kind = 0; kind < 2; ++kind) {  // top / bottom, rows (cy, cx)
            Edge cur{kind, 0, 0, 0};
            bool active = false;
            for (auto& rc : m_rows) {
                int cy = rc.first, cx = rc.second;
                bool exposed = kind == 0 ? !m_solid.count({cx, cy + 1}) : !m_solid.count({cx, cy - 1});
                if (exposed) {
                    if (active && cur.line == cy && cur.b == cx - 1) {
                        cur.b = cx;
                    } else {
                        if (active) m_edges.push_back(cur);
                        cur = Edge{kind, cy, cx, cx};
                        active = true;
                    }
                } else {
                    if (active) m_edges.push_back(cur);
                    active = false;
                }
            }
            if (active) m_edges.push_back(cur);
        }
        for (int kind = 2; kind < 4; ++kind) {  // left / right, cols (cx, cy)
            Edge cur{kind, 0, 0, 0};
            bool active = false;
            for (auto& cc : m_cols) {
                int cx = cc.first, cy = cc.second;
                bool exposed = kind == 2 ? !m_solid.count({cx - 1, cy}) : !m_solid.count({cx + 1, cy});
                if (exposed) {
                    if (active && cur.line == cx && cur.b == cy - 1) {
                        cur.b = cy;
                    } else {
                        if (active) m_edges.push_back(cur);
                        cur = Edge{kind, cx, cy, cy};
                        active = true;
                    }
                } else {
                    if (active) m_edges.push_back(cur);
                    active = false;
                }
            }
            if (active) m_edges.push_back(cur);
        }
    }

    // Merge cells into rectangles (rows of equal width stacked on each other).
    void buildRects() {
        std::map<Cell, Cell> active;  // run (x0, x1) -> (y0, y1)
        size_t i = 0, n = m_rows.size();
        while (i < n) {
            int cy = m_rows[i].first;
            std::vector<Cell> runs;
            int a = m_rows[i].second, b = a;
            ++i;
            while (i < n && m_rows[i].first == cy) {
                int cx = m_rows[i].second;
                if (cx == b + 1) {
                    b = cx;
                } else {
                    runs.push_back({a, b});
                    a = b = cx;
                }
                ++i;
            }
            runs.push_back({a, b});

            std::map<Cell, Cell> next;
            for (auto& run : runs) {
                auto it = active.find(run);
                if (it != active.end() && it->second.second == cy - 1) {
                    next[run] = {it->second.first, cy};
                    active.erase(it);
                } else {
                    next[run] = {cy, cy};
                }
            }
            for (auto& kv : active) {
                m_rects.push_back({kv.first.first, kv.first.second, kv.second.first, kv.second.second});
            }
            active = std::move(next);
        }
        for (auto& kv : active) {
            m_rects.push_back({kv.first.first, kv.first.second, kv.second.first, kv.second.second});
        }
    }

    // ---------------------------------------------------------------- faces
    bool doFaces(std::function<bool()> const& over) {
        while (m_i < m_edges.size()) {
            genEdge(m_edges[m_i++]);
            if (over()) return false;
        }
        m_i = 0;
        return true;
    }

    void genEdge(Edge const& e) {
        float L = (float)(e.b - e.a + 1);
        float mid = (e.a + e.b + 1) * 15.f;
        switch (e.kind) {
            case 0: {  // top face
                float y = (e.line + 1) * 30.f;
                strip(mid, y - 1.8f, L, 0.12f, CH_GLOW, 3, 20);   // glow line
                strip(mid, y + 5.25f, L, 0.35f, CH_HALO, -1, 18); // light bloom
                decorateTop(e, y);
                break;
            }
            case 1: {  // bottom face
                float y = e.line * 30.f;
                strip(mid, y + 1.8f, L, 0.12f, CH_GLOW, 3, 20);
                strip(mid, y - 5.25f, L, 0.35f, CH_HALO, -1, 18);
                decorateBottom(e, y);
                break;
            }
            case 2: {  // left face
                float x = e.line * 30.f;
                strip(x + 1.8f, mid, 0.12f, L, CH_GLOW, 3, 20);
                strip(x - 5.25f, mid, 0.35f, L, CH_HALO, -1, 18);
                break;
            }
            default: {  // right face
                float x = (e.line + 1) * 30.f;
                strip(x - 1.8f, mid, 0.12f, L, CH_GLOW, 3, 20);
                strip(x + 5.25f, mid, 0.35f, L, CH_HALO, -1, 18);
                break;
            }
        }
    }

    void decorateTop(Edge const& e, float top) {
        for (int cx = e.a; cx <= e.b; ++cx) {
            if (m_occ.count({cx, e.line + 1})) continue;  // something already sits here
            float x = cx * 30.f + 15.f;
            bool edge = (cx == e.a || cx == e.b);

            if (m_crown) {
                if (m_style->flames) {
                    if (roll(0.25f + 0.5f * m_q, cx)) flame(x, top);
                } else {
                    crown(x, top, edge, cx);
                }
            }
            if (m_style->embers && roll(0.10f * m_q, cx)) ember(cx, x, top);
            if (m_style->rays && roll(0.08f * m_q, cx)) ray(x, top);
            if (m_style->sparkles && roll(0.07f * m_q, cx)) sparkle(cx, x, top);
        }
    }

    void decorateBottom(Edge const& e, float bottom) {
        if (!m_style->icicles) return;
        for (int cx = e.a; cx <= e.b; ++cx) {
            if (m_occ.count({cx, e.line - 1})) continue;
            if (!roll(0.3f * m_q, cx)) continue;
            float s = rnd() < 0.3f ? 1.0f : (rnd() < 0.5f ? 0.5f : 0.75f);
            spike(cx * 30.f + 15.f, bottom - 15.f * s, s, s, CH_MID, true, -1, 5);
        }
    }

    void crown(float x, float top, bool edge, int cx) {
        if (edge && roll(0.35f + 0.6f * m_q, cx)) {
            spike(x, top + 15.f, 1.f, 1.f, CH_BRIGHT, false, -1, 5);
        } else if (roll(0.5f * m_q, cx)) {
            spike(x, top + 7.5f, 0.5f, 0.5f, CH_MID, false, -1, 5);
        }
    }

    void flame(float x, float top) {
        int n = 2 + (int)(rnd() * 2.f);
        for (int k = 0; k < n; ++k) {
            float h = 0.5f + rnd() * 0.9f;
            float xo = (k - (n - 1) * 0.5f) * 8.f + (rnd() - 0.5f) * 3.f;
            spike(x + xo, top + 15.f * h, 0.35f, h, (k % 2) ? CH_BRIGHT : CH_MID, false, -1, 6);
        }
    }

    // glowing bit floating above a platform (embers / bubbles)
    void ember(int cx, float x, float top) {
        float y = top + 25.f + rnd() * 70.f;
        if (m_occ.count({cx, (int)std::floor(y / 30.f)})) return;
        diamond(x + (rnd() - 0.5f) * 20.f, y, 0.1f + rnd() * 0.12f, CH_GLOW, -1, 8);
    }

    void ray(float x, float top) {
        float h = 1.2f + rnd() * 1.6f;
        strip(x, top + 15.f * h, 0.35f, h, CH_HALO, -1, 3);  // soft beam
        strip(x, top + 15.f * h, 0.05f, h, CH_GLOW, -1, 4);  // bright core
    }

    void sparkle(int cx, float x, float top) {
        float y = top + 20.f + rnd() * 60.f;
        if (m_occ.count({cx, (int)std::floor(y / 30.f)})) return;
        float xx = x + (rnd() - 0.5f) * 16.f;
        strip(xx, y, 0.45f, 0.05f, CH_BRIGHT, -1, 9);
        strip(xx, y, 0.05f, 0.45f, CH_BRIGHT, -1, 9);
        diamond(xx, y, 0.12f, CH_GLOW, -1, 10);
    }

    // --------------------------------------------------------------- panels
    bool doPanels(std::function<bool()> const& over) {
        while (m_i < m_rects.size()) {
            genRect(m_rects[m_i++]);
            if (over()) return false;
        }
        m_i = 0;
        return true;
    }

    void genRect(Rect const& r) {
        int w = r.x1 - r.x0 + 1, h = r.y1 - r.y0 + 1;
        int mn = std::min(w, h);
        float W = w * 30.f, H = h * 30.f;
        float cx = (r.x0 + r.x1 + 1) * 15.f, cy = (r.y0 + r.y1 + 1) * 15.f;
        float left = cx - W / 2.f, bottom = cy - H / 2.f;
        int cell = r.x0;

        // inset panel frame with corner rivets
        if (w >= 2 && h >= 2 && roll(0.5f + 0.45f * m_q, cell)) {
            float inset = 7.f;
            float fw = (W - 2.f * inset) / 30.f, fh = (H - 2.f * inset) / 30.f;
            strip(cx, cy + H / 2.f - inset, fw, 0.1f, CH_MID, 3, 22);
            strip(cx, cy - H / 2.f + inset, fw, 0.1f, CH_MID, 3, 22);
            strip(cx - W / 2.f + inset, cy, 0.1f, fh, CH_MID, 3, 22);
            strip(cx + W / 2.f - inset, cy, 0.1f, fh, CH_MID, 3, 22);
            for (int sx = -1; sx <= 1; sx += 2) {
                for (int sy = -1; sy <= 1; sy += 2) {
                    diamond(cx + sx * (W / 2.f - inset), cy + sy * (H / 2.f - inset), 0.13f, CH_BRIGHT, 3, 23);
                }
            }
        }

        // emblem in the middle of big pieces
        if (mn >= 2 && roll(0.45f + 0.4f * m_q, cell)) {
            if (m_style->gears) {
                Obj g;  // rotating gear
                g.id = 88;
                g.x = cx;
                g.y = cy;
                g.sx = g.sy = std::min(1.2f, 0.28f * mn);
                g.base = m_ids[CH_MID];
                g.detail = m_ids[CH_BRIGHT];
                g.z = 3;
                g.zo = 24;
                add(g);
            } else {
                float s = std::min(2.0f, 0.4f * mn);
                diamond(cx, cy, s, CH_MID, 3, 24);
                diamond(cx, cy, s * 0.5f, CH_GLOW, 3, 25);
            }
        }

        // circuit traces
        if (m_style->circuits && w >= 2 && roll(0.7f, cell)) {
            float xs = left + 8.f;
            float len = (W - 16.f) * (0.4f + 0.5f * rnd());
            float y = bottom + 8.f + rnd() * (H - 16.f);
            strip(xs + len / 2.f, y, len / 30.f, 0.06f, CH_MID, 3, 22);
            diamond(xs, y, 0.12f, CH_BRIGHT, 3, 23);
            diamond(xs + len, y, 0.14f, CH_BRIGHT, 3, 23);
            float dir = (y - bottom > H / 2.f) ? -1.f : 1.f;
            strip(xs + len, y + dir * 7.5f, 0.06f, 0.5f, CH_MID, 3, 22);
            diamond(xs + len, y + dir * 15.f, 0.1f, CH_GLOW, 3, 23);
        }

        // dashed wave lines
        if (m_style->waves && w >= 2 && roll(0.7f, cell)) {
            float y = bottom + 8.f + rnd() * (H - 16.f);
            int n = std::min(40, (int)(W / 12.f));
            for (int k = 0; k < n; ++k) {
                strip(left + 6.f + k * 12.f + 4.f, y + ((k % 2) ? 3.f : -3.f), 0.3f, 0.06f,
                      (k % 3 == 0) ? CH_BRIGHT : CH_MID, 3, 22);
            }
        }

        // single-cell pieces get a small gem
        if (mn == 1 && w * h <= 2 && roll(0.35f * m_q, cell)) {
            diamond(cx, cy, 0.35f, CH_MID, 3, 22);
        }
    }

    // -------------------------------------------------------------- hazards
    bool doHazards(std::function<bool()> const& over) {
        while (m_i < m_hazards.size() && m_i < 6000) {
            auto& h = m_hazards[m_i++];
            if (roll(0.4f + 0.6f * m_q, (int)std::floor(h.x / 30.f))) {
                Obj o;
                o.id = h.id;
                o.x = h.x;
                o.y = h.y;
                o.rot = h.rot;
                o.fx = h.fx;
                o.fy = h.fy;
                o.sx = o.sy = 1.3f;
                o.base = m_ids[CH_GLOW];
                o.detail = m_ids[CH_GLOW];
                o.z = -1;
                o.zo = 15;
                add(o);
            }
            if (over()) return false;
        }
        m_i = 0;
        return true;
    }

    // ------------------------------------------------------- bg ornaments
    bool areaFree(int cx, int cy) {
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy)
                if (m_occ.count({cx + dx, cy + dy})) return false;
        return true;
    }

    // Only runs in background mode "Full": sparse saws / glowing gems in EMPTY areas.
    bool doBgShapes(std::function<bool()> const& over) {
        if (m_bgMode < 2) return true;
        while (m_bx < m_maxCx + 10) {
            int range = std::max(6, (m_maxCy - m_baseCy) + 8);
            int cy = m_baseCy + 3 + (int)(rnd() * range);
            if (roll(0.85f, m_bx) && areaFree(m_bx, cy)) {
                float x = m_bx * 30.f + 15.f, y = cy * 30.f + 15.f;
                bool gem = rnd() < 0.5f;
                if (!gem) {
                    Obj o;
                    o.id = 88;
                    o.x = x;
                    o.y = y;
                    o.sx = o.sy = 1.5f + rnd() * 2.5f;
                    o.base = m_ids[CH_DARK];
                    o.detail = m_ids[CH_MID];
                    o.z = -3;
                    o.zo = -4;
                    add(o);
                } else {
                    float s = 1.5f + rnd() * 2.5f;
                    diamond(x, y, s, CH_DARK, -3, -4);
                    diamond(x, y, s * 0.55f, CH_GLOW, -3, -3);
                }
            }
            m_bx += m_bgStep + (int)(rnd() * 3.f);
            if (over()) return false;
        }
        m_i = 0;
        return true;
    }

    // ------------------------------------------------------------- triggers
    bool doTriggers() {
        auto const& s = *m_style;
        float tx = -30.f;
        int row = 0;
        auto col = [&](int ch, ccColor3B c, float op, bool blend) {
            m_out.push_back(colorTrig(tx, 90.f + 30.f * row++, ch, c, op, blend));
        };
        if (m_bgMode >= 1) {
            col(1000, s.bg, 1.f, false);     // background
            col(1001, s.dark, 1.f, false);   // ground 1
            col(1009, s.dark, 1.f, false);   // ground 2
            col(1002, s.bright, 1.f, true);  // line
        }
        col(m_ids[CH_DARK], s.dark, 0.55f, false);
        col(m_ids[CH_MID], s.mid, 0.60f, false);
        col(m_ids[CH_BRIGHT], s.bright, 0.75f, false);
        col(m_ids[CH_GLOW], s.glow, 0.80f, true);
        col(m_ids[CH_HALO], s.glow, 0.25f, true);

        if (m_pulses) {
            int idx = 0;
            float end = (m_maxCx + 1) * 30.f;
            for (float x = std::max(0, m_minCx) * 30.f; x < end; x += m_pulseStep, ++idx) {
                int target = idx % 2 == 0 ? m_ids[CH_GLOW] : m_ids[CH_BRIGHT];
                m_out.push_back(pulseTrig(x, 450.f + 30.f * (idx % 6), target, s.glow));
            }
        }
        m_i = 0;
        return true;
    }

    // ---------------------------------------------------------------- place
    bool doPlace(std::function<bool()> const& over) {
        while (m_placed < m_out.size()) {
            std::string chunk;
            size_t n = 0;
            while (m_placed < m_out.size() && n < 200) {
                chunk += m_out[m_placed++];
                ++n;
            }
            m_lel->createObjectsFromString(chunk, true, true);
            if (over()) return false;
        }
        return true;
    }

    // --------------------------------------------------------------- finish
    void finish() {
        this->unschedule(schedule_selector(AutoRateRunner::tick));
        m_notif->hide();

        std::string report;
        if (m_removeOnly) {
            report = fmt::format("Removed <cg>{}</c> Auto Rate objects.", m_nRemoved);
        } else if (m_abort) {
            report = "Nothing to decorate: no blocks, slopes or hazards were found"
                     + std::string(m_selMode ? " in your selection." : " in this level.");
        } else {
            report = fmt::format(
                "Style: <cy>{}</c>  |  Quality: <cy>{}</c>\n"
                "Mode: <cy>{}</c>\n"
                "Decorated: {} blocks, {} hazards (existing deco objects seen: {})\n"
                "Free color channels used: <cg>{}, {}, {}, {}, {}</c>\n"
                "Added: <cg>{}</c> objects + triggers (replaced {} old ones)\n\n"
                "To remove it: pause menu -> Remove.",
                m_style->name, m_qName, m_selMode ? "selected objects only" : "whole level",
                m_nSolid, m_nHaz, m_nDeco,
                m_ids[0], m_ids[1], m_ids[2], m_ids[3], m_ids[4], m_nAdded, m_nRemoved);
        }
        FLAlertLayer::create("Auto Rate", report, "OK")->show();
        this->removeFromParent();
    }
};

}  // namespace

// ------------------------------------------------------------------- hook
class $modify(AutoRatePauseLayer, EditorPauseLayer) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorPauseLayer::init(lel)) return false;

        auto mod = Mod::get();
        auto win = CCDirector::get()->getWinSize();
        float cx = win.width - 95.f;

        auto menu = CCMenu::create();
        menu->setID("auto-rate-menu"_spr);
        menu->setPosition({cx, 30.f});

        auto bigBtn = [&](char const* label, SEL_MenuHandler cb, float y, std::string const& id) {
            auto btn = CCMenuItemSpriteExtra::create(ButtonSprite::create(label), this, cb);
            btn->setID(id);
            btn->setPosition({0.f, y});
            menu->addChild(btn);
        };
        auto arrow = [&](bool right, SEL_MenuHandler cb, float y, std::string const& id) {
            auto spr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
            spr->setScale(0.8f);
            spr->setFlipX(right);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setID(id);
            btn->setPosition({right ? 80.f : -80.f, y});
            menu->addChild(btn);
        };
        auto valueLabel = [&](std::string const& text, float y, std::string const& id) {
            auto lbl = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
            lbl->setScale(0.45f);
            lbl->setID(id);
            lbl->setPosition({cx, 30.f + y});
            this->addChild(lbl);
        };

        bigBtn("Auto Rate", menu_selector(AutoRatePauseLayer::onAutoRate), 0.f, "auto-rate-button"_spr);
        bigBtn("Remove", menu_selector(AutoRatePauseLayer::onRemove), 34.f, "auto-rate-remove"_spr);

        arrow(false, menu_selector(AutoRatePauseLayer::onBgPrev), 68.f, "auto-rate-bg-prev"_spr);
        arrow(true, menu_selector(AutoRatePauseLayer::onBgNext), 68.f, "auto-rate-bg-next"_spr);
        valueLabel("Background: " + mod->getSettingValue<std::string>("bg-mode"), 68.f, "auto-rate-bg-label"_spr);

        arrow(false, menu_selector(AutoRatePauseLayer::onQualityPrev), 102.f, "auto-rate-quality-prev"_spr);
        arrow(true, menu_selector(AutoRatePauseLayer::onQualityNext), 102.f, "auto-rate-quality-next"_spr);
        valueLabel("Quality: " + mod->getSettingValue<std::string>("quality"), 102.f, "auto-rate-quality-label"_spr);

        arrow(false, menu_selector(AutoRatePauseLayer::onStylePrev), 136.f, "auto-rate-style-prev"_spr);
        arrow(true, menu_selector(AutoRatePauseLayer::onStyleNext), 136.f, "auto-rate-style-next"_spr);
        valueLabel("Style: " + mod->getSettingValue<std::string>("style"), 136.f, "auto-rate-style-label"_spr);

        this->addChild(menu);
        return true;
    }

    void setLabel(std::string const& id, std::string const& text) {
        if (auto lbl = typeinfo_cast<CCLabelBMFont*>(this->getChildByID(id))) {
            lbl->setString(text.c_str());
        }
    }

    void stepStyle(int dir) {
        auto mod = Mod::get();
        auto next = stepName(STYLE_NAMES, mod->getSettingValue<std::string>("style"), dir);
        mod->setSettingValue<std::string>("style", next);
        this->setLabel("auto-rate-style-label"_spr, "Style: " + next);
    }

    void stepQuality(int dir) {
        auto mod = Mod::get();
        auto next = stepName(QUALITY_NAMES, mod->getSettingValue<std::string>("quality"), dir);
        mod->setSettingValue<std::string>("quality", next);
        this->setLabel("auto-rate-quality-label"_spr, "Quality: " + next);
    }

    void stepBg(int dir) {
        auto mod = Mod::get();
        auto next = stepName(BG_NAMES, mod->getSettingValue<std::string>("bg-mode"), dir);
        mod->setSettingValue<std::string>("bg-mode", next);
        this->setLabel("auto-rate-bg-label"_spr, "Background: " + next);
    }

    void onStylePrev(CCObject*) { this->stepStyle(-1); }
    void onStyleNext(CCObject*) { this->stepStyle(1); }
    void onQualityPrev(CCObject*) { this->stepQuality(-1); }
    void onQualityNext(CCObject*) { this->stepQuality(1); }
    void onBgPrev(CCObject*) { this->stepBg(-1); }
    void onBgNext(CCObject*) { this->stepBg(1); }

    void onAutoRate(CCObject*) {
        auto mod = Mod::get();
        auto style = mod->getSettingValue<std::string>("style");
        auto quality = mod->getSettingValue<std::string>("quality");
        auto secs = mod->getSettingValue<int64_t>("analysis-time");
        auto lel = m_editorLayer;

        int selected = 0;
        if (lel->m_editorUI) {
            if (auto arr = lel->m_editorUI->getSelectedObjects()) selected = (int)arr->count();
        }
        std::string target = selected > 0
            ? fmt::format("Only your <cy>{}</c> selected objects will be decorated.", selected)
            : std::string("The <cy>whole level</c> will be decorated. (Select objects first to decorate only those.)");

        createQuickPopup(
            "Auto Rate",
            fmt::format(
                "Style: <cy>{}</c>   Quality: <cy>{}</c>\n{}\n"
                "The analysis takes about <cy>{}</c> seconds.\n"
                "Earlier Auto Rate decoration is replaced automatically.",
                style, quality, target, secs),
            "Cancel", "Start",
            [this, lel](FLAlertLayer*, bool ok) {
                if (!ok) return;
                this->onResume(nullptr);
                AutoRateRunner::start(lel);
            });
    }

    void onRemove(CCObject*) {
        auto lel = m_editorLayer;
        createQuickPopup(
            "Auto Rate",
            "Delete everything Auto Rate added to this level?\nYour own objects are not touched.",
            "Cancel", "Delete",
            [this, lel](FLAlertLayer*, bool ok) {
                if (!ok) return;
                this->onResume(nullptr);
                AutoRateRunner::start(lel, true);
            });
    }
};
