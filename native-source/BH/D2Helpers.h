#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <Windows.h>
#include "D2Structs.h"

#define INVALID_PARTY_ID 0xFFFF

bool IsValidMonster(UnitAny* pUnit);
int GetRelation(UnitAny* unit);
UnitAny* __stdcall GetClientMercUnit();
// Exact S13 interaction identity, not the inherited selected-unit binding.
// Must be read on the game thread.
UnitAny* D2CLIENT_GetCurrentInteractingNPC();
RosterUnit* FindPlayerRoster(DWORD unitId);
void Print(char* format, ...);
void PartyPrint(char* format, ...);

CellFile* LoadBmpCellFile(BYTE* buf1, int width, int height);
CellFile* LoadBmpCellFile(char* filename);
CellFile* InitCellFile(CellFile* cf);
void DeleteCellFile(CellFile* cf);

DWORD GetDistanceSquared(DWORD x1, DWORD y1, DWORD x2, DWORD y2);
bool SetSkill(WORD wSkillId, bool Left);
bool GetSkill(WORD wSkillId);
bool CastOnMap(WORD x, WORD y, bool Left);
bool RunTo(WORD x, WORD y);
bool Interact(DWORD UnitId, DWORD UnitType);
bool SendSyncMsg();

// D2BS-style direct game function calls (must be called on game thread)
bool ClickMap(DWORD dwClickType, int wX, int wY, BOOL bShift, UnitAny* pUnit = nullptr);

std::string GetItemCode(const char (&code)[4]);
std::string GetItemName(UnitAny* item);
bool IsTown(DWORD levelId);

bool IsGameReady();
DWORD GetPlayerArea();
int32_t __fastcall UTILITY_CalcPercent(int32_t nValue, double nPercent, uint32_t nBase);
wchar_t* GetTblEntryByIndex(int nIndex, int nSource);
int ItemColorFromQuality(unsigned int quality);
