// Direct mode's "Sign in with a code", as the Rondo server's admin page does it (server/src/tv.ts with
// youtubei.js's OAuth2): the YouTube TV app's own client signs in the way a smart TV does. A code is
// shown (on the dashboard, or on the Y1), the owner enters it at google.com/device, and the tokens are
// kept in <data>/youtube-tv.json (0600, never off the Y1), refreshed as they expire. YouTube takes them
// from its TV client only: they bring YouTube's home for the account, Subscriptions and History, and
// YouTube Music's playlists and Liked Music. cookies.txt (innertube.h) still adds YouTube Music's own
// home and history.
#pragma once
#include <string>

namespace rondo::direct::tv {

// {"signedIn", "name", "handle", "pending": {"url", "code", "expiresAt" (ms since 1970)} | null,
//  "error": "denied" | "expired" | "failed" | "revoked" | null}
std::string stateJson();
// a new code: asks Google, then waits for the approval on a thread of its own; the state as above
std::string begin();
void cancel();
void signOut();  // forgets the tokens here, and gives them back to Google
bool signedIn();
// the access token for a TV request ("" when signed out), refreshed first when it has expired
std::string bearer();
int generation();  // changes with a sign-in or out (answers cached before were the other account's)

}  // namespace rondo::direct::tv
