#include <la.h>
#include <stdint.h>
#ifndef EXTENDED_KERNEL
#   define EXTENDED_KERNEL
#endif
uint8_t color = 0;

#define idx uint32_t

volatile uint8_t* realVMEM = 0xa0000;
volatile uint8_t * VMEM = ((volatile uint8_t *)0xA0000); // Volatile keyword recommended for memory-mapped I/O
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200

#define abs(a) ((a)<0?(-(a)):(a))

/*typedef struct {
    float x, y, z;
} Vertex3D;*/
V3f cam_pos = {0, 0, 0};
V3f cam_rot = {0, 0, 0};
float cam_fov = 90.0f;

typedef V3f(filter)(V2f pos, V3f color);
filter *shader = NULL;

EXTENDED_KERNEL void ksetup_gm(){
    
    __asm__ volatile ("cli");

    VMEM = kmalloc(320*200); // this shit lowk sucks

    // *** Attribute Controller Registers (Port 0x3C0) ***
    // Before accessing the Attribute Controller, you must read from 0x3DA
    // to reset the internal flip-flop that determines whether the next write to 0x3C0
    // is an index or a value.
    unsigned char temp = inb(0x3DA); // Reset flip-flop to INDEX state

    // Now write the index, then the value for each pair
    outb(0x3C0, 0x00); outb(0x3C0, 0x0); // Palette Register 00
    outb(0x3C0, 0x1); outb(0x3C0, 0x0); // Palette Register 01
    outb(0x3C0, 0x2); outb(0x3C0, 0x0); // Palette Register 02
    outb(0x3C0, 0x3); outb(0x3C0, 0x0); // Palette Register 03
    outb(0x3C0, 0x4); outb(0x3C0, 0x0); // Palette Register 04
    outb(0x3C0, 0x5); outb(0x3C0, 0x0); // Palette Register 05
    outb(0x3C0, 0x6); outb(0x3C0, 0x0); // Palette Register 06
    outb(0x3C0, 0x7); outb(0x3C0, 0x0); // Palette Register 07
    outb(0x3C0, 0x8); outb(0x3C0, 0x0); // Palette Register 08
    outb(0x3C0, 0x9); outb(0x3C0, 0x0); // Palette Register 09
    outb(0x3C0, 0xa); outb(0x3C0, 0x0); // Palette Register 0A
    outb(0x3C0, 0xb); outb(0x3C0, 0x0); // Palette Register 0B
    outb(0x3C0, 0xc); outb(0x3C0, 0x0); // Palette Register 0C
    outb(0x3C0, 0xd); outb(0x3C0, 0x0); // Palette Register 0D
    outb(0x3C0, 0xe); outb(0x3C0, 0x0); // Palette Register 0E
    outb(0x3C0, 0xf); outb(0x3C0, 0x0); // Palette Register 0F
    outb(0x3C0, 0x10); outb(0x3C0, 0x41); // Mode Control Register (0x10)
    outb(0x3C0, 0x11); outb(0x3C0, 0x0); // Overscan Register (0x11)
    outb(0x3C0, 0x12); outb(0x3C0, 0xF); // Color Plane Enable (0x12)
    outb(0x3C0, 0x13); outb(0x3C0, 0x0); // Horizontal Panning (0x13)
    outb(0x3C0, 0x14); outb(0x3C0, 0x1); // Color Select (0x14)

    // Finally, set the Attribute Controller to video enabled state
    // This is often done by writing the index again and setting bit 5
    temp = inb(0x3DA); // Reset flip-flop again
    outb(0x3C0, 0x20); // Write index 0x20 (Enable Attributes) with bit 5 set -> Video Enabled

    // *** Sequencer Registers (Port 0x3C4) ***
    outb(0x3C4, 0x0); outb(0x3C5, 0x3); // Reset Register (0x00)
    outb(0x3C4, 0x1); outb(0x3C5, 0x1); // Clocking Mode Register (0x01)
    outb(0x3C4, 0x2); outb(0x3C5, 0xF); // Enable Register (0x02)
    outb(0x3C4, 0x3); outb(0x3C5, 0x0); // Character Map Select Register (0x03)
    outb(0x3C4, 0x4); outb(0x3C5, 0xE); // Memory Mode Register (0x04)

    // *** Graphics Controller Registers (Port 0x3CE) ***
    outb(0x3CE, 0x0); outb(0x3CF, 0x0); // Set/Reset Register (0x00)
    outb(0x3CE, 0x1); outb(0x3CF, 0x0); // Enable Set/Reset Register (0x01)
    outb(0x3CE, 0x2); outb(0x3CF, 0x0); // Color Compare Register (0x02)
    outb(0x3CE, 0x3); outb(0x3CF, 0x0); // Data Rotate Register (0x03)
    outb(0x3CE, 0x4); outb(0x3CF, 0x0); // Read Map Select Register (0x04)
    outb(0x3CE, 0x5); outb(0x3CF, 0x40); // Graphics Mode Register (0x05)
    outb(0x3CE, 0x6); outb(0x3CF, 0x5); // Miscellaneous Register (0x06)
    outb(0x3CE, 0x7); outb(0x3CF, 0xF); // Color Don't Care Register (0x07)
    outb(0x3CE, 0x8); outb(0x3CF, 0xFF); // Bit Mask Register (0x08)

    // *** CRT Controller Registers (Port 0x3D4) ***
    outb(0x3D4, 0x0); outb(0x3D5, 0x5F); // Horizontal Total (0x00)
    outb(0x3D4, 0x1); outb(0x3D5, 0x4F); // Horizontal Display Enable End (0x01)
    outb(0x3D4, 0x2); outb(0x3D5, 0x50); // Start Horizontal Blanking (0x02)
    outb(0x3D4, 0x3); outb(0x3D5, 0x82); // End Horizontal Blanking (0x03)
    outb(0x3D4, 0x4); outb(0x3D5, 0x54); // Start Horizontal Retrace (0x04)
    outb(0x3D4, 0x5); outb(0x3D5, 0x80); // End Horizontal Retrace (0x05)
    outb(0x3D4, 0x6); outb(0x3D5, 0xBF); // Vertical Total (0x06)
    outb(0x3D4, 0x7); outb(0x3D5, 0x1F); // Overflow (0x07)
    outb(0x3D4, 0x8); outb(0x3D5, 0x0); // Preset Row Scan (0x08)
    outb(0x3D4, 0x9); outb(0x3D5, 0x41); // Maximum Scan Line (0x09)
    outb(0x3D4, 0xA); outb(0x3D5, 0x0); // Cursor Start (0x0A)
    outb(0x3D4, 0xB); outb(0x3D5, 0x0); // Cursor End (0x0B)
    outb(0x3D4, 0xC); outb(0x3D5, 0x0); // Start Address High (0x0C)
    outb(0x3D4, 0xD); outb(0x3D5, 0x0); // Start Address Low (0x0D)
    outb(0x3D4, 0xE); outb(0x3D5, 0x0); // Cursor Position High (0x0E)
    outb(0x3D4, 0xF); outb(0x3D5, 0x0); // Cursor Position Low (0x0F)
    outb(0x3D4, 0x10); outb(0x3D5, 0x9C); // Vertical Retrace Start (0x10)
    outb(0x3D4, 0x11); outb(0x3D5, 0x8E); // Vertical Retrace End (0x11)
    outb(0x3D4, 0x12); outb(0x3D5, 0x8F); // Vertical Display Enable End (0x12)
    outb(0x3D4, 0x13); outb(0x3D5, 0x28); // Offset (0x13)
    outb(0x3D4, 0x14); outb(0x3D5, 0x40); // Underline Location (0x14)
    outb(0x3D4, 0x15); outb(0x3D5, 0x96); // Start Vertical Blanking (0x15)
    outb(0x3D4, 0x16); outb(0x3D5, 0xB9); // End Vertical Blanking (0x16)
    outb(0x3D4, 0x17); outb(0x3D5, 0xA3); // CRT Mode Control (0x17)
    outb(0x3D4, 0x18); outb(0x3D5, 0xFF); // Line Compare (0x18)

    // *** Miscellaneous Output Register (Port 0x3C2) ***
    // This needs to be written *before* setting sequencer reset
    outb(0x3C2, 0x63); // Miscellaneous Output Register

    uint8_t palette_idx = 0;
    for (idx r = 0; r < 8; r++) { // 3-bit Red (0-7)
        for (idx g = 0; g < 8; g++) { // 3-bit Green (0-7)
            for (idx b = 0; b < 4; b++, palette_idx++) { // 2-bit Blue (0-3), increment idx
                
                // Calculate scaled 6-bit DAC values (0-63)
                // Scaling formula for 3-bit -> 6-bit: (val << 3) | (val >> 2)
                // Scaling formula for 2-bit -> 6-bit: (val << 4) | (val << 2) | (val >> 2)
                uint8_t dac_red = (r << 3) | (r >> 2);
                uint8_t dac_green = (g << 3) | (g >> 2);
                uint8_t dac_blue = (b << 4) | (b << 2) | (b >> 2);

                // Step 1: Select the palette index to modify
                outb(0x3C8, palette_idx);

                // Step 2: Write the Red, Green, and Blue values consecutively to port 0x3C9
                outb(0x3C9, dac_red);
                outb(0x3C9, dac_green);
                outb(0x3C9, dac_blue);
                
                // Optional: Add a small delay if needed for very slow systems
                // asm volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t");
            }
        }
    }

    // Re-enable interrupts
    __asm__ volatile ("sti");
}
EXTENDED_KERNEL void kend_gm(){
    kfree(VMEM);
}

static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b){
	return ((r >> 5) << 5) |
	       ((g >> 5) << 2) |
	       (b >> 6);
}

EXTENDED_KERNEL void kswapbuf(){
    for(idx i=0;i<SCREEN_WIDTH*SCREEN_HEIGHT;i++){
		uint8_t c = VMEM[i];
	    if(shader){
		    uint8_t r, g, b;
		    kgetcolor(c, &r, &g, &b);

		    size_t x = i % SCREEN_WIDTH;
		    size_t y = i / SCREEN_WIDTH;

		    V3f col = shader(
				v2f((float)x / SCREEN_WIDTH, (float)y /SCREEN_HEIGHT),
				 v3f(
				    r / 255.0f,
				    g / 255.0f,
				    b / 255.0f));

		    uint8_t nr = (uint8_t)(clampf(col.x, 0, 1) * 255.0f);
		    uint8_t ng = (uint8_t)(clampf(col.y, 0, 1) * 255.0f);
		    uint8_t nb = (uint8_t)(clampf(col.z, 0, 1) * 255.0f);
		    c = rgb332(nr, ng, nb);
	    }
		realVMEM[i] = c;
    }
}

EXTENDED_KERNEL void kclear(){
    for(idx i=0;i<320*200;i++){
        VMEM[i]=color;
    }
}

EXTENDED_KERNEL void kgetcolor(uint8_t color,
                                   uint8_t* r,
                                   uint8_t* g,
                                   uint8_t* b)
{
	uint8_t r_bits = (color >> 5) & 0x7;
	uint8_t g_bits = (color >> 2) & 0x7;
	uint8_t b_bits =  color       & 0x3;

	*r = r_bits << 5; // минимальный r
	*g = g_bits << 5; // минимальный g
	*b = b_bits << 6; // минимальный b
}

EXTENDED_KERNEL void ksetcolor(uint8_t r, uint8_t g, uint8_t b){
    uint8_t r_bits = (r >> 5) & 0x7; // 0xFF -> 0x7, 0xF8 -> 0x7, etc.
    uint8_t g_bits = (g >> 5) & 0x7;
    uint8_t b_bits = (b >> 6) & 0x3; // 0xFF -> 0x3, 0xFC -> 0x3, etc.
    color = (r_bits << 5) | (g_bits << 2) | b_bits;
}


EXTENDED_KERNEL void kplot_pixel(float x, float y) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        VMEM[(idx)(y) * SCREEN_WIDTH + (idx)(x)] = color;
    }
}

EXTENDED_KERNEL void kdraw_rect(float x, float y, float width, float height) {
    for (idx w = 0; w < width; w++) {
        kplot_pixel(x + w, y);
        kplot_pixel(x + w, y + height - 1);
    }

    for (idx h = 1; h < height - 1; h++) {
        kplot_pixel(x, y + h);             
        kplot_pixel(x + width - 1, y + h);
    }
}

// To draw a filled rectangle, you would iterate over the area:
EXTENDED_KERNEL void kfill_rect(float x, float y, float width, float height) {
    for (idx h = 0; h < height; h++) {
        for (idx w = 0; w < width; w++) {
            kplot_pixel(x + w, y + h);
        }
    }
}

EXTENDED_KERNEL void kswap(float *a, float *b) {
    float temp = *a;
    *a = *b;
    *b = temp;
}

EXTENDED_KERNEL void kdraw_line(V2f p0, V2f p1) {
    float dx = abs(p1.x - p0.x), sx = p0.x < p1.x ? 1 : -1;
    float dy = abs(p1.y - p0.y), sy = p0.y < p1.y ? 1 : -1;
    float err = (dx > dy ? dx : -dy) / 2;

    while (kplot_pixel(p0.x, p0.y), p0.x != p1.x || p0.y != p1.y) {
        float e2 = err;
        if (e2 > -dx) { err -= dy; p0.x += sx; }
        if (e2 <  dy) { err += dx; p0.y += sy; }
    }
}

// Triangle drawing function using three lines
EXTENDED_KERNEL void kdraw_triangle(V2f p0, V2f p1, V2f p2) {
    kdraw_line(p0, p1);
    kdraw_line(p1, p2);
    kdraw_line(p2, p0);
}

// Filled triangle algorithm using edge walking method
EXTENDED_KERNEL void kfill_triangle(V2f p0, V2f p1, V2f p2) {
    // Sort vertices by Y coordinate (p0.y <= p1.y <= p2.y)
    if (p0.y > p1.y) { kswap(&p0.x, &p1.x); kswap(&p0.y, &p1.y); }
    if (p0.y > p2.y) { kswap(&p0.x, &p2.x); kswap(&p0.y, &p2.y); }
    if (p1.y > p2.y) { kswap(&p1.x, &p2.x); kswap(&p1.y, &p2.y); }
    
    // Calculate slopes for the three edges
    float invslope_01 = 0, invslope_12 = 0, invslope_02 = 0;
    
    if (p1.y != p0.y) invslope_01 = (float)(p1.x - p0.x) / (p1.y - p0.y);
    if (p2.y != p1.y) invslope_12 = (float)(p2.x - p1.x) / (p2.y - p1.y);
    if (p2.y != p0.y) invslope_02 = (float)(p2.x - p0.x) / (p2.y - p0.y);
    
    // Draw top half (from p0.y to p1.y)
    float startx = p0.x, endx = p0.x;
    
    for (float scanline_y = p0.y; scanline_y <= p1.y; scanline_y++) {
        if (scanline_y >= 0 && scanline_y < SCREEN_HEIGHT) {
            float left_x = (float)startx;
            float right_x = (float)endx;
            
            // Ensure left_x <= right_x
            if (left_x > right_x) {
                kswap(&left_x, &right_x);
            }
            
            // Fill the horizontal line
            for (float x = left_x; x <= right_x; x++) {
                if (x >= 0 && x < SCREEN_WIDTH) {
                    kplot_pixel(x, scanline_y);
                }
            }
        }
        
        startx += invslope_01;
        endx += invslope_02;
    }
    
    // Reset starting X for bottom half
    startx = p1.x;
    
    // Draw bottom half (from p1.y to p2.y)
    for (float scanline_y = p1.y + 1; scanline_y <= p2.y; scanline_y++) {
        if (scanline_y >= 0 && scanline_y < SCREEN_HEIGHT) {
            float left_x = (float)startx;
            float right_x = (float)endx;
            
            // Ensure left_x <= right_x
            if (left_x > right_x) {
                kswap(&left_x, &right_x);
            }
            
            // Fill the horizontal line
            for (float x = left_x; x <= right_x; x++) {
                if (x >= 0 && x < SCREEN_WIDTH) {
                    kplot_pixel(x, scanline_y);
                }
            }
        }
        
        startx += invslope_12;
        endx += invslope_02;
    }
}

EXTENDED_KERNEL void ksetcamera(V3f v){
    cam_pos = v;
}

// Alternative filled triangle using barycentric coordinates (slower but more robust)
EXTENDED_KERNEL void kfill_triangle_barycentric(V2f p0, V2f p1, V2f p2) {
    // Calculate bounding box
    float min_x = (p0.x < p1.x) ? p0.x : p1.x;
    min_x = (min_x < p2.x) ? min_x : p2.x;
    float max_x = (p0.x > p1.x) ? p0.x : p1.x;
    max_x = (max_x > p2.x) ? max_x : p2.x;
    float min_y = (p0.y < p1.y) ? p0.y : p1.y;
    min_y = (min_y < p2.y) ? min_y : p2.y;
    float max_y = (p0.y > p1.y) ? p0.y : p1.y;
    max_y = (max_y > p2.y) ? max_y : p2.y;
    
    // Clamp to screen bounds
    min_x = (min_x < 0) ? 0 : min_x;
    max_x = (max_x >= SCREEN_WIDTH) ? SCREEN_WIDTH - 1 : max_x;
    min_y = (min_y < 0) ? 0 : min_y;
    max_y = (max_y >= SCREEN_HEIGHT) ? SCREEN_HEIGHT - 1 : max_y;
    
    // Calculate edge vectors
    float ax = p1.x - p0.x, ay = p1.y - p0.y;
    float bx = p2.x - p0.x, by = p2.y - p0.y;
    
    // Calculate determinant for area calculation
    float det = ax * by - ay * bx;
    if (det == 0) return; // Degenerate triangle
    
    // Iterate through bounding box
    for (float y = min_y; y <= max_y; y++) {
        for (float x = min_x; x <= max_x; x++) {
            // Calculate barycentric coordinates
            float px = x - p0.x, py = y - p0.y;
            float s = ax * px + ay * py;
            float t = bx * px + by * py;
            
            // Check if point is inside triangle
            if (det > 0) {
                if (s < 0 || t < 0 || s + t > det) continue;
            } else {
                if (s > 0 || t > 0 || s + t < det) continue;
            }
            
            kplot_pixel(x, y);
        }
    }
}

EXTENDED_KERNEL bool k3d22d(V3f v, V2f *out) {
	//TODO: use camera rotation and FOV(done?).
	// formula is P_rel = pos - cam_pos;
	// R(alpha, beta, gamma) = rot_X(gamma) * rot_Y(beta) * rot_Z(alpha);
	// P = P_rel * R(cam_rot.x, cam_rot.y, cam_rot.z);
	// X = P.x / (P.z * tan(fov / 2); // fov in radiance?
	// Y = P.y / (P.z * tan(fov / 2); // fov in radiance?
	//NOTE: la.h have m3f_mul_vec, m3f_mul, m3f_rot_x(angle), m3f_rot_y(angle), m3f_rot_z(angle),\
	// so theoretically it shouldn't be that hard
	M3f R = m3f_rot_x(-cam_rot.z);
		R = m3f_mul(R, m3f_rot_y(-cam_rot.y));
		R = m3f_mul(R, m3f_rot_z(-cam_rot.x));
	V3f rel = v3f_sub(v, cam_pos);
	V3f norm = m3f_mul_vec(R, rel);
	if(norm.z > 0){
		V2f divider = v2ff((cam_pos.z - v.z) * tanf(cam_fov / 2));
		*out = v2f_div(norm.xy, divider);
		return true;
	}
	else{
		*out = v2ff(-1);
	}
}

EXTENDED_KERNEL void knorm(V2f p, V2f *out){
	out->x=160-p.x;
	out->y=100-p.y;
}

EXTENDED_KERNEL void kdrawarray(const V3f* vertex_array, idx num_vertices) {
    idx num_triangles = num_vertices / 3;
    if (num_vertices % 3 != 0) {

        ksetcolor(255,0,0);
        kfill_triangle(v2f(0, 0),
					   v2f(33, 0),
					   v2f(0, 33));
    }

    for (idx i = 0; i < num_triangles * 3; i += 3) {
        // Calculate indices for x, y, z of v0, v1, v2
        idx i0 = i + 0;
        idx i1 = i + 1;
        idx i2 = i + 2;

        V3f v0 = vertex_array[i0];
        V3f v1 = vertex_array[i1];
        V3f v2 = vertex_array[i2];

        // --- Projection and Normalization Pipeline ---
        // Project 3D coordinates to 2D using k3d22d
	    V2f p0_2d;
	    V2f p1_2d;
	    V2f p2_2d;

        // Check for potential division by zero in k3d22d
        if (v0.z != 0) {
            k3d22d(v0, &p0_2d);
        } else {
            // Handle Z=0 case (e.g., set to center or skip triangle)
            // For now, let's set it to an off-screen coordinate to effectively skip drawing
	        p0_2d = v2ff(-1);
        }
        if (v1.z != 0) {
            k3d22d(v1, &p1_2d);
        } else {
	        p1_2d = v2ff(-1);
        }
        if (v2.z != 0) {
            k3d22d(v2, &p2_2d);
        } else {
	        p2_2d = v2ff(-1);
        }

        // Normalize projected coordinates using knorm
        // Pass pointers to temporary variables to receive the normalized output
	    V2f norm_p0;
	    V2f norm_p1;
	    V2f norm_p2;

        knorm(p0_2d, &norm_p0);
        knorm(p1_2d, &norm_p1);
        knorm(p2_2d, &norm_p2);
        kfill_triangle(
				norm_p0,
				norm_p1,
				norm_p2);
    }
}

#undef idx