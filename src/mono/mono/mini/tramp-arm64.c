/*
 * tramp-arm64.c: JIT trampoline code for ARM64
 *
 * Copyright 2013 Xamarin Inc
 *
 * Based on tramp-arm.c:
 * 
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *
 * (C) 2001-2003 Ximian, Inc.
 * Copyright 2003-2011 Novell Inc
 * Copyright 2011 Xamarin Inc
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "mini.h"
#include "debugger-agent.h"

#include <mono/arch/arm64/arm64-codegen.h>
#include <mono/metadata/abi-details.h>

#define ALIGN_TO(val,align) ((((guint64)val) + ((align) - 1)) & ~((align) - 1))

void
mono_arch_patch_callsite (guint8 *method_start, guint8 *code_ptr, guint8 *addr)
{
	mono_arm_patch (code_ptr - 4, addr, MONO_R_ARM64_BL);
	mono_arch_flush_icache (code_ptr - 4, 4);
}

void
mono_arch_patch_plt_entry (guint8 *code, gpointer *got, mgreg_t *regs, guint8 *addr)
{
	guint32 ins;
	guint64 slot_addr;
	int disp;

	/* 
	 * Decode the address loaded by the PLT entry emitted by arch_emit_plt_entry () in
	 * aot-compiler.c
	 */

	/* adrp */
	ins = ((guint32*)code) [0];
	g_assert (((ins >> 24) & 0x1f) == 0x10);
	disp = (((ins >> 5) & 0x7ffff) << 2) | ((ins >> 29) & 0x3);
	/* FIXME: disp is signed */
	g_assert ((disp >> 20) == 0);

	slot_addr = ((guint64)code + (disp << 12)) & ~0xfff;

	/* add x16, x16, :lo12:got */
	ins = ((guint32*)code) [1];
	g_assert (((ins >> 22) & 0x3) == 0);
	slot_addr += (ins >> 10) & 0xfff;

	/* ldr x16, [x16, <offset>] */
	ins = ((guint32*)code) [2];
	g_assert (((ins >> 24) & 0x3f) == 0x39);
	slot_addr += ((ins >> 10) & 0xfff) * 8;

	g_assert (*(guint64*)slot_addr);
	*(gpointer*)slot_addr = addr;
}

guint8*
mono_arch_get_call_target (guint8 *code)
{
	guint32 imm;
	int disp;

	code -= 4;

	imm = *(guint32*)code;
	/* Should be a bl */
	g_assert (((imm >> 31) & 0x1) == 0x1);
	g_assert (((imm >> 26) & 0x7) == 0x5);

	disp = (imm & 0x3ffffff);
	if ((disp >> 25) != 0)
		/* Negative, sing extend to 32 bits */
		disp = disp | 0xfc000000;

	return code + (disp * 4);
}

guint32
mono_arch_get_plt_info_offset (guint8 *plt_entry, mgreg_t *regs, guint8 *code)
{
	/* The offset is stored as the 5th word of the plt entry */
	return ((guint32*)plt_entry) [4];
}

#ifndef DISABLE_JIT

guchar*
mono_arch_create_generic_trampoline (MonoTrampolineType tramp_type, MonoTrampInfo **info, gboolean aot)
{
	guint8 *code, *buf, *tramp, *labels [16];
	int i, buf_len, imm;
	int frame_size, offset, gregs_offset, num_fregs, fregs_offset, arg_offset, lmf_offset, res_offset;
	guint64 gregs_regset;
	GSList *unwind_ops = NULL;
	MonoJumpInfo *ji = NULL;
	char *tramp_name;

	buf_len = 768;
	buf = code = mono_global_codeman_reserve (buf_len);

	/*
	 * We are getting called by a specific trampoline, ip1 contains the trampoline argument.
	 */

	/* Compute stack frame size and offsets */
	offset = 0;
	/* frame block */
	offset += 2 * 8;
	/* gregs */
	gregs_offset = offset;
	offset += 32 * 8;
	/* fregs */
	// FIXME: Save 128 bits
	/* Only have to save the argument regs */
	num_fregs = 8;
	fregs_offset = offset;
	offset += num_fregs * 8;
	/* arg */
	arg_offset = offset;
	offset += 8;
	/* result */
	res_offset = offset;
	offset += 8;
	/* LMF */
	lmf_offset = offset;
	offset += sizeof (MonoLMF);
	//offset += 22 * 8;
	frame_size = ALIGN_TO (offset, MONO_ARCH_FRAME_ALIGNMENT);

	/* Setup stack frame */
	imm = frame_size;
	while (imm > 256) {
		arm_subx_imm (code, ARMREG_SP, ARMREG_SP, 256);
		imm -= 256;
	}
	arm_subx_imm (code, ARMREG_SP, ARMREG_SP, imm);
	arm_stpx (code, ARMREG_FP, ARMREG_LR, ARMREG_SP, 0);
	arm_movspx (code, ARMREG_FP, ARMREG_SP);

	/* Save gregs */
	// FIXME: Optimize this
	gregs_regset = ~((1 << ARMREG_FP) | (1 << ARMREG_SP));
	code = mono_arm_emit_store_regarray (code, gregs_regset, ARMREG_FP, gregs_offset);
	/* Save fregs */
	for (i = 0; i < num_fregs; ++i)
		arm_strfpx (code, i, ARMREG_FP, fregs_offset + (i * 8));
	/* Save trampoline arg */
	arm_strx (code, ARMREG_IP1, ARMREG_FP, arg_offset);

	/* Setup LMF */
	arm_addx_imm (code, ARMREG_IP0, ARMREG_FP, lmf_offset);
	code = mono_arm_emit_store_regset (code, MONO_ARCH_LMF_REGS, ARMREG_IP0, MONO_STRUCT_OFFSET (MonoLMF, gregs));
	/* Save caller fp */
	arm_ldrx (code, ARMREG_IP1, ARMREG_FP, 0);
	arm_strx (code, ARMREG_IP1, ARMREG_IP0, MONO_STRUCT_OFFSET (MonoLMF, gregs) + (MONO_ARCH_LMF_REG_FP * 8));
	/* Save caller sp */
	arm_movx (code, ARMREG_IP1, ARMREG_FP);
	imm = frame_size;
	while (imm > 256) {
		arm_addx_imm (code, ARMREG_IP1, ARMREG_IP1, 256);
		imm -= 256;
	}
	arm_addx_imm (code, ARMREG_IP1, ARMREG_IP1, imm);
	arm_strx (code, ARMREG_IP1, ARMREG_IP0, MONO_STRUCT_OFFSET (MonoLMF, gregs) + (MONO_ARCH_LMF_REG_SP * 8));
	/* Save caller pc */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		arm_movx (code, ARMREG_LR, ARMREG_RZR);
	else
		arm_ldrx (code, ARMREG_LR, ARMREG_FP, 8);
	arm_strx (code, ARMREG_LR, ARMREG_IP0, MONO_STRUCT_OFFSET (MonoLMF, pc));

	/* Save LMF */
	/* Similar to emit_save_lmf () */
	if (aot) {
		code = mono_arm_emit_aotconst (&ji, code, buf, ARMREG_IP0, MONO_PATCH_INFO_JIT_ICALL_ADDR, "mono_get_lmf_addr");
	} else {
		tramp = (guint8*)mono_get_lmf_addr;
		code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)tramp);
	}
	arm_blrx (code, ARMREG_IP0);
	/* r0 contains the address of the tls slot holding the current lmf */
	/* ip0 = lmf */
	arm_addx_imm (code, ARMREG_IP0, ARMREG_FP, lmf_offset);
	/* lmf->lmf_addr = lmf_addr */
	arm_strx (code, ARMREG_R0, ARMREG_IP0, MONO_STRUCT_OFFSET (MonoLMF, lmf_addr));
	/* lmf->previous_lmf = *lmf_addr */
	arm_ldrx (code, ARMREG_IP1, ARMREG_R0, 0);
	arm_strx (code, ARMREG_IP1, ARMREG_IP0, MONO_STRUCT_OFFSET (MonoLMF, previous_lmf));
	/* *lmf_addr = lmf */
	arm_strx (code, ARMREG_IP0, ARMREG_R0, 0);

	/* Call the C trampoline function */
	/* Arg 1 = gregs */
	arm_addx_imm (code, ARMREG_R0, ARMREG_FP, gregs_offset);
	/* Arg 2 = caller */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		arm_movx (code, ARMREG_R1, ARMREG_RZR);
	else
		arm_ldrx (code, ARMREG_R1, ARMREG_FP, gregs_offset + (ARMREG_LR * 8));
	/* Arg 3 = arg */
	if (MONO_TRAMPOLINE_TYPE_HAS_ARG (tramp_type))
		/* Passed in r0 */
		arm_ldrx (code, ARMREG_R2, ARMREG_FP, gregs_offset + (ARMREG_R0 * 8));
	else
		arm_ldrx (code, ARMREG_R2, ARMREG_FP, arg_offset);
	/* Arg 4 = trampoline addr */
	arm_movx (code, ARMREG_R3, ARMREG_RZR);

	if (aot) {
		char *icall_name = g_strdup_printf ("trampoline_func_%d", tramp_type);
		code = mono_arm_emit_aotconst (&ji, code, buf, ARMREG_IP0, MONO_PATCH_INFO_JIT_ICALL_ADDR, icall_name);
	} else {
		tramp = (guint8*)mono_get_trampoline_func (tramp_type);
		code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)tramp);
	}
	arm_blrx (code, ARMREG_IP0);

	/* Save the result */
	arm_strx (code, ARMREG_R0, ARMREG_FP, res_offset);

	/* Restore LMF */
	/* Similar to emit_restore_lmf () */
	/* Clobbers ip0/ip1 */
	/* ip0 = lmf */
	arm_addx_imm (code, ARMREG_IP0, ARMREG_FP, lmf_offset);
	/* ip1 = lmf->previous_lmf */
	arm_ldrx (code, ARMREG_IP1, ARMREG_IP0, MONO_STRUCT_OFFSET (MonoLMF, previous_lmf));
	/* ip0 = lmf->lmf_addr */
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP0, MONO_STRUCT_OFFSET (MonoLMF, lmf_addr));
	/* *lmf_addr = previous_lmf */
	arm_strx (code, ARMREG_IP1, ARMREG_IP0, 0);

	/* Check for thread interruption */
	/* This is not perf critical code so no need to check the interrupt flag */
	if (aot) {
		code = mono_arm_emit_aotconst (&ji, code, buf, ARMREG_IP0, MONO_PATCH_INFO_JIT_ICALL_ADDR, "mono_thread_force_interruption_checkpoint_noraise");
	} else {
		code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)mono_thread_force_interruption_checkpoint_noraise);
	}
	arm_blrx (code, ARMREG_IP0);
	/* Check whenever there is an exception to be thrown */
	labels [0] = code;
	arm_cbnzx (code, ARMREG_R0, 0);

	/* Normal case */

	/* Restore gregs */
	/* Only have to load the argument regs (r0..r8) and the rgctx reg */
	code = mono_arm_emit_load_regarray (code, 0x1ff | (1 << ARMREG_LR) | (1 << MONO_ARCH_RGCTX_REG), ARMREG_FP, gregs_offset);
	/* Restore fregs */
	for (i = 0; i < num_fregs; ++i)
		arm_ldrfpx (code, i, ARMREG_FP, fregs_offset + (i * 8));

	/* Load the result */
	arm_ldrx (code, ARMREG_IP1, ARMREG_FP, res_offset);

	/* These trampolines return a value */
	if (tramp_type == MONO_TRAMPOLINE_RGCTX_LAZY_FETCH)
		arm_movx (code, ARMREG_R0, ARMREG_IP1);

	/* Cleanup frame */
	code = mono_arm_emit_destroy_frame (code, frame_size, ((1 << ARMREG_IP0)));

	if (tramp_type == MONO_TRAMPOLINE_RGCTX_LAZY_FETCH)
		arm_retx (code, ARMREG_LR);
	else
		arm_brx (code, ARMREG_IP1);

	/* Exception case */
	mono_arm_patch (labels [0], code, MONO_R_ARM64_CBZ);

	/*
	 * We have an exception we want to throw in the caller's frame, so pop
	 * the trampoline frame and throw from the caller.
	 */
	code = mono_arm_emit_destroy_frame (code, frame_size, ((1 << ARMREG_IP0)));
	/* We are in the parent frame, the exception is in x0 */
	/*
	 * EH is initialized after trampolines, so get the address of the variable
	 * which contains throw_exception, and load it from there.
	 */
	if (aot) {
		/* Not really a jit icall */
		code = mono_arm_emit_aotconst (&ji, code, buf, ARMREG_IP0, MONO_PATCH_INFO_JIT_ICALL_ADDR, "throw_exception_addr");
	} else {
		code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)mono_get_throw_exception_addr ());
	}
	arm_ldrx (code, ARMREG_IP0, ARMREG_IP0, 0);
	/* lr contains the return address, the trampoline will use it as the throw site */
	arm_brx (code, ARMREG_IP0);

	g_assert ((code - buf) < buf_len);
	mono_arch_flush_icache (buf, code - buf);

	if (info) {
		tramp_name = mono_get_generic_trampoline_name (tramp_type);
		*info = mono_tramp_info_create (tramp_name, buf, code - buf, ji, unwind_ops);
		g_free (tramp_name);
	}

	return buf;
}

gpointer
mono_arch_create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain, guint32 *code_len)
{
	guint8 *code, *buf, *tramp;
	int buf_len = 64;

	/*
	 * Return a trampoline which calls generic trampoline TRAMP_TYPE passing in ARG1.
	 * Pass the argument in ip1, clobbering ip0.
	 */
	tramp = mono_get_trampoline_code (tramp_type);

	buf = code = mono_global_codeman_reserve (buf_len);

	code = mono_arm_emit_imm64 (code, ARMREG_IP1, (guint64)arg1);
	code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)tramp);

	arm_brx (code, ARMREG_IP0);

	g_assert ((code - buf) < buf_len);
	mono_arch_flush_icache (buf, code - buf);
	if (code_len)
		*code_len = code - buf;

	return buf;
}

gpointer
mono_arch_get_unbox_trampoline (MonoMethod *m, gpointer addr)
{
	guint8 *code, *start;
	guint32 size = 32;
	MonoDomain *domain = mono_domain_get ();

	start = code = mono_domain_code_reserve (domain, size);
	code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)addr);
	arm_addx_imm (code, ARMREG_R0, ARMREG_R0, sizeof (MonoObject));
	arm_brx (code, ARMREG_IP0);

	g_assert ((code - start) <= size);
	mono_arch_flush_icache (start, code - start);
	return start;
}

gpointer
mono_arch_get_static_rgctx_trampoline (MonoMethod *m, MonoMethodRuntimeGenericContext *mrgctx, gpointer addr)
{
	guint8 *code, *start;
	guint32 buf_len = 32;
	MonoDomain *domain = mono_domain_get ();

	start = code = mono_domain_code_reserve (domain, buf_len);
	code = mono_arm_emit_imm64 (code, MONO_ARCH_RGCTX_REG, (guint64)mrgctx);
	code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)addr);
	arm_brx (code, ARMREG_IP0);

	g_assert ((code - start) <= buf_len);

	mono_arch_flush_icache (start, code - start);

	return start;
}

gpointer
mono_arch_create_rgctx_lazy_fetch_trampoline (guint32 slot, MonoTrampInfo **info, gboolean aot)
{
	guint8 *code, *buf;
	int buf_size;
	int i, depth, index, njumps;
	gboolean is_mrgctx;
	guint8 **rgctx_null_jumps;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;
	guint8 *tramp;
	guint32 code_len;

	is_mrgctx = MONO_RGCTX_SLOT_IS_MRGCTX (slot);
	index = MONO_RGCTX_SLOT_INDEX (slot);
	if (is_mrgctx)
		index += MONO_SIZEOF_METHOD_RUNTIME_GENERIC_CONTEXT / sizeof (gpointer);
	for (depth = 0; ; ++depth) {
		int size = mono_class_rgctx_get_array_size (depth, is_mrgctx);

		if (index < size - 1)
			break;
		index -= size - 1;
	}

	buf_size = 64 + 16 * depth;
	code = buf = mono_global_codeman_reserve (buf_size);

	rgctx_null_jumps = g_malloc0 (sizeof (guint8*) * (depth + 2));
	njumps = 0;

	/* The vtable/mrgtx is in R0 */
	g_assert (MONO_ARCH_VTABLE_REG == ARMREG_R0);

	if (is_mrgctx) {
		/* get mrgctx ptr */
		arm_movx (code, ARMREG_IP1, ARMREG_R0);
 	} else {
		/* load rgctx ptr from vtable */
		code = mono_arm_emit_ldrx (code, ARMREG_IP1, ARMREG_R0, MONO_STRUCT_OFFSET (MonoVTable, runtime_generic_context));
		/* is the rgctx ptr null? */
		/* if yes, jump to actual trampoline */
		rgctx_null_jumps [njumps ++] = code;
		arm_cbzx (code, ARMREG_IP1, 0);
	}

	for (i = 0; i < depth; ++i) {
		/* load ptr to next array */
		if (is_mrgctx && i == 0) {
			code = mono_arm_emit_ldrx (code, ARMREG_IP1, ARMREG_IP1, MONO_SIZEOF_METHOD_RUNTIME_GENERIC_CONTEXT);
		} else {
			code = mono_arm_emit_ldrx (code, ARMREG_IP1, ARMREG_IP1, 0);
		}
		/* is the ptr null? */
		/* if yes, jump to actual trampoline */
		rgctx_null_jumps [njumps ++] = code;
		arm_cbzx (code, ARMREG_IP1, 0);
	}

	/* fetch slot */
	code = mono_arm_emit_ldrx (code, ARMREG_IP1, ARMREG_IP1, sizeof (gpointer) * (index + 1));
	/* is the slot null? */
	/* if yes, jump to actual trampoline */
	rgctx_null_jumps [njumps ++] = code;
	arm_cbzx (code, ARMREG_IP1, 0);
	/* otherwise return, result is in IP1 */
	arm_movx (code, ARMREG_R0, ARMREG_IP1);
	arm_brx (code, ARMREG_LR);

	g_assert (njumps <= depth + 2);
	for (i = 0; i < njumps; ++i)
		mono_arm_patch (rgctx_null_jumps [i], code, MONO_R_ARM64_CBZ);

	g_free (rgctx_null_jumps);

	/* Slowpath */

	/* Call mono_rgctx_lazy_fetch_trampoline (), passing in the slot as argument */
	/* The vtable/mrgctx is still in R0 */
	if (aot) {
		code = mono_arm_emit_aotconst (&ji, code, buf, ARMREG_IP0, MONO_PATCH_INFO_JIT_ICALL_ADDR, g_strdup_printf ("specific_trampoline_lazy_fetch_%u", slot));
	} else {
		tramp = mono_arch_create_specific_trampoline (GUINT_TO_POINTER (slot), MONO_TRAMPOLINE_RGCTX_LAZY_FETCH, mono_get_root_domain (), &code_len);
		code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)tramp);
	}
	arm_brx (code, ARMREG_IP0);

	mono_arch_flush_icache (buf, code - buf);

	g_assert (code - buf <= buf_size);

	if (info) {
		char *name = mono_get_rgctx_fetch_trampoline_name (slot);
		*info = mono_tramp_info_create (name, buf, code - buf, ji, unwind_ops);
		g_free (name);
	}

	return buf;
}

gpointer
mono_arch_create_general_rgctx_lazy_fetch_trampoline (MonoTrampInfo **info, gboolean aot)
{
	guint8 *code, *buf;
	int tramp_size;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;

	g_assert (aot);

	tramp_size = 32;

	code = buf = mono_global_codeman_reserve (tramp_size);

	mono_add_unwind_op_def_cfa (unwind_ops, code, buf, ARMREG_SP, 0);

	// FIXME: Currently, we always go to the slow path.
	/* Load trampoline addr */
	arm_ldrx (code, ARMREG_IP0, MONO_ARCH_RGCTX_REG, 8);
	/* The vtable/mrgctx is in R0 */
	g_assert (MONO_ARCH_VTABLE_REG == ARMREG_R0);
	arm_brx (code, ARMREG_IP0);

	mono_arch_flush_icache (buf, code - buf);

	g_assert (code - buf <= tramp_size);

	if (info)
		*info = mono_tramp_info_create ("rgctx_fetch_trampoline_general", buf, code - buf, ji, unwind_ops);

	return buf;
}

/*
 * mono_arch_create_sdb_trampoline:
 *
 *   Return a trampoline which captures the current context, passes it to
 * debugger_agent_single_step_from_context ()/debugger_agent_breakpoint_from_context (),
 * then restores the (potentially changed) context.
 */
guint8*
mono_arch_create_sdb_trampoline (gboolean single_step, MonoTrampInfo **info, gboolean aot)
{
	int tramp_size = 512;
	int offset, imm, frame_size, ctx_offset;
	guint64 gregs_regset;
	guint8 *code, *buf;
	GSList *unwind_ops = NULL;
	MonoJumpInfo *ji = NULL;

	code = buf = mono_global_codeman_reserve (tramp_size);

	/* Compute stack frame size and offsets */
	offset = 0;
	/* frame block */
	offset += 2 * 8;
	/* MonoContext */
	ctx_offset = offset;
	offset += sizeof (MonoContext);
	offset = ALIGN_TO (offset, MONO_ARCH_FRAME_ALIGNMENT);
	frame_size = offset;

	// FIXME: Unwind info

	/* Setup stack frame */
	imm = frame_size;
	while (imm > 256) {
		arm_subx_imm (code, ARMREG_SP, ARMREG_SP, 256);
		imm -= 256;
	}
	arm_subx_imm (code, ARMREG_SP, ARMREG_SP, imm);
	arm_stpx (code, ARMREG_FP, ARMREG_LR, ARMREG_SP, 0);
	arm_movspx (code, ARMREG_FP, ARMREG_SP);

	/* Initialize a MonoContext structure on the stack */
	/* No need to save fregs */
	gregs_regset = ~((1 << ARMREG_FP) | (1 << ARMREG_SP));
	code = mono_arm_emit_store_regarray (code, gregs_regset, ARMREG_FP, ctx_offset + G_STRUCT_OFFSET (MonoContext, regs));
	/* Save caller fp */
	arm_ldrx (code, ARMREG_IP1, ARMREG_FP, 0);
	arm_strx (code, ARMREG_IP1, ARMREG_FP, ctx_offset + G_STRUCT_OFFSET (MonoContext, regs) + (ARMREG_FP * 8));
	/* Save caller sp */
	arm_movx (code, ARMREG_IP1, ARMREG_FP);
	imm = frame_size;
	while (imm > 256) {
		arm_addx_imm (code, ARMREG_IP1, ARMREG_IP1, 256);
		imm -= 256;
	}
	arm_addx_imm (code, ARMREG_IP1, ARMREG_IP1, imm);
	arm_strx (code, ARMREG_IP1, ARMREG_FP, ctx_offset + G_STRUCT_OFFSET (MonoContext, regs) + (ARMREG_SP * 8));
	/* Save caller ip */
	arm_ldrx (code, ARMREG_IP1, ARMREG_FP, 8);
	arm_strx (code, ARMREG_IP1, ARMREG_FP, ctx_offset + G_STRUCT_OFFSET (MonoContext, pc));

	/* Call the single step/breakpoint function in sdb */
	/* Arg1 = ctx */
	arm_addx_imm (code, ARMREG_R0, ARMREG_FP, ctx_offset);
	if (aot) {
		if (single_step)
			code = mono_arm_emit_aotconst (&ji, code, buf, ARMREG_IP0, MONO_PATCH_INFO_JIT_ICALL_ADDR, "debugger_agent_single_step_from_context");
		else
			code = mono_arm_emit_aotconst (&ji, code, buf, ARMREG_IP0, MONO_PATCH_INFO_JIT_ICALL_ADDR, "debugger_agent_breakpoint_from_context");
	} else {
		gpointer addr = single_step ? debugger_agent_single_step_from_context : debugger_agent_breakpoint_from_context;

		code = mono_arm_emit_imm64 (code, ARMREG_IP0, (guint64)addr);
	}
	arm_blrx (code, ARMREG_IP0);

	/* Restore ctx */
	/* Save fp/pc into the frame block */
	arm_ldrx (code, ARMREG_IP0, ARMREG_FP, ctx_offset + G_STRUCT_OFFSET (MonoContext, regs) + (ARMREG_FP * 8));
	arm_strx (code, ARMREG_IP0, ARMREG_FP, 0);
	arm_ldrx (code, ARMREG_IP0, ARMREG_FP, ctx_offset + G_STRUCT_OFFSET (MonoContext, pc));
	arm_strx (code, ARMREG_IP0, ARMREG_FP, 8);
	gregs_regset = ~((1 << ARMREG_FP) | (1 << ARMREG_SP));
	code = mono_arm_emit_load_regarray (code, gregs_regset, ARMREG_FP, ctx_offset + G_STRUCT_OFFSET (MonoContext, regs));

	code = mono_arm_emit_destroy_frame (code, frame_size, ((1 << ARMREG_IP0) | (1 << ARMREG_IP1)));

	arm_retx (code, ARMREG_LR);

	mono_arch_flush_icache (code, code - buf);
	g_assert (code - buf <= tramp_size);

	const char *tramp_name = single_step ? "sdb_single_step_trampoline" : "sdb_breakpoint_trampoline";
	*info = mono_tramp_info_create (tramp_name, buf, code - buf, ji, unwind_ops);

	return buf;
}

#else /* DISABLE_JIT */

guchar*
mono_arch_create_generic_trampoline (MonoTrampolineType tramp_type, MonoTrampInfo **info, gboolean aot)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_arch_create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain, guint32 *code_len)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_arch_get_unbox_trampoline (MonoMethod *m, gpointer addr)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_arch_get_static_rgctx_trampoline (MonoMethod *m, MonoMethodRuntimeGenericContext *mrgctx, gpointer addr)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_arch_create_rgctx_lazy_fetch_trampoline (guint32 slot, MonoTrampInfo **info, gboolean aot)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_arch_get_nullified_class_init_trampoline (MonoTrampInfo **info)
{
	g_assert_not_reached ();
	return NULL;
}

guint8*
mono_arch_create_sdb_trampoline (gboolean single_step, MonoTrampInfo **info, gboolean aot)
{
	g_assert_not_reached ();
	return NULL;
}

#endif /* !DISABLE_JIT */
