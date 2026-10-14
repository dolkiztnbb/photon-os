#ifndef KGR
#define KGR

#include <kdef.h>
#include <../graphics.h>
#include <../../drivers/screen.h>

void SetPixel(const Pixel_t* p);
void DR(const Rect_t* r);

void Print(const Text_t* r);
void PrintAt(const Text_t* r);

#endif