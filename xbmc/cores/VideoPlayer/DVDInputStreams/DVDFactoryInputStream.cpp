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
#include "DVDFactoryInputStream.h"
#include "DVDInputStream.h"
#include "DVDInputStreamFile.h"
#include "DVDInputStreamNavigator.h"
#include "DVDInputStreamFFmpeg.h"
#include "DVDInputStreamPVRManager.h"
#ifdef HAVE_LIBBLURAY
#include "DVDInputStreamBluray.h"
#endif
#ifdef ENABLE_DVDINPUTSTREAM_STACK
#include "DVDInputStreamStack.h"
#endif
#include "FileItem.h"
#include "storage/MediaManager.h"
#include "URL.h"
#include "filesystem/File.h"
#include "utils/URIUtils.h"
#include "filesystem/Directory.h"
#include "settings/Settings.h"
#include "utils/log.h"


CDVDInputStream* CDVDFactoryInputStream::CreateInputStream(IVideoPlayer* pPlayer, CFileItem fileitem)
{
  std::string file = fileitem.GetPath();

  if (CSettings::GetInstance().GetBool("videoplayer.supportmvc"))
  {
    std::string ssif_file;
    if (URIUtils::IsProtocol(file, "bluray"))
    {
      CURL url(file);
      ssif_file = url.GetHostName();
    }
    else if (fileitem.IsBDFile())
    {
      std::string temp = URIUtils::GetDirectory(file);
      URIUtils::RemoveSlashAtEnd(temp);
      ssif_file = URIUtils::GetDirectory(temp);
    }
    else if(fileitem.IsDiscImage())
    {
      CURL url("udf://");
      url.SetHostName(file);
      ssif_file = url.Get();
    }
    if (!ssif_file.empty())
    {
      CURL url3(ssif_file.append("BDMV/STREAM/SSIF"));
      CFileItemList _3ditems;
      XFILE::CDirectory::GetDirectory(url3, _3ditems, XFILE::CDirectory::CHints(), true);
      if (!_3ditems.IsEmpty())
      {
        _3ditems.Sort(SortByTrackNumber,  SortOrderDescending);
        _3ditems.Sort(SortBySize, SortOrderDescending);
        fileitem.SetPath(_3ditems[0]->GetPath());
        CLog::Log(LOGNOTICE, "CDVDFactoryInputStream::CreateInputStream - <f1> %s <f2> %s <ssif> %s", CURL::Decode(file).c_str(), CURL::Decode(fileitem.GetPath()).c_str(), ssif_file.c_str());
        return (new CDVDInputStreamFile(fileitem));
      }
    }
    CLog::Log(LOGNOTICE, "CDVDFactoryInputStream::CreateInputStream - <f1> %s <ssif> %s", CURL::Decode(file).c_str(), ssif_file.c_str());
  }

  if (fileitem.IsDiscImage())
  {
#ifdef HAVE_LIBBLURAY
    CURL url("udf://");
    url.SetHostName(file);
    url.SetFileName("BDMV/index.bdmv");
    if(XFILE::CFile::Exists(url.Get()))
        return new CDVDInputStreamBluray(pPlayer, fileitem);
#endif

    return new CDVDInputStreamNavigator(pPlayer, fileitem);
  }

#ifdef HAS_DVD_DRIVE
  if(file.compare(g_mediaManager.TranslateDevicePath("")) == 0)
  {
#ifdef HAVE_LIBBLURAY
    if(XFILE::CFile::Exists(URIUtils::AddFileToFolder(file, "BDMV/index.bdmv")))
        return new CDVDInputStreamBluray(pPlayer, fileitem);
#endif

    return new CDVDInputStreamNavigator(pPlayer, fileitem);
  }
#endif

  if (fileitem.IsDVDFile(false, true))
    return (new CDVDInputStreamNavigator(pPlayer, fileitem));
  else if(file.substr(0, 6) == "pvr://")
    return new CDVDInputStreamPVRManager(pPlayer, fileitem);
#ifdef HAVE_LIBBLURAY
  else if (fileitem.IsType(".bdmv") || fileitem.IsType(".mpls") || file.substr(0, 7) == "bluray:")
    return new CDVDInputStreamBluray(pPlayer, fileitem);
#endif
  else if(file.substr(0, 6) == "rtp://"
       || file.substr(0, 7) == "rtsp://"
       || file.substr(0, 6) == "sdp://"
       || file.substr(0, 6) == "udp://"
       || file.substr(0, 6) == "tcp://"
       || file.substr(0, 6) == "mms://"
       || file.substr(0, 7) == "mmst://"
       || file.substr(0, 7) == "mmsh://")
    return new CDVDInputStreamFFmpeg(fileitem);
#ifdef ENABLE_DVDINPUTSTREAM_STACK
  else if(file.substr(0, 8) == "stack://")
    return new CDVDInputStreamStack(fileitem);
#endif
#ifdef HAS_LIBRTMP
  else if(file.substr(0, 7) == "rtmp://"
       || file.substr(0, 8) == "rtmpt://"
       || file.substr(0, 8) == "rtmpe://"
       || file.substr(0, 9) == "rtmpte://"
       || file.substr(0, 8) == "rtmps://")
    return new CDVDInputStreamFFmpeg(fileitem);
#endif
  else if (fileitem.IsInternetStream())
  {
    if (fileitem.IsType(".m3u8"))
      return new CDVDInputStreamFFmpeg(fileitem);

    if (fileitem.ContentLookup())
    {
      // request header
      fileitem.SetMimeType("");
      fileitem.FillInMimeType();
    }

    if (fileitem.GetMimeType() == "application/vnd.apple.mpegurl")
      return new CDVDInputStreamFFmpeg(fileitem);
  }

  // our file interface handles all these types of streams
  return (new CDVDInputStreamFile(fileitem));
}
