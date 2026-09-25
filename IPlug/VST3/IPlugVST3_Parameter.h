/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

#pragma once

#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "base/source/fstring.h"

#include "IPlugParameter.h"

BEGIN_IPLUG_NAMESPACE

/** VST3 parameter helper */
class IPlugVST3Parameter : public Steinberg::Vst::Parameter
{
public:
  IPlugVST3Parameter(IParam* pParam, Steinberg::Vst::ParamID tag, Steinberg::Vst::UnitID unitID)
  : mIPlugParam(pParam)
  {
    // PluginMaker alteration: decode UTF-8. assign(const char*) is fromAscii,
    // which widened each byte on its own and mangled non-ASCII names.
    Steinberg::UString(info.title, str16BufferSize(Steinberg::Vst::String128)).assign(UTF8ToUTF16String(pParam->GetName()).c_str());
    Steinberg::UString(info.units, str16BufferSize(Steinberg::Vst::String128)).assign(UTF8ToUTF16String(pParam->GetLabel()).c_str());

    precision = pParam->GetDisplayPrecision();

    if (pParam->Type() != IParam::kTypeDouble)
      info.stepCount = pParam->GetRange();
    else
      info.stepCount = 0; // continuous

    Steinberg::int32 flags = 0;

    if (pParam->GetCanAutomate()) flags |= Steinberg::Vst::ParameterInfo::kCanAutomate;
    if (pParam->Type() == IParam::kTypeEnum) flags |= Steinberg::Vst::ParameterInfo::kIsList;

    info.defaultNormalizedValue = valueNormalized = pParam->ToNormalized(pParam->GetDefault());
    info.flags = flags;
    info.id = tag;
    info.unitId = unitID;
  }

  void toString(Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string) const override
  {
    // PluginMaker alteration: nothing to write into a NULL host buffer, and
    // decode UTF-8 display text (fromAscii widened each byte).
    if (!string) return;
    WDL_String display;
    mIPlugParam->GetDisplay(valueNormalized, true, display);
    Steinberg::UString(string, 128).assign(UTF8ToUTF16String(display.Get()).c_str());
  }

  bool fromString(const Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized) const override
  {
    // PluginMaker alteration: refuse a NULL host string, and encode UTF-8
    // (text8() narrowed each UTF-16 unit).
    if (!string) return false;
    const std::string utf8 = UTF16ToUTF8String(std::u16string(reinterpret_cast<const char16_t*>(string)));
    valueNormalized = mIPlugParam->ToNormalized(mIPlugParam->StringToValue(utf8.c_str()));

    return true;
  }

  Steinberg::Vst::ParamValue toPlain(Steinberg::Vst::ParamValue normValue) const override
  {
    return mIPlugParam->FromNormalized(normValue);
  }

  Steinberg::Vst::ParamValue toNormalized(Steinberg::Vst::ParamValue plainValue) const override
  {
    return mIPlugParam->ToNormalized(plainValue);
  }

  OBJ_METHODS(IPlugVST3Parameter, Parameter)

protected:
  IParam* mIPlugParam = nullptr;
};

/** VST3 preset parameter helper */
class IPlugVST3PresetParameter : public Steinberg::Vst::Parameter
{
public:
  IPlugVST3PresetParameter(int nPresets)
  : Steinberg::Vst::Parameter(STR16("Preset"), kPresetParam, STR16(""), 0, nPresets - 1, Steinberg::Vst::ParameterInfo::kIsProgramChange)
  {}
  
  Steinberg::Vst::ParamValue toPlain(Steinberg::Vst::ParamValue valueNormalized) const override
  {
    return std::round(valueNormalized * info.stepCount);
  }
  
  Steinberg::Vst::ParamValue toNormalized(Steinberg::Vst::ParamValue plainValue) const override
  {
    return plainValue / info.stepCount;
  }
  
  OBJ_METHODS(IPlugVST3PresetParameter, Steinberg::Vst::Parameter)
};

/** VST3 bypass parameter helper */
class IPlugVST3BypassParameter : public Steinberg::Vst::StringListParameter
{
public:
  IPlugVST3BypassParameter()
  : Steinberg::Vst::StringListParameter(STR16("Bypass"), kBypassParam, 0, Steinberg::Vst::ParameterInfo::kCanAutomate | Steinberg::Vst::ParameterInfo::kIsBypass | Steinberg::Vst::ParameterInfo::kIsList)
  {
    appendString(STR16("off"));
    appendString(STR16("on"));
  }
  
  OBJ_METHODS(IPlugVST3BypassParameter, StringListParameter)
};

END_IPLUG_NAMESPACE

