/* Standalone executor. Every queue transition and completion decision shares
   the executor lock; callbacks run outside it. Events are reset under the
   same lock as the predicate, so a wake racing with Pending cannot be lost.
   Contexts are retained task addresses; bit 0 suppresses cancellation during
   an active defer cleanup. No platform C library or external linker is used. */
enum { EX_EVENT=8, EX_HEAD=16, EX_TAIL=24, EX_ACTIVE=32, EX_CLOSED=40,
       EX_COUNT=48, EX_EXITED=56, EX_TASKS=64, EX_REFS=72, EX_THREADS=80,
       EX_WAITER=88, EX_BYTES=96 };
enum { TK_EX=0, TK_FRAME=8, TK_STATUS=16, TK_NEXT=24, TK_QUEUED=32,
       TK_RUNNING=40, TK_NOTIFIED=48, TK_CANCEL=56, TK_WAITER=64,
       TK_REFS=72, TK_ACTIVE_NEXT=80, TK_BYTES=88 };

static void af_get(Runtime *r,const char *dst,const char *base,int offset) {
    op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(dst),x64_memory(X64_WIDTH_QWORD,base,offset));
}
static void af_put(Runtime *r,const char *base,int offset,const char *src) {
    op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,base,offset),x64_register(src));
}
static void af_set(Runtime *r,const char *base,int offset,long long value) {
    op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,base,offset),x64_immediate(value));
}
static void af_local_arg(Runtime *r,int n,int slot) { load(r,arg(r,n),slot); }
static void af_field_arg(Runtime *r,int n,int slot,int field) {
    load(r,"r10",slot); af_get(r,arg(r,n),"r10",field);
}
static void af_ptr_arg(Runtime *r,int n,int slot,int field) {
    load(r,"r10",slot);
    op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register(arg(r,n)),x64_memory(X64_WIDTH_NONE,"r10",field));
}
static void af_indirect(Runtime *r,int frame,int field,int context,int discard) {
    load(r,"r10",frame); af_get(r,"r11","r10",field);
    mov(r,arg(r,0),"r10");
    if(field==8) imm(r,arg(r,1),discard); else af_local_arg(r,1,context);
    if(r->target==TARGET_COFF) stack(r,X64_OP_SUB,32);
    op1(r,X64_OP_CALL,x64_register("r11"));
    if(r->target==TARGET_COFF) stack(r,X64_OP_ADD,32);
}
static void af_lock_local(Runtime *r,int slot) { af_local_arg(r,0,slot); call(r,"__dmm_async_lock"); }
static void af_unlock_local(Runtime *r,int slot) { af_local_arg(r,0,slot); call(r,"__dmm_async_unlock"); }
static void af_event(Runtime *r,int slot,const char *fn) {
    af_field_arg(r,0,slot,EX_EVENT); call(r,fn);
}
static void af_allocate(Runtime *r,int bytes,int slot) {
    imm(r,arg(r,0),1); imm(r,arg(r,1),bytes); call(r,"calloc");
    test(r,"rax"); branch(r,X64_OP_JNE,"allocated"); op0(r,X64_OP_UD2); label(r,"allocated"); store(r,"rax",slot);
}
static void af_callback_ptr(Runtime *r,const char *symbol,int field) {
    data_address(r,"r11",symbol); af_put(r,"rax",field,"r11");
}

static void own_async_support(Runtime *r) {
    begin(r,"__dmm_async_lock",16); store(r,arg(r,0),8);
    label(r,"retry"); af_local_arg(r,0,8); imm(r,arg(r,1),1); call(r,"atomic_swap");
    test(r,"rax"); branch(r,X64_OP_JE,"done");
    {const unsigned char pause[]={0xf3,0x90}; (void)native_bytes(r->object,pause,sizeof(pause));}
    branch(r,X64_OP_JMP,"retry"); label(r,"done"); end(r);
    begin(r,"__dmm_async_unlock",0); imm(r,arg(r,1),0); call(r,"atomic_store"); end(r);

    /* Reference operations return the post-operation value. */
    for(int release=0;release<2;release++) {
        begin(r,release ? "__dmm_async_ref_release":"__dmm_async_ref_retain",32);
        store(r,arg(r,0),8);
        label(r,"retry"); load(r,"r10",8); af_get(r,"rax","r10",0);
        store(r,"rax",24);
        mov(r,"r11","rax"); constant(r,release ? X64_OP_SUB:X64_OP_ADD,"r11",1);
        mov(r,arg(r,0),"r10"); mov(r,arg(r,1),"rax"); mov(r,arg(r,2),"r11");
        store(r,"r11",16); call(r,"atomic_compare_exchange");
        load(r,"r11",24); binary(r,X64_OP_CMP,"rax","r11"); branch(r,X64_OP_JNE,"retry"); load(r,"rax",16); end(r);
    }
    begin(r,"__dmm_async_executor_release",16); store(r,arg(r,0),8);
    af_ptr_arg(r,0,8,EX_REFS); call(r,"__dmm_async_ref_release");
    test(r,"rax"); branch(r,X64_OP_JNE,"done");
    af_event(r,8,"__dmm_async_wait_destroy"); af_field_arg(r,0,8,EX_THREADS); call(r,"free");
    af_local_arg(r,0,8); call(r,"free"); label(r,"done"); end(r);
    begin(r,"__dmm_async_context_retain",16);
    constant(r,X64_OP_AND,arg(r,0),-2); test(r,arg(r,0)); branch(r,X64_OP_JE,"done");
    store(r,arg(r,0),8); af_ptr_arg(r,0,8,TK_REFS); call(r,"__dmm_async_ref_retain");
    label(r,"done"); end(r);
    begin(r,"__dmm_async_context_release",32);
    constant(r,X64_OP_AND,arg(r,0),-2); test(r,arg(r,0)); branch(r,X64_OP_JE,"done");
    store(r,arg(r,0),8); af_ptr_arg(r,0,8,TK_REFS); call(r,"__dmm_async_ref_release");
    test(r,"rax"); branch(r,X64_OP_JNE,"done");
    af_field_arg(r,0,8,TK_EX); store(r,arg(r,0),16); af_local_arg(r,0,8); call(r,"free");
    af_local_arg(r,0,16); call(r,"__dmm_async_executor_release"); label(r,"done"); end(r);
    begin(r,"__dmm_async_cancel_requested",0); zero(r,"rax");
    test(r,arg(r,0)); branch(r,X64_OP_JE,"done");
    op2(r,X64_OP_TEST,X64_WIDTH_QWORD,x64_register(arg(r,0)),x64_immediate(1)); branch(r,X64_OP_JNE,"done");
    af_get(r,"rax",arg(r,0),TK_CANCEL); label(r,"done"); end(r);

    begin(r,"__dmm_async_frame_destroy",32);
    store(r,arg(r,0),8); store(r,arg(r,1),16);
    test(r,arg(r,1)); branch(r,X64_OP_JE,"free");
    af_get(r,"rax",arg(r,0),48); test(r,"rax"); branch(r,X64_OP_JE,"free");
    load(r,"r10",8); af_set(r,"r10",48,0); af_get(r,"r11","r10",32);
    test(r,"r11"); branch(r,X64_OP_JE,"free");
    op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register(arg(r,0)),x64_memory(X64_WIDTH_NONE,"r10",80));
    if(r->target==TARGET_COFF) stack(r,X64_OP_SUB,32);
    op1(r,X64_OP_CALL,x64_register("r11"));
    if(r->target==TARGET_COFF) stack(r,X64_OP_ADD,32);
    label(r,"free"); af_local_arg(r,0,8); call(r,"free"); end(r);

    begin(r,"__dmm_async_discard_poll",32);
    store(r,arg(r,0),8); store(r,arg(r,1),16);
    mov(r,"r10",arg(r,0)); af_get(r,"r11","r10",72);
    af_get(r,"rax","r10",48); test(r,"rax"); branch(r,X64_OP_JE,"ready");
    test(r,"r11"); branch(r,X64_OP_JE,"ready");
    af_get(r,"rax","r10",0); data_address(r,"r11","__dmm_async_join_poll");
    binary(r,X64_OP_CMP,"rax","r11"); branch(r,X64_OP_JNE,"ordinary");
    af_get(r,"rax","r10",80); test(r,"rax"); branch(r,X64_OP_JNE,"ready");
    op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_NONE,"r10",88));
    branch(r,X64_OP_JMP,"output");
    label(r,"ordinary"); op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_NONE,"r10",80));
    label(r,"output"); af_put(r,"r10",64,"rax"); af_indirect(r,8,72,16,0); end(r);
    label(r,"ready"); imm(r,"rax",1); end(r);

    /* Queue insertion requires the executor lock; running tasks only remember
       notification until their current poll has returned. */
    begin(r,"__dmm_async_enqueue",16); store(r,arg(r,0),8); store(r,arg(r,1),16);
    mov(r,"r10",arg(r,0)); mov(r,"r11",arg(r,1));
    af_get(r,"rax","r11",TK_STATUS); test(r,"rax"); branch(r,X64_OP_JNE,"done");
    af_set(r,"r11",TK_NOTIFIED,1);
    af_get(r,"rax","r11",TK_RUNNING); test(r,"rax"); branch(r,X64_OP_JNE,"done");
    af_get(r,"rax","r11",TK_QUEUED); test(r,"rax"); branch(r,X64_OP_JNE,"done");
    af_set(r,"r11",TK_QUEUED,1); af_set(r,"r11",TK_NEXT,0);
    af_get(r,"rax","r10",EX_TAIL); test(r,"rax"); branch(r,X64_OP_JE,"first");
    af_put(r,"rax",TK_NEXT,"r11"); branch(r,X64_OP_JMP,"tail");
    label(r,"first"); af_put(r,"r10",EX_HEAD,"r11");
    label(r,"tail"); af_put(r,"r10",EX_TAIL,"r11"); af_event(r,8,"__dmm_async_wake");
    label(r,"done"); end(r);
    begin(r,"__dmm_async_context_wake",32);
    constant(r,X64_OP_AND,arg(r,0),-2); test(r,arg(r,0)); branch(r,X64_OP_JE,"done");
    store(r,arg(r,0),8); af_get(r,"r10",arg(r,0),TK_EX); store(r,"r10",16);
    af_lock_local(r,16); af_local_arg(r,0,16); af_local_arg(r,1,8); call(r,"__dmm_async_enqueue");
    /* Local block_on contexts have running=1 and park on their own event. */
    af_event(r,16,"__dmm_async_wake"); af_unlock_local(r,16); label(r,"done"); end(r);
    begin(r,"__dmm_async_request_cancel",32);
    store(r,arg(r,0),8); af_get(r,"r10",arg(r,0),TK_EX); store(r,"r10",16);
    af_lock_local(r,16); load(r,"r10",8); af_get(r,"rax","r10",TK_STATUS);
    test(r,"rax"); branch(r,X64_OP_JNE,"unlock"); af_set(r,"r10",TK_CANCEL,1);
    af_local_arg(r,0,16); af_local_arg(r,1,8); call(r,"__dmm_async_enqueue");
    label(r,"unlock"); af_unlock_local(r,16); end(r);
}

static void own_async_worker(Runtime *r) {
    begin(r,"__dmm_async_worker",64); store(r,arg(r,0),8);
    label(r,"next"); af_lock_local(r,8); load(r,"r10",8); af_get(r,"rax","r10",EX_HEAD);
    test(r,"rax"); branch(r,X64_OP_JNE,"task");
    af_get(r,"rax","r10",EX_CLOSED); test(r,"rax"); branch(r,X64_OP_JE,"park");
    af_get(r,"rax","r10",EX_ACTIVE); test(r,"rax"); branch(r,X64_OP_JE,"exit");
    label(r,"park"); af_event(r,8,"__dmm_async_wait_reset"); af_unlock_local(r,8);
    af_event(r,8,"__dmm_async_wait"); branch(r,X64_OP_JMP,"next");
    label(r,"task"); store(r,"rax",16); mov(r,"r11","rax"); af_get(r,"rax","r11",TK_NEXT);
    af_put(r,"r10",EX_HEAD,"rax"); test(r,"rax"); branch(r,X64_OP_JNE,"popped"); af_set(r,"r10",EX_TAIL,0);
    label(r,"popped"); af_set(r,"r11",TK_QUEUED,0); af_set(r,"r11",TK_RUNNING,1); af_set(r,"r11",TK_NOTIFIED,0);
    af_get(r,"rax","r11",TK_CANCEL); store(r,"rax",24); af_get(r,"rax","r11",TK_FRAME); store(r,"rax",32);
    af_unlock_local(r,8); load(r,"rax",24); test(r,"rax"); branch(r,X64_OP_JNE,"cancel_poll");
    af_indirect(r,32,0,16,0); branch(r,X64_OP_JMP,"polled");
    label(r,"cancel_poll"); af_indirect(r,32,24,16,0);
    label(r,"polled"); store(r,"rax",40); af_lock_local(r,8);
    load(r,"r10",16); load(r,"rax",40); test(r,"rax"); branch(r,X64_OP_JNE,"ready");
    af_set(r,"r10",TK_RUNNING,0); af_get(r,"rax","r10",TK_NOTIFIED); test(r,"rax"); branch(r,X64_OP_JNE,"requeue");
    /* A request during a normal Pending poll must enter cancel_poll next. */
    load(r,"rax",24); test(r,"rax"); branch(r,X64_OP_JNE,"continue");
    af_get(r,"rax","r10",TK_CANCEL); test(r,"rax"); branch(r,X64_OP_JE,"continue");
    label(r,"requeue"); af_local_arg(r,0,8); af_local_arg(r,1,16); call(r,"__dmm_async_enqueue");
    label(r,"continue"); af_unlock_local(r,8); branch(r,X64_OP_JMP,"next");
    label(r,"ready"); af_get(r,"rax","r10",TK_CANCEL); test(r,"rax"); branch(r,X64_OP_JE,"normal");
    /* The cancellation decision wins under lock. Destruction happens before
       publishing Cancelled, and a join waiter cannot observe freed storage. */
    af_unlock_local(r,8);
    af_local_arg(r,0,32); af_local_arg(r,1,16); call(r,"__dmm_async_discard_poll");
    test(r,"rax"); branch(r,X64_OP_JNE,"discard_ready");
    af_lock_local(r,8); load(r,"r10",16); af_set(r,"r10",TK_RUNNING,0);
    af_get(r,"rax","r10",TK_NOTIFIED); test(r,"rax"); branch(r,X64_OP_JNE,"requeue");
    branch(r,X64_OP_JMP,"continue");
    label(r,"discard_ready"); af_indirect(r,32,8,0,1); af_lock_local(r,8);
    load(r,"r10",16); af_set(r,"r10",TK_FRAME,0); imm(r,"rax",2); branch(r,X64_OP_JMP,"publish");
    label(r,"normal"); imm(r,"rax",1);
    label(r,"publish"); af_put(r,"r10",TK_STATUS,"rax"); af_set(r,"r10",TK_RUNNING,0);
    af_get(r,"rax","r10",TK_WAITER); store(r,"rax",48); af_set(r,"r10",TK_WAITER,0);
    load(r,"r11",8); af_get(r,"rax","r11",EX_ACTIVE); constant(r,X64_OP_SUB,"rax",1); af_put(r,"r11",EX_ACTIVE,"rax");
    /* Unlink from the active list without changing completed handle state. */
    op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("r11"),x64_memory(X64_WIDTH_NONE,"r11",EX_TASKS));
    label(r,"find"); af_get(r,"rax","r11",0); binary(r,X64_OP_CMP,"rax","r10"); branch(r,X64_OP_JE,"unlink");
    op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("r11"),x64_memory(X64_WIDTH_NONE,"rax",TK_ACTIVE_NEXT)); branch(r,X64_OP_JMP,"find");
    label(r,"unlink"); af_get(r,"rax","r10",TK_ACTIVE_NEXT); af_put(r,"r11",0,"rax");
    af_event(r,8,"__dmm_async_wake"); af_unlock_local(r,8);
    af_local_arg(r,0,48); call(r,"__dmm_async_context_wake");
    af_local_arg(r,0,48); call(r,"__dmm_async_context_release");
    af_local_arg(r,0,16); call(r,"__dmm_async_context_release"); branch(r,X64_OP_JMP,"next");
    label(r,"exit"); af_get(r,"rax","r10",EX_EXITED); constant(r,X64_OP_ADD,"rax",1); af_put(r,"r10",EX_EXITED,"rax");
    af_get(r,"r11","r10",EX_COUNT); binary(r,X64_OP_CMP,"rax","r11"); branch(r,X64_OP_JNE,"not_last");
    af_get(r,"rax","r10",EX_WAITER); store(r,"rax",48); af_set(r,"r10",EX_WAITER,0);
    af_unlock_local(r,8); af_local_arg(r,0,48); call(r,"__dmm_async_context_wake");
    af_local_arg(r,0,48); call(r,"__dmm_async_context_release"); zero(r,"rax"); end(r);
    label(r,"not_last"); af_unlock_local(r,8); zero(r,"rax"); end(r);
}

static void own_async_create(Runtime *r) {
    begin(r,"__dmm_async_executor_create",48); store(r,arg(r,0),8);
    test(r,arg(r,0)); branch(r,X64_OP_JG,"valid"); op0(r,X64_OP_UD2); label(r,"valid");
    af_allocate(r,EX_BYTES,16); load(r,"r11",8); af_put(r,"rax",EX_COUNT,"r11"); af_set(r,"rax",EX_REFS,1);
    call(r,"__dmm_async_wait_create"); load(r,"r10",16); af_put(r,"r10",EX_EVENT,"rax");
    af_local_arg(r,0,8); imm(r,arg(r,1),8); call(r,"calloc");
    test(r,"rax"); branch(r,X64_OP_JNE,"threads"); op0(r,X64_OP_UD2); label(r,"threads");
    load(r,"r10",16); af_put(r,"r10",EX_THREADS,"rax"); zero(r,"rax"); store(r,"rax",24);
    label(r,"loop"); data_address(r,arg(r,0),"__dmm_async_worker"); af_local_arg(r,1,16); call(r,"__dmm_async_thread_create");
    load(r,"r10",16); af_get(r,"r10","r10",EX_THREADS); load(r,"r11",24);
    op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_indexed_memory(X64_WIDTH_QWORD,"r10","r11",8,0),x64_register("rax"));
    constant(r,X64_OP_ADD,"r11",1); store(r,"r11",24); load(r,"rax",8); binary(r,X64_OP_CMP,"r11","rax");
    branch(r,X64_OP_JNE,"loop"); load(r,"rax",16); end(r);

    begin(r,"__dmm_async_default",16);
    data_address(r,arg(r,0),".Lnative_async_default_lock"); call(r,"__dmm_async_lock");
    data_address(r,"r10",".Lnative_async_default"); af_get(r,"rax","r10",0); test(r,"rax"); branch(r,X64_OP_JNE,"exists");
    imm(r,arg(r,0),2); call(r,"__dmm_async_executor_create"); data_address(r,"r10",".Lnative_async_default"); af_put(r,"r10",0,"rax");
    label(r,"exists"); store(r,"rax",8); data_address(r,arg(r,0),".Lnative_async_default_lock"); call(r,"__dmm_async_unlock");
    load(r,"rax",8); end(r);
}

static void own_async_spawn(Runtime *r) {
    begin(r,"__dmm_async_spawn",64);
    store(r,arg(r,0),8); store(r,arg(r,1),16); store(r,arg(r,2),24);
    test(r,arg(r,0)); branch(r,X64_OP_JNE,"explicit"); call(r,"__dmm_async_default"); store(r,"rax",8); label(r,"explicit");
    af_allocate(r,TK_BYTES,32); load(r,"r11",8); af_put(r,"rax",TK_EX,"r11");
    load(r,"r11",16); af_put(r,"rax",TK_FRAME,"r11"); af_set(r,"rax",TK_REFS,2);
    af_ptr_arg(r,0,8,EX_REFS); call(r,"__dmm_async_ref_retain");
    load(r,"rax",24); constant(r,X64_OP_ADD,"rax",104); mov(r,arg(r,1),"rax"); imm(r,arg(r,0),1); call(r,"calloc");
    test(r,"rax"); branch(r,X64_OP_JNE,"join"); op0(r,X64_OP_UD2); label(r,"join"); store(r,"rax",40);
    af_callback_ptr(r,"__dmm_async_join_poll",0); af_callback_ptr(r,"__dmm_async_join_destroy",8);
    af_callback_ptr(r,"__dmm_async_join_cancel",24);
    load(r,"r11",32); af_put(r,"rax",64,"r11"); load(r,"r11",24); af_put(r,"rax",56,"r11");
    load(r,"r11",16); af_get(r,"r11","r11",72); af_put(r,"rax",72,"r11");
    load(r,"r11",16); af_get(r,"r11","r11",32); af_put(r,"rax",32,"r11");
    af_lock_local(r,8); load(r,"r10",8); af_get(r,"rax","r10",EX_CLOSED); test(r,"rax"); branch(r,X64_OP_JE,"open"); op0(r,X64_OP_UD2);
    label(r,"open"); af_get(r,"rax","r10",EX_ACTIVE); constant(r,X64_OP_ADD,"rax",1); af_put(r,"r10",EX_ACTIVE,"rax");
    af_get(r,"rax","r10",EX_TASKS); load(r,"r11",32); af_put(r,"r11",TK_ACTIVE_NEXT,"rax"); af_put(r,"r10",EX_TASKS,"r11");
    af_local_arg(r,0,8); af_local_arg(r,1,32); call(r,"__dmm_async_enqueue"); af_unlock_local(r,8);
    load(r,"rax",40); end(r);
}

static void own_async_join(Runtime *r) {
    for(int cancel=0;cancel<2;cancel++) {
        begin(r,cancel ? "__dmm_async_join_cancel":"__dmm_async_join_poll",64);
        if(cancel) {
            af_get(r,"rax",arg(r,0),48); test(r,"rax"); branch(r,X64_OP_JE,"unclaimed");
            call(r,"__dmm_async_discard_poll"); end(r); label(r,"unclaimed");
        }
        store(r,arg(r,0),8); store(r,arg(r,1),16); af_get(r,"r10",arg(r,0),64); store(r,"r10",24);
        if(cancel) {af_local_arg(r,0,24); call(r,"__dmm_async_request_cancel");}
        af_field_arg(r,0,24,TK_EX); store(r,arg(r,0),32); af_lock_local(r,32);
        load(r,"r10",24); af_get(r,"rax","r10",TK_STATUS); test(r,"rax"); branch(r,X64_OP_JNE,"ready");
        af_get(r,"rax","r10",TK_WAITER); test(r,"rax"); branch(r,X64_OP_JNE,"pending");
        af_local_arg(r,0,16); call(r,"__dmm_async_context_retain"); load(r,"r10",24); load(r,"r11",16); af_put(r,"r10",TK_WAITER,"r11");
        label(r,"pending"); af_unlock_local(r,32); zero(r,"rax"); end(r);
        label(r,"ready"); store(r,"rax",40); af_get(r,"rax","r10",TK_FRAME); store(r,"rax",48); af_unlock_local(r,32);
        if(cancel) {
            load(r,"rax",48); test(r,"rax"); branch(r,X64_OP_JE,"release");
            af_local_arg(r,0,48); af_local_arg(r,1,16); call(r,"__dmm_async_discard_poll");
            test(r,"rax"); branch(r,X64_OP_JNE,"discard_ready"); zero(r,"rax"); end(r);
            label(r,"discard_ready"); af_indirect(r,48,8,0,1);
        } else {
            load(r,"rax",40); compare(r,"rax",1); branch(r,X64_OP_JNE,"cancelled");
            af_local_arg(r,0,8); constant(r,X64_OP_ADD,arg(r,0),88);
            af_local_arg(r,1,48); constant(r,X64_OP_ADD,arg(r,1),80); af_field_arg(r,2,8,56); call(r,"copy");
            af_indirect(r,48,8,0,0); load(r,"r10",8); af_set(r,"r10",80,0); branch(r,X64_OP_JMP,"release");
            label(r,"cancelled"); load(r,"r10",8); af_set(r,"r10",80,1); af_set(r,"r10",88,0);
        }
        label(r,"release"); af_local_arg(r,0,24); call(r,"__dmm_async_context_release");
        load(r,"r10",8); af_set(r,"r10",64,0); af_set(r,"r10",16,cancel ? -2:-1); af_set(r,"r10",48,cancel ? 0:1);
        imm(r,"rax",1); end(r);
    }
    begin(r,"__dmm_async_join_destroy",32); store(r,arg(r,0),8);
    test(r,arg(r,1)); branch(r,X64_OP_JE,"free");
    mov(r,"r10",arg(r,0)); af_get(r,"rax","r10",48); test(r,"rax"); branch(r,X64_OP_JE,"free");
    af_get(r,"rax","r10",80); test(r,"rax"); branch(r,X64_OP_JNE,"free");
    af_get(r,"r11","r10",32); test(r,"r11"); branch(r,X64_OP_JE,"free");
    op2(r,X64_OP_LEA,X64_WIDTH_QWORD,x64_register(arg(r,0)),x64_memory(X64_WIDTH_NONE,"r10",88));
    if(r->target==TARGET_COFF) stack(r,X64_OP_SUB,32);
    op1(r,X64_OP_CALL,x64_register("r11"));
    if(r->target==TARGET_COFF) stack(r,X64_OP_ADD,32);
    label(r,"free"); af_local_arg(r,0,8); call(r,"free"); end(r);
}

static void own_async_cancel(Runtime *r) {
    begin(r,"__dmm_async_cancel",16); store(r,arg(r,0),8);
    af_allocate(r,88,16); af_callback_ptr(r,"__dmm_async_cancel_poll",0);
    af_callback_ptr(r,"__dmm_async_frame_destroy",8); af_callback_ptr(r,"__dmm_async_cancel_poll",24);
    load(r,"r11",8); af_put(r,"rax",64,"r11"); end(r);
    begin(r,"__dmm_async_cancel_poll",32); store(r,arg(r,0),8); store(r,arg(r,1),16);
    af_get(r,"r10",arg(r,0),64); store(r,"r10",24);
    af_get(r,"rax","r10",16); compare(r,"rax",-1); branch(r,X64_OP_JE,"discard");
    compare(r,"rax",-2); branch(r,X64_OP_JE,"ready");
    af_indirect(r,24,24,16,0); test(r,"rax"); branch(r,X64_OP_JNE,"ready"); zero(r,"rax"); end(r);
    label(r,"discard"); af_local_arg(r,0,24); af_local_arg(r,1,16); call(r,"__dmm_async_discard_poll");
    test(r,"rax"); branch(r,X64_OP_JNE,"ready"); zero(r,"rax"); end(r);
    label(r,"ready"); af_indirect(r,24,8,0,1); load(r,"r10",8); af_set(r,"r10",64,0);
    af_set(r,"r10",16,-1); imm(r,"rax",1); end(r);
}

static void own_async_shutdown(Runtime *r) {
    begin(r,"__dmm_async_shutdown",48); store(r,arg(r,0),8); store(r,arg(r,1),16);
    af_allocate(r,88,24); af_callback_ptr(r,"__dmm_async_shutdown_poll",0);
    af_callback_ptr(r,"__dmm_async_frame_destroy",8); af_callback_ptr(r,"__dmm_async_shutdown_poll",24);
    load(r,"r11",8); af_put(r,"rax",64,"r11");
    af_lock_local(r,8); load(r,"r10",8); af_set(r,"r10",EX_CLOSED,1);
    load(r,"rax",16); test(r,"rax"); branch(r,X64_OP_JE,"wake");
    af_get(r,"rax","r10",EX_TASKS); store(r,"rax",32);
    label(r,"task"); load(r,"r10",32); test(r,"r10"); branch(r,X64_OP_JE,"wake");
    af_set(r,"r10",TK_CANCEL,1); af_get(r,"rax","r10",TK_ACTIVE_NEXT); store(r,"rax",32);
    af_local_arg(r,0,8); mov(r,arg(r,1),"r10"); call(r,"__dmm_async_enqueue"); branch(r,X64_OP_JMP,"task");
    label(r,"wake"); af_event(r,8,"__dmm_async_wake"); af_unlock_local(r,8); load(r,"rax",24); end(r);
    begin(r,"__dmm_async_shutdown_poll",48); store(r,arg(r,0),8); store(r,arg(r,1),16);
    af_get(r,"rax",arg(r,0),64); store(r,"rax",24); af_lock_local(r,24);
    load(r,"r10",24); af_get(r,"rax","r10",EX_EXITED); af_get(r,"r11","r10",EX_COUNT);
    binary(r,X64_OP_CMP,"rax","r11"); branch(r,X64_OP_JE,"ready");
    af_get(r,"rax","r10",EX_WAITER); test(r,"rax"); branch(r,X64_OP_JNE,"pending");
    af_local_arg(r,0,16); call(r,"__dmm_async_context_retain"); load(r,"r10",24); load(r,"r11",16); af_put(r,"r10",EX_WAITER,"r11");
    label(r,"pending"); af_unlock_local(r,24); zero(r,"rax"); end(r);
    label(r,"ready"); af_unlock_local(r,24); zero(r,"rax"); store(r,"rax",32);
    label(r,"join"); load(r,"r10",24); af_get(r,"rax","r10",EX_COUNT); load(r,"r11",32);
    binary(r,X64_OP_CMP,"r11","rax"); branch(r,X64_OP_JE,"finished"); af_get(r,"r10","r10",EX_THREADS);
    op2(r,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(arg(r,0)),x64_indexed_memory(X64_WIDTH_QWORD,"r10","r11",8,0));
    call(r,"__dmm_async_thread_join"); load(r,"rax",32); constant(r,X64_OP_ADD,"rax",1); store(r,"rax",32); branch(r,X64_OP_JMP,"join");
    label(r,"finished"); af_local_arg(r,0,24); call(r,"__dmm_async_executor_release");
    load(r,"r10",8); af_set(r,"r10",64,0); af_set(r,"r10",16,-1); imm(r,"rax",1); end(r);
}

static void own_async_block_on(Runtime *r) {
    begin(r,"__dmm_async_block_on",48); store(r,arg(r,0),8);
    af_allocate(r,EX_BYTES,16); af_set(r,"rax",EX_REFS,1);
    call(r,"__dmm_async_wait_create"); load(r,"r10",16); af_put(r,"r10",EX_EVENT,"rax");
    imm(r,arg(r,0),1); imm(r,arg(r,1),TK_BYTES); call(r,"calloc");
    test(r,"rax"); branch(r,X64_OP_JNE,"context"); op0(r,X64_OP_UD2); label(r,"context"); store(r,"rax",24);
    load(r,"r11",16); af_put(r,"rax",TK_EX,"r11"); af_set(r,"rax",TK_REFS,1); af_set(r,"rax",TK_RUNNING,1);
    label(r,"poll"); af_lock_local(r,16); load(r,"r10",24); af_set(r,"r10",TK_NOTIFIED,0);
    af_event(r,16,"__dmm_async_wait_reset"); af_unlock_local(r,16);
    af_indirect(r,8,0,24,0); test(r,"rax"); branch(r,X64_OP_JNE,"ready");
    af_lock_local(r,16); load(r,"r10",24); af_get(r,"rax","r10",TK_NOTIFIED); store(r,"rax",32); af_unlock_local(r,16);
    load(r,"rax",32); test(r,"rax"); branch(r,X64_OP_JNE,"poll"); af_event(r,16,"__dmm_async_wait"); branch(r,X64_OP_JMP,"poll");
    label(r,"ready"); af_lock_local(r,16); load(r,"r10",24); af_set(r,"r10",TK_STATUS,1); af_unlock_local(r,16);
    af_local_arg(r,0,24); call(r,"__dmm_async_context_release"); load(r,"rax",8); end(r);

    begin(r,"__dmm_async_default_drain",16);
    data_address(r,"r10",".Lnative_async_default"); af_get(r,"rax","r10",0); test(r,"rax"); branch(r,X64_OP_JE,"done");
    mov(r,arg(r,0),"rax"); imm(r,arg(r,1),0); call(r,"__dmm_async_shutdown"); store(r,"rax",8);
    mov(r,arg(r,0),"rax"); call(r,"__dmm_async_block_on"); af_indirect(r,8,8,0,0);
    data_address(r,"r10",".Lnative_async_default"); af_set(r,"r10",0,0); label(r,"done"); end(r);
}

/* Private I/O lifetime contract. The frame and confirmation adapter own one
   reference each. Confirmation retains the adapter reference through wake
   delivery, so a resumed cancellation cannot free the operation underneath it. */
static void own_async_io(Runtime *r) {
    begin(r,"__dmm_async_io_create",32);
    store(r,arg(r,0),8); store(r,arg(r,1),16); af_allocate(r,56,24);
    af_set(r,"rax",8,2); load(r,"r11",8); af_put(r,"rax",40,"r11"); load(r,"r11",16); af_put(r,"rax",48,"r11"); end(r);
    begin(r,"__dmm_async_io_release",32); store(r,arg(r,0),8);
    af_ptr_arg(r,0,8,8); call(r,"__dmm_async_ref_release"); test(r,"rax"); branch(r,X64_OP_JNE,"done");
    load(r,"r10",8); af_get(r,"r11","r10",48); test(r,"r11"); branch(r,X64_OP_JE,"free");
    af_get(r,arg(r,0),"r10",40);
    if(r->target==TARGET_COFF) stack(r,X64_OP_SUB,32);
    op1(r,X64_OP_CALL,x64_register("r11"));
    if(r->target==TARGET_COFF) stack(r,X64_OP_ADD,32);
    label(r,"free"); af_local_arg(r,0,8); call(r,"free"); label(r,"done"); end(r);
    begin(r,"__dmm_async_io_poll",32); store(r,arg(r,0),8); store(r,arg(r,1),16);
    af_lock_local(r,8); load(r,"r10",8); af_get(r,"rax","r10",24); test(r,"rax"); branch(r,X64_OP_JNE,"ready");
    af_get(r,"rax","r10",32); test(r,"rax"); branch(r,X64_OP_JNE,"pending");
    af_local_arg(r,0,16); call(r,"__dmm_async_context_retain"); load(r,"r10",8); load(r,"r11",16); af_put(r,"r10",32,"r11");
    label(r,"pending"); af_unlock_local(r,8); zero(r,"rax"); end(r);
    label(r,"ready"); af_unlock_local(r,8); imm(r,"rax",1); end(r);
    begin(r,"__dmm_async_io_request_cancel",16); store(r,arg(r,0),8);
    af_lock_local(r,8); load(r,"r10",8); af_set(r,"r10",16,1); af_unlock_local(r,8); end(r);
    begin(r,"__dmm_async_io_cancel_requested",0); af_get(r,"rax",arg(r,0),16); end(r);
    begin(r,"__dmm_async_io_confirm",32); store(r,arg(r,0),8);
    af_lock_local(r,8); load(r,"r10",8); af_get(r,"rax","r10",24); test(r,"rax"); branch(r,X64_OP_JE,"first"); op0(r,X64_OP_UD2);
    label(r,"first"); af_set(r,"r10",24,1); af_get(r,"rax","r10",32); store(r,"rax",16); af_set(r,"r10",32,0); af_unlock_local(r,8);
    af_local_arg(r,0,16); call(r,"__dmm_async_context_wake"); af_local_arg(r,0,16); call(r,"__dmm_async_context_release");
    af_local_arg(r,0,8); call(r,"__dmm_async_io_release"); end(r);
    begin(r,"__dmm_async_io_destroy",16); store(r,arg(r,0),8);
    af_lock_local(r,8); load(r,"r10",8); af_get(r,"rax","r10",24); test(r,"rax"); branch(r,X64_OP_JNE,"confirmed"); op0(r,X64_OP_UD2);
    label(r,"confirmed"); af_unlock_local(r,8); af_local_arg(r,0,8); call(r,"__dmm_async_io_release"); end(r);
}

static void own_async_executor(Runtime *r) {
    r->object->section=NATIVE_DATA;
    (void)native_buffer_align(&r->object->sections[NATIVE_DATA],8);
    (void)native_define(r->object,".Lnative_async_default_lock",0,0); (void)native_uint(r->object,0,8);
    (void)native_define(r->object,".Lnative_async_default",0,0); (void)native_uint(r->object,0,8);
    r->object->section=NATIVE_TEXT;
    own_async_support(r); own_async_worker(r); own_async_create(r); own_async_spawn(r);
    own_async_join(r); own_async_cancel(r); own_async_shutdown(r); own_async_block_on(r);
    own_async_io(r);
}
