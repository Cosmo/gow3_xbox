/* UWP host mappings. Physical pages are backed only when touched. Single mappings
 * use private memory; a live physical alias promotes that page to a small section.
 * Unmapped direct-page contents survive in a bounded-to-touched-pages snapshot. */
typedef struct { uintptr_t owner; unsigned char *saved; HANDLE section; unsigned references; } UwpPhysical;
typedef struct { uintptr_t address; uint64_t physical; int mapped, kind; int protection[4]; } UwpPage;
static UwpPhysical *uwp_physical;
static UwpPage *uwp_pages;
static size_t uwp_count,uwp_capacity;
static SRWLOCK uwp_commit_lock=SRWLOCK_INIT;
static uint64_t uwp_faults,uwp_aliases,uwp_snapshots,uwp_moves;
static void uwp_fail(const char *what,uintptr_t address);
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
    fprintf(stderr,"STOP: UWP %s at %p, error %lu\n",what,(void *)address,GetLastError()); exit(21);
}
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
static int uwp_reserve(uintptr_t address,uint64_t size) {
    if(address<USER_MIN || size>USER_MAX-address) return -1;
    for(uintptr_t at=address&~(uintptr_t)65535,end=align_up(address+size,65536);at<end;at+=65536) {
        if(uwp_page(at)) continue;
        if(!VirtualAlloc2FromApp(NULL,(void *)at,65536,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)) return -1;
        if(uwp_count+4>uwp_capacity) {
            size_t capacity=uwp_capacity ? uwp_capacity*2 : 256;
            UwpPage *next=realloc(uwp_pages,capacity*sizeof(*next));
            if(!next) uwp_fail("virtual page bookkeeping",at);
            uwp_pages=next; uwp_capacity=capacity;
        }
        for(unsigned n=0;n<3;++n) {
            if(!VirtualFree((void *)(at+n*PAGE),PAGE,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) uwp_fail("splitting placeholder",at+n*PAGE);
        }
        size_t i=uwp_index(at);
        memmove(uwp_pages+i+4,uwp_pages+i,(uwp_count-i)*sizeof(*uwp_pages));
        for(unsigned n=0;n<4;++n) uwp_pages[i+n]=(UwpPage){.address=at+n*PAGE};
        uwp_count+=4;
    }
    return 0;
}
static int uwp_commit(UwpPage *page) {
    if(page->kind!=1) return 0;
    MEMORY_BASIC_INFORMATION info;
    if(!VirtualQuery((void *)page->address,&info,sizeof(info))) return 0;
    if(info.State==MEM_COMMIT) return 0;
    if(!VirtualAllocFromApp((void *)page->address,PAGE,MEM_COMMIT,PAGE_READWRITE)) uwp_fail("commit-on-touch",page->address);
    UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
    if(physical->saved) { memcpy((void *)page->address,physical->saved,PAGE); free(physical->saved); physical->saved=NULL; ++uwp_moves; }
    uwp_apply_protection(page);
    ++uwp_faults;
    return 1;
}
int runtime_memory_handle_commit_fault(uintptr_t address,int access) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    UwpPage *page=uwp_page(address);
    int handled=0;
    if(page && page->mapped && page->kind==1) {
        unsigned index=(unsigned)((address-page->address)/4096);
        int wanted=page->protection[index];
        int permitted=access==1 ? (wanted&PROT_WRITE) : access==8 ? (wanted&PROT_EXEC) : (wanted&(PROT_READ|PROT_WRITE|PROT_EXEC));
        if(permitted) {
            handled=uwp_commit(page);
            if(!handled && (wanted&(PROT_EXEC|PROT_WRITE))==(PROT_EXEC|PROT_WRITE)) {
                MEMORY_BASIC_INFORMATION info; DWORD old;
                uintptr_t subpage=address&~(uintptr_t)4095;
                if(VirtualQuery((void *)subpage,&info,sizeof(info)) &&
                   ((access==1 && info.Protect==PAGE_EXECUTE_READ)||(access==8 && info.Protect==PAGE_READWRITE))) {
                    handled=VirtualProtectFromApp((void *)subpage,4096,access==8 ? PAGE_EXECUTE_READ : PAGE_READWRITE,&old);
                    if(handled && access==8) FlushInstructionCache(GetCurrentProcess(),(void *)subpage,4096);
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return handled;
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
        if(!VirtualFree((void *)page->address,PAGE,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) uwp_fail("private page release",page->address);
    } else if(page->kind==2) {
        if(!UnmapViewOfFile2(GetCurrentProcess(),(void *)page->address,MEM_PRESERVE_PLACEHOLDER)) uwp_fail("alias page unmap",page->address);
    }
    if(physical->owner==page->address) {
        physical->owner=0;
        for(size_t i=0;i<uwp_count;++i) if(uwp_pages[i].mapped && &uwp_pages[i]!=page && uwp_pages[i].physical==page->physical) { physical->owner=uwp_pages[i].address; break; }
    }
    if(physical->references) --physical->references;
    page->mapped=page->kind=0;
    memset(page->protection,0,sizeof(page->protection));
    return 0;
}
static int uwp_promote(UwpPhysical *physical,uint64_t offset) {
    if(physical->section) return 0;
    HANDLE section=CreateFileMappingFromApp(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE|SEC_COMMIT,PAGE,NULL);
    if(!section) { fprintf(stderr,"UWP memory: physical alias section failed, error %lu\n",GetLastError()); return -1; }
    UwpPage *owner=uwp_page(physical->owner);
    unsigned char data[16384]={0}; DWORD old;
    if(owner) {
        uwp_commit(owner);
        if(!VirtualProtectFromApp((void *)owner->address,PAGE,PAGE_READONLY,&old)) uwp_fail("alias promotion read",owner->address);
        memcpy(data,(void *)owner->address,PAGE);
        if(!VirtualFree((void *)owner->address,PAGE,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) uwp_fail("alias promotion release",owner->address);
        if(!MapViewOfFile3FromApp(section,GetCurrentProcess(),(void *)owner->address,0,PAGE,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0)) uwp_fail("canonical alias map",owner->address);
        memcpy((void *)owner->address,data,PAGE);
        uwp_apply_protection(owner);
        owner->kind=2;
    }
    physical->section=section;
    (void)offset; ++uwp_aliases;
    return 0;
}
static int host_map(uintptr_t address,uint64_t size,int prot,int reserved,uint64_t offset) {
    if(uwp_reserve(address,size)) return -1;
    for(uint64_t n=0;n<size;n+=PAGE) {
        UwpPage *page=uwp_page(address+n);
        if(page->mapped) uwp_detach(page,1);
        if(reserved) continue;
        UwpPhysical *physical=&uwp_physical[(offset+n)/PAGE];
        if(physical->references && uwp_promote(physical,offset+n)) uwp_fail("alias section resource limit",page->address);
        if(physical->section) {
            if(!MapViewOfFile3FromApp(physical->section,GetCurrentProcess(),(void *)page->address,0,PAGE,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0)) uwp_fail("physical alias map",page->address);
            page->kind=2;
        } else {
            if(!VirtualAlloc2FromApp(NULL,(void *)page->address,PAGE,MEM_RESERVE|MEM_REPLACE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)) uwp_fail("private page reservation",page->address);
            page->kind=1;
        }
        page->mapped=1; page->physical=offset+n;
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
static int host_protect(uintptr_t address,uint64_t size,int prot) {
    for(uintptr_t at=address&~(uintptr_t)4095;at<address+size;at+=4096) {
        UwpPage *page=uwp_page(at); if(!page||!page->mapped) continue;
        MEMORY_BASIC_INFORMATION info; DWORD old;
        if(!VirtualQuery((void *)at,&info,sizeof(info))) return -1;
        if(info.State==MEM_COMMIT && !VirtualProtectFromApp((void *)at,4096,win_prot(prot),&old)) return -1;
        page->protection[(at-page->address)/4096]=prot;
    }
    return 0;
}
static void host_discard(uint64_t offset,uint64_t size) {
    if(!uwp_physical) return;
    for(uint64_t at=offset;at<offset+size;at+=PAGE) {
        UwpPhysical *physical=&uwp_physical[at/PAGE];
        free(physical->saved); physical->saved=NULL;
        if(physical->section) { CloseHandle(physical->section); physical->section=NULL; }
        physical->owner=0; physical->references=0;
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
        if(physical->section) {
            void *view=MapViewOfFile3FromApp(physical->section,GetCurrentProcess(),NULL,0,PAGE,0,PAGE_READWRITE,NULL,0);
            if(!view) { result=0; break; }
            memcpy((unsigned char *)view+offset,source,n); UnmapViewOfFile(view);
        } else {
            uwp_commit(page); DWORD old,protections[4];
            for(unsigned i=0;i<4;++i) {
                MEMORY_BASIC_INFORMATION info;
                if(!VirtualQuery((void *)(page->address+i*4096),&info,sizeof(info))) uwp_fail("host copy protection query",page->address);
                protections[i]=info.Protect;
            }
            if(!VirtualProtectFromApp((void *)page->address,PAGE,PAGE_READWRITE,&old)) { result=0; break; }
            memcpy((void *)address,source,n);
            for(unsigned i=0;i<4;++i)
                if(!VirtualProtectFromApp((void *)(page->address+i*4096),4096,protections[i],&old)) uwp_fail("host copy protection restore",page->address);
        }
        address+=n; source=(const unsigned char *)source+n; size-=n;
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    return result;
}
static void uwp_memory_report(void) {
    printf("UWP memory: commit faults=%" PRIu64 ", promoted alias pages=%" PRIu64 ", snapshots=%" PRIu64 ", restored=%" PRIu64 ", virtual pages=%zu\n",uwp_faults,uwp_aliases,uwp_snapshots,uwp_moves,uwp_count);
}
