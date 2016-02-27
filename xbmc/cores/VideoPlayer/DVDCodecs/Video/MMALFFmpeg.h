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
#pragma once

#include <memory>
#include <queue>
#include "DVDCodecs/Video/DVDVideoCodecFFmpeg.h"
#include "libavcodec/avcodec.h"
#include "MMALCodec.h"

class CDecoder;
class CMMALRenderer;
struct MMAL_BUFFER_HEADER_T;
struct GPU_MEM_PTR_T;
class CBufferPool;
typedef std::shared_ptr<CBufferPool> CBufferPoolPtr;

// a mmal video frame
class CMMALYUVBuffer : public CMMALBuffer
{
public:
  CMMALYUVBuffer(const CBufferPoolPtr &pool, unsigned int width, unsigned int height, unsigned int aligned_width, unsigned int aligned_height);
  virtual ~CMMALYUVBuffer();

  GPU_MEM_PTR_T *gmem;
  // reference counting
  CMMALYUVBuffer* Acquire();
  long Release();
private:
  CBufferPoolPtr m_pool;
};

// interface between CDecoder and CMMALYUVBuffer that can outlive CDecoder
class CBufferPool
{
public:
  CBufferPool(CMMALRenderer *renderer);
  virtual ~CBufferPool();
  static void AlignedSize(AVCodecContext *avctx, int &w, int &h);
  CMMALYUVBuffer *GetFreeBuffer();
  CMMALYUVBuffer *GetBuffer(const CBufferPoolPtr &pool, unsigned int w, unsigned int h, AVCodecContext* avctx);
  void ReleaseBuffer(CMMALYUVBuffer *buffer);
  unsigned sizeUsed() { return m_usedBuffers.size(); }
  unsigned sizeFree() { return m_freeBuffers.size(); }
  void Close();
  CMMALRenderer *m_renderer;

private:
  CCriticalSection m_section;
  std::deque<CMMALYUVBuffer *> m_usedBuffers;
  std::deque<CMMALYUVBuffer *> m_freeBuffers;
  bool m_closing;
};

class CDecoder
  : public CDVDVideoCodecFFmpeg::IHardwareDecoder
{
public:
  CDecoder();
  virtual ~CDecoder();
  static void AlignedSize(AVCodecContext *avctx, int &w, int &h);
  virtual bool Open(AVCodecContext* avctx, AVCodecContext* mainctx, const enum AVPixelFormat, unsigned int surfaces);
  virtual int Decode(AVCodecContext* avctx, AVFrame* frame);
  virtual bool GetPicture(AVCodecContext* avctx, AVFrame* frame, DVDVideoPicture* picture);
  virtual int Check(AVCodecContext* avctx);
  virtual void Close();
  virtual const std::string Name() { return "mmal"; }
  virtual unsigned GetAllowedReferences();
  virtual long Release();

  static void FFReleaseBuffer(void *opaque, uint8_t *data);
  static int FFGetBuffer(AVCodecContext *avctx, AVFrame *pic, int flags);

protected:
  MMAL_BUFFER_HEADER_T *GetMmal();
  AVCodecContext *m_avctx;
  unsigned int m_shared;
  CCriticalSection m_section;
  CBufferPoolPtr m_pool;
  CMMALRenderer *m_renderer;
};

