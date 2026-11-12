#include "D2Handlers.h"


VOID __declspec(naked) OOGDraw_Interception()
{
	__asm
	{
		PUSHAD
		CALL OOGDraw
		POPAD
		MOV ESI,DWORD PTR DS:[ESI+0x3C]
		TEST ESI, ESI
		RETN
	}
}

void __declspec(naked) GameLoop_Interception()
{
	__asm
	{
		pushad;
		call GameLoop;
		popad;
 
		pop eax;
		sub esp, 0x20;
		mov [esp + 0xC], ecx;
		push eax;
		ret;
	}
}
