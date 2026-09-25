#pragma once
// maya/app/app.hpp — the terminal device's vocabulary, in one include:
//
//   options.hpp        Mode, RenderBackend, Options
//   frame_request.hpp  request_animation_frame (widgets ask; the host draws)
//   keys.hpp           navigation keys; key_is / ctrl_is / alt_is
//   device.hpp         detail::Device, the device internals
//   theme_canvas.hpp   theme background fill, app_set_theme
//
// Not a runtime: <maya/app.hpp> (one level up) runs programs through jaal.

#include "options.hpp"
#include "frame_request.hpp"
#include "keys.hpp"
#include "device.hpp"
#include "theme_canvas.hpp"
