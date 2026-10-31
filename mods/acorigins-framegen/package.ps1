# Builds the "Frame Generation for Assassin's Creed Origins" mod package from an OptiScaler build.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File mods\acorigins-framegen\package.ps1 `
#       -BuildDir x64\Release\a -OutDir package
#
# Result: <OutDir>\ACOrigins-FrameGen\ with everything that goes into the game folder, plus a .zip of it.
# Only what DLSS Frame Generation needs is included; NVIDIA's Streamline files are fetched by the user with
# OptiScaler\Streamlined_fetcher_windows.bat (they are not ours to redistribute).

param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Version = '1.0.1'
)

$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
$Repo = (Resolve-Path (Join-Path $Here '..\..')).Path
$BuildDir = (Resolve-Path $BuildDir).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$Name = 'ACOrigins-FrameGen'
$Pkg = Join-Path $OutDir $Name
if (Test-Path $Pkg) { Remove-Item -Recurse -Force $Pkg }
New-Item -ItemType Directory -Force -Path $Pkg | Out-Null

function Copy-Required([string]$From, [string]$To) {
    if (-not (Test-Path $From)) { throw "Missing: $From" }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
    Copy-Item -Force $From $To
}

# OptiScaler loads as winmm.dll (ReShade already uses dxgi.dll)
Copy-Required (Join-Path $BuildDir 'OptiScaler.dll') (Join-Path $Pkg 'winmm.dll')

# DirectX 12 Agility SDK runtime for the DX12 frame generation swapchain
Get-ChildItem (Join-Path $BuildDir 'OptiScaler\D3D12_OptiScaler') -Filter *.dll | ForEach-Object {
    Copy-Required $_.FullName (Join-Path $Pkg ('OptiScaler\D3D12_OptiScaler\' + $_.Name))
}

# Downloads NVIDIA Streamline (sl.*.dll and nvngx_dlssg.dll) into OptiScaler\streamline
Copy-Required (Join-Path $Repo 'Streamlined_fetcher_windows.bat') (Join-Path $Pkg 'OptiScaler\Streamlined_fetcher_windows.bat')

# Optional MFG Unlocker plugin goes here
Copy-Required (Join-Path $Here 'plugins-readme.txt') (Join-Path $Pkg 'OptiScaler\plugins\README.txt')

# OptiScaler.ini: the full documented file with the Origins settings filled in
$preset = [ordered]@{
    'Upscalers'    = [ordered]@{ 'Dx11Upscaler' = 'dlss' }
    'FrameGen'     = [ordered]@{ 'Enabled' = 'true'; 'FGInput' = 'upscaler'; 'FGOutput' = 'dlssg'; 'FGNvngxReplacement' = 'None' }
    'XeFG'         = [ordered]@{ 'ForceBorderless' = 'true' }
    'DLSSG'        = [ordered]@{ 'UseGamesReflexMarkers' = 'false' }
    'Plugins'      = [ordered]@{ 'LoadAsiPlugins' = 'true' }
    'Hotfix'       = [ordered]@{ 'CheckForUpdate' = 'false' }
}

$lines = Get-Content -Path (Join-Path $BuildDir 'OptiScaler.ini') -Encoding UTF8
$section = ''
$done = @{}
for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    if ($line -match '^\[(.+)\]\s*$') { $section = $Matches[1]; continue }
    if ($line -match '^([A-Za-z0-9_]+)=' -and $preset.Contains($section)) {
        $key = $Matches[1]
        if ($preset[$section].Contains($key)) {
            $lines[$i] = "$key=$($preset[$section][$key])"
            $done["$section/$key"] = $true
        }
    }
}
foreach ($s in $preset.Keys) {
    foreach ($k in $preset[$s].Keys) {
        if (-not $done.ContainsKey("$s/$k")) { throw "OptiScaler.ini has no $k in [$s]" }
    }
}
$header = @(
    "; Frame Generation for Assassin's Creed Origins $Version - OptiScaler settings.",
    "; Changed from OptiScaler's defaults: Dx11Upscaler, FrameGen Enabled/FGInput/FGOutput/FGNvngxReplacement,",
    "; XeFG ForceBorderless, DLSSG UseGamesReflexMarkers, Plugins LoadAsiPlugins and Hotfix CheckForUpdate (off: no",
    "; internet access, and official OptiScaler updates would remove the Origins changes). Everything else is 'auto'.",
    ''
)
[System.IO.File]::WriteAllLines((Join-Path $Pkg 'OptiScaler.ini'), [string[]]($header + $lines), (New-Object System.Text.UTF8Encoding($false)))

# Read me, licences and where the source is (GPL-3.0)
Copy-Required (Join-Path $Here 'README.txt') (Join-Path $Pkg "README - Frame Generation for AC Origins.txt")
Copy-Required (Join-Path $Repo 'LICENSE') (Join-Path $Pkg 'Licenses\OptiScaler_LICENSE.txt')
Copy-Required (Join-Path $Repo 'external\reshade\LICENSE.md') (Join-Path $Pkg 'Licenses\ReShade_LICENSE.md')
Copy-Required (Join-Path $BuildDir 'Licenses\DirectX_LICENSE.txt') (Join-Path $Pkg 'Licenses\DirectX_LICENSE.txt')

$commit = (git -C $Repo rev-parse HEAD 2>$null)
if (-not $commit) { $commit = 'unknown' }
$branch = (git -C $Repo rev-parse --abbrev-ref HEAD 2>$null)
if (-not $branch -or $branch -eq 'HEAD') { $branch = $env:GITHUB_REF_NAME }
$source = @(
    "Frame Generation for Assassin's Creed Origins $Version",
    '',
    'winmm.dll is OptiScaler (GPL-3.0) with changes for Assassin''s Creed Origins.',
    'Source code: https://github.com/adnan-altaee/OptiScaler',
    "Branch: $branch",
    "Commit: $commit",
    '',
    'OptiScaler: https://github.com/optiscaler/OptiScaler'
)
[System.IO.File]::WriteAllLines((Join-Path $Pkg 'SOURCE.txt'), [string[]]$source, (New-Object System.Text.UTF8Encoding($false)))

$zip = Join-Path $OutDir "$Name-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $Pkg '*') -DestinationPath $zip

Write-Host "Package: $Pkg"
Get-ChildItem -Recurse -File $Pkg | ForEach-Object { Write-Host ('  ' + $_.FullName.Substring($Pkg.Length + 1) + '  ' + $_.Length) }
Write-Host "Zip: $zip"
