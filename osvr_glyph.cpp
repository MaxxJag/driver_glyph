//============ Copyright (c) Valve Corporation, All rights reserved. ============

#include <openvr_driver.h>
#include "driverlog.h"

#include <cmath>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>
#include <regex>

#if defined( _WINDOWS )
#define DIRECTINPUT_VERSION 0x800
#include <windows.h>
#include <SetupAPI.h>
#include <dinput.h>
#endif

using namespace vr;
using namespace std;

#if defined(_WIN32)
#define HMD_DLL_EXPORT extern "C" __declspec( dllexport )
#define HMD_DLL_IMPORT extern "C" __declspec( dllimport )
#elif defined(__GNUC__) || defined(COMPILER_GCC) || defined(__APPLE__)
#define HMD_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#define HMD_DLL_IMPORT extern "C" 
#else
#error "Unsupported Platform."
#endif

inline double deg2rad(double deg) {
	static const double pi_on_180 = 4.0 * atan(1.0) / 180.0;
	return deg * pi_on_180;
}

inline HmdQuaternion_t HmdQuaternion_Init(double w, double x, double y, double z)
{
	HmdQuaternion_t quat;
	quat.w = w;
	quat.x = x;
	quat.y = y;
	quat.z = z;
	return quat;
}

inline void HmdMatrix_SetIdentity(HmdMatrix34_t *pMatrix)
{
	pMatrix->m[0][0] = 1.f;
	pMatrix->m[0][1] = 0.f;
	pMatrix->m[0][2] = 0.f;
	pMatrix->m[0][3] = 0.f;
	pMatrix->m[1][0] = 0.f;
	pMatrix->m[1][1] = 1.f;
	pMatrix->m[1][2] = 0.f;
	pMatrix->m[1][3] = 0.f;
	pMatrix->m[2][0] = 0.f;
	pMatrix->m[2][1] = 0.f;
	pMatrix->m[2][2] = 1.f;
	pMatrix->m[2][3] = 0.f;
}

inline HmdQuaternion_t HmdQuaternion_Rotate(double xAngle, double yAngle, double zAngle)
{
	double x = 1.0 * sin(deg2rad(xAngle) / 2);
	double y = 0.0 * sin(deg2rad(xAngle) / 2);
	double z = 0.0 * sin(deg2rad(xAngle) / 2);
	double w = cos(deg2rad(xAngle) / 2);

	x += 0.0 * sin(deg2rad(yAngle) / 2);
	y += 1.0 * sin(deg2rad(yAngle) / 2);
	z += 0.0 * sin(deg2rad(yAngle) / 2);
	w += cos(deg2rad(yAngle) / 2);

	x += 0.0 * sin(deg2rad(zAngle) / 2);
	y += 0.0 * sin(deg2rad(zAngle) / 2);
	z += 1.0 * sin(deg2rad(zAngle) / 2);
	w += cos(deg2rad(zAngle) / 2);

	double mag = sqrt(w*w + x*x + y*y + z*z);

	return { w / mag, x / mag, y / mag, z / mag };
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

class CWatchdogDriver_Glyph : public IVRWatchdogProvider
{
public:
	CWatchdogDriver_Glyph()
	{
		m_pWatchdogThread = nullptr;
	}

	virtual EVRInitError Init(vr::IVRDriverContext *pDriverContext);
	virtual void Cleanup();

private:
	std::thread *m_pWatchdogThread;
};

CWatchdogDriver_Glyph g_watchdogDriverNull;


bool g_bExiting = false;

void WatchdogThreadFunction()
{
	while (!g_bExiting)
	{
#if defined( _WINDOWS )
		// on windows send the event when the Y key is pressed.
		if ((0x01 & GetAsyncKeyState('Y')) != 0)
		{
			// Y key was pressed. 
			//vr::VRWatchdogHost()->WatchdogWakeUp();
		}
		std::this_thread::sleep_for(std::chrono::microseconds(500));
#else
		// for the other platforms, just send one every five seconds
		std::this_thread::sleep_for(std::chrono::seconds(5));
		vr::VRWatchdogHost()->WatchdogWakeUp();
#endif
	}
}

EVRInitError CWatchdogDriver_Glyph::Init(vr::IVRDriverContext *pDriverContext)
{
	VR_INIT_WATCHDOG_DRIVER_CONTEXT(pDriverContext);
	InitDriverLog(vr::VRDriverLog());

	// Watchdog mode on Windows starts a thread that listens for the 'Y' key on the keyboard to 
	// be pressed. A real driver should wait for a system button event or something else from the 
	// the hardware that signals that the VR system should start up.
	g_bExiting = false;
	/*m_pWatchdogThread = new std::thread(WatchdogThreadFunction);
	if (!m_pWatchdogThread)
	{
		DriverLog("Unable to create watchdog thread\n");
		return VRInitError_Driver_Failed;
	}*/

	return VRInitError_None;
}


void CWatchdogDriver_Glyph::Cleanup()
{
	g_bExiting = true;
	if (m_pWatchdogThread)
	{
		m_pWatchdogThread->join();
		delete m_pWatchdogThread;
		m_pWatchdogThread = nullptr;
	}

	CleanupDriverLog();
}

BOOL g_deviceIsActive = FALSE;

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
class CGlyphDeviceDriver : public ITrackedDeviceServerDriver, public IVRDisplayComponent
{
private:
	LPDIRECTINPUT8	lpdi;
	LPDIRECTINPUTDEVICE8  lpdiJoystick;
	DIJOYSTATE2 joyState;
	thread *gamepadPollingThread;
	BOOL useSBS = false;
public:
	CGlyphDeviceDriver()
	{
		lpdiJoystick = NULL;
		joyState = { 0 };
		m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
		m_ulPropertyContainer = vr::k_ulInvalidPropertyContainer;

		m_flIPD = vr::VRSettings()->GetFloat(k_pch_SteamVR_Section, k_pch_SteamVR_IPD_Float);
		useSBS = vr::VRSettings()->GetBool("driver_glyph", "useSBS");

		m_sSerialNumber = "Glyph001";
		m_sModelNumber = "Avegant Glyph";

		m_nWindowX = 0;
		m_nWindowY = 0;
		m_nWindowWidth = m_nRenderWidth = 1280;
		m_nWindowHeight = m_nRenderHeight = 720;
		m_flSecondsFromVsyncToPhotons = 0.0f;
		m_flDisplayFrequency = 60.0f;

		DISPLAY_DEVICEA *device = (DISPLAY_DEVICEA *)calloc(1, sizeof(DISPLAY_DEVICEA));

		device->cb = sizeof(DISPLAY_DEVICEA);
		int deviceIndex = 0;
		bool found = false;
		while (EnumDisplayDevicesA(NULL, deviceIndex, device, 0) != 0) {
			DriverLog("Device String: %s\n", device->DeviceString);
			DriverLog("Device Name: %s\n", device->DeviceName);

			char DeviceName[32];
			int displayIndex = 0;

			strcpy_s(DeviceName, 32, device->DeviceName);
			while (EnumDisplayDevicesA(DeviceName, displayIndex, device, 0) != 0) {
				DriverLog("Display ID: %s\n", device->DeviceID);
				DriverLog("Display Name: %s\n\n", device->DeviceName);

				if (!strncmp("MONITOR\\AVG0065", device->DeviceID, 15)) {
					DriverLog("Display Device Found\n");
					DEVMODEA deviceSettings = { 0 };
					deviceSettings.dmSize = sizeof(DEVMODEA);

					DriverLog("Getting Display Settings\n");
					if (EnumDisplaySettingsExA(DeviceName, ENUM_CURRENT_SETTINGS, &deviceSettings, 0) != 0) {
						POINTL point = deviceSettings.dmPosition;
						POINT mPoint = { point.x, point.y };
						HMONITOR monitor = MonitorFromPoint(mPoint, MONITOR_DEFAULTTONEAREST);
						MONITORINFOEXA *monInfo = (MONITORINFOEXA *)calloc(1, sizeof(MONITORINFOEXA));
						monInfo->cbSize = sizeof(MONITORINFOEXA);

						GetMonitorInfoA(monitor, monInfo);

						DriverLog("Monitor Rect: %ld, %ld, %ld, %ld\n", monInfo->rcMonitor.left, monInfo->rcMonitor.right, monInfo->rcMonitor.top, monInfo->rcMonitor.bottom);

						DriverLog("Display BPP: %d\n", deviceSettings.dmBitsPerPel);
						DriverLog("Display Width: %d\n", deviceSettings.dmPelsWidth);
						DriverLog("Display Height: %d\n", deviceSettings.dmPelsHeight);
						DriverLog("Display Position: %d, %d\n", point.x, point.y);
						DriverLog("Display Frequency: %d\n", deviceSettings.dmDisplayFrequency);

						m_flDisplayFrequency = (float)deviceSettings.dmDisplayFrequency;
						m_nWindowX = point.x;
						m_nWindowY = point.y;
						m_nWindowWidth = deviceSettings.dmPelsWidth;
						m_nWindowHeight = deviceSettings.dmPelsHeight;
						m_nRenderWidth = deviceSettings.dmPelsWidth;
						m_nRenderHeight = deviceSettings.dmPelsHeight;

						DriverLog("Serial Number: %s\n", m_sSerialNumber.c_str());
						DriverLog("Model Number: %s\n", m_sModelNumber.c_str());
						DriverLog("Window: %d %d %d %d\n", m_nWindowX, m_nWindowY, m_nWindowWidth, m_nWindowHeight);
						DriverLog("Render Target: %d %d\n", m_nRenderWidth, m_nRenderHeight);
						DriverLog("Seconds from Vsync to Photons: %f\n", m_flSecondsFromVsyncToPhotons);
						DriverLog("Display Frequency: %f\n", m_flDisplayFrequency);
						DriverLog("IPD: %f\n", m_flIPD);
					}
					else {
						DriverLog("Display Info Error: %i\n", GetLastError());
					}
					found = true;
				}

				if (found)
					break;

				displayIndex++;
			}

			if (found)
				break;

			deviceIndex++;
		}

		DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (LPVOID *)&lpdi, NULL);

		lpdi->EnumDevices(DI8DEVCLASS_GAMECTRL, staticGamepadSelect, this, DIEDFL_ATTACHEDONLY);
	}

	virtual ~CGlyphDeviceDriver()
	{
	}

	static BOOL CALLBACK staticGamepadSelect(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef)
	{
		CGlyphDeviceDriver * thisClass = (CGlyphDeviceDriver *)pvRef;

		return thisClass->GamepadSelect(lpddi);
	}

	BOOL CALLBACK GamepadSelect(LPCDIDEVICEINSTANCE lpddi)
	{
		char ProductName[260];
		wchar_t wProductGUID[64];
		char ProductGUID[64];
		 
		StringFromGUID2(lpddi->guidProduct, wProductGUID, 64);
		wcstombs_s(NULL, ProductGUID, wProductGUID, 64);
		wcstombs_s(NULL, ProductName, lpddi->tszProductName, 260);

		if (!strcmp(ProductGUID, "{00092C43-0000-0000-0000-504944564944}")) {
			lpdi->CreateDevice(lpddi->guidInstance, &lpdiJoystick, NULL);
			lpdiJoystick->SetDataFormat(&c_dfDIJoystick2);

			DriverLog("Glyph gamepad found: %s %s\n", ProductName, ProductGUID);

			return DIENUM_STOP;
		}
		else {
			DriverLog("Non Glyph Gamepad found: %s %s\n", ProductName, ProductGUID);
			return DIENUM_CONTINUE;
		}
	}

	static void staticPollGamepad(CGlyphDeviceDriver *current)
	{
		current->pollGamepad();
	}

	void pollGamepad()
	{
		while (g_deviceIsActive) {
			if (m_unObjectId != vr::k_unTrackedDeviceIndexInvalid)
			{
				HRESULT hr;

				if (lpdiJoystick != NULL) {
					hr = lpdiJoystick->Poll();

					if (FAILED(hr)) {
						lpdiJoystick->Acquire();
					}
					else {
						lpdiJoystick->GetDeviceState(sizeof(DIJOYSTATE2), &joyState);

						vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_unObjectId, GetPose(), sizeof(DriverPose_t));
					}
				}
				std::this_thread::sleep_for(std::chrono::microseconds(250));
			}
			else {
				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
		}
	}

	virtual EVRInitError Activate(vr::TrackedDeviceIndex_t unObjectId)
	{
		g_deviceIsActive = TRUE;
		gamepadPollingThread = new std::thread (CGlyphDeviceDriver::staticPollGamepad, this);
		if (!gamepadPollingThread) {
			DriverLog("Error starting head tracking thread\n");
		}

		m_unObjectId = unObjectId;
		m_ulPropertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);

		vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, Prop_ModelNumber_String, m_sModelNumber.c_str());
		vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, Prop_RenderModelName_String, m_sModelNumber.c_str());
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, Prop_UserIpdMeters_Float, m_flIPD);
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, Prop_UserHeadToEyeDepthMeters_Float, 0.f);
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, Prop_DisplayFrequency_Float, m_flDisplayFrequency);
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, Prop_SecondsFromVsyncToPhotons_Float, m_flSecondsFromVsyncToPhotons);

		// return a constant that's not 0 (invalid) or 1 (reserved for Oculus)
		vr::VRProperties()->SetUint64Property(m_ulPropertyContainer, Prop_CurrentUniverseId_Uint64, 1);

		// avoid "not fullscreen" warnings from vrmonitor
		vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer, Prop_IsOnDesktop_Bool, false);

		// Icons can be configured in code or automatically configured by an external file "drivername\resources\driver.vrresources".
		// Icon properties NOT configured in code (post Activate) are then auto-configured by the optional presence of a driver's "drivername\resources\driver.vrresources".
		// In this manner a driver can configure their icons in a flexible data driven fashion by using an external file.
		//
		// The structure of the driver.vrresources file allows a driver to specialize their icons based on their HW.
		// Keys matching the value in "Prop_ModelNumber_String" are considered first, since the driver may have model specific icons.
		// An absence of a matching "Prop_ModelNumber_String" then considers the ETrackedDeviceClass ("HMD", "Controller", "GenericTracker", "TrackingReference")
		// since the driver may have specialized icons based on those device class names.
		//
		// An absence of either then falls back to the "system.vrresources" where generic device class icons are then supplied.
		//
		// Please refer to "bin\drivers\sample\resources\driver.vrresources" which contains this sample configuration.
		//
		// "Alias" is a reserved key and specifies chaining to another json block.
		//
		// In this sample configuration file (overly complex FOR EXAMPLE PURPOSES ONLY)....
		//
		// "Model-v2.0" chains through the alias to "Model-v1.0" which chains through the alias to "Model-v Defaults".
		//
		// Keys NOT found in "Model-v2.0" would then chase through the "Alias" to be resolved in "Model-v1.0" and either resolve their or continue through the alias.
		// Thus "Prop_NamedIconPathDeviceAlertLow_String" in each model's block represent a specialization specific for that "model".
		// Keys in "Model-v Defaults" are an example of mapping to the same states, and here all map to "Prop_NamedIconPathDeviceOff_String".
		//
		bool bSetupIconUsingExternalResourceFile = true;
		if (!bSetupIconUsingExternalResourceFile)
		{
			// Setup properties directly in code.
			// Path values are of the form {drivername}\icons\some_icon_filename.png
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceOff_String, "{sample}/icons/headset_sample_status_off.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearching_String, "{sample}/icons/headset_sample_status_searching.gif");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{sample}/icons/headset_sample_status_searching_alert.gif");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReady_String, "{sample}/icons/headset_sample_status_ready.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{sample}/icons/headset_sample_status_ready_alert.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceNotReady_String, "{sample}/icons/headset_sample_status_error.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceStandby_String, "{sample}/icons/headset_sample_status_standby.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceAlertLow_String, "{sample}/icons/headset_sample_status_ready_low.png");
		}

		//return (found ? VRInitError_None : VRInitError_Init_HmdNotFound);
		//return VRInitError_None;
		return lpdiJoystick != NULL ? VRInitError_None : VRInitError_Init_HmdNotFound;
	}

	virtual void Deactivate()
	{
		g_deviceIsActive = FALSE;
		if (gamepadPollingThread) {
			gamepadPollingThread->join();
			delete gamepadPollingThread;
			gamepadPollingThread = NULL;
		}
		m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
	}

	virtual void EnterStandby()
	{
	}

	void *GetComponent(const char *pchComponentNameAndVersion)
	{
		if (!_stricmp(pchComponentNameAndVersion, vr::IVRDisplayComponent_Version))
		{
			return (vr::IVRDisplayComponent*)this;
		}

		// override this to add a component to a driver
		return NULL;
	}

	virtual void PowerOff()
	{
	}

	/** debug request from a client */
	virtual void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize)
	{
		if (unResponseBufferSize >= 1)
			pchResponseBuffer[0] = 0;
	}

	virtual void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight)
	{
		*pnX = m_nWindowX;
		*pnY = m_nWindowY;
		*pnWidth = m_nWindowWidth;
		*pnHeight = m_nWindowHeight;
	}

	virtual bool IsDisplayOnDesktop()
	{
		return false;
	}

	virtual bool IsDisplayRealDisplay()
	{
		return true;
	}

	virtual void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight)
	{
		*pnWidth = m_nRenderWidth;
		*pnHeight = m_nRenderHeight;
	}

	virtual void GetEyeOutputViewport(EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight)
	{
		if (useSBS) {
			*pnWidth = m_nWindowWidth / 2;
		}
		else {
			*pnWidth = m_nWindowWidth;
		}
		*pnHeight = m_nWindowHeight;

		if (eEye == Eye_Left)
		{
			*pnX = 0;
			*pnY = 0;
		}
		else
		{
			if (useSBS) {
				*pnX = m_nWindowWidth / 2;
			}
			else {
				*pnX = m_nWindowWidth;
			}
			*pnY = 0;
		}

		DriverLog("GetEyeOutput %s (%i, %i, %i, %i\n", eEye == Eye_Left?"Left Eye":"Right Eye", *pnX, *pnY, *pnWidth, *pnHeight);
	}

	virtual void GetProjectionRaw(EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom)
	{
		*pfLeft = -1.0;
		*pfRight = 1.0;
		*pfTop = -1.0;
		*pfBottom = 1.0;
	}

	virtual DistortionCoordinates_t ComputeDistortion(EVREye eEye, float fU, float fV)
	{
		DistortionCoordinates_t coordinates;
		coordinates.rfBlue[0] = fU;
		coordinates.rfBlue[1] = fV;
		coordinates.rfGreen[0] = fU;
		coordinates.rfGreen[1] = fV;
		coordinates.rfRed[0] = fU;
		coordinates.rfRed[1] = fV;
		return coordinates;
	}

	virtual DriverPose_t GetPose()
	{
		DriverPose_t pose = { 0 };
		pose.poseIsValid = true;
		pose.result = TrackingResult_Running_OK;
		pose.deviceIsConnected = true;
		pose.poseTimeOffset = -0.016f;

		pose.qWorldFromDriverRotation = HmdQuaternion_Init(1, 0, 0, 0);
		pose.qDriverFromHeadRotation = HmdQuaternion_Init(1, 0, 0, 0);

		double degX = (360.0 / 65535.0) * (joyState.lZ) + 180;
		double degY = (360.0 / 65535.0) * (joyState.lRx) + 180;
		double degZ = -(360.0 / 65535.0) * (joyState.lY) + 180;

		pose.qRotation = HmdQuaternion_Rotate(degX, degY, degZ);

		//DriverLog("Values x: %f, y: %f, z: %f, w: %f, degX: %f, degY: %f, degZ: %f, lX: %ld, lY: %ld, lZ: %ld\n", pose.qRotation.x, pose.qRotation.y, pose.qRotation.z, pose.qRotation.w, degX, degY, degZ, joyState.lX, joyState.lY, joyState.lZ);

		return pose;
	}


	void RunFrame()
	{
	}

	std::string GetSerialNumber() const { return m_sSerialNumber; }

private:
	vr::TrackedDeviceIndex_t m_unObjectId;
	vr::PropertyContainerHandle_t m_ulPropertyContainer;

	std::string m_sSerialNumber;
	std::string m_sModelNumber;

	int32_t m_nWindowX;
	int32_t m_nWindowY;
	int32_t m_nWindowWidth;
	int32_t m_nWindowHeight;
	int32_t m_nRenderWidth;
	int32_t m_nRenderHeight;
	float m_flSecondsFromVsyncToPhotons;
	float m_flDisplayFrequency;
	float m_flIPD;
};

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
class CServerDriver_Glyph : public IServerTrackedDeviceProvider
{
public:
	CServerDriver_Glyph()
		: m_pNullHmdLatest(NULL)
		, m_bEnableNullDriver(false)
	{
	}

	virtual EVRInitError Init(vr::IVRDriverContext *pDriverContext);
	virtual void Cleanup();
	virtual const char * const *GetInterfaceVersions() { return vr::k_InterfaceVersions; }
	virtual void RunFrame();
	virtual bool ShouldBlockStandbyMode() { return false; }
	virtual void EnterStandby() {}
	virtual void LeaveStandby() {}

private:
	CGlyphDeviceDriver *m_pNullHmdLatest;

	bool m_bEnableNullDriver;
};

CServerDriver_Glyph g_serverDriverNull;


EVRInitError CServerDriver_Glyph::Init(vr::IVRDriverContext *pDriverContext)
{
	VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
	InitDriverLog(vr::VRDriverLog());

	m_pNullHmdLatest = new CGlyphDeviceDriver();
	vr::VRServerDriverHost()->TrackedDeviceAdded(m_pNullHmdLatest->GetSerialNumber().c_str(), vr::TrackedDeviceClass_HMD, m_pNullHmdLatest);
	return VRInitError_None;
}

void CServerDriver_Glyph::Cleanup()
{
	CleanupDriverLog();
	delete m_pNullHmdLatest;
	m_pNullHmdLatest = NULL;
}


void CServerDriver_Glyph::RunFrame()
{
	if (m_pNullHmdLatest)
	{
		m_pNullHmdLatest->RunFrame();
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
HMD_DLL_EXPORT void *HmdDriverFactory(const char *pInterfaceName, int *pReturnCode)
{
	if (0 == strcmp(IServerTrackedDeviceProvider_Version, pInterfaceName))
	{
		return &g_serverDriverNull;
	}
	if (0 == strcmp(IVRWatchdogProvider_Version, pInterfaceName))
	{
		return &g_watchdogDriverNull;
	}

	if (pReturnCode)
		*pReturnCode = VRInitError_Init_InterfaceNotFound;

	return NULL;
}