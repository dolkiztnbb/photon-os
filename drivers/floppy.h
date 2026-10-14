#include <../libc/stdint>
#include <../drivers/screen.h>
#include <../cpu/timer.h>

#define ATA_PRIMARY_BASE 0x1F0
#define ATA_SECONDARY_BASE 0x170
#define ATA_REG_DEVICE 0
#define ATA_REG_DATA (0x00)
#define ATA_REG_ERROR (0x01)
#define ATA_REG_FEATURES (0x01)
#define ATA_REG_SECTOR_COUNT (0x02)
#define ATA_REG_LBA_LOW (0x03)
#define ATA_REG_LBA_MID (0x04)
#define ATA_REG_LBA_HIGH (0x05)
#define ATA_REG_DRIVE_SELECT (0x06)
#define ATA_REG_COMMAND (0x07)
#define ATA_REG_STATUS (0x07)
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define FLOPPY_DOR         0x3F2 
#define FLOPPY_MSR         0x3F4 
#define FLOPPY_DATA        0x3F5 
#define FLOPPY_RATE        0x3F7
#define DEVICE_TYPE_HDD 'H'
#define DEVICE_TYPE_FDD 'F'


#define inb port_byte_in
#define outb port_byte_out

char dbgm;

void hdd_write_sector(const uint8_t drive, const uint32_t lba, const void* buf){
    write_sector(drive, lba, buf);
}

void hdd_read_sector(const uint8_t drive, const uint32_t lba, void* buf){
    read_sector(drive, lba, buf);
}



typedef struct {
    uint8_t sides;           
    uint8_t tracks;          
    uint8_t sectors_per_track; 
    uint16_t bytes_per_sector; 
} FloppyParameters;

const FloppyParameters floppy_configs[] = {
    {2, 80, 9, 512},  
    {2, 80, 18, 512}, 
    {2, 40, 9, 512},  
};
#define NUM_CONFIGS (sizeof(floppy_configs) / sizeof(floppy_configs[0]))

int try_read_sector(uint8_t drive, const FloppyParameters* params, uint8_t* buffer) {
    
    for (uint8_t head = 0; head < params->sides; head++) {
        for (uint8_t track = 0; track < 2; track++) { 
            if (!floppy_read_sector(drive, track, head, 1, buffer)) {
                return 0; 
            }
        }
    }
    return 1; 
}

FloppyParameters detect_floppy_type(uint8_t drive) {
    FloppyParameters result = {0}; 
    uint8_t buffer[512];           
    
    for (int i = 0; i < NUM_CONFIGS; i++) {
        const FloppyParameters* config = &floppy_configs[i];
        
        if (try_read_sector(drive, config, buffer)) {
            
            result.sides = config->sides;
            result.tracks = config->tracks;
            result.sectors_per_track = config->sectors_per_track;
            result.bytes_per_sector = config->bytes_per_sector;
            return result;
        }
    }
    
    return result;
}
// Функция для чтения сектора
int device_read(char type, uint8_t device, uint32_t lba, void* buffer) {
    if (type == DEVICE_TYPE_HDD) {
        hdd_read_sector(device, lba, buffer);
    } else if (type == DEVICE_TYPE_FDD) {
       
        FloppyParameters params = detect_floppy_type(device);
        if (params.sides == 0 || params.tracks == 0 || params.sectors_per_track == 0 || params.bytes_per_sector == 0) {
            return 0;
        }
        uint8_t head = (lba / params.sectors_per_track) % params.sides;
        uint8_t track = (lba / params.sectors_per_track) / params.sides;
        uint8_t sector = (lba % params.sectors_per_track) + 1;
        floppy_read_sector(device, track, head, sector, buffer);
    }
    return 0;
}
// Функция для записи сектора
int device_write(char type, uint8_t device, uint32_t lba, const void* buffer) {
    if (type == DEVICE_TYPE_HDD) {
       
        hdd_write_sector(device, lba, buffer);
    } else if (type == DEVICE_TYPE_FDD) {
       
        FloppyParameters params = detect_floppy_type(device);
        if (params.sides == 0 || params.tracks == 0 || params.sectors_per_track == 0 || params.bytes_per_sector == 0) {
            return 0;
        }
        uint8_t head = (lba / params.sectors_per_track) % params.sides;
        uint8_t track = (lba / params.sectors_per_track) / params.sides;
        uint8_t sector = (lba % params.sectors_per_track) + 1;
        floppy_write_sector(device, track, head, sector, buffer);
    }
    return 0;
}

char floppy=0;

void floppy_init() {
    port_byte_out(FLOPPY_DOR, 0x0C);
	print(font, "Init Floppy\n", 0xf, 2);
	delay(1000);
	floppy=1;
    if(dbgm)print(font,"DATA;",0x7,DRAW_FG);
    port_byte_out(FLOPPY_DOR, 0x0C | 0x04);
    port_byte_out(FLOPPY_DATA, 0x3);
    port_byte_out(FLOPPY_DATA, 0x02);
    port_byte_out(FLOPPY_DATA, 0x00);
}
void floppy_seek(uint8_t drive, uint8_t track) {
    while ((port_byte_in(FLOPPY_MSR) & 0x10) != 0) {
    }
        port_byte_out(FLOPPY_DATA, 0x70);
        port_byte_out(FLOPPY_DATA, drive << 2);
        port_byte_out(FLOPPY_DATA, track);
    while ((port_byte_in(FLOPPY_MSR) & 0x80) == 0) {
    }
}
// void floppy_read_sector(uint8_t drive, uint8_t track, uint8_t head, uint8_t sector, void* buffer) {
//     uint8_t* buf = (uint8_t*)buffer;
//     if(dbgm)print(font,"MSR;",0x7,DRAW_FG);
//     while ((port_byte_in(FLOPPY_MSR) & 0x10) != 0) {
//         //if(dbgm)print(font,".",0x7,DRAW_FG);
//     }
//     if(dbgm)print(font,"DATA;",0x7,DRAW_FG);
//     port_byte_out(FLOPPY_DATA, 0x20); // Команда чтения
//     port_byte_out(FLOPPY_DATA, 0x02); // Размер сектора
//     port_byte_out(FLOPPY_DATA, sector);
//     port_byte_out(FLOPPY_DATA, track);
//     port_byte_out(FLOPPY_DATA, head);
//     port_byte_out(FLOPPY_DATA, 0x1B);
//     port_byte_out(FLOPPY_DATA, 0x00);
//     port_byte_out(FLOPPY_DATA, 0x00);
//     if(dbgm)print(font,"DEL;",0x7,DRAW_FG);
//     delay(100);
//     if(dbgm)print(font,"MSR;",0x7,DRAW_FG);
//     while ((port_byte_in(FLOPPY_MSR) & 0x80) == 0) {
//         //if(dbgm)print(font,"/",0x7,DRAW_FG);
//     }
//     if(dbgm)print(font,"OK;",0x7,DRAW_FG);
//     for (int i = 0; i < 512; i++) {
//         buf[i] = port_byte_in(FLOPPY_DATA);
//     }
// }

char wait_dma(){
    return (inb(0x0a)&0x04)==0;
}

void setup_dma(uint16_t buffer_addr,uint16_t buffer_size){
    outb(0x3f2,0b00011100);
    outb(0x0a, 0b00000110);
    outb(0x08, 0b00010100);
    outb(0x0b, 0b01010110);
    outb(0x0c, 0         );
    outb(0x0a, 0b00000010);
    outb(0x08, 0b00010000);
    if((inb(0x0a)&0x04)==0){
        if (dbgm) print(font, "The DMA is enabled.", 0xf, DRAW_FG);
    }
    if((inb(0x0a)&0x04)!=0){
        if (dbgm) print(font, "The DMA isn't enabled.", 0x7, DRAW_FG);
    }


    outb(0x08,0x10);
    outb(0x87,(buffer_addr>>16)&0xff);
    outb(0x02,buffer_addr&0xff);
    outb(0x03,(buffer_addr>>8)&0xff);
    outb(0x04,buffer_size&0xff);
    outb(0x05,(buffer_size>>8)&0xff);
    outb(0x0a,inb(0x0a)|0x04);
    outb(0x0b,inb(0x0b)& ~0x04);
    
}

void floppy_read_sector(uint8_t drive, uint8_t track, uint8_t head, uint8_t start_sector, void* buffer) {


    const uint8_t* buf = (const uint8_t*)buffer;
    setup_dma(buf,512);

    //clear(buf,512);

    outb(0x3f7, 0x00);
    delay(100);
    outb(0x3f2, inb(0x3f2)|0x04);
    delay(300);
    int cmds[]={
        0x3f7,0x03,
        0x3f2,0xf2,
        0x3f2,0x00,

        0x3f7,0x04,
        0x3f2,drive,        // drive here

        0x3f7,0x07,
        0x3f2,drive,        // drive here
        0x3f2,track,        // track here

        0x3f7,0x05,
        0x3f2,0x01,
        0x3f2,start_sector, // sector here
        0x3f2,0x01,
        0x3f2,0x02,
        0x3f2,head,         // head here
        0x3f2,track         // track here
    };

    char x[6];
    clear(x,6);
    h2c(buf,x);
    for(int i=0;i<15;i++){
        clear(x,6);
        hex_to_ascii(cmds[i*2],x);
        if (dbgm) print(font, "CMD:", 0x7, DRAW_FG);
        if (dbgm) print(font, x, 0x7, DRAW_FG);
        if (dbgm) print(font, " ", 0x7, DRAW_FG);
        clear(x,6);
        hex_to_ascii(cmds[i*2+1],x);
        if (dbgm) print(font, "VAL:", 0x7, DRAW_FG);
        if (dbgm) print(font, x, 0x7, DRAW_FG);
        if (dbgm) print(font, "\n", 0x7, DRAW_FG);
        
        outb(cmds[i*2],cmds[i*2+1]);
    }
    while(!wait_dma());
    return 1;
    //for(int i=0;i<512;i++){
    //    //while((inb(0x3f4)&0xc0)!=0x80);
    //    if (dbgm) print(font, (buf[i]&255), 0x7, DRAW_FG);
    //}
    if ((inb(0x3f4)&0x80)==0) print(font, "ERROR;", 0xf, DRAW_FG);
    /*
    print(font,"READ;",0x9,DRAW_FG);
    uint16_t num_sectors = 1;
    uint8_t* buf = (uint8_t*)buffer;
    uint16_t bytes_to_read = num_sectors * 512; // Размер одного сектора - 512 байт

    if (dbgm) print(font, "MSR;", 0x7, DRAW_FG);
    while ((port_byte_in(0x3f4)&0x80) != 0x80) {
    // Ждем, пока контроллер не станет готовым
    }


    //uint8_t head = 0;
    //uint8_t drive = 0;
    uint8_t cylinder = 0;

    if (dbgm) print(font, "READ;", 0x7, DRAW_FG);

    port_byte_out(FLOPPY_DATA, 0x20); // Команда чтения с повторными попытками
    port_byte_out(FLOPPY_DATA, 0x02); // Размер сектора (2 = 512 байт)
    port_byte_out(FLOPPY_DATA, start_sector); // Номер сектора (1-18)
    port_byte_out(FLOPPY_DATA, track); // Номер дорожки (0-79)
    port_byte_out(FLOPPY_DATA, head); // Номер головки (0 или 1)
    port_byte_out(FLOPPY_DATA, 0x1B); // Длина заголовка сектора (27 байт)
    port_byte_out(FLOPPY_DATA, 0xFF); // Заполнение
    port_byte_out(FLOPPY_DATA, 0xFF); // Заполнение
    

    if (dbgm) print(font, "DEL;", 0x7, DRAW_FG);
    //delay(100); // Короткая задержка для стаби
    

    if (dbgm) print(font, "MSR;", 0x7, DRAW_FG);
    while ((port_byte_in(0x3f4)&0x80) != 0x80) {
        // Ждем, пока контроллер не станет готовым
    }
    if (dbgm) print(font, "OK;", 0x7, DRAW_FG);

    char x[6];
    clear(x,6);
    hex_to_ascii(bytes_to_read,x);
    if (dbgm) print(font, "TR:", 0x7, DRAW_FG);
    if (dbgm) print(font, x, 0x7, DRAW_FG);
    if (dbgm) print(font, ";", 0x7, DRAW_FG);
    // Читаем данные из порта в буфер
    if (dbgm) print(font, ";", 0x7, DRAW_FG);
    for (int i = 0; i < bytes_to_read; i++) {
        buf[i] = port_byte_in(0x3f5);
        if (dbgm) print(font, ".", 0x7, DRAW_FG);
    }
    for(int i=0;i<10;i++){
        clear(x,6);
        hex_to_ascii(buf[i],x);
        if (dbgm) print(font, x, 0xf, DRAW_FG);
        if (dbgm) print(font, ";", 0xf, DRAW_FG);
    }
        */
}


void floppy_write_sector(uint8_t drive, uint8_t track, uint8_t head,
                         uint8_t sector, const void* buffer) {
    const uint8_t* buf = (const uint8_t*)buffer;
    while ((port_byte_in(FLOPPY_MSR) & 0x10) != 0) {
    }
    port_byte_out(FLOPPY_DATA, 0x30);
    port_byte_out(FLOPPY_DATA, 0x02);
    port_byte_out(FLOPPY_DATA, sector);
    port_byte_out(FLOPPY_DATA, track);
    port_byte_out(FLOPPY_DATA, head);
    port_byte_out(FLOPPY_DATA, 0x1B);
    port_byte_out(FLOPPY_DATA, 0xFF);
    port_byte_out(FLOPPY_DATA, 0xFF);
    for (int i = 0; i < 512; i++) {
        port_byte_out(FLOPPY_DATA, buf[i]);
    }
    while ((port_byte_in(FLOPPY_MSR) & 0x80) == 0) {
    }
}
uint8_t id_hdd(int drive) {
    uint8_t buffer[512];
    int lba = 0, tm = 0;
    while (port_byte_in(ATA_PRIMARY_BASE + ATA_REG_STATUS) & 0x80) {
        if (tm > 10000) {
            if(dbgm)print(font,"HDDTO;",0x7,DRAW_FG);
            return 0;
        }
        tm++;
    }
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_SECTOR_COUNT, 1);
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_LBA_LOW, lba & 0xFF);
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
    uint8_t drive_head = ((lba >> 24) & 0x0F) | (drive << 4) | 0xE0;
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_DEVICE, drive_head);
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);
    while (!(port_byte_in(ATA_PRIMARY_BASE + ATA_REG_STATUS) & 0x8)) {
        if (tm > 10000) {
            if(dbgm)print(font,"HDDTO;",0x7,DRAW_FG);
            return 0;
        }
        tm++;
    }
    for (int i = 0; i < 256; i++) {
        uint16_t word = port_word_in(ATA_PRIMARY_BASE + ATA_REG_DATA);
        buffer[i * 2] = word & 0xFF;
        buffer[i * 2 + 1] = (word >> 8) & 0xFF;
    }
    while (port_byte_in(ATA_PRIMARY_BASE + ATA_REG_STATUS) & 0x80) {
        if (tm > 10000) {
            if(dbgm)print(font,"HDDTO;",0x7,DRAW_FG);
            return 0;
        }
        tm++;
    }
    uint8_t status = port_byte_in(ATA_PRIMARY_BASE + ATA_REG_STATUS);
    if (status & 0x01) {
        uint8_t error = port_byte_in(ATA_PRIMARY_BASE + ATA_REG_ERROR);
        if(dbgm)print(font,"HDDRE;",0x7,DRAW_FG);
        return 0;
    } else {
        if(dbgm)print(font,"HDDOK;",0x7,DRAW_FG);
        return 1;
    }
}
int check_floppy_controller(uint8_t drive) {
    uint8_t status = port_byte_in(FLOPPY_MSR);
    if ((status & 0x80) == 0) {
        return 0; // Контроллер не готов или отсутствует
    }
    return 1;
}
uint8_t check_device_type(uint8_t drive) {
    if(dbgm)print(font,"CDT;",0x7,DRAW_FG);
    if (id_hdd(drive)) {
        if(dbgm)print(font,"HDD;",0x7,DRAW_FG);
        return 0;
    }
    if (check_floppy_controller(drive)) {
        if(dbgm)print(font,"FLP;",0x7,DRAW_FG);
        return 1;
    }
    if(dbgm)print(font,"UNK;",0x7,DRAW_FG);
    return 0xff;
}

void toggle_dbg(){
	dbgm^=1;
}
