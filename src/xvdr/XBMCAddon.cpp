/*
 *      xbmc-addon-xvdr - XVDR addon for XBMC
 *
 *      Copyright (C) 2010 Alwin Esch (Team XBMC)
 *      Copyright (C) 2012 Alexander Pipelka
 *
 *      https://github.com/pipelka/xbmc-addon-xvdr
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
 *  along with this program; if not, write to the Free Software
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "XBMCAddon.h"
#include "XBMCClient.h"
#include "XBMCSettings.h"

#include "xvdr/demux.h"
#include "xvdr/command.h"
#include "xvdr/connection.h"
#include "xvdr/packetbuffer.h"

#include "xbmc_pvr_dll.h"
#include "xbmc_addon_types.h"

#include <string.h>
#include <sstream>
#include <string>
#include <iostream>
#include <stdlib.h>

using namespace ADDON;
using namespace XVDR;

#define XVDR_HOOK_SETTINGS_CHANNELSCAN 1001

CHelper_libXBMC_addon* XBMC = NULL;
CHelper_libKODI_guilib* GUI = NULL;
CHelper_libXBMC_pvr* PVR = NULL;
CHelper_libXBMC_codec* CODEC = NULL;

Demux* mDemuxer = NULL;
cXBMCClient *mClient = NULL;
XVDR::Mutex addonMutex;

int CurrentChannel = 0;

static int priotable[] = { 0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,99,100 };

void ADDON_Cleanup() {
  delete GUI;
  delete PVR;
  delete XBMC;
  delete CODEC;
  GUI = NULL;
  PVR = NULL;
  XBMC = NULL;
  CODEC = NULL;
}

extern "C" {

/***********************************************************
 * Standard AddOn related public library functions
 ***********************************************************/

ADDON_STATUS ADDON_Create(void* hdl, void* props)
{
  XVDR::MutexLock lock(&addonMutex);

  if (!hdl || !props)
    return ADDON_STATUS_UNKNOWN;

  XBMC = new CHelper_libXBMC_addon;
  if (!XBMC->RegisterMe(hdl))
  {
    ADDON_Cleanup();
    return ADDON_STATUS_UNKNOWN;
  }

  GUI = new CHelper_libKODI_guilib;
  if (!GUI->RegisterMe(hdl))
    return ADDON_STATUS_UNKNOWN;

  PVR = new CHelper_libXBMC_pvr;
  if (!PVR->RegisterMe(hdl))
  {
    ADDON_Cleanup();
    return ADDON_STATUS_UNKNOWN;
  }

  CODEC = new CHelper_libXBMC_codec;
  if (!CODEC->RegisterMe(hdl))
  {
    ADDON_Cleanup();
    return ADDON_STATUS_UNKNOWN;
  }

  XBMC->Log(LOG_DEBUG, "Creating VDR XVDR PVR-Client");

  cXBMCSettings& s = cXBMCSettings::GetInstance();
  s.load();

  mClient = new cXBMCClient;
  mClient->SetTimeout(s.ConnectTimeout() * 1000);
  mClient->SetCompressionLevel(s.Compression() * 3);
  mClient->SetAudioType(s.AudioType());

  TimeMs RetryTimeout;
  bool bConnected = false;

  while (!(bConnected = mClient->Open(s.Hostname())) && RetryTimeout.Elapsed() < (uint32_t)s.ConnectTimeout() * 1000)
	XVDR::CondWait::SleepMs(100);

  if (!bConnected){
    ADDON_Cleanup();
    return ADDON_STATUS_LOST_CONNECTION;
  }

  if (!mClient->EnableStatusInterface(s.HandleMessages()))
  {
    return ADDON_STATUS_LOST_CONNECTION;
  }

  mClient->ChannelFilter(s.FTAChannels(), s.NativeLangOnly(), s.vcaids);
  mClient->SetUpdateChannels(s.UpdateChannels());

  PVR_MENUHOOK hook;

  // add menuhook if scanning is supported
  if(mClient->SupportChannelScan()) {
    hook.category = PVR_MENUHOOK_SETTING;
    hook.iHookId = XVDR_HOOK_SETTINGS_CHANNELSCAN;
    hook.iLocalizedStringId = 30008;
    PVR->AddMenuHook(&hook);
  }

  return ADDON_STATUS_OK;
}

ADDON_STATUS ADDON_GetStatus()
{
  return ADDON_STATUS_OK;
}

void ADDON_Destroy()
{
  XVDR::MutexLock lock(&addonMutex);

  delete mClient;
  mClient = NULL;

  ADDON_Cleanup();
}

bool ADDON_HasSettings()
{
  return true;
}

unsigned int ADDON_GetSettings(ADDON_StructSetting ***sSet)
{
  return 0;
}

ADDON_STATUS ADDON_SetSetting(const char *settingName, const void *settingValue)
{
  XVDR::MutexLock lock(&addonMutex);

  bool bChanged = false;
  cXBMCSettings& s = cXBMCSettings::GetInstance();
  bChanged = s.set(settingName, settingValue);

  if(!bChanged)
    return ADDON_STATUS_OK;

  if(strcmp(settingName, "host") == 0 || strcmp(settingName, "piconpath") == 0)
    return ADDON_STATUS_NEED_RESTART;

  s.checkValues();

  mClient->SetTimeout(s.ConnectTimeout() * 1000);
  mClient->SetCompressionLevel(s.Compression() * 3);
  mClient->SetAudioType(s.AudioType());

  if(!bChanged)
    return ADDON_STATUS_OK;

  mClient->EnableStatusInterface(s.HandleMessages());
  mClient->SetUpdateChannels(s.UpdateChannels());
  mClient->ChannelFilter(s.FTAChannels(), s.NativeLangOnly(), s.vcaids);

  return ADDON_STATUS_OK;
}

void ADDON_Stop()
{
}

void ADDON_FreeSettings()
{
}

void ADDON_Announce(const char *flag, const char *sender, const char *message, const void *data)
{
}

/***********************************************************
 * PVR Client AddOn specific public library functions
 ***********************************************************/

PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities)
{
  pCapabilities->bSupportsEPG                = true;
  pCapabilities->bSupportsTV                 = true;
  pCapabilities->bSupportsRadio              = true;
  pCapabilities->bSupportsRecordings         = true;
  pCapabilities->bSupportsTimers             = true;
  pCapabilities->bSupportsChannelGroups      = true;
  pCapabilities->bSupportsChannelScan        = true; //false;
  pCapabilities->bHandlesInputStream         = true;
  pCapabilities->bHandlesDemuxing            = true;

  pCapabilities->bSupportsRecordingPlayCount = true;
  pCapabilities->bSupportsRecordingEdl       = true;
  pCapabilities->bSupportsLastPlayedPosition = true;

  return PVR_ERROR_NO_ERROR;
}

const char * GetBackendName(void)
{
  static std::string BackendName = mClient ? mClient->GetServerName() : "unknown";
  return BackendName.c_str();
}

const char * GetBackendVersion(void)
{
  static std::string BackendVersion;
  if (mClient) {
    std::stringstream format;
    format << mClient->GetVersion() << "(Protocol: " << mClient->GetProtocol() << ")";
    BackendVersion = format.str();
  }
  return BackendVersion.c_str();
}

const char * GetConnectionString(void)
{
  static std::string ConnectionString;
  std::stringstream format;

  if (mClient) {
    format << cXBMCSettings::GetInstance().Hostname();
  }
  else {
    format << cXBMCSettings::GetInstance().Hostname() << " (addon error!)";
  }
  ConnectionString = format.str();
  return ConnectionString.c_str();
}

PVR_ERROR GetDriveSpace(long long *iTotal, long long *iUsed)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  return (mClient->GetDriveSpace(iTotal, iUsed) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);
}

PVR_ERROR OpenDialogChannelScan(void)
{
  mClient->DialogChannelScan();
  return PVR_ERROR_NO_ERROR;
}

/*******************************************/
/** PVR EPG Functions                     **/

PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL &channel, time_t iStart, time_t iEnd)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  mClient->Lock();

  mClient->SetHandle(handle);
  PVR_ERROR rc = (mClient->GetEPGForChannel(channel.iUniqueId, iStart, iEnd) ? PVR_ERROR_NO_ERROR: PVR_ERROR_SERVER_ERROR);

  mClient->Unlock();
  return rc;
}


/*******************************************/
/** PVR Channel Functions                 **/

int GetChannelsAmount(void)
{
  if (!mClient)
    return 0;

  return mClient->GetChannelsCount();
}

PVR_ERROR GetChannels(ADDON_HANDLE handle, bool bRadio)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  mClient->Lock();

  mClient->SetHandle(handle);
  PVR_ERROR rc = (mClient->GetChannelsList(bRadio) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);


  mClient->Unlock();
  return rc;
}


/*******************************************/
/** PVR Channelgroups Functions           **/

int GetChannelGroupsAmount()
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  return mClient->GetChannelGroupCount(cXBMCSettings::GetInstance().AutoChannelGroups());
}

PVR_ERROR GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  mClient->Lock();

  PVR_ERROR rc = PVR_ERROR_NO_ERROR;
  mClient->SetHandle(handle);

  if(mClient->GetChannelGroupCount(cXBMCSettings::GetInstance().AutoChannelGroups()) > 0)
    rc = mClient->GetChannelGroupList(bRadio) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR;

  mClient->Unlock();
  return rc;
}

PVR_ERROR GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP &group)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  mClient->Lock();

  mClient->SetHandle(handle);
  PVR_ERROR rc = (mClient->GetChannelGroupMembers(group.strGroupName, group.bIsRadio) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);

  mClient->Unlock();
  return rc;
}


/*******************************************/
/** PVR Timer Functions                   **/

PVR_ERROR GetTimerTypes(PVR_TIMER_TYPE types[], int *size)
{
  /* TODO: Implement this to get support for the timer features introduced with PVR API 1.9.7 */
  return PVR_ERROR_NOT_IMPLEMENTED;
}

int GetTimersAmount(void)
{
  if (!mClient)
    return 0;

  return mClient->GetTimersCount();
}

PVR_ERROR GetTimers(ADDON_HANDLE handle)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  mClient->Lock();

  mClient->SetHandle(handle);
  PVR_ERROR rc = (mClient->GetTimersList() ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);

  mClient->Unlock();
  return rc;
}

PVR_ERROR AddTimer(const PVR_TIMER &timer)
{
  Timer xvdrtimer;
  xvdrtimer << timer;

  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  return mClient->AddTimer(xvdrtimer) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR DeleteTimer(const PVR_TIMER &timer, bool bForce)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  int rc = mClient->DeleteTimer(timer.iClientIndex, bForce);

  if(rc == XVDR_RET_OK)
	return PVR_ERROR_NO_ERROR;
  else if(rc == XVDR_RET_RECRUNNING)
    return PVR_ERROR_RECORDING_RUNNING;

  return PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR UpdateTimer(const PVR_TIMER &timer)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  Timer xvdrtimer;
  xvdrtimer << timer;

  return mClient->UpdateTimer(xvdrtimer) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR;
}


/*******************************************/
/** PVR Recording Functions               **/

int GetRecordingsAmount(bool deleted)
{
  if (!mClient)
    return 0;

  mClient->Lock();

  int result = mClient->GetRecordingsCount();

  mClient->Unlock();
  return result;
}

PVR_ERROR GetRecordings(ADDON_HANDLE handle, bool deleted)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  mClient->Lock();

  mClient->SetHandle(handle);
  PVR_ERROR rc = mClient->GetRecordingsList() ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR;

  mClient->Unlock();
  return rc;
}

PVR_ERROR RenameRecording(const PVR_RECORDING &recording)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  return mClient->RenameRecording(recording.strRecordingId, recording.strTitle) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR DeleteRecording(const PVR_RECORDING &recording)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  int rc = mClient->DeleteRecording(recording.strRecordingId);

  if(rc == XVDR_RET_OK)
    return PVR_ERROR_NO_ERROR;
  else if(rc == XVDR_RET_DATALOCKED)
    return PVR_ERROR_RECORDING_RUNNING;
  else
    return PVR_ERROR_SERVER_ERROR;
}

/*******************************************/
/** PVR Live Stream Functions             **/

void ChannelNotification(Demux::SwitchStatus status)
{
  switch (status)
  {
    // active recording
    case Demux::SC_ACTIVE_RECORDING:
      mClient->Notification(INFO, mClient->GetLocalizedString(30062));
      break;
    // all receivers busy
    case Demux::SC_DEVICE_BUSY:
      mClient->Notification(INFO, mClient->GetLocalizedString(30063));
      break;
    // encrypted channel
    case Demux::SC_ENCRYPTED:
      mClient->Notification(INFO, mClient->GetLocalizedString(30066));
      break;
    // error on switching channel
    case Demux::SC_ERROR:
      mClient->Notification(INFO, mClient->GetLocalizedString(30064));
      break;
    // invalid channel
    case Demux::SC_INVALID_CHANNEL:
      mClient->Notification(FAILURE, mClient->GetLocalizedString(30065), "");
      break;
  }
}

bool OpenLiveStream(const PVR_CHANNEL &channel)
{
  mClient->Lock();

  if (mDemuxer)
  {
    mDemuxer->Close();
    delete mDemuxer;
  }

  cXBMCSettings& s = cXBMCSettings::GetInstance();
  PacketBuffer* buf = NULL;

  // simple timeshift
  if(s.TSMethod() == 0) {
    XBMC->Log(LOG_NOTICE, "doing simple server-side timeshift");
  }

  // full-timeshift (ram)
  else if(s.TSMethod() == 1) {
    buf = (s.TSBufferSize() > 0) ? PacketBuffer::create(s.TSBufferSize() * 1024 * 1024) : NULL;
    if(buf != NULL) {
      XBMC->Log(LOG_NOTICE, "doing timeshift in memory using %f Mb RAM", s.TSBufferSize());
    }
  }

  // full-timeshift (hdd)
  else if(s.TSMethod() == 2) {
    std::string tsfile = s.TSFolder();

    // use temp folder if tsfolder is empty
    if(tsfile.empty()) {
      XVDR::ClientInterface::GetTempFolder(tsfile);
    }

    XVDR::ClientInterface::TrimPath(tsfile, true);
    tsfile += "xvdr-timeshift.dat";

    buf = (s.TSBufferSizeHDD() > 0) ? PacketBuffer::create(s.TSBufferSizeHDD() * 1024 * 1024, tsfile) : NULL;
    if(buf != NULL) {
      XBMC->Log(LOG_NOTICE, "doing timeshift on hdd at '%s' using %f Mb", tsfile.c_str(), s.TSBufferSizeHDD());
    }
  }

  mDemuxer = new Demux(mClient, buf);
  mDemuxer->SetTimeout(cXBMCSettings::GetInstance().ConnectTimeout() * 1000);
  mDemuxer->SetAudioType(cXBMCSettings::GetInstance().AudioType());
  mDemuxer->SetPriority(priotable[cXBMCSettings::GetInstance().Priority()]);

  if(!channel.bIsRadio) {
    mDemuxer->SetStartWithIFrame(cXBMCSettings::GetInstance().StartWithIFrame());
  }

  const cXBMCSettings& settings = cXBMCSettings::GetInstance();
  Demux::SwitchStatus status = mDemuxer->OpenChannel(settings.Hostname(), channel.iUniqueId, settings.ClientName());

  if (status == Demux::SC_OK)
    CurrentChannel = channel.iChannelNumber;
  else
    ChannelNotification(status);

  mClient->Unlock();

  return (status == Demux::SC_OK);
}

void CloseLiveStream(void)
{
  mClient->Lock();

  if (mDemuxer)
  {
    mDemuxer->Close();
    delete mDemuxer;
    mDemuxer = NULL;
  }

  mClient->Unlock();
}

PVR_ERROR GetStreamProperties(PVR_STREAM_PROPERTIES* pProperties)
{
  if (!mDemuxer)
    return PVR_ERROR_SERVER_ERROR;

  *pProperties << mDemuxer->GetStreamProperties();

  return PVR_ERROR_NO_ERROR;
}

void DemuxAbort(void)
{
  if (!mDemuxer)
    return;

  mDemuxer->Abort();
}

void DemuxReset(void)
{
}

void DemuxFlush(void)
{
}

DemuxPacket* DemuxRead(void)
{
  if (!mDemuxer)
    return NULL;

  return (DemuxPacket*)mDemuxer->Read();
}

int GetCurrentClientChannel(void)
{
  if (mDemuxer)
    return CurrentChannel;

  return -1;
}

bool SwitchChannel(const PVR_CHANNEL &channel)
{
  mClient->Lock();

  if (mDemuxer == NULL)
  {
    mClient->Unlock();
    return false;
  }

  bool rc = false;
  mDemuxer->SetTimeout(cXBMCSettings::GetInstance().ConnectTimeout() * 1000);
  mDemuxer->SetAudioType(cXBMCSettings::GetInstance().AudioType());
  mDemuxer->SetPriority(priotable[cXBMCSettings::GetInstance().Priority()]);

  if(!channel.bIsRadio) {
    mDemuxer->SetStartWithIFrame(cXBMCSettings::GetInstance().StartWithIFrame());
  }

  Demux::SwitchStatus status = mDemuxer->SwitchChannel(channel.iUniqueId);

  if(status == Demux::SC_OK)
    CurrentChannel = channel.iChannelNumber;
  else
    ChannelNotification(status);

  mClient->Unlock();

  return (status == Demux::SC_OK);
}

PVR_ERROR SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
  mClient->Lock();

  if (mDemuxer == NULL)
  {
    mClient->Unlock();
    return PVR_ERROR_SERVER_ERROR;
  }

  mDemuxer->RequestSignalInfo();
  signalStatus << mDemuxer->GetSignalStatus();

  mClient->Unlock();
  return PVR_ERROR_NO_ERROR;
}


/*******************************************/
/** PVR Recording Stream Functions        **/

void CloseRecordedStream();

bool OpenRecordedStream(const PVR_RECORDING &recording)
{
  if(!mClient)
    return false;

  mClient->CloseRecording();
  mClient->SetTimeout(cXBMCSettings::GetInstance().ConnectTimeout() * 1000);

  return mClient->OpenRecording(recording.strRecordingId);
}

void CloseRecordedStream(void)
{
  if (!mClient)
    return;

  mClient->CloseRecording();
}

int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
  if (!mClient)
    return -1;

  return mClient->ReadRecording(pBuffer, iBufferSize);
}

long long SeekRecordedStream(long long iPosition, int iWhence /* = SEEK_SET */)
{
  if (mClient)
    return mClient->SeekRecording(iPosition, iWhence);

  return -1;
}

long long PositionRecordedStream(void)
{
  if (mClient)
    return mClient->RecordingPosition();

  return 0;
}

long long LengthRecordedStream(void)
{
  if (mClient)
    return mClient->RecordingLength();

  return 0;
}

bool CanPauseStream()
{
  return true;
}

bool CanSeekStream()
{
  mClient->Lock();
  bool rc = (mDemuxer != NULL) && mDemuxer->CanSeekStream();
  mClient->Unlock();

  return rc;
}

void PauseStream(bool bPaused)
{
  mClient->Lock();

  if(mDemuxer != NULL)
    mDemuxer->Pause(bPaused);

  mClient->Unlock();
}

bool SeekTime(int time, bool backwards, double *startpts)
{
  mClient->Lock();
  bool rc = (mDemuxer != NULL) && mDemuxer->SeekTime(time, backwards, startpts);
  mClient->Unlock();
  return rc;
}

const char* GetPVRAPIVersion(void)
{
  static const char *strApiVersion = XBMC_PVR_API_VERSION;
  return strApiVersion;
}

const char* GetMininumPVRAPIVersion(void)
{
  static const char *strMinApiVersion = XBMC_PVR_MIN_API_VERSION;
  return strMinApiVersion;
}

const char* GetGUIAPIVersion(void)
{
  static const char *strApiVersion = KODI_GUILIB_API_VERSION;
  return strApiVersion;
}

const char* GetMininumGUIAPIVersion(void)
{
  static const char *strMinApiVersion = KODI_GUILIB_MIN_API_VERSION;
  return strMinApiVersion;
}

PVR_ERROR SetRecordingPlayCount(const PVR_RECORDING &recording, int count)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  if(!mClient->SetRecordingPlayCount(recording.strRecordingId, count))
    return PVR_ERROR_SERVER_ERROR;

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR SetRecordingLastPlayedPosition(const PVR_RECORDING &recording, int lastplayedposition)
{
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  return mClient->SetRecordingLastPosition(recording.strRecordingId, lastplayedposition) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR;
}

int GetRecordingLastPlayedPosition(const PVR_RECORDING &recording)
{
  if (!mClient)
    return -1;

  return mClient->GetRecordingLastPosition(recording.strRecordingId);
}

PVR_ERROR CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item) {
  switch(menuhook.iHookId) {
    case XVDR_HOOK_SETTINGS_CHANNELSCAN:
      OpenDialogChannelScan();
      return PVR_ERROR_NO_ERROR;
  }

  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR GetRecordingEdl(const PVR_RECORDING& recording, PVR_EDL_ENTRY edl[], int *size) {
  if (!mClient)
    return PVR_ERROR_SERVER_ERROR;

  RecordingEdl list;

  if(!mClient->LoadRecordingEdl(recording.strRecordingId, list)) {
    XBMC->Log(LOG_DEBUG, "unable to load edl !");
    *size = 0;
    return PVR_ERROR_NO_ERROR;
  }

  int maxsize = *size;
  *size = 0;

  int64_t start = 0;
  int64_t end = 0;

  for(RecordingEdl::iterator i = list.begin(); i != list.end(); i++) {
    if(*size == maxsize) {
      break;
    }

    // convert in/out marks (scene) into cut marks
    end = (int64_t)((double)(i->FrameBegin * 1000) / i->Fps);

    if(start < end) {
      edl[*size].type = PVR_EDL_TYPE_CUT;
      edl[*size].start = start;
      edl[*size].end = end;
      (*size)++;
    }

    start = (int64_t)((double)(i->FrameEnd * 1000) / i->Fps);
  }

  // add last part
  if(*size > 0) {
    edl[*size].type = PVR_EDL_TYPE_CUT;
    edl[*size].start = start;
    edl[*size].end = 1000 * 60 * 60 * 24;
    (*size)++;
  }

  return PVR_ERROR_NO_ERROR;
}

const char* GetBackendHostname() {
  return cXBMCSettings::GetInstance().Hostname().c_str();
}

bool IsTimeshifting() {
  return false;
}

/** UNUSED API FUNCTIONS */
PVR_ERROR DeleteChannel(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR RenameChannel(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR MoveChannel(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR OpenDialogChannelSettings(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR OpenDialogChannelAdd(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR UndeleteRecording(const PVR_RECORDING& recording) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR DeleteAllRecordingsFromTrash() { return PVR_ERROR_NOT_IMPLEMENTED; }
int ReadLiveStream(unsigned char *pBuffer, unsigned int iBufferSize) { return 0; }
long long SeekLiveStream(long long iPosition, int iWhence /* = SEEK_SET */) { return -1; }
long long PositionLiveStream(void) { return -1; }
long long LengthLiveStream(void) { return -1; }
const char * GetLiveStreamURL(const PVR_CHANNEL &channel) { return ""; }
unsigned int GetChannelSwitchDelay(void) { return 0; }
void SetSpeed(int speed) {};

time_t GetPlayingTime() { return 0; }
time_t GetBufferTimeStart() { return 0; }
time_t GetBufferTimeEnd() { return 0; }
}
