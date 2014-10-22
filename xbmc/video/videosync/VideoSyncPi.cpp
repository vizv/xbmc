/*
 *      Copyright (C) 2005-2014 Team XBMC
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

#include "video/videosync/VideoSyncPi.h"
#include "guilib/GraphicContext.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "linux/RBP.h"

bool CVideoSyncPi::Setup(PUPDATECLOCK func)
{
  UpdateClock = func;
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: setting up RPi");
  return true;
}

void CVideoSyncPi::Run(volatile bool& stop)
{
  while (!stop)
  {
    g_RBP.WaitVsync();
    uint64_t now = CurrentHostCounter();
    UpdateClock(1, now);
  }
}

void CVideoSyncPi::Cleanup()
{
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: cleaning up RPi");
}

float CVideoSyncPi::GetFps()
{
  m_fps = g_graphicsContext.GetFPS();
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: fps: %.2f", m_fps);
  return m_fps;
}

#endif
