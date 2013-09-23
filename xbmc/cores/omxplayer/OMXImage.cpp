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

#if (defined HAVE_CONFIG_H) && (!defined TARGET_WINDOWS)
  #include "config.h"
#elif defined(TARGET_WINDOWS)
#include "system.h"
#endif

#include "OMXImage.h"

#include "utils/log.h"
#include "linux/XMemUtils.h"

#include <sys/time.h>
#include <inttypes.h>
#include "guilib/GraphicContext.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "linux/RBP.h"

#define EXIF_TAG_ORIENTATION    0x0112


#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXImageFile"

COMXImageFile::COMXImageFile()
{
  m_image_size    = 0;
  m_image_buffer  = NULL;
  m_orientation   = 0;
  m_width         = 0;
  m_height        = 0;
  m_filename      = "";
}

COMXImageFile::~COMXImageFile()
{
  if(m_image_buffer)
    free(m_image_buffer);
}

typedef enum {      /* JPEG marker codes */
  M_SOF0  = 0xc0,
  M_SOF1  = 0xc1,
  M_SOF2  = 0xc2,
  M_SOF3  = 0xc3,
  M_SOF5  = 0xc5,
  M_SOF6  = 0xc6,
  M_SOF7  = 0xc7,
  M_JPG   = 0xc8,
  M_SOF9  = 0xc9,
  M_SOF10 = 0xca,
  M_SOF11 = 0xcb,
  M_SOF13 = 0xcd,
  M_SOF14 = 0xce,
  M_SOF15 = 0xcf,

  M_DHT   = 0xc4,
  M_DAC   = 0xcc,

  M_RST0  = 0xd0,
  M_RST1  = 0xd1,
  M_RST2  = 0xd2,
  M_RST3  = 0xd3,
  M_RST4  = 0xd4,
  M_RST5  = 0xd5,
  M_RST6  = 0xd6,
  M_RST7  = 0xd7,

  M_SOI   = 0xd8,
  M_EOI   = 0xd9,
  M_SOS   = 0xda,
  M_DQT   = 0xdb,
  M_DNL   = 0xdc,
  M_DRI   = 0xdd,
  M_DHP   = 0xde,
  M_EXP   = 0xdf,

  M_APP0  = 0xe0,
  M_APP1  = 0xe1,
  M_APP2  = 0xe2,
  M_APP3  = 0xe3,
  M_APP4  = 0xe4,
  M_APP5  = 0xe5,
  M_APP6  = 0xe6,
  M_APP7  = 0xe7,
  M_APP8  = 0xe8,
  M_APP9  = 0xe9,
  M_APP10 = 0xea,
  M_APP11 = 0xeb,
  M_APP12 = 0xec,
  M_APP13 = 0xed,
  M_APP14 = 0xee,
  M_APP15 = 0xef,
  // extensions
  M_JPG0  = 0xf0,
  M_JPG1  = 0xf1,
  M_JPG2  = 0xf2,
  M_JPG3  = 0xf3,
  M_JPG4  = 0xf4,
  M_JPG5  = 0xf5,
  M_JPG6  = 0xf6,
  M_JPG7  = 0xf7,
  M_JPG8  = 0xf8,
  M_JPG9  = 0xf9,
  M_JPG10 = 0xfa,
  M_JPG11 = 0xfb,
  M_JPG12 = 0xfc,
  M_JPG13 = 0xfd,
  M_JPG14 = 0xfe,
  M_COM   = 0xff,

  M_TEM   = 0x01,
} JPEG_MARKER;

static uint8_t inline READ8(uint8_t * &p)
{
  uint8_t r = p[0];
  p += 1;
  return r;
}

static uint16_t inline READ16(uint8_t * &p)
{
  uint16_t r = (p[0] << 8) | p[1];
  p += 2;
  return r;
}

static uint32_t inline READ32(uint8_t * &p)
{
  uint32_t r = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  p += 4;
  return r;
}

static void inline SKIPN(uint8_t * &p, unsigned int n)
{
  p += n;
}


OMX_IMAGE_CODINGTYPE COMXImageFile::GetCodingType(unsigned int &width, unsigned int &height)
{
  OMX_IMAGE_CODINGTYPE eCompressionFormat = OMX_IMAGE_CodingMax;
  bool progressive = false;
  m_orientation   = 0;

  if(!m_image_size)
    return OMX_IMAGE_CodingMax;

  uint8_t *p = m_image_buffer;
  uint8_t *q = m_image_buffer + m_image_size;

  /* JPEG Header */
  if(READ16(p) == 0xFFD8)
  {
    eCompressionFormat = OMX_IMAGE_CodingJPEG;

    READ8(p);
    unsigned char marker = READ8(p);
    unsigned short block_size = 0;
    bool nMarker = false;

    while(p < q)
    {
      switch(marker)
      {
        case M_DQT:
        case M_DNL:
        case M_DHP:
        case M_EXP:

        case M_DHT:

        case M_SOF0:
        case M_SOF1:
        case M_SOF2:
        case M_SOF3:

        case M_SOF5:
        case M_SOF6:
        case M_SOF7:

        case M_JPG:
        case M_SOF9:
        case M_SOF10:
        case M_SOF11:

        case M_SOF13:
        case M_SOF14:
        case M_SOF15:

        case M_APP0:
        case M_APP1:
        case M_APP2:
        case M_APP3:
        case M_APP4:
        case M_APP5:
        case M_APP6:
        case M_APP7:
        case M_APP8:
        case M_APP9:
        case M_APP10:
        case M_APP11:
        case M_APP12:
        case M_APP13:
        case M_APP14:
        case M_APP15:

        case M_JPG0:
        case M_JPG1:
        case M_JPG2:
        case M_JPG3:
        case M_JPG4:
        case M_JPG5:
        case M_JPG6:
        case M_JPG7:
        case M_JPG8:
        case M_JPG9:
        case M_JPG10:
        case M_JPG11:
        case M_JPG12:
        case M_JPG13:
        case M_JPG14:
        case M_COM:
          block_size = READ16(p);
          nMarker = true;
          break;

        case M_SOS:
        default:
          nMarker = false;
          break;
      }

      if(!nMarker)
      {
        break;
      }

      if(marker >= M_SOF0 && marker <= M_SOF15 && marker != M_DHT && marker != M_DAC)
      {
        if(marker == M_SOF2 || marker == M_SOF6 || marker == M_SOF10 || marker == M_SOF14)
        {
          progressive = true;
        }
        int readBits = 2;
        SKIPN(p, 1);
        readBits ++;
        height = READ16(p);
        readBits += 2;
        width = READ16(p);
        readBits += 2;
        SKIPN(p, 1 * (block_size - readBits));
      }
      else if(marker == M_APP1)
      {
        int readBits = 2;

        // Exif header
        if(READ32(p) == 0x45786966)
        {
          bool bMotorolla = false;
          bool bError = false;
          SKIPN(p, 1 * 2);
          readBits += 2;
        
          char o1 = READ8(p);
          char o2 = READ8(p);
          readBits += 2;

          /* Discover byte order */
          if(o1 == 'M' && o2 == 'M')
            bMotorolla = true;
          else if(o1 == 'I' && o2 == 'I')
            bMotorolla = false;
          else
            bError = true;
        
          SKIPN(p, 1 * 2);
          readBits += 2;

          if(!bError)
          {
            unsigned int offset, a, b, numberOfTags, tagNumber;
  
            // Get first IFD offset (offset to IFD0)
            if(bMotorolla)
            {
              SKIPN(p, 1 * 2);
              readBits += 2;

              a = READ8(p);
              b = READ8(p);
              readBits += 2;
              offset = (a << 8) + b;
            }
            else
            {
              a = READ8(p);
              b = READ8(p);
              readBits += 2;
              offset = (b << 8) + a;

              SKIPN(p, 1 * 2);
              readBits += 2;
            }

            offset -= 8;
            if(offset > 0)
            {
              SKIPN(p, 1 * offset);
              readBits += offset;
            } 

            // Get the number of directory entries contained in this IFD
            if(bMotorolla)
            {
              a = READ8(p);
              b = READ8(p);
              numberOfTags = (a << 8) + b;
            }
            else
            {
              a = READ8(p);
              b = READ8(p);
              numberOfTags = (b << 8) + a;
            }
            readBits += 2;

            while(numberOfTags && p < q)
            {
              // Get Tag number
              if(bMotorolla)
              {
                a = READ8(p);
                b = READ8(p);
                tagNumber = (a << 8) + b;
                readBits += 2;
              }
              else
              {
                a = READ8(p);
                b = READ8(p);
                tagNumber = (b << 8) + a;
                readBits += 2;
              }

              //found orientation tag
              if(tagNumber == EXIF_TAG_ORIENTATION)
              {
                if(bMotorolla)
                {
                  SKIPN(p, 1 * 7);
                  readBits += 7;
                  m_orientation = READ8(p);
                  readBits += 1;
                  SKIPN(p, 1 * 2);
                  readBits += 2;
                }
                else
                {
                  SKIPN(p, 1 * 6);
                  readBits += 6;
                  m_orientation = READ8(p);
                  readBits += 1;
                  SKIPN(p, 1 * 3);
                  readBits += 3;
                }
                break;
              }
              else
              {
                SKIPN(p, 1 * 10);
                readBits += 10;
              }
              numberOfTags--;
            }
          }
        }
        readBits += 4;
        SKIPN(p, 1 * (block_size - readBits));
      }
      else
      {
        SKIPN(p, 1 * (block_size - 2));
      }

      READ8(p);
      marker = READ8(p);

    }
  }

  if(m_orientation > 8)
    m_orientation = 0;

  if(eCompressionFormat == OMX_IMAGE_CodingMax)
  {
    CLog::Log(LOGERROR, "%s::%s error unsupported image format\n", CLASSNAME, __func__);
  }

  if(progressive)
  {
    CLog::Log(LOGWARNING, "%s::%s progressive images not supported by decoder\n", CLASSNAME, __func__);
    eCompressionFormat = OMX_IMAGE_CodingMax;
  }

  return eCompressionFormat;
}


bool COMXImageFile::ReadFile(const CStdString& inputFile)
{
  XFILE::CFile      m_pFile;
  m_filename = inputFile.c_str();
  if(!m_pFile.Open(inputFile, 0))
  {
    CLog::Log(LOGERROR, "%s::%s %s not found\n", CLASSNAME, __func__, inputFile.c_str());
    return false;
  }

  if(m_image_buffer)
    free(m_image_buffer);
  m_image_buffer = NULL;

  m_image_size = m_pFile.GetLength();

  if(!m_image_size)
  {
    CLog::Log(LOGERROR, "%s::%s %s m_image_size zero\n", CLASSNAME, __func__, inputFile.c_str());
    return false;
  }
  m_image_buffer = (uint8_t *)malloc(m_image_size);
  if(!m_image_buffer)
  {
    CLog::Log(LOGERROR, "%s::%s %s m_image_buffer null (%lu)\n", CLASSNAME, __func__, inputFile.c_str(), m_image_size);
    return false;
  }
  
  m_pFile.Read(m_image_buffer, m_image_size);
  m_pFile.Close();

  OMX_IMAGE_CODINGTYPE eCompressionFormat = GetCodingType(m_width, m_height);
  if(eCompressionFormat != OMX_IMAGE_CodingJPEG)
  {
    CLog::Log(LOGERROR, "%s::%s %s GetCodingType=0x%x\n", CLASSNAME, __func__, inputFile.c_str(), eCompressionFormat);
    return false;
  }

  if(m_width < 1 || m_height < 1)
  {
    CLog::Log(LOGERROR, "%s::%s %s m_width=%d m_height=%d\n", CLASSNAME, __func__, inputFile.c_str(), m_width, m_height);
    return false;
  }

  return true;
}

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXImage"

COMXImage::COMXImage()
{
  m_decoded_buffer = NULL;
  OMX_INIT_STRUCTURE(m_decoded_format);
}

COMXImage::~COMXImage()
{
  Close();

  OMX_INIT_STRUCTURE(m_decoded_format);
  m_decoded_buffer = NULL;
}

void COMXImage::Close()
{
  CSingleLock lock(m_OMXSection);

  if(m_omx_decoder.IsInitialized())
  {
    m_omx_decoder.FlushInput();
    m_omx_decoder.FreeInputBuffers();
  }
  if(m_omx_resize.IsInitialized())
  {
    m_omx_resize.FlushOutput();
    m_omx_resize.FreeOutputBuffers();
  }
  if(m_omx_tunnel_decode.IsInitialized())
  {
    m_omx_tunnel_decode.Flush();
    m_omx_tunnel_decode.Deestablish();
  }
  if(m_omx_decoder.IsInitialized())
    m_omx_decoder.Deinitialize();
  if(m_omx_resize.IsInitialized())
    m_omx_resize.Deinitialize();
}

bool COMXImage::HandlePortSettingChange(unsigned int resize_width, unsigned int resize_height)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  // on the first port settings changed event, we create the tunnel and alloc the buffer
  if (!m_decoded_buffer)
  {
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    port_def.format.image.nSliceHeight = 16;
    m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    port_def.nPortIndex = m_omx_resize.GetInputPort();
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_resize, m_omx_resize.GetInputPort());

    omx_err = m_omx_tunnel_decode.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_tunnel_decode.Establish\n", CLASSNAME, __func__);
      return false;
    }
    omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.WaitForEvent=%x\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &port_def);

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
    port_def.format.image.eColorFormat = OMX_COLOR_Format32bitARGB8888;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nStride = resize_width*4;
    port_def.format.image.nSliceHeight = 0;
    port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

    omx_err = m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    OMX_INIT_STRUCTURE(m_decoded_format);
    m_decoded_format.nPortIndex = m_omx_resize.GetOutputPort();
    omx_err = m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &m_decoded_format);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    assert(m_decoded_format.nBufferCountActual == 1);

    omx_err = m_omx_resize.AllocOutputBuffers();
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.AllocOutputBuffers result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    omx_err = m_omx_resize.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    m_decoded_buffer = m_omx_resize.GetOutputBuffer();

    if(!m_decoded_buffer)
    {
      CLog::Log(LOGERROR, "%s::%s no output buffer\n", CLASSNAME, __func__);
      return false;
    }

    omx_err = m_omx_resize.FillThisBuffer(m_decoded_buffer);
    if(omx_err != OMX_ErrorNone)
     {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize FillThisBuffer result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
  }
  // on subsequent port settings changed event, we just copy the port settings
  else
  {
    // a little surprising, make a note
    CLog::Log(LOGDEBUG, "%s::%s m_omx_resize second port changed event\n", CLASSNAME, __func__);
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_resize.DisablePort(m_omx_resize.GetInputPort(), true);

    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    port_def.nPortIndex = m_omx_resize.GetInputPort();
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.WaitForEvent=%x\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_resize.EnablePort(m_omx_resize.GetInputPort(), true);
  }
  return true;
}

bool COMXImage::Decode(const uint8_t *demuxer_content, unsigned demuxer_bytes, unsigned width, unsigned height, unsigned stride, void *pixels)
{
  CSingleLock lock(m_OMXSection);
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;

  if(!demuxer_content || !demuxer_bytes)
  {
    CLog::Log(LOGERROR, "%s::%s no input buffer\n", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_decoder.Initialize("OMX.broadcom.image_decode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_resize.Initialize("OMX.broadcom.resize", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_resize.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  // set input format
  OMX_IMAGE_PARAM_PORTFORMATTYPE imagePortFormat;
  OMX_INIT_STRUCTURE(imagePortFormat);
  imagePortFormat.nPortIndex = m_omx_decoder.GetInputPort();
  imagePortFormat.eCompressionFormat = OMX_IMAGE_CodingJPEG;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamImagePortFormat, &imagePortFormat);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetParameter OMX_IndexParamImagePortFormat result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.AllocInputBuffers result(0x%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  while(demuxer_bytes > 0 || !m_decoded_buffer)
  {
    long timeout = 0;
    if (demuxer_bytes)
    {
       omx_buffer = m_omx_decoder.GetInputBuffer(1000);
       if(omx_buffer == NULL)
         return false;

       omx_buffer->nOffset = omx_buffer->nFlags  = 0;

       omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
       memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

       demuxer_content += omx_buffer->nFilledLen;
       demuxer_bytes -= omx_buffer->nFilledLen;

       if(demuxer_bytes == 0)
         omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

       omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
       if (omx_err != OMX_ErrorNone)
       {
         CLog::Log(LOGERROR, "%s::%s OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
         return false;
       }
    }
    else
    {
       // we've submitted all buffers so can wait now
       timeout = 1000;
    }
    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, timeout);
    if(omx_err == OMX_ErrorNone)
    {
      if (!HandlePortSettingChange(width, height))
      {
        CLog::Log(LOGERROR, "%s::%s HandlePortSettingChange() failed\n", CLASSNAME, __func__);
        return false;
      }
    }
    // we treat it as an error if a real timeout occurred
    else  if (timeout)
    {
      CLog::Log(LOGERROR, "%s::%s HandlePortSettingChange() failed\n", CLASSNAME, __func__);
      return false;
    }
  }

  omx_err = m_omx_resize.WaitForOutputDone(1000);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_resize.WaitForOutputDone result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  if(m_omx_decoder.BadState())
    return false;

  assert(m_decoded_buffer->nFilledLen <= stride * height);
  memcpy( (char*)pixels, m_decoded_buffer->pBuffer, m_decoded_buffer->nFilledLen);

  Close();
  return true;
}

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXImageEnc"

COMXImageEnc::COMXImageEnc()
{
  CSingleLock lock(m_OMXSection);
  OMX_INIT_STRUCTURE(m_encoded_format);
  m_encoded_buffer = NULL;
}

COMXImageEnc::~COMXImageEnc()
{
  CSingleLock lock(m_OMXSection);

  OMX_INIT_STRUCTURE(m_encoded_format);
  m_encoded_buffer = NULL;
  if(m_omx_encoder.IsInitialized())
    m_omx_encoder.Deinitialize();
}

bool COMXImageEnc::Encode(unsigned char *buffer, int size, unsigned width, unsigned height, unsigned int pitch)
{
  CSingleLock lock(m_OMXSection);

  unsigned int demuxer_bytes = 0;
  const uint8_t *demuxer_content = NULL;
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;
  OMX_INIT_STRUCTURE(m_encoded_format);

  if (pitch == 0)
     pitch = 4 * width;

  if (!buffer || !size) 
  {
    CLog::Log(LOGERROR, "%s::%s error no buffer\n", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_encoder.Initialize("OMX.broadcom.image_encode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_encoder.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_INIT_STRUCTURE(port_def);
  port_def.nPortIndex = m_omx_encoder.GetInputPort();

  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
  port_def.format.image.eColorFormat = OMX_COLOR_Format32bitARGB8888;
  port_def.format.image.nFrameWidth = width;
  port_def.format.image.nFrameHeight = height;
  port_def.format.image.nStride = pitch;
  port_def.format.image.nSliceHeight = (height+15) & ~15;
  port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_INIT_STRUCTURE(port_def);
  port_def.nPortIndex = m_omx_encoder.GetOutputPort();

  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
  port_def.format.image.eColorFormat = OMX_COLOR_FormatUnused;
  port_def.format.image.nFrameWidth = width;
  port_def.format.image.nFrameHeight = height;
  port_def.format.image.nStride = 0;
  port_def.format.image.nSliceHeight = 0;
  port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_IMAGE_PARAM_QFACTORTYPE qfactor;
  OMX_INIT_STRUCTURE(qfactor);
  qfactor.nPortIndex = m_omx_encoder.GetOutputPort();
  qfactor.nQFactor = 16;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamQFactor, &qfactor);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter OMX_IndexParamQFactor result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_encoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.AllocInputBuffers result(0x%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_encoder.AllocOutputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.AllocOutputBuffers result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_encoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  demuxer_content = buffer;
  demuxer_bytes   = height * pitch;

  if(!demuxer_bytes || !demuxer_content)
    return false;

  while(demuxer_bytes > 0)
  {
    omx_buffer = m_omx_encoder.GetInputBuffer(1000);
    if(omx_buffer == NULL)
    {
      return false;
    }

    omx_buffer->nOffset = omx_buffer->nFlags  = 0;

    omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
    memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

    demuxer_content += omx_buffer->nFilledLen;
    demuxer_bytes -= omx_buffer->nFilledLen;

    if(demuxer_bytes == 0)
      omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

    omx_err = m_omx_encoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
      break;
    }
  }

  m_encoded_buffer = m_omx_encoder.GetOutputBuffer();

  if(!m_encoded_buffer)
  {
    CLog::Log(LOGERROR, "%s::%s no output buffer\n", CLASSNAME, __func__);
    return false;
  }

  omx_err = m_omx_encoder.FillThisBuffer(m_encoded_buffer);
  if(omx_err != OMX_ErrorNone)
    return false;

  omx_err = m_omx_encoder.WaitForOutputDone(1000);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_resize.WaitForOutputDone result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_encoded_format.nPortIndex = m_omx_encoder.GetOutputPort();
  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &m_encoded_format);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  if(m_omx_encoder.BadState())
    return false;

  return true;
}

bool COMXImageEnc::CreateThumbnailFromSurface(unsigned char* buffer, unsigned int width, unsigned int height,
    unsigned int format, unsigned int pitch, const CStdString& destFile)
{
  if(format != XB_FMT_A8R8G8B8 || !buffer)
  {
    CLog::Log(LOGDEBUG, "%s::%s : %s failed format=0x%x\n", CLASSNAME, __func__, destFile.c_str(), format);
    return false;
  }

  if(!Encode(buffer, height * pitch, width, height, pitch))
  {
    CLog::Log(LOGDEBUG, "%s::%s : %s encode failed\n", CLASSNAME, __func__, destFile.c_str());
    return false;
  }

  XFILE::CFile file;
  if (file.OpenForWrite(destFile, true))
  {
    CLog::Log(LOGDEBUG, "%s::%s : %s width %d height %d\n", CLASSNAME, __func__, destFile.c_str(), width, height);

    file.Write(m_encoded_buffer->pBuffer, m_encoded_buffer->nFilledLen);
    file.Close();
    return true;
  }

  return false;
}

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXReEnc"

COMXImageReEnc::COMXImageReEnc()
{
  m_encoded_buffer = NULL;
  m_pDestBuffer = NULL;
  m_nDestAllocSize = 0;
}

COMXImageReEnc::~COMXImageReEnc()
{
  Close();
  if (m_pDestBuffer)
    free (m_pDestBuffer);
  m_pDestBuffer = NULL;
  m_nDestAllocSize = 0;
}

void COMXImageReEnc::Close()
{
  CSingleLock lock(m_OMXSection);

  if(m_omx_decoder.IsInitialized())
  {
    m_omx_decoder.FlushInput();
    m_omx_decoder.FreeInputBuffers();
  }
  if(m_omx_resize.IsInitialized())
  {
    m_omx_resize.FlushOutput();
    m_omx_resize.FreeOutputBuffers();
  }
  if(m_omx_encoder.IsInitialized())
  {
    m_omx_encoder.FlushOutput();
    m_omx_encoder.FreeOutputBuffers();
  }
  if(m_omx_tunnel_decode.IsInitialized())
  {
    m_omx_tunnel_decode.Flush();
    m_omx_tunnel_decode.Deestablish();
  }
  if(m_omx_tunnel_resize.IsInitialized())
  {
    m_omx_tunnel_resize.Flush();
    m_omx_tunnel_resize.Deestablish();
  }
  if(m_omx_decoder.IsInitialized())
    m_omx_decoder.Deinitialize();
  if(m_omx_resize.IsInitialized())
    m_omx_resize.Deinitialize();
  if(m_omx_encoder.IsInitialized())
    m_omx_encoder.Deinitialize();
}



bool COMXImageReEnc::HandlePortSettingChange(unsigned int resize_width, unsigned int resize_height, bool port_settings_changed)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  // on the first port settings changed event, we create the tunnel and alloc the buffer
  if (!port_settings_changed)
  {
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_decoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    // TODO: jpeg decoder can decimate by factors of 2
    port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
    port_def.format.image.nSliceHeight = 16;//(port_def.format.image.nFrameHeight+15) & ~15;
    port_def.format.image.nStride = 0;

    m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_resize.GetInputPort();

    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nSliceHeight = (resize_height+15) & ~15;
    port_def.format.image.nStride = 0;
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_encoder.GetInputPort();
    m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_encoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nSliceHeight = (resize_height+15) & ~15;
    port_def.format.image.nStride = 0;
    m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_encoder.GetOutputPort();
    omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_encoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
    port_def.format.image.eColorFormat = OMX_COLOR_FormatUnused;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nStride = 0;
    port_def.format.image.nSliceHeight = 0;
    port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

    omx_err = m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    OMX_IMAGE_PARAM_QFACTORTYPE qfactor;
    OMX_INIT_STRUCTURE(qfactor);
    qfactor.nPortIndex = m_omx_encoder.GetOutputPort();
    qfactor.nQFactor = 16;

    omx_err = m_omx_encoder.SetParameter(OMX_IndexParamQFactor, &qfactor);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter OMX_IndexParamQFactor result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_err = m_omx_encoder.AllocOutputBuffers();
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_encoder.AllocOutputBuffers result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_resize, m_omx_resize.GetInputPort());

    omx_err = m_omx_tunnel_decode.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_tunnel_decode.Establish\n", CLASSNAME, __func__);
      return false;
    }

    m_omx_tunnel_resize.Initialize(&m_omx_resize, m_omx_resize.GetOutputPort(), &m_omx_encoder, m_omx_encoder.GetInputPort());

    omx_err = m_omx_tunnel_resize.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_tunnel_resize.Establish\n", CLASSNAME, __func__);
      return false;
    }

    omx_err = m_omx_resize.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_err = m_omx_encoder.SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    if(m_omx_encoder.BadState())
      return false;
  }
  // on subsequent port settings changed event, we just copy the port settings
  else
  {
    // a little surprising, make a note
    CLog::Log(LOGDEBUG, "%s::%s m_omx_resize second port changed event\n", CLASSNAME, __func__);
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_resize.DisablePort(m_omx_resize.GetInputPort(), true);

    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    port_def.nPortIndex = m_omx_resize.GetInputPort();
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.WaitForEvent=%x\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_resize.EnablePort(m_omx_resize.GetInputPort(), true);
  }
  return true;
}

bool COMXImageReEnc::ReEncode(COMXImageFile &srcFile, unsigned int maxWidth, unsigned int maxHeight, void * &pDestBuffer, unsigned int &nDestSize)
{
  CSingleLock lock(m_OMXSection);
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  g_RBP.ClampLimits(maxWidth, maxHeight, srcFile.GetWidth(), srcFile.GetHeight(), srcFile.GetOrientation() & 4);
  unsigned int demuxer_bytes = srcFile.GetImageSize();
  unsigned char *demuxer_content = (unsigned char *)srcFile.GetImageBuffer();
  // initial dest buffer size
  nDestSize = 0;

  if(!demuxer_content || !demuxer_bytes)
  {
    CLog::Log(LOGERROR, "%s::%s no input buffer\n", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_decoder.Initialize("OMX.broadcom.image_decode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_resize.Initialize("OMX.broadcom.resize", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_resize.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_encoder.Initialize("OMX.broadcom.image_encode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_encoder.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  // set input format
  OMX_IMAGE_PARAM_PORTFORMATTYPE imagePortFormat;
  OMX_INIT_STRUCTURE(imagePortFormat);
  imagePortFormat.nPortIndex = m_omx_decoder.GetInputPort();
  imagePortFormat.eCompressionFormat = OMX_IMAGE_CodingJPEG;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamImagePortFormat, &imagePortFormat);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetParameter OMX_IndexParamImagePortFormat result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.AllocInputBuffers result(0x%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  bool port_settings_changed = false, eos = false;
  while(demuxer_bytes > 0 || !port_settings_changed || !eos)
  {
    long timeout = 0;
    if (demuxer_bytes)
    {
       OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(1000);
       if(omx_buffer)
       {
         omx_buffer->nOffset = omx_buffer->nFlags  = 0;

         omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
         memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

         demuxer_content += omx_buffer->nFilledLen;
         demuxer_bytes -= omx_buffer->nFilledLen;
         if(demuxer_bytes == 0)
           omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

         omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
         if (omx_err != OMX_ErrorNone)
         {
           CLog::Log(LOGERROR, "%s::%s OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
           return false;
         }
      }
    }

    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, 0);
    if(omx_err == OMX_ErrorNone)
    {
      if (!HandlePortSettingChange(maxWidth, maxHeight, port_settings_changed))
      {
        CLog::Log(LOGERROR, "%s::%s HandlePortSettingChange() failed\n", CLASSNAME, __func__);
        return false;
      }
      port_settings_changed = true;
    }

    if (!m_encoded_buffer && port_settings_changed && demuxer_bytes == 0)
    {
      m_encoded_buffer = m_omx_encoder.GetOutputBuffer();
      omx_err = m_omx_encoder.FillThisBuffer(m_encoded_buffer);
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "%s::%s FillThisBuffer() failed\n", CLASSNAME, __func__);
        return false;
      }
    }
    if (m_encoded_buffer)
    {
      if (m_omx_encoder.WaitForOutputDone(1000) == OMX_ErrorNone && m_encoded_buffer->nFilledLen)
      {
        if (m_encoded_buffer->nFlags & OMX_BUFFERFLAG_EOS)
           eos = true;

        if (nDestSize + m_encoded_buffer->nFilledLen > m_nDestAllocSize)
        {
           m_nDestAllocSize = std::max(1024U*1024U, m_nDestAllocSize*2);
           m_pDestBuffer = realloc(m_pDestBuffer, m_nDestAllocSize);
        }
        memcpy((char *)m_pDestBuffer + nDestSize, m_encoded_buffer->pBuffer, m_encoded_buffer->nFilledLen);
        nDestSize += m_encoded_buffer->nFilledLen;
        m_encoded_buffer = NULL;
      }
    }
  }

  Close();

  if(m_omx_decoder.BadState())
    return false;

  pDestBuffer = m_pDestBuffer;
  CLog::Log(LOGDEBUG, "%s::%s : %s %dx%d -> %dx%d\n", CLASSNAME, __func__, srcFile.GetFilename(), srcFile.GetWidth(), srcFile.GetHeight(), maxWidth, maxHeight);

  return true;
}


#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXTexture"

COMXTexture::COMXTexture()
{
}

COMXTexture::~COMXTexture()
{
  Close();
}

void COMXTexture::Close()
{
  CSingleLock lock(m_OMXSection);

  if(m_omx_tunnel_decode.IsInitialized())
  {
    m_omx_tunnel_decode.Flush();
    m_omx_tunnel_decode.Deestablish(false);
  }
  if(m_omx_tunnel_egl.IsInitialized())
  {
    m_omx_tunnel_egl.Flush();
    m_omx_tunnel_egl.Deestablish(false);
  }
  if(m_omx_egl_render.IsInitialized())
  {
    m_omx_egl_render.FlushInput();
    m_omx_egl_render.FlushOutput();
  }
  // delete components
  if(m_omx_decoder.IsInitialized())
    m_omx_decoder.Deinitialize();
  if(m_omx_resize.IsInitialized())
    m_omx_resize.Deinitialize();
  if(m_omx_egl_render.IsInitialized())
    m_omx_egl_render.Deinitialize();
}

bool COMXTexture::HandlePortSettingChange(unsigned int resize_width, unsigned int resize_height, void *egl_image, void *egl_display, bool port_settings_changed)
{
  OMX_ERRORTYPE omx_err;

  assert(!port_settings_changed);

  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_INIT_STRUCTURE(port_def);

  port_def.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  // TODO: jpeg decoder can decimate by factors of 2
  port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  port_def.format.image.nSliceHeight = 16;
  port_def.format.image.nStride = 0;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  port_def.nPortIndex = m_omx_resize.GetInputPort();

  omx_err = m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  port_def.nPortIndex = m_omx_resize.GetOutputPort();
  omx_err = m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_resize.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  port_def.format.image.nFrameWidth = resize_width;
  port_def.format.image.nFrameHeight = resize_height;
  port_def.format.image.nSliceHeight = 16;
  port_def.format.image.nStride = 0;
  omx_err = m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  port_def.nPortIndex = m_omx_egl_render.GetOutputPort();
  omx_err = m_omx_egl_render.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }
  port_def.nBufferCountActual = 1;
  port_def.format.video.pNativeWindow = egl_display;

  omx_err = m_omx_egl_render.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_egl_render.UseEGLImage(&m_egl_buffer, m_omx_egl_render.GetOutputPort(), NULL, egl_image);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.UseEGLImage (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_resize, m_omx_resize.GetInputPort());

  omx_err = m_omx_tunnel_decode.Establish(false);
  if(omx_err != OMX_ErrorNone) {
    CLog::Log(LOGERROR, "%s::%s m_omx_tunnel_decode.Establish (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_omx_tunnel_egl.Initialize(&m_omx_resize, m_omx_resize.GetOutputPort(), &m_omx_egl_render, m_omx_egl_render.GetInputPort());

  omx_err = m_omx_tunnel_egl.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_tunnel_egl.Establish (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_resize.SetStateForComponent(OMX_StateExecuting);
  if(omx_err != OMX_ErrorNone) {
    CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.GetParameter (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_egl_render.SetStateForComponent(OMX_StateExecuting);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.SetStateForComponent (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  return true;
}

bool COMXTexture::Decode(const uint8_t *demuxer_content, unsigned demuxer_bytes, unsigned int width, unsigned int height, void *egl_image, void *egl_display)
{
  CSingleLock lock(m_OMXSection);
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  if(!demuxer_content || !demuxer_bytes)
  {
    CLog::Log(LOGERROR, "%s::%s no input buffer\n", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_decoder.Initialize("OMX.broadcom.image_decode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.Initialize", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_resize.Initialize("OMX.broadcom.resize", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_resize.Initialize", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_egl_render.Initialize("OMX.broadcom.egl_render", OMX_IndexParamVideoInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.Initialize", CLASSNAME, __func__);
    return false;
  }

  OMX_IMAGE_PARAM_PORTFORMATTYPE imagePortFormat;
  OMX_INIT_STRUCTURE(imagePortFormat);
  imagePortFormat.nPortIndex = m_omx_decoder.GetInputPort();
  imagePortFormat.eCompressionFormat = OMX_IMAGE_CodingJPEG;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamImagePortFormat, &imagePortFormat);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.SetParameter (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - Error alloc buffers  (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_sched.SetStateForComponent (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  bool port_settings_changed = false;
  bool eos = false;
  while(demuxer_bytes > 0 || !port_settings_changed || !eos)
  {
    if (demuxer_bytes)
    {
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(1000);
      if(omx_buffer)
      {
        omx_buffer->nOffset = omx_buffer->nFlags  = 0;

        omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
        memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

        demuxer_content += omx_buffer->nFilledLen;
        demuxer_bytes -= omx_buffer->nFilledLen;

        if(demuxer_bytes == 0)
          omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

        omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
        if (omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "%s::%s - m_omx_decoder.OMX_EmptyThisBuffer (%x)", CLASSNAME, __func__, omx_err);
          return false;
         }
      }
    }
    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, 0);
    if(omx_err == OMX_ErrorNone)
    {
      if (!HandlePortSettingChange(width, height, egl_image, egl_display, port_settings_changed))
      {
        CLog::Log(LOGERROR, "%s::%s - HandlePortSettingChange failed (%x)", CLASSNAME, __func__, omx_err);
        return false;
      }
      port_settings_changed = true;
    }
    else if(omx_err == OMX_ErrorStreamCorrupt)
    {
      CLog::Log(LOGERROR, "%s::%s - image not unsupported (%x)", CLASSNAME, __func__, omx_err);
      return false;
    }

    if (port_settings_changed && m_egl_buffer && demuxer_bytes == 0 && !eos)
    {
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_egl_render.GetOutputBuffer();
      if (omx_buffer)
      {
        if (omx_buffer != m_egl_buffer)
        {
          CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.GetOutputBuffer (%p,%p)", CLASSNAME, __func__, omx_buffer, m_egl_buffer);
          return false;
        }

        omx_err = m_omx_egl_render.FillThisBuffer(m_egl_buffer);
        if(omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.FillThisBuffer (%x)", CLASSNAME, __func__, omx_err);
          return false;
        }

        omx_err = m_omx_egl_render.WaitForOutputDone(1000);
        if(omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.WaitForOutputDone result(0x%x)\n", CLASSNAME, __func__, omx_err);
          return false;
        }
        eos = true;
      }
    }
  }
  omx_err = m_omx_egl_render.SendCommand(OMX_CommandPortDisable, m_omx_egl_render.GetOutputPort(), NULL);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.GetParameter (%x)", CLASSNAME, __func__, omx_err);
    return false;
  }
  Close();
  return true;
}
