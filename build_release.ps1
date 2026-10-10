<#
.SYNOPSIS
    Builds Harissa64 and packages a release zip ready for an RGH/JTAG Xbox 360.

.DESCRIPTION
    1. Rebuilds the Release | Xbox 360 configuration with Visual Studio 2010
       and the Xbox 360 SDK (skip with -NoBuild).
    2. Checks the output (a fresh harissa64v2.xex with the title data).
    3. Assembles release\Harissa64\ and zips it to
       release\Harissa64-<version>.zip, where <version> comes from VERSION.

    The zip holds one folder, Harissa64\, to extract and copy as-is to the
    console (for example to Hdd1:\Emulators\):

      Harissa64\
        default.xex           the emulator (dashboards scan for default.xex)
        roms\                 put your games here
        art\                  cover, background and icon for the dashboard
        README.md, LICENSE, THIRD_PARTY.md, INSTALL.txt

    Saves and settings are created by the emulator on the console. No ROMs,
    saves, logs or local settings are ever packaged.

.PARAMETER NoBuild
    Package the existing build instead of rebuilding.

.EXAMPLE
    .\build_release.ps1
    .\build_release.ps1 -NoBuild
#>
[CmdletBinding()]
param(
    [switch] $NoBuild
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$version = (Get-Content (Join-Path $repo 'VERSION') -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must look like 1.2.3 (got '$version')." }

$build = Join-Path $repo 'platform\xbox360\Release'
$xex = Join-Path $build 'harissa64v2.xex'
$art = Join-Path $repo 'platform\xbox360\title\art'
$releaseDir = Join-Path $repo 'release'
$stage = Join-Path $releaseDir 'Harissa64'
$zip = Join-Path $releaseDir "Harissa64-$version.zip"

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

# ---------------------------------------------------------------- build --
if (-not $NoBuild) {
    Step "Building Release | Xbox 360"
    $vcvars = 'E:\tools\Visual Studio 10.0\VC\vcvarsall.bat'
    if (-not (Test-Path $vcvars)) {
        $vsTools = [Environment]::GetEnvironmentVariable('VS100COMNTOOLS', 'Machine')
        if (-not $vsTools) { $vsTools = $env:VS100COMNTOOLS }
        if ($vsTools) { $vcvars = Join-Path $vsTools '..\..\VC\vcvarsall.bat' }
    }
    if (-not (Test-Path $vcvars)) { throw "Visual Studio 2010 (vcvarsall.bat) not found." }
    $msbuild = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\MSBuild.exe'
    $sln = Join-Path $repo 'platform\xbox360\harissa64v2.sln'
    $log = Join-Path $env:TEMP 'harissa64_release_build.log'
    $started = Get-Date
    # The XEX packaging step does not always run again on its own: start from no xex.
    if (Test-Path $xex) { Remove-Item -LiteralPath $xex -Force }
    cmd /c "`"$vcvars`" x86 >nul && `"$msbuild`" `"$sln`" /t:Rebuild /p:Configuration=Release /p:Platform=`"Xbox 360`" /m /nologo /v:minimal /fl /flp:logfile=`"$log`";verbosity=normal"
    if ($LASTEXITCODE -ne 0) { throw "Build failed (see $log)." }
    if (-not (Test-Path $xex) -or (Get-Item $xex).LastWriteTime -lt $started) {
        throw "Build finished but $xex was not regenerated (see $log)."
    }
}

# ---------------------------------------------------------------- checks --
Step "Checking build output"
if (-not (Test-Path $xex)) { throw "Missing $xex. Build first or drop -NoBuild." }
foreach ($f in 'cover.png', 'background.png', 'icon.png') {
    if (-not (Test-Path (Join-Path $art $f))) { throw "Missing $(Join-Path $art $f)." }
}

# ---------------------------------------------------------------- stage --
Step "Staging release\Harissa64"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $xex (Join-Path $stage 'default.xex')
Copy-Item (Join-Path $repo 'README.md'), (Join-Path $repo 'LICENSE'), (Join-Path $repo 'THIRD_PARTY.md') $stage
New-Item -ItemType Directory -Force (Join-Path $stage 'roms') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'art') | Out-Null
foreach ($f in 'cover.png', 'background.png', 'icon.png') { Copy-Item (Join-Path $art $f) (Join-Path $stage 'art') }

$install = @"
Harissa64 $version - Nintendo 64 emulator for Xbox 360 (RGH/JTAG)
==================================================================

1. Copy the whole Harissa64 folder to your console,
   for example to Hdd1:\Emulators\Harissa64\

2. Put your own games (.z64, .n64, .v64 or .zip) in Harissa64\roms\
   File names must be at most 42 characters, without commas, '+', ';', '='
   or accents (limits of the Xbox 360 file system).

3. In Aurora, add the parent folder (e.g. Hdd1:\Emulators\) to the scan
   paths, or browse to Harissa64 and launch default.xex. It shows up as
   "Harissa64". The art folder has a cover and a background you can load
   from the game's asset menu in Aurora.

In a game: a short press on BACK opens the menu (save and load states,
settings, back to the game list). BACK + RB saves a state, BACK + LB loads it.
Your saves go to Harissa64\saves\.

Games we tested and their speed: docs/games.md on the project page.
Source code: https://github.com/kernel64/Harissa64
Licence: GNU GPL v2 (see LICENSE).
"@
[IO.File]::WriteAllText((Join-Path $stage 'INSTALL.txt'), $install.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# ---------------------------------------------------------------- zip --
Step "Creating $zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
# CreateFromDirectory keeps the empty roms\ folder.
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)

# Sanity check: no ROMs or local files slipped in.
$zipFile = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $bad = $zipFile.Entries | Where-Object { $_.FullName -match '(?i)\.(z64|n64|v64|log|ini|h64s|bin|bmp)$' }
    if ($bad) { throw "Unexpected files in the zip: $($bad.FullName -join ', ')" }
    $count = $zipFile.Entries.Count
}
finally { $zipFile.Dispose() }

$size = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Step "Done: release\Harissa64-$version.zip ($size MB, $count entries)"
