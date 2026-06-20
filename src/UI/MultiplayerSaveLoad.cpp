/**
*  yrpp-spawner
*
*  Copyright(C) 2022-present CnCNet
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.If not, see <http://www.gnu.org/licenses/>.
*/

// Mid-game multiplayer save loading. The host opens the stock savegame load
// dialog (from the in-game options menu's Load Game button or the desync
// dialog), and picking a save schedules a synchronized reload for everyone
// instead of loading it locally. See SessionExt::Schedule_Multiplayer_Load /
// Load_Multiplayer_Save (Ext/Session/Body.cpp) for the rest of the mechanism.

#include <Ext/Session/Body.h>

#include <SessionClass.h>
#include <LoadOptionsClass.h>
#include <EventClass.h>
#include <IPXManagerClass.h>
#include <Unsorted.h>

#include <Utilities/Macro.h>

#include <chrono>
#include <windows.h>

/**
 *  Intercept the savegame load dialog's load action.
 *
 *  LoadOptionsClass::Dialog (0x558DD0), in its Load case, is about to load the
 *  selected save at 0x559532 (`call [eax+4]` = Load_File; EBP already points at
 *  the FileEntry's filename, FileEntry+0x100; ESI = LoadOptionsClass*). In a
 *  spawner multiplayer game the host must not load locally - that would desync
 *  everyone. Instead schedule a synchronized reload and tell the other players,
 *  then close the dialog without loading.
 *
 *  The 5-byte hook covers the `call` (3) + `test al, al` (2); returning 0 runs
 *  the original local load (singleplayer, and the spawn-time LoadMission path
 *  which does not go through this dialog, are unaffected). Returning 0x5595A0
 *  takes the dialog's "done" path (field_18 is the clicked button id here, so it
 *  is != -1 and the loop exits, ending the dialog) without performing the load.
 */
DEFINE_HOOK(0x559532, LoadOptionsDialog_InterceptMultiplayerLoad, 0x5)
{
	enum { SkipLoadAndCloseDialog = 0x5595A0 };

	if (!SessionExt::Is_Spawner_Session() || SessionClass::Instance.GameMode != GameMode::LAN)
		return 0; // singleplayer / non-spawner: load normally.

	GET(const char*, filename, EBP); // EBP = FileEntry + 0x100 (the filename).

	// Only the host initiates a load; it schedules + broadcasts. (The Load Game
	// button is host-only, so non-host should not reach here - but if it does,
	// we still skip the local load below to avoid desyncing them.)
	if (SessionClass::Instance.Am_I_Master())
		SessionExt::Schedule_Multiplayer_Load(filename, /* broadcast */ true);

	return SkipLoadAndCloseDialog;
}


/**
 *  List multiplayer saves in the load dialog.
 *
 *  LoadOptionsClass::Load_Dialog (0x5587F0) opens the dialog with the extension
 *  the constructor defaulted to ("SAV", for singleplayer). Multiplayer saves use
 *  ".NET" (Phobos writes SVGM_###.NET), so force that extension in a spawner
 *  multiplayer game - this is what makes the engine's in-game Load Game button
 *  (and the desync dialog's) list the right saves. ECX = LoadOptionsClass*.
 */
DEFINE_HOOK(0x5587F0, LoadOptionsClass_LoadDialog_MultiplayerExtension, 0x7)
{
	GET(LoadOptionsClass*, opts, ECX);

	if (SessionExt::Is_Spawner_Session() && SessionClass::Instance.GameMode == GameMode::LAN)
		opts->Extension = "NET";

	return 0;
}


/**
 *  Returns to Queue_AI_Multiplayer's caller. The freeze hook below fires at the
 *  function's entry, before its prologue, so the stack is just the return
 *  address - a bare `retn` cleanly returns to the main loop.
 */
static __declspec(naked) void QueueAIMultiplayer_ReturnToCaller()
{
	__asm { retn }
}

/**
 *  Freeze the simulation while a multiplayer save load is pending.
 *
 *  When a load has been scheduled, every machine must stop advancing the game
 *  until the load happens, so they all reload from the same point and stay in
 *  sync. Queue_AI_Multiplayer (0x6475F0) drives the multiplayer frame; at its
 *  entry, if a load is pending, clear the command queues and just service the
 *  network (Game::CallBack + IPX) until the countdown elapses, then return to
 *  the main loop - Spawner::After_Main_Loop performs the actual reload.
 *
 *  This is a no-op during a desync, where the desync dialog already halts the
 *  game and runs its own countdown (so this never sees a pending load there).
 *  The hook overwrites `mov eax, Session` (5 bytes); return 0 re-runs it.
 */
DEFINE_HOOK(0x6475F0, QueueAIMultiplayer_FreezeForPendingLoad, 0x5)
{
	if (!SessionExt::PendingMultiplayerSaveLoadTime.has_value())
		return 0;

	EventClass::DoList.Init();
	EventClass::OutList.Init();

	while (std::chrono::steady_clock::now() < *SessionExt::PendingMultiplayerSaveLoadTime) {
		Game::CallBack();
		IPXManagerClass::Instance.Service();
		Sleep(5);
	}

	EventClass::DoList.Init();
	EventClass::OutList.Init();

	return reinterpret_cast<DWORD>(&QueueAIMultiplayer_ReturnToCaller);
}
