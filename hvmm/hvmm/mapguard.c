//
// mapguard.c: keeps the guest mappings honest.
//
// A mapping made by IOCTL_MAP_GPA_RANGE points at the host frames that backed the guest when
// it was made. Nothing pins those frames: when the VM stops, or the host takes memory back, the
// frames go back to the host and a stale mapping would show someone else's memory. Two things
// here stop that, and neither costs anything on a read:
//
//  1. When a VM worker process (vmwp.exe) exits, every mapping of its partition is unmapped at
//     once from the process that holds it. The process-exit notification runs before the
//     worker's handles are closed, so before vid.sys can tear the partition down.
//  2. IOCTL_VERIFY_GPA_MAPPINGS re-reads a few frames of every mapping of a handle from the
//     partition and unmaps the ones that no longer match. A client sends it about once a second
//     to catch what (1) cannot see, such as memory taken back while the VM keeps running.
//
// A read from an unmapped range faults in the client, which then falls back to the driver's
// classic read path. Nothing is ever read from a frame the guest no longer owns, as long as
// (1) fires before the frames are released; the verify call is the backstop.
//

#include "hvmm.h"

NTKERNELAPI PCHAR NTAPI PsGetProcessImageFileName(_In_ PEPROCESS Process);

#define MAPGUARD_SYSTEM_EXTENDED_HANDLE_INFORMATION ((SYSTEM_INFORMATION_CLASS)64)

typedef struct _MAPGUARD_HANDLE_ENTRY {
	PVOID Object;
	ULONG_PTR UniqueProcessId;
	ULONG_PTR HandleValue;
	ULONG GrantedAccess;
	USHORT CreatorBackTraceIndex;
	USHORT ObjectTypeIndex;
	ULONG HandleAttributes;
	ULONG Reserved;
} MAPGUARD_HANDLE_ENTRY, *PMAPGUARD_HANDLE_ENTRY;

typedef struct _MAPGUARD_HANDLE_INFORMATION {
	ULONG_PTR NumberOfHandles;
	ULONG_PTR Reserved;
	MAPGUARD_HANDLE_ENTRY Handles[1];
} MAPGUARD_HANDLE_INFORMATION, *PMAPGUARD_HANDLE_INFORMATION;

//
// Every open handle to \\.\hvmm, so the exit notification can find the mappings.
// Lock order: g_ContextLock, then a context's own Lock.
//

static LIST_ENTRY g_ContextList;
static KSPIN_LOCK g_ContextLock;
static BOOLEAN g_NotifyRegistered = FALSE;
static volatile LONG g_DroppedByExit = 0;

//
// Host RAM ranges, read once. Hot-added memory after that is simply not mapped (the classic
// read path still serves it).
//

static PPHYSICAL_MEMORY_RANGE g_RamRanges = NULL;
static FAST_MUTEX g_RamLock;

//
// The last partition -> worker lookup. Finding the worker walks the system handle table, so it
// is done once per partition, not once per map call.
//

static FAST_MUTEX g_VmwpCacheLock;
static PFILE_OBJECT g_VmwpCachePartition = NULL;
static PEPROCESS g_VmwpCacheProcess = NULL;

static VOID MapGuardProcessNotify(HANDLE ParentId, HANDLE ProcessId, BOOLEAN Create);

NTSTATUS MapGuardInit(VOID)
{
	NTSTATUS Status;

	InitializeListHead(&g_ContextList);
	KeInitializeSpinLock(&g_ContextLock);
	ExInitializeFastMutex(&g_RamLock);
	ExInitializeFastMutex(&g_VmwpCacheLock);

	Status = PsSetCreateProcessNotifyRoutine(MapGuardProcessNotify, FALSE);
	if (NT_SUCCESS(Status)) {
		g_NotifyRegistered = TRUE;
	}
	else {
		KDbgLog("MapGuardInit: PsSetCreateProcessNotifyRoutine failed", Status);
	}
	return Status;
}

VOID MapGuardUnload(VOID)
{
	if (g_NotifyRegistered) {
		PsSetCreateProcessNotifyRoutine(MapGuardProcessNotify, TRUE);
		g_NotifyRegistered = FALSE;
	}

	ExAcquireFastMutex(&g_VmwpCacheLock);
	if (g_VmwpCacheProcess != NULL) {
		ObDereferenceObject(g_VmwpCacheProcess);
		g_VmwpCacheProcess = NULL;
	}
	g_VmwpCachePartition = NULL;
	ExReleaseFastMutex(&g_VmwpCacheLock);

	if (g_RamRanges != NULL) {
		ExFreePool(g_RamRanges);
		g_RamRanges = NULL;
	}
}

VOID MapGuardRegisterContext(PHVMM_FILE_CONTEXT pFileContext)
{
	KIRQL OldIrql;

	KeAcquireSpinLock(&g_ContextLock, &OldIrql);
	InsertTailList(&g_ContextList, &pFileContext->GlobalLink);
	KeReleaseSpinLock(&g_ContextLock, OldIrql);
}

VOID MapGuardUnregisterContext(PHVMM_FILE_CONTEXT pFileContext)
{
	KIRQL OldIrql;

	KeAcquireSpinLock(&g_ContextLock, &OldIrql);
	RemoveEntryList(&pFileContext->GlobalLink);
	InitializeListHead(&pFileContext->GlobalLink);
	KeReleaseSpinLock(&g_ContextLock, OldIrql);
}

//
// True when the frame lies inside one of the host RAM ranges.
//

BOOLEAN MapGuardIsHostRam(ULONG64 Pfn)
{
	PPHYSICAL_MEMORY_RANGE Ranges;
	ULONG i;

	if (g_RamRanges == NULL) {
		ExAcquireFastMutex(&g_RamLock);
		if (g_RamRanges == NULL) {
			g_RamRanges = MmGetPhysicalMemoryRanges();
		}
		ExReleaseFastMutex(&g_RamLock);
	}

	Ranges = g_RamRanges;
	if (Ranges == NULL) {
		return FALSE;
	}

	for (i = 0; Ranges[i].BaseAddress.QuadPart != 0 || Ranges[i].NumberOfBytes.QuadPart != 0; i++) {
		ULONG64 First = (ULONG64)Ranges[i].BaseAddress.QuadPart / PAGE_SIZE;
		ULONG64 Count = (ULONG64)Ranges[i].NumberOfBytes.QuadPart / PAGE_SIZE;
		if (Pfn >= First && Pfn < First + Count) {
			return TRUE;
		}
	}
	return FALSE;
}

//
// The vmwp.exe process that holds a handle to this partition, referenced, or NULL. Found in the
// system handle table: the entry whose object is the partition file object and whose process
// image is vmwp.exe.
//

PEPROCESS MapGuardFindVmwp(PFILE_OBJECT Partition)
{
	PMAPGUARD_HANDLE_INFORMATION Info = NULL;
	ULONG Size = 0x100000;
	ULONG Needed = 0;
	NTSTATUS Status;
	PEPROCESS Found = NULL;
	ULONG_PTR i;

	ExAcquireFastMutex(&g_VmwpCacheLock);
	if (g_VmwpCachePartition == Partition && g_VmwpCacheProcess != NULL &&
		PsGetProcessExitStatus(g_VmwpCacheProcess) == STATUS_PENDING) {
		Found = g_VmwpCacheProcess;
		ObReferenceObject(Found);
		ExReleaseFastMutex(&g_VmwpCacheLock);
		return Found;
	}
	ExReleaseFastMutex(&g_VmwpCacheLock);

	for (;;) {
		Info = (PMAPGUARD_HANDLE_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, Size, 'gMvH');
		if (Info == NULL) {
			return NULL;
		}
		Status = ZwQuerySystemInformation(MAPGUARD_SYSTEM_EXTENDED_HANDLE_INFORMATION, Info, Size, &Needed);
		if (Status != STATUS_INFO_LENGTH_MISMATCH) {
			break;
		}
		ExFreePoolWithTag(Info, 'gMvH');
		Info = NULL;
		Size = (Needed > Size ? Needed : Size * 2) + 0x10000;
		if (Size > 0x10000000) {
			return NULL;
		}
	}

	if (!NT_SUCCESS(Status)) {
		KDbgLog("MapGuardFindVmwp: handle query failed", Status);
		ExFreePoolWithTag(Info, 'gMvH');
		return NULL;
	}

	for (i = 0; i < Info->NumberOfHandles && Found == NULL; i++) {
		PEPROCESS Process = NULL;
		PCHAR Image;

		if (Info->Handles[i].Object != (PVOID)Partition) {
			continue;
		}
		if (!NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)Info->Handles[i].UniqueProcessId, &Process))) {
			continue;
		}
		Image = PsGetProcessImageFileName(Process);
		if (Image != NULL && _stricmp(Image, "vmwp.exe") == 0) {
			Found = Process;            // keeps the lookup's reference
		}
		else {
			ObDereferenceObject(Process);
		}
	}

	ExFreePoolWithTag(Info, 'gMvH');

	if (Found == NULL) {
		KDbgPrintString("MapGuardFindVmwp: no vmwp.exe holds this partition");
		return NULL;
	}

	ExAcquireFastMutex(&g_VmwpCacheLock);
	if (g_VmwpCacheProcess != NULL) {
		ObDereferenceObject(g_VmwpCacheProcess);
	}
	g_VmwpCachePartition = Partition;
	g_VmwpCacheProcess = Found;
	ObReferenceObject(Found);           // one for the cache, one for the caller
	ExReleaseFastMutex(&g_VmwpCacheLock);

	return Found;
}

//
// A VM worker exited: take every mapping of its partitions off every handle and unmap it.
// Runs at PASSIVE_LEVEL in the context of the exiting process.
//

static VOID MapGuardProcessNotify(HANDLE ParentId, HANDLE ProcessId, BOOLEAN Create)
{
	LIST_ENTRY Doomed;
	PLIST_ENTRY pCtxEntry;
	KIRQL OldIrql;
	LONG Count = 0;

	UNREFERENCED_PARAMETER(ParentId);

	if (Create) {
		return;
	}

	InitializeListHead(&Doomed);

	KeAcquireSpinLock(&g_ContextLock, &OldIrql);
	for (pCtxEntry = g_ContextList.Flink; pCtxEntry != &g_ContextList; pCtxEntry = pCtxEntry->Flink) {
		PHVMM_FILE_CONTEXT pCtx = CONTAINING_RECORD(pCtxEntry, HVMM_FILE_CONTEXT, GlobalLink);
		PLIST_ENTRY pEntry, pNext;

		KeAcquireSpinLockAtDpcLevel(&pCtx->Lock);
		for (pEntry = pCtx->MappingList.Flink; pEntry != &pCtx->MappingList; pEntry = pNext) {
			PHVMM_GPA_MAPPING pMapping = CONTAINING_RECORD(pEntry, HVMM_GPA_MAPPING, Link);
			pNext = pEntry->Flink;
			if (pMapping->Vmwp != NULL && PsGetProcessId(pMapping->Vmwp) == ProcessId) {
				RemoveEntryList(pEntry);
				pCtx->MappingCount--;
				InsertTailList(&Doomed, pEntry);
				Count++;
			}
		}
		KeReleaseSpinLockFromDpcLevel(&pCtx->Lock);
	}
	KeReleaseSpinLock(&g_ContextLock, OldIrql);

	if (Count == 0) {
		return;
	}

	KDbgLog("MapGuard: VM worker exited, unmapping", (ULONG)Count);
	InterlockedAdd(&g_DroppedByExit, Count);

	while (!IsListEmpty(&Doomed)) {
		VidReleaseGpaMapping(CONTAINING_RECORD(RemoveHeadList(&Doomed), HVMM_GPA_MAPPING, Link));
	}
}

//
// True when a few frames of the mapping (first, middle, last page) are still the frames the
// partition maps those guest pages to.
//

static BOOLEAN MapGuardMappingStillValid(PHVMM_GPA_MAPPING pMapping)
{
	PVM_PROCESS_CONTEXT pPartitionHandle;
	VM_LAYOUT Layout;
	PPFN_NUMBER MdlPfns;
	UINT64 Pages = pMapping->MappedBytes / PAGE_SIZE;
	UINT64 Samples[3];
	ULONG s;
	BOOLEAN Valid = TRUE;

	if (pMapping->Vmwp != NULL && PsGetProcessExitStatus(pMapping->Vmwp) != STATUS_PENDING) {
		return FALSE;
	}
	if (pMapping->Partition == NULL || pMapping->Partition->FsContext == NULL ||
		(pMapping->Partition->Flags & FO_CLEANUP_COMPLETE) != 0) {
		return FALSE;
	}

	pPartitionHandle = (PVM_PROCESS_CONTEXT)((PCHAR)pMapping->Partition->FsContext - 1);
	if (!VidResolveLayout(pPartitionHandle, &Layout) || !Layout.IsFullVm) {
		return FALSE;
	}

	MdlPfns = MmGetMdlPfnArray(pMapping->Mdl);
	Samples[0] = 0;
	Samples[1] = Pages / 2;
	Samples[2] = Pages - 1;

	__try {
		for (s = 0; s < 3 && Valid; s++) {
			UINT64 Page = pMapping->GpaStartPage + Samples[s];
			PGPAR_OBJECT objGpar = VidGetGparObjectForGpa(pPartitionHandle, &Layout, Page);
			PMEMORY_BLOCK objMBlock;
			PCHAR pArray;
			ULONG64 HostPfn;

			if (objGpar == NULL || Page < objGpar->GpaIndexStart || Page > objGpar->GpaIndexEnd) {
				Valid = FALSE;
				break;
			}
			objMBlock = (PMEMORY_BLOCK)*(PVOID*)((PCHAR)objGpar + Layout.ObjMblockOffset);
			if (objMBlock == NULL) {
				Valid = FALSE;
				break;
			}
			pArray = (PCHAR)*(PVOID*)((PCHAR)objMBlock + Layout.GuestGpaArrayOffset);
			if (pArray == NULL) {
				Valid = FALSE;
				break;
			}
			HostPfn = *(PULONG)(pArray + 0x10 * (Page - objGpar->GpaIndexStart));
			if (HostPfn != (ULONG64)MdlPfns[Samples[s]]) {
				Valid = FALSE;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		Valid = FALSE;
	}

	return Valid;
}

//
// IOCTL_VERIFY_GPA_MAPPINGS
//

NTSTATUS MapGuardVerify(PFILE_OBJECT FileObject, PCHAR pBuffer, ULONG outLen, PULONG pBytesReturned)
{
	PHVMM_FILE_CONTEXT pFileContext;
	VERIFY_GPA_MAPPINGS_OUTPUT Output;
	LIST_ENTRY Work, Keep, Drop;
	KIRQL OldIrql;

	*pBytesReturned = 0;

	if (outLen < sizeof(VERIFY_GPA_MAPPINGS_OUTPUT)) {
		return STATUS_BUFFER_TOO_SMALL;
	}
	if (FileObject == NULL || FileObject->FsContext2 == NULL) {
		return STATUS_INVALID_DEVICE_STATE;
	}
	pFileContext = (PHVMM_FILE_CONTEXT)FileObject->FsContext2;

	RtlZeroMemory(&Output, sizeof(Output));
	InitializeListHead(&Work);
	InitializeListHead(&Keep);
	InitializeListHead(&Drop);

	//
	// Take the mappings off the handle, check them at PASSIVE_LEVEL, put the good ones back.
	//

	KeAcquireSpinLock(&pFileContext->Lock, &OldIrql);
	while (!IsListEmpty(&pFileContext->MappingList)) {
		InsertTailList(&Work, RemoveHeadList(&pFileContext->MappingList));
		pFileContext->MappingCount--;
	}
	KeReleaseSpinLock(&pFileContext->Lock, OldIrql);

	while (!IsListEmpty(&Work)) {
		PLIST_ENTRY pEntry = RemoveHeadList(&Work);
		PHVMM_GPA_MAPPING pMapping = CONTAINING_RECORD(pEntry, HVMM_GPA_MAPPING, Link);

		Output.Checked++;
		if (MapGuardMappingStillValid(pMapping)) {
			InsertTailList(&Keep, pEntry);
		}
		else {
			InsertTailList(&Drop, pEntry);
			Output.Dropped++;
		}
	}

	KeAcquireSpinLock(&pFileContext->Lock, &OldIrql);
	while (!IsListEmpty(&Keep)) {
		InsertTailList(&pFileContext->MappingList, RemoveHeadList(&Keep));
		pFileContext->MappingCount++;
	}
	KeReleaseSpinLock(&pFileContext->Lock, OldIrql);

	while (!IsListEmpty(&Drop)) {
		VidReleaseGpaMapping(CONTAINING_RECORD(RemoveHeadList(&Drop), HVMM_GPA_MAPPING, Link));
	}

	Output.DroppedByExit = (UINT32)g_DroppedByExit;
	RtlCopyMemory(pBuffer, &Output, sizeof(Output));
	*pBytesReturned = sizeof(Output);
	return STATUS_SUCCESS;
}
