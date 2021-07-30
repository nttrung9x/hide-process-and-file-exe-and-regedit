#include "PsMonitor.h"
#include "ExcludeList.h"
#include "Helper.h"
#include "PsTable.h"
#include "PsRules.h"
#include "Driver.h"
#include "Configs.h"

#define PSMON_ALLOC_TAG 'nMsP'

#define PROCESS_QUERY_LIMITED_INFORMATION      0x1000
#define SYSTEM_PROCESS_ID (HANDLE)4

BOOLEAN g_psMonitorInited = FALSE;
PVOID g_obRegCallback = NULL;

OB_OPERATION_REGISTRATION g_regOperation[2];
OB_CALLBACK_REGISTRATION g_regCallback;

PsRulesContext g_excludeProcessRules;
PsRulesContext g_protectProcessRules;
PsRulesContext g_hideProcessRules;

FAST_MUTEX      g_processTableLock;
KGUARDED_MUTEX  g_activeProcListLock;

typedef struct _ProcessListEntry {
	LPCWSTR path;
	ULONG inherit;
} ProcessListEntry, *PProcessListEntry;

// Use this variable for hard code full path to applications that can see hidden objects
// For instance: L"\\Device\\HarddiskVolume1\\Windows\\System32\\calc.exe",
// Notice: this array should be NULL terminated
CONST ProcessListEntry g_excludeProcesses[] = {
	{ NULL, PsRuleTypeWithoutInherit }
};

// Use this variable for hard code full path to applications that will be protected 
// For instance: L"\\Device\\HarddiskVolume1\\Windows\\System32\\cmd.exe",
// Notice: this array should be NULL terminated
CONST ProcessListEntry g_protectProcesses[] = {
	{ NULL, PsRuleTypeWithoutInherit }
};

#define CSRSS_PAHT_BUFFER_SIZE 256

UNICODE_STRING g_csrssPath;
WCHAR          g_csrssPathBuffer[CSRSS_PAHT_BUFFER_SIZE];

_Must_inspect_result_
_IRQL_requires_max_(APC_LEVEL)
NTKERNELAPI
NTSTATUS
PsLookupProcessByProcessId(
	_In_ HANDLE ProcessId,
	_Outptr_ PEPROCESS* Process
);

BOOLEAN CheckProtectedOperation(HANDLE Source, HANDLE Destination)
{
	ProcessTableEntry srcInfo, destInfo;
	BOOLEAN result;

	if (Source == Destination)
		return FALSE;

	srcInfo.processId = Source;
	
	ExAcquireFastMutex(&g_processTableLock);
	result = GetProcessInProcessTable(&srcInfo);
	ExReleaseFastMutex(&g_processTableLock);
	
	if (!result)
		return FALSE;

	destInfo.processId = Destination;

	// Spinlock is locked once for both Get\Update process table functions
	// because we want to prevent situations when another thread can change 
	// any state of process beetwen get and update functions on this place
	ExAcquireFastMutex(&g_processTableLock);

	if (!GetProcessInProcessTable(&destInfo))
	{
		ExReleaseFastMutex(&g_processTableLock);
		return FALSE;
	}

	// Not-inited process can open any process (parent, csrss, etc)
	if (!destInfo.inited)
	{
		result = TRUE;

		// Update if source is subsystem and destination isn't inited
		if (srcInfo.subsystem)
		{
			destInfo.inited = TRUE;
			if (!UpdateProcessInProcessTable(&destInfo))
				result = FALSE;
		}

		ExReleaseFastMutex(&g_processTableLock);

		if (!result)
			LogWarning("Warning, can't update initial state for process: %p", destInfo.processId);

		return FALSE;
	}

	ExReleaseFastMutex(&g_processTableLock);

	if (!destInfo.protected)
		return FALSE;

	if (srcInfo.protected)
		return FALSE;

	if (srcInfo.subsystem)
		return FALSE;
	
	return TRUE;
}

OB_PREOP_CALLBACK_STATUS ProcessPreCallback(PVOID RegistrationContext, POB_PRE_OPERATION_INFORMATION OperationInformation)
{
	UNREFERENCED_PARAMETER(RegistrationContext);

	if (!IsDriverEnabled())
		return OB_PREOP_SUCCESS;

	if (OperationInformation->KernelHandle)
		return OB_PREOP_SUCCESS;
	
	LogInfo("Process object operation, destPid:%d, srcTid:%d, oper: %s, space: %s",
		PsGetProcessId(OperationInformation->Object), PsGetCurrentThreadId(),
		(OperationInformation->Operation == OB_OPERATION_HANDLE_CREATE ? "create" : "dup"),
		(OperationInformation->KernelHandle ? "kernel" : "user")
	);

	if (!CheckProtectedOperation(PsGetCurrentProcessId(), PsGetProcessId(OperationInformation->Object)))
	{
		LogInfo("Allow protected process access from %Iu to %Iu", (ULONG_PTR)PsGetCurrentProcessId(), (ULONG_PTR)PsGetProcessId(OperationInformation->Object));
		return OB_PREOP_SUCCESS;
	}

	LogTrace("Disallow protected process access from %Iu to %Iu", (ULONG_PTR)PsGetCurrentProcessId(), (ULONG_PTR)PsGetProcessId(OperationInformation->Object));

	if (OperationInformation->Operation == OB_OPERATION_HANDLE_CREATE)
		OperationInformation->Parameters->CreateHandleInformation.DesiredAccess = (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION);
	else
		OperationInformation->Parameters->DuplicateHandleInformation.DesiredAccess = (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION);

	return OB_PREOP_SUCCESS;
}

OB_PREOP_CALLBACK_STATUS ThreadPreCallback(PVOID RegistrationContext, POB_PRE_OPERATION_INFORMATION OperationInformation)
{
	UNREFERENCED_PARAMETER(RegistrationContext);

	if (!IsDriverEnabled())
		return OB_PREOP_SUCCESS;

	if (OperationInformation->KernelHandle)
		return OB_PREOP_SUCCESS;

	LogInfo("Thread object operation, destPid:%Iu, destTid:%Iu, srcPid:%Iu, oper:%s, space:%s",
		(ULONG_PTR)PsGetThreadProcessId(OperationInformation->Object),
		(ULONG_PTR)PsGetThreadId(OperationInformation->Object),
		(ULONG_PTR)PsGetCurrentProcessId(),
		(OperationInformation->Operation == OB_OPERATION_HANDLE_CREATE ? "create" : "dup"),
		(OperationInformation->KernelHandle ? "kernel" : "user")
	);

	if (!CheckProtectedOperation(PsGetCurrentProcessId(), PsGetThreadProcessId(OperationInformation->Object)))
	{
		LogInfo("Allow protected thread access from %Iu to %Iu", (ULONG_PTR)PsGetCurrentProcessId(), (ULONG_PTR)PsGetThreadProcessId(OperationInformation->Object));
		return OB_PREOP_SUCCESS;
	}

	LogTrace("Disallow protected thread access from %Iu to %Iu", (ULONG_PTR)PsGetCurrentProcessId(), (ULONG_PTR)PsGetThreadProcessId(OperationInformation->Object));

	if (OperationInformation->Operation == OB_OPERATION_HANDLE_CREATE)
		OperationInformation->Parameters->CreateHandleInformation.DesiredAccess = (SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION);
	else
		OperationInformation->Parameters->DuplicateHandleInformation.DesiredAccess = (SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION);

	return OB_PREOP_SUCCESS;
}

//TODO:
// - Find an offset on driver initialization step

BOOLEAN FindActiveProcessLinksOffset(PEPROCESS Process, ULONG* Offset)
{
#ifdef _M_AMD64
	ULONG peak = 0x300;
#else
	ULONG peak = 0x150;
#endif
	HANDLE* ptr = (HANDLE*)Process;
	HANDLE processId = PsGetProcessId(Process);
	ULONG i;

	// EPROCESS ActiveProcessLinks field is next to UniqueProcessId
	//    ...
	//	+ 0x0b4 UniqueProcessId : Ptr32 Void
	//	+ 0x0b8 ActiveProcessLinks : _LIST_ENTRY
	//	+ 0x0c0 Flags2 : Uint4B
	//    ...
	for (i = 10; i < peak / sizeof(HANDLE); i++)
	{
		if (ptr[i] == processId)
		{
			*Offset = sizeof(HANDLE) * (i + 1);
			LogInfo("EPROCESS->ActiveProcessList offset is %x", *Offset);
			return TRUE;
		}
	}

	return FALSE;
}

VOID UnlinkProcessFromList(PLIST_ENTRY Current)
{ // https://github.com/landhb/HideProcess/blob/master/driver/hideprocess.c
	PLIST_ENTRY Previous, Next;

	Previous = (Current->Blink);
	Next = (Current->Flink);

	// Loop over self (connect previous with next)
	Previous->Flink = Next;
	Next->Blink = Previous;

	// Re-write the current LIST_ENTRY to point to itself (avoiding BSOD)
	Current->Blink = (PLIST_ENTRY)&Current->Flink;
	Current->Flink = (PLIST_ENTRY)&Current->Flink;
}

VOID UnlinkProcessFromActiveProcessLinks(PEPROCESS Process)
{
	ULONG eprocListOffset = 0;
	PLIST_ENTRY CurrentList = NULL;

	if (!FindActiveProcessLinksOffset(Process, &eprocListOffset))
	{
		LogError("Error, can't find active process list offset, eprocess:%p", Process);
		return;
	}

	CurrentList = (PLIST_ENTRY)((ULONG_PTR)Process + eprocListOffset);

	// We use g_activeProcListLock to sync our modification inside hidden and raise
	// IRQL to disable special APC's because we want to minimize a BSOD chance because
	// of lack of the ActiveProcessLinks syncronization

	KeAcquireGuardedMutex(&g_activeProcListLock);
	UnlinkProcessFromList(CurrentList);
	KeReleaseGuardedMutex(&g_activeProcListLock);
}

VOID LinkProcessFromList(PLIST_ENTRY Current, PLIST_ENTRY Target)
{
	PLIST_ENTRY Previous, Next;

	if (Current->Blink != Current->Flink)
		return;

	Previous = Target;
	Next = Target->Flink;

	Current->Blink = Previous;
	Current->Flink = Next;

	Previous->Flink = (PLIST_ENTRY)&Current->Flink;
	Next->Blink = (PLIST_ENTRY)&Current->Flink;
}

VOID LinkProcessToActiveProcessLinks(PEPROCESS Process)
{
	ULONG eprocListOffset = 0;
	PLIST_ENTRY CurrentList = NULL, TargetList = NULL;
	PEPROCESS System;
	NTSTATUS status;

	if (!FindActiveProcessLinksOffset(Process, &eprocListOffset))
	{
		LogWarning("Warning, can't find active process list offset, eprocess:%p", Process);
		return;
	}

	status = PsLookupProcessByProcessId(SYSTEM_PROCESS_ID, &System);
	if (!NT_SUCCESS(status))
	{
		LogWarning("Warning, can't find active system process");
		return;
	}

	CurrentList = (PLIST_ENTRY)((ULONG_PTR)Process + eprocListOffset);
	TargetList  = (PLIST_ENTRY)((ULONG_PTR)System + eprocListOffset);

	KeAcquireGuardedMutex(&g_activeProcListLock);
	LinkProcessFromList(CurrentList, TargetList);
	KeReleaseGuardedMutex(&g_activeProcListLock);

	ObDereferenceObject(System);
}

VOID CheckProcessFlags(PProcessTableEntry Entry, PCUNICODE_STRING ImgPath, HANDLE ParentId)
{
	ProcessTableEntry lookup;
	ULONG inheritType;
	BOOLEAN result;

	RtlZeroMemory(&lookup, sizeof(lookup));

	Entry->inited = (!g_psMonitorInited ? TRUE : FALSE);
	if (Entry->processId == SYSTEM_PROCESS_ID)
		Entry->subsystem = TRUE;
	else
		Entry->subsystem = RtlEqualUnicodeString(&g_csrssPath, ImgPath, TRUE);

	// Check exclude flag

	Entry->excluded = FALSE;
	Entry->inheritExclusion = PsRuleTypeWithoutInherit;

	if (FindInheritanceInPsRuleList(g_excludeProcessRules, ImgPath, &inheritType))
	{
		Entry->excluded = TRUE;
		Entry->inheritExclusion = inheritType;
	}
	else if (ParentId != 0)
	{
		lookup.processId = ParentId;

		ExAcquireFastMutex(&g_processTableLock);
		result = GetProcessInProcessTable(&lookup);
		ExReleaseFastMutex(&g_processTableLock);

		if (result)
		{
			if (lookup.inheritExclusion == PsRuleTypeInherit)
			{
				Entry->excluded = TRUE;
				Entry->inheritExclusion = PsRuleTypeInherit;
			}
			else if (lookup.inheritExclusion == PsRuleTypeInheritOnce)
			{
				Entry->excluded = TRUE;
				Entry->inheritExclusion = PsRuleTypeWithoutInherit;
			}
		}
	}

	// Check protected flag

	Entry->protected = FALSE;
	Entry->inheritProtection = PsRuleTypeWithoutInherit;

	if (FindInheritanceInPsRuleList(g_protectProcessRules, ImgPath, &inheritType))
	{
		Entry->protected = TRUE;
		Entry->inheritProtection = inheritType;
	}
	else if (ParentId != 0)
	{
		lookup.processId = ParentId;

		ExAcquireFastMutex(&g_processTableLock);
		result = GetProcessInProcessTable(&lookup);
		ExReleaseFastMutex(&g_processTableLock);

		if (result)
		{
			if (lookup.inheritProtection == PsRuleTypeInherit)
			{
				Entry->protected = TRUE;
				Entry->inheritProtection = PsRuleTypeInherit;
			}
			else if (lookup.inheritProtection == PsRuleTypeInheritOnce)
			{
				Entry->protected = TRUE;
				Entry->inheritProtection = PsRuleTypeWithoutInherit;
			}
		}
	}

	// Check hidden flag

	Entry->hidden = FALSE;
	Entry->inheritStealth = PsRuleTypeWithoutInherit;

	if (FindInheritanceInPsRuleList(g_hideProcessRules, ImgPath, &inheritType))
	{
		Entry->hidden = TRUE;
		Entry->inheritStealth = inheritType;
	}
	else if (ParentId != 0)
	{
		lookup.processId = ParentId;

		ExAcquireFastMutex(&g_processTableLock);
		result = GetProcessInProcessTable(&lookup);
		ExReleaseFastMutex(&g_processTableLock);

		if (result)
		{
			if (lookup.inheritStealth == PsRuleTypeInherit)
			{
				Entry->hidden = TRUE;
				Entry->inheritStealth = PsRuleTypeInherit;
			}
			else if (lookup.inheritStealth == PsRuleTypeInheritOnce)
			{
				Entry->hidden = TRUE;
				Entry->inheritStealth = PsRuleTypeWithoutInherit;
			}
		}
	}

	if (Entry->hidden)
		UnlinkProcessFromActiveProcessLinks(Entry->reference);
}

VOID CreateProcessNotifyCallback(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo)
{
	ProcessTableEntry entry;
	BOOLEAN result;

	UNREFERENCED_PARAMETER(Process);

	if (CreateInfo)
		LogInfo(
			"Created process, pid:%p, srcPid:%p, srcTid:%p, image:%wZ",
			ProcessId, 
			PsGetCurrentProcessId(), 
			PsGetCurrentThreadId(), 
			CreateInfo->ImageFileName
		);
	else
		LogInfo(
			"Destroyed process, pid:%p, srcPid:%p, srcTid:%p",
			ProcessId,
			PsGetCurrentProcessId(),
			PsGetCurrentThreadId()
		);

	RtlZeroMemory(&entry, sizeof(entry));
	entry.processId = ProcessId;
	entry.reference = Process;

	if (CreateInfo)
	{
		const USHORT maxBufSize = CreateInfo->ImageFileName->Length + NORMALIZE_INCREAMENT;
		UNICODE_STRING normalized;
		NTSTATUS status;

		normalized.Buffer = (PWCH)ExAllocatePoolWithTag(PagedPool, maxBufSize, PSMON_ALLOC_TAG);
		normalized.Length = 0;
		normalized.MaximumLength = maxBufSize;

		if (!normalized.Buffer)
		{
			LogWarning("Error, can't allocate buffer");
			return;
		}

		status = NormalizeDevicePath(CreateInfo->ImageFileName, &normalized);
		if (!NT_SUCCESS(status))
		{
			LogWarning("Error, path normalization failed with code:%08x, path:%wZ", status, CreateInfo->ImageFileName);
			ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
			return;
		}

		CheckProcessFlags(&entry, &normalized, PsGetCurrentProcessId());

		if (entry.excluded)
			LogTrace("Excluded process:%p", ProcessId);

		if (entry.protected)
			LogTrace("Protected process:%p", ProcessId);

		if (entry.hidden)
			LogTrace("Hidden process:%p", ProcessId);

		ExAcquireFastMutex(&g_processTableLock);
		result = AddProcessToProcessTable(&entry);
		ExReleaseFastMutex(&g_processTableLock);

		if (!result)
			LogWarning("Warning, can't add process(pid:%p) to process table", ProcessId);

		ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
	}
	else
	{
		ExAcquireFastMutex(&g_processTableLock);
		result = RemoveProcessFromProcessTable(&entry);
		ExReleaseFastMutex(&g_processTableLock);

		if (!result)
			LogWarning("Warning, can't remove process(pid:%p) from process table", ProcessId);
	}
}

BOOLEAN IsProcessExcluded(HANDLE ProcessId)
{
	ProcessTableEntry entry;
	BOOLEAN result;

	// Exclude system process because we don't want affect to kernel-mode threads
	if (ProcessId == SYSTEM_PROCESS_ID)
		return TRUE;

	entry.processId = ProcessId;

	ExAcquireFastMutex(&g_processTableLock);
	result = GetProcessInProcessTable(&entry);
	ExReleaseFastMutex(&g_processTableLock);

	if (!result)
		return FALSE;

	return entry.excluded;
}

BOOLEAN IsProcessProtected(HANDLE ProcessId)
{
	ProcessTableEntry entry;
	BOOLEAN result;

	entry.processId = ProcessId;

	ExAcquireFastMutex(&g_processTableLock);
	result = GetProcessInProcessTable(&entry);
	ExReleaseFastMutex(&g_processTableLock);

	if (!result)
		return FALSE;

	return entry.protected;
}

NTSTATUS ParsePsConfigEntry(PUNICODE_STRING Entry, PUNICODE_STRING Path, PULONG Inherit)
{
	USHORT inx, length = Entry->Length / sizeof(WCHAR);
	LPWSTR str = Entry->Buffer;
	UNICODE_STRING command, template;

	RtlZeroMemory(&command, sizeof(command));

	for (inx = 0; inx < length; inx++)
	{
		if (str[inx] == L';')
		{
			command.Buffer = str + inx + 1;
			command.Length = (length - inx - 1) * sizeof(WCHAR);
			command.MaximumLength = command.Length;
			break;
		}
	}

	if (inx == 0)
		return STATUS_NO_DATA_DETECTED;

	Path->Buffer = Entry->Buffer;
	Path->Length = inx * sizeof(WCHAR);
	Path->MaximumLength = Path->Length;

	RtlInitUnicodeString(&template, L"none");
	if (RtlCompareUnicodeString(&command, &template, TRUE) == 0)
	{
		*Inherit = PsRuleTypeWithoutInherit;
		return STATUS_SUCCESS;
	}

	RtlInitUnicodeString(&template, L"always");
	if (RtlCompareUnicodeString(&command, &template, TRUE) == 0)
	{
		*Inherit = PsRuleTypeInherit;
		return STATUS_SUCCESS;
	}

	RtlInitUnicodeString(&template, L"once");
	if (RtlCompareUnicodeString(&command, &template, TRUE) == 0)
	{
		*Inherit = PsRuleTypeInheritOnce;
		return STATUS_SUCCESS;
	}

	return STATUS_NOT_FOUND;
}

VOID LoadProtectedRulesCallback(PUNICODE_STRING Str, PVOID Params)
{
	UNICODE_STRING path;
	ULONG inherit;
	PsRuleEntryId ruleId;

	UNREFERENCED_PARAMETER(Params);

	if (NT_SUCCESS(ParsePsConfigEntry(Str, &path, &inherit)))
		AddProtectedImage(&path, inherit, FALSE, &ruleId);
}

VOID LoadIgnoredRulesCallback(PUNICODE_STRING Str, PVOID Params)
{
	UNICODE_STRING path;
	ULONG inherit;
	PsRuleEntryId ruleId;

	UNREFERENCED_PARAMETER(Params);

	if (NT_SUCCESS(ParsePsConfigEntry(Str, &path, &inherit)))
		AddExcludedImage(&path, inherit, FALSE, &ruleId);
}

VOID LoadHiddenRulesCallback(PUNICODE_STRING Str, PVOID Params)
{
	UNICODE_STRING path;
	ULONG inherit;
	PsRuleEntryId ruleId;

	UNREFERENCED_PARAMETER(Params);
	
	if (NT_SUCCESS(ParsePsConfigEntry(Str, &path, &inherit)))
		AddHiddenImage(&path, inherit, FALSE, &ruleId);
}

NTSTATUS InitializePsMonitor(PDRIVER_OBJECT DriverObject)
{
	const USHORT maxBufSize = 512;
	NTSTATUS status;
	UNICODE_STRING str, normalized, csrss;
	UINT32 i;
	PsRuleEntryId ruleId;

	UNREFERENCED_PARAMETER(DriverObject);

	// Set csrss path

	RtlZeroMemory(g_csrssPathBuffer, sizeof(g_csrssPathBuffer));
	g_csrssPath.Buffer = g_csrssPathBuffer;
	g_csrssPath.Length = 0;
	g_csrssPath.MaximumLength = sizeof(g_csrssPathBuffer);

	RtlInitUnicodeString(&csrss, L"\\SystemRoot\\System32\\csrss.exe");
	status = NormalizeDevicePath(&csrss, &g_csrssPath);
	if (!NT_SUCCESS(status))
	{
		LogError("Error, subsystem path normalization failed with code:%08x", status);
		return status;
	}

	LogTrace("Subsystem path: %wZ", &g_csrssPath);

	// Init normalization buffer

	normalized.Buffer = (PWCH)ExAllocatePoolWithTag(NonPagedPool, maxBufSize, PSMON_ALLOC_TAG);
	normalized.Length = 0;
	normalized.MaximumLength = maxBufSize;
	if (!normalized.Buffer)
	{
		LogError("Error, allocation failed");
		return STATUS_ACCESS_DENIED;
	}

	// Initialize and fill exclude file\dir lists 

	// exclude

	status = InitializePsRuleListContext(&g_excludeProcessRules);
	if (!NT_SUCCESS(status))
	{
		LogError("Error, excluded process rules initialization failed with code:%08x", status);
		ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
		return status;
	}

	for (i = 0; g_excludeProcesses[i].path; i++)
	{
		RtlInitUnicodeString(&str, g_excludeProcesses[i].path);

		status = NormalizeDevicePath(&str, &normalized);
		LogTrace("Normalized excluded: %wZ", &normalized);
		if (!NT_SUCCESS(status))
		{
			LogWarning("Path normalization failed with code:%08x, path:%wZ", status, &str);
			continue;
		}

		AddRuleToPsRuleList(g_excludeProcessRules, &normalized, g_excludeProcesses[i].inherit, &ruleId);
	}

	// Load entries from the config
	CfgEnumConfigsTable(IgnoreImagesTable, &LoadIgnoredRulesCallback, NULL);

	// protected

	status = InitializePsRuleListContext(&g_protectProcessRules);
	if (!NT_SUCCESS(status))
	{
		LogError("Error, protected process rules initialization failed with code:%08x", status);
		DestroyPsRuleListContext(g_excludeProcessRules);
		ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
		return status;
	}

	for (i = 0; g_protectProcesses[i].path; i++)
	{
		RtlInitUnicodeString(&str, g_protectProcesses[i].path);

		status = NormalizeDevicePath(&str, &normalized);
		LogTrace("Normalized protected %wZ", &normalized);
		if (!NT_SUCCESS(status))
		{
			LogWarning("Path normalization failed with code:%08x, path:%wZ", status, &str);
			continue;
		}

		AddRuleToPsRuleList(g_protectProcessRules, &normalized, g_protectProcesses[i].inherit, &ruleId);
	}

	// Load entries from the config
	CfgEnumConfigsTable(ProtectImagesTable, &LoadProtectedRulesCallback, NULL);

	// hidden

	status = InitializePsRuleListContext(&g_hideProcessRules);
	if (!NT_SUCCESS(status))
	{
		LogError("Error, hidden process rules initialization failed with code:%08x", status);
		DestroyPsRuleListContext(g_excludeProcessRules);
		DestroyPsRuleListContext(g_protectProcessRules);
		ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
		return status;
	}

	// Load entries from the config
	CfgEnumConfigsTable(HideImagesTable, &LoadHiddenRulesCallback, NULL);

	// Process table

	ExInitializeFastMutex(&g_processTableLock);
	KeInitializeGuardedMutex(&g_activeProcListLock);

	status = InitializeProcessTable(&CheckProcessFlags);
	if (!NT_SUCCESS(status))
	{
		DestroyPsRuleListContext(g_excludeProcessRules);
		DestroyPsRuleListContext(g_protectProcessRules);
		DestroyPsRuleListContext(g_hideProcessRules);
		ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
		return status;
	}

	ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);

	g_psMonitorInited = TRUE;

	// Register ps\thr pre create\duplicate object callback

	g_regOperation[0].ObjectType = PsProcessType;
	g_regOperation[0].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
	g_regOperation[0].PreOperation = ProcessPreCallback;
	g_regOperation[0].PostOperation = NULL;

	g_regOperation[1].ObjectType = PsThreadType;
	g_regOperation[1].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
	g_regOperation[1].PreOperation = ThreadPreCallback;
	g_regOperation[1].PostOperation = NULL;

	g_regCallback.Version = OB_FLT_REGISTRATION_VERSION;
	g_regCallback.OperationRegistrationCount = 2;
	g_regCallback.RegistrationContext = NULL;
	g_regCallback.OperationRegistration = g_regOperation;
	RtlInitUnicodeString(&g_regCallback.Altitude, L"1000");

	status = ObRegisterCallbacks(&g_regCallback, &g_obRegCallback);
	if (!NT_SUCCESS(status))
	{
		LogError("Error, object filter registration failed with code:%08x", status);
		DestroyPsMonitor();
		return status;
	}

	// Register rocess create\destroy callback

	status = PsSetCreateProcessNotifyRoutineEx(&CreateProcessNotifyCallback, FALSE);
	if (!NT_SUCCESS(status))
	{
		LogError("Error, process notify registartion failed with code:%08x", status);
		DestroyPsMonitor();
		return status;
	}

	LogTrace("Initialization is completed");
	return status;
}

VOID CleanupHiddenProcessCallback(PProcessTableEntry entry)
{
	if (!entry->hidden)
		return;

	LinkProcessToActiveProcessLinks(entry->reference);

	entry->hidden = FALSE;
}

NTSTATUS DestroyPsMonitor()
{
	if (!g_psMonitorInited)
		return STATUS_ALREADY_DISCONNECTED;

	if (g_obRegCallback)
	{
		ObUnRegisterCallbacks(g_obRegCallback);
		g_obRegCallback = NULL;
	}

	PsSetCreateProcessNotifyRoutineEx(&CreateProcessNotifyCallback, TRUE);

	DestroyPsRuleListContext(g_excludeProcessRules);
	DestroyPsRuleListContext(g_protectProcessRules);
	DestroyPsRuleListContext(g_hideProcessRules);

	ExAcquireFastMutex(&g_processTableLock);
	ClearProcessTable(&CleanupHiddenProcessCallback);
	ExReleaseFastMutex(&g_processTableLock);

	g_psMonitorInited = FALSE;

	LogTrace("Deinitialization is completed");
	return STATUS_SUCCESS;
}

NTSTATUS SetStateForProcessesByImage(PCUNICODE_STRING ImagePath, BOOLEAN Excluded, BOOLEAN Protected, BOOLEAN Hidden)
{
	PSYSTEM_PROCESS_INFORMATION processInfo = NULL, first;
	SIZE_T size = 0, offset;
	NTSTATUS status;

	status = QuerySystemInformation(SystemProcessInformation, &processInfo, &size);
	if (!NT_SUCCESS(status))
	{
		LogWarning("Query system information(pslist) failed with code:%08x", status);
		return status;
	}

	offset = 0;
	first = processInfo;
	do
	{
		HANDLE hProcess;
		CLIENT_ID clientId;
		OBJECT_ATTRIBUTES attribs;
		PUNICODE_STRING procName;
		ProcessTableEntry entry;

		processInfo = (PSYSTEM_PROCESS_INFORMATION)((SIZE_T)processInfo + offset);

		if (processInfo->ProcessId == 0)
		{
			offset = processInfo->NextEntryOffset;
			continue;
		}

		InitializeObjectAttributes(&attribs, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
		clientId.UniqueProcess = processInfo->ProcessId;
		clientId.UniqueThread = 0;

		status = ZwOpenProcess(&hProcess, 0x1000/*PROCESS_QUERY_LIMITED_INFORMATION*/, &attribs, &clientId);
		if (!NT_SUCCESS(status))
		{
			LogWarning("Can't open process (pid:%p) failed with code:%08x", processInfo->ProcessId, status);
			offset = processInfo->NextEntryOffset;
			continue;
		}

		status = QueryProcessInformation(ProcessImageFileName, hProcess, &procName, &size);
		ZwClose(hProcess);

		if (!NT_SUCCESS(status))
		{
			LogWarning("Query process information(pid:%p) failed with code:%08x", processInfo->ProcessId, status);
			offset = processInfo->NextEntryOffset;
			continue;
		}

		entry.processId = processInfo->ProcessId;

		if (RtlCompareUnicodeString(procName, ImagePath, TRUE) == 0)
		{
			BOOLEAN result = TRUE;

			// Spinlock is locked once for both Get\Update process table functions
			// because we want to prevent situations when another thread can change 
			// any state of process beetwen get and update functions on this place
			ExAcquireFastMutex(&g_processTableLock);
			
			if (GetProcessInProcessTable(&entry))
			{
				if (Excluded)
				{
					entry.excluded = TRUE;
					entry.inheritExclusion = PsRuleTypeWithoutInherit;
				}

				if (Protected)
				{
					entry.protected = TRUE;
					entry.inheritProtection = PsRuleTypeWithoutInherit;
				}

				if (Hidden)
				{
					if (!entry.hidden)
						UnlinkProcessFromActiveProcessLinks(entry.reference);

					entry.hidden = TRUE;
					entry.inheritStealth = PsRuleTypeWithoutInherit;
				}

				if (!UpdateProcessInProcessTable(&entry))
					result = FALSE;
			}

			ExReleaseFastMutex(&g_processTableLock);

			if (!result)
				LogWarning("Can't update process %p", processInfo->ProcessId);
		}

		FreeInformation(procName);
		offset = processInfo->NextEntryOffset;
	} while (offset);

	FreeInformation(first);
	return STATUS_SUCCESS;
}

NTSTATUS AddProtectedImage(PUNICODE_STRING ImagePath, ULONG InheritType, BOOLEAN ApplyForProcesses, PULONGLONG ObjId)
{
	const USHORT maxBufSize = ImagePath->Length + NORMALIZE_INCREAMENT;
	UNICODE_STRING normalized;
	NTSTATUS status;

	normalized.Buffer = (PWCH)ExAllocatePoolWithTag(PagedPool, maxBufSize, PSMON_ALLOC_TAG);
	normalized.Length = 0;
	normalized.MaximumLength = maxBufSize;

	if (!normalized.Buffer)
	{
		LogWarning("Error, can't allocate buffer");
		return STATUS_MEMORY_NOT_ALLOCATED;
	}

	status = NormalizeDevicePath(ImagePath, &normalized);
	if (!NT_SUCCESS(status))
	{
		LogWarning("Error, path normalization failed with code:%08x, path:%wZ", status, ImagePath);
		ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
		return status;
	}

	LogTrace("Adding protect image: %wZ", &normalized);
	status = AddRuleToPsRuleList(g_protectProcessRules, &normalized, InheritType, ObjId);

	if (ApplyForProcesses)
		SetStateForProcessesByImage(&normalized, FALSE, TRUE, FALSE);

	ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);

	return status;
}

NTSTATUS GetProtectedProcessState(HANDLE ProcessId, PULONG InheritType, PBOOLEAN Enable)
{
	ProcessTableEntry entry;
	BOOLEAN result;

	entry.processId = ProcessId;

	ExAcquireFastMutex(&g_processTableLock);
	result = GetProcessInProcessTable(&entry);
	ExReleaseFastMutex(&g_processTableLock);

	if (!result)
		return STATUS_NOT_FOUND;

	*Enable = entry.protected;
	*InheritType = entry.inheritProtection;

	return STATUS_SUCCESS;
}

NTSTATUS SetProtectedProcessState(HANDLE ProcessId, ULONG InheritType, BOOLEAN Enable)
{
	NTSTATUS status = STATUS_SUCCESS;
	ProcessTableEntry entry;
	BOOLEAN result;

	entry.processId = ProcessId;

	ExAcquireFastMutex(&g_processTableLock);

	if (!GetProcessInProcessTable(&entry))
	{
		ExReleaseFastMutex(&g_processTableLock);
		return STATUS_NOT_FOUND;
	}

	if (Enable)
	{
		entry.protected = TRUE;
		entry.inheritProtection = InheritType;
	}
	else
	{
		entry.protected = FALSE;
	}

	result = UpdateProcessInProcessTable(&entry);
	
	ExReleaseFastMutex(&g_processTableLock);

	if (!result)
		return STATUS_NOT_FOUND;

	return status;
}

NTSTATUS RemoveProtectedImage(ULONGLONG ObjId)
{
	return RemoveRuleFromPsRuleList(g_protectProcessRules, ObjId);
}

NTSTATUS RemoveAllProtectedImages()
{
	return RemoveAllRulesFromPsRuleList(g_protectProcessRules);
}

NTSTATUS AddExcludedImage(PUNICODE_STRING ImagePath, ULONG InheritType, BOOLEAN ApplyForProcesses, PULONGLONG ObjId)
{
	const USHORT maxBufSize = ImagePath->Length + NORMALIZE_INCREAMENT;
	UNICODE_STRING normalized;
	NTSTATUS status;

	normalized.Buffer = (PWCH)ExAllocatePoolWithTag(PagedPool, maxBufSize, PSMON_ALLOC_TAG);
	normalized.Length = 0;
	normalized.MaximumLength = maxBufSize;

	if (!normalized.Buffer)
	{
		LogWarning("Error, can't allocate buffer");
		return STATUS_MEMORY_NOT_ALLOCATED;
	}

	status = NormalizeDevicePath(ImagePath, &normalized);
	if (!NT_SUCCESS(status))
	{
		LogWarning("Error, path normalization failed with code:%08x, path:%wZ", status, ImagePath);
		ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
		return status;
	}

	LogTrace("Adding exclude image: %wZ", &normalized);
	status = AddRuleToPsRuleList(g_excludeProcessRules, &normalized, InheritType, ObjId);

	if (ApplyForProcesses)
		SetStateForProcessesByImage(&normalized, TRUE, FALSE, FALSE);

	ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);

	return status;
}

NTSTATUS GetExcludedProcessState(HANDLE ProcessId, PULONG InheritType, PBOOLEAN Enable)
{
	ProcessTableEntry entry;
	BOOLEAN result;

	entry.processId = ProcessId;

	ExAcquireFastMutex(&g_processTableLock);
	result = GetProcessInProcessTable(&entry);
	ExReleaseFastMutex(&g_processTableLock);

	if (!result)
		return STATUS_NOT_FOUND;

	*Enable = entry.excluded;
	*InheritType = entry.inheritExclusion;

	return STATUS_SUCCESS;
}

NTSTATUS SetExcludedProcessState(HANDLE ProcessId, ULONG InheritType, BOOLEAN Enable)
{
	NTSTATUS status = STATUS_SUCCESS;
	ProcessTableEntry entry;
	BOOLEAN result;

	entry.processId = ProcessId;

	ExAcquireFastMutex(&g_processTableLock);
	
	if (!GetProcessInProcessTable(&entry))
	{
		ExReleaseFastMutex(&g_processTableLock);
		return STATUS_NOT_FOUND;
	}

	if (Enable)
	{
		entry.excluded = TRUE;
		entry.inheritExclusion = InheritType;
	}
	else
	{
		entry.excluded = FALSE;
	}

	result = UpdateProcessInProcessTable(&entry);
	
	ExReleaseFastMutex(&g_processTableLock);

	if (!result)
		return STATUS_NOT_FOUND;

	return status;
}

NTSTATUS RemoveExcludedImage(ULONGLONG ObjId)
{
	return RemoveRuleFromPsRuleList(g_excludeProcessRules, ObjId);
}

NTSTATUS RemoveAllExcludedImages()
{
	return RemoveAllRulesFromPsRuleList(g_excludeProcessRules);
}

NTSTATUS AddHiddenImage(PUNICODE_STRING ImagePath, ULONG InheritType, BOOLEAN ApplyForProcesses, PULONGLONG ObjId)
{
	const USHORT maxBufSize = ImagePath->Length + NORMALIZE_INCREAMENT;
	UNICODE_STRING normalized;
	NTSTATUS status;

	normalized.Buffer = (PWCH)ExAllocatePoolWithTag(PagedPool, maxBufSize, PSMON_ALLOC_TAG);
	normalized.Length = 0;
	normalized.MaximumLength = maxBufSize;

	if (!normalized.Buffer)
	{
		LogWarning("Error, can't allocate buffer");
		return STATUS_MEMORY_NOT_ALLOCATED;
	}

	status = NormalizeDevicePath(ImagePath, &normalized);
	if (!NT_SUCCESS(status))
	{
		LogWarning("Error, path normalization failed with code:%08x, path:%wZ", status, ImagePath);
		ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);
		return status;
	}

	LogTrace("Adding hidden image: %wZ", &normalized);
	status = AddRuleToPsRuleList(g_hideProcessRules, &normalized, InheritType, ObjId);

	//TODO: 
	if (ApplyForProcesses)
		SetStateForProcessesByImage(&normalized, FALSE, FALSE, TRUE);

	ExFreePoolWithTag(normalized.Buffer, PSMON_ALLOC_TAG);

	return status;
}

NTSTATUS GetHiddenProcessState(HANDLE ProcessId, PULONG InheritType, PBOOLEAN Enable)
{
	ProcessTableEntry entry;
	BOOLEAN result;

	entry.processId = ProcessId;

	ExAcquireFastMutex(&g_processTableLock);
	result = GetProcessInProcessTable(&entry);
	ExReleaseFastMutex(&g_processTableLock);

	if (!result)
		return STATUS_NOT_FOUND;

	*Enable = entry.hidden;
	*InheritType = entry.inheritStealth;

	return STATUS_SUCCESS;
}

NTSTATUS SetHiddenProcessState(HANDLE ProcessId, ULONG InheritType, BOOLEAN Enable) 
{
	ProcessTableEntry entry;
	BOOLEAN result;

	if (!ProcessId || ProcessId == SYSTEM_PROCESS_ID)
		return STATUS_NOT_SUPPORTED;

	entry.processId = ProcessId;

	ExAcquireFastMutex(&g_processTableLock);
	
	if (!GetProcessInProcessTable(&entry))
	{
		ExReleaseFastMutex(&g_processTableLock);
		return STATUS_NOT_FOUND;
	}

	if (Enable)
	{
		if (!entry.hidden)
			UnlinkProcessFromActiveProcessLinks(entry.reference);

		entry.hidden = TRUE;
		entry.inheritStealth = InheritType;
	}
	else
	{
		if (!entry.hidden)
		{
			ExReleaseFastMutex(&g_processTableLock);
			return STATUS_NOT_CAPABLE;
		}

		LinkProcessToActiveProcessLinks(entry.reference);

		entry.hidden = FALSE;
		entry.inheritStealth = 0;
	}

	result = UpdateProcessInProcessTable(&entry);
	
	ExReleaseFastMutex(&g_processTableLock);

	if (!result)
		return STATUS_NOT_FOUND;

	return STATUS_SUCCESS;
}

NTSTATUS RemoveHiddenImage(ULONGLONG ObjId)
{
	return RemoveRuleFromPsRuleList(g_hideProcessRules, ObjId);
}

NTSTATUS RemoveAllHiddenImages()
{
	return RemoveAllRulesFromPsRuleList(g_hideProcessRules);
}

NTSTATUS RemoveAllHiddenProcesses()
{
	ExAcquireFastMutex(&g_processTableLock);
	EnumProcessTable(&CleanupHiddenProcessCallback);
	ExReleaseFastMutex(&g_processTableLock);
	return STATUS_SUCCESS;
}