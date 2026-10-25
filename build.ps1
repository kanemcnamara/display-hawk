# Builds Display Hawk with the Qt MinGW toolchain installed under C:\Qt and deploys the Qt runtime next to the exe.
param(
    [ValidateSet('Release', 'Debug')] [string] $Config = 'Release',
    [string] $QtDir = 'C:\Qt\6.8.3\mingw_64',
    [string] $MinGWDir = 'C:\Qt\Tools\mingw1310_64'
)
# Native tools (cmake, windeployqt) print status to stderr; failures are detected via $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
$pyScripts = Join-Path $env:APPDATA 'Python\Python312\Scripts'
$env:PATH = "$MinGWDir\bin;$QtDir\bin;$pyScripts;$env:PATH"
$build = Join-Path $PSScriptRoot "build\$Config"

# A running instance locks the exe and DLLs, which makes linking or deployment fail (or leave empty files).
Get-Process DisplayHawk -ErrorAction SilentlyContinue | Stop-Process -Force

cmake -S $PSScriptRoot -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_PREFIX_PATH=$QtDir"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$ErrorActionPreference = 'Continue'  # windeployqt prints harmless warnings to stderr
windeployqt --no-translations --no-compiler-runtime (Join-Path $build 'DisplayHawk.exe') 2>&1 | Out-Null
# MinGW runtime DLLs
Copy-Item "$MinGWDir\bin\libgcc_s_seh-1.dll", "$MinGWDir\bin\libstdc++-6.dll", "$MinGWDir\bin\libwinpthread-1.dll" $build -Force
$broken = Get-ChildItem $build -Recurse -Filter *.dll | Where-Object Length -eq 0
if ($broken) {
    Write-Error "Deployment left empty DLLs: $($broken.FullName -join ', ')"
    exit 1
}
Write-Host "Built $build\DisplayHawk.exe"
