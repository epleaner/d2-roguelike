#include <Windows.h>
#include "D2Ptrs.h"
#include "Modules/Item/Item.h"

DWORD __declspec(naked) __fastcall D2CLIENT_GetUnitName_STUB(DWORD UnitAny)
{
	__asm
	{
		mov eax, ecx
		jmp D2CLIENT_GetUnitName_I
	}
}

// D2BS-style SendGamePacket stub
// The game function at 0x143E0 expects: packet length in EBX, packet pointer on stack
// We wrap this as __fastcall(dwLen, bPacket) for C++ usage
DWORD __declspec(naked) __fastcall D2CLIENT_SendGamePacket_ASM(DWORD /*dwLen*/, BYTE* /*bPacket*/)
{
    __asm
    {
        push edi
        mov edi, edx           // bPacket (2nd fastcall arg = EDX) -> EDI
        mov ebx, ecx           // dwLen (1st fastcall arg = ECX) -> EBX
        push edi               // push packet pointer onto stack
        call D2CLIENT_SendGamePacket_I
        pop edi
        ret
    }
}

DWORD __declspec(naked) __fastcall D2CLIENT_InitAutomapLayer(DWORD nLayerNo)
{
	__asm
	{
		push eax;
		mov eax, ecx;
		call D2CLIENT_InitAutomapLayer_I;
		pop eax;
		ret;
	}
}

DWORD __declspec(naked) __fastcall TestPvpFlag_STUB(DWORD planum1, DWORD planum2, DWORD flagmask)
{
	__asm
	{
		push esi;
		push[esp + 8];
		mov esi, edx;
		mov edx, ecx;
		call D2CLIENT_TestPvpFlag_I;
		pop esi;
		ret 4;
	}
}

DWORD __declspec(naked) __fastcall D2CLIENT_GetLevelName_STUB(DWORD levelId)
{
    __asm
    {
        // ESI is the engine's hidden input, but callee-saved in our C++ ABI.
        push esi
        mov esi, ecx
        call D2CLIENT_GetLevelName_I
        pop esi
        ret
    }
}

DWORD __declspec(naked) __fastcall D2CLIENT_GetUIVar_STUB(DWORD varno)
{
	__asm
	{
		mov eax, ecx;
		jmp D2CLIENT_GetUiVar_I;
	}
}

__declspec(naked) CellFile* __fastcall D2CLIENT_LoadUiImage(CHAR* szPath)
{
	__asm
	{
		MOV EAX, ECX
		PUSH 0
		CALL D2CLIENT_LoadUiImage_I
		RETN
	}
}

DWORD __declspec(naked) __fastcall D2CLIENT_ClickParty_ASM(RosterUnit* RosterUnit, DWORD Mode)
{
	__asm
	{
		mov eax, ecx
		jmp D2CLIENT_ClickParty_I
	}
}

__declspec(naked) int __stdcall D2CLIENT_PlaySound_STUB(int nSound, UnitAny* pUnit, int nTriggerFrame, int param_4, int param_5)
{
	__asm
	{
		push EBX
		mov EBX, [esp + 8]                                   // nSound
		push[esp + 24]                                      // param_5
		push[esp + 24]                                      // param_4
		push[esp + 24]                                      // nTriggerFrame
		push[esp + 24]                                      // pUnit
		call D2CLIENT_PlaySound                              // 0x88A70
		pop EBX
		retn 20
	}
}

__declspec(naked) void __stdcall D2CLIENT_PlayItemDropSounds_STUB(UnitAny* pUnit)
{
	__asm
	{
		mov EAX, [esp + 4]                                   // pUnit
		call D2CLIENT_PlayItemDropSounds                     // 0x827C0
		retn 4
	}
}

// Waypoint tab switching stubs
// ValidateSkillLevelRequirements at 0x6fb5a140 takes act index in EAX
// UpdateRoomLevelTracker at 0x6fb59f40 reads DAT_6fbacdd6 and populates WP list
// Wrapped as __fastcall so GameCallQueue can pass act index via ECX
DWORD __declspec(naked) __fastcall D2CLIENT_SetWaypointTab_STUB(DWORD actIndex)
{
	__asm
	{
		push ebx
		mov eax, ecx                  // actIndex from fastcall ECX -> EAX
		mov ebx, 0x6FB5A140           // ValidateSkillLevelRequirements
		call ebx
		mov ebx, 0x6FB59F40           // UpdateRoomLevelTracker
		call ebx
		pop ebx
		ret
	}
}

__declspec (naked) int __fastcall ITEMS_GetItemPropertiesString_STUB(int nStatFilter2, UnitAny* pItem, wchar_t* strBuffer, int nBufferLen, int a5, int nLayer, int nStatFilter, int a8, int a9, wchar_t* strBuffer2)
{
	__asm
	{
		push edi
		push esi
		mov edi, [esp + 40]
		mov esi, [esp + 36]
		push[esp + 32]
		push[esp + 32]
		push[esp + 32]
		push[esp + 32]
		push[esp + 32]
		push[esp + 32]
		push edx
		call D2CLIENT_GetItemPropertiesString
		pop esi
		pop edi
		retn 32
	}
}

__declspec(naked) MonSeqTxt* __stdcall D2COMMON_GetSequenceInfo_STUB(UnitAny* pUnit)
{
	__asm
	{
		mov EAX, [esp + 4]                                   // pUnit
		call D2COMMON_GetSequenceInfo                        // 0x2E840
		retn 4
	}
}

__declspec(naked) int __stdcall D2COMMON_GetSequenceIndex_STUB(UnitAny* pUnit)
{
	__asm
	{
		push ESI
		mov ESI, [esp + 8]                                   // param_1
		call D2COMMON_GetSequenceIndex                       // 0x2E790
		pop ESI
		retn 4
	}
}

__declspec(naked) int __stdcall D2COMMON_GetFrameMinAccr_STUB(int nIndex, UnitAny* pUnit)
{
	__asm
	{
		mov EAX, [esp + 4]                                   // nIndex
		push[esp + 8]                                       // pUnit
		call D2COMMON_GetFrameMinAccr                        // 0x323E0
		retn 8
	}
}

__declspec(naked) void __stdcall D2CLIENT_GetItemFromPacket_NewGround_STUB(px9c* packet)
{
	__asm
	{
		mov EAX, [esp + 4]									// packet

		call D2CLIENT_ItemPacketBuildAction0_NewGround		// 0x84BB0
    retn 4
  }
}


__declspec(naked) void __stdcall D2CLIENT_ItemPacketBuildAction2_DropToGround_STUB(px9c* packet)
{
	__asm
	{
		mov EAX, [esp + 4]                                   // packet
		call D2CLIENT_ItemPacketBuildAction2_DropToGround   // 0x84990
		retn 4
	}
}


__declspec(naked) void __stdcall D2CLIENT_ItemPacketBuildAction4_PutInContainer_STUB(px9c* packet)
{
	__asm
	{
		mov EAX, [esp + 4]                                   // packet
		call D2CLIENT_ItemPacketBuildAction4_PutInContainer // 0x84750
		retn 4
	}
}


__declspec(naked) void __stdcall D2CLIENT_GetItemDescription_STUB(ItemsTxt* pItemTxtRec, wchar_t* pBuffer, UnitAny* pItem)
{
	__asm
	{
		push ESI
		mov EAX, [esp + 8]                                   // pItemTxtRec
		mov ESI, [esp + 12]                                  // pBuffer
		push[esp + 16]                                      // pItem
		call D2CLIENT_GetItemDescription                     // 0x8D600
		pop ESI
		retn 12
	}
}

__declspec(naked) SoundInstance* __stdcall D2CLIENT_GetExistingSoundInstanceInQueue_STUB(int nSound, SoundInstance* pSoundInstanceToSkip)
{
	__asm
	{
		mov EAX, [esp + 4]                                   // param_1
		push[esp + 8]                                       // pSoundInstanceToSkip
		call D2CLIENT_GetExistingSoundInstanceInQueue        // 0x87FE0
		retn 8
	}
}

__declspec(naked) void __stdcall D2CLIENT_AdjustSoundBasedOnCoords_STUB(float* fPos, UnitAny* pUnit, int nSound)
{
	__asm
	{
		push EDI
		mov EDI, [esp + 8]                                   // fPos
		push[esp + 16]                                      // nSound
		push[esp + 16]                                      // pUnit
		call D2CLIENT_AdjustSoundBasedOnCoords               // 0x87F30
		pop EDI
		retn 12
	}
}

__declspec(naked) float __stdcall D2CLIENT_SoundTestCollision_STUB(int nSound, UnitAny* pTarget)
{
	__asm
	{
		mov EAX, [esp + 4]                                   // nSound
		push[esp + 8]                                       // pTarget
		call D2CLIENT_SoundTestCollision                     // 0x88440
		// Float return is stored in ST(0) register
		retn 8
	}
}

__declspec(naked) void __stdcall D2CLIENT_LinkSoundInstanceToClientUnit_STUB(UnitAny* pUnit, int nSoundInstanceId)
{
	__asm
	{
		mov EAX, [esp + 4]                                   // pUnit
		push[esp + 8]                                       // nSoundInstanceId
		call D2CLIENT_LinkSoundInstanceToClientUnit          // 0x22390
		retn 8
	}
}