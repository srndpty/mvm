<#
.SYNOPSIS
    window の描画が必要な試験 (workstation の GUI・preview の試験) を実行できる環境かを確かめ、記録する。

.DESCRIPTION
    2026-10-13、monitor の電源が切られている間に全 CTest を実行し、window の描画が必要な 5 件が落ちた
    (docs/mlt-color-race-p35a2.md)。「monitor の電源が入っている」だけでは足りないので、次を全部確かめる。
      - 対話の session で、lock されていない
      - 接続中の monitor が 1 つ以上ある (PnP と WMI の両方)
      - Windows が報告する描画先の電源状態が on (自動消灯していない)
      - 実際に試験用の window を描画できる (mvm_test_fixed_test_window)
      - 実際に preview が frame を提示できる (mvm_p5c_preview_smoke)
    結果を JSON で書く。環境を直すことはしない (入力を送って画面を起こす、などはしない)。

    終了コード: 0 = VALID、2 = ENVIRONMENT_INVALID。

.EXAMPLE
    pwsh scripts/gui-environment-preflight.ps1 -OutputFile build/x/gui-environment.json
#>
param(
    [Parameter(Mandatory)][string]$OutputFile,
    [string]$BinDirectory = '',
    [switch]$SkipRenderProbe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BinDirectory) { $BinDirectory = Join-Path $repo 'build\ucrt64-release\bin' }
. (Join-Path $PSScriptRoot 'lib\test-display-lease.ps1')

Add-Type -AssemblyName System.Windows.Forms
if (-not ('Mvm.Tests.GuiEnvironment' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Mvm.Tests {
    public static class GuiEnvironment {
        [StructLayout(LayoutKind.Sequential)] struct LastInput { public uint Size; public uint Time; }
        [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LastInput info);
        [DllImport("user32.dll")] static extern uint GetDpiForSystem();
        [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
        [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
        [DllImport("kernel32.dll")] static extern uint WTSGetActiveConsoleSessionId();
        public static double IdleSeconds() {
            var info = new LastInput(); info.Size = 8; GetLastInputInfo(ref info);
            return (Environment.TickCount - (int)info.Time) / 1000.0;
        }
        public static uint SystemDpi() { return GetDpiForSystem(); }
        public static uint ConsoleSession() { return WTSGetActiveConsoleSessionId(); }
        // 入力を受ける desktop を開けるか (lock 画面・secure desktop の間は開けない)。
        public static bool InputDesktopAvailable() {
            IntPtr desktop = OpenInputDesktop(0, false, 0x0100);
            if (desktop == IntPtr.Zero) return false;
            CloseDesktop(desktop);
            return true;
        }
    }
}
'@
}

$reasons = [System.Collections.Generic.List[string]]::new()
$result = [ordered]@{ checked_at = (Get-Date -Format o); verdict = 'UNKNOWN'; reasons = $reasons }

# --- session ---
$session = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
$console = [Mvm.Tests.GuiEnvironment]::ConsoleSession()
$locked = @(Get-Process LogonUI -ErrorAction SilentlyContinue).Count -gt 0
$inputDesktop = [Mvm.Tests.GuiEnvironment]::InputDesktopAvailable()
$result.session = [ordered]@{ process_session = $session; console_session = $console; session_name = "$env:SESSIONNAME"
    user_interactive = [Environment]::UserInteractive; lock_screen_process = $locked; input_desktop_available = $inputDesktop
    idle_seconds = [math]::Round([Mvm.Tests.GuiEnvironment]::IdleSeconds(), 1)
}
if (-not [Environment]::UserInteractive) { $reasons.Add('対話の session ではありません') }
if ($session -ne $console) { $reasons.Add("console の session ($console) ではありません (session $session)") }
if ($locked -or -not $inputDesktop) { $reasons.Add('画面が lock されているか、入力を受ける desktop を開けません') }

# --- monitor ---
$pnp = @(Get-PnpDevice -Class Monitor -ErrorAction SilentlyContinue | ForEach-Object {
        [ordered]@{ name = $_.FriendlyName; status = "$($_.Status)"; present = [bool]$_.Present } })
$present = @($pnp | Where-Object { $_.present }).Count
$wmi = @(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBasicDisplayParams -ErrorAction SilentlyContinue | ForEach-Object {
        [ordered]@{ instance = $_.InstanceName; active = [bool]$_.Active; width_cm = $_.MaxHorizontalImageSize; height_cm = $_.MaxVerticalImageSize } })
$active = @($wmi | Where-Object { $_.active }).Count
$screens = @([System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
        [ordered]@{ device = $_.DeviceName; x = $_.Bounds.X; y = $_.Bounds.Y; width = $_.Bounds.Width; height = $_.Bounds.Height
            primary = $_.Primary; bits_per_pixel = $_.BitsPerPixel } })
$result.monitors = [ordered]@{ pnp = $pnp; pnp_present = $present; wmi = $wmi; wmi_active = $active; screens = $screens
    system_dpi = [Mvm.Tests.GuiEnvironment]::SystemDpi()
}
if ($present -lt 1) { $reasons.Add('接続中の monitor がありません (PnP)') }
if ($active -lt 1) { $reasons.Add('動作中の monitor がありません (WMI)') }

# --- 電源 ---
$videoIdle = @(powercfg /q SCHEME_CURRENT SUB_VIDEO VIDEOIDLE | Select-String '0x[0-9a-fA-F]+\s*$' | ForEach-Object {
        [Convert]::ToInt32(([regex]::Match($_.Line, '0x[0-9a-fA-F]+')).Value, 16) })
$displayState = $null
$leaseError = ''
try {
    $lease = Start-MvmTestDisplayLease
    try { $displayState = $lease.State } finally { $lease.Dispose() }
} catch { $leaseError = "$($_.Exception.Message)" }
$result.power = [ordered]@{ display_state = $displayState; display_state_meaning = '1 = on、2 = dimmed、0 = off'
    display_state_error = $leaseError; video_idle_timeout_seconds = $videoIdle
}
if ($displayState -ne 1) { $reasons.Add("描画先の電源状態が on ではありません: $displayState $leaseError") }

# --- 実際の描画と提示 ---
$probes = @()
if (-not $SkipRenderProbe -and $reasons.Count -eq 0) {
    $env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
    $env:MVM_TEST_FIXED_WINDOW = '1'
    $env:MVM_TEST_AUDIO_VOLUME_SCALE = '0.25'
    $fixture = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4'
    foreach ($probe in @(
            @{ name = 'fixed-test-window'; exe = 'mvm_test_fixed_test_window.exe'; args = @() },
            @{ name = 'p5c-preview-smoke'; exe = 'mvm_p5c_preview_smoke.exe'; args = @($fixture) })) {
        $exe = Join-Path $BinDirectory $probe.exe
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        $output = if (Test-Path -LiteralPath $exe) { (& $exe @($probe.args) 2>&1 | ForEach-Object { "$_" }) } else { @("実行 file がありません: $exe") }
        $code = if (Test-Path -LiteralPath $exe) { $LASTEXITCODE } else { -1 }
        $probes += [ordered]@{ name = $probe.name; exit_code = $code; seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1)
            sha256 = if (Test-Path -LiteralPath $exe) { (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash } else { $null }
            output_tail = @($output | Select-Object -Last 6)
        }
        if ($code -ne 0) { $reasons.Add("$($probe.name) が window を描画・提示できません (終了コード $code)") }
    }
} elseif (-not $SkipRenderProbe) {
    $reasons.Add('前の条件が満たされていないので、描画の probe は実行していません')
}
$result.render_probes = $probes

$result.verdict = if ($reasons.Count -eq 0) { 'VALID' } else { 'ENVIRONMENT_INVALID' }
New-Item -ItemType Directory -Force (Split-Path -Parent $OutputFile) | Out-Null
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputFile -Encoding utf8
Write-Host "GUI の環境: $($result.verdict)"
foreach ($reason in $reasons) { Write-Host "  $reason" }
Write-Host ("  monitor: PnP {0}、WMI {1}、screen {2}、DPI {3}、電源 {4}、無操作 {5} 秒" -f $present, $active, $screens.Count,
    $result.monitors.system_dpi, $displayState, $result.session.idle_seconds)
if ($reasons.Count -gt 0) { exit 2 }
