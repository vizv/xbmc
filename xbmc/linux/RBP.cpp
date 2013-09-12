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

#include "RBP.h"
#if defined(TARGET_RASPBERRY_PI)

#include "utils/log.h"
#include "guilib/GraphicContext.h"
#include "settings/DisplaySettings.h"
#include "settings/AdvancedSettings.h"

CRBP::CRBP()
{
  m_initialized     = false;
  m_omx_initialized = false;
  m_DllBcmHost      = new DllBcmHost();
  m_OMX             = new COMXCore();
}

CRBP::~CRBP()
{
  Deinitialize();
  delete m_OMX;
  delete m_DllBcmHost;
}

bool CRBP::Initialize()
{
  m_initialized = m_DllBcmHost->Load();
  if(!m_initialized)
    return false;

  m_DllBcmHost->bcm_host_init();

  m_omx_initialized = m_OMX->Initialize();
  if(!m_omx_initialized)
    return false;

  char response[80] = "";
  m_arm_mem = 0;
  m_gpu_mem = 0;
  if (vc_gencmd(response, sizeof response, "get_mem arm") == 0)
    vc_gencmd_number_property(response, "arm", &m_arm_mem);
  if (vc_gencmd(response, sizeof response, "get_mem gpu") == 0)
    vc_gencmd_number_property(response, "gpu", &m_gpu_mem);

  return true;
}

void CRBP::LogFirmwareVerison()
{
  char  response[160];
  m_DllBcmHost->vc_gencmd(response, sizeof response, "version");
  response[sizeof(response) - 1] = '\0';
  CLog::Log(LOGNOTICE, "Raspberry PI firmware version: %s", response);
  CLog::Log(LOGNOTICE, "ARM mem: %dMB GPU mem: %dMB", m_arm_mem, m_gpu_mem);
}

void CRBP::GetDisplaySize(int &width, int &height)
{
  DISPMANX_DISPLAY_HANDLE_T display;
  DISPMANX_MODEINFO_T info;

  display = vc_dispmanx_display_open( 0 /*screen*/ );
  if (vc_dispmanx_display_get_info(display, &info) == 0)
  {
    width = info.width;
    height = info.height;
  }
  else
  {
    width = 0;
    height = 0;
  }
  vc_dispmanx_display_close(display );
}

unsigned char *CRBP::CaptureDisplay(int width, int height, int *pstride, bool swap_red_blue, bool video_only)
{
  DISPMANX_DISPLAY_HANDLE_T display;
  DISPMANX_RESOURCE_HANDLE_T resource;
  VC_RECT_T rect;
  unsigned char *image = NULL;
  uint32_t vc_image_ptr;
  int stride;
  uint32_t flags = 0;

  if (video_only)
    flags |= DISPMANX_SNAPSHOT_NO_RGB|DISPMANX_SNAPSHOT_FILL;
  if (swap_red_blue)
    flags |= DISPMANX_SNAPSHOT_SWAP_RED_BLUE;
  if (!pstride)
    flags |= DISPMANX_SNAPSHOT_PACK;

  display = vc_dispmanx_display_open( 0 /*screen*/ );
  stride = ((width + 15) & ~15) * 4;
  image = new unsigned char [height * stride];

  if (image)
  {
    resource = vc_dispmanx_resource_create( VC_IMAGE_RGBA32, width, height, &vc_image_ptr );

    vc_dispmanx_snapshot(display, resource, (DISPMANX_TRANSFORM_T)flags);

    vc_dispmanx_rect_set(&rect, 0, 0, width, height);
    vc_dispmanx_resource_read_data(resource, &rect, image, stride);
    vc_dispmanx_resource_delete( resource );
    vc_dispmanx_display_close(display );
  }
  if (pstride)
    *pstride = stride;
  return image;
}

void CRBP::Deinitialize()
{
  if(m_omx_initialized)
    m_OMX->Deinitialize();

  m_DllBcmHost->bcm_host_deinit();

  if(m_initialized)
    m_DllBcmHost->Unload();

  m_initialized     = false;
  m_omx_initialized = false;
}

bool CRBP::ClampLimits(unsigned int &width, unsigned int &height, unsigned int m_width, unsigned int m_height, bool transposed)
{
  RESOLUTION_INFO& res_info = CDisplaySettings::Get().GetResolutionInfo(g_graphicsContext.GetVideoResolution());
  unsigned int max_width = width;
  unsigned int max_height = height;
  const unsigned int gui_width = transposed ? res_info.iHeight:res_info.iWidth;
  const unsigned int gui_height = transposed ? res_info.iWidth:res_info.iHeight;
  const float aspect = (float)m_width / m_height;
  bool clamped = false;

  if (max_width == 0 || max_height == 0)
  {
    max_height = g_advancedSettings.m_imageRes;

    if (g_advancedSettings.m_fanartRes > g_advancedSettings.m_imageRes)
    { // 16x9 images larger than the fanart res use that rather than the image res
      if (fabsf(aspect / (16.0f/9.0f) - 1.0f) <= 0.01f && m_height >= g_advancedSettings.m_fanartRes)
      {
        max_height = g_advancedSettings.m_fanartRes;
      }
    }
    max_width = max_height * 16/9;
  }

  if (gui_width)
    max_width = min(max_width, gui_width);
  if (gui_height)
    max_height = min(max_height, gui_height);

  max_width  = min(max_width, 2048U);
  max_height = min(max_height, 2048U);

  width = m_width;
  height = m_height;
  if (width > max_width || height > max_height)
  {
    if ((unsigned int)(max_width / aspect + 0.5f) > max_height)
      max_width = (unsigned int)(max_height * aspect + 0.5f);
    else
      max_height = (unsigned int)(max_width / aspect + 0.5f);
    width = max_width;
    height = max_height;
    clamped = true;
  }
  // Texture.cpp wants even width/height
  width  = (width  + 15) & ~15;
  height = (height + 15) & ~15;

  return clamped;
}

bool CRBP::CreateThumbnailFromSurface(unsigned char* buffer, unsigned int width, unsigned int height,
      unsigned int format, unsigned int pitch, const CStdString& destFile)
{
  COMXImageEnc omxImageEnc;
  bool ret = omxImageEnc.CreateThumbnailFromSurface(buffer, width, height, format, pitch, destFile);
  if (!ret)
    CLog::Log(LOGNOTICE, "%s: unable to create thumbnail %s %dx%d", __func__, destFile.c_str(), width, height);
  return ret;
}

COMXImageFile *CRBP::LoadJpeg(const CStdString& texturePath)
{
  COMXImageFile *file = new COMXImageFile();
  if (!file->ReadFile(texturePath))
  {
    CLog::Log(LOGNOTICE, "%s: unable to load %s", __func__, texturePath.c_str());
    delete file;
    file = NULL;
  }
  return file;
}

void CRBP::CloseJpeg(COMXImageFile *file)
{
  delete file;
}

bool CRBP::DecodeJpeg(COMXImageFile *file, unsigned int width, unsigned int height, unsigned int stride, void *pixels)
{
  bool ret = false;
  COMXImage omx_image;
  if (omx_image.Decode(file->GetImageBuffer(), file->GetImageSize(), width, height, stride, pixels))
  {
    assert(width  == omx_image.GetDecodedWidth());
    assert(height == omx_image.GetDecodedHeight());
    assert(stride == omx_image.GetDecodedStride());
    ret = true;
  }
  else
    CLog::Log(LOGNOTICE, "%s: unable to decode %s %dx%d", __func__, file->GetFilename(), width, height);
  omx_image.Close();
  return ret;
}
#endif
