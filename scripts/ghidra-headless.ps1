# Runs Ghidra's headless analyzer on the MH3U project (ghidra/MH3U) with more memory than
# analyzeHeadless.bat's hardcoded 2G. Extra arguments are passed straight to AnalyzeHeadless.
# Usage:
#   scripts/ghidra-headless.ps1 -process MH3G_Cafe_US.rpx -noanalysis -readOnly -postScript ProgramSummary.java
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Ghidra = "D:\Projects\Tools\Ghidra - 12.0",
    [string]$MaxMem = "6G",
    [Parameter(ValueFromRemainingArguments = $true)] [string[]]$Rest
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$launch = Join-Path $Ghidra "support\launch.bat"
if (-not (Test-Path $launch)) { throw "Ghidra not found at $Ghidra" }

$argv = @("fg", "jdk", "Ghidra-Headless", $MaxMem, "-XX:+UseG1GC",  # batch arg parsing splits on '=', so no key=value VM args here
          "ghidra.app.util.headless.AnalyzeHeadless", (Join-Path $root "ghidra"), "MH3U",
          "-scriptPath", (Join-Path $root "ghidra\scripts")) + $Rest
# Windows PowerShell turns every stderr line of a native program into an error record; with "Stop" the JVM's startup
# warning would end this script (and the output after it) while Ghidra keeps running. Pass stderr through as text.
$ErrorActionPreference = "Continue"
& $launch @argv 2>&1 | ForEach-Object { "$_" }
exit $LASTEXITCODE
