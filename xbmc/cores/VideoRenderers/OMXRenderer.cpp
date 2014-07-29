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

#ifdef TARGET_RASPBERRY_PI

#include "Util.h"
#include "OMXRenderer.h"
#include "cores/dvdplayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "filesystem/File.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/MathUtils.h"
#include "windowing/WindowingFactory.h"
#include "cores/dvdplayer/DVDCodecs/Video/OpenMaxVideo.h"

#define CLASSNAME "COMXRenderer"

#ifdef _DEBUG
#define OMX_DEBUG_VERBOSE
#endif

COMXRenderer::YUVBUFFER::YUVBUFFER()
{
  memset(&fields, 0, sizeof(fields));
  memset(&image , 0, sizeof(image));
  openMaxBuffer = NULL;
  mmal_buffer = NULL;
}

COMXRenderer::YUVBUFFER::~YUVBUFFER()
{
  CLog::Log(LOGERROR, "%s::%s Delete %p", CLASSNAME, __func__, openMaxBuffer);
}

static void* pool_allocator_alloc(void *context, uint32_t size)
{
  return mmal_port_payload_alloc((MMAL_PORT_T *)context, size);
}

static void pool_allocator_free(void *context, void *mem)
{
  mmal_port_payload_free((MMAL_PORT_T *)context, (uint8_t *)mem);
}

static void vout_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  mmal_buffer_header_release(buffer);
}

void COMXRenderer::vout_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  COpenMaxVideoBuffer *omvb = (COpenMaxVideoBuffer *)buffer->user_data;

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s port:%p buffer %p (%p), len %d cmd:%x", CLASSNAME, __func__, port, buffer, omvb, buffer->length, buffer->cmd);
  #endif

  if (m_format == RENDER_FMT_OMXEGL)
  {
    omvb->Release();
  }
  else if (m_format == RENDER_FMT_YUV420P)
  {
    mmal_buffer_header_release(buffer);
  }
  else assert(0);
}

static void vout_input_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  COMXRenderer *omx = reinterpret_cast<COMXRenderer*>(port->userdata);
  omx->vout_input_port_cb(port, buffer);
}

bool COMXRenderer::init_vout(MMAL_ES_FORMAT_T *format)
{
  MMAL_STATUS_T status;
  // todo: deinterlace

  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  /* Create video renderer */
  status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &m_vout);
  if(status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create vout component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  m_vout->control->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_vout->control, vout_control_port_cb);
  if(status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable vout control port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }
  m_vout_input = m_vout->input[0];
  m_vout_input->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  mmal_format_full_copy(m_vout_input->format, format);
  status = mmal_port_format_commit(m_vout_input);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit vout input format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  if (m_format == RENDER_FMT_YUV420P)
    m_vout_input->buffer_num = m_NumYV12Buffers;
  else
    m_vout_input->buffer_num = m_vout_input->buffer_num_min;
  m_vout_input->buffer_size = m_vout_input->buffer_size_recommended;

  status = mmal_port_enable(m_vout_input, vout_input_port_cb_static);
  if(status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to vout enable input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  status = mmal_component_enable(m_vout);
  if(status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable vout component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  if (m_format == RENDER_FMT_YUV420P)
  {
    printf("m_vout_input_pool: %dx%d=%dM\n", m_vout_input->buffer_num, m_vout_input->buffer_size, m_vout_input->buffer_num * m_vout_input->buffer_size >> 20);
    m_vout_input_pool = mmal_pool_create_with_allocator(m_vout_input->buffer_num, m_vout_input->buffer_size, m_vout_input, pool_allocator_alloc, pool_allocator_free);
    if (!m_vout_input_pool)
    {
      CLog::Log(LOGERROR, "%s::%s Failed to create pool for decoder input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
      return false;
    }
  }
  return true;
}

bool COMXRenderer::change_vout_input_format(MMAL_ES_FORMAT_T *format)
{
  MMAL_STATUS_T status;
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  status = mmal_port_disable(m_vout_input);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to disable vout input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  mmal_format_full_copy(m_vout_input->format, format);
  status = mmal_port_format_commit(m_vout_input);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit vout input format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  m_vout_input->buffer_size = m_vout_input->buffer_size_min;
  status = mmal_port_enable(m_vout_input, vout_input_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable vout input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }
  return true;
}


COMXRenderer::COMXRenderer()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  m_iYV12RenderBuffer = 0;
  m_NumYV12Buffers = 0;

  m_vout = NULL;
  m_vout_input = NULL;
  m_vout_input_pool = NULL;

  m_changed_count_vout = 0;

  m_orientation = 0;
  m_src_rect.SetRect(0, 0, 0, 0);
  m_dst_rect.SetRect(0, 0, 0, 0);
  m_video_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_display_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_StereoInvert = false;
}

COMXRenderer::~COMXRenderer()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  UnInit();
}

void COMXRenderer::AddProcessor(COpenMaxVideoBuffer *openMaxBuffer, int index)
{
  CLog::Log(LOGDEBUG, "%s::%s - %p (%p) %i", CLASSNAME, __func__, openMaxBuffer, openMaxBuffer->mmal_buffer, index);

  YUVBUFFER &buf = m_buffers[index];
  COpenMaxVideoBuffer *pic = openMaxBuffer->Acquire();
  SAFE_RELEASE(buf.openMaxBuffer);
  buf.openMaxBuffer = pic;
}

bool COMXRenderer::Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, float fps, unsigned flags, ERenderFormat format, unsigned extended_format, unsigned int orientation)
{
  if (m_sourceWidth == width && m_sourceHeight == height && m_orientation == orientation && m_fps == fps && m_iFlags == flags && m_format == format)
    return true;

  m_sourceWidth  = width;
  m_sourceHeight = height;
  m_orientation  = orientation;

  m_fps = fps;
  m_iFlags = flags;
  m_format = format;

  CLog::Log(LOGDEBUG, "%s::%s - %dx%d->%dx%d@%.2f flags:%x format:%d ext:%x orient:%d", CLASSNAME, __func__, width, height, d_width, d_height, fps, flags, format, extended_format, orientation);

  // calculate the input frame aspect ratio
  CalculateFrameAspectRatio(d_width, d_height);
  ChooseBestResolution(fps);
  m_destWidth = g_graphicsContext.GetResInfo(m_resolution).iWidth;
  m_destHeight = g_graphicsContext.GetResInfo(m_resolution).iHeight;
  SetViewMode(CMediaSettings::Get().GetCurrentVideoSettings().m_ViewMode);
  ManageDisplay();

  m_bConfigured = true;

  return true;
}

int COMXRenderer::NextYV12Texture()
{
  return (m_iYV12RenderBuffer + 1) % m_NumYV12Buffers;
}

int COMXRenderer::GetImage(YV12Image *image, int source, bool readonly)
{
  CLog::Log(LOGDEBUG, "%s::%s - %p %d %d", CLASSNAME, __func__, image, source, readonly);
  if (!image) return -1;

  /* take next available buffer */
  if( source == AUTOSOURCE )
   source = NextYV12Texture();

  if (m_format == RENDER_FMT_OMXEGL)
  {
  }
  else if (m_format == RENDER_FMT_YUV420P)
  {
    const int pitch = ALIGN_UP(m_sourceWidth, 32);
    const int aligned_height = ALIGN_UP(m_sourceHeight, 16);
    MMAL_BUFFER_HEADER_T *buffer = NULL;
    if (!m_vout)
    {
      MMAL_ES_FORMAT_T *m_es_format = mmal_format_alloc();
      m_es_format->encoding = MMAL_ENCODING_I420;
      m_es_format->type = MMAL_ES_TYPE_VIDEO;
      m_es_format->es->video.width = pitch;
      m_es_format->es->video.height = aligned_height;
      m_es_format->es->video.crop.width = m_sourceWidth;
      m_es_format->es->video.crop.height = m_sourceHeight;

      if (!m_vout && init_vout(m_es_format))
      {
         mmal_format_free(m_es_format);
         return -1;
      }
      mmal_format_free(m_es_format);
    }

    buffer = mmal_queue_timedwait(m_vout_input_pool->queue, 500);
    if (!buffer)
    {
      CLog::Log(LOGERROR, "%s::%s - mmal_queue_get failed", CLASSNAME, __func__);
      return -1;
    }

    mmal_buffer_header_reset(buffer);

    buffer->length = 3 * pitch * aligned_height >> 1;
    assert(buffer->length <= buffer->alloc_size);

    image->width    = m_sourceWidth;
    image->height   = m_sourceHeight;
    image->flags    = 0;
    image->cshift_x = 1;
    image->cshift_y = 1;
    image->bpp      = 1;

    image->stride[0] = pitch;
    image->stride[1] = image->stride[2] = pitch>>image->cshift_x;

    image->planesize[0] = pitch * aligned_height;
    image->planesize[1] = image->planesize[2] = (pitch>>image->cshift_x)*(aligned_height>>image->cshift_y);

    image->plane[0] = (uint8_t *)buffer->data;
    image->plane[1] = image->plane[0] + image->planesize[0];
    image->plane[2] = image->plane[1] + image->planesize[1];

    CLog::Log(LOGDEBUG, "%s::%s - %p %d", CLASSNAME, __func__, buffer, source);
    m_buffers[source].mmal_buffer = buffer;
    //return -1;
  }
  else assert(0);

  return source;
}

void COMXRenderer::ReleaseImage(int source, bool preserve)
{
  CLog::Log(LOGDEBUG, "%s::%s - %d %d", CLASSNAME, __func__, source, preserve);
  // no need to release anything here since we're using system memory
}

void COMXRenderer::Reset()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
}

void COMXRenderer::Flush()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
}

void COMXRenderer::Update()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  if (!m_bConfigured) return;
  ManageDisplay();
}

void COMXRenderer::RenderUpdate(bool clear, DWORD flags, DWORD alpha)
{
  CLog::Log(LOGDEBUG, "%s::%s - %d %x %d", CLASSNAME, __func__, clear, flags, alpha);

  if (!m_bConfigured) return;

  CSingleLock lock(g_graphicsContext);

  ManageDisplay();

  SetVideoRect(m_sourceRect, m_destRect);

  CRect old = g_graphicsContext.GetScissors();

  g_graphicsContext.BeginPaint();
  g_graphicsContext.SetScissors(m_destRect);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  g_graphicsContext.SetScissors(old);
  g_graphicsContext.EndPaint();

  if (m_format == RENDER_FMT_BYPASS)
    return;

  COpenMaxVideoBuffer *omvb = m_buffers[m_iYV12RenderBuffer].openMaxBuffer;
  if (omvb)
  {
    CLog::Log(LOGDEBUG, "%s::%s %p (%p) index:%d frame:%d(%d)", CLASSNAME, __func__, omvb, omvb->mmal_buffer, m_iYV12RenderBuffer, m_changed_count_vout, omvb->m_changed_count);
    assert(omvb);

    if (!m_vout && init_vout(omvb->GetFormat()))
       return;

    if (m_changed_count_vout != omvb->m_changed_count)
    {
      CLog::Log(LOGDEBUG, "%s::%s format changed frame:%d(%d)", CLASSNAME, __func__, m_changed_count_vout, omvb->m_changed_count);
      change_vout_input_format(omvb->GetFormat());
      m_changed_count_vout = omvb->m_changed_count;
    }
    omvb->Acquire();
    mmal_port_send_buffer(m_vout_input, omvb->mmal_buffer);

  }
  else if (m_format == RENDER_FMT_YUV420P)
  {
    CLog::Log(LOGDEBUG, "%s::%s - %p %d", CLASSNAME, __func__, m_buffers[m_iYV12RenderBuffer].mmal_buffer, m_iYV12RenderBuffer);
    if (m_buffers[m_iYV12RenderBuffer].mmal_buffer)
    {
      mmal_port_send_buffer(m_vout_input, m_buffers[m_iYV12RenderBuffer].mmal_buffer);
      m_buffers[m_iYV12RenderBuffer].mmal_buffer = NULL;
      //mmal_buffer_header_release(m_buffers[m_iYV12RenderBuffer].mmal_buffer);
    }
  }
  else assert(0);
}

void COMXRenderer::FlipPage(int source)
{
  CLog::Log(LOGDEBUG, "%s::%s - %d", CLASSNAME, __func__, source);
  if( source >= 0 && source < m_NumYV12Buffers )
    m_iYV12RenderBuffer = source;
  else
    m_iYV12RenderBuffer = NextYV12Texture();
}

unsigned int COMXRenderer::PreInit()
{
  CSingleLock lock(g_graphicsContext);
  m_bConfigured = false;
  UnInit();
  m_resolution = CDisplaySettings::Get().GetCurrentResolution();
  if ( m_resolution == RES_WINDOW )
    m_resolution = RES_DESKTOP;

  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  m_formats.clear();
  m_formats.push_back(RENDER_FMT_YUV420P);
  m_formats.push_back(RENDER_FMT_OMXEGL);
  m_formats.push_back(RENDER_FMT_BYPASS);

  m_iYV12RenderBuffer = 0;
  m_NumYV12Buffers = 3;

  return 0;
}

void COMXRenderer::UnInit()
{
  CSingleLock lock(g_graphicsContext);
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  if (m_vout)
  {
    mmal_component_disable(m_vout);
    mmal_port_disable(m_vout->control);
  }

  if (m_vout_input)
  {
    mmal_port_flush(m_vout_input);
    mmal_port_disable(m_vout_input);
    m_vout_input = NULL;
  }

  if (m_vout_input_pool)
  {
    mmal_pool_destroy(m_vout_input_pool);
    m_vout_input_pool = NULL;
  }

  if (m_vout)
  {
    mmal_component_release(m_vout);
    m_vout = NULL;
  }

  for (int i=0; i<NUM_BUFFERS; i++)
  {
    YUVBUFFER &buf = m_buffers[i];
    if (buf.openMaxBuffer)
    {
      CLog::Log(LOGDEBUG, "%s::%s Delete %p", CLASSNAME, __func__, buf.openMaxBuffer);
      SAFE_RELEASE(buf.openMaxBuffer);
    }
  }
  m_bConfigured = false;
}

bool COMXRenderer::RenderCapture(CRenderCapture* capture)
{
  if (!m_bConfigured)
    return false;

  bool succeeded = false;

  CLog::Log(LOGDEBUG, "%s::%s - %p", CLASSNAME, __func__, capture);

  return succeeded;
}

//********************************************************************************************************
// YV12 Texture creation, deletion, copying + clearing
//********************************************************************************************************

bool COMXRenderer::Supports(EDEINTERLACEMODE mode)
{
  if(mode == VS_DEINTERLACEMODE_OFF
  || mode == VS_DEINTERLACEMODE_AUTO
  || mode == VS_DEINTERLACEMODE_FORCE)
    return true;

  return false;
}

bool COMXRenderer::Supports(EINTERLACEMETHOD method)
{
  if (method == VS_INTERLACEMETHOD_DEINTERLACE_HALF)
    return true;

  return false;
}

bool COMXRenderer::Supports(ERENDERFEATURE feature)
{
  if (feature == RENDERFEATURE_STRETCH         ||
      feature == RENDERFEATURE_CROP            ||
      feature == RENDERFEATURE_ZOOM            ||
      feature == RENDERFEATURE_VERTICAL_SHIFT  ||
      feature == RENDERFEATURE_PIXEL_RATIO)
    return true;

  return false;
}

bool COMXRenderer::Supports(ESCALINGMETHOD method)
{
  return false;
}

EINTERLACEMETHOD COMXRenderer::AutoInterlaceMethod()
{
  return VS_INTERLACEMETHOD_DEINTERLACE_HALF;
}

unsigned int COMXRenderer::GetProcessorSize()
{
  if (m_format == RENDER_FMT_OMXEGL)
    return 1;
  else
    return 0;
}

#endif

void COMXRenderer::SetVideoRect(const CRect& InSrcRect, const CRect& InDestRect)
{
  // we get called twice a frame for left/right. Can ignore the rights.
  if (g_graphicsContext.GetStereoView() == RENDER_STEREO_VIEW_RIGHT)
    return;

  if (!m_vout)
    return;

  std::string  stereo_mode;

  switch(CMediaSettings::Get().GetCurrentVideoSettings().m_StereoMode)
  {
    case RENDER_STEREO_MODE_SPLIT_VERTICAL:   stereo_mode = "left_right"; break;
    case RENDER_STEREO_MODE_SPLIT_HORIZONTAL: stereo_mode = "top_bottom"; break;
    default:                                  stereo_mode = "" /*m_hints.stereo_mode*/; break;
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
  if (m_orientation == 90 || m_orientation == 270)
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

  CLog::Log(LOGDEBUG, "%s::%s %d,%d,%d,%d -> %d,%d,%d,%d orient:%d (%d,%d,%d,%d,%s)", CLASSNAME, __func__,
      (int)SrcRect.x1, (int)SrcRect.y1, (int)SrcRect.x2, (int)SrcRect.y2,
      (int)DestRect.x1, (int)DestRect.y1, (int)DestRect.x2, (int)DestRect.y2, m_orientation,
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
      SrcRect.x1 += m_sourceWidth / 2;
      SrcRect.x2 += m_sourceWidth / 2;
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
      SrcRect.y1 += m_sourceHeight / 2;
      SrcRect.y2 += m_sourceHeight / 2;
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

  if (m_orientation)
  {
    region.set |= MMAL_DISPLAY_SET_TRANSFORM;
    if (m_orientation == 90)
      region.transform = MMAL_DISPLAY_ROT90;
    else if (m_orientation == 180)
      region.transform = MMAL_DISPLAY_ROT180;
    else if (m_orientation == 270)
      region.transform = MMAL_DISPLAY_ROT270;
    else assert(0);
  }

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
