// Work handed to the UI thread by other threads (the dashboard's requests), run by the main loop: what
// 1.0.x's Handler(Looper.getMainLooper()).post and the dashboard's onMain did.
#pragma once
#include <stdint.h>

#include <functional>

namespace rondo::shell {

void mainQueueInit();  // on the UI thread, before anything is posted
void postToMain(std::function<void()> work, int64_t delayMs = 0);
int mainQueueFd();         // readable when work is waiting (for the main loop's poll)
int64_t mainQueueWakeAt();  // CLOCK_MONOTONIC ns of the next delayed work, 0 for none
bool runMainQueue();       // on the UI thread: runs the work that is due; true when anything ran
// Runs work on the UI thread and waits for it, at most timeoutMs: false when it didn't finish in time
// (it may still run later). On the UI thread itself it runs at once.
bool onMain(std::function<void()> work, int timeoutMs = 5000);

}  // namespace rondo::shell
