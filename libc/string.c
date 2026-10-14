#include <libc/string.h>
#include <libc/mem.h>
#include <kernel/math.h>
#include <stddef.h>
#include <stdarg.h>
/**
 * K&R implementation
 */
void int_to_ascii(const int n1, char str[]) {
    int n = n1;
    int i = 0, sign;
    if ((sign = n) < 0) {
        n = -n;
    }
    do {
        str[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);
    if (sign < 0) {
        str[i++] = '-';
    }
    str[i] = '\0';
    reverse(str);
}

void hex_to_ascii(const int n, char str[]) {
    append(str, '0');
    append(str, 'x');
    char zeros = 0;
    //int n=n1
    int32_t tmp;
    int i;
    for (i = 28; i > 0; i -= 4) {
        tmp = (n >> i) & 0xF;
        if (tmp == 0 && zeros == 0) continue;
        zeros = 1;
        if (tmp >= 0xA) {
            append(str, tmp - 0xA + 'A');
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
    //int n=n1
    int32_t tmp;
    int i;
    for (i = 28; i > 0; i -= 4) {
        tmp = (n >> i) & 0xF;
        if (tmp == 0 && zeros == 0) continue;
        zeros = 1;
        if (tmp >= 0xA) {
            append(str, tmp - 0xA + 'A');
        }
        else append(str, tmp + '0');
    }

    tmp = n & 0xF;
    if (tmp >= 0xA)if((tmp - 0xA + 'A')==':')append(str,'A');
            else append(str, tmp - 0xA + 'A');
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
    for(int i=0;i<length;i++)
        s[i]='\0';
}

void split(const char s[], char buf[], const char token, const int cnt) {
    if (!s || !buf) return;

    int current_token = 0;
    int i = 0;
    int j = 0;
    buf[0] = '\0';

    int count = cnt - 1;
    if (count < 0) return;

    while (s[i] != '\0') {
        while (s[i] != '\0' && s[i] == token) {
            i++;
        }

        if (s[i] == '\0') break;

        if (current_token == count) {
            // Защита от переполнения 64-байтных буферов ядра (max 63 символа + '\0')
            while (s[i] != '\0' && s[i] != token) {
                if (j < 63) {
                    buf[j++] = s[i];
                }
                i++;
            }
            buf[j] = '\0';
            return;
        }

        while (s[i] != '\0' && s[i] != token) {
            i++;
        }

        current_token++;
    }
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


void append_string(char s[], char s2[]) {
    int len = strlen(s);
    int i;
    for (i = 0; s2[i] != '\0'; i++) {
        s[len + i] = s2[i];
    }
    s[len + i] = '\0';
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

const static char bigToSmall[128] = {
    ['!'] = '1',
    ['@'] = '2',
    ['#'] = '3',
    ['$'] = '4',
    ['%'] = '5',
    ['^'] = '6',
    ['&'] = '7',
    ['*'] = '8',
    ['('] = '9',
    [')'] = '0',
    ['_'] = '-',    
    ['+'] = '=',
    ['~'] = '`',
    ['{'] = '[',
    ['}'] = ']',
    ['|'] = '\\',
    [':'] = ';',
    ['"'] = '\'',
    ['<'] = ',',
    ['>'] = '.',
    ['?'] = '/'
};

char toSmall(const char c1) {
    // 1. Буквы A-Z — СРАЗУ возвращаем result и выходим из функции!
    if (c1 >= 'A' && c1 <= 'Z') {
        return c1 + 0x20;
    }

    // 2. Спецсимволы
    unsigned char uc = (unsigned char)c1;
    if (uc < 128 && bigToSmall[uc] != '\0') {
        return bigToSmall[uc];
    }

    // 3. Остальное
    return c1;
}
char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    do {
        if (*s == (char)c) {
            last = s;
        }
    } while (*s++);

    (void)last;
    return (char *)last;
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

    return result * sign; // Возвращаем число с учетом знака
}

char* strncat(char* destination, const char* source) {
	int len = strlen(destination);

	for (int i = 0; source[i] != '\0'; i++) {
		destination[len + i] = source[i];
	}

	destination[len + strlen(source)] = '\0';

	return destination;
}

/**
 * Переводит число в строку с заданным основанием и минимальной шириной.
 * @param value Число для перевода
 * @param base Основание (10 для d, 16 для x)
 * @param uppercase Использовать ли заглавные буквы (для %X)
 * @param width Минимальная ширина (заполнение нулями)
 * @param out_buf Буфер для записи результата
 */
void base_to_string(long value, int base, int uppercase, int width, char *out_buf) {
    char buffer[64];
    int i = 0;
    int is_negative = 0;

    // Обработка отрицательных чисел для десятичной системы
    if (value < 0 && base == 10) {
        is_negative = 1;
        value = -value;
    } else if (value == 0) {
        buffer[i++] = '0';
    }

    // Основной цикл конвертации
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    while (value > 0) {
        buffer[i++] = digits[value % base];
        value /= base;
    }

    // Добавляем нули для соблюдения ширины (учитываем знак минуса)
    int current_len = i + is_negative;
    while (current_len < width && i < 63) {
        buffer[i++] = '0';
        current_len++;
    }

    if (is_negative) {
        buffer[i++] = '-';
    }

    // Переворачиваем строку в результирующий буфер
    int out_idx = 0;
    for (int j = i - 1; j >= 0; j--) {
        out_buf[out_idx++] = buffer[j];
    }
    out_buf[out_idx] = '\0';
}

static inline int is_space(char c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f');
}

static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline int is_digit(char c) {
    return (c >= '0' && c <= '9');
}

int vsscanf(const char *str, const char *format, va_list args) {
    int assigned = 0;

    while (*format && *str) {
        // Пропускаем пробельные символы в формате
        if (is_space(*format)) {
            while (is_space(*format)) format++;
            while (is_space(*str)) str++;
            continue;
        }

        // Сопоставление обычных символов
        if (*format != '%') {
            if (*str != *format) break;
            str++;
            format++;
            continue;
        }

        format++; // Пропускаем '%'

        if (*format == '%') {
            if (*str != '%') break;
            str++;
            format++;
            continue;
        }

        // Автоматический пропуск ведущих пробелов в строке для большинства спецификаторов
        if (*format != 'c') {
            while (is_space(*str)) str++;
        }

        switch (*format) {
            case 'x':
            case 'X': {
                unsigned int *out = va_arg(args, unsigned int *);
                if (!*str) goto done;

                // Пропуск префикса 0x или 0X, если за ним следуют шестнадцатеричные цифры
                if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X') && hex_val(str[2]) != -1) {
                    str += 2;
                }

                unsigned int val = 0;
                int digits = 0;

                while (*str) {
                    int v = hex_val(*str);
                    if (v == -1) break;
                    val = (val << 4) | (unsigned int)v;
                    digits++;
                    str++;
                }

                if (digits == 0) goto done;

                *out = val;
                assigned++;
                break;
            }

            case 'd': {
                int *out = va_arg(args, int *);
                if (!*str) goto done;

                int sign = 1;
                if (*str == '-') {
                    sign = -1;
                    str++;
                } else if (*str == '+') {
                    str++;
                }

                int digits = 0;
                int val = 0;

                while (is_digit(*str)) {
                    val = val * 10 + (*str - '0');
                    digits++;
                    str++;
                }

                if (digits == 0) goto done;

                *out = val * sign;
                assigned++;
                break;
            }

            case 'u': {
                unsigned int *out = va_arg(args, unsigned int *);
                if (!*str) goto done;

                int digits = 0;
                unsigned int val = 0;

                while (is_digit(*str)) {
                    val = val * 10 + (unsigned int)(*str - '0');
                    digits++;
                    str++;
                }

                if (digits == 0) goto done;

                *out = val;
                assigned++;
                break;
            }

            case 's': {
                char *out = va_arg(args, char *);
                if (!*str) goto done;

                int count = 0;
                while (*str && !is_space(*str)) {
                    *out++ = *str++;
                    count++;
                }
                *out = '\0';

                if (count == 0) goto done;

                assigned++;
                break;
            }

            case 'c': {
                char *out = va_arg(args, char *);
                if (!*str) goto done;

                *out = *str++;
                assigned++;
                break;
            }

            default:
                goto done;
        }

        format++;
    }

done:
    return assigned;
}

// Обертка для удобного вызова sscanf(...)
int sscanf(const char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vsscanf(str, format, args);
    va_end(args);
    return ret;
}