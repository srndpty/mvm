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

function Assert-MvmCachedToolchain {
    param(
        [Parameter(Mandatory)][string]$CachePath,
        [Parameter(Mandatory)][string]$Ucrt64,
        [Parameter(Mandatory)][string]$RepoRoot
    )

    $entries = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($line in Get-Content -LiteralPath $CachePath) {
        [void]$entries.Add($line.Replace('\', '/'))
    }
    $root = $Ucrt64.Replace('\', '/').TrimEnd('/')
    $repo = $RepoRoot.Replace('\', '/').TrimEnd('/')
    $required = @(
        "CMAKE_C_COMPILER:FILEPATH=$root/bin/gcc.exe"
        "CMAKE_CXX_COMPILER:FILEPATH=$root/bin/g++.exe"
        "Qt6_DIR:PATH=$root/lib/cmake/Qt6"
        "CMAKE_HOME_DIRECTORY:INTERNAL=$repo"
    )
    foreach ($entry in $required) {
        if (-not $entries.Contains($entry)) {
            throw "既存 CMake cache の toolchain が期待と異なります: $entry。通常の build を実行してください。"
        }
    }
}
