#ifndef XEMU_CONFIG_H
#define XEMU_CONFIG_H

#include <stdbool.h>

typedef enum CONFIG_DISPLAY_RENDERER {
    CONFIG_DISPLAY_RENDERER_NULL,
    CONFIG_DISPLAY_RENDERER_OPENGL,
    CONFIG_DISPLAY_RENDERER_VULKAN,
    CONFIG_DISPLAY_RENDERER__COUNT
} CONFIG_DISPLAY_RENDERER;

typedef struct XemuConfig {
    struct {
        CONFIG_DISPLAY_RENDERER renderer;
    } display;
    struct {
        bool use_dsp;
        bool use_dsp_jit;
        bool hrtf;
        float volume_limit;
        struct {
            int num_workers;
        } vp;
    } audio;
} XemuConfig;

extern XemuConfig g_config;

#endif
