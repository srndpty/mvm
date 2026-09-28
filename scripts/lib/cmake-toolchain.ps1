function Set-MvmUcrt64Environment {
    param([Parameter(Mandatory)][string]$Ucrt64)

    $root = $Ucrt64.Replace('\', '/')
    $env:PATH = "$Ucrt64\bin;$env:PATH"
    $env:PKG_CONFIG_PATH = "$root/lib/pkgconfig;$root/share/pkgconfig"
}

function Get-MvmCMakeToolchainArguments {
    param([Parameter(Mandatory)][string]$Ucrt64)

    $root = $Ucrt64.Replace('\', '/')
    return @(
        "-DMVM_UCRT64_ROOT:PATH=$root"
        "-DCMAKE_C_COMPILER:FILEPATH=$root/bin/gcc.exe"
        "-DCMAKE_CXX_COMPILER:FILEPATH=$root/bin/g++.exe"
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$root/bin/ninja.exe"
        "-DCMAKE_PREFIX_PATH:PATH=$root"
        "-DQt6_DIR:PATH=$root/lib/cmake/Qt6"
        "-DPKG_CONFIG_EXECUTABLE:FILEPATH=$root/bin/pkgconf.exe"
    )
}

function Get-MvmConfigureSignature {
    param(
        [Parameter(Mandatory)][string]$Preset,
        [Parameter(Mandatory)][string]$Ucrt64,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string[]]$ToolchainArguments
    )

    $parts = @(
        "preset=$Preset"
        "ucrt64=$($Ucrt64.Replace('\', '/').TrimEnd('/').ToLowerInvariant())"
        "repo=$($RepoRoot.Replace('\', '/').TrimEnd('/').ToLowerInvariant())"
        "toolchain=$($ToolchainArguments -join '|')"
    )
    foreach ($name in @('CMakePresets.json', 'CMakeUserPresets.json')) {
        $path = Join-Path $RepoRoot $name
        $hash = if (Test-Path -LiteralPath $path -PathType Leaf) {
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        } else {
            'MISSING'
        }
        $parts += "$name=$hash"
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($parts -join "`n")
    return [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes))
}

function Assert-MvmCachedToolchain {
    param(
        [Parameter(Mandatory)][string]$CachePath,
        [Parameter(Mandatory)][string]$PresetsPath,
        [Parameter(Mandatory)][string]$Preset,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string[]]$ToolchainArguments
    )

    $presets = (Get-Content -LiteralPath $PresetsPath -Raw | ConvertFrom-Json).configurePresets
    $chain = @()
    $name = $Preset
    while ($name) {
        if ($name -in @($chain | ForEach-Object name)) {
            throw "CMake preset の継承が循環しています: $name"
        }
        $found = @($presets | Where-Object name -EQ $name)
        if ($found.Count -ne 1) { throw "CMake preset を一意に解決できません: $name" }
        $chain = @($found[0]) + $chain
        $parents = @()
        if ($found[0].PSObject.Properties['inherits']) { $parents = @($found[0].inherits) }
        if ($parents.Count -gt 1) { throw "複数継承の CMake preset は cache 再利用に対応していません: $name" }
        $name = if ($parents.Count -eq 1) { [string]$parents[0] } else { '' }
    }

    $expected = @{}
    $generator = ''
    foreach ($item in $chain) {
        if ($item.PSObject.Properties['generator']) { $generator = [string]$item.generator }
        foreach ($property in @($item.cacheVariables.PSObject.Properties)) {
            $value = $property.Value
            if ($value -is [pscustomobject]) { $value = $value.value }
            $expected[$property.Name] = [string]$value
        }
    }
    if (-not $generator -or -not $expected.ContainsKey('CMAKE_BUILD_TYPE')) {
        throw "CMake preset の generator または build type がありません: $Preset"
    }
    foreach ($arg in $ToolchainArguments) {
        if ($arg -notmatch '^-D([^:=]+)(?::[^=]+)?=(.*)$') {
            throw "toolchain 引数を検査できません: $arg"
        }
        $expected[$Matches[1]] = $Matches[2]
    }
    $expected['CMAKE_GENERATOR'] = $generator
    $expected['CMAKE_HOME_DIRECTORY'] = $RepoRoot
    $expected['BUILD_TESTING'] = 'ON'
    $expected['MVM_ENABLE_COVERAGE'] = 'OFF'
    $expected['MVM_ENABLE_QT'] = 'ON'

    $actual = @{}
    foreach ($line in Get-Content -LiteralPath $CachePath) {
        if ($line -match '^([^/#:][^:]*):[^=]+=(.*)$') {
            $actual[$Matches[1]] = $Matches[2]
        }
    }
    foreach ($key in $expected.Keys) {
        $wanted = ([string]$expected[$key]).Replace('\', '/').ToLowerInvariant()
        $found = if ($actual.ContainsKey($key)) {
            ([string]$actual[$key]).Replace('\', '/').ToLowerInvariant()
        } else {
            ''
        }
        if ($found -ne $wanted) {
            throw "既存 CMake cache の設定が preset と異なります: $key。通常の build を実行してください。"
        }
    }
}
