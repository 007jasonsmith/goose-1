/* Copyright (c) 2014 Eric Clark
 * See the file LICENSE for copying permission.
 */

#include <system.h>
#include <x86_64.h>
#include <multiboot2.h>
#include <acpi.h>
#include <mem.h>
#include <interrupt.h>
#include <ioport.h>

#include "system/mmu.h"

#include "device/vc.h"
#include "device/uart.h"

#include "fs/tar.h"

static int kbd(regs_t *regs);

static handler_t intvec[256];

/* From the linker script */
extern uint8_t *_end;

void
main(uint32_t magic, uint32_t addr)
{
	vc_clear();
	kconsole = &uartdev;
	/* kconsole = &vcdev; */

	if (multiboot_init(magic, addr)) {
		klogf(LOG_EMERG, "multiboot2_init failed.\n");
		return;
	}

	/* Allocator setup */
	{
		kmalloc_init();

		/* Give a page to kmalloc, so the frame allocator can use it */
		uintptr_t heapaddr = (uintptr_t)ALIGN(initrd_phys + initrd_len, FRAME_LEN);
		kbfree(mmu_mapregion(heapaddr, HEAP), FRAME_LEN);

		/* Free a small amount of space for the frame allocator */
		heapaddr += FRAME_LEN;
		frame_free(heapaddr, (0x400000 - heapaddr) / FRAME_LEN);

		/* Indicate to the frame allocator that 16MB to 64MB is usable */
		frame_free(0x01000000, 48*1024*1024 >> 12);
	}

	/* Map QEMU's acpi tables */
	mmu_map(0x07ffe000, (uintptr_t)VIRTUAL(0x07ffe000));
	mmu_map(0x07fff000, (uintptr_t)VIRTUAL(0x07fff000));

	/* Parse the ACPI tables for information needed by APIC and HPET */
	if (acpi_init()) {
		klogf(LOG_EMERG, "acpi_init failed.\n");
		return;
	}

	/* Initialize the interrupt controller */
	if (apic_init()) {
		klogf(LOG_EMERG, "apic_init failed. No apic?\n");
		return;
	}

	/* Initialize the UART after the APIC so it can unmask the interrupt */
	uart_init();
	kconsole = &uartdev;

	klogf(LOG_INFO, "initrd: %#lx %d\n", initrd_phys, initrd_len);
	tar_demo((uintptr_t)VIRTUAL(initrd_phys));

	klogf(LOG_INFO, "Kernel End: %#lx\n", &_end);
	klogf(LOG_DEBUG, "IA32_PAT: %#lx\n", rdmsr(IA32_PAT));

	/* Enable an interrpt for testing */
	bind_vector(IRQBASEVEC + 1, kbd);
	enable_isa_irq(1);

	sti();
	while (1) {
		char ch;
		kconsole->read(kconsole, &ch, 1);
		kprintf("%d ", ch);
	}
}

static int
kbd(regs_t *regs)
{
	kprintf("Kbd\n");

	send_eoi(1);

	return 1;
}

void
interrupt(regs_t* regs)
{
	uint64_t cr2;

	assert(regs->vector < 256);

	if (intvec[regs->vector] != NULL && intvec[regs->vector](regs))
		return;

	asm volatile("movq %%cr2, %q0" : "=a"(cr2) : :);
	klogf(
		LOG_EMERG,
		"\n"
		"=== PANIC ==================================================\n"
		"Interrupt Vector: %d\n"
		"Error code: %#lx\n"
		"Flags: %#lx\n"
		"RIP: %#lx\n"
		"RAX: %#lx RBX: %#lx\n"
		"RCX: %#lx RDX: %#lx\n"
		"RDI: %#lx RSI: %#lx\n"
		"RBP: %#lx RSP: %#lx\n"
		" R8: %#lx  R9: %#lx\n"
		"R10: %#lx R11: %#lx\n"
		"R12: %#lx R13: %#lx\n"
		"R14: %#lx R15: %#lx\n"
		"CR2: %#lx\n",

		regs->vector,
		regs->error_code,
		regs->rflags,
		regs->rip,
		regs->rax,	regs->rbx,	regs->rcx,	regs->rdx,
		regs->rdi,	regs->rsi,	regs->rbp,	regs->rsp,
		regs->r8,	regs->r9,	regs->r10,	regs->r11,
		regs->r12,	regs->r13,	regs->r14,	regs->r15,
		cr2
	);

	klogf(LOG_EMERG, "\nHalted\n");

	asm volatile("1: hlt; jmp 1b;");
}

void
bind_vector(uint8_t irq, handler_t f)
{
	assert(intvec[irq] == NULL);

	intvec[irq] = f;
}

void
clear_vector(uint8_t irq)
{
	intvec[irq] = NULL;
}
