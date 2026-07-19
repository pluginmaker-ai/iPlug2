#include "../IPlug/Extras/WebView/IPlugWebView_win_lifetime.h"

#include <cassert>
#include <commctrl.h>
#include <future>
#include <iostream>
#include <thread>

using namespace iplug::webview_win;

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
LRESULT CALLBACK TestSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                  LPARAM lParam, UINT_PTR, DWORD_PTR)
{
  return DefSubclassProc(hwnd, message, wParam, lParam);
}
}

int main()
{
  RegistrationIds first;
  RegistrationIds second;

  assert(first.AreUnique());
  assert(second.AreUnique());
  assert(first.aspectRatioSubclass != second.aspectRatioSubclass);
  assert(first.parentWatchSubclass != second.parentWatchSubclass);
  assert(first.frameSnapTimer != second.frameSnapTimer);
  assert(first.openSettleTimer != second.openSettleTimer);

  // SetWindowSubclass keys registrations by HWND + callback + ID. Distinct
  // instance IDs must preserve independent refData and removing one instance
  // must leave the other registration intact.
  HWND window = CreateWindowExW(0, L"STATIC", L"IPlugWebViewLifetimeTest",
                                WS_OVERLAPPED, 0, 0, 1, 1,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  assert(window != nullptr);
  const DWORD_PTR firstRefData = 0x1111;
  const DWORD_PTR secondRefData = 0x2222;
  assert(SetWindowSubclass(window, TestSubclassProc, first.aspectRatioSubclass, firstRefData));
  assert(SetWindowSubclass(window, TestSubclassProc, second.aspectRatioSubclass, secondRefData));

  DWORD_PTR observedRefData = 0;
  assert(GetWindowSubclass(window, TestSubclassProc, first.aspectRatioSubclass, &observedRefData));
  assert(observedRefData == firstRefData);
  assert(GetWindowSubclass(window, TestSubclassProc, second.aspectRatioSubclass, &observedRefData));
  assert(observedRefData == secondRefData);
  assert(RemoveWindowSubclass(window, TestSubclassProc, first.aspectRatioSubclass));
  assert(!GetWindowSubclass(window, TestSubclassProc, first.aspectRatioSubclass, &observedRefData));
  assert(GetWindowSubclass(window, TestSubclassProc, second.aspectRatioSubclass, &observedRefData));
  assert(observedRefData == secondRefData);
  assert(RemoveWindowSubclass(window, TestSubclassProc, second.aspectRatioSubclass));
  DestroyWindow(window);

  LifetimeState lifetime;
  const HWND parent = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
  const HWND otherParent = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x5678));
  int owner = 0;

  {
    std::lock_guard<std::recursive_mutex> lock(lifetime.mutex);
    const uint64_t firstGeneration = lifetime.BeginGenerationLocked(&owner, parent);
    assert(lifetime.MatchesLocked(firstGeneration, parent));
    assert(!lifetime.MatchesLocked(firstGeneration, otherParent));

    lifetime.InvalidateGenerationLocked();
    assert(!lifetime.MatchesLocked(firstGeneration, parent));
    assert(lifetime.MarkDiscardLoggedLocked(firstGeneration));
    assert(!lifetime.MarkDiscardLoggedLocked(firstGeneration));

    const uint64_t secondGeneration = lifetime.BeginGenerationLocked(&owner, parent);
    assert(secondGeneration != firstGeneration);
    assert(lifetime.MatchesLocked(secondGeneration, parent));
    assert(!lifetime.MatchesLocked(firstGeneration, parent));
    assert(lifetime.MarkDiscardLoggedLocked(secondGeneration));
  }

  // A callback keeps the lifetime mutex locked from validation through its
  // final owner access. Destruction/invalidation on another thread therefore
  // cannot slip between the alive check and use of the raw owner pointer.
  LifetimeState synchronizedLifetime;
  uint64_t synchronizedGeneration = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(synchronizedLifetime.mutex);
    synchronizedGeneration = synchronizedLifetime.BeginGenerationLocked(&owner, parent);
  }
  std::promise<void> callbackHasLock;
  std::future<void> callbackHasLockFuture = callbackHasLock.get_future();
  std::promise<void> releaseCallback;
  std::shared_future<void> releaseCallbackFuture = releaseCallback.get_future().share();
  std::thread callbackThread([&synchronizedLifetime, synchronizedGeneration,
                              parent, &owner, &callbackHasLock,
                              releaseCallbackFuture]() {
    std::lock_guard<std::recursive_mutex> lock(synchronizedLifetime.mutex);
    assert(synchronizedLifetime.MatchesLocked(synchronizedGeneration, parent));
    callbackHasLock.set_value();
    releaseCallbackFuture.wait();
    assert(synchronizedLifetime.owner == &owner);
  });
  callbackHasLockFuture.wait();
  const bool invalidatorCouldEnter = synchronizedLifetime.mutex.try_lock();
  if (invalidatorCouldEnter)
    synchronizedLifetime.mutex.unlock();
  assert(!invalidatorCouldEnter);
  releaseCallback.set_value();
  callbackThread.join();
  {
    std::lock_guard<std::recursive_mutex> lock(synchronizedLifetime.mutex);
    synchronizedLifetime.InvalidateGenerationLocked();
    assert(!synchronizedLifetime.MatchesLocked(synchronizedGeneration, parent));
  }

  std::cout << "IPlugWebViewWinLifetimeTest passed\n";
  return 0;
}
