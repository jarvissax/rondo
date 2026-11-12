// net.h.
#include "net.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <mutex>
#include <string>

#include "version.h"

namespace rondo {
extern const char kRootCerts[];  // generated from native/assets/certs/*.pem (native/CMakeLists.txt)

namespace net {

const char* userAgent() {
  static const std::string ua = std::string("Rondo/") + kRondoVersion + " (Innioasis Y1)";
  return ua.c_str();
}

namespace {
// The bundled roots, then every certificate in /system/etc/security/cacerts (PEM, one a file)
const std::string& wideRoots() {
  static std::once_flag once;
  static std::string all;
  std::call_once(once, [] {
    all = kRootCerts;
    all += "\n";
    const char* dir = "/system/etc/security/cacerts";
    DIR* d = opendir(dir);
    if (!d) return;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string path = std::string(dir) + "/" + e->d_name;
      FILE* f = fopen(path.c_str(), "rb");
      if (!f) continue;
      char buf[8192];
      std::string pem;
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) pem.append(buf, n);
      fclose(f);
      // the file is the PEM block followed by `openssl x509 -text` output: only the block
      size_t a = pem.find("-----BEGIN CERTIFICATE-----"), b = pem.find("-----END CERTIFICATE-----");
      if (a == std::string::npos || b == std::string::npos || b < a) continue;
      all.append(pem, a, b + 25 - a);
      all += "\n";
    }
    closedir(d);
  });
  return all;
}
}  // namespace

void trust(CURL* curl, bool wide) {
  curl_blob roots;
  if (wide) {
    const std::string& all = wideRoots();
    roots = {(void*)all.data(), all.size(), CURL_BLOB_NOCOPY};
  } else {
    roots = {(void*)kRootCerts, strlen(kRootCerts), CURL_BLOB_NOCOPY};
  }
  curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &roots);  // curl copies what it needs per connection
}

bool retryable(CURLcode rc) { return rc == CURLE_COULDNT_CONNECT || rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR; }

}  // namespace net
}  // namespace rondo
