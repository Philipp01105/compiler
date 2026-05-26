/* Memory widths and eight-byte aggregate/array ABI slots are distinct. */
static int type_layout_depth(const IrModule *module,IrTypeId id,IrTypeLayout *layout,size_t depth) {
    if (id>=module->type_count || depth>module->type_count) return 0;
    const IrType *type=&module->types[id];
    *layout=(IrTypeLayout){.size=8,.alignment=8,.storage_slots=1};
    if (type->kind == IR_TYPE_POINTER) return 1;
    if (type->kind == IR_TYPE_SLICE) { layout->size=16; layout->storage_slots=2; return 1; }
    if (type->kind == IR_TYPE_PRIMITIVE) {
        if (type->primitive == TYPE_VOID || type->primitive == TYPE_UNKNOWN) return 0;
        layout->size=data_type_bytes(type->primitive); layout->alignment=layout->size; return 1;
    }
    size_t slots=0;
    if (type->kind == IR_TYPE_ARRAY) {
        IrTypeLayout element;
        if (!type_layout_depth(module,type->element_type,&element,depth+1) || !type->array_length ||
            type->array_length>SIZE_MAX/element.storage_slots) return 0;
        slots=type->array_length*element.storage_slots;
    } else {
        int found=0;
        for (size_t s=0;s<module->structure_count;s++) {
            const IrAggregate *structure=&module->structures[s];
            if (structure->symbol_id != type->symbol_id) continue;
            found=1;
            for (size_t f=0;f<structure->field_count;f++) {
                IrTypeLayout field;
                if (!type_layout_depth(module,structure->fields[f].type_id,&field,depth+1) || field.storage_slots>SIZE_MAX-slots) return 0;
                slots+=field.storage_slots;
            }
            if (!slots) slots=1;
        }
        for (size_t e=0;e<module->enum_count;e++) {
            const IrEnum *enumeration=&module->enums[e];
            if (enumeration->symbol_id != type->symbol_id) continue;
            found=1; size_t largest=0;
            for (size_t v=0;v<enumeration->variant_count;v++) {
                const IrEnumVariant *variant=&enumeration->variants[v]; size_t payload=0;
                for (size_t p=0;p<variant->payload_count;p++) {
                    IrTypeLayout value;
                    if (!type_layout_depth(module,variant->payload_types[p],&value,depth+1) || value.storage_slots>SIZE_MAX-payload) return 0;
                    payload+=value.storage_slots;
                }
                if (payload>largest) largest=payload;
            }
            if (largest == SIZE_MAX) return 0;
            slots=largest+1;
        }
        if (!found) return 0;
    }
    if (!slots || slots>SIZE_MAX/8) return 0;
    layout->size=slots*8; layout->storage_slots=slots; return 1;
}

int ir_type_layout(const IrModule *module,IrTypeId type,IrTypeLayout *layout) {
    return module && layout && type_layout_depth(module,type,layout,0);
}
