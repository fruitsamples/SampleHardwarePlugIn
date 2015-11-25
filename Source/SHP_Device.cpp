/*	Copyright � 2007 Apple Inc. All Rights Reserved.
	
	Disclaimer: IMPORTANT:  This Apple software is supplied to you by 
			Apple Inc. ("Apple") in consideration of your agreement to the
			following terms, and your use, installation, modification or
			redistribution of this Apple software constitutes acceptance of these
			terms.  If you do not agree with these terms, please do not use,
			install, modify or redistribute this Apple software.
			
			In consideration of your agreement to abide by the following terms, and
			subject to these terms, Apple grants you a personal, non-exclusive
			license, under Apple's copyrights in this original Apple software (the
			"Apple Software"), to use, reproduce, modify and redistribute the Apple
			Software, with or without modifications, in source and/or binary forms;
			provided that if you redistribute the Apple Software in its entirety and
			without modifications, you must retain this notice and the following
			text and disclaimers in all such redistributions of the Apple Software. 
			Neither the name, trademarks, service marks or logos of Apple Inc. 
			may be used to endorse or promote products derived from the Apple
			Software without specific prior written permission from Apple.  Except
			as expressly stated in this notice, no other rights or licenses, express
			or implied, are granted by Apple herein, including but not limited to
			any patent rights that may be infringed by your derivative works or by
			other works in which the Apple Software may be incorporated.
			
			The Apple Software is provided by Apple on an "AS IS" basis.  APPLE
			MAKES NO WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION
			THE IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS
			FOR A PARTICULAR PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND
			OPERATION ALONE OR IN COMBINATION WITH YOUR PRODUCTS.
			
			IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL
			OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
			SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
			INTERRUPTION) ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION,
			MODIFICATION AND/OR DISTRIBUTION OF THE APPLE SOFTWARE, HOWEVER CAUSED
			AND WHETHER UNDER THEORY OF CONTRACT, TORT (INCLUDING NEGLIGENCE),
			STRICT LIABILITY OR OTHERWISE, EVEN IF APPLE HAS BEEN ADVISED OF THE
			POSSIBILITY OF SUCH DAMAGE.
*/
//=============================================================================
//	Includes
//=============================================================================

//	Self Include
#include "SHP_Device.h"

//	Internal Includes
#include "SHP_Control.h"
#include "SHP_PlugIn.h"
#include "SHP_Stream.h"

//	HPBase Includes
#include "HP_DeviceSettings.h"
#include "HP_HogMode.h"
#include "HP_IOCycleTelemetry.h"
#include "HP_IOProcList.h"
#include "HP_IOThread.h"

//	PublicUtility Includes
#include "CAAudioBufferList.h"
#include "CAAudioTimeStamp.h"
#include "CAAutoDisposer.h"
#include "CACFString.h"
#include "CADebugMacros.h"
#include "CAException.h"
#include "CAHostTimeBase.h"
#include "CALogMacros.h"
#include "CAMutex.h"

//=============================================================================
//	Logging
//=============================================================================

#if	CoreAudio_Debug
//	#define Log_ControlLife				1
//	#define	Log_HardareStartStop		1
//	#define	Log_HardareNotifications	1
//	#define	Log_InterestNotification	1
#endif

//=============================================================================
//	SHP_Device
//=============================================================================

SHP_Device::SHP_Device(AudioDeviceID inAudioDeviceID, SHP_PlugIn* inPlugIn)
:
	HP_Device(inAudioDeviceID, kAudioDeviceClassID, inPlugIn, 1, false),
	mSHPPlugIn(inPlugIn),
	mHogMode(NULL),
	mIOThread(NULL),
	mAnchorHostTime(0),
	mControlsInitialized(false),
	mControlProperty(NULL)
{
}

SHP_Device::~SHP_Device()
{
}

void	SHP_Device::Initialize()
{
	HP_Device::Initialize();
	
	//	allocate the hog mode implementation
	mHogMode = new HP_HogMode(this);
	mHogMode->Initialize();
	
	//	allocate the IO thread implementation
	mIOThread = new HP_IOThread(this);
	
	//	create the streams
	CreateStreams();
	
	//  set the default buffer size before we go any further
	mIOBufferFrameSize = 512;
	mIOBufferFrameSize = DetermineIOBufferFrameSize();
	
	//	allocate the property object that maps device control properties onto control objects
	mControlProperty = new HP_DeviceControlProperty(this);
	AddProperty(mControlProperty);
	
	//	make sure that the controls are always instantiated in the master process so that they can be saved later
	UInt32 isMaster = 0;
	UInt32 theSize = sizeof(UInt32);
	AudioHardwareGetProperty(kAudioHardwarePropertyProcessIsMaster, &theSize, &isMaster);
	if(isMaster != 0)
	{
		CreateControls();
	}
}

void	SHP_Device::Teardown()
{
	//	stop things
	Do_StopAllIOProcs();
	
	//	release hog mode if we have it
	if(mHogMode != NULL)
	{
		if(mHogMode->CurrentProcessIsOwner())
		{
			mHogMode->Release();
		}
		delete mHogMode;
		mHogMode = NULL;
	}
	
	//	teardown the other stuff we allocated
	if(mControlProperty != NULL)
	{
		RemoveProperty(mControlProperty);
		delete mControlProperty;
		mControlProperty = NULL;
	}

	ReleaseControls();
	ReleaseStreams();	
	
	delete mIOThread;
	mIOThread = NULL;
	
	HP_Device::Teardown();
}

void	SHP_Device::Finalize()
{
	//	Finalize() is called in place of Teardown() when we're being lazy about
	//	cleaning up. The idea is to do as little work as possible here.
	
	//	go through the streams and finalize them
	SHP_Stream* theStream;
	UInt32 theStreamIndex;
	UInt32 theNumberStreams;
	
	//	input
	theNumberStreams = GetNumberStreams(true);
	for(theStreamIndex = 0; theStreamIndex != theNumberStreams; ++theStreamIndex)
	{
		theStream = static_cast<SHP_Stream*>(GetStreamByIndex(true, theStreamIndex));
		theStream->Finalize();
	}
	
	//	output
	theNumberStreams = GetNumberStreams(false);
	for(theStreamIndex = 0; theStreamIndex != theNumberStreams; ++theStreamIndex)
	{
		theStream = static_cast<SHP_Stream*>(GetStreamByIndex(false, theStreamIndex));
		theStream->Finalize();
	}
	
	//	release hog mode if we have it
	if(mHogMode->CurrentProcessIsOwner())
	{
		mHogMode->Release();
	}
}

CFStringRef	SHP_Device::CopyDeviceName() const
{
	CFStringRef theAnswer = CFSTR("Sample Hardware Plug-In Device");
	CFRetain(theAnswer);
	return theAnswer;
}

CFStringRef	SHP_Device::CopyDeviceManufacturerName() const
{
	CFStringRef theAnswer = CFSTR("Apple Computer, Inc.");
	CFRetain(theAnswer);
	return theAnswer;
}

CFStringRef	SHP_Device::CopyDeviceUID() const
{
	CFStringRef theAnswer = CFSTR("Sample Hardware Plug-In Device");
	CFRetain(theAnswer);
	return theAnswer;
}

bool	SHP_Device::HogModeIsOwnedBySelf() const
{
	bool theAnswer = false;
	if(mHogMode != NULL)
	{
		theAnswer = mHogMode->CurrentProcessIsOwner();
	}
	return theAnswer;
}

bool	SHP_Device::HogModeIsOwnedBySelfOrIsFree() const
{
	bool theAnswer = true;
	if(mHogMode != NULL)
	{
		theAnswer = mHogMode->CurrentProcessIsOwnerOrIsFree();
	}
	return theAnswer;
}

void	SHP_Device::HogModeStateChanged()
{
	HP_Device::HogModeStateChanged();

	//	hold the device state lock until the changes have been completed
	//	it is vital that whenever taking both locks, that the device state
	//	lock be take prior to attempting to lock the IO lock.
	bool doUnlockDeviceStateGuard = GetDeviceStateMutex().Lock();
	
	//	Synchronize with the IO thread.
	bool doUnlockIOThreadGuard = mIOThread->GetIOGuard().Lock();
	
	RefreshAvailableStreamFormats();
	
	//	unlock the locks so that re-entry can happen
	if(doUnlockIOThreadGuard)
	{
		mIOThread->GetIOGuard().Unlock();
	}
	if(doUnlockDeviceStateGuard)
	{
		GetDeviceStateMutex().Unlock();
	}
}

bool	SHP_Device::HasProperty(const AudioObjectPropertyAddress& inAddress) const
{
	bool theAnswer = false;
	
	//	take and hold the state mutex
	CAMutex::Locker theStateMutex(const_cast<SHP_Device*>(this)->GetDeviceStateMutex());
	
	//  create the controls if necessary
	if(IsControlRelatedProperty(inAddress.mSelector))
	{
		const_cast<SHP_Device*>(this)->CreateControls();
	}
	
	switch(inAddress.mSelector)
	{
		case kAudioDevicePropertyIOCycleUsage:
			theAnswer = true;
			break;
		
		default:
			theAnswer = HP_Device::HasProperty(inAddress);
			break;
	};
	
	return theAnswer;
}

bool	SHP_Device::IsPropertySettable(const AudioObjectPropertyAddress& inAddress) const
{
	bool theAnswer = false;
	
	//	take and hold the state mutex
	CAMutex::Locker theStateMutex(const_cast<SHP_Device*>(this)->GetDeviceStateMutex());
	
	//  create the controls if necessary
	if(IsControlRelatedProperty(inAddress.mSelector))
	{
		const_cast<SHP_Device*>(this)->CreateControls();
	}
	
	switch(inAddress.mSelector)
	{
		case kAudioDevicePropertyHogMode:
			theAnswer = true;
			break;
			
		case kAudioDevicePropertyIOCycleUsage:
			theAnswer = true;
			break;
			
		default:
			theAnswer = HP_Device::IsPropertySettable(inAddress);
			break;
	};
	
	return theAnswer;
}

UInt32	SHP_Device::GetPropertyDataSize(const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
	UInt32	theAnswer = 0;
	
	//	take and hold the state mutex
	CAMutex::Locker theStateMutex(const_cast<SHP_Device*>(this)->GetDeviceStateMutex());
	
	//  create the controls if necessary
	if(IsControlRelatedProperty(inAddress.mSelector))
	{
		const_cast<SHP_Device*>(this)->CreateControls();
	}
	
	switch(inAddress.mSelector)
	{
		case kAudioDevicePropertyIOCycleUsage:
			theAnswer = sizeof(Float32);
			break;
				
		default:
			theAnswer = HP_Device::GetPropertyDataSize(inAddress, inQualifierDataSize, inQualifierData);
			break;
	};
	
	return theAnswer;
}

void	SHP_Device::GetPropertyData(const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32& ioDataSize, void* outData) const
{
	//	take and hold the state mutex
	CAMutex::Locker theStateMutex(const_cast<SHP_Device*>(this)->GetDeviceStateMutex());
	
	//  create the controls if necessary
	if(IsControlRelatedProperty(inAddress.mSelector))
	{
		const_cast<SHP_Device*>(this)->CreateControls();
	}
	
	switch(inAddress.mSelector)
	{
		case kAudioDevicePropertyHogMode:
			ThrowIf(ioDataSize != GetPropertyDataSize(inAddress, inQualifierDataSize, inQualifierData), CAException(kAudioHardwareBadPropertySizeError), "SHP_Device::GetPropertyData: wrong data size for kAudioDevicePropertyHogMode");
			*(static_cast<pid_t*>(outData)) = mHogMode->GetOwner();
			break;
			
		case kAudioDevicePropertyIOCycleUsage:
			ThrowIf(ioDataSize != GetPropertyDataSize(inAddress, inQualifierDataSize, inQualifierData), CAException(kAudioHardwareBadPropertySizeError), "SHP_Device::GetPropertyData: wrong data size for kAudioDevicePropertyIOCycleUsage");
			*(static_cast<Float32*>(outData)) = mIOThread->GetIOCycleUsage();
			break;
			
		default:
			HP_Device::GetPropertyData(inAddress, inQualifierDataSize, inQualifierData, ioDataSize, outData);
			break;
	};
}

void	SHP_Device::SetPropertyData(const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, const AudioTimeStamp* inWhen)
{
	//	take and hold the state mutex
	CAMutex::Locker theStateMutex(GetDeviceStateMutex());
	
	//  create the controls if necessary
	if(IsControlRelatedProperty(inAddress.mSelector))
	{
		CreateControls();
	}
	
	switch(inAddress.mSelector)
	{
		case kAudioDevicePropertyHogMode:
			ThrowIf(inDataSize != GetPropertyDataSize(inAddress, inQualifierDataSize, inQualifierData), CAException(kAudioHardwareBadPropertySizeError), "SHP_Device::SetPropertyData: wrong data size for kAudioDevicePropertyHogMode");
			if(mHogMode->IsFree())
			{
				mHogMode->Take();
			}
			else if(mHogMode->CurrentProcessIsOwner())
			{
				mHogMode->Release();
			}
			else
			{
				DebugMessage("SHP_Device::SetPropertyData: hog mode owned by another process");
				throw CAException(kAudioDevicePermissionsError);
			}
			HogModeStateChanged();
			*((pid_t*)inData) = mHogMode->GetOwner();
			break;
			
		case kAudioDevicePropertyIOCycleUsage:
			{
				ThrowIf(inDataSize != GetPropertyDataSize(inAddress, inQualifierDataSize, inQualifierData), CAException(kAudioHardwareBadPropertySizeError), "SHP_Device::SetPropertyData: wrong data size for kAudioDevicePropertyIOCycleUsage");
				mIOThread->SetIOCycleUsage(*(static_cast<const Float32*>(inData)));
				CAPropertyAddress theAddress(kAudioDevicePropertyIOCycleUsage, kAudioObjectPropertyScopeGlobal, 0);
				PropertiesChanged(1, &theAddress);
			}
			break;
			
		default:
			HP_Device::SetPropertyData(inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, inWhen);
			break;
	};
}

void	SHP_Device::PropertyListenerAdded(const AudioObjectPropertyAddress& inAddress)
{
	HP_Object::PropertyListenerAdded(inAddress);
	
	if((inAddress.mSelector == kAudioObjectPropertySelectorWildcard) || IsControlRelatedProperty(inAddress.mSelector))
	{
		//  make sure the controls have been loaded
		CreateControls();
	}
}

bool	SHP_Device::IsSafeToExecuteCommand()
{
	bool theAnswer = true;
	
	//	it isn't safe to execute commands from the IOThread
	if(mIOThread != NULL)
	{
		theAnswer = !mIOThread->IsCurrentThread();
	}
	
	return theAnswer;
}

bool	SHP_Device::StartCommandExecution(void** outSavedCommandState)
{
	//	lock the IOGuard since we're doing something that
	//	affects what goes on in the IOThread
	*outSavedCommandState = 0;
	
	if(mIOThread != NULL)
	{
		*outSavedCommandState = mIOThread->GetIOGuard().Lock() ? (void*)1 : (void*)0;
	}
	
	return true;
}

void	SHP_Device::FinishCommandExecution(void* inSavedCommandState)
{
	if((mIOThread != NULL) && (inSavedCommandState != 0))
	{
		mIOThread->GetIOGuard().Unlock();
	}
}

void	SHP_Device::Do_StartIOProc(AudioDeviceIOProc inProc)
{
	//	make sure we can start
	ThrowIf(!HogModeIsOwnedBySelfOrIsFree(), CAException(kAudioDevicePermissionsError), "SHP_Device::Do_StartIOProc: can't start the IOProc because hog mode is owned by another process");
	
	//	take hog mode if there are any non-mixable output streams
	if(HasAnyNonMixableStreams(false) && !HogModeIsOwnedBySelf())
	{
		mHogMode->Take();
	}
	
	//	start
	HP_Device::Do_StartIOProc(inProc);
}

void	SHP_Device::Do_StartIOProcAtTime(AudioDeviceIOProc inProc, AudioTimeStamp& ioStartTime, UInt32 inStartTimeFlags)
{
	//	make sure we can start
	ThrowIf(!HogModeIsOwnedBySelfOrIsFree(), CAException(kAudioDevicePermissionsError), "SHP_Device::Do_StartIOProcAtTime: can't start the IOProc because hog mode is owned by another process");
	
	//	take hog mode if there are any non-mixable output streams
	if(HasAnyNonMixableStreams(false) && !HogModeIsOwnedBySelf())
	{
		mHogMode->Take();
	}
	
	//	start
	HP_Device::Do_StartIOProcAtTime(inProc, ioStartTime, inStartTimeFlags);
}

CAGuard*	SHP_Device::GetIOGuard()
{
	//	this method returns the CAGuard that is to be used to synchronize with the IO cycle
	//	by default, there is no CAGuard to synchronize with
	return mIOThread->GetIOGuardPtr();
}

bool	SHP_Device::CallIOProcs(const AudioTimeStamp& inCurrentTime, const AudioTimeStamp& inInputTime, const AudioTimeStamp& inOutputTime)
{
	//	This method is called by during the IO cycle by HP_IOThread when it is time to read the
	//	input data call the IOProcs and write the output data. It returns whether or not the
	//	operation was successful. In this sample device, this method is broken up into
	//	smaller calls for specific phases of the cycle fore easier and saner handling.
	bool theHardwareIOSucceeded = true;
	
	StartIOCycle();
	
	//	read the input data
	if(HasInputStreams())
	{
		//	refresh the input buffers
		mIOProcList->RefreshIOProcBufferLists(true);
		
		//	pre-process the input data
		PreProcessInputData(inInputTime);
		
		//	read the data
		theHardwareIOSucceeded = ReadInputData(inInputTime, GetIOBufferSetID());
		
		//	post-process the data that was read
		if(theHardwareIOSucceeded)
		{
			PostProcessInputData(inInputTime);
		}
	}
	
	if(theHardwareIOSucceeded)
	{
		//	get the shared input buffer list
		AudioBufferList* theInputBufferList = mIOProcList->GetSharedAudioBufferList(true);
	
		//	mark the telemetry
		mIOCycleTelemetry->IOCycleIOProcsBegin(GetIOCycleNumber());
		
		//	iterate through the IOProcs
		UInt32 theNumberIOProcs = mIOProcList->GetNumberIOProcs();
		for(UInt32 theIOProcIndex = 0; theIOProcIndex < theNumberIOProcs; ++theIOProcIndex)
		{
			//	get the IO proc
			HP_IOProc* theIOProc = mIOProcList->GetIOProcByIndex(theIOProcIndex);
			
			//	call it
			theIOProc->Call(inCurrentTime, inInputTime, theInputBufferList, inOutputTime, NULL);
			
			//	pre-process it before handing it to the hardware
			PreProcessOutputData(inOutputTime, *theIOProc);
		}
		
		//	mark the telemetry
		mIOCycleTelemetry->IOCycleIOProcsEnd(GetIOCycleNumber());
		
		//	write the output data
		if(HasOutputStreams())
		{
			theHardwareIOSucceeded = WriteOutputData(inOutputTime, GetIOBufferSetID());
		}
	}
	
	FinishIOCycle();
	
	return theHardwareIOSucceeded;
}

void	SHP_Device::StartIOEngine()
{
	//	the IOGuard should already be held prior to calling this routine
	if(!IsIOEngineRunning())
	{
		StartHardware();
		mIOThread->Start();
	}
}

void	SHP_Device::StartIOEngineAtTime(const AudioTimeStamp& inStartTime, UInt32 inStartTimeFlags)
{
	//	the IOGuard should already be held prior to calling this routine
	if(!IsIOEngineRunning())
	{
		//	if the engine isn't already running, then just start it
		StartHardware();
		mIOThread->Start();
	}
	else
	{
		//	the engine is already running, so we have to resynch the IO thread to the new start time
		AudioTimeStamp theStartSampleTime = inStartTime;
		theStartSampleTime.mFlags = kAudioTimeStampSampleTimeValid;
		
		//	factor out the input/output-ness of the start time to get the sample time of the anchor point
		if((inStartTimeFlags & kAudioDeviceStartTimeIsInputFlag) != 0)
		{
			theStartSampleTime.mSampleTime += GetIOBufferFrameSize();
			theStartSampleTime.mSampleTime += GetSafetyOffset(true);
		}
		else
		{
			theStartSampleTime.mSampleTime -= GetIOBufferFrameSize();
			theStartSampleTime.mSampleTime -= GetSafetyOffset(false);
		}
		
		//	need an extra cycle to ensure correctness
		theStartSampleTime.mSampleTime -= GetIOBufferFrameSize();
		
		//	calculate the host time of the anchor point
		AudioTimeStamp theStartTime;
		theStartTime.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
		TranslateTime(theStartSampleTime, theStartTime);
		
		//	resynch the IO thread
		mIOThread->Resynch(&theStartTime, true);
		mIOCycleTelemetry->Resynch(GetIOCycleNumber(), theStartTime);
	}
}

void	SHP_Device::StopIOEngine()
{
	//	the IOGuard should already be held prior to calling this routine
	mIOThread->Stop();
	StopHardware();
}

void	SHP_Device::StartHardware()
{
	#if Log_HardareStartStop
		DebugMessage("SHP_Device::StartHardware: starting the hardware");
	#endif
}

void	SHP_Device::StopHardware()
{
	#if Log_HardareStartStop
		DebugMessage("SHP_Device::StopHardware: stopping the hardware");
	#endif
}

void	SHP_Device::StartIOCycle()
{
	//	this method is called at the beginning of the IO cycle to kick things off
}

void	SHP_Device::PreProcessInputData(const AudioTimeStamp& /*inInputTime*/)
{
	//	this method is called just prior to reading the input data
}

bool	SHP_Device::ReadInputData(const AudioTimeStamp& /*inStartTime*/, UInt32 /*inBufferSetID*/)
{
	//	this method is called to read the input data
	//	it returns true if the read completed successfully
	return true;
}

void	SHP_Device::PostProcessInputData(const AudioTimeStamp& /*inInputTime*/)
{
	//	this method is called just after reading the input data but prior to handing it to any IOProcs
	
	//	get the input buffer list
	AudioBufferList* theInputBufferList = mIOProcList->GetSharedAudioBufferList(true);
	
	//	mark the telemetry
	if((theInputBufferList != NULL) && mIOCycleTelemetry->IsCapturing() && CAAudioBufferList::HasData(*theInputBufferList))
	{
		mIOCycleTelemetry->InputDataPresent(GetIOCycleNumber());
	}
}

void	SHP_Device::PreProcessOutputData(const AudioTimeStamp& /*inOuputTime*/, HP_IOProc& inIOProc)
{
	//	this method is called just after getting the data from the IOProc but before writing it to the hardware
	if(mIOCycleTelemetry->IsCapturing() && inIOProc.BufferListHasData(false))
	{
		mIOCycleTelemetry->OutputDataPresent(GetIOCycleNumber());
	}
}

bool	SHP_Device::WriteOutputData(const AudioTimeStamp& /*inStartTime*/, UInt32 /*inBufferSetID*/)
{
	//	this method is called to write the output data
	//	it returns true if the write completed successfully
	return true;
}

void	SHP_Device::FinishIOCycle()
{
	//	this method is called at the end of the IO cycle
}

UInt32  SHP_Device::GetIOCycleNumber() const
{
	return mIOThread->GetIOCycleNumber();
}

void	SHP_Device::GetCurrentTime(AudioTimeStamp& outTime)
{
	ThrowIf(!IsIOEngineRunning(), CAException(kAudioHardwareNotRunningError), "SHP_Device::GetCurrentTime: can't because the engine isn't running");
	
	//	compute the host ticks pere frame
	Float64 theActualHostTicksPerFrame = CAHostTimeBase::GetFrequency() / GetCurrentNominalSampleRate();
	
	//	clear the output time stamp
	outTime = CAAudioTimeStamp::kZero;
	
	//	put in the current host time
	outTime.mHostTime = CAHostTimeBase::GetTheCurrentTime();
	
	//	calculate how many host ticks away from the anchor time stamp the current host time is
	Float64 theSampleOffset = 0.0;
	if(outTime.mHostTime >= mAnchorHostTime)
	{
		theSampleOffset = outTime.mHostTime - mAnchorHostTime;
	}
	else
	{
		//	do it this way to avoid overflow problems with the unsigned numbers
		theSampleOffset = mAnchorHostTime - outTime.mHostTime;
		theSampleOffset *= -1.0;
	}
	
	//	convert it to a number of samples
	theSampleOffset /= theActualHostTicksPerFrame;
	
	//	lop off the fractional sample
	theSampleOffset = floor(theSampleOffset);
	
	//	put in the sample time
	outTime.mSampleTime = theSampleOffset;
	
	//	put in the rate scalar
	outTime.mRateScalar = 1.0;
	
	//	set the flags
	outTime.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid | kAudioTimeStampRateScalarValid;
}

void	SHP_Device::SafeGetCurrentTime(AudioTimeStamp& outTime)
{
	//	The difference between GetCurrentTime and SafeGetCurrentTime is that GetCurrentTime should only
	//	be called in situations where the device state or clock state is in a known good state, such
	//	as during the IO cycle. Being in a known good state allows GetCurrentTime to bypass any
	//	locks that ensure coherent cross-thread access to the device time base info.
	//	SafeGetCurrentTime, then, will be called when the state is in question and all the locks should
	//	be obeyed.
	
	//	Our state here in the sample device has no such threading issues, so we pass this call on
	//	to GetCurrentTime.
	GetCurrentTime(outTime);
}

void	SHP_Device::TranslateTime(const AudioTimeStamp& inTime, AudioTimeStamp& outTime)
{
	//	the input time stamp has to have at least one of the sample or host time valid
	ThrowIf((inTime.mFlags & kAudioTimeStampSampleHostTimeValid) == 0, CAException(kAudioHardwareIllegalOperationError), "SHP_Device::TranslateTime: have to have either sample time or host time valid on the input");
	ThrowIf(!IsIOEngineRunning(), CAException(kAudioHardwareNotRunningError), "SHP_Device::TranslateTime: can't because the engine isn't running");

	//	compute the host ticks pere frame
	Float64 theActualHostTicksPerFrame = CAHostTimeBase::GetFrequency() / GetCurrentNominalSampleRate();

	//	calculate the sample time
	Float64 theOffset = 0.0;
	if((outTime.mFlags & kAudioTimeStampSampleTimeValid) != 0)
	{
		if((inTime.mFlags & kAudioTimeStampSampleTimeValid) != 0)
		{
			//	no calculations necessary
			outTime.mSampleTime = inTime.mSampleTime;
		}
		else if((inTime.mFlags & kAudioTimeStampHostTimeValid) != 0)
		{
			//	calculate how many host ticks away from the current 0 time stamp the input host time is
			if(inTime.mHostTime >= mAnchorHostTime)
			{
				theOffset = inTime.mHostTime - mAnchorHostTime;
			}
			else
			{
				//	do it this way to avoid overflow problems with the unsigned numbers
				theOffset = mAnchorHostTime - inTime.mHostTime;
				theOffset *= -1.0;
			}
			
			//	convert it to a number of samples
			theOffset /= theActualHostTicksPerFrame;
			
			//	lop off the fractional sample
			outTime.mSampleTime = floor(theOffset);
		}
		else
		{
			//	no basis for projection, so put in a 0
			outTime.mSampleTime = 0;
		}
	}
	
	//	calculate the host time
	if((outTime.mFlags & kAudioTimeStampHostTimeValid) != 0)
	{
		if((inTime.mFlags & kAudioTimeStampHostTimeValid) != 0)
		{
			//	no calculations necessary
			outTime.mHostTime = inTime.mHostTime;
		}
		else if((inTime.mFlags & kAudioTimeStampSampleTimeValid) != 0)
		{
			//	calculate how many samples away from the current 0 time stamp the input sample time is
			theOffset = inTime.mSampleTime;
			
			//	convert it to a number of host ticks
			theOffset *= theActualHostTicksPerFrame;
			
			//	lop off the fractional host tick
			theOffset = floor(theOffset);
			
			//	put in the host time as an offset from the 0 time stamp's host time
			outTime.mHostTime = mAnchorHostTime + static_cast<UInt64>(theOffset);
		}
		else
		{
			//	no basis for projection, so put in a 0
			outTime.mHostTime = 0;
		}
	}
	
	//	calculate the rate scalar
	if(outTime.mFlags & kAudioTimeStampRateScalarValid)
	{
		//	the sample device has perfect timing
		outTime.mRateScalar = 1.0;
	}
}

void	SHP_Device::GetNearestStartTime(AudioTimeStamp& ioRequestedStartTime, UInt32 inFlags)
{
	bool isConsultingHAL = (inFlags & kAudioDeviceStartTimeDontConsultHALFlag) == 0;
	bool isConsultingDevice = (inFlags & kAudioDeviceStartTimeDontConsultDeviceFlag) == 0;

	ThrowIf(!IsIOEngineRunning(), CAException(kAudioHardwareNotRunningError), "SHP_Device::GetNearestStartTime: can't because there isn't anything running yet");
	ThrowIf(!isConsultingHAL && !isConsultingDevice, CAException(kAudioHardwareNotRunningError), "SHP_Device::GetNearestStartTime: can't because the start time flags are conflicting");

	UInt32 theIOBufferFrameSize = GetIOBufferFrameSize();
	bool isInput = (inFlags & kAudioDeviceStartTimeIsInputFlag) != 0;
	UInt32 theSafetyOffset = GetSafetyOffset(isInput);
	
	//	fix up the requested time so we have everything we need
	AudioTimeStamp theRequestedStartTime;
	theRequestedStartTime.mFlags = ioRequestedStartTime.mFlags | kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
	TranslateTime(ioRequestedStartTime, theRequestedStartTime);
	
	//	figure out the requested position in terms of the IO thread position
	AudioTimeStamp theTrueRequestedStartTime = theRequestedStartTime;

	//  only do this math if we are supposed to consult the HAL
	if(isConsultingHAL)
	{
		theTrueRequestedStartTime.mFlags = kAudioTimeStampSampleTimeValid;
		if(isInput)
		{
			theTrueRequestedStartTime.mSampleTime += theIOBufferFrameSize;
			theTrueRequestedStartTime.mSampleTime += theSafetyOffset;
		}
		else
		{
			theTrueRequestedStartTime.mSampleTime -= theIOBufferFrameSize;
			theTrueRequestedStartTime.mSampleTime -= theSafetyOffset;
		}
			
		AudioTimeStamp theMinimumStartSampleTime;
		AudioTimeStamp theMinimumStartTime;
		if(mIOProcList->IsOnlyNULLEnabled())
		{
			//	no IOProcs are enabled, so we can start whenever
			
			//	the minimum starting time is the current time
			GetCurrentTime(theMinimumStartSampleTime);
			
			//	plus some slop
			theMinimumStartSampleTime.mSampleTime += theSafetyOffset + (2 * theIOBufferFrameSize);
			theMinimumStartTime.mFlags = kAudioTimeStampSampleTimeValid;
			
			if(theTrueRequestedStartTime.mSampleTime < theMinimumStartSampleTime.mSampleTime)
			{
				//	clamp it to the minimum
				theTrueRequestedStartTime = theMinimumStartSampleTime;
			}
		}
		else if(mIOProcList->IsAnythingEnabled())
		{
			//	an IOProc is already running, so the next start time is two buffers
			//	from wherever the IO thread is currently
			mIOThread->GetCurrentPosition(theMinimumStartSampleTime);
			theMinimumStartSampleTime.mSampleTime += (2 * theIOBufferFrameSize);
			theMinimumStartTime.mFlags = kAudioTimeStampSampleTimeValid;
			
			if(theTrueRequestedStartTime.mSampleTime < theMinimumStartSampleTime.mSampleTime)
			{
				//	clamp it to the minimum
				theTrueRequestedStartTime = theMinimumStartSampleTime;
			}
			else if(theTrueRequestedStartTime.mSampleTime > theMinimumStartSampleTime.mSampleTime)
			{
				//	clamp it to an even IO cycle
				UInt32 theNumberBuffers = static_cast<UInt32>(theTrueRequestedStartTime.mSampleTime - theMinimumStartSampleTime.mSampleTime);
				theNumberBuffers /= theIOBufferFrameSize;
				theNumberBuffers += 2;
				
				theTrueRequestedStartTime.mSampleTime = theMinimumStartSampleTime.mSampleTime + (theNumberBuffers * theIOBufferFrameSize);
			}
		}
		
		//	bump the sample time in the right direction
		if(isInput)
		{
			theTrueRequestedStartTime.mSampleTime -= theIOBufferFrameSize;
			theTrueRequestedStartTime.mSampleTime -= theSafetyOffset;
		}
		else
		{
			theTrueRequestedStartTime.mSampleTime += theIOBufferFrameSize;
			theTrueRequestedStartTime.mSampleTime += theSafetyOffset;
		}
	}
		
	//	convert it back if neccessary
	if(theTrueRequestedStartTime.mSampleTime != theRequestedStartTime.mSampleTime)
	{
		TranslateTime(theTrueRequestedStartTime, theRequestedStartTime);
	}
	
	//	now filter it through the hardware, unless told not to
	if(mIOProcList->IsOnlyNULLEnabled() && isConsultingDevice)
	{
	}
	
	//	assign the return value
	ioRequestedStartTime = theRequestedStartTime;
}

void	SHP_Device::StartIOCycleTimingServices()
{
	//	Note that the IOGuard is _not_ held during this call!
	
	//	This method is called when an IO thread is in it's initialization phase
	//	prior to it requiring any timing services. The device's timing services
	//	should be initialized when this method returns.
	
	//	in this sample driver, we base our timing on the CPU clock and assume a perfect sample rate
	mAnchorHostTime = CAHostTimeBase::GetCurrentTime();
}

bool	SHP_Device::UpdateIOCycleTimingServices()
{
	//	This method is called by an IO cycle when it's cycle starts.
	return true;
}

void	SHP_Device::StopIOCycleTimingServices()
{
	//	This method is called when an IO cycle has completed it's run and is tearing down.
	mAnchorHostTime = 0;
}

void	SHP_Device::CreateStreams()
{
	//  common variables
	OSStatus		theError = 0;
	AudioObjectID   theNewStreamID = 0;
	SHP_Stream*		theStream = NULL;

	//  create a vector of AudioStreamIDs to hold the stream ids we are creating
	std::vector<AudioStreamID> theStreamIDs;
	
	//  in this sample device there is only one input stream and one output stream

	//  instantiate an AudioStream
#if	(MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_4)
	theError = AudioHardwareClaimAudioStreamID(mSHPPlugIn->GetInterface(), GetObjectID(), &theNewStreamID);
#else
	theError = AudioObjectCreate(mSHPPlugIn->GetInterface(), GetObjectID(), kAudioStreamClassID, &theNewStreamID);
#endif
	if(theError == 0)
	{
		//  create the stream
		theStream = new SHP_Stream(theNewStreamID, mSHPPlugIn, this, true, 1);
		theStream->Initialize();
		
		//	add to the list of streams in this device
		AddStream(theStream);
		
		//  store the new stream ID
		theStreamIDs.push_back(theNewStreamID);
	}

	//  claim a stream ID for the stream
#if	(MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_4)
	theError = AudioHardwareClaimAudioStreamID(mSHPPlugIn->GetInterface(), GetObjectID(), &theNewStreamID);
#else
	theError = AudioObjectCreate(mSHPPlugIn->GetInterface(), GetObjectID(), kAudioStreamClassID, &theNewStreamID);
#endif
	if(theError == 0)
	{
		//  create the stream
		theStream = new SHP_Stream(theNewStreamID, mSHPPlugIn, this, false, 1);
		theStream->Initialize();
		
		//	add to the list of streams in this device
		AddStream(theStream);
		
		//  store the new stream ID
		theStreamIDs.push_back(theNewStreamID);
	}

	//  now tell the HAL about the new stream IDs
	if(theStreamIDs.size() != 0)
	{
		//	set the object state mutexes
		for(std::vector<AudioStreamID>::iterator theIterator = theStreamIDs.begin(); theIterator != theStreamIDs.end(); std::advance(theIterator, 1))
		{
			HP_Object* theObject = HP_Object::GetObjectByID(*theIterator);
			if(theObject != NULL)
			{
				HP_Object::SetObjectStateMutexForID(*theIterator, theObject->GetObjectStateMutex());
			}
		}
		
#if	(MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_4)
		theError = AudioHardwareStreamsCreated(mSHPPlugIn->GetInterface(), GetObjectID(), theStreamIDs.size(), &(theStreamIDs.front()));
#else
		theError = AudioObjectsPublishedAndDied(mSHPPlugIn->GetInterface(), GetObjectID(), theStreamIDs.size(), &(theStreamIDs.front()), 0, NULL);
#endif
		ThrowIfError(theError, CAException(theError), "SHP_Device::CreateStreams: couldn't tell the HAL about the streams");
	}
}

void	SHP_Device::ReleaseStreams()
{
	//	This method is only called when tearing down, so there isn't any need to inform the HAL about changes
	//	since the HAL has long since released it's internal representation of these stream objects. Note that
	//	if this method needs to be called outside of teardown, it would need to be modified to call
	//	AudioObjectsPublishedAndDied (or AudioHardwareStreamsDied on pre-Tiger systems) to notify the HAL about
	//	the state change.
	while(GetNumberStreams(true) > 0)
	{
		//	get the stream
		SHP_Stream* theStream = static_cast<SHP_Stream*>(GetStreamByIndex(true, 0));
		
		//	remove the object state mutex
		HP_Object::SetObjectStateMutexForID(theStream->GetObjectID(), NULL);

		//	remove it from the lists
		RemoveStream(theStream);
		
		//	toss it
		theStream->Teardown();
		delete theStream;
	}
	
	while(GetNumberStreams(false) > 0)
	{
		//	get the stream
		SHP_Stream* theStream = static_cast<SHP_Stream*>(GetStreamByIndex(false, 0));
		
		//	remove the object state mutex
		HP_Object::SetObjectStateMutexForID(theStream->GetObjectID(), NULL);

		//	remove it from the lists
		RemoveStream(theStream);
		
		//	toss it
		theStream->Teardown();
		delete theStream;
	}
}

void	SHP_Device::RefreshAvailableStreamFormats()
{
	UInt32 theStreamIndex;
	UInt32 theNumberStreams;
	SHP_Stream* theStream;
	
	theNumberStreams = GetNumberStreams(true);
	for(theStreamIndex = 0; theStreamIndex < theNumberStreams; ++theStreamIndex)
	{
		theStream = static_cast<SHP_Stream*>(GetStreamByIndex(true, theStreamIndex));
		theStream->RefreshAvailablePhysicalFormats();
	}
	
	theNumberStreams = GetNumberStreams(false);
	for(theStreamIndex = 0; theStreamIndex < theNumberStreams; ++theStreamIndex)
	{
		theStream = static_cast<SHP_Stream*>(GetStreamByIndex(false, theStreamIndex));
		theStream->RefreshAvailablePhysicalFormats();
	}
}

void	SHP_Device::CreateControls()
{
	if(!mControlsInitialized)
	{
		OSStatus theError = 0;
		UInt32 theNumberChannels = 0;
		UInt32 theChannelIndex = 0;
		HP_Control* theControl = NULL;
		
		mControlsInitialized = true;
	
#if	(MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_4)
		//  create a vector of AudioObjectIDs to hold the object ids we are creating
		AudioObjectID theControlID;
		std::vector<AudioObjectID> theControlIDs;
#endif
		
		//	get the number of input channels so we can make a channel strip for each channel
	
		//  iterate through the input channels
		theNumberChannels = GetTotalNumberChannels(true);
		for(theChannelIndex = 0; theChannelIndex <= theNumberChannels; ++theChannelIndex)
		{
			//	make an input volume control
			
			//  instantiate an AudioControl
#if	(MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_4)
			theError = AudioObjectCreate(mSHPPlugIn->GetInterface(), GetObjectID(), kAudioVolumeControlClassID, &theControlID);
			if(theError == 0)
#endif
			{
				//  create the control
				theControl = new SHP_LevelControl(theControlID, kAudioVolumeControlClassID, kAudioDevicePropertyScopeInput, theChannelIndex, mSHPPlugIn, this);
				theControl->Initialize();

				//  add it to the list
				AddControl(theControl);
			
				//  store the new stream ID
				theControlIDs.push_back(theControlID);
			}
			
			//	make an input mute control
			
			//  instantiate an AudioControl
#if	(MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_4)
			theError = AudioObjectCreate(mSHPPlugIn->GetInterface(), GetObjectID(), kAudioMuteControlClassID, &theControlID);
			if(theError == 0)
#endif
			{
				//  create the control
				theControl = new SHP_BooleanControl(theControlID, kAudioMuteControlClassID, kAudioDevicePropertyScopeInput, theChannelIndex, mSHPPlugIn, this);
				theControl->Initialize();

				//  add it to the list
				AddControl(theControl);
			
				//  store the new stream ID
				theControlIDs.push_back(theControlID);
			}
			
			//	make an input data source control
			
			//  instantiate an AudioControl
#if	(MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_4)
			theError = AudioObjectCreate(mSHPPlugIn->GetInterface(), GetObjectID(), kAudioDataSourceControlClassID, &theControlID);
			if(theError == 0)
#endif
			{
				//  create the control
				theControl = new SHP_SelectorControl(theControlID, kAudioDataSourceControlClassID, kAudioDevicePropertyScopeInput, theChannelIndex, mSHPPlugIn, this);
				theControl->Initialize();

				//  add it to the list
				AddControl(theControl);
			
				//  store the new stream ID
				theControlIDs.push_back(theControlID);
			}			
		}
	
		//  iterate through the output channels
		theNumberChannels = GetTotalNumberChannels(false);
		for(theChannelIndex = 0; theChannelIndex <= theNumberChannels; ++theChannelIndex)
		{
			//	make an output volume control
			
			//  instantiate an AudioControl
#if	(MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_4)
			theError = AudioObjectCreate(mSHPPlugIn->GetInterface(), GetObjectID(), kAudioVolumeControlClassID, &theControlID);
			if(theError == 0)
#endif
			{
				//  create the control
				theControl = new SHP_LevelControl(theControlID, kAudioVolumeControlClassID, kAudioDevicePropertyScopeOutput, theChannelIndex, mSHPPlugIn, this);
				theControl->Initialize();

				//  add it to the list
				AddControl(theControl);
			
				//  store the new stream ID
				theControlIDs.push_back(theControlID);
			}
			
			//	make an output mute control
			
			//  instantiate an AudioControl
#if	(MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_4)
			theError = AudioObjectCreate(mSHPPlugIn->GetInterface(), GetObjectID(), kAudioMuteControlClassID, &theControlID);
			if(theError == 0)
#endif
			{
				//  create the control
				theControl = new SHP_BooleanControl(theControlID, kAudioMuteControlClassID, kAudioDevicePropertyScopeOutput, theChannelIndex, mSHPPlugIn, this);
				theControl->Initialize();

				//  add it to the list
				AddControl(theControl);
			
				//  store the new stream ID
				theControlIDs.push_back(theControlID);
			}
			
			//	make an output data source control
			
			//  instantiate an AudioControl
#if	(MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_4)
			theError = AudioObjectCreate(mSHPPlugIn->GetInterface(), GetObjectID(), kAudioDataSourceControlClassID, &theControlID);
			if(theError == 0)
#endif
			{
				//  create the control
				theControl = new SHP_SelectorControl(theControlID, kAudioDataSourceControlClassID, kAudioDevicePropertyScopeOutput, theChannelIndex, mSHPPlugIn, this);
				theControl->Initialize();

				//  add it to the list
				AddControl(theControl);
			
				//  store the new stream ID
				theControlIDs.push_back(theControlID);
			}			
		}
		
#if	(MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_4)
		//	tell the HAL about the new controls
		if(theControlIDs.size() > 0)
		{
			//	set the object state mutexes
			for(std::vector<AudioObjectID>::iterator theIterator = theControlIDs.begin(); theIterator != theControlIDs.end(); std::advance(theIterator, 1))
			{
				HP_Object* theObject = HP_Object::GetObjectByID(*theIterator);
				if(theObject != NULL)
				{
					HP_Object::SetObjectStateMutexForID(*theIterator, theObject->GetObjectStateMutex());
				}
			}
		
			theError = AudioObjectsPublishedAndDied(mSHPPlugIn->GetInterface(), GetObjectID(), theControlIDs.size(), &(theControlIDs.front()), 0, NULL);
			ThrowIfError(theError, CAException(theError), "SHP_Device::CreateControls: couldn't tell the HAL about the controls");
		}
#endif
	}
}

void	SHP_Device::ReleaseControls()
{
	//	This method is only called when tearing down, so there isn't any need to inform the HAL about changes
	//	since the HAL has long since released it's internal representation of these control objects. Note that
	//	if this method needs to be called outside of teardown, it would need to be modified to call
	//	AudioObjectsPublishedAndDied (or nothing on pre-Tiger systems, since controls weren't first
	//	class objects yet) to notify the HAL about the state change.
	if(mControlsInitialized)
	{
		mControlsInitialized = false;
		ControlList::iterator theIterator = mControlList.begin();
		while(theIterator != mControlList.end())
		{
			HP_Control* theControl = *theIterator;
			HP_Object::SetObjectStateMutexForID(theControl->GetObjectID(), NULL);
			theControl->Teardown();
			delete theControl;
			std::advance(theIterator, 1);
		}
		mControlList.clear();
	}
}

bool	SHP_Device::IsControlRelatedProperty(AudioObjectPropertySelector inSelector)
{
	//	This function determines whether or not a given property selector might be implemented by a
	//	control object. Note that this list only covers standard control properties and would need
	//	to be augmented by any custom properties the device may have.
	bool theAnswer = false;
	
	switch(inSelector)
	{
		//  AudioObject Properties
		case kAudioObjectPropertyOwnedObjects:
		
		//	AudioSystem Properties
		case kAudioHardwarePropertyBootChimeVolumeScalar:
		case kAudioHardwarePropertyBootChimeVolumeDecibels:
		case kAudioHardwarePropertyBootChimeVolumeRangeDecibels:
		case kAudioHardwarePropertyBootChimeVolumeScalarToDecibels:
		case kAudioHardwarePropertyBootChimeVolumeDecibelsToScalar:
		
		//  AudioDevice Properties
		case kAudioDevicePropertyJackIsConnected:
		case kAudioDevicePropertyVolumeScalar:
		case kAudioDevicePropertyVolumeDecibels:
		case kAudioDevicePropertyVolumeRangeDecibels:
		case kAudioDevicePropertyVolumeScalarToDecibels:
		case kAudioDevicePropertyVolumeDecibelsToScalar:
		case kAudioDevicePropertyStereoPan:
		case kAudioDevicePropertyStereoPanChannels:
		case kAudioDevicePropertyMute:
		case kAudioDevicePropertySolo:
		case kAudioDevicePropertyDataSource:
		case kAudioDevicePropertyDataSources:
		case kAudioDevicePropertyDataSourceNameForIDCFString:
		case kAudioDevicePropertyClockSource:
		case kAudioDevicePropertyClockSources:
		case kAudioDevicePropertyClockSourceNameForIDCFString:
		case kAudioDevicePropertyClockSourceKindForID:
		case kAudioDevicePropertyPlayThru:
		case kAudioDevicePropertyPlayThruSolo:
		case kAudioDevicePropertyPlayThruVolumeScalar:
		case kAudioDevicePropertyPlayThruVolumeDecibels:
		case kAudioDevicePropertyPlayThruVolumeRangeDecibels:
		case kAudioDevicePropertyPlayThruVolumeScalarToDecibels:
		case kAudioDevicePropertyPlayThruVolumeDecibelsToScalar:
		case kAudioDevicePropertyPlayThruStereoPan:
		case kAudioDevicePropertyPlayThruStereoPanChannels:
		case kAudioDevicePropertyPlayThruDestination:
		case kAudioDevicePropertyPlayThruDestinations:
		case kAudioDevicePropertyPlayThruDestinationNameForIDCFString:
		case kAudioDevicePropertyChannelNominalLineLevel:
		case kAudioDevicePropertyChannelNominalLineLevels:
		case kAudioDevicePropertyChannelNominalLineLevelNameForIDCFString:
		case kAudioDevicePropertyDriverShouldOwniSub:
		case kAudioDevicePropertySubVolumeScalar:
		case kAudioDevicePropertySubVolumeDecibels:
		case kAudioDevicePropertySubVolumeRangeDecibels:
		case kAudioDevicePropertySubVolumeScalarToDecibels:
		case kAudioDevicePropertySubVolumeDecibelsToScalar:
		case kAudioDevicePropertySubMute:
		case kAudioDevicePropertyDataSourceNameForID:
		case kAudioDevicePropertyClockSourceNameForID:
		case kAudioDevicePropertyPlayThruDestinationNameForID:
		case kAudioDevicePropertyChannelNominalLineLevelNameForID:
			theAnswer = true;
			break;
	};
	
	return theAnswer;
}
