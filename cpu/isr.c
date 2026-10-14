#include "isr.h"
#include "idt.h"
#include <drivers/screen.h>
#include <drivers/keyboard.h>
#include <libc/string.h>
#include "timer.h"
#include "ports.h"
#include "mmio.h"
#include <kernel/kernel.h>

isr_t interrupt_handlers[256];

extern void int128(const registers_t *r);
extern char* font;

char* crash_text;

#define sg(n) set_idt_gate(n, (uint32_t)isr##n);
#define sgu(n) set_idt_gate(n, (uint32_t)isr##n);

/* Can't do this with a loop because we need the address
 * of the function names */
void isr_install() {
    set_idt_gate(0, (uint32_t)isr0);
    set_idt_gate(1, (uint32_t)isr1);
    set_idt_gate(2, (uint32_t)isr2);
    set_idt_gate(3, (uint32_t)isr3);
    set_idt_gate(4, (uint32_t)isr4);
    set_idt_gate(5, (uint32_t)isr5);
    set_idt_gate(6, (uint32_t)isr6);
    set_idt_gate(7, (uint32_t)isr7);
    set_idt_gate(8, (uint32_t)isr8);
    set_idt_gate(9, (uint32_t)isr9);
    set_idt_gate(10, (uint32_t)isr10);
    set_idt_gate(11, (uint32_t)isr11);
    set_idt_gate(12, (uint32_t)isr12);
    set_idt_gate(13, (uint32_t)isr13);
    set_idt_gate(14, (uint32_t)isr14);
    set_idt_gate(15, (uint32_t)isr15);
    set_idt_gate(16, (uint32_t)isr16);
    set_idt_gate(17, (uint32_t)isr17);
    set_idt_gate(18, (uint32_t)isr18);
    set_idt_gate(19, (uint32_t)isr19);
    set_idt_gate(20, (uint32_t)isr20);
    set_idt_gate(21, (uint32_t)isr21);
    set_idt_gate(22, (uint32_t)isr22);
    set_idt_gate(23, (uint32_t)isr23);
    set_idt_gate(24, (uint32_t)isr24);
    set_idt_gate(25, (uint32_t)isr25);
    set_idt_gate(26, (uint32_t)isr26);
    set_idt_gate(27, (uint32_t)isr27);
    set_idt_gate(28, (uint32_t)isr28);
    set_idt_gate(29, (uint32_t)isr29);
    set_idt_gate(30, (uint32_t)isr30);
    set_idt_gate(31, (uint32_t)isr31);
    sg(48) sg(49)
    sg(50) sg(51) sg(52) sg(53) sg(54)
    sg(55) sg(56) sg(57) sg(58) sg(59)
    sg(60) sg(61) sg(62) sg(63) sg(64)
    sg(65) sg(66) sg(67) sg(68) sg(69)
    sg(70) sg(71) sg(72) sg(73) sg(74)
    sg(75) sg(76) sg(77) sg(78) sg(79)
    sg(80) sg(81) sg(82) sg(83) sg(84)
    sg(85) sg(86) sg(87) sg(88) sg(89)
    sg(90) sg(91) sg(92) sg(93) sg(94)
    sg(95) sg(96) sg(97) sg(98) sg(99)

    sg(100) sg(101) sg(102) sg(103) sg(104)
    sg(105) sg(106) sg(107) sg(108) sg(109)
    sg(110) sg(111) sg(112) sg(113) sg(114)
    sg(115) sg(116) sg(117) sg(118) sg(119)
    sg(120) sg(121) sg(122) sg(123) sg(124)
    sg(125) sg(126) sg(127) sgu(128) sg(129)
    sg(130) sg(131) sg(132) sg(133) sg(134)
    sg(135) sg(136) sg(137) sg(138) sg(139)
    sg(140) sg(141) sg(142) sg(143) sg(144)
    sg(145) sg(146) sg(147) sg(148) sg(149)

    sg(150) sg(151) sg(152) sg(153) sg(154)
    sg(155) sg(156) sg(157) sg(158) sg(159)
    sg(160) sg(161) sg(162) sg(163) sg(164)
    sg(165) sg(166) sg(167) sg(168) sg(169)
    sg(170) sg(171) sg(172) sg(173) sg(174)
    sg(175) sg(176) sg(177) sg(178) sg(179)
    sg(180) sg(181) sg(182) sg(183) sg(184)
    sg(185) sg(186) sg(187) sg(188) sg(189)
    sg(190) sg(191) sg(192) sg(193) sg(194)
    sg(195) sg(196) sg(197) sg(198) sg(199)

    sg(200) sg(201) sg(202) sg(203) sg(204)
    sg(205) sg(206) sg(207) sg(208) sg(209)
    sg(210) sg(211) sg(212) sg(213) sg(214)
    sg(215) sg(216) sg(217) sg(218) sg(219)
    sg(220) sg(221) sg(222) sg(223) sg(224)
    sg(225) sg(226) sg(227) sg(228) sg(229)
    sg(230) sg(231) sg(232) sg(233) sg(234)
    sg(235) sg(236) sg(237) sg(238) sg(239)
    sg(240) sg(241) sg(242) sg(243) sg(244)
    sg(245) sg(246) sg(247) sg(248) sg(249)

    sg(250) sg(251) sg(252) sg(253) sg(254)
    sg(255)
    


    //set_idt_gate(32, (uint32_t)debug);

    // Remap the PIC
    port_byte_out(0x20, 0x11);
    port_byte_out(0xA0, 0x11);
    port_byte_out(0x21, 0x20);
    port_byte_out(0xA1, 0x28);
    port_byte_out(0x21, 0x04);
    port_byte_out(0xA1, 0x02);
    port_byte_out(0x21, 0x01);
    port_byte_out(0xA1, 0x01);
    port_byte_out(0x21, 0x0);
    port_byte_out(0xA1, 0x0); 

    // Install the IRQs
    set_idt_gate(32, (uint32_t)irq0);
    set_idt_gate(33, (uint32_t)irq1);
    set_idt_gate(34, (uint32_t)irq2);
    set_idt_gate(35, (uint32_t)irq3);
    set_idt_gate(36, (uint32_t)irq4);
    set_idt_gate(37, (uint32_t)irq5);
    set_idt_gate(38, (uint32_t)irq6);
    set_idt_gate(39, (uint32_t)irq7);
    set_idt_gate(40, (uint32_t)irq8);
    set_idt_gate(41, (uint32_t)irq9);
    set_idt_gate(42, (uint32_t)irq10);
    set_idt_gate(43, (uint32_t)irq11);
    set_idt_gate(44, (uint32_t)irq12);
    set_idt_gate(45, (uint32_t)irq13);
    set_idt_gate(46, (uint32_t)irq14);
    set_idt_gate(47, (uint32_t)irq15);

    set_idt(); // Load with ASM
}

/* To print the message which defines every exception */
const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",

    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",

    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",

    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    [0x80] = "Kernel Interrupt",
    [IRQ0] = "Timer Interrupt",
    [IRQ1] = "Keyboard Interrupt",
    
};
uint8_t int_c;
uint8_t int_ns[8] = {0};

void isr_handler(const registers_t *r) {
	irq_handler(r);
}

void none(const registers_t *r){
//rett:
	kprintf(font,0xc,2,"|< No interrupt named int%x\n",r->int_no);
    return;
}

void debug(const registers_t *r){
	kprintf(font,0x9,2,"DEBUG!\n");
	//delay(1000);
}

void doNothing(const registers_t *r){}

void register_interrupt_handler(const uint8_t n, const isr_t handler) {
    if ( handler!=none )kprintd("int%d \"%s\"@%X\n",n,exception_messages[n],handler);
    interrupt_handlers[n] = handler;
    
}

void irq_handler(const registers_t *r) {
    for (int i = 0; i < 7; i++) { // Исправлено: было i < 8 (чтение int_ns[8] за границей массива)
        int_ns[i] = int_ns[i + 1];
    }
    int_ns[7] = r->int_no;

    // Для таймера (IRQ0) EOI нужно послать ДО mt_schedule(), иначе после переключения контекста PIC останется заблокирован
    if (r->int_no == IRQ0) {
        port_byte_out(0x20, 0x20);
    }

    if (interrupt_handlers[r->int_no & 0xff] != 0) {
        isr_t handler = interrupt_handlers[r->int_no & 0xff];
        handler(r);
    }

    // Для остальных аппаратных IRQ (32..47, включая клавиатуру IRQ1 и мышь IRQ12)
    // отправляем EOI ПОСЛЕ того, как обработчик вычитал данные из порта 0x60!
    if (r->int_no > IRQ0 && r->int_no < 48) {
        if (r->int_no >= 40) port_byte_out(0xA0, 0x20); /* slave */
        port_byte_out(0x20, 0x20); /* master */
    }
}

void dump_idt_entry(int n) {
	uint32_t base;
	uint16_t limit;
	asm volatile("sidt %0" : "=m"(*(uint64_t*)&limit));
	uint32_t idt_addr = *(uint32_t*)((uint8_t*)&limit + 2);

	idt_gate_t *idt = (idt_gate_t *)idt_addr;
	idt_gate_t e = idt[n];

	uint32_t offset = (e.high_offset << 16) | e.low_offset;

	kprintf(font, 0xf, 2, "IDT[%d]: handler=%x sel=%x flags=%x\n",
	       n, offset, e.sel, e.flags);
	kprintf(font, 0xf, 2, "int %d: handler=%x\n",
	       n, interrupt_handlers[n]);
}


extern void kprintf(const unsigned char font[], const unsigned short color, const char attr, const char *format, ...);
extern void con_setp(uint8_t,uint8_t);

extern uint32_t current_task_idx;

#include "ksym.h"

// Структура стандартного стек-фрейма GCC (cdecl)
struct stack_frame {
    struct stack_frame* ebp;
    uint32_t eip;
};

void dfh(const registers_t *r) {
    if (current_task_idx == 0) asm volatile("cli");
    cls(0x17);

    // 1. Заголовок
    setcursorpos(0, 1);
    for (int i = 0; i < 80; i++) kprintf(font, 0x0701, 3, " ");
    setcursorpos(0, 1);
    kprintf(font, 0x0701, 3, " A fatal exception have occured!");

    setcursorpos(0, 3);
    kprintf(font, 0x0107, 3, "STOP (PID: %d; EIP: 0x%08X)", current_task_idx, r->eip);
    setcursorpos(0, 4);
    kprintf(font, 0x0107, 3, "INTERRUPT CAUSE: %s", exception_messages[r->int_no]);

    setcursorpos(0, 5);
    kprintf(font, 0x0107, 3, "Additional info: %s", (current_task_idx == 0) ? "Kernel crashed" : "(none)");

    // 2. Блок Call Trace
    setcursorpos(0, 7);
    for (int i = 0; i < 80; i++) kprintf(font, 0x0701, 3, " ");
    setcursorpos(0, 7);
    kprintf(font, 0x0701, 3, " Call Trace (Last executed functions):");

    uint32_t curr_eip = r->eip;
    struct stack_frame *frame = (struct stack_frame*)r->ebp;

    int line = 9;
    for (int depth = 0; depth < 8; depth++) {
        uint32_t offset = 0;
        const char* func_name = ksym_resolve(curr_eip, &offset);

        setcursorpos(2, line++);
        kprintf(font, 0x0107, 3, "#%d [0x%08X] <%s + 0x%X>", depth, curr_eip, func_name, offset);

        // Проверка корректности указателя ebp (защита от циклов и вылета за стек)
        if (!frame || (uint32_t)frame < 0x1000 || ((uint32_t)frame & 3)) {
            break;
        }

        // Если стек поврежден или EBP указывает вниз (зацикливание)
        if (frame->ebp && (uint32_t)frame->ebp <= (uint32_t)frame) {
            break;
        }

        curr_eip = frame->eip;
        frame = frame->ebp;
    }

    // 3. Блок дополнительного текста ошибки
    setcursorpos(0, 18);
    for (int i = 0; i < 80; i++) kprintf(font, 0x0701, 3, " ");
    setcursorpos(0, 18);
    kprintf(font, 0x0701, 3, " Exception details:");
    setcursorpos(0, 20);
    kprintf(font, 0x0107, 3, "%s", crash_text);

    if (current_task_idx == 0) {
        while (1) asm volatile("hlt");
    }

    mt_kill_task(current_task_idx);
    return;
}

uint32_t tps = 1000;

void irq_install() {
    /* Enable interruptions */
    /* IRQ0: timer */
    for( int i=0;i<256;i++ ){
        register_interrupt_handler(i,none); 
    }
    register_interrupt_handler(0x0,dfh);
    register_interrupt_handler(0x1,mmio_debug_handler);
    register_interrupt_handler(0x6,dfh);
    register_interrupt_handler(0x8,dfh);
    register_interrupt_handler(0xb,dfh);
    register_interrupt_handler(0xd,dfh);
    register_interrupt_handler(14, mmio_page_fault_handler);
    register_interrupt_handler(0x80,int80h);
    
    /* IRQ1: keyboard */
	for (int i = IRQ0; i <= IRQ15; ++i){
		register_interrupt_handler(i, none);
	}
	register_interrupt_handler(IRQ14, doNothing);
	init_timer(tps);
	init_mouse();
    return;
}
