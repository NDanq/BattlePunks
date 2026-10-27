param(
    [string]$OutDir,                       # where <Tag>_v<view>.bmp land
    [string]$Tag,
    [int[]]$Views = @(0, 1, 2, 3),
    [string]$Level = "TranslucencyLab",    # another level for regression captures (its own camera; views ignored)
    [int]$Rhi = 2,                         # 1 OpenGL, 2 Vulkan, 4 D3D12
    [int]$Frame = 300,                     # the game frame captured ([Record] StartFrame)
    [string[]]$Extra = @(),                # more -Section.Key=Value arguments
    [string]$Exe = "$PSScriptRoot\..\..\Binaries\Windows_Debug\WorldsLab.exe",
    [int]$Timeout = 180,
    [switch]$Stats                         # also log the GPU time per span at $Frame (stat frame)
)
# TranslucencyLab captures (Engine/Modules/RenderCore/LIT_TRANSLUCENCY.md, "Validation"):
# each view's frame $Frame, with the render clock fixed ([Rendering] FixedStep) so the
# particles are the same every run. FrameRecorder pipes the frame to "ffmpeg"; this
# folder's stand-in (fakeffmpeg\) keeps it as a .bmp. Compare runs with compare.py.
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$env:PATH = "$PSScriptRoot\fakeffmpeg;" + $env:PATH
foreach ($view in $Views) {
    $stem = Join-Path (Resolve-Path $OutDir) "${Tag}_v$view"
    Remove-Item "$stem.bmp" -ErrorAction SilentlyContinue
    $gameArgs = @("-Game.StartLevel=$Level", "-WorldsLab.TranslucencyLabView=$view", "-Display.RHI=$Rhi",
              "-Rendering.FixedStep=0.0166667", "-Record.File=$stem.mp4", "-Record.Fps=30",
              "-Record.StartFrame=$Frame", "-Record.Seconds=0.03", "-WorldsLab.HoldCamera=1") + $Extra
    if ($Stats) { $gameArgs += @("-Stats.Frame=1", "-WorldsLab.TranslucencyLabStats=$Frame") }
    $p = Start-Process -FilePath $Exe -ArgumentList $gameArgs -WorkingDirectory (Split-Path $Exe) -PassThru
    $waited = 0
    while (-not (Test-Path "$stem.bmp") -and $waited -lt $Timeout -and -not $p.HasExited) { Start-Sleep -Seconds 1; $waited++ }
    Start-Sleep -Seconds 1
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    if (Test-Path "$stem.bmp") { Write-Output "saved $stem.bmp" } else { Write-Output "no frame for view $view" }
    if ($Stats) {
        $log = Get-ChildItem (Join-Path (Split-Path $Exe) "Saved\Logs\WorldsLabGame-*.log") | Sort-Object LastWriteTime | Select-Object -Last 1
        Select-String -Path $log.FullName -Pattern "\[TranslucencyLab\] GPU ms" | ForEach-Object { "view ${view}: " + ($_.Line -replace '^.*GPU ms', 'GPU ms') }
    }
}
