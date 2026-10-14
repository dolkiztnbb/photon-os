#include <../../drivers/disk.h>
#include <../../drivers/mcfs.h>
#include <../../micro/main.h>
#include <../../buildinfo.h>
#include <stdint.h>


#define CRT_INDEX 0x3D4
#define CRT_DATA 0x3D5
#define DISPLAY_ENABLE_REG 0x1A

#define pr(x, ...) printf(font, 0x7, 2, x, ...);

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

int load()
{
    cls(0x0);
    /*
    outb(0x3C8, 0x00);
    for (int i = 0; i < 8; i++) {
        outb(0x3C9, (i&1)*64); // R
        outb(0x3C9, (i&1)*63); // R
        outb(0x3C9, (i&1)*63); // R
        //outb(0x3C9, (pal[i]>>1&1)*63); // G
        //outb(0x3C9, (pal[i]>>2&1)*63); // B
    }
    for (int i = 0; i < 32; i++) {
        outb(0x3C9, i*2|63); // R
        outb(0x3C9, i*2|63); // G
        outb(0x3C9, i*2|63); // B
    }
    //start from 40
    for (int i = 0; i < 216; i++) {
        outb(0x3C9, (i%6)*10); // R
        outb(0x3C9, (((int)(i/6))%6)*10); // G
        outb(0x3C9, (((int)(i/36))%6)*10); // B
    }*/
    cls(0);
    uintptr_t eip = get_eip();
    memory_copy(0x100, boot_device, 2);
    boot = 0xa0 + 0x10 * ((boot_device[0] & 0xff) - 0x80);
    setspacing(4, 6);
    printf(font, 0x7, 2, "[LOADER] PC (eip) = %X\n", eip);
    printf(font, 0x7, 2, "[LOADER] expecting %d sectors \n", get_total_sectors(boot));
    int off = 1;
    for (; off < 300; off++)
    {
        unsigned char *buffer = (unsigned char *)(mainaddress + (off - 1) * 0x200);
        if (read_sector(boot, off, buffer) != 0)
            break;
    }
    setspacing(4, 6);
    eip = get_eip();
    printf(font, 0x7, 2, "[LOADER] PC (eip) = %X\n", eip);
    printf(font, 0x7, 2, "[LOADER] Successfully loaded kernel to %X\n", mainaddress);
    asm ("nop;nop;nop;nop;");
    printf(font, 0xf, 2, "\nWelcome to ");
    printf(font, 0x9, 2, "PhotonOS!\n\n");
    unsigned char *p = (unsigned char *)0x900000;
    off = 0x0;
    printf(font, 0x7, 2, "[LOADER] expecting %d sectors \n", get_total_sectors(boot));
    while (read_sector(boot, off, p) == 0)
    {
        // printf(font,0x7,2,"check %X sect\n",off);
        if (p[0] == 'M' && p[1] == 'C' && p[2] == 'F' && p[3] == 'S')
        {
            printf(font, 0x7, 2, "[LOADER] found mcfs partition at offset=%X\n", off * 0x200);
            mcfs_p = off;
            break;
        }
        off++;
    }
    printf(font, 0x7, 2, "[LOADER] %s \n", BUILD_DATE);
    printf(font, 0x7, 2, "[LOADER] IDT MOZHET BYT UDALENA, DAZHE int6 UBIVAET SISTEMU \n");

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

int main()
{
    int c = 0;
    char line[128];
    char cmd[128];
    printf(font, 0x7, 2, "DRIVE %x\n", boot & 0xff);
    set_size(1, 1);
    registers_t regs; // Адрес, где можно хранить данные
    while (1)
    {
        printf(font, 0xf, 2, "^");
        wtc();
    asm("int $6");
        getline(line);
        split(line, cmd, ' ', 1);
        printf(font, 0xf, 2, "\n");
        if (strcmp(cmd, "re") == 0){
            port_byte_out(0xCF9, 0xE);
        }else if (strcmp(cmd, "help") == 0){
            printf(font, 0xf, 2, "re   reboot system\n");
            printf(font, 0xf, 2, "ls   list disk\n");
            printf(font, 0xf, 2, "lr   load program\n");
            printf(font, 0xf, 2, "li   load image\n");
            printf(font, 0xf, 2, "cat  print text from file\n");
            printf(font, 0xf, 2, "p    print all colors\n");
            printf(font, 0xf, 2, "c    hex editor for disk\n");
            printf(font, 0xf, 2, "cr   hex editor for ram\n");
            printf(font, 0xf, 2, "demo a small demo for OS\n");
            printf(font, 0xf, 2, "wd   copy disk to disk\n");
        }
        else if (strcmp(cmd, "ls") == 0){
            int fp = mcfs_p;
            char g[] = {0x0,0x1,0x2};
            unsigned char *rs = (unsigned char *)0x900000;
            read_sector(boot, fp, rs);
            unsigned long long size = rs[0x1b]|(rs[0x1a]<<8);
            printf(font, 0xf, 2, "ds %d\n", size);
            int zc=0;
            while((fp-mcfs_p)<size){
                read_sector(boot, fp, rs);
                if(rs[0]==0x80){
                    struct fileheader *x = rs;
                    printf(font, 0xf, 2, "f '%s'", x->name);
                    unsigned long long i;
                    printf(font, 0xf, 2, ", %d kb//%ds\n",((((x->lengthb2)<<8|(x->lengthb3))&0xffff))>>1,((x->lengthb2)<<8|(x->lengthb3))&0xffff);
                    i=((x->lengthb2)<<8|(x->lengthb3))&0xffff;
                    fp+=i-1;
                } else {
                    zc++;
                }

                if(zc>5)break;
                fp++;
            }
        }else if (strcmp(cmd, "lr") == 0){
            int fp = mcfs_p;
            char g[] = {0x0,0x1,0x2};
            unsigned char *rs = (unsigned char *)0x900000;
            read_sector(boot, fp, rs);
            unsigned long long size = rs[0x1b]|(rs[0x1a]<<8);
            split(line, cmd, ' ', 2); // the program to run
            printf(font, 0xf, 2, "Finding... %d\n");
            while((fp-mcfs_p)<size){
                read_sector(boot, fp, rs);
                if(rs[0]==0x80){
                    struct fileheader *x = rs;
                    unsigned long long i=((x->lengthb2)<<8|(x->lengthb3))&0xffff;
                    if ((strcmp(x->name, cmd)==0) || ((strlen(cmd)==2) && (strcmp("lr", cmd)==0))){
                        printf(font, 0x7, 2, "Found '%s' at s-%x\n",cmd,fp);
                        for(int p=0;p<i;p++){
                            read_sector(boot, fp+p+1, rs);
                            rs+=0x200;
                        }
                        rs-=0x200*i;
                        get_current_regs(&regs);
                        dump_callstack(&regs);
                        printf(font, 0xf, 2, "execute\n");
                        int(*func)() = (int(*)())rs;
                        int a=func();
                        printf(font, 0xf, 2, "end of execution, result is 0x%X(%d)\n",a,a);
                        get_current_regs(&regs);
                        dump_callstack(&regs);
                        break;
                    } else {
                        struct fileheader *x = rs;
                        printf(font, 0x7, 2, "name: '%s' ", x->name);
                        unsigned long long i;
                        printf(font, 0x7, 2, "size: %b\n", ((x->lengthb2)<<8|(x->lengthb3))&0xffff);
                    }
                }
                fp++;
            }
        }else if (strcmp(cmd, "cat") == 0){
            int fp = mcfs_p;
            unsigned char *rs = (unsigned char *)0xf00000;

            read_sector(boot, fp, rs);
            unsigned long long size = rs[0x1b] | (rs[0x1a] << 8);

            split(line, cmd, ' ', 2); // имя файла

            while ((fp - mcfs_p) < size)
            {
                read_sector(boot, fp, rs);
                if (rs[0] == 0x80)
                {
                    struct fileheader *x = rs;
                    unsigned long long i = ((x->lengthb2) << 8 | (x->lengthb3)) & 0xffff;

                    if ((strcmp(x->name, cmd) == 0) || ((strlen(cmd) == 2) && (strcmp("lr", cmd) == 0)))
                    {
                        for (int p = 0; p < i; p++)
                        {
                            read_sector(boot, fp + p + 1, rs + p * 0x200);
                        }
                        printf(font, 0x7, 2, "%s\n", rs);
                        break;
                    }
                }
                fp++;
            }
            if((fp - mcfs_p) >= size){
                printf(font, 0x7, 2, "file not found\n");
            }
        }else if (strcmp(cmd, "li") == 0){
            int fp = mcfs_p;
            unsigned char *rs = (unsigned char *)0xf00000;

            read_sector(boot, fp, rs);
            unsigned long long size = rs[0x1b] | (rs[0x1a] << 8);

            split(line, cmd, ' ', 2); // имя файла
            printf(font, 0xf, 2, "Finding... %d\n", fp);

            while ((fp - mcfs_p) < size)
            {
                read_sector(boot, fp, rs);
                if (rs[0] == 0x80)
                {
                    struct fileheader *x = rs;
                    unsigned long long i = ((x->lengthb2) << 8 | (x->lengthb3)) & 0xffff;

                    if ((strcmp(x->name, cmd) == 0) || ((strlen(cmd) == 2) && (strcmp("lr", cmd) == 0)))
                    {
                        printf(font, 0x7, 2, "Found '%s' at s-%x\n", cmd, fp);

                        // Читаем весь файл в память
                        for (int p = 0; p < i; p++)
                        {
                            read_sector(boot, fp + p + 1, rs + p * 0x200);
                        }

                        // Теперь rs указывает на начало файла
                        uint8_t *image_data = rs;

                        // Чтение ширины и высоты
                        uint16_t height = ((uint16_t)image_data[0] << 8) | image_data[1];
                        uint16_t width = ((uint16_t)image_data[2] << 8) | image_data[3];
                        // Данные изображения начинаются после 4 байт заголовка
                        uint8_t *pixels = image_data + 4;
                        // Теперь можешь использовать width, height и pixels
                        // Например, передать их в функцию отрисовки
                        cls(0x0);
                        for(int y=0;y<200;y++){
                            for(int x=0;x<320;x++){
                                swrite(x,y,((x>>4)^(y>>4))&1?0x12:0x13);
                            }
                        }
                        for(int y=0;y<height;y++){
                            for(int x=0;x<width;x++){
                                swrite(x+160-width/2,y,pixels[x+y*(width)]);
                            }
                        }
                        con_setp(0,32);
                        printf(font, 0x7, 3, "'%s', %d x %d (%ds or %dkb)\n", cmd, width, height,width*height/0x200,width*height/1024);
                        wtc();
                        wtc();
                        cls(0x0);
                        break;
                    }
                }
                fp++;
            }
        }else if (strcmp(cmd, "p") == 0){
            for(int i=0;i<256;i++){
                printf(font, i, 2, "@");
                if(i%24==0)printf(font, 0x0f, 3, "\n");
            }
            printf(font, 0x0f, 3, "\n");
        }else if (strcmp(cmd, "c") == 0){
            setscroll(0);
            cls(0x0);
            micro_hx(mcfs_p, boot);
            setscroll(1);
        }else if (strcmp(cmd, "cr") == 0){
            setscroll(0);
            cls(0x0);
            micro_hx_r();
            setscroll(1);
        }else if (strcmp(cmd, "rd") == 0){
            split(line, cmd, ' ', 2);
            char dt[512] = {};
            int w = atoi(cmd);
            read_sector(boot, w, dt);
            printf(font, 0x07, 2, "o-%X s-%X (s-%d)\n", w * 0x200, w, w);
            hexd(dt,w*0x200,-1);
        }else if (strcmp(cmd, "wb") == 0){
            split(line, cmd, ' ', 2);
            int byte = atoi(cmd);
            split(line, cmd, ' ', 3);
            int sector = atoi(cmd);
            split(line, cmd, ' ', 4);
            int offset = atoi(cmd);
            printf(font, 0xf, 2, "writing b %X o-%d-s-%d\n", byte, offset, sector);
            char dt[512] = {};
            read_sector(boot, sector, dt);
            dt[offset] = byte;
            write_sector(boot, sector, dt);
        }else if (strcmp(cmd, "demo") == 0){
            set_size(2, 2);
            ignorexoverflow(1);
            cls(0x0);
            for (int j = 0; j < strlen(f) * 6; j++)
            {
                // while(1){};
                /*for(int y=0;y<16;y++){
                    for(int x=0;x<40;x++){
                    rect(x*8,y*8,8,8,((x^y)&0xf)+0x10);
                    }
                }*/
                c = 0;
                for (int i = 0; i < strlen(f); i++)
                {
                    float t = (i + j) / M_PI * 16;
                    float x = ((int)(t) & 255) * (1 - ((int)(t) & 255) / 256.) * (1 - ((int)(t) & 256) / 128.) / 32.;
                    unsigned short bgcol = (0x20 + (((c + j) / 3) % 24) + 0x60 - 0x18) << 8;
                    unsigned short bgcol2 = (0x20 + (((c + j) / 3) % 24) + 0x90) << 8;
                    unsigned short fgcol = 0x0020 + (((c + j) / 3) % 24);
                    if (i * 5 - j + 160 > -3 && i * 5 - j + 160 < 160)
                    {
                        drawchar(font, 0, i * 5 - j + 160, x * 10 + 50 + 11, 0, 1);
                        drawchar(font, 0, i * 5 - j + 160, x * 10 + 46 - 11, 0, 1);
                        drawchar(font, 0, i * 5 - j + 160, x * 10 + 50 + 7, bgcol2, 1);
                        drawchar(font, 0, i * 5 - j + 160, x * 10 + 46 - 7, bgcol2, 1);
                        drawchar(font, 0, i * 5 - j + 160, x * 10 + 50 + 1, bgcol, 1);
                        drawchar(font, 0, i * 5 - j + 160, x * 10 + 46 - 1, bgcol, 1);
                        drawchar(font, f[i], i * 5 - j + 160, x * 10 + 48, fgcol, 2);
                    }
                    c++;
                }
                for(int i=0;i<4000000;i++)asm("nop");
                //delay(25);
            }
            set_size(1, 1);
            ignorexoverflow(0);
            cls(0x0);
        }else if (strcmp(cmd, "wd") == 0){
            while(1){
                wtc();
                char src,dst ;
                int hdas=get_total_sectors(0xa0);
                int hdbs=get_total_sectors(0xb0);
                
                printf(font, 0x7, 2, "%d sectors on HDA\n", hdas);
                printf(font, 0x7, 2, "%d sectors on HDB\n", hdbs);
                printf(font, 0x7, 3, "Writing until first error.\n");
                printf(font, 0xf, 3, "WARNING: ALL OF THE DATA IN FS WILL BE COPIED TO THE DESTINATION!\n");
                printf(font, 0x7, 3, "Source:\n");
                printf(font, 0x7, 3, "   [A] - HDA\n");
                printf(font, 0x7, 3, "   [B] - HDB\n>[");
                wtc();
                src = getkey();
                if(src!='A' && src!='B'){
                    printf(font, 0xf, 3, "ABORTED\n");
                    break;
                } else {
                    printf(font, 0x7, 3, "%c]\n",src);
                }
                wtc();
                printf(font, 0x7, 3, "Destination:\n");
                printf(font, 0x7, 3, "   [A] - HDA\n");
                printf(font, 0x7, 3, "   [B] - HDB\n>[");
                wtc();
                dst = getkey();
                if(dst!='A' && dst!='B'){
                    printf(font, 0xf, 3, "ABORTED\n");
                    break;
                } else {
                    printf(font, 0x7, 3, "%c]\n",dst);
                }
                char p[512];
                int sect=0;
                if(src=='A')src=0xa0;
                if(dst=='A')dst=0xa0;
                if(src=='B')src=0xb0;
                if(dst=='B')dst=0xb0;
                printf(font, 0x7, 3, "Copying now.\n");
                con_setcolumn(0);
                printf(font, 0x7, 3, "copied %d s (%d kb or %f mb)",sect,sect/2,sect/2./1024.);
                hdas=get_total_sectors(0xa0);
                hdbs=get_total_sectors(0xb0);
                while(sect<get_total_sectors(src)){
                    clear(p,512);
                    hdas=get_total_sectors(src);
                    read_sector(src,sect,p);
                    hdbs=get_total_sectors(dst);
                    write_sector(dst,sect,p);
                    sect++;
                    if((sect%128)==0){
                        con_setcolumn(0);
                        printf(font, 0x7, 3, "copied %d s (%d kb or %f mb)",sect,sect/2,sect/2./1024.);
                    }
                    if((sect%16)==0){
                        printf(font, 0x7, 3, ".");
                    }
                    for(int i=0;i<3500000;i++)asm("nop");
                }
                con_setcolumn(0);
                printf(font, 0xa, 3, "copied %d s (%d kb or %f mb)              \n",sect,sect/2,sect/2./1024.);
                break;
            }
        }
    }
}
