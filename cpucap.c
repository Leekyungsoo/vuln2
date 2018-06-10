#include <linux/linkage.h>

#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/fpsimdmacros.h>
#include <asm/kvm.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_mmu.h>

#define CPU_GP_REG_OFFSET(x)	(CPU_GP_REGS + x)
#define CPU_XREG_OFFSET(x)	CPU_GP_REG_OFFSET(CPU_USER_PT_REGS + 8*x)

.text
.pushsection.hyp.text, "ax"

.macro save_callee_saved_regs ctxt
stp	x19, x20, [\ctxt, #CPU_XREG_OFFSET(19)]
stp	x21, x22, [\ctxt, #CPU_XREG_OFFSET(21)]
stp	x23, x24, [\ctxt, #CPU_XREG_OFFSET(23)]
stp	x25, x26, [\ctxt, #CPU_XREG_OFFSET(25)]
stp	x27, x28, [\ctxt, #CPU_XREG_OFFSET(27)]
stp	x29, lr, [\ctxt, #CPU_XREG_OFFSET(29)]
.endm

.macro restore_callee_saved_regs ctxt
ldp	x19, x20, [\ctxt, #CPU_XREG_OFFSET(19)]
ldp	x21, x22, [\ctxt, #CPU_XREG_OFFSET(21)]
ldp	x23, x24, [\ctxt, #CPU_XREG_OFFSET(23)]
ldp	x25, x26, [\ctxt, #CPU_XREG_OFFSET(25)]
ldp	x27, x28, [\ctxt, #CPU_XREG_OFFSET(27)]
ldp	x29, lr, [\ctxt, #CPU_XREG_OFFSET(29)]
.endm

/*
* u64 __guest_enter(struct kvm_vcpu *vcpu,
*		     struct kvm_cpu_context *host_ctxt);
*/
ENTRY(__guest_enter)
// x0: vcpu
// x1: host context
// x2-x17: clobbered by macros
// x18: guest context

// Store the host regs
save_callee_saved_regs x1

// Store the host_ctxt for use at exit time
str	x1, [sp, # - 16]!

add	x18, x0, #VCPU_CONTEXT

// Restore guest regs x0-x17
ldp	x0, x1, [x18, #CPU_XREG_OFFSET(0)]
ldp	x2, x3, [x18, #CPU_XREG_OFFSET(2)]
ldp	x4, x5, [x18, #CPU_XREG_OFFSET(4)]
ldp	x6, x7, [x18, #CPU_XREG_OFFSET(6)]
ldp	x8, x9, [x18, #CPU_XREG_OFFSET(8)]
ldp	x10, x11, [x18, #CPU_XREG_OFFSET(10)]
ldp	x12, x13, [x18, #CPU_XREG_OFFSET(12)]
ldp	x14, x15, [x18, #CPU_XREG_OFFSET(14)]
ldp	x16, x17, [x18, #CPU_XREG_OFFSET(16)]

// Restore guest regs x19-x29, lr
restore_callee_saved_regs x18

// Restore guest reg x18
ldr	x18, [x18, #CPU_XREG_OFFSET(18)]

// Do not touch any register after this!
eret
ENDPROC(__guest_enter)

ENTRY(__guest_exit)
// x0: return code
// x1: vcpu
// x2-x29,lr: vcpu regs
// vcpu x0-x1 on the stack

add	x1, x1, #VCPU_CONTEXT

ALTERNATIVE(nop, SET_PSTATE_PAN(1), ARM64_HAS_PAN, CONFIG_ARM64_PAN)

// Store the guest regs x2 and x3
stp	x2, x3, [x1, #CPU_XREG_OFFSET(2)]

// Retrieve the guest regs x0-x1 from the stack
ldp	x2, x3, [sp], #16	// x0, x1

						// Store the guest regs x0-x1 and x4-x18
	stp	x2, x3, [x1, #CPU_XREG_OFFSET(0)]
	stp	x4, x5, [x1, #CPU_XREG_OFFSET(4)]
	stp	x6, x7, [x1, #CPU_XREG_OFFSET(6)]
	stp	x8, x9, [x1, #CPU_XREG_OFFSET(8)]
	stp	x10, x11, [x1, #CPU_XREG_OFFSET(10)]
	stp	x12, x13, [x1, #CPU_XREG_OFFSET(12)]
	stp	x14, x15, [x1, #CPU_XREG_OFFSET(14)]
	stp	x16, x17, [x1, #CPU_XREG_OFFSET(16)]
	str	x18, [x1, #CPU_XREG_OFFSET(18)]

	// Store the guest regs x19-x29, lr
	save_callee_saved_regs x1

	// Restore the host_ctxt from the stack
	ldr	x2, [sp], #16

	// Now restore the host regs
	restore_callee_saved_regs x2

	// If we have a pending asynchronous abort, now is the
	// time to find out. From your VAXorcist book, page 666:
	// "Threaten me not, oh Evil one!  For I speak with
	// the power of DEC, and I command thee to show thyself!"
	mrs	x2, elr_el2
	mrs	x3, esr_el2
	mrs	x4, spsr_el2
	mov	x5, x0

	dsb	sy		// Synchronize against in-flight ld/st
	msr	daifclr, #4	// Unmask aborts

					// This is our single instruction exception window. A pending
					// SError is guaranteed to occur at the earliest when we unmask
					// it, and at the latest just after the ISB.
	.global	abort_guest_exit_start
	abort_guest_exit_start :

isb

.global	abort_guest_exit_end
abort_guest_exit_end :

// If the exception took place, restore the EL1 exception
// context so that we can report some information.
// Merge the exception code with the SError pending bit.
tbz	x0, #ARM_EXIT_WITH_SERROR_BIT, 1f
msr	elr_el2, x2
msr	esr_el2, x3
msr	spsr_el2, x4
orr	x0, x0, x5
1:	ret
ENDPROC(__guest_exit)

ENTRY(__fpsimd_guest_restore)
stp	x2, x3, [sp, # - 16]!
stp	x4, lr, [sp, # - 16]!

alternative_if_not ARM64_HAS_VIRT_HOST_EXTN
mrs	x2, cptr_el2
bic	x2, x2, #CPTR_EL2_TFP
msr	cptr_el2, x2
alternative_else
mrs	x2, cpacr_el1
orr	x2, x2, #CPACR_EL1_FPEN
msr	cpacr_el1, x2
alternative_endif
isb

mrs	x3, tpidr_el2

ldr	x0, [x3, #VCPU_HOST_CONTEXT]
kern_hyp_va x0
add	x0, x0, #CPU_GP_REG_OFFSET(CPU_FP_REGS)
bl	__fpsimd_save_state

add	x2, x3, #VCPU_CONTEXT
add	x0, x2, #CPU_GP_REG_OFFSET(CPU_FP_REGS)
bl	__fpsimd_restore_state

// Skip restoring fpexc32 for AArch64 guests
mrs	x1, hcr_el2
tbnz	x1, #HCR_RW_SHIFT, 1f
ldr	x4, [x3, #VCPU_FPEXC32_EL2]
msr	fpexc32_el2, x4
1:
ldp	x4, lr, [sp], #16
ldp	x2, x3, [sp], #16
ldp	x0, x1, [sp], #16

eret
ENDPROC(__fpsimd_guest_restore)

ENTRY(__qcom_hyp_sanitize_btac_predictors)
/**
* Call SMC64 with Silicon provider serviceID 23<<8 (0xc2001700)
* 0xC2000000-0xC200FFFF: assigned to SiP Service Calls
* b15-b0: contains SiP functionID
*/
movz    x0, #0x1700
movk    x0, #0xc200, lsl #16
smc     #0
ret
ENDPROC(__qcom_hyp_sanitize_btac_predictors)