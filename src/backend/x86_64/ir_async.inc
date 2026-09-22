/* Private native ABI: [poll, cleanup, state, result..., pinned storage...].
   r14 addresses the top of pinned storage, exactly like the ordinary rbp
   storage layout. The real stack is used only for ABI calls and saved regs. */
static long long async_header_offset(const Emitter *emitter, size_t field) {
    return -(long long) emitter->async_frame_bytes + (long long) field;
}

static int copy_async_storage(Emitter *emitter, IrTypeId type, const char *source, const char *destination) {
    /* r11 is not an argument register: parameter capture may still need rcx. */
    IrTypeLayout layout;
    if (!ir_type_layout(emitter->module, type, &layout)) return 0;
    size_t bytes = layout.size;
    if (emitter->module->types[type].kind == IR_TYPE_ARRAY) {
        const IrType *array = &emitter->module->types[type];
        if (!ir_type_layout(emitter->module, array->element_type, &layout) ||
            array->array_length > SIZE_MAX / layout.size) return 0;
        bytes = array->array_length * layout.size;
    }
    size_t offset = 0;
    for (; bytes - offset >= 8; offset += 8) {
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r11"),
                    x64_memory(X64_WIDTH_QWORD, source, (long long) offset));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, destination, (long long) offset), x64_register("r11"));
    }
    for (; offset < bytes; offset++) {
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_BYTE, x64_register("r11b"),
                    x64_memory(X64_WIDTH_BYTE, source, (long long) offset));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_BYTE,
                    x64_memory(X64_WIDTH_BYTE, destination, (long long) offset), x64_register("r11b"));
    }
    return 1;
}

static void async_copy_parameter_word(Emitter *emitter, size_t offset) {
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rbx"),
                x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "r14", -(long long) offset), x64_register("rbx"));
}

static void async_callback_mode(Emitter *emitter, size_t value, size_t field, int cleanup) {
    write_value_load(emitter, "rax", value);
    write_register_move(emitter, argument_register(emitter->target, 0), "rax");
    if (field == 8) write_immediate(emitter,argument_register(emitter->target,1),0);
    else write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,
                     x64_register(argument_register(emitter->target,1)),
                     x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(emitter,40)));
    if (cleanup && field!=8) write_x64_2(emitter,X64_OP_OR,X64_WIDTH_QWORD,
        x64_register(argument_register(emitter->target,1)),x64_immediate(1));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r11"),
                x64_memory(X64_WIDTH_QWORD, "rax", (long long) field));
    if (emitter->target == TARGET_COFF)
        write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate(32));
    write_x64_1(emitter, X64_OP_CALL, X64_WIDTH_NONE, x64_register("r11"));
    if (emitter->target == TARGET_COFF)
        write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate(32));
}
static void async_callback(Emitter *e,size_t value,size_t field) { async_callback_mode(e,value,field,0); }

static int emit_async_await(Emitter *emitter, const IrInstruction *instruction) {
    char ready[96];
    (void) snprintf(ready, sizeof(ready), ".LIR_async_ready_%zu_%zu",
                    emitter->function_index, instruction->target_a);
    write_labelf(emitter, ".LIR_async_resume_%zu_%zu:\n",
                 emitter->function_index, instruction->target_a);
    async_callback_mode(emitter, instruction->operand_a, 0,instruction->async_cleanup);
    write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rax"));
    write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(ready));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "r14", async_header_offset(emitter, 16)),
                x64_immediate((long long) instruction->target_a));
    char pending[80];
    (void) snprintf(pending, sizeof(pending), ".LIR_async_pending_%zu", emitter->function_index);
    write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(pending));
    write_labelf(emitter, "%s:\n", ready);
    if(!instruction->async_cleanup) {
        write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(argument_register(emitter->target,0)),
            x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(emitter,40)));
        write_call(emitter,"__dmm_async_cancel_requested");
        write_x64_2(emitter,X64_OP_TEST,X64_WIDTH_QWORD,x64_register("rax"),x64_register("rax"));
        char cancel[96]; snprintf(cancel,sizeof(cancel),".LIR_%zu_%zu",emitter->function_index,instruction->target_b);
        write_x64_1(emitter,X64_OP_JNE,X64_WIDTH_NONE,x64_label(cancel));
    }
    write_value_load(emitter, "rax", instruction->operand_a);
    if (instruction->result != IR_VALUE_NONE) {
        if (is_inline_structure(emitter->module, instruction) || instruction->is_array) {
            write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(80));
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp", -(long long) aggregate_result_offset(emitter, instruction)));
            if (!copy_async_storage(emitter, instruction->type_id, "rax", "rbx")) return 0;
            write_register_move(emitter, "rax", "rbx");
        } else
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                        x64_memory(X64_WIDTH_QWORD, "rax", 80));
        write_value_store(emitter, "rax", instruction->result);
    }
    async_callback(emitter, instruction->operand_a, 8);
    return 1;
}

static int emit_executor_operation(Emitter *e, const IrInstruction *in) {
    if (in->async_operation==ASYNC_NATIVE_FUTURE) {
        write_value_load(e,"rax",in->operand_a);
        write_value_store(e,"rax",in->result);
        return 1;
    }
    const char *name=NULL;
    if(in->async_operation==ASYNC_BLOCK_ON && in->operand_b==IR_VALUE_NONE) write_call(e,"__dmm_async_default");
    if (in->async_operation==ASYNC_SPAWN || in->async_operation==ASYNC_SHUTDOWN) {
        if (in->operand_b==IR_VALUE_NONE) write_immediate(e,argument_register(e->target,0),0);
        else { write_value_load(e,"rax",in->operand_b); write_register_move(e,argument_register(e->target,0),"rax"); }
        write_value_load(e,"rax",in->operand_a); write_register_move(e,argument_register(e->target,1),"rax");
        if (in->async_operation==ASYNC_SPAWN) {
            size_t slots=type_slots(e->module,e->module->types[in->type_id].element_type);
            write_immediate(e,argument_register(e->target,2),(long long)(slots*8));
            name="__dmm_async_spawn";
        } else name="__dmm_async_shutdown";
    } else {
        write_value_load(e,"rax",in->operand_a); write_register_move(e,argument_register(e->target,0),"rax");
        name=in->async_operation==ASYNC_CREATE ? "__dmm_async_executor_create" :
             in->async_operation==ASYNC_CANCEL ? "__dmm_async_cancel" : "__dmm_async_block_on";
    }
    write_call(e,name);
    if (in->async_operation==ASYNC_BLOCK_ON) {
        if (in->result!=IR_VALUE_NONE) {
            if (is_inline_structure(e->module,in) || in->is_array) {
                write_x64_2(e,X64_OP_ADD,X64_WIDTH_QWORD,x64_register("rax"),x64_immediate(80));
                write_x64_2(e,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rbx"),
                            x64_memory(X64_WIDTH_NONE,"rbp",-(long long)aggregate_result_offset(e,in)));
                if (!copy_async_storage(e,in->type_id,"rax","rbx")) return 0;
                write_register_move(e,"rax","rbx");
            } else write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rax",80));
            write_value_store(e,"rax",in->result);
        }
        async_callback(e,in->operand_a,8);
    } else if (in->result!=IR_VALUE_NONE) write_value_store(e,"rax",in->result);
    return 1;
}

static void async_pending_state(Emitter *e,size_t state) {
    if(!e->async_result_callback)
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(e,16)),x64_immediate((long long)state));
    char pending[96]; snprintf(pending,sizeof(pending),e->async_result_callback ? ".LIR_async_result_pending_%zu":".LIR_async_pending_%zu",e->function_index);
    write_x64_1(e,X64_OP_JMP,X64_WIDTH_NONE,x64_label(pending));
}

/* rax is an address in the pinned frame. Clear each child slot only after
   its cancellation acknowledges cleanup; resuming repeats this walk safely. */
static int async_cancel_children(Emitter *e,IrTypeId id,size_t state) {
    const IrType *t=&e->module->types[id];
    size_t seq=e->bounds_sequence++;
    char done[96]; snprintf(done,sizeof(done),".LIR_cancel_child_done_%zu_%zu",e->function_index,seq);
    if (t->kind==IR_TYPE_FUTURE || t->kind==IR_TYPE_JOIN || t->kind==IR_TYPE_EXECUTOR) {
        write_x64_2(e,X64_OP_CMP,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rax",0),x64_immediate(0));
        write_x64_1(e,X64_OP_JE,X64_WIDTH_NONE,x64_label(done));
        write_x64_2(e,X64_OP_SUB,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(16));
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rsp",0),x64_register("rax"));
        if (t->kind==IR_TYPE_EXECUTOR) {
            char existing[96]; snprintf(existing,sizeof(existing),".LIR_cancel_executor_%zu_%zu",e->function_index,seq);
            write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rax",0));
            write_x64_2(e,X64_OP_TEST,X64_WIDTH_QWORD,x64_register("rax"),x64_immediate(1));
            write_x64_1(e,X64_OP_JNE,X64_WIDTH_NONE,x64_label(existing));
            write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rsp",0));
            write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(argument_register(e->target,0)),x64_memory(X64_WIDTH_QWORD,"rax",0));
            write_immediate(e,argument_register(e->target,1),1);
            write_call(e,"__dmm_async_shutdown");
            write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rbx"),x64_memory(X64_WIDTH_QWORD,"rsp",0));
            write_x64_2(e,X64_OP_OR,X64_WIDTH_QWORD,x64_register("rax"),x64_immediate(1));
            write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rbx",0),x64_register("rax"));
            write_labelf(e,"%s:\n",existing);
            write_x64_2(e,X64_OP_AND,X64_WIDTH_QWORD,x64_register("rax"),x64_immediate(-2));
        } else write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rax",0));
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rsp",8),x64_register("rax"));
        char ready[96]; snprintf(ready,sizeof(ready),".LIR_cancel_child_ready_%zu_%zu",e->function_index,seq);
        char discard[96],polled[96];
        snprintf(discard,sizeof(discard),".LIR_cancel_child_discard_%zu_%zu",e->function_index,seq);
        snprintf(polled,sizeof(polled),".LIR_cancel_child_polled_%zu_%zu",e->function_index,seq);
        write_x64_2(e,X64_OP_CMP,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rax",16),x64_immediate(-1));
        write_x64_1(e,X64_OP_JE,X64_WIDTH_NONE,x64_label(discard));
        write_x64_2(e,X64_OP_CMP,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rax",16),x64_immediate(-2));
        write_x64_1(e,X64_OP_JE,X64_WIDTH_NONE,x64_label(ready));
        write_register_move(e,argument_register(e->target,0),"rax");
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(argument_register(e->target,1)),x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(e,40)));
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("r11"),x64_memory(X64_WIDTH_QWORD,"rax",24));
        if (e->target==TARGET_COFF) write_x64_2(e,X64_OP_SUB,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(32));
        write_x64_1(e,X64_OP_CALL,X64_WIDTH_NONE,x64_register("r11"));
        if (e->target==TARGET_COFF) write_x64_2(e,X64_OP_ADD,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(32));
        write_x64_1(e,X64_OP_JMP,X64_WIDTH_NONE,x64_label(polled));
        write_labelf(e,"%s:\n",discard);
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(argument_register(e->target,0)),x64_memory(X64_WIDTH_QWORD,"rsp",8));
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(argument_register(e->target,1)),x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(e,40)));
        write_call(e,"__dmm_async_discard_poll");
        write_labelf(e,"%s:\n",polled);
        write_x64_2(e,X64_OP_TEST,X64_WIDTH_QWORD,x64_register("rax"),x64_register("rax"));
        write_x64_1(e,X64_OP_JNE,X64_WIDTH_NONE,x64_label(ready));
        async_pending_state(e,state);
        write_labelf(e,"%s:\n",ready);
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rsp",8));
        write_register_move(e,argument_register(e->target,0),"rax");
        write_immediate(e,argument_register(e->target,1),1);
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("r11"),x64_memory(X64_WIDTH_QWORD,"rax",8));
        if(e->target==TARGET_COFF) write_x64_2(e,X64_OP_SUB,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(32));
        write_x64_1(e,X64_OP_CALL,X64_WIDTH_NONE,x64_register("r11"));
        if(e->target==TARGET_COFF) write_x64_2(e,X64_OP_ADD,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(32));
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rsp",0));
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rax",0),x64_immediate(0));
        write_x64_2(e,X64_OP_ADD,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(16));
        write_labelf(e,"%s:\n",done);
        return 1;
    }
    if (t->kind==IR_TYPE_ARRAY || t->kind==IR_TYPE_NAMED) {
        const IrAggregate *a=t->kind==IR_TYPE_NAMED ? aggregate_for_symbol(e->module,t->symbol_id):NULL;
        const IrEnum *en=t->kind==IR_TYPE_NAMED ? enum_for_symbol(e->module,t->symbol_id,NULL):NULL;
        if (!a && !en && t->kind!=IR_TYPE_ARRAY) return 1;
        write_x64_2(e,X64_OP_SUB,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(16));
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rsp",0),x64_register("rax"));
        size_t variants=en ? en->variant_count:1;
        for(size_t v=0;v<variants;v++) {
            char next[96]; snprintf(next,sizeof(next),".LIR_cancel_variant_%zu_%zu_%zu",e->function_index,seq,v);
            if(en) {
                write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rsp",0));
                write_x64_2(e,X64_OP_CMP,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rax",0),x64_immediate((long long)v));
                write_x64_1(e,X64_OP_JNE,X64_WIDTH_NONE,x64_label(next));
            }
            size_t count=a ? a->field_count : en ? en->variants[v].payload_count : t->array_length;
            size_t offset=en ? 8:0;
            for(size_t n=0;n<count;n++) {
                IrTypeId child=a ? a->fields[n].type_id : en ? en->variants[v].payload_types[n] : t->element_type;
                IrTypeLayout layout; if(!ir_type_layout(e->module,child,&layout)) return 0;
                size_t alignment = layout.alignment > 8 ? layout.alignment : 8;
                if (a && a->is_native) offset = a->fields[n].native_offset;
                else if (a || en) offset = (offset + alignment - 1) & ~(alignment - 1);
                write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rsp",0));
                write_x64_2(e,X64_OP_ADD,X64_WIDTH_QWORD,x64_register("rax"),x64_immediate((long long)offset));
                if(!async_cancel_children(e,child,state)) return 0;
                offset+=layout.storage_slots*8;
            }
            if(en) write_labelf(e,"%s:\n",next);
        }
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"rsp",0));
        write_x64_2(e,X64_OP_ADD,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(16));
    }
    return 1;
}

static int emit_async_cancel_instruction(Emitter *e,const IrInstruction *in,size_t index) {
    if(in->opcode==IR_OP_CANCEL_CHECK) {
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_register(argument_register(e->target,0)),x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(e,40)));
        write_call(e,"__dmm_async_cancel_requested");
        char normal[96],cancel[96]; snprintf(normal,sizeof(normal),".LIR_%zu_%zu",e->function_index,in->target_a);
        snprintf(cancel,sizeof(cancel),".LIR_%zu_%zu",e->function_index,in->target_b);
        write_x64_2(e,X64_OP_TEST,X64_WIDTH_QWORD,x64_register("rax"),x64_register("rax"));
        write_x64_1(e,X64_OP_JNE,X64_WIDTH_NONE,x64_label(cancel));
        write_x64_1(e,X64_OP_JMP,X64_WIDTH_NONE,x64_label(normal)); return 1;
    }
    if(in->opcode==IR_OP_CANCEL_RETURN) {
        write_x64_2(e,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(e,16)),x64_immediate(-2));
        write_immediate(e,"rax",1);
        char epilogue[96]; snprintf(epilogue,sizeof(epilogue),".LIR_epilogue_%zu",e->function_index);
        write_x64_1(e,X64_OP_JMP,X64_WIDTH_NONE,x64_label(epilogue)); return 1;
    }
    write_labelf(e,".LIR_async_resume_%zu_%zu:\n",e->function_index,in->target_a);
    if(in->opcode==IR_OP_CANCEL_AWAIT) {
        write_x64_2(e,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_NONE,"r14",-(long long)value_offset(in->operand_a)));
        return async_cancel_children(e,producer(e->function,in->operand_a)->type_id,in->target_a);
    }
    const IrInstruction *local=local_declaration(e->function,in->symbol_id,index);
    const IrParameter *p=function_parameter(e->function,in->symbol_id);
    if(!local&&!p) return 0;
    size_t flag=local ? declaration_flag_offset(e,local):parameter_flag_offset(e,p);
    char done[96]; snprintf(done,sizeof(done),".LIR_cancel_drop_done_%zu_%zu",e->function_index,index);
    write_local_load(e,"rax",flag);
    write_x64_2(e,X64_OP_TEST,X64_WIDTH_QWORD,x64_register("rax"),x64_register("rax"));
    write_x64_1(e,X64_OP_JE,X64_WIDTH_NONE,x64_label(done));
    size_t offset=local ? declaration_offset(e,local):parameter_offset(e,p);
    if(p&&(type_is_structure(e->module,in->type_id)||e->module->types[in->type_id].kind==IR_TYPE_ARRAY)) write_local_load(e,"rax",offset);
    else write_x64_2(e,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_NONE,"r14",-(long long)offset));
    if(!async_cancel_children(e,in->type_id,in->target_a)) return 0;
    IrTypeKind kind=e->module->types[in->type_id].kind;
    if(kind==IR_TYPE_FUTURE || kind==IR_TYPE_JOIN || kind==IR_TYPE_EXECUTOR) {
        write_immediate(e,"rax",0); write_local_store(e,"rax",flag);
    } else {
        IrInstruction drop=*in; drop.opcode=IR_OP_DROP;
        if(!emit_ownership_effect(e,&drop,index)) return 0;
    }
    write_labelf(e,"%s:\n",done); return 1;
}

static void async_complete(Emitter *emitter) {
    IrTypeId output = emitter->function->return_type_id;
    if (type_is_structure(emitter->module, output) ||
        emitter->module->types[output].kind == IR_TYPE_ARRAY) {
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                    x64_memory(X64_WIDTH_NONE, "r14", async_header_offset(emitter, 80)));
        (void) copy_async_storage(emitter, output, "rax", "rbx");
    } else
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, "r14", async_header_offset(emitter, 80)), x64_register("rax"));
    write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(emitter,48)),x64_immediate(1));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "r14", async_header_offset(emitter, 16)), x64_immediate(-1));
    write_immediate(emitter, "rax", 1);
}

/* Called after ordinary parameter spills. No user body runs in a constructor. */
static int async_construct_and_poll(Emitter *emitter) {
    size_t slots = type_slots(emitter->module, emitter->function->return_type_id);
    if (slots == 0) slots = 1;
    emitter->async_frame_bytes = emitter->frame_size + 8 + 80 + slots * 8;
    write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate(16));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "rsp", 0), x64_register("r14"));
    write_immediate(emitter, argument_register(emitter->target, 0), 1);
    write_immediate(emitter, argument_register(emitter->target, 1), (long long) emitter->async_frame_bytes);
    write_call(emitter, "calloc");
    char allocated[80];
    (void) snprintf(allocated, sizeof(allocated), ".LIR_async_allocated_%zu", emitter->function_index);
    write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rax"));
    write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(allocated));
    write_x64_0(emitter, X64_OP_UD2);
    write_labelf(emitter, "%s:\n", allocated);
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("r14"),
                x64_memory(X64_WIDTH_NONE, "rax", (long long) emitter->async_frame_bytes));
    for (size_t p = 0; p < emitter->function->parameter_count; p++) {
        const IrParameter *parameter = &emitter->function->parameters[p];
        async_copy_parameter_word(emitter, parameter_offset(emitter, parameter));
        if (parameter->is_slice) async_copy_parameter_word(emitter, slice_length_offset(emitter, p));
        if (!parameter->is_receiver &&
            (ir_type_properties(emitter->module, parameter->type_id) & SEMANTIC_TYPE_NEEDS_DROP))
            async_copy_parameter_word(emitter, parameter_flag_offset(emitter, parameter));
        if (!type_is_structure(emitter->module, parameter->type_id) &&
            emitter->module->types[parameter->type_id].kind != IR_TYPE_ARRAY) continue;
        size_t copy_offset = parameter_copy_offset(emitter, p);
        for (size_t s = 0; s < type_slots(emitter->module, parameter->type_id); s++)
            async_copy_parameter_word(emitter, copy_offset - s * 8);
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                    x64_memory(X64_WIDTH_NONE, "r14", -(long long) parameter_copy_offset(emitter, p)));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, "r14", -(long long) parameter_offset(emitter, parameter)),
                    x64_register("rbx"));
    }
    X64Operand poll = x64_rip_memory(X64_WIDTH_NONE, ".LIR_async_poll_", emitter->function_index);
    X64Operand cleanup = x64_rip_memory(X64_WIDTH_NONE, ".LIR_async_cleanup_", emitter->function_index);
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"), poll);
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rax", 0), x64_register("rbx"));
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"), cleanup);
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rax", 8), x64_register("rbx"));
    X64Operand cancel = x64_rip_memory(X64_WIDTH_NONE,".LIR_async_cancel_",emitter->function_index);
    X64Operand drop = x64_rip_memory(X64_WIDTH_NONE,".LIR_async_result_drop_",emitter->function_index);
    X64Operand cancel_output=x64_rip_memory(X64_WIDTH_NONE,".LIR_async_result_cancel_",emitter->function_index);
    write_x64_2(emitter,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rbx"),cancel);
    write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rax",24),x64_register("rbx"));
    write_x64_2(emitter,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rbx"),drop);
    write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rax",32),x64_register("rbx"));
    write_x64_2(emitter,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rbx"),cancel_output);
    write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"rax",72),x64_register("rbx"));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r14"), x64_memory(X64_WIDTH_QWORD, "rsp", 0));
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rsp"), x64_memory(X64_WIDTH_NONE, "rbp", -8));
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbx"));
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbp"));
    write_x64_0(emitter, X64_OP_RET);

    write_labelf(emitter, ".LIR_async_cleanup_%zu:\n", emitter->function_index);
    const char *arg = argument_register(emitter->target, 0);
    write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, arg, 16), x64_immediate(-1));
    char valid[80];
    (void) snprintf(valid, sizeof(valid), ".LIR_async_cleanup_valid_%zu", emitter->function_index);
    write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(valid));
    write_x64_2(emitter,X64_OP_CMP,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,arg,16),x64_immediate(-2));
    write_x64_1(emitter,X64_OP_JE,X64_WIDTH_NONE,x64_label(valid));
    write_x64_0(emitter, X64_OP_UD2);
    write_labelf(emitter, "%s:\n", valid);
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbp"));
    write_call(emitter, "__dmm_async_frame_destroy");
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbp"));
    write_x64_0(emitter, X64_OP_RET);

    write_labelf(emitter,".LIR_async_result_drop_%zu:\n",emitter->function_index);
    write_x64_1(emitter,X64_OP_PUSH,X64_WIDTH_QWORD,x64_register("rbp"));
    write_register_move(emitter,"rbp","rsp");
    write_x64_1(emitter,X64_OP_PUSH,X64_WIDTH_QWORD,x64_register("rbx"));
    write_x64_1(emitter,X64_OP_PUSH,X64_WIDTH_QWORD,x64_register("rbx"));
    write_register_move(emitter,"rax",arg);
    if ((ir_type_properties(emitter->module,emitter->function->return_type_id)&SEMANTIC_TYPE_NEEDS_DROP) &&
        !emit_drop_type(emitter,emitter->function->return_type_id)) return 0;
    write_x64_2(emitter,X64_OP_ADD,X64_WIDTH_QWORD,x64_register("rsp"),x64_immediate(8));
    write_x64_1(emitter,X64_OP_POP,X64_WIDTH_QWORD,x64_register("rbx"));
    write_x64_1(emitter,X64_OP_POP,X64_WIDTH_QWORD,x64_register("rbp"));
    write_x64_0(emitter,X64_OP_RET);

    write_labelf(emitter,".LIR_async_result_cancel_%zu:\n",emitter->function_index);
    write_x64_1(emitter,X64_OP_PUSH,X64_WIDTH_QWORD,x64_register("rbp"));
    write_register_move(emitter,"rbp","rsp");
    write_x64_1(emitter,X64_OP_PUSH,X64_WIDTH_QWORD,x64_register("rbx"));
    write_x64_1(emitter,X64_OP_PUSH,X64_WIDTH_QWORD,x64_register("r14"));
    write_x64_2(emitter,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("r14"),x64_memory(X64_WIDTH_NONE,arg,(long long)emitter->async_frame_bytes));
    write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(emitter,40)),
        x64_register(argument_register(emitter->target,1)));
    write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,x64_register("rax"),x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(emitter,64)));
    emitter->async_result_callback=1;
    if(!async_cancel_children(emitter,emitter->function->return_type_id,0)) return 0;
    emitter->async_result_callback=0;
    write_immediate(emitter,"rax",1);
    char result_epilogue[96]; snprintf(result_epilogue,sizeof(result_epilogue),".LIR_async_result_epilogue_%zu",emitter->function_index);
    write_x64_1(emitter,X64_OP_JMP,X64_WIDTH_NONE,x64_label(result_epilogue));
    write_labelf(emitter,".LIR_async_result_pending_%zu:\n",emitter->function_index);
    write_immediate(emitter,"rax",0);
    write_labelf(emitter,"%s:\n",result_epilogue);
    write_x64_2(emitter,X64_OP_LEA,X64_WIDTH_QWORD,x64_register("rsp"),x64_memory(X64_WIDTH_NONE,"rbp",-16));
    write_x64_1(emitter,X64_OP_POP,X64_WIDTH_QWORD,x64_register("r14"));
    write_x64_1(emitter,X64_OP_POP,X64_WIDTH_QWORD,x64_register("rbx"));
    write_x64_1(emitter,X64_OP_POP,X64_WIDTH_QWORD,x64_register("rbp"));
    write_x64_0(emitter,X64_OP_RET);

    /* Both entries preserve pinned storage. The cancellation entry chooses
       the cleanup edge of the suspended state on its first invocation. */
    write_labelf(emitter,".LIR_async_cancel_%zu:\n",emitter->function_index);
    char cancel_body[96]; snprintf(cancel_body,sizeof(cancel_body),".LIR_async_cancel_body_%zu",emitter->function_index);
    write_x64_2(emitter,X64_OP_CMP,X64_WIDTH_QWORD,x64_memory(X64_WIDTH_QWORD,arg,48),x64_immediate(0));
    write_x64_1(emitter,X64_OP_JE,X64_WIDTH_NONE,x64_label(cancel_body));
    write_x64_1(emitter,X64_OP_JMP,X64_WIDTH_NONE,x64_label("__dmm_async_discard_poll"));
    write_labelf(emitter,"%s:\n",cancel_body);
    write_immediate(emitter,"r10",1);
    char common[96]; snprintf(common,sizeof(common),".LIR_async_common_%zu",emitter->function_index);
    write_x64_1(emitter,X64_OP_JMP,X64_WIDTH_NONE,x64_label(common));
    write_labelf(emitter, ".LIR_async_poll_%zu:\n", emitter->function_index);
    write_immediate(emitter,"r10",0);
    write_labelf(emitter,"%s:\n",common);
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbp"));
    write_register_move(emitter, "rbp", "rsp");
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbx"));
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("r14"));
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("r14"),
                x64_memory(X64_WIDTH_NONE, arg, (long long) emitter->async_frame_bytes));
    emitter->async_storage = 1;
    write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,
                 x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(emitter,40)),
                 x64_register(argument_register(emitter->target,1)));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                x64_memory(X64_WIDTH_QWORD, "r14", async_header_offset(emitter, 16)));
    char start[80];
    (void) snprintf(start, sizeof(start), ".LIR_async_start_%zu", emitter->function_index);
    char dispatch[96]; snprintf(dispatch,sizeof(dispatch),".LIR_async_dispatch_%zu",emitter->function_index);
    write_x64_2(emitter,X64_OP_TEST,X64_WIDTH_QWORD,x64_register("r10"),x64_register("r10"));
    write_x64_1(emitter,X64_OP_JE,X64_WIDTH_NONE,x64_label(dispatch));
    write_x64_2(emitter,X64_OP_CMP,X64_WIDTH_QWORD,
                 x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(emitter,56)),x64_immediate(0));
    write_x64_1(emitter,X64_OP_JNE,X64_WIDTH_NONE,x64_label(dispatch));
    write_x64_2(emitter,X64_OP_MOV,X64_WIDTH_QWORD,
                 x64_memory(X64_WIDTH_QWORD,"r14",async_header_offset(emitter,56)),x64_immediate(1));
    char entry[96]; snprintf(entry,sizeof(entry),".LIR_%zu_%zu",emitter->function_index,emitter->function->async_cancel_entry);
    write_x64_2(emitter,X64_OP_CMP,X64_WIDTH_QWORD,x64_register("rax"),x64_immediate(0));
    write_x64_1(emitter,X64_OP_JE,X64_WIDTH_NONE,x64_label(entry));
    for (size_t i=0;i<emitter->function->instruction_count;i++) {
        const IrInstruction *in=&emitter->function->instructions[i];
        if (in->opcode!=IR_OP_AWAIT || in->async_cleanup) continue;
        snprintf(entry,sizeof(entry),".LIR_%zu_%zu",emitter->function_index,in->target_b);
        write_x64_2(emitter,X64_OP_CMP,X64_WIDTH_QWORD,x64_register("rax"),x64_immediate((long long)in->target_a));
        write_x64_1(emitter,X64_OP_JE,X64_WIDTH_NONE,x64_label(entry));
    }
    write_labelf(emitter,"%s:\n",dispatch);
    write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(0));
    write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(start));
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *instruction = &emitter->function->instructions[i];
        if (instruction->opcode != IR_OP_AWAIT && instruction->opcode != IR_OP_CANCEL_AWAIT &&
            instruction->opcode != IR_OP_CANCEL_DROP) continue;
        char resume[96];
        (void) snprintf(resume, sizeof(resume), ".LIR_async_resume_%zu_%zu",
                        emitter->function_index, instruction->target_a);
        write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate((long long) instruction->target_a));
        write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(resume));
    }
    write_x64_0(emitter, X64_OP_UD2);
    write_labelf(emitter, "%s:\n", start);
    return 1;
}
