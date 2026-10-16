/* Kyty's Xbox pager (1303170): a cached temporary LocalState file, pressure
 * headroom, guest-code pinning, and protection-preserving eviction. Page-in fills
 * a bounded shared frame through an alias before publishing its guest views. */
#include <fileapifromapp.h>
#include <winioctl.h>

extern uint64_t gow3_uwp_app_memory_usage(void);
extern uint64_t gow3_uwp_app_memory_limit(void);
extern void gow3_uwp_pager_thread_init(void);
static HANDLE uwp_swap_file=INVALID_HANDLE_VALUE,uwp_swap_section;
static unsigned char *uwp_swap_alias,*uwp_swap_frames;
static uint64_t uwp_swap_frame_count;
static unsigned char uwp_swap_copy[16384];
static uint64_t uwp_swap_out,uwp_swap_in,uwp_swap_disk_pages,uwp_swap_views;
static uint64_t uwp_swap_hash_checks,uwp_swap_hash_errors,uwp_swap_view_evictions;
static uint64_t uwp_swap_view_limit=UINT64_C(64)*1024*1024/PAGE;
static size_t uwp_swap_cursor,uwp_swap_view_cursor;
static int uwp_swap_disabled,uwp_swap_started;
static uint64_t uwp_swap_page_out_locked(uint64_t bytes,UwpPage *exclude,int include_views);

static uint64_t uwp_swap_hash(const void *data) {
    const uint64_t *words=data;
    uint64_t hash=UINT64_C(0xcbf29ce484222325);
    for(size_t i=0;i<PAGE/sizeof(*words);++i) hash=(hash^words[i])*UINT64_C(0x100000001b3);
    return hash;
}
static int uwp_swap_open(void) {
    if(uwp_swap_section) return 1;
    if(uwp_swap_disabled) return 0;
    const char *path=getenv("GOW3_UWP_SWAP_FILE");
    if(!path||!*path) return 0;
    wchar_t wide[32768];
    if(!MultiByteToWideChar(CP_UTF8,0,path,-1,wide,32768)) return 0;
    CREATEFILE2_EXTENDED_PARAMETERS params={0};
    params.dwSize=sizeof(params);
    params.dwFileAttributes=FILE_ATTRIBUTE_TEMPORARY;
    params.dwFileFlags=FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_RANDOM_ACCESS;
    uwp_swap_file=CreateFile2FromAppW(wide,GENERIC_READ|GENERIC_WRITE,0,CREATE_ALWAYS,&params);
    DWORD done=0;
    if(uwp_swap_file==INVALID_HANDLE_VALUE ||
       !DeviceIoControl(uwp_swap_file,FSCTL_SET_SPARSE,NULL,0,NULL,0,&done,NULL)) goto failed;
    uwp_swap_section=CreateFileMappingFromApp(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE|SEC_RESERVE,uwp_swap_view_limit*PAGE,NULL);
    if(!uwp_swap_section) goto failed;
    uwp_swap_alias=MapViewOfFile3FromApp(uwp_swap_section,GetCurrentProcess(),NULL,0,
        uwp_swap_view_limit*PAGE,0,PAGE_READWRITE,NULL,0);
    if(!uwp_swap_alias) goto failed;
    uwp_swap_frames=calloc((size_t)uwp_swap_view_limit,1);
    if(!uwp_swap_frames) goto failed;
    printf("UWP swap: temporary sparse disk backing ready; %" PRIu64 " MiB mapped-window limit\n",uwp_swap_view_limit*PAGE>>20);
    return 1;
failed:
    printf("UWP swap: unavailable, error %lu\n",GetLastError());
    if(uwp_swap_alias) UnmapViewOfFile(uwp_swap_alias);
    if(uwp_swap_section) CloseHandle(uwp_swap_section);
    free(uwp_swap_frames);
    uwp_swap_alias=uwp_swap_frames=NULL; uwp_swap_section=NULL;
    if(uwp_swap_file!=INVALID_HANDLE_VALUE) CloseHandle(uwp_swap_file);
    uwp_swap_file=INVALID_HANDLE_VALUE; uwp_swap_disabled=1;
    return 0;
}
static int uwp_swap_transfer(uint64_t physical,void *data,int write) {
    OVERLAPPED offset={0}; DWORD done=0;
    offset.Offset=(DWORD)physical; offset.OffsetHigh=(DWORD)(physical>>32);
    BOOL ok=write ? WriteFile(uwp_swap_file,data,(DWORD)PAGE,&done,&offset)
                  : ReadFile(uwp_swap_file,data,(DWORD)PAGE,&done,&offset);
    return ok && done==PAGE;
}
static void uwp_swap_split(UwpPage *page) {
    if(page->split) return;
    for(unsigned i=0;i<3;++i)
        if(!VirtualFree((void *)(page->address+i*4096),4096,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
            uwp_fail("swap placeholder split",page->address);
    page->split=1;
}
static void uwp_swap_coalesce(UwpPage *page) {
    if(!page->split) return;
    if(!VirtualFree((void *)page->address,PAGE,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        uwp_fail("swap placeholder coalesce",page->address);
    page->split=0;
}
static void uwp_swap_unmap_view(UwpPage *page) {
    UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
    if(physical->frame) {
        DWORD old;
        for(unsigned i=0;i<4;++i) if((page->view_mask&(1u<<i)) && (page->protection[i]&PROT_WRITE))
            if(!VirtualProtectFromApp((void *)(page->address+i*4096),4096,PAGE_READONLY,&old))
                uwp_fail("swap frame write protection",page->address);
        unsigned char *data=uwp_swap_alias+(physical->frame-1)*PAGE;
        physical->hash=uwp_swap_hash(data); physical->hash_valid=1;
        if(!uwp_swap_transfer(page->physical,data,1)) uwp_fail("swap frame write",page->address);
    }
    for(unsigned i=0;i<4;++i) if(page->view_mask&(1u<<i))
        if(!UnmapViewOfFile2(GetCurrentProcess(),(void *)(page->address+i*4096),MEM_PRESERVE_PLACEHOLDER))
            uwp_fail("swap view release",page->address);
    if(page->view_mask) --uwp_swap_views;
    page->view_mask=0;
    if(physical->frame) {
        uwp_swap_frames[physical->frame-1]=0;
        --uwp_swap_frame_count; physical->frame=0;
    }
    page->kind=4;
}
static int uwp_swap_view_room(UwpPage *exclude) {
    for(size_t n=0;uwp_swap_frame_count>=uwp_swap_view_limit && n<uwp_count;++n) {
        if(uwp_swap_view_cursor>=uwp_count) uwp_swap_view_cursor=0;
        UwpPage *page=&uwp_pages[uwp_swap_view_cursor++];
        if(page==exclude || !page->mapped || (page->kind!=3&&page->kind!=4)) continue;
        UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
        if(physical->pinned || !physical->frame) continue;
        physical->hash_valid=0;
        uwp_swap_unmap_view(page);
        ++uwp_swap_view_evictions;
    }
    return uwp_swap_frame_count<uwp_swap_view_limit;
}
static unsigned char *uwp_swap_bytes(uint64_t offset) {
    UwpPhysical *physical=&uwp_physical[offset/PAGE];
    if(!physical->frame) {
        UwpPage *page=uwp_page(physical->owner);
        if(!uwp_swap_section || !uwp_swap_view_room(page)) uwp_fail("swap frame capacity",physical->owner);
        uint64_t frame=0;
        while(frame<uwp_swap_view_limit && uwp_swap_frames[frame]) ++frame;
        if(frame==uwp_swap_view_limit) uwp_fail("swap frame bookkeeping",physical->owner);
        unsigned char *data=uwp_swap_alias+frame*PAGE;
        if(!VirtualAllocFromApp(data,PAGE,MEM_COMMIT,PAGE_READWRITE)) uwp_fail("swap frame commit",physical->owner);
        if(!uwp_swap_transfer(offset,data,0)) uwp_fail("swap frame read",physical->owner);
        if(physical->hash_valid) {
            ++uwp_swap_hash_checks;
            if(uwp_swap_hash(data)!=physical->hash) {
                ++uwp_swap_hash_errors;
                uwp_fail("swap integrity check",physical->owner);
            }
            physical->hash_valid=0;
        }
        uwp_swap_frames[frame]=1; ++uwp_swap_frame_count;
        physical->frame=(unsigned)frame+1; uwp_swap_in+=PAGE;
    }
    return uwp_swap_alias+(physical->frame-1)*PAGE;
}
static int uwp_swap_write_backing(uint64_t offset,uint64_t within,const void *source,uint64_t size) {
    UwpPhysical *physical=&uwp_physical[offset/PAGE];
    unsigned char *data=physical->frame ? uwp_swap_alias+(physical->frame-1)*PAGE : uwp_swap_copy;
    if(!physical->frame && !uwp_swap_transfer(offset,data,0)) return 0;
    memcpy(data+within,source,(size_t)size);
    physical->hash_valid=0;
    return physical->frame || uwp_swap_transfer(offset,data,1);
}
static int uwp_swap_page_in(UwpPage *page) {
    UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
    uwp_swap_bytes(page->physical);
    unsigned before=page->view_mask;
    for(unsigned i=0;i<4;++i) {
        if((page->view_mask&(1u<<i)) || !page->protection[i]) continue;
        for(unsigned attempt=0;;++attempt) {
            if(MapViewOfFile3FromApp(uwp_swap_section,GetCurrentProcess(),(void *)(page->address+i*4096),
                (physical->frame-1)*PAGE+i*4096,4096,MEM_REPLACE_PLACEHOLDER,win_prot(page->protection[i]),NULL,0)) break;
            if(attempt==7 || !uwp_swap_page_out_locked(UINT64_C(16)*1024*1024,page,1))
                uwp_fail("swap page-in",page->address+i*4096);
        }
        page->view_mask|=1u<<i;
    }
    if(!before && page->view_mask) { page->kind=3; ++uwp_swap_views; }
    return before!=page->view_mask;
}
static int uwp_swap_protect(UwpPage *page,unsigned index,int prot) {
    unsigned bit=1u<<index;
    uintptr_t address=page->address+index*4096;
    if(page->view_mask&bit) {
        DWORD old;
        if(!VirtualProtectFromApp((void *)address,4096,win_prot(prot),&old)) {
            // A read-only section view has a read-only maximum. Replace just that
            // subpage with the new permission; its frame bytes are already complete.
            if(!UnmapViewOfFile2(GetCurrentProcess(),(void *)address,MEM_PRESERVE_PLACEHOLDER)) return 0;
            page->view_mask&=~bit;
            if(prot) {
                if(!MapViewOfFile3FromApp(uwp_swap_section,GetCurrentProcess(),(void *)address,
                    (uwp_physical[page->physical/PAGE].frame-1)*PAGE+index*4096,4096,MEM_REPLACE_PLACEHOLDER,win_prot(prot),NULL,0))
                    uwp_fail("swap protection remap",address);
                page->view_mask|=bit;
            }
            if(!page->view_mask) { page->kind=4; --uwp_swap_views; }
        }
    }
    page->protection[index]=prot;
    return 1;
}
static uint64_t uwp_swap_page_out_locked(uint64_t bytes,UwpPage *exclude,int include_views) {
    if(!uwp_swap_open()) return 0;
    uint64_t released=0;
    for(size_t scanned=0;scanned<uwp_count && released<bytes;++scanned) {
        if(uwp_swap_cursor>=uwp_count) uwp_swap_cursor=0;
        UwpPage *page=&uwp_pages[uwp_swap_cursor++];
        if(page==exclude || !page->mapped || !page->pageable || (page->kind!=1&&page->kind!=3)) continue;
        if((page->kind==1 && !page->committed) || (page->kind==3 && !include_views)) continue;
        UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
        if(physical->pinned || physical->references!=1 || physical->section) continue;
        if(page->kind==3) {
            physical->hash_valid=0;
            uwp_swap_unmap_view(page);
            released+=PAGE; uwp_swap_out+=PAGE;
            continue;
        }
        int readable=1;
        for(unsigned i=0;i<4;++i)
            if(!(page->protection[i]&PROT_READ)||(page->protection[i]&PROT_EXEC)) readable=0;
        if(!readable) continue;
        DWORD old;
        for(unsigned i=0;i<4;++i)
            if(!VirtualProtectFromApp((void *)(page->address+i*4096),4096,PAGE_READONLY,&old))
                uwp_fail("swap write protection",page->address);
        physical->hash=uwp_swap_hash((void *)page->address); physical->hash_valid=1;
        if(!uwp_swap_transfer(page->physical,(void *)page->address,1)) uwp_fail("swap private write",page->address);
        if(!VirtualFree((void *)page->address,PAGE,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
            uwp_fail("swap private page release",page->address);
        page->kind=4;
        page->committed=0;
        uwp_swap_split(page);
        if(!physical->disk) { physical->disk=1; ++uwp_swap_disk_pages; }
        released+=PAGE; uwp_swap_out+=PAGE;
    }
    return released;
}
static uint64_t uwp_swap_reclaim(uint64_t bytes,int include_views) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    uint64_t result=uwp_swap_page_out_locked(bytes,NULL,include_views);
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return result;
}
uint64_t runtime_memory_page_out(uint64_t bytes) { return uwp_swap_reclaim(bytes,1); }
static void uwp_swap_report(void) {
    read_lock(); AcquireSRWLockExclusive(&uwp_commit_lock);
    uint64_t resident=0,pinned=0;
    for(size_t i=0;i<uwp_count;++i) {
        const UwpPage *page=&uwp_pages[i];
        if(!page->mapped || !page->pageable) continue;
        if(uwp_physical[page->physical/PAGE].pinned) pinned+=PAGE;
        else if(page->kind==1 && page->committed) resident+=PAGE;
    }
    uint64_t out=uwp_swap_out,in=uwp_swap_in,disk=uwp_swap_disk_pages,views=uwp_swap_views;
    uint64_t checks=uwp_swap_hash_checks,errors=uwp_swap_hash_errors,evictions=uwp_swap_view_evictions;
    ReleaseSRWLockExclusive(&uwp_commit_lock); read_unlock();
    const char *profile=getenv("GOW3_IO_STATS");
    if(!out&&!(profile&&*profile=='1')) return;
    printf("UWP swap: out=%" PRIu64 " in=%" PRIu64 " disk=%" PRIu64 " views=%" PRIu64
           " MiB; checks=%" PRIu64 " corrupt=%" PRIu64 " window evictions=%" PRIu64 "\n",
           out>>20,in>>20,disk*PAGE>>20,views*PAGE>>20,checks,errors,evictions);
    printf("UWP swap pools: unpinned private=%" PRIu64 " pinned=%" PRIu64 " MiB\n",resident>>20,pinned>>20);
}
static void *uwp_pager(void *unused) {
    (void)unused;
    gow3_uwp_pager_thread_init();
    uint64_t reported=0;
    int draining=0;
    for(;;) {
        uint64_t limit=gow3_uwp_app_memory_limit(),used=gow3_uwp_app_memory_usage();
        uint64_t out=0;
        if(limit>(UINT64_C(1024)<<20)) {
            if(used>limit-(UINT64_C(384)<<20)) draining=1;
            if(used<=limit-(UINT64_C(512)<<20)) draining=0;
            // Shared-frame pages already have a bounded working set. Recycling those
            // repeatedly cannot free more private commit and turns pressure into churn.
            if(draining) out=uwp_swap_reclaim(UINT64_C(2)<<20,0);
        }
        uint64_t now=GetTickCount64();
        if(now-reported>=5000) {
            uwp_swap_report();
            reported=now;
        }
        Sleep(out ? 1 : 50);
    }
    return NULL;
}
void runtime_memory_start_paging(void) {
    if(uwp_swap_started) return;
    const char *enabled=getenv("GOW3_UWP_SWAP");
    if(enabled && *enabled=='0') { uwp_swap_disabled=1; puts("UWP swap: disabled by private setting"); return; }
    runtime_memory_reserve();
    if(!uwp_swap_open()) { puts("UWP swap: pager could not start"); return; }
    pthread_t thread;
    if(pthread_create(&thread,NULL,uwp_pager,NULL)) uwp_fail("pager thread",0);
    pthread_detach(thread); uwp_swap_started=1;
    printf("UWP swap: pager active, app budget=%" PRIu64 " MiB, pressure headroom=384 MiB\n",gow3_uwp_app_memory_limit()>>20);
}
