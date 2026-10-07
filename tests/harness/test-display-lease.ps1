$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Add-Type -Path @(
    (Join-Path $repoRoot 'scripts/lib/test-display-lease.cs'),
    (Join-Path $PSScriptRoot 'test_display_lease_fake.cs'))
foreach ($modeName in @('CreateFailure', 'SetFailure', 'RegisterFailure', 'MissingRegistration', 'InitiallyOff')) {
    $fakeApi = [TestDisplayPowerApi]::new()
    $fakeApi.Mode = $modeName
    $rejected = $false
    try { $unexpected = [Mvm.Tests.DisplayPowerLease]::new($fakeApi); $unexpected.Dispose() }
    catch { $rejected = $_.Exception.ToString().Contains('PROTOCOL_INVALID') }
    if (-not $rejected) { throw "電源前提の失敗を拒否しません: $modeName" }
    if ($modeName -ne 'CreateFailure' -and $fakeApi.Closes -ne 1) {
        throw "失敗時に電源要求を解放しません: $modeName"
    }
}
$fakeApi = [TestDisplayPowerApi]::new()
$lease = [Mvm.Tests.DisplayPowerLease]::new($fakeApi)
$lease.AssertValid()
$fakeApi.Changed.Invoke(0)
$fakeApi.Changed.Invoke(1)
$rejected = $false
try { $lease.AssertValid() } catch { $rejected = $_.Exception.ToString().Contains('PROTOCOL_INVALID') }
if (-not $rejected) { throw '途中で消灯した run を復帰後に有効と扱いました' }
$lease.Dispose()
$lease.Dispose()
if ($fakeApi.Sets -ne 2 -or $fakeApi.Clears -ne 2 -or $fakeApi.Closes -ne 1 -or $fakeApi.Unregisters -ne 1) {
    throw '電源要求と通知の取得・解放が一致しません'
}
Write-Host '電源前提の拒否・途中消灯の保持・解放を検証しました'
foreach ($modeName in @('ClearFailure', 'UnregisterFailure')) {
    $fakeApi = [TestDisplayPowerApi]::new()
    $fakeApi.Mode = $modeName
    $lease = [Mvm.Tests.DisplayPowerLease]::new($fakeApi)
    $rejected = $false
    try { $lease.Dispose() } catch { $rejected = $_.Exception.ToString().Contains('PROTOCOL_INVALID') }
    if (-not $rejected -or $fakeApi.Closes -ne 1 -or $fakeApi.Clears -ne 2) {
        throw "解放の失敗を隠す、または残る要求を解放しません: $modeName"
    }
}
Write-Host '電源前提の解放失敗も検証しました'
