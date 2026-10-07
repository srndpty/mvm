function Start-MvmTestDisplayLease {
    if (-not ('Mvm.Tests.DisplayPowerLease' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'test-display-lease.cs')
    }
    $lease = [Mvm.Tests.DisplayPowerLease]::new()
    Write-Host "描画試験の電源前提を取得: display=$($lease.State)、自動消灯・スリープ防止 (実行中のみ)、観測=$($lease.Observations -join ', ')"
    return $lease
}
