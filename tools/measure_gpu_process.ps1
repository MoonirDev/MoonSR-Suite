# Records per-process Windows GPU engine counters without launching or controlling the application.
param(
    [Parameter(Mandatory=$true)][int]$ProcessId,
    [ValidateRange(3,120)][int]$Seconds = 20,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$target = Get-Process -Id $ProcessId
if (!$target.StartTime) { throw 'Cannot read the target process start time; choose an accessible process to guard against PID reuse.' }
$started = $target.StartTime.ToUniversalTime()
$name = $target.ProcessName
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$rows = [System.Collections.Generic.List[object]]::new()
$missing = [System.Collections.Generic.List[object]]::new()
$counter = '\GPU Engine(pid_' + $ProcessId + '_*)\Utilization Percentage'
$sampledAt = [DateTime]::UtcNow
for ($sample = 0; $sample -lt $Seconds; ++$sample) {
    $live = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (!$live -or $live.StartTime.ToUniversalTime() -ne $started) {
        $missing.Add([pscustomobject]@{sample=$sample; reason='Process exited or PID was reused'})
        break
    }
    try {
        $result = Get-Counter -Counter $counter -SampleInterval 1 -MaxSamples 1 -ErrorAction Stop
        $valid = 0
        foreach ($entry in $result.CounterSamples) {
            if ($entry.Status -ne 0 -or ![double]::IsFinite($entry.CookedValue)) { continue }
            if ($entry.InstanceName -notmatch '^pid_(\d+)_luid_(0x[0-9a-f]+_0x[0-9a-f]+)_phys_(\d+)_eng_(\d+)_engtype_(.+)$') { continue }
            if ([int]$Matches[1] -ne $ProcessId) { continue }
            $rows.Add([pscustomobject]@{
                sample=$sample; utc=$result.Timestamp.ToUniversalTime().ToString('o'); process_id=$ProcessId
                adapter_luid=$Matches[2]; physical_adapter=[int]$Matches[3]; engine=[int]$Matches[4]
                engine_type=$Matches[5]; utilization_percent=[double]$entry.CookedValue
            })
            ++$valid
        }
        if (!$valid) { $missing.Add([pscustomobject]@{sample=$sample; reason='No valid GPU engine counters'}) }
    } catch {
        $missing.Add([pscustomobject]@{sample=$sample; reason=$_.Exception.Message})
    }
}
if ($rows.Count) { $rows | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $OutputDirectory 'gpu_samples.csv') }
else { 'sample,utc,process_id,adapter_luid,physical_adapter,engine,engine_type,utilization_percent' | Set-Content -LiteralPath (Join-Path $OutputDirectory 'gpu_samples.csv') }
if ($missing.Count) { $missing | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $OutputDirectory 'missing_samples.csv') }
else { 'sample,reason' | Set-Content -LiteralPath (Join-Path $OutputDirectory 'missing_samples.csv') }
$coverage = @($rows | Select-Object -ExpandProperty sample -Unique).Count
$summary = @($rows | Group-Object adapter_luid,physical_adapter,engine_type | ForEach-Object {
    $group = $_.Group
    # Sum only engines of the same type on one adapter within each observed sample.
    $values = @($group | Group-Object sample | ForEach-Object { ($_.Group | Measure-Object utilization_percent -Sum).Sum } | Sort-Object)
    [pscustomobject]@{
        adapter_luid=$group[0].adapter_luid; physical_adapter=$group[0].physical_adapter
        engine_type=$group[0].engine_type; observed_samples=$values.Count
        mean_percent=($values | Measure-Object -Average).Average
        p95_percent=$values[[math]::Ceiling(.95*$values.Count)-1]; max_percent=$values[-1]
    }
})
$report = [ordered]@{
    process_name=$name; process_id=$ProcessId; process_started_utc=$started.ToString('o')
    measurement_started_utc=$sampledAt.ToString('o'); requested_samples=$Seconds
    observed_samples=$coverage; missing_samples=$missing.Count
    adequate_counter_coverage=($coverage -ge [math]::Ceiling(.8*$Seconds))
    notes='Windows per-process GPU Engine counters, separated by adapter/type. Sums across engines may exceed 100%; this is not a normalized whole-GPU percentage. Absent counters are missing data, not zero usage. Scene motion, capture/output FPS, sync, scaling, flow settings, and background load must be matched separately.'
    engines=$summary
}
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'gpu_summary.json') -Encoding UTF8
$summary | Format-Table -AutoSize
Write-Host "Observed $coverage/$Seconds samples for $name (PID $ProcessId). Report: $OutputDirectory"
if (!$report.adequate_counter_coverage) { throw 'Insufficient valid GPU counters; do not interpret this run as a utilization measurement.' }
