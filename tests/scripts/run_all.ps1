# Builds every target and runs the unit tests where they can run.
#   1. Windows host, MSVC 2026 + CMake/Ninja       -> build-msvc\h64test.exe --unit
#   2. WSL gcc little-endian, ppc64 big-endian/qemu -> tests/scripts/run_wsl.sh
#   3. Xbox 360, VS2010 + XDK (MSBuild)             -> platform\xbox360\Release\harissa64v2.xex
#      (+ the execmem proof of concept with -ExecMem)
# The Xbox build only compiles: its unit tests run at start-up in Xenia or on
# the console (see CLAUDE.md). Exit code 1 if anything fails.
param([switch]$ExecMem, [switch]$SkipXbox, [switch]$SkipWsl)

$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$vs2026 = 'E:\tools\Microsoft Visual Studio\2026\VC\Auxiliary\Build\vcvars64.bat'
$vs2010 = 'E:\tools\Visual Studio 10.0\VC\vcvarsall.bat'
$msbuild = 'C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe'
$failed = @()

Write-Host '== Windows host (MSVC 2026)'
$out = cmd /c "`"$vs2026`" >nul 2>&1 && cd /d `"$root`" && cmake -S . -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release >nul && cmake --build build-msvc && build-msvc\h64test.exe --unit" 2>&1
$out | Select-String -Pattern 'warning|error|UNIT|FAIL' | ForEach-Object { $_.Line }
if ($LASTEXITCODE -ne 0) { $failed += 'msvc' }

if (-not $SkipWsl) {
    Write-Host '== WSL'
    $wslRoot = (wsl -d Ubuntu -- wslpath -a ($root -replace '\\', '/')).Trim()
    wsl -d Ubuntu -- bash "$wslRoot/tests/scripts/run_wsl.sh"
    if ($LASTEXITCODE -ne 0) { $failed += 'wsl' }
}

if (-not $SkipXbox) {
    Write-Host '== Xbox 360 (VS2010 + XDK)'
    $sln = Join-Path $root 'platform\xbox360\harissa64v2.sln'
    $out = cmd /c "`"$vs2010`" x86 >nul && `"$msbuild`" `"$sln`" /p:Configuration=Release /p:Platform=`"Xbox 360`" /m /v:minimal" 2>&1
    $out | Select-String -Pattern 'warning|error' | ForEach-Object { $_.Line }
    if ($LASTEXITCODE -ne 0) { $failed += 'xbox' }
    $xex = Join-Path $root 'platform\xbox360\Release\harissa64v2.xex'
    if (Test-Path $xex) { Write-Host ("xex: {0} ({1:N0} bytes, {2})" -f $xex, (Get-Item $xex).Length, (Get-Item $xex).LastWriteTime) }
    else { $failed += 'xbox (no xex)' }
    if ($ExecMem) {
        cmd /c (Join-Path $root 'platform\xbox360\tools\execmem\build.cmd')
        if ($LASTEXITCODE -ne 0) { $failed += 'execmem' }
    }
}

if ($failed.Count) { Write-Host "RESULT: FAILED ($($failed -join ', '))"; exit 1 }
Write-Host 'RESULT: ALL TARGETS OK'
exit 0
