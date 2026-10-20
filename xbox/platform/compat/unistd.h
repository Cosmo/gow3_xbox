#pragma once
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <errno.h>
typedef intptr_t ssize_t;
typedef intptr_t pid_t;
typedef unsigned int mode_t;
#define close _close
#define read _read
#define write _write
#define unlink _unlink
#define rmdir _rmdir
#define access _access
#define getpid _getpid
#define strdup _strdup
#define lseek _lseeki64
#define F_OK 0
#define R_OK 4
#define W_OK 2
static inline int ftruncate(int fd,int64_t length) { int result=_chsize_s(fd,length); if(result) { errno=result; return -1; } return 0; }
