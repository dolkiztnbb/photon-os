unsigned char mouse[] = {
    0b11000000,
    0b10100000,
    0b10010000,
    0b10001000,
    0b10001100,
    0b11110000,
    0b00000000,
    0b00000000,
    
};



void drawicon(unsigned char icon[],int x_pos,int y_pos,unsigned char color,unsigned char istransparent){
    if(istransparent){
        for(int y=0;y<8;y++){
            for(int x=0;x<8;x++){
                unsigned char i = (icon[y]>>(uint8_t)(7-x))&1;
                if(i){
                    swrite(x_pos+x,y_pos+y,color);
                }
            }
        }
    } else {
        for(int y=0;y<8;y++){
            for(int x=0;x<8;x++){
                unsigned char i = (icon[y]>>(uint8_t)(7-x))&1;
                if(i){
                    swrite(x_pos+x,y_pos+y,color);
                } else {
                    swrite(x_pos+x,y_pos+y,0);
                }
            }
        }
    }

}