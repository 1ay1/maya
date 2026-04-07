# Roadmap: Production-Quality TUI Framework

Gap analysis comparing maya to Ink (React for CLI), which powers Claude Code's terminal UI.
All designs below use C++26 idioms that match maya's existing patterns: compile-time DSL with structural NTTPs, type-state safety via `requires`, signal-based reactivity, and zero-cost abstractions.

## What maya has today

- Flexbox layout engine with compile-time DSL
- Canvas API with double-buffered SIMD diff at 60fps
- Signal/slot reactivity system (SolidJS-inspired)
- 50+ basic components (text, box, border, etc.)
- Braille rendering for high-res graphics
- StylePool with interned styles for fast comparison
- Type-state render pipeline (linear types with `&&`-qualified methods)

---

## Tier 1 — Core Infrastructure

### 1. Scrollable Container

**DSL integration** — extends the pipe syntax naturally:

```cpp
// Compile-time scrollable with structural NTTP config
struct ScrollCfg {
    bool auto_bottom   = false;   // auto-scroll to bottom on new content
    bool show_bar      = true;    // scrollbar indicator
    int  overscan      = 3;       // render N extra rows above/below viewport
};

template <ScrollCfg Cfg = ScrollCfg{}>
struct ScrollTag {};

template <ScrollCfg Cfg = ScrollCfg{}>
inline constexpr ScrollTag<Cfg> scrollable{};

// Type-state: scroll modifiers only valid after | scrollable
template <ScrollCfg Cfg>
struct AutoBottomTag {};

template <FlexDirection Dir, BoxCfg BC, typename... Cs, ScrollCfg SC>
    requires (/* node has scrollable */)
constexpr auto operator|(ScrollNode<Dir, BC, SC, Cs...> n, AutoBottomTag<SC>) { ... }

// Usage:
auto chat = v(
    map(messages, [](auto& m) { return render_message(m); })
) | scrollable<{.auto_bottom = true, .show_bar = true}> | grow_<1>;
```

**Implementation — virtual viewport via `ComponentElement`:**

```cpp
// ScrollState holds reactive scroll position
struct ScrollState {
    Signal<int> offset{0};           // current scroll offset (rows)
    Signal<int> content_height{0};   // total content height
    Signal<int> viewport_height{0};  // visible area height

    // Derived signals — computed lazily, memoized
    Computed<bool> at_bottom = computed([&] {
        return offset() + viewport_height() >= content_height();
    });

    Computed<float> progress = computed([&] {
        int max = content_height() - viewport_height();
        return max > 0 ? static_cast<float>(offset()) / max : 1.0f;
    });
};

// Renders as ComponentElement — layout allocates size, then we slice
Element scroll_render(ScrollState& state, Element content, int width, int height) {
    state.viewport_height.set(height);
    // Measure content at full width, unconstrained height
    auto measured = layout::measure(content, width, /*max_height=*/INT_MAX);
    state.content_height.set(measured.height);

    // Render only the visible slice [offset, offset + height)
    return layout::slice(content, state.offset(), height);
}
```

**Scrollbar — braille-resolution indicator in 1 column:**

```cpp
// 8 dots per character cell vertically → sub-character precision
void render_scrollbar(Canvas& c, int x, int y, int h, float progress, float ratio) {
    int thumb_dots = std::max(2, static_cast<int>(ratio * h * 4));
    int total_dots = h * 4;
    int thumb_start = static_cast<int>(progress * (total_dots - thumb_dots));

    for (int row = 0; row < h; ++row) {
        char32_t ch = U'\u2800'; // braille base
        for (int dot = 0; dot < 4; ++dot) {
            int pos = row * 4 + dot;
            if (pos >= thumb_start && pos < thumb_start + thumb_dots)
                ch |= braille_dot_bit(0, dot); // left column dots
        }
        c.set(x, y + row, ch, scrollbar_style);
    }
}
```

---

### 2. Text Input

**Concept-based event binding:**

```cpp
// InputConfig as structural NTTP — compile-time validation
struct InputCfg {
    bool multiline     = false;
    bool history       = true;
    int  max_length    = 0;      // 0 = unlimited
    bool password      = false;  // mask characters
};

template <InputCfg Cfg = InputCfg{}>
class Input {
    Signal<std::string> value_{""};
    Signal<int>         cursor_{0};
    Signal<bool>        focused_{false};

    // History as a signal over a vector — update() for in-place mutation
    Signal<std::vector<std::string>> history_{std::vector<std::string>{}};
    Signal<int> history_index_{-1};

public:
    // Concept-constrained event handlers
    template <std::invocable<std::string_view> F>
    void on_submit(F&& fn);

    template <std::invocable<std::string_view> F>
    void on_change(F&& fn);

    // Expose signals for reactive composition
    [[nodiscard]] const auto& value()   const { return value_; }
    [[nodiscard]] const auto& cursor()  const { return cursor_; }
    [[nodiscard]] const auto& focused() const { return focused_; }

    // Build into the DSL tree
    [[nodiscard]] Element build() const;

    // Process a key event — returns whether it was consumed
    [[nodiscard]] bool handle(const KeyEvent& ev);
};

// Usage:
Input<{.multiline = true, .history = true}> prompt;

auto ui = v(
    scroll_area,
    h(
        text("> ") | Dim,
        prompt  // satisfies Node concept, has .build()
    ) | border_<Round> | bcol<80, 80, 100>
);
```

**Cursor rendering — uses the terminal's native cursor:**

```cpp
// After diff, emit cursor position escape
void emit_cursor(std::string& out, int x, int y) {
    // CSI y;x H — position hardware cursor
    out += "\x1b[";
    append_int(out, y + 1);
    out += ';';
    append_int(out, x + 1);
    out += 'H';
    out += "\x1b[?25h"; // show cursor
}
```

---

### 3. Focus Management

**Thread-local focus scope — mirrors ReactiveScope pattern:**

```cpp
// FocusScope — RAII, nestable, thread-local (same pattern as ReactiveScope)
struct FocusScope {
    FocusScope* parent;
    std::vector<FocusNode*> nodes;  // registered focusable nodes in this scope
    int active_index = 0;

    explicit FocusScope(FocusScope* parent = nullptr);
    ~FocusScope();
};

inline thread_local FocusScope* current_focus_scope = nullptr;

// FocusNode — registered automatically via RAII
struct FocusNode {
    Signal<bool> focused{false};
    int order;  // tab order within scope

    FocusNode() {
        if (current_focus_scope)
            current_focus_scope->nodes.push_back(this);
    }
    ~FocusNode() {
        if (current_focus_scope)
            std::erase(current_focus_scope->nodes, this);
    }
};

// Navigate focus
void focus_next() {
    auto* scope = current_focus_scope;
    if (!scope || scope->nodes.empty()) return;
    scope->nodes[scope->active_index]->focused.set(false);
    scope->active_index = (scope->active_index + 1) % scope->nodes.size();
    scope->nodes[scope->active_index]->focused.set(true);
}
```

**DSL integration — `| focusable` as a compile-time pipe:**

```cpp
struct FocusableTag {};
inline constexpr FocusableTag focusable{};

// Wraps any node in a focus-aware container
template <Node N>
struct FocusableNode {
    N inner;
    FocusNode focus;

    [[nodiscard]] Element build() const {
        auto elem = inner.build();
        if (focus.focused()) {
            // Apply focus ring style
            return apply_focus_ring(std::move(elem));
        }
        return elem;
    }
};

template <Node N>
auto operator|(N n, FocusableTag) {
    return FocusableNode<N>{std::move(n), {}};
}
```

**Modal focus trapping — nested FocusScope:**

```cpp
// Modal creates a nested scope — Tab cycles within the modal only
template <Node Content>
struct ModalNode {
    Content content;

    [[nodiscard]] Element build() const {
        FocusScope scope(current_focus_scope);  // push nested scope
        auto elem = content.build();            // children register here
        // scope destructor pops
        return overlay(std::move(elem));
    }
};
```

---

### 4. Streaming Text

**Append-only signal — O(1) incremental updates:**

```cpp
// StreamSignal<T> — append-only variant of Signal for streaming content.
// Subscribers receive only the delta, not the full value.
template <typename T>
class StreamSignal {
    struct Node final : detail::ReactiveNode {
        std::vector<T> buffer;
        size_t last_notified = 0;  // watermark for delta tracking

        void mark_dirty() override {}
        void evaluate() override {}
    };

    std::shared_ptr<Node> node_;

public:
    void append(T&& chunk) {
        node_->buffer.push_back(std::move(chunk));
        detail::notify_subscribers(node_.get());
    }

    // Full content
    [[nodiscard]] std::span<const T> get() const {
        detail::track(node_.get());
        return node_->buffer;
    }

    // Delta since last read
    [[nodiscard]] std::span<const T> delta() const {
        detail::track(node_.get());
        auto d = std::span{node_->buffer}.subspan(node_->last_notified);
        node_->last_notified = node_->buffer.size();
        return d;
    }
};

// Usage:
StreamSignal<std::string> tokens;

// In LLM callback:
tokens.append("Hello");
tokens.append(" world");

// In render — only processes new tokens:
auto stream_text = component([&](int w, int h) -> Element {
    auto new_tokens = tokens.delta();
    for (auto& tok : new_tokens)
        accumulated += tok;
    return text(accumulated);
});
```

---

## Tier 2 — Widget Library

### 5. Select / Menu

**Compile-time option list via structural NTTP:**

```cpp
// Options known at compile time
template <Str... Options>
struct SelectNode {
    Signal<int> selected{0};

    [[nodiscard]] Element build() const {
        int i = 0;
        return v(
            (build_option<Options>(i++, selected())...  )
        ).build();
    }
};

template <Str... Options>
auto select() { return SelectNode<Options...>{}; }

// Usage:
auto menu = select<"Option A", "Option B", "Option C">();

// Runtime options via range
template <std::ranges::range R>
auto select(R&& options, Signal<int>& selected);
```

### 6. Spinner

**Constexpr animation frames — zero-cost frame lookup:**

```cpp
struct SpinnerStyle {
    std::span<const std::string_view> frames;
    int interval_ms;
};

// All frames constexpr — no heap allocation
constexpr std::string_view dots_frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
constexpr std::string_view line_frames[] = {"-","\\","|","/"};

constexpr SpinnerStyle dots  = {dots_frames, 80};
constexpr SpinnerStyle line  = {line_frames, 100};

template <const SpinnerStyle& Style = dots>
struct SpinnerNode {
    [[nodiscard]] Element build(float t) const {
        int frame = static_cast<int>(t * 1000 / Style.interval_ms)
                    % Style.frames.size();
        return text(Style.frames[frame]);
    }
};

// Usage in DSL:
h(spinner<dots>(), text(" Loading...") | Dim)
```

### 7. Progress Bar

**Braille-resolution progress — 2x horizontal resolution:**

```cpp
template <int Width = 20>
struct ProgressNode {
    const Signal<float>& progress;

    [[nodiscard]] Element build() const {
        float p = std::clamp(progress(), 0.0f, 1.0f);
        int total_eighths = Width * 8;
        int filled = static_cast<int>(p * total_eighths);

        std::string bar;
        for (int i = 0; i < Width; ++i) {
            int cell_eighths = std::clamp(filled - i * 8, 0, 8);
            // Unicode block elements: ▏▎▍▌▋▊▉█
            constexpr char32_t blocks[] =
                {U' ', U'▏', U'▎', U'▍', U'▌', U'▋', U'▊', U'▉', U'█'};
            encode_utf8(blocks[cell_eighths], bar);
        }
        return text(std::move(bar));
    }
};
```

---

## Tier 3 — Rich Content Rendering

### 8. Markdown Rendering

**Constexpr tokenizer + compile-time style mapping:**

```cpp
// Markdown AST nodes — variant-based, same pattern as Element
struct MdText   { std::string content; };
struct MdBold   { std::vector<MdNode> children; };
struct MdItalic { std::vector<MdNode> children; };
struct MdCode   { std::string content; std::string lang; };
struct MdList   { std::vector<MdNode> items; bool ordered; };
struct MdLink   { std::string text; std::string url; };
struct MdHeading{ std::vector<MdNode> children; int level; };
struct MdBlock  { std::vector<MdNode> children; };  // blockquote

struct MdNode {
    using Variant = std::variant<MdText, MdBold, MdItalic, MdCode,
                                 MdList, MdLink, MdHeading, MdBlock>;
    Variant inner;
};

// Map MdNode → Element using visit + overload (exhaustive dispatch)
Element md_to_element(const MdNode& node) {
    return std::visit(overload{
        [](const MdText& t)   { return text(t.content); },
        [](const MdBold& b)   {
            return h(map(b.children, md_to_element)).build()
                   | StyTag<CTStyle{.bold_ = true}>{};
        },
        [](const MdItalic& i) {
            return h(map(i.children, md_to_element)).build()
                   | StyTag<CTStyle{.italic_ = true}>{};
        },
        [](const MdCode& c)   {
            // Syntax highlighting integration point
            return vstack()
                .border(BorderStyle::Round)
                .border_color(Color::rgb(60, 60, 80))
                .padding(0, 1, 0, 1)(
                    highlight(c.content, c.lang)  // returns Element
                );
        },
        [](const MdLink& l) {
            // OSC 8 hyperlink: \e]8;;URL\e\\TEXT\e]8;;\e\\
            return text(l.text) | Underline | Fg<100, 149, 237>;
        },
        // ... remaining visitors
    }, node.inner);
}

// Top-level API
Element md(std::string_view markdown) {
    auto ast = parse_markdown(markdown);
    return v(map(ast.blocks, md_to_element)).build();
}

// Usage:
auto response = md(R"(
## Hello
This is **bold** and *italic*.

```cpp
int main() { return 0; }
```)");
```

### 9. Syntax Highlighting

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

### 10. Static Component

**Split terminal into frozen (scrollback) + active (live) regions:**

```cpp
// Static items are written once above the active render area.
// Uses terminal scrolling regions (DECSTBM) to partition the screen.
//
// Architecture:
//   ┌─────────────────────┐
//   │ Static region        │ ← completed messages, written with raw write()
//   │ (grows upward)       │    never re-rendered by diff engine
//   ├─────────────────────┤
//   │ Active region        │ ← current response, rendered by pipeline
//   │ (fixed height)       │    double-buffered + SIMD diff as usual
//   └─────────────────────┘

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

## Tier 4 — Animation & Polish

### 11. Animation System

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

### 12. Absolute Positioning / Z-Index

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
