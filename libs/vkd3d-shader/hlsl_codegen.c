/*
 * HLSL optimization and code generation
 *
 * Copyright 2019-2020 Zebediah Figura for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "hlsl.h"
#include <stdio.h>

/* Split uniforms into two variables representing the constant and temp
 * registers, and copy the former to the latter, so that writes to uniforms
 * work. */
static void prepend_uniform_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var)
{
    struct vkd3d_string_buffer *name;
    struct hlsl_ir_var *const_var;
    struct hlsl_ir_store *store;
    struct hlsl_ir_load *load;

    if (!(name = vkd3d_string_buffer_get(&ctx->string_buffers)))
    {
        ctx->failed = true;
        return;
    }
    vkd3d_string_buffer_printf(name, "<uniform-%s>", var->name);
    if (!(const_var = hlsl_new_var(vkd3d_strdup(name->buffer), var->data_type, var->loc, NULL, var->reg_reservation)))
    {
        vkd3d_string_buffer_release(&ctx->string_buffers, name);
        ctx->failed = true;
        return;
    }
    vkd3d_string_buffer_release(&ctx->string_buffers, name);
    list_add_head(&ctx->globals->vars, &const_var->scope_entry);
    var->is_uniform = 0;
    const_var->is_uniform = 1;

    if (!(load = hlsl_new_var_load(const_var, var->loc)))
    {
        ctx->failed = true;
        return;
    }
    list_add_head(instrs, &load->node.entry);

    if (!(store = hlsl_new_simple_store(var, &load->node)))
    {
        ctx->failed = true;
        return;
    }
    list_add_after(&load->node.entry, &store->node.entry);
}

static void prepend_input_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var,
        struct hlsl_type *type, unsigned int field_offset, const char *semantic)
{
    struct vkd3d_string_buffer *name;
    struct hlsl_ir_constant *offset;
    struct hlsl_ir_store *store;
    struct hlsl_ir_var *varying;
    struct hlsl_ir_load *load;

    if (!(name = vkd3d_string_buffer_get(&ctx->string_buffers)))
    {
        ctx->failed = true;
        return;
    }
    vkd3d_string_buffer_printf(name, "<input-%s>", semantic);
    if (!(varying = hlsl_new_var(vkd3d_strdup(name->buffer), type, var->loc, vkd3d_strdup(semantic), NULL)))
    {
        vkd3d_string_buffer_release(&ctx->string_buffers, name);
        ctx->failed = true;
        return;
    }
    vkd3d_string_buffer_release(&ctx->string_buffers, name);
    varying->is_input_varying = 1;
    list_add_head(&ctx->globals->vars, &varying->scope_entry);

    if (!(load = hlsl_new_var_load(varying, var->loc)))
    {
        ctx->failed = true;
        return;
    }
    list_add_head(instrs, &load->node.entry);

    if (!(offset = hlsl_new_uint_constant(ctx, field_offset * 4, var->loc)))
    {
        ctx->failed = true;
        return;
    }
    list_add_after(&load->node.entry, &offset->node.entry);

    if (!(store = hlsl_new_store(var, &offset->node, &load->node, 0, var->loc)))
    {
        ctx->failed = true;
        return;
    }
    list_add_after(&offset->node.entry, &store->node.entry);
}

static void prepend_input_struct_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var,
        struct hlsl_type *type, unsigned int field_offset)
{
    struct hlsl_struct_field *field;

    LIST_FOR_EACH_ENTRY(field, type->e.elements, struct hlsl_struct_field, entry)
    {
        if (field->type->type == HLSL_CLASS_STRUCT)
            prepend_input_struct_copy(ctx, instrs, var, field->type, field_offset + field->reg_offset);
        else if (field->semantic)
            prepend_input_copy(ctx, instrs, var, field->type, field_offset + field->reg_offset, field->semantic);
        else
            hlsl_error(ctx, field->loc, VKD3D_SHADER_ERROR_HLSL_MISSING_SEMANTIC,
                    "Field '%s' is missing a semantic.", field->name);
    }
}

/* Split input varyings into two variables representing the varying and temp
 * registers, and copy the former to the latter, so that writes to input
 * varyings work. */
static void prepend_input_var_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var)
{
    if (var->data_type->type == HLSL_CLASS_STRUCT)
        prepend_input_struct_copy(ctx, instrs, var, var->data_type, 0);
    else if (var->semantic)
        prepend_input_copy(ctx, instrs, var, var->data_type, 0, var->semantic);

    var->is_input_varying = 0;
}

static void append_output_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var,
        struct hlsl_type *type, unsigned int field_offset, const char *semantic)
{
    struct vkd3d_string_buffer *name;
    struct hlsl_ir_constant *offset;
    struct hlsl_ir_store *store;
    struct hlsl_ir_var *varying;
    struct hlsl_ir_load *load;

    if (!(name = vkd3d_string_buffer_get(&ctx->string_buffers)))
    {
        ctx->failed = true;
        return;
    }
    vkd3d_string_buffer_printf(name, "<output-%s>", semantic);
    if (!(varying = hlsl_new_var(vkd3d_strdup(name->buffer), type, var->loc, vkd3d_strdup(semantic), NULL)))
    {
        vkd3d_string_buffer_release(&ctx->string_buffers, name);
        ctx->failed = true;
        return;
    }
    vkd3d_string_buffer_release(&ctx->string_buffers, name);
    varying->is_output_varying = 1;
    list_add_head(&ctx->globals->vars, &varying->scope_entry);

    if (!(offset = hlsl_new_uint_constant(ctx, field_offset * 4, var->loc)))
    {
        ctx->failed = true;
        return;
    }
    list_add_tail(instrs, &offset->node.entry);

    if (!(load = hlsl_new_load(var, &offset->node, type, var->loc)))
    {
        ctx->failed = true;
        return;
    }
    list_add_after(&offset->node.entry, &load->node.entry);

    if (!(store = hlsl_new_store(varying, NULL, &load->node, 0, var->loc)))
    {
        ctx->failed = true;
        return;
    }
    list_add_after(&load->node.entry, &store->node.entry);
}

static void append_output_struct_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var,
        struct hlsl_type *type, unsigned int field_offset)
{
    struct hlsl_struct_field *field;

    LIST_FOR_EACH_ENTRY(field, type->e.elements, struct hlsl_struct_field, entry)
    {
        if (field->type->type == HLSL_CLASS_STRUCT)
            append_output_struct_copy(ctx, instrs, var, field->type, field_offset + field->reg_offset);
        else if (field->semantic)
            append_output_copy(ctx, instrs, var, field->type, field_offset + field->reg_offset, field->semantic);
        else
            hlsl_error(ctx, field->loc, VKD3D_SHADER_ERROR_HLSL_MISSING_SEMANTIC,
                    "Field '%s' is missing a semantic.", field->name);
    }
}

/* Split output varyings into two variables representing the temp and varying
 * registers, and copy the former to the latter, so that reads from output
 * varyings work. */
static void append_output_var_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var)
{
    if (var->data_type->type == HLSL_CLASS_STRUCT)
        append_output_struct_copy(ctx, instrs, var, var->data_type, 0);
    else if (var->semantic)
        append_output_copy(ctx, instrs, var, var->data_type, 0, var->semantic);

    var->is_output_varying = 0;
}

static bool transform_ir(struct hlsl_ctx *ctx, bool (*func)(struct hlsl_ctx *ctx, struct hlsl_ir_node *, void *),
        struct list *instrs, void *context)
{
    struct hlsl_ir_node *instr, *next;
    bool progress = 0;

    LIST_FOR_EACH_ENTRY_SAFE(instr, next, instrs, struct hlsl_ir_node, entry)
    {
        if (instr->type == HLSL_IR_IF)
        {
            struct hlsl_ir_if *iff = hlsl_ir_if(instr);

            progress |= transform_ir(ctx, func, &iff->then_instrs, context);
            progress |= transform_ir(ctx, func, &iff->else_instrs, context);
        }
        else if (instr->type == HLSL_IR_LOOP)
            progress |= transform_ir(ctx, func, &hlsl_ir_loop(instr)->body, context);

        progress |= func(ctx, instr, context);
    }

    return progress;
}

static void replace_node(struct hlsl_ir_node *old, struct hlsl_ir_node *new)
{
    struct hlsl_src *src, *next;

    LIST_FOR_EACH_ENTRY_SAFE(src, next, &old->uses, struct hlsl_src, entry)
    {
        hlsl_src_remove(src);
        hlsl_src_from_node(src, new);
    }
    list_remove(&old->entry);
    hlsl_free_instr(old);
}

static bool is_vec1(const struct hlsl_type *type)
{
    return (type->type == HLSL_CLASS_SCALAR) || (type->type == HLSL_CLASS_VECTOR && type->dimx == 1);
}

static bool fold_redundant_casts(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    if (instr->type == HLSL_IR_EXPR)
    {
        struct hlsl_ir_expr *expr = hlsl_ir_expr(instr);
        const struct hlsl_type *src_type = expr->operands[0].node->data_type;
        const struct hlsl_type *dst_type = expr->node.data_type;

        if (expr->op != HLSL_IR_UNOP_CAST)
            return false;

        if (hlsl_types_are_equal(src_type, dst_type)
                || (src_type->base_type == dst_type->base_type && is_vec1(src_type) && is_vec1(dst_type)))
        {
            replace_node(&expr->node, expr->operands[0].node);
            return true;
        }
    }

    return false;
}

static bool split_struct_copies(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    const struct hlsl_struct_field *field;
    const struct hlsl_ir_load *rhs_load;
    const struct hlsl_ir_node *rhs;
    const struct hlsl_type *type;
    struct hlsl_ir_store *store;

    if (instr->type != HLSL_IR_STORE)
        return false;

    store = hlsl_ir_store(instr);
    rhs = store->rhs.node;
    type = rhs->data_type;
    if (type->type != HLSL_CLASS_STRUCT)
        return false;

    rhs_load = hlsl_ir_load(rhs);

    LIST_FOR_EACH_ENTRY(field, type->e.elements, struct hlsl_struct_field, entry)
    {
        struct hlsl_ir_store *field_store;
        struct hlsl_ir_node *offset, *add;
        struct hlsl_ir_load *field_load;
        struct hlsl_ir_constant *c;

        if (!(c = hlsl_new_uint_constant(ctx, field->reg_offset * 4, instr->loc)))
        {
            ctx->failed = true;
            return false;
        }
        list_add_before(&instr->entry, &c->node.entry);

        offset = &c->node;
        if (rhs_load->src.offset.node)
        {
            if (!(add = hlsl_new_binary_expr(HLSL_IR_BINOP_ADD, rhs_load->src.offset.node, &c->node)))
            {
                ctx->failed = true;
                return false;
            }
            list_add_before(&instr->entry, &add->entry);
            offset = add;
        }
        if (!(field_load = hlsl_new_load(rhs_load->src.var, offset, field->type, instr->loc)))
        {
            ctx->failed = true;
            return false;
        }
        list_add_before(&instr->entry, &field_load->node.entry);

        offset = &c->node;
        if (store->lhs.offset.node)
        {
            if (!(add = hlsl_new_binary_expr(HLSL_IR_BINOP_ADD, store->lhs.offset.node, &c->node)))
            {
                ctx->failed = true;
                return false;
            }
            list_add_before(&instr->entry, &add->entry);
            offset = add;
        }

        if (!(field_store = hlsl_new_store(store->lhs.var, offset, &field_load->node, 0, instr->loc)))
        {
            ctx->failed = true;
            return false;
        }
        list_add_before(&instr->entry, &field_store->node.entry);
    }

    /* Remove the store instruction, so that we can split structs which contain
     * other structs. Although assignments produce a value, we don't allow
     * HLSL_IR_STORE to be used as a source. */
    list_remove(&store->node.entry);
    hlsl_free_instr(&store->node);
    return true;
}

static bool fold_constants(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    struct hlsl_ir_constant *arg1, *arg2 = NULL, *res;
    struct hlsl_ir_expr *expr;
    unsigned int i;

    if (instr->type != HLSL_IR_EXPR)
        return false;
    expr = hlsl_ir_expr(instr);

    for (i = 0; i < ARRAY_SIZE(expr->operands); ++i)
    {
        if (expr->operands[i].node && expr->operands[i].node->type != HLSL_IR_CONSTANT)
            return false;
    }
    arg1 = hlsl_ir_constant(expr->operands[0].node);
    if (expr->operands[1].node)
        arg2 = hlsl_ir_constant(expr->operands[1].node);

    if (!(res = vkd3d_calloc(1, sizeof(*res))))
    {
        ctx->failed = true;
        return false;
    }
    init_node(&res->node, HLSL_IR_CONSTANT, instr->data_type, instr->loc);

    switch (instr->data_type->base_type)
    {
        case HLSL_TYPE_UINT:
        {
            unsigned int i;

            switch (expr->op)
            {
                case HLSL_IR_BINOP_ADD:
                    for (i = 0; i < instr->data_type->dimx; ++i)
                        res->value.u[i] = arg1->value.u[i] + arg2->value.u[i];
                    break;

                case HLSL_IR_BINOP_MUL:
                    for (i = 0; i < instr->data_type->dimx; ++i)
                        res->value.u[i] = arg1->value.u[i] * arg2->value.u[i];
                    break;

                default:
                    FIXME("Fold uint op %#x.\n", expr->op);
                    vkd3d_free(res);
                    return false;
            }
            break;
        }

        default:
            FIXME("Fold type %#x op %#x.\n", instr->data_type->base_type, expr->op);
            vkd3d_free(res);
            return false;
    }

    list_add_before(&expr->node.entry, &res->node.entry);
    replace_node(&expr->node, &res->node);
    return true;
}

static bool dce(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    switch (instr->type)
    {
        case HLSL_IR_CONSTANT:
        case HLSL_IR_EXPR:
        case HLSL_IR_LOAD:
        case HLSL_IR_SWIZZLE:
            if (list_empty(&instr->uses))
            {
                list_remove(&instr->entry);
                hlsl_free_instr(instr);
                return true;
            }
            break;

        case HLSL_IR_STORE:
        {
            struct hlsl_ir_store *store = hlsl_ir_store(instr);
            struct hlsl_ir_var *var = store->lhs.var;

            if (var->last_read < instr->index)
            {
                list_remove(&instr->entry);
                hlsl_free_instr(instr);
                return true;
            }
            break;
        }

        case HLSL_IR_IF:
        case HLSL_IR_JUMP:
        case HLSL_IR_LOOP:
            break;
    }

    return false;
}

/* Allocate a unique, ordered index to each instruction, which will be used for
 * computing liveness ranges. */
static unsigned int index_instructions(struct list *instrs, unsigned int index)
{
    struct hlsl_ir_node *instr;

    LIST_FOR_EACH_ENTRY(instr, instrs, struct hlsl_ir_node, entry)
    {
        instr->index = index++;

        if (instr->type == HLSL_IR_IF)
        {
            struct hlsl_ir_if *iff = hlsl_ir_if(instr);
            index = index_instructions(&iff->then_instrs, index);
            index = index_instructions(&iff->else_instrs, index);
        }
        else if (instr->type == HLSL_IR_LOOP)
        {
            index = index_instructions(&hlsl_ir_loop(instr)->body, index);
            hlsl_ir_loop(instr)->next_index = index;
        }
    }

    return index;
}

static void dump_function_decl(struct rb_entry *entry, void *context)
{
    struct hlsl_ir_function_decl *func = RB_ENTRY_VALUE(entry, struct hlsl_ir_function_decl, entry);

    if (func->body)
        hlsl_dump_function(func);
}

static void dump_function(struct rb_entry *entry, void *context)
{
    struct hlsl_ir_function *func = RB_ENTRY_VALUE(entry, struct hlsl_ir_function, entry);
    rb_for_each_entry(&func->overloads, dump_function_decl, NULL);
}

/* Compute the earliest and latest liveness for each variable. In the case that
 * a variable is accessed inside of a loop, we promote its liveness to extend
 * to at least the range of the entire loop. Note that we don't need to do this
 * for anonymous nodes, since there's currently no way to use a node which was
 * calculated in an earlier iteration of the loop. */
static void compute_liveness_recurse(struct list *instrs, unsigned int loop_first, unsigned int loop_last)
{
    struct hlsl_ir_node *instr;
    struct hlsl_ir_var *var;

    LIST_FOR_EACH_ENTRY(instr, instrs, struct hlsl_ir_node, entry)
    {
        switch (instr->type)
        {
        case HLSL_IR_STORE:
        {
            struct hlsl_ir_store *store = hlsl_ir_store(instr);

            var = store->lhs.var;
            if (!var->first_write)
                var->first_write = loop_first ? min(instr->index, loop_first) : instr->index;
            store->rhs.node->last_read = instr->index;
            if (store->lhs.offset.node)
                store->lhs.offset.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_EXPR:
        {
            struct hlsl_ir_expr *expr = hlsl_ir_expr(instr);
            unsigned int i;

            for (i = 0; i < ARRAY_SIZE(expr->operands) && expr->operands[i].node; ++i)
                expr->operands[i].node->last_read = instr->index;
            break;
        }
        case HLSL_IR_IF:
        {
            struct hlsl_ir_if *iff = hlsl_ir_if(instr);

            compute_liveness_recurse(&iff->then_instrs, loop_first, loop_last);
            compute_liveness_recurse(&iff->else_instrs, loop_first, loop_last);
            iff->condition.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_LOAD:
        {
            struct hlsl_ir_load *load = hlsl_ir_load(instr);

            var = load->src.var;
            var->last_read = max(var->last_read, loop_last ? max(instr->index, loop_last) : instr->index);
            if (load->src.offset.node)
                load->src.offset.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_LOOP:
        {
            struct hlsl_ir_loop *loop = hlsl_ir_loop(instr);

            compute_liveness_recurse(&loop->body, loop_first ? loop_first : instr->index,
                    loop_last ? loop_last : loop->next_index);
            break;
        }
        case HLSL_IR_SWIZZLE:
        {
            struct hlsl_ir_swizzle *swizzle = hlsl_ir_swizzle(instr);

            swizzle->val.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_CONSTANT:
        case HLSL_IR_JUMP:
            break;
        }
    }
}

static void compute_liveness(struct hlsl_ctx *ctx, struct hlsl_ir_function_decl *entry_func)
{
    struct hlsl_scope *scope;
    struct hlsl_ir_var *var;

    /* Index 0 means unused; index 1 means function entry, so start at 2. */
    index_instructions(entry_func->body, 2);

    LIST_FOR_EACH_ENTRY(scope, &ctx->scopes, struct hlsl_scope, entry)
    {
        LIST_FOR_EACH_ENTRY(var, &scope->vars, struct hlsl_ir_var, scope_entry)
            var->first_write = var->last_read = 0;
    }

    LIST_FOR_EACH_ENTRY(var, &ctx->globals->vars, struct hlsl_ir_var, scope_entry)
    {
        if (var->is_uniform || var->is_input_varying)
            var->first_write = 1;
        else if (var->is_output_varying)
            var->last_read = UINT_MAX;
    }

    LIST_FOR_EACH_ENTRY(var, entry_func->parameters, struct hlsl_ir_var, param_entry)
    {
        var->first_write = 1;
    }

    if (entry_func->return_var)
        entry_func->return_var->last_read = UINT_MAX;

    compute_liveness_recurse(entry_func->body, 0, 0);
}

int hlsl_emit_dxbc(struct hlsl_ctx *ctx, struct hlsl_ir_function_decl *entry_func)
{
    struct hlsl_ir_var *var;

    list_move_head(entry_func->body, &ctx->static_initializers);

    LIST_FOR_EACH_ENTRY(var, &ctx->globals->vars, struct hlsl_ir_var, scope_entry)
    {
        if (var->is_uniform)
            prepend_uniform_copy(ctx, entry_func->body, var);
    }

    LIST_FOR_EACH_ENTRY(var, entry_func->parameters, struct hlsl_ir_var, param_entry)
    {
        if (var->is_uniform)
            prepend_uniform_copy(ctx, entry_func->body, var);
        if (var->is_input_varying)
            prepend_input_var_copy(ctx, entry_func->body, var);
        if (var->is_output_varying)
            append_output_var_copy(ctx, entry_func->body, var);
    }
    if (entry_func->return_var)
        append_output_var_copy(ctx, entry_func->body, entry_func->return_var);

    while (transform_ir(ctx, fold_redundant_casts, entry_func->body, NULL));
    while (transform_ir(ctx, split_struct_copies, entry_func->body, NULL));
    while (transform_ir(ctx, fold_constants, entry_func->body, NULL));

    do
        compute_liveness(ctx, entry_func);
    while (transform_ir(ctx, dce, entry_func->body, NULL));

    compute_liveness(ctx, entry_func);

    if (TRACE_ON())
        rb_for_each_entry(&ctx->functions, dump_function, NULL);

    if (ctx->failed)
        return VKD3D_ERROR_INVALID_SHADER;
    return VKD3D_ERROR_NOT_IMPLEMENTED;
}
