# Compares two builds of the network benchmark on a machine that is not quiet.
#
# It runs `tessera_net_bench network` from each build one after the other, several times, so that whatever the
# machine is doing affects both builds about equally, and prints the median and the range of the phase time of
# each, and the difference. Build the baseline first and keep a copy of its executable:
#
#   ./tools/bench_compare.ps1 -Baseline C:\copies\bench-before.exe -Candidate .\build\windows-msvc\bench\Release\tessera_net_bench.exe
#
# The times are only comparable between the two builds of one run of this script.
param(
    [Parameter(Mandatory = $true)][string]$Baseline,
    [Parameter(Mandatory = $true)][string]$Candidate,
    [int]$Runs = 4
)

$ErrorActionPreference = 'Stop'

function Get-PhaseTimes([string]$Executable) {
    $text = (& $Executable network 2>&1 | Out-String)
    $one = [regex]::Match($text, '1 worker: network phase ([\d.]+) ms')
    $eight = [regex]::Match($text, '8 workers: network phase ([\d.]+) ms')
    if (-not $one.Success -or -not $eight.Success) {
        throw "Could not read the phase times from the output of $Executable"
    }
    return @([double]$one.Groups[1].Value, [double]$eight.Groups[1].Value)
}

function Get-Median([double[]]$Values) {
    $sorted = $Values | Sort-Object
    $middle = [int][math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2 -eq 1) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2
}

$before = @{ one = @(); eight = @() }
$after = @{ one = @(); eight = @() }
for ($run = 1; $run -le $Runs; $run++) {
    $times = Get-PhaseTimes $Baseline
    $before.one += $times[0]; $before.eight += $times[1]
    $times = Get-PhaseTimes $Candidate
    $after.one += $times[0]; $after.eight += $times[1]
}

function Show([string]$Label, [double[]]$Before, [double[]]$After) {
    $b = Get-Median $Before
    $a = Get-Median $After
    $bRange = $Before | Measure-Object -Minimum -Maximum
    $aRange = $After | Measure-Object -Minimum -Maximum
    '{0}: baseline {1:N1} ms ({2:N1} to {3:N1}), candidate {4:N1} ms ({5:N1} to {6:N1}), difference {7:+0.0;-0.0} ms' -f `
        $Label, $b, $bRange.Minimum, $bRange.Maximum, $a, $aRange.Minimum, $aRange.Maximum, ($a - $b)
}

Show 'network phase, 1 thread' $before.one $after.one
Show 'network phase, 8 workers' $before.eight $after.eight
