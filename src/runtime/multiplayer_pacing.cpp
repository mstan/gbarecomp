#include "multiplayer_pacing.h"
#include <thread>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gbarecomp {
GbaNetplayPacer::~GbaNetplayPacer() {
#if defined(_WIN32)
    if (wake_timer_) CloseHandle(static_cast<HANDLE>(wake_timer_));
#endif
}
void GbaNetplayPacer::idle() {
#if defined(_WIN32)
    if (!tried_timer_) {
        tried_timer_=true;
        // Available on Windows 10 1803+. Failure uses the ordinary portable
        // sleep below, rather than changing process/system timer policy.
        wake_timer_=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
    }
    if (wake_timer_) {
        LARGE_INTEGER due; due.QuadPart=-10000; // relative 1 ms in 100 ns units
        if (SetWaitableTimer(static_cast<HANDLE>(wake_timer_),&due,0,nullptr,nullptr,FALSE)) {
            WaitForSingleObject(static_cast<HANDLE>(wake_timer_),100);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}
