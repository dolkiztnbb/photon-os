// english

// system messages

const char LOC_KERNEL_GREET[] = "Welcome to PhotonOS!\n";
const char LOC_KERNEL_NO_GRAPHICS[] = "This distribution does not support graphics (see ./setup.h)\n";
const char LOC_KERNEL_CMD_TOO_LONG[] = "Command too long, ignored\n";
const char LOC_KERNEL_CMD_NOT_FOUND[] = "%s: command not found\n";
const char LOC_KERNEL_NOT_IMPLEMENTED[] = "%s: not implemented\n";

// general messages

const char LOC_KERNEL_LOADING_FILE_NOT_GIVEN [] = "File for loading not given\n";
const char LOC_KERNEL_LOADING_FILE[] = "Loading %s...\n";
const char LOC_KERNEL_FILES_ONBOARD [] = "Files on disk:\n";
const char LOC_KERNEL_FILE_IMPOSSIBLE_TO_LOAD [] = "File impossible to load\n";
const char LOC_KERNEL_FILE_NOT_FOUND[] = "%s not found\n";
const char LOC_KERNEL_FIRST_LOAD_INIT[] = "You must load init first\n";
const char LOC_KERNEL_FILES_CURRENT_DIRECTORY[] = "Directory %s:\n";
const char LOC_KERNEL_FILES_CURRENT_DIRECTORY_LIKE[] = "Directory %s (root-like):\n";

// help category

const char LOC_KERNEL_CMD_HELP [] = "use: help [auto cd charmap dir exit list load loadfont ls lsblk lspci lst memleak mk run touch zombies exit]\n";

const char LOC_KERNEL_CMD_LIST [] = "LIST: display all files in the root directory\nAliases:\n- ls\n- dir\n";
const char LOC_KERNEL_CMD_LOAD [] = "LOAD <file>: load file into executable memory\n";
const char LOC_KERNEL_CMD_RUN [] = "RUN: run the file loaded with the LOAD command\n";
const char LOC_KERNEL_CMD_AUTO [] = "AUTO: load 'init' from the disk and run it\n";
const char LOC_KERNEL_CMD_LSPCI [] = "LSPCI: display all PCI devices\n";
const char LOC_KERNEL_CMD_LSBLK [] = "LSBLK: display block devices (SATA/IDE/CD-ROM/ramdisk) and the boot device\n";
const char LOC_KERNEL_CMD_CHARMAP [] = "CHARMAP: display the character map\n";
const char LOC_KERNEL_CMD_ZOMBIES [] = "ZOMBIES: create zombies\n";
const char LOC_KERNEL_CMD_CD [] = "CD: change to a different directory\n";
const char LOC_KERNEL_CMD_LST [] = "LST: display all active tasks\n";
const char LOC_KERNEL_CMD_MK [] = "MK: create a directory\nAliases:\n- mkdir\n- mkd\n";
const char LOC_KERNEL_CMD_TOUCH [] = "TOUCH: create an empty file\n";
const char LOC_KERNEL_CMD_BIT9 [] = "BIT9: toggle 9-pixel wide character mode\n";
const char LOC_KERNEL_CMD_LOADFONT [] = "LOADFONT <file>: load a font file\n";
const char LOC_KERNEL_CMD_MEM [] = "MEM: display memory information\n";
const char LOC_KERNEL_CMD_EXIT [] = "EXIT: (try to) exit the system\n";
const char LOC_KERNEL_CMD_TYPE [] = "CAT: print file contents\n";
const char LOC_KERNEL_CMD_MEMLEAK [] = "MEMLEAK: allocate 64kb of ram to nowhere\n";
const char LOC_KERNEL_CMD_REBOOT [] = "REBOOT: reboot the system\n";