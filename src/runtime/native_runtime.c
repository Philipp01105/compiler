/* Native runtime lowering lives here, independently of language IR emission. */
#include "native_runtime.h"
#include "native/encoder.h"
#include <stdio.h>
#include <string.h>

typedef struct { NativeObject *object; TargetFormat target; const char *function; } Runtime;
static const char *arg(Runtime *r, int n) {
    static const char *const win[]={"rcx","rdx","r8","r9"};
    static const char *const unix_args[]={"rdi","rsi","rdx","rcx","r8","r9"};
    return r->target==TARGET_COFF?win[n]:unix_args[n];
}
static void op0(Runtime *r,X64Opcode op) { (void)native_encode(r->object,&(X64Instruction){.opcode=op}); }
static void op1(Runtime *r,X64Opcode op,X64Operand a) {
    X64Instruction in=x64_instruction1(op,X64_WIDTH_NONE,a);(void)native_encode(r->object,&in);
}
static void op2(Runtime *r,X64Opcode op,X64Width width,X64Operand a,X64Operand b) {
    X64Instruction in=x64_instruction2(op,width,a,b);(void)native_encode(r->object,&in);
}
static void mov(Runtime *r,const char *dst,const char *src) {op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(dst),x64_register(src));}
static void imm(Runtime *r,const char *dst,long long value) {op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(dst),x64_immediate(value));}
static void stack(Runtime *r,X64Opcode op,long long bytes) {op2(r,op,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(bytes));}
static void store(Runtime *r,const char *src,int offset) {op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rbp",-offset),x64_register(src));}
static void load(Runtime *r,const char *dst,int offset) {op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(dst),x64_memory(X64_WIDTH_QWORD,"rbp",-offset));}
static void address(Runtime *r,const char *dst,int offset) {op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register(dst),x64_memory(X64_WIDTH_NONE,"rbp",-offset));}
static void data_address(Runtime *r,const char *dst,const char *symbol) {
    X64Operand mem=x64_rip_memory(X64_WIDTH_NONE,symbol,0);mem.has_symbol_suffix=0;
    op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register(dst),mem);
}
static void call(Runtime *r,const char *symbol) {
    char import[128];
    if (strncmp(symbol,"__dmm_",6) && strcmp(symbol,"snprintf")) {
        snprintf(import,sizeof(import),"__dmm_libc_%s",symbol);symbol=import;
    }
    if(r->target==TARGET_COFF)stack(r,X64_OP_SUB,32);
    else op2(r,X64_OP_XOR,X64_WIDTH_DWORD,x64_register("eax"),x64_register("eax"));
    op1(r,X64_OP_CALL,x64_label(symbol));
    if(r->target==TARGET_COFF)stack(r,X64_OP_ADD,32);
}
static void label_name(Runtime *r,char *out,size_t size,const char *suffix) {snprintf(out,size,".Lnative_%s_%s",r->function,suffix);}
static void label(Runtime *r,const char *suffix) {char name[256];label_name(r,name,sizeof(name),suffix);(void)native_define(r->object,name,0,0);}
static void branch(Runtime *r,X64Opcode op,const char *suffix) {char name[256];label_name(r,name,sizeof(name),suffix);op1(r,op,x64_label(name));}
static void test(Runtime *r,const char *value) {op2(r,X64_OP_TEST,X64_WIDTH_QWORD,x64_register(value),x64_register(value));}
static void begin(Runtime *r,const char *name,int frame) {
    r->function=name;(void)native_define(r->object,name,1,1);
    op1(r,X64_OP_PUSH,x64_register("rbp"));mov(r,"rbp","rsp");if(frame)stack(r,X64_OP_SUB,frame);
}
static void end(Runtime *r) {mov(r,"rsp","rbp");op1(r,X64_OP_POP,x64_register("rbp"));op0(r,X64_OP_RET);}
static void nullsafe(Runtime *r,int n) {
    char suffix[32];snprintf(suffix,sizeof(suffix),"nonnull%d",n);test(r,arg(r,n));branch(r,X64_OP_JNE,suffix);
    data_address(r,arg(r,n),".Lnative_empty");label(r,suffix);
}
static void emit_alias(Runtime *r,const char *name,const char *import,int null_mask) {
    r->function=name;(void)native_define(r->object,name,1,1);
    for(int n=0;n<2;++n)if(null_mask&(1<<n))nullsafe(r,n);
    char symbol[128];snprintf(symbol,sizeof(symbol),"__dmm_libc_%s",import);
    op1(r,X64_OP_JMP,x64_label(symbol));
}
static void scan(Runtime *r,const char *name,int character) {
    begin(r,name,16);imm(r,"rax",0);store(r,"rax",8);
    data_address(r,arg(r,0),character?".Lnative_char":".Lnative_integer");address(r,arg(r,1),8);call(r,"scanf");
    load(r,"rax",8);if(character)op2(r,X64_OP_MOVZX,X64_WIDTH_QWORD,x64_register("rax"),x64_register("al"));end(r);
}
static void scan_string(Runtime *r) {
    begin(r,"__dmm_rt_scan_string",16);
    data_address(r,arg(r,0),".Lnative_scan_string");data_address(r,arg(r,1),".Lnative_buffer");call(r,"scanf");
    op2(r,X64_OP_CMP,X64_WIDTH_DWORD,x64_register("eax"),x64_immediate(1));branch(r,X64_OP_JE,"done");
    data_address(r,"rax",".Lnative_buffer");op2(r,X64_OP_MOV,X64_WIDTH_BYTE,x64_memory(X64_WIDTH_BYTE,"rax",0),x64_immediate(0));
    label(r,"done");data_address(r,"rax",".Lnative_buffer");end(r);
}
static void integer_string(Runtime *r) {
    begin(r,"__dmm_rt_int_to_string",32);store(r,arg(r,0),8);store(r,arg(r,1),16);store(r,arg(r,2),24);
    test(r,arg(r,1));branch(r,X64_OP_JE,"zero");op2(r,X64_OP_CMP,X64_WIDTH_QWORD,x64_register(arg(r,2)),x64_immediate(0));branch(r,X64_OP_JLE,"zero");
    load(r,arg(r,0),16);load(r,arg(r,1),24);data_address(r,arg(r,2),".Lnative_integer");load(r,arg(r,3),8);call(r,"snprintf");
    load(r,arg(r,0),16);call(r,"strlen");branch(r,X64_OP_JMP,"done");label(r,"zero");imm(r,"rax",0);label(r,"done");end(r);
}
static void string_integer(Runtime *r) {
    begin(r,"__dmm_rt_string_to_int",0);nullsafe(r,0);imm(r,arg(r,1),0);imm(r,arg(r,2),10);call(r,"strtoll");end(r);
}
static void io_result(Runtime *r) {
    if(r->target==TARGET_COFF)op2(r,X64_OP_MOVSX,X64_WIDTH_QWORD,x64_register("rax"),x64_register("eax"));
    else {
        op2(r,X64_OP_CMP,X64_WIDTH_QWORD,x64_register("rax"),x64_immediate(-1));branch(r,X64_OP_JNE,"done");
        call(r,"__errno_location");op2(r,X64_OP_MOVSX,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_DWORD,"rax",0));op1(r,X64_OP_NEG,x64_register("rax"));
    }
}
static void io(Runtime *r,const char *name,const char *import,int count) {
    begin(r,name,32);for(int n=0;n<count;++n)store(r,arg(r,n),8+n*8);
    if(count==3 && strcmp(import,"open")) {
        test(r,arg(r,2));branch(r,X64_OP_JL,"invalid");
        if(r->target==TARGET_COFF) {imm(r,"rax",UINT32_MAX);op2(r,X64_OP_CMP,X64_WIDTH_QWORD,x64_register(arg(r,2)),x64_register("rax"));branch(r,X64_OP_JG,"invalid");}
    }
    if(!strcmp(import,"open")){test(r,arg(r,0));branch(r,X64_OP_JE,"invalid");}
    imm(r,arg(r,0),0);call(r,"fflush");for(int n=0;n<count;++n)load(r,arg(r,n),8+n*8);
    if(r->target==TARGET_COFF && !strcmp(import,"open")) {
        mov(r,"r10",arg(r,1));op2(r,X64_OP_AND,X64_WIDTH_QWORD,x64_register(arg(r,1)),x64_immediate(3));
        const long long masks[]={64,512,1024},mapped[]={256,512,8};
        for(int n=0;n<3;++n){char skip[32];snprintf(skip,sizeof(skip),"flag%d",n);
            op2(r,X64_OP_TEST,X64_WIDTH_QWORD,x64_register("r10"),x64_immediate(masks[n]));branch(r,X64_OP_JE,skip);
            op2(r,X64_OP_OR,X64_WIDTH_QWORD,x64_register(arg(r,1)),x64_immediate(mapped[n]));label(r,skip);}
        op2(r,X64_OP_OR,X64_WIDTH_QWORD,x64_register(arg(r,1)),x64_immediate(32768));
        imm(r,arg(r,2),384);
    }
    call(r,import);
    if (r->target==TARGET_ELF && (count==1 || !strcmp(import,"open")))
        op2(r,X64_OP_MOVSX,X64_WIDTH_QWORD,x64_register("rax"),x64_register("eax"));
    io_result(r);branch(r,X64_OP_JMP,"done");label(r,"invalid");imm(r,"rax",-22);label(r,"done");end(r);
}
static void read_value(Runtime *r) {
    begin(r,"__dmm_rt_read_value",96);store(r,arg(r,0),8);store(r,arg(r,1),16);
    address(r,arg(r,1),96);imm(r,arg(r,2),63);call(r,"__dmm_rt_sys_read");test(r,"rax");branch(r,X64_OP_JLE,"zero");
    address(r,"r10",96);op2(r,X64_OP_MOV,X64_WIDTH_BYTE,x64_indexed_memory(X64_WIDTH_BYTE,"r10","rax",1,0),x64_immediate(0));
    load(r,arg(r,0),16);nullsafe(r,0);data_address(r,arg(r,1),".Lnative_char");call(r,"strcmp");test(r,"rax");branch(r,X64_OP_JNE,"integer");
    op2(r,X64_OP_MOVZX,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_BYTE,"rbp",-96));branch(r,X64_OP_JMP,"done");
    label(r,"integer");address(r,arg(r,0),96);call(r,"__dmm_rt_string_to_int");branch(r,X64_OP_JMP,"done");
    label(r,"zero");imm(r,"rax",0);label(r,"done");end(r);
}
static void windows_snprintf(Runtime *r) {
    /* msvcrt exposes _snprintf; guarantee the C99 terminating-byte contract. */
    begin(r,"snprintf",32);store(r,"rcx",8);store(r,"rdx",16);call(r,"__dmm_libc_snprintf");store(r,"rax",24);
    load(r,"r10",8);load(r,"r11",16);test(r,"r10");branch(r,X64_OP_JE,"done");test(r,"r11");branch(r,X64_OP_JE,"done");
    op2(r,X64_OP_MOV,X64_WIDTH_BYTE,x64_indexed_memory(X64_WIDTH_BYTE,"r10","r11",1,-1),x64_immediate(0));
    label(r,"done");load(r,"rax",24);end(r);
}
const char *native_runtime_import(const char *name,TargetFormat target) {
    if (!strncmp(name,"__dmm_libc_",11)) name+=11;
    static const char *const imports[]={"printf","putchar","puts","strcmp","strcpy","strcat","malloc","calloc","free","strlen","snprintf","scanf","strdup","strtoll","fflush","read","write","open","close","exit","__libc_start_main","__errno_location"};
    if(!strcmp(name,"snprintf"))return target==TARGET_COFF?"_snprintf":"snprintf";
    for(size_t n=0;n<sizeof(imports)/sizeof(imports[0]);++n)if(!strcmp(name,imports[n])) {
        if(target==TARGET_COFF) {
            if(!strcmp(name,"strdup"))return "_strdup";
            if(!strcmp(name,"strtoll"))return "_strtoi64";
            if(!strcmp(name,"read"))return "_read";
            if(!strcmp(name,"write"))return "_write";
            if(!strcmp(name,"open"))return "_open";
            if(!strcmp(name,"close"))return "_close";
            if(!strcmp(name,"__errno_location")||!strcmp(name,"__libc_start_main"))return NULL;
        }return imports[n];
    }return NULL;
}
int native_runtime_emit(NativeObject *object,TargetFormat target) {
    Runtime r={object,target,NULL};object->section=NATIVE_RODATA;
    const char *const names[]={".Lnative_empty",".Lnative_integer",".Lnative_char",".Lnative_scan_string"};
    const char *const values[]={"","%lld","%c","%255s"};
    for(size_t n=0;n<4;++n){(void)native_define(object,names[n],0,0);(void)native_bytes(object,values[n],strlen(values[n])+1);}
    object->section=NATIVE_DATA;(void)native_define(object,".Lnative_buffer",0,0);(void)native_bytes(object,NULL,256);
    object->section=NATIVE_TEXT;
    emit_alias(&r,"__dmm_rt_strlen","strlen",1);emit_alias(&r,"__dmm_rt_strcmp","strcmp",3);
    emit_alias(&r,"__dmm_rt_strcpy","strcpy",2);emit_alias(&r,"__dmm_rt_strcat","strcat",2);
    emit_alias(&r,"__dmm_rt_strdup","strdup",1);emit_alias(&r,"__dmm_rt_malloc","malloc",0);
    scan(&r,"__dmm_rt_scan_int",0);scan(&r,"__dmm_rt_scan_char",1);scan_string(&r);
    if(target==TARGET_COFF)windows_snprintf(&r);
    integer_string(&r);string_integer(&r);io(&r,"__dmm_rt_sys_read","read",3);io(&r,"__dmm_rt_sys_write","write",3);
    io(&r,"__dmm_rt_sys_open","open",3);io(&r,"__dmm_rt_sys_close","close",1);read_value(&r);
    (void)native_define(object,"__dmm_entry",0,1);
    if(target==TARGET_COFF) {
        stack(&r,X64_OP_SUB,40);op1(&r,X64_OP_CALL,x64_label("main"));
        op2(&r,X64_OP_MOV,X64_WIDTH_DWORD,x64_register("ecx"),x64_register("eax"));op1(&r,X64_OP_CALL,x64_label("__dmm_libc_exit"));op0(&r,X64_OP_UD2);
    }else {
        mov(&r,"r9","rdx");op2(&r,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rsi"),x64_memory(X64_WIDTH_QWORD,"rsp",0));
        op2(&r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rdx"),x64_memory(X64_WIDTH_NONE,"rsp",8));
        op2(&r,X64_OP_AND,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(-16));imm(&r,"rax",0);
        op1(&r,X64_OP_PUSH,x64_register("rax"));op1(&r,X64_OP_PUSH,x64_register("rsp"));data_address(&r,"rdi","main");
        imm(&r,"rcx",0);imm(&r,"r8",0);op1(&r,X64_OP_CALL,x64_label("__dmm_libc___libc_start_main"));op0(&r,X64_OP_UD2);
    }
    return !object->failed;
}
