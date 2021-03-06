/**********************************************************************
* Copyright (c) 2013-2014  Red Hat, Inc.
*
* Developed by Daynix Computing LTD.
*
* Authors:
*     Dmitry Fleytman <dmitry@daynix.com>
*     Pavel Gurvich <pavel@daynix.com>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
**********************************************************************/

#include "stdafx.h"
#include "RedirectorStrategy.h"
#include "trace.h"
#include "RedirectorStrategy.tmh"
#include "FilterDevice.h"
#include "UsbDkNames.h"
#include "ControlDevice.h"
#include "WdfRequest.h"

NTSTATUS CUsbDkRedirectorStrategy::MakeAvailable()
{
    auto status = m_Target.Create(m_Owner->WdfObject());
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Cannot create USB target");
        return status;
    }

    status = m_ControlDevice->NotifyRedirectorAttached(m_DeviceID, m_InstanceID, m_Owner);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Failed to raise creation notification");
    }

    return status;
}

NTSTATUS CUsbDkRedirectorStrategy::Create(CUsbDkFilterDevice *Owner)
{
    auto status = CUsbDkFilterStrategy::Create(Owner);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Base class creation failed");
        return status;
    }

    m_IncomingDataQueue = new CUsbDkRedirectorQueueData(*m_Owner);
    if (!m_IncomingDataQueue)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! RW Queue allocation failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = m_IncomingDataQueue->Create();
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! RW Queue creation failed");
    }

    m_IncomingConfigQueue = new CUsbDkRedirectorQueueConfig(*m_Owner);
    if (!m_IncomingConfigQueue)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! IOCTL Queue allocation failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = m_IncomingConfigQueue->Create();
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! IOCTL Queue creation failed");
    }
    return status;
}

void CUsbDkRedirectorStrategy::Delete()
{
    CUsbDkFilterStrategy::Delete();
}

void CUsbDkRedirectorStrategy::PatchDeviceID(PIRP Irp)
{
    static const WCHAR RedirectorDeviceId[] = L"USB\\Vid_2B23&Pid_CAFE&Rev_0001";
    static const WCHAR RedirectorHardwareIds[] = L"USB\\Vid_2B23&Pid_CAFE&Rev_0001\0USB\\Vid_2B23&Pid_CAFE\0";
    static const WCHAR RedirectorCompatibleIds[] = L"USB\\Class_FF&SubClass_FF&Prot_FF\0USB\\Class_FF&SubClass_FF\0USB\\Class_FF\0";

    static const size_t MAX_DEC_NUMBER_LEN = 11;
    WCHAR SzInstanceID[ARRAY_SIZE(USBDK_DRIVER_NAME) + MAX_DEC_NUMBER_LEN + 1];

    const WCHAR *Buffer;
    SIZE_T Size = 0;

    PIO_STACK_LOCATION  irpStack = IoGetCurrentIrpStackLocation(Irp);

    switch (irpStack->Parameters.QueryId.IdType)
    {
        case BusQueryDeviceID:
            Buffer = &RedirectorDeviceId[0];
            Size = sizeof(RedirectorDeviceId);
            break;

        case BusQueryInstanceID:
        {
            CString InstanceID;
            auto status = InstanceID.Create(USBDK_DRIVER_NAME, m_Owner->GetInstanceNumber());
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Failed to create instance ID string %!STATUS!", status);
                return;
            }

            Size = InstanceID.ToWSTR(SzInstanceID, sizeof(SzInstanceID));
            Buffer = &SzInstanceID[0];
            break;
        }

        case BusQueryHardwareIDs:
            Buffer = &RedirectorHardwareIds[0];
            Size = sizeof(RedirectorHardwareIds);
            break;

        case BusQueryCompatibleIDs:
            Buffer = &RedirectorCompatibleIds[0];
            Size = sizeof(RedirectorCompatibleIds);
            break;

        default:
            Buffer = nullptr;
            break;
    }

    if (Buffer != nullptr)
    {
        auto Result = DuplicateStaticBuffer(Buffer, Size);

        if (Result == nullptr)
        {
            return;
        }

        if (Irp->IoStatus.Information)
        {
            ExFreePool(reinterpret_cast<PVOID>(Irp->IoStatus.Information));

        }
        Irp->IoStatus.Information = reinterpret_cast<ULONG_PTR>(Result);
    }
}

NTSTATUS CUsbDkRedirectorStrategy::PNPPreProcess(PIRP Irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
    switch (irpStack->MinorFunction)
    {
    case IRP_MN_QUERY_ID:
        return PostProcessOnSuccess(Irp,
                                    [this](PIRP Irp)
                                    {
                                        PatchDeviceID(Irp);
                                    });

    case IRP_MN_QUERY_CAPABILITIES:
        return PostProcessOnSuccess(Irp,
                                    [](PIRP Irp)
                                    {
                                        auto irpStack = IoGetCurrentIrpStackLocation(Irp);
                                        irpStack->Parameters.DeviceCapabilities.Capabilities->RawDeviceOK = 1;
                                        irpStack->Parameters.DeviceCapabilities.Capabilities->NoDisplayInUI = 1;
                                        irpStack->Parameters.DeviceCapabilities.Capabilities->Removable = 0;
                                        irpStack->Parameters.DeviceCapabilities.Capabilities->EjectSupported = 0;
                                        irpStack->Parameters.DeviceCapabilities.Capabilities->SilentInstall = 1;
                                    });
    default:
        return CUsbDkFilterStrategy::PNPPreProcess(Irp);
    }
}

typedef struct tag_USBDK_REDIRECTOR_REQUEST_CONTEXT
{
    WDFMEMORY LockedBuffer;
    ULONG64 EndpointAddress;
    USB_DK_TRANSFER_TYPE TransferType;
    PULONG64 BytesTransferred;
    WDF_USB_CONTROL_SETUP_PACKET SetupPacket;

    WDFMEMORY LockedIsochronousPacketsArray;
    WDFMEMORY LockedIsochronousResultsArray;
} USBDK_REDIRECTOR_REQUEST_CONTEXT, *PUSBDK_REDIRECTOR_REQUEST_CONTEXT;

class CRedirectorRequest : public CWdfRequest
{
public:
    CRedirectorRequest(WDFREQUEST Request)
        : CWdfRequest(Request)
    {}

    PUSBDK_REDIRECTOR_REQUEST_CONTEXT Context()
    { return reinterpret_cast<PUSBDK_REDIRECTOR_REQUEST_CONTEXT>(UsbDkFilterRequestGetContext(m_Request)); }

private:
    void SetBytesWritten(size_t numBytes);
    void SetBytesRead(size_t numBytes);
};

template <typename TLockerFunc>
NTSTATUS CUsbDkRedirectorStrategy::IoInCallerContextRW(CRedirectorRequest &WdfRequest,
                                                       TLockerFunc LockerFunc)
{
    PUSB_DK_TRANSFER_REQUEST TransferRequest;

    auto status = WdfRequest.FetchInputObject(TransferRequest);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! failed to read transfer request, %!STATUS!", status);
        return status;
    }

    PUSBDK_REDIRECTOR_REQUEST_CONTEXT context = WdfRequest.Context();
    context->EndpointAddress = TransferRequest->endpointAddress;
    context->TransferType = static_cast<USB_DK_TRANSFER_TYPE>(TransferRequest->transferType);

    status = WdfRequest.FetchOutputObject(context->BytesTransferred);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! failed to fetch output buffer, %!STATUS!", status);
        return status;
    }

    switch (context->TransferType)
    {
    case ControlTransferType:
        status = IoInCallerContextRWControlTransfer(WdfRequest, *TransferRequest);
        break;
    case BulkTransferType:
    case InterruptTransferType:
        status = LockerFunc(WdfRequest, *TransferRequest, context->LockedBuffer);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Failed to lock user buffer, %!STATUS!", status);
        }
        break;

    case IsochronousTransferType:
        status = IoInCallerContextRWIsoTransfer(WdfRequest, *TransferRequest, LockerFunc);
        break;

    default:
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Error: Wrong TransferType: %d", context->TransferType);
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    return status;
}

template <typename TLockerFunc>
NTSTATUS CUsbDkRedirectorStrategy::IoInCallerContextRWIsoTransfer(CRedirectorRequest &WdfRequest,
                                                                  const USB_DK_TRANSFER_REQUEST &TransferRequest,
                                                                  TLockerFunc LockerFunc)
{
    PUSBDK_REDIRECTOR_REQUEST_CONTEXT context = WdfRequest.Context();
    auto status = LockerFunc(WdfRequest, TransferRequest, context->LockedBuffer);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Failed to lock user buffer, %!STATUS!", status);
        return status;
    }

#pragma warning(push)
#pragma warning(disable:4244) //Unsafe conversions on 32 bit
    status = WdfRequest.LockUserBufferForRead(reinterpret_cast<PVOID>(TransferRequest.IsochronousPacketsArray),
        sizeof(ULONG64) * TransferRequest.IsochronousPacketsArraySize, context->LockedIsochronousPacketsArray);
#pragma warning(pop)
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Failed to lock user buffer of Iso Packet Lengths, %!STATUS!", status);
        return status;
    }

#pragma warning(push)
#pragma warning(disable:4244) //Unsafe conversions on 32 bit
    status = WdfRequest.LockUserBufferForWrite(reinterpret_cast<PVOID>(TransferRequest.Result.isochronousResultsArray),
        sizeof(USB_DK_ISO_TRANSFER_RESULT) * TransferRequest.IsochronousPacketsArraySize, context->LockedIsochronousResultsArray);
#pragma warning(pop)
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Failed to lock user buffer of Iso Packet Result, %!STATUS!", status);
    }

    return status;
}

NTSTATUS CUsbDkRedirectorStrategy::IoInCallerContextRWControlTransfer(CRedirectorRequest &WdfRequest,
                                                                      const USB_DK_TRANSFER_REQUEST &TransferRequest)
{
    NTSTATUS status;
    PUSBDK_REDIRECTOR_REQUEST_CONTEXT context = WdfRequest.Context();
    __try
    {
#pragma warning(push)
#pragma warning(disable:4244) //Unsafe conversions on 32 bit
        ProbeForRead(static_cast<PVOID>(TransferRequest.buffer), sizeof(WDF_USB_CONTROL_SETUP_PACKET), 1);
        context->SetupPacket = *static_cast<PWDF_USB_CONTROL_SETUP_PACKET>(TransferRequest.buffer);
#pragma warning(pop)
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! ProbeForRead failed!");
        return STATUS_ACCESS_VIOLATION;
    }

    size_t bufferLength = static_cast<size_t>(TransferRequest.bufferLength) - sizeof(WDF_USB_CONTROL_SETUP_PACKET);
    if (bufferLength > 0)
    {
        if (context->SetupPacket.Packet.bm.Request.Dir == BMREQUEST_HOST_TO_DEVICE) // write
        {
#pragma warning(push)
#pragma warning(disable:4244) //Unsafe conversions on 32 bit
            status = WdfRequest.LockUserBufferForRead(static_cast<PVOID>(static_cast<PWDF_USB_CONTROL_SETUP_PACKET>(TransferRequest.buffer) + 1),
                                                      bufferLength, context->LockedBuffer);
#pragma warning(pop)
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! LockUserBufferForRead failed %!STATUS!", status);
            }
        }
        else // read
        {
#pragma warning(push)
#pragma warning(disable:4244) //Unsafe conversions on 32 bit
            status = WdfRequest.LockUserBufferForWrite(static_cast<PVOID>(static_cast<PWDF_USB_CONTROL_SETUP_PACKET>(TransferRequest.buffer) + 1),
                                                       bufferLength, context->LockedBuffer);
#pragma warning(pop)
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! LockUserBufferForWrite failed %!STATUS!", status);
            }
        }
    }
    else
    {
        context->LockedBuffer = WDF_NO_HANDLE;
        status = STATUS_SUCCESS;
    }

    return status;
}

void CUsbDkRedirectorStrategy::IoInCallerContext(WDFDEVICE Device, WDFREQUEST Request)
{
    CRedirectorRequest WdfRequest(Request);
    WDF_REQUEST_PARAMETERS Params;
    WdfRequest.GetParameters(Params);

    NTSTATUS status = STATUS_SUCCESS;

    if (Params.Type == WdfRequestTypeDeviceControl)
    {
        switch (Params.Parameters.DeviceIoControl.IoControlCode)
        {
        case IOCTL_USBDK_DEVICE_READ_PIPE:
            status = IoInCallerContextRW(WdfRequest,
                                         [](const CRedirectorRequest &WdfRequest, const USB_DK_TRANSFER_REQUEST &Transfer, WDFMEMORY &LockedMemory)
#pragma warning(push)
#pragma warning(disable:4244) //Unsafe conversions on 32 bit
                                         { return WdfRequest.LockUserBufferForWrite(Transfer.buffer, Transfer.bufferLength, LockedMemory); });
#pragma warning(pop)
            break;
        case IOCTL_USBDK_DEVICE_WRITE_PIPE:
            status = IoInCallerContextRW(WdfRequest,
                                         [](const CRedirectorRequest &WdfRequest, const USB_DK_TRANSFER_REQUEST &Transfer, WDFMEMORY &LockedMemory)
#pragma warning(push)
#pragma warning(disable:4244) //Unsafe conversions on 32 bit
                                         { return WdfRequest.LockUserBufferForRead(Transfer.buffer, Transfer.bufferLength, LockedMemory); });
#pragma warning(pop)
            break;
        default:
            break;
        }
    }

    if (NT_SUCCESS(status))
    {
        CUsbDkFilterStrategy::IoInCallerContext(Device, WdfRequest.Detach());
    }
    else
    {
        WdfRequest.SetStatus(status);
    }
}

void CUsbDkRedirectorStrategy::DoControlTransfer(CRedirectorRequest &WdfRequest, WDFMEMORY DataBuffer)
{
    PUSBDK_REDIRECTOR_REQUEST_CONTEXT context = WdfRequest.Context();

    WDFMEMORY_OFFSET TransferOffset;
    TransferOffset.BufferOffset = 0;
    if (DataBuffer != WDF_NO_HANDLE)
    {
        CPreAllocatedWdfMemoryBuffer MemoryBuffer(DataBuffer);
        TransferOffset.BufferLength = static_cast<ULONG>(MemoryBuffer.Size());
    }
    else
    {
        TransferOffset.BufferLength = 0;
    }

    auto status = m_Target.ControlTransferAsync(WdfRequest, &context->SetupPacket, DataBuffer, &TransferOffset,
                                  [](WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
                                  {
                                         UNREFERENCED_PARAMETER(Target);
                                         UNREFERENCED_PARAMETER(Context);

                                         CRedirectorRequest WdfRequest(Request);
                                         auto RequestContext = WdfRequest.Context();

                                         auto status = Params->IoStatus.Status;
                                         auto usbCompletionParams = Params->Parameters.Usb.Completion;
                                         *RequestContext->BytesTransferred = usbCompletionParams->Parameters.DeviceControlTransfer.Length;

                                         if (!NT_SUCCESS(status))
                                         {
                                             TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Control transfer failed: %!STATUS! UsbdStatus 0x%x\n",
                                                 status, usbCompletionParams->UsbdStatus);
                                         }

                                         WdfRequest.SetOutputDataLen(sizeof(*RequestContext->BytesTransferred));
                                         WdfRequest.SetStatus(status);
                                  });

    WdfRequest.SetStatus(status);
}

void CUsbDkRedirectorStrategy::IoDeviceControl(WDFREQUEST Request,
                                               size_t OutputBufferLength,
                                               size_t InputBufferLength,
                                               ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    switch (IoControlCode)
    {
        default:
        {
            CWdfRequest(Request).ForwardToIoQueue(*m_IncomingConfigQueue);
            break;
        }
        case IOCTL_USBDK_DEVICE_READ_PIPE:
        {
            ReadPipe(Request);
            break;
        }
        case IOCTL_USBDK_DEVICE_WRITE_PIPE:
        {
            WritePipe(Request);
            break;
        }
    }
}

void CUsbDkRedirectorStrategy::IoDeviceControlConfig(WDFREQUEST Request,
                                                     size_t OutputBufferLength,
                                                     size_t InputBufferLength,
                                                     ULONG IoControlCode)
{
    switch (IoControlCode)
    {
        default:
        {
            CUsbDkFilterStrategy::IoDeviceControl(Request, OutputBufferLength, InputBufferLength, IoControlCode);
            return;
        }
        case IOCTL_USBDK_DEVICE_ABORT_PIPE:
        {
            CWdfRequest WdfRequest(Request);
            UsbDkHandleRequestWithInput<ULONG64>(WdfRequest,
                                            [this, Request](ULONG64 *endpointAddress, size_t)
                                            {return m_Target.AbortPipe(Request, *endpointAddress); });
            return;
        }
        case IOCTL_USBDK_DEVICE_RESET_PIPE:
        {
            CWdfRequest WdfRequest(Request);
            UsbDkHandleRequestWithInput<ULONG64>(WdfRequest,
                                            [this, Request](ULONG64 *endpointAddress, size_t)
                                            {return m_Target.ResetPipe(Request, *endpointAddress); });
            return;
        }
        case IOCTL_USBDK_DEVICE_SET_ALTSETTING:
        {
            ASSERT(m_IncomingDataQueue);

            m_IncomingDataQueue->StopSync();
            CWdfRequest WdfRequest(Request);
            UsbDkHandleRequestWithInput<USBDK_ALTSETTINGS_IDXS>(WdfRequest,
                                                [this, Request](USBDK_ALTSETTINGS_IDXS *altSetting, size_t)
                                                {return m_Target.SetInterfaceAltSetting(altSetting->InterfaceIdx, altSetting->AltSettingIdx);});
            m_IncomingDataQueue->Start();
            return;
        }
        case IOCTL_USBDK_DEVICE_RESET_DEVICE:
        {
            CWdfRequest WdfRequest(Request);
            auto status = m_Target.ResetDevice(Request);
            WdfRequest.SetStatus(status);
            return;
        }
    }
}

void CUsbDkRedirectorStrategy::WritePipe(WDFREQUEST Request)
{
    CRedirectorRequest WdfRequest(Request);
    PUSBDK_REDIRECTOR_REQUEST_CONTEXT Context = WdfRequest.Context();

    switch (Context->TransferType)
    {
    case ControlTransferType:
        DoControlTransfer(WdfRequest, Context->LockedBuffer);
        break;
    case BulkTransferType:
    case InterruptTransferType:
        if (Context->LockedBuffer != WDF_NO_HANDLE)
        {
            m_Target.WritePipeAsync(WdfRequest.Detach(), Context->EndpointAddress, Context->LockedBuffer,
                                    [](WDFREQUEST Request, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT)
                                    {
                                        CRedirectorRequest WdfRequest(Request);
                                        auto RequestContext = WdfRequest.Context();

                                        auto status = Params->IoStatus.Status;
                                        auto usbCompletionParams = Params->Parameters.Usb.Completion;
                                        *RequestContext->BytesTransferred = usbCompletionParams->Parameters.PipeWrite.Length;

                                        if (!NT_SUCCESS(status))
                                        {
                                            TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Write failed: %!STATUS! UsbdStatus 0x%x\n",
                                                status, usbCompletionParams->UsbdStatus);
                                        }

                                        WdfRequest.SetOutputDataLen(sizeof(*RequestContext->BytesTransferred));
                                        WdfRequest.SetStatus(status);
                                    });
        }
        else
        {
            WdfRequest.SetStatus(STATUS_INVALID_PARAMETER);
        }
        break;
    case IsochronousTransferType:
        if (Context->LockedBuffer != WDF_NO_HANDLE)
        {
            CPreAllocatedWdfMemoryBufferT<ULONG64> PacketSizesArray(Context->LockedIsochronousPacketsArray);

            m_Target.WriteIsochronousPipeAsync(WdfRequest.Detach(),
                                               Context->EndpointAddress,
                                               Context->LockedBuffer,
                                               PacketSizesArray,
                                               PacketSizesArray.ArraySize(),
                                               IsoRWCompletion);
        }
        else
        {
            WdfRequest.SetStatus(STATUS_INVALID_PARAMETER);
        }
        break;
    default:
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Error: Wrong transfer type: %d\n", Context->TransferType);
        WdfRequest.SetStatus(STATUS_INVALID_PARAMETER);
    }
}

void CUsbDkRedirectorStrategy::ReadPipe(WDFREQUEST Request)
{
    CRedirectorRequest WdfRequest(Request);
    PUSBDK_REDIRECTOR_REQUEST_CONTEXT Context = WdfRequest.Context();
    switch (Context->TransferType)
    {
    case ControlTransferType:
        DoControlTransfer(WdfRequest, Context->LockedBuffer);
        break;
    case BulkTransferType:
    case InterruptTransferType:
        if (Context->LockedBuffer != WDF_NO_HANDLE)
        {
            m_Target.ReadPipeAsync(WdfRequest.Detach(), Context->EndpointAddress, Context->LockedBuffer,
                                   [](WDFREQUEST Request, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT)
                                   {
                                        CRedirectorRequest WdfRequest(Request);
                                        auto RequestContext = WdfRequest.Context();

                                        auto status = Params->IoStatus.Status;
                                        auto usbCompletionParams = Params->Parameters.Usb.Completion;
                                        *RequestContext->BytesTransferred = usbCompletionParams->Parameters.PipeRead.Length;

                                        if (!NT_SUCCESS(status))
                                        {
                                            TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Read failed: %!STATUS! UsbdStatus 0x%x\n",
                                                status, usbCompletionParams->UsbdStatus);
                                        }

                                        WdfRequest.SetOutputDataLen(sizeof(*RequestContext->BytesTransferred));
                                        WdfRequest.SetStatus(status);
                                    });
        }
        else
        {
            WdfRequest.SetStatus(STATUS_INVALID_PARAMETER);
        }
        break;
    case IsochronousTransferType:
        if (Context->LockedBuffer != WDF_NO_HANDLE)
        {
            CPreAllocatedWdfMemoryBufferT<ULONG64> PacketSizesArray(Context->LockedIsochronousPacketsArray);

            m_Target.ReadIsochronousPipeAsync(WdfRequest.Detach(),
                                              Context->EndpointAddress,
                                              Context->LockedBuffer,
                                              PacketSizesArray,
                                              PacketSizesArray.ArraySize(),
                                              IsoRWCompletion);
        }
        else
        {
            WdfRequest.SetStatus(STATUS_INVALID_PARAMETER);
        }
        break;
    default:
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Error: Wrong transfer type: %d\n", Context->TransferType);
        WdfRequest.SetStatus(STATUS_INVALID_PARAMETER);
    }
}

void CUsbDkRedirectorStrategy::IsoRWCompletion(WDFREQUEST Request, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS CompletionParams, WDFCONTEXT)
{
    CRedirectorRequest WdfRequest(Request);
    auto Context = WdfRequest.Context();

    CPreAllocatedWdfMemoryBufferT<URB> urb(CompletionParams->Parameters.Usb.Completion->Parameters.PipeUrb.Buffer);
    CPreAllocatedWdfMemoryBufferT<USB_DK_ISO_TRANSFER_RESULT> IsoPacketResult(Context->LockedIsochronousResultsArray);

    ASSERT(urb->UrbIsochronousTransfer.NumberOfPackets == IsoPacketResult.ArraySize());

    *Context->BytesTransferred = 0;

    for (ULONG i = 0; i < urb->UrbIsochronousTransfer.NumberOfPackets; i++)
    {
        IsoPacketResult[i].actualLength = urb->UrbIsochronousTransfer.IsoPacket[i].Length;
        IsoPacketResult[i].transferResult = urb->UrbIsochronousTransfer.IsoPacket[i].Status;
        *Context->BytesTransferred += IsoPacketResult[i].actualLength;
    }

    auto status = CompletionParams->IoStatus.Status;
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! Request completion error %!STATUS!", status);
    }
    else if (!USBD_SUCCESS(urb->UrbHeader.Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! USB request completion error %lu", urb->UrbHeader.Status);
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

    WdfRequest.SetOutputDataLen(sizeof(*Context->BytesTransferred));
    WdfRequest.SetStatus(status);
}

size_t CUsbDkRedirectorStrategy::GetRequestContextSize()
{
    return sizeof(USBDK_REDIRECTOR_REQUEST_CONTEXT);
}

void CUsbDkRedirectorStrategy::OnClose()
{
    USB_DK_DEVICE_ID ID;
    UsbDkFillIDStruct(&ID, *m_DeviceID->begin(), *m_InstanceID->begin());

    auto status = m_ControlDevice->RemoveRedirect(ID);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_REDIRECTOR, "%!FUNC! RemoveRedirect failed: %!STATUS!", status);
    }
}

void CUsbDkRedirectorQueueData::SetCallbacks(WDF_IO_QUEUE_CONFIG &QueueConfig)
{
    QueueConfig.EvtIoDeviceControl = [](WDFQUEUE Q, WDFREQUEST R, size_t OL, size_t IL, ULONG CTL)
                                     { UsbDkFilterGetContext(WdfIoQueueGetDevice(Q))->UsbDkFilter->m_Strategy->IoDeviceControl(R, OL, IL, CTL); };
}

void CUsbDkRedirectorQueueConfig::SetCallbacks(WDF_IO_QUEUE_CONFIG &QueueConfig)
{
    QueueConfig.EvtIoDeviceControl = [](WDFQUEUE Q, WDFREQUEST R, size_t OL, size_t IL, ULONG CTL)
                                     { UsbDkFilterGetContext(WdfIoQueueGetDevice(Q))->UsbDkFilter->m_Strategy->IoDeviceControlConfig(R, OL, IL, CTL); };
}
