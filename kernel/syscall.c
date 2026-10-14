#include <cpu/int80h.h>
#include <kernel/files.h>
#include <cpu/ports.h>
#include <drivers/keyboard.h>
#include <drivers/screen.h>
#include <cpu/timer.h>
#include <cpu/mt.h>
#include <drivers/blockdev.h>

extern uint32_t page_directory[1024];
static uint8_t bcd_to_bin(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

static uint8_t read_cmos_register(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

extern volatile int32_t mouse_delta_x;
extern volatile int32_t mouse_delta_y;
extern volatile uint8_t mouse_buttons;

void int80h(registers_t *r) {
    if (!r) return;

    uint32_t syscall_num = r->eax;
    uint32_t arg1        = r->ebx;
    uint32_t arg2        = r->ecx;
    uint32_t arg3        = r->edx;
    uint32_t arg4        = r->esi;
    uint32_t arg5        = r->edi;

    int32_t ret = -1;
    task_t* cur_task = mt_get_current_task();
    uint32_t cr3 = cur_task ? cur_task->cr3 : (uint32_t)page_directory;
    if (cr3 == 0) cr3 = (uint32_t)page_directory;

    switch (syscall_num) {

        // ==========================================
        // 1. VFS / ФАЙЛЫ (0x01 - 0x0E)
        // ==========================================
        case SYS_OPEN: {
            if (!arg1) { ret = ERR_NOT_FOUND; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg1, 1, 0)) {
                ret = -1; break;
            }
            ret = kopen_at(cur_task ? cur_task->cwd : NULL, (const char*)arg1, (char)arg2);
            break;
        }

        case SYS_READ: {
            if (!arg2 || arg3 == 0) { ret = 0; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg2, arg3, 1)) {
                ret = -1; break;
            }
            ret = kread((int32_t)arg1, (char*)arg2, (uint32_t)arg3);
            break;
        }

        case SYS_WRITE: {
            if (!arg2 || arg3 == 0) { ret = 0; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg2, arg3, 0)) {
                ret = -1; break;
            }
            ret = kwrite((int32_t)arg1, (const char*)arg2, (uint32_t)arg3);
            break;
        }

        case SYS_CLOSE:
            ret = kclose((int32_t)arg1);
            break;

        case SYS_SEEK:
            ret = kseek((int32_t)arg1, (int32_t)arg2, (int)arg3);
            break;

        case SYS_QUERY: {
            if (!arg2) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg2, sizeof(FILE_QUERY), 1)) {
                ret = -1; break;
            }
            kquery((int32_t)arg1, (FILE_QUERY*)arg2);
            ret = ERR_OK;
            break;
        }

        case SYS_SET_IO_COLOR:
            ksetiocolor((uint8_t)arg1, (uint8_t)arg2, (uint8_t)arg3, (uint8_t)arg4);
            ret = ERR_OK;
            break;

        case SYS_READDIR: {
            if (!arg3) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg3, sizeof(vfs_dirent_t), 1)) {
                ret = -1; break;
            }
            ret = kreaddir((int32_t)arg1, (uint32_t)arg2, (vfs_dirent_t*)arg3);
            break;
        }

        case SYS_MKDIR: {
            if (!arg1) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg1, 1, 0)) {
                ret = -1; break;
            }
            ret = kmkdir(cur_task ? cur_task->cwd : NULL, (const char*)arg1);
            break;
        }

        case SYS_UNLINK: {
            if (!arg1) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg1, 1, 0)) {
                ret = -1; break;
            }
            ret = kunlink(cur_task ? cur_task->cwd : NULL, (const char*)arg1);
            break;
        }

        case SYS_STAT: {
            if (!arg1 || !arg2) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3) {
                if (!vmm_check_user_memory((uint32_t*)cr3, arg1, 1, 0) ||
                    !vmm_check_user_memory((uint32_t*)cr3, arg2, sizeof(vfs_stat_t), 1)) {
                    ret = -1; break;
                }
            }
            ret = kstat(cur_task ? cur_task->cwd : NULL, (const char*)arg1, (vfs_stat_t*)arg2);
            break;
        }

        case SYS_CHDIR: {
            if (!arg1 || !fs_root) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg1, 1, 0)) {
                ret = -1; break;
            }
            Dnode* target = dnode_find_by_path(fs_root, cur_task ? cur_task->cwd : NULL, (const char*)arg1);
            if (!target || target->type != TYPE_DIR) {
                ret = ERR_NOT_FOUND;
            } else {
                if (cur_task) cur_task->cwd = target;
                ret = ERR_OK;
            }
            break;
        }

        case SYS_GETCWD: {
            if (!arg1 || arg2 == 0) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg1, arg2, 1)) {
                ret = -1; break;
            }
            char path_buf[256] = {0};
            dnode_get_path(cur_task && cur_task->cwd ? cur_task->cwd : fs_root, path_buf);
            strncpy((char*)arg1, path_buf, arg2 - 1);
            ((char*)arg1)[arg2 - 1] = '\0';
            ret = ERR_OK;
            break;
        }

        case SYS_DUP2:
            ret = kdup2((int32_t)arg1, (int32_t)arg2);
            break;

        // ==========================================
        // 2. УПРАВЛЕНИЕ ПАМЯТЬЮ (0x10 - 0x1F)
        // ==========================================
        case SYS_MALLOC:
            ret = (int32_t)sys_malloc(arg1);
            break;

        case SYS_FREE:
            kfree((void*)arg1);
            ret = ERR_OK;
            break;

        case SYS_MSTATUS:
            if (cur_task) ret = (int32_t)(cur_task->info.heap_curr - cur_task->info.heap_start);
            else ret = 0;
            break;

        case SYS_BRK:
            ret = (int32_t)sys_brk((void*)arg1);
            break;

        case SYS_MMAP: {
            if (arg2 == 0 || !cur_task || !cur_task->proc_space.pd_virt) { ret = 0; break; }
            uint32_t pages = (arg2 + PAGE_SIZE - 1) / PAGE_SIZE;
            uintptr_t virt = arg1 ? arg1 : cur_task->info.heap_curr;
            for (uint32_t p = 0; p < pages; p++) {
                void* phys = kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
                if (!phys) { ret = 0; break; }
                memset(phys, 0, PAGE_SIZE);
                vmm_map_page(cur_task->proc_space.pd_virt, virt + p * PAGE_SIZE, (uintptr_t)phys, 0x07);
            }
            ret = (int32_t)virt;
            break;
        }

        case SYS_MUNMAP: {
            if (arg2 == 0 || !cur_task || !cur_task->proc_space.pd_virt) { ret = -1; break; }
            uint32_t pages = (arg2 + PAGE_SIZE - 1) / PAGE_SIZE;
            for (uint32_t p = 0; p < pages; p++) {
                uintptr_t virt = arg1 + p * PAGE_SIZE;
                uintptr_t phys = vmm_get_phys_mapping(cur_task->proc_space.pd_virt, virt);
                if (phys) {
                    vmm_unmap_page(cur_task->proc_space.pd_virt, virt);
                    kfree((void*)phys);
                }
            }
            ret = 0;
            break;
        }
        case SYS_SHM_CREATE:
            ret = sys_shm_create((uint32_t)arg1);
            break;

        case SYS_SHM_ATTACH:
            ret = (int32_t)sys_shm_attach((int32_t)arg1);
            break;

        case SYS_SHM_DETACH:
            ret = sys_shm_detach((int32_t)arg1);
            break;
        // ==========================================
        // 3. ПРОЦЕССЫ И МНОГОЗАДАЧНОСТЬ (0x20 - 0x2F)
        // ==========================================
        case SYS_ADD_TASK:
            ret = mt_add_task((void (*)(void*))arg1);
            break;

        case SYS_KILL_TASK:
            mt_kill_task((int)arg1);
            ret = ERR_OK;
            break;

        case SYS_EXEC: {
            const char* path = (const char*)arg1;
            char** u_argv = (char**)arg2;
            int argc = 0;
            
            // Если процесс передал аргументы, считаем их количество
            if (u_argv) {
                while (u_argv[argc] != NULL && argc < 31) argc++;
            } else {
                // Если аргументов нет, создадим фиктивный argv[0] = path
                static char* fallback_argv[2];
                fallback_argv[0] = (char*)path;
                fallback_argv[1] = NULL;
                u_argv = fallback_argv;
                argc = 1;
            }
            ret = pex_exec(path, argc, u_argv);
            break;
        }

        case SYS_EXIT:
            mt_exit_self_code((int)arg1);
            while (1) { asm volatile("cli; hlt"  : : :  "memory"); }
            break;

        case SYS_WAITPID:
            if (arg2 && cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg2, sizeof(int), 1)) {
                ret = -1; break;
            }
            ret = sys_waitpid((int)arg1, (int*)arg2);
            break;

        case SYS_GETPID:
            ret = mt_get_current_pid();
            break;

        case SYS_YIELD:
            mt_schedule();
            ret = ERR_OK;
            break;

        case SYS_SLEEP:
            sys_sleep(arg1);
            ret = ERR_OK;
            break;
        case SYS_GETUID:
            ret = cur_task ? cur_task->uid : 0;
            break;

        case SYS_REBOOT:
            // Безопасность: перезагружать систему может ТОЛЬКО root!
            if (cur_task && cur_task->uid != 0) {
                ret = -1; // Access Denied
                break;
            }
            
            // Аппаратная перезагрузка через порт 0xCF9 (PCI reset control)
            port_byte_out(0xCF9, 0x0E);

            // Запасной способ через контроллер клавиатуры PS/2 (если 0xCF9 не сработал)
            port_byte_out(0x64, 0xFE);

            while (1) { asm volatile("cli; hlt"  : : :  "memory"); }
            ret = ERR_OK;
            break;
        case SYS_SETUID: {
            int new_uid = (int)arg1;
            // Безопасность: только root (UID 0) может сменить свой UID на любой другой (сбросить права).
            // Обычный пользователь не может сам себе вернуть 0.
            if (cur_task && (cur_task->uid == 0 || cur_task->uid == new_uid)) {
                cur_task->uid = new_uid;
                ret = 0;
            } else {
                ret = -1; // Access Denied (EPERM)
            }
            break;
        }
        // ==========================================
        // 4. КОНСОЛЬ, TTY И ЭКРАН (0x30 - 0x3F)
        // ==========================================
        case SYS_SETIOCOL:
            ksetiocolor((uint8_t)arg1, (uint8_t)arg2, (uint8_t)arg3, (uint8_t)arg4);
            ret = ERR_OK;
            break;

        case SYS_SETIOFL:
            ksetioflags((uint8_t)arg1);
            ret = ERR_OK;
            break;

        case SYS_GOTOXY:
            setcursorpos(arg1, arg2);
            ret = ERR_OK;
            break;

        case SYS_GET_CURSOR: {
            if (!arg1 || !arg2) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3) {
                if (!vmm_check_user_memory((uint32_t*)cr3, arg1, sizeof(int), 1) ||
                    !vmm_check_user_memory((uint32_t*)cr3, arg2, sizeof(int), 1)) {
                    ret = -1; break;
                }
            }
            getcursorpos((int*)arg1, (int*)arg2);
            ret = ERR_OK;
            break;
        }

        case SYS_POLL_KEY:
            ret = (int32_t)poll_key();
            break;

        case SYS_CLEAR_SCR:
            cls((short)arg1 ? (short)arg1 : 0x07);
            ret = ERR_OK;
            break;

        case SYS_GET_SCR_SIZE: {
            if (!arg1 || !arg2) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3) {
                if (!vmm_check_user_memory((uint32_t*)cr3, arg1, sizeof(int), 1) ||
                    !vmm_check_user_memory((uint32_t*)cr3, arg2, sizeof(int), 1)) {
                    ret = -1; break;
                }
            }
            *(int*)arg1 = 80;
            *(int*)arg2 = 25;
            ret = ERR_OK;
            break;
        }
        case SYS_GET_MOUSE_DELTA: {
            // arg1: указатель на структуру mouse_state_t в пространстве пользователя
            if (!arg1) { 
                ret = ERR_INVALID_PARAM; 
                break; 
            }

            // Проверка безопасности доступа к памяти Ring 3
            if (cur_task && cur_task->cr3) {
                if (!vmm_check_user_memory((uint32_t*)cr3, arg1, sizeof(mouse_state_t), 1)) {
                    ret = -1; 
                    break;
                }
            }

            // Атомарно забираем дельту и сбрасываем счетчики
            asm volatile("cli"  : : :  "memory");
            int32_t dx = mouse_delta_x;
            int32_t dy = mouse_delta_y;
            uint8_t btn = mouse_buttons;
            mouse_delta_x = 0;
            mouse_delta_y = 0;
            asm volatile("sti" : : : "memory");

            mouse_state_t* user_mouse = (mouse_state_t*)arg1;
            user_mouse->dx = dx;
            user_mouse->dy = dy;
            user_mouse->buttons = btn;

            ret = ERR_OK;
            break;
        }
        // ==========================================
        // 5. ВРЕМЯ И ТАЙМЕРЫ (0x40 - 0x4F)
        // ==========================================
        case SYS_GET_TICKS:
            ret = (int32_t)get_ticks();
            break;

        case SYS_GET_TIME: {
            if (!arg1 || !arg2 || !arg3) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3) {
                if (!vmm_check_user_memory((uint32_t*)cr3, arg1, sizeof(uint8_t), 1) ||
                    !vmm_check_user_memory((uint32_t*)cr3, arg2, sizeof(uint8_t), 1) ||
                    !vmm_check_user_memory((uint32_t*)cr3, arg3, sizeof(uint8_t), 1)) {
                    ret = -1; break;
                }
            }
            *(uint8_t*)arg1 = bcd_to_bin(read_cmos_register(0x04)); // Hours
            *(uint8_t*)arg2 = bcd_to_bin(read_cmos_register(0x02)); // Minutes
            *(uint8_t*)arg3 = bcd_to_bin(read_cmos_register(0x00)); // Seconds
            ret = ERR_OK;
            break;
        }

        case SYS_GET_DATE: {
            if (!arg1 || !arg2 || !arg3) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3) {
                if (!vmm_check_user_memory((uint32_t*)cr3, arg1, sizeof(uint8_t), 1) ||
                    !vmm_check_user_memory((uint32_t*)cr3, arg2, sizeof(uint8_t), 1) ||
                    !vmm_check_user_memory((uint32_t*)cr3, arg3, sizeof(uint16_t), 1)) {
                    ret = -1; break;
                }
            }
            *(uint8_t*)arg1 = bcd_to_bin(read_cmos_register(0x07)); // Day
            *(uint8_t*)arg2 = bcd_to_bin(read_cmos_register(0x08)); // Month
            *(uint16_t*)arg3 = 2000 + bcd_to_bin(read_cmos_register(0x09)); // Year
            ret = ERR_OK;
            break;
        }

        // ==========================================
        // 6. IPC И СИГНАЛЫ (0x50 - 0x5F)
        // ==========================================
        case SYS_PIPE: {
            if (!arg1) { ret = ERR_INVALID_PARAM; break; }
            if (cur_task && cur_task->cr3 && !vmm_check_user_memory((uint32_t*)cr3, arg1, sizeof(int32_t) * 2, 1)) {
                ret = -1; break;
            }
            ret = kcreate_pipe((int32_t*)arg1);
            break;
        }

        case SYS_SIGNAL: {
            int target_pid = (int)arg1;
            int sig = (int)arg2;
            if (target_pid < 0 || (uint32_t)target_pid >= (uint32_t)mt_get_tasks()) {
                ret = -1; break;
            }
            if (sig == 9 || sig == 15) { // SIGKILL / SIGTERM
                mt_kill_task(target_pid);
                ret = ERR_OK;
            } else {
                ret = -1;
            }
            break;
        }

        // ==========================================
        // ПРИВИЛЕГИРОВАННЫЕ ДИСК И PCI
        // (сектор отсчитывается от начала образа ОС на том устройстве,
        //  с которого загрузились: SATA, IDE, CD-ROM или копия в ОЗУ)
        // ==========================================
        case SYS_AHCI_READ:
            ret = blk_read_sector((uint32_t)arg1, (char*)arg2);
            break;

        case SYS_AHCI_WRITE:
            ret = blk_write_sector((uint32_t)arg1, (const char*)arg2);
            break;

        case SYS_PCI_READ:
            ret = pci_read_config_32((uint8_t)arg1, (uint8_t)arg2, (uint8_t)arg3, (uint8_t)arg4);
            break;

        case SYS_PCI_WRITE:
            pci_write_config_32((uint8_t)arg1, (uint8_t)arg2, (uint8_t)arg3, (uint8_t)arg4, (uint32_t)arg5);
            ret = ERR_OK;
            break;

        case SYS_PCI_SCAN:
            ret = pci_scan_bus();
            break;

        default:
            kprinte("unhandled syscall 0x%X\n", syscall_num);
            ret = -1;
            break;
    }

    r->eax = ret;
}