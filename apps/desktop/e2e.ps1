# End-to-end tests of the desktop app on Windows: configure, build, run.
#
#   powershell -File apps\desktop\e2e.ps1 [-Windowed] [-Repeat N] [-BuildDir DIR] [extra ctest arguments]
#
# Same as, after the normal desktop build:
#   ctest --test-dir build\desktop -L desktop-e2e -C Release --output-on-failure
# Scratch folders and failure screenshots: <build>\apps\desktop\e2e\{runs,failures}.
param(
  [switch]$Windowed,
  [int]$Repeat = 1,
  [string]$BuildDir = "",
  [Parameter(ValueFromRemainingArguments = $true)][string[]]$CtestArgs = @()
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ($BuildDir -eq "") { $BuildDir = Join-Path $root "build\desktop" }

cmake -S $root -B $BuildDir -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $BuildDir -j 6 --config Release --target convertible_desktop convertible_desktop_e2e
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Windowed) { $env:CVD_E2E_WINDOWED = "1" }
for ($i = 1; $i -le $Repeat; $i++) {
  Write-Host "== desktop end-to-end run $i of $Repeat"
  ctest --test-dir $BuildDir -L desktop-e2e -C Release --output-on-failure @CtestArgs
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
