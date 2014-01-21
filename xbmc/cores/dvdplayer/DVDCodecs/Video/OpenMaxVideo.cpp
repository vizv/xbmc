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

#include "OpenMaxVideo.h"

#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "windowing/WindowingFactory.h"
#include "DVDVideoCodec.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "ApplicationMessenger.h"
#include "Application.h"

#include <IL/OMX_Core.h>
#include <IL/OMX_Component.h>
#include <IL/OMX_Index.h>
#include <IL/OMX_Image.h>

#include "cores/omxplayer/OMXImage.h"

#define DEFAULT_TIMEOUT 1000
//#define OMX_DEBUG_VERBOSE

#define CLASSNAME "COpenMaxVideo"

// DecoderFillBufferDone -- OpenMax output buffer has been filled
static OMX_ERRORTYPE DecoderFillBufferDoneCallback(
  OMX_HANDLETYPE hComponent,
  OMX_PTR pAppData,
  OMX_BUFFERHEADERTYPE* pBuffer)
{
  COpenMaxVideo *ctx = static_cast<COpenMaxVideo*>(pBuffer->pPlatformPrivate);
  return ctx->DecoderFillBufferDone(hComponent, pBuffer);
}

VdpauBufferPool::VdpauBufferPool()
{
}

VdpauBufferPool::~VdpauBufferPool()
{
}

COpenMaxVideo::COpenMaxVideo()
{
  m_portChanging = false;

  memset(&m_videobuffer, 0, sizeof(DVDVideoPicture));
  m_drop_state = false;
  m_decoded_width = 0;
  m_decoded_height = 0;
  m_omx_input_eos = false;
  m_omx_output_eos = false;
  m_videoplayback_done = false;

  m_port_settings_changed = false;
}

COpenMaxVideo::~COpenMaxVideo()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s\n", CLASSNAME, __func__);
  #endif
  if (m_is_open)
    Close();
}

bool COpenMaxVideo::Open(CDVDStreamInfo &hints)
{
  //#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s\n", CLASSNAME, __func__);
  //#endif

  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  std::string decoder_name;

  m_decoded_width  = hints.width;
  m_decoded_height = hints.height;

  m_egl_display = g_Windowing.GetEGLDisplay();
  m_egl_context = g_Windowing.GetEGLContext();

  m_codingType = OMX_VIDEO_CodingUnused;

  switch (hints.codec)
  {
    case AV_CODEC_ID_H264:
      // H.264
      m_codingType = OMX_VIDEO_CodingAVC;
    break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_MPEG4:
      // MPEG-4, DivX 4/5 and Xvid compatible
      m_codingType = OMX_VIDEO_CodingMPEG4;
    break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      // MPEG-2
      m_codingType = OMX_VIDEO_CodingMPEG2;
    break;
    case AV_CODEC_ID_VP6:
      // this form is encoded upside down
      // fall through
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP6A:
      // VP6
      m_codingType = OMX_VIDEO_CodingVP6;
    break;
    case AV_CODEC_ID_VP8:
      // VP8
      m_codingType = OMX_VIDEO_CodingVP8;
    break;
    case AV_CODEC_ID_THEORA:
      // theora
      m_codingType = OMX_VIDEO_CodingTheora;
    break;
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_MJPEGB:
      // mjpg
      m_codingType = OMX_VIDEO_CodingMJPEG;
    break;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
      // VC-1, WMV9
      m_codingType = OMX_VIDEO_CodingWMV;
      break;
    default:
      CLog::Log(LOGERROR, "%s::%s : Video codec unknown: %x", CLASSNAME, __func__, hints.codec);
      return false;
    break;
  }

  // initialize OpenMAX.
  if (!m_omx_decoder.Initialize("OMX.broadcom.video_decode", OMX_IndexParamVideoInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.Initialize", CLASSNAME, __func__);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateIdle);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetStateForComponent omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_VIDEO_PARAM_PORTFORMATTYPE formatType;
  OMX_INIT_STRUCTURE(formatType);
  formatType.nPortIndex = m_omx_decoder.GetInputPort();
  formatType.eCompressionFormat = m_codingType;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamVideoPortFormat, &formatType);
  if (omx_err != OMX_ErrorNone)
    return false;

  OMX_PARAM_PORTDEFINITIONTYPE portParam;
  OMX_INIT_STRUCTURE(portParam);
  portParam.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &portParam);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error OMX_IndexParamPortDefinition omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  portParam.nPortIndex = m_omx_decoder.GetInputPort();
  portParam.nBufferCountActual = 20;
  portParam.format.video.nFrameWidth  = m_decoded_width;
  portParam.format.video.nFrameHeight = m_decoded_height;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &portParam);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error OMX_IndexParamPortDefinition omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  // request portsettingschanged on aspect ratio change
  OMX_CONFIG_REQUESTCALLBACKTYPE notifications;
  OMX_INIT_STRUCTURE(notifications);
  notifications.nPortIndex = m_omx_decoder.GetOutputPort();
  notifications.nIndex = OMX_IndexParamBrcmPixelAspectRatio;
  notifications.bEnable = OMX_TRUE;

  omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexConfigRequestCallback, &notifications);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s OMX_IndexConfigRequestCallback error (0%08x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  if (NaluFormatStartCodes(hints.codec, (uint8_t *)hints.extradata, hints.extrasize))
  {
    OMX_NALSTREAMFORMATTYPE nalStreamFormat;
    OMX_INIT_STRUCTURE(nalStreamFormat);
    nalStreamFormat.nPortIndex = m_omx_decoder.GetInputPort();
    nalStreamFormat.eNaluFormat = OMX_NaluFormatStartCodes;

    omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexParamNalStreamFormatSelect, &nalStreamFormat);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s OMX_IndexParamNalStreamFormatSelect error (0%08x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  // Alloc buffers for the omx input port.
  omx_err = m_omx_decoder.AllocInputBuffers();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s AllocInputBuffers error (0%08x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.SetStateForComponent error (0%08x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  if (!SendDecoderConfig((uint8_t *)hints.extradata, hints.extrasize))
    return false;

  m_is_open = true;
  m_drop_state = false;
  m_videoplayback_done = false;

  return true;
}

void COpenMaxVideo::Deinitialize()
{
  if (m_omx_egl_render.IsInitialized())
    m_omx_egl_render.Deinitialize();
  if (m_omx_decoder.IsInitialized())
    m_omx_decoder.Deinitialize();
}

void COpenMaxVideo::Close()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s\n", CLASSNAME, __func__);
  #endif
  if (m_omx_decoder.IsInitialized())
  {
    if (m_omx_tunnel.IsInitialized())
      m_omx_tunnel.Deestablish();

    //m_omx_egl_render.DisablePort(m_omx_egl_render.GetOutputPort(), true);

    StopDecoder();
    Deinitialize();
  }

  m_is_open = false;
}

void COpenMaxVideo::SetDropState(bool bDrop)
{
  m_drop_state = bDrop;

  if (m_drop_state)
  {
    OMX_ERRORTYPE omx_err;

    // blow all but the last ready video frame
    while (m_omx_output_ready.size() > 1)
    {
      CSingleLock lock(m_bufferPool.renderPicSec);
      m_dts_queue.pop();
      COpenMaxVideoBuffer *buffer = m_omx_output_ready.front();
      OMX_BUFFERHEADERTYPE *omx_buffer = buffer->omx_buffer;
      m_omx_output_ready.pop();
      lock.Leave();
      // return the omx buffer back to OpenMax to fill.
      omx_buffer->nFlags = 0;
      omx_buffer->nFilledLen = 0;
      assert(omx_buffer->nOutputPortIndex == m_omx_egl_render.GetOutputPort());
      CLog::Log(LOGDEBUG, "%s::%s FillThisBuffer(%p) %d (%p)\n", CLASSNAME, __func__, buffer, buffer->refCount, omx_buffer);
      omx_err = m_omx_egl_render.FillThisBuffer(omx_buffer);

      if (omx_err)
        CLog::Log(LOGERROR, "%s::%s - OMX_FillThisBuffer, omx_err(0x%x)\n",
          CLASSNAME, __func__, omx_err);
    }

   #if defined(OMX_DEBUG_VERBOSE)
   CLog::Log(LOGDEBUG, "%s::%s - m_drop_state(%d)\n",
     CLASSNAME, __func__, m_drop_state);
   #endif
  }
}

bool COpenMaxVideo::SendDecoderConfig(uint8_t *extradata, int extrasize)
{
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  /* send decoder config */
  if (extrasize > 0 && extradata != NULL)
  {
    OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();

    if (omx_buffer == NULL)
    {
      CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFilledLen = extrasize;
    if (omx_buffer->nFilledLen > omx_buffer->nAllocLen)
    {
      CLog::Log(LOGERROR, "%s::%s - omx_buffer->nFilledLen > omx_buffer->nAllocLen", CLASSNAME, __func__);
      return false;
    }

    memcpy((unsigned char *)omx_buffer->pBuffer, extradata, omx_buffer->nFilledLen);
    omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;

//CLog::Log(LOGINFO, "%s::%s - Empty(%d,%x)\n", CLASSNAME, __func__, omx_buffer->nFilledLen, omx_buffer->nFlags); CLog::MemDump((char *)omx_buffer->pBuffer, std::min(64U, omx_buffer->nFilledLen));
    omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
  }
  return true;
}

bool COpenMaxVideo::NaluFormatStartCodes(enum AVCodecID codec, uint8_t *extradata, int extrasize)
{
  switch(codec)
  {
    case AV_CODEC_ID_H264:
      if (extrasize < 7 || extradata == NULL)
        return true;
      // valid avcC atom data always starts with the value 1 (version), otherwise annexb
      else if ( *extradata != 1 )
        return true;
    default: break;
  }
  return false;
}

bool COpenMaxVideo::PortSettingsChanged()
{
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  if (m_port_settings_changed)
  {
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
    return false;
  }

  OMX_PARAM_PORTDEFINITIONTYPE port_image;
  OMX_INIT_STRUCTURE(port_image);
  port_image.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_image);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_CONFIG_POINTTYPE pixel_aspect;
  OMX_INIT_STRUCTURE(pixel_aspect);
  pixel_aspect.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamBrcmPixelAspectRatio, &pixel_aspect);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error m_omx_decoder.GetParameter(OMX_IndexParamBrcmPixelAspectRatio) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  if (m_port_settings_changed)
  {
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
    return true;
  }

  OMX_CALLBACKTYPE callbacks = { NULL, NULL, DecoderFillBufferDoneCallback };
  if (!m_omx_egl_render.Initialize("OMX.broadcom.egl_render", OMX_IndexParamVideoInit, &callbacks))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.Initialize", CLASSNAME, __func__);
    return false;
  }

  OMX_CONFIG_PORTBOOLEANTYPE discardMode;
  OMX_INIT_STRUCTURE(discardMode);
  discardMode.nPortIndex = m_omx_egl_render.GetInputPort();
  discardMode.bEnabled = OMX_FALSE;
  omx_err = m_omx_egl_render.SetParameter(OMX_IndexParamBrcmVideoEGLRenderDiscardMode, &discardMode);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error m_omx_egl_render.SetParameter(OMX_IndexParamBrcmVideoEGLRenderDiscardMode) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_omx_egl_render.ResetEos();

  CLog::Log(LOGDEBUG, "%s::%s - %dx%d@%.2f interlace:%d deinterlace:%d", CLASSNAME, __func__,
      port_image.format.video.nFrameWidth, port_image.format.video.nFrameHeight,
      port_image.format.video.xFramerate / (float)(1<<16), 0,0);

  m_omx_tunnel.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_egl_render, m_omx_egl_render.GetInputPort());

  omx_err = m_omx_tunnel.Establish();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  if (!AllocOMXOutputBuffers())
  {
    CLog::Log(LOGERROR, "%s::%s - AllocOMXOutputBuffers failed", CLASSNAME, __func__);
    return false;
  }
  omx_err = m_omx_egl_render.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.SetStateForComponent omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = PrimeFillBuffers();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.PrimeFillBuffers omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_port_settings_changed = true;
  return true;
}


int COpenMaxVideo::Decode(uint8_t* pData, int iSize, double dts, double pts)
{
  if (pData)
  {
    int demuxer_bytes = iSize;
    uint8_t *demuxer_content = pData;

    // we need to queue then de-queue the demux packet, seems silly but
    // omx might not have a omx input buffer available when we are called
    // and we must store the demuxer packet and try again later.
    omx_demux_packet demux_packet;
    demux_packet.dts = dts;
    demux_packet.pts = pts;

    demux_packet.size = demuxer_bytes;
    demux_packet.buff = new OMX_U8[demuxer_bytes];
    memcpy(demux_packet.buff, demuxer_content, demuxer_bytes);
    m_demux_queue.push(demux_packet);

    // try to send any/all demux packets to omx decoder.
    while (m_omx_decoder.GetInputBufferSpace() && !m_demux_queue.empty())
    {
      OMX_ERRORTYPE omx_err;

      demux_packet = m_demux_queue.front();
      m_demux_queue.pop();
      // need to lock here to retreve an input buffer and pop the queue
      unsigned int demuxer_bytes = (unsigned int)demux_packet.size;
      uint8_t *demuxer_content = demux_packet.buff;

      while (demuxer_bytes)
      {
        // 500ms timeout
        OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(500);
        if (omx_buffer == NULL)
        {
          CLog::Log(LOGERROR, "OMXVideo::Decode timeout\n");
          return VC_ERROR;
        }

        omx_buffer->nFlags  = m_omx_input_eos ? OMX_BUFFERFLAG_EOS : 0;
        omx_buffer->nOffset = 0;

        omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
        omx_buffer->nTimeStamp = ToOMXTime((uint64_t)(demux_packet.pts == DVD_NOPTS_VALUE) ? 0 : demux_packet.pts);
        omx_buffer->pAppPrivate = omx_buffer;
        memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

        demuxer_bytes -= omx_buffer->nFilledLen;
        demuxer_content += omx_buffer->nFilledLen;

        if (demuxer_bytes == 0)
          omx_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

        //CLog::Log(LOGINFO, "VideD: dts:%.0f pts:%.0f flags:%x len:%d remain:%d)\n", demux_packet.dts, demux_packet.pts, omx_buffer->nFlags, omx_buffer->nFilledLen, demuxer_bytes);
        omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
        if (omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
          return VC_ERROR;
        }

        omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, 0);
        if (omx_err == OMX_ErrorNone)
        {
          if (!PortSettingsChanged())
          {
            CLog::Log(LOGERROR, "%s::%s - error PortSettingsChanged omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
            return VC_ERROR;
          }
        }
        else if (omx_err != OMX_ErrorTimeout)
        {
          CLog::Log(LOGERROR, "%s::%s - video not supported omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
          return VC_ERROR;
        }
        omx_err = m_omx_decoder.WaitForEvent(OMX_EventParamOrConfigChanged, 0);
        if (omx_err == OMX_ErrorNone)
        {
          if (!PortSettingsChanged())
          {
            CLog::Log(LOGERROR, "%s::%s - error PortSettingsChanged (EventParamOrConfigChanged) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
            return VC_ERROR;
          }
        }
        else if (omx_err == OMX_ErrorStreamCorrupt)
        {
          CLog::Log(LOGERROR, "%s::%s - video not supported 2 omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
          return VC_ERROR;
        }
      }
      delete demux_packet.buff;
      // only push if we are successful with feeding OMX_EmptyThisBuffer
      m_dts_queue.push(demux_packet.dts);

    }
    #if defined(OMX_DEBUG_VERBOSE)
    if (!m_omx_decoder.GetInputBufferSpace())
      CLog::Log(LOGDEBUG,
        "%s::%s - buffering demux, m_demux_queue_size(%d), demuxer_bytes(%d)\n",
        CLASSNAME, __func__, m_demux_queue.size(), demuxer_bytes);
    #endif
  }

  if (m_omx_output_ready.empty())
    return VC_BUFFER;

  return VC_PICTURE | VC_BUFFER;
}

void COpenMaxVideo::Reset(void)
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s\n", CLASSNAME, __func__);
  #endif
  ::Sleep(100);
}

COpenMaxVideoBuffer::COpenMaxVideoBuffer(COpenMaxVideo *omv, CCriticalSection &section)
    : m_omv(omv), refCount(0), renderPicSection(section)
{
  omx_buffer = NULL;
  width = 0;
  height = 0;
  index = 0;
  egl_image = 0;
  texture_id = 0;
  valid = 0;
  fence = 0;
}

COpenMaxVideoBuffer::~COpenMaxVideoBuffer()
{
}


void COpenMaxVideo::ReleaseOpenMaxBuffer(COpenMaxVideoBuffer *buffer)
{
  COpenMaxVideo *ctx = static_cast<COpenMaxVideo*>(buffer->omx_buffer->pPlatformPrivate);
  bool done = buffer->omx_buffer->nFlags & OMX_BUFFERFLAG_EOS;
  if (!done)
  {
    // return the omx buffer back to OpenMax to fill.
    buffer->omx_buffer->nFlags = 0;
    buffer->omx_buffer->nFilledLen = 0;
    assert(buffer->omx_buffer->nOutputPortIndex == ctx->m_omx_egl_render.GetOutputPort());
    CLog::Log(LOGDEBUG, "%s::%s FillThisBuffer(%p) %p->%d (%p)\n", CLASSNAME, __func__, buffer->omx_buffer, buffer, buffer->refCount, (void *)0);
    OMX_ERRORTYPE omx_err = ctx->m_omx_egl_render.FillThisBuffer(buffer->omx_buffer);

    if (omx_err)
      CLog::Log(LOGERROR, "%s::%s - OMX_FillThisBuffer, omx_err(0x%x)\n",
        CLASSNAME, __func__, omx_err);
  }
}

bool COpenMaxVideo::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  //CLog::Log(LOGDEBUG, "%s::%s - m_omx_output_busy.size()=%d m_omx_output_ready.size()=%d\n", CLASSNAME, __func__, m_omx_output_busy.size(), m_omx_output_ready.size());

  while (m_omx_output_busy.size() > 2)
  {
    // fetch a output buffer and pop it off the busy list
    CSingleLock lock(m_bufferPool.renderPicSec);
    COpenMaxVideoBuffer *buffer = m_omx_output_busy.front();
    m_omx_output_busy.pop();
    lock.Leave();
    ReleaseOpenMaxBuffer(buffer);
    CLog::Log(LOGDEBUG, "%s::%s release %p->%d\n", CLASSNAME, __func__, buffer, buffer->refCount);
  }

  if (!m_omx_output_ready.empty())
  {
    COpenMaxVideoBuffer *buffer;
    // fetch a output buffer and pop it off the ready list
    CSingleLock lock(m_bufferPool.renderPicSec);
    buffer = m_omx_output_ready.front();
    m_omx_output_ready.pop();
    m_omx_output_busy.push(buffer);
    lock.Leave();

    pDvdVideoPicture->dts = DVD_NOPTS_VALUE;
    pDvdVideoPicture->pts = DVD_NOPTS_VALUE;
    pDvdVideoPicture->format = RENDER_FMT_OMXEGL;
    pDvdVideoPicture->openMaxBuffer = buffer;

    if (!m_dts_queue.empty())
    {
      pDvdVideoPicture->dts = m_dts_queue.front();
      m_dts_queue.pop();
    }
    // nTimeStamp is in microseconds
    double ts = FromOMXTime(buffer->omx_buffer->nTimeStamp);
    pDvdVideoPicture->pts = (ts == 0) ? DVD_NOPTS_VALUE : ts;
    //CLog::Log(LOGINFO, "VideE: dts:%.0f pts:%.0f openMaxBuffer:%p omx_buffer:%p egl_image:%p texture_id:%x\n",      pDvdVideoPicture->dts, pDvdVideoPicture->pts, pDvdVideoPicture->openMaxBuffer, pDvdVideoPicture->openMaxBuffer->omx_buffer, pDvdVideoPicture->openMaxBuffer->egl_image, pDvdVideoPicture->openMaxBuffer->texture_id);
  }
  #if defined(OMX_DEBUG_VERBOSE)
  else
  {
    CLog::Log(LOGDEBUG, "%s::%s - called but m_omx_output_ready is empty\n",
      CLASSNAME, __func__);
  }
  #endif

  pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
  pDvdVideoPicture->iFlags |= m_drop_state ? DVP_FLAG_DROPPED : 0;

  CLog::Log(LOGDEBUG, "%s::%s acquire %p->%d (%p)\n", CLASSNAME, __func__, pDvdVideoPicture->openMaxBuffer, pDvdVideoPicture->openMaxBuffer->refCount, pDvdVideoPicture);
  return true;
}

// DecoderFillBufferDone -- OpenMax output buffer has been filled
OMX_ERRORTYPE COpenMaxVideo::DecoderFillBufferDone(
  OMX_HANDLETYPE hComponent,
  OMX_BUFFERHEADERTYPE* pBuffer)
{
  COpenMaxVideoBuffer *buffer = (COpenMaxVideoBuffer*)pBuffer->pAppPrivate;

  //#if defined(OMX_DEBUG_FILLBUFFERDONE)
  CLog::Log(LOGDEBUG, "%s::%s - %p (%p,%p) buffer_size(%u), timestamp(%.0f)\n",
    CLASSNAME, __func__, buffer, pBuffer, buffer->omx_buffer, pBuffer->nFilledLen, (double)FromOMXTime(buffer->omx_buffer->nTimeStamp));
  //#endif

  if (!m_portChanging)
  {
    // queue output omx buffer to ready list.
    CSingleLock lock(m_bufferPool.renderPicSec);
    m_omx_output_ready.push(buffer);
    lock.Leave();
  }

  return OMX_ErrorNone;
}

OMX_ERRORTYPE COpenMaxVideo::PrimeFillBuffers(void)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  COpenMaxVideoBuffer *buffer;

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s\n", CLASSNAME, __func__);
  #endif
  // tell OpenMax to start filling output buffers
  for (size_t i = 0; i < m_bufferPool.allRenderPics.size(); i++)
  {
    buffer = m_bufferPool.allRenderPics[i];
    // always set the port index.
    buffer->omx_buffer->nOutputPortIndex = m_omx_egl_render.GetOutputPort();
    // Need to clear the EOS flag.
    //buffer->omx_buffer->nFlags &= ~OMX_BUFFERFLAG_EOS;
    buffer->omx_buffer->pAppPrivate = buffer;
    buffer->omx_buffer->pPlatformPrivate = this;
    buffer->omx_buffer->nFlags = 0;
    buffer->omx_buffer->nFilledLen = 0;

    CLog::Log(LOGDEBUG, "%s::%s FillThisBuffer(%p) %d (%p)\n", CLASSNAME, __func__, buffer, buffer->refCount, buffer->omx_buffer);
    omx_err = m_omx_egl_render.FillThisBuffer(buffer->omx_buffer);
    if (omx_err)
      CLog::Log(LOGERROR, "%s::%s - OMX_FillThisBuffer failed with omx_err(0x%x)\n",
        CLASSNAME, __func__, omx_err);
  }

  return omx_err;
}

OMX_ERRORTYPE COpenMaxVideo::FreeOMXInputBuffers(void)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  /*
  omx_err = OMX_SendCommand(m_omx_decoder, OMX_CommandFlush, m_omx_input_port, 0);
  if (omx_err)
    CLog::Log(LOGERROR, "%s::%s - OMX_CommandFlush failed with omx_err(0x%x)\n",
      CLASSNAME, __func__, omx_err);
  else if (wait)
    sem_wait(m_omx_flush_input);
  */

  // free omx input port buffers.
  //m_omx_decoder.FreeInputBuffers();

  // empty input buffer queue. not decoding so don't need lock/unlock.
  while (!m_demux_queue.empty())
    m_demux_queue.pop();
  while (!m_dts_queue.empty())
    m_dts_queue.pop();

  return(omx_err);
}

bool COpenMaxVideo::CallbackAllocOMXEGLTextures(void *userdata)
{
  COpenMaxVideo *omx = static_cast<COpenMaxVideo*>(userdata);
  return omx->AllocOMXOutputEGLTextures() == OMX_ErrorNone;
}

bool COpenMaxVideo::CallbackFreeOMXEGLTextures(void *userdata)
{
  COpenMaxVideo *omx = static_cast<COpenMaxVideo*>(userdata);
  return omx->FreeOMXOutputEGLTextures() == OMX_ErrorNone;
}

bool COpenMaxVideo::AllocOMXOutputBuffers(void)
{
  return g_OMXImage.SendMessage(CallbackAllocOMXEGLTextures, (void *)this);
}

bool COpenMaxVideo::FreeOMXOutputBuffers(void)
{
  return g_OMXImage.SendMessage(CallbackFreeOMXEGLTextures, (void *)this);
}

OMX_ERRORTYPE COpenMaxVideo::AllocOMXOutputEGLTextures(void)
{
  OMX_ERRORTYPE omx_err;

  EGLint attrib = EGL_NONE;
  COpenMaxVideoBuffer *egl_buffer;

  // Obtain the information about the output port.
  OMX_PARAM_PORTDEFINITIONTYPE port_format;
  OMX_INIT_STRUCTURE(port_format);
  port_format.nPortIndex = m_omx_egl_render.GetOutputPort();
  omx_err = m_omx_egl_render.GetParameter(OMX_IndexParamPortDefinition, &port_format);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.GetParameter OMX_IndexParamPortDefinition omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  port_format.nBufferCountActual = 4;
  omx_err = m_omx_egl_render.SetParameter(OMX_IndexParamPortDefinition, &port_format);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.SetParameter OMX_IndexParamPortDefinition omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG,
    "%s::%s (1) - oport(%d), nFrameWidth(%u), nFrameHeight(%u), nStride(%x), nBufferCountMin(%u), nBufferCountActual(%u), nBufferSize(%u)\n",
    CLASSNAME, __func__, m_omx_egl_render.GetOutputPort(),
    port_format.format.video.nFrameWidth, port_format.format.video.nFrameHeight,port_format.format.video.nStride,
    port_format.nBufferCountMin, port_format.nBufferCountActual, port_format.nBufferSize);
  #endif

  glActiveTexture(GL_TEXTURE0);

  omx_err =  m_omx_egl_render.EnablePort(m_omx_egl_render.GetOutputPort(), false);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.EnablePort omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  for (size_t i = 0; i < port_format.nBufferCountActual; i++)
  {
    egl_buffer = new COpenMaxVideoBuffer(this, m_bufferPool.renderPicSec);
    egl_buffer->width  = m_decoded_width;
    egl_buffer->height = m_decoded_height;

    glGenTextures(1, &egl_buffer->texture_id);
    glBindTexture(GL_TEXTURE_2D, egl_buffer->texture_id);

    // create space for buffer with a texture
    glTexImage2D(
      GL_TEXTURE_2D,      // target
      0,                  // level
      GL_RGBA,            // internal format
      m_decoded_width,    // width
      m_decoded_height,   // height
      0,                  // border
      GL_RGBA,            // format
      GL_UNSIGNED_BYTE,   // type
      NULL);              // pixels -- will be provided later

    // create EGLImage from texture
    egl_buffer->egl_image = eglCreateImageKHR(
      m_egl_display,
      m_egl_context,
      EGL_GL_TEXTURE_2D_KHR,
      (EGLClientBuffer)(egl_buffer->texture_id),
      &attrib);
    if (!egl_buffer->egl_image)
    {
      CLog::Log(LOGERROR, "%s::%s - ERROR creating EglImage\n", CLASSNAME, __func__);
      return(OMX_ErrorUndefined);
    }
    egl_buffer->index = i;

    // tell decoder output port that it will be using EGLImage
    omx_err = m_omx_egl_render.UseEGLImage(
      &egl_buffer->omx_buffer, m_omx_egl_render.GetOutputPort(), egl_buffer, egl_buffer->egl_image);
    if (omx_err)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_UseEGLImage failed with omx_err(0x%x)\n",
        CLASSNAME, __func__, omx_err);
      return(omx_err);
    }
    m_bufferPool.allRenderPics.push_back(egl_buffer);

    CLog::Log(LOGDEBUG, "%s::%s - Texture %p Width %d Height %d\n",
      CLASSNAME, __func__, egl_buffer->egl_image, egl_buffer->width, egl_buffer->height);
  }
  omx_err = m_omx_egl_render.WaitForCommand(OMX_CommandPortEnable, m_omx_egl_render.GetOutputPort());
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.WaitForCommand(OMX_CommandPortEnable) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);

  m_omx_output_eos = false;
  CSingleLock lock(m_bufferPool.renderPicSec);
  while (!m_omx_output_busy.empty())
    m_omx_output_busy.pop();
  while (!m_omx_output_ready.empty())
    m_omx_output_ready.pop();
  lock.Leave();

  return omx_err;
}

OMX_ERRORTYPE COpenMaxVideo::FreeOMXOutputEGLTextures(void)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  COpenMaxVideoBuffer *egl_buffer;

  omx_err = m_omx_egl_render.DisablePort(m_omx_egl_render.GetOutputPort(), false);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.DisablePort(%d) omx_err(0x%08x)\n", CLASSNAME, __func__, m_omx_egl_render.GetOutputPort(), omx_err);

  for (size_t i = 0; i < m_bufferPool.allRenderPics.size(); i++)
  {
    egl_buffer = m_bufferPool.allRenderPics[i];
    // tell decoder output port to stop using the EGLImage
    omx_err = m_omx_egl_render.FreeOutputBuffer(egl_buffer->omx_buffer);
    if (omx_err != OMX_ErrorNone)
      CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.FreeOutputBuffer(%p) omx_err(0x%08x)\n", CLASSNAME, __func__, egl_buffer->omx_buffer, omx_err);
    // destroy egl_image
    eglDestroyImageKHR(m_egl_display, egl_buffer->egl_image);
    // free texture
    glDeleteTextures(1, &egl_buffer->texture_id);
    delete egl_buffer;
  }
  m_bufferPool.allRenderPics.clear();
  omx_err = m_omx_egl_render.WaitForCommand(OMX_CommandPortDisable, m_omx_egl_render.GetOutputPort());
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s WaitForCommand:OMX_CommandPortDisable omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);

  return omx_err;
}


// StopPlayback -- Stop video playback
OMX_ERRORTYPE COpenMaxVideo::StopDecoder(void)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s\n", CLASSNAME, __func__);
  #endif
#if 1
  // transition decoder component from executing to idle
  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateIdle);
  if (omx_err)
  {
    CLog::Log(LOGERROR, "%s::%s - setting OMX_StateIdle failed with omx_err(0x%x)\n",
      CLASSNAME, __func__, omx_err);
    return omx_err;
  }
  omx_err = m_omx_egl_render.SetStateForComponent(OMX_StateIdle);
  if (omx_err)
  {
    CLog::Log(LOGERROR, "%s::%s - setting egl OMX_StateIdle failed with omx_err(0x%x)\n",
      CLASSNAME, __func__, omx_err);
    return omx_err;
  }
#endif
  // we can free our allocated port buffers in OMX_StateIdle state.
  // free OpenMax input buffers.
  FreeOMXInputBuffers();
  // free OpenMax output buffers.
  FreeOMXOutputBuffers();

#if 0
  // transition decoder component from idle to loaded
  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateLoaded);
  if (omx_err)
    CLog::Log(LOGERROR,
      "%s::%s - setting OMX_StateLoaded failed with omx_err(0x%x)\n",
      CLASSNAME, __func__, omx_err);
  omx_err = m_omx_egl_render.SetStateForComponent(OMX_StateLoaded);
  if (omx_err)
    CLog::Log(LOGERROR,
      "%s::%s - setting OMX_StateLoaded egl failed with omx_err(0x%x)\n",
      CLASSNAME, __func__, omx_err);
#endif
  return omx_err;
}

