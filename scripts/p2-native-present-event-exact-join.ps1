Set-StrictMode -Version Latest

function Invoke-MvmNativePresentEventExactJoin {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$App,
        [Parameter(Mandatory=$true)]$Etw
    )

    function Fail-Join([string]$Message){throw $Message}
    function Need-Join($Object,[string]$Name){
        if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){
            Fail-Join "exact join必須fieldがありません: $Name"
        }
        return $Object.$Name
    }
    function Parse-Swapchain($Value){
        $text=[string]$Value
        if($text.StartsWith('0x')){return [Convert]::ToUInt64($text.Substring(2),16)}
        return [uint64]$text
    }

    $opportunity=Need-Join $App 'presentation_opportunity'
    $hook=Need-Join $App 'native_present_hook'
    $measurementStart=[int64](Need-Join $opportunity 'measurement_start_qpc')
    $measurementEnd=[int64](Need-Join $opportunity 'measurement_end_qpc_exclusive')
    if($measurementStart-le0-or$measurementEnd-le$measurementStart){Fail-Join 'Layer 2 measurement windowが不正です'}

    $nativeAll=@(Need-Join $hook 'records')
    if($nativeAll.Count-eq0){Fail-Join 'native Present recordが空です'}
    $nativeSwapchains=@($nativeAll|ForEach-Object{[uint64](Need-Join $_ 'swapchain_identity')}|Sort-Object -Unique)
    if($nativeSwapchains.Count-ne1){Fail-Join "native swapchain identityが一意ではありません: $($nativeSwapchains.Count)"}
    $nativeSwapchain=[uint64]$nativeSwapchains[0]
    $native=@($nativeAll|Where-Object{
        [int64](Need-Join $_ 'present_enter_qpc')-ge$measurementStart-and
        [int64](Need-Join $_ 'present_return_qpc')-lt$measurementEnd
    })
    $boundaryNative=@($nativeAll|Where-Object{
        [int64](Need-Join $_ 'present_enter_qpc')-lt$measurementStart-or
        [int64](Need-Join $_ 'present_return_qpc')-ge$measurementEnd
    })
    if($native.Count-eq0){Fail-Join 'Layer 2 cohortにsuccessful native Presentがありません'}

    $targetPid=[int64](Need-Join $Etw 'target_process_id')
    if($App.PSObject.Properties.Name-contains'process_id'-and[int64]$App.process_id-ne$targetPid){
        Fail-Join 'app/PresentMon target PIDが一致しません'
    }
    $allTargetEvents=@(Need-Join $Etw 'events'|Where-Object{
        [int64](Need-Join $_ 'process_id')-eq$targetPid-and
        (Parse-Swapchain (Need-Join $_ 'swap_chain_address'))-eq$nativeSwapchain
    }|Sort-Object{[int64]$_.present_start_qpc})
    $windowEvents=@($allTargetEvents|Where-Object{
        [int64](Need-Join $_ 'present_start_qpc')-ge$measurementStart-and
        [int64](Need-Join $_ 'present_start_qpc')-lt$measurementEnd
    })
    $boundaryEvents=@($windowEvents|Where-Object{
        $qpc=[int64]$_.present_start_qpc
        @($boundaryNative|Where-Object{
            $qpc-ge[int64]$_.present_enter_qpc-and$qpc-le[int64]$_.present_return_qpc
        }).Count-gt0
    })
    $nonBoundaryEvents=@($windowEvents|Where-Object{
        $qpc=[int64]$_.present_start_qpc
        @($boundaryNative|Where-Object{
            $qpc-ge[int64]$_.present_enter_qpc-and$qpc-le[int64]$_.present_return_qpc
        }).Count-eq0
    })
    # Layer 2 cohortはnative submission側で決める。measurement window内でも、どの
    # cohort native intervalにも属さないteardown等のPresentEventは診断対象に留める。
    $events=@($nonBoundaryEvents|Where-Object{
        $qpc=[int64]$_.present_start_qpc
        @($native|Where-Object{
            $qpc-ge[int64]$_.present_enter_qpc-and$qpc-le[int64]$_.present_return_qpc
        }).Count-gt0
    })
    $outsideNativeCohortCount=$nonBoundaryEvents.Count-$events.Count
    if($events.Count-ne$native.Count){
        Fail-Join "successful native Present/target PresentEvent件数が一致しません (native=$($native.Count) event=$($events.Count))"
    }

    $sequenceBase=[int64](Need-Join $events[0] 'sequence_index')
    for($index=0;$index-lt$events.Count;++$index){
        if([int64](Need-Join $events[$index] 'sequence_index')-ne$sequenceBase+$index){
            Fail-Join "PresentEvent sequenceが連続していません: $index"
        }
        if($index-gt0-and[int64]$events[$index].present_start_qpc-le[int64]$events[$index-1].present_start_qpc){
            Fail-Join "PresentEvent PresentStart順序がstrictではありません: $index"
        }
    }

    $joined=@();$consumed=@{}
    for($index=0;$index-lt$native.Count;++$index){
        $record=$native[$index]
        $enter=[int64](Need-Join $record 'present_enter_qpc')
        $returned=[int64](Need-Join $record 'present_return_qpc')
        if($enter-le0-or$returned-lt$enter){Fail-Join "native Present intervalが不正です: $index"}
        $candidates=@($events|Where-Object{
            $start=[int64](Need-Join $_ 'present_start_qpc')
            $start-ge$enter-and$start-le$returned-and
            [int64](Need-Join $_ 'thread_id')-eq[int64](Need-Join $record 'thread_id')-and
            [int64](Need-Join $_ 'sync_interval')-eq[int64](Need-Join $record 'sync_interval')-and
            [int64](Need-Join $_ 'present_flags')-eq[int64](Need-Join $record 'present_flags')
        })
        if($candidates.Count-ne1){
            Fail-Join "native Presentに対するexact PresentEvent候補が一意ではありません: index=$index candidates=$($candidates.Count)"
        }
        $presentEvent=$candidates[0]
        $eventKey="{0}|{1}"-f[uint64](Need-Join $presentEvent 'sequence_index'),[int64](Need-Join $presentEvent 'present_start_qpc')
        if($consumed.ContainsKey($eventKey)){Fail-Join "PresentEventが複数native Presentへ結合されました: $eventKey"}
        $consumed[$eventKey]=$true
        if($presentEvent-ne$events[$index]){Fail-Join "native/PresentEventのstrict order joinが一致しません: $index"}
        $joined+=[pscustomobject]@{native=$record;present_event=$presentEvent}
    }
    if($consumed.Count-ne$events.Count){Fail-Join '未消費のtarget PresentEventがあります'}

    $boundaryJoinDiagnostics=@();$boundaryJoined=@()
    foreach($record in $boundaryNative){
        $enter=[int64](Need-Join $record 'present_enter_qpc');$returned=[int64](Need-Join $record 'present_return_qpc')
        $candidates=@($allTargetEvents|Where-Object{
            $start=[int64](Need-Join $_ 'present_start_qpc')
            $start-ge$enter-and$start-le$returned-and
            [int64](Need-Join $_ 'thread_id')-eq[int64](Need-Join $record 'thread_id')-and
            [int64](Need-Join $_ 'sync_interval')-eq[int64](Need-Join $record 'sync_interval')-and
            [int64](Need-Join $_ 'present_flags')-eq[int64](Need-Join $record 'present_flags')
        })
        $diagnostic=[pscustomobject]@{native=$record;candidate_count=$candidates.Count;present_event=$(if($candidates.Count-eq1){$candidates[0]}else{$null})}
        $boundaryJoinDiagnostics+=$diagnostic
        if($candidates.Count-eq1){$boundaryJoined+=$diagnostic}
    }

    return [pscustomobject]@{
        cohort_inclusion_authority='present_enter_qpc >= measurement_start_qpc && present_return_qpc < measurement_end_qpc_exclusive'
        target_process_id=$targetPid
        target_swapchain_identity=[string]$nativeSwapchain
        measurement_start_qpc=$measurementStart
        measurement_end_qpc_exclusive=$measurementEnd
        native=$native
        events=$events
        joined=$joined
        all_target_events=$allTargetEvents
        measurement_window_target_events=$windowEvents
        outside_cohort_events=@($nonBoundaryEvents|Where-Object{
            $qpc=[int64]$_.present_start_qpc
            @($native|Where-Object{
                $qpc-ge[int64]$_.present_enter_qpc-and$qpc-le[int64]$_.present_return_qpc
            }).Count-eq0
        })
        boundary_native=$boundaryNative
        boundary_events=$boundaryEvents
        boundary_joined=$boundaryJoined
        boundary_join_diagnostics=$boundaryJoinDiagnostics
        boundary_native_count=$boundaryNative.Count
        boundary_event_count=$boundaryEvents.Count
        outside_native_cohort_event_count=$outsideNativeCohortCount
    }
}
