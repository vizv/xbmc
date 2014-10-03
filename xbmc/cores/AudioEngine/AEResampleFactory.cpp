/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "settings/Settings.h"
#include "AEResampleFactory.h"
#include "Engines/ActiveAE/ActiveAEResampleFFMPEG.h"
#if defined(TARGET_RASPBERRY_PI)
  #include "Engines/ActiveAE/ActiveAEResamplePi.h"
#endif

extern "C" {
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
}

namespace ActiveAE
{

IAEResample *CAEResampleFactory::Create()
{
#if defined(TARGET_RASPBERRY_PI)
  if (CSettings::Get().GetBool("videoplayer.resamplepi"))
    return new CActiveAEResamplePi();
#endif
  return new CActiveAEResampleFFMPEG();
}


uint64_t CAEResampleFactory::GetAVChannelLayout(CAEChannelInfo &info)
{
  uint64_t channelLayout = 0;
  if (info.HasChannel(AE_CH_FL))   channelLayout |= AV_CH_FRONT_LEFT;
  if (info.HasChannel(AE_CH_FR))   channelLayout |= AV_CH_FRONT_RIGHT;
  if (info.HasChannel(AE_CH_FC))   channelLayout |= AV_CH_FRONT_CENTER;
  if (info.HasChannel(AE_CH_LFE))  channelLayout |= AV_CH_LOW_FREQUENCY;
  if (info.HasChannel(AE_CH_BL))   channelLayout |= AV_CH_BACK_LEFT;
  if (info.HasChannel(AE_CH_BR))   channelLayout |= AV_CH_BACK_RIGHT;
  if (info.HasChannel(AE_CH_FLOC)) channelLayout |= AV_CH_FRONT_LEFT_OF_CENTER;
  if (info.HasChannel(AE_CH_FROC)) channelLayout |= AV_CH_FRONT_RIGHT_OF_CENTER;
  if (info.HasChannel(AE_CH_BC))   channelLayout |= AV_CH_BACK_CENTER;
  if (info.HasChannel(AE_CH_SL))   channelLayout |= AV_CH_SIDE_LEFT;
  if (info.HasChannel(AE_CH_SR))   channelLayout |= AV_CH_SIDE_RIGHT;
  if (info.HasChannel(AE_CH_TC))   channelLayout |= AV_CH_TOP_CENTER;
  if (info.HasChannel(AE_CH_TFL))  channelLayout |= AV_CH_TOP_FRONT_LEFT;
  if (info.HasChannel(AE_CH_TFC))  channelLayout |= AV_CH_TOP_FRONT_CENTER;
  if (info.HasChannel(AE_CH_TFR))  channelLayout |= AV_CH_TOP_FRONT_RIGHT;
  if (info.HasChannel(AE_CH_TBL))   channelLayout |= AV_CH_TOP_BACK_LEFT;
  if (info.HasChannel(AE_CH_TBC))   channelLayout |= AV_CH_TOP_BACK_CENTER;
  if (info.HasChannel(AE_CH_TBR))   channelLayout |= AV_CH_TOP_BACK_RIGHT;

  return channelLayout;
}

//CAEChannelInfo CActiveAEResampleFFMPEG::GetAEChannelLayout(uint64_t layout)
//{
//  CAEChannelInfo channelLayout;
//  channelLayout.Reset();
//
//  if (layout & AV_CH_FRONT_LEFT           ) channelLayout += AE_CH_FL  ;
//  if (layout & AV_CH_FRONT_RIGHT          ) channelLayout += AE_CH_FR  ;
//  if (layout & AV_CH_FRONT_CENTER         ) channelLayout += AE_CH_FC  ;
//  if (layout & AV_CH_LOW_FREQUENCY        ) channelLayout += AE_CH_LFE ;
//  if (layout & AV_CH_BACK_LEFT            ) channelLayout += AE_CH_BL  ;
//  if (layout & AV_CH_BACK_RIGHT           ) channelLayout += AE_CH_BR  ;
//  if (layout & AV_CH_FRONT_LEFT_OF_CENTER ) channelLayout += AE_CH_FLOC;
//  if (layout & AV_CH_FRONT_RIGHT_OF_CENTER) channelLayout += AE_CH_FROC;
//  if (layout & AV_CH_BACK_CENTER          ) channelLayout += AE_CH_BC  ;
//  if (layout & AV_CH_SIDE_LEFT            ) channelLayout += AE_CH_SL  ;
//  if (layout & AV_CH_SIDE_RIGHT           ) channelLayout += AE_CH_SR  ;
//  if (layout & AV_CH_TOP_CENTER           ) channelLayout += AE_CH_TC  ;
//  if (layout & AV_CH_TOP_FRONT_LEFT       ) channelLayout += AE_CH_TFL ;
//  if (layout & AV_CH_TOP_FRONT_CENTER     ) channelLayout += AE_CH_TFC ;
//  if (layout & AV_CH_TOP_FRONT_RIGHT      ) channelLayout += AE_CH_TFR ;
//  if (layout & AV_CH_TOP_BACK_LEFT        ) channelLayout += AE_CH_BL  ;
//  if (layout & AV_CH_TOP_BACK_CENTER      ) channelLayout += AE_CH_BC  ;
//  if (layout & AV_CH_TOP_BACK_RIGHT       ) channelLayout += AE_CH_BR  ;
//
//  return channelLayout;
//}

AVSampleFormat CAEResampleFactory::GetAVSampleFormat(AEDataFormat format)
{
  if      (format == AE_FMT_U8)     return AV_SAMPLE_FMT_U8;
  else if (format == AE_FMT_S16NE)  return AV_SAMPLE_FMT_S16;
  else if (format == AE_FMT_S32NE)  return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_S24NE4) return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_S24NE4MSB)return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_S24NE3) return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_FLOAT)  return AV_SAMPLE_FMT_FLT;
  else if (format == AE_FMT_DOUBLE) return AV_SAMPLE_FMT_DBL;

  else if (format == AE_FMT_U8P)     return AV_SAMPLE_FMT_U8P;
  else if (format == AE_FMT_S16NEP)  return AV_SAMPLE_FMT_S16P;
  else if (format == AE_FMT_S32NEP)  return AV_SAMPLE_FMT_S32P;
  else if (format == AE_FMT_S24NE4P) return AV_SAMPLE_FMT_S32P;
  else if (format == AE_FMT_S24NE4MSBP)return AV_SAMPLE_FMT_S32P;
  else if (format == AE_FMT_S24NE3P) return AV_SAMPLE_FMT_S32P;
  else if (format == AE_FMT_FLOATP)  return AV_SAMPLE_FMT_FLTP;
  else if (format == AE_FMT_DOUBLEP) return AV_SAMPLE_FMT_DBLP;

  if (AE_IS_PLANAR(format))
    return AV_SAMPLE_FMT_FLTP;
  else
    return AV_SAMPLE_FMT_FLT;
}

uint64_t CAEResampleFactory::GetAVChannel(enum AEChannel aechannel)
{
  switch (aechannel)
  {
  case AE_CH_FL:   return AV_CH_FRONT_LEFT;
  case AE_CH_FR:   return AV_CH_FRONT_RIGHT;
  case AE_CH_FC:   return AV_CH_FRONT_CENTER;
  case AE_CH_LFE:  return AV_CH_LOW_FREQUENCY;
  case AE_CH_BL:   return AV_CH_BACK_LEFT;
  case AE_CH_BR:   return AV_CH_BACK_RIGHT;
  case AE_CH_FLOC: return AV_CH_FRONT_LEFT_OF_CENTER;
  case AE_CH_FROC: return AV_CH_FRONT_RIGHT_OF_CENTER;
  case AE_CH_BC:   return AV_CH_BACK_CENTER;
  case AE_CH_SL:   return AV_CH_SIDE_LEFT;
  case AE_CH_SR:   return AV_CH_SIDE_RIGHT;
  case AE_CH_TC:   return AV_CH_TOP_CENTER;
  case AE_CH_TFL:  return AV_CH_TOP_FRONT_LEFT;
  case AE_CH_TFC:  return AV_CH_TOP_FRONT_CENTER;
  case AE_CH_TFR:  return AV_CH_TOP_FRONT_RIGHT;
  case AE_CH_TBL:  return AV_CH_TOP_BACK_LEFT;
  case AE_CH_TBC:  return AV_CH_TOP_BACK_CENTER;
  case AE_CH_TBR:  return AV_CH_TOP_BACK_RIGHT;
  default:
    return 0;
  }
}

int CAEResampleFactory::GetAVChannelIndex(enum AEChannel aechannel, uint64_t layout)
{
  return av_get_channel_layout_channel_index(layout, GetAVChannel(aechannel));
}

}
