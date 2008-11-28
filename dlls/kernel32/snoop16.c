/*
 * 386-specific Win16 dll<->dll snooping functions
 *
 * Copyright 1998 Marcus Meissner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "wine/winbase16.h"
#include "winternl.h"
#include "wine/library.h"
#include "kernel_private.h"
#include "kernel16_private.h"
#include "toolhelp.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(snoop);

#ifdef __i386__

#include "pshpack1.h"

void WINAPI SNOOP16_Entry(FARPROC proc, LPBYTE args, CONTEXT86 *context);
void WINAPI SNOOP16_Return(FARPROC proc, LPBYTE args, CONTEXT86 *context);

typedef	struct tagSNOOP16_FUN {
	/* code part */
	BYTE		lcall;		/* 0x9a call absolute with segment */
	DWORD		snr;
	/* unreached */
	int		nrofargs;
	FARPROC16	origfun;
	char		*name;
} SNOOP16_FUN;

typedef struct tagSNOOP16_DLL {
	HMODULE16	hmod;
	HANDLE16	funhandle;
	SNOOP16_FUN	*funs;
	struct tagSNOOP16_DLL	*next;
	char name[1];
} SNOOP16_DLL;

typedef struct tagSNOOP16_RETURNENTRY {
	/* code part */
	BYTE		lcall;		/* 0x9a call absolute with segment */
	DWORD		snr;
	/* unreached */
	FARPROC16	origreturn;
	SNOOP16_DLL	*dll;
	DWORD		ordinal;
	WORD		origSP;
	WORD		*args;		/* saved args across a stdcall */
} SNOOP16_RETURNENTRY;

typedef struct tagSNOOP16_RETURNENTRIES {
	SNOOP16_RETURNENTRY entry[65500/sizeof(SNOOP16_RETURNENTRY)];
	HANDLE16	rethandle;
	struct tagSNOOP16_RETURNENTRIES	*next;
} SNOOP16_RETURNENTRIES;

typedef struct tagSNOOP16_RELAY {
	WORD		pushbp;		/* 0x5566 */
	BYTE		pusheax;	/* 0x50 */
	WORD		pushax;		/* 0x5066 */
	BYTE		pushl;		/* 0x68 */
	DWORD		realfun;	/* SNOOP16_Return */
	BYTE		lcall;		/* 0x9a call absolute with segment */
	DWORD		callfromregs;
	WORD		seg;
        WORD		lret;           /* 0xcb66 */
} SNOOP16_RELAY;

#include "poppack.h"

static	SNOOP16_DLL		*firstdll = NULL;
static	SNOOP16_RETURNENTRIES 	*firstrets = NULL;
static	SNOOP16_RELAY		*snr;
static	HANDLE16		xsnr = 0;

void
SNOOP16_RegisterDLL(HMODULE16 hModule,LPCSTR name) {
	SNOOP16_DLL	**dll = &(firstdll);
	char		*s;

	if (!TRACE_ON(snoop)) return;

        TRACE("hmod=%x, name=%s\n", hModule, name);

	if (!snr) {
		xsnr=GLOBAL_Alloc(GMEM_ZEROINIT,2*sizeof(*snr),0,WINE_LDT_FLAGS_CODE|WINE_LDT_FLAGS_32BIT);
		snr = GlobalLock16(xsnr);
		snr[0].pushbp	= 0x5566;
		snr[0].pusheax	= 0x50;
		snr[0].pushax	= 0x5066;
		snr[0].pushl	= 0x68;
		snr[0].realfun	= (DWORD)SNOOP16_Entry;
		snr[0].lcall 	= 0x9a;
		snr[0].callfromregs = (DWORD)__wine_call_from_16_regs;
		snr[0].seg      = wine_get_cs();
		snr[0].lret     = 0xcb66;

		snr[1].pushbp	= 0x5566;
		snr[1].pusheax	= 0x50;
		snr[1].pushax	= 0x5066;
		snr[1].pushl	= 0x68;
		snr[1].realfun	= (DWORD)SNOOP16_Return;
		snr[1].lcall 	= 0x9a;
		snr[1].callfromregs = (DWORD)__wine_call_from_16_regs;
		snr[1].seg      = wine_get_cs();
		snr[1].lret     = 0xcb66;
	}
	while (*dll) {
		if ((*dll)->hmod == hModule)
                {
                    /* another dll, loaded at the same address */
                    GlobalUnlock16((*dll)->funhandle);
                    GlobalFree16((*dll)->funhandle);
                    break;
                }
		dll = &((*dll)->next);
	}

	if (*dll)
		*dll = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *dll, sizeof(SNOOP16_DLL)+strlen(name));
	else
		*dll = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SNOOP16_DLL)+strlen(name));	

	(*dll)->next	= NULL;
	(*dll)->hmod	= hModule;
	if ((s=strrchr(name,'\\')))
		name = s+1;
	strcpy( (*dll)->name, name );
	if ((s=strrchr((*dll)->name,'.')))
		*s='\0';
	(*dll)->funhandle = GlobalHandleToSel16(GLOBAL_Alloc(GMEM_ZEROINIT,65535,0,WINE_LDT_FLAGS_CODE));
	(*dll)->funs = GlobalLock16((*dll)->funhandle);
	if (!(*dll)->funs) {
		HeapFree(GetProcessHeap(),0,*dll);
		FIXME("out of memory\n");
		return;
	}
}

FARPROC16
SNOOP16_GetProcAddress16(HMODULE16 hmod,DWORD ordinal,FARPROC16 origfun) {
	SNOOP16_DLL			*dll = firstdll;
	SNOOP16_FUN			*fun;
	NE_MODULE			*pModule = NE_GetPtr(hmod);
	unsigned char			*cpnt;
	char				name[200];

	if (!TRACE_ON(snoop) || !pModule || !HIWORD(origfun))
		return origfun;
	if (!*(LPBYTE)MapSL((SEGPTR)origfun)) /* 0x00 is an impossible opcode, possible dataref. */
		return origfun;
	while (dll) {
		if (hmod == dll->hmod)
			break;
		dll=dll->next;
	}
	if (!dll)	/* probably internal */
		return origfun;
	if (ordinal>65535/sizeof(SNOOP16_FUN))
		return origfun;
	fun = dll->funs+ordinal;
	/* already done? */
	fun->lcall 	= 0x9a;
	fun->snr	= MAKELONG(0,xsnr);
	fun->origfun	= origfun;
	if (fun->name)
		return (FARPROC16)(SEGPTR)MAKELONG(((char*)fun-(char*)dll->funs),dll->funhandle);
	cpnt = (unsigned char *)pModule + pModule->ne_restab;
	while (*cpnt) {
		cpnt += *cpnt + 1 + sizeof(WORD);
		if (*(WORD*)(cpnt+*cpnt+1) == ordinal) {
			sprintf(name,"%.*s",*cpnt,cpnt+1);
			break;
		}
	}
	/* Now search the non-resident names table */

	if (!*cpnt && pModule->nrname_handle) {
		cpnt = GlobalLock16( pModule->nrname_handle );
		while (*cpnt) {
			cpnt += *cpnt + 1 + sizeof(WORD);
			if (*(WORD*)(cpnt+*cpnt+1) == ordinal) {
				    sprintf(name,"%.*s",*cpnt,cpnt+1);
				    break;
			}
		}
	}
	if (*cpnt)
        {
            fun->name = HeapAlloc(GetProcessHeap(),0,strlen(name)+1);
            strcpy( fun->name, name );
        }
	else
            fun->name = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,1); /* empty string */

	if (!SNOOP16_ShowDebugmsgSnoop(dll->name, ordinal, fun->name))
		return origfun;

	/* more magic. do not try to snoop thunk data entries (MMSYSTEM) */
	if (strchr(fun->name,'_')) {
		char *s=strchr(fun->name,'_');

		if (!strncasecmp(s,"_thunkdata",10)) {
			HeapFree(GetProcessHeap(),0,fun->name);
			fun->name = NULL;
			return origfun;
		}
	}
	fun->lcall 	= 0x9a;
	fun->snr	= MAKELONG(0,xsnr);
	fun->origfun	= origfun;
	fun->nrofargs	= -1;
	return (FARPROC16)(SEGPTR)MAKELONG(((char*)fun-(char*)dll->funs),dll->funhandle);
}

#define CALLER1REF (*(DWORD*)(MapSL( MAKESEGPTR(context->SegSs,LOWORD(context->Esp)+4))))
void WINAPI SNOOP16_Entry(FARPROC proc, LPBYTE args, CONTEXT86 *context) {
	DWORD		ordinal=0;
	DWORD		entry=(DWORD)MapSL( MAKESEGPTR(context->SegCs,LOWORD(context->Eip)) )-5;
	WORD		xcs = context->SegCs;
	SNOOP16_DLL	*dll = firstdll;
	SNOOP16_FUN	*fun = NULL;
	SNOOP16_RETURNENTRIES	**rets = &firstrets;
	SNOOP16_RETURNENTRY	*ret;
	unsigned	i=0;
	int		max;

	while (dll) {
		if (xcs == dll->funhandle) {
			fun = (SNOOP16_FUN*)entry;
			ordinal = fun-dll->funs;
			break;
		}
		dll=dll->next;
	}
	if (!dll) {
		FIXME("entrypoint 0x%08x not found\n",entry);
		return; /* oops */
	}
	while (*rets) {
		for (i=0;i<sizeof((*rets)->entry)/sizeof((*rets)->entry[0]);i++)
			if (!(*rets)->entry[i].origreturn)
				break;
		if (i!=sizeof((*rets)->entry)/sizeof((*rets)->entry[0]))
			break;
		rets = &((*rets)->next);
	}
	if (!*rets) {
		HANDLE16 hand = GlobalHandleToSel16(GLOBAL_Alloc(GMEM_ZEROINIT,65535,0,WINE_LDT_FLAGS_CODE));
		*rets = GlobalLock16(hand);
		(*rets)->rethandle = hand;
		i = 0;	/* entry 0 is free */
	}
	ret = &((*rets)->entry[i]);
	ret->lcall 	= 0x9a;
	ret->snr	= MAKELONG(sizeof(SNOOP16_RELAY),xsnr);
	ret->origreturn	= (FARPROC16)CALLER1REF;
	CALLER1REF	= MAKELONG((char*)&(ret->lcall)-(char*)((*rets)->entry),(*rets)->rethandle);
	ret->dll	= dll;
	ret->args	= NULL;
	ret->ordinal	= ordinal;
	ret->origSP	= LOWORD(context->Esp);

	context->Eip= LOWORD(fun->origfun);
	context->SegCs = HIWORD(fun->origfun);


	DPRINTF("%04x:CALL %s.%d: %s(",GetCurrentThreadId(), dll->name,ordinal,fun->name);
	if (fun->nrofargs>0) {
		max = fun->nrofargs;
		if (max>16) max=16;
		for (i=max;i--;)
			DPRINTF("%04x%s",*(WORD*)((char *) MapSL( MAKESEGPTR(context->SegSs,LOWORD(context->Esp)) )+8+sizeof(WORD)*i),i?",":"");
		if (max!=fun->nrofargs)
			DPRINTF(" ...");
	} else if (fun->nrofargs<0) {
		DPRINTF("<unknown, check return>");
		ret->args = HeapAlloc(GetProcessHeap(),0,16*sizeof(WORD));
		memcpy(ret->args,(LPBYTE)((char *) MapSL( MAKESEGPTR(context->SegSs,LOWORD(context->Esp)) )+8),sizeof(WORD)*16);
	}
	DPRINTF(") ret=%04x:%04x\n",HIWORD(ret->origreturn),LOWORD(ret->origreturn));
}

void WINAPI SNOOP16_Return(FARPROC proc, LPBYTE args, CONTEXT86 *context) {
	SNOOP16_RETURNENTRY	*ret = (SNOOP16_RETURNENTRY*)((char *) MapSL( MAKESEGPTR(context->SegCs,LOWORD(context->Eip)) )-5);

	/* We haven't found out the nrofargs yet. If we called a cdecl
	 * function it is too late anyway and we can just set '0' (which
	 * will be the difference between orig and current SP
	 * If pascal -> everything ok.
	 */
	if (ret->dll->funs[ret->ordinal].nrofargs<0) {
		ret->dll->funs[ret->ordinal].nrofargs=(LOWORD(context->Esp)-ret->origSP-4)/2;
	}
	context->Eip = LOWORD(ret->origreturn);
	context->SegCs  = HIWORD(ret->origreturn);
        DPRINTF("%04x:RET  %s.%d: %s(",
                GetCurrentThreadId(),ret->dll->name,ret->ordinal,
                ret->dll->funs[ret->ordinal].name);
	if (ret->args) {
		int	i,max;

		max = ret->dll->funs[ret->ordinal].nrofargs;
		if (max>16)
			max=16;
		if (max<0)
			max=0;

		for (i=max;i--;)
			DPRINTF("%04x%s",ret->args[i],i?",":"");
		if (max!=ret->dll->funs[ret->ordinal].nrofargs)
			DPRINTF(" ...");
		HeapFree(GetProcessHeap(),0,ret->args);
		ret->args = NULL;
	}
        DPRINTF(") retval = %04x:%04x ret=%04x:%04x\n",
                (WORD)context->Edx,(WORD)context->Eax,
                HIWORD(ret->origreturn),LOWORD(ret->origreturn));
	ret->origreturn = NULL; /* mark as empty */
}
#else	/* !__i386__ */
void SNOOP16_RegisterDLL(HMODULE16 hModule,LPCSTR name) {
	if (!TRACE_ON(snoop)) return;
	FIXME("snooping works only on i386 for now.\n");
}

FARPROC16 SNOOP16_GetProcAddress16(HMODULE16 hmod,DWORD ordinal,FARPROC16 origfun) {
	return origfun;
}
#endif	/* !__i386__ */
