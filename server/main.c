#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LINE_BUF 512
#define NAME_MAX 8
#define GAME_NAME_MAX 32
#define GAME_ID_LEN 8
#define TOKEN_LEN 16
#define MAX_GAMES_LIMIT 32
#define MAX_PLAYERS_LIMIT 16
#define DEFAULT_MAX_GAMES 5
#define DEFAULT_MAX_PLAYERS 10
#define DEFAULT_JOIN_TIMEOUT_SEC 600
#define DEFAULT_DROP_TIMEOUT_SEC 15

typedef struct
{
    char host_name[256];
    int lobby_port;
    int game_port_min;
    int game_port_max;
    int max_games;
    int max_players_default;
    int join_timeout_sec;
    int drop_timeout_sec;
} ServerConfig;

typedef struct LobbyClient
{
    int fd;
    char name[NAME_MAX + 1];
    char id[GAME_ID_LEN + 1];
    bool connected;
    pthread_mutex_t send_mu;
} LobbyClient;

typedef struct Game
{
    bool in_use;
    bool active;
    bool ended;
    char id[GAME_ID_LEN + 1];
    char name[GAME_NAME_MAX + 1];
    int max_players;
    int player_count;
    int port;
    time_t created_at;
    LobbyClient *waiting_clients[MAX_PLAYERS_LIMIT];
    char player_names[MAX_PLAYERS_LIMIT][NAME_MAX + 1];
    char tokens[MAX_PLAYERS_LIMIT][TOKEN_LEN + 1];
    pthread_t thread;
} Game;

typedef struct
{
    Game *game;
    ServerConfig *cfg;
} GameThreadArgs;

static ServerConfig g_cfg;
static Game g_games[MAX_GAMES_LIMIT];
static bool *g_port_used = NULL;
static int g_port_range = 0;
static pthread_mutex_t g_games_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t get_time_ms(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

static void str_trim(char *s)
{
    if (!s)
        return;
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    if (*s == '\0')
        return;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;
    end[1] = '\0';
}

static bool is_alnum_str(const char *s)
{
    if (!s || *s == '\0')
        return false;
    for (const char *p = s; *p; p++)
    {
        if (!isalnum((unsigned char)*p))
            return false;
    }
    return true;
}

static void gen_id(char *out, size_t len)
{
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t alpha_len = sizeof(alphabet) - 1;
    for (size_t i = 0; i + 1 < len; i++)
    {
        out[i] = alphabet[rand() % alpha_len];
    }
    out[len - 1] = '\0';
}

static bool parse_int(const char *value, int *out)
{
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || *end != '\0')
        return false;
    if (v < 0 || v > INT32_MAX)
        return false;
    *out = (int)v;
    return true;
}

static bool load_config(const char *path, ServerConfig *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    cfg->host_name[0] = '\0';
    cfg->lobby_port = 0;
    cfg->game_port_min = 0;
    cfg->game_port_max = 0;
    cfg->max_games = DEFAULT_MAX_GAMES;
    cfg->max_players_default = DEFAULT_MAX_PLAYERS;
    cfg->join_timeout_sec = DEFAULT_JOIN_TIMEOUT_SEC;
    cfg->drop_timeout_sec = DEFAULT_DROP_TIMEOUT_SEC;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        str_trim(key);
        str_trim(value);
        if (*key == '\0' || *value == '\0')
            continue;

        if (strcmp(key, "host_name") == 0)
        {
            snprintf(cfg->host_name, sizeof(cfg->host_name), "%s", value);
        }
        else if (strcmp(key, "lobby_port") == 0)
        {
            parse_int(value, &cfg->lobby_port);
        }
        else if (strcmp(key, "game_port_min") == 0)
        {
            parse_int(value, &cfg->game_port_min);
        }
        else if (strcmp(key, "game_port_max") == 0)
        {
            parse_int(value, &cfg->game_port_max);
        }
        else if (strcmp(key, "max_games") == 0)
        {
            int v = 0;
            if (parse_int(value, &v))
                cfg->max_games = v;
        }
        else if (strcmp(key, "max_players_default") == 0)
        {
            int v = 0;
            if (parse_int(value, &v))
                cfg->max_players_default = v;
        }
        else if (strcmp(key, "join_timeout_sec") == 0)
        {
            int v = 0;
            if (parse_int(value, &v))
                cfg->join_timeout_sec = v;
        }
        else if (strcmp(key, "drop_timeout_sec") == 0)
        {
            int v = 0;
            if (parse_int(value, &v))
                cfg->drop_timeout_sec = v;
        }
    }

    fclose(f);
    return true;
}

static bool validate_config(const ServerConfig *cfg)
{
    if (cfg->host_name[0] == '\0')
        return false;
    if (cfg->lobby_port <= 0 || cfg->lobby_port > 65535)
        return false;
    if (cfg->game_port_min <= 0 || cfg->game_port_min > 65535)
        return false;
    if (cfg->game_port_max <= 0 || cfg->game_port_max > 65535)
        return false;
    if (cfg->game_port_min > cfg->game_port_max)
        return false;
    if (cfg->max_games <= 0 || cfg->max_games > MAX_GAMES_LIMIT)
        return false;
    if (cfg->max_players_default <= 0 || cfg->max_players_default > MAX_PLAYERS_LIMIT)
        return false;
    if (cfg->join_timeout_sec <= 0)
        return false;
    if (cfg->drop_timeout_sec <= 0)
        return false;
    return true;
}

static ssize_t recv_line(int fd, char *buf, size_t buf_len, int timeout_ms)
{
    size_t used = 0;
    while (used + 1 < buf_len)
    {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0)
            return -1;
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0)
            return r;
        if (c == '\n')
            break;
        buf[used++] = c;
    }
    buf[used] = '\0';
    return (ssize_t)used;
}

static void send_json(LobbyClient *client, const char *fmt, ...)
{
    char out[LINE_BUF];
    va_list args;
    va_start(args, fmt);
    vsnprintf(out, sizeof(out), fmt, args);
    va_end(args);

    size_t len = strlen(out);
    if (len + 2 < sizeof(out))
    {
        out[len] = '\n';
        out[len + 1] = '\0';
    }

    pthread_mutex_lock(&client->send_mu);
    send(client->fd, out, strlen(out), 0);
    pthread_mutex_unlock(&client->send_mu);
}

static bool json_get_string(const char *line, const char *key, char *out, size_t out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(line, pattern);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '"')
        return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len)
        out[i++] = *p++;
    out[i] = '\0';
    return *p == '"';
}

static bool json_get_int(const char *line, const char *key, int *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(line, pattern);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p)
        return false;
    if (v < INT32_MIN || v > INT32_MAX)
        return false;
    *out = (int)v;
    return true;
}

static int acquire_game_port(void)
{
    for (int i = 0; i < g_port_range; i++)
    {
        if (!g_port_used[i])
        {
            g_port_used[i] = true;
            return g_cfg.game_port_min + i;
        }
    }
    return -1;
}

static void release_game_port(int port)
{
    if (port < g_cfg.game_port_min || port > g_cfg.game_port_max)
        return;
    int idx = port - g_cfg.game_port_min;
    if (idx >= 0 && idx < g_port_range)
        g_port_used[idx] = false;
}

static Game *find_game_by_id_locked(const char *id)
{
    for (int i = 0; i < g_cfg.max_games; i++)
    {
        if (g_games[i].in_use && strcmp(g_games[i].id, id) == 0)
            return &g_games[i];
    }
    return NULL;
}

static void remove_client_from_game_locked(Game *game, LobbyClient *client)
{
    for (int i = 0; i < game->player_count; i++)
    {
        if (game->waiting_clients[i] == client)
        {
            int last = game->player_count - 1;
            if (i != last)
            {
                game->waiting_clients[i] = game->waiting_clients[last];
                memmove(game->player_names[i], game->player_names[last], sizeof(game->player_names[i]));
                memmove(game->tokens[i], game->tokens[last], sizeof(game->tokens[i]));
            }
            game->waiting_clients[last] = NULL;
            game->player_names[last][0] = '\0';
            game->tokens[last][0] = '\0';
            game->player_count--;
            return;
        }
    }
}

static void remove_client_from_all_games_locked(LobbyClient *client)
{
    for (int i = 0; i < g_cfg.max_games; i++)
    {
        if (!g_games[i].in_use || g_games[i].active || g_games[i].ended)
            continue;
        remove_client_from_game_locked(&g_games[i], client);
    }
}

static void notify_start_and_close(Game *game)
{
    for (int i = 0; i < game->player_count; i++)
    {
        LobbyClient *client = game->waiting_clients[i];
        if (!client || !client->connected)
            continue;
        send_json(client,
                  "{\"cmd\":\"start\",\"host\":\"%s\",\"port\":%d,\"token\":\"%s\"}",
                  g_cfg.host_name, game->port, game->tokens[i]);
        shutdown(client->fd, SHUT_RDWR);
        close(client->fd);
        client->connected = false;
    }
}

static void end_game(Game *game)
{
    pthread_mutex_lock(&g_games_mu);
    game->in_use = false;
    game->active = false;
    game->ended = true;
    release_game_port(game->port);
    pthread_mutex_unlock(&g_games_mu);
}

static void remove_players_from_other_games_locked(Game *game)
{
    for (int i = 0; i < game->player_count; i++)
    {
        LobbyClient *client = game->waiting_clients[i];
        if (!client)
            continue;
        for (int gi = 0; gi < g_cfg.max_games; gi++)
        {
            if (!g_games[gi].in_use || &g_games[gi] == game)
                continue;
            remove_client_from_game_locked(&g_games[gi], client);
        }
    }
}

static void *game_thread(void *arg)
{
    GameThreadArgs *args = (GameThreadArgs *)arg;
    Game *game = args->game;
    int max_players = game->max_players;
    int listen_fd = -1;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("game socket");
        end_game(game);
        free(args);
        return NULL;
    }

    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(game->port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("game bind");
        close(sockfd);
        end_game(game);
        free(args);
        return NULL;
    }

    if (listen(sockfd, max_players) < 0)
    {
        perror("game listen");
        close(sockfd);
        end_game(game);
        free(args);
        return NULL;
    }

    listen_fd = sockfd;

    int fds[MAX_PLAYERS_LIMIT];
    bool connected[MAX_PLAYERS_LIMIT];
    for (int i = 0; i < max_players; i++)
    {
        fds[i] = -1;
        connected[i] = false;
    }

    uint64_t drop_deadline = 0;

    while (1)
    {
        struct pollfd pfds[MAX_PLAYERS_LIMIT + 1];
        int idx_map[MAX_PLAYERS_LIMIT + 1];
        int pcount = 0;

        pfds[pcount].fd = listen_fd;
        pfds[pcount].events = POLLIN;
        pfds[pcount].revents = 0;
        idx_map[pcount] = -1;
        pcount++;

        for (int i = 0; i < max_players; i++)
        {
            if (fds[i] >= 0)
            {
                pfds[pcount].fd = fds[i];
                pfds[pcount].events = POLLIN;
                pfds[pcount].revents = 0;
                idx_map[pcount] = i;
                pcount++;
            }
        }

        int poll_ret = poll(pfds, pcount, 200);
        uint64_t now_ms = get_time_ms();

        if (poll_ret < 0)
        {
            if (errno == EINTR)
                continue;
            perror("game poll");
            break;
        }

        if (drop_deadline > 0 && now_ms >= drop_deadline)
        {
            printf("Game %s ended due to drop timeout\n", game->id);
            break;
        }

        if (poll_ret == 0)
            continue;

        for (int i = 0; i < pcount; i++)
        {
            if ((pfds[i].revents & POLLIN) == 0)
                continue;

            if (idx_map[i] == -1)
            {
                struct sockaddr_in cliaddr;
                socklen_t clilen = sizeof(cliaddr);
                int client_fd = accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
                if (client_fd < 0)
                    continue;

                char buf[LINE_BUF];
                ssize_t r = recv(client_fd, buf, sizeof(buf) - 1, 0);
                if (r <= 0)
                {
                    close(client_fd);
                    continue;
                }
                buf[r] = '\0';

                if (strncmp(buf, "REGISTER", 8) != 0)
                {
                    close(client_fd);
                    continue;
                }

                int slot = -1;
                for (int s = 0; s < max_players; s++)
                {
                    if (!connected[s])
                    {
                        slot = s;
                        break;
                    }
                }

                if (slot < 0)
                {
                    close(client_fd);
                    continue;
                }

                if (fds[slot] >= 0)
                    close(fds[slot]);
                fds[slot] = client_fd;
                connected[slot] = true;
                bool all_connected = true;
                for (int s = 0; s < max_players; s++)
                {
                    if (!connected[s])
                    {
                        all_connected = false;
                        break;
                    }
                }
                if (all_connected)
                    drop_deadline = 0;
                continue;
            }

            int slot = idx_map[i];
            char buf[2048];
            ssize_t r = recv(fds[slot], buf, sizeof(buf), 0);
            if (r <= 0)
            {
                close(fds[slot]);
                fds[slot] = -1;
                connected[slot] = false;
                if (drop_deadline == 0)
                    drop_deadline = now_ms + (uint64_t)g_cfg.drop_timeout_sec * 1000u;
                continue;
            }
            bool all_connected = true;
            for (int s = 0; s < max_players; s++)
            {
                if (!connected[s])
                {
                    all_connected = false;
                    break;
                }
            }
            if (!all_connected)
                continue;

            int next = (slot + 1) % max_players;
            if (fds[next] >= 0)
            {
                ssize_t sent = send(fds[next], buf, (size_t)r, 0);
                if (sent < 0)
                {
                    close(fds[next]);
                    fds[next] = -1;
                    connected[next] = false;
                    if (drop_deadline == 0)
                        drop_deadline = now_ms + (uint64_t)g_cfg.drop_timeout_sec * 1000u;
                }
            }
        }
    }

    for (int i = 0; i < max_players; i++)
    {
        if (fds[i] >= 0)
            close(fds[i]);
    }
    if (listen_fd >= 0)
        close(listen_fd);

    end_game(game);
    free(args);
    return NULL;
}

static void start_game_locked(Game *game)
{
    int port = acquire_game_port();
    if (port < 0)
    {
        printf("No available game ports\n");
        for (int i = 0; i < game->player_count; i++)
        {
            LobbyClient *client = game->waiting_clients[i];
            if (client && client->connected)
                send_json(client, "{\"ok\":false,\"error\":\"no_ports\"}");
        }
        game->in_use = false;
        return;
    }

    game->port = port;
    game->active = true;

    GameThreadArgs *args = calloc(1, sizeof(GameThreadArgs));
    args->game = game;
    args->cfg = &g_cfg;

    pthread_create(&game->thread, NULL, game_thread, args);

    remove_players_from_other_games_locked(game);
    notify_start_and_close(game);
}

static void send_game_list(LobbyClient *client)
{
    pthread_mutex_lock(&g_games_mu);
    char out[LINE_BUF];
    size_t out_used = 0;
    out_used += (size_t)snprintf(out + out_used, sizeof(out) - out_used, "{\"ok\":true,\"games\":[");
    bool first = true;
    for (int i = 0; i < g_cfg.max_games; i++)
    {
        if (!g_games[i].in_use)
            continue;
        if (!first)
            out_used += (size_t)snprintf(out + out_used, sizeof(out) - out_used, ",");
        first = false;
        char entry[LINE_BUF];
        char names[LINE_BUF];
        size_t names_used = 0;
        names[0] = '\0';
        names_used += (size_t)snprintf(names + names_used, sizeof(names) - names_used, "[");
        for (int p = 0; p < g_games[i].player_count; p++)
        {
            if (p > 0)
                names_used += (size_t)snprintf(names + names_used, sizeof(names) - names_used, ",");
            names_used += (size_t)snprintf(names + names_used, sizeof(names) - names_used, "\"%s\"",
                                           g_games[i].player_names[p]);
        }
        names_used += (size_t)snprintf(names + names_used, sizeof(names) - names_used, "]");
        snprintf(entry, sizeof(entry),
                 "{\"id\":\"%s\",\"name\":\"%s\",\"players\":%d,\"max\":%d,\"active\":%s,\"player_names\":%s}",
                 g_games[i].id, g_games[i].name, g_games[i].player_count,
                 g_games[i].max_players, g_games[i].active ? "true" : "false", names);
        out_used += (size_t)snprintf(out + out_used, sizeof(out) - out_used, "%s", entry);
    }
    out_used += (size_t)snprintf(out + out_used, sizeof(out) - out_used, "]}");
    pthread_mutex_unlock(&g_games_mu);

    send_json(client, "%s", out);
}

static void expire_pending_games(void)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&g_games_mu);
    for (int i = 0; i < g_cfg.max_games; i++)
    {
        Game *game = &g_games[i];
        if (!game->in_use || game->active || game->ended)
            continue;
        if ((now - game->created_at) > g_cfg.join_timeout_sec)
        {
            for (int p = 0; p < game->player_count; p++)
            {
                LobbyClient *client = game->waiting_clients[p];
                if (client && client->connected)
                {
                    send_json(client, "{\"ok\":false,\"error\":\"timeout\"}");
                }
            }
            game->in_use = false;
        }
    }
    pthread_mutex_unlock(&g_games_mu);
}

static void *maintenance_thread(void *arg)
{
    (void)arg;
    while (1)
    {
        expire_pending_games();
        sleep(1);
    }
    return NULL;
}

static void *client_thread(void *arg)
{
    LobbyClient *client = (LobbyClient *)arg;
    char line[LINE_BUF];

    ssize_t len = recv_line(client->fd, line, sizeof(line), 10000);
    if (len <= 0)
        goto done;

    char cmd[32];
    if (!json_get_string(line, "cmd", cmd, sizeof(cmd)) || strcmp(cmd, "hello") != 0)
        goto done;

    char name[NAME_MAX + 1];
    if (!json_get_string(line, "name", name, sizeof(name)))
        goto done;

    if (!is_alnum_str(name) || strlen(name) > NAME_MAX)
    {
        send_json(client, "{\"ok\":false,\"error\":\"invalid_name\"}");
        goto done;
    }

    snprintf(client->name, sizeof(client->name), "%s", name);
    send_json(client, "{\"ok\":true,\"name\":\"%s\"}", client->name);

    while (1)
    {
        ssize_t r = recv_line(client->fd, line, sizeof(line), 30000);
        if (r <= 0)
            break;

        if (!json_get_string(line, "cmd", cmd, sizeof(cmd)))
        {
            send_json(client, "{\"ok\":false,\"error\":\"bad_cmd\"}");
            continue;
        }

        if (strcmp(cmd, "list") == 0)
        {
            send_game_list(client);
            continue;
        }
        if (strcmp(cmd, "heartbeat") == 0)
        {
            send_json(client, "{\"ok\":true}");
            continue;
        }

        if (strcmp(cmd, "create") == 0)
        {
            char gname[GAME_NAME_MAX + 1] = "";
            json_get_string(line, "name", gname, sizeof(gname));
            int max_players = g_cfg.max_players_default;
            json_get_int(line, "max_players", &max_players);
            if (max_players <= 0 || max_players > MAX_PLAYERS_LIMIT)
                max_players = g_cfg.max_players_default;

            pthread_mutex_lock(&g_games_mu);
            int slot = -1;
            int in_use = 0;
            for (int i = 0; i < g_cfg.max_games; i++)
            {
                if (g_games[i].in_use)
                    in_use++;
                else if (slot < 0)
                    slot = i;
            }
            if (in_use >= g_cfg.max_games || slot < 0)
            {
                pthread_mutex_unlock(&g_games_mu);
                send_json(client, "{\"ok\":false,\"error\":\"max_games\"}");
                continue;
            }

            Game *game = &g_games[slot];
            memset(game, 0, sizeof(*game));
            game->in_use = true;
            game->active = false;
            game->ended = false;
            game->max_players = max_players;
            game->player_count = 0;
            game->created_at = time(NULL);
            snprintf(game->name, sizeof(game->name), "%s", gname[0] ? gname : "Game");
            gen_id(game->id, sizeof(game->id));

            game->waiting_clients[0] = client;
            snprintf(game->player_names[0], sizeof(game->player_names[0]), "%s", client->name);
            gen_id(game->tokens[0], sizeof(game->tokens[0]));
            game->player_count = 1;
            pthread_mutex_unlock(&g_games_mu);

            send_json(client, "{\"ok\":true,\"game_id\":\"%s\",\"status\":\"waiting\"}", game->id);
            continue;
        }

        if (strcmp(cmd, "join") == 0)
        {
            char game_id[GAME_ID_LEN + 1];
            if (!json_get_string(line, "game_id", game_id, sizeof(game_id)))
            {
                send_json(client, "{\"ok\":false,\"error\":\"missing_game_id\"}");
                continue;
            }

            pthread_mutex_lock(&g_games_mu);
            Game *game = find_game_by_id_locked(game_id);
            if (!game || game->active)
            {
                pthread_mutex_unlock(&g_games_mu);
                send_json(client, "{\"ok\":false,\"error\":\"not_found\"}");
                continue;
            }
            if (game->player_count >= game->max_players)
            {
                pthread_mutex_unlock(&g_games_mu);
                send_json(client, "{\"ok\":false,\"error\":\"full\"}");
                continue;
            }

            int idx = game->player_count++;
            game->waiting_clients[idx] = client;
            snprintf(game->player_names[idx], sizeof(game->player_names[idx]), "%s", client->name);
            gen_id(game->tokens[idx], sizeof(game->tokens[idx]));

            if (game->player_count >= game->max_players)
            {
                start_game_locked(game);
                pthread_mutex_unlock(&g_games_mu);
                break;
            }
            pthread_mutex_unlock(&g_games_mu);

            send_json(client, "{\"ok\":true,\"status\":\"waiting\"}");
            continue;
        }

        if (strcmp(cmd, "leave") == 0)
        {
            char game_id[GAME_ID_LEN + 1];
            if (!json_get_string(line, "game_id", game_id, sizeof(game_id)))
            {
                send_json(client, "{\"ok\":false,\"error\":\"missing_game_id\"}");
                continue;
            }
            pthread_mutex_lock(&g_games_mu);
            Game *game = find_game_by_id_locked(game_id);
            if (!game || game->active)
            {
                pthread_mutex_unlock(&g_games_mu);
                send_json(client, "{\"ok\":false,\"error\":\"not_found\"}");
                continue;
            }
            remove_client_from_game_locked(game, client);
            pthread_mutex_unlock(&g_games_mu);
            send_json(client, "{\"ok\":true}");
            continue;
        }

        send_json(client, "{\"ok\":false,\"error\":\"unknown_cmd\"}");
    }

done:
    pthread_mutex_lock(&g_games_mu);
    remove_client_from_all_games_locked(client);
    pthread_mutex_unlock(&g_games_mu);
    if (client->fd >= 0)
        close(client->fd);
    client->connected = false;
    pthread_mutex_destroy(&client->send_mu);
    free(client);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        return 1;
    }

    if (!load_config(argv[1], &g_cfg))
    {
        fprintf(stderr, "Failed to load config: %s\n", argv[1]);
        return 1;
    }
    if (!validate_config(&g_cfg))
    {
        fprintf(stderr, "Invalid config\n");
        return 1;
    }

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    g_port_range = g_cfg.game_port_max - g_cfg.game_port_min + 1;
    g_port_used = calloc((size_t)g_port_range, sizeof(bool));
    if (!g_port_used)
    {
        fprintf(stderr, "Failed to allocate port map\n");
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(g_cfg.lobby_port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("bind");
        close(sockfd);
        return 1;
    }

    if (listen(sockfd, 16) < 0)
    {
        perror("listen");
        close(sockfd);
        return 1;
    }

    pthread_t maint;
    pthread_create(&maint, NULL, maintenance_thread, NULL);

    printf("Lobby listening on port %d, host %s\n", g_cfg.lobby_port, g_cfg.host_name);

    while (1)
    {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int client_fd = accept(sockfd, (struct sockaddr *)&cliaddr, &clilen);
        if (client_fd < 0)
            continue;

        LobbyClient *client = calloc(1, sizeof(LobbyClient));
        if (!client)
        {
            close(client_fd);
            continue;
        }
        client->fd = client_fd;
        client->connected = true;
        gen_id(client->id, sizeof(client->id));
        pthread_mutex_init(&client->send_mu, NULL);

        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, client);
        pthread_detach(tid);
    }

    return 0;
}
