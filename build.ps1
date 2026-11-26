<#
.SYNOPSIS
  Local build of EXR Compare (the same steps as the GitHub Actions workflow).

.DESCRIPTION
  Needs: Visual Studio 2022/2026 with "Desktop development with C++", the .NET 10 SDK, git.
  vcpkg is cloned into build\vcpkg automatically. First run builds OpenEXR + OpenColorIO (~20 min).

  Output: build\publish\ExrCompare\ExrCompare.exe (portable folder)
          build\publish\single\ExrCompare.exe     (with -SingleFile)
#>
param([switch]$SingleFile, [switch]$SkipTests)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$vcpkgCommit = "9d1fc4d0609cd98b9ad221f626bf030e856b6345"
$vcpkg = Join-Path $root "build\vcpkg"

if (-not (Test-Path "$vcpkg\vcpkg.exe")) {
    git clone https://github.com/microsoft/vcpkg $vcpkg
    git -C $vcpkg checkout $vcpkgCommit
    & "$vcpkg\bootstrap-vcpkg.bat" -disableMetrics
}

cmake -S "$root\native" -B "$root\build\native-build" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
if ($LASTEXITCODE) { throw "CMake configure failed" }
cmake --build "$root\build\native-build" --config Release --parallel
if ($LASTEXITCODE) { throw "Native build failed" }
cmake --install "$root\build\native-build" --config Release --prefix "$root\build\native"
if (-not $SkipTests) {
    ctest --test-dir "$root\build\native-build" -C Release --output-on-failure
    if ($LASTEXITCODE) { throw "Native tests failed" }
}

$common = @("-c", "Release", "-r", "win-x64", "--self-contained", "-p:Platform=x64", "-p:ExrCoreDir=$root\build\native\bin")
dotnet publish "$root\app\ExrCompare\ExrCompare.csproj" @common -o "$root\build\publish\ExrCompare"
if ($LASTEXITCODE) { throw "App publish failed" }
if ($SingleFile) {
    dotnet publish "$root\app\ExrCompare\ExrCompare.csproj" @common -p:PublishSingleFile=true `
        -p:IncludeNativeLibrariesForSelfExtract=true -p:IncludeAllContentForSelfExtract=true -o "$root\build\publish\single"
    if ($LASTEXITCODE) { throw "Single-file publish failed" }
}
Write-Host "`nDone: $root\build\publish\ExrCompare\ExrCompare.exe"
