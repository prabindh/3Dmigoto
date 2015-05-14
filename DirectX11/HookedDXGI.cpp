
#include "DLLMainHook.h"
#include "log.h"
#include "util.h"

#include "HackerDXGI.h"


// This class is for a different approach than the wrapping of the system objects
// like we do with ID3D11Device for example.  When we wrap a COM object like that,
// it's not a real C++ object, and consequently cannot use the superclass normally,
// and requires boilerplate call-throughs for every interface to the object.  We
// may only care about a 5 calls, but we have to wrap all 150 calls. 
//
// Rather than do that with DXGI, this approach will be to singly hook the calls we
// are interested in, using the Deviare in-proc hooking.  We'll still create
// objects for encapsulation, by returning HackerDXGIFactory and HackerDXGIFactory2.




// -----------------------------------------------------------------------------
// The signature copied from dxgi.h, in C section.
// This unusual format also provides storage for the original pointer to the 
// routine.

HRESULT(STDMETHODCALLTYPE *OrigCreateSwapChain)(
	IDXGIFactory * This,
	/* [annotation][in] */
	_In_  IUnknown *pDevice,
	/* [annotation][in] */
	_In_  DXGI_SWAP_CHAIN_DESC *pDesc,
	/* [annotation][out] */
	_Out_  IDXGISwapChain **ppSwapChain);


// Our override method for any callers to CreateSwapChain.

HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
	IDXGIFactory * This,
	/* [annotation][in] */
	_In_  IUnknown *pDevice,
	/* [annotation][in] */
	_In_  DXGI_SWAP_CHAIN_DESC *pDesc,
	/* [annotation][out] */
	_Out_  IDXGISwapChain **ppSwapChain)
{
	HRESULT hr;
	IDXGISwapChain *mOrigSwapChain;

	hr = OrigCreateSwapChain(This, pDevice, pDesc, ppSwapChain);
	mOrigSwapChain = *ppSwapChain;

	LogInfo("HookedSwapChain::HookedCreateSwapChain mOrigSwapChain: %p, pDevice: %p, result: %d \n", mOrigSwapChain, pDevice, hr);

	return hr;
}


// -----------------------------------------------------------------------------

// This serves a dual purpose of defining the interface routine as required by
// DXGI, and also is the storage for the original call, returned by cHookMgr.Hook.

HRESULT(STDMETHODCALLTYPE *pOrigPresent)(
	IDXGISwapChain * This,
	/* [in] */ UINT SyncInterval,
	/* [in] */ UINT Flags);

static SIZE_T nHookId = 0;

//Overlay *overlay;


// Log both to console using Nektra logging, and using our LogInfo, 
// in case we are loading so early that our regular log is not ready.

#define DoubleLog(fmt, ...) \
	if (LogFile) fprintf(LogFile, fmt, __VA_ARGS__); \
	if (bLog) NktHookLibHelpers::DebugPrint(fmt, __VA_ARGS__)


// -----------------------------------------------------------------------------

typedef HRESULT(WINAPI *lpfnCreateDXGIFactory)(REFIID riid, void **ppFactory);
SIZE_T nFactory_ID;
lpfnCreateDXGIFactory fnOrigCreateFactory;


// Actual function called by the game for every CreateDXGIFactory they make.
// This is only called for the in-process game, not system wide.
//
// This is our replacement, so that we can return a wrapped factory, which
// will allow us access to the SwapChain.
// 
// The documentation says specifically to NOT mix Factory1 and Factory use,
// so we will not look for Factory1 here.

static HRESULT WINAPI Hooked_CreateDXGIFactory(REFIID riid, void **ppFactory)
{
	InitD311();
	LogInfo("Hooked_CreateDXGIFactory called with riid: %s \n", NameFromIID(riid).c_str());
	LogInfo("  calling original CreateDXGIFactory API\n");

	// If we are being requested to create a DXGIFactory2, lie and say it's not possible.
	if (riid == __uuidof(IDXGIFactory2))
	{
		LogInfo("  returns E_NOINTERFACE as error. \n");
		*ppFactory = NULL;
		return E_NOINTERFACE;
	}

	IDXGIFactory *origFactory;
	HRESULT hr = fnOrigCreateFactory(riid, (void **)&origFactory);
	if (FAILED(hr))
	{
		LogInfo("  failed with HRESULT=%x \n", hr);
		return hr;
	}
	LogInfo("  CreateDXGIFactory returned factory = %p, result = %x \n", origFactory, hr);

	HackerDXGIFactory *factoryWrap;
	factoryWrap = new HackerDXGIFactory(origFactory, NULL, NULL);

	// ToDo: this null check is not necessary as it would throw exception.
	if (factoryWrap == NULL)
	{
		LogInfo("  error allocating factoryWrap. \n");
		origFactory->Release();
		return E_OUTOFMEMORY;
	}
	if (ppFactory)
		*ppFactory = factoryWrap;

	return hr;
}


// -----------------------------------------------------------------------------

typedef HRESULT(WINAPI *lpfnCreateDXGIFactory1)(REFIID riid, void **ppFactory1);
SIZE_T nFactory1_ID; 
lpfnCreateDXGIFactory1 fnOrigCreateFactory1;

// It's not legal to mix Factory1 and Factory in the same app, so we'll not
// look for Factory here.  Bizarrely though, Factory2 is expected.

static HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void **ppFactory1)
{
	InitD311();
	LogInfo("Hooked_CreateDXGIFactory1 called with riid: %s \n", NameFromIID(riid).c_str());
	LogInfo("  calling original CreateDXGIFactory1 API\n");

	// If we are being requested to create a DXGIFactory2, lie and say it's not possible.
	if (riid == __uuidof(IDXGIFactory2))
	{
		LogInfo("  returns E_NOINTERFACE as error. \n");
		*ppFactory1 = NULL;
		return E_NOINTERFACE;
	}

	IDXGIFactory1 *origFactory1;
	HRESULT hr = fnOrigCreateFactory1(riid, (void **)&origFactory1);
	if (FAILED(hr))
	{
		LogInfo("  failed with HRESULT=%x \n", hr);
		return hr;
	}
	LogInfo("  CreateDXGIFactory1 returned factory = %p, result = %x \n", origFactory1, hr);

	HackerDXGIFactory1 *factory1Wrap;
	factory1Wrap = new HackerDXGIFactory1(origFactory1, NULL, NULL);

	// ToDo: this null check is not necessary as it would throw exception.
	if (factory1Wrap == NULL)
	{
		LogInfo("  error allocating factoryWrap. \n");
		origFactory1->Release();
		return E_OUTOFMEMORY;
	}
	if (ppFactory1)
		*ppFactory1 = factory1Wrap;

	return hr;
}

// -----------------------------------------------------------------------------

// Load the dxgi.dll and hook the two calls for CreateFactory.

bool InstallDXGIHooks(void)
{
	HINSTANCE hDXGI;
	LPVOID fnCreateDXGIFactory;
	LPVOID fnCreateDXGIFactory1;
	DWORD dwOsErr;

	// Not certain this is necessary, but it won't hurt, and ensures it's loaded.
	LoadLibrary(L"dxgi.dll");

	DoubleLog("Attempting to hook dxgi CreateFactory using Deviare in-proc. \n");
	cHookMgr.SetEnableDebugOutput(bLog);

	hDXGI = NktHookLibHelpers::GetModuleBaseAddress(L"dxgi.dll");
	if (hDXGI == NULL)
	{
		DoubleLog("  Failed to get dxgi module for CreateFactory hook. \n");
		return false;
	}

	fnCreateDXGIFactory = NktHookLibHelpers::GetProcedureAddress(hDXGI, "CreateDXGIFactory");
	if (fnCreateDXGIFactory == NULL)
	{
		DoubleLog("  Failed to GetProcedureAddress of CreateDXGIFactory for dxgi hook. \n");
		return false;
	}
	dwOsErr = cHookMgr.Hook(__out &nFactory_ID, __out(LPVOID*)&(fnOrigCreateFactory),
		__in fnCreateDXGIFactory, __in Hooked_CreateDXGIFactory);
	DoubleLog("  Install Hook for DXGI::CreateDXGIFactory using Deviare in-proc: %x \n", dwOsErr);
	if (dwOsErr != 0)
		return false;

	fnCreateDXGIFactory1 = NktHookLibHelpers::GetProcedureAddress(hDXGI, "CreateDXGIFactory1");
	if (fnCreateDXGIFactory1 == NULL)
	{
		DoubleLog("  Failed to GetProcedureAddress of CreateDXGIFactory1 for dxgi hook. \n");
		return false;
	}
	dwOsErr = cHookMgr.Hook(__out &nFactory1_ID, __out (LPVOID*)&(fnOrigCreateFactory1),
		__in fnCreateDXGIFactory1, __in Hooked_CreateDXGIFactory1);
	DoubleLog("  Install Hook for DXGI::CreateDXGIFactory1 using Deviare in-proc: %x \n", dwOsErr);
	if (dwOsErr != 0)
		return false;

	DoubleLog("  Successfully hooked CreateDXGIFactory using Deviare in-proc. \n");
	return true;
}



// -----------------------------------------------------------------------------

// Some partly obsolete comments, but still maybe worthwhile as thoughts on DXGI.

// The hooks need to be installed late, and cannot be installed during DLLMain, because
// they need to be installed in the COM object vtable itself, and the order cannot be
// defined that early.  Because the documentation says it's not viable at DLLMain time,
// we'll install these hooks at InitD311() time, essentially the first call of D3D11.
// 
// The piece we care about in DXGI is the swap chain, and we don't otherwise have a
// good way to access it.  It can be created directly via DXGI, and not through 
// CreateDeviceAndSwapChain.
//
// Not certain, but it seems likely that we only need to hook a given instance of the
// calls we want, because they are not true objects with attached vtables, they have
// a non-standard vtable/indexing system, and the main differentiator is the object
// passed in as 'this'.  
//
// After much experimentation and study, it seems clear that we should use the in-proc
// version of Deviare. I tried to see if Deviare2 would be a match, but they have a 
// funny event callback mechanism that requires an ATL object connection, and is not
// really suited for same-process operations.  It's really built with separate
// processes in mind.

// For this object, we want to use the CINTERFACE, not the C++ interface.
// The reason is that it allows us easy access to the COM object vtable, which
// we need to hook in order to override the functions.  Once we include dxgi.h
// it will be defined with C headers instead.
//
// This is a little odd, but it's the way that Detours hooks COM objects, and
// thus it seems superior to the Nektra approach of groping the vtable directly
// using constants and void* pointers.
// 
// This is only used for .cpp file here, not the .h file, because otherwise other
// units get compiled with this CINTERFACE, which wrecks their calling out.

// -----------------------------------------------------------------------------

// Obsolete code that is useful for reference only.
// This is a technique I created, doesn't exist on the internet.

//#define CINTERFACE
//#include <dxgi.h>
//#undef CINTERFACE
//
//#include <d3d11.h>
//
//#include "log.h"
//#include "DLLMainHook.h"
////#include "Overlay.h"

//bool InstallDXGIHooks(void)
//{
//	// Seems like we need to do this in order to init any use of COM here.
//	CoInitializeEx(NULL, COINIT_MULTITHREADED);
//
//	HRESULT result;
//	IDXGIFactory* factory;
//		
//	// We need a DXGI interface factory in order to get access to the CreateSwapChain
//	// call. This CreateDXGIFactory call is defined as an export in dxgi.dll and is
//	// called normally.  It will return an interface to a COM object.
//	// We will have access to the COM routines available after we have one of these.
//
//	result = CreateDXGIFactory(IID_IDXGIFactory, (void**)(&factory));
//	if (FAILED(result))
//	{
//		LogInfo("*** InstallDXGIHooks CreateDXGIFactory failed: %d \n", result);
//		return false;
//	}
//
//	// Hook the factory call for CreateSwapChain, as the game may use that to create
//	// its swapchain instead of CreateDeviceAndSwapChain.
//
//	cHookMgr.SetEnableDebugOutput(TRUE);
//
//	// Routine address fetched from the COM vtable.  This address will be patched by
//	// deviare to jump to HookedCreateSwapChain, which will then need the original
//	// address to call the unmodified function, found in OrigCreateSwapChain.
//
//	LPVOID CreateSwapChain = factory->lpVtbl->CreateSwapChain;
//
//	DWORD dwOsErr;
//	dwOsErr = cHookMgr.Hook(&nCSCHookId, &CreateSwapChain, &OrigCreateSwapChain, HookedCreateSwapChain);
//	if (FAILED(dwOsErr))
//	{
//		LogInfo("*** InstallDXGIHooks Hook failed: %d \n", dwOsErr);
//		return false;
//	}
//
//	LogInfo("InstallDXGIHooks CreateSwapChain result: %d, at: %p \n", dwOsErr, OrigCreateSwapChain);
//
//
//	// Create a SwapChain, just so we can get access to its vtable, and thus hook
//	// the Present() call to ours.
//
//	// not sure where to get device from.
//	//IDXGIFactory_CreateSwapChain(factory, pDevice, pDesc, ppSwapChain);
//
//	//HackerDevice *pDevice;
//	//DXGI_SWAP_CHAIN_DESC *pDesc;
//	//IDXGISwapChain *ppSwapChain;
//	//IDXGIFactory2_CreateSwapChain(factory, pDevice, pDesc, &ppSwapChain);
//
//	return true;
//}

//void UninstallDXGIHooks()
//{
//	DWORD dwOsErr; 
//	dwOsErr = cHookMgr.Unhook(nCSCHookId);
//}

// -----------------------------------------------------------------------------
//HookedSwapChain::HookedSwapChain(IDXGISwapChain* pOrigSwapChain)
//{
//	DWORD dwOsErr;
//
//	mOrigSwapChain = pOrigSwapChain;
//
//	if (pOrigPresent == NULL)
//	{
//		LPVOID dxgiSwapChain = pOrigSwapChain->lpVtbl->Present;
//
//		cHookMgr.SetEnableDebugOutput(TRUE);
//		dwOsErr = cHookMgr.Hook(&nCSCHookId, (LPVOID*)&pOrigPresent, &dxgiSwapChain, HookedPresent);
//		if (dwOsErr)
//		{
//			LogInfo("*** HookedSwapChain::HookedSwapChain Hook failed: %d \n", dwOsErr);
//			return;
//		}
//	}
//	LogInfo("HookedSwapChain::HookedSwapChain hooked Present result: %d, at: %p \n", dwOsErr, pOrigPresent);
//}
//
//
//HookedSwapChain::~HookedSwapChain()
//{
//	
//}


// Hook the Present call in the DXGI COM interface.
// Will only hook once, because there is only one instance
// of the actual underlying code.
//
// The cHookMgr is assumed to already be created and initialized by the
// C++ runtime, even if we are not hooking in DLLMain.

//void HookSwapChain(IDXGISwapChain* pSwapChain, ID3D11Device* pDevice, ID3D11DeviceContext* pContext)
//{
//	DWORD dwOsErr;
//	HRESULT hr;
//
//	if (pOrigPresent != NULL)
//	{
//		LogInfo("*** HookSwapChain called again. SwapChain: %p, Device: %p, Context: %p \n", pSwapChain, pDevice, pContext);
//		return;
//	}
//
//	// Seems like we need to do this in order to init any use of COM here.
//	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
//	if (hr != NOERROR)
//		LogInfo("HookSwapChain CoInitialize return error: %d \n", hr);
//
//	// The tricky part- fetching the actual address of the original
//	// DXGI::Present call.
//	LPVOID dxgiSwapChain = pSwapChain->lpVtbl->Present;
//
//	cHookMgr.SetEnableDebugOutput(bLog);
//
//	dwOsErr = cHookMgr.Hook(&nHookId, (LPVOID*)&pOrigPresent, dxgiSwapChain, HookedPresent, 0);
//	if (dwOsErr)
//	{
//		LogInfo("*** HookSwapChain Hook failed: %d \n", dwOsErr);
//		return;
//	}
//
//
//	// Create Overlay class that will be responsible for drawing any text
//	// info over the game. Using the original Device and Context.
//	//	overlay = new Overlay(pDevice, pContext);
//
//	LogInfo("HookSwapChain hooked Present result: %d, at: %p \n", dwOsErr, pOrigPresent);
//}

// -----------------------------------------------------------------------------

// The hooked static version of Present.
//
// It is worth noting, since it took me 3 days to figure it out, than even though
// this is defined C style, that it must use STDMETHODCALLTYPE (or__stdcall) 
// because otherwise the stack is broken by the different calling conventions.
//
// In normal method calls, the 'this' parameter is implicitly added.  Since we are
// using the C style dxgi interface though, we are declaring this routine differently.
//
// Since we want to allow reentrancy for this call, we need to use the returned
// lpfnPresent to call the original, instead of the alternate approach offered by
// Deviare.

//static HRESULT STDMETHODCALLTYPE HookedPresent(
//	IDXGISwapChain * This,
//	/* [in] */ UINT SyncInterval,
//	/* [in] */ UINT Flags)
//{
//	HRESULT hr;
//
//	// Draw the on-screen overlay text with hunting info, before final Present.
//	//overlay->DrawOverlay();
//
//	hr = pOrigPresent(This, SyncInterval, Flags);
//
//	LogDebug("HookedPresent result: %d \n", hr);
//
//	return hr;
//}

