#include <kgr.h>
#include <../graphics.h>


void SetPixel(const Pixel_t* p){
    swrite(p->point.x, p->point.y, rgb2c(&p->color));
}

void DR(const Rect_t* r){
    if(r->attr&1){
        tex_rect(r->start.x, r->start.y, r->size.x, r->size.y, r->texture.texture, r->texture.size);
    }
    else{
        rect(r->start.x, r->start.y, r->size.x, r->size.y, rgb2c(&r->color));
    }
}

void Print(const Text_t* r){
    print(r->font, r->msg, rgb2c(&r->color), r->attr);
}

void PrintAt(const Text_t* r){
    print_at(r->font, r->msg, r->start.x, r->start.y, rgb2c(&r->color), r->attr);
}