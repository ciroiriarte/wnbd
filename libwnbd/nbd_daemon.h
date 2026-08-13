/*
 * Copyright (C) 2023 Cloudbase Solutions
 * Copyright (c) 2026 Ciro Iriarte
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>

#include "nbd_protocol.h"
#include "wnbd_log.h"

// Minimal information to identify pending requests.
struct PendingRequestInfo
{
    UINT64 RequestHandle;
    WnbdRequestType RequestType;
    UINT32 Length;
};

// Serializes outbound NBD frames so the dispatcher threads cannot interleave a
// request's header and payload on the shared socket. Normally a real mutex.
//
// WNBD_TEST_DISABLE_SENDLOCK swaps in a no-op lockable so the concurrency soak
// test (TestNbdSoak.*) can be run as a negative control that proves it detects
// missing frame serialization. It MUST NEVER be defined in a shipping build.
#ifdef WNBD_TEST_DISABLE_SENDLOCK
#pragma message("WNBD_TEST_DISABLE_SENDLOCK set: NBD SendLock is a no-op (test builds only).")
struct WnbdSendLock
{
    void lock() {}
    void unlock() {}
    bool try_lock() { return true; }
};
#else
using WnbdSendLock = std::mutex;
#endif

class NbdDaemon
{
private:
    WNBD_PROPERTIES WnbdProps = {0};

    SOCKET Socket = INVALID_SOCKET;
    bool NbdTransmissionStarted = false;

    std::mutex ShutdownLock;
    WnbdSendLock SendLock;
    bool Terminated = false;
    bool TerminateInProgress = false;
    PWNBD_DISK WnbdDisk = nullptr;

    void* PreallocatedRBuff = nullptr;
    ULONG PreallocatedRBuffSz = 0;

    // NBD replies provide limited information. We need to track
    // the request size on our own in order to know how large is
    // the data buffer that follows the reply header.
    std::unordered_map<UINT64, PendingRequestInfo> PendingRequests;
    std::mutex PendingRequestsLock;

    std::thread ReplyDispatcher;

public:
    NbdDaemon(PWNBD_PROPERTIES Properties)
    {
        WnbdProps = *Properties;
    }

    ~NbdDaemon()
    {
        Shutdown();
        Wait();

        if (ReplyDispatcher.joinable()) {
            LogInfo("Waiting for NBD reply dispatcher thread.");
            ReplyDispatcher.join();
            LogInfo("NBD reply dispatcher stopped.");
        }

        if (PreallocatedRBuff) {
            free(PreallocatedRBuff);
        }

        if (WnbdDisk) {
            WnbdClose(WnbdDisk);
        }
    }

    DWORD Start();
    DWORD Wait();
    DWORD Shutdown(bool HardRemove=false);

private:
    DWORD TryStart();
    DWORD ConnectNbdServer(
        std::string HostName,
        uint32_t PortNumber);
    DWORD DisconnectNbd();

    void NbdReplyWorker();
    DWORD ProcessNbdReply(LPOVERLAPPED Overlapped);

    // WNBD IO entry points
    static void Read(
        PWNBD_DISK Disk,
        UINT64 RequestHandle,
        PVOID Buffer,
        UINT64 BlockAddress,
        UINT32 BlockCount,
        BOOLEAN ForceUnitAccess);
    static void Write(
        PWNBD_DISK Disk,
        UINT64 RequestHandle,
        PVOID Buffer,
        UINT64 BlockAddress,
        UINT32 BlockCount,
        BOOLEAN ForceUnitAccess);
    static void Flush(
        PWNBD_DISK Disk,
        UINT64 RequestHandle,
        UINT64 BlockAddress,
        UINT32 BlockCount);
    static void Unmap(
        PWNBD_DISK Disk,
        UINT64 RequestHandle,
        PWNBD_UNMAP_DESCRIPTOR Descriptors,
        UINT32 Count);

    static constexpr WNBD_INTERFACE WnbdInterface =
    {
        Read,
        Write,
        Flush,
        Unmap,
    };
};