# syncPreviewSourcesAt() の失敗時 transaction 契約を source 上で固定する。
#
# preview source の差し替えは engine と GPU device を要求するため、controller を
# その場で駆動する unit test は書けない。代わりに「壊れると部分 commit が起きる」
# 順序と rollback 到達性を、実際の並びから検査する。
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$controllerPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.cpp'
$controller = Get-Content -LiteralPath $controllerPath -Raw

function Get-FunctionBody {
    param([string]$Text, [string]$Signature)

    $start = $Text.IndexOf($Signature)
    if ($start -lt 0) { throw "関数が見つかりません: $Signature" }
    $braceIndex = $Text.IndexOf('{', $start)
    if ($braceIndex -lt 0) { throw "関数本体の開始が見つかりません: $Signature" }
    $depth = 0
    for ($index = $braceIndex; $index -lt $Text.Length; $index++) {
        $character = $Text[$index]
        if ($character -eq '{') { $depth++ }
        elseif ($character -eq '}') {
            $depth--
            if ($depth -eq 0) {
                return $Text.Substring($braceIndex, $index - $braceIndex + 1)
            }
        }
    }
    throw "関数本体の終端が見つかりません: $Signature"
}

$body = Get-FunctionBody -Text $controller `
    -Signature 'bool MvmController::syncPreviewSourcesAt(std::int64_t timelineFrame, QString& error)'

# --- 1. audio の差し替えは composition submit の後で行う -----------------------
# 先に audio を差し替えると、submit 失敗時に audio だけ新しい状態で残る。
$submitIndex = $body.IndexOf('previewEngine_->submitComposition(')
$audioIndex = $body.IndexOf('applyAudioSourceFor(')
$seekIndex = $body.IndexOf('previewEngine_->seekFrameRequest(')
if ($submitIndex -lt 0) { throw 'submitComposition の呼び出しがありません' }
if ($audioIndex -lt 0) { throw 'applyAudioSourceFor の呼び出しがありません' }
if ($seekIndex -lt 0) { throw 'seekFrameRequest の呼び出しがありません' }
if ($audioIndex -lt $submitIndex) {
    throw 'audio の差し替えが composition submit より前にあります (部分 commit になります)'
}
if ($seekIndex -lt $audioIndex) {
    throw 'seek が audio 差し替えより前にあります (契約と順序が違います)'
}

# --- 2. audio 差し替え後の失敗経路はすべて rollback を通る --------------------
# audio を触った後に rollback せず return false する経路があると、
# video だけ元に戻って audio が新しいまま残る。
$afterAudio = $body.Substring($audioIndex)
$failureReturns = [regex]::Matches($afterAudio, 'return false;')
if ($failureReturns.Count -lt 2) {
    throw 'audio 差し替え後の失敗経路が想定より少ないです。検査が空振りしています'
}
foreach ($match in $failureReturns) {
    $window = $afterAudio.Substring(0, $match.Index)
    $lastRollback = $window.LastIndexOf('rollback();')
    $lastReturn = $window.LastIndexOf('return false;')
    if ($lastRollback -lt $lastReturn) {
        throw 'audio 差し替え後に rollback を通らない失敗経路があります'
    }
}

# --- 3. rollback は video source と audio の両方を戻す ------------------------
$rollbackStart = $body.IndexOf('const auto rollback = [&] {')
if ($rollbackStart -lt 0) { throw 'rollback lambda がありません' }
$rollbackEnd = $body.IndexOf('};', $rollbackStart)
$rollbackBody = $body.Substring($rollbackStart, $rollbackEnd - $rollbackStart)
if (-not $rollbackBody.Contains('previewEngine_->removeSource(source)')) {
    throw 'rollback が新規 video source を外していません'
}
if (-not $rollbackBody.Contains('retiredSources_.push_back(source)')) {
    throw 'rollback が removeSource 拒否時に retirement queue へ回していません'
}
if (-not $rollbackBody.Contains('revertAudioSource(audioUndo')) {
    throw 'rollback が audio を元へ戻していません'
}

# --- 4. audio の差し替えは undo を記録してから行う ----------------------------
$applyBody = Get-FunctionBody -Text $controller `
    -Signature 'bool MvmController::applyAudioSourceFor(std::int64_t timelineFrame, AudioSwitchUndo& undo,'
$undoIndex = $applyBody.IndexOf('undo.previous = audioSource_;')
$removeIndex = $applyBody.IndexOf('previewEngine_->removeSource(audioSource_->source)')
$addIndex = $applyBody.IndexOf('previewEngine_->addSource(descriptor)')
if ($undoIndex -lt 0) { throw 'applyAudioSourceFor が旧 source を控えていません' }
if ($removeIndex -lt 0 -or $addIndex -lt 0) { throw 'applyAudioSourceFor の remove/add がありません' }
if ($undoIndex -gt $removeIndex) {
    throw '旧 audio source を控える前に remove しています (戻せなくなります)'
}
if ($removeIndex -gt $addIndex) {
    throw 'audio の remove より前に add しています (engine は 1 件しか受理しません)'
}

# --- 5. identity は timing に効く入力をすべて含む ------------------------------
$headerPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.h'
$header = Get-Content -LiteralPath $headerPath -Raw
$identityStart = $header.IndexOf('struct AudioSourceIdentity {')
if ($identityStart -lt 0) { throw 'AudioSourceIdentity がありません' }
$identityEnd = $header.IndexOf('};', $identityStart)
$identity = $header.Substring($identityStart, $identityEnd - $identityStart)
foreach ($field in @('clipId', 'sourceInFrame', 'timelineStartFrame',
                     'sourceFpsNum', 'sourceFpsDen', 'timelineFpsNum', 'timelineFpsDen')) {
    if (-not $identity.Contains($field)) {
        throw "AudioSourceIdentity に $field がありません (offset が変わっても再利用されます)"
    }
}

Write-Output 'preview transaction contract: PASS'
