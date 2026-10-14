#include <../libc/mem.h>

extern uint8_t ahci_read_sector(uint32_t lba, char* buffer);

char read_sector(char drive, int lba, unsigned char *buffer){
	return !ahci_read_sector(lba, buffer);
}

typedef struct __attribute__((packed)){
    char sign[4]; // ALWAYS "MCFS"!!!
    char name[16]; // disk name
    uint32_t size;  // in sectors
} MCFSDI; // McFS Disk Instance 

typedef struct __attribute__((packed)){
    char type; // 0x80 file
    char attributes; // ????
    char name[16]; // file name
    uint32_t size; // in sectors
} MCFSFI; // McFS File Instance

char drive = 0xa0;
uint64_t drive_size = 0;
uint64_t vd_offset = 0;
char *buffer = (char*)0x600;

EXTENDED_KERNEL int vd_init(MCFSDI *out_drive, char in_drive){

    drive_size = 0xffffff;
    drive = in_drive;

    for(int i=0; i<drive_size; i++){
        read_sector(in_drive, i, buffer);   
        if(strncmp(buffer,"MCFS",4)==0){
            memcpy(out_drive,buffer,sizeof(MCFSDI)+1);
            vd_offset = i+1;
            return 0;
        }
        for(int i=0;i<100000;i++)asm("nop");
    }
    return 1;
}

// ⬛4171

EXTENDED_KERNEL int vd_find_file(MCFSDI *in_disk, MCFSFI *out_file, char name[16]){
    for(int i=vd_offset; i<in_disk->size; i++){

        read_sector(drive, i, buffer);
        MCFSFI* file = (MCFSFI*)buffer;

        if( (file->type&0xff) == 0x80 ){

            if (strcmp(file->name,name)==0){
                memcpy(out_file, file, sizeof(MCFSFI));
                return 0;
            } else {
                i+=file->size-1;
            }
            
        }
        for(int i=0;i<100000;i++)asm("nop");

    }
    return 1;
}

EXTENDED_KERNEL int vd_get_files(MCFSDI *in_disk){
    int count = 0;

    for(int i=vd_offset; i<in_disk->size; i++){

        read_sector(drive, i, buffer);
        MCFSFI* file = (MCFSFI*)buffer;

        if( (file->type&0xff) == 0x80 ){
            count++;
            i+=file->size;
        }
        for(int i=0;i<100000;i++)asm("nop");

    }
    return count;
}

EXTENDED_KERNEL int vd_get_file_by_idx(MCFSDI *in_disk, MCFSFI *out_file, int idx){
    int count = 0;
    for(int i=vd_offset; i<in_disk->size; i++){

        read_sector(drive, i, buffer);
        MCFSFI* file = (MCFSFI*)buffer;

        if( (file->type&0xff) == 0x80 ){
            if(idx==count){
                memcpy(out_file, file, sizeof(MCFSFI));
                return count;
            }
            count++;
            i+=file->size-1;
        }
        for(int i=0;i<100000;i++)asm("nop");

    }
    return count;
}

EXTENDED_KERNEL int vd_read_file(MCFSDI *in_disk, char name[16], char* output){
    MCFSFI* file;
    int sector = 0;
    for(int i=vd_offset; i<in_disk->size; i++){
        read_sector(drive, i, buffer);
        file = (MCFSFI*)buffer;

        if( (file->type&0xff) == 0x80 ){

            if (strcmp(file->name,name)==0){
            	sector = i+1;
                goto found;
            } else {
                i+=file->size-1;
            }
            
        }
        for(int i=0;i<100000;i++)asm("nop");
    }	
    return 1;
found:	
	for(int i = 0; i<file->size; i++){
		read_sector(drive, sector+i, output+i*512);
        for(int i=0;i<300000;i++)asm("nop");
	}
	return 0;
}
