/*
The following .c & .h files are already imported:

/drivers/    
/drivers/       screen.h
/drivers/       disk.h

/libc/          string.h
/libc/          stdint
/libc/          mem.h

/kernel/        graphics.h
/kernel/        kernel.h
/kernel/        fnt.h
/kernel/klib/   kgr.h

/disk/          disk.c

/cpu/syscall/   base.h
/cpu/           isr.h

*/
#include <../../cpu/timer.h>
#include <../../drivers/fs.h>




int main(){

    //
    volatile uint8_t* video_memory = (uint8_t*)0xA0000; // ????? ??????????? ??? ??????????? ???????
    //for (int i = 0; i < 1024 * 768; i++) {
    ///    video_memory[i] = 0xff;
    //}
    //volatile header[480*320]; // TODO: FILL DATA
    setspacing(8,8);
    cls(0x0);
    char bitmap[] = {
        0b10001110,
        0b00011001,
        0b00100101,
        0b01000011,
        0b01000010,
        0b10100100,
        0b10011000,
        0b01100001,
    };
    char disk_type = 'N';
    //could be N(one), H(ard drive),  F(loppy)

    char m[100];
    char user[7]="PHT*OS";
    char mode[7]="KERNEL";
    char os_drive;

    for(int y=0;y<8;y++){
        for(int x=0;x<8;x++){
            if(((bitmap[y]>>x)&1))rect(x*2,y*2,2,2,0xf);
        }
    }
    print(font,"   WELCOME TO PHOTON OS!\n",0xf,DRAW_FG|DRAW_THICK);
    print(font,"   BUILT FOR EDUCATIONAL PURPOSES\n",0x7,DRAW_FG|DRAW_THICK);
    print(font,"\n",0xf,DRAW_FG|DRAW_THICK);
    char rbuf[512] = "You gonna get so much sex!";
    uint8_t checksum = 0;
    uint8_t type;
    char sussy[8];
    clear(sussy,8);


    /*

    if(check_floppy_controller(0)){
        print(font,"Your PC supports FDD controller.\n",0xf,DRAW_FG);
        for(uint16_t drive_n=0;drive_n<0xff;drive_n++){
            clear(sussy,8);
            hex_to_ascii(drive_n,sussy);
            print(font,"DE",0xf,DRAW_FG);
            print(font,sussy,0xf,DRAW_FG);
            print(font,";",0xf,DRAW_FG);
            check_device_type(drive_n);
            type = check_device_type(drive_n);
            print(font,(type==0?"H":"F"),0xf,DRAW_FG);
            print(font,"\n",0xf,DRAW_FG);
            if(type!=0xff){
                disk_type=(type==0?'H':'F');
                if(disk_type=='F'){
                    floppy_init();
                }
                break;
            }
        }
    } else {
        print(font,"Your PC DOESN'T support FDD.\n",0x8,DRAW_FG);
        for(uint16_t drive_n=0;drive_n<0xff;drive_n++){
            clear(sussy,8);
            hex_to_ascii(drive_n,sussy);
            print(font,"DE",0xf,DRAW_FG);
            print(font,sussy,0xf,DRAW_FG);
            print(font,";",0xf,DRAW_FG);
            check_device_type(drive_n);
            type = check_device_type(drive_n);
            print(font,(type==0?"H":"F"),0xf,DRAW_FG);
            print(font,"\n",0xf,DRAW_FG);
            if(type!=0xff){
                disk_type='H';
            }
        }
    }

    for(uint8_t drive_n = 0; drive_n < 0xff; drive_n++) {
        clear(m, 100);
        clear(rbuf, 512);
        clear(sussy, 8);
        readSector(drive_n,0,rbuf);
        checksum = ((rbuf[0x1e4]) & 255) | ((rbuf[0x1e5]) & 255) ^ ((rbuf[0x1e6]) & 255);
        print(font,"read ",0xf,DRAW_FG);
        for(int i=0;i<8;i++){
            hex_to_ascii(rbuf[i]&255, sussy);
            print(font,sussy,0xf,DRAW_FG);
            print(font,", ",0xf,DRAW_FG);
            clear(sussy, 8);
        }
        print(font,"\n",0xf,DRAW_FG);
        hex_to_ascii(rbuf[0]&255, sussy);
        type = check_device_type(drive_n);
        if((rbuf[0]&255)==0x80){
            cls(0x0);
            print(font, "                                        ", 0x0003, DRAW_FG);
            print(font, "========== KERNEL LOAD ERROR! ==========\n", 6, DRAW_FG);
            print(font, "The FDD (which you probably have) is    ", 6, DRAW_FG);
            print(font, "unscannable. Please try to reboot OS.   \n", 6, DRAW_FG);
            print(font, "========== KERNEL LOAD ERROR! ==========", 6, DRAW_FG);
            print(font, "                                        ", 0x0003, DRAW_FG);
            for(int i=0;i<10;i++){
                setcursorpos(0,8*8);
                print(font, "Reboot in ", 0x7, DRAW_FG);
                clear(sussy,8);
                int_to_ascii(10-i, sussy);
                print(font,sussy,0xf,DRAW_FG|DRAW_BG);
                print(font, " ", 0xf, DRAW_FG|DRAW_BG);
                delay(1000);
            }
            port_byte_out(0xCF9, 0xE);
        }
        if(checksum == (0x88 ^ 0x11)) { 
            clear(sussy, 8);
            hex_to_ascii(drive_n, sussy);
            print(font, "Disk ", 0xf, DRAW_FG);
            print(font, sussy, 0xf, DRAW_FG);
            print(font, " is a PhotonOS ", 0xf, DRAW_FG);
    
            uint8_t type = check_device_type(drive_n);
            if(type == 0){
                print(font, "on HDD\n", 0xf, DRAW_FG);
            }
            if(type == 1){
                print(font, "on FDD\n", 0xf, DRAW_FG);
            }
            if(type == 0xff){
                print(font, "as Unknown Drive\n", 0xf, DRAW_FG);
            }
            break;
        }
    }
    print(font, "Initializing root...\n", 0xf, DRAW_FG);
    create_file_or_dir("/", 0, 0);
    clear(sussy, 8);
    int_to_ascii(allocate_block(), sussy);
    print(font, sussy, 0xf, DRAW_FG);
    print(font, " blocks are available.\n", 0xf, DRAW_FG);
    
            /*
        char sussy[8];
        clear(sussy,8);
        hex_to_ascii(drive_n,sussy);
        print(font,".cdt ",0xf,DRAW_FG);
        print(font,sussy,0xf,DRAW_FG);
        uint8_t type = check_device_type(drive_n);
        print(font," check\n",0xf,DRAW_FG);
        if(type!=0xff){
            print(font,"Disk ",0xf,DRAW_FG);
            print(font,sussy,0xf,DRAW_FG);
            print(font," is on ",0xf,DRAW_FG);
            if(type==0){
                print(font,"HDD\n",0xf,DRAW_FG);
            }
            if(type==1){
                print(font,"FDD\n",0xf,DRAW_FG);
            }
            break; 
        } else {
            print(font,"Disk ",0xf,DRAW_FG);
            print(font,sussy,0xf,DRAW_FG);
            print(font," have failed\n",0xf,DRAW_FG);
        }* /
    for(int i=0;i<10;i++){
        print(font,"\n",0xf,DRAW_FG|DRAW_THICK);
    }

    read_sector(os_drive,0,rbuf);
    strncpy(0x7c00,rbuf,512);
    for(int i=0;i<0x100;i++){
        read_sector(os_drive,1+i,rbuf);
        strncpy(0x1000+i*512,rbuf,512);
    }
    print(font,"   Running off 32 bit mode. Currently\n",0xf,DRAW_FG);
    print(font,"   booted up as ",0xf,DRAW_FG);
    print(font,user,0xf,DRAW_FG|DRAW_THICK);
    print(font,"@",0xf,DRAW_FG|DRAW_THICK);
    print(font,mode,0xf,DRAW_FG|DRAW_THICK);
    print(font,".\n\n",0xf,DRAW_FG|DRAW_THICK);
     */
    uint8_t root_dir, tempinit, ti2;
    root_dir=create_file_or_dir("/", 1, 0);
tempinit= create_file_or_dir("user", 1, root_dir);
              create_file_or_dir("phtu", 1, tempinit);
          create_file_or_dir("bin", 1, root_dir);
          create_file_or_dir("lib", 1, root_dir);
          create_file_or_dir("kernel", 1, root_dir);

    print(font,"........................................\nPress any key to continue.\n",0xf,DRAW_FG);
    
    wtc();

    while(1){
        print(font,user,0x7,DRAW_FG);
        print(font,"@",0xf,DRAW_FG);
        print(font,mode,0x7,DRAW_FG);
        print(font,">",0xf,DRAW_FG);
        wtc();
        getline(m);
        print(font,"\n",0x0,DRAW_FG);
        char cmd[20];
        split(m,cmd,' ',1);
        if( strcmp(cmd,"reload")==0 ){
            read_sector(os_drive,0,rbuf);
            strncpy(0x7c00,rbuf,512);
            for(int i=0;i<0x100;i++){
                read_sector(os_drive,1+i,rbuf);
                strncpy(0x1000+i*512,rbuf,512);
            }
        } else if( strcmp(cmd,"mkd")==0 ){ 
            char tk[16];
            split(m,tk,' ',0);
            create_file_or_dir(tk, 0, 0);
            
        }else if( strcmp(cmd,"ls")==0 ){
            char tk[16];
            char name[16];
            split(m,tk,' ',0);
            int idx=atoi(tk);
            int i=1,id=0,type=0;
            int result_or_st=0;
            clear(name,16);
            ls_n(idx, 0,name);
            print(font,"Contents of directory ",0xf,DRAW_FG);
            print(font,name,0xf,DRAW_FG);
            print(font,":\n ",0xf,DRAW_FG);
            print(font,"\n",0xf,DRAW_FG);
                    //  0                136       216
            print(font,"FILENAME.........ID........TYPE.........\n",0x7,DRAW_FG);
            
            result_or_st=ls(idx, i+2);
            while(result_or_st!=ERROR_OUT_OF_RANGE){
                result_or_st=ls(idx, i);
                clear(name,16);
                id=ls(idx,i+0);
                type=ls(idx,i+1);
                ls_n(idx, i+2, name);
                
                if(id==-3)break;
                if(i==1 && id==-3){
                    print(font,"The folder is empty.\n",0xf,DRAW_FG);
                }
                setcursorx(0);
                print(font,name,0xf,DRAW_FG);
                setcursorx(5*17);
                print_int(font,id,0xf,DRAW_FG);
                setcursorx(5*27);
                print(font,type?"folder":"file",0xf,DRAW_FG);
                print(font,"\n",0xf,DRAW_FG);

                i+=3;
            }
            print(font,"........................................",0x7,DRAW_FG);
            print(font,"\n",0x7,DRAW_FG);

        }
        else if( strcmp(cmd,"ct")==0 ){
            int i=0;
            int j=0;
            char* c[2];
            split(m,c,' ',0);
            for(i=0;i<16;i++)print(font,c,i,DRAW_FG);print(font,"\n",0xf,DRAW_FG);
            for(j=0;j<3;j++){for(i=0;i<24;i++)print(font,c,d_hsv2c(i,j,2),DRAW_FG);
            print(font,"\n",0xf,DRAW_FG);}
            for(j=1;j<3;j++){for(i=0;i<24;i++)print(font,c,d_hsv2c(i,2,2-j),DRAW_FG);
                print(font,"\n",0xf,DRAW_FG);}
        } else if( strcmp(cmd,"reboot")==0 )  {
            port_byte_out(0xCF9, 0xE);
        } else if( strcmp(cmd,"cls")==0 ){ 
            cls(0x0);
        } else if( strcmp(cmd,"lds")==0 )  {
            print(font,"bootsect...\n", 0x7, DRAW_FG);
            //readSector(os_drive,0,rbuf);
            //strncpy(0x7c00,rbuf,512);
            for(int i=0;i<0x100;i++){
                // clear(sussy,8);
                // hex_to_ascii(i,sussy);
                readSector(os_drive,1+i,rbuf);
                print(font,"sector ", 0x7, DRAW_FG);
                print_int(font,i, 0x7, DRAW_FG);
                print(font,"...\n", 0x7, DRAW_FG);
                strncpy(0x1000+i*512,rbuf,512);
            }
        } else if( strcmp(cmd,"ddbg")==0 )  {
            print(font,"Toggled Drive Debug\n",0xf,DRAW_FG);
            toggle_dbg();
        } else if( strcmp(cmd,"256")==0 )  {
            cls(0x0);
            int xx,yy;
            
            for(
                int i=0;
                i<256;
                i++
            ){
                char* ar = {'\0'};
                if(i=='\n')i='.';
                ar[0]=i;
                print_at(font,ar,xx,yy,0xf,DRAW_FG|DRAW_BG);
                if(i%32==0){
                    xx=0;
                    yy+=8;
                } else {
                    xx+=8;
                };
            }
            setcursorpos(0,9*8);
            print(font,"This is a character test\n",0xf,DRAW_FG);
            print(font,"I'm troubleshooting TS because\n",0xf,DRAW_FG);
            print(font,"somehow something doesnt import more characters. 3:<\n",0xf,DRAW_FG);
        } else if( strcmp(cmd,"fuck")==0 )  {
            print(font,"really? (O///O )\n",0xf,DRAW_FG);
        } else if( strcmp(cmd,"display")==0 )  {
            set_mode_1024x768();
            set_hvm(1);
        } else if( strcmp(cmd,"ihns")==0 || strcmp(cmd,"IHNS")==0 )  {
            flip_shift();
        } else if( strcmp(cmd,"help")==0 )  {
            print(font,"mkd        ct         reboot      lds\n",0xf,DRAW_FG);
            print(font,"help       ddbg       cls         ls\n",0xf,DRAW_FG);
            print(font,"display    ihns\n",0xf,DRAW_FG);
        }  else if( strcmp(cmd,"info")==0 )  {
            print(font,"    ._. .        .          ._. ._. \n",0xf,DRAW_FG);
            print(font,"    |_| |_  ._. _|_ ._. __  | | |_. \n",0xf,DRAW_FG);
            print(font,"    |   | | |_|  |_ |_| | | |_| ._| \n",0xf,DRAW_FG);
            print(font,"      indev since 2024           \n",0xf,DRAW_FG);
            print(font," \n",0xf,DRAW_FG);
        }  
    











        else if (m[0]!='\0'){
            print(font,"kernel: ",0xf,DRAW_FG);
            print(font,cmd,0xf,DRAW_FG);
            print(font,": unknown command\n",0xf,DRAW_FG);
        }
        

        
        
        
    }
    return 0;
}