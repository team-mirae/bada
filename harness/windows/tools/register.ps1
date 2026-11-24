# Registers the built text service where it is (a development registration), or removes it. It
# needs an administrator: an unelevated shell is asked to elevate (UAC).
#   tools\register.ps1               regsvr32 build\x64\bada-tsf.dll
#   tools\register.ps1 -Unregister   regsvr32 /u, and nothing of bada is left in the registry
#   tools\register.ps1 -Status       what is registered now; changes nothing
#
# Registering writes two registry keys and nothing else:
#   HKLM\SOFTWARE\Classes\CLSID\{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}     the COM class and its DLL
#   HKLM\SOFTWARE\Microsoft\CTF\TIP\{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}  the TSF profile (Japanese)
#                                                                           and its categories
# It adds bada to nobody's keyboards, changes no language, and makes nothing the default: the user
# adds the keyboard themselves (README, "Use it"). Apps that were running load the DLL only after
# they restart, and keep it open until they end: tools\build.ps1 moves a DLL that is in use aside,
# and those apps keep the old one until they restart.
param(
    [ValidateSet('x64', 'x86')] [string] $Arch = 'x64',
    [switch] $Unregister,
    [switch] $Status
)
$ErrorActionPreference = 'Stop'
$clsid = '{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}'
$here = Split-Path $PSScriptRoot -Parent
$dll = Join-Path $here "build\$Arch\bada-tsf.dll"

function Show-Status {
    $class = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
    $tip = "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
    if (Test-Path $class) {
        Write-Host "registered: $class"
        Write-Host "  InprocServer32 = $((Get-ItemProperty "$class\InprocServer32").'(default)')"
    } else { Write-Host "not registered: $class" }
    if (Test-Path $tip) { Write-Host "registered: $tip" } else { Write-Host "not registered: $tip" }
}
if ($Status) { Show-Status; exit 0 }

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Arch', $Arch)
    if ($Unregister) { $argv += '-Unregister' }
    $p = Start-Process powershell -Verb RunAs -ArgumentList $argv -Wait -PassThru
    Show-Status
    exit $p.ExitCode
}

if (-not (Test-Path $dll)) { throw "Nothing built at ${dll}: run tools\build.ps1 first." }
$regsvr = if ($Arch -eq 'x86') { "$env:SystemRoot\SysWOW64\regsvr32.exe" } else { "$env:SystemRoot\System32\regsvr32.exe" }
$arguments = if ($Unregister) { @('/s', '/u', "`"$dll`"") } else { @('/s', "`"$dll`"") }
$p = Start-Process $regsvr -ArgumentList $arguments -Wait -PassThru
if ($p.ExitCode) { Write-Host "regsvr32 failed ($($p.ExitCode))"; exit $p.ExitCode }
Show-Status
exit 0
