typedef struct { uintptr_t address; int stop,error; } UwpSwapReader;
extern int runtime_file_paged_selftest(void *buffer,uint64_t size);
static uint64_t uwp_swap_test_word(uint64_t index) { return index^UINT64_C(0x6d2a8c1e4790b3f5); }
static void *uwp_swap_test_reader(void *opaque) {
    UwpSwapReader *reader=opaque;
    while(!__atomic_load_n(&reader->stop,__ATOMIC_ACQUIRE)) {
        const volatile uint64_t *data=(void *)reader->address;
        for(unsigned i=0;i<PAGE/8;++i)
            if(data[i]!=uwp_swap_test_word(4*PAGE/8+i)) { __atomic_store_n(&reader->error,1,__ATOMIC_RELEASE); return NULL; }
    }
    return NULL;
}
static int uwp_swap_selftest(void) {
    const uint64_t mapped_size=UINT64_C(256)<<20,touched_size=UINT64_C(64)<<20;
    int64_t physical=0; void *mapping=NULL,*alias=NULL;
    if(direct_allocate(INT64_C(3)<<30,(int64_t)POOL_SIZE,mapped_size,PAGE,3,&physical) ||
       direct_map(&mapping,mapped_size,0x33,0,physical,PAGE)) return 60;
    volatile uint64_t *data=mapping;
    for(uint64_t i=0;i<touched_size/8;++i) data[i]=uwp_swap_test_word(i);
    runtime_memory_handle_commit_fault_at((uintptr_t)mapping,0,1);
    if(host_protect((uintptr_t)mapping+PAGE+4096,4096,PROT_READ) ||
       host_protect((uintptr_t)mapping+2*PAGE+8192,4096,0)) return 61;
    uint64_t old_limit=uwp_swap_view_limit;
    uwp_swap_view_limit=(UINT64_C(8)<<20)/PAGE;
    Sleep(100);
    uint64_t before=gow3_uwp_app_memory_usage();
    uint64_t out=runtime_memory_page_out(UINT64_C(48)<<20);
    Sleep(100);
    uint64_t after=gow3_uwp_app_memory_usage();
    MEMORY_BASIC_INFORMATION info;
    if(out<(UINT64_C(32)<<20) || !VirtualQuery((unsigned char *)mapping+PAGE,&info,sizeof(info)) || info.State!=MEM_RESERVE) return 62;
    if(uwp_page((uintptr_t)mapping)->kind!=1 || uwp_page((uintptr_t)mapping+2*PAGE)->kind!=1) return 63;
    printf("UWP swap release probe: out=%" PRIu64 " MiB, app before=%" PRIu64 " after=%" PRIu64 " MiB\n",out>>20,before>>20,after>>20);
    if(host_protect((uintptr_t)mapping+2*PAGE+8192,4096,PROT_READ|PROT_WRITE)) return 64;
    for(uint64_t i=0;i<touched_size/8;++i) if(data[i]!=uwp_swap_test_word(i)) return 65;
    if(runtime_file_paged_selftest((unsigned char *)mapping+(UINT64_C(16)<<20),UINT64_C(16)<<20)) return 83;
    for(uint64_t i=(UINT64_C(16)<<20)/8;i<(UINT64_C(32)<<20)/8;++i)
        if(data[i]!=uwp_swap_test_word(i)) return 84;
    /* Late guest-code pins must leave the shared cache, even when their total
     * exceeds its capacity. Otherwise a later scene could exhaust every slot. */
    for(uint64_t n=UINT64_C(16)<<20;n<(UINT64_C(32)<<20);n+=PAGE) {
        uintptr_t address=(uintptr_t)mapping+n;
        if(!runtime_memory_handle_commit_fault_at(address,0,1) || uwp_page(address)->kind!=2 ||
           uwp_physical[(physical+n)/PAGE].frame || *(volatile uint64_t *)address!=uwp_swap_test_word(n/8)) return 85;
    }
    if(uwp_swap_views>uwp_swap_view_limit || !uwp_swap_hash_checks || uwp_swap_hash_errors) return 66;
    UwpSwapReader reader={.address=(uintptr_t)mapping+4*PAGE};
    pthread_t thread;
    if(pthread_create(&thread,NULL,uwp_swap_test_reader,&reader)) return 67;
    for(unsigned n=0;n<48;++n) {
        runtime_memory_page_out(touched_size);
        Sleep(1);
    }
    __atomic_store_n(&reader.stop,1,__ATOMIC_RELEASE);
    if(pthread_join(thread,NULL)||reader.error) return 68;
    runtime_memory_page_out(touched_size);
    uintptr_t target=(uintptr_t)mapping+PAGE+8192;
    if(host_protect(target,4096,0)) return 69;
    if(runtime_memory_handle_commit_fault(target,0)) return 70;
    uint64_t replacement=UINT64_C(0x1234abcdef987650);
    if(!runtime_memory_write_backing(target,&replacement,sizeof(replacement))) return 71;
    if(!VirtualQuery((void *)target,&info,sizeof(info))||
       (info.State==MEM_COMMIT && info.Protect!=PAGE_NOACCESS)||uwp_page(target)->protection[2]) return 72;
    if(host_protect(target,4096,PROT_READ|PROT_WRITE)||*(volatile uint64_t *)target!=replacement) return 74;
    if(!VirtualQuery((void *)(target-4096),&info,sizeof(info))||info.Protect!=PAGE_READONLY) return 73;
    if(direct_map(&alias,PAGE,3,0,physical+4*PAGE,PAGE)) return 75;
    if(*(volatile uint64_t *)alias!=uwp_swap_test_word(4*PAGE/8)) return 76;
    *(volatile uint64_t *)alias=replacement;
    if(data[4*PAGE/8]!=replacement) return 77;
    if(direct_unmap(mapping,mapped_size)) return 78;
    mapping=NULL;
    if(direct_map(&mapping,mapped_size,0x33,0,physical,PAGE)) return 79;
    if(*(volatile uint64_t *)((uintptr_t)mapping+PAGE+8192)!=replacement ||
       *(volatile uint64_t *)((uintptr_t)mapping+4*PAGE)!=replacement) return 80;
    if(direct_release((uint64_t)physical,mapped_size)) return 81;
    if(uwp_swap_views || uwp_swap_disk_pages || uwp_swap_frame_count) return 82;
    uwp_swap_view_limit=old_limit;
    puts("PASS: disk eviction releases private commit and restores all 64 MiB byte patterns");
    puts("PASS: bounded mapped window, checksummed page-in, concurrent readers during eviction");
    puts("PASS: guest file I/O spans a paged range larger than the resident window");
    puts("PASS: late guest-code pins exceed the shared cache without occupying its slots");
    puts("PASS: guest-code pins, GPU no-access protections, protected writes, remaps and aliases survive swap");
    return 0;
}
