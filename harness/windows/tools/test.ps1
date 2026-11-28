# Runs the tests of a build (tools\build.ps1). None reads or writes the user's own settings: each
# has a folder of its own in the temporary folder.
#   tools\test.ps1                the key-table cases, the transcripts, the core, the state, and the
#                                 text service loaded from its DLL in a TSF thread of the test's own
#   tools\test.ps1 -Registered    also the registered text service (tools\register.ps1 first)
#   tools\test.ps1 -InSession     from a logon that isn't interactive (SSH): TSF works only in an
#                                 interactive one, so the text service test is run in the session of
#                                 the user logged on at the machine, through a scheduled task (an
#                                 interactive logon type, so no password), on a desktop of its own
#                                 that is never shown. Nothing appears on the user's screen.
param(
    [ValidateSet('x64', 'x86')] [string] $Arch = 'x64',
    [switch] $Registered,
    [switch] $InSession
)
$ErrorActionPreference = 'Continue'
$here = Split-Path $PSScriptRoot -Parent
$root = Split-Path (Split-Path $here -Parent) -Parent
$build = Join-Path $here "build\$Arch"
if (-not (Test-Path "$build\bada-tsf.dll")) { Write-Host "Nothing built in ${build}: run tools\build.ps1 first."; exit 2 }

$failed = @()
function Run($name, $exe, $arguments) {
    Write-Host "== $name"
    & $exe @arguments
    if ($LASTEXITCODE) { $script:failed += $name }
}

# The text service test in the interactive user's session, with a scheduled task. The task runs the
# exe as that user with an interactive logon type (no password), waits for it, and reads back what
# it wrote to a file. The task is removed afterwards. The user is the one logged on at the console.
function RunInSession($name, $hostArguments) {
    Write-Host "== $name (in the interactive session)"
    $user = (Get-CimInstance Win32_ComputerSystem).UserName
    if (-not $user) { Write-Host 'FAIL: nobody is logged on at the machine, so TSF can''t be reached'; $script:failed += $name; return }
    $out = Join-Path $env:TEMP ("bada-tsf-host-{0}.txt" -f [guid]::NewGuid())
    Remove-Item $out -ErrorAction SilentlyContinue
    $exe = "$build\bada_tsf_host.exe"
    # cmd wraps the exe so its console output is captured even on a desktop with no console.
    $inner = (@("`"$exe`"") + $hostArguments + @('--own-desktop')) -join ' '
    $argline = "/c `"$inner > `"$out`" 2>&1`""
    $task = 'bada-tsf-host'
    try {
        $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument $argline -WorkingDirectory $build
        $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
        Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings -Force | Out-Null
        Start-ScheduledTask -TaskName $task
        $deadline = (Get-Date).AddMinutes(4)
        do {
            Start-Sleep -Milliseconds 400
            $info = Get-ScheduledTaskInfo -TaskName $task
            $state = (Get-ScheduledTask -TaskName $task).State
        } while ($state -eq 'Running' -and (Get-Date) -lt $deadline)
        if (Test-Path $out) { Get-Content $out -Encoding utf8 | ForEach-Object { Write-Host $_ } }
        else { Write-Host 'FAIL: the task wrote no output'; $script:failed += $name; return }
        if ($info.LastTaskResult -ne 0) { Write-Host "FAIL: the task's result was $($info.LastTaskResult)"; $script:failed += $name }
    } finally {
        Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
        Remove-Item $out -ErrorAction SilentlyContinue
    }
}

Run 'key table' "$build\bada_keytable_cases.exe" @("$root\docs\cases\key-table.json")
Run 'transcripts' "$build\bada_transcript.exe" @("$root\tools\conformance\transcripts\any-engine.jsonl",
                                                 "$root\tools\conformance\transcripts\bada.jsonl")
Run 'core' "$build\bada_core_tests.exe" @()
Run 'state' "$build\bada_state_tests.exe" @()
Run 'tide' "$build\bada_tide_tests.exe" @()
$hostArguments = if ($Registered) { @('--registered') } else { @('--dll', "`"$build\bada-tsf.dll`"") }
if ($InSession) {
    RunInSession 'text service' $hostArguments
} else {
    Run 'text service' "$build\bada_tsf_host.exe" ($hostArguments -replace '"', '')
}

if ($failed.Count) { Write-Host "FAILED: $($failed -join ', ')"; exit 1 }
Write-Host 'all passed'
exit 0
