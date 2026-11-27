#pragma once
#include <recomp_audio/drc.h>
static inline void gba_audio_config_defaults(rab_config* config) {
    rab_config_defaults(config);
    config->delivery_profile = 2;
}
#define rab_config_defaults gba_audio_config_defaults
