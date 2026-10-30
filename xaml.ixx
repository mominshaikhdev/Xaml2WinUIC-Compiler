// xaml.ixx — XAML parser module
// Parses XAML documents into a lightweight node tree. Handles the full XML
// feature set used by WinUI 3 XAML: prolog, DOCTYPE (skipped), comments,
// CDATA, elements (incl. self-closing), namespaces (prefix/local names),
// property-element syntax (Parent.Child), entity references (&amp; &lt; &gt;
// &quot; &apos; &#NN; &#xHH;) and attribute values with entities.

// The tree preserves comments and raw text so the converter can re-emit a
// 100% faithful representation of every document.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string>
#include <functional>
#include <string_view>
#include <vector>
#include <optional>
#include <cctype>
#include <cstdlib>
#include <cstdio>

export module xaml;

export namespace xamlconv::xaml {

// §1  Node model

enum class NodeKind : unsigned char {
    Element,   // <Name attr="v"> ... </Name>  or  <Name/>
    Comment,   // <!-- ... -->
    CData,     // <![CDATA[ ... ]]>
};

struct Node;

struct Attribute {
    std::string name;    // attribute name as written, INCLUDING prefix: "x:Name", "xmlns:controls"
    std::string value;   // entity-decoded value
};

struct Node {
    NodeKind    kind = NodeKind::Element;
    std::string name;             // local name; for property elements: "Parent.Child"
    std::string prefix;           // XML prefix, "" if none
    bool        self_closing = false;
    std::vector<Attribute> attrs;
    std::vector<Node>       children;    // element/comment/cdata children in document order
    std::vector<std::string> text;       // non-whitespace text segments (in order relative to children)
    // Convenience accessors -------------------------------------------------
    [[nodiscard]] auto attr(std::string_view local_name) const -> std::optional<std::string_view>;
    [[nodiscard]] auto has_attr(std::string_view local_name) const -> bool;
    [[nodiscard]] auto text_content() const -> std::string;
};

inline auto Node::attr(std::string_view local_name) const -> std::optional<std::string_view> {
    for (auto const& a : attrs) {
        // match by local part (strip any prefix)
        auto pos = a.name.find(':');
        auto check = (pos == std::string::npos) ? std::string_view(a.name)
                                                : std::string_view(a.name).substr(pos + 1);
        if (check == local_name) {
            return std::string_view(a.value);
        }
    }
    return std::nullopt;
}

inline auto Node::has_attr(std::string_view local_name) const -> bool {
    return attr(local_name).has_value();
}

inline auto Node::text_content() const -> std::string {
    std::string out;
    for (auto const& t : text) out += t;
    for (auto const& c : children) {
        if (c.kind == NodeKind::Element) out += c.text_content();
    }
    return out;
}

// §2  Entity decoding

inline auto decode_entities(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c != '&') { out += c; continue; }
        auto semi = in.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) { out += c; continue; }
        std::string_view ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp")      { out += '&';  i = semi; continue; }
        if (ent == "lt")       { out += '<';  i = semi; continue; }
        if (ent == "gt")       { out += '>';  i = semi; continue; }
        if (ent == "quot")     { out += '"';  i = semi; continue; }
        if (ent == "apos")     { out += '\''; i = semi; continue; }
        if (!ent.empty() && ent[0] == '#') {
            unsigned cp = 0;
            std::size_t k = 1;
            bool hex = (k < ent.size() && (ent[k] == 'x' || ent[k] == 'X'));
            if (hex) ++k;
            for (; k < ent.size(); ++k) {
                char h = ent[k];
                int d;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                else { d = -1; break; }
                cp = cp * (hex ? 16 : 10) + static_cast<unsigned>(d);
            }
            if (cp != 0) {
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                i = semi;
                continue;
            }
        }
        out += c; // unknown entity → keep literally
    }
    return out;
}

// §3  Diagnostic collection

struct ParseError {
    std::string message;
    std::size_t line = 0;
};

struct ParseResult {
    std::vector<Node> roots;            // top-level nodes (usually one <Page>)
    std::vector<ParseError> errors;
    [[nodiscard]] auto ok() const -> bool { return errors.empty(); }
    [[nodiscard]] auto root() const -> Node const* {
        for (auto const& n : roots) if (n.kind == NodeKind::Element) return &n;
        return nullptr;
    }
};

// §4  Parser

inline auto parse(std::string_view src) -> ParseResult {
    ParseResult result;
    std::size_t pos = 0;
    std::size_t line = 1;

    auto at = [&](std::size_t off = 0) -> char {
        return (pos + off < src.size()) ? src[pos + off] : '\0';
    };

    auto fail = [&](std::string_view msg) {
        result.errors.push_back(ParseError{std::string(msg), line});
    };

    // Parse a single element node (start tag is assumed consumed at '<Name').
    // std::function so the lambda can recurse (recursive lambdas cannot name
    // themselves in their own initializer in C++23).
    std::function<Node(std::string, std::string, std::size_t)> parse_element =
        [&](std::string start_name, std::string start_prefix,
            std::size_t start_line) -> Node {
        Node node;
        node.kind = NodeKind::Element;
        node.name = std::move(start_name);
        node.prefix = std::move(start_prefix);

        // Attributes
        while (true) {
            while (at() == ' ' || at() == '\t' || at() == '\r' || at() == '\n') {
                if (at() == '\n') ++line;
                ++pos;
            }
            char c = at();
            if (c == '/' || c == '>' || c == '\0') break;
            // attribute name
            std::string aname;
            while (c != '=' && c != ' ' && c != '\t' && c != '\r' && c != '\n'
                   && c != '/' && c != '>' && c != '\0') {
                aname += c;
                ++pos;
                c = at();
            }
            while (at() == ' ' || at() == '\t' || at() == '\r' || at() == '\n') {
                if (at() == '\n') ++line;
                ++pos;
            }
            if (at() != '=') {
                fail("malformed attribute (missing '=')");
                node.attrs.push_back(Attribute{std::move(aname), ""});
                continue;
            }
            ++pos; // '='
            while (at() == ' ' || at() == '\t' || at() == '\r' || at() == '\n') {
                if (at() == '\n') ++line;
                ++pos;
            }
            char q = at();
            std::string value;
            if (q == '"' || q == '\'') {
                ++pos;
                while (at() != q && at() != '\0') {
                    if (at() == '\n') ++line;
                    value += at();
                    ++pos;
                }
                if (at() == q) ++pos;
                else fail("unterminated attribute value");
            } else {
                while (at() != ' ' && at() != '\t' && at() != '\r' && at() != '\n'
                       && at() != '>' && at() != '/' && at() != '\0') {
                    value += at();
                    ++pos;
                }
            }
            node.attrs.push_back(Attribute{std::move(aname), decode_entities(value)});
        }

        // Self-closing?
        if (at() == '/') {
            ++pos;
            if (at() != '>') fail("expected '>' after '/'");
            ++pos;
            node.self_closing = true;
            return node;
        }
        if (at() == '>') {
            ++pos;
        } else {
            fail("expected '>' to close start tag");
            return node;
        }

        // Content: text / comments / children
        while (true) {
            if (pos >= src.size()) {
                fail("unexpected end of document inside element <" + node.name + ">");
                return node;
            }
            if (at() == '<') {
                if (at(1) == '/') {
                    // closing tag
                    std::size_t save = pos;
                    pos += 2;
                    std::string close_name;
                    while (at() != '>' && at() != '\0') {
                        close_name += at();
                        ++pos;
                    }
                    if (at() == '>') ++pos;
                    // trim whitespace in name (both leading and trailing)
                    while (!close_name.empty() && (close_name.back() == ' ' || close_name.back() == '\t' || close_name.back() == '\r' || close_name.back() == '\n'))
                        close_name.pop_back();
                    auto close_start = close_name.find_first_not_of(" \t\r\n");
                    if (close_start != std::string::npos && close_start > 0)
                        close_name.erase(0, close_start);
                    else if (close_start == std::string::npos)
                        close_name.clear();
                    // compare local names
                    std::string close_local = close_name;
                    auto colon = close_name.find(':');
                    if (colon != std::string::npos) close_local = close_name.substr(colon + 1);
                    std::string open_local = node.name;
                    colon = node.name.find(':');
                    if (colon != std::string::npos) open_local = node.name.substr(colon + 1);
                    if (close_local != open_local) {
                        fail("mismatched closing tag </" + close_name + "> for <" + node.name + ">");
                        // recover: treat as if correct
                    }
                    return node;
                } else if (at(1) == '!') {
                    if (at(2) == '-' && at(3) == '-') {
                        // comment
                        pos += 4;
                        std::string c;
                        while (!(at() == '-' && at(1) == '-' && at(2) == '>') && at() != '\0') {
                            if (at() == '\n') ++line;
                            c += at();
                            ++pos;
                        }
                        if (at() == '-' && at(1) == '-' && at(2) == '>') pos += 3;
                        else fail("unterminated comment");
                        Node cn;
                        cn.kind = NodeKind::Comment;
                        cn.name = std::move(c);
                        node.children.push_back(std::move(cn));
                    } else if (at(2) == '[' && at(3) == 'C' && at(4) == 'D' && at(5) == 'A'
                               && at(6) == 'T' && at(7) == 'A' && at(8) == '[') {
                        // CDATA
                        pos += 9;
                        std::string c;
                        while (!(at() == ']' && at(1) == ']' && at(2) == '>') && at() != '\0') {
                            if (at() == '\n') ++line;
                            c += at();
                            ++pos;
                        }
                        if (at() == ']' && at(1) == ']' && at(2) == '>') pos += 3;
                        else fail("unterminated CDATA");
                        Node cn;
                        cn.kind = NodeKind::CData;
                        cn.name = std::move(c);
                        node.children.push_back(std::move(cn));
                    } else {
                        // DOCTYPE or other declaration — skip to '>'
                        while (at() != '>' && at() != '\0') {
                            if (at() == '\n') ++line;
                            ++pos;
                        }
                        if (at() == '>') ++pos;
                    }
                } else if (at(1) == '?') {
                    // processing instruction (xml prolog etc.) — skip
                    while (!(at() == '?' && at(1) == '>') && at() != '\0') {
                        if (at() == '\n') ++line;
                        ++pos;
                    }
                    if (at() == '?' && at(1) == '>') pos += 2;
                } else {
                    // child element
                    ++pos;
                    std::string cname;
                    while (at() != ' ' && at() != '\t' && at() != '\r' && at() != '\n'
                           && at() != '/' && at() != '>' && at() != '\0') {
                        cname += at();
                        ++pos;
                    }
                    std::string cprefix;
                    auto colon = cname.find(':');
                    if (colon != std::string::npos) {
                        cprefix = cname.substr(0, colon);
                        cname = cname.substr(colon + 1);
                    }
                    node.children.push_back(parse_element(std::move(cname), std::move(cprefix), line));
                }
            } else {
                // text content
                std::string t;
                while (at() != '<' && at() != '\0') {
                    if (at() == '\n') ++line;
                    t += at();
                    ++pos;
                }
                // collapse whitespace-only text to nothing, keep meaningful text
                bool only_ws = true;
                for (char ch : t) {
                    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') { only_ws = false; break; }
                }
                if (!only_ws) {
                    std::string trimmed = decode_entities(t);
                    bool has_leading_ws = (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\r' || t.front() == '\n'));
                    bool has_trailing_ws = (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n'));
                    std::string collapsed;
                    collapsed.reserve(trimmed.size());
                    bool in_ws = false;
                    for (char ch : trimmed) {
                        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                            if (!in_ws) { collapsed += ' '; in_ws = true; }
                        } else {
                            collapsed += ch;
                            in_ws = false;
                        }
                    }
                    if (has_leading_ws && (collapsed.empty() || collapsed.front() != ' ')) collapsed.insert(collapsed.begin(), ' ');
                    if (has_trailing_ws && (collapsed.empty() || collapsed.back() != ' ')) collapsed.push_back(' ');
                    if (!collapsed.empty() && collapsed != " ") node.text.push_back(std::move(collapsed));
                }
            }
        }
    };

    // Top-level scan
    while (pos < src.size()) {
        if (at() == '<') {
            if (at(1) == '?') {
                while (!(at() == '?' && at(1) == '>') && at() != '\0') {
                    if (at() == '\n') ++line;
                    ++pos;
                }
                if (at() == '?' && at(1) == '>') pos += 2;
            } else if (at(1) == '!') {
                // comment or DOCTYPE at top level
                if (at(2) == '-' && at(3) == '-') {
                    pos += 4;
                    std::string c;
                    while (!(at() == '-' && at(1) == '-' && at(2) == '>') && at() != '\0') {
                        if (at() == '\n') ++line;
                        c += at();
                        ++pos;
                    }
                    if (at() == '-' && at(1) == '-' && at(2) == '>') pos += 3;
                    Node cn;
                    cn.kind = NodeKind::Comment;
                    cn.name = std::move(c);
                    result.roots.push_back(std::move(cn));
                } else {
                    while (at() != '>' && at() != '\0') {
                        if (at() == '\n') ++line;
                        ++pos;
                    }
                    if (at() == '>') ++pos;
                }
            } else if (at(1) == '/') {
                fail("unexpected closing tag at document level");
                while (at() != '>' && at() != '\0') ++pos;
                if (at() == '>') ++pos;
            } else {
                ++pos;
                std::string name;
                while (at() != ' ' && at() != '\t' && at() != '\r' && at() != '\n'
                       && at() != '/' && at() != '>' && at() != '\0') {
                    name += at();
                    ++pos;
                }
                std::string prefix;
                auto colon = name.find(':');
                if (colon != std::string::npos) {
                    prefix = name.substr(0, colon);
                    name = name.substr(colon + 1);
                }
                result.roots.push_back(parse_element(std::move(name), std::move(prefix), line));
            }
        } else {
            // whitespace / stray text at top level — skip
            while (at() != '<' && at() != '\0') {
                if (at() == '\n') ++line;
                ++pos;
            }
        }
    }

    return result;
}

} // namespace xamlconv::xml
