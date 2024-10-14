/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2024 Source2ZE
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common.h"
#include "eventlistener.h"
#include "ctimer.h"
#include "gamesystem.h"
#include "entity/ccsplayercontroller.h"
#include "entity/ccsplayerpawn.h"
#include "vendor/nlohmann/json_fwd.hpp"

using ordered_json = nlohmann::ordered_json;

#define EW_PREFIX " \4[EntWatch]\1 "

#define EW_HUD_PREF_KEY_NAME "entwatch_hud"

enum EWItemHandlerType
{
	Type_None,
	Button,
	GameUi,
	Counter,	// used for CounterDown/Up modes
	Other,		// anything else
};

enum EWItemMode
{
	Mode_None = 1,
	Cooldown,				/* Infinite uses, cooldown between each use */
	MaxUses,				/* Limited uses with no cooldown between uses */
	MaxUsesWithCooldown,	/* Limited uses with a cooldown between each use */
	CooldownAfterUses,		/* Cooldown after all uses, allowing for more uses */
	CounterDown,			/* math_counter - stops when minimum reached */
	CounterUp,				/* math_counter - stops when maximum reached */
};

enum EWDropReason
{
	Drop,
	Death,
	Disconnect
};

enum EWHudMode
{
	Hud_None,
	Hud_On,
	Hud_ItemOnly,
};

enum EWTemplated
{
	Template_Auto = -1,
	Template_No,
	Template_Yes,
};

struct EWItemInstance;
struct EWItemHandler
{
	// Config variables
	EWItemHandlerType type;
	EWItemMode mode;
	std::string szHammerid;
	std::string szOutput;		/* Output name for when this is used e.g. OnPressed */
	int iCooldown;
	int iMaxuses;
	bool bShowUse;			/* Whether to show when this is used */
	bool bShowHud;			/* Track this cd/uses on hud/scoreboard */
	EWTemplated templated;		/* Is this entity templated (should we check for template suffix) */

	// Instance variables
	EWItemInstance* pItem;
	int iEntIndex;
	int iCurrentCooldown;
	int iCurrentUses;
	int iCounterValue;
	int iCounterMax;
	
	void SetDefaultValues();
	void Print();
public:
	EWItemHandler(EWItemHandler* pOther);
	EWItemHandler(ordered_json jsonKeys);
	
	void RemoveHook();
	void RegisterEntity(CBaseEntity* pEnt);
};

struct EWItem
{
	std::string szItemName;			/* Name to show on pickup/drop/use */
	std::string szShortName;		/* Name to show on hud/scoreboard */
	std::string szHammerid;			/* Hammerid of the weapon */
	char sChatColor[2];
	bool bShowPickup;				/* Whether to show pickup/drop messages in chat */
	bool bShowHud;					/* Whether to show this item on hud/scoreboard */
	bool bTransfer;					/* Can this item be transferred */
	EWTemplated templated;				/* Is this item templated (should we check for template suffix) */
	CUtlVector<EWItemHandler*> vecHandlers;		/* List of item abilities */
	CUtlVector<std::string*> vecTriggers;		/* HammerIds of triggers associated with this item */

	void SetDefaultValues();
	void ParseColor(std::string value);
public:
	EWItem(EWItem* pItem);
	EWItem(ordered_json jsonKeys);
};

struct EWItemInstance : EWItem	/* Current instance of defined items */
{
	int iOwnerSlot; /* Slot of the current holder */
	int iWeaponEnt;
	int iTemplateNum;
	bool bDropping;
	bool bAllowDrop; /* Whether this item should drop on death/disconnect only false for knife items */
	char sClantag[64];
	bool bHasThisClantag;

public:
	EWItemInstance(int iWeapon, EWItem* pItem) :
		EWItem(pItem),
		iOwnerSlot(-1),
		iWeaponEnt(iWeapon),
		iTemplateNum(-1),
		bDropping(false),
		bAllowDrop(true),
		bHasThisClantag(false) {};
	bool RegisterHandler(CBaseEntity* pEnt, EWItemHandlerType entType, int iHandlerTemplateNum);
	bool RemoveHandler(CBaseEntity* pEnt);
	int FindHandlerByEntIndex(int indexToFind);
	
	void Pickup(int slot);
	void Drop(EWDropReason reason, CCSPlayerController* pController);
};

class CEWHandler
{
public:
	CEWHandler()
	{
		bConfigLoaded = false;
		mapItemConfig.SetLessFunc(DefLessFunc(uint32));
		m_bHudTicking = false;
		iUseHookId = -1;
		iStartTouchHookId = -1;
		iTouchHookId = -1;
		iEndTouchHookId = -1;
		for (int i = 0; i < MAXPLAYERS+1; i++)
		{
			m_bEbanned[i] = false;
		}
	}

	bool bConfigLoaded;

	EWHudMode GetPlayerHudMode(CPlayerSlot slot);
	void SetPlayerHudMode(CPlayerSlot slot, EWHudMode mode);

	void UnLoadConfig();
	void LoadMapConfig(const char* sMapName);
	void LoadConfig(const char* sFilePath);		

	void PrintLoadedConfig(int iSlot) { PrintLoadedConfig(CPlayerSlot(iSlot)); };
	void PrintLoadedConfig(CPlayerSlot slot);

	void ClearItems();

	int FindItemIdByWeapon(std::string sHammerid);
	int FindItemInstanceByWeapon(int iWeaponEnt);
	int FindItemInstanceByOwner(int iOwnerSlot, bool bOnlyTransferrable, int iStartItem);
	int FindItemInstanceByName(std::string sItemName, bool bOnlyTransferrable);
	void RegisterHandler(CBaseEntity* pEnt, EWItemHandlerType entType);
	bool RegisterTrigger(CBaseEntity* pEnt);
	void AddTouchHook(CBaseEntity* pEnt);
	void Hook_Touch(CBaseEntity* pOther);
	bool RemoveTrigger(CBaseEntity* pEnt);
	void RemoveAllTriggers();
	void RemoveHandler(CBaseEntity* pEnt);
	void ResetAllClantags();

	void RegisterItem(int itemId, CBasePlayerWeapon* pWeapon);
	void PlayerPickup(CCSPlayerPawn* pPawn, CBasePlayerWeapon* pPlayerWeapon);
	void PlayerDrop(EWDropReason reason, int iItemInstance, CCSPlayerController* pController);

	void AddUseHook(CBaseEntity* pEnt);
	void RemoveUseHook(CBaseEntity* pEnt);
	void Hook_Use(InputData_t* pInput);

	CUtlMap<uint32, EWItem*> mapItemConfig;		/* items defined in the config */
	CUtlVector<EWItemInstance*> vecItems;						/* all items found spawned */
	
	CUtlVector<CHandle<CBaseEntity>> vecHookedTriggers;
	int iStartTouchHookId;
	int iTouchHookId;
	int iEndTouchHookId;

	CUtlVector<CHandle<CBaseEntity>> vecUseHookedEntities;
	int iUseHookId;

	bool m_bHudTicking;

	// TODO: Move to player class
	bool m_bEbanned[MAXPLAYERS+1];
};

extern CEWHandler* g_pEWHandler;
extern bool g_bEnableEntWatch;

void EW_OnLevelInit(const char* sMapName);
void EW_RoundPreStart();
void EW_OnEntitySpawned(CEntityInstance* pEntity);
void EW_OnEntityDeleted(CEntityInstance* pEntity);
void EW_OnWeaponDeleted(CBaseEntity* pEntity);
bool EW_Detour_CCSPlayer_WeaponServices_CanUse(CCSPlayer_WeaponServices* pWeaponServices, CBasePlayerWeapon* pPlayerWeapon);
void EW_Detour_CCSPlayer_WeaponServices_EquipWeapon(CCSPlayer_WeaponServices* pWeaponServices, CBasePlayerWeapon* pPlayerWeapon);
void EW_DropWeapon(CCSPlayer_WeaponServices* pWeaponServices, CBasePlayerWeapon* pWeapon);
void EW_PlayerDeath(IGameEvent* pEvent);
void EW_PlayerDisconnect(int slot);
void EW_SendBeginNewMatchEvent();
int GetTemplateSuffixNumber(const char* szName);
//float EW_UpdateHud();