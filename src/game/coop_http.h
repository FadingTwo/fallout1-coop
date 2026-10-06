#ifndef FALLOUT_GAME_COOP_HTTP_H_
#define FALLOUT_GAME_COOP_HTTP_H_

#include <string>

namespace fallout {

// Minimal blocking HTTP/1.0 client for the co-op menu's update check and
// bug reports. Plain http:// only. `timeoutMs` bounds connecting and each
// wait for data.
//
// Returns true when the server answered with a 2xx status; `body` gets the
// response body, `error` a reason otherwise.
bool coop_http_get(const std::string& url, std::string* body, std::string* error, int timeoutMs = 8000);
bool coop_http_post(const std::string& url, const std::string& contentType, const std::string& data, std::string* body, std::string* error, int timeoutMs = 8000);

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_HTTP_H_ */
