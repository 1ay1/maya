// tests/compile_fail/seam.cpp — mistakes at the maya/jaal seam that must
// not compile. Each MAYA_CASE is one mistake; CASE 0 is the control (it must
// compile, so a case can't "pass" because the file is broken).
#include <maya/host/run.hpp>

#include <optional>
#include <variant>

using namespace maya;

struct Quit {};
using Msg = std::variant<Quit>;

#if MAYA_CASE == 0
// control: a well-formed program
struct App {
    struct Model {};
    using Msg = ::Msg;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }
    static Element view(const Model&) { return dsl::text("x"); }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{{'q', Quit{}}, {SpecialKey::Tab, Quit{}}};
        return keys(k);
    }
};
int main() { return run<App>({}); }

#elif MAYA_CASE == 1
// '\t' arrives as SpecialKey::Tab: this entry could never fire
const keymap<Msg> keys{{'\t', Quit{}}};

#elif MAYA_CASE == 2
// ESC arrives as SpecialKey::Escape
const keymap<Msg> keys{{'\x1b', Quit{}}};

#elif MAYA_CASE == 3
// Ctrl+A arrives as CharKey 'a' with ctrl set, never as byte 0x01
const keymap<Msg> keys{{'\x01', Quit{}}};

#elif MAYA_CASE == 4
// Event lists something the terminal never sends
struct Tick {};
struct App {
    struct Model {};
    using Msg = ::Msg;
    using Cmd = jaal::Cmd<Msg>;
    using Event = std::variant<KeyEvent, Tick>;
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }
    static Element view(const Model&) { return dsl::text("x"); }
    static std::optional<Msg> on(const Model&, const KeyEvent&) { return Quit{}; }
    static std::optional<Msg> on(const Model&, const Tick&) { return Quit{}; }
};
int main() { return run<App>({}); }

#elif MAYA_CASE == 5
// on() takes a type the terminal never sends: every key would be dropped
struct MyKey { char c; };
struct App {
    struct Model {};
    using Msg = ::Msg;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }
    static Element view(const Model&) { return dsl::text("x"); }
    static std::optional<Msg> on(const Model&, const MyKey&) { return Quit{}; }
};
int main() { return run<App>({}); }

#elif MAYA_CASE == 6
// Event lists MouseEvent but there is no on() for it: clicks would be dropped
struct App {
    struct Model {};
    using Msg = ::Msg;
    using Cmd = jaal::Cmd<Msg>;
    using Event = std::variant<KeyEvent, MouseEvent>;
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }
    static Element view(const Model&) { return dsl::text("x"); }
    static std::optional<Msg> on(const Model&, const KeyEvent&) { return Quit{}; }
};
int main() { return run<App>({}); }

#elif MAYA_CASE == 7
// the router API is gone; old code must not quietly compile to "no input"
struct App {
    struct Model {};
    using Msg = ::Msg;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;
};

#endif
