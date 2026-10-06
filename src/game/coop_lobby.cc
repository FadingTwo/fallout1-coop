#include "game/coop_lobby.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <SDL.h>

#include "game/coop.h"
#include "game/coop_http.h"
#include "game/coop_net.h"
#include "plib/gnw/debug.h"

namespace fallout {

#ifdef _WIN32
typedef SOCKET LobbySocket;
#define LOBBY_INVALID_SOCKET INVALID_SOCKET
#define lobby_close closesocket
#else
typedef int LobbySocket;
#define LOBBY_INVALID_SOCKET (-1)
#define lobby_close close
#endif

// Search: "FCOOP?". Answer: "FCOOP!|version|checksum|tcp port|players|name".
#define LOBBY_QUERY "FCOOP?"
#define LOBBY_ANSWER "FCOOP!"

// The public listing is refreshed this often; the server forgets a game
// after 90 seconds without one.
#define LOBBY_ANNOUNCE_INTERVAL 30000

static LobbySocket lobby_host_socket = LOBBY_INVALID_SOCKET;
static std::string lobby_server;
static bool lobby_public = false;
static unsigned int lobby_last_announce = 0;
static bool lobby_announced = false;
static int lobby_announced_players = 0;
static std::atomic<bool> lobby_busy(false);

static bool lobby_socket_startup()
{
#ifdef _WIN32
    static bool started = false;
    if (!started) {
        WSADATA data;
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    return started;
#else
    return true;
#endif
}

static std::string lobby_checksum_text()
{
    char text[32];
    snprintf(text, sizeof(text), "%016llx", (unsigned long long)coop_net_data_checksum());
    return text;
}

static bool lobby_non_blocking(LobbySocket socket)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Only printable characters, no separators, at most `max` long.
static std::string lobby_clean(const std::string& text, size_t max)
{
    std::string result;
    for (char ch : text) {
        if (ch >= 32 && ch < 127 && ch != '|' && ch != '=' && ch != '&') {
            result += ch;
        }
        if (result.size() >= max) {
            break;
        }
    }
    return result;
}

// Runs one request in the background, at most one at a time, so the game
// never waits for the server.
static void lobby_post_in_background(const std::string& body)
{
    if (lobby_server.empty() || lobby_busy.exchange(true)) {
        return;
    }

    std::string url = lobby_server + "/fallout-coop/lobby";
    std::thread([url, body]() {
        std::string response;
        std::string error;
        if (!coop_http_post(url, "text/plain; charset=utf-8", body, &response, &error, 6000)) {
            debug_printf("\nCOOP LOBBY: %s\n", error.c_str());
        }
        lobby_busy = false;
    }).detach();
}

void coop_lobby_host_start(const std::string& server, bool listPublic)
{
    coop_lobby_host_stop();

    lobby_server = server;
    lobby_public = listPublic && !server.empty();
    lobby_announced = false;
    lobby_last_announce = 0;

    if (!lobby_socket_startup()) {
        return;
    }

    LobbySocket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == LOBBY_INVALID_SOCKET) {
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)coop_net_port());
    if (bind(sock, (sockaddr*)&address, sizeof(address)) != 0 || !lobby_non_blocking(sock)) {
        debug_printf("\nCOOP LOBBY: cannot answer LAN searches on UDP port %d\n", coop_net_port());
        lobby_close(sock);
        return;
    }

    lobby_host_socket = sock;
    debug_printf("\nCOOP LOBBY: hosting%s\n", lobby_public ? ", listed publicly" : "");
}

void coop_lobby_host_pump(const std::string& name, int players)
{
    std::string cleanName = lobby_clean(name, 24);

    if (lobby_host_socket != LOBBY_INVALID_SOCKET) {
        char buffer[64];
        sockaddr_in from;
        for (int count = 0; count < 16; count++) {
            socklen_t fromLength = sizeof(from);
            int received = (int)recvfrom(lobby_host_socket, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&from, &fromLength);
            if (received <= 0) {
                break;
            }
            buffer[received] = '\0';
            if (strncmp(buffer, LOBBY_QUERY, strlen(LOBBY_QUERY)) != 0) {
                continue;
            }

            char answer[160];
            snprintf(answer, sizeof(answer), "%s|%s|%s|%d|%d|%s", LOBBY_ANSWER, COOP_VERSION,
                lobby_checksum_text().c_str(), coop_net_port(), players, cleanName.c_str());
            sendto(lobby_host_socket, answer, (int)strlen(answer), 0, (sockaddr*)&from, fromLength);
        }
    }

    // Refreshed regularly, and at once when someone joins or leaves (a
    // full game drops out of the list).
    if (lobby_public && (!lobby_announced || players != lobby_announced_players || SDL_GetTicks() - lobby_last_announce >= LOBBY_ANNOUNCE_INTERVAL)) {
        if (lobby_busy) {
            return; // Next frame.
        }
        lobby_last_announce = SDL_GetTicks();
        lobby_announced = true;
        lobby_announced_players = players;

        char body[256];
        snprintf(body, sizeof(body), "action=announce\nport=%d\nplayers=%d\nversion=%s\nchecksum=%s\nname=%s\nrelay=%s\n",
            coop_net_port(), players, COOP_VERSION, lobby_checksum_text().c_str(), cleanName.c_str(), coop_net_relay_code());
        lobby_post_in_background(body);
    }
}

void coop_lobby_host_stop()
{
    if (lobby_host_socket != LOBBY_INVALID_SOCKET) {
        lobby_close(lobby_host_socket);
        lobby_host_socket = LOBBY_INVALID_SOCKET;
    }

    if (lobby_public && lobby_announced) {
        char body[64];
        snprintf(body, sizeof(body), "action=remove\nport=%d\n", coop_net_port());
        // A refresh may still be on its way; the server forgets the game
        // soon anyway if this one is skipped.
        lobby_post_in_background(body);
    }
    lobby_public = false;
    lobby_announced = false;
}

bool coop_lobby_host_public()
{
    return lobby_public;
}

// Broadcast addresses to search: everything, each interface's own
// broadcast address, and this computer.
static std::vector<unsigned int> lobby_search_targets()
{
    std::vector<unsigned int> targets = { INADDR_BROADCAST, INADDR_LOOPBACK };
#ifndef _WIN32
    ifaddrs* list = NULL;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* entry = list; entry != NULL; entry = entry->ifa_next) {
            if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET || (entry->ifa_flags & IFF_BROADCAST) == 0 || entry->ifa_broadaddr == NULL) {
                continue;
            }
            targets.push_back(ntohl(((sockaddr_in*)entry->ifa_broadaddr)->sin_addr.s_addr));
        }
        freeifaddrs(list);
    }
#endif
    return targets;
}

static int lobby_other_versions = 0;

static void lobby_find_lan(std::vector<CoopLobbyGame>& games, const std::string& checksum)
{
    if (!lobby_socket_startup()) {
        return;
    }

    LobbySocket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == LOBBY_INVALID_SOCKET) {
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));

    for (unsigned int target : lobby_search_targets()) {
        sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(target);
        address.sin_port = htons((unsigned short)coop_net_port());
        sendto(sock, LOBBY_QUERY, (int)strlen(LOBBY_QUERY), 0, (sockaddr*)&address, sizeof(address));
    }

    unsigned int start = SDL_GetTicks();
    while (SDL_GetTicks() - start < 1000) {
        fd_set read;
        FD_ZERO(&read);
        FD_SET(sock, &read);
        timeval wait = { 0, 100 * 1000 };
        if (select((int)sock + 1, &read, NULL, NULL, &wait) <= 0) {
            continue;
        }

        char buffer[256];
        sockaddr_in from;
        socklen_t fromLength = sizeof(from);
        int received = (int)recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&from, &fromLength);
        if (received <= 0) {
            continue;
        }
        buffer[received] = '\0';

        // FCOOP!|version|checksum|port|players|name
        std::vector<std::string> fields;
        std::string current;
        for (int index = 0; index < received; index++) {
            if (buffer[index] == '|') {
                fields.push_back(current);
                current.clear();
            } else {
                current += buffer[index];
            }
        }
        fields.push_back(current);
        if (fields.size() < 6 || fields[0] != LOBBY_ANSWER) {
            continue;
        }
        if (fields[1] != COOP_VERSION || fields[2] != checksum) {
            lobby_other_versions++;
            continue;
        }

        char host[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));

        CoopLobbyGame game;
        game.address = host;
        game.port = atoi(fields[3].c_str());
        game.name = lobby_clean(fields[5], 24);
        game.lan = true;
        game.relay.clear();

        // The same host answers on several of the addresses searched.
        bool known = false;
        for (const CoopLobbyGame& other : games) {
            if (other.port == game.port && (other.address == game.address || other.name == game.name)) {
                known = true;
            }
        }
        if (!known && atoi(fields[4].c_str()) < 2) {
            games.push_back(game);
        }
    }

    lobby_close(sock);
}

static void lobby_find_public(std::vector<CoopLobbyGame>& games, const std::string& server, const std::string& checksum)
{
    if (server.empty()) {
        return;
    }

    std::string body;
    std::string error;
    if (!coop_http_get(server + "/fallout-coop/lobby", &body, &error, 5000)) {
        debug_printf("\nCOOP LOBBY: %s\n", error.c_str());
        return;
    }

    // address|port|players|version|checksum|name|relay per line (relay:
    // the code to join through the server, or empty).
    size_t lineStart = 0;
    while (lineStart < body.size()) {
        size_t lineEnd = body.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = body.size();
        }
        std::string line = body.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;

        std::vector<std::string> fields;
        size_t fieldStart = 0;
        for (int index = 0; index < 5; index++) {
            size_t bar = line.find('|', fieldStart);
            if (bar == std::string::npos) {
                break;
            }
            fields.push_back(line.substr(fieldStart, bar - fieldStart));
            fieldStart = bar + 1;
        }
        if (fields.size() < 5) {
            continue;
        }
        fields.push_back(line.substr(fieldStart));

        if (atoi(fields[2].c_str()) >= 2) {
            continue;
        }
        if (fields[3] != COOP_VERSION || fields[4] != checksum) {
            lobby_other_versions++;
            continue;
        }

        std::string name = fields[5];
        std::string relay;
        size_t bar = name.find('|');
        if (bar != std::string::npos) {
            relay = lobby_clean(name.substr(bar + 1), 8);
            name = name.substr(0, bar);
        }

        CoopLobbyGame game;
        game.address = lobby_clean(fields[0], 64);
        game.port = atoi(fields[1].c_str());
        game.name = lobby_clean(name, 24);
        game.lan = false;
        game.relay = relay;

        bool known = false;
        for (const CoopLobbyGame& other : games) {
            if (other.port == game.port && other.name == game.name) {
                known = true; // Already found on the LAN.
            }
        }
        if (!known && game.port > 0) {
            games.push_back(game);
        }
    }
}

std::vector<CoopLobbyGame> coop_lobby_find(const std::string& server, int* otherVersions)
{
    std::vector<CoopLobbyGame> games;
    std::string checksum = lobby_checksum_text();
    lobby_other_versions = 0;
    lobby_find_lan(games, checksum);
    lobby_find_public(games, server, checksum);
    if (otherVersions != nullptr) {
        *otherVersions = lobby_other_versions;
    }
    return games;
}

} // namespace fallout
