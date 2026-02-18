#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LINE_BUF 512
#define REQ_BUF 1024
#define NAME_MAX 8
#define GAME_NAME_MAX 32
#define GAME_ID_LEN 8
#define TOKEN_LEN 16
#define MAX_GAMES_LIMIT 32
#define MAX_PLAYERS_LIMIT 16
#define MAX_CLIENTS_LIMIT 64
#define DEFAULT_MAX_GAMES 5
#define DEFAULT_MAX_PLAYERS 10
#define DEFAULT_JOIN_TIMEOUT_SEC 600
#define DEFAULT_DROP_TIMEOUT_SEC 15
#define DEFAULT_IDLE_TIMEOUT_SEC 600
#define DEFAULT_UDP_DUP_DELAY_MS 15
#define DIAG_INTERVAL_MS 10000
#define GAME_SOCKET_BUF_BYTES 16384
#define DUP_QUEUE_CAP 256

typedef enum
{
    TRANSPORT_TCP = 0,
    TRANSPORT_UDP = 1
} TransportMode;

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
    int idle_timeout_sec;
    int udp_dup_enabled;
    int udp_dup_delay_ms;
} ServerConfig;

typedef struct
{
    bool in_use;
    char id[GAME_ID_LEN + 1];
    char name[NAME_MAX + 1];
    time_t last_seen;
    bool pending_start;
    int start_port;
    char start_host[256];
    TransportMode start_transport;
} LobbyClient;

typedef struct
{
    bool in_use;
    bool active;
    bool ended;
    char id[GAME_ID_LEN + 1];
    char name[GAME_NAME_MAX + 1];
    int max_players;
    int player_count;
    int port;
    TransportMode transport;
    time_t created_at;
    char player_ids[MAX_PLAYERS_LIMIT][GAME_ID_LEN + 1];
    char player_names[MAX_PLAYERS_LIMIT][NAME_MAX + 1];
    char tokens[MAX_PLAYERS_LIMIT][TOKEN_LEN + 1];
    pthread_t thread;
} Game;

typedef struct
{
    Game *game;
    ServerConfig *cfg;
} GameThreadArgs;

typedef struct
{
    bool in_use;
    uint64_t due_ms;
    struct sockaddr_in dst;
    socklen_t dst_len;
    size_t len;
    uint8_t data[2048];
} DupPacket;

static ServerConfig g_cfg;
static Game g_games[MAX_GAMES_LIMIT];
static LobbyClient g_clients[MAX_CLIENTS_LIMIT];
static bool *g_port_used = NULL;
static int g_port_range = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

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

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static TransportMode transport_from_query(const char *value)
{
    if (value)
    {
        while (*value == ' ' || *value == '\t')
            value++;
        if ((value[0] == 'u' || value[0] == 'U') &&
            (value[1] == 'd' || value[1] == 'D') &&
            (value[2] == 'p' || value[2] == 'P'))
            return TRANSPORT_UDP;
    }
    return TRANSPORT_TCP;
}

static const char *transport_to_string(TransportMode mode)
{
    return mode == TRANSPORT_UDP ? "udp" : "tcp";
}

static void tune_socket_common(int fd)
{
    int buf = GAME_SOCKET_BUF_BYTES;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
}

static void tune_socket_tcp_stream(int fd)
{
    int one = 1;
    tune_socket_common(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static bool sockaddr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_family == b->sin_family &&
           a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static bool payload_is_register(const uint8_t *buf, size_t len)
{
    if (len >= 8 && memcmp(buf, "REGISTER", 8) == 0)
        return true;
    if (len >= 10 && memcmp(buf + 2, "REGISTER", 8) == 0)
        return true;
    return false;
}

static bool seq_is_newer(uint16_t a, uint16_t b)
{
    uint16_t d = (uint16_t)(a - b);
    return d != 0 && d < 0x8000u;
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
    cfg->idle_timeout_sec = DEFAULT_IDLE_TIMEOUT_SEC;
    cfg->udp_dup_enabled = 1;
    cfg->udp_dup_delay_ms = DEFAULT_UDP_DUP_DELAY_MS;

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
            snprintf(cfg->host_name, sizeof(cfg->host_name), "%s", value);
        else if (strcmp(key, "lobby_port") == 0)
            parse_int(value, &cfg->lobby_port);
        else if (strcmp(key, "game_port_min") == 0)
            parse_int(value, &cfg->game_port_min);
        else if (strcmp(key, "game_port_max") == 0)
            parse_int(value, &cfg->game_port_max);
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
        else if (strcmp(key, "idle_timeout_sec") == 0)
        {
            int v = 0;
            if (parse_int(value, &v))
                cfg->idle_timeout_sec = v;
        }
        else if (strcmp(key, "udp_dup_delay_ms") == 0)
        {
            int v = 0;
            if (parse_int(value, &v))
                cfg->udp_dup_delay_ms = v;
        }
        else if (strcmp(key, "udp_dup_enabled") == 0)
        {
            int v = 0;
            if (parse_int(value, &v))
                cfg->udp_dup_enabled = v ? 1 : 0;
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
    if (cfg->idle_timeout_sec <= 0)
        return false;
    if (cfg->udp_dup_enabled != 0 && cfg->udp_dup_enabled != 1)
        return false;
    if (cfg->udp_dup_delay_ms < 0 || cfg->udp_dup_delay_ms > 1000)
        return false;
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

static LobbyClient *find_client_by_id_locked(const char *id)
{
    for (int i = 0; i < MAX_CLIENTS_LIMIT; i++)
    {
        if (g_clients[i].in_use && strcmp(g_clients[i].id, id) == 0)
            return &g_clients[i];
    }
    return NULL;
}

static LobbyClient *create_client_locked(const char *name)
{
    for (int i = 0; i < MAX_CLIENTS_LIMIT; i++)
    {
        if (!g_clients[i].in_use)
        {
            g_clients[i].in_use = true;
            gen_id(g_clients[i].id, sizeof(g_clients[i].id));
            snprintf(g_clients[i].name, sizeof(g_clients[i].name), "%s", name);
            g_clients[i].last_seen = time(NULL);
            g_clients[i].pending_start = false;
            g_clients[i].start_port = 0;
            g_clients[i].start_host[0] = '\0';
            g_clients[i].start_transport = TRANSPORT_TCP;
            return &g_clients[i];
        }
    }
    return NULL;
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

static void remove_client_from_game_locked(Game *game, const char *client_id)
{
    for (int i = 0; i < game->player_count; i++)
    {
        if (strcmp(game->player_ids[i], client_id) == 0)
        {
            int last = game->player_count - 1;
            if (i != last)
            {
                memmove(game->player_ids[i], game->player_ids[last], sizeof(game->player_ids[i]));
                memmove(game->player_names[i], game->player_names[last], sizeof(game->player_names[i]));
                memmove(game->tokens[i], game->tokens[last], sizeof(game->tokens[i]));
            }
            game->player_ids[last][0] = '\0';
            game->player_names[last][0] = '\0';
            game->tokens[last][0] = '\0';
            game->player_count--;
            return;
        }
    }
}

static void end_game(Game *game)
{
    pthread_mutex_lock(&g_lock);
    game->in_use = false;
    game->active = false;
    game->ended = true;
    release_game_port(game->port);
    pthread_mutex_unlock(&g_lock);
}

static void *game_thread(void *arg)
{
    GameThreadArgs *args = (GameThreadArgs *)arg;
    Game *game = args->game;
    int max_players = game->max_players;

    int sockfd = socket(AF_INET, game->transport == TRANSPORT_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("game socket");
        end_game(game);
        free(args);
        return NULL;
    }

    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    tune_socket_common(sockfd);

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

    if (game->transport == TRANSPORT_TCP && listen(sockfd, max_players) < 0)
    {
        perror("game listen");
        close(sockfd);
        end_game(game);
        free(args);
        return NULL;
    }

    int fds[MAX_PLAYERS_LIMIT];
    bool connected[MAX_PLAYERS_LIMIT];
    struct sockaddr_in udp_peers[MAX_PLAYERS_LIMIT];
    bool seq_seen[MAX_PLAYERS_LIMIT];
    uint16_t seq_expected[MAX_PLAYERS_LIMIT];
    uint16_t seq_last[MAX_PLAYERS_LIMIT];
    DupPacket dupq[DUP_QUEUE_CAP];
    for (int i = 0; i < max_players; i++)
    {
        fds[i] = -1;
        connected[i] = false;
        memset(&udp_peers[i], 0, sizeof(udp_peers[i]));
        seq_seen[i] = false;
        seq_expected[i] = 0;
        seq_last[i] = 0;
    }
    for (int i = 0; i < DUP_QUEUE_CAP; i++)
    {
        dupq[i].in_use = false;
    }

    uint64_t drop_deadline = now_ms() + (uint64_t)g_cfg.drop_timeout_sec * 1000u;
    uint64_t last_activity_ms = now_ms();
    uint64_t next_diag_ms = now_ms() + DIAG_INTERVAL_MS;
    uint64_t rx_packets = 0;
    uint64_t tx_packets = 0;
    uint64_t dup_packets = 0;
    uint64_t register_packets = 0;
    uint64_t drop_packets = 0;
    uint64_t unknown_packets = 0;
    uint64_t seq_init = 0;
    uint64_t seq_in_order = 0;
    uint64_t seq_ahead = 0;
    uint64_t seq_behind = 0;
    uint64_t seq_dup = 0;
    uint64_t seq_gap_pkts = 0;
    uint64_t seq_short = 0;
    uint16_t seq_max_gap = 0;

    while (1)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        int maxfd = sockfd;

        if (game->transport == TRANSPORT_TCP)
        {
            for (int i = 0; i < max_players; i++)
            {
                if (fds[i] >= 0)
                {
                    FD_SET(fds[i], &rfds);
                    if (fds[i] > maxfd)
                        maxfd = fds[i];
                }
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = game->transport == TRANSPORT_UDP ? 10000 : 20000;
        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0)
        {
            if (errno == EINTR)
                continue;
            perror("game select");
            break;
        }

        uint64_t now = now_ms();

        for (int i = 0; i < DUP_QUEUE_CAP; i++)
        {
            if (!dupq[i].in_use || dupq[i].due_ms > now)
                continue;
            if (sendto(sockfd, dupq[i].data, dupq[i].len, 0, (struct sockaddr *)&dupq[i].dst, dupq[i].dst_len) >= 0)
            {
                tx_packets++;
                dup_packets++;
            }
            else
            {
                drop_packets++;
            }
            dupq[i].in_use = false;
        }

        if (now >= next_diag_ms)
        {
            printf("Game %s mode=%s stats rx=%llu tx=%llu dup_tx=%llu reg=%llu drop=%llu unknown=%llu seq_i=%llu seq_o=%llu seq_l=%llu seq_dup=%llu seq_gap=%llu seq_maxgap=%u seq_short=%llu\n",
                   game->id, transport_to_string(game->transport),
                   (unsigned long long)rx_packets, (unsigned long long)tx_packets,
                   (unsigned long long)dup_packets, (unsigned long long)register_packets,
                   (unsigned long long)drop_packets, (unsigned long long)unknown_packets,
                   (unsigned long long)(seq_in_order + seq_init), (unsigned long long)seq_ahead,
                   (unsigned long long)seq_behind, (unsigned long long)seq_dup,
                   (unsigned long long)seq_gap_pkts, (unsigned)seq_max_gap,
                   (unsigned long long)seq_short);
            next_diag_ms = now + DIAG_INTERVAL_MS;
        }

        if (drop_deadline > 0 && now >= drop_deadline)
        {
            printf("Game %s ended due to drop timeout\n", game->id);
            break;
        }
        if (g_cfg.idle_timeout_sec > 0 &&
            now - last_activity_ms >= (uint64_t)g_cfg.idle_timeout_sec * 1000u)
        {
            printf("Game %s ended due to idle timeout\n", game->id);
            break;
        }

        if (rv == 0)
            continue;

        if (game->transport == TRANSPORT_TCP && FD_ISSET(sockfd, &rfds))
        {
            struct sockaddr_in cliaddr;
            socklen_t clilen = sizeof(cliaddr);
            int client_fd = accept(sockfd, (struct sockaddr *)&cliaddr, &clilen);
            if (client_fd >= 0)
            {
                tune_socket_tcp_stream(client_fd);
                char buf[32];
                ssize_t r = recv(client_fd, buf, sizeof(buf), 0);
                if (r <= 0 || strncmp(buf, "REGISTER", 8) != 0)
                {
                    close(client_fd);
                }
                else
                {
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
                    }
                    else
                    {
                        if (fds[slot] >= 0)
                            close(fds[slot]);
                        fds[slot] = client_fd;
                        connected[slot] = true;
                        register_packets++;
                        last_activity_ms = now;

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
                    }
                }
            }
        }

        if (game->transport == TRANSPORT_TCP)
        {
            for (int i = 0; i < max_players; i++)
            {
                if (fds[i] < 0)
                    continue;
                if (!FD_ISSET(fds[i], &rfds))
                    continue;

                char buf[2048];
                ssize_t r = recv(fds[i], buf, sizeof(buf), 0);
                if (r <= 0)
                {
                    close(fds[i]);
                    fds[i] = -1;
                    connected[i] = false;
                    drop_packets++;
                    if (drop_deadline == 0)
                        drop_deadline = now + (uint64_t)g_cfg.drop_timeout_sec * 1000u;
                    continue;
                }
                rx_packets++;
                last_activity_ms = now;

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

                int next = (i + 1) % max_players;
                if (fds[next] >= 0)
                {
                    ssize_t sent = send(fds[next], buf, (size_t)r, 0);
                    if (sent < 0)
                    {
                        close(fds[next]);
                        fds[next] = -1;
                        connected[next] = false;
                        drop_packets++;
                        if (drop_deadline == 0)
                            drop_deadline = now + (uint64_t)g_cfg.drop_timeout_sec * 1000u;
                    }
                    else
                    {
                        tx_packets++;
                    }
                }
            }
        }
        else if (FD_ISSET(sockfd, &rfds))
        {
            uint8_t buf[2048];
            struct sockaddr_in src;
            socklen_t srclen = sizeof(src);
            ssize_t r = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
            if (r <= 0)
                continue;

            rx_packets++;
            last_activity_ms = now;

            int slot = -1;
            for (int i = 0; i < max_players; i++)
            {
                if (connected[i] && sockaddr_equal(&udp_peers[i], &src))
                {
                    slot = i;
                    break;
                }
            }

            if (slot < 0)
            {
                if (!payload_is_register(buf, (size_t)r))
                {
                    unknown_packets++;
                    continue;
                }
                for (int i = 0; i < max_players; i++)
                {
                    if (!connected[i])
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0)
                {
                    drop_packets++;
                    continue;
                }

                connected[slot] = true;
                udp_peers[slot] = src;
                register_packets++;

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

            if (!payload_is_register(buf, (size_t)r))
            {
                if ((size_t)r < 2)
                {
                    seq_short++;
                }
                else
                {
                    uint16_t seq = ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
                    if (!seq_seen[slot])
                    {
                        seq_seen[slot] = true;
                        seq_expected[slot] = (uint16_t)(seq + 1u);
                        seq_last[slot] = seq;
                        seq_init++;
                    }
                    else if (seq == seq_expected[slot])
                    {
                        seq_expected[slot] = (uint16_t)(seq_expected[slot] + 1u);
                        seq_last[slot] = seq;
                        seq_in_order++;
                    }
                    else if (seq_is_newer(seq, seq_expected[slot]))
                    {
                        uint16_t gap = (uint16_t)(seq - seq_expected[slot]);
                        seq_ahead++;
                        seq_gap_pkts += gap;
                        if (gap > seq_max_gap)
                            seq_max_gap = gap;
                        seq_expected[slot] = (uint16_t)(seq + 1u);
                        seq_last[slot] = seq;
                    }
                    else
                    {
                        seq_behind++;
                        if (seq == seq_last[slot])
                            seq_dup++;
                        seq_last[slot] = seq;
                    }
                }
            }

            int next = (slot + 1) % max_players;
            if (!connected[next])
            {
                drop_packets++;
                continue;
            }

            if (sendto(sockfd, buf, (size_t)r, 0, (struct sockaddr *)&udp_peers[next], sizeof(udp_peers[next])) < 0)
            {
                drop_packets++;
                continue;
            }
            tx_packets++;

            bool queued = false;
            if (g_cfg.udp_dup_enabled)
            {
                for (int i = 0; i < DUP_QUEUE_CAP; i++)
                {
                    if (dupq[i].in_use)
                        continue;
                    dupq[i].in_use = true;
                    dupq[i].due_ms = now + (uint64_t)g_cfg.udp_dup_delay_ms;
                    dupq[i].dst = udp_peers[next];
                    dupq[i].dst_len = sizeof(udp_peers[next]);
                    dupq[i].len = (size_t)r;
                    memcpy(dupq[i].data, buf, (size_t)r);
                    queued = true;
                    break;
                }
            }
            if (g_cfg.udp_dup_enabled && !queued)
                drop_packets++;
        }
    }

    for (int i = 0; i < max_players; i++)
    {
        if (fds[i] >= 0)
            close(fds[i]);
    }
    close(sockfd);

    printf("Game %s final mode=%s stats rx=%llu tx=%llu dup_tx=%llu reg=%llu drop=%llu unknown=%llu seq_i=%llu seq_o=%llu seq_l=%llu seq_dup=%llu seq_gap=%llu seq_maxgap=%u seq_short=%llu\n",
           game->id, transport_to_string(game->transport),
           (unsigned long long)rx_packets, (unsigned long long)tx_packets,
           (unsigned long long)dup_packets, (unsigned long long)register_packets,
           (unsigned long long)drop_packets, (unsigned long long)unknown_packets,
           (unsigned long long)(seq_in_order + seq_init), (unsigned long long)seq_ahead,
           (unsigned long long)seq_behind, (unsigned long long)seq_dup,
           (unsigned long long)seq_gap_pkts, (unsigned)seq_max_gap,
           (unsigned long long)seq_short);

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
        game->in_use = false;
        return;
    }

    game->port = port;
    game->active = true;

    {
        char ts[32];
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
        printf("%s Game start id=%s name=\"%s\" mode=%s port=%d players=%d dup=%s dup_delay_ms=%d names=",
               ts, game->id, game->name, transport_to_string(game->transport), game->port, game->player_count,
               g_cfg.udp_dup_enabled ? "on" : "off", g_cfg.udp_dup_delay_ms);
        for (int i = 0; i < game->player_count; i++)
        {
            if (i > 0)
                printf(",");
            printf("%s", game->player_names[i]);
        }
        printf("\n");
    }

    GameThreadArgs *args = calloc(1, sizeof(GameThreadArgs));
    args->game = game;
    args->cfg = &g_cfg;
    pthread_create(&game->thread, NULL, game_thread, args);

    for (int i = 0; i < game->player_count; i++)
    {
        LobbyClient *client = find_client_by_id_locked(game->player_ids[i]);
        if (client)
        {
            client->pending_start = true;
            client->start_port = game->port;
            snprintf(client->start_host, sizeof(client->start_host), "%s", g_cfg.host_name);
            client->start_transport = game->transport;
        }
    }

    for (int i = 0; i < game->player_count; i++)
    {
        for (int gi = 0; gi < g_cfg.max_games; gi++)
        {
            if (!g_games[gi].in_use || &g_games[gi] == game)
                continue;
            remove_client_from_game_locked(&g_games[gi], game->player_ids[i]);
        }
    }
}

static void expire_pending_games(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < g_cfg.max_games; i++)
    {
        Game *game = &g_games[i];
        if (!game->in_use || game->active || game->ended)
            continue;
        if ((now - game->created_at) > g_cfg.join_timeout_sec)
        {
            char ts[32];
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
            printf("%s Game timeout id=%s name=\"%s\"\n", ts, game->id, game->name);
            game->in_use = false;
        }
    }
}

static void expire_clients(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CLIENTS_LIMIT; i++)
    {
        if (!g_clients[i].in_use)
            continue;
        if ((now - g_clients[i].last_seen) > 3600)
            g_clients[i].in_use = false;
    }
}

static void send_http(int fd, const char *body)
{
    char header[128];
    int len = (int)strlen(body);
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n",
             len);
    send(fd, header, strlen(header), 0);
    send(fd, body, len, 0);
}

static void get_query_param(const char *query, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!query || !*query)
        return;
    size_t key_len = strlen(key);
    const char *p = query;
    while (*p)
    {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=')
        {
            p += key_len + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < out_len)
            {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return;
        }
        p = strchr(p, '&');
        if (!p)
            break;
        p++;
    }
}

static void url_decode(char *s)
{
    char *o = s;
    char *p = s;
    while (*p)
    {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]))
        {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 3;
        }
        else if (*p == '+')
        {
            *o++ = ' ';
            p++;
        }
        else
        {
            *o++ = *p++;
        }
    }
    *o = '\0';
}

static void handle_request(int fd, const char *path, const char *query)
{
    char name[NAME_MAX + 1];
    char client_id[GAME_ID_LEN + 1];
    char game_id[GAME_ID_LEN + 1];
    char game_name[GAME_NAME_MAX + 1];
    char max_players_str[8];
    char transport_str[8];
    int max_players = 0;
    TransportMode transport = TRANSPORT_TCP;

    if (strcmp(path, "/hello") == 0)
    {
        get_query_param(query, "name", name, sizeof(name));
        url_decode(name);
        if (!is_alnum_str(name) || strlen(name) > NAME_MAX)
        {
            send_http(fd, "{\"ok\":false,\"error\":\"invalid_name\"}");
            return;
        }
        pthread_mutex_lock(&g_lock);
        LobbyClient *client = create_client_locked(name);
        pthread_mutex_unlock(&g_lock);
        if (!client)
        {
            send_http(fd, "{\"ok\":false,\"error\":\"server_full\"}");
            return;
        }
        char body[LINE_BUF];
        snprintf(body, sizeof(body), "{\"ok\":true,\"client_id\":\"%s\",\"name\":\"%s\"}",
                 client->id, client->name);
        send_http(fd, body);
        return;
    }

    get_query_param(query, "client_id", client_id, sizeof(client_id));
    pthread_mutex_lock(&g_lock);
    LobbyClient *client = find_client_by_id_locked(client_id);
    if (client)
        client->last_seen = time(NULL);
    pthread_mutex_unlock(&g_lock);

    if (!client)
    {
        send_http(fd, "{\"ok\":false,\"error\":\"bad_client\"}");
        return;
    }

    if (strcmp(path, "/list") == 0)
    {
        pthread_mutex_lock(&g_lock);
        char out[LINE_BUF];
        size_t used = 0;
        used += (size_t)snprintf(out + used, sizeof(out) - used, "{\"ok\":true,\"games\":[");
        bool first = true;
        for (int i = 0; i < g_cfg.max_games; i++)
        {
            if (!g_games[i].in_use)
                continue;
            if (!first)
                used += (size_t)snprintf(out + used, sizeof(out) - used, ",");
            first = false;
            used += (size_t)snprintf(out + used, sizeof(out) - used,
                                     "{\"id\":\"%s\",\"name\":\"%s\",\"players\":%d,\"max\":%d,\"active\":%s,\"transport\":\"%s\"}",
                                     g_games[i].id, g_games[i].name, g_games[i].player_count,
                                     g_games[i].max_players, g_games[i].active ? "true" : "false",
                                     transport_to_string(g_games[i].transport));
        }
        used += (size_t)snprintf(out + used, sizeof(out) - used, "]}");
        pthread_mutex_unlock(&g_lock);
        send_http(fd, out);
        return;
    }

    if (strcmp(path, "/create") == 0)
    {
        get_query_param(query, "name", game_name, sizeof(game_name));
        url_decode(game_name);
        get_query_param(query, "max_players", max_players_str, sizeof(max_players_str));
        get_query_param(query, "transport", transport_str, sizeof(transport_str));
        transport = transport_from_query(transport_str);
        printf("Create request client=%s game=\"%s\" transport_param=\"%s\" resolved=%s\n",
               client->id, game_name[0] ? game_name : "Game", transport_str, transport_to_string(transport));
        if (!parse_int(max_players_str, &max_players) || max_players <= 0 || max_players > MAX_PLAYERS_LIMIT)
            max_players = g_cfg.max_players_default;

        pthread_mutex_lock(&g_lock);
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
            pthread_mutex_unlock(&g_lock);
            send_http(fd, "{\"ok\":false,\"error\":\"max_games\"}");
            return;
        }

        Game *game = &g_games[slot];
        memset(game, 0, sizeof(*game));
        game->in_use = true;
        game->active = false;
        game->ended = false;
        game->max_players = max_players;
        game->player_count = 0;
        game->transport = transport;
        game->created_at = time(NULL);
        snprintf(game->name, sizeof(game->name), "%s", game_name[0] ? game_name : "Game");
        gen_id(game->id, sizeof(game->id));

        snprintf(game->player_ids[0], sizeof(game->player_ids[0]), "%s", client->id);
        snprintf(game->player_names[0], sizeof(game->player_names[0]), "%s", client->name);
        gen_id(game->tokens[0], sizeof(game->tokens[0]));
        game->player_count = 1;

        pthread_mutex_unlock(&g_lock);

        char body[LINE_BUF];
        snprintf(body, sizeof(body), "{\"ok\":true,\"game_id\":\"%s\",\"status\":\"waiting\",\"transport\":\"%s\"}",
                 game->id, transport_to_string(game->transport));
        send_http(fd, body);
        return;
    }

    if (strcmp(path, "/join") == 0)
    {
        get_query_param(query, "game_id", game_id, sizeof(game_id));
        pthread_mutex_lock(&g_lock);
        Game *game = find_game_by_id_locked(game_id);
        if (!game || game->active)
        {
            pthread_mutex_unlock(&g_lock);
            send_http(fd, "{\"ok\":false,\"error\":\"not_found\"}");
            return;
        }
        if (game->player_count >= game->max_players)
        {
            pthread_mutex_unlock(&g_lock);
            send_http(fd, "{\"ok\":false,\"error\":\"full\"}");
            return;
        }

        int idx = game->player_count++;
        snprintf(game->player_ids[idx], sizeof(game->player_ids[idx]), "%s", client->id);
        snprintf(game->player_names[idx], sizeof(game->player_names[idx]), "%s", client->name);
        gen_id(game->tokens[idx], sizeof(game->tokens[idx]));

        if (game->player_count >= game->max_players)
            start_game_locked(game);

        pthread_mutex_unlock(&g_lock);

        send_http(fd, "{\"ok\":true,\"status\":\"waiting\"}");
        return;
    }

    if (strcmp(path, "/leave") == 0)
    {
        get_query_param(query, "game_id", game_id, sizeof(game_id));
        pthread_mutex_lock(&g_lock);
        Game *game = find_game_by_id_locked(game_id);
        if (!game || game->active)
        {
            pthread_mutex_unlock(&g_lock);
            send_http(fd, "{\"ok\":false,\"error\":\"not_found\"}");
            return;
        }
        remove_client_from_game_locked(game, client->id);
        pthread_mutex_unlock(&g_lock);
        send_http(fd, "{\"ok\":true}");
        return;
    }

    if (strcmp(path, "/wait") == 0)
    {
        get_query_param(query, "game_id", game_id, sizeof(game_id));
        Game *game = NULL;
        if (game_id[0])
        {
            pthread_mutex_lock(&g_lock);
            game = find_game_by_id_locked(game_id);
            pthread_mutex_unlock(&g_lock);
        }
        if (game_id[0] && !game)
        {
            send_http(fd, "{\"ok\":false,\"error\":\"not_found\"}");
            return;
        }
        if (client->pending_start)
        {
            client->pending_start = false;
            char body[LINE_BUF];
            snprintf(body, sizeof(body),
                     "{\"cmd\":\"start\",\"host\":\"%s\",\"port\":%d,\"transport\":\"%s\",\"token\":\"%s\"}",
                     client->start_host, client->start_port, transport_to_string(client->start_transport), "");
            send_http(fd, body);
            return;
        }
        if (game)
        {
            char body[LINE_BUF];
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"status\":\"waiting\",\"players\":%d,\"max\":%d}",
                     game->player_count, game->max_players);
            send_http(fd, body);
            return;
        }
        send_http(fd, "{\"ok\":true,\"status\":\"waiting\",\"players\":0,\"max\":0}");
        return;
    }

    if (strcmp(path, "/ping") == 0)
    {
        send_http(fd, "{\"ok\":true}");
        return;
    }

    send_http(fd, "{\"ok\":false,\"error\":\"unknown\"}");
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

    printf("Lobby HTTP listening on port %d, host %s\n", g_cfg.lobby_port, g_cfg.host_name);

    while (1)
    {
        expire_pending_games();
        expire_clients();

        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int client_fd = accept(sockfd, (struct sockaddr *)&cliaddr, &clilen);
        if (client_fd < 0)
            continue;

        char req[REQ_BUF + 1];
        ssize_t r = recv(client_fd, req, REQ_BUF, 0);
        if (r <= 0)
        {
            close(client_fd);
            continue;
        }
        req[r] = '\0';

        char method[8];
        char url[256];
        if (sscanf(req, "%7s %255s", method, url) != 2)
        {
            close(client_fd);
            continue;
        }
        if (strcmp(method, "GET") != 0)
        {
            close(client_fd);
            continue;
        }

        char *query = strchr(url, '?');
        if (query)
        {
            *query = '\0';
            query++;
        }

        handle_request(client_fd, url, query);
        close(client_fd);
    }

    return 0;
}
