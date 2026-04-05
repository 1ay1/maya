#pragma once
// maya::components::PermissionPrompt — Tool call approval UI
//
// Renders inside a ToolCard when a tool needs user confirmation.
// Three styles matching Zed's agent permission flow:
//
//   Flat — simple Allow / Deny buttons (symlink escape, etc.)
//   Dropdown — Allow / Deny + granularity picker ("always for X")
//   Dangerous — red-highlighted deny-by-default for risky operations
//
// Usage:
//
//   auto perm = PermissionPrompt({
//       .title = "Run `rm -rf build/`",
//       .options = {
//           {.label = "Only this time"},
//           {.label = "Always for `rm` commands", .decision = PermissionDecision::AllowAlways},
//       },
//   });
//   perm.update(ev);
//   auto ui = perm.render(frame);
//   if (perm.decided()) handle(perm.decision());

#include "core.hpp"
#include "button.hpp"
#include "key_binding.hpp"

namespace maya::components {

// ── Decision types ──────────────────────────────────────────────────────────

enum class PermissionDecision {
    AllowOnce,
    AllowAlways,
    DenyOnce,
    DenyAlways,
    Undecided,
};

// ── Dropdown option ─────────────────────────────────────────────────────────

struct PermissionOption {
    std::string         label;
    PermissionDecision  decision   = PermissionDecision::AllowAlways;
    bool                is_default = false;
};

// ── Prompt style ────────────────────────────────────────────────────────────

enum class PermissionStyle {
    Flat,       // Simple Allow / Deny (e.g. symlink escape)
    Dropdown,   // Allow / Deny + granularity dropdown
    Dangerous,  // Red warning, deny-focused
};

// ── Props ───────────────────────────────────────────────────────────────────

struct PermissionPromptProps {
    std::string title    = "";        // e.g. "Run `cargo test`"
    std::string warning  = "";        // optional danger message
    PermissionStyle style = PermissionStyle::Dropdown;
    std::vector<PermissionOption> options = {};
    bool show_keyhints   = true;      // show keyboard shortcut hints
};

// ── Component ───────────────────────────────────────────────────────────────

class PermissionPrompt {
    PermissionPromptProps props_;
    PermissionDecision    decision_  = PermissionDecision::Undecided;
    bool                  dropdown_open_ = false;
    int                   dropdown_idx_  = 0;

public:
    explicit PermissionPrompt(PermissionPromptProps props = {})
        : props_(std::move(props))
    {
        // Find default option index
        for (int i = 0; i < static_cast<int>(props_.options.size()); ++i) {
            if (props_.options[i].is_default) { dropdown_idx_ = i; break; }
        }
    }

    [[nodiscard]] bool decided() const { return decision_ != PermissionDecision::Undecided; }
    [[nodiscard]] PermissionDecision decision() const { return decision_; }

    void reset() { decision_ = PermissionDecision::Undecided; dropdown_open_ = false; }

    void update(const Event& ev);

    [[nodiscard]] Element render(int frame = 0) const;
};

} // namespace maya::components
