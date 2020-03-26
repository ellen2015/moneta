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
#include "FileIo.hpp"
#include "Process.hpp"
#include "Memory.hpp"
#include "Interface.hpp"
#include "MemDump.hpp"
#include "Suspicions.hpp"

using namespace std;
using namespace Memory;

wstring Suspicion::GetDescription() {
	switch (this->SuspicionType) {
	case MODIFIED_CODE: return L"Modified code";
	case UNSIGNED_MODULE: return L"Unsigned module";
	case MISSING_PEB_MODULE: return L"Missing PEB module";
	case MISMATCHING_PEB_MODULE: return L"Mismatching PEB module";
	case MODIFIED_HEADER: return L"Modified PE header";
	case DISK_PERMISSION_MISMATCH: return L"Inconsistent +x between disk and memory";
	case XMAP: return L"Abnormal executable memory type";
	case PHANTOM_IMAGE: return L"Phantom image";
	case XPRV: return L"Abnormal executable memory type";
	default: return L"?";
	}
}

Suspicion::Suspicion(Process* ParentProc, Entity* ParentObj, Subregion* Block, Suspicion::Type Type) : ParentProcess(ParentProc), ParentObject(ParentObj), Block(Block), SuspicionType(Type) {}

void Suspicion::EnumerateMap(map <uint8_t*, map<uint8_t*, list<Suspicion *>>>& SuspicionsMap) {
	for (map <uint8_t*, map<uint8_t*, list<Suspicion *>>>::const_iterator AbMapItr = SuspicionsMap.begin(); AbMapItr != SuspicionsMap.end(); ++AbMapItr) {
		printf("0x%p [%d sblocks]\r\n", AbMapItr->first, AbMapItr->second.size());
		for (map<uint8_t*, list<Suspicion *>>::const_iterator SbMapItr = AbMapItr->second.begin(); SbMapItr != AbMapItr->second.end(); SbMapItr++) {
			printf("  0x%p [%d list elements]\r\n", SbMapItr->first, SbMapItr->second.size());
			for (list<Suspicion *>::const_iterator ListItr = SbMapItr->second.begin(); ListItr != SbMapItr->second.end(); ++ListItr) {
				if (!(*ListItr)->IsFullEntitySuspicion()) {
					printf("    0x%p : %d : %ws\r\n", (*ListItr)->GetBlock()->GetBasic()->BaseAddress, (*ListItr)->GetType(), (*ListItr)->GetDescription().c_str());
				}
				else {
					printf("    0x%p : %d : %ws : Full entity\r\n", (*ListItr)->GetParentObject()->GetStartVa(), (*ListItr)->GetType(), (*ListItr)->GetDescription().c_str());
				}
			}
		}
	}
}

/* Inspect entity: generates a list of suspicions for either a region or subregion
   + It is important to ensure that all new suspicions are added to the list of the existing map entry even if they share the same base address. This allows the ablock map entry to also hold sblock suspicions such as modified hdr.
   ~ Create an entry in the primary map with the allocation base as a key and save a reference to the secondary map.
   ~ Create a suspicion list for the ablock and every sblock, ensuring that sblocks with suspicions which share the ablock address (for example modified headers) are adding their suspicions to the same list so there are not 2 map entries for the ablock address.
   ~ In the event that no new lists were added to the secondary map, erase the primary map entry. Otherwise, preserve both the primary and secondary map entries.

	Region map -> Key [Allocation base]
					-> Suspicions map -> Key [Subregion address]
										   -> Suspicions list

*/

bool Suspicion::InspectEntity(Process &ParentProc, Entity &ParentObj, map <uint8_t*, map<uint8_t*, list<Suspicion *>>> &SuspicionsMap) {
	list<Suspicion *> AbSuspList;
	SuspicionsMap.insert(make_pair(ParentObj.GetStartVa(), map<uint8_t*, list<Suspicion *>>()));
	map<uint8_t*, list<Suspicion *>>& RefSbMap = SuspicionsMap.at(ParentObj.GetStartVa());

	switch (ParentObj.GetType()) {
		case Entity::Type::PE_FILE: {
			PeVm::Body* PeEntity = dynamic_cast<PeVm::Body*>(&ParentObj);

			if (!PeEntity->IsNonExecutableImage()) {
				if (!PeEntity->IsSigned()) {
					AbSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, nullptr, UNSIGNED_MODULE));
				}

				if (!PeEntity->GetPebModule().Exists()) {
					AbSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, nullptr, MISSING_PEB_MODULE));
				}
				else {
					if (_wcsicmp(PeEntity->GetPebModule().GetPath().c_str(), PeEntity->GetFileBase()->GetPath().c_str()) != 0) { // Since the PEB module is queried by base address with GetModuleInfo/GetModuleFileNameExW rather than by name with GetModuleHandleEx, there may be a PEB link with a base address matching this image region but with a misleading name/path
						if (ParentProc.IsWow64()) { // This is an edge case in which in Wow64 a module may appear as C:\Windows\System32\kernel32.dll although the true path is C:\Windows\SysWOW64\kernel32.dll due to Wow64 FS redirection.
							wchar_t ReFormattedPath[MAX_PATH + 1] = { 0 };

							if (FileBase::ArchWow64PathExpand(PeEntity->GetPebModule().GetPath().c_str(), ReFormattedPath, MAX_PATH + 1)) {
								if (_wcsicmp(ReFormattedPath, PeEntity->GetFileBase()->GetPath().c_str()) != 0) {
									AbSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, nullptr, MISMATCHING_PEB_MODULE));
								}
							}
						}
						else {
							AbSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, nullptr, MISMATCHING_PEB_MODULE));
						}
					}
				}

				if (PeEntity->GetPe() != nullptr) {
					vector<PeVm::Section*> Sections = PeEntity->GetSections();
					for (vector<PeVm::Section*>::const_iterator SectItr = Sections.begin(); SectItr != Sections.end(); ++SectItr) {
						vector<Subregion*> Subregions = (*SectItr)->GetSubregions();

						for (vector<Subregion*>::iterator SbItr = Subregions.begin(); SbItr != Subregions.end(); ++SbItr) {
							list<Suspicion *> SbSuspList;
							list<Suspicion *>& TargetSuspList = (*SbItr)->GetBasic()->BaseAddress == ParentObj.GetStartVa() ? AbSuspList : SbSuspList;

							if (strcmp(reinterpret_cast<const char*>((*SectItr)->GetHeader()->Name), "Header") == 0 && (*SbItr)->GetPrivateSize()) {
								TargetSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, *SbItr, MODIFIED_HEADER));
							}

							if (Subregion::PageExecutable((*SbItr)->GetBasic()->Protect) && !((*SectItr)->GetHeader()->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
								TargetSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, *SbItr, DISK_PERMISSION_MISMATCH));
							}

							if (Subregion::PageExecutable((*SbItr)->GetBasic()->Protect) && (*SbItr)->GetPrivateSize()) {
								TargetSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, *SbItr, MODIFIED_CODE));
							}

							if (SbSuspList.size()) { // Do not insert the list to the map if it overlaps with the ablock.
								RefSbMap.insert(make_pair((uint8_t *)(*SbItr)->GetBasic()->BaseAddress, SbSuspList));
							}
						}
					}
				}
				else {
					AbSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, nullptr, PHANTOM_IMAGE));
				}
			}

			break;
		}
		case Entity::Type::MAPPED_FILE: {
			vector<Subregion*> Subregions = ParentObj.GetSubregions(); // This must be done explicitly, otherwise each time GetSubregions is called a temporary copy of the list is created and the begin/end iterators will become useless in identifying the end of the list, causing an exception as it loops out of bounds.
			for (vector<Subregion*>::iterator SbItr = Subregions.begin(); SbItr != Subregions.end(); ++SbItr) {
				list<Suspicion *> SbSuspList;
				if (Subregion::PageExecutable((*SbItr)->GetBasic()->Protect)) {
					SbSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, *SbItr, XMAP));
				}
				if (SbSuspList.size()) {
					RefSbMap.insert(make_pair((uint8_t*)(*SbItr)->GetBasic()->BaseAddress, SbSuspList));
				}
			}

			break;
		}
		case Entity::Type::UNKNOWN: {
			vector<Subregion*> Subregions = ParentObj.GetSubregions(); // This must be done explicitly, otherwise each time GetSubregions is called a temporary copy of the list is created and the begin/end iterators will become useless in identifying the end of the list, causing an exception as it loops out of bounds.

			if (Subregions.front()->GetBasic()->Type == MEM_PRIVATE) {
				for (vector<Subregion*>::iterator SbItr = Subregions.begin(); SbItr != Subregions.end(); ++SbItr) {
					list<Suspicion *> SbSuspList;
					if (Subregion::PageExecutable((*SbItr)->GetBasic()->Protect)) {
						SbSuspList.push_back(new Suspicion(&ParentProc, &ParentObj, *SbItr, XPRV));
					}

					if (SbSuspList.size()) {
						RefSbMap.insert(make_pair((uint8_t*)(*SbItr)->GetBasic()->BaseAddress, SbSuspList));
					}
				}
			}

			break;
		}
	}

	if (AbSuspList.size()) {
		RefSbMap.insert(make_pair(ParentObj.GetStartVa(), AbSuspList));
	}

	if (!RefSbMap.size()) {
		SuspicionsMap.erase(ParentObj.GetStartVa());
	}

	return true;
}