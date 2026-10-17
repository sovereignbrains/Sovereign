# CPU per process over a window (needs admin for a service's): which of a
# client's processes keeps the CPU busy while nothing happens.
#   powershell -File cpu-split.ps1 -Names sovereign-tray, sovereign-core -Seconds 60
param([string[]]$Names, [int]$Seconds = 60)
$before = @{}
foreach ($p in Get-Process -Name $Names -ErrorAction SilentlyContinue) { $before[$p.Id] = $p.TotalProcessorTime.TotalMilliseconds }
Start-Sleep -Seconds $Seconds
foreach ($p in Get-Process -Name $Names -ErrorAction SilentlyContinue) {
  $used = $p.TotalProcessorTime.TotalMilliseconds - $(if ($before.ContainsKey($p.Id)) { $before[$p.Id] } else { 0 })
  '{0} pid={1}: {2:N2}% of one core, wakeups ~ threads={3}' -f $p.ProcessName, $p.Id, (100 * $used / ($Seconds * 1000)), $p.Threads.Count
}
