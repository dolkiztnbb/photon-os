void console_i(const registers_t* r){
    switch (r->ebx)
    {
    case 0:
       Print((Text_t*)r->ecx);
        break;
    case 1:
        PrintAt((Text_t*)r->ecx);
        break;
    case 2:
        //ReadPixel((Pixel_t*)r->ecx);
        break;
    default:
        break;
    }
}