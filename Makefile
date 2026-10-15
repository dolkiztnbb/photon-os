MAKEFLAGS += --no-print-directory

BUILD_DIR := output/build
IMAGE_DIR := $(BUILD_DIR)/image
IMAGE := $(IMAGE_DIR)/os-image.bin
ISO := $(IMAGE_DIR)/photon.iso

# --- Раскладка образа диска (в 512-байтных секторах) -------------------------
# Те же числа передаются в boot/bootsect.asm через -D (правило boot/bootsect.bin),
# stage2 и ядро узнают их в рантайме из заголовка в MBR (boot/bootinfo.h).
# stage2 занимает LBA 1..IMG_STAGE2_SECTORS (увеличено для поддержки загрузки ядра из ФС)
IMG_STAGE2_SECTORS := 31
# ядро (init.bin): LBA IMG_INIT_LBA, не больше IMG_INIT_SECTORS секторов
IMG_INIT_LBA := 60
IMG_INIT_SECTORS := 324
# файловая система (fs.vd)
IMG_FS_LBA := 384
# полный размер образа (4096 * 512 = 2 МБ)
IMG_SECTORS := 4096
# куда грузить ядро и копию образа (ramdisk) при загрузке с CD/USB
KERNEL_ADDR := 0x100000
RAMDISK_ADDR := 0x1000000
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

# Формируем объекты ядра
INIT_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
INIT_OBJ += $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
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

# Ядру нужно знать раскладку образа только как запасной вариант (если его
# запустили без нашего загрузчика); обычно он берёт её из bootinfo.
CFLAGS += -DFS_START_SECTOR=$(IMG_FS_LBA)

# --- QEMU --------------------------------------------------------------------
# Под WSL используем Windows-сборку QEMU (как раньше), иначе системную.
ifneq ("$(wildcard /mnt/c/Program*/qemu/qemu-system-x86_64.exe)","")
	QEMU := "/mnt/c/Program Files/qemu/qemu-system-x86_64.exe"
	QEMU_FLAGS := -cpu max -m 64M -serial stdio -display sdl
else
	QEMU := qemu-system-x86_64
	QEMU_FLAGS := -cpu max -m 64M -serial stdio
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

# Образ диска: MBR(stage1) | stage2 | ... | init.bin | fs.vd  (см. docs/BOOT.md)
.PHONY: os-image.bin iso check_bootinfo test_boot test_blkdev test
os-image.bin: $(IMAGE)

$(IMAGE): boot/bootsect.bin boot/stage2.bin init.bin build_vd tools/mkimage.py
	@printf "\033[107;30m Building image... \033[0m\n"
	@python3 tools/check_bootinfo.py
	@mkdir -p $(IMAGE_DIR)
	@python3 tools/mkimage.py --out $@ --sectors $(IMG_SECTORS) \
		--part boot/bootsect.bin@0:1 \
		--part boot/stage2.bin@1:$(IMG_STAGE2_SECTORS) \
		--part init.bin@$(IMG_INIT_LBA):$(IMG_INIT_SECTORS) \
		--part kernel/datslaf/fs.vd@$(IMG_FS_LBA):
	@head -c 524288 /dev/zero | LC_ALL=C tr "\x00" "\x5f" >$(BUILD_DIR)/disk2.bin
	@cat $@ | sha256sum > $(BUILD_DIR)/sha256.txt

# Загрузочный ISO (CD-ROM + гибридный MBR для записи на флешку через dd)
iso: $(ISO)
$(ISO): $(IMAGE) tools/mkiso.py
	@printf "\033[107;30m Building bootable ISO... \033[0m\n"
	@python3 tools/mkiso.py --image $(IMAGE) --out $@

# --- Тесты загрузчика и драйверов без QEMU ----------------------------------------
# Проверка согласованности boot/bootinfo.h и boot/bootinfo.inc
check_bootinfo:
	@python3 tools/check_bootinfo.py

# Прогон загрузчика в Unicorn (pip install unicorn): HDD/CHS/CD/USB-ISO
test_boot: os-image.bin iso
	@python3 tools/boottest.py --image $(IMAGE) --iso $(ISO) --mode all

# Хост-тест drivers/ata.c + blockdev.c + ramdisk.c на эмуляторе IDE-контроллера
BLKDEV_TEST := $(BUILD_DIR)/blkdev_test
test_blkdev: os-image.bin iso
	@mkdir -p $(BUILD_DIR)
	@gcc -no-pie -O1 -g -I. -fno-builtin -w -DATA_HOST_TEST -DPHT_BOOTINFO_ADDR=0x20000 \
		drivers/ata.c drivers/blockdev.c drivers/ramdisk.c \
		tools/blkdev_test/test.c tools/blkdev_test/sim.c -o $(BLKDEV_TEST)
	@for s in hdd cd cd-nord usb usb-iso legacy nodisk cdonly; do \
		SIM_QUIET=1 $(BLKDEV_TEST) $$s $(IMAGE) $(ISO) || exit 1; \
	done

test: test_boot test_blkdev

# stage1 получает раскладку образа через -D, stage2 читает её из MBR
boot/bootsect.bin: boot/bootsect.asm boot/bootinfo.inc
	@printf "  ASM\t$<              \n"
	@nasm $< -f bin -o $@ -w+number-overflow -w+other \
		-DIMG_STAGE2_SECTORS=$(IMG_STAGE2_SECTORS) -DIMG_INIT_LBA=$(IMG_INIT_LBA) \
		-DIMG_INIT_SECTORS=$(IMG_INIT_SECTORS) -DIMG_FS_LBA=$(IMG_FS_LBA) \
		-DIMG_SECTORS=$(IMG_SECTORS) -DKERNEL_ADDR=$(KERNEL_ADDR) -DRAMDISK_ADDR=$(RAMDISK_ADDR)

boot/stage2.bin: boot/bootinfo.inc

padding.bin:
	@head -c 131072 /dev/zero | LC_ALL=C tr "\x00" "\x5f" >padding.bin

buildinfo:
	@echo '#define BUILD_DATE "PhotonOS build at ${DATE}"' > buildinfo.h

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

# Директория для хранения ядер внутри ФС 
KERNEL_FS_DIR := $(FS_SOURCE_DIR)/sys/kernel

# Сборка .vd образа файловой системы: только один рабочий kernel.bin
build_vd: $(MAP_OUTPUT) init.bin
	@printf "\033[107;30m Preparing /sys/kernel in filesystem... \033[0m\n"
	@mkdir -p $(KERNEL_FS_DIR)
	@rm -f $(KERNEL_FS_DIR)/kernel*.bin $(KERNEL_FS_DIR)/versions.txt $(KERNEL_FS_DIR)/default $(KERNEL_FS_DIR)/active
	@cp init.bin $(KERNEL_FS_DIR)/kernel.bin
# 	@echo "kernel.bin" > $(KERNEL_FS_DIR)/boot.cfg
	@printf "\033[107;30m Building full filesystem .vd image... \033[0m\n"
	@python3 ${FS_BUILDER} ${FS_SOURCE_DIR} kernel/datslaf/fs.vd

update:
	@$(MAKE) buildinfo
	@$(MAKE) autozip
	@printf "Output would be written to output/build/image/os-image.bin\n"

setup:
	@-mkdir -p image

run: os-image.bin
	@$(QEMU) $(QEMU_FLAGS) \
		-drive id=disk,file=$(IMAGE),if=none,format=raw \
		-drive id=target,file=disk_target.bin,if=none,format=raw \
		-device ahci,id=ahci \
		-device ide-hd,drive=disk,bus=ahci.0,bootindex=1 \
		-device ide-hd,drive=target,bus=ahci.1,bootindex=2

# Классический IDE (PATA) диск
run_ide: os-image.bin
	@$(QEMU) $(QEMU_FLAGS) \
		-drive file=$(IMAGE),format=raw,if=ide,index=0,media=disk

# Загрузка с CD-ROM (IDE ATAPI)
run_cdrom: iso
	@$(QEMU) $(QEMU_FLAGS) -boot d -cdrom $(ISO)

# USB-флешка с сырым образом (USB 2.0 EHCI)
run_usb: os-image.bin
	@$(QEMU) $(QEMU_FLAGS) \
		-drive if=none,id=usbstick,file=$(IMAGE),format=raw \
		-device usb-ehci,id=ehci \
		-device usb-storage,bus=ehci.0,drive=usbstick

# USB-флешка с гибридным ISO
run_usb_iso: iso
	@$(QEMU) $(QEMU_FLAGS) \
		-drive if=none,id=usbstick,file=$(ISO),format=raw \
		-device usb-ehci,id=ehci \
		-device usb-storage,bus=ehci.0,drive=usbstick

#-audiodev pa,id=snd0,server=/mnt/wslg/PulseServer,out.latency=50000,out.buffer-length=100000 \

run_alt: 
	@qemu-system-x86_64 -hda output/build/disk2.bin -m 64M -soundhw pcspk

run_debug: os-image.bin
	@qemu-system-x86_64 -drive file=$(IMAGE),format=raw,index=0,media=disk -soundhw pcspk -m 64M

run_floppy: os-image.bin
	@ndisasm -i $(IMAGE) 1> disasm.txt
	@qemu-system-x86_64 -fda $(IMAGE) -d trace:fdc_ioport_read,trace:fdc_ioport_write,trace:dma_blk_io,trace:dma_aio_cancel,trace:dma_complete,trace:dma_blk_cb,trace:dma_map_wait

debug: os-image.bin init.elf
	@qemu-system-i386 -s -drive file=$(IMAGE),format=raw -d guest_errors,int &

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
	@rm -f $(KERNEL_FS_DIR)/kernel*.bin $(KERNEL_FS_DIR)/boot.cfg $(KERNEL_FS_DIR)/default $(KERNEL_FS_DIR)/active $(KERNEL_FS_DIR)/versions.txt
	@-rm -rf image

clear:
	@$(MAKE) clean

Wrun:
	@wsl bash -c "cd ${PROJECT_PATH};make run;make clean"

Wcycle:
	@wsl bash -c "cd ${PROJECT_PATH};make cycle;make clean"