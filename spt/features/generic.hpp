#pragma once
#include "..\feature.hpp"
#include "thirdparty\Signal.h"

#if defined(OE)
#include "vector.h"
#else
#include "mathlib\vector.h"
#endif

typedef void(__stdcall* _HudUpdate)(bool bActive);
typedef bool(__cdecl* _SV_ActivateServer)();
typedef void(__fastcall* _FinishRestore)(void* thisptr, int edx);
typedef void(__fastcall* _SetPaused)(void* thisptr, int edx, bool paused);
typedef void(__fastcall* _CViewRender__OnRenderStart)(void* thisptr, int edx);
typedef const Vector&(__cdecl* _MainViewOrigin)();
typedef void*(__cdecl* _GetClientModeNormal)();

// For hooks used by many features
class GenericFeature : public Feature
{
public:
	_HudUpdate ORIG_HudUpdate = nullptr;
	_SetPaused ORIG_SetPaused = nullptr;
	_SV_ActivateServer ORIG_SV_ActivateServer = nullptr;
	_FinishRestore ORIG_FinishRestore = nullptr;
	_MainViewOrigin ORIG_MainViewOrigin = nullptr;
	_GetClientModeNormal ORIG_GetClientModeNormal = nullptr;
	bool shouldPreventNextUnpause = false;

	void AdjustAngles_hook();
	Vector GetCameraOrigin();

protected:
	virtual bool ShouldLoadFeature() override;
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	uintptr_t ORIG_CHudDamageIndicator__GetDamagePosition = 0;
	uintptr_t ORIG_CHLClient__CanRecordDemo = 0;

	static void __stdcall HOOKED_HudUpdate(bool bActive);
	static bool __cdecl HOOKED_SV_ActivateServer();
	static void __fastcall HOOKED_FinishRestore(void* thisptr, int edx);
	static void __fastcall HOOKED_SetPaused(void* thisptr, int edx, bool paused);
};

extern GenericFeature spt_generic;