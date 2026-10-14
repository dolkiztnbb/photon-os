#ifndef BLKDEV_SIM_H
#define BLKDEV_SIM_H
#include <stdint.h>

/* содержимое «дисков» задаёт тест */
extern uint8_t* sim_hdd;  extern uint32_t sim_hdd_sectors;   /* primary master, 512 */
extern uint8_t* sim_cd;   extern uint32_t sim_cd_blocks;     /* secondary master, 2048 */
extern int sim_hdd_present, sim_cd_present;
extern int sim_quiet;
extern unsigned long sim_io_count;

void sim_reset(void);

#endif
