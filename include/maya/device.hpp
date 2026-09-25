#pragma once
// maya/device.hpp — the terminal device's vocabulary, in one include:
//
//   device/options.hpp        Mode, RenderBackend, Options
//   device/frame_request.hpp  request_animation_frame (widgets ask; the host draws)
//   device/keys.hpp           navigation keys; key_is / ctrl_is / alt_is
//   device/internals.hpp      detail::Device, the device's state
//   device/theme_canvas.hpp   theme background fill, app_set_theme
//
// The device is what maya can do to a terminal. It has no loop and no
// policy: <maya/screen.hpp> is its public face, and a runtime drives that.
// To write an app, include <maya/host/run.hpp>.

#include "device/options.hpp"
#include "device/frame_request.hpp"
#include "device/keys.hpp"
#include "device/internals.hpp"
#include "device/theme_canvas.hpp"
