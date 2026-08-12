# Install the Fusion macro and the Edit-page templates.
#
# All three are the SAME file format -- a GroupOperator in a .setting. Only the
# folder decides what Resolve calls it:
#
#   Fusion\Macros                       -> a macro on the Fusion page
#   Fusion\Templates\Edit\Effects       -> a Fusion Effect in the Edit page's
#                                          Effects library, dragged onto a clip
#   Fusion\Templates\Edit\Generators    -> a Generator, dragged onto the
#                                          timeline as its own clip
#
# Deliberately NOT elevated. These live under %APPDATA%, which the user already
# owns -- unlike the .ofx bundle in Program Files. Running this as admin would
# install them into the ADMINISTRATOR's profile, where Resolve would never look.

$ErrorActionPreference = "Stop"

$repo    = Split-Path -Parent $PSScriptRoot
$srcDir  = Join-Path $repo "fusion"
$support = Join-Path $env:APPDATA "Blackmagic Design\DaVinci Resolve\Support\Fusion"

if (-not (Test-Path $srcDir)) {
    throw "No fusion\ directory. Run: python tools/make_fusion_macro.py"
}

# Subfoldered so it shows up under its own heading rather than loose among
# whatever else is installed.
$targets = @(
    @{ Src = "BetterLine.setting";                  Dst = "Macros" },
    @{ Src = "Edit\Effects\BetterLine.setting";     Dst = "Templates\Edit\Effects\Better Line" },
    @{ Src = "Edit\Generators\BetterLine.setting";  Dst = "Templates\Edit\Generators\Better Line" }
)

foreach ($t in $targets) {
    $src = Join-Path $srcDir $t.Src
    if (-not (Test-Path $src)) { throw "Missing $src -- regenerate it first." }

    $dstDir = Join-Path $support $t.Dst
    New-Item -ItemType Directory -Force $dstDir | Out-Null

    $dst = Join-Path $dstDir "BetterLine.setting"
    Copy-Item $src $dst -Force

    # Verified by content, not by the copy returning without error: a partial
    # write leaves a file that exists and does not parse, which surfaces much
    # later as "the macro is missing from the menu".
    $a = (Get-FileHash $src -Algorithm SHA256).Hash
    $b = (Get-FileHash $dst -Algorithm SHA256).Hash
    if ($a -ne $b) { throw "Copy verification FAILED for $dst" }

    "installed  $dst"
}

""
"Restart DaVinci Resolve. Then:"
"  Fusion page  -> Effects Library > Macros > BetterLine"
"  Edit page    -> Effects > Video Effects > Better Line   (drop on a clip)"
"  Edit page    -> Effects > Generators   > Better Line   (drop on the timeline)"
