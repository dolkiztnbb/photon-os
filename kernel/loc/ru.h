// russian

// system messages

const char LOC_KERNEL_GREET[] = "Добро пожаловать в PhotonOS!\n";
const char LOC_KERNEL_NO_GRAPHICS[] = "Эта сборка не поддерживает графику (см. ./setup.h)\n";
const char LOC_KERNEL_CMD_TOO_LONG[] = "Слишком длинная команда, игнорируется\n";
const char LOC_KERNEL_CMD_NOT_FOUND[] = "%s: команда не найдена\n";

// general messages

const char LOC_KERNEL_LOADING_FILE_NOT_GIVEN [] = "Файл для загрузки не указан\n";
const char LOC_KERNEL_LOADING_FILE[] = "Загрузка %s...\n";
const char LOC_KERNEL_FILES_ONBOARD [] = "Файлы на диске:\n";
const char LOC_KERNEL_FILE_IMPOSSIBLE_TO_LOAD [] = "Невозможно загрузить файл\n";
const char LOC_KERNEL_FILE_NOT_FOUND[] = "%s не найден\n";
const char LOC_KERNEL_FIRST_LOAD_INIT[] = "Вы должны сначала загрузить init\n";
const char LOC_KERNEL_FILES_CURRENT_DIRECTORY[] = "Каталог %s:\n";
const char LOC_KERNEL_FILES_CURRENT_DIRECTORY_LIKE[] = "Каталог %s (как root):\n";

// help category

const char LOC_KERNEL_CMD_HELP [] = "исп.: help [auto cd charmap dir exit list load loadfont ls lsblk lspci lst mem mk run touch zombies exit]\n";

const char LOC_KERNEL_CMD_LIST [] = "LIST: показать все файлы в каталоге\nСинонимы:\n- ls\n- dir\n";
const char LOC_KERNEL_CMD_LOAD [] = "LOAD <file>: загрузить файл в исполняемую память\n";
const char LOC_KERNEL_CMD_RUN [] = "RUN: запустить файл, загруженный командой LOAD\n";
const char LOC_KERNEL_CMD_AUTO [] = "AUTO: загрузить 'init' с диска и запустить его\n";
const char LOC_KERNEL_CMD_LSPCI [] = "LSPCI: показать все устройства PCI\n";
const char LOC_KERNEL_CMD_LSBLK [] = "LSBLK: показать блочные устройства (SATA/IDE/CD-ROM/ramdisk) и загрузочное\n";
const char LOC_KERNEL_CMD_CHARMAP [] = "CHARMAP: показать карту символов\n";
const char LOC_KERNEL_CMD_ZOMBIES [] = "ZOMBIES: создать зомби\n";
const char LOC_KERNEL_CMD_CD [] = "CD: изменить текущий каталог\n";
const char LOC_KERNEL_CMD_LST [] = "LST: показать все активные задачи\n";
const char LOC_KERNEL_CMD_MK [] = "MK: создать каталог\nСинонимы:\n- mkdir\n- mkd\n";
const char LOC_KERNEL_CMD_TOUCH [] = "TOUCH: создать пустой файл\n";
const char LOC_KERNEL_CMD_BIT9 [] = "BIT9: переключить режим символа шириной 9 пикселей\n";
const char LOC_KERNEL_CMD_MEM [] = "MEM: показать информацию о памяти\n";
const char LOC_KERNEL_CMD_EXIT [] = "EXIT: (попытаться) выйти из системы\n";
const char LOC_KERNEL_CMD_TYPE [] = "CAT: показать содержимое файла\n";
const char LOC_KERNEL_CMD_LOADFONT [] = "LOADFONT <file>: загрузить файл шрифта\n";