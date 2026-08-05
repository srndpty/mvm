<#
.SYNOPSIS
    4K の logical timeline から original / proxy の scenario を生成する (S7 / H)。

.DESCRIPTION
    同じ logical timeline から
      - original scenario (4K をそのまま)
      - proxy-resolved scenario (preview 用に proxy へ差し替え)
    を生成する。

    差し替えは **MLT graph を構築する前** に scenario 上で行う。
    proxy 情報を MLT XML にも MLT property にも持たせない。
    解決そのものは mvm_bench resolve-proxy (tests/harness/proxy_resolver.h) が行う。

    final 用は必ず original を返すので、ここでは preview 用だけを生成する。
    final の scenario は original scenario と同一である。

.EXAMPLE
    pwsh scripts/make-proxy-scenarios.ps1
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$Bench     = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
$ScenDir   = Join-Path $RepoRoot 'bench\scenarios'
$ProxyDir  = Join-Path $RepoRoot 'tests\assets\benchmark\_proxy'
if (-not (Test-Path $Bench)) { throw "mvm_bench がありません: $Bench" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

# --- logical timeline (4K 版) -----------------------------------------------
# 構造は S5 / S7 の 5 トラックと同じ。V1 を 4K にしたもの。
$src = Get-Content (Join-Path $ScenDir 's7-five-track-60s.json') -Raw | ConvertFrom-Json

$orig = $src | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$orig.name = 's7-4k-original'
$orig.'//' = 'S7 の 4K logical timeline (original)。preview 用の proxy 版は resolve-proxy が生成する。'
# V1 を 4K 素材に差し替える。他のトラックは 1080p のままでよい
# (proxy の効果を V1 の解像度差で見るため)。
$orig.tracks[0].clips[0].source = 'v4k60_h264.mp4'

$origPath = Join-Path $ScenDir 's7-4k-original.json'
$orig | ConvertTo-Json -Depth 20 | Set-Content $origPath -Encoding UTF8
Write-Host "書き出し: $origPath"

# --- proxy 版 ---------------------------------------------------------------
foreach ($cand in @('gop12', 'gop1')) {
    $proxyFile = Join-Path $ProxyDir "v4k60_h264_proxy_$cand.mp4"
    if (-not (Test-Path $proxyFile)) {
        Write-Host "proxy がまだありません ($cand)。先に pwsh scripts/proxy-matrix.ps1 を実行してください。" -ForegroundColor Yellow
        continue
    }
    $out = Join-Path $ScenDir "s7-4k-proxy-$cand.json"

    # media_root からの相対で書く。絶対パスを scenario に埋めない。
    $rel = "_proxy/v4k60_h264_proxy_$cand.mp4"

    $res = & $Bench resolve-proxy $origPath --target preview `
        --map "v4k60_h264.mp4=$rel" --require-proxy --out $out
    if ($LASTEXITCODE -ne 0) {
        Write-Host ($res | Out-String)
        throw "resolve-proxy が失敗しました ($cand)"
    }

    # 名前だけは後から書き換える (resolve-proxy は source しか触らない)。
    $o = Get-Content $out -Raw | ConvertFrom-Json
    $o.name = "s7-4k-proxy-$cand"
    $o | ConvertTo-Json -Depth 20 | Set-Content $out -Encoding UTF8
    Write-Host "書き出し: $out"

    # final は必ず original であることを、生成のたびに機械で確認する。
    $tmp = Join-Path $env:TEMP "mvm-final-check-$cand.json"
    & $Bench resolve-proxy $origPath --target final --map "v4k60_h264.mp4=$rel" --out $tmp | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "final の解決が失敗しました ($cand)" }
    if ((Get-Content $tmp -Raw) -match [regex]::Escape($rel)) {
        throw "final の scenario に proxy パスが混入しています ($cand)"
    }
    Remove-Item $tmp -ErrorAction SilentlyContinue
}

Write-Host "`nfinal 用の scenario は original scenario と同一です ($origPath)。"
