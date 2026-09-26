# build_headless.ps1 - compile + run the headless Pono UI renderer (MinGW).
#
# Builds LVGL once into a cached static lib, then compiles the pono builders +
# harness and links pono-headless.exe, runs it to render a screen to BMP, and
# converts the BMP to PNG (System.Drawing) for inspection.
#
#   pwsh sim/build_headless.ps1 [-Screen home] [-AdvanceMs 700] [-Rebuild]
#
param(
  [string]$Screen = 'home',
  [int]$AdvanceMs = 700,
  [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'

$bin  = 'C:\Strawberry\c\bin'
$gcc  = Join-Path $bin 'gcc.exe'
$gpp  = Join-Path $bin 'g++.exe'
$ar   = Join-Path $bin 'ar.exe'
# The repo root is this script's parent directory, wherever the checkout lives.
$root = Split-Path -Parent $PSScriptRoot
$sim  = Join-Path $root 'sim'
$obj  = Join-Path $sim '_obj'
New-Item -ItemType Directory -Force $obj | Out-Null

$inc = @("-I$root", "-I$root\src", "-I$root\lvgl", '-DLV_CONF_INCLUDE_SIMPLE')
$cflags   = @('-std=c11',   '-O2', '-Wno-unused-parameter', '-Wno-unused-variable')
$cxxflags = @('-std=c++17', '-O2', '-Wno-unused-parameter')

# 1) LVGL core -> liblvgl.a (one-time, cached)
$lib = Join-Path $obj 'liblvgl.a'
if ($Rebuild -and (Test-Path $lib)) { Remove-Item $lib -Force }
if (-not (Test-Path $lib)) {
  Write-Host '[1/3] Compiling LVGL core (one-time, ~1-2 min)...'
  # extra\libs (fsdrv/png/gif/...) pull in POSIX/3rd-party deps MinGW lacks and
  # are unused by the cockpit; keep extra\themes + extra\layouts (flex/grid).
  $lvglsrc = Get-ChildItem "$root\lvgl\src" -Recurse -Filter *.c |
    Where-Object { $_.FullName -notmatch '\\extra\\libs\\' } |
    ForEach-Object { $_.FullName }
  Push-Location $obj
  try {
    & $gcc -c @cflags @inc $lvglsrc
    if ($LASTEXITCODE -ne 0) { throw "LVGL compile failed ($LASTEXITCODE)" }
    $os = Get-ChildItem "$obj\*.o" | ForEach-Object { $_.FullName }
    & $ar rcs $lib $os
    if ($LASTEXITCODE -ne 0) { throw "ar failed ($LASTEXITCODE)" }
  } finally { Pop-Location }
  Write-Host "      liblvgl.a ready ($((Get-ChildItem $lib).Length) bytes)"
} else {
  Write-Host '[1/3] liblvgl.a cached (use -Rebuild to force)'
}

# 2) Pono fonts (C, designated initializers -> must use gcc not g++)
Write-Host '[2/3] Compiling Pono fonts...'
$fontobj = @()
foreach ($f in (Get-ChildItem "$root\assets\pono\fonts\*.c")) {
  $o = Join-Path $obj ("font_" + $f.BaseName + ".o")
  & $gcc -c @cflags @inc $f.FullName -o $o
  if ($LASTEXITCODE -ne 0) { throw "font $($f.Name) failed" }
  $fontobj += $o
}

# 2b) Pono canned-animation frames (C, designated initializers -> gcc)
Write-Host '[2b/3] Compiling Pono anim frames...'
$animobj = @()
if (Test-Path "$root\assets\pono\anim") {
  foreach ($f in (Get-ChildItem "$root\assets\pono\anim\*.c" -ErrorAction SilentlyContinue)) {
    $o = Join-Path $obj ("anim_" + $f.BaseName + ".o")
    & $gcc -c @cflags @inc $f.FullName -o $o
    if ($LASTEXITCODE -ne 0) { throw "anim $($f.Name) failed" }
    $animobj += $o
  }
}

# 3) builders + harness -> exe
Write-Host '[3/3] Compiling + linking harness...'
$exe = Join-Path $sim 'pono-headless.exe'
& $gpp @cxxflags @inc `
  (Join-Path $sim 'pono_headless.cpp') `
  (Join-Path $root 'src\pono_theme.cpp') `
  (Join-Path $root 'src\pono_home.cpp') `
  (Join-Path $root 'src\pono_anim.cpp') `
  (Join-Path $root 'src\prompt_layout.cpp') `
  $fontobj $animobj $lib -o $exe -lm
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
Write-Host "      built $exe"

# render
$bmp = Join-Path $sim "$Screen.bmp"
$png = Join-Path $sim "$Screen.png"
& $exe $bmp $AdvanceMs $Screen
$rc = $LASTEXITCODE
# Exit 3 means the render is fine but something sits under the E-STOP; keep
# the picture so the overlap can be seen, then fail.
if ($rc -ne 0 -and $rc -ne 3) { throw "render failed ($rc)" }

Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile($bmp)
$img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$img.Dispose()
if ($rc -eq 3) { throw "something sits under the E-STOP, see $png" }
Write-Host "RENDER OK -> $png"
