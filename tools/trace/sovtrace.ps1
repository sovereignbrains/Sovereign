<#
.SYNOPSIS
  Record and read the service's ETW events (provider Sovereign.Core, see src/service/trace.h).

.DESCRIPTION
  start   Starts an ad-hoc ETW session writing Sovereign.Core to an .etl file (needs admin).
  stop    Stops it (the file is complete only after this).
  dump    Prints the events of an .etl file, one per line, oldest first. With -AsObject
          it returns objects (Time, Level, Event, Keywords, ProcessId, Fields) for scripts.

  Decoding goes through tracerpt's XML output: it understands TraceLogging's
  self-describing events; Get-WinEvent reads the file but drops their fields.

  Keywords (trace.h): 0x1 lifecycle, 0x2 control commands, 0x4 power, 0x8 sing-box
  warn/error lines, 0x10 sing-box info/debug lines (name every domain - off unless
  asked for), 0x20 failures reported by WIL. "default" = 0x2F, "all" = everything.

.EXAMPLE
  pwsh tools/trace/sovtrace.ps1 start -Path C:\temp\sov.etl -Keywords all
  pwsh tools/trace/sovtrace.ps1 stop
  pwsh tools/trace/sovtrace.ps1 dump -Path C:\temp\sov.etl
#>
param(
  [Parameter(Mandatory, Position = 0)][ValidateSet('start', 'stop', 'dump')][string]$Action,
  [string]$Path,
  [string]$Name = 'Sovereign-AdHoc',
  [string]$Keywords = 'default',
  [ValidateRange(1, 5)][int]$Level = 5,
  [switch]$AsObject
)
$ErrorActionPreference = 'Stop'
$provider = '{356e995a-3c2d-5ae3-fad1-410ee24d609d}'   # Sovereign.Core (name hash)
$levelNames = @{ 1 = 'critical'; 2 = 'error'; 3 = 'warning'; 4 = 'info'; 5 = 'verbose' }

function Invoke-Native([string]$exe, [string[]]$arguments) {
  $output = & $exe @arguments 2>&1
  if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed ($LASTEXITCODE): $($output -join ' ')" }
}

switch ($Action) {
  'start' {
    if (-not $Path) { throw 'start needs -Path <file.etl>' }
    $mask = switch ($Keywords) { 'default' { '0x2f' } 'all' { '0xffffffffffffffff' } default { $Keywords } }
    New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Path))) | Out-Null
    Invoke-Native logman @('start', $Name, '-p', $provider, $mask, "$Level", '-o', $Path, '-ets')
  }
  'stop' {
    Invoke-Native logman @('stop', $Name, '-ets')
  }
  'dump' {
    if (-not $Path) { throw 'dump needs -Path <file.etl>' }
    $xmlPath = Join-Path ([IO.Path]::GetTempPath()) ("sovtrace-{0}.xml" -f [guid]::NewGuid())
    try {
      Invoke-Native tracerpt @($Path, '-of', 'XML', '-lr', '-o', $xmlPath, '-y')
      $xml = [xml](Get-Content $xmlPath -Raw -Encoding utf8)
      $events = foreach ($e in $xml.Events.Event) {
        if ($e.System.Provider.Guid -ne $provider) { continue }   # the session header, other providers
        $fields = [ordered]@{}
        foreach ($d in @($e.EventData.Data)) { if ($d) { $fields[$d.Name] = $d.InnerText } }
        # tracerpt writes local clock time with a wrong offset (+02:59 on a UTC+3
        # machine, 26.09.2026), so applying the offset shifts events by a minute.
        # The clock part is right: take it as local time.
        $clock = $e.System.TimeCreated.SystemTime -replace '(Z|[+-]\d\d:\d\d)$', ''
        [pscustomobject]@{
          Time      = [datetime]::Parse($clock, [cultureinfo]::InvariantCulture)
          Level     = $levelNames[[int]$e.System.Level]
          Event     = $e.RenderingInfo.Task
          Keywords  = $e.System.Keywords
          ProcessId = [int]$e.System.Execution.ProcessID
          Fields    = $fields
        }
      }
      if ($AsObject) { return $events }
      foreach ($ev in $events) {
        $text = ($ev.Fields.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '
        '{0:yyyy-MM-dd HH:mm:ss.fff} {1,-8} {2,-7} {3,-16} {4}' -f $ev.Time, $ev.Level, $ev.ProcessId, $ev.Event, $text
      }
    } finally {
      Remove-Item $xmlPath -ErrorAction SilentlyContinue
    }
  }
}
