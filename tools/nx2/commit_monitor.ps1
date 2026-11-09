# Samples the machine once a second for 9 minutes into a CSV: Windows commit, free RAM, lichii's
# memory, total and lichii CPU use, and GPU use (the 3D engine, for lichii and for everything).
# Run it before starting the emulator:
#   pwsh -NoProfile -File tools/nx2/commit_monitor.ps1 -out $env:TEMP\monitor.csv
# Reading it:
#  - commit reaching its limit, or a "low virtual memory" event 2004 in the System log when a log
#    ends, means memory exhaustion rather than a GPU fault;
#  - lichii_gpu3d_pct near 100 with lichii_cpu_pct well under 100 * cores means GPU-bound;
#  - the reverse (one core of lichii pegged, the GPU idle) means the guest CPU emulation is the limit.
param($out)
"time,commit_used_gb,commit_limit_gb,phys_free_gb,lichii_private_gb,lichii_ws_gb,cpu_total_pct,lichii_cpu_pct,gpu3d_total_pct,lichii_gpu3d_pct" | Set-Content $out
$cores = [Environment]::ProcessorCount
$end = (Get-Date).AddMinutes(9)
$prev = $null
while ((Get-Date) -lt $end) {
    $p = Get-Process lichii -ErrorAction SilentlyContinue | Select-Object -First 1
    $now = Get-Date
    $c = Get-Counter '\Memory\Committed Bytes', '\Memory\Commit Limit', '\Memory\Available Bytes', '\Processor(_Total)\% Processor Time' -ErrorAction SilentlyContinue
    $v = $c.CounterSamples | ForEach-Object { $_.CookedValue }

    $gpuAll = 0.0
    $gpuLichii = 0.0
    try {
        $g = Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage' -ErrorAction Stop
        foreach ($s in $g.CounterSamples) {
            $gpuAll += $s.CookedValue
            if ($p -and $s.InstanceName -like "pid_$($p.Id)_*") { $gpuLichii += $s.CookedValue }
        }
    } catch { }

    $cpuLichii = ''
    if ($p) {
        $t = $p.TotalProcessorTime.TotalSeconds
        if ($prev -and $prev.id -eq $p.Id) {
            $dt = ($now - $prev.time).TotalSeconds
            if ($dt -gt 0) { $cpuLichii = '{0:N0}' -f (100.0 * ($t - $prev.cpu) / $dt) }
        }
        $prev = @{ id = $p.Id; cpu = $t; time = $now }
        $mem = '{0:N2},{1:N2}' -f ($p.PrivateMemorySize64 / 1GB), ($p.WorkingSet64 / 1GB)
    } else {
        $prev = $null
        $mem = ','
    }
    '{0},{1:N2},{2:N2},{3:N2},{4},{5:N0},{6},{7:N0},{8:N0}' -f (Get-Date -Format HH:mm:ss.f), ($v[0] / 1GB), ($v[1] / 1GB), ($v[2] / 1GB), $mem, $v[3], $cpuLichii, [math]::Min($gpuAll, 100), [math]::Min($gpuLichii, 100) | Add-Content $out
}
