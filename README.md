# Xaml2WinUIC — WinUI 3 XAML → `.winui` Compiler

A native C++23 converter that scans a project's **WinUI 3 / Windows App SDK XAML** together with its **C# / C++/WinRT / C++/CX code-behind** and emits a `.winui` source file for every XAML file — a 100% equivalent, projection-agnostic projection that preserves the exact layout, colors, styles, resources, bindings and event wiring of the original markup.

The emitted `.winui` files are compiled by **[`winuic-compiler`](https://github.com/mominshaikhdev/winuic-compiler)** into a language-agnostic ECMA-335 `.winmd` — no XAML, no IDL, no manual UI code, no partial-class boilerplate.

---

## Table of Contents

- [Xaml2WinUIC — WinUI 3 XAML → `.winui` Compiler](#xaml2winuic--winui-3-xaml--winui-compiler)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
  - [Pipeline](#pipeline)
  - [Features](#features)
  - [CLI Reference](#cli-reference)
    - [Examples](#examples)
  - [Code-Behind Language Modes](#code-behind-language-modes)
    - [`-l auto` (default)](#-l-auto-default)
    - [C++/CX](#ccx)
    - [Semantics](#semantics)
  - [Verbatim XAML Fidelity](#verbatim-xaml-fidelity)
  - [Third-Party \& Custom Control Registration](#third-party--custom-control-registration)
      - [Comprehensive multi-control registry example](#comprehensive-multi-control-registry-example)
  - [Output Format](#output-format)
  - [Examples](#examples-1)
    - [WinUI 3 Gallery (C#) — 190 files, 0 failures](#winui-3-gallery-c--190-files-0-failures)
    - [C++/WinRT project](#cwinrt-project)
  - [Pipeline Verification](#pipeline-verification)
    - [One-shot build with `-winui`](#one-shot-build-with--winui)
    - [One-shot build with `-winui`](#one-shot-build-with--winui-1)
  - [Reference-Only Asset Copying](#reference-only-asset-copying)
  - [Building from Source](#building-from-source)
  - [Related](#related)

---

## Overview

`Xaml2WinUIC` converts a WinUI 3 project **in place**: it reads each `.xaml` file, resolves its code-behind (`Foo.xaml.cs` / `Foo.cs` for C#, or `Foo.idl` + `Foo.xaml.h` + `Foo.xaml.cpp` for C++), and emits `<Name>.winui` mirroring the original directory structure. Nothing is lost: unmapped attributes, styles, templates, resource dictionaries, converters, xmlns declarations and CData are carried verbatim.

```
  [Foo.xaml] ──┐
               ├──▶ Xaml2WinUIC ──▶ [Foo.winui] ──▶ winuic ──▶ [Foo.winmd]
  [Foo.xaml.cs]┘      │                              ▲
                      │                              │
               third-party control                   │
               registry JSON (*.json) ───────────────┘
```

The converter is the front half of the pipeline; `winuic` is the grammar/`winmd` reference: **["WinUI Language Specification & A-Z Grammar Reference"](https://github.com/mominshaikhdev/winuic-compiler)**.

---

## Pipeline

1. **Scan** — recursive walk of the input project directory (or an explicit file list).
2. **Parse XAML** — XML parsing with xmlns tracking, element/attribute preservation and CData support.
3. **Resolve code-behind** — per file, in the requested language dialect (see [Code-Behind Language Modes](#code-behind-language-modes)).
4. **Convert** — the XAML tree is lowered to `.winui`: control names, named/labeled constructor arguments, universal modifiers, event handlers as `.onX { self.Handler(); }` closures, bindings and resource references, verbatim raw pairs and fragment families.
5. **Emit** — a `.winui` file per XAML file, mirroring the source tree layout.

---

## Features

- **100% equivalent projections** — every XAML attribute and node survives into the `.winui` (and therefore the `.winmd`), so design and behavior are reproduced exactly by the projection.
- **Dual-language code-behind** — full C# (`partial class`, `x:Bind` targets, event handlers) and C++ (IDL, headers, C++/WinRT, **C++/CX `ref class` / `property` / `^`**) support. `-l auto|cs|cpp` selects the dialect; auto mode prefers C# when a `.cs` exists and falls back to C++.
- **ExtendContentIntoTitleBar detection** — automatically detected from all code-behind languages (C# `this.ExtendsContentIntoTitleBar = true`, C++ `this->ExtendsContentIntoTitleBar = true`, C++/CX `this->ExtendsContentIntoTitleBar = true`, IDL) and emitted as `.extendContentIntoTitleBar;` directive.
- **Event wiring** — XAML `Click="OnClick"` becomes `.onClick { self.OnClick(); }` with the handler emitted as a `callback` declaration; typed parameters on the handler are preserved.
- **State & bindings** — `x:Class` members referenced by `x:Bind` become `@state`/`@binding` variables; `Mode=TwoWay` is detected and preserved (16 in the WinUI-Gallery conversion); `{Binding ElementName=X, Path=P}` becomes `x.p` member references; `{StaticResource}`/`{ThemeResource}` become resource-key expressions.
- **Typed state vars** — code-behind types are normalized into the .winui type vocabulary and carried through the winmd: `ObservableCollection<string>`/`List<string>` → `string[]`, `DateTime` → `datetime`, primitives as-is. Custom-class member paths (`$viewModel.title`) flatten into flat state vars (`viewModelTitle`), keeping the winuic philosophy of flat, typed state. `CollectionViewSource` element bindings resolve to the element's `View` getter.
- **XAML code examples are converted too** — embedded sample markup (`ControlExample.Xaml` / any `X.Yaml` property holding `<x:String>` XAML source) is recursively parsed and lowered into real `.winui` layout (events, bindings and comments included), not just carried verbatim — the sample UI compiles for real. When the control also carries a live `<X.Example>` tree, the string stays display-only and no duplicate UI is emitted. The verbatim source is still kept as a `.raw("Xaml", …)` pair, so nothing is lost.
- **Verbatim pass-through** — unmapped attributes become `.raw("key", "value")`; fragment families (styles, templates, row/column definitions, resource dictionaries, visual states, animations, brushes, …) are serialized back to XAML text byte-for-byte inside `Raw() .raw("Resource", …)` views.
- **Third-party controls** — any control works out of the box (unknown controls map to their XAML name and take raw attributes); explicit property/event/rename metadata comes from JSON registries with **any file name** — no hardcoded `winui.json`, no recompilation.
- **Batch conversion** — directories, explicit file lists, or comma-separated CSV like `winuic`'s `-files`; 190/190 WinUI-Gallery XAML files convert cleanly (0 failures, 0 warnings downstream in `winuic`).
- **Asset copying** — referenced assets (logos, icons, images) are automatically copied from the source project to the output directory, preserving relative paths.
- **No hardcoded paths** — all system paths use environment variables (`%ProgramFiles%`, `%ProgramFiles(x86)%`); the CLI works on any Windows machine.

---

## CLI Reference

```
xaml2winuic.exe <project-dir|file> [more inputs...] [options]
xaml2winuic.exe -f file1.xaml,file2.xaml [options]
```

| Flag | Long Flag | Description | Default |
| :--- | :--- | :--- | :--- |
| *positional* | — | One or more input directories or `.xaml` files (recursive scan for directories); commas are also accepted inside the list | — |
| `-f` | `-files <csv>` | Comma-separated list of `.xaml` files to convert | `<positional args>` |
| `-o` | `-out_dir <dir>` | Output directory for the emitted `.winui` files | `<input>/winui_src` |
| `-l` | `-language <mode>` | Code-behind dialect: `auto`, `cs`, `cpp` | `auto` |
| `-c` | `-control <file>` | Control-registry JSON (**any name**, repeatable, no recompile); every `*.json` next to `xaml2winuic.exe` is auto-loaded as a control registry too | `*.json` next to the exe |
| `-h` | `-help` | Show usage and flags summary | — |

### Examples

```cmd
:: Convert an entire project (C# auto-detected)
xaml2winuic.exe WinUI-Gallery

:: Convert a C++/WinRT project explicitly
xaml2winuic.exe Cpp-WinUI3-MetaScrub-Unpackaged -l cpp

:: Only two files, custom output dir, custom control registry
xaml2winuic.exe -f MainWindow.xaml,App.xaml -o out -c MyControls.json

:: Multiple inputs with commas, like winuic's -files
xaml2winuic.exe "MainWindow.xaml, ColorSelector.xaml, App.xaml"
```

---

## Code-Behind Language Modes

Per XAML file, `Xaml2WinUIC` resolves exactly **one** code-behind language — a XAML file has a single real code-behind in a single language.

### `-l auto` (default)
1. **C# wins if it exists**: `Foo.cs` → `Foo.xaml.cs` → `Foo.xaml` are probed. Any found is parsed (native extractor if configured, otherwise the built-in C# tokenizer/parser) and the file is emitted as `// Language: csharp`.
2. **C++ fallback** (when no C# exists, or `-l cpp`): candidates are merged into a single code-behind:
   - `Foo.idl` / `Foo.xaml.idl` — `runtimeclass` members with exact property types;
   - `Foo.xaml.h` / `Foo.h` — declared methods and properties (C++/WinRT headers);
   - `Foo.xaml.cpp` / `Foo.cpp` — implemented methods, event handler signatures.

### C++/CX
C++/CX sources (`public ref class`, `property Platform::String^ Title { get; set; }`, `^` handles, `sealed`/`final` modifiers) are auto-detected and handled by a dedicated walker.

### Semantics
- Handlers referenced by XAML events (`Click="OnClick"`) are emitted as `callback OnClick();` declarations and invoked from the generated `.onClick { self.OnClick(); }` closure.
- Code-behind members referenced by `x:Bind` targets are emitted as typed state/binding variables.
- Fields and methods unrelated to the UI are not projected (they belong to the code-behind, not the view metadata).

---

## Verbatim XAML Fidelity

Every construct that has no abstract `.winui` equivalent is passed through verbatim instead of being dropped or approximated — the compiler's rule is: **nothing is ever lost, anything unfaithful is a hard error**.

| XAML construct | `.winui` representation |
| :--- | :--- |
| Unmapped attribute (e.g. `AutomationProperties.AutomationId="__GoBack"`) | `.raw("AutomationProperties.AutomationId", "__GoBack")` |
| `x:Name`, `Grid.Row`, `Canvas.ZIndex`, … | `.raw("x:Name", "RootGrid")` |
| Fragment-family subtrees (`Style`, `RowDefinition`, `DataTemplate`, `ResourceDictionary`, `VisualStateManager.VisualStateGroups`, `Storyboard`, brushes, `x:String`, …) | serialized back to XAML text, carried as `Raw() .raw("Resource", "<…>")` |
| `xmlns` declarations, element `prefix` (non-`x`/`d`/`mc`) | `.raw("xmlns", …)` / raw prefix pairs |
| CData / comment content | preserved verbatim inside the raw payload |
| `Style="{StaticResource Key}"` / `ThemeResource` | resource-key expressions (typed, not raw) |

---

## Third-Party & Custom Control Registration

Unknown controls never block conversion: any control not in the built-in catalog is emitted under its XAML name, takes raw attributes and children, and is validated dynamically at `.winmd` compile time.

> [!IMPORTANT]
> **SPECIAL NOTE — THIRD-PARTY CONTROLS NEVER REQUIRE A RECOMPILE**
> - **Any JSON file name works.** There is no hardcoded `winui.json`. Every `*.json` next to `xaml2winuic.exe` is auto-loaded on every run (non-registry JSON is skipped silently), and `-c/-control <file>` adds per-project registries on top — the later file wins on duplicate names.
> - **One shared schema serves both compilers.** The exact same registry JSON is consumed by `Xaml2WinUIC` (conversion side) and `winuic` (`.winmd` side), so registering a control once makes it fully typed end-to-end: named args, typed properties, `.onX { }` event closures, universal modifiers and fragment families.
> - **Future controls still convert cleanly.** A control that ships tomorrow — not yet in the registry — is emitted under its XAML name with `.raw()` passthrough and verbatim fragments, so the conversion never breaks and the `.winmd` still carries the full XAML surface.
> - **Zero recompilation, zero code changes.** Adding or updating registries is purely data: drop a JSON file next to the exe (or pass `-control`) and rerun.

For exact property/event typing and abstract-name renames, register the control in a JSON registry — **any file name** works, and every `*.json` next to `xaml2winuic.exe` is auto-loaded (non-registry JSON files are skipped silently). This is the **same registry format** consumed by `winuic`, so one file serves both compilers; per-project registries passed via `-control` overlay the auto-loaded ones.

```json
{
  "backend": "winui3",
  "controls": [
    {
      "abstract_name": "RadialGauge",
      "platform_type": "RadialGauge",
      "platform_namespace": "MyLib.Controls",
      "category": "third-party",
      "accepts_children": false,
      "accepts_text_content": false,
      "properties": [
        { "abstract_name": "Minimum", "platform_setter": "Minimum", "transform": "double" },
        { "abstract_name": "Value",   "platform_setter": "Value",   "transform": "double" }
      ],
      "events": [
        { "abstract_name": "ValueChanged", "platform_event": "ValueChanged" }
      ],
      "xaml": {
        "properties": { "Minimum": "min", "Value": "value" },
        "events":     { "ValueChanged": "onValueChanged" }
      }
    }
  ],
  "xamlEvents":      ["Tapped"],
  "universalMods":   [{ "xaml": "IsSpinning", "winui": "spin" }],
  "fragFamilies":    ["GaugePalette"]
}
```

Conversion is then fully typed: `RadialGauge(min: 0, max: 100, value: 42).onValueChanged { … }.onTapped { … }`.

#### Comprehensive multi-control registry example

Everything the schema can express in one file — typed properties (with transforms), events, per-control `xaml` renames, global `xamlEvents`, `universalMods` and `fragFamilies`:

```json
{
  "backend": "winui3",
  "controls": [
    {
      "abstract_name": "AreaChart",
      "platform_type": "AreaChart",
      "platform_namespace": "Contoso.Visualization",
      "category": "custom",
      "accepts_children": false,
      "accepts_text_content": false,
      "properties": [
        { "abstract_name": "dataSource", "platform_setter": "DataSource", "transform": "box" },
        { "abstract_name": "seriesCount", "platform_setter": "SeriesCount", "transform": "int" },
        { "abstract_name": "lineWidth",   "platform_setter": "LineWidth",   "transform": "double" },
        { "abstract_name": "accent",      "platform_setter": "AccentBrush", "transform": "brush" },
        { "abstract_name": "padding",     "platform_setter": "Padding",     "transform": "thickness" }
      ],
      "events": [
        { "abstract_name": "onPointSelected", "platform_event": "PointSelected" }
      ],
      "xaml": {
        "properties": {
          "DataSource":  "data",
          "SeriesCount": "series",
          "LineWidth":   "width",
          "AccentBrush": "accent",
          "Padding":     "padding"
        },
        "events": { "PointSelected": "onPoint" }
      }
    },
    {
      "abstract_name": "RadialGauge",
      "platform_type": "RadialGauge",
      "platform_namespace": "MyLib.Controls",
      "category": "third-party",
      "accepts_children": false,
      "accepts_text_content": false,
      "properties": [
        { "abstract_name": "minimum",     "platform_setter": "Minimum", "transform": "double" },
        { "abstract_name": "maximum",     "platform_setter": "Maximum", "transform": "double" },
        { "abstract_name": "value",       "platform_setter": "Value",   "transform": "double" },
        { "abstract_name": "tickSpacing", "platform_setter": "TickSpacing", "transform": "double" },
        { "abstract_name": "unit",        "platform_setter": "Unit",    "transform": "enum" },
        { "abstract_name": "isAnimated",  "platform_setter": "IsAnimated", "transform": "nullable_bool" }
      ],
      "events": [
        { "abstract_name": "onValueChanged", "platform_event": "ValueChanged" }
      ],
      "xaml": {
        "properties": {
          "Minimum":    "min",
          "Maximum":    "max",
          "Value":      "value",
          "TickSpacing": "tickSpacing",
          "Unit":       "unit",
          "IsAnimated": "isAnimated"
        },
        "events": { "ValueChanged": "onValueChanged" }
      }
    },
    {
      "abstract_name": "LedStrip",
      "platform_type": "LedStrip",
      "platform_namespace": "Fabrikam.Hardware",
      "category": "custom",
      "accepts_children": false,
      "accepts_text_content": true,
      "properties": [
        { "abstract_name": "ledCount", "platform_setter": "LedCount", "transform": "int" },
        { "abstract_name": "glow",     "platform_setter": "GlowBrush", "transform": "brush" },
        { "abstract_name": "isLit",    "platform_setter": "IsLit",     "transform": "nullable_bool" }
      ],
      "events": [
        { "abstract_name": "onTapped", "platform_event": "Tapped" }
      ],
      "xaml": {
        "properties": { "LedCount": "count", "GlowBrush": "glow", "IsLit": "lit" },
        "events":     { "Tapped": "onTapped" }
      }
    }
  ],
  "xamlEvents":    [ "PointerEntered", "DoubleTapped" ],
  "universalMods": [
    { "xaml": "IsSpinning", "winui": "spin" },
    { "xaml": "CornerRadius", "winui": "cornerRadius" }
  ],
  "fragFamilies":  [ "GaugePalette", "ChartTheme" ]
}
```

Round trip — this XAML:

```xml
<AreaChart x:Name="MainChart" DataSource="q1" SeriesCount="3" LineWidth="2.5"
           AccentBrush="#1E90FF" Padding="4,0,4,0" PointSelected="OnPoint"
           PointerEntered="OnHover"/>
<RadialGauge Minimum="0" Maximum="100" Value="42" TickSpacing="10"
             Unit="percent" IsAnimated="True" ValueChanged="OnGaugeChanged"
             IsSpinning="True"/>
<LedStrip x:Name="TopBanner">Top banner</LedStrip>
```

converts to fully typed `.winui` (raw passthrough for anything still unmapped):

```winui
AreaChart(data: "q1", series: 3, width: 2.5, accent: "#1E90FF")
    .onPoint { self.OnPoint(); }
    .onPointerEntered { self.OnHover(); }
    .raw("x:Name", "MainChart")
    .raw("Padding", "4,0,4,0")  // non-uniform Thickness: verbatim passthrough

RadialGauge(min: 0, max: 100, value: 42, tickSpacing: 10, unit: "percent", isAnimated: true)
    .onValueChanged { self.OnGaugeChanged(); }
    .spin(true)
    .raw("x:Name", "RadialGauge")

LedStrip(text: "Top banner")    // accepts_text_content: text becomes a text: arg
    .raw("x:Name", "TopBanner") // no xaml map entry: carried verbatim
```

Every key does something concrete: `xaml.properties` renames the attribute, `xaml.events` wires the handler to an `.onX { self.Handler(); }` closure, `xamlEvents`/`universalMods` apply globally to any control, `fragFamilies` keeps the named subtree as a verbatim `Raw()` fragment, and anything not mapped falls back to `.raw("key", "value")` so nothing is ever lost.

---

## Output Format

Every `.winui` file opens with a `#version` directive and a provenance header, then declares the view (or resource view) with the same name as its `x:Class`:

```winui
#version 1

// Generated by Xaml2WinUIC
// Source : WinUIGallery/MainWindow.xaml
// Language: csharp
// 100% equivalent .winui projection of the original WinUI 3 XAML.

export view MainWindow {
    @state var rootFrame: string;
    callback RootGrid_Loaded();
    callback TitleBar_BackRequested();

    body {
        MicaBackdrop
        Grid.onPointerPressed { self.RootGrid_PointerPressed(); }
             .raw("x:Name", "RootGrid")
             .raw("RowDefinitions", "<RowDefinition Height=\"Auto\"/><RowDefinition Height=\"*\"/>")
        {
            TitleBar.onBackRequested { self.TitleBar_BackRequested(); }
                    .raw("Title", "WinUI 3 Gallery")
            {
                AutoSuggestBox(placeholder: "Search…").onQuerySubmitted { self.OnControlsSearchBoxQuerySubmitted(); }
            }
        }
    }
}
```

Resource dictionaries (e.g. `App.xaml`) are emitted as `<Class>Resources` views whose entire payload is a verbatim `Raw()` fragment — merged dictionaries, theme dictionaries, converters and `x:String` entries all survive exactly.

---

## Examples

### WinUI 3 Gallery (C#) — 190 files, 0 failures
```cmd
xaml2winuic.exe WinUI-Gallery
```
→ 190 `.winui` files in `winui_src_test`, then:

```cmd
winuic.exe -f <all files> -o winui_src_compile_test -n WinUIGallery
```
→ **Succeeded 190 / Failed 0 / warnings 0**, and the emitted `.winmd` carries 2966 verbatim raw pairs, 98 state/binding variables, 661 event closures and 16 two-way bindings.

### C++/WinRT project
```cmd
xaml2winuic.exe Cpp-WinUI3-MetaScrub-Unpackaged -l cpp
```
→ IDL + `.xaml.h` + `.xaml.cpp` merged per view; handlers wired as `.onX { self.Handler(); }`.

---

## Pipeline Verification

The full `Xaml2WinUIC` → `winuic` → `cppwinrtc` pipeline has been verified against two real-world WinUI 3 apps:

| App | XAML Files | `.winui` Emitted | Assets Copied | C++23 Modules | Failures |
|---|---|---|---|---|---|
| **MetaScrub** | 7 | 7 | 0 | 31 | 0 |
| **WinUI Gallery** | 190 | 190 | 69 | 185 | 0 |

All 190 WinUI Gallery XAML files convert cleanly — 0 failures, 0 warnings downstream in `winuic`.
The emitted `.winui` files pass through `winuic` → `.winmd` → `cppwinrtc -winui` → 31 C++23 named modules (7 view modules + 22 WinRT projection modules + component + main + winrt_base, including the auto-split `Microsoft.UI.Xaml` monolith) all compiled successfully.

### One-shot build with `-winui`

```bat
:: Full pipeline: xaml → winui → winmd → C++23 modules (all compiled)
xaml2winuic.exe MetaScrub -l cpp
winuic.exe -f *.winui -o winuic_out -n MetaScrub
cppwinrtc.exe -input winuic_out/MetaScrub.winmd -winui -output .
```

### One-shot build with `-winui`

```bat
:: Full pipeline: xaml → winui → winmd → C++23 modules (all compiled)
xaml2winuic.exe MetaScrub -l cpp
winuic.exe -f *.winui -o winuic_out -n MetaScrub
cppwinrtc.exe -input winuic_out/MetaScrub.winmd -winui -base -component -output cppwinrtc_full/winrt_projection
```

3rd-party XAML controls (SettingsCard, Shimmer, WrapPanel, etc.) are preserved through the pipeline — the `winui.json` registry maps them to platform types, and the catalog in the `.winmd` carries `type`, `template_resource_key`, and `resource_dictionary` for downstream projections.

---

## Reference-Only Asset Copying

`Xaml2WinUIC` scans every XAML source file for asset references (`ms-appx:///Assets/...`,
`"Assets/..."` quoted paths) and copies **only the files actually used** to the output
directory. It walks up from each input file and checks sibling directories for `Assets/`
folders using Win32 APIs, so it works even when the `.winui` output is in a different
tree from the original source project.

```
Xaml2WinUIC --- XAML scanned: 188, .winui emitted: 188, assets copied: 23, failed: 0
```

No dead assets, no wasted disk space — only the images, icons and resources that the
XAML files actually reference are copied.

## Building from Source

Requirements: MSVC (C++23 modules), Windows App SDK headers for the control registry.

```cmd
cd Xaml2WinUIC
cmd /c build.bat
```

`build.bat` compiles the module graph (`registry.ixx`, `xaml.ixx`, `lang.ixx`, `convert.ixx`, `extensions.ixx`, `cli.ixx`, …) into `xaml2winuic.exe` next to the sources. The JSON extension loader (`extensions.ixx`) is linked in as `extensions.obj`.

---

## Related

- [`winuic-compiler` — .winui compiler & A-Z grammar reference](https://github.com/mominshaikhdev/winuic-compiler) (the authoritative DSL specification)
- [`cppwinc-compiler`](https://github.com/mominshaikhdev/cppwinc-compiler) — C++/WinRT projection that consumes the emitted `.winmd`
- [WinUI 3 Gallery](https://github.com/microsoft/WinUI-Gallery) — the reference corpus used for the 190-file regression suite
