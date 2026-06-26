# Animation

maya ships a complete, declarative animation **framework**. The design goal is
that animating *anything* — a colour fade, a slide-in, a staggered list, a
multi-step intro, a streaming typewriter — is a few lines that read like
intent, with the library owning **time, ticking, frame-request cadence, and
lifecycle**. A widget author never writes frame plumbing again:

> **No clock. No `dt`. No `request_animation_frame()` in widget code.**

There are two headers:

- **`maya/core/animation.hpp`** — the **math**: easing curves, `lerp`,
  `Tween`, `Spring`, `Animated`, and `RateCursor` (the streaming-typewriter
  integrator). Pure, allocation-free value types you *can* tick by hand.
- **`maya/core/motion.hpp`** — the **framework** around that math: `Clock`,
  `Motion<T>`, `pulse()`, `Timeline`, `Stagger`, `Mount`. These own the clock,
  the tick, and the frame request so you just declare and read.

A third header, **`maya/anim/text_reveal.hpp`**, lifts the streaming reveal
("text materialising in real time") into a reusable decorator any widget can
call on any text leaf.

Everything is header-only, `constexpr` where the math allows, and
allocation-free on the steady-state path. Reading a settled `Motion` is a
single branch; an in-flight one costs one `tick()` + one bool. You can hold
hundreds.

---

## Mental model: who owns time?

Before the framework, every animated widget repeated the same three chores by
hand:

```cpp
auto now = steady_clock::now();                 // 1. read the clock
double dt = (now - last_) / 1000.0; last_ = now; //    compute + clamp dt
value_.tick(dt);                                // 2. tick the math
if (!value_.done()) request_animation_frame();  // 3. keep the loop awake
```

…copy-pasted into composer, welcome screen, markdown, spinner — each with its
own subtly different remount / dropped-frame / throttle logic.

`motion.hpp` folds all of that into one place. The two layers:

| Layer | You write… | The framework owns… |
|-------|-----------|---------------------|
| `animation.hpp` (math) | `tick(dt)`, read the clock yourself | nothing — pure value types |
| `motion.hpp` (framework) | `m.to(target); m.get();` | clock, dt, frame request, lifecycle |

Reach for `motion.hpp` 95% of the time. Drop to `animation.hpp` only when you
need a value type with no host loop (off-screen scratch, a custom integrator,
unit tests).

---

## Easing curves

`maya::anim::ease` is a set of pure `constexpr` functions mapping a normalised
parameter `t ∈ [0,1]` to a shaped progress. Because they're `constexpr`, you
can `static_assert` on them:

```cpp
static_assert(ease::in_out_cubic(0.5) == 0.5);
```

### Catalogue

| Function | Shape | Use for |
|----------|-------|---------|
| `linear` | straight line | constant-speed motion, colour mixes |
| `in_quad` / `out_quad` / `in_out_quad` | gentle accel / decel | subtle fades |
| `in_cubic` / `out_cubic` / `in_out_cubic` | stronger accel / decel | the workhorse UI curve |
| `in_out_quint` | snappy ends, slow middle | "settle" feel |
| `smoothstep` / `smootherstep` | Perlin S-curves, no overshoot | shimmer, breathe, clamped quantities |
| `in_back` / `out_back` | anticipation / overshoot | the classic UI "pop" |

!!! warning "`back` curves escape `[0,1]`"
    `in_back` dips **below 0** at the start (wind-up); `out_back` overshoots
    **past 1** near the end then settles (snap into place). That's the point —
    callers lerping a *position* get the overshoot. But callers lerping a
    **clamped** quantity (an alpha, a colour mix) should prefer `out_cubic`,
    or the overshoot produces out-of-range values.

### The `Fn` alias

Widgets that accept a curve as a parameter take an `ease::Fn`:

```cpp
using Fn = double (*)(double) noexcept;   // a function pointer to any ease

void slide_in(ease::Fn curve = ease::out_back);
```

---

## `lerp` — the interpolation primitive

```cpp
template <typename T> requires std::is_arithmetic_v<T>
constexpr T lerp(T a, T b, double t) noexcept;     // numbers

Color lerp(Color a, Color b, double t) noexcept;   // truecolor, per-channel
```

The colour overload mixes the RGB channels componentwise (gamma-naive — fine
for UI fades) and rounds to nearest. Named / indexed colours degrade to their
RGB projection. To make a custom `T` animatable, add a `lerp(T, T, double)`
overload (found by ADL).

```cpp
Color hot = lerp(Color::rgb(255, 90, 200), Color::rgb(120, 230, 255), 0.4);
```

---

## `Tween<T>` — fixed-duration eased interpolation

A deterministic A→B interpolation through an easing curve. Ends **exactly** at
`target` after `duration` seconds.

```cpp
Tween<double> t{0.0, 1.0, 0.3, ease::out_cubic};  // 0→1 over 300ms
for (...) {
    double v = t.tick(dt);   // advance, returns current value
    if (t.done()) break;
}
```

| Method | Effect |
|--------|--------|
| `tick(dt)` | advance by `dt` seconds, returns current value |
| `value()` | current value without advancing |
| `done()` | true once `elapsed ≥ duration` |
| `retarget(to, dur, curve?)` | continue from the **current** value toward a new target — no visual jump |
| `reset_to(v)` | jump to `v`, kill motion |
| `progress()` | eased progress in `[0,1]` |

`retarget` is what keeps a re-aimed tween continuous mid-flight: it snaps the
`from_` to the live value, so the value never teleports.

---

## `Spring<T>` — physically-modelled motion

A semi-implicit (symplectic) Euler spring. Unlike a tween it has no fixed
duration — it settles asymptotically toward the target, and **retargets
mid-flight without a discontinuity** because velocity is preserved. That
momentum is what makes spring motion feel *alive*.

```cpp
Spring<double> s{0.0, spring_presets::wobbly};
s.set_target(1.0);          // pull toward 1, keeping any current velocity
double v = s.tick(dt);
bool   settled = s.done();  // within rest_eps of target AND nearly still
```

### `SpringParams` and presets

A spring is `{stiffness, damping, mass, rest_eps}`. Stiffness pulls toward the
target; damping bleeds velocity. The **damping ratio** ζ = c / (2·√(k·m))
characterises the response:

- ζ < 1 — **under-damped**: overshoots and oscillates
- ζ = 1 — **critically damped**: fastest settle, no overshoot
- ζ > 1 — **over-damped**: crawls in slowly

Build springs with the `consteval` factory so a degenerate config is a
**compile error**, not a run-time NaN:

```cpp
constexpr SpringParams custom = spring_presets::make(/*stiffness=*/210,
                                                     /*zeta=*/0.8);
```

Hand-tuned presets (Framer-Motion-ish):

| Preset | Stiffness | ζ | Feel |
|--------|-----------|---|------|
| `gentle` | 120 | 0.90 | soft, no overshoot |
| `snappy` | 210 | 0.80 | quick with a tiny pop |
| `wobbly` | 180 | 0.45 | bouncy, oscillates |
| `stiff` | 300 | 1.00 | fast, critical |
| `molasses` | 60 | 1.20 | slow, heavy |

### Momentum-preserving retarget

`set_target()` reprojects the current velocity onto the new base→target span,
so a spring re-aimed mid-flight carries its momentum into the new motion —
no jump, no dead stop. For `Color`, the velocity is reprojected on the
max-channel-delta span so hue stays coupled.

---

## `Animated<T>` — tween *or* spring, one type

The ergonomic wrapper that holds either mode behind one interface. Construct
with a factory, then `tick`/`value`/`done`/`set_target` uniformly:

```cpp
auto a = Animated<double>::tween(0.0, 1.0, 0.3, ease::out_cubic);
auto b = Animated<Color>::spring(Color::white(), spring_presets::snappy);

a.tick(dt);  b.tick(dt);
a.set_target(0.5, 0.2);            // tween: needs a duration
b.set_target(Color::red());        // spring: duration ignored, momentum kept
```

`Animated` is what `Motion<T>` (below) wraps — but you'll almost always use
`Motion` directly, since it removes the manual `tick`/clock entirely.

---

## `Clock` — the one frame-time source

A `Clock` answers two questions a widget keeps asking:

```cpp
clk.now_ms();   // monotonic ms since the clock's epoch — for phase effects
clk.dt();       // seconds since the previous frame, clamped + remount-aware
```

`dt()` is **cached per frame** (keyed on the integer millisecond), so every
`Motion` reading it within one frame sees the *same* delta — critical for
staggers and parallel timelines to stay in lockstep. It also:

- **clamps dropped frames** to `kMaxStep` so a stall doesn't teleport motion;
- **detects remounts** — a gap longer than `kRemountGapMs` returns `dt = 0`
  so animation eases from rest instead of jumping.

```cpp
Clock& default_clock() noexcept;   // process-wide, thread_local
```

There is one process-wide `default_clock()` backing everything (it's
`thread_local`, so a worker thread that animates an off-screen scratch gets
its own consistent timeline). You rarely touch it directly — `Motion`,
`pulse`, `Timeline`, `Stagger` all read it for you.

!!! danger "Clock separation: don't mix the view loop and a tick subscription"
    `default_clock().dt()` is the **view-loop** frame clock — its internal
    `last_` is bumped by *every* `view()` read (every `Motion`, every
    `pulse()`). If a host also drives a **separate** timer subscription (e.g.
    a 33 ms "tick" that advances a spinner in the reducer), that timer must
    **not** read `default_clock().dt()` for its own delta. During an active
    frame the view reads the clock at ~60 fps, so by the time the timer reads
    `dt()` the frame gap is already consumed — it sees a few stray ms instead
    of the real tick interval, and its animation crawls. A tick-driven value
    should use its own inter-tick wall-clock gap. Keep the two clocks separate.

---

## `Motion<T>` — the headline type

A **self-driving** animated value. Hold one as a (mutable) widget member, set
its target, and read the live value in `build()`. `.get()` owns the entire
frame chore: pull `dt` from the clock, tick the math, and — while still moving
— nudge the run loop for another frame. Once settled, `.get()` is a pure read
with **no** frame request, so the loop returns to idle and you pay nothing.

```cpp
struct Toggle {
    anim::Motion<double> x{0.0};               // tween mode, starts at 0
    void set(bool on) { x.to(on ? 1.0 : 0.0, 0.18); }

    maya::Element build() const {
        int fill = int(x.get() * width);        // ← ticks + requests frames
        return bar(fill);
    }
};
```

### Construction

```cpp
Motion<double> a{0.0};                          // tween, settled at 0
Motion<double> b{0.0, 0.4, ease::out_cubic};    // tween, default 400ms + curve
auto c = Motion<Color>::spring(Color::white(),  // spring mode
                               spring_presets::wobbly);
```

### Targeting

| Call | Mode | Effect |
|------|------|--------|
| `to(target, dur=-1, curve=nullptr)` | tween | retarget over a duration — continues from the live value, no jump. `dur<0` uses the default; `curve=nullptr` keeps the current curve |
| `spring_to(target)` | spring | retarget preserving velocity |
| `snap(v)` | both | jump instantly, kill motion |

### Reading

| Call | Effect |
|------|--------|
| `get()` | **THE** call — ticks against the shared clock, requests a frame while moving, returns the value. Idempotent within a frame (the clock caches `dt`), so reading the same Motion twice in one `build()` does not double-step it |
| `get_on(clk)` | tick against an explicit clock (tests, off-screen scratch) |
| `peek()` | current value without ticking or requesting a frame |
| `target()` / `moving()` | introspection |

That's the whole contract: **`to()` to aim, `get()` to read.** No `dt`, no
clock, no RAF anywhere in your widget.

---

## `pulse()` and friends — perpetual phase

Many "alive" effects — blink, breathe, shimmer, a caret pulse — don't have a
target; they cycle forever off wall-clock. These free functions turn the
clock's `now_ms()` into a normalised phase and request a frame so the cycle
keeps ticking:

```cpp
double p = anim::loop_phase(period_ms);   // sawtooth 0→1, repeating
double b = anim::pulse(1400.0);           // eased triangle 0→1→0 (breathing)
Color c = anim::pulse_between(dim, bright, 1400.0);  // lerp by pulse()
```

`pulse()` is a smoothstep-shaped ping-pong, so it decelerates at both ends —
the natural breathing feel. Each is a pure read of the clock; they hold no
state, so you can sprinkle them inline:

```cpp
auto breath_col = anim::lerp(Color::rgb(60,60,80),
                             Color::rgb(180,220,255), anim::pulse(1400.0));
el | fg(breath_col);
```

---

## `Timeline` — multi-step choreography

For animation a single `Motion` can't express: *"fade in, **then** slide up,
**then** (after 200 ms) pulse"* or *"do A and B in parallel."* A `Timeline` is
a list of **tracks**; each track is a list of **keyframes** with absolute start
times and durations. You describe the schedule once, then `sample()` it each
frame and read each track's value.

```cpp
anim::Timeline tl;

auto opacity = tl.track(0.0);                  // start value 0
opacity.hold(0.0, 0.1)                         // wait 100ms…
       .to(1.0, 0.3, ease::out_cubic);         // …then fade in over 300ms

auto y = tl.track(8.0);                         // start offset 8
y.at(0.1)                                       // jump cursor to t=0.1s…
 .to(0.0, 0.4, ease::out_back);                 // …slide up, overlapping the fade

tl.play();                                      // start / restart the playhead

// in build():
tl.sample();                                    // advance off the clock + RAF
double op = tl.track_at(0).value();
double yy = tl.track_at(1).value();
```

### Track building

| Call | Effect |
|------|--------|
| `track(initial)` | create a track, returns a `Track` handle |
| `hold(value, secs)` | hold `value` for `secs` (a gap before the next move) |
| `to(target, secs, curve)` | move to `target` over `secs` from the cursor |
| `at(abs_secs)` | reposition the cursor to an **absolute** time (for overlapping tracks) |
| `value()` | the track's value at the current playhead |

Tracks are cheap value handles into the timeline's storage. Use `track_at(i)`
in `build()` to re-obtain a handle to a track you built during setup (it does
**not** create a new track).

### Playback

| Call | Effect |
|------|--------|
| `play()` / `stop()` / `seek(s)` | playhead control |
| `sample(clk?)` | advance the playhead off the clock, request a frame while running, returns `done()` |
| `set_loop(on)` | loop the playhead |
| `done()` / `playhead()` / `duration()` | introspection |

---

## `Stagger` — index-phased fan-out

The canonical "list items cascade in" / "menu opens row by row" motion: the
*same* animation applied to N items, each delayed by `step_s · index`.

```cpp
double e = mount.elapsed_ms(700) / 1000.0;     // seconds since the list mounted
for (int i = 0; i < n; ++i) {
    double p = anim::stagger_progress(e, i, /*step=*/0.08, /*dur=*/0.35);
    rows.push_back(row(i) | opacity(p));        // p eases 0→1, offset per item
}
bool finished = anim::stagger_done(e, n, 0.08, 0.35);
```

- `stagger_progress(elapsed_s, index, step_s, dur_s, curve?)` — eased `[0,1]`
  progress for item `i`, clamp-safe at both ends. Item `i` starts at
  `i·step_s`.
- `stagger_done(elapsed_s, count, step_s, dur_s)` — true once every item has
  finished, so the host can stop requesting frames.

---

## `Mount` — "ms since this widget appeared"

Many widgets animate relative to when they **mounted** — a splash that fades
in, a sigil that draws on first appearance, a list that cascades. They replay
a fixed schedule from `t = 0` each time they come on screen.

```cpp
struct Splash {
    mutable anim::Mount mount;
    maya::Element build() const {
        int age = mount.elapsed_ms(1200);                 // animate for 1.2s
        double p = anim::ease::out_cubic(std::min(1.0, age / 1200.0));
        return logo() | opacity(p);
    }
};
```

- `elapsed_ms(animate_for_ms = 0)` — ms since mount. If `animate_for_ms > 0`,
  requests a frame while `age < animate_for_ms` (a one-shot intro that plays
  then idles); if `0`, requests a frame every call (perpetual). A gap longer
  than `kRemountGapMs` (500 ms) is treated as a remount and restarts from 0.
- `remount()` — force the next `elapsed_ms()` to restart from 0.
- `mounted()` — has it started?

`Stagger` is typically driven by a `Mount` clock (the list's mount time).

---

## `RateCursor` — the streaming-typewriter integrator

The shape a **streaming reveal** needs, and that neither `Tween`
(duration-based) nor `Spring` (asymptotic, overshoots) can express: a
**monotone** cursor (think "codepoints revealed so far") chasing a **moving**
target ("codepoints available") at a controlled *rate*.

### The rate-smoothed bounded-lag model

A streaming reveal must satisfy two things that pull against each other, and
the pacing historically pendulum'd between failing each:

- **Keep up** — track the model's speed so the reveal never falls seconds
  behind and dumps the buffered remainder at end-of-stream. A cruise *speed*
  cap can't: a model faster than the cap outruns the cursor all turn.
- **Don't teleport** — never paste a fat chunk in one frame, so the typewriter
  keeps animating even when the wire delivers in bursts (SSE / proxy batching,
  slow links). A *lag* cap that SNAPS the cursor to the edge keeps up, but the
  snap **is** the teleport.

The resolution: the rate that holds the cursor a bounded **time** behind the
live edge is just `backlog / drain_secs_` — a proportional controller. At
steady state the backlog settles at `rate * drain_secs_`, so the cursor moves
at **exactly the model's speed** (keeps up, any speed, fixed `~drain_secs_`
lag). Applied raw it would teleport on a chunky wire, so the **rate itself is
low-passed** over `rate_tau_`: a burst raises the target rate but the cursor
accelerates toward it across a few frames, revealing the chunk as a fast
*animated slide*. The model's speed sets the cruise; `drain_secs_` absorbs
jitter; `rate_tau_` absorbs bursts.

- **`drain_secs_`** is the target lag (seconds behind the edge) *and* the
  buffer window: `rate = backlog / drain_secs_` tracks the wire at that delay.
- **`rate_tau_`** is the anti-teleport low-pass — how fast the glide rate may
  change. Small = snappier on a burst; large = smears it into a slower slide.
- **`floor_rate_`** is a minimum reveal speed so a trickle still types out
  promptly instead of inching in at the wire's dribble.
- An optional **finalize ramp** (`set_deadline`) guarantees the cursor reaches
  the edge by a wall-clock deadline (end-of-stream settle), bypassing the
  smoothing because that's a hard correctness guarantee.

### API

```cpp
RateCursor c{/*cruise=*/45.0, /*lead_secs=*/0.4};

// each frame:
c.advance_floor(committed_cp);     // never lag behind already-committed units
double pos = c.tick(total_cp, dt); // integrate toward the target, returns pos

// settle: reach the edge within `secs` no matter what
c.set_deadline(remaining_s);
```

| Method | Effect |
|--------|--------|
| `tick(target, dt)` | advance toward `target` by `dt`, returns the new position |
| `set_pacing(floor, lead)` | retune the floor speed + lag window |
| `set_smoothing(rate_tau)` | tune how fast the glide rate adapts (anti-teleport) |
| `set_pos(p)` / `advance_floor(p)` | hard-set / monotone snap-forward |
| `set_deadline(secs)` / `clear_deadline()` / `ramping()` | finalize ramp |
| `pos()` / `reset()` | read / reset |

`dt` should be pre-clamped by the host to a sane frame budget so a long stall
doesn't teleport the cursor on the first frame back.

---

## `text_reveal` — the typewriter as a decorator

`maya/anim/text_reveal.hpp` lifts the "text materialising in real time" effect
— the scramble→resolve tip, the hot→cool gradient trail, the ghosted
not-yet-typed body, the bright sweep cursor at the reveal front, and the
pulsing end-caret — into a reusable decorator that operates on the trailing
edge of **any** text leaf.

```cpp
TextElement leaf; leaf.content = body;

anim::TextRevealParams rp;
rp.ms_total    = clock.now_ms();
rp.edge_age_ms = age_of_newest_cp;
rp.revealed_cp = cursor;             // from a RateCursor, typically
rp.total_cp    = total;
anim::decorate_text_reveal(leaf, rp);          // scramble + gradient + ghost

if (caught_up) anim::decorate_end_caret(leaf, rp.ms_total);  // awaiting-byte caret
```

### Height-stability — the load-bearing invariant

`decorate_text_reveal` is **purely visual and height-stable**: it never adds or
removes codepoints (scramble substitutes equal-display-width glyphs; ghost
cells become width-matched spaces), so the leaf's wrapped height is identical
before and after. That is what makes it safe to drop into a widget that commits
rows to native terminal scrollback — it cannot reflow the layout or push
committed rows up.

### The decorator holds no clock and no cursor

Time and cursor state are **inputs**. The caller supplies `ms_total`, the age
of the trailing edge, and how many codepoints are revealed vs total. Pair it
with a `RateCursor` + a per-frame clock read for the full self-driving
experience, or feed it a fixed cursor for a static one-shot.

### `TextRevealParams`

| Field | Meaning |
|-------|---------|
| `ms_total` | monotonic wall-clock ms (drives scramble churn, gradient, pulses) |
| `edge_age_ms` | age of the newest codepoint — 0 = full heat, large = settled (cool) |
| `revealed_cp` / `total_cp` | the cursor: `revealed` are typed, the rest ghosted. Equal ⇒ no ghosting (decoration only) |
| `clip_active` / `clipped_unrevealed_cp` | for hosts that clip visible bytes mid-line (scrollback-safe) — count the within-line cp past the cursor |
| `trail_len` / `scramble_len` / `scramble_ms` / `char_step_ms` / `ghost_extra` | tunables (defaults match the original markdown reveal) |
| `enable_scramble` / `_gradient` / `_ghost` / `_sweep` / `_caret` | master toggles — take just the parts you want |
| `ghost_blank` | **default `true`**: unrevealed cp render as width-matched **spaces** — genuinely invisible, the text materialises out of empty space. `false`: keep the glyph dim (fade-in look) |

### `ghost_blank` — true invisibility vs fade-in

With `ghost_blank = true` (the default), each not-yet-typed codepoint is
replaced by as many spaces as its display width (1 column for ASCII, 2 for
CJK). A space paints nothing, so the cp is genuinely *absent* — the true
left-to-right typewriter — yet the column count is identical, so committed
rows never reflow. With `ghost_blank = false` the real glyph is kept but styled
default-fg + dim; that's only invisible on the terminal **default**
background, so on a themed message background it reads as "already there."
Prefer the default unless you specifically want a fade-up look.

### `clip_text_to_cursor` — the *content-cut* typewriter

For a **generic** (non-scrollback) caller that wants the leaf to physically
grow a codepoint at a time, `clip_text_to_cursor(leaf, revealed_cp)` truncates
the content to the first `revealed_cp` codepoints and clips the runs. Unlike
`decorate_text_reveal` this **changes** the leaf height as the cursor advances
(by design — that's what typing looks like), so it's for interactive / one-shot
UI, **not** the height-stable streaming-markdown path.

### `decorate_end_caret`

Recolours the final codepoint with a magenta↔cyan pulse — the "awaiting next
byte" cue. Height/width-stable (no bytes added when the leaf is non-empty).
Call it **instead** of the sweep cursor once the reveal cursor has caught up to
the edge, so the two don't compete for the eye.

---

## Putting it together

A complete, self-driving streaming reveal — no clock, no `dt`, no RAF in the
widget body:

```cpp
struct StreamView {
    mutable anim::Mount        mount;
    mutable anim::RateCursor   cursor{45.0, 0.4};   // cruise 45 cp/s, 0.4s lead
    std::string                body;                 // grows as bytes arrive

    maya::Element build() const {
        const std::int64_t ms  = anim::default_clock().now_ms();
        const double       dt  = anim::default_clock().dt();
        const std::size_t  tot = utf8_count(body);

        const std::size_t revealed =
            static_cast<std::size_t>(cursor.tick(double(tot), dt));

        TextElement leaf; leaf.content = body;
        anim::TextRevealParams rp;
        rp.ms_total    = ms;
        rp.edge_age_ms = mount.elapsed_ms();   // newest-cp age (approx) + RAF
        rp.revealed_cp = revealed;
        rp.total_cp    = tot;
        anim::decorate_text_reveal(leaf, rp);
        if (revealed >= tot) anim::decorate_end_caret(leaf, ms);

        // mount.elapsed_ms() above already re-armed the frame while animating;
        // request_animation_frame() directly works too if you don't hold a Mount.
        return maya::Element{std::move(leaf)};
    }
};
```

See **`examples/motion_showcase.cpp`** for a one-screen tour of every layer —
spring slider, colour tween, `pulse()` breathing, orbiting dots, spinners, an
eased progress bar, a bouncing spring, wave text, a matrix-rain column, a
two-track timeline, a stagger cascade, and the live `text_reveal` typewriter —
all declarative, with **no clock, no `dt`, no `request_animation_frame`
anywhere in the file**.

---

## Performance notes

- **Allocation-free steady state.** `Tween`/`Spring`/`Animated`/`RateCursor`
  are trivially-copyable value types. A settled `Motion::get()` is one branch.
- **One dt per frame.** `Clock::dt()` is cached on the integer millisecond, so
  200 `Motion`s reading it in one `build()` each see the same delta with one
  real computation.
- **Idle is free.** A `Motion` only requests a frame while *moving*; once
  settled the loop returns to event-driven idle and you pay nothing.
- **`decorate_text_reveal` is cheap and width-stable.** It rewrites only the
  trailing `trail_len` runs and re-wraps the leaf **only** when scramble could
  have changed display width — blanking preserves width exactly, so the common
  frame skips the re-wrap entirely.
