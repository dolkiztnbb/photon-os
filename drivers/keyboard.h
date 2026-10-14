#ifndef KBRD
#define KBRD

#include <libc/stdint>

// Однобайтовые коды спецклавиш (не конфликтуют со стандартным ASCII)
#define KEY_UP    ((char)0x80)
#define KEY_DOWN  ((char)0x81)
#define KEY_LEFT  ((char)0x82)
#define KEY_RIGHT ((char)0x83)
#define KEY_WIN   ((char)0x84)

void init_keyboard(void);
char getch(void);
char poll_key(void);

extern char isCaps;
extern char shift;
extern char ctrl;
extern char kbutton[256];

#endif
