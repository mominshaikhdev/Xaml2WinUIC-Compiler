// cli.ixx — xaml2winuic command-line interface
// ONE GOAL: scan a project's XAML + ALL of its C# (or C++) code-behind and
// emit .winui source files that reproduce the exact layout, colors and
// design of every XAML document — 100% equivalent UI trees.

// Usage:
// xaml2winuic.exe <project-dir|file> [more inputs...] [options]
// Options:
// -o, -out_dir <dir>         output directory (default: <input>/winui_src)
// -l, -language auto|cs|cpp   code-behind dialect (default: auto)
// -h, --help

// The built-in C#/C++/C++/CX parsers are used unless a pluggable extractor
// DLL is provided (in-process LoadLibrary, stdcall export):
// int ExtractCodeBehind(const wchar_t* file, wchar_t** json_out)
// → S_OK + CoTaskMemAlloc'd JSON string.
// { "name","namespace","fullName","base",
// "properties":[{"name","type","kind","writeable","static","field"}],
// "methods":[...] }
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <format>
#include <print>
#include <fstream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <set>
#include <utility>
#include <cstdint>
#include <cstdlib>

export module xamlconv.cli;

import xaml;
import lang;
import convert;
import xamlconv.ext;

export namespace xamlconv::cli {

// §1  Helpers

// wchar_t → UTF-8 (avoids C4244 wchar_t → char narrowing).
inline auto wstr_to_str(std::wstring const& ws) -> std::string {
    if (ws.empty()) return {};
    int sz = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (sz <= 0) return {};
    std::string out(static_cast<std::size_t>(sz), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), sz, nullptr, nullptr);
    return out;
}

// §2  Options

struct CliOptions {
    std::vector<std::string> inputs;
    std::string out_dir;
    bool        out_dir_set = false;
    std::string language = "auto";
    std::string cs_extractor;
    std::string cpp_extractor;
    std::vector<std::string> control_files;   // runtime control-registry JSON files
};

inline auto error(std::string_view msg) -> void {
    std::println(stderr, "error: {}", msg);
}

inline auto sanitize_sample_xaml(std::string_view sample) -> std::string {
    std::string without_comments;
    without_comments.reserve(sample.size());
    for (std::size_t i = 0; i < sample.size();) {
        if (sample.substr(i, 4) == "<!--") {
            auto end = sample.find("-->", i + 4);
            auto comment_end = end == std::string_view::npos ? sample.size() : end + 3;
            for (; i < comment_end; ++i)
                if (sample[i] == '\n') without_comments += '\n';
            continue;
        }
        without_comments += sample[i++];
    }

    sample = without_comments;
    for (std::size_t marker = sample.find("$(");
         marker != std::string_view::npos;
         marker = sample.find("$(", marker)) {
        auto tag_start = sample.rfind('<', marker);
        auto tag_end = sample.find('>', marker);
        if (tag_start == std::string_view::npos || tag_end == std::string_view::npos) break;
        auto tag_name_end = sample.find_first_of(" \t\r\n/>", tag_start + 1);
        if (tag_name_end == std::string_view::npos || marker > tag_name_end) {
            marker += 2;
            continue;
        }

        auto parent_start = sample.rfind('<', tag_start - 1);
        if (parent_start == std::string_view::npos) {
            marker = tag_end + 1;
            continue;
        }
        auto parent_name_end = sample.find_first_of(" \t\r\n/>", parent_start + 1);
        if (parent_name_end == std::string_view::npos || parent_name_end == parent_start + 1) {
            marker = tag_end + 1;
            continue;
        }
        auto parent_name = sample.substr(parent_start + 1, parent_name_end - parent_start - 1);
        if (parent_name.starts_with("/") || parent_name.find('.') == std::string_view::npos) {
            marker = tag_end + 1;
            continue;
        }
        std::string closing_tag = "</" + std::string(parent_name) + ">";
        auto close = sample.find(closing_tag, tag_end + 1);
        if (close == std::string_view::npos) {
            marker = tag_end + 1;
            continue;
        }
        std::string reduced(sample);
        reduced.erase(parent_start, close + closing_tag.size() - parent_start);
        without_comments = std::move(reduced);
        sample = without_comments;
        marker = sample.find("$(", parent_start);
    }

    std::string sanitized;
    sanitized.reserve(sample.size());
    bool in_tag = false;
    char quote = '\0';
    std::size_t quote_start = 0;

    for (std::size_t i = 0; i < sample.size();) {
        char ch = sample[i];
        if (quote == '\0' && in_tag && sample.substr(i, 3) == "...") {
            i += 3;
            continue;
        }
        if (in_tag && sample.substr(i, 2) == "$(") {
            auto end = sample.find(')', i + 2);
            if (end != std::string_view::npos) {
                if (quote != '\0') {
                    auto attribute_start = quote_start;
                    while (attribute_start > 0
                        && sanitized[attribute_start - 1] != ' '
                        && sanitized[attribute_start - 1] != '\t'
                        && sanitized[attribute_start - 1] != '\r'
                        && sanitized[attribute_start - 1] != '\n'
                        && sanitized[attribute_start - 1] != '<')
                        --attribute_start;
                    sanitized.resize(attribute_start);
                    auto value_end = sample.find(quote, end + 1);
                    if (value_end == std::string_view::npos) {
                        i = end + 1;
                        quote = '\0';
                        continue;
                    }
                    i = value_end + 1;
                    quote = '\0';
                } else if (sanitized.size() >= 1
                    && (sanitized.back() == '<'
                        || (sanitized.size() >= 2
                            && sanitized.substr(sanitized.size() - 2) == "</"))) {
                    sanitized += "CircleEase";
                    i = end + 1;
                } else {
                    i = end + 1;
                }
                continue;
            }
        }

        sanitized += ch;
        ++i;
        if (quote != '\0') {
            if (ch == quote) quote = '\0';
        } else if (in_tag && (ch == '"' || ch == '\'')) {
            quote = ch;
            quote_start = sanitized.size() - 1;
        } else if (ch == '<') {
            in_tag = true;
        } else if (ch == '>') {
            in_tag = false;
        }
    }
    return sanitized;
}

// Split a comma-separated list ("a.xaml,b.xaml") — spaces around items are
// trimmed. Empty items are dropped. Each item may be wrapped in double
// quotes so individual items containing spaces can be quoted:
//   xaml2winuic -f "\"a b.xaml\",\"c d.xaml\""
inline auto split_csv(std::string_view s) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        auto comma = s.find(',', pos);
        std::string_view item = (comma == std::string_view::npos)
            ? s.substr(pos) : s.substr(pos, comma - pos);
        auto b = item.find_first_not_of(" \t");
        auto e = item.find_last_not_of(" \t");
        if (b != std::string_view::npos && e >= b) {
            auto trimmed = item.substr(b, e - b + 1);
            if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
                trimmed = trimmed.substr(1, trimmed.size() - 2);
            out.emplace_back(trimmed);
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return out;
}

inline auto print_help(char const* prog) -> void {
    (void)prog;
    std::println("");
    std::println("xaml2winuic — scans a project's XAML + C#/C++ code-behind and emits .winui");
    std::println("              files that match the exact layout, colors and design of the XAML.");
    std::println("");
    std::println("USAGE:");
    std::println("  xaml2winuic <project-dir|file> [more inputs...] [options]");
    std::println("  xaml2winuic -f file1.xaml,file2.xaml [options]");
    std::println("");
    std::println("OPTIONS:");
    std::println("  -f, -files <csv>          comma-separated .xaml files (positional inputs");
    std::println("                            accept commas too, like winuic's -files)");
    std::println("  -o, -out_dir <dir>       output directory (default: <input>/winui_src)");
    std::println("  -l, -language auto|cs|cpp  code-behind dialect (default: auto)");
    std::println("  -c, -control <file>      control-registry JSON (ANY name, repeatable,");
    std::println("                           no recompile); every *.json next to the exe is");
    std::println("                           auto-loaded as a control registry too");
    std::println("  -h, -help                 show this help");
    std::println("");
    std::println("ASSETS & LICENSE:");
    std::println("  Referenced assets and any LICENSE file found in the input root are");
    std::println("  automatically copied to the output directory.");
    std::println("");
    std::println("A *.json control registry next to xaml2winuic.exe is auto-loaded (third-party");
    std::println("");
    std::println("EXAMPLES:");
    std::println("  xaml2winuic WinUI-Gallery");
    std::println("  xaml2winuic Cpp-WinUI3-MetaScrub-Unpackaged -l cpp");
    std::println("");
}

inline auto parse_args(int argc, char** argv) -> std::optional<CliOptions> {
    CliOptions o;
    // Resolve a relative switch path ("..\\out", "proj\\App.xaml") against
    // the current directory so conversion works from any launch directory.
    auto norm_path = [](std::string const& value) -> std::string {
        // CLI path cleanup: cmd.exe can leave a stray double quote on an
        // argument when a quoted path ends with a backslash (e.g.
        //  -o "M:\out with space\"  arrives as argv  M:\out with space" ).
        // Quotes are illegal in Windows file names, so strip them.
        std::string v = value;
        while (!v.empty() && v.front() == '"') v.erase(v.begin());
        while (!v.empty() && v.back() == '"') v.pop_back();
        if (v.size() < 2) return v;
        if (v[1] == ':') return v;                      // drive-absolute
        if (v[0] == '/' || v[0] == '\\') return v;  // rooted
        std::error_code ec;
        auto abs = std::filesystem::absolute(v, ec);
        if (ec) return v;
        auto canon = std::filesystem::weakly_canonical(abs, ec);
        if (ec) return abs.string();
        return canon.string();
    };
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "-help" || a == "--help" || a == "-h" || a == "/?") { print_help(argv[0]); return std::nullopt; }
        else if (a == "-out_dir" || a == "-o") {
            if (++i >= argc) { error("-out_dir requires a value"); return std::nullopt; }
            o.out_dir = norm_path(argv[i]);
            o.out_dir_set = true;
        }
        else if (a == "-language" || a == "-l") {
            if (++i >= argc) { error("-language requires a value"); return std::nullopt; }
            o.language = argv[i];
            if (o.language != "auto" && o.language != "cs" && o.language != "cpp") {
                error("-language must be auto, cs or cpp");
                return std::nullopt;
            }
        }
        else if (a == "-cs_extractor") {
            if (++i >= argc) { error("-cs_extractor requires a path"); return std::nullopt; }
            o.cs_extractor = argv[i];
        }
        else if (a == "-cpp_extractor") {
            if (++i >= argc) { error("-cpp_extractor requires a path"); return std::nullopt; }
            o.cpp_extractor = argv[i];
        }
        else if (a == "-control" || a == "-c") {
            if (++i >= argc) { error("-control requires a JSON path"); return std::nullopt; }
            o.control_files.emplace_back(norm_path(argv[i]));
        }
        else if (a == "-files" || a == "-f") {
            // comma-separated list of files (same syntax as winuic -files)
            if (++i >= argc) { error("-files requires a value"); return std::nullopt; }
            for (auto& item : split_csv(argv[i])) o.inputs.emplace_back(norm_path(item));
        }
        else if (a.starts_with("-")) { error(std::format("unknown option '{}'", a)); return std::nullopt; }
        else {
            // positional input: a comma-separated list is allowed too
            for (auto& item : split_csv(a)) o.inputs.emplace_back(norm_path(item));
        }
    }
    if (o.inputs.empty()) {
        error("no input project/directory specified. Run with -help for usage.");
        return std::nullopt;
    }
    return o;
}

// §2  File loading (UTF-8 BOM / UTF-16 detection) & Win32 FS helpers
// Filesystem I/O is done via Win32 APIs: MSVC's std::filesystem internals
// (_Bitmask types) cannot be re-instantiated across module boundaries.
// The exe carries a longPathAware manifest (no 260-char limit); longify()
// adds the \\?\ prefix for paths near the old limit so deeply nested
// projects always work.

inline auto longify(std::filesystem::path const& p) -> std::wstring {
    std::wstring w = p.wstring();
    if (w.size() > 250 && !w.starts_with(L"\\\\?\\") && w.size() >= 3 && w[1] == L':') {
        w = L"\\\\?\\" + w;
    }
    return w;
}

inline auto file_exists(std::filesystem::path const& p) -> bool {
    return ::GetFileAttributesW(longify(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline auto is_file(std::filesystem::path const& p) -> bool {
    DWORD attr = ::GetFileAttributesW(longify(p).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

inline auto is_directory(std::filesystem::path const& p) -> bool {
    DWORD attr = ::GetFileAttributesW(longify(p).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Recursive create of every missing directory component.
inline auto ensure_dir(std::filesystem::path const& p) -> void {
    std::wstring w = longify(p);
    for (std::size_t i = (w.starts_with(L"\\\\?\\") ? 4u : 0u); i < w.size(); ++i) {
        if (w[i] == L'\\' || w[i] == L'/') {
            w[i] = L'\0';
            ::CreateDirectoryW(w.c_str(), nullptr);
            w[i] = L'\\';
        }
    }
    ::CreateDirectoryW(w.c_str(), nullptr);
}

inline auto delete_file(std::filesystem::path const& p) -> void {
    ::DeleteFileW(longify(p).c_str());
}

inline auto ext_of(std::filesystem::path const& p) -> std::string {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return e;
}

// Recursive file listing (Win32, no std::filesystem I/O in module code).
inline auto collect_xaml_files(std::filesystem::path const& root,
                               std::vector<std::filesystem::path>& out) -> void {
    std::wstring dir = longify(root);
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::filesystem::path full = root / name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collect_xaml_files(full, out);
        } else {
            std::string e = ext_of(full);
            if (e == ".xaml") out.push_back(full);
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

inline auto collect_csharp_files(std::filesystem::path const& root,
                                 std::vector<std::filesystem::path>& out) -> void {
    std::wstring dir = longify(root);
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (name == L"bin" || name == L"obj" || name == L".git") continue;
            collect_csharp_files(root / name, out);
        } else {
            auto full = root / name;
            if (ext_of(full) == ".cs") out.push_back(std::move(full));
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// Relative path of `full` under `root`, slash-separated.
inline auto rel_under(std::filesystem::path const& root, std::filesystem::path const& full) -> std::string {
    // Use weakly_canonical to handle "." root and relative paths properly
    auto r = std::filesystem::weakly_canonical(root);
    auto f = std::filesystem::weakly_canonical(full);
    auto rstr = r.wstring();
    auto fstr = f.wstring();
    auto pos = fstr.find(rstr);
    if (pos != std::wstring::npos) fstr = fstr.substr(pos + rstr.size());
    while (!fstr.empty() && (fstr.front() == L'\\' || fstr.front() == L'/')) fstr.erase(0, 1);
    std::string out;
    for (wchar_t c : fstr) out += (c == L'\\') ? '/' : static_cast<char>(c);
    return out;
}

inline auto utf16_to_utf8(std::u16string_view s) -> std::string {
    if (s.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<wchar_t const*>(s.data()),
                                  static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<wchar_t const*>(s.data()),
                          static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

inline auto read_all(std::ifstream& in) -> std::string {
    std::string data;
    char buf[16384];
    while (in.read(buf, sizeof buf)) data.append(buf, sizeof buf);
    data.append(buf, static_cast<std::size_t>(in.gcount()));
    return data;
}

inline auto load_text_file(std::filesystem::path const& p) -> std::string {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string data = read_all(in);

    // UTF-16 LE / BE
    if (data.size() >= 2) {
        auto b0 = static_cast<unsigned char>(data[0]);
        auto b1 = static_cast<unsigned char>(data[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
            std::u16string u16;
            for (std::size_t i = 2; i + 1 < data.size(); i += 2) {
                u16.push_back(static_cast<char16_t>(
                    static_cast<unsigned char>(data[i]) |
                    (static_cast<unsigned char>(data[i + 1]) << 8)));
            }
            return utf16_to_utf8(u16);
        }
        if (b0 == 0xFE && b1 == 0xFF) {
            std::u16string u16;
            for (std::size_t i = 2; i + 1 < data.size(); i += 2) {
                u16.push_back(static_cast<char16_t>(
                    (static_cast<unsigned char>(data[i]) << 8) |
                    static_cast<unsigned char>(data[i + 1])));
            }
            return utf16_to_utf8(u16);
        }
    }
    // UTF-8 BOM
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF
        && static_cast<unsigned char>(data[1]) == 0xBB
        && static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return data;
}

// §3  Code-behind resolution (built-in parsers, pluggable extractor override)

// Runs a pluggable extractor DLL in-process for one code-behind source file.
// Contract (stdcall export):
//   int ExtractCodeBehind(const wchar_t* file, wchar_t** json_out)
// → S_OK(0) + CoTaskMemAlloc'd JSON string on success.
inline auto run_extractor(std::string const& target, std::filesystem::path const& file,
                          std::string& output) -> bool {
    std::wstring wpath = longify(target);
    HMODULE mod = ::LoadLibraryW(wpath.c_str());
    if (!mod) return false;
    using Fn = int(__stdcall*)(wchar_t const*, wchar_t**);
    auto fn = reinterpret_cast<Fn>(::GetProcAddress(mod, "ExtractCodeBehind"));
    if (!fn) {
        ::FreeLibrary(mod);
        return false;
    }
    wchar_t* json = nullptr;
    int hr = fn(file.wstring().c_str(), &json);
    if (hr == 0 && json) {
        std::wstring w(json);
        ::CoTaskMemFree(json);
        output.clear();
        output.reserve(w.size());
        for (wchar_t c : w) output.push_back(static_cast<char>(c));
    }
    ::FreeLibrary(mod);
    return hr == 0 && !output.empty();
}

// Resolve the code-behind files for one XAML file and parse them.
// Pluggable extractors are tried first when configured; the built-in
// parsers are the fallback.
inline auto resolve_code_behind(std::filesystem::path const& xaml, std::string const& forced_lang,
                                std::string const& cs_extractor, std::string const& cpp_extractor)
    -> std::pair<lang::CodeBehind, std::string> {

    std::filesystem::path base = xaml;
    base.replace_extension("");
    auto parent = xaml.parent_path();

    // C# candidates: Foo.xaml.cs / Foo.cs
    for (auto const& cand : { parent / (base.filename().string() + ".cs"),
                              parent / (xaml.stem().string() + ".xaml.cs"),
                              std::filesystem::path(xaml.string() + ".cs") }) {
        if (file_exists(cand) && ext_of(cand) == ".cs") {
            if (!cs_extractor.empty()) {
                std::string json;
                if (run_extractor(cs_extractor, cand, json)) {
                    auto cb = lang::parse_meta_json(json);
                    if (cb.found && forced_lang != "cpp") return {std::move(cb), "csharp"};
                }
            }
            auto src = load_text_file(cand);
            // Pass the XAML file stem: the .cs may declare helper classes
            // before the view class, and the view's own class is the one
            // whose members the bindings must resolve against.
            auto cb = lang::parse_csharp(src, xaml.stem().string());
            if (forced_lang != "cpp") return {std::move(cb), "csharp"};
        }
    }

    // C++ candidates: Foo.idl + Foo.xaml.h + Foo.xaml.cpp
    {
        lang::CodeBehind merged;
        bool found = false;
        auto stem = xaml.stem().string();
        auto try_extractor = [&](std::filesystem::path const& cand) {
            if (cpp_extractor.empty()) return false;
            std::string json;
            if (!run_extractor(cpp_extractor, cand, json)) return false;
            auto cb = lang::parse_meta_json(json);
            if (!cb.found && cb.properties.empty() && cb.methods.empty()) return false;
            if (!found) {
                merged = std::move(cb);
            } else {
                merged.methods.insert(merged.methods.end(), cb.methods.begin(), cb.methods.end());
                merged.properties.insert(merged.properties.end(), cb.properties.begin(), cb.properties.end());
                for (auto const& [nm, bd] : cb.method_bodies)
                    if (merged.method_bodies.find(nm) == merged.method_bodies.end())
                        merged.method_bodies[nm] = bd;
            }
            found = true;
            return true;
        };
        for (auto const& suffix : { ".idl", ".xaml.idl" }) {
            auto cand = parent / (stem + suffix);
            if (file_exists(cand)) {
                try_extractor(cand);
                auto cb = lang::parse_idl(load_text_file(cand));
                if (cb.found) { merged = std::move(cb); found = true; }
            }
        }
        for (auto const& suffix : { ".xaml.h", ".h" }) {
            auto cand = parent / (stem + suffix);
            if (file_exists(cand)) {
                try_extractor(cand);
                auto cb = lang::parse_cpp_header(load_text_file(cand));
                if (cb.found) {
                    if (!found) {
                        merged = std::move(cb);
                    } else {
                        // Supplement extractor metadata with declarations it does not model.
                        for (auto const& method : cb.methods) {
                            if (std::find(merged.methods.begin(), merged.methods.end(), method) == merged.methods.end())
                                merged.methods.push_back(method);
                        }
                        for (auto const& property : cb.properties) {
                            auto existing = std::find_if(merged.properties.begin(), merged.properties.end(),
                                [&](auto const& member) { return member.name == property.name; });
                            if (existing == merged.properties.end())
                                merged.properties.push_back(property);
                            else if (property.is_field && !existing->is_field)
                                *existing = property;
                        }
                        for (auto const& [nm, bd] : cb.method_bodies)
                            if (merged.method_bodies.find(nm) == merged.method_bodies.end())
                                merged.method_bodies[nm] = bd;
                        if (cb.extends_content_into_titlebar) merged.extends_content_into_titlebar = true;
                        if (cb.window_title.empty()) cb.window_title = merged.window_title;
                        if (!cb.window_title.empty()) merged.window_title = cb.window_title;
                    }
                    found = true;
                }
            }
        }
        for (auto const& suffix : { ".xaml.cpp", ".cpp" }) {
            auto cand = parent / (stem + suffix);
            if (file_exists(cand)) {
                if (!try_extractor(cand)) {
                    auto cb = lang::parse_cpp_source(load_text_file(cand));
                    if (cb.found || !cb.methods.empty()) {
                        if (!found) merged = std::move(cb);
                        else {
                            merged.methods.insert(merged.methods.end(), cb.methods.begin(), cb.methods.end());
                            for (auto const& [nm, bd] : cb.method_bodies)
                                if (merged.method_bodies.find(nm) == merged.method_bodies.end())
                                    merged.method_bodies[nm] = bd;
                            if (cb.extends_content_into_titlebar) merged.extends_content_into_titlebar = true;
                            if (cb.window_title.empty()) cb.window_title = merged.window_title;
                            if (!cb.window_title.empty()) merged.window_title = cb.window_title;
                        }
                        found = true;
                    }
                }
            }
        }
        if (found && forced_lang != "cs") {
            return {std::move(merged), "cpp"};
        }
    }
    return {{}, "auto"};
}

// §4  Project scan & conversion

struct XamlJob {
    std::filesystem::path xaml_path;
    std::string            rel_path;      // relative to project root (for header comment)
    lang::CodeBehind       codebehind;
    std::string            language;      // "csharp" | "cpp"
};

struct JobResult {
    std::filesystem::path xaml;
    std::string out_path;        // relative output path
    std::string text;
    bool        success = false;
    bool        is_resource = false;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// Collect conversion jobs from an input path.
inline auto collect_jobs(std::filesystem::path const& input, CliOptions const& o,
                         std::vector<XamlJob>& jobs, std::vector<std::string>& notes) -> void {
    if (!file_exists(input)) {
        notes.push_back("path does not exist: " + input.string());
        return;
    }
    if (is_file(input)) {
        if (ext_of(input) == ".xaml") {
            XamlJob j;
            j.xaml_path = input;
            j.rel_path = input.filename().string();
            auto [cb, lang] = resolve_code_behind(input, o.language, o.cs_extractor, o.cpp_extractor);
            j.codebehind = std::move(cb);
            j.language = lang;
            jobs.push_back(std::move(j));
        } else {
            notes.push_back("ignoring non-XAML file: " + input.string());
        }
        return;
    }
    // directory — recursive scan
    std::vector<std::filesystem::path> xamls;
    collect_xaml_files(input, xamls);
    std::sort(xamls.begin(), xamls.end());
    for (auto const& x : xamls) {
        XamlJob j;
        j.xaml_path = x;
        j.rel_path = rel_under(input, x);
        auto [cb, lang] = resolve_code_behind(x, o.language, o.cs_extractor, o.cpp_extractor);
        j.codebehind = std::move(cb);
        j.language = lang;
        jobs.push_back(std::move(j));
    }
}

inline auto run_job(XamlJob const& j, CliOptions const& o,
                    std::vector<std::string> const& project_view_types,
                    std::vector<std::string> const& unprojected_source_types) -> JobResult {
    JobResult r;
    r.xaml = j.xaml_path;
    std::string rel = j.rel_path;
    if (rel.size() >= 5 && rel.ends_with(".xaml")) rel.resize(rel.size() - 5);
    r.out_path = rel + ".winui";

    auto src = load_text_file(j.xaml_path);
    if (src.empty()) { r.errors.push_back("cannot read file"); return r; }

    auto doc = xaml::parse(src);
    if (!doc.ok()) {
        for (auto const& e : doc.errors) r.errors.push_back(std::format("{}:{}", e.line, e.message));
        return r;
    }
    auto* root = doc.root();
    if (!root) { r.errors.push_back("no root element"); return r; }

    convert::ConvertOptions copts;
    copts.source_rel_path = j.rel_path;
    copts.language = j.language;
    copts.source_file_path = j.xaml_path.string();
    copts.project_view_types = project_view_types;
    copts.unprojected_source_types = unprojected_source_types;

    // Pure C#/C++ control classes: when the code-behind declares a control
    // base (Button, Control, ...) but the XAML root is NOT the class itself
    // (e.g. CopyButton.xaml is a ResourceDictionary), a second view for the
    // class must be emitted so the winmd declares the control with its real
    // base. convert() does the emission — it owns the element walker needed
    // to lower the control's ControlTemplate into the view body.
    auto const& cbd = j.codebehind;
    bool standard_view_base = (cbd.base_class == "Page" || cbd.base_class == "Window"
        || cbd.base_class == "Application" || cbd.base_class == "UserControl"
        || cbd.base_class == "ContentDialog" || cbd.base_class == "DataTemplateSelector"
        || cbd.base_class.empty());
    copts.emit_control_view = (cbd.found && !standard_view_base && !cbd.class_name.empty());

    auto res = convert::convert(doc, j.codebehind, copts);
    r.warnings = std::move(res.warnings);
    r.errors = std::move(res.errors);
    r.is_resource = res.is_resource_view;
    if (res.text.empty() && r.errors.empty()) r.errors.push_back("conversion produced no output");
    if (r.errors.empty()) {
        r.success = true;
        r.text = std::move(res.text);
        if (copts.emit_control_view && res.view_name != cbd.class_name) r.is_resource = false;
    }
    return r;
}

// §5  Main entry

export inline auto run(int argc, char** argv) -> int {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "-help" || std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
            print_help(argv[0]);
            return 0;
        }
    }
    auto opts = parse_args(argc, argv);
    if (!opts) return 1;

    // runtime control extensions (no recompile ever needed)
    // Every *.json next to the executable is tried as a control registry —
    // ANY file name works; files that do not parse as a registry (e.g.
    // unrelated JSON) are skipped silently. Every -control <file> overlay
    // passed on the command line is applied on top (last one wins).
    // (Reads use a plain block loop — iterator-based string construction
    // is unreliable under C++ modules instantiation in importers.)
    auto read_whole_file = [](std::string const& path) -> std::string {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        std::string s;
        char buf[65536];
        while (in.read(buf, sizeof buf) || in.gcount() > 0)
            s.append(buf, static_cast<std::size_t>(in.gcount()));
        return s;
    };
    auto load_json_file = [&](std::string const& path) -> bool {
        std::string json = read_whole_file(path);
        if (json.empty()) { error(std::format("cannot open control-registry file '{}'", path)); return false; }
        if (!ext::load_extensions_from_json(json)) {
            error(std::format("malformed control-registry file '{}' (see --help for the JSON schema)", path));
            return false;
        }
        std::println("  extensions: loaded '{}'", path);
        return true;
    };
    {
        auto exe_path = std::filesystem::path(argv[0]);
        std::wstring dir = exe_path.parent_path().wstring();
        std::wstring pattern = dir + L"\\*.json";
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring full = dir + L"\\" + fd.cFileName;
                std::string json = read_whole_file(wstr_to_str(full));
                if (json.empty()) continue;
                if (ext::load_extensions_from_json(json)) {
                    std::string narrow = wstr_to_str(full);
                    std::println("  extensions: auto-loaded '{}'", narrow);
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        for (auto const& cf : opts->control_files) {
            if (!load_json_file(cf)) return 1;
        }
    }

    // collect jobs
    std::vector<XamlJob> jobs;
    std::vector<std::string> notes;
    for (auto const& in : opts->inputs) collect_jobs(in, *opts, jobs, notes);
    if (jobs.empty()) {
        std::println(stderr, "error: no .xaml files found in the given inputs");
        for (auto const& n : notes) std::println(stderr, "  note: {}", n);
        return 1;
    }

    std::vector<std::string> project_view_types;
    std::set<std::string> project_view_keys;
    for (auto const& job : jobs) {
        if (!job.codebehind.found || job.codebehind.class_name.empty()) continue;
        std::string key = job.codebehind.class_name;
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (project_view_keys.insert(key).second)
            project_view_types.push_back(job.codebehind.class_name);
    }

    std::vector<std::filesystem::path> csharp_files;
    for (auto const& in : opts->inputs) {
        auto path = std::filesystem::path(in);
        if (is_file(path)) {
            if (ext_of(path) == ".cs") csharp_files.push_back(path);
        } else if (xamlconv::cli::is_directory(path)) {
            collect_csharp_files(path, csharp_files);
        }
    }
    std::set<std::string> unprojected_type_keys;
    std::vector<std::string> unprojected_source_types;
    for (auto const& file : csharp_files) {
        for (auto& type : lang::declared_type_names(load_text_file(file))) {
            std::string key = type;
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (project_view_keys.contains(key) || !unprojected_type_keys.insert(key).second)
                continue;
            unprojected_source_types.push_back(std::move(type));
        }
    }

    // output directory
    std::string out_dir = opts->out_dir;
    if (!opts->out_dir_set) {
        std::filesystem::path first = opts->inputs.front();
        if (is_file(first)) first = first.parent_path();
        out_dir = (first / "winui_src").string();
    }
    ensure_dir(out_dir);

    // convert (parallel, every available core)
    std::size_t nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 1;
    if (nthreads > jobs.size()) nthreads = jobs.size();
    std::mutex mtx;
    std::atomic<std::size_t> next{0};
    std::vector<JobResult> results(jobs.size());

    std::vector<std::jthread> workers;
    for (std::size_t t = 0; t < nthreads; ++t) {
        workers.emplace_back([&]() {
            while (true) {
                std::size_t idx = next.fetch_add(1);
                if (idx >= jobs.size()) break;
                auto r = run_job(jobs[idx], *opts, project_view_types, unprojected_source_types);
                std::lock_guard lk(mtx);
                results[idx] = std::move(r);
            }
        });
    }
    for (auto& w : workers) if (w.joinable()) w.join();

    // write outputs
    std::size_t written = 0;
    for (auto& r : results) {
        if (!r.success || r.out_path.empty()) continue;
        auto full = std::filesystem::path(out_dir) / r.out_path;
        ensure_dir(full.parent_path());
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(r.text.data(), static_cast<std::streamsize>(r.text.size()));
            ++written;
        }
    }

    // Copy only assets referenced in the XAML source files to the output
    std::size_t assets_copied = 0;
    {
        // Step 1: Extract referenced asset paths from all XAML source files
        std::set<std::string> referenced_assets;
        std::set<std::string> referenced_samples;
        for (auto const& j : jobs) {
            auto content = load_text_file(j.xaml_path);
            std::size_t pos = 0;
            while ((pos = content.find("SampleDefinition", pos)) != std::string::npos) {
                auto equals = content.find('=', pos + 16);
                if (equals == std::string::npos) break;
                auto quote = content.find_first_of("\"'", equals + 1);
                if (quote == std::string::npos) break;
                auto end = content.find(content[quote], quote + 1);
                if (end == std::string::npos) break;
                std::string sample = content.substr(quote + 1, end - quote - 1);
                for (auto& ch : sample) if (ch == '/') ch = '\\';
                bool unsafe = sample.empty() || sample.front() == '\\'
                    || (sample.size() > 1 && sample[1] == ':');
                for (std::size_t part = 0; !unsafe && part < sample.size();) {
                    auto end_part = sample.find('\\', part);
                    if (end_part == std::string::npos) end_part = sample.size();
                    if (sample.substr(part, end_part - part) == "..") unsafe = true;
                    part = end_part + 1;
                }
                if (!unsafe) referenced_samples.insert(sample);
                pos = end + 1;
            }
            pos = 0;
            while ((pos = content.find("Assets", pos)) != std::string::npos) {
                auto start = pos + 6;
                if (start >= content.size() ||
                    (content[start] != '/' && content[start] != '\\')) {
                    pos = start;
                    continue;
                }
                ++start;
                auto end = start;
                // Paths may contain spaces; stop at XAML/URI boundaries instead.
                while (end < content.size() && content[end] != '"' && content[end] != '\''
                       && content[end] != '<' && content[end] != '>' && content[end] != ')'
                       && content[end] != '\n' && content[end] != '\r' && content[end] != '\t')
                    ++end;
                if (end > start) {
                    std::string path = "Assets/" + content.substr(start, end - start);
                    for (auto& c : path)
                        if (c == '\\') c = '/';
                    referenced_assets.insert(std::move(path));
                }
                pos = end;
            }
        }
        std::size_t samples_copied = 0;
        for (auto const& sample : referenced_samples) {
            std::string relative = sample;
            for (auto& ch : relative) if (ch == '/') ch = '\\';
            bool copied = false;
            for (auto const& job : jobs) {
                auto dir = job.xaml_path.parent_path();
                for (int depth = 0; depth < 12 && !dir.empty(); ++depth) {
                    auto source_root = dir / "Samples";
                    auto source = source_root / std::filesystem::path(relative);
                    if (file_exists(source)) {
                        auto dest = std::filesystem::path(out_dir) / "Samples" / std::filesystem::path(relative);
                        std::error_code ec;
                        std::filesystem::create_directories(dest.parent_path(), ec);
                        if (!ec) std::filesystem::copy_file(source, dest,
                            std::filesystem::copy_options::overwrite_existing, ec);
                        if (ec) {
                            std::println(stderr, "warning: could not copy sample '{}' from '{}': {}",
                                relative, source.string(), ec.message());
                        } else {
                            ++samples_copied;
                            auto sample_text = load_text_file(source);
                            std::string section, line, buffer, xaml_sample;
                            auto flush_section = [&]() {
                                if (section == "xaml") xaml_sample = buffer;
                                buffer.clear();
                            };
                            std::istringstream sample_stream(sample_text);
                            while (std::getline(sample_stream, line)) {
                                if (!line.empty() && line.back() == '\r') line.pop_back();
                                if (line.rfind("--- ", 0) == 0) {
                                    flush_section();
                                    section = line.substr(4);
                                    for (auto& ch : section)
                                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                    while (!section.empty() && (section.back() == ' ' || section.back() == '\t'))
                                        section.pop_back();
                                } else {
                                    if (!buffer.empty()) buffer += '\n';
                                    buffer += line;
                                }
                            }
                            flush_section();
                            if (!xaml_sample.empty()) {
                                std::string wrapped_sample = "<Page xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">"
                                    + sanitize_sample_xaml(xaml_sample) + "</Page>";
                                auto parsed_sample = xaml::parse(wrapped_sample);
                                if (!parsed_sample.ok()) {
                                    std::println(stderr, "warning: XAML sample '{}' could not be translated to .winui",
                                        source.string());
                                    for (auto const& message : parsed_sample.errors)
                                        std::println(stderr, "        line {}: {}", message.line, message.message);
                                } else {
                                    lang::CodeBehind no_codebehind;
                                    convert::ConvertOptions sample_options;
                                    sample_options.source_file_path = source.string();
                                    auto translated = convert::convert(parsed_sample, no_codebehind, sample_options);
                                    if (translated.text.empty() || !translated.errors.empty()) {
                                        std::println(stderr, "warning: XAML sample '{}' conversion failed",
                                            source.string());
                                        for (auto const& message : translated.errors)
                                            std::println(stderr, "        {}", message);
                                    } else {
                                        auto sidecar = std::filesystem::path(out_dir) / "Samples" / "WinUI"
                                            / std::filesystem::path(relative);
                                        sidecar.replace_extension(".winui");
                                        std::filesystem::create_directories(sidecar.parent_path(), ec);
                                        std::ofstream sidecar_out(sidecar, std::ios::binary | std::ios::trunc);
                                        if (!ec && sidecar_out) {
                                            sidecar_out.write(translated.text.data(),
                                                static_cast<std::streamsize>(translated.text.size()));
                                        } else {
                                            std::println(stderr, "warning: could not write translated sample '{}'",
                                                sidecar.string());
                                        }
                                    }
                                }
                            }
                        }
                        copied = true;
                        break;
                    }
                    auto parent = dir.parent_path();
                    if (parent.empty() || parent == dir) break;
                    dir = std::move(parent);
                }
                if (copied) break;
            }
            if (!copied)
                std::println(stderr, "warning: referenced external sample '{}' was not found", relative);
        }
        if (samples_copied)
            std::println("  external samples copied: {}", samples_copied);

        // Step 2: Find source root(s) containing Assets/ (Win32, no std::filesystem iteration)
        std::vector<std::wstring> src_roots;
        auto add_src_root = [&](std::wstring const& rs) {
            for (auto const& s : src_roots) if (s == rs) return;
            src_roots.push_back(rs);
        };
        for (auto const& in_path : opts->inputs) {
            std::filesystem::path p(in_path);
            if (is_file(p)) p = p.parent_path();
            add_src_root(p.wstring());
            std::wstring dir = p.wstring();
            for (int d = 0; d < 10 && !dir.empty(); ++d) {
                // Check this directory for Assets/
                if (::GetFileAttributesW((dir + L"\\Assets").c_str()) != INVALID_FILE_ATTRIBUTES) {
                    add_src_root(dir);
                    break;
                }
                // Check children for Assets/ subdirectory (Win32 FindFirstFile)
                std::wstring pattern = dir + L"\\*";
                WIN32_FIND_DATAW fd;
                HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        if (fd.cFileName[0] == L'.') continue;
                        std::wstring child = dir + L"\\" + fd.cFileName;
                        if (::GetFileAttributesW((child + L"\\Assets").c_str()) != INVALID_FILE_ATTRIBUTES)
                            add_src_root(child);
                    } while (::FindNextFileW(h, &fd));
                    ::FindClose(h);
                }
                // Go up one level
                auto sep = dir.find_last_of(L"\\/");
                if (sep != std::wstring::npos) dir.resize(sep); else break;
            }
        }
        // Step 3: Copy only the referenced assets from source root(s) to output
        for (auto const& rs : src_roots) {
            std::filesystem::path src_root(rs);
            for (auto const& asset_rel : referenced_assets) {
                auto asset_file = src_root / asset_rel;
                if (!file_exists(asset_file)) continue;
                auto dest = std::filesystem::path(out_dir) / asset_rel;
                ensure_dir(dest.parent_path());
                std::error_code cec;
                std::filesystem::copy_file(asset_file, dest,
                    std::filesystem::copy_options::overwrite_existing, cec);
                if (!cec) ++assets_copied;
            }
        }
        // Copy LICENSE file to output directory if present
        for (auto const& rs : src_roots) {
            auto lic = std::filesystem::path(rs) / "LICENSE";
            if (file_exists(lic)) {
                std::error_code lec;
                std::filesystem::copy_file(lic, std::filesystem::path(out_dir) / "LICENSE",
                    std::filesystem::copy_options::overwrite_existing, lec);
                break;
            }
        }
    }

    // summary
    std::size_t failed = 0;
    for (auto const& r : results) if (!r.success) ++failed;

    std::println("");
    std::println("  xaml2winuic — {} XAML scanned, {} .winui emitted, {} assets copied, {} failed", jobs.size(), written, assets_copied, failed);
    for (auto const& r : results) {
        if (!r.warnings.empty()) {
            std::println("    ! {} — warnings", r.xaml.string());
            for (auto const& warning : r.warnings) std::println("        {}", warning);
        }
        if (!r.success) {
            std::println("    ✗ {} — FAILED", r.xaml.string());
            for (auto const& e : r.errors) std::println("        {}", e);
        }
    }
    return failed > 0 ? 1 : 0;
}

} // namespace xamlconv::cli
