<#
.SYNOPSIS
  Builds Sovereign-Setup-<version>.exe and its .sha256 into -OutDir.

.DESCRIPTION
  Release build (preset dist: the CRT linked in, so no VC++ redistributable),
  the files the installer ships staged into one folder, Inno Setup's compiler
  over installer/sovereign.iss, and a sha256sum-style checksum next to the
  installer - the tray's updater checks the download against it.
  Needs what the CI's build job needs (vendor/sing-box, vendor/wintun, MinGW
  gcc, vcpkg) plus Inno Setup 6 (installed with Chocolatey if missing).

.EXAMPLE
  pwsh installer/build.ps1 -Version 0.2.0 -OutDir out
#>
param(
  [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
  [string]$OutDir = 'out'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
  cmake --preset dist "-DSOVEREIGN_VERSION=$Version"
  if ($LASTEXITCODE -ne 0) { throw "configure failed" }
  cmake --build --preset dist
  if ($LASTEXITCODE -ne 0) { throw "build failed" }

  # Everything the service and the tray load at run time, from where CMake
  # put it (the service's POST_BUILD copies the DLLs next to its exe).
  $stage = Join-Path $root 'build/dist/stage'
  Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  $files = @(
    'build/dist/src/service/sovereign-core.exe',
    'build/dist/src/service/sovereign-gocore.dll',
    'build/dist/src/service/wintun.dll',
    'build/dist/src/service/libcronet.dll',
    'build/dist/src/tray/sovereign-tray.exe',
    'LICENSE'
  )
  foreach ($file in $files) {
    if (-not (Test-Path $file)) { throw "missing $file" }
    Copy-Item $file $stage
  }

  $iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  if (-not $iscc) {
    $known = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    if (-not (Test-Path $known)) {
      choco install innosetup -y --no-progress
      if ($LASTEXITCODE -ne 0) { throw "installing Inno Setup failed" }
    }
    $iscc = Get-Item $known
  }

  New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
  $out = (Resolve-Path $OutDir).Path
  & $iscc.Source "/DAppVersion=$Version" "/DSourceDir=$stage" "/DOutputDir=$out" 'installer/sovereign.iss'
  if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }

  $name = "Sovereign-Setup-$Version.exe"
  $hash = (Get-FileHash (Join-Path $out $name) -Algorithm SHA256).Hash.ToLower()
  # LF, no BOM: the format sha256sum -c reads, and what the updater parses.
  [System.IO.File]::WriteAllText((Join-Path $out "$name.sha256"), "$hash  $name`n")
  Write-Host "$name  $hash"
} finally {
  Pop-Location
}
