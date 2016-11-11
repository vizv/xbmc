/*
 *      Copyright (C) 2005-2013 Team XBMC
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

#include "DVDCodecUtils.h"
#include "TimingConstants.h"
#include "utils/log.h"
#include "cores/FFmpeg.h"
#include "cores/VideoPlayer/Process/VideoBuffer.h"
#include "Util.h"
#include <assert.h>

extern "C" {
#include "libswscale/swscale.h"
#include "libavutil/intreadwrite.h"
}

bool CDVDCodecUtils::IsVP3CompatibleWidth(int width)
{
  // known hardware limitation of purevideo 3 (VP3). (the Nvidia 9400 is a purevideo 3 chip)
  // from nvidia's linux vdpau README: All current third generation PureVideo hardware
  // (G98, MCP77, MCP78, MCP79, MCP7A) cannot decode H.264 for the following horizontal resolutions:
  // 769-784, 849-864, 929-944, 1009–1024, 1793–1808, 1873–1888, 1953–1968 and 2033-2048 pixel.
  // This relates to the following macroblock sizes.
  int unsupported[] = {49, 54, 59, 64, 113, 118, 123, 128};
  for (unsigned int i = 0; i < sizeof(unsupported) / sizeof(int); i++)
  {
    if (unsupported[i] == (width + 15) / 16)
      return false;
  }
  return true;
}

double CDVDCodecUtils::NormalizeFrameduration(double frameduration, bool *match)
{
  //if the duration is within 20 microseconds of a common duration, use that
  const double durations[] = {DVD_TIME_BASE * 1.001 / 24.0, DVD_TIME_BASE / 24.0, DVD_TIME_BASE / 25.0,
                              DVD_TIME_BASE * 1.001 / 30.0, DVD_TIME_BASE / 30.0, DVD_TIME_BASE / 50.0,
                              DVD_TIME_BASE * 1.001 / 60.0, DVD_TIME_BASE / 60.0};

  double lowestdiff = DVD_TIME_BASE;
  int    selected   = -1;
  for (size_t i = 0; i < ARRAY_SIZE(durations); i++)
  {
    double diff = fabs(frameduration - durations[i]);
    if (diff < DVD_MSEC_TO_TIME(0.02) && diff < lowestdiff)
    {
      selected = i;
      lowestdiff = diff;
    }
  }

  if (selected != -1)
  {
    if (match)
      *match = true;
    return durations[selected];
  }
  else
  {
    if (match)
      *match = false;
    return frameduration;
  }
}

bool CDVDCodecUtils::IsH264AnnexB(std::string format, AVStream *avstream)
{
  assert(avstream->codec->codec_id == AV_CODEC_ID_H264 || avstream->codec->codec_id == AV_CODEC_ID_H264_MVC);
  if (avstream->codec->extradata_size < 4)
    return true;
  if (avstream->codec->extradata[0] == 1)
    return false;
  if (format == "avi")
  {
    BYTE *src = avstream->codec->extradata;
    unsigned startcode = AV_RB32(src);
    if (startcode == 0x00000001 || (startcode & 0xffffff00) == 0x00000100)
      return true;
    if (avstream->codec->codec_tag == MKTAG('A', 'V', 'C', '1') || avstream->codec->codec_tag == MKTAG('a', 'v', 'c', '1'))
      return false;
  }
  return true;
}

bool CDVDCodecUtils::ProcessH264MVCExtradata(uint8_t *data, int data_size, uint8_t **mvc_data, int *mvc_data_size)
{
  uint8_t* extradata = data;
  int extradata_size = data_size;

  if (extradata_size > 4 && *(char *)extradata == 1)
  {
    // Find "mvcC" atom
    uint32_t state = -1;
    int i = 0;
    for (; i < extradata_size; i++)
    {
      state = (state << 8) | extradata[i];
      if (state == MKBETAG('m', 'v', 'c', 'C'))
        break;
    }
    if (i >= 8 && i < extradata_size)
    {
      // Update pointers to the start of the mvcC atom
      extradata = extradata + i - 7;
      extradata_size = extradata_size - i + 7;
      // verify size atom and actual size
      if (extradata_size >= 14 && (AV_RB32(extradata) + 4) <= extradata_size)
      {
        extradata += 8;
        extradata_size -= 8;
        if (*(char *)extradata == 1)
        {
          if (mvc_data)
            *mvc_data = extradata;
          if (mvc_data_size)
            *mvc_data_size = extradata_size;
          return true;
        }
      }
    }
  }
  return false;
}
