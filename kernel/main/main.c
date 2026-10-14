#include <../../drivers/disk.h>
#include <../../drivers/mcfs.h>
#include <../../cpu/idt.h>
#include <../../cpu/isr.h>
//#include <../../micro/main.h>
#include <../../buildinfo.h>
#include <../kernel.h>
#include <stdint.h>


#define CRT_INDEX 0x3D4
#define CRT_DATA 0x3D5
#define DISPLAY_ENABLE_REG 0x1A

#define pr(x, ...) printf(font, 0x7, 2, x, __VA_ARGS__);

uintptr_t get_eip(void)
{
    uintptr_t eip;
    __asm__ volatile(
        "call 1f\n" // Вызываем следующую метку
        "1: pop %0" // Забираем адрес из стека (это адрес метки 1)
        : "=r"(eip));
    return eip;
}

char boot_device[2];
char boot;

int mcfs_p = 0;


int load()
{
    //cls(0x0);
    uintptr_t eip = get_eip();
    memory_copy(0x100, boot_device, 2);
    boot = 0xa0 + 0x10 * ((boot_device[0] & 0xff) - 0x80);
    setspacing(4, 6);
	printf(font, 0x7, 2, "[LOADER] PC (eip) = %X\n", eip);
    // printf(font, 0x7, 2, "[LOADER] expecting %d sectors \n", get_total_sectors(boot));
    int off = 1;
    for (; off < 300; off++)
    {
        unsigned char *buffer = (unsigned char *)(mainaddress + (off - 1) * 0x200);
        if (read_sector(boot, off, buffer) != 0)
            break;
    }
    setspacing(4, 6);
    eip = get_eip();
    // printf(font, 0x7, 2, "[LOADER] PC (eip) = %X\n", eip);
	printf(font, 0x7, 2, "[LOADER] Successfully loaded kernel to %X\n", mainaddress);
    asm ("nop");
    unsigned char *p = (unsigned char *)0x900000;
    off = 0x0;
    //printf(font, 0x7, 2, "[LOADER] expecting %d sectors \n", get_total_sectors(boot));
    get_total_sectors(boot);
    while (read_sector(boot, off, p) == 0)
    {
        //printf(font,0x7,2,"check %X sect\n",off);
        if (p[0] == 'M' && p[1] == 'C' && p[2] == 'F' && p[3] == 'S')
        {
            //printf(font, 0x7, 2, "[LOADER] found mcfs partition at offset=%X\n", off * 0x200);
            mcfs_p = off;
            break;
        }
        off++;
    }
    // printf(font, 0x7, 2, "[LOADER] %s \n", BUILD_DATE);
    //printf(font, 0x7, 2, "\n");

}

// Гречь Мрак Анала
#define VGA_RESET_PORT 0x3C2

const char f[] = "PHOTONOS by 039bb & Kamanodzu. Made from Nov '24 - May '25 on C and ASseMbly, capable of loading data from disk and running from RAM. " /*"Rise and shine, Mister Freeman rise and shine. Not that I wish to imply that you have been sleeping on the job. No one is more deserving of a rest, and all the effort in the world would have gone to waste until, well let's just say your hour has come again. The right man in the wrong place can make all the difference in the world. So, wake up, Mister Freeman, wake up and smell the ashes."*/;


struct fileheader {
    char type;
    char attributes;
    char name[16];
    char lengthb0;
    char lengthb1;
    char lengthb2;
    char lengthb3;
};

uint32_t swap_endian_32(uint32_t value) {
    return ((value >> 24) & 0x000000FF) |
           ((value >> 8)  & 0x0000FF00) |
           ((value << 8)  & 0x00FF0000) |
           ((value << 24) & 0xFF000000);
}

void hexd(char *dt, int offset, int sel)
{
    int w = offset;
    for (int y = 0; y < 32; y++)
    {
        con_setcolumn(0);
        printf(font, 0x07, 2, "%X", (y * 0x10 + w) & 0xfffff);
        con_setcolumn(5);
        printf(font, 0x0f, 2, "|");
        for (int x = 0; x < 8; x++)
        {
            con_setcolumn(x * 2 + 6);
            if (sel == (x + y * 16))
            {
                printf(font, (0x09) << 8, 3, "%X", (dt[x + y * 16]) & 0xff);
            }
            else
            {
                printf(font, (0x07 + 0x8 * (x & 1)), 3, "%X", (dt[x + y * 16]) & 0xff);
            }
        }
        for (int x = 8; x < 16; x++)
        {
            con_setcolumn(x * 2 + 1 + 6);
            if (sel == (x + y * 16))
            {
                printf(font, (0x09) << 8, 3, "%X", (dt[x + y * 16]) & 0xff);
            }
            else
            {
                printf(font, (0x07 + 0x8 * (x & 1)), 3, "%X", (dt[x + y * 16]) & 0xff);
            }
        }

        con_setcolumn(39);
        printf(font, 0x0f, 2, "|");
        for (int x = 0; x < 16; x++)
        {
            con_setcolumn(x + 40);
            if (((dt[x + y * 16]) & 0xff) < 0x1f)
            {
                //printf(font, 0x0f, 2, ".", (dt[x + y * 16]) & 0xff);
            }
            else
            {
                printf(font, 0x0f, 2, "%c", (dt[x + y * 16]) & 0xff);
            }
        }
        printf(font, 0x0c, 0, "\n");
    }
}

#define dbg asm("int $1");

int main()
{
    //cls(0x0);
    int c = 0;
    char line[128];
    char cmd[128]="a init";
    set_size(1, 1);
    int fp = mcfs_p;
    unsigned char *rs = (unsigned char *)0xf00000;
    read_sector(boot, fp, rs);
    unsigned long long size = rs[0x1b]|(rs[0x1a]<<8);
    split(line, cmd, ' ', 2); // the program to run
    boot&=0xff;
    while((fp-mcfs_p)<size){
        read_sector(boot, fp, rs);
        if (rs[0] == 0x80){
            struct fileheader *x = rs;
            unsigned long long i=((x->lengthb2)<<8|(x->lengthb3))&0xffff;
            if ((strcmp(x->name, "init")==0)){
                for(int p=0;p<i;p++){
                    read_sector(boot, fp+p+1, rs);
                    rs+=0x200;
                }
                rs-=0x200*i;
				int esp = 0;
				asm volatile("movl %%esp, %0" : "=r"(esp) : : "esp");
                int(*func)() = (int(*)())((void*)rs);
                printf(font, 0xf, 2, "\n\nLoading init executable...\n");
                int a=func();
                printf(font, 0xc, 2, "\n\nRoot have exited with return code 0x%X (%d)!\n",a,a);
                break;
            } else {}
        }
        fp++;
        for(int i=0;i<200;i++)asm(
            "nop"
        );
    }
    printf(font, 0xc, 2, "Main ended?\n");
    //while(1);
}