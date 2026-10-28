# Packages a Release build of Cha-Cha.exe as a portable zip (Yakumo-style: the user only supplies their dump).
# Usage: powershell -File scripts/package-release.ps1 [-BuildDir build\clang] [-Version 0.1.0]
# Output: build\release\Cha-Cha-<version>-windows-x64.zip
#
# Copies an explicit list of files only - never anything from game/, extracted/, cache/ or save/. Note: Cha-Cha.exe
# itself contains the recompiled game code (C++ translated from the RPX); see docs/release/README.txt.
param(
    [string]$BuildDir = "build\clang",
    [string]$Version = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "$BuildDir\runtime\boot\Release"
$exe = Join-Path $bin "Cha-Cha.exe"
if (-not (Test-Path $exe)) { throw "No Release build at $exe - build it first: cmake --build $BuildDir --config Release --target mh3u" }

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = (git -C $root status --porcelain --untracked-files=no) -ne $null
if ($Version -eq "") { $Version = "dev-$commit" }
$name = "Cha-Cha-$Version-windows-x64"
$out = Join-Path $root "build\release"
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage "licenses") | Out-Null

# Program + SDL3 (built against the Vulkan SDK's copy)
Copy-Item $exe $stage
Copy-Item (Join-Path $bin "SDL3.dll") $stage
# Visual C++ runtime (redistributable), newest toolset in the installed Visual Studio
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$crt = Get-ChildItem (Join-Path $vs "VC\Redist\MSVC") -Directory | Sort-Object Name -Descending |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $crt) { throw "Visual C++ redistributable not found under $vs\VC\Redist\MSVC" }
foreach ($dll in "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") { Copy-Item (Join-Path $crt.FullName $dll) $stage }

# Text files
(Get-Content (Join-Path $root "docs\release\README.txt") -Raw).Replace("{VERSION}", $Version) |
    Set-Content -Encoding utf8 (Join-Path $stage "README.txt")
Set-Content -Encoding utf8 (Join-Path $stage "portable.txt") @"
This file makes Cha-Cha portable: it keeps its settings, saves and caches in the data folder next to Cha-Cha.exe.
Delete it to use %APPDATA%\Cha-Cha instead.
"@
Copy-Item (Join-Path $root "input.example.ini") $stage
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")  # Cha-Cha: GPL-3.0
Copy-Item (Join-Path $root "docs\release\THIRD_PARTY_NOTICES.md") (Join-Path $stage "licenses")
Copy-Item (Join-Path $root "$BuildDir\_deps\zstd-src\LICENSE") (Join-Path $stage "licenses\zstd-LICENSE.txt")
Copy-Item (Join-Path $root "$BuildDir\_deps\imgui-src\LICENSE.txt") (Join-Path $stage "licenses\DearImGui-LICENSE.txt")
$sdkVersion = Split-Path -Leaf $env:VULKAN_SDK
Set-Content -Encoding utf8 (Join-Path $stage "BUILDINFO.txt") @"
Version: $Version
Commit: $commit$(if ($dirty) { " (with uncommitted changes)" })
Built: $((Get-Item $exe).LastWriteTimeUtc.ToString("yyyy-MM-dd HH:mm:ss")) UTC
Platform: Windows x86-64
Configuration: Release ($BuildDir)
Vulkan SDK: $sdkVersion
"@

# Guard: nothing game-derived may be in the package
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in ".wua", ".rpx", ".rpl", ".elf", ".arc", ".spv", ".bin" }
if ($bad) { throw "refusing to package game-derived files: $($bad.Name -join ', ')" }

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Get-ChildItem $stage -Recurse -File | Select-Object @{n = "File"; e = { $_.FullName.Substring($stage.Length + 1) } }, Length | Format-Table -AutoSize
Write-Host "Packaged $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
