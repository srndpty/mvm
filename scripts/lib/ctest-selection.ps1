function Get-MvmDailyTestArguments {
    return @('-Preset', 'ucrt64-release', '-Group', 'BuildDependent', '-Portable', '-Fast')
}

function Get-MvmFullTestArguments {
    return @('-Preset', 'both', '-Group', 'All')
}

function Get-MvmNormalExcludePattern {
    param([switch]$Portable, [switch]$Fast)

    $labels = @('performance', 'stability')
    if ($Portable) { $labels += 'workstation' }
    if ($Fast) { $labels += 'extended' }
    return $labels -join '|'
}

function Assert-MvmRequiredCTestCount {
    param([int]$Total, [switch]$Required)

    if ($Total -ne 0) { return }
    if ($Required) {
        throw '対象テストが 0 件です。この種別は 1 件以上あるはずなので失敗にします。'
    }
    throw '対象テストが 0 件です。ラベル指定を確認してください。'
}

# CTest JSON の command と DEPENDS だけで、ビルド種別非依存を保守的に判定する。
function Get-MvmBuildIndependentTestNames {
    param(
        [Parameter(Mandatory)][array]$Tests,
        [Parameter(Mandatory)][string]$BuildDir
    )

    $buildPrefix = ($BuildDir -replace '\\', '/').ToLowerInvariant().TrimEnd('/') + '/'
    $testOutputPrefix = $buildPrefix + 'tests/'

    $dependsRelated = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($t in $Tests) {
        if (-not $t.PSObject.Properties['properties']) { continue }
        foreach ($prop in @($t.properties)) {
            if ($prop.name -ne 'DEPENDS') { continue }
            [void]$dependsRelated.Add($t.name)
            foreach ($d in @($prop.value)) { [void]$dependsRelated.Add("$d") }
        }
    }

    $names = @()
    foreach ($t in $Tests) {
        if (-not $t.PSObject.Properties['command']) { continue }
        $command = @($t.command)
        if ($command.Count -eq 0) { continue }
        if ([IO.Path]::GetFileNameWithoutExtension("$($command[0])") -ne 'pwsh') { continue }
        if ($dependsRelated.Contains($t.name)) { continue }
        $usesBuildOutput = $false
        foreach ($arg in $command) {
            $normalized = ("$arg" -replace '\\', '/').ToLowerInvariant()
            $at = $normalized.IndexOf($buildPrefix)
            while ($at -ge 0) {
                if ($normalized.IndexOf($testOutputPrefix, $at) -ne $at) { $usesBuildOutput = $true; break }
                $at = $normalized.IndexOf($buildPrefix, $at + 1)
            }
            if ($usesBuildOutput) { break }
        }
        if (-not $usesBuildOutput) { $names += $t.name }
    }
    return $names
}
