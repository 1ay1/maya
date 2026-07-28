#include "maya/app/inline.hpp"
#include "maya/dsl.hpp"
#include <maya/maya.hpp>

using namespace maya;
using namespace maya::dsl;


int main()
{
    constexpr auto ui = v(
        t<"Hello, maya"> | Bold | Fg<100, 180, 255>,
        t<"a tiny banner"> | Dim
    ) | border_<Round> | pad<1>;

    print(ui.build());
}
