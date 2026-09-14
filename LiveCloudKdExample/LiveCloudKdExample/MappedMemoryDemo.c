//
// MappedMemoryDemo.c: verification and benchmark for the mapped guest memory
// path (HvmmMappedMemory.h over IOCTL_MAP_GPA_RANGE in hvmm.sys).
//
// Runs as action [2] of LiveCloudKdExample:
//   1. maps every run of the selected VM and prints coverage;
//   2. byte-compares random pages read through the mapping against
//      SdkReadPhysicalMemory (ReadInterfaceHvmmDrvInternal);
//   3. times 512 scattered 4 KB reads and one 16 MB contiguous read both ways;
//   4. optionally lets you stop the VM and checks the mapping survives.
//

#include "LiveCloudKdExample.h"
#include "HvmmMappedMemory.h"

#define DEMO_COMPARE_PAGES      10000
#define DEMO_SCATTER_PAGES      512
#define DEMO_CONTIG_BYTES       (16ULL * 1024 * 1024)
#define DEMO_CONTIG_BLOCK       (1024 * 1024)   // what LiveCloudKd's dumper uses per read

static ULONG64 g_RngState = 0x9E3779B97F4A7C15ULL;

static ULONG64 DemoRandom()
{
	// xorshift64*: deterministic so a failing page can be reproduced.
	g_RngState ^= g_RngState >> 12;
	g_RngState ^= g_RngState << 25;
	g_RngState ^= g_RngState >> 27;
	return g_RngState * 2685821657736338717ULL;
}

static double DemoSeconds(LARGE_INTEGER Start, LARGE_INTEGER End)
{
	static LARGE_INTEGER Freq = { 0 };
	if (Freq.QuadPart == 0) {
		QueryPerformanceFrequency(&Freq);
	}
	return (double)(End.QuadPart - Start.QuadPart) / (double)Freq.QuadPart;
}

//
// Pick a random page-aligned GPA that is inside a mapped chunk.
//

static UINT64 DemoRandomMappedPage(PHVMM_MAPPED_MEMORY Mapped)
{
	PHVMM_MAPPED_CHUNK Chunk = &Mapped->Chunks[DemoRandom() % Mapped->ChunkCount];
	UINT64 Pages = Chunk->Length / DUMP_PAGE_SIZE;
	return Chunk->GpaStart + (DemoRandom() % Pages) * DUMP_PAGE_SIZE;
}

static VOID DemoPrintCoverage(PHVMM_MAPPED_MEMORY Mapped, double MapSeconds)
{
	ULONG i;

	wprintf(L"\n   Runs reported by hvlib: %lu\n", Mapped->RunCount);
	for (i = 0; i < Mapped->RunCount && i < 16; i++) {
		wprintf(L"    run %2lu  GPA 0x%012llX  length 0x%llX (%llu MB)\n", i, Mapped->Runs[i].GpaStart, Mapped->Runs[i].Length, Mapped->Runs[i].Length >> 20);
	}
	if (Mapped->RunCount > 16) {
		wprintf(L"    ... %lu more\n", Mapped->RunCount - 16);
	}

	wprintf(L"   Mapped chunks: %lu (%llu map IOCTLs, %.3f s)\n", Mapped->ChunkCount, Mapped->MapCalls, MapSeconds);
	for (i = 0; i < Mapped->ChunkCount && i < 16; i++) {
		wprintf(L"    chunk %2lu  GPA 0x%012llX  length 0x%llX  VA %p\n", i, Mapped->Chunks[i].GpaStart, Mapped->Chunks[i].Length, Mapped->Chunks[i].UserVa);
	}
	if (Mapped->ChunkCount > 16) {
		wprintf(L"    ... %lu more\n", Mapped->ChunkCount - 16);
	}

	wprintf(L"   Coverage: %llu MB of %llu MB (%.2f%%)\n",
		Mapped->MappedBytes >> 20, Mapped->RunBytes >> 20,
		Mapped->RunBytes ? 100.0 * (double)Mapped->MappedBytes / (double)Mapped->RunBytes : 0.0);
}

//
// The whole design rests on the mapping being read-only. Prove it on every run:
// a write into the first mapped page must fault.
//

static BOOLEAN DemoWriteMustFault(PHVMM_MAPPED_MEMORY Mapped)
{
	volatile UCHAR *Page = (volatile UCHAR *)Mapped->Chunks[0].UserVa;
	BOOLEAN Faulted = FALSE;

	__try {
		Page[0] = Page[0];
	}
	__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		Faulted = TRUE;
	}

	wprintf(L"   Write to the mapping: %s\n", Faulted ? L"faulted (read-only, as required)" : L"SUCCEEDED - the mapping is writable, this is a bug");
	return Faulted;
}

//
// Byte compare. The VM should be suspended for this; a running guest changes
// pages between the two reads and would show as mismatches. Pages that differ
// are re-read once so a transient change is reported separately from a stable
// one. Only stable mismatches are bugs.
//

static BOOLEAN DemoByteCompare(ULONG64 Partition, PHVMM_MAPPED_MEMORY Mapped, BOOLEAN Suspended)
{
	PUCHAR ViaSdk = (PUCHAR)malloc(DUMP_PAGE_SIZE);
	PUCHAR ViaMap = (PUCHAR)malloc(DUMP_PAGE_SIZE);
	ULONG i;
	ULONG Compared = 0, SdkFailed = 0, Transient = 0, Stable = 0;
	ULONG Pages = DEMO_COMPARE_PAGES;
	UINT64 FirstStable = 0;

	if (ViaSdk == NULL || ViaMap == NULL) {
		free(ViaSdk);
		free(ViaMap);
		return FALSE;
	}

	if (Mapped->MappedBytes / DUMP_PAGE_SIZE < Pages) {
		Pages = (ULONG)(Mapped->MappedBytes / DUMP_PAGE_SIZE);
	}

	wprintf(L"\n   Byte compare: %lu random pages, mapping vs SdkReadPhysicalMemory(ReadInterfaceHvmmDrvInternal)%s\n",
		Pages, Suspended ? L", VM suspended" : L", VM running (transient mismatches expected)");

	for (i = 0; i < Pages; i++) {
		UINT64 Gpa = DemoRandomMappedPage(Mapped);

		if (!SdkReadPhysicalMemory(Partition, Gpa, DUMP_PAGE_SIZE, ViaSdk, ReadInterfaceHvmmDrvInternal)) {
			SdkFailed++;
			continue;
		}
		if (!HvmmMappedRead(Mapped, Gpa, ViaMap, DUMP_PAGE_SIZE)) {
			wprintf(L"   ERROR: HvmmMappedRead failed at GPA 0x%llX\n", Gpa);
			break;
		}
		Compared++;

		if (memcmp(ViaSdk, ViaMap, DUMP_PAGE_SIZE) != 0) {
			//
			// Re-read both. A stable difference is a real bug.
			//
			SdkReadPhysicalMemory(Partition, Gpa, DUMP_PAGE_SIZE, ViaSdk, ReadInterfaceHvmmDrvInternal);
			HvmmMappedRead(Mapped, Gpa, ViaMap, DUMP_PAGE_SIZE);
			if (memcmp(ViaSdk, ViaMap, DUMP_PAGE_SIZE) != 0) {
				if (Stable == 0) {
					FirstStable = Gpa;
				}
				Stable++;
			}
			else {
				Transient++;
			}
		}
	}

	wprintf(L"   compared %lu  sdk-read-failed %lu  transient %lu  STABLE MISMATCH %lu\n", Compared, SdkFailed, Transient, Stable);
	if (Stable != 0) {
		ULONG Off;
		SdkReadPhysicalMemory(Partition, FirstStable, DUMP_PAGE_SIZE, ViaSdk, ReadInterfaceHvmmDrvInternal);
		HvmmMappedRead(Mapped, FirstStable, ViaMap, DUMP_PAGE_SIZE);
		wprintf(L"   first stable mismatch at GPA 0x%llX, first differing bytes:\n", FirstStable);
		for (Off = 0; Off < DUMP_PAGE_SIZE; Off++) {
			if (ViaSdk[Off] != ViaMap[Off]) {
				wprintf(L"    +0x%03lX  sdk %02X  map %02X\n", Off, ViaSdk[Off], ViaMap[Off]);
				break;
			}
		}
	}

	free(ViaSdk);
	free(ViaMap);
	return Stable == 0 ? TRUE : FALSE;
}

static VOID DemoBenchmark(ULONG64 Partition, PHVMM_MAPPED_MEMORY Mapped)
{
	PUCHAR Buffer = (PUCHAR)malloc(DEMO_CONTIG_BLOCK);
	UINT64 *Gpas = (UINT64 *)malloc(DEMO_SCATTER_PAGES * sizeof(UINT64));
	LARGE_INTEGER T0, T1;
	double SdkScatter, MapScatter, SdkContig, MapContig;
	ULONG i;
	UINT64 ContigStart = 0, ContigAvail = 0, Off;
	volatile UCHAR Sink = 0;

	if (Buffer == NULL || Gpas == NULL) {
		free(Buffer);
		free(Gpas);
		return;
	}

	for (i = 0; i < DEMO_SCATTER_PAGES; i++) {
		Gpas[i] = DemoRandomMappedPage(Mapped);
	}

	//
	// 512 scattered pages
	//

	QueryPerformanceCounter(&T0);
	for (i = 0; i < DEMO_SCATTER_PAGES; i++) {
		SdkReadPhysicalMemory(Partition, Gpas[i], DUMP_PAGE_SIZE, Buffer, ReadInterfaceHvmmDrvInternal);
	}
	QueryPerformanceCounter(&T1);
	SdkScatter = DemoSeconds(T0, T1);

	QueryPerformanceCounter(&T0);
	for (i = 0; i < DEMO_SCATTER_PAGES; i++) {
		HvmmMappedRead(Mapped, Gpas[i], Buffer, DUMP_PAGE_SIZE);
	}
	QueryPerformanceCounter(&T1);
	MapScatter = DemoSeconds(T0, T1);

	//
	// 16 MB contiguous, from the largest chunk, in 1 MB blocks like the dumper.
	//

	for (i = 0; i < Mapped->ChunkCount; i++) {
		if (Mapped->Chunks[i].Length > ContigAvail) {
			ContigAvail = Mapped->Chunks[i].Length;
			ContigStart = Mapped->Chunks[i].GpaStart;
		}
	}
	if (ContigAvail > DEMO_CONTIG_BYTES) {
		ContigAvail = DEMO_CONTIG_BYTES;
	}

	QueryPerformanceCounter(&T0);
	for (Off = 0; Off + DEMO_CONTIG_BLOCK <= ContigAvail; Off += DEMO_CONTIG_BLOCK) {
		SdkReadPhysicalMemory(Partition, ContigStart + Off, DEMO_CONTIG_BLOCK, Buffer, ReadInterfaceHvmmDrvInternal);
	}
	QueryPerformanceCounter(&T1);
	SdkContig = DemoSeconds(T0, T1);

	QueryPerformanceCounter(&T0);
	for (Off = 0; Off + DEMO_CONTIG_BLOCK <= ContigAvail; Off += DEMO_CONTIG_BLOCK) {
		HvmmMappedRead(Mapped, ContigStart + Off, Buffer, DEMO_CONTIG_BLOCK);
		Sink ^= Buffer[0];
	}
	QueryPerformanceCounter(&T1);
	MapContig = DemoSeconds(T0, T1);

	wprintf(L"\n   Benchmark                     SdkReadPhysicalMemory     mapped        speedup\n");
	wprintf(L"   %-28s  %10.3f ms  %10.3f ms  %8.1fx\n", L"512 scattered 4 KB reads", SdkScatter * 1e3, MapScatter * 1e3, MapScatter > 0 ? SdkScatter / MapScatter : 0.0);
	wprintf(L"   %-28s  %10.3f ms  %10.3f ms  %8.1fx\n", L"16 MB contiguous read", SdkContig * 1e3, MapContig * 1e3, MapContig > 0 ? SdkContig / MapContig : 0.0);
	wprintf(L"   %-28s  %10.1f MB/s %10.1f MB/s\n", L"contiguous throughput",
		SdkContig > 0 ? (double)ContigAvail / SdkContig / 1048576.0 : 0.0,
		MapContig > 0 ? (double)ContigAvail / MapContig / 1048576.0 : 0.0);

	free(Buffer);
	free(Gpas);
}

static VOID DemoLifecycle(PHVMM_MAPPED_MEMORY Mapped)
{
	UCHAR Buffer[DUMP_PAGE_SIZE];
	int Key;

	wprintf(L"\n   Lifecycle test: stop (or turn off) the VM now, then press 'y' here. Any other key skips.\n   > ");
	Key = _getch();
	wprintf(L"%c\n", Key);
	if (Key != 'y' && Key != 'Y') {
		return;
	}

	wprintf(L"   HvmmMappedIsAlive: %s (expected: FALSE after the VM stopped)\n", HvmmMappedIsAlive(Mapped) ? L"TRUE" : L"FALSE");
	wprintf(L"   Reading one page through the stale mapping: ");
	if (HvmmMappedRead(Mapped, Mapped->Chunks[0].GpaStart, Buffer, DUMP_PAGE_SIZE)) {
		wprintf(L"ok (no crash; the bytes are whatever the host put in those pages now)\n");
	}
	else {
		wprintf(L"read failed (fallback path)\n");
	}
	wprintf(L"   Start the VM again and rerun action 2 to check remap.\n");
}

BOOLEAN MappedMemoryDemo(ULONG64 Partition)
{
	PHVMM_MAPPED_MEMORY Mapped = NULL;
	LARGE_INTEGER T0, T1;
	BOOLEAN Suspended = FALSE;
	BOOLEAN CompareOk;
	int Key;

	wprintf(L"\n   Mapped memory: opening \\\\.\\hvmm and mapping every run...\n");

	QueryPerformanceCounter(&T0);
	if (!HvmmMappedOpen(Partition, NULL, &Mapped)) {
		DWORD Error = GetLastError();
		if (Error == ERROR_NOT_SUPPORTED) {
			wprintf(L"   ERROR: the loaded hvmm.sys did not answer the mapping query, so no map request was sent.\n"
				L"   Load the hvmm.sys built from this branch ('sc qc hvmm' shows which binary the service uses).\n");
		}
		else {
			wprintf(L"   ERROR: HvmmMappedOpen failed (LastError %lu).\n"
				L"   Causes: a container partition, or the VM is not running.\n", Error);
		}
		return FALSE;
	}
	QueryPerformanceCounter(&T1);

	DemoPrintCoverage(Mapped, DemoSeconds(T0, T1));

	if (!DemoWriteMustFault(Mapped)) {
		HvmmMappedClose(Mapped);
		return FALSE;
	}

	wprintf(L"\n   Suspend the VM for the byte compare? (y/n) > ");
	Key = _getch();
	wprintf(L"%c\n", Key);
	if (Key == 'y' || Key == 'Y') {
		Suspended = SdkControlVmState(Partition, SuspendVm, SuspendResumePowershell, FALSE);
		if (!Suspended) {
			wprintf(L"   WARNING: suspend failed, comparing against a running guest.\n");
		}
	}

	CompareOk = DemoByteCompare(Partition, Mapped, Suspended);

	if (Suspended) {
		SdkControlVmState(Partition, ResumeVm, SuspendResumePowershell, FALSE);
	}

	DemoBenchmark(Partition, Mapped);

	wprintf(L"\n   Fallback reads issued so far: %llu\n", Mapped->FallbackReads);
	wprintf(L"   Byte compare: %s\n", CompareOk ? L"PASS" : L"FAIL");

	DemoLifecycle(Mapped);
	HvmmMappedClose(Mapped);

	return CompareOk;
}
