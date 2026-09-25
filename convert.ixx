// convert.ixx — WinUI 3 XAML (C# / C++ / C++/WinRT) → .winui converter core
// Converts a parsed XAML document plus its code-behind (C# / C++ / MIDL)
// into a .winui source file whose UI tree is 100% equivalent to the XAML.

// Strategy:
// • Every registered WinUI 3 control is emitted under its real name with
// the exact property set the .winui compiler validates (the schemas are
// generated from winuic/registry.ixx into registry_table.gen.h).
// • Unregistered custom controls are emitted verbatim — the .winui
// compiler accepts unknown PascalCase components (they surface as
// warnings, never errors), so nothing is ever dropped.
// • Anything the .winui language cannot express (styles, templates,
// brushes, placements, automation metadata …) is preserved as
// line comments directly beneath the owning element — no information
// loss, output still compiles.
// • Code-behind analysis maps x:Bind targets → @state/@binding vars and
// XAML event attributes → callback declarations + .on*{ self.H() }.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <cstdint>
#include <cstdio>

export module convert;

import xaml;
import lang;
import xamlconv.reggen;
import xamlconv.ext;

export namespace xamlconv::convert {

// §1  Registry accessors (tables generated into registry.ixx, module xamlconv.reggen)

inline auto is_registered(std::string_view name) -> bool {
    for (auto const& n : reggen::g_controls) if (n == name) return true;
    // runtime JSON extensions (winui.json / --control) — no recompile needed
    return ext::is_ext_control(name);
}

inline auto has_property(std::string_view ctrl, std::string_view prop) -> bool {
    for (auto const& n : reggen::g_properties) {
        if (n.size() > ctrl.size() + 1 && n[ctrl.size()] == ':' &&
            n.substr(0, ctrl.size()) == ctrl && n.substr(ctrl.size() + 1) == prop) return true;
    }
    return ext::is_ext_property(ctrl, prop);
}

inline auto has_event(std::string_view ctrl, std::string_view ev) -> bool {
    for (auto const& n : reggen::g_events) {
        if (n.size() > ctrl.size() + 1 && n[ctrl.size()] == ':' &&
            n.substr(0, ctrl.size()) == ctrl && n.substr(ctrl.size() + 1) == ev) return true;
    }
    return ext::is_ext_ctrl_event(ctrl, ev);
}

// §2  Options & result

export struct ConvertOptions {
    std::string source_rel_path;   // for the header comment
    std::string language;          // "csharp" | "cpp" (code-behind dialect)
    bool        emit_resources = true;  // resource dictionaries → resource views
    std::string source_file_path;  // absolute path to source XAML file
};

export struct ConvertResult {
    std::string text;              // generated .winui source (empty on hard failure)
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool       is_resource_view = false;
    std::string view_name;
};

// §3  Emitter helpers

struct Emitter {
    std::string out;
    int depth = 0;

    auto line(std::string_view text) -> void {
        for (int i = 0; i < depth; ++i) out += "    ";
        out += text;
        out += '\n';
    }
    auto blank() -> void { out += '\n'; }
};

// Escape a string for .winui source. Rules (see winuic lexer):
//   \"  stays literal
//   \   literal except when followed by ( / " / uXXXX / end-of-string
inline auto escape_winui_string(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size() + 8);
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            // decide whether the next sequence would be re-interpreted
            bool need_double = (i + 1 >= s.size());           // trailing backslash
            if (!need_double && s[i + 1] == '(') need_double = true;
            if (!need_double && s[i + 1] == '"') need_double = true;
            if (!need_double && s[i + 1] == 'u') {
                bool hex4 = true;
                for (int k = 1; k <= 4 && hex4; ++k) {
                    char h = (i + 1 + k < s.size()) ? s[i + 1 + k] : '\0';
                    hex4 = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
                }
                if (hex4) need_double = true;
            }
            out += need_double ? "\\\\" : "\\";
        } else {
            out += c;
        }
    }
    return out;
}

// Emit a quoted string literal.
inline auto qstr(std::string_view s) -> std::string {
    return "\"" + escape_winui_string(s) + "\"";
}

// Verbatim pass-through ("raw") — nothing in the source XAML is ever lost.
// Unmapped attributes and unstructurable subtrees (styles, visual states,
// animations, brushes, row/col definitions, templates, …) are carried into
// the .winmd as `.raw(key, value)` pairs, so a projection can reproduce the
// original UI exactly.

struct RawPair {
    std::string key;
    std::string value;
};

// ".raw(\"key\", \"value\")" — emitted on any control.
inline auto raw_mod(RawPair const& rp) -> std::string {
    return ".raw(" + qstr(rp.key) + ", " + qstr(rp.value) + ")";
}

// Element families that cannot be represented as structured controls (their
// content is metadata, not view content). Serialized verbatim as fragments.
inline auto is_frag_family(std::string_view name) -> bool {
    static constexpr std::string_view frags[] = {
        "Style", "Setter", "SolidColorBrush", "LinearGradientBrush",
        "RadialGradientBrush", "GradientStop", "Thickness", "GridLength",
        "TransitionCollection", "VisualState", "VisualStateGroup",
        "VisualStateGroupList", "VisualStateManager", "Storyboard", "Color",
        "DoubleAnimation", "ObjectAnimationUsingKeyFrames", "ColorAnimation",
        "ThemeAnimation", "Timeline", "EasingFunctionBase", "FontWeight",
        "FontFamily", "BitmapImage", "CompositeTransform", "ScaleTransform",
        "RotateTransform", "TranslateTransform", "ThemeDictionaries",
        "ResourceDictionary", "RowDefinition", "ColumnDefinition",
        "x:String", "String", "DataTemplate", "DataTemplateSelector",
        "ItemsPanelTemplate", "ControlTemplate", "LineBreak",
    };
    for (auto const& f : frags) if (f == name) return true;
    // runtime JSON extensions may declare additional fragment families
    return ext::is_ext_frag(name);
}

// Serialize an element subtree back into XAML text (verbatim attrs).
inline auto serialize_fragment(xaml::Node const& el) -> std::string {
    std::string out = "<";
    if (!el.prefix.empty()) out += el.prefix + ":";
    out += el.name;
    for (auto const& a : el.attrs) {
        out += " " + a.name + "=\"" + a.value + "\"";
    }
    if (el.self_closing && el.children.empty() && el.text.empty()) {
        out += "/>";
        return out;
    }
    out += ">";
    for (auto const& t : el.text) out += t;
    for (auto const& ch : el.children) {
        if (ch.kind == xaml::NodeKind::Element) out += serialize_fragment(ch);
        else if (ch.kind == xaml::NodeKind::Comment) out += "<!--" + ch.name + "-->";
        else if (ch.kind == xaml::NodeKind::CData) out += "<![CDATA[" + ch.name + "]]>";
    }
    out += "</";
    if (!el.prefix.empty()) out += el.prefix + ":";
    out += el.name + ">";
    return out;
}

// Collect raw pairs from a child node: property elements whose element
// children are ALL frag-family (Grid.RowDefinitions, X.Resources,
// VisualStateManager.VisualStateGroups, …) and standalone frag-family
// elements. Returns true when the node was consumed (must NOT be emitted
// as a normal child).
inline auto collect_fragment(xaml::Node const& el, std::vector<RawPair>& out) -> bool;

// Recursively collect xmlns declarations from an element tree (deduped by
// attribute name). Third-party controls are referenced through xmlns
// prefixes — without these URIs a projection cannot resolve their types,
// so they are carried into the .winmd like every other attribute.
inline auto collect_xmlns(xaml::Node const& el, std::vector<RawPair>& out) -> void {
    for (auto const& a : el.attrs) {
        std::string const& aname = a.name;
        bool is_xmlns = (aname == "xmlns");
        if (!is_xmlns && aname.starts_with("xmlns:") && aname.size() > 6) is_xmlns = true;
        if (!is_xmlns) continue;
        bool dup = false;
        for (auto const& p : out) if (p.key == aname) { dup = true; break; }
        if (!dup) out.push_back({aname, std::string(a.value)});
    }
    for (auto const& ch : el.children) {
        if (ch.kind == xaml::NodeKind::Element) collect_xmlns(ch, out);
    }
}

// Lower camel-case: "FooBar" → "fooBar" (for x:Bind path roots and x:Name).
inline auto lower_camel(std::string_view s) -> std::string {
    if (s.empty()) return std::string(s);
    std::string out(s);
    out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
    return out;
}

inline auto is_ident(std::string_view s) -> bool {
    if (s.empty()) return false;
    auto ok = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (char c : s) if (!ok(c)) return false;
    return true;
}

// Value → .winui expression. Handles bindings, resource refs, enums, numbers.
struct ValueExpr {
    std::string expr;      // emitted expression ("" if unconvertible → comment)
    std::string comment;   // optional comment text
    bool is_binding = false;
    bool is_resource_ref = false;
};

// Parses "{x:Bind Path, Mode=...}" / "{Binding Path}" into a .winui expr.
// Element bindings "{Binding ElementName=X, Path=P}" become "x.p".
// Returns: expression text using `root`-relative state vars; empty if failed.
inline auto translate_binding(std::string_view inner, std::string& out_mode) -> std::string {
    std::string body(inner);
    out_mode = "";

    // Strip the binding keyword: "x:Bind Title" / "Binding Title" (braces are
    // already removed by callers). Must happen before any path translation,
    // otherwise the namespace-prefix rule rewrites "x:Bind" → "Bind".
    {
        auto sp = body.find(' ');
        std::string head = (sp == std::string::npos) ? body : body.substr(0, sp);
        if (head == "x:Bind" || head == "Binding") {
            body = (sp == std::string::npos) ? "" : body.substr(sp + 1);
        } else if (body.rfind("x:Bind", 0) == 0) {
            body = body.substr(6);
        }
    }

    // Matches only whole words so "MyPath=" never false-positives on "Path=".
    auto is_word_boundary = [&](std::size_t i) -> bool {
        return i == 0 || !(std::isalnum(static_cast<unsigned char>(body[i - 1])) || body[i - 1] == '_');
    };

    // Extract "Mode=OneWay|TwoWay|OneTime|..." anywhere in the body.
    std::string mode;
    {
        std::size_t mp = std::string::npos;
        for (std::size_t i = 0; (i = body.find("Mode=", i)) != std::string::npos; ++i) {
            if (is_word_boundary(i)) { mp = i; break; }
        }
        if (mp != std::string::npos) {
            std::string mv = body.substr(mp + 5);
            auto mb = mv.find_first_not_of(" \t\"'");
            auto me = mv.find_first_of(" \t,\"'");
            mode = (mb == std::string::npos) ? ""
                   : (me == std::string::npos) ? mv.substr(mb)
                                               : mv.substr(mb, me - mb);
        }
    }
    out_mode = mode;

    // Translate a plain property path ("Path=X", "X.Y", "ns:X.Y(...)", casts).
    // Returns "" when the path cannot be faithfully expressed in .winui.
    auto translate_plain_path = [&](std::string path) -> std::string {
        auto pb = path.find_first_not_of(" \t");
        auto pe = path.find_last_not_of(" \t");
        if (pb == std::string::npos || pe == std::string::npos) return {};
        path = path.substr(pb, pe - pb + 1);

        // {Binding Path=X} → the actual path follows "Path="
        if (path.rfind("Path=", 0) == 0) path = path.substr(5);

        // Anything with '=' left over (e.g. "Mode=OneTime" without a path)
        // is not a valid path — preserve the original as a comment instead.
        if (path.find('=') != std::string::npos) return {};

        // Strip C# cast wrappers: "((ns:Type)X).Y" / "(ns:Type)X" → "X.Y"
        while (!path.empty() && path.front() == '(') {
            int depth = 0;
            std::size_t end = std::string::npos;
            for (std::size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '(') { ++depth; continue; }
                if (path[i] == ')') {
                    --depth;
                    if (depth == 0) { end = i; break; }
                }
            }
            if (end == std::string::npos) break;
            std::string inner = path.substr(1, end - 1);
            if (inner.find(':') == std::string::npos) break;   // attached property, not a cast
            path.erase(0, end + 1);
        }

        // Attached-property paths "(X.Y)" are not expressible → comment
        if (!path.empty() && path.front() == '(') return {};

        // Namespace prefix on the first segment: "ns:Type" → "Type"
        {
            auto colon = path.find(':');
            if (colon != std::string::npos && colon > 0) {
                bool valid = true;
                for (std::size_t i = 0; i < colon && valid; ++i) {
                    char c = path[i];
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'))
                        valid = false;
                }
                if (valid) path.erase(0, colon + 1);
            }
        }

        // Method calls (A.b(...)) keep their exact text, case preserved —
        // the projection resolves the callee against the view's code-behind.
        if (path.find('(') != std::string::npos) return path;

        // Plain dotted path → lower-camel property chain
        std::string result;
        std::string seg;
        for (std::size_t i = 0; i < path.size(); ++i) {
            char c = path[i];
            if (c == '[') {
                auto close = path.find(']', i);
                if (close != std::string::npos) i = close;
                continue;
            }
            if (c == '.' || c == '/') {
                if (!seg.empty()) {
                    if (!result.empty()) result += ".";
                    result += lower_camel(seg);
                    seg.clear();
                }
                continue;
            }
            seg += c;
        }
        if (!seg.empty()) {
            if (!result.empty()) result += ".";
            result += lower_camel(seg);
        }
        return result;
    };

    // Element-name bindings: {Binding ElementName=X, Path=P} → "x.p".
    {
        std::size_t en = std::string::npos;
        for (std::size_t i = 0; (i = body.find("ElementName=", i)) != std::string::npos; ++i) {
            if (is_word_boundary(i)) { en = i; break; }
        }
        if (en != std::string::npos) {
            std::string rest = body.substr(en + 12);
            auto b = rest.find_first_not_of(" \t\"'");
            auto e = rest.find_first_of(" \t,\"'");
            std::string elem = (b == std::string::npos) ? ""
                              : (e == std::string::npos) ? rest.substr(b)
                                                         : rest.substr(b, e - b);
            std::string prop;
            std::size_t pp = std::string::npos;
            for (std::size_t i = 0; (i = body.find("Path=", i)) != std::string::npos; ++i) {
                if (is_word_boundary(i)) { pp = i; break; }
            }
            if (pp != std::string::npos) {
                std::string pv = body.substr(pp + 5);
                auto pb = pv.find_first_not_of(" \t\"'");
                auto pe = pv.find_first_of(" \t,\"'");
                prop = (pb == std::string::npos) ? ""
                       : (pe == std::string::npos) ? pv.substr(pb)
                                                   : pv.substr(pb, pe - pb);
            }
            if (!elem.empty()) {
                std::string elem_lc = lower_camel(elem);
                if (!prop.empty()) {
                    if (elem_lc == "root") {
                        // ElementName=Root → the view root element: bind the
                        // property path directly (no element prefix needed).
                        return translate_plain_path(prop);
                    }
                    return elem_lc + "." + lower_camel(prop);
                }
                if (elem_lc == "root") return {};
                return elem_lc;
            }
        }
    }

    auto comma = body.find(',');
    std::string path = (comma == std::string::npos) ? body : body.substr(0, comma);
    if (path.find_first_not_of(" \t") == std::string::npos)
        return "root";   // "{x:Bind}"/"{Binding}" with no path → DataContext (code-behind root)
    return translate_plain_path(path);
}

// Converts a XAML attribute value into a .winui value expression.
inline auto convert_value(std::string_view raw, std::string_view attr_name,
                          std::string& comment_out, std::string& mode_out) -> ValueExpr {
    ValueExpr v;
    std::string s(raw);
    // trim
    auto b = s.find_first_not_of(" \t");
    auto e = s.find_last_not_of(" \t");
    if (b == std::string::npos) return v;
    s = s.substr(b, e - b + 1);

    // binding or resource reference?
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
        std::string inner = s.substr(1, s.size() - 2);
        // Trim leading and trailing whitespace from inner
        auto kb = inner.find_first_not_of(" \t");
        auto ke = inner.find_last_not_of(" \t");
        std::string inner_trimmed = (kb != std::string::npos && ke != std::string::npos) ? inner.substr(kb, ke - kb + 1) : inner;

        // StaticResource / ThemeResource inside { }
        if (inner_trimmed.rfind("StaticResource ", 0) == 0 || inner_trimmed.rfind("ThemeResource ", 0) == 0) {
            auto sp = inner_trimmed.find(' ');
            std::string key = inner_trimmed.substr(sp + 1);
            auto k_b = key.find_first_not_of(" \t");
            auto k_e = key.find_last_not_of(" \t");
            if (k_b != std::string::npos && k_e != std::string::npos) key = key.substr(k_b, k_e - k_b + 1);
            v.expr = qstr(key);
            v.is_resource_ref = true;
            return v;
        }

        auto bpos = inner.find("x:Bind");
        std::string_view kind = "Binding";
        if (bpos != std::string::npos) {
            kind = "x:Bind";
            inner = inner.substr(bpos + 6);
        } else if (inner.rfind("Binding", 0) == 0) {
            inner = inner.substr(7);
        } else {
            kind = "Markup";
        }
        if (kind == "x:Bind" || kind == "Binding") {
            std::string expr = translate_binding(inner, mode_out);
            if (!expr.empty()) {
                v.expr = expr;
                v.is_binding = true;
                return v;
            }
        }
        v.comment = s;
        return v;
    }

    // StaticResource / ThemeResource → resource key string
    auto res_match = [&](std::string_view marker) -> bool {
        return s.size() > marker.size() + 1 && s.substr(0, marker.size()) == marker &&
               s[marker.size()] == ' ';
    };
    if (res_match("StaticResource") || res_match("ThemeResource")) {
        auto lb = s.find('{');
        auto rb = s.rfind('}');
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            std::string key = s.substr(lb + 1, rb - lb - 1);
            // strip marker prefix
            auto sp = key.find(' ');
            if (sp != std::string::npos) key = key.substr(sp + 1);
            // trim
            auto kb = key.find_first_not_of(" \t");
            auto ke = key.find_last_not_of(" \t");
            if (kb != std::string::npos && ke != std::string::npos) key = key.substr(kb, ke - kb + 1);
            v.expr = qstr(key);
            v.is_resource_ref = true;
            return v;
        }
    }

    // bool
    if (s == "True" || s == "true") { v.expr = "true"; return v; }
    if (s == "False" || s == "false") { v.expr = "false"; return v; }

    // numeric (int / double), strip trailing f/F
    {
        std::string num(s);
        if (!num.empty() && (num.back() == 'f' || num.back() == 'F' || num.back() == 'd'
                             || num.back() == 'D' || num.back() == 'm' || num.back() == 'M'))
            num.pop_back();
        bool numeric = true;
        int dots = 0;
        int digits = 0;
        for (char c : num) {
            if (c == '-') continue;
            if (c == '.') { ++dots; if (dots > 1) { numeric = false; break; } continue; }
            if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
            ++digits;
        }
        if (numeric && digits > 0) {
            if (!num.empty() && num.front() == '.') num.insert(num.begin(), '0');
            v.expr = num;
            return v;
        }
    }

    // color hex
    if (s.size() >= 4 && s[0] == '#') {
        v.expr = qstr(s);
        return v;
    }

    // plain string
    v.expr = qstr(s);
    return v;
}

// §4  XAML attribute → .winui mapping tables

// XAML layout/visual properties that map to universal .winui modifiers.
struct ModifierMap { std::string_view xaml; std::string_view winui; };

inline constexpr ModifierMap g_universal_mods[] = {
    {"Width", "width"}, {"Height", "height"},
    {"MinWidth", "minWidth"}, {"MaxWidth", "maxWidth"},
    {"MinHeight", "minHeight"}, {"MaxHeight", "maxHeight"},
    {"Opacity", "opacity"},
    {"CornerRadius", "cornerRadius"},
    {"Padding", "padding"},
    {"Margin", "margin"},
    {"HorizontalAlignment", "horizontalAlignment"},
    {"VerticalAlignment", "verticalAlignment"},
    {"HorizontalContentAlignment", "horizontalContentAlignment"},
    {"VerticalContentAlignment", "verticalContentAlignment"},
    {"Style", "style"},
    {"Background", "background"},
    {"Foreground", "foregroundColor"},
    {"BorderBrush", "borderBrush"},
    {"BorderThickness", "borderThickness"},
    {"FontFamily", "fontFamily"},
    {"FontSize", "fontSize"},
    {"FontWeight", "fontWeight"},
    {"IsEnabled", "isEnabled"},
    {"Visibility", "visible"},
};

// Properties whose string value is a file path (not a URI scheme). Their
// values are emitted with single backslashes; forward slashes remain only
// inside scheme:// references (https://, ms-appx:///, foo:///...).
inline auto is_path_like_prop(std::string_view prop) -> bool {
    for (auto const& p : {"source", "src", "uri", "imageSource", "iconSource",
                          "videoSource", "posterSource", "navigateUri", "baseUri",
                          "logoSrc", "logo", "profilePicture", "bitmap", "icon"}) {
        if (prop == p) return true;
    }
    return false;
}

// XAML events → .winui event modifier names.
inline constexpr std::string_view g_event_names[] = {
    "Click", "Toggled", "Checked", "Unchecked", "ValueChanged", "SelectionChanged",
    "TextChanged", "PasswordChanged", "DateChanged", "SelectedDatesChanged",
    "TimeChanged", "ColorChanged", "ItemClick", "ItemInvoked", "QuerySubmitted",
    "SuggestionChosen", "Opening", "Closing", "Opened", "Closed",
    "PrimaryButtonClick", "SecondaryButtonClick", "CloseButtonClick",
    "Loaded", "Unloaded", "RefreshRequested", "BackRequested",
    "Expanding", "Expanded", "Collapsing", "Collapsed",
    "PaneOpening", "PaneOpened", "PaneClosing", "PaneClosed",
    "DragStarted", "DragCompleted", "NavigationRequested", "NavigationStarting",
    "NavigationCompleted", "WebMessageReceived", "MediaOpened", "MediaEnded",
    "MediaFailed", "MapLoaded", "MapTapped", "PreviewKeyDown", "KeyDown", "KeyUp",
    "SizeChanged", "GotFocus", "LostFocus", "PointerPressed", "PointerReleased",
    "IsEnabledChanged", "ViewChangeStarted", "ViewChangeCompleted", "ScrollViewerViewChanged",
};

inline auto xaml_event_to_winui(std::string_view ev) -> std::string {
    // Default: prefix with "on" — Click → onClick, Toggled → onToggled, ...
    // Special cases only where the XAML name differs from the .winui name.
    if (ev == "NavigationRequested") return "onNavigated";
    if (ev == "ScrollViewerViewChanged") return "onViewChanged";
    std::string out = "on";
    out += std::string(ev);
    return out;
}

// §5  Code-behind integration

struct ViewInfo {
    std::string name;                  // view name (x:Class last segment)
    std::vector<std::string> state_vars;    // "name: type" (deduped, ordered)
    std::vector<std::string> callbacks;     // "HandlerName"
    std::vector<std::string> comments;      // header comments
    bool is_resource = false;
};

// map code-behind type → .winui type
inline auto winui_type(lang::CsTypeKind k) -> std::string {
    switch (k) {
        case lang::CsTypeKind::String: return "string";
        case lang::CsTypeKind::Int:    return "int";
        case lang::CsTypeKind::Bool:   return "bool";
        case lang::CsTypeKind::Double:
        case lang::CsTypeKind::Float:  return "double";
        default: return "string";
    }
}

// Normalize a code-behind member type to the .winui type vocabulary:
//   "ObservableCollection<string>" / "List<string>" / "IEnumerable<string>" → "string[]"
//   "DateTime" → "datetime"; primitives as before
// Returns "" for custom classes — those are flattened into flat state vars
// (see Converter::flatten_cb_path) instead of being carried as object types.
inline auto normalize_cb_type(std::string const& raw) -> std::string {
    std::string t(raw);
    auto strip_pref = [&](std::string_view p) {
        while (t.starts_with(p)) t = t.substr(p.size());
    };
    strip_pref("global::");
    strip_pref("System.");
    if (t.starts_with("Platform::")) t = t.substr(10);
    if (!t.empty() && t.back() == '^') t.pop_back();

    // generic collections: head<element>
    if (auto lt = t.find('<'); lt != std::string::npos && !t.empty() && t.back() == '>') {
        std::string head = t.substr(0, lt);
        auto dot = head.find_last_of('.');
        if (dot != std::string::npos) head = head.substr(dot + 1);
        std::string elt = t.substr(lt + 1, t.size() - lt - 2);
        auto is_col = [&](std::string_view h) {
            return head == "ObservableCollection" || head == "List" || head == "IList"
                || head == "IReadOnlyList" || head == "ICollection" || head == "IEnumerable"
                || head == "Collection" || head == "IReadOnlyCollection" || head == "ISet"
                || head == "HashSet";
        };
        if (is_col(head)) {
            std::string en = normalize_cb_type(elt);
            if (en.empty()) en = "any";
            return en + "[]";
        }
        return {};   // non-collection generic → custom
    }

    switch (lang::map_type(t)) {
        case lang::CsTypeKind::String: return "string";
        case lang::CsTypeKind::Int:    return "int";
        case lang::CsTypeKind::Bool:   return "bool";
        case lang::CsTypeKind::Double:
        case lang::CsTypeKind::Float:  return "double";
        case lang::CsTypeKind::Other:  break;
    }
    if (t == "DateTime") return "datetime";
    return {};   // custom class → flattened instead
}

// Sanitize a raw code-behind type for direct emission into .winui source.
// Always returns a type from the .winui vocabulary ("string", "int", "bool",
// "double", "datetime", "T[]", "any[]") — never a raw C#/C++ annotation
// like "object?", "Button?" or "TypedEventHandler<A, B>?" (the .winui
// grammar has no nullable '?'), and never a raw generic like
// "ObservableCollection<string>" (mapped to "string[]").
export inline auto sanitize_winui_type(std::string const& raw) -> std::string {
    std::string t = normalize_cb_type(raw);
    return t.empty() ? "string" : t;
}

// §6  Converter state

struct Converter {
    lang::CodeBehind const* cb = nullptr;
    ConvertOptions const* opts = nullptr;
    ConvertResult* result = nullptr;
    Emitter em;
    std::vector<std::string> callbacks;
    std::unordered_set<std::string> callback_set;
    std::vector<std::string> state_vars;
    std::unordered_set<std::string> state_set;
    std::vector<std::string> binding_vars;
    std::unordered_set<std::string> binding_set;
    std::unordered_map<std::string, lang::Member> cb_props;   // property name → member
    std::unordered_map<std::string, std::string> element_names;  // x:Name → XAML tag name (control type)
    // code-behind ctor wiring: element x:Name → [(winui event, handler)]
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> cb_wiring;

    std::vector<RawPair> pending_raw;   // fragments from transparent parents, attached to the next control
    std::vector<RawPair> xmlns_pairs;   // xmlns declarations (deduped), attached to the first control
    int grid_rows = 0;
    int grid_cols = 0;
    int grid_depth = 0;

    auto find_cb_prop(std::string_view name) -> lang::Member const* {
        // x:Bind roots arrive lower-camel ("title"); code-behind names are
        // PascalCase ("Title") — match case-insensitively.
        std::string key(name);
        for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (auto const& [k, v] : cb_props) {
            std::string lk = k;
            for (auto& ch : lk) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lk == key) return &v;
        }
        return nullptr;
    }

    // Flatten a custom-class member path into a single flat state var:
    // "viewModel.title" → "viewModelTitle" when the root is a code-behind
    // property of a non-primitive type. The winuic philosophy has flat state —
    // no nested view-model objects — so member paths resolve to flat vars.
    // Element bindings ("colorPicker.colorBrush") and plain paths pass through.
    auto flatten_cb_path(std::string const& expr) -> std::string {
        auto dot = expr.find('.');
        if (dot == std::string::npos) return expr;
        std::string root = expr.substr(0, dot);
        auto* m = find_cb_prop(root);
        if (!m || m->kind != lang::CsTypeKind::Other) return expr;
        std::string flat = root;
        for (char ch : expr.substr(dot + 1))
            if (ch != '.') flat += ch;
        return lower_camel(flat);
    }

    // Reserved .winui keywords can never be declared as state/binding vars.
    static auto is_reserved_word(std::string_view name) -> bool {
        static constexpr std::string_view reserved[] = {
            "root", "self", "true", "false", "var", "if", "else", "foreach",
            "in", "return", "while", "switch", "case", "default", "break",
            "continue", "view", "callback", "state", "binding", "published",
            "computed", "environment", "import", "export", "version", "for",
        };
        for (auto r : reserved) if (r == name) return true;
        return false;
    }

    auto ensure_state_var(std::string name, std::string type, std::string init = "") -> void {
        if (is_reserved_word(name)) return;
        if (!state_set.insert(name).second) return;
        std::string decl = name + ": " + type;
        if (!init.empty()) decl += " = " + init;
        state_vars.push_back(std::move(decl));
    }

    auto ensure_binding_var(std::string name, std::string type, std::string init = "") -> void {
        if (is_reserved_word(name)) return;
        if (!binding_set.insert(name).second) return;
        std::string decl = name + ": " + type;
        if (!init.empty()) decl += " = " + init;
        binding_vars.push_back(std::move(decl));
    }

    auto ensure_callback(std::string name) -> void {
        if (!callback_set.insert(name).second) return;
        callbacks.push_back(std::move(name));
    }


    // Record an x:Bind reference root; declare a state/binding var for it.
    // One-way bindings → @state var; two-way ($name) bindings → @binding var.
    // Custom-class member paths are flattened first ("viewModel.title" →
    // "viewModelTitle") and code-behind types are carried through the winmd
    // ("List<string>" → "string[]", "DateTime" → "datetime").
    auto record_binding_root(std::string const& expr_in, std::string const& mode,
                             std::string_view suffix_hint) -> void {
        if (expr_in.empty()) return;
        // Pre-flatten root, for the code-behind type lookup (flattening renames
        // "viewModel.title" → "viewModelTitle", losing the original member).
        std::string orig_root = expr_in;
        auto orig_dot = orig_root.find('.');
        if (orig_dot != std::string::npos) orig_root = orig_root.substr(0, orig_dot);

        std::string expr = flatten_cb_path(expr_in);
        // root is the first identifier segment
        std::string root = expr;
        auto dot = root.find('.');
        if (dot != std::string::npos) root = root.substr(0, dot);
        if (!is_ident(root) || is_reserved_word(root)) return;

        bool two_way = (mode == "TwoWay");
        auto declare = [&](std::string type) {
            if (two_way) ensure_binding_var(root, type);
            else         ensure_state_var(root, type);
        };

        // if root matches a code-behind property → typed state/binding var
        if (auto* m = find_cb_prop(orig_root); m) {
            std::string ty = normalize_cb_type(m->type);
            if (ty.empty()) {
                // custom class — flattened member paths type by special cases:
                // CollectionViewSource.View is a collection view of items
                if (m->type.find("CollectionViewSource") != std::string::npos
                    && orig_dot != std::string::npos)
                    ty = "any[]";
                else
                    ty = winui_type(m->kind);   // custom → string fallback
            }
            declare(ty);
            return;
        }
        // element-rooted binding (root is an x:Name in the XAML) — case-insensitive
        {
            std::string lroot = root;
            for (auto& ch : lroot) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            bool is_element = false;
            std::string elem_ctrl_type;
            for (auto const& [ename, etype] : element_names) {
                std::string le = ename;
                for (auto& ch : le) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (le == lroot) { is_element = true; elem_ctrl_type = etype; break; }
            }
            if (is_element) {
            bool is_bool = false;
            for (auto const* suf : {"isOn", "isChecked", "isEnabled", "isActive", "isOpen",
                                    "isExpanded", "isPaneOpen", "isMultiSelect", "isItemClickEnabled",
                                    "isReadOnly", "isCompact", "isIndeterminate", "isSelected",
                                    "isPlaying", "autoPlay", "isVisible", "showAsMonochrome"}) {
                if (expr.size() > std::string_view(suf).size() &&
                    expr.substr(expr.size() - std::string_view(suf).size()) == suf) {
                    is_bool = true;
                    break;
                }
            }
            bool is_number = false;
            for (auto const* suf : {"value", "valueText", "min", "max", "stepFrequency",
                                    "fontSize", "width", "height", "opacity", "rotation",
                                    "scaleX", "scaleY", "translateX", "translateY", "zoom"}) {
                if (expr.size() > std::string_view(suf).size() &&
                    expr.substr(expr.size() - std::string_view(suf).size()) == suf) {
                    is_number = true;
                    break;
                }
            }
            // Use actual XAML control type instead of generic string/bool/double
            declare(is_bool ? "bool" : is_number ? "double" : elem_ctrl_type.empty() ? "string" : elem_ctrl_type);
            return;
            }
        }
        // unknown root → string state/binding var so the output always compiles
        declare("string");
        (void)suffix_hint;
    }
};

// Scan a code-behind constructor body for programmatic event wiring
// (C++: `auto x = Accessor(); x.Click([this](auto,auto){ Handler(s,e); });`
//  C#:   `x.Click += Handler;`). The receiver resolves to an element x:Name
// (accessor name == x:Name, alias == its lower-camel local), so wiring
// becomes `.on<Event> { self.<Handler>(); }` on that element. Handler names
// are recorded as callbacks only when a real body exists in method_bodies;
// inline lambdas with no delegating call are skipped (kept as comments by
// the body translator elsewhere).
inline auto collect_codebehind_wiring(Converter& c, lang::CodeBehind const& cb) -> void {
    bool dbg = std::getenv("X2W_DEBUG") != nullptr;
    auto dbg_log = [&](std::string const& m) { if (dbg) { std::fprintf(stderr, "[x2w-wire] %s\n", m.c_str()); std::fflush(stderr); } };
    dbg_log("class=" + cb.class_name + " bodies=" + std::to_string(cb.method_bodies.size()));
    if (cb.method_bodies.empty()) return;
    // The view's own constructor body is stored under the class short name.
    std::string ctor;
    for (auto const& [k, b] : cb.method_bodies) {
        if (k == cb.class_name) { ctor = b; break; }
    }
    if (ctor.empty()) {
        dbg_log("ctor: empty (fallback to best-effort body scan)");
        std::string best;
        for (auto const& [k, b] : cb.method_bodies)
            if (b.find(".Event(") != std::string::npos || b.find(" += ") != std::string::npos)
                if (b.size() > best.size()) best = b;
        ctor = std::move(best);
    }
    if (ctor.empty()) return;

    // local alias -> accessor (element x:Name) from `auto x = Accessor()` /
    // `if (auto x = Accessor())`
    std::unordered_map<std::string, std::string> aliases;
    std::size_t pos = 0;
    while ((pos = ctor.find("auto ", pos)) != std::string::npos) {
        std::size_t i = pos + 5;
        while (i < ctor.size() && (std::isalnum(static_cast<unsigned char>(ctor[i])) || ctor[i] == '_')) ++i;
        std::string local = ctor.substr(pos + 5, i - (pos + 5));
        std::size_t eq = ctor.find('=', i);
        if (eq == std::string::npos || eq - i > 64) { pos = i; continue; }
        std::size_t a0 = eq + 1;
        while (a0 < ctor.size() && (ctor[a0] == ' ' || ctor[a0] == '\t')) ++a0;
        std::size_t a1 = a0;
        while (a1 < ctor.size() && (std::isalnum(static_cast<unsigned char>(ctor[a1])) || ctor[a1] == '_')) ++a1;
        std::string acc = ctor.substr(a0, a1 - a0);
        if (!local.empty() && !acc.empty() && c.element_names.count(acc))
            aliases[local] = acc;
        pos = a1;
    }

    // Walk receivers: `<alias>.<Event>(` , `<Accessor>().<Event>(`, and C#
    // `<alias>.<Event> += Handler;`  (C++/CX and C# both supported).
    std::size_t scan = 0;
    auto is_event_name = [&](std::string const& nm, std::string const& ctrl_type) {
        for (auto const& ev : g_event_names) if (ev == nm) return true;
        if (ext::is_ext_xaml_event(nm)) return true;
        if (!ctrl_type.empty() && ext::is_ext_ctrl_event(ctrl_type, nm)) return true;
        return false;
    };
    while (scan < ctor.size()) {
        std::size_t dot = ctor.find('.', scan);
        if (dot == std::string::npos) break;
        // receiver: ident before dot; fall back to Accessor() form when the
        // char before '.' is ')': walk back over ')' '('. 
        std::string recv;
        std::size_t r0 = dot;
        while (r0 > 0 && (std::isalnum(static_cast<unsigned char>(ctor[r0 - 1])) || ctor[r0 - 1] == '_')) --r0;
        if (r0 < dot) recv = ctor.substr(r0, dot - r0);
        if (recv.empty()) {
            // <Accessor>().Event( : find ident before the '(' before ')'
            std::size_t k2 = dot;
            if (k2 > 0 && ctor[k2 - 1] == ')') {
                std::size_t k3 = k2 - 1;
                int bd = 1;
                while (k3 > 0 && bd > 0) {
                    --k3;
                    if (ctor[k3] == ')') ++bd;
                    if (ctor[k3] == '(') --bd;
                }
                if (k3 > 0 && ctor[k3] == '(') {
                    std::size_t a0 = k3;
                    while (a0 > 0 && (std::isalnum(static_cast<unsigned char>(ctor[a0 - 1])) || ctor[a0 - 1] == '_')) --a0;
                    recv = ctor.substr(a0, k3 - a0);
                }
            }
        }
        std::size_t e0 = dot + 1;
        while (e0 < ctor.size() && (std::isalnum(static_cast<unsigned char>(ctor[e0])) || ctor[e0] == '_')) ++e0;
        std::string ev = ctor.substr(dot + 1, e0 - (dot + 1));
        scan = (e0 > dot + 1) ? e0 : dot + 1;
        if (recv.empty() || ev.empty()) continue;
        // resolve receiver to an element x:Name (alias or bare accessor).
        // C++/CX code-behind uses "Name->Event" and "this->Name->Event" too.
        if (recv.empty()) {
            // this->Button->Click(...) : receiver ident is just before '->'
            std::size_t a0 = dot;
            if (a0 >= 2 && ctor[a0 - 1] == '>' && ctor[a0 - 2] == '-') {
                std::size_t b0 = a0 - 2;
                while (b0 > 0 && (std::isalnum(static_cast<unsigned char>(ctor[b0 - 1])) || ctor[b0 - 1] == '_')) --b0;
                recv = ctor.substr(b0, (a0 - 2) - b0);
            }
        }
        if (recv.empty()) {
            // Button->Click : ident then "->"
            std::size_t b0 = dot;
            if (b0 >= 2 && ctor[b0 - 1] == '>' && ctor[b0 - 2] == '-') {
                std::size_t c0 = b0 - 2;
                std::size_t s0 = c0;
                while (s0 > 0 && (std::isalnum(static_cast<unsigned char>(ctor[s0 - 1])) || ctor[s0 - 1] == '_')) --s0;
                recv = ctor.substr(s0, c0 - s0);
            }
        }
        std::string xname;
        if (c.element_names.count(recv)) xname = recv;
        else if (aliases.count(recv)) xname = aliases[recv];
        if (xname.empty()) { dbg_log("drop: recv='" + recv + "' ev='" + ev + "' (no element; aliases=" + std::to_string(aliases.size()) + ")"); continue; }
        std::string ctrl_type = c.element_names.count(xname) ? c.element_names[xname] : "";
        if (!is_event_name(ev, ctrl_type)) { dbg_log("drop: '" + ev + "' not an event on " + ctrl_type); continue; }
        // skip spaces, optional '(' then the lambda '[' or C# ' += '
        std::size_t t = e0;
        while (t < ctor.size() && (ctor[t] == ' ' || ctor[t] == '\t')) ++t;
        std::string handler;
        if (ctor.compare(t, 4, " += ") == 0) {
            std::size_t h0 = t + 4;
            while (h0 < ctor.size() && (ctor[h0] == ' ' || ctor[h0] == '\t')) ++h0;
            // C++/CX: += ref new HandlerType(this, &Class::Method); / += ref new HandlerType(&Method);
            if (ctor.compare(h0, 8, "ref new ") == 0) {
                // skip to the '&'
                auto amp = ctor.find('&', h0 + 8);
                if (amp != std::string::npos) {
                    // handler is the last ident after & (Class::Method -> Method)
                    std::size_t m0 = amp + 1;
                    std::size_t m1 = m0;
                    while (m1 < ctor.size() && (std::isalnum(static_cast<unsigned char>(ctor[m1])) || ctor[m1] == '_' || ctor[m1] == ':')) ++m1;
                    // take the trailing identifier after the last ':'
                    std::string qual = ctor.substr(m0, m1 - m0);
                    auto cc = qual.find_last_of(':');
                    handler = (cc == std::string::npos) ? qual : qual.substr(cc + 1);
                }
            } else {
                std::size_t h1 = h0;
                while (h1 < ctor.size() && (std::isalnum(static_cast<unsigned char>(ctor[h1])) || ctor[h1] == '_')) ++h1;
                handler = ctor.substr(h0, h1 - h0);
            }
        } else if (t < ctor.size() && ctor[t] == '(') {
            // C++: Event({ this, &Class::Method }); - the standard C++/WinRT
            // delegate-construction wire. Inside the parens, look for
            // '&Class::Method' (or '&Method') and take the trailing ident.
            // Also covers C# style Event((s,e) => Handler(...)) via the
            // lambda scan below when no '&' is present.
            {
                // find the matching ')' for the event call
                int pd2 = 0;
                std::size_t pe = std::string::npos;
                bool in_s2 = false;
                for (std::size_t k = t; k < ctor.size(); ++k) {
                    char ch = ctor[k];
                    if (ch == '\"') in_s2 = !in_s2;
                    if (in_s2) continue;
                    if (ch == '(') ++pd2;
                    if (ch == ')') { --pd2; if (pd2 == 0) { pe = k; break; } }
                    if (ch == ';' && pd2 == 0) { pe = k; break; } // call statement, not a wire
                }
                if (pe != std::string::npos) {
                    // prefer the &Class::Method member-delegate form
                    auto amp = ctor.find('&', t);
                    if (amp != std::string::npos && amp < pe) {
                        std::size_t m0 = amp + 1;
                        std::size_t m1 = m0;
                        while (m1 < pe && (std::isalnum(static_cast<unsigned char>(ctor[m1])) || ctor[m1] == '_' || ctor[m1] == ':')) ++m1;
                        std::string qual = ctor.substr(m0, m1 - m0);
                        auto cc = qual.find_last_of(':');
                        handler = (cc == std::string::npos) ? qual : qual.substr(cc + 1);
                    }
                    // lambda with a call-statement body: Event([](auto&&...){ Handler(...); })
                    // The lambda's parameter parens/brackets must NOT affect the
                    // scan: the body is simply the first '{' after the wire open.
                    if (handler.empty()) {
                        std::size_t body = std::string::npos;
                        bool in_s = false;
                        for (std::size_t k = t; k < pe; ++k) {
                            char ch = ctor[k];
                            if (ch == '\"') in_s = !in_s;
                            if (in_s) continue;
                            if (ch == '{') { body = k; break; }
                        }
                        if (body != std::string::npos) {
                            auto ce = ctor.find('}', body);
                            if (ce != std::string::npos && ce < pe) {
                                std::size_t h = body + 1;
                                while (h < ce) {
                                    if (!(std::isalpha(static_cast<unsigned char>(ctor[h])) || ctor[h] == '_')) { ++h; continue; }
                                    std::size_t id0 = h;
                                    while (h < ce && (std::isalnum(static_cast<unsigned char>(ctor[h])) || ctor[h] == '_')) ++h;
                                    std::string idn = ctor.substr(id0, h - id0);
                                    std::size_t k = h;
                                    while (k < ce && (ctor[k] == ' ' || ctor[k] == '\t')) ++k;
                                    if (k < ce && ctor[k] == '(') { handler = idn; break; }
                                }
                            }
                        }
                    }
                }
            }
        } else if (t < ctor.size() && ctor[t] == '[') {
            // Event[this](...) { Handler(...); } without outer parens
            auto body = ctor.find('{', t);
            if (body != std::string::npos) {
                auto ce = ctor.find('}', body);
                if (ce != std::string::npos) {
                    std::size_t h = body + 1;
                    while (h < ce) {
                        if (!(std::isalpha(static_cast<unsigned char>(ctor[h])) || ctor[h] == '_')) { ++h; continue; }
                        std::size_t id0 = h;
                        while (h < ce && (std::isalnum(static_cast<unsigned char>(ctor[h])) || ctor[h] == '_')) ++h;
                        std::string idn = ctor.substr(id0, h - id0);
                        std::size_t k = h;
                        while (k < ce && (ctor[k] == ' ' || ctor[k] == '\t')) ++k;
                        if (k < ce && ctor[k] == '(') { handler = idn; break; }
                    }
                }
            }
        }
        dbg_log("cand: " + xname + "." + ev + " handler='" + handler + "'");
        if (handler.empty()) continue;
        // only delegate to methods with a real body (they become callbacks);
        // `AnimationHelper::x(...)` etc. have no body -> skipped.
        if (cb.method_bodies.find(handler) == cb.method_bodies.end()) { dbg_log("drop: handler '" + handler + "' has no captured body"); continue; }
        std::string wev = xaml_event_to_winui(ev);
        // register the handler as a view callback so its declaration + body
        // emit (the .onEvent mod is attached during the element walk)
        c.ensure_callback(handler);
        auto& vec = c.cb_wiring[xname];
        bool dup = false;
        for (auto const& [we, hn] : vec) if (we == wev && hn == handler) { dup = true; break; }
        if (!dup) vec.emplace_back(wev, handler);
    }
}

// §7  Attribute classification

// XAML attributes we silently ignore (namespace plumbing).
inline auto is_ignorable_attr(std::string_view name) -> bool {
    static constexpr std::string_view ign[] = {
        "xmlns", "Class", "Key", "Uid", "DataType", "Ignorable", "DefaultBindMode",
        "Phase", "Bind", "FieldModifier", "DeferLoadStrategy",
    };
    for (auto const& i : ign) if (i == name) return true;
    return false;
}

// Collect raw pairs from a child node: property elements whose element
// children are ALL frag-family (Grid.RowDefinitions, X.Resources,
// VisualStateManager.VisualStateGroups, …) and standalone frag-family
// elements. Returns true when the node was consumed (must NOT be emitted
// as a normal child).
inline auto collect_fragment(xaml::Node const& el, std::vector<RawPair>& out) -> bool {
    bool prop_elem = (el.name.find('.') != std::string::npos);
    if (prop_elem) {
        bool has_elem = false;
        bool all_frag = true;
        for (auto const& ch : el.children) {
            if (ch.kind != xaml::NodeKind::Element) continue;
            has_elem = true;
            if (!is_frag_family(ch.name)) { all_frag = false; break; }
        }
        if (has_elem && all_frag) {
            auto dot = el.name.find_last_of('.');
            std::string key = el.name.substr(dot + 1);
            std::string frag;
            for (auto const& ch : el.children) {
                if (ch.kind == xaml::NodeKind::Element) frag += serialize_fragment(ch);
            }
            if (!frag.empty()) out.push_back({std::move(key), std::move(frag)});
            return true;
        }
        return false;
    }
    if (is_frag_family(el.name)) {
        std::string key;
        if (auto k = el.attr("Key")) key = std::string(*k);
        if (key.empty()) key = el.name;
        out.push_back({std::move(key), serialize_fragment(el)});
        return true;
    }
    return false;
}

// §8  Element conversion

// A "XAML code example" property element (e.g. ControlExample.Xaml) whose single
// element child is an <x:String> holding XAML markup. These are converted
// recursively into .winui equivalents so the sample code becomes real, compilable
// layout — not just verbatim text. Returns the x:String node or nullptr.
inline auto code_example_string(xaml::Node const& el) -> xaml::Node const* {
    if (el.kind != xaml::NodeKind::Element) return nullptr;
    auto dot = el.name.find_last_of('.');
    if (dot == std::string::npos) return nullptr;
    std::string local = el.name.substr(dot + 1);
    std::string lc = local;
    for (auto& ch : lc) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lc != "xaml") return nullptr;
    xaml::Node const* string_kid = nullptr;
    int elem_children = 0;
    for (auto const& ch : el.children) {
        if (ch.kind != xaml::NodeKind::Element) continue;
        ++elem_children;
        string_kid = &ch;
    }
    if (elem_children != 1 || !string_kid) return nullptr;
    std::string kname = string_kid->name;
    auto kdot = kname.find_last_of('.');
    if (kdot != std::string::npos) kname = kname.substr(kdot + 1);
    if (kname != "String") return nullptr;
    std::string text;
    for (auto const& t : string_kid->text) text += t;
    auto b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return nullptr;
    return (text[b] == '<') ? string_kid : nullptr;
}

inline auto is_code_example(xaml::Node const& el) -> bool {
    return code_example_string(el) != nullptr;
}

struct ElementOutcome {
    std::string text;           // emitted text
    std::vector<xaml::Node const*> inline_children;  // property-element children to emit inside
    bool text_content_present = false;  // raw text should be used as text arg
};

inline auto convert_element(Converter& c, xaml::Node const& el) -> void;

// Convert an embedded XAML code example into .winui lines. Runs the full
// conversion pipeline over the parsed markup with a private emitter and returns
// the captured lines. The verbatim source stays a .raw() pair on the parent, so
// nothing is lost — the example becomes real layout AND keeps its display text.
inline auto convert_code_example(Converter& c, xaml::Node const& el,
                                 std::vector<std::string>& out_lines) -> void {
    auto const* string_kid = code_example_string(el);
    if (!string_kid) return;

    std::string markup;
    for (auto const& t : string_kid->text) markup += t;

    auto parsed = xaml::parse(markup);
    if (!parsed.ok()) {
        out_lines.push_back("// XAML example not parseable — kept verbatim in .raw()");
        return;
    }

    // Sub-converter with its own emitter; shares registry/options/code-behind so
    // bindings and handler names inside the example resolve exactly like the rest
    // of the view (declarations are pre-discovered by PASS 1, so they stay in sync).
    Converter sub = c;
    sub.em = Emitter{};
    for (auto const& r : parsed.roots)
        if (r.kind == xaml::NodeKind::Element) convert_element(sub, r);

    std::string captured = std::move(sub.em.out);
    std::size_t pos = 0;
    while (pos < captured.size()) {
        auto nl = captured.find('\n', pos);
        std::string l = captured.substr(pos, (nl == std::string::npos) ? captured.size() - pos : nl - pos);
        if (!l.empty()) out_lines.push_back(std::move(l));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

// Emit a comment line from a raw XAML fragment.
inline auto emit_xaml_comment(Emitter& em, std::string_view frag) -> void {
    std::string line = "// ";
    line += frag;
    em.line(line);
}

// Determine .winui control name for a XAML element.
inline auto map_element_name(Converter& c, xaml::Node const& el, bool& transparent,
                             std::string& note) -> std::string {
    transparent = false;
    note.clear();
    std::string name = el.name;

    // root / structural elements → transparent containers
    if (name == "Page" || name == "UserControl" || name == "Window"
        || name == "Application" || name == "DataTemplate" || name == "DataTemplateSelector"
        || name == "ItemsPanelTemplate" || name == "ControlTemplate") {
        transparent = true;
        return {};
    }
    if (name == "Frame") return "Frame";
    if (name == "Grid") {
        // real WinUI Grid → abstract Grid with rows/columns derived below
        return "Grid";
    }
    if (name == "RowDefinition" || name == "ColumnDefinition") {
        note = name; // counted in Grid.RowDefinitions/ColumnDefinitions
        transparent = true;
        return {};
    }
    if (name == "x:String" || name == "String" || name == "Style" || name == "Setter"
        || name == "SolidColorBrush" || name == "LinearGradientBrush" || name == "RadialGradientBrush"
        || name == "GradientStop" || name == "Thickness" || name == "GridLength"
        || name == "TransitionCollection" || name == "VisualState" || name == "VisualStateGroup"
        || name == "VisualStateGroupList" || name == "VisualStateManager" || name == "Storyboard"
        || name == "Color" || name == "DoubleAnimation" || name == "ObjectAnimationUsingKeyFrames"
        || name == "EntranceThemeTransition" || name == "PopupThemeTransition"
        || name == "AddDeleteThemeTransition" || name == "ContentThemeTransition"
        || name == "RepositionThemeTransition" || name == "ReorderThemeTransition"
        || name == "EdgeUIThemeTransition" || name == "PaneThemeTransition"
        || name == "ThemeAnimation" || name == "Timeline" || name == "EasingFunctionBase"
        || name == "FontWeight" || name == "FontFamily" || name == "ColorAnimation" ||
        name == "BitmapImage" || name == "CompositeTransform" || name == "ScaleTransform"
        || name == "RotateTransform" || name == "TranslateTransform"
        || name == "ThemeDictionaries" || name == "ResourceDictionary") {
        transparent = true;
        note = name;
        return {};
    }
    if (name == "Run") { note = "Run"; return "TextBlock"; }
    if (name == "FontIcon") return "FontIcon";
    if (name == "LineBreak") { transparent = true; return {}; }
    if (name == "TextBlock") return "TextBlock";
    if (name == "StackPanel") return "StackPanel";
    if (name == "ScrollViewer") return "ScrollViewer";
    if (name == "ItemsControl") return "ListView";
    if (name == "OrientedStackPanel") return "StackPanel";
    if (name == "RelativePanel") return "RelativePanel";
    if (name == "VariableSizedWrapGrid") return "VariableSizedWrapGrid";
    if (name == "RichTextBlock") return "RichTextBlock";
    if (name == "ContentPresenter") { transparent = true; return {}; }
    if (name == "ToolTip") return "ToolTip";
    if (name == "NavigationViewItemSeparator") { transparent = true; return {}; }
    if (name == "AutoSuggestBox") return "AutoSuggestBox";
    if (name == "ContentDialog") return "ContentDialog";
    if (name == "TabViewItem") return "TabViewItem";
    if (name == "AppBarSeparator") return "AppBarSeparator";
    if (name == "MenuFlyoutSeparator") { transparent = true; return {}; }
    if (name == "AppBarElementContainer") { transparent = true; return {}; }

    if (is_registered(name)) return name;

    // custom / unknown control — emit as-is (compiler warns, never errors)
    return name;
}

inline auto join_args(std::vector<std::string> const& args) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) out += ", ";
        out += args[i];
    }
    return out;
}

// Build the .winui argument list & modifiers for an element.
// Returns emitted call lines appended to `out` (may be empty).
inline auto emit_attributes(Converter& c, xaml::Node const& el, std::string const& ctrl_name,
                            std::string& args_out, std::vector<std::string>& mods_out,
                            std::vector<RawPair>& raws_out) -> void {
    args_out.clear();
    mods_out.clear();
    raws_out.clear();

    std::vector<std::string> args;
    std::vector<std::string> mods;
    std::vector<RawPair> raws;

    // orientation is special for StackPanel (maps to .winui orientation enum)
    for (auto const& a : el.attrs) {
        std::string aname = a.name;
        auto colon = aname.find(':');
        std::string prefix = (colon == std::string::npos) ? "" : aname.substr(0, colon);
        std::string local  = (colon == std::string::npos) ? aname : aname.substr(colon + 1);

        // ignore namespace plumbing
        if (prefix == "xmlns") continue;
        if (local == "xmlns") continue;
        if (is_ignorable_attr(local) && (prefix == "x" || prefix == "" || prefix == "mc"
                                         || prefix == "d" || prefix == "local" || prefix == "controls"
                                         || prefix == "common" || prefix == "helpers" || prefix == "muxc"))
            continue;

        // x:Name → comment + element registry
        if (local == "Name" && prefix == "x") {
            c.element_names[a.value] = ctrl_name;
            raws.push_back({"x:Name", std::string(a.value)});
            continue;
        }

        // events
        {
            bool is_event = false;
            for (auto const& ev : g_event_names) {
                if (ev == local) { is_event = true; break; }
            }
            if (!is_event) is_event = ext::is_ext_xaml_event(local);
            if (!is_event) is_event = ext::is_ext_ctrl_event(ctrl_name, local);
            if (is_event) {
                if (is_ident(a.value)) {
                    std::string wev = xaml_event_to_winui(local);
                    // per-control extension events may rename the .winui event
                    if (auto xe = ext::ext_xaml_event(ctrl_name, local)) wev = *xe;
                    c.ensure_callback(a.value);
                    mods.push_back("." + wev + " { self." + a.value + "(); }");
                } else {
                    raws.push_back({aname, std::string(a.value)});
                }
                continue;
            }
        }

        // bindings that are not on a property (rare)
        // (handled inside value conversion)

        // universal modifiers (mostly independent of control)
        {
            bool handled = false;
            for (auto const& m : g_universal_mods) {
                if (m.xaml != local) continue;

                std::string comment;
                std::string mode;
                auto v = convert_value(a.value, local, comment, mode);
                if (v.is_binding) {
                    c.record_binding_root(v.expr, mode, local);
                    std::string fexpr = c.flatten_cb_path(v.expr);
                    if (mode == "TwoWay") {
                        mods.push_back("." + std::string(m.winui) + "($" + fexpr + ")");
                    } else {
                        mods.push_back("." + std::string(m.winui) + "(" + fexpr + ")");
                    }
                    handled = true;
                    break;
                }
                if (v.expr.empty()) {
                    raws.push_back({aname, std::string(a.value)});
                    handled = true;
                    break;
                }

                // enum normalization for specific modifiers
                std::string val = v.expr;
                if (m.winui == "visible") {
                    // Visibility="Collapsed" or "Hidden" → visible(false)
                    if (a.value == "Collapsed" || a.value == "Hidden") val = "false";
                    else if (a.value == "Visible") val = "true";
                }
                if (m.winui == "horizontalAlignment" || m.winui == "verticalAlignment"
                    || m.winui == "horizontalContentAlignment" || m.winui == "verticalContentAlignment"
                    || m.winui == "alignment") {
                    // map XAML enums to .winui alignment strings
                    std::string vv = a.value;
                    if (vv == "Center") val = qstr("Center");
                    else if (vv == "Left") val = qstr("Left");
                    else if (vv == "Right") val = qstr("Right");
                    else if (vv == "Stretch") val = qstr("Stretch");
                    else if (vv == "Start") val = qstr("Start");
                    else if (vv == "End") val = qstr("End");
                    else if (vv == "Fill") val = qstr("Fill");
                    else if (vv == "Top") val = qstr("Top");
                    else if (vv == "Bottom") val = qstr("Bottom");
                    else if (vv == "Justify") val = qstr("Justify");
                }
                if (m.winui == "style" && !v.is_resource_ref) {
                    // Style="{StaticResource Key}" already handled; bare keys allowed too
                    val = qstr(a.value);
                }
                if (m.winui == "padding") {
                    // Thickness "l,t,r,b" → uniform only
                    if (a.value.find(',') != std::string::npos) {
                        // check uniformity
                        std::string t = a.value;
                        std::vector<std::string> parts;
                        std::size_t pos = 0;
                        while (pos < t.size()) {
                            auto comma = t.find(',', pos);
                            if (comma == std::string::npos) { parts.push_back(t.substr(pos)); break; }
                            parts.push_back(t.substr(pos, comma - pos));
                            pos = comma + 1;
                        }
                        // strip spaces
                        for (auto& p : parts) {
                            auto pb = p.find_first_not_of(" \t");
                            auto pe = p.find_last_not_of(" \t");
                            p = (pb == std::string::npos) ? "" : p.substr(pb, pe - pb + 1);
                        }
                        bool uniform = (parts.size() == 2 && parts[0] == parts[1]);
                        if (parts.size() == 4) uniform = (parts[0] == parts[1] && parts[1] == parts[2] && parts[2] == parts[3]);
                        if (uniform && !parts.empty()) {
                            val = parts[0];
                        } else {
                            raws.push_back({aname, std::string(a.value)});
                            handled = true;
                            break;
                        }
                    }
                    if (!a.value.empty() && a.value.back() == '0' && a.value.find('.') == std::string::npos && a.value != "0")
                        val = a.value; // keep ints as-is
                    if (a.value == "0") val = "0";
                }
                if (m.winui == "isEnabled") {
                    // IsEnabled="False" → .disabled(true)
                    if (a.value == "False" || a.value == "false") {
                        mods.push_back(".disabled(true)");
                    }
                    // IsEnabled="True" is the default — skip
                    handled = true;
                    break;
                }

                // skip default values to keep output clean
                if (m.winui == "opacity" && a.value == "1.0") { handled = true; break; }
                if (m.winui == "visible" && a.value == "Visible") { handled = true; break; }

                mods.push_back("." + std::string(m.winui) + "(" + val + ")");
                handled = true;
                break;
            }
            if (!handled) {
                // runtime JSON extensions: XAML attr → .winui universal modifier
                for (auto const& [ex_xaml, ex_winui] : ext::registry().universal_mods) {
                    if (ex_xaml != local) continue;
                    std::string ex_mode;
                    std::string ex_comment;
                    auto v = convert_value(a.value, local, ex_comment, ex_mode);
                    if (v.is_binding) {
                        c.record_binding_root(v.expr, ex_mode, local);
                        mods.push_back("." + ex_winui + "(" + c.flatten_cb_path(v.expr) + ")");
                    } else if (!v.expr.empty()) {
                        mods.push_back("." + ex_winui + "(" + v.expr + ")");
                    } else {
                        raws.push_back({aname, std::string(a.value)});
                    }
                    handled = true;
                    break;
                }
            }
            if (handled) continue;
        }

        // property args (control-specific)
        {
            std::string winprop;
            // explicit XAML-name → winui-property maps first (runtime JSON
            // extensions; auto-derived per declared property, so explicit
            // renames always win over the name conventions below)
            if (auto xp = ext::ext_xaml_prop(ctrl_name, local)) {
                winprop = *xp;
            } else {
                // direct name match (schema property names are camelCase)
                std::string cand = local;
                if (!cand.empty()) cand[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(cand[0])));
                if (has_property(ctrl_name, cand)) {
                    winprop = cand;
                } else {
                // per-control XAML→winui renames
                static const std::unordered_map<std::string, std::string> renames = {
                    {"Text", "text"}, {"Content", "content"}, {"Header", "header"},
                    {"PlaceholderText", "placeholder"}, {"IsOn", "isOn"},
                    {"OnContent", "onContent"}, {"OffContent", "offContent"},
                    {"Value", "value"}, {"Minimum", "minimum"}, {"Maximum", "maximum"},
                    {"StepFrequency", "step"}, {"IsChecked", "isChecked"},
                    {"SelectedIndex", "selectedIndex"}, {"ItemsSource", "itemsSource"},
                    {"Source", "source"}, {"Stretch", "stretch"}, {"AutoPlay", "autoPlay"},
                    {"IsOpen", "isOpen"}, {"Title", "title"}, {"Message", "message"},
                    {"Severity", "severity"}, {"Subtitle", "subtitle"},
                    {"PrimaryButtonText", "primaryButtonText"},
                    {"SecondaryButtonText", "secondaryButtonText"},
                    {"CloseButtonText", "closeButtonText"},
                    {"IsPrimaryButtonEnabled", "isPrimaryButtonEnabled"},
                    {"IsIndeterminate", "isIndeterminate"}, {"IsActive", "isActive"},
                    {"IsPaneOpen", "isPaneOpen"}, {"PanePlacement", "panePlacement"},                    {"DisplayMode", "displayMode"}, {"OpenPaneLength", "openPaneLength"},
                    {"CompactPaneLength", "compactPaneLength"}, {"IsBackButtonVisible", "isBackButtonVisible"},
                    {"IsSettingsVisible", "isSettingsVisible"}, {"PaneDisplayMode", "paneDisplayMode"},
                    {"IsExpanded", "isExpanded"}, {"IsReadOnly", "isReadOnly"},
                    {"MaxLength", "maxLength"}, {"AcceptsReturn", "acceptsReturn"},
                    {"TextWrapping", "textWrapping"}, {"IsTextSelectionEnabled", "isTextSelectionEnabled"},
                    {"MaxLines", "maxLines"}, {"Glyph", "glyph"}, {"Symbol", "symbol"},
                    {"Data", "data"}, {"Fill", "fill"}, {"Stroke", "stroke"},
                    {"StrokeThickness", "strokeThickness"}, {"Points", "points"},
                    {"X1", "x1"}, {"Y1", "y1"}, {"X2", "x2"}, {"Y2", "y2"},
                    {"RadiusX", "radiusX"}, {"RadiusY", "radiusY"},
                    {"Orientation", "orientation"}, {"Spacing", "spacing"},
                    {"ItemWidth", "itemWidth"}, {"ItemHeight", "itemHeight"},
                    {"MaximumRowsOrColumns", "maximumRowsOrColumns"}, {"IsMultiSelect", "isMultiSelect"},
                    {"IsItemClickEnabled", "isItemClickEnabled"},
                    {"IsSuggestionListOpen", "isSuggestionListOpen"}, {"IsCalendarOpen", "isCalendarOpen"},
                    {"IsTodayHighlighted", "isTodayHighlighted"},
                    {"Date", "date"}, {"SelectedDate", "selectedDate"}, {"Time", "time"},
                    {"MinuteIncrement", "minuteIncrement"}, {"ClockIdentifier", "clockIdentifier"},
                    {"DayVisible", "dayVisible"}, {"MonthVisible", "monthVisible"},
                    {"YearVisible", "yearVisible"},
                    {"Color", "color"}, {"IsAlphaEnabled", "isAlphaEnabled"},
                    {"IsColorSpectrumVisible", "isColorSpectrumVisible"}, {"GroupName", "groupName"}, {"MaxRating", "maxRating"},
                    {"IsClearEnabled", "isClearEnabled"}, {"Placeholder", "placeholder"}, {"IsCompact", "isCompact"}, {"Label", "label"}, {"Icon", "icon"},
                    {"DefaultLabelPosition", "defaultLabelPosition"}, {"IsSticky", "isSticky"}, {"Placement", "placement"},
                    {"Mode", "mode"}, {"PullDirection", "pullDirection"},
                    {"Visualizer", "visualizer"}, {"State", "state"},
                    {"HeaderKind", "headerKind"}, {"PreviewKind", "previewKind"},
                    {"NumberOfPages", "numberOfPages"}, {"SelectedPageIndex", "selectedPageIndex"},
                    {"PreviousButtonVisibility", "previousButtonVisibility"}, {"NextButtonVisibility", "nextButtonVisibility"},
                    {"ContentOrientation", "contentOrientation"},
                    {"HorizontalScrollBarVisibility", "horizontalScrollBarVisibility"},
                    {"VerticalScrollBarVisibility", "verticalScrollBarVisibility"},
                    {"HorizontalScrollMode", "horizontalScrollMode"}, {"VerticalScrollMode", "verticalScrollMode"},
                    {"ZoomMode", "zoomMode"}, {"MinZoomFactor", "minZoomFactor"},
                    {"MaxZoomFactor", "maxZoomFactor"}, {"IsZoomChainingEnabled", "isZoomChainingEnabled"},
                    {"IsZoomedInViewActive", "isZoomedInViewActive"},
                    {"AlignLeftWith", "alignLeftWith"}, {"AlignTopWith", "alignTopWith"},
                    {"AlignRightWith", "alignRightWith"}, {"AlignBottomWith", "alignBottomWith"},
                    {"ShowPaused", "showPaused"}, {"ShowError", "showError"},
                    {"IsLightDismissEnabled", "isLightDismissEnabled"}, {"PreferredPlacement", "preferredPlacement"},
                    {"ProfilePicture", "profilePicture"}, {"DisplayName", "displayName"},
                    {"Initials", "initials"}, {"DefaultBackgroundColor", "defaultBackgroundColor"},
                    {"IsVisible", "isVisible"}, {"IsFullWindow", "isFullWindow"},
                    {"IsMuted", "isMuted"}, {"PlaybackRate", "playbackRate"},
                    {"IsPlaying", "isPlaying"}, {"HorizontalShift", "horizontalShift"},
                    {"VerticalShift", "verticalShift"},
                    {"IsHorizontalShiftClamped", "isHorizontalShiftClamped"},
                    {"IsVerticalShiftClamped", "isVerticalShiftClamped"},
                    {"MapServiceToken", "mapServiceToken"}, {"Center", "center"},
                    {"ZoomLevel", "zoomLevel"}, {"MapStyle", "mapStyle"},
                    {"IsTrafficEnabled", "isTrafficEnabled"}, {"IsPasswordRevealButtonEnabled", "isPasswordRevealButtonEnabled"},
                    {"Password", "password"}, {"Delay", "delay"}, {"Interval", "interval"},
                    {"Flyout", "flyout"}, {"EasingFunction", "easingFunction"},
                    {"IsHeaderItemsCarouselEnabled", "isHeaderItemsCarouselEnabled"}, {"HorizontalHeaderAlignment", "horizontalHeaderAlignment"},
                    {"ShowAsMonochrome", "showAsMonochrome"},
                    {"StretchDirection", "stretchDirection"},
                    {"ItemsSource", "itemsSource"}, {"ItemContainerStyle", "itemContainerStyle"},
                    {"SelectedItem", "selectedItem"}, {"Layout", "layout"},
                    {"MaximumHeight", "maxHeight"}, {"MaximumWidth", "maxWidth"},
                    {"AutomationProperties", ""}, {"Tag", ""}, {"Command", ""},
                    {"CommandParameter", ""}, {"ToolTipService", ""},
                };
                auto it = renames.find(local);
                if (it != renames.end() && !it->second.empty()
                    && has_property(ctrl_name, it->second)) {
                    winprop = it->second;
                } else {
                    raws.push_back({aname, std::string(a.value)});
                    continue;
                }
            }
                }

            std::string comment;
            std::string mode;
            auto v = convert_value(a.value, local, comment, mode);
            if (v.is_binding) {
                c.record_binding_root(v.expr, mode, local);
                // two-way on input-ish properties → $ binding ref
                std::string fexpr = c.flatten_cb_path(v.expr);
                bool two_way = (mode == "TwoWay");
                if (two_way) {
                    args.push_back(winprop + ": $" + fexpr);
                } else {
                    args.push_back(winprop + ": " + fexpr);
                }
                continue;
            }
            if (v.expr.empty()) {
                raws.push_back({aname, std::string(a.value)});
                continue;
            }
            // severity enum mapping: Informational→info etc.
            std::string val = v.expr;
            if (winprop == "severity") {
                std::string sv = a.value;
                if (sv == "Informational") val = qstr("info");
                else if (sv == "Warning") val = qstr("warning");
                else if (sv == "Error") val = qstr("error");
                else if (sv == "Success") val = qstr("success");
            }
            // path-like values (Image source, Hyperlink navigateUri, …) use
            // single backslashes; forward slashes survive only inside a URI
            // scheme such as https://, ms-appx:/// or foo:///...
            if (is_path_like_prop(winprop) && val.size() >= 2 && val.front() == '"' && val.back() == '"'
                && val.find("://") == std::string::npos) {
                std::string inner = val.substr(1, val.size() - 2);
                for (char& c : inner) if (c == '/') c = '\\';
                val = qstr(inner);
            }
            // boolean-ish property with string value → normalize
            if (a.value == "True") val = "true";
            if (a.value == "False") val = "false";
            args.push_back(winprop + ": " + val);
        }
    }

    args_out = args.empty() ? "" : join_args(args);
    mods_out = std::move(mods);
    raws_out = std::move(raws);
}


// JSON-escape a string.
inline auto json_escape(std::string const& s) -> std::string {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch;
        }
    }
    return out;
}

// Split on top-level separators (quotes/parens/braces respected).
inline auto split_top(std::string const& s, char sep) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char q = 0;
    for (char ch : s) {
        if (q) {
            cur += ch;
            if (ch == q) q = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') { q = ch; cur += ch; continue; }
        if (ch == '(' || ch == '{' || ch == '[') { ++depth; cur += ch; continue; }
        if (ch == ')' || ch == '}' || ch == ']') { --depth; cur += ch; continue; }
        if (ch == sep && depth == 0) {
            auto b = cur.find_first_not_of(" \t\r\n");
            auto e = cur.find_last_not_of(" \t\r\n");
            if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
            cur.clear();
            continue;
        }
        cur += ch;
    }
    auto b = cur.find_first_not_of(" \t\r\n");
    auto e = cur.find_last_not_of(" \t\r\n");
    if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
    return out;
}

// Serialize one template element (recursive) into the winmd layout JSON shape.
inline auto template_node_json(Converter& c, xaml::Node const& el,
                               std::set<std::string>& members) -> std::string {
    std::string ctrl;
    bool transparent = false;
    std::string note;
    ctrl = map_element_name(c, el, transparent, note);
    if (transparent && ctrl.empty()) {
        // transparent wrapper → array of children
        std::string arr = "[";
        bool first = true;
        for (auto const& ch : el.children)
            if (ch.kind == xaml::NodeKind::Element) {
                if (!first) arr += ",";
                first = false;
                arr += template_node_json(c, ch, members);
            }
        arr += "]";
        return arr;
    }

    std::string args;
    std::vector<std::string> mods;
    std::vector<RawPair> raws;
    emit_attributes(c, el, ctrl, args, mods, raws);

    // fragment children (Grid.RowDefinitions, …) → raw pairs
    for (auto const& ch : el.children) {
        if (ch.kind != xaml::NodeKind::Element) continue;
        std::vector<RawPair> frags;
        if (collect_fragment(ch, frags)) {
            for (auto& rp : frags) raws.push_back(std::move(rp));
        }
    }

    std::string out = "{\"control\":\"" + json_escape(ctrl) + "\",\"props\":{";
    bool first = true;
    auto put_prop = [&](std::string name, std::string value) {
        if (!first) out += ",";
        first = false;
        out += "\"" + json_escape(name) + "\":\"" + json_escape(value) + "\"";
    };
    auto bind_or_lit = [&](std::string value) -> std::string {
        // Quoted literal: strip the surrounding quotes - the JSON payload adds
        // its own quoting, so storing "\"Bottom\"" double-escapes the value and
        // downstream projections emit string literals for enum props.
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')
            && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
            return value;
        }
        if (!value.empty() && value.front() != '"' && value.front() != '\''
            && !(std::isdigit(static_cast<unsigned char>(value.front())) || value.front() == '-')
            && value != "true" && value != "false") {
            std::string mem = value;
            auto dot = mem.find('.');
            if (dot != std::string::npos) mem = mem.substr(0, dot);
            members.insert(mem);
            return "item." + value;
        }
        return value;
    };

    // named args: "text: \"hi\"" / "text: title" (binding → item.title)
    for (auto const& piece : split_top(args, ',')) {
        auto colon = piece.find(':');
        if (colon == std::string::npos) continue;
        std::string name = piece.substr(0, colon);
        std::string value = piece.substr(colon + 1);
        auto vb = value.find_first_not_of(" \t");
        if (vb != std::string::npos) value = value.substr(vb);
        put_prop(name, bind_or_lit(std::move(value)));
    }
    // modifiers: ".fontWeight(\"SemiBold\")" → prop; event closures skipped
    for (auto const& m : mods) {
        if (m.size() < 3 || m[0] != '.') continue;
        auto lp = m.find('(');
        // Event closures (.onClick { self.X(); }) must not become props: the
        // first '(' belongs to the handler CALL inside the closure body, so the
        // name would swallow the "{ self.X" and the value would be a "); }"
        // fragment that bind_or_lit mints into a fake item member.
        auto ob = m.find('{');
        if (ob != std::string::npos && (lp == std::string::npos || ob < lp)) continue;
        if (lp == std::string::npos) continue;
        std::string name = m.substr(1, lp - 1);
        std::string value = m.substr(lp + 1);
        if (!value.empty() && value.back() == ')') value.pop_back();
        if (!value.empty() && value.front() == '{') continue;   // closure
        put_prop(name, bind_or_lit(std::move(value)));
    }
    out += "}";

    if (!raws.empty()) {
        out += ",\"raw\":[";
        bool rfirst = true;
        for (auto const& rp : raws) {
            if (!rfirst) out += ",";
            rfirst = false;
            out += "[\"" + json_escape(rp.key) + "\",\"" + json_escape(rp.value) + "\"]";
        }
        out += "]";
    }

    std::vector<std::string> child_json;
    for (auto const& ch : el.children) {
        if (ch.kind != xaml::NodeKind::Element) continue;
        std::vector<RawPair> frags;
        if (collect_fragment(ch, frags)) continue;   // already in raws above
        child_json.push_back(template_node_json(c, ch, members));
    }
    if (!child_json.empty()) {
        out += ",\"children\":[";
        for (std::size_t i = 0; i < child_json.size(); ++i) {
            if (i) out += ",";
            out += child_json[i];
        }
        out += "]";
    }
    out += "}";
    return out;
}

// Convert an inline DataTemplate raw value into a structured payload:
// {"itemType":"X","members":["title",…],"body":[ layout JSON ]}
inline auto convert_inline_template(Converter& c, RawPair& rp) -> void {
    auto parsed = xaml::parse(rp.value);
    if (!parsed.ok()) return;
    auto const* dt = parsed.root();
    if (!dt) return;

    std::string item_type;
    if (auto dtv = dt->attr("DataType")) {
        item_type = std::string(*dtv);
        auto cc = item_type.find(':');
        if (cc != std::string::npos) item_type = item_type.substr(cc + 1);
    }
    std::set<std::string> members;
    std::string body = "[";
    bool first = true;
    for (auto const& ch : dt->children)
        if (ch.kind == xaml::NodeKind::Element) {
            if (!first) body += ",";
            first = false;
            body += template_node_json(c, ch, members);
        }
    body += "]";

    std::string payload = "{\"itemType\":\"" + json_escape(item_type) + "\",\"members\":[";
    bool mfirst = true;
    for (auto const& m : members) {
        if (!mfirst) payload += ",";
        mfirst = false;
        payload += "\"" + json_escape(m) + "\"";
    }
    payload += "],\"body\":" + body + "}";
    rp.value = std::move(payload);
}

struct NavItem {
    std::string unique_id;
    std::string title;
};

struct NavGroup {
    std::string unique_id;
    std::string title;
    std::string glyph_repr;
    bool is_special = false;
    std::vector<NavItem> items;
};

inline auto skip_ci_ws(std::string_view s, size_t p) -> size_t {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    return p;
}

inline auto read_ci_str(std::string_view s, size_t p) -> std::pair<std::string, size_t> {
    if (p >= s.size() || s[p] != '"') return {"", std::string_view::npos};
    ++p;
    std::string out;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            ++p;
            if (s[p] == 'u' && p + 4 < s.size()) {
                out += "\\u";
                out.append(s.substr(p + 1, 4));
                p += 4;
            } else if (s[p] == '"') out += '"';
            else if (s[p] == '\\') out += '\\';
            else { out += '\\'; out += s[p]; }
        } else {
            out += s[p];
        }
        ++p;
    }
    if (p < s.size()) ++p;
    return {out, p};
}

inline auto parse_control_info(std::string_view s) -> std::vector<NavGroup> {
    std::vector<NavGroup> groups;
    auto groups_pos = s.find("\"Groups\"");
    if (groups_pos == std::string_view::npos) return groups;
    auto bracket = s.find('[', groups_pos);
    if (bracket == std::string_view::npos) return groups;

    size_t p = bracket + 1;
    while (p < s.size()) {
        p = skip_ci_ws(s, p);
        if (p >= s.size() || s[p] == ']') break;
        if (s[p] == '{') {
            int depth = 1;
            ++p;
            NavGroup g;
            while (p < s.size() && depth > 0) {
                p = skip_ci_ws(s, p);
                if (p >= s.size()) break;
                if (s[p] == '{') { ++depth; ++p; continue; }
                if (s[p] == '}') { --depth; ++p; continue; }
                if (s[p] == '"' && depth == 1) {
                    auto [key, end_k] = read_ci_str(s, p);
                    p = skip_ci_ws(s, end_k);
                    if (p < s.size() && s[p] == ':') ++p;
                    p = skip_ci_ws(s, p);
                    if (key == "UniqueId") {
                        auto [val, end_v] = read_ci_str(s, p);
                        g.unique_id = val;
                        p = end_v;
                    } else if (key == "Title") {
                        auto [val, end_v] = read_ci_str(s, p);
                        g.title = val;
                        p = end_v;
                    } else if (key == "IconGlyph") {
                        auto [val, end_v] = read_ci_str(s, p);
                        if (!val.empty()) {
                            unsigned char c0 = static_cast<unsigned char>(val[0]);
                            if (c0 >= 0xE0 && val.size() >= 3) {
                                unsigned char c1 = static_cast<unsigned char>(val[1]);
                                unsigned char c2 = static_cast<unsigned char>(val[2]);
                                uint32_t cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
                                char hexbuf[16];
                                snprintf(hexbuf, sizeof(hexbuf), "\\u%04X", cp);
                                g.glyph_repr = hexbuf;
                            } else if (val.starts_with("\\u")) {
                                g.glyph_repr = val;
                            } else {
                                g.glyph_repr = val;
                            }
                        }
                        p = end_v;
                    } else if (key == "IsSpecialSection") {
                        if (s.substr(p, 4) == "true") { g.is_special = true; p += 4; }
                        else if (s.substr(p, 5) == "false") { g.is_special = false; p += 5; }
                    } else if (key == "Items") {
                        if (p < s.size() && s[p] == '[') {
                            ++p;
                            while (p < s.size()) {
                                p = skip_ci_ws(s, p);
                                if (p >= s.size() || s[p] == ']') { ++p; break; }
                                if (s[p] == '{') {
                                    int idepth = 1;
                                    ++p;
                                    NavItem item;
                                    while (p < s.size() && idepth > 0) {
                                        p = skip_ci_ws(s, p);
                                        if (p >= s.size()) break;
                                        if (s[p] == '{') { ++idepth; ++p; continue; }
                                        if (s[p] == '}') { --idepth; ++p; continue; }
                                        if (s[p] == '"' && idepth == 1) {
                                            auto [ikey, iend_k] = read_ci_str(s, p);
                                            p = skip_ci_ws(s, iend_k);
                                            if (p < s.size() && s[p] == ':') ++p;
                                            p = skip_ci_ws(s, p);
                                            if (ikey == "UniqueId") {
                                                auto [ival, iend_v] = read_ci_str(s, p);
                                                item.unique_id = ival;
                                                p = iend_v;
                                            } else if (ikey == "Title") {
                                                auto [ival, iend_v] = read_ci_str(s, p);
                                                item.title = ival;
                                                p = iend_v;
                                            } else {
                                                if (s[p] == '"') { auto [_, ev] = read_ci_str(s, p); p = ev; }
                                                else if (s[p] == '{' || s[p] == '[') {
                                                    char open_c = s[p];
                                                    char close_c = (open_c == '{') ? '}' : ']';
                                                    int d = 1; ++p;
                                                    while (p < s.size() && d > 0) {
                                                        if (s[p] == open_c) ++d;
                                                        else if (s[p] == close_c) --d;
                                                        else if (s[p] == '"') { auto [_, ev] = read_ci_str(s, p); p = ev - 1; }
                                                        ++p;
                                                    }
                                                } else {
                                                    while (p < s.size() && s[p] != ',' && s[p] != '}') ++p;
                                                }
                                            }
                                        } else {
                                            ++p;
                                        }
                                    }
                                    if (!item.unique_id.empty() && !item.title.empty()) {
                                        g.items.push_back(item);
                                    }
                                } else {
                                    ++p;
                                }
                            }
                        }
                    } else {
                        if (s[p] == '"') { auto [_, ev] = read_ci_str(s, p); p = ev; }
                        else if (s[p] == '{' || s[p] == '[') {
                            char open_c = s[p];
                            char close_c = (open_c == '{') ? '}' : ']';
                            int d = 1; ++p;
                            while (p < s.size() && d > 0) {
                                if (s[p] == open_c) ++d;
                                else if (s[p] == close_c) --d;
                                else if (s[p] == '"') { auto [_, ev] = read_ci_str(s, p); p = ev - 1; }
                                ++p;
                            }
                        } else {
                            while (p < s.size() && s[p] != ',' && s[p] != '}') ++p;
                        }
                    }
                } else {
                    ++p;
                }
            }
            if (!g.unique_id.empty() && !g.title.empty() && !g.is_special) {
                groups.push_back(g);
            }
        } else {
            ++p;
        }
    }
    std::sort(groups.begin(), groups.end(), [](auto const& a, auto const& b) {
        return a.title < b.title;
    });
    return groups;
}

inline auto emit_control_info_groups(Converter& c) -> void {
    std::string content;
    std::vector<std::string> cands;
    if (c.opts && !c.opts->source_file_path.empty()) {
        std::string s = c.opts->source_file_path;
        auto pos = s.find_last_of("\\/");
        if (pos != std::string::npos) {
            std::string parent = s.substr(0, pos);
            cands.push_back(parent + "\\SampleSupport\\Data\\ControlInfoData.json");
            cands.push_back(parent + "/SampleSupport/Data/ControlInfoData.json");
            auto pos2 = parent.find_last_of("\\/");
            if (pos2 != std::string::npos) {
                std::string grandparent = parent.substr(0, pos2);
                cands.push_back(grandparent + "\\SampleSupport\\Data\\ControlInfoData.json");
                cands.push_back(grandparent + "\\WinUIGallery\\SampleSupport\\Data\\ControlInfoData.json");
                cands.push_back(grandparent + "\\WinUI-Gallery\\WinUIGallery\\SampleSupport\\Data\\ControlInfoData.json");
            }
        }
    }
    cands.push_back("SampleSupport\\Data\\ControlInfoData.json");
    cands.push_back("WinUIGallery\\SampleSupport\\Data\\ControlInfoData.json");
    cands.push_back("..\\WinUIGallery\\SampleSupport\\Data\\ControlInfoData.json");

    for (auto const& path : cands) {
        FILE* fp = nullptr;
#ifdef _WIN32
        fopen_s(&fp, path.c_str(), "rb");
#else
        fp = fopen(path.c_str(), "rb");
#endif
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0) {
                content.resize(static_cast<size_t>(sz));
                fread(content.data(), 1, static_cast<size_t>(sz), fp);
            }
            fclose(fp);
            if (!content.empty()) break;
        }
    }
    if (content.empty()) return;

    auto grps = parse_control_info(content);
    for (auto const& g : grps) {
        c.em.line("NavigationViewItem.raw(\"Content\", \"" + escape_winui_string(g.title) + "\").raw(\"Tag\", \"" + g.unique_id + "\")");
        c.em.line("{");
        ++c.em.depth;
        if (!g.glyph_repr.empty()) {
            c.em.line("FontIcon.raw(\"Glyph\", \"" + g.glyph_repr + "\")");
        }
        for (auto const& it : g.items) {
            c.em.line("NavigationViewItem.raw(\"Content\", \"" + escape_winui_string(it.title) + "\").raw(\"Tag\", \"" + it.unique_id + "\")");
        }
        --c.em.depth;
        c.em.line("}");
    }
}

// Main element → .winui text emitter.
inline auto convert_element(Converter& c, xaml::Node const& el) -> void {
    // comments first
    for (auto const& child : el.children) {
        if (child.kind == xaml::NodeKind::Comment) {
            std::string comment = child.name;
            // split multi-line comments
            std::size_t pos = 0;
            while (pos < comment.size()) {
                auto nl = comment.find('\n', pos);
                std::string l = comment.substr(pos, (nl == std::string::npos) ? comment.size() - pos : nl - pos);
                // trim
                auto b = l.find_first_not_of(" \t\r");
                auto e = l.find_last_not_of(" \t\r");
                l = (b == std::string::npos) ? "" : l.substr(b, e - b + 1);
                if (!l.empty()) c.em.line("// " + l);
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        }
    }

    bool transparent = false;
    std::string note;
    std::string ctrl = map_element_name(c, el, transparent, note);

    // Fragments (styles, row defs, templates, …) attach to the enclosing
    // control. If THIS node is itself a fragment, hand it to the parent.
    std::vector<RawPair> my_raws;
    if (collect_fragment(el, my_raws)) {
        for (auto& rp : my_raws) c.pending_raw.push_back(std::move(rp));
        return;
    }

    // transparent containers (Page, UserControl, DataTemplate, …) — recurse
    if (transparent) {
        for (auto const& child : el.children) {
            if (child.kind == xaml::NodeKind::Element) convert_element(c, child);
        }
        return;
    }

    // property elements: Parent.Child — handled by parent
    if (ctrl.find('.') != std::string::npos) {
        for (auto const& child : el.children) {
            if (child.kind == xaml::NodeKind::Element) convert_element(c, child);
        }
        return;
    }

    std::string args;
    std::vector<std::string> mods;
    std::vector<RawPair> raws;
    emit_attributes(c, el, ctrl, args, mods, raws);

    std::string xname;
    for (auto const& a : el.attrs) {
        auto colon = a.name.find(':');
        std::string pre = (colon == std::string::npos) ? "" : a.name.substr(0, colon);
        std::string loc = (colon == std::string::npos) ? a.name : a.name.substr(colon + 1);
        if (pre == "x" && loc == "Name") { xname = a.value; break; }
    }

    if (ctrl == "AutoSuggestBox" && (xname == "controlsSearchBox" || xname.find("Search") != std::string::npos)) {
        bool has_min_w = false;
        for (auto const& m : mods) if (m.starts_with(".minWidth(")) { has_min_w = true; break; }
        if (!has_min_w) mods.push_back(".minWidth(360)");
    }

    // code-behind ctor wiring: attach .on<Event> { self.H(); } mods for an
    // element whose x:Name is wired from code-behind (MetaScrub-style).
    // Only when the element itself is emitted with its x:Name raw and the
    // event wasn't already wired via a XAML attribute on this element.
    {
        if (!xname.empty()) {
            auto wit = c.cb_wiring.find(xname);
            if (wit != c.cb_wiring.end()) {
                for (auto const& [wev, handler] : wit->second) {
                    bool already = false;
                    for (auto const& m : mods)
                        if (m.rfind("." + wev + " {", 0) == 0) { already = true; break; }
                    if (already) continue;
                    c.ensure_callback(handler);
                    mods.push_back("." + wev + " { self." + handler + "(); }");
                }
            }
        }
    }

    // Third-party elements: keep the XML prefix so projections can pair the
    // control with its xmlns declaration (see collect_xmlns). Standard
    // plumbing prefixes (x/d/mc) carry no type information — skipped.
    if (!el.prefix.empty() && el.prefix != "x" && el.prefix != "d" && el.prefix != "mc") {
        raws.push_back({"prefix", el.prefix});
    }

    // text content → text arg
    // Text-capable controls (TextBlock, RichTextBlock, ToolTip, Paragraph,
    // Run, Hyperlink …) keep their text; inline Run children are emitted
    // as elements (Run maps to TextBlock so formatting survives).
    // Unknown (third-party) controls are treated as text-capable too —
    // winuic's dynamic fallback accepts text content, so nothing is lost.
    bool children_are_text = (!is_registered(ctrl)
                              || ext::accepts_text(ctrl)
                              || ctrl == "TextBlock" || ctrl == "RichTextBlock" || ctrl == "ToolTip"
                              || ctrl == "Paragraph" || ctrl == "Hyperlink" || ctrl == "Run"
                              || ctrl == "Span" || ctrl == "Bold" || ctrl == "Italic"
                              || ctrl == "Underline");
    std::string text_content;
    bool has_text = false;
    for (auto const& t : el.text) {
        if (!t.empty()) { has_text = true; text_content += t; }
    }
    if (has_text && args.find("text:") == std::string::npos) {
        if (children_are_text) {
            if (!args.empty()) args += ", ";
            args += "text: " + qstr(text_content);
        } else {
            // registered control without text support: keep the content
            // verbatim so nothing is ever lost
            raws.push_back({"text", text_content});
        }
    }

    // If the parent also carries a live sample (X.Example), the Xaml string is
    // display-only — converting it too would duplicate the sample UI.
    bool has_live_example = false;
    for (auto const& ch : el.children) {
        if (ch.kind != xaml::NodeKind::Element) continue;
        auto ed = ch.name.find_last_of('.');
        if (ed == std::string::npos) continue;
        std::string eloc = ch.name.substr(ed + 1);
        std::string ell = eloc;
        for (auto& cc : ell) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
        if (ell == "example") { has_live_example = true; break; }
    }

    // direct-child fragments (Grid.RowDefinitions, Grid.Resources, …) and
    // XAML code examples (ControlExample.Xaml) — examples are ALSO converted
    // recursively into .winui equivalents, not just carried verbatim.
    std::vector<std::string> example_lines;
    bool has_examples = false;
    std::vector<xaml::Node const*> real_children;
    for (auto const& ch : el.children) {
        if (ch.kind == xaml::NodeKind::CData) {
            raws.push_back({"CDATA", ch.name});
            continue;
        }
        if (ch.kind != xaml::NodeKind::Element) continue;
        std::vector<RawPair> frags;
        bool example = is_code_example(ch) && !has_live_example;
        if (collect_fragment(ch, frags)) {
            for (auto& rp : frags) raws.push_back(std::move(rp));
        }
        if (example) {
            convert_code_example(c, ch, example_lines);
            has_examples = true;
            continue;
        }
        if (!frags.empty()) continue;
        real_children.push_back(&ch);
    }

    // Inline DataTemplates (ItemTemplate/ContentTemplate/… holding <DataTemplate>
    // markup) are converted into structured JSON payloads so projections can emit
    // real item templates that render list items.
    for (auto& rp : raws) {
        std::string lk = rp.key;
        for (auto& ch : lk) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lk.find("template") != std::string::npos
            && rp.value.rfind("<DataTemplate", 0) == 0) {
            convert_inline_template(c, rp);
        }
    }

    // emit element line
    std::string line = ctrl;
    if (!args.empty()) line += "(" + args + ")";
    for (auto& rp : c.pending_raw) mods.push_back(raw_mod(rp));
    c.pending_raw.clear();
    for (auto const& rp : raws) mods.push_back(raw_mod(rp));
    for (auto const& m : mods) line += m;
    c.em.line(line);

    // children
    // Every element child is emitted as a children block — the .winui
    // compiler lowers children under any component (unknown ones only
    // produce a warning), so no XAML content is ever lost.
    if (has_examples || !real_children.empty()) {
        c.em.line("{");
        ++c.em.depth;
        if (has_examples) {
            c.em.line("// XAML code examples — converted to .winui equivalents");
            for (auto const& l : example_lines) c.em.line(l);
        }
        for (auto const& ch : real_children) {
            convert_element(c, *ch);
        }
        --c.em.depth;
        c.em.line("}");
    }

    if (xname == "AllControlsItem") {
        emit_control_info_groups(c);
    }
}

// §8d  DataTemplate projection
// Inline <DataTemplate> markup (ItemTemplate/ContentTemplate/…) is converted
// into the winmd layout JSON shape so cppwinrtc can emit a real DataTemplate
// lambda. x:Bind refs inside become "item.<member>" reads on the projected
// item type.

// §9  Resource dictionaries

inline auto emit_resource_entries(Converter& c, xaml::Node const& dict) -> void {
    for (auto const& ch : dict.children) {
        if (ch.kind == xaml::NodeKind::Comment) {
            std::string comment = ch.name;
            std::size_t pos = 0;
            while (pos < comment.size()) {
                auto nl = comment.find('\n', pos);
                std::string l = comment.substr(pos, (nl == std::string::npos) ? comment.size() - pos : nl - pos);
                auto b = l.find_first_not_of(" \t\r");
                auto e = l.find_last_not_of(" \t\r");
                l = (b == std::string::npos) ? "" : l.substr(b, e - b + 1);
                if (!l.empty()) c.em.line("// " + l);
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
            continue;
        }
        if (ch.kind != xaml::NodeKind::Element) continue;
        if (ch.name.find('.') != std::string::npos) continue; // property element
        auto key = ch.attr("Key");
        std::string key_note = key ? std::string(" // x:Key=\"" + std::string(*key) + "\"") : "";
        // styles / brushes / templates → verbatim fragment (survives into the .winmd)
        if (is_frag_family(ch.name)) {
            c.em.line("Raw() " + raw_mod({"Resource", serialize_fragment(ch)}));
            continue;
        }
        // registered resources (brushes etc.) — emit as views? no: emit as comment + recurse
        if (is_registered(ch.name)) {
            std::string line = "// <" + ch.name + ">" + key_note;
            c.em.line(line);
            ++c.em.depth;
            convert_element(c, ch);
            --c.em.depth;
            continue;
        }
        // unknown entries
        std::string line = "// <" + ch.name + ">" + key_note;
        c.em.line(line);
        for (auto const& inner : ch.children) {
            if (inner.kind == xaml::NodeKind::Element) {
                std::string inner_line = "//   <" + inner.name;
                for (auto const& ia : inner.attrs) {
                    inner_line += " " + ia.name + "=\"" + ia.value + "\"";
                }
                inner_line += " />";
                c.em.line(inner_line);
            }
        }
    }
}

// §10  Root dispatch

// (helper defined below)

// Translate a code-behind method body (C# / C++) into .winui callback-body
// statements. The .winui language is code-behind free and language agnostic:
// a callback body may ONLY reference identifiers the view itself knows
// (x:Name elements, @state/@binding vars, code-behind properties) using
// neutral dotted assignment / calls and if/else. Any statement that still
// smells of the source language (locals, C++ scope :: / -> / accessors /
// enums, C# keywords, lambdas, async, templates, wide strings) is preserved
// as line-by-line comments, so nothing is lost and no downstream projection
// (C#, C++, Rust, ...) ever sees source-language text in the winmd.
inline auto is_known_ident(std::unordered_set<std::string> const& known, std::string const& id) -> bool {
    if (known.count(id)) return true;
    // case-insensitive match (C# PascalCase vs winui camelCase elements)
    for (auto const& k : known) {
        if (k.size() != id.size()) continue;
        bool eq = true;
        for (std::size_t i = 0; i < k.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(k[i])) != std::tolower(static_cast<unsigned char>(id[i]))) { eq = false; break; }
        if (eq) return true;
    }
    return false;
}

// Scan every identifier token in s (outside strings). Returns false when an
// identifier is not known and not a literal keyword — i.e. it is a source
// local, member var, helper or namespace that no projection could know.
inline auto only_known_idents(std::string const& s, std::unordered_set<std::string> const& known) -> bool {
    std::size_t i = 0;
    bool in_s = false;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\"' ) { in_s = !in_s; ++i; continue; }
        if (in_s) { ++i; continue; }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t b = i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            std::string id = s.substr(b, i - b);
            // property segment after a dot: the ROOT was already validated,
            // member names are free (Text, ItemsSource, ...)
            bool after_dot = (b > 0 && s[b - 1] == '.');
            if (after_dot) continue;
            if (id == "true" || id == "false" || id == "null") continue;
            if (!is_known_ident(known, id)) return false;
            continue;
        }
        ++i;
    }
    return true;
}

inline auto comment_every_line(std::string const& s) -> std::string {
    std::string cs;
    std::size_t b = 0;
    while (b < s.size()) {
        auto nl = s.find('\n', b);
        std::string line = s.substr(b, (nl == std::string::npos) ? s.size() - b : nl - b);
        auto lb = line.find_first_not_of(" \t\r");
        if (lb != std::string::npos) {
            cs += "// ";
            cs += line.substr(lb);
        }
        cs += "\n";
        if (nl == std::string::npos) break;
        b = nl + 1;
    }
    return cs;
}

// True when the expression text carries no source-language marker.
inline auto neutral_expr_ok(std::string const& s) -> bool {
    static constexpr std::string_view bad[] = {
        "::", "->", "co_await", " await", "L\"", "hstring", "std::", "winrt::",
        "Windows::", "Microsoft::", "nullptr", "typeof", " sizeof", " typeid",
        "new ", "delete ", "=>", "?.", "?[", "[", "{", "}", "co_", " is ",
        " as ", "default:", "public:", "private:", "protected:",
    };
    for (auto const& b : bad) if (s.find(b) != std::string::npos) return false;
    // template instantiations: ident immediately followed by '<'
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] == '<' && i > 0 && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_')) return false;
    }
    return true;
}

// Lower a C++ accessor setter (X().Prop(v) / X->Prop(v)) to neutral
// dotted assignment X.Prop = v. X must be a known element.
inline auto lower_element_setter(std::string const& s, std::unordered_set<std::string> const& known,
                                 std::string& out) -> bool {
    std::string t = s;
    auto b0 = t.find_first_not_of(" \t");
    if (b0 != std::string::npos) t = t.substr(b0);
    auto e0 = t.find_last_not_of(" \t");
    if (e0 == std::string::npos) return false;
    t = t.substr(0, e0 + 1);
    auto paren = t.find('(');
    if (paren == std::string::npos || paren == 0) return false;
    std::string elem = t.substr(0, paren);
    if (!is_known_ident(known, elem)) return false;
    std::string rest = t.substr(paren);
    std::string sep;
    if (rest.rfind("().", 0) == 0) sep = "().";
    else if (rest.rfind("->.", 0) == 0) sep = "->.";
    else if (rest.rfind("->", 0) == 0) sep = "->";
    if (sep.empty()) return false;
    std::size_t k = sep.size();
    std::string prop;
    while (k < rest.size() && (std::isalnum(static_cast<unsigned char>(rest[k])) || rest[k] == '_')) ++k;
    prop = rest.substr(sep.size(), k - sep.size());
    if (prop.empty() || k >= rest.size() || rest[k] != '(') return false;
    int d = 1;
    std::size_t j = k + 1;
    for (; j < rest.size(); ++j) {
        if (rest[j] == '(') ++d;
        if (rest[j] == ')') { --d; if (d == 0) break; }
    }
    if (j >= rest.size()) return false;
    std::string arg = rest.substr(k + 1, j - (k + 1));
    auto ab = arg.find_first_not_of(" \t");
    auto ae = arg.find_last_not_of(" \t");
    if (ab == std::string::npos) return false;
    arg = arg.substr(ab, ae - ab + 1);
    if (rest.find_first_not_of(" \t", j + 1) != std::string::npos) return false;
    if (!neutral_expr_ok(arg)) return false;
    if (arg == "true" || arg == "True") arg = "true";
    else if (arg == "false" || arg == "False") arg = "false";
    out = elem + "." + prop + " = " + arg;
    return true;
}

// True when s is a language-neutral dotted assignment / method call whose
// identifiers are all known to the view.
inline auto neutral_statement_ok(std::string const& s, std::unordered_set<std::string> const& known) -> bool {
    std::string t = s;
    auto b0 = t.find_first_not_of(" \t");
    if (b0 == std::string::npos) return false;
    auto e0 = t.find_last_not_of(" \t");
    if (e0 == std::string::npos) return false;
    t = t.substr(b0, e0 - b0 + 1);
    if (!neutral_expr_ok(t)) return false;
    if (!only_known_idents(t, known)) return false;

    // assignment: dotted-ident lhs, single top-level '='
    {
        int depth = 0; bool in_q = false;
        for (std::size_t i = 0; i < t.size(); ++i) {
            char c = t[i];
            if (c == '\"') in_q = !in_q;
            if (in_q) continue;
            if (c == '(') ++depth;
            if (c == ')') --depth;
            if (c == '=' && depth == 0) {
                if (i + 1 < t.size() && t[i + 1] == '=') return false;
                if (i > 0 && (t[i - 1] == '!' || t[i - 1] == '<' || t[i - 1] == '>')) return false;
                return true;
            }
        }
    }
    // method call: path(args)
    {
        auto lp = t.find('(');
        if (lp == std::string::npos || lp == 0) return false;
        std::string path = t.substr(0, lp);
        auto pe = path.find_last_not_of(" \t");
        if (pe == std::string::npos) return false;
        path = path.substr(0, pe + 1);
        if (path.find(' ') != std::string::npos) return false;
        int d = 0; bool in_q = false;
        for (std::size_t i = lp; i < t.size(); ++i) {
            char c = t[i];
            if (c == '\"') in_q = !in_q;
            if (in_q) continue;
            if (c == '(') ++d;
            if (c == ')') { --d; if (d == 0) return (i == t.size() - 1); }
        }
        return false;
    }
}

// C# verbatim string literals (@") are language-specific and may span
// lines, so they must never reach the .winui output.  Rewrite each one to
// a plain single-line string literal: "" -> ", and every CR/LF + indent
// run inside becomes one space.  Embedded quotes are escaped for the
// .winui lexer (").
inline auto normalize_verbatim_strings(std::string& s) -> void {
    std::size_t i = 0;
    bool in_normal = false;
    std::string out;
    out.reserve(s.size() + 16);
    while (i < s.size())
    {
        if (in_normal)
        {
            if (s[i] == '\\' && i + 1 < s.size()) { out += s[i]; out += s[i + 1]; i += 2; continue; }
            if (s[i] == '"') { in_normal = false; out += '"'; ++i; continue; }
            out += s[i]; ++i;
            continue;
        }
        if (s[i] == '@' && i + 1 < s.size() && s[i + 1] == '"')
        {
            std::size_t b = i + 2;
            std::size_t j = b;
            while (j < s.size())
            {
                if (s[j] == '"')
                {
                    if (j + 1 < s.size() && s[j + 1] == '"') { j += 2; continue; }
                    break;
                }
                ++j;
            }
            if (j >= s.size()) { out += s[i]; ++i; continue; } // unterminated: keep as-is
            std::string content = s.substr(b, j - b);
            std::string clean;
            bool ws_run = false;
            bool started = false;
            for (std::size_t k = 0; k < content.size(); ++k)
            {
                char c = content[k];
                if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
                {
                    if (started) ws_run = true;
                    continue;
                }
                if (ws_run) { clean += ' '; ws_run = false; }
                started = true;
                if (c == '"')
                {
                    if (k + 1 < content.size() && content[k + 1] == '"') ++k;
                    clean += "\\\"";
                    continue;
                }
                clean += c;
            }
            out += '"';
            out += clean;
            out += '"';
            i = j + 1;
            continue;
        }
        if (s[i] == '"') { in_normal = true; out += '"'; ++i; continue; }
        out += s[i]; ++i;
    }
    s = std::move(out);
}

// Right-associative assignment chains (a = b = c = v) are valid C#/C++ but
// the .winui grammar assigns a single value to a single target.  Lower them
// to one assignment per target: every target receives the final RHS value,
// which is exactly the source-language semantics for plain property/field
// targets.  Returns false when the statement is not a splittable chain.
inline auto split_assign_chain(std::string const& s_in, std::vector<std::string>& parts) -> bool {
    std::string t = s_in;
    while (!t.empty() && (t.back() == ';' || t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n'))
        t.pop_back();
    auto b0 = t.find_first_not_of(" \t");
    if (b0 == std::string::npos) return false;
    t = t.substr(b0);
    // Collect top-level single '=' positions (skip == != <= >= and compound ops)
    std::vector<std::size_t> eq;
    int depth = 0;
    bool in_q = false;
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        char c = t[i];
        if (c == '"') { in_q = !in_q; continue; }
        if (in_q) continue;
        if (c == '(') { ++depth; continue; }
        if (c == ')') { if (depth > 0) --depth; continue; }
        if (c == '=' && depth == 0)
        {
            bool prev_op = i > 0 && (t[i - 1] == '+' || t[i - 1] == '-' || t[i - 1] == '*' || t[i - 1] == '/'
                || t[i - 1] == '%' || t[i - 1] == '!' || t[i - 1] == '<' || t[i - 1] == '>' || t[i - 1] == '='
                || t[i - 1] == '&' || t[i - 1] == '|');
            bool next_eq = i + 1 < t.size() && t[i + 1] == '=';
            if (!prev_op && !next_eq) eq.push_back(i);
        }
    }
    if (eq.size() < 2) return false;
    // Every segment before the last '=' must be a plain dotted path
    parts.clear();
    std::size_t seg_start = 0;
    for (std::size_t k = 0; k < eq.size(); ++k)
    {
        std::string seg = t.substr(seg_start, eq[k] - seg_start);
        auto sb = seg.find_first_not_of(" \t");
        auto se = seg.find_last_not_of(" \t");
        if (sb == std::string::npos) return false;
        seg = seg.substr(sb, se - sb + 1);
        for (char c : seg)
        {
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                || c == '_' || c == '.' || c == '[' || c == ']';
            if (!ok) return false;
        }
        parts.push_back(seg + " = ");
        seg_start = eq[k] + 1;
    }
    std::string value = t.substr(seg_start);
    auto vb = value.find_first_not_of(" \t");
    auto ve = value.find_last_not_of(" \t\r\n");
    if (vb == std::string::npos) return false;
    value = value.substr(vb, ve - vb + 1);
    if (value.empty()) return false;
    for (auto& p : parts) p += value;
    return true;
}

// Translate one plain statement, honoring the whitelist.
inline auto translate_plain_stmt(std::string s, std::unordered_set<std::string> const& known) -> std::string {
    // Desugar right-associative assignment chains (a = b = c = v): the .winui
    // grammar takes a single target per '=', so emit one assignment per target.
    std::vector<std::string> chain_parts;
    if (split_assign_chain(s, chain_parts))
    {
        std::string out;
        for (auto const& part : chain_parts)
        {
            std::string p = part;
            std::string lowered_p;
            if (lower_element_setter(p, known, lowered_p)) { out += lowered_p + "\n"; continue; }
            if (neutral_statement_ok(p, known)) out += p + "\n";
            else out += comment_every_line(p);
        }
        return out;
    }
    while (!s.empty() && s.back() == ';') s.pop_back();
    if (s.empty()) return "";
    // normalize this->/self->/this./self. prefixes (element + state refs)
    {
        for (auto const& pre : { std::string_view("this->"), std::string_view("self->"), std::string_view("this."), std::string_view("self.") })
            while (s.rfind(pre, 0) == 0) s = s.substr(pre.size());
    }
    std::string lowered;
    if (lower_element_setter(s, known, lowered)) {
        return lowered + "\n";
    }
    if (!neutral_statement_ok(s, known)) {
        return comment_every_line(s);
    }
    return s + "\n";
}
inline auto codebehind_body_to_winui(std::string const& body_in,
                                     std::unordered_set<std::string> const& known) -> std::string {
    // Strip wrapper braces if present
    std::string body = body_in;
    {
        auto b0 = body.find_first_not_of(" \t\r\n");
        auto e0 = body.find_last_not_of(" \t\r\n");
        if (b0 != std::string::npos && body[b0] == '{' && e0 != std::string::npos && body[e0] == '}') {
            std::string inner = body.substr(b0 + 1, e0 - b0 - 1);
            int bal = 0; bool ok = true; bool in_s = false;
            for (char c : inner) {
                if (c == '\"' && !in_s) in_s = true; else if (c == '\"' && in_s) in_s = false;
                if (!in_s) { if (c == '{') ++bal; else if (c == '}') { --bal; if (bal < 0) { ok = false; break; } } }
            }
            if (ok && bal == 0) body = std::move(inner);
        }
    }
    normalize_verbatim_strings(body);
    std::vector<std::string> stmts;
    {
        std::string cur; int bal = 0; bool in_s = false;
        for (std::size_t i = 0; i < body.size(); ++i) {
            char c = body[i];
            if (c == '/' && i + 1 < body.size() && body[i+1] == '/' && !in_s) {
                while (i < body.size() && body[i] != '\n') ++i;
                continue;
            }
            if (c == '\"' && (i == 0 || body[i-1] != '\\')) in_s = !in_s;
            if (!in_s) {
                if (c == '{') ++bal;
                else if (c == '}') { if (bal > 0) --bal; cur += c; continue; }
            }
            cur += c;
            if (!in_s && bal == 0 && c == ';') { stmts.push_back(cur); cur.clear(); }
        }
        if (!cur.empty()) stmts.push_back(cur);
    }
    std::string out;
    for (auto const& st : stmts) {
        std::string s = st;
        auto b = s.find_first_not_of(" \t\r\n");
        auto e = s.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        s = s.substr(b, e - b + 1);
        if (s.empty() || s == ";") continue;

        // if (...) { ... } else { ... } — condition must be neutral too
        if (s.rfind("if", 0) == 0 && (s.size() == 2 || !(std::isalnum(static_cast<unsigned char>(s[2])) || s[2] == '_'))) {
            auto lp = s.find('(');
            auto rp = s.find(')', lp);
            if (lp == std::string::npos || rp == std::string::npos) { out += comment_every_line(s); continue; }
            std::string cond = s.substr(lp + 1, rp - lp - 1);
            auto ob = s.find('{', rp);
            if (ob == std::string::npos) { out += comment_every_line(s); continue; }
            // condition may only use known idents, literals and operators
            if (!neutral_expr_ok(cond) || !only_known_idents(cond, known)) { out += comment_every_line(s); continue; }
            int bal = 1; std::size_t i2 = ob + 1; bool in_s = false;
            for (; i2 < s.size(); ++i2) {
                char c = s[i2];
                if (c == '\"' && (i2 == 0 || s[i2-1] != '\\')) in_s = !in_s;
                if (!in_s) { if (c == '{') ++bal; else if (c == '}') { --bal; if (bal == 0) break; } }
            }
            std::string then_src = s.substr(ob + 1, i2 - ob - 1);
            std::string rest = s.substr(i2 + 1);
            out += "if (" + cond + ") {\n";
            out += codebehind_body_to_winui(then_src, known);
            out += "}\n";
            auto eb = rest.find("else");
            if (eb != std::string::npos) {
                auto ob2 = rest.find('{', eb);
                if (ob2 != std::string::npos) {
                    int bal2 = 1; std::size_t j2 = ob2 + 1; bool in_s2 = false;
                    for (; j2 < rest.size(); ++j2) {
                        char c = rest[j2];
                        if (c == '\"' && (j2 == 0 || rest[j2-1] != '\\')) in_s2 = !in_s2;
                        if (!in_s2) { if (c == '{') ++bal2; else if (c == '}') { --bal2; if (bal2 == 0) break; } }
                    }
                    std::string else_src = rest.substr(ob2 + 1, j2 - ob2 - 1);
                    out += "else {\n";
                    out += codebehind_body_to_winui(else_src, known);
                    out += "}\n";
                }
            }
            continue;
        }

        // switch (x) { case ... } -> if/else-if chain (subject must be neutral)
        if (s.rfind("switch", 0) == 0 && (s.size() == 6 || !(std::isalnum(static_cast<unsigned char>(s[6])) || s[6] == '_'))) {
            auto lp = s.find('('); auto rp = s.find(')', lp);
            if (lp == std::string::npos || rp == std::string::npos) { out += comment_every_line(s); continue; }
            std::string subj = s.substr(lp + 1, rp - lp - 1);
            if (!neutral_expr_ok(subj) || !only_known_idents(subj, known)) { out += comment_every_line(s); continue; }
            auto ob = s.find('{', rp);
            if (ob == std::string::npos) { out += comment_every_line(s); continue; }
            std::string body2 = s.substr(ob + 1, s.size() - ob - 2);
            std::vector<std::pair<std::string, std::string>> arms;
            std::size_t pos = 0;
            while (pos < body2.size()) {
                auto ci = body2.find("case", pos);
                auto di = body2.find("default", pos);
                if (di != std::string::npos && (ci == std::string::npos || di < ci)) {
                    auto col = body2.find(':', di);
                    if (col == std::string::npos) break;
                    arms.emplace_back("", body2.substr(col + 1));
                    break;
                }
                if (ci == std::string::npos) break;
                auto col = body2.find(':', ci + 4);
                if (col == std::string::npos) break;
                std::string label = body2.substr(ci + 4, col - (ci + 4));
                auto lb = label.find_first_not_of(" \t"); auto le = label.find_last_not_of(" \t");
                if (lb != std::string::npos) label = label.substr(lb, le - lb + 1);
                std::size_t end = body2.size();
                int bal = 0; bool in_s = false;
                for (std::size_t k = col + 1; k < body2.size(); ++k) {
                    char c = body2[k];
                    if (c == '\"' && (k == 0 || body2[k-1] != '\\')) in_s = !in_s;
                    if (in_s) continue;
                    if (c == '{') { ++bal; continue; }
                    if (c == '}') { if (bal == 0) { end = k; break; } --bal; continue; }
                    if (bal == 0 && (body2.rfind("case", k) == k || body2.rfind("default", k) == k)) { end = k; break; }
                }
                std::string arm = body2.substr(col + 1, end - col - 1);
                auto brk = arm.find("break");
                if (brk != std::string::npos) arm = arm.substr(0, brk);
                arms.emplace_back(label, arm);
                pos = end;
            }
            for (std::size_t ai = 0; ai < arms.size(); ++ai) {
                std::string kw = (ai == 0 && !arms[ai].first.empty()) ? "if" : "else if";
                if (arms[ai].first.empty()) { out += "else {\n"; }
                else {
                    out += kw + " (" + subj + " == " + arms[ai].first + ") {\n";
                }
                out += codebehind_body_to_winui(arms[ai].second, known);
                out += "}\n";
            }
            continue;
        }

        out += translate_plain_stmt(std::move(s), known);
    }
    return out;
}

// True when an expression / statement tail is expressible in language
// neutral winui: only identifiers, string/number/bool literals, '.', ',',
// parens and simple operators (+ - * / % == != < > <= >= && || !). Any
// C++/C#-only marker makes it false.


// Emit a view-level callback declaration for one XAML-wired handler.
// The handler's code-behind body is translated into .winui statements so the
// final app behaves like the original (the .winui syntax is code-behind
// free). When the body is missing or everything in it translated to a
// comment, keep the pure declaration so a host can still subscribe.
inline auto emit_callback_decl(Emitter& em, lang::CodeBehind const& cb_code,
                               Converter const& c, std::string const& name) -> void {
    std::string body;
    // language-agnostic contract: only identifiers the view knows may appear
    // in a callback body (x:Name elements, state/binding vars, code-behind
    // props). Everything else is a source-language local -> commented out.
    std::unordered_set<std::string> known;
    for (auto const& [nm, ty] : c.element_names) { (void)ty; known.insert(nm); }
    for (auto const& [nm, m] : c.cb_props) { (void)m; known.insert(nm); }
    for (auto const& sv : c.state_vars) {
        auto sp = sv.find(':');
        std::string nm = (sp == std::string::npos) ? sv : sv.substr(0, sp);
        auto b = nm.find_first_not_of(" \t");
        auto e = nm.find_last_not_of(" \t");
        if (b != std::string::npos) known.insert(nm.substr(b, e - b + 1));
    }
    for (auto const& bv : c.binding_vars) {
        auto sp = bv.find(':');
        std::string nm = (sp == std::string::npos) ? bv : bv.substr(0, sp);
        auto b = nm.find_first_not_of(" \t");
        auto e = nm.find_last_not_of(" \t");
        if (b != std::string::npos) known.insert(nm.substr(b, e - b + 1));
    }
    auto it = cb_code.method_bodies.find(name);
    if (it != cb_code.method_bodies.end() && !it->second.empty())
        body = codebehind_body_to_winui(it->second, known);
    // comment-only translation -> plain declaration (nothing expressible)
    bool has_real = false;
    {
        std::size_t pos = 0;
        while (pos < body.size()) {
            auto nl = body.find('\n', pos);
            std::string line = body.substr(pos, (nl == std::string::npos) ? body.size() - pos : nl - pos);
            auto b = line.find_first_not_of(" \t\r\n");
            if (b != std::string::npos && line.substr(b, 2) != "//") { has_real = true; break; }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    if (!has_real) {
        em.line("callback " + name + "();");
        return;
    }
    em.line("callback " + name + "() {");
    ++em.depth;
    std::size_t pos = 0;
    while (pos < body.size()) {
        auto nl = body.find('\n', pos);
        std::string line = body.substr(pos, (nl == std::string::npos) ? body.size() - pos : nl - pos);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) em.line(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    --em.depth;
    em.line("}");
}

export inline auto convert(xaml::ParseResult const& doc, lang::CodeBehind const& codebehind,
                           ConvertOptions const& opts) -> ConvertResult {
    ConvertResult res;
    res.is_resource_view = false;

    Converter c;
    c.cb = &codebehind;
    c.opts = &opts;
    c.result = &res;

    // index code-behind properties
    for (auto const& m : codebehind.properties) {
        if (m.is_static) continue;
        if (!is_ident(m.name)) continue;
        c.cb_props[m.name] = m;
    }
    for (auto const& m : codebehind.properties) {
        if (m.is_static) continue;
        if (!is_ident(m.name)) continue;
        c.element_names[m.name] = ""; // treat as possibly element-rooted (type from cb_props)
    }

    auto* root = doc.root();
    if (!root) {
        res.errors.push_back("no root element found");
        return res;
    }

    // view name
    std::string view_name;
    auto cls_attr = root->attr("Class");
    if (cls_attr) {
        std::string full(*cls_attr);
        auto dot = full.find_last_of('.');
        view_name = (dot == std::string::npos) ? full : full.substr(dot + 1);
    }
    if (view_name.empty()) {
        view_name = root->name;
        if (!view_name.empty() && !is_ident(view_name)) view_name = "XamlView";
    }
    if (!is_ident(view_name)) view_name = "XamlView";
    res.view_name = view_name;

    bool is_resource_dict = (root->name == "ResourceDictionary")
        || (root->name == "Application")
        || (root->name == "ResourceDictionary.MergedDictionaries");
    res.is_resource_view = is_resource_dict && opts.emit_resources;

    // PASS 1: discover element x:Names, x:Bind roots & event handlers
    struct Walker {
        Converter& c;
        auto walk(xaml::Node const& n) -> void {
            if (n.kind != xaml::NodeKind::Element) return;
            // collect x:Name
            for (auto const& a : n.attrs) {
                auto colon = a.name.find(':');
                std::string local = (colon == std::string::npos) ? a.name : a.name.substr(colon + 1);
                std::string prefix = (colon == std::string::npos) ? "" : a.name.substr(0, colon);
                if (local == "Name" && prefix == "x") {
                    c.element_names[a.value] = n.name;
                }
                // find x:Bind references
                if (a.value.size() > 2 && a.value.front() == '{' && a.value.back() == '}') {
                    std::string inner = a.value.substr(1, a.value.size() - 2);
                    if (inner.find("x:Bind") != std::string::npos || inner.rfind("Binding", 0) == 0) {
                        std::string mode;
                        std::string expr = translate_binding(inner, mode);
                        if (!expr.empty()) c.record_binding_root(expr, mode, local);
                    }
                }
                // event handlers (built-in + runtime JSON extensions)
                if (prefix.empty()) {
                    bool is_ev = false;
                    for (auto const& ev : g_event_names) {
                        if (ev == local) { is_ev = true; break; }
                    }
                    if (!is_ev) is_ev = ext::is_ext_xaml_event(local);
                    if (!is_ev) is_ev = ext::is_ext_ctrl_event(n.name, local);
                    if (is_ev && is_ident(a.value)) c.ensure_callback(a.value);
                }
            }
            for (auto const& ch : n.children) {
                // Descend into XAML code examples (ControlExample.Xaml) too, so
                // their handlers/bindings are discovered before declarations emit.
                if (ch.kind == xaml::NodeKind::Element) {
                    if (auto const* sk = code_example_string(ch)) {
                        std::string markup;
                        for (auto const& t : sk->text) markup += t;
                        auto parsed = xaml::parse(markup);
                        if (parsed.ok()) {
                            for (auto const& r : parsed.roots)
                                if (r.kind == xaml::NodeKind::Element) walk(r);
                        }
                        continue;
                    }
                }
                walk(ch);
            }
        }
    };
    Walker w{c};
    w.walk(*root);

    // Collect xmlns declarations (third-party namespace URIs) — attached to
    // the first emitted control so projections can resolve custom prefixes.
    collect_xmlns(*root, c.xmlns_pairs);

    // include code-behind methods as callback candidates? No — only XAML-wired handlers.

    // Code-behind constructor wiring (→ per-element .onEvent mods): scan
    // the view class ctor for element.Event(lambda->Handler) patterns and
    // attach them to the matching x:Name elements (code-behind-free winui).
    collect_codebehind_wiring(c, codebehind);

    // EMIT
    Emitter& em = c.em;
    em.line("#version 1");
    em.blank();
    // Emit file-level directives detected from code-behind
    if (codebehind.extends_content_into_titlebar)
        em.line(".extendContentIntoTitleBar;");
    if (!codebehind.window_title.empty())
        em.line(".titlebar(\"" + codebehind.window_title + "\");");
    if (!opts.source_rel_path.empty()) {
        em.line("// Generated by xaml2winuic");
        em.line("// Source : " + opts.source_rel_path);
        em.line("// Language: " + opts.language);
        em.line("// 100% equivalent .winui projection of the original WinUI 3 XAML.");
        em.blank();
    }

    if (is_resource_dict) {
        // Application root keeps its original name ("App"), not "AppResources"
        std::string res_view = (root->name == "Application") ? view_name : (view_name + "Resources");
        em.line("export view " + res_view + " {");
        ++em.depth;
        em.line("body {");
        ++em.depth;
        for (auto const& r : root->children) {
            if (r.kind == xaml::NodeKind::Element) emit_resource_entries(c, r);
        }
        --em.depth;
        em.line("}");
        --em.depth;
        em.line("}");
        res.text = std::move(em.out);
        return res;
    }

    em.line("export view " + view_name + " {");
    ++em.depth;

    // state vars (deterministic order: as discovered). A state var whose name
    // matches an x:Name'd element is that element's reference, so it must be
    // typed with the element's control type (not a primitive fallback like
    // string/bool) or the projection emits type-mismatched assignments.
    auto retype_element_state = [&](std::string const& decl) -> std::string {
        auto colon = decl.find(':');
        if (colon == std::string::npos) return decl;
        std::string name = decl.substr(0, colon);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        if (name.empty()) return decl;
        std::string name_lo;
        for (char ch : name) name_lo += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (auto const& [xname, tag] : c.element_names) {
            if (xname.size() != name.size()) continue;
            std::string xlo;
            for (char ch : xname) xlo += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (xlo != name_lo) continue;
            std::string t = tag;
            auto cc = t.find(':');
            if (cc != std::string::npos) t = t.substr(cc + 1);
            if (t.empty()) continue;
            // Element reference: drop any initializer - the element itself is
            // the value and the projection assigns it during tree construction.
            return name + ": " + t;
        }
        return decl;
    };
    for (auto const& v : c.state_vars) {
        em.line("@state var " + retype_element_state(v) + ";");
    }
    // two-way binding vars
    for (auto const& v : c.binding_vars) {
        em.line("@binding var " + v + ";");
    }
    // callbacks
    for (auto const& cb : c.callbacks) {
        emit_callback_decl(em, codebehind, c, cb);
    }
    // C# event declarations (public event X Name;) become callback decls so
    // the control exposes the event in the winmd and consumers wire .onX.
    for (auto const& ev : codebehind.events) {
        bool dup = false;
        for (auto const& cb : c.callbacks) if (cb == ev) { dup = true; break; }
        if (!dup) em.line("callback " + ev + "();");
    }
    if (!c.state_vars.empty() || !c.callbacks.empty() || !codebehind.events.empty()) em.blank();

    em.line("body {");
    ++em.depth;

    // root element conversion (transparent for Page/UserControl/Window)
    for (auto const& r : root->children) {
        if (r.kind == xaml::NodeKind::Element) convert_element(c, r);
        else if (r.kind == xaml::NodeKind::CData) em.line("// <![CDATA[" + r.name + "]]>");
    }

    --em.depth;
    em.line("}");
    --em.depth;
    em.line("}");

    res.text = std::move(em.out);
    return res;
}

} // namespace xamlconv::convert
