# What's new in the Rondo server

The server's releases, newest first (tags `server-v<version>`, on the
[releases page](https://github.com/jarvissax/rondo/releases)). Until 3.0.1 the server shared Rondo's
version, and its changes are in Rondo's [CHANGELOG.md](../CHANGELOG.md).

## 3.1.1 (November 12, 2026)

- **`/healthz` no longer sends the `api` number** that 3.1.0 added. Nothing reads it: Rondo tells
  whether a server is new enough from the server's version, against the oldest server it works with.
  Nothing else changes, so this update is optional.

## 3.1.0 (November 12, 2026)

- **A version of its own.** The server is released apart from Rondo on the Y1 from now on, so a
  Rondo release no longer asks you to update the server. The admin page's Overview says when a
  newer server is out.
- **`/healthz` says the server's version,** which Rondo checks against the oldest server it works
  with. (It also sends an `api` number, which nothing reads; 3.1.1 drops it.)
