<#
.SYNOPSIS
  Installs Sovereign with the given installer the way the updater does
  (silently, elevated), checks the result, runs the same installer again as
  an update, uninstalls, checks nothing is left. Needs admin (CI runners are).

.EXAMPLE
  pwsh installer/smoke.ps1 -Installer out/Sovereign-Setup-0.2.0.exe
#>
param([Parameter(Mandatory)][string]$Installer)
$ErrorActionPreference = 'Stop'
$app = Join-Path $env:ProgramFiles 'Sovereign'
$failures = 0

function Expect([bool]$ok, [string]$what) {
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what"; $script:failures++ }
}

function Send-PipeCommand([string]$json) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'sovereign-control', [System.IO.Pipes.PipeDirection]::InOut)
  try {
    $pipe.Connect(10000)
    $request = [System.Text.Encoding]::UTF8.GetBytes($json)
    $pipe.Write($request, 0, $request.Length)
    $pipe.Flush()
    $buffer = New-Object byte[] 65536
    $read = $pipe.Read($buffer, 0, $buffer.Length)
    return [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
  } finally {
    $pipe.Dispose()
  }
}

function Run-Setup([string]$file, [string]$arguments) {
  $process = Start-Process -FilePath $file -ArgumentList $arguments -PassThru -Wait
  return $process.ExitCode
}

function Check-Installed([string]$when) {
  Write-Host "== installed ($when)"
  foreach ($file in 'sovereign-core.exe', 'sovereign-gocore.dll', 'wintun.dll', 'libcronet.dll', 'sovereign-tray.exe', 'unins000.exe') {
    Expect (Test-Path (Join-Path $app $file)) "$file in Program Files"
  }
  $service = Get-Service SovereignCore -ErrorAction SilentlyContinue
  Expect ($null -ne $service) 'the service is registered'
  if ($service) {
    Expect ($service.StartType -eq 'Automatic') 'it starts with Windows'
    Expect ($service.Status -eq 'Running') 'it runs'
  }
  $pong = ''
  try { $pong = Send-PipeCommand '{"cmd":"box_ping"}' } catch { Write-Host "  pipe: $_" }
  Expect ($pong -match 'box_pong') "the core answers over the pipe ($pong)"
  $shortcut = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Sovereign.lnk'
  Expect (Test-Path $shortcut) 'a Start menu shortcut'
}

Write-Host "== install $Installer"
Expect ((Run-Setup $Installer '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=setup-install.log') -eq 0) 'setup exit code 0'
Check-Installed 'first install'

# What the tray's updater runs; the running service and tray must be stopped
# and brought back by the installer itself.
Write-Host "== update over a running install"
Expect ((Run-Setup $Installer '/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LOG=setup-update.log') -eq 0) 'setup exit code 0'
Check-Installed 'after the update'

Write-Host "== uninstall"
Expect ((Run-Setup (Join-Path $app 'unins000.exe') '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART') -eq 0) 'uninstaller exit code 0'
Start-Sleep -Seconds 3  # the uninstaller removes itself from a copy that is still exiting
Expect ($null -eq (Get-Service SovereignCore -ErrorAction SilentlyContinue)) 'the service is gone'
Expect (-not (Test-Path (Join-Path $app 'sovereign-core.exe'))) 'the files are gone'
Get-Process sovereign-tray -ErrorAction SilentlyContinue | Stop-Process -Force

if ($failures -gt 0) {
  Get-Content setup-install.log, setup-update.log -ErrorAction SilentlyContinue | Select-Object -Last 80
  throw "$failures check(s) failed"
}
Write-Host 'installer smoke: OK'
