#include "OVR_CAPI.h"
#include "OVR_Version.h"
#include "XR_Math.h"

#include "version.h"

#include "Common.h"
#include "Session.h"
#include "Runtime.h"
#include "InputManager.h"
#include "SwapChain.h"

#include <Windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <list>
#include <vector>
#include <algorithm>
#include <thread>
#include <detours/detours.h>

#define REV_DEFAULT_TIMEOUT 10000

XrInstance g_Instance = XR_NULL_HANDLE;
std::list<ovrHmdStruct> g_Sessions;

bool LoadRenderDoc()
{
	LONG error = ERROR_SUCCESS;

	// Open the libraries key
	char keyPath[MAX_PATH] = { "RenderDoc.RDCCapture.1\\DefaultIcon" };
	HKEY iconKey;
	error = RegOpenKeyExA(HKEY_CLASSES_ROOT, keyPath, 0, KEY_READ, &iconKey);
	if (error != ERROR_SUCCESS)
		return false;

	// Get the default library
	char path[MAX_PATH];
	DWORD length = MAX_PATH;
	error = RegQueryValueExA(iconKey, "", NULL, NULL, (PBYTE)path, &length);
	RegCloseKey(iconKey);
	if (error != ERROR_SUCCESS)
		return false;

	if (path[0] == '\0')
		return false;

	strcpy(strrchr(path, '\\') + 1, "renderdoc.dll");
	return LoadLibraryA(path) != NULL;
}

void AttachDetours();
void DetachDetours();

OVR_PUBLIC_FUNCTION(ovrResult) ovr_Initialize(const ovrInitParams* params)
{
	if (g_Instance)
		return ovrSuccess;

#if 0
	LoadRenderDoc();
#endif

	MicroProfileOnThreadCreate("Main");
	MicroProfileSetForceEnable(true);
	MicroProfileSetEnableAllGroups(true);
	MicroProfileSetForceMetaCounters(true);
	MicroProfileWebServerStart();

	DetachDetours();
	ovrResult rs = Runtime::Get().CreateInstance(&g_Instance, params);
	AttachDetours();
	return rs;
}

OVR_PUBLIC_FUNCTION(void) ovr_Shutdown()
{
	REV_TRACE(ovr_Shutdown);

	// End all sessions
	std::vector<XrSession> ToDestroy;
	auto it = g_Sessions.begin();
	while (it != g_Sessions.end())
	{
		// After years of work I have perfected my most unmaintainable line of
		// code. It's very important that the iterator is incremented after the
		// pointer is taken but before ovr_Destroy() is called or we *will* crash.
		ovr_Destroy(&*it++);
	}

	// Destroy and reset the instance
	XrResult rs = xrDestroyInstance(g_Instance);
	assert(XR_SUCCEEDED(rs));
	g_Instance = XR_NULL_HANDLE;

	MicroProfileShutdown();
}

OVR_PUBLIC_FUNCTION(void) ovr_GetLastErrorInfo(ovrErrorInfo* errorInfo)
{
	REV_TRACE(ovr_GetLastErrorInfo);

	if (!errorInfo)
		return;

	xrResultToString(g_Instance, g_LastResult, errorInfo->ErrorString);
	errorInfo->Result = ResultToOvrResult(g_LastResult);
}

OVR_PUBLIC_FUNCTION(const char*) ovr_GetVersionString()
{
	REV_TRACE(ovr_GetVersionString);

	return OVR_VERSION_STRING;
}

OVR_PUBLIC_FUNCTION(int) ovr_TraceMessage(int level, const char* message) { return 0; /* Debugging feature */ }

OVR_PUBLIC_FUNCTION(ovrResult) ovr_IdentifyClient(const char* identity) { return ovrSuccess; /* Debugging feature */ }

OVR_PUBLIC_FUNCTION(ovrHmdDesc) ovr_GetHmdDesc(ovrSession session)
{
	REV_TRACE(ovr_GetHmdDesc);

	ovrHmdDesc desc = { Runtime::Get().MinorVersion < 38 ? ovrHmd_CV1 : ovrHmd_RiftS };
	if (!session)
		return desc;

	XrInstanceProperties props = XR_TYPE(INSTANCE_PROPERTIES);
	xrGetInstanceProperties(session->Instance, &props);

	strcpy_s(desc.ProductName, 64, "Oculus Rift S");
	strcpy_s(desc.Manufacturer, 64, props.runtimeName);

	if (session->SystemProperties.trackingProperties.orientationTracking)
		desc.AvailableTrackingCaps |= ovrTrackingCap_Orientation;
	if (session->SystemProperties.trackingProperties.positionTracking)
		desc.AvailableTrackingCaps |= ovrTrackingCap_Orientation;
	desc.DefaultTrackingCaps = desc.AvailableTrackingCaps;

	for (int i = 0; i < ovrEye_Count; i++)
	{
		// Compensate for the 3-DOF eye pose on pre-1.17
		if (Runtime::Get().MinorVersion < 17)
		{
			desc.DefaultEyeFov[i] = OVR::FovPort::Uncant(XR::FovPort(session->ViewPoses[i].fov), XR::Quatf(session->ViewPoses[i].pose.orientation));
			desc.MaxEyeFov[i] = desc.DefaultEyeFov[i];
		}
		else
		{
			desc.DefaultEyeFov[i] = XR::FovPort(session->ViewFov[i].recommendedFov);
			desc.MaxEyeFov[i] = XR::FovPort(session->ViewFov[i].maxMutableFov);
		}
		desc.Resolution.w += (int)session->ViewConfigs[i].recommendedImageRectWidth;
		desc.Resolution.h = std::max(desc.Resolution.h, (int)session->ViewConfigs[i].recommendedImageRectHeight);
	}

	XrIndexedFrameState* frame = session->CurrentFrame;
	desc.DisplayRefreshRate = frame->predictedDisplayPeriod > 0 ? 1e9f / frame->predictedDisplayPeriod : 90.0f;
	return desc;
}

OVR_PUBLIC_FUNCTION(unsigned int) ovr_GetTrackerCount(ovrSession session)
{
	REV_TRACE(ovr_GetTrackerCount);

	if (!session)
		return ovrError_InvalidSession;

	// Pre-1.37 applications need virtual sensors to avoid a loss of tracking being detected
	return Runtime::Get().MinorVersion < 37 ? 3 : 0;
}

OVR_PUBLIC_FUNCTION(ovrTrackerDesc) ovr_GetTrackerDesc(ovrSession session, unsigned int trackerDescIndex)
{
	REV_TRACE(ovr_GetTrackerDesc);

	ovrTrackerDesc desc = { 0 };
	if (trackerDescIndex < ovr_GetTrackerCount(session))
	{
		desc.FrustumHFovInRadians = OVR::DegreeToRad(100.0f);
		desc.FrustumVFovInRadians = OVR::DegreeToRad(70.0f);
		desc.FrustumNearZInMeters = 0.4f;
		desc.FrustumFarZInMeters = 2.5;
	}
	return desc;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_Create(ovrSession* pSession, ovrGraphicsLuid* pLuid)
{
	REV_TRACE(ovr_Create);

	if (!pSession)
		return ovrError_InvalidParameter;

	*pSession = nullptr;

	// Initialize the opaque pointer with our own OpenXR-specific struct
	g_Sessions.emplace_back();
	ovrSession session = &g_Sessions.back();

	// Initialize session, it will not be fully usable until a swapchain is created
	session->InitSession(g_Instance);
	if (pLuid)
		*pLuid = session->Adapter;
	*pSession = session;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_Destroy(ovrSession session)
{
	REV_TRACE(ovr_Destroy);

	session->EndSession();

	if (!session->HookedFunctions.empty())
	{
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		for (auto it : session->HookedFunctions)
		DetourDetach(it.first, it.second);
		DetourTransactionCommit();
	}

	// Delete the session from the list of sessions
	g_Sessions.erase(std::find_if(g_Sessions.begin(), g_Sessions.end(), [session](ovrHmdStruct const& o) { return &o == session; }));
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetSessionStatus(ovrSession session, ovrSessionStatus* sessionStatus)
{
	REV_TRACE(ovr_GetSessionStatus);

	if (!session)
		return ovrError_InvalidSession;

	if (!sessionStatus)
		return ovrError_InvalidParameter;

	SessionStatusBits& status = session->SessionStatus;
	XrEventDataBuffer event = XR_TYPE(EVENT_DATA_BUFFER);
	while (xrPollEvent(session->Instance, &event) == XR_SUCCESS)
	{
		switch (event.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			const XrEventDataSessionStateChanged& stateChanged =
				reinterpret_cast<XrEventDataSessionStateChanged&>(event);
			if (stateChanged.session == session->Session)
			{
				switch (stateChanged.state)
				{
				case XR_SESSION_STATE_IDLE:
					status.HmdPresent = true;
					break;
				case XR_SESSION_STATE_READY:
					status.IsVisible = true;
					status.HmdMounted = true;
					break;
				case XR_SESSION_STATE_SYNCHRONIZED:
					status.HmdMounted = false;
					break;
				case XR_SESSION_STATE_VISIBLE:
					status.HmdMounted = true;
					status.HasInputFocus = false;
					break;
				case XR_SESSION_STATE_FOCUSED:
					status.HasInputFocus = true;
					break;
				case XR_SESSION_STATE_STOPPING:
					status.IsVisible = false;
					break;
				case XR_SESSION_STATE_LOSS_PENDING:
					status.DisplayLost = true;
					break;
				case XR_SESSION_STATE_EXITING:
					status.ShouldQuit = true;
					break;
				}
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
		{
			const XrEventDataInstanceLossPending& lossPending =
				reinterpret_cast<XrEventDataInstanceLossPending&>(event);
			status.ShouldQuit = true;
			break;
		}
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
		{
			const XrEventDataReferenceSpaceChangePending& spaceChange =
				reinterpret_cast<XrEventDataReferenceSpaceChangePending&>(event);
			if (spaceChange.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
			{
				if (spaceChange.poseValid)
					session->CalibratedOrigin = XR::Posef(session->CalibratedOrigin) * XR::Posef(spaceChange.poseInPreviousSpace);
				status.ShouldRecenter = true;
			}
			break;
		}
		}
		event = XR_TYPE(EVENT_DATA_BUFFER);
	}

	sessionStatus->IsVisible = status.IsVisible;
	sessionStatus->HmdPresent = status.HmdPresent;
	sessionStatus->HmdMounted = status.HmdMounted;
	sessionStatus->DisplayLost = status.DisplayLost;
	sessionStatus->ShouldQuit = status.ShouldQuit;
	sessionStatus->ShouldRecenter = status.ShouldRecenter;
	sessionStatus->HasInputFocus = status.HasInputFocus;
	sessionStatus->OverlayPresent = status.OverlayPresent;
#if 0 // TODO: Re-enable once we figure out why this crashes Arktika.1
	sessionStatus->DepthRequested = session->Extensions->CompositionDepth;
#endif

	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SetTrackingOriginType(ovrSession session, ovrTrackingOrigin origin)
{
	REV_TRACE(ovr_SetTrackingOriginType);

	if (!session)
		return ovrError_InvalidSession;

	session->TrackingSpace = (XrReferenceSpaceType)(XR_REFERENCE_SPACE_TYPE_LOCAL + origin);
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrTrackingOrigin) ovr_GetTrackingOriginType(ovrSession session)
{
	REV_TRACE(ovr_GetTrackingOriginType);

	if (!session)
		return ovrTrackingOrigin_EyeLevel;

	return (ovrTrackingOrigin)(session->TrackingSpace - XR_REFERENCE_SPACE_TYPE_LOCAL);
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_RecenterTrackingOrigin(ovrSession session)
{
	REV_TRACE(ovr_RecenterTrackingOrigin);

	if (!session)
		return ovrError_InvalidSession;

	XrSpaceLocation relation = XR_TYPE(SPACE_LOCATION);
	CHK_XR(xrLocateSpace(session->ViewSpace, session->LocalSpace, (*session->CurrentFrame).predictedDisplayTime, &relation));

	if (!(relation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT))
		return ovrError_InvalidHeadsetOrientation;

	return ovr_SpecifyTrackingOrigin(session, XR::Posef(relation.pose));
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SpecifyTrackingOrigin(ovrSession session, ovrPosef originPose)
{
	if (!session)
		return ovrError_InvalidSession;

	// Get a leveled head pose
	float yaw;
	OVR::Quatf(originPose.Orientation).GetYawPitchRoll(&yaw, nullptr, nullptr);
	OVR::Posef newOrigin = OVR::Posef(session->CalibratedOrigin) * OVR::Posef(OVR::Quatf(OVR::Axis_Y, yaw), originPose.Position);
	session->CalibratedOrigin = newOrigin.Normalized();

	XrSpace oldSpace = session->LocalSpace;
	XrReferenceSpaceCreateInfo spaceInfo = XR_TYPE(REFERENCE_SPACE_CREATE_INFO);
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace = XR::Posef(session->CalibratedOrigin);
	CHK_XR(xrCreateReferenceSpace(session->Session, &spaceInfo, &session->LocalSpace));
	CHK_XR(xrDestroySpace(oldSpace));

	ovr_ClearShouldRecenterFlag(session);
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_ClearShouldRecenterFlag(ovrSession session)
{
	session->SessionStatus.ShouldRecenter = false;
}

OVR_PUBLIC_FUNCTION(ovrTrackingState) ovr_GetTrackingState(ovrSession session, double absTime, ovrBool latencyMarker)
{
	REV_TRACE(ovr_GetTrackingState);

	ovrTrackingState state = { 0 };

	if (session && session->Input)
		session->Input->GetTrackingState(session, &state, absTime);

	return state;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetDevicePoses(ovrSession session, ovrTrackedDeviceType* deviceTypes, int deviceCount, double absTime, ovrPoseStatef* outDevicePoses)
{
	REV_TRACE(ovr_GetDevicePoses);

	if (!session)
		return ovrError_InvalidSession;

	return session->Input->GetDevicePoses(session, deviceTypes, deviceCount, absTime, outDevicePoses);
}

struct ovrSensorData_;
typedef struct ovrSensorData_ ovrSensorData;

OVR_PUBLIC_FUNCTION(ovrTrackingState) ovr_GetTrackingStateWithSensorData(ovrSession session, double absTime, ovrBool latencyMarker, ovrSensorData* sensorData)
{
	REV_TRACE(ovr_GetTrackingStateWithSensorData);

	// This is a private API, ignore the raw sensor data request and hope for the best.
	assert(sensorData == nullptr);

	return ovr_GetTrackingState(session, absTime, latencyMarker);
}

OVR_PUBLIC_FUNCTION(ovrTrackerPose) ovr_GetTrackerPose(ovrSession session, unsigned int trackerPoseIndex)
{
	REV_TRACE(ovr_GetTrackerPose);

	ovrTrackerPose tracker = { 0 };

	if (!session)
		return tracker;

	if (trackerPoseIndex < ovr_GetTrackerCount(session))
	{
		const OVR::Posef poses[] = {
			OVR::Posef(OVR::Quatf(OVR::Axis_Y, OVR::DegreeToRad(90.0f)), OVR::Vector3f(-2.0f, 0.0f, 0.2f)),
			OVR::Posef(OVR::Quatf(OVR::Axis_Y, OVR::DegreeToRad(0.0f)), OVR::Vector3f(-0.2f, 0.0f, -2.0f)),
			OVR::Posef(OVR::Quatf(OVR::Axis_Y, OVR::DegreeToRad(180.0f)), OVR::Vector3f(0.2f, 0.0f, 2.0f))
		};
		OVR::Posef trackerPose = poses[trackerPoseIndex];

		XrSpaceLocation relation = XR_TYPE(SPACE_LOCATION);
		if (XR_SUCCEEDED(xrLocateSpace(session->ViewSpace, session->LocalSpace, (*session->CurrentFrame).predictedDisplayTime, &relation)))
		{
			// Create a leveled head pose
			if (relation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
			{
				float yaw;
				XR::Posef headPose(relation.pose);
				headPose.Rotation.GetYawPitchRoll(&yaw, nullptr, nullptr);
				headPose.Rotation = OVR::Quatf(OVR::Axis_Y, yaw);
				trackerPose = headPose * trackerPose;
			}
		}

		tracker.Pose = trackerPose;
		tracker.LeveledPose = trackerPose;
		tracker.TrackerFlags = ovrTracker_Connected | ovrTracker_PoseTracked;
	}

	return tracker;
}

// Pre-1.7 input state
typedef struct ovrInputState1_
{
	double              TimeInSeconds;
	unsigned int        Buttons;
	unsigned int        Touches;
	float               IndexTrigger[ovrHand_Count];
	float               HandTrigger[ovrHand_Count];
	ovrVector2f         Thumbstick[ovrHand_Count];
	ovrControllerType   ControllerType;
} ovrInputState1;

// Pre-1.11 input state
typedef struct ovrInputState2_
{
	double              TimeInSeconds;
	unsigned int        Buttons;
	unsigned int        Touches;
	float               IndexTrigger[ovrHand_Count];
	float               HandTrigger[ovrHand_Count];
	ovrVector2f         Thumbstick[ovrHand_Count];
	ovrControllerType   ControllerType;
	float               IndexTriggerNoDeadzone[ovrHand_Count];
	float               HandTriggerNoDeadzone[ovrHand_Count];
	ovrVector2f         ThumbstickNoDeadzone[ovrHand_Count];
} ovrInputState2;

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetInputState(ovrSession session, ovrControllerType controllerType, ovrInputState* inputState)
{
	REV_TRACE(ovr_GetInputState);

	if (!session)
		return ovrError_InvalidSession;

	if (!inputState)
		return ovrError_InvalidParameter;

	ovrInputState state = { 0 };

	ovrResult result = ovrSuccess;
	if (session->Input && session->Session)
		result = session->Input->GetInputState(session, controllerType, &state);

	// We need to make sure we don't write outside of the bounds of the struct
	// when the client expects a pre-1.7 version of LibOVR.
	if (Runtime::Get().MinorVersion < 7)
		memcpy(inputState, &state, sizeof(ovrInputState1));
	else if (Runtime::Get().MinorVersion < 11)
		memcpy(inputState, &state, sizeof(ovrInputState2));
	else
		memcpy(inputState, &state, sizeof(ovrInputState));

	return result;
}

OVR_PUBLIC_FUNCTION(unsigned int) ovr_GetConnectedControllerTypes(ovrSession session)
{
	REV_TRACE(ovr_GetConnectedControllerTypes);

	return ovrControllerType_Touch | ovrControllerType_XBox | ovrControllerType_Remote;
}

OVR_PUBLIC_FUNCTION(ovrTouchHapticsDesc) ovr_GetTouchHapticsDesc(ovrSession session, ovrControllerType controllerType)
{
	REV_TRACE(ovr_GetTouchHapticsDesc);

	return InputManager::GetTouchHapticsDesc(controllerType);
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SetControllerVibration(ovrSession session, ovrControllerType controllerType, float frequency, float amplitude)
{
	REV_TRACE(ovr_SetControllerVibration);

	if (!session || !session->Input)
		return ovrError_InvalidSession;

	return session->Input->SetControllerVibration(session, controllerType, frequency, amplitude);
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SubmitControllerVibration(ovrSession session, ovrControllerType controllerType, const ovrHapticsBuffer* buffer)
{
	REV_TRACE(ovr_SubmitControllerVibration);

	if (!session || !session->Input)
		return ovrError_InvalidSession;

	return session->Input->SubmitControllerVibration(session, controllerType, buffer);
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetControllerVibrationState(ovrSession session, ovrControllerType controllerType, ovrHapticsPlaybackState* outState)
{
	REV_TRACE(ovr_GetControllerVibrationState);

	if (!session || !session->Input)
		return ovrError_InvalidSession;

	return session->Input->GetControllerVibrationState(session, controllerType, outState);
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_TestBoundary(ovrSession session, ovrTrackedDeviceType deviceBitmask,
	ovrBoundaryType boundaryType, ovrBoundaryTestResult* outTestResult)
{
	REV_TRACE(ovr_TestBoundary);

	outTestResult->ClosestDistance = INFINITY;

	std::vector<ovrPoseStatef> poses;
	std::vector<ovrTrackedDeviceType> devices;
	for (uint32_t i = 1; i & ovrTrackedDevice_All; i <<= 1)
	{
		if (i & deviceBitmask)
			devices.push_back((ovrTrackedDeviceType)i);
	}

	poses.resize(devices.size());
	CHK_OVR(ovr_GetDevicePoses(session, devices.data(), (uint32_t)devices.size(), 0.0f, poses.data()));

	for (size_t i = 0; i < devices.size(); i++)
	{
		ovrBoundaryTestResult result = { 0 };
		ovrResult err = ovr_TestBoundaryPoint(session, &poses[i].ThePose.Position, boundaryType, &result);
		if (OVR_SUCCESS(err) && result.ClosestDistance < outTestResult->ClosestDistance)
			*outTestResult = result;
	}
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_TestBoundaryPoint(ovrSession session, const ovrVector3f* point,
	ovrBoundaryType singleBoundaryType, ovrBoundaryTestResult* outTestResult)
{
	REV_TRACE(ovr_TestBoundaryPoint);

	ovrBoundaryTestResult result = { 0 };

	result.IsTriggering = ovrFalse;

	ovrVector3f bounds;
	CHK_OVR(ovr_GetBoundaryDimensions(session, singleBoundaryType, &bounds));

	// Clamp the point to the AABB
	OVR::Vector2f p(point->x, point->z);
	OVR::Vector2f halfExtents(bounds.x / 2.0f, bounds.z / 2.0f);
	OVR::Vector2f clamped = OVR::Vector2f::Min(OVR::Vector2f::Max(p, -halfExtents), halfExtents);

	// If the point is inside the AABB, we need to do some extra work
	if (clamped.Compare(p))
	{
		if (std::abs(p.x) > std::abs(p.y))
			clamped.x = halfExtents.x * (p.x / std::abs(p.x));
		else
			clamped.y = halfExtents.y * (p.y / std::abs(p.y));
	}

	// We don't have a ceiling, use the height from the original point
	result.ClosestPoint.x = clamped.x;
	result.ClosestPoint.y = point->y;
	result.ClosestPoint.z = clamped.y;

	// Get the normal, closest distance is the length of this normal
	OVR::Vector2f normal = p - clamped;
	result.ClosestDistance = normal.Length();

	// Normalize the normal
	normal.Normalize();
	result.ClosestPointNormal.x = normal.x;
	result.ClosestPointNormal.y = 0.0f;
	result.ClosestPointNormal.z = normal.y;

	*outTestResult = result;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SetBoundaryLookAndFeel(ovrSession session, const ovrBoundaryLookAndFeel* lookAndFeel)
{
	REV_TRACE(ovr_SetBoundaryLookAndFeel);

	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_ResetBoundaryLookAndFeel(ovrSession session)
{
	REV_TRACE(ovr_ResetBoundaryLookAndFeel);

	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetBoundaryGeometry(ovrSession session, ovrBoundaryType boundaryType, ovrVector3f* outFloorPoints, int* outFloorPointsCount)
{
	REV_TRACE(ovr_GetBoundaryGeometry);

	if (!session)
		return ovrError_InvalidSession;

	if (outFloorPoints)
	{
		ovrVector3f bounds;
		CHK_OVR(ovr_GetBoundaryDimensions(session, boundaryType, &bounds));
		for (int i = 0; i < 4; i++)
		{
			outFloorPoints[i] = (OVR::Vector3f(bounds) / 2.0f);
			if (i % 2 == 0)
				outFloorPoints[i].x *= -1.0f;
			if (i / 2 == 0)
				outFloorPoints[i].z *= -1.0f;
		}
	}
	if (outFloorPointsCount)
		*outFloorPointsCount = 4;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetBoundaryDimensions(ovrSession session, ovrBoundaryType boundaryType, ovrVector3f* outDimensions)
{
	REV_TRACE(ovr_GetBoundaryDimensions);

	if (!session)
		return ovrError_InvalidSession;

	XrExtent2Df bounds;
	CHK_XR(xrGetReferenceSpaceBoundsRect(session->Session, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds));

	outDimensions->x = bounds.width;
	outDimensions->y = 0.0f; // TODO: Find some good default height
	outDimensions->z = bounds.height;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetBoundaryVisible(ovrSession session, ovrBool* outIsVisible)
{
	REV_TRACE(ovr_GetBoundaryVisible);

	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_RequestBoundaryVisible(ovrSession session, ovrBool visible)
{
	REV_TRACE(ovr_RequestBoundaryVisible);

	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetTextureSwapChainLength(ovrSession session, ovrTextureSwapChain chain, int* out_Length)
{
	REV_TRACE(ovr_GetTextureSwapChainLength);

	if (!chain)
		return ovrError_InvalidParameter;

	MICROPROFILE_META_CPU("Identifier", (int)chain->Swapchain);
	*out_Length = chain->Length;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetTextureSwapChainCurrentIndex(ovrSession session, ovrTextureSwapChain chain, int* out_Index)
{
	REV_TRACE(ovr_GetTextureSwapChainCurrentIndex);

	if (!chain)
		return ovrError_InvalidParameter;

	MICROPROFILE_META_CPU("Identifier", (int)chain->Swapchain);
	MICROPROFILE_META_CPU("Index", chain->CurrentIndex);
	*out_Index = chain->CurrentIndex;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetTextureSwapChainDesc(ovrSession session, ovrTextureSwapChain chain, ovrTextureSwapChainDesc* out_Desc)
{
	REV_TRACE(ovr_GetTextureSwapChainDesc);

	if (!chain)
		return ovrError_InvalidParameter;

	MICROPROFILE_META_CPU("Identifier", (int)chain->Swapchain);
	*out_Desc = chain->Desc;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_CommitTextureSwapChain(ovrSession session, ovrTextureSwapChain chain)
{
	REV_TRACE(ovr_CommitTextureSwapChain);

	if (!session)
		return ovrError_InvalidSession;

	if (!chain)
		return ovrError_InvalidParameter;

	MICROPROFILE_META_CPU("Identifier", (int)chain->Swapchain);
	MICROPROFILE_META_CPU("CurrentIndex", chain->CurrentIndex);

	XrSwapchainImageReleaseInfo releaseInfo = XR_TYPE(SWAPCHAIN_IMAGE_RELEASE_INFO);
	CHK_XR(xrReleaseSwapchainImage(chain->Swapchain, &releaseInfo));

	if (!chain->Desc.StaticImage)
	{
		XrSwapchainImageAcquireInfo acquireInfo = XR_TYPE(SWAPCHAIN_IMAGE_ACQUIRE_INFO);
		CHK_XR(xrAcquireSwapchainImage(chain->Swapchain, &acquireInfo, &chain->CurrentIndex));

		{
			std::unique_lock<std::mutex> lk(session->ChainMutex);
			session->AcquiredChains.push_back(chain->Swapchain);
		}
	}

	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_DestroyTextureSwapChain(ovrSession session, ovrTextureSwapChain chain)
{
	REV_TRACE(ovr_DestroyTextureSwapChain);

	if (!chain)
		return;

	{
		std::unique_lock<std::mutex> lk(session->ChainMutex);
		session->AcquiredChains.remove(chain->Swapchain);
	}

	XrResult rs = xrDestroySwapchain(chain->Swapchain);
	assert(XR_SUCCEEDED(rs));
	delete[] chain->Images;
	delete chain;
}

OVR_PUBLIC_FUNCTION(void) ovr_DestroyMirrorTexture(ovrSession session, ovrMirrorTexture mirrorTexture)
{
	REV_TRACE(ovr_DestroyMirrorTexture);

	if (!mirrorTexture)
		return;

	ovr_DestroyTextureSwapChain(session, mirrorTexture->Dummy);
	delete mirrorTexture;
}

OVR_PUBLIC_FUNCTION(ovrSizei) ovr_GetFovTextureSize(ovrSession session, ovrEyeType eye, ovrFovPort fov, float pixelsPerDisplayPixel)
{
	REV_TRACE(ovr_GetFovTextureSize);

	// TODO: Add support for pixelsPerDisplayPixel
	ovrSizei size = {
		(int)(session->PixelsPerTan[eye].x * (fov.LeftTan + fov.RightTan)),
		(int)(session->PixelsPerTan[eye].y * (fov.UpTan + fov.DownTan)),
	};
	return size;
}

OVR_PUBLIC_FUNCTION(ovrEyeRenderDesc) ovr_GetRenderDesc2(ovrSession session, ovrEyeType eyeType, ovrFovPort fov)
{
	REV_TRACE(ovr_GetRenderDesc);

	if (!session)
		return ovrEyeRenderDesc();

	ovrEyeRenderDesc desc = { eyeType, fov };

	for (int i = 0; i < eyeType; i++)
		desc.DistortedViewport.Pos.x += session->ViewConfigs[i].recommendedImageRectWidth;

	desc.DistortedViewport.Size.w = session->ViewConfigs[eyeType].recommendedImageRectWidth;
	desc.DistortedViewport.Size.h = session->ViewConfigs[eyeType].recommendedImageRectHeight;
	desc.PixelsPerTanAngleAtCenter = session->PixelsPerTan[eyeType];

	XrView views[ovrEye_Count] = { XR_TYPE(VIEW), XR_TYPE(VIEW) };
	session->LocateViews(views);
	desc.HmdToEyePose = XR::Posef(views[eyeType].pose);
	return desc;
}

typedef struct OVR_ALIGNAS(4) ovrEyeRenderDesc1_ {
	ovrEyeType Eye;
	ovrFovPort Fov;
	ovrRecti DistortedViewport;
	ovrVector2f PixelsPerTanAngleAtCenter;
	ovrVector3f HmdToEyeOffset;
} ovrEyeRenderDesc1;

OVR_PUBLIC_FUNCTION(ovrEyeRenderDesc1) ovr_GetRenderDesc(ovrSession session, ovrEyeType eyeType, ovrFovPort fov)
{
	ovrEyeRenderDesc1 legacy = {};
	ovrEyeRenderDesc desc = ovr_GetRenderDesc2(session, eyeType, fov);
	memcpy(&legacy, &desc, sizeof(ovrEyeRenderDesc1));
	legacy.HmdToEyeOffset = desc.HmdToEyePose.Position;
	return legacy;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_WaitToBeginFrame(ovrSession session, long long frameIndex)
{
	REV_TRACE(ovr_WaitToBeginFrame);
	MICROPROFILE_META_CPU("Wait Frame", (int)frameIndex);

	if (!session)
		return ovrError_InvalidSession;

	XrIndexedFrameState* frameState = session->CurrentFrame + 1;
	if (frameState > &session->FrameStats[ovrMaxProvidedFrameStats - 1])
		frameState = session->FrameStats;

	XrFrameWaitInfo waitInfo = XR_TYPE(FRAME_WAIT_INFO);
	CHK_XR(xrWaitFrame(session->Session, &waitInfo, frameState));
	frameState->frameIndex = frameIndex + 1;
	session->CurrentFrame = frameState;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_BeginFrame(ovrSession session, long long frameIndex)
{
	REV_TRACE(ovr_BeginFrame);
	MICROPROFILE_META_CPU("Begin Frame", (int)frameIndex);

	if (!session)
		return ovrError_InvalidSession;

	// Wait on all outstanding surfaces
	XrSwapchainImageWaitInfo chainWaitInfo = XR_TYPE(SWAPCHAIN_IMAGE_WAIT_INFO);
	chainWaitInfo.timeout = XR_NO_DURATION;
	{
		std::unique_lock<std::mutex> lk(session->ChainMutex);
		while (!session->AcquiredChains.empty())
		{
			CHK_XR(xrWaitSwapchainImage(session->AcquiredChains.front(), &chainWaitInfo));
			session->AcquiredChains.pop_front();
		}
	}

	XrFrameBeginInfo beginInfo = XR_TYPE(FRAME_BEGIN_INFO);
	CHK_XR(xrBeginFrame(session->Session, &beginInfo));
	return ovrSuccess;
}

union XrCompositionLayerUnion
{
	XrCompositionLayerBaseHeader Header;
	XrCompositionLayerProjection Projection;
	XrCompositionLayerQuad Quad;
	XrCompositionLayerCylinderKHR Cylinder;
	XrCompositionLayerCubeKHR Cube;
};

struct XrCompositionLayerProjectionViewStereo
{
	XrCompositionLayerProjectionView Views[ovrEye_Count];
};

OVR_PUBLIC_FUNCTION(ovrResult) ovr_EndFrame(ovrSession session, long long frameIndex, const ovrViewScaleDesc* viewScaleDesc,
	ovrLayerHeader const * const * layerPtrList, unsigned int layerCount)
{
	REV_TRACE(ovr_EndFrame);
	MICROPROFILE_META_CPU("End Frame", (int)frameIndex);

	if (!session)
		return ovrError_InvalidSession;

	std::vector<XrCompositionLayerBaseHeader*> layers;
	std::list<XrCompositionLayerUnion> layerData;
	std::list<XrCompositionLayerProjectionViewStereo> viewData;
	std::list<XrCompositionLayerDepthInfoKHR> depthData;
	for (unsigned int i = 0; i < layerCount; i++)
	{
		ovrLayer_Union* layer = (ovrLayer_Union*)layerPtrList[i];

		if (!layer)
			continue;

		ovrLayerType type = layer->Header.Type;
		const bool upsideDown = layer->Header.Flags & ovrLayerFlag_TextureOriginAtBottomLeft;
		const bool headLocked = layer->Header.Flags & ovrLayerFlag_HeadLocked;

		// Version 1.25 introduced a 128-byte reserved parameter, so on older versions the actual data
		// falls within this reserved parameter and we need to move the pointer back into the actual data area.
		// NOTE: Do not read the header after this operation as it will fall outside of the layer memory.
		if (Runtime::Get().MinorVersion < 25)
			layer = (ovrLayer_Union*)((char*)layer - sizeof(ovrLayerHeader::Reserved));

		// The oculus runtime is very tolerant of invalid viewports, so this lambda ensures we submit valid ones.
		auto ClampRect = [](ovrRecti rect, ovrTextureSwapChain chain)
		{
			OVR::Sizei chainSize(chain->Desc.Width, chain->Desc.Height);

			if (rect.Size.w <= 0 || rect.Size.h <= 0)
				return XR::Recti(OVR::Vector2i::Max(rect.Pos, OVR::Vector2i()), chainSize);

			return XR::Recti(OVR::Vector2i::Max(rect.Pos, OVR::Vector2i()),
				OVR::Sizei::Min(rect.Size, chainSize));
		};

		layerData.emplace_back();
		XrCompositionLayerUnion& newLayer = layerData.back();

		if (type == ovrLayerType_EyeFov || type == ovrLayerType_EyeMatrix || type == ovrLayerType_EyeFovDepth)
		{
			XrCompositionLayerProjection& projection = newLayer.Projection;
			projection = XR_TYPE(COMPOSITION_LAYER_PROJECTION);

			ovrTextureSwapChain texture = nullptr;
			viewData.emplace_back();
			int i;
			for (i = 0; i < ovrEye_Count; i++)
			{
				if (layer->EyeFov.ColorTexture[i])
					texture = layer->EyeFov.ColorTexture[i];

				if (!texture)
					break;

				XrCompositionLayerProjectionView& view = viewData.back().Views[i];
				view = XR_TYPE(COMPOSITION_LAYER_PROJECTION_VIEW);

				if (type == ovrLayerType_EyeMatrix)
				{
					// RenderPose is the first member that's differently aligned
					view.pose = XR::Posef(layer->EyeMatrix.RenderPose[i]);
					view.fov = XR::Matrix4f(layer->EyeMatrix.Matrix[i]);
				}
				else
				{
					view.pose = XR::Posef(layer->EyeFov.RenderPose[i]);

					// The Climb specifies an invalid fov in the first frame, ignore the layer
					XR::FovPort Fov(layer->EyeFov.Fov[i]);
					if (Fov.GetMaxSideTan() > 0.0f)
						view.fov = Fov;
					else
						break;
				}

				// Flip the field-of-view to flip the image, invert the check for OpenGL
				if (texture->Images->type == XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR ? !upsideDown : upsideDown)
					OVR::OVRMath_Swap(view.fov.angleUp, view.fov.angleDown);

				if (type == ovrLayerType_EyeFovDepth && Runtime::Get().CompositionDepth)
				{
					depthData.emplace_back();
					XrCompositionLayerDepthInfoKHR& depthInfo = depthData.back();
					depthInfo = XR_TYPE(COMPOSITION_LAYER_DEPTH_INFO_KHR);

					ovrTextureSwapChain depthTexture = layer->EyeFovDepth.DepthTexture[i];
					depthInfo.subImage.swapchain = depthTexture->Swapchain;
					depthInfo.subImage.imageRect = ClampRect(layer->EyeFovDepth.Viewport[i], depthTexture);
					depthInfo.subImage.imageArrayIndex = 0;

					const ovrTimewarpProjectionDesc& projDesc = layer->EyeFovDepth.ProjectionDesc;
					depthInfo.minDepth = 0.0f;
					depthInfo.maxDepth = 1.0f;
					depthInfo.nearZ = projDesc.Projection23 / projDesc.Projection22;
					depthInfo.farZ = projDesc.Projection23 / (1.0f + projDesc.Projection22);

					if (viewScaleDesc)
					{
						depthInfo.nearZ *= viewScaleDesc->HmdSpaceToWorldScaleInMeters;
						depthInfo.farZ *= viewScaleDesc->HmdSpaceToWorldScaleInMeters;
					}

					view.next = &depthData.back();
				}

				view.subImage.swapchain = texture->Swapchain;
				view.subImage.imageRect = ClampRect(layer->EyeFov.Viewport[i], texture);
				view.subImage.imageArrayIndex = 0;
			}

			// Verify all views were initialized without errors, otherwise ignore the layer
			if (i < ovrEye_Count)
				continue;

			projection.viewCount = ovrEye_Count;
			projection.views = reinterpret_cast<XrCompositionLayerProjectionView*>(&viewData.back());
		}
		else if (type == ovrLayerType_Quad)
		{
			ovrTextureSwapChain texture = layer->Quad.ColorTexture;
			if (!texture)
				continue;

			XrCompositionLayerQuad& quad = newLayer.Quad;
			quad = XR_TYPE(COMPOSITION_LAYER_QUAD);
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = texture->Swapchain;
			quad.subImage.imageRect = ClampRect(layer->Quad.Viewport, texture);
			quad.subImage.imageArrayIndex = 0;
			quad.pose = XR::Posef(layer->Quad.QuadPoseCenter);
			quad.size = XR::Vector2f(layer->Quad.QuadSize);
		}
		else if (type == ovrLayerType_Cylinder && Runtime::Get().CompositionCylinder)
		{
			ovrTextureSwapChain texture = layer->Cylinder.ColorTexture;
			if (!texture)
				continue;

			XrCompositionLayerCylinderKHR& cylinder = newLayer.Cylinder;
			cylinder = XR_TYPE(COMPOSITION_LAYER_CYLINDER_KHR);
			cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			cylinder.subImage.swapchain = texture->Swapchain;
			cylinder.subImage.imageRect = ClampRect(layer->Cylinder.Viewport, texture);
			cylinder.subImage.imageArrayIndex = 0;
			cylinder.pose = XR::Posef(layer->Cylinder.CylinderPoseCenter);
			cylinder.radius = layer->Cylinder.CylinderRadius;
			cylinder.centralAngle = layer->Cylinder.CylinderAngle;
			cylinder.aspectRatio = layer->Cylinder.CylinderAspectRatio;
		}
		else if (type == ovrLayerType_Cube && Runtime::Get().CompositionCube)
		{
			if (!layer->Cube.CubeMapTexture)
				continue;

			XrCompositionLayerCubeKHR& cube = newLayer.Cube;
			cube = XR_TYPE(COMPOSITION_LAYER_CUBE_KHR);
			cube.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			cube.swapchain = layer->Cube.CubeMapTexture->Swapchain;
			cube.imageArrayIndex = 0;
			cube.orientation = XR::Quatf(layer->Cube.Orientation);
		}
		else
		{
			// Layer type not recognized or disabled, ignore the layer
			assert(type == ovrLayerType_Disabled);
			continue;
		}

		XrCompositionLayerBaseHeader& header = newLayer.Header;
		header.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		if (headLocked)
			header.space = session->ViewSpace;
		else
			header.space = (session->TrackingSpace == XR_REFERENCE_SPACE_TYPE_STAGE) ?
				session->StageSpace : session->LocalSpace;

		layers.push_back(&newLayer.Header);
	}

	XrFrameEndInfo endInfo = XR_TYPE(FRAME_END_INFO);
	endInfo.displayTime = (*session->CurrentFrame).predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = (uint32_t)layers.size();
	endInfo.layers = layers.data();
	CHK_XR(xrEndFrame(session->Session, &endInfo));

	MicroProfileFlip();

	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SubmitFrame2(ovrSession session, long long frameIndex, const ovrViewScaleDesc* viewScaleDesc,
	ovrLayerHeader const * const * layerPtrList, unsigned int layerCount)
{
	REV_TRACE(ovr_SubmitFrame);
	MICROPROFILE_META_CPU("Submit Frame", (int)frameIndex);

	if (!session)
		return ovrError_InvalidSession;

	if (frameIndex <= 0)
		frameIndex = (*session->CurrentFrame).frameIndex;

	CHK_OVR(ovr_EndFrame(session, frameIndex, viewScaleDesc, layerPtrList, layerCount));
	CHK_OVR(ovr_WaitToBeginFrame(session, frameIndex + 1));
	CHK_OVR(ovr_BeginFrame(session, frameIndex + 1));
	return ovrSuccess;
}

typedef struct OVR_ALIGNAS(4) ovrViewScaleDesc1_ {
	ovrVector3f HmdToEyeOffset[ovrEye_Count]; ///< Translation of each eye.
	float HmdSpaceToWorldScaleInMeters; ///< Ratio of viewer units to meter units.
} ovrViewScaleDesc1;

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SubmitFrame(ovrSession session, long long frameIndex, const ovrViewScaleDesc1* viewScaleDesc,
	ovrLayerHeader const * const * layerPtrList, unsigned int layerCount)
{
	// TODO: We don't ever use viewScaleDesc so no need to do any conversion.
	return ovr_SubmitFrame2(session, frameIndex, nullptr, layerPtrList, layerCount);
}

typedef struct OVR_ALIGNAS(4) ovrPerfStatsPerCompositorFrame1_
{
	int     HmdVsyncIndex;
	int     AppFrameIndex;
	int     AppDroppedFrameCount;
	float   AppMotionToPhotonLatency;
	float   AppQueueAheadTime;
	float   AppCpuElapsedTime;
	float   AppGpuElapsedTime;
	int     CompositorFrameIndex;
	int     CompositorDroppedFrameCount;
	float   CompositorLatency;
	float   CompositorCpuElapsedTime;
	float   CompositorGpuElapsedTime;
	float   CompositorCpuStartToGpuEndElapsedTime;
	float   CompositorGpuEndToVsyncElapsedTime;
} ovrPerfStatsPerCompositorFrame1;

typedef struct OVR_ALIGNAS(4) ovrPerfStats1_
{
	ovrPerfStatsPerCompositorFrame1  FrameStats[ovrMaxProvidedFrameStats];
	int                             FrameStatsCount;
	ovrBool                         AnyFrameStatsDropped;
	float                           AdaptiveGpuPerformanceScale;
} ovrPerfStats1;

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetPerfStats(ovrSession session, ovrPerfStats* outStats)
{
	REV_TRACE(ovr_GetPerfStats);

	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_ResetPerfStats(ovrSession session)
{
	REV_TRACE(ovr_ResetPerfStats);

	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(double) ovr_GetPredictedDisplayTime(ovrSession session, long long frameIndex)
{
	REV_TRACE(ovr_GetPredictedDisplayTime);
	XR_FUNCTION(session->Instance, ConvertTimeToWin32PerformanceCounterKHR);

	MICROPROFILE_META_CPU("Predict Frame", (int)frameIndex);

	XrIndexedFrameState* CurrentFrame = session->CurrentFrame;
	XrTime displayTime = CurrentFrame->predictedDisplayTime;

	if (frameIndex > 0)
		displayTime += CurrentFrame->predictedDisplayPeriod * (CurrentFrame->frameIndex - frameIndex);

	static double PerfFrequencyInverse = 0.0;
	if (PerfFrequencyInverse == 0.0)
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		PerfFrequencyInverse = 1.0 / (double)freq.QuadPart;
	}

	LARGE_INTEGER li;
	if (XR_FAILED(ConvertTimeToWin32PerformanceCounterKHR(session->Instance, displayTime, &li)))
		return 0.0;

	return li.QuadPart * PerfFrequencyInverse;
}

OVR_PUBLIC_FUNCTION(double) ovr_GetTimeInSeconds()
{
	REV_TRACE(ovr_GetTimeInSeconds);

	static double PerfFrequencyInverse = 0.0;
	if (PerfFrequencyInverse == 0.0)
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		PerfFrequencyInverse = 1.0 / (double)freq.QuadPart;
	}

	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	return li.QuadPart * PerfFrequencyInverse;
}

OVR_PUBLIC_FUNCTION(ovrBool) ovr_GetBool(ovrSession session, const char* propertyName, ovrBool defaultVal)
{
	REV_TRACE(ovr_GetBool);

	return defaultVal;
}

OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetBool(ovrSession session, const char* propertyName, ovrBool value)
{
	REV_TRACE(ovr_SetBool);

	// TODO: Should we handle QueueAheadEnabled with always-on reprojection?
	return false;
}

OVR_PUBLIC_FUNCTION(int) ovr_GetInt(ovrSession session, const char* propertyName, int defaultVal)
{
	REV_TRACE(ovr_GetInt);

	if (strcmp("TextureSwapChainDepth", propertyName) == 0)
		return REV_DEFAULT_SWAPCHAIN_DEPTH;

	return defaultVal;
}

OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetInt(ovrSession session, const char* propertyName, int value)
{
	REV_TRACE(ovr_SetInt);

	return false;
}

OVR_PUBLIC_FUNCTION(float) ovr_GetFloat(ovrSession session, const char* propertyName, float defaultVal)
{
	REV_TRACE(ovr_GetFloat);

	if (session)
	{
		if (strcmp(propertyName, "IPD") == 0)
		{
			// Locate the eyes in view space to compute the IPD
			XrView views[ovrEye_Count] = { XR_TYPE(VIEW), XR_TYPE(VIEW) };
			if (OVR_FAILURE(session->LocateViews(views)))
				return 0.0f;

			return XR::Vector3f(views[ovrEye_Left].pose.position).Distance(
				XR::Vector3f(views[ovrEye_Right].pose.position)
			);
		}

		if (strcmp(propertyName, "VsyncToNextVsync") == 0)
			return (*session->CurrentFrame).predictedDisplayPeriod / 1e9f;
	}

	// Override defaults, we should always return a valid value for these
	if (strcmp(propertyName, OVR_KEY_PLAYER_HEIGHT) == 0)
		defaultVal = OVR_DEFAULT_PLAYER_HEIGHT;
	else if (strcmp(propertyName, OVR_KEY_EYE_HEIGHT) == 0)
		defaultVal = OVR_DEFAULT_EYE_HEIGHT;

	return defaultVal;
}

OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetFloat(ovrSession session, const char* propertyName, float value)
{
	REV_TRACE(ovr_SetFloat);

	return false;
}

OVR_PUBLIC_FUNCTION(unsigned int) ovr_GetFloatArray(ovrSession session, const char* propertyName, float values[], unsigned int valuesCapacity)
{
	REV_TRACE(ovr_GetFloatArray);

	if (strcmp(propertyName, OVR_KEY_NECK_TO_EYE_DISTANCE) == 0)
	{
		if (valuesCapacity < 2)
			return 0;

		values[0] = OVR_DEFAULT_NECK_TO_EYE_HORIZONTAL;
		values[1] = OVR_DEFAULT_NECK_TO_EYE_VERTICAL;
		return 2;
	}

	return 0;
}

OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetFloatArray(ovrSession session, const char* propertyName, const float values[], unsigned int valuesSize)
{
	REV_TRACE(ovr_SetFloatArray);

	return false;
}

OVR_PUBLIC_FUNCTION(const char*) ovr_GetString(ovrSession session, const char* propertyName, const char* defaultVal)
{
	REV_TRACE(ovr_GetString);

	if (!session)
		return defaultVal;

	// Override defaults, we should always return a valid value for these
	if (strcmp(propertyName, OVR_KEY_GENDER) == 0)
		defaultVal = OVR_DEFAULT_GENDER;

	return defaultVal;
}

OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetString(ovrSession session, const char* propertyName, const char* value)
{
	REV_TRACE(ovr_SetString);

	return false;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_Lookup(const char* name, void** data)
{
	// We don't communicate with the ovrServer.
	return ovrError_ServiceError;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetExternalCameras(ovrSession session, ovrExternalCamera* cameras, unsigned int* inoutCameraCount)
{
	// TODO: Support externalcamera.cfg used by the SteamVR Unity plugin
	return ovrError_NoExternalCameraInfo;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SetExternalCameraProperties(ovrSession session, const char* name, const ovrCameraIntrinsics* const intrinsics, const ovrCameraExtrinsics* const extrinsics)
{
	return ovrError_NoExternalCameraInfo;
}

OVR_PUBLIC_FUNCTION(unsigned int) ovr_GetEnabledCaps(ovrSession session)
{
	return 0;
}

OVR_PUBLIC_FUNCTION(void) ovr_SetEnabledCaps(ovrSession session, unsigned int hmdCaps)
{
}

OVR_PUBLIC_FUNCTION(unsigned int) ovr_GetTrackingCaps(ovrSession session)
{
	return 0;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_ConfigureTracking(
	ovrSession session,
	unsigned int requestedTrackingCaps,
	unsigned int requiredTrackingCaps)
{
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_IsExtensionSupported(
	ovrSession session,
	ovrExtensions extension,
	ovrBool* outExtensionSupported)
{
	if (!outExtensionSupported)
		return ovrError_InvalidParameter;

	// TODO: Extensions support
	*outExtensionSupported = false;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_EnableExtension(ovrSession session, ovrExtensions extension)
{
	// TODO: Extensions support
	return ovrError_InvalidOperation;
}

typedef struct ovrViewportStencilDesc_ {
	ovrFovStencilType StencilType;
	ovrEyeType Eye;
	ovrFovPort FovPort; /// Typically Fov obtained from ovrEyeRenderDesc
	ovrQuatf HmdToEyeRotation; /// Typically HmdToEyePose.Orientation obtained from ovrEyeRenderDesc
} ovrViewportStencilDesc;

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetViewportStencil(
	ovrSession session,
	const ovrViewportStencilDesc* viewportStencilDesc,
	ovrFovStencilMeshBuffer* outMeshBuffer)
{
	ovrFovStencilDesc fovStencilDesc = {};
	fovStencilDesc.StencilType = viewportStencilDesc->StencilType;
	fovStencilDesc.StencilFlags = 0;
	fovStencilDesc.Eye = viewportStencilDesc->Eye;
	fovStencilDesc.FovPort = viewportStencilDesc->FovPort;
	fovStencilDesc.HmdToEyeRotation = viewportStencilDesc->HmdToEyeRotation;
	return ovr_GetFovStencil(session, &fovStencilDesc, outMeshBuffer);
}

static const XrVector2f VisibleRectangle[] = {
	{ 0.0f, 0.0f },
	{ 1.0f, 0.0f },
	{ 1.0f, 1.0f },
	{ 0.0f, 1.0f }
};

static const uint16_t VisibleRectangleIndices[] = {
	0, 1, 2, 0, 2, 3
};

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetFovStencil(
	ovrSession session,
	const ovrFovStencilDesc* fovStencilDesc,
	ovrFovStencilMeshBuffer* meshBuffer)
{
	if (!Runtime::Get().VisibilityMask)
		return ovrError_Unsupported;

	if (!session)
		return ovrError_InvalidSession;

	XR_FUNCTION(session->Instance, GetVisibilityMaskKHR);

	if (fovStencilDesc->StencilType == ovrFovStencil_VisibleRectangle)
	{
		meshBuffer->UsedVertexCount = sizeof(VisibleRectangle) / sizeof(XrVector2f);
		meshBuffer->UsedIndexCount = sizeof(VisibleRectangleIndices) / sizeof(uint16_t);

		if (meshBuffer->AllocVertexCount >= meshBuffer->UsedVertexCount)
			memcpy(meshBuffer->VertexBuffer, VisibleRectangle, sizeof(VisibleRectangle));
		if (meshBuffer->AllocIndexCount >= meshBuffer->UsedIndexCount)
			memcpy(meshBuffer->IndexBuffer, VisibleRectangleIndices, sizeof(VisibleRectangleIndices));
		return ovrSuccess;
	}

	std::vector<uint32_t> indexBuffer;
	if (meshBuffer->AllocIndexCount > 0)
		indexBuffer.resize(meshBuffer->AllocIndexCount);

	XrVisibilityMaskTypeKHR type = (XrVisibilityMaskTypeKHR)(fovStencilDesc->StencilType + 1);
	XrVisibilityMaskKHR mask = XR_TYPE(VISIBILITY_MASK_KHR);
	mask.vertexCapacityInput = meshBuffer->AllocVertexCount;
	mask.vertices = (XrVector2f*)meshBuffer->VertexBuffer;
	mask.indexCapacityInput = meshBuffer->AllocIndexCount;
	mask.indices = meshBuffer->IndexBuffer ? indexBuffer.data() : nullptr;
	CHK_XR(GetVisibilityMaskKHR(session->Session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, fovStencilDesc->Eye, type, &mask));
	meshBuffer->UsedVertexCount = mask.vertexCountOutput;
	meshBuffer->UsedIndexCount = mask.indexCountOutput;

	if (meshBuffer->VertexBuffer && !(fovStencilDesc->StencilFlags & ovrFovStencilFlag_MeshOriginAtBottomLeft))
	{
		for (int i = 0; i < meshBuffer->AllocVertexCount; i++)
			meshBuffer->VertexBuffer[i].y = 1.0f - meshBuffer->VertexBuffer[i].y;
	}

	if (meshBuffer->IndexBuffer)
	{
		for (int i = 0; i < meshBuffer->AllocIndexCount; i++)
			meshBuffer->IndexBuffer[i] = (uint16_t)indexBuffer[i];
	}

	return ovrSuccess;
}

struct ovrDesktopWindowDesc_;
typedef struct ovrDesktopWindowDesc_ ovrDesktopWindowDesc;

struct ovrHybridInputFocusState_;
typedef struct ovrHybridInputFocusState_ ovrHybridInputFocusState;

typedef uint32_t ovrDesktopWindowHandle;

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitDesktopWindow(
	ovrSession session,
	ovrDesktopWindowHandle* outWindowHandle)
{
	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_ShowDesktopWindow(
	ovrSession session,
	const ovrDesktopWindowDesc* windowDesc)
{
	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_HideDesktopWindow(
	ovrSession session,
	ovrDesktopWindowHandle windowHandle)
{
	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetHybridInputFocus(
	ovrSession session,
	ovrControllerType controllerType,
	ovrHybridInputFocusState* outState)
{
	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_ShowAvatarHands(
	ovrSession session,
	ovrBool showHands)
{
	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_ShowKeyboard()
{
	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_EnableHybridRaycast()
{
	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_QueryDistortion()
{
	return ovrError_Unsupported;
}

OVR_PUBLIC_FUNCTION(ovrHmdColorDesc)
ovr_GetHmdColorDesc(ovrSession session)
{
	ovrHmdColorDesc desc = { ovrColorSpace_Unknown };
	return desc;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_SetClientColorDesc(ovrSession session, const ovrHmdColorDesc* colorDesc)
{
	return ovrError_Unsupported;
}
