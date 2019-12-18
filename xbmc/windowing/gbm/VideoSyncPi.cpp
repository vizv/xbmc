/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncPi.h"
#include "ServiceBroker.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "threads/Thread.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <fcntl.h>
#include "xf86drm.h"
#include "xf86drmMode.h"

void CVideoSyncPi::VBlankHandler(int fd, unsigned int frame, unsigned int sec,
			   unsigned int usec)
{
  drmVBlank vbl;
  struct timeval end;
  double t;
  CLog::Log(LOGDEBUG, "vblank_handler count: %i\n", m_vbl_count);
  vbl.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT);
  vbl.request.sequence = 1;
  vbl.request.signal = reinterpret_cast<std::uintptr_t>(this);
  drmWaitVBlank(fd, &vbl);
  m_vbl_count++;
  uint64_t now = CurrentHostCounter();
  UpdateClock(1, now, m_refClock);

  if (m_vbl_count == 60)
  {
    gettimeofday(&end, NULL);
    t = end.tv_sec + end.tv_usec * 1e-6 -
      (m_start.tv_sec + m_start.tv_usec * 1e-6);
    CLog::Log(LOGDEBUG, "freq: %.02fHz\n", m_vbl_count / t);
    m_vbl_count = 0;
    m_start = end;
  }
}

static void vblank_handler(int fd, unsigned int frame, unsigned int sec,
			   unsigned int usec, void *data)
{
  CVideoSyncPi *s = reinterpret_cast<CVideoSyncPi*>(data);
  s->VBlankHandler(fd, frame, sec, usec);
}

bool CVideoSyncPi::Setup(PUPDATECLOCK func)
{
  UpdateClock = func;
  m_abort = false;
  CServiceBroker::GetWinSystem()->Register(this);
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: setting up RPi");
  return true;
}

void CVideoSyncPi::Run(CEvent& stopEvent)
{
  /* This shouldn't be very busy and timing is important so increase priority */
  CThread::GetCurrentThread()->SetPriority(CThread::GetCurrentThread()->GetPriority()+1);

  // hack - get this from somewhere!
  int fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (fd < 0)
  {
    CLog::Log(LOGWARNING, "CVideoReferenceClock: failed to open device (%d)", fd);
    return;
  }

  drmVBlank vbl;
  drmEventContext evctx;

  /* Get current count first */
  vbl.request.type = DRM_VBLANK_RELATIVE;
  vbl.request.sequence = 0;
  int ret = drmWaitVBlank(fd, &vbl);
  if (ret != 0)
  {
    CLog::Log(LOGWARNING, "drmWaitVBlank (relative) failed ret: %i\n", ret);
    return;
  }
  CLog::Log(LOGDEBUG, "starting count: %d\n", vbl.request.sequence);
  /* Queue an event for frame + 1 */
  vbl.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT);
  vbl.request.sequence = 1;
  vbl.request.signal = reinterpret_cast<std::uintptr_t>(this);
  ret = drmWaitVBlank(fd, &vbl);
  if (ret != 0)
  {
    CLog::Log(LOGWARNING, "drmWaitVBlank (relative, event) failed ret: %i\n", ret);
    return;
  }

  /* Set up our event handler */
  memset(&evctx, 0, sizeof evctx);
  evctx.version = DRM_EVENT_CONTEXT_VERSION;
  evctx.vblank_handler = vblank_handler;
  evctx.page_flip_handler = NULL;

  m_vbl_count = 0;
  gettimeofday(&m_start, NULL);

  while (!stopEvent.Signaled() && !m_abort)
  {
    struct timeval timeout = { .tv_sec = 3, .tv_usec = 0 };
    fd_set fds;
    int ret;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    ret = select(fd + 1, &fds, NULL, NULL, &timeout);
    if (ret <= 0) {
      CLog::Log(LOGWARNING, "select timed out or error (ret %d)\n", ret);
      break;
    } else if (FD_ISSET(0, &fds)) {
      CLog::Log(LOGDEBUG, "got event (ret %d)\n", ret);
    }
    ret = drmHandleEvent(fd, &evctx);
    if (ret != 0) {
      CLog::Log(LOGWARNING, "drmHandleEvent failed: %i\n", ret);
      return;
    }
  }
}

void CVideoSyncPi::Cleanup()
{
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: cleaning up RPi");
  CServiceBroker::GetWinSystem()->Unregister(this);
}

float CVideoSyncPi::GetFps()
{
  m_fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: fps: %.2f", m_fps);
  return m_fps;
}

void CVideoSyncPi::OnResetDisplay()
{
  m_abort = true;
}

void CVideoSyncPi::RefreshChanged()
{
  if (m_fps != CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS())
    m_abort = true;
}
