// Minimal inline rendering test
#include <maya/maya.hpp>
#include <maya/dsl.hpp>
#include <maya/components/components.hpp>

using namespace maya;
using namespace maya::dsl;
using namespace maya::components;

int main() {
    int frame = 0;
    run(
        {.fps = 30, .alt_screen = false},
        [&](const Event& ev) {
            return !key(ev, 'q') && !key(ev, SpecialKey::Escape);
        },
        [&] {
            frame++;
            return vstack().gap(1)(
                MessageBubble({
                    .role = Role::User,
                    .content = "Hello world test message",
                    .timestamp = "12:00",
                }),
                text("Frame " + std::to_string(frame), Style{}.with_fg(palette().muted)),
                StatusBar(StatusBarProps{.sections = {
                    {.content = " test", .color = palette().primary, .bold = true},
                    {.content = "frame " + std::to_string(frame), .color = palette().muted},
                }})
            );
        }
    );
}
