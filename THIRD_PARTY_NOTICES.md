# StratoCam third-party notices

StratoCam itself is licensed under Apache License 2.0. The Windows distribution contains the following third-party components. System DLLs supplied by Windows are not redistributed.

## FFmpeg

The Windows application dynamically links to `avformat-63.dll`, `avcodec-63.dll`, `avutil-61.dll`, and `swresample-7.dll` from BtbN FFmpeg `n9.0.1-11-ge47273f4d9`, shared LGPL build with extra version `20260830`. The content SHA-256 is pinned in `scripts/bootstrap-windows-deps.ps1`; bootstrap rejects any later file published under the upstream rolling asset URL unless the dependency is deliberately reviewed and updated.

The runtime reports `--enable-shared`, `--disable-static`, `--enable-version3`, `--enable-libopus`, `--enable-libopenh264`, `--disable-libx264`, and `--disable-libx265`; it does not report `--enable-gpl` or `--enable-nonfree`. The distributed FFmpeg libraries are therefore treated as LGPL-3.0-or-later components. They remain separate replaceable DLLs beside `StratoCam.exe`. The installer includes the distribution's `FFMPEG-LICENSE.txt`. FFmpeg source and build information are available from <https://ffmpeg.org/> and <https://github.com/BtbN/FFmpeg-Builds>.

Only FFmpeg's muxing, H.264 parsing, native AAC audio encoding and audio-resampling facilities are used by StratoCam. The broad BtbN build contains additional optional codecs and libraries that StratoCam does not invoke.

## Opus

The redistributed FFmpeg build contains libopus even though StratoCam v1.0 records AAC-LC and does not invoke the Opus encoder. Opus is licensed under a three-clause BSD-style license. The complete text remains distributed as `licenses/Opus-COPYING.txt` for the bundled FFmpeg binary.

## OpenH264

The software decoder fallback statically links OpenH264 2.6.0 built from Cisco commit `652bdb7719f30b52b08e506645a7322ff1b2cc6f`. OpenH264 source is BSD-2-Clause licensed. The complete text is distributed as `licenses/OpenH264-LICENSE.txt`. The release does not redistribute Cisco's precompiled binary; it builds the library reproducibly from source.

## Android platform-tools / ADB

The Windows installer bundles only `adb.exe`, `AdbWinApi.dll`, and `AdbWinUsbApi.dll` from Android SDK Platform-Tools 37.0.1. The complete upstream `NOTICE.txt` is distributed as `licenses/Android-platform-tools-NOTICE.txt`. No private ADB RSA key is bundled; each user keeps the normal per-user key and must approve the computer on the phone.

## MinGW-w64 / GCC runtime

The Windows executable is built with WinLibs GCC 16.2 and MinGW-w64 14.0. Static GCC runtime code is covered by the GCC Runtime Library Exception. `libwinpthread-1.dll` is distributed beside the application and originates from the MinGW-w64 runtime. Upstream licensing information is available from <https://www.mingw-w64.org/> and <https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html>.

## OBS Studio plugin

The optional `stratocam-obs.dll` links to OBS Studio/libobs 32.2.2 and is licensed GPL-2.0-or-later. Its stable source ID remains `android_cam_source` for scene compatibility. The installer includes `licenses/OBS-GPL-2.0.txt`; plugin-specific notice text is also installed. OBS Studio itself is not redistributed.

## Inno Setup

The Windows installer is generated with the official signed Inno Setup 7.1.0 x64 compiler. Inno Setup is a build tool and is not included inside the installed StratoCam application. See <https://jrsoftware.org/isinfo.php>.
