/*
 * Copyright (C) 2023 Cloudbase Solutions
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "pch.h"
#include "mock_wnbd_daemon.h"
#include "utils.h"
#include "options.h"
#include "test_skip.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace std;

void FillNbdProperties(PWNBD_PROPERTIES WnbdProps, std::string& InstanceName)
{
    string NbdExportName = GetOpt<string>("nbd-export-name");
    string NbdHostName = GetOpt<string>("nbd-hostname");
    DWORD NbdPort = GetOpt<DWORD>("nbd-port");

    if (NbdExportName.empty()) {
        throw runtime_error("missing NBD export");
    }
    if (NbdHostName.empty()) {
        throw runtime_error("missing NBD server address");
    }

    InstanceName = GetNewInstanceName();
    InstanceName.copy(WnbdProps->InstanceName, WNBD_MAX_NAME_LENGTH);
    string(WNBD_OWNER_NAME).copy(WnbdProps->Owner, WNBD_MAX_OWNER_LENGTH);

    WnbdProps->Flags.UseUserspaceNbd = 1;

    NbdHostName.copy(WnbdProps->NbdProperties.Hostname,
                     WNBD_MAX_NAME_LENGTH);
    NbdExportName.copy(WnbdProps->NbdProperties.ExportName,
                       WNBD_MAX_NAME_LENGTH);
    WnbdProps->NbdProperties.PortNumber = NbdPort;
}

class NbdMapping {
private:
    std::thread NbdDaemonThread;
    std::string InstanceName;

public:
    NbdMapping(PWNBD_PROPERTIES WnbdProps) {
        FillNbdProperties(WnbdProps, InstanceName);

        NbdDaemonThread = std::thread([&]{
            WNBD_PROPERTIES Props = *WnbdProps;
            WnbdRunNbdDaemon(&Props);
        });

        // Wait for the disk to become available.
        GetDiskPath(InstanceName.c_str(), false);
    }

    ~NbdMapping() {
        WNBD_REMOVE_OPTIONS RemoveOptions = { 0 };
        RemoveOptions.Flags.HardRemove = 1;
        WnbdRemoveEx(InstanceName.c_str(), &RemoveOptions);

        if (NbdDaemonThread.joinable()) {
            cout << "Waiting for NBD daemon thread." << endl;
            NbdDaemonThread.join();
            cout << "Nbd daemon stopped." << endl;
        }
    }

    const std::string& GetInstanceName() const {
        return InstanceName;
    }
};

HANDLE OpenNbdDisk(
    const std::string& InstanceName,
    DWORD DesiredAccess = GENERIC_READ | GENERIC_WRITE)
{
    string DiskPath = GetDiskPath(InstanceName.c_str());
    HANDLE DiskHandle = CreateFileA(
        DiskPath.c_str(),
        DesiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL);
    return DiskHandle;
}

void WriteSingleNbdBlock(HANDLE DiskHandle, DWORD BlockSize)
{
    unique_ptr<void, decltype(&free)> WriteBuffer(malloc(BlockSize), free);
    ASSERT_TRUE(WriteBuffer.get()) << "couldn't allocate: " << BlockSize;
    memset(WriteBuffer.get(), 0x5a, BlockSize);

    DWORD BytesWritten = 0;
    ASSERT_TRUE(WriteFile(DiskHandle, WriteBuffer.get(), BlockSize,
                          &BytesWritten, NULL))
        << "write failed: " << WinStrError(GetLastError());
    ASSERT_EQ(BlockSize, BytesWritten);

    LARGE_INTEGER Offset = { 0 };
    ASSERT_TRUE(SetFilePointerEx(DiskHandle, Offset, NULL, FILE_BEGIN));
}

bool WaitForFile(const char* Path, DWORD TimeoutMs)
{
    DWORD RemainingMs = TimeoutMs;
    while (RemainingMs) {
        DWORD Attributes = GetFileAttributesA(Path);
        if (Attributes != INVALID_FILE_ATTRIBUTES &&
                !(Attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return true;
        }
        Sleep(100);
        RemainingMs = RemainingMs > 100 ? RemainingMs - 100 : 0;
    }
    return false;
}

const char* GetMissingNbdParamReason() {
    if (GetOpt<string>("nbd-export-name").empty()) {
        return "No NBD export provided, skipping NBD tests.";
    }
    if (GetOpt<string>("nbd-hostname").empty()) {
        return "No NBD server address provided, skipping NBD tests.";
    }
    return nullptr;
}


TEST(TestNbd, TestMap) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    WNBD_PROPERTIES WnbdProps = { 0 };
    WNBD_CONNECTION_INFO ConnectionInfo = { 0 };

    {
        NbdMapping Mapping(&WnbdProps);

        NTSTATUS Status = WnbdShow(WnbdProps.InstanceName, &ConnectionInfo);
        ASSERT_FALSE(Status) << "couldn't retrieve WNBD disk info";

        EXPECT_EQ(
            WnbdProps.InstanceName,
            string(ConnectionInfo.Properties.InstanceName));
        EXPECT_EQ(
            WnbdProps.InstanceName,
            string(ConnectionInfo.Properties.SerialNumber));
        EXPECT_EQ(
            string(WNBD_OWNER_NAME),
            string(ConnectionInfo.Properties.Owner));
        EXPECT_LT(0UL, ConnectionInfo.Properties.BlockCount);
        EXPECT_LT(0ULL, ConnectionInfo.Properties.BlockSize);
        EXPECT_EQ(_getpid(), ConnectionInfo.Properties.Pid);

        EXPECT_TRUE(ConnectionInfo.Properties.Flags.UseUserspaceNbd);
    }

    // The mapping went out of scope, let's ensure that it got
    // disconnected.
    NTSTATUS Status = WnbdShow(WnbdProps.InstanceName, &ConnectionInfo);
    EXPECT_EQ(ERROR_FILE_NOT_FOUND, Status);
}


TEST(TestNbd, TestFlush) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    WNBD_PROPERTIES WnbdProps = { 0 };
    NbdMapping Mapping(&WnbdProps);

    string DiskPath = GetDiskPath(WnbdProps.InstanceName);
    HANDLE DiskHandle = CreateFileA(
        DiskPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL);
    ASSERT_NE(INVALID_HANDLE_VALUE, DiskHandle)
        << "couldn't open disk: " << DiskPath
        << ", error: " << WinStrError(GetLastError());
    unique_ptr<void, decltype(&CloseHandle)> DiskHandleCloser(
        DiskHandle, &CloseHandle);

    ASSERT_TRUE(FlushFileBuffers(DiskHandle))
        << "flush failed: " << WinStrError(GetLastError());
}


TEST(TestNbdHandshakeFailures, NegotiationFailureReturnsError) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    WNBD_PROPERTIES WnbdProps = { 0 };
    std::string InstanceName;
    FillNbdProperties(&WnbdProps, InstanceName);

    DWORD Status = WnbdRunNbdDaemon(&WnbdProps);
    EXPECT_NE(0UL, Status) << "malformed NBD negotiation unexpectedly mapped";
}

TEST(TestNbdFailures, ReadFailureAfterWriteCompletes) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    WNBD_PROPERTIES WnbdProps = { 0 };
    NbdMapping Mapping(&WnbdProps);

    WNBD_CONNECTION_INFO ConnectionInfo = { 0 };
    NTSTATUS Status = WnbdShow(WnbdProps.InstanceName, &ConnectionInfo);
    ASSERT_FALSE(Status) << "couldn't retrieve WNBD disk info";
    const DWORD BlockSize = ConnectionInfo.Properties.BlockSize;
    ASSERT_LT(0UL, BlockSize);

    HANDLE DiskHandle = OpenNbdDisk(Mapping.GetInstanceName());
    ASSERT_NE(INVALID_HANDLE_VALUE, DiskHandle)
        << "couldn't open disk, error: " << WinStrError(GetLastError());
    unique_ptr<void, decltype(&CloseHandle)> DiskHandleCloser(
        DiskHandle, &CloseHandle);
    SetDiskWritable(DiskHandle);

    WriteSingleNbdBlock(DiskHandle, BlockSize);

    unique_ptr<void, decltype(&free)> ReadBuffer(malloc(BlockSize), free);
    ASSERT_TRUE(ReadBuffer.get()) << "couldn't allocate: " << BlockSize;
    DWORD BytesRead = 0;
    ASSERT_FALSE(ReadFile(DiskHandle, ReadBuffer.get(), BlockSize,
                          &BytesRead, NULL))
        << "read unexpectedly succeeded";
}

TEST(TestNbdFailures, RemovalWhileReadIsStalledCompletes) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    WNBD_PROPERTIES WnbdProps = { 0 };
    auto Mapping = make_unique<NbdMapping>(&WnbdProps);

    WNBD_CONNECTION_INFO ConnectionInfo = { 0 };
    NTSTATUS Status = WnbdShow(WnbdProps.InstanceName, &ConnectionInfo);
    ASSERT_FALSE(Status) << "couldn't retrieve WNBD disk info";
    const DWORD BlockSize = ConnectionInfo.Properties.BlockSize;
    ASSERT_LT(0UL, BlockSize);

    HANDLE DiskHandle = OpenNbdDisk(Mapping->GetInstanceName());
    ASSERT_NE(INVALID_HANDLE_VALUE, DiskHandle)
        << "couldn't open disk, error: " << WinStrError(GetLastError());
    unique_ptr<void, decltype(&CloseHandle)> DiskHandleCloser(
        DiskHandle, &CloseHandle);
    SetDiskWritable(DiskHandle);

    WriteSingleNbdBlock(DiskHandle, BlockSize);

    const int ReadCount = 4;
    std::vector<std::unique_ptr<void, decltype(&free)>> ReadBuffers;
    std::vector<HANDLE> ReadHandles;
    std::atomic<int> ReadFinished { 0 };
    std::vector<BOOL> ReadSucceeded(ReadCount, TRUE);
    std::vector<DWORD> BytesRead(ReadCount, 0);
    std::vector<std::thread> ReadThreads;

    for (int Index = 0; Index < ReadCount; ++Index) {
        ReadBuffers.emplace_back(malloc(BlockSize), free);
        ASSERT_TRUE(ReadBuffers.back().get())
            << "couldn't allocate: " << BlockSize;
        HANDLE ReadHandle = OpenNbdDisk(Mapping->GetInstanceName(), GENERIC_READ);
        ASSERT_NE(INVALID_HANDLE_VALUE, ReadHandle)
            << "couldn't open read handle, error: " << WinStrError(GetLastError());
        ReadHandles.push_back(ReadHandle);

        ReadThreads.emplace_back([&, Index] {
            ReadSucceeded[Index] = ReadFile(ReadHandles[Index],
                                            ReadBuffers[Index].get(),
                                            BlockSize,
                                            &BytesRead[Index],
                                            NULL);
            ++ReadFinished;
        });
    }

    const char* StallMarker = getenv("WNBD_STALL_MARKER");
    ASSERT_TRUE(StallMarker && StallMarker[0])
        << "WNBD_STALL_MARKER must be set for deterministic stall tests";
    ASSERT_TRUE(WaitForFile(StallMarker, 10000))
        << "fake server did not confirm a stalled read";

    Mapping.reset();

    EVENTUALLY(ReadFinished.load() == ReadCount, 40, 250);
    for (auto& Thread: ReadThreads) {
        Thread.join();
    }
    for (HANDLE ReadHandle: ReadHandles) {
        CloseHandle(ReadHandle);
    }
    for (int Index = 0; Index < ReadCount; ++Index) {
        EXPECT_FALSE(ReadSucceeded[Index])
            << "stalled read " << Index << " unexpectedly succeeded";
    }

    WNBD_CONNECTION_INFO RemovedInfo = { 0 };
    EXPECT_EQ(ERROR_FILE_NOT_FOUND,
              WnbdShow(WnbdProps.InstanceName, &RemovedInfo));
}

TEST(TestNbd, TestIO) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    // 1. Connect the NBD disk and retrieve connection information
    // -----------------------------------------------------------
    WNBD_PROPERTIES WnbdProps = { 0 };
    NbdMapping Mapping(&WnbdProps);

    WNBD_CONNECTION_INFO ConnectionInfo = { 0 };
    NTSTATUS Status = WnbdShow(WnbdProps.InstanceName, &ConnectionInfo);
    ASSERT_FALSE(Status) << "couldn't retrieve WNBD disk info";

    ASSERT_LT(0ULL, ConnectionInfo.Properties.BlockCount);
    EXPECT_LT(0UL, ConnectionInfo.Properties.BlockSize);

    UINT64 BlockCount = ConnectionInfo.Properties.BlockCount;
    UINT32 BlockSize = ConnectionInfo.Properties.BlockSize;

    string DiskPath = GetDiskPath(WnbdProps.InstanceName);
    DWORD OpenFlags = FILE_ATTRIBUTE_NORMAL |
                      FILE_FLAG_NO_BUFFERING |
                      FILE_FLAG_WRITE_THROUGH;
    HANDLE DiskHandle = CreateFileA(
        DiskPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        OpenFlags,
        NULL);
    ASSERT_NE(INVALID_HANDLE_VALUE, DiskHandle)
        << "couldn't open disk: " << DiskPath
        << ", error: " << WinStrError(GetLastError());
    unique_ptr<void, decltype(&CloseHandle)> DiskHandleCloser(
        DiskHandle, &CloseHandle);
    SetDiskWritable(DiskHandle);

    // 2. Write one block at the beginning of the disk and then read it
    // ----------------------------------------------------------------
    int WriteBufferSize = 4 << 20;
    static_assert(4 << 20 == 2 * WNBD_DEFAULT_MAX_TRANSFER_LENGTH);

    unique_ptr<void, decltype(&free)> WriteBuffer(
        malloc(WriteBufferSize), free);
    ASSERT_TRUE(WriteBuffer.get()) << "couldn't allocate: " << WriteBufferSize;

    unsigned int Rand;
    EXPECT_EQ(0, rand_s(&Rand));
    memset(WriteBuffer.get(), Rand, WriteBufferSize);

    int ReadBufferSize = 4 << 20;
    unique_ptr<void, decltype(&free)> ReadBuffer(
        malloc(ReadBufferSize), free);
    ASSERT_TRUE(ReadBuffer.get()) << "couldn't allocate: " << ReadBufferSize;

    DWORD BytesWritten = 0;
    // 1 block
    ASSERT_TRUE(WriteFile(
        DiskHandle, WriteBuffer.get(),
        BlockSize, &BytesWritten, NULL));
    ASSERT_EQ(BlockSize, BytesWritten);

    LARGE_INTEGER Offset = { 0 };
    ASSERT_TRUE(SetFilePointerEx(DiskHandle, Offset, NULL, FILE_BEGIN));

    DWORD BytesRead = 0;
    // Clear the read buffer
    memset(ReadBuffer.get(), 0, ReadBufferSize);
    ASSERT_TRUE(ReadFile(
        DiskHandle, ReadBuffer.get(),
        BlockSize, &BytesRead, NULL));
    ASSERT_EQ(BlockSize, BytesRead);
    ASSERT_FALSE(memcmp(ReadBuffer.get(), WriteBuffer.get(),
                 BytesRead));

    // 3. Write twice the maximum transfer length and then verify the content
    // ----------------------------------------------------------------------
    EXPECT_EQ(0, rand_s(&Rand));
    memset(WriteBuffer.get(), Rand, WriteBufferSize);

    Offset.QuadPart = BlockSize;
    ASSERT_TRUE(SetFilePointerEx(DiskHandle, Offset, NULL, FILE_BEGIN));

    ASSERT_TRUE(WriteFile(
        DiskHandle, WriteBuffer.get(),
        2 * WNBD_DEFAULT_MAX_TRANSFER_LENGTH,
        &BytesWritten, NULL));
    ASSERT_EQ(2 * WNBD_DEFAULT_MAX_TRANSFER_LENGTH, BytesWritten);

    Offset.QuadPart = BlockSize;
    ASSERT_TRUE(SetFilePointerEx(DiskHandle, Offset, NULL, FILE_BEGIN));
    memset(ReadBuffer.get(), 0, ReadBufferSize);
    ASSERT_TRUE(ReadFile(
        DiskHandle, ReadBuffer.get(),
        2 * WNBD_DEFAULT_MAX_TRANSFER_LENGTH, &BytesRead, NULL));
    ASSERT_EQ(2 * WNBD_DEFAULT_MAX_TRANSFER_LENGTH, BytesRead);
    ASSERT_FALSE(memcmp(ReadBuffer.get(), WriteBuffer.get(),
                 BytesRead));

    // 4. Write one block at the end of the disk and verify the content
    // ----------------------------------------------------------------
    EXPECT_EQ(0, rand_s(&Rand));
    memset(WriteBuffer.get(), Rand, WriteBufferSize);

    Offset.QuadPart = (BlockCount - 1) * BlockSize;
    ASSERT_TRUE(SetFilePointerEx(DiskHandle, Offset, NULL, FILE_BEGIN));
    ASSERT_TRUE(WriteFile(
        DiskHandle, WriteBuffer.get(),
        BlockSize, &BytesWritten, NULL));
    ASSERT_EQ(BlockSize, BytesWritten);

    Offset.QuadPart = (BlockCount - 1) * BlockSize;
    ASSERT_TRUE(SetFilePointerEx(DiskHandle, Offset, NULL, FILE_BEGIN));
    memset(ReadBuffer.get(), 0, ReadBufferSize);
    ASSERT_TRUE(ReadFile(
        DiskHandle, ReadBuffer.get(),
        BlockSize, &BytesRead, NULL));
    ASSERT_EQ(BlockSize, BytesRead);
    ASSERT_FALSE(memcmp(ReadBuffer.get(), WriteBuffer.get(),
                 BytesRead));

    // 5. Read/Write past the end of the disk, expecting a failure.
    // --------------------------------------------------------------
    Offset.QuadPart = BlockCount * BlockSize;
    ASSERT_TRUE(SetFilePointerEx(DiskHandle, Offset, NULL, FILE_BEGIN));

    ASSERT_FALSE(ReadFile(
        DiskHandle, ReadBuffer.get(),
        BlockSize, &BytesRead, NULL));
    ASSERT_EQ(0, BytesRead);

    ASSERT_FALSE(WriteFile(
        DiskHandle, WriteBuffer.get(),
        BlockSize, &BytesWritten, NULL));
    ASSERT_EQ(0, BytesWritten);

    ASSERT_TRUE(FlushFileBuffers(DiskHandle));
}
