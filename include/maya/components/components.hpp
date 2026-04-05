#pragma once
// maya::components — React-like component library for terminal UIs
//
// Every component is a function (stateless) or class (stateful) that
// produces maya::Element. Compose them with maya's DSL and layout system.
//
// Usage:
//   #include <maya/components/components.hpp>
//   using namespace maya::components;
//
//   // Stateless (function component):
//   auto ui = Callout({.severity = Severity::Warning, .title = "Heads up"});
//
//   // Stateful (class component):
//   TextInput input({.placeholder = "Type here..."});
//   input.update(ev);   // in event handler
//   input.render();     // in render function
//
// ═══════════════════════════════════════════════════════════════════════════
// Component inventory:
//
// PRIMITIVES (stateless — function returns Element)
//   Spinner         Animated loading indicator
//   ProgressBar     Linear progress with segments
//   Button          Styled text button (filled/outlined/tinted/ghost)
//   Chip            Small badge/tag label
//   Badge           Numeric count badge
//   Callout         Info/success/warning/error notification box
//   DiffStat        "+N / -M" colored badge
//   Divider         Horizontal/vertical separator
//   KeyBinding      Keyboard shortcut display
//   KeyBindings     Row of keybindings
//   Toggle          Checkbox / switch / dot toggle
//   Sparkline       Mini inline chart with block characters
//   Gauge           Labeled value meter with thresholds
//   BarChart        Horizontal bar chart with labels
//   CostMeter       AI token usage and cost display
//   FormField       Label + description + error wrapper
//   TokenStream     Live token generation rate with sparkline
//
// INPUT (stateful — class with update() + render())
//   TextInput       Multi-line text editor with cursor
//   SearchInput     Single-line search with filter
//   RadioGroup<T>   Mutually exclusive option selection
//   NumberInput     Numeric stepper with +/- bounds
//   StreamingText   Character-by-character text reveal
//
// LAYOUT (stateful)
//   Disclosure      Collapsible section with chevron
//   List<T>         Scrollable list with keyboard selection
//   Tabs            Tab bar with active indicator
//   ScrollView      Virtual scrolling container
//   Select<T>       Dropdown selector
//   Table           Grid with columns, headers, sorting, selection
//   Tree<T>         Recursive hierarchical view with connectors
//   FileTree        File browser with icons and git status
//
// CONTENT (stateless)
//   Markdown        Markdown-to-Element renderer
//   CodeBlock       Code display with line numbers
//   DiffView        Unified diff with colored +/- lines
//   LogView         ANSI-aware scrollable log viewer
//
// AGENT COMPOSITES (for building Zed-like agent UIs)
//   MessageBubble   Chat message container (user/assistant/system)
//   ToolCard        Tool call card with status and collapsible body
//   ThinkingBlock   Collapsible thinking/reasoning display
//   StatusBar       Bottom bar with sections
//   ThreadList      Conversation history with time groups
//   ActivityBar     Sidebar with plan/edits/status sections
//   ContextWindow   Context window usage segmented bar
//   Timeline        Vertical event timeline (CI/pipeline style)
//   Waterfall       Timing waterfall chart (devtools / CI style)
//
// VISUALIZATION
//   Heatmap         Grid heatmap (GitHub contribution graph style)
//   FlameChart      Flame graph visualization for profiling
//   GitGraph        ASCII git commit graph with branch lines
//   InlineDiff      Word-level diff within lines
//
// NAVIGATION & NOTIFICATIONS
//   Breadcrumb      Path navigation breadcrumbs
//   Toast           Auto-dismissing stacked notifications
// ═══════════════════════════════════════════════════════════════════════════

// Core: palette, enums, helpers
#include "core.hpp"

// Primitives
#include "spinner.hpp"
#include "progress_bar.hpp"
#include "button.hpp"
#include "chip.hpp"
#include "callout.hpp"
#include "diff_stat.hpp"
#include "divider.hpp"
#include "key_binding.hpp"
#include "toggle.hpp"
#include "sparkline.hpp"
#include "gauge.hpp"
#include "bar_chart.hpp"
#include "cost_meter.hpp"
#include "form_field.hpp"
#include "token_stream.hpp"

// Input
#include "text_input.hpp"
#include "search_input.hpp"
#include "radio_group.hpp"
#include "number_input.hpp"
#include "streaming_text.hpp"

// Layout
#include "disclosure.hpp"
#include "list.hpp"
#include "tabs.hpp"
#include "scroll_view.hpp"
#include "select.hpp"
#include "table.hpp"
#include "tree.hpp"
#include "file_tree.hpp"

// Content
#include "markdown.hpp"
#include "code_block.hpp"
#include "diff_view.hpp"
#include "log_view.hpp"

// Agent composites
#include "permission.hpp"
#include "message_bubble.hpp"
#include "tool_card.hpp"
#include "thinking.hpp"
#include "status_bar.hpp"
#include "thread_list.hpp"
#include "activity_bar.hpp"
#include "context_window.hpp"
#include "timeline.hpp"
#include "waterfall.hpp"

// Visualization
#include "heatmap.hpp"
#include "flame_chart.hpp"
#include "git_graph.hpp"
#include "inline_diff.hpp"

// Navigation & Notifications
#include "breadcrumb.hpp"
#include "toast.hpp"

// Additional components
#include "context_pill.hpp"
#include "glimmer_text.hpp"
#include "read_card.hpp"
#include "command_card.hpp"
#include "accordion_bar.hpp"
#include "feedback_buttons.hpp"
#include "checkpoint.hpp"
#include "edit_card.hpp"
#include "search_card.hpp"
#include "model_selector.hpp"
