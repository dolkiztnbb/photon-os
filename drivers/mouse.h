#include <libc/stdint>
#include <drivers/screen.h>
#include <cpu/timer.h>
#include <cpu/ports.h>
#define KBC_CMD_REG 0x64    // Регистр команды контроллера клавиатуры
#define KBC_DATA_REG 0x60   // Регистр данных контроллера клавиатуры

#define inb port_byte_in
#define outb port_byte_out
void outport64(unsigned char data){
    volatile unsigned char good = 0x02;
    while ((good & 0x02) != 0)
        good = inb(0x64);
    outb(0x64, data);
}
void outport60(unsigned char data)
{
    volatile unsigned char good = 0x02;
    while ((good & 0x02) != 0)
        good = inb(0x64);
    outb(0x60, data);
}
unsigned char inport64(){
    return inb(0x64);
}
unsigned char inport60(){
    return inb(0x64);
}
#define outb port_byte_out


// Инициализация мыши
int mouse_init() {
    int x;
    unsigned char data_read,mouse_type;
    for(x = 0; x < 5; x++)
    {
        outport64(0xA7);                    //reset every thing
        outport64(0xA8);
        outport64(0xD4);
        outport60(0xF5);
        data_read = inport60();             //    did it 
        if (data_read != 0xFA) continue;    //    work ?
        outport64(0xD4);
        outport60(0xFF);
        data_read = inport60();             //    did it
        if (data_read != 0xFA) continue;    //    work ?
        data_read = inport60();             //    did the self-
        if (data_read != 0xAA) continue;    //    test work ?
        mouse_type = inport60();
        outport64(0xD4);
        outport60(0xE6);
        data_read = inport60();             //    did it
        if (data_read != 0xFA) continue;    //    work ?
        outport64(0x20);                    
        data_read = inport60();             //
        data_read |= 0x02;                  // 
        outport64(0x60);                    //    get it to report data
        outport60(data_read);               //
        outport64(0xD4);                    //
        outport60(0xF4);
        data_read = inport60();             //    did it
        if (data_read != 0xFA) continue;    //    work ?
        break;
    }
}


// byte 0 data[0]
// byte 1 data[1]
// byte 2 data[2]
// byte 3 button[0]
// byte 4 button[1]
// byte 5 button[2]
// byte 6 dx
// byte 7 dy

void ps2_mouse(char* data)
{
    data[0] = inport60();
    data[1] = inport60();
    data[2] = inport60();
    if ((data[0] & 0x01) != data[3])
    {
        data[3] ^= 1;
    }
    if ((data[0] & 0x04) != data[4])
    {
        data[4] ^= 1;
    }
    if ((data[0] & 0x02) != data[5])
    {
        data[5] ^= 1;
    }
    if (data[0] & 0x10)data[6] = (char)((256 - data[1]) * -1);
    else data[6] = (char)data[1];
    if (data[0] & 0x20)data[7] = (char)(256 - data[2]);
    else data[7] = (char)(data[2] * -1);

    if (data[7] > 184) data[7] = 184;
    else if (data[7] < 0) data[7] = 0;
    if (data[6] > 311) data[6] = 311;
    else if (data[6] < 0) data[6] = 0;
}