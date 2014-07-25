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
#include "cores/dvdplayer/DVDStreamInfo.h"
#include "guilib/Geometry.h"

#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_util_params.h>


#define AUTOSOURCE -1

class CBaseTexture;
class COpenMaxVideoBuffer;

struct DVDVideoPicture;

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
  std::vector<ERenderFormat> m_formats;

  bool                 m_bConfigured;
  unsigned int         m_extended_format;
  unsigned int         m_destWidth;
  unsigned int         m_destHeight;
  int                  m_neededBuffers;


  CDVDStreamInfo    m_hints;
  CRect                     m_src_rect;
  CRect                     m_dst_rect;
  RENDER_STEREO_MODE        m_video_stereo_mode;
  RENDER_STEREO_MODE        m_display_stereo_mode;
  bool                      m_StereoInvert;

  virtual void SetVideoRect(const CRect& SrcRect, const CRect& DestRect);

};

#else
#include "LinuxRenderer.h"
#endif
