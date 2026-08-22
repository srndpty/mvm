param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$SourceA,
    [Parameter(Mandatory=$true)][string]$SourceB,
    [Parameter(Mandatory=$true)][string]$Metrics,
    [Parameter(Mandatory=$true)][int]$WarmupSeconds,
    [Parameter(Mandatory=$true)][int]$MeasureSeconds
)
$ErrorActionPreference='Stop'
Remove-Item Env:QSG_NO_VSYNC -ErrorAction SilentlyContinue
$env:PATH="C:\msys64\ucrt64\bin;$env:PATH"
& $Executable --source-a $SourceA --source-b $SourceB --metrics $Metrics `
    --warmup-seconds $WarmupSeconds --measure-seconds $MeasureSeconds `
    --seed 20260808 --seek-count 1000 --display-timeout-ms 2000 `
    --gpu-completion fence --mode playback --vblank-observer `
    --presentation-opportunity-ring --incremental-mapper-shadow
exit $LASTEXITCODE
