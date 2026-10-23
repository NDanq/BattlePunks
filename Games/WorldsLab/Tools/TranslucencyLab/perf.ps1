param(
    [string]$Out,                              # a CSV: config, backend, view, run, frame, span, ms (appended)
    [string[]]$Configs = @("all="),            # "name=-Section.Key=Value -Section.Key=Value": each config's arguments
    [int[]]$Views = @(0, 1, 2, 3),
    [int[]]$Rhis = @(2, 4),                    # 2 Vulkan, 4 D3D12
    [int]$Runs = 2,                            # launches per config, view and backend
    [int]$Frame = 480, [int]$Samples = 3,      # GPU spans logged at Frame, Frame + 120, ...
    [string]$Exe = "$PSScriptRoot\..\..\Binaries\Windows_RelWithDebInfo\WorldsLab.exe",
    [int]$Timeout = 150
)
# TranslucencyLab's GPU cost (Engine/Modules/RenderCore/LIT_TRANSLUCENCY_PERF.md;
# .claude/skills/perf-testing): the scene's GPU time per span ("stat frame"), with the
# render clock fixed and no frame read back (a capture's readback stalls the GPU and
# skews the spans). RelWithDebInfo by default: Debug's numbers are not the engine's.
#
# The configs are INTERLEAVED -- each round launches every config once, view by view --
# so a GPU clock drifting over the minutes a measurement takes hits every config alike.
# Summarise with perf.py.
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Exe)) { throw "no ${Exe}: build the RelWithDebInfo tree (Build/editor-relwithdebinfo) first" }
$Logs = Join-Path (Split-Path $Exe) "Saved\Logs"
if (-not (Test-Path $Out)) { "config,backend,view,run,frame,span,ms" | Out-File -FilePath $Out -Encoding ascii }
$Parsed = foreach ($each in $Configs) {
    $name, $rest = $each -split '=', 2
    [pscustomobject]@{ Name = $name; Args = @(($rest -split '\s+') | Where-Object { $_ }) }
}
foreach ($rhi in $Rhis) {
    $backend = if ($rhi -eq 4) { "D3D12" } elseif ($rhi -eq 1) { "OpenGL" } else { "Vulkan" }
    foreach ($view in $Views) {
        for ($run = 1; $run -le $Runs; $run++) {
            foreach ($config in $Parsed) {
                $before = Get-Date
                $gameArgs = @("-Game.StartLevel=TranslucencyLab", "-WorldsLab.TranslucencyLabView=$view", "-Display.RHI=$rhi",
                              "-Rendering.FixedStep=0.0166667", "-Stats.Frame=1", "-WorldsLab.TranslucencyLabStats=$Frame",
                              "-WorldsLab.TranslucencyLabStatsCount=$Samples") + $config.Args
                $p = Start-Process -FilePath $Exe -ArgumentList $gameArgs -WorkingDirectory (Split-Path $Exe) -PassThru
                $lines = @()
                $waited = 0
                while ($waited -lt $Timeout -and -not $p.HasExited) {
                    Start-Sleep -Seconds 2; $waited += 2
                    $log = Get-ChildItem "$Logs\WorldsLabGame-*.log" -ErrorAction SilentlyContinue |
                           Where-Object { $_.LastWriteTime -ge $before } | Sort-Object LastWriteTime | Select-Object -Last 1
                    if ($log) { $lines = @(Select-String -Path $log.FullName -Pattern "\[TranslucencyLab\] GPU ms at frame (\d+):(.*)") }
                    if ($lines.Count -ge $Samples) { break }
                }
                if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
                foreach ($line in $lines) {
                    $frameAt = $line.Matches[0].Groups[1].Value
                    $spans = $line.Matches[0].Groups[2].Value.Trim() -split '\s+'
                    for ($i = 0; $i + 1 -lt $spans.Count; $i += 2) {
                        "$($config.Name),$backend,$view,$run,$frameAt,$($spans[$i]),$($spans[$i + 1])" | Out-File -FilePath $Out -Append -Encoding ascii
                    }
                }
                Write-Output "$($config.Name) $backend view $view run ${run}: $($lines.Count) samples"
            }
        }
    }
}
