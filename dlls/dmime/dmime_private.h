/* DirectMusicInteractiveEngine Private Include
 *
 * Copyright (C) 2003-2004 Rok Mandeljc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef __WINE_DMIME_PRIVATE_H
#define __WINE_DMIME_PRIVATE_H

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "wingdi.h"
#include "winuser.h"

#include "wine/debug.h"
#include "wine/list.h"
#include "wine/unicode.h"
#include "winreg.h"
#include "objbase.h"

#include "dmusici.h"
#include "dmusicf.h"
#include "dmusics.h"

/*****************************************************************************
 * Interfaces
 */
typedef struct IDirectMusicPerformance8Impl IDirectMusicPerformance8Impl;
typedef struct IDirectMusicSegment8Impl IDirectMusicSegment8Impl;
typedef struct IDirectMusicSegmentState8Impl IDirectMusicSegmentState8Impl;
typedef struct IDirectMusicGraphImpl IDirectMusicGraphImpl;
typedef struct IDirectMusicAudioPathImpl IDirectMusicAudioPathImpl;
typedef struct IDirectMusicTool8Impl IDirectMusicTool8Impl;
typedef struct IDirectMusicPatternTrackImpl IDirectMusicPatternTrackImpl;

typedef struct IDirectMusicLyricsTrack IDirectMusicLyricsTrack;
typedef struct IDirectMusicMarkerTrack IDirectMusicMarkerTrack;
typedef struct IDirectMusicParamControlTrack IDirectMusicParamControlTrack;
typedef struct IDirectMusicSegTriggerTrack IDirectMusicSegTriggerTrack;
typedef struct IDirectMusicSeqTrack IDirectMusicSeqTrack;
typedef struct IDirectMusicSysExTrack IDirectMusicSysExTrack;
typedef struct IDirectMusicTempoTrack IDirectMusicTempoTrack;
typedef struct IDirectMusicTimeSigTrack IDirectMusicTimeSigTrack;
typedef struct IDirectMusicWaveTrack IDirectMusicWaveTrack;
	
/*****************************************************************************
 * Predeclare the interface implementation structures
 */
extern IDirectMusicPerformance8Vtbl DirectMusicPerformance8_Vtbl;

extern IUnknownVtbl                  DirectMusicSegment8_Unknown_Vtbl;
extern IDirectMusicSegment8Vtbl      DirectMusicSegment8_Segment_Vtbl;
extern IDirectMusicObjectVtbl        DirectMusicSegment8_Object_Vtbl;
extern IPersistStreamVtbl            DirectMusicSegment8_PersistStream_Vtbl;

extern IDirectMusicSegmentState8Vtbl DirectMusicSegmentState8_Vtbl;

extern IUnknownVtbl                  DirectMusicGraph_Unknown_Vtbl;
extern IDirectMusicGraphVtbl         DirectMusicGraph_Gtaph_Vtbl;
extern IDirectMusicObjectVtbl        DirectMusicGraph_Object_Vtbl;
extern IPersistStreamVtbl            DirectMusicGraph_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicAudioPath_Unknown_Vtbl;
extern IDirectMusicAudioPathVtbl     DirectMusicAudioPath_AudioPath_Vtbl;
extern IDirectMusicObjectVtbl        DirectMusicAudioPath_Object_Vtbl;
extern IPersistStreamVtbl            DirectMusicAudioPath_Persist_Stream_Vtbl;

extern IDirectMusicTool8Vtbl         DirectMusicTool8_Vtbl;

extern IDirectMusicPatternTrackVtbl  DirectMusicPatternTrack_Vtbl;

extern IUnknownVtbl                  DirectMusicLyricsTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicLyricsTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicLyricsTrack_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicMarkerTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicMarkerTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicMarkerTrack_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicParamControlTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicParamControlTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicParamControlTrack_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicSegTriggerTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicSegTriggerTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicSegTriggerTrack_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicSeqTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicSeqTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicSeqTrack_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicSysExTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicSysExTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicSysExTrack_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicTempoTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicTempoTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicTempoTrack_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicTimeSigTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicTimeSigTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicTimeSigTrack_PersistStream_Vtbl;

extern IUnknownVtbl                  DirectMusicWaveTrack_Unknown_Vtbl;
extern IDirectMusicTrack8Vtbl        DirectMusicWaveTrack_Track_Vtbl;
extern IPersistStreamVtbl            DirectMusicWaveTrack_PersistStream_Vtbl;

/*****************************************************************************
 * ClassFactory
 */
extern HRESULT WINAPI DMUSIC_CreateDirectMusicPerformanceImpl (LPCGUID lpcGUID, LPVOID *ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicSegmentImpl (LPCGUID lpcGUID, LPVOID *ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicSegmentStateImpl (LPCGUID lpcGUID, LPVOID *ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicGraphImpl (LPCGUID lpcGUID, LPVOID *ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicAudioPathImpl (LPCGUID lpcGUID, LPVOID *ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicToolImpl (LPCGUID lpcGUID, LPVOID *ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicPatternTrackImpl (LPCGUID lpcGUID, LPVOID *ppobj, LPUNKNOWN pUnkOuter);

extern HRESULT WINAPI DMUSIC_CreateDirectMusicLyricsTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicMarkerTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicParamControlTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicSegTriggerTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicSeqTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicSysExTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicTempoTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicTimeSigTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);
extern HRESULT WINAPI DMUSIC_CreateDirectMusicWaveTrack (LPCGUID lpcGUID, LPVOID* ppobj, LPUNKNOWN pUnkOuter);


/*****************************************************************************
 * Auxiliary definitions
 */
typedef struct _DMUS_PRIVATE_SEGMENT_TRACK {
  struct list entry; /* for listing elements */
  DWORD dwGroupBits;
  IDirectMusicTrack* pTrack;
} DMUS_PRIVATE_SEGMENT_TRACK, *LPDMUS_PRIVATE_SEGMENT_TRACK;

typedef struct _DMUS_PRIVATE_TEMPO_ITEM {
  struct list entry; /* for listing elements */
  DMUS_IO_TEMPO_ITEM item;
} DMUS_PRIVATE_TEMPO_ITEM, *LPDMUS_PRIVATE_TEMPO_ITEM;

typedef struct _DMUS_PRIVATE_SEGMENT_ITEM {
  struct list entry; /* for listing elements */
  DMUS_IO_SEGMENT_ITEM_HEADER header;
  IDirectMusicObject* pObject;
  WCHAR wszName[DMUS_MAX_NAME];
} DMUS_PRIVATE_SEGMENT_ITEM, *LPDMUS_PRIVATE_SEGMENT_ITEM;

typedef struct _DMUS_PRIVATE_GRAPH_TOOL {
  struct list entry; /* for listing elements */
  DWORD dwIndex;
  IDirectMusicTool* pTool;
} DMUS_PRIVATE_GRAPH_TOOL, *LPDMUS_PRIVATE_GRAPH_TOOL;

typedef struct _DMUS_PRIVATE_TEMPO_PLAY_STATE {
  DWORD dummy;
} DMUS_PRIVATE_TEMPO_PLAY_STATE, *LPDMUS_PRIVATE_TEMPO_PLAY_STATE;

/* some sort of aux. performance channel: as far as i can understand, these are 
   used to represent a particular midi channel in particular group at particular
   group; so all we need to do is to fill it with parent port, group and midi 
   channel ? */
typedef struct DMUSIC_PRIVATE_PCHANNEL_ {
	DWORD channel; /* map to this channel... */
	DWORD group; /* ... in this group ... */
	IDirectMusicPort *port; /* ... at this port */
} DMUSIC_PRIVATE_PCHANNEL, *LPDMUSIC_PRIVATE_PCHANNEL;

/*****************************************************************************
 * IDirectMusicPerformance8Impl implementation structure
 */
struct IDirectMusicPerformance8Impl {
  /* IUnknown fields */
  IDirectMusicPerformance8Vtbl *lpVtbl;
  DWORD                  ref;

  /* IDirectMusicPerformanceImpl fields */
  IDirectMusic8*         pDirectMusic;
  IDirectSound*          pDirectSound;
  IDirectMusicGraph*     pToolGraph;
  DMUS_AUDIOPARAMS       pParams;

  /* global parameters */
  BOOL  fAutoDownload;
  char  cMasterGrooveLevel;
  float fMasterTempo;
  long  lMasterVolume;
	
  /* performance channels */
  DMUSIC_PRIVATE_PCHANNEL PChannel[1];

   /* IDirectMusicPerformance8Impl fields */
  IDirectMusicAudioPath* pDefaultPath;
  HANDLE hNotification;
  REFERENCE_TIME rtMinimum;

  REFERENCE_TIME rtLatencyTime;
  DWORD dwBumperLength;
  DWORD dwPrepareTime;
  /** Message Processing */
  HANDLE         procThread;
  DWORD          procThreadId;
  REFERENCE_TIME procThreadStartTime;
  BOOL           procThreadTicStarted;
  CRITICAL_SECTION safe;
  struct DMUS_PMSGItem* head; 
  struct DMUS_PMSGItem* imm_head; 
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicPerformance8Impl_QueryInterface (LPDIRECTMUSICPERFORMANCE8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicPerformance8Impl_AddRef (LPDIRECTMUSICPERFORMANCE8 iface);
extern ULONG WINAPI   IDirectMusicPerformance8Impl_Release (LPDIRECTMUSICPERFORMANCE8 iface);
/* IDirectMusicPerformance: */
extern HRESULT WINAPI IDirectMusicPerformance8Impl_Init (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusic** ppDirectMusic, LPDIRECTSOUND pDirectSound, HWND hWnd);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_PlaySegment (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicSegment* pSegment, DWORD dwFlags, __int64 i64StartTime, IDirectMusicSegmentState** ppSegmentState);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_Stop (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicSegment* pSegment, IDirectMusicSegmentState* pSegmentState, MUSIC_TIME mtTime, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetSegmentState (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicSegmentState** ppSegmentState, MUSIC_TIME mtTime);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_SetPrepareTime (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwMilliSeconds);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetPrepareTime (LPDIRECTMUSICPERFORMANCE8 iface, DWORD* pdwMilliSeconds);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_SetBumperLength (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwMilliSeconds);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetBumperLength (LPDIRECTMUSICPERFORMANCE8 iface, DWORD* pdwMilliSeconds);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_SendPMsg (LPDIRECTMUSICPERFORMANCE8 iface, DMUS_PMSG* pPMSG);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_MusicToReferenceTime (LPDIRECTMUSICPERFORMANCE8 iface, MUSIC_TIME mtTime, REFERENCE_TIME* prtTime);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_ReferenceToMusicTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME rtTime, MUSIC_TIME* pmtTime);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_IsPlaying (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicSegment* pSegment, IDirectMusicSegmentState* pSegState);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME* prtNow, MUSIC_TIME* pmtNow);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_AllocPMsg (LPDIRECTMUSICPERFORMANCE8 iface, ULONG cb, DMUS_PMSG** ppPMSG);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_FreePMsg (LPDIRECTMUSICPERFORMANCE8 iface, DMUS_PMSG* pPMSG);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetGraph (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicGraph** ppGraph);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_SetGraph (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicGraph* pGraph);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_SetNotificationHandle (LPDIRECTMUSICPERFORMANCE8 iface, HANDLE hNotification, REFERENCE_TIME rtMinimum);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetNotificationPMsg (LPDIRECTMUSICPERFORMANCE8 iface, DMUS_NOTIFICATION_PMSG** ppNotificationPMsg);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_AddNotificationType (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_RemoveNotificationType (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_AddPort (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicPort* pPort);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_RemovePort (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicPort* pPort);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_AssignPChannelBlock (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwBlockNum, IDirectMusicPort* pPort, DWORD dwGroup);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_AssignPChannel (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwPChannel, IDirectMusicPort* pPort, DWORD dwGroup, DWORD dwMChannel);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_PChannelInfo (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwPChannel, IDirectMusicPort** ppPort, DWORD* pdwGroup, DWORD* pdwMChannel);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_DownloadInstrument (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicInstrument* pInst, DWORD dwPChannel, IDirectMusicDownloadedInstrument** ppDownInst, DMUS_NOTERANGE* pNoteRanges, DWORD dwNumNoteRanges, IDirectMusicPort** ppPort, DWORD* pdwGroup, DWORD* pdwMChannel);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_Invalidate (LPDIRECTMUSICPERFORMANCE8 iface, MUSIC_TIME mtTime, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetParam (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, DWORD dwGroupBits, DWORD dwIndex, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_SetParam (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, DWORD dwGroupBits, DWORD dwIndex, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetGlobalParam (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, void* pParam, DWORD dwSize);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_SetGlobalParam (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, void* pParam, DWORD dwSize);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetLatencyTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME* prtTime);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetQueueTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME* prtTime);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_AdjustTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME rtAmount);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_CloseDown (LPDIRECTMUSICPERFORMANCE8 iface);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetResolvedTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME rtTime, REFERENCE_TIME* prtResolved, DWORD dwTimeResolveFlags);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_MIDIToMusic (LPDIRECTMUSICPERFORMANCE8 iface, BYTE bMIDIValue, DMUS_CHORD_KEY* pChord, BYTE bPlayMode, BYTE bChordLevel, WORD* pwMusicValue);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_MusicToMIDI (LPDIRECTMUSICPERFORMANCE8 iface, WORD wMusicValue, DMUS_CHORD_KEY* pChord, BYTE bPlayMode, BYTE bChordLevel, BYTE* pbMIDIValue);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_TimeToRhythm (LPDIRECTMUSICPERFORMANCE8 iface, MUSIC_TIME mtTime, DMUS_TIMESIGNATURE* pTimeSig, WORD* pwMeasure, BYTE* pbBeat, BYTE* pbGrid, short* pnOffset);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_RhythmToTime (LPDIRECTMUSICPERFORMANCE8 iface, WORD wMeasure, BYTE bBeat, BYTE bGrid, short nOffset, DMUS_TIMESIGNATURE* pTimeSig, MUSIC_TIME* pmtTime);
/* IDirectMusicPerformance8: */
extern HRESULT WINAPI IDirectMusicPerformance8Impl_InitAudio (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusic** ppDirectMusic, IDirectSound** ppDirectSound, HWND hWnd, DWORD dwDefaultPathType, DWORD dwPChannelCount, DWORD dwFlags, DMUS_AUDIOPARAMS* pParams);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_PlaySegmentEx (LPDIRECTMUSICPERFORMANCE8 iface, IUnknown* pSource, WCHAR* pwzSegmentName, IUnknown* pTransition, DWORD dwFlags, __int64 i64StartTime, IDirectMusicSegmentState** ppSegmentState, IUnknown* pFrom, IUnknown* pAudioPath);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_StopEx (LPDIRECTMUSICPERFORMANCE8 iface, IUnknown* pObjectToStop, __int64 i64StopTime, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_ClonePMsg (LPDIRECTMUSICPERFORMANCE8 iface, DMUS_PMSG* pSourcePMSG, DMUS_PMSG** ppCopyPMSG);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_CreateAudioPath (LPDIRECTMUSICPERFORMANCE8 iface, IUnknown* pSourceConfig, BOOL fActivate, IDirectMusicAudioPath** ppNewPath);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_CreateStandardAudioPath (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwType, DWORD dwPChannelCount, BOOL fActivate, IDirectMusicAudioPath** ppNewPath);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_SetDefaultAudioPath (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicAudioPath* pAudioPath);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetDefaultAudioPath (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicAudioPath** ppAudioPath);
extern HRESULT WINAPI IDirectMusicPerformance8Impl_GetParamEx (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, DWORD dwTrackID, DWORD dwGroupBits, DWORD dwIndex, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);

/*****************************************************************************
 * IDirectMusicSegment8Impl implementation structure
 */
struct IDirectMusicSegment8Impl {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicSegment8Vtbl *SegmentVtbl;
  IDirectMusicObjectVtbl *ObjectVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicSegment8Impl fields */
  LPDMUS_OBJECTDESC      pDesc;
  DMUS_IO_SEGMENT_HEADER header;
  IDirectMusicGraph*     pGraph; 
  struct list Tracks;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicSegment8Impl_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSegment8Impl_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicSegment8Impl_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicSegment(8): */
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_QueryInterface (LPDIRECTMUSICSEGMENT8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSegment8Impl_IDirectMusicSegment8_AddRef (LPDIRECTMUSICSEGMENT8 iface);
extern ULONG WINAPI   IDirectMusicSegment8Impl_IDirectMusicSegment8_Release (LPDIRECTMUSICSEGMENT8 iface);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetLength (LPDIRECTMUSICSEGMENT8 iface, MUSIC_TIME* pmtLength);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetLength (LPDIRECTMUSICSEGMENT8 iface, MUSIC_TIME mtLength);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetRepeats (LPDIRECTMUSICSEGMENT8 iface, DWORD* pdwRepeats);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetRepeats (LPDIRECTMUSICSEGMENT8 iface, DWORD dwRepeats);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetDefaultResolution (LPDIRECTMUSICSEGMENT8 iface, DWORD* pdwResolution);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetDefaultResolution (LPDIRECTMUSICSEGMENT8 iface, DWORD dwResolution);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetTrack (LPDIRECTMUSICSEGMENT8 iface, REFGUID rguidType, DWORD dwGroupBits, DWORD dwIndex, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetTrackGroup (LPDIRECTMUSICSEGMENT8 iface, IDirectMusicTrack* pTrack, DWORD* pdwGroupBits);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_InsertTrack (LPDIRECTMUSICSEGMENT8 iface, IDirectMusicTrack* pTrack, DWORD dwGroupBits);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_RemoveTrack (LPDIRECTMUSICSEGMENT8 iface, IDirectMusicTrack* pTrack);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_InitPlay (LPDIRECTMUSICSEGMENT8 iface, IDirectMusicSegmentState** ppSegState, IDirectMusicPerformance* pPerformance, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetGraph (LPDIRECTMUSICSEGMENT8 iface, IDirectMusicGraph** ppGraph);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetGraph (LPDIRECTMUSICSEGMENT8 iface, IDirectMusicGraph* pGraph);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_AddNotificationType (LPDIRECTMUSICSEGMENT8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_RemoveNotificationType (LPDIRECTMUSICSEGMENT8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetParam (LPDIRECTMUSICSEGMENT8 iface, REFGUID rguidType, DWORD dwGroupBits, DWORD dwIndex, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetParam (LPDIRECTMUSICSEGMENT8 iface, REFGUID rguidType, DWORD dwGroupBits, DWORD dwIndex, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_Clone (LPDIRECTMUSICSEGMENT8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicSegment** ppSegment);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetStartPoint (LPDIRECTMUSICSEGMENT8 iface, MUSIC_TIME mtStart);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetStartPoint (LPDIRECTMUSICSEGMENT8 iface, MUSIC_TIME* pmtStart);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetLoopPoints (LPDIRECTMUSICSEGMENT8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetLoopPoints (LPDIRECTMUSICSEGMENT8 iface, MUSIC_TIME* pmtStart, MUSIC_TIME* pmtEnd);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetPChannelsUsed (LPDIRECTMUSICSEGMENT8 iface, DWORD dwNumPChannels, DWORD* paPChannels);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_SetTrackConfig (LPDIRECTMUSICSEGMENT8 iface, REFGUID rguidTrackClassID, DWORD dwGroupBits, DWORD dwIndex, DWORD dwFlagsOn, DWORD dwFlagsOff);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_GetAudioPathConfig (LPDIRECTMUSICSEGMENT8 iface, IUnknown** ppAudioPathConfig);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_Compose (LPDIRECTMUSICSEGMENT8 iface, MUSIC_TIME mtTime, IDirectMusicSegment* pFromSegment, IDirectMusicSegment* pToSegment, IDirectMusicSegment** ppComposedSegment);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_Download (LPDIRECTMUSICSEGMENT8 iface, IUnknown *pAudioPath);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicSegment8_Unload (LPDIRECTMUSICSEGMENT8 iface, IUnknown *pAudioPath);
/* IDirectMusicObject: */
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicObject_QueryInterface (LPDIRECTMUSICOBJECT iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSegment8Impl_IDirectMusicObject_AddRef (LPDIRECTMUSICOBJECT iface);
extern ULONG WINAPI   IDirectMusicSegment8Impl_IDirectMusicObject_Release (LPDIRECTMUSICOBJECT iface);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicObject_GetDescriptor (LPDIRECTMUSICOBJECT iface, LPDMUS_OBJECTDESC pDesc);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicObject_SetDescriptor (LPDIRECTMUSICOBJECT iface, LPDMUS_OBJECTDESC pDesc);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IDirectMusicObject_ParseDescriptor (LPDIRECTMUSICOBJECT iface, LPSTREAM pStream, LPDMUS_OBJECTDESC pDesc);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicSegment8Impl_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicSegment8Impl_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicSegment8Impl_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicSegment8Impl_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicSegmentState8Impl implementation structure
 */
struct IDirectMusicSegmentState8Impl {
  /* IUnknown fields */
  IDirectMusicSegmentState8Vtbl *lpVtbl;
  DWORD          ref;

  /* IDirectMusicSegmentState8Impl fields */
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicSegmentState8Impl_QueryInterface (LPDIRECTMUSICSEGMENTSTATE8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSegmentState8Impl_AddRef (LPDIRECTMUSICSEGMENTSTATE8 iface);
extern ULONG WINAPI   IDirectMusicSegmentState8Impl_Release (LPDIRECTMUSICSEGMENTSTATE8 iface);
/* IDirectMusicSegmentState(8): */
extern HRESULT WINAPI IDirectMusicSegmentState8Impl_GetRepeats (LPDIRECTMUSICSEGMENTSTATE8 iface,  DWORD* pdwRepeats);
extern HRESULT WINAPI IDirectMusicSegmentState8Impl_GetSegment (LPDIRECTMUSICSEGMENTSTATE8 iface, IDirectMusicSegment** ppSegment);
extern HRESULT WINAPI IDirectMusicSegmentState8Impl_GetStartTime (LPDIRECTMUSICSEGMENTSTATE8 iface, MUSIC_TIME* pmtStart);
extern HRESULT WINAPI IDirectMusicSegmentState8Impl_GetSeek (LPDIRECTMUSICSEGMENTSTATE8 iface, MUSIC_TIME* pmtSeek);
extern HRESULT WINAPI IDirectMusicSegmentState8Impl_GetStartPoint (LPDIRECTMUSICSEGMENTSTATE8 iface, MUSIC_TIME* pmtStart);
extern HRESULT WINAPI IDirectMusicSegmentState8Impl_SetTrackConfig (LPDIRECTMUSICSEGMENTSTATE8 iface, REFGUID rguidTrackClassID, DWORD dwGroupBits, DWORD dwIndex, DWORD dwFlagsOn, DWORD dwFlagsOff);
extern HRESULT WINAPI IDirectMusicSegmentState8Impl_GetObjectInPath (LPDIRECTMUSICSEGMENTSTATE8 iface, DWORD dwPChannel, DWORD dwStage, DWORD dwBuffer, REFGUID guidObject, DWORD dwIndex, REFGUID iidInterface, void** ppObject);

/*****************************************************************************
 * IDirectMusicGraphImpl implementation structure
 */
struct IDirectMusicGraphImpl {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicGraphVtbl *GraphVtbl;
  IDirectMusicObjectVtbl *ObjectVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicGraphImpl fields */
  LPDMUS_OBJECTDESC pDesc;
  WORD              num_tools;
  struct list       Tools;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicGraphImpl_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicGraphImpl_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicGraphImpl_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicGraph: */
extern HRESULT WINAPI IDirectMusicGraphImpl_IDirectMusicGraph_QueryInterface (LPDIRECTMUSICGRAPH iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicGraphImpl_IDirectMusicGraph_AddRef (LPDIRECTMUSICGRAPH iface);
extern ULONG WINAPI   IDirectMusicGraphImpl_IDirectMusicGraph_Release (LPDIRECTMUSICGRAPH iface);extern HRESULT WINAPI IDirectMusicGraphImpl_StampPMsg (LPDIRECTMUSICGRAPH iface, DMUS_PMSG* pPMSG);
extern HRESULT WINAPI IDirectMusicGraphImpl_IDirectMusicGraph_InsertTool (LPDIRECTMUSICGRAPH iface, IDirectMusicTool* pTool, DWORD* pdwPChannels, DWORD cPChannels, LONG lIndex);
extern HRESULT WINAPI IDirectMusicGraphImpl_IDirectMusicGraph_GetTool (LPDIRECTMUSICGRAPH iface, DWORD dwIndex, IDirectMusicTool** ppTool);
extern HRESULT WINAPI IDirectMusicGraphImpl_IDirectMusicGraph_RemoveTool (LPDIRECTMUSICGRAPH iface, IDirectMusicTool* pTool);
/* IDirectMusicObject: */
extern HRESULT WINAPI IDirectMusicGraphImpl_IDirectMusicObject_QueryInterface (LPDIRECTMUSICOBJECT iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicGraphImpl_IDirectMusicObject_AddRef (LPDIRECTMUSICOBJECT iface);
extern ULONG WINAPI   IDirectMusicGraphImpl_IDirectMusicObject_Release (LPDIRECTMUSICOBJECT iface);
extern HRESULT WINAPI IDirectMusicGraphImpl_IDirectMusicObject_GetDescriptor (LPDIRECTMUSICOBJECT iface, LPDMUS_OBJECTDESC pDesc);
extern HRESULT WINAPI IDirectMusicGraphImpl_IDirectMusicObject_SetDescriptor (LPDIRECTMUSICOBJECT iface, LPDMUS_OBJECTDESC pDesc);
extern HRESULT WINAPI IDirectMusicGraphImpl_IDirectMusicObject_ParseDescriptor (LPDIRECTMUSICOBJECT iface, LPSTREAM pStream, LPDMUS_OBJECTDESC pDesc);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicGraphImpl_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicGraphImpl_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicGraphImpl_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicGraphImpl_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicGraphImpl_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicGraphImpl_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicGraphImpl_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicGraphImpl_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicAudioPathImpl implementation structure
 */
struct IDirectMusicAudioPathImpl {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicAudioPathVtbl *AudioPathVtbl;
  IDirectMusicObjectVtbl *ObjectVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicAudioPathImpl fields */
  LPDMUS_OBJECTDESC pDesc;
	
  IDirectMusicPerformance8* pPerf;
  IDirectMusicGraph*        pToolGraph;
  IDirectSoundBuffer*       pDSBuffer;
  IDirectSoundBuffer*       pPrimary;

  BOOL fActive;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicAudioPathImpl_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicAudioPathImpl_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicAudioPath: */
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicAudioPath_QueryInterface (LPDIRECTMUSICAUDIOPATH iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicAudioPathImpl_IDirectMusicAudioPath_AddRef (LPDIRECTMUSICAUDIOPATH iface);
extern ULONG WINAPI   IDirectMusicAudioPathImpl_IDirectMusicAudioPath_Release (LPDIRECTMUSICAUDIOPATH iface);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicAudioPath_GetObjectInPath (LPDIRECTMUSICAUDIOPATH iface, DWORD dwPChannel, DWORD dwStage, DWORD dwBuffer, REFGUID guidObject, WORD dwIndex, REFGUID iidInterface, void** ppObject);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicAudioPath_Activate (LPDIRECTMUSICAUDIOPATH iface, BOOL fActivate);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicAudioPath_SetVolume (LPDIRECTMUSICAUDIOPATH iface, long lVolume, DWORD dwDuration);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicAudioPath_ConvertPChannel (LPDIRECTMUSICAUDIOPATH iface, DWORD dwPChannelIn, DWORD* pdwPChannelOut);
/* IDirectMusicObject: */
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicObject_QueryInterface (LPDIRECTMUSICOBJECT iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicAudioPathImpl_IDirectMusicObject_AddRef (LPDIRECTMUSICOBJECT iface);
extern ULONG WINAPI   IDirectMusicAudioPathImpl_IDirectMusicObject_Release (LPDIRECTMUSICOBJECT iface);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicObject_GetDescriptor (LPDIRECTMUSICOBJECT iface, LPDMUS_OBJECTDESC pDesc);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicObject_SetDescriptor (LPDIRECTMUSICOBJECT iface, LPDMUS_OBJECTDESC pDesc);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IDirectMusicObject_ParseDescriptor (LPDIRECTMUSICOBJECT iface, LPSTREAM pStream, LPDMUS_OBJECTDESC pDesc);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicAudioPathImpl_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicAudioPathImpl_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicAudioPathImpl_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicTool8Impl implementation structure
 */
struct IDirectMusicTool8Impl {
  /* IUnknown fields */
  IDirectMusicTool8Vtbl *lpVtbl;
  DWORD          ref;

  /* IDirectMusicTool8Impl fields */
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicTool8Impl_QueryInterface (LPDIRECTMUSICTOOL8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicTool8Impl_AddRef (LPDIRECTMUSICTOOL8 iface);
extern ULONG WINAPI   IDirectMusicTool8Impl_Release (LPDIRECTMUSICTOOL8 iface);
/* IDirectMusicTool(8): */
extern HRESULT WINAPI IDirectMusicTool8Impl_Init (LPDIRECTMUSICTOOL8 iface, IDirectMusicGraph* pGraph);
extern HRESULT WINAPI IDirectMusicTool8Impl_GetMsgDeliveryType (LPDIRECTMUSICTOOL8 iface, DWORD* pdwDeliveryType);
extern HRESULT WINAPI IDirectMusicTool8Impl_GetMediaTypeArraySize (LPDIRECTMUSICTOOL8 iface, DWORD* pdwNumElements);
extern HRESULT WINAPI IDirectMusicTool8Impl_GetMediaTypes (LPDIRECTMUSICTOOL8 iface, DWORD** padwMediaTypes, DWORD dwNumElements);
extern HRESULT WINAPI IDirectMusicTool8Impl_ProcessPMsg (LPDIRECTMUSICTOOL8 iface, IDirectMusicPerformance* pPerf, DMUS_PMSG* pPMSG);
extern HRESULT WINAPI IDirectMusicTool8Impl_Flush (LPDIRECTMUSICTOOL8 iface, IDirectMusicPerformance* pPerf, DMUS_PMSG* pPMSG, REFERENCE_TIME rtTime);
extern HRESULT WINAPI IDirectMusicTool8Impl_Clone (LPDIRECTMUSICTOOL8 iface, IDirectMusicTool** ppTool);

/*****************************************************************************
 * IDirectMusicPatternTrackImpl implementation structure
 */
struct IDirectMusicPatternTrackImpl {
  /* IUnknown fields */
  IDirectMusicPatternTrackVtbl *lpVtbl;
  DWORD          ref;

  /* IDirectMusicPatternTrackImpl fields */
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicPatternTrackImpl_QueryInterface (LPDIRECTMUSICPATTERNTRACK iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicPatternTrackImpl_AddRef (LPDIRECTMUSICPATTERNTRACK iface);
extern ULONG WINAPI   IDirectMusicPatternTrackImpl_Release (LPDIRECTMUSICPATTERNTRACK iface);
/* IDirectMusicPatternTrack: */
extern HRESULT WINAPI IDirectMusicPatternTrackImpl_CreateSegment (LPDIRECTMUSICPATTERNTRACK iface, IDirectMusicStyle* pStyle, IDirectMusicSegment** ppSegment);
extern HRESULT WINAPI IDirectMusicPatternTrackImpl_SetVariation (LPDIRECTMUSICPATTERNTRACK iface, IDirectMusicSegmentState* pSegState, DWORD dwVariationFlags, DWORD dwPart);
extern HRESULT WINAPI IDirectMusicPatternTrackImpl_SetPatternByName (LPDIRECTMUSICPATTERNTRACK iface, IDirectMusicSegmentState* pSegState, WCHAR* wszName, IDirectMusicStyle* pStyle, DWORD dwPatternType, DWORD* pdwLength);

/*****************************************************************************
 * IDirectMusicLyricsTrack implementation structure
 */
struct IDirectMusicLyricsTrack
{
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicLyricsTrack fields */
  LPDMUS_OBJECTDESC pDesc;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicLyricsTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicLyricsTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicLyricsTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicLyricsTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicLyricsTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicLyricsTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicLyricsTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicLyricsTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicLyricsTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicMarkerTrack implementation structure
 */
struct IDirectMusicMarkerTrack {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicMarkerTrack fields */
  LPDMUS_OBJECTDESC pDesc;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicMarkerTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicMarkerTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicMarkerTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicMarkerTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicMarkerTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicMarkerTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicMarkerTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicMarkerTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicMarkerTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicParamControlTrack implementation structure
 */
struct IDirectMusicParamControlTrack {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicParamControlTrack fields */
  LPDMUS_OBJECTDESC pDesc;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicParamControlTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicParamControlTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicParamControlTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicParamControlTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicParamControlTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicParamControlTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicParamControlTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicParamControlTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicParamControlTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicSegTriggerTrack implementation structure
 */
struct IDirectMusicSegTriggerTrack {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicSegTriggerTrack fields */
  LPDMUS_OBJECTDESC pDesc;

  struct list Items;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSegTriggerTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicSegTriggerTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSegTriggerTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicSegTriggerTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicSegTriggerTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicSegTriggerTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicSegTriggerTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicSeqTrack implementation structure
 */
struct IDirectMusicSeqTrack {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicSeqTrack fields */
  LPDMUS_OBJECTDESC pDesc;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicSeqTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSeqTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicSeqTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSeqTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicSeqTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicSeqTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicSeqTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicSeqTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicSeqTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicSeqTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicSeqTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicSeqTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicSeqTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicSeqTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicSysExTrack implementation structure
 */
struct IDirectMusicSysExTrack {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicSysExTrack fields */
  LPDMUS_OBJECTDESC pDesc;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicSysExTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSysExTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicSysExTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicSysExTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicSysExTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicSysExTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicSysExTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicSysExTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicSysExTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicSysExTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicSysExTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicSysExTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicSysExTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicSysExTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicTempoTrack implementation structure
 */
struct IDirectMusicTempoTrack {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicTempoTrack fields */
  LPDMUS_OBJECTDESC pDesc;
  BOOL enabled;
  struct list Items;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicTempoTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicTempoTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicTempoTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicTempoTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicTempoTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicTempoTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicTempoTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicTempoTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicTempoTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicTempoTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicTempoTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicTempoTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicTempoTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicTempoTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicTimeSigTrack implementation structure
 */
struct IDirectMusicTimeSigTrack {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicTimeSigTrack fields */
  LPDMUS_OBJECTDESC pDesc;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicTimeSigTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicTimeSigTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicTimeSigTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicTimeSigTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicTimeSigTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicTimeSigTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicTimeSigTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);

/*****************************************************************************
 * IDirectMusicWaveTrack implementation structure
 */
struct IDirectMusicWaveTrack {
  /* IUnknown fields */
  IUnknownVtbl *UnknownVtbl;
  IDirectMusicTrack8Vtbl *TrackVtbl;
  IPersistStreamVtbl *PersistStreamVtbl;
  DWORD          ref;

  /* IDirectMusicWaveTrack fields */
  LPDMUS_OBJECTDESC pDesc;
};

/* IUnknown: */
extern HRESULT WINAPI IDirectMusicWaveTrack_IUnknown_QueryInterface (LPUNKNOWN iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicWaveTrack_IUnknown_AddRef (LPUNKNOWN iface);
extern ULONG WINAPI   IDirectMusicWaveTrack_IUnknown_Release (LPUNKNOWN iface);
/* IDirectMusicTrack(8): */
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_QueryInterface (LPDIRECTMUSICTRACK8 iface, REFIID riid, LPVOID *ppobj);
extern ULONG WINAPI   IDirectMusicWaveTrack_IDirectMusicTrack_AddRef (LPDIRECTMUSICTRACK8 iface);
extern ULONG WINAPI   IDirectMusicWaveTrack_IDirectMusicTrack_Release (LPDIRECTMUSICTRACK8 iface);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_Init (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegment* pSegment);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_InitPlay (LPDIRECTMUSICTRACK8 iface, IDirectMusicSegmentState* pSegmentState, IDirectMusicPerformance* pPerformance, void** ppStateData, DWORD dwVirtualTrackID, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_EndPlay (LPDIRECTMUSICTRACK8 iface, void* pStateData);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_Play (LPDIRECTMUSICTRACK8 iface, void* pStateData, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, MUSIC_TIME mtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_GetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_SetParam (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, MUSIC_TIME mtTime, void* pParam);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_IsParamSupported (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_AddNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_RemoveNotificationType (LPDIRECTMUSICTRACK8 iface, REFGUID rguidNotificationType);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_Clone (LPDIRECTMUSICTRACK8 iface, MUSIC_TIME mtStart, MUSIC_TIME mtEnd, IDirectMusicTrack** ppTrack);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_PlayEx (LPDIRECTMUSICTRACK8 iface, void* pStateData, REFERENCE_TIME rtStart, REFERENCE_TIME rtEnd, REFERENCE_TIME rtOffset, DWORD dwFlags, IDirectMusicPerformance* pPerf, IDirectMusicSegmentState* pSegSt, DWORD dwVirtualID);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_GetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, REFERENCE_TIME* prtNext, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_SetParamEx (LPDIRECTMUSICTRACK8 iface, REFGUID rguidType, REFERENCE_TIME rtTime, void* pParam, void* pStateData, DWORD dwFlags);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_Compose (LPDIRECTMUSICTRACK8 iface, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
extern HRESULT WINAPI IDirectMusicWaveTrack_IDirectMusicTrack_Join (LPDIRECTMUSICTRACK8 iface, IDirectMusicTrack* pNewTrack, MUSIC_TIME mtJoin, IUnknown* pContext, DWORD dwTrackGroup, IDirectMusicTrack** ppResultTrack);
/* IPersistStream: */
extern HRESULT WINAPI IDirectMusicWaveTrack_IPersistStream_QueryInterface (LPPERSISTSTREAM iface, REFIID riid, void** ppvObject);
extern ULONG WINAPI   IDirectMusicWaveTrack_IPersistStream_AddRef (LPPERSISTSTREAM iface);
extern ULONG WINAPI   IDirectMusicWaveTrack_IPersistStream_Release (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicWaveTrack_IPersistStream_GetClassID (LPPERSISTSTREAM iface, CLSID* pClassID);
extern HRESULT WINAPI IDirectMusicWaveTrack_IPersistStream_IsDirty (LPPERSISTSTREAM iface);
extern HRESULT WINAPI IDirectMusicWaveTrack_IPersistStream_Load (LPPERSISTSTREAM iface, IStream* pStm);
extern HRESULT WINAPI IDirectMusicWaveTrack_IPersistStream_Save (LPPERSISTSTREAM iface, IStream* pStm, BOOL fClearDirty);
extern HRESULT WINAPI IDirectMusicWaveTrack_IPersistStream_GetSizeMax (LPPERSISTSTREAM iface, ULARGE_INTEGER* pcbSize);


/*****************************************************************************
 * Misc.
 */

#include "dmutils.h"

#endif	/* __WINE_DMIME_PRIVATE_H */
