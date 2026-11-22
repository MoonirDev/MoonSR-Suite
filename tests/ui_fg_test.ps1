# End-to-end test of frame generation THROUGH THE UI (not the CLI): starts the test pattern, then MoonSR.exe with the
# automation options (--test-source, --test-fg, --test-autostart, --test-seconds, --test-status-file). MoonSR selects the
# test pattern, starts scaling with frame generation at once, writes its status values once per second, stops after
# -RunSeconds and exits. The script prints the last status and the engine's "Stats:" log lines and fails (exit code 1) if
# no generated frames were reported. The test options never change the persisted settings.
#
#   powershell -ExecutionPolicy Bypass -File tests\ui_fg_test.ps1 [-SourceFps 60] [-Multiplier 2] [-BuildDir build\main] [-RunSeconds 12]
param([int]$SourceFps = 60, [int]$Multiplier = 2, [string]$BuildDir = "build\main", [int]$RunSeconds = 12)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "$BuildDir\bin"
$dir = Join-Path $env:LOCALAPPDATA "MoonSR"
New-Item -ItemType Directory -Force $dir | Out-Null
$status = Join-Path ([System.IO.Path]::GetTempPath()) ("moonsr_ui_status_{0}.txt" -f $PID)
if (Test-Path $status) { Remove-Item $status -Force }
$logPath = Join-Path $dir "moonsr.log"

$tp = Start-Process -FilePath (Join-Path $bin "moonsr_testpattern.exe") -ArgumentList "--fps $SourceFps --seconds $($RunSeconds + 20)" -PassThru
Start-Sleep -Seconds 2
$exit = 1
try {
    $argLine = "--test-source `"MoonSR Test Pattern`" --test-fg $Multiplier --test-autostart --test-seconds $RunSeconds --test-status-file `"$status`""
    "MoonSR.exe $argLine"
    $app = Start-Process -FilePath (Join-Path $bin "MoonSR.exe") -ArgumentList $argLine -PassThru
    if (-not $app.WaitForExit(($RunSeconds + 20) * 1000)) {
        "TIMEOUT: MoonSR did not exit by itself; killing it"
        $app.Kill()
    } else {
        "MoonSR exit code: $($app.ExitCode)"
    }

    "--- last status file ---"
    if (-not (Test-Path $status)) { throw "No status file was written" }
    $text = Get-Content $status
    $text
    $kv = @{}
    foreach ($l in $text) { $i = $l.IndexOf('='); if ($i -gt 0) { $kv[$l.Substring(0, $i)] = $l.Substring($i + 1) } }
    $gen = 0.0
    [void][double]::TryParse($kv["generatedFps"], [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$gen)
    if ($gen -gt 0) { "RESULT: PASS (generatedFps = $gen)"; $exit = 0 }
    else { "RESULT: FAIL (generatedFps = $gen; state = $($kv['state']); message = $($kv['message']))" }
}
finally {
    if ($app -and -not $app.HasExited) { $app.Kill() }
    if ($tp -and -not $tp.HasExited) { [void]$tp.CloseMainWindow(); Start-Sleep -Milliseconds 500; if (-not $tp.HasExited) { $tp.Kill() } }
}
"--- log (last Stats lines; the log is restarted by each MoonSR run) ---"
if (Test-Path $logPath) {
    Get-Content $logPath | Where-Object { $_ -match 'Stats:' } | Select-Object -Last 6
}
exit $exit
