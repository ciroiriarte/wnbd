#include "pch.h"

#include "nbd_protocol.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

typedef DWORD (*NbdRecvExactFn)(
    _In_ SOCKET Fd,
    _Inout_ PVOID Data,
    _In_ size_t Length,
    _In_opt_ PVOID Context);

PNBD_HANDSHAKE_RPL NbdReadHandshakeReplyWithRecv(
    _In_ SOCKET Fd,
    _In_ NbdRecvExactFn RecvFn,
    _In_opt_ PVOID Context);

DWORD NbdValidateInitialMagic(
    _In_reads_bytes_(Length) const CHAR* Magic,
    _In_ size_t Length);

DWORD NbdValidateOptionMagic(
    _In_ UINT64 Magic,
    _Out_opt_ PBOOLEAN OldStyle);

DWORD NbdParseOptGoReply(
    _In_ UINT32 ReplyType,
    _In_reads_bytes_opt_(DataLength) const CHAR* Data,
    _In_ UINT32 DataLength,
    _Inout_ PUINT64 Size,
    _Inout_ PUINT16 Flags,
    _Inout_ PBOOLEAN SeenExport);

UINT32 NbdMaskClientFlags(
    _In_ UINT32 ClientFlags,
    _In_ UINT16 ServerFlags);

DWORD NbdRequireFixedNewstyle(
    _In_ UINT32 ClientFlags);

DWORD NbdGetReadResponseBufferSize(
    _In_ UINT32 RequestLength,
    _In_ UINT32 PreallocatedBufferSize,
    _Out_ PUINT32 DataBufferSize);

size_t NbdGetDefaultPendingRequestsReserve();

DWORD NbdSendWrite(
    _In_ SOCKET Fd,
    _In_ UINT64 Offset,
    _In_ ULONG Length,
    _In_ PVOID Data,
    _In_ UINT64 Handle,
    _In_ UINT32 NbdTransmissionFlags);

void NbdEncodeRequest(
    _Out_ PNBD_REQUEST Request,
    _In_ UINT64 Offset,
    _In_ ULONG Length,
    _In_ UINT64 Handle,
    _In_ NbdRequestType RequestType);

namespace {

UINT16 Swap16(UINT16 Value)
{
    return (UINT16) (((Value & 0x00ff) << 8) |
                     ((Value & 0xff00) >> 8));
}

UINT32 Swap32(UINT32 Value)
{
    return ((Value & 0x000000ff) << 24) |
           ((Value & 0x0000ff00) << 8) |
           ((Value & 0x00ff0000) >> 8) |
           ((Value & 0xff000000) >> 24);
}

UINT64 Swap64(UINT64 Value)
{
    return ((Value & 0x00000000000000ffULL) << 56) |
           ((Value & 0x000000000000ff00ULL) << 40) |
           ((Value & 0x0000000000ff0000ULL) << 24) |
           ((Value & 0x00000000ff000000ULL) << 8) |
           ((Value & 0x000000ff00000000ULL) >> 8) |
           ((Value & 0x0000ff0000000000ULL) >> 24) |
           ((Value & 0x00ff000000000000ULL) >> 40) |
           ((Value & 0xff00000000000000ULL) >> 56);
}


class WinsockSession {
private:
    WSADATA WsaData = {};
    bool Started = false;

public:
    WinsockSession()
    {
        // ASSERT_* can't be used here: it expands to `return;`, which MSVC
        // rejects inside a constructor (C2534).
        int Err = WSAStartup(MAKEWORD(2, 2), &WsaData);
        EXPECT_EQ(0, Err) << "WSAStartup failed: " << Err;
        Started = (Err == 0);
    }

    ~WinsockSession()
    {
        if (Started) {
            WSACleanup();
        }
    }
};

struct LoopbackSockets {
    SOCKET Client = INVALID_SOCKET;
    SOCKET Server = INVALID_SOCKET;

    LoopbackSockets() = default;
    LoopbackSockets(const LoopbackSockets&) = delete;
    LoopbackSockets& operator=(const LoopbackSockets&) = delete;
    LoopbackSockets(LoopbackSockets&& Other) noexcept
    {
        Client = Other.Client;
        Server = Other.Server;
        Other.Client = INVALID_SOCKET;
        Other.Server = INVALID_SOCKET;
    }
    LoopbackSockets& operator=(LoopbackSockets&& Other) noexcept
    {
        if (this != &Other) {
            if (Client != INVALID_SOCKET) {
                closesocket(Client);
            }
            if (Server != INVALID_SOCKET) {
                closesocket(Server);
            }
            Client = Other.Client;
            Server = Other.Server;
            Other.Client = INVALID_SOCKET;
            Other.Server = INVALID_SOCKET;
        }
        return *this;
    }

    ~LoopbackSockets()
    {
        if (Client != INVALID_SOCKET) {
            closesocket(Client);
        }
        if (Server != INVALID_SOCKET) {
            closesocket(Server);
        }
    }
};

LoopbackSockets CreateLoopbackSockets()
{
    SOCKET Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    EXPECT_NE(INVALID_SOCKET, Listener);

    sockaddr_in Address = {};
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = 0;
    EXPECT_EQ(0, bind(Listener, (sockaddr*) &Address, sizeof(Address)));
    EXPECT_EQ(0, listen(Listener, 1));

    int AddressLength = sizeof(Address);
    EXPECT_EQ(0, getsockname(Listener, (sockaddr*) &Address, &AddressLength));

    LoopbackSockets Sockets;
    Sockets.Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    EXPECT_NE(INVALID_SOCKET, Sockets.Client);
    EXPECT_EQ(0, connect(Sockets.Client, (sockaddr*) &Address, AddressLength));

    Sockets.Server = accept(Listener, nullptr, nullptr);
    EXPECT_NE(INVALID_SOCKET, Sockets.Server);
    closesocket(Listener);

    DWORD TimeoutMs = 2000;
    EXPECT_EQ(0, setsockopt(Sockets.Server, SOL_SOCKET, SO_RCVTIMEO,
                            (const char*) &TimeoutMs, sizeof(TimeoutMs)));
    return Sockets;
}

void RecvExactForTest(SOCKET Socket, void* Buffer, size_t Length)
{
    char* Cursor = (char*) Buffer;
    while (Length) {
        int Received = recv(Socket, Cursor, (int) Length, 0);
        ASSERT_GT(Received, 0) << "recv failed: " << WSAGetLastError();
        Cursor += Received;
        Length -= Received;
    }
}

std::array<CHAR, 12> ExportInfo(UINT64 Size, UINT16 Flags)
{
    std::array<CHAR, 12> Data = {};
    UINT16 Type = Swap16(NBD_INFO_EXPORT);
    UINT64 ExportSize = Swap64(Size);
    UINT16 ExportFlags = Swap16(Flags);

    memcpy(Data.data(), &Type, sizeof(Type));
    memcpy(Data.data() + sizeof(Type), &ExportSize, sizeof(ExportSize));
    memcpy(Data.data() + sizeof(Type) + sizeof(ExportSize),
           &ExportFlags,
           sizeof(ExportFlags));

    return Data;
}

UINT32 HostToBig32(UINT32 Value)
{
    return Swap32(Value);
}

UINT64 HostToBig64(UINT64 Value)
{
    return Swap64(Value);
}

std::array<CHAR, sizeof(NBD_HANDSHAKE_RPL)> HandshakeHeader(
    UINT64 Magic,
    UINT32 Option,
    UINT32 ReplyType,
    UINT32 Datasize)
{
    std::array<CHAR, sizeof(NBD_HANDSHAKE_RPL)> Header = {};
    auto Reply = (PNBD_HANDSHAKE_RPL) Header.data();
    Reply->Magic = HostToBig64(Magic);
    Reply->Option = HostToBig32(Option);
    Reply->ReplyType = HostToBig32(ReplyType);
    Reply->Datasize = HostToBig32(Datasize);
    return Header;
}

struct FailingPayloadReaderContext {
    std::array<CHAR, sizeof(NBD_HANDSHAKE_RPL)> Header;
    int Calls;
};

DWORD FailingPayloadReader(
    SOCKET Fd,
    PVOID Data,
    size_t Length,
    PVOID RawContext)
{
    UNREFERENCED_PARAMETER(Fd);

    auto Context = (FailingPayloadReaderContext*) RawContext;
    ++Context->Calls;

    if (Context->Calls == 1) {
        EXPECT_EQ(sizeof(Context->Header), Length);
        memcpy(Data, Context->Header.data(), Context->Header.size());
        return 0;
    }

    EXPECT_EQ(4, Length);
    return ERROR_GRACEFUL_DISCONNECT;
}

void ExpectRequestEncoding(
    const NBD_REQUEST& Request,
    NbdRequestType RequestType)
{
    EXPECT_EQ((UINT32) NBD_REQUEST_MAGIC, Swap32(Request.Magic));
    EXPECT_EQ((UINT32) RequestType, Swap32(Request.Type));
    EXPECT_EQ(0ULL, Swap64(Request.From));
    EXPECT_EQ(0UL, Swap32(Request.Length));
}

}

TEST(TestNbdProtocol, RejectsMalformedInitialMagic)
{
    EXPECT_EQ(ERROR_BAD_FORMAT, NbdValidateInitialMagic("BADMAGIC", 8));
    EXPECT_EQ(ERROR_BAD_FORMAT, NbdValidateInitialMagic(INIT_PASSWD, 7));
}

TEST(TestNbdProtocol, ValidatesOldstyleAndOptionMagic)
{
    BOOLEAN OldStyle = TRUE;
    EXPECT_EQ(0, NbdValidateOptionMagic(OPTION_MAGIC, &OldStyle));
    EXPECT_FALSE(OldStyle);

    EXPECT_EQ(ERROR_NOT_SUPPORTED,
              NbdValidateOptionMagic(CLIENT_MAGIC, &OldStyle));
    EXPECT_TRUE(OldStyle);

    EXPECT_EQ(ERROR_BAD_FORMAT, NbdValidateOptionMagic(0, &OldStyle));
    EXPECT_FALSE(OldStyle);
}

TEST(TestNbdProtocol, RejectsTruncatedInfoReply)
{
    UINT64 Size = 0;
    UINT16 Flags = 0;
    BOOLEAN SeenExport = FALSE;
    CHAR Data[] = { 0 };

    EXPECT_EQ(ERROR_BAD_FORMAT,
              NbdParseOptGoReply(
                  NBD_REP_INFO,
                  Data,
                  sizeof(Data),
                  &Size,
                  &Flags,
                  &SeenExport));
    EXPECT_FALSE(SeenExport);
}

TEST(TestNbdProtocol, RejectsAckBeforeExportInfo)
{
    UINT64 Size = 0;
    UINT16 Flags = 0;
    BOOLEAN SeenExport = FALSE;

    EXPECT_EQ(ERROR_BAD_FORMAT,
              NbdParseOptGoReply(
                  NBD_REP_ACK,
                  nullptr,
                  0,
                  &Size,
                  &Flags,
                  &SeenExport));
}

TEST(TestNbdProtocol, RejectsAckWithPayload)
{
    UINT64 Size = 0;
    UINT16 Flags = 0;
    BOOLEAN SeenExport = TRUE;
    CHAR Data[] = { 'x' };

    EXPECT_EQ(ERROR_BAD_FORMAT,
              NbdParseOptGoReply(
                  NBD_REP_ACK,
                  Data,
                  sizeof(Data),
                  &Size,
                  &Flags,
                  &SeenExport));
}

TEST(TestNbdProtocol, ReadHandshakeReplyReturnsNullOnPayloadFailure)
{
    FailingPayloadReaderContext Context = {
        HandshakeHeader(REPLY_MAGIC, NBD_OPT_GO, NBD_REP_INFO, 4),
        0,
    };

    EXPECT_EQ(nullptr,
              NbdReadHandshakeReplyWithRecv(
                  INVALID_SOCKET,
                  FailingPayloadReader,
                  &Context));
    EXPECT_EQ(2, Context.Calls);
}

TEST(TestNbdProtocol, RejectsMalformedExportInfoLength)
{
    auto Data = ExportInfo(0x1122334455667788ULL, NBD_FLAG_SEND_FLUSH);
    UINT64 Size = 0;
    UINT16 Flags = 0;
    BOOLEAN SeenExport = FALSE;

    EXPECT_EQ(ERROR_BAD_FORMAT,
              NbdParseOptGoReply(
                  NBD_REP_INFO,
                  Data.data(),
                  (UINT32) Data.size() - 1,
                  &Size,
                  &Flags,
                  &SeenExport));
    EXPECT_FALSE(SeenExport);
}

TEST(TestNbdProtocol, ParsesValidExportInfoThenAck)
{
    auto Data = ExportInfo(0x1122334455667788ULL,
                           NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH);
    UINT64 Size = 0;
    UINT16 Flags = 0;
    BOOLEAN SeenExport = FALSE;

    EXPECT_EQ(0,
              NbdParseOptGoReply(
                  NBD_REP_INFO,
                  Data.data(),
                  (UINT32) Data.size(),
                  &Size,
                  &Flags,
                  &SeenExport));
    EXPECT_TRUE(SeenExport);
    EXPECT_EQ(0x1122334455667788ULL, Size);
    EXPECT_EQ(NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH, Flags);

    EXPECT_EQ(0,
              NbdParseOptGoReply(
                  NBD_REP_ACK,
                  nullptr,
                  0,
                  &Size,
                  &Flags,
                  &SeenExport));
}

TEST(TestNbdProtocol, EncodesDisconnectWithZeroOffsetAndLength)
{
    NBD_REQUEST Request = {};
    NbdEncodeRequest(&Request, 0, 0, 0, NBD_CMD_DISC);

    ExpectRequestEncoding(Request, NBD_CMD_DISC);
}

TEST(TestNbdProtocol, EncodesFlushWithZeroOffsetAndLength)
{
    NBD_REQUEST Request = {};
    NbdEncodeRequest(&Request, 0, 0, 0, NBD_CMD_FLUSH);

    ExpectRequestEncoding(Request, NBD_CMD_FLUSH);
}

TEST(TestNbdProtocol, MasksClientFlagsToServerAdvertisedFlags)
{
    const UINT32 Requested = NBD_FLAG_FIXED_NEWSTYLE |
                             NBD_FLAG_NO_ZEROES |
                             (1u << 12);
    const UINT16 Advertised = NBD_FLAG_FIXED_NEWSTYLE;

    EXPECT_EQ((UINT32) NBD_FLAG_FIXED_NEWSTYLE,
              NbdMaskClientFlags(Requested, Advertised));
}

TEST(TestNbdProtocol, FixedNewstyleRequirementRejectsMaskedClientFlags)
{
    const UINT32 Requested = NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES;
    const UINT16 Advertised = NBD_FLAG_NO_ZEROES;
    const UINT32 Masked = NbdMaskClientFlags(Requested, Advertised);

    EXPECT_EQ((UINT32) NBD_FLAG_NO_ZEROES, Masked);
    EXPECT_EQ(ERROR_NOT_SUPPORTED, NbdRequireFixedNewstyle(Masked));
    EXPECT_EQ(0, NbdRequireFixedNewstyle(NBD_FLAG_FIXED_NEWSTYLE));
}

TEST(TestNbdProtocolPerformance, ReadResponseUsesActualRequestLength)
{
    UINT32 DataBufferSize = 0;

    EXPECT_EQ(0, NbdGetReadResponseBufferSize(4096, 1024 * 1024,
                                              &DataBufferSize));
    EXPECT_EQ(4096UL, DataBufferSize);

    DataBufferSize = 0;
    EXPECT_EQ(0, NbdGetReadResponseBufferSize(1024 * 1024, 1024 * 1024,
                                              &DataBufferSize));
    EXPECT_EQ(1024UL * 1024UL, DataBufferSize);
}

TEST(TestNbdProtocolPerformance, ReadResponseRejectsOversizedRequest)
{
    UINT32 DataBufferSize = 0xaaaaaaaa;

    EXPECT_EQ(ERROR_FILE_TOO_LARGE,
              NbdGetReadResponseBufferSize(1024 * 1024 + 1,
                                           1024 * 1024,
                                           &DataBufferSize));
    EXPECT_EQ(0xaaaaaaaaUL, DataBufferSize);
    EXPECT_EQ(ERROR_INVALID_PARAMETER,
              NbdGetReadResponseBufferSize(1, 1, nullptr));
}

TEST(TestNbdProtocolPerformance, WritePathSendsHeaderAndPayload)
{
    WinsockSession Winsock;
    auto Sockets = CreateLoopbackSockets();
    std::vector<CHAR> Payload(4096, 0x5a);

    ASSERT_EQ(0, NbdSendWrite(Sockets.Client,
                              512,
                              (ULONG) Payload.size(),
                              Payload.data(),
                              0x1122334455667788ULL,
                              NBD_CMD_FLAG_FUA));
    shutdown(Sockets.Client, SD_SEND);

    NBD_REQUEST Request = {};
    RecvExactForTest(Sockets.Server, &Request, sizeof(Request));
    EXPECT_EQ((UINT32) NBD_REQUEST_MAGIC, Swap32(Request.Magic));
    EXPECT_EQ((UINT32) (NBD_CMD_WRITE | NBD_CMD_FLAG_FUA),
              Swap32(Request.Type));
    // The NBD handle/cookie is opaque: it is sent verbatim (host order, not
    // byte-swapped) and echoed back unchanged by the server.
    EXPECT_EQ(0x1122334455667788ULL, Request.Handle);
    EXPECT_EQ(512ULL, Swap64(Request.From));
    EXPECT_EQ(Payload.size(), Swap32(Request.Length));

    std::vector<CHAR> ReceivedPayload(Payload.size());
    RecvExactForTest(Sockets.Server,
                     ReceivedPayload.data(),
                     ReceivedPayload.size());
    EXPECT_EQ(Payload, ReceivedPayload);
}

TEST(TestNbdProtocolPerformance, PendingRequestReserveAvoidsExpectedRehash)
{
    const size_t Reserve = NbdGetDefaultPendingRequestsReserve();
    ASSERT_GE(Reserve, 1024ULL);

    std::unordered_map<UINT64, UINT32> PendingRequests;
    PendingRequests.reserve(Reserve);
    const size_t BucketCount = PendingRequests.bucket_count();

    for (size_t Index = 0; Index < Reserve; ++Index) {
        PendingRequests.emplace((UINT64) Index, (UINT32) Index);
        ASSERT_EQ(BucketCount, PendingRequests.bucket_count())
            << "rehash at insert " << Index;
    }
}
