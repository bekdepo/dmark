/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    nonpnp.c

Abstract:

    Purpose of this driver is to demonstrate how to write a legacy (NON WDM)
    driver using framework, show how to handle 4 different ioctls -
    METHOD_NEITHER - in particular and also show how to read & write to file
    from KernelMode using Zw functions.

    For a non-framework version of sample on how to handle IOCTLs in driver,
    study src\general\IOCTL in the DDK.

Environment:

    Kernel mode only.

--*/

#include "nonpnp.h"


#ifdef ALLOC_PRAGMA
#pragma alloc_text( INIT, DriverEntry )
#pragma alloc_text( PAGE, NonPnpDeviceAdd)
#pragma alloc_text( PAGE, NonPnpEvtDriverContextCleanup)
#pragma alloc_text( PAGE, NonPnpEvtDriverUnload)
#pragma alloc_text( PAGE, NonPnpEvtDeviceIoInCallerContext)
#pragma alloc_text( PAGE, NonPnpEvtDeviceFileCreate)
#pragma alloc_text( PAGE, NonPnpEvtFileClose)
#pragma alloc_text( PAGE, FileEvtIoRead)
#pragma alloc_text( PAGE, FileEvtIoWrite)
#pragma alloc_text( PAGE, FileEvtIoDeviceControl)
#endif // ALLOC_PRAGMA

VOID ProcessCreationCallback(
    _Inout_   PEPROCESS              Process,
    _In_      HANDLE                 ProcessId,
    _In_opt_  PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(CreateInfo);
    KdPrint(("Something with process: %d", ProcessId));
}

NTSTATUS
DriverEntry(
    IN OUT PDRIVER_OBJECT   DriverObject,
    IN PUNICODE_STRING      RegistryPath
    )
/*++

Routine Description:
    This routine is called by the Operating System to initialize the driver.

    It creates the device object, fills in the dispatch entry points and
    completes the initialization.

Arguments:
    DriverObject - a pointer to the object that represents this device
    driver.

    RegistryPath - a pointer to our Services key in the registry.

Return Value:
    STATUS_SUCCESS if initialized; an error otherwise.

--*/
{
    NTSTATUS                       status;
    WDF_DRIVER_CONFIG              config;
    WDFDRIVER                      hDriver;
    PWDFDEVICE_INIT                pInit = NULL;
    WDF_OBJECT_ATTRIBUTES          attributes;

    __asm int 3;

    KdPrint(("Driver Frameworks NONPNP Legacy Driver Example\n"));


    WDF_DRIVER_CONFIG_INIT(
        &config,
        WDF_NO_EVENT_CALLBACK // This is a non-pnp driver.
        );

    //
    // Tell the framework that this is non-pnp driver so that it doesn't
    // set the default AddDevice routine.
    //
    config.DriverInitFlags |= WdfDriverInitNonPnpDriver;

    //
    // NonPnp driver must explicitly register an unload routine for
    // the driver to be unloaded.
    //
    config.EvtDriverUnload = NonPnpEvtDriverUnload;

    //
    // Register a cleanup callback so that we can call WPP_CLEANUP when
    // the framework driver object is deleted during driver unload.
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = NonPnpEvtDriverContextCleanup;

    //
    // Create a framework driver object to represent our driver.
    //
    status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            &attributes,
                            &config,
                            &hDriver);
    if (!NT_SUCCESS(status)) {
        KdPrint (("NonPnp: WdfDriverCreate failed with status 0x%x\n", status));
        return status;
    }

    status = PsSetCreateProcessNotifyRoutineEx(ProcessCreationCallback, FALSE);
    KdPrint(("Status for CreateProcessNotifyRoutineEx is %d : %x\n", status, status));

    //
    // On Win2K system,  you will experience some delay in getting trace events
    // due to the way the ETW is activated to accept trace messages.
    //
    KdPrint(("NonPnp: DriverEntry: tracing enabled\n"));

    KdPrint(("Driver Frameworks NONPNP Legacy Driver Example"));

    //
    //
    // In order to create a control device, we first need to allocate a
    // WDFDEVICE_INIT structure and set all properties.
    //
    pInit = WdfControlDeviceInitAllocate(
                            hDriver,
                            &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R
                            );

    if (pInit == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        return status;
    }

    //
    // Call NonPnpDeviceAdd to create a deviceobject to represent our
    // software device.
    //
    status = NonPnpDeviceAdd(hDriver, pInit);
    
    

    return status;
}



NTSTATUS
NonPnpDeviceAdd(
    IN WDFDRIVER Driver,
    IN PWDFDEVICE_INIT DeviceInit
    )
/*++

Routine Description:

    Called by the DriverEntry to create a control-device. This call is
    responsible for freeing the memory for DeviceInit.

Arguments:

    DriverObject - a pointer to the object that represents this device
    driver.

    DeviceInit - Pointer to a driver-allocated WDFDEVICE_INIT structure.

Return Value:

    STATUS_SUCCESS if initialized; an error otherwise.

--*/
{
    NTSTATUS                       status;
    WDF_OBJECT_ATTRIBUTES           attributes;
    WDF_IO_QUEUE_CONFIG      ioQueueConfig;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDFQUEUE                            queue;
    WDFDEVICE   controlDevice;
    DECLARE_CONST_UNICODE_STRING(ntDeviceName, NTDEVICE_NAME_STRING) ;
    DECLARE_CONST_UNICODE_STRING(symbolicLinkName, SYMBOLIC_NAME_STRING) ;

    UNREFERENCED_PARAMETER( Driver );

    PAGED_CODE();

    KdPrint(("NonPnpDeviceAdd DeviceInit %p\n", DeviceInit));
    //
    // Set exclusive to TRUE so that no more than one app can talk to the
    // control device at any time.
    //
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);


    status = WdfDeviceInitAssignName(DeviceInit, &ntDeviceName);

    if (!NT_SUCCESS(status)) {
        KdPrint(( "WdfDeviceInitAssignName failed %!STATUS!", status));
        goto End;
    }

    WdfControlDeviceInitSetShutdownNotification(DeviceInit,
                                                NonPnpShutdown,
                                                WdfDeviceShutdown);

    //
    // Initialize WDF_FILEOBJECT_CONFIG_INIT struct to tell the
    // framework whether you are interested in handling Create, Close and
    // Cleanup requests that gets generated when an application or another
    // kernel component opens an handle to the device. If you don't register
    // the framework default behaviour would be to complete these requests
    // with STATUS_SUCCESS. A driver might be interested in registering these
    // events if it wants to do security validation and also wants to maintain
    // per handle (fileobject) context.
    //

    WDF_FILEOBJECT_CONFIG_INIT(
                        &fileConfig,
                        NonPnpEvtDeviceFileCreate,
                        NonPnpEvtFileClose,
                        WDF_NO_EVENT_CALLBACK // not interested in Cleanup
                        );

    WdfDeviceInitSetFileObjectConfig(DeviceInit,
                                       &fileConfig,
                                       WDF_NO_OBJECT_ATTRIBUTES);

    //
    // In order to support METHOD_NEITHER Device controls, or
    // NEITHER device I/O type, we need to register for the
    // EvtDeviceIoInProcessContext callback so that we can handle the request
    // in the calling threads context.
    //
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit,
                                    NonPnpEvtDeviceIoInCallerContext);

    //
    // Specify the size of device context
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,
                                    CONTROL_DEVICE_EXTENSION);

    status = WdfDeviceCreate(&DeviceInit,
                             &attributes,
                             &controlDevice);
    if (!NT_SUCCESS(status)) {
        KdPrint(( "WdfDeviceCreate failed %!STATUS!", status));
        goto End;
    }

    //
    // Create a symbolic link for the control object so that usermode can open
    // the device.
    //


    status = WdfDeviceCreateSymbolicLink(controlDevice,
                                &symbolicLinkName);

    if (!NT_SUCCESS(status)) {
        //
        // Control device will be deleted automatically by the framework.
        //
        KdPrint(( "WdfDeviceCreateSymbolicLink failed %!STATUS!", status));
        goto End;
    }

    //
    // Configure a default queue so that requests that are not
    // configure-fowarded using WdfDeviceConfigureRequestDispatching to goto
    // other queues get dispatched here.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig,
                                    WdfIoQueueDispatchSequential);

    ioQueueConfig.EvtIoRead = FileEvtIoRead;
    ioQueueConfig.EvtIoWrite = FileEvtIoWrite;
    ioQueueConfig.EvtIoDeviceControl = FileEvtIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    //
    // Since we are using Zw function set execution level to passive so that
    // framework ensures that our Io callbacks called at only passive-level
    // even if the request came in at DISPATCH_LEVEL from another driver.
    //
    //attributes.ExecutionLevel = WdfExecutionLevelPassive;

    //
    // By default, Static Driver Verifier (SDV) displays a warning if it 
    // doesn't find the EvtIoStop callback on a power-managed queue. 
    // The 'assume' below causes SDV to suppress this warning. If the driver 
    // has not explicitly set PowerManaged to WdfFalse, the framework creates
    // power-managed queues when the device is not a filter driver.  Normally 
    // the EvtIoStop is required for power-managed queues, but for this driver
    // it is not needed b/c the driver doesn't hold on to the requests or 
    // forward them to other drivers. This driver completes the requests 
    // directly in the queue's handlers. If the EvtIoStop callback is not 
    // implemented, the framework waits for all driver-owned requests to be
    // done before moving in the Dx/sleep states or before removing the 
    // device, which is the correct behavior for this type of driver.
    // If the requests were taking an indeterminate amount of time to complete,
    // or if the driver forwarded the requests to a lower driver/another stack,
    // the queue should have an EvtIoStop/EvtIoResume.
    //
    __analysis_assume(ioQueueConfig.EvtIoStop != 0);
    status = WdfIoQueueCreate(controlDevice,
                              &ioQueueConfig,
                              &attributes,
                              &queue // pointer to default queue
                              );
    __analysis_assume(ioQueueConfig.EvtIoStop == 0);
    if (!NT_SUCCESS(status)) {
        KdPrint(( "WdfIoQueueCreate failed %!STATUS!", status));
        goto End;
    }

    //
    // Control devices must notify WDF when they are done initializing.   I/O is
    // rejected until this call is made.
    //
    WdfControlFinishInitializing(controlDevice);

End:
    //
    // If the device is created successfully, framework would clear the
    // DeviceInit value. Otherwise device create must have failed so we
    // should free the memory ourself.
    //
    if (DeviceInit != NULL) {
        WdfDeviceInitFree(DeviceInit);
    }

    return status;

}

VOID
NonPnpEvtDriverContextCleanup(
    IN WDFOBJECT Driver
    )
/*++
Routine Description:

   Called when the driver object is deleted during driver unload.
   You can free all the resources created in DriverEntry that are
   not automatically freed by the framework.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

Return Value:

    NTSTATUS

--*/
{
    KdPrint(( "Entered NonPnpEvtDriverContextCleanup\n" ));
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();
}



VOID
NonPnpEvtDeviceFileCreate (
    IN WDFDEVICE            Device,
    IN WDFREQUEST Request,
    IN WDFFILEOBJECT        FileObject
    )
/*++

Routine Description:

    The framework calls a driver's EvtDeviceFileCreate callback
    when it receives an IRP_MJ_CREATE request.
    The system sends this request when a user application opens the
    device to perform an I/O operation, such as reading or writing a file.
    This callback is called synchronously, in the context of the thread
    that created the IRP_MJ_CREATE request.

Arguments:

    Device - Handle to a framework device object.
    FileObject - Pointer to fileobject that represents the open handle.
    CreateParams - Parameters of IO_STACK_LOCATION for create

Return Value:

   NT status code

--*/
{
    PUNICODE_STRING             fileName;
    UNICODE_STRING              absFileName, directory;
    OBJECT_ATTRIBUTES           fileAttributes;
    IO_STATUS_BLOCK             ioStatus;
    PCONTROL_DEVICE_EXTENSION   devExt;
    NTSTATUS                    status;
    USHORT                      length = 0;


    UNREFERENCED_PARAMETER( FileObject );

    PAGED_CODE ();

    devExt = ControlGetData(Device);

    //
    // Assume the directory is a temp directory under %windir%
    //
    RtlInitUnicodeString(&directory, L"\\SystemRoot\\temp");

    //
    // Parsed filename has "\" in the begining. The object manager strips
    // of all "\", except one, after the device name.
    //
    fileName = WdfFileObjectGetFileName(FileObject);

    KdPrint(( "NonPnpEvtDeviceFileCreate %wZ%wZ", &directory, fileName));

    //
    // Find the total length of the directory + filename
    //
    length = directory.Length + fileName->Length;

    absFileName.Buffer = ExAllocatePoolWithTag(PagedPool, length, POOL_TAG);
    if(absFileName.Buffer == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        KdPrint(( "ExAllocatePoolWithTag failed"));
        goto End;
    }
    absFileName.Length = 0;
    absFileName.MaximumLength =  length;

    status = RtlAppendUnicodeStringToString(&absFileName, &directory);
    if (!NT_SUCCESS(status)) {
        KdPrint(( "RtlAppendUnicodeStringToString failed with status %!STATUS!", status));
        goto End;
    }

    status = RtlAppendUnicodeStringToString(&absFileName, fileName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("RtlAppendUnicodeStringToString failed with status %!STATUS!", status));
        goto End;
    }

    KdPrint(( "Absolute Filename %wZ", &absFileName));

    InitializeObjectAttributes( &fileAttributes,
                                &absFileName,
                                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                NULL, // RootDirectory
                                NULL // SecurityDescriptor
                                );

    status = ZwCreateFile (
                    &devExt->FileHandle,
                    SYNCHRONIZE | GENERIC_WRITE | GENERIC_READ,
                    &fileAttributes,
                    &ioStatus,
                    NULL,// alloc size = none
                    FILE_ATTRIBUTE_NORMAL,
                    FILE_SHARE_READ,
                    FILE_OPEN_IF,
                    FILE_SYNCHRONOUS_IO_NONALERT |FILE_NON_DIRECTORY_FILE,
                    NULL,// eabuffer
                    0// ealength
                    );

    if (!NT_SUCCESS(status)) {

        KdPrint(( "ZwCreateFile failed with status %!STATUS!", status ));
        devExt->FileHandle = NULL;
    }

End:
    if(absFileName.Buffer != NULL) {
        ExFreePool(absFileName.Buffer);
    }

    WdfRequestComplete(Request, status);

    return;
}


VOID
NonPnpEvtFileClose (
    IN WDFFILEOBJECT    FileObject
    )

/*++

Routine Description:

   EvtFileClose is called when all the handles represented by the FileObject
   is closed and all the references to FileObject is removed. This callback
   may get called in an arbitrary thread context instead of the thread that
   called CloseHandle. If you want to delete any per FileObject context that
   must be done in the context of the user thread that made the Create call,
   you should do that in the EvtDeviceCleanp callback.

Arguments:

    FileObject - Pointer to fileobject that represents the open handle.

Return Value:

   VOID

--*/
{
    PCONTROL_DEVICE_EXTENSION devExt;

    PAGED_CODE ();


    KdPrint(( "NonPnpEvtFileClose\n"));

    devExt = ControlGetData(WdfFileObjectGetDevice(FileObject));

    if(devExt->FileHandle) {
        KdPrint(( "Closing File Handle %p", devExt->FileHandle));
        ZwClose(devExt->FileHandle);
    }

    return;
}


VOID
FileEvtIoRead(
    IN WDFQUEUE         Queue,
    IN WDFREQUEST       Request,
    IN size_t            Length
    )
/*++

Routine Description:

    This event is called when the framework receives IRP_MJ_READ requests.
    We will just read the file.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
            I/O request.
    Request - Handle to a framework request object.

    Length  - number of bytes to be read.
                   Queue is by default configured to fail zero length read & write requests.

Return Value:

  None.

--*/
{
    NTSTATUS                   status = STATUS_SUCCESS;
    PVOID                       outBuf;
    IO_STATUS_BLOCK             ioStatus;
    PCONTROL_DEVICE_EXTENSION   devExt;
    FILE_POSITION_INFORMATION   position;
    ULONG_PTR                   bytesRead = 0;
    size_t  bufLength;

    KdPrint(("FileEvtIoRead: Request: 0x%p, Queue: 0x%p\n", Request, Queue));

    PAGED_CODE ();

    //
    // Get the request buffer. Since the device is set to do buffered
    // I/O, this function will retrieve Irp->AssociatedIrp.SystemBuffer.
    //
    status = WdfRequestRetrieveOutputBuffer(Request, 0, &outBuf, &bufLength);
    if(!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;

    }

    devExt = ControlGetData(WdfIoQueueGetDevice(Queue));

    if(devExt->FileHandle) {

        //
        // Set the file position to the beginning of the file.
        //
        position.CurrentByteOffset.QuadPart = 0;
        status = ZwSetInformationFile(devExt->FileHandle,
                             &ioStatus,
                             &position,
                             sizeof(FILE_POSITION_INFORMATION),
                             FilePositionInformation);
        if (NT_SUCCESS(status)) {

            status = ZwReadFile (devExt->FileHandle,
                                NULL,//   Event,
                                NULL,// PIO_APC_ROUTINE  ApcRoutine
                                NULL,// PVOID  ApcContext
                                &ioStatus,
                                outBuf,
                                (ULONG)Length,
                                0, // ByteOffset
                                NULL // Key
                                );

            if (!NT_SUCCESS(status)) {

                KdPrint(("ZwReadFile failed with status 0x%x", status));
            }

            status = ioStatus.Status;
            bytesRead = ioStatus.Information;
        }
    }

    WdfRequestCompleteWithInformation(Request, status, bytesRead);

}



VOID
FileEvtIoWrite(
    IN WDFQUEUE         Queue,
    IN WDFREQUEST       Request,
    IN size_t            Length
    )
/*++

Routine Description:

    This event is called when the framework receives IRP_MJ_WRITE requests.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
            I/O request.
    Request - Handle to a framework request object.

    Length  - number of bytes to be written.
                   Queue is by default configured to fail zero length read & write requests.


Return Value:

   None
--*/
{
    NTSTATUS                   status = STATUS_SUCCESS;
    PVOID                       inBuf;
    IO_STATUS_BLOCK             ioStatus;
    PCONTROL_DEVICE_EXTENSION   devExt;
    FILE_POSITION_INFORMATION   position;
    ULONG_PTR                   bytesWritten = 0;
    size_t      bufLength;


    KdPrint(("FileEvtIoWrite: Request: 0x%p, Queue: 0x%p\n", Request, Queue));
    PAGED_CODE ();

    //
    // Get the request buffer. Since the device is set to do buffered
    // I/O, this function will retrieve Irp->AssociatedIrp.SystemBuffer.
    //
    status = WdfRequestRetrieveInputBuffer(Request, 0, &inBuf, &bufLength);
    if(!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;

    }

    devExt = ControlGetData(WdfIoQueueGetDevice(Queue));

    if(devExt->FileHandle) {

        //
        // Set the file position to the beginning of the file.
        //
        position.CurrentByteOffset.QuadPart = 0;

        status = ZwSetInformationFile(devExt->FileHandle,
                             &ioStatus,
                             &position,
                             sizeof(FILE_POSITION_INFORMATION),
                             FilePositionInformation);
        if (NT_SUCCESS(status))
        {

            status = ZwWriteFile(devExt->FileHandle,
                                NULL,//   Event,
                                NULL,// PIO_APC_ROUTINE  ApcRoutine
                                NULL,// PVOID  ApcContext
                                &ioStatus,
                                inBuf,
                                (ULONG)Length,
                                0, // ByteOffset
                                NULL // Key
                                );
            if (!NT_SUCCESS(status))
            {
                KdPrint(( "ZwWriteFile failed with status 0x%x", status));
            }

            status = ioStatus.Status;
            bytesWritten =  ioStatus.Information;
        }
    }

    WdfRequestCompleteWithInformation(Request, status, bytesWritten);

}

VOID
FileEvtIoDeviceControl(
    IN WDFQUEUE         Queue,
    IN WDFREQUEST       Request,
    IN size_t            OutputBufferLength,
    IN size_t            InputBufferLength,
    IN ULONG            IoControlCode
    )
/*++
Routine Description:

    This event is called when the framework receives IRP_MJ_DEVICE_CONTROL
    requests from the system.

Arguments:

    Queue - Handle to the framework queue object that is associated
            with the I/O request.
    Request - Handle to a framework request object.

    OutputBufferLength - length of the request's output buffer,
                        if an output buffer is available.
    InputBufferLength - length of the request's input buffer,
                        if an input buffer is available.

    IoControlCode - the driver-defined or system-defined I/O control code
                    (IOCTL) that is associated with the request.

Return Value:

   VOID

--*/
{
    NTSTATUS            status = STATUS_SUCCESS;// Assume success
    PCHAR               inBuf = NULL, outBuf = NULL; // pointer to Input and output buffer
    PCHAR               data = "this String is from Device Driver !!!";
    ULONG               datalen = (ULONG) strlen(data)+1;//Length of data including null
    PCHAR               buffer = NULL;
    PREQUEST_CONTEXT    reqContext = NULL;
    size_t               bufSize;

    UNREFERENCED_PARAMETER( Queue );

    PAGED_CODE();

    if(!OutputBufferLength || !InputBufferLength)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    //
    // Determine which I/O control code was specified.
    //

    switch (IoControlCode)
    {
    case IOCTL_NONPNP_METHOD_BUFFERED:


        KdPrint(("Called IOCTL_NONPNP_METHOD_BUFFERED\n"));

        //
        // For bufffered ioctls WdfRequestRetrieveInputBuffer &
        // WdfRequestRetrieveOutputBuffer return the same buffer
        // pointer (Irp->AssociatedIrp.SystemBuffer), so read the
        // content of the buffer before writing to it.
        //
        status = WdfRequestRetrieveInputBuffer(Request, 0, &inBuf, &bufSize);
        if(!NT_SUCCESS(status)) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        ASSERT(bufSize == InputBufferLength);

        //
        // Read the input buffer content.
        // We are using the following function to print characters instead
        // TraceEvents with %s format because the string we get may or
        // may not be null terminated. The buffer may contain non-printable
        // characters also.
        //
        //Hexdump((TRACE_LEVEL_VERBOSE,  DBG_IOCTL, "Data from User : %!HEXDUMP!\n",
        //                log_xstr(inBuf, (USHORT)InputBufferLength)));
        PrintChars(inBuf, InputBufferLength  );


        status = WdfRequestRetrieveOutputBuffer(Request, 0, &outBuf, &bufSize);
        if(!NT_SUCCESS(status)) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        ASSERT(bufSize == OutputBufferLength);

        //
        // Writing to the buffer over-writes the input buffer content
        //

        RtlCopyMemory(outBuf, data, OutputBufferLength);

//        Hexdump((TRACE_LEVEL_VERBOSE,  DBG_IOCTL, "Data to User : %!HEXDUMP!\n",
//                        log_xstr(outBuf, (USHORT)datalen)));
        PrintChars(outBuf, datalen  );

        //
        // Assign the length of the data copied to IoStatus.Information
        // of the request and complete the request.
        //
        WdfRequestSetInformation(Request,
                OutputBufferLength < datalen? OutputBufferLength:datalen);

        //
        // When the request is completed the content of the SystemBuffer
        // is copied to the User output buffer and the SystemBuffer is
        // is freed.
        //

       break;


    case IOCTL_NONPNP_METHOD_IN_DIRECT:


        KdPrint(("Called IOCTL_NONPNP_METHOD_IN_DIRECT\n"));

        //
        // Get the Input buffer. WdfRequestRetrieveInputBuffer returns
        // Irp->AssociatedIrp.SystemBuffer.
        //
        status = WdfRequestRetrieveInputBuffer(Request, 0, &inBuf, &bufSize);
        if(!NT_SUCCESS(status)) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        ASSERT(bufSize == InputBufferLength);

//        Hexdump((TRACE_LEVEL_VERBOSE,  DBG_IOCTL, "Data from User : %!HEXDUMP!\n",
//                        log_xstr(inBuf, (USHORT)InputBufferLength)));
        PrintChars(inBuf, InputBufferLength);

        //
        // Get the output buffer. Framework calls MmGetSystemAddressForMdlSafe
        // on the Irp->MdlAddress and returns the system address.
        // Oddity: For this method, this buffer is intended for transfering data
        // from the application to the driver.
        //

        status = WdfRequestRetrieveOutputBuffer(Request, 0, &buffer, &bufSize);
        if(!NT_SUCCESS(status)) {
            break;
        }

        ASSERT(bufSize == OutputBufferLength);

        //Hexdump((TRACE_LEVEL_VERBOSE,  DBG_IOCTL, "Data from User in OutputBuffer: %!HEXDUMP!\n",
        //                log_xstr(buffer, (USHORT)OutputBufferLength)));
        PrintChars(buffer, OutputBufferLength);

        //
        // Return total bytes read from the output buffer.
        // Note OutputBufferLength = MmGetMdlByteCount(Irp->MdlAddress)
        //

        WdfRequestSetInformation(Request, OutputBufferLength);

        //
        // NOTE: Changes made to the  SystemBuffer are not copied
        // to the user input buffer by the I/O manager
        //

      break;

    case IOCTL_NONPNP_METHOD_OUT_DIRECT:


        KdPrint(("Called IOCTL_NONPNP_METHOD_OUT_DIRECT\n"));

        //
        // Get the Input buffer. WdfRequestRetrieveInputBuffer returns
        // Irp->AssociatedIrp.SystemBuffer.
        //
        status = WdfRequestRetrieveInputBuffer(Request, 0, &inBuf, &bufSize);
        if(!NT_SUCCESS(status)) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        ASSERT(bufSize == InputBufferLength);

        //Hexdump((TRACE_LEVEL_VERBOSE,  DBG_IOCTL, "Data from User : %!HEXDUMP!\n",
        //                log_xstr(inBuf, (USHORT)InputBufferLength)));
        PrintChars(inBuf, InputBufferLength);

        //
        // Get the output buffer. Framework calls MmGetSystemAddressForMdlSafe
        // on the Irp->MdlAddress and returns the system address.
        // For this method, this buffer is intended for transfering data from the
        // driver to the application.
        //
        status = WdfRequestRetrieveOutputBuffer(Request, 0, &buffer, &bufSize);
        if(!NT_SUCCESS(status)) {
            break;
        }

        ASSERT(bufSize == OutputBufferLength);

        //
        // Write data to be sent to the user in this buffer
        //
        RtlCopyMemory(buffer, data, OutputBufferLength);

        //Hexdump((TRACE_LEVEL_VERBOSE,  DBG_IOCTL, "Data to User : %!HEXDUMP!\n",
        //                log_xstr(buffer, (USHORT)datalen)));
        PrintChars(buffer, datalen);

        WdfRequestSetInformation(Request,
                    OutputBufferLength < datalen? OutputBufferLength: datalen);

        //
        // NOTE: Changes made to the  SystemBuffer are not copied
        // to the user input buffer by the I/O manager
        //

        break;

    case IOCTL_NONPNP_METHOD_NEITHER:
        {
            size_t inBufLength, outBufLength;

            //
            // The NonPnpEvtDeviceIoInCallerContext has already probe and locked the
            // pages and mapped the user buffer into system address space and
            // stored memory buffer pointers in the request context. We can get the
            // buffer pointer by calling WdfMemoryGetBuffer.
            //
            KdPrint(("Called IOCTL_NONPNP_METHOD_NEITHER\n"));

            reqContext = GetRequestContext(Request);

            inBuf = WdfMemoryGetBuffer(reqContext->InputMemoryBuffer, &inBufLength);
            outBuf = WdfMemoryGetBuffer(reqContext->OutputMemoryBuffer, &outBufLength);

            if(inBuf == NULL || outBuf == NULL) {
                status = STATUS_INVALID_PARAMETER;
            }

            ASSERT(inBufLength == InputBufferLength);
            ASSERT(outBufLength == OutputBufferLength);

            //
            // Now you can safely read the data from the buffer in any arbitrary
            // context.
            //
            //Hexdump((TRACE_LEVEL_VERBOSE,  DBG_IOCTL, "Data from User : %!HEXDUMP!\n",
            //                log_xstr(inBuf, (USHORT)inBufLength)));
            PrintChars(inBuf, inBufLength);

            //
            // Write to the buffer in any arbitrary context.
            //
            RtlCopyMemory(outBuf, data, outBufLength);

            //Hexdump((TRACE_LEVEL_VERBOSE,  DBG_IOCTL, "Data to User : %!HEXDUMP!\n",
            //                log_xstr(outBuf, (USHORT)datalen)));
            PrintChars(outBuf, datalen);

            //
            // Assign the length of the data copied to IoStatus.Information
            // of the Irp and complete the Irp.
            //
            WdfRequestSetInformation(Request,
                    outBufLength < datalen? outBufLength:datalen);

            break;
        }
    default:

        //
        // The specified I/O control code is unrecognized by this driver.
        //
        status = STATUS_INVALID_DEVICE_REQUEST;
        KdPrint(("ERROR: unrecognized IOCTL %x\n", IoControlCode));
        break;
    }

    KdPrint(("Completing Request %p with status %X", Request, status ));

    WdfRequestComplete( Request, status);

}

VOID
NonPnpEvtDeviceIoInCallerContext(
    IN WDFDEVICE  Device,
    IN WDFREQUEST Request
    )
/*++
Routine Description:

    This I/O in-process callback is called in the calling threads context/address
    space before the request is subjected to any framework locking or queueing
    scheme based on the device pnp/power or locking attributes set by the
    driver. The process context of the calling app is guaranteed as long as
    this driver is a top-level driver and no other filter driver is attached
    to it.

    This callback is only required if you are handling method-neither IOCTLs,
    or want to process requests in the context of the calling process.

    Driver developers should avoid defining neither IOCTLs and access user
    buffers, and use much safer I/O tranfer methods such as buffered I/O
    or direct I/O.

Arguments:

    Device - Handle to a framework device object.

    Request - Handle to a framework request object. Framework calls
              PreProcess callback only for Read/Write/ioctls and internal
              ioctl requests.

Return Value:

    VOID

--*/
{
    NTSTATUS                   status = STATUS_SUCCESS;
    PREQUEST_CONTEXT            reqContext = NULL;
    WDF_OBJECT_ATTRIBUTES           attributes;
    WDF_REQUEST_PARAMETERS  params;
    size_t              inBufLen, outBufLen;
    PVOID              inBuf, outBuf;

    PAGED_CODE();

    WDF_REQUEST_PARAMETERS_INIT(&params);

    WdfRequestGetParameters(Request,  &params );

    KdPrint(("Entered NonPnpEvtDeviceIoInCallerContext %p \n", Request));

    //
    // Check to see whether we have recevied a METHOD_NEITHER IOCTL. if not
    // just send the request back to framework because we aren't doing
    // any pre-processing in the context of the calling thread process.
    //
    if(!(params.Type == WdfRequestTypeDeviceControl &&
            params.Parameters.DeviceIoControl.IoControlCode ==
                                    IOCTL_NONPNP_METHOD_NEITHER)) {
        //
        // Forward it for processing by the I/O package
        //
        status = WdfDeviceEnqueueRequest(Device, Request);
        if( !NT_SUCCESS(status) ) {
            KdPrint(("Error forwarding Request 0x%x",  status));
            goto End;
        }

        return;
    }

    KdPrint(("EvtIoPreProcess: received METHOD_NEITHER ioctl \n"));

    //
    // In this type of transfer, the I/O manager assigns the user input
    // to Type3InputBuffer and the output buffer to UserBuffer of the Irp.
    // The I/O manager doesn't copy or map the buffers to the kernel
    // buffers.
    //
    status = WdfRequestRetrieveUnsafeUserInputBuffer(Request, 0, &inBuf, &inBufLen);
    if(!NT_SUCCESS(status)) {
        KdPrint(("Error WdfRequestRetrieveUnsafeUserInputBuffer failed 0x%x",  status));
        goto End;
    }

    status = WdfRequestRetrieveUnsafeUserOutputBuffer(Request, 0, &outBuf, &outBufLen);
    if(!NT_SUCCESS(status)) {
       KdPrint((
                                    "Error WdfRequestRetrieveUnsafeUserOutputBuffer failed 0x%x",  status));
       goto End;
    }

    //
    // Allocate a context for this request so that we can store the memory
    // objects created for input and output buffer.
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, REQUEST_CONTEXT);

    status = WdfObjectAllocateContext(Request, &attributes, &reqContext);
    if(!NT_SUCCESS(status)) {
        KdPrint((
                                    "Error WdfObjectAllocateContext failed 0x%x",  status));
        goto End;
    }

    //
    // WdfRequestProbleAndLockForRead/Write function checks to see
    // whether the caller in the right thread context, creates an MDL,
    // probe and locks the pages, and map the MDL to system address
    // space and finally creates a WDFMEMORY object representing this
    // system buffer address. This memory object is associated with the
    // request. So it will be freed when the request is completed. If we
    // are accessing this memory buffer else where, we should store these
    // pointers in the request context.
    //

    #pragma prefast(suppress:6387, "If inBuf==NULL at this point, then inBufLen==0")    
    status = WdfRequestProbeAndLockUserBufferForRead(Request,
                            inBuf,
                            inBufLen,
                            &reqContext->InputMemoryBuffer);

    if(!NT_SUCCESS(status)) {
        KdPrint((
                                    "Error WdfRequestProbeAndLockUserBufferForRead failed 0x%x",  status));
        goto End;
    }

    #pragma prefast(suppress:6387, "If outBuf==NULL at this point, then outBufLen==0") 
    status = WdfRequestProbeAndLockUserBufferForWrite(Request,
                            outBuf,
                            outBufLen,
                            &reqContext->OutputMemoryBuffer);
    if(!NT_SUCCESS(status)) {
        KdPrint((
                                    "Error WdfRequestProbeAndLockUserBufferForWrite failed 0x%x",  status));
        goto End;
    }

    //
    // Finally forward it for processing by the I/O package
    //
    status = WdfDeviceEnqueueRequest(Device, Request);
    if(!NT_SUCCESS(status)) {
        KdPrint((
                                    "Error WdfDeviceEnqueueRequest failed 0x%x",  status));
        goto End;
    }

    return;

End:

    KdPrint(("EvtIoPreProcess failed %x \n", status));
    WdfRequestComplete(Request, status);
    return;
}

VOID
NonPnpShutdown(
    WDFDEVICE Device
    )
/*++

Routine Description:
    Callback invoked when the machine is shutting down.  If you register for
    a last chance shutdown notification you cannot do the following:
    o Call any pageable routines
    o Access pageable memory
    o Perform any file I/O operations

    If you register for a normal shutdown notification, all of these are
    available to you.

    This function implementation does nothing, but if you had any outstanding
    file handles open, this is where you would close them.

Arguments:
    Device - The device which registered the notification during init

Return Value:
    None

  --*/

{
    UNREFERENCED_PARAMETER(Device);
    return;
}


VOID
NonPnpEvtDriverUnload(
    IN WDFDRIVER Driver
    )
/*++
Routine Description:

   Called by the I/O subsystem just before unloading the driver.
   You can free the resources created in the DriverEntry either
   in this routine or in the EvtDriverContextCleanup callback.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

Return Value:

    NTSTATUS

--*/
{
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    KdPrint(( "Entered NonPnpDriverUnload\n"));

    return;
}

VOID
PrintChars(
    _In_reads_(CountChars) PCHAR BufferAddress,
    _In_ size_t CountChars
    )
{
    if (CountChars) {

        while (CountChars--) {

            if (*BufferAddress > 31
                 && *BufferAddress != 127) {

                KdPrint (( "%c", *BufferAddress) );

            } else {

                KdPrint(( ".") );

            }
            BufferAddress++;
        }
        KdPrint (("\n"));
    }
    return;
}

