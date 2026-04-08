# Roadmap: Production-Quality TUI Framework

Gap analysis comparing maya to Ink (React for CLI), which powers Claude Code's terminal UI.
All designs below use C++26 idioms that match maya's existing patterns: compile-time DSL with structural NTTPs, type-state safety via `requires`, signal-based reactivity, and zero-cost abstractions.

## What's been built

The following features from the original roadmap have been fully implemented:

### Tier 1 — Core Infrastructure ✅

- **Scrollable Container** — `widget/scrollable.hpp`: `ScrollableView` with viewport clipping, scrollbar rendering, overflow hidden, and margin offset
- **Text Input** — `widget/input.hpp`: `Input<Cfg>` with cursor, placeholder, password masking, and validation; `widget/textarea.hpp`: `TextArea` for multi-line editing
- **Focus Management** — `FocusNode` integrated into the framework for keyboard navigation across interactive widgets
- **Streaming Text** — Markdown widget handles streaming content; `ThinkingIndicator` provides animated progress during generation

### Tier 2 — Widget Library ✅

- **Select / Menu** — `widget/select.hpp`: `Select` with options and filtering; `widget/menu.hpp`: `Menu` with keyboard navigation
- **Spinner / Progress** — `widget/progress.hpp`: `ProgressBar` with dynamic width
- **50+ additional widgets** across 9 categories:
  - **Input**: Button, Checkbox, Radio, Slider, Input, TextArea, Select, Menu, CommandPalette
  - **Data Display**: Table, List, Tree, Badge, Callout
  - **Navigation**: Tabs, Breadcrumb, ActivityBar
  - **Display**: Divider, Disclosure, Sparkline, KeyHelp
  - **Overlay**: Modal, Popup, Toast
  - **Visualization**: BarChart, LineChart, Gauge, Heatmap, PixelCanvas
  - **Specialized**: Calendar, LogViewer, DiffView, PlanView
  - **Tool Widgets**: FileRef, Message

### Tier 3 — Rich Content ✅

- **Markdown Rendering** — `src/widget/markdown.cpp`: full renderer supporting headers, code blocks (fenced), ordered/unordered lists, links (OSC 8 hyperlinks), bold/italic emphasis, and tables

---

## What maya has today

- 50+ widgets across 9 categories (Input, Data Display, Navigation, Display, Overlay, Visualization, Specialized, Tool Widgets)
- Compile-time DSL with both compile-time and runtime pipes
- Runtime pipe system for dynamic values
- Flexbox layout engine
- Canvas API with double-buffered SIMD diff at 60fps
- Signal/slot reactivity system (SolidJS-inspired)
- Interactive widgets with FocusNode and keyboard handling
- Full markdown renderer with streaming support
- Mode::Inline for inline terminal rendering
- Braille rendering for high-res graphics
- StylePool with interned styles for fast comparison
- Type-state render pipeline (linear types with `&&`-qualified methods)

---

## Remaining Roadmap

The following features are not yet implemented.

### Syntax Highlighting

**Tree-sitter integration with constexpr theme mapping:**

```cpp
// Highlight theme — maps token types to compile-time styles
struct HighlightTheme {
    CTStyle keyword;
    CTStyle string;
    CTStyle comment;
    CTStyle function;
    CTStyle type;
    CTStyle number;
    CTStyle operator_;
    CTStyle punctuation;
};

constexpr HighlightTheme monokai = {
    .keyword     = {.has_fg=true, .fg_r=249, .fg_g=38,  .fg_b=114},
    .string      = {.has_fg=true, .fg_r=230, .fg_g=219, .fg_b=116},
    .comment     = {.has_fg=true, .fg_r=117, .fg_g=113, .fg_b=94, .italic_=true},
    .function    = {.has_fg=true, .fg_r=166, .fg_g=226, .fg_b=46},
    .type        = {.has_fg=true, .fg_r=102, .fg_g=217, .fg_b=239},
    .number      = {.has_fg=true, .fg_r=174, .fg_g=129, .fg_b=255},
    .operator_   = {.has_fg=true, .fg_r=249, .fg_g=38,  .fg_b=114},
    .punctuation = {.has_fg=true, .fg_r=248, .fg_g=248, .fg_b=242},
};

// Highlight function — returns vector of styled text spans
Element highlight(std::string_view code, std::string_view lang,
                  const HighlightTheme& theme = monokai) {
    auto tokens = tree_sitter_tokenize(code, lang);
    std::vector<Element> spans;
    for (auto& [text, type] : tokens) {
        Style s = theme_style_for(theme, type).runtime();
        spans.push_back(Element{TextElement{std::string{text}, s}});
    }
    return Element{ElementList{std::move(spans)}};
}
```

### Animation System

**`Animated<T>` — a signal that interpolates over time:**

```cpp
// Easing functions — all constexpr
namespace ease {
    constexpr float linear(float t)     { return t; }
    constexpr float ease_in(float t)    { return t * t; }
    constexpr float ease_out(float t)   { return t * (2 - t); }
    constexpr float ease_in_out(float t){
        return t < 0.5f ? 2*t*t : -1 + (4 - 2*t)*t;
    }

    // Spring physics — consteval parameter validation
    struct Spring {
        float stiffness = 100.0f;
        float damping   = 10.0f;
        float mass      = 1.0f;

        consteval Spring validate() const {
            if (stiffness <= 0) throw "stiffness must be positive";
            if (damping < 0)    throw "damping must be non-negative";
            if (mass <= 0)      throw "mass must be positive";
            return *this;
        }
    };

    constexpr float spring(float t, Spring s = Spring{}.validate()) {
        float omega = std::sqrt(s.stiffness / s.mass);
        float zeta  = s.damping / (2 * std::sqrt(s.stiffness * s.mass));
        // Underdamped spring approximation
        return 1.0f - std::exp(-zeta * omega * t) *
               std::cos(omega * std::sqrt(1 - zeta*zeta) * t);
    }
}

// Animated<T> — wraps Signal<T>, interpolates between old and new values
template <typename T, auto EasingFn = ease::ease_out>
class Animated {
    Signal<T> target_;
    T current_;
    T previous_;
    float progress_ = 1.0f;
    float duration_;

public:
    explicit Animated(T initial, float duration_s = 0.3f)
        : target_(initial), current_(initial), previous_(initial),
          duration_(duration_s) {}

    void set(T value) {
        previous_ = current_;
        target_.set(std::move(value));
        progress_ = 0.0f;
    }

    // Called each frame with dt
    void tick(float dt) {
        if (progress_ >= 1.0f) return;
        progress_ = std::min(1.0f, progress_ + dt / duration_);
        float t = EasingFn(progress_);
        current_ = lerp(previous_, target_(), t);
    }

    [[nodiscard]] const T& get() const { return current_; }
};

// Usage:
Animated<float> sidebar_width(30.0f, 0.2f);
sidebar_width.set(50.0f);  // smoothly animates over 200ms
```

### Absolute Positioning / Z-Index

**Overlay layer — rendered after main tree, composited onto canvas:**

```cpp
// OverlayCfg as structural NTTP
struct OverlayCfg {
    int z_index = 1;
    enum class Anchor { TopLeft, Center, BottomRight } anchor = Anchor::Center;
};

template <OverlayCfg Cfg = OverlayCfg{}>
struct OverlayTag {};

template <OverlayCfg Cfg = OverlayCfg{}>
inline constexpr OverlayTag<Cfg> overlay_{};

// Overlays are collected during build, sorted by z_index, rendered last
struct OverlayEntry {
    Element content;
    int z_index;
    OverlayCfg::Anchor anchor;
};

// Render pipeline extended: after paint(), composite overlays
template<>
class RenderPipeline<stage::Painted> {
    [[nodiscard]] RenderPipeline<stage::Composited>
    composite(std::span<OverlayEntry> overlays) && {
        // Sort by z_index, render each onto back buffer
        std::ranges::sort(overlays, {}, &OverlayEntry::z_index);
        for (auto& ov : overlays) {
            auto rect = compute_anchor_rect(ov.anchor, back_.width(), back_.height());
            render_element(ov.content, back_, rect);
        }
        return {back_, pool_, theme_, out_};
    }
};
```

### Static Component (Partial Terminal Updates)

**Split terminal into frozen (scrollback) + active (live) regions:**

```cpp
// Static items are written once above the active render area.
// Uses terminal scrolling regions (DECSTBM) to partition the screen.
//
// Architecture:
//   +-----------------------+
//   | Static region         | <- completed messages, written with raw write()
//   | (grows upward)        |    never re-rendered by diff engine
//   +-----------------------+
//   | Active region         | <- current response, rendered by pipeline
//   | (fixed height)        |    double-buffered + SIMD diff as usual
//   +-----------------------+

class StaticRegion {
    std::string frozen_output_;     // accumulated static content
    int frozen_rows_ = 0;

public:
    // Freeze an element — render it once, append to scrollback
    void freeze(const Element& elem, int width) {
        Canvas tmp(width, /*height=*/measure(elem, width));
        render_to(elem, tmp);

        // Serialize canvas to ANSI string (no diff — full render)
        std::string ansi = serialize_full(tmp);

        // Write to terminal above active area
        frozen_output_ += ansi;
        frozen_rows_ += tmp.height();

        // Adjust scroll region: DECSTBM sets active area
        // \e[{frozen_rows_+1};{screen_height}r
    }
};

// DSL: static_() wraps completed content
template <Node... Children>
struct StaticNode {
    std::tuple<Children...> children;
    StaticRegion& region;

    [[nodiscard]] Element build() const {
        // On first build: render children, freeze, return empty
        // On subsequent builds: return empty (already in scrollback)
        auto elem = v(/* children... */).build();
        region.freeze(elem, /*width from layout*/);
        return Element{TextElement{""}};  // placeholder — occupies no space
    }
};
```

---

## Design Principles

These designs follow maya's existing C++26 patterns:

| Pattern | Where used | C++26 feature |
|---------|-----------|---------------|
| **Structural NTTPs** | `ScrollCfg`, `InputCfg`, `OverlayCfg`, `HighlightTheme` | Structural types as template params |
| **Type-state via `requires`** | Focus modifiers only after `\| focusable`, border color after border | `requires` clauses |
| **Pipe composition** | `\| scrollable`, `\| focusable`, `\| overlay_<>` | `operator\|` overloads |
| **RAII scopes** | `FocusScope` mirrors `ReactiveScope` | Thread-local, deterministic destruction |
| **Signal reactivity** | `ScrollState`, `Input::value_`, `Animated<T>` | `Signal<T>`, `Computed<T>`, `Effect` |
| **`consteval` validation** | `Spring::validate()`, `SpinnerStyle` frame arrays | Compile-time error on invalid config |
| **`constexpr` computation** | Easing functions, braille scrollbar, block progress | Zero-cost runtime |
| **Concepts** | `Node`, event handler constraints, `std::ranges::range` | Concept-based polymorphism |
| **`std::variant` + `visit`** | Markdown AST, Element dispatch | Closed sum types |
| **Linear types (`&&`)** | Extended render pipeline stages | Move-only state transitions |
| **Zero-copy rendering** | `StreamSignal::delta()`, `StaticRegion` | `std::span`, direct string append |
