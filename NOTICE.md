# Notices

xbrt is licensed under the GNU General Public License, version 2 or (at your option) any later version. See LICENSE.

Third-party code in this repository or linked into its binaries:

| Component | Where | Licence | Notes |
|---|---|---|---|
| font8x8 (Daniel Hepper, from Marcel Sondaar's and IBM's public-domain VGA fonts) | `shell/uwp/font8x8.h` | Public domain | The status screen's 8x8 glyphs |
| Route 2 shell and gates (`defjam-one-s`, lopezjon45-web) | `shell/uwp/gfx.cpp`, `core/disc.cpp` (the XISO lister), `scripts/isa-gate.py` | Ours | Moved here; route 2 is parked |
| One S low-memory probe (`one-s-lowmem-probe`, lopezjon45-web) | `host/memory.cpp`, `host/probes.cpp` | Ours | Its reservations and compat-mode thunks became `GuestMemory::reserve` and the U0 probes |
| Zydis and Zycore (zyantific) | `external/zydis` | MIT | The code map's x86 decoder, built without the encoder |
| XbSymbolDatabase (Cxbx-Reloaded) | `external/XbSymbolDatabase` | MIT | XDK function and variable names for overrides and the survey |
| glslang (Khronos) | `external/glslang` | BSD-3-Clause, with MIT and Apache-2.0 parts (its LICENSE.txt) | GLSL to SPIR-V in the shader chain (`host/m0`), built without HLSL input or the optimizer |
| SPIRV-Cross (Khronos) | `external/SPIRV-Cross` | Apache-2.0 | SPIR-V to HLSL in the shader chain (`host/m0`) |
| xemu (xemu-project, from QEMU) | `external/xemu` | GPL-2.0-or-later, LGPL-2.1-or-later and MIT per file header | The MCPX APU, the NV2A and QEMU's VGA core, byte-identical at the pin in `external/xemu-vendor/MANIFEST` |
| nv2a_vsh_cpu (xemu-project) | `external/nv2a_vsh_cpu` | Public domain (Unlicense) | The NV2A vertex shader interpreter xemu's PGRAPH uses |
| libsamplerate 0.2.2 (libsndfile) | `external/libsamplerate` | BSD-2-Clause | The APU voice processor's resampler |

Apache-2.0 is compatible with GPL version 3, not version 2, so a binary that links glslang and SPIRV-Cross (xbrt.exe
and xbrt_desktop.exe from M0 on) is distributed under GPL-3.0-or-later, which xbrt's "or any later version" allows.

Planned donors, not yet taken (their notices will be added with their code):

- xboxrecomp (vyanhursky; our fork lopezjon45-web/xboxrecomp): MIT, with LGPL-2.1-or-later files (the MCPX APU core
  and the NV2A register model adapted from xemu). GPL-2.0-or-later satisfies the LGPL for static linking.
- Cxbx-Reloaded: GPL-2.0-or-later. The Xbox KPCR/KPRCB/KTHREAD layouts (`src/core/kernel/common/types.h`) and the
  fs: encoding list (`EmuFS.cpp`) are the first pieces the runtime will take.
