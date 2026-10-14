#include <stdint.h>

uint32_t int80(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    uint32_t result;
    asm volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(eax), "b"(ebx), "c"(ecx), "d"(edx)
        : "memory"
    );
    return result;
}

int main(){
    int80(4,0x7,2,(uint32_t)"Init loaded\n\n\0");
    int80(4,0x9,2,(uint32_t)"PhotonOS!\n\0");
    int80(4,0x9,2,(uint32_t)"PhotonOS!\n\0");
}