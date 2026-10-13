// maya_standalone_tests — one binary, argv-dispatched, for maya's tests that
// keep their own int main() (multi-phase programs, the streaming reveal/lag
// probes shared with agentty's suite, and the benchmark). Each still runs in
// its OWN process per ctest entry:
//     maya_standalone_tests <name> [args…]
// so isolation is unchanged — but 8 links collapse into 1.
//
// Each test's main() is renamed to <name>_main at compile time via a per-source
// -Dmain=<name>_main (see the maya CMakeLists); we declare and dispatch them
// here. Every one of these takes no arguments (verified), so the table is a
// single flat X-macro list.
#include <cstdio>
#include <string_view>

#define MAYA_FOLD(name) extern int name##_main();
#include "maya_standalone_tests.def"
#undef MAYA_FOLD

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <test-name>\n\nAvailable tests:\n", argv[0]);
#define MAYA_FOLD(name) std::fprintf(stderr, "  %s\n", #name);
#include "maya_standalone_tests.def"
#undef MAYA_FOLD
        return 2;
    }
    const std::string_view which{argv[1]};

#define MAYA_FOLD(name) if (which == #name) return name##_main();
#include "maya_standalone_tests.def"
#undef MAYA_FOLD

    std::fprintf(stderr, "%s: unknown test '%.*s'\n",
                 argv[0], static_cast<int>(which.size()), which.data());
    return 2;
}
