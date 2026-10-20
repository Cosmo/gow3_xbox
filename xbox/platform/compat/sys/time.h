#pragma once
#include <winsock2.h>
#ifdef __cplusplus
extern "C" {
#endif
int gettimeofday(struct timeval *value, void *zone);
#ifdef __cplusplus
}
#endif
