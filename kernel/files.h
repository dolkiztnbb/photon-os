#ifndef __FILES_H__
#define __FILES_H__

#include <stdint.h>
#include <stddef.h>
#include <setup.h>
#include <kernel/datslaf/fs.h>

#define STDOUT 0
#define STDIN  1
#define STDERR 2

// Коды ошибок
#define ERR_OK             0
#define ERR_READ_ONLY     -2
#define ERR_WRITE_ONLY    -4
#define ERR_UNCLOSABLE    -8
#define ERR_UNSEEKABLE   -16
#define ERR_USED         -32
#define ERR_SIZE_OVERFLOW -64
#define ERR_NOT_FOUND    -128
#define ERR_NO_HANDLES   -256
#define ERR_INVALID_PARAM -512
#define ERR_NOT_DIR      -1024
#define ERR_IS_DIR       -2048

// Флаги открытия
#define READ     (1<<0) 
#define WRITE    (1<<1) 
#define APPEND   (1<<2) 
#define BINARY   (1<<3)
#define TEXT     (1<<4)
#define CREATE   (1<<5)
#define TRUNCATE (1<<6)
#define USED     (1<<7)
#define IS_PIPE  (1<<8)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define FREE     0
#define MAX_HANDLES 256
#define PIPE_BUFFER_SIZE 1024

typedef struct pipe_buffer {
    char buffer[PIPE_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int ref_count;
} pipe_buffer_t;

typedef struct {
    uint32_t pos;
    uint32_t fd;
    uint32_t flags;
    char error;
    Dnode* node;
    pipe_buffer_t* pipe; // Буфер для anonymous pipes
} FILE_HANDLE;

typedef struct {
    char name[MAX_FILENAME_LENGTH];
    uint32_t size;
    Dnode* node;
} FILE_QUERY;

typedef struct {
    char name[MAX_FILENAME_LENGTH];
    uint8_t type;
    uint32_t size;
} vfs_dirent_t;

typedef struct {
    uint8_t type;
    uint32_t size;
    uint32_t permissions;
    uint32_t change_time;
} vfs_stat_t;

extern FILE_HANDLE* handles;
extern Dnode* fs_root;

void    kfinit(Dnode* root);
void    ksetiocolor(uint8_t in_bg, uint8_t in_fg, uint8_t out_bg, uint8_t out_fg);
void    ksetioflags(uint8_t in_f);
int32_t kopen(const char* path, char flags);
int32_t kopen_at(Dnode* cwd, const char* path, char flags);
int32_t kwrite(int32_t fd, const char* data, uint32_t size);
int32_t kread(int32_t fd, char* source, uint32_t size);
int32_t kseek(int32_t fd, int32_t offset, int whence);
int32_t kclose(int32_t fd);
void    kquery(int32_t fd, FILE_QUERY* query);
int32_t kreaddir(int32_t fd, uint32_t index, vfs_dirent_t* entry);
int32_t kmkdir(Dnode* cwd, const char* path);
int32_t kunlink(Dnode* cwd, const char* path);
int32_t kstat(Dnode* cwd, const char* path, vfs_stat_t* st);
int32_t kdup2(int32_t oldfd, int32_t newfd);
int32_t kcreate_pipe(int32_t pipefd[2]);

#endif