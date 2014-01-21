#pragma once
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

#if defined(HAVE_LIBOPENMAX)

#include "system_gl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "linux/OMXCore.h"
#include "linux/OMXClock.h"

#include "cores/dvdplayer/DVDStreamInfo.h"
#include "DVDVideoCodec.h"
#include "threads/Event.h"

#include <queue>
#include <semaphore.h>



typedef struct omx_codec_capability {
    // level is OMX_VIDEO_AVCPROFILETYPE, OMX_VIDEO_H263PROFILETYPE,
    // or OMX_VIDEO_MPEG4PROFILETYPE depending on context.
    OMX_U32 level;
    // level is OMX_VIDEO_AVCLEVELTYPE, OMX_VIDEO_H263LEVELTYPE,
    // or OMX_VIDEO_MPEG4PROFILETYPE depending on context.
    OMX_U32 profile;
} omx_codec_capability;

typedef struct omx_demux_packet {
  OMX_U8 *buff;
  int size;
  double dts;
  double pts;
} omx_demux_packet;

// an omx egl video frame
class COpenMaxVideoBuffer
{
public:
  OMX_BUFFERHEADERTYPE *omx_buffer;
  int width;
  int height;
  int index;
  int m_refCount;

  // used for egl based rendering if active
  EGLImageKHR egl_image;
  GLuint texture_id;
    // reference counting
  OpenMaxVideoBuffer* Retain();
  long                Release();

private:
  long                m_refs;
};

class COpenMaxVideo
{
public:
  COpenMaxVideo();
  virtual ~COpenMaxVideo();

  // Required overrides
  bool Open(CDVDStreamInfo &hints);
  void Close(void);
  int  Decode(uint8_t *pData, int iSize, double dts, double pts);
  void Reset(void);
  bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  void SetDropState(bool bDrop);
  // OpenMax decoder callback routines.
  OMX_ERRORTYPE DecoderFillBufferDone(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE* pBuffer);

protected:
  void QueryCodec(void);
  OMX_ERRORTYPE PrimeFillBuffers(void);
  OMX_ERRORTYPE AllocOMXInputBuffers(void);
  OMX_ERRORTYPE FreeOMXInputBuffers(void);
  bool AllocOMXOutputBuffers(void);
  bool FreeOMXOutputBuffers(void);
  static bool CallbackAllocOMXEGLTextures(void*);
  OMX_ERRORTYPE AllocOMXOutputEGLTextures(void);
  static bool CallbackFreeOMXEGLTextures(void*);
  OMX_ERRORTYPE FreeOMXOutputEGLTextures(void);

  // TODO Those should move into the base class. After start actions can be executed by callbacks.
  OMX_ERRORTYPE StartDecoder(void);
  OMX_ERRORTYPE StopDecoder(void);

  // EGL Resources
  EGLDisplay        m_egl_display;
  EGLContext        m_egl_context;

  // Video format
  DVDVideoPicture   m_videobuffer;
  bool              m_drop_state;
  int               m_decoded_width;
  int               m_decoded_height;

  std::queue<double> m_dts_queue;
  std::queue<omx_demux_packet> m_demux_queue;

  // OpenMax input buffers (demuxer packets)
  bool              m_omx_input_eos;
  //sem_t             *m_omx_flush_input;

  // OpenMax output buffers (video frames)
  pthread_mutex_t   m_omx_output_mutex;
  std::queue<OpenMaxVideoBuffer*> m_omx_output_busy;
  std::queue<OpenMaxVideoBuffer*> m_omx_output_ready;
  std::vector<OpenMaxVideoBuffer*> m_omx_output_buffers;
  bool              m_omx_output_eos;
  //sem_t             *m_omx_flush_output;

  bool              m_portChanging;

  volatile bool     m_videoplayback_done;

  bool              m_is_open;
  // initialize OpenMax and get decoder component
  bool Initialize( const CStdString &decoder_name);
  void Deinitialize();

  // Components
  COMXCoreComponent m_omx_decoder;
  COMXCoreComponent m_omx_egl_render;

  COMXCoreTunel     m_omx_tunnel;
  OMX_VIDEO_CODINGTYPE m_codingType;
  bool m_port_settings_changed;

  bool PortSettingsChanged();
  bool SendDecoderConfig(uint8_t *extradata, int extrasize);
  bool NaluFormatStartCodes(enum AVCodecID codec, uint8_t *extradata, int extrasize);
  void ReleaseOpenMaxBuffer(OpenMaxVideoBuffer *buffer);
};

// defined(HAVE_LIBOPENMAX)
#endif
