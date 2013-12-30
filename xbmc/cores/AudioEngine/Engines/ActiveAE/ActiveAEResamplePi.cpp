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

#include "system.h"

#if defined(TARGET_RASPBERRY_PI)

#include "ActiveAEResamplePi.h"
#include "linux/RBP.h"

#define CLASSNAME "CActiveAEResamplePi"

#define BUFFERSIZE (4*128*1024)

using namespace ActiveAE;

CActiveAEResample::CActiveAEResample()
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  m_pContext = NULL;
  m_loaded = false;
  if (m_dllAvUtil.Load() && m_dllSwResample.Load())
    m_loaded = true;

  m_Initialized = false;
  m_pConvert = NULL;
  m_last_src_fmt = AV_SAMPLE_FMT_NONE;
  m_last_dst_fmt = AV_SAMPLE_FMT_NONE;
  memset(m_rematrix, 0, sizeof(m_rematrix));
}

CActiveAEResample::~CActiveAEResample()
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  DeInit();
  if (m_pContext)
    m_dllSwResample.swr_free(&m_pContext);

  m_dllAvUtil.Unload();
  m_dllSwResample.Unload();
}

void CActiveAEResample::DeInit()
{
  CLog::Log(LOGDEBUG, "%s:%s", CLASSNAME, __func__);
  if (m_Initialized)
  {
    m_omx_mixer.FlushAll();
    m_omx_mixer.Deinitialize();
    m_Initialized = false;
  }
}

bool CActiveAEResample::Init(uint64_t dst_chan_layout, int dst_channels, int dst_rate, AVSampleFormat dst_fmt, int dst_bits, uint64_t src_chan_layout, int src_channels, int src_rate, AVSampleFormat src_fmt, int src_bits, bool upmix, bool normalize, CAEChannelInfo *remapLayout, AEQuality quality)
{
  CLog::Log(LOGINFO, "%s::%s remap:%d", CLASSNAME, __func__, !remapLayout);
  if (!m_loaded)
    return false;

  m_dst_chan_layout = dst_chan_layout;
  m_dst_channels = dst_channels;
  m_dst_rate = dst_rate;
  m_dst_fmt = dst_fmt;
  m_dst_bits = dst_bits;
  m_src_chan_layout = src_chan_layout;
  m_src_channels = src_channels;
  m_src_rate = src_rate;
  m_src_fmt = src_fmt;
  m_src_bits = src_bits;

  if (m_dst_chan_layout == 0)
    m_dst_chan_layout = m_dllAvUtil.av_get_default_channel_layout(m_dst_channels);
  if (m_src_chan_layout == 0)
    m_src_chan_layout = m_dllAvUtil.av_get_default_channel_layout(m_src_channels);

  m_pContext = m_dllSwResample.swr_alloc_set_opts(NULL, m_dst_chan_layout, m_dst_fmt, m_dst_rate,
                                                        m_src_chan_layout, m_src_fmt, m_src_rate,
                                                        0, NULL);
  if(quality == AE_QUALITY_HIGH)
  {
    m_dllAvUtil.av_opt_set_double(m_pContext, "cutoff", 1.0, 0);
    m_dllAvUtil.av_opt_set_int(m_pContext,"filter_size", 256, 0);
  }
  else if(quality == AE_QUALITY_MID)
  {
    // 0.97 is default cutoff so use (1.0 - 0.97) / 2.0 + 0.97
    m_dllAvUtil.av_opt_set_double(m_pContext, "cutoff", 0.985, 0);
    m_dllAvUtil.av_opt_set_int(m_pContext,"filter_size", 64, 0);
  }
  else if(quality == AE_QUALITY_LOW)
  {
    m_dllAvUtil.av_opt_set_double(m_pContext, "cutoff", 0.97, 0);
    m_dllAvUtil.av_opt_set_int(m_pContext,"filter_size", 32, 0);
  }

  if (m_dst_fmt == AV_SAMPLE_FMT_S32 || m_dst_fmt == AV_SAMPLE_FMT_S32P)
  {
    m_dllAvUtil.av_opt_set_int(m_pContext, "output_sample_bits", m_dst_bits, 0);
  }

  // tell resampler to clamp float values
  // not required for sink stage (remapLayout == true)
  if ((m_dst_fmt == AV_SAMPLE_FMT_FLT || m_dst_fmt == AV_SAMPLE_FMT_FLTP) &&
      (m_src_fmt == AV_SAMPLE_FMT_FLT || m_src_fmt == AV_SAMPLE_FMT_FLTP) &&
      !remapLayout && normalize)
  {
     m_dllAvUtil.av_opt_set_double(m_pContext, "rematrix_maxval", 1.0, 0);
  }

  if(!m_pContext)
  {
    CLog::Log(LOGERROR, "CActiveAEResample::Init - create context failed");
    return false;
  }
  if (remapLayout)
  {
    // one-to-one mapping of channels
    // remapLayout is the layout of the sink, if the channel is in our src layout
    // the channel is mapped by setting coef 1.0
    memset(m_rematrix, 0, sizeof(m_rematrix));
    m_dst_chan_layout = 0;
    for (unsigned int out=0; out<remapLayout->Count(); out++)
    {
      m_dst_chan_layout += (uint64_t) (1 << out);
      int idx = GetAVChannelIndex((*remapLayout)[out], m_src_chan_layout);
      if (idx >= 0)
      {
        m_rematrix[out][idx] = 1.0;
      }
    }

    m_dllAvUtil.av_opt_set_int(m_pContext, "out_channel_count", m_dst_channels, 0);
    m_dllAvUtil.av_opt_set_int(m_pContext, "out_channel_layout", m_dst_chan_layout, 0);

    if (m_dllSwResample.swr_set_matrix(m_pContext, (const double*)m_rematrix, AE_CH_MAX) < 0)
    {
      CLog::Log(LOGERROR, "CActiveAEResample::Init - setting channel matrix failed");
      return false;
    }
  }
  // stereo upmix
  else if (upmix && m_src_channels == 2 && m_dst_channels > 2)
  {
    memset(m_rematrix, 0, sizeof(m_rematrix));
    for (int out=0; out<m_dst_channels; out++)
    {
      uint64_t out_chan = m_dllAvUtil.av_channel_layout_extract_channel(m_dst_chan_layout, out);
      switch(out_chan)
      {
        case AV_CH_FRONT_LEFT:
        case AV_CH_BACK_LEFT:
        case AV_CH_SIDE_LEFT:
          m_rematrix[out][0] = 1.0;
          break;
        case AV_CH_FRONT_RIGHT:
        case AV_CH_BACK_RIGHT:
        case AV_CH_SIDE_RIGHT:
          m_rematrix[out][1] = 1.0;
          break;
        case AV_CH_FRONT_CENTER:
          m_rematrix[out][0] = 0.5;
          m_rematrix[out][1] = 0.5;
          break;
        case AV_CH_LOW_FREQUENCY:
          m_rematrix[out][0] = 0.5;
          m_rematrix[out][1] = 0.5;
          break;
        default:
          break;
      }
    }

    if (m_dllSwResample.swr_set_matrix(m_pContext, (const double*)m_rematrix, AE_CH_MAX) < 0)
    {
      CLog::Log(LOGERROR, "CActiveAEResample::Init - setting channel matrix failed");
      return false;
    }
  }

  if(m_dllSwResample.swr_init(m_pContext) < 0)
  {
    CLog::Log(LOGERROR, "CActiveAEResample::Init - init resampler failed");
    return false;
  }

  // This may be called before Application calls g_RBP.Initialise, so call it here too
  g_RBP.Initialize();

  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  if (!m_omx_mixer.Initialize("OMX.broadcom.audio_mixer", OMX_IndexParamAudioInit))
    CLog::Log(LOGERROR, "%s::%s - m_omx_mixer.Initialize omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  OMX_INIT_STRUCTURE(m_pcm_input);
  m_pcm_input.nPortIndex            = m_omx_mixer.GetInputPort();
  m_pcm_input.eNumData              = OMX_NumericalDataSigned;
  m_pcm_input.eEndian               = OMX_EndianLittle;
  m_pcm_input.bInterleaved          = OMX_TRUE;
  m_pcm_input.nBitPerSample         = 16;
  m_pcm_input.ePCMMode              = OMX_AUDIO_PCMModeLinear;
  m_pcm_input.nChannels             = src_channels;
  m_pcm_input.nSamplingRate         = src_rate;
  m_pcm_input.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
  m_pcm_input.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
  m_pcm_input.eChannelMapping[2] = OMX_AUDIO_ChannelMax;

  omx_err = m_omx_mixer.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_input);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - error m_omx_mixer in SetParameter omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  OMX_INIT_STRUCTURE(m_pcm_output);
  m_pcm_output.nPortIndex            = m_omx_mixer.GetOutputPort();
  m_pcm_output.eNumData              = OMX_NumericalDataSigned;
  m_pcm_output.eEndian               = OMX_EndianLittle;
  m_pcm_output.bInterleaved          = OMX_TRUE;
  m_pcm_output.nBitPerSample         = 16;
  m_pcm_output.ePCMMode              = OMX_AUDIO_PCMModeLinear;
  m_pcm_output.nChannels             = dst_channels;
  m_pcm_output.nSamplingRate         = dst_rate;
  m_pcm_output.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
  m_pcm_output.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
  m_pcm_output.eChannelMapping[2] = OMX_AUDIO_ChannelMax;

  omx_err = m_omx_mixer.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - error m_omx_mixer out SetParameter omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  // set up the number/size of buffers for decoder input
  OMX_PARAM_PORTDEFINITIONTYPE port_param;
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_mixer.GetInputPort();

  omx_err = m_omx_mixer.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - error get OMX_IndexParamPortDefinition (input) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  port_param.nBufferCountActual = std::max((unsigned int)port_param.nBufferCountMin, (unsigned int)1);
  port_param.nBufferSize = BUFFERSIZE;

  omx_err = m_omx_mixer.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - error set OMX_IndexParamPortDefinition (input) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  omx_err = m_omx_mixer.AllocInputBuffers();
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - Error alloc buffers 0x%08x", CLASSNAME, __func__, omx_err);

  // set up the number/size of buffers for decoder output
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_mixer.GetOutputPort();

  omx_err = m_omx_mixer.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - error get OMX_IndexParamPortDefinition (input) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  port_param.nBufferCountActual = std::max((unsigned int)port_param.nBufferCountMin, (unsigned int)1);
  port_param.nBufferSize = BUFFERSIZE;

  omx_err = m_omx_mixer.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - error set OMX_IndexParamPortDefinition (input) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  omx_err = m_omx_mixer.AllocOutputBuffers();
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - Error alloc buffers 0x%08x", CLASSNAME, __func__, omx_err);

  omx_err = m_omx_mixer.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - m_omx_mixer OMX_StateExecuting omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  m_Initialized = true;

  return true;
}

void CActiveAEResample::ConvertFormat(uint8_t *dst_planes[SWR_CH_MAX], AVSampleFormat dst_fmt, uint8_t *src_planes[SWR_CH_MAX], AVSampleFormat src_fmt, int samples)
{
  const int sample_rate = 48000;
  const int channels = 2;
  const int64_t ch_layout = m_dllAvUtil.av_get_default_channel_layout(channels);
 
  /* need to convert format */
  if (m_pConvert && (src_fmt != m_last_src_fmt || dst_fmt != m_last_dst_fmt))
  {
    m_dllSwResample.swr_free(&m_pConvert);
  }

  if (!m_pConvert)
  {
    m_last_src_fmt = src_fmt;
    m_last_dst_fmt = dst_fmt;
    m_pConvert = m_dllSwResample.swr_alloc_set_opts(NULL,
                      ch_layout, dst_fmt, sample_rate,
                      ch_layout, src_fmt, sample_rate,
                      0, NULL);

    if(!m_pConvert || m_dllSwResample.swr_init(m_pConvert) < 0)
      CLog::Log(LOGERROR, "%s::%s - Unable to initialise convert format %d to %d", CLASSNAME, __func__, (int)src_fmt, (int)dst_fmt);
  }

  /* use unaligned flag to keep output packed */
  if(m_dllSwResample.swr_convert(m_pConvert, dst_planes, samples, (const uint8_t **)src_planes, samples) < 0)
    CLog::Log(LOGERROR, "%s::%s - Unable to convert format %d to %d", CLASSNAME, __func__, (int)src_fmt, (int)dst_fmt);
}

int CActiveAEResample::Resample(uint8_t **dst_buffer, int dst_samples, uint8_t **src_buffer, int src_samples, double ratio)
{
  CLog::Log(LOGINFO, "%s::%s format:%d->%d rate:%d->%d chan:%d->%d samples %d->%d (%f) %d", CLASSNAME, __func__, (int)m_src_fmt, (int)m_dst_fmt, m_src_rate, m_dst_rate, m_src_channels, m_dst_channels, src_samples, dst_samples, ratio, m_Initialized);
  if (!m_Initialized)
    return 0;

  if (ratio != 1.0)
  {
    if (m_dllSwResample.swr_set_compensation(m_pContext,
                                            (dst_samples*ratio-dst_samples)*m_dst_rate/m_src_rate,
                                             dst_samples*m_dst_rate/m_src_rate) < 0)
    {
      CLog::Log(LOGERROR, "CActiveAEResample::Resample - set compensation failed");
      return 0;
    }
  }

  int ret = m_dllSwResample.swr_convert(m_pContext, dst_buffer, dst_samples, (const uint8_t**)src_buffer, src_samples);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CActiveAEResample::Resample - resample failed");
    return 0;
  }

  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;


  OMX_CONFIG_BRCMAUDIODOWNMIXCOEFFICIENTS8x8 mix;
  OMX_INIT_STRUCTURE(mix);

  assert(sizeof(mix.coeff)/sizeof(mix.coeff[0]) == 64);

  for(size_t out = 0; out < 8; ++out)
  {
    char s[25600], *t=s;
    for(size_t in = 0; in < 8; ++in)
       t += sprintf(t, "% 6.2f ", m_rematrix[out][in]);
    CLog::Log(LOGINFO, "  %s", s);
  }

  for(size_t out = 0; out < m_dst_channels; ++out)
    for(size_t in = 0; in < 8; ++in)
      mix.coeff[out*8+in] = static_cast<unsigned int>(0x10000 * m_rematrix[out][in]);

  mix.nPortIndex = m_omx_mixer.GetInputPort();
  omx_err = m_omx_mixer.SetConfig(OMX_IndexConfigBrcmAudioDownmixCoefficients8x8, &mix);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error setting mixer OMX_IndexConfigBrcmAudioDownmixCoefficients, error 0x%08x\n",
              CLASSNAME, __func__, omx_err);
    return false;
  }


  BYTE *tmp_buffer[SWR_CH_MAX] = {0};
  tmp_buffer[0] = (BYTE*)m_dllAvUtil.av_malloc(BUFFERSIZE + FF_INPUT_BUFFER_PADDING_SIZE);

//CLog::MemDump((char *)src_buffer[0], 256);
//CLog::MemDump((char *)dst_buffer[0], 256);

  ConvertFormat(tmp_buffer, AV_SAMPLE_FMT_S16, src_buffer, m_src_fmt, src_samples);

//CLog::MemDump((char *)tmp_buffer[0], 256);

  const int pitch = m_src_channels * 2;
  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;
  OMX_BUFFERHEADERTYPE *m_encoded_buffer = NULL;

  omx_buffer = m_omx_mixer.GetInputBuffer(1000);
  if(omx_buffer == NULL)			
    return false;

  omx_buffer->nOffset = omx_buffer->nFlags  = 0;
  omx_buffer->nFilledLen = src_samples * pitch;

  assert(omx_buffer->nFilledLen <= omx_buffer->nAllocLen);

  memcpy(omx_buffer->pBuffer, tmp_buffer[0], omx_buffer->nFilledLen);

  omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

  omx_err = m_omx_mixer.EmptyThisBuffer(omx_buffer);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s OMX_EmptyThisBuffer() failed with result(0x%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_encoded_buffer = m_omx_mixer.GetOutputBuffer();

  if(!m_encoded_buffer)
  {
    CLog::Log(LOGERROR, "%s::%s no output buffer", CLASSNAME, __func__);
    return false;
  }

  omx_err = m_omx_mixer.FillThisBuffer(m_encoded_buffer);
  if(omx_err != OMX_ErrorNone)
    return false;

  omx_err = m_omx_mixer.WaitForOutputDone(1000);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_mixer.WaitForOutputDone result(0x%x)", CLASSNAME, __func__, omx_err);
    return false;
  }
  assert(m_encoded_buffer->nFilledLen < m_encoded_buffer->nAllocLen);
//printf("%s::%s Got %d (%x)\n", CLASSNAME, __func__, m_encoded_buffer->nFilledLen, m_encoded_buffer->nFlags);

  if(m_omx_mixer.BadState())
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_mixer.BadState", CLASSNAME, __func__);
    return false;
  }

  m_dllAvUtil.av_free(tmp_buffer[0]);
  tmp_buffer[0] = m_encoded_buffer->pBuffer;

//CLog::MemDump((char *)tmp_buffer[0], 256);
//  ConvertFormat(dst_buffer, m_dst_fmt, tmp_buffer, AV_SAMPLE_FMT_S16, dst_samples);
//CLog::MemDump((char *)dst_buffer[0], 256);

  return ret;
}

int64_t CActiveAEResample::GetDelay(int64_t base)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  return m_dllSwResample.swr_get_delay(m_pContext, base);
}

int CActiveAEResample::GetBufferedSamples()
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  return m_dllAvUtil.av_rescale_rnd(m_dllSwResample.swr_get_delay(m_pContext, m_src_rate),
                                    m_dst_rate, m_src_rate, AV_ROUND_UP);
}

int CActiveAEResample::CalcDstSampleCount(int src_samples, int dst_rate, int src_rate)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  return m_dllAvUtil.av_rescale_rnd(src_samples, dst_rate, src_rate, AV_ROUND_UP);
}

int CActiveAEResample::GetSrcBufferSize(int samples)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  return m_dllAvUtil.av_samples_get_buffer_size(NULL, m_src_channels, samples, m_src_fmt, 1);
}

int CActiveAEResample::GetDstBufferSize(int samples)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  return m_dllAvUtil.av_samples_get_buffer_size(NULL, m_dst_channels, samples, m_dst_fmt, 1);
}

uint64_t CActiveAEResample::GetAVChannelLayout(CAEChannelInfo &info)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  uint64_t channelLayout = 0;
  if (info.HasChannel(AE_CH_FL))   channelLayout |= AV_CH_FRONT_LEFT;
  if (info.HasChannel(AE_CH_FR))   channelLayout |= AV_CH_FRONT_RIGHT;
  if (info.HasChannel(AE_CH_FC))   channelLayout |= AV_CH_FRONT_CENTER;
  if (info.HasChannel(AE_CH_LFE))  channelLayout |= AV_CH_LOW_FREQUENCY;
  if (info.HasChannel(AE_CH_BL))   channelLayout |= AV_CH_BACK_LEFT;
  if (info.HasChannel(AE_CH_BR))   channelLayout |= AV_CH_BACK_RIGHT;
  if (info.HasChannel(AE_CH_FLOC)) channelLayout |= AV_CH_FRONT_LEFT_OF_CENTER;
  if (info.HasChannel(AE_CH_FROC)) channelLayout |= AV_CH_FRONT_RIGHT_OF_CENTER;
  if (info.HasChannel(AE_CH_BC))   channelLayout |= AV_CH_BACK_CENTER;
  if (info.HasChannel(AE_CH_SL))   channelLayout |= AV_CH_SIDE_LEFT;
  if (info.HasChannel(AE_CH_SR))   channelLayout |= AV_CH_SIDE_RIGHT;
  if (info.HasChannel(AE_CH_TC))   channelLayout |= AV_CH_TOP_CENTER;
  if (info.HasChannel(AE_CH_TFL))  channelLayout |= AV_CH_TOP_FRONT_LEFT;
  if (info.HasChannel(AE_CH_TFC))  channelLayout |= AV_CH_TOP_FRONT_CENTER;
  if (info.HasChannel(AE_CH_TFR))  channelLayout |= AV_CH_TOP_FRONT_RIGHT;
  if (info.HasChannel(AE_CH_TBL))   channelLayout |= AV_CH_TOP_BACK_LEFT;
  if (info.HasChannel(AE_CH_TBC))   channelLayout |= AV_CH_TOP_BACK_CENTER;
  if (info.HasChannel(AE_CH_TBR))   channelLayout |= AV_CH_TOP_BACK_RIGHT;

  return channelLayout;
}

//CAEChannelInfo CActiveAEResample::GetAEChannelLayout(uint64_t layout)
//{
//  CAEChannelInfo channelLayout;
//  channelLayout.Reset();
//
//  if (layout & AV_CH_FRONT_LEFT           ) channelLayout += AE_CH_FL  ;
//  if (layout & AV_CH_FRONT_RIGHT          ) channelLayout += AE_CH_FR  ;
//  if (layout & AV_CH_FRONT_CENTER         ) channelLayout += AE_CH_FC  ;
//  if (layout & AV_CH_LOW_FREQUENCY        ) channelLayout += AE_CH_LFE ;
//  if (layout & AV_CH_BACK_LEFT            ) channelLayout += AE_CH_BL  ;
//  if (layout & AV_CH_BACK_RIGHT           ) channelLayout += AE_CH_BR  ;
//  if (layout & AV_CH_FRONT_LEFT_OF_CENTER ) channelLayout += AE_CH_FLOC;
//  if (layout & AV_CH_FRONT_RIGHT_OF_CENTER) channelLayout += AE_CH_FROC;
//  if (layout & AV_CH_BACK_CENTER          ) channelLayout += AE_CH_BC  ;
//  if (layout & AV_CH_SIDE_LEFT            ) channelLayout += AE_CH_SL  ;
//  if (layout & AV_CH_SIDE_RIGHT           ) channelLayout += AE_CH_SR  ;
//  if (layout & AV_CH_TOP_CENTER           ) channelLayout += AE_CH_TC  ;
//  if (layout & AV_CH_TOP_FRONT_LEFT       ) channelLayout += AE_CH_TFL ;
//  if (layout & AV_CH_TOP_FRONT_CENTER     ) channelLayout += AE_CH_TFC ;
//  if (layout & AV_CH_TOP_FRONT_RIGHT      ) channelLayout += AE_CH_TFR ;
//  if (layout & AV_CH_TOP_BACK_LEFT        ) channelLayout += AE_CH_BL  ;
//  if (layout & AV_CH_TOP_BACK_CENTER      ) channelLayout += AE_CH_BC  ;
//  if (layout & AV_CH_TOP_BACK_RIGHT       ) channelLayout += AE_CH_BR  ;
//
//  return channelLayout;
//}

AVSampleFormat CActiveAEResample::GetAVSampleFormat(AEDataFormat format)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  if      (format == AE_FMT_U8)     return AV_SAMPLE_FMT_U8;
  else if (format == AE_FMT_S16NE)  return AV_SAMPLE_FMT_S16;
  else if (format == AE_FMT_S32NE)  return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_S24NE4) return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_FLOAT)  return AV_SAMPLE_FMT_FLT;
  else if (format == AE_FMT_DOUBLE) return AV_SAMPLE_FMT_DBL;

  else if (format == AE_FMT_U8P)     return AV_SAMPLE_FMT_U8P;
  else if (format == AE_FMT_S16NEP)  return AV_SAMPLE_FMT_S16P;
  else if (format == AE_FMT_S32NEP)  return AV_SAMPLE_FMT_S32P;
  else if (format == AE_FMT_S24NE4P) return AV_SAMPLE_FMT_S32P;
  else if (format == AE_FMT_FLOATP)  return AV_SAMPLE_FMT_FLTP;
  else if (format == AE_FMT_DOUBLEP) return AV_SAMPLE_FMT_DBLP;

  return AV_SAMPLE_FMT_FLT;
}

AEDataFormat CActiveAEResample::GetAESampleFormat(AVSampleFormat format, int bits)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  if      (format == AV_SAMPLE_FMT_U8)   return AE_FMT_U8;
  else if (format == AV_SAMPLE_FMT_S16)  return AE_FMT_S16NE;
  else if (format == AV_SAMPLE_FMT_S32 && bits == 32)  return AE_FMT_S32NE;
  else if (format == AV_SAMPLE_FMT_S32 && bits == 24)  return AE_FMT_S24NE4;
  else if (format == AV_SAMPLE_FMT_FLT)  return AE_FMT_FLOAT;
  else if (format == AV_SAMPLE_FMT_DBL)  return AE_FMT_DOUBLE;

  else if (format == AV_SAMPLE_FMT_U8P)   return AE_FMT_U8P;
  else if (format == AV_SAMPLE_FMT_S16P)  return AE_FMT_S16NEP;
  else if (format == AV_SAMPLE_FMT_S32P && bits == 32)  return AE_FMT_S32NEP;
  else if (format == AV_SAMPLE_FMT_S32P && bits == 24)  return AE_FMT_S24NE4P;
  else if (format == AV_SAMPLE_FMT_FLTP)  return AE_FMT_FLOATP;
  else if (format == AV_SAMPLE_FMT_DBLP)  return AE_FMT_DOUBLEP;

  CLog::Log(LOGERROR, "CActiveAEResample::GetAESampleFormat - format not supported");
  return AE_FMT_INVALID;
}

uint64_t CActiveAEResample::GetAVChannel(enum AEChannel aechannel)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  switch (aechannel)
  {
  case AE_CH_FL:   return AV_CH_FRONT_LEFT;
  case AE_CH_FR:   return AV_CH_FRONT_RIGHT;
  case AE_CH_FC:   return AV_CH_FRONT_CENTER;
  case AE_CH_LFE:  return AV_CH_LOW_FREQUENCY;
  case AE_CH_BL:   return AV_CH_BACK_LEFT;
  case AE_CH_BR:   return AV_CH_BACK_RIGHT;
  case AE_CH_FLOC: return AV_CH_FRONT_LEFT_OF_CENTER;
  case AE_CH_FROC: return AV_CH_FRONT_RIGHT_OF_CENTER;
  case AE_CH_BC:   return AV_CH_BACK_CENTER;
  case AE_CH_SL:   return AV_CH_SIDE_LEFT;
  case AE_CH_SR:   return AV_CH_SIDE_RIGHT;
  case AE_CH_TC:   return AV_CH_TOP_CENTER;
  case AE_CH_TFL:  return AV_CH_TOP_FRONT_LEFT;
  case AE_CH_TFC:  return AV_CH_TOP_FRONT_CENTER;
  case AE_CH_TFR:  return AV_CH_TOP_FRONT_RIGHT;
  case AE_CH_TBL:  return AV_CH_TOP_BACK_LEFT;
  case AE_CH_TBC:  return AV_CH_TOP_BACK_CENTER;
  case AE_CH_TBR:  return AV_CH_TOP_BACK_RIGHT;
  default:
    return 0;
  }
}

int CActiveAEResample::GetAVChannelIndex(enum AEChannel aechannel, uint64_t layout)
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  return m_dllAvUtil.av_get_channel_layout_channel_index(layout, GetAVChannel(aechannel));
}

#endif
