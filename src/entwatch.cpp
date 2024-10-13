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

#include "entwatch.h"
#include "usermessages.pb.h"
#include "gameevents.pb.h"
#include "cs2fixes.h"
#include "commands.h"
#include "utils/entity.h"
#include "playermanager.h"
#include "ctimer.h"
#include "eventlistener.h"
#include "zombiereborn.h"
#include "entity/cbaseentity.h"
#include "entity/cgamerules.h"
#include "entity/services.h"
#include "entity/cteam.h"
#include "entity/cparticlesystem.h"
#include "engine/igameeventsystem.h"
#include "entity/cbasebutton.h"
#include "networksystem/inetworkmessages.h"
#include "recipientfilters.h"
#include "serversideclient.h"
#include "user_preferences.h"
#include "customio.h"
#include <sstream>
#include "leader.h"
#include "tier0/vprof.h"
#include <fstream>
#include "vendor/nlohmann/json.hpp"

#include "tier0/memdbgon.h"

class InputData_t
{
public:
	CBaseEntity* pActivator;
	CBaseEntity* pCaller;
	variant_t      value;
	int            nOutputID;
};

extern CGlobalVars* gpGlobals;
extern IGameEventManager2* g_gameEventManager;
extern IGameEventSystem* g_gameEventSystem;
extern INetworkMessages* g_pNetworkMessages;
extern CCSGameRules* g_pGameRules;

CEWHandler* g_pEWHandler = nullptr;

SH_DECL_MANUALHOOK1_void(CBaseEntity_Use, 0, 0, 0, InputData_t*);
SH_DECL_MANUALHOOK1_void(CBaseEntity_StartTouch, 0, 0, 0, CBaseEntity*);
SH_DECL_MANUALHOOK1_void(CBaseEntity_Touch, 0, 0, 0, CBaseEntity*);
SH_DECL_MANUALHOOK1_void(CBaseEntity_EndTouch, 0, 0, 0, CBaseEntity*);

bool g_bEnableEntWatch = false;
FAKE_BOOL_CVAR(entwatch_enable, "Whether to enable EntWatch features", g_bEnableEntWatch, false, false)

bool g_bEnableFiltering = true;
FAKE_BOOL_CVAR(entwatch_auto_filter, "Whether to automatically block non-item holders from triggering uses", g_bEnableFiltering, true, false)

bool g_bUseEntwatchClantag = true;
FAKE_BOOL_CVAR(entwatch_clantag, "Whether to set item holder's clantag and set score", g_bUseEntwatchClantag, true, false)

int g_iItemHolderScore = 9999;
FAKE_INT_CVAR(entwatch_score, "Score to give item holders (0 = dont change score at all) Requires entwatch_clantag 1", g_iItemHolderScore, 9999, false);

void EWItemHandler::SetDefaultValues()
{
	type = EWItemHandlerType::Button;
	mode = EWItemMode::Mode_None;
	szHammerid = "";
	szOutput = "";
	iCooldown = 0;
	iMaxuses = 0;
	bShowUse = true;
	bShowHud = true;
}

void EWItemHandler::Print()
{
	Message("type: %d\n", (int)type);
	Message("mode: %d\n", (int)mode);
	Message("hammerid: %s\n", szHammerid.c_str());
	Message("event: %s\n", szOutput.c_str());
	Message("cooldown: %d\n", iCooldown);
	Message("maxuses: %d\n", iMaxuses);
	Message("bshowuse: %s\n", bShowUse ? "True" : "False");
	Message("bshowhud: %s\n", bShowHud ? "True" : "False");
}

EWItemHandler::EWItemHandler(EWItemHandler* pOther)
{
	type = pOther->type;
	mode = pOther->mode;
	szHammerid = pOther->szHammerid;
	szOutput = pOther->szOutput;
	iCooldown = pOther->iCooldown;
	iMaxuses = pOther->iMaxuses;
	bShowUse = pOther->bShowUse;
	bShowHud = pOther->bShowHud;

	pItem = pOther->pItem;
	iEntIndex = -1;
	iCurrentCooldown = 0;
	iCurrentUses = 0;
	iCounterValue = 0;
	iCounterMax = 0;
}

EWItemHandler::EWItemHandler(ordered_json jsonKeys)
{
	SetDefaultValues();

	if (jsonKeys.contains("type"))
	{
		std::string value = jsonKeys["type"].get<std::string>();
		if (value == "game_ui" || value == "gameui")
			type = EWItemHandlerType::GameUi;
		else
			type = EWItemHandlerType::Button;
	}

	if (jsonKeys.contains("hammerid"))
		szHammerid = jsonKeys["hammerid"].get<std::string>();

	if (jsonKeys.contains("event"))
		szOutput = jsonKeys["event"].get<std::string>();

	if (jsonKeys.contains("mode"))
		mode = (EWItemMode)jsonKeys["mode"].get<int>();

	if (jsonKeys.contains("cooldown"))
		iCooldown = jsonKeys["cooldown"].get<int>();

	if (jsonKeys.contains("maxuses"))
		iMaxuses = jsonKeys["maxuses"].get<int>();

	if (jsonKeys.contains("message"))
		bShowUse = jsonKeys["message"].get<bool>();

	if (jsonKeys.contains("ui"))
		bShowHud = jsonKeys["ui"].get<bool>();
}

void EWItemHandler::RemoveHook()
{
	CBaseEntity* pEnt = (CBaseEntity*)g_pEntitySystem->GetEntityInstance((CEntityIndex)iEntIndex);
	if (pEnt)
	{
		g_pEWHandler->RemoveHandler(pEnt);
	}
}

// entity type matches handler type here
// just do the correct stuff
void EWItemHandler::RegisterEntity(CBaseEntity* pEntity)
{
	iEntIndex = pEntity->entindex();
	//TODO FireOutput hook (WAITING FOR ICE)
	switch (type)
	{
	case Button:
		g_pEWHandler->AddUseHook(pEntity);
		//TODO fireoutput stuff?
		break;
	case GameUi:
		break;
	case Other:
		break;
	default:
		break;
	}
}

void EWItem::SetDefaultValues()
{
	szItemName = "";
	szShortName = "";
	/* no default hammerid */
	V_strcpy(sChatColor, "\x01");
	bShowPickup = true;
	bShowHud = true;
	bTransfer = true;
	vecHandlers.Purge();
	vecTriggers.Purge();
}

void EWItem::ParseColor(std::string value)
{
	if (value == "white" || value == "default")
		V_strcpy(sChatColor, "\x01");
	else if (value == "darkred")
		V_strcpy(sChatColor, "\x02");
	else if (value == "team")
		V_strcpy(sChatColor, "\x03");
	else if (value == "green")
		V_strcpy(sChatColor, "\x04");
	else if (value == "lightgreen")
		V_strcpy(sChatColor, "\x05");
	else if (value == "olive")
		V_strcpy(sChatColor, "\x06");
	else if (value == "red")
		V_strcpy(sChatColor, "\x07");
	else if (value == "gray" || value == "grey")
		V_strcpy(sChatColor, "\x08");
	else if (value == "yellow")
		V_strcpy(sChatColor, "\x09");
	else if (value == "silver")
		V_strcpy(sChatColor, "\x0A");
	else if (value == "blue")
		V_strcpy(sChatColor, "\x0B");
	else if (value == "darkblue")
		V_strcpy(sChatColor, "\x0C");
	// \x0D is the same as \x0A
	else if (value == "purple" || value == "pink")
		V_strcpy(sChatColor, "\x0E");
	else if (value == "red2")
		V_strcpy(sChatColor, "\x0F");
	else if (value == "orange" || value == "gold")
		V_strcpy(sChatColor, "\x10");
}

EWItem::EWItem(EWItem* pItem)
{
	szItemName = pItem->szItemName;
	szShortName = pItem->szShortName;
	szHammerid = pItem->szHammerid;
	V_strcpy(sChatColor, pItem->sChatColor);
	bShowPickup = pItem->bShowPickup;
	bShowHud = pItem->bShowHud;
	bTransfer = pItem->bTransfer;

	vecHandlers.Purge();
	FOR_EACH_VEC(pItem->vecHandlers, i)
	{
		EWItemHandler* pHandler = new EWItemHandler(pItem->vecHandlers[i]);
		vecHandlers.AddToTail(pHandler);
	}

	vecTriggers.Purge();
	FOR_EACH_VEC(pItem->vecTriggers, i)
	{
		vecTriggers.AddToTail(pItem->vecTriggers[i]);
	}
}

EWItem::EWItem(ordered_json jsonKeys)
{
	SetDefaultValues();

	if (jsonKeys.contains("name"))
		szItemName = jsonKeys["name"].get<std::string>();

	if (jsonKeys.contains("shortname"))
		szShortName = jsonKeys["shortname"].get<std::string>();
	else
		szShortName = szItemName; // Set shortname to long name if it isnt set

	// We know it contains hammerid
	szHammerid = jsonKeys["hammerid"].get<std::string>();

	if (jsonKeys.contains("color"))
		ParseColor(jsonKeys["color"].get<std::string>());
	else if (jsonKeys.contains("colour")) // gotta represent
		ParseColor(jsonKeys["colour"].get<std::string>());

	if (jsonKeys.contains("message"))
		bShowPickup = jsonKeys["message"].get<bool>();

	if (jsonKeys.contains("ui"))
		bShowHud = jsonKeys["ui"].get<bool>();

	if (jsonKeys.contains("transfer"))
		bTransfer = jsonKeys["transfer"].get<bool>();

	if (jsonKeys.contains("triggers"))
	{
		if (jsonKeys["triggers"].size() > 0)
		{
			for (std::string bl : jsonKeys["triggers"])
			{
				if (bl == "")
					continue;

				std::string* trig = new std::string(bl);
				vecTriggers.AddToTail(trig);
			}
		}
	}

	if (jsonKeys.contains("handlers"))
	{
		if (jsonKeys["handlers"].size() > 0)
		{
			for (auto& [key, handlerEntry] : jsonKeys["handlers"].items())
			{
				EWItemHandler* handler = new EWItemHandler(handlerEntry);
				vecHandlers.AddToTail(handler);
			}
		}
	}
}

// true: found a matching handler for this ent, false: didnt
bool EWItemInstance::RegisterHandler(CBaseEntity* pEnt, EWItemHandlerType entType)
{
	FOR_EACH_VEC(vecHandlers, i)
	{
		if (vecHandlers[i]->iEntIndex != -1)
			continue; // this handler is already setup

		// Check handler type
		if (vecHandlers[i]->type != entType)
			continue;

		// check handler id
		std::string hammerid = pEnt->m_sUniqueHammerID.Get().String();
		if (vecHandlers[i]->szHammerid != hammerid)
			continue;

		vecHandlers[i]->RegisterEntity(pEnt);
		vecHandlers[i]->pItem = this;
		return true;
	}
	return false;
}

bool EWItemInstance::RemoveHandler(CBaseEntity* pEnt)
{
	FOR_EACH_VEC(vecHandlers, i)
	{
		if (vecHandlers[i]->iEntIndex == pEnt->entindex())
		{
			//TODO: check FireOutput unhook?
			if (vecHandlers[i]->type == EWItemHandlerType::Button)
				g_pEWHandler->RemoveUseHook(pEnt);
			vecHandlers[i]->iEntIndex = -1;
			return true;
		}
	}
	return false;
}

int EWItemInstance::FindHandlerByEntIndex(int indexToFind)
{
	if (vecHandlers.Count() <= 0)
		return -1;

	FOR_EACH_VEC(vecHandlers, i)
	{
		if (vecHandlers[i]->iEntIndex == indexToFind)
		{
			return i;
		}
	}
	return -1;
}

/* Called when a player picks up this item */
void EWItemInstance::Pickup(int slot)
{
	iOwnerSlot = slot;

	ZEPlayer* pPlayer = g_playerManager->GetPlayer(CPlayerSlot(iOwnerSlot));
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iOwnerSlot);
	if (!pPlayer || !pController)
	{
		iOwnerSlot = -1;
		return;
	}

	if (pPlayer->IsFakeClient())
		Message(EW_PREFIX "%s [BOT] has picked up %s (weaponid:%d)\n", pController->GetPlayerName(), szItemName.c_str(), iWeaponEnt);
	else
		Message(EW_PREFIX "%s [%llu] has picked up %s (weaponid:%d)\n", pController->GetPlayerName(), pPlayer->GetUnauthenticatedSteamId64(), szItemName.c_str(), iWeaponEnt);

	// Set clantag
	if (g_bUseEntwatchClantag && bShowHud)
	{
		// Only set clantag if owner doesnt already have one set
		bool bShouldSetClantag = true;
		int otherItem = g_pEWHandler->FindItemInstanceByOwner(iOwnerSlot, false, 0);
		while (otherItem != -1)
		{
			if (g_pEWHandler->vecItems[otherItem]->bHasThisClantag)
			{
				bShouldSetClantag = false;
				break;
			}
			otherItem = g_pEWHandler->FindItemInstanceByOwner(iOwnerSlot, false, otherItem + 1);
		}

		if (bShouldSetClantag)
		{
			bHasThisClantag = true;
			pController->m_szClan(sClantag);
			if (g_iItemHolderScore > -1)
			{
				int score = pController->m_iScore + g_iItemHolderScore;
				pController->m_iScore = score;
			}

			EW_SendBeginNewMatchEvent();
		}
	}

	if (bShowPickup)
		ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "\x03%s \x05has picked up %s%s", pController->GetPlayerName(), sChatColor, szItemName.c_str());
}

void EWItemInstance::Drop(EWDropReason reason, CCSPlayerController* pController)
{
	ZEPlayer* pPlayer = g_playerManager->GetPlayer(CPlayerSlot(iOwnerSlot));
	if (!pPlayer)
	{
		iOwnerSlot = -1;
		bDropping = false;
		return;
	}

	if (g_bUseEntwatchClantag && bShowHud && bHasThisClantag)
	{
		bool bSetAnotherClantag = false;

		// Check if this player is holding another item that should be shown
		int otherItem = g_pEWHandler->FindItemInstanceByOwner(iOwnerSlot, false, 0);
		while (otherItem != -1)
		{
			if (g_pEWHandler->vecItems[otherItem]->bShowHud && !g_pEWHandler->vecItems[otherItem]->bHasThisClantag)
			{
				// Player IS holding another item, score doesnt need adjusting

				pController->m_szClan(g_pEWHandler->vecItems[otherItem]->sClantag);
				g_pEWHandler->vecItems[otherItem]->bHasThisClantag = true;
				bSetAnotherClantag = true;
				break;
			}
			otherItem = g_pEWHandler->FindItemInstanceByOwner(iOwnerSlot, false, otherItem + 1);
		}
		bHasThisClantag = false;

		if (!bSetAnotherClantag)
		{
			if (g_iItemHolderScore != 0)
			{
				int score = pController->m_iScore - g_iItemHolderScore;
				pController->m_iScore = score;
			}

			pController->m_szClan("");
		}

		EW_SendBeginNewMatchEvent();
	}

	char sPlayerInfo[64];
	if (pPlayer->IsFakeClient())
		V_snprintf(sPlayerInfo, sizeof(sPlayerInfo), "%s [BOT]", pController->GetPlayerName());
	else
		V_snprintf(sPlayerInfo, sizeof(sPlayerInfo), "%s [%llu]", pController->GetPlayerName(), pPlayer->GetUnauthenticatedSteamId64());

	switch (reason)
	{
	case EWDropReason::Drop:
		Message(EW_PREFIX "%s has dropped %s (weaponid:%d)\n", sPlayerInfo, szItemName.c_str(), iWeaponEnt);
		if (bShowPickup)
			ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "\x03%s \x05has dropped %s%s", pController->GetPlayerName(), sChatColor, szItemName.c_str());
		break;
	case EWDropReason::Death:
		if (bAllowDrop)
		{
			Message(EW_PREFIX "%s has died and dropped %s (weaponid:%d)\n", sPlayerInfo, szItemName.c_str(), iWeaponEnt);
			if (bShowPickup)
				ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "\x03%s\x05 died and dropped %s%s", pController->GetPlayerName(), sChatColor, szItemName.c_str());
		}
		else
		{
			Message(EW_PREFIX "%s has died with %s (weaponid:%d)\n", sPlayerInfo, szItemName.c_str(), iWeaponEnt);
			if (bShowPickup)
				ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "\x03%s\x05 died with %s%s", pController->GetPlayerName(), sChatColor, szItemName.c_str());
		}
		break;
	case EWDropReason::Disconnect:
		if (bAllowDrop)
		{
			Message(EW_PREFIX "%s has disconnected and dropped %s (weaponid:%d)\n", sPlayerInfo, szItemName.c_str(), iWeaponEnt);
			if (bShowPickup)
				ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "\x03%s\x05 disconnected and dropped %s%s", pController->GetPlayerName(), sChatColor, szItemName.c_str());
		}
		else
		{
			Message(EW_PREFIX "%s has disconnected with %s (weaponid:%d)\n", sPlayerInfo, szItemName.c_str(), iWeaponEnt);
			if (bShowPickup)
				ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "\x03%s\x05 disconnected with %s%s", pController->GetPlayerName(), sChatColor, szItemName.c_str());
		}
		break;
	default:
		break;
	}

	iOwnerSlot = -1;
	bDropping = false;
}

EWHudMode CEWHandler::GetPlayerHudMode(CPlayerSlot slot)
{
	return (EWHudMode)(g_pUserPreferencesSystem->GetPreferenceInt(slot.Get(), EW_HUD_PREF_KEY_NAME, (int)EWHudMode::Hud_On));
}

void CEWHandler::SetPlayerHudMode(CPlayerSlot slot, EWHudMode mode)
{
	g_pUserPreferencesSystem->SetPreferenceInt(slot.Get(), EW_HUD_PREF_KEY_NAME, (int)mode);
}

void CEWHandler::UnLoadConfig()
{
	if (!bConfigLoaded)
		return;

	//Clantags first so scores can be set back properly
	ResetAllClantags();

	mapItemConfig.Purge();

	FOR_EACH_VEC(vecItems, i)
	{
		FOR_EACH_VEC(vecItems[i]->vecHandlers, j)
		{
			vecItems[i]->vecHandlers[j]->RemoveHook();
		}
	}
	vecItems.Purge();

	RemoveAllTriggers();

	bConfigLoaded = false;
}

/*
 *  Load a config file by mapname
 */
void CEWHandler::LoadMapConfig(const char* sMapName)
{
	const char* pszMapConfigPath = "addons/cs2fixes/configs/entwatch/maps/";

	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "%s%s.jsonc", pszMapConfigPath, sMapName);

	LoadConfig(szPath);
}

/*
 * Load a config file from /csgo/ folder
 */
void CEWHandler::LoadConfig(const char* sFilePath)
{
	UnLoadConfig();

	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "%s%s%s", Plat_GetGameDirectory(), "/csgo/", sFilePath);

	std::ifstream jsoncFile(szPath);

	if (!jsoncFile.is_open())
	{
		Message("Failed to open %s\n", sFilePath);
		return;
	}

	ordered_json jsonItems = ordered_json::parse(jsoncFile, nullptr, true, true);

	for (auto& [szItemName, jsonItemData] : jsonItems.items())
	{
		if (!jsonItemData.contains("hammerid"))
		{
			Panic("[EntWatch] Item without a hammerid\n");
			continue;
		}

		std::string sHammerid = jsonItemData["hammerid"].get<std::string>();
		EWItem* item = new EWItem(jsonItemData);

		mapItemConfig.Insert(hash_32_fnv1a_const(sHammerid.c_str()), item);
	}
	bConfigLoaded = true;
}

void CEWHandler::PrintLoadedConfig(CPlayerSlot slot)
{
	CCSPlayerController* player = CCSPlayerController::FromSlot(slot);
	if (!bConfigLoaded)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "No config loaded.");
		return;
	}

	FOR_EACH_MAP(mapItemConfig, i)
	{
		EWItem* item = mapItemConfig.Element(i);
		if (!item)
		{
			ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "Null item int the item map at pos %d", i);
			continue;
		}

		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "------------ Item %02d ------------", i);
		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "     Name:  %s", item->szItemName.c_str());
		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "ShortName:  %s", item->szShortName.c_str());
		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX " Hammerid:  %s", item->szHammerid.c_str());
		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "  Message:  %s", item->bShowPickup ? "True" : "False");
		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "       UI:  %s", item->bShowHud ? "True" : "False");
		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX " Transfer:  %s", item->bTransfer ? "True" : "False");

		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX " ");
		if (item->vecHandlers.Count() == 0)
		{
			ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "        No handlers set.");
		}
		else
		{
			FOR_EACH_VEC(item->vecHandlers, j)
			{
				// "          "
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "          --------- Handler %d ---------", j);
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "              Type:  %s", item->vecHandlers[j]->type == EWItemHandlerType::Button ? "Button" : "GameUi");
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "              Mode:  %d", (int)item->vecHandlers[j]->mode);
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "          Hammerid:  %s", item->vecHandlers[j]->szHammerid.c_str());
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "             Event:  %s", item->vecHandlers[j]->szOutput.c_str());
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "          Cooldown:  %d", item->vecHandlers[j]->iCooldown);
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "          Max Uses:  %d", item->vecHandlers[j]->iMaxuses);
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "           Message:  %s", item->vecHandlers[j]->bShowUse ? "True" : "False");
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "          --------- --------- ---------");
			}
		}

		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX " ");
		if (item->vecTriggers.Count() == 0)
		{
			ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "       No triggers set.");
		}
		else
		{
			FOR_EACH_VEC(item->vecTriggers, j)
			{
				ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX " Trigger %d:  %s", j, item->vecTriggers[j]->c_str());
			}
		}

		ClientPrint(player, HUD_PRINTCONSOLE, EW_PREFIX "------------ ------- ------------");
	}
	ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "See console for output.");
}

void CEWHandler::ClearItems()
{
	vecItems.Purge();
}

int CEWHandler::FindItemIdByWeapon(std::string sHammerid)
{
	uint16 i = mapItemConfig.Find(hash_32_fnv1a_const(sHammerid.c_str()));
	//Message("Finding... %d\n", i);
	if (!mapItemConfig.IsValidIndex(i))
		return -1;
	return (int)i;
}

int CEWHandler::FindItemInstanceByWeapon(int iWeaponEnt)
{
	FOR_EACH_VEC(vecItems, i)
	{
		if (vecItems[i]->iWeaponEnt == iWeaponEnt)
			return i;
	}
	return -1;
}

int CEWHandler::FindItemInstanceByOwner(int iOwnerSlot, bool bOnlyTransferrable, int iStartItem)
{
	for (int i = iStartItem; i < vecItems.Count(); i++)
	{
		if (bOnlyTransferrable && !vecItems[i]->bTransfer)
			continue;

		if (vecItems[i]->iOwnerSlot == iOwnerSlot)
			return i;
	}
	return -1;
}

int CEWHandler::FindItemInstanceByName(std::string sItemName, bool bOnlyTransferrable)
{
	std::string lowercaseInput = "";
	for (char ch : sItemName) {
		lowercaseInput += std::tolower(ch);
	}

	FOR_EACH_VEC(vecItems, i)
	{
		if (bOnlyTransferrable && !vecItems[i]->bTransfer)
			continue;

		std::string itemname = "";

		// Short name first
		for (char ch : vecItems[i]->szShortName) {
			itemname += std::tolower(ch);
		}

		if (itemname == lowercaseInput)
			return i;

		// long name
		itemname = "";
		for (char ch : vecItems[i]->szItemName) {
			itemname += std::tolower(ch);
		}

		if (itemname == lowercaseInput)
			return i;
	}
	return -1;
}

void CEWHandler::RegisterHandler(CBaseEntity* pEnt, EWItemHandlerType entType)
{
	FOR_EACH_VEC(vecItems, i)
	{
		if (vecItems[i]->RegisterHandler(pEnt, entType))
		{
			Message("REGISTERED HANDLER. Instance:%d  entindex:%d  type:%d\n", i + 1, pEnt->entindex(), (int)entType);
			return;
		}
	}
}

bool CEWHandler::RegisterTrigger(CBaseEntity* pEnt)
{
	FOR_EACH_MAP_FAST(mapItemConfig, i)
	{
		EWItem* pItem = mapItemConfig.Element(i);
		if (pItem->vecTriggers.Count() < 1)
			continue;

		const char* sHammerid = pEnt->m_sUniqueHammerID().Get();
		FOR_EACH_VEC(pItem->vecTriggers, j)
		{
			if (strcmp(sHammerid, pItem->vecTriggers[j]->c_str()))
				continue;

			AddTouchHook(pEnt);

			return true;
		}
	}
	return false;
}

void CEWHandler::AddTouchHook(CBaseEntity* pEnt)
{
	if (vecHookedTriggers.Count() <= 0)
	{
		static int startOffset = g_GameConfig->GetOffset("CBaseEntity::StartTouch");
		SH_MANUALHOOK_RECONFIGURE(CBaseEntity_StartTouch, startOffset, 0, 0);
		iStartTouchHookId = SH_ADD_MANUALVPHOOK(CBaseEntity_StartTouch, pEnt, SH_MEMBER(this, &CEWHandler::Hook_Touch), false);

		static int touchOffset = g_GameConfig->GetOffset("CBaseEntity::Touch");
		SH_MANUALHOOK_RECONFIGURE(CBaseEntity_Touch, touchOffset, 0, 0);
		iTouchHookId = SH_ADD_MANUALVPHOOK(CBaseEntity_Touch, pEnt, SH_MEMBER(this, &CEWHandler::Hook_Touch), false);

		static int endOffset = g_GameConfig->GetOffset("CBaseEntity::EndTouch");
		SH_MANUALHOOK_RECONFIGURE(CBaseEntity_EndTouch, endOffset, 0, 0);
		iEndTouchHookId = SH_ADD_MANUALVPHOOK(CBaseEntity_EndTouch, pEnt, SH_MEMBER(this, &CEWHandler::Hook_Touch), false);
	}

	vecHookedTriggers.AddToTail(pEnt->GetHandle());
}

void CEWHandler::Hook_Touch(CBaseEntity* pOther)
{
	CBaseEntity* pEntity = META_IFACEPTR(CBaseEntity);
	if (!pEntity)
		RETURN_META(MRES_IGNORED);

	bool bFound = false;
	FOR_EACH_VEC(vecHookedTriggers, i)
	{
		if (vecHookedTriggers[i] == pEntity->GetHandle())
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
		RETURN_META(MRES_IGNORED);

	CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pOther;
	if (!pPawn)
		RETURN_META(MRES_IGNORED);

	CCSPlayerController* pController = pPawn->GetOriginalController();
	if (!pController)
		RETURN_META(MRES_IGNORED);

	if (m_bEbanned[pController->GetPlayerSlot()])
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	RETURN_META(MRES_IGNORED);
}

bool CEWHandler::RemoveTrigger(CBaseEntity* pEnt)
{
	FOR_EACH_VEC(vecHookedTriggers, i)
	{
		if (vecHookedTriggers[i] == pEnt->GetHandle())
		{
			vecHookedTriggers.Remove(i);
			if (vecHookedTriggers.Count() <= 0)
			{
				Message("[EntWatch] Fully unhooking touch hooks\n");
				SH_REMOVE_HOOK_ID(iStartTouchHookId);
				SH_REMOVE_HOOK_ID(iTouchHookId);
				SH_REMOVE_HOOK_ID(iEndTouchHookId);
				iStartTouchHookId = -1;
				iTouchHookId = -1;
				iEndTouchHookId = -1;
			}
			return true;
		}
	}
	return false;
}

void CEWHandler::RemoveAllTriggers()
{
	Message("[EntWatch] Fully unhooking touch hooks\n");
	SH_REMOVE_HOOK_ID(iStartTouchHookId);
	SH_REMOVE_HOOK_ID(iTouchHookId);
	SH_REMOVE_HOOK_ID(iEndTouchHookId);
	iStartTouchHookId = -1;
	iTouchHookId = -1;
	iEndTouchHookId = -1;
	vecHookedTriggers.Purge();
}

void CEWHandler::RemoveHandler(CBaseEntity* pEnt)
{
	FOR_EACH_VEC(vecItems, i)
	{
		if (vecItems[i]->RemoveHandler(pEnt))
			return;
	}
}

void CEWHandler::ResetAllClantags()
{
	// Reset item holders scores to what they should be
	if (g_iItemHolderScore != 0)
	{
		FOR_EACH_VEC(vecItems, i)
		{
			if (vecItems[i]->bHasThisClantag)
			{
				CCSPlayerController* pOwner = CCSPlayerController::FromSlot(CPlayerSlot(vecItems[i]->iOwnerSlot));
				if (!pOwner)
					continue;

				int score = pOwner->m_iScore - g_iItemHolderScore;
				pOwner->m_iScore = score;
				vecItems[i]->bHasThisClantag = false;
			}
		}
	}


	// Reset everyone's scores and tags for insurance
	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CCSPlayerController* pController = CCSPlayerController::FromSlot(i);
		if (!pController)
			continue;

		// Bring score down below g_iItemHolderScore so new item holders show above
		if (pController->m_iScore >= g_iItemHolderScore)
		{
			int score = pController->m_iScore % g_iItemHolderScore;
			pController->m_iScore = score;
		}

		pController->m_szClan("");
	}

	EW_SendBeginNewMatchEvent();
}

void CEWHandler::RegisterItem(int itemId, CBasePlayerWeapon* pWeapon)
{
	Message("Registering item %d (item instance:%d)\n", itemId, vecItems.Count() + 1);
	EWItem* item = mapItemConfig.Element(itemId);

	EWItemInstance* instance = new EWItemInstance(pWeapon->entindex(), item);

	V_snprintf(instance->sClantag, sizeof(EWItemInstance::sClantag), "[+]%s:", instance->szShortName.c_str());

	bool bKnife = pWeapon->GetWeaponVData()->m_GearSlot() == GEAR_SLOT_KNIFE;
	instance->bAllowDrop = !bKnife;

	vecItems.AddToTail(instance);
}

/* Player picked up a weapon in the config */
void CEWHandler::PlayerPickup(CCSPlayerPawn* pPawn, CBasePlayerWeapon* pPlayerWeapon)
{
	FOR_EACH_VEC(vecItems, i)
	{
		if (vecItems[i]->iWeaponEnt == -1)
		{
			Message("Invalid weapon ent in item instance %d!\n", i);
			continue;
		}

		if (pPlayerWeapon->entindex() != vecItems[i]->iWeaponEnt)
			continue;

		vecItems[i]->Pickup(pPawn->m_hOriginalController->GetPlayerSlot());

		/*
		if (!m_bHudTicking)
		{
			m_bHudTicking = true;
			new CTimer(0.5f, false, false, []
				{
					return EW_UpdateHud();
				});
		}
		*/
	}
}

void CEWHandler::PlayerDrop(EWDropReason reason, int iItemInstance, CCSPlayerController* pController)
{
	if (!pController)
		return;

	// Drop is only called when ONE item is being dropped by choice
	// Death and disconnect drop all items currently owned
	if (reason == EWDropReason::Drop)
	{
		if (iItemInstance == -1 || iItemInstance >= vecItems.Count())
			return;

		EWItemInstance* pItem = vecItems[iItemInstance];
		if (!pItem)
			return;

		if (pItem->iOwnerSlot != pController->GetPlayerSlot())
			return;

		pItem->Drop(reason, pController);
	}
	else
	{
		// Find all items owned by this player
		FOR_EACH_VEC(vecItems, i)
		{
			if (vecItems[i]->iOwnerSlot != pController->GetPlayerSlot())
				continue;

			vecItems[i]->Drop(reason, pController);
		}
	}
}

void CEWHandler::AddUseHook(CBaseEntity* pEnt)
{
	if (vecUseHookedEntities.Count() <= 0)
	{
		static int offset = g_GameConfig->GetOffset("CBaseEntity::Use");
		SH_MANUALHOOK_RECONFIGURE(CBaseEntity_Use, offset, 0, 0);
		iUseHookId = SH_ADD_MANUALVPHOOK(CBaseEntity_Use, pEnt, SH_MEMBER(this, &CEWHandler::Hook_Use), false);
	}

	vecUseHookedEntities.AddToTail(pEnt->GetHandle());
}

void CEWHandler::RemoveUseHook(CBaseEntity* pEnt)
{
	FOR_EACH_VEC(vecUseHookedEntities, i)
	{
		if (vecUseHookedEntities[i] == pEnt->GetHandle())
		{
			vecUseHookedEntities.Remove(i);
			if (vecUseHookedEntities.Count() <= 0)
			{
				Message("[EntWatch] Fully unhooking use hook\n");
				SH_REMOVE_HOOK_ID(iUseHookId);
				iUseHookId = -1;
			}
			return;
		}
	}
}

void CEWHandler::Hook_Use(InputData_t* pInput)
{
	//Message("#####    USE HOOK    #####\n");
	CBaseEntity* pEntity = META_IFACEPTR(CBaseEntity);
	if (!pEntity)
		RETURN_META(MRES_IGNORED);

	bool bFound = false;
	FOR_EACH_VEC(vecUseHookedEntities, i)
	{
		if (vecUseHookedEntities[i] == pEntity->GetHandle())
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
		RETURN_META(MRES_IGNORED);

	int index = pEntity->entindex();
	int itemIndex = -1;
	int handlerIndex = -1;
	FOR_EACH_VEC(vecItems, i)
	{
		int j = vecItems[i]->FindHandlerByEntIndex(index);
		if (j != -1)
		{
			itemIndex = i;
			handlerIndex = j;
			break;
		}
	}

	//Message("Used:%d   Found[item:%d  handler:%d]\n", index, itemIndex, handlerIndex);
	if (itemIndex == -1 || handlerIndex == -1)
		RETURN_META(MRES_IGNORED);

	// Prevent uses from non item owners if we are filtering
	META_RES resVal = MRES_IGNORED;
	if (g_bEnableFiltering)
		resVal = MRES_SUPERCEDE;

	CBaseEntity* pActivator = pInput->pActivator;

	if (!pActivator || !pActivator->IsPawn())
		RETURN_META(resVal);

	EWItemInstance* pItem = vecItems[itemIndex];
	CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pActivator;
	CCSPlayerController* pController = pPawn->GetOriginalController();

	//Message("presser:%d   owner:%d\n", pController->GetPlayerSlot(), pItem->iOwnerSlot);
	if (!pController || pController->GetPlayerSlot() != pItem->iOwnerSlot)
		RETURN_META(resVal);


	const char* classname = pEntity->GetClassname();

	if (!strcmp(classname, "func_button") ||
		!strcmp(classname, "func_rot_button") ||
		!strcmp(classname, "momentary_rot_button"))
	{
		CBaseButton* pButton = (CBaseButton*)pEntity;
		if (pButton->m_bLocked || pButton->m_bDisabled)
			RETURN_META(resVal);
	}

	//
	// WE SHOW USE MESSAGE IN FireOutput
	// This is just to prevent unnecessary shit with buttons like movement
	//
	// TODO: move this to FireOutput
	//if (vecItems[itemIndex]->vecHandlers[handlerIndex]->bShowUse)
		//ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "\x04%s \x05used %s%s", pController->GetPlayerName(), vecItems[itemIndex]->sChatColor, vecItems[itemIndex]->szItemName);

	Message("[EntWatch] %s used %s (weaponid: %d)\n", pController->GetPlayerName(), pItem->szItemName.c_str(), pItem->iWeaponEnt);
	RETURN_META(MRES_IGNORED);
}

// Update cd and uses of all held items
#if 0
float EW_UpdateHud()
{
	if (g_bFixHudFlashing && !g_pGameRules->m_bGameRestart)
		return 0.5f;

	char sHudTextNoPlayerNames[256] = "";
	char sHudText[256] = "";
	bool bFirst = true;
	FOR_EACH_VEC(g_pEWHandler->vecItems, i)
	{
		EWItemInstance* pItem = g_pEWHandler->vecItems[i];
		if (!pItem)
			continue;

		if (pItem->iOwnerSlot == -1 || pItem->iWeaponEnt == -1)
			continue;

		if (!pItem->bShowHud)
			continue;

		CCSPlayerController* pOwner = CCSPlayerController::FromSlot(CPlayerSlot(pItem->iOwnerSlot));
		if (!pOwner)
			continue;

		if (bFirst)
		{
			bFirst = false;
			V_snprintf(sHudText, sizeof(sHudText), "%s [+]: %s", pItem->szShortName.c_str(), pOwner->GetPlayerName());
			V_snprintf(sHudTextNoPlayerNames, sizeof(sHudTextNoPlayerNames), "%s [+]", pItem->szShortName.c_str());
		}
		else
		{
			V_snprintf(sHudText, sizeof(sHudText), "%s<br>%s [+]: %s", sHudText, pItem->szShortName.c_str(), pOwner->GetPlayerName());
			V_snprintf(sHudTextNoPlayerNames, sizeof(sHudTextNoPlayerNames), "%s<br>%s [+]", sHudTextNoPlayerNames, pItem->szShortName.c_str());
		}

	}

	if (sHudText[0] == '\0')
		return 0.5f;

	CRecipientFilter filterHud;
	CRecipientFilter filterHudNoPlayerNames;

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CCSPlayerController* pController = CCSPlayerController::FromSlot(i);
		if (!pController)
			continue;
		auto pPawn = pController->GetPawn();
		if (!pPawn)
			continue;

		EWHudMode mode = g_pEWHandler->GetPlayerHudMode(CPlayerSlot(i));
		if (mode == EWHudMode::Hud_On)
			filterHud.AddRecipient(CPlayerSlot(i));
		else if (mode == EWHudMode::Hud_ItemOnly)
			filterHudNoPlayerNames.AddRecipient(CPlayerSlot(i));
	}

	IGameEvent* pEvent = g_gameEventManager->CreateEvent("show_survival_respawn_status");
	if (!pEvent)
	{
		Panic("Failed to create show_survival_respawn_status event\n");
		return 0.5f;
	}
	pEvent->SetString("loc_token", sHudText);
	pEvent->SetInt("duration", 1);
	pEvent->SetInt("userid", -1);

	if (filterHud.GetRecipientCount() > 0)
	{
		INetworkMessageInternal* pMsg = g_pNetworkMessages->FindNetworkMessageById(GE_Source1LegacyGameEvent);
		if (!pMsg)
		{
			Panic("Failed to create Source1LegacyGameEvent\n");
			return 0.5f;
		}
		CNetMessagePB<CMsgSource1LegacyGameEvent>* data = pMsg->AllocateMessage()->ToPB<CMsgSource1LegacyGameEvent>();
		g_gameEventManager->SerializeEvent(pEvent, data);
		g_gameEventSystem->PostEventAbstract(-1, false, &filterHud, pMsg, data, 0);
		delete data;
	}

	if (filterHudNoPlayerNames.GetRecipientCount() > 0)
	{
		INetworkMessageInternal* pMsg = g_pNetworkMessages->FindNetworkMessageById(GE_Source1LegacyGameEvent);
		if (!pMsg)
		{
			Panic("Failed to create Source1LegacyGameEvent\n");
			return 0.5f;
		}
		pEvent->SetString("loc_token", sHudTextNoPlayerNames);
		CNetMessagePB<CMsgSource1LegacyGameEvent>* data = pMsg->AllocateMessage()->ToPB<CMsgSource1LegacyGameEvent>();
		g_gameEventManager->SerializeEvent(pEvent, data);
		g_gameEventSystem->PostEventAbstract(-1, false, &filterHudNoPlayerNames, pMsg, data, 0);
		delete data;
	}

	g_gameEventManager->FreeEvent(pEvent);

	return 0.5f;
}
#endif

void EW_OnLevelInit(const char* sMapName)
{
	g_pEWHandler->UnLoadConfig();
	g_pEWHandler->LoadMapConfig(sMapName);
}

void EW_RoundPreStart()
{
	if (!g_pEWHandler)
		return;

	g_pEWHandler->ResetAllClantags();
	g_pEWHandler->ClearItems();
	g_pEWHandler->m_bHudTicking = false;
}

void EW_OnEntitySpawned(CEntityInstance* pEntity)
{
	if (!g_pEWHandler || !g_pEWHandler->bConfigLoaded)
		return;

	CBaseEntity* pEnt = (CBaseEntity*)pEntity;
	if (!V_strcmp(pEnt->m_sUniqueHammerID().Get(), ""))
		return;

	const char* classname = pEnt->GetClassname();
	if (!strncmp(classname, "weapon_", 7))
	{
		int i = g_pEWHandler->FindItemIdByWeapon(pEnt->m_sUniqueHammerID.Get().String());
		if (i != -1)
		{
			g_pEWHandler->RegisterItem(i, (CBasePlayerWeapon*)pEntity);
		}
		return;
	}

	if (!strncmp(classname, "trigger_", 8))
		if (g_pEWHandler->RegisterTrigger(pEnt))
			return;

	// dont check lights
	if (!strncmp(classname, "light_", 6))
		return;

	EWItemHandlerType type = EWItemHandlerType::Type_None;
	if (!strcmp(classname, "func_button") ||
		!strcmp(classname, "func_rot_button") ||
		!strcmp(classname, "func_door") ||
		!strcmp(classname, "func_physbox") || //prop_physics & physbox can have OnPlayerUse output
		!strncmp(classname, "prop_physics", 12) ||
		!strcmp(classname, "momentary_rot_button"))
		type = EWItemHandlerType::Button; // if someone uses a different ent as button just shoot me
	else if (pEnt->AsGameUI() != nullptr)
		type = EWItemHandlerType::GameUi;
	else if (!strcmp(classname, "math_counter"))
		type = EWItemHandlerType::Counter;
	else
		type = EWItemHandlerType::Other;

	if (type != EWItemHandlerType::Type_None)
	{
		// delay it cuz stupid spawn orders
		CHandle<CBaseEntity> hEntity = pEnt->GetHandle();
		new CTimer(0.5, false, false, [hEntity, type] {
			if (hEntity.Get())
				g_pEWHandler->RegisterHandler(hEntity.Get(), type);
			return -1.0;
			});

	}

}

void EW_OnEntityDeleted(CEntityInstance* pEntity)
{
	if (!g_pEWHandler || !g_pEWHandler->bConfigLoaded)
		return;

	CBaseEntity* pEnt = (CBaseEntity*)pEntity;
	if (!V_strcmp(pEnt->m_sUniqueHammerID().Get(), ""))
		return;

	const char* classname = pEnt->GetClassname();
	if (!strncmp(classname, "weapon_", 7))
	{
		EW_OnWeaponDeleted(pEnt);
		return;
	}

	if (!strncmp(classname, "trigger_", 8))
		if (g_pEWHandler->RemoveTrigger(pEnt))
			return;

	// dont check lights
	if (!strncmp(classname, "light_", 6))
		return;

	g_pEWHandler->RemoveHandler(pEnt);
}

void EW_OnWeaponDeleted(CBaseEntity* pEntity)
{
	//TODO: check if this weapon is in the config
	//		if it is, remove it's instance from the list
	// we know it has a hammerid here
}

bool EW_Detour_CCSPlayer_WeaponServices_CanUse(CCSPlayer_WeaponServices* pWeaponServices, CBasePlayerWeapon* pPlayerWeapon)
{
	// false=block it, true=dont block it
	if (!g_pEWHandler || !g_pEWHandler->bConfigLoaded)
		return true;

	CCSPlayerPawn* pPawn = pWeaponServices->__m_pChainEntity();
	if (!pPawn)
		return true;

	// We don't care about weapons that don't have a hammerid
	if (!V_strcmp(pPlayerWeapon->m_sUniqueHammerID().Get(), ""))
		return true;

	//TODO Eban check
	if (g_pEWHandler->m_bEbanned[pPawn->GetOriginalController()->GetPlayerSlot()])
		return false;

	return true;
}

void EW_Detour_CCSPlayer_WeaponServices_EquipWeapon(CCSPlayer_WeaponServices* pWeaponServices, CBasePlayerWeapon* pPlayerWeapon)
{
	if (!g_pEWHandler || !g_pEWHandler->bConfigLoaded)
		return;

	CCSPlayerPawn* pPawn = pWeaponServices->__m_pChainEntity();
	if (!pPawn)
		return;

	// We don't care about weapons that don't have a hammerid
	if (!V_strcmp(pPlayerWeapon->m_sUniqueHammerID().Get(), ""))
		return;

	std::string hammerid = pPlayerWeapon->m_sUniqueHammerID.Get().String();
	int i = g_pEWHandler->FindItemIdByWeapon(hammerid);
	if (i == -1)
		return;

	g_pEWHandler->PlayerPickup(pPawn, pPlayerWeapon);
}

void EW_DropWeapon(CCSPlayer_WeaponServices* pWeaponServices, CBasePlayerWeapon* pWeapon)
{
	if (!g_pEWHandler || !g_pEWHandler->bConfigLoaded)
		return;

	CCSPlayerPawn* pPawn = pWeaponServices->__m_pChainEntity();
	if (!pPawn)
		return;

	// We don't care about weapons that don't have a hammerid
	if (!V_strcmp(pWeapon->m_sUniqueHammerID().Get(), ""))
		return;

	// Check if this weapon is in the config
	int i = g_pEWHandler->FindItemInstanceByWeapon(pWeapon->entindex());
	if (i == -1)
		return;

	g_pEWHandler->vecItems[i]->bDropping = true;
	CHandle<CCSPlayerController> hController = pPawn->m_hOriginalController;

	new CTimer(0.0, false, false, [i, hController] {
		if (i < g_pEWHandler->vecItems.Count())
		{
			if (g_pEWHandler->vecItems[i]->bDropping && hController.Get())
				g_pEWHandler->PlayerDrop(EWDropReason::Drop, i, hController.Get());
		}

		return -1.0;
		});
}

void EW_PlayerDeath(IGameEvent* pEvent)
{
	CCSPlayerController* pVictim = (CCSPlayerController*)pEvent->GetPlayerController("userid");
	if (!pVictim)
		return;

	CHandle<CCSPlayerController> hController = pVictim->GetHandle();

	// Death message should show over drop messages
	new CTimer(0.0, false, false, [hController] {
		if (hController.Get())
			g_pEWHandler->PlayerDrop(EWDropReason::Death, -1, hController.Get());
		return -1.0;
		});

}

void EW_PlayerDisconnect(int slot)
{
	g_pEWHandler->m_bEbanned[slot] = false;

	CCSPlayerController* pController = CCSPlayerController::FromSlot(slot);
	if (!pController)
		return;

	// Disconnect message should show over death and drop messages
	g_pEWHandler->PlayerDrop(EWDropReason::Disconnect, -1, pController);
}

void EW_SendBeginNewMatchEvent()
{
	IGameEvent* pEvent = g_gameEventManager->CreateEvent("begin_new_match");
	if (!pEvent)
	{
		Panic("Failed to create begin_new_match event\n");
		return;
	}

	INetworkMessageInternal* pMsg = g_pNetworkMessages->FindNetworkMessageById(GE_Source1LegacyGameEvent);
	if (!pMsg)
	{
		Panic("Failed to create Source1LegacyGameEvent\n");
		return;
	}
	CNetMessagePB<CMsgSource1LegacyGameEvent>* data = pMsg->AllocateMessage()->ToPB<CMsgSource1LegacyGameEvent>();
	g_gameEventManager->SerializeEvent(pEvent, data);

	CRecipientFilter filter;
	filter.AddAllPlayers();
	g_gameEventSystem->PostEventAbstract(-1, false, &filter, pMsg, data, 0);
	delete data;
}

// Hud command
#if 0
CON_COMMAND_CHAT(hud, "Toggle EntWatch HUD")
{
	if (!g_bEnableEntWatch)
		return;

	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Only usable in game.");
		return;
	}

	CPlayerSlot slot = CPlayerSlot(player->GetPlayerSlot());
	ZEPlayer* pPlayer = g_playerManager->GetPlayer(slot);
	if (!pPlayer)
	{
		Message(EW_PREFIX "Failed to get player to toggle hud.");
		return;
	}

	EWHudMode mode = (EWHudMode)(g_pEWHandler->GetPlayerHudMode(slot));
	if (mode == EWHudMode::Hud_None)
		mode = EWHudMode::Hud_On;
	else if (mode == EWHudMode::Hud_On)
		mode = EWHudMode::Hud_ItemOnly;
	else
		mode = EWHudMode::Hud_None;

	g_pEWHandler->SetPlayerHudMode(slot, mode);

	if (mode == EWHudMode::Hud_None)
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "EntWatch HUD\x07 disabled.");
	else if (mode == EWHudMode::Hud_On)
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "EntWatch HUD\x04 enabled.");
	else
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "EntWatch HUD\x04 enabled\x10 (Item names only).");
}
#endif

CON_COMMAND_CHAT_FLAGS(ew_reload, "Reloads the current map's entwatch config", ADMFLAG_CONFIG)
{
	if (!g_bEnableEntWatch)
		return;
	if (!g_pEWHandler)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "There has been an error initialising entwatch.");
		return;
	}

	// LoadConfig unloads the current config already
	g_pEWHandler->LoadMapConfig(gpGlobals->mapname.ToCStr());

	if (!g_pEWHandler->bConfigLoaded)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Error reloading config, check console log for details.");
		return;
	}

	ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Config reloaded successfully.");
}

CON_COMMAND_CHAT_FLAGS(eban, "Ban a player from picking up items", ADMFLAG_BAN)
{
	if (!g_bEnableEntWatch)
		return;

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Usage: !eban <player>");
		return;
	}

	// TODO: duration argument

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_TARGET_BLOCKS, nType))
	{
		return;
	}

	const char* pszCommandPlayerName = player ? player->GetPlayerName() : CONSOLE_NAME;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		g_pEWHandler->m_bEbanned[pSlots[i]] = true;

		if (iNumClients == 1)
			PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "ebanned", "", EW_PREFIX);
	}

	if (iNumClients > 1)
		PrintMultiAdminAction(nType, pszCommandPlayerName, "ebanned", "", EW_PREFIX);
}

CON_COMMAND_CHAT_FLAGS(eunban, "Unban a player from picking up items", ADMFLAG_BAN)
{
	if (!g_bEnableEntWatch)
		return;

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Usage: !eunban <player>");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_TARGET_BLOCKS, nType))
	{
		return;
	}

	const char* pszCommandPlayerName = player ? player->GetPlayerName() : CONSOLE_NAME;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		g_pEWHandler->m_bEbanned[pSlots[i]] = false;

		if (iNumClients == 1)
			PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "eunbanned", "", EW_PREFIX);
	}

	if (iNumClients > 1)
		PrintMultiAdminAction(nType, pszCommandPlayerName, "eunbanned", "", EW_PREFIX);
}

bool g_bTransferForceEquip = false;
FAKE_BOOL_CVAR(entwatch_transfer_forceequip, "Whether to force call EquipWeapon when transferring entwatch items", g_bTransferForceEquip, false, false);

CON_COMMAND_CHAT_FLAGS(etransfer, "Transfer an EntWatch item", ADMFLAG_GENERIC)
{
	if (!g_bEnableEntWatch)
		return;

	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Usage: !etransfer <owner>/$<itemname> <receiver>");
		return;
	}

	const char* pszCommandPlayerName = player ? player->GetPlayerName() : "Console";

	int iItemInstance = -1;
	bool bTransferFromPlayer = true;

	if (args[1][0] == '$')
	{
		char sItemName[64];

		bTransferFromPlayer = false;
		V_strcpy(sItemName, args[1] + 1);
		iItemInstance = g_pEWHandler->FindItemInstanceByName(sItemName, true);

		if (iItemInstance == -1)
		{
			ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Could not find an item with the name:\x04 %s", sItemName);
			return;
		}
		if (g_pEWHandler->vecItems[iItemInstance]->iOwnerSlot != -1)
		{
			bTransferFromPlayer = true;
		}
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	// Find receiver
	if (!g_playerManager->CanTargetPlayers(player, args[2], iNumClients, pSlots, NO_MULTIPLE | NO_SPECTATOR | NO_DEAD))
	{
		return;
	}

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Receiver not found.");
		return;
	}

	if (g_pEWHandler->m_bEbanned[pSlots[0]])
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Specified receiver is ebanned.");
		return;
	}

	CCSPlayerController* pReceiver = CCSPlayerController::FromSlot(pSlots[0]);
	if (!pReceiver)
		return;
	CCSPlayerPawn* pReceiverPawn = pReceiver->GetPlayerPawn();
	if (!pReceiverPawn || !pReceiverPawn->m_pWeaponServices)
		return;

	// Player to player transfer
	if (bTransferFromPlayer)
	{
		iNumClients = 0;
		CCSPlayerController* pOwner;
		if (iItemInstance == -1)
		{
			if (!g_playerManager->CanTargetPlayers(player, args[2], iNumClients, pSlots, NO_MULTIPLE | NO_SPECTATOR | NO_DEAD))
			{
				return;
			}
			if (!iNumClients)
			{
				ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "Item owner not found.");
				return;
			}
			pOwner = CCSPlayerController::FromSlot(pSlots[0]);
		}
		else
		{
			pOwner = CCSPlayerController::FromSlot(CPlayerSlot(g_pEWHandler->vecItems[iItemInstance]->iOwnerSlot));
		}

		if (!pOwner)
			return;
		CCSPlayerPawn* pOwnerPawn = pOwner->GetPlayerPawn();
		if (!pOwnerPawn || !pOwnerPawn->m_pWeaponServices)
			return;

		if (iItemInstance == -1)
		{
			iItemInstance = g_pEWHandler->FindItemInstanceByOwner(pOwner->GetPlayerSlot(), true, 0);
			if (iItemInstance == -1)
			{
				ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "%s does not have an item that can be transferred.", pOwner->GetPlayerName());
				return;
			}
		}

		CBasePlayerWeapon* pItemWeapon = (CBasePlayerWeapon*)g_pEntitySystem->GetEntityInstance((CEntityIndex)g_pEWHandler->vecItems[iItemInstance]->iWeaponEnt);
		gear_slot_t itemSlot = pItemWeapon->GetWeaponVData()->m_GearSlot();

		// Make current item owner drop the item weapon
		CUtlVector<CHandle<CBasePlayerWeapon>>* weapons = pOwnerPawn->m_pWeaponServices()->m_hMyWeapons();
		FOR_EACH_VEC(*weapons, i)
		{
			CBasePlayerWeapon* pWeapon = (*weapons)[i].Get();

			if (!pWeapon)
				continue;

			if (pWeapon->GetWeaponVData()->m_GearSlot() == itemSlot)
			{
				pOwnerPawn->m_pWeaponServices()->DropWeapon(pWeapon);
				break;
			}
		}

		// Make receiver drop the weapon in the item slot
		weapons = pReceiverPawn->m_pWeaponServices()->m_hMyWeapons();
		FOR_EACH_VEC(*weapons, i)
		{
			CBasePlayerWeapon* pWeapon = (*weapons)[i].Get();

			if (!pWeapon)
				continue;

			if (pWeapon->GetWeaponVData()->m_GearSlot() == itemSlot)
			{
				pReceiverPawn->m_pWeaponServices()->DropWeapon(pWeapon);
				break;
			}
		}

		// Give the item to the receiver
		Vector vecOrigin = pReceiverPawn->GetAbsOrigin();
		pItemWeapon->Teleport(&vecOrigin, nullptr, nullptr);

		if (g_bTransferForceEquip)
			pReceiverPawn->m_pWeaponServices()->EquipWeapon(pItemWeapon);

		ZEPlayer* pZEOwner = g_playerManager->GetPlayer(pOwner->GetPlayerSlot());
		char sOwnerInfo[64];
		if (pZEOwner->IsFakeClient())
			V_snprintf(sOwnerInfo, sizeof(sOwnerInfo), "%s [BOT]", pOwner->GetPlayerName());
		else
			V_snprintf(sOwnerInfo, sizeof(sOwnerInfo), "%s [%llu]", pOwner->GetPlayerName(), pZEOwner->GetUnauthenticatedSteamId64());

		ZEPlayer* pZEReceiver = g_playerManager->GetPlayer(pReceiver->GetPlayerSlot());
		char sReceiverInfo[64];
		if (pZEReceiver->IsFakeClient())
			V_snprintf(sReceiverInfo, sizeof(sReceiverInfo), "%s [BOT]", pReceiver->GetPlayerName());
		else
			V_snprintf(sReceiverInfo, sizeof(sReceiverInfo), "%s [%llu]", pReceiver->GetPlayerName(), pZEReceiver->GetUnauthenticatedSteamId64());

		Message("[EntWatch] %s transferred %s from %s to %s\n",
			pszCommandPlayerName,
			g_pEWHandler->vecItems[iItemInstance]->szItemName.c_str(),
			sOwnerInfo,
			sReceiverInfo);

		ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "Admin\x02 %s\x01 has transferred%s %s\x01 from\x02 %s\x01 to\x02 %s\x01.",
			pszCommandPlayerName,
			g_pEWHandler->vecItems[iItemInstance]->sChatColor,
			g_pEWHandler->vecItems[iItemInstance]->szItemName.c_str(),
			pOwner->GetPlayerName(),
			pReceiver->GetPlayerName());
		return;
	}

	// Dropped weapon to player transfer 

	CBasePlayerWeapon* pItemWeapon = (CBasePlayerWeapon*)g_pEntitySystem->GetEntityInstance((CEntityIndex)g_pEWHandler->vecItems[iItemInstance]->iWeaponEnt);
	gear_slot_t itemSlot = pItemWeapon->GetWeaponVData()->m_GearSlot();

	// Make receiver drop weapon in item slot
	CUtlVector<CHandle<CBasePlayerWeapon>>* weapons = pReceiverPawn->m_pWeaponServices()->m_hMyWeapons();
	FOR_EACH_VEC(*weapons, i)
	{
		CBasePlayerWeapon* pWeapon = (*weapons)[i].Get();

		if (!pWeapon)
			continue;

		if (pWeapon->GetWeaponVData()->m_GearSlot() == itemSlot)
		{
			pReceiverPawn->m_pWeaponServices()->DropWeapon(pWeapon);
			break;
		}
	}

	// Give the item to the receiver
	Vector vecOrigin = pReceiverPawn->GetAbsOrigin();
	pItemWeapon->Teleport(&vecOrigin, nullptr, nullptr);

	if (g_bTransferForceEquip)
		pReceiverPawn->m_pWeaponServices()->EquipWeapon(pItemWeapon);

	ZEPlayer* pZEReceiver = g_playerManager->GetPlayer(pReceiver->GetPlayerSlot());
	char sReceiverInfo[64];
	if (pZEReceiver->IsFakeClient())
		V_snprintf(sReceiverInfo, sizeof(sReceiverInfo), "%s [BOT]", pReceiver->GetPlayerName());
	else
		V_snprintf(sReceiverInfo, sizeof(sReceiverInfo), "%s [%llu]", pReceiver->GetPlayerName(), pZEReceiver->GetUnauthenticatedSteamId64());

	Message("[EntWatch] %s transferred %s to %s\n",
		pszCommandPlayerName,
		g_pEWHandler->vecItems[iItemInstance]->szItemName.c_str(),
		sReceiverInfo);

	ClientPrintAll(HUD_PRINTTALK, EW_PREFIX "Admin\x02 %s\x01 has transferred%s %s\x01 to\x02 %s\x01.",
		pszCommandPlayerName,
		g_pEWHandler->vecItems[iItemInstance]->sChatColor,
		g_pEWHandler->vecItems[iItemInstance]->szItemName.c_str(),
		pReceiver->GetPlayerName());
}

CON_COMMAND_CHAT(ew_dump, "Prints the currently loaded config to console")
{
	if (!g_bEnableEntWatch)
		return;

	if (!g_pEWHandler)
	{
		ClientPrint(player, HUD_PRINTTALK, EW_PREFIX "There has been an error initialising entwatch.");
		return;
	}

	g_pEWHandler->PrintLoadedConfig(player->GetPlayerSlot());
}
