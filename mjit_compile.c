/**********************************************************************

  mjit_compile.c - MRI method JIT compiler

  Copyright (C) 2017 Takashi Kokubun <takashikkbn@gmail.com>.

**********************************************************************/

#include "internal.h"
#include "vm_core.h"
#include "vm_exec.h"
#include "mjit.h"
#include "insns.inc"
#include "insns_info.inc"
#include "vm_insnhelper.h"

/* Storage to keep compiler's status.  This should have information
   which is global during one `mjit_compile` call.  Ones conditional
   in each branch should be stored in `compile_branch`.  */
struct compile_status {
    int success; /* has TRUE if compilation has had no issue */
    int *compiled_for_pos; /* compiled_for_pos[pos] has TRUE if the pos is compiled */
    /* If TRUE, JIT-ed code will use local variables to store pushed values instead of
       using VM's stack and moving stack pointer. */
    int local_stack_p;
};

/* Storage to keep data which is consistent in each conditional branch.
   This is created and used for one `compile_insns` call and its values
   should be copied for extra `compile_insns` call. */
struct compile_branch {
    unsigned int stack_size; /* this simulates sp (stack pointer) of YARV */
    int finish_p; /* if TRUE, compilation in this branch should stop and let another branch to be compiled */
};

struct case_dispatch_var {
    FILE *f;
    unsigned int base_pos;
    VALUE last_value;
};

/* Returns iseq from cc if it's available and still not obsoleted. */
static const rb_iseq_t *
get_iseq_if_available(CALL_CACHE cc)
{
    if (GET_GLOBAL_METHOD_STATE() == cc->method_state
        && mjit_valid_class_serial_p(cc->class_serial)
        && cc->me && cc->me->def->type == VM_METHOD_TYPE_ISEQ) {
        return rb_iseq_check(cc->me->def->body.iseq.iseqptr);
    }
    return NULL;
}

/* Returns TRUE if iseq is inlinable, otherwise NULL. This becomes TRUE in the same condition
   as CI_SET_FASTPATH (in vm_callee_setup_arg) is called from vm_call_iseq_setup. */
static int
inlinable_iseq_p(CALL_INFO ci, CALL_CACHE cc, const rb_iseq_t *iseq)
{
    extern int rb_simple_iseq_p(const rb_iseq_t *iseq);
    return iseq != NULL
        && rb_simple_iseq_p(iseq) && !(ci->flag & VM_CALL_KW_SPLAT) /* top of vm_callee_setup_arg */
        && (!IS_ARGS_SPLAT(ci) && !IS_ARGS_KEYWORD(ci) && !(METHOD_ENTRY_VISI(cc->me) == METHOD_VISI_PROTECTED)); /* CI_SET_FASTPATH */
}

static int
compile_case_dispatch_each(VALUE key, VALUE value, VALUE arg)
{
    struct case_dispatch_var *var = (struct case_dispatch_var *)arg;
    unsigned int offset;

    if (var->last_value != value) {
        offset = FIX2INT(value);
        var->last_value = value;
        fprintf(var->f, "      case %d:\n", offset);
        fprintf(var->f, "        goto label_%d;\n", var->base_pos + offset);
        fprintf(var->f, "        break;\n");
    }
    return ST_CONTINUE;
}

/* Calling rb_id2str in MJIT worker causes random SEGV. So this is disabled by default. */
static void
comment_id(FILE *f, ID id)
{
#ifdef MJIT_COMMENT_ID
    VALUE name = rb_id2str(id);
    const char *p, *e;
    char c, prev = '\0';

    if (!name) return;
    p = RSTRING_PTR(name);
    e = RSTRING_END(name);
    fputs("/* :\"", f);
    for (; p < e; ++p) {
	switch (c = *p) {
	  case '*': case '/': if (prev != (c ^ ('/' ^ '*'))) break;
	  case '\\': case '"': fputc('\\', f);
	}
	fputc(c, f);
	prev = c;
    }
    fputs("\" */", f);
#endif
}

static void compile_insns(FILE *f, const struct rb_iseq_constant_body *body, unsigned int stack_size,
                          unsigned int pos, struct compile_status *status);

/* Main function of JIT compilation, vm_exec_core counterpart for JIT. Compile one insn to `f`, may modify
   b->stack_size and return next position.

   When you add a new instruction to insns.def, it would be nice to have JIT compilation support here but
   it's optional. This JIT compiler just ignores ISeq which includes unknown instruction, and ISeq which
   does not have it can be compiled as usual. */
static unsigned int
compile_insn(FILE *f, const struct rb_iseq_constant_body *body, const int insn, const VALUE *operands,
             const unsigned int pos, struct compile_status *status, struct compile_branch *b)
{
    unsigned int next_pos = pos + insn_len(insn);

/*****************/
 #include "mjit_compile.inc"
/*****************/

    return next_pos;
}

/* Compile one conditional branch.  If it has branchXXX insn, this should be
   called multiple times for each branch.  */
static void
compile_insns(FILE *f, const struct rb_iseq_constant_body *body, unsigned int stack_size,
              unsigned int pos, struct compile_status *status)
{
    int insn;
    struct compile_branch branch;

    branch.stack_size = stack_size;
    branch.finish_p = FALSE;

    while (pos < body->iseq_size && !status->compiled_for_pos[pos] && !branch.finish_p) {
#if OPT_DIRECT_THREADED_CODE || OPT_CALL_THREADED_CODE
        insn = rb_vm_insn_addr2insn((void *)body->iseq_encoded[pos]);
#else
        insn = (int)body->iseq_encoded[pos];
#endif
        status->compiled_for_pos[pos] = TRUE;

        fprintf(f, "\nlabel_%d: /* %s */\n", pos, insn_name(insn));
        pos = compile_insn(f, body, insn, body->iseq_encoded + (pos+1), pos, status, &branch);
        if (status->success && branch.stack_size > body->stack_max) {
            if (mjit_opts.warnings || mjit_opts.verbose)
                fprintf(stderr, "MJIT warning: JIT stack exceeded its max\n");
            status->success = FALSE;
        }
        if (!status->success)
            break;
    }
}

/* Print the block to cancel JIT execution. */
static void
compile_cancel_handler(FILE *f, const struct rb_iseq_constant_body *body, struct compile_status *status)
{
    unsigned int i;
    fprintf(f, "\ncancel:\n");
    if (status->local_stack_p) {
        for (i = 0; i < body->stack_max; i++) {
            fprintf(f, "    *((VALUE *)reg_cfp->bp + %d) = stack[%d];\n", i + 1, i);
        }
    }
    fprintf(f, "    return Qundef;\n");
}

/* Compile ISeq to C code in F.  It returns 1 if it succeeds to compile. */
int
mjit_compile(FILE *f, const struct rb_iseq_constant_body *body, const char *funcname)
{
    struct compile_status status;
    status.success = TRUE;
    status.compiled_for_pos = ZALLOC_N(int, body->iseq_size);
    status.local_stack_p = !body->catch_except_p;

    if (!mjit_opts.debug) {
        fprintf(f, "#undef OPT_CHECKED_RUN\n");
        fprintf(f, "#define OPT_CHECKED_RUN 0\n\n");
    }

#ifdef _WIN32
    fprintf(f, "__declspec(dllexport)\n");
#endif
    fprintf(f, "VALUE\n%s(rb_execution_context_t *ec, rb_control_frame_t *reg_cfp)\n{\n", funcname);
    if (status.local_stack_p) {
        fprintf(f, "    VALUE stack[%d];\n", body->stack_max);
    }
    else {
        fprintf(f, "    VALUE *stack = reg_cfp->sp;\n");
    }
    fprintf(f, "    static const VALUE *const original_body_iseq = (VALUE *)0x%"PRIxVALUE";\n",
            (VALUE)body->iseq_encoded);

    /* Simulate `opt_pc` in setup_parameters_complex */
    if (body->param.flags.has_opt) {
        int i;
        fprintf(f, "\n");
        fprintf(f, "    switch (reg_cfp->pc - reg_cfp->iseq->body->iseq_encoded) {\n");
        for (i = 0; i <= body->param.opt_num; i++) {
            VALUE pc_offset = body->param.opt_table[i];
            fprintf(f, "      case %"PRIdVALUE":\n", pc_offset);
            fprintf(f, "        goto label_%"PRIdVALUE";\n", pc_offset);
        }
        fprintf(f, "    }\n");
    }

    /* ISeq might be used for catch table too. For that usage, this code cancels JIT execution. */
    fprintf(f, "    if (reg_cfp->pc != original_body_iseq) {\n");
    fprintf(f, "        return Qundef;\n");
    fprintf(f, "    }\n");

    compile_insns(f, body, 0, 0, &status);
    compile_cancel_handler(f, body, &status);
    fprintf(f, "\n} /* end of %s */\n", funcname);

    xfree(status.compiled_for_pos);
    return status.success;
}
