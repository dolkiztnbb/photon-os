#include "files.h"
#include <cpu/malloc/kmalloc.h>
#include <libc/string.h>

extern char* font;

// Подключаем функцию синхронизации цвета из screen.c
extern void sync_screen_color(uint8_t bg, uint8_t fg);

FILE_HANDLE* handles = (FILE_HANDLE*)__PHT_MEM_HANDLES;
Dnode* fs_root = NULL;

uint8_t stdin_fg = 0x7;
uint8_t stdin_bg = 0;
uint8_t stdout_fg = 0x7;
uint8_t stdout_bg = 0;
uint8_t stdin_flags = 0;
uint8_t stdout_flags = 0;

void ksetiocolor(uint8_t in_bg, uint8_t in_fg, uint8_t out_bg, uint8_t out_fg) {
    stdin_fg = in_fg;
    stdin_bg = in_bg;
    stdout_fg = out_fg;
    stdout_bg = out_bg;

    // Мгновенно синхронизируем цвет терминала для будущих kputc
    sync_screen_color(out_bg, out_fg);
}

void ksetioflags(uint8_t in_f) {
    stdin_flags = in_f;
}

void kfinit(Dnode* root) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        handles[i].flags = FREE;
        handles[i].node = NULL;
        handles[i].pipe = NULL;
        handles[i].pos = 0;
        handles[i].fd = i;
        handles[i].error = ERR_OK;
    }

    handles[STDOUT].flags = USED | WRITE;
    handles[STDIN].flags  = USED | READ;
    handles[STDERR].flags = USED | WRITE;

    fs_root = root;
    if (fs_root == NULL) {
        kprinte("fs_root=NULL, mayday\n");
    }
}

int32_t kopen_at(Dnode* cwd, const char* path, char flags) {
    if (!path || !fs_root) return ERR_NOT_FOUND;

    int32_t free_handle_idx = -1;
    for (int i = 3; i < MAX_HANDLES; i++) {
        if ((handles[i].flags & USED) != USED) {
            free_handle_idx = i;
            break;
        }
    }
    if (free_handle_idx == -1) return ERR_NO_HANDLES;

    Dnode* node = dnode_find_by_path(fs_root, cwd, path);

    if (node == NULL) {
        if (flags & CREATE) {
            node = dnode_create_at_path(fs_root, cwd, path, TYPE_FILE);
            if (node == NULL) return ERR_NOT_FOUND;
        } else {
            return ERR_NOT_FOUND;
        }
    }

    if ((flags & TRUNCATE) && node->type == TYPE_FILE && node->data != NULL) {
        kfree(node->data);
        node->data = NULL;
        node->size = 0;
    }

    FILE_HANDLE* h = &handles[free_handle_idx];
    h->node = node;
    h->pipe = NULL;
    h->flags = (uint32_t)flags | USED;
    h->fd = free_handle_idx;
    h->error = ERR_OK;
    h->pos = (flags & APPEND) ? node->size : 0;

    return free_handle_idx;
}

int32_t kopen(const char* path, char flags) {
    return kopen_at(NULL, path, flags);
}

int32_t kwrite(int32_t fd, const char* data, uint32_t size) {
    if (fd < 0 || fd >= MAX_HANDLES || !data || size == 0) return 0;

    FILE_HANDLE* ptr = &handles[fd];

    // Если STDOUT/STDERR НЕ перенаправлены через dup2 в pipe или файл — пишем на экран
    if ((fd == STDOUT || fd == STDERR) && !(ptr->flags & IS_PIPE) && ptr->node == NULL) {
        for (uint32_t i = 0; i < size; i++) {
            kputc(data[i]);
        }
        return size;
    }

    if ((ptr->flags & USED) != USED) return ERR_USED;
    if ((ptr->flags & WRITE) != WRITE) return ERR_READ_ONLY;

    // Поддержка Pipe (в том числе при перенаправленном STDOUT через dup2)
    if ((ptr->flags & IS_PIPE) && ptr->pipe) {
        uint32_t written = 0;
        while (written < size && ptr->pipe->count < PIPE_BUFFER_SIZE) {
            ptr->pipe->buffer[ptr->pipe->head] = data[written++];
            ptr->pipe->head = (ptr->pipe->head + 1) % PIPE_BUFFER_SIZE;
            ptr->pipe->count++;
        }
        return written;
    }

    if (ptr->node == NULL) return ERR_USED;
    if (ptr->node->type == TYPE_DIR) return ERR_IS_DIR;

    uint32_t written = dnode_write(ptr->node, (char*)data, size, ptr->pos);
    ptr->pos += written;
    return written;
}

int32_t kread(int32_t fd, char* source, uint32_t size) {
    if (fd < 0 || fd >= MAX_HANDLES || !source || size == 0) return 0;

    FILE_HANDLE* ptr = &handles[fd];

    // Если STDIN НЕ перенаправлен через dup2 в pipe или файл — читаем с аппаратной клавиатуры
    if (fd == STDIN && !(ptr->flags & IS_PIPE) && ptr->node == NULL) {
        uint32_t i = 0;
        while (i < size) {
            char j = getch();
            if (j == '\n' || j == '\r') {
                source[i++] = '\0';
                break;
            } else if (j == '\b') {
                if (i > 0) {
                    i--;
                    kprintf(font, stdin_fg | (stdin_bg << 8), stdin_flags, "\b \b");
                }
            } else if ((unsigned char)j >= 32 && (unsigned char)j <= 126) {
                source[i++] = j;
                kprintf(font, stdin_fg | (stdin_bg << 8), stdin_flags, "%c", j);
            }
        }
        return i;
    }

    if (!(ptr->flags & READ)) return ERR_WRITE_ONLY;

    // Поддержка Pipe
    if ((ptr->flags & IS_PIPE) && ptr->pipe) {
        uint32_t read_bytes = 0;
        while (read_bytes < size && ptr->pipe->count > 0) {
            source[read_bytes++] = ptr->pipe->buffer[ptr->pipe->tail];
            ptr->pipe->tail = (ptr->pipe->tail + 1) % PIPE_BUFFER_SIZE;
            ptr->pipe->count--;
        }
        return read_bytes;
    }

    if (ptr->node == NULL) return ERR_USED;
    if (ptr->node->type == TYPE_DIR) return ERR_IS_DIR;

    if (ptr->node->data == NULL || ptr->pos >= ptr->node->size) {
        return 0;
    }

    uint32_t bytes_left = ptr->node->size - ptr->pos;
    if (size > bytes_left) {
        size = bytes_left;
    }

    uint32_t read_bytes = dnode_read(ptr->node, source, size, ptr->pos);
    ptr->pos += read_bytes;
    return read_bytes;
}

int32_t kseek(int32_t fd, int32_t offset, int whence) {
    if (fd <= STDERR || fd >= MAX_HANDLES) return ERR_USED;
    FILE_HANDLE* ptr = &handles[fd];

    if ((ptr->flags & USED) != USED || (ptr->flags & IS_PIPE)) return ERR_UNSEEKABLE;
    if (ptr->node == NULL) return ERR_USED;

    int32_t new_pos = 0;
    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = (int32_t)ptr->pos + offset; break;
        case SEEK_END: new_pos = (int32_t)ptr->node->size + offset; break;
        default: return ERR_INVALID_PARAM;
    }

    if (new_pos < 0 || (uint32_t)new_pos > ptr->node->size) {
        return ERR_INVALID_PARAM;
    }

    ptr->pos = (uint32_t)new_pos;
    return ptr->pos;
}

static void krelease_handle_target(FILE_HANDLE* ptr) {
    if (!ptr || (ptr->flags & USED) != USED) return;
    if ((ptr->flags & IS_PIPE) && ptr->pipe) {
        ptr->pipe->ref_count--;
        if (ptr->pipe->ref_count <= 0) {
            kfree(ptr->pipe);
        }
        ptr->pipe = NULL;
    } else if (ptr->node) {
        if ((ptr->flags & WRITE) || (ptr->flags & CREATE)) {
            fs_flush_node(ptr->node);
        }
        ptr->node = NULL;
    }
}

int32_t kclose(int32_t fd) {
    if (fd < 0 || fd >= MAX_HANDLES) return ERR_USED;

    // Если закрывают перенаправленный STDOUT/STDIN/STDERR — возвращаем его к стандартной консоли
    if (fd <= STDERR) {
        FILE_HANDLE* std_ptr = &handles[fd];
        if ((std_ptr->flags & IS_PIPE) || std_ptr->node != NULL) {
            krelease_handle_target(std_ptr);
            std_ptr->pos = 0;
            std_ptr->flags = USED | ((fd == STDIN) ? READ : WRITE);
            std_ptr->error = ERR_OK;
            return ERR_OK;
        }
        return ERR_UNCLOSABLE;
    }

    FILE_HANDLE* ptr = &handles[fd];
    if ((ptr->flags & USED) != USED) return ERR_USED;

    krelease_handle_target(ptr);

    ptr->pos = 0;
    ptr->node = NULL;
    ptr->flags = FREE;
    ptr->error = ERR_OK;
    return ERR_OK;
}

void kquery(int32_t fd, FILE_QUERY* query) {
    if (fd < 0 || fd >= MAX_HANDLES || !query) return;
    FILE_HANDLE* ptr = &handles[fd];
    if ((ptr->flags & USED) != USED || ptr->node == NULL) return;

    strncpy(query->name, ptr->node->name, MAX_FILENAME_LENGTH - 1);
    query->name[MAX_FILENAME_LENGTH - 1] = '\0';
    query->size = ptr->node->size;
    query->node = ptr->node;
}

int32_t kreaddir(int32_t fd, uint32_t index, vfs_dirent_t* entry) {
    if (fd < 0 || fd >= MAX_HANDLES || !entry) return ERR_INVALID_PARAM;
    FILE_HANDLE* ptr = &handles[fd];
    if ((ptr->flags & USED) != USED || !ptr->node || ptr->node->type != TYPE_DIR) {
        return ERR_NOT_DIR;
    }
    if (!ptr->node->children || index >= ptr->node->children->size) {
        return ERR_NOT_FOUND;
    }

    Dnode* child = vector_get(ptr->node->children, index);
    if (!child) return ERR_NOT_FOUND;

    strncpy(entry->name, child->name, MAX_FILENAME_LENGTH - 1);
    entry->name[MAX_FILENAME_LENGTH - 1] = '\0';
    entry->type = child->type;
    entry->size = child->size;

    return ERR_OK;
}

int32_t kmkdir(Dnode* cwd, const char* path) {
    if (!path || !fs_root) return ERR_INVALID_PARAM;
    Dnode* n = dnode_create_at_path(fs_root, cwd, path, TYPE_DIR);
    if (!n) return ERR_NOT_FOUND;

    fs_flush_node(n);
    return ERR_OK;
}

int32_t kunlink(Dnode* cwd, const char* path) {
    if (!path || !fs_root) return ERR_INVALID_PARAM;
    Dnode* target = dnode_find_by_path(fs_root, cwd, path);
    if (!target) return ERR_NOT_FOUND;
    if (target == fs_root) return ERR_USED;

    if (target->type == TYPE_DIR && target->children && target->children->size > 0) {
        return ERR_USED;
    }

    Dnode* parent = target->parent;
    if (parent && parent->children) {
        dnode_remove_child(parent, target);
    }

    if (target->id >= 0) {
        fs_delete_node(target);
    }

    if (target->data) {
        kfree(target->data);
        target->data = NULL;
    }
    if (target->children) {
        vector_free(target->children);
        kfree(target->children);
        target->children = NULL;
    }
    kfree(target);
    return ERR_OK;
}

int32_t kstat(Dnode* cwd, const char* path, vfs_stat_t* st) {
    if (!path || !st || !fs_root) return ERR_INVALID_PARAM;
    Dnode* target = dnode_find_by_path(fs_root, cwd, path);
    if (!target) return ERR_NOT_FOUND;

    st->type = target->type;

    if (st->type == TYPE_FILE) st->size = target->size;
    if (st->type == TYPE_DIR)  st->size = target->children->size;

    st->permissions = target->permissions;
    st->change_time = target->change_time;
    return ERR_OK;
}

int32_t kdup2(int32_t oldfd, int32_t newfd) {
    if (oldfd < 0 || oldfd >= MAX_HANDLES || newfd < 0 || newfd >= MAX_HANDLES) {
        return ERR_INVALID_PARAM;
    }
    if ((handles[oldfd].flags & USED) != USED) {
        return ERR_USED;
    }
    if (oldfd == newfd) {
        return newfd;
    }

    // Корректно освобождаем старый pipe/файл на newfd (даже если newfd == STDOUT/STDIN/STDERR)
    krelease_handle_target(&handles[newfd]);

    handles[newfd] = handles[oldfd];
    handles[newfd].fd = newfd;
    if ((handles[newfd].flags & IS_PIPE) && handles[newfd].pipe) {
        handles[newfd].pipe->ref_count++;
    }
    return newfd;
}

int32_t kcreate_pipe(int32_t pipefd[2]) {
    if (!pipefd) return ERR_INVALID_PARAM;

    int32_t r_fd = -1, w_fd = -1;
    for (int i = 3; i < MAX_HANDLES; i++) {
        if ((handles[i].flags & USED) != USED) {
            if (r_fd == -1) r_fd = i;
            else if (w_fd == -1) { w_fd = i; break; }
        }
    }
    if (r_fd == -1 || w_fd == -1) return ERR_NO_HANDLES;

    pipe_buffer_t* pipe = (pipe_buffer_t*)kmalloc(sizeof(pipe_buffer_t));
    if (!pipe) return ERR_NO_HANDLES;

    memset(pipe, 0, sizeof(pipe_buffer_t));
    pipe->ref_count = 2;

    handles[r_fd].fd = r_fd;
    handles[r_fd].flags = USED | READ | IS_PIPE;
    handles[r_fd].pipe = pipe;
    handles[r_fd].node = NULL;

    handles[w_fd].fd = w_fd;
    handles[w_fd].flags = USED | WRITE | IS_PIPE;
    handles[w_fd].pipe = pipe;
    handles[w_fd].node = NULL;

    pipefd[0] = r_fd;
    pipefd[1] = w_fd;
    return ERR_OK;
}
