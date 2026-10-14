#include "fs.h"
#include <cpu/malloc/kmalloc.h>
#include <libc/string.h>
#include <drivers/screen.h>
#include <kernel/debug.h>

void vector_init(DnodeVector* v, uint32_t initial_capacity) {
    if (!v) return;
    v->capacity = initial_capacity ? initial_capacity : 4;
    v->size = 0;
    v->items = (Dnode**)kmalloc(sizeof(Dnode*) * v->capacity);
}

void vector_add(DnodeVector* v, Dnode* node) {
    if (!v || !node) return;
    if (v->size == v->capacity) {
        uint32_t new_capacity = v->capacity * 2;
        Dnode** new_items = (Dnode**)kmalloc(sizeof(Dnode*) * new_capacity);
        if (!new_items) return;
        
        for (uint32_t i = 0; i < v->size; i++) {
            new_items[i] = v->items[i];
        }
        
        kfree(v->items);
        v->items = new_items;
        v->capacity = new_capacity;
    }
    v->items[v->size++] = node;
}

Dnode* vector_get(DnodeVector* v, uint32_t index) {
    if (!v || !v->items || index >= v->size) {
        return 0;
    }
    return v->items[index];
}

void vector_free(DnodeVector* v) {
    if (!v) return;
    if (v->items) {
        kfree(v->items);
        v->items = 0;
    }
    v->size = 0;
    v->capacity = 0;
}

void dnode_remove_child(Dnode* parent, Dnode* child) {
    if (!parent || !parent->children || !child) return;
    DnodeVector* v = parent->children;
    int found_idx = -1;
    for (uint32_t i = 0; i < v->size; i++) {
        if (v->items[i] == child) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx != -1) {
        for (uint32_t i = (uint32_t)found_idx; i < v->size - 1; i++) {
            v->items[i] = v->items[i + 1];
        }
        v->size--;
    }
}

void dnode_add_child(Dnode* parent, Dnode* child) {
    if (!parent || !child || parent->type != TYPE_DIR) return;

    if (parent->children == 0) {
        parent->children = (DnodeVector*)kmalloc(sizeof(DnodeVector));
        if (!parent->children) return;
        vector_init(parent->children, 4);
    }

    child->parent = parent;
    vector_add(parent->children, child);
}

Dnode* create_node(const char* name, uint8_t type) {
    if (!name) return NULL;
    Dnode* node = (Dnode*)kmalloc(sizeof(Dnode));
    if (!node) return NULL;

    memset(node, 0, sizeof(Dnode));
    node->id = -1;
    node->data_block = 0;
    
    int i = 0;
    for(; i < 31 && name[i] != '\0'; i++) {
        node->name[i] = name[i];
    }
    node->name[i] = '\0';
    
    node->type = type;
    if (type == TYPE_DIR) {
        node->children = (DnodeVector*)kmalloc(sizeof(DnodeVector));
        if (node->children) {
            vector_init(node->children, 4);
        }
    }
    return node;
}

Dnode* dnode_find_child(Dnode* parent, const char* name) {
    if (!parent || !name || parent->type != TYPE_DIR || !parent->children) return 0;
    if (strcmp(name, "..") == 0) return parent->parent;
    if (strcmp(name, ".") == 0) return parent;

    for (uint32_t i = 0; i < parent->children->size; i++) {
        Dnode* child = vector_get(parent->children, i);
        if (child && strcmp(name, child->name) == 0) {
            return child;
        }
    }
    return 0;
}

void dnode_get_path(Dnode* node, char* buffer) {
    if (!node || !buffer) return;
    if (node->parent != node && node->parent != 0) {
        dnode_get_path(node->parent, buffer);
    }
    strncat(buffer, node->name);
    if (node->type == TYPE_DIR) {
        strncat(buffer, "/");
    }
}

Dnode* dnode_find_by_path(Dnode* root, Dnode* cwd, const char* path) {
    if (!path || path[0] == '\0') return 0;
    Dnode* current = (path[0] == '/') ? root : (cwd ? cwd : root);
    if (!current) return 0;

    const char* ptr = path;
    while (*ptr != '\0') {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        char segment[32];
        int idx = 0;
        while (*ptr != '\0' && *ptr != '/' && idx < (sizeof(segment) - 1)) {
            segment[idx++] = *ptr++;
        }
        segment[idx] = '\0';
        while (*ptr != '\0' && *ptr != '/') ptr++;

        if (strcmp(segment, ".") == 0) continue;
        if (strcmp(segment, "..") == 0) {
            if (current->parent) current = current->parent;
            continue;
        }

        if (current->type != TYPE_DIR || !current->children) return 0;

        Dnode* found = 0;
        for (uint32_t i = 0; i < current->children->size; i++) {
            Dnode* child = vector_get(current->children, i);
            if (child && strcmp(child->name, segment) == 0) {
                found = child;
                break;
            }
        }
        if (!found) return 0;
        current = found;
    }
    return current;
}

Dnode* dnode_create_at_path(Dnode* root, Dnode* cwd, const char* full_path, uint8_t type) {
    if (!full_path || full_path[0] == '\0') return 0;

    const char* last_slash = strrchr(full_path, '/');
    char parent_path[128] = {0};
    char name[32] = {0};

    if (last_slash == 0) {
        strncpy(name, full_path, sizeof(name) - 1);
    } else if (last_slash == full_path) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        strncpy(name, last_slash + 1, sizeof(name) - 1);
    } else {
        size_t dir_len = last_slash - full_path;
        if (dir_len >= sizeof(parent_path)) dir_len = sizeof(parent_path) - 1;
        strncpy(parent_path, full_path, dir_len);
        strncpy(name, last_slash + 1, sizeof(name) - 1);
    }

    if (strlen(name) == 0) return 0;

    Dnode* parent = (parent_path[0] == '\0') ? (cwd ? cwd : root)
                                             : dnode_find_by_path(root, cwd, parent_path);

    if (!parent || parent->type != TYPE_DIR) return 0;

    Dnode* new_node = create_node(name, type);
    if (!new_node) return 0;
    
    dnode_add_child(parent, new_node);
    return new_node;
}