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
#include "dialogs/GUIDialogKaiToast.h"
#include "filesystem/File.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/Texture.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/MathUtils.h"
#include "utils/SystemInfo.h"
#include "windowing/WindowingFactory.h"
#include "cores/FFmpeg.h"

#define CLASSNAME "COMXRenderer"

COMXRenderer::COMXRenderer()
{
}

COMXRenderer::~COMXRenderer()
{
  UnInit();
}

void COMXRenderer::AddProcessor(COpenMaxVideoBuffer *openMaxBuffer, int index)
{
  CLog::Log(LOGNOTICE, "%s::%s - %p %i", CLASSNAME, __func__, openMaxBuffer, index);
  //YUVBUFFER &buf = m_buffers[index];
  //COpenMaxVideoBuffer *pic = openMaxBuffer->Acquire();
  //SAFE_RELEASE(buf.openMaxBuffer);
  //buf.openMaxBuffer = pic;
}

bool COMXRenderer::Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, float fps, unsigned flags, ERenderFormat format, unsigned extended_format, unsigned int orientation)
{
  if(m_sourceWidth  != width
  || m_sourceHeight != height)
  {
    m_sourceWidth       = width;
    m_sourceHeight      = height;
  }
  CLog::Log(LOGNOTICE, "%s::%s - %dx%d->%dx%d@%.2f flags:%x format:%d ext:%x orient:%d", CLASSNAME, __func__, width, height, d_width, d_height, fps, flags, format, extended_format, orientation);

  m_fps = fps;
  m_iFlags = flags;
  m_format = format;

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
  if(m_NumYV12Buffers)
    return (m_iYV12RenderBuffer + 1) % m_NumYV12Buffers;
  else
    return -1;
}

bool COMXRenderer::AddVideoPicture(DVDVideoPicture* picture, int index)
{
  int source = index;
  if(source < 0 || NextYV12Texture() < 0)
    return false;

  CLog::Log(LOGNOTICE, "%s::%s - %p %i", CLASSNAME, __func__, picture, index);
/*  DXVABuffer *buf = (DXVABuffer*)m_VideoBuffers[source];
  buf->id = m_processor->Add(picture);*/
  return true;
}

int COMXRenderer::GetImage(YV12Image *image, int source, bool readonly)
{
  CLog::Log(LOGNOTICE, "%s::%s - %p %d %d", CLASSNAME, __func__, image, source, readonly);
  /* take next available buffer */
  if( source == AUTOSOURCE )
    source = NextYV12Texture();

  if( source < 0 || NextYV12Texture() < 0)
    return -1;

  //YUVBuffer *buf = (YUVBuffer*)m_VideoBuffers[source];

  image->cshift_x = 1;
  image->cshift_y = 1;
  image->height = m_sourceHeight;
  image->width = m_sourceWidth;
  image->flags = 0;
  image->bpp = 1;

  for(int i=0;i<3;i++)
  {
    //image->stride[i] = buf->planes[i].rect.Pitch;
    //image->plane[i]  = (BYTE*)buf->planes[i].rect.pBits;
  }

  return source;
}

void COMXRenderer::ReleaseImage(int source, bool preserve)
{
  CLog::Log(LOGNOTICE, "%s::%s - %d %d", CLASSNAME, __func__, source, preserve);
  // no need to release anything here since we're using system memory
}

void COMXRenderer::Reset()
{
  CLog::Log(LOGNOTICE, "%s::%s", CLASSNAME, __func__);
}

void COMXRenderer::Update()
{
  CLog::Log(LOGNOTICE, "%s::%s", CLASSNAME, __func__);
  if (!m_bConfigured) return;
  ManageDisplay();
}

void COMXRenderer::RenderUpdate(bool clear, DWORD flags, DWORD alpha)
{
  CLog::Log(LOGNOTICE, "%s::%s - %d %x %d", CLASSNAME, __func__, clear, flags, alpha);

  if (!m_bConfigured) return;

  CSingleLock lock(g_graphicsContext);

  ManageDisplay();

  Render(flags);
}

void COMXRenderer::FlipPage(int source)
{
  if(source == AUTOSOURCE)
    source = NextYV12Texture();

  //if (m_VideoBuffers[m_iYV12RenderBuffer] != NULL)
  //  m_VideoBuffers[m_iYV12RenderBuffer]->StartDecode();

  if( source >= 0 && source < m_NumYV12Buffers )
    m_iYV12RenderBuffer = source;
  else
    m_iYV12RenderBuffer = 0;

  CLog::Log(LOGNOTICE, "%s::%s - %d", CLASSNAME, __func__, source);

  //if (m_VideoBuffers[m_iYV12RenderBuffer] != NULL)
  //  m_VideoBuffers[m_iYV12RenderBuffer]->StartRender();

}

unsigned int COMXRenderer::PreInit()
{
  CSingleLock lock(g_graphicsContext);
  m_bConfigured = false;
  UnInit();
  m_resolution = CDisplaySettings::Get().GetCurrentResolution();
  if ( m_resolution == RES_WINDOW )
    m_resolution = RES_DESKTOP;

  CLog::Log(LOGNOTICE, "%s::%s", CLASSNAME, __func__);

  m_formats.clear();
  m_formats.push_back(RENDER_FMT_OMXEGL);

  //m_iRequestedMethod = CSettings::Get().GetInt("videoplayer.rendermethod");

  return 0;
}

void COMXRenderer::UnInit()
{
  CSingleLock lock(g_graphicsContext);
  CLog::Log(LOGNOTICE, "%s::%s", CLASSNAME, __func__);

  m_bConfigured = false;
  m_NumYV12Buffers = 0;
}

void COMXRenderer::Render(DWORD flags)
{
  CLog::Log(LOGNOTICE, "%s::%s - %x", CLASSNAME, __func__, flags);
}


bool COMXRenderer::RenderCapture(CRenderCapture* capture)
{
  if (!m_bConfigured || m_NumYV12Buffers == 0)
    return false;

  bool succeeded = false;

  CLog::Log(LOGNOTICE, "%s::%s - %p", CLASSNAME, __func__, capture);

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
  return 3;//m_processor->Size();
}

#endif
