/* Memory widths and eight-byte aggregate/array ABI slots are distinct. */
static int type_layout_depth(const IrModule *module, IrTypeId id, IrTypeLayout *layout, size_t depth) {
    if (id >= module->type_count || depth > module->type_count) return 0;
    const IrType *type = &module->types[id];
    *layout = (IrTypeLayout)
    {
        .size = 8,.alignment = 8,.storage_slots = 1
    };
    if (type->kind == IR_TYPE_POINTER || type->kind == IR_TYPE_FUNCTION ||
        type->kind == IR_TYPE_FUTURE || type->kind == IR_TYPE_JOIN || type->kind == IR_TYPE_EXECUTOR) return 1;
    if (type->kind == IR_TYPE_SLICE) {
        layout->size = 16;
        layout->storage_slots = 2;
        return 1;
    }
    if (type->kind == IR_TYPE_PRIMITIVE) {
        if (!data_type_has_value(type->primitive) || type->primitive == TYPE_UNKNOWN) return 0;
        layout->size = data_type_bytes(type->primitive);
        layout->alignment = layout->size;
        return 1;
    }
    size_t slots = 0;
    if (type->kind == IR_TYPE_ARRAY) {
        IrTypeLayout element;
        if (!type_layout_depth(module, type->element_type, &element, depth + 1) || !type->array_length ||
            type->array_length > SIZE_MAX / element.storage_slots)
            return 0;
        slots = type->array_length * element.storage_slots;
        if (element.alignment > layout->alignment) layout->alignment = element.alignment;
    } else {
        int found = 0;
        if (type->symbol_id < module->semantics->symbol_count &&
            module->semantics->symbols[type->symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE) {
            found = 1;
            slots = 2;
        }
        for (size_t s = 0; s < module->structure_count; s++) {
            const IrAggregate *structure = &module->structures[s];
            if (structure->symbol_id != type->symbol_id) continue;
            if (structure->is_native) {
                if (structure->is_opaque || !structure->native_layout.size ||
                    structure->native_layout.size > SIZE_MAX - 7) return 0;
                layout->size = structure->native_layout.size;
                layout->alignment = structure->native_layout.alignment;
                layout->storage_slots = (layout->size + 7) / 8;
                return 1;
            }
            found = 1;
            for (size_t f = 0; f < structure->field_count; f++) {
                IrTypeLayout field;
                if (!type_layout_depth(module, structure->fields[f].type_id, &field, depth + 1) || field.storage_slots >
                    SIZE_MAX - slots) return 0;
                size_t alignment_slots = field.alignment > 8 ? field.alignment / 8 : 1;
                if (slots > SIZE_MAX - (alignment_slots - 1)) return 0;
                slots = (slots + alignment_slots - 1) & ~(alignment_slots - 1);
                if (field.storage_slots > SIZE_MAX - slots) return 0;
                slots += field.storage_slots;
                if (field.alignment > layout->alignment) layout->alignment = field.alignment;
            }
            if (!slots) slots = 1;
        }
        for (size_t e = 0; e < module->enum_count; e++) {
            const IrEnum *enumeration = &module->enums[e];
            if (enumeration->symbol_id != type->symbol_id) continue;
            found = 1;
            size_t largest = 1;
            for (size_t v = 0; v < enumeration->variant_count; v++) {
                const IrEnumVariant *variant = &enumeration->variants[v];
                size_t payload = 1;
                for (size_t p = 0; p < variant->payload_count; p++) {
                    IrTypeLayout value;
                    if (!type_layout_depth(module, variant->payload_types[p], &value, depth + 1) || value.storage_slots
                        > SIZE_MAX - payload) return 0;
                    size_t alignment_slots = value.alignment > 8 ? value.alignment / 8 : 1;
                    if (payload > SIZE_MAX - (alignment_slots - 1)) return 0;
                    payload = (payload + alignment_slots - 1) & ~(alignment_slots - 1);
                    if (value.storage_slots > SIZE_MAX - payload) return 0;
                    payload += value.storage_slots;
                    if (value.alignment > layout->alignment) layout->alignment = value.alignment;
                }
                if (payload > largest) largest = payload;
            }
            if (largest == SIZE_MAX) return 0;
            slots = largest;
        }
        if (!found) return 0;
    }
    if (!slots || slots > SIZE_MAX / 8) return 0;
    size_t alignment_slots = layout->alignment / 8;
    if (slots > SIZE_MAX - (alignment_slots - 1)) return 0;
    slots = (slots + alignment_slots - 1) & ~(alignment_slots - 1);
    if (slots > SIZE_MAX / 8) return 0;
    layout->size = slots * 8;
    layout->storage_slots = slots;
    return 1;
}

int ir_type_layout(const IrModule *module, IrTypeId type, IrTypeLayout *layout) {
    return module && layout && type_layout_depth(module, type, layout, 0);
}

static unsigned ir_type_properties_depth(const IrModule *module, IrTypeId id,
                                         size_t depth) {
    if (module == NULL || id >= module->type_count || depth > module->type_count)
        return 0;
    const IrType *type = &module->types[id];
    if (type->kind == IR_TYPE_PRIMITIVE)
        return SEMANTIC_TYPE_COPYABLE | SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC;
    if (type->kind == IR_TYPE_FUTURE || type->kind == IR_TYPE_JOIN || type->kind == IR_TYPE_EXECUTOR)
        return SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP | SEMANTIC_TYPE_MUST_CONSUME;
    if (type->kind == IR_TYPE_POINTER || type->kind == IR_TYPE_FUNCTION || type->kind == IR_TYPE_SLICE)
        return SEMANTIC_TYPE_COPYABLE;
    if (type->kind == IR_TYPE_ARRAY)
        return ir_type_properties_depth(module, type->element_type, depth + 1);
    for (size_t i = 0; i < module->structure_count; i++)
        if (module->structures[i].symbol_id == type->symbol_id)
            return module->structures[i].type_properties;
    return semantic_symbol_type_properties(module->semantics, type->symbol_id);
}

unsigned ir_type_properties(const IrModule *module, IrTypeId type) {
    return ir_type_properties_depth(module, type, 0);
}
