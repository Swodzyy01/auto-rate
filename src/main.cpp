// Auto Rate - Geode mod for Geometry Dash
// Analyses the whole level in the editor, finds FREE color channel IDs,
// then adds a themed decoration pass: edge glow, crowns, icicles,
// hazard glow, skyline backdrop, background shapes, color + pulse triggers.
//
// Work is time-sliced (about 10 ms per frame) so the editor never freezes.

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
#include <vector>

using namespace geode::prelude;

namespace {

using Cell = std::pair<int, int>;  // (column, row) in 30-unit grid cells

// ------------------------------------------------------------------ styles
struct StyleDef {
    char const* name;
    ccColor3B bg, dark, mid, bright, glow;
    bool icicles;   // hanging spikes under platforms
    int jag;        // skyline jaggedness
    int bgShapes;   // 0 = saws, 1 = diamonds, 2 = mixed
};

const StyleDef STYLES[] = {
    //  name      bg                dark              mid               bright             glow
    {"Hell",   {28, 4, 4},       {110, 14, 8},     {210, 70, 12},    {255, 150, 25},   {255, 215, 110}, true,  3, 0},
    {"Heaven", {140, 195, 255},  {175, 205, 240},  {235, 242, 255},  {255, 255, 255},  {255, 238, 170}, false, 1, 1},
    {"Gold",   {40, 24, 0},      {120, 70, 0},     {220, 150, 10},   {255, 210, 45},   {255, 250, 180}, true,  2, 2},
    {"Neon",   {3, 16, 9},       {8, 72, 32},      {20, 175, 75},    {85, 255, 125},   {200, 255, 210}, false, 2, 1},
    {"Blue",   {5, 10, 62},      {14, 40, 150},    {30, 112, 230},   {85, 205, 255},   {205, 240, 255}, true,  2, 2},
};

StyleDef const& findStyle(std::string const& n) {
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
    s << ';';
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
    s << ';';
    return s.str();
}

std::string pulseTrig(float x, float y, int channel, ccColor3B c) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(2);
    s << "1,1006,2," << x << ",3," << y
      << ",7," << (int)c.r << ",8," << (int)c.g << ",9," << (int)c.b
      << ",45,0.00,46,0.05,47,0.30,48,0,52,0,51," << channel << ';';
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

// ------------------------------------------------------------------ runner
class AutoRateRunner : public CCNode {
public:
    static void start(LevelEditorLayer* lel) {
        auto r = new AutoRateRunner();
        if (r->init(lel)) {
            r->autorelease();
            lel->addChild(r);
        } else {
            delete r;
        }
    }

private:
    enum class Phase { Scan, Faces, Skyline, Hazards, BgShapes, Triggers, Place, Finish };
    struct Hz { int id; float x, y, rot; bool fx, fy; };
    struct Run { bool active = false; int cy = 0, start = 0, last = 0; };

    LevelEditorLayer* m_lel = nullptr;
    StyleDef const* m_style = &STYLES[0];
    float m_q = 0.7f;
    int m_bgStep = 9;
    float m_pulseStep = 240.f;
    bool m_bg = true, m_crown = true, m_pulses = true;
    std::string m_qName;

    Ref<Notification> m_notif;
    Phase m_phase = Phase::Scan;
    bool m_abort = false;
    size_t m_i = 0;

    // analysis results
    std::vector<Ref<GameObject>> m_objs;
    std::set<Cell> m_solid, m_occ;
    std::vector<Cell> m_rows;  // (row, column) sorted
    std::vector<Hz> m_hazards;
    std::map<int, int> m_decoChunk;
    std::set<int> m_usedColors;
    bool m_hasBounds = false;
    int m_minCx = 0, m_maxCx = 0, m_minCy = 0, m_maxCy = 0, m_baseCy = 0;
    int m_nSolid = 0, m_nHaz = 0, m_nDeco = 0, m_nAdded = 0;
    int m_ids[4] = {0, 0, 0, 0};  // dark, mid, bright, glow channels (free ones)

    // generation state
    std::mt19937 m_rng;
    float m_p1 = 0, m_p2 = 0;
    Run m_topRun, m_botRun;
    int m_sx = 0, m_sxEnd = 0, m_bx = 0;
    std::vector<std::string> m_out;
    size_t m_placed = 0;

    bool init(LevelEditorLayer* lel) {
        if (!CCNode::init()) return false;
        m_lel = lel;
        auto mod = Mod::get();
        m_style = &findStyle(mod->getSettingValue<std::string>("style"));
        m_qName = mod->getSettingValue<std::string>("quality");
        m_q = m_qName == "Fast" ? 0.4f : (m_qName == "Ultra" ? 1.0f : 0.7f);
        m_bgStep = (int)(14.f - 9.f * m_q);
        m_pulseStep = m_qName == "Fast" ? 360.f : (m_qName == "Ultra" ? 150.f : 240.f);
        m_bg = mod->getSettingValue<bool>("background");
        m_crown = mod->getSettingValue<bool>("crown");
        m_pulses = mod->getSettingValue<bool>("pulses");

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
    void add(Obj const& o) { m_out.push_back(toStr(o)); m_nAdded++; }

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
                case Phase::Faces:    name = "edges & crowns";    done = doFaces(over); break;
                case Phase::Skyline:  name = "background city";   done = doSkyline(over); break;
                case Phase::Hazards:  name = "hazard glow";       done = doHazards(over); break;
                case Phase::BgShapes: name = "background shapes"; done = doBgShapes(over); break;
                case Phase::Triggers: name = "colors & pulses";   done = doTriggers(); break;
                case Phase::Place:    name = "placing objects";   done = doPlace(over); break;
                case Phase::Finish:   finish(); return;
            }
            if (!done) {
                m_notif->setString(fmt::format("Auto Rate: {} ({} objects queued)", name, m_out.size()));
                return;
            }
            m_phase = (Phase)((int)m_phase + 1);
            if (m_abort && m_phase != Phase::Finish) m_phase = Phase::Finish;
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

    void scanOne(GameObject* o) {
        auto type = o->m_objectType;
        auto p = o->getPosition();
        int cx = (int)std::floor(p.x / 30.f);
        int cy = (int)std::floor(p.y / 30.f);

        // Parse the save string: it lists every color channel the object uses.
        auto kv = parseKV(std::string(o->getSaveString(m_lel).c_str()));
        for (int k : {21, 22, 23, 50, 51}) {
            auto it = kv.find(k);
            if (it != kv.end()) m_usedColors.insert(std::atoi(it->second.c_str()));
        }

        m_occ.insert({cx, cy});

        if (type == GameObjectType::Decoration) {
            m_nDeco++;
            m_decoChunk[(int)std::floor(cx / 10.f)]++;
            return;
        }

        bool solid = type == GameObjectType::Solid;
        bool haz = type == GameObjectType::Hazard;
        bool slope = type == GameObjectType::Slope;

        if (solid) {
            m_nSolid++;
            m_solid.insert({cx, cy});
        }
        if (haz) {
            m_nHaz++;
            float rot = kv.count(6) ? (float)std::atof(kv[6].c_str()) : 0.f;
            bool fx = kv.count(4) && kv[4] == "1";
            bool fy = kv.count(5) && kv[5] == "1";
            m_hazards.push_back({o->m_objectID, p.x, p.y, rot, fx, fy});
        }
        if (solid || haz || slope) {
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
        m_baseCy = std::max(0, m_minCy);
        for (auto& c : m_solid) m_rows.push_back({c.second, c.first});
        std::sort(m_rows.begin(), m_rows.end());

        // Find 4 FREE color channel ids (custom channels are 1..999).
        int found = 0;
        for (int id = 999; id >= 1 && found < 4; --id) {
            if (!m_usedColors.count(id)) m_ids[found++] = id;
        }

        m_p1 = rnd() * 6.28f;
        m_p2 = rnd() * 6.28f;
        m_sx = m_minCx - 8;
        m_sxEnd = m_maxCx + 12;
        m_bx = m_minCx;
    }

    // ---------------------------------------------------------------- faces
    void flushRun(Run& r, bool top) {
        if (!r.active) return;
        int len = r.last - r.start + 1;
        Obj o;
        o.id = 1;
        o.x = (r.start + r.last + 1) * 15.f;
        o.y = top ? (r.cy + 1) * 30.f - 1.8f : r.cy * 30.f + 1.8f;
        o.sx = (float)len;
        o.sy = 0.12f;
        o.base = m_ids[3];  // glow
        o.z = 3;
        o.zo = 20;
        add(o);
        r.active = false;
    }

    void feedRun(Run& r, bool exposed, int cx, int cy, bool top) {
        if (exposed) {
            if (r.active && r.cy == cy && r.last == cx - 1) {
                r.last = cx;
            } else {
                flushRun(r, top);
                r.active = true;
                r.cy = cy;
                r.start = r.last = cx;
            }
        } else {
            flushRun(r, top);
        }
    }

    void addCrown(int cx, int cy, bool edge) {
        float x = cx * 30.f + 15.f, top = (cy + 1) * 30.f;
        Obj o;
        o.id = 8;
        o.x = x;
        o.z = -1;
        o.zo = 5;
        if (edge && roll(0.35f + 0.6f * m_q, cx)) {
            o.y = top + 15.f;
            o.base = m_ids[2];
            add(o);
        } else if (roll(0.5f * m_q, cx)) {
            o.sx = o.sy = 0.5f;
            o.y = top + 7.5f;
            o.base = m_ids[1];
            add(o);
        }
    }

    void addIcicle(int cx, int cy) {
        float s = rnd() < 0.3f ? 1.0f : (rnd() < 0.5f ? 0.5f : 0.75f);
        Obj o;
        o.id = 8;
        o.fy = true;
        o.sx = o.sy = s;
        o.x = cx * 30.f + 15.f;
        o.y = cy * 30.f - 15.f * s;
        o.base = m_ids[1];
        o.z = -1;
        o.zo = 5;
        add(o);
    }

    bool doFaces(std::function<bool()> const& over) {
        while (m_i < m_rows.size()) {
            int cy = m_rows[m_i].first, cx = m_rows[m_i].second;
            ++m_i;

            bool topExp = !m_solid.count({cx, cy + 1});
            bool botExp = !m_solid.count({cx, cy - 1});
            feedRun(m_topRun, topExp, cx, cy, true);
            feedRun(m_botRun, botExp, cx, cy, false);

            if (m_crown && topExp && !m_occ.count({cx, cy + 1})) {
                bool edge = !m_solid.count({cx - 1, cy}) || !m_solid.count({cx + 1, cy});
                addCrown(cx, cy, edge);
            }
            if (m_style->icicles && botExp && !m_occ.count({cx, cy - 1}) && roll(0.3f * m_q, cx)) {
                addIcicle(cx, cy);
            }
            if (over()) return false;
        }
        flushRun(m_topRun, true);
        flushRun(m_botRun, false);
        m_i = 0;
        return true;
    }

    // -------------------------------------------------------------- skyline
    bool doSkyline(std::function<bool()> const& over) {
        if (!m_bg) return true;
        float baseY = m_baseCy * 30.f;
        while (m_sx < m_sxEnd) {
            int W = 2 + (int)(rnd() * 4.f);
            float n1 = 0.5f + 0.5f * std::sin(m_sx * 0.21f + m_p1);
            float n2 = 0.5f + 0.5f * std::sin(m_sx * 0.53f + m_p2);
            int H = 2 + (int)(n1 * 5.f + n2 * 3.f) + (int)(rnd() * (m_style->jag + 1));

            Obj b;
            b.id = 1;
            b.x = (m_sx * 2 + W) * 15.f;
            b.y = baseY + 15.f * H;
            b.sx = (float)W;
            b.sy = (float)H;
            b.base = m_ids[0];
            b.z = -3;
            b.zo = -8;
            add(b);

            float top = baseY + 30.f * H;
            for (int k = 0; k < W; ++k) {
                bool edge = (k == 0 || k == W - 1);
                Obj sp;
                sp.id = 8;
                sp.x = (m_sx + k) * 30.f + 15.f;
                sp.base = m_ids[1];
                sp.z = -3;
                sp.zo = -7;
                if (edge && roll(0.9f, m_sx)) {
                    sp.y = top + 15.f;
                    add(sp);
                } else if (roll(0.5f * m_q, m_sx)) {
                    sp.sx = sp.sy = 0.5f;
                    sp.y = top + 7.5f;
                    add(sp);
                }
            }

            // lit windows
            if (H >= 3 && roll(0.7f * m_q + 0.2f, m_sx)) {
                int n = 1 + (int)(rnd() * 3.f);
                for (int w = 0; w < n; ++w) {
                    Obj win;
                    win.id = 1;
                    win.sx = 0.2f;
                    win.sy = 0.3f;
                    win.x = m_sx * 30.f + 8.f + rnd() * (W * 30.f - 16.f);
                    win.y = baseY + 12.f + rnd() * (30.f * H - 30.f);
                    win.base = m_ids[2];
                    win.z = -3;
                    win.zo = -6;
                    add(win);
                }
            }

            m_sx += W;
            if (over()) return false;
        }
        m_i = 0;
        return true;
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
                o.base = m_ids[3];
                o.detail = m_ids[3];
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

    bool doBgShapes(std::function<bool()> const& over) {
        if (!m_bg) return true;
        while (m_bx < m_maxCx + 10) {
            int range = std::max(6, (m_maxCy - m_baseCy) + 8);
            int cy = m_baseCy + 3 + (int)(rnd() * range);
            if (roll(0.85f, m_bx) && areaFree(m_bx, cy)) {
                float x = m_bx * 30.f + 15.f, y = cy * 30.f + 15.f;
                bool diamond = m_style->bgShapes == 1 || (m_style->bgShapes == 2 && rnd() < 0.5f);
                if (!diamond) {
                    Obj o;
                    o.id = 88;  // large saw, rotates by itself
                    o.x = x;
                    o.y = y;
                    o.sx = o.sy = 1.5f + rnd() * 2.5f;
                    o.base = m_ids[0];
                    o.detail = m_ids[1];
                    o.z = -3;
                    o.zo = -4;
                    add(o);
                } else {
                    float s = 1.5f + rnd() * 2.5f;
                    Obj outer;
                    outer.id = 1;
                    outer.x = x;
                    outer.y = y;
                    outer.rot = 45.f;
                    outer.sx = outer.sy = s;
                    outer.base = m_ids[0];
                    outer.z = -3;
                    outer.zo = -4;
                    add(outer);
                    Obj inner = outer;
                    inner.sx = inner.sy = s * 0.55f;
                    inner.base = m_ids[3];  // glowing core
                    inner.zo = -3;
                    add(inner);
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
        if (m_bg) {
            col(1000, s.bg, 1.f, false);    // background
            col(1001, s.dark, 1.f, false);  // ground 1
            col(1009, s.dark, 1.f, false);  // ground 2
            col(1002, s.bright, 1.f, true); // line
        }
        col(m_ids[0], s.dark, 0.55f, false);
        col(m_ids[1], s.mid, 0.60f, false);
        col(m_ids[2], s.bright, 0.75f, false);
        col(m_ids[3], s.glow, 0.80f, true);

        if (m_pulses) {
            int idx = 0;
            float end = (m_maxCx + 1) * 30.f;
            for (float x = std::max(0, m_minCx) * 30.f; x < end; x += m_pulseStep, ++idx) {
                int target = idx % 2 == 0 ? m_ids[3] : m_ids[2];
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
        if (m_abort) {
            report = "Nothing to decorate: no blocks, slopes or hazards were found in this level.";
        } else {
            report = fmt::format(
                "Style: <cy>{}</c>  |  Quality: <cy>{}</c>\n"
                "Analysed: {} solid blocks, {} hazards, {} existing decoration objects\n"
                "Free color channels used: <cg>{}, {}, {}, {}</c>\n"
                "Added: <cg>{}</c> objects + triggers\n\n"
                "Undo with Ctrl+Z (it may take a few presses).",
                m_style->name, m_qName, m_nSolid, m_nHaz, m_nDeco,
                m_ids[0], m_ids[1], m_ids[2], m_ids[3], m_nAdded);
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

        auto win = CCDirector::get()->getWinSize();
        auto spr = ButtonSprite::create("Auto Rate");
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AutoRatePauseLayer::onAutoRate));
        btn->setID("auto-rate-button"_spr);

        auto menu = CCMenu::create();
        menu->setID("auto-rate-menu"_spr);
        menu->setPosition({win.width - 70.f, 30.f});
        menu->addChild(btn);
        this->addChild(menu);
        return true;
    }

    void onAutoRate(CCObject*) {
        auto mod = Mod::get();
        auto style = mod->getSettingValue<std::string>("style");
        auto quality = mod->getSettingValue<std::string>("quality");
        auto lel = m_editorLayer;

        createQuickPopup(
            "Auto Rate",
            fmt::format(
                "Style: <cy>{}</c>\nQuality: <cy>{}</c>\n\n"
                "Analyse the whole level and decorate it?\n"
                "(Change the options in the mod's settings. Undo: Ctrl+Z)",
                style, quality),
            "Cancel", "Start",
            [this, lel](FLAlertLayer*, bool ok) {
                if (!ok) return;
                this->onResume(nullptr);
                AutoRateRunner::start(lel);
            });
    }
};
