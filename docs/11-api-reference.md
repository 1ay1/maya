# API Reference

Complete reference for all public types, functions, and constants in maya.

## Table of Contents

- [App Framework](#app-framework)
- [DSL Nodes](#dsl-nodes)
- [DSL Style Tags](#dsl-style-tags)
- [DSL Layout Tags](#dsl-layout-tags)
- [DSL Runtime Pipes](#dsl-runtime-pipes)
- [DSL Factory Functions](#dsl-factory-functions)
- [DSL Constants](#dsl-constants)
- [Element Types](#element-types)
- [Style System](#style-system)
- [Color](#color)
- [Theme](#theme)
- [Border](#border)
- [Layout Types](#layout-types)
- [Event System](#event-system)
- [Event Predicates](#event-predicates)
- [Signals](#signals)
- [Canvas](#canvas)
- [Widgets](#widgets)
- [Core Types](#core-types)

---

## App Framework

### run()

```cpp
// With configuration
template <AnyEventFn EventFn, AnyRenderFn RenderFn>
void run(RunConfig cfg, EventFn&& event_fn, RenderFn&& render_fn);

// With defaults
template <AnyEventFn EventFn, AnyRenderFn RenderFn>
void run(EventFn&& event_fn, RenderFn&& render_fn);
```

### RunConfig

```cpp
struct RunConfig {
    std::string_view title      = "";
    int              fps        = 0;       // 0 = event-driven
    bool             mouse      = false;
    Mode             mode       = Mode::Fullscreen;
    Theme            theme      = theme::dark;
};
```

### Ctx

```cpp
struct Ctx {
    Size  size;    // {Columns width, Rows height}
    Theme theme;
};
```

### live()

```cpp
template <AnyLiveRenderFn RenderFn>
void live(LiveConfig cfg, RenderFn&& render_fn);
```

### LiveConfig

```cpp
struct LiveConfig {
    int   fps       = 30;
    int   max_width = 0;     // 0 = auto-detect
    bool  cursor    = false;
};
```

### canvas_run()

```cpp
Status canvas_run(
    CanvasConfig                                   cfg,
    std::function<void(StylePool&, int w, int h)>  on_resize,
    std::function<bool(const Event&)>              on_event,
    std::function<void(Canvas&, int w, int h)>     on_paint);
```

### CanvasConfig

```cpp
struct CanvasConfig {
    int         fps        = 60;
    bool        mouse      = false;
    Mode        mode       = Mode::Fullscreen;
    std::string title;
};
```

### print()

```cpp
void print(const Element& root);
void print(const Element& root, int width);
```

### quit()

```cpp
void quit() noexcept;  // Schedule clean exit after current frame
```

---

## DSL Nodes

All nodes satisfy the `Node` concept:

```cpp
template <typename T>
concept Node = requires(const T& n) {
    { n.build() } -> std::convertible_to<Element>;
};

template <typename T>
concept DslChild = Node<T> || ElementRange<T>;
```

`DslChild` is what `v()` and `h()` accept — either a `Node` (compile-time or
runtime) or an `ElementRange` (e.g. `std::vector<Element>`).

### TextNode

```cpp
template <Str S, CTStyle Sty = CTStyle{}>
struct TextNode {
    Element build() const;
};
```

Created via `t<"...">`.

### RuntimeTextNode

```cpp
template <typename S>
struct RuntimeTextNode {
    S content;
    Style style{};
    TextWrap wrap{TextWrap::Wrap};

    operator Element() const;
    Element build() const;
};
```

Created via `text()`. Supports `| Bold`, `| Fg<R,G,B>`, etc.

### BoxNode

```cpp
template <FlexDirection Dir, BoxCfg Cfg, typename... Children>
struct BoxNode {
    std::tuple<Children...> children;
    Element build() const;
};
```

Created via `v()` and `h()`. Supports layout pipes.

### DynNode

```cpp
template <typename F>
struct DynNode {
    F fn;
    Element build() const;  // Calls fn()
};
```

Created via `dyn()`.

### MapNode

```cpp
template <typename R, typename Proj>
struct MapNode {
    R range;
    Proj proj;
    Element build() const;  // Iterates range, applies proj
};
```

Created via `map()`.

### SpacerNode

```cpp
struct SpacerNode {
    operator Element() const;
    Element build() const;  // BoxElement with grow=1
};
```

### SepNode

```cpp
struct SepNode {
    operator Element() const;
    Element build() const;  // Horizontal separator line
};
```

### VSepNode

```cpp
struct VSepNode {
    operator Element() const;
    Element build() const;  // Vertical separator line
};
```

### BlankNode

```cpp
struct BlankNode {
    operator Element() const;
    Element build() const;  // Empty TextElement
};
```

---

## DSL Style Tags

```cpp
inline constexpr StyTag<...> Bold;
inline constexpr StyTag<...> Dim;
inline constexpr StyTag<...> Italic;
inline constexpr StyTag<...> Underline;
inline constexpr StyTag<...> Strike;
inline constexpr StyTag<...> Inverse;

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr StyTag<...> Fg;

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr StyTag<...> Bg;
```

---

## DSL Layout Tags

```cpp
template <int T, int R = T, int B = T, int L = R>
inline constexpr PadTag<T,R,B,L> pad;

template <int G>
inline constexpr GapTag<G> gap_;

template <BorderStyle BS>
inline constexpr BorderTag<BS> border_;

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr BColTag<R,G,B> bcol;    // Requires border first

template <int G = 1>
inline constexpr GrowTag<G> grow_;
```

---

## DSL Runtime Pipes

Runtime pipe tags for dynamic values. Same `|` syntax as compile-time pipes.

### Layout Pipes

| Function | Tag Type | Description |
|----------|----------|-------------|
| `padding(int)` / `padding(int,int)` / `padding(int,int,int,int)` | `RPad` | Runtime padding |
| `gap(int)` | `RGap` | Gap between children |
| `margin(int)` / `margin(int,int)` / `margin(int,int,int,int)` | `RMargin` | Outer margin |
| `grow(float g = 1.0f)` | `RGrow` | Flex grow factor |
| `width(int)` | `RWidth` | Fixed width |
| `height(int)` | `RHeight` | Fixed height |

### Border Pipes

| Function | Tag Type | Description |
|----------|----------|-------------|
| `border(BorderStyle)` | `RBorder` | Border style |
| `bcolor(Color)` | `RBCol` | Border color |
| `btext(string, BorderTextPos, BorderTextAlign)` | `RBText` | Border text label |

### Style Pipes

| Function | Tag Type | Description |
|----------|----------|-------------|
| `fgc(Color)` | `RFg` | Foreground color |
| `bgc(Color)` | `RBg` | Background color |

### Alignment Pipes

| Function | Tag Type | Description |
|----------|----------|-------------|
| `align(Align)` | `RAlign` | Cross-axis alignment |
| `justify(Justify)` | `RJust` | Main-axis distribution |
| `overflow(Overflow)` | `ROvf` | Overflow behavior |

### WrappedNode

```cpp
template <Node Inner>
struct WrappedNode;
```

Created automatically when a runtime pipe is applied to any Node. Satisfies `Node`.
Multiple runtime pipes chain onto the same WrappedNode without extra nesting.

---

## DSL Factory Functions

```cpp
// Compile-time text
template <Str S>
inline constexpr TextNode<S> t;

// Vertical stack — accepts Nodes and ElementRanges (e.g. vector<Element>)
template <DslChild... Cs>
constexpr auto v(Cs... cs) -> BoxNode<Column, BoxCfg{}, Cs...>;

// Horizontal stack — accepts Nodes and ElementRanges (e.g. vector<Element>)
template <DslChild... Cs>
constexpr auto h(Cs... cs) -> BoxNode<Row, BoxCfg{}, Cs...>;

// Runtime builders (promoted from detail namespace)
auto box() -> BoxBuilder;      // Base builder
auto vstack() -> BoxBuilder;   // Column direction
auto hstack() -> BoxBuilder;   // Row direction
auto center() -> BoxBuilder;   // Centered, grow=1

// Runtime text
template <typename S>
auto text(S&& content, Style s = {}) -> RuntimeTextNode<decay_t<S>>;

template <typename S>
auto text(S&& content, Style s, TextWrap w) -> RuntimeTextNode<decay_t<S>>;

// Dynamic node
template <typename F>
auto dyn(F&& fn) -> DynNode<decay_t<F>>;

// Map range
template <std::ranges::range R, typename Proj>
auto map(R&& range, Proj&& proj) -> MapNode<decay_t<R>, decay_t<Proj>>;

// Spacer/separator/blank (function forms)
constexpr auto spacer()    -> SpacerNode;
constexpr auto separator() -> SepNode;
constexpr auto blank()     -> BlankNode;
```

---

## DSL Constants

```cpp
inline constexpr SpacerNode space;
inline constexpr SepNode    sep;
inline constexpr VSepNode   vsep;
inline constexpr BlankNode  blank_;

inline constexpr BorderStyle Round  = BorderStyle::Round;
inline constexpr BorderStyle Single = BorderStyle::Single;
inline constexpr BorderStyle Thick  = BorderStyle::Bold;
inline constexpr BorderStyle Double = BorderStyle::Double;
```

---

## Element Types

### Element

```cpp
struct Element {
    std::variant<BoxElement, TextElement, ElementList> inner;

    // Implicit constructors from each variant type
    Element(BoxElement);
    Element(TextElement);
    Element(ElementList);

    Element build() const;  // Returns *this (satisfies Node concept)
};
```

### TextElement

```cpp
struct TextElement {
    std::string content;
    Style       style{};
    TextWrap    wrap{TextWrap::Wrap};

    Size measure(int max_width) const;
    std::vector<std::string> format(int max_width) const;
};
```

### BoxElement

```cpp
struct BoxElement {
    FlexStyle             layout{};
    Style                 style{};
    BorderConfig          border{};
    Overflow              overflow{Overflow::Visible};
    std::vector<Element>  children;

    bool has_border() const;
    int  inner_horizontal() const;  // padding + border horizontal
    int  inner_vertical() const;    // padding + border vertical
};
```

### ElementList

```cpp
struct ElementList {
    std::vector<Element> items;
};
```

### TextWrap

```cpp
enum class TextWrap {
    Wrap,              // Word wrap at container width
    TruncateEnd,       // "Long te..."
    TruncateMiddle,    // "Lon...xt"
    TruncateStart,     // "...g text"
    NoWrap             // Overflow
};
```

---

## Style System

### Style

```cpp
struct Style {
    std::optional<Color> fg, bg;
    bool bold = false, dim = false, italic = false;
    bool underline = false, strikethrough = false, inverse = false;

    Style with_fg(Color c) const;
    Style with_bg(Color c) const;
    Style with_bold(bool v = true) const;
    Style with_dim(bool v = true) const;
    Style with_italic(bool v = true) const;
    Style with_underline(bool v = true) const;
    Style with_strikethrough(bool v = true) const;
    Style with_inverse(bool v = true) const;

    Style merge(const Style& other) const;
    bool  empty() const;
    std::string to_sgr() const;
};

Style operator|(const Style& lhs, const Style& rhs);  // Merge
```

---

## Color

```cpp
class Color {
public:
    enum class Kind { Named, Indexed, Rgb };

    // Named colors (constexpr)
    static constexpr Color black();
    static constexpr Color red();
    static constexpr Color green();
    static constexpr Color yellow();
    static constexpr Color blue();
    static constexpr Color magenta();
    static constexpr Color cyan();
    static constexpr Color white();
    static constexpr Color bright_black();   // gray
    static constexpr Color bright_red();
    static constexpr Color bright_green();
    static constexpr Color bright_yellow();
    static constexpr Color bright_blue();
    static constexpr Color bright_magenta();
    static constexpr Color bright_cyan();
    static constexpr Color bright_white();
    static constexpr Color gray();

    // RGB (constexpr)
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b);

    // Hex (consteval — compile-time only)
    static consteval Color hex(uint32_t rgb);

    // HSL (constexpr)
    static constexpr Color hsl(float h, float s, float l);

    // Indexed (256-color)
    static Color indexed(uint8_t index);

    // Accessors
    Kind kind() const;
    uint8_t r() const, g() const, b() const;
    uint8_t index() const;

    // Adjustment (constexpr)
    Color lighten(float amount) const;
    Color darken(float amount) const;

    // SGR sequences
    std::string fg_sgr() const;
    std::string bg_sgr() const;
};
```

---

## Theme

```cpp
struct Theme {
    Color primary, secondary, accent;
    Color success, error, warning, info;
    Color text, inverse_text, muted;
    Color surface, background, border;
    Color diff_added, diff_removed, diff_changed;
    Color highlight, selection, cursor, link;
    Color placeholder, shadow, overlay;

    static constexpr Theme derive(Theme base, auto&& patch);
};

namespace theme {
    inline constexpr Theme dark;
    inline constexpr Theme light;
    inline constexpr Theme dark_ansi;
    inline constexpr Theme light_ansi;
}
```

---

## Border

### BorderStyle

```cpp
enum class BorderStyle {
    None, Single, Double, Round, Bold,
    SingleDouble, DoubleSingle, Classic, Arrow
};
```

### BorderSides

```cpp
struct BorderSides {
    bool top = false, right = false, bottom = false, left = false;

    static BorderSides all();
    static BorderSides none();
    static BorderSides horizontal();  // top + bottom
    static BorderSides vertical();    // left + right
};
```

### BorderText

```cpp
struct BorderText {
    std::string    content;
    BorderTextPos  position = BorderTextPos::Top;
    BorderTextAlign align   = BorderTextAlign::Start;
    int            offset   = 0;
};
```

### BorderColors

```cpp
struct BorderColors {
    std::optional<Color> top, right, bottom, left;
    static BorderColors uniform(Color c);
};
```

### BorderConfig

```cpp
struct BorderConfig {
    BorderStyle  style = BorderStyle::None;
    BorderSides  sides{};
    BorderColors colors{};
    std::optional<BorderText> text;

    bool empty() const;
};
```

---

## Layout Types

### FlexDirection

```cpp
enum class FlexDirection { Row, Column, RowReverse, ColumnReverse };
```

### FlexWrap

```cpp
enum class FlexWrap { NoWrap, Wrap, WrapReverse };
```

### Align

```cpp
enum class Align { Start, Center, End, Stretch, Baseline, Auto };
```

### Justify

```cpp
enum class Justify { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };
```

### Overflow

```cpp
enum class Overflow { Visible, Hidden, Scroll };
```

### Dimension

```cpp
struct Dimension {
    enum class Kind { Auto, Fixed, Percent };
    Kind  kind;
    float value;

    static Dimension auto_();
    static Dimension fixed(int v);
    static Dimension percent(float v);

    bool is_auto() const;
    bool is_fixed() const;
    bool is_percent() const;
    int  resolve(int parent) const;
};

Dimension operator""_pct(unsigned long long v);  // 50_pct
```

### FlexStyle

```cpp
struct FlexStyle {
    FlexDirection direction = FlexDirection::Row;
    FlexWrap      wrap      = FlexWrap::NoWrap;
    Align         align_items = Align::Stretch;
    Align         align_self  = Align::Auto;
    Justify       justify     = Justify::Start;
    float         grow   = 0;
    float         shrink = 1;
    Dimension     basis  = Dimension::auto_();
    Dimension     width  = Dimension::auto_();
    Dimension     height = Dimension::auto_();
    Dimension     min_width  = Dimension::auto_();
    Dimension     min_height = Dimension::auto_();
    Dimension     max_width  = Dimension::auto_();
    Dimension     max_height = Dimension::auto_();
    int           gap = 0;
    Edges<int>    padding{};
    Edges<int>    margin{};
};
```

### Edges\<T\>

```cpp
template <typename T>
struct Edges {
    T top{}, right{}, bottom{}, left{};

    Edges() = default;
    Edges(T all);            // uniform
    Edges(T v, T h);         // vertical, horizontal
    Edges(T t, T r, T b, T l);

    T horizontal() const;    // left + right
    T vertical() const;      // top + bottom
};
```

---

## Event System

### Event

```cpp
using Event = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;
```

### KeyEvent

```cpp
using Key = std::variant<CharKey, SpecialKey>;

struct CharKey { char32_t codepoint; };

enum class SpecialKey {
    Up, Down, Left, Right, Home, End,
    PageUp, PageDown, Tab, BackTab,
    Backspace, Delete, Insert, Enter, Escape,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
};

struct Modifiers {
    bool ctrl = false, alt = false, shift = false, super_ = false;
    bool none() const;
};

struct KeyEvent {
    Key         key;
    Modifiers   mods;
    std::string raw_sequence;
};
```

### MouseEvent

```cpp
enum class MouseButton { Left, Right, Middle, ScrollUp, ScrollDown, None };
enum class MouseEventKind { Press, Release, Move };

struct MouseEvent {
    MouseButton    button;
    MouseEventKind kind;
    Columns        x;
    Rows           y;
    Modifiers      mods;
};
```

### Other Events

```cpp
struct PasteEvent  { std::string content; };
struct FocusEvent  { bool focused; };
struct ResizeEvent { Columns width; Rows height; };
```

---

## Event Predicates

```cpp
// Keyboard
bool key(const Event&, char c);
bool key(const Event&, char32_t cp);
bool key(const Event&, SpecialKey sk);
bool ctrl(const Event&, char c);
bool alt(const Event&, char c);
bool shift(const Event&, SpecialKey sk);
bool any_key(const Event&);
const KeyEvent* as_key(const Event&);

// Mouse
struct MousePos { int col, row; };
bool mouse_clicked(const Event&, MouseButton = MouseButton::Left);
bool mouse_released(const Event&, MouseButton = MouseButton::Left);
bool mouse_moved(const Event&);
bool scrolled_up(const Event&);
bool scrolled_down(const Event&);
std::optional<MousePos> mouse_pos(const Event&);
const MouseEvent* as_mouse(const Event&);

// Other
bool resized(const Event&, int* w = nullptr, int* h = nullptr);
bool pasted(const Event&, std::string* out = nullptr);
bool focused(const Event&);
bool unfocused(const Event&);

// Fire-and-forget
template <typename F> bool on(const Event&, char c, F&& fn);
template <typename F> bool on(const Event&, char c1, char c2, F&& fn);
template <typename F> bool on(const Event&, SpecialKey sk, F&& fn);
```

---

## Signals

### Signal\<T\>

```cpp
template <typename T>
class Signal {
    Signal();                        // Default-construct T
    explicit Signal(U&& initial);    // Construct from value

    const T& get() const;           // Read (auto-tracks dependencies)
    const T& operator()() const;    // Shorthand for get()
    void set(const T& v);           // Write (notifies if changed)
    void set(T&& v);                // Write (move)
    void update(F&& fn);            // Mutate in-place (always notifies)
    auto map(F&& fn) const -> Computed<R>;  // Derive computed value
    uint64_t version() const;       // Change counter
};
```

### Computed\<T\>

```cpp
template <typename T>
class Computed {
    const T& get() const;           // Read (recomputes if dirty)
    const T& operator()() const;
};

template <std::invocable F>
auto computed(F&& fn) -> Computed<invoke_result_t<F>>;
```

### Effect

```cpp
class Effect {
    Effect();                        // Default (inactive)
    explicit Effect(F&& fn);        // Create and run immediately
    void dispose();                  // Unsubscribe from all deps
    bool active() const;
};

template <std::invocable F>
Effect effect(F&& fn);
```

### Batch

```cpp
class Batch {
    Batch();      // Begin batch
    ~Batch();     // End batch, flush notifications
};

template <std::invocable F>
decltype(auto) batch(F&& fn);  // Run fn inside a batch scope
```

---

## Canvas

### Canvas

```cpp
class Canvas {
    Canvas(int width, int height, StylePool* pool);

    void set(int x, int y, char32_t ch, uint16_t style_id, uint8_t width = 0);
    void write_text(int x, int y, std::string_view text, uint16_t style_id);
    void fill(Rect region, char32_t ch, uint16_t style_id);
    void clear();
    void resize(int w, int h);

    Cell get(int x, int y) const;
    int  width() const;
    int  height() const;

    void push_clip(Rect clip);
    void pop_clip();
};
```

### Cell

```cpp
struct Cell {
    char32_t character;
    uint16_t style_id;
    uint16_t hyperlink_id;
    uint8_t  width;

    uint64_t pack() const;
    static Cell unpack(uint64_t v);
};
```

### StylePool

```cpp
class StylePool {
    uint16_t intern(const Style& s);
    const Style& get(uint16_t id) const;
    void clear();
    std::size_t size() const;
};
```

### render_tree()

```cpp
void render_tree(const Element& root, Canvas& canvas,
                 StylePool& pool, const Theme& theme);
```

---

## Widgets

All widgets live in `maya::widget`. Full documentation in [13-widgets.md](13-widgets.md).

### Input

| Widget | Header | Description |
|--------|--------|-------------|
| `Input` | `widget/input.hpp` | Single-line text input with cursor and history |
| `TextArea` | `widget/textarea.hpp` | Multi-line text editor |
| `Checkbox` | `widget/checkbox.hpp` | Toggle checkbox |
| `ToggleSwitch` | `widget/checkbox.hpp` | iOS-style toggle switch |
| `Radio` | `widget/radio.hpp` | Radio button group |
| `Select` | `widget/select.hpp` | Dropdown select menu |
| `Slider` | `widget/slider.hpp` | Numeric slider |
| `Button` | `widget/button.hpp` | Clickable button with variants |
| `CommandPalette` | `widget/command_palette.hpp` | Fuzzy-search command launcher |

### Data Display

| Widget | Header | Description |
|--------|--------|-------------|
| `Table` | `widget/table.hpp` | Tabular data with column alignment |
| `Tree` | `widget/tree.hpp` | Expandable tree view |
| `List` | `widget/list.hpp` | Selectable item list |
| `KeyHelp` | `widget/key_help.hpp` | Keyboard shortcut legend |
| `Badge` | `widget/badge.hpp` | Inline status badge |
| `Callout` | `widget/callout.hpp` | Info/success/warning/error callout box |
| `Link` | `widget/link.hpp` | Hyperlink element |

### Navigation

| Widget | Header | Description |
|--------|--------|-------------|
| `Tabs` | `widget/tabs.hpp` | Tab bar navigation |
| `Breadcrumb` | `widget/breadcrumb.hpp` | Breadcrumb path navigation |
| `Menu` | `widget/menu.hpp` | Menu with selectable items |
| `ActivityBar` | `widget/activity_bar.hpp` | Vertical icon sidebar |
| `Scrollable` | `widget/scrollable.hpp` | Scrollable content region |

### Display

| Widget | Header | Description |
|--------|--------|-------------|
| `StreamingMarkdown` | `widget/markdown.hpp` | Streaming markdown renderer |
| `Spinner` | `widget/spinner.hpp` | Animated loading spinner |
| `ProgressBar` | `widget/progress.hpp` | Progress bar with percentage |
| `Gauge` | `widget/gauge.hpp` | Gauge / meter display |
| `Divider` | `widget/divider.hpp` | Horizontal or vertical divider |
| `Image` | `widget/image.hpp` | Terminal image display |
| `gradient()` | `widget/gradient.hpp` | Color gradient text builder |
| `Disclosure` | `widget/disclosure.hpp` | Collapsible disclosure section |

### Overlay

| Widget | Header | Description |
|--------|--------|-------------|
| `Modal` | `widget/modal.hpp` | Modal dialog with buttons |
| `Popup` | `widget/popup.hpp` | Floating popup |
| `ToastManager` | `widget/toast.hpp` | Toast notification manager |

### Visualization

| Widget | Header | Description |
|--------|--------|-------------|
| `BarChart` | `widget/bar_chart.hpp` | Horizontal/vertical bar chart |
| `LineChart` | `widget/line_chart.hpp` | Line chart with series |
| `Sparkline` | `widget/sparkline.hpp` | Inline sparkline graph |
| `Heatmap` | `widget/heatmap.hpp` | Grid heatmap |
| `Calendar` | `widget/calendar.hpp` | Calendar date display |
| `PixelCanvas` | `widget/canvas.hpp` | Pixel-level drawing canvas |

### Specialized

| Widget | Header | Description |
|--------|--------|-------------|
| `UserMessage` | `widget/message.hpp` | Chat user message bubble |
| `AssistantMessage` | `widget/message.hpp` | Chat assistant message bubble |
| `ThinkingBlock` | `widget/thinking.hpp` | Collapsible AI thinking block |
| `DiffView` | `widget/diff_view.hpp` | Side-by-side or unified diff |
| `PlanView` | `widget/plan_view.hpp` | Task plan with status tracking |
| `LogViewer` | `widget/log_viewer.hpp` | Filterable log viewer |
| `SearchResult` | `widget/search_result.hpp` | Grouped search results display |
| `FileRef` | `widget/file_ref.hpp` | File reference with icon |
| `Permission` | `widget/permission.hpp` | Permission approval prompt |

### Tool Widgets

| Widget | Header | Description |
|--------|--------|-------------|
| `ToolCall` | `widget/tool_call.hpp` | Generic tool invocation display |
| `BashTool` | `widget/bash_tool.hpp` | Shell command tool call |
| `ReadTool` | `widget/read_tool.hpp` | File read tool call |
| `EditTool` | `widget/edit_tool.hpp` | File edit tool call |
| `WriteTool` | `widget/write_tool.hpp` | File write tool call |
| `FetchTool` | `widget/fetch_tool.hpp` | HTTP fetch tool call |
| `AgentTool` | `widget/agent_tool.hpp` | Sub-agent tool call |

---

## Core Types

### Strong\<Tag, T\>

```cpp
template <typename Tag, typename T>
struct Strong {
    T value{};
    // Arithmetic: Strong + Strong, Strong - Strong, etc.
    // Comparison: ==, !=, <, >, <=, >=
};
```

### Size / Position / Rect

```cpp
using Columns = Strong<ColumnTag, int>;
using Rows    = Strong<RowTag, int>;

struct Size     { Columns width; Rows height; };
struct Position { Columns x; Rows y; };
struct Rect     { Position pos; Size size; };
```

### Result / Status / Error

```cpp
enum class ErrorKind {
    TerminalInit, Io, LayoutOverflow, InvalidStyle,
    InvalidUtf8, Unsupported, Signal, WouldBlock
};

struct Error {
    ErrorKind kind;
    std::string message;
    std::source_location location;

    static Error terminal(std::string msg);
    static Error io(std::string msg);
    static Error from_errno(std::string ctx);
};

template <typename T>
using Result = std::expected<T, Error>;
using Status = Result<void>;
```
