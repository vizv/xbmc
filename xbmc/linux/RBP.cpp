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
#include "utils/URIUtils.h"
#include "windowing/WindowingFactory.h"
#include "settings/Settings.h"
#include "Application.h"

#define CheckError() m_result = eglGetError(); if(m_result != EGL_SUCCESS) CLog::Log(LOGERROR, "EGL error in %s: %x",__FUNCTION__, m_result);

CRBP::CRBP()
: CThread("CRBPWorker")
{
  m_initialized     = false;
  m_omx_initialized = false;
  m_DllBcmHost      = new DllBcmHost();
  m_OMX             = new COMXCore();
  pthread_mutex_init(&m_texqueue_mutex, NULL);
  pthread_cond_init(&m_texqueue_cond, NULL);
}

CRBP::~CRBP()
{
  Deinitialize();
  pthread_mutex_destroy(&m_texqueue_mutex);
  pthread_cond_destroy(&m_texqueue_cond);
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

  Create();

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

bool CRBP::CreateThumb(const CStdString& srcFile, unsigned int maxHeight, unsigned int maxWidth, std::string &additional_info, const CStdString& destFile)
{
  bool okay = false;
  COMXImageFile file;
  COMXImageReEnc reenc;
  void *pDestBuffer;
  unsigned int nDestSize;
  if (URIUtils::HasExtension(srcFile, ".jpg|.tbn") && file.ReadFile(srcFile) && reenc.ReEncode(file, maxWidth, maxHeight, pDestBuffer, nDestSize))
  {
    XFILE::CFile outfile;
    if (outfile.OpenForWrite(destFile, true))
    {
      outfile.Write(pDestBuffer, nDestSize);
      outfile.Close();
      okay = true;
    }
    else
      CLog::Log(LOGERROR, "%s: can't open output file: %s\n", __func__, destFile.c_str());
  }
  if (!okay)
     CLog::Log(LOGERROR, "%s: %dx%d %s->%s (%s) = %d", __func__, maxWidth, maxHeight, srcFile.c_str(), destFile.c_str(), additional_info.c_str(), okay);
  return okay;
}

void CRBP::AllocTextureInternal(struct textureinfo *tex)
{
//printf("%s %s\n", __func__, tex->filename);
  glGenTextures(1, (GLuint*) &tex->texture);
//printf("Texture alloc: %d (%s) %p\n", tex->texture, tex->filename, tex);
  assert(tex->texture);
  glBindTexture(GL_TEXTURE_2D, tex->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLenum type = CSettings::Get().GetBool("videoscreen.textures32") ? GL_UNSIGNED_BYTE:GL_UNSIGNED_SHORT_5_6_5;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tex->width, tex->height, 0, GL_RGB, type, 0);
  tex->egl_image = eglCreateImageKHR(m_egl_display, m_egl_context, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)tex->texture, NULL);
  sem_post(&tex->sync);
  GLint m_result;
  CheckError();
}

void CRBP::GetTexture(void *userdata, GLuint *texture)
{
  struct textureinfo *tex = static_cast<struct textureinfo *>(userdata);
//printf("%s: %p %d\n", __func__, tex, tex ? tex->texture:0);
  assert(texture && tex);
  *texture = tex->texture;
}

void CRBP::DestroyTextureInternal(struct textureinfo *tex)
{
  // we can only call gl functions from the application thread

  bool s = true;
  //printf("%s: %p %d %p\n", __func__, tex, tex->texture, tex->egl_image);
  assert (tex->egl_image);
  assert(tex->texture);
  s = eglDestroyImageKHR(m_egl_display, tex->egl_image);
  assert(s);
  glDeleteTextures(1, (GLuint*) &tex->texture);
  sem_post(&tex->sync);
}

void CRBP::DestroyTexture(void *userdata)
{
  struct textureinfo *tex = static_cast<struct textureinfo *>(userdata);
  // we can only call gl functions from the application thread

  tex->action = TEXTURE_DELETE;
  sem_init(&tex->sync, 0, 0);
  if ( g_application.IsCurrentThread() )
  {
     DestroyTextureInternal(tex);
  }
  else
  {
    pthread_mutex_lock(&m_texqueue_mutex);
    m_texqueue.push(tex);
    pthread_cond_broadcast(&m_texqueue_cond);
    pthread_mutex_unlock(&m_texqueue_mutex);
  }
  // wait for function to have finished (in texture thread)
  sem_wait(&tex->sync);
  memset(tex, 0, sizeof *tex);
  delete tex;
}

bool CRBP::DecodeJpegToTexture(COMXImageFile *file, unsigned int width, unsigned int height, void **userdata)
{
  bool ret = false;
  COMXTexture omx_image;

  struct textureinfo *tex = new struct textureinfo;
  assert(tex);
  if (!tex)
    return NULL;

  memset(tex, 0, sizeof *tex);
  tex->parent = (void *)this;
  tex->width = width;
  tex->height = height;
  tex->egl_image = NULL;
  tex->filename = file->GetFilename();
  tex->action = TEXTURE_ALLOC;
  sem_init(&tex->sync, 0, 0);

  pthread_mutex_lock(&m_texqueue_mutex);
  m_texqueue.push(tex);
  pthread_cond_broadcast(&m_texqueue_cond);
  pthread_mutex_unlock(&m_texqueue_mutex);

  // wait for function to have finished (in texture thread)
  sem_wait(&tex->sync);
  assert(tex->egl_image);
  assert(tex->texture);

  if (tex && tex->egl_image && tex->texture && omx_image.Decode(file->GetImageBuffer(), file->GetImageSize(), width, height, tex->egl_image, m_egl_display))
  {
    ret = true;
    *userdata = tex;
  }
  else
  {
    CLog::Log(LOGNOTICE, "%s: unable to decode to texture %s %dx%d", __func__, file->GetFilename(), width, height);
    DestroyTexture(tex);
  }
  return ret;
}

static bool ChooseConfig(EGLDisplay display, const EGLint *configAttrs, EGLConfig *config)
{
  EGLBoolean eglStatus = true;
  EGLint     configCount = 0;
  EGLConfig* configList = NULL;
  GLint m_result;
  // Find out how many configurations suit our needs
  eglStatus = eglChooseConfig(display, configAttrs, NULL, 0, &configCount);
  CheckError();

  if (!eglStatus || !configCount)
  {
    CLog::Log(LOGERROR, "EGL failed to return any matching configurations: %i", configCount);
    return false;
  }

  // Allocate room for the list of matching configurations
  configList = (EGLConfig*)malloc(configCount * sizeof(EGLConfig));
  if (!configList)
  {
    CLog::Log(LOGERROR, "EGL failure obtaining configuration list");
    return false;
  }

  // Obtain the configuration list from EGL
  eglStatus = eglChooseConfig(display, configAttrs, configList, configCount, &configCount);
  CheckError();
  if (!eglStatus || !configCount)
  {
    CLog::Log(LOGERROR, "EGL failed to populate configuration list: %d", eglStatus);
    return false;
  }

  // Select an EGL configuration that matches the native window
  *config = configList[0];

  free(configList);
  return m_result == EGL_SUCCESS;
}

void CRBP::CreateContext()
{
  EGLConfig egl_config;
  GLint m_result;

  m_egl_display = g_Windowing.GetEGLDisplay();
  eglInitialize(m_egl_display, NULL, NULL);
  CheckError();
  eglBindAPI(EGL_OPENGL_ES_API);
  CheckError();
  static const EGLint contextAttrs [] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  static const EGLint configAttrs [] = {
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,     16,
        EGL_STENCIL_SIZE,    0,
        EGL_SAMPLE_BUFFERS,  0,
        EGL_SAMPLES,         0,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
  };
  bool s = ChooseConfig(m_egl_display, configAttrs, &egl_config);
  CheckError();
  if (!s)
  {
    CLog::Log(LOGERROR, "%s: Could not find a compatible configuration",__FUNCTION__);
    return;
  }
  m_egl_context = eglCreateContext(m_egl_display, egl_config, g_Windowing.GetEGLContext(), contextAttrs);
  CheckError();
  if (m_egl_context == EGL_NO_CONTEXT)
  {
    CLog::Log(LOGERROR, "%s: Could not create a context",__FUNCTION__);
    return;
  }
  EGLSurface egl_surface = eglCreatePbufferSurface(m_egl_display, egl_config, NULL);
  CheckError();
  if (egl_surface == EGL_NO_SURFACE)
  {
    CLog::Log(LOGERROR, "%s: Could not create a surface",__FUNCTION__);
    return;
  }
  s = eglMakeCurrent(m_egl_display, egl_surface, egl_surface, m_egl_context);
  CheckError();
  if (!s)
  {
    CLog::Log(LOGERROR, "%s: Could not make current",__FUNCTION__);
    return;
  }
}

void CRBP::Process()
{
  bool firsttime = true;

  while(!m_bStop)
  {
    struct textureinfo *tex = NULL;
    pthread_mutex_lock(&m_texqueue_mutex);
    while (!m_bStop)
    {
      if (!m_texqueue.empty())
      {
        tex = m_texqueue.front();
        m_texqueue.pop();
        break;
      }
      int retcode = pthread_cond_wait(&m_texqueue_cond, &m_texqueue_mutex);
      if (retcode != 0)
        break;
    }
    pthread_mutex_unlock(&m_texqueue_mutex);

    if (firsttime)
      CreateContext();
    firsttime = false;

    assert(tex);
    //printf("Got job %s\n", tex->filename);
    if (tex->action == TEXTURE_ALLOC)
      AllocTextureInternal(tex);
    else if (tex->action == TEXTURE_DELETE)
      DestroyTextureInternal(tex);
    else assert(0);
  }
}

void CRBP::OnStartup()
{
}

void CRBP::OnExit()
{
}

#endif
