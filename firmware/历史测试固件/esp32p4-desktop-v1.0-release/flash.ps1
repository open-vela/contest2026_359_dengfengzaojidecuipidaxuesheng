param(
    [string]$Port = "COM7",
    [int]$Baud = 921600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$image = Join-Path $PSScriptRoot "nuttx.bin"
$expected = "74420C66BE6A0298DBBEB21326600D47010612C27A097030DF4E47CC90E2C058"

if (-not (Test-Path -LiteralPath $image -PathType Leaf)) {
    throw "Firmware not found: $image"
}

$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $image).Hash
if ($actual -ne $expected) {
    throw "SHA256 mismatch. Expected $expected, got $actual"
}

& py -m esptool `
    --chip esp32p4 `
    --port $Port `
    --baud $Baud `
    --before default-reset `
    --after hard-reset `
    write-flash `
    --flash-size 4MB `
    --flash-mode dio `
    --flash-freq 80m `
    0x2000 $image

if ($LASTEXITCODE -ne 0) {
    throw "esptool failed with exit code $LASTEXITCODE"
}

Write-Host "Flash and verification completed. UART0 is 115200 baud."
