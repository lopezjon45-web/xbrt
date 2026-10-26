# host/xemu

Upstream xemu's MCPX APU and NV2A run inside xbrt from `external/xemu`, byte-identical at the pin in
`external/xemu-vendor/MANIFEST`. This folder holds everything that is xbrt's own:

- `machine.cpp` maps each device BAR on the trap router and delivers each device's interrupt line through the fork's
  timer thread (`xbox_ApuIrqPending`, `xbox_Nv2aIrqPending`).
- `xbrt_xemu.h` is the C ABI between xbrt's C++ and the C xemu side. No QEMU type crosses it.
- `shim/` holds the QEMU API that the vendored files call: QOM, memory regions, the BQL, threads, timers and the clock
  thread. It is built with clang-cl.
- `renderers/` holds xbrt's PGRAPH renderers.

## Threads

- **Guest threads.** They run device callbacks inside the trap, holding the BQL for one access.
- **`nv2a.pfifo_thread`.** It runs PFIFO, every PGRAPH method and every renderer op.
- **`xbrt.clock`.** It runs QEMU timers (PTIMER's alarm) and the vblank, both under the BQL.
- **The fork's kernel timer thread.** It calls the title's ISRs and DPCs. It holds no device lock.

## The renderer seam

A renderer is upstream's `PGRAPHRenderer` (`external/xemu/hw/xbox/nv2a/pgraph/pgraph.h`): a type, a name and an
ops table. xbrt adds no wrapper. A renderer includes `hw/xbox/nv2a/nv2a_int.h` and works on `NV2AState`, as
upstream's `gl/` and `vk/` renderers do.

To add a renderer:

1. Add a value to `CONFIG_DISPLAY_RENDERER` in `shim/include/xemu-config.h`, before `__COUNT`.
2. Write the renderer as `renderers/<name>.c`. `build.ps1` compiles every `.c` file there with the xemu side.
3. Register it in `shim/src/machine.c` with `pgraph_renderer_register`. Add an `XbrtRenderer` value in
   `xbrt_xemu.h` that selects it.
4. Select it with `select_xemu_renderer` (`machine.h`) before the devices attach. The desktop host does this for
   `--run <disc> --renderer <name>`.

No vendored file changes. `renderers/probe.c` is the worked example. The xemu test `probe_counts_one_draw` proves it:
one push buffer draw reaches its `draw_begin` and `draw_end`.

The PGRAPH core calls these ops without checking for NULL, so a renderer must set each one:
`clear_report_value`, `clear_surface`, `draw_begin`, `draw_end`, `flip_stall`, `flush_draw`, `get_report`,
`image_blit`, `pre_savevm_trigger`, `pre_savevm_wait`, `pre_shutdown_trigger`, `pre_shutdown_wait`,
`process_pending`, `process_pending_reports`, `surface_update`. The core checks the others for NULL before it calls
them.

`process_pending` runs on the PFIFO thread with `pfifo.lock` held. It must complete a pending sync or flush exactly
as `null/renderer.c` does: clear the flag and set the event. If it does not, `nv2a_reset` waits forever on
`flush_complete`. Method ops (`draw_begin`, `surface_update` and the rest) run with `pgraph.lock` held.
`get_report` must write the report, as `pgraph_write_zpass_pixel_cnt_report` does.

## Dirty tracking

xemu's GL and Vulkan renderers call `memory_region_test_and_clear_dirty` to decide when guest CPU writes make a
cached texture or surface stale. Under QEMU's softmmu, every guest store sets those bits. In xbrt, guest code runs
natively and nothing sees its stores, so the shim answers dirty for every page, every time, and
`memory_region_set_dirty` does nothing. A caching renderer therefore re-uploads everything and never serves stale
data. Choosing a real CPU write source is the renderer unit's job. The options are write-watching the pages it
caches or hashing their contents, and the choice needs measurement.

## Open items

- `AvSetDisplayMode` in the fork programs no PCRTC or PRAMDAC register, so `NV_PCRTC_START` stays 0 after a mode
  change. A presenter that scans out from it would show the wrong memory (issue #15).
- `KeSynchronizeExecution` takes no lock. The NV2A and APU ISRs run on the fork's timer thread, so a synchronize
  routine on a title thread can overlap them (issue #16).
- Vblank is the clock thread's 59.94 Hz tick (`XbrtHost.vblank_hz`), not a display's. A presenter that has a real
  vblank should drive the GraphicHwOps hook from it instead.
