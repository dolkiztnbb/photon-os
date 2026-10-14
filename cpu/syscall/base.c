#include <base.h>
#include <console.h>
#include <graph.h>

void init_syscall(const int n){
    set_idt_gate(n,callback);
}

void console_i(const registers_t* r);

void callback(const registers_t* r){
    switch (r->eax)
    {
    case CONSOLE_I:
        console_i(r);
        break;
    case GRAPH_I:
        graph_i(r);
        break;
    default:
        break;
    }
}