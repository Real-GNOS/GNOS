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

#include "main.h"

CHEATSTATE g_Cheat = { FALSE, FALSE, FALSE, FALSE };

// ---------------------------------------------------------------------------
// Per-frame cheat effects
// ---------------------------------------------------------------------------

VOID
PAL_CheatApply(
   VOID
)
/*++

  Purpose:

    Apply the continuous effects of the active cheats. Called once per
    frame from both the field and the battle loop.

--*/
{
   int                  i;

   if (g_Cheat.fGodMode && gpGlobals->fInMainGame)
   {
      for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
      {
         WORD wPlayerRole = gpGlobals->rgParty[i].wPlayerRole;

         gpGlobals->g.PlayerRoles.rgwHP[wPlayerRole] =
            gpGlobals->g.PlayerRoles.rgwMaxHP[wPlayerRole];
         gpGlobals->g.PlayerRoles.rgwMP[wPlayerRole] =
            gpGlobals->g.PlayerRoles.rgwMaxMP[wPlayerRole];
      }
   }

   if (g_Cheat.fOneHitKill && gpGlobals->fInBattle)
   {
      for (i = 0; i <= g_Battle.wMaxEnemyIndex; i++)
      {
         if (g_Battle.rgEnemy[i].wObjectID != 0 &&
            (SHORT)g_Battle.rgEnemy[i].e.wHealth > 1)
         {
            g_Battle.rgEnemy[i].e.wHealth = 1;
         }
      }
   }
}

VOID
PAL_CheatDrawIndicator(
   VOID
)
{
   if (!g_Cheat.fGodMode && !g_Cheat.fOneHitKill && !g_Cheat.fNoWall && !g_Cheat.fNoItemCost)
   {
      return;
   }

   PAL_DrawText(L"\x4FEE\x6539\x5668", PAL_XY(320 - 16 * 3 - 4, 1), 0x2D, TRUE, FALSE, FALSE);
}

// ---------------------------------------------------------------------------
// Cheat actions
// ---------------------------------------------------------------------------

static VOID
PAL_CheatHealParty(
   VOID
)
{
   int                  i;

   for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
   {
      WORD wPlayerRole = gpGlobals->rgParty[i].wPlayerRole;

      gpGlobals->g.PlayerRoles.rgwHP[wPlayerRole] =
         gpGlobals->g.PlayerRoles.rgwMaxHP[wPlayerRole];
      gpGlobals->g.PlayerRoles.rgwMP[wPlayerRole] =
         gpGlobals->g.PlayerRoles.rgwMaxMP[wPlayerRole];

      PAL_CurePoisonByLevel(wPlayerRole, 9);
   }

   PAL_ClearAllPlayerStatus();
}

static VOID
PAL_CheatCash(
   VOID
)
{
   DWORD   dwCash = gpGlobals->dwCash + 99999;

   gpGlobals->dwCash = (dwCash > 999999) ? 999999 : dwCash;
}

static VOID
PAL_CheatMaxLevel(
   VOID
)
{
   int                  i, j;

   for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
   {
      WORD wPlayerRole = gpGlobals->rgParty[i].wPlayerRole;
      WORD wLevel      = gpGlobals->g.PlayerRoles.rgwLevel[wPlayerRole];

      if (wLevel < MAX_LEVELS)
      {
         PAL_PlayerLevelUp(wPlayerRole, MAX_LEVELS - wLevel);
      }

      gpGlobals->g.PlayerRoles.rgwHP[wPlayerRole] =
         gpGlobals->g.PlayerRoles.rgwMaxHP[wPlayerRole];
      gpGlobals->g.PlayerRoles.rgwMP[wPlayerRole] =
         gpGlobals->g.PlayerRoles.rgwMaxMP[wPlayerRole];

      // Learn every magic this role can get from level ups.
      for (j = 0; j < gpGlobals->g.nLevelUpMagic; j++)
      {
         WORD wMagic = gpGlobals->g.lprgLevelUpMagic[j].m[wPlayerRole].wMagic;

         if (wMagic != 0 &&
            gpGlobals->g.lprgLevelUpMagic[j].m[wPlayerRole].wLevel <=
            gpGlobals->g.PlayerRoles.rgwLevel[wPlayerRole])
         {
            PAL_AddMagic(wPlayerRole, wMagic);
         }
      }
   }
}

static VOID
PAL_CheatGiveAllItems(
   VOID
)
{
   int      i, j;
   BOOL     rgfIsEnemy[MAX_OBJECTS];
   LPOBJECT pObj;

   memset(rgfIsEnemy, 0, sizeof(rgfIsEnemy));

   for (i = 0; i < gpGlobals->g.nEnemyTeam; i++)
   {
      for (j = 0; j < MAX_ENEMIES_IN_TEAM; j++)
      {
         WORD w = gpGlobals->g.lprgEnemyTeam[i].rgwEnemy[j];

         if (w != 0 && w != 0xFFFF && w < MAX_OBJECTS)
         {
            rgfIsEnemy[w] = TRUE;
         }
      }
   }

   for (i = 1; i < MAX_OBJECTS; i++)
   {
      pObj = &gpGlobals->g.rgObject[i];

      if (rgfIsEnemy[i])
      {
         continue;
      }

      // Heuristic: item objects carry a bitmap, a price and item flags.
      if (pObj->item.wBitmap == 0 || pObj->item.wPrice == 0 || pObj->item.wFlags == 0)
      {
         continue;
      }

      if (PAL_MKFGetChunkSize(pObj->item.wBitmap, gpGlobals->f.fpBALL) <= 0)
      {
         continue;
      }

      PAL_AddItemToInventory((WORD)i, 99);
   }

   PAL_CompressInventory();
}

static VOID
PAL_CheatKillAllEnemies(
   VOID
)
{
   int                  i;

   if (!gpGlobals->fInBattle)
   {
      return;
   }

   for (i = 0; i <= g_Battle.wMaxEnemyIndex; i++)
   {
      if (g_Battle.rgEnemy[i].wObjectID != 0)
      {
         g_Battle.rgEnemy[i].e.wHealth = 0;
      }
   }
}

// ---------------------------------------------------------------------------
// Cheat panel
// ---------------------------------------------------------------------------

typedef enum tagCHEATITEMTYPE
{
   kCheatItemToggle = 0,
   kCheatItemAction
} CHEATITEMTYPE;

typedef struct tagCHEATMENUITEM
{
   LPCWSTR    lpszLabel;
   CHEATITEMTYPE   iType;
   BOOL       *pfValue;        // for toggles
   VOID       (*fnAction)(VOID); // for actions
   BOOL       fBattleOnly;
} CHEATMENUITEM;

static const CHEATMENUITEM g_rgCheatMenu[] =
{
   { L"\x65E0\x654C\x6A21\x5F0F",       kCheatItemToggle, &g_Cheat.fGodMode,    NULL,                     FALSE },
   { L"\x4E00\x51FB\x5FC5\x6740",       kCheatItemToggle, &g_Cheat.fOneHitKill, NULL,                     FALSE },
   { L"\x7A7F\x5899\x884C\x8D70",       kCheatItemToggle, &g_Cheat.fNoWall,     NULL,                     FALSE },
   { L"\x9053\x5177\x4E0D\x6D88\x8017", kCheatItemToggle, &g_Cheat.fNoItemCost, NULL,                     FALSE },
   { L"\x5168\x5458\x6EE1\x8840",       kCheatItemAction, NULL,                  PAL_CheatHealParty,        FALSE },
   { L"\x91D1\x5E01+99999",            kCheatItemAction, NULL,                  PAL_CheatCash,             FALSE },
   { L"\x5168\x5458\x6EE1\x7EA7",       kCheatItemAction, NULL,                  PAL_CheatMaxLevel,         FALSE },
   { L"\x83B7\x5F97\x5168\x90E8\x9053\x5177", kCheatItemAction, NULL,             PAL_CheatGiveAllItems,     FALSE },
   { L"\x79D2\x6740\x5168\x573A",       kCheatItemAction, NULL,                  PAL_CheatKillAllEnemies,   TRUE  },
};

static const WCHAR SC_rgszCheatTitle[] =
   L"\x8D85\x7EA7\x4FEE\x6539\x5668   \x7A7A\x683C:\x6267\x884C   Esc:\x5173\x95ED";

static BOOL
PAL_CheatMenuItemEnabled(
   const CHEATMENUITEM *lpItem
)
{
   if (lpItem->fBattleOnly && !gpGlobals->fInBattle)
   {
      return FALSE;
   }

   return TRUE;
}

static VOID
PAL_CheatMenuActivate(
   INT iItem
)
{
   const CHEATMENUITEM *lpItem = &g_rgCheatMenu[iItem];

   if (!PAL_CheatMenuItemEnabled(lpItem))
   {
      return;
   }

   if (lpItem->iType == kCheatItemToggle)
   {
      *(lpItem->pfValue) = !*(lpItem->pfValue);
   }
   else if (lpItem->fnAction != NULL)
   {
      lpItem->fnAction();
   }
}

VOID
PAL_OpenCheatMenu(
   VOID
)
{
   const INT        nItem        = sizeof(g_rgCheatMenu) / sizeof(g_rgCheatMenu[0]);
   const INT        nRow         = nItem + 1;    // + title row
   const INT        iRowHeight   = 16;
   const INT        nItemRows    = nRow;

   INT              i, iCurrent  = 0;
   INT              iCellW = 16, iCellH = 16, iLeftB = 16, iTopB = 16;
   INT              iMaxLabel    = 0;
   INT              iContentW, iContentH, iBoxW, iBoxH;
   INT              iBoxX, iBoxY, iInnerX, iInnerY, iTextY;
   INT              iNRows, iNColumns;
   SDL_Rect         rect, rectInner;
   SDL_Surface     *pSaved       = NULL;
   SDL_Surface     *pInterior    = NULL;
   LPCWSTR          lpStateOn    = L"\x5F00";
   LPCWSTR          lpStateOff   = L"\x5173";

   if (gpScreen == NULL || gpSpriteUI == NULL)
   {
      return;
   }

   iCellW  = PAL_RLEGetWidth(PAL_SpriteGetFrame(gpSpriteUI, 1));
   iCellH  = PAL_RLEGetHeight(PAL_SpriteGetFrame(gpSpriteUI, 3));
   iLeftB  = PAL_RLEGetWidth(PAL_SpriteGetFrame(gpSpriteUI, 0)) +
             PAL_RLEGetWidth(PAL_SpriteGetFrame(gpSpriteUI, 2));
   iTopB   = PAL_RLEGetHeight(PAL_SpriteGetFrame(gpSpriteUI, 0)) +
             PAL_RLEGetHeight(PAL_SpriteGetFrame(gpSpriteUI, 6));

   if (iCellW <= 0) iCellW = 16;
   if (iCellH <= 0) iCellH = 16;

   for (i = 0; i < nItem; i++)
   {
      INT w = PAL_TextWidth(g_rgCheatMenu[i].lpszLabel);

      if (w > iMaxLabel)
      {
         iMaxLabel = w;
      }
   }

   {
      INT iStateW = PAL_TextWidth(lpStateOn) > PAL_TextWidth(lpStateOff) ?
         PAL_TextWidth(lpStateOn) : PAL_TextWidth(lpStateOff);
      INT iTitleW = PAL_TextWidth(SC_rgszCheatTitle);

      iContentW = 8 + iMaxLabel + 16 + iStateW + 8;

      if (iTitleW + 16 > iContentW)
      {
         iContentW = iTitleW + 16;
      }
   }

   iContentH = nItemRows * iRowHeight;

   iNColumns = (iContentW + iCellW - 1) / iCellW;
   iNRows    = (iContentH + iCellH - 1) / iCellH;

   iBoxW = iLeftB + iNColumns * iCellW + 6;
   iBoxH = iTopB  + iNRows    * iCellH + 6;

   iBoxX = (320 - iBoxW) / 2;
   iBoxY = (200 - iBoxH) / 2;

   if (iBoxX < 0) iBoxX = 0;
   if (iBoxY < 0) iBoxY = 0;

   rect.x = iBoxX;
   rect.y = iBoxY;
   rect.w = iBoxW;
   rect.h = iBoxH;

   iInnerX = iBoxX + PAL_RLEGetWidth(PAL_SpriteGetFrame(gpSpriteUI, 0));
   iInnerY = iBoxY + PAL_RLEGetHeight(PAL_SpriteGetFrame(gpSpriteUI, 0));
   iTextY  = iInnerY + (iNRows * iCellH - nRow * iRowHeight) / 2;

   pSaved = VIDEO_DuplicateSurface(gpScreen, &rect);

   PAL_CreateBox(PAL_XY(iBoxX, iBoxY), iNRows, iNColumns, 0, FALSE);

   //
   // Freeze the clean box interior; every key state change restores it so
   // highlights and toggle values never ghost on the previous frame.
   //
   rectInner.x = iInnerX;
   rectInner.y = iInnerY;
   rectInner.w = iNColumns * iCellW;
   rectInner.h = iNRows * iCellH;

   pInterior = VIDEO_DuplicateSurface(gpScreen, &rectInner);

   while (TRUE)
   {
      INT iInnerW = iNColumns * iCellW;
      INT y;

      PAL_ClearKeyState();
      PAL_ProcessEvent();
      UTIL_Delay(10);

      if (pInterior != NULL)
      {
         VIDEO_CopySurface(pInterior, NULL, gpScreen, &rectInner);
      }

      if (g_InputState.dwKeyPress & kKeyCheat)
      {
         break;
      }
      else if (g_InputState.dwKeyPress & kKeyMenu)
      {
         break;
      }

      if (g_InputState.dwKeyPress & (kKeyUp | kKeyLeft))
      {
         iCurrent = (iCurrent + nItem - 1) % nItem;
      }
      else if (g_InputState.dwKeyPress & (kKeyDown | kKeyRight))
      {
         iCurrent = (iCurrent + 1) % nItem;
      }
      else if ((g_InputState.dwKeyPress & kKeySearch) &&
         !(SDL_GetModState() & KMOD_CTRL))
      {
         PAL_CheatMenuActivate(iCurrent);
      }

      // draw title
      PAL_DrawText(SC_rgszCheatTitle, PAL_XY(iInnerX + 8, iTextY), 0x4F, TRUE, FALSE, FALSE);

      for (i = 0; i < nItem; i++)
      {
         const CHEATMENUITEM *lpItem = &g_rgCheatMenu[i];
         BOOL                fEnabled = PAL_CheatMenuItemEnabled(lpItem);
         BYTE                bColor;

         y = iTextY + (i + 1) * iRowHeight;

         if (i == iCurrent)
         {
            bColor = fEnabled ? MENUITEM_COLOR : MENUITEM_COLOR_SELECTED_INACTIVE;
         }
         else if (!fEnabled)
         {
            bColor = MENUITEM_COLOR_INACTIVE;
         }
         else
         {
            bColor = MENUITEM_COLOR;
         }

         PAL_DrawText(lpItem->lpszLabel, PAL_XY(iInnerX + 8, y), bColor, TRUE, FALSE, FALSE);

         if (lpItem->iType == kCheatItemToggle)
         {
            LPCWSTR lpState = *(lpItem->pfValue) ? lpStateOn : lpStateOff;

            PAL_DrawText(lpState,
               PAL_XY(iInnerX + iInnerW - 8 - PAL_TextWidth(lpState), y),
               *(lpItem->pfValue) ? MENUITEM_COLOR_CONFIRMED : MENUITEM_COLOR_INACTIVE,
               TRUE, FALSE, FALSE);
         }
      }

      VIDEO_UpdateScreen(NULL);
   }

   if (pInterior != NULL)
   {
      VIDEO_FreeSurface(pInterior);
   }

   if (pSaved != NULL)
   {
      VIDEO_CopySurface(pSaved, NULL, gpScreen, &rect);
      VIDEO_FreeSurface(pSaved);
   }

   VIDEO_UpdateScreen(&rect);
   PAL_ClearKeyState();
}

// ---------------------------------------------------------------------------
// Force-attack: start a battle against the nearest NPC
// ---------------------------------------------------------------------------

typedef struct tagNPCBATTLE
{
   BOOL       fActive;
   WORD       wObjectID;   // base enemy object id (a real rgObject slot)
   WORD       wSpriteNum;  // sprite of the NPC, loaded from MGO.MKF in battle
   ENEMY      e;           // synthesized enemy data
} NPCBATTLE;

static NPCBATTLE   s_NPCBattle;

static WORD
PAL_CheatPartyAverageLevel(
   VOID
)
{
   int      i;
   DWORD    dwLevel = 0;

   for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
   {
      dwLevel += gpGlobals->g.PlayerRoles.rgwLevel[gpGlobals->rgParty[i].wPlayerRole];
   }

   dwLevel /= (DWORD)(gpGlobals->wMaxPartyMemberIndex + 1);

   if (dwLevel < 1)
   {
      dwLevel = 1;
   }
   if (dwLevel > MAX_LEVELS)
   {
      dwLevel = MAX_LEVELS;
   }

   return (WORD)dwLevel;
}

BOOL
PAL_CheatNPCBattleActive(
   VOID
)
{
   return s_NPCBattle.fActive;
}

ENEMY
PAL_CheatNPCBattleEnemy(
   VOID
)
{
   return s_NPCBattle.e;
}

WORD
PAL_CheatNPCBattleObjectID(
   VOID
)
{
   return s_NPCBattle.wObjectID;
}

WORD
PAL_CheatNPCBattleSprite(
   VOID
)
{
   return s_NPCBattle.wSpriteNum;
}

VOID
PAL_CheatNPCBattleEnd(
   VOID
)
{
   memset(&s_NPCBattle, 0, sizeof(s_NPCBattle));
}

VOID
PAL_ForceBattleNearestNPC(
   VOID
)
/*++

  Purpose:

    Start a battle right away, using the NPC nearest to the party as the
    only enemy. Bound to Ctrl+Alt+F.

--*/
{
   int              i, iFirst, iLast;
   int              x, y, dist, best;
   int              wAvgLevel;
   LPEVENTOBJECT    p, pBest;
   WORD             wBestBaseObject = 0;
   int              iLevelDiff, iBestLevelDiff = 0x7FFF;
   WORD             w, wEnemyID;

   if (gpGlobals->fInBattle || gpGlobals->fEnteringScene || !gpGlobals->fInMainGame)
   {
      return;
   }

   //
   // Find the nearest visible NPC event object.
   //
   x = PAL_X(gpGlobals->viewport) + PAL_X(gpGlobals->partyoffset);
   y = PAL_Y(gpGlobals->viewport) + PAL_Y(gpGlobals->partyoffset);

   pBest = NULL;
   best  = 0x7FFF;

   iFirst = gpGlobals->g.rgScene[gpGlobals->wNumScene - 1].wEventObjectIndex + 1;
   iLast  = gpGlobals->g.rgScene[gpGlobals->wNumScene].wEventObjectIndex;

   for (i = iFirst; i <= iLast; i++)
   {
      p = &gpGlobals->g.lprgEventObject[i - 1];

      if (p->sState <= 0 || p->wSpriteNum == 0)
      {
         continue;
      }

      dist = abs(p->x - x) + abs(p->y - y) * 2;

      if (dist < best)
      {
         best = dist;
         pBest = p;
      }
   }

   if (pBest == NULL)
   {
      return;
   }

   wAvgLevel = PAL_CheatPartyAverageLevel();

   //
   // Pick a real enemy object whose level is closest to the party level.
   // Its data is only used for things we do not override (sounds,
   // resistances) and to keep rgObject[..].enemy.* lookups valid.
   //
   for (i = 0; i < gpGlobals->g.nEnemyTeam; i++)
   {
      int j;

      for (j = 0; j < MAX_ENEMIES_IN_TEAM; j++)
      {
         w = gpGlobals->g.lprgEnemyTeam[i].rgwEnemy[j];

         if (w == 0 || w == 0xFFFF || w >= MAX_OBJECTS)
         {
            continue;
         }

         wEnemyID = gpGlobals->g.rgObject[w].enemy.wEnemyID;

         if (wEnemyID >= gpGlobals->g.nEnemy)
         {
            continue;
         }

         iLevelDiff = abs((int)gpGlobals->g.lprgEnemy[wEnemyID].wLevel - wAvgLevel);

         if (wBestBaseObject == 0 || iLevelDiff < iBestLevelDiff)
         {
            iBestLevelDiff = iLevelDiff;
            wBestBaseObject = w;
         }
      }
   }

   if (wBestBaseObject == 0)
   {
      return;
   }

   //
   // Synthesize the NPC "enemy".
   //
   s_NPCBattle.e = gpGlobals->g.lprgEnemy[gpGlobals->g.rgObject[wBestBaseObject].enemy.wEnemyID];

   s_NPCBattle.e.wHealth              = (WORD)(300 + 80 * wAvgLevel);
   s_NPCBattle.e.wLevel               = (WORD)wAvgLevel;
   s_NPCBattle.e.wExp                 = (WORD)(20 * wAvgLevel);
   s_NPCBattle.e.wCash                = (WORD)(10 * wAvgLevel);
   s_NPCBattle.e.wMagic               = 0;   // never cast magic
   s_NPCBattle.e.wMagicRate           = 0;
   s_NPCBattle.e.wAttackEquivItem     = 0;
   s_NPCBattle.e.wAttackEquivItemRate = 0;
   s_NPCBattle.e.wStealItem           = 0;
   s_NPCBattle.e.nStealItem           = 0;
   s_NPCBattle.e.wDualMove            = 0;
   s_NPCBattle.e.wCollectValue        = 0;
   s_NPCBattle.e.wAttackStrength      = (WORD)(30 + 6 * wAvgLevel);
   s_NPCBattle.e.wMagicStrength       = (WORD)(30 + 6 * wAvgLevel);
   s_NPCBattle.e.wDefense             = (WORD)(10 + 2 * wAvgLevel);
   s_NPCBattle.e.wDexterity           = (WORD)(30 + 3 * wAvgLevel);
   s_NPCBattle.e.wFleeRate            = 0;

   //
   // The idle frame count follows the NPC's own per-direction frame count
   // (clamped to the actual sprite in PAL_LoadBattleSprites).
   //
   s_NPCBattle.e.wIdleFrames          = (pBest->nSpriteFrames > 0) ? pBest->nSpriteFrames : 1;
   s_NPCBattle.e.wMagicFrames         = 0;
   s_NPCBattle.e.wAttackFrames        = 0;
   s_NPCBattle.e.wYPosOffset          = 0;

   if (s_NPCBattle.e.wIdleAnimSpeed == 0)
   {
      s_NPCBattle.e.wIdleAnimSpeed = 4;
   }

   s_NPCBattle.wObjectID  = wBestBaseObject;
   s_NPCBattle.wSpriteNum = pBest->wSpriteNum;
   s_NPCBattle.fActive    = TRUE;

   PAL_StartBattle(0, FALSE);

   PAL_CheatNPCBattleEnd();

   // Make the transition back to the field less abrupt.
   gpGlobals->fNeedToFadeIn = TRUE;
}
