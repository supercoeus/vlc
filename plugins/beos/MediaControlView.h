/*****************************************************************************
 * MediaControlView.h: beos interface
 *****************************************************************************
 * Copyright (C) 1999, 2000, 2001 VideoLAN
 * $Id: MediaControlView.h,v 1.2.4.3 2002/10/09 15:29:51 stippi Exp $
 *
 * Authors: Tony Castley <tony@castley.net>
 *          Stephan Aßmus <stippi@yellowbites.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

#ifndef BEOS_MEDIA_CONTROL_VIEW_H
#define BEOS_MEDIA_CONTROL_VIEW_H

#include <Box.h>
#include <Control.h>

class BBitmap;
class PlayPauseButton;
class PositionInfoView;
class SeekSlider;
class TransportButton;
class VolumeSlider;

class MediaControlView : public BBox
{
 public:
								MediaControlView( BRect frame );
	virtual						~MediaControlView();

								// BBox
	virtual	void				AttachedToWindow();
	virtual	void				FrameResized(float width, float height);
	virtual	void				GetPreferredSize(float* width, float* height);
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				Pulse(); // detect stopped stream

								// MediaControlView
			void				SetProgress(uint64 seek, uint64 size);

			void				SetStatus(int status, int rate); 
			void				SetEnabled(bool enable);
			void				SetAudioEnabled(bool enable);
			uint32				GetSeekTo() const;
			uint32				GetVolume() const;
			void				SetSkippable(bool backward,
											 bool forward);
			void				SetMuted(bool mute);

			sem_id				fScrubSem;
    
 private:
			void				_LayoutControls(BRect frame) const;
			BRect				_MinFrame() const;
			void				_LayoutControl(BView* view,
											   BRect frame,
											   bool resizeWidth = false,
											   bool resizeHeight = false) const;


			VolumeSlider*		fVolumeSlider;
			SeekSlider*			fSeekSlider;
			TransportButton*	fSkipBack;
			TransportButton*	fSkipForward;
			TransportButton*	fRewind;
			TransportButton*	fForward;
			PlayPauseButton*	fPlayPause;
			TransportButton*	fStop;
			TransportButton*	fMute;
			PositionInfoView*	fPositionInfo;

			int					fCurrentRate;
			int					fCurrentStatus;
			float				fBottomControlHeight;
			BRect				fOldBounds;
};

class SeekSlider : public BControl
{
 public:
								SeekSlider(BRect frame,
										   const char* name,
										   MediaControlView* owner,
										   int32 minValue,
										   int32 maxValue);

	virtual						~SeekSlider();

								// BControl
	virtual	void				AttachedToWindow();
	virtual void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
										   const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);
	virtual	void				ResizeToPreferred();

								// SeekSlider
			void				SetPosition(float position);

private:
			int32				_ValueFor(float x) const;
			void				_StrokeFrame(BRect frame,
											 rgb_color left,
											 rgb_color top,
											 rgb_color right,
											 rgb_color bottom);
			void				_BeginSeek();
			void				_Seek();
			void				_EndSeek();

			MediaControlView*	fOwner;	
			bool				fTracking;
			int32				fMinValue;
			int32				fMaxValue;
};

class VolumeSlider : public BControl
{
 public:
								VolumeSlider(BRect frame,
											 const char* name,
											 int32 minValue,
											 int32 maxValue,
											 BMessage* message = NULL,
											 BHandler* target = NULL);

	virtual						~VolumeSlider();

								// BControl
	virtual	void				AttachedToWindow();
	virtual	void				SetValue(int32 value);
	virtual void				SetEnabled(bool enable);
	virtual void				Draw(BRect updateRect);
	virtual void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
										   const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);

								// VolumeSlider
			bool				IsValid() const;
			void				SetMuted(bool mute);

 private:
			void				_MakeBitmaps();
			void				_DimBitmap(BBitmap* bitmap);
			int32				_ValueFor(float xPos) const;

			BBitmap*			fLeftSideBits;
			BBitmap*			fRightSideBits;
			BBitmap*			fKnobBits;
			bool				fTracking;
			bool				fMuted;
			int32				fMinValue;
			int32				fMaxValue;
};

class PositionInfoView : public BView
{
 public:
								PositionInfoView( BRect frame,
												  const char* name );
	virtual						~PositionInfoView();

								// BView
	virtual	void				Draw( BRect updateRect );
	virtual	void				ResizeToPreferred();
	virtual	void				GetPreferredSize( float* width,
												  float* height );
	virtual	void				Pulse();

								// PositionInfoView
	enum
	{
		MODE_SMALL,
		MODE_BIG,
	};

			void				SetMode( uint32 mode );
			void				GetBigPreferredSize( float* width,
													 float* height );

			void				SetFile( int32 index, int32 size );
			void				SetTitle( int32 index, int32 size );
			void				SetChapter( int32 index, int32 size );
			void				SetTime( int32 seconds );
			void				SetTime( const char* string );
 private:
			void				_InvalidateContents( uint32 which = 0 );
			void				_MakeString( BString& into,
											 int32 index,
											 int32 maxIndex ) const;
//			void				_DrawAlignedString( const char* string,
//													BRect frame,
//													alignment mode = B_ALIGN_LEFT );

			uint32				fMode;
			int32				fCurrentFileIndex;
			int32				fCurrentFileSize;
			int32				fCurrentTitleIndex;
			int32				fCurrentTitleSize;
			int32				fCurrentChapterIndex;
			int32				fCurrentChapterSize;

			int32				fSeconds;
			BString				fTimeString;
			bigtime_t			fLastPulseUpdate;
			float				fStackedWidthCache;
			float				fStackedHeightCache;
};

#endif	// BEOS_MEDIA_CONTROL_VIEW_H
