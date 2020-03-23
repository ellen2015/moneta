/*
__________________________________________________________________________________________
| _______  _____  __   _ _______ _______ _______                                         |
| |  |  | |     | | \  | |______    |    |_____|                                         |
| |  |  | |_____| |  \_| |______    |    |     |                                         |
|________________________________________________________________________________________|
| Moneta ~ Usermode memory scanner & malware hunter                                      |
|----------------------------------------------------------------------------------------|
| https://www.forrest-orr.net/post/malicious-memory-artifacts-part-ii-bypassing-scanners |
|----------------------------------------------------------------------------------------|
| Author: Forrest Orr - 2020                                                             |
|----------------------------------------------------------------------------------------|
| Contact: forrest.orr@protonmail.com                                                    |
|----------------------------------------------------------------------------------------|
| Licensed under GNU GPLv3                                                               |
|________________________________________________________________________________________|
| ## Features                                                                            |
|                                                                                        |
| ~ Query the memory attributes of any accessible process(es).                           |
| ~ Identify private, mapped and image memory.                                           |
| ~ Correlate regions of memory to their underlying file on disks.                       |
| ~ Identify PE headers and sections corresponding to image memory.                      |
| ~ Identify modified regions of mapped image memory.                                    |
| ~ Identify abnormal memory attributes indicative of malware.                           |
| ~ Create memory dumps of user-specified memory ranges                                  |
| ~ Calculate memory permission/type statistics                                          |
|________________________________________________________________________________________|

*/

#include "StdAfx.h"
#include "Memory.hpp"
#include "Interface.hpp"
#include "Thread.hpp"

using namespace std;
using namespace Memory;

Subregion::Subregion(HANDLE hProcess, MEMORY_BASIC_INFORMATION* Mbi, vector<Thread*> Threads1) : ProcessHandle(hProcess), Basic(Mbi) {
	for (vector<Thread*>::const_iterator ThItr = Threads1.begin(); ThItr != Threads1.end(); ++ThItr) {
		if ((*ThItr)->GetEntryPoint() >= this->Basic->BaseAddress && (*ThItr)->GetEntryPoint() < ((uint8_t*)this->Basic->BaseAddress + this->Basic->RegionSize)) {
			this->Threads.push_back(new Thread((*ThItr)->GetTid(), (*ThItr)->GetEntryPoint()));
		}
	}

	if (Mbi->State == MEM_COMMIT && Mbi->Type != MEM_PRIVATE) {
		this->PrivateSize = this->QueryPrivateSize(); // This is the most thorough way to query this data however it is a major performance drain. Working set queries have been moved to only occur on selected subregion blocks.
	}
}

Subregion::~Subregion() {
	if (this->Basic != nullptr) {
		delete Basic;
	}

	for (vector<Thread*>::const_iterator Itr = this->Threads.begin(); Itr != this->Threads.end(); ++Itr) {
		delete* Itr;
	}
}

void Subregion::SetPrivateSize(uint32_t dwPrivateSize) {
	this->PrivateSize = dwPrivateSize;
}

const wchar_t* Subregion::ProtectSymbol(uint32_t dwProtect) {
	switch (dwProtect) {
	case PAGE_READONLY:
		return L"R";
	case PAGE_READWRITE:
		return L"RW";
	case PAGE_EXECUTE_READ:
		return L"RX";
	case PAGE_EXECUTE_READWRITE:
		return L"RWX";
	case PAGE_EXECUTE_WRITECOPY:
		return L"RWXC";
	case PAGE_EXECUTE:
		return L"X";
	case PAGE_WRITECOPY:
		return L"WC";
	case PAGE_NOACCESS:
		return L"NA";
	case PAGE_WRITECOMBINE:
		return L"WCB";
	case PAGE_GUARD:
		return L"PG";
	case PAGE_NOCACHE:
		return L"NC";
	default: return L"?";
	}
}

const wchar_t* Subregion::StateSymbol(uint32_t dwState) {
	if (dwState == MEM_COMMIT) {
		return L"Commit";
	}
	else if (dwState == MEM_FREE) {
		return L"Free";
	}
	else if (dwState == MEM_RESERVE) {
		return L"Reserve";
	}
	else {
		return L"?";
	}
}

const wchar_t* Subregion::AttribDesc(MEMORY_BASIC_INFORMATION* Mbi) {
	if (Mbi->State == MEM_COMMIT) {
		return ProtectSymbol(Mbi->Protect);
	}
	else if (Mbi->State == MEM_FREE) {
		return L"Free";
	}
	else if (Mbi->State == MEM_RESERVE) {
		return L"Reserve";
	}

	return L"?";
}

const wchar_t* Subregion::TypeSymbol(uint32_t dwType) {
	if (dwType == MEM_IMAGE) {
		return L"IMG";
	}
	else if (dwType == MEM_MAPPED) {
		return L"MAP";
	}
	else if (dwType == MEM_PRIVATE) {
		return L"PRV";
	}
	else {
		return L"?";
	}
}

uint32_t Subregion::QueryPrivateSize() {
	uint32_t dwPrivateSize = 0;

	if (this->Basic->State == MEM_COMMIT && this->Basic->Protect != PAGE_NOACCESS) {
		PSAPI_WORKING_SET_EX_INFORMATION* pWorkingSets = new PSAPI_WORKING_SET_EX_INFORMATION;
		uint32_t dwWorkingSetsSize = sizeof(PSAPI_WORKING_SET_EX_INFORMATION);

		for (uint32_t dwPageOffset = 0; dwPageOffset < this->Basic->RegionSize; dwPageOffset += 0x1000) {
			pWorkingSets->VirtualAddress = ((uint8_t*)this->Basic->BaseAddress + dwPageOffset);
			if (K32QueryWorkingSetEx(this->ProcessHandle, pWorkingSets, dwWorkingSetsSize)) {
				//Interface::Log("+ Successfully queried working set at 0x%p\r\n", pWorkingSets->VirtualAddress);
				if (!pWorkingSets->VirtualAttributes.Shared) {
					//Interface::Log("* Page at 0x%p is shared\r\n", pWorkingSets->VirtualAddress);
					dwPrivateSize += 0x1000;
				}
			}
			else {
				Interface::Log("- Failed to query working set at 0x%p\r\n", pWorkingSets->VirtualAddress);
			}
		}
	}

	return dwPrivateSize;
}

bool Subregion::PageExecutable(uint32_t dwProtect) {
	return (dwProtect == PAGE_EXECUTE || dwProtect == PAGE_EXECUTE_READ || dwProtect == PAGE_EXECUTE_READWRITE);
}