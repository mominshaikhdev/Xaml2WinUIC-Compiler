// extensions.ixx — RUNTIME control-registry extensions for xaml2winuic
// Third-party and future controls are described in JSON files and loaded at
// startup — NO recompile of the compiler is ever required to support a new
// control. The file format is shared with winuic (winui.json / --control):

// {
// "backend": "winui3",
// "controls": [
// {
// "abstract_name": "RadialGauge",          // unique abstract name
// "platform_type": "RadialGauge",          // name used in XAML
// "platform_namespace": "MyLib.Controls",  // WinRT namespace
// "category": "third-party",
// "accepts_children": true,
// "accepts_text_content": false,
// "properties": [
// { "abstract_name": "value", "platform_setter": "Value",
// "transform": "double" }
// ],
// "events": [
// { "abstract_name": "onValueChanged", "platform_event": "ValueChanged" }
// ],
// "xaml": {                                 // optional XAML-name overrides
// "properties": { "Maximum": "max" },
// "events":     { "ValueChanged": "onValueChanged" }
// }
// }
// ],
// "xamlEvents":  [ "Tapped" ],                  // global XAML event names
// "universalMods": [ { "xaml": "IsSpinning", "winui": "spin" } ],
// "fragFamilies": [ "CustomMetadata" ]          // verbatim-fragment families
// }

// The winuic keys (abstract_name, platform_type, platform_namespace, ...)
// are the same here, so ONE file serves BOTH compilers. The xamlEvents /
// universalMods / fragFamilies keys are xaml2winuic-only and ignored by
// winuic (its reader skips unknown keys).

// Convention (used when no explicit "xaml" map is given):
// • XAML property attribute = upperFirst(abstract_name)
// ("value" → Value="...")
// • XAML event attribute    = platform_event, or "on" stripped from
// abstract_name ("onValueChanged" → ValueChanged="...")
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <functional>

export module xamlconv.ext;

export namespace xamlconv::ext {

// Runtime extension tables. Populated once at startup from winui.json
// (next to the executable) and every --control <file> overlay. All lookups
// are read-only after loading, so the parallel conversion workers are safe.
struct ExtensionRegistry {
    std::unordered_set<std::string> controls;       // XAML element names
    std::unordered_set<std::string> properties;     // "Control:prop" (winui camelCase)
    std::unordered_set<std::string> events;         // "Control:event" (winui on* names)
    std::unordered_set<std::string> xaml_events;    // global XAML event attribute names
    std::vector<std::pair<std::string, std::string>> universal_mods; // {XAML attr, winui mod}
    std::unordered_set<std::string> frag_families;  // verbatim-fragment element names
    std::unordered_set<std::string> text_capable;   // controls accepting a positional text arg

    // explicit XAML-name → winui-name maps, keyed by control name
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> xaml_props;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> xaml_evts;
};

inline auto registry() -> ExtensionRegistry& {
    static ExtensionRegistry r;
    return r;
}

// Lookup helpers (used by convert.ixx)

inline auto is_ext_control(std::string_view name) -> bool {
    return registry().controls.contains(std::string(name));
}

inline auto is_ext_property(std::string_view ctrl, std::string_view prop) -> bool {
    std::string key(ctrl);
    key += ':';
    key += prop;
    return registry().properties.contains(key);
}

inline auto is_ext_xaml_event(std::string_view name) -> bool {
    return registry().xaml_events.contains(std::string(name));
}

inline auto is_ext_ctrl_event(std::string_view ctrl, std::string_view name) -> bool {
    std::string key(ctrl);
    key += ':';
    key += name;
    if (registry().events.contains(key)) return true;
    // declared events are also reachable by their XAML-side name
    // (platform_event, or the "on" stripped from the abstract name)
    auto it = registry().xaml_evts.find(std::string(ctrl));
    if (it == registry().xaml_evts.end()) return false;
    return it->second.contains(std::string(name));
}

// XAML attribute → .winui property for extension controls. Returns the
// winui property name, or nullopt when the control does not declare it.
inline auto ext_xaml_prop(std::string_view ctrl, std::string_view xaml_name)
    -> std::optional<std::string> {
    auto const& reg = registry();
    auto it = reg.xaml_props.find(std::string(ctrl));
    if (it == reg.xaml_props.end()) return std::nullopt;
    auto pit = it->second.find(std::string(xaml_name));
    if (pit == it->second.end()) return std::nullopt;
    return pit->second;
}

// XAML event name → .winui event modifier for extension controls.
inline auto ext_xaml_event(std::string_view ctrl, std::string_view xaml_name)
    -> std::optional<std::string> {
    auto const& reg = registry();
    auto it = reg.xaml_evts.find(std::string(ctrl));
    if (it == reg.xaml_evts.end()) return std::nullopt;
    auto eit = it->second.find(std::string(xaml_name));
    if (eit == it->second.end()) return std::nullopt;
    return eit->second;
}

inline auto is_ext_frag(std::string_view name) -> bool {
    return registry().frag_families.contains(std::string(name));
}

// True when the control declared accepts_text_content: true — its inner
// text becomes a positional string argument ("LedStrip(\"Top banner\")").
inline auto accepts_text(std::string_view ctrl) -> bool {
    return registry().text_capable.contains(std::string(ctrl));
}

// JSON loading
// Tiny hand-rolled reader (no external dependencies), mirroring the winuic
// loader so the identical file works for both compilers.

inline auto load_extensions_from_json(std::string_view json) -> bool {
    auto skip_ws = [](std::string_view s, std::size_t p) -> std::size_t {
        while (p < s.size() && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) ++p;
        return p;
    };

    auto read_string = [](std::string_view s, std::size_t p)
        -> std::pair<std::string, std::size_t> {
        if (p >= s.size() || s[p] != '"') return {"", std::string_view::npos};
        ++p;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                ++p;
                switch (s[p]) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    default:   out += s[p]; break;
                }
            } else {
                out += s[p];
            }
            ++p;
        }
        if (p < s.size()) ++p;
        return {out, p};
    };

    // Find value for a key inside an object scope; returns a string value or
    // the position of a { } / [ ] block (as a single-char marker plus pos).
    auto find_key = [&](std::string_view s, std::size_t obj_start, std::string_view key)
        -> std::pair<std::string, std::size_t> {
        std::size_t p = obj_start;
        int depth = 0;
        while (p < s.size()) {
            p = skip_ws(s, p);
            if (p >= s.size()) break;
            char c = s[p];
            if (c == '{' || c == '[') { ++depth; ++p; continue; }
            if (c == '}' || c == ']') {
                if (depth == 0) break;
                --depth; ++p; continue;
            }
            if (c == '"' && depth == 0) {
                auto [kname, kend] = read_string(s, p);
                if (kend == std::string_view::npos) break;
                p = skip_ws(s, kend);
                if (p < s.size() && s[p] == ':') ++p;
                p = skip_ws(s, p);
                if (kname == key) {
                    if (p < s.size() && s[p] == '"') {
                        auto [val, vend] = read_string(s, p);
                        return {val, vend};
                    }
                    return {std::string(s.substr(p, 1)), p};
                }
                if (p < s.size()) {
                    if (s[p] == '"') { auto [_v, _e] = read_string(s, p); p = _e; }
                    else if (s[p] == '{' || s[p] == '[') {
                        int d2 = 0;
                        while (p < s.size()) {
                            if (s[p] == '{' || s[p] == '[') ++d2;
                            else if (s[p] == '}' || s[p] == ']') { --d2; if (d2 == 0) { ++p; break; } }
                            ++p;
                        }
                    } else {
                        while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']') ++p;
                    }
                }
            } else {
                ++p;
            }
        }
        return {"", std::string_view::npos};
    };

    // Iterate over array elements; cb(obj_start, obj_end) per object.
    auto for_each_array_obj = [&](std::string_view s, std::size_t arr_pos,
        std::function<void(std::size_t, std::size_t)> cb) -> void {
        std::size_t p = skip_ws(s, arr_pos);
        if (p >= s.size() || s[p] != '[') return;
        ++p;
        while (p < s.size()) {
            p = skip_ws(s, p);
            if (p >= s.size() || s[p] == ']') break;
            if (s[p] != '{') { ++p; continue; }
            std::size_t obj_start = p;
            int d = 0;
            std::size_t obj_end = p;
            while (obj_end < s.size()) {
                if (s[obj_end] == '{') ++d;
                else if (s[obj_end] == '}') { --d; if (d == 0) { ++obj_end; break; } }
                ++obj_end;
            }
            cb(obj_start, obj_end);
            p = obj_end;
            p = skip_ws(s, p);
            if (p < s.size() && s[p] == ',') ++p;
        }
    };

    auto upper_first = [](std::string s) -> std::string {
        if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 32);
        return s;
    };

    std::size_t root = skip_ws(json, 0);
    if (root >= json.size() || json[root] != '{') return false;

    auto& reg = registry();

    // "controls" array
    std::size_t p = root + 1;
    int depth = 0;
    std::size_t ctrl_arr_pos = std::string_view::npos;
    while (p < json.size()) {
        p = skip_ws(json, p);
        if (p >= json.size()) break;
        char c = json[p];
        if (c == '{' || c == '[') { ++depth; ++p; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0) break; --depth; ++p; continue;
        }
        if (c == '"' && depth == 0) {
            auto [kname, kend] = read_string(json, p);
            if (kend == std::string_view::npos) break;
            p = skip_ws(json, kend);
            if (p < json.size() && json[p] == ':') ++p;
            p = skip_ws(json, p);
            if (kname == "controls") { ctrl_arr_pos = p; break; }
            if (p < json.size()) {
                if (json[p] == '"') { auto [_v, _e] = read_string(json, p); p = _e; }
                else if (json[p] == '{' || json[p] == '[') {
                    int d2 = 0;
                    while (p < json.size()) {
                        if (json[p] == '{' || json[p] == '[') ++d2;
                        else if (json[p] == '}' || json[p] == ']') { --d2; if (d2 == 0) { ++p; break; } }
                        ++p;
                    }
                } else { while (p < json.size() && json[p] != ',' && json[p] != '}' && json[p] != ']') ++p; }
            }
        } else { ++p; }
    }

    if (ctrl_arr_pos != std::string_view::npos) {
        for_each_array_obj(json, ctrl_arr_pos, [&](std::size_t os, std::size_t) {
            std::size_t inner = os + 1;
            auto [abstract_name, _a] = find_key(json, inner, "abstract_name");
            auto [platform_type, _b] = find_key(json, inner, "platform_type");
            if (abstract_name.empty() && platform_type.empty()) return;
            std::string ctrl = platform_type.empty() ? abstract_name : platform_type;
            reg.controls.insert(ctrl);

            std::vector<std::string> prop_abstracts;
            std::vector<std::string> evt_abstracts;
            std::vector<std::string> evt_platforms;

            // properties / events arrays (shared schema with winuic)
            std::size_t q = inner;
            int d = 0;
            while (q < json.size()) {
                q = skip_ws(json, q);
                if (q >= json.size()) break;
                char c = json[q];
                if (c == '{' || c == '[') { ++d; ++q; continue; }
                if (c == '}' || c == ']') {
                    if (d == 0) break; --d; ++q; continue;
                }
                if (c == '"' && d == 0) {
                    auto [kn, ke] = read_string(json, q);
                    if (ke == std::string_view::npos) break;
                    q = skip_ws(json, ke);
                    if (q < json.size() && json[q] == ':') ++q;
                    q = skip_ws(json, q);
                    if (kn == "properties") {
                        for_each_array_obj(json, q, [&](std::size_t ps, std::size_t) {
                            auto [pan, _p1] = find_key(json, ps + 1, "abstract_name");
                            auto [pset, _p2] = find_key(json, ps + 1, "platform_setter");
                            if (pan.empty()) return;
                            prop_abstracts.push_back(pan);
                            reg.properties.insert(ctrl + ":" + pan);
                            (void)pset;
                        });
                    } else if (kn == "events") {
                        for_each_array_obj(json, q, [&](std::size_t es, std::size_t) {
                            auto [ean, _e1] = find_key(json, es + 1, "abstract_name");
                            auto [epev, _e2] = find_key(json, es + 1, "platform_event");
                            if (ean.empty()) return;
                            evt_abstracts.push_back(ean);
                            evt_platforms.push_back(epev);
                            reg.events.insert(ctrl + ":" + ean);
                        });
                    } else if (kn == "accepts_text_content") {
                        // scalar true/false flag — text becomes a positional arg
                        std::string flag;
                        if (q < json.size() && json[q] == '"') {
                            auto [fv, fe] = read_string(json, q);
                            if (fe != std::string_view::npos) flag = fv;
                        } else {
                            std::size_t fb = q;
                            while (fb < json.size() && json[fb] != ',' && json[fb] != '}'
                                   && json[fb] != ']') ++fb;
                            flag = std::string(json.substr(q, fb - q));
                        }
                        if (flag == "true") reg.text_capable.insert(ctrl);
                    } else if (kn == "xaml") {
                        // optional per-control XAML-name overrides: scan the
                        // object [q .. matching }] for "properties"/"events"
                        // maps of {XAML-name → winui-name} pairs.
                        std::size_t xaml_end = q;
                        {
                            int od = 0;
                            while (xaml_end < json.size()) {
                                if (json[xaml_end] == '{') ++od;
                                else if (json[xaml_end] == '}') { --od; if (od == 0) { ++xaml_end; break; } }
                                ++xaml_end;
                            }
                        }
                        std::size_t pp = q + 1;
                        int pd = 0;
                        std::string pending_map_key;
                        while (pp < xaml_end) {
                            pp = skip_ws(json, pp);
                            if (pp >= xaml_end) break;
                            char pc = json[pp];
                            if (pc == '{' || pc == '[') { ++pd; ++pp; continue; }
                            if (pc == '}' || pc == ']') { if (pd == 0) break; --pd; ++pp; continue; }
                            if (pc == '"' && pd == 0) {
                                auto [k3, k3e] = read_string(json, pp);
                                if (k3e == std::string_view::npos) break;
                                pp = skip_ws(json, k3e);
                                if (pp < xaml_end && json[pp] == ':') ++pp;
                                pp = skip_ws(json, pp);
                                if (pp < xaml_end && json[pp] == '{') {
                                    // value is a name→name map
                                    std::size_t mstart = pp;
                                    int md = 0;
                                    std::size_t mend = pp;
                                    while (mend < xaml_end) {
                                        if (json[mend] == '{') ++md;
                                        else if (json[mend] == '}') { --md; if (md == 0) { ++mend; break; } }
                                        ++mend;
                                    }
                                    if (k3 == "properties" || k3 == "events") {
                                        pending_map_key = k3;
                                        std::size_t qq = mstart + 1;
                                        int qd = 0;
                                        while (qq < mend) {
                                            qq = skip_ws(json, qq);
                                            if (qq >= mend) break;
                                            char qc = json[qq];
                                            if (qc == '{' || qc == '[') { ++qd; ++qq; continue; }
                                            if (qc == '}' || qc == ']') { if (qd == 0) break; --qd; ++qq; continue; }
                                            if (qc == '"' && qd == 0) {
                                                auto [k4, k4e] = read_string(json, qq);
                                                if (k4e == std::string_view::npos) break;
                                                qq = skip_ws(json, k4e);
                                                if (qq < mend && json[qq] == ':') ++qq;
                                                qq = skip_ws(json, qq);
                                                auto [v4, v4e] = read_string(json, qq);
                                                if (!k4.empty() && !v4.empty()) {
                                                    if (pending_map_key == "properties") reg.xaml_props[ctrl][k4] = v4;
                                                    else reg.xaml_evts[ctrl][k4] = v4;
                                                    qq = v4e;
                                                } else { ++qq; }
                                            } else { ++qq; }
                                        }
                                    }
                                    pp = mend;
                                    continue;
                                }
                                // scalar value — skip
                                if (pp < xaml_end && json[pp] == '"') { auto [_v, _e] = read_string(json, pp); pp = _e; }
                                else if (pp < xaml_end && (json[pp] == '{' || json[pp] == '[')) {
                                    int d3 = 0;
                                    while (pp < xaml_end) {
                                        if (json[pp] == '{' || json[pp] == '[') ++d3;
                                        else if (json[pp] == '}' || json[pp] == ']') { --d3; if (d3 == 0) { ++pp; break; } }
                                        ++pp;
                                    }
                                } else { while (pp < xaml_end && json[pp] != ',' && json[pp] != '}' && json[pp] != ']') ++pp; }
                            } else { ++pp; }
                        }
                    }
                    // skip value
                    if (q < json.size()) {
                        if (json[q] == '"') { auto [_v, _e] = read_string(json, q); q = _e; }
                        else if (json[q] == '{' || json[q] == '[') {
                            int d2 = 0;
                            while (q < json.size()) {
                                if (json[q] == '{' || json[q] == '[') ++d2;
                                else if (json[q] == '}' || json[q] == ']') { --d2; if (d2 == 0) { ++q; break; } }
                                ++q;
                            }
                        } else { while (q < json.size() && json[q] != ',' && json[q] != '}' && json[q] != ']') ++q; }
                    }
                } else { ++q; }
            }

            // auto-derive XAML-name maps (convention, unless overridden)
            for (auto const& pa : prop_abstracts) {
                std::string xaml_attr = upper_first(pa);
                auto& pm = reg.xaml_props[ctrl];
                if (!pm.contains(xaml_attr)) pm[xaml_attr] = pa;
            }
            for (std::size_t i = 0; i < evt_abstracts.size(); ++i) {
                std::string xaml_evt;
                if (!evt_platforms[i].empty()) {
                    xaml_evt = evt_platforms[i];
                } else {
                    std::string n = evt_abstracts[i];
                    xaml_evt = n.starts_with("on") ? n.substr(2) : n;
                    if (xaml_evt.empty()) xaml_evt = evt_abstracts[i];
                }
                auto& em = reg.xaml_evts[ctrl];
                if (!em.contains(xaml_evt)) em[xaml_evt] = evt_abstracts[i];
            }
        });
    }

    // top-level "xamlEvents" array
    {
        std::size_t q = root + 1;
        int d = 0;
        while (q < json.size()) {
            q = skip_ws(json, q);
            if (q >= json.size()) break;
            char c = json[q];
            if (c == '{' || c == '[') { ++d; ++q; continue; }
            if (c == '}' || c == ']') {
                if (d == 0) break; --d; ++q; continue;
            }
            if (c == '"' && d == 0) {
                auto [kn, ke] = read_string(json, q);
                if (ke == std::string_view::npos) break;
                q = skip_ws(json, ke);
                if (q < json.size() && json[q] == ':') ++q;
                q = skip_ws(json, q);
                if (kn == "xamlEvents" && q < json.size() && json[q] == '[') {
                    std::size_t pp = q;
                    while (pp < json.size()) {
                        pp = skip_ws(json, pp);
                        if (pp >= json.size() || json[pp] == ']') break;
                        if (json[pp] == '"') {
                            auto [v, ve] = read_string(json, pp);
                            if (!v.empty()) reg.xaml_events.insert(v);
                            pp = ve;
                        } else { ++pp; }
                    }
                    break;
                }
                if (q < json.size()) {
                    if (json[q] == '"') { auto [_v, _e] = read_string(json, q); q = _e; }
                    else if (json[q] == '{' || json[q] == '[') {
                        int d2 = 0;
                        while (q < json.size()) {
                            if (json[q] == '{' || json[q] == '[') ++d2;
                            else if (json[q] == '}' || json[q] == ']') { --d2; if (d2 == 0) { ++q; break; } }
                            ++q;
                        }
                    } else { while (q < json.size() && json[q] != ',' && json[q] != '}' && json[q] != ']') ++q; }
                }
            } else { ++q; }
        }
    }

    // top-level "universalMods" array
    {
        std::size_t q = root + 1;
        int d = 0;
        std::size_t arr_pos = std::string_view::npos;
        while (q < json.size()) {
            q = skip_ws(json, q);
            if (q >= json.size()) break;
            char c = json[q];
            if (c == '{' || c == '[') { ++d; ++q; continue; }
            if (c == '}' || c == ']') {
                if (d == 0) break; --d; ++q; continue;
            }
            if (c == '"' && d == 0) {
                auto [kn, ke] = read_string(json, q);
                if (ke == std::string_view::npos) break;
                q = skip_ws(json, ke);
                if (q < json.size() && json[q] == ':') ++q;
                q = skip_ws(json, q);
                if (kn == "universalMods") { arr_pos = q; break; }
                if (q < json.size()) {
                    if (json[q] == '"') { auto [_v, _e] = read_string(json, q); q = _e; }
                    else if (json[q] == '{' || json[q] == '[') {
                        int d2 = 0;
                        while (q < json.size()) {
                            if (json[q] == '{' || json[q] == '[') ++d2;
                            else if (json[q] == '}' || json[q] == ']') { --d2; if (d2 == 0) { ++q; break; } }
                            ++q;
                        }
                    } else { while (q < json.size() && json[q] != ',' && json[q] != '}' && json[q] != ']') ++q; }
                }
            } else { ++q; }
        }
        if (arr_pos != std::string_view::npos) {
            for_each_array_obj(json, arr_pos, [&](std::size_t os, std::size_t) {
                auto [xaml_name, _a] = find_key(json, os + 1, "xaml");
                auto [winui_name, _b] = find_key(json, os + 1, "winui");
                if (!xaml_name.empty() && !winui_name.empty()) {
                    registry().universal_mods.push_back({xaml_name, winui_name});
                }
            });
        }
    }

    // top-level "fragFamilies" array
    {
        std::size_t q = root + 1;
        int d = 0;
        while (q < json.size()) {
            q = skip_ws(json, q);
            if (q >= json.size()) break;
            char c = json[q];
            if (c == '{' || c == '[') { ++d; ++q; continue; }
            if (c == '}' || c == ']') {
                if (d == 0) break; --d; ++q; continue;
            }
            if (c == '"' && d == 0) {
                auto [kn, ke] = read_string(json, q);
                if (ke == std::string_view::npos) break;
                q = skip_ws(json, ke);
                if (q < json.size() && json[q] == ':') ++q;
                q = skip_ws(json, q);
                if (kn == "fragFamilies" && q < json.size() && json[q] == '[') {
                    std::size_t pp = q;
                    while (pp < json.size()) {
                        pp = skip_ws(json, pp);
                        if (pp >= json.size() || json[pp] == ']') break;
                        if (json[pp] == '"') {
                            auto [v, ve] = read_string(json, pp);
                            if (!v.empty()) reg.frag_families.insert(v);
                            pp = ve;
                        } else { ++pp; }
                    }
                    break;
                }
                if (q < json.size()) {
                    if (json[q] == '"') { auto [_v, _e] = read_string(json, q); q = _e; }
                    else if (json[q] == '{' || json[q] == '[') {
                        int d2 = 0;
                        while (q < json.size()) {
                            if (json[q] == '{' || json[q] == '[') ++d2;
                            else if (json[q] == '}' || json[q] == ']') { --d2; if (d2 == 0) { ++q; break; } }
                            ++q;
                        }
                    } else { while (q < json.size() && json[q] != ',' && json[q] != '}' && json[q] != ']') ++q; }
                }
            } else { ++q; }
        }
    }

    return true;
}

} // namespace xamlconv::ext
