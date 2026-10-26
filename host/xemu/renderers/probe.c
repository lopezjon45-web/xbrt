/* sync, flush and process_pending must match null/renderer.c, or PFIFO waits for a flush that never completes. */
#include "renderers/probe.h"

#include "hw/xbox/nv2a/nv2a_int.h"

XbrtProbeCounts xbrt_probe_counts;

static void count(uint64_t *counter) { __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED); }

static void probe_init(NV2AState *d, Error **errp)
{
    (void)d;
    (void)errp;
    count(&xbrt_probe_counts.init);
}

static void probe_process_pending(NV2AState *d)
{
    if (!qatomic_read(&d->pgraph.sync_pending) && !qatomic_read(&d->pgraph.flush_pending)) return;
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_mutex_lock(&d->pgraph.lock);
    if (qatomic_read(&d->pgraph.sync_pending)) {
        qatomic_set(&d->pgraph.sync_pending, false);
        qemu_event_set(&d->pgraph.sync_complete);
    }
    if (qatomic_read(&d->pgraph.flush_pending)) {
        qatomic_set(&d->pgraph.flush_pending, false);
        qemu_event_set(&d->pgraph.flush_complete);
    }
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
}

static void probe_clear_surface(NV2AState *d, uint32_t parameter)
{
    (void)d;
    (void)parameter;
    count(&xbrt_probe_counts.clear_surface);
}

static void probe_draw_begin(NV2AState *d)
{
    (void)d;
    count(&xbrt_probe_counts.draw_begin);
}

static void probe_draw_end(NV2AState *d)
{
    (void)d;
    count(&xbrt_probe_counts.draw_end);
}

static void probe_flip_stall(NV2AState *d)
{
    (void)d;
    count(&xbrt_probe_counts.flip_stall);
}

static void probe_get_report(NV2AState *d, uint32_t parameter)
{
    count(&xbrt_probe_counts.get_report);
    pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
}

static void probe_surface_update(NV2AState *d, bool upload, bool color_write, bool zeta_write)
{
    (void)d;
    (void)upload;
    (void)color_write;
    (void)zeta_write;
    count(&xbrt_probe_counts.surface_update);
}

static void probe_nothing(NV2AState *d) { (void)d; }

const PGRAPHRenderer xbrt_probe_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_XBRT_PROBE,
    .name = "xbrt probe",
    .ops = {
        .init = probe_init,
        .clear_report_value = probe_nothing,
        .clear_surface = probe_clear_surface,
        .draw_begin = probe_draw_begin,
        .draw_end = probe_draw_end,
        .flip_stall = probe_flip_stall,
        .flush_draw = probe_nothing,
        .get_report = probe_get_report,
        .image_blit = probe_nothing,
        .pre_savevm_trigger = probe_nothing,
        .pre_savevm_wait = probe_nothing,
        .pre_shutdown_trigger = probe_nothing,
        .pre_shutdown_wait = probe_nothing,
        .process_pending = probe_process_pending,
        .process_pending_reports = probe_nothing,
        .surface_update = probe_surface_update,
    },
};
