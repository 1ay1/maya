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
//   • rich cells     USER carries a warm root tint; NAME = bright name +
//                    dim argv trail in one cell (spans clip with the
//                    truncation); the CPU bar is a TableCell::dyn painted
//                    over a dim track at the SOLVED width; MEM / MEM% are
//                    pressure-tinted, DISK shows a rate or a ·; the active
//                    sort column's values brighten into a spine; the
//                    hottest row wears a » edge marker
//   • ink tiers      per-column header_style: sortable columns read as
//                    clickable, the meter + S columns recede (inert),
//                    the active sort header shouts (sort_header_style);
//                    MEM and MEM% share ONE sort key via hit_index
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
    std::string user;
    std::string name;
    double cpu;
    int mem_mb;
    double mem_pct;
    int disk_kb;      // I/O rate; 0 = idle
    int threads;
    char state;
};

std::vector<Proc> fake_procs() {
    struct Seed { const char* user; const char* name; };
    const Seed seeds[] = {
        {"root",  "systemd"}, {"ayush", "rb"}, {"ayush", "agentty"},
        {"ayush", "kitty"}, {"ayush", "chrome --type=renderer"},
        {"ayush", "tmux: server"}, {"ayush", "pipewire-pulse"},
        {"root",  "kworker/u50:11"}, {"root", "sshd"}, {"ayush", "bash"},
        {"ayush", "htop"}, {"root", "dockerd"}, {"postgres", "postgres"},
        {"ayush", "nvim"}, {"ayush", "firefox"}, {"ayush", "cc1plus"},
        {"ayush", "make"}, {"ayush", "git"}, {"ayush", "node"},
        {"ayush", "cargo"}, {"ayush", "rust-analyzer"}, {"ayush", "clangd"},
        {"root",  "Xorg"}, {"ayush", "pulseaudio"},
    };
    std::vector<Proc> ps;
    int i = 0;
    for (const auto& s : seeds) {
        const int mem = 16 + (i * 251) % 2048;
        ps.push_back({900000 + i * 137, s.user, s.name,
                      (i * 73 % 970) / 10.0,
                      mem,
                      mem / 320.0,                         // fake % of 32G
                      i % 4 == 0 ? (i * 811) % 4096 : 0,   // some do I/O
                      1 + i % 12,
                      i % 3 == 0 ? 'R' : i % 7 == 0 ? 'D' : 'S'});
        ++i;
    }
    return ps;
}

std::string fmt1(double v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

std::string human_rate(int kb) {
    char buf[24];
    if (kb >= 1024) std::snprintf(buf, sizeof buf, "%.1fM/s", kb / 1024.0);
    else            std::snprintf(buf, sizeof buf, "%dK/s", kb);
    return buf;
}

// green → yellow → red for a 0..1 load fraction.
Color load_color(double f) {
    if (f < 0.5) return Color::hex(0x50FA7B);
    if (f < 0.8) return Color::hex(0xF1FA8C);
    return Color::hex(0xFF5555);
}

// An eighth-block bar painted at the column's solved width, over a dim
// track so the meter reads as a gauge even when the load is low.
TableCell meter_cell(double frac) {
    return TableCell::dyn([frac](int w) {
        static const char* kEighths[] =
            {"", "\u258f", "\u258e", "\u258d", "\u258c", "\u258b", "\u258a", "\u2589"};
        const double cells = frac * w;
        const int full = static_cast<int>(cells);
        const int frac8 = static_cast<int>((cells - full) * 8.0);
        const Color fg = load_color(frac);
        TableCell c;
        std::string bar;
        int drawn = 0;
        for (int i = 0; i < full && i < w; ++i) { bar += "\u2588"; ++drawn; }
        if (full < w && frac8 > 0) { bar += kEighths[frac8]; ++drawn; }
        c.span(bar, Style{}.with_fg(fg));
        // Dim track fills the remainder so the column always reads as a bar.
        std::string track;
        for (int i = drawn; i < w; ++i) track += "\u2591";   // ░
        c.span(track, Style{}.with_fg(Color::hex(0x2E3140)));
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
    struct Mouse { MouseEvent ev; };
    struct Quit {};
    using Msg = std::variant<Key, Mouse, Quit>;

    // Hit kinds for the row / header click rects the Table registers.
    static constexpr std::uint32_t kHitRow    = 1;
    static constexpr std::uint32_t kHitHeader = 2;

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
                    case 1: return a.user < b.user;
                    case 2: return a.name < b.name;
                    case 3: return a.cpu < b.cpu;
                    case 4: return a.mem_mb < b.mem_mb;
                    case 5: return a.disk_kb < b.disk_kb;
                    case 6: return a.threads < b.threads;
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
        const int n = static_cast<int>(m.procs.size());

        if (const auto* mo = std::get_if<Mouse>(&msg)) {
            // Resolve the click against the rects the Table registered this
            // frame: a row selects, a header cell sorts on that column's key
            // (toggling direction if it's already the sort column).
            const auto& me = mo->ev;
            if (me.kind != MouseEventKind::Press) return {std::move(m), C{}};
            if (me.button == MouseButton::Left) {
                if (const auto hit = hit_test(me.x.value, me.y.value)) {
                    if (hit_kind(*hit) == kHitRow) {
                        m.cursor = std::clamp(static_cast<int>(hit_index(*hit)),
                                              0, n - 1);
                    } else if (hit_kind(*hit) == kHitHeader) {
                        const int key = static_cast<int>(hit_index(*hit));
                        if (key == m.sort_col) m.desc = !m.desc;
                        else { m.sort_col = key; m.desc = true; }
                        sort_procs(m);
                    }
                }
            } else if (me.button == MouseButton::ScrollUp) {
                m.cursor = std::clamp(m.cursor - 3, 0, n - 1);
            } else if (me.button == MouseButton::ScrollDown) {
                m.cursor = std::clamp(m.cursor + 3, 0, n - 1);
            }
            return {std::move(m), C{}};
        }

        const auto& ev = std::get<Key>(msg).ev;
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
                    case 's': m.sort_col = (m.sort_col + 1) % 8;
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
        // The model's sort key is semantic (pid/user/name/cpu/mem/disk/thr/
        // state); the BAR column sits at rendered index 3, so keys ≥ 3
        // shift by 1.
        const int sort_render_col = m.sort_col < 3 ? m.sort_col : m.sort_col + 1;
        TableConfig cfg;
        cfg.selectable  = true;
        cfg.sort_col    = sort_render_col;
        cfg.sort_desc   = m.desc;
        cfg.show_status = true;
        cfg.show_border = true;
        cfg.stripe_rows = true;                    // quiet zebra for scanability
        cfg.title       = "PROCESSES";
        cfg.border_color = Color::hex(0x44475A);
        cfg.header_bg    = Color::hex(0x2A2C3A);   // quiet rail band
        cfg.header_style = Style{}.with_bold().with_fg(Color::hex(0x8088A8));
        cfg.sort_header_style =                    // active column shouts
            Style{}.with_bold().with_fg(Color::hex(0xBD93F9));
        cfg.alt_row_style = Style{}.with_bg(Color::hex(0x25262F));  // zebra
        cfg.selected_bg  = Color::hex(0x33354A);   // cursor row strip
        cfg.selected_style = Style{};              // ink stays semantic
        cfg.cursor_bar_color = Color::hex(0xBD93F9);
        cfg.scrollbar_thumb_color = Color::hex(0xBD93F9);
        cfg.scrollbar_track_color = Color::hex(0x3A3D4C);
        cfg.show_separator = false;
        cfg.cell_padding = 0;
        cfg.column_gap   = 1;
        cfg.row_hit_kind    = kHitRow;     // rows register hit_id(kHitRow, i)
        cfg.header_hit_kind = kHitHeader;  // headers → hit_id(kHitHeader, key)

        // Per-column header ink: the SORTABLE columns read as clickable
        // (mid ink); the meter + S columns are inert (dim). The active
        // sort column overrides all of this with sort_header_style.
        const Style sortable = Style{}.with_fg(Color::hex(0x8088A8));
        const Style inert    = Style{}.with_fg(Color::hex(0x555873));
        Table tbl({{.header = "PID", .align = ColumnAlign::Right,
                    .keep = kKeepAlways, .header_style = sortable,
                    .hit_index = 0},
                   {.header = "USER", .keep = 5, .header_style = sortable,
                    .hit_index = 1},
                   {.header = "NAME", .keep = kKeepAlways, .weight = 3.0f,
                    .min_width = 10, .header_style = sortable, .hit_index = 2},
                   {.header = "", .keep = 1, .weight = 1.0f,
                    .min_width = 6, .max_width = 14,
                    .header_style = inert, .hit_index = kNoHeaderHit},  // CPU bar
                   {.header = "CPU%", .align = ColumnAlign::Right,
                    .keep = kKeepAlways, .header_style = sortable,
                    .hit_index = 3},
                   {.header = "MEM", .align = ColumnAlign::Right, .keep = 4,
                    .header_style = sortable, .hit_index = 4},
                   {.header = "MEM%", .align = ColumnAlign::Right, .keep = 6,
                    .header_style = sortable, .hit_index = 4},   // shares MEM key
                   {.header = "DISK", .align = ColumnAlign::Right, .keep = 2,
                    .header_style = sortable, .hit_index = 5},
                   {.header = "THR", .align = ColumnAlign::Right, .keep = 3,
                    .header_style = inert, .hit_index = 6},
                   {.header = "S", .align = ColumnAlign::Center, .keep = 7,
                    .header_style = inert, .hit_index = kNoHeaderHit}}, cfg);

        // Hottest process wears the » marker.
        int hot = 0;
        for (size_t i = 0; i < m.procs.size(); ++i)
            if (m.procs[i].cpu > m.procs[hot].cpu) hot = static_cast<int>(i);

        // The active sort column's VALUES get brighter ink so the column
        // the list is ranked by reads as the spine.
        const int sk = m.sort_col;
        auto spine = [&](int key, Color base) {
            return sk == key ? Style{}.with_bold().with_fg(base)
                             : Style{}.with_fg(base);
        };

        std::vector<TableRow> rows;
        rows.reserve(m.procs.size());
        for (size_t i = 0; i < m.procs.size(); ++i) {
            const auto& p = m.procs[i];
            const double frac = std::min(1.0, p.cpu / 100.0);
            TableRow row;

            row.cells.emplace_back(std::to_string(p.pid),
                                   spine(0, Color::hex(0x6272A4)));
            // root processes wear a warm tint on the user cell.
            const bool root = p.user == "root";
            row.cells.emplace_back(p.user,
                root ? Style{}.with_fg(Color::hex(0xFFB86C))
                     : spine(1, Color::hex(0x8088A8)));
            // NAME: bright command + dim argv trail, ONE cell — the spans
            // clip together with the … truncation.
            TableCell name;
            const auto sp = p.name.find(' ');
            const Style name_st = spine(2, Color::hex(0xF8F8F2));
            if (sp == std::string::npos) {
                name.span(p.name, name_st);
            } else {
                name.span(p.name.substr(0, sp), name_st);
                name.span(p.name.substr(sp), Style{}.with_fg(Color::hex(0x6C7086)));
            }
            row.cells.push_back(std::move(name));
            row.cells.push_back(meter_cell(frac));
            row.cells.emplace_back(fmt1(p.cpu),
                                   sk == 3 ? Style{}.with_bold().with_fg(load_color(frac))
                                           : Style{}.with_fg(load_color(frac)));
            // MEM value tinted by pressure; the dim "M" unit rides along.
            const Color mem_c = p.mem_pct > 4 ? Color::hex(0xFF79C6)
                              : p.mem_pct > 2 ? Color::hex(0xBD93F9)
                                              : Color::hex(0x8088A8);
            row.cells.emplace_back(std::to_string(p.mem_mb) + "M", spine(4, mem_c));
            row.cells.emplace_back(fmt1(p.mem_pct), spine(4, mem_c));
            row.cells.emplace_back(
                p.disk_kb ? human_rate(p.disk_kb) : std::string("\u00b7"),
                p.disk_kb ? spine(5, Color::hex(0x8BE9FD))
                          : Style{}.with_fg(Color::hex(0x44475A)));
            row.cells.emplace_back(std::to_string(p.threads),
                                   Style{}.with_fg(Color::hex(0x6C7086)));
            const Color st_c = p.state == 'R' ? Color::hex(0x50FA7B)
                             : p.state == 'D' ? Color::hex(0xFF5555)
                                              : Color::hex(0x6C7086);
            row.cells.emplace_back(std::string(1, p.state),
                                   Style{}.with_bold().with_fg(st_c));
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
              text("click", Style{}.with_bold()), text("·row/header  ", Style{}.with_dim()),
              text("q", Style{}.with_bold()), text("·quit", Style{}.with_dim()))
                | height(1)
        );
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return Sub<Msg>::batch({
            Sub<Msg>::on_key([](const KeyEvent& ev) -> std::optional<Msg> {
                if (const auto* ck = std::get_if<CharKey>(&ev.key);
                    ck && ck->codepoint == 'q')
                    return Quit{};
                return Key{ev};
            }),
            Sub<Msg>::on_mouse([](const MouseEvent& me) -> std::optional<Msg> {
                return Mouse{me};
            }),
        });
    }
};

static_assert(Program<ProcTableDemo>, "ProcTableDemo must satisfy Program");

int main() {
    run<ProcTableDemo>({.title = "proc_table", .mouse = true});
}
