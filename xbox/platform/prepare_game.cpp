#include "prepare_game.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>

// Each step follows its Python original, including its checks; the comments name the original.
namespace Gow3Prepare {
namespace {
using View=std::span<const uint8_t>;
using Wide=__int128;
using winrt::Windows::Data::Xml::Dom::XmlElement;

constexpr uint64_t image_limit=512ull<<20;
constexpr int64_t eboot_base=0x400000;
constexpr const char* known_titles[]={"CUSA01623","CUSA01715","CUSA01720"};
// The dynamic tags of the two relocation tables: offset and size.
constexpr std::pair<uint64_t,uint64_t> relocation_tables[]={{0x61000029,0x6100002d},{0x6100002f,0x61000031}};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
// A patch site whose signature is not found exactly once (patches.py LookupError).
struct Missing : std::runtime_error { using std::runtime_error::runtime_error; };

std::string hex(uint64_t value) {
    char text[24]; std::snprintf(text,sizeof text,"%#llx",static_cast<unsigned long long>(value)); return text;
}
uint64_t add(uint64_t a,uint64_t b) { if(b>UINT64_MAX-a) fail("out of bounds: "+hex(a)+"+"+hex(b)); return a+b; }
uint64_t as_u64(Wide value) { if(value<0||value>Wide(UINT64_MAX)) fail("value out of range"); return uint64_t(value); }
int64_t as_i64(Wide value) { if(value<Wide(INT64_MIN)||value>Wide(INT64_MAX)) fail("value out of range"); return int64_t(value); }

View span(View data,uint64_t offset,uint64_t size) {
    if(offset>data.size()||size>data.size()-offset)
        fail("out of bounds: "+hex(offset)+"+"+hex(size)+" / "+hex(data.size()));
    return data.subspan(offset,size);
}
template<class T> T read(View data,uint64_t offset) {
    T value; std::memcpy(&value,span(data,offset,sizeof value).data(),sizeof value); return value;
}
template<class T> void put(Bytes& out,T value) {
    auto bytes=reinterpret_cast<const uint8_t*>(&value); out.insert(out.end(),bytes,bytes+sizeof value);
}
void put_text(Bytes& out,const char* text,size_t size) { out.insert(out.end(),text,text+size); }
// The caller has checked that the bytes lie in the image.
void store(Bytes& image,uint64_t offset,uint64_t value) { std::memcpy(image.data()+offset,&value,sizeof value); }

// Python int(text) (base 10) and int(text, 0): sign, 0x/0o/0b with base 0, "_" between digits.
Wide parse_int(std::string text,bool any_base) {
    auto space=[](char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; };
    while(!text.empty()&&space(text.back())) text.pop_back();
    size_t i=0;
    while(i<text.size()&&space(text[i])) ++i;
    bool negative=false;
    if(i<text.size()&&(text[i]=='+'||text[i]=='-')) negative=text[i++]=='-';
    unsigned base=10;
    if(any_base&&i+1<text.size()&&text[i]=='0'&&std::strchr("xXoObB",text[i+1])) {
        base=std::strchr("xX",text[i+1]) ? 16 : std::strchr("oO",text[i+1]) ? 8 : 2;
        i+=2;
        if(i<text.size()&&text[i]=='_') ++i;
    }
    uint64_t value=0; bool digits=false,nonzero_after_zero=false,leading_zero=false;
    for(;i<text.size();++i) {
        char c=text[i];
        if(c=='_') {
            if(!digits||i+1>=text.size()||text[i+1]=='_') fail("invalid literal for int(): '"+text+"'");
            continue;
        }
        unsigned digit=c>='0'&&c<='9' ? unsigned(c-'0') : c>='a'&&c<='z' ? unsigned(c-'a'+10) : c>='A'&&c<='Z' ? unsigned(c-'A'+10) : 99;
        if(digit>=base) fail("invalid literal for int(): '"+text+"'");
        if(!digits&&digit==0) leading_zero=true;
        else if(leading_zero&&digit) nonzero_after_zero=true;
        if(value>(UINT64_MAX-digit)/base) fail("integer too large: '"+text+"'");
        value=value*base+digit; digits=true;
    }
    if(!digits) fail("invalid literal for int(): '"+text+"'");
    if(any_base&&base==10&&leading_zero&&nonzero_after_zero) fail("invalid literal for int() with base 0: '"+text+"'");
    return negative ? -Wide(value) : Wide(value);
}

// prepare.py parse_self
struct Segment { uint32_t type,flags; uint64_t offset,vaddr,paddr,filesz,memsz,align; };
struct Self { Bytes elf; uint64_t entry=0; std::vector<Segment> ph; };

Self parse_self(View data) {
    if(data.empty()) fail("empty SELF (0 bytes)");
    View magic=span(data,0,4);
    const bool plain_elf=!std::memcmp(magic.data(),"\x7f" "ELF",4);
    uint64_t count=0,base=0;
    if(!plain_elf) {
        if(std::memcmp(magic.data(),"O\x15=\x1d",4)) fail("expected PS4 SELF or ELF");
        count=read<uint16_t>(data,24);
        base=32+count*32;
    }
    View header=span(data,base,64);
    if(std::memcmp(header.data(),"\x7f" "ELF\x02\x01\x01",7)||read<uint16_t>(header,18)!=62)
        fail("expected little-endian x86-64 ELF");
    const uint16_t phentsize=read<uint16_t>(header,54),phnum=read<uint16_t>(header,56);
    if(phentsize!=56||phnum==0||phnum>=256) fail("unsupported program headers");
    Self self;
    self.entry=read<uint64_t>(header,24);
    const uint64_t phoff=read<uint64_t>(header,32);
    for(uint64_t i=0;i<phnum;++i) {
        View entry=span(data,add(add(base,phoff),i*56),56);
        self.ph.push_back({read<uint32_t>(entry,0),read<uint32_t>(entry,4),read<uint64_t>(entry,8),read<uint64_t>(entry,16),
                           read<uint64_t>(entry,24),read<uint64_t>(entry,32),read<uint64_t>(entry,40),read<uint64_t>(entry,48)});
    }
    const uint64_t header_end=add(phoff,uint64_t(phnum)*56);
    uint64_t end=header_end;
    for(const auto& p:self.ph) end=std::max(end,add(p.offset,p.filesz));
    if(end>image_limit) fail("probe image exceeds 512 MiB limit");
    // An unpacked ELF already holds its segments at their file offsets.
    if(plain_elf) {
        View whole=span(data,0,end);
        self.elf.assign(whole.begin(),whole.end());
        return self;
    }
    View headers=span(data,base,header_end);
    self.elf.assign(end,0);
    std::copy(headers.begin(),headers.end(),self.elf.begin());
    std::vector<std::pair<uint64_t,uint64_t>> covered{{0,header_end}};
    for(uint64_t i=0;i<count;++i) {
        View entry=span(data,32+i*32,32);
        const uint64_t flags=read<uint64_t>(entry,0),offset=read<uint64_t>(entry,8);
        const uint64_t size=read<uint64_t>(entry,16),memory=read<uint64_t>(entry,24);
        if(!(flags&0x800)) continue;
        if(flags&10) fail("encrypted/compressed SELF segment is unsupported");
        const uint64_t index=(flags>>20)&4095;
        if(index>=self.ph.size()) fail("invalid segment index");
        const auto& p=self.ph[index];
        if(size!=p.filesz||size!=memory) fail("unsupported blocked segment layout");
        View bytes=span(data,offset,size);
        std::copy(bytes.begin(),bytes.end(),self.elf.begin()+p.offset);
        covered.emplace_back(p.offset,p.offset+size);
    }
    for(size_t i=0;i<self.ph.size();++i) {
        const auto& p=self.ph[i];
        if(!p.filesz||std::any_of(covered.begin(),covered.end(),[&](const auto& c) {
               return c.first<=p.offset&&p.offset+p.filesz<=c.second; })) continue;
        if(p.type==1||p.type==2||p.type==0x61000000||p.type==0x61000010) fail("required segment "+std::to_string(i)+" is unavailable");
    }
    return self;
}

const Segment* find_segment(const std::vector<Segment>& ph,uint32_t type) {
    for(const auto& p:ph) if(p.type==type) return &p;
    return nullptr;
}
const Segment& require_segment(const std::vector<Segment>& ph,uint32_t type) {
    if(auto p=find_segment(ph,type)) return *p;
    fail("program header "+hex(type)+" is missing");
}

// The dynamic table and the strings it names (prepare.py prepare, inspect_libc; link_modules.py module).
struct Dynamic { std::vector<std::pair<uint64_t,uint64_t>> entries; std::map<uint64_t,uint64_t> tags; View blob,strings; };

uint64_t tag(const std::map<uint64_t,uint64_t>& tags,uint64_t key) {
    auto found=tags.find(key);
    if(found==tags.end()) fail("dynamic tag "+hex(key)+" is missing");
    return found->second;
}
Dynamic dynamic(const Self& self,bool stop_at_null) {
    Dynamic d;
    const auto& dp=require_segment(self.ph,2);
    for(uint64_t pos=dp.offset;pos<dp.offset+dp.filesz;pos+=16) {
        View entry=span(self.elf,pos,16);
        const uint64_t key=read<uint64_t>(entry,0),value=read<uint64_t>(entry,8);
        if(stop_at_null&&!key) break;
        d.entries.emplace_back(key,value);
        d.tags[key]=value;
    }
    const auto& lib=require_segment(self.ph,0x61000000);
    d.blob=span(self.elf,lib.offset,lib.filesz);
    d.strings=span(d.blob,tag(d.tags,0x61000035),tag(d.tags,0x61000037));
    return d;
}
std::string text(View strings,uint64_t offset) {
    span(strings,offset,1);
    auto begin=strings.begin()+offset,end=std::find(begin,strings.end(),uint8_t(0));
    if(end==strings.end()) fail("unterminated string");
    if(std::any_of(begin,end,[](uint8_t c) { return c>=0x80; })) fail("non-ASCII symbol name");
    return std::string(begin,end);
}
View symbol_table(const Dynamic& d) { return span(d.blob,tag(d.tags,0x61000039),tag(d.tags,0x6100003f)); }

// prepare.py inspect_libc: whether this dump's _init_env is exactly RET.
bool init_env_is_ret(View source) {
    const Self self=parse_self(source);
    const Dynamic d=dynamic(self,false);
    View syms=symbol_table(d);
    bool result=false;
    for(uint64_t pos=0;pos<syms.size();pos+=24) {
        View entry=span(syms,pos,24);
        const uint8_t info=entry[4];
        const uint16_t section=read<uint16_t>(entry,6);
        const uint64_t value=read<uint64_t>(entry,8),size=read<uint64_t>(entry,16);
        const std::string symbol=text(d.strings,read<uint32_t>(entry,0));
        const std::string key=symbol.substr(0,symbol.find('#'));
        // The NIDs of _init_env and _ZNSt8ios_base4InitC1Ev (prepare.py nid).
        const bool init_env=key=="bzQExy189ZI";
        if((!init_env&&key!="sqWytnhYdEg")||!section||(info&15)!=2) continue;
        const Segment* code_segment=nullptr;
        for(const auto& p:self.ph)
            if(p.type==1&&(p.flags&1)&&p.vaddr<=value&&Wide(value)<Wide(p.vaddr)+p.filesz) { code_segment=&p; break; }
        if(!code_segment) fail("libc.prx: "+symbol+" lies outside its code");
        View code=span(self.elf,code_segment->offset+value-code_segment->vaddr,size);
        if(init_env) result=code.size()==1&&code[0]==0xc3;
    }
    return result;
}

// prepare.py prepare: the eboot's memory image, its imports and relocations (boot.bin, G3PROBE2).
struct Relocation { uint64_t target,kind; int64_t value,addend; };
struct Probe {
    uint64_t size=0,entry=0,flags=0;
    std::vector<std::array<uint64_t,3>> segments;
    std::vector<std::string> names;
    std::vector<Relocation> relocs;
    Bytes image;
};

bool mapped(const std::vector<const Segment*>& loads,uint64_t address,uint64_t size) {
    return std::any_of(loads.begin(),loads.end(),[&](const Segment* p) {
        return p->vaddr<=address&&Wide(address)+size<=Wide(p->vaddr)+p->memsz; });
}
std::vector<const Segment*> load_segments(const Self& self) {
    std::vector<const Segment*> loads;
    for(const auto& p:self.ph) if(p.type==1||p.type==0x61000010) loads.push_back(&p);
    if(loads.empty()) fail("no loadable segments");
    return loads;
}
uint64_t memory_size(const std::vector<const Segment*>& loads) {
    Wide size=0;
    for(auto p:loads) size=std::max(size,Wide(p->vaddr)+p->memsz);
    return size>Wide(image_limit) ? image_limit+1 : uint64_t(size);
}

Probe prepare_eboot(const Self& self,bool init_env) {
    const auto loads=load_segments(self);
    Probe probe;
    probe.size=memory_size(loads);
    if(probe.size>image_limit||!mapped(loads,self.entry,1)) fail("invalid memory image");
    probe.entry=self.entry;
    probe.image.assign(probe.size,0);
    for(auto p:loads) {
        if(p->filesz>p->memsz) fail("segment file size exceeds memory size");
        View bytes=span(self.elf,p->offset,p->filesz);
        std::copy(bytes.begin(),bytes.end(),probe.image.begin()+p->vaddr);
    }
    const Dynamic d=dynamic(self,true);
    View syms=symbol_table(d);
    if(syms.size()%24) fail("bad symbol table size");
    struct Symbol { std::string name; unsigned type; uint16_t section; uint64_t value; };
    std::vector<Symbol> symbols;
    for(uint64_t pos=0;pos<syms.size();pos+=24) {
        View entry=span(syms,pos,24);
        symbols.push_back({text(d.strings,read<uint32_t>(entry,0)),entry[4]&15u,read<uint16_t>(entry,6),read<uint64_t>(entry,8)});
    }
    std::map<uint64_t,uint64_t> imports;
    std::vector<uint64_t> import_order;
    for(auto [offset_tag,size_tag]:relocation_tables) {
        View table=span(d.blob,tag(d.tags,offset_tag),tag(d.tags,size_tag));
        if(table.size()%24) fail("bad relocation table size");
        for(uint64_t pos=0;pos<table.size();pos+=24) {
            View entry=span(table,pos,24);
            const uint64_t target=read<uint64_t>(entry,0),info=read<uint64_t>(entry,8);
            const int64_t addend=read<int64_t>(entry,16);
            const uint64_t kind=info&0xffffffff,sym=info>>32;
            if(!mapped(loads,target,8)) fail("relocation outside mapped segments: "+hex(target));
            if(kind==8) {
                probe.relocs.push_back({target,0,addend,0});
            } else if(kind==1||kind==6||kind==7) {
                if(sym>=symbols.size()) fail("relocation symbol out of range");
                const auto& symbol=symbols[sym];
                if(symbol.section) {
                    if(symbol.section==0xfff1) store(probe.image,target,as_u64(Wide(symbol.value)+addend));
                    else probe.relocs.push_back({target,0,as_i64(Wide(symbol.value)+addend),0});
                } else {
                    auto [import,inserted]=imports.try_emplace(sym,imports.size());
                    if(inserted) import_order.push_back(sym);
                    const uint64_t import_kind=symbol.type==1 ? 2 : 1;
                    if((import_kind==1&&addend)||addend<0||addend>=4096) fail("unsupported import addend");
                    probe.relocs.push_back({target,import_kind,int64_t(import->second),addend});
                }
            } else {
                fail("unsupported relocation "+std::to_string(kind));
            }
        }
    }
    for(auto sym:import_order) probe.names.push_back(symbols[sym].name);
    for(const auto& name:probe.names) if(name.size()>=128) fail("import name too long");
    for(auto p:loads) probe.segments.push_back({p->vaddr,p->memsz,p->flags});
    probe.flags=init_env;
    return probe;
}

// link_modules.py: the eboot and the bundled modules in one image (boot-linked.bin, G3PROBE5).
struct Identity {
    std::string nid,library; uint64_t library_version; std::string module; uint64_t module_version;
    auto operator<=>(const Identity&) const=default;
};
using Key=std::optional<Identity>;
struct Symbol { std::string name; unsigned type,binding; uint16_t section; uint64_t value,size; Key identity; };
struct ModuleRelocation { uint64_t target,kind,symbol; int64_t addend; };
struct Module {
    Self self;
    Dynamic d;
    std::map<std::string,std::pair<std::string,uint64_t>> libraries,modules;
    std::vector<Symbol> symbols;
    std::vector<ModuleRelocation> relocs;
    // NID#library#module names link; plain names such as module_start do not.
    Key identity(const std::string& name) const {
        const auto first=name.find('#'),second=first==std::string::npos ? first : name.find('#',first+1);
        if(second==std::string::npos||name.find('#',second+1)!=std::string::npos) return std::nullopt;
        auto library=libraries.find(name.substr(first+1,second-first-1));
        if(library==libraries.end()) fail("unknown library in "+name);
        auto module=modules.find(name.substr(second+1));
        if(module==modules.end()) fail("unknown module in "+name);
        return Identity{name.substr(0,first),library->second.first,library->second.second,module->second.first,module->second.second};
    }
};

// link_libc.py encode_id
std::string encode_id(uint64_t value) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result(1,alphabet[value&63]);
    for(value>>=6;value;value>>=6) result.insert(result.begin(),alphabet[value&63]);
    return result;
}

Module load_module(View source) {
    Module m;
    m.self=parse_self(source);
    m.d=dynamic(m.self,true);
    for(auto [key,value]:m.d.entries) {
        if(key!=0x61000013&&key!=0x61000015&&key!=0x6100000d&&key!=0x6100000f) continue;
        auto& table=key==0x61000013||key==0x61000015 ? m.libraries : m.modules;
        const std::string id=encode_id(value>>48);
        std::pair<std::string,uint64_t> identity{text(m.d.strings,value&0xffffffff),(value>>32)&65535};
        auto found=table.find(id);
        if(found!=table.end()&&found->second!=identity) fail("conflicting module/library IDs");
        table[id]=identity;
    }
    View syms=symbol_table(m.d);
    for(uint64_t pos=0;pos<syms.size();pos+=24) {
        View entry=span(syms,pos,24);
        Symbol s;
        s.name=text(m.d.strings,read<uint32_t>(entry,0));
        s.type=entry[4]&15u; s.binding=entry[4]>>4;
        s.section=read<uint16_t>(entry,6); s.value=read<uint64_t>(entry,8); s.size=read<uint64_t>(entry,16);
        if(!s.name.empty()) s.identity=m.identity(s.name);
        m.symbols.push_back(std::move(s));
    }
    for(auto [offset_tag,size_tag]:relocation_tables) {
        View table=span(m.d.blob,tag(m.d.tags,offset_tag),tag(m.d.tags,size_tag));
        for(uint64_t pos=0;pos<table.size();pos+=24) {
            View entry=span(table,pos,24);
            const uint64_t info=read<uint64_t>(entry,8);
            m.relocs.push_back({read<uint64_t>(entry,0),info&0xffffffff,info>>32,read<int64_t>(entry,16)});
        }
    }
    return m;
}

// Initial-exec `mov rax, fs:[0]` becomes GS (link_modules.py patch_fs_loads).
void patch_fs_loads(Bytes& image,const std::vector<Segment>& ph,uint64_t base) {
    static const uint8_t load[]={0x64,0x48,0x8b,0x04,0x25,0,0,0,0};
    for(const auto& p:ph) {
        if(p.type!=1||!(p.flags&1)) continue;
        const uint64_t end=std::min<uint64_t>(base+p.vaddr+p.filesz,image.size());
        for(uint64_t at=base+p.vaddr;at+sizeof load<=end;) {
            auto found=std::search(image.begin()+at,image.begin()+end,std::begin(load),std::end(load));
            if(found==image.begin()+end) break;
            *found=0x65;
            at=uint64_t(found-image.begin())+sizeof load;
        }
    }
}

Bytes link(const Module& main,Probe& probe,const std::vector<Module>& modules) {
    // The import table: identity -> index, in the order identities first appear (a Python dict).
    std::vector<Key> keys;
    std::vector<uint64_t> indexes;
    std::map<Key,size_t> positions;
    auto set=[&](const Key& key,uint64_t index) {
        auto [position,inserted]=positions.try_emplace(key,keys.size());
        if(inserted) { keys.push_back(key); indexes.push_back(index); }
        else indexes[position->second]=index;
    };
    for(size_t i=0;i<probe.names.size();++i) set(main.identity(probe.names[i]),i);
    // Host tables key imports as NID#library: the eboot's own letters (#c#C) differ per game.
    std::vector<std::optional<std::string>> names;
    for(const auto& name:probe.names) {
        auto key=main.identity(name);
        names.push_back(key ? key->nid+"#"+key->library : name);
    }
    auto imported=[&](const Symbol& symbol)->uint64_t {
        auto position=positions.find(symbol.identity);
        if(position!=positions.end()) return indexes[position->second];
        set(symbol.identity,names.size());
        names.push_back(symbol.identity ? std::optional<std::string>(symbol.identity->nid+"#"+symbol.identity->library) : std::nullopt);
        return names.size()-1;
    };
    struct Export { uint64_t address,kind; };
    std::map<Identity,Export> exports;
    std::map<std::pair<std::string,std::string>,std::vector<Export>> by_nid;
    std::vector<std::array<uint64_t,7>> table;
    Bytes& image=probe.image;
    uint64_t base=(probe.size+65535)&~uint64_t(65535);
    patch_fs_loads(image,main.self.ph,0);
    uint64_t tls_module=2;
    for(const auto& m:modules) {
        const auto loads=load_segments(m.self);
        const uint64_t modsize=memory_size(loads);
        if(Wide(base)+modsize>Wide(image_limit)) fail("linked image exceeds probe limit");
        if(base+modsize<image.size()) fail("module overlaps the image");
        image.resize(base+modsize,0);
        for(auto p:loads) {
            if(p->filesz>p->memsz) fail("invalid module segment size");
            View bytes=span(m.self.elf,p->offset,p->filesz);
            std::copy(bytes.begin(),bytes.end(),image.begin()+base+p->vaddr);
            probe.segments.push_back({base+p->vaddr,p->memsz,p->flags});
        }
        const Segment* tls=find_segment(m.self.ph,7);
        uint64_t module_id=0;
        if(tls) {
            if(tls->filesz>tls->memsz||!mapped(loads,tls->vaddr,tls->memsz)) fail("unsupported module TLS layout");
            module_id=tls_module++;
        }
        for(const auto& r:m.relocs) {
            if(!mapped(loads,r.target,8)) fail("module relocation outside image "+hex(r.target));
            const uint64_t target=r.target+base;
            if(r.kind==8) {
                probe.relocs.push_back({target,0,as_i64(Wide(base)+r.addend),0});
            } else if(r.kind==16||r.kind==1||r.kind==6||r.kind==7) {
                if(r.symbol>=m.symbols.size()) fail("module relocation symbol out of range");
                const auto& symbol=m.symbols[r.symbol];
                if(r.kind==16) {
                    if(!symbol.name.empty()||symbol.type!=3||r.addend||!module_id) fail("unsupported external TLS relocation");
                    store(image,target,module_id);
                } else if(symbol.section) {
                    if(symbol.section==0xfff1) store(image,target,as_u64(Wide(symbol.value)+r.addend));
                    else probe.relocs.push_back({target,0,as_i64(Wide(base)+symbol.value+r.addend),0});
                } else if(!symbol.identity&&symbol.binding==2) {
                    store(image,target,0);  // weak undefined (module_start/stop)
                } else {
                    const uint64_t kind=symbol.type==1 ? 2 : 1;
                    if((kind==1&&r.addend)||r.addend<0||r.addend>=4096) fail("unsupported module import addend");
                    probe.relocs.push_back({target,kind,int64_t(imported(symbol)),r.addend});
                }
            } else {
                fail("unsupported module relocation type "+std::to_string(r.kind));
            }
        }
        for(const auto& s:m.symbols) {
            if(!s.section||(s.binding!=1&&s.binding!=2)||!s.identity||(s.type!=1&&s.type!=2)) continue;
            if(!mapped(loads,s.value,std::max<uint64_t>(1,s.size))) fail("export outside load segments");
            if(exports.count(*s.identity)) fail("ambiguous export");
            const Export entry{base+s.value,s.type==1 ? 2u : 1u};
            exports[*s.identity]=entry;
            by_nid[{s.identity->nid,s.identity->library}].push_back(entry);
        }
        patch_fs_loads(image,m.self.ph,base);
        auto init=m.d.tags.find(12);
        table.push_back({base,modsize,as_u64(Wide(base)+(init==m.d.tags.end() ? 0 : init->second)),
                         tls ? base+tls->vaddr : 0,tls ? tls->memsz : 0,tls ? tls->filesz : 0,module_id});
        base=(base+modsize+65535)&~uint64_t(65535);
    }
    std::vector<std::array<uint64_t,3>> bindings;
    for(size_t i=0;i<keys.size();++i) {
        const auto& key=keys[i];
        if(!key) continue;
        const Export* found=nullptr;
        if(auto entry=exports.find(*key);entry!=exports.end()) found=&entry->second;
        if(!found&&key->library=="libSceLibcInternal") {
            auto candidates=by_nid.find({key->nid,"libc"});
            if(candidates!=by_nid.end()&&candidates->second.size()==1) found=&candidates->second[0];
        }
        if(found) bindings.push_back({indexes[i],found->address,found->kind});
    }
    std::array<uint64_t,4> main_tls{};
    if(const Segment* tls=find_segment(main.self.ph,7)) {
        main_tls={tls->vaddr,tls->filesz,tls->memsz,tls->align};
        if(tls->filesz>tls->memsz||tls->memsz>1024*1024||Wide(tls->vaddr)+tls->filesz>Wide(probe.size)) fail("unsupported eboot TLS layout");
    }
    const auto& procparam=require_segment(main.self.ph,0x61000001);
    Bytes out;
    out.reserve(image.size()+probe.relocs.size()*32+names.size()*128+(1<<16));
    put_text(out,"G3PROBE5",8);
    for(uint64_t value:{uint64_t(image.size()),probe.entry,uint64_t(probe.segments.size()),uint64_t(probe.relocs.size()),
                        uint64_t(names.size()),probe.flags,procparam.vaddr}) put(out,value);
    for(auto value:main_tls) put(out,value);
    put<uint64_t>(out,table.size());
    for(const auto& entry:table) for(auto value:entry) put(out,value);
    put<uint64_t>(out,bindings.size());
    for(const auto& binding:bindings) for(auto value:binding) put(out,value);
    for(const auto& segment:probe.segments) for(auto value:segment) put(out,value);
    for(const auto& name:names) {
        if(!name) fail("import without a name");
        if(name->size()>=128) fail("name too long");
        put_text(out,name->data(),name->size());
        out.resize(out.size()+128-name->size(),0);
    }
    for(const auto& r:probe.relocs) { put(out,r.target); put(out,r.kind); put(out,r.value); put(out,r.addend); }
    out.insert(out.end(),image.begin(),image.end());
    return out;
}

// prepare.py sfo
struct SfoValue { std::optional<uint32_t> number; std::string text; };
std::map<std::string,SfoValue> sfo(View data) {
    if(data.empty()) fail("param.sfo is empty (0 bytes): the game files are incomplete");
    View head=span(data,0,20);
    if(std::memcmp(head.data(),"\0PSF",4)) fail("bad SFO signature");
    const uint32_t keys=read<uint32_t>(head,8),values=read<uint32_t>(head,12),count=read<uint32_t>(head,16);
    std::map<std::string,SfoValue> result;
    for(uint32_t i=0;i<count;++i) {
        View entry=span(data,20+uint64_t(i)*16,16);
        const uint16_t key=read<uint16_t>(entry,0),format=read<uint16_t>(entry,2);
        const uint32_t size=read<uint32_t>(entry,4),offset=read<uint32_t>(entry,12);
        const uint64_t start=uint64_t(keys)+key;
        if(start>=data.size()) fail("bad SFO key");
        auto end=std::find(data.begin()+start,data.end(),uint8_t(0));
        if(end==data.end()) fail("bad SFO key");
        View value=span(data,uint64_t(values)+offset,size);
        SfoValue parsed;
        if(format==0x404) {
            if(size!=4) fail("bad SFO integer");
            parsed.number=read<uint32_t>(value,0);
        } else {
            size_t length=value.size();
            while(length&&value[length-1]==0) --length;
            parsed.text.assign(value.begin(),value.begin()+length);
        }
        result[std::string(data.begin()+start,end)]=parsed;
    }
    return result;
}

// patches.py game_profile: the title ID, else the title without ® and ™.
bool known_game(const std::map<std::string,SfoValue>& values) {
    auto id=values.find("TITLE_ID");
    if(id!=values.end()&&!id->second.number)
        for(auto known:known_titles) if(id->second.text==known) return true;
    auto title=values.find("TITLE");
    if(title==values.end()||title->second.number) return false;
    std::string name=title->second.text;
    for(const char* mark:{"\xc2\xae","\xe2\x84\xa2"})
        for(size_t at;(at=name.find(mark))!=std::string::npos;) name.erase(at,std::strlen(mark));
    return name.find("God of War III")!=std::string::npos;
}

// content_profile.py prepare (SKU full)
Bytes content(const std::map<std::string,SfoValue>& values) {
    Bytes out;
    put_text(out,"G3CONT01",8);
    put<uint32_t>(out,3);
    for(int i=1;i<=4;++i) {
        auto value=values.find("USER_DEFINED_PARAM_"+std::to_string(i));
        if(value!=values.end()&&!value->second.number) fail("invalid user-defined parameter");
        put<uint32_t>(out,value==values.end() ? 0 : *value->second.number);
    }
    return out;
}

// patches.py: the selected graphics patches compiled against this eboot (patches.bin, G3PATCH2).
std::optional<std::string> attribute(const XmlElement& element,const wchar_t* name) {
    auto node=element.GetAttributeNode(name);
    if(!node) return std::nullopt;
    return winrt::to_string(node.Value());
}
std::vector<XmlElement> elements(const winrt::Windows::Data::Xml::Dom::XmlNodeList& list) {
    std::vector<XmlElement> result;
    for(const auto& node:list) result.push_back(node.as<XmlElement>());
    return result;
}
struct Meta { XmlElement element{nullptr}; std::optional<std::string> name,version,elf,enabled,note; };

// patches.py eboot_segments and eboot_image: the PT_LOAD file bytes at their virtual addresses.
std::vector<std::pair<uint64_t,uint64_t>> eboot_segments(View elf) {
    const uint64_t phoff=read<uint64_t>(elf,0x20);
    const uint16_t phentsize=read<uint16_t>(elf,0x36),phnum=read<uint16_t>(elf,0x38);
    std::vector<std::pair<uint64_t,uint64_t>> segments;
    for(uint64_t i=0;i<phnum;++i) {
        View entry=span(elf,add(phoff,i*phentsize),56);
        if(read<uint32_t>(entry,0)==1) segments.emplace_back(read<uint64_t>(entry,16),read<uint64_t>(entry,16)+read<uint64_t>(entry,40));
    }
    return segments;
}
Bytes eboot_image(View elf) {
    const uint64_t phoff=read<uint64_t>(elf,0x20);
    const uint16_t phentsize=read<uint16_t>(elf,0x36),phnum=read<uint16_t>(elf,0x38);
    std::vector<std::array<uint64_t,3>> loads;
    for(uint64_t i=0;i<phnum;++i) {
        View entry=span(elf,add(phoff,i*phentsize),56);
        if(read<uint32_t>(entry,0)==1) loads.push_back({read<uint64_t>(entry,16),read<uint64_t>(entry,8),read<uint64_t>(entry,32)});
    }
    if(loads.empty()) fail("eboot.bin has no loadable segments");
    uint64_t size=0;
    for(const auto& [vaddr,offset,filesz]:loads) size=std::max(size,add(vaddr,filesz));
    if(size>image_limit) fail("eboot image too large");
    Bytes image(size,0);
    for(const auto& [vaddr,offset,filesz]:loads) {
        View bytes=span(elf,offset,filesz);
        std::copy(bytes.begin(),bytes.end(),image.begin()+vaddr);
    }
    return image;
}

std::vector<int> parse_pattern(const std::string& text) {
    std::vector<int> pattern;
    size_t at=0;
    while(true) {
        at=text.find_first_not_of(" \t\r\n",at);
        if(at==std::string::npos) break;
        const size_t end=std::min(text.find_first_of(" \t\r\n",at),text.size());
        const std::string token=text.substr(at,end-at);
        if(token=="??") {
            pattern.push_back(-1);
        } else {
            std::string digits=token;
            if(digits.size()>2&&digits[0]=='0'&&(digits[1]=='x'||digits[1]=='X')) digits=digits.substr(2);
            char* stop=nullptr;
            const unsigned long value=std::strtoul(digits.c_str(),&stop,16);
            if(digits.empty()||*stop||value>255) fail("bad pattern byte '"+token+"'");
            pattern.push_back(int(value));
        }
        at=end;
    }
    return pattern;
}

// Image offsets where the pattern matches, at most two of them.
std::vector<uint64_t> find_pattern(View image,const std::vector<int>& pattern) {
    size_t best_start=0,best_end=0,start=SIZE_MAX;
    for(size_t i=0;i<=pattern.size();++i) {
        const bool fixed=i<pattern.size()&&pattern[i]>=0;
        if(fixed&&start==SIZE_MAX) start=i;
        if(!fixed&&start!=SIZE_MAX) {
            if(i-start>best_end-best_start) { best_start=start; best_end=i; }
            start=SIZE_MAX;
        }
    }
    if(best_end==best_start) fail("pattern without fixed bytes");
    const std::vector<uint8_t> anchor(pattern.begin()+best_start,pattern.begin()+best_end);
    const std::boyer_moore_horspool_searcher searcher(anchor.begin(),anchor.end());
    std::vector<uint64_t> found;
    for(auto at=std::search(image.begin(),image.end(),searcher);at!=image.end()&&found.size()<2;
        at=std::search(at+1,image.end(),searcher)) {
        const uint64_t position=uint64_t(at-image.begin());
        if(position<best_start) continue;
        const uint64_t site=position-best_start;
        if(site+pattern.size()>image.size()) continue;
        bool match=true;
        for(size_t i=0;i<pattern.size()&&match;++i) match=pattern[i]<0||image[site+i]==pattern[i];
        if(match) found.push_back(site);
    }
    return found;
}

// The image offset of a Line's or Target's site: its Pattern (found once) plus At, else its Address.
int64_t locate(View image,const XmlElement& element) {
    auto pattern=attribute(element,L"Pattern");
    if(!pattern||pattern->empty()) {
        auto address=attribute(element,L"Address");
        if(!address) fail("patch site without an address");
        return as_i64(parse_int(*address,true)-eboot_base);
    }
    auto found=find_pattern(image,parse_pattern(*pattern));
    if(found.size()!=1) throw Missing(found.empty() ? "signature not found" : "signature found more than once");
    return as_i64(Wide(found[0])+parse_int(attribute(element,L"At").value_or("0"),true));
}

std::map<std::string,int64_t> resolve_targets(View image,const XmlElement& meta,const std::vector<int64_t>& sites) {
    std::map<std::string,int64_t> targets;
    for(const auto& target:elements(meta.GetElementsByTagName(L"Target"))) {
        const std::string name=attribute(target,L"Name").value_or("");
        if(auto line=attribute(target,L"Line");line&&!line->empty()) {
            Wide index=parse_int(*line,false);
            if(index<0) index+=Wide(sites.size());
            if(index<0||index>=Wide(sites.size())) fail("target line out of range");
            targets[name]=sites[size_t(index)];
            continue;
        }
        int64_t at=locate(image,target);
        if(auto ref=attribute(target,L"Ref");ref&&!ref->empty()) {
            if(at<0) fail("target outside the eboot");
            const int32_t displacement=read<int32_t>(image,uint64_t(at));
            at=as_i64(Wide(at)+4+parse_int(attribute(target,L"Tail").value_or("0"),false)+displacement);
        }
        targets[name]=at;
    }
    return targets;
}

void append_hex(Bytes& out,const std::string& text) {
    std::string digits;
    for(char c:text) if(!std::strchr(" \t\r\n\f\v",c)) digits+=c;
    if(digits.size()%2) fail("bad hex value '"+text+"'");
    for(size_t i=0;i<digits.size();i+=2) {
        auto nibble=[&](char c)->unsigned {
            if(c>='0'&&c<='9') return c-'0';
            if(c>='a'&&c<='f') return c-'a'+10;
            if(c>='A'&&c<='F') return c-'A'+10;
            fail("bad hex value '"+text+"'");
        };
        out.push_back(uint8_t(nibble(digits[i])<<4|nibble(digits[i+1])));
    }
}

// Value bytes with each {rel:NAME:TAIL} replaced by the rel32 to target NAME, measured from the end of the
// instruction (TAIL bytes after the operand).
Bytes fill(std::string value,int64_t at,const std::map<std::string,int64_t>& targets) {
    value.erase(std::remove(value.begin(),value.end(),' '),value.end());
    Bytes out;
    for(size_t i=0;i<value.size();) {
        const size_t open=value.find('{',i),close=open==std::string::npos ? open : value.find('}',open);
        if(close==std::string::npos) { append_hex(out,value.substr(i)); break; }
        append_hex(out,value.substr(i,open-i));
        const std::string inner=value.substr(open+1,close-open-1);
        const size_t first=inner.find(':'),second=first==std::string::npos ? first : inner.find(':',first+1);
        if(second==std::string::npos||inner.find(':',second+1)!=std::string::npos) fail("bad reference {"+inner+"}");
        auto target=targets.find(inner.substr(first+1,second-first-1));
        if(target==targets.end()) fail("unknown target in {"+inner+"}");
        const Wide rel=Wide(target->second)-(Wide(at)+Wide(out.size())+4+parse_int(inner.substr(second+1),false));
        if(rel<INT32_MIN||rel>INT32_MAX) fail("reference out of range in {"+inner+"}");
        put<int32_t>(out,int32_t(rel));
        i=close+1;
    }
    return out;
}

Bytes encode(const XmlElement& line) {
    const auto kind=attribute(line,L"Type"),value=attribute(line,L"Value");
    if(!kind||!value) fail("patch line without a type or value");
    Bytes out;
    if(*kind=="bytes") {
        std::string digits=*value;
        digits.erase(std::remove(digits.begin(),digits.end(),' '),digits.end());
        append_hex(out,digits);
    } else if(*kind=="bytes16"||*kind=="bytes32"||*kind=="bytes64") {
        const unsigned size=unsigned(std::stoul(kind->substr(5)))/8;
        const Wide number=parse_int(*value,true);
        if(number<0||(size<8&&number>=(Wide(1)<<(8*size)))||number>Wide(UINT64_MAX)) fail("int too big to convert");
        const uint64_t bits=uint64_t(number);
        for(unsigned i=0;i<size;++i) out.push_back(uint8_t(bits>>(8*i)));
    } else if(*kind=="float32"||*kind=="float64") {
        char* stop=nullptr;
        const double number=std::strtod(value->c_str(),&stop);
        while(stop&&std::strchr(" \t\r\n",*stop)&&*stop) ++stop;
        if(value->empty()||*stop) fail("could not convert string to float: '"+*value+"'");
        if(*kind=="float64") { put(out,number); }
        else {
            const float single=float(number);
            if(std::isinf(single)&&!std::isinf(number)) fail("float too large to pack with f format");
            put(out,single);
        }
    } else if(*kind=="utf8") {
        put_text(out,value->data(),value->size()+1);
    } else if(*kind=="utf16") {
        const winrt::hstring wide=winrt::to_hstring(*value);
        for(wchar_t c:wide) put<uint16_t>(out,uint16_t(c));
        put<uint16_t>(out,0);
    } else {
        fail("unsupported patch type '"+*kind+"'");
    }
    return out;
}

// patches.py selected_patches: the file's enabled patches for this version, then extra; one resolution
// patch replaces the texture fix.
std::vector<std::string> selected_patches(const std::vector<Meta>& metas,const std::string& version,const std::string& extra) {
    std::vector<const Meta*> current;
    for(const auto& meta:metas) if(meta.version==version) current.push_back(&meta);
    auto contains=[](const std::vector<std::string>& list,const std::string& name) {
        return std::find(list.begin(),list.end(),name)!=list.end(); };
    std::vector<std::string> names;
    for(auto meta:current) {
        std::string enabled=meta->enabled.value_or("false");
        std::transform(enabled.begin(),enabled.end(),enabled.begin(),[](char c) { return char(std::tolower(uint8_t(c))); });
        if(enabled=="true") names.push_back(meta->name.value_or(""));
    }
    std::vector<std::string> more;
    for(size_t at=0;at<=extra.size();) {
        const size_t end=std::min(extra.find(';',at),extra.size());
        std::string name=extra.substr(at,end-at);
        name.erase(0,name.find_first_not_of(" \t\r\n"));
        name.erase(name.find_last_not_of(" \t\r\n")+1);
        if(!name.empty()&&!contains(names,name)) more.push_back(name);
        at=end+1;
    }
    names.insert(names.end(),more.begin(),more.end());
    more.clear();
    for(auto meta:current) if(meta->name=="Bug Fix - Startup Black Screen"&&!contains(names,*meta->name)) more.push_back(*meta->name);
    names.insert(names.end(),more.begin(),more.end());
    std::vector<std::string> exclusive;
    for(const auto& name:names) if(name.rfind("Resolution Patch",0)==0) exclusive.push_back(name);
    if(exclusive.size()>1) fail("choose one resolution patch, not several");
    std::vector<std::string> result;
    for(const auto& name:names) if(exclusive.empty()||name!="Bug Fix - Texture Corruption Fix") result.push_back(name);
    return result;
}

// patches.py patch_requirements: the direct memory the selected patches' notes ask for ("Requires setting DMEM
// to N MB", on top of the retail pool), the retail pool when none does.
constexpr int dmem_retail_mb=5056;
int dmem_requirement(const std::vector<Meta>& metas,const std::vector<std::string>& names,const std::string& version) {
    int dmem=0;
    for(const auto& meta:metas) {
        if(!meta.name||std::find(names.begin(),names.end(),*meta.name)==names.end()||meta.version!=version) continue;
        const std::string note=meta.note.value_or("");
        for(size_t at=note.find("DMEM to ");at!=std::string::npos;at=note.find("DMEM to ",at+1)) {
            size_t end=at+8;
            while(end<note.size()&&std::isdigit(uint8_t(note[end]))) ++end;
            if(end==at+8||note.compare(end,3," MB")!=0) continue;
            dmem=std::max(dmem,dmem_retail_mb+std::stoi(note.substr(at+8,end-at-8)));
            break;
        }
    }
    return dmem ? dmem : dmem_retail_mb;
}

Bytes compile_patches(const std::vector<Meta>& metas,const std::vector<std::string>& names,View image,
                      const std::vector<std::pair<uint64_t,uint64_t>>& segments,const char* resolution) {
    std::map<std::string,const Meta*> found;
    for(const auto& meta:metas)
        if(meta.name&&std::find(names.begin(),names.end(),*meta.name)!=names.end()&&meta.elf.value_or("eboot.bin")=="eboot.bin")
            found[*meta.name]=&meta;
    for(const auto& name:names) if(!found.count(name)) fail("patch not found: "+name);
    std::vector<std::pair<int64_t,Bytes>> writes;
    for(const auto& name:names) {
        const XmlElement& meta=found[name]->element;
        const auto lines=elements(meta.GetElementsByTagName(L"Line"));
        std::vector<int64_t> sites;
        std::map<std::string,int64_t> targets;
        try {
            for(const auto& line:lines) sites.push_back(locate(image,line));
            targets=resolve_targets(image,meta,sites);
        } catch(const Missing& error) {
            fail(std::string(resolution)+": required graphics patch unavailable: "+name+": "+error.what());
        }
        for(size_t i=0;i<lines.size();++i) {
            if(auto target=attribute(lines[i],L"Target");target&&!target->empty()) {
                auto site=targets.find(*target);
                if(site==targets.end()) fail(name+": unknown target "+*target);
                sites[i]=site->second;
            }
        }
        for(size_t i=0;i<lines.size();++i) {
            const int64_t offset=sites[i];
            const std::string value=attribute(lines[i],L"Value").value_or("");
            Bytes data=value.find('{')!=std::string::npos ? fill(value,offset,targets) : encode(lines[i]);
            if(!std::any_of(segments.begin(),segments.end(),[&](const auto& s) {
                   return Wide(s.first)<=offset&&Wide(offset)+data.size()<=Wide(s.second); }))
                fail(name+": address "+hex(uint64_t(offset+eboot_base))+" is outside the eboot");
            writes.emplace_back(offset,std::move(data));
        }
    }
    Bytes out;
    put_text(out,"G3PATCH2",8);
    put<uint64_t>(out,eboot_base);
    put<uint64_t>(out,writes.size());
    for(const auto& [offset,data]:writes) {
        put<uint64_t>(out,as_u64(offset));
        put<uint64_t>(out,data.size());
        out.insert(out.end(),data.begin(),data.end());
    }
    return out;
}
}

std::string CheckTitle(const Bytes& data) {
    try {
        if(known_game(sfo(data))) return {};
        return "is not God of War III Remastered.";
    } catch(const std::exception& error) {
        return std::string("has an unreadable sce_sys\\param.sfo (")+error.what()+").";
    }
}

Files Prepare(const Dump& dump,const std::string& patch_xml) {
    const auto values=sfo(dump.sfo);
    if(!known_game(values)) fail("No graphics patch profile for the prepared game");
    if(dump.eboot.empty()) fail("eboot.bin is empty (0 bytes): the game files are incomplete");
    if(dump.libc.empty()) fail("libc.prx is empty (0 bytes): the game files are incomplete");
    if(dump.fios.empty()) fail("libSceFios2.prx is empty (0 bytes): the game files are incomplete");
    Files files;
    const Self eboot=parse_self(dump.eboot);
    Probe probe=prepare_eboot(eboot,init_env_is_ret(dump.libc));
    std::vector<Module> modules;
    modules.push_back(load_module(dump.libc));
    modules.push_back(load_module(dump.fios));
    files.boot=link(load_module(dump.eboot),probe,modules);
    files.content=content(values);
    winrt::Windows::Data::Xml::Dom::XmlDocument document;
    std::string xml=patch_xml;
    if(xml.rfind("\xef\xbb\xbf",0)==0) xml.erase(0,3);
    try { document.LoadXml(winrt::to_hstring(xml)); }
    catch(const winrt::hresult_error& error) { fail("the patch file is not valid XML: "+winrt::to_string(error.message())); }
    std::vector<Meta> metas;
    for(const auto& element:elements(document.GetElementsByTagName(L"Metadata")))
        metas.push_back({element,attribute(element,L"Name"),attribute(element,L"AppVer"),attribute(element,L"AppElf"),
                         attribute(element,L"isEnabled"),attribute(element,L"Note")});
    const auto segments=eboot_segments(eboot.elf);
    const Bytes image=eboot_image(eboot.elf);
    for(const char* name:Resolutions) {
        const std::string resolution=name;
        const auto names=selected_patches(metas,"01.02",resolution=="native" ? "" : "Resolution Patch - "+resolution);
        files.resolutions.push_back({resolution,compile_patches(metas,names,image,segments,name),dmem_requirement(metas,names,"01.02")});
    }
    return files;
}

uint64_t Fingerprint(const Dump& dump,const std::string& patch_xml) {
    uint64_t hash=0xcbf29ce484222325ull;
    auto mix=[&](const uint8_t* data,size_t size) {
        const uint64_t length=size;
        for(unsigned i=0;i<8;++i) { hash^=uint8_t(length>>(8*i)); hash*=0x100000001b3ull; }
        for(size_t i=0;i<size;++i) { hash^=data[i]; hash*=0x100000001b3ull; }
    };
    for(const Bytes* file:{&dump.eboot,&dump.libc,&dump.fios,&dump.sfo}) mix(file->data(),file->size());
    mix(reinterpret_cast<const uint8_t*>(patch_xml.data()),patch_xml.size());
    return hash;
}
}
