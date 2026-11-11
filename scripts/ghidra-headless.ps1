# Runs Ghidra's headless analyzer on the MH3U project (ghidra/MH3U) with more memory than
# analyzeHeadless.bat's hardcoded 2G. Extra arguments are passed straight to AnalyzeHeadless.
# Usage:
#   scripts/ghidra-headless.ps1 -process MH3G_Cafe_US.rpx -noanalysis -readOnly -postScript ProgramSummary.java
#
# Java: Ghidra gets its own JDK, whatever JAVA_HOME says (Android Studio sets it to its JBR for Gradle). In order:
# -Java <jdk dir>, $env:GHIDRA_JAVA_HOME, else the newest JDK (21+, with javac) under Program Files' Java vendors.
# JAVA_HOME is set for the Ghidra process only. Ghidra caches compiled scripts per user; classes built by a newer JDK
# fail on an older one ("class file version 70.0 ... up to 69.0"), so the cache is cleared whenever the JDK changes.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Ghidra = "D:\Projects\Tools\Ghidra - 12.0",
    [string]$MaxMem = "6G",
    [string]$Java = $env:GHIDRA_JAVA_HOME,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]]$Rest
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$launch = Join-Path $Ghidra "support\launch.bat"
if (-not (Test-Path $launch)) { throw "Ghidra not found at $Ghidra" }

function Get-JdkMajor([string]$dir) {  # from the JDK's release file: JAVA_VERSION="26.0.1" -> 26
    $release = Join-Path $dir "release"
    if (-not (Test-Path (Join-Path $dir "bin\javac.exe")) -or -not (Test-Path $release)) { return 0 }
    $m = Select-String -Path $release -Pattern '^JAVA_VERSION="(\d+)' | Select-Object -First 1
    if ($m) { [int]$m.Matches[0].Groups[1].Value } else { 0 }
}
if (-not $Java) {
    $vendors = "Java", "Eclipse Adoptium", "Microsoft", "Zulu", "Amazon Corretto", "BellSoft" |
        ForEach-Object { Join-Path $env:ProgramFiles $_ } | Where-Object { Test-Path $_ }
    $Java = Get-ChildItem $vendors -Directory -ErrorAction SilentlyContinue |
        Where-Object { (Get-JdkMajor $_.FullName) -ge 21 } |
        Sort-Object { Get-JdkMajor $_.FullName } -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (-not $Java) { throw "No JDK 21+ found under $env:ProgramFiles; pass -Java <jdk dir> or set GHIDRA_JAVA_HOME" }
}
if ((Get-JdkMajor $Java) -lt 21) { throw "$Java is not a JDK 21+ (Ghidra needs one, with javac)" }

# the compiled-script cache, cleared when the JDK differs from the one that filled it
$props = Get-Content (Join-Path $Ghidra "Ghidra\application.properties")
$version = ($props | Select-String '^application\.version=(.+)$').Matches[0].Groups[1].Value.Trim()
$release = ($props | Select-String '^application\.release\.name=(.+)$').Matches[0].Groups[1].Value.Trim()
$osgi = Join-Path $env:APPDATA "ghidra\ghidra_${version}_$release\osgi"
$marker = Join-Path $osgi "cha-cha-jdk.txt"
$last = if (Test-Path $marker) { (Get-Content $marker -Raw).Trim() } else { "" }
if ($last -ne $Java) {
    $bundles = Join-Path $osgi "compiled-bundles"
    if (Test-Path $bundles) { Remove-Item $bundles -Recurse -Force; "ghidra-headless: JDK changed, script cache cleared" }
    New-Item -ItemType Directory -Force $osgi | Out-Null
    Set-Content -Path $marker -Value $Java -Encoding utf8
}

$argv = @("fg", "jdk", "Ghidra-Headless", $MaxMem, "-XX:+UseG1GC",  # batch arg parsing splits on '=', so no key=value VM args here
          "ghidra.app.util.headless.AnalyzeHeadless", (Join-Path $root "ghidra"), "MH3U",
          "-scriptPath", (Join-Path $root "ghidra\scripts")) + $Rest
# Windows PowerShell turns every stderr line of a native program into an error record; with "Stop" the JVM's startup
# warning would end this script (and the output after it) while Ghidra keeps running. Pass stderr through as text.
$ErrorActionPreference = "Continue"
$saved = $env:JAVA_HOME
$env:JAVA_HOME = $Java
try {
    & $launch @argv 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
} finally {
    $env:JAVA_HOME = $saved
}
exit $code
