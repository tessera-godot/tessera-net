# Builds and tests Tessera Net on Windows. Usage: ./tools/build.ps1
# Requires: CMake 3.21+ and Visual Studio 2022 with the C++ workload.
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

cmake --preset windows-msvc
if ($LASTEXITCODE -ne 0) { throw "Configure failed. Check that Visual Studio 2022 (C++ workload) and CMake are installed." }

cmake --build --preset windows-msvc-release
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

ctest --preset windows-msvc-release
if ($LASTEXITCODE -ne 0) { throw "Tests failed." }

Write-Host "Build and tests succeeded."
