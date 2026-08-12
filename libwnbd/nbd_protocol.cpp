/*
 * Copyright (c) 2019 SUSE LLC
 * Copyright (c) 2026 Ciro Iriarte
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

// Disable "enum class" warnings
#pragma warning(disable:26812)

#include "nbd_protocol.h"
#include "wnbd_log.h"
#include "utils.h"

#include <boost/endian/conversion.hpp>

using boost::endian::native_to_big;
using boost::endian::native_to_big_inplace;
using boost::endian::big_to_native;
using boost::endian::big_to_native_inplace;

typedef DWORD (*NbdRecvExactFn)(
    _In_ SOCKET Fd,
    _Inout_ PVOID Data,
    _In_ size_t Length,
    _In_opt_ PVOID Context);

_Use_decl_annotations_
DWORD RecvExact(
    SOCKET Fd,
    PVOID Data,
    size_t Length)
{
    if (Fd == INVALID_SOCKET) {
        return ERROR_INVALID_HANDLE;
    }
    INT Result = 0;
    PCHAR CurrDataPtr = (PCHAR) Data;
    while (0 < Length) {
        Result = ::recv(Fd, CurrDataPtr, (int) Length, 0);
        if (Result > 0) {
            Length -= Result;
            CurrDataPtr = CurrDataPtr + Result;
        } else {
            if (Result) {
                auto Err = WSAGetLastError();
                switch(Err) {
                case WSAEINTR:
                    LogInfo("Request canceled.");
                    // Not a typo.
                    Result = ERROR_CANCELLED;
                    break;
                case WSAESHUTDOWN:
                case WSAECONNRESET:
                case WSAEDISCON:
                    LogInfo("Connection closed. "
                            "Status: %d. Message: %s",
                            Err, win32_strerror(Err).c_str());
                    Result = ERROR_GRACEFUL_DISCONNECT;
                    break;
                default:
                    LogError("Read failed. "
                             "Error: %d. Error message: %s",
                             Err, win32_strerror(Err).c_str());
                }
            } else {
                LogInfo("Connection closed.");
                Result = ERROR_GRACEFUL_DISCONNECT;
            }
            return Result;
        }
    }
    if (Length) {
        // Shouldn't get here.
        LogError("Couldn't receive all data.");
        return ERROR_GEN_FAILURE;
    }
    return 0;
}

DWORD SendExact(
    _In_ SOCKET Fd,
    _In_ PVOID Data,
    _In_ size_t Length)
{
    if (Fd == INVALID_SOCKET) {
        return ERROR_INVALID_HANDLE;
    }
    INT Result = 0;
    PCHAR CurrDataPtr = (PCHAR) Data;
    while (Length > 0) {
        Result = ::send(Fd, CurrDataPtr, (int) Length, 0);
        if (Result <= 0) {
            auto Err = WSAGetLastError();
            switch(Err) {
            case WSAEINTR:
                LogInfo("Request canceled.");
                Result = ERROR_CANCELLED;
                break;
            case WSAESHUTDOWN:
            case WSAECONNRESET:
            case WSAEDISCON:
                LogInfo("Connection closed. "
                        "Status: %d. Message: %s.",
                        Err, win32_strerror(Err).c_str());
                Result = ERROR_GRACEFUL_DISCONNECT;
                break;
            default:
                LogError("Send failed. "
                         "Error: %d. Error message: %s",
                         Err, win32_strerror(Err).c_str());
            }
            return Result;
        }
        Length -= Result;
        CurrDataPtr += Result;
    }
    if (Length) {
        // Shouldn't get here.
        LogError("Couldn't send all data.");
        return ERROR_GEN_FAILURE;
    }
    return 0;
}

DWORD NbdSendHandshakeRequest(
    _In_ SOCKET Fd,
    _In_ UINT32 Option,
    _In_ size_t Datasize,
    _Maybenull_ PVOID Data)
{
    NBD_HANDSHAKE_REQ Request;
    Request.Magic = native_to_big(OPTION_MAGIC);
    Request.Option = native_to_big(Option);
    Request.Datasize = native_to_big((ULONG)Datasize);

    DWORD Retval = SendExact(Fd, &Request, sizeof(Request));
    if (!Retval && Data) {
        Retval = SendExact(Fd, Data, Datasize);
    }
    return Retval;
}

DWORD NbdSendInfoRequest(
    _In_ SOCKET Fd,
    _In_ UINT32 Options,
    _In_ USHORT NumberOfRequests,
    _Maybenull_ PUINT16 Requests,
    _In_ std::string ExportName)
{
    UINT16 NumberOfRequestsBE = native_to_big(NumberOfRequests);
    UINT32 NameLenBE = native_to_big((UINT32) ExportName.length());
    size_t TotalLen = sizeof(UINT32) +
                    ExportName.length() +
                    sizeof(UINT16) +
                    NumberOfRequests * sizeof(UINT16);

    DWORD Retval = 0;
    if (Retval = NbdSendHandshakeRequest(Fd, Options, TotalLen, NULL); Retval)
        return Retval;
    if (Retval = SendExact(Fd, &NameLenBE, sizeof(NameLenBE)); Retval)
        return Retval;
    if (Retval = SendExact(Fd, (PVOID) ExportName.c_str(),
                           ExportName.length()); Retval)
        return Retval;
    if (Retval = SendExact(Fd, &NumberOfRequestsBE,
                           sizeof(NumberOfRequestsBE)); Retval)
        return Retval;

    if (NumberOfRequests > 0 && Requests) {
        Retval = SendExact(
            Fd, Requests,
            NumberOfRequests * sizeof(UINT16));
    }

    return Retval;
}

namespace {

DWORD NbdRecvExactAdapter(
    SOCKET Fd,
    PVOID Data,
    size_t Length,
    PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return RecvExact(Fd, Data, Length);
}

}

#pragma warning(disable:6001)
PNBD_HANDSHAKE_RPL NbdReadHandshakeReplyWithRecv(
    _In_ SOCKET Fd,
    _In_ NbdRecvExactFn RecvFn,
    _In_opt_ PVOID Context)
{
    if (!RecvFn) {
        return nullptr;
    }

    PNBD_HANDSHAKE_RPL Reply = (PNBD_HANDSHAKE_RPL) calloc(
        1, sizeof(NBD_HANDSHAKE_RPL));
    if (!Reply) {
        LogError("Unable to allocate %d bytes.", sizeof(NBD_HANDSHAKE_RPL));
        return nullptr;
    }

    DWORD Retval = RecvFn(Fd, Reply, sizeof(*Reply), Context);
    if (Retval) {
        free(Reply);
        return nullptr;
    }

    big_to_native_inplace(Reply->Magic);
    big_to_native_inplace(Reply->Option);
    big_to_native_inplace(Reply->ReplyType);
    big_to_native_inplace(Reply->Datasize);

    if (REPLY_MAGIC != Reply->Magic) {
        LogError("Received invalid negotiation magic %llu (expected %llu)",
                 Reply->Magic, REPLY_MAGIC);
        free(Reply);
        return nullptr;
    }

    if (Reply->Datasize > 0) {
        size_t NewSize = sizeof(NBD_HANDSHAKE_RPL) + Reply->Datasize;
        PVOID ReplyTemp = realloc(Reply, NewSize);
        if (!ReplyTemp) {
            LogError("Unable to allocate %d bytes.", NewSize);
            free(Reply);
            return nullptr;
        }
        Reply = (PNBD_HANDSHAKE_RPL) ReplyTemp;

        Retval = RecvFn(Fd, Reply->Data, Reply->Datasize, Context);
        if (Retval) {
            free(Reply);
            return nullptr;
        }
    }
    return Reply;
}

PNBD_HANDSHAKE_RPL NbdReadHandshakeReply(_In_ SOCKET Fd)
{
    return NbdReadHandshakeReplyWithRecv(Fd, NbdRecvExactAdapter, nullptr);
}
#pragma warning(default:6001)

void NbdParseSizes(
    _In_ PCHAR Data,
    _Inout_ PUINT64 Size,
    _Inout_ PUINT16 Flags)
{
    CopyMemory(Size, Data, sizeof(*Size));
    big_to_native_inplace(*Size);
    Data += sizeof(*Size);
    CopyMemory(Flags, Data, sizeof(*Flags));
    big_to_native_inplace(*Flags);
}

DWORD NbdValidateInitialMagic(
    _In_reads_bytes_(Length) const CHAR* Magic,
    _In_ size_t Length)
{
    const size_t ExpectedLength = sizeof(INIT_PASSWD) - 1;
    if (!Magic ||
            Length != ExpectedLength ||
            memcmp(Magic, INIT_PASSWD, ExpectedLength)) {
        return ERROR_BAD_FORMAT;
    }
    return 0;
}

DWORD NbdValidateOptionMagic(
    _In_ UINT64 Magic,
    _Out_opt_ PBOOLEAN OldStyle)
{
    if (OldStyle) {
        *OldStyle = FALSE;
    }
    if (OPTION_MAGIC == Magic) {
        return 0;
    }
    if (CLIENT_MAGIC == Magic) {
        if (OldStyle) {
            *OldStyle = TRUE;
        }
        return ERROR_NOT_SUPPORTED;
    }
    return ERROR_BAD_FORMAT;
}

DWORD NbdParseOptGoReply(
    _In_ UINT32 ReplyType,
    _In_reads_bytes_opt_(DataLength) const CHAR* Data,
    _In_ UINT32 DataLength,
    _Inout_ PUINT64 Size,
    _Inout_ PUINT16 Flags,
    _Inout_ PBOOLEAN SeenExport)
{
    if (!Size || !Flags || !SeenExport) {
        return ERROR_INVALID_PARAMETER;
    }

    if (ReplyType == NBD_REP_ACK) {
        if (DataLength != 0) {
            return ERROR_BAD_FORMAT;
        }
        return *SeenExport ? 0 : ERROR_BAD_FORMAT;
    }

    if (ReplyType != NBD_REP_INFO) {
        return 0;
    }

    if (!Data || DataLength < sizeof(UINT16)) {
        return ERROR_BAD_FORMAT;
    }

    UINT16 Type;
    memcpy(&Type, Data, sizeof(Type));
    big_to_native_inplace(Type);
    if (Type != NBD_INFO_EXPORT) {
        return 0;
    }

    if (DataLength != sizeof(Type) + sizeof(*Size) + sizeof(*Flags)) {
        return ERROR_BAD_FORMAT;
    }

    NbdParseSizes((PCHAR) Data + sizeof(Type), Size, Flags);
    *SeenExport = TRUE;
    return 0;
}

// Parses an NBD_INFO_BLOCK_SIZE information reply sent in response to
// NBD_OPT_GO. Non-NBD_REP_INFO replies and info replies that carry a different
// information type are ignored (returns 0 without touching BlockSizeInfo). A
// truncated or oversized NBD_INFO_BLOCK_SIZE payload is rejected.
DWORD NbdParseInfoBlockSize(
    _In_ UINT32 ReplyType,
    _In_reads_bytes_opt_(DataLength) const CHAR* Data,
    _In_ UINT32 DataLength,
    _Inout_ PNBD_BLOCK_SIZE_INFO BlockSizeInfo)
{
    if (!BlockSizeInfo) {
        return ERROR_INVALID_PARAMETER;
    }

    if (ReplyType != NBD_REP_INFO) {
        return 0;
    }

    if (!Data || DataLength < sizeof(UINT16)) {
        return ERROR_BAD_FORMAT;
    }

    UINT16 Type;
    memcpy(&Type, Data, sizeof(Type));
    big_to_native_inplace(Type);
    if (Type != NBD_INFO_BLOCK_SIZE) {
        return 0;
    }

    // Type (UINT16) followed by minimum, preferred and maximum block sizes,
    // each a big-endian UINT32.
    if (DataLength != sizeof(Type) + 3 * sizeof(UINT32)) {
        return ERROR_BAD_FORMAT;
    }

    UINT32 Values[3];
    memcpy(Values, Data + sizeof(Type), sizeof(Values));
    BlockSizeInfo->Minimum = big_to_native(Values[0]);
    BlockSizeInfo->Preferred = big_to_native(Values[1]);
    BlockSizeInfo->Maximum = big_to_native(Values[2]);
    BlockSizeInfo->Received = TRUE;
    return 0;
}

UINT32 NbdMaskClientFlags(
    _In_ UINT32 ClientFlags,
    _In_ UINT16 ServerFlags)
{
    return ClientFlags & ServerFlags;
}

DWORD NbdRequireFixedNewstyle(
    _In_ UINT32 ClientFlags)
{
    return (ClientFlags & NBD_FLAG_FIXED_NEWSTYLE) ? 0 : ERROR_NOT_SUPPORTED;
}

DWORD NbdGetReadResponseBufferSize(
    _In_ UINT32 RequestLength,
    _In_ UINT32 PreallocatedBufferSize,
    _Out_ PUINT32 DataBufferSize)
{
    if (!DataBufferSize) {
        return ERROR_INVALID_PARAMETER;
    }

    if (RequestLength > PreallocatedBufferSize) {
        return ERROR_FILE_TOO_LARGE;
    }

    *DataBufferSize = RequestLength;
    return 0;
}

size_t NbdGetDefaultPendingRequestsReserve()
{
    return 1024;
}

DWORD NbdSendOptExportName(
    _In_ SOCKET Fd,
    _In_ PUINT64 Size,
    _In_ PUINT16 Flags,
    _In_ std::string ExportName,
    _In_ UINT16 GFlags)
{
    DWORD Retval = NbdSendHandshakeRequest(
        Fd, NBD_OPT_EXPORT_NAME,
        ExportName.length(), (PVOID) ExportName.c_str());
    if (Retval) {
        return Retval;
    }

    CHAR Buf[sizeof(*Flags) + sizeof(*Size)];
    ZeroMemory(Buf, sizeof(*Flags) + sizeof(*Size));
    if (Retval = RecvExact(Fd, Buf, sizeof(Buf)); Retval) {
        return Retval;
    }
    
    NbdParseSizes(Buf, Size, Flags);
    if (!(GFlags & NBD_FLAG_NO_ZEROES)) {
        CHAR Temp[125] = { 0 };
        // read the reserved bytes.
        Retval = RecvExact(Fd, Temp, 124);
    }

    return Retval;
}

DWORD NbdNegotiate(
    _In_ SOCKET Fd,
    _In_ PUINT64 Size,
    _In_ PUINT16 Flags,
    _In_ std::string ExportName,
    _In_ UINT32 ClientFlags,
    _Inout_opt_ PNBD_BLOCK_SIZE_INFO BlockSizeInfo)
{
    UINT64 Magic = 0;
    UINT16 GFlags = 0;
    CHAR Buf[256] = { 0 };

    DWORD Retval = RecvExact(Fd, Buf, 8);
    if (Retval) {
        return Retval;
    }
    Retval = NbdValidateInitialMagic(Buf, 8);
    if (Retval) {
        LogError("Received invalid NBD initial magic.");
        return Retval;
    }
    LogDebug("Received NBD INIT_PASSWD");

    if (Retval = RecvExact(Fd, &Magic, sizeof(Magic)); Retval) {
        return Retval;
    }
    big_to_native_inplace(Magic);
    BOOLEAN OldStyle = FALSE;
    Retval = NbdValidateOptionMagic(Magic, &OldStyle);
    if (Retval) {
        if (OldStyle) {
            LogInfo("Old-style NBD server is unsupported.");
        } else {
            LogError("Received invalid NBD option magic %llu.", Magic);
        }
        return Retval;
    }

    if (Retval = RecvExact(Fd, &GFlags, sizeof(UINT16)); Retval) {
        return Retval;
    }
    big_to_native_inplace(GFlags);

    if (GFlags & NBD_FLAG_NO_ZEROES) {
        ClientFlags |= NBD_FLAG_NO_ZEROES;
    }
    ClientFlags = NbdMaskClientFlags(ClientFlags, GFlags);

    Retval = NbdRequireFixedNewstyle(ClientFlags);
    if (Retval) {
        // This implementation relies on fixed-newstyle option haggling and
        // NBD_OPT_GO, so reject non-fixed newstyle instead of silently sending
        // unsupported option requests.
        LogError("NBD server does not advertise fixed-newstyle; "
                 "NBD_OPT_GO negotiation is unsupported.");
        return Retval;
    }

    big_to_native_inplace(ClientFlags);
    Retval = SendExact(Fd, (PCHAR) &ClientFlags, sizeof(ClientFlags));
    if (Retval) {
        LogError("Could not send NBD handshake request.");
        return Retval;
    }

    PNBD_HANDSHAKE_RPL Reply = NULL;
    BOOLEAN SeenExport = FALSE;
    // Request the export details as well as the block-size constraints. The
    // request identifiers are sent verbatim, so they must already be in
    // big-endian byte order.
    UINT16 InfoRequests[] = {
        native_to_big((UINT16) NBD_INFO_EXPORT),
        native_to_big((UINT16) NBD_INFO_BLOCK_SIZE),
    };
    Retval = NbdSendInfoRequest(
        Fd, NBD_OPT_GO,
        (USHORT) (sizeof(InfoRequests) / sizeof(InfoRequests[0])),
        InfoRequests, ExportName);
    if (Retval) {
        LogError("Could not send NBD handshake request.");
        return Retval;
    }

    do {
        if (Reply) {
            free(Reply);
        }
        Reply = NbdReadHandshakeReply(Fd);
        if (!Reply) {
            LogError("Couldn't retrieve handshake reply.");
            return ERROR_GEN_FAILURE;
        }
        if (Reply->ReplyType & NBD_REP_FLAG_ERROR) {
            switch (Reply->ReplyType) {
            case NBD_REP_ERR_UNSUP:
                LogWarning("Received NBD_REP_ERR_UNSUP reply. "
                           "Retrying with legacy NBD_OPT_EXPORT_NAME.");
                free(Reply);
                Retval = NbdSendOptExportName(Fd, Size, Flags,
                                              ExportName, GFlags);
                if (Retval) {
                    LogError("NBD_OPT_EXPORT_NAME failed.");
                } else {
                    LogInfo("Legacy NBD_OPT_EXPORT_NAME succeeded.");
                }
                return Retval;
            case NBD_REP_ERR_POLICY:
                if (Reply->Datasize > 0) {
                    // ensure that the log message is null terminated.
                    Reply->Data[Reply->Datasize - 1] = '\0';
                    LogError("Connection not allowed by server policy. "
                             "Server said: %s", Reply->Data);
                } else {
                    LogError("Connection not allowed by server policy.");
                }
                free(Reply);
                return ERROR_ACCESS_DENIED;
            case NBD_REP_ERR_BLOCK_SIZE_REQD:
                // The client already requests NBD_INFO_BLOCK_SIZE, so this
                // should not happen; surface it clearly if a server still
                // rejects the mapping over block-size constraints.
                LogError("Server requires the client to honor block-size "
                         "constraints (NBD_REP_ERR_BLOCK_SIZE_REQD).");
                free(Reply);
                return ERROR_NOT_SUPPORTED;
            default:
                if (Reply->Datasize > 0) {
                     // ensure that the log message is null terminated.
                    Reply->Data[Reply->Datasize - 1] = '\0';
                    LogError("Unknown error returned by server. "
                             "Server said: %s", Reply->Data);
                } else {
                    LogWarning("Unknown error returned by server.");
                }
                free(Reply);
                return ERROR_ACCESS_DENIED;
            }
        }

        Retval = NbdParseOptGoReply(
            Reply->ReplyType,
            Reply->Data,
            Reply->Datasize,
            Size,
            Flags,
            &SeenExport);
        if (Retval) {
            LogError("Malformed NBD_OPT_GO reply.");
            free(Reply);
            return Retval;
        }

        if (BlockSizeInfo) {
            Retval = NbdParseInfoBlockSize(
                Reply->ReplyType,
                Reply->Data,
                Reply->Datasize,
                BlockSizeInfo);
            if (Retval) {
                LogError("Malformed NBD_INFO_BLOCK_SIZE reply.");
                free(Reply);
                return Retval;
            }
        }

        if (Reply->ReplyType == NBD_REP_ACK) {
            LogDebug("Received NBD_REP_ACK.");
        } else if (Reply->ReplyType != NBD_REP_INFO) {
            LogWarning("Ignoring unknown reply to NBD_OPT_GO: %u.",
                       (unsigned int) Reply->ReplyType);
        }
    } while (Reply->ReplyType != NBD_REP_ACK);

    if (Reply) {
        free(Reply);
    }

    LogInfo("NBD negotiation successful.");
    return 0;
}

void NbdEncodeRequest(
    _Out_ PNBD_REQUEST Request,
    _In_ UINT64 Offset,
    _In_ ULONG Length,
    _In_ UINT64 Handle,
    _In_ NbdRequestType RequestType)
{
    Request->Magic = native_to_big((ULONG) NBD_REQUEST_MAGIC);
    Request->Type = native_to_big((ULONG) RequestType);
    Request->Length = native_to_big((ULONG) Length);
    Request->From = native_to_big((UINT64) Offset);
    Request->Handle = Handle;
}

_Use_decl_annotations_
DWORD NbdRequest(
    SOCKET Fd,
    UINT64 Offset,
    ULONG Length,
    UINT64 Handle,
    NbdRequestType RequestType)
{
    if (INVALID_SOCKET == Fd) {
        return ERROR_INVALID_HANDLE;
    }

    NBD_REQUEST Request;
    NbdEncodeRequest(&Request, Offset, Length, Handle, RequestType);

    DWORD Retval = SendExact(Fd, &Request, sizeof(NBD_REQUEST));
    if (Retval) {
        LogError("Could not send request: %s.",
                 NbdRequestTypeStr(RequestType));
    }
    return Retval;
}

_Use_decl_annotations_
DWORD NbdSendWrite(
    SOCKET Fd,
    UINT64 Offset,
    ULONG Length,
    PVOID Data,
    UINT64 Handle,
    UINT32 NbdTransmissionFlags)
{

    if (!Data) {
        LogError("No input buffer.");
        return ERROR_INVALID_PARAMETER;
    }
    if (INVALID_SOCKET == Fd) {
        LogError("Invalid socket.");
        return ERROR_INVALID_HANDLE;
    }

    NBD_REQUEST Request;
    NbdEncodeRequest(
        &Request,
        Offset,
        Length,
        Handle,
        (NbdRequestType) (NBD_CMD_WRITE | NbdTransmissionFlags));

    DWORD Retval = SendExact(Fd, &Request, sizeof(Request));
    if (!Retval && Length) {
        Retval = SendExact(Fd, Data, Length);
    }
    if (Retval) {
        LogError("Couldn't submit NBD_CMD_WRITE.");
    }
    return Retval;
}

_Use_decl_annotations_
DWORD NbdReadReply(SOCKET Fd, PNBD_REPLY Reply)
{
    DWORD Retval = RecvExact(Fd, Reply, sizeof(NBD_REPLY));
    if (Retval) {
        if (Retval != ERROR_GRACEFUL_DISCONNECT &&
                Retval != ERROR_CANCELLED) {
            LogError("Couldn't read NBD reply.");
        }
        return Retval;
    }

    if (NBD_REPLY_MAGIC != big_to_native(Reply->Magic)) {
        LogError("Invalid NBD_REPLY_MAGIC: %#x",
                 big_to_native(Reply->Magic));
        return ERROR_BAD_FORMAT;
    }

    return 0;
}

const char* NbdRequestTypeStr(NbdRequestType RequestType) {
    switch(RequestType) {
    case NBD_CMD_READ:
        return "NBD_CMD_READ";
    case NBD_CMD_WRITE:
        return "NBD_CMD_WRITE";
    case NBD_CMD_DISC:
        return "NBD_CMD_DISC";
    case NBD_CMD_FLUSH:
        return "NBD_CMD_FLUSH";
    case NBD_CMD_TRIM:
        return "NBD_CMD_TRIM";
    default:
        return "UNKNOWN";
    }
}
