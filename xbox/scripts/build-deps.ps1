#requires -Version 7.2
# Fetches the pinned third-party sources (fetch-deps.py) and builds the two that need their own toolchain:
# glslang (host shader compiler) and FFmpeg (static, LGPL, clang-cl through MSYS2). Results stay in
# xbox/.local/deps; a step whose output exists is skipped, delete its folder to rebuild it.
[CmdletBinding()]
param([string]$Python='python',[string]$Msys2='C:\msys64')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$deps=Join-Path $root '.local/deps'
& $Python -I (Join-Path $PSScriptRoot 'fetch-deps.py')
if($LASTEXITCODE) { throw 'Fetching dependencies failed' }
& $Python -I (Join-Path $PSScriptRoot 'boost-headers.py')
if($LASTEXITCODE) { throw 'Fetching Boost headers failed' }

$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:PATH="$vs/VC/Tools/Llvm/x64/bin;$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin;C:/Program Files/CMake/bin;C:/Program Files/Ninja;$env:PATH"

$glslang=Join-Path $deps 'glslang-install/bin/glslang.exe'
if(!(Test-Path -LiteralPath $glslang)) {
    $build=Join-Path $deps 'glslang-build'
    $source=Join-Path $deps 'glslang'
    $install=[IO.Path]::GetFullPath((Join-Path $deps 'glslang-install')).Replace([char]92,[char]47)
    & cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$install" `
        -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=ON -DGLSLANG_ENABLE_INSTALL=ON
    if($LASTEXITCODE) { throw 'glslang configure failed' }
    & cmake --build $build --target install
    if($LASTEXITCODE) { throw 'glslang build failed' }
}

$ffmpeg=Join-Path $deps 'ffmpeg-install'
if(!(Test-Path -LiteralPath (Join-Path $ffmpeg 'lib/libavcodec.a'))) {
    $build=Join-Path $deps 'ffmpeg-build'
    New-Item -ItemType Directory -Force $build | Out-Null
    $unix={ param($path) $full=[IO.Path]::GetFullPath($path); '/' + $full.Substring(0,1).ToLower() + $full.Substring(2).Replace('\','/') }
    $options=@(
        "--prefix=$(& $unix $ffmpeg)", '--toolchain=msvc', '--cc=clang-cl', '--cxx=clang-cl', '--ld=lld-link',
        '--target-os=win64', '--arch=x86_64', '--enable-static', '--disable-shared', '--disable-programs',
        '--disable-doc', '--enable-runtime-cpudetect', '--disable-autodetect', '--disable-everything',
        '--disable-network', '--disable-avdevice', '--disable-avfilter', '--disable-postproc',
        '--enable-avcodec', '--enable-avformat', '--enable-swresample', '--enable-swscale',
        '--enable-decoder=aac,aac_latm,atrac3,atrac3p,atrac9,h263,h264,hevc,mjpeg,mjpegb,mp3,mpeg2video,mpeg4,opus,pcm_s16le,pcm_s8,vorbis,vp8,vp9',
        '--enable-encoder=ffv1,h263,ljpeg,mjpeg,mpeg4,pcm_s16le',
        '--enable-parser=aac,aac_latm,h263,h264,mjpeg,mpeg4video,mpegaudio,mpegvideo,opus,vorbis,vp8,vp9',
        '--enable-demuxer=aac,avi,h264,m4v,matroska,mjpeg,mov,mp3,mpegps,mpegvideo,oma,pcm_s16le,pcm_s8,pmp,wav',
        '--enable-muxer=avi', '--enable-protocol=file', '--enable-bsf=mjpeg2jpeg,vp9_superframe_split')
    $script="set -e; export PATH=`"$(& $unix (Join-Path $deps 'nasm')):`$PATH`"; cd `"$(& $unix $build)`"; " +
            "`"$(& $unix (Join-Path $deps 'ffmpeg'))/configure`" $($options -join ' ') && make -j$([Environment]::ProcessorCount) && make install"
    $env:MSYS2_PATH_TYPE='inherit'; $env:CHERE_INVOKING='1'; $env:MSYSTEM='MSYS'
    & (Join-Path $Msys2 'usr/bin/bash.exe') -lc $script
    if($LASTEXITCODE) { throw "FFmpeg build failed; see $build/ffbuild/config.log" }
}
Write-Output "UWP dependencies ready in $deps"
