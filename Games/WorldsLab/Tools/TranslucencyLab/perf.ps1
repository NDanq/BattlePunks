param(
    [string]$Out,                              # a CSV: config, backend, view, run, frame, span, ms (appended)
    [string]$Config,                           # its name in the CSV
    [string[]]$Extra = @(),                    # the -Section.Key=Value arguments that make the config
    [int[]]$Views = @(0, 1, 2, 3),
    [int[]]$Rhis = @(2, 4),                    # 2 Vulkan, 4 D3D12
    [int]$Runs = 2,                            # launches per view
    [int]$Frame = 480, [int]$Samples = 3,      # GPU spans logged at Frame, Frame + 120, ...
    [string]$Exe = "$PSScriptRoot\..\..\Binaries\Windows_Debug\WorldsLab.exe",
    [int]$Timeout = 150
)
# TranslucencyLab's GPU cost (Engine/Modules/RenderCore/LIT_TRANSLUCENCY_PERF.md): the
# scene's GPU time per span ("stat frame"), with the render clock fixed and no frame
# read back (a capture's readback stalls the GPU and skews the spans). Summarise with
# perf.py.
$ErrorActionPreference = "Stop"
$Logs = Join-Path (Split-Path $Exe) "Saved\Logs"
if (-not (Test-Path $Out)) { "config,backend,view,run,frame,span,ms" | Out-File -FilePath $Out -Encoding ascii }
foreach ($rhi in $Rhis) {
    $backend = if ($rhi -eq 4) { "D3D12" } elseif ($rhi -eq 1) { "OpenGL" } else { "Vulkan" }
    foreach ($view in $Views) {
        for ($run = 1; $run -le $Runs; $run++) {
            $before = Get-Date
            $gameArgs = @("-Game.StartLevel=TranslucencyLab", "-WorldsLab.TranslucencyLabView=$view", "-Display.RHI=$rhi",
                          "-Rendering.FixedStep=0.0166667", "-Stats.Frame=1", "-WorldsLab.TranslucencyLabStats=$Frame",
                          "-WorldsLab.TranslucencyLabStatsCount=$Samples") + $Extra
            $p = Start-Process -FilePath $Exe -ArgumentList $gameArgs -WorkingDirectory (Split-Path $Exe) -PassThru
            $lines = @()
            $waited = 0
            while ($waited -lt $Timeout -and -not $p.HasExited) {
                Start-Sleep -Seconds 2; $waited += 2
                $log = Get-ChildItem "$Logs\WorldsLabGame-*.log" | Where-Object { $_.LastWriteTime -ge $before } |
                       Sort-Object LastWriteTime | Select-Object -Last 1
                if ($log) { $lines = @(Select-String -Path $log.FullName -Pattern "\[TranslucencyLab\] GPU ms at frame (\d+):(.*)") }
                if ($lines.Count -ge $Samples) { break }
            }
            if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
            foreach ($line in $lines) {
                $frameAt = $line.Matches[0].Groups[1].Value
                $spans = $line.Matches[0].Groups[2].Value.Trim() -split '\s+'
                for ($i = 0; $i + 1 -lt $spans.Count; $i += 2) {
                    "$Config,$backend,$view,$run,$frameAt,$($spans[$i]),$($spans[$i + 1])" | Out-File -FilePath $Out -Append -Encoding ascii
                }
            }
            Write-Output "$Config $backend view $view run ${run}: $($lines.Count) samples"
        }
    }
}
