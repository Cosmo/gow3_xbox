#include "guest_swap_test.inc"
static void *uwp_test_protect_pages(void *opaque) {
    uintptr_t base=(uintptr_t)opaque;
    for(unsigned round=0;round<32;++round) for(unsigned i=0;i<256;++i) {
        if(host_protect(base+i*PAGE+8192,4096,PROT_READ)||
           host_protect(base+i*PAGE+8192,4096,PROT_READ|PROT_WRITE)) return (void *)1;
    }
    return NULL;
}
extern uint64_t gow3_uwp_app_memory_usage(void);
static void *uwp_test_table_writer(void *opaque) {
    volatile LONG *flags=opaque;
    InterlockedExchange(&flags[0],1);
    write_lock();
    InterlockedExchange(&flags[1],1);
    write_unlock();
    return NULL;
}
int runtime_memory_selftest(void) {
    int64_t physical=0; void *first=NULL,*alias=NULL,*remapped=NULL;
    puts("UWP memory contract self-test starting");
    if(direct_allocate(0,(int64_t)POOL_SIZE,2*PAGE,PAGE,0,&physical)) return 1;
    if(direct_map(&first,2*PAGE,3,0,physical,PAGE)) return 2;
    volatile uint32_t *value=first;
    if(value[0] || value[PAGE/4]) return 3;
    value[0]=0x13579bdf; value[PAGE/4]=0x2468ace0;
    if(direct_unmap(first,2*PAGE)) return 4;
    if(direct_map(&remapped,2*PAGE,3,0,physical,PAGE)) return 5;
    value=remapped;
    if(value[0]!=0x13579bdf || value[PAGE/4]!=0x2468ace0) return 6;
    if(direct_map(&alias,2*PAGE,3,0,physical,PAGE)) return 7;
    volatile uint32_t *other=alias;
    if(other[0]!=value[0]) return 8;
    other[0]=0xcafebabe; if(value[0]!=0xcafebabe) return 9;
    value[PAGE/4]=0x12345678; if(other[PAGE/4]!=0x12345678) return 10;
    if(kernel_mprotect(remapped,PAGE,1)) return 11;
    uint32_t copy=0x87654321;
    if(!runtime_memory_write_backing((uintptr_t)remapped,&copy,sizeof(copy)) || value[0]!=copy || other[0]!=copy) return 12;
    // Removing the canonical mapping must choose the live alias as owner;
    // removing the final mapping must preserve contents without an owner search.
    if(direct_unmap(remapped,2*PAGE) || other[0]!=copy || other[PAGE/4]!=0x12345678) return 90;
    remapped=NULL;
    if(direct_map(&remapped,2*PAGE,3,0,physical,PAGE)) return 91;
    value=remapped;
    if(value[0]!=copy || value[PAGE/4]!=0x12345678) return 92;
    if(direct_unmap(alias,2*PAGE) || value[0]!=copy) return 93;
    if(direct_release((uint64_t)physical,2*PAGE)) return 13;
    first=NULL;
    if(direct_allocate(0,(int64_t)POOL_SIZE,PAGE,PAGE,0,&physical) || direct_map(&first,PAGE,3,0,physical,PAGE)) return 14;
    if(*(volatile uint32_t *)first) return 15;
    if(direct_release((uint64_t)physical,PAGE)) return 16;
    first=NULL;
    if(map_flexible(&first,2*PAGE,3,0)) return 17;
    volatile LONG table_flags[2]={0,0};
    pthread_t table_writer;
    read_lock();
    if(pthread_create(&table_writer,NULL,uwp_test_table_writer,(void *)table_flags)) { read_unlock(); return 101; }
    while(!InterlockedCompareExchange(&table_flags[0],0,0)) Sleep(0);
    Sleep(10);
    uint32_t table_prot;
    int nested_query=query_protection(first,NULL,NULL,&table_prot);
    int writer_overlapped=InterlockedCompareExchange(&table_flags[1],0,0);
    read_unlock();
    if(pthread_join(table_writer,NULL) || nested_query || table_prot!=3 || !table_flags[1] || writer_overlapped) return 102;
    write_lock();
    nested_query=query_protection(first,NULL,NULL,&table_prot);
    write_unlock();
    if(nested_query || table_prot!=3) return 103;
    if(host_protect((uintptr_t)first+4096,4096,PROT_READ)) return 23;
    if(*((volatile uint32_t *)first+1024)) return 24;
    MEMORY_BASIC_INFORMATION before,restricted,after;
    if(!VirtualQuery(first,&before,sizeof(before)) || !VirtualQuery((unsigned char *)first+4096,&restricted,sizeof(restricted)) || !VirtualQuery((unsigned char *)first+8192,&after,sizeof(after))) return 25;
    if(before.Protect!=PAGE_READWRITE || restricted.Protect!=PAGE_READONLY || after.Protect!=PAGE_READWRITE) return 26;
    if(!runtime_memory_write_backing((uintptr_t)first+4096,&copy,sizeof(copy))) return 27;
    if(!VirtualQuery((unsigned char *)first+4096,&restricted,sizeof(restricted)) || restricted.Protect!=PAGE_READONLY) return 28;
    if(host_protect((uintptr_t)first+12288,4096,0)) return 94;
    uint64_t crossing=UINT64_C(0x1020304050607080),written=0;
    if(!runtime_memory_write_backing((uintptr_t)first+8192-4,&crossing,sizeof(crossing))) return 95;
    memcpy(&written,(unsigned char *)first+8192-4,sizeof(written));
    if(written!=crossing) return 96;
    if(!VirtualQuery(first,&before,sizeof(before)) || before.Protect!=PAGE_READWRITE) return 97;
    if(!VirtualQuery((unsigned char *)first+4096,&restricted,sizeof(restricted)) || restricted.Protect!=PAGE_READONLY) return 98;
    if(!VirtualQuery((unsigned char *)first+8192,&after,sizeof(after)) || after.Protect!=PAGE_READWRITE) return 99;
    if(!VirtualQuery((unsigned char *)first+12288,&after,sizeof(after)) || after.Protect!=PAGE_NOACCESS) return 100;
    value=first; value[0]=0xabc; value[PAGE/4]=0xdef;
    if(release_flexible(first,PAGE)) return 18;
    remapped=NULL;
    if(map_flexible(&remapped,PAGE,3,0)) return 19;
    if(*(volatile uint32_t *)remapped || value[PAGE/4]!=0xdef) return 20;
    if(release_flexible(remapped,PAGE) || release_flexible((unsigned char *)first+PAGE,PAGE)) return 21;
    first=NULL;
    if(map_flexible(&first,256*PAGE,3,0)) return 29;
    pthread_t protector;
    if(pthread_create(&protector,NULL,uwp_test_protect_pages,first)) return 30;
    for(unsigned i=0;i<256;++i) if(*(volatile uint32_t *)((uintptr_t)first+i*PAGE+8192)) return 31;
    void *thread_result=NULL;
    if(pthread_join(protector,&thread_result)||thread_result) return 32;
    for(unsigned i=0;i<256;++i) {
        if(!VirtualQuery((void *)((uintptr_t)first+i*PAGE+8192),&after,sizeof(after))||after.Protect!=PAGE_READWRITE) return 33;
    }
    DWORD old;
    uintptr_t target=(uintptr_t)first+8192;
    if(!VirtualProtectFromApp((void *)target,4096,PAGE_READONLY,&old)) return 34;
    if(!runtime_memory_handle_commit_fault(target,1)) return 35;
    if(!VirtualQuery((void *)target,&after,sizeof(after))||after.Protect!=PAGE_READWRITE) return 36;
    *(volatile uint32_t *)target=0xabcdef;
    if(release_flexible(first,256*PAGE)) return 37;
    const uint64_t memory_before=gow3_uwp_app_memory_usage();
    first=NULL;
    const uint64_t probe_size=UINT64_C(64)*1024*1024;
    if(map_flexible(&first,probe_size,3,0)) return 38;
    for(uint64_t offset=0;offset<probe_size;offset+=PAGE) *(volatile uint32_t *)((uintptr_t)first+offset)=0x12345678;
    Sleep(250);
    const uint64_t memory_peak=gow3_uwp_app_memory_usage();
    if(release_flexible(first,probe_size)) return 39;
    if(!VirtualQuery(first,&after,sizeof(after))||after.State==MEM_COMMIT) return 40;
    Sleep(250);
    const uint64_t memory_after=gow3_uwp_app_memory_usage();
    printf("UWP 64 MiB release probe: before=%" PRIu64 " MiB, peak=%" PRIu64 " MiB, after=%" PRIu64 " MiB\n",
        memory_before>>20,memory_peak>>20,memory_after>>20);
    if(flexible_bytes) return 22;
    int swap_result=uwp_swap_selftest();
    if(swap_result) { printf("STOP: swap self-test %d\n",swap_result); return swap_result; }
    puts("PASS: lazy zero pages, remap preservation, live aliases, protected host writes, zero on reuse");
    puts("PASS: partial flexible unmap retains the other page and reuses a zero page");
    puts("PASS: 4 KiB protection boundaries survive first commit and protected host copies");
    puts("PASS: nested region queries preserve reader/writer exclusion with a queued writer");
    puts("PASS: concurrent page commits and protection changes agree with tracked permissions");
    puts("PASS: a stale read-only host page recovers to its tracked writable protection");
    puts("PASS: released guest pages are no longer committed in Windows");
    uwp_memory_report(); return 0;
}
