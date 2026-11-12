# What's new in the Rondo server

The server's releases, newest first (tags `server-v<version>`, on the
[releases page](https://github.com/jarvissax/rondo/releases)). Until 3.0.1 the server shared Rondo's
version, and its changes are in Rondo's [CHANGELOG.md](../CHANGELOG.md).

## Unreleased

- **`/healthz` no longer says an API number.** Rondo checks the server's version itself, against the
  oldest server it works with.

## 3.1.0 (November 12, 2026)

- **A version of its own.** The server is released apart from Rondo on the Y1 from now on, so a
  Rondo release no longer asks you to update the server. The admin page's Overview says when a
  newer server is out.
- **`/healthz` says the server's version and its API number,** which Rondo uses to tell whether the
  server is new enough for it.
