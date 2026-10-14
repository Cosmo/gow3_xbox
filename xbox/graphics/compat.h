#pragma once
#include "gow3_win_compat.h"
#include <io.h>
#ifndef ssize_t
typedef intptr_t ssize_t;
#endif
#define fseeko _fseeki64
#define ftello _ftelli64
#define fileno _fileno
