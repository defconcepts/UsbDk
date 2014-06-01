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

#include "DeviceAccess.h"
#include "trace.h"
#include "Irp.h"
#include "RegText.h"
#include "DeviceAccess.tmh"

#include "UsbIOCtl.h"

#if !TARGET_OS_WIN_XP
bool CWdmUSBD::Create()
{
    auto status = IoCreateDevice(m_Driver, 0, NULL, FILE_DEVICE_UNKNOWN, FILE_AUTOGENERATED_DEVICE_NAME, FALSE, &m_USBDDevice);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! IoCreateDevice failed, %!STATUS!", status);
        return false;
    }

    m_AttachmentPoint = IoAttachDeviceToDeviceStack(m_USBDDevice, m_TargetDevice);
    if (m_AttachmentPoint == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! IoAttachDeviceToDeviceStack returned NULL");
        return false;
    }

    status = USBD_CreateHandle(m_USBDDevice, m_AttachmentPoint, USBD_CLIENT_CONTRACT_VERSION_602, 'DBHR', &m_USBDHandle);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! USBD_CreateHandle failed, %!STATUS!", status);
        return false;
    }

    return true;
}

CWdmUSBD::~CWdmUSBD()
{
    if (m_USBDHandle != nullptr)
    {
        USBD_CloseHandle(m_USBDHandle);
    }
    if (m_AttachmentPoint != nullptr)
    {
        IoDetachDevice(m_AttachmentPoint);
    }
    if (m_USBDDevice != nullptr)
    {
        IoDeleteDevice(m_USBDDevice);
    }
}
#endif

ULONG CDeviceAccess::GetAddress()
{
    DEVICE_CAPABILITIES Capabilities;

    if (!NT_SUCCESS(QueryCapabilities(Capabilities)))
    {
        return NO_ADDRESS;
    }

    return Capabilities.Address;
}

PWCHAR CWdfDeviceAccess::QueryBusID(BUS_QUERY_ID_TYPE idType)
{
    UNREFERENCED_PARAMETER(idType);
    ASSERT(!"NOT IMPLEMENTED");
    return nullptr;
}

NTSTATUS CWdfDeviceAccess::QueryCapabilities(DEVICE_CAPABILITIES &Capabilities)
{
    UNREFERENCED_PARAMETER(Capabilities);
    ASSERT(!"NOT IMPLEMENTED");
    return STATUS_NOT_IMPLEMENTED;
}

CMemoryBuffer *CWdfDeviceAccess::GetDeviceProperty(DEVICE_REGISTRY_PROPERTY propertyId)
{
    PAGED_CODE();
    WDFMEMORY devProperty;

    NTSTATUS status = WdfDeviceAllocAndQueryProperty(m_DevObj,
                                                     propertyId,
                                                     NonPagedPool,
                                                     WDF_NO_OBJECT_ATTRIBUTES,
                                                     &devProperty);

    if (NT_SUCCESS(status))
    {
        return CMemoryBuffer::GetMemoryBuffer(devProperty);
    }

    return nullptr;
}

PWCHAR CWdmDeviceAccess::QueryBusID(BUS_QUERY_ID_TYPE idType)
{
    CIrp irp;

    auto status = irp.Create(m_DevObj);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Error %!STATUS! during IRP creation", status);
        return nullptr;
    }

    irp.Configure([idType] (PIO_STACK_LOCATION s)
                  {
                      s->MajorFunction = IRP_MJ_PNP;
                      s->MinorFunction = IRP_MN_QUERY_ID;
                      s->Parameters.QueryId.IdType = idType;
                  });

    status = irp.SendSynchronously();

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Error %!STATUS! during %!devid! query", status, idType);
        return nullptr;
    }

    PWCHAR idData;
    irp.ReadResult([&idData](ULONG_PTR information)
                   { idData = reinterpret_cast<PWCHAR>(information); });

    return (idData != nullptr) ? MakeNonPagedDuplicate(idType, idData) : nullptr;
}

NTSTATUS CWdmDeviceAccess::QueryCapabilities(DEVICE_CAPABILITIES &Capabilities)
{
    CIrp irp;

    auto status = irp.Create(m_DevObj);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Error %!STATUS! during IRP creation", status);
        return status;
    }

    Capabilities = {};
    Capabilities.Size = sizeof(Capabilities);

    irp.Configure([&Capabilities](PIO_STACK_LOCATION s)
                  {
                      s->MajorFunction = IRP_MJ_PNP;
                      s->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
                      s->Parameters.DeviceCapabilities.Capabilities = &Capabilities;
                  });

    status = irp.SendSynchronously();
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Error %!STATUS! during capabilities query", status);
    }

    return status;
}

SIZE_T CWdmDeviceAccess::GetIdBufferLength(BUS_QUERY_ID_TYPE idType, PWCHAR idData)
{
    switch (idType)
    {
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs:
        return CRegMultiSz::GetBufferLength(idData) + sizeof(WCHAR);
    default:
        return CRegSz::GetBufferLength(idData);
    }
}

PWCHAR CWdmDeviceAccess::MakeNonPagedDuplicate(BUS_QUERY_ID_TYPE idType, PWCHAR idData)
{
    auto bufferLength = GetIdBufferLength(idType, idData);

    auto newIdData = ExAllocatePoolWithTag(NonPagedPool, bufferLength, 'IDHR');
    if (newIdData != nullptr)
    {
        RtlCopyMemory(newIdData, idData, bufferLength);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Failed to allocate non-paged buffer for %!devid!", idType);
    }

    ExFreePool(idData);
    return static_cast<PWCHAR>(newIdData);
}

CMemoryBuffer *CWdmDeviceAccess::GetDeviceProperty(DEVICE_REGISTRY_PROPERTY propertyId)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVACCESS, "%!FUNC! Entry for device 0x%p, %!devprop!", m_DevObj, propertyId);

    PAGED_CODE();

    ULONG bytesNeeded = 0;
    auto status = IoGetDeviceProperty(m_DevObj, propertyId, 0, nullptr, &bytesNeeded);

    if (!NT_SUCCESS(status))
    {
        if (status == STATUS_BUFFER_TOO_SMALL)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVACCESS,
                "%!FUNC! Property %!devprop! size for device 0x%p is %lu bytes", propertyId, m_DevObj, bytesNeeded);

            CObjHolder<CWdmMemoryBuffer> buffer(new CWdmMemoryBuffer());
            if (buffer)
            {
                status = buffer->Create(bytesNeeded, NonPagedPool);
                if (!NT_SUCCESS(status))
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS,
                        "%!FUNC! Error %!STATUS! during memory buffer creation", status);
                    return nullptr;
                }

                status = IoGetDeviceProperty(m_DevObj, propertyId, static_cast<ULONG>(buffer->Size()), buffer->Ptr(), &bytesNeeded);
                if (NT_SUCCESS(status))
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS,
                        "%!FUNC! Error %!STATUS! while reading property for device 0x%p, %!devprop!", status, m_DevObj, propertyId);
                    return buffer.detach();
                }
            }
        }
        else
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS,
                "%!FUNC! Error %!STATUS! while reading property for device 0x%p, %!devprop!", status, m_DevObj, propertyId);
        }
    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVACCESS,
            "%!FUNC! Empty property read for device 0x%p, %!devprop!", m_DevObj, propertyId);
        return CMemoryBuffer::GetMemoryBuffer(nullptr, 0);
    }

    return nullptr;
}

NTSTATUS CWdmUsbDeviceAccess::Reset()
{
    CIoControlIrp Irp;
    auto status = Irp.Create(m_DevObj, IOCTL_INTERNAL_USB_CYCLE_PORT);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Error %!STATUS! during IOCTL IRP creation", status);
        return status;
    }

    status = Irp.SendSynchronously();

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Send IOCTL IRP Error %!STATUS!", status);
    }

    return status;
}

NTSTATUS CWdmUsbDeviceAccess::GetDeviceDescriptor(USB_DEVICE_DESCRIPTOR &Descriptor)
{
    URB Urb;
    UsbDkBuildDescriptorRequest(Urb, USB_DEVICE_DESCRIPTOR_TYPE, 0, Descriptor);
    return UsbDkSendUrbSynchronously(m_DevObj, Urb);
}

NTSTATUS CWdmUsbDeviceAccess::GetConfigurationDescriptor(UCHAR Index, USB_CONFIGURATION_DESCRIPTOR &Descriptor, size_t Length)
{
    RtlZeroMemory(&Descriptor, Length);

    URB Urb;
    UsbDkBuildDescriptorRequest(Urb, USB_CONFIGURATION_DESCRIPTOR_TYPE, Index, Descriptor, static_cast<ULONG>(Length));

    auto status = UsbDkSendUrbSynchronously(m_DevObj, Urb);
    if (Descriptor.wTotalLength == 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Invalid configuration descriptor on unknown size received.");
        return USBD_STATUS_INAVLID_CONFIGURATION_DESCRIPTOR;
    }

    if ((Descriptor.wTotalLength <= Length) && !NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Failed to retrieve the configuration descriptor.");
        return status;
    }

    return STATUS_SUCCESS;
}

USB_DK_DEVICE_SPEED UsbDkWdmUsbDeviceGetSpeed(PDEVICE_OBJECT DevObj, PDRIVER_OBJECT DriverObj)
{
#if !TARGET_OS_WIN_XP
    CWdmUSBD USBD(DriverObj, DevObj);

    if (!USBD.Create())
    {
        return NoSpeed;
    }

    if (USBD.IsSuperSpeed())
    {
        return SuperSpeed;
    }

    if (USBD.IsHighSpeed())
    {
        return HighSpeed;
    }

    return FullSpeed;
#else
    //TODO: Not implemented
    UNREFERENCED_PARAMETER(DriverObj);
    UNREFERENCED_PARAMETER(DevObj);
    ASSERT(false);
    return NoSpeed;
#endif
}

bool UsbDkWdmUsbDeviceIsHub(PDEVICE_OBJECT PDO)
{
    CWdmUsbDeviceAccess pdoAccess(PDO);

    USB_DEVICE_DESCRIPTOR DevDescr;
    if (!NT_SUCCESS(pdoAccess.GetDeviceDescriptor(DevDescr)))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Cannot query device descriptor");
        return false;
    }

    return (DevDescr.bDeviceClass == USB_DEVICE_CLASS_HUB);
}

bool UsbDkGetWdmDeviceIdentity(const PDEVICE_OBJECT PDO,
                               CObjHolder<CRegText> *DeviceID,
                               CObjHolder<CRegText> *InstanceID)
{
    CWdmDeviceAccess pdoAccess(PDO);

    if (DeviceID != nullptr)
    {
        *DeviceID = pdoAccess.GetDeviceID();
        if (!(*DeviceID) || (*DeviceID)->empty())
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! No Device IDs read");
            return false;
        }
    }

    if (InstanceID != nullptr)
    {
        *InstanceID = pdoAccess.GetInstanceID();
        if (!(*InstanceID) || (*InstanceID)->empty())
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! No Instance ID read");
            return false;
        }
    }

    return true;
}

NTSTATUS UsbDkSendUrbSynchronously(PDEVICE_OBJECT Target, URB &Urb)
{
    CIoControlIrp Irp;
    auto status = Irp.Create(Target, IOCTL_INTERNAL_USB_SUBMIT_URB);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Error %!STATUS! during IOCTL IRP creation", status);
        return status;
    }

    Irp.Configure([&Urb] (PIO_STACK_LOCATION s)
                  { s->Parameters.Others.Argument1 = &Urb; });

    status = Irp.SendSynchronously();

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVACCESS, "%!FUNC! Send URB IRP Error %!STATUS!", status);
    }

    return status;
}
