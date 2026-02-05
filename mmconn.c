#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <conio.h>
#include <atari.h>

#include "fujinet-fuji.h"

#define HOST_BUF_LEN 64
#define DEFAULT_PORT 5004
#define HOSTNAME_MAX_LEN (HOST_BUF_LEN - 3)

#define UI_TITLE_Y 0
#define UI_HOST_Y 2
#define UI_PORT_Y 4
#define UI_TRANSPORT_Y 6
#define UI_REGISTER_Y 7
#define UI_CONNECT_Y 9
#define UI_STATUS_Y 11
#define HOST_FIELD_WIDTH 32
#define PORT_FIELD_WIDTH 5
#define DEFAULT_STATUS_MSG "\xD4\xC1\xC2 move fields \xD3\xD0\xC1\xC3\xC5 toggle values"

static char host_buf[HOST_BUF_LEN];

typedef enum
{
    RESET_WARM = 0,
    RESET_COLD = 1
} ResetType;

typedef enum
{
    FIELD_HOST = 0,
    FIELD_PORT,
    FIELD_TRANSPORT,
    FIELD_REGISTER,
    FIELD_CONNECT,
    FIELD_COUNT
} Field;

typedef struct
{
    char host[HOSTNAME_MAX_LEN + 1];
    char port[6];
    bool transport_tcp;
    bool send_register;
    Field focus;
    char status[40];
} FormState;

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

static void build_host_buffer(const char *hostname, uint8_t flags, uint8_t audf3)
{
    size_t len;

    memset(host_buf, 0, sizeof(host_buf));
    strncpy(host_buf, hostname, HOSTNAME_MAX_LEN);
    host_buf[HOST_BUF_LEN - 3] = '\0';

    len = strlen(host_buf);
    if (len + 2 < HOST_BUF_LEN)
    {
        host_buf[len + 1] = (char)flags;
        host_buf[len + 2] = (char)audf3;
    }
}

static void set_status(const char *msg)
{
    gotoxy(0, UI_STATUS_Y);
    {
        uint8_t i;
        for (i = 0; i < 40; ++i)
        {
            cputc(' ');
        }
    }
    cprintf("%s", msg);
}

static void draw_label(uint8_t y, const char *label, bool focused)
{
    gotoxy(0, y);
    if (focused)
    {
        cprintf("> ");
    }
    else
    {
        cprintf("  ");
    }
    cprintf("%s", label);
}

static void draw_field_value(uint8_t x, uint8_t y, const char *value, uint8_t width)
{
    uint8_t i = 0;
    size_t len = strlen(value);
    const char *start = value;

    if (len > width)
    {
        start = value + (len - width);
    }
    gotoxy(x, y);
    while (start[i] != '\0' && i < width)
    {
        cprintf("%c", start[i]);
        ++i;
    }
    while (i < width)
    {
        cprintf(" ");
        ++i;
    }
}

static void draw_toggle_value(uint8_t x, uint8_t y, const char *left, const char *right, bool right_selected)
{
    gotoxy(x, y);
    if (right_selected)
    {
        cprintf("%s  [%s]", left, right);
    }
    else
    {
        cprintf("[%s]  %s", left, right);
    }
}

static void draw_form(const FormState *state)
{
    clrscr();
    gotoxy(0, UI_TITLE_Y);
    cprintf("FujiNet NETStream Connect");

    draw_label(UI_HOST_Y, "Host:", state->focus == FIELD_HOST);
    draw_field_value(8, UI_HOST_Y, state->host, HOST_FIELD_WIDTH);

    draw_label(UI_PORT_Y, "Port:", state->focus == FIELD_PORT);
    draw_field_value(8, UI_PORT_Y, state->port, PORT_FIELD_WIDTH);

    draw_label(UI_TRANSPORT_Y, "Transport:", state->focus == FIELD_TRANSPORT);
    draw_toggle_value(12, UI_TRANSPORT_Y, "TCP", "UDP", !state->transport_tcp);

    draw_label(UI_REGISTER_Y, "Send REGISTER?", state->focus == FIELD_REGISTER);
    draw_toggle_value(18, UI_REGISTER_Y, "Yes", "No", !state->send_register);

    draw_label(UI_CONNECT_Y, "[ CONNECT ]", state->focus == FIELD_CONNECT);

    set_status(state->status);
}

static void advance_focus(FormState *state, int direction)
{
    int next = (int)state->focus + direction;
    if (next < 0)
    {
        next = FIELD_COUNT - 1;
    }
    else if (next >= FIELD_COUNT)
    {
        next = 0;
    }
    state->focus = (Field)next;
    strncpy(state->status, DEFAULT_STATUS_MSG, sizeof(state->status) - 1);
}

static bool is_printable(char c)
{
    return (c >= 32 && c <= 126);
}

static void handle_host_input(FormState *state, char key)
{
    size_t len = strlen(state->host);

    if (key == CH_DEL || key == CH_DELCHR)
    {
        if (len > 0)
        {
            state->host[len - 1] = '\0';
        }
        return;
    }

    if (is_printable(key) && len < HOSTNAME_MAX_LEN)
    {
        state->host[len] = key;
        state->host[len + 1] = '\0';
    }
}

static void handle_port_input(FormState *state, char key)
{
    size_t len = strlen(state->port);

    if (key == CH_DEL || key == CH_DELCHR)
    {
        if (len > 0)
        {
            state->port[len - 1] = '\0';
        }
        return;
    }

    if (key >= '0' && key <= '9')
    {
        if (len < 5)
        {
            state->port[len] = key;
            state->port[len + 1] = '\0';
        }
    }
}

static bool parse_port(const char *text, uint16_t *out_port)
{
    unsigned long value = 0;
    const char *p = text;

    if (*p == '\0')
    {
        return false;
    }

    while (*p != '\0')
    {
        if (*p < '0' || *p > '9')
        {
            return false;
        }
        value = value * 10 + (unsigned long)(*p - '0');
        if (value > 65535UL)
        {
            return false;
        }
        ++p;
    }

    *out_port = (uint16_t)value;
    return true;
}

int main(void)
{
    bool ok;
    uint8_t flags = 0;
    void (*saveVVBLKI)(void);
    FormState form;
    uint16_t port_value = 0;
    char key;

    /* Setup screen */
    saveVVBLKI = OS.vvblki;
    OS.vvblki = (void (*)(void))0xE45F;
    OS.sdmctl = 0x22;

    memset(&form, 0, sizeof(form));
    strncpy(form.port, "5004", sizeof(form.port) - 1);
    form.transport_tcp = true;
    form.send_register = true;
    form.focus = FIELD_HOST;
    strncpy(form.status, DEFAULT_STATUS_MSG, sizeof(form.status) - 1);

    draw_form(&form);

    while (1)
    {
        key = cgetc();

        if (key == CH_TAB)
        {
            advance_focus(&form, 1);
            draw_form(&form);
            continue;
        }
        if (key == CH_CURS_UP)
        {
            advance_focus(&form, -1);
            draw_form(&form);
            continue;
        }
        if (key == CH_CURS_DOWN)
        {
            advance_focus(&form, 1);
            draw_form(&form);
            continue;
        }

        switch (form.focus)
        {
            case FIELD_HOST:
                if (key == CH_ENTER)
                {
                    advance_focus(&form, 1);
                    draw_form(&form);
                }
                else
                {
                    handle_host_input(&form, key);
                    draw_field_value(8, UI_HOST_Y, form.host, HOSTNAME_MAX_LEN);
                }
                break;
            case FIELD_PORT:
                if (key == CH_ENTER)
                {
                    advance_focus(&form, 1);
                    draw_form(&form);
                }
                else
                {
                    handle_port_input(&form, key);
                    draw_field_value(8, UI_PORT_Y, form.port, 5);
                }
                break;
            case FIELD_TRANSPORT:
                if (key == CH_CURS_LEFT || key == CH_CURS_RIGHT || key == ' ')
                {
                    form.transport_tcp = !form.transport_tcp;
                    draw_toggle_value(12, UI_TRANSPORT_Y, "TCP", "UDP", !form.transport_tcp);
                }
                else if (key == CH_ENTER)
                {
                    advance_focus(&form, 1);
                    draw_form(&form);
                }
                break;
            case FIELD_REGISTER:
                if (key == CH_CURS_LEFT || key == CH_CURS_RIGHT || key == ' ')
                {
                    form.send_register = !form.send_register;
                    draw_toggle_value(18, UI_REGISTER_Y, "Yes", "No", !form.send_register);
                }
                else if (key == CH_ENTER)
                {
                    advance_focus(&form, 1);
                    draw_form(&form);
                }
                break;
            case FIELD_CONNECT:
                if (key == CH_ENTER)
                {
                    if (form.host[0] == '\0')
                    {
                        strncpy(form.status, "Host is required.", sizeof(form.status) - 1);
                        form.focus = FIELD_HOST;
                        draw_form(&form);
                        break;
                    }
                    if (!parse_port(form.port, &port_value))
                    {
                        strncpy(form.status, "Port must be 1-65535.", sizeof(form.status) - 1);
                        form.focus = FIELD_PORT;
                        draw_form(&form);
                        break;
                    }
                    if (port_value == 0)
                    {
                        strncpy(form.status, "Port must be 1-65535.", sizeof(form.status) - 1);
                        form.focus = FIELD_PORT;
                        draw_form(&form);
                        break;
                    }
                    goto connect_now;
                }
                break;
            default:
                break;
        }
    }

connect_now:
    clrscr();
    cprintf("Connecting FujiNet NETStream...");

    flags = 0;
    if (form.transport_tcp)
    {
        flags |= (1u << 0);
    }
    if (form.send_register)
    {
        flags |= (1u << 1);
    }
    flags |= (1u << 2); /* TX clock external */
    if (get_tv() == AT_PAL)
    {
        flags |= (1u << 4);
    }
    build_host_buffer(form.host, flags, 21);
#ifdef DEBUG
    printf("\nHost buffer: %s\n", host_buf);
    printf("Host: %s\n", form.host);
    printf("Port: %s\n", form.port);
    printf("Transport: %s\n", form.transport_tcp ? "TCP" : "UDP");
    printf("Send REGISTER: %s\n", form.send_register ? "Yes" : "No");
    printf("Press any key to continue...");
    (void)cgetc();
#endif
    ok = fuji_enable_udpstream(swap16(port_value), host_buf);
    (void)ok;
#ifndef DISK
    /* MIDIMaze Cartridge 
     * - Unmount D1 so it won't respond when midimaze allows for handler
     * - Mount All so CONFIG is out of the way
     */
    ok = fuji_unmount_disk_image(0); 
    (void)ok;
    ok = fuji_mount_all();
    (void)ok;
#else
    /* MIDIMaze XEX
     * - Mount all so it boots the game
     */
    ok = fuji_mount_all();
    (void)ok;
#endif

    cprintf("Done!\n");

    OS.vvblki = saveVVBLKI;

    atari_reset(RESET_WARM);
    return 0;
}
