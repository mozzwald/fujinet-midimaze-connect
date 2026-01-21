#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <conio.h>
#include <atari.h>

#include "fujinet-fuji.h"

#define HOST_BUF_LEN 64
#define DEFAULT_HOST "tcp:fujinet.online"
#define DEFAULT_PORT 5004

static char host_buf[HOST_BUF_LEN];

typedef enum
{
    RESET_WARM = 0,
    RESET_COLD = 1
} ResetType;

static void atari_reset(ResetType type)
{
    if (type == RESET_COLD)
    {
        *(volatile uint8_t *)0x0244 = 0x00;
        __asm__("jmp $E477");
    }
    else
    {
        *(volatile uint8_t *)0x08 = 0xFF;
        __asm__("jmp $E474");
    }
}

static uint16_t swap16(uint16_t value)
{
    return (uint16_t)(((uint32_t)value << 8) | ((uint32_t)value >> 8));
}

static void build_host_buffer(const char *host, uint8_t flags)
{
    size_t len;

    memset(host_buf, 0, sizeof(host_buf));
    strncpy(host_buf, host, HOST_BUF_LEN - 2);
    host_buf[HOST_BUF_LEN - 2] = '\0';

    len = strlen(host_buf);
    if (len + 1 < HOST_BUF_LEN)
    {
        host_buf[len + 1] = (char)flags;
    }
}

int main(void)
{
    bool ok;
    uint8_t flags = 0;

    flags |= (1u << 0); /* TCP */
    flags |= (1u << 1); /* REGISTER enabled */
    /* servermode Raw -> bit2 stays 0 */

    clrscr();
    cprintf("Connecting FujiNet NETStream!\n");
    build_host_buffer(DEFAULT_HOST, flags);
    ok = fuji_enable_udpstream(swap16((uint16_t)DEFAULT_PORT), host_buf);
    (void)ok;

    ok = fuji_unmount_disk_image(1);
    (void)ok;
    ok = fuji_mount_all();
    (void)ok;

    atari_reset(RESET_WARM);
    return 0;
}
