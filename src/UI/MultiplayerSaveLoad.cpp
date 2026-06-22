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
 *  Skip the load and take the dialog's "done" path (0x5595A0), discarding the
 *  filename argument that was pushed at 0x55952F for the Load_File call.
 *
 *  Load_File is __thiscall(this, filename) and cleans the pushed argument with
 *  `retn 4`; since we jump over the call, we must pop those 4 bytes ourselves or
 *  the dialog's stack is left unbalanced (and it later returns to garbage). At
 *  0x5595A0 field_18 is the clicked button id (!= -1), so the dialog loop exits
 *  cleanly. `push/retn` jumps there without clobbering any register.
 */
static __declspec(naked) void LoadOptionsDialog_SkipLoad_Stub()
{
	__asm
	{
		add esp, 4
		push 0x5595A0
		retn
	}
}

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
 *  which does not go through this dialog, are unaffected).
 */
DEFINE_HOOK(0x559532, LoadOptionsDialog_InterceptMultiplayerLoad, 0x5)
{
	if (!SessionExt::Is_Spawner_Session() || SessionClass::Instance.GameMode != GameMode::LAN)
		return 0; // singleplayer / non-spawner: load normally.

	GET(const char*, filename, EBP); // EBP = FileEntry + 0x100 (the filename).

	// Only the host initiates a load; it schedules + broadcasts. (The Load Game
	// button is host-only, so non-host should not reach here - but if it does,
	// we still skip the local load below to avoid desyncing them.)
	if (SessionClass::Instance.Am_I_Master())
		SessionExt::Schedule_Multiplayer_Load(filename, /* broadcast */ true);

	return reinterpret_cast<DWORD>(&LoadOptionsDialog_SkipLoad_Stub);
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


// SpecialDialog (0xA8EDA0) is the in-game menu state. The menu loop (Special_Dialog,
// 0x48C920) opens each sub-dialog (game controls, sound, abort...) by switching on
// it. A custom value routes the multiplayer Load Game button through that same loop.
static int& SpecialDialog = *reinterpret_cast<int*>(0xA8EDA0);
static constexpr int SDLG_NONE = 0;
static constexpr int EXT_SDLG_LOAD = 20; // unused by the engine's switch (cases 1-9)

/**
 *  Open the multiplayer Load Game dialog from the in-game menu loop, not nested.
 *
 *  Vanilla `GameOptionsClass::Dialog` handles the Load Game button (1310) by
 *  opening LoadOptionsClass::Dialog *inline*, from inside the button's WM_COMMAND
 *  handler. That handler runs while `Main_Loop` is on the stack (InMainLoop set),
 *  and OwnerDraw::DialogMessageHandler (0x623120) only runs Main_Loop/Call_Back
 *  when InMainLoop is clear - so the nested load dialog's message pump services
 *  neither the simulation nor the network, and the other players time us out and
 *  drop the connection. The engine's other menus (Game Controls, etc.) avoid this
 *  by setting SpecialDialog and returning, letting the top-level Special_Dialog
 *  loop open them. We do the same for Load: set EXT_SDLG_LOAD and take the dialog's
 *  clean exit (0x4F14EC sets the result and returns), then open the load dialog at
 *  the Special_Dialog level below. Singleplayer keeps the vanilla inline behaviour.
 *
 *  Fires only on the 1310 (Load Game) branch (after the `wParam == 1310` check).
 */
DEFINE_HOOK(0x4F135C, GameOptionsDialog_RouteMultiplayerLoadGame, 0x6)
{
	if (!SessionExt::Is_Spawner_Session() || SessionClass::Instance.GameMode != GameMode::LAN)
		return 0; // singleplayer / non-spawner: open the load dialog inline as normal.

	SpecialDialog = EXT_SDLG_LOAD;
	return 0x4F14EC; // pop the saved registers, set the dialog result, retn 10h.
}

/**
 *  Handle EXT_SDLG_LOAD in the in-game menu loop (Special_Dialog switch dispatch,
 *  0x48C960, where EAX = the current SpecialDialog value). Open the load dialog at
 *  this level - where DialogMessageHandler keeps the game/network alive, exactly
 *  like the engine's own menus (Game Controls, etc.).
 *
 *  SpecialDialog is left set to EXT_SDLG_LOAD while the dialog runs: the dialog's
 *  pump runs Main_Loop, and Main_Loop only renders/handles the tactical map when
 *  SpecialDialog == SDLG_NONE. Clearing it early lets the game keep drawing the map
 *  over the (owner-draw) dialog and steal scroll input, leaving the dialog invisible
 *  - exactly how Game Controls stays visible is by keeping SpecialDialog non-zero.
 *  The network still runs, so peers don't time out.
 *
 *  Leave the menu loop via the ENGINE'S OWN code, not a hand-built cross-function
 *  jump (that corrupts the stack and crashes). After the dialog returns, clear
 *  SpecialDialog and set EAX (the switch value) to SDLG_NONE, then `return 0`. The
 *  engine re-executes the overwritten `lea ecx,[eax-1]; cmp ecx,8`: with eax=0,
 *  ecx=0xFFFFFFFF > 8, so `ja 0x48C9B0` (default case), whose vanilla
 *  `cmp eax,ebp; jz 0x48CC37` then exits the loop with the engine's own stack.
 */
DEFINE_HOOK(0x48C960, SpecialDialog_OpenMultiplayerLoadGame, 0x6)
{
	if (SpecialDialog != EXT_SDLG_LOAD)
		return 0; // not ours: run the engine's normal switch.

	{
		LoadOptionsClass opts;
		opts.Extension = "NET";
		opts.LoadDialog();
	}

	SpecialDialog = SDLG_NONE; // menu done
	R->EAX(SDLG_NONE);         // fall through the re-executed switch to the default case's vanilla loop-exit.
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

// Send_Packets (0x649CA0): flush the OutList into the meta-packet and send it.
//   int __fastcall(ConnManClass* net, char* buf, int metasize, int maxahead, int my_sent)
using SendPacketsFunc = int(__fastcall*)(void*, char*, int, int, int);

/**
 *  Freeze the simulation while a multiplayer save load is pending.
 *
 *  When a load has been scheduled, every machine must stop advancing the game
 *  until the load happens, so they all reload from the same point and stay in
 *  sync. Queue_AI_Multiplayer (0x6475F0) drives the multiplayer frame; at its
 *  entry, if a load is pending, clear the command queues, send our frame-sync
 *  ONCE, then just service the network (Game::CallBack + IPX) until the countdown
 *  elapses, then return to the main loop - Spawner::After_Main_Loop performs the
 *  actual reload. Mirrors Vinifera's _Queue_AI_Multiplayer_No_Processing_If_Loading_Save.
 *
 *  The Send_Packets is essential: it tells the other players "I have frozen at
 *  this frame (no further events)", so they can advance to their own freeze point
 *  instead of blocking in Wait_For_Players waiting for our frame - which would
 *  leave them pre-load while we reload, and the post-load handshake would then
 *  reject the state as "scenarios don't match".
 *
 *  This is a no-op during a desync, where the desync dialog already halts the
 *  game and runs its own countdown (so this never sees a pending load there).
 *  The hook overwrites `mov eax, Session` (5 bytes); return 0 re-runs it.
 */
DEFINE_HOOK(0x6475F0, QueueAIMultiplayer_FreezeForPendingLoad, 0x5)
{
	if (!SessionExt::PendingMultiplayerSaveLoadTime.has_value())
		return 0;

	// Don't freeze while an in-game menu / dialog is open (SpecialDialog != 0). The
	// host schedules the load from inside the load dialog, whose own pump runs
	// Main_Loop -> Queue_AI_Multiplayer; freezing there (nested in the dialog's loop,
	// a second Main_Loop on the stack) crashes the host. Defer until the menu closes,
	// so the freeze runs from the main loop - exactly as it does on the clients,
	// which schedule from normal play. (During a desync the game is suspended, so
	// this path is never reached there.)
	if (SpecialDialog != SDLG_NONE)
		return 0;

	// Session networking globals the function itself reads (see the YR
	// Queue_AI_Multiplayer): the meta-packet buffer + size, the max-ahead, our
	// sent-command counter, and the connection manager (Ipx) for LAN/Internet.
	auto* const send_packets = reinterpret_cast<SendPacketsFunc>(0x649CA0);
	void* const net = reinterpret_cast<void*>(0xA8E9C0);                  // IPXManagerClass Ipx
	char* const meta_packet = reinterpret_cast<char*>(0xA8D812);          // Session.MetaPacket
	const int meta_size = *reinterpret_cast<int*>(0xA8DA40);              // Session.MetaSize
	const int max_ahead = *reinterpret_cast<int*>(0xA8B550);             // Session.MaxAhead
	const int my_sent = *reinterpret_cast<unsigned short*>(0xAFA400);    // Queue_AI_Multiplayer::my_sent

	EventClass::DoList.Init();
	EventClass::OutList.Init();

	send_packets(net, meta_packet, meta_size, max_ahead, my_sent);

	while (std::chrono::steady_clock::now() < *SessionExt::PendingMultiplayerSaveLoadTime) {
		Game::CallBack();
		IPXManagerClass::Instance.Service();
		Sleep(5);
	}

	EventClass::DoList.Init();
	EventClass::OutList.Init();

	return reinterpret_cast<DWORD>(&QueueAIMultiplayer_ReturnToCaller);
}
