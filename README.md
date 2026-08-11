# Better Line

An OFX plugin for DaVinci Resolve Studio that draws editable, styleable, animatable
lines and curves.

It is deliberately a primitive rather than a feature. A styled polyline covers a
surprising amount of a motion-design toolkit: an underline, a connector between two
things, a hand-drawn highlight box, a circle round the thing, an arrow, a border, a
progress rule. Resolve has no native equivalent that is both editable on the viewer and
animatable with real easing.

**Status: Phase 5 of 9.** The rasterizer core, the parameter model, both render paths and the
viewer overlay are built. The rasterizer is verified offline and CPU/GPU parity is under
test; **the overlay is not verified at all** — it needs a draw context and pen events, so
there is no way to exercise it outside Resolve. See
[Phase 0 is not finished](#phase-0-is-not-finished).

---

## Build

Requires Visual Studio with the C++ toolset, and CUDA for the GPU path (optional today,
required from Phase 4).

```bash
powershell -File scripts/build.ps1 -Config Release
```

The OFX SDK is **vendored** in `third_party/openfx`, copied from
`C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX` — ProgramData,
not Program Files.

Blackmagic's copy is used rather than upstream OpenFX for one specific reason:
`ofxsImageEffect.cpp:2084` **binds `OFX::Private::gDrawSuite`**. Upstream ships
`ofxDrawSuite.h` but never fetches it, which forces a plugin to wrap the interact main
entry by hand purely to steal `kOfxInteractPropDrawContext` out of the draw action's
`inArgs` (this is what `resolve-text-animator` has to do). Vendoring Blackmagic's copy
deletes that shim before it is written.

## Install

```bash
powershell -File scripts/install.ps1 -Build
```

Prompts for elevation, since `C:\Program Files\Common Files\OFX\Plugins` needs it. Then:

1. **Fully restart Resolve** — it scans for OFX plugins only at launch.
2. Apply **Better Line** (group `BetterLine`) from the OpenFX library.
3. Under the Viewer, set the on-screen-control dropdown to **"Open FX Overlay"**.
   Resolve hides OFX overlays until you do. This is the single most common
   "the plugin is broken" report, and it is never the plugin.

Requires Resolve **Studio**; the free edition loads no third-party OFX at all.

---

## Testing without Resolve

```bash
build/Release/linetest.exe build/linetest.png
```

Renders a twelve-case contact sheet — caps, bezier flattening, taper, dashes, outline,
glow, gradient, trim, closed paths, and a sub-pixel hairline — over a checkerboard so
alpha reads as alpha rather than silently as black. This is where antialiasing quality
and dash boundaries get judged, because judging them by scrubbing a timeline is slow and
you cannot diff two timelines.

`--cell 512` scales the *geometry* rather than the pixel sizes, so a bigger cell is a
zoom: antialiasing can be inspected closely without the line also getting fatter.

Note this is the **CPU** path. The CUDA kernel is a separate *compilation* of the same
header, so a `linetest` render is not proof the two agree:

```bash
build/Release/cudatest.exe
```

`src/core` being header-only means both paths compile the same `ShadePixel`, but "same
source" is not "same result" — nvcc reassociates float arithmetic. In Resolve every frame
goes through the kernel, so a divergence `linetest` cannot see is one nobody sees until it
ships. The suite covers the degenerate branches deliberately (zero feather, sub-pixel
width, a fully collapsed trim that emits nothing, butt-capped dashes on a curve), composites
over a **non-black** source so premultiplication mistakes cannot hide behind a zero, and
runs on the **null stream** on purpose to exercise the `cudaDeviceSynchronize` branch.

Tolerance is 5e-6 against a measured worst of 2.3e-6. It earned its keep immediately: at a
looser 1e-5 it would have missed the cap-plane cancellation bug below.

One case is allowed a small number of boundary samples, and the reason is worth stating
because it is a property of the design rather than a bug. Gradient coordinates are taken from
the nearest primitive; where an arrowhead overlaps its own shaft, both contain the pixel,
their coordinate frames genuinely differ, and which is nearer is decided by about 1e-7 — so a
thin locus lands on either side depending on how each compiler rounds. Measured at 53 samples
of 65536 (~13 pixels), worst 1.2e-2 in one channel, under half a step of 8-bit output.

It cannot be removed by eliminating the overlap: two SDF shapes that merely *touch* have
`d = 0` along the join, which is **half coverage** — a visible crack across the shape. The
overlap is load-bearing. So the suite allows a quarter of a percent of samples at
sub-perceptual error and still fails loudly on anything wider or larger, and prints the count
either way so the distinction is visible rather than assumed.

Also registered with CTest, so `ctest --test-dir build -C Release` runs it.

---

## How it works

### The data model, and what it costs

Point geometry lives in **fixed, host-keyframable parameter slots**, so Resolve can
keyframe and curve-edit every point natively with its own tools.

The consequence is a hard ceiling. OFX cannot create parameters at runtime, so every slot
must exist before anything is drawn: `kMaxStrokes * kMaxPoints * 2` Double2D parameters
are defined unconditionally, currently **4 strokes x 16 points = 128 point parameters**,
around 300 in total. This is *not* the "infinite curves" a serialized string blob would
allow; that design has no ceiling but no host keyframing either, and would require the
animation to be entirely plugin-owned.

Within that, one split does a lot of work:

> **Anything that animates is a parameter. Anything structural is a string.**

Positions and handles get real keyframable slots. Corner/smooth flags go in a lenient
comma-separated `StringParam` per stroke, parsed so that "anything which is not a digit
separates" — a half-typed entry is harmless and never throws mid-render. Nobody keyframes
whether a point is a corner, so it does not deserve a Double2D multiplied by 128.

### Coordinates

Points are stored **normalised**, `(0,0)` bottom-left to `(1,1)` top-right, as plain
`eDoubleTypePlain` doubles — deliberately not `eDoubleTypeNormalisedXY`, which was
deprecated in OFX 1.4 and whose interpretation in Resolve is unverified. Plain doubles mean
the plugin owns the convention and no host can disagree with it.

Widths are a percentage of frame height. `renderScale` cannot substitute for this: it
tracks *proxy* scale, not timeline resolution, so it is still 1.0 on a UHD timeline even
though the frame has doubled. Only a ratio makes a design authored at 1080p land
identically at UHD.

Softness is the deliberate exception and is authored in **pixels**, scaled by `renderScale`
only. It is the antialiasing width, and one screen pixel of softness should stay one screen
pixel at any resolution.

**There is no y-flip anywhere.** OFX images are bottom-up and canonical overlay coordinates
run the same way, so stored points, the rasterizer and the overlay all share one y-up space.
`resolve-text-animator` has a top-down core and must convert at a single carefully-guarded
boundary; having no boundary at all is better, and it is available here only because
nothing in this plugin reads pixels.

### The rasterizer

Neither existing plugin draws new geometry — `resolve-text-animator` only resamples pixels
Resolve has already antialiased — so this part has no precedent to copy.

Every stroke becomes a list of **round cones** (capsules with two radii). Each pixel asks
for its distance to the nearest one, and then every feature is a threshold on that single
number:

| Feature | How |
|---|---|
| Core stroke | `smoothstep` across `d = 0` with the feather width |
| Taper | the two radii differ; a constant-width line is just `ra == rb` |
| Joins | round, free — the union of two overlapping capsules *is* a round join |
| Outline | a second `smoothstep` at `d = outlineWidth`, composited under |
| Glow | `smoothstep` on the same `d` at a large radius — **no blur pass** |
| Drop shadow | the same loop evaluated at `p − offset` — **no blur pass, no buffer** |
| Arrowheads | one segment with a full-width base and a zero-width tip |
| Gradient | lerp by arc length, or by signed offset across the stroke |

**Arrowheads are a real triangle**, and the first attempt — a round cone with a full-width
base and a zero-width tip — was wrong in a way worth recording, because it looks correct on
paper. The flanks of a cone are the tangent lines from the tip to the base circle, and those
touch the circle *behind* the base plane, so cutting the back off flat leaves a short arc of
that circle on each side rather than a straight edge. The result has a visible kink where arc
meets cut. A cone is not a triangle.

So `kTriangle` is a second primitive kind on the same struct, sharing the tile grid and the
upload. `Roundness` subtracts a radius from the triangle SDF, which rounds the corners and
inflates the shape; the build compensates by insetting each vertex along its angle bisector
by `r / sin(half the interior angle)`, so rounding softens the corners without growing the
head. Past what the triangle can absorb the insets would cross over and turn it inside out,
so the offset is capped at a share of the shorter adjacent edge — over-rounding degrades to a
blunt lozenge.

**The stroke is pulled back from any arrowed end**, by the arrow's length. Without it the
line's own round cap sits exactly where the tip is, so a disc of `halfWidth` bulges straight
out past the point and the arrow ends in a blob — the arrowhead's whole job undone by the
cap behind it.

Arrows are emitted at the **trimmed** ends, so one leads a draw-on reveal instead of waiting
where the line has not reached; and outside the dash walk, because a dashed connector wants
one arrow, not one per dash.

**An arrowhead belongs to the line, not to the path.** It is gated on whether any trim window
is visible at all, so when an Offset sweep carries the window off the end the head leaves with
it. Emitting at the path's ends regardless left a head hanging in space after the stroke it
terminates had been trimmed away, which reads as a separate object stuck near the line rather
than part of it.

### Trim wrapping on closed paths

`Wrap on Closed Paths` (on by default) decides whether a closed path's trim carries on past
the seam. Wrapping is usually what a ring wants — drawing itself on should continue round
rather than stop at an arbitrary seam.

But it makes one common move impossible: sliding a short window with Offset from −1 to 1 to
fly a segment along the path and off the end. Wrapped, the window can never leave, so the
segment circles forever and the animation has no beginning or end. Turned off, a closed path
trims exactly like an open one and the same Offset sweep works on both.

The wrap path also had a real bug, found by reading segment counts rather than pictures: the
window span was recomputed *after* the start had been wrapped, mixing a wrapped start with an
unwrapped end. At Offset 1 that turned a 0.22 window into `Min(1.22 − 0, 1)` — the whole ring.
A sliding window silently became a full circle at exactly one offset, and every frame looked
plausible on its own. `linetest --sheet sweep` renders wrapped, unwrapped and open side by
side across the sweep, because only the sequence shows it.

**Placement is a control, and the default is base-attached.** `Arrow Position` runs from 0
(tip on the path end) through 0.5 (centred) to **1 (base on the end, the default)**, where the
head carries on past where the line stops.

Tip-on-end is the intuitive choice and it is the wrong one. The whole head then hangs off the
*back* of the line, and on a curve — where the direction at the final point has already swung
away from the direction the line appears to be travelling — it reads as an arrow that slid
off the end it belongs to. Nothing about the geometry is wrong there; it just does not read.
`linetest --sheet caps` carries the same hooked path at position 1 and position 0 side by side.

**Direction comes from the chord across the part of the path the head actually covers**, and
that span follows `Arrow Position` — it is `arrowLength * (1 - position)`, not a fixed
lookback.

Both ends of that range fail differently if it is fixed. At position 0 the head lies along the
last arrow-length of the line, and a chord over exactly that is right; aligning to the
instantaneous final tangent instead leaves the head visibly skewed against the curve beneath
it. At position 1 the head has moved entirely past the end and covers no path at all, so that
same backward chord averages in a stretch the head has left behind, and it points off along
where the line *was* heading rather than where it is going. Scaling the lookback by
`(1 - position)` makes both fall out of one rule. A floor of 15% keeps a head sitting past the
end aimed by a short chord rather than a single flattened edge, which would jitter with the
flattening tolerance. A degenerate chord falls back to the tangent, which is always defined.

The shaft is pulled back only by the part of the head that lies *before* the end, so at the
default of 1 the line keeps its full length.

### Fill for closed paths

Fill is the one feature the tile grid cannot serve. It needs a **winding number**, and a
winding number needs every edge whose y-range straddles the pixel's scanline — including
edges on the far side of a large shape, which are not *nearby* but do decide whether the
pixel is inside. The tile grid holds what is nearby, so it has the wrong answer by
construction.

So fill has its own index, binned by horizontal **band** (a run of 16 scanlines) rather than
by tile. Every edge that can affect a pixel's winding is in that pixel's band by definition.
For a circle or a rounded box a band holds a handful of edges, so the per-pixel cost stays
small however large the shape gets on screen.

The same edge list supplies the distance used to antialias the fill boundary, which is why
bands carry a small vertical margin — without it a pixel just under an edge would find its
nearest edge one band away, and the fill would go hard along every band boundary. Winding
gives inside/outside, distance gives the soft edge, and combining them into a signed distance
means the fill boundary runs through the same smoothstep the stroke uses, so the two edges
match instead of one looking crisper.

Two consequences worth knowing:

- **Fill uses the whole contour, before trim and dashes touch it.** A trim is a reveal of the
  line; a shape whose fill came and went with it would be a different shape every frame
  rather than one being drawn. Animate `Fill Opacity` for that.
- **A filled interior has no stroke geometry anywhere near it**, so the shading pass cannot be
  gated on the tile lookup — that is the bug where a large shape comes out hollow. Fill is
  measured before the tile lookup and composited between the glow and the outline.

Nonzero winding rule, so a self-intersecting contour still has a definite answer. Multiple
closed strokes accumulate into one winding, which is what makes an inner contour wound the
other way cut a hole.

`linetest --sheet fill` covers rect, circle, fill-with-no-stroke, fill under trim and dashes,
a self-intersecting bowtie, and the compositing order against outline, glow and shadow.

*Known limitation:* the drop shadow is cast by the stroke, not by the filled area.

### Gradients

Four modes — None, **Along Length**, **Across Width** (edge to edge), **Centre Out** — plus
`Start`, `End` and `Midpoint` on the ramp.

The first version drove Across Width from the SDF distance, and that could not work. The
distance is unsigned across the width: it cannot tell the two sides of a line apart, so the
ramp was symmetric about the centreline whatever else was done to it, and the only thing it
could produce was a band along both edges. That is why it behaved like an outline rather than
a gradient — the shape it needed was not expressible.

The fix is a signed cross-stroke coordinate carried alongside the distance, `-1` at one edge
to `+1` at the other, normalised by the *local* half-width so the ramp follows a taper instead
of staying parallel to the centreline. Across Width uses it signed; Centre Out folds it. The
old symmetric look is still there, now as a deliberate choice.

`Start`/`End` place and scale the ramp, and are allowed to run **past 0..1** — confining them
to the stroke would force the ramp to span the whole of it, which is what made the gradient
feel like a fixed effect rather than a control. `End` below `Start` reverses it, which saves a
separate invert. `Midpoint` biases the halfway point the way a Photoshop gradient midpoint
does, with the exponent chosen so the ramp passes through exactly 0.5 there.

`linetest --sheet grad` renders all four modes plus narrow bands, reversal, an off-the-end
ramp, midpoint bias, taper and dashes.

**The drop shadow costs one extra distance evaluation per segment**, in the same loop, at a
displaced position — not a second pass over a blurred buffer. The tile grid expands each
primitive by the offset magnitude plus softness, so a segment is binned into the tiles its
*shadow* can reach; without that the shadow clips along tile boundaries, the same failure as
an under-reported glow radius, just displaced.

The exact analytic falloff for glow and feather, with no separable blur and no scratch
buffer, is the main reason for choosing an SDF over a scanline rasterizer.

Cost is kept off the pixel by a **16x16 tile grid**: each tile holds the indices of
primitives whose bounds, expanded by the style's maximum reach, overlap it. Without it the
kernel is O(pixels x primitives).

### Trim and dashes are resolved as geometry, not as coverage

This is the most important decision in the rasterizer and the easiest one to get wrong.

Modulating coverage by a dash pattern at shading time gives the wrong answer at every dash
boundary: the distance field still describes the *uninterrupted* line, so each dash gets a
hard sliced edge instead of a cap, round caps stop being round, and feather leaks across
the gaps. Cutting the **geometry** instead means each dash is a run in its own right, and
the SDF caps it correctly for free. Trim is the same operation with one dash.

Round caps need no special handling — a capsule already is one. Butt and square caps clip
the run with a half-plane at each end. A square cap is a butt cap on a run extended by its
own radius, so the two share a code path.

**Cap clipping has to be applied to every segment near an end, not just the terminal one.**
Clipping only the terminal segment looks obviously sufficient and holds only while a segment
is longer than the line is thick. A flattened curve produces segments a few pixels long
against a radius many times that, so every interior capsule is a fat blob bulging straight
through the flat end — butt-capped dashes came out clean on straight lines and lumpy and
rounded on curves, which is the exact signature of that mistake.

It must equally stay **local** to the ends. These are infinite half-planes, and a run that
curves back on itself — a circle trimmed to 300°, say — has most of its length sitting
behind the plane at its own end, so applying the cap to every segment erases most of the
arc. The build walks out from each end and stops once the accumulated length exceeds the
radius, which is exactly how far a capsule can protrude.

`linetest --sheet caps` covers both, including the 300° arc that catches over-clipping.

**The cap plane stores a point on the plane, not a scalar offset.** The tidier algebraic
form, `Dot(n, p) − d` with `d = Dot(n, planePoint)`, subtracts two numbers of the order of
the frame size to produce a distance of the order of a pixel. That is catastrophic
cancellation: float epsilon on a coordinate of 200 is already 2e-5, and it grows with
resolution. It surfaced as a CPU/GPU parity failure of 1.7e-5 on butt-capped dashes — the
two compilers rounding the same doomed subtraction differently. `Dot(n, p − planePoint)`
subtracts the positions first, is exact for nearby coordinates, and costs identical
arithmetic. Parity error dropped 8x, to 2.0e-6.

### Taper has three stops, not two

`Start`, **`Body`**, `End`, interpolated as a quadratic Bézier along arc length.

Two stops could not express the shape a taper control most obviously ought to make: setting
both ends thin interpolates thin-to-thin, so the line disappears instead of swelling through
its body and pointing at both tips.

The Bézier rather than two lerps joined at the middle buys two things. It is smooth — two
lerps leave a corner, so a leaf comes out as a hard-edged rhombus. And it still reduces
*exactly* to a straight taper: the control point is `2·body − (start+end)/2`, so setting
`body` to the average of the ends collapses the quadratic algebraically to `s(1−t) + e·t`.

`Body` is the width at the midpoint, not the raw Bézier control value — a control point sits
off the curve, and a control labelled "Body" that the line never reaches would be a poor
thing to hand somebody. The profile is clamped at zero because a body much thinner than the
ends drives the control point negative, and a negative radius inverts the round cone rather
than pinching it.

**Subdivision is driven by the width profile as well as by curvature.** A round cone
interpolates its radius *linearly* between its two ends, so a segment can only represent a
straight ramp. That is exactly right when the profile is straight and silently wrong when it
is not: a path drawn with corner points flattens to one segment per span, so a leaf taper
evaluates to radius 0 at both ends of that single segment and the stroke **disappears
completely**. It presented as "taper only works on curves", because only curves were being
subdivided enough to show it.

So a curved profile forces up to 48 samples along the path; a straight taper still costs one
segment. The extra points are interpolated linearly between existing flattened vertices,
which is exact — the path is a straight line between them by construction — so this adds
width detail without moving the geometry at all.

### Flattening

Curve flattening measures how far the handles stray from the chord, treated as a **segment**
rather than an infinite line — control points that project outside the chord describe a curve
that doubles back along it, which is collinear but not flat.

The obvious alternative, the second differences of the control polygon, is wrong in the most
common case there is. A corner point has a zero-length handle, which puts `p1` on `p0` and
`p2` on `p3`; the second differences then evaluate to the full chord length, so a perfectly
straight line asks for the subdivision of a hard curve. Measured before the fix:

| Case | Second differences | Distance to chord |
|---|---|---|
| Straight 2-point line | 30 vertices | **2** |
| Four-corner rectangle | 93 vertices | **5** |
| Bezier S-curve | 42 vertices | 29 |

A zero handle is the default for every new point, so that penalty applied to almost every
path anyone would draw. Output is pixel-identical.

---

### Taper follows the trim

A taper is a statement about the ends of a **line**, and while a trim is running the line's
ends are not the path's. Welded to the path, a stroke drawing on begins at whatever thickness
the profile happens to have where the reveal starts and only thins where the *path* ends — so
the leading tip, the one place a tapered stroke should obviously come to a point, stays blunt
for the whole animation. **Taper Follows Trim** (default on) maps the profile across the
visible window instead. The arrowheads already ride the trimmed ends for exactly this reason.

It changes nothing when nothing is trimmed, which is why it can default on safely.

Mechanically, `EmitRun` no longer derives the taper parameter from arc length. The caller
passes the profile's value at each end of the run, and the flag chooses between path-absolute
(the old behaviour, reproduced exactly) and window-relative. Three consequences fall out:

- **Dashes still share one profile.** Each dash receives the slice of the domain its position
  in the run earns it, so a dashed tapered line thins along the run rather than each dash
  tapering individually.
- **A window across the seam of a closed path is two runs but one taper.** The split point is
  expressed as its fraction of the window and handed to both halves, or the seam would show as
  a step in the width.
- **Subdivision is measured against the profile's own sweep**, not the path length. A profile
  that completes over a short trimmed window changes just as much over far less distance, and
  a step derived from the path would under-sample it — the leaf taper would come back as the
  flat-topped shape a single round cone can represent, which is the bug that made taper look
  broken on straight spans in the first place.

Turn it **off** to weld the profile to the path, which is what a brush stroke with a
deliberate thick spot wants when a trim slides across it.

`linetest --sheet taper` renders a draw-on at 25 / 50 / 75 / 100% with the flag off and on,
plus a dashed run and a mid-path window.

## The overlay

Resolve's OFX host supports neither parametric parameters nor custom parameter interacts, so
the viewer is the only surface where a real editor can exist — the Inspector can only ever
show a list of numbers.

| Action | |
|---|---|
| Drag a point | move it |
| **Shift**-drag a point or handle | lock the drag to an axis |
| **Ctrl**-drag a point | pull a bezier handle out of it, turning a corner into a curve |
| **Ctrl**-click a point (no drag) | collapse the handle, turning it back into a corner |
| Drag a tangent tip | reshape the curve; the opposite handle mirrors |
| **Alt**-click a point | delete it (refuses below two points) |
| **Shift**-click near the curve | insert a point there |
| Click a point | select it, which reveals that stroke's tangent handles |
| **Drag from empty space** | rubber-band select every point inside |
| **Shift**-drag from empty space | add those points to the selection |
| **Drag a selected point** | move the whole selection together |
| Click empty space | clear the selection |
| Drag a curve-editor handle | reshape the In or Out easing curve |

### Multi-point selection, and why it exists

A rounded rectangle forced this. Its corners are circular arcs **baked into points**, so
widening it with a non-uniform Transform scale stretches those arcs into ellipses — the shape
cannot be reproportioned by the transform without deforming the very thing the corner radius
was for. Selecting one side's points and moving them together does it exactly: the straight
edges lengthen and every fillet is left alone.

(The generator's separate **Width** and **Height** cover the same need at creation time. Use
those when you know the proportions up front; use the selection when you don't.)

- **No modifier of its own.** Dragging a point already in the selection moves the selection;
  dragging one outside it drops the selection and takes just that point. That is what every
  vector editor does, and it matters here because Shift, Alt and Ctrl are all already spoken
  for. Empty-space drag was likewise doing nothing at all, so the rubber band collides with
  no existing gesture.
- **The axis lock applies to the delta, not to each point.** Locking per point would let two
  points snap to different axes and shear the shape.
- **Deltas are applied to positions recorded at press**, not accumulated frame to frame, so
  rounding cannot creep and no point can drift away from the others over a long drag.
- **Inserting or deleting a point clears the selection.** Both shift every later index, so a
  selection holding raw indices would afterwards point at the wrong points — and the next
  multi-drag would move them. Dropping it is cheaper and safer than renumbering it.
- **A marquee opens no edit block.** Selecting writes no parameter, and calling `EndEdit` for
  a gesture that never called `BeginEdit` is exactly the unbalanced begin/end that leaves the
  host's interact state broken for the rest of the session.

> **Untested outside Resolve, like the rest of the overlay.** It needs a draw context and pen
> events, so there is no offline harness for it — see
> [Phase 0 is not finished](#phase-0-is-not-finished).

## Presets

Resolve gives OFX plugins no preset mechanism of its own, so this is one: **Save Preset**,
**Load Preset**, a **Reset All**, and a preset **folder** you can point anywhere.

Files are plain JSON in `Documents\BetterLine\Presets`, readable and hand-editable. The folder
is overridable and remembered in `%LOCALAPPDATA%\BetterLine\settings.json` — deliberately *not*
an OFX parameter, because a parameter lives inside the **project**: it travels with the
timeline and belongs to one instance of the effect. "Where my presets live" belongs to the
person and the machine and has to apply to every instance ever created, including ones in
projects that do not exist yet.

**Driven by a registry, not a hand-written struct.** Multi Transform mirrors each parameter by
hand — struct, serialiser, parser — 370 lines for ~100 parameters. This plugin has well over
200, and every new one would have to be added in all three places; forgetting produces a
preset that silently does not save that setting, a bug with no symptom until someone reloads
and quietly loses their work. Instead every parameter registers its name and type at the point
of definition (they already funnel through two helpers), and save/load iterate that list. New
parameters are covered automatically.

Details that matter:

- **A missing key resets that parameter to its default**, rather than leaving it alone. That is
  what makes a preset deterministic: loading the same file from two different starting states
  gives the same result, instead of quietly inheriting whatever was set before.
- **A malformed file is refused, not partially applied.** Half a preset is a state the user
  never authored and cannot get back from.
- **The whole load is one edit block** — one undo step and one cache invalidation instead of
  240 of each — with a re-entrancy guard, since every `setValue` dispatches back into
  `changedParam` and `syncVisibility` would otherwise run hundreds of times per load.
- **The file dialogs are never inside an edit block.** `changedParam` is dispatched
  synchronously from the button press, so the dialog opens inside that call, and a modal Win32
  dialog pumps its own message loop — holding a block open across that hands the host an
  unbounded amount of unrelated work while it believes an edit is in flight.
- Numbers are written with **nine significant digits**, the documented minimum to round-trip a
  float. At the default six, every point would shift slightly each time a preset was saved and
  reloaded.
- Parameters are written **sorted**, so saving twice from the same state produces byte-identical
  files — diffable, and sensible to keep in version control.
- Preset controls are themselves excluded from presets. A preset recording where presets live
  would overwrite that setting on every load.

`linetest --sheet check` round-trips every value type including quotes, backslashes, tabs,
control characters and empty strings, and asserts that malformed input is rejected.

*Not saved:* keyframe curves. A preset stores each parameter's current value.

## Add Shape

Rectangle, ellipse and regular polygon, written into a stroke slot as **ordinary control
points** — then the generator gets out of the way. There is no live "rectangle object": what
lands in the slot is points, keyframable and draggable like any others.

There is deliberately no separate Triangle: a triangle is a three-sided polygon, and two
controls for one idea is one control too many.

That is the whole design decision. A shape that stayed a rectangle would need its own
parameters, its own overlay handles and its own animation path, and would still be less useful
the moment someone wanted to move one corner. Since the plugin already has a global Transform,
placing and sizing a shape needs no per-shape controls at all.

| Control | Notes |
|---|---|
| **Shape** | Rectangle, Ellipse, Polygon |
| **Sides** | Polygon only. 3 is a triangle. Range shrinks to 3..8 once Corner Radius leaves zero |
| **Corner Radius** | % of the shape's *inscribed* radius; ignored by Ellipse |
| **Width**, **Height** | Half-extents, both as a % of frame *height*, so equal values are a true square on any timeline |
| **Into** | `Next Free` picks the first stroke that is switched off, so adding never overwrites work in progress |

- **Corner Radius is measured against the inscribed radius**, the distance from the centre to
  the middle of an edge — not to a vertex. Vertex distance is not comparable across shapes: a
  triangle's corners sit twice as far out as its edges, so the percentage that gently softened a
  rectangle turned a triangle into a blob. Measured this way a triangle and a square agree, and
  **100% is "as round as this shape gets"** for all of them — a square at 100% is exactly a
  circle.
- **Rounding costs two points per corner**, since each vertex becomes two points joined by an
  arc, and the slot ceiling is 16. So the *range* of Sides halves the moment Corner Radius
  leaves zero, rather than the button refusing a combination the controls happily allowed you
  to dial in. A slider that stops where the limit is explains the limit better than a dialog
  does. The refusal survives as an unreachable backstop, because drawing a polygon with
  silently fewer sides than asked for is the one outcome worse than a message.
- **Handles are symmetric**, because a slot stores one tangent per point. That costs nothing
  here: a circle's four-point bezier is naturally symmetric, and at a rounded corner the
  mirrored handle points back along the straight edge it came from — collinear with it, so the
  edge stays exactly straight. Confirmed by the flattener, which still emits one vertex per
  straight edge (a sharp octagon flattens to 9 points, not 90).
- **The x axis is divided by the frame aspect — once, at the very end.** Slot x is a fraction of
  frame *width* and y of *height*, so a circle built with equal radii comes out an ellipse 1.78x
  too wide on a 16:9 timeline. Every generator therefore works in **square units** and `MakeShape`
  squashes x last, for positions and tangents alike.

  Doing it any earlier is a real bug and was one: a fillet's handle length is a single scalar,
  and applying a scalar in units whose two axes have different scales stretches its horizontal
  component by the aspect. One flank of every rounded corner came out an aspect-ratio too long
  and the arc leaned into the vertical edge. Squashing last is also exactly right geometrically,
  since square units map to pixels by a single factor (the frame height), so a circular fillet
  stays circular.

  Neither the stretched circle nor the leaning corner can appear in a square test cell, so
  `--sheet check` asserts both numerically — the corner one by requiring every handle of a
  rounded rectangle to be the same length *in pixels* on a 16:9 frame.
- **Width and Height are separate, rather than one Size plus Transform Scale.** A non-uniform
  Transform scale stretches the corner fillets from circular arcs into elliptical ones, so a
  wide rounded rectangle cannot be made by squashing a square one — the proportions have to
  exist *before* the corners are rounded. To reproportion one after the fact, use
  [multi-point selection](#multi-point-selection-and-why-it-exists).
- **Existing keys on the target slot are deleted first.** They describe the shape that used to
  be there and would win at every frame, making the button look like it had done nothing.
- Shape controls are `AddParamUI`, so they neither invalidate the render cache nor appear in
  presets — they describe the *next* shape, not the current look.

`linetest --sheet shapes` renders twelve variants. Note the contact sheet writes its rows
top-down and so displays every case **y-inverted**; orientation is asserted numerically in
`--sheet check` instead, which is why the triangle points down in the sheet and up in Resolve.

### Does scaling a generated shape cost quality?

**No** — and this is why the generator only needs a rough Size. The global Transform is applied
to control points in **pixel space, before flattening**:

```
slot point -> pixels -> Xform2D::Apply -> FlattenStroke -> SDF
```

So a scaled-up shape is re-flattened at its final on-screen size and rasterized analytically by
the SDF. Scaling up *increases* subdivision rather than stretching a fixed mesh — the opposite
of resampling an image. There is nothing to lose.

One consequence worth knowing: **Transform does not scale stroke Thickness.** Scale a square up
and its outline stays the same weight, as with SVG's `vector-effect: non-scaling-stroke`. That
is usually what motion design wants, but if you need the weight to grow, animate Thickness.

## SVG import

**Import SVG** reads path geometry into a second store that sits alongside the keyframable
point slots.

The split is the point. Slots exist so Resolve can keyframe an *individual* point, which costs
two parameters each and caps the plugin at 4 x 16 = 64 points. Real artwork is nowhere near
that — one of the test files is **19 subpaths and 258 points**, four times the entire slot
budget, and any logo containing text is far worse (two subpaths per letter). But an imported
logo is exactly the thing nobody keyframes point-by-point: it gets placed, scaled and drawn
on, which Transform and Trim already do. So it lives in one string parameter with no ceiling.

Imported paths share everything else — style, transform, trim, dashes, stagger, animation.

### What it reads

All of `M/L/H/V/C/S/Q/T/A/Z`, absolute and relative, plus `rect`, `circle`, `ellipse`, `line`,
`polygon` and `polyline`. Transforms on the element and on every ancestor `<g>`, including
`matrix`, `translate`, `scale`, `rotate` (with and without a centre), `skewX` and `skewY`.

- **Quadratics are exact.** A quadratic *is* a cubic whose controls sit two thirds of the way
  from each endpoint to the quadratic control.
- **Arcs are converted** via the endpoint-to-centre parameterisation, split into segments of
  at most 90° — a single cubic cannot usefully represent more than a quarter ellipse.
- **Numbers may run together.** `M0,0L10-5L20,0` is legal SVG and `.5.5` is two numbers, so
  the scanner cannot simply hand the rest of the string to `strtod`.
- **`<defs>`, `<clipPath>`, `<mask>`, `<symbol>`, `<pattern>` and `<marker>` are skipped.** This
  is required, not a refinement: a clip path is described with the same `<path>` element as
  real artwork, so a scanner that takes every path in the file imports the clipping rectangle
  as a shape. An Inkscape export did exactly that and came in with a large square across the
  whole logo.

### Coordinates

Blob coordinates are **square units** — y spans the frame height and x is scaled by the height
too, then centred. Slot points use width for x, which is right for hand-placed geometry and
wrong for artwork: stretching x by the frame aspect would import a circle as an ellipse. The
document is fitted to its longer axis so nothing is cropped, and SVG's y-down axis is flipped
to this plugin's y-up here, once.

The blob is parsed once and cached against the string it came from. Reparsing tens of
kilobytes every frame would make the per-frame cost scale with how detailed the artwork is,
which is exactly backwards.

### Paint, and the fill-only problem

Each imported subpath carries the paint it was drawn with — stroke colour, fill colour, stroke
width, and whether it had either at all. `Use Imported Colours` and `Use Imported Widths` are
the escapes back to the global style, because artwork that silently disconnected the Colour and
Thickness controls would be a worse problem than the single-colour silhouette it replaced.

Two rules make those toggles behave, and both were bugs first:

- **The global `Fill Closed Paths` is a master switch over the per-path flag, not a second
  opinion.** Filled artwork used to be filled unconditionally, with no way to turn it back into
  an outline.
- **`Use Imported Widths` off means the global Thickness applies to *every* imported path,
  including ones the artwork never stroked.** This is the one that matters. Most icon sets are
  drawn as filled silhouettes with no stroke anywhere — one test file has five `fill`
  declarations and zero `stroke` — and **Trim reveals a stroke**, so a shape with none animates
  not at all. Importing such a file used to produce artwork that could not be drawn on, which
  is the single reason someone brings a logo into a line plugin.

So the importer picks the starting state from what the file actually contains: filled artwork
turns the global Fill on, and artwork with **no** stroke anywhere turns `Use Imported Widths`
off, handing every contour the global Thickness. Files that do carry strokes are left alone —
there the artwork's own widths are the better answer. Both are ordinary parameters afterwards.

A fill can't be trimmed instead, and deliberately: a trim is a reveal of the *line*, and a shape
whose fill came and went with it would be a different shape every frame.

### Limitations

- **Gradients, masks and filters are ignored.** Only flat `fill` and `stroke` paint is read,
  from either presentation attributes or an inline `style=`.
- **Fill is decided by paint, not by `Z`.** An SVG fill implicitly closes its subpath whether
  or not the path data closes it, so an import fills on the `fill` attribute alone. Gating that
  on `Z` cost an Inkscape cat its nose — one filled subpath among six stroked ones, written
  without a closing `Z` because filling it never needed one. The `closed` flag still gates
  **hand-drawn** strokes, where it is the user's own toggle and the label says so.
- **Text must be converted to outlines first.** There is no font rasterizer here.
- Imported paths are not editable point-by-point in the overlay. Use Transform, or edit the
  path data string by hand.

`linetest out.png --svg file.svg [--fill]` imports and renders a real file offline, through
the blob round-trip, so what you see is what would actually be stored. `--sheet check` asserts
the command set, including relative forms, implicit linetos, run-together numbers and packed
arc flags — a relative command that lands plausibly is not something visual inspection catches.

### Global transform

`Position`, `Rotation`, `Scale` and `Anchor` move every stroke together. **Link Scale** (on by
default) drives both axes from the single `Scale` value; turning it off reveals `Scale Y`.

**Scale is two scalars, not one Double2D with the axes mirrored** — and the first attempt was
the mirror, which is worth recording because it looked obviously right and behaved badly.
`changedParam` names the *parameter* that changed but never which *component*, so a Double2D
has to infer which axis moved by comparing against a cached value, then write the other axis
back through the host. That write is a round trip you can watch happen — drag X and Y follows
a beat later — and the inference loses track whenever the host coalesces events, leaving the
two stuck out of step with no way back except touching them again.

There is nothing to write in the current design. When linked, `Scale Y` is simply **not read**,
and it is hidden so the panel cannot show a number that has no effect. The two cannot drift
apart because nothing ever tries to keep them together.

The gizmo's scale handle multiplies **both** axes by the same ratio whether linked or not, so
dragging it on an unlinked shape keeps the stretch it was given rather than flattening it. Editing points one
at a time is fine for *drawing* a shape and useless for *placing* one — nudging a six-point
arrow across the frame means six drags that all have to agree. All four are keyframable, so
this is also how you fly a whole shape around without touching a point.

Applied in **pixel space**, not in the normalised space points are stored in. Normalised units
are not square on a non-square frame, so a rotation performed there shears the shape instead of
turning it — a rotated square would come out as a rhombus, and differently at every timeline
resolution.

Bezier handles go through the **vector** form: they are offsets from their point, so they
rotate and scale with the shape but must not be translated. Applying the position to them as
well would double every handle's length.

**There is an on-screen gizmo**: a centre cross to move, a round handle to rotate and a square
one to scale, both orbiting with the current rotation so the gizmo reads as attached to what it
drives. The handles sit at a fixed *screen* radius rather than on the artwork's bounds — an
imported logo can be any size, and handles that scaled with it would be off the viewport for a
large one and unclickable for a small one.

Drags are anchored to the press rather than accumulated per event, because accumulating drifts:
each event's delta is rounded independently. A press exactly on the anchor would make every
later scale ratio a division by zero, so the press distance has a floor.

**Imported paths are drawn in the overlay too**, ghosted, through the same square mapping and
the same transform the renderer uses. Without that the overlay showed point handles for
whatever the slot strokes happened to be while the imported artwork on screen had nothing
attached to it at all — which reads as "the transform does not affect my import" even though it
always did.

**The overlay draws handles transformed and writes drags back through the inverse.** Drawing
untransformed positions would put every handle somewhere the stroke is not, the moment the
transform is touched; and storing a drag without inverting it would apply the transform a
second time on the next draw, so the point would run away from the cursor. Insert-on-curve
inverts too, since the click lands on the transformed curve.

`linetest --sheet check` asserts the inverse round-trips across pivots, offsets, rotations,
non-uniform and mirrored scales — including a near-zero scale, which is not invertible and is
guarded to a no-op on that axis so a shape scaled to nothing can still be scaled back up.
A drift here would be invisible in any single frame and would creep a little further on every
mouse-move event.

### The easing curve editor

`Show Curve Editor` draws an editable easing curve in the viewer, for the In or Out stage.
This has to live in the viewer: Resolve's OFX host supports neither parametric parameters nor
custom parameter interacts, so the Inspector can only ever show four numbers.

**In and Out are tabs on the panel itself.** The stage started as a dropdown in the Inspector,
three collapsed groups away — which is not somewhere anyone will find it, and the entire point
of an on-screen editor is not having to go and look. The tab rects are *derived* rather than
stored, so the drawing and the hit test cannot fall out of step; a stored rect would be stale
on the first click after a viewer resize.

The inactive stage is drawn **ghosted behind** the active one. The two curves are usually being
shaped against each other — an entrance and the exit that has to answer it — and showing only
one means holding the other in your head.

It plots through the **same `ApplyEasing` the renderer calls**, so it shows the curve that
will actually be used rather than an approximation of it, and the drag maps back onto the four
human controls rather than writing raw bézier handles — the Inspector still shows numbers
someone can reason about after a drag. That is why `Anticipation` and `Overshoot` now allow
negative values: a handle can legitimately go either side of the rails, and clamping at zero
would make half the curve space unreachable by dragging while the handle stuck against an
invisible wall.

Two rules carried from Multi Transform's version:

- **x is clamped to 0..1, y is not.** Outside 0..1 in x the bézier folds back on itself and
  one time maps to two values, which the solver cannot answer. In y, dragging past the rails
  is exactly what produces anticipation and overshoot.
- **The plotted y range is latched, not refitted every draw.** Refitting continuously turns a
  drag into a feedback loop: pulling a handle up grows the range, growing the range rescales
  the plot, and the rescale drags the handle back down under the cursor — the graph appears to
  fight the edit. Between drags it refits freely in *both* directions, since a range that could
  only grow would stay zoomed out after one wild drag. The fit includes the handle positions so
  an off-curve handle stays grabbable.

The panel is hit-tested before the path handles because it is drawn over them, and it swallows
any click inside it so a miss near a handle does not fall through and start dragging a point
behind it. When hidden it is skipped from hit-testing **entirely**, not merely not drawn.

The toggle is registered as a UI-only parameter: showing an editing aid should not invalidate
the render cache and throw away every frame.

Points are drawn **square when they are corners and round when they are smooth**, because a
corner's handle tips sit exactly on top of the point and so cannot be drawn — without the
distinction there is nothing on screen to say that Ctrl-dragging will do anything.

Ctrl-drag is not a convenience. A corner's handles are at zero length, so they can be
neither seen nor hit, and hit-testing them there anyway would be worse: the tangent test runs
before the point test, so it would swallow every attempt to drag the point itself. Since
every point starts at zero — the defaults and every Shift-inserted point alike — handles were
otherwise reachable only by typing numbers into the Inspector.

The click-versus-drag distinction is measured in **screen pixels** (3px), so the threshold is
the same however far the viewer is zoomed in, and the collapse is written before `EndEdit` so
it stays one undo step.

Shift-drag's axis is decided from the **drag so far**, not latched when Shift went down, so
Shift can be pressed or released part-way through and simply starts or stops constraining.
The two deltas are compared in screen pixels: canonical units are not square on an anamorphic
frame, and comparing them raw would bias every choice towards the same axis. On a handle the
lock is relative to its own point, so Shift makes it exactly horizontal or vertical rather
than merely stopping it drifting. (Shift-drag on a point and Shift-click near the curve do not
collide — the point hit-test runs first.)

Registered in `describe()`, not `describeInContext()` — the overlay descriptor is a
plugin-level property and the host caches the descriptor.

**The keyframe-aware write rule is what the whole fixed-slot design turns on:**

```cpp
if (p->getNumKeys() > 0) p->setValueAtTime(t, x, y);
else                     p->setValue(x, y);
```

`setValueAtTime` unconditionally sets a key on the first drag of every point, silently
converting static geometry into animated geometry — and you would only find out later, when
the line drifted on a frame you never touched. `setValue` unconditionally is the mirror
mistake: on an already-animated point it edits the underlying static value, which the
animation then overrides, so the drag appears to do nothing.

**Insert and delete shift whole animation curves**, via `ValueParam::copyFrom`, not by
reading and rewriting today's value — that would quietly discard every keyframe on every
point after the insertion. The slot being inserted into still holds its previous contents
after the shift, so it gets `deleteAllKeys()` first; without that the "new" point inherits
its neighbour's animation.

Correspondingly, **topology does not animate**. Stroke count, point count, enabled and
closed are all `setAnimates(false)`. Point *positions* are keyframable and that is the point
of the design, but a point count that differed frame to frame would make "which slots hold
real points" unanswerable, and the shifting logic incoherent.

Other rules carried in:

- Everything is drawn in **canonical** coordinates, anchored to the source clip's region of
  definition. Every handle size and hit radius goes through `sx()`/`sy()`, or handles become
  unusably tiny as soon as you zoom out.
- The guide curve is flattened to **half a screen pixel**, not half an image pixel, so it
  stays smooth when zoomed in and cheap when zoomed out. Reusing the renderer's tolerance
  would show a visibly faceted guide over a line that is actually smooth.
- Hit tolerance is a **fixed on-screen** distance, so grabbing a handle feels the same at any
  zoom.
- Tangent handles are hit-tested **before** points and drawn only for the selected stroke. A
  handle you can see but cannot grab is worse than one that is not drawn.
- `loseFocus` force-closes abandoned edit blocks, and `penDown` calls the same backstop.
- `keyDown`/`keyUp` track Alt and Shift because **OFX pen actions carry no modifier state**,
  and both return `false` — trapping them would swallow the host's own shortcuts.
- `pixelScale <= 0` is guarded; a degenerate scale collapses every screen-relative size to
  zero and the overlay silently vanishes.

**Known open question:** the interact slaves only to the *structural* parameters, not to all
256 point slots. Slaving everything is the thorough answer but registers a couple of hundred
watchers on a host whose behaviour at that count is unmeasured, and slaving is an
optimisation — a drag calls `requestRedraw` itself. The cost is that a point edited by typing
into the Inspector may not move the on-screen handle until the next viewer refresh. Revisit
once `probe.log` says what this host is comfortable with.

## Performance

The render touches the whole frame; the line covers a few percent of it. Everything here
follows from that asymmetry.

- **Rows are copied with one `memcpy`**, not a four-float assignment per pixel inside the
  shading loop. That is the same memory traffic written the slowest possible way, and it is
  the dominant cost of a frame — the copy covers the whole frame whatever the line does.
- **Empty tiles are skipped a whole column at a time.** `ShadePixel` already returns
  immediately for a pixel with nothing near it, but at UHD that is eight million calls whose
  entire job is to look up an empty tile and return. `AnythingAt` rejects a 16-pixel run in
  one test.
- **Diagnostics are formatted once, not once per frame.** `ProbeOnce` refuses to log twice,
  but the messages were being built — stringstream, allocations, conversions — on every render
  before it got the chance.
- **The overlay caches its clips and reuses its buffers.** `fetchClip` is a host round-trip and
  `buildContext` runs on every draw *and* every pen-motion event. Drawing also went back to the
  host for point values that `buildStroke` had just fetched; a handle cost five redundant
  parameter fetches per redraw.
- **`syncVisibility` skips redundant `setIsSecret` calls.** It touches every one of several
  hundred point parameters, each a host round-trip that can re-lay-out the Inspector, and
  almost all of them were setting the value the parameter already had.
- The overlay's flattening tolerance is **floored**. It is expressed in screen pixels, which
  shrink without limit as the viewer zooms in, so an unbounded tolerance asks for subdivision
  far finer than the screen can show — paid for on every redraw.

### Instance creation, not rendering

Reported symptom: playback fine, scrubbing fine, but **creating** a Better Line or landing on
one for the first time stalls. That is not the render — playback renders more frames than
scrubbing does. It is what the host does per *instance*, and this plugin has several hundred
parameters for it to do it to.

Two fixes, both aimed squarely at instance creation:

- **Point parameters are fetched lazily.** The ceiling is 256 and a normal instance touches
  four. Fetching them all in the constructor put the cost of the worst case on every instance
  the host creates. The interact does the same — it is rebuilt whenever the viewer wants one,
  and fetching 256 parameters to draw a two-point line was most of that cost.
- **The descriptors carry their default secrecy.** Every parameter used to be described
  visible, and the constructor then made ~250 `setIsSecret` calls to hide the unused ones. Now
  a new instance is born correct and `syncVisibility` has nothing to push. `DefaultStrokeSecret`
  / `DefaultPointSecret` are shared by the descriptor and the instance cache so the two cannot
  drift.

`ProbeScope` instruments `ctor`, `syncVisibility`, `render`, `buildScene`, `changedParam` and
`overlayDraw` separately, reporting call count, mean and worst to `probe.log` on a logarithmic
schedule. Where time goes in a host is not guessable — an effect can be fast per frame and
still make the editor sluggish, because the editor does things per *edit* that have nothing to
do with rendering.

**If rendering is ever the problem, check `probe.log` for `render dispatch: cuda=`.** A `no`
there means every frame is on the CPU and dwarfs everything else.

The remaining known cost is the **parameter count**. It started at 8 strokes x 16 points
(256 point parameters, ~390 in total) and came down to **4 strokes** after the stalling was
reported — about 148 parameters removed.

These slots are **not free at rest**: the host allocates, tracks, snapshots for undo and
panels every one of them whether or not a point is using it. `describeInContext` logs the real
total to `probe.log` rather than leaving it to be estimated from reading the source, because
guessing at that number is how it got out of hand in the first place.

`kMaxPoints` is the remaining lever — 16 per stroke is generous when a line needs 2, a box 4
and a circle 4.

## Host caveats

Carried in from `resolve-multi-transform` and `resolve-text-animator`, where nearly every
one was discovered by shipping something broken first.

- **Static CRT is mandatory.** An OFX bundle is loaded by a host without our
  redistributables, and a missing VCRUNTIME dll fails as a *silent no-load* — Resolve simply
  never lists the plugin, with no error anywhere.
- **`CMAKE_CUDA_ARCHITECTURES` must be set before CUDA is enabled.** Naming CUDA in
  `project(LANGUAGES ...)` enables it immediately and installs a default, after which a
  `if(NOT DEFINED ...)` guard silently never fires and the plugin ships PTX-only code the
  driver must JIT. This project keeps CUDA out of `project()` entirely and uses
  `enable_language(CUDA)` after the fact.
- **`ofxsHWNDInteract.cpp` must be excluded** from the Support library. It references symbols
  declared nowhere in the SDK. Blackmagic's own Makefile omits it too.
- **`ImageProcessor` is in `ofxsProcessing.h`**, not `ofxsImageEffect.h`.
- **Every value-bearing parameter declares `eCacheInvalidateValueAll`.** The OFX default
  means "invalidate only the frames this parameter's keyframe covers", which is right for a
  purely keyframed effect and wrong here — the trim window reinterprets the geometry at every
  frame. Resolve tolerates the default; Fusion does not, and Fusion is correct.
- **`setOutputFrameVarying(true)`.** Without it a host may render one frame and reuse it for
  the whole clip. Fusion does exactly that; Resolve's Edit page happens to re-render
  regardless, which is why it goes unnoticed there.
- **`isIdentity` returns false unconditionally**, and is deliberately not tested at
  `args.time`. The host caches that verdict per frame, so moving the line to cover a frame
  that previously drew nothing leaves the host convinced nothing happens there — the edit
  appears to do nothing until the cache is purged by hand.
- **Never write a parameter from `render()`.** Not allowed by OFX.
- **Ungrouped parameters must all be defined before the first group**, or Resolve scatters
  the ones that sit between two groups to the other end of the panel.
- **`eStringTypeLabel` is invisible in Resolve** — the text sits in the *value* while Resolve
  draws the empty label. Use `eStringTypeSingleLine` + `setEnabled(false)` for read-outs.
- **Resolve reports `maxPages = 0`.** There is no tab mechanism available; one page, groups
  for structure.
- **Resolve supports neither parametric parameters nor custom parameter interacts**, which is
  why the easing curve editor (Phase 8) has to live in the viewer overlay.
- **All timing will be authored in frames.** Fusion does not publish
  `kOfxImageEffectPropFrameRate` on clips and the Support library *throws* when a property is
  missing; thrown out of the render action that becomes `kOfxStatErrMissingHostFeature`,
  failing every frame — the effect simply appears to do nothing.

### Phase 0 is not finished

Two things still need measuring inside Resolve before the data model is settled:

1. **`maxParameters`.** The whole fixed-slot design rests on it, and a host that reports a
   limit will refuse the parameters past it *silently*. `HostProbe` logs it on load.
2. **The clip-time origin.** The two existing plugins disagree, and the disagreement is real:
   - `resolve-multi-transform/src/ClipTime.h` subtracts `timeLineGetBounds` t1 from `args.time`.
   - `resolve-text-animator/src/clip_time.h` concluded the opposite after longer
     investigation — *"Resolve hands this plugin clip-relative time already ... t1 was the
     worst of them: eleven different values on one clip."*

   This project defaults to the text-animator conclusion: use `args.time` directly, subtract
   nothing, keep no learned state. That needs confirming on a trimmed clip and on a Fusion
   clip before any frame-based animation is built on top of it.

After installing and restarting Resolve, `%LOCALAPPDATA%\BetterLine\probe.log` holds the
answer to (1) plus the full host capability table.

---

## Auto in/out animation

**Off by default, and inert when off.** The primary intended use of this plugin is inside a
Fusion comp, where animation is keyed by hand against Fusion's own spline editor — which is
better than anything an OFX overlay could offer. Every parameter here is an ordinary
keyframable OFX parameter and that path is the one that must not be interfered with, so with
`Animate` unticked nothing in this section touches a value: trim passes through unclamped,
thickness is re-derived from the same percentage it was seeded with, and no stage is
evaluated. Eased overshoot is clamped at the point a stage produces it, not on the way
through, precisely so manual values keep the beyond-0..1 range that closed-path winding uses.

The auto system is for the case where none of that is wanted: drop the effect on a clip and
have it draw itself on and off with no keys at all.

Plugin-owned timing anchored to the clip. It needs **no keyframes**: drop the effect on a
clip of any length and the line draws itself on at the head and off at the tail. Keyframes
still work — every parameter here is an ordinary OFX parameter — but nothing has to be keyed
for the common case. Resolve's host supports no parametric parameters, so the host could not
own this animation even if we wanted it to.

Two stages, **In** and **Out**, each with a **Target** rather than a fixed job:

| Target | |
|---|---|
| Trim End | the classic draw-on |
| Trim Start | the tail catches up and swallows the line |
| Trim Offset | the visible span slides along the path |
| Opacity, Fill Opacity | fades |
| Thickness | strokes that thicken on arrival |
| Dash Offset | marching ants |

Making the target a choice is what stops the same timing machinery being rebuilt four times.
Every target is **seeded with the value the user set** and only overwritten by a stage that
drives it, so anything left untargeted stays under manual or keyframed control — no branch,
no second code path.

**Timing anchors** are an affine change of variable, `local = (t − offset) · scale`: Clip
Start, Clip End (times run negative, so an exit is written `−20` with duration `20`), and
Stretch (a percentage of clip length). The `length − 1` in the last two is deliberate — a
clip spanning `length` frames has its last visible frame at `length − 1`, and using the
length itself puts the reference one frame past the end, so an outro finishes fractionally
short of where it was asked to.

**Easing** is a CSS cubic-bézier lifted from `resolve-multi-transform/src/AnimEngine.h`,
driven by four human controls (Ease In, Ease Out, Anticipation, Overshoot) plus presets.
Newton-Raphson first because it converges in a couple of iterations; bisection as a fallback
because Newton diverges where the slope goes flat, and a render action is a bad place for a
loop that might not terminate. The preset writes the four controls and touching a control
selects Custom, guarded against re-entry because `setValue` dispatches straight back into
`changedParam`. The curve is always built from the four controls, never from the preset
index — reading the preset at render time would mean a hand-adjusted curve still rendered as
whatever it started from.

A stage applies at **every** frame, holding `from` before its window and `to` after. Skipping
it outside the window looks tidier and is wrong: an entrance that only applied once it had
started would leave the line fully drawn on every frame before it, which is the one thing an
entrance must not do. Out is applied second, so where both drive the same target the exit
wins once it begins — and which stage is running comes from its own window, never from
comparing eased values, because an overshooting entrance sits above 1 while a dormant exit
rests at exactly 1.

Trim is clamped to 0..1 **after** easing, unlike Multi Transform's transforms: a trim past
the end of the path is meaningless and a negative one inverts the cut, so overshoot shapes
the approach without producing a nonsense window.

All timing is in **frames**. There is deliberately no frame-rate helper anywhere in this
project — see the host caveats.

`linetest --sheet anim` renders a strip across a clip, through the same core seed/apply/write
path the plugin uses, so a strip is evidence about what Resolve will do rather than about a
lookalike written for the harness.

### Time origin — both sibling plugins were right

The two sibling plugins reached opposite conclusions about the render time, and the
disagreement is real:

- `resolve-multi-transform` subtracts `timeLineGetBounds`' `t1`, because its host hands out
  absolute timeline frames (~108000).
- `resolve-text-animator` concluded Resolve already hands the plugin clip-relative time, and
  that subtracting anything moves the animation off the clip's first frame by that amount.

**Both are right, on different clip types.** Measured here: on a **Fusion clip** the render
time runs in the composition's own small range and reads as clip-relative. On a **still, an
adjustment clip or ordinary footage** it is a timeline frame in the tens of thousands — and
then a 24-frame entrance evaluates to progress 4500, clamps to 1, and the line is simply
fully drawn on every frame. The effect appears to do nothing at all.

This project originally hard-coded the text-animator answer, which is why animation worked on
Fusion clips and nowhere else. Neither answer can be hard-coded.

`Time Origin` is therefore **Auto** by default, detecting it: the render time sitting *inside*
the reported bounds, with those bounds well away from zero, is the signature of a timeline
frame — a clip-relative time would be near 0 and nowhere near `t1`. **Clip Relative** and
**Timeline Absolute** force it.

The reported clip start includes any head handle, so `Time Offset` exists to nudge frame 0
back onto the first visible frame. `probe.log` records the raw bounds, which interpretation
was chosen, and the resolved clip frame; a resolved frame far too large to be a frame within a
clip is called out explicitly, because that state renders as an inert-looking effect rather
than an error.

### Adjustment clips, and why a stage can refuse to run

Clip End and Stretch both measure from the clip's **last** frame, so neither means anything
without a clip length — and on an **adjustment clip** Resolve reports no usable bounds.

Evaluating them anyway is not a small error, it is a catastrophic one. With length 0 the
anchor silently collapses to Clip Start, so an exit written `Start −20, Duration 20` is
already *complete* at frame zero: Trim Start sits at 1, the line is fully retracted on every
frame, and the effect appears to do nothing at all.

A stage anchored to Clip End or Stretch is therefore **skipped** when no length is known. That
degrades to "the exit never runs", which leaves the line visible and obviously unanimated — a
failure you can see and describe, rather than a blank frame that looks like a broken plugin.
It is also logged to `probe.log` with the fix, which is to set **Clip Length** by hand.

### Stagger

`Stagger` delays each stroke behind the one before it, with Forward / Reverse / Centre Out /
Random ordering.

This is the one piece of animation manual keyframing cannot replace. Trim is a *single*
window shared by every stroke, so there is no parameter you could key that would draw stroke
one and then stroke two — the effect would need a trim per stroke, which is a parameter slot
per stroke that nobody would ever key by hand.

Random is a deterministic shuffle from the seed, not a call to any random source: the same
seed must give the same order on every frame or the strokes reshuffle themselves during
playback. Centre Out is deliberately a *rank with ties* — the two strokes either side of the
middle share a turn — so only the other three are strict permutations. `linetest --sheet check`
asserts exactly that, registered with CTest, because two strokes assigned the same turn would
animate together and no still image would show it.

**Stagger is why styles are per-stroke.** A stagger means each stroke is at a different point
in the same animation, so an animated opacity or thickness is genuinely a different value per
stroke. One shared style cannot express that, and the strokes would all animate together —
precisely what stagger exists to avoid. Every `Segment` and `FillEdge` therefore carries a
style index, and the shading takes the style from whichever primitive owns the pixel. A
segment casts its shadow with **its own** offset, which is not merely more flexible but
necessary: the offset lives in the style, and which style applies is not known until the
nearest segment has been found.

### Wiggle

Seeded wobble along the path, for a hand-drawn feel. Amount, wavelength, phase and seed.

Determinism is the whole requirement — noise drawn from anything that varies per render
shimmers between frames, and a line that shimmers reads as broken rather than hand-drawn. It
is an integer avalanche hash, not the `sin(x)*43758.5` trick, which is famously
platform-dependent: exactly the property this must not have when CPU and GPU are compared.
Two octaves, because one is recognisably a sine wave.

The path is **resampled** before displacement. Without that the wobble could only appear
where the flattener happened to put vertices, and a straight span has exactly two — so a
straight line would merely have its endpoints nudged. Same failure the width profile hit,
same fix.

Displacement is **perpendicular** to the local direction rather than a free 2D offset, which
would stretch and compress the path as well as bending it and make the line visibly change
length as it wobbles. The noise is indexed by **arc length**, so the wobble is pinned to the
path and stays put when the line is trimmed or dashed, and animating Phase makes it crawl
rather than re-roll. Each stroke is seeded separately or a multi-stroke shape wobbles in
lockstep and reads as one rigid squiggle.

`linetest --sheet wiggle` covers a straight span, phase crawl, and the interactions with
dashes, taper and arrowheads.

## Roadmap

| # | | |
|---|---|---|
| 0 | Host probe + parameter-count spike | **blocked on a Resolve run** |
| 1 | CMake, scripts, bundle, loading plugin | done |
| 2 | Core geometry, SDF, tile grid, `linetest` | done |
| 3 | CPU render path visible in Resolve | written, unverified |
| 4 | CUDA path + `cudatest` parity | done, 8/8 agree |
| 5 | Overlay: point/tangent editing, keyframe-aware writes | written, **untestable offline** |
| 6 | Arrowheads, drop shadow, fill for closed paths, gradient rework | done |
| 7 | Auto in/out with target choice, anchors, easing | done (stagger + wiggle outstanding) |
| 8 | Easing curve editor in the overlay | done (no saved-curve library) |
| 9 | Measured host table written up here | |
