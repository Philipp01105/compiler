#include "linker.h"
#include "encoder.h"
#include "../../runtime/native_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

typedef struct { size_t symbol, iat; const char *export_name; } Import;
static size_t align(size_t n,size_t a){return (n+a-1)&~(a-1);}
static int relocate(NativeObject *object,const uint64_t addresses[3]) {
    for(size_t n=0;n<object->relocation_count;++n) {
        const NativeRelocation *r=&object->relocations[n];const NativeSymbol *s=&object->symbols[r->symbol];
        size_t width=r->kind==NATIVE_ADDR64?8:4;
        if(!s->defined||s->section<0||s->section>=NATIVE_SECTION_COUNT||
           r->offset>object->sections[r->section].size||width>object->sections[r->section].size-r->offset){native_error(object,"Invalid or unresolved executable relocation");return 0;}
        uint64_t value=addresses[s->section]+s->offset+(uint64_t)r->addend;
        if(r->kind!=NATIVE_ADDR64) {
            int64_t relative=(int64_t)(value-(addresses[r->section]+r->offset));
            if(relative<INT32_MIN||relative>INT32_MAX){native_error(object,"Executable relocation exceeds signed 32-bit range");return 0;}
        }
        native_buffer_patch(&object->sections[r->section],r->offset,value-(r->kind==NATIVE_ADDR64?0:addresses[r->section]+r->offset),width);
    }return 1;
}
static int add_imports(NativeObject *object,TargetFormat target,Import **result,size_t *count) {
    size_t maximum=object->symbol_count;
    Import *imports=calloc(maximum?maximum:1,sizeof(*imports));if(!imports){native_error(object,"Out of memory building native imports");return 0;}
    *count=0;object->section=NATIVE_DATA;
    if(!native_buffer_align(&object->sections[NATIVE_DATA],8))goto failure;
    for(size_t n=0;n<maximum;++n)if(!object->symbols[n].defined) {
        const char *name=native_runtime_import(object->symbols[n].name,target);
        if(!name){char message[256];snprintf(message,sizeof(message),"Internal linker cannot resolve symbol '%s'",object->symbols[n].name);native_error(object,message);goto failure;}
        Import *import=&imports[(*count)++];import->symbol=n;import->export_name=name;import->iat=object->sections[NATIVE_DATA].size;
        char slot[64];snprintf(slot,sizeof(slot),".Lnative_iat_%zu",n);
        object->section=NATIVE_DATA;if(!native_define(object,slot,0,0)||!native_uint(object,0,8))goto failure;
        char symbol[512];snprintf(symbol,sizeof(symbol),"%s",object->symbols[n].name);
        object->section=NATIVE_TEXT;if(!native_define(object,symbol,1,1))goto failure;
        X64Operand memory=x64_rip_memory(X64_WIDTH_QWORD,slot,0);memory.has_symbol_suffix=0;
        X64Instruction jump=x64_instruction1(X64_OP_JMP,X64_WIDTH_NONE,memory);
        if(!native_encode(object,&jump))goto failure;
    }
    object->section=NATIVE_DATA;if(!native_uint(object,0,8))goto failure;
    *result=imports;return 1;
failure:free(imports);if(!object->failed)native_error(object,"Out of memory building imports");return 0;
}
#define UINT(b,v,n) do {if(!native_buffer_uint((b),(uint64_t)(v),(n)))goto failure;}while(0)
#define BYTES(b,p,n) do {if(!native_buffer_bytes((b),(p),(n)))goto failure;}while(0)
#define PAD(b,a) do {if(!native_buffer_align((b),(a)))goto failure;}while(0)
static void patch(NativeBuffer *b,size_t offset,uint64_t value,size_t width){native_buffer_patch(b,offset,value,width);}
static void pe_section(NativeBuffer *out,size_t header,const char *name,size_t size,size_t rva,size_t raw,size_t flags) {
    memcpy(out->data+header,name,strlen(name));patch(out,header+8,size,4);patch(out,header+12,rva,4);
    patch(out,header+16,align(size,512),4);patch(out,header+20,raw,4);patch(out,header+36,flags,4);
}
static int write_pe(NativeObject *object,const Import *imports,size_t count,NativeBuffer *out) {
    NativeBuffer idata={0},base_reloc={0};size_t rva[5],raw[5],sizes[5];
    sizes[0]=object->sections[0].size;sizes[1]=object->sections[1].size;sizes[2]=object->sections[2].size;
    rva[0]=4096;raw[0]=1024;
    for(size_t n=1;n<4;++n){rva[n]=align(rva[n-1]+sizes[n-1],4096);raw[n]=raw[n-1]+align(sizes[n-1],512);}
    BYTES(&idata,NULL,40);size_t lookup=idata.size;BYTES(&idata,NULL,(count+1)*8);
    size_t dll=idata.size;BYTES(&idata,"msvcrt.dll",11);
    for(size_t n=0;n<count;++n) {
        PAD(&idata,2);size_t hint=idata.size;UINT(&idata,0,2);BYTES(&idata,imports[n].export_name,strlen(imports[n].export_name)+1);
        patch(&idata,lookup+n*8,rva[3]+hint,8);patch(&object->sections[NATIVE_DATA],imports[n].iat,rva[3]+hint,8);
    }
    patch(&idata,0,rva[3]+lookup,4);patch(&idata,12,rva[3]+dll,4);
    patch(&idata,16,rva[2]+(count?imports[0].iat:0),4);
    sizes[3]=idata.size;rva[4]=align(rva[3]+sizes[3],4096);raw[4]=raw[3]+align(sizes[3],512);
    /* DIR64 blocks retain relocations for enum payload pointers under ASLR. */
    for(size_t section=0;section<3;++section) {
        for(size_t page=0;page<align(sizes[section],4096);page+=4096) {
            size_t begin=base_reloc.size;UINT(&base_reloc,rva[section]+page,4);UINT(&base_reloc,0,4);
            for(size_t n=0;n<object->relocation_count;++n) {
                const NativeRelocation *rel=&object->relocations[n];
                if((size_t)rel->section==section&&rel->kind==NATIVE_ADDR64&&rel->offset>=page&&rel->offset<page+4096)
                    UINT(&base_reloc,0xa000u+(rel->offset-page),2);
            }
            if(base_reloc.size==begin+8)base_reloc.size=begin;
            else {PAD(&base_reloc,4);patch(&base_reloc,begin+4,base_reloc.size-begin,4);}
        }
    }
    if(!base_reloc.size){UINT(&base_reloc,0,4);UINT(&base_reloc,12,4);UINT(&base_reloc,0,4);}
    sizes[4]=base_reloc.size;
    uint64_t addresses[3]={UINT64_C(0x140000000)+rva[0],UINT64_C(0x140000000)+rva[1],UINT64_C(0x140000000)+rva[2]};
    if(!relocate(object,addresses))goto failure;
    if(raw[4]+align(sizes[4],512)>UINT32_MAX || rva[4]+align(sizes[4],4096)>UINT32_MAX){native_error(object,"PE executable exceeds format limits");goto failure;}
    BYTES(out,NULL,1024);patch(out,0,0x5a4d,2);patch(out,0x3c,0x80,4);
    patch(out,0x80,0x4550,4);patch(out,0x84,0x8664,2);patch(out,0x86,5,2);patch(out,0x94,240,2);patch(out,0x96,0x22,2);
    size_t opt=0x98;patch(out,opt,0x20b,2);patch(out,opt+2,1,1);
    patch(out,opt+4,align(sizes[0],512),4);patch(out,opt+8,raw[4]+align(sizes[4],512)-raw[1],4);
    size_t entry=native_symbol(object,"__dmm_entry");patch(out,opt+16,rva[0]+object->symbols[entry].offset,4);patch(out,opt+20,rva[0],4);
    patch(out,opt+24,UINT64_C(0x140000000),8);patch(out,opt+32,4096,4);patch(out,opt+36,512,4);
    patch(out,opt+40,6,2);patch(out,opt+48,6,2);patch(out,opt+56,align(rva[4]+sizes[4],4096),4);patch(out,opt+60,1024,4);
    patch(out,opt+68,3,2);patch(out,opt+70,0x160,2);patch(out,opt+72,8*1024*1024,8);patch(out,opt+80,4096,8);
    patch(out,opt+88,1024*1024,8);patch(out,opt+96,4096,8);patch(out,opt+108,16,4);
    patch(out,opt+112+8,rva[3],4);patch(out,opt+112+12,40,4);
    patch(out,opt+112+5*8,rva[4],4);patch(out,opt+116+5*8,sizes[4],4);
    patch(out,opt+112+12*8,rva[2]+(count?imports[0].iat:0),4);patch(out,opt+116+12*8,(count+1)*8,4);
    const char *const names[]={".text",".rdata",".data",".idata",".reloc"};
    const size_t flags[]={0x60000020,0x40000040,0xc0000040,0x40000040,0x42000040};
    for(size_t n=0;n<5;++n)pe_section(out,opt+240+n*40,names[n],sizes[n],rva[n],raw[n],flags[n]);
    for(size_t n=0;n<3;++n){BYTES(out,object->sections[n].data,sizes[n]);PAD(out,512);}
    BYTES(out,idata.data,idata.size);PAD(out,512);BYTES(out,base_reloc.data,base_reloc.size);PAD(out,512);
    free(idata.data);free(base_reloc.data);return 1;
failure:free(idata.data);free(base_reloc.data);return 0;
}
static void phdr(NativeBuffer *out,size_t index,uint64_t type,uint64_t flags,uint64_t offset,uint64_t size,uint64_t alignment) {
    size_t p=64+index*56;patch(out,p,type,4);patch(out,p+4,flags,4);patch(out,p+8,offset,8);
    patch(out,p+16,UINT64_C(0x400000)+offset,8);patch(out,p+24,UINT64_C(0x400000)+offset,8);
    patch(out,p+32,size,8);patch(out,p+40,size,8);patch(out,p+48,alignment,8);
}
static int write_elf(NativeObject *object,const Import *imports,size_t count,NativeBuffer *out) {
    NativeBuffer strings={0},symbols={0},hash={0},relas={0},dynamic={0};
    const uint64_t base=0x400000;size_t offsets[3]={4096,0,0};
    offsets[1]=align(offsets[0]+object->sections[0].size,4096);
    size_t metadata=align(offsets[1]+object->sections[1].size,8);
    UINT(&strings,0,1);size_t library=strings.size;BYTES(&strings,"libc.so.6",10);
    BYTES(&symbols,NULL,24);
    for(size_t n=0;n<count;++n) {
        size_t name=strings.size;BYTES(&strings,imports[n].export_name,strlen(imports[n].export_name)+1);
        UINT(&symbols,name,4);UINT(&symbols,0x12,1);UINT(&symbols,0,1);UINT(&symbols,0,2);UINT(&symbols,0,8);UINT(&symbols,0,8);
    }
    UINT(&hash,1,4);UINT(&hash,count+1,4);UINT(&hash,0,4);BYTES(&hash,NULL,(count+1)*4);
    size_t str_offset=metadata,sym_offset=align(str_offset+strings.size,8),hash_offset=sym_offset+symbols.size;
    size_t rela_offset=align(hash_offset+hash.size,8);
    offsets[2]=align(rela_offset+count*24,4096);
    uint64_t addresses[3]={base+offsets[0],base+offsets[1],base+offsets[2]};
    for(size_t n=0;n<count;++n) {
        UINT(&relas,addresses[2]+imports[n].iat,8);UINT(&relas,((uint64_t)(n+1)<<32)|6u,8);UINT(&relas,0,8);
    }
    size_t dyn_offset=align(offsets[2]+object->sections[2].size,8);
#define DYNAMIC(tag,value) do {UINT(&dynamic,(tag),8);UINT(&dynamic,(value),8);}while(0)
    DYNAMIC(1,library);DYNAMIC(5,base+str_offset);DYNAMIC(6,base+sym_offset);DYNAMIC(10,strings.size);DYNAMIC(11,24);
    DYNAMIC(4,base+hash_offset);DYNAMIC(7,base+rela_offset);DYNAMIC(8,relas.size);DYNAMIC(9,24);DYNAMIC(0,0);
#undef DYNAMIC
    if(!relocate(object,addresses))goto failure;
    BYTES(out,NULL,4096);memcpy(out->data,"\177ELF\2\1\1",7);
    patch(out,16,2,2);patch(out,18,62,2);patch(out,20,1,4);
    size_t entry=native_symbol(object,"__dmm_entry");patch(out,24,addresses[0]+object->symbols[entry].offset,8);
    patch(out,32,64,8);patch(out,52,64,2);patch(out,54,56,2);patch(out,56,7,2);
    const char interpreter[]="/lib64/ld-linux-x86-64.so.2";size_t interp=64+7*56;
    memcpy(out->data+interp,interpreter,sizeof(interpreter));
    phdr(out,0,6,4,64,7*56,8);phdr(out,1,3,4,interp,sizeof(interpreter),1);
    phdr(out,2,1,5,0,offsets[0]+object->sections[0].size,4096);
    phdr(out,3,1,4,offsets[1],rela_offset+relas.size-offsets[1],4096);
    phdr(out,4,1,6,offsets[2],dyn_offset+dynamic.size-offsets[2],4096);
    phdr(out,5,2,6,dyn_offset,dynamic.size,8);phdr(out,6,0x6474e551,6,0,0,16);
    BYTES(out,object->sections[0].data,object->sections[0].size);PAD(out,4096);
    BYTES(out,object->sections[1].data,object->sections[1].size);PAD(out,8);
    BYTES(out,strings.data,strings.size);PAD(out,8);BYTES(out,symbols.data,symbols.size);BYTES(out,hash.data,hash.size);PAD(out,8);
    BYTES(out,relas.data,relas.size);PAD(out,4096);BYTES(out,object->sections[2].data,object->sections[2].size);PAD(out,8);BYTES(out,dynamic.data,dynamic.size);
    free(strings.data);free(symbols.data);free(hash.data);free(relas.data);free(dynamic.data);return 1;
failure:free(strings.data);free(symbols.data);free(hash.data);free(relas.data);free(dynamic.data);return 0;
}
int native_link_executable(NativeObject *object,TargetFormat target,NativeBuffer *output) {
    if (!native_validate(object)) return 0;
    Import *imports=NULL;size_t count=0;
    if(!native_runtime_emit(object,target)||!add_imports(object,target,&imports,&count))return 0;
    int success=target==TARGET_COFF?write_pe(object,imports,count,output):write_elf(object,imports,count,output);
    free(imports);if(!success&&!object->failed)native_error(object,"Out of memory writing executable");return success;
}
