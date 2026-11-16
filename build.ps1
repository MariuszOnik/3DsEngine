# Bakes the Morrowind cells (tools/mwbake.py) into romfs/ and builds mwcell3d.3dsx.
# Usage:
#   .\build.ps1                       bake (defaults: Seyda Neen, radius 2) + make
#   .\build.ps1 run                   ... then open in Azahar
#   .\build.ps1 -nobake               only make (romfs as it is)
#   -bake "--radius 0 --tex 64"       arguments for mwbake.py
#   -tag t1                           copy as builds\mwcell3d-<tag>.3dsx
param([string]$what = "", [string]$bake = "", [string]$tag = "test", [switch]$nobake)
$ErrorActionPreference = "Continue"   # gcc warnings on stderr are not errors
$here = $PSScriptRoot
$bash = "C:\devkitPro\msys2\usr\bin\bash.exe"
$unix = "/" + $here.Substring(0, 1).ToLower() + $here.Substring(2).Replace("\", "/")
$env_ = "export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM; cd '$unix'"
if ($what -eq "clean") {
    & $bash -lc "$env_ && make clean"
    exit $LASTEXITCODE
}
if (-not $nobake) {
    $bakeArgs = @()
    if ($bake) { $bakeArgs = $bake.Split(" ", [StringSplitOptions]::RemoveEmptyEntries) }
    python (Join-Path $here "tools\mwbake.py") @bakeArgs
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
# make does not see changes inside romfs/: drop the .3dsx so it is always packed anew
Remove-Item -ErrorAction SilentlyContinue (Join-Path $here "mwcell3d.3dsx"), (Join-Path $here "mwcell3d.smdh")
$title = "MW cell $tag " + (Get-Date -Format "MM-dd HH:mm")
& $bash -lc "$env_ && make APP_TITLE='$title' APP_DESCRIPTION='$title'"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
New-Item -ItemType Directory -Force (Join-Path $here "builds") | Out-Null
$copy = Join-Path $here "builds\mwcell3d-$tag.3dsx"
Copy-Item -Force (Join-Path $here "mwcell3d.3dsx") $copy
Write-Host "copy: $copy"
if ($what -eq "run") {
    $azahar = "D:\Programowanie\azahar-windows-mxe-2127.0-alpha3\azahar-windows-mxe-2127.0-alpha3\azahar.exe"
    Start-Process $azahar -ArgumentList "`"$copy`""
}
