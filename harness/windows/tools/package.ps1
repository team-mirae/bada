# Builds the installer a user downloads: build\dist\bada-<version>-setup.exe (installer\bada.iss,
# Inno Setup 6).
#   tools\package.ps1              build x64 and x86 (tools\build.ps1), then package
#   tools\package.ps1 -SkipBuild   package what is in build\ already
# Signing (off by default): set BADA_SIGN_CERT to a .pfx file, and BADA_SIGN_PASSWORD if it has
# one, and the DLLs, the installer and its uninstaller are signed with signtool (Windows SDK).
# BADA_SIGN_TIMESTAMP overrides the timestamp server.
param([switch] $SkipBuild)
$ErrorActionPreference = 'Stop'
$here = Split-Path $PSScriptRoot -Parent
$root = Split-Path (Split-Path $here -Parent) -Parent
$build = Join-Path $here 'build'

if (-not $SkipBuild) {
    foreach ($arch in 'x64', 'x86') {
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\build.ps1" -Arch $arch
        if ($LASTEXITCODE) { throw "the $arch build failed" }
    }
}
$dlls = foreach ($arch in 'x64', 'x86') { "$build\$arch\bada-tsf.dll" }
foreach ($dll in $dlls) { if (-not (Test-Path $dll)) { throw "$dll is missing: run tools\build.ps1 -Arch x64 and -Arch x86." } }
# The engine's data, which either build copies beside the two.
if (-not (Test-Path "$build\data\kanji.bin")) { throw "$build\data is missing: run tools\build.ps1." }

# One version for the whole repository.
$version = (Get-Content "$root\VERSION" -Raw).Trim()

$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
foreach ($dir in "$env:LOCALAPPDATA\Programs\Inno Setup 6", "${env:ProgramFiles(x86)}\Inno Setup 6", "$env:ProgramFiles\Inno Setup 6") {
    if (-not $iscc -and (Test-Path "$dir\ISCC.exe")) { $iscc = "$dir\ISCC.exe" }
}
if (-not $iscc) { throw 'ISCC.exe not found: install Inno Setup 6 (https://jrsoftware.org/isinfo.php).' }

$argv = @("/DVersion=$version", "/DBuild=$build")
if ($env:BADA_SIGN_CERT) {
    $signtool = (Get-Command signtool -ErrorAction SilentlyContinue).Source
    if (-not $signtool) {
        $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
    }
    if (-not $signtool) { throw 'signtool.exe not found: install the Windows SDK.' }
    $timestamp = if ($env:BADA_SIGN_TIMESTAMP) { $env:BADA_SIGN_TIMESTAMP } else { 'http://timestamp.digicert.com' }
    $sign = @('sign', '/fd', 'sha256', '/tr', $timestamp, '/td', 'sha256', '/f', $env:BADA_SIGN_CERT)
    if ($env:BADA_SIGN_PASSWORD) { $sign += @('/p', $env:BADA_SIGN_PASSWORD) }
    & $signtool @sign @dlls
    if ($LASTEXITCODE) { throw 'signing the DLLs failed' }
    # Inno Setup runs this for the installer and the uninstaller; $f is the file.
    $quoted = ($sign | ForEach-Object { if ($_ -match '\s') { "`$q$_`$q" } else { $_ } }) -join ' '
    $argv += @('/DSign', "/Sbada=`$q$signtool`$q $quoted `$f")
}

& $iscc @argv "$here\installer\bada.iss"
if ($LASTEXITCODE) { throw "ISCC failed ($LASTEXITCODE)" }
Write-Host "Built $build\dist\bada-$version-setup.exe"
exit 0
