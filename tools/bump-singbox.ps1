# Moves the pinned sing-box to another release - everything a bump changes that a
# script can change (see 512db6e, 1.14.3 -> 1.14.4, for the manual version):
#
#   tools/codegen/sing-box.version         the tag and the commit it points at
#   vendor/sing-box                        that tag, checked out (not committed)
#   gocore, tools/boxrun, tools/goldengen  go mod tidy against it
#   src/generated                          tools/codegen/generate.ps1
#   tools/conformance/reference-release.sha256
#                                          the official windows-amd64 zip, its
#                                          sha256 checked against the digest GitHub
#                                          reports for the asset
#   README.md, bench/*.ps1                 the version they name
#
# Not here, on purpose: re-recording the golden ClientHellos (they are recorded
# from the reference with conformance-harness, after the old ones were checked
# against the new reference - CI does that check) and the C++ side (gocore/main.go
# follows upstream API changes by hand).
#
#   pwsh tools/bump-singbox.ps1 -Version 1.14.5
#   pwsh tools/bump-singbox.ps1 -Version v1.14.5 -SkipGo   # pin and files only
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Version,
  [switch]$SkipGo
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

$Version = $Version.TrimStart('v')
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "version must be MAJOR.MINOR.PATCH, got '$Version'" }
$tag = "v$Version"

$versionFile = Join-Path $root 'tools/codegen/sing-box.version'
$oldTag = (Get-Content $versionFile)[0].Trim()
$old = $oldTag.TrimStart('v')

# Rewrites a file keeping its line endings (the working copies are CRLF on
# Windows, LF on the runners; a bump must not turn one into the other).
function Update-File([string]$Path, [scriptblock]$Edit) {
  $text = [IO.File]::ReadAllText($Path)
  $new = & $Edit $text
  if ($new -ne $text) { [IO.File]::WriteAllText($Path, $new, (New-Object Text.UTF8Encoding $false)) }
}
function Headers {
  $h = @{ 'User-Agent' = 'sovereign-bump'; Accept = 'application/vnd.github+json' }
  if ($env:GITHUB_TOKEN) { $h.Authorization = "Bearer $env:GITHUB_TOKEN" }
  $h
}

# 1. The tag's commit. An annotated tag lists the commit as <tag>^{}.
$refs = @(git ls-remote https://github.com/SagerNet/sing-box.git "refs/tags/$tag" "refs/tags/$tag^{}")
if ($LASTEXITCODE -ne 0 -or $refs.Count -eq 0) { throw "sing-box has no tag $tag" }
$line = $refs | Where-Object { $_ -match "\^\{\}$" } | Select-Object -First 1
if (-not $line) { $line = $refs[0] }
$sha = ($line -split '\s+')[0]
if ($sha -notmatch '^[0-9a-f]{40}$') { throw "unexpected ls-remote answer: $line" }

# 2. The reference zip: its sha256 must equal the digest GitHub reports for the asset.
$zipName = "sing-box-$Version-windows-amd64.zip"
$release = Invoke-RestMethod -Headers (Headers) "https://api.github.com/repos/SagerNet/sing-box/releases/tags/$tag"
$asset = $release.assets | Where-Object name -eq $zipName
if (-not $asset) { throw "release $tag has no asset $zipName" }
if (-not $asset.digest -or $asset.digest -notmatch '^sha256:([0-9a-f]{64})$') { throw "release $tag reports no sha256 digest for $zipName" }
$digest = $Matches[1]

$work = Join-Path ([IO.Path]::GetTempPath()) "bump-singbox-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
try {
  $zip = Join-Path $work $zipName
  $ProgressPreference = 'SilentlyContinue'  # Windows PowerShell 5.1 crawls with the progress bar
  for ($try = 1; -not (Test-Path $zip); $try++) {
    try {
      Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = 'sovereign-bump' } -OutFile $zip -Uri "https://github.com/SagerNet/sing-box/releases/download/$tag/$zipName"
    } catch {
      if ($try -ge 4) { throw "$zipName download failed: $_" }
      Start-Sleep -Seconds (5 * $try)
    }
  }
  $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
  if ($actual -ne $digest) { throw "$zipName sha256 is $actual, GitHub reports $digest" }
} finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

# 3. The pin and the files that name the version.
Update-File $versionFile { param($t) $t -replace '^v\d+\.\d+\.\d+', $tag -replace '(?m)^[0-9a-f]{40}', $sha }
Update-File (Join-Path $root 'tools/conformance/reference-release.sha256') {
  param($t) $t -replace '^[0-9a-f]{64}  sing-box-[\d.]+-windows-amd64\.zip', "$digest  $zipName"
}
Update-File (Join-Path $root 'README.md') { param($t) $t.Replace("--branch $oldTag ", "--branch $tag ") }
foreach ($script in 'bench/check-links.ps1', 'bench/reality-rate.ps1') {
  Update-File (Join-Path $root $script) { param($t) $t.Replace("sing-box-$old-windows-amd64", "sing-box-$Version-windows-amd64") }
}

# 4. The sources the Go modules and the generator build against.
$vendor = Join-Path $root 'vendor/sing-box'
if (Test-Path $vendor) { Remove-Item -Recurse -Force $vendor }
git clone --depth 1 --branch $tag https://github.com/SagerNet/sing-box.git $vendor
if ($LASTEXITCODE -ne 0) { throw "cloning sing-box $tag failed" }
$head = (git -C $vendor rev-parse HEAD).Trim()
if ($head -ne $sha) { throw "vendor/sing-box is at $head, the tag's commit is $sha" }

if (-not $SkipGo) {
  foreach ($module in 'gocore', 'tools/boxrun', 'tools/goldengen') {
    Push-Location (Join-Path $root $module)
    try {
      go mod tidy
      if ($LASTEXITCODE -ne 0) { throw "go mod tidy failed in $module" }
    } finally {
      Pop-Location
    }
  }
  & (Join-Path $root 'tools/codegen/generate.ps1')
}

Write-Output "sing-box $old -> $Version (commit $sha, reference sha256 $digest)"
