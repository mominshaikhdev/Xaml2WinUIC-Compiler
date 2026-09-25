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
            auto cb = lang::parse_csharp(src);
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
                if (!try_extractor(cand)) {
                    auto cb = lang::parse_cpp_header(load_text_file(cand));
                    if (cb.found) {
                        if (!found) {
                            merged = std::move(cb);
                        } else {
                            // merge methods & properties from the header into the IDL result
                            merged.methods.insert(merged.methods.end(), cb.methods.begin(), cb.methods.end());
                            merged.properties.insert(merged.properties.end(), cb.properties.begin(), cb.properties.end());
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

inline auto run_job(XamlJob const& j, CliOptions const& o) -> JobResult {
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

    auto res = convert::convert(doc, j.codebehind, copts);
    r.warnings = std::move(res.warnings);
    r.errors = std::move(res.errors);
    r.is_resource = res.is_resource_view;
    if (res.text.empty() && r.errors.empty()) r.errors.push_back("conversion produced no output");
    if (r.errors.empty()) {
        r.success = true;
        r.text = std::move(res.text);

        // Pure C#/C++ control classes: when the code-behind declares a control
        // base (Button, ContentControl, ...) but the XAML root is NOT the class
        // itself (e.g. CopyButton.xaml is a ResourceDictionary), emit a second
        // view for the class so the winmd declares the control with its real
        // base. Consumers then resolve events like Click through the base.
        auto const& cbd = j.codebehind;
        bool standard_view_base = (cbd.base_class == "Page" || cbd.base_class == "Window"
            || cbd.base_class == "Application" || cbd.base_class == "UserControl"
            || cbd.base_class == "ContentDialog" || cbd.base_class == "DataTemplateSelector"
            || cbd.base_class.empty());
        if (cbd.found && !standard_view_base && !cbd.class_name.empty()
            && res.view_name != cbd.class_name) {
            std::string cls = cbd.class_name;
            if (cls.find(':') != std::string::npos) cls = cls.substr(cls.rfind(':') + 1);
            r.text += "\n";
            r.text += "export view " + cls + " : " + cbd.base_class + " {\n";
            // Types must come from the .winui vocabulary. Raw C# annotations
            // ("object?", "Button?", "TypedEventHandler<A, B>?", generics)
            // are illegal in .winui and made winuic fail with "expected
            // identifier, got '?'" — sanitize them.
            for (auto const& p : cbd.properties)
                r.text += "    @state var " + p.name + ": "
                        + convert::sanitize_winui_type(p.type) + ";\n";
            for (auto const& ev : cbd.events)
                r.text += "    callback " + ev + "();\n";
            r.text += "    body {\n    }\n}\n";
            r.is_resource = false;
        }
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
                auto r = run_job(jobs[idx], *opts);
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
        for (auto const& j : jobs) {
            auto content = load_text_file(j.xaml_path);
            // ms-appx:///Assets/...
            {
                std::string needle = "ms-appx:///Assets/";
                std::size_t pos = 0;
                while ((pos = content.find(needle, pos)) != std::string::npos) {
                    std::size_t start = pos + needle.size();
                    std::size_t end = start;
                    // Asset names may contain spaces ("my photo.png"), so
                    // only structural delimiters terminate the path — the
                    // closing quote in quoted attribute values, XML markup
                    // characters and line breaks.
                    while (end < content.size() && content[end] != '"' && content[end] != '>'
                           && content[end] != ')' && content[end] != '\n'
                           && content[end] != '\r' && content[end] != '\t')
                        ++end;
                    if (end > start)
                        referenced_assets.insert("Assets/" + content.substr(start, end - start));
                    pos = end;
                }
            }
            // "Assets/..." or "Assets\\..." quoted relative paths
            {
                std::string needle = "\"Assets/";
                std::size_t pos = 0;
                while ((pos = content.find(needle, pos)) != std::string::npos) {
                    std::size_t start = pos + needle.size();
                    std::size_t end = start;
                    while (end < content.size() && content[end] != '"' && content[end] != '\r'
                           && content[end] != '\n')
                        ++end;
                    if (end > start) {
                        std::string path = content.substr(start, end - start);
                        auto dot = path.rfind('.');
                        if (dot != std::string::npos && dot + 1 < path.size())
                            referenced_assets.insert(path);
                    }
                    pos = end;
                }
            }
        }
        // Step 2: Find source root(s) containing Assets/ (Win32, no std::filesystem iteration)
        std::vector<std::wstring> src_roots;
        auto add_src_root = [&](std::wstring const& rs) {
            for (auto const& s : src_roots) if (s == rs) return;
            src_roots.push_back(rs);
        };
        for (auto const& in_path : opts->inputs) {
            std::filesystem::path p(in_path);
            if (is_file(p)) p = p.parent_path();
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
    }

    // summary
    std::size_t failed = 0;
    for (auto const& r : results) if (!r.success) ++failed;

    std::println("");
    std::println("  xaml2winuic — {} XAML scanned, {} .winui emitted, {} assets copied, {} failed", jobs.size(), written, assets_copied, failed);
    for (auto const& r : results) {
        if (!r.success) {
            std::println("    ✗ {} — FAILED", r.xaml.string());
            for (auto const& e : r.errors) std::println("        {}", e);
        }
    }
    return failed > 0 ? 1 : 0;
}

} // namespace xamlconv::cli
