
asm("jmp main");
#define PRINT_BUFFER_SIZE 256
static char print_buffer[PRINT_BUFFER_SIZE];
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
int vsnprintf(char *buffer, size_t size, const char *format, va_list args) {
    char *ptr = buffer;
    char *end = buffer + size - 1; // Leave space for null terminator
    const char *fmt_ptr = format;
    
    if (size == 0) return 0;
    
    while (*fmt_ptr && ptr < end) {
        if (*fmt_ptr != '%') {
            *ptr++ = *fmt_ptr++;
            continue;
        }
        
        fmt_ptr++; // Skip '%'
        
        if (ptr >= end) break;
        
        switch (*fmt_ptr) {
            case 'd': {
                int value = va_arg(args, int);
                // Handle negative numbers
                if (value < 0 && ptr < end) {
                    *ptr++ = '-';
                    value = -value;
                }
                
                // Convert to string (reverse order)
                char temp[12]; // Enough for 32-bit int
                int len = 0;
                if (value == 0) {
                    temp[len++] = '0';
                } else {
                    while (value > 0 && len < 11) {
                        temp[len++] = '0' + (value % 10);
                        value /= 10;
                    }
                }
                
                // Copy in correct order
                while (len > 0 && ptr < end) {
                    *ptr++ = temp[--len];
                }
                break;
            }
            
            case 'x':
            case 'X': {
                unsigned int value = va_arg(args, unsigned int);
                char temp[9]; // 8 hex digits + null
                int len = 0;
                
                if (value == 0) {
                    temp[len++] = '0';
                } else {
                    while (value > 0 && len < 8) {
                        int digit = value % 16;
                        if (digit < 10) {
                            temp[len++] = '0' + digit;
                        } else {
                            temp[len++] = (*fmt_ptr == 'x') ? ('a' + digit - 10) : ('A' + digit - 10);
                        }
                        value /= 16;
                    }
                }
                
                // Copy in correct order
                while (len > 0 && ptr < end) {
                    *ptr++ = temp[--len];
                }
                break;
            }
            
            case 's': {
                char *str = va_arg(args, char*);
                if (str) {
                    while (*str && ptr < end) {
                        *ptr++ = *str++;
                    }
                }
                break;
            }
            
            case 'c': {
                char c = (char)va_arg(args, int);
                *ptr++ = c;
                break;
            }
            
            default:
                *ptr++ = *fmt_ptr;
                break;
        }
        fmt_ptr++;
    }
    
    *ptr = '\0'; // Null terminate
    return ptr - buffer;
}
void PRT(uint8_t color, uint8_t flags, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(print_buffer, sizeof(print_buffer), fmt, args);
    va_end(args);

    // Syscall 6: formatted print
    int80(4, (uint32_t)color, flags, (uint32_t)print_buffer);
    return;
}
int open(const char* filename) {
    uint32_t result;
    asm volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(0x0100), "b"(filename)
        :
    );
    return (int)(result & 0xFF); // Clear upper bits, only return low byte
}
int read(int handle, void* buffer, unsigned int count) {
    uint32_t result;
    asm volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(0x0101), "b"(handle), "c"(buffer), "d"(count)
        :
    );
    return (int)(result & 0xFFFF); // Return low word for byte count
}
int seek(int handle, int offset, int whence) {
    uint32_t result;
    asm volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(0x0102), "b"(handle), "c"(offset), "d"(whence)
        :
    );
    return (int)result; // Seek returns full 32-bit position
}
int close(int handle) {
    uint32_t result;
    asm volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(0x0103), "b"(handle)
        :
    );
    return (int)(result & 0xFF); // Clear upper bits
}
int writefile(char* filename, char* data, int count) {
    uint32_t result=0;
	PRT(0xf,2,"%s %d\n",filename,count);
    asm volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(0x0104), "b"(filename), "c"(count), "d"(data)
        :
    );
    return (int)result; // Seek returns full 32-bit position
}

void input(char* buffer, int c){
    int80(5,0,c,buffer);
    return;
}

void con_setp(int x, int y){
    int80(2,x,y,0);
}
void cls(int c){
    con_setp(0,0);
    int80(3,c,0,0);
}
void swrite(int x, int y, char z){
    uint8_t *m = 0xa0000;
    m[(x%320)+(y%200)*320]=z&0xff;
}


#define print(x) int80(4,0x7,2,(uint32_t)x)

char mathff(const int a, const int mi, const int ma){
    return a >= mi && a <= ma;
}

void int_to_ascii(const int n, char str[]);
void hex_to_ascii(const int n, char str[]);
void h2c(const int n, char str[]);
void reverse(char s[]);
int strlen(const char s[]);
void backspace(char s[]);
void append(char s[], const char n);
int strcmp(const char s1[],const  char s2[]);
void cut(char s[], const int from, const int to);
void clear(char s[],int length);
char* cutr(char s[], const int from, const int to);
void split(const char s[], char buf[], const char token, const int count);  // count - по-счёту, не idx
int splitidx(const char s[], const char token, const int count);         // получение начала токена по счёту count

int strncmp(const char* str1, const char* str2, const uint32_t n);

//true if str has 1+ char from target
char subsymbolac(const char str[], const char target);
char subsymbolaa(const char str[], const char target[]);
char subsymbolca(const char str, const char target[]);

char toSmall(const char c);

char* strncpy(char* destination, const char* source, const uint32_t num);

void int_to_ascii(const int n1, char str[]) {
    int n=n1;
    int i, sign;
    if ((sign = n) < 0) n = -n;
    i = 0;
    do {
        str[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);

    if (sign < 0) str[i++] = '-';
    str[i] = '\0';

    reverse(str);
}

void hex_to_ascii(const int n, char str[]) {
    append(str, '0');
    append(str, 'X');
    char zeros = 0;
    //int n=n1
    int32_t tmp;
    int i;
    for (i = 28; i > 0; i -= 4) {
        tmp = (n >> i) & 0xF;
        if (tmp == 0 && zeros == 0) continue;
        zeros = 1;
        if (tmp > 0xA) {
            if((tmp - 0xA + 'A')==':')append(str,'A');
            else append(str, tmp - 0xA + 'A');
        }
        else append(str, tmp + '0');
    }

    tmp = n & 0xF;
    if (tmp >= 0xA)if((tmp - 0xA + 'A')==':')append(str,'A');
            else append(str, tmp - 0xA + 'A');
    else append(str, tmp + '0');
}

void float_to_ascii(double x, int nn, char s[]){
    size_t n = 10;
    size_t j = 0;
    size_t tl = 0;
    for(int i=0;i<(
        0>nn?0:nn>8?8:nn
    );i++)n*=10;
    char this[40];
    char tmp[20];
    clear(this,40);
    clear(tmp,20);
    int_to_ascii((size_t)(x),tmp);
    for(int i=0;i<strlen(tmp);i++){
        append(this,tmp[i]);
        tl++;
    }
    append(this,'.');
    tl++;
    j = x*n-(size_t)(x)*n;
    if(j<0)j=-j;
    clear(tmp,20);
    int_to_ascii(j,tmp);
    for(int i=0;i<strlen(tmp);i++){
        append(this,tmp[i]);
        tl++;
    }
    strncpy(s,this,tl);
}

void b2c(const int n, char str[], const int size){
	int i=0;
    for(i=0;i<size;i++)
    {
        str[i]='0'+(n>>(size-i-1)&1);
    }
}

void h2c(const int n, char str[]) {
    //append(str, '0');
    //append(str, 'X');
    char zeros = 0;

    int32_t tmp;
    int i;
    for (i = 28; i > 0; i -= 4) {
        tmp = (n >> i) & 0xF;
        if (tmp == 0 && zeros == 0) continue;
        zeros = 1;
        
        if (tmp > 0xA) {
            if((tmp - 0xA + 'A')==':')append(str,'A');
            else append(str, tmp - 0xA + 'A');
        }
        else append(str, tmp + '0');
    }

    tmp = n & 0xF;
    if (tmp >= 0xA) append(str, tmp - 0xA + 'A');
    else append(str, tmp + '0');
}



/* K&R */
void reverse(char s[]) {
    int c, i, j;
    for (i = 0, j = strlen(s)-1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

// cuts an array 
// [from, to]
void cut(char s[], const int f, const int t){
    int from=f;
    int to=t;
    char ns[to-from];int i;
    if(to<from){ //xor swap :3
        from^=to;
        to^=from;
        from^=to;
    }
    for(i=from;i<to;i++){
        ns[i-to]=s[i];
    }
    s=ns;
}

void clear(char s[],int length){
    for(int i=0;i<length;i++)s[i]='\0';
}

void split(const char s[], char buf[], const char token, const int count) {
    int i = 0;
    int offset = 0;
    int token_count = 0;
    while (s[i + offset] != '\0') {
        if (s[i + offset] == token) {
            token_count++;
            if (token_count == count){
                buf[i] = '\0';
                return;
            }
            offset += i + 1;
            i = 0;
        } else {
            buf[i] = s[i + offset];
            i++;
        }
    }
    buf[i] = '\0';
}
int splitidx(const char s[], const char token, const int count){
    int i = 0;
    int offset = 0;
    int token_count = 0;
    while (s[i + offset] != '\0') {
        if (s[i + offset] == token) {
            token_count++;
            if (token_count == count){
                return offset;
            }
            offset += i + 1;
            i = 0;
        } else {
            i++;
        }
    }
    return offset;
}


/* K&R */
int strlen(const char s[]) {
    int i = 0;
    while (s[i] != '\0') ++i;
    return i;
}

void append(char s[], const char n) {
    int len = strlen(s);
    s[len] = n;
    s[len+1] = '\0';
}

void backspace(char s[]) {
    int len = strlen(s);
    s[len-1] = '\0';
}

/* K&R 
 * Returns <0 if s1<s2, 0 if s1==s2, >0 if s1>s2 */
int strcmp(const char s1[],const char s2[]) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;
    }
    return s1[i] - s2[i];
}

int strncmp(const char* str1, const char* str2, const uint32_t n)
{
    const char* end = str1+n;
    int result = 0;
    for(;result == 0 && str1 != end && (*str1 || *str2); result = *(str1++)-*(str2++));
    return result;
}

char subsymbolac(const char str[], const char target){
    for(int i=0;str[i]!='\0';i++){
        if(str[i]==target) return 1;
    }
    return 0;
}

char subsymbolaa(const char str[], const char target[]){
    for(int i=0;str[i]!='\0';i++){
        for(int j=0; target[j]!='\0';j++){
            if(str[i]==target[j]) return 1;
        }
    }
    return 0;
}

char subsymbolca(const char str, const char target[]){
    for(int j=0; target[j]!='\0';j++){
        if(str==target[j]) return 1;
    }
    return 0;
}

//privet NONOONONONO. not privet yes private

const static char bigToSmall[] = {
    ['!']='1',
    ['@']='2',
    ['#']='3',
    ['$']='4',
    ['%']='5',
    ['^']='6',
    ['&']='7',
    ['*']='8',
    ['(']='9',
    [')']='0',
    ['_']='-',
    ['+']='=',
    ['>']='.',
    ['<']=','
};

const static char big[] = "!@#$%^&*()_+><\0";

// '\0' if cant
char toSmall(const char c1) {
    char c=c1;
    if (mathff(c, 0x41, 0x5d)) {
        c += 0x20;
        return c;
    }
    else if (bigToSmall[c]!='\0') {
        c = bigToSmall[c];
        return c;
    }
    else return c;
}

char* strncpy(char* destination, const char* source, uint32_t num) {
  uint32_t i = 0;                           // начинаем копирование с начала
  while (source[i] != '\0' && i < num) {  // копируем пока не упремся в \0 или
                                          // не скопируем нужно количество байт
    destination[i] = source[i];           // копируем байт
    i++;                                  // двигаемся вперед
  }

  destination[i] = '\0';  // нужно не забыть пометить конец строки

  return destination;
}
int atoi(const char* str) {
    if (str == NULL) {
        return 0; // Если строка пустая или NULL, возвращаем 0
    }

    int result = 0; // Результат
    int sign = 1;   // Знак числа (1 для положительного, -1 для отрицательного)
    int i = 0;      // Индекс символа

    // Пропускаем пробельные символы
    while (str[i] == ' ') {
        i++;
    }

    // Проверяем знак числа
    if (str[i] == '-') {
        sign = -1;
        i++;
    } else if (str[i] == '+') {
        i++;
    }

    // Проверяем на наличие префикса 0x или 0X для шестнадцатеричных чисел
    if (str[i] == '0' && (str[i + 1] == 'x' || str[i + 1] == 'X')) {
        i += 2; // Пропускаем '0x' или '0X'

        // Преобразуем шестнадцатеричные цифры
        while (1) {
            char c = str[i];

            if (c >= '0' && c <= '9') {
                result = result * 16 + (c - '0');
            } else if (c >= 'a' && c <= 'f') {
                result = result * 16 + (c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                result = result * 16 + (c - 'A' + 10);
            } else {
                break; // Не шестнадцатеричный символ — выходим
            }
            i++;
        }

        return result * sign;
    }

    // Иначе считаем, что это десятичное число
    while (str[i] >= '0' && str[i] <= '9') {
        result = result * 10 + (str[i] - '0'); // Добавляем цифру к результату
        i++;
    }

    return result * sign; // Возвращаем число с %учетом знака
}

char* strncat(char* destination, const char* source) {
	int len = strlen(destination);

	for (int i = 0; source[i] != '\0'; i++) {
		destination[len + i] = source[i];
	}

	destination[len + strlen(source)] = '\0';

	return destination;
}

void wtc(){
    int80(7,0,0,0);
}

int getch() {

    uint32_t result;
    asm volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(0x0008)
        :
    );
    return (int)result; // Seek returns full 32-bit position
}




// Directory entry structure
#define MAX_DIR_ENTRIES 64
#define DIR_ENTRY_NAME_LEN 16

struct dir_entry {
    char name[DIR_ENTRY_NAME_LEN];
    unsigned long long size;  // Size in bytes
    unsigned long long sectors; // Size in sectors
    unsigned char attributes;
};

struct dir_entry dir_entries[MAX_DIR_ENTRIES];
int dir_entry_count = 0;

// Directory listing syscall
int listdir(struct dir_entry *buffer, int max_entries) {
    uint32_t result;
    asm volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(0x0200), "b"(buffer), "c"(max_entries)
        : "memory"
    );
    return (int)(result & 0xFFFF);
}

void memory_copy(const void *source, void *dest, const int nbytes) {
    int i;
    for (i = 0; i < nbytes; i++) {
        *(char*)(dest + i) = *(char*)(source + i);
    }
}

void memory_set(void *dest, const uint8_t val, const uint32_t len) {
    uint8_t *temp = (uint8_t *)dest;
    for (int i = len ; i != 0; i--) *temp++ = val;
}

char memory_compare(void* x, void* y, const int len) {
    unsigned char* a = (unsigned char*)x;
    unsigned char* b = (unsigned char*)y;
    
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return 1; // Возвращаем 1 при первом несовпадении
        }
    }
    return 0; // Возвращаем 0, если все байты совпадают
}

#define memcpy memory_copy
#define memset memory_set
#define memcmp memory_compare
