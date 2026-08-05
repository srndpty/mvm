<#
.SYNOPSIS
    resolve-proxy の out ファイル契約を検査する (S7.1)。

.DESCRIPTION
    終了コードだけでは見えない部分を検査する。

      1. required 解決に失敗したとき、out を新規作成しない
      2. required 解決に失敗したとき、既存の out を変更しない
      3. 失敗時に一時ファイル (*.mvmtmp) を残さない
      4. 成功時は out に proxy パスが入る
      5. final の出力に proxy パスが 1 件も無い
      6. optional (音声専用 source / WAV) は original のまま残る

    1 と 2 が守られていないと、失敗した実行が「それらしい scenario」を残し、
    次の測定がそれを拾って **proxy が効いていない構成で測ってしまう**。
    実際 S7 の M8 は V2 が original のまま「proxy 評価」として報告されていた。

.EXAMPLE
    pwsh scripts/check-resolver-contract.ps1 -Bench build/ucrt64-release/bin/mvm_bench.exe `
        -Scenario bench/scenarios/s7-4k-original.json -WorkDir build/tmp/resolver
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Bench,
    [Parameter(Mandatory)][string]$Scenario,
    [Parameter(Mandatory)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$V1  = 'v4k60_h264.mp4'
$V2  = 'v1080p60_hevc.mp4'
$A1  = 'v1080p60_h264.mp4'   # optional。音声専用に使う video ファイル
$A2  = 'wav_48k.wav'         # optional。WAV
$PX1 = '_proxy/v4k60_h264_proxy_gop12.mp4'
$PX2 = '_proxy/v1080p60_hevc_proxy_gop12.mp4'
$MAP_OK   = "$V1=$PX1;$V2=$PX2"
$MAP_BAD  = "$V1=$PX1;$V2=_proxy/mvm_no_such_proxy.mp4"
$REQ      = "$V1;$V2"

if (-not (Test-Path $Bench))    { throw "mvm_bench がありません: $Bench" }
if (-not (Test-Path $Scenario)) { throw "scenario がありません: $Scenario" }

if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir | Out-Null

$failures = New-Object System.Collections.Generic.List[string]
function Check {
    param([string]$What, [bool]$Ok, [string]$Detail = '')
    if ($Ok) {
        Write-Host "  OK  $What"
    } else {
        Write-Host "  NG  $What $Detail" -ForegroundColor Red
        $failures.Add("$What $Detail")
    }
}

function Invoke-Bench {
    param([string[]]$ChildArgs)
    & $Bench @ChildArgs 2>&1 | Out-Null
    return $LASTEXITCODE
}

function Get-TempLeftovers {
    return @(Get-ChildItem -Path $WorkDir -Filter '*.mvmtmp' -Recurse -File -ErrorAction SilentlyContinue)
}

# --- 1) 失敗時に out を新規作成しない ---------------------------------------
Write-Host "`n[1] required 解決に失敗したとき out を作らない"
$out1 = Join-Path $WorkDir 'never-created.json'
$code = Invoke-Bench @('resolve-proxy', $Scenario, '--target', 'preview',
                       '--map', $MAP_BAD, '--require-proxy-ids', $REQ, '--out', $out1)
Check 'exit 4 で落ちる' ($code -eq 4) "(実際 $code)"
Check 'out が作られていない' (-not (Test-Path $out1))
Check '一時ファイルが残っていない' ((@(Get-TempLeftovers)).Count -eq 0)

# --- 2) 成功時は out に proxy が入る ----------------------------------------
Write-Host "`n[2] 成功時の out"
$out2 = Join-Path $WorkDir 'preview.json'
$code = Invoke-Bench @('resolve-proxy', $Scenario, '--target', 'preview',
                       '--map', $MAP_OK, '--require-proxy-ids', $REQ, '--out', $out2)
Check 'exit 0' ($code -eq 0) "(実際 $code)"
Check 'out が作られた' (Test-Path $out2)
$prev = Get-Content $out2 -Raw
Check "V1 が proxy になっている" ($prev -match [regex]::Escape($PX1))
Check "V2 が proxy になっている" ($prev -match [regex]::Escape($PX2))

# --- 3) optional は original のまま ------------------------------------------
Write-Host "`n[3] optional source は original のまま"
$prevJson = $prev | ConvertFrom-Json
$sources = @()
foreach ($t in $prevJson.tracks) {
    foreach ($c in $t.clips) {
        # 文字トラックの clip は source を持たない。
        if ($c.PSObject.Properties['source']) { $sources += $c.source }
    }
}
Check "A1 ($A1) が original のまま" ($sources -contains $A1)
Check "A2 ($A2) が original のまま" ($sources -contains $A2)

# --- 4) 失敗時に既存の out を変更しない --------------------------------------
Write-Host "`n[4] required 解決に失敗したとき既存の out を変更しない"
$before = (Get-FileHash $out2 -Algorithm SHA256).Hash
$code = Invoke-Bench @('resolve-proxy', $Scenario, '--target', 'preview',
                       '--map', $MAP_BAD, '--require-proxy-ids', $REQ, '--out', $out2)
$after = (Get-FileHash $out2 -Algorithm SHA256).Hash
Check 'exit 4 で落ちる' ($code -eq 4) "(実際 $code)"
Check '既存 out の内容が変わっていない' ($before -eq $after)
Check '一時ファイルが残っていない' ((@(Get-TempLeftovers)).Count -eq 0)

# --- 5) final には proxy パスが 1 件も無い -----------------------------------
Write-Host "`n[5] final は必ず original"
$out5 = Join-Path $WorkDir 'final.json'
$code = Invoke-Bench @('resolve-proxy', $Scenario, '--target', 'final',
                       '--map', $MAP_OK, '--require-proxy-ids', $REQ, '--out', $out5)
Check 'exit 0' ($code -eq 0) "(実際 $code)"
Check 'out が作られた' (Test-Path $out5)
$fin = Get-Content $out5 -Raw
Check "final に $PX1 が無い" (-not ($fin -match [regex]::Escape($PX1)))
Check "final に $PX2 が無い" (-not ($fin -match [regex]::Escape($PX2)))
Check 'final に _proxy/ が一切無い' (-not ($fin -match '_proxy/'))

# --- 結果 --------------------------------------------------------------------
if ($failures.Count -ne 0) {
    Write-Host "`nresolver の out 契約に違反があります ($($failures.Count) 件):" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}
Write-Host "`nresolver の out 契約はすべて満たされています。"
exit 0
