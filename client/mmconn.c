#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <conio.h>
#include <atari.h>

#include "fujinet-fuji.h"
#include "fujinet-network.h"

#define HOST_BUF_LEN 64
#define HOSTNAME_MAX_LEN (HOST_BUF_LEN - 3)

#define LOBBY_HOST_DEFAULT "fujinet.online"
#define LOBBY_PORT_DEFAULT "5000"
#define PLAYER_NAME_MAX 8

#define UI_TITLE_Y 0
#define UI_STATUS_Y 22

#define FIELD_WIDTH_HOST 32
#define FIELD_WIDTH_PORT 5
#define FIELD_WIDTH_NAME 8
#define FIELD_WIDTH_GAME 20

#define LIST_REFRESH_TICKS 620
#define HEARTBEAT_TICKS 620

#define MAX_GAMES 8
#define GAME_ID_LEN 8
#define GAME_NAME_MAX 32

static char host_buf[HOST_BUF_LEN];

typedef enum
{
    SCREEN_CONFIG = 0,
    SCREEN_LIST,
    SCREEN_CREATE,
    SCREEN_WAIT
} Screen;

typedef struct
{
    char lobby_host[HOSTNAME_MAX_LEN + 1];
    char lobby_port[6];
    char player_name[PLAYER_NAME_MAX + 1];
} LobbyConfig;

typedef struct
{
    char id[GAME_ID_LEN + 1];
    char name[GAME_NAME_MAX + 1];
    uint8_t players;
    uint8_t max_players;
    bool active;
} GameEntry;

typedef struct
{
    LobbyConfig cfg;
    Screen screen;
    char status[40];
    uint8_t focus;
    bool lobby_connected;
    char devicespec[64];
    char current_game_id[GAME_ID_LEN + 1];
    char start_host[HOSTNAME_MAX_LEN + 1];
    uint16_t start_port;
} AppState;

static AppState g_state;
static GameEntry g_games[MAX_GAMES];
static uint8_t g_game_count = 0;
static uint8_t g_selected = 0;
static char g_game_name[GAME_NAME_MAX + 1] = "Game";
static char g_game_max[3] = "10";
static char g_line[256];
static char g_cmd[16];
static uint32_t g_last_refresh = 0;
static uint32_t g_last_heartbeat = 0;
static uint32_t g_now = 0;
static int g_port = 0;
static char g_key = 0;
static char g_id[GAME_ID_LEN + 1];
static char g_name_buf[GAME_NAME_MAX + 1];
static int g_players = 0;
static int g_max_players = 0;
static bool g_active = false;
static char g_slice[256];
static size_t g_slice_len = 0;
static const char *g_p = NULL;
static const char *g_idp = NULL;
static const char *g_obj_start = NULL;
static const char *g_obj_end = NULL;

static uint32_t rtclok_now(void)
{
    return ((uint32_t)OS.rtclok[0] << 16) | ((uint32_t)OS.rtclok[1] << 8) | OS.rtclok[2];
}

static uint32_t rtclok_diff(uint32_t now, uint32_t then)
{
    if (now >= then)
        return now - then;
    return (0x1000000u - then + now);
}

static void atari_reset_warm(void)
{
    *(volatile uint8_t *)0x08 = 0xFF;
    __asm__("jmp $E474");
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
    uint8_t i;
    gotoxy(0, UI_STATUS_Y);
    for (i = 0; i < 40; ++i)
        cputc(' ');
    gotoxy(0, UI_STATUS_Y);
    cprintf("%s", msg);
}

static void draw_field_value(uint8_t x, uint8_t y, const char *value, uint8_t width)
{
    uint8_t i = 0;
    size_t len = strlen(value);
    const char *start = value;

    if (len > width)
        start = value + (len - width);

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

static bool is_printable(char c)
{
    return (c >= 32 && c <= 126);
}

static bool is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void handle_text_input(char *buf, size_t max_len, char key, bool alnum_only)
{
    size_t len = strlen(buf);
    if (key == CH_DEL || key == CH_DELCHR)
    {
        if (len > 0)
            buf[len - 1] = '\0';
        return;
    }
    if (len + 1 >= max_len)
        return;
    if (alnum_only)
    {
        if (!is_alnum(key))
            return;
    }
    else
    {
        if (!is_printable(key))
            return;
    }
    buf[len] = key;
    buf[len + 1] = '\0';
}

static void draw_config_screen(const AppState *state)
{
    clrscr();
    gotoxy(0, UI_TITLE_Y);
    cprintf("MIDIMaze Lobby");

    gotoxy(0, 2);
    cprintf(state->focus == 0 ? "> Host: " : "  Host: ");
    draw_field_value(8, 2, state->cfg.lobby_host, FIELD_WIDTH_HOST);

    gotoxy(0, 4);
    cprintf(state->focus == 1 ? "> Port: " : "  Port: ");
    draw_field_value(8, 4, state->cfg.lobby_port, FIELD_WIDTH_PORT);

    gotoxy(0, 6);
    cprintf(state->focus == 2 ? "> Name: " : "  Name: ");
    draw_field_value(8, 6, state->cfg.player_name, FIELD_WIDTH_NAME);

    gotoxy(0, 8);
    cprintf(state->focus == 3 ? "> [ CONNECT ]" : "  [ CONNECT ]");

    set_status(state->status);
}

static void draw_list_screen(const GameEntry *games, uint8_t game_count, uint8_t selected)
{
    uint8_t i;
    uint8_t y;
    clrscr();
    gotoxy(0, UI_TITLE_Y);
    cprintf("Lobby Games");
    gotoxy(0, 2);
    cprintf("R=Refresh  C=Create  ENTER=Join");

    for (i = 0; i < game_count; i++)
    {
        y = 4 + i;
        gotoxy(0, y);
        if (i == selected)
            cprintf("> ");
        else
            cprintf("  ");
        cprintf("%s (%u/%u)%s", games[i].name, games[i].players, games[i].max_players,
                games[i].active ? "*" : "");
    }
    set_status("Waiting for games...");
}

static void draw_create_screen(const AppState *state, const char *game_name, const char *max_players, uint8_t focus)
{
    clrscr();
    gotoxy(0, UI_TITLE_Y);
    cprintf("Create Game");

    gotoxy(0, 3);
    cprintf(focus == 0 ? "> Name: " : "  Name: ");
    draw_field_value(8, 3, game_name, FIELD_WIDTH_GAME);

    gotoxy(0, 5);
    cprintf(focus == 1 ? "> Max: " : "  Max: ");
    draw_field_value(8, 5, max_players, 2);

    gotoxy(0, 7);
    cprintf(focus == 2 ? "> [ CREATE ]" : "  [ CREATE ]");

    gotoxy(0, 9);
    cprintf(focus == 3 ? "> [ BACK ]" : "  [ BACK ]");

    set_status(state->status);
}

static void draw_wait_screen(const char *game_id)
{
    clrscr();
    gotoxy(0, UI_TITLE_Y);
    cprintf("Waiting for Players");
    gotoxy(0, 3);
    cprintf("Game: %s", game_id);
    gotoxy(0, 5);
    cprintf("Press ESC to cancel");
    set_status("Waiting for lobby start...");
}

static void lobby_build_devicespec(AppState *state)
{
    snprintf(state->devicespec, sizeof(state->devicespec), "N1:TCP://%s:%s/",
             state->cfg.lobby_host, state->cfg.lobby_port);
}

static bool lobby_open(AppState *state)
{
    lobby_build_devicespec(state);
    if (network_open(state->devicespec, 12, 0) != 0)
        return false;
    state->lobby_connected = true;
    return true;
}

static void lobby_close(AppState *state)
{
    if (!state->lobby_connected)
        return;
    network_close(state->devicespec);
    state->lobby_connected = false;
}

static bool lobby_send(AppState *state, const char *line)
{
    uint16_t len = (uint16_t)strlen(line);
    if (network_write(state->devicespec, (const uint8_t *)line, len) != 0)
        return false;
    return true;
}

static bool lobby_read_line(AppState *state, char *out, size_t out_len, uint32_t timeout_ticks)
{
    static char rx_buf[256];
    static uint16_t rx_len = 0;
    int16_t r;
    char *nl;
    size_t line_len;
    uint32_t start = rtclok_now();

    while (rtclok_diff(rtclok_now(), start) < timeout_ticks)
    {
        if (rx_len >= sizeof(rx_buf))
            rx_len = 0;

        r = network_read_nb(state->devicespec, (uint8_t *)rx_buf + rx_len,
                            (uint16_t)(sizeof(rx_buf) - rx_len - 1));
        if (r > 0)
        {
            rx_len += (uint16_t)r;
            rx_buf[rx_len] = '\0';
            nl = strchr(rx_buf, '\n');
            if (nl)
            {
                line_len = (size_t)(nl - rx_buf);
                if (line_len >= out_len)
                    line_len = out_len - 1;
                memcpy(out, rx_buf, line_len);
                out[line_len] = '\0';
                memmove(rx_buf, nl + 1, rx_len - (uint16_t)(line_len + 1));
                rx_len -= (uint16_t)(line_len + 1);
                return true;
            }
        }
        if (kbhit())
            return false;
    }
    return false;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[32];
    const char *p;
    size_t i;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p && (*p == ' ' || *p == '\t'))
        p++;
    if (*p != '"')
        return false;
    p++;
    i = 0;
    while (*p && *p != '"' && i + 1 < out_len)
        out[i++] = *p++;
    out[i] = '\0';
    return *p == '"';
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char pat[32];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p && (*p == ' ' || *p == '\t'))
        p++;
    *out = atoi(p);
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char pat[32];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p && (*p == ' ' || *p == '\t'))
        p++;
    if (strncmp(p, "true", 4) == 0)
    {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

static uint8_t parse_games_list(const char *json, GameEntry *out, uint8_t max_out)
{
    uint8_t count = 0;
    g_p = strstr(json, "\"games\"");
    if (!g_p)
        return 0;
    g_p = strchr(g_p, '[');
    if (!g_p)
        return 0;
    g_p++;
    while (*g_p && count < max_out)
    {
        g_idp = strstr(g_p, "\"id\"");
        if (!g_idp)
            break;
        g_obj_start = g_idp;
        g_obj_end = strchr(g_obj_start, '}');
        if (!g_obj_end)
            break;

        g_id[0] = '\0';
        g_name_buf[0] = '\0';
        g_players = 0;
        g_max_players = 0;
        g_active = false;

        g_slice_len = (size_t)(g_obj_end - g_obj_start + 1);
        if (g_slice_len >= sizeof(g_slice))
            g_slice_len = sizeof(g_slice) - 1;
        memcpy(g_slice, g_obj_start, g_slice_len);
        g_slice[g_slice_len] = '\0';

        json_get_string(g_slice, "id", g_id, sizeof(g_id));
        json_get_string(g_slice, "name", g_name_buf, sizeof(g_name_buf));
        json_get_int(g_slice, "players", &g_players);
        json_get_int(g_slice, "max", &g_max_players);
        json_get_bool(g_slice, "active", &g_active);

        snprintf(out[count].id, sizeof(out[count].id), "%s", g_id);
        snprintf(out[count].name, sizeof(out[count].name), "%s", g_name_buf[0] ? g_name_buf : "Game");
        out[count].players = (uint8_t)g_players;
        out[count].max_players = (uint8_t)g_max_players;
        out[count].active = g_active;
        count++;

        g_p = g_obj_end + 1;
    }
    return count;
}

static bool parse_port(const char *text, uint16_t *out_port)
{
    unsigned long value = 0;
    const char *p = text;

    if (*p == '\0')
        return false;

    while (*p != '\0')
    {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10 + (unsigned long)(*p - '0');
        if (value > 65535UL)
            return false;
        ++p;
    }

    *out_port = (uint16_t)value;
    return true;
}

static bool start_netstream(const char *host, uint16_t port)
{
    uint8_t flags = 0;
    flags |= (1u << 0); /* TCP */
    flags |= (1u << 1); /* REGISTER */
    flags |= (1u << 2); /* TX clock external */
    if (get_tv() == AT_PAL)
        flags |= (1u << 4);

    build_host_buffer(host, flags, 21);

    if (!fuji_enable_udpstream(swap16(port), host_buf))
        return false;

    fuji_mount_all();
    return true;
}

int main(void)
{
    memset(&g_state, 0, sizeof(g_state));

    strncpy(g_state.cfg.lobby_host, LOBBY_HOST_DEFAULT, sizeof(g_state.cfg.lobby_host) - 1);
    strncpy(g_state.cfg.lobby_port, LOBBY_PORT_DEFAULT, sizeof(g_state.cfg.lobby_port) - 1);
    g_state.cfg.player_name[0] = '\0';
    g_state.screen = SCREEN_CONFIG;
    g_state.focus = 0;
    strncpy(g_state.status, "TAB/ARROWS move, ENTER select", sizeof(g_state.status) - 1);

    draw_config_screen(&g_state);

    while (1)
    {
        if (g_state.screen == SCREEN_CONFIG)
        {
            g_key = cgetc();
            if (g_key == CH_TAB || g_key == CH_CURS_DOWN)
            {
                g_state.focus = (g_state.focus + 1) % 4;
                draw_config_screen(&g_state);
                continue;
            }
            if (g_key == CH_CURS_UP)
            {
                g_state.focus = (g_state.focus == 0) ? 3 : (g_state.focus - 1);
                draw_config_screen(&g_state);
                continue;
            }

            if (g_state.focus == 0)
            {
                if (g_key == CH_ENTER)
                {
                    g_state.focus = 1;
                    draw_config_screen(&g_state);
                }
                else
                {
                    handle_text_input(g_state.cfg.lobby_host, sizeof(g_state.cfg.lobby_host), g_key, false);
                    draw_field_value(8, 2, g_state.cfg.lobby_host, FIELD_WIDTH_HOST);
                }
                continue;
            }
            if (g_state.focus == 1)
            {
                if (g_key == CH_ENTER)
                {
                    g_state.focus = 2;
                    draw_config_screen(&g_state);
                }
                else
                {
                    handle_text_input(g_state.cfg.lobby_port, sizeof(g_state.cfg.lobby_port), g_key, true);
                    draw_field_value(8, 4, g_state.cfg.lobby_port, FIELD_WIDTH_PORT);
                }
                continue;
            }
            if (g_state.focus == 2)
            {
                if (g_key == CH_ENTER)
                {
                    g_state.focus = 3;
                    draw_config_screen(&g_state);
                }
                else
                {
                    handle_text_input(g_state.cfg.player_name, sizeof(g_state.cfg.player_name), g_key, true);
                    draw_field_value(8, 6, g_state.cfg.player_name, FIELD_WIDTH_NAME);
                }
                continue;
            }
            if (g_state.focus == 3 && g_key == CH_ENTER)
            {
                uint16_t port = 0;
                if (g_state.cfg.lobby_host[0] == '\0')
                {
                    set_status("Host required");
                    continue;
                }
                if (!parse_port(g_state.cfg.lobby_port, &port) || port == 0)
                {
                    set_status("Port invalid");
                    continue;
                }
                if (g_state.cfg.player_name[0] == '\0')
                {
                    set_status("Name required");
                    continue;
                }

                if (network_init() != 0)
                {
                    set_status("Network init failed");
                    continue;
                }

                clrscr();
                cprintf("Connecting lobby...");
                if (!lobby_open(&g_state))
                {
                    set_status("Lobby connect failed");
                    draw_config_screen(&g_state);
                    continue;
                }

                snprintf(g_line, sizeof(g_line), "{\"cmd\":\"hello\",\"name\":\"%s\"}\n", g_state.cfg.player_name);
                if (!lobby_send(&g_state, g_line))
                {
                    set_status("Lobby write failed");
                    lobby_close(&g_state);
                    draw_config_screen(&g_state);
                    continue;
                }

                if (!lobby_read_line(&g_state, g_line, sizeof(g_line), 200))
                {
                    set_status("No lobby response");
                    lobby_close(&g_state);
                    draw_config_screen(&g_state);
                    continue;
                }

                g_state.screen = SCREEN_LIST;
                g_last_refresh = 0;
                g_selected = 0;
                draw_list_screen(g_games, g_game_count, g_selected);
                continue;
            }
        }
        else if (g_state.screen == SCREEN_LIST)
        {
            g_now = rtclok_now();
            if (g_last_refresh == 0 || rtclok_diff(g_now, g_last_refresh) >= LIST_REFRESH_TICKS)
            {
                lobby_send(&g_state, "{\"cmd\":\"list\"}\n");
                if (lobby_read_line(&g_state, g_line, sizeof(g_line), 200))
                {
                    g_game_count = parse_games_list(g_line, g_games, MAX_GAMES);
                    if (g_selected >= g_game_count)
                        g_selected = 0;
                    draw_list_screen(g_games, g_game_count, g_selected);
                }
                g_last_refresh = g_now;
            }

            if (!kbhit())
                continue;
            g_key = cgetc();
            if (g_key == 'r' || g_key == 'R')
            {
                g_last_refresh = 0;
                continue;
            }
            if (g_key == 'c' || g_key == 'C')
            {
                g_state.screen = SCREEN_CREATE;
                g_state.focus = 0;
                strncpy(g_state.status, "Enter game settings", sizeof(g_state.status) - 1);
                draw_create_screen(&g_state, g_game_name, g_game_max, g_state.focus);
                continue;
            }
            if (g_key == CH_CURS_UP && g_selected > 0)
            {
                g_selected--;
                draw_list_screen(g_games, g_game_count, g_selected);
                continue;
            }
            if (g_key == CH_CURS_DOWN && g_selected + 1 < g_game_count)
            {
                g_selected++;
                draw_list_screen(g_games, g_game_count, g_selected);
                continue;
            }
            if (g_key == CH_ENTER && g_game_count > 0)
            {
                snprintf(g_line, sizeof(g_line), "{\"cmd\":\"join\",\"game_id\":\"%s\"}\n", g_games[g_selected].id);
                lobby_send(&g_state, g_line);
                if (lobby_read_line(&g_state, g_line, sizeof(g_line), 200))
                {
                    snprintf(g_state.current_game_id, sizeof(g_state.current_game_id), "%s", g_games[g_selected].id);
                    g_state.screen = SCREEN_WAIT;
                    draw_wait_screen(g_state.current_game_id);
                    g_last_heartbeat = rtclok_now();
                }
                continue;
            }
        }
        else if (g_state.screen == SCREEN_CREATE)
        {
            g_key = cgetc();
            if (g_key == CH_TAB || g_key == CH_CURS_DOWN)
            {
                g_state.focus = (g_state.focus + 1) % 4;
                draw_create_screen(&g_state, g_game_name, g_game_max, g_state.focus);
                continue;
            }
            if (g_key == CH_CURS_UP)
            {
                g_state.focus = (g_state.focus == 0) ? 3 : (g_state.focus - 1);
                draw_create_screen(&g_state, g_game_name, g_game_max, g_state.focus);
                continue;
            }
            if (g_state.focus == 0)
            {
                if (g_key == CH_ENTER)
                {
                    g_state.focus = 1;
                    draw_create_screen(&g_state, g_game_name, g_game_max, g_state.focus);
                }
                else
                {
                    handle_text_input(g_game_name, sizeof(g_game_name), g_key, false);
                    draw_field_value(8, 3, g_game_name, FIELD_WIDTH_GAME);
                }
                continue;
            }
            if (g_state.focus == 1)
            {
                if (g_key == CH_ENTER)
                {
                    g_state.focus = 2;
                    draw_create_screen(&g_state, g_game_name, g_game_max, g_state.focus);
                }
                else
                {
                    handle_text_input(g_game_max, sizeof(g_game_max), g_key, true);
                    draw_field_value(8, 5, g_game_max, 2);
                }
                continue;
            }
            if (g_state.focus == 2 && g_key == CH_ENTER)
            {
                snprintf(g_line, sizeof(g_line), "{\"cmd\":\"create\",\"name\":\"%s\",\"max_players\":%s}\n",
                         g_game_name, g_game_max);
                lobby_send(&g_state, g_line);
                if (lobby_read_line(&g_state, g_line, sizeof(g_line), 200))
                {
                    json_get_string(g_line, "game_id", g_state.current_game_id, sizeof(g_state.current_game_id));
                    g_state.screen = SCREEN_WAIT;
                    draw_wait_screen(g_state.current_game_id);
                    g_last_heartbeat = rtclok_now();
                }
                continue;
            }
            if (g_state.focus == 3 && g_key == CH_ENTER)
            {
                g_state.screen = SCREEN_LIST;
                draw_list_screen(g_games, g_game_count, g_selected);
                continue;
            }
        }
        else if (g_state.screen == SCREEN_WAIT)
        {
            g_now = rtclok_now();
            if (kbhit())
            {
                g_key = cgetc();
                if (g_key == CH_ESC)
                {
                    snprintf(g_line, sizeof(g_line), "{\"cmd\":\"leave\",\"game_id\":\"%s\"}\n", g_state.current_game_id);
                    lobby_send(&g_state, g_line);
                    g_state.screen = SCREEN_LIST;
                    draw_list_screen(g_games, g_game_count, g_selected);
                    continue;
                }
            }

            if (rtclok_diff(g_now, g_last_heartbeat) >= HEARTBEAT_TICKS)
            {
                lobby_send(&g_state, "{\"cmd\":\"heartbeat\"}\n");
                g_last_heartbeat = g_now;
            }

            if (lobby_read_line(&g_state, g_line, sizeof(g_line), 20))
            {
                if (json_get_string(g_line, "cmd", g_cmd, sizeof(g_cmd)) && strcmp(g_cmd, "start") == 0)
                {
                    json_get_string(g_line, "host", g_state.start_host, sizeof(g_state.start_host));
                    g_port = 0;
                    json_get_int(g_line, "port", &g_port);
                    g_state.start_port = (uint16_t)g_port;
                    lobby_close(&g_state);

                    clrscr();
                    cprintf("Starting game...");
                    if (start_netstream(g_state.start_host, g_state.start_port))
                    {
                        cprintf("Done!\n");
                        atari_reset_warm();
                    }
                    else
                    {
                        cprintf("NetStream failed\n");
                        return 1;
                    }
                }
            }
        }
    }

}
