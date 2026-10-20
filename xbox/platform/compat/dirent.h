#pragma once
#include <windows.h>
struct dirent { char d_name[1024]; unsigned char d_type; };
typedef struct { HANDLE handle; WIN32_FIND_DATAW data; int first; struct dirent entry; } DIR;
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
#ifdef __cplusplus
extern "C" {
#endif
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#ifdef __cplusplus
}
#endif
