#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#include <phtstd.h>

typedef struct {
	char line[60];
	char separator;
} file;

/*

    ProstoDOS for PhotonOS

*/

//#define printf( PRN(0xf,2,

char greet[] = "ProstoShell, default command line for PhotonOS. Coperight 2025\n";

int main(){
    int80(4,0xf,2,(uint32_t)"\nWelcome to \0");
    int80(4,0xb,2,(uint32_t)"PhotonOS!\n\n\0");
    int80(4,0xb,2,greet);
    char buf[32];
    struct dir_entry entries[MAX_DIR_ENTRIES];
    //PRT(0xf,2,"%d",getch());
    while (1){
        print("~");
        input(buf,32);
        print("\n");
        //print("1");
        if(strcmp(buf,"quit")==0){
            break;
        } else if (strcmp(buf, "dir")==0 || strcmp(buf, "ls")==0 ){
            int count = listdir(entries, MAX_DIR_ENTRIES);
            if (count > 0) {
                PRT(0x7,2,"Directory contents (%d entries):\n", count);

                for(int i = 0; i < count; i++) {
                    PRT(0x7,2,"  '%s' %db %ds\n", 
                           entries[i].name,
                           entries[i].size,
                           entries[i].sectors);
                }
            } else {
                PRT(0x7,2,"No files\n");
            }
        } else if (strcmp(buf, "ed")==0){
        	buf[0]=0xff;
        	uint16_t line_counter;
        	file f[80];
			char rfile[512*8];
			char filename[16] = "textfile";
        	while(1){
				if(buf[0]=='x'){
					break;
				} else if (buf[0]==' ') { // write to memory
					PRT(0xf,2," * %d",line_counter+1);
					memcpy(buf+2,f[line_counter].line,60);
					line_counter++;
				} else if (buf[0]=='h') { // write to memory
					PRT(0xf,2,"\n[h]elp [f]ile [w]rite [q]uery e[x]it");
				} else if (buf[0]=='f') { // select file
					PRT(0xf,2," s");
					memcpy(buf+2,filename,60);
				} else if (buf[0]=='w') { // write file
					PRT(0xf,2," <");
					writefile(filename,rfile,512*8);
					PRT(0x9,2,"<");
				} else if (buf[0]=='q') { // query
					PRT(0xf,2,"\n %s %d", filename, line_counter);
				} else {
		        	PRT(0xf,2," ?");
				}
	        	PRT(0xf,2,"\n");
	        	PRT(0xf,2,"<");
	        	wtc();
				input(buf,60);
        	}
    	} else if (strcmp(buf, "help")==0){
            PRT(0x7,2,"dir help quit ed\n");
        } else if (strcmp(buf, "")!=0) { // execute / show
            int file_handle = open(buf)&0xff;
            if (file_handle!=0xff) {
                PRT(0x7,2,"Execute/Type/View >");
                char c=getch();
                PRT(0x7,2,"%c",c);
                if(c=='t'){
                    print("T\n");
                    char *buffer=0x200000;
                    int bytes_read = read(file_handle, buffer,0xfff);
                    if (bytes_read > 0) {
                        //buffer[bytes_read] = '\0';
                        int80(4, 0xf, 2, (uint32_t)buffer);
                    }
                } else if(c=='vE'){
                    print("E\n");
                    wtc();
                    char *buffer=0x200000;
                    int bytes_read = read(file_handle, buffer,0xffffff);
                    int(*func)() = (int(*)())buffer;
                    int a=func();
                    PRT(0x7,2,"\nreturn value %d\n",a);
                } else if(c=='v'){
                    char *buffer=0x200000;
                    int bytes_read = read(file_handle, buffer,0xffffff);
                    uint8_t *image_data = 0x200000;
                    uint16_t height = ((uint16_t)image_data[0] << 8) | image_data[1];
                    uint16_t width = ((uint16_t)image_data[2] << 8) | image_data[3];
                    uint8_t *pixels = image_data + 4;
                    cls(0x0);
                    for(int y=0;y<200;y++){
                        for(int x=0;x<320;x++){
                            swrite(x,y,((x>>4)^(y>>4))&1?0x12:0x13);
                        }
                    }
                    for(int y=0;y<height;y++){
                        for(int x=0;x<width;x++){
                            swrite(x+160-width/2,y+100-height/2,pixels[x+y*(width)]);
                        }
                    }
                    con_setp(0,32);
                    PRT(0x7, 3, "'%s', %d x %d (%ds or %dkb)\n", buf, width, height,width*height/0x200,width*height/1024);
                    wtc();
                    wtc();
                    cls(0x0);
                }
                seek(file_handle, 0, 0);
                close(file_handle);
            } else {
                print("No such file or command\n");
            }
        } else {
        }
    }
    return 0;
}
