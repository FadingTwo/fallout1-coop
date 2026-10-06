#ifndef FALLOUT_GAME_COOP_NET_H_
#define FALLOUT_GAME_COOP_NET_H_

#include <string>

#include <stddef.h>
#include <stdint.h>

namespace fallout {

// Co-op over the network (see coop.h): player 1's machine hosts and runs the
// whole game; player 2's machine is a terminal that shows frames rendered by
// the host and sends its mouse and keyboard input back.
//
// fallout.cfg:
//   [coop]
//   enabled=1
//   mode=host          ; or "client"; absent = both players on one machine
//   host=192.168.1.10  ; client only
//   port=27015

#define COOP_CONFIG_MODE_KEY "mode"
#define COOP_CONFIG_HOST_KEY "host"
#define COOP_CONFIG_PORT_KEY "port"

#define COOP_NET_DEFAULT_PORT 27015

typedef enum CoopNetMode {
    COOP_NET_NONE,
    COOP_NET_HOST,
    COOP_NET_CLIENT,
} CoopNetMode;

typedef enum CoopNetMessageType {
    // client -> host: magic, protocol version, data checksum.
    COOP_NET_MSG_HELLO = 1,
    // host -> client: accepted flag, reason.
    COOP_NET_MSG_WELCOME = 2,
    // client -> host: input event.
    COOP_NET_MSG_INPUT = 3,
    // host -> client: screen update.
    COOP_NET_MSG_FRAME = 4,
    // host -> client: status line shown over the frame.
    COOP_NET_MSG_STATUS = 5,
    // host -> client: sound effect to play.
    COOP_NET_MSG_SOUND = 6,
    // either way: leaving.
    COOP_NET_MSG_BYE = 7,
    // Client -> host (since 1.5.7, HELLO flag 2): u32 frames received so
    // far, so the host never has more than a few frames on the way.
    COOP_NET_MSG_ACK = 8,
    // Host -> client (since 1.5.7, HELLO flag 4): a FRAME, compressed (u32
    // its size, then coop_net_compress data).
    COOP_NET_MSG_FRAME_PACKED = 9,
} CoopNetMessageType;

typedef enum CoopNetInputKind {
    // x, y: absolute screen position; value: mouse buttons (MOUSE_STATE_*).
    COOP_NET_INPUT_MOUSE = 0,
    // value: game key code (KEY_*).
    COOP_NET_INPUT_KEY = 1,
} CoopNetInputKind;

typedef struct CoopNetInput {
    int kind;
    int x;
    int y;
    int value;
} CoopNetInput;

CoopNetMode coop_net_mode();

// For this session (the multiplayer menu); not saved.
void coop_net_set_mode(CoopNetMode mode);
void coop_net_set_host(const char* host);
int coop_net_port();
// For one connection (a game picked in the server browser).
void coop_net_set_port(int port);

// Why the last connection attempt failed.
const char* coop_net_last_error();

// This computer's LAN addresses, comma separated ("" if unknown).
void coop_net_local_addresses(char* buffer, size_t size);

// Reads the [coop] network settings. Called by coop_init().
void coop_net_init();
void coop_net_exit();

// Checksum over the game data both machines must share (MASTER.DAT,
// CRITTER.DAT, the patches directory without saves, and the language).
uint64_t coop_net_data_checksum();

// Host: starts listening. Returns false on failure.
// Host: whether another port may be used when the set one is taken
// (online, where players come through the relay). Back to the set port
// when hosting ends.
void coop_net_set_any_port_ok(bool ok);

bool coop_net_host_start();

// Host: accepts a client, completes the handshake and receives input.
// Cheap to call every frame.
void coop_net_host_pump();

// Host: true once a client has passed the handshake.
bool coop_net_client_connected();

// Host: next input event received from the client, if any.
bool coop_net_next_input(CoopNetInput* input);

// Host: sends the current screen (8-bit, with palette) to the client.
void coop_net_send_screen(const unsigned char* pixels, int pitch, int width, int height, const unsigned char* palette);

// Host: sends the screen as currently composed.
void coop_net_send_current_screen();

// Host: a sound for the client to play (GSOUND_EVENT_*, see gsound.h).
void coop_net_send_sound(int event, const char* name, int a, int b, int c);

// Host: status line the client shows over the frames ("" for none).
void coop_net_send_status(const char* text);

// Client: connects, then shows the host's frames and sends input until
// the host leaves or the window is closed. Returns a process exit code.
int coop_net_client_run();

// Relay through the co-op server ("host:port"): no open port needed.
// Host: opens a room and gets its code ("" without one); client: joins a
// code instead of connecting to [coop] host ("" for a direct connection).
void coop_net_set_relay_server(const std::string& server);
bool coop_net_relay_host_start(std::string* error, const std::string& wanted = std::string());
void coop_net_relay_host_stop();
const char* coop_net_relay_code();
void coop_net_set_relay_join(const std::string& code);
// A game code typed by a player (ABC123, ABC-123): true, and the code.
bool coop_net_parse_code(const char* text, std::string* code);

// Tests: packs and unpacks `data`; the packed size, or -1 if it didn't
// come back the same.
int coop_net_pack_selftest(const unsigned char* data, size_t size);

// Client: the size of player 1's frames (false before the first), and a
// point of a frame to this screen's coordinates (frames are scaled to fit
// when the screen sizes differ).
bool coop_net_client_frame_size(int* width, int* height);
// Client: the host's own screen size (frames may be scaled down to fit
// this screen); false when the host didn't say.
bool coop_net_client_host_size(int* width, int* height);
void coop_net_client_frame_to_window(int* x, int* y);

// Client: leave at the next frame.
void coop_net_client_request_exit();

// Client, for tests: called when the host has left, before returning.
void coop_net_set_client_exit_hook(void (*proc)());

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_NET_H_ */
