# Screenshots of the MoonSR UI through its --test-* hooks: starts MoonSR.exe on a page in a theme, grabs the window from the
# screen (DWM frame bounds, including the title bar) and closes it. The test options never change the persisted settings.
#
#   powershell -ExecutionPolicy Bypass -File tools\ui_shots.ps1 [-BuildDir build\main] [-OutDir <dir>]
#                [-Pages home,upscaling,loadorder,framegen] [-Extra "--test-fg-mode auto --test-fg-target 144"] [-Tag _auto]
#                [-Keys ...] [-Themes dark,light] [-Dpi 0] [-Size 1120,740] [-NoMica]
# The window is placed at (40,20) by the test mode, so keep other windows away from that area while it runs.
param([string]$BuildDir = "build\main", [string]$OutDir = "", [string[]]$Pages = @("home", "upscaling", "loadorder"),
      [string[]]$Themes = @("dark", "light"), [int]$Dpi = 0, [string]$Size = "1120,740", [switch]$NoMica,
      [string]$Keys = "", [string]$Extra = "", [string]$Tag = "")
$ErrorActionPreference = "Stop"
# Lists may also arrive as one comma-separated string (powershell -File passes arrays that way).
$Pages = @($Pages | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Themes = @($Themes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "$BuildDir\bin\MoonSR.exe"
if (-not $OutDir) { $OutDir = Join-Path $root "$BuildDir\ui_shots" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Drawing;
public static class ShotNative {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
[void][ShotNative]::SetProcessDPIAware()

foreach ($theme in $Themes) {
    foreach ($page in $Pages) {
        $a = "--test-page $page --test-theme $theme --test-size $Size"
        if ($Dpi -gt 0) { $a += " --test-dpi $Dpi" }
        if ($NoMica) { $a += " --test-no-mica" }
        if ($Keys) { $a += " --test-keys $Keys" }
        if ($Extra) { $a += " $Extra" }
        $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru
        try {
            $h = [IntPtr]::Zero
            for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); $h = $p.MainWindowHandle }
            if ($h -eq [IntPtr]::Zero) { throw "no window for $a" }
            [void][ShotNative]::SetForegroundWindow($h)
            Start-Sleep -Milliseconds 1800
            $r = New-Object ShotNative+RECT
            [void][ShotNative]::DwmGetWindowAttribute($h, 9, [ref]$r, 16)   # DWMWA_EXTENDED_FRAME_BOUNDS
            $w = $r.R - $r.L; $hh = $r.B - $r.T
            $bm = New-Object System.Drawing.Bitmap $w, $hh
            $g = [System.Drawing.Graphics]::FromImage($bm)
            $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $hh))
            $g.Dispose()
            $file = Join-Path $OutDir ("{0}{1}_{2}.png" -f $page, $Tag, $theme)
            $bm.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bm.Dispose()
            "$file ($w x $hh)"
        } finally { if (-not $p.HasExited) { $p.Kill() } }
    }
}
