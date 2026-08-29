#pragma once
// maya::widget::OutputChannel — log output pane (background-free)
//
// The Output panel: a header showing the selected channel (dropdown affordance)
// and a stream of log lines coloured by level (info / warn / error / debug /
// trace) with an optional dim timestamp per line.
//
// Usage:
//   OutputChannel o{"C/C++ · clangd"};
//   o.info("indexed 1204 files").warn("no compile_commands.json")
//    .error("unresolved include <foo.h>", "12:04:31");
//   Element ui = o | dsl::grow();

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct OutputChannelTheme {
    Color header = Color::hex(0xBAC2DE);
    Color chevron = Color::hex(0x585B70);
    Color time    = Color::hex(0x494D64);
    Color info    = Color::hex(0x9399B2);
    Color warn    = Color::hex(0xE2B341);
    Color error   = Color::hex(0xF38BA8);
    Color debug   = Color::hex(0x6C7086);
    Color trace    = Color::hex(0x585B70);
};

class OutputChannel {
public:
    enum Level : uint8_t { Info, Warn, Error, Debug, Trace };

    explicit OutputChannel(std::string channel = "Output") : channel_(std::move(channel)) {}

    OutputChannel& log(Level lv, std::string text, std::string ts = {}) {
        lines_.push_back({lv, std::move(text), std::move(ts)}); return *this;
    }
    OutputChannel& info(std::string t, std::string ts = {})  { return log(Info, std::move(t), std::move(ts)); }
    OutputChannel& warn(std::string t, std::string ts = {})  { return log(Warn, std::move(t), std::move(ts)); }
    OutputChannel& error(std::string t, std::string ts = {}) { return log(Error, std::move(t), std::move(ts)); }
    OutputChannel& debug(std::string t, std::string ts = {}) { return log(Debug, std::move(t), std::move(ts)); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        {   // channel header (dropdown)
            std::string s; std::vector<StyledRun> r;
            r.push_back({0, channel_.size(), Style{}.with_fg(theme.header).with_bold()}); s = channel_;
            std::string chev = "  \xef\x84\x87"; //  chevron-down
            r.push_back({s.size(), chev.size(), Style{}.with_fg(theme.chevron)}); s += chev;
            rows.push_back(Element{TextElement{ .content=std::move(s), .style=Style{},
                                                .wrap=TextWrap::NoWrap, .runs=std::move(r) }});
        }
        for (const auto& l : lines_) rows.push_back(line_row(l));
        return dsl::v(std::move(rows)).build();
    }

private:
    struct L { Level lv; std::string text, ts; };
    std::string    channel_;
    std::vector<L> lines_;
    OutputChannelTheme theme;

    Color lc(Level lv) const {
        switch (lv) { case Warn: return theme.warn; case Error: return theme.error;
                      case Debug: return theme.debug; case Trace: return theme.trace;
                      default: return theme.info; }
    }
    const char* tag(Level lv) const {
        switch (lv) { case Warn: return "[warn] "; case Error: return "[error] ";
                      case Debug: return "[debug] "; case Trace: return "[trace] ";
                      default: return "[info] "; }
    }

    Element line_row(const L& l) const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        if (!l.ts.empty()) put(l.ts + " ", Style{}.with_fg(theme.time));
        put(tag(l.lv), Style{}.with_fg(lc(l.lv)).with_bold());
        put(l.text, Style{}.with_fg(l.lv == Error || l.lv == Warn ? lc(l.lv) : theme.info));
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
