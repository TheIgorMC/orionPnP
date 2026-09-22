<#
  .SYNOPSIS
  Flash beta1 firmware to the ATmega328PB feeder board via ISP (USBasp).

  .DESCRIPTION
  No bootloader on this board (ISP-only) - see project.md "PlatformIO".
  Sequence: burn fuses -> (best-effort) chip erase -> short settle delay
  -> upload firmware.

  Why the settle delay: a fuse write is followed by an avrdude-issued
  target reset, and the new fuse bits (in particular CKDIV8/oscillator
  selection - this board runs on the internal 8MHz RC oscillator, no
  crystal) only take effect from that reset onward. Hitting the target
  with another ISP transaction immediately after can race that reset and
  cause sync errors - the delay just gives it a moment to actually come
  up under the new clock config first.

  Why the erase step is best-effort, not required: avrdude already
  performs a full chip erase implicitly as part of any AVR flash write
  (default behavior, unless told not to), so a separate erase ahead of
  *uploading firmware* is usually redundant. Whether PlatformIO's
  atmelavr platform exposes a bare `-t erase` target at all wasn't
  verified against the actual toolchain when this script was written -
  if it errors with something like "Unknown target 'erase'", that's
  expected on some PlatformIO versions; the script logs a warning and
  continues rather than aborting the whole flash over it. Use -SkipErase
  to skip attempting it entirely once you know whether your setup has it.

  Only the fuses target actually needs to run every time the board's
  clock/BOD/EESAVE config changes (board_hardware.* in platformio.ini) -
  not on every firmware iteration. Use -SkipFuses for routine reflashes
  once fuses are already correct on this specific chip.

  .PARAMETER SkipFuses
  Skip the fuse-burn step (use once fuses are already correctly set on
  this chip - routine firmware-only reflashes).

  .PARAMETER SkipErase
  Skip the best-effort chip-erase step entirely.

  .PARAMETER SettleSeconds
  Seconds to wait after burning fuses before the next ISP operation.
  Default 2.

  .EXAMPLE
  ./flash.ps1                      # full sequence: fuses, erase, wait, upload
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

function Resolve-Pio {
  $cmd = Get-Command pio -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

  $fallback = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
  if (Test-Path $fallback) { return $fallback }

  throw "Could not find 'pio' on PATH or at $fallback - install PlatformIO or fix the fallback path in this script."
}

$Pio = Resolve-Pio
Write-Host "Using PlatformIO: $Pio" -ForegroundColor DarkGray

if (-not $SkipFuses) {
  Write-Host "== Burning fuses ($EnvName) ==" -ForegroundColor Cyan
  & $Pio run -e $EnvName -t fuses
  if ($LASTEXITCODE -ne 0) { throw "Fuse burn failed (exit $LASTEXITCODE)" }
} else {
  Write-Host "== Skipping fuse burn (-SkipFuses) ==" -ForegroundColor DarkGray
}

if (-not $SkipErase) {
  Write-Host "== Chip erase (best-effort) ==" -ForegroundColor Cyan
  & $Pio run -e $EnvName -t erase
  if ($LASTEXITCODE -ne 0) {
    Write-Warning "Erase target failed or doesn't exist on this PlatformIO setup (exit $LASTEXITCODE) - continuing anyway, since avrdude erases implicitly on upload."
  }
} else {
  Write-Host "== Skipping erase (-SkipErase) ==" -ForegroundColor DarkGray
}

if (-not $SkipFuses) {
  Write-Host "Waiting $SettleSeconds s for the target to settle under the new fuse config..." -ForegroundColor DarkGray
  Start-Sleep -Seconds $SettleSeconds
}

Write-Host "== Uploading firmware ==" -ForegroundColor Cyan
& $Pio run -e $EnvName -t upload
if ($LASTEXITCODE -ne 0) { throw "Upload failed (exit $LASTEXITCODE)" }

Write-Host "Done." -ForegroundColor Green
