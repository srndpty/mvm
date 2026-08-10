[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$PublicDir)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$forbidden = @(
    'AVFrame',
    'ID3D11',
    'QRhi',
    'QQuick',
    'SourceDecodeWorker',
    'CompositorCoordinator',
    'CompositorSpikeState',
    'SourceGeneration',
    'ResourceEpoch',
    'CompositionEpoch',
    'mutex',
    'condition_variable'
)

$headers = Get-ChildItem -LiteralPath $PublicDir -Filter '*.h' -File
if ($headers.Count -eq 0) {
    throw "public headerが0件です: $PublicDir"
}

$violations = @()
foreach ($header in $headers) {
    $content = Get-Content -LiteralPath $header.FullName -Raw
    foreach ($token in $forbidden) {
        if ($content -match [regex]::Escape($token)) {
            $violations += "$($header.Name): forbidden token '$token'"
        }
    }
    foreach ($include in [regex]::Matches($content, '#include\s+[<"]([^>"]+)[>"]')) {
        $name = $include.Groups[1].Value
        if ($name -match '^(Qt|Q[A-Z])|d3d|dxgi|libav|ffmpeg|gpu_preview|audio_preview|app/preview') {
            $violations += "$($header.Name): forbidden include '$name'"
        }
    }
}

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host "public header dependency検査: $($headers.Count)件、違反なし"
