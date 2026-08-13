# Open: generator animation starts at the wrong frame

**Status:** diagnosed to a hypothesis, not yet confirmed. One experiment settles it.

## Symptom

With **Better Line Generator** on the Edit page, the in/out animation does not start where the
clip starts. The **out** animation appears correct. The plain **Better Line** effect on a clip
or adjustment clip is correct at both ends.

## Evidence

`%LOCALAPPDATA%\BetterLine\probe.log`, one `cliptime:` line per Resolve session:

```
effect     t1=86381 t2=86420  args.time=86401  -> ABSOLUTE   clipFrame=20  length=40
generator  t1=0     t2=119    args.time=21     -> relative   clipFrame=21  length=120
```

The effect line is decisive: `t2-t1+1 = 40` on a 40-frame clip, and t1 sits at 01:00:00:00
(86400 @ 24fps). **For an effect, Resolve narrows `timeLineGetBounds` to the clip.** That is why
`args.time - t1` lands on the clip's first frame.

## Why the first hypothesis was wrong

Initially blamed the auto origin heuristic in `src/ClipTime.h`:

```cpp
absolute = boundsOk && t1 > 1.0 && argsTime > t1 - 1.0 && argsTime < t2 + 1.0;
```

`t1 > 1.0` is false for the generator's 0-based bounds, so it picks `relative`. But that alone
cannot be the whole story, and the user's own observation is what rules it out:

> the end animation is correct, so if they can recognize the end of the animation, they can
> definitely recognize the start

If `clipFrame` were simply offset, **both** stages would shift together — an offset displaces
the end exactly as much as the start. So a uniform offset does not produce "end right, start
wrong".

## The hypothesis that does fit

The anchors are **not** symmetric (`src/core/Anim.h:107-110`):

```cpp
case kAnchorClipStart: return clipFrame;            // does NOT use length
case kAnchorClipEnd:   return clipFrame - last;     // uses length
```

Suppose that for the generator `timeLineGetBounds` reports the **TIMELINE**, not the clip, and
`args.time` is timeline-absolute:

- `length` = the timeline's length, not the clip's.
- **In** stage (Clip Start anchor) fires at `clipFrame == 0` — timeline frame 0, *before* the
  generator clip begins. By the time the clip is visible the draw-on has already completed, so
  it looks like there is no in-animation at all.
- **Out** stage (Clip End anchor) fires at `clipFrame == last` — the *timeline's* last frame.
  **If the generator clip runs to the end of the timeline, this coincidentally lands correctly.**

Same wrong bounds; one stage right by accident. That is exactly the reported asymmetry.

## The experiment that settles it

**Move the generator clip so it does NOT touch the end of the timeline** — put it in the middle,
with empty space after it. Then:

- If the **out** animation now also fires at the wrong time (at the timeline's end rather than
  the clip's), the hypothesis is confirmed: bounds are the timeline, not the clip.
- If the out animation still lands on the clip's last frame, the hypothesis is wrong and the
  bounds really are the clip's — look elsewhere, and capture a fresh `cliptime:` line with the
  clip at a known non-zero timeline position.

Either way, **delete `probe.log` first** so the new `cliptime:` line is unambiguous — it is
written once per Resolve process, so old sessions clutter the tail.

## If confirmed, candidate fixes

`timeLineGetBounds` cannot be the source of truth for a generator. Options, roughly in order of
preference:

1. **`_dstClip->getFrameRange(t1, t2)`** — the output clip exists in the generator context (it is
   the only clip that does). Check whether Resolve reports the generator clip's own range there.
   Log it next to the existing `cliptime:` line before trusting it.
2. **`getEffectDuration()`** — already used as a last-resort length. Reports the *available*
   span rather than the visible one, so it is wrong when the clip is trimmed, but it may still be
   the clip rather than the timeline.
3. **Widen the auto heuristic** so a 0-based range is not automatically read as clip-relative.
   Weakest option: it cannot distinguish "0-based clip range" from "0-based timeline" without a
   second source of truth, which is the whole problem.

Whatever is chosen must keep the **effect** path byte-identical — it is correct today, and the
probe log above is the reference for what correct looks like.

## Workaround today

Set **Time Offset** to minus the generator's start frame. Crude, but it is exactly the escape
hatch that control exists for.

**Time Origin = Timeline Absolute does NOT help here** — it subtracts `t1`, which is 0.

## Relevant code

| What | Where |
|---|---|
| Origin/length resolution, and the `cliptime:` log line | `src/ClipTime.h:70-140` |
| Anchor asymmetry (start ignores length, end uses it) | `src/core/Anim.h:107-110` |
| `StageUsable` — skips length-dependent anchors when length is unknown | `src/core/Anim.h:148` |
| Where timing is requested per render | `src/BetterLinePlugin.cpp:1527` |
| Generator context declaration and RoD | `src/BetterLinePlugin.cpp`, `getRegionOfDefinition` |

## Prior context

The two sibling plugins disagree about clip time, and this plugin follows
`resolve-text-animator`'s conclusion (use `args.time`, subtract nothing, keep no learned state)
with an explicit Auto/Relative/Absolute override because Resolve gets it differently wrong on
different clip types. That history is in the README's host-caveats section; this is a third clip
type behaving differently again.
