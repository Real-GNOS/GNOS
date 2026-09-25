/*
 * audio.h — AC97 PCM-out streaming and the /dev/dsp device. (GPLv2)
 */
#ifndef GNUCOS_AUDIO_H
#define GNUCOS_AUDIO_H

#include <stdint.h>

int  ac97_init(void);
int  ac97_present(void);
int  ac97_selftest(void);
int  audio_start(void);
int  audio_write(const int16_t *src, uint32_t frames);
void audio_drain(void);
int  audio_vfs_register(void);

#endif
