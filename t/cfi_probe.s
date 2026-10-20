/* cfi_probe.s - CFI opcodes that no installed binary emits.
 *
 * A sweep of readelf --debug-dump=frames over everything the suite compares
 * against (both workloads, prof itself, libc, libstdc++, gdb: 500k opcodes)
 * finds only eleven distinct DW_CFA opcodes. Six more that this parser
 * implements are simply not reachable through any of those files, so three
 * mutations of the code that handles them survived with 73246 rows still
 * agreeing. A test that cannot see a defect is not evidence about it.
 *
 * So this file writes those opcodes by hand. The authorities are unchanged
 * and still two: gas encodes the .cfi_* directives into opcode bytes, and
 * readelf says what the resulting table means. prof has to agree with
 * readelf about it, exactly as for a compiler-generated file. The bodies are
 * nops: nothing here is ever called, only unwound on paper.
 *
 * Built by the Makefile into t/cfi_probe and fed to t/cfi_cmp.py.
 */

	.text

/* DW_CFA_restore back onto a rule the CIE itself set.
 *
 * The 2070 DW_CFA_restore opcodes in the corpus all restore a register the
 * CIE never mentioned, so "restore to undefined" and "restore to a
 * zero-initialised row" are the same answer, and a mutant that dropped the
 * CIE's initial row entirely could not be told from correct code. Moving the
 * return-address column away from the CIE's rule and then restoring it is
 * what makes the difference observable. */
	.globl probe_restore
	.type probe_restore, @function
probe_restore:
	.cfi_startproc
	nop
	.cfi_offset 16, -24
	nop
	.cfi_restore 16
	nop
	ret
	.cfi_endproc
	.size probe_restore, .-probe_restore

/* The register-rule forms: same_value, val_offset, register, undefined. */
	.globl probe_rules
	.type probe_rules, @function
probe_rules:
	.cfi_startproc
	nop
	.cfi_same_value 6
	nop
	.cfi_val_offset 3, -32
	nop
	.cfi_register 6, 3
	nop
	.cfi_undefined 6
	nop
	ret
	.cfi_endproc
	.size probe_rules, .-probe_rules

/* Two opcodes with no .cfi_* directive at all, written as raw bytes.
 *
 * DW_CFA_def_cfa_sf (0x12) takes a register and a FACTORED offset, i.e. one
 * that has to be multiplied by the CIE's data alignment factor (-8 here).
 * Getting that multiply wrong is invisible on x86-64 because gcc always has a
 * positive CFA offset and so always uses the unfactored DW_CFA_def_cfa_offset.
 * 0x07 is rsp, 0x7d is sleb128 -3, so the CFA becomes rsp+24.
 *
 * DW_CFA_GNU_negative_offset_extended (0x2f) is the pre-DWARF-3 way to spell
 * a positive CFA offset: reg 3, offset 2, meaning -(2 * -8) = c+16. */
	.globl probe_escapes
	.type probe_escapes, @function
probe_escapes:
	.cfi_startproc
	nop
	.cfi_escape 0x12, 0x07, 0x7d
	nop
	.cfi_escape 0x2f, 0x03, 0x02
	nop
	ret
	.cfi_endproc
	.size probe_escapes, .-probe_escapes

/* The opcodes with no .cfi_* directive and no occurrence anywhere in the
 * corpus, as raw bytes. Each is followed by a nop so it lands in its own row
 * and readelf prints the result where it can be compared.
 *
 *   0x04 advance_loc4            4-byte delta (gas uses the 1- and 2-byte forms)
 *   0x05 offset_extended         reg 6 (rbp) at CFA-24
 *   0x06 restore_extended        rbp back to the CIE's rule, i.e. undefined
 *   0x13 def_cfa_offset_sf       factored: -4 * -8 = CFA offset 32
 *   0x15 val_offset_sf           factored: rbp IS CFA+32
 *   0x16 val_expression          rbp is whatever DW_OP_reg0 computes ("vexp")
 *   0x2e GNU_args_size           carries no rule; the point is that skipping
 *                                it wrongly derails every row after it
 *
 * The order is not arbitrary: the two opcodes that carry a length (0x16's
 * expression body, 0x2e's operand) come BEFORE opcodes that set a rule, so a
 * version that mis-skips either one derails the rows that follow and the
 * comparison sees it. With val_expression last, a mutant that never skipped
 * its body produced byte-identical rows and survived.
 */
	.globl probe_escapes2
	.type probe_escapes2, @function
probe_escapes2:
	.cfi_startproc
	nop
	.cfi_escape 0x05, 0x06, 0x03
	nop
	.cfi_escape 0x04, 0x01, 0x00, 0x00, 0x00
	.cfi_escape 0x06, 0x06
	nop
	.cfi_escape 0x16, 0x06, 0x01, 0x50
	nop
	.cfi_escape 0x13, 0x7c
	nop
	.cfi_escape 0x2e, 0x10
	.cfi_escape 0x15, 0x06, 0x7c
	nop
	ret
	.cfi_endproc
	.size probe_escapes2, .-probe_escapes2

/* The remember/restore state stack, nested, which the corpus does cover
 * (16917 pairs) but never more than one deep. */
	.globl probe_remember
	.type probe_remember, @function
probe_remember:
	.cfi_startproc
	nop
	.cfi_def_cfa_offset 16
	.cfi_remember_state
	nop
	.cfi_def_cfa_offset 32
	.cfi_remember_state
	nop
	.cfi_def_cfa_offset 64
	nop
	.cfi_restore_state
	nop
	.cfi_restore_state
	nop
	ret
	.cfi_endproc
	.size probe_remember, .-probe_remember

	.globl main
	.type main, @function
main:
	.cfi_startproc
	xorl %eax, %eax
	ret
	.cfi_endproc
	.size main, .-main

	.section .note.GNU-stack,"",@progbits
