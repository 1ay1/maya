// proc_table.cpp — the rockbottom-shaped process list on maya::Table.
//
// Everything a real proc list needs, all from ONE widget:
//
//   • selection      ▎ cursor bar + row strip (selected_bg), ↑↓/j/k,
//                    PgUp/PgDn, g/G
//   • windowing      the body shows however many rows FIT the pane —
//                    tbl.build() | grow(1) and the count falls out of
//                    the layout; a ▐ scrollbar gutter tracks the window
//   • sorting        s cycles the sort column, r flips direction — the
//                    host sorts its data, the table shows ▾/▴ + accent
//   • responsive     ONE solve_columns plan feeds header + rows: NAME
//                    flexes (weight, truncates with …), the meter
//                    breathes min→max, low-keep columns shed whole
//   • rich cells     NAME = bright name + dim argv trail in one cell
//                    (spans clip with the truncation); the CPU bar is
//                    a TableCell::dyn painted at the SOLVED width; the
//                    hottest row wears a » edge marker
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

// green → yellow → red for a 0..1 load fraction.
Color load_color(double f) {
    if (f < 0.5) return Color::hex(0x50FA7B);
    if (f < 0.8) return Color::hex(0xF1FA8C);
    return Color::hex(0xFF5555);
}

// An eighth-block bar painted at the column's solved width.
TableCell meter_cell(double frac) {
    return TableCell::dyn([frac](int w) {
        static const char* kEighths[] =
            {"", "\u258f", "\u258e", "\u258d", "\u258c", "\u258b", "\u258a", "\u2589"};
        const double cells = frac * w;
        const int full = static_cast<int>(cells);
        const int frac8 = static_cast<int>((cells - full) * 8.0);
        TableCell c;
        std::string bar;
        for (int i = 0; i < full && i < w; ++i) bar += "\u2588";
        if (full < w && frac8 > 0) bar += kEighths[frac8];
        c.span(bar, Style{}.with_fg(load_color(frac)));
        return c;
    });
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
        // The model's sort key is semantic (pid/name/cpu/mem/thr/state);
        // the BAR column sits at rendered index 2, so keys ≥ 2 shift by 1.
        const int sort_render_col = m.sort_col < 2 ? m.sort_col : m.sort_col + 1;
        TableConfig cfg;
        cfg.selectable  = true;
        cfg.sort_col    = sort_render_col;
        cfg.sort_desc   = m.desc;
        cfg.show_status = true;
        cfg.show_border = true;
        cfg.stripe_rows = false;
        cfg.title       = "PROCESSES";
        cfg.border_color = Color::hex(0x44475A);
        cfg.header_bg    = Color::hex(0x2A2C3A);   // quiet rail band
        cfg.selected_bg  = Color::hex(0x33354A);   // cursor row strip
        cfg.selected_style = Style{};              // ink stays semantic
        cfg.cursor_bar_color = Color::hex(0xFFB86C);
        cfg.show_separator = false;
        cfg.cell_padding = 0;
        cfg.column_gap   = 1;

        Table tbl({{.header = "PID", .align = ColumnAlign::Right,
                    .keep = kKeepAlways},
                   {.header = "NAME", .keep = kKeepAlways, .weight = 3.0f,
                    .min_width = 10},
                   {.header = "", .keep = 1, .weight = 1.0f,
                    .min_width = 6, .max_width = 14,
                    .hit_index = kNoHeaderHit},           // the CPU bar
                   {.header = "CPU%", .align = ColumnAlign::Right,
                    .keep = kKeepAlways},
                   {.header = "MEM", .align = ColumnAlign::Right, .keep = 3},
                   {.header = "THR", .align = ColumnAlign::Right, .keep = 2},
                   {.header = "S", .keep = 4}}, cfg);

        // Hottest process wears the » marker.
        int hot = 0;
        for (size_t i = 0; i < m.procs.size(); ++i)
            if (m.procs[i].cpu > m.procs[hot].cpu) hot = static_cast<int>(i);

        std::vector<TableRow> rows;
        rows.reserve(m.procs.size());
        for (size_t i = 0; i < m.procs.size(); ++i) {
            const auto& p = m.procs[i];
            const double frac = std::min(1.0, p.cpu / 100.0);
            TableRow row;

            row.cells.emplace_back(std::to_string(p.pid),
                                   Style{}.with_fg(Color::hex(0x6272A4)));
            // NAME: bright command + dim argv trail, ONE cell — the spans
            // clip together with the … truncation.
            TableCell name;
            const auto sp = p.name.find(' ');
            if (sp == std::string::npos) {
                name.span(p.name, Style{}.with_fg(Color::hex(0xF8F8F2)));
            } else {
                name.span(p.name.substr(0, sp),
                          Style{}.with_fg(Color::hex(0xF8F8F2)));
                name.span(p.name.substr(sp), Style{}.with_dim());
            }
            row.cells.push_back(std::move(name));
            row.cells.push_back(meter_cell(frac));
            row.cells.emplace_back(fmt1(p.cpu),
                                   Style{}.with_fg(load_color(frac)));
            row.cells.emplace_back(std::to_string(p.mem_mb) + "M");
            row.cells.emplace_back(std::to_string(p.threads),
                                   Style{}.with_dim());
            row.cells.emplace_back(std::string(1, p.state),
                                   p.state == 'R'
                                       ? Style{}.with_fg(Color::hex(0x50FA7B))
                                       : Style{}.with_dim());
            if (static_cast<int>(i) == hot) {
                row.edge = "\u00bb";
                row.edge_color = Color::hex(0xFF5555);
            }
            rows.push_back(std::move(row));
        }
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
