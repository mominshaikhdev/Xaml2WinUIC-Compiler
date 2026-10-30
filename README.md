# Xaml2WinUIC — WinUI 3 XAML → `.winui` Compiler

A native C++23 converter that scans a project's **WinUI 3 / Windows App SDK XAML** together with its **C# / CppWinC / C++/CX code-behind** and emits a `.winui` source file for every XAML file — a 100% equivalent, projection-agnostic projection that preserves the exact layout, colors, styles, resources, bindings and event wiring of the original markup.

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
  - [Future-Proof & 3rd-Party Agnostic Architecture](#future-proof--3rd-party-agnostic-architecture)
  - [Third-Party \& Custom Control Registration](#third-party--custom-control-registration)
      - [Comprehensive multi-control registry example](#comprehensive-multi-control-registry-example)
  - [Registry event routing (`xamlEvents`)](#registry-event-routing-xamlevents)
  - [Output Format](#output-format)
  - [Reference-Only Asset \& LICENSE Copying](#reference-only-asset--license-copying)
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

The `.winui` files are plain text in the project's own source language;
`winuic` compiles them into a language-projection-agnostic `.winmd`, so any
downstream projection (C++, C#, windows-rs, Python, Go) consumes the same metadata
with no language baked in.

---

## Pipeline

1. **Scan** — recursive walk of the input project directory (or an explicit file list).
2. **Parse XAML** — XML parsing with xmlns tracking, element/attribute preservation, whitespace resilience, and CData support.
3. **Resolve code-behind** — per file, in the requested language dialect (see [Code-Behind Language Modes](#code-behind-language-modes)).
4. **Convert** — the XAML tree is lowered to `.winui`: control names, named/labeled constructor arguments, universal modifiers, event handlers as `.onX { self.Handler(); }` closures, bindings and resource references, verbatim raw pairs, property elements, and fragment families.
5. **Emit** — a `.winui` file per XAML file, mirroring the source tree layout.

---

## Features

- **100% equivalent projections** — every XAML attribute and node survives into the `.winui` (and therefore the `.winmd`), so design and behavior are reproduced exactly by the projection.
- **Dual-language code-behind** — full C# (`partial class`, `x:Bind` targets, event handlers) and C++ (IDL, headers, CppWinC, **C++/CX `ref class` / `property` / `^`**) support. `-l auto|cs|cpp` selects the dialect; auto mode prefers C# when a `.cs` exists and falls back to C++.
- **ExtendContentIntoTitleBar detection** — automatically detected from all code-behind languages (C# `this.ExtendsContentIntoTitleBar = true`, C++ `this->ExtendsContentIntoTitleBar = true`, C++/CX `this->ExtendsContentIntoTitleBar = true`, IDL) and emitted as `.extendContentIntoTitleBar;` directive.
- **Event wiring** — XAML `Click="OnClick"` becomes `.onClick { self.OnClick(); }` with the handler emitted as a `callback` declaration; typed parameters on the handler are preserved.
- **State & bindings** — `x:Class` members referenced by `x:Bind` become `@state`/`@binding` variables; `Mode=TwoWay` is detected and preserved (16 in the WinUI-Gallery conversion); `{Binding ElementName=X, Path=P}` becomes `x.p` member references; `{StaticResource}`/`{ThemeResource}` become resource-key expressions.
- **Typed state vars** — code-behind types are normalized into the .winui type vocabulary and carried through the winmd: `ObservableCollection<string>`/`List<string>` → `string[]`, `DateTime` → `datetime`, primitives as-is. Custom-class member paths (`$viewModel.title`) flatten into flat state vars (`viewModelTitle`), keeping the winuic philosophy of flat, typed state. `CollectionViewSource` element bindings resolve to the element's `View` getter.
- **XAML code examples are converted too** — embedded sample markup (`ControlExample.Xaml` / any `X.Yaml` property holding `<x:String>` XAML source) is recursively parsed and lowered into real `.winui` layout (events, bindings and comments included), not just carried verbatim — the sample UI compiles for real. When the control also carries a live `<X.Example>` tree, the string stays display-only and no duplicate UI is emitted. The verbatim source is still kept as a `.raw("Xaml", …)` pair, so nothing is lost.
- **Verbatim pass-through** — unmapped attributes become `.raw("key", "value")`; fragment families (styles, templates, row/column definitions, resource dictionaries, visual states, animations, brushes, …) are serialized back to XAML text byte-for-byte inside `Raw() .raw("Resource", …)` views.
- **100% Future-Proof Property Elements** — property elements (`<Button.Flyout>`, `<SplitView.Pane>`, `<controls:SettingsCard.HeaderIcon>`, `<custom:ThirdParty.Prop>`) are preserved with their exact property associations intact, ensuring downstream compilers invoke the proper property setters without flattening or losing non-visual nodes.
- **Third-party controls** — any control works out of the box (unknown controls map to their XAML name and take raw attributes); explicit property/event/rename metadata comes from JSON registries with **any file name** — no hardcoded `winui.json`, no recompilation.
- **Batch conversion** — directories, explicit file lists, or comma-separated CSV like `winuic`'s `-files`; 190/190 WinUI-Gallery XAML files convert cleanly (0 failures, 0 warnings downstream in `winuic`).
- **Asset copying** — referenced assets (logos, icons, images) are automatically copied from the source project to the output directory, preserving relative paths.
- **No hardcoded paths** — all system paths use environment variables (`%ProgramFiles%`, `%ProgramFiles(x86)%`); the CLI works on any Windows machine.

---

## CLI Reference

```cmd
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
:: Convert an entire WinUI 3 project in-place
xaml2winuic.exe C:\Projects\MyWinUIApp

:: Convert with explicit C# code-behind and custom output folder
xaml2winuic.exe C:\Projects\MyWinUIApp -l cs -o C:\Projects\MyWinUIApp\winui_src

:: Convert with a third-party control registry
xaml2winuic.exe C:\Projects\MyWinUIApp -c C:\Registries\toolkit.json
```

---

## Code-Behind Language Modes

### `-l auto` (default)
1. **C# wins if it exists**: `Foo.cs` → `Foo.xaml.cs` → `Foo.xaml` are probed. Any found is parsed (native extractor if configured, otherwise the built-in C# tokenizer/parser) and the file is emitted as `// Language: csharp`.
2. **C++ fallback** (when no C# exists, or `-l cpp`): candidates are merged into a single code-behind:
   - `Foo.idl` / `Foo.xaml.idl` — `runtimeclass` members with exact property types;
   - `Foo.xaml.h` / `Foo.h` — declared methods and properties (CppWinC headers);
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
| Property Elements (`<Button.Flyout>`, `<SplitView.Pane>`, `<controls:SettingsCard.HeaderIcon>`, `<custom:ThirdParty.Prop>`) | `.raw("Flyout", "<MenuFlyout>...</MenuFlyout>")` / `.raw("HeaderIcon", "...")` |
| Fragment-family subtrees (`Style`, `RowDefinition`, `DataTemplate`, `ResourceDictionary`, `VisualStateManager.VisualStateGroups`, `Storyboard`, brushes, `x:String`, …) | serialized back to XAML text, carried as `Raw() .raw("Resource", "<…>")` |
| `xmlns` declarations, element `prefix` (non-`x`/`d`/`mc`) | `.raw("xmlns", …)` / raw prefix pairs |
| CData / comment content | preserved verbatim inside the raw payload |
| `Style="{StaticResource Key}"` / `ThemeResource` | resource-key expressions (typed, not raw) |

---

## Future-Proof & 3rd-Party Agnostic Architecture

`Xaml2WinUIC` is engineered so that future WinUI updates, 3rd-party component suites (CommunityToolkit, WinUI Essentials, Syncfusion, DevExpress, Infragistics, Telerik), and arbitrary custom XAML extensions **never require recompiling the converter**:

1. **Whitespace & Tag Resilience**: The XML scanner parses closing tags irrespective of leading/trailing spaces, tabs, or multiline formatting (`</  toolkit:SettingsCard  \r\n>`), preventing parser desynchronization across diverse formatting tools.
2. **Universal Property Element Lowering**: Property elements that are not container item collections (`Children`, `Items`, `Child`, `Content`, `Inlines`, `PrimaryCommands`, `SecondaryCommands`, `Commands`, `Tabs`) automatically preserve their property name and serialized child subtree into `.raw("<PropertyName>", "<SerializedContent>")`. The property association is never discarded, and child elements are never improperly dropped into the visual tree.
3. **Dynamic ECMA-335 Metadata Pass-Through**: The `.winui` DSL carries all raw attributes and fragments into the `.winmd` binary metadata tables without loss. Downstream language projection compilers (`CppWinC`, `CsWinC`, `windows-rs`, `PyWinC`) consume these raw attributes directly from metadata and invoke the corresponding native COM/WinRT setters dynamically.

---

## Third-Party & Custom Control Registration

Unknown controls never block conversion: any control not in the built-in catalog (including third-party component suites such as **WinUI Essentials**, **CommunityToolkit**, Syncfusion, DevExpress, or proprietary in-house controls) is emitted under its XAML name, takes raw attributes and children, and is validated dynamically at `.winmd` compile time.

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
    }
  ],
  "xamlEvents":    ["Tapped", "RightTapped", "DoubleTapped"],
  "universalMods": [
    { "xaml": "CornerRadius", "winui": "cornerRadius" },
    { "xaml": "Elevation",    "winui": "elevation" }
  ],
  "fragFamilies":  ["GaugePalette", "ChartTheme"]
}
```

---

## Registry event routing (`xamlEvents`)

Events declared in `xamlEvents` or in a control's `events` list route through the same pipeline as built-in events:
`Click="OnClick"` → `.onClick { self.OnClick(); }` with an emitted `callback OnClick();` declaration in the `.winui` file. Custom handlers receive the exact signature found in code-behind.

---

## Output Format

A converted `.winui` file mirrors the project structure:

```swift
// Language: csharp
// Source: C:\Projects\MyWinUIApp\Views\MainPage.xaml

page MainPage {
    @state title: string = "Dashboard";
    callback OnRefresh();

    Column(spacing: 12) {
        TextBlock(text: $title).font(size: 24, bold: true)
        Button("Refresh").onClick { self.OnRefresh(); }
    }
}
```

The output conforms to the **[WinUI Language Specification](https://github.com/mominshaikhdev/winuic-compiler)** and is ready for direct consumption by `winuic`.

---

## Reference-Only Asset & LICENSE Copying

During conversion, `xaml2winuic` scans attribute values for local asset references (`Assets/foo.png`, `ms-appx:///Assets/foo.png`) and automatically copies referenced files from the source tree into the target output tree so assets are preserved next to the generated `.winui` files. The project's root `LICENSE` file is also copied when present.

---

## Building from Source

```cmd
git clone https://github.com/mominshaikhdev/xaml2winuic-compiler.git
cd xaml2winuic-compiler
build.bat
```

Requires Visual Studio 2022/2026 with MSVC C++23 modules support.

---

## Related

- **[`winuic-compiler`](https://github.com/mominshaikhdev/winuic-compiler)** — Compiles `.winui` files into pure language-agnostic ECMA-335 `.winmd` metadata.
- **[`CppWinC-Compiler`](https://github.com/mominshaikhdev/CppWinC-Compiler)** — Consumes `.winmd` metadata and emits native C++20/C++23 WinUI 3 binaries.
