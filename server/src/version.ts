// The server's own version (package.json: tools/set-version.sh server X.Y.Z), released apart from
// Rondo on the Y1 (tags server-v<version>), and the device–server protocol's number, raised only when
// a change needs both sides: Rondo names the lowest it works with (kServerApiNeeded in
// native/core/version.h) and asks for an update of an older server. GET /healthz says both.
import { readFileSync } from "node:fs";

export const VERSION: string = JSON.parse(readFileSync(new URL("../package.json", import.meta.url), "utf8")).version;
export const API_VERSION = 1;
