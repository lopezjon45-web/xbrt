# xbrt

An original-Xbox runtime for the Xbox One S (Dev Mode): the title's own 32-bit x86 code runs natively in compat mode
inside an x64 UWP Game package, the xboxkrnl exports are served by host code, and the NV2A, MCPX APU and OHCI are
modelled. The design is `~/mesa-one-s/xbox-runtime/candidate-opus/` (design.md, modules.md, units.md) as amended by
`SYNTHESIS.md`. Licence: GPL-2.0-or-later (LICENSE, NOTICE.md).

## Layout

- `core/` is portable C++23 with no Windows: the XBE parser, the XISO index, the launch link, and (U1) symbols, the
  code map and the survey. It builds on the Mac with `scripts/build-mac.sh`.
- `host/` is Windows x64: guest memory, the CPU-mode probes (U0), later the CPU core, the kernel adapter and devices.
- `shell/uwp/` is the console app (CoreApplication, D3D12 to D3D11On12, the status screen, protocol activation).
  `shell/desktop/` is the same reserve, device and probes as a console program for the windows-2022 runner.
- `tools/xbrt-inspect/` prints what the runtime reads from an XBE or a disc image.
- `host/m0/` is the M0 probe (SYNTHESIS.md): xemu's generated shaders through glslang, SPIRV-Cross and D3DCompile, their
  pipelines on D3D11On12, and the CPU cost of a draw. `tools/m0/pack-corpus.py` packs the corpus that the public fork
  lopezjon45-web/xemu (branch m0-corpus) records from nxdk_pgraph_tests; `tools/m0/corpus.lock` pins it;
  `console/run-m0.sh` runs the probe on the console and `scripts/m0-report.py` applies the decision rule.
- `package/make-manifests.py` writes the package manifests, including the start-refusal bisect ladder.
- `console/` holds the scripts that put a build on the console through the Device Portal.

## Build

Windows (CI, windows-2022): `build.ps1` in a VS 2022 x64 developer prompt. Mac: `scripts/build-mac.sh`.

The packages come out of CI as prerelease assets (`build-<run number>`), never as workflow artifacts.

## Rules

- No disc data, XBE, package or key is ever committed (`.gitignore`, and the survey output stays on the Mac or the
  console).
- No title id and no guest address of a known title appears in `core/`, `host/` or the fork
  (`scripts/check-no-title-code.sh`).
- A missing kernel export, device register or instruction is a named refusal, never a faked value.
