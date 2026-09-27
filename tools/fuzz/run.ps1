<#
.SYNOPSIS
  Runs the libFuzzer targets of tests/fuzz (built with the ci-fuzz preset) for a while each.

.DESCRIPTION
  Each target starts from its seeds in tests/fuzz/corpus/<target> and writes what it finds
  into a work corpus under the temp directory, so the seeds in the repo stay as they are
  (new interesting inputs are worth adding to them by hand, with -minimize). A crash, an
  uncaught exception or a broken property (std::abort in the target) leaves its input in
  -Artifacts as <target>-crash-<hash> and fails the run.

.EXAMPLE
  pwsh tools/fuzz/run.ps1 -Seconds 60
#>
param(
  [int]$Seconds = 60,
  [string]$Build = "build/ci-fuzz",
  [string]$Artifacts = "fuzz-artifacts",
  [string[]]$Targets = @('client-hello', 'config', 'tray-input', 'control')
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '../..')
New-Item -ItemType Directory -Force $Artifacts | Out-Null
$artifactDir = (Resolve-Path $Artifacts).Path
$failed = @()

foreach ($target in $Targets) {
  $exe = Join-Path $repo "$Build/tests/fuzz/fuzz-$target.exe"
  if (-not (Test-Path $exe)) { throw "no $exe - build the ci-fuzz preset first" }
  $seeds = Join-Path $repo "tests/fuzz/corpus/$target"
  $work = Join-Path ([IO.Path]::GetTempPath()) "sovereign-fuzz-$target"
  New-Item -ItemType Directory -Force $work | Out-Null

  $options = @("-max_total_time=$Seconds", "-timeout=10", "-rss_limit_mb=2048",
    "-artifact_prefix=$artifactDir/$target-", "-print_final_stats=1")
  # JSON-shaped inputs: a dictionary of syntax and the keys the code branches on.
  if ($target -ne 'client-hello') { $options += "-dict=$(Join-Path $repo 'tests/fuzz/json.dict')" }

  Write-Host "== $target ($Seconds s)"
  # libFuzzer reports on stderr; keep its summary lines and the whole report on failure.
  $output = & $exe $work $seeds @options 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $output | Where-Object { $_ -match '^(#\d+\s+(DONE|INITED)|stat::number_of_executed_units|stat::peak_rss_mb)' } |
    ForEach-Object { Write-Host "   $_" }
  if ($code -ne 0) {
    Write-Host ($output | Select-Object -Last 60 | Out-String)
    $failed += $target
  }
}

if ($failed.Count -gt 0) {
  throw "fuzz findings in: $($failed -join ', ') - inputs in $artifactDir"
}
Write-Host "no findings"
