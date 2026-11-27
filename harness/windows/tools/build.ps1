# Builds the engine (cargo, the static CRT) and the harness (CMake, Ninja, MSVC): the text service
# bada-tsf.dll and the tests, into harness\windows\build\<arch>. Warnings are errors.
#   tools\build.ps1              x64
#   tools\build.ps1 -Arch x86    for 32-bit apps (needs: rustup target add i686-pc-windows-msvc)
#   tools\build.ps1 -Clean       from nothing
param(
    [ValidateSet('x64', 'x86')] [string] $Arch = 'x64',
    [switch] $Clean
)
$ErrorActionPreference = 'Stop'
$here = Split-Path $PSScriptRoot -Parent
$build = Join-Path $here "build\$Arch"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the C++ tools (Build Tools 2022 is enough).' }
# cargo is not on every shell's PATH (an SSH session's, say).
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { $env:Path = "$env:USERPROFILE\.cargo\bin;$env:Path" }
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { throw 'cargo not found: install Rust (rustup) first.' }

# That Visual Studio's developer environment, for this architecture: cl, link, rc, ninja.
$vsArch = @{ x64 = 'amd64'; x86 = 'x86' }[$Arch]
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=$vsArch -host_arch=amd64" | Out-Null

if ($Clean) { Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue }
# cargo and cmake write their progress to stderr, which must not stop the script.
$ErrorActionPreference = 'Continue'
# A registered DLL is open in every app that loaded it, so the linker can't write over it. Windows
# lets an open file be renamed: it is moved aside, and a later build removes it once no app has it.
# Apps that were running keep the old one until they restart.
# bada-tide's helper (bada-tide.exe) is the same while it runs.
foreach ($name in 'bada-tsf.dll', 'bada-tide.exe') {
    $base = [IO.Path]::GetFileNameWithoutExtension($name)
    $ext = [IO.Path]::GetExtension($name)
    Get-ChildItem $build -Filter "$base.old-*$ext" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    $file = Join-Path $build $name
    if (Test-Path $file) {
        try { [IO.File]::Open($file, 'Open', 'ReadWrite', 'None').Close() }
        catch { Rename-Item $file ('{0}.old-{1}{2}' -f $base, (Get-Date -Format 'yyyyMMddHHmmss'), $ext) }
    }
}
cmake -S $here -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "Built $build\bada-tsf.dll"
exit 0
