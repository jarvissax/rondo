// Direct mode's local server: the Rondo server's API on 127.0.0.1, answered by routes.h, so every part
// of Rondo (the screens' requests, the artwork loader, the player's streams, the dashboard) talks to
// it as it talks to a Rondo server, in either mode.
#pragma once

namespace rondo::direct {

constexpr int kPort = 47811;
constexpr const char* kBase = "http://127.0.0.1:47811";

// Listens on 127.0.0.1:port, a thread a connection (keep-alive); false when the port can't be had
bool serve(int port = kPort);

}  // namespace rondo::direct
