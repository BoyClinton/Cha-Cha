# Builds Cha-Cha for Android: build/android/Cha-Cha-<version>-android-arm64.apk (arm64-v8a). docs/android.md has the setup.
#
#   scripts/build-android.ps1                 debug-signed APK (installs with adb, fine for testing)
#   scripts/build-android.ps1 -Release -Keystore my.jks -KeyAlias chacha
#                                             release APK signed with your own key (apksigner asks for the password)
#
# Steps (each skipped when its output is up to date):
#   1. FFmpeg's H.264 decoder for arm64, in WSL (scripts/android/build-ffmpeg.sh)
#   2. shaderc for arm64 with the NDK's ndk-build (its sources ship with the NDK)
#   3. the game code from your dump: recomp_emit of the PC build tree (build/cmake) into build/android/recomp-out
#   4. libmain.so (the game) + libSDL3.so with CMake + the NDK toolchain into build/android/cmake
#   5. the APK with Gradle (android/): SDL's Java glue + the native libraries
# Needs (tools/README.md § Android): the Android SDK with NDK 30 and CMake 4.1.2, Android Studio's JBR, WSL Ubuntu with
# ~/android-deps/{android-ndk-r30,ffmpeg-*}, tools/bin/android/SDL3 (release-3.4.16) and tools/bin/android/Vulkan-Headers,
# and a configured PC build in build/cmake (README "Building the runtime").
param(
    [string]$Version,  # release version (0.2.0); default: git describe of the checkout
    [switch]$Release,
    [string]$Keystore,
    [string]$KeyAlias,
    [int]$Jobs = 6  # parallel compilers: the generated game code needs 1-2 GB of memory per file
)
# cmdlet errors stop the script; native tools are judged by their exit code (Check), since PowerShell 5.1 turns any
# stderr line of theirs (javac notes, CMake warnings) into an error that 'Stop' would end the script on
$ErrorActionPreference = 'Continue'
$PSDefaultParameterValues = @{ '*:ErrorAction' = 'Stop' }
$ProgressPreference = 'SilentlyContinue'

$repo = (Resolve-Path "$PSScriptRoot\..").Path
$out = Join-Path $repo 'build\android'
$abi = 'arm64-v8a'
$api = 29
$sdk = if ($env:ANDROID_HOME -and (Test-Path $env:ANDROID_HOME)) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }
$ndk = Join-Path $sdk 'ndk\30.0.16248370'
$cmakeDir = Join-Path $sdk 'cmake\4.1.2\bin'
$jbr = 'C:\Program Files\Android\Android Studio\jbr'
$sdl = Join-Path $repo 'tools\bin\android\SDL3'
$vkHeaders = Join-Path $repo 'tools\bin\android\Vulkan-Headers\include'
$deps = Join-Path $out "deps\$abi"
# ndk-build's generated-code steps cannot handle spaces in paths (the checkout has them): its work tree lives here
$work = "$env:LOCALAPPDATA\cha-cha-android"

foreach ($p in @($ndk, $cmakeDir, $jbr, $sdl, $vkHeaders)) {
    if (-not (Test-Path $p)) { throw "missing $p (see tools/README.md § Android)" }
}
$env:JAVA_HOME = $jbr
$env:ANDROID_HOME = $sdk
New-Item -ItemType Directory -Force $out, "$deps\lib", "$deps\include" | Out-Null

function Step($text) { Write-Host "== $text" -ForegroundColor Cyan }
function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" } }

# --- 1. FFmpeg ----------------------------------------------------------------------------------------------------
Step 'FFmpeg (WSL)'
$wslRepo = '/mnt/' + $repo.Substring(0, 1).ToLower() + ($repo.Substring(2) -replace '\\', '/')
wsl -d Ubuntu-22.04 -- bash "$wslRepo/scripts/android/build-ffmpeg.sh" "$wslRepo/build/android/deps/$abi"
Check 'FFmpeg build'

# --- 2. shaderc ---------------------------------------------------------------------------------------------------
Step 'shaderc (ndk-build)'
$shadercLib = Join-Path $deps 'lib\libshaderc_combined.a'
if (-not (Test-Path $shadercLib)) {
    $shadercSrc = Join-Path $ndk 'sources\third_party\shaderc'
    $obj = Join-Path $work 'shaderc\obj'
    Push-Location $shadercSrc
    try {
        # the component libraries; ndk-build's own "combined" step quotes its llvm-ar script wrongly on Windows
        & "$ndk\ndk-build.cmd" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_STL=c++_static "APP_ABI=$abi" `
            "APP_PLATFORM=android-$api" "NDK_OUT=$obj" "NDK_LIBS_OUT=$work\shaderc\libs" -j $Jobs `
            shaderc shaderc_util glslang OSDependent OGLCompiler HLSL SPIRV SPIRV-Tools SPIRV-Tools-opt
        Check 'shaderc build'
    } finally { Pop-Location }
    $libs = Join-Path $obj "local\$abi"
    $mri = "create $work\shaderc\libshaderc_combined.a`n"
    foreach ($l in 'shaderc', 'shaderc_util', 'glslang', 'OSDependent', 'OGLCompiler', 'HLSL', 'SPIRV', 'SPIRV-Tools-opt', 'SPIRV-Tools') {
        $mri += "addlib $libs\lib$l.a`n"
    }
    $mri += "save`nend`n"
    # a file without byte-order mark, fed through cmd (a PowerShell pipe puts one in front of the script)
    [IO.File]::WriteAllText("$work\shaderc\combine.mri", $mri, (New-Object System.Text.ASCIIEncoding))
    cmd /c "`"$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-ar.exe`" -M < `"$work\shaderc\combine.mri`""
    Check 'shaderc combine'
    Copy-Item "$work\shaderc\libshaderc_combined.a" $shadercLib
    Copy-Item -Recurse -Force (Join-Path $shadercSrc 'libshaderc\include\shaderc') (Join-Path $deps 'include')
}

# --- 3. game code -------------------------------------------------------------------------------------------------
Step 'game code (recomp_emit)'
$vsCmake = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $vsCmake -or -not (Test-Path "$repo\build\cmake\CMakeCache.txt")) { throw 'configure the PC build in build/cmake first (README)' }
& $vsCmake --build "$repo\build\cmake" --config Release --target recomp_emit
Check 'recomp_emit build'
$recompOut = Join-Path $out 'recomp-out'
$stamp = Join-Path $recompOut 'functions.h'
$inputs = @("$repo\config\functions.csv", "$repo\config\overrides.txt", "$repo\build\cmake\recomp\Release\recomp_emit.exe")
$stale = -not (Test-Path $stamp) -or ($inputs | Where-Object { (Get-Item $_).LastWriteTime -gt (Get-Item $stamp).LastWriteTime })
if ($stale) {
    New-Item -ItemType Directory -Force $recompOut | Out-Null
    & "$repo\build\cmake\recomp\Release\recomp_emit.exe" $repo $recompOut 64
    Check 'recomp_emit'
}

# --- 4. native libraries ------------------------------------------------------------------------------------------
Step 'libmain.so + libSDL3.so (CMake, NDK)'
$build = Join-Path $out 'cmake'
$sysrootLib = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\$api"
& "$cmakeDir\cmake.exe" -S $repo -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$cmakeDir\ninja.exe" `
    "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" "-DANDROID_ABI=$abi" "-DANDROID_PLATFORM=android-$api" `
    -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release `
    "-DFETCHCONTENT_SOURCE_DIR_SDL3=$sdl" "-DVulkan_INCLUDE_DIR=$vkHeaders" "-DVulkan_LIBRARY=$sysrootLib\libvulkan.so" `
    "-DMH3U_RECOMP_OUT=$recompOut" "-DMH3U_ANDROID_DEPS=$deps"
Check 'CMake configure'
& "$cmakeDir\cmake.exe" --build $build --target mh3u -j $Jobs
Check 'native build'
$jni = Join-Path $out "jniLibs\$abi"
New-Item -ItemType Directory -Force $jni | Out-Null
$strip = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
foreach ($so in 'libmain.so', 'libSDL3.so') {
    $src = Get-ChildItem -Recurse $build -Filter $so | Select-Object -First 1 -ExpandProperty FullName
    if (-not $src) { throw "$so not built" }
    & $strip --strip-unneeded -o (Join-Path $jni $so) $src
    Check "strip $so"
}

# --- 5. APK -------------------------------------------------------------------------------------------------------
Step 'APK (Gradle)'
# launcher icon: the PNG inside icon/icon.ico
$res = Join-Path $out 'res\mipmap-xxxhdpi'
New-Item -ItemType Directory -Force $res | Out-Null
$ico = [IO.File]::ReadAllBytes("$repo\icon\icon.ico")
$size = [BitConverter]::ToUInt32($ico, 14); $offset = [BitConverter]::ToUInt32($ico, 18)
[IO.File]::WriteAllBytes((Join-Path $res 'ic_launcher.png'), $ico[$offset..($offset + $size - 1)])

$version = if ($Version) { $Version } else { (git -C $repo describe --tags --always --dirty 2>$null) }
if (-not $version) { $version = 'dev' }
# versionCode from x.y.z: 0.2.0 -> 2000 (each part up to 99); 1 for development builds
$code = if ($version -match '^(\d+)\.(\d+)\.(\d+)') { [int]$Matches[1] * 1000000 + [int]$Matches[2] * 1000 + [int]$Matches[3] } else { 1 }
if ($code -eq 0) { $code = 1 }
$task = if ($Release) { 'assembleRelease' } else { 'assembleDebug' }
Push-Location "$repo\android"
try {
    & .\gradlew.bat $task --no-daemon "-PchachaSdlDir=$sdl" "-PchachaJniLibs=$out\jniLibs" "-PchachaGenRes=$out\res" `
        "-PchachaVersionName=$version" "-PchachaVersionCode=$code"
    Check 'Gradle'
} finally { Pop-Location }

$apkDir = "$repo\android\app\build\outputs\apk\" + $(if ($Release) { 'release' } else { 'debug' })
$apk = Join-Path $out "Cha-Cha-$version-android-arm64.apk"
if ($Release) {
    $unsigned = Get-ChildItem $apkDir -Filter '*-unsigned.apk' | Select-Object -First 1 -ExpandProperty FullName
    if (-not $Keystore) {
        Copy-Item $unsigned ($apk -replace '\.apk$', '-unsigned.apk')
        Write-Host "unsigned: sign it with apksigner (-Keystore / -KeyAlias)" -ForegroundColor Yellow
        return
    }
    $bt = Join-Path $sdk 'build-tools\36.1.0'
    & "$bt\zipalign.exe" -f -p 4 $unsigned "$apk.aligned"
    Check 'zipalign'
    & "$bt\apksigner.bat" sign --ks $Keystore --ks-key-alias $KeyAlias --out $apk "$apk.aligned"
    Check 'apksigner'
    Remove-Item "$apk.aligned"
} else {
    Copy-Item (Join-Path $apkDir 'app-debug.apk') $apk -Force
}
Write-Host "APK: $apk" -ForegroundColor Green
