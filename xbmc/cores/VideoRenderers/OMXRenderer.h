#pragma once

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

#if defined(TARGET_RASPBERRY_PI)

#include "guilib/GraphicContext.h"
#include "RenderFlags.h"
#include "RenderFormats.h"
#include "BaseRenderer.h"
#include "RenderCapture.h"
#include "settings/VideoSettings.h"
#include "cores/VideoRenderers/RenderFlags.h"
#include "cores/VideoRenderers/RenderFormats.h"

#define ALIGN(value, alignment) (((value)+((alignment)-1))&~((alignment)-1))
#define CLAMP(a, min, max) ((a) > (max) ? (max) : ( (a) < (min) ? (min) : a ))

#define AUTOSOURCE -1

#define IMAGE_FLAG_WRITING   0x01 /* image is in use after a call to GetImage, caller may be reading or writing */
#define IMAGE_FLAG_READING   0x02 /* image is in use after a call to GetImage, caller is only reading */
#define IMAGE_FLAG_DYNAMIC   0x04 /* image was allocated due to a call to GetImage */
#define IMAGE_FLAG_RESERVED  0x08 /* image is reserved, must be asked for specifically used to preserve images */

#define IMAGE_FLAG_INUSE (IMAGE_FLAG_WRITING | IMAGE_FLAG_READING | IMAGE_FLAG_RESERVED)

class CBaseTexture;
class COpenMaxVideoBuffer;

struct DVDVideoPicture;

struct DRAWRECT
{
  float left;
  float top;
  float right;
  float bottom;
};


#define PLANE_Y 0
#define PLANE_U 1
#define PLANE_V 2
#define PLANE_UV 1

#define FIELD_FULL 0
#define FIELD_TOP 1
#define FIELD_BOT 2

class COMXRenderer : public CBaseRenderer
{
public:
  COMXRenderer();
  ~COMXRenderer();

  virtual void Update();
  virtual void SetupScreenshot() {};

  bool RenderCapture(CRenderCapture* capture);

  // Player functions
  virtual bool         Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, float fps, unsigned flags, ERenderFormat format, unsigned extended_format, unsigned int orientation);
  virtual int          GetImage(YV12Image *image, int source = AUTOSOURCE, bool readonly = false);
  virtual void         ReleaseImage(int source, bool preserve = false);
  virtual bool         AddVideoPicture(DVDVideoPicture* picture, int index);
  virtual void         FlipPage(int source);
  virtual unsigned int PreInit();
  virtual void         UnInit();
  virtual void         Reset(); /* resets renderer after seek for example */
  virtual bool         IsConfigured() { return m_bConfigured; }
  virtual void         AddProcessor(COpenMaxVideoBuffer *openMaxVideoBuffer, int index);
  virtual std::vector<ERenderFormat> SupportedFormats() { return m_formats; }

  virtual bool         Supports(ERENDERFEATURE feature);
  virtual bool         Supports(EDEINTERLACEMODE mode);
  virtual bool         Supports(EINTERLACEMETHOD method);
  virtual bool         Supports(ESCALINGMETHOD method);

  virtual EINTERLACEMETHOD AutoInterlaceMethod();

  void                 RenderUpdate(bool clear, DWORD flags = 0, DWORD alpha = 255);

  virtual unsigned int GetProcessorSize();
  virtual void         SetBufferSize(int numBuffers) { m_neededBuffers = numBuffers; }
  virtual unsigned int GetMaxBufferSize() { return NUM_BUFFERS; }

protected:
  virtual void Render(DWORD flags);
  int          NextYV12Texture();

  int  m_iYV12RenderBuffer;
  int  m_NumYV12Buffers;
  std::vector<ERenderFormat> m_formats;

  bool                 m_bConfigured;
  unsigned int         m_extended_format;
  unsigned int         m_destWidth;
  unsigned int         m_destHeight;
  int                  m_neededBuffers;
};

#else
#include "LinuxRenderer.h"
#endif
