#include "DriverReader.h"


HANDLE DriverReader::hDeviceDrv = NULL;
char DriverReader::targetProc[];
char DriverReader::previousTargetProc[];
uintptr_t DriverReader::DTBTargetProcess = 0;
uintptr_t DriverReader::virtualSizeTargetProcess = 0;
uintptr_t DriverReader::pBaseAddressTargetProcess = 0;
uintptr_t DriverReader::pVadRootTargetProcess = 0;
uintptr_t DriverReader::pPEBTargetProcess = 0;
std::vector<EnumerateRemoteSectionData> DriverReader::sections;
std::vector<EnumerateRemoteModuleData> DriverReader::modules;

int DriverReader::getDeviceHandle(LPTSTR name)
{
	DriverReader::hDeviceDrv = CreateFile(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (DriverReader::hDeviceDrv == INVALID_HANDLE_VALUE)
	{
		std::cout << "[-] Handle failed: " << std::dec << GetLastError() << std::endl;
		return 1;
	}
	std::cout << "[+] HANDLE obtained" << std::endl;
	return 0;
}



ULONG64 fn_mapPhysical(ULONG64 physicaladdress, DWORD size)
{
	READ_REQUEST inbuffer = { 0, 0, physicaladdress, 0, size };
	ULONG64 outbuffer[2] = { 0 };
	DWORD bytes_returned = 0;
	DeviceIoControl(DriverReader::hDeviceDrv,
		IOCTL_GIO_MAPPHYSICAL,
		&inbuffer,
		sizeof(inbuffer),
		&outbuffer,
		sizeof(outbuffer),
		&bytes_returned,
		(LPOVERLAPPED)NULL);

	return outbuffer[0];
}

ULONG64 fn_unmapPhysical(ULONG64 address)
{
	ULONG64 inbuffer = address;
	ULONG64 outbuffer[2] = { 0 };
	DWORD bytes_returned = 0;
	DeviceIoControl(DriverReader::hDeviceDrv,
		IOCTL_GIO_UNMAPPHYSICAL,
		(LPVOID)&inbuffer,
		sizeof(inbuffer),
		(LPVOID)outbuffer,
		sizeof(outbuffer),
		&bytes_returned,
		(LPOVERLAPPED)NULL);

	return outbuffer[0];
}

BOOL GIO_memcpy(ULONG64 dest, ULONG64 src, DWORD size)
{
	MEMCPY_REQUEST mystructIn = { dest, src, size };
	BYTE outbuffer[0x30] = { 0 };
	DWORD returned = 0;

	DeviceIoControl(DriverReader::hDeviceDrv, IOCTL_GIO_MEMCPY, (LPVOID)&mystructIn, sizeof(mystructIn), (LPVOID)outbuffer, sizeof(outbuffer), &returned, NULL);
	if (returned) {
		return TRUE;
	}
	return FALSE;
}


// Read a Physical memory
bool ReadPhyMemory(uintptr_t physicalAddress, LPVOID  lpBuffer, SIZE_T  nSize, SIZE_T  *lpNumberOfBytesRead)
{
	// Read physical memory
	uint64_t memory = fn_mapPhysical(physicalAddress, nSize);

	if (!memory)
		return false;

	// Copy the buffer so we can free the mapped memory
	memcpy((void*)lpBuffer, (const void*)memory, nSize);

	// Free mapped memory
	fn_unmapPhysical(memory);

	return true;

}

uint64_t VAtoPhylAddress(uintptr_t directoryTableBase, LPVOID virtualAddress)
{
	uintptr_t va = (uint64_t)virtualAddress;

	// PMl4 - Page Map level 4
	// PDPT - Page Directory Pointer Table
	// PD - Page Directory
	// PT - Page Table 
	unsigned short PML4 = (USHORT)((va >> (12 + 9 + 9 + 9)) & 0x1FF);
	unsigned short PDPT = (USHORT)((va >> (12 + 9 + 9)) & 0x1FF);
	unsigned short PD = (USHORT)((va >> (12 + 9)) & 0x1FF);
	unsigned short PT = (USHORT)((va >> 12) & 0x1FF);
	////std::cout << "- virtualAddress " << virtualAddress << std::endl;
	////std::cout << "- directoryTableBase" << directoryTableBase << std::endl;
	////std::cout << "- PML4 " << PML4 << std::endl;
	////std::cout << "- PDPT " << PDPT << std::endl;
	////std::cout << "- PD " << PD << std::endl;
	////std::cout << "- PT " << PT << std::endl;

	// Obtain the PML4 Entry (PML4E)
	uintptr_t PML4E = 0;
	ReadPhyMemory(directoryTableBase + PML4 * sizeof(ULONGLONG), &PML4E, sizeof(uint64_t), NULL);
	//std::cout << "- PML4E " << PML4E << std::endl;
	if (PML4E == 0)
		return 0;

	// Obtain the PDPT Entry. It is the base address of the next table
	uintptr_t PDPTE = 0;
	ReadPhyMemory((PML4E & 0xFFFFFFFFFF000) + PDPT * sizeof(ULONGLONG), &PDPTE, sizeof(uint64_t), NULL);
	////std::cout << "- PDPTE " << PDPTE << std::endl;
	if (PDPTE == 0)
		return 0;

	// Checking this bit will allow us to determinate if PDPTE maps a 1GB page or not.
	// In that case we need to calculate the final base address extracting bits 51-30 (0xFFFFFC0000000) 
	// from PDPTE nad bits 29-0 from the VA (0x3FFFFFFF).
	if ((PDPTE & (1 << 7)) != 0)
		return (PDPTE & 0xFFFFFC0000000) + (va & 0x3FFFFFFF);

	// If PS bit was zero we need to obtain the base address of the next table on the chain.
	uint64_t PDE = 0;
	ReadPhyMemory((PDPTE & 0xFFFFFFFFFF000) + PD * sizeof(ULONGLONG), &PDE, sizeof(uint64_t), NULL);
	////std::cout << "- PDE " << PDE << std::endl;
	if (PDE == 0)
		return 0;

	// Again we need to check the PS flag for PDE, in this case it will be a 2MB page if 1.
	// In that case we need to calculate the final base address extracting bits 51-21 (0xFFFFFFFE00000)
	// from PDE and 20-0 from the VA (0x1FFFFF)
	if ((PDE & (1 << 7)) != 0)
		return (PDE & 0xFFFFFFFE00000) + (va & 0x1FFFFF);

	// Let's obtain the PT entry if PS was 0.
	uintptr_t PTE = 0;
	ReadPhyMemory((PDE & 0xFFFFFFFFFF000) + PT * sizeof(ULONGLONG), &PTE, sizeof(uint64_t), NULL);
	////std::cout << "- PTE " << PTE << std::endl;

	if (PTE == 0)
		return 0;

	// Each PTE corresponds to a 4KB page. Final physical address is obtaining extracting the bits 51-12 from the PTE (0xFFFFFFFFFF000)
	// and the 11-0 from the VA (0xFFF).
	return (PTE & 0xFFFFFFFFFF000) + (va & 0xFFF);
}


// Write a VirtualMemory (Kernel or Usermode)
bool DriverReader::WriteVirtualMemory(uint64_t directoryTableBase, uintptr_t virtualAddress, LPVOID  lpBuffer, SIZE_T  nSize, SIZE_T  *lpNumberOfBytesWritten)
{

	// Translate Virtual to physical
	uint64_t physicalAddress = VAtoPhylAddress(directoryTableBase,  (LPVOID) virtualAddress);

	// Control if physicalAddress is valid
	if (!physicalAddress)
		return false;

	// Read physical memory
	uint64_t memory = fn_mapPhysical(physicalAddress, nSize);

	if (!memory)
		return false;

	// Copy the new value to the already mapped physical memory
	memcpy((void*)memory, (const void*)lpBuffer , nSize);

	// Free mapped memory so we can persist the changes
	fn_unmapPhysical(memory);

	return true;
}

bool DriverReader::ReadVirtualMemory(uint64_t directoryTableBase, uintptr_t virtualAddress, LPCVOID lpBuffer, SIZE_T  nSize, SIZE_T  *lpNumberOfBytesRead)
{
	//std::cout << virtualAddress << std::endl;
	// Translate Virtual to physical
	uint64_t physicalAddress = VAtoPhylAddress(directoryTableBase, (LPVOID)virtualAddress);

	// std::cout << physicalAddress << std::endl;
	// Control if physicalAddress is valid
	if (!physicalAddress)
		return false;

	// Read physical memory
	uint64_t memory = fn_mapPhysical(physicalAddress, nSize);

	if (!memory)
		return false;

	// Copy the buffer so we can free the mapped memory
	memcpy((void*)lpBuffer, (const void*)memory, nSize);

	// Free mapped memory
	fn_unmapPhysical(memory);

	return true;
}

// From here we are implementing the V2 of DriverHelper. Focus will be set on implementing functions that allows us to exploit a driver and perform actions like: dump the target process and RWMemory from kernel.
// We can leak a kernel pointer to an EPROCESS structure. We can use this to traverse over the double linked list to enumerate every process.
bool DriverReader::LeakKernelPointers(std::vector<uintptr_t> &pKernelPointers)
{

	SYSTEM_HANDLE_INFORMATION_EX* pHandleInfo = NULL;

	// Initial size of the buffer, we are going to make it bigger if it is necesary later
	DWORD lBuffer = 0x10000;

	// This option will allow us to get the list of kernel pointers
	const unsigned long SystemExtendedHandleInformation = 0x40;

	DWORD retSize = 0;
	NTSTATUS status;

	do {
		if (pHandleInfo != NULL) {
			// Cleaning the buffer if this is not the first execution of the DO
			HeapFree(GetProcessHeap(), 0, pHandleInfo);
			pHandleInfo = NULL;
		}

		// Expanding the buffer *2
		lBuffer *= 2;

		// Dinamically allocate memory on the Heap for the buffer. I tried to use VirtualAlloc but it didn't work.
		pHandleInfo = (SYSTEM_HANDLE_INFORMATION_EX*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, lBuffer);

		if (pHandleInfo == NULL)
		{
			std::cout << "[-] LeakKernelPointer pHandleInfo NULL" << std::endl;
			return false;
		}
	} while ((status = NtQuerySystemInformation(static_cast<SYSTEM_INFORMATION_CLASS>(SystemExtendedHandleInformation), pHandleInfo, lBuffer, &retSize)) == STATUS_INFO_LENGTH_MISMATCH);

	/*
	The returned structure will have the following definition
	typedef struct SYSTEM_HANDLE_INFORMATION_EX
	{
		ULONG_PTR NumberOfHandles;
		ULONG_PTR Reserved;
		SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
	};
	*/

	std::cout << "[+] LeakKernelPointer NUmberOfHandles: " << pHandleInfo->NumberOfHandles << std::endl;
	// NumberOfHandles will tell us how many times we need to iterate the array.
	for (unsigned int i = 0; i < pHandleInfo->NumberOfHandles; i++)
	{
		// Lets get all the handles from the process with PID 4 (system)
		ULONG SystemPID = 4;
		// Atribbute value for Process HANDLEs
		ULONG ProcessHandleAttribute = 0x102A;

		// Is this the best option? Maybe there is a better one
		if (pHandleInfo->Handles[i].UniqueProcessId == SystemPID && pHandleInfo->Handles[i].HandleAttributes == ProcessHandleAttribute)
		{
			pKernelPointers.push_back(reinterpret_cast<uintptr_t>(pHandleInfo->Handles[i].Object));
		}

	}
	return true;
}


// Thanks to https://twitter.com/SpecialHoang for this function
// https://github.com/hoangprod/DanSpecial/blob/master/DanSpecial/memory.cpp
uintptr_t DriverReader::FindDirectoryBase()
{
	printf("[+] Attempting to find Dirbase.\n");

	for (int i = 0; i < 10; i++)
	{
		uintptr_t lpBuffer = fn_mapPhysical(i * 0x10000, 0x10000);

		for (int uOffset = 0; uOffset < 0x10000; uOffset += 0x1000)
		{

			if (0x00000001000600E9 ^ (0xffffffffffff00ff & *reinterpret_cast<uintptr_t*>(lpBuffer + uOffset)))
				continue;
			if (0xfffff80000000000 ^ (0xfffff80000000000 & *reinterpret_cast<uintptr_t*>(lpBuffer + uOffset + 0x70)))
				continue;
			if (0xffffff0000000fff & *reinterpret_cast<uintptr_t*>(lpBuffer + uOffset + 0xa0))
				continue;

			return *reinterpret_cast<uintptr_t*>(lpBuffer + uOffset + 0xa0);
		}

		fn_unmapPhysical((lpBuffer));
	}

	return NULL;
}


/*

0: kd> dd  0xffffbe8c2141d040
ffffbe8c`2141d040  00b60003 00000000 2141d048 ffffbe8c		<==== 00b60003 if KPROCESS
ffffbe8c`2141d050  2141d048 ffffbe8c 2141d058 ffffbe8c
ffffbe8c`2141d060  2141d058 ffffbe8c 001ab000 00000000
ffffbe8c`2141d070  214aa338 ffffbe8c 25089338 ffffbe8c
ffffbe8c`2141d080  00000000 00000000 00000000 00000000
ffffbe8c`2141d090  00140001 00000000 00000003 00000000
ffffbe8c`2141d0a0  00000000 00000000 00000000 00000000
ffffbe8c`2141d0b0  00000000 00000000 00000000 00000000


0: kd> dt nt!_KPROCESS 0xffffbe8c2141d040
   +0x000 Header           : _DISPATCHER_HEADER
   +0x018 ProfileListHead  : _LIST_ENTRY [ 0xffffbe8c`2141d058 - 0xffffbe8c`2141d058 ]
   +0x028 DirectoryTableBase : 0x1ab000
   +0x030 ThreadListHead   : _LIST_ENTRY [ 0xffffbe8c`214aa338 - 0xffffbe8c`25089338 ]
   +0x040 ProcessLock      : 0

*/
// This is necessary to check if the pointer we have its a KPROCESS pointer :)
uintptr_t DriverReader::ObtainKProcessPointer(uint64_t directoryTableBase, std::vector<uintptr_t> pKernelPointers)
{
	//The header of a KPROCESS has the value 00b60003
	unsigned int KProcessHeader = 0x00b60003;

	unsigned int bHeader = 0;

	for (uintptr_t pointer : pKernelPointers)
	{
		// read header
		DriverReader::ReadVirtualMemory(directoryTableBase, pointer, &bHeader, sizeof(unsigned int), NULL);


		// Compare Header with value
		if (bHeader == KProcessHeader)
		{
			std::cout << "[+] ObtainKProcessPointer found." << std::endl;
			return pointer;
		}

		std::cout << "[-] ObtainKProcessPointer not found." << std::endl;
	}

	return 0;

}

uintptr_t DriverReader::GetKProcess(uintptr_t &directoryTableBase)
{
	// Define the vector of pointers to return
	std::vector<uintptr_t> pKernelPointers;

	// We need the DirectoryBaseTable of the process to translate Virtual to Phyisical Addresses
	directoryTableBase = DriverReader::FindDirectoryBase();

	std::cout << "[+] GetKprocess - directoryTableBase 0x" << std::hex << directoryTableBase << std::endl;

	// Lets use NtQuerySystemInformation with SystemExtendedHandleInformation to get the list of kernel pointers
	if (!(DriverReader::LeakKernelPointers(pKernelPointers)))
		return 0;

	// Validate KProcess Header to identify all the handles to KPROCESS structures.
	auto pKprocess = DriverReader::ObtainKProcessPointer(directoryTableBase, pKernelPointers);

	if (pKprocess == 0)
	{
		std::cout << "[-] ObtainKProcessPointer not found." << std::endl;
		return 0;
	}

	return pKprocess;
}



//0: kd > dt nt!_EPROCESS 0xFFFFDA8ADA8E2800
//+ 0x000 Pcb              : _KPROCESS
//+ 0x2d8 ProcessLock : _EX_PUSH_LOCK
//+ 0x2e0 RundownProtect : _EX_RUNDOWN_REF
//+ 0x2e8 UniqueProcessId : 0x00000000`00000638 Void
//+ 0x2f0 ActiveProcessLinks : _LIST_ENTRY[0xffffda8a`da9634f0 - 0xffffda8a`da86daf0]
//+ 0x450 ImageFileName    : [15]  "spoolsv.exe"
//+ 0x45f PriorityClass : 0x2 ''
//+ 0x460 SecurityPort : (null)
//  +0x3b0 Job              : 0xffffda8a`de7ca860 _EJOB
//   +0x3b8 SectionObject    : 0xffffc98f`cdb93180 Void
//   +0x3c0 SectionBaseAddress : 0x00007ff6`18b50000 Void

uintptr_t DriverReader::SearchKProcess(LPCVOID processName, uintptr_t &directoryTableBase, uintptr_t pKProcess)
{

	uintptr_t initialProcessId = 0;
	DriverReader::ReadVirtualMemory(directoryTableBase, pKProcess + OFFSET_UNIQUEPROCESSID, &initialProcessId, sizeof(initialProcessId), NULL);
	uintptr_t currentProcessId = 0;
	uintptr_t currentKProcess = 0;


	PLIST_ENTRY   initialEntry = (PLIST_ENTRY)(pKProcess + OFFSET_ACTIVEPROCESSLINKS);
	PLIST_ENTRY currentEntry = initialEntry;
	uintptr_t imagefilename_offset = OFFSET_IMAGEFILENAME - OFFSET_ACTIVEPROCESSLINKS;

	do
	{
		char currentKProcessName[15] = { 0 };

		// Obtain KProcessName
		DriverReader::ReadVirtualMemory(directoryTableBase, reinterpret_cast<uintptr_t>(currentEntry) + imagefilename_offset, &currentKProcessName, sizeof(currentKProcessName), NULL);

		if (strcmp(static_cast<const char *>(processName), currentKProcessName) == 0)
		{
			std::cout << "[+] KProcess Target Found: 0x" << std::hex << (uintptr_t)(currentEntry) - OFFSET_ACTIVEPROCESSLINKS << std::endl;
			return (uintptr_t)(currentEntry)-OFFSET_ACTIVEPROCESSLINKS;
		}

		// Set next entry on the list
		DriverReader::ReadVirtualMemory(directoryTableBase, reinterpret_cast<uintptr_t>(currentEntry) + sizeof(uintptr_t), &currentEntry, sizeof(currentEntry), NULL);

	} while (currentEntry != initialEntry);


	return 0;

}


// This function retrieves information from the EProcess/KProcess structure of the target process in kernel.
// All this will be stored on global variables so we can use them anywhere.
bool DriverReader::ObtainKProcessInfo(uintptr_t &directoryTableBase, uintptr_t pKProcessAddress)
{
	std::cout << "\t[+] Grabing info from target process" << std::endl;
	if (!DriverReader::ReadVirtualMemory(directoryTableBase, pKProcessAddress + OFFSET_SECTIONBASEADDRESS,
		&(DriverReader::pBaseAddressTargetProcess), sizeof(uintptr_t), NULL))
	{
		std::cout << "[-] Failed trying to obtain the BaseAddress." << std::endl;
		return false;
	}
	std::cout << "\t[+] BaseAddress: 0x" << std::hex << DriverReader::pBaseAddressTargetProcess << std::endl;


	if (!DriverReader::ReadVirtualMemory(directoryTableBase, pKProcessAddress + OFFSET_DIRECTORYTABLEBASE,
		&(DriverReader::DTBTargetProcess), sizeof(uintptr_t), NULL))
	{
		std::cout << "[-] Failed trying to obtain the DirectoryTableBase." << std::endl;
		return false;
	}
	std::cout << "\t[+] DirectoryTableBase: 0x" << std::hex << DriverReader::DTBTargetProcess << std::endl;

	if (!DriverReader::ReadVirtualMemory(directoryTableBase, pKProcessAddress + OFFSET_VIRTUALSIZE,
		&(DriverReader::virtualSizeTargetProcess), sizeof(uintptr_t), NULL))
	{
		std::cout << "[-] Failed trying to obtain the VirtualSize." << std::endl;
		return false;
	}
	std::cout << "\t[+] VirtualSize: 0x" << std::hex << DriverReader::virtualSizeTargetProcess << std::endl;

if (!DriverReader::ReadVirtualMemory(directoryTableBase, pKProcessAddress + OFFSET_VADROOT,
		&(DriverReader::pVadRootTargetProcess), sizeof(uintptr_t), NULL))
	{
		std::cout << "[-] Failed trying to obtain the VadRoot." << std::endl;
		return false;
	}
	std::cout << "\t[+] VadRoot: 0x" << std::hex << DriverReader::pVadRootTargetProcess << std::endl;


	if (!DriverReader::ReadVirtualMemory(directoryTableBase, pKProcessAddress + OFFSET_EPROCESS_PEB,
		&(DriverReader::pPEBTargetProcess), sizeof(uintptr_t), NULL))
	{
		std::cout << "[-] Failed trying to obtain the PEB." << std::endl;
		return false;
	}
	std::cout << "\t[+] PEB: 0x" << std::hex << DriverReader::pPEBTargetProcess << std::endl;


	return true;
}


// Functions that will help us to dump the VadRoot AVL Tree, which has all the memory information about a particular process.

EnumerateRemoteSectionData GetVadNodeInfo(uintptr_t directoryTableBase, uintptr_t node)
{
/*
#define OFFSET_STARTINGVPN 0x018
#define OFFSET_ENDINGVPN 0x01c
#define OFFSET_STARTINGVPNHIGH 0x020
#define OFFSET_ENDINGVPNHIGH 0x021
*/
	uint64_t startingVPNLow = 0;
	uint64_t endingVPNLow = 0;
	uint64_t startingVPNHigh = 0;
	uint64_t endingVPNHigh = 0;
	unsigned long u = 0;

	// Reading the starting and ending VPN.
	DriverReader::ReadVirtualMemory(directoryTableBase, node + OFFSET_STARTINGVPN, &startingVPNLow, sizeof(uint32_t), NULL);
	DriverReader::ReadVirtualMemory(directoryTableBase, node + OFFSET_ENDINGVPN, &endingVPNLow, sizeof(uint32_t), NULL);
	DriverReader::ReadVirtualMemory(directoryTableBase, node + OFFSET_STARTINGVPNHIGH, &startingVPNHigh, sizeof(uint8_t), NULL);
	DriverReader::ReadVirtualMemory(directoryTableBase, node + OFFSET_ENDINGVPNHIGH, &endingVPNHigh, sizeof(uint8_t), NULL);
	
	// Reading the unsigned long u from MMVAD_SHORT
	DriverReader::ReadVirtualMemory(directoryTableBase, node + OFFSET_MMVAD_SHORT_U, &u, sizeof(unsigned long), NULL);

	// We need to put together this two parts, some lshr will doo all the work.
	uint64_t startingVPN = (startingVPNLow << 12) | (startingVPNHigh << 44);
	uint64_t endingVPN = ( (endingVPNLow + 1) << 12 | (endingVPNHigh << 44));

	// Let's create the object for our section.
	EnumerateRemoteSectionData section = {};
	section.BaseAddress = (void *)startingVPN;
	section.Size = endingVPN - startingVPN;

	section.Protection = SectionProtection::NoAccess;
	// To get the Protection Flag we need first to obtain the index of the protection from the _MMVAD_FLAGS->Protection
	//		[+0x000 ( 2: 0)] VadType          : 0x2 [Type: unsigned long]
    //		[+0x000 ( 7: 3)] Protection       : 0x7 [Type: unsigned long]
    //		[+0x000 (13: 8)] PreferredNode    : 0x0 [Type: unsigned long]
    //		[+0x000 (14:14)] NoChange         : 0x0 [Type: unsigned long]
    //		[+0x000 (15:15)] PrivateMemory    : 0x0 [Type: unsigned long]
	//    0xF8 == 11111000  <----- Mask to extract bits 7:3
	DWORD protection = ( u >> 3) & 0x1F;
	protection = ProtectionFlags[protection];

	if ((protection & PAGE_EXECUTE) == PAGE_EXECUTE) section.Protection |= SectionProtection::Execute;
	if ((protection & PAGE_EXECUTE_READ) == PAGE_EXECUTE_READ) section.Protection |= SectionProtection::Execute | SectionProtection::Read;
	if ((protection & PAGE_EXECUTE_READWRITE) == PAGE_EXECUTE_READWRITE) section.Protection |= SectionProtection::Execute | SectionProtection::Read | SectionProtection::Write;
	if ((protection & PAGE_EXECUTE_WRITECOPY) == PAGE_EXECUTE_WRITECOPY) section.Protection |= SectionProtection::Execute | SectionProtection::Read | SectionProtection::CopyOnWrite;
	if ((protection & PAGE_READONLY) == PAGE_READONLY) section.Protection |= SectionProtection::Read;
	if ((protection & PAGE_READWRITE) == PAGE_READWRITE) section.Protection |= SectionProtection::Read | SectionProtection::Write;
	if ((protection & PAGE_WRITECOPY) == PAGE_WRITECOPY) section.Protection |= SectionProtection::Read | SectionProtection::CopyOnWrite;
	if ((protection & PAGE_GUARD) == PAGE_GUARD) section.Protection |= SectionProtection::Guard;

	//  [+0x000 (15:15)] PrivateMemory    : 0x0 [Type: unsigned long]
    //  [+0x000 (16:16)] PrivateFixup     : 0x0 [Type: unsigned long]
    //  [+0x000 (17:17)] ManySubsections  : 0x0 [Type: unsigned long]
    //  [+0x000 (18:18)] Enclave          : 0x0 [Type: unsigned long]
	//  We need the memory type, we can check with the bit 15 if its private memory
	//  TODO: not mandatory, this is why we see an unknown on the GUI when displaying all the sections.

	return section;
}


// Since we can't open a handle to the process and call VirtualQueryEx
void DriverReader::WalkVadAVLTree(uintptr_t directoryTableBase, uintptr_t start)
{

	if (start == NULL)
		return;
	
	// Since we need to traverse a balanced tree, 
	// we first read all the left branches and then we read the right one while we go up again.
	uintptr_t left = 0;
	DriverReader::ReadVirtualMemory(directoryTableBase, start, &left, sizeof(uintptr_t), NULL);

	// Yep, recursion ;)
	WalkVadAVLTree(directoryTableBase, left);

	// Now the right nodes.
	uintptr_t right = 0;
	DriverReader::ReadVirtualMemory(directoryTableBase, start + sizeof(uintptr_t), &right, sizeof(uintptr_t), NULL);

	// We need to obtain information from each node: starting and ending address, protection, etc.
	EnumerateRemoteSectionData section = GetVadNodeInfo(directoryTableBase, start);

	// We push that information so we can later notify ReClass.
	DriverReader::sections.push_back(section);

	// And again recursion
	WalkVadAVLTree(directoryTableBase, right);
}
	
	
void DriverReader::EnumRing3ProcessModules(uintptr_t directoryTableBase)
{
 
	// Variables used to store lpr pointer and data.
	PEB_LDR_DATA ldr;
	uintptr_t pLDR = 0;

	// We need to dereference the pointer and obtain retrieve the whole LDR structure.
	DriverReader::ReadVirtualMemory(DriverReader::DTBTargetProcess, DriverReader::pPEBTargetProcess + OFFSET_PEB_LDR , &pLDR, sizeof(uintptr_t), NULL);
	DriverReader::ReadVirtualMemory(DriverReader::DTBTargetProcess, pLDR , &ldr,sizeof(PEB_LDR_DATA), NULL);

	// InMemoryOrderModuleList will have the head of a linked list.
	// We can traverse the whole list to obtain all the currently loaded modules.
	LIST_ENTRY* head = ldr.InMemoryOrderModuleList.Flink;
	LIST_ENTRY* next = head;

    PLDR_MODULE pLdrModule = nullptr;
	LDR_MODULE LdrModule;
	do
	{
            LDR_DATA_TABLE_ENTRY LdrEntry;
            LDR_DATA_TABLE_ENTRY* Base = CONTAINING_RECORD(head, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);

            if (DriverReader::ReadVirtualMemory(DriverReader::DTBTargetProcess, (uintptr_t)Base, &LdrEntry, sizeof(LdrEntry), NULL))
            {
                char* pLdrModuleOffset = reinterpret_cast<char*>(head) - sizeof(LIST_ENTRY);
				
				// Obtaining module pointer
				DriverReader::ReadVirtualMemory(DriverReader::DTBTargetProcess, (uintptr_t)pLdrModuleOffset, &pLdrModule, sizeof(pLdrModule), NULL);
                // Retrieven module information
				DriverReader::ReadVirtualMemory(DriverReader::DTBTargetProcess, (uintptr_t)pLdrModule, &LdrModule, sizeof(LdrModule), NULL);

                if (LdrEntry.DllBase)
                {
					//std::wstring fullname = LdrModule.FullDllName;

					// Retrieve the FullDllName
					WCHAR strFullDllName[MAX_PATH] = { 0 };
					if (DriverReader::ReadVirtualMemory(DriverReader::DTBTargetProcess, 
						reinterpret_cast<uintptr_t>(LdrModule.FullDllName.Buffer),
						&strFullDllName,
						LdrModule.FullDllName.Length, NULL))
					{
						// We create the EnumerateRemoteModuleData so we can return it to ReClass
						EnumerateRemoteModuleData module = {};
						
						// Debuging code :P
						// wprintf(L"Full Dll Name: %s\n", strFullDllName);
						// std::cout<< "BaseAddress:     " << LdrModule.BaseAddress<<std::endl;
						
						module.BaseAddress = LdrModule.BaseAddress;
						std::copy(strFullDllName, strFullDllName + MAX_PATH, module.Path);
						module.Size = LdrModule.SizeOfImage;
						
						// We push the current module into the vecto we later use to notify ReClass
						DriverReader::modules.push_back(module);
					}
                }

                head = LdrEntry.InMemoryOrderLinks.Flink;
            }
        }
        while (head != next);

	return;
}