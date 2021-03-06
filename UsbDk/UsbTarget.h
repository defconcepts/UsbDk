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

#pragma once

#include "Alloc.h"
#include "Urb.h"

class CWdfRequest;

class CWdfUsbPipe : public CAllocatable<NonPagedPool, 'PUHR'>
{
public:
    CWdfUsbPipe()
    {}

    void Create(WDFUSBDEVICE Device, WDFUSBINTERFACE Interface, UCHAR PipeIndex);
    void ReadAsync(CWdfRequest &Request, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);
    void WriteAsync(CWdfRequest &Request, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);

    void ReadIsochronousAsync(CWdfRequest &Request,
        WDFMEMORY Buffer,
        PULONG64 PacketSizes,
        size_t PacketNumber,
        PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion)
    {
        SubmitIsochronousTransfer(Request, CIsochronousUrb::URB_DIRECTION_IN, Buffer, PacketSizes, PacketNumber, Completion);
    }

    void WriteIsochronousAsync(CWdfRequest &Request,
        WDFMEMORY Buffer,
        PULONG64 PacketSizes,
        size_t PacketNumber,
        PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion)
    {
        SubmitIsochronousTransfer(Request, CIsochronousUrb::URB_DIRECTION_OUT, Buffer, PacketSizes, PacketNumber, Completion);
    }

    NTSTATUS Abort(WDFREQUEST Request);
    NTSTATUS Reset(WDFREQUEST Request);
    UCHAR EndpointAddress() const
    {
        return m_Info.EndpointAddress;
    }

private:
    WDFUSBINTERFACE m_Interface = WDF_NO_HANDLE;
    WDFUSBDEVICE m_Device = WDF_NO_HANDLE;
    WDFUSBPIPE m_Pipe = WDF_NO_HANDLE;
    WDF_USB_PIPE_INFORMATION m_Info;

    void SubmitIsochronousTransfer(CWdfRequest &Request,
        CIsochronousUrb::Direction Direction,
        WDFMEMORY Buffer,
        PULONG64 PacketSizes,
        size_t PacketNumber,
        PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);
    CWdfUsbPipe(const CWdfUsbPipe&) = delete;
    CWdfUsbPipe& operator= (const CWdfUsbPipe&) = delete;
};

class CWdfUsbInterface : public CAllocatable<NonPagedPool, 'IUHR'>
{
public:
    CWdfUsbInterface()
    {}

    NTSTATUS Create(WDFUSBDEVICE Device, UCHAR InterfaceIdx);
    NTSTATUS SetAltSetting(ULONG64 AltSettingIdx);

    CWdfUsbPipe *FindPipeByEndpointAddress(ULONG64 EndpointAddress);
    NTSTATUS Reset(WDFREQUEST Request);

private:
    WDFUSBDEVICE m_UsbDevice;
    WDFUSBINTERFACE m_Interface;

    CObjHolder<CWdfUsbPipe, CVectorDeleter<CWdfUsbPipe> > m_Pipes;
    BYTE m_NumPipes = 0;

    CWdfUsbInterface(const CWdfUsbInterface&) = delete;
    CWdfUsbInterface& operator= (const CWdfUsbInterface&) = delete;
};

class CWdfUsbTarget
{
public:
    CWdfUsbTarget() {}
    ~CWdfUsbTarget();

    NTSTATUS Create(WDFDEVICE Device);
    void DeviceDescriptor(USB_DEVICE_DESCRIPTOR &Descriptor);
    NTSTATUS SetInterfaceAltSetting(ULONG64 InterfaceIdx, ULONG64 AltSettingIdx);

    void WritePipeAsync(WDFREQUEST Request, ULONG64 EndpointAddress, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);
    void ReadPipeAsync(WDFREQUEST Request, ULONG64 EndpointAddress, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);

    void ReadIsochronousPipeAsync(WDFREQUEST Request, ULONG64 EndpointAddress, WDFMEMORY Buffer,
                                  PULONG64 PacketSizes, size_t PacketNumber,
                                  PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);
    void WriteIsochronousPipeAsync(WDFREQUEST Request, ULONG64 EndpointAddress, WDFMEMORY Buffer,
                                   PULONG64 PacketSizes, size_t PacketNumber,
                                   PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);

    NTSTATUS ControlTransferAsync(CWdfRequest &WdfRequest, PWDF_USB_CONTROL_SETUP_PACKET SetupPacket, WDFMEMORY Data,
                                  PWDFMEMORY_OFFSET TransferOffset, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);
    NTSTATUS AbortPipe(WDFREQUEST Request, ULONG64 EndpointAddress);
    NTSTATUS ResetPipe(WDFREQUEST Request, ULONG64 EndpointAddress);
    NTSTATUS ResetDevice(WDFREQUEST Request);

private:
    CWdfUsbPipe *FindPipeByEndpointAddress(ULONG64 EndpointAddress);

    WDFDEVICE m_Device = WDF_NO_HANDLE;
    WDFUSBDEVICE m_UsbDevice = WDF_NO_HANDLE;

    CObjHolder<CWdfUsbInterface, CVectorDeleter<CWdfUsbInterface> > m_Interfaces;
    UCHAR m_NumInterfaces = 0;

    CWdfUsbTarget(const CWdfUsbTarget&) = delete;
    CWdfUsbTarget& operator= (const CWdfUsbTarget&) = delete;
};
