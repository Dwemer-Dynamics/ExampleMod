# Builds the example for x64 and x86 (Release) outside the source folder.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-BuildRoot <folder>]
param(
    [string]$BuildRoot = (Join-Path $env:LOCALAPPDATA "DwemerDynamics\ExampleMod\build")
)
$ErrorActionPreference = "Stop"

$source = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmake) {
    # Find bundled CMake in any installed Visual Studio edition.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $cmake = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
    }
}
if (-not $cmake -or -not (Test-Path -LiteralPath $cmake)) {
    throw "CMake was not found. Install Visual Studio 2022 Desktop development with C++, including CMake tools, or add CMake to PATH."
}

foreach ($arch in @(@{ Name = "x64"; Platform = "x64" }, @{ Name = "x86"; Platform = "Win32" })) {
    $dir = Join-Path $BuildRoot $arch.Name
    & $cmake -S $source -B $dir -A $arch.Platform
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $($arch.Name)" }
    & $cmake --build $dir --config Release
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $($arch.Name)" }
    Write-Host "Built $dir\Release\example_mod.exe"
}
