#include <../isr.h>
#include <../../kernel/klib/kgr.h>
#include <../../kernel/klib/kdef.h>

#define CONSOLE_I 0
#define GRAPH_I 1


void callback(const registers_t* r);

extern void init_syscall(const int n);
