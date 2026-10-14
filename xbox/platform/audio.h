#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Gow3UwpAudioStream Gow3UwpAudioStream;
int gow3_uwp_audio_init(void);
int gow3_uwp_audio_channels(void);
Gow3UwpAudioStream* gow3_uwp_audio_open(int channels,int is_float);
void gow3_uwp_audio_close(Gow3UwpAudioStream* stream);
int gow3_uwp_audio_queued(Gow3UwpAudioStream* stream);
void gow3_uwp_audio_put(Gow3UwpAudioStream* stream,const void* data,int bytes);
#ifdef __cplusplus
}
#endif
