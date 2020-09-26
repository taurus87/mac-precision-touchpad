#include "driver.h"
#include "Input.tmh"

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtPtpSpiInputRoutineWorker(
	WDFDEVICE Device
)
{
	NTSTATUS Status;
	PDEVICE_CONTEXT pDeviceContext;
	WDF_OBJECT_ATTRIBUTES Attributes;
	BOOLEAN RequestStatus;
	WDFREQUEST SpiHidReadRequest;
	WDFMEMORY SpiHidReadOutputMemory;

	PAGED_CODE();

	pDeviceContext = DeviceGetContext(Device);
	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
	Attributes.ParentObject = pDeviceContext->SpiDevice;

	Status = WdfRequestCreate(
		&Attributes,
		pDeviceContext->SpiTrackpadIoTarget,
		&SpiHidReadRequest
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestCreate fails, status = %!STATUS!",
			Status
		);

		KdPrintEx((
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_INFO_LEVEL,
			"WdfRequestCreate fails, status = 0x%x \n",
			Status
			));

		pDeviceContext->DelayedRequest = TRUE;
		return;
	}

	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
	Attributes.ParentObject = SpiHidReadRequest;

	Status = WdfMemoryCreate(
		&Attributes,
		NonPagedPoolNx,
		PTP_POOL_TAG,
		REPORT_BUFFER_SIZE,
		&SpiHidReadOutputMemory,
		NULL
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! WdfMemoryCreate fails, status = %!STATUS!",
			Status
		);

		KdPrintEx((
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_INFO_LEVEL,
			"WdfMemoryCreate fails, status = 0x%x \n",
			Status
			));

		pDeviceContext->DelayedRequest = TRUE;
		return;
	}

	// Invoke HID read request to the device.
	Status = WdfIoTargetFormatRequestForInternalIoctl(
		pDeviceContext->SpiTrackpadIoTarget,
		SpiHidReadRequest,
		IOCTL_HID_READ_REPORT,
		SpiHidReadOutputMemory,
		0,
		SpiHidReadOutputMemory,
		0
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! WdfIoTargetFormatRequestForInternalIoctl fails, status = %!STATUS!",
			Status
		);

		KdPrintEx((
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_INFO_LEVEL,
			"WdfIoTargetFormatRequestForInternalIoctl fails, status = 0x%x \n",
			Status
		));

		pDeviceContext->DelayedRequest = TRUE;
		return;
	}

	WdfRequestSetCompletionRoutine(
		SpiHidReadRequest,
		AmtPtpRequestCompletionRoutine,
		pDeviceContext
	);

	RequestStatus = WdfRequestSend(
		SpiHidReadRequest,
		pDeviceContext->SpiTrackpadIoTarget,
		NULL
	);

	if (!RequestStatus)
	{
		KdPrintEx((
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_INFO_LEVEL,
			"AmtPtpSpiInputRoutineWorker request not sent! \n"
		));
	}

	pDeviceContext->PendingRequest = RequestStatus;
	pDeviceContext->DelayedRequest = !RequestStatus;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtPtpRequestCompletionRoutine(
	WDFREQUEST SpiRequest,
	WDFIOTARGET Target,
	PWDF_REQUEST_COMPLETION_PARAMS Params,
	WDFCONTEXT Context
)
{
	NTSTATUS Status;
	PDEVICE_CONTEXT pDeviceContext;

	LONG SpiRequestLength;
	PSPI_TRACKPAD_PACKET pSpiTrackpadPacket;

	WDFREQUEST PtpRequest;
	PTP_REPORT PtpReport;
	WDFMEMORY PtpRequestMemory;

	LARGE_INTEGER CurrentCounter;
	LONGLONG CounterDelta;
	BOOLEAN SessionEnded = TRUE;

	PAGED_CODE();
	UNREFERENCED_PARAMETER(Target);

	// Get context
	pDeviceContext = (PDEVICE_CONTEXT) Context;

	// Wait
	KeWaitForSingleObject(
		&pDeviceContext->PtpRequestRoutineEvent,
		Executive,
		KernelMode,
		FALSE,
		NULL
	);

	// Clear event
	KeClearEvent(
		&pDeviceContext->PtpRequestRoutineEvent
	);

	// Read report and fulfill PTP request (if have)
	// Not needed to re-format the WDF request
	// timer worker will do that

	Status = WdfIoQueueRetrieveNextRequest(
		pDeviceContext->HidIoQueue,
		&PtpRequest
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! No pending PTP request. Routine exit"
		);

		KdPrintEx((
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_INFO_LEVEL,
			"No pending PTP request. Routine exit \n"
		));

		pDeviceContext->DelayedRequest = TRUE;
		goto set_event;
	}

	SpiRequestLength = (LONG) WdfRequestGetInformation(SpiRequest);
	pSpiTrackpadPacket = (PSPI_TRACKPAD_PACKET) WdfMemoryGetBuffer(Params->Parameters.Ioctl.Output.Buffer, NULL);

	// Get Counter
	KeQueryPerformanceCounter(
		&CurrentCounter
	);

	CounterDelta = (CurrentCounter.QuadPart - pDeviceContext->LastReportTime.QuadPart) / 100;
	pDeviceContext->LastReportTime.QuadPart = CurrentCounter.QuadPart;

	// Write report
	PtpReport.ReportID = REPORTID_MULTITOUCH;
	PtpReport.ContactCount = pSpiTrackpadPacket->NumOfFingers;
	PtpReport.IsButtonClicked = pSpiTrackpadPacket->ClickOccurred;

	UINT8 AdjustedCount = (pSpiTrackpadPacket->NumOfFingers > 5) ? 5 : pSpiTrackpadPacket->NumOfFingers;
	for (UINT8 Count = 0; Count < AdjustedCount; Count++)
	{
		PtpReport.Contacts[Count].ContactID = Count;
		PtpReport.Contacts[Count].X = ((pSpiTrackpadPacket->Fingers[Count].X - pDeviceContext->TrackpadInfo.XMin) > 0) ? (USHORT)(pSpiTrackpadPacket->Fingers[Count].X - pDeviceContext->TrackpadInfo.XMin) : 0;
		PtpReport.Contacts[Count].Y = ((pDeviceContext->TrackpadInfo.YMax - pSpiTrackpadPacket->Fingers[Count].Y) > 0) ? (USHORT)(pDeviceContext->TrackpadInfo.YMax - pSpiTrackpadPacket->Fingers[Count].Y) : 0;
		PtpReport.Contacts[Count].TipSwitch = (pSpiTrackpadPacket->Fingers[Count].Pressure > 0) ? 1 : 0;

		// $S = \pi * (Touch_{Major} * Touch_{Minor}) / 4$
		// $S = \pi * r^2$
		// $r^2 = (Touch_{Major} * Touch_{Minor}) / 4$
		// Using i386 in 2018 is evil
		PtpReport.Contacts[Count].Confidence = (pSpiTrackpadPacket->Fingers[Count].TouchMajor < 2500 &&
			pSpiTrackpadPacket->Fingers[Count].TouchMinor < 2500) ? 1 : 0;

		if (!SessionEnded && PtpReport.Contacts[Count].TipSwitch) SessionEnded = FALSE;

		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_HID_INPUT,
			"%!FUNC! PTP Contact %d OX %d, OY %d, X %d, Y %d",
			Count,
			pSpiTrackpadPacket->Fingers[Count].OriginalX,
			pSpiTrackpadPacket->Fingers[Count].OriginalY,
			pSpiTrackpadPacket->Fingers[Count].X,
			pSpiTrackpadPacket->Fingers[Count].Y
		);
	}

	if (SessionEnded)
	{
		for (UINT8 i = 0; i < MAPPING_MAX; i++)
		{
			pDeviceContext->PtpMapping[i].ContactID = -1;
			pDeviceContext->PtpMapping[i].OriginalX = -1;
			pDeviceContext->PtpMapping[i].OriginalY = -1;
		}
	}

	if (CounterDelta >= 0xFF)
	{
		PtpReport.ScanTime = 0xFF;
	}
	else
	{
		PtpReport.ScanTime = (USHORT) CounterDelta;
	}

	// Done with the prev memory
	WdfObjectDelete(SpiRequest);

	Status = WdfRequestRetrieveOutputMemory(
		PtpRequest,
		&PtpRequestMemory
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestRetrieveOutputBuffer failed with %!STATUS!",
			Status
		);

		KdPrintEx((
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_INFO_LEVEL,
			"WdfRequestRetrieveOutputBuffer failed, status = 0x%x \n",
			Status
			));

		goto exit;
	}

	Status = WdfMemoryCopyFromBuffer(
		PtpRequestMemory,
		0,
		(PVOID) &PtpReport,
		sizeof(PTP_REPORT)
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfMemoryCopyFromBuffer failed with %!STATUS!",
			Status
		);

		KdPrintEx((
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_INFO_LEVEL,
			"WdfMemoryCopyFromBuffer failed, status = 0x%x \n",
			Status
			));

		goto exit;
	}

	// Set information
	WdfRequestSetInformation(
		PtpRequest,
		sizeof(PTP_REPORT)
	);

	// Clear flag
	pDeviceContext->PendingRequest = FALSE;

exit:
	WdfRequestComplete(
		PtpRequest,
		Status
	);

set_event:

	// Set event
	KeSetEvent(
		&pDeviceContext->PtpRequestRoutineEvent, 
		0, 
		FALSE
	);
}
