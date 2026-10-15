#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int gow3_uwp_gamepad(uint32_t* buttons,uint8_t axes[6]);
int gow3_uwp_dialog_gamepad(uint32_t* buttons,uint8_t axes[6]);
int gow3_uwp_dialog_key(uint32_t key,int down,int repeat);
void gow3_uwp_vibration(uint8_t large_motor,uint8_t small_motor);
#ifdef __cplusplus
}
#endif
