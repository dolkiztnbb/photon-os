#ifndef RAMDISK_H
#define RAMDISK_H

#include <boot/bootinfo.h>

/* Регистрирует "rd0" — копию загрузочного образа, которую stage2 положил в
 * память (см. поле ramdisk_addr в bootinfo). Если копии нет — ничего не делает. */
void ramdisk_init_from_bootinfo(const pht_bootinfo_t* bi);

#endif
