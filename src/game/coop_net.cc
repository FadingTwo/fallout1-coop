#include "game/coop_net.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <SDL.h>

#include "game/config.h"
#include "game/coop.h"
#include "game/cycle.h"
#include "game/display.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

// "FOCP"
#define COOP_NET_MAGIC 0x46434F50

// Bumped whenever the wire format changes; both sides must match.
#define COOP_NET_PROTOCOL_VERSION 1

// Messages: u32 length (of type + payload), u8 type, payload.
#define COOP_NET_HEADER_SIZE 5
#define COOP_NET_MAX_MESSAGE (4 * 1024 * 1024)

// Something is sent at least this often (ms) on a quiet connection.
#define COOP_NET_KEEPALIVE 30000

#define COOP_NET_HANDSHAKE_TIMEOUT 10000

// Sending to a peer that has gone must fail, not kill the process.
#if defined(MSG_NOSIGNAL)
#define COOP_SEND_FLAGS MSG_NOSIGNAL
#else
#define COOP_SEND_FLAGS 0
#endif

#ifdef _WIN32
typedef SOCKET CoopSocket;
#define COOP_INVALID_SOCKET INVALID_SOCKET
#define coop_close_socket closesocket
#else
typedef int CoopSocket;
#define COOP_INVALID_SOCKET (-1)
#define coop_close_socket close
#endif

// A message-framed, non-blocking TCP connection.
typedef struct CoopConnection {
    CoopSocket socket = COOP_INVALID_SOCKET;
    std::vector<unsigned char> in;
    std::vector<unsigned char> out;
    bool failed = false;
} CoopConnection;

typedef struct CoopMessage {
    int type;
    std::vector<unsigned char> payload;
} CoopMessage;

// Little-endian payload writer/reader.
class CoopWriter {
public:
    std::vector<unsigned char> data;

    void u8(unsigned int value) { data.push_back((unsigned char)value); }
    void u16(unsigned int value)
    {
        u8(value & 0xFF);
        u8((value >> 8) & 0xFF);
    }
    void u32(unsigned int value)
    {
        u16(value & 0xFFFF);
        u16((value >> 16) & 0xFFFF);
    }
    void u64(uint64_t value)
    {
        u32((unsigned int)(value & 0xFFFFFFFF));
        u32((unsigned int)(value >> 32));
    }
    void bytes(const void* src, size_t length)
    {
        const unsigned char* p = (const unsigned char*)src;
        data.insert(data.end(), p, p + length);
    }
    void str(const char* text)
    {
        size_t length = strlen(text);
        u16((unsigned int)length);
        bytes(text, length);
    }
};

class CoopReader {
public:
    CoopReader(const std::vector<unsigned char>& data)
        : data(data)
    {
    }

    bool ok() const { return !overrun; }
    bool atEnd() const { return pos >= data.size(); }

    unsigned int u8()
    {
        if (pos + 1 > data.size()) {
            overrun = true;
            return 0;
        }
        return data[pos++];
    }
    unsigned int u16() { return u8() | (u8() << 8); }
    unsigned int u32() { return u16() | (u16() << 16); }
    uint64_t u64()
    {
        uint64_t low = u32();
        uint64_t high = u32();
        return low | (high << 32);
    }
    const unsigned char* bytes(size_t length)
    {
        if (pos + length > data.size()) {
            overrun = true;
            return NULL;
        }
        const unsigned char* p = data.data() + pos;
        pos += length;
        return p;
    }
    std::string str()
    {
        size_t length = u16();
        const unsigned char* p = bytes(length);
        return p != NULL ? std::string((const char*)p, length) : std::string();
    }

private:
    const std::vector<unsigned char>& data;
    size_t pos = 0;
    bool overrun = false;
};

static bool coop_socket_startup();
static bool coop_socket_set_non_blocking(CoopSocket socket);
static void coop_relay_host_pump();
static CoopSocket coop_relay_take_incoming();
static void coop_connection_close(CoopConnection* connection);
static void coop_connection_send(CoopConnection* connection, int type, const std::vector<unsigned char>& payload);
static void coop_connection_flush(CoopConnection* connection);
static void coop_connection_receive(CoopConnection* connection);
static bool coop_connection_next(CoopConnection* connection, CoopMessage* message);
static void coop_host_handle(const CoopMessage& message);
static void coop_net_write_sound(int event, const char* name, int a, int b, int c);

static CoopNetMode coop_net_mode_value = COOP_NET_NONE;
static int coop_net_port_value = COOP_NET_DEFAULT_PORT;
static std::string coop_net_error;
static std::string coop_net_host_name;
// Hosting online: any free port will do (see coop_net_host_start).
static bool coop_net_any_port_ok = false;
// The port to go back to after hosting on another one (0: none).
static int coop_net_configured_port = 0;

static bool coop_net_checksum_done = false;
static uint64_t coop_net_checksum_value = 0;

// Host state.
static CoopSocket coop_listen_socket = COOP_INVALID_SOCKET;
static CoopConnection coop_client;
static bool coop_client_ready = false;

// Testing: a client that doesn't acknowledge frames, like those before 1.5.7.
static bool coop_net_no_acks = getenv("COOP_NET_NO_ACKS") != NULL;
// Testing: a client that doesn't take packed frames.
static bool coop_net_no_packing = getenv("COOP_NET_NO_PACKING") != NULL;
// Testing: the host doesn't hold frames back (still measures the delay).
static bool coop_net_no_flow = getenv("COOP_NET_NO_FLOW") != NULL;

// Flow control (clients that acknowledge frames): at most this many frames
// on the way, so a slow line can't pile up seconds of delay in the buffers
// between (the system's, the relay's).
#define COOP_NET_FRAMES_IN_FLIGHT 2
static bool coop_client_acks = false;
static bool coop_client_unpacks = false;
static unsigned int coop_frames_sent = 0;
static unsigned int coop_frames_acked = 0;
static std::deque<unsigned int> coop_frame_sent_at;
static unsigned long long coop_stats_delay_total = 0;
static unsigned int coop_stats_delay_count = 0;
static unsigned int coop_stats_delay_max = 0;

// Player 2's screen size (since 1.5.7; 0 = unknown): frames bigger than
// it are scaled down before sending (player 2 would only scale them down
// anyway), and their mouse positions back up.
static int coop_client_screen_width = 0;
static int coop_client_screen_height = 0;
static int coop_sent_scaled_from_width = 0;
static int coop_sent_scaled_from_height = 0;
static std::vector<unsigned char> coop_scaled_frame;
static unsigned int coop_client_since = 0;
static std::deque<CoopNetInput> coop_client_input;

// Traffic statistics for the debug log.
static unsigned long long coop_stats_bytes = 0;
static unsigned long long coop_stats_raw_bytes = 0;
static unsigned int coop_stats_frames = 0;
static unsigned int coop_stats_since = 0;

// Last frame sent, to send only what changed.
static std::vector<unsigned char> coop_sent_frame;
static unsigned char coop_sent_palette[768];
static int coop_sent_width = 0;
static int coop_sent_height = 0;
static std::string coop_sent_status;

// Background music playing, for clients joining later.
static bool coop_music_playing = false;
static std::string coop_music_name;
static int coop_music_args[3];

// Frames are dropped while this much is still waiting to be sent.
#define COOP_NET_MAX_BACKLOG (2 * 1024 * 1024)

static void coop_client_exit_hook_default() { }

static bool coop_client_exit_requested = false;
static void (*coop_client_exit_hook)() = coop_client_exit_hook_default;

void coop_net_init()
{
    char* mode;
    if (config_get_string(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_MODE_KEY, &mode)) {
        if (compat_stricmp(mode, "host") == 0) {
            coop_net_mode_value = COOP_NET_HOST;
        } else if (compat_stricmp(mode, "client") == 0) {
            coop_net_mode_value = COOP_NET_CLIENT;
        }
    }

    int port;
    if (config_get_value(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_PORT_KEY, &port) && port > 0 && port < 65536) {
        coop_net_port_value = port;
    }

    char* host;
    if (config_get_string(&game_config, COOP_CONFIG_KEY, COOP_CONFIG_HOST_KEY, &host)) {
        coop_net_host_name = host;
    }
}

void coop_net_exit()
{
    if (coop_client.socket != COOP_INVALID_SOCKET) {
        coop_connection_send(&coop_client, COOP_NET_MSG_BYE, std::vector<unsigned char>());
        coop_connection_flush(&coop_client);
        coop_connection_close(&coop_client);
    }

    if (coop_listen_socket != COOP_INVALID_SOCKET) {
        coop_close_socket(coop_listen_socket);
        coop_listen_socket = COOP_INVALID_SOCKET;
    }

    coop_net_relay_host_stop();

    if (coop_net_configured_port != 0) {
        coop_net_port_value = coop_net_configured_port;
        coop_net_configured_port = 0;
    }

    coop_client_ready = false;
}

CoopNetMode coop_net_mode()
{
    return coop_net_mode_value;
}

void coop_net_set_mode(CoopNetMode mode)
{
    coop_net_mode_value = mode;
}

void coop_net_set_host(const char* host)
{
    coop_net_host_name = host;
}

int coop_net_port()
{
    return coop_net_port_value;
}

void coop_net_set_port(int port)
{
    if (port > 0 && port < 65536) {
        coop_net_port_value = port;
    }
}

const char* coop_net_last_error()
{
    return coop_net_error.c_str();
}

void coop_net_local_addresses(char* buffer, size_t size)
{
    std::string result;

#ifdef _WIN32
    if (coop_socket_startup()) {
        char name[256];
        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        addrinfo* addresses = NULL;
        if (gethostname(name, sizeof(name)) == 0 && getaddrinfo(name, NULL, &hints, &addresses) == 0) {
            for (addrinfo* it = addresses; it != NULL; it = it->ai_next) {
                char text[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &(((sockaddr_in*)it->ai_addr)->sin_addr), text, sizeof(text)) != NULL) {
                    result += (result.empty() ? "" : ", ") + std::string(text);
                }
            }
            freeaddrinfo(addresses);
        }
    }
#else
    ifaddrs* interfaces = NULL;
    if (getifaddrs(&interfaces) == 0) {
        for (ifaddrs* it = interfaces; it != NULL; it = it->ifa_next) {
            if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET || (it->ifa_flags & IFF_LOOPBACK) != 0) {
                continue;
            }

            char text[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &(((sockaddr_in*)it->ifa_addr)->sin_addr), text, sizeof(text)) != NULL) {
                result += (result.empty() ? "" : ", ") + std::string(text);
            }
        }
        freeifaddrs(interfaces);
    }
#endif

    snprintf(buffer, size, "%s", result.c_str());
}

// -----------------------------------------------------------------------------
// Data checksum

static uint64_t coop_fnv1a(uint64_t hash, const void* data, size_t length)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t index = 0; index < length; index++) {
        hash ^= p[index];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

static uint64_t coop_hash_file(uint64_t hash, const std::filesystem::path& path)
{
    FILE* stream = fopen(path.string().c_str(), "rb");
    if (stream == NULL) {
        return coop_fnv1a(hash, "missing", 7);
    }

    std::vector<unsigned char> buffer(1 << 20);
    size_t read;
    while ((read = fread(buffer.data(), 1, buffer.size(), stream)) != 0) {
        hash = coop_fnv1a(hash, buffer.data(), read);
    }

    fclose(stream);
    return hash;
}

// Game data files: master.dat and critter.dat as configured, and every file
// under the patches directory except saves and files the game writes.
uint64_t coop_net_data_checksum()
{
    if (coop_net_checksum_done) {
        return coop_net_checksum_value;
    }

    uint64_t hash = 0xCBF29CE484222325ULL;

    const char* keys[] = { GAME_CONFIG_MASTER_DAT_KEY, GAME_CONFIG_CRITTER_DAT_KEY };
    for (const char* key : keys) {
        char* path;
        if (config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, key, &path)) {
            hash = coop_hash_file(hash, path);
        }
    }

    char* patches;
    if (config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_PATCHES_KEY, &patches)) {
        std::vector<std::string> files;
        std::error_code error;
        std::filesystem::recursive_directory_iterator it(patches, error);
        for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            std::string relative = std::filesystem::relative(it->path(), patches, error).generic_string();
            std::string upper = relative;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

            if (it->is_directory(error)) {
                if (upper == "SAVEGAME") {
                    it.disable_recursion_pending();
                }
                continue;
            }

            std::string extension = std::filesystem::path(upper).extension().string();
            if (extension == ".SAV" || extension == ".BAK" || extension == ".CFG") {
                continue;
            }

            files.push_back(relative);
        }

        std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
            return compat_stricmp(a.c_str(), b.c_str()) < 0;
        });

        for (const std::string& file : files) {
            std::string upper = file;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
            hash = coop_fnv1a(hash, upper.c_str(), upper.size());
            hash = coop_hash_file(hash, std::filesystem::path(patches) / file);
        }
    }

    char* language;
    if (config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_LANGUAGE_KEY, &language)) {
        std::string upper = language;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        hash = coop_fnv1a(hash, upper.c_str(), upper.size());
    }

    coop_net_checksum_value = hash;
    coop_net_checksum_done = true;

    debug_printf("\nCOOP NET: data checksum %016llx\n", (unsigned long long)hash);

    return hash;
}

// -----------------------------------------------------------------------------
// Sockets

static bool coop_socket_startup()
{
#ifdef _WIN32
    static bool started = false;
    if (!started) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        started = true;
    }
#endif
    return true;
}

static bool coop_socket_set_non_blocking(CoopSocket socket)
{
#ifdef SO_NOSIGPIPE
    // macOS has no MSG_NOSIGNAL.
    int noSigPipe = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif

#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static int coop_socket_error()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool coop_socket_would_block()
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

static void coop_connection_close(CoopConnection* connection)
{
    if (connection->socket != COOP_INVALID_SOCKET) {
        coop_close_socket(connection->socket);
    }

    connection->socket = COOP_INVALID_SOCKET;
    connection->in.clear();
    connection->out.clear();
    connection->failed = false;
}

static void coop_connection_send(CoopConnection* connection, int type, const std::vector<unsigned char>& payload)
{
    if (connection->socket == COOP_INVALID_SOCKET) {
        return;
    }

    CoopWriter header;
    header.u32((unsigned int)payload.size() + 1);
    header.u8(type);

    connection->out.insert(connection->out.end(), header.data.begin(), header.data.end());
    connection->out.insert(connection->out.end(), payload.begin(), payload.end());
    coop_connection_flush(connection);
}

static void coop_connection_flush(CoopConnection* connection)
{
    while (connection->socket != COOP_INVALID_SOCKET && !connection->out.empty()) {
        int sent = (int)send(connection->socket, (const char*)connection->out.data(), (int)connection->out.size(), COOP_SEND_FLAGS);
        if (sent > 0) {
            connection->out.erase(connection->out.begin(), connection->out.begin() + sent);
        } else {
            if (sent < 0 && !coop_socket_would_block()) {
                debug_printf("\nCOOP NET: send failed (error %d)\n", coop_socket_error());
                connection->failed = true;
            }
            break;
        }
    }
}

static void coop_connection_receive(CoopConnection* connection)
{
    unsigned char buffer[64 * 1024];
    while (connection->socket != COOP_INVALID_SOCKET) {
        int received = (int)recv(connection->socket, (char*)buffer, sizeof(buffer), 0);
        if (received > 0) {
            connection->in.insert(connection->in.end(), buffer, buffer + received);
        } else {
            if (received == 0 || !coop_socket_would_block()) {
                debug_printf("\nCOOP NET: %s\n", received == 0 ? "the other side closed the connection" : "receive failed");
                if (received != 0) {
                    debug_printf("COOP NET: (error %d)\n", coop_socket_error());
                }
                connection->failed = true;
            }
            break;
        }
    }
}

static bool coop_connection_next(CoopConnection* connection, CoopMessage* message)
{
    if (connection->in.size() < COOP_NET_HEADER_SIZE) {
        return false;
    }

    CoopReader reader(connection->in);
    unsigned int length = reader.u32();
    if (length == 0 || length > COOP_NET_MAX_MESSAGE) {
        debug_printf("\nCOOP NET: bad message length %u\n", length);
        connection->failed = true;
        return false;
    }

    if (connection->in.size() < 4 + length) {
        return false;
    }

    message->type = connection->in[4];
    message->payload.assign(connection->in.begin() + COOP_NET_HEADER_SIZE, connection->in.begin() + 4 + length);
    connection->in.erase(connection->in.begin(), connection->in.begin() + 4 + length);
    return true;
}

// -----------------------------------------------------------------------------
// Host

// Sends the screen as it is, cursor included (the client shows no cursor
// of its own: the host draws player 2's, with its real shape).
void coop_net_send_current_screen()
{
    if (!coop_client_ready || gSdlSurface == NULL || gSdlSurface->format->palette == NULL) {
        return;
    }

    unsigned char palette[768];
    SDL_Color* colors = gSdlSurface->format->palette->colors;
    for (int index = 0; index < 256; index++) {
        palette[index * 3] = colors[index].r;
        palette[index * 3 + 1] = colors[index].g;
        palette[index * 3 + 2] = colors[index].b;
    }

    coop_net_send_screen((const unsigned char*)gSdlSurface->pixels, gSdlSurface->pitch, gSdlSurface->w, gSdlSurface->h, palette);
}

void coop_net_set_any_port_ok(bool ok)
{
    coop_net_any_port_ok = ok;
}

bool coop_net_host_start()
{
    if (!coop_socket_startup()) {
        return false;
    }

    coop_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (coop_listen_socket == COOP_INVALID_SOCKET) {
        debug_printf("\nCOOP NET: cannot create socket\n");
        return false;
    }

    int reuse = 1;
    setsockopt(coop_listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)coop_net_port_value);

    bool bound = bind(coop_listen_socket, (sockaddr*)&address, sizeof(address)) == 0;
    if (!bound && coop_net_any_port_ok) {
        // Online the port doesn't matter (players come through the relay):
        // when it's taken (another copy, a game server), any free one.
        address.sin_port = 0;
        bound = bind(coop_listen_socket, (sockaddr*)&address, sizeof(address)) == 0;
        socklen_t length = sizeof(address);
        if (bound && getsockname(coop_listen_socket, (sockaddr*)&address, &length) == 0) {
            debug_printf("\nCOOP NET: port %d is taken, using %d\n", coop_net_port_value, ntohs(address.sin_port));
            coop_net_configured_port = coop_net_port_value;
            coop_net_port_value = ntohs(address.sin_port);
        }
    }

    if (!bound
        || listen(coop_listen_socket, 1) != 0
        || !coop_socket_set_non_blocking(coop_listen_socket)) {
        debug_printf("\nCOOP NET: cannot listen on port %d\n", coop_net_port_value);
        coop_close_socket(coop_listen_socket);
        coop_listen_socket = COOP_INVALID_SOCKET;
        return false;
    }

    // Compute up front so the handshake does not stall the game.
    coop_net_data_checksum();

    debug_printf("\nCOOP NET: hosting on port %d\n", coop_net_port_value);
    return true;
}

// The next joining player: directly, or through the relay.
static CoopSocket coop_host_accept()
{
    CoopSocket socket = accept(coop_listen_socket, NULL, NULL);
    if (socket == COOP_INVALID_SOCKET) {
        socket = coop_relay_take_incoming();
    }
    return socket;
}

// Someone else trying to join while player 2 is here: told the game is
// full, and closed a little later (closing at once could lose the answer).
typedef struct CoopRefused {
    CoopConnection connection;
    unsigned int since;
} CoopRefused;

static std::deque<CoopRefused> coop_refused;

static void coop_host_refuse_extra_clients()
{
    for (int count = 0; count < 4 && coop_client.socket != COOP_INVALID_SOCKET; count++) {
        CoopSocket socket = coop_host_accept();
        if (socket == COOP_INVALID_SOCKET) {
            break;
        }
        coop_socket_set_non_blocking(socket);

        CoopRefused refused;
        refused.connection.socket = socket;
        refused.since = get_time();
        CoopWriter writer;
        writer.u8(0);
        writer.str("This game is full.");
        coop_connection_send(&(refused.connection), COOP_NET_MSG_WELCOME, writer.data);
        coop_refused.push_back(refused);
        debug_printf("\nCOOP NET: refused another client (game full)\n");
    }

    for (auto it = coop_refused.begin(); it != coop_refused.end();) {
        coop_connection_flush(&(it->connection));
        coop_connection_receive(&(it->connection));
        it->connection.in.clear();
        if (elapsed_time(it->since) > 2000 || it->connection.failed) {
            coop_connection_close(&(it->connection));
            it = coop_refused.erase(it);
        } else {
            ++it;
        }
    }
}

void coop_net_host_pump()
{
    if (coop_listen_socket == COOP_INVALID_SOCKET) {
        return;
    }

    coop_relay_host_pump();

    if (coop_client.socket != COOP_INVALID_SOCKET || !coop_refused.empty()) {
        coop_host_refuse_extra_clients();
    }

    if (coop_client.socket == COOP_INVALID_SOCKET) {
        CoopSocket socket = coop_host_accept();
        if (socket == COOP_INVALID_SOCKET) {
            return;
        }

        int noDelay = 1;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
        coop_socket_set_non_blocking(socket);

        coop_client.socket = socket;
        coop_client_ready = false;
        coop_client_since = get_time();
        debug_printf("\nCOOP NET: client connecting\n");
    }

    coop_connection_receive(&coop_client);

    CoopMessage message;
    while (coop_connection_next(&coop_client, &message)) {
        coop_host_handle(message);
    }

    coop_connection_flush(&coop_client);

    if (!coop_client_ready && coop_client.socket != COOP_INVALID_SOCKET && elapsed_time(coop_client_since) > COOP_NET_HANDSHAKE_TIMEOUT) {
        debug_printf("\nCOOP NET: handshake timed out\n");
        coop_client.failed = true;
    }

    if (coop_client.failed) {
        debug_printf("\nCOOP NET: client disconnected\n");
        coop_connection_close(&coop_client);
        coop_client_ready = false;
        coop_client_input.clear();
    }
}

static void coop_host_welcome(bool accepted, const char* reason)
{
    CoopWriter writer;
    writer.u8(accepted ? 1 : 0);
    writer.str(reason);
    // Since 1.5.7: this screen's size (frames may come scaled down).
    writer.u16((unsigned int)screenGetWidth());
    writer.u16((unsigned int)screenGetHeight());
    coop_connection_send(&coop_client, COOP_NET_MSG_WELCOME, writer.data);
}

static void coop_host_handle(const CoopMessage& message)
{
    CoopReader reader(message.payload);

    switch (message.type) {
    case COOP_NET_MSG_HELLO:
        if (true) {
            unsigned int magic = reader.u32();
            unsigned int protocol = reader.u32();
            uint64_t checksum = reader.u64();
            // Since 1.2: flags (1: colorblind outlines on player 2's
            // screen); since 1.3 followed by player 2's outline colors.
            // Since 1.5.7 flag 2: the client acknowledges frames.
            unsigned int flags = reader.atEnd() ? 0 : reader.u8();
            coop_client_acks = (flags & 2) != 0;
            coop_client_unpacks = (flags & 4) != 0;
            coop_frames_sent = 0;
            coop_frames_acked = 0;
            coop_frame_sent_at.clear();
            int outlines[2] = { (flags & 1) != 0 ? 2 : 0, (flags & 1) != 0 ? 3 : 1 };
            if (!reader.atEnd()) {
                outlines[0] = reader.u8();
                outlines[1] = reader.u8();
            }
            // Since 1.5.7: player 2's screen size.
            coop_client_screen_width = 0;
            coop_client_screen_height = 0;
            if (!reader.atEnd()) {
                coop_client_screen_width = reader.u16();
                coop_client_screen_height = reader.u16();
                debug_printf("COOP: player 2's screen is %dx%d\n", coop_client_screen_width, coop_client_screen_height);
            }
            coop_set_outline_colors(1, outlines);
            debug_printf("COOP: player 2 outlines: %s, %s\n", coop_outline_color_name(outlines[0]), coop_outline_color_name(outlines[1]));

            if (!reader.ok() || magic != COOP_NET_MAGIC) {
                coop_host_welcome(false, "Not a Fallout co-op client.");
                coop_client.failed = true;
            } else if (protocol != COOP_NET_PROTOCOL_VERSION) {
                coop_host_welcome(false, "Different co-op version.");
                coop_client.failed = true;
            } else if (checksum != coop_net_data_checksum()) {
                coop_host_welcome(false, "Game data differs (version, language or patches).");
                coop_client.failed = true;
            } else {
                coop_host_welcome(true, "");
                coop_client_ready = true;
                coop_sent_width = 0;
                coop_sent_height = 0;
                coop_sent_status.clear();

                if (coop_music_playing) {
                    coop_net_write_sound(GSOUND_EVENT_MUSIC, coop_music_name.c_str(), coop_music_args[0], coop_music_args[1], coop_music_args[2]);
                }
                debug_printf("\nCOOP NET: client joined\n");
            }
        }
        break;
    case COOP_NET_MSG_INPUT:
        if (coop_client_ready) {
            CoopNetInput input;
            input.kind = reader.u8();
            input.x = (short)reader.u16();
            input.y = (short)reader.u16();
            input.value = (int)reader.u32();
            // Positions in a scaled-down frame: back to this screen.
            if (input.kind == COOP_NET_INPUT_MOUSE && coop_sent_scaled_from_width != 0 && coop_sent_width > 0 && coop_sent_height > 0) {
                input.x = input.x * coop_sent_scaled_from_width / coop_sent_width + coop_sent_scaled_from_width / coop_sent_width / 2;
                input.y = input.y * coop_sent_scaled_from_height / coop_sent_height + coop_sent_scaled_from_height / coop_sent_height / 2;
            }
            if (reader.ok()) {
                coop_client_input.push_back(input);
            }
        }
        break;
    case COOP_NET_MSG_ACK:
        if (true) {
            unsigned int received = reader.u32();
            while (reader.ok() && coop_frames_acked < received && coop_frames_acked < coop_frames_sent) {
                if (!coop_frame_sent_at.empty()) {
                    unsigned int delay = elapsed_time(coop_frame_sent_at.front());
                    coop_frame_sent_at.pop_front();
                    coop_stats_delay_total += delay;
                    coop_stats_delay_count++;
                    coop_stats_delay_max = std::max(coop_stats_delay_max, delay);
                }
                coop_frames_acked++;
            }
        }
        break;
    case COOP_NET_MSG_BYE:
        coop_client.failed = true;
        break;
    }
}

bool coop_net_client_connected()
{
    return coop_client_ready;
}

bool coop_net_next_input(CoopNetInput* input)
{
    if (coop_client_input.empty()) {
        return false;
    }

    *input = coop_client_input.front();
    coop_client_input.pop_front();
    return true;
}

// Frame compression: a small LZ77. Tokens: a byte c < 0x80 is followed by
// c + 1 literal bytes; c >= 0x80 copies (c & 0x7F) + 4 bytes from u16
// (little endian) bytes back. Game screens have long flat or repeating
// stretches, so this usually saves a lot.
#define COOP_PACK_MIN_MATCH 4
#define COOP_PACK_MAX_MATCH (0x7F + COOP_PACK_MIN_MATCH)
#define COOP_PACK_HASH_BITS 15

static void coop_net_compress(const unsigned char* data, size_t size, std::vector<unsigned char>* out)
{
    static std::vector<int> table(1 << COOP_PACK_HASH_BITS);
    std::fill(table.begin(), table.end(), -1);

    out->clear();
    out->reserve(size / 2 + 16);
    size_t literalStart = 0;
    size_t pos = 0;

    auto flushLiterals = [&](size_t end) {
        while (literalStart < end) {
            size_t count = std::min<size_t>(end - literalStart, 128);
            out->push_back((unsigned char)(count - 1));
            out->insert(out->end(), data + literalStart, data + literalStart + count);
            literalStart += count;
        }
    };

    while (pos + COOP_PACK_MIN_MATCH <= size) {
        unsigned int key = (unsigned int)data[pos] | ((unsigned int)data[pos + 1] << 8) | ((unsigned int)data[pos + 2] << 16) | ((unsigned int)data[pos + 3] << 24);
        unsigned int hash = (key * 2654435761u) >> (32 - COOP_PACK_HASH_BITS);
        int candidate = table[hash];
        table[hash] = (int)pos;

        if (candidate >= 0 && pos - candidate <= 0xFFFF && memcmp(data + candidate, data + pos, COOP_PACK_MIN_MATCH) == 0) {
            size_t length = COOP_PACK_MIN_MATCH;
            while (length < COOP_PACK_MAX_MATCH && pos + length < size && data[candidate + length] == data[pos + length]) {
                length++;
            }
            flushLiterals(pos);
            unsigned int offset = (unsigned int)(pos - candidate);
            out->push_back((unsigned char)(0x80 | (length - COOP_PACK_MIN_MATCH)));
            out->push_back((unsigned char)(offset & 0xFF));
            out->push_back((unsigned char)(offset >> 8));
            pos += length;
            literalStart = pos;
        } else {
            pos++;
        }
    }
    flushLiterals(size);
}

// False when `data` is damaged.
static bool coop_net_decompress(const unsigned char* data, size_t size, size_t expected, std::vector<unsigned char>* out)
{
    out->clear();
    out->reserve(expected);
    size_t pos = 0;
    while (pos < size) {
        unsigned char token = data[pos++];
        if (token < 0x80) {
            size_t count = (size_t)token + 1;
            if (pos + count > size || out->size() + count > expected) {
                return false;
            }
            out->insert(out->end(), data + pos, data + pos + count);
            pos += count;
        } else {
            if (pos + 2 > size) {
                return false;
            }
            size_t length = (size_t)(token & 0x7F) + COOP_PACK_MIN_MATCH;
            size_t offset = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
            pos += 2;
            if (offset == 0 || offset > out->size() || out->size() + length > expected) {
                return false;
            }
            size_t from = out->size() - offset;
            for (size_t index = 0; index < length; index++) {
                out->push_back((*out)[from + index]);
            }
        }
    }
    return out->size() == expected;
}

int coop_net_pack_selftest(const unsigned char* data, size_t size)
{
    std::vector<unsigned char> packed;
    std::vector<unsigned char> unpacked;
    coop_net_compress(data, size, &packed);
    if (!coop_net_decompress(packed.data(), packed.size(), size, &unpacked) || unpacked.size() != size
        || (size != 0 && memcmp(unpacked.data(), data, size) != 0)) {
        return -1;
    }
    return (int)packed.size();
}

static void coop_write_varint(CoopWriter* writer, unsigned int value)
{
    while (value >= 0x80) {
        writer->u8((value & 0x7F) | 0x80);
        value >>= 7;
    }
    writer->u8(value);
}

static unsigned int coop_read_varint(CoopReader* reader)
{
    unsigned int value = 0;
    for (int shift = 0; shift < 32; shift += 7) {
        unsigned int byte = reader->u8();
        value |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0 || !reader->ok()) {
            break;
        }
    }
    return value;
}

// Frame: u16 width, u16 height, u8 flags (1: palette follows, 768 bytes
// RGB), then runs over the pixels (row by row) until all are covered:
// varint pixels unchanged, varint count, count new pixels.
static std::vector<unsigned char> coop_frame_scratch;

void coop_net_send_screen(const unsigned char* pixels, int pitch, int width, int height, const unsigned char* palette)
{
    if (!coop_client_ready || coop_client.out.size() > COOP_NET_MAX_BACKLOG) {
        return;
    }

    // Enough on the way already: the next frame shows the changes since.
    if (coop_client_acks && !coop_net_no_flow && coop_frames_sent - coop_frames_acked >= COOP_NET_FRAMES_IN_FLIGHT) {
        return;
    }

    // Bigger than player 2's screen: scaled down to fit it (same shape).
    coop_sent_scaled_from_width = 0;
    coop_sent_scaled_from_height = 0;
    int clientWidth = coop_client_screen_width;
    int clientHeight = coop_client_screen_height;
    if (clientWidth > 0 && clientHeight > 0 && (width > clientWidth || height > clientHeight)) {
        int scaledWidth = clientWidth;
        int scaledHeight = height * clientWidth / width;
        if (scaledHeight > clientHeight) {
            scaledHeight = clientHeight;
            scaledWidth = width * clientHeight / height;
        }
        if (scaledWidth > 0 && scaledHeight > 0) {
            coop_scaled_frame.resize((size_t)scaledWidth * scaledHeight);
            static std::vector<int> columns;
            columns.resize(scaledWidth);
            for (int column = 0; column < scaledWidth; column++) {
                columns[column] = column * width / scaledWidth;
            }
            for (int row = 0; row < scaledHeight; row++) {
                const unsigned char* src = pixels + (size_t)(row * height / scaledHeight) * pitch;
                unsigned char* dest = coop_scaled_frame.data() + (size_t)row * scaledWidth;
                for (int column = 0; column < scaledWidth; column++) {
                    dest[column] = src[columns[column]];
                }
            }
            coop_sent_scaled_from_width = width;
            coop_sent_scaled_from_height = height;
            pixels = coop_scaled_frame.data();
            pitch = scaledWidth;
            width = scaledWidth;
            height = scaledHeight;
        }
    }

    bool full = width != coop_sent_width || height != coop_sent_height;
    if (full) {
        coop_sent_frame.assign((size_t)width * height, 0);
        coop_sent_width = width;
        coop_sent_height = height;
    }

    bool paletteChanged = full || memcmp(palette, coop_sent_palette, sizeof(coop_sent_palette)) != 0;

    CoopWriter writer;
    writer.u16(width);
    writer.u16(height);
    writer.u8(paletteChanged ? 1 : 0);
    if (paletteChanged) {
        writer.bytes(palette, 768);
        memcpy(coop_sent_palette, palette, sizeof(coop_sent_palette));
    }

    bool anyChange = paletteChanged;
    size_t total = (size_t)width * height;

    // The pixels as one run (rows packed), for the comparison below.
    coop_frame_scratch.resize(total);
    unsigned char* current = coop_frame_scratch.data();
    for (int row = 0; row < height; row++) {
        memcpy(current + (size_t)row * width, pixels + (size_t)row * pitch, width);
    }
    unsigned char* sent = coop_sent_frame.data();

    size_t index = 0;
    while (index < total) {
        size_t start = index;
        if (!full) {
            // Unchanged pixels, a word at a time while they match.
            while (index + 8 <= total && memcmp(current + index, sent + index, 8) == 0) {
                index += 8;
            }
            while (index < total && current[index] == sent[index]) {
                index++;
            }
        }
        size_t unchanged = index - start;

        start = index;
        while (index < total && (full || current[index] != sent[index])) {
            index++;
        }
        size_t changed = index - start;

        coop_write_varint(&writer, (unsigned int)unchanged);
        coop_write_varint(&writer, (unsigned int)changed);
        if (changed != 0) {
            writer.bytes(current + start, changed);
            memcpy(sent + start, current + start, changed);
            anyChange = true;
        }
    }

    // An unchanged frame now and then keeps a quiet connection open.
    static unsigned int lastFrameSent = 0;
    if (anyChange || elapsed_time(lastFrameSent) > COOP_NET_KEEPALIVE) {
        lastFrameSent = get_time();
        // Compressed when player 2 understands it and it's smaller.
        static std::vector<unsigned char> packed;
        bool sendPacked = false;
        if (coop_client_unpacks && writer.data.size() > 256) {
            coop_net_compress(writer.data.data(), writer.data.size(), &packed);
            sendPacked = packed.size() + 4 < writer.data.size();
        }
        if (sendPacked) {
            CoopWriter packedWriter;
            packedWriter.u32((unsigned int)writer.data.size());
            packedWriter.bytes(packed.data(), packed.size());
            coop_connection_send(&coop_client, COOP_NET_MSG_FRAME_PACKED, packedWriter.data);
            coop_stats_raw_bytes += writer.data.size();
            writer.data.swap(packedWriter.data);
        } else {
            coop_connection_send(&coop_client, COOP_NET_MSG_FRAME, writer.data);
            coop_stats_raw_bytes += writer.data.size();
        }
        coop_frames_sent++;
        if (coop_client_acks) {
            coop_frame_sent_at.push_back(get_time());
        }

        coop_stats_bytes += writer.data.size() + COOP_NET_HEADER_SIZE;
        coop_stats_frames++;
        if (coop_stats_since == 0) {
            coop_stats_since = get_time();
        } else if (elapsed_time(coop_stats_since) >= 10000) {
            unsigned int elapsed = elapsed_time(coop_stats_since);
            debug_printf("\nCOOP NET: %u frames, %llu KB/s (%llu KB/s unpacked), %llu bytes/frame, delay %u ms (max %u)\n",
                coop_stats_frames,
                coop_stats_bytes * 1000 / elapsed / 1024,
                coop_stats_raw_bytes * 1000 / elapsed / 1024,
                coop_stats_bytes / coop_stats_frames,
                coop_stats_delay_count != 0 ? (unsigned int)(coop_stats_delay_total / coop_stats_delay_count) : 0,
                coop_stats_delay_max);
            coop_stats_delay_total = 0;
            coop_stats_raw_bytes = 0;
            coop_stats_delay_count = 0;
            coop_stats_delay_max = 0;
            coop_stats_bytes = 0;
            coop_stats_frames = 0;
            coop_stats_since = get_time();
        }
    }
}

void coop_net_send_status(const char* text)
{
    if (!coop_client_ready || coop_sent_status == text) {
        return;
    }

    coop_sent_status = text;

    CoopWriter writer;
    writer.str(text);
    coop_connection_send(&coop_client, COOP_NET_MSG_STATUS, writer.data);
}

void coop_net_client_request_exit()
{
    coop_client_exit_requested = true;
}

static void coop_net_write_sound(int event, const char* name, int a, int b, int c)
{
    CoopWriter writer;
    writer.u8(event);
    writer.str(name);
    writer.u32((unsigned int)a);
    writer.u32((unsigned int)b);
    writer.u32((unsigned int)c);
    coop_connection_send(&coop_client, COOP_NET_MSG_SOUND, writer.data);
}

void coop_net_send_sound(int event, const char* name, int a, int b, int c)
{
    if (event == GSOUND_EVENT_MUSIC) {
        coop_music_playing = true;
        coop_music_name = name;
        coop_music_args[0] = a;
        coop_music_args[1] = b;
        coop_music_args[2] = c;
    } else if (event == GSOUND_EVENT_MUSIC_STOP) {
        coop_music_playing = false;
    }

    if (coop_client_ready && coop_client.out.size() < COOP_NET_MAX_BACKLOG) {
        coop_net_write_sound(event, name, a, b, c);
    }
}

void coop_net_set_client_exit_hook(void (*proc)())
{
    coop_client_exit_hook = proc != NULL ? proc : coop_client_exit_hook_default;
}

// -----------------------------------------------------------------------------
// Relay: both players connect out to the co-op server, which passes the
// bytes on, so player 1 needs no open port (see tools/coop_server.py).

#define COOP_RELAY_CONNECT_TIMEOUT 5000
#define COOP_RELAY_JOIN_TIMEOUT 20000
#define COOP_RELAY_RETRY_INTERVAL 5000

// host:port of the relay, and the game's code (host: own room; client:
// the room to join, empty for a direct connection).
static std::string coop_relay_server;
static std::string coop_relay_code_value;
static std::string coop_relay_join_code;
// Client: the host's screen size (0 = not told, before 1.5.7).
static int coop_client_host_width = 0;
static int coop_client_host_height = 0;
static CoopSocket coop_relay_control = COOP_INVALID_SOCKET;
static std::string coop_relay_control_in;
static unsigned int coop_relay_retry_at = 0;
static std::deque<CoopSocket> coop_relay_incoming;

// Connects to "host:port" (blocking, as before relays existed).
static CoopSocket coop_connect_direct(const std::string& host, int port, std::string* error)
{
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portText[16];
    snprintf(portText, sizeof(portText), "%d", port);

    addrinfo* addresses = NULL;
    if (getaddrinfo(host.c_str(), portText, &hints, &addresses) != 0 || addresses == NULL) {
        *error = "Cannot resolve host " + host + ".";
        return COOP_INVALID_SOCKET;
    }

    CoopSocket socket = ::socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
    bool connected = socket != COOP_INVALID_SOCKET && connect(socket, addresses->ai_addr, (int)addresses->ai_addrlen) == 0;
    freeaddrinfo(addresses);

    if (!connected) {
        if (socket != COOP_INVALID_SOCKET) {
            coop_close_socket(socket);
        }
        *error = "Cannot connect to " + host + ".";
        return COOP_INVALID_SOCKET;
    }

    return socket;
}

// Waits up to `ms` for `socket` to be readable (or writable).
static bool coop_socket_wait(CoopSocket socket, bool write, unsigned int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval wait = { (long)(ms / 1000), (long)((ms % 1000) * 1000) };
    return select((int)socket + 1, write ? NULL : &set, write ? &set : NULL, NULL, &wait) > 0;
}

static bool coop_relay_send_line(CoopSocket socket, const std::string& line);
static bool coop_relay_read_line(CoopSocket socket, unsigned int ms, std::string* line);

// Connects to the relay without hanging the game for long when it can't
// be reached. The relay is "host:port", or "http://host[:port]/path" when
// it sits behind a web server (an HTTP upgrade first; port 80 is open
// almost everywhere). The socket is non-blocking.
static CoopSocket coop_relay_connect(const std::string& server, std::string* error)
{
    std::string address = server;
    std::string path;
    bool http = address.compare(0, 7, "http://") == 0;
    if (http) {
        address = address.substr(7);
        size_t slash = address.find('/');
        path = slash != std::string::npos ? address.substr(slash) : "/";
        address = address.substr(0, slash);
        if (address.find(':') == std::string::npos) {
            address += ":80";
        }
    }

    size_t colon = address.rfind(':');
    if (address.empty() || colon == std::string::npos) {
        *error = "No co-op server set.";
        return COOP_INVALID_SOCKET;
    }
    std::string host = address.substr(0, colon);
    std::string port = address.substr(colon + 1);

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = NULL;
    if (!coop_socket_startup() || getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0 || addresses == NULL) {
        *error = "Cannot reach the co-op server.";
        return COOP_INVALID_SOCKET;
    }

    CoopSocket socket = ::socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
    bool connected = false;
    if (socket != COOP_INVALID_SOCKET && coop_socket_set_non_blocking(socket)) {
        if (connect(socket, addresses->ai_addr, (int)addresses->ai_addrlen) == 0) {
            connected = true;
        } else if (coop_socket_would_block() && coop_socket_wait(socket, true, COOP_RELAY_CONNECT_TIMEOUT)) {
            int socketError = 0;
            socklen_t length = sizeof(socketError);
            connected = getsockopt(socket, SOL_SOCKET, SO_ERROR, (char*)&socketError, &length) == 0 && socketError == 0;
        }
    }
    freeaddrinfo(addresses);

    if (!connected) {
        if (socket != COOP_INVALID_SOCKET) {
            coop_close_socket(socket);
        }
        *error = "Cannot reach the co-op server.";
        return COOP_INVALID_SOCKET;
    }

    int noDelay = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

    if (http) {
        // The web server routes this host name to the relay.
        std::string status;
        bool upgraded = coop_relay_send_line(socket, "GET " + path + " HTTP/1.1\r\nHost: fallout-coop-relay\r\nUpgrade: fcoop-relay\r\nConnection: Upgrade\r")
            && coop_relay_send_line(socket, "\r")
            && coop_relay_read_line(socket, COOP_RELAY_CONNECT_TIMEOUT, &status)
            && status.compare(0, 12, "HTTP/1.1 101") == 0;
        std::string header = "-";
        while (upgraded && !header.empty()) {
            upgraded = coop_relay_read_line(socket, COOP_RELAY_CONNECT_TIMEOUT, &header);
        }
        if (!upgraded) {
            debug_printf("\nCOOP NET: relay upgrade failed: %s\n", status.c_str());
            coop_close_socket(socket);
            *error = "The co-op server did not answer.";
            return COOP_INVALID_SOCKET;
        }
    }

    return socket;
}

static bool coop_relay_send_line(CoopSocket socket, const std::string& line)
{
    std::string text = line + "\n";
    size_t sent = 0;
    unsigned int start = get_time();
    while (sent < text.size() && elapsed_time(start) < COOP_RELAY_CONNECT_TIMEOUT) {
        int count = (int)send(socket, text.data() + sent, (int)(text.size() - sent), COOP_SEND_FLAGS);
        if (count > 0) {
            sent += count;
        } else if (!coop_socket_would_block() || !coop_socket_wait(socket, true, 100)) {
            if (!coop_socket_would_block()) {
                return false;
            }
        }
    }
    return sent == text.size();
}

// Reads one line, a byte at a time (nothing after it is taken: the game's
// own bytes follow).
static bool coop_relay_read_line(CoopSocket socket, unsigned int ms, std::string* line)
{
    line->clear();
    unsigned int start = get_time();
    while (elapsed_time(start) < ms && line->size() < 200) {
        char ch;
        int count = (int)recv(socket, &ch, 1, 0);
        if (count == 1) {
            if (ch == '\n') {
                return true;
            }
            if (ch != '\r') {
                *line += ch;
            }
        } else if (count == 0 || !coop_socket_would_block()) {
            return false;
        } else {
            coop_socket_wait(socket, false, 100);
        }
    }
    return false;
}

void coop_net_set_relay_server(const std::string& server)
{
    coop_relay_server = server;
}

void coop_net_set_relay_join(const std::string& code)
{
    coop_relay_join_code = code;
}

const char* coop_net_relay_code()
{
    return coop_relay_code_value.c_str();
}

// Asks the relay for player 1's room (`wanted`: the code to keep when
// reconnecting). Returns the control connection, and the code.
static CoopSocket coop_relay_request_room(const std::string& server, const std::string& wanted, std::string* code, std::string* error)
{
    CoopSocket socket = coop_relay_connect(server, error);
    if (socket == COOP_INVALID_SOCKET) {
        return COOP_INVALID_SOCKET;
    }

    std::string line;
    if (!coop_relay_send_line(socket, "FCOOP-RELAY HOST " + (wanted.empty() ? std::string("-") : wanted))
        || !coop_relay_read_line(socket, COOP_RELAY_CONNECT_TIMEOUT, &line)
        || line.compare(0, 5, "ROOM ") != 0) {
        *error = line.compare(0, 4, "ERR ") == 0 ? line.substr(4) : "The co-op server did not answer.";
        coop_close_socket(socket);
        return COOP_INVALID_SOCKET;
    }

    *code = line.substr(5);
    return socket;
}

static void coop_relay_set_control(CoopSocket socket, const std::string& code)
{
    coop_relay_control = socket;
    coop_relay_control_in.clear();
    coop_relay_code_value = code;
    debug_printf("\nCOOP NET: relay room %s\n", code.c_str());
}

// Reconnecting runs in the background (the server may be slow to answer
// or down for a while), and the game picks up the result.
// state: 0 running, 1 done (result ready), 2 abandoned by the game; who
// moves it last closes an unwanted socket.
typedef struct CoopRelayReconnect {
    std::atomic<int> state { 0 };
    std::string wanted;
    CoopSocket socket = COOP_INVALID_SOCKET;
    std::string code;
    std::string error;
} CoopRelayReconnect;

static std::shared_ptr<CoopRelayReconnect> coop_relay_reconnect;

bool coop_net_relay_host_start(std::string* error, const std::string& wanted)
{
    coop_net_relay_host_stop();

    std::string code;
    CoopSocket socket = coop_relay_request_room(coop_relay_server, wanted, &code, error);
    if (socket == COOP_INVALID_SOCKET) {
        return false;
    }
    coop_relay_set_control(socket, code);
    return true;
}

void coop_net_relay_host_stop()
{
    if (coop_relay_control != COOP_INVALID_SOCKET) {
        coop_close_socket(coop_relay_control);
        coop_relay_control = COOP_INVALID_SOCKET;
    }
    for (CoopSocket socket : coop_relay_incoming) {
        coop_close_socket(socket);
    }
    coop_relay_incoming.clear();
    coop_relay_code_value.clear();

    // A reconnect still running closes its own socket when it is done.
    if (coop_relay_reconnect != NULL) {
        std::shared_ptr<CoopRelayReconnect> pending = coop_relay_reconnect;
        coop_relay_reconnect = NULL;
        if (pending->state.exchange(2) == 1 && pending->socket != COOP_INVALID_SOCKET) {
            coop_close_socket(pending->socket);
        }
    }
}

static void coop_relay_host_pump()
{
    if (coop_relay_code_value.empty()) {
        return;
    }

    if (coop_relay_control == COOP_INVALID_SOCKET) {
        // Lost (the server restarted, say): try to get the same code back,
        // in the background.
        if (coop_relay_reconnect != NULL) {
            if (coop_relay_reconnect->state != 1) {
                return;
            }
            std::shared_ptr<CoopRelayReconnect> result = coop_relay_reconnect;
            coop_relay_reconnect = NULL;
            if (result->socket != COOP_INVALID_SOCKET) {
                if (result->code != result->wanted) {
                    // Someone else got the old code meanwhile: tell player 1.
                    std::string message = "New online game code: " + result->code;
                    display_print((char*)message.c_str());
                }
                coop_relay_set_control(result->socket, result->code);
            } else {
                debug_printf("\nCOOP NET: relay: %s\n", result->error.c_str());
            }
            coop_relay_retry_at = get_time();
            return;
        }

        if (elapsed_time(coop_relay_retry_at) < COOP_RELAY_RETRY_INTERVAL) {
            return;
        }

        std::shared_ptr<CoopRelayReconnect> attempt = std::make_shared<CoopRelayReconnect>();
        attempt->wanted = coop_relay_code_value;
        coop_relay_reconnect = attempt;
        std::string server = coop_relay_server;
        std::thread([attempt, server]() {
            std::string code;
            std::string error;
            CoopSocket socket = coop_relay_request_room(server, attempt->wanted, &code, &error);
            attempt->socket = socket;
            attempt->code = code;
            attempt->error = error;
            if (attempt->state.exchange(1) == 2 && socket != COOP_INVALID_SOCKET) {
                coop_close_socket(socket);
            }
        }).detach();
        return;
    }

    char buffer[256];
    for (;;) {
        int count = (int)recv(coop_relay_control, buffer, sizeof(buffer), 0);
        if (count > 0) {
            coop_relay_control_in.append(buffer, count);
            continue;
        }
        if (count == 0 || !coop_socket_would_block()) {
            debug_printf("\nCOOP NET: relay connection lost\n");
            coop_close_socket(coop_relay_control);
            coop_relay_control = COOP_INVALID_SOCKET;
            coop_relay_retry_at = get_time() - COOP_RELAY_RETRY_INTERVAL + 1000;
        }
        break;
    }

    size_t end;
    while ((end = coop_relay_control_in.find('\n')) != std::string::npos) {
        std::string line = coop_relay_control_in.substr(0, end);
        coop_relay_control_in.erase(0, end + 1);
        if (line.compare(0, 5, "CONN ") != 0) {
            continue; // PING
        }

        // Someone joins: a connection of our own for them.
        std::string error;
        CoopSocket socket = coop_relay_connect(coop_relay_server, &error);
        if (socket == COOP_INVALID_SOCKET) {
            debug_printf("\nCOOP NET: relay: %s\n", error.c_str());
            continue;
        }
        if (!coop_relay_send_line(socket, "FCOOP-RELAY ACCEPT " + coop_relay_code_value + " " + line.substr(5))) {
            coop_close_socket(socket);
            continue;
        }
        debug_printf("\nCOOP NET: relay: player joining\n");
        coop_relay_incoming.push_back(socket);
    }
}

static CoopSocket coop_relay_take_incoming()
{
    if (coop_relay_incoming.empty()) {
        return COOP_INVALID_SOCKET;
    }
    CoopSocket socket = coop_relay_incoming.front();
    coop_relay_incoming.pop_front();
    return socket;
}

static CoopSocket coop_relay_join(std::string* error)
{
    CoopSocket socket = coop_relay_connect(coop_relay_server, error);
    if (socket == COOP_INVALID_SOCKET) {
        return COOP_INVALID_SOCKET;
    }

    std::string line;
    if (!coop_relay_send_line(socket, "FCOOP-RELAY JOIN " + coop_relay_join_code)
        || !coop_relay_read_line(socket, COOP_RELAY_JOIN_TIMEOUT, &line)
        || line != "OK") {
        *error = line.compare(0, 4, "ERR ") == 0 ? line.substr(4) : "No answer through the co-op server.";
        coop_close_socket(socket);
        return COOP_INVALID_SOCKET;
    }

    debug_printf("\nCOOP NET: joined %s through the relay\n", coop_relay_join_code.c_str());
    return socket;
}

bool coop_net_parse_code(const char* text, std::string* code)
{
    // Three letters and three digits; a dash or space between is fine.
    std::string compact;
    for (const char* ch = text; *ch != '\0'; ch++) {
        if (*ch != '-' && *ch != ' ') {
            compact += (char)toupper((unsigned char)*ch);
        }
    }
    if (compact.size() != 6) {
        return false;
    }
    for (int index = 0; index < 6; index++) {
        bool letter = compact[index] >= 'A' && compact[index] <= 'Z';
        bool digit = compact[index] >= '0' && compact[index] <= '9';
        if (index < 3 ? !letter : !digit) {
            return false;
        }
    }
    *code = compact;
    return true;
}

// -----------------------------------------------------------------------------
// Client

static bool coop_client_connect(CoopConnection* connection, std::string* error)
{
    if (!coop_socket_startup()) {
        *error = "Networking unavailable.";
        return false;
    }

    CoopSocket socket;
    if (!coop_relay_join_code.empty()) {
        socket = coop_relay_join(error);
        if (socket == COOP_INVALID_SOCKET) {
            return false;
        }
    } else {
        if (coop_net_host_name.empty()) {
            *error = "No host configured ([coop] host=...).";
            return false;
        }

        socket = coop_connect_direct(coop_net_host_name, coop_net_port_value, error);
        if (socket == COOP_INVALID_SOCKET) {
            return false;
        }
    }

    int noDelay = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
    coop_socket_set_non_blocking(socket);
    connection->socket = socket;

    CoopWriter hello;
    hello.u32(COOP_NET_MAGIC);
    hello.u32(COOP_NET_PROTOCOL_VERSION);
    hello.u64(coop_net_data_checksum());
    int outlines[2];
    coop_outline_colors_from_config(outlines);
    // Flags: 1 colorblind (1.2), 2 acknowledges frames, 4 packed frames (1.5.7).
    hello.u8((outlines[0] == 2 && outlines[1] == 3 ? 1 : 0) | (coop_net_no_acks ? 0 : 2) | (coop_net_no_packing ? 0 : 4));
    hello.u8(outlines[0]);
    hello.u8(outlines[1]);
    hello.u16((unsigned int)screenGetWidth());
    hello.u16((unsigned int)screenGetHeight());
    coop_connection_send(connection, COOP_NET_MSG_HELLO, hello.data);

    unsigned int start = get_time();
    while (elapsed_time(start) < COOP_NET_HANDSHAKE_TIMEOUT) {
        coop_connection_flush(connection);
        coop_connection_receive(connection);

        CoopMessage message;
        if (coop_connection_next(connection, &message) && message.type == COOP_NET_MSG_WELCOME) {
            CoopReader reader(message.payload);
            bool accepted = reader.u8() != 0;
            std::string reason = reader.str();
            coop_client_host_width = 0;
            coop_client_host_height = 0;
            if (!reader.atEnd()) {
                coop_client_host_width = reader.u16();
                coop_client_host_height = reader.u16();
            }
            if (!accepted) {
                *error = "Host refused: " + reason;
                coop_connection_close(connection);
                return false;
            }
            return true;
        }

        if (connection->failed) {
            break;
        }

        SDL_Delay(10);
    }

    *error = "No answer from the host.";
    coop_connection_close(connection);
    return false;
}

// Player 1's frames on this screen: their size, and where they are drawn
// (scaled to fit, keeping their shape, when the screen sizes differ).
static int coop_client_frame_width = 0;
static int coop_client_frame_height = 0;
static int coop_client_draw_x = 0;
static int coop_client_draw_y = 0;
static int coop_client_draw_width = 0;
static int coop_client_draw_height = 0;

bool coop_net_client_host_size(int* width, int* height)
{
    *width = coop_client_host_width;
    *height = coop_client_host_height;
    return coop_client_host_width > 0 && coop_client_host_height > 0;
}

bool coop_net_client_frame_size(int* width, int* height)
{
    *width = coop_client_frame_width;
    *height = coop_client_frame_height;
    return coop_client_draw_width != 0;
}

void coop_net_client_frame_to_window(int* x, int* y)
{
    if (coop_client_draw_width != 0) {
        *x = coop_client_draw_x + (2 * *x + 1) * coop_client_draw_width / (2 * coop_client_frame_width);
        *y = coop_client_draw_y + (2 * *y + 1) * coop_client_draw_height / (2 * coop_client_frame_height);
    }
}

int coop_net_client_run()
{
    CoopConnection connection;
    std::string error;
    coop_client_exit_requested = false;
    coop_net_error.clear();
    if (!coop_client_connect(&connection, &error)) {
        debug_printf("\nCOOP NET: %s\n", error.c_str());
        coop_net_error = error;
        return 1;
    }

    debug_printf("\nCOOP NET: connected to %s\n", coop_net_host_name.c_str());

    // The host's palette comes with its frames, color cycling included; the
    // client's own cycling would overwrite parts of it (wrong colors in the
    // ending slides, say, where the host stops cycling).
    struct NoColorCycling {
        bool wasEnabled = cycle_is_enabled();
        NoColorCycling() { cycle_disable(); }
        ~NoColorCycling()
        {
            if (wasEnabled) {
                cycle_enable();
            }
        }
    } noColorCycling;

    std::vector<unsigned char> frame;
    int& frameWidth = coop_client_frame_width;
    int& frameHeight = coop_client_frame_height;
    int& drawX = coop_client_draw_x;
    int& drawY = coop_client_draw_y;
    int& drawWidth = coop_client_draw_width;
    int& drawHeight = coop_client_draw_height;
    frameWidth = 0;
    frameHeight = 0;
    drawWidth = 0;
    drawHeight = 0;
    std::vector<int> drawColumns;
    unsigned int framesReceived = 0;
    unsigned int framesAcked = 0;
    std::string status;

    int soundsPlayed = 0;

    int sentX = -1;
    int sentY = -1;
    int sentButtons = -1;

    // Frames go into a full-screen window, so the window system (mouse
    // cursor, screenshots) sees them.
    int window = win_add(0, 0, screenGetWidth(), screenGetHeight(), colorTable[0], WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (window == -1) {
        coop_connection_close(&connection);
        return 1;
    }
    unsigned char* windowBuffer = win_get_buf(window);
    int windowWidth = screenGetWidth();
    int windowHeight = screenGetHeight();
    win_draw(window);

    // The host draws player 2's cursor into the frames; keep the local one
    // invisible (but working, so the mouse is still tracked).
    // (Static: the mouse keeps a pointer to the shape.)
    static unsigned char blankCursor = 0;
    mouse_set_shape(&blankCursor, 1, 1, 1, 0, 0, 0);
    mouse_show();

    bool hostLeft = false;
    bool hostSaidBye = false;
    while (!hostLeft) {
        sharedFpsLimiter.mark();

        // Local input goes to the host, except the quit keys: they leave.
        int keyCode = get_input();
        if (keyCode == KEY_F10 || keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X) {
            coop_client_exit_requested = true;
        } else if (keyCode >= 0) {
            CoopWriter writer;
            writer.u8(COOP_NET_INPUT_KEY);
            writer.u16(0);
            writer.u16(0);
            writer.u32((unsigned int)keyCode);
            coop_connection_send(&connection, COOP_NET_MSG_INPUT, writer.data);
        }

        int x;
        int y;
        mouse_get_position(&x, &y);
        if (drawWidth != 0 && (drawWidth != frameWidth || drawHeight != frameHeight)) {
            x = std::clamp((x - drawX) * frameWidth / drawWidth, 0, frameWidth - 1);
            y = std::clamp((y - drawY) * frameHeight / drawHeight, 0, frameHeight - 1);
        }
        int mouseEvents = mouse_get_buttons();
        int buttons = 0;
        if ((mouseEvents & MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT) != 0) {
            buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
        }
        if ((mouseEvents & MOUSE_EVENT_RIGHT_BUTTON_DOWN_REPEAT) != 0) {
            buttons |= MOUSE_STATE_RIGHT_BUTTON_DOWN;
        }

        // Also now and then while nothing changes: a quiet connection may
        // be closed on the way (proxies, the co-op server's relay).
        static unsigned int lastMouseSent = 0;
        if (x != sentX || y != sentY || buttons != sentButtons || elapsed_time(lastMouseSent) > COOP_NET_KEEPALIVE) {
            lastMouseSent = get_time();
            CoopWriter writer;
            writer.u8(COOP_NET_INPUT_MOUSE);
            writer.u16((unsigned int)x);
            writer.u16((unsigned int)y);
            writer.u32((unsigned int)buttons);
            coop_connection_send(&connection, COOP_NET_MSG_INPUT, writer.data);
            sentX = x;
            sentY = y;
            sentButtons = buttons;
        }

        // Host output.
        coop_connection_flush(&connection);
        coop_connection_receive(&connection);

        bool redraw = false;
        CoopMessage message;
        while (coop_connection_next(&connection, &message)) {
            if (message.type == COOP_NET_MSG_FRAME_PACKED) {
                // Unpacked, it's a normal frame.
                static std::vector<unsigned char> unpacked;
                CoopReader packed(message.payload);
                size_t size = packed.u32();
                bool ok = packed.ok() && size <= COOP_NET_MAX_MESSAGE
                    && coop_net_decompress(message.payload.data() + 4, message.payload.size() - 4, size, &unpacked);
                if (!ok) {
                    debug_printf("\nCOOP NET: damaged packed frame\n");
                    connection.failed = true;
                    break;
                }
                message.payload.swap(unpacked);
                message.type = COOP_NET_MSG_FRAME;
            }
            CoopReader reader(message.payload);
            switch (message.type) {
            case COOP_NET_MSG_FRAME:
                framesReceived++;
                if (true) {
                    int width = reader.u16();
                    int height = reader.u16();
                    int flags = reader.u8();

                    if (width != frameWidth || height != frameHeight) {
                        frame.assign((size_t)width * height, 0);
                        frameWidth = width;
                        frameHeight = height;

                        if (width > 0 && height > 0) {
                            if (width * windowHeight <= height * windowWidth) {
                                drawHeight = windowHeight;
                                drawWidth = width * windowHeight / height;
                            } else {
                                drawWidth = windowWidth;
                                drawHeight = height * windowWidth / width;
                            }
                            drawX = (windowWidth - drawWidth) / 2;
                            drawY = (windowHeight - drawHeight) / 2;
                            drawColumns.resize(drawWidth);
                            for (int column = 0; column < drawWidth; column++) {
                                drawColumns[column] = column * width / drawWidth;
                            }
                            memset(windowBuffer, colorTable[0], (size_t)windowWidth * windowHeight);
                            debug_printf("COOP NET: frames %dx%d shown at %dx%d\n", width, height, drawWidth, drawHeight);
                        }
                    }

                    if ((flags & 1) != 0) {
                        const unsigned char* rgb = reader.bytes(768);
                        if (rgb != NULL) {
                            unsigned char palette[768];
                            for (int index = 0; index < 768; index++) {
                                palette[index] = rgb[index] >> 2;
                            }
                            setSystemPalette(palette);
                        }
                    }

                    size_t total = frame.size();
                    size_t index = 0;
                    while (index < total && reader.ok()) {
                        index += coop_read_varint(&reader);
                        size_t count = coop_read_varint(&reader);
                        const unsigned char* pixels = reader.bytes(count);
                        if (pixels == NULL || index + count > total) {
                            break;
                        }
                        memcpy(frame.data() + index, pixels, count);
                        index += count;
                    }

                    redraw = true;
                }
                break;
            case COOP_NET_MSG_STATUS:
                status = reader.str();
                redraw = true;
                break;
            case COOP_NET_MSG_SOUND:
                if (true) {
                    int event = reader.u8();
                    std::string name = reader.str();
                    int a = (int)reader.u32();
                    int b = (int)reader.u32();
                    int c = (int)reader.u32();
                    if (reader.ok()) {
                        soundsPlayed++;
                        switch (event) {
                        case GSOUND_EVENT_SFX:
                            gsound_play_sfx_file_volume(name.c_str(), a);
                            break;
                        case GSOUND_EVENT_MUSIC:
                            gsound_background_play(name.c_str(), a, b, c);
                            break;
                        case GSOUND_EVENT_MUSIC_STOP:
                            gsound_background_stop();
                            break;
                        case GSOUND_EVENT_SPEECH:
                            if (a == 0 && b == 0 && c == 0) {
                                // A conversation's lip-synced speech.
                                gsound_speech_play(name.c_str(), 10, 14, 15);
                            } else {
                                gsound_speech_play(name.c_str(), a, b, c);
                            }
                            break;
                        case GSOUND_EVENT_SPEECH_STOP:
                            gsound_speech_stop();
                            break;
                        }
                    }
                }
                break;
            case COOP_NET_MSG_BYE:
                hostLeft = true;
                hostSaidBye = true;
                break;
            }
        }

        // Frames received so far, so the host sends the next ones.
        if (framesReceived != framesAcked && !coop_net_no_acks) {
            CoopWriter writer;
            writer.u32(framesReceived);
            coop_connection_send(&connection, COOP_NET_MSG_ACK, writer.data);
            framesAcked = framesReceived;
        }

        if (connection.failed || coop_client_exit_requested) {
            hostLeft = true;
        }

        if (redraw && frameWidth != 0) {
            int width = drawX + drawWidth;
            int height = drawY + drawHeight;
            for (int row = 0; row < drawHeight; row++) {
                unsigned char* dest = windowBuffer + (size_t)(drawY + row) * windowWidth + drawX;
                const unsigned char* src = frame.data() + (size_t)(row * frameHeight / drawHeight) * frameWidth;
                if (drawWidth == frameWidth) {
                    memcpy(dest, src, drawWidth);
                } else {
                    for (int column = 0; column < drawWidth; column++) {
                        dest[column] = src[drawColumns[column]];
                    }
                }
            }

            if (!status.empty()) {
                // Status line over the top of the picture.
                for (int row = 0; row < 14 && row < height; row++) {
                    memset(windowBuffer + row * windowWidth, colorTable[0], width);
                }
                int oldFont = text_curr();
                text_font(101);
                text_to_buf(windowBuffer + 2 * windowWidth + 4, status.c_str(), width - 8, windowWidth, colorTable[992]);
                text_font(oldFont);
            }

            win_draw(window);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    debug_printf("\nCOOP NET: host left (%d sounds played)\n", soundsPlayed);
    if (!coop_client_exit_requested) {
        // A goodbye from the host, or the connection broke (network, or the
        // co-op server between restarting): then joining again may work.
        coop_net_error = hostSaidBye ? "Player 1 has left the game." : "The connection to player 1 was lost.";
    }
    coop_client_exit_hook();
    win_delete(window);
    frameWidth = 0;
    frameHeight = 0;
    drawWidth = 0;
    drawHeight = 0;

    // The game's own cursor again (the menu after leaving).
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    coop_connection_send(&connection, COOP_NET_MSG_BYE, std::vector<unsigned char>());
    coop_connection_flush(&connection);
    coop_connection_close(&connection);
    return 0;
}

} // namespace fallout
