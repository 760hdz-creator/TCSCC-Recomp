# Third-party notices

TCSCC Recomp is licensed under GPL-3.0 (see `LICENSE`). It builds on or adapts:

| Project | License | Use | URL |
|-|-|-|-|
| XenonRecomp / XenonAnalyse / XenonUtils | MIT | PPC recompiler, XEX loader (patched: `tools/patches/xenonrecomp_pz.patch`) | https://github.com/hedge-dev/XenonRecomp |
| XenosRecomp | MIT | Shader microcode translator, forked into `runtime/src/gpu/xenos/` (modified) | https://github.com/hedge-dev/XenosRecomp |
| UnleashedRecomp | GPL-3.0 | Reference runtime; kernel/XAM HLE patterns adapted | https://github.com/hedge-dev/UnleashedRecomp |
| Xenia | BSD-3-Clause | XEX patching code inside XenonUtils (`xex_patcher`); format reference | https://github.com/xenia-project/xenia |
| simde | MIT | SIMD intrinsics used by recompiled code | https://github.com/simd-everywhere/simde |
| libmspack | LGPL-2.1 | LZX decompression inside XenonUtils | https://www.cabextract.org.uk/libmspack/ |
| tiny-AES-c | Unlicense | XEX decryption inside XenonUtils | https://github.com/kokke/tiny-AES-c |
| fmt | MIT | Formatting in XenonRecomp | https://github.com/fmtlib/fmt |
| toml++ | MIT | Config parsing in XenonRecomp | https://github.com/marzer/tomlplusplus |
| FFmpeg (n7.1) | LGPL-2.1+ | XMA audio decoding: libavutil + WMA Pro/XMA2 decoder subset, built unmodified from `external/ffmpeg` (`tools/ffmpeg/`, `runtime/ffmpeg_xma.cmake`) | https://ffmpeg.org |
| Dear ImGui (v1.91.9) | MIT | Extra Settings menu (F1 / L3+R3), vendored unmodified in `runtime/thirdparty/imgui/` | https://github.com/ocornut/imgui |
| AMD FidelityFX CAS | MIT | Sharpening filter math, reimplemented in `kPostPs` (`runtime/src/gpu/d3d11_renderer.cpp`) | https://github.com/GPUOpen-Effects/FidelityFX-CAS |
| SMAA (Jimenez, Echevarria, Masia, Navarro, Gutierrez) | MIT-style (`runtime/src/gpu/smaa/LICENSE.txt`) | `SMAA.hlsl`, `AreaTex.h`, `SearchTex.h` vendored unmodified in `runtime/src/gpu/smaa/` (commit 71c806a); anti-aliasing option | https://github.com/iryoku/smaa |
| NVIDIA Image Scaling SDK 1.0.3 (NVIDIA) | MIT (`runtime/src/gpu/nis/LICENSE.txt`) | `NIS_Config.h`, `NIS_Scaler.h` vendored unmodified in `runtime/src/gpu/nis/` (commit 35e13ba; `nis_hlsl.inc` = NIS_Scaler.h as a raw string); Upscaler=NIS option | https://github.com/NVIDIAGameWorks/NVIDIAImageScaling |

The online code in `runtime/src/online/` (packet transport, Steam helper, virtual Xbox sockets, XSession and
matchmaking) is adapted from the Tekken Tag Tournament 2 PC port by shihabal3amri
(https://github.com/shihabal3amri/tekken-tag-tournament-2-pc), used with the author's permission, via
8gram Labs's TTT2-Recomp fork. That project has no license file. Ask its author before reusing the code
in other projects.

Online play needs the Steamworks SDK (Valve, Steamworks SDK license), which isn't included: the build looks for
it in `external/steamworks_sdk_165`, and `steam_api64.dll` from it ships next to the program.

Tom Clancy's Splinter Cell: Conviction is the property of Ubisoft. This project contains no game data.
