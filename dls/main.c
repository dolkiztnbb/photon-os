asm("jmp main");
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

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
    char* VGA = (char*)0Xb8000;
    char s[] = "G00DBYE 0V3RW0RLD\0";
    for(uint32_t i=0;s[i];i++){
        VGA[i*2+164]=s[i];
        VGA[i*2+165]=0x1f;
    }
    return 0x69;
}  
