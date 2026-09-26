$ErrorActionPreference = 'Stop'

$vcvars = 'E:\Visual Studio 2026\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}

# vcvars64.bat only persists its environment inside cmd. Import `set` into this process.
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^(?<name>[^=]+)=(?<value>.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches.name, $Matches.value, 'Process')
    }
}

$env:PATH = @(
    'E:\Visual Studio 2026\VC\Tools\Llvm\x64\bin'
    'C:\VulkanSDK\1.4.350.0\Bin'
    'C:\Qt\Tools\Ninja'
    $env:PATH
) -join ';'

Set-Location -LiteralPath $PSScriptRoot

$cache = Join-Path $PSScriptRoot '_Build\windows\CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) {
    cmake -S . -B _Build/windows -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_C_COMPILER=clang-cl `
        -DCMAKE_CXX_COMPILER=clang-cl `
        -DCMAKE_PREFIX_PATH='C:/Qt/6.11.1/msvc2022_64'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

cmake --build _Build/windows --target launcher --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --install _Build/windows --prefix _Build/windows/install
exit $LASTEXITCODE
