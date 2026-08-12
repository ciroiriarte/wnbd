/*
 * Copyright (c) 2019 SUSE LLC
 * Copyright (c) 2026 Ciro Iriarte
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#ifndef SCSI_DRIVER_EXTENSIONS_H
#define SCSI_DRIVER_EXTENSIONS_H 1

#include "common.h"
#include "wnbd_ioctl.h"

// Number of hash buckets used to index submitted requests by tag. Must be a
// power of two; sized to comfortably cover the maximum per-LUN queue depth.
#define WNBD_SUBMITTED_REQ_HASH_BUCKETS 256
#define WNBD_SUBMITTED_REQ_HASH_MASK (WNBD_SUBMITTED_REQ_HASH_BUCKETS - 1)

// Hash index of devices by connection id, avoiding a full device-list scan on
// hot IOCTL lookups. Bucket count must be a power of two.
#define WNBD_CONN_ID_HASH_BUCKETS 64
#define WNBD_CONN_ID_HASH_MASK (WNBD_CONN_ID_HASH_BUCKETS - 1)
#define WNBD_CONN_ID_BUCKET(ConnId) ((ULONG)((ConnId) & WNBD_CONN_ID_HASH_MASK))

// Number of SRB queue elements pre-allocated per device into a bounded free
// list, sized to cover the default per-LUN queue depth. Requests beyond this
// fall back to direct nonpaged pool allocation.
#define WNBD_SRB_POOL_SIZE 256

typedef struct _WNBD_EXTENSION {
    UNICODE_STRING                    DeviceInterface;
    LIST_ENTRY                        DeviceList;
    KSPIN_LOCK                        DeviceListLock;
    // Connection-id-keyed index into DeviceList, guarded by DeviceListLock.
    LIST_ENTRY                        ConnIdHashBuckets[WNBD_CONN_ID_HASH_BUCKETS];
    LONG                              DeviceCount;
    ERESOURCE                         DeviceCreationLock;

    EX_RUNDOWN_REF                    RundownProtection;
    KEVENT                            GlobalDeviceRemovalEvent;
} WNBD_EXTENSION, *PWNBD_EXTENSION;

typedef struct _WNBD_DISK_DEVICE
{
    LIST_ENTRY                  ListEntry;
    // Links the device into a WNBD_EXTENSION ConnIdHashBuckets chain.
    LIST_ENTRY                  ConnIdHashLink;
    PWNBD_EXTENSION             DeviceExtension;

    BOOLEAN                     Connected;
    WNBD_PROPERTIES             Properties;
    WNBD_CONNECTION_ID          ConnectionId;

    USHORT                      Bus;
    USHORT                      Target;
    USHORT                      Lun;

    INT                         DiskNumber;
    WCHAR                       PNPDeviceID[WNBD_MAX_NAME_LENGTH];
    PDEVICE_OBJECT              PDO;

    PINQUIRYDATA                InquiryData;

    LIST_ENTRY                  PendingReqListHead;
    KSPIN_LOCK                  PendingReqListLock;

    LIST_ENTRY                  SubmittedReqListHead;
    KSPIN_LOCK                  SubmittedReqListLock;
    // Hash buckets indexed by request tag, guarded by SubmittedReqListLock.
    // Provides O(1) completion lookup instead of scanning the whole submitted
    // list. Kept in sync with SubmittedReqListHead.
    LIST_ENTRY                  SubmittedReqHashBuckets[WNBD_SUBMITTED_REQ_HASH_BUCKETS];

    // Bounded free list of pre-allocated SRB queue elements, avoiding nonpaged
    // pool churn on the hot dispatch/completion path. Guarded by its own lock.
    LIST_ENTRY                  SrbElementPool;
    KSPIN_LOCK                  SrbElementPoolLock;

    KSEMAPHORE                  DeviceEvent;
    PVOID                       DeviceMonitorThread;
    BOOLEAN                     HardRemoveDevice;
    KEVENT                      DeviceRemovalEvent;
    // The rundown protection provides device reference counting, preventing
    // it from being deallocated while still being accessed. This is
    // especially important for IO dispatching.
    EX_RUNDOWN_REF              RundownProtection;

    WNBD_DRV_STATS              Stats;
} WNBD_DISK_DEVICE, *PWNBD_DISK_DEVICE;

typedef struct _SRB_QUEUE_ELEMENT {
    LIST_ENTRY Link;
    // Links the element into a SubmittedReqHashBuckets chain while submitted.
    LIST_ENTRY HashLink;
    PVOID Srb;
    UINT64 StartingLbn;
    ULONG DataLength;
    BOOLEAN FUA;
    PVOID DeviceExtension;
    UINT64 Tag;
    BOOLEAN Aborted;
    BOOLEAN Completed;
    // TRUE if this element came from the device's pre-allocated pool and must
    // be returned to it rather than freed to nonpaged pool.
    BOOLEAN Pooled;
    // Retrieved using KeQueryInterruptTime.
    UINT64 ReqTimestamp;
} SRB_QUEUE_ELEMENT, * PSRB_QUEUE_ELEMENT;

SCSI_ADAPTER_CONTROL_STATUS
WnbdHwAdapterControl(_In_ PVOID DeviceExtension,
                     _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
                     _In_ PVOID Parameters);

ULONG
WnbdHwFindAdapter(_In_ PVOID DeviceExtension,
                  _In_ PVOID HwContext,
                  _In_ PVOID BusInformation,
                  _In_ PVOID LowerDevice,
                  _In_ PCHAR ArgumentString,
                  _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                  _In_ PBOOLEAN Again);

VOID
WnbdHwFreeAdapterResources(_In_ PVOID DeviceExtension);

BOOLEAN
WnbdHwInitialize(_In_ PVOID DeviceExtension);

VOID
WnbdHwProcessServiceRequest(_In_ PVOID DeviceExtension,
                            _In_ PVOID Irp);

BOOLEAN
WnbdHwResetBus(_In_ PVOID DeviceExtension,
               _In_ ULONG PathId);

BOOLEAN
WnbdHwStartIo(_In_ PVOID PDevExt,
              _In_ PVOID PSrb);

#endif
