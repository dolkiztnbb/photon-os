
void graph_i(const registers_t* r){
    switch (r->ebx)
    {
    case 0:
        SetPixel((Pixel_t*)r->ecx);
        break;
    case 1:
        DR((Rect_t*)r->ecx);
        break;
    case 2:
        //ReadPixel((Pixel_t*)r->ecx);
        break;
    default:
        break;
    }
}