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
#include "settings/Settings.h"
#include "settings/MediaSettings.h"
#include "ApplicationMessenger.h"
#include "Application.h"
#include "threads/Atomics.h"
#include "guilib/GUIWindowManager.h"
#include "cores/VideoRenderers/RenderFlags.h"
#include "settings/DisplaySettings.h"
#include "cores/VideoRenderers/RenderManager.h"

#include "linux/RBP.h"

#define DTS_QUEUE

#define DEFAULT_TIMEOUT 1000
#ifdef _DEBUG
#define OMX_DEBUG_VERBOSE
#endif

#define CLASSNAME "COpenMaxVideoBuffer"

COpenMaxVideoBuffer::COpenMaxVideoBuffer(COpenMaxVideo *omv)
    : m_omv(omv), m_refs(0)
{
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  mmal_buffer = NULL;
  width = 0;
  height = 0;
  index = 0;
  m_aspect_ratio = 0.0f;
  dts = DVD_NOPTS_VALUE;
}

COpenMaxVideoBuffer::~COpenMaxVideoBuffer()
{
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
}


COpenMaxVideoBuffer* COpenMaxVideoBuffer::Acquire()
{
  long count = AtomicIncrement(&m_refs);
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%p) ref:%ld", CLASSNAME, __func__, this, mmal_buffer, count);
  #endif
  (void)count;
  return this;
}

void COpenMaxVideoBuffer::Render()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%p) index:%d", CLASSNAME, __func__, this, mmal_buffer, index);
  #endif

  m_omv->Render(this, index++);
}

long COpenMaxVideoBuffer::Release()
{
  long count = AtomicDecrement(&m_refs);
#if defined(OMX_DEBUG_VERBOSE)
CLog::Log(LOGDEBUG, "%s::%s %p (%p) ref:%ld", CLASSNAME, __func__, this, mmal_buffer, count);
#endif
  if (count == 0)
  {
    m_omv->ReleaseOpenMaxBuffer(this);
  }
  return count;
}

#undef CLASSNAME
#define CLASSNAME "COpenMaxVideo"

COpenMaxVideo::COpenMaxVideo()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  #endif
  pthread_mutex_init(&m_omx_output_mutex, NULL);

  m_drop_state = false;
  m_decoded_width = 0;
  m_decoded_height = 0;

  m_finished = false;
  m_pFormatName = "omx-xxxx";

  m_deinterlace_enabled = false;
  m_deinterlace_request = VS_DEINTERLACEMODE_OFF;
  m_startframe = false;
  m_decoderPts = DVD_NOPTS_VALUE;
  m_droppedPics = 0;
  m_decode_frame_number = 1;
  m_skipDeinterlaceFields = false;


  m_dec = NULL;
  m_dec_input = NULL;
  m_dec_output = NULL;
  m_dec_input_pool = NULL;

  m_vout = NULL;
  m_vout_input = NULL;
  m_vout_input_pool = NULL;

  m_format = NULL;

  m_deinterlace = NULL;
  m_deinterlace_input = NULL;
  m_deinterlace_output = NULL;
  m_deinterlace_input_pool = NULL;
  m_deinterlaced = NULL;
  m_codingType = 0;

  m_src_rect.SetRect(0, 0, 0, 0);
  m_dst_rect.SetRect(0, 0, 0, 0);
  m_video_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_display_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_StereoInvert = false;

  pthread_mutex_init(&m_mutex, NULL);
  pthread_cond_init(&m_cond, NULL);

}

COpenMaxVideo::~COpenMaxVideo()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  #endif
  assert(m_finished);


  while (!m_demux_queue.empty())
    m_demux_queue.pop();
#ifdef DTS_QUEUE
  while (!m_dts_queue.empty())
    m_dts_queue.pop();
#endif

  pthread_mutex_destroy(&m_omx_output_mutex);


  pthread_cond_destroy(&m_cond);
  pthread_mutex_destroy(&m_mutex);

  if (m_dec) {
    mmal_component_disable(m_dec);
    mmal_port_disable(m_dec->control);
  }

  if (m_dec_input)
    mmal_port_disable(m_dec_input);

  if (m_dec_output)
    mmal_port_disable(m_dec_output);

  if (m_vout) {
    mmal_component_disable(m_vout);
    mmal_port_disable(m_vout->control);
  }

  if (m_vout_input)
    mmal_port_disable(m_vout_input);

  if (m_dec_input_pool)
    mmal_pool_destroy(m_dec_input_pool);

  if (m_vout_input_pool)
    mmal_pool_destroy(m_vout_input_pool);

  if (m_vout)
    mmal_component_release(m_vout);

  if (m_deinterlace)
    mmal_component_release(m_deinterlace);

  if (m_dec)
    mmal_component_release(m_dec);

  if (m_format)
    mmal_format_free(m_format);
}

static void dec_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  MMAL_STATUS_T status;

  if (buffer->cmd == MMAL_EVENT_ERROR)
  {
    status = (MMAL_STATUS_T)*(uint32_t *)buffer->data;
    CLog::Log(LOGERROR, "%s::%s Error (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
  }

  mmal_buffer_header_release(buffer);
}

static void dec_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s port:%p buffer %p, len %d cmd:%x", CLASSNAME, __func__, port, buffer, buffer->length, buffer->cmd);
#endif
  mmal_buffer_header_release(buffer);
}


void COpenMaxVideo::dec_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
#if defined(OMX_DEBUG_VERBOSE)
  if (!(buffer->cmd == 0 && buffer->length > 0))
    CLog::Log(LOGDEBUG, "%s::%s port:%p buffer %p, len %d cmd:%x", CLASSNAME, __func__, port, buffer, buffer->length, buffer->cmd);
#endif
  bool kept = false;

  if (buffer->cmd == 0)
  {
    if (buffer->length > 0)
    {
      //assert(buffer->user_data == NULL);
      COpenMaxVideoBuffer *omvb = new COpenMaxVideoBuffer(this);
      omvb->mmal_buffer = buffer;
      buffer->user_data = (void *)omvb;

      assert(!(buffer->flags & MMAL_BUFFER_HEADER_FLAG_DECODEONLY));

      #ifdef DTS_QUEUE
      pthread_mutex_lock(&m_omx_output_mutex);
      if (!m_dts_queue.empty())
      {
        omvb->dts = m_dts_queue.front();
        m_dts_queue.pop();
      }
      else assert(0);
      pthread_mutex_unlock(&m_omx_output_mutex);
      #endif

#if defined(OMX_DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s::%s - %p (%p) buffer_size(%u) dts:%.3f pts:%.3f flags:%x:%x frame:%d",
        CLASSNAME, __func__, buffer, omvb, buffer->length, omvb->dts*1e-6, buffer->pts*1e-6, buffer->flags, buffer->type->video.flags, (int)buffer->user_data);
#endif

      if (m_drop_state)
      {
        CLog::Log(LOGDEBUG, "%s::%s - dropping %p (%p) (drop:%d)", CLASSNAME, __func__, omvb, buffer, m_drop_state);
      }
      else
      {
        omvb->width = m_decoded_width;
        omvb->height = m_decoded_height;
        omvb->m_aspect_ratio = m_aspect_ratio;
        pthread_mutex_lock(&m_omx_output_mutex);
        m_omx_output_ready.push(omvb);
        pthread_mutex_unlock(&m_omx_output_mutex);
        kept = true;
      }
    }
  }
  else if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED)
  {
    MMAL_EVENT_FORMAT_CHANGED_T *fmt = mmal_event_format_changed_get(buffer);
    MMAL_ES_FORMAT_T *format = mmal_format_alloc();
    mmal_format_full_copy(format, fmt->format);
    format->encoding = MMAL_ENCODING_OPAQUE;
    m_format = format;
    if (m_format->es->video.par.num && m_format->es->video.par.den)
      m_aspect_ratio = (float)m_format->es->video.par.num / m_format->es->video.par.den;
    m_decoded_width = m_format->es->video.width;
    m_decoded_height = m_format->es->video.height;
    CLog::Log(LOGDEBUG, "%s::%s format changed: %dx%d %.2f", CLASSNAME, __func__, m_decoded_width, m_decoded_height, m_aspect_ratio);
  }
  if (!kept)
    mmal_buffer_header_release(buffer);
}

static void dec_output_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  COpenMaxVideo *omx = reinterpret_cast<COpenMaxVideo*>(port->userdata);
  omx->dec_output_port_cb(port, buffer);
}

static void vout_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  mmal_buffer_header_release(buffer);
}

void COpenMaxVideo::vout_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  COpenMaxVideoBuffer *omvb = (COpenMaxVideoBuffer *)buffer->user_data;

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s port:%p buffer %p (%p), len %d cmd:%x", CLASSNAME, __func__, port, buffer, omvb, buffer->length, buffer->cmd);
  #endif
  omvb->Release();
}

static void vout_input_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  COpenMaxVideo *omx = reinterpret_cast<COpenMaxVideo*>(port->userdata);
  omx->vout_input_port_cb(port, buffer);
}

static void* pool_allocator_alloc(void *context, uint32_t size)
{
  return mmal_port_payload_alloc((MMAL_PORT_T *)context, size);
}

static void pool_allocator_free(void *context, void *mem)
{
  mmal_port_payload_free((MMAL_PORT_T *)context, (uint8_t *)mem);
}


int COpenMaxVideo::change_output_format()
{
  MMAL_STATUS_T status;
  int ret = 0;
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  status = mmal_port_disable(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to disable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  mmal_format_full_copy(m_dec_output->format, m_format);
  status = mmal_port_format_commit(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit output format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  m_dec_output->buffer_num = 40;
  m_dec_output->buffer_size = m_dec_output->buffer_size_min;
  status = mmal_port_enable(m_dec_output, dec_output_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  // todo: deinterlace

  /* Create video renderer */
  /* FIXME: Should we move this to format change of deinterlace plugin?
   * */
  status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &m_vout);
  if(status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create vout component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  m_vout->control->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_vout->control, vout_control_port_cb);
  if(status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable vout control port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  m_vout_input = m_vout->input[0];
  m_vout_input->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  mmal_format_full_copy(m_vout_input->format, m_format);
  m_vout_input->buffer_num = m_dec_output->buffer_num;
  status = mmal_port_format_commit(m_vout_input);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit vout input format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  status = mmal_port_enable(m_vout_input, vout_input_port_cb_static);
  if(status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to vout enable input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  status = mmal_component_enable(m_vout);
  if(status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable vout component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  m_vout_input_pool = mmal_pool_create_with_allocator(m_vout_input->buffer_num, m_vout_input->buffer_size, m_vout_input, pool_allocator_alloc, pool_allocator_free);
  if(!m_vout_input_pool)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create pool for vout input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = EXIT_FAILURE;
    goto out;
  }

out:
    return ret;
}

static void RenderUpdateCallBack(const void *ctx, const CRect &SrcRect, const CRect &DestRect)
{
  COpenMaxVideo *omv= (COpenMaxVideo *)ctx;
  omv->SetVideoRect(SrcRect, DestRect);
}

bool COpenMaxVideo::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options, OpenMaxVideoPtr myself)
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s useomx:%d software:%d", CLASSNAME, __func__, CSettings::Get().GetBool("videoplayer.useomx"), hints.software);
  #endif

  // we always qualify even if DVDFactoryCodec does this too.
  if (!CSettings::Get().GetBool("videoplayer.useomx") || hints.software)
    return false;

  m_hints = hints;
  MMAL_STATUS_T status;
  MMAL_PARAMETER_BOOLEAN_T error_concealment;

  m_deinterlace_request = CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode;

  m_myself = myself;
  m_decoded_width  = hints.width;
  m_decoded_height = hints.height;
  m_forced_aspect_ratio = hints.forced_aspect;
  m_aspect_ratio = hints.aspect;

  switch (hints.codec)
  {
    case AV_CODEC_ID_H264:
      // H.264
      m_codingType = MMAL_ENCODING_H264;
      m_pFormatName = "omx-h264";
    break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_MPEG4:
      // MPEG-4, DivX 4/5 and Xvid compatible
      m_codingType = MMAL_ENCODING_MP4V;
      m_pFormatName = "omx-mpeg4";
    break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      // MPEG-2
      m_codingType = MMAL_ENCODING_MP2V;
      m_pFormatName = "omx-mpeg2";
    break;
    case AV_CODEC_ID_VP6:
      // this form is encoded upside down
      // fall through
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP6A:
      // VP6
      m_codingType = MMAL_ENCODING_VP6;
      m_pFormatName = "omx-vp6";
    break;
    case AV_CODEC_ID_VP8:
      // VP8
      m_codingType = MMAL_ENCODING_VP8;
      m_pFormatName = "omx-vp8";
    break;
    case AV_CODEC_ID_THEORA:
      // theora
      m_codingType = MMAL_ENCODING_THEORA;
      m_pFormatName = "omx-theora";
    break;
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_MJPEGB:
      // mjpg
      m_codingType = MMAL_ENCODING_MJPEG;
      m_pFormatName = "omx-mjpg";
    break;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
      // VC-1, WMV9
      m_codingType = MMAL_ENCODING_WVC1;
      m_pFormatName = "omx-vc1";
      break;
    default:
      CLog::Log(LOGERROR, "%s::%s : Video codec unknown: %x", CLASSNAME, __func__, hints.codec);
      return false;
    break;
  }

  if ( (m_codingType == MMAL_ENCODING_MP2V && !g_RBP.GetCodecMpg2() ) ||
       (m_codingType == MMAL_ENCODING_WVC1   && !g_RBP.GetCodecWvc1() ) )
  {
    CLog::Log(LOGWARNING, "%s::%s Codec %s is not supported", CLASSNAME, __func__, m_pFormatName);
    return false;
  }

  // initialize mmal.
  status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &m_dec);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create MMAL decoder component %s (status=%x %s)", CLASSNAME, __func__, MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
    return false;
  }

  m_dec->control->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_dec->control, dec_control_port_cb);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder control port %s (status=%x %s)", CLASSNAME, __func__, MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_input = m_dec->input[0];

  m_dec_input->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  m_dec_input->format->type = MMAL_ES_TYPE_VIDEO;
  m_dec_input->format->encoding = m_codingType;
  //m_dec_input->format->es->video.width = hints.width;
  //m_dec_input->format->es->video.height = hints.height;
  m_dec_input->format->flags |= MMAL_ES_FORMAT_FLAG_FRAMED;

  error_concealment.hdr.id = MMAL_PARAMETER_VIDEO_DECODE_ERROR_CONCEALMENT;
  error_concealment.hdr.size = sizeof(MMAL_PARAMETER_BOOLEAN_T);
  error_concealment.enable = MMAL_FALSE;
  status = mmal_port_parameter_set(m_dec_input, &error_concealment.hdr);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to disable error concealment on %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));

  status = mmal_port_parameter_set_uint32(m_dec_input, MMAL_PARAMETER_EXTRA_BUFFERS, GetAllowedReferences() + 4);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to enable extra buffers on %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));

  status = mmal_format_extradata_alloc(m_dec_input->format, hints.extrasize);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s failed to allocate extradata", CLASSNAME, __func__);
    return false;
  }
  m_dec_input->format->extradata_size = hints.extrasize;
  if (m_dec_input->format->extradata_size)
     memcpy(m_dec_input->format->extradata, hints.extradata, m_dec_input->format->extradata_size);

  status = mmal_port_format_commit(m_dec_input);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit format for decoder input port %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));
    return false;
  }
  m_dec_input->buffer_size = m_dec_input->buffer_size_recommended;
  m_dec_input->buffer_num = m_dec_input->buffer_num_recommended;

  status = mmal_port_enable(m_dec_input, dec_input_port_cb);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder input port %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_output = m_dec->output[0];
  m_dec_output->userdata = (struct MMAL_PORT_USERDATA_T *)this;

  status = mmal_port_enable(m_dec_output, dec_output_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder output port %s (status=%x %s)", CLASSNAME, __func__, m_dec_output->name, status, mmal_status_to_string(status));
    return false;
  }

  status = mmal_component_enable(m_dec);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder component %s (status=%x %s)", CLASSNAME, __func__, m_dec->name, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_input_pool = mmal_pool_create_with_allocator(m_dec_input->buffer_num, m_dec_input->buffer_size, m_dec_input, pool_allocator_alloc, pool_allocator_free);
  if (!m_dec_input_pool)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create pool for decoder input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  m_drop_state = false;
  m_startframe = false;

  return true;
}

void COpenMaxVideo::Dispose()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  #endif
  // we are happy to exit, but let last shared pointer being deleted trigger the destructor
  bool done = false;
  pthread_mutex_lock(&m_omx_output_mutex);
  if (m_omx_output_busy.empty())
    done = true;
  m_finished = true;
  pthread_mutex_unlock(&m_omx_output_mutex);
  if (done)
  {
    assert(m_dts_queue.empty());
    m_myself.reset();
  }
}

void COpenMaxVideo::SetDropState(bool bDrop)
{
#if defined(OMX_DEBUG_VERBOSE)
  if (m_drop_state != bDrop)
    CLog::Log(LOGDEBUG, "%s::%s - m_drop_state(%d)",
      CLASSNAME, __func__, bDrop);
#endif
  // xxx m_drop_state = bDrop;
  if (m_drop_state)
  {
    while (1)
    {
      COpenMaxVideoBuffer *buffer = NULL;
      pthread_mutex_lock(&m_omx_output_mutex);
      // fetch a output buffer and pop it off the ready list
      if (!m_omx_output_ready.empty())
      {
        buffer = m_omx_output_ready.front();
        m_omx_output_ready.pop();
      }
      pthread_mutex_unlock(&m_omx_output_mutex);
      if (buffer)
        ReturnOpenMaxBuffer(buffer);
      else
        break;
    }
  }
}

int COpenMaxVideo::Decode(uint8_t* pData, int iSize, double dts, double pts)
{
  #if defined(OMX_DEBUG_VERBOSE)
  //CLog::Log(LOGDEBUG, "%s::%s - %-8p %-6d dts:%.3f pts:%.3f demux_queue(%d) dts_queue(%d) ready_queue(%d) busy_queue(%d)",
  //   CLASSNAME, __func__, pData, iSize, dts == DVD_NOPTS_VALUE ? 0.0 : dts*1e-6, pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6, m_demux_queue.size(), m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy.size());
  #endif

  unsigned int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  MMAL_BUFFER_HEADER_T *buffer;
  MMAL_STATUS_T status;

  // we need to queue then de-queue the demux packet, seems silly but
  // omx might not have a omx input buffer available when we are called
  // and we must store the demuxer packet and try again later.
  while (demuxer_bytes)
  {
      // 500ms timeout
      buffer = mmal_queue_timedwait(m_dec_input_pool->queue, 500);
      if (!buffer)
      {
        CLog::Log(LOGERROR, "%s::%s - mmal_queue_get failed", CLASSNAME, __func__);
        return VC_ERROR;
      }

      mmal_buffer_header_reset(buffer);
      buffer->cmd = 0;
      buffer->pts = pts == DVD_NOPTS_VALUE ? MMAL_TIME_UNKNOWN : pts;
      buffer->dts = dts == DVD_NOPTS_VALUE ? MMAL_TIME_UNKNOWN : dts;
      buffer->length = demuxer_bytes > buffer->alloc_size ? buffer->alloc_size : demuxer_bytes;
      buffer->user_data = (void *)m_decode_frame_number;

      if (m_drop_state)
      {
        // Request decode only (maintain ref frames, but don't return a picture)
        buffer->flags |= MMAL_BUFFER_HEADER_FLAG_DECODEONLY;
        m_droppedPics += m_deinterlace_enabled ? 2:1;
      }
      memcpy(buffer->data, demuxer_content, buffer->length);
      demuxer_bytes   -= buffer->length;
      demuxer_content += buffer->length;

      if (demuxer_bytes == 0)
        buffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;

      #if defined(OMX_DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s::%s - %-8p %-6d dts:%.3f pts:%.3f flags:%x frame:%d demux_queue(%d) dts_queue(%d) ready_queue(%d) busy_queue(%d)",
         CLASSNAME, __func__, buffer, iSize, dts == DVD_NOPTS_VALUE ? 0.0 : dts*1e-6, pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6, buffer->flags, (int)buffer->user_data, m_demux_queue.size(), m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy.size());
      #endif
      status = mmal_port_send_buffer(m_dec_input, buffer);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed send buffer to decoder input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return VC_ERROR;
      }

      if (demuxer_bytes == 0)
      {
        m_decode_frame_number++;
        m_startframe = true;
#ifdef DTS_QUEUE
        if (!m_drop_state)
        {
          // only push if we are successful with feeding OMX_EmptyThisBuffer
          pthread_mutex_lock(&m_omx_output_mutex);
          m_dts_queue.push(dts);
          //assert(m_dts_queue.size() < 100);
          pthread_mutex_unlock(&m_omx_output_mutex);
        }
#endif
        if (m_format && !m_vout_input_pool)
        {
          if (change_output_format() < 0)
          {
            CLog::Log(LOGERROR, "%s::%s - change_output_format() failed", CLASSNAME, __func__);
            return VC_ERROR;
          }
          while (buffer = mmal_queue_get(m_vout_input_pool->queue), buffer)
          {
            mmal_buffer_header_reset(buffer);
            buffer->cmd = 0;
            #if defined(OMX_DEBUG_VERBOSE)
            CLog::Log(LOGDEBUG, "%s::%s Send buffer %p from pool to decoder output port %p", CLASSNAME, __func__, buffer, m_dec_output);
            #endif
            status = mmal_port_send_buffer(m_dec_output, buffer);
            if (status != MMAL_SUCCESS)
            {
              CLog::Log(LOGERROR, "%s::%s - Failed send buffer to decoder output port (status=0%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
              return VC_ERROR;
            }
          }
        }
      }
  }
  if (m_omx_output_ready.empty())
  {
    //CLog::Log(LOGDEBUG, "%s::%s - empty: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());
    return VC_BUFFER;
  }

  //CLog::Log(LOGDEBUG, "%s::%s -  full: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());
  return VC_PICTURE;
}

void COpenMaxVideo::Reset(void)
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  #endif
  mmal_port_flush(m_dec_input);
  mmal_port_flush(m_dec_output);
  //mmal_port_flush(m_vout_input);

  // blow all ready video frames
  SetDropState(true);
  SetDropState(false);
#ifdef DTS_QUEUE
  pthread_mutex_lock(&m_omx_output_mutex);
  while (!m_dts_queue.empty())
    m_dts_queue.pop();
  pthread_mutex_unlock(&m_omx_output_mutex);
#endif

  while (!m_demux_queue.empty())
    m_demux_queue.pop();
  m_startframe = false;
  m_decoderPts = DVD_NOPTS_VALUE;
  m_droppedPics = 0;
  m_decode_frame_number = 1;
}


void COpenMaxVideo::ReturnOpenMaxBuffer(COpenMaxVideoBuffer *buffer)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%d)", CLASSNAME, __func__, buffer, m_omx_output_busy.size());
#endif

  bool done = buffer->mmal_buffer->flags & OMX_BUFFERFLAG_EOS;
  if (!done)
  {
    mmal_buffer_header_release(buffer->mmal_buffer);

#if defined(OMX_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s::%s FillThisBuffer(%p) %p->%ld", CLASSNAME, __func__, buffer, buffer->mmal_buffer, buffer->m_refs);
#endif
  }
}

void COpenMaxVideo::Render(COpenMaxVideoBuffer *omvb, int index)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%p)", CLASSNAME, __func__, omvb, omvb->mmal_buffer);
#endif

  if (index == 0)
  {
    omvb->Acquire();
    mmal_port_send_buffer(m_vout_input, omvb->mmal_buffer);
  }

  MMAL_BUFFER_HEADER_T *buffer;
  MMAL_STATUS_T status;
  while (buffer = mmal_queue_get(m_vout_input_pool->queue), buffer)
  {
    mmal_buffer_header_reset(buffer);
    buffer->cmd = 0;
    #if defined(OMX_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s::%s Send buffer %p from pool to decoder output port %p demux_queue(%d) dts_queue(%d) ready_queue(%d) busy_queue(%d)", CLASSNAME, __func__, buffer, m_dec_output,
      m_demux_queue.size(), m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy.size());
    #endif
    status = mmal_port_send_buffer(m_dec_output, buffer);
    if (status != MMAL_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s::%s - Failed send buffer to decoder output port (status=0%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
      return;
    }
  }
}

void COpenMaxVideo::ReleaseOpenMaxBuffer(COpenMaxVideoBuffer *buffer)
{
  // remove from busy list
  pthread_mutex_lock(&m_omx_output_mutex);
  m_omx_output_busy.erase(std::remove(m_omx_output_busy.begin(), m_omx_output_busy.end(), buffer), m_omx_output_busy.end());
  pthread_mutex_unlock(&m_omx_output_mutex);
  ReturnOpenMaxBuffer(buffer);
  bool done = false;
  pthread_mutex_lock(&m_omx_output_mutex);
  if (m_finished && m_omx_output_busy.empty())
    done = true;
  pthread_mutex_unlock(&m_omx_output_mutex);
  if (done)
    m_myself.reset();
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%p)", CLASSNAME, __func__, buffer, buffer->mmal_buffer);
  #endif
  delete buffer;
}

bool COpenMaxVideo::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  //CLog::Log(LOGDEBUG, "%s::%s - m_omx_output_busy.size()=%d m_omx_output_ready.size()=%d", CLASSNAME, __func__, m_omx_output_busy.size(), m_omx_output_ready.size());
  //CLog::Log(LOGDEBUG, "%s::%s -  full: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());

  if (!m_omx_output_ready.empty())
  {
    COpenMaxVideoBuffer *buffer;
    // fetch a output buffer and pop it off the ready list
    pthread_mutex_lock(&m_omx_output_mutex);
    buffer = m_omx_output_ready.front();
    m_omx_output_ready.pop();
    m_omx_output_busy.push_back(buffer);
    pthread_mutex_unlock(&m_omx_output_mutex);

    assert(buffer->mmal_buffer);
    memset(pDvdVideoPicture, 0, sizeof *pDvdVideoPicture);
    pDvdVideoPicture->format = RENDER_FMT_OMXEGL; // RENDER_FMT_BYPASS;
    pDvdVideoPicture->openMaxBuffer = buffer;
    pDvdVideoPicture->color_range  = 0;
    pDvdVideoPicture->color_matrix = 4;
    pDvdVideoPicture->iWidth  = buffer->width ? buffer->width : m_decoded_width;
    pDvdVideoPicture->iHeight = buffer->height ? buffer->height : m_decoded_height;
    pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
    pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;

    if (buffer->m_aspect_ratio > 0.0 && !m_forced_aspect_ratio)
    {
      pDvdVideoPicture->iDisplayWidth  = ((int)lrint(pDvdVideoPicture->iHeight * buffer->m_aspect_ratio)) & -3;
      if (pDvdVideoPicture->iDisplayWidth > pDvdVideoPicture->iWidth)
      {
        pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
        pDvdVideoPicture->iDisplayHeight = ((int)lrint(pDvdVideoPicture->iWidth / buffer->m_aspect_ratio)) & -3;
      }
    }

    // timestamp is in microseconds
    pDvdVideoPicture->dts = buffer->dts;
    pDvdVideoPicture->pts = buffer->mmal_buffer->pts == MMAL_TIME_UNKNOWN ? DVD_NOPTS_VALUE : buffer->mmal_buffer->pts;

    pDvdVideoPicture->openMaxBuffer->Acquire();
    pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
#if defined(OMX_DEBUG_VERBOSE)
    CLog::Log(LOGINFO, "%s::%s dts:%.3f pts:%.3f flags:%x:%x frame:%d openMaxBuffer:%p mmal_buffer:%p", CLASSNAME, __func__,
        pDvdVideoPicture->dts == DVD_NOPTS_VALUE ? 0.0 : pDvdVideoPicture->dts*1e-6, pDvdVideoPicture->pts == DVD_NOPTS_VALUE ? 0.0 : pDvdVideoPicture->pts*1e-6,
        pDvdVideoPicture->iFlags, buffer->mmal_buffer->flags, (int)buffer->mmal_buffer->user_data, pDvdVideoPicture->openMaxBuffer, pDvdVideoPicture->openMaxBuffer->mmal_buffer);
#endif
    assert(!(buffer->mmal_buffer->flags & MMAL_BUFFER_HEADER_FLAG_DECODEONLY));
  }
  else
  {
    CLog::Log(LOGERROR, "%s::%s - called but m_omx_output_ready is empty", CLASSNAME, __func__);
    return false;
  }

  if (pDvdVideoPicture->pts != DVD_NOPTS_VALUE)
    m_decoderPts = pDvdVideoPicture->pts;
  else
    m_decoderPts = pDvdVideoPicture->dts; // xxx is DVD_NOPTS_VALUE better?

  g_renderManager.RegisterRenderUpdateCallBack((const void*)this, RenderUpdateCallBack);

  return true;
}

bool COpenMaxVideo::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s - %p (%p)", CLASSNAME, __func__, pDvdVideoPicture->openMaxBuffer, pDvdVideoPicture->openMaxBuffer ? pDvdVideoPicture->openMaxBuffer->mmal_buffer : 0);
#endif
  if (pDvdVideoPicture->format == RENDER_FMT_OMXEGL)
  {
    pDvdVideoPicture->openMaxBuffer->Release();
    //delete pDvdVideoPicture->openMaxBuffer;
  }
  memset(pDvdVideoPicture, 0, sizeof *pDvdVideoPicture);
  return true;
}

bool COpenMaxVideo::GetCodecStats(double &pts, int &droppedPics)
{
  pts = m_decoderPts;
  droppedPics = m_droppedPics;
  m_droppedPics = 0;
#if defined(OMX_DEBUG_VERBOSE)
  //CLog::Log(LOGDEBUG, "%s::%s - pts:%.0f droppedPics:%d", CLASSNAME, __func__, pts, droppedPics);
#endif
  return true;
}

void COpenMaxVideo::SetVideoRect(const CRect& InSrcRect, const CRect& InDestRect)
{
  // we get called twice a frame for left/right. Can ignore the rights.
  if (g_graphicsContext.GetStereoView() == RENDER_STEREO_VIEW_RIGHT)
    return;

  std::string  stereo_mode;

  switch(CMediaSettings::Get().GetCurrentVideoSettings().m_StereoMode)
  {
    case RENDER_STEREO_MODE_SPLIT_VERTICAL:   stereo_mode = "left_right"; break;
    case RENDER_STEREO_MODE_SPLIT_HORIZONTAL: stereo_mode = "top_bottom"; break;
    default:                                  stereo_mode = m_hints.stereo_mode; break;
  }

  if (CMediaSettings::Get().GetCurrentVideoSettings().m_StereoInvert)
    stereo_mode = RenderManager::GetStereoModeInvert(stereo_mode);

  CRect SrcRect = InSrcRect, DestRect = InDestRect;
  unsigned flags = RenderManager::GetStereoModeFlags(stereo_mode);
  RENDER_STEREO_MODE video_stereo_mode = (flags & CONF_FLAGS_STEREO_MODE_SBS) ? RENDER_STEREO_MODE_SPLIT_VERTICAL :
                                         (flags & CONF_FLAGS_STEREO_MODE_TAB) ? RENDER_STEREO_MODE_SPLIT_HORIZONTAL : RENDER_STEREO_MODE_OFF;
  bool stereo_invert                   = (flags & CONF_FLAGS_STEREO_CADANCE_RIGHT_LEFT) ? true : false;
  RENDER_STEREO_MODE display_stereo_mode = g_graphicsContext.GetStereoMode();

  // fix up transposed video
  if (m_hints.orientation == 90 || m_hints.orientation == 270)
  {
    float diff = (DestRect.Height() - DestRect.Width()) * 0.5f;
    DestRect.x1 -= diff;
    DestRect.x2 += diff;
    DestRect.y1 += diff;
    DestRect.y2 -= diff;
  }

  // check if destination rect or video view mode has changed
  if (!(m_dst_rect != DestRect) && !(m_src_rect != SrcRect) && m_video_stereo_mode == video_stereo_mode && m_display_stereo_mode == display_stereo_mode && m_StereoInvert == stereo_invert)
    return;

  CLog::Log(LOGDEBUG, "%s::%s %d,%d,%d,%d -> %d,%d,%d,%d (%d,%d,%d,%d,%s)", CLASSNAME, __func__,
      (int)SrcRect.x1, (int)SrcRect.y1, (int)SrcRect.x2, (int)SrcRect.y2,
      (int)DestRect.x1, (int)DestRect.y1, (int)DestRect.x2, (int)DestRect.y2,
      video_stereo_mode, display_stereo_mode, CMediaSettings::Get().GetCurrentVideoSettings().m_StereoInvert, g_graphicsContext.GetStereoView(), stereo_mode.c_str());

  m_src_rect = SrcRect;
  m_dst_rect = DestRect;
  m_video_stereo_mode = video_stereo_mode;
  m_display_stereo_mode = display_stereo_mode;
  m_StereoInvert = stereo_invert;

  // might need to scale up m_dst_rect to display size as video decodes
  // to separate video plane that is at display size.
  RESOLUTION res = g_graphicsContext.GetVideoResolution();
  CRect gui(0, 0, CDisplaySettings::Get().GetResolutionInfo(res).iWidth, CDisplaySettings::Get().GetResolutionInfo(res).iHeight);
  CRect display(0, 0, CDisplaySettings::Get().GetResolutionInfo(res).iScreenWidth, CDisplaySettings::Get().GetResolutionInfo(res).iScreenHeight);

  switch (video_stereo_mode)
  {
  case RENDER_STEREO_MODE_SPLIT_VERTICAL:
    // optimisation - use simpler display mode in common case of unscaled 3d with same display mode
    if (video_stereo_mode == display_stereo_mode && DestRect.x1 == 0.0f && DestRect.x2 * 2.0f == gui.Width() && !stereo_invert)
    {
      SrcRect.x2 *= 2.0f;
      DestRect.x2 *= 2.0f;
      video_stereo_mode = RENDER_STEREO_MODE_OFF;
      display_stereo_mode = RENDER_STEREO_MODE_OFF;
    }
    else if (display_stereo_mode == RENDER_STEREO_MODE_ANAGLYPH_RED_CYAN || display_stereo_mode == RENDER_STEREO_MODE_ANAGLYPH_GREEN_MAGENTA)
    {
      SrcRect.x2 *= 2.0f;
    }
    else if (stereo_invert)
    {
      SrcRect.x1 += m_decoded_width / 2;
      SrcRect.x2 += m_decoded_width / 2;
    }
    break;

  case RENDER_STEREO_MODE_SPLIT_HORIZONTAL:
    // optimisation - use simpler display mode in common case of unscaled 3d with same display mode
    if (video_stereo_mode == display_stereo_mode && DestRect.y1 == 0.0f && DestRect.y2 * 2.0f == gui.Height() && !stereo_invert)
    {
      SrcRect.y2 *= 2.0f;
      DestRect.y2 *= 2.0f;
      video_stereo_mode = RENDER_STEREO_MODE_OFF;
      display_stereo_mode = RENDER_STEREO_MODE_OFF;
    }
    else if (display_stereo_mode == RENDER_STEREO_MODE_ANAGLYPH_RED_CYAN || display_stereo_mode == RENDER_STEREO_MODE_ANAGLYPH_GREEN_MAGENTA)
    {
      SrcRect.y2 *= 2.0f;
    }
    else if (stereo_invert)
    {
      SrcRect.y1 += m_decoded_height / 2;
      SrcRect.y2 += m_decoded_height / 2;
    }
    break;

  default: break;
  }

  if (gui != display)
  {
    float xscale = display.Width()  / gui.Width();
    float yscale = display.Height() / gui.Height();
    DestRect.x1 *= xscale;
    DestRect.x2 *= xscale;
    DestRect.y1 *= yscale;
    DestRect.y2 *= yscale;
  }

  MMAL_DISPLAYREGION_T region;
  memset(&region, 0, sizeof region);

  region.set                 = MMAL_DISPLAY_SET_DEST_RECT|MMAL_DISPLAY_SET_SRC_RECT|MMAL_DISPLAY_SET_FULLSCREEN|MMAL_DISPLAY_SET_NOASPECT|MMAL_DISPLAY_SET_MODE;
  region.dest_rect.x         = lrintf(DestRect.x1);
  region.dest_rect.y         = lrintf(DestRect.y1);
  region.dest_rect.width     = lrintf(DestRect.Width());
  region.dest_rect.height    = lrintf(DestRect.Height());

  region.src_rect.x          = lrintf(SrcRect.x1);
  region.src_rect.y          = lrintf(SrcRect.y1);
  region.src_rect.width      = lrintf(SrcRect.Width());
  region.src_rect.height     = lrintf(SrcRect.Height());

  region.fullscreen = MMAL_FALSE;
  region.noaspect = MMAL_TRUE;

  if (video_stereo_mode == RENDER_STEREO_MODE_SPLIT_HORIZONTAL && display_stereo_mode == RENDER_STEREO_MODE_SPLIT_HORIZONTAL)
    region.mode = MMAL_DISPLAY_MODE_LETTERBOX;//OMX_DISPLAY_MODE_STEREO_TOP_TO_TOP;
  else if (video_stereo_mode == RENDER_STEREO_MODE_SPLIT_HORIZONTAL && display_stereo_mode == RENDER_STEREO_MODE_SPLIT_VERTICAL)
    region.mode = MMAL_DISPLAY_MODE_LETTERBOX;//OMX_DISPLAY_MODE_STEREO_TOP_TO_LEFT;
  else if (video_stereo_mode == RENDER_STEREO_MODE_SPLIT_VERTICAL && display_stereo_mode == RENDER_STEREO_MODE_SPLIT_HORIZONTAL)
    region.mode = MMAL_DISPLAY_MODE_LETTERBOX;//OMX_DISPLAY_MODE_STEREO_LEFT_TO_TOP;
  else if (video_stereo_mode == RENDER_STEREO_MODE_SPLIT_VERTICAL && display_stereo_mode == RENDER_STEREO_MODE_SPLIT_VERTICAL)
    region.mode = MMAL_DISPLAY_MODE_LETTERBOX;//OMX_DISPLAY_MODE_STEREO_LEFT_TO_LEFT;
  else
    region.mode = MMAL_DISPLAY_MODE_LETTERBOX;

  MMAL_STATUS_T status = mmal_util_set_display_region(m_vout_input, &region);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to set display region (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));

  CLog::Log(LOGDEBUG, "%s::%s %d,%d,%d,%d -> %d,%d,%d,%d mode:%d", CLASSNAME, __func__,
      region.src_rect.x, region.src_rect.y, region.src_rect.width, region.src_rect.height,
      region.dest_rect.x, region.dest_rect.y, region.dest_rect.width, region.dest_rect.height, region.mode);
}
