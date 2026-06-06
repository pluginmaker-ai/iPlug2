/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#include "IPlugSentryAudioDetectors.h"

#ifdef IPLUG_USE_SENTRY

  #include <atomic>
  #include <chrono>
  #include <cmath>
  #include <cstdint>

  #if defined(__APPLE__)
    #include <mach/mach_time.h>
  #elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
      #define NOMINMAX
    #endif
    #include <windows.h>
  #endif

  #include "IPlugSentryEventRing.h"

namespace iplug
{
namespace sentry
{
namespace audiodetectors
{

// Mirror the watchdog's slot-count ceiling so the detector's parallel
// state table can be indexed by the same SlotHandle::idx without ever
// going out of range. If the watchdog grows its table the detector
// tracks it automatically (-D override flows through both).
#ifndef IPLUG_WATCHDOG_SLOT_COUNT
  #define IPLUG_WATCHDOG_SLOT_COUNT 128
#endif

namespace
{
  constexpr size_t kSlotCount = IPLUG_WATCHDOG_SLOT_COUNT;

  // Hysteresis: the BLOCK_CPU_OVER_80 latch only clears once we observe
  // this many consecutive blocks at <=80% load.
  constexpr int kRecoveryClearBlocks = 8;

  constexpr float kLoadThreshold = 0.8f;

  // Cap the NaN/Inf fast-path scan at 32 channels so its per-block
  // budget is fixed regardless of host channel count.
  constexpr int32_t kFastScanMaxChannels = 32;

  // Per-instance detector state. All fields touched ONLY by the audio
  // thread that owns the slot — relaxed atomics suffice because there
  // is no cross-thread reader (the drain thread reads the event ring,
  // not this struct).
  struct DetectorSlot
  {
    std::atomic<uint64_t> blockStartNs{0};
    std::atomic<bool>     cpuOverLatched{false};
    std::atomic<int>      cleanBlockRun{0};
    std::atomic<uint16_t> beginGen{0};
  };

  DetectorSlot gDetectorSlots[kSlotCount];

  // Use the public eventring::NowNs so probe-seam + detectors record on the
  // same clock source. Single source of truth — no monotonic-epoch drift.
  inline uint64_t NowNs() noexcept
  {
    return iplug::sentry::eventring::NowNs();
  }

  bool HandleResolves(SlotHandle handle, DetectorSlot*& outSlot) noexcept
  {
    if (handle.idx == 0) return false;
    if (handle.idx > (uint16_t) kSlotCount) return false;
    outSlot = &gDetectorSlots[handle.idx - 1];
    return true;
  }

  void EmitBlockCpuOverEvent(float loadPct) noexcept
  {
    using namespace eventring;
    EventRecord rec{};
    rec.timestampNs = NowNs();
    rec.kind        = (uint16_t) EVENT_KIND_BLOCK_CPU_OVER_80;
    rec.channel     = -1;
    rec.value       = loadPct;
    rec.stageId     = 0;
    rec.reserved0   = 0;
    rec.reserved1   = 0;
    (void) Push(rec);
  }

  void EmitNanInfEvent(eventring::EventKind kind,
                       int16_t channel,
                       float value) noexcept
  {
    using namespace eventring;
    EventRecord rec{};
    rec.timestampNs = NowNs();
    rec.kind        = (uint16_t) kind;
    rec.channel     = channel;
    rec.value       = value;
    rec.stageId     = 0;
    rec.reserved0   = 0;
    rec.reserved1   = 0;
    (void) Push(rec);
  }

  template <typename SampleT>
  bool FastScanHitsNonFinite(const SampleT* const* pOutputs,
                             int32_t nChannels, int32_t nFrames) noexcept
  {
    if (!pOutputs || nChannels <= 0 || nFrames <= 0) return false;
    const int32_t chCap = nChannels < kFastScanMaxChannels
                            ? nChannels : kFastScanMaxChannels;
    const int32_t mid = nFrames / 2;
    const int32_t last = nFrames - 1;
    for (int32_t c = 0; c < chCap; ++c)
    {
      const SampleT* ch = pOutputs[c];
      if (!ch) continue;
      if (!std::isfinite(ch[0])) return true;
      if (!std::isfinite(ch[mid])) return true;
      if (!std::isfinite(ch[last])) return true;
    }
    return false;
  }

  template <typename SampleT>
  void EmitFirstNonFinite(const SampleT* const* pOutputs,
                          int32_t nChannels, int32_t nFrames) noexcept
  {
    // Cap channels at the same ceiling the fast scan uses so a 64-channel
    // instrument with a NaN can't blow the per-block budget. The fast scan
    // would only have detected a NaN inside this range anyway — anything
    // past kFastScanMaxChannels was outside the audited window.
    const int32_t chCap = nChannels < kFastScanMaxChannels
                            ? nChannels : kFastScanMaxChannels;
    for (int32_t c = 0; c < chCap; ++c)
    {
      const SampleT* ch = pOutputs[c];
      if (!ch) continue;
      for (int32_t f = 0; f < nFrames; ++f)
      {
        const SampleT v = ch[f];
        if (std::isnan(v))
        {
          EmitNanInfEvent(eventring::EVENT_KIND_NAN_OUTPUT,
                          (int16_t)(c > 32767 ? 32767 : c),
                          1.0f);
          return;
        }
        if (std::isinf(v))
        {
          EmitNanInfEvent(eventring::EVENT_KIND_INF_OUTPUT,
                          (int16_t)(c > 32767 ? 32767 : c),
                          (float)((double) v > 0 ? 1.0 : -1.0));
          return;
        }
      }
    }
  }

  void RunCpuEdgeDetector(DetectorSlot& slot,
                          uint64_t elapsedNs,
                          int32_t blockSize,
                          double sampleRate) noexcept
  {
    if (blockSize <= 0 || sampleRate <= 0.0) return;

    const double budgetNs = ((double) blockSize * 1.0e9) / sampleRate;
    if (budgetNs <= 0.0) return;
    const float loadPct = (float)((double) elapsedNs / budgetNs);

    const bool over = loadPct > kLoadThreshold;
    const bool wasLatched = slot.cpuOverLatched.load(std::memory_order_relaxed);

    if (over)
    {
      slot.cleanBlockRun.store(0, std::memory_order_relaxed);
      if (!wasLatched)
      {
        slot.cpuOverLatched.store(true, std::memory_order_relaxed);
        EmitBlockCpuOverEvent(loadPct);
      }
      return;
    }

    if (!wasLatched) return;
    const int run = slot.cleanBlockRun.load(std::memory_order_relaxed) + 1;
    if (run >= kRecoveryClearBlocks)
    {
      slot.cpuOverLatched.store(false, std::memory_order_relaxed);
      slot.cleanBlockRun.store(0, std::memory_order_relaxed);
    }
    else
    {
      slot.cleanBlockRun.store(run, std::memory_order_relaxed);
    }
  }

  template <typename SampleT>
  void ProcessBlockEndImpl(SlotHandle handle,
                           const SampleT* const* pOutputs,
                           int32_t nChannels,
                           int32_t nFrames,
                           int32_t blockSize,
                           double sampleRate) noexcept
  {
    DetectorSlot* slotPtr = nullptr;
    if (!HandleResolves(handle, slotPtr)) return;
    DetectorSlot& slot = *slotPtr;

    const uint16_t beginGen = slot.beginGen.load(std::memory_order_relaxed);
    if (beginGen != handle.gen) return;

    const uint64_t startNs = slot.blockStartNs.load(std::memory_order_relaxed);
    if (startNs != 0)
    {
      const uint64_t endNs = NowNs();
      const uint64_t elapsedNs = (endNs > startNs) ? (endNs - startNs) : 0;
      RunCpuEdgeDetector(slot, elapsedNs, blockSize, sampleRate);
      slot.blockStartNs.store(0, std::memory_order_relaxed);
    }

    if (pOutputs && nChannels > 0 && nFrames > 0)
    {
      if (FastScanHitsNonFinite(pOutputs, nChannels, nFrames))
        EmitFirstNonFinite(pOutputs, nChannels, nFrames);
    }
  }
} // anonymous namespace

void ProcessBlockBegin(SlotHandle handle) noexcept
{
  DetectorSlot* slotPtr = nullptr;
  if (!HandleResolves(handle, slotPtr)) return;
  DetectorSlot& slot = *slotPtr;

  // If the slot's last-known generation differs from the incoming handle,
  // a new IPlugProcessor instance has reclaimed the slot since the
  // previous tick. Wipe latch + recovery so the new owner doesn't
  // inherit a stuck cpuOverLatched=true from the prior tenant.
  const uint16_t prevGen = slot.beginGen.load(std::memory_order_relaxed);
  if (prevGen != handle.gen)
  {
    slot.cpuOverLatched.store(false, std::memory_order_relaxed);
    slot.cleanBlockRun.store(0, std::memory_order_relaxed);
  }
  slot.beginGen.store(handle.gen, std::memory_order_relaxed);
  slot.blockStartNs.store(NowNs(), std::memory_order_relaxed);
}

void ProcessBlockEnd(SlotHandle handle,
                     const float* const* pOutputs,
                     int32_t nChannels,
                     int32_t nFrames,
                     int32_t blockSize,
                     double sampleRate) noexcept
{
  ProcessBlockEndImpl<float>(handle, pOutputs, nChannels, nFrames,
                             blockSize, sampleRate);
}

void ProcessBlockEnd(SlotHandle handle,
                     const double* const* pOutputs,
                     int32_t nChannels,
                     int32_t nFrames,
                     int32_t blockSize,
                     double sampleRate) noexcept
{
  ProcessBlockEndImpl<double>(handle, pOutputs, nChannels, nFrames,
                              blockSize, sampleRate);
}

} // namespace audiodetectors
} // namespace sentry
} // namespace iplug

#else // !IPLUG_USE_SENTRY

// OFF build: header provides inline no-op definitions. This TU is empty
// but kept in the CMake target so the source list is symmetric across
// OFF/ON configurations.

#endif // IPLUG_USE_SENTRY
