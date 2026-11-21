# Wraps the packaged game (Saved\Package\Windows, from Tools\package_game.bat) in a single setup exe.
# Output: Saved\Installer\ManchesterRide-Setup.exe
# Uses only what Windows already has: the .NET Framework C# compiler, plus 7-Zip if installed (faster zipping).
param([string]$Package, [string]$Out)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Split-Path (Split-Path $here)
if (-not $Package) { $Package = Join-Path $root 'Saved\Package\Windows' }
if (-not $Out) { $Out = Join-Path $root 'Saved\Installer\ManchesterRide-Setup.exe' }
if (-not (Test-Path (Join-Path $Package 'ManchesterRide.exe'))) { throw "No packaged game in $Package. Run Tools\package_game.bat first." }
$work = Join-Path $root 'Saved\Installer\work'
New-Item -ItemType Directory -Force $work | Out-Null

$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$stub = Join-Path $work 'stub.exe'
& $csc /nologo /target:winexe /optimize+ /platform:anycpu "/out:$stub" "/win32icon:$here\art\app.ico" `
  "/resource:$here\art\side.png,side.png" "/resource:$here\art\app.ico,app.ico" `
  /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll "$here\Setup.cs"
if ($LASTEXITCODE -ne 0) { throw 'The installer program did not compile.' }

# Files the shipping game does not need at runtime, left out to keep the installer small.
# Each is either an optional engine library the game never calls, or a developer-only debug lib.
#   DirectML.dll / onnxruntime*.dll - the NNE neural-network runtime; this game runs no ML models.
#   EOSSDK-Win64-Shipping.dll       - Epic Online Services; the game uses the Null subsystem over the Playit tunnel, not EOS.
#   GFSDK_Aftermath_Lib.x64.dll     - NVIDIA Aftermath GPU crash dumps (developer diagnostics).
#   d3d12SDKLayers.dll              - the Direct3D 12 debug layer, only loaded with -d3ddebug.
#   msquic.dll                      - the QUIC transport, unused here.
#   UEPrereqSetup_x64.exe           - the VC++ redistributable installer. This per-user setup never runs it
#                                     (running it needs admin), so it was dead weight; the game relies on the
#                                     Visual C++ 2015-2022 x64 runtime already being present, as it did before.
# If any turns out to be needed, the game fails to launch in the post-build test and it goes back in.
$excludeNames = @('DirectML.dll','onnxruntime.dll','onnxruntime_providers_shared.dll','EOSSDK-Win64-Shipping.dll','GFSDK_Aftermath_Lib.x64.dll','d3d12SDKLayers.dll','msquic.dll','UEPrereqSetup_x64.exe')

$zip = Join-Path $work 'payload.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
$7z = 'C:\Program Files\7-Zip\7z.exe'
if (Test-Path $7z) {
  $xargs = $excludeNames | ForEach-Object { "-xr!$_" }
  Push-Location $Package
  & $7z a -tzip -mx=9 -mfb=258 -mmt=on -bso0 -bsp0 @xargs $zip '.\*'
  $code = $LASTEXITCODE; Pop-Location
  if ($code -ne 0) { throw '7-Zip failed.' }
} else {
  # No 7-Zip: prune a throwaway copy, then zip it at maximum compression.
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $prune = Join-Path $work 'stage'
  if (Test-Path $prune) { Remove-Item $prune -Recurse -Force }
  Copy-Item $Package $prune -Recurse
  Get-ChildItem $prune -Recurse -File | Where-Object { $excludeNames -contains $_.Name } | Remove-Item -Force
  [System.IO.Compression.ZipFile]::CreateFromDirectory($prune, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
  Remove-Item $prune -Recurse -Force
}

# Game version from Source/ManchesterRide/Net/MRVersion.h, stamped in so the dedicated server knows what this installer holds.
$verH = Get-Content (Join-Path $root 'Source\ManchesterRide\Net\MRVersion.h') -Raw
$build = [uint32]([regex]::Match($verH, '#define\s+MR_GAME_BUILD\s+(\d+)').Groups[1].Value)
$version = [regex]::Match($verH, '#define\s+MR_GAME_VERSION\s+"([^"]+)"').Groups[1].Value
if (-not $build -or -not $version) { throw 'Could not read MR_GAME_BUILD / MR_GAME_VERSION from MRVersion.h' }
$vb = New-Object byte[] 64
[System.Text.Encoding]::ASCII.GetBytes('MRVER001').CopyTo($vb, 0)
[System.BitConverter]::GetBytes($build).CopyTo($vb, 8)
$vs = [System.Text.Encoding]::ASCII.GetBytes($version); [Array]::Copy($vs, 0, $vb, 12, [Math]::Min(52, $vs.Length))

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$o = [System.IO.File]::Create($Out)
try {
  $s = [System.IO.File]::OpenRead($stub); $s.CopyTo($o); $offset = $s.Length; $s.Dispose()
  $z = [System.IO.File]::OpenRead($zip); $z.CopyTo($o, 1MB); $length = $z.Length; $z.Dispose()
  $o.Write($vb, 0, 64)
  $o.Write([System.Text.Encoding]::ASCII.GetBytes('MRPAYLD2'), 0, 8)
  $o.Write([System.BitConverter]::GetBytes([int64]$offset), 0, 8)
  $o.Write([System.BitConverter]::GetBytes([int64]$length), 0, 8)
} finally { $o.Dispose() }
Remove-Item $zip -Force
"{0}  {1:N0} MB  (version {2}, build {3})" -f $Out, ((Get-Item $Out).Length / 1MB), $version, $build
