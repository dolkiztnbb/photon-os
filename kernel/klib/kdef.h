#ifndef KDEF
#define KDEF

#include <../../libc/stdint>
#include <../../drivers/screen.h>

typedef struct{
    uint32_t x;
    uint32_t y;
} Point_t;

typedef struct{
    uint8_t R;
    uint8_t G;
    uint8_t B;
} Color_t;

typedef struct{
    uint8_t h;
    uint8_t s;
    uint8_t v;
} HSV_t;

typedef struct{
    Point_t point;
    Color_t color;
} Pixel_t;

typedef struct{
    char* texture;
    size_t size;
} Texture_t;

typedef struct{
    Point_t start;
    Point_t size;
    char attr;
    Color_t color;
    Texture_t texture; 
} Rect_t;

typedef struct{
    char font[MAXCHAR][MAXSYMX][MAXSYMY];
    char* msg;
    Color_t color;
    char attr;
    Point_t start;
} Text_t;


#endif
