/* -*- mode: c; tab-width: 4; c-basic-offset: 4; c-file-style: "linux" -*- */
//
// Copyright (c) 2009-2011, Wei Mingzhi <whistler_wmz@users.sf.net>.
// Copyright (c) 2011-2026, SDLPAL development team.
// All rights reserved.
//
// This file is part of SDLPAL.
//
// SDLPAL is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef CHEAT_H
#define CHEAT_H

#include "common.h"
#include "global.h"

typedef struct tagCHEATSTATE
{
   BOOL    fGodMode;       // 无敌模式 (party HP/MP locked at max each frame)
   BOOL    fOneHitKill;    // 一击必杀 (enemies are clamped to 1 HP in battle)
   BOOL    fNoWall;        // 穿墙行走 (ignore terrain/NPC blocking in the field)
   BOOL    fNoItemCost;    // 道具不消耗 (using/throwing items does not consume them)
} CHEATSTATE;

extern CHEATSTATE g_Cheat;

PAL_C_LINKAGE_BEGIN

// Apply the per-frame effects of the active cheats. Call once per
// frame, both in the field and in battle.
VOID
PAL_CheatApply(
   VOID
);

// Draw a small watermark while any cheat is on. Call after the scene
// has been composed and before blitting to the screen.
VOID
PAL_CheatDrawIndicator(
   VOID
);

// Open the cheat panel ("修改器"). Runs a modal loop.
VOID
PAL_OpenCheatMenu(
   VOID
);

// Immediately start a battle against the nearest NPC (Ctrl+Alt+F).
VOID
PAL_ForceBattleNearestNPC(
   VOID
);

// --- "force attack" support helpers used by battle.c -------------

BOOL
PAL_CheatNPCBattleActive(
   VOID
);

ENEMY
PAL_CheatNPCBattleEnemy(
   VOID
);

WORD
PAL_CheatNPCBattleObjectID(
   VOID
);

WORD
PAL_CheatNPCBattleSprite(
   VOID
);

VOID
PAL_CheatNPCBattleEnd(
   VOID
);

PAL_C_LINKAGE_END

#endif
