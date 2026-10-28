# Extracts game/*.wua into extracted/ using ZArchive, then decompresses the RPX to an ELF.
# Usage: powershell -File scripts/extract.ps1 [-Wua game\mh3u.wua]
param(
    [string]$Wua = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$zarchive = Join-Path $root "tools\bin\zarchive.exe"
$rpxtool  = Join-Path $root "tools\bin\wiiurpxtool.exe"
$outDir   = Join-Path $root "extracted"

if (-not (Test-Path $zarchive)) { throw "Missing $zarchive - see tools/README.md" }

if ($Wua -eq "") {
    $found = Get-ChildItem (Join-Path $root "game") -Filter *.wua | Select-Object -First 1
    if ($null -eq $found) { throw "No .wua in game/ - see game/README.md" }
    $Wua = $found.FullName
}

Write-Host "Extracting $Wua -> $outDir"
& $zarchive $Wua $outDir
if ($LASTEXITCODE -ne 0) { throw "zarchive failed ($LASTEXITCODE)" }

# A .wua holds one folder per title (base / update / DLC), each with code/ content/ meta/.
# Decompress every RPX found so Ghidra and our tools can read them.
$rpxs = Get-ChildItem $outDir -Recurse -Filter *.rpx
foreach ($rpx in $rpxs) {
    $elf = [IO.Path]::ChangeExtension($rpx.FullName, ".elf")
    if (Test-Path $rpxtool) {
        Write-Host "Decompressing $($rpx.Name) -> $(Split-Path -Leaf $elf)"
        & $rpxtool -d $rpx.FullName $elf
    } else {
        Write-Warning "wiiurpxtool not found; skipping RPX decompression"
    }
}
Write-Host "Done. Next: scripts/fingerprint.ps1"
