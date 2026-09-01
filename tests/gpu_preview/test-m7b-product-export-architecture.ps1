$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$source = Get-Content -Raw (Join-Path $repoRoot "src/media/mlt/mvm_mlt_export.c")
$requiredProperties = @('"fill"', '"distort"', '"b_alpha"', '"repeat_off"', '"mirror_off"', '"keyed"', '"halign"', '"valign"')
$hasFixedProperties = (@($requiredProperties | Where-Object { -not $source.Contains($_) }).Count -eq 0)
$checks = @(
    @{ Name = "tractor graph"; Pass = $source.Contains("mlt_tractor_new") -and $source.Contains("mlt_tractor_set_track") },
    @{ Name = "timeline absolute transition range"; Pass = $source.Contains("clip->timeline_start_frame + clip->timeline_duration_frames - 1") },
    @{ Name = "transition local rect keys"; Pass = $source -match '(?s)anim_set_rect\(props, "rect".*key->local_frame' },
    @{ Name = "P0 rotation property"; Pass = $source.Contains('"fix_rotate_x"') -and -not $source.Contains('MLT_TRANSITION_PROPERTIES(transition), "fix_rotate_z"') },
    @{ Name = "P0 fixed properties"; Pass = $hasFixedProperties },
    @{ Name = "V2 opaque affine filter forbidden"; Pass = $source.Contains("V2へopaque-black affine filterをattachしてはならない") -and $source.Contains("opaque_black_affine_filter_count = 0") }
)
$failed = @($checks | Where-Object { -not $_.Pass })
foreach ($check in $checks) { Write-Host "$(if ($check.Pass) {'PASS'} else {'FAIL'}) $($check.Name)" }
if ($checks.Count -eq 0 -or $failed.Count -ne 0) { exit 1 }
exit 0
