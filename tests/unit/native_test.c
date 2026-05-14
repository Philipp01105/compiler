#include "native/encoder.h"
#include "native/linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"native test failed at line %d: %s\n",__LINE__,#x);return 0;}}while(0)
static uint64_t read_uint(const NativeBuffer *b,size_t at,size_t size) {
    if(at>b->size||size>b->size-at)return UINT64_MAX;
    uint64_t v=0;for(size_t n=0;n<size;++n)v|=(uint64_t)b->data[at+n]<<(n*8);return v;
}
static int encoding_case(X64Instruction in,const unsigned char *expected,size_t size) {
    NativeObject object={0};CHECK(native_encode(&object,&in));
    CHECK(object.sections[0].size==size && !memcmp(object.sections[0].data,expected,size));
    native_object_free(&object);return 1;
}
static int encoder_tests(void) {
    const unsigned char sib[]={0x4f,0x8b,0x54,0xec,0x80};
    CHECK(encoding_case(x64_instruction2(X64_OP_MOV,X64_WIDTH_QWORD,x64_register("r10"),
        x64_indexed_memory(X64_WIDTH_QWORD,"r12","r13",8,-128)),sib,sizeof(sib)));
    const unsigned char low[]={0x40,0x8a,0x38};
    CHECK(encoding_case(x64_instruction2(X64_OP_MOV,X64_WIDTH_BYTE,x64_register("dil"),x64_memory(X64_WIDTH_BYTE,"rax",0)),low,sizeof(low)));
    const unsigned char disp[]={0x49,0x89,0x85,0x80,0,0,0};
    CHECK(encoding_case(x64_instruction2(X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"r13",128),x64_register("rax")),disp,sizeof(disp)));
    const unsigned char sse[]={0xf2,0x45,0x0f,0x58,0xca};
    CHECK(encoding_case(x64_instruction2(X64_OP_ADDSD,X64_WIDTH_NONE,x64_register("xmm9"),x64_register("xmm10")),sse,sizeof(sse)));
    const unsigned char integer[]={0x48,0x69,0xc9,4,0,0,0};
    CHECK(encoding_case(x64_instruction2(X64_OP_IMUL,X64_WIDTH_QWORD,x64_register("rcx"),x64_immediate(4)),integer,sizeof(integer)));
    NativeObject object={0};
    X64Instruction invalid=x64_instruction2(X64_OP_ADDSS,X64_WIDTH_NONE,x64_register("rax"),x64_register("rbx"));
    CHECK(!native_encode(&object,&invalid)&&object.failed&&object.sections[0].size==0);native_object_free(&object);
    memset(&object,0,sizeof(object));invalid=x64_instruction2(X64_OP_MOV,X64_WIDTH_QWORD,x64_register("nonsense"),x64_immediate(0));
    CHECK(!native_encode(&object,&invalid)&&object.sections[0].size==0);native_object_free(&object);
    memset(&object,0,sizeof(object));invalid=x64_instruction2(X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rbp",INT64_MAX));
    CHECK(!native_encode(&object,&invalid)&&object.sections[0].size==0);native_object_free(&object);
    return 1;
}
static int make_object(NativeObject *o) {
    CHECK(native_define(o,"main",1,1));X64Instruction ret=x64_instruction0(X64_OP_RET,X64_WIDTH_NONE);CHECK(native_encode(o,&ret));
    o->section=NATIVE_RODATA;CHECK(native_define(o,".Lpayload",0,0));CHECK(native_reference(o,"main",NATIVE_ADDR64,0,0));CHECK(native_uint(o,0,8));
    return 1;
}
static size_t pe_offset(const NativeBuffer *b,uint64_t rva) {
    size_t table=0x98+240;
    for(size_t n=0;n<5;++n){size_t h=table+n*40;uint64_t start=read_uint(b,h+12,4),size=read_uint(b,h+8,4);
        if(rva>=start && rva-start<size)return (size_t)(read_uint(b,h+20,4)+rva-start);}
    return SIZE_MAX;
}
static int binary_tests(TargetFormat target) {
    NativeObject o={0};NativeBuffer b={0};CHECK(make_object(&o));CHECK(native_write_object(&o,target,&b));
    if(target==TARGET_ELF){CHECK(!memcmp(b.data,"\177ELF",4));CHECK(read_uint(&b,16,2)==1);CHECK(read_uint(&b,18,2)==62);
        size_t sections=(size_t)read_uint(&b,40,8);CHECK(sections+11*64<=b.size);
        CHECK(read_uint(&b,sections+5*64+4,4)==4);CHECK(read_uint(&b,sections+7*64+4,4)==2);
    }else{CHECK(read_uint(&b,0,2)==0x8664);CHECK(read_uint(&b,2,2)==3);CHECK(read_uint(&b,4,4)==0);
        size_t reloc=(size_t)read_uint(&b,20+40+24,4);CHECK(read_uint(&b,reloc+8,2)==1);}
    free(b.data);native_object_free(&o);memset(&b,0,sizeof(b));memset(&o,0,sizeof(o));CHECK(make_object(&o));
    CHECK(native_link_executable(&o,target,&b));
    if(target==TARGET_ELF) {
        CHECK(read_uint(&b,16,2)==2);CHECK(read_uint(&b,56,2)==7);
        CHECK(read_uint(&b,24,8)>=0x401000);uint64_t dyn=0,dyn_size=0,rw=0,rw_size=0;
        for(size_t n=0;n<7;++n){size_t p=64+n*56;uint64_t kind=read_uint(&b,p,4),flags=read_uint(&b,p+4,4),off=read_uint(&b,p+8,8),size=read_uint(&b,p+32,8);
            CHECK(off+size<=b.size);CHECK(!(flags==7));
            if(kind==2){dyn=off;dyn_size=size;}if(kind==1&&flags==6){rw=read_uint(&b,p+16,8);rw_size=size;}}
        CHECK(dyn&&dyn_size);uint64_t rela=0,rela_size=0,symtab=0,strtab=0,str_size=0;
        for(size_t n=(size_t)dyn;n<dyn+dyn_size;n+=16){uint64_t tag=read_uint(&b,n,8),value=read_uint(&b,n+8,8);
            if(tag==5)strtab=value-0x400000;if(tag==10)str_size=value;if(tag==6)symtab=value-0x400000;
            if(tag==7)rela=value-0x400000;if(tag==8)rela_size=value;}
        CHECK(rela_size&&rela+rela_size<=b.size&&strtab+str_size<=b.size);
        for(size_t n=(size_t)rela;n<rela+rela_size;n+=24){uint64_t cell=read_uint(&b,n,8),info=read_uint(&b,n+8,8);
            CHECK((info&UINT32_MAX)==6);CHECK(cell>=rw&&cell+8<=rw+rw_size);
            uint64_t symbol=symtab+(info>>32)*24;CHECK(symbol+24<=b.size);CHECK(read_uint(&b,(size_t)symbol,4)<str_size);}
        uint64_t payload=read_uint(&b,(size_t)(read_uint(&b,64+3*56+8,8)),8);CHECK(payload==0x401000);
    } else {
        CHECK(read_uint(&b,0,2)==0x5a4d);CHECK(read_uint(&b,0x80,4)==0x4550);CHECK(read_uint(&b,0x98,2)==0x20b);
        size_t dir=pe_offset(&b,read_uint(&b,0x98+120,4));CHECK(dir!=SIZE_MAX);
        size_t lookup=pe_offset(&b,read_uint(&b,dir,4)),iat=pe_offset(&b,read_uint(&b,dir+16,4));CHECK(lookup!=SIZE_MAX&&iat!=SIZE_MAX);
        size_t count=0;while(read_uint(&b,lookup+count*8,8)){
            uint64_t name=read_uint(&b,lookup+count*8,8);CHECK(read_uint(&b,iat+count*8,8)==name);
            size_t hint=pe_offset(&b,name);CHECK(hint!=SIZE_MAX&&hint+2<b.size);
            CHECK(memchr(b.data+hint+2,0,b.size-hint-2));CHECK(++count<100);}
        CHECK(count>10);size_t reloc=pe_offset(&b,read_uint(&b,0x98+112+5*8,4));CHECK(reloc!=SIZE_MAX);
        CHECK(read_uint(&b,reloc+8,2)==0xa000);size_t payload=pe_offset(&b,0x2000);CHECK(payload!=SIZE_MAX);
        CHECK(read_uint(&b,payload,8)==UINT64_C(0x140001000));
    }
    free(b.data);native_object_free(&o);
    memset(&o,0,sizeof(o));memset(&b,0,sizeof(b));CHECK(make_object(&o));CHECK(native_symbol(&o,"unknown_external")!=SIZE_MAX);
    CHECK(!native_link_executable(&o,target,&b)&&strstr(o.error,"unknown_external"));free(b.data);native_object_free(&o);return 1;
}
int main(void){return encoder_tests()&&binary_tests(TARGET_ELF)&&binary_tests(TARGET_COFF)?0:1;}
