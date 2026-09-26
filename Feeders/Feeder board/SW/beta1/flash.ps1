<#
  .SYNOPSIS
  Flash beta1 firmware to the ATmega328PB feeder board via ISP (USBasp).

  .DESCRIPTION
  No bootloader on this board (ISP-only) - see project.md "PlatformIO".
  Sequence: build -> burn fuses -> chip erase -> short settle delay ->
  write + verify flash.

  Erase and flash go straight through avrdude (not `pio -t erase`/
  `pio -t upload`), matching the actual commands used by hand on this
  board's bench:

    avrdude -C <conf> -c usbasp -p m328pb -e
    avrdude -C <conf> -c usbasp -p m328pb -D -U flash:w:<hex>:i -U flash:v:<hex>:i

  `-e` is a standalone chip erase; `-D` on the flash step disables
  avrdude's own implicit auto-erase (redundant after the explicit erase
  above, and skipping it avoids a second erase cycle); the second `-U`
  reads back and verifies against the same hex after writing. Building
  through `pio run` first (no upload) ensures firmware.hex is current
  before avrdude writes it directly - avrdude has no idea whether the
  hex is stale.

  Why the settle delay between erase and flash: a fuse write is followed
  by an avrdude-issued target reset, and fuse bits that affect the clock
  (CKDIV8/oscillator selection - this board runs on the internal 8MHz RC
  oscillator, no crystal) only take effect from that reset onward.
  Hitting the target with another ISP transaction immediately after can
  race that reset and cause sync errors.

  Fuses still go through `pio run -t fuses`, not a raw avrdude command -
  no hand-run avrdude fuse invocation for this board has turned up yet.
  Swap this step for a raw avrdude -U lfuse:w:...:m/-U hfuse:.../-U
  efuse:... call if/when one does, to match the erase/flash steps below.

  .PARAMETER SkipFuses
  Skip the fuse-burn step (use once fuses are already correctly set on
  this chip - routine firmware-only reflashes).

  .PARAMETER SkipErase
  Skip the chip-erase step (falls back to avrdude's implicit erase on
  write - drop -D from the flash command below too if you do this).

  .PARAMETER SettleSeconds
  Seconds to wait after erase (and after fuses, if burned) before the
  next ISP operation. Default 2.

  .EXAMPLE
  ./flash.ps1                      # full sequence: build, fuses, erase, wait, flash+verify
  .EXAMPLE
  ./flash.ps1 -SkipFuses           # routine reflash, fuses already set
#>

param(
  [switch]$SkipFuses,
  [switch]$SkipErase,
  [int]$SettleSeconds = 2
)

$ErrorActionPreference = "Stop"
$EnvName = "atmega328pb_isp"
$PlatformioRoot = Join-Path $env:USERPROFILE ".platformio"
$Avrdude = Join-Path $PlatformioRoot "packages\tool-avrdude\avrdude.exe"
$AvrdudeConf = Join-Path $PlatformioRoot "packages\tool-avrdude\avrdude.conf"
$HexPath = ".pio/build/$EnvName/firmware.hex"

function Resolve-Pio {
  $cmd = Get-Command pio -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

  $fallback = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
  if (Test-Path $fallback) { return $fallback }

  throw "Could not find 'pio' on PATH or at $fallback - install PlatformIO or fix the fallback path in this script."
}

if (-not (Test-Path $Avrdude)) { throw "avrdude not found at $Avrdude - fix the path in this script." }
if (-not (Test-Path $AvrdudeConf)) { throw "avrdude.conf not found at $AvrdudeConf - fix the path in this script." }

$Pio = Resolve-Pio
Write-Host "Using PlatformIO: $Pio" -ForegroundColor DarkGray
Write-Host "Using avrdude: $Avrdude" -ForegroundColor DarkGray

Write-Host "== Building ($EnvName) ==" -ForegroundColor Cyan
& $Pio run -e $EnvName
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

if (-not $SkipFuses) {
  Write-Host "== Burning fuses ($EnvName) ==" -ForegroundColor Cyan
  & $Pio run -e $EnvName -t fuses
  if ($LASTEXITCODE -ne 0) { throw "Fuse burn failed (exit $LASTEXITCODE)" }

  Write-Host "Waiting $SettleSeconds s for the target to settle under the new fuse config..." -ForegroundColor DarkGray
  Start-Sleep -Seconds $SettleSeconds
} else {
  Write-Host "== Skipping fuse burn (-SkipFuses) ==" -ForegroundColor DarkGray
}

if (-not $SkipErase) {
  Write-Host "== Chip erase ==" -ForegroundColor Cyan
  & $Avrdude -C $AvrdudeConf -c usbasp -p m328pb -e
  if ($LASTEXITCODE -ne 0) { throw "Chip erase failed (exit $LASTEXITCODE)" }

  Write-Host "Waiting $SettleSeconds s before flashing..." -ForegroundColor DarkGray
  Start-Sleep -Seconds $SettleSeconds
} else {
  Write-Host "== Skipping erase (-SkipErase) ==" -ForegroundColor DarkGray
}

Write-Host "== Writing + verifying flash ==" -ForegroundColor Cyan
$flashArg = "flash:w:${HexPath}:i"
$verifyArg = "flash:v:${HexPath}:i"
if ($SkipErase) {
  & $Avrdude -C $AvrdudeConf -c usbasp -p m328pb -U $flashArg -U $verifyArg
} else {
  & $Avrdude -C $AvrdudeConf -c usbasp -p m328pb -D -U $flashArg -U $verifyArg
}
if ($LASTEXITCODE -ne 0) { throw "Flash write/verify failed (exit $LASTEXITCODE)" }

Write-Host "Done." -ForegroundColor Green
