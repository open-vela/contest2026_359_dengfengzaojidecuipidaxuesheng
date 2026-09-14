# Flash an ESP32-P4 Simple Boot image using esptool 5.x.
# Usage (PowerShell):
#   powershell -File tools\flash_p4_nsh.ps1 -Variant v1.x -Port COM7 -Transport uart-bridge
#   powershell -File tools\flash_p4_nsh.ps1 -Port COM23 -Transport usb-jtag -Firmware path\nuttx.bin -DryRun
#
# Select the physical download interface, not the firmware console setting.

param(
    [ValidateSet("selected", "v1.x", "v3.2", "v3.2-usb")]
    [string]$Variant = "selected",
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [Parameter(Mandatory = $true)]
    [ValidateSet("usb-jtag", "uart-bridge")]
    [string]$Transport,
    [string]$Firmware,
    [string]$Python = "python",
    [ValidateRange(9600, 3000000)]
    [int]$Baud = 460800,
    [switch]$ProbeOnly,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$FirmwareRoot = Join-Path $Repo "firmware\esp32p4-nsh"
if ($Firmware) {
    $Fw = $Firmware
} elseif ($Variant -eq "selected") {
    $Fw = Join-Path $FirmwareRoot "nuttx.bin"
} else {
    $Fw = Join-Path $FirmwareRoot "$Variant\esp32p4-nsh-$Variant.bin"
}
if (-not $ProbeOnly) {
    if (-not (Test-Path -LiteralPath $Fw -PathType Leaf)) {
        throw "Missing firmware: $Fw"
    }
    $Fw = (Resolve-Path -LiteralPath $Fw).Path
    $Size = (Get-Item -LiteralPath $Fw).Length
    if ($Size -lt 24 -or $Size + 0x2000 -gt 0x400000) {
        throw "Invalid firmware size or overlap with /data at 0x400000."
    }
}

if ($Transport -eq "usb-jtag") {
    $Before = "usb-reset"
    $After = "watchdog-reset"
} else {
    $Before = "default-reset"
    $After = "hard-reset"
}
$ToolArgs = @("-m", "esptool", "--chip", "esp32p4", "--port", $Port,
    "--baud", "$Baud", "--before", $Before, "--after", $After)
# Apache NuttX Config.mk: ESP32-P4 simple-boot app offset is 0x2000 (ROM).
if ($ProbeOnly) {
    $ToolArgs += "chip-id"
} else {
    $ToolArgs += @("write-flash", "0x2000", $Fw)
}
# Keep image-header flash settings; do not impose another board's flash size.
Write-Host ($Python + " " + (($ToolArgs | ForEach-Object { '"' + $_ + '"' }) -join " "))
if ($DryRun) {
    return
}
& $Python @ToolArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "Automatic download failed. Preserve esptool output; check the selected physical interface and close serial monitors. No alternate reset or erase is attempted."
    exit $LASTEXITCODE
}
Write-Host "esptool completed. This does not verify application boot or display operation."
