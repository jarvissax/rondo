// base_rom.h.
#include "base_rom.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cutils/properties.h>

#include <string>
#include <vector>

namespace rondo {
namespace {

// What firmware/build.sh leaves out (its ROOT_DAEMONS, the same list): zxly's agent and the copy of
// itself it keeps, its downloader, the downloader's download tool and state file, its hidden copy of pm,
// SPWall and its tools (a packet filter, a code injector, a renamed iptables), the stock firmware's
// distribution tag, and the paths the same family writes its downloads and helpers to
constexpr const char* kFiles[] = {
    "/system/bin/aggdll", "/system/bin/net_server", "/system/bin/wifi_init", "/system/bin/wifi_check",
    "/system/bin/libsp.so", "/system/bin/mp", "/system/fonts/AndroidBoot.ttf",
    "/system/bin/wifi_looper", "/system/bin/wifi_pm", "/system/bin/pm_temp", "/system/bin/am_temp",
    "/system/bin/su_zxly", "/system/lib/libsu2update.so", "/system/lib/libsu2update.so.st", "/system/lib/libkey.so",
    "/system/bin/libwp.so", "/system/bin/libld.so", "/system/bin/libip.so", "/system/channel/qbchannel.ini",
};
// the programs among them: a process running one is ended (by its program's path, deleted or not)
constexpr const char* kPrograms[] = {
    "/system/bin/aggdll", "/system/bin/net_server", "/system/bin/wifi_init", "/system/bin/wifi_check",
    "/system/bin/libsp.so", "/system/bin/wifi_looper", "/system/bin/wifi_pm", "/system/bin/su_zxly",
    "/system/bin/libwp.so", "/system/bin/libld.so", "/system/bin/libip.so",
};
// init's services for them, on boot images before 3.0's (tools/firmware/BootImage.java)
constexpr const char* kServices[] = {"aggsrv", "spwall"};

// aggdll puts a setuid-root program of its own in am's place and keeps Android's am script as am2;
// this is that script, byte for byte (Android 4.2's, the stock firmware's)
constexpr char kAmScript[] =
    "# Script to start \"am\" on the device, which has a very rudimentary\n"
    "# shell.\n"
    "#\n"
    "base=/system\n"
    "export CLASSPATH=$base/framework/am.jar\n"
    "exec app_process $base/bin com.android.commands.am.Am \"$@\"\n"
    "\n";
constexpr const char* kAm = "/system/bin/am";
constexpr const char* kAm2 = "/system/bin/am2";
constexpr const char* kPm = "/system/bin/pm";
constexpr gid_t kShellGid = 2000;  // AID_SHELL: Android's /system/bin programs are root:shell, 0755

bool present(const char* path) {
  struct stat st;
  return lstat(path, &st) == 0;
}

// root:shell, 0755, a plain file
bool stockOwnerAndMode(const char* path) {
  struct stat st;
  return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0755 && st.st_uid == 0 && st.st_gid == kShellGid;
}

bool amIsStock() {
  if (!stockOwnerAndMode(kAm)) return false;
  char b[sizeof(kAmScript) + 1];
  int fd = open(kAm, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = read(fd, b, sizeof(b));
  close(fd);
  return n == ssize_t(sizeof(kAmScript) - 1) && memcmp(b, kAmScript, n) == 0;
}

// /system's device, type and whether it is mounted read-only, from /proc/mounts
bool systemMount(std::string& dev, std::string& type, bool& readOnly) {
  FILE* f = fopen("/proc/mounts", "r");
  if (!f) return false;
  char line[512];
  bool found = false;
  while (fgets(line, sizeof(line), f)) {
    char d[200], m[200], t[64], o[200];
    if (sscanf(line, "%199s %199s %63s %199s", d, m, t, o) != 4 || strcmp(m, "/system") != 0) continue;
    dev = d, type = t;
    readOnly = strncmp(o, "ro", 2) == 0 && (o[2] == ',' || o[2] == '\0');
    found = true;  // the last line for /system is the one in effect
  }
  fclose(f);
  return found;
}

std::string base(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

void add(std::string& list, const std::string& item) {
  if (!list.empty()) list += ", ";
  list += item;
}

// init's services stopped (init ends each one's process group: aggdll and the wifi_init it started)
std::string stopServices() {
  std::string did;
  for (const char* s : kServices) {
    char prop[64], v[PROPERTY_VALUE_MAX] = "";
    snprintf(prop, sizeof(prop), "init.svc.%s", s);
    property_get(prop, v, "");
    if (!v[0] || !strcmp(v, "stopped")) continue;
    property_set("ctl.stop", s);
    for (int i = 0; i < 40; i++) {  // 2 s at most
      usleep(50000);
      property_get(prop, v, "");
      if (!strcmp(v, "stopped")) break;
    }
    add(did, s);
  }
  return did;
}

// Whatever still runs one of kPrograms, ended: by /proc/<pid>/exe (" (deleted)" once its file is gone),
// else by its command line's first word
std::string endPrograms() {
  std::string did;
  DIR* d = opendir("/proc");
  if (!d) return did;
  pid_t self = getpid();
  while (dirent* e = readdir(d)) {
    if (!isdigit((unsigned char)e->d_name[0])) continue;
    pid_t pid = atoi(e->d_name);
    if (pid <= 1 || pid == self) continue;
    char p[64], exe[256] = "";
    snprintf(p, sizeof(p), "/proc/%d/exe", pid);
    ssize_t n = readlink(p, exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = '\0';
      if (char* del = strstr(exe, " (deleted)")) *del = '\0';
    } else {
      snprintf(p, sizeof(p), "/proc/%d/cmdline", pid);
      int fd = open(p, O_RDONLY | O_CLOEXEC);
      if (fd < 0) continue;
      n = read(fd, exe, sizeof(exe) - 1);
      close(fd);
      if (n <= 0) continue;
      exe[n] = '\0';  // the first word ends at its zero byte
    }
    for (const char* prog : kPrograms) {
      if (strcmp(exe, prog) != 0) continue;
      if (kill(pid, SIGKILL) == 0) add(did, base(prog) + " (" + std::to_string(pid) + ")");
      break;
    }
  }
  closedir(d);
  return did;
}

// am back as Android's script (written beside it, then renamed over it), with Android's owner and mode
bool restoreAm() {
  std::string tmp = std::string(kAm) + ".rondo";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
  if (fd < 0) return false;
  bool ok = write(fd, kAmScript, sizeof(kAmScript) - 1) == ssize_t(sizeof(kAmScript) - 1);
  ok = fchown(fd, 0, kShellGid) == 0 && ok;
  ok = fchmod(fd, 0755) == 0 && ok;
  ok = fsync(fd) == 0 && ok;
  ok = close(fd) == 0 && ok;
  if (!ok || rename(tmp.c_str(), kAm) != 0) {
    int e = errno;
    unlink(tmp.c_str());
    errno = e;
    return false;
  }
  return true;
}

void* run(void*) {
  prctl(PR_SET_NAME, "rondo-baserom");
  sleep(2);  // off the way to the first frame
  std::string stopped = stopServices();
  std::string ended = endPrograms();

  std::vector<const char*> files;
  for (const char* f : kFiles)
    if (present(f)) files.push_back(f);
  bool am = !amIsStock(), am2 = present(kAm2), pm = present(kPm) && !stockOwnerAndMode(kPm);

  std::string deleted, fixed, problem;
  if (!files.empty() || am || am2 || pm) {
    std::string dev, type;
    bool readOnly = true;
    if (!systemMount(dev, type, readOnly)) {
      problem = "/system isn't in /proc/mounts";
    } else if (readOnly && mount(dev.c_str(), "/system", type.c_str(), MS_REMOUNT, nullptr) != 0) {
      problem = std::string("/system couldn't be remounted read-write (") + strerror(errno) + ")";
    } else {
      for (const char* f : files) {
        if (unlink(f) == 0) add(deleted, f);
        else add(problem, std::string(f) + " stays (" + strerror(errno) + ")");
      }
      if (rmdir("/system/channel") == 0) add(deleted, "/system/channel");  // only once it's empty
      if (am) {
        if (restoreAm()) add(fixed, "am is Android's script again");
        else add(problem, std::string("am couldn't be put back (") + strerror(errno) + ")");
      }
      if (present(kAm2) && amIsStock()) {
        if (unlink(kAm2) == 0) add(deleted, kAm2);
        else add(problem, std::string(kAm2) + " stays (" + strerror(errno) + ")");
      }
      if (pm) {
        if (chown(kPm, 0, kShellGid) == 0 && chmod(kPm, 0755) == 0) add(fixed, "pm's owner and mode are Android's again");
        else add(problem, std::string("pm's owner and mode couldn't be set (") + strerror(errno) + ")");
      }
      sync();
      if (readOnly) {
        bool back = false;
        // the kernel refuses while a file on it is open for writing or deleted and still open: a program
        // just ended holds its own until it has exited
        for (int i = 0; i < 30 && !back; i++) {
          back = mount(dev.c_str(), "/system", type.c_str(), MS_REMOUNT | MS_RDONLY, nullptr) == 0;
          if (!back) usleep(100000);
        }
        if (!back) add(problem, std::string("/system stays read-write (") + strerror(errno) + ")");
      }
    }
    // anything one of them started again meanwhile
    std::string again = endPrograms();
    if (!again.empty()) add(ended, again);
  }

  if (stopped.empty() && ended.empty() && deleted.empty() && fixed.empty() && problem.empty()) return nullptr;
  std::string line;
  auto part = [&](const std::string& s) { line += (line.empty() ? "" : "; ") + s; };
  if (!stopped.empty()) part("stopped init's " + stopped);
  if (!ended.empty()) part("ended " + ended);
  if (!deleted.empty()) part("deleted " + deleted);
  if (!fixed.empty()) part(fixed);
  if (!problem.empty()) part("but " + problem);
  fprintf(stderr, "rondo-core: the stock firmware's backdoors: %s\n", line.c_str());
  return nullptr;
}

}  // namespace

void removeBaseRomBackdoors() {
  pthread_t t;
  if (pthread_create(&t, nullptr, run, nullptr) == 0) pthread_detach(t);
}

}  // namespace rondo
