#include <stdint.h>
#include "memory_check.h"
#include <macros/macros.h>
#ifdef __PHT_AMPHIBIOUS__
	#include <LLGA/fnt.h>
	#include <kgraphics.h>
#endif

#include "loc/loc.h"
#include <cpu/mt.h>
#include <cpu/isr.h>
#include <cpu/malloc/kmalloc.h>
#include <buildinfo.h>
#include <setup.h>
#include <kernel/int80.h>
#include <kernel/files.h>
// #include <kernel/microfs.h>
#include <drivers/pci.h>
#include <drivers/blockdev.h>
#include <kernel/pex/header.h>
#include <kernel/pex/loader.c> // <-------- kill this
#include <drivers/vgarom.h>
#include <kernel/datslaf/fs.h>
#include <kernel/debug.h>
#include <cpu/mmio.h>
#include <cpu/vmm.h>

void task(void* arg) { 
    // Приводим указатель к нужному типу
    task_context_info_t* info = (task_context_info_t*)arg;
    int my_pid = info->pid;

	while(1);
    for(int j=0; j<15; j++){
		comprintf("I am task with PID: %d (step %d)\n",my_pid,j);
        
        for(volatile int i=0; i<500000; i++); 
    }
}

// void taskbar(void* arg){
// 	uint32_t x,y;
// 	uint32_t t = 0, mt=0, a=0;
//     uint32_t malloc_pool_size = __PHT_MEM_MALLOC_END - __PHT_MEM_MALLOC_POOL;
// //            
// 	char text1[] = "ALLOC POOL                             ";
// 	char text2[] = "THREAD POOL                            ";

// 	while(1){
// 		t = get_ticks();
// 		mt = mt_get_tasks();
// 		a = kmallocated();
// 		asm volatile("cli");
// 		getcursorpos(&x,&y);

// 		setcursorpos(0,0);
// 		for(int i=0;i<80;i++)kprintf(0x0,0x0f00,6," ");
// 		setcursorpos(0,-1);
// 		kprintf(0x0,0x0f00,6," %2d:%2d [",t/60000,t/1000%60);

// 		if(t/5000%2==0){
// 			//	show memory
// 			for(int i=0;i<40;i++){
// 				if(a/(float)malloc_pool_size*40.>i)kprintf(0x0,0x070f,6,"%c",text1[i]);
// 				else kprintf(0x0,0x0f07,6,"%c",text1[i]);
// 			}
// 			kprintf(0x0,0x0f00,6,"] %dkb %d%%",a/1024,(int)((a/(float)malloc_pool_size)*100));
// 		} else {
// 			for(int i=0;i<40;i++){
// 				if(mt/(float)MAX_TASKS*40.>i)kprintf(0x0,0x070f,6,"%c",text2[i]);
// 				else kprintf(0x0,0x0f07,6,"%c",text2[i]);
// 			}
// 			kprintf(0x0,0x0f00,6,"] %dthr %d%%",mt,(int)((mt/(float)MAX_TASKS)*100));

// 		}

// 		setcursorpos(x,y);
// 		asm volatile("sti");
// 		for(volatile int i=0;i<1000000;i++);
// 	}
// }

// pseudouser, replace this later with a real user space program
char runas[] = "kernel";

#define EQ(a, b) strcmp(a, b) == 0

void main(void *arg);

// Границы .bss из init.ld
extern char __bss_start[];
extern char __bss_end[];

void __attribute__((section(".text.main"))) init(){
	// Загрузчик кладёт в память только init.bin (.text + .data); секцию .bss
	// никто не обнуляет, и на реальном железе статические переменные стартовали
	// бы с мусором. Делаем это первым делом, до любого обращения к ним.
	memory_set(__bss_start, 0, (uint32_t)(__bss_end - __bss_start));

	kprintd("Load OK\n");
    cls(0x000f);
    // Let user know that we are here

    setspacing(4,6);

    // Setup base IRQ/ISR
    asm("cli;");
    isr_install();
    irq_install();
	init_keyboard();
    asm("sti;");

	mt_add_task(main);
	while(1);
}

void zombies() {
	for(int i=0;i<5;i++)kprintd("taskz: %d\n",mt_add_task(task));
}

void main(void *arg){
    // 1. ПЕРВЫМ ДЕЛОМ инициализируем paging!
    kprintd("Initializing paging...\n");
    init_paging();	

    // 2. И только ПОТОМ ищем диски (AHCI / IDE / ATAPI / ramdisk) и выбираем
    //    тот, с которого нас загрузили (см. drivers/blockdev.c)
    kprintd("Initializing block devices...\n");
    if (blockdev_init_boot() != 0) {
        kprinte("No boot device: filesystem will be unavailable\n");
    }

    char line[64]={};
    char command[64] = {};
    int code;

    kprintd("Reading stored filesystem...\n");

    Dnode* ramfs_root = kmalloc(sizeof(Dnode));
    fs_load_from_disk(&ramfs_root);
    if (ramfs_root && ramfs_root->children)
        kprintd("Filesystem (%x) loaded, root has %d children\n", ramfs_root, ramfs_root->children->size);
    else
        kprinte("Filesystem not loaded (see messages above, try 'lsblk')\n");

#ifdef __PHT_TEXTMODE__
    kprintw(LOC_KERNEL_NO_GRAPHICS);
#endif

    asm("sti;");

    kfinit(ramfs_root);

    char* init = (char*)0;

    memory_validate();
	
    ksym_load();

    ksetiocolor(0x0,0xf,0x0,0x7);

    Dnode* current_node;
    Dnode* current_node_if_fail;
    Dnode* sys;

    // (УДАЛИТЕ отсюда повторный вызов init_paging())

    sys = DFC(ramfs_root,"sys");
    if (sys==0) {
        kprinte("No /sys directory found, are you sure the system is OK?\n");
    }	

	if (ramfs_root==0){
		kprinte("The root is not present\n");
	}

	current_node = DFC(ramfs_root,"home");

	if(current_node==0){
		kprinti("There seems to be a problem with FS, because:\n");
		kprinti("* ramfs_root = %x \n",ramfs_root);
		kprinti("* current_node = %x \n",current_node);
		kprinti("* sys = %x \n",sys);
	}

	char* path=kmalloc(256);

	char* init_str = "/bin/init";
	
	uint32_t child_pid = pex_exec("/bin/init",1,&init_str);

	sys_waitpid(child_pid, 0);


	 
	kprintf(0x0,0xf,2,"\033[97mi\033[90mntegrated \033[97mk\033[90mernel \033[97msh\033[90mell\033[97m, built-in within the kernel version %d.%d.%d\n",__PHT_KV_HIGH__,__PHT_KV_MID__,__PHT_KV_LOW__);
	
	uint8_t b9=1;

	while(1){
        clear(path,256);
        dnode_get_path(current_node,path);
        kprintf(font,0x00f,2,"\033[37m%s@%s> ",runas,path);
        
        char line[128] = {0};
        code = kread(STDIN, line, 127);
        kprintf(font,0x0007,2,"\n");

        if(code == ERR_SIZE_OVERFLOW) continue;

        // ПАРСЕР АРГУМЕНТОВ ЯДРА
        char* argv[32];
        int argc = 0;
        char* ptr = line;
        
        while (*ptr && argc < 31) {
            while (*ptr == ' ' || *ptr == '\t') ptr++;
            if (!*ptr) break;

            if (*ptr == '"') {
                ptr++;
                argv[argc++] = ptr;
                while (*ptr && *ptr != '"') ptr++;
                if (*ptr == '"') { *ptr = '\0'; ptr++; }
            } else {
                argv[argc++] = ptr;
                while (*ptr && *ptr != ' ' && *ptr != '\t') ptr++;
                if (*ptr != '\0') { *ptr = '\0'; ptr++; }
            }
        }
        argv[argc] = 0;

        if (argc == 0) continue;
        char* command = argv[0];

        // Исполнение PEX файлов
        if(command[0] == '.'){
            char* program = command + 1; // Убираем точку
            argv[0] = program; // Имя программы в argv[0] должно быть без точки
            uint32_t child_pid = pex_exec(program, argc, argv);
            if (child_pid > 0) {
                sys_waitpid(child_pid, 0);
            }
        }
		else if(EQ(command,"help")){
			split(line,command,' ',2);
			if(EQ(command,"list")||EQ(command,"ls")||EQ(command,"dir")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_LIST);
			}
			else if(EQ(command,"load")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_LOAD);
			}
			else if(EQ(command,"run")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_RUN);
			}
			else if(EQ(command,"auto")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_AUTO);
			}
			else if(EQ(command,"lspci")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_LSPCI);
			}
			else if(EQ(command,"lsblk")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_LSBLK);
			}
			else if(EQ(command,"charmap")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_CHARMAP);
			}
			else if(EQ(command,"lst")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_LST);
			}
			else if(EQ(command,"zombies")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_ZOMBIES);
			}
			else if(EQ(command,"cd")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_CD);
			}
			else if(EQ(command,"mkdir")||EQ(command,"mkd")||EQ(command,"mk")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_MK);
			}
			else if(EQ(command,"touch")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_TOUCH);
			}
			else if(EQ(command,"bit9")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_BIT9);
			}
			else if(EQ(command,"memleak")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_MEMLEAK);
			}
			else if(EQ(command,"loadfont")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_LOADFONT);
			}
			else if(EQ(command,"exit")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_EXIT);
			}
			else if(EQ(command,"reboot")){
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_REBOOT);
			}
			else if (EQ(command, "cat") || EQ(command, "type")) {
				kprintf(font, 0x0007, 2, LOC_KERNEL_CMD_TYPE);
			}
			else if(EQ(command,"klist")||EQ(command,"kver")||EQ(command,"kernels")){
				kprintf(font,0x0007,2,"KLIST: list kernels in /sys/kernel (supports versioning)\n");
			}
			else if(EQ(command,"kset")){
				kprintf(font,0x0007,2,"KSET <file>: set default kernel to boot (writes /sys/kernel/boot.cfg)\n");
			}
			else if(EQ(command,"kinstall")){
				kprintf(font,0x0007,2,"KINSTALL <src> <dest>: install kernel file to /sys/kernel/\n");
			}
			else if(EQ(command,"kinfo")){
				kprintf(font,0x0007,2,"KINFO: show kernel version and /sys/kernel info\n");
			}
			else {
				kprintf(font,0x0007,2,LOC_KERNEL_CMD_HELP);
				kprintf(font,0x0007,2,"Additional: klist kset kinstall kinfo\n");
			}
		}
		else if (EQ(command, "list") || EQ(command, "ls") || EQ(command, "dir")) {

			Dnode* working_node = NULL;

			// Если аргумент пустой — используем текущую папку, иначе ищем по пути
			if (argc == 1) {
				working_node = current_node;
			} else {
				working_node = dnode_find_by_path(ramfs_root, current_node, argv[1]);
			}

			// 1. Проверка на NULL (защита от Page Fault)
			if (!working_node) {
				kprinte("ls: path not found\n");
				continue; // или return
			}

			// 2. Если это файл, а не директория — просто выводим имя файла
			if (working_node->type == TYPE_FILE) {
				kprintf("%s\n", working_node->name);
				continue;
			}

			if (working_node->type == TYPE_DIR && working_node->children != 0 && working_node->children->size > 0) {
				
				// 1. Находим максимальную длину имени среди детей для красивого выравнивания
				int max_name_length = 0;
				for (uint32_t i = 0; i < working_node->children->size; i++) {
					Dnode* child = vector_get(working_node->children, i);
					if (!child) continue;

					int len = strlen(child->name);
					if (child->type == TYPE_DIR) len += 1; // Учитываем плюс 1 символ для '/'

					if (len > max_name_length) {
						max_name_length = len;
					}
				}

				// 2. Выводим список элементов
				for (uint32_t i = 0; i < working_node->children->size; i++) {
					Dnode* child = vector_get(working_node->children, i);
					if (!child) continue;

					char string_buffer[80];
					char int_buffer[12];
					char display_name[64];

					// Гарантированная очистка буферов перед каждой записью!
					clear(string_buffer, 80);
					clear(int_buffer, 12);
					clear(display_name, 64);

					strncpy(display_name, child->name, 63);

					if (child->type == TYPE_DIR) {
						append(display_name, '/');
						
						// Формируем формат вывода, например: "%-15s %o %d\n"
						append(string_buffer, '%');
						int_to_ascii(max_name_length, int_buffer);
						append_string(string_buffer, int_buffer);
						append_string(string_buffer, "s %o %d\n");

						kprintf(0, 0x09, 2, string_buffer, display_name, child->permissions, child->children ? child->children->size : 0);
					} 
					else if (child->type == TYPE_FILE) {
						append(string_buffer, '%');
						int_to_ascii(max_name_length, int_buffer);
						append_string(string_buffer, int_buffer);
						append_string(string_buffer, "s %o %d\n");

						kprintf(0, 0x0A, 2, string_buffer, display_name, child->permissions, child->size);
					}
				}
			} else {
				kprintf(0, 0x07, 2, "<none>\n");
			}
		}
		else if(EQ(command,"reboot")){
			port_byte_out(0xCF9, 0xE);
		}
		else if(EQ(command,"cd")){
			current_node_if_fail = current_node;

			if (argv[1][0] != '\0') {
				current_node = dnode_find_by_path(ramfs_root, current_node, argv[1]);
			}
			if (current_node == 0) {
				current_node = current_node_if_fail;
				kprintf(font, 0x000c, 2, "Cannot change directory to %s\n", argv[1]);
			}
		}
		else if(EQ(command,"mk")||EQ(command,"mkdir")||EQ(command,"mkd")){

			Dnode* working_node = dnode_create_at_path(ramfs_root, current_node, argv[1], TYPE_DIR);
			if (working_node == 0) {
				kprintf(font, 0x000c, 2, "Failed to create directory: %s\n", argv[1]);
			}
		}
		else if(EQ(command,"touch")){

			Dnode* working_node = dnode_create_at_path(ramfs_root, current_node, argv[1], TYPE_FILE);
			if (working_node == 0) {
				kprintf(font, 0x000c, 2, "Failed to create file: %s\n", argv[1]);
			}
		}
		else if(EQ(command,"loadfont")){
			Dnode* target = 0;
			if (argv[1][0] != '\0') {
				target = dnode_find_by_path(ramfs_root, current_node, argv[1]);
			}

			if (target == 0) {
				kprintf(font, 0x000c, 2, "File not found: %s\n", argv[1]);
			} else if (target->type == TYPE_DIR) {
				kprintf(font, 0x000c, 2, "%s is a directory\n", argv[1]);
			} else {
				if (target->size == 4096) {
					vga_load_test_font((uint8_t*)target->data, 16);
					kprintf(font, 0x0007, 2, "Font loaded from %s\n", argv[1]);
				} else {
					kprintf(font, 0x0007, 2, "Not a valid font (.bm) file\n");
				}
			}
		}
		else if(EQ(command,"back")){
			current_node = current_node->parent;
		}
		else if(EQ(command,"lst")){
			mt_list_tasks();
		}
		else if(EQ(command,"lspci")){
			pci_scan();
		}
		else if(EQ(command,"lsblk")){
			blockdev_print();
		}
		else if(EQ(command,"klist")||EQ(command,"kver")||EQ(command,"kernels")||EQ(command,"kversions")){
			Dnode* kdir = dnode_find_by_path(ramfs_root, ramfs_root, "/sys/kernel");
			if (!kdir) {
				kprintf(font,0x000c,2,"/sys/kernel not found\n");
			} else if (kdir->type != TYPE_DIR) {
				kprintf(font,0x000c,2,"/sys/kernel is not a directory\n");
			} else {
				kprintf(font,0x0007,2,"Kernels in /sys/kernel:\n");
				if (kdir->children && kdir->children->size > 0) {
					for (uint32_t i=0;i<kdir->children->size;i++) {
						Dnode* child = vector_get(kdir->children, i);
						if (!child) continue;
						if (child->type == TYPE_FILE) {
							// skip meta files for cleaner list, but show them with marker
							int is_meta = (strcmp(child->name,"boot.cfg")==0 || strcmp(child->name,"default")==0 || strcmp(child->name,"active")==0 || strcmp(child->name,"versions.txt")==0 || strcmp(child->name,"README.txt")==0);
							if (is_meta) {
								kprintf(font,0x0008,2,"  [meta] %s (%d bytes)\n", child->name, child->size);
							} else {
								kprintf(font,0x000a,2,"  [kernel] %s (%d bytes)\n", child->name, child->size);
							}
						} else if (child->type == TYPE_DIR) {
							kprintf(font,0x0009,2,"  [dir] %s/\n", child->name);
						}
					}
				} else {
					kprintf(font,0x0007,2,"  <none>\n");
				}
				// show boot.cfg / default
				Dnode* bc = DFC(kdir,"boot.cfg");
				if (bc && bc->type==TYPE_FILE && bc->data) {
					kprintf(font,0x0007,2,"Default (boot.cfg): %s\n", bc->data);
				} else {
					Dnode* def = DFC(kdir,"default");
					if (def && def->type==TYPE_FILE && def->data) {
						kprintf(font,0x0007,2,"Default (default): %s\n", def->data);
					} else {
						kprintf(font,0x0007,2,"Default: kernel.bin (fallback)\n");
					}
				}
				kprintf(font,0x0007,2,"Current running: kernel %d.%d.%d\n", __PHT_KV_HIGH__, __PHT_KV_MID__, __PHT_KV_LOW__);
			}
		}
		else if(EQ(command,"kset")){
			if (argc < 2) {
				kprintf(font,0x000c,2,"Usage: kset <kernel_filename> - set default kernel in /sys/kernel/\n");
				kprintf(font,0x0007,2,"Example: kset kernel-1.0.0.bin\n");
			} else {
				Dnode* kdir = dnode_find_by_path(ramfs_root, ramfs_root, "/sys/kernel");
				if (!kdir) {
					kprintf(font,0x000c,2,"/sys/kernel not found\n");
				} else {
					Dnode* target = DFC(kdir, argv[1]);
					if (!target || target->type != TYPE_FILE) {
						kprintf(font,0x000c,2,"Kernel file not found: %s\n", argv[1]);
					} else {
						// update boot.cfg, default, active
						const char* files_to_update[] = {"boot.cfg","default","active"};
						for (int f=0; f<3; f++) {
							Dnode* bf = DFC(kdir, files_to_update[f]);
							if (!bf) {
								char pathbuf[64];
								clear(pathbuf,64);
								append_string(pathbuf,"/sys/kernel/");
								append_string(pathbuf,files_to_update[f]);
								bf = dnode_create_at_path(ramfs_root, ramfs_root, pathbuf, TYPE_FILE);
							}
							if (bf) {
								// clear old data
								if (bf->data) kfree(bf->data);
								uint32_t len = strlen(argv[1]);
								bf->data = kmalloc(len+2);
								if (bf->data) {
									for (uint32_t i=0;i<len;i++) bf->data[i]=argv[1][i];
									bf->data[len]='\n';
									bf->data[len+1]='\0';
									bf->size=len+1;
									fs_flush_node(bf);
								}
							}
						}
						kprintf(font,0x000a,2,"Default kernel set to %s\n", argv[1]);
						kprintf(font,0x0007,2,"It will be used on next boot (if bootloader FS support present)\n");
					}
				}
			}
		}
		else if(EQ(command,"kinstall")){
			if (argc < 3) {
				kprintf(font,0x000c,2,"Usage: kinstall <source_path> <dest_name>\n");
				kprintf(font,0x0007,2,"Example: kinstall /home/newkernel.bin kernel-1.1.0.bin\n");
				kprintf(font,0x0007,2,"Copies file to /sys/kernel/<dest_name>\n");
			} else {
				Dnode* src = dnode_find_by_path(ramfs_root, current_node, argv[1]);
				if (!src || src->type != TYPE_FILE) {
					kprintf(font,0x000c,2,"Source file not found: %s\n", argv[1]);
				} else {
					char destpath[128];
					clear(destpath,128);
					append_string(destpath,"/sys/kernel/");
					append_string(destpath,argv[2]);
					// check if dest exists
					Dnode* existing = dnode_find_by_path(ramfs_root, ramfs_root, destpath);
					if (existing) {
						kprintf(font,0x000e,2,"Destination %s already exists, overwriting...\n", destpath);
						if (existing->data) kfree(existing->data);
						existing->data = kmalloc(src->size);
						if (existing->data) {
							for (uint32_t i=0;i<src->size;i++) existing->data[i]=src->data[i];
							existing->size=src->size;
							fs_flush_node(existing);
							kprintf(font,0x000a,2,"Installed %s (%d bytes)\n", destpath, src->size);
						}
					} else {
						Dnode* dst = dnode_create_at_path(ramfs_root, ramfs_root, destpath, TYPE_FILE);
						if (!dst) {
							kprintf(font,0x000c,2,"Failed to create %s\n", destpath);
						} else {
							dst->data = kmalloc(src->size);
							if (dst->data) {
								for (uint32_t i=0;i<src->size;i++) dst->data[i]=src->data[i];
								dst->size=src->size;
								fs_flush_node(dst);
								kprintf(font,0x000a,2,"Installed %s (%d bytes)\n", destpath, src->size);
							} else {
								kprintf(font,0x000c,2,"Out of memory\n");
							}
						}
					}
				}
			}
		}
		else if(EQ(command,"kinfo")){
			kprintf(font,0x0007,2,"PhotonOS kernel %d.%d.%d\n", __PHT_KV_HIGH__, __PHT_KV_MID__, __PHT_KV_LOW__);
			kprintf(font,0x0007,2,"Build: %s\n", BUILD_DATE);
			Dnode* kdir = dnode_find_by_path(ramfs_root, ramfs_root, "/sys/kernel");
			if (kdir) {
				kprintf(font,0x0007,2,"/sys/kernel exists, %d entries\n", kdir->children ? kdir->children->size : 0);
			} else {
				kprintf(font,0x0007,2,"/sys/kernel not found (old image?)\n");
			}
			blockdev_print();
		}
		else if(EQ(command,"memleak")){
			kmalloc(64*1024);
		}
		else if(EQ(command,"bit9")){
			b9 = !b9;
			if(b9){
				kprintf(font,0x0007,2,"9-pixel wide character mode enabled\n");
			} else {
				kprintf(font,0x0007,2,"9-pixel wide character mode disabled\n");
			}
			kprintf(font,0x0007,2,"\xb0\xb1\xb2\xdb\xb2\xb1\xb0\n");
			bit9(b9);
		}
		else if(EQ(command,"charmap")){
			kprintf(font,0x000f,3," . . . . . . . . . . . . . . . . \n");
			for(int y=1;y<16;y++){
				kprintf(font,0x000f,3," ");
				if(y&1)kprintf(font,0x000f,3," ");
				for(int x=0;x<16;x++){
					kprintf(font,0x000f,3,"%c ",x+y*16);
				}	
				kprintf(font,0x000f,3,"\n");
			}
			for(int i=0;i<8;i++)kprintf(font,i,2,"\x8d\x8d");
			kprintf(font,0x000f,3,"\n");
			for(int i=0;i<8;i++)kprintf(font,i+8,2,"\x8d\x8d");
			kprintf(font,0x000f,3,"\n");
		}
		else if (EQ(command, "cat") || EQ(command, "type")) {
			Dnode* target = 0;
			if (argv[1][0] != '\0') {
				target = dnode_find_by_path(ramfs_root, current_node, argv[1]);
			}

			if (target == 0) {
				kprintf(font, 0x000c, 2, "File not found: %s\n", argv[1]);
			} else if (target->type == TYPE_DIR) {
				kprintf(font, 0x000c, 2, "%s is a directory\n", argv[1]);
			} else {
				if (target->data != 0 && target->size > 0) {
					// Выводим строго заданное количество байт без ожидания '\0'
					kwrite(STDOUT, (const char*)target->data, target->size);
					kprintf(font, 0x0007, 2, "\n");
				} else {
					kprintf(font, 0x0007, 2, "(file is empty)\n");
				}
			}
		}
		else if(EQ(command,"exit")){
			kprintf(font,0x000c,3,"There is no exit.\n");
		}
		else if(EQ(command,"load")){
			// split(line,command,' ',2);

			// if (EQ(line, command)){
			// 	kprintf(font,0x000c,2,LOC_KERNEL_LOADING_FILE_NOT_GIVEN);
			// 	continue;
			// }
			
			// kprintf(font,0x0007,2,LOC_KERNEL_LOADING_FILE,command);
			// kfree(init);

			// if(vd_find_file(&disk,&file,command)){
			// 	kprintf(font,0x0007,2,LOC_KERNEL_FILE_NOT_FOUND,command);
			// }

			// init = kmalloc(file.size*512);

			// if(vd_read_file(&disk,command,init)){
			// 	kprintf(font,0x000c,2,LOC_KERNEL_FILES_ONBOARD);
			// }
			// kprintf(font,0x0007,2,LOC_KERNEL_STATISTICS);
			// kprintf(font,0x0007,2,LOC_KERNEL_STATISTICS_FILENAME,file.size/2);
			// kprintf(font,0x0007,2,LOC_KERNEL_STATISTICS_SIZE,file.name);
		}
		else if(EQ(command,"zombies")){
			zombies();
		}
		else if(EQ(command,"show")){
			// показать байты в оперативной памяти, начиная с адреса, указанного в команде (тип hex, show 900000:)

			uint32_t code; // Используем беззнаковый тип для 32-битного адреса

			// Прочесть адрес в HEX
			sscanf(argv[1], "%x", &code);

			char *RAM = (char*)0x00000000;

			kprintf(font, 0x000f, 2, "          0");
			for (int j = 1; j < 16; j++) {
				kprintf(font, 0x000f, 2, " %x", j);
			}
			kprintf(font, 0x0007, 2, "\n");

			for (int i = -2; i < 3; i++) {
				// 1. Проверяем выход за нижнюю границу (меньше 0)
				if (i < 0 && code < (uint32_t)(-i * 16)) {
					continue; // Пропускаем строки с адресами меньше 0x0
				}

				uint32_t addr = code + i * 16;

				// 2. Проверяем переполнение верхней границы (4 ГБ для 32-битного режима)
				if (addr > 0xFFFFFFF0) {
					break; 
				}

				// Выводим сам адрес (исправлено double-addition: берем addr, а не code + addr)
				kprintf(font, 0x0007 + 8*(i==0), 2, "%08X ", addr);

				// HEX-дамп байт
				for (int j = 0; j < 16; j++) {
					kprintf(font, 0x0007 + 8*(j&1), 2, "%02X", (unsigned char)RAM[addr + j]);
				}
				kprintf(font, 0x0007, 2, " ");

				// ASCII-дамп символов
				for (int j = 0; j < 16; j++) {
					char ch = RAM[addr + j];
					kprintf(font, 0x0007 + 8*(j&1), 2, "%c", (ch >= 32 && ch <= 126) ? ch : '.');
				}
				kprintf(font, 0x0007, 2, "\n");
			}
		}
		else if(EQ(command,"auto")){
			kprintw(LOC_KERNEL_NOT_IMPLEMENTED, command);
		}
		else if(EQ(command,"run")){
			kprintw(LOC_KERNEL_NOT_IMPLEMENTED, command);
		}
		else if(strlen(command)>0){
			kprintf(font,0x000c,2,LOC_KERNEL_CMD_NOT_FOUND,command);
		} else {
			
		}
   	}
    
}

// #define LA_IMPLEMENTATION
// #include <la.h>
