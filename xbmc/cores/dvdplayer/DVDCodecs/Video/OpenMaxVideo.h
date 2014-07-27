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

#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_util_params.h>

#include "cores/dvdplayer/DVDStreamInfo.h"
#include "DVDVideoCodec.h"
#include "threads/Event.h"
#include "xbmc/settings/VideoSettings.h"

#include <queue>
#include <semaphore.h>
#include <boost/shared_ptr.hpp>
#include "utils/StdString.h"
#include "guilib/Geometry.h"
#include "rendering/RenderSystem.h"

typedef struct omx_demux_packet {
  uint8_t *buff;
  int size;
  double dts;
  double pts;
} omx_demux_packet;

class COpenMaxVideo;
typedef boost::shared_ptr<COpenMaxVideo> OpenMaxVideoPtr;

enum { OMV_FLAGS_PORTS_SETTNGS_CHANGED = 1 << 0 };
// an omx egl video frame
class COpenMaxVideoBuffer
{
public:
  COpenMaxVideoBuffer(COpenMaxVideo *omv);
  virtual ~COpenMaxVideoBuffer();

  MMAL_BUFFER_HEADER_T *mmal_buffer;
  int width;
  int height;
  float m_aspect_ratio;
  int index;
  double dts;
  uint32_t m_changed_count;
  // reference counting
  COpenMaxVideoBuffer* Acquire();
  long                 Release();
  MMAL_ES_FORMAT_T    *GetFormat();
  COpenMaxVideo *m_omv;
  long m_refs;
private:
};

class COpenMaxVideo
{
public:
  COpenMaxVideo();
  virtual ~COpenMaxVideo();

  // Required overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options, OpenMaxVideoPtr myself);
  virtual void Dispose(void);
  virtual int  Decode(uint8_t *pData, int iSize, double dts, double pts);
  virtual void Reset(void);
  virtual bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual bool ClearPicture(DVDVideoPicture* pDvdVideoPicture);
  virtual unsigned GetAllowedReferences() { return 2; }
  virtual void SetDropState(bool bDrop);
  virtual const char* GetName(void) { return (const char*)m_pFormatName; }
  virtual bool GetCodecStats(double &pts, int &droppedPics);

  // OpenMax decoder callback routines.
  void ReleaseOpenMaxBuffer(COpenMaxVideoBuffer *buffer);
  void Recycle(MMAL_BUFFER_HEADER_T *buffer);

  // MMAL decoder callback routines.
  void dec_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
  uint32_t          m_changed_count;
  uint32_t          m_changed_count_dec;
  MMAL_ES_FORMAT_T *GetFormat() { return m_format; }

protected:
  void QueryCodec(void);
  void ReturnOpenMaxBuffer(COpenMaxVideoBuffer *buffer);

  // Video format
  bool              m_drop_state;
  int               m_decoded_width;
  int               m_decoded_height;
  unsigned int      m_egl_buffer_count;
  bool              m_finished;
  float             m_aspect_ratio;
  bool              m_forced_aspect_ratio;
  const char        *m_pFormatName;
  OpenMaxVideoPtr   m_myself;

  std::queue<double> m_dts_queue;
  std::queue<omx_demux_packet> m_demux_queue;

  // OpenMax output buffers (video frames)
  pthread_mutex_t   m_omx_output_mutex;
  std::vector<COpenMaxVideoBuffer*> m_omx_output_busy;
  std::queue<COpenMaxVideoBuffer*> m_omx_output_ready;
  std::vector<COpenMaxVideoBuffer*> m_omx_output_buffers;

  // initialize OpenMax and get decoder component
  bool Initialize( const CStdString &decoder_name);

  CDVDStreamInfo    m_hints;
  // Components
  bool              m_deinterlace_enabled;
  EDEINTERLACEMODE  m_deinterlace_request;
  unsigned int      m_decode_frame_number;
  double            m_decoderPts;
  unsigned int      m_droppedPics;
  bool              m_skipDeinterlaceFields;


  MMAL_COMPONENT_T *m_dec;
  MMAL_PORT_T *m_dec_input;
  MMAL_PORT_T *m_dec_output;
  MMAL_POOL_T *m_dec_input_pool;
  MMAL_POOL_T *m_dec_output_pool;
  MMAL_QUEUE_T *m_decoded;

  MMAL_ES_FORMAT_T *m_format;

  MMAL_FOURCC_T m_codingType;
  int change_dec_output_format();
};

// defined(HAVE_LIBOPENMAX)
#endif
