#include <stdint.h>
#include <cpu/ports.h>
#define port_word_out outl
#include <drivers/dumb-ahci.h>
#include <drivers/dumb-pci.h>


// #include <debug.h>



// By assumption user has 16 megs of RAM. And so we have these free patches:
// 0x000100 - 0x007c00 (Before the place that we not are at.)
// 0x007e00 - 0x009000 (Before the place that we are at.)
// 0x00f800 - 0x0a0000 (Between the place and VRAM.)
// 0x100000 - 0xbfffff (Base memory)
// 0xc00000 - 0xcfffff (AHCI)
// 0xd00000 - 0xffffff (Allocatable)

// 21.01.26 stack leaks now
// 21.01.26 stack now doesnt leak
// 24.01.26 transferred screen.c to text mode
// 29.01.26 now it switches to graphics mode in 3-3-2 RGB mode

void __attribute__((section(".text.kernel_main"))) kernel_main() {
    char* vmem=(char*)0xb8000;
    asm("cli;");
    uint32_t abar = pci_scan_bus();
    initialize_ahci_and_read(abar);
    char* init_dest = (char*)0x100000;
    
    for (int i = 0; i < 310; i++) {
        if(ahci_read_sector(60 + i, init_dest + (i * 512))){
            while(1);
        };
        for(int j=0;j<512;j++){
            vmem[j*2]=init_dest[i*512+j];
            vmem[j*2+1]=0x1b;
        }
        for(int i=0;i<1000;i++)asm("nop");
    }

    // kprintd("Jumping to Init at 0x100000...\n");
    void (*init_entry)() = (void (*)())0x100000;
    init_entry();
    vmem[0]='!';
    vmem[1]=0x40;
}
