/* UWP host mappings. Physical pages are backed only when touched. Single mappings
 * use private memory; a live physical alias promotes that page to a small section.
 * Unmapped direct-page contents survive in a bounded-to-touched-pages snapshot. */
typedef struct { uintptr_t owner; unsigned char *saved; HANDLE section; unsigned references,frame; uint64_t hash; unsigned char pinned,disk,hash_valid; } UwpPhysical;
typedef struct { uintptr_t address; uint64_t physical; int mapped, kind, pageable, split, committed, isolated, placed; unsigned view_mask; int protection[4]; } UwpPage;
static UwpPhysical *uwp_physical;
static UwpPage *uwp_pages;
static size_t uwp_count,uwp_capacity;
static SRWLOCK uwp_commit_lock=SRWLOCK_INIT;
static uint64_t uwp_faults,uwp_aliases,uwp_snapshots,uwp_moves;
static void uwp_fail(const char *what,uintptr_t address);
static UwpPage *uwp_page(uintptr_t address);
static int uwp_promote(UwpPhysical *physical,uint64_t offset);
static DWORD win_prot(int prot) {
    if(prot&PROT_EXEC) return PAGE_EXECUTE_READ;
    if(prot&PROT_WRITE) return PAGE_READWRITE;
    return (prot&PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void uwp_apply_protection(UwpPage *page) {
    for(unsigned i=0;i<4;++i) {
        DWORD old;
        if(!VirtualProtectFromApp((void *)(page->address+i*4096),4096,win_prot(page->protection[i]),&old)) uwp_fail("page protection",page->address+i*4096);
    }
}
static void uwp_fail(const char *what,uintptr_t address) {
    fprintf(stderr,"STOP: UWP %s at %p, error %lu\n",what,(void *)address,GetLastError());
    fflush(stderr); TerminateProcess(GetCurrentProcess(),21); abort();
}
#include "guest_swap.inc"
static size_t uwp_index(uintptr_t address) {
    size_t lo=0,hi=uwp_count;
    while(lo<hi) { size_t mid=(lo+hi)/2; if(uwp_pages[mid].address<address) lo=mid+1; else hi=mid; }
    return lo;
}
static UwpPage *uwp_page(uintptr_t address) {
    size_t i=uwp_index(address&~(uintptr_t)(PAGE-1));
    return i<uwp_count && uwp_pages[i].address==(address&~(uintptr_t)(PAGE-1)) ? &uwp_pages[i] : NULL;
}
void runtime_memory_reserve(void) {
    if(uwp_physical) return;
    uwp_physical=calloc((POOL_SIZE+FLEX_SPAN)/PAGE,sizeof(*uwp_physical));
    if(!uwp_physical) uwp_fail("physical page bookkeeping",0);
    puts("UWP memory: reserving guest windows on demand; 16 KiB commit-on-touch pages");
}
static int pool(void) { runtime_memory_reserve(); return 0; }
static uintptr_t uwp_available(uintptr_t address,uint64_t size) {
    for(uintptr_t at=address;at<address+size;) {
        UwpPage *own=uwp_page(at);
        if(own) { at=own->address+PAGE; continue; }
        MEMORY_BASIC_INFORMATION info;
        if(!VirtualQuery((void *)at,&info,sizeof(info))) return 0;
        uintptr_t end=(uintptr_t)info.BaseAddress+info.RegionSize;
        if(info.State!=MEM_FREE) return end;
        at=end;
    }
    return address;
}
// Unused guest windows stay one placeholder per reserved run; a 16 KiB page gets its own region
// only when it is first used. Every host region costs time when the process is torn down: with a
// region per reserved page, closing the game kept its window or last frame on screen for minutes.
static int uwp_reserve(uintptr_t address,uint64_t size) {
    if(address<USER_MIN || size>USER_MAX-address) return -1;
    uintptr_t end=align_up(address+size,65536);
    for(uintptr_t at=address&~(uintptr_t)65535;at<end;) {
        if(uwp_page(at)) { at+=65536; continue; }
        uintptr_t run=at;
        while(run<end && !uwp_page(run)) run+=65536;
        if(!VirtualAlloc2FromApp(NULL,(void *)at,run-at,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)) return -1;
        size_t count=(run-at)/PAGE;
        if(uwp_count+count>uwp_capacity) {
            size_t capacity=uwp_capacity ? uwp_capacity : 256;
            while(capacity<uwp_count+count) capacity*=2;
            UwpPage *next=realloc(uwp_pages,capacity*sizeof(*next));
            if(!next) uwp_fail("virtual page bookkeeping",at);
            uwp_pages=next; uwp_capacity=capacity;
        }
        size_t i=uwp_index(at);
        memmove(uwp_pages+i+count,uwp_pages+i,(uwp_count-i)*sizeof(*uwp_pages));
        for(size_t n=0;n<count;++n) uwp_pages[i+n]=(UwpPage){.address=at+n*PAGE};
        uwp_count+=count;
        at=run;
    }
    return 0;
}
// Splits this page out of a larger unused placeholder, so it can be replaced on its own.
static void uwp_isolate(UwpPage *page) {
    if(page->isolated) return;
    MEMORY_BASIC_INFORMATION info;
    if(!VirtualQuery((void *)page->address,&info,sizeof(info)) || info.State!=MEM_RESERVE) uwp_fail("placeholder query",page->address);
    uintptr_t base=(uintptr_t)info.AllocationBase,end=page->address+info.RegionSize;
    if(base<page->address && !VirtualFree((void *)base,page->address-base,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
        uwp_fail("splitting placeholder",page->address);
    if(end>page->address+PAGE && !VirtualFree((void *)page->address,PAGE,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
        uwp_fail("splitting placeholder",page->address);
    page->isolated=1;
}
static int uwp_commit(UwpPage *page) {
    if(page->kind==4||page->kind==3) return uwp_swap_page_in(page);
    if(page->kind!=1) return 0;
    if(page->committed) return 0;
    if(!page->placed) {
        uwp_isolate(page);
        if(!VirtualAlloc2FromApp(NULL,(void *)page->address,PAGE,MEM_RESERVE|MEM_REPLACE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)) uwp_fail("private page reservation",page->address);
        page->placed=1;
    }
    for(unsigned attempt=0;!VirtualAllocFromApp((void *)page->address,PAGE,MEM_COMMIT,PAGE_READWRITE);++attempt)
        if(attempt==7 || !uwp_swap_page_out_locked(UINT64_C(16)<<20,page,1)) uwp_fail("commit-on-touch",page->address);
    page->committed=1;
    UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
    if(physical->saved) { memcpy((void *)page->address,physical->saved,PAGE); free(physical->saved); physical->saved=NULL; ++uwp_moves; }
    uwp_apply_protection(page);
    ++uwp_faults;
    return 1;
}
int runtime_memory_handle_commit_fault_at(uintptr_t address,int access,int guest_code) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    UwpPage *page=uwp_page(address);
    int handled=0;
    if(page && page->mapped && (page->kind==1||page->kind==3||page->kind==4)) {
        if(guest_code) uwp_physical[page->physical/PAGE].pinned=1;
        unsigned index=(unsigned)((address-page->address)/4096);
        int wanted=page->protection[index];
        int permitted=access==1 ? (wanted&PROT_WRITE) : access==8 ? (wanted&PROT_EXEC) : (wanted&(PROT_READ|PROT_WRITE|PROT_EXEC));
        if(permitted) {
            if(guest_code && (page->kind==3||page->kind==4)) {
                if(uwp_promote(&uwp_physical[page->physical/PAGE],page->physical))
                    uwp_fail("pinned page promotion",page->address);
                handled=1;
            } else handled=uwp_commit(page);
            if(!handled && (wanted&(PROT_EXEC|PROT_WRITE))==(PROT_EXEC|PROT_WRITE)) {
                MEMORY_BASIC_INFORMATION info; DWORD old;
                uintptr_t subpage=address&~(uintptr_t)4095;
                if(VirtualQuery((void *)subpage,&info,sizeof(info)) &&
                   ((access==1 && info.Protect==PAGE_EXECUTE_READ)||(access==8 && info.Protect==PAGE_READWRITE))) {
                    handled=VirtualProtectFromApp((void *)subpage,4096,access==8 ? PAGE_EXECUTE_READ : PAGE_READWRITE,&old);
                    if(handled && access==8) FlushInstructionCache(GetCurrentProcess(),(void *)subpage,4096);
                }
            } else if(!handled) {
                MEMORY_BASIC_INFORMATION info; DWORD old;
                uintptr_t subpage=address&~(uintptr_t)4095;
                if(VirtualQuery((void *)subpage,&info,sizeof(info))&&info.State==MEM_COMMIT&&info.Protect!=win_prot(wanted))
                    handled=VirtualProtectFromApp((void *)subpage,4096,win_prot(wanted),&old);
            }
        }
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return handled;
}
int runtime_memory_handle_commit_fault(uintptr_t address,int access) {
    return runtime_memory_handle_commit_fault_at(address,access,0);
}
static int uwp_detach(UwpPage *page,int preserve) {
    if(!page->mapped) return 0;
    UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
    if(page->kind==1) {
        MEMORY_BASIC_INFORMATION info; DWORD old;
        if(preserve && page->physical<POOL_SIZE && VirtualQuery((void *)page->address,&info,sizeof(info)) && info.State==MEM_COMMIT) {
            unsigned char *saved=malloc(PAGE);
            if(!saved) uwp_fail("direct-page snapshot",page->address);
            if(!VirtualProtectFromApp((void *)page->address,PAGE,PAGE_READONLY,&old)) uwp_fail("snapshot protection",page->address);
            memcpy(saved,(void *)page->address,PAGE);
            free(physical->saved); physical->saved=saved; ++uwp_snapshots;
        }
        if(page->placed && !VirtualFree((void *)page->address,PAGE,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) uwp_fail("private page release",page->address);
        page->placed=0;
    } else if(page->kind==3||page->kind==4) {
        physical->hash_valid=0;
        uwp_swap_unmap_view(page);
    } else if(page->kind==2) {
        if(!UnmapViewOfFile2(GetCurrentProcess(),(void *)page->address,MEM_PRESERVE_PLACEHOLDER)) uwp_fail("alias page unmap",page->address);
    }
    if(physical->owner==page->address) {
        physical->owner=0;
        // With only this mapping left there cannot be a replacement owner.
        // Searching every virtual page for every released page makes large
        // unmaps quadratic and blocks file reads behind the region-table lock.
        if(physical->references>1)
            for(size_t i=0;i<uwp_count;++i) if(uwp_pages[i].mapped && &uwp_pages[i]!=page && uwp_pages[i].physical==page->physical) { physical->owner=uwp_pages[i].address; break; }
    }
    if(physical->references) --physical->references;
    page->mapped=page->kind=page->committed=0;
    memset(page->protection,0,sizeof(page->protection));
    return 0;
}
static int uwp_promote(UwpPhysical *physical,uint64_t offset) {
    if(physical->section) return 0;
    HANDLE section=CreateFileMappingFromApp(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE|SEC_COMMIT,PAGE,NULL);
    if(!section) { fprintf(stderr,"UWP memory: physical alias section failed, error %lu\n",GetLastError()); return -1; }
    void *alias=MapViewOfFile3FromApp(section,GetCurrentProcess(),NULL,0,PAGE,0,PAGE_READWRITE,NULL,0);
    if(!alias) { CloseHandle(section); return -1; }
    UwpPage *owner=uwp_page(physical->owner);
    DWORD old;
    if(owner) {
        if(owner->kind==3||owner->kind==4) {
            uwp_swap_unmap_view(owner);
            if(!uwp_swap_transfer(owner->physical,alias,0)) uwp_fail("promoted page read",owner->address);
            if(physical->hash_valid) {
                ++uwp_swap_hash_checks;
                if(uwp_swap_hash(alias)!=physical->hash) {
                    ++uwp_swap_hash_errors;
                    uwp_fail("promoted page integrity",owner->address);
                }
            }
        } else {
            uwp_commit(owner);
            if(!VirtualProtectFromApp((void *)owner->address,PAGE,PAGE_READONLY,&old)) uwp_fail("alias promotion read",owner->address);
            memcpy(alias,(void *)owner->address,PAGE);
            if(!VirtualFree((void *)owner->address,PAGE,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) uwp_fail("alias promotion release",owner->address);
            owner->placed=0;
        }
        uwp_swap_coalesce(owner);
        if(!MapViewOfFile3FromApp(section,GetCurrentProcess(),(void *)owner->address,0,PAGE,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0)) uwp_fail("canonical alias map",owner->address);
        uwp_apply_protection(owner);
        owner->kind=2;
        owner->committed=0;
    }
    UnmapViewOfFile(alias);
    physical->section=section;
    if(physical->disk) { physical->disk=0; --uwp_swap_disk_pages; }
    physical->hash_valid=0;
    (void)offset; ++uwp_aliases;
    return 0;
}
static int host_map(uintptr_t address,uint64_t size,int prot,int reserved,uint64_t offset) {
    if(uwp_reserve(address,size)) return -1;
    int pageable=0;
    if(size>=(UINT64_C(256)<<20) && offset<POOL_SIZE && !(prot&PROT_EXEC))
        for(unsigned i=0;i<LIMIT;++i)
            if(blocks[i].used && blocks[i].type==3 && blocks[i].start<=offset && offset-blocks[i].start<blocks[i].size) { pageable=1; break; }
    for(uint64_t n=0;n<size;n+=PAGE) {
        UwpPage *page=uwp_page(address+n);
        if(page->mapped) uwp_detach(page,1);
        if(reserved) continue;
        UwpPhysical *physical=&uwp_physical[(offset+n)/PAGE];
        if(physical->references && uwp_promote(physical,offset+n)) uwp_fail("alias section resource limit",page->address);
        if(physical->section || !physical->disk) uwp_swap_coalesce(page);
        if(physical->section || physical->disk) uwp_isolate(page);
        if(physical->section) {
            if(!MapViewOfFile3FromApp(physical->section,GetCurrentProcess(),(void *)page->address,0,PAGE,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0)) uwp_fail("physical alias map",page->address);
            page->kind=2;
        } else if(physical->disk) {
            page->kind=4;
            uwp_swap_split(page);
        } else {
            page->kind=1;
        }
        page->mapped=1; page->physical=offset+n; page->pageable=pageable; page->committed=0;
        for(unsigned i=0;i<4;++i) page->protection[i]=prot;
        if(page->kind==2) uwp_apply_protection(page);
        if(!physical->owner) physical->owner=page->address;
        ++physical->references;
    }
    return 0;
}
static int host_unmap(uintptr_t address,uint64_t size) {
    for(uintptr_t at=address;at<address+size;at+=PAGE) { UwpPage *page=uwp_page(at); if(page) uwp_detach(page,1); }
    return 0;
}
extern uint64_t runtime_disabled_optimizations;
// The 4 KiB pages of one 16 KiB page are one host region (committed or mapped as a whole), so they
// take one query and one protection change: the GPU's page tracking unprotects 256 KiB windows here
// under the commit lock, and page by page the Series X kept guest threads waiting on that lock for
// hundreds of milliseconds per stall (toggle bit 62: page by page, as before).
static int host_protect(uintptr_t address,uint64_t size,int prot) {
    AcquireSRWLockExclusive(&uwp_commit_lock);
    int result=0;
    const int page_by_page=(int)((__atomic_load_n(&runtime_disabled_optimizations,__ATOMIC_RELAXED)>>62)&1);
    const uintptr_t last=(address+size+4095)&~(uintptr_t)4095;
    for(uintptr_t at=address&~(uintptr_t)4095;at<last;) {
        UwpPage *page=uwp_page(at); if(!page||!page->mapped) { at+=4096; continue; }
        if(page->kind==3||page->kind==4) {
            if(!uwp_swap_protect(page,(unsigned)((at-page->address)/4096),prot)) { result=-1; break; }
            at+=4096; continue;
        }
        uintptr_t end=page->address+PAGE<last ? page->address+PAGE : last;
        if(page_by_page) end=at+4096;
        MEMORY_BASIC_INFORMATION info; DWORD old;
        if(!VirtualQuery((void *)at,&info,sizeof(info))) { result=-1;break; }
        if(info.State==MEM_COMMIT && !VirtualProtectFromApp((void *)at,end-at,win_prot(prot),&old)) { result=-1;break; }
        for(uintptr_t sub=at;sub<end;sub+=4096) page->protection[(sub-page->address)/4096]=prot;
        at=end;
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    return result;
}
static void host_discard(uint64_t offset,uint64_t size) {
    if(!uwp_physical) return;
    for(uint64_t at=offset;at<offset+size;at+=PAGE) {
        UwpPhysical *physical=&uwp_physical[at/PAGE];
        free(physical->saved); physical->saved=NULL;
        if(physical->section) { CloseHandle(physical->section); physical->section=NULL; }
        physical->owner=0; physical->references=0;
        if(physical->disk) { physical->disk=0; --uwp_swap_disk_pages; }
        physical->pinned=physical->hash_valid=0;
    }
}
static int uwp_backing_write(uintptr_t address,const void *source,uint64_t size) {
    AcquireSRWLockExclusive(&uwp_commit_lock);
    int result=1;
    while(size) {
        UwpPage *page=uwp_page(address);
        if(!page||!page->mapped) { result=0; break; }
        uint64_t offset=address-page->address,n=PAGE-offset; if(n>size) n=size;
        UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
        if(physical->disk && !physical->section) {
            if(!uwp_swap_write_backing(page->physical,offset,source,n)) { result=0; break; }
        } else if(physical->section) {
            void *view=MapViewOfFile3FromApp(physical->section,GetCurrentProcess(),NULL,0,PAGE,0,PAGE_READWRITE,NULL,0);
            if(!view) { result=0; break; }
            memcpy((unsigned char *)view+offset,source,n); UnmapViewOfFile(view);
        } else {
            uwp_commit(page);
            // Fence writes are usually eight bytes. Touch only their 4 KiB host
            // pages; changing all 16 KiB also stalls unrelated guest writers.
            for(uint64_t copied=0;copied<n;) {
                uintptr_t at=address+copied,subpage=at&~(uintptr_t)4095;
                uint64_t bytes=4096-(at-subpage); if(bytes>n-copied) bytes=n-copied;
                MEMORY_BASIC_INFORMATION info; DWORD old;
                if(!VirtualQuery((void *)subpage,&info,sizeof(info))) uwp_fail("host copy protection query",subpage);
                int change=info.Protect!=PAGE_READWRITE && info.Protect!=PAGE_EXECUTE_READWRITE;
                if(change && !VirtualProtectFromApp((void *)subpage,4096,PAGE_READWRITE,&old)) { result=0; break; }
                memcpy((void *)at,(const unsigned char *)source+copied,bytes);
                if(change && !VirtualProtectFromApp((void *)subpage,4096,info.Protect,&old)) uwp_fail("host copy protection restore",subpage);
                copied+=bytes;
            }
            if(!result) break;
        }
        address+=n; source=(const unsigned char *)source+n; size-=n;
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    return result;
}
static void uwp_memory_report(void) {
    uwp_swap_report();
    printf("UWP memory: commit faults=%" PRIu64 ", promoted alias pages=%" PRIu64 ", snapshots=%" PRIu64 ", restored=%" PRIu64 ", virtual pages=%zu\n",uwp_faults,uwp_aliases,uwp_snapshots,uwp_moves,uwp_count);
}
