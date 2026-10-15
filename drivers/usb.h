#ifndef USB_H
#define USB_H

#include <libc/stdint>

/* Инициализация USB-контроллера (EHCI) и монтирование флешек в блочную систему */
void usb_init(void);

#endif /* USB_H */