#pragma once

#include <stdint.h>

typedef struct XbrtProbeCounts {
    uint64_t init, draw_begin, draw_end, clear_surface, flip_stall, get_report, surface_update;
} XbrtProbeCounts;

extern XbrtProbeCounts xbrt_probe_counts;

typedef struct PGRAPHRenderer PGRAPHRenderer;
extern const PGRAPHRenderer xbrt_probe_renderer;
