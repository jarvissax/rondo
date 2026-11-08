// What every request to the server shares (Api.kt, Tls.kt and ConnectRetry.kt in 1.0.x): the user
// agent, the roots it trusts, and which failures are tried again at once.
#pragma once
#include <curl/curl.h>

namespace rondo::net {

// "Rondo/<version> (Innioasis Y1; native)"
const char* userAgent();
// The two bundled roots (ISRG Root X1 and YR, kept as extra trusted roots), or with wide, Android's
// system roots as well: a server whose certificate isn't Let's Encrypt's. A request that fails
// verification with the bundled two is made again with wide (the system's set is read once, and
// costs more to load per connection).
void trust(CURL* curl, bool wide);
// A failure 1.0.x retried after 250 ms, up to 20 times: the hotspot refusing a connect, a reset
// (OkHttp's SocketException).
bool retryable(CURLcode rc);

}  // namespace rondo::net
