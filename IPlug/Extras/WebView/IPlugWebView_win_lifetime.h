/*
 ==============================================================================

  MIT License

  Copyright (c) 2026 iPlug2 contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

 ==============================================================================
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <windows.h>

namespace iplug
{
namespace webview_win
{

inline UINT_PTR NextRegistrationId()
{
  // HWND registrations are keyed by callback + ID, not by dwRefData. A
  // process-wide monotonic ID prevents a new editor at a reused address from
  // inheriting an old editor's subclass or timer ownership.
  static std::atomic<UINT_PTR> sNextId { 0x1AA60000 };
  UINT_PTR id = sNextId.fetch_add(1, std::memory_order_relaxed);
  if (id == 0)
    id = sNextId.fetch_add(1, std::memory_order_relaxed);
  return id;
}

struct RegistrationIds
{
  RegistrationIds()
    : aspectRatioSubclass(NextRegistrationId())
    , parentWatchSubclass(NextRegistrationId())
    , frameSnapTimer(NextRegistrationId())
    , openSettleTimer(NextRegistrationId())
  {
  }

  bool AreUnique() const
  {
    return aspectRatioSubclass != 0 && parentWatchSubclass != 0 &&
           frameSnapTimer != 0 && openSettleTimer != 0 &&
           aspectRatioSubclass != parentWatchSubclass &&
           aspectRatioSubclass != frameSnapTimer &&
           aspectRatioSubclass != openSettleTimer &&
           parentWatchSubclass != frameSnapTimer &&
           parentWatchSubclass != openSettleTimer &&
           frameSnapTimer != openSettleTimer;
  }

  const UINT_PTR aspectRatioSubclass;
  const UINT_PTR parentWatchSubclass;
  const UINT_PTR frameSnapTimer;
  const UINT_PTR openSettleTimer;
};

// This state deliberately outlives IWebViewImpl when a COM completion or a
// Win32 subclass callback is still queued. Callers hold mutex from validation
// through their final owner access; invalidation/destruction takes the same
// mutex, closing the "checked alive, then raw pointer was freed" race.
struct LifetimeState
{
  uint64_t BeginGenerationLocked(void* newOwner, HWND newParent)
  {
    owner = newOwner;
    parent = newParent;
    return ++generation;
  }

  uint64_t InvalidateGenerationLocked()
  {
    parent = NULL;
    return ++generation;
  }

  bool MatchesLocked(uint64_t expectedGeneration, HWND expectedParent) const
  {
    return owner != nullptr && generation == expectedGeneration &&
           parent == expectedParent;
  }

  bool MarkDiscardLoggedLocked(uint64_t discardedGeneration)
  {
    if (lastLoggedDiscardedGeneration == discardedGeneration)
      return false;
    lastLoggedDiscardedGeneration = discardedGeneration;
    return true;
  }

  std::recursive_mutex mutex;
  void* owner = nullptr;
  uint64_t generation = 0;
  HWND parent = NULL;
  uint64_t lastLoggedDiscardedGeneration = static_cast<uint64_t>(-1);
};

} // namespace webview_win
} // namespace iplug
