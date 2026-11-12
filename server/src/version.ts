// The server's own version (package.json: tools/set-version.sh server X.Y.Z), released apart from
// Rondo on the Y1 (tags server-v<version>). GET /healthz says it, and a Rondo checks it against the
// oldest server it works with (kServerVersionNeeded in native/core/version.h), asking for an update of
// an older one. So a server release keeps working with the Rondos already out there: it adds, and
// doesn't change or take away what they use.
import { readFileSync } from "node:fs";

export const VERSION: string = JSON.parse(readFileSync(new URL("../package.json", import.meta.url), "utf8")).version;
