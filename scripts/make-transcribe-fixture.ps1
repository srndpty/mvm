[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureDir = Join-Path $repoRoot 'tests/assets/transcribe'
New-Item -ItemType Directory -Force -Path $fixtureDir | Out-Null
$voice = New-Object -ComObject SAPI.SpVoice
$stream = New-Object -ComObject SAPI.SpFileStream
$stream.Format.Type = 22
$stream.Open((Join-Path $fixtureDir 'speech.wav'), 3, $false)
try {
    $voice.AudioOutputStream = $stream
    $voice.Speak('This is a short caption test. The video editor displays these words.') | Out-Null
} finally { $stream.Close() }
Write-Host '音声を再生せず、固定本文の認識用音声を生成しました。'
