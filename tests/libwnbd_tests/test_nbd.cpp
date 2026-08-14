/*
 * Copyright (C) 2023 Cloudbase Solutions
 * Copyright (c) 2026 Ciro Iriarte
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "pch.h"
#include "mock_wnbd_daemon.h"
#include "utils.h"
#include "options.h"
#include "test_skip.h"

#include <atomic>
#include <chrono>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <stdexcept>
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

// ---------------------------------------------------------------------------
// Concurrency soak tests (TestNbdSoak.*)
//
// These drive the driver plus the single per-mapping userspace NBD daemon under
// sustained, overlapping multi-threaded I/O. Every worker handle funnels
// through the same device and therefore the same NBD connection, so concurrent
// I/O exercises exactly the fork's hot-path concurrency changes:
//   - the by-tag submitted-request completion hash: a request delivered to the
//     wrong outstanding buffer shows up as a read-back mismatch. Bucket
//     collisions require the monotonic tag counter to wrap 256 past a still
//     outstanding request, which only happens under continuous churn -- hence
//     a time-bounded soak rather than a single burst.
//   - the userspace SendLock: it only matters when multiple dispatcher threads
//     send concurrently on the shared socket. The daemon uses one dispatcher by
//     default, so SendLock is exercised only when the mapping is created with
//     WNBD_DISPATCHER_THREADS>1; then a missing lock interleaves NBD
//     header/payload bytes on the wire, surfacing as I/O errors or corrupted
//     block contents. (Proven by the sendlock-negative-control CI job.)
//
// Excluded from the default functional run and driven as a dedicated,
// time-bounded CI step. Tunable via env: WNBD_SOAK_SECONDS, WNBD_SOAK_THREADS,
// WNBD_DISPATCHER_THREADS.
// Unbuffered, write-through I/O is used so each request reaches the driver
// instead of being served from the Windows cache.

static DWORD SoakEnvDword(const char* Name, DWORD Default) {
    string Value = GetEnv(Name);
    if (Value.empty()) {
        return Default;
    }
    try {
        unsigned long Parsed = std::stoul(Value);
        return Parsed ? (DWORD)Parsed : Default;
    } catch (const std::exception&) {
        return Default;
    }
}

static HANDLE OpenSoakDisk(const string& InstanceName) {
    string DiskPath = GetDiskPath(InstanceName.c_str());
    return CreateFileA(
        DiskPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        NULL);
}

// Positioned synchronous I/O: passing an OVERLAPPED to a synchronous handle
// performs the transfer at the given (block-aligned) byte offset and blocks
// until it completes. Returns TRUE only on a full-length transfer.
static BOOL SoakPositionedIo(
    BOOL Write, HANDLE Handle, void* Buffer, DWORD Length, UINT64 ByteOffset)
{
    OVERLAPPED Overlapped = { 0 };
    Overlapped.Offset = (DWORD)(ByteOffset & 0xFFFFFFFFULL);
    Overlapped.OffsetHigh = (DWORD)(ByteOffset >> 32);
    DWORD Transferred = 0;
    BOOL Ok = Write
        ? WriteFile(Handle, Buffer, Length, &Transferred, &Overlapped)
        : ReadFile(Handle, Buffer, Length, &Transferred, &Overlapped);
    return Ok && Transferred == Length;
}

// Pattern A: every worker owns a disjoint, well-separated region and repeatedly
// writes self-describing content then reads it straight back. Because no two
// workers touch the same block, a correct system must always read back exactly
// what was written; any mismatch is data corruption (wrong-buffer completion,
// torn frame, or a stale/foreign reply). This is the deterministic oracle.
TEST(TestNbdSoak, DisjointRegionsNoCorruption) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    WNBD_PROPERTIES WnbdProps = { 0 };
    NbdMapping Mapping(&WnbdProps);

    WNBD_CONNECTION_INFO ConnectionInfo = { 0 };
    ASSERT_FALSE(WnbdShow(WnbdProps.InstanceName, &ConnectionInfo))
        << "couldn't retrieve WNBD disk info";
    UINT64 BlockCount = ConnectionInfo.Properties.BlockCount;
    UINT32 BlockSize = ConnectionInfo.Properties.BlockSize;
    ASSERT_LT(0ULL, BlockCount);
    ASSERT_LT(0UL, BlockSize);
    SetDiskWritable(string(WnbdProps.InstanceName));

    const DWORD Threads = SoakEnvDword("WNBD_SOAK_THREADS", 32);
    const DWORD Seconds = SoakEnvDword("WNBD_SOAK_SECONDS", 20);
    const UINT64 RegionBlocks = 64;

    // Spread each worker's region across the device so regions are disjoint and
    // non-adjacent.
    UINT64 StrideBlocks = BlockCount / ((UINT64)Threads + 2);
    ASSERT_GE(StrideBlocks, RegionBlocks) << "disk too small for the soak layout";

    std::atomic<uint64_t> Corruptions{ 0 };
    std::atomic<uint64_t> IoErrors{ 0 };
    std::atomic<uint64_t> TotalOps{ 0 };
    std::mutex FirstFailureLock;
    string FirstFailure;
    auto Record = [&](const string& Message) {
        std::lock_guard<std::mutex> Guard(FirstFailureLock);
        if (FirstFailure.empty()) FirstFailure = Message;
    };

    auto Deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(Seconds);

    auto Worker = [&](DWORD WorkerId) {
        HANDLE Handle = OpenSoakDisk(Mapping.GetInstanceName());
        if (Handle == INVALID_HANDLE_VALUE) {
            IoErrors++;
            Record("worker " + std::to_string(WorkerId) + " open failed: " +
                   WinStrError(GetLastError()));
            return;
        }
        unique_ptr<void, decltype(&CloseHandle)> HandleCloser(
            Handle, &CloseHandle);

        unique_ptr<void, decltype(&_aligned_free)> WriteBuf(
            _aligned_malloc(BlockSize, BlockSize), &_aligned_free);
        unique_ptr<void, decltype(&_aligned_free)> ReadBuf(
            _aligned_malloc(BlockSize, BlockSize), &_aligned_free);
        if (!WriteBuf.get() || !ReadBuf.get()) {
            IoErrors++;
            Record("worker " + std::to_string(WorkerId) + " alloc failed");
            return;
        }

        UINT64 RegionBase = (UINT64)WorkerId * StrideBlocks;
        uint64_t Seq = 0;
        while (std::chrono::steady_clock::now() < Deadline) {
            UINT64 Block = RegionBase + (Seq % RegionBlocks);
            UINT64 ByteOffset = Block * BlockSize;

            // Self-describing: a stale read or a foreign reply both mismatch.
            uint64_t Tag = ((uint64_t)WorkerId << 48) ^ (Block << 16) ^
                           (Seq & 0xFFFF);
            BYTE* Write = (BYTE*)WriteBuf.get();
            for (UINT32 i = 0; i < BlockSize; i += sizeof(uint64_t)) {
                uint64_t Value = Tag ^ (uint64_t)i;
                memcpy(Write + i, &Value, sizeof(uint64_t));
            }

            if (!SoakPositionedIo(TRUE, Handle, WriteBuf.get(), BlockSize,
                                  ByteOffset)) {
                IoErrors++;
                Record("write failed at block " + std::to_string(Block) +
                       ": " + WinStrError(GetLastError()));
                break;
            }
            memset(ReadBuf.get(), 0, BlockSize);
            if (!SoakPositionedIo(FALSE, Handle, ReadBuf.get(), BlockSize,
                                  ByteOffset)) {
                IoErrors++;
                Record("read failed at block " + std::to_string(Block) +
                       ": " + WinStrError(GetLastError()));
                break;
            }
            if (memcmp(ReadBuf.get(), WriteBuf.get(), BlockSize) != 0) {
                Corruptions++;
                Record("data mismatch at block " + std::to_string(Block) +
                       " (worker " + std::to_string(WorkerId) + ", seq " +
                       std::to_string(Seq) + ")");
            }
            TotalOps++;
            Seq++;
        }
    };

    std::vector<std::thread> Workers;
    for (DWORD i = 0; i < Threads; i++) {
        Workers.emplace_back(Worker, i);
    }
    for (auto& Thread : Workers) {
        Thread.join();
    }

    cout << "Soak (disjoint): " << TotalOps.load() << " write+verify ops over "
         << Threads << " threads / " << Seconds << "s." << endl;
    EXPECT_EQ(0ULL, IoErrors.load()) << "I/O errors during soak: " << FirstFailure;
    EXPECT_EQ(0ULL, Corruptions.load())
        << "data corruption during soak: " << FirstFailure;
    EXPECT_LT(0ULL, TotalOps.load()) << "soak performed no I/O";
}

// Pattern B: many workers hammer the SAME block, each filling it with a single
// distinct byte. A single-block write is one atomic NBD request, so a correctly
// framed result is always homogeneous (every byte equal). A missing SendLock
// interleaves header/payload from the two dispatcher workers on the shared
// socket, which shows up either as an I/O/protocol error or as a
// non-homogeneous block. Reader threads sample the block concurrently.
TEST(TestNbdSoak, OverlappingBlockFramingIsAtomic) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    WNBD_PROPERTIES WnbdProps = { 0 };
    NbdMapping Mapping(&WnbdProps);

    WNBD_CONNECTION_INFO ConnectionInfo = { 0 };
    ASSERT_FALSE(WnbdShow(WnbdProps.InstanceName, &ConnectionInfo))
        << "couldn't retrieve WNBD disk info";
    UINT64 BlockCount = ConnectionInfo.Properties.BlockCount;
    UINT32 BlockSize = ConnectionInfo.Properties.BlockSize;
    ASSERT_LT(0ULL, BlockCount);
    ASSERT_LT(0UL, BlockSize);
    SetDiskWritable(string(WnbdProps.InstanceName));

    const DWORD Threads = SoakEnvDword("WNBD_SOAK_THREADS", 32);
    const DWORD Seconds = SoakEnvDword("WNBD_SOAK_SECONDS", 20);
    const UINT64 TargetBlock = BlockCount / 2;
    const UINT64 ByteOffset = TargetBlock * BlockSize;

    std::atomic<uint64_t> IoErrors{ 0 };
    std::atomic<uint64_t> Writes{ 0 };
    std::atomic<uint64_t> Reads{ 0 };
    std::atomic<uint64_t> FramingViolations{ 0 };
    std::mutex FirstFailureLock;
    string FirstFailure;
    auto Record = [&](const string& Message) {
        std::lock_guard<std::mutex> Guard(FirstFailureLock);
        if (FirstFailure.empty()) FirstFailure = Message;
    };

    auto Deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(Seconds);

    auto Writer = [&](DWORD WorkerId) {
        HANDLE Handle = OpenSoakDisk(Mapping.GetInstanceName());
        if (Handle == INVALID_HANDLE_VALUE) {
            IoErrors++;
            Record("writer open failed: " + WinStrError(GetLastError()));
            return;
        }
        unique_ptr<void, decltype(&CloseHandle)> HandleCloser(
            Handle, &CloseHandle);
        unique_ptr<void, decltype(&_aligned_free)> WriteBuf(
            _aligned_malloc(BlockSize, BlockSize), &_aligned_free);
        if (!WriteBuf.get()) {
            IoErrors++;
            Record("writer alloc failed");
            return;
        }
        memset(WriteBuf.get(), (BYTE)(0xA0 + (WorkerId & 0x3F)), BlockSize);

        while (std::chrono::steady_clock::now() < Deadline) {
            if (!SoakPositionedIo(TRUE, Handle, WriteBuf.get(), BlockSize,
                                  ByteOffset)) {
                IoErrors++;
                Record("shared-block write failed: " +
                       WinStrError(GetLastError()));
                break;
            }
            Writes++;
        }
    };

    auto Reader = [&]() {
        HANDLE Handle = OpenSoakDisk(Mapping.GetInstanceName());
        if (Handle == INVALID_HANDLE_VALUE) {
            IoErrors++;
            Record("reader open failed: " + WinStrError(GetLastError()));
            return;
        }
        unique_ptr<void, decltype(&CloseHandle)> HandleCloser(
            Handle, &CloseHandle);
        unique_ptr<void, decltype(&_aligned_free)> ReadBuf(
            _aligned_malloc(BlockSize, BlockSize), &_aligned_free);
        if (!ReadBuf.get()) {
            IoErrors++;
            Record("reader alloc failed");
            return;
        }

        while (std::chrono::steady_clock::now() < Deadline) {
            memset(ReadBuf.get(), 0, BlockSize);
            if (!SoakPositionedIo(FALSE, Handle, ReadBuf.get(), BlockSize,
                                  ByteOffset)) {
                IoErrors++;
                Record("shared-block read failed: " +
                       WinStrError(GetLastError()));
                break;
            }
            Reads++;
            // A single-sector write is atomic, so any observed block must be
            // homogeneous regardless of which writer won. Non-homogeneous means
            // header/payload framing was corrupted on the wire.
            BYTE* Bytes = (BYTE*)ReadBuf.get();
            BYTE First = Bytes[0];
            for (UINT32 i = 1; i < BlockSize; i++) {
                if (Bytes[i] != First) {
                    FramingViolations++;
                    Record("non-homogeneous shared block: byte[0]=" +
                           std::to_string((int)First) + " byte[" +
                           std::to_string(i) + "]=" +
                           std::to_string((int)Bytes[i]));
                    break;
                }
            }
        }
    };

    std::vector<std::thread> ThreadPool;
    DWORD Readers = Threads / 8 ? Threads / 8 : 1;
    for (DWORD i = 0; i < Threads; i++) {
        ThreadPool.emplace_back(Writer, i);
    }
    for (DWORD i = 0; i < Readers; i++) {
        ThreadPool.emplace_back(Reader);
    }
    for (auto& Thread : ThreadPool) {
        Thread.join();
    }

    cout << "Soak (shared block): " << Writes.load() << " writes, "
         << Reads.load() << " reads over " << Threads << " writers / "
         << Seconds << "s." << endl;
    EXPECT_EQ(0ULL, IoErrors.load())
        << "I/O errors on the shared block (possible framing corruption): "
        << FirstFailure;
    EXPECT_EQ(0ULL, FramingViolations.load())
        << "framing corruption on the shared block: " << FirstFailure;
    EXPECT_LT(0ULL, Writes.load()) << "soak performed no writes";
}

// Teardown under load: hard-remove the mapping while many threads are actively
// writing. This drives the driver's removal path (DrainDeviceQueue, submitted-
// request tag-hash unlink, rundown release) concurrently with in-flight I/O and
// the userspace daemon shutdown -- the removal-under-I/O race the other soak
// tests never hit. The oracle is completion: no deadlock (a hang trips the CI
// step timeout) and no crash. Run under Driver Verifier, a use-after-free or
// pool error on the teardown path bugchecks the machine and fails the run.
// Handles are opened before the storm so no worker calls GetDiskPath (which can
// throw) while the device is being removed.
TEST(TestNbdSoak, TeardownUnderLoad) {
    if (const char* SkipReason = GetMissingNbdParamReason()) {
        WNBD_GTEST_SKIP(SkipReason);
    }

    const DWORD Threads = SoakEnvDword("WNBD_SOAK_THREADS", 16);
    const DWORD Rounds = SoakEnvDword("WNBD_SOAK_TEARDOWN_ROUNDS", 4);

    for (DWORD Round = 0; Round < Rounds; Round++) {
        WNBD_PROPERTIES WnbdProps = { 0 };
        auto Mapping = std::make_unique<NbdMapping>(&WnbdProps);

        WNBD_CONNECTION_INFO ConnectionInfo = { 0 };
        ASSERT_FALSE(WnbdShow(WnbdProps.InstanceName, &ConnectionInfo))
            << "couldn't retrieve WNBD disk info";
        UINT64 BlockCount = ConnectionInfo.Properties.BlockCount;
        UINT32 BlockSize = ConnectionInfo.Properties.BlockSize;
        ASSERT_LT(0ULL, BlockCount);
        ASSERT_LT(0UL, BlockSize);
        SetDiskWritable(string(WnbdProps.InstanceName));

        UINT64 StrideBlocks = BlockCount / ((UINT64)Threads + 2);
        ASSERT_GE(StrideBlocks, 64ULL) << "disk too small for the teardown layout";

        // Open every handle up front (disk is present now).
        std::vector<HANDLE> Handles(Threads, INVALID_HANDLE_VALUE);
        for (DWORD i = 0; i < Threads; i++) {
            Handles[i] = OpenSoakDisk(Mapping->GetInstanceName());
        }

        std::atomic<bool> Stop{ false };
        std::atomic<uint64_t> Ops{ 0 };
        std::atomic<uint64_t> IoErrors{ 0 };
        std::vector<std::thread> Workers;

        for (DWORD i = 0; i < Threads; i++) {
            Workers.emplace_back([&, i] {
                HANDLE Handle = Handles[i];
                if (Handle == INVALID_HANDLE_VALUE) {
                    return;
                }
                unique_ptr<void, decltype(&_aligned_free)> Buf(
                    _aligned_malloc(BlockSize, BlockSize), &_aligned_free);
                if (!Buf.get()) {
                    return;
                }
                memset(Buf.get(), 0x5a, BlockSize);
                UINT64 RegionBase = (UINT64)i * StrideBlocks;
                uint64_t Seq = 0;
                while (!Stop.load()) {
                    UINT64 Block = RegionBase + (Seq % 64);
                    // Errors are expected once removal starts; stop on the first.
                    if (!SoakPositionedIo(TRUE, Handle, Buf.get(), BlockSize,
                                          Block * BlockSize)) {
                        IoErrors++;
                        break;
                    }
                    Ops++;
                    Seq++;
                }
            });
        }

        // Let the I/O storm ramp up, then hard-remove mid-flight. NbdMapping's
        // destructor issues a hard remove and joins the daemon thread; if that
        // deadlocks against the in-flight I/O the test hangs (caught by the CI
        // timeout).
        Sleep(500);
        Mapping.reset();
        Stop.store(true);

        for (auto& Worker : Workers) {
            Worker.join();
        }
        for (HANDLE Handle : Handles) {
            if (Handle != INVALID_HANDLE_VALUE) {
                CloseHandle(Handle);
            }
        }

        cout << "Teardown-under-load round " << Round << ": " << Ops.load()
             << " ops before removal, " << IoErrors.load()
             << " threads saw the expected removal error." << endl;
    }

    SUCCEED() << "teardown under concurrent I/O completed without hang or crash";
}
