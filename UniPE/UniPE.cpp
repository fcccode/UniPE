#include <unicorn/unicorn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <iostream>
#include <set>
#include <map>
#include <string> 


#include <Windows.h>

#include "UC_Windows.h"

using namespace std;

typedef map<uint64_t, std::string> TImportNameResolver;

TImportNameResolver ImportNameResolver;

typedef struct {
	uint64_t EntryPoint;
	uint64_t ImageBase;
	uint64_t ImageSize;
	// For clean up
	uint8_t *VirtualImage;
} *PSPEImage, SPEImage;


static void hook_Imports(uc_engine *uc, uint64_t address, uint32_t size, void *user_data);

bool isImportKnown(uint64_t Address) {
	if (ImportNameResolver.find(Address) != ImportNameResolver.end()) {
		return true;
	}

	return false;
}

void hookImport(uc_engine *uc, uint64_t Address) {
	uc_hook Import;
	uc_err err = uc_hook_add(uc, &Import, UC_HOOK_BLOCK, hook_Imports, nullptr, Address, Address);
	if (err) {
		printf("Failed on uc_hook_add() with error returned %u: %s\n",
			err, uc_strerror(err));
	}
}

void hookImports(uc_engine *uc) {
	for (auto &Entry : ImportNameResolver) {
		hookImport(uc, Entry.first);
	}


	// Always hook this APIs
	HMODULE hDLL = LoadLibrary("kernel32.dll");
	FARPROC Proc = GetProcAddress(hDLL, "GetProcAddress");
	if (isImportKnown((uint64_t)Proc) == false) {
		ImportNameResolver[(uint64_t)Proc] = "kernel32.dll_GetProcAddress";
		hookImport(uc, (uint64_t)Proc);
	}

	Proc = GetProcAddress(hDLL, "LoadLibraryA");
	if (isImportKnown((uint64_t)Proc) == false) {
		ImportNameResolver[(uint64_t)Proc] = "kernel32.dll_LoadLibraryA";
		hookImport(uc, (uint64_t)Proc);
	}

	Proc = GetProcAddress(hDLL, "LoadLibraryW");
	if (isImportKnown((uint64_t)Proc) == false) {
		ImportNameResolver[(uint64_t)Proc] = "kernel32.dll_LoadLibraryW";
		hookImport(uc, (uint64_t)Proc);
	}
}

void dumpFile(uint8_t *VirtualImage, uint32_t ImageBase, uint32_t ImageSize) {
	char Buffer[1024];
	sprintf(Buffer, "g:/Cracking/UnicornBasedAttacks/Dump_%08X.exe", ImageBase);
	FILE *fp = fopen(Buffer, "wb");
	fwrite(VirtualImage, ImageSize, 1, fp);
	fclose(fp);
}

bool loadPE64(uc_engine *uc, char *FilePath, PSPEImage PEImage) {
	FILE *fp = fopen(FilePath, "rb");
	if (!fp) {
		return 0;
	}

	// Get FileSize
	fseek(fp, 0, SEEK_END);
	size_t FileSize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	// Alloc Mem
	uint8_t *FileMem = (uint8_t *)calloc(1, FileSize);

	// Read and close
	fread(FileMem, 1, FileSize, fp);
	fclose(fp);


	PIMAGE_DOS_HEADER pVDosHeader = (PIMAGE_DOS_HEADER) FileMem;
	PIMAGE_NT_HEADERS pVNTHeaders = (PIMAGE_NT_HEADERS)(((BYTE *)pVDosHeader) + (pVDosHeader->e_lfanew));	

	// Allocate Virtual Image
	//uint8_t *VImage = (uint8_t *) calloc(1, pVNTHeaders->OptionalHeader.SizeOfImage);		
	uint8_t *VImage = (uint8_t *)VirtualAlloc(NULL, pVNTHeaders->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	memset(VImage, 0, pVNTHeaders->OptionalHeader.SizeOfImage);
	
	// Get ImageBase
	uint64_t ImageBase = pVNTHeaders->OptionalHeader.ImageBase;
	uint64_t EP = pVNTHeaders->OptionalHeader.AddressOfEntryPoint;

	// Parse the sections and load image into memory	
	PIMAGE_SECTION_HEADER psectionheader = (PIMAGE_SECTION_HEADER)(pVNTHeaders + 1);	
	for (int i = 0; i < pVNTHeaders->FileHeader.NumberOfSections; i++) {
		uint64_t VA = psectionheader->VirtualAddress;
		uint64_t VASize = psectionheader->Misc.VirtualSize;

		uint64_t RawOffset = psectionheader->PointerToRawData;
		uint64_t RawSize = psectionheader->SizeOfRawData;

		//copy information
		memcpy(VImage + VA, FileMem + RawOffset, (size_t) RawSize);

		//Copy the header
		if (i == 0) {			
			memcpy(VImage, FileMem, (size_t)RawOffset);
		}

		psectionheader++;
	}

	// Parse ImportTable

	// Apply relocs (only if we choose a different imagebase)

	// Map memory to unicorn
	uc_err err = uc_mem_map(uc, ImageBase, pVNTHeaders->OptionalHeader.SizeOfImage, UC_PROT_ALL);
	if (err) {
		free(FileMem);
		printf("Failed on uc_mem_map() with error returned %u: %s\n", err, uc_strerror(err));
		return 0;
	}

	err = uc_mem_write(uc, ImageBase, VImage, pVNTHeaders->OptionalHeader.SizeOfImage);
	if (err) {
		free(FileMem);
		printf("Failed on uc_mem_write() with error returned %u: %s\n", err, uc_strerror(err));
		return 0;
	}		

	//Fill return struct
	PEImage->ImageBase = ImageBase;
	PEImage->ImageSize = pVNTHeaders->OptionalHeader.SizeOfImage;
	PEImage->EntryPoint = EP;
	PEImage->VirtualImage = VImage;

	//Done
	free(FileMem);

	return true;
}

void UnMapRegion(uc_engine *uc, HMODULE hmod) {
	MEMORY_BASIC_INFORMATION info;
	// Start at PE32 header
	SIZE_T len = VirtualQuery(hmod, &info, sizeof(info));

	BYTE* dllBase = (BYTE*)info.AllocationBase;
	BYTE* address = dllBase;
	for (;;) {
		len = VirtualQuery(address, &info, sizeof(info));

		if (info.AllocationBase != dllBase)
			break;
		
		// Unmap all regions
		uc_mem_region *regions;
		int err_count = 0;
		uint32_t count;
		uc_err err = uc_mem_regions(uc, &regions, &count);

		for (uint32_t i = 0; i < count; i++) {
			if (regions[i].begin >= (uint64_t)info.BaseAddress && regions[i].end <= ((uint64_t)info.BaseAddress + info.RegionSize)) {
				err = uc_mem_unmap(uc, regions[i].begin, (size_t) (regions[i].end - regions[i].begin + 1));
				if (err) {
					printf("Failed on UnMapRegion::uc_mem_unmap() with error returned %u: %s\n", err, uc_strerror(err));
				}
			}
		}
		
		address = (BYTE*)info.BaseAddress + info.RegionSize;
	}
}

void MapDLLMemory(uc_engine *uc, HMODULE hmod, bool Write=false) {
	//Unmap all code sections first
	UnMapRegion(uc, hmod);

	MEMORY_BASIC_INFORMATION info;	
	SIZE_T len = VirtualQuery(hmod, &info, sizeof(info));	

	BYTE* dllBase = (BYTE*)info.AllocationBase;
	BYTE* address = dllBase;
	for (;;) {
		len = VirtualQuery(address, &info, sizeof(info));
		
		if (info.AllocationBase != dllBase) 
			break;				
						
		if (info.Protect != 0) {
			if (Write == true) {
				uc_err err = uc_mem_map(uc, (uint64_t)info.BaseAddress, info.RegionSize, UC_PROT_ALL);
				if (err) {					
					printf("Failed on MapDLLMemory::uc_mem_map() with error returned %u: %s\n", err, uc_strerror(err));					
				}

				err = uc_mem_write(uc, (uint64_t)info.BaseAddress, (const void *)info.BaseAddress, info.RegionSize);
				if (err) {
					printf("Failed on MapDLLMemory::uc_mem_write() with error returned %u: %s\n", err, uc_strerror(err));
				}
			}
			else {				
				uc_err err = uc_mem_map_ptr(uc, (uint64_t)info.BaseAddress, info.RegionSize, UC_PROT_ALL, info.BaseAddress);
				if (err) {
					printf("Failed on MapDLLMemory::uc_mem_map_ptr() %08X with error returned %u: %s\n", (uint32_t)info.BaseAddress, err, uc_strerror(err));
				}
			}
		}
		address = (BYTE*)info.BaseAddress + info.RegionSize;
	}
}

void parseImports(uc_engine *uc, uint8_t *VirtualImage, uint32_t ImportDirectoryRawAddr) {
	PIMAGE_IMPORT_DESCRIPTOR PImportDscrtr = (PIMAGE_IMPORT_DESCRIPTOR)& VirtualImage[ImportDirectoryRawAddr];
	
	for (; PImportDscrtr->Name != 0; PImportDscrtr++) {
		DWORD *PtrImport;		
		char *ImportNameRawRel;
		std::string ImportLibraryName;
		DWORD IATRVAPtr;
		DWORD FirstThunkRVA;

		//Convert Dll Name RVA to RAW
		ImportLibraryName = (char *)VirtualImage + PImportDscrtr->Name;
		//this->PEImportTable.insertImportLibrary(ImportLibraryName, PImportDscrtr->TimeDateStamp, PImportDscrtr->ForwarderChain);

		// Map into unicorn
		HMODULE hDLL = LoadLibrary(ImportLibraryName.c_str());
		if (!hDLL) {
			printf("Failed to load %s!\n", ImportLibraryName.c_str());
			return;
		}		

		uint32_t ModuleSize = GetModuleSize(hDLL);
		//Map the DLL into UC 
		//uc_mem_map(uc, (uint64_t) hDLL, ModuleSize, UC_PROT_ALL);
		DWORD AllAccess = PAGE_EXECUTE_READWRITE;
		DWORD OldAccess = 0;

		MEMORY_BASIC_INFORMATION info;
		SIZE_T len = VirtualQuery(hDLL, &info, sizeof(info));
		MapDLLMemory(uc, hDLL);

		if (PImportDscrtr->TimeDateStamp == 0) {
			IATRVAPtr = PImportDscrtr->FirstThunk;			
		}
		else {
			IATRVAPtr = PImportDscrtr->OriginalFirstThunk;			
		}

		PtrImport = (DWORD *) (VirtualImage + IATRVAPtr);
		//PtrImport = (DWORD *) (VirtualImage + PtrImport[0]);
		FirstThunkRVA = PImportDscrtr->FirstThunk;
		
		//Parse the Import Names/Ordinals
		uint8_t Int3 = 0xCC;
		for (int i = 0; PtrImport[i] != 0; i++) {
			if (PtrImport[i] & IMAGE_ORDINAL_FLAG32) {
				//this->PEImportTable.insertImport(ImportLibraryName, PtrImport[i] & 0xFFFF, PEImport::IMPORT_BY_ORDINAL, IATRVAPtr, IATRAWPtr, FirstThunkRVA, PtrImport[i] & 0xFFFF);
				FARPROC Proc = GetProcAddress(hDLL, LPCSTR(PtrImport[i] & 0xFFFF));
				*((DWORD *)(VirtualImage + FirstThunkRVA)) = (DWORD) Proc;

				//Patch CC into Import
				//uc_mem_write(uc, (uint64_t)Proc, &Int3, 1);

				std::string Import = ImportLibraryName + "_Ordinal_" + std::to_string(PtrImport[i] & 0xFFFF);
				ImportNameResolver[(uint64_t)Proc] = Import;
			}
			else {				 
				 ImportNameRawRel = (char *) (VirtualImage + PtrImport[i]) + 2;
				//this->PEImportTable.insertImport(ImportLibraryName, ImportNameRawRel, PEImport::IMPORT_BY_NAME, IATRVAPtr, IATRAWPtr, FirstThunkRVA, PtrImport[i] & 0xFFFF);
				FARPROC Proc = GetProcAddress(hDLL, ImportNameRawRel);
				*((DWORD *)(VirtualImage + FirstThunkRVA)) = (DWORD)Proc;


				//Patch CC to handle import
				//uc_mem_write(uc, (uint64_t)Proc, &Int3, 1);

				std::string Import = ImportLibraryName + "_" + ImportNameRawRel;
				ImportNameResolver[(uint64_t)Proc] = Import;
			}
			//Go to the next slot
			IATRVAPtr += 4;			
			FirstThunkRVA += 4;
		}
	}
}

bool loadPE32(uc_engine *uc, char *FilePath, PSPEImage PEImage) {
	FILE *fp = fopen(FilePath, "rb");
	if (!fp) {
		return 0;
	}

	EnableDebugPrivilege();

	// Get FileSize
	fseek(fp, 0, SEEK_END);
	size_t FileSize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	// Alloc Mem
	uint8_t *FileMem = (uint8_t *)calloc(1, FileSize);

	// Read and close
	fread(FileMem, 1, FileSize, fp);
	fclose(fp);


	PIMAGE_DOS_HEADER pVDosHeader = (PIMAGE_DOS_HEADER)FileMem;
	PIMAGE_NT_HEADERS32 pVNTHeaders = (PIMAGE_NT_HEADERS32)(((BYTE *)pVDosHeader) + (pVDosHeader->e_lfanew));

	// Allocate Virtual Image
	//uint8_t *VImage = (uint8_t *) calloc(1, pVNTHeaders->OptionalHeader.SizeOfImage);
	uint8_t *VImage = (uint8_t *)VirtualAlloc(NULL, pVNTHeaders->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	memset(VImage, 0, pVNTHeaders->OptionalHeader.SizeOfImage);

	// Get ImageBase
	uint64_t ImageBase = pVNTHeaders->OptionalHeader.ImageBase;
	uint64_t EP = pVNTHeaders->OptionalHeader.AddressOfEntryPoint;
	uint32_t ImageSize = pVNTHeaders->OptionalHeader.SizeOfImage;

	// Parse the sections and load image into memory	
	PIMAGE_SECTION_HEADER psectionheader = (PIMAGE_SECTION_HEADER)(pVNTHeaders + 1);
	for (int i = 0; i < pVNTHeaders->FileHeader.NumberOfSections; i++) {
		uint64_t VA = psectionheader->VirtualAddress;
		uint64_t VASize = psectionheader->Misc.VirtualSize;

		uint64_t RawOffset = psectionheader->PointerToRawData;
		uint64_t RawSize = psectionheader->SizeOfRawData;

		//copy information
		memcpy(VImage + VA, FileMem + RawOffset, (size_t)RawSize);

		//Copy the header
		if (i == 0) {
			memcpy(VImage, FileMem, (size_t)RawOffset);
		}

		psectionheader++;
	}

	// Fix Section VAs in VImage Header	
	PIMAGE_DOS_HEADER pVDosHeaderVImage = (PIMAGE_DOS_HEADER)VImage;
	PIMAGE_NT_HEADERS32 pVNTHeadersVImage = (PIMAGE_NT_HEADERS32)(((BYTE *)pVDosHeaderVImage) + (pVDosHeaderVImage->e_lfanew));

	psectionheader = (PIMAGE_SECTION_HEADER)(pVNTHeadersVImage + 1);
	for (int i = 0; i < pVNTHeadersVImage->FileHeader.NumberOfSections; i++) {
		psectionheader[i].PointerToRawData = psectionheader[i].VirtualAddress;
		psectionheader[i].SizeOfRawData = psectionheader[i].Misc.VirtualSize;
	}	

	/* delta is offset of allocated memory in target process */
	uint32_t delta = (DWORD_PTR)((LPBYTE)VImage);

	// Apply relocs (only if we choose a different imagebase)
	PIMAGE_DATA_DIRECTORY  datadir = (PIMAGE_DATA_DIRECTORY) (&pVNTHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]);

	if (datadir->VirtualAddress != 0) {
		/* Point to first relocation block copied in temporary buffer */
		PIMAGE_BASE_RELOCATION reloc = (PIMAGE_BASE_RELOCATION)(VImage + datadir->VirtualAddress);

		/* Browse all relocation blocks */
		while (reloc->VirtualAddress != 0) {
			/* We check if the current block contains relocation descriptors, if not we skip to the next block */
			if (reloc->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION))
			{
				/* We count the number of relocation descriptors */
				DWORD relocDescNb = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
				/* relocDescList is a pointer to first relocation descriptor */
				LPWORD relocDescList = (LPWORD)((LPBYTE)reloc + sizeof(IMAGE_BASE_RELOCATION));

				/* For each descriptor */
				for (uint32_t i = 0; i < relocDescNb; i++)
				{
					if (relocDescList[i] > 0)
					{
						/* Locate data that must be reallocated in buffer (data being an address we use pointer of pointer) */
						/* reloc->VirtualAddress + (0x0FFF & (list[i])) -> add botom 12 bit to block virtual address */
						DWORD_PTR *p = (DWORD_PTR *)(VImage + (reloc->VirtualAddress + (0x0FFF & (relocDescList[i]))));
						/* Change the offset to adapt to injected module base address */

						if ((reloc->VirtualAddress + (0x0FFF & (relocDescList[i]))) >= 0x002263 && (reloc->VirtualAddress + (0x0FFF & (relocDescList[i]))) <= 0x002270) {
							int i = 1;
						}						
						*p -= pVNTHeaders->OptionalHeader.ImageBase;
						*p += delta;						
					}
				}
			}
			/* Set reloc pointer to the next relocation block */
			reloc = (PIMAGE_BASE_RELOCATION)((LPBYTE)reloc + reloc->SizeOfBlock);
		}
	}

	//CLean Relocs
	pVNTHeadersVImage->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = 0;
	pVNTHeadersVImage->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 0;

	// Map memory to unicorn
	//uc_err err = uc_mem_map(uc, ImageBase, pVNTHeaders->OptionalHeader.SizeOfImage, UC_PROT_ALL);
	
	uc_err err = uc_mem_map_ptr(uc, (uint64_t) VImage, pVNTHeaders->OptionalHeader.SizeOfImage, UC_PROT_ALL, VImage);
	if (err) {
		printf("loadPE32 failed!\n");
		return 0;
	}

	// Parse ImportTable	
	parseImports(uc, VImage, pVNTHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	/*if (uc_mem_write(uc, ImageBase, VImage, pVNTHeaders->OptionalHeader.SizeOfImage)) {
		printf("loadPE32 failed!\n");
		free(FileMem);
		return 0;
	}*/

	
	//Fill return struct
	PEImage->ImageBase = (uint64_t) VImage;
	PEImage->ImageSize = pVNTHeaders->OptionalHeader.SizeOfImage;
	PEImage->EntryPoint = EP + (uint64_t)VImage;
	PEImage->VirtualImage = VImage;
	

	//Old way
	/*PEImage->ImageBase = (uint64_t)ImageBase;
	PEImage->ImageSize = pVNTHeaders->OptionalHeader.SizeOfImage;
	PEImage->EntryPoint = EP + (uint64_t)ImageBase;
	PEImage->VirtualImage = VImage;
	*/
	//Done
	free(FileMem);

	return true;
}

void printX86Regs(uc_engine *uc) {
	uint32_t EAX;
	uint32_t EBX;
	uint32_t ECX;
	uint32_t EDX;
	uint32_t ESI;
	uint32_t EDI;
	uint32_t EBP;
	uint32_t ESP;
	uint32_t EIP;
	uint32_t Eflags;

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

	uc_reg_read(uc, UC_X86_REG_EAX, &EAX);
	uc_reg_read(uc, UC_X86_REG_EBX, &EBX);
	uc_reg_read(uc, UC_X86_REG_ECX, &ECX);
	uc_reg_read(uc, UC_X86_REG_EDX, &EDX);
	uc_reg_read(uc, UC_X86_REG_ESI, &ESI);
	uc_reg_read(uc, UC_X86_REG_EDI, &EDI);
	uc_reg_read(uc, UC_X86_REG_EBP, &EBP);
	uc_reg_read(uc, UC_X86_REG_ESP, &ESP);
	uc_reg_read(uc, UC_X86_REG_EIP, &EIP);	
	uc_reg_read(uc, UC_X86_REG_EFLAGS, &Eflags);

	printf("EAX=%08X   EBX=%08X   ECX=%08X   EDX=%08X   ESI=%08X\n", EAX, EBX, ECX, EDX, ESI);
	printf("EDI=%08X   EBP=%08X   ESP=%08X   EIP=%08X   ", EDI, EBP, ESP, EIP);	
	if ((Eflags >> 11) & 1) {
		printf("%so ", KCYN);
	}
	else {
		printf("%so ", KWHT);
	}
	if ((Eflags >> 10) & 1) {
		printf("%sd ", KCYN);
	}
	else {
		printf("%sd ", KWHT);
	}
	if ((Eflags >> 9) & 1) {
		printf("%sI ", KCYN);
	}
	else {
		printf("%sI ", KWHT);
	}
	if ((Eflags >> 7) & 1) {
		printf("%ss ", KCYN);
	}
	else {
		printf("%ss ", KWHT);
	}
	if ((Eflags >> 6) & 1) {
		printf("%sZ ", KCYN);
	}
	else {
		printf("%sZ ", KWHT);
	}
	if ((Eflags >> 4) & 1) {
		printf("%sa ", KCYN);
	}
	else {
		printf("%sa ", KWHT);
	}
	
	if ((Eflags >> 2) & 1) {
		printf("%sP ", KCYN);
	}
	else {
		printf("%sP ", KWHT);
	}

	if ((Eflags >> 1) & 1) {
		printf("%sc\n\n", KCYN);
	}
	else {
		printf("%sc\n\n", KWHT);
	}
	printf("%s", KWHT);
}

//VERY basic descriptor init function, sets many fields to user space sane defaults
static void init_descriptor(struct SegmentDescriptor *desc, uint32_t base, uint32_t limit, uint8_t is_code)
{
	desc->desc = 0;  //clear the descriptor
	desc->base0 = base & 0xffff;
	desc->base1 = (base >> 16) & 0xff;
	desc->base2 = base >> 24;
	if (limit > 0xfffff) {
		//need Giant granularity
		limit >>= 12;
		desc->granularity = 1;
	}
	desc->limit0 = limit & 0xffff;
	desc->limit1 = limit >> 16;

	//some sane defaults
	desc->dpl = 3;
	desc->present = 1;
	desc->db = 1;   //32 bit
	desc->type = is_code ? 0xb : 3;
	desc->system = 1;  //code or data
}

uint32_t getCurrentFSValue() {
	LDT_ENTRY entry;
	HANDLE handle;
	unsigned char offset[4];

	ZeroMemory(&entry, sizeof(LDT_ENTRY));

	handle = GetCurrentThread();//some handle;

	GetThreadSelectorEntry(handle, 0x53, &entry);

	offset[3] = entry.HighWord.Bits.BaseHi;
	offset[2] = entry.HighWord.Bits.BaseMid;
	memcpy(offset, &entry.BaseLow, 2);
	uint32_t TIB = *(uint32_t *)offset;
	return TIB;
}


void setupSegmentRegs(uc_engine *uc)
{
	uint64_t FS = getCurrentFSValue();
	printf("FS             : %08X\n", (uint32_t) FS);

	MEMORY_BASIC_INFORMATION info;
	SIZE_T len = VirtualQuery((LPCVOID)FS , &info, sizeof(info));

	// Copy the FS pages into unicorn
	MapDLLMemory(uc, (HMODULE) info.AllocationBase, true);	

	//uc_err err = uc_mem_map_ptr(uc, (uint64_t)info.BaseAddress, info.RegionSize, UC_PROT_ALL, info.BaseAddress);

	//uc_err err = uc_mem_map(uc, (uint64_t)info.BaseAddress, info.RegionSize, UC_PROT_ALL);
	//err = uc_mem_write(uc, (uint64_t)info.BaseAddress, (const void *)info.BaseAddress, info.RegionSize);	

	// Setup GDT
	uc_x86_mmr gdtr;
	const uint64_t gdt_address = 0xc0000000;
	const uint64_t fs_address = FS;

	struct SegmentDescriptor *gdt = (struct SegmentDescriptor*)calloc(31, sizeof(struct SegmentDescriptor));	
	int r_cs = 0x73;
	int r_ss = 0x88;      //ring 0
	int r_ds = 0x7b;
	int r_es = 0x7b;
	int r_fs = 0x83;

	gdtr.base = gdt_address;
	gdtr.limit = 31 * sizeof(struct SegmentDescriptor) - 1;

	init_descriptor(&gdt[14], 0, 0xfffff000, 1);  //code segment
	init_descriptor(&gdt[15], 0, 0xfffff000, 0);  //data segment
	init_descriptor(&gdt[16], (uint32_t) fs_address, 0xfff, 0);  //one page data segment simulate fs
	init_descriptor(&gdt[17], 0, 0xfffff000, 0);  //ring 0 data
	gdt[17].dpl = 0;  //set descriptor privilege level

  // Initialize emulator in X86-32bit mode
	//uc_open(UC_ARCH_X86, UC_MODE_32, &uc);		
	
	// map 64k for a GDT
	uc_err err = uc_mem_map(uc, gdt_address, 0x10000, UC_PROT_WRITE | UC_PROT_READ);
	if (err) {
		printf("Failed on setupSegmentRegs::uc_mem_map() gdt_address with error returned %u: %s\n", err, uc_strerror(err));
	}
	
	//set up a GDT BEFORE you manipulate any segment registers
	uc_reg_write(uc, UC_X86_REG_GDTR, &gdtr);
	
	// write gdt to be emulated to memory
	err = uc_mem_write(uc, gdt_address, gdt, 31 * sizeof(struct SegmentDescriptor));	
	if (err) {
		printf("Failed on setupSegmentRegs::uc_mem_write() with error returned %u: %s\n", err, uc_strerror(err));
	}

	//@peter not needed and not working
	// map 1 page for FS
	/*err = uc_mem_map(uc, fs_address, 0x1000, UC_PROT_WRITE | UC_PROT_READ);	
	if (err) {
		printf("Failed on setupSegmentRegs::uc_mem_map() fs_address with error returned %u: %s\n", err, uc_strerror(err));
	}*/

	// when setting SS, need rpl == cpl && dpl == cpl
	// emulator starts with cpl == 0, so we need a dpl 0 descriptor and rpl 0 selector
	uc_reg_write(uc, UC_X86_REG_SS, &r_ss);	
	uc_reg_write(uc, UC_X86_REG_CS, &r_cs);	
	uc_reg_write(uc, UC_X86_REG_DS, &r_ds);	
	uc_reg_write(uc, UC_X86_REG_ES, &r_es);	
	uc_reg_write(uc, UC_X86_REG_FS, &r_fs);	
}

// callback for tracing instruction
static bool hook_Segment_error(uc_engine *uc, uc_mem_type type,
	uint64_t address, int size, int64_t value, void *user_data)
{	
	uint32_t PEB_Offset = 0x1000;
	uint32_t Ldr_Offset = 0x2000;
	uint32_t LE_Offset = 0x3000;
	uint32_t FullDllName_Offset = 0x4000;

	uint32_t EIP;
	uc_reg_read(uc, UC_X86_REG_EIP, &EIP);

	printf("0x%08X Missing memory at 0x%X, data size = %u, data value = 0x%X\n", EIP, (uint32_t) address, size, (uint32_t)value);

	switch (type) {
	case UC_MEM_READ_UNMAPPED:
	case UC_MEM_WRITE_UNMAPPED:	
	case UC_MEM_FETCH_UNMAPPED:
	{
			//Like in code
			//Map mem into UC
			MEMORY_BASIC_INFORMATION info;
			SIZE_T len = VirtualQuery((LPCVOID)address, &info, sizeof(info));
			MapDLLMemory(uc, (HMODULE) info.AllocationBase);
		
			/*uc_err err = uc_mem_map(uc, (uint64_t)info.BaseAddress, info.RegionSize, UC_PROT_ALL);
			if (err) {
				printf("uc_mem_map failed!\n");
			}
			err = uc_mem_write(uc, (uint64_t)info.BaseAddress, (const void *)info.BaseAddress, info.RegionSize);
			*/
			return true;
		}
	default:
		// return false to indicate we want to stop emulation
		return false;		
	}	
}


/*
	Handle interrupts
*/
void hook_intr(uc_engine *uc, uint32_t intno, void *user_data) {
	uint32_t EIP;
	uc_reg_read(uc, UC_X86_REG_EIP, &EIP);

	printf("0x%08X Interrupt %i\n", EIP, intno);
}

// callback for tracing instruction
static void hook_code(uc_engine *uc, uint64_t address, uint32_t size, void *user_data)
{
	printX86Regs(uc);
}

// callback for tracing instruction
static uint32_t OLDEip = 0;
static uint32_t USER_Esp = 0;
static uint32_t Import = 0;
static uint32_t *UC_ESP;
static uint32_t UC_EAX = 0;
static void hook_Imports(uc_engine *uc, uint64_t address, uint32_t size, void *user_data)
{
	std::string &API = ImportNameResolver[address];

	std::cout << std::hex << address << " Calling import: " << API << "\n";
	
	uc_reg_read(uc, UC_X86_REG_ESP, &UC_ESP);
	
	Import = (uint32_t)address;

	OLDEip = UC_ESP[0];
	uint32_t Arg0 = UC_ESP[1];
	uint32_t Arg1 = UC_ESP[2];

	//Catch GetProcAdress 
	if (API.find("GetProcAddress") != -1) {
		//
		char ModuleName[1024];
		GetModuleFileName((HMODULE) Arg0, ModuleName, 1024);		
		//printf("0x%08X GetProcAdress Handle: %08X Module: %s API: %s\n", OLDEip, Arg0, ModuleName, (char *)Arg1);		

		FARPROC Proc = GetProcAddress( (HMODULE) Arg0, (LPCSTR) Arg1);
		if (isImportKnown((uint64_t)Proc) == false) {
			// Add import
			ImportNameResolver[(uint64_t)Proc] = (char *)Arg1;

			// Hook Import
			hookImport(uc, (uint64_t) Proc);
		}
	}

	//printX86Regs(uc);

	// Simulate pushad
	UC_ESP -= 8;
	uc_reg_read(uc, UC_X86_REG_EDI, &UC_ESP[0]);
	uc_reg_read(uc, UC_X86_REG_ESI, &UC_ESP[1]);
	uc_reg_read(uc, UC_X86_REG_EBP, &UC_ESP[2]);
	uc_reg_read(uc, UC_X86_REG_ESP, &UC_ESP[3]);
	uc_reg_read(uc, UC_X86_REG_EBX, &UC_ESP[4]);
	uc_reg_read(uc, UC_X86_REG_EDX, &UC_ESP[5]);
	uc_reg_read(uc, UC_X86_REG_ECX, &UC_ESP[6]);
	uc_reg_read(uc, UC_X86_REG_EAX, &UC_ESP[7]);
	
	__asm {		
		// Store UniPE registers
		pushad		
		mov USER_Esp, esp

		// Switch to UC context
		mov esp, UC_ESP
		popad

		// Remove return location
		add esp, 4

		//Call import
		call Import

		// Switch Context
		pushad

		mov UC_ESP, esp

		// Resoter UniPE context
		mov esp, USER_Esp		
		popad
	}

	uc_reg_write(uc, UC_X86_REG_EIP, &OLDEip);

	uc_reg_write(uc, UC_X86_REG_EDI, &UC_ESP[0]);
	uc_reg_write(uc, UC_X86_REG_ESI, &UC_ESP[1]);
	uc_reg_write(uc, UC_X86_REG_EBP, &UC_ESP[2]);	
	uc_reg_write(uc, UC_X86_REG_EBX, &UC_ESP[4]);
	uc_reg_write(uc, UC_X86_REG_EDX, &UC_ESP[5]);
	uc_reg_write(uc, UC_X86_REG_ECX, &UC_ESP[6]);
	uc_reg_write(uc, UC_X86_REG_EAX, &UC_ESP[7]);

	//Restore ESP
	UC_ESP += 8;
	uc_reg_write(uc, UC_X86_REG_ESP, &UC_ESP);	

	//printX86Regs(uc);
	
	/*if (API == "USER32.dll_MessageBoxW") {
		MessageBoxW((HWND)ESP[1], (LPCWSTR)ESP[2], (LPCWSTR)ESP[3], ESP[4]);
		uint32_t EIP;
		EIP = ESP[0];
		printf("Return @EIP %08X\n", EIP);
		ESP -= 1;
		uc_reg_write(uc, UC_X86_REG_ESP, &ESP);
		uc_reg_write(uc, UC_X86_REG_EIP, &EIP);
	}*/
}

void SetupHooks(uc_engine *uc) {
	uc_hook SegmentError;	
	uc_hook Interrupt;

	// tracing all instruction by having @begin > @end
	uc_hook_add(uc, &SegmentError, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, hook_Segment_error, NULL, 1, 0);

	// Hook interrupts
	uc_hook_add(uc, &Interrupt, UC_HOOK_INTR, hook_intr, nullptr, 1, 0);	
}


int main(int argc, char **argv)
{
	printf("UNIPE - A small framework to execute PE files with UniCorn\n");
	printf("pgarba - 2018\n\n");

	EnableDebugPrivilege();
	
	char FilePath[] = "g:/Cracking/UnicornBasedAttacks/UniTest/Release/UniTest.exe";

	uc_engine *uc;
	uc_err err;

	// Initialize emulator in X86-64bit mode
	err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
	if (err != UC_ERR_OK) {
		printf("Failed on uc_open() with error returned: %u\n", err);
		return -1;		
	}	

	// Load PE into memory
	SPEImage PEImage;
	loadPE32(uc, FilePath, &PEImage);	

	setupSegmentRegs(uc);

	// Init the Arguments	
	// Virtual Stack
	//uint32_t Stack[0x10000];
	const uint32_t StackSize = 0x100000;
	uint32_t *Stack = (uint32_t *) VirtualAlloc(NULL, StackSize, MEM_COMMIT, PAGE_READWRITE);
	memset(Stack, 0, StackSize);
	int StackPtr = (StackSize - 1) / 4;

	// Alloc Stack in userland :D
	uint64_t StackTop = (uint64_t) Stack + StackSize;
	err = uc_mem_map_ptr(uc, (uint64_t)Stack, StackSize, UC_PROT_ALL, (void *)Stack);
	if (err) {
		printf("Failed on uc_emu_start() with error returned %u: %s\n",
			err, uc_strerror(err));
		return 0;
	}

	StackTop = StackTop - 0x20;
	((uint32_t *)StackTop)[0] = (uint32_t) 0;
	((uint32_t *)StackTop)[1] = (uint32_t) 0;
	uc_reg_write(uc, UC_X86_REG_ESP, &StackTop);

	// Setup
	SetupHooks(uc);

	// Hook Imports
	hookImports(uc);

	// Print Some info
	printf("Stack          : %08X\n", (uint32_t) Stack);
	printf("Stack Region   : %08X - %08X\n", (uint32_t)Stack, (uint32_t) Stack + StackSize);
	printf("Loading Address: %08X\n", (uint32_t) PEImage.ImageBase);
	printf("Image Size     : %08X\n", (uint32_t) PEImage.ImageSize);
	printf("Image Region   : %08X - %08X\n", (uint32_t) PEImage.ImageBase, (uint32_t) PEImage.ImageBase + (uint32_t) PEImage.ImageSize);
	printf("\n\n");

	// emulate code in infinite time & unlimited instructions
	uint64_t IP = PEImage.EntryPoint;	
	// initialize machine registers
	uint32_t In = 0;
	uc_reg_write(uc, UC_X86_REG_ECX, &In);
	uc_reg_write(uc, UC_X86_REG_EDX, &In);		

	//dumpFile(PEImage.VirtualImage, (uint32_t)PEImage.VirtualImage, PEImage.ImageSize);
	//dumpFile(VImage, (uint32_t)VImage, ImageSize);
	
	do {
		err = uc_emu_start(uc, IP, PEImage.ImageBase + PEImage.ImageSize, 0, 1);

		uc_reg_read(uc, UC_X86_REG_EIP, &IP);
		
		printX86Regs(uc);
	} while (!err);
	if (err) {
		printf("Failed on uc_emu_start() with error returned %u: %s\n",
			err, uc_strerror(err));
	}
	
	// Done
	printX86Regs(uc);
	uc_close(uc);

	// Clean up
	VirtualFree(PEImage.VirtualImage, (size_t) PEImage.ImageSize, 0);

    return 0;
}


