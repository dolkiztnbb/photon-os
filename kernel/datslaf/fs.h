#ifndef __DATSLAF__
#define __DATSLAF__
#include <libc/stdint>
#include <libc/mem.h>

#define TYPE_FILE 0
#define TYPE_DIR  1

#define MAX_FILENAME_LENGTH 32

typedef struct Dnode Dnode;
typedef struct DnodeVector DnodeVector;

struct Dnode {
    uint8_t type;
    uint32_t (*read)(Dnode* node, char* buffer, uint32_t length, uint32_t offset);
    uint32_t (*write)(Dnode* node, char* buffer, uint32_t length, uint32_t offset);
    char name[MAX_FILENAME_LENGTH];
    char* data;
    uint32_t change_time;
    uint32_t size;
    uint32_t permissions;
    DnodeVector* children;
    Dnode* parent;

    int32_t id;
    uint32_t data_block;
};

struct DnodeVector {
    struct Dnode** items;
    uint32_t capacity;
    uint32_t size;
} __attribute__((packed));

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t total_nodes;
    uint32_t data_block_start;
} disk_superblock_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    char     name[MAX_FILENAME_LENGTH];
    uint32_t size;
    uint32_t change_time;
    uint32_t permissions;
    int32_t  parent_id;
    uint32_t data_block;
    uint8_t  reserved[11];
} disk_dnode_t;

void        vector_init(DnodeVector* v, uint32_t initial_capacity);
void        vector_add(DnodeVector* v, Dnode* node);
void        vector_free(DnodeVector* v);
Dnode*      vector_get(DnodeVector* v, uint32_t index);
void        dnode_remove_child(Dnode* parent, Dnode* child);

Dnode*      create_node(const char* name, uint8_t type);
void        dnode_add_child(Dnode* parent, Dnode* child);
Dnode*      dnode_find_child(Dnode* parent, const char* name);
void        dnode_get_path(Dnode* node, char* buffer);
Dnode*      dnode_find_by_path(Dnode* root, Dnode* cwd, const char* path);
Dnode*      dnode_create_at_path(Dnode* root, Dnode* cwd, const char* full_path, uint8_t type);

uint32_t    dnode_read(Dnode* node, char* buffer, uint32_t length, uint32_t offset);    
uint32_t    dnode_write(Dnode* node, char* buffer, uint32_t length, uint32_t offset);

void        fs_save_to_disk(Dnode* root);
void        fs_load_from_disk(Dnode** node);
void        fs_delete_node(Dnode* node); 
void        fs_flush_node(Dnode* node);

#define DAC dnode_add_child
#define DFC dnode_find_child
#define DNC create_node

#endif