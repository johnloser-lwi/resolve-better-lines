#!/usr/bin/env python3
"""Generate the Fusion macro that wraps the Better Line OFX node.

Run:  python tools/make_fusion_macro.py
Out:  fusion/BetterLine.setting

Why a generator rather than a checked-in hand-written file: the control list is
the plugin's parameter list, and that moves. Every parameter added to
ParamNames.h needs a matching row here or it silently will not appear on the
macro -- a hand-maintained copy of 120 identifiers drifts on the first busy
afternoon. The manifest below is the only thing to edit.

The identifiers are NOT guesses. They were read back from a live Fusion comp:
Fusion names an OFX node `ofx.<plugin identifier>` and exposes each parameter
under its OFX name, so `thickness` stays `thickness` -- but colours are split
into four separate scalar inputs (`colorRed` .. `colorAlpha`) and there is no
combined control, which is the sort of thing that has to be looked at rather
than assumed.
"""

import os

OFX_ID   = "ofx.com.resolvebetterline.BetterLine"
LINE     = "BLLine"      # inner OFX node
SHADOW   = "BLShadow"    # inner Fusion Shadow node
BG       = "BLCanvas"    # generator-only transparent canvas

# (page, [(input id, label), ...]) on the OFX node.
#
# Deliberately WITHOUT the per-point geometry slots (s0p0_pos ... s3p15_tan).
# Two reasons, and the second is the one that matters:
#
#  - There are 128 of them, and they would double the macro's control count.
#  - The plugin hides unused points dynamically via setIsSecret. A macro's
#    instance inputs have static visibility, so exposing them would show all
#    sixteen points of all four strokes permanently, including the ones the
#    plugin has hidden. Worse than the raw node, not better.
#
# Points are edited on the viewer overlay anyway. Open the group and select the
# inner node to get it.
PAGES = [
    ("Strokes", [
        ("strokeCount", "Strokes"),
        ("s0_enable", "Stroke 1 Enabled"), ("s0_count", "Stroke 1 Points"),
        ("s0_closed", "Stroke 1 Closed"),
        ("s1_enable", "Stroke 2 Enabled"), ("s1_count", "Stroke 2 Points"),
        ("s1_closed", "Stroke 2 Closed"),
        ("s2_enable", "Stroke 3 Enabled"), ("s2_count", "Stroke 3 Points"),
        ("s2_closed", "Stroke 3 Closed"),
        ("s3_enable", "Stroke 4 Enabled"), ("s3_count", "Stroke 4 Points"),
        ("s3_closed", "Stroke 4 Closed"),
    ]),
    ("Shape", [
        ("shapeKind", "Shape"), ("shapeSides", "Sides"),
        ("shapeCorner", "Corner Radius"), ("shapeLinkWH", "Lock Width to Height"),
        ("shapeWidth", "Width"), ("shapeSize", "Height"),
        ("shapeTarget", "Into"), ("shapeAdd", "Add Shape"),
    ]),
    ("Import", [
        ("importEnabled", "Show Imported Path"),
        ("importUseColours", "Use Imported Colours"),
        ("importUseWidths", "Use Imported Widths"),
        ("importWidthScale", "Imported Width Scale"),
        ("importSvg", "Import SVG..."), ("clearImport", "Clear Import"),
    ]),
    ("Transform", [
        ("xformPosition", "Position"), ("xformRotation", "Rotation"),
        ("xformScale", "Scale"), ("xformScaleY", "Scale Y"),
        ("xformScaleLink", "Link Scale"), ("xformAnchor", "Anchor"),
    ]),
    ("Style", [
        ("colorRed", "Colour Red"), ("colorGreen", "Colour Green"),
        ("colorBlue", "Colour Blue"), ("colorAlpha", "Colour Alpha"),
        ("opacity", "Opacity"), ("thickness", "Thickness"),
        ("feather", "Softness"), ("cap", "Caps"),
        ("taperStart", "Taper Start"), ("taperBody", "Taper Body"),
        ("taperEnd", "Taper End"), ("taperFollowsTrim", "Taper Follows Trim"),
        ("fill", "Fill Closed Paths"),
        ("fillColorRed", "Fill Red"), ("fillColorGreen", "Fill Green"),
        ("fillColorBlue", "Fill Blue"), ("fillColorAlpha", "Fill Alpha"),
        ("fillOpacity", "Fill Opacity"),
    ]),
    ("Effects", [
        ("outline", "Outline"), ("outlineWidth", "Outline Width"),
        ("outlineColorRed", "Outline Red"), ("outlineColorGreen", "Outline Green"),
        ("outlineColorBlue", "Outline Blue"), ("outlineColorAlpha", "Outline Alpha"),
        ("glow", "Glow"), ("glowRadius", "Glow Radius"),
        ("glowIntensity", "Glow Intensity"),
        ("glowColorRed", "Glow Red"), ("glowColorGreen", "Glow Green"),
        ("glowColorBlue", "Glow Blue"), ("glowColorAlpha", "Glow Alpha"),
        ("shadow", "Built-in Drop Shadow"), ("shadowOffset", "Built-in Shadow Offset"),
        ("shadowSoftness", "Built-in Shadow Softness"),
        ("shadowOpacity", "Built-in Shadow Opacity"),
        ("shadowColorRed", "Built-in Shadow Red"),
        ("shadowColorGreen", "Built-in Shadow Green"),
        ("shadowColorBlue", "Built-in Shadow Blue"),
        ("shadowColorAlpha", "Built-in Shadow Alpha"),
    ]),
    ("Arrows & Dashes", [
        ("arrowStart", "Arrow at Start"), ("arrowEnd", "Arrow at End"),
        ("arrowLength", "Arrow Length"), ("arrowWidth", "Arrow Width"),
        ("arrowPosition", "Arrow Position"), ("arrowRoundness", "Arrow Roundness"),
        ("dash", "Dashed"), ("dashLength", "Dash Length"),
        ("dashGap", "Gap Length"), ("dashPhase", "Dash Offset"),
    ]),
    ("Gradient", [
        ("gradMode", "Gradient"),
        ("gradColorRed", "Gradient Red"), ("gradColorGreen", "Gradient Green"),
        ("gradColorBlue", "Gradient Blue"), ("gradColorAlpha", "Gradient Alpha"),
        ("gradStart", "Gradient Start"), ("gradEnd", "Gradient End"),
        ("gradMidpoint", "Gradient Midpoint"),
        ("gradSeamless", "Seamless on Closed Paths"),
    ]),
    ("Trim", [
        ("trimStart", "Trim Start"), ("trimEnd", "Trim End"),
        ("trimOffset", "Trim Offset"), ("trimWrap", "Wrap on Closed Paths"),
        ("wiggle", "Wiggle"), ("wiggleAmount", "Wiggle Amount"),
        ("wiggleWavelength", "Wiggle Wavelength"),
        ("wigglePhase", "Wiggle Phase"), ("wiggleSeed", "Wiggle Seed"),
    ]),
    ("Animation", [
        ("animate", "Animate"), ("timeOrigin", "Time Origin"),
        ("timeOffset", "Time Offset"), ("clipLength", "Clip Length"),
        ("inEnable", "In Enabled"), ("inTarget", "In Target"),
        ("inFrom", "In From"), ("inTo", "In To"), ("inAnchor", "In Anchor"),
        ("inStart", "In Start"), ("inDuration", "In Duration"),
        ("inEase", "In Easing"), ("inEaseIn", "In Ease In %"),
        ("inEaseOut", "In Ease Out %"), ("inAnticipation", "In Anticipation %"),
        ("inOvershoot", "In Overshoot %"),
        ("outEnable", "Out Enabled"), ("outTarget", "Out Target"),
        ("outFrom", "Out From"), ("outTo", "Out To"), ("outAnchor", "Out Anchor"),
        ("outStart", "Out Start"), ("outDuration", "Out Duration"),
        ("outEase", "Out Easing"), ("outEaseIn", "Out Ease In %"),
        ("outEaseOut", "Out Ease Out %"),
        ("outAnticipation", "Out Anticipation %"),
        ("outOvershoot", "Out Overshoot %"),
        ("stagger", "Stagger"), ("staggerOrder", "Stagger Order"),
        ("staggerSeed", "Stagger Seed"),
        ("AllowResize", "Use plugin RoD for output size"),
    ]),
]

# The Fusion Shadow node bolted on downstream. This is the point of the macro:
# a soft shadow cast from the LINE'S OWN ALPHA, computed inside Fusion where the
# alpha survives, rather than depending on the host carrying alpha out of the
# effect and into a separate one.
SHADOW_CONTROLS = [
    ("Blend",         "Shadow Blend"),
    ("ShadowOffset",  "Shadow Offset"),
    ("Softness",      "Shadow Softness"),
    ("Red",           "Shadow Red"),
    ("Green",         "Shadow Green"),
    ("Blue",          "Shadow Blue"),
    ("Alpha",         "Shadow Alpha"),
    ("LightPosition", "Shadow Light Position"),
    ("LightDistance", "Shadow Light Distance"),
    ("OutputMode",    "Shadow Output"),
]


def build(macro_name, background):
    """Emit one .setting.

    @param background  give the OFX node a comp-sized transparent canvas to draw
                       on, instead of leaving its input for the user to connect.

    An Edit-page GENERATOR has nothing upstream by definition, and an unconnected
    input leaves the output size to the plugin's own region of definition. That
    does now resolve to the project extent, but a Background with
    UseFrameFormatSettings makes the canvas explicit rather than inferred -- the
    size stops depending on a host negotiation the Edit page gives no way to
    inspect. It defaults to fully transparent, so it adds nothing visible.
    """
    L = []
    a = L.append

    a("{")
    a("\tTools = ordered() {")
    a("\t\t%s = GroupOperator {" % macro_name)
    a("\t\t\tCtrlWZoom = false,")
    a("\t\t\tNameSet = true,")
    a("\t\t\tInputs = ordered() {")

    # The image input first: Fusion treats MainInput1 as the node's primary
    # connection, which is what puts the arrow on the left of the tile. A
    # generator has nothing upstream, so it exposes no image input at all.
    if not background:
        a('\t\t\t\tMainInput1 = InstanceInput {')
        a('\t\t\t\t\tSourceOp = "%s",' % LINE)
        a('\t\t\t\t\tSource = "Source",')
        a('\t\t\t\t\tName = "Input",')
        a('\t\t\t\t},')

    # No Default is emitted on any instance input, deliberately. An InstanceInput
    # carrying a Default OVERRIDES the value the plugin declared, so writing one
    # here would quietly replace every real default -- Strokes would arrive as 0
    # rather than 1, Thickness as 0 rather than its declared width. Omitting it
    # lets each control inherit what the plugin already says it should be.
    n = 1
    for page, controls in PAGES:
        for src, label in controls:
            n += 1
            a('\t\t\t\tInput%d = InstanceInput {' % n)
            a('\t\t\t\t\tSourceOp = "%s",' % LINE)
            a('\t\t\t\t\tSource = "%s",' % src)
            a('\t\t\t\t\tName = "%s",' % label)
            a('\t\t\t\t\tPage = "%s",' % page)
            a('\t\t\t\t},')

    for src, label in SHADOW_CONTROLS:
        n += 1
        a('\t\t\t\tInput%d = InstanceInput {' % n)
        a('\t\t\t\t\tSourceOp = "%s",' % SHADOW)
        a('\t\t\t\t\tSource = "%s",' % src)
        a('\t\t\t\t\tName = "%s",' % label)
        a('\t\t\t\t\tPage = "Shadow",')
        a('\t\t\t\t},')

    a("\t\t\t},")
    a("\t\t\tOutputs = {")
    a("\t\t\t\tMainOutput1 = InstanceOutput {")
    a('\t\t\t\t\tSourceOp = "%s",' % SHADOW)
    a('\t\t\t\t\tSource = "Output",')
    a("\t\t\t\t},")
    a("\t\t\t},")
    a("\t\t\tViewInfo = GroupInfo {")
    a("\t\t\t\tPos = { 0, 0 },")
    a("\t\t\t\tFlags = {")
    a("\t\t\t\t\tAllowPan = false,")
    a("\t\t\t\t\tGridSnap = true,")
    a("\t\t\t\t\tAutoSnap = true,")
    a("\t\t\t\t},")
    a("\t\t\t\tSize = { 300, 172.606, 150, 24.2424 },")
    a('\t\t\t\tDirection = "Horizontal",')
    a('\t\t\t\tPipeStyle = "Direct",')
    a("\t\t\t\tScale = 1,")
    a("\t\t\t\tOffset = { 0, 0 },")
    a("\t\t\t},")
    a("\t\t\tTools = ordered() {")

    if background:
        a("\t\t\t\t%s = Background {" % BG)
        a("\t\t\t\t\tCtrlWZoom = false,")
        a("\t\t\t\t\tNameSet = true,")
        a("\t\t\t\t\tInputs = {")
        # 1 = follow the comp's frame format, so the canvas is the timeline's
        # size. The Width/Height below are only the fallback Fusion falls back
        # to if that setting is ever switched off.
        a("\t\t\t\t\t\tUseFrameFormatSettings = Input { Value = 1, },")
        a("\t\t\t\t\t\tWidth = Input { Value = 1920, },")
        a("\t\t\t\t\t\tHeight = Input { Value = 1080, },")
        a("\t\t\t\t\t\tTopLeftRed = Input { Value = 0, },")
        a("\t\t\t\t\t\tTopLeftGreen = Input { Value = 0, },")
        a("\t\t\t\t\t\tTopLeftBlue = Input { Value = 0, },")
        a("\t\t\t\t\t\tTopLeftAlpha = Input { Value = 0, },")
        a("\t\t\t\t\t},")
        a("\t\t\t\t\tViewInfo = OperatorInfo { Pos = { -110, 0 } },")
        a("\t\t\t\t},")

    a("\t\t\t\t%s = %s {" % (LINE, OFX_ID))
    a("\t\t\t\t\tCtrlWZoom = false,")
    a("\t\t\t\t\tNameSet = true,")
    if background:
        a("\t\t\t\t\tInputs = {")
        a('\t\t\t\t\t\tSource = Input {')
        a('\t\t\t\t\t\t\tSourceOp = "%s",' % BG)
        a('\t\t\t\t\t\t\tSource = "Output",')
        a("\t\t\t\t\t\t},")
        a("\t\t\t\t\t},")
    a("\t\t\t\t\tViewInfo = OperatorInfo { Pos = { 0, 0 } },")
    a("\t\t\t\t},")

    a("\t\t\t\t%s = Shadow {" % SHADOW)
    a("\t\t\t\t\tCtrlWZoom = false,")
    a("\t\t\t\t\tNameSet = true,")
    a("\t\t\t\t\tInputs = {")
    # Off by default: a macro that silently adds a shadow to every line would be
    # a surprise, and Blend 0 leaves the Shadow node a pass-through.
    a("\t\t\t\t\t\tBlend = Input { Value = 0, },")
    a("\t\t\t\t\t\tSoftness = Input { Value = 2, },")
    a("\t\t\t\t\t\tShadowOffset = Input { Value = { 0.51, 0.49 }, },")
    a('\t\t\t\t\t\tInput = Input {')
    a('\t\t\t\t\t\t\tSourceOp = "%s",' % LINE)
    a('\t\t\t\t\t\t\tSource = "Output",')
    a("\t\t\t\t\t\t},")
    a("\t\t\t\t\t},")
    a("\t\t\t\t\tViewInfo = OperatorInfo { Pos = { 110, 0 } },")
    a("\t\t\t\t},")

    a("\t\t\t},")
    a("\t\t},")
    a("\t},")
    a("}")
    return "\n".join(L) + "\n"


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s" % path)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "fusion")

    # An Edit-page template and a Fusion macro are THE SAME FILE FORMAT -- only
    # the folder decides which. Templates/Edit/Effects makes it a Fusion Effect
    # on the Edit page, Templates/Edit/Generators makes it a Generator, and the
    # Macros folder makes it a Fusion-page macro. Verified by reading a shipped
    # generator template, which is a plain GroupOperator with no MediaIn or
    # MediaOut of its own.
    effect = build("BetterLine", background=False)
    write(os.path.join(out, "BetterLine.setting"), effect)
    write(os.path.join(out, "Edit", "Effects", "BetterLine.setting"), effect)

    generator = build("BetterLineGenerator", background=True)
    write(os.path.join(out, "Edit", "Generators", "BetterLine.setting"), generator)

    total = sum(len(c) for _, c in PAGES) + len(SHADOW_CONTROLS)
    print("  %d exposed controls across %d pages + Shadow" % (total, len(PAGES)))


if __name__ == "__main__":
    main()
