/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
 */

/**
 * @file
 * @brief IPlugProcessor implementation.
 */

#include "IPlugProcessor.h"

#ifdef OS_WIN
#define strtok_r strtok_s
#endif

// A-5 watchdog. Include guard is gated on IPLUG_USE_SENTRY so the OFF build
// literally never sees the header — the byte-identical guarantee becomes
// load-bearing on this single include directive rather than on each scattered
// call-site #ifdef (the call sites are still gated as belt-and-braces).
#ifdef IPLUG_USE_SENTRY
  #include <cstdint>
  #include <cstring>
  #include "Extras/Sentry/IPlugSentryWatchdog.h"
#endif

using namespace iplug;

#ifdef IPLUG_USE_SENTRY
namespace
{
  // Pack/unpack a watchdog::SlotHandle into the void* mWatchdogSlot member.
  // SlotHandle is 32 bits (two uint16_t) so it fits comfortably in the low
  // half of any 64-bit pointer slot. memcpy dodges strict-aliasing concerns;
  // the encoded value is opaque to anyone other than the watchdog itself.
  static_assert(sizeof(iplug::sentry::watchdog::SlotHandle) <= sizeof(void*),
                "SlotHandle must fit inside void*");
  static_assert(sizeof(iplug::sentry::watchdog::SlotHandle) == 4,
                "SlotHandle layout assumed to be {uint16_t,uint16_t}");

  void* PackSlotHandle(iplug::sentry::watchdog::SlotHandle h)
  {
    uintptr_t bits = 0;
    std::memcpy(&bits, &h, sizeof(h));
    return reinterpret_cast<void*>(bits);
  }

  iplug::sentry::watchdog::SlotHandle UnpackSlotHandle(void* p)
  {
    iplug::sentry::watchdog::SlotHandle h{0, 0};
    if (!p) return h;
    const uintptr_t bits = reinterpret_cast<uintptr_t>(p);
    std::memcpy(&h, &bits, sizeof(h));
    return h;
  }
}
#endif // IPLUG_USE_SENTRY

IPlugProcessor::IPlugProcessor(const Config& config, EAPI plugAPI)
: mPlugType((EIPlugPluginType) config.plugType)
, mDoesMIDIIn(config.plugDoesMidiIn)
, mDoesMIDIOut(config.plugDoesMidiOut)
, mDoesMPE(config.plugDoesMPE)
, mLatency(config.latency)
{
  int totalNInBuses, totalNOutBuses;
  int totalNInChans, totalNOutChans;

  ParseChannelIOStr(config.channelIOStr, mIOConfigs, totalNInChans, totalNOutChans, totalNInBuses, totalNOutBuses);

  mScratchData[ERoute::kInput].Resize(totalNInChans);
  mScratchData[ERoute::kOutput].Resize(totalNOutChans);

  sample** ppInData = mScratchData[ERoute::kInput].Get();

  for (auto i = 0; i < totalNInChans; ++i, ++ppInData)
  {
    IChannelData<>* pInChannel = new IChannelData<>;
    pInChannel->mConnected = false;
    pInChannel->mData = ppInData;
    mChannelData[ERoute::kInput].Add(pInChannel);
  }

  sample** ppOutData = mScratchData[ERoute::kOutput].Get();

  for (auto i = 0; i < totalNOutChans; ++i, ++ppOutData)
  {
    IChannelData<>* pOutChannel = new IChannelData<>;
    pOutChannel->mConnected = false;
    pOutChannel->mData = ppOutData;
    pOutChannel->mIncomingData = nullptr;
    mChannelData[ERoute::kOutput].Add(pOutChannel);
  }

#ifdef IPLUG_USE_SENTRY
  // A-5 — claim a heartbeat slot for this instance. Register runs on the
  // host thread (slow path; mutex inside the watchdog). A zero-handle return
  // (table full) is silently tolerated: Tick on a zero handle is a no-op, so
  // the plugin still runs without watchdog coverage on the overflow instance.
  mWatchdogSlot = PackSlotHandle(iplug::sentry::watchdog::Register());
#endif
}

IPlugProcessor::~IPlugProcessor()
{
  TRACE

#ifdef IPLUG_USE_SENTRY
  // Symmetric with the ctor. Unregister is host-thread + slow-path; it
  // bumps the slot's generation counter so any in-flight Tick from a
  // racing audio thread fails its gen cross-check and becomes a no-op.
  iplug::sentry::watchdog::Unregister(UnpackSlotHandle(mWatchdogSlot));
  mWatchdogSlot = nullptr;
#endif

  mChannelData[ERoute::kInput].Empty(true);
  mChannelData[ERoute::kOutput].Empty(true);
  mIOConfigs.Empty(true);
}

void IPlugProcessor::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  const int nIn = mChannelData[ERoute::kInput].GetSize();
  const int nOut = mChannelData[ERoute::kOutput].GetSize();

  int j = 0;
  for (int i = 0; i < nOut; ++i)
  {
    if (i < nIn)
    {
      memcpy(outputs[i], inputs[i], nFrames * sizeof(sample));
      j++;
    }
  }
  // zero remaining outs
  for (/* same j */; j < nOut; ++j)
  {
    memset(outputs[j], 0, nFrames * sizeof(sample));
  }
}

void IPlugProcessor::ProcessMidiMsg(const IMidiMsg& msg)
{
  SendMidiMsg(msg);
}

bool IPlugProcessor::SendMidiMsgs(WDL_TypedBuf<IMidiMsg>& msgs)
{
  bool rc = true;
  int n = msgs.GetSize();

  for (auto i = 0; i < n; ++i)
    rc &= SendMidiMsg(msgs.Get()[i]);

  return rc;
}

double IPlugProcessor::GetSamplesPerBeat() const
{
  const double tempo = GetTempo();

  if (tempo > 0.0)
    return GetSampleRate() * 60.0 / tempo;

  return 0.0;
}

#pragma mark -

void IPlugProcessor::GetBusName(ERoute direction, int busIdx, int nBuses, WDL_String& str) const
{
  if(direction == ERoute::kInput)
  {
    if(nBuses == 1)
    {
      str.Set("Input");
    }
    else if(nBuses == 2)
    {
      if(busIdx == 0)
        str.Set("Main Input");
      else
        str.Set("Aux Input");
    }
    else
    {
      str.SetFormatted(MAX_BUS_NAME_LEN, "Input %i", busIdx + 1);
    }
  }
  else
  {
    if(nBuses == 1)
    {
      str.Set("Output");
    }
    else
    {
      str.SetFormatted(MAX_BUS_NAME_LEN, "Output %i", busIdx + 1);
    }
  }
}

int IPlugProcessor::GetIOConfigWithChanCounts(std::vector<int>& inputBuses, std::vector<int>& outputBuses)
{
  for (auto configIdx = 0; configIdx < NIOConfigs(); configIdx++)
  {
    const IOConfig* pConfig = GetIOConfig(configIdx);
    int configNInputBuses = pConfig->NBuses(ERoute::kInput);
    int configNOutputBuses = pConfig->NBuses(ERoute::kOutput);
    
    if (configNInputBuses == static_cast<int>(inputBuses.size()) &&
        configNOutputBuses == static_cast<int>(outputBuses.size()))
    {
      bool match = true;
      for (auto inputBusIdx = 0; inputBusIdx < configNInputBuses && match; inputBusIdx++)
      {
        if (pConfig->NChansOnBusSAFE(ERoute::kInput, inputBusIdx) != inputBuses[inputBusIdx])
          match = false;
      }
      for (auto outputBusIdx = 0; outputBusIdx < configNOutputBuses && match; outputBusIdx++)
      {
        if (pConfig->NChansOnBusSAFE(ERoute::kOutput, outputBusIdx) != outputBuses[outputBusIdx])
          match = false;
      }
      if (match)
        return configIdx;
    }
  }
  return -1;
}

int IPlugProcessor::MaxNBuses(ERoute direction, int* pConfigIdxWithTheMostBuses) const
{
  int maxNBuses = 0;
  int configWithMostBuses = 0;
  int maxChans = 0;
  
  for (auto i = 0; i < NIOConfigs(); i++)
  {
    auto thisIOConfigNBuses = GetIOConfig(i)->NBuses(direction);
    if(thisIOConfigNBuses >= maxNBuses)
    {
      maxNBuses = thisIOConfigNBuses;
      
      auto thisIOConfigNChans = GetIOConfig(i)->GetTotalNChannels(direction);
      
      if(thisIOConfigNChans > maxChans)
      {
        maxChans = thisIOConfigNChans;
        configWithMostBuses = i;
      }
    }
  }
  
  if(pConfigIdxWithTheMostBuses)
    *pConfigIdxWithTheMostBuses = configWithMostBuses;

  return maxNBuses;
}

int IPlugProcessor::MaxNChannelsForBus(ERoute direction, int busIdx) const
{
  const int maxNBuses = MaxNBuses(direction);
  std::vector<int> maxChansOnBuses;
  maxChansOnBuses.resize(maxNBuses);

  // find the maximum channel count for each bus
  for (auto configIdx = 0; configIdx < NIOConfigs(); configIdx++)
  {
    const IOConfig* pIOConfig = GetIOConfig(configIdx);
    
    for (auto bus = 0; bus < maxNBuses; bus++)
      maxChansOnBuses[bus] = std::max(pIOConfig->NChansOnBusSAFE(direction, bus), maxChansOnBuses[bus]);
  }

  return maxChansOnBuses.size() > 0 ? maxChansOnBuses[busIdx] : 0;
}

int IPlugProcessor::NChannelsConnected(ERoute direction) const
{
  const WDL_PtrList<IChannelData<>>& channelData = mChannelData[direction];

  int count = 0;
  for (auto i = 0; i < channelData.GetSize(); i++)
  {
    count += (int) IsChannelConnected(direction, i);
  }

  return count;
}

bool IPlugProcessor::LegalIO(int NInputChans, int NOutputChans) const
{
  bool legal = false;

  for (auto i = 0; i < NIOConfigs() && !legal; ++i)
  {
    const IOConfig* pIO = GetIOConfig(i);
    legal = ((NInputChans < 0 || NInputChans == pIO->GetTotalNChannels(ERoute::kInput)) && (NOutputChans < 0 || NOutputChans == pIO->GetTotalNChannels(ERoute::kOutput)));
  }

  return legal;
}

void IPlugProcessor::LimitToStereoIO()
{
  if (MaxNChannels(ERoute::kInput) > 2)
    SetChannelConnections(ERoute::kInput, 2, MaxNChannels(ERoute::kInput) - 2, false);

  if (MaxNChannels(ERoute::kOutput) > 2)
    SetChannelConnections(ERoute::kOutput, 2, MaxNChannels(ERoute::kOutput) - 2, false);
}

void IPlugProcessor::SetChannelLabel(ERoute direction, int idx, const char* formatStr, bool zeroBased)
{
  if (idx >= 0 && idx < MaxNChannels(direction))
    mChannelData[direction].Get(idx)->mLabel.SetFormatted(MAX_CHAN_NAME_LEN, formatStr, idx+(zeroBased?0:1));
}

void IPlugProcessor::SetLatency(int samples)
{
  mLatency = samples;

  if (mLatencyDelay)
    mLatencyDelay->SetDelayTime(samples);
}

// static
int IPlugProcessor::ParseChannelIOStr(const char* IOStr, WDL_PtrList<IOConfig>& channelIOList, int& totalNInChans, int& totalNOutChans, int& totalNInBuses, int& totalNOutBuses)
{
  bool foundAWildcard = false;
  int IOConfigIndex = 0;
  
  DBGMSG("Channel I/O string: \"%s\"\n", IOStr);

  char* IOStrlocal = strdup(IOStr);
  char* pChannelIOStr = strtok_r(IOStrlocal, " ", &IOStrlocal);

  while (pChannelIOStr)
  {
    int NInChans = 0, NOutChans = 0;
    int NInBuses = 0, NOutBuses = 0;

    // Build a temporary IOConfig, in case the string is invalid
    IOConfig* pConfig = new IOConfig();

    char* pIOStr = strtok_r(pChannelIOStr, "-", &pChannelIOStr); // Don't write directly into IOStr, since strtok will modify it

    while (pIOStr != nullptr)
    {
      WDL_String tempStr(pIOStr);
      ERoute dir = (ERoute) (NInBuses > 0); // 2nd time around the loop, this will be the output
      
      if (CStringHasContents(tempStr.Get()))
      {
        char* pBusStr = strtok_r(tempStr.Get(), ".", &(pIOStr));

        while (pBusStr != nullptr)
        {
          int NChans = 0;
          
          if (strcmp(pBusStr, "*") == 0)
          {
            foundAWildcard = true;
            NChans = MAX_BUS_CHANS; // for wildcard bus pretend MAX_BUS_CHANS channels are present
          }
          else
          {
            NChans = atoi(pBusStr);
          }
          
          pConfig->AddBusInfo(dir, NChans);

          if(dir == ERoute::kInput)
          {
            NInChans += NChans;
            NInBuses++;
          }
          else
          {
            NOutChans += NChans;
            NOutBuses++;
          }
          
          pBusStr = strtok_r(nullptr, ".", &(pIOStr));
        }
      }
      
      pIOStr = strtok_r(pChannelIOStr, "-", &pChannelIOStr); // Don't write directly into IOStr, since strtok will modify it
    }

    channelIOList.Add(pConfig);

    DBGMSG("Channel I/O #%i - input bus count: %i, output bus count %i\n", IOConfigIndex + 1, NInBuses, NOutBuses);
    DBGMSG("Channel I/O #%i - input channel count: %i, output channel count %i\n\n", IOConfigIndex + 1, NInChans, NOutChans);

    totalNInChans = std::max(totalNInChans, NInChans);
    totalNOutChans = std::max(totalNOutChans, NOutChans);
    totalNInBuses = std::max(totalNInBuses, NInBuses);
    totalNOutBuses = std::max(totalNOutBuses, NOutBuses);

    IOConfigIndex++;

    pChannelIOStr = strtok_r(nullptr, " ", &IOStrlocal);
  }

  free(IOStrlocal);
  
  assert(foundAWildcard == false || (foundAWildcard == true && IOConfigIndex == 1)); // an association of a wildcard channel I/O config with others is incorrect
  
  return IOConfigIndex;
}

int IPlugProcessor::GetAUPluginType() const
{
  if (mPlugType == EIPlugPluginType::kEffect)
  {
    if (DoesMIDIIn())
      return 'aumf';
    else
      return 'aufx';
  }
  else if (mPlugType == EIPlugPluginType::kInstrument)
  {
    return 'aumu';
  }
  else if (mPlugType == EIPlugPluginType::kMIDIEffect)
  {
    return 'aumi';
  }
  else
    return 'aufx';
}

void IPlugProcessor::InitLatencyDelay()
{
  if (mLatency && mLatencyDelay == nullptr)
  {
    mLatencyDelay = std::make_unique<NChanDelayLine<sample>>(MaxNChannels(ERoute::kInput), MaxNChannels(ERoute::kOutput));
    mLatencyDelay->SetDelayTime(mLatency);
  }
}

void IPlugProcessor::SetChannelConnections(ERoute direction, int idx, int n, bool connected)
{
  WDL_PtrList<IChannelData<>>& channelData = mChannelData[direction];
  
  for (auto i = idx; i < idx + n; ++i)
  {
    if (i < channelData.GetSize())
    {
      IChannelData<>* pChannel = channelData.Get(i);
      pChannel->mConnected = connected;
      
      if (!connected)
        *(pChannel->mData) = pChannel->mScratchBuf.Get();
    }
  }
}

void IPlugProcessor::AttachBuffers(ERoute direction, int idx, int n, PLUG_SAMPLE_DST** ppData, int nFrames)
{
  WDL_PtrList<IChannelData<>>& channelData = mChannelData[direction];
  
  for (auto i = idx; i < idx + n; ++i)
  {
    IChannelData<>* pChannel = channelData.Get(i);
    if (pChannel->mConnected)
      *(pChannel->mData) = *(ppData++);
  }
}

void IPlugProcessor::AttachBuffers(ERoute direction, int idx, int n, PLUG_SAMPLE_SRC** ppData, int nFrames)
{
  WDL_PtrList<IChannelData<>>& channelData = mChannelData[direction];
  
  for (auto i = idx; i < idx + n; ++i)
  {
    IChannelData<>* pChannel = channelData.Get(i);
    if (pChannel->mConnected)
    {
      if (sizeof(PLUG_SAMPLE_DST) == 8 && sizeof(PLUG_SAMPLE_SRC) == 8)
        *(pChannel->mData) = (PLUG_SAMPLE_DST*) *ppData;
        
      pChannel->mIncomingData = *ppData;
      ppData++;
    }
  }
}

void IPlugProcessor::PassThroughBuffers(PLUG_SAMPLE_DST type, int nFrames)
{
  if (mLatencyDelay)
    mLatencyDelay->ProcessBlock(mScratchData[ERoute::kInput].Get(), mScratchData[ERoute::kOutput].Get(), nFrames);
  else
    IPlugProcessor::ProcessBlock(mScratchData[ERoute::kInput].Get(), mScratchData[ERoute::kOutput].Get(), nFrames);
}

void IPlugProcessor::PassThroughBuffers(PLUG_SAMPLE_SRC type, int nFrames)
{
  // for PLUG_SAMPLE_SRC bit buffers, first run the delay (if mLatency) on the PLUG_SAMPLE_DST IPlug buffers
  PassThroughBuffers(PLUG_SAMPLE_DST(0.), nFrames);

  int i, n = MaxNChannels(ERoute::kOutput);
  IChannelData<>** ppOutChannel = mChannelData[ERoute::kOutput].GetList();

  for (i = 0; i < n; ++i, ++ppOutChannel)
  {
    IChannelData<>* pOutChannel = *ppOutChannel;
    if (pOutChannel->mConnected)
    {
      CastCopy(pOutChannel->mIncomingData, *(pOutChannel->mData), nFrames);
    }
  }
}

void IPlugProcessor::ProcessBuffers(PLUG_SAMPLE_DST type, int nFrames)
{
#ifdef IPLUG_USE_SENTRY
  // A-5 watchdog tick. Real-time-safe: one relaxed load+compare (gen check
  // inside Tick), one fetch_add, two relaxed stores, one release store.
  // No allocations, no locks, no syscalls. Inlines to nothing in the OFF
  // build (header gives an inline empty stub, but the #ifdef guard means
  // we don't even reach the include there). On a zero handle (table full
  // at registration time) the call returns after a single compare-against-
  // zero, so the overflow case is also wait-free.
  //
  // Snapshot the three host-thread-written values into locals for the call
  // — the compiler likely already does this, but make it explicit so
  // there is no question about which value the watchdog observes.
  const int    snapBlockSize = mBlockSize;
  const double snapSampleRate = mSampleRate;
  const bool   snapRenderingOffline = mRenderingOffline;
  iplug::sentry::watchdog::Tick(
    UnpackSlotHandle(mWatchdogSlot),
    snapBlockSize,
    snapSampleRate,
    snapRenderingOffline);
#endif
  ProcessBlock(mScratchData[ERoute::kInput].Get(), mScratchData[ERoute::kOutput].Get(), nFrames);
}

void IPlugProcessor::ProcessBuffers(PLUG_SAMPLE_SRC type, int nFrames)
{
  ProcessBuffers((PLUG_SAMPLE_DST) 0, nFrames);
  int i, n = MaxNChannels(ERoute::kOutput);
  IChannelData<>** ppOutChannel = mChannelData[ERoute::kOutput].GetList();

  for (i = 0; i < n; ++i, ++ppOutChannel)
  {
    IChannelData<>* pOutChannel = *ppOutChannel;

    if (pOutChannel->mConnected)
    {
      CastCopy(pOutChannel->mIncomingData, *(pOutChannel->mData), nFrames);
    }
  }
}

void IPlugProcessor::ProcessBuffersAccumulating(int nFrames)
{
  ProcessBuffers((PLUG_SAMPLE_DST) 0, nFrames);
  int i, n = MaxNChannels(ERoute::kOutput);
  IChannelData<>** ppOutChannel = mChannelData[ERoute::kOutput].GetList();

  for (i = 0; i < n; ++i, ++ppOutChannel)
  {
    IChannelData<>* pOutChannel = *ppOutChannel;
    if (pOutChannel->mConnected)
    {
      PLUG_SAMPLE_SRC* pDest = pOutChannel->mIncomingData;
      PLUG_SAMPLE_DST* pSrc = *(pOutChannel->mData); // TODO : check this: PLUG_SAMPLE_DST will allways be float, because this is only for VST2 accumulating
      for (int j = 0; j < nFrames; ++j, ++pDest, ++pSrc)
      {
        *pDest += (PLUG_SAMPLE_SRC) *pSrc;
      }
    }
  }
}

void IPlugProcessor::ZeroScratchBuffers()
{
  int i, nIn = MaxNChannels(ERoute::kInput), nOut = MaxNChannels(ERoute::kOutput);

  for (i = 0; i < nIn; ++i)
  {
    IChannelData<>* pInChannel = mChannelData[ERoute::kInput].Get(i);
    memset(pInChannel->mScratchBuf.Get(), 0, mBlockSize * sizeof(PLUG_SAMPLE_DST));
  }

  for (i = 0; i < nOut; ++i)
  {
    IChannelData<>* pOutChannel = mChannelData[ERoute::kOutput].Get(i);
    memset(pOutChannel->mScratchBuf.Get(), 0, mBlockSize * sizeof(PLUG_SAMPLE_DST));
  }
}

void IPlugProcessor::SetBlockSize(int blockSize)
{
  if (blockSize != mBlockSize)
  {
    int i, nIn = MaxNChannels(ERoute::kInput), nOut = MaxNChannels(ERoute::kOutput);

    for (i = 0; i < nIn; ++i)
    {
      IChannelData<>* pInChannel = mChannelData[ERoute::kInput].Get(i);
      pInChannel->mScratchBuf.Resize(blockSize);
      memset(pInChannel->mScratchBuf.Get(), 0, blockSize * sizeof(PLUG_SAMPLE_DST));
    }

    for (i = 0; i < nOut; ++i)
    {
      IChannelData<>* pOutChannel = mChannelData[ERoute::kOutput].Get(i);
      pOutChannel->mScratchBuf.Resize(blockSize);
      memset(pOutChannel->mScratchBuf.Get(), 0, blockSize * sizeof(PLUG_SAMPLE_DST));
    }

    mBlockSize = blockSize;
  }
}
