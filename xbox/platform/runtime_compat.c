#define GOW3_UWP_ADAPTER_IMPLEMENTATION
#include "runtime_compat.h"
#include "dirent.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
static int wide(const char *path,wchar_t *out,int capacity) {
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,out,capacity)) { errno=EINVAL; return 0; }
    return 1;
}
int gow3_uwp_open(const char *path,int flags,...) {
    wchar_t name[4096]; if (!wide(path,name,4096)) return -1;
    DWORD access=(flags&_O_RDWR) ? GENERIC_READ|GENERIC_WRITE : (flags&_O_WRONLY) ? GENERIC_WRITE : GENERIC_READ;
    DWORD creation=(flags&_O_CREAT) ? ((flags&_O_EXCL) ? CREATE_NEW : (flags&_O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                  : (flags&_O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    CREATEFILE2_EXTENDED_PARAMETERS params={sizeof(params),FILE_ATTRIBUTE_NORMAL,0,0,NULL,NULL};
    HANDLE handle=CreateFile2FromAppW(name,access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,creation,&params);
    if (handle==INVALID_HANDLE_VALUE) { errno=GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND ? ENOENT : EACCES; return -1; }
    int fd=_open_osfhandle((intptr_t)handle,flags & (_O_APPEND|_O_BINARY|_O_TEXT|_O_RDONLY|_O_WRONLY|_O_RDWR));
    if (fd<0) CloseHandle(handle);
    return fd;
}
FILE *gow3_uwp_fopen(const char *path,const char *mode) {
    int flags=strchr(mode,'+') ? _O_RDWR : *mode=='r' ? _O_RDONLY : _O_WRONLY;
    if (*mode=='w') flags|=_O_CREAT|_O_TRUNC;
    if (*mode=='a') flags|=_O_CREAT|_O_APPEND;
    flags|=strchr(mode,'b') ? _O_BINARY : _O_TEXT;
    int fd=gow3_uwp_open(path,flags); if(fd<0) return NULL;
    FILE *f=_fdopen(fd,mode); if(!f) _close(fd); return f;
}
static __time64_t unix_time(FILETIME t) {
    ULARGE_INTEGER u; u.LowPart=t.dwLowDateTime; u.HighPart=t.dwHighDateTime;
    return (__time64_t)(u.QuadPart/10000000ULL-11644473600ULL);
}
int gow3_uwp_stat(const char *path,struct _stat64 *out) {
    wchar_t name[4096]; WIN32_FILE_ATTRIBUTE_DATA data;
    if (!wide(path,name,4096)) return -1;
    if (!GetFileAttributesExFromAppW(name,GetFileExInfoStandard,&data)) { errno=ENOENT; return -1; }
    memset(out,0,sizeof(*out)); out->st_mode=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR|_S_IREAD : _S_IFREG|_S_IREAD;
    out->st_nlink=1; out->st_size=((int64_t)data.nFileSizeHigh<<32)|data.nFileSizeLow;
    out->st_atime=unix_time(data.ftLastAccessTime); out->st_mtime=unix_time(data.ftLastWriteTime); out->st_ctime=unix_time(data.ftCreationTime);
    return 0;
}
int gow3_uwp_mkdir(const char *path) { wchar_t name[4096]; if(!wide(path,name,4096)) return -1; if(CreateDirectoryFromAppW(name,NULL)) return 0; errno=GetLastError()==ERROR_ALREADY_EXISTS ? EEXIST : EACCES; return -1; }
HANDLE gow3_uwp_mapping(HANDLE file,LPSECURITY_ATTRIBUTES security,ULONG access,ULONG protect,ULONG attributes,ULONG64 bytes,PCWSTR name,MEM_EXTENDED_PARAMETER *params,ULONG count) {
    (void)access; (void)params; (void)count;
    return CreateFileMappingFromApp(file,security,protect|attributes,bytes,name);
}
DWORD gow3_uwp_attributes(const char *path) { wchar_t name[4096]; WIN32_FILE_ATTRIBUTE_DATA data; if(!wide(path,name,4096)||!GetFileAttributesExFromAppW(name,GetFileExInfoStandard,&data)) return INVALID_FILE_ATTRIBUTES; return data.dwFileAttributes; }
BOOL gow3_uwp_delete(const char *path) { wchar_t name[4096]; return wide(path,name,4096)&&DeleteFileFromAppW(name); }
BOOL gow3_uwp_remove_directory(const char *path) { wchar_t name[4096]; return wide(path,name,4096)&&RemoveDirectoryFromAppW(name); }
static void find_data(WIN32_FIND_DATAA *out,const WIN32_FIND_DATAW *in) {
    memset(out,0,sizeof(*out)); out->dwFileAttributes=in->dwFileAttributes;
    out->nFileSizeHigh=in->nFileSizeHigh; out->nFileSizeLow=in->nFileSizeLow;
    WideCharToMultiByte(CP_UTF8,0,in->cFileName,-1,out->cFileName,MAX_PATH,NULL,NULL);
}
HANDLE gow3_uwp_find(const char *pattern,WIN32_FIND_DATAA *out) { wchar_t name[4096]; WIN32_FIND_DATAW data; if(!wide(pattern,name,4096)) return INVALID_HANDLE_VALUE; HANDLE h=FindFirstFileExFromAppW(name,FindExInfoBasic,&data,FindExSearchNameMatch,NULL,0); if(h!=INVALID_HANDLE_VALUE) find_data(out,&data); return h; }
BOOL gow3_uwp_find_next(HANDLE h,WIN32_FIND_DATAA *out) { WIN32_FIND_DATAW data; if(!FindNextFileW(h,&data)) return FALSE; find_data(out,&data); return TRUE; }
DIR *opendir(const char *path) {
    wchar_t name[4096]; if(!wide(path,name,4096)) return NULL;
    size_t n=wcslen(name); if(n+3>=4096) { errno=ENAMETOOLONG; return NULL; } wcscat_s(name,4096,L"\\*");
    DIR *d=calloc(1,sizeof(*d)); if(!d) return NULL;
    d->handle=FindFirstFileExFromAppW(name,FindExInfoBasic,&d->data,FindExSearchNameMatch,NULL,0);
    if(d->handle==INVALID_HANDLE_VALUE) { free(d); errno=ENOENT; return NULL; } d->first=1; return d;
}
struct dirent *readdir(DIR *d) { if(!d) return NULL; if(d->first) d->first=0; else if(!FindNextFileW(d->handle,&d->data)) return NULL; if(!WideCharToMultiByte(CP_UTF8,0,d->data.cFileName,-1,d->entry.d_name,sizeof(d->entry.d_name),NULL,NULL)) return NULL; d->entry.d_type=d->data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY ? DT_DIR : DT_REG; return &d->entry; }
int closedir(DIR *d) { if(!d) return -1; FindClose(d->handle); free(d); return 0; }
int gettimeofday(struct timeval *value,void *zone) { (void)zone; FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime; uint64_t us=u.QuadPart/10-11644473600000000ULL; value->tv_sec=(long)(us/1000000); value->tv_usec=(long)(us%1000000); return 0; }
