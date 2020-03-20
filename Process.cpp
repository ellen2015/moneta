#include "StdAfx.h"
#include "FileIo.hpp"
#include "PeFile.hpp"
#include "Process.hpp"
#include "Memory.hpp"
#include "Interface.hpp"
#include "MemDump.hpp"
#include "Suspicions.hpp"
#include "Signing.hpp"
#include "Thread.hpp"

using namespace std;
using namespace Memory;

Process::~Process() {
	if (this->Handle != nullptr) {
		CloseHandle(this->Handle);
	}

	for (vector<Thread*>::const_iterator Itr = this->Threads.begin(); Itr != this->Threads.end(); ++Itr) {
		delete* Itr;
	}

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		if (Itr->second->GetType() == Entity::Type::PE_FILE) {
			delete dynamic_cast<PeVm::Body*>(Itr->second); // This will call the destructors for PE, mapped file and entity all to be called in inheritted order.
		}
		else if (Itr->second->GetType() == Entity::Type::MAPPED_FILE) {
			delete dynamic_cast<MappedFile*>(Itr->second);
		}
		else {
			delete dynamic_cast<ABlock*>(Itr->second);;
		}
	}
}

#define ThreadQuerySetWin32StartAddress 9

Process::Process(uint32_t dwPid) : Pid(dwPid) {
	//
	// Initialize a new entity for each allocation base and add it to this process address space map
	//

	this->Handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, dwPid);

	if (this->Handle != nullptr) {
		//Interface::Log("* Generating region and subregion blocks.\r\n");

		//
		//
		//

		HANDLE hThreadSnap = INVALID_HANDLE_VALUE;
		THREADENTRY32 ThreadEntry;

		if ((hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)) != INVALID_HANDLE_VALUE) {
			ThreadEntry.dwSize = sizeof(THREADENTRY32);

			if (Thread32First(hThreadSnap, &ThreadEntry)) {
				do
				{
					if (ThreadEntry.th32OwnerProcessID == this->Pid) {
						Thread* CurrentThread = new Thread(ThreadEntry.th32ThreadID);
						//Interface::Log("* TID: 0x%08x\r\n", CurrentThread.GetTid());
						HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, false, CurrentThread->GetTid()); // OpenThreadToken consistently failed even with impersonation (ERROR_NO_TOKEN). The idea was abandoned due to lack of relevance. Get-InjectedThread returns the user as SYSTEM even when it was a regular user which launched the remote thread.

						if (hThread != nullptr) {
							typedef NTSTATUS(NTAPI* NtQueryInformationThread_t) (HANDLE, THREADINFOCLASS, void*, uint32_t, uint32_t*);
							static NtQueryInformationThread_t NtQueryInformationThread = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
							void* pStartAddress = nullptr;
							HANDLE hDupThread = nullptr;

							if (DuplicateHandle(GetCurrentProcess(), hThread, GetCurrentProcess(), &hDupThread, THREAD_QUERY_INFORMATION, FALSE, 0)) { // Without duplicating this handle NtQueryInformationThread will consistently fail to query the start address.
								NTSTATUS NtStatus = NtQueryInformationThread(hDupThread, (THREADINFOCLASS)ThreadQuerySetWin32StartAddress, &pStartAddress, sizeof(pStartAddress), nullptr);

								if (NT_SUCCESS(NtStatus)) {
									CurrentThread->SetEntryPoint(pStartAddress);
									//Interface::Log("* Start address: 0x%p\r\n", CurrentThread.GetEntryPoint());
								}

								CloseHandle(hDupThread);
							}

							this->Threads.push_back(CurrentThread);
							CloseHandle(hThread);
						}
					}
				} while (Thread32Next(hThreadSnap, &ThreadEntry));
			}

			CloseHandle(hThreadSnap);
		}
		//system("pause");

		wchar_t ImageName[MAX_PATH + 1] = { 0 }, DevFilePath[MAX_PATH + 1] = { 0 };

		if (GetModuleBaseNameW(this->Handle, nullptr, ImageName, MAX_PATH + 1) && GetProcessImageFileNameW(this->Handle, DevFilePath, sizeof(DevFilePath))) {
			wchar_t ImageFilePath[MAX_PATH + 1] = { 0 };

			if (FileBase::TranslateDevicePath(DevFilePath, ImageFilePath)) {
				//printf("Translated %ws to %ws\r\n", DevFilePath, ImageFilePath);
				this->Name = wstring(ImageName);
				this->ImageFilePath = wstring(ImageFilePath);
				Interface::Log(VerbosityLevel::Debug, "* Mapping address space of PID %d [%ws]\r\n", this->Pid, this->Name.c_str());
				typedef BOOL(WINAPI* ISWOW64PROCESS) (HANDLE, PBOOL);
				static ISWOW64PROCESS IsWow64Process = (ISWOW64PROCESS)GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "IsWow64Process");

				if (IsWow64Process != nullptr) {
					BOOL bSelfWow64 = FALSE;

					if (IsWow64Process(GetCurrentProcess(), (PBOOL)&bSelfWow64)) {
						if (IsWow64Process(this->Handle, (PBOOL)&this->Wow64)) {
							if (this->IsWow64()) {
								//CloseHandle(this->Handle);
								Interface::Log(VerbosityLevel::Debug, "* PID %d is Wow64\r\n", this->Pid);
								//system("pause");
								//throw 2;
							}
							else {
								if (bSelfWow64) {
									Interface::Log(VerbosityLevel::Debug, "* Cannot scan non-Wow64 process from Wow64 Moneta instance\r\n");
									throw 2;
								}
							}
						}
					}
				}
			}
		}

		Interface::Log(VerbosityLevel::Debug, "* Scanning sblocks...\r\n");
		//system("pause");
		SIZE_T cbRegionSize = 0;
		vector<Subregion*> Subregions;
		vector<Subregion*>::iterator ABlock;
		//Entity* CurrentEntity = nullptr;

		//Loop memory, building list of Subregions. Once a block is found which does not match the "current" allocation base, create a new entity containing the corresponding sblock list, and insert it into the address space entities map using the ablock as the key.

		//if (this->Pid == 3272) system("pause");

		for (uint8_t* pBaseAddr = nullptr;; pBaseAddr += cbRegionSize) {
			MEMORY_BASIC_INFORMATION* pMbi = new MEMORY_BASIC_INFORMATION;

			if (VirtualQueryEx(this->Handle, pBaseAddr, (MEMORY_BASIC_INFORMATION*)pMbi, sizeof(MEMORY_BASIC_INFORMATION)) == sizeof(MEMORY_BASIC_INFORMATION)) {
				cbRegionSize = pMbi->RegionSize;

				if (!Subregions.empty()) { // If the sblock list is empty then there is no ablock for comparison
					//
					// In the event that this is a new ablock, create a map pair and insert it into the entities map
					//

					Interface::Log(VerbosityLevel::Debug, "Sblock list not empty\r\n");

					if (pMbi->AllocationBase != (*ABlock)->GetBasic()->AllocationBase) {
						Interface::Log(VerbosityLevel::Debug, "Found a new ablock. Saving sblock list to new entity entry.\r\n");
						this->Entities.insert(make_pair((uint8_t*)(*ABlock)->GetBasic()->AllocationBase, Entity::Create(this->Handle, Subregions)));
						Subregions.clear();
					}
					//Interface::Log("done2\r\n");
				}

				Interface::Log(VerbosityLevel::Debug, "Adding mew sblock to list\r\n");
				Subregions.push_back(new Subregion(this->Handle, (MEMORY_BASIC_INFORMATION*)pMbi, this->Threads));
				ABlock = Subregions.begin(); // This DOES fix a bug.
			}
			else {
				Interface::Log(VerbosityLevel::Debug, "VirtualQuery failed\r\n");
				//system("pause");
				delete pMbi;
				if (!Subregions.empty()) { // Edge case: new ablock not yet found but finished enumerating sblocks.
					this->Entities.insert(make_pair((uint8_t*)(*ABlock)->GetBasic()->AllocationBase, Entity::Create(this->Handle, Subregions)));
				}
				//Interface::Log("done\r\n");
				break;
			}
		}

		//this->PermissionRecords = new MemoryPermissionRecord(this->Blocks); // Initialize 
		//CloseHandle(hProcess);
	}
	else {
		Interface::Log(VerbosityLevel::Debug, "- Failed to open handle to PID %d\r\n", this->Pid);
		throw 1; // Not throwing a specific value crashes it
	}

	//Interface::Log("* Finished generating region and subregion blocks\r\n");
}

void AlignName(const wchar_t* pOriginalName, wchar_t* pAlignedName, int32_t nAlignTo) { // Make generic and move to interface?
	assert(nAlignTo >= 1);
	assert(wcslen(pOriginalName) <= nAlignTo);

	if (wcslen(pOriginalName)) {
		wcsncpy_s(pAlignedName, (nAlignTo + 1), pOriginalName, nAlignTo);
		for (int32_t nX = wcslen(pAlignedName); nX < nAlignTo; nX++) {
			wcscat_s(pAlignedName, (nAlignTo + 1), L" ");
		}
	}
	else {
		wcscpy_s(pAlignedName, (nAlignTo + 1), L" ");

		for (int32_t nX = 1; nX < nAlignTo; nX++) {
			wcscat_s(pAlignedName, (nAlignTo + 1), L" ");
		}
	}
}

/*

ArchWow64PathExpand

The purpose of this function is to receive an unformatted file path (which may
contain architecture folders or environment variables) and convert the two
ambiguous architecture directories to Wow64 if applicable.

1. Expand all environment variables.
2. Check whether the path begins with either of the ambiguous architecture
folders: C:\Windows\system32, C:\Program Files
3. If the path does not begin with an ambiguous arch folder return it as is.
4. If the path does begin with an ambiguous arch folder then convert it to
the Wow64 equivalent and return it.

Examples:

%programfiles%\example1\example.exe -> C:\Program Files (x86)\example1\example.exe
C:\Program Files (x86)\example2\example.exe -> C:\Program Files (x86)\example2\example.exe
C:\Program Files\example3\example.exe -> C:\Program Files (x86)\example3\example.exe
C:\Windows\system32\notepad.exe -> C:\Windows\syswow64\notepad.exe

*/

void EnumerateThreads(const wstring Indent, vector<Thread*> Threads) {
	for (vector<Thread*>::iterator ThItr = Threads.begin(); ThItr != Threads.end(); ++ThItr) {
		Interface::Log("%wsThread 0x%p [TID 0x%08x]\r\n", Indent.c_str(), (*ThItr)->GetEntryPoint(), (*ThItr)->GetTid());
	}
}

/*
Region map -> Key [Allocation base]
				-> Suspicions map -> Key [Subregion address]
									   -> Suspicions list
*/

int32_t FilterSuspicions(map <uint8_t*, map<uint8_t*, list<Suspicion *>>>&SuspicionsMap) {
	//Interface::Log("* Filtering suspicions...\r\n");
	//printf("before:\r\n");
	//Suspicion::EnumerateMap(SuspicionsMap);
	bool bReWalkMap = false;

	do {
		if (bReWalkMap) {
			bReWalkMap = false; // The re-walk boolean is only set when a suspicion was filtered. Reset it each time this happens.
		}

		/* Concept ~ Walk the map and search through the suspicion list corresponding to each sblock.
		             When a suspicion is filtered, remove it from the list, and remove the sblock map
					 entry if it was the only suspicion in its list. If the ablock map only had the
					 one sblock map entry, then remove the ablock map entry as well. Walk the list
					 again now that the map has updated, and repeat the process until there are no
					 filterable suspicions remaining
		
		*/
		for (map <uint8_t*, map<uint8_t*, list<Suspicion *>>>::const_iterator AbMapItr = SuspicionsMap.begin(); !bReWalkMap && AbMapItr != SuspicionsMap.end(); ++AbMapItr) {
			map < uint8_t*, list<Suspicion *>>& RefSbMap = SuspicionsMap.at(AbMapItr->first);
			int32_t nSbIndex = 0;

			for (map<uint8_t*, list<Suspicion *>>::const_iterator SbMapItr = AbMapItr->second.begin(); !bReWalkMap && SbMapItr != AbMapItr->second.end(); ++SbMapItr, nSbIndex++) {
				//list<Suspicion *> SuspListCopy = SbMapItr->second;
				//list<Suspicion *>::const_iterator SuspCopyItr = SuspListCopy.begin();
				list<Suspicion *>& RefSuspList = RefSbMap.at(SbMapItr->first);
				//list<Suspicion *>& RefSuspList = reinterpret_cast<list<Suspicion *>>(SbMapItr->second); // Bug: element removed from list which is still being iterated
				list<Suspicion *>::const_iterator SuspItr = SbMapItr->second.begin();

				for (int32_t nSuspIndex = 0; !bReWalkMap && SuspItr != SbMapItr->second.end(); ++SuspItr, nSuspIndex++) {
					switch ((*SuspItr)->GetType()) {
					/*case Suspicion::Type::XPRV: {
						//Interface::Log("* Filtered executable private memory at 0x%p\r\n", (*SuspItr)->GetBlock()->GetBasic()->BaseAddress);
						bReWalkMap = true;
						RefSuspList.erase(SuspItr);

						if (!RefSuspList.size()) {
							//
							// Erase the suspicion list from the sblock map and then erase the sblock map from the ablock map. Finalize by removing the ablock map from the suspicion map itself.
							//

							RefSbMap.erase(SbMapItr);

							if (!RefSbMap.size()) {
								SuspicionsMap.erase(AbMapItr); // Will this cause a bug if multiple suspicions are erased in one call to this function?
							}
						}
						break;
					}*/
					case Suspicion::Type::MISSING_PEB_MODULE: {
						/* Filter cases for missing PEB modules:
							 ~ Signed metadata PEs. These appear in the C:\Windows\System32\WinMetadata folder with the .winmd extension. They've also been noted to appear in WindpwsApps, SystemApps and others.

							   0x000000000F3E0000:0x0009e000 | Executable image | C:\Windows\System32\WinMetadata\Windows.UI.winmd | Missing PEB module
							   0x000000000F3E0000:0x0009e000 | R        | Header   | 0x00000000
							   0x000000000F3E0000:0x0009e000 | R        | .text    | 0x00000000
							   0x000000000F3E0000:0x0009e000 | R        | .rsrc    | 0x00000000
						*/

						PeVm::Body* PeEntity = dynamic_cast<PeVm::Body*>((*SuspItr)->GetParentObject());

						if (PeEntity->IsSigned()) {
							static const wchar_t* pWinmbExt = L".winmd";
							//if (_wcsnicmp(PeEntity->GetPath().c_str(), Environment::MetadataPath.c_str(), Environment::MetadataPath.length()) == 0 && _wcsicmp(PeEntity->GetPath().c_str() + PeEntity->GetPath().length() - wcslen(pWinmbExt), pWinmbExt) == 0) {
							if (_wcsicmp(PeEntity->GetFileBase()->GetPath().c_str() + PeEntity->GetFileBase()->GetPath().length() - wcslen(pWinmbExt), pWinmbExt) == 0) {
								if (PeEntity->GetPe() != nullptr && PeEntity->GetPe()->GetEntryPoint() == 0) {
									//Interface::Log("* %ws is within metadata path\r\n", PeEntity->GetPath().c_str());
									//system("pause");

									bReWalkMap = true;
									RefSuspList.erase(SuspItr);

									if (!RefSuspList.size()) {
										//
										// Erase the suspicion list from the sblock map and then erase the sblock map from the ablock map. Finalize by removing the ablock map from the suspicion map itself.
										//

										RefSbMap.erase(SbMapItr);

										if (!RefSbMap.size()) {
											SuspicionsMap.erase(AbMapItr); // Will this cause a bug if multiple suspicions are erased in one call to this function?
										}
									}
								}
							}
						}

						break;
					}
					}
				}
			}
		}
	} while (bReWalkMap);

	//Suspicion::EnumerateMap(SuspicionsMap);
	//printf("enum done\r\n");
	//Interface::Log("* Done filtering suspicions.\r\n");
	return 0;
}

int32_t AppendOverlapSuspicion(map<uint8_t*, list<Suspicion *>>* pSbMap, uint8_t *pSbAddress, bool bEntityTop) {
	if (pSbMap != nullptr && pSbMap->count(pSbAddress)) {
		list<Suspicion *>& SuspicionsList = pSbMap->at(pSbAddress);

		for (list<Suspicion *>::const_iterator SuspItr = SuspicionsList.begin(); SuspItr != SuspicionsList.end(); ++SuspItr) {
			if (bEntityTop == (*SuspItr)->IsFullEntitySuspicion()) {
				Interface::Log(" | ");
				Interface::Log(12, "%ws", (*SuspItr)->GetDescription().c_str());
			}
		}
	}
}

int32_t SubEntitySuspCount(map<uint8_t*, list<Suspicion*>>* pSbMap, uint8_t* pSbAddress) {
	int32_t nCount = 0;

	if (pSbMap != nullptr && pSbMap->count(pSbAddress)) {
		list<Suspicion*>& SuspicionsList = pSbMap->at(pSbAddress);

		for (list<Suspicion*>::const_iterator SuspItr = SuspicionsList.begin(); SuspItr != SuspicionsList.end(); ++SuspItr) {
			if (!(*SuspItr)->IsFullEntitySuspicion()) {
				nCount++;
			}
		}
	}

	return nCount;
}

bool Process::DumpBlock(MemDump &ProcDmp, MEMORY_BASIC_INFORMATION *pMbi, wstring Indent) {
	wchar_t DumpFilePath[MAX_PATH + 1] = { 0 };

	if (pMbi->State == MEM_COMMIT) {
		if (ProcDmp.Create(pMbi, DumpFilePath, MAX_PATH + 1)) {
			Interface::Log("%ws~ Memory dumped to %ws\r\n", Indent.c_str(), DumpFilePath);
			return true;
		}
		else {
			Interface::Log("%ws~ Memory dump failed.\r\n", Indent.c_str());
			return false;
		}
	}
}
/*
1. Loop entities to build suspicions list
2. Filter suspicions
3. Loop entities for enumeration if:
   mselect == process
   mselect == sblock and this eneity contains the sblock
   mselect == suspicious and there is 1 or more suspicions
4. Show the process if it has not been shown before
5. Display entity info (exe image, private, mapped + total size) ALWAYS (criteria already applied going into loop) along with suspicions (if any)
5. For PEs, loop sblocks/sections. Enum if:
	mselect == process
	mselect == sblock && sblock == current, or the  "from base" option is set
	or mselect == suspicious and the current sblock has a suspicion or the  "from base" option is set
6. Dump the current sblock based on the same criteria as above but ONLY if the "from base" option is not set.
7. Dump the entire PE entity if it met the initial enum criteria and "from base" option is set

8. For private/mapped loop sblocks and enum if:
	mselect == process
	mselect == sblock && sblock == current, or the  "from base" option is set
	or mselect == suspicious and the current sblock has a suspicion or the  "from base" option is set
9. Dump the current sblock based on the same criteria as above but ONLY if the "from base" option is not set.
10. Dump the entire entity if it met the initial enum criteria and "from base" option is set
*/

vector<Subregion*> Process::Enumerate(uint64_t qwOptFlags, MemorySelectionType MemSelectType, uint8_t *pSelectSblock) {
	bool bShownProc = false;
	MemDump ProcDmp(this->Handle, this->Pid);
	wstring_convert<codecvt_utf8_utf16<wchar_t>> UnicodeConverter;
	map <uint8_t*, map<uint8_t*, list<Suspicion *>>> SuspicionsMap; // More efficient to only filter this map once. Currently filtering it for every single entity
	vector<Subregion*> SelectedSbrs;

	//
	// Build suspicions list for following memory selection and apply filters to it.
	//

	//Interface::Log("* Inspecting/filtering entities...\r\n");

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		Suspicion::InspectEntity(*this, *Itr->second, SuspicionsMap);
	}

	if (SuspicionsMap.size()) {
		FilterSuspicions(SuspicionsMap);
	}

	//Interface::Log("* Finished inspecting/filtering entities.\r\n");

	//
	// Display information on each selected sblock and/or entity within the process address space
	//

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		auto AbMapItr = SuspicionsMap.find(Itr->second->GetStartVa()); // An iterator into the main ablock map which points to the entry for the sb map.
		map<uint8_t*, list<Suspicion *>>* pSbMap = nullptr;

		if (AbMapItr != SuspicionsMap.end()) {
			pSbMap = &SuspicionsMap.at(Itr->second->GetStartVa());
		}

		if (MemSelectType == MemorySelectionType::All ||
			(MemSelectType == MemorySelectionType::Block && ((pSelectSblock >= Itr->second->GetStartVa()) && (pSelectSblock < Itr->second->GetEndVa()))) ||
			(MemSelectType == MemorySelectionType::Suspicious && AbMapItr != SuspicionsMap.end())) {

			//
			// Display process and/or entity information: the criteria has already been met for this to be done without further checks
			//

			if (!bShownProc) {
				Interface::Log("\r\n");
				Interface::Log(11, "%ws", this->Name.c_str()); // 13
				Interface::Log(" : ");
				Interface::Log(11, "%d", this->GetPid());
				Interface::Log(" : ");
				Interface::Log(11, "%ws", this->IsWow64() ? L"Wow64" : L"x64");
				Interface::Log(" : ");
				Interface::Log(11, "%ws\r\n", this->ImageFilePath.c_str());
				//Interface::Log("]\r\n");
				bShownProc = true;
			}

			if(Itr->second->GetSubregions().front()->GetBasic()->State != MEM_FREE)
			Interface::Log("  0x%p:0x%08x   ", Itr->second->GetStartVa(), Itr->second->GetEntitySize()); // 13
			if (Itr->second->GetType() == Entity::Type::PE_FILE) {
				PeVm::Body* PeEntity = dynamic_cast<PeVm::Body*>(Itr->second);

				//Interface::Log(13, "  0x%p:0x%08x", PeEntity->GetPeFile(), PeEntity->GetEntitySize());
				//Interface::Log("   | ");
				Interface::Log("| ");
				if (PeEntity->IsNonExecutableImage()) {
					Interface::Log(6, "Unexecutable image  "); //11
					//Interface::Log("  0x%p:0x%08x   | Unexecutable image  | %ws", PeEntity->GetPeFile(), PeEntity->GetEntitySize(), PeEntity->GetFileBase()->GetPath().c_str());
				}
				else {
					Interface::Log(6, "Executable image    ");
					//Interface::Log("  0x%p:0x%08x   | Executable image    | %ws", PeEntity->GetPeFile(), PeEntity->GetEntitySize(), PeEntity->GetFileBase()->GetPath().c_str());
				}
				Interface::Log("| %ws", PeEntity->GetFileBase()->GetPath().c_str());
			}
			else if (Itr->second->GetType() == Entity::Type::MAPPED_FILE) {
				Interface::Log("| ");
				Interface::Log(6, "Mapped");
				Interface::Log("   | %ws", dynamic_cast<MappedFile*>(Itr->second)->GetFileBase()->GetPath().c_str());
			}
			else {
				//Interface::Log("  0x%p:0x%08x   | %ws", Itr->second->GetStartVa(), Itr->second->GetEntitySize(), Subregion::AttribDesc(Itr->second->GetSubregions().front()->GetBasic())); // Free memory presents the only exception here, as it has a blank type. While such memory can paint a slightly more detailed picture of a process memory space, it has no allocation base and no type which makes it impossible to parse/enumerate in the style in which this program was written.
				if (Itr->second->GetSubregions().front()->GetBasic()->Type == MEM_PRIVATE) {
					//Interface::Log(13, "  0x%p:0x%08x   ");
					Interface::Log("| ");
					Interface::Log(6, "Private");
				}
				else {
					continue;
				}
			}

			//
			// Display suspicions associated with the entity, if the current entity has any suspicions associated with it
			//

			AppendOverlapSuspicion(pSbMap, (uint8_t*)Itr->second->GetStartVa(), true);
			Interface::Log("\r\n");

			if (Interface::GetVerbosity() == VerbosityLevel::Detail) {
				if (Itr->second->GetType() == Entity::Type::PE_FILE) {
					PeVm::Body* PeEntity = dynamic_cast<PeVm::Body*>(Itr->second);

					Interface::Log("  |__ Mapped file base: 0x%p\r\n", PeEntity->GetStartVa());
					Interface::Log("    | Mapped file size: %d\r\n", PeEntity->GetEntitySize());
					Interface::Log("    | Mapped file path: %ws\r\n", PeEntity->GetFileBase()->GetPath().c_str());
					Interface::Log("    | Size of image: %d\r\n", PeEntity->GetImageSize());
					Interface::Log("    | Non-executable: %ws\r\n", PeEntity->IsNonExecutableImage() ? L"yes" : L"no");
					Interface::Log("    | Partially mapped: %ws\r\n", PeEntity->IsPartiallyMapped() ? L"yes" : L"no");
					Interface::Log("    | Signed: %ws\r\n", PeEntity->IsSigned() ? L"yes" : L"no");
					Interface::Log("    | Signing level: %ws\r\n", TranslateSigningLevel(PeEntity->GetSigningLevel()));
					Interface::Log("    |__ PEB module");

					if (PeEntity->GetPebModule().Exists()) {
						Interface::Log("\r\n");
						Interface::Log("      | Name: %ws\r\n", PeEntity->GetPebModule().GetName().c_str());
						Interface::Log("      | Image base: 0x%p\r\n", PeEntity->GetPebModule().GetBase());
						Interface::Log("      | Image size: %d\r\n", PeEntity->GetPebModule().GetSize());
						Interface::Log("      | Entry point: 0x%p\r\n", PeEntity->GetPebModule().GetEntryPoint());
						Interface::Log("      | Image file path: %ws\r\n", PeEntity->GetPebModule().GetPath().c_str());
					}
					else {
						Interface::Log(" (missing)\r\n");
					}
				}
				else if (Itr->second->GetType() == Entity::Type::MAPPED_FILE) {
					Interface::Log("  |__ Mapped file base: 0x%p\r\n", Itr->second->GetStartVa());
					Interface::Log("    | Mapped file size: %d\r\n", Itr->second->GetEntitySize());
					Interface::Log("    | Mapped file path: %ws\r\n", dynamic_cast<MappedFile*>(Itr->second)->GetFileBase()->GetPath().c_str());
				}

				/*
				if (Itr->second->GetRegionInfo() != nullptr) {
					// Due to flag inconsistency between architectures and different Windows version MEMORY_REGION_INFORMATION has been excluded
				}
				*/
			}

			//
			// Display the section/sblock information associated with this eneity provided it meets the selection criteria
			//

			vector<Subregion*> Subregions = Itr->second->GetSubregions();

			for (vector<Subregion*>::iterator SbItr = Subregions.begin(); SbItr != Subregions.end(); ++SbItr) {
				if (MemSelectType == MemorySelectionType::All ||
					(MemSelectType == MemorySelectionType::Block && (pSelectSblock == (*SbItr)->GetBasic()->BaseAddress || (qwOptFlags & MONETA_FLAG_FROM_BASE))) ||
					(MemSelectType == MemorySelectionType::Suspicious && ((qwOptFlags & MONETA_FLAG_FROM_BASE) || 
																		  (pSbMap != nullptr &&
																		   pSbMap->count((uint8_t*)(*SbItr)->GetBasic()->BaseAddress)) &&
																		   SubEntitySuspCount(pSbMap, (uint8_t*)(*SbItr)->GetBasic()->BaseAddress) > 0))) {
					wchar_t AlignedAttribDesc[9] = { 0 };

					/*
					if (!(*SbItr)->GetPrivateSize()) { // Doing this here will not work since private size is needed for suspicion gathering
						(*SbItr)->SetPrivateSize((*SbItr)->QueryPrivateSize()); //Performance optimization: only query the working set on selected regions/subregions. Doing it on every block of enumerated memory slows scans down substantially.
					}
					*/

					AlignName(Subregion::AttribDesc((*SbItr)->GetBasic()), AlignedAttribDesc, 8);

					if (Itr->second->GetType() == Entity::Type::PE_FILE && !dynamic_cast<PeVm::Body*>(Itr->second)->GetFileBase()->IsPhantom()) {
						//
						// Generate a list of all sections overlapping with this sblock and display them all. A typical example is a +r sblock at the end of the PE which encompasses all consecutive readonly sections ie. .rdata, .rsrc, .reloc
						//
						vector<PeVm::Section*> OverlapSections = dynamic_cast<PeVm::Body*>(Itr->second)->FindOverlapSect(*(*SbItr));

						if (OverlapSections.empty()) {
							Interface::Log("    0x%p:0x%08x | %ws | ?        | 0x%08x", (*SbItr)->GetBasic()->BaseAddress, (*SbItr)->GetBasic()->RegionSize, AlignedAttribDesc, (*SbItr)->GetPrivateSize());
							AppendOverlapSuspicion(pSbMap, (uint8_t*)(*SbItr)->GetBasic()->BaseAddress, false);
							Interface::Log("\r\n");
						}
						else{
							for (vector<PeVm::Section*>::const_iterator SectItr = OverlapSections.begin(); SectItr != OverlapSections.end(); ++SectItr) {
								wchar_t AlignedSectName[9] = { 0 };
								char AnsiSectName[9];

								strncpy_s(AnsiSectName, 9, (char*)(*SectItr)->GetHeader()->Name, 8);
								wstring UnicodeSectName = UnicodeConverter.from_bytes(AnsiSectName);
								AlignName((const wchar_t*)UnicodeSectName.c_str(), AlignedSectName, 8);

								Interface::Log("    0x%p:0x%08x | %ws | %ws | 0x%08x", (*SbItr)->GetBasic()->BaseAddress, (*SbItr)->GetBasic()->RegionSize, AlignedAttribDesc, AlignedSectName, (*SbItr)->GetPrivateSize());
								AppendOverlapSuspicion(pSbMap, (uint8_t*)(*SbItr)->GetBasic()->BaseAddress, false);
								Interface::Log("\r\n");

							}
						}
					}
					else {
						Interface::Log("    0x%p:0x%08x | %ws | 0x%08x", (*SbItr)->GetBasic()->BaseAddress, (*SbItr)->GetBasic()->RegionSize, AlignedAttribDesc, (*SbItr)->GetPrivateSize());
						AppendOverlapSuspicion(pSbMap, (uint8_t*)(*SbItr)->GetBasic()->BaseAddress, false);
						Interface::Log("\r\n");
					}

					if (Interface::GetVerbosity() == VerbosityLevel::Detail) {
						Interface::Log("    |__ Base address: 0x%p\r\n", (*SbItr)->GetBasic()->BaseAddress);
						Interface::Log("      | Size: 0x%d\r\n", (*SbItr)->GetBasic()->RegionSize);
						Interface::Log("      | Permissions: %ws\r\n", Subregion::ProtectSymbol((*SbItr)->GetBasic()->Protect));
						Interface::Log("      | Type: %ws\r\n", Subregion::TypeSymbol((*SbItr)->GetBasic()->Type));
						Interface::Log("      | State: %ws\r\n", Subregion::StateSymbol((*SbItr)->GetBasic()->State));
						Interface::Log("      | Allocation base: 0x%p\r\n", (*SbItr)->GetBasic()->AllocationBase);
						Interface::Log("      | Allocation permissions: %ws\r\n", Subregion::ProtectSymbol((*SbItr)->GetBasic()->AllocationProtect));
						Interface::Log("      | Private size: %d [%d pages]\r\n", (*SbItr)->GetPrivateSize(), (*SbItr)->GetPrivateSize() / 0x1000);
					}

					EnumerateThreads(L"      ", (*SbItr)->GetThreads());

					if ((qwOptFlags & MONETA_FLAG_MEMDUMP)) {
						if (!(qwOptFlags & MONETA_FLAG_FROM_BASE)) {
							this->DumpBlock(ProcDmp, (*SbItr)->GetBasic(), L"      ");
						}
					}

					SelectedSbrs.push_back(*SbItr);
				}
			}

			if ((qwOptFlags & MONETA_FLAG_MEMDUMP)) {
				if ((qwOptFlags & MONETA_FLAG_FROM_BASE)) {
					if (Entity::Dump(ProcDmp, *Itr->second)) {
						Interface::Log("      ~ Generated full region dump at 0x%p\r\n", Itr->second->GetStartVa());
					}
					else {
						Interface::Log("      ~ Failed to generate full region dump at 0x%p\r\n", Itr->second->GetStartVa());
					}
				}
			}
		}
	}

	return SelectedSbrs;
}
