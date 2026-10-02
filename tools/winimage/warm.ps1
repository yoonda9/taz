# Warms the TAZ layer's C:\taz checkout: installs the mise tools, runs CI's
# Windows recipes once so every cache is filled, and prints `FACTS {json}`
# with the timings. Exits non-zero if a recipe fails or the tree is dirty.
$ErrorActionPreference = 'Continue'
$env:CI = '1'
$env:UV_LOCKED = '1'
Set-Location C:\taz

$timings = [ordered]@{}
$failed = @()
function Step([string]$name, [scriptblock]$block) {
    $start = Get-Date
    & $block 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $timings[$name] = [int]((Get-Date) - $start).TotalSeconds
    Write-Output "=== $name exit=$code secs=$($timings[$name])"
    if ($code -ne 0) { $script:failed += $name }
}

Step 'mise install' { mise trust C:\taz\mise.toml; mise install }
Step 'just setup' { just setup }
Step 'just doctor' { just doctor }
Step 'just deps' { just deps }
Step 'just deps windows-asan' { just deps windows-asan }
Step 'just build' { just build }
$env:TAZ_DAEMON = 'daemon/build/windows-debug/tazd.exe'
Step 'just test' { just test }
Step 'just lint' { just lint }

$dirty = @(git status --short).Count
$facts = [ordered]@{
    commit = (git log -1 --format='%h %s')
    dirty = $dirty
    failed = $failed
    timings = $timings
}
Write-Output ('FACTS ' + ($facts | ConvertTo-Json -Compress))
if ($failed.Count -or $dirty) { exit 1 }
