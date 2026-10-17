# One client's whole round, elevated once: measure.ps1 (connected, window
# hidden), then killswitch.ps1. Its output goes to results\<client>.log.
#
#   powershell -File run.ps1 -Client hiddify -Dirs 'C:\...\Hiddify' -Core HiddifyCli
param(
  [Parameter(Mandatory)] [string]$Client,
  [string[]]$Dirs = @(),
  [string[]]$Names = @(),
  [string[]]$Core = @(),
  [string]$Server = '89.169.33.181',
  [switch]$NoKill
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $here "results\$Client.log"
New-Item -ItemType Directory -Force (Join-Path $here 'results') | Out-Null
Start-Transcript -Path $log -Append | Out-Null
try {
  & (Join-Path $here 'measure.ps1') -Client $Client -Dirs $Dirs -Names $Names -Expect $Server
  if (-not $NoKill) {
    & (Join-Path $here 'killswitch.ps1') -Client $Client -Core $Core -Server $Server
  }
} finally {
  Stop-Transcript | Out-Null
}
