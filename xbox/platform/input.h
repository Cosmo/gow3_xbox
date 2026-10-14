#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int gow3_uwp_gamepad(uint32_t* buttons,uint8_t axes[6]);
void gow3_uwp_vibration(uint8_t large,uint8_t small);
#ifdef __cplusplus
}
#endif
