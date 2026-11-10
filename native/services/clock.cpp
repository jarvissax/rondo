// clock.h.
#include "clock.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <cutils/properties.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <yyjson.h>

#include "../core/api.h"
#include "../core/net.h"

namespace rondo {
namespace {

// the kernel's Android alarm driver: what sets the RTC on this kernel (Android's AlarmManagerService)
constexpr unsigned long kAlarmSetRtc = _IOW('a', 5, struct timespec);

size_t onHeader(char* data, size_t size, size_t n, void* out) {
  size_t len = size * n;
  std::string line(data, len);
  if (line.size() > 5 && strncasecmp(line.c_str(), "date:", 5) == 0) {
    std::string v = line.substr(5);
    while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
    *static_cast<std::string*>(out) = v;
  }
  return len;
}
size_t discard(char*, size_t size, size_t n, void*) { return size * n; }

// the certificate's dates aren't checked (the clock is what is wrong); its chain and name are
CURLcode noTimeCheck(CURL*, void* ctx, void*) {
  X509_STORE* store = SSL_CTX_get_cert_store(static_cast<SSL_CTX*>(ctx));
  X509_VERIFY_PARAM* param = X509_STORE_get0_param(store);
  X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_NO_CHECK_TIME);
  return CURLE_OK;
}

// "Sun, 08 Nov 2026 19:25:07 GMT" (RFC 7231's IMF-fixdate), as seconds since 1970
time_t parseDate(const std::string& s) {
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char mon[4] = {};
  int d, y, h, mi, se;
  if (sscanf(s.c_str(), "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) != 6) return 0;
  int m = -1;
  for (int i = 0; i < 12; i++)
    if (!strcmp(mon, kMonths[i])) m = i;
  if (m < 0 || y < 2024) return 0;
  // days from 1970 to the date (the civil-from-days inverse), no time zone involved
  int yy = y - (m < 2), era = yy / 400, yoe = yy - era * 400, mp = (m + 10) % 12;  // m from 0: March is 0
  int doy = (153 * mp + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long long days = (long long)era * 146097 + doe - 719468;
  return time_t(days * 86400 + h * 3600 + mi * 60 + se);
}

}  // namespace

bool syncClock(const std::string& url) {
  if (url.empty()) return false;
  CURL* c = curl_easy_init();
  if (!c) return false;
  std::string date;
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, net::userAgent());
  net::trust(c, false);
  curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, noTimeCheck);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 20000L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &date);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
  CURLcode rc = curl_easy_perform(c);
  if (rc == CURLE_PEER_FAILED_VERIFICATION) {  // a server with another authority: the system's roots
    net::trust(c, true);
    rc = curl_easy_perform(c);
  }
  curl_easy_cleanup(c);
  time_t server = rc == CURLE_OK ? parseDate(date) : 0;
  if (!server) return false;
  time_t here = time(nullptr);
  if (here >= server - 2 && here <= server + 2) return true;
  if (!setWallClock(server)) return false;
  fprintf(stderr, "rondo-core: the clock was %lld s off: set from %s\n", (long long)(here - server), url.c_str());
  return true;
}

namespace {

const char* kZoneFile = "/data/rondo/timezone";

bool firmware2() {
  char fw[PROPERTY_VALUE_MAX] = "";
  property_get("ro.rondo.firmware", fw, "");
  return atoi(fw) >= 2;
}

// whether the firmware's tzdata has the zone (Android 4.2's zoneinfo.idx: a 40-byte name padded with
// zeros, then three 32-bit numbers, for each zone)
bool knownZone(const std::string& name) {
  if (name.empty() || name.size() >= 40) return false;
  FILE* f = fopen("/system/usr/share/zoneinfo/zoneinfo.idx", "rb");
  if (!f) return false;
  char e[52];
  bool found = false;
  while (!found && fread(e, 1, sizeof(e), f) == sizeof(e)) found = strnlen(e, 40) == name.size() && memcmp(e, name.data(), name.size()) == 0;
  fclose(f);
  return found;
}

void setZone(const std::string& name, const char* why) {
  char now[PROPERTY_VALUE_MAX] = "";
  property_get("persist.sys.timezone", now, "");
  if (name != now) {
    property_set("persist.sys.timezone", name.c_str());
    fprintf(stderr, "rondo-core: time zone %s (%s)\n", name.c_str(), why);
  }
  std::string tmp = std::string(kZoneFile) + ".part";
  if (FILE* f = fopen(tmp.c_str(), "w")) {
    bool ok = fprintf(f, "%s\n", name.c_str()) > 0;
    ok = fflush(f) == 0 && ok;
    fclose(f);
    if (ok) rename(tmp.c_str(), kZoneFile);
  }
}

}  // namespace

void timeZoneDefault() {
  if (firmware2() && access(kZoneFile, F_OK) != 0) setZone("Etc/UTC", "the default until the server names one");
}

bool timeZoneFromServer(Api& api) {
  if (!firmware2()) return true;  // 1.0.x's firmware: Android's own setting
  std::string body;
  int status = api.get("/timezone", body);
  if (status == 0 || status >= 500) return false;
  if (status != 200) return true;  // an older server, without /timezone
  yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0);
  yyjson_val* tz = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "timezone") : nullptr;
  std::string name = yyjson_is_str(tz) ? yyjson_get_str(tz) : "";
  yyjson_doc_free(doc);
  if (name.empty()) return true;  // the server couldn't tell: the zone stays
  if (!knownZone(name)) {
    fprintf(stderr, "rondo-core: time zone %s isn't in this firmware's tzdata: left as it was\n", name.c_str());
    return true;
  }
  setZone(name, "from the server");
  return true;
}

}  // namespace rondo

namespace rondo {

bool setWallClock(time_t t) {
  timeval tv{t, 0};
  if (settimeofday(&tv, nullptr) != 0) return false;
  // Android's alarm driver sets the RTC on this kernel (AlarmManagerService's way), so the time holds
  // while the Y1 is off and over a restart
  timespec ts{t, 0};
  int fd = open("/dev/alarm", O_RDWR);
  bool rtc = fd >= 0 && ioctl(fd, kAlarmSetRtc, &ts) == 0;
  if (fd >= 0) close(fd);
  if (!rtc) fprintf(stderr, "rondo: the RTC wasn't set (%s)\n", strerror(errno));
  return true;
}

void setTimeZoneByHand(const std::string& name) {
  if (knownZone(name)) setZone(name, "set by hand");
}

bool knownTimeZone(const std::string& name) { return knownZone(name); }

}  // namespace rondo
