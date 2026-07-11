// proc_table.cpp — the rockbottom-shaped process list on maya::Table.
//
// Everything a real proc list needs, all from ONE widget:
//
//   • selection      ▎ cursor bar + bold row, ↑↓/j/k, PgUp/PgDn, g/G
//   • windowing      the body shows however many rows FIT the pane —
//                    tbl.build() | grow(1) and the count falls out of
//                    the layout; a ▐ scrollbar gutter tracks the window
//   • sorting        s cycles the sort column, r flips direction — the
//                    host sorts its data, the table shows ▾/▴ + accent
//   • responsive     ONE solve_columns plan feeds header + rows: NAME
//                    flexes (weight=1, truncates with …), low-keep
//                    columns shed whole as the terminal narrows
//   • mouse-ready    rows/headers register hit_id rects (row_hit_kind /
//                    header_hit_kind) — resolve clicks with hit_test
//
// Resize the terminal — width resheds columns, height rewindows rows.
// q quits.

#include <maya/maya.hpp>
#include <maya/widget/table.hpp>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;

namespace {

struct Proc {
    int pid;
    std::string name;
    double cpu;
    int mem_mb;
    int threads;
    char state;
};

std::vector<Proc> fake_procs() {
    const char* names[] = {
        "rb", "agentty", "kitty", "chrome --type=renderer",
        "tmux: server", "pipewire-pulse", "kworker/u50:11", "systemd",
        "sshd", "bash", "htop", "dockerd", "postgres", "nvim",
        "firefox", "cc1plus", "make", "git", "node", "cargo",
        "rust-analyzer", "clangd", "Xorg", "pulseaudio",
    };
    std::vector<Proc> ps;
    int i = 0;
    for (auto* n : names) {
        ps.push_back({900000 + i * 137, n,
                      (i * 73 % 970) / 10.0,
                      16 + (i * 251) % 2048,
                      1 + i % 12,
                      i % 3 == 0 ? 'R' : 'S'});
        ++i;
    }
    return ps;
}

std::string fmt1(double v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

} // namespace

struct ProcTableDemo {
    struct Model {
        std::vector<Proc> procs = fake_procs();
        int cursor    = 0;
        int sort_col  = 2;       // CPU%
        bool desc     = true;
    };

    struct Key { KeyEvent ev; };
    struct Quit {};
    using Msg = std::variant<Key, Quit>;

    static Model init() {
        Model m;
        sort_procs(m);
        return m;
    }

    static void sort_procs(Model& m) {
        auto cmp = [&](const Proc& a, const Proc& b) {
            auto lt = [&]() -> bool {
                switch (m.sort_col) {
                    case 0: return a.pid < b.pid;
                    case 1: return a.name < b.name;
                    case 2: return a.cpu < b.cpu;
                    case 3: return a.mem_mb < b.mem_mb;
                    case 4: return a.threads < b.threads;
                    default: return a.state < b.state;
                }
            };
            return m.desc ? !lt() && !(a.pid == b.pid) : lt();
        };
        std::stable_sort(m.procs.begin(), m.procs.end(), cmp);
    }

    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        using C = Cmd<Msg>;
        if (std::holds_alternative<Quit>(msg)) return {std::move(m), C::quit()};
        const auto& ev = std::get<Key>(msg).ev;
        const int n = static_cast<int>(m.procs.size());
        bool handled = std::visit(overload{
            [&](SpecialKey sk) {
                switch (sk) {
                    case SpecialKey::Up:       m.cursor -= 1; return true;
                    case SpecialKey::Down:     m.cursor += 1; return true;
                    case SpecialKey::PageUp:   m.cursor -= 10; return true;
                    case SpecialKey::PageDown: m.cursor += 10; return true;
                    case SpecialKey::Home:     m.cursor = 0; return true;
                    case SpecialKey::End:      m.cursor = n; return true;
                    default: return false;
                }
            },
            [&](CharKey ck) {
                switch (ck.codepoint) {
                    case 'k': m.cursor -= 1; return true;
                    case 'j': m.cursor += 1; return true;
                    case 'g': m.cursor = 0; return true;
                    case 'G': m.cursor = n; return true;
                    case 's': m.sort_col = (m.sort_col + 1) % 6;
                              sort_procs(m); return true;
                    case 'r': m.desc = !m.desc; sort_procs(m); return true;
                    default: return false;
                }
            },
            [](auto&&) { return false; },
        }, ev.key);
        (void)handled;
        m.cursor = std::clamp(m.cursor, 0, n - 1);
        return {std::move(m), C{}};
    }

    static Element view(const Model& m) {
        TableConfig cfg;
        cfg.selectable  = true;
        cfg.sort_col    = m.sort_col;
        cfg.sort_desc   = m.desc;
        cfg.show_status = true;
        cfg.show_border = true;
        cfg.title       = "PROCESSES";
        cfg.border_color = Color::hex(0x44475A);

        Table tbl({{"PID", 0, ColumnAlign::Right},
                   {"NAME", 0, ColumnAlign::Left, 0, /*weight=*/1.0f, /*min=*/10},
                   {"CPU%", 0, ColumnAlign::Right},
                   {"MEM", 0, ColumnAlign::Right, 2},
                   {"THR", 0, ColumnAlign::Right, 1},
                   {"S", 0, ColumnAlign::Left, 3}}, cfg);
        std::vector<std::vector<std::string>> rows;
        rows.reserve(m.procs.size());
        for (const auto& p : m.procs)
            rows.push_back({std::to_string(p.pid), p.name, fmt1(p.cpu),
                            std::to_string(p.mem_mb) + "M",
                            std::to_string(p.threads),
                            std::string(1, p.state)});
        tbl.set_rows(std::move(rows));
        tbl.set_selected(m.cursor);

        return v(
            h(gradient("proc_table", Color::hex(0xFF5F6D), Color::hex(0xFFC371),
                       Style{}.with_bold()),
              space,
              text("resize me — width resheds, height rewindows",
                   Style{}.with_dim())) | padding(0, 1),
            tbl.build() | grow(1),
            h(text(" ↑↓/j/k", Style{}.with_bold()), text("·move  ", Style{}.with_dim()),
              text("s", Style{}.with_bold()), text("·sort col  ", Style{}.with_dim()),
              text("r", Style{}.with_bold()), text("·reverse  ", Style{}.with_dim()),
              text("q", Style{}.with_bold()), text("·quit", Style{}.with_dim()))
                | height(1)
        );
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return Sub<Msg>::on_key([](const KeyEvent& ev) -> std::optional<Msg> {
            if (const auto* ck = std::get_if<CharKey>(&ev.key);
                ck && ck->codepoint == 'q')
                return Quit{};
            return Key{ev};
        });
    }
};

static_assert(Program<ProcTableDemo>, "ProcTableDemo must satisfy Program");

int main() {
    run<ProcTableDemo>({.title = "proc_table"});
}
