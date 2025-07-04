#include <ntddk.h>
#include <dontuse.h>
#include <intrin.h>
#include<ntifs.h>
#define MAX_THREAD_MIGRATIONS 10 //maximum of 10 thread migrations per call of RunHeatsinkLogic.
#define LOW_PRIORITY_THREASHOLD 10 //0-31 scale, 0 lowest. 31 = realtime.

typedef PEPROCESS(*t_PsGetNextProcess)(PEPROCESS Process);
typedef PETHREAD(*t_PsGetNextProcessThread)(PEPROCESS Process, PETHREAD Thread);

t_PsGetNextProcess PsGetNextProcess = NULL;
t_PsGetNextProcessThread PsGetNextProcessThread = NULL;


ULONG ReadCoreTemperature(ULONG coreIndex);
void HeatsinkVoidRoutine(PDEVICE_OBJECT DeviceObject, PVOID Context);


NTSTATUS MigrateThreadsFromCore(ULONG Core);
NTSTATUS CreateCloseHeatsink(PDEVICE_OBJECT DeviceObject, PIRP irp);
void HeatsinkUnload(PDRIVER_OBJECT DriverObject);
NTSTATUS RunHeatsinkLogic(PVOID Context);

PHEATSINK_CONTEXT g_Context = NULL; //global context variable. will be used everywhere.
ULONG g_NumCores = 0; //global context variable for #of cores in the User's CPU.
typedef struct _HEATSINK_CONTEXT {
	BOOLEAN StopFlag;
	PIO_WORKITEM workItem; //Passes the work item so I can free it in ProperCleaning.
	KEVENT UnloadEvent;
	
} HEATSINK_CONTEXT, * PHEATSINK_CONTEXT;

/*
driver entry function. I know, its ugly, theres a whole mem aloc function here. it sucks.
*/
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING registrypath) {
	UNREFERENCED_PARAMETER(registrypath);
	PDEVICE_OBJECT DeviceObject = NULL; //initializes deviceobject variable.
	// we need a deviceobject to queue the work item. very unfortunate, but it is what it is.
	DriverObject->MajorFunction[IRP_MJ_CREATE] = CreateCloseHeatsink; //while this is unnecessary for my algorithm, i declare IRP_MJ_CLOSE and IRP_MJ_CREATE to be able to open handles easily,
	//through windbg or if another process does so by accident. its an edge case (the second example), but it's required for safety.
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = CreateCloseHeatsink;
	DriverObject->DriverUnload = HeatsinkUnload;
	NTSTATUS status = IoCreateDevice(DriverObject, 0, NULL, NULL, 0, TRUE, &DeviceObject);
	//this IoCreateDevice function really came in clutch. I made it TRUE to the exclusive part, so that I can declare that only 1 DeviceObject is allowed to run at a time.
	//this is better, and a LOT less overhead than using mutex. :)

	if (!NT_SUCCESS(status)) {//checks if the deviceobject was properly created.
		KdPrint(("Failed to create device Object (0x%08X)\n", status));
		return status;
	}
	DriverObject->DeviceObject = DeviceObject;
	PHEATSINK_CONTEXT context = (PHEATSINK_CONTEXT)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(HEATSINK_CONTEXT), 'STRT');//allocates mem for my context routine. STRT stands for STARTUP. this will be freed.
	//context will be passed as the PVOID Context of the WorkItem. thi sis necessary, as it contains every critical piece of information.
	if (!context) {
		KdPrint(("Not enough resources to allocate HEATSINK_CONTEXT.\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PIO_WORKITEM workItem = IoAllocateWorkItem(DeviceObject);//allocates work item, so that we can call our function on repeat if the work item isn't null. if it is, we just simply not run the code.
	if (!workItem) {
		ExFreePoolWithTag(context, 'STRT');
		KdPrint(("Failed to allocate work item.\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	context->StopFlag = FALSE;
	context->workItem = workItem;

	g_NumCores = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);// sets the # of cores in the CPU.
	
	KeInitializeEvent(&context->UnloadEvent, NotificationEvent, FALSE);
	g_Context = context;//sets context to global variable, so i can always access it. i know, it's dirty. but whatever.
	
	IoQueueWorkItem(workItem, HeatsinkVoidRoutine, DelayedWorkQueue, (PVOID)context);
	UNICODE_STRING name;
	RtlInitUnicodeString(&name, L"PsGetNextProcess");
	PsGetNextProcess = (t_PsGetNextProcess)MmGetSystemRoutineAddress(&name);
	RtlInitUnicodeString(&name, L"PsGetNextProcessThread");
	PsGetNextProcessThread = (t_PsGetNextProcessThread)MmGetSystemRoutineAddress(&name);

	if (!PsGetNextProcess || !PsGetNextProcessThread) {
		KdPrint(("Failed to resolve PsGetNextProcess or PsGetNextProcessThread\n"));
		return STATUS_UNSUCCESSFUL;
	}

	return STATUS_SUCCESS;
}
ULONG ReadCoreTemperature(ULONG coreIndex) //will check for bit31 later; it determines if the reading is valid or not.
{
	KAFFINITY oldAffinity = KeSetSystemAffinityThreadEx((KAFFINITY)1 << coreIndex); // (KAFFINITY)1 << core creates a bitmask with only the 'core'-th bit set, selecting that CPU core.
	//0001 = 1st core, 0010 = 2nd core, 0100 = 3rd core, 1000 = 4th core, etc.
	ULONG64 msrval = __readmsr(0x19C); //ULONG64 because __readmsr is 64 bits. 
	if (msrval & (1ULL << 31)) {
		KeRevertToUserAffinityThreadEx(oldAffinity);
		return (msrval >> 16) & 0x7F;//moves the temperature reading to the first 6 bits. this makes it easier to read. Maximum delta is 127*C, or 0x7F. 
		//Before: xxxx xxxx DDD DDDD xxxx xxxx xxxx xxxx (where D = delta bits)
	    //After:  0000 0000 0000 0000 xxxx xxxx DDD DDDD
	}
	KeRevertToUserAffinityThreadEx(oldAffinity);
	return(ULONG)-1;// if invalid reading, return ULONG-1. this'll be our "error check" in the loop. 
	
}
/*
* this function is like a screener.it checks if a certain flag(passed in context) is raised. if it is, it calls HeatsinkUnload.
* otherwise, it just keeps calling back RunHeatsinkLogic.
*
*
*/
void HeatsinkVoidRoutine(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
	PHEATSINK_CONTEXT contextheatsink = (PHEATSINK_CONTEXT)Context;
	//no need to set contextheatsink's flag to g_context's flag, because they're both pointers to the same val. if g_context changes the val of stopflag,
	// then all pointers to said struct (which is allocated in the mempool) will recieve that "update"
	LARGE_INTEGER delay;//sets the delay of execution before requeuing another workitem.
	delay.QuadPart = -10 * 1000 * 1000; //1 second.

	if (contextheatsink->StopFlag == TRUE) {
		KeSetEvent(&contextheatsink->UnloadEvent, IO_NO_INCREMENT, FALSE);//sets event. waits for the work item to finish.
		IoFreeWorkItem(contextheatsink->workItem);//frees the work item.

		ExFreePoolWithTag(contextheatsink, 'STRT');//frees the allocated mempool.

		return;
	}
	NTSTATUS status = RunHeatsinkLogic(Context);
	if (NT_SUCCESS(status)) { //if the thread allocations worked, you... just relax for a second.
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
	}
	//if the thread allocation did OR did not work, we requeue the work item.
	IoQueueWorkItem(contextheatsink->workItem, HeatsinkVoidRoutine, DelayedWorkQueue, Context);
}
/*
helper function to read MSR (Model Specific Register. contains direct sensor information, like temp, cycle, etc.). Returns delta temperature from TjMax (maximum temp).
*/

NTSTATUS MigrateThreadsFromCore(ULONG HotCore)
{
	//Step1: Find coolest core. 
	ULONG coolestCore = 0;
	ULONG maxDelta = 0;//we set this to 0 because it's an unsigned LONG. 0 = 000000000 (didnt count how many 0s there were, so dont cry about it). 
	//this maxdelta is impossible to reach, as the moment a CPU core reaches that temp, the OS shuts down to ensure no damage is done to the CPU.
	KAFFINITY oldAffinity;
	for (ULONG core = 0; core < g_NumCores; core++) {//finds coldest core. we will migrate "low" threads to this core.
		ULONG delta = ReadCoreTemperature(core);
		if (delta > maxDelta) {
			maxDelta = delta;
			coolestCore = core;
		}
	}
	if (maxDelta == 0) {
		return STATUS_UNSUCCESSFUL;
	}
	KAFFINITY coolestAffinity = ((KAFFINITY)1 << coolestCore);

	ULONG migrations = 0;//sets current thread migrations. for our case, we won't go past 3; this limits the loop.
	for (PEPROCESS proc = PsGetNextProcess(NULL); proc && migrations < MAX_THREAD_MIGRATIONS; proc = PsGetNextProcess(proc)) {
		for (PETHREAD thread = PsGetNextProcessThread(proc, NULL); thread && migrations < MAX_THREAD_MIGRATIONS; thread = PsGetNextProcessThread(proc, thread)) {
			//ObReferenceObject(thread);This is unecessary. The API takes ETHREAD* directly, and it won't race-free that object for me. 
			// if someone exits the process mid-migration, i'd be dereferencing a freed object.
			//it's better to skip reference counting entirely and accept that tiny risk, on a driver that's already risky in nature. 
			KPRIORITY prio = KeQueryPriorityThread((PKTHREAD)thread);//fetches the prio of the thread we're on.
			if (KeGetCurrentProcessorNumberEx(NULL) != HotCore) continue; //if the thread isnt on 
			if (proc == (PEPROCESS)PsInitialSystemProcess) continue;//if it's a system critical thread, skip. 
			//while the (PEPROCESS) typecast may seem redundant, it's necessary; i'm using ntifs and ntddh; both are required. unfortunately, one handles PEPROCESS with _EPROCESS*
			//and the other with _KPROCESS*; they're both the same, similar to PETHREAD being the same as PKTHREAD.
			if (prio <= LOW_PRIORITY_THREASHOLD) {//if said thread's priority is lower than "LOW PRIORITY THRESHOLD", which is 10;
				KeSetIdealProcessorThread(thread, coolestCore);
				migrations++;
				KdPrint(("Migrated thread %p to core %d\n", thread, coolestCore));
			}
			//ObDereferenceObject(thread); Unecessary. check comment for ObReferenceObject.

		}
	}
	return migrations ? STATUS_SUCCESS : STATUS_NOT_FOUND; //returns status success if migrations = 10; otherwise, status not found.
	
	
}

NTSTATUS CreateCloseHeatsink(PDEVICE_OBJECT DeviceObject, PIRP irp)
{
	irp->IoStatus.Status = STATUS_SUCCESS;
	irp->IoStatus.Information = 0;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}
/*
Unloads the driver. Waits until the current workitem (if there is one) is completed, then frees it.
*/
void HeatsinkUnload(PDRIVER_OBJECT DriverObject)
{
	if (g_Context) {
		// Request work item shutdown
		if (g_Context->workItem) {
			g_Context->StopFlag = TRUE;
			KeWaitForSingleObject(&g_Context->UnloadEvent, Executive, KernelMode, FALSE, NULL);
		}
		
		
		// Free context structure
		ExFreePoolWithTag(g_Context, 'STRT');
		g_Context = NULL;
	}

	//Deletes the deviceObject. 
	PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
	if (deviceObject) {
		IoDeleteDevice(deviceObject);
	}
}
/*
This is where the magic happens. Currently, only supports intel cores. Read README for extra information when AMD support will come.
*/
NTSTATUS RunHeatsinkLogic(PVOID Context)
{

	PHEATSINK_CONTEXT heatsinkctx = (PHEATSINK_CONTEXT)Context;
	for (ULONG core = 0; core < g_NumCores; core++) {//fetches all delta Temps of all cores, by pinning to each core, and reading the MSR of that core.
		ULONG delta = ReadCoreTemperature(core); //reads core temperature, and returns it in "delta". delta is smaller the closer it is to MAX_TEMP TJMax.

		//check for critical temperature.
		if (delta != (ULONG)-1 && delta <= 15) {
			//migrate low priority thread(s), up to 3. 
			NTSTATUS status = MigrateThreadsFromCore(core);
			if (!NT_SUCCESS(status)) {

			}
			return status;
		}
	}
		
}