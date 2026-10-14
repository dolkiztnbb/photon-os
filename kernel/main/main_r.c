// #include <../../drivers/mouse.h>
#include <../../drivers/disk.h>
#include <../../macros/macros.h>
int boot = 0;

int *ret;

int test(int x)
{
    // print(font,"job, given arg ",0x7,2);
    // print_int(font,x,0x7,2);
    // print(font,"; ",0x7,2);
    if (*ret == 0){
        print(font, "dead!!\n", 0x7, 2);
    }
    return -1 * (*ret);
}
uintptr_t get_eip(void) {
    uintptr_t eip;
    __asm__ volatile (
        "call 1f\n"         // Вызываем следующую метку
        "1: pop %0"         // Забираем адрес из стека (это адрес метки 1)
        : "=r"(eip)
    );
    return eip;
}



/// @brief
/// @return
int main()
{
    cls(0xf);
    delay(100);
    cls(0x0);
    delay(100);

    setspacing(5, 6);
    char m[64];
    char cmd[64];
    char p[2];
    char n;
    char boot_device[2];
    memory_copy(0x8c00, boot_device, 2);
    boot = (boot_device[0] & 0xff);

    int timeout = 0;
    unsigned char* buffer = (unsigned char*)0x7c00;
    char drive = 0xa0+(boot-0x80)*0x10;
    read_sector(drive,0,buffer);
    if( buffer[0x199]==0x80 &&
        buffer[0x19a]==0x08 &&
        buffer[0x19b]==0x11 ) {
            printf(font, 0xf, 2, "Running off PHTOS bootloader\n");
            printf(font, 0x7, 2, "expecting %d sectors \n", get_total_sectors(drive));
            int off=1;
    wtc();
    wtc();
            for(;off<300;off++){
                buffer = (unsigned char*)(mainaddress+(off-1)*0x200);
                read_sector(0xa0,off,buffer);
            }
            printf(font, 0x7, 2, "Loaded %d sectors \n",off);
    }
    wtc();
    wtc();

    uintptr_t eip = get_eip();
    printf(font, 0xf, 2, "PC (eip) = %X\n", eip);
    if (eip < mainaddress) {
        asm volatile ("jmp *%0" :: "r"(mainaddress)); // Переход по адресу mainaddress
    } else {
        printf(font, 0x0f00, 3, "All done!\n");
        printf(font, 0x0f00, 3, "Press any key to continue...\n");
        wtc();
        wtc();
    }
    cls(0x0);
    con_setp(3, 2);
    print(font, "PhotonOS Bootloader", 0x7, 2);
    con_setp(3, 4);
    print(font, "1. Boot to kernel", 0x7, 2);
    con_setp(3, 5);
    print(font, "2. Boot to desktop", 0x7, 2);
    *ret = 1;
    wtc();
    n = getkey();
    cls(0x0);
    setscroll(2);
    if (n == '!')
    {
        // debug_MnPMTM(1);
        // init_MnPMTM();
        // stack_add(test,9,0);
        // stack_add(test,13,1);
        // stack_add(test,150,2);
        set_hvm(1);
        while (1)
        {
            printf(font, 0x1, 2, "kernel");
            printf(font, 0xf, 2, "@");
            printf(font, 0x9, 2, "machine");
            printf(font, 0xf, 2, " >");
            wtc();
            getline(m);
            split(m, cmd, ' ', 1);
            printf(font, 0xf, 2, "\n");

            if (strcmp(cmd, "hex") == 0)
            {

                split(m, cmd, ' ', 2);
                int x = atoi(cmd);
                printf(font, 0x7, 2, "bin: %b\n", x);
                printf(font, 0x7, 2, "dec: %d\n", x);
                printf(font, 0x7, 2, "hex: %x\n", x);
            }
            else if (strcmp(cmd, "ct") == 0)
            {
                cls(0x0);
                for (char y = 0; y < 4; y++)
                {
                    for (char x = 0; x < 32; x++)
                    {
                        con_setp(x, y);
                        print_char(font, x + y * 32, 0x0f00, DRAW_BG | DRAW_FG);
                    }
                    print_char(font, '\n', 0x7, 2);
                }
                for (char y = 0; y < 8; y++)
                {
                    for (char x = 0; x < 32; x++)
                    {
                        con_setp(x + 32, y);
                        print_char(font, x + y * 32, 0x000f, DRAW_BG | DRAW_FG);
                    }
                    print_char(font, '\n', 0x7, 2);
                }
                con_setp(0, 4);
            }

            else if (strcmp(cmd, "ss") == 0)
            {
                split(m, cmd, ' ', 2);
                int x = atoi(cmd);
                printf(font, 0xf, 2, "Set scrolling to %d\n", x);
                setscroll(x);
            }

            else if (strcmp(cmd, "inb") == 0)
            {
                split(m, cmd, ' ', 2);
                int x = atoi(cmd);
                printf(font, 0x7, 2, "inb(%d) -> %d (0x%X)\n", x, inb(x), inb(x));
            }

            else if (strcmp(cmd, "outb") == 0)
            {
                split(m, cmd, ' ', 2);
                int x = atoi(cmd);
                split(m, cmd, ' ', 3);
                int y = atoi(cmd);
                printf(font, 0x7, 2, "outb(addr=%d (0x%X) val=%d (0x%X))\n", x, x, y, y);
                outb(x, y);
            }

            else if (strcmp(cmd, "re") == 0)
            {
                port_byte_out(0xCF9, 0xE);
            }

            else if (strcmp(cmd, "inw") == 0)
            {
                split(m, cmd, ' ', 2);
                int x = atoi(cmd);
                printf(font, 0x7, 2, "inw(%d) -> %d\n", x, port_word_in(x));
            }

            else if (strcmp(cmd, "outw") == 0)
            {
                split(m, cmd, ' ', 2);
                int x = atoi(cmd);
                split(m, cmd, ' ', 3);
                int y = atoi(cmd);
                printf(font, 0x7, 2, "outw(addr=%d val=%d)\n", x, y);
                port_word_out(x, y);
            }

            else if (strcmp(m, "micro") == 0)
            {
                // launch_micro();
            }

            else if (strcmp(m, "help") == 0)
            {
                printf(font, 0xf, 2,
                       "job    lsst   help   ctsk   dbg    int    dev    cls    qf\n"
                       "inb    inw    outb   outw   ct\n");
            }

            else if (strcmp(cmd, "ctsk") == 0)
            {
                split(m, cmd, ' ', 2);
                int x = atoi(cmd);
                split(m, cmd, ' ', 3);
                int y = atoi(cmd);
                printf(font, 0xf, 2, "ARG %d ID %d\n", x, y);
                // stack_add(test,10,20);
            }

            else if (strcmp(cmd, "cls") == 0)
            {
                cls(0x0);
            }

            else if (strcmp(m, "dbg") == 0)
            {
                // debug_MnPMTM(1);
            }

            else if (strcmp(cmd, "dev") == 0)
            {
                if (boot == 0x80 || boot == 0x81)
                {
                    printf(font, 0x7, 2, "hd%d\n", boot - 0x80);
                }
                else if (boot == 0x00 || boot == 0x01)
                {
                    printf(font, 0x7, 2, "fd%d\n", boot);
                }
                else
                {
                    printf(font, 0x7, 2, "ud%d\n", boot);
                }
            }

            else if (strcmp(m, "int") == 0)
            {
                printf(font, 0x7, 2,
                       "Mode: [(a)dd (s)ubstract (m)ultiply (d)ivide] ");

                while (getkey() == 0)
                    ;
                char op = getkey();
                printf(font, 0xf, 2, "%c\n", op);

                size_t a, b;
                printf(font, 0x7, 2, "\nNumber A: ");
                wtc();
                getline(m);
                a = atoi(m);

                printf(font, 0x7, 2, "\nNumber B: ");
                wtc();
                getline(m);
                b = atoi(m);

                printf(font, 0x7, 2, "\nResult:\n          >");

                if (op == 'A')
                    printf(font, 0xf, 2, "%d", a + b);
                if (op == 'S')
                    printf(font, 0xf, 2, "%d", a - b);
                if (op == 'D')
                    printf(font, 0xf, 2, "%f", (double)a / (double)b);
                if (op == 'M')
                    printf(font, 0xf, 2, "%f", (double)a * (double)b);

                printf(font, 0x7, 2, "<\n");
            }

            else if (strlen(cmd) != 0)
            {
                printf(font, 0xf, 2, "no known function named '%s'\n", cmd);
            }
        }
    }
    else if (n == '@')
    {
        /*setcursorpos(32,24);
        print(font,"Unsupported function\n",0xf,2);
        for(int i=0;i<3;i++){
            setcursorpos(32,24+16);
            print(font, "* Reboot in ", 0x7, DRAW_FG|DRAW_BG);
            print_int(font, 3-i, 0xf, DRAW_FG|DRAW_BG);
            print(font, " *", 0xf, DRAW_FG|DRAW_BG);
            delay(1000);
        }
        port_byte_out(0xCF9, 0xE);
    */
        int x = 0;
        int y = 0;
        cls(0x0);
        while (1)
        {
            rect(x, y, 2, 2, 0xf);
            while (getkey() == 0)
            {
                for (int i = 0; i < 0xf; i++)
                {
                    rect(x, y, 2, 2, 0x10 + i);
                    delay(20);
                    if (getkey() != 0)
                    {
                        rect(x, y, 2, 2, 0xf);
                        break;
                    }
                }
            };
            char z = getkey();
            if (z == 56)
                y--;
            if (z == 50)
                y++;
            if (z == 52)
                x--;
            if (z == 54)
                x++;
            setcursorpos(x, y);
            delay(25); // while(getkey()!=0);
        }
    }
    else
    {
        setcursorpos(32, 24);
        print(font, "Invalid function\n", 0xf, 2);
        for (int i = 0; i < 3; i++)
        {
            setcursorpos(32, 24 + 16);
            print(font, "* Reboot in ", 0x7, DRAW_FG | DRAW_BG);
            print_int(font, 3 - i, 0xf, DRAW_FG | DRAW_BG);
            print(font, " *", 0xf, DRAW_FG | DRAW_BG);
            delay(1000);
        }
        port_byte_out(0xCF9, 0xE);
    }
}
