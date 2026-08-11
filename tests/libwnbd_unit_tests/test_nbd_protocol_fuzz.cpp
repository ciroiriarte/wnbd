#include "pch.h"

#include <array>
#include <vector>

DWORD NbdParseOptGoReply(
    _In_ UINT32 ReplyType,
    _In_reads_bytes_opt_(DataLength) const CHAR* Data,
    _In_ UINT32 DataLength,
    _Inout_ PUINT64 Size,
    _Inout_ PUINT16 Flags,
    _Inout_ PBOOLEAN SeenExport);

namespace {

UINT16 Swap16(UINT16 Value)
{
    return (UINT16) (((Value & 0x00ff) << 8) |
                     ((Value & 0xff00) >> 8));
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

std::vector<CHAR> Prefix(
    const std::array<CHAR, 12>& Data,
    size_t Length)
{
    return std::vector<CHAR>(Data.begin(), Data.begin() + Length);
}

struct OptGoSeed {
    const char* Name;
    UINT32 ReplyType;
    std::vector<CHAR> Data;
    BOOLEAN InitialSeenExport;
    DWORD ExpectedResult;
    BOOLEAN ExpectedSeenExport;
};

std::vector<OptGoSeed> OptGoSeedCorpus()
{
    const auto ValidExport = ExportInfo(0x1122334455667788ULL,
                                        NBD_FLAG_HAS_FLAGS |
                                        NBD_FLAG_SEND_FLUSH);
    std::vector<OptGoSeed> Seeds = {
        {
            "valid export info",
            NBD_REP_INFO,
            std::vector<CHAR>(ValidExport.begin(), ValidExport.end()),
            FALSE,
            0,
            TRUE,
        },
        {
            "ack after export info",
            NBD_REP_ACK,
            {},
            TRUE,
            0,
            TRUE,
        },
        {
            "ack before export info",
            NBD_REP_ACK,
            {},
            FALSE,
            ERROR_BAD_FORMAT,
            FALSE,
        },
        {
            "ack with payload",
            NBD_REP_ACK,
            { 'x' },
            TRUE,
            ERROR_BAD_FORMAT,
            TRUE,
        },
        {
            "unknown reply with payload",
            0x7fffffff,
            { 'x', 'y', 'z' },
            FALSE,
            0,
            FALSE,
        },
    };

    for (size_t Length = 0; Length < ValidExport.size(); ++Length) {
        Seeds.push_back({
            "truncated export info",
            NBD_REP_INFO,
            Prefix(ValidExport, Length),
            FALSE,
            ERROR_BAD_FORMAT,
            FALSE,
        });
    }
    return Seeds;
}

void ExpectAckInvariant(
    const std::vector<CHAR>& Data,
    BOOLEAN InitialSeenExport)
{
    UINT64 Size = 0xaaaaaaaaaaaaaaaaULL;
    UINT16 Flags = 0xbbbb;
    BOOLEAN SeenExport = InitialSeenExport;

    DWORD Result = NbdParseOptGoReply(
        NBD_REP_ACK,
        Data.empty() ? nullptr : Data.data(),
        (UINT32) Data.size(),
        &Size,
        &Flags,
        &SeenExport);

    if (!Data.empty()) {
        EXPECT_EQ(ERROR_BAD_FORMAT, Result);
    } else if (InitialSeenExport) {
        EXPECT_EQ(0, Result);
    } else {
        EXPECT_EQ(ERROR_BAD_FORMAT, Result);
    }
    EXPECT_EQ(InitialSeenExport, SeenExport);
    EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, Size);
    EXPECT_EQ(0xbbbb, Flags);
}

void ExpectInfoInvariant(
    const std::vector<CHAR>& Data)
{
    UINT64 Size = 0xaaaaaaaaaaaaaaaaULL;
    UINT16 Flags = 0xbbbb;
    BOOLEAN SeenExport = FALSE;

    DWORD Result = NbdParseOptGoReply(
        NBD_REP_INFO,
        Data.empty() ? nullptr : Data.data(),
        (UINT32) Data.size(),
        &Size,
        &Flags,
        &SeenExport);

    const bool HasInfoType = Data.size() >= sizeof(UINT16);
    UINT16 InfoType = 0xffff;
    if (HasInfoType) {
        memcpy(&InfoType, Data.data(), sizeof(InfoType));
        InfoType = Swap16(InfoType);
    }

    if (!HasInfoType) {
        EXPECT_EQ(ERROR_BAD_FORMAT, Result);
        EXPECT_FALSE(SeenExport);
    } else if (InfoType == NBD_INFO_EXPORT) {
        if (Data.size() == 12) {
            EXPECT_EQ(0, Result);
            EXPECT_TRUE(SeenExport);
        } else {
            EXPECT_EQ(ERROR_BAD_FORMAT, Result);
            EXPECT_FALSE(SeenExport);
        }
    } else {
        EXPECT_EQ(0, Result);
        EXPECT_FALSE(SeenExport);
        EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, Size);
        EXPECT_EQ(0xbbbb, Flags);
    }
}

std::vector<CHAR> MutatedPayload(UINT32 Seed, size_t Length)
{
    std::vector<CHAR> Data(Length);
    UINT32 State = Seed;
    for (size_t Index = 0; Index < Length; ++Index) {
        State = State * 1664525u + 1013904223u;
        Data[Index] = (CHAR) (State >> 24);
    }
    return Data;
}

} // namespace

TEST(TestNbdProtocolFuzz, SeedCorpusPreservesOptGoStateMachineInvariants)
{
    for (const auto& Seed : OptGoSeedCorpus()) {
        SCOPED_TRACE(Seed.Name);
        UINT64 Size = 0;
        UINT16 Flags = 0;
        BOOLEAN SeenExport = Seed.InitialSeenExport;

        EXPECT_EQ(Seed.ExpectedResult,
                  NbdParseOptGoReply(
                      Seed.ReplyType,
                      Seed.Data.empty() ? nullptr : Seed.Data.data(),
                      (UINT32) Seed.Data.size(),
                      &Size,
                      &Flags,
                      &SeenExport));
        EXPECT_EQ(Seed.ExpectedSeenExport, SeenExport);
    }
}

TEST(TestNbdProtocolFuzz, DeterministicMutationsPreserveAckAndInfoInvariants)
{
    for (UINT32 Seed = 0; Seed < 64; ++Seed) {
        for (size_t Length = 0; Length <= 64; ++Length) {
            SCOPED_TRACE(::testing::Message()
                         << "seed=" << Seed << " length=" << Length);
            const auto Data = MutatedPayload(Seed, Length);

            ExpectAckInvariant(Data, FALSE);
            ExpectAckInvariant(Data, TRUE);
            ExpectInfoInvariant(Data);

            UINT64 Size = 0xaaaaaaaaaaaaaaaaULL;
            UINT16 Flags = 0xbbbb;
            BOOLEAN SeenExport = FALSE;
            EXPECT_EQ(0,
                      NbdParseOptGoReply(
                          0x7fffffff,
                          Data.empty() ? nullptr : Data.data(),
                          (UINT32) Data.size(),
                          &Size,
                          &Flags,
                          &SeenExport));
            EXPECT_FALSE(SeenExport);
            EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, Size);
            EXPECT_EQ(0xbbbb, Flags);
        }
    }
}
