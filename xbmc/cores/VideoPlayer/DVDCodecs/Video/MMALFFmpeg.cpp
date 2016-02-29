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

#include "system.h"
#ifdef HAS_MMAL

#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/MMALRenderer.h"
#include "../DVDCodecUtils.h"
#include "MMALFFmpeg.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "linux/RBP.h"
#include "settings/AdvancedSettings.h"

extern "C" {
#include "libavutil/imgutils.h"
}

//-----------------------------------------------------------------------------
// MMAL Buffers
//-----------------------------------------------------------------------------

#define CLASSNAME "CMMALYUVBuffer"

CMMALYUVBuffer::CMMALYUVBuffer(const CBufferPoolPtr &pool, unsigned int width, unsigned int height, unsigned int aligned_width, unsigned int aligned_height)
    : m_pool(pool)
{
  m_width = width;
  m_height = height;
  m_aligned_width = aligned_width;
  m_aligned_height = aligned_height;
  m_aspect_ratio = 0.0f;
  m_refs = 0;
  mmal_buffer = nullptr;
  unsigned int size_pic = (m_aligned_width * m_aligned_height * 3) >> 1;
  gmem = new GPU_MEM_PTR_T;
  if (g_RBP.gpu_malloc_cached(size_pic, gmem) != 0)
    CLog::Log(LOGERROR, "%s::%s av_gpu_malloc_cached(%d) failed", CLASSNAME, __FUNCTION__, size_pic);
  gmem->opaque = (void *)this;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s buf:%p gmem:%p mmal:%p", CLASSNAME, __FUNCTION__, this, gmem, mmal_buffer);
}

CMMALYUVBuffer::~CMMALYUVBuffer()
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s buf:%p gmem:%p", CLASSNAME, __FUNCTION__, this, gmem);
  if (gmem)
  {
    g_RBP.gpu_free(gmem);
    delete gmem;
  }
  gmem = nullptr;
}

CMMALYUVBuffer* CMMALYUVBuffer::Acquire()
{
  long count = AtomicIncrement(&m_refs);
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p (%p) ref:%ld", CLASSNAME, __FUNCTION__, this, mmal_buffer, count);
  (void)count;
  return this;
}

long CMMALYUVBuffer::Release()
{
  long count = AtomicDecrement(&m_refs);
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p (%p) ref:%ld", CLASSNAME, __FUNCTION__, this, mmal_buffer, count);
  if (count == 0)
  {
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG,"%s::%s used:%d free:%d", CLASSNAME, __FUNCTION__, m_pool->sizeUsed(), m_pool->sizeFree());
    if (mmal_buffer)
      mmal_buffer_header_release(mmal_buffer);
    m_pool->ReleaseBuffer(this);
  }
  else
    assert(count > 0);
  return count;
}


//-----------------------------------------------------------------------------
// Buffer Pool
//-----------------------------------------------------------------------------

#undef CLASSNAME
#define CLASSNAME "CBufferPool"

CBufferPool::CBufferPool(CMMALRenderer *renderer)
  : m_renderer(renderer)
{
  m_closing = false;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - create %p", CLASSNAME, __FUNCTION__, this);
}

CBufferPool::~CBufferPool()
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - destroy %p", CLASSNAME, __FUNCTION__, this);
}

void CBufferPool::Close()
{
  CSingleLock lock(m_section);
  m_closing = true;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - close %p", CLASSNAME, __FUNCTION__, this);

  while (!m_freeBuffers.empty())
  {
    delete m_freeBuffers.front();
    m_freeBuffers.pop_front();
  }
  assert(m_freeBuffers.empty());
}

CMMALYUVBuffer *CBufferPool::GetFreeBuffer()
{
  CSingleLock lock(m_section);
  CMMALYUVBuffer *buffer = nullptr;
  if (!m_freeBuffers.empty())
  {
    buffer = m_freeBuffers.front();
    m_freeBuffers.pop_front();
  }
  return buffer;
}

void CBufferPool::AlignedSize(AVCodecContext *avctx, int &w, int &h)
{
  AVPicture picture;
  int unaligned;
  int stride_align[AV_NUM_DATA_POINTERS];

  avcodec_align_dimensions2(avctx, &w, &h, stride_align);
  // gpu requirements
  w = (w + 31) & ~31;
  h = (h + 15) & ~15;

  do {
    // NOTE: do not align linesizes individually, this breaks e.g. assumptions
    // that linesize[0] == 2*linesize[1] in the MPEG-encoder for 4:2:2
    av_image_fill_linesizes(picture.linesize, avctx->pix_fmt, w);
    // increase alignment of w for next try (rhs gives the lowest bit set in w)
    w += w & ~(w - 1);

    unaligned = 0;
    for (int i = 0; i < 4; i++)
      unaligned |= picture.linesize[i] % stride_align[i];
  } while (unaligned);
}

CMMALYUVBuffer *CBufferPool::GetBuffer(const CBufferPoolPtr &pool, unsigned int width, unsigned int height, AVCodecContext *avctx)
{
  CSingleLock lock(m_section);
  CMMALYUVBuffer *buffer = nullptr;
  while (1)
  {
    CMMALYUVBuffer *buf = GetFreeBuffer();
    if (!buf)
      break;
    if (buf->m_width == width && buf->m_height == height)
    {
      buffer = buf;
      break;
    }
    delete buf;
  }
  if (!buffer)
  {
    // ffmpeg requirements
    int aligned_width = width;
    int aligned_height = height;
    AlignedSize(avctx, aligned_width, aligned_height);
    buffer = new CMMALYUVBuffer(pool, width, height, aligned_width, aligned_height);
  }
  m_usedBuffers.push_back(buffer);
  return buffer;
}

void CBufferPool::ReleaseBuffer(CMMALYUVBuffer *buffer)
{
  CSingleLock lock(m_section);
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG,"%s::%s buf:%p gmem:%p used:%d free:%d closing:%d", CLASSNAME, __FUNCTION__, buffer, buffer->gmem, sizeUsed(), sizeFree(), m_closing);
  unsigned int size = sizeUsed();
  m_usedBuffers.erase(std::remove(m_usedBuffers.begin(), m_usedBuffers.end(), buffer), m_usedBuffers.end());
  assert(size == sizeUsed() + 1);
  buffer->mmal_buffer = NULL;
  if (m_closing)
    delete buffer;
  else
    m_freeBuffers.push_back(buffer);
}


//-----------------------------------------------------------------------------
// MMAL Decoder
//-----------------------------------------------------------------------------

#undef CLASSNAME
#define CLASSNAME "CDecoder"

CDecoder::CDecoder()
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - create %p", CLASSNAME, __FUNCTION__, this);
  m_shared = 0;
  m_avctx = nullptr;
  m_renderer = nullptr;
}

CDecoder::~CDecoder()
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - destroy %p used:%d free:%d", CLASSNAME, __FUNCTION__, this, m_pool->sizeUsed(), m_pool->sizeFree());
  Close();
}

long CDecoder::Release()
{
  return IHardwareDecoder::Release();
}

void CDecoder::Close()
{
  CSingleLock lock(m_section);
  m_pool->Close();
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG,"%s::%s used:%d free:%d", CLASSNAME, __FUNCTION__, m_pool->sizeUsed(), m_pool->sizeFree());
}

void CDecoder::FFReleaseBuffer(void *opaque, uint8_t *data)
{
  GPU_MEM_PTR_T *gmem = (GPU_MEM_PTR_T *)opaque;
  CMMALYUVBuffer *YUVBuffer = (CMMALYUVBuffer *)gmem->opaque;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG,"%s::%s buf:%p gmem:%p", CLASSNAME, __FUNCTION__, YUVBuffer, gmem);

  YUVBuffer->Release();
}

int CDecoder::FFGetBuffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
  CDVDVideoCodecFFmpeg *ctx = (CDVDVideoCodecFFmpeg*)avctx->opaque;
  CDecoder *dec = (CDecoder*)ctx->GetHardware();
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG,"%s::%s %dx%d format:%x flags:%x", CLASSNAME, __FUNCTION__, frame->width, frame->height, frame->format, flags);

  if ((avctx->codec->capabilities & AV_CODEC_CAP_DR1) == 0 || frame->format != AV_PIX_FMT_YUV420P)
  {
    assert(0);
    return avcodec_default_get_buffer2(avctx, frame, flags);
  }

  CSingleLock lock(dec->m_section);

  CMMALYUVBuffer *YUVBuffer = dec->m_pool->GetBuffer(dec->m_pool, frame->width, frame->height, dec->m_avctx);

  GPU_MEM_PTR_T *gmem = YUVBuffer->gmem;
  YUVBuffer->Acquire();
  AVBufferRef *buf = av_buffer_create((uint8_t *)gmem->arm, (YUVBuffer->m_aligned_width * YUVBuffer->m_aligned_height * 3)>>1, CDecoder::FFReleaseBuffer, gmem, AV_BUFFER_FLAG_READONLY);
  if (!buf)
  {
    CLog::Log(LOGERROR, "%s::%s av_buffer_create() failed", CLASSNAME, __FUNCTION__);
    dec->m_pool->ReleaseBuffer(YUVBuffer);
    return -1;
  }

  for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
  {
    frame->buf[i] = NULL;
    frame->data[i] = NULL;
    frame->linesize[i] = 0;
  }

  frame->buf[0] = buf;
  frame->linesize[0] = YUVBuffer->m_aligned_width;
  frame->linesize[1] = YUVBuffer->m_aligned_width>>1;
  frame->linesize[2] = YUVBuffer->m_aligned_width>>1;
  frame->data[0] = (uint8_t *)gmem->arm;
  frame->data[1] = frame->data[0] + YUVBuffer->m_aligned_width * YUVBuffer->m_aligned_height;
  frame->data[2] = frame->data[1] + (YUVBuffer->m_aligned_width>>1) * (YUVBuffer->m_aligned_height>>1);
  frame->extended_data = frame->data;
  // Leave extended buf alone

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG,"%s::%s buf:%p gmem:%p used:%d free:%d avbuf:%p:%p:%p", CLASSNAME, __FUNCTION__, YUVBuffer, gmem, dec->m_pool->sizeUsed(), dec->m_pool->sizeFree(), frame->data[0], frame->data[1], frame->data[2]);

  return 0;
}


bool CDecoder::Open(AVCodecContext *avctx, AVCodecContext* mainctx, enum AVPixelFormat fmt, unsigned int surfaces)
{
  CSingleLock lock(m_section);

  m_renderer = (CMMALRenderer *)mainctx->hwaccel_context;
  m_pool = std::make_shared<CBufferPool>(m_renderer);

  CLog::Log(LOGNOTICE, "%s::%s - m_renderer:%p", CLASSNAME, __FUNCTION__, m_renderer);
  assert(m_renderer);
  mainctx->hwaccel_context = nullptr;

  if (surfaces > m_shared)
    m_shared = surfaces;

  CLog::Log(LOGDEBUG, "%s::%s MMAL - source requires %d references", CLASSNAME, __FUNCTION__, avctx->refs);

  avctx->get_buffer2 = CDecoder::FFGetBuffer;
  mainctx->get_buffer2 = CDecoder::FFGetBuffer;

  m_avctx = mainctx;
  return true;
}

int CDecoder::Decode(AVCodecContext* avctx, AVFrame* frame)
{
  int status = Check(avctx);
  if(status)
    return status;

  if(frame)
    return VC_BUFFER | VC_PICTURE;
  else
    return VC_BUFFER;
}

MMAL_BUFFER_HEADER_T *CDecoder::GetMmal()
{
  MMAL_POOL_T *render_pool = m_renderer->GetPool(RENDER_FMT_MMAL, false);
  assert(render_pool);
  MMAL_BUFFER_HEADER_T *mmal_buffer = mmal_queue_timedwait(render_pool->queue, 500);
  if (!mmal_buffer)
  {
    CLog::Log(LOGERROR, "%s::%s - mmal_queue_get failed", CLASSNAME, __FUNCTION__);
    return nullptr;
  }
  mmal_buffer_header_reset(mmal_buffer);
  mmal_buffer->cmd = 0;
  mmal_buffer->offset = 0;
  mmal_buffer->flags = 0;
  mmal_buffer->user_data = NULL;
  return mmal_buffer;
}

bool CDecoder::GetPicture(AVCodecContext* avctx, AVFrame* frame, DVDVideoPicture* picture)
{
  CDVDVideoCodecFFmpeg* ctx = (CDVDVideoCodecFFmpeg*)avctx->opaque;
  bool ret = ctx->GetPictureCommon(picture);
  if (!ret)
    return false;

  if (frame->format != AV_PIX_FMT_YUV420P || frame->buf[1] != nullptr || frame->buf[0] == nullptr)
    return false;

  MMAL_BUFFER_HEADER_T *mmal_buffer = GetMmal();
  if (!mmal_buffer)
    return false;

  CSingleLock lock(m_section);

  AVBufferRef *buf = frame->buf[0];
  GPU_MEM_PTR_T *gmem = (GPU_MEM_PTR_T *)av_buffer_get_opaque(buf);
  mmal_buffer->data = (uint8_t *)gmem->vc_handle;
  mmal_buffer->alloc_size = mmal_buffer->length = gmem->numbytes;

  picture->MMALBuffer = (CMMALYUVBuffer *)gmem->opaque;
  assert(picture->MMALBuffer);
  picture->format = RENDER_FMT_MMAL;
  assert(!picture->MMALBuffer->mmal_buffer);
  picture->MMALBuffer->mmal_buffer = mmal_buffer;

  // need to flush ARM cache so GPU can see it (HEVC will have already done this)
  if (avctx->codec_id != AV_CODEC_ID_HEVC)
    g_RBP.gpu_cache_flush(gmem);

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - mmal:%p dts:%.3f pts:%.3f buf:%p gpu:%p", CLASSNAME, __FUNCTION__, picture->MMALBuffer->mmal_buffer, 1e-6*picture->dts, 1e-6*picture->pts, picture->MMALBuffer, gmem);
  return true;
}

int CDecoder::Check(AVCodecContext* avctx)
{
  CSingleLock lock(m_section);
  return 0;
}

unsigned CDecoder::GetAllowedReferences()
{
  return m_shared;
}

#endif
