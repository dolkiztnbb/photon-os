MAKEFLAGS += --no-print-directory

BUILD_DIR := output/build
# Находим все C файлы
C_SOURCES = $(wildcard kernel/*.c kernel/datslaf/*.c drivers/*.c cpu/*.c libc/*.c)
# Находим все ASM файлы (интерфейсные, которые компилируются в .o)
ASM_SOURCES = $(wildcard cpu/*.asm)
EXCLUDES = *.o *.so *.bin

# Пути к скриптам
FS_BUILDER := ./kernel/datslaf/build_fs.py
MAP_BUILDER := ./kernel/datslaf/generate_map.py
FS_SOURCE_DIR := ./kernel/datslaf/root
MAP_OUTPUT := $(FS_SOURCE_DIR)/sys/kernelmapping.txt

DATE := $(shell date "+%b_%d_%Y_%H-%M")
ZIP_NAME := autobuild_$(DATE).zip

# Превращаем .c в .o
OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
# Превращаем .asm в .o и добавляем к списку
OBJ += $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))

COMMON_OBJ = $(BUILD_DIR)/libc/string.o \
			 $(BUILD_DIR)/cpu/ports.o \
			 $(BUILD_DIR)/cpu/malloc/kmalloc.o

KERNEL_BRIDGE_OBJ = output/build/drivers/dumb-pci.o \
					output/build/kernel/loader.o \
					output/build/cpu/ports.o \
					output/build/boot/kernel_entry.o
					
EXCLUDE_SRC = loader.c dumb-pci.c
filtered_sources = $(filter-out $(EXCLUDE_SRC), $(C_SOURCES))

# Формируем объекты
INIT_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(filtered_sources))
INIT_OBJ += $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
INIT_OBJ := $(filter-out $(BUILD_DIR)/drivers/dumb-pci.o, $(INIT_OBJ))
INIT_OBJ := $(filter-out $(BUILD_DIR)/kernel/loader.o, $(INIT_OBJ))
INIT_OBJ += $(BUILD_DIR)/cpu/malloc/kmalloc.o

ifeq ("$(wildcard /c/msys64/usr/lib/gcc/x86_64-pc-msys/13.3.0/include)","")
	CFLAGS = -O1 -I. -w -nostdlib -I/c/msys64/usr/lib/gcc/x86_64-pc-msys/13.3.0/include -ffreestanding -Wdiscarded-qualifiers -Wpointer-sign -Wint-conversion -fno-exceptions -m32 -Wcomment -Wunused-parameter -fsemantic-interposition -ftree-coalesce-vars -fweb -frename-registers -Wno-pointer-sign -Wno-implicit-function-declaration -Wdiscarded-qualifiers -Wincompatible-pointer-types
else
	CFLAGS = -O1 -I. -w -nostdlib -ffreestanding -Wdiscarded-qualifiers -Wpointer-sign -Wint-conversion -fno-exceptions -m32 -Wcomment -Wunused-parameter -fsemantic-interposition -ftree-coalesce-vars -fweb -frename-registers -Wno-pointer-sign -Wno-implicit-function-declaration -Wdiscarded-qualifiers -Wincompatible-pointer-types
endif

# fixed makefile to run on both machines
ifeq ("$(wildcard /usr/local/i386elfgcc/bin/i386-elf-gcc)","")
	LD = i386-elf-ld 
	CC = i386-elf-gcc
	OBJC = i386-elf-objcopy
	NM = i386-elf-nm
else
	ifeq ("$(wildcard /mnt/d/gcc/bin/i386-elf-gcc)","")
		LD = /usr/local/i386elfgcc/bin/i386-elf-ld 
		CC = /usr/local/i386elfgcc/bin/i386-elf-gcc
		OBJC = /usr/local/i386elfgcc/bin/i386-elf-objcopy
		NM = /usr/local/i386elfgcc/bin/i386-elf-nm
	else # added fallback options when i386elfgcc is not installed
# ONLY IF YOU ARE X86
		LD = ld -m elf_i386 -s
		CC = gcc -fno-pie -O1
		OBJC = objcopy
		NM = nm
	endif
endif

auto:
	@$(MAKE) cycle

autozip:${ZIP_NAME}
${ZIP_NAME}: 
	@mkdir -p autozip
	@zip -r "autozip/$@" "." -x "*.zip" -x "*.rar" -x "*.o" -x "*.so" -x "*.bin" -x "*.swp" -x .git/\* -x .idea/\* -x "$(BUILD_DIR)/\*" -qq

info:
	@printf "\033[107;30m LD: ${LD} \033[0m\n"
	@printf "\033[107;30m GCC: ${CC} \033[0m\n"
	@printf "\033[107;30m OBJC: ${OBJC} \033[0m\n"
	@printf "\033[107;30m NM: ${NM} \033[0m\n"

# First rule is run by default
os-image.bin: boot/bootsect.bin kernel.bin init.bin build_vd
	@printf "\033[107;30m Building aligned image... \033[0m\n"
	@dd if=/dev/zero of=$@ bs=512 count=128 > /dev/null
	@dd if=boot/bootsect.bin of=$@ conv=notrunc bs=512 seek=0 2> /dev/null
	@dd if=kernel.bin of=$@ conv=notrunc bs=512 seek=1 2> /dev/null
	@dd if=init.bin of=$@ conv=notrunc bs=512 seek=60 2> /dev/null
	@dd if=kernel/datslaf/fs.vd of=$@ conv=notrunc bs=512 seek=384 2> /dev/null
	@-mkdir -p output/build/image
	@head -c 524288 /dev/zero | LC_ALL=C tr "\x00" "\x5f" >output/build/disk2.bin
	@mv os-image.bin output/build/image/os-image.bin
	@truncate -s 2M output/build/image/os-image.bin
	@cat output/build/image/os-image.bin | sha256sum > output/build/sha256.txt

padding.bin:
	@head -c 131072 /dev/zero | LC_ALL=C tr "\x00" "\x5f" >padding.bin

buildinfo:
	@echo '#define BUILD_DATE "PhotonOS build at ${DATE}"' > buildinfo.h

# 3. Правило сборки kernel.bin (Мост)
kernel.bin: $(KERNEL_BRIDGE_OBJ)
	@printf "\033[0m  LD    bridge,\t$@              \n"
	@${LD} -no-pie --oformat=binary -T kernel.ld -o $@ $^
	
kernel.elf: $(KERNEL_BRIDGE_OBJ)
	@printf "\033[0m  LD    bridge,\t$@              \n"
	@${LD}  --oformat=binary -T kernel.ld -o $@ $^

init.bin: $(INIT_OBJ)
	@printf "\033[0m  LD    init,\t$@              \n"
	@${LD} -no-pie --oformat=binary -T init.ld -o $@ $^

init.elf: $(INIT_OBJ)
	@printf "\033[0m  LD    init.elf,\t$@          \n"
	@${LD} -no-pie -T init.ld -o $@ $^

# Генерация карты символов в kernel/datslaf/root/sys/kernelmapping.txt
$(MAP_OUTPUT): init.elf
	@mkdir -p $(dir $@)
	@python3 $(MAP_BUILDER) init.elf $@ $(NM)

# Сборка .vd образа файловой системы с предварительным обновлением карты символов
build_vd: $(MAP_OUTPUT)
	@printf "\033[107;30m Building full filesystem .vd image... \033[0m\n"
	@python3 ${FS_BUILDER} ${FS_SOURCE_DIR} kernel/datslaf/fs.vd

update:
	@$(MAKE) buildinfo
	@$(MAKE) autozip
	@printf "Output would be written to output/build/image/os-image.bin\n"

setup:
	@-mkdir -p image

run: os-image.bin
	@"/mnt/c/Program Files/qemu/qemu-system-x86_64.exe" \
		-drive id=disk,file=output/build/image/os-image.bin,if=none,format=raw \
		-device ahci,id=ahci \
		-cpu max \
		-device ide-hd,drive=disk,bus=ahci.0 \
 		-serial stdio \
		-display sdl

#-audiodev pa,id=snd0,server=/mnt/wslg/PulseServer,out.latency=50000,out.buffer-length=100000 \

run_alt: 
	@qemu-system-x86_64 -hda output/build/disk2.bin -m 64M -soundhw pcspk

run_debug: os-image.bin
	@qemu-system-x86_64 -drive file=output/build/os-image.bin,format=raw,index=0,media=disk -soundhw pcspk -m 64M

run_floppy: os-image.bin
	@ndisasm -i output/build/os-image.bin 1> disasm.txt
	@qemu-system-x86_64 -fda output/build/os-image.bin -d trace:fdc_ioport_read,trace:fdc_ioport_write,trace:dma_blk_io,trace:dma_aio_cancel,trace:dma_complete,trace:dma_blk_cb,trace:dma_map_wait

debug: os-image.bin kernel.elf
	@qemu-system-i386 -s -drive file=os-image.bin,format=raw -d guest_errors,int &

compile_c: ${OBJ}
	@printf "  CCs\t${FILE}              \n"
	@${CC} ${CFLAGS} -c ${FILE} -o ${OUTPUT}.o -fPIE
	@printf "  LD\t${FILE} -> ${OUTPUT}.o\033[33m              \n"
	@${LD} -o ${OUTPUT} -T al.ld -emain $^ --oformat binary
	@printf "\033[0m"

compile_raw_c:
	@printf "\033[0m  CCr\t${FILE}\033[33m              \n"
	@${CC} ${CFLAGS} -c ${FILE} -o ${OUTPUT}.o -fPIE
	@printf "\033[0m  LD\t${OUTPUT}.o -> ${OUTPUT}.elf\033[33m              \n"
	@${LD} -m elf_i386 ${OUTPUT}.o -o ${OUTPUT}.elf -T al.ld --entry=main
	@printf "\033[0m  OBJC\t${OUTPUT}.elf -> ${OUTPUT}.bin\033[33m              \n"
	@${OBJC} -I elf32-i386 -O binary ${OUTPUT}.elf ${OUTPUT}.bin 
	@printf "\033[0m"
	@rm ${OUTPUT}.o
	@rm ${OUTPUT}.elf

compile_asm:
	@printf "  ASM\t${FILE} -> ${OUTPUT}              \n"
	@nasm ${FILE} -o ${OUTPUT} -w+number-overflow -w+other

# Обновленная цель cycle
cycle:
	@$(MAKE) clear
	@printf "\n\033[107;30m Building image with filesystem & map... \033[0m\n"
	@$(MAKE) os-image.bin
	@printf "\033[107;30m Updating... \033[0m\n"
	@$(MAKE) update
	
$(BUILD_DIR)/%.o: %.c ${HEADERS}
	@mkdir -p $(dir $@)
	@printf "  CC\t$<              \n"
	@${CC} ${CFLAGS} -fno-pic -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	@printf "  ASM\t$<            \n"
	@nasm $< -f elf -o $@ -w+number-overflow -w+other

%.bin: %.asm
	@printf "  ASM\t$<              \n"
	@nasm $< -f bin -o $@ -w+number-overflow -w+other

clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf *.bin *.dis *.o os-image.bin *.elf *.swp
	@rm -f *.vd $(MAP_OUTPUT)
	@rm -rf kernel/*.o boot/*.bin drivers/*.o boot/*.o cpu/*.o libc/*.o */*.o
	@-rm -rf image

clear:
	@$(MAKE) clean

Wrun:
	@wsl bash -c "cd ${PROJECT_PATH};make run;make clean"

Wcycle:
	@wsl bash -c "cd ${PROJECT_PATH};make cycle;make clean"