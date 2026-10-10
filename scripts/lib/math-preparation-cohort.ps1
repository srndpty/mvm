function Get-MvmMathPreparationCohort {
    param([Parameter(Mandatory)][string]$Path)
    $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
    if ($manifest.schema -ne 1 -or -not $manifest.ContainsKey('tests')) {
        throw 'cohort manifest の形式が不正です。'
    }
    $names = @($manifest.tests)
    if ($names.Count -eq 0 -or @($names | Where-Object { $_ -isnot [string] -or $_ -notmatch '^\w[\w.-]*$' }).Count -ne 0) {
        throw 'cohort の試験名が空または不正です。'
    }
    if (@($names | Sort-Object -Unique).Count -ne $names.Count) { throw 'cohort の試験名が重複しています。' }
    if ('math_transform_native_playback' -notin $names) { throw 'cohort に対象の native playback 試験がありません。' }
    return $names
}

function Assert-MvmMathPreparationCohortSelection {
    param([Parameter(Mandatory)][string[]]$Expected, [Parameter(Mandatory)][string[]]$Actual)
    if (@(Compare-Object $Expected $Actual).Count -ne 0) {
        throw 'cohort manifest と実際の CTest 試験集合が一致しません。'
    }
}
