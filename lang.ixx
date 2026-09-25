// lang.ixx — Code-behind extractor for C#, C++/WinRT and MIDL (.idl)
// Extracts the subset of a code-behind file that matters for a faithful
// XAML → .winui conversion:

// C#  (.xaml.cs):      class name, base type, public properties/fields
// (x:Bind targets) and method names (event handlers)
// C++ (.xaml.h/.cpp):  struct name, method declarations incl. property
// accessor pairs
// IDL (.idl):          runtimeclass members — exact property types

// The parser is intentionally pragmatic and fault-tolerant: it never fails
// the whole conversion because of syntax it does not recognize — it simply
// collects what it can.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <map>
#include <cctype>
#include <cstdlib>

export module lang;

export namespace xamlconv::lang {

// §1  Shared model

enum class CsTypeKind : unsigned char {
    String,
    Int,
    Bool,
    Double,
    Float,
    Other,
};

struct Member {
    std::string name;
    std::string type;        // raw type text (e.g. "ObservableCollection<string>")
    CsTypeKind  kind = CsTypeKind::Other;   // mapped primitive kind
    bool        writeable = false;          // has setter
    bool        is_field  = false;
    bool        is_static = false;
};

struct CodeBehind {
    std::string    class_name;         // last identifier segment of x:Class
    std::string    full_class_name;    // e.g. WinUIGallery.ControlPages.ButtonPage
    std::string    base_class;         // Page / UserControl / Window ...
    std::vector<Member> properties;    // typed members (x:Bind targets)
    std::vector<std::string> events;   // C# event declarations (public event X Name;)
    std::vector<std::string> methods;  // method names (event handler candidates)
    // code-behind logic: method name -> raw body text (statements inside { })
    std::map<std::string, std::string> method_bodies;
    bool           found = false;
    bool           extends_content_into_titlebar = false;
    std::string    window_title;      // Title("...") / .titlebar directive payload
};

// Detect whether the code-behind extends ITS OWN window into the title bar.
// Accepted forms (assignment OR call - both are the real-world idioms):
//   C#:  this.ExtendsContentIntoTitleBar = true;   ExtendsContentIntoTitleBar = true;
//   C++: this->ExtendsContentIntoTitleBar(true);   ExtendsContentIntoTitleBar(true);
// Bare statement-level forms inside the view's own class (Window subclasses
// in the ctor) qualify. Mentions that configure a DIFFERENT window -
// `newWindow.Extends...`, `currentWindow.Extends...`,
// `extendWindow.AppWindow.TitleBar.Extends...`, or object-initializer
// entries (`new Window() { ExtendsContentIntoTitleBar = true, ... }`) -
// must NOT flag the view, or every sample page that merely demos title-bar
// windows would carry the app-level marker.
inline auto detect_titlebar_extend(std::string const& src) -> bool
{
    std::string const needle = "ExtendsContentIntoTitleBar";
    // Comments and string/char-literal contents are replaced with spaces
    // (offset-preserving) before scanning: a call/assignment preceded by a
    // comment line must still qualify (its previous real token is a
    // statement boundary), and mentions inside strings must not count.
    std::string clean = src;
    {
        enum class St { Code, Line, Block, Str, Chr } st = St::Code;
        for (std::size_t i = 0; i < clean.size(); ++i)
        {
            char c = clean[i];
            if (st == St::Code)
            {
                if (c == '/' && i + 1 < clean.size() && clean[i + 1] == '/') { clean[i] = ' '; clean[i + 1] = ' '; st = St::Line; ++i; continue; }
                if (c == '/' && i + 1 < clean.size() && clean[i + 1] == '*') { clean[i] = ' '; clean[i + 1] = ' '; st = St::Block; ++i; continue; }
                if (c == '"') { st = St::Str; continue; }
                if (c == '\'') { st = St::Chr; continue; }
                continue;
            }
            if (st == St::Line) { if (c == '\n') st = St::Code; else clean[i] = ' '; continue; }
            if (st == St::Block)
            {
                if (c == '*' && i + 1 < clean.size() && clean[i + 1] == '/') { clean[i] = ' '; clean[i + 1] = ' '; st = St::Code; ++i; continue; }
                if (c != '\n') clean[i] = ' ';
                continue;
            }
            if (st == St::Str)
            {
                if (c == '\\' && i + 1 < clean.size()) { clean[i] = ' '; clean[i + 1] = ' '; ++i; continue; }
                if (c == '"') { st = St::Code; continue; }
                if (c != '\n') clean[i] = ' ';
                continue;
            }
            if (st == St::Chr)
            {
                if (c == '\\' && i + 1 < clean.size()) { clean[i] = ' '; clean[i + 1] = ' '; ++i; continue; }
                if (c == '\'') { st = St::Code; continue; }
                if (c != '\n') clean[i] = ' ';
                continue;
            }
        }
    }
    std::string const& src2 = clean;
    // pass 1: call form - ExtendsContentIntoTitleBar(true) on this/bare.
    // A '(' immediately after the name (plus whitespace) means the call
    // idiom; the argument must be a bool literal.
    {
        std::size_t pos = 0;
        while ((pos = src2.find(needle, pos)) != std::string::npos)
        {
            std::size_t lp = pos + needle.size();
            while (lp < src2.size() && (src2[lp] == ' ' || src2[lp] == '\t' || src2[lp] == '\r' || src2[lp] == '\n')) ++lp;
            if (lp < src2.size() && src2[lp] == '(')
            {
                std::size_t a = lp + 1;
                while (a < src2.size() && (src2[a] == ' ' || src2[a] == '\t')) ++a;
                std::string arg(src2, a, 5);
                for (auto& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (arg.rfind("true", 0) == 0)
                {
                    // Receiver analysis: walk back over whitespace to the name start.
                    std::size_t b = pos;
                    while (b > 0 && (src2[b-1] == ' ' || src2[b-1] == '\t' || src2[b-1] == '\r' || src2[b-1] == '\n')) --b;
                    bool member_of = (b > 0 && (src2[b-1] == '.'));
                    bool member_of_arrow = (b > 1 && src2[b-1] == '>' && src2[b-2] == '-');
                    if (member_of || member_of_arrow)
                    {
                        std::size_t id_end = b - (member_of ? 1 : 2);
                        std::size_t id_begin = id_end;
                        while (id_begin > 0 && (std::isalnum(static_cast<unsigned char>(src2[id_begin-1])) || src2[id_begin-1] == '_')) --id_begin;
                        std::string recv = src.substr(id_begin, id_end - id_begin);
                        if (recv == "this") return true;   // this->ExtendsContentIntoTitleBar(true)
                        pos += needle.size();              // otherWindow.Extends...(true) -> not this view
                        continue;
                    }
                    // Bare call: accept only as a real statement (prev token is a
                    // statement boundary) and never inside an object initializer.
                    std::size_t s = b;
                    while (s > 0 && (src2[s-1] == ' ' || src2[s-1] == '\t' || src2[s-1] == '\r' || src2[s-1] == '\n')) --s;
                    char prev = (s > 0) ? src2[s-1] : '\0';
                    if (prev != ';' && prev != '{' && prev != '}' && prev != ':' && prev != '\0')
                    {
                        pos += needle.size();
                        continue;
                    }
                    return true;
                }
            }
            pos += needle.size();
        }
    }
    // pass 2: assignment form (original logic below).
    std::string const needle2 = "ExtendsContentIntoTitleBar";
    std::size_t pos = 0;
    while ((pos = src2.find(needle2, pos)) != std::string::npos)
    {
        // Must be an assignment `= true` within a short window after the name.
        std::size_t eq = src2.find('=', pos + needle2.size());
        if (eq == std::string::npos || eq > pos + needle2.size() + 40)
        {
            pos += needle2.size();
            continue;
        }
        std::size_t tv = eq + 1;
        while (tv < src2.size() && (src2[tv] == ' ' || src2[tv] == '\t' || src2[tv] == '\r' || src2[tv] == '\n')) ++tv;
        std::string after(src2, tv, 16);
        for (auto& c : after) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool is_true = (after.find("true") == 0);
        if (!is_true)
        {
            pos += needle.size();
            continue;
        }

        // Receiver analysis: walk back over whitespace to the name start.
        std::size_t b = pos;
        while (b > 0 && (src2[b-1] == ' ' || src2[b-1] == '\t' || src2[b-1] == '\r' || src2[b-1] == '\n')) --b;
        bool member_of = (b > 0 && (src2[b-1] == '.' ));
        bool member_of_arrow = (b > 1 && src2[b-1] == '>' && src2[b-2] == '-');
        if (member_of || member_of_arrow)
        {
            std::size_t id_end = b - (member_of ? 1 : 2);
            std::size_t id_begin = id_end;
            while (id_begin > 0 && (std::isalnum(static_cast<unsigned char>(src2[id_begin-1])) || src2[id_begin-1] == '_')) --id_begin;
            std::string recv = src2.substr(id_begin, id_end - id_begin);
            if (recv == "this") return true;   // this.ExtendsContentIntoTitleBar = true
            pos += needle.size();               // otherWindow.Extends... -> not this view
            continue;
        }

        // Bare identifier: accept only as a real statement (prev token is a
        // statement boundary), and never inside an object initializer such as
        // `new Window() { ExtendsContentIntoTitleBar = true, ... }`.
        std::size_t s = b;
        while (s > 0 && (src2[s-1] == ' ' || src2[s-1] == '\t' || src2[s-1] == '\r' || src2[s-1] == '\n')) --s;
        char prev = (s > 0) ? src2[s-1] : '\0';
        if (prev != ';' && prev != '{' && prev != '}' && prev != ':' && prev != '\0')
        {
            pos += needle.size();
            continue;
        }
        // Object-initializer guard: find the nearest enclosing '{' and check
        // whether the statement range before it contains a `new` expression.
        bool inside_obj_init = false;
        int depth = 0;
        std::size_t scan = s;
        while (scan > 0)
        {
            char c = src[scan - 1];
            if (c == '}') ++depth;
            else if (c == '{')
            {
                if (depth == 0)
                {
                    std::size_t m = scan - 1;
                    while (m > 0 && (src[m-1] == ' ' || src[m-1] == '\t' || src[m-1] == '\r' || src[m-1] == '\n')) --m;
                    std::size_t k = m;
                    while (k > 0 && src[k-1] != ';' && src[k-1] != '}' && src[k-1] != '{') --k;
                    std::string before(src, k, m - k);
                    if (before.find("new") != std::string::npos) inside_obj_init = true;
                    break;
                }
                --depth;
            }
            --scan;
        }
        if (inside_obj_init)
        {
            pos += needle.size();
            continue;
        }
        return true;
    }
    return false;
}

// Detect the window title set by the view's own code-behind. Same contract
// as detect_titlebar_extend: only the view's own window qualifies.
//   C#:  this.Title = "MetaScrub";   Title = "MetaScrub";
//   C++: Title(L"MetaScrub");        this->Title(L"MetaScrub");
// Mentions on OTHER receivers (page.Title(...), someOtherWindow.Title(...))
// and string literals mentioning Title must NOT flag.
inline auto detect_window_title(std::string const& src) -> std::string
{
    std::string const needle = "Title";
    std::string clean = src;
    {
        enum class St { Code, Line, Block, Str, Chr } st = St::Code;
        for (std::size_t i = 0; i < clean.size(); ++i)
        {
            char c = clean[i];
            if (st == St::Code)
            {
                if (c == '/' && i + 1 < clean.size() && clean[i + 1] == '/') { clean[i] = ' '; clean[i + 1] = ' '; st = St::Line; ++i; continue; }
                if (c == '/' && i + 1 < clean.size() && clean[i + 1] == '*') { clean[i] = ' '; clean[i + 1] = ' '; st = St::Block; ++i; continue; }
                if (c == '"') { st = St::Str; continue; }
                if (c == '\'') { st = St::Chr; continue; }
                continue;
            }
            if (st == St::Line) { if (c == '\n') st = St::Code; else clean[i] = ' '; continue; }
            if (st == St::Block)
            {
                if (c == '*' && i + 1 < clean.size() && clean[i + 1] == '/') { clean[i] = ' '; clean[i + 1] = ' '; st = St::Code; ++i; continue; }
                if (c != '\n') clean[i] = ' ';
                continue;
            }
            if (st == St::Str)
            {
                if (c == '\\' && i + 1 < clean.size()) { clean[i] = ' '; clean[i + 1] = ' '; ++i; continue; }
                if (c == '"') { st = St::Code; continue; }
                if (c != '\n') clean[i] = ' ';
                continue;
            }
            if (st == St::Chr)
            {
                if (c == '\\' && i + 1 < clean.size()) { clean[i] = ' '; clean[i + 1] = ' '; ++i; continue; }
                if (c == '\'') { st = St::Code; continue; }
                if (c != '\n') clean[i] = ' ';
                continue;
            }
        }
    }
    std::string const& s2 = clean;
    std::size_t pos = 0;
    while ((pos = s2.find(needle, pos)) != std::string::npos)
    {
        // Word-boundary: the char before must not be an identifier char.
        if (pos > 0 && (std::isalnum(static_cast<unsigned char>(s2[pos - 1])) || s2[pos - 1] == '_'))
        {
            pos += needle.size();
            continue;
        }
        std::size_t lp = pos + needle.size();
        while (lp < s2.size() && (s2[lp] == ' ' || s2[lp] == '\t' || s2[lp] == '\r' || s2[lp] == '\n')) ++lp;
        if (lp >= s2.size()) { pos += needle.size(); continue; }

        // Receiver analysis: walk back over whitespace to the name start.
        std::size_t b = pos;
        while (b > 0 && (s2[b - 1] == ' ' || s2[b - 1] == '\t' || s2[b - 1] == '\r' || s2[b - 1] == '\n')) --b;
        bool member_of = (b > 0 && s2[b - 1] == '.');
        bool member_of_arrow = (b > 1 && s2[b - 1] == '>' && s2[b - 2] == '-');
        if (member_of || member_of_arrow)
        {
            std::size_t id_end = b - (member_of ? 1 : 2);
            std::size_t id_begin = id_end;
            while (id_begin > 0 && (std::isalnum(static_cast<unsigned char>(s2[id_begin - 1])) || s2[id_begin - 1] == '_')) --id_begin;
            std::string recv = s2.substr(id_begin, id_end - id_begin);
            if (recv != "this") { pos += needle.size(); continue; }   // other.Title(...) -> not this view
        }
        else
        {
            // Bare call: accept only as a real statement (prev token is a
            // statement boundary) and never inside an object initializer.
            std::size_t s = b;
            while (s > 0 && (s2[s - 1] == ' ' || s2[s - 1] == '\t' || s2[s - 1] == '\r' || s2[s - 1] == '\n')) --s;
            char prev = (s > 0) ? s2[s - 1] : '\0';
            if (prev != ';' && prev != '{' && prev != '}' && prev != ':' && prev != '\0')
            {
                pos += needle.size();
                continue;
            }
        }

        // String argument: "..." or L"..." on the call form, or the RHS of an
        // assignment form `Title = "..."`.
        std::size_t q = lp;
        std::size_t eq = lp;
        if (s2[lp] == '=')
        {
            // assignment form: Title = "..."
            std::size_t v = lp + 1;
            while (v < s2.size() && (s2[v] == ' ' || s2[v] == '\t' || s2[v] == '\r' || s2[v] == '\n')) ++v;
            eq = v;
            q = v;
        }
        else
        {
            // call form: Title("...") - skip past '(' and whitespace to the argument
            std::size_t v = lp;
            while (v < s2.size() && (s2[v] == '(' || s2[v] == ' ' || s2[v] == '\t' || s2[v] == '\r' || s2[v] == '\n')) ++v;
            q = v;
        }
        if (q < s2.size() && (s2[q] == 'L' || s2[q] == 'u' || s2[q] == 'U') && q + 1 < s2.size() && s2[q + 1] == '"')
            ++q; // wide/unicode string literal prefix (C++ form)
        if (q < s2.size() && s2[q] == '"')
        {
            std::size_t e = q + 1;
            while (e < s2.size() && s2[e] != '"')
            {
                if (s2[e] == '\\' && e + 1 < s2.size()) ++e;
                ++e;
            }
            if (e < s2.size())
            {
                std::string title(src, q + 1, e - q - 1);
                // Unescape the common \" and \\ pairs.
                std::string out;
                for (std::size_t i = 0; i < title.size(); ++i)
                {
                    if (title[i] == '\\' && i + 1 < title.size() && (title[i + 1] == '"' || title[i + 1] == '\\')) { out += title[i + 1]; ++i; }
                    else out += title[i];
                }
                return out;
            }
        }
        pos += needle.size();
    }
    return {};
}

// §2  Type mapping

inline auto map_type(std::string_view type) -> CsTypeKind {
    std::string t(type);
    // C++/CX normalization: strip "Platform::" prefix and trailing "^" handle
    if (t.starts_with("Platform::")) t = t.substr(10);
    if (!t.empty() && t.back() == '^') t.pop_back();
    if (t == "string" || t == "String" || t == "System.String"
        || t == "winrt::hstring" || t == "HSTRING" || t == "hstring") return CsTypeKind::String;
    if (t == "int" || t == "Int32" || t == "System.Int32"
        || t == "int32_t" || t == "int64_t" || t == "Int64"
        || t == "uint32_t" || t == "uint64_t" || t == "UInt32"
        || t == "long" || t == "short" || t == "byte"
        || t == "int16_t" || t == "uint16_t" || t == "uint8_t"
        || t == "Integer" || t == "INT32" || t == "INT64") return CsTypeKind::Int;
    if (t == "bool" || t == "Boolean" || t == "System.Boolean"
        || t == "BOOL") return CsTypeKind::Bool;
    if (type == "double" || type == "Double" || type == "System.Double"
        || type == "float64_t") return CsTypeKind::Double;
    if (type == "float" || type == "Single" || type == "System.Single"
        || type == "float32_t") return CsTypeKind::Float;
    return CsTypeKind::Other;
}

inline auto map_idl_type(std::string_view type) -> CsTypeKind {
    // MIDL 3.0 type names
    if (type == "String") return CsTypeKind::String;
    if (type == "Int32" || type == "UInt32" || type == "Int64"
        || type == "UInt64" || type == "Int16" || type == "UInt16"
        || type == "Byte" || type == "SByte" || type == "IntPtr") return CsTypeKind::Int;
    if (type == "Boolean") return CsTypeKind::Bool;
    if (type == "Double") return CsTypeKind::Double;
    if (type == "Single") return CsTypeKind::Float;
    return CsTypeKind::Other;
}

// §3  Common tokenizer helpers

enum class TokKind : unsigned char { Ident, Sym, Str, Num, End };

struct Token {
    TokKind kind;
    std::string text;
};

// Tokenize C#-style source, stripping comments and strings (kept as single tokens).
inline auto tokenize_cs(std::string_view src, std::vector<std::string>& strings) -> std::vector<Token> {
    std::vector<Token> out;
    std::size_t i = 0;
    int block_depth = 0;
    while (i < src.size()) {
        char c = src[i];
        // skip whitespace (never emit tokens for it)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        // preprocessor lines (#if / #define...) → skip to end of line
        if (c == '#' && (i == 0 || src[i-1] == '\n')) {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i+1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i+1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i+1] == '/')) ++i;
            i += (i + 1 < src.size()) ? 2 : 0;
            continue;
        }
        if (c == '"') {
            std::string s;
            ++i;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < src.size()) { s += src[i]; s += src[i+1]; i += 2; }
                else { s += src[i]; ++i; }
            }
            if (i < src.size()) ++i;
            strings.push_back(s);
            out.push_back(Token{TokKind::Str, s});
            continue;
        }
        if (c == '\'') {
            ++i;
            while (i < src.size() && src[i] != '\'') {
                if (src[i] == '\\' && i + 1 < src.size()) i += 2;
                else ++i;
            }
            if (i < src.size()) ++i;
            out.push_back(Token{TokKind::Str, "char"});
            continue;
        }
        if (c == '@' && i + 1 < src.size() && src[i+1] == '"') {
            // verbatim string
            i += 2;
            std::string s;
            while (i < src.size()) {
                if (src[i] == '"' && i + 1 < src.size() && src[i+1] == '"') { s += '"'; i += 2; continue; }
                if (src[i] == '"') break;
                s += src[i];
                ++i;
            }
            if (i < src.size()) ++i;
            strings.push_back(s);
            out.push_back(Token{TokKind::Str, s});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string n;
            while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i]))
                   || src[i] == '.' || src[i] == '_' || src[i] == 'x' || src[i] == 'X'
                   || src[i] == 'u' || src[i] == 'L' || src[i] == 'f' || src[i] == 'F'
                   || src[i] == 'd' || src[i] == 'D' || src[i] == 'm' || src[i] == 'M')) {
                n += src[i];
                ++i;
            }
            out.push_back(Token{TokKind::Num, std::move(n)});
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::string id;
            while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) {
                id += src[i];
                ++i;
            }
            out.push_back(Token{TokKind::Ident, std::move(id)});
            continue;
        }
        if (c == ':' && i + 1 < src.size() && src[i + 1] == ':') {
            out.push_back(Token{TokKind::Sym, "::"});
            i += 2;
            continue;
        }
        if (c == '{') ++block_depth;
        else if (c == '}') --block_depth;
        out.push_back(Token{TokKind::Sym, std::string(1, c)});
        ++i;
    }
    (void)block_depth;
    return out;
}

// Raw text capture of method bodies for XAML-wired handlers. The token walk
// above records names only; this pass re-scans the source text so the code-
// behind logic ("{ ... }") can be carried into .winui callback bodies.
// C#:    private void Button_Click(object sender, RoutedEventArgs e) { ... }
// C++:   void Button_Click(IInspectable const&, RoutedEventArgs const&) { ... }
// Skips constructors, properties and anything that is not name( ... ) { }.
inline auto capture_codebehind_bodies(std::string_view src, CodeBehind& cb) -> void {
    bool dbg = std::getenv("X2W_DEBUG") != nullptr;
    auto dbg_log = [&](std::string const& m) { if (dbg) { std::fprintf(stderr, "[x2w] %s\n", m.c_str()); std::fflush(stderr); } };
    dbg_log("capture: methods=" + std::to_string(cb.methods.size()) + " class=" + cb.class_name);
    if (cb.methods.empty()) return;
    std::size_t i = 0;
    while (i < src.size()) {
        // find identifier start
        if (!(std::isalpha(static_cast<unsigned char>(src[i])) || src[i] == '_')) { ++i; continue; }
        std::size_t id_start = i;
        while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) ++i;
        std::string name(src.substr(id_start, i - id_start));
        // candidate: name must be in the method list and followed by optional ws + '('.
        // The constructor (name == class name) is captured too: C++/WinRT
        // code-behind wires element events in the ctor body, and that wiring
        // must survive into .winui (collect_codebehind_wiring reads it under
        // the class-name key).
        bool wanted = false;
        for (auto const& m : cb.methods) if (m == name) { wanted = true; break; }
        if (!wanted && !cb.class_name.empty() && name == cb.class_name) wanted = true;
        if (!wanted) continue;
        std::size_t p = i;
        while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
        if (p >= src.size() || src[p] != '(') continue;
        // skip the parameter list
        int pd = 1;
        ++p;
        while (p < src.size() && pd > 0) {
            char c = src[p];
            if (c == '\"') { ++p; while (p < src.size() && !(src[p] == '\"' && (p == 0 || src[p-1] != '\\'))) ++p; ++p; continue; }
            if (c == '(') ++pd;
            if (c == ')') --pd;
            ++p;
        }
        while (p < src.size() && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) ++p;
        // C++ constructors carry a member-init list between ')' and '{':
        //   PickersPage::PickersPage() : m_a(x), m_b(y) { ... }
        // Skip it (tracking parens/brackets) until the body brace at paren depth 0.
        if (p < src.size() && src[p] == ':'
            && !(p + 1 < src.size() && src[p + 1] == ':')) {
            int pdepth = 0;
            while (p < src.size()) {
                char ch = src[p];
                if (ch == '\"') { ++p; while (p < src.size() && !(src[p] == '\"' && (p == 0 || src[p-1] != '\\'))) ++p; ++p; continue; }
                if (ch == '(' || ch == '[') ++pdepth;
                if (ch == ')' || ch == ']') { if (pdepth > 0) --pdepth; }
                if (ch == '{' && pdepth == 0) break;
                ++p;
            }
            while (p < src.size() && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) ++p;
        }
        if (p >= src.size() || src[p] != '{') continue;
        // balanced body capture (string/char aware)
        std::size_t b0 = p;
        int bd = 0;
        while (p < src.size()) {
            char c = src[p];
            if (c == '\"') { ++p; while (p < src.size() && !(src[p] == '\"' && (p == 0 || src[p-1] != '\\'))) ++p; ++p; continue; }
            if (c == '\'') { ++p; if (p < src.size()) ++p; continue; }
            if (c == '{') ++bd;
            if (c == '}') { --bd; if (bd == 0) { ++p; break; } }
            ++p;
        }
        std::string body(src.substr(b0 + 1, p - b0 - 2));
        // strip trailing/leading whitespace, keep interior lines
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ' || body.back() == '\t')) body.pop_back();
        while (!body.empty() && (body.front() == '\n' || body.front() == '\r' || body.front() == ' ' || body.front() == '\t')) body.erase(0, 1);
        if (!body.empty() && cb.method_bodies.find(name) == cb.method_bodies.end())
        {
            cb.method_bodies[name] = std::move(body);
            dbg_log("body: " + name + " (" + std::to_string(body.size()) + " chars)");
        }
        i = p;
    }
}


// §4  C# class extraction

inline auto parse_csharp(std::string_view src) -> CodeBehind {
    CodeBehind cb;
    std::vector<std::string> strings;
    auto toks = tokenize_cs(src, strings);

    // Detect ExtendsContentIntoTitleBar = true in source text (before any early return)
    cb.extends_content_into_titlebar = detect_titlebar_extend(std::string(src));
    cb.window_title = detect_window_title(std::string(src));

    // find "class <Name>" (optionally "partial class") and its base list
    std::size_t cls = std::string_view::npos;
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].kind == TokKind::Ident && toks[i].text == "class"
            && i + 1 < toks.size() && toks[i+1].kind == TokKind::Ident) {
            cls = i;
            break;
        }
    }
    if (cls == std::string_view::npos) return cb;

    std::size_t name_idx = cls + 1;
    cb.class_name = toks[name_idx].text;

    // namespace from file header ("namespace X.Y;") for full class name
    std::string ns;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (toks[i].kind == TokKind::Ident && toks[i].text == "namespace") {
            ns.clear();
            std::size_t j = i + 1;
            while (j < toks.size() && toks[j].kind == TokKind::Ident) {
                if (!ns.empty()) ns += ".";
                ns += toks[j].text;
                ++j;
            }
            break;
        }
    }
    cb.full_class_name = ns.empty() ? cb.class_name : ns + "." + cb.class_name;
    cb.found = true;

    // base class: after "class Name" comes ':' or '{'
    std::size_t base_end = name_idx + 1;
    if (base_end < toks.size() && toks[base_end].text == ":") {
        std::size_t j = base_end + 1;
        while (j < toks.size() && toks[j].kind == TokKind::Ident) {
            cb.base_class = toks[j].text;
            ++j;
            if (j < toks.size() && toks[j].text == ",") { ++j; continue; }
            break;
        }
    }

    // Walk the class body: find '{' then scan members until matching '}'.
    std::size_t open = std::string_view::npos;
    for (std::size_t i = name_idx + 1; i < toks.size(); ++i) {
        if (toks[i].text == "{") { open = i; break; }
    }
    if (open == std::string_view::npos) return cb;

    int depth = 0;
    for (std::size_t i = open; i < toks.size(); ++i) {
        if (toks[i].text == "{") ++depth;
        if (toks[i].text == "}") {
            --depth;
            if (depth == 0) break; // end of class
            continue;
        }
        if (depth > 1) continue; // inside method/ctor body

        // member declaration start: skip attributes
        while (i < toks.size() && toks[i].text == "[") {
            int adepth = 1;
            ++i;
            while (i < toks.size() && adepth > 0) {
                if (toks[i].text == "[") ++adepth;
                if (toks[i].text == "]") --adepth;
                ++i;
            }
        }
        if (i >= toks.size()) break;

        // collect modifiers
        bool is_static = false;
        bool is_const  = false;
        std::size_t j = i;
        while (j < toks.size() && toks[j].kind == TokKind::Ident
               && (toks[j].text == "public" || toks[j].text == "private"
                   || toks[j].text == "internal" || toks[j].text == "protected"
                   || toks[j].text == "static" || toks[j].text == "sealed"
                   || toks[j].text == "partial" || toks[j].text == "abstract"
                   || toks[j].text == "virtual" || toks[j].text == "override"
                   || toks[j].text == "async" || toks[j].text == "readonly"
                   || toks[j].text == "ref" || toks[j].text == "new"
                   || toks[j].text == "required" || toks[j].text == "unsafe"
                   || toks[j].text == "extern" || toks[j].text == "const"
                   || toks[j].text == "volatile" || toks[j].text == "in"
                   || toks[j].text == "out")) {
            if (toks[j].text == "static") is_static = true;
            if (toks[j].text == "const")  is_const = true;
            ++j;
        }
        // skip "partial" appearing between "public" and "class" — handled above

        // type tokens until name: could include generics < > and namespaces.
        // An identifier followed by ( { ; = , is the MEMBER NAME; everything
        // before it is the type (may be multi-word: "unsigned long long", ...).
        std::string type;
        std::size_t k = j;
        while (k < toks.size()) {
            auto const& t = toks[k];
            if (t.kind == TokKind::Ident && k + 1 < toks.size()) {
                std::string nxt = toks[k + 1].text;
                if (nxt == "(" || nxt == "{" || nxt == ";" || nxt == "=" || nxt == ",") {
                    break; // this identifier is the member name
                }
            }
            if (t.kind == TokKind::Sym && (t.text == ";" || t.text == "=" || t.text == "(")) break;
            if (t.kind == TokKind::Sym && t.text == ",") break;
            if (t.kind == TokKind::Sym && t.text == "{") break;
            type += t.text;
            ++k;
        }
        if (type.empty() || k >= toks.size()) { ++i; continue; }
        std::string tname = toks[k].text;
        // member name token:
        //  - followed by '(' → method
        //  - followed by '{' → property
        //  - followed by ';' or '=' → field
        std::size_t after = k + 1;
        if (after >= toks.size()) { ++i; continue; }
        std::string next = toks[after].text;

        if (next == "(") {
            // method — record name
            bool dup = false;
            for (auto const& m : cb.methods) if (m == tname) dup = true;
            if (!dup && !is_static) cb.methods.push_back(tname);
            // skip to matching ')'
            int pdepth = 1;
            ++after;
            while (after < toks.size() && pdepth > 0) {
                if (toks[after].text == "(") ++pdepth;
                if (toks[after].text == ")") --pdepth;
                ++after;
            }
            // skip to end of statement / body
            if (after < toks.size() && toks[after].text == "{") {
                // method body — skip until matching '}'
                int bdepth = 1;
                ++after;
                while (after < toks.size() && bdepth > 0) {
                    if (toks[after].text == "{") ++bdepth;
                    if (toks[after].text == "}") --bdepth;
                    ++after;
                }
            } else {
                while (after < toks.size() && toks[after].text != ";" && toks[after].text != "{") ++after;
                if (after < toks.size() && toks[after].text == "{") {
                    int bdepth = 1;
                    ++after;
                    while (after < toks.size() && bdepth > 0) {
                        if (toks[after].text == "{") ++bdepth;
                        if (toks[after].text == "}") --bdepth;
                        ++after;
                    }
                }
            }
            i = after;
            continue;
        }

        if (next == "{") {
            // property — check for setter
            bool has_setter = false;
            int bdepth = 1;
            std::size_t p = after + 1;
            while (p < toks.size() && bdepth > 0) {
                if (toks[p].text == "{") ++bdepth;
                if (toks[p].text == "}") --bdepth;
                if (bdepth == 1 && toks[p].kind == TokKind::Ident && toks[p].text == "set") has_setter = true;
                ++p;
            }
            // skip '= ...;' initializer and expression bodies
            std::size_t q = p;
            if (q < toks.size() && toks[q].text == "=") {
                while (q < toks.size() && toks[q].text != ";" && toks[q].text != "{") ++q;
            }
            if (q < toks.size() && toks[q].text == "{") {
                int bdepth2 = 1;
                ++q;
                while (q < toks.size() && bdepth2 > 0) {
                    if (toks[q].text == "{") ++bdepth2;
                    if (toks[q].text == "}") --bdepth2;
                    ++q;
                }
            }
            Member m;
            m.name = tname;
            m.type = type;
            m.kind = map_type(type);
            m.writeable = has_setter;
            m.is_field = false;
            m.is_static = is_static;
            cb.properties.push_back(std::move(m));
            i = q;
            continue;
        }

        if (next == ";" || next == "=") {
            // field
            Member m;
            m.name = tname;
            m.type = type;
            m.kind = map_type(type);
            m.writeable = true;
            m.is_field = true;
            m.is_static = is_static;
            cb.properties.push_back(std::move(m));
            // skip initializer
            std::size_t q = after;
            while (q < toks.size() && toks[q].text != ";") ++q;
            i = q;
            continue;
        }

        i = j; // unrecognized — advance past modifiers and try again
    }

    capture_codebehind_bodies(src, cb);
    return cb;
}

// §5  C++ header extraction (.xaml.h)

inline auto parse_cpp_cx(std::string_view src) -> CodeBehind;

inline auto parse_cpp_header(std::string_view src) -> CodeBehind {
    CodeBehind cb;
    std::vector<std::string> no_strings;
    auto toks = tokenize_cs(src, no_strings);

    // Detect ExtendsContentIntoTitleBar in source text
    cb.extends_content_into_titlebar = detect_titlebar_extend(std::string(src));
    cb.window_title = detect_window_title(std::string(src));
    if (cb.extends_content_into_titlebar) cb.found = true;

    // C++/CX ("ref class" / "ref struct") → dedicated walker.
    // C++/CX is compiled by cl /ZW only; clang-cl cannot handle it, so the
    // built-in CX parser is the only C++/CX extraction path.
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].kind == TokKind::Ident && toks[i].text == "ref"
            && i + 1 < toks.size() && toks[i + 1].kind == TokKind::Ident
            && (toks[i + 1].text == "class" || toks[i + 1].text == "struct")) {
            return parse_cpp_cx(src);
        }
    }

    // C++/CX ("ref class" / "ref struct") → dedicated walker.

    // find "struct <Name> : ...T<Name>"
    std::size_t name_idx = std::string_view::npos;
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].kind == TokKind::Ident && toks[i].text == "struct"
            && i + 1 < toks.size() && toks[i+1].kind == TokKind::Ident) {
            std::size_t j = i + 2;
            // collect base list tokens
            std::string base_text;
            while (j < toks.size() && toks[j].text != "{") {
                base_text += toks[j].text;
                ++j;
            }
            if (base_text.find(toks[i+1].text + "T<") != std::string::npos
                || base_text.find("T<") != std::string::npos) {
                name_idx = i + 1;
                break;
            }
        }
    }
    if (name_idx == std::string_view::npos) {
        // fallback: any struct whose name matches the file base
        return cb;
    }
    cb.class_name = toks[name_idx].text;
    cb.found = true;
    cb.base_class = "Window";

    // members until first "}" at depth 1
    std::size_t open = std::string_view::npos;
    for (std::size_t i = name_idx + 1; i < toks.size(); ++i) {
        if (toks[i].text == "{") { open = i; break; }
    }
    if (open == std::string_view::npos) return cb;

    int depth = 0;
    for (std::size_t i = open; i < toks.size(); ++i) {
        if (toks[i].text == "{") ++depth;
        if (toks[i].text == "}") {
            --depth;
            if (depth == 0) break;
            continue;
        }
        if (depth > 1) continue;
        // member: [Type...] Name(  → method
        // Type must be followed by identifier (name) then '('
        std::size_t j = i;
        // skip access specifiers public:/private:
        if (toks[j].kind == TokKind::Ident && (toks[j].text == "public" || toks[j].text == "private"
            || toks[j].text == "protected") && j + 1 < toks.size() && toks[j+1].text == ":") {
            i = j + 1;
            continue;
        }
        // find "ident ident (" pattern
        std::size_t k = j;
        std::size_t k_limit = (std::min)(toks.size(), j + 50);
        while (k + 2 < k_limit) {
            if (toks[k].kind == TokKind::Ident && toks[k+1].kind == TokKind::Ident
                && toks[k+2].text == "(") break;
            ++k;
            if (k < toks.size() && (toks[k].text == ";" || toks[k].text == "}")) { k = std::string_view::npos; break; }
        }
        if (k == std::string_view::npos || k + 2 >= toks.size()) { ++i; continue; }
        std::string ret_type = toks[k].text;
        std::string mname = toks[k+1].text;
        // skip ctor (name == class name)
        // skip to matching ')' then ';' or '{'
        std::size_t p = k + 3;
        int pdepth = 1;
        while (p < toks.size() && pdepth > 0) {
            if (toks[p].text == "(") ++pdepth;
            if (toks[p].text == ")") --pdepth;
            ++p;
        }
        bool is_prop_getter = (ret_type != "void" && ret_type != "fire_and_forget");
        // property pair detection: getter "T Name()" + setter "void Name(T const&)"
        // we record getters as properties with inferred type; setters ignored.
        std::size_t q = p;
        while (q < toks.size() && toks[q].text != ";" && toks[q].text != "{") ++q;
        if (q < toks.size() && toks[q].text == "{") {
            int bdepth = 1;
            ++q;
            while (q < toks.size() && bdepth > 0) {
                if (toks[q].text == "{") ++bdepth;
                if (toks[q].text == "}") --bdepth;
                ++q;
            }
        }
        if (is_prop_getter) {
            Member m;
            m.name = mname;
            m.type = ret_type;
            m.kind = map_type(ret_type);
            m.writeable = true; // assume settable (typical C++/WinRT accessor pair)
            m.is_field = false;
            cb.properties.push_back(std::move(m));
        } else {
            bool dup = false;
            for (auto const& mm : cb.methods) if (mm == mname) dup = true;
            if (!dup) cb.methods.push_back(mname);
        }
        i = q;
    }
    return cb;
}

// §5b C++/CX header extraction
//   public ref class MainPage sealed : Windows::UI::Xaml::Controls::Page
//   property Platform::String^ Title { get; set; }   → typed property
//   event ...^ Name;  Platform::String^ field_;       → fields / events

inline auto parse_cpp_cx(std::string_view src) -> CodeBehind {
    CodeBehind cb;

    // Detect ExtendsContentIntoTitleBar in source text
    cb.extends_content_into_titlebar = detect_titlebar_extend(std::string(src));
    cb.window_title = detect_window_title(std::string(src));
    if (cb.extends_content_into_titlebar) cb.found = true;

    std::vector<std::string> no_strings;
    auto toks = tokenize_cs(src, no_strings);

    // find "ref class <Name>" optionally followed by sealed/abstract/final
    std::size_t name_idx = std::string_view::npos;
    for (std::size_t i = 0; i + 2 < toks.size(); ++i) {
        if (toks[i].kind != TokKind::Ident || toks[i].text != "ref") continue;
        if (toks[i + 1].kind != TokKind::Ident) continue;
        if (toks[i + 1].text != "class" && toks[i + 1].text != "struct") continue;
        std::size_t k = i + 2;
        if (k < toks.size() && toks[k].kind == TokKind::Ident
            && (toks[k].text == "sealed" || toks[k].text == "abstract" || toks[k].text == "final")) {
            ++k;
        }
        if (k >= toks.size() || toks[k].kind != TokKind::Ident) continue;
        name_idx = k;
        break;
    }
    if (name_idx == std::string_view::npos) return cb;

    cb.class_name = toks[name_idx].text;
    cb.found = true;

    // base class: last identifier between ':' and '{'
    std::size_t open = std::string_view::npos;
    std::string base;
    for (std::size_t i = name_idx + 1; i < toks.size(); ++i) {
        if (toks[i].text == "{") { open = i; break; }
        if (toks[i].text == ":") {
            base.clear();
            for (std::size_t j = i + 1; j < toks.size()
                 && toks[j].text != "{" && toks[j].text != ";"; ++j) {
                if (toks[j].kind == TokKind::Ident
                    && toks[j].text != "public" && toks[j].text != "private"
                    && toks[j].text != "protected" && toks[j].text != "virtual") {
                    base = toks[j].text;
                }
            }
        }
    }
    if (!base.empty()) cb.base_class = base;
    if (open == std::string_view::npos) return cb;

    int depth = 0;
    for (std::size_t i = open; i < toks.size(); ++i) {
        if (toks[i].text == "{") ++depth;
        if (toks[i].text == "}") {
            --depth;
            if (depth == 0) break;
            continue;
        }
        if (depth > 1) continue;

        // statement terminator — no-op
        if (toks[i].text == ";") continue;

        // skip access specifiers "public:" / "private:" / "protected:"
        if (toks[i].kind == TokKind::Ident
            && (toks[i].text == "public" || toks[i].text == "private" || toks[i].text == "protected")
            && i + 1 < toks.size() && toks[i + 1].text == ":") {
            ++i;
            continue;
        }

        std::size_t j = i;

        // skip leading modifiers (static property, sealed, etc.)
        while (j < toks.size() && toks[j].kind == TokKind::Ident
               && (toks[j].text == "static" || toks[j].text == "const"
                   || toks[j].text == "virtual" || toks[j].text == "override"
                   || toks[j].text == "inline" || toks[j].text == "sealed"
                   || toks[j].text == "abstract" || toks[j].text == "explicit")) {
            ++j;
        }

        // property: property <Type> <Name> { get() {...} set(...) {...} }
        if (j < toks.size() && toks[j].kind == TokKind::Ident && toks[j].text == "property") {
            ++j;
            std::string type;
            std::size_t nm = std::string_view::npos;
            while (j + 1 < toks.size()) {
                if (toks[j].kind == TokKind::Ident && toks[j + 1].text == "{") {
                    nm = j;
                    break;
                }
                if (toks[j].text == "{") break;
                type += toks[j].text;
                ++j;
            }
            if (nm != std::string_view::npos && toks[nm + 1].text == "{") {
                bool has_set = false;
                int bd = 1;
                std::size_t p = nm + 2;
                while (p < toks.size() && bd > 0) {
                    if (toks[p].text == "{") ++bd;
                    if (toks[p].text == "}") --bd;
                    if (bd == 1 && toks[p].kind == TokKind::Ident && toks[p].text == "set") has_set = true;
                    ++p;
                }
                Member m;
                m.name = toks[nm].text;
                m.type = type;
                m.kind = map_type(type);
                m.writeable = has_set;
                m.is_field = false;
                cb.properties.push_back(std::move(m));
                i = p - 1;
            } else {
                i = j;
            }
            continue;
        }

        // C++/WinRT event field: winrt::event<Delegate> Name; → callback decl
        if (j + 2 < toks.size() && toks[j].kind == TokKind::Ident && toks[j].text == "winrt"
            && toks[j + 1].text == "::" && toks[j + 2].kind == TokKind::Ident
            && toks[j + 2].text == "event") {
            std::size_t p = j + 3;
            int depth = 0;
            while (p < toks.size()) {
                std::string const& t = toks[p].text;
                if (t == "<") ++depth;
                else if (t == ">") { if (depth) --depth; }
                else if (depth == 0 && (t == ";" || t == "{")) break;
                ++p;
            }
            std::string ev_name;
            for (std::size_t q = j + 3; q < p; ++q)
                if (toks[q].kind == TokKind::Ident && toks[q].text != "event") ev_name = toks[q].text;
            if (!ev_name.empty()) {
                bool dup = false;
                for (auto const& e : cb.events) if (e == ev_name) dup = true;
                if (!dup) cb.events.push_back(ev_name);
            }
            i = p;
            continue;
        }

        // event declaration: event <Type>^ <Name>; → carried as a callback decl
        // so the control exposes the event in the winmd (consumers wire .onX).
        // Covers C# (event EventHandler<T>? Name;) and C++/CX (event X^ Name;).
        if (j < toks.size() && toks[j].kind == TokKind::Ident && toks[j].text == "event") {
            std::size_t p = j + 1;
            // skip the delegate type (generics + ? nullable)
            int depth = 0;
            while (p < toks.size()) {
                std::string const& t = toks[p].text;
                if (t == "<") ++depth;
                else if (t == ">") { if (depth) --depth; }
                else if (depth == 0 && (t == ";" || t == "{")) break;
                ++p;
            }
            // event name = last identifier before the terminator
            std::string ev_name;
            for (std::size_t q = j + 1; q < p; ++q)
                if (toks[q].kind == TokKind::Ident && toks[q].text != "event") ev_name = toks[q].text;
            if (!ev_name.empty()) {
                bool dup = false;
                for (auto const& e : cb.events) if (e == ev_name) dup = true;
                if (!dup) cb.events.push_back(ev_name);
            }
            if (p < toks.size() && toks[p].text == "{") {
                int bd = 1;
                ++p;
                while (p < toks.size() && bd > 0) {
                    if (toks[p].text == "{") ++bd;
                    if (toks[p].text == "}") --bd;
                    ++p;
                }
            }
            i = p;
            continue;
        }

        // method / field: <Type> <Name>(  or  <Type> <Name>;|=
        std::string type;
        std::size_t k = j;
        while (k + 1 < toks.size()) {
            if (toks[k].kind == TokKind::Ident) {
                std::string nxt = toks[k + 1].text;
                if (nxt == "(") {
                    std::string mname = toks[k].text;
                    bool dup = false;
                    for (auto const& m : cb.methods) if (m == mname) dup = true;
                    if (!dup && mname != cb.class_name && mname != "get" && mname != "set") {
                        cb.methods.push_back(mname);
                    }
                    std::size_t p = k + 2;
                    int pd = 1;
                    while (p < toks.size() && pd > 0) {
                        if (toks[p].text == "(") ++pd;
                        if (toks[p].text == ")") --pd;
                        ++p;
                    }
                    while (p < toks.size() && toks[p].text != ";" && toks[p].text != "{") ++p;
                    if (p < toks.size() && toks[p].text == "{") {
                        int bd = 1;
                        ++p;
                        while (p < toks.size() && bd > 0) {
                            if (toks[p].text == "{") ++bd;
                            if (toks[p].text == "}") --bd;
                            ++p;
                        }
                    }
                    i = p - 1;
                    break;
                }
                if (nxt == ";" || nxt == "=") {
                    Member m;
                    m.name = toks[k].text;
                    m.type = type;
                    m.kind = map_type(type);
                    m.writeable = true;
                    m.is_field = true;
                    cb.properties.push_back(std::move(m));
                    std::size_t p = k + 1;
                    while (p < toks.size() && toks[p].text != ";") ++p;
                    i = p;
                    break;
                }
            }
            type += toks[k].text;
            ++k;
        }
        if (k >= toks.size()) break;
    }
    return cb;
}

// §6  MIDL (.idl) extraction

inline auto parse_idl(std::string_view src) -> CodeBehind {
    CodeBehind cb;

    // Detect ExtendsContentIntoTitleBar in source text
    cb.extends_content_into_titlebar = detect_titlebar_extend(std::string(src));
    cb.window_title = detect_window_title(std::string(src));
    if (cb.extends_content_into_titlebar) cb.found = true;

    std::vector<std::string> no_strings;
    auto toks = tokenize_cs(src, no_strings);

    // find "runtimeclass <Name>" (inside namespace). An .idl often declares
    // model/helper classes first and the view class last (or behind
    // [default_interface]) - pick the class that looks like a UI view:
    // 1. one whose base type ends in Page/UserControl/Window/Control/App,
    // 2. else one preceded by [default_interface],
    // 3. else the LAST runtimeclass declared.
    struct Rc { std::size_t idx; std::string name; std::string base; bool def_iface; };
    std::vector<Rc> rcs;
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].kind == TokKind::Ident && toks[i].text == "runtimeclass"
            && i + 1 < toks.size() && toks[i+1].kind == TokKind::Ident) {
            Rc rc; rc.idx = i; rc.name = toks[i+1].text; rc.def_iface = false;
            // look backwards for [default_interface] on the preceding line
            for (std::size_t b = (i > 6 ? i - 6 : 0); b < i; ++b)
                if (toks[b].text == "default_interface") { rc.def_iface = true; break; }
            // base class: "runtimeclass X : Base"
            std::size_t j = i + 2;
            if (j < toks.size() && toks[j].text == ":") {
                ++j;
                while (j < toks.size() && toks[j].kind == TokKind::Ident) {
                    if (!rc.base.empty()) rc.base += ".";
                    rc.base += toks[j].text;
                    ++j;
                    if (j < toks.size() && toks[j].text == ",") { ++j; continue; }
                    break;
                }
            }
            rcs.push_back(std::move(rc));
        }
    }
    if (rcs.empty()) return cb;
    auto uiish = [](std::string const& b) {
        if (b.empty()) return false;
        for (auto const& suf : { "Page", "UserControl", "Window", "Control", "Application", "App" }) {
            if (b.size() >= std::string_view(suf).size()
                && b.compare(b.size() - std::string_view(suf).size(), std::string_view(suf).size(), suf) == 0)
                return true;
        }
        return false;
    };
    std::size_t cls_idx = std::string_view::npos;
    for (auto const& rc : rcs) if (uiish(rc.base)) { cls_idx = rc.idx; break; }
    if (cls_idx == std::string_view::npos)
        for (auto const& rc : rcs) if (rc.def_iface) { cls_idx = rc.idx; break; }
    if (cls_idx == std::string_view::npos) cls_idx = rcs.back().idx;

    std::string ns;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (toks[i].kind == TokKind::Ident && toks[i].text == "namespace") {
            ns.clear();
            std::size_t j = i + 1;
            while (j < toks.size() && toks[j].kind == TokKind::Ident) {
                if (!ns.empty()) ns += ".";
                ns += toks[j].text;
                ++j;
            }
            break;
        }
    }

    cb.class_name = toks[cls_idx + 1].text;
    cb.full_class_name = ns.empty() ? cb.class_name : ns + "." + cb.class_name;
    cb.found = true;

    // base class: "runtimeclass X : Base"
    std::size_t be = cls_idx + 2;
    if (be < toks.size() && toks[be].text == ":") {
        std::size_t j = be + 1;
        while (j < toks.size() && toks[j].kind == TokKind::Ident) {
            cb.base_class = toks[j].text;
            ++j;
            if (j < toks.size() && toks[j].text == ",") { ++j; continue; }
            break;
        }
    }

    // members: Type Name { get; set; };   or   ReturnType Method(params);
    std::size_t open = std::string_view::npos;
    for (std::size_t i = cls_idx + 1; i < toks.size(); ++i) {
        if (toks[i].text == "{") { open = i; break; }
    }
    if (open == std::string_view::npos) return cb;

    int depth = 0;
    for (std::size_t i = open; i < toks.size(); ++i) {
        if (toks[i].text == "{") ++depth;
        if (toks[i].text == "}") {
            --depth;
            if (depth == 0) break;
            continue;
        }
        if (depth > 1) continue;
        // skip attributes [default_interface] etc.
        while (i < toks.size() && toks[i].text == "[") {
            int ad = 1;
            ++i;
            while (i < toks.size() && ad > 0) {
                if (toks[i].text == "[") ++ad;
                if (toks[i].text == "]") --ad;
                ++i;
            }
        }
        if (i >= toks.size()) break;

        // pattern: Ident Type Name ('{' | '(')
        std::size_t k = i;
        while (k + 2 < toks.size()) {
            if (toks[k].kind == TokKind::Ident && toks[k+1].kind == TokKind::Ident
                && (toks[k+2].text == "{" || toks[k+2].text == "(")) break;
            ++k;
            if (toks[k].text == ";" || toks[k].text == "}") { k = std::string_view::npos; break; }
        }
        if (k == std::string_view::npos || k + 2 >= toks.size()) { ++i; continue; }
        std::string idl_type = toks[k].text;
        std::string mname = toks[k+1].text;

        if (toks[k+2].text == "(") {
            // method
            bool dup = false;
            for (auto const& m : cb.methods) if (m == mname) dup = true;
            if (!dup) cb.methods.push_back(mname);
            // skip to ';'
            std::size_t q = k + 3;
            while (q < toks.size() && toks[q].text != ";") ++q;
            i = q;
            continue;
        }

        // property: { get; set; } — determine writeable
        bool writeable = false;
        std::size_t q = k + 3;
        int bd = 1;
        while (q < toks.size() && bd > 0) {
            if (toks[q].text == "{") ++bd;
            if (toks[q].text == "}") --bd;
            if (bd == 1 && toks[q].kind == TokKind::Ident && toks[q].text == "set") writeable = true;
            ++q;
        }
        while (q < toks.size() && toks[q].text != ";") ++q;
        Member m;
        m.name = mname;
        m.type = idl_type;
        m.kind = map_idl_type(idl_type);
        m.writeable = writeable;
        cb.properties.push_back(std::move(m));
        i = q;
    }
    return cb;
}

// §7  C++ source (.cpp) extraction — method definitions

inline auto parse_cpp_source(std::string_view src) -> CodeBehind {
    // Only method names are collected here (for handler verification);
    // types come from the matching .idl / .h file.
    CodeBehind cb;
    std::vector<std::string> no_strings;
    auto toks = tokenize_cs(src, no_strings);
    // Detect ExtendsContentIntoTitleBar in source text
    cb.extends_content_into_titlebar = detect_titlebar_extend(std::string(src));
    cb.window_title = detect_window_title(std::string(src));
    if (cb.extends_content_into_titlebar) cb.found = true;

    // pattern: RetType Namespace::Class::Method( params ) { ... }
    // The tokenizer lexes '::' as a single token, so match Ident '::' Ident '('.
    // Nested qualifiers (winrt::MetaScrub::...) are consumed as the class part;
    // the method name is the last Ident before '('.
    for (std::size_t i = 0; i + 2 < toks.size(); ++i) {
        // find '::'
        if (toks[i].text != "::") continue;
        // preceding Ident = class (allow prior '::' segments: Class:: or ns::Class::)
        std::size_t cls = i;
        while (cls > 0 && toks[cls - 1].kind == TokKind::Ident) {
            // walk back over "Ident ::" pairs only
            std::size_t c2 = cls - 1;
            bool cont = false;
            if (c2 >= 2 && toks[c2 - 1].text == "::" && toks[c2 - 2].kind == TokKind::Ident) {
                cls = c2 - 1;
                cont = true;
            }
            if (!cont) { cls = c2; break; }
            if (c2 - 1 == 0) break;
        }
        if (cls >= i) continue;
        // following Ident = method name, then '('
        std::size_t m = i + 1;
        if (m >= toks.size() || toks[m].kind != TokKind::Ident) continue;
        if (m + 1 >= toks.size() || toks[m + 1].text != "(") continue;
        std::string mname = toks[m].text;
        // Out-of-line constructor definition (`Class::Class()`) names the
        // class: record it so the ctor body is captured under the class-name
        // key (collect_codebehind_wiring reads ctor wiring from there).
        if (cb.class_name.empty() && mname == toks[cls].text)
        {
            cb.class_name = toks[cls].text;
            cb.full_class_name = cb.class_name;
            cb.found = true;
        }
        bool dup = false;
        for (auto const& mm : cb.methods) if (mm == mname) dup = true;
        if (!dup) cb.methods.push_back(mname);
        // skip the parameter list and optional body
        std::size_t p = m + 1;
        int pd = 1;
        ++p;
        while (p < toks.size() && pd > 0) {
            if (toks[p].text == "(") ++pd;
            if (toks[p].text == ")") --pd;
            ++p;
        }
        if (p < toks.size() && toks[p].text == "{") {
            int bd = 1;
            ++p;
            while (p < toks.size() && bd > 0) {
                if (toks[p].text == "{") ++bd;
                if (toks[p].text == "}") --bd;
                ++p;
            }
        }
        i = p;
    }
    capture_codebehind_bodies(src, cb);
    return cb;
}

// §8  Dispatch by file extension

inline auto parse_code_behind(std::string_view src, std::string_view ext) -> CodeBehind {
    if (ext == ".cs")       return parse_csharp(src);
    if (ext == ".h"  || ext == ".hpp") return parse_cpp_header(src);
    if (ext == ".cpp" || ext == ".cxx") return parse_cpp_source(src);
    if (ext == ".idl")      return parse_idl(src);
    return {};
}

// §9  Pluggable extractor contract (csmeta.exe / cppmeta.exe JSON)
// Both pluggable extractors emit the same JSON:
//   { "name","namespace","fullName","base",
//     "properties":[{"name","type","kind","writeable","static","field"}],
//     "methods":[...] }
// Kind: "string"|"int"|"bool"|"double"|"float"|"other".

namespace jp { // tiny JSON value parser (private helper)

struct JsonValue {
    enum class Kind { Null, Bool, Str, Num, Arr, Obj } kind = Kind::Null;
    bool        b = false;
    double      num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;
};

inline auto skip_ws(std::string_view s, std::size_t& p) -> void {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
}

inline auto parse_json_value(std::string_view s, std::size_t& p) -> JsonValue {
    skip_ws(s, p);
    JsonValue v;
    if (p >= s.size()) return v;
    char c = s[p];
    if (c == '"') {
        ++p;
        v.kind = JsonValue::Kind::Str;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                ++p;
                switch (s[p]) {
                    case 'n': v.str += '\n'; break;
                    case 'r': v.str += '\r'; break;
                    case 't': v.str += '\t'; break;
                    case 'u': {
                        // \uXXXX → UTF-8 (only BMP, escapes are ASCII in practice)
                        if (p + 4 < s.size()) {
                            unsigned cp = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = s[p + 1 + i];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            }
                            p += 4;
                            if (cp < 0x80) v.str += static_cast<char>(cp);
                            else if (cp < 0x800) {
                                v.str += static_cast<char>(0xC0 | (cp >> 6));
                                v.str += static_cast<char>(0x80 | (cp & 0x3F));
                            } else {
                                v.str += static_cast<char>(0xE0 | (cp >> 12));
                                v.str += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                v.str += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default: v.str += s[p]; break;
                }
                ++p;
            } else {
                v.str += s[p];
                ++p;
            }
        }
        if (p < s.size()) ++p; // closing quote
        return v;
    }
    if (c == '{') {
        ++p;
        v.kind = JsonValue::Kind::Obj;
        while (true) {
            skip_ws(s, p);
            if (p < s.size() && s[p] == '}') { ++p; break; }
            auto key = parse_json_value(s, p);
            skip_ws(s, p);
            if (p < s.size() && s[p] == ':') ++p;
            auto val = parse_json_value(s, p);
            v.obj.emplace_back(key.str, std::move(val));
            skip_ws(s, p);
            if (p < s.size() && s[p] == ',') ++p;
            else if (p < s.size() && s[p] == '}') { ++p; break; }
        }
        return v;
    }
    if (c == '[') {
        ++p;
        v.kind = JsonValue::Kind::Arr;
        while (true) {
            skip_ws(s, p);
            if (p < s.size() && s[p] == ']') { ++p; break; }
            v.arr.push_back(parse_json_value(s, p));
            skip_ws(s, p);
            if (p < s.size() && s[p] == ',') ++p;
            else if (p < s.size() && s[p] == ']') { ++p; break; }
        }
        return v;
    }
    if (c == 't') { v.kind = JsonValue::Kind::Bool; v.b = true; p += 4; return v; }
    if (c == 'f') { v.kind = JsonValue::Kind::Bool; v.b = false; p += 5; return v; }
    if (c == 'n') { p += 4; return v; } // null
    // number
    v.kind = JsonValue::Kind::Num;
    std::string num;
    while (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '-'
                            || s[p] == '+' || s[p] == '.' || s[p] == 'e' || s[p] == 'E')) {
        num += s[p++];
    }
    v.num = std::strtod(num.c_str(), nullptr);
    return v;
}

inline auto find(JsonValue const& o, std::string_view key) -> JsonValue const* {
    for (auto const& [k, v] : o.obj) if (k == key) return &v;
    return nullptr;
}

inline auto str_of(JsonValue const* v) -> std::string {
    return (v && v->kind == JsonValue::Kind::Str) ? v->str : std::string();
}

inline auto bool_of(JsonValue const* v) -> bool {
    return v && v->kind == JsonValue::Kind::Bool && v->b;
}

inline auto kind_from_name(std::string_view k) -> CsTypeKind {
    if (k == "string") return CsTypeKind::String;
    if (k == "int")    return CsTypeKind::Int;
    if (k == "bool")   return CsTypeKind::Bool;
    if (k == "double") return CsTypeKind::Double;
    if (k == "float")  return CsTypeKind::Float;
    return CsTypeKind::Other;
}

} // namespace jp

// Parses the pluggable extractor JSON contract into a CodeBehind.
inline auto parse_meta_json(std::string_view json) -> CodeBehind {
    CodeBehind cb;
    std::size_t p = 0;
    auto root = jp::parse_json_value(json, p);
    if (root.kind != jp::JsonValue::Kind::Obj) return cb;

    cb.class_name = jp::str_of(jp::find(root, "name"));
    cb.full_class_name = jp::str_of(jp::find(root, "fullName"));
    if (cb.full_class_name.empty()) cb.full_class_name = cb.class_name;
    cb.base_class = jp::str_of(jp::find(root, "base"));
    cb.found = !cb.class_name.empty();

    if (auto* props = jp::find(root, "properties");
        props && props->kind == jp::JsonValue::Kind::Arr) {
        for (auto const& pr : props->arr) {
            if (pr.kind != jp::JsonValue::Kind::Obj) continue;
            Member m;
            m.name = jp::str_of(jp::find(pr, "name"));
            if (m.name.empty()) continue;
            m.type = jp::str_of(jp::find(pr, "type"));
            m.kind = jp::kind_from_name(jp::str_of(jp::find(pr, "kind")));
            m.writeable = jp::bool_of(jp::find(pr, "writeable"));
            m.is_static = jp::bool_of(jp::find(pr, "static"));
            m.is_field = jp::bool_of(jp::find(pr, "field"));
            cb.properties.push_back(std::move(m));
        }
    }
    if (auto* methods = jp::find(root, "methods");
        methods && methods->kind == jp::JsonValue::Kind::Arr) {
        for (auto const& m : methods->arr) {
            if (m.kind == jp::JsonValue::Kind::Str) cb.methods.push_back(m.str);
        }
    }
    return cb;
}

} // namespace xamlconv::lang
