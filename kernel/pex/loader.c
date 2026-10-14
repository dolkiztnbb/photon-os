#include "header.h"
#include "loader.h"
#include <kernel/files.h>
#include <cpu/mt.h>
#include <cpu/vmm.h>
#include <libc/mem.h>
#include <cpu/malloc/kmalloc.h>

int pex_exec(const char* path, int argc, char** argv) {
    int32_t fd = kopen(path, READ | BINARY);
    if (fd < 0) return -1;

    pex_header_t header;
    int32_t bytes_read = kread(fd, (char*)&header, sizeof(pex_header_t));
    if (bytes_read != sizeof(pex_header_t) || header.magic != PEX_MAGIC) {
        kclose(fd);
        return -2;
    }

    // Проверка прав доступа
    task_t* cur_task = mt_get_current_task();
    if ((header.flags & (PEX_FLAG_ALLOW_IO | PEX_FLAG_RING0)) && cur_task && cur_task->uid != 0) {
        kprinte("pex: permission denied: '%s' requires root/sudo\n", path);
        kclose(fd);
        return -1;
    }

    // Пропускаем точки и слэши в начале относительного пути (./bin/psh -> bin/psh)
    const char* p = path;
    while (*p == '.' || *p == '/') p++;

    // Проверяем, лежит ли файл в доверенной системной папке bin или sys
    int is_system_path = (strncmp(p, "bin/", 4) == 0 || strncmp(p, "sys/", 4) == 0);

    if (!is_system_path) {
        header.flags &= ~(PEX_FLAG_SUID | PEX_FLAG_RING0 | PEX_FLAG_ALLOW_IO);
        kprinte("pex: not running '%s' from sudo\n", path);
    }

    process_space_t proc = vmm_create_process_space();
    if (!proc.pd_virt || !proc.cr3_phys) {
        kclose(fd);
        return -3;
    }

    uint32_t code_data_size = header.text_size + header.data_size;
    uint32_t total_mem_size = code_data_size + header.bss_size;
    uint32_t pages_count = (total_mem_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint8_t* temp_kernel_buf = (uint8_t*)kmalloc(code_data_size > 0 ? code_data_size : 4);
    if (!temp_kernel_buf) {
        vmm_destroy_process_space(&proc, 0, 0, 0);
        kclose(fd);
        return -4;
    }

    // 1. Выделяем страницы под стек процесса
    uint32_t stack_pages = (header.stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (stack_pages == 0) stack_pages = 4;
    uintptr_t stack_top_virt = PEX_STACK_BASE;

    for (uint32_t i = 0; i < stack_pages; i++) {
        void* phys_stack = kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
        if (!phys_stack) {
            kfree(temp_kernel_buf);
            vmm_destroy_process_space(&proc, 0, header.stack_size, 0);
            kclose(fd);
            return -4;
        }
        memset(phys_stack, 0, PAGE_SIZE);
        uintptr_t virt_stack = stack_top_virt - ((i + 1) * PAGE_SIZE);
        vmm_map_page(proc.pd_virt, virt_stack, (uintptr_t)phys_stack, 7);
    }

    // 2. Кучу (heap_pages) заранее НЕ выделяем!
    // Страницы кучи выделяются лениво по мере вызовов malloc()/brk() в sys_malloc (mt.c).

    // 3. Читаем .text + .data с диска
    kseek(fd, header.header_size, SEEK_SET);
    if (code_data_size > 0 &&
        kread(fd, (char*)temp_kernel_buf, code_data_size) != (int32_t)code_data_size) {
        kfree(temp_kernel_buf);
        vmm_destroy_process_space(&proc, 0, header.stack_size, 0);
        kclose(fd);
        return -5;
    }
    kclose(fd);

    // 4. Выделяем страницы под .text + .data + .bss и копируем код/данные (остаток .bss уже занулен)
    for (uint32_t i = 0; i < pages_count; i++) {
        void* phys_page = kmalloc_aligned_reserved(PAGE_SIZE, PAGE_SIZE);
        if (!phys_page) {
            kfree(temp_kernel_buf);
            vmm_destroy_process_space(&proc, total_mem_size, header.stack_size, 0);
            return -4;
        }
        memset(phys_page, 0, PAGE_SIZE);

        uintptr_t virt_addr = PEX_VIRT_BASE + (i * PAGE_SIZE);
        vmm_map_page(proc.pd_virt, virt_addr, (uintptr_t)phys_page, 7);

        uint32_t page_offset = i * PAGE_SIZE;
        if (page_offset < code_data_size) {
            uint32_t bytes_to_copy = code_data_size - page_offset;
            if (bytes_to_copy > PAGE_SIZE) bytes_to_copy = PAGE_SIZE;
            memcpy(phys_page, temp_kernel_buf + page_offset, bytes_to_copy);
        }
    }

    kfree(temp_kernel_buf);

    uint32_t entry_address = PEX_VIRT_BASE + header.entry_point;

    int pid = mt_add_task_ex_vmm(
        (void (*)(void*))entry_address,
        header.stack_size, header.heap_size,
        proc, total_mem_size, argc, argv,
        header.flags
    );
    if (pid < 0) {
        vmm_destroy_process_space(&proc, total_mem_size, header.stack_size, 0);
        return -6;
    }

    return pid;
}
