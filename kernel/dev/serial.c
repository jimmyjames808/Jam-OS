#include <stdint.h>
#include <jam/serial.h>
#include <jam/x86.h>

#define COM1 0x3f8

static bool present;

bool serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xc7);   /* FIFO on, cleared */
    outb(COM1 + 4, 0x1e);   /* loopback mode for the self-test */
    outb(COM1 + 0, 0xae);
    if (inb(COM1 + 0) != 0xae) {
        present = false;    /* no UART: normal on modern PCs */
        return false;
    }
    outb(COM1 + 4, 0x0f);   /* normal operation */
    present = true;
    return true;
}

static void put(char c)
{
    for (int spins = 0; !(inb(COM1 + 5) & 0x20); spins++)
        if (spins > 100000)
            return;         /* never hang the kernel on a stuck UART */
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, size_t len)
{
    if (!present)
        return;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            put('\r');
        put(s[i]);
    }
}
