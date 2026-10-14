#include <stddef.h>
#include <../macros/macros.h>

volatile extern char font FONT_CL_CH;

#define kprint(x) printf(font,0xf,2,x);

void get_current_regs(registers_t *regs) {
    asm volatile (
        "movl %%ebp, %0\n\t"
        : "=m"(regs->ebp)
        :
        : "memory"
    );
}

void dump_callstack(registers_t *r) {
    kprint("\nCall stack:\n");

    uint32_t *ebp = (uint32_t*)r->ebp;
    char buf[16];
    for (int i = 0; i < 16 && ebp != NULL; i++) {
        // Проверка на валидность указателя
        if ((uint32_t)ebp < 0x1000 || (uint32_t)ebp > 0xFFFFFFF0) {
            kprint("Invalid EBP: ");
            int_to_ascii((uint32_t)ebp, buf);
            kprint(buf);
            kprint("\n");
            break;
        }

        uint32_t eip = ebp[1];

        kprint(" .");
        clear(buf,16);
        h2c(eip, buf);
        kprint(buf);
        kprint("\n");

        ebp = (uint32_t*)*ebp;
    }

    kprint("END\n");
}