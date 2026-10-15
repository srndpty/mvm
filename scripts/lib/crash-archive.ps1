# crash の証拠 (dump・試験の log) を、repo と build/ の外に保全する archive。
#
# build/ は容量のために丸ごと消すことがある (2026-10-15 に実施)。crash の証拠はそれと一緒に消えては
# いけないので、既定では %LOCALAPPDATA%\mvm\crash-archive に置く (MVM_CRASH_ARCHIVE で変更できる)。
# repo の中は拒否する。
#
# 規則 (docs/crash-evidence.md):
#   - dump は内容の SHA-256 を名前にして objects/ に置く。同じ名前の別の dump も上書きしない。
#   - 元の path・名前・時刻・SHA-256 を index.jsonl に追記する。index は追記だけで書き換えない。
#   - 容量: archive の合計が config.json の max_bytes を超える、または volume の空きが min_free_bytes を
#     下回る複製は、1 件も始めずに失敗する。
#   - 自動では何も消さない。消すのは Remove-MvmCrashArchiveObject に SHA-256・理由・-Apply を明示した
#     ときだけで、その記録 (tombstone) を index に残す。
#   - 保全に失敗したら例外にする。呼び出し側は、crash しうる probe を起動しない。
#
# 使い方: . scripts/lib/crash-archive.ps1
#         $a = Open-MvmCrashArchive
#         try { Save-MvmCrashDumps -Archive $a -Directory "$env:LOCALAPPDATA\CrashDumps" -Context '...' } finally { Close-MvmCrashArchive $a }

$script:MvmCrashArchiveRepo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:MvmCrashArchiveDefaults = [ordered]@{ max_bytes = 64GB; min_free_bytes = 32GB }

function Get-MvmCrashArchiveRoot {
    param([string]$Root)
    if (-not $Root) { $Root = $env:MVM_CRASH_ARCHIVE }
    if (-not $Root) { $Root = Join-Path $env:LOCALAPPDATA 'mvm\crash-archive' }
    $full = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $repo = $script:MvmCrashArchiveRepo.TrimEnd('\')
    if ($full -eq $repo -or $full.StartsWith("$repo\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "crash の archive を repo の中に置けません (build/ と一緒に消えるため): $full"
    }
    return $full
}

function Open-MvmCrashArchive {
    param([string]$Root, [int]$LockTimeoutSeconds = 30)
    $root = Get-MvmCrashArchiveRoot $Root
    if ((Test-Path -LiteralPath $root) -and -not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "crash の archive の場所が directory ではありません: $root"
    }
    foreach ($d in @('', 'objects', 'runs', 'incoming')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $root $d) -ErrorAction Stop | Out-Null
    }
    # 書き手を 1 つにする (index の追記と容量の判定を排他にする)。
    $lockPath = Join-Path $root '.lock'
    $deadline = [DateTime]::UtcNow.AddSeconds($LockTimeoutSeconds)
    $lock = $null
    while (-not $lock) {
        try {
            $lock = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
        } catch [IO.IOException] {
            if ([DateTime]::UtcNow -gt $deadline) { throw "crash の archive の lock を取れません: $lockPath" }
            Start-Sleep -Milliseconds 200
        }
    }
    try {
        $configPath = Join-Path $root 'config.json'
        if (-not (Test-Path -LiteralPath $configPath)) {
            $script:MvmCrashArchiveDefaults | ConvertTo-Json | Set-Content -LiteralPath $configPath -Encoding utf8NoBOM
        }
        $config = Get-Content -Raw -LiteralPath $configPath | ConvertFrom-Json
        foreach ($k in 'max_bytes', 'min_free_bytes') {
            if (-not ($config.PSObject.Properties.Name -contains $k) -or [long]$config.$k -lt 0) {
                throw "config.json の $k が無いか不正です: $configPath"
            }
        }
        $indexPath = Join-Path $root 'index.jsonl'
        $known = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        if (Test-Path -LiteralPath $indexPath) {
            foreach ($line in [IO.File]::ReadLines($indexPath)) {
                if (-not $line) { continue }
                $r = $line | ConvertFrom-Json
                if ($r.kind -eq 'dump') { [void]$known.Add("$($r.original_path)|$($r.sha256)") }
            }
        }
        return [pscustomobject]@{
            Root = $root; Objects = (Join-Path $root 'objects'); Runs = (Join-Path $root 'runs')
            Incoming = (Join-Path $root 'incoming'); Index = $indexPath; Lock = $lock
            MaxBytes = [long]$config.max_bytes; MinFreeBytes = [long]$config.min_free_bytes; Known = $known
        }
    } catch {
        $lock.Dispose()
        throw
    }
}

function Close-MvmCrashArchive {
    param([Parameter(Mandatory)]$Archive)
    if ($Archive.Lock) { $Archive.Lock.Dispose(); $Archive.Lock = $null }
}

function Add-MvmCrashArchiveIndex {
    param([Parameter(Mandatory)]$Archive, [Parameter(Mandatory)][hashtable]$Record)
    $Record['recorded_at'] = (Get-Date).ToString('o')
    $line = ([pscustomobject]$Record | ConvertTo-Json -Compress -Depth 6) + "`n"
    [IO.File]::AppendAllText($Archive.Index, $line, [Text.UTF8Encoding]::new($false))
}

function Get-MvmCrashArchiveUsage {
    param([Parameter(Mandatory)]$Archive)
    $files = @(Get-ChildItem -LiteralPath $Archive.Root -Recurse -File -Force -ErrorAction Stop)
    $bytes = [long](($files | Measure-Object -Property Length -Sum).Sum)
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Archive.Root))
    [pscustomobject]@{
        root = $Archive.Root; bytes = $bytes
        objects = @(Get-ChildItem -LiteralPath $Archive.Objects -File).Count
        runs = @(Get-ChildItem -LiteralPath $Archive.Runs -Directory).Count
        max_bytes = $Archive.MaxBytes; free_bytes = $drive.AvailableFreeSpace; min_free_bytes = $Archive.MinFreeBytes
    }
}

# 新しく書く byte 数が上限に収まるかを確かめる。収まらなければ何も書かずに失敗する。
function Assert-MvmCrashArchiveCapacity {
    param([Parameter(Mandatory)]$Archive, [Parameter(Mandatory)][long]$NewBytes)
    $u = Get-MvmCrashArchiveUsage $Archive
    if ($u.bytes + $NewBytes -gt $Archive.MaxBytes) {
        throw ("crash の archive の上限を超えます: 使用 {0:N0} + 追加 {1:N0} > 上限 {2:N0} byte ({3}\config.json)" -f
            $u.bytes, $NewBytes, $Archive.MaxBytes, $Archive.Root)
    }
    if ($u.free_bytes - $NewBytes -lt $Archive.MinFreeBytes) {
        throw ("volume の空きが足りません: 空き {0:N0} - 追加 {1:N0} < 下限 {2:N0} byte" -f
            $u.free_bytes, $NewBytes, $Archive.MinFreeBytes)
    }
}

function Get-MvmSha256 {
    param([Parameter(Mandatory)][string]$Path)
    # WER が書いている途中の dump は共有読み取りでも開けない。開けなければ例外にする (保全の失敗)。
    $s = [IO.File]::Open($Path, 'Open', 'Read', 'Read')
    try { return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($s)) } finally { $s.Dispose() }
}

# Directory の *.dmp をすべて保全する。保全済み (同じ元 path と内容) は飛ばす。
# 戻り値: 各 dump の記録 (status = archived | already_object | already_indexed)。
function Save-MvmCrashDumps {
    param(
        [Parameter(Mandatory)]$Archive,
        [Parameter(Mandatory)][string[]]$Directory,
        [string]$Context = ''
    )
    $candidates = @()
    foreach ($dir in $Directory) {
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
        foreach ($f in @(Get-ChildItem -LiteralPath $dir -Filter *.dmp -File)) {
            $sha = Get-MvmSha256 $f.FullName
            $candidates += [pscustomobject]@{ File = $f; Sha = $sha }
        }
    }
    $newBytes = [long]0
    foreach ($c in $candidates) {
        if (-not (Test-Path -LiteralPath (Join-Path $Archive.Objects "$($c.Sha).dmp"))) { $newBytes += $c.File.Length }
    }
    Assert-MvmCrashArchiveCapacity -Archive $Archive -NewBytes $newBytes

    $results = @()
    foreach ($c in $candidates) {
        $f = $c.File
        $object = Join-Path $Archive.Objects "$($c.Sha).dmp"
        $key = "$($f.FullName)|$($c.Sha)"
        $status = 'already_indexed'
        if (-not (Test-Path -LiteralPath $object)) {
            $tmp = Join-Path $Archive.Incoming ([guid]::NewGuid().ToString() + '.tmp')
            [IO.File]::Copy($f.FullName, $tmp, $false)
            $copied = Get-MvmSha256 $tmp
            if ($copied -ne $c.Sha) {
                Remove-Item -LiteralPath $tmp
                throw "複製の内容が元と一致しません (書き込み中の可能性): $($f.FullName)"
            }
            [IO.File]::Move($tmp, $object, $false)
            $status = 'archived'
        } elseif (-not $Archive.Known.Contains($key)) {
            # 同じ内容が別の名前・場所で既に保全されている。index だけ足す。
            if ((Get-Item -LiteralPath $object).Length -ne $f.Length) { throw "object の大きさが一致しません: $object" }
            $status = 'already_object'
        }
        if ($status -ne 'already_indexed') {
            Add-MvmCrashArchiveIndex -Archive $Archive -Record @{
                kind = 'dump'; sha256 = $c.Sha; size = $f.Length; original_path = $f.FullName
                original_name = $f.Name; original_last_write = $f.LastWriteTime.ToString('o')
                object = "objects/$($c.Sha).dmp"; context = $Context
            }
            [void]$Archive.Known.Add($key)
        }
        $results += [pscustomobject]@{ name = $f.Name; original_path = $f.FullName; sha256 = $c.Sha; size = $f.Length; status = $status }
    }
    return , $results
}

# 既存の dump を保全してから Action を実行する。保全に失敗したら Action を実行せずに例外にする。
# crash しうる probe・試験は、これを通して起動する (WER の回転で未保全の dump を消さないため)。
function Invoke-MvmAfterCrashArchive {
    param(
        [Parameter(Mandatory)]$Archive,
        [Parameter(Mandatory)][string[]]$Directory,
        [Parameter(Mandatory)][string]$Context,
        [Parameter(Mandatory)][scriptblock]$Action
    )
    $saved = Save-MvmCrashDumps -Archive $Archive -Directory $Directory -Context $Context
    & $Action $saved
}

# run ごとの directory を作る。既にあれば失敗する (上書きしない)。
function New-MvmCrashArchiveRun {
    param([Parameter(Mandatory)]$Archive, [Parameter(Mandatory)][string]$Name)
    if ($Name -notmatch '^[A-Za-z0-9._-]+$') { throw "run の名前に使えない文字があります: $Name" }
    $path = Join-Path $Archive.Runs ("{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Name)
    if (Test-Path -LiteralPath $path) { throw "run の directory が既にあります: $path" }
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    Add-MvmCrashArchiveIndex -Archive $Archive -Record @{ kind = 'run'; path = "runs/$(Split-Path $path -Leaf)" }
    return $path
}

# 小さな記録 (log・JSON) を run の directory へ複製する。同名があれば失敗する。
function Copy-MvmCrashArchiveFile {
    param([Parameter(Mandatory)]$Archive, [Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    $full = [IO.Path]::GetFullPath($Destination)
    if (-not $full.StartsWith($Archive.Runs + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "複製先が archive の runs/ の外です: $full"
    }
    Assert-MvmCrashArchiveCapacity -Archive $Archive -NewBytes (Get-Item -LiteralPath $Source).Length
    [IO.File]::Copy($Source, $full, $false)
}

# 明示した object だけを消す。-Apply が無ければ何もせず、対象を返すだけ。
function Remove-MvmCrashArchiveObject {
    param(
        [Parameter(Mandatory)]$Archive,
        [Parameter(Mandatory)][string[]]$Sha256,
        [Parameter(Mandatory)][string]$Reason,
        [switch]$Apply
    )
    if ($Reason.Trim().Length -lt 8) { throw '消す理由を 8 文字以上で書いてください' }
    $targets = foreach ($s in $Sha256) {
        if ($s -notmatch '^[0-9A-Fa-f]{64}$') { throw "SHA-256 ではありません: $s" }
        $path = Join-Path $Archive.Objects "$($s.ToUpperInvariant()).dmp"
        if (-not (Test-Path -LiteralPath $path)) { throw "object がありません: $s" }
        [pscustomobject]@{ sha256 = $s.ToUpperInvariant(); path = $path; size = (Get-Item -LiteralPath $path).Length }
    }
    if (-not $Apply) { return , @($targets) }
    foreach ($t in $targets) {
        Remove-Item -LiteralPath $t.path -ErrorAction Stop
        Add-MvmCrashArchiveIndex -Archive $Archive -Record @{ kind = 'removed'; sha256 = $t.sha256; size = $t.size; reason = $Reason }
    }
    return , @($targets)
}
