#ifndef ISR_H
#define ISR_H

#include <libc/stdint>

/* Use macros to generate ISR declarations */
#define DISR(n) extern void isr##n();
#define DIRQ(n) extern void irq##n();

/* Generate ISR declarations */
DISR(0)  DISR(1)  DISR(2)  DISR(3)
DISR(4)  DISR(5)  DISR(6)  DISR(7)
DISR(8)  DISR(9)  DISR(10) DISR(11)
DISR(12) DISR(13) DISR(14) DISR(15)
DISR(16) DISR(17) DISR(18) DISR(19)
DISR(20) DISR(21) DISR(22) DISR(23)
DISR(24) DISR(25) DISR(26) DISR(27)
DISR(28) DISR(29) DISR(30) DISR(31)
DISR(48) DISR(49)

DISR(50) DISR(51) DISR(52) DISR(53) DISR(54)
DISR(55) DISR(56) DISR(57) DISR(58) DISR(59)
DISR(60) DISR(61) DISR(62) DISR(63) DISR(64)
DISR(65) DISR(66) DISR(67) DISR(68) DISR(69)
DISR(70) DISR(71) DISR(72) DISR(73) DISR(74)
DISR(75) DISR(76) DISR(77) DISR(78) DISR(79)
DISR(80) DISR(81) DISR(82) DISR(83) DISR(84)
DISR(85) DISR(86) DISR(87) DISR(88) DISR(89)
DISR(90) DISR(91) DISR(92) DISR(93) DISR(94)
DISR(95) DISR(96) DISR(97) DISR(98) DISR(99)

DISR(100) DISR(101) DISR(102) DISR(103) DISR(104)
DISR(105) DISR(106) DISR(107) DISR(108) DISR(109)
DISR(110) DISR(111) DISR(112) DISR(113) DISR(114)
DISR(115) DISR(116) DISR(117) DISR(118) DISR(119)
DISR(120) DISR(121) DISR(122) DISR(123) DISR(124)
DISR(125) DISR(126) DISR(127) DISR(128) DISR(129)
DISR(130) DISR(131) DISR(132) DISR(133) DISR(134)
DISR(135) DISR(136) DISR(137) DISR(138) DISR(139)
DISR(140) DISR(141) DISR(142) DISR(143) DISR(144)
DISR(145) DISR(146) DISR(147) DISR(148) DISR(149)

DISR(150) DISR(151) DISR(152) DISR(153) DISR(154)
DISR(155) DISR(156) DISR(157) DISR(158) DISR(159)
DISR(160) DISR(161) DISR(162) DISR(163) DISR(164)
DISR(165) DISR(166) DISR(167) DISR(168) DISR(169)
DISR(170) DISR(171) DISR(172) DISR(173) DISR(174)
DISR(175) DISR(176) DISR(177) DISR(178) DISR(179)
DISR(180) DISR(181) DISR(182) DISR(183) DISR(184)
DISR(185) DISR(186) DISR(187) DISR(188) DISR(189)
DISR(190) DISR(191) DISR(192) DISR(193) DISR(194)
DISR(195) DISR(196) DISR(197) DISR(198) DISR(199)

DISR(200) DISR(201) DISR(202) DISR(203) DISR(204)
DISR(205) DISR(206) DISR(207) DISR(208) DISR(209)
DISR(210) DISR(211) DISR(212) DISR(213) DISR(214)
DISR(215) DISR(216) DISR(217) DISR(218) DISR(219)
DISR(220) DISR(221) DISR(222) DISR(223) DISR(224)
DISR(225) DISR(226) DISR(227) DISR(228) DISR(229)
DISR(230) DISR(231) DISR(232) DISR(233) DISR(234)
DISR(235) DISR(236) DISR(237) DISR(238) DISR(239)
DISR(240) DISR(241) DISR(242) DISR(243) DISR(244)
DISR(245) DISR(246) DISR(247) DISR(248) DISR(249)

DISR(250) DISR(251) DISR(252) DISR(253) DISR(254)
DISR(255)

/* Generate IRQ declarations */
DIRQ(0)  DIRQ(1)  DIRQ(2)  DIRQ(3)
DIRQ(4)  DIRQ(5)  DIRQ(6)  DIRQ(7)
DIRQ(8)  DIRQ(9)  DIRQ(10) DIRQ(11)
DIRQ(12) DIRQ(13) DIRQ(14) DIRQ(15)

/* IRQ definitions */
#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ3 35
#define IRQ4 36
#define IRQ5 37
#define IRQ6 38
#define IRQ7 39
#define IRQ8 40
#define IRQ9 41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

typedef struct {
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t useresp;
    uint32_t ss;
} registers_t;

void isr_install();
void isr_handler(const registers_t *r);
void irq_install();

typedef void (*isr_t)(const registers_t*);
void register_interrupt_handler(const uint8_t n, const isr_t handler);

#endif