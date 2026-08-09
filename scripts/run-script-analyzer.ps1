[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$AnalysisPath,

    [Parameter(Mandatory)]
    [string]$SettingsPath,

    [Parameter(Mandatory)]
    [version]$RequiredVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    Import-Module PSScriptAnalyzer -RequiredVersion $RequiredVersion
    $issues = @(Invoke-ScriptAnalyzer -Path $AnalysisPath -Settings $SettingsPath `
        -Severity Error, Warning)
} catch {
    Write-Error "PSScriptAnalyzer $RequiredVersion の実行に失敗しました: $($_.Exception.Message)"
    exit 2
}

if ($issues.Count -ne 0) {
    $issues | ForEach-Object {
        Write-Output ("  {0}:{1} [{2}] {3}" -f (Split-Path $_.ScriptPath -Leaf), $_.Line,
                                                $_.RuleName, $_.Message)
    }
    exit 1
}

Write-Output "OK (PSScriptAnalyzer $RequiredVersion)"
exit 0
