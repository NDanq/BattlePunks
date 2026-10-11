#include "Public/Collab/CollabProtocol.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace Collab
{
namespace
{
    void SerializeStrings(EngineCore::Archive& Ar, std::vector<std::string>& InOutList)
    {
        uint32_t Count = uint32_t(InOutList.size());
        Ar << Count;
        if(Ar.IsLoading())
        {
            if(Count > MaxListEntries)
            {
                Ar.SetError("Collab: list too long");
                return;
            }
            InOutList.assign(Count, std::string());
        }
        for(std::string& Each : InOutList)
        {
            Ar << Each;
        }
    }

    template<typename TEnum>
    void SerializeEnum(EngineCore::Archive& Ar, TEnum& InOutValue)
    {
        uint8_t Raw = uint8_t(InOutValue);
        Ar << Raw;
        InOutValue = TEnum(Raw);
    }

    std::string Lower(std::string InText)
    {
        for(char& Each : InText)
        {
            Each = char(std::tolower(static_cast<unsigned char>(Each)));
        }
        return InText;
    }
}

const char* GetRejectReasonName(const ERejectReason InReason)
{
    switch(InReason)
    {
        case ERejectReason::BadHello:       return "bad hello";
        case ERejectReason::Protocol:       return "protocol version";
        case ERejectReason::Password:       return "password";
        case ERejectReason::EngineMismatch: return "engine mismatch";
        case ERejectReason::EngineDirty:    return "uncommitted engine changes";
        case ERejectReason::Full:           return "server full";
    }
    return "unknown";
}

void HelloMessage::Serialize(EngineCore::Archive& Ar)
{
    Ar << Protocol << User << ProjectId << Branch << Head << EngineTree << bEngineDirty << Password;
}

void MemberState::Serialize(EngineCore::Archive& Ar)
{
    Ar << MemberId << User << Head << OpenLevel;
    SerializeEnum(Ar, Play);
    SerializeStrings(Ar, Dirty);
}

void WelcomeMessage::Serialize(EngineCore::Archive& Ar)
{
    Ar << SessionId << YourId << EngineTree;
    uint32_t Count = uint32_t(Roster.size());
    Ar << Count;
    if(Ar.IsLoading())
    {
        if(Count > MaxListEntries)
        {
            Ar.SetError("Collab: roster too long");
            return;
        }
        Roster.assign(Count, MemberState());
    }
    for(MemberState& Each : Roster)
    {
        Each.Serialize(Ar);
    }
}

void RejectMessage::Serialize(EngineCore::Archive& Ar)
{
    SerializeEnum(Ar, Reason);
    Ar << Text;
}

void PresenceMessage::Serialize(EngineCore::Archive& Ar)
{
    Ar << MemberId << Head << OpenLevel;
    SerializeEnum(Ar, Play);
    Ar << bFullDirty;
    SerializeStrings(Ar, DirtyAdded);
    SerializeStrings(Ar, DirtyRemoved);
}

void MemberJoinedMessage::Serialize(EngineCore::Archive& Ar)
{
    Member.Serialize(Ar);
}

void MemberLeftMessage::Serialize(EngineCore::Archive& Ar)
{
    Ar << MemberId << Reason;
}

void NotifyMessage::Serialize(EngineCore::Archive& Ar)
{
    SerializeEnum(Ar, Severity);
    Ar << Title << Body;
}

EMessage PeekType(const std::vector<uint8_t>& InBytes)
{
    EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
    uint16_t Version = 0;
    if(!Ar.OpenChunk(MessageTag, Version))
    {
        return EMessage::None;
    }
    uint16_t Type = 0;
    Ar << Type;
    return Ar.HasError() ? EMessage::None : EMessage(Type);
}

std::string NormalizeRemoteUrl(const std::string& InUrl)
{
    std::string Url = InUrl;
    /* Trim. */
    while(!Url.empty() && std::isspace(static_cast<unsigned char>(Url.back())))  { Url.pop_back(); }
    while(!Url.empty() && std::isspace(static_cast<unsigned char>(Url.front()))) { Url.erase(Url.begin()); }

    std::string Host;
    std::string Path;
    const size_t Scheme = Url.find("://");
    if(Scheme != std::string::npos)
    {
        /* scheme://[user@]host[:port]/path */
        const std::string Rest = Url.substr(Scheme + 3);
        const size_t Slash = Rest.find('/');
        std::string Authority = Rest.substr(0, Slash);
        Path = Slash == std::string::npos ? std::string() : Rest.substr(Slash + 1);
        const size_t At = Authority.rfind('@');
        if(At != std::string::npos) { Authority = Authority.substr(At + 1); }
        const size_t Colon = Authority.find(':');
        if(Colon != std::string::npos) { Authority = Authority.substr(0, Colon); }
        Host = Lower(Authority);
        if(Lower(Url.substr(0, Scheme)) == "file")
        {
            Host.clear();
            Path = Rest;
        }
    }
    else
    {
        /* scp syntax, [user@]host:path -- unless it is a Windows drive ("C:\...") or has no colon. */
        const size_t Colon = Url.find(':');
        const size_t FirstSlash = Url.find_first_of("/\\");
        const bool bDrive = Colon == 1 && std::isalpha(static_cast<unsigned char>(Url[0]));
        if(Colon != std::string::npos && !bDrive && (FirstSlash == std::string::npos || Colon < FirstSlash))
        {
            std::string Authority = Url.substr(0, Colon);
            const size_t At = Authority.rfind('@');
            if(At != std::string::npos) { Authority = Authority.substr(At + 1); }
            Host = Lower(Authority);
            Path = Url.substr(Colon + 1);
        }
        else
        {
            Path = Url;
        }
    }

    std::replace(Path.begin(), Path.end(), '\\', '/');
    while(!Path.empty() && Path.back() == '/') { Path.pop_back(); }
    if(Path.size() > 4 && Lower(Path.substr(Path.size() - 4)) == ".git") { Path.resize(Path.size() - 4); }
    while(!Path.empty() && Path.front() == '/' && !Host.empty()) { Path.erase(Path.begin()); }

    return Host.empty() ? Path : Host + "/" + Path;
}

std::string MakeProjectId(const std::string& InModule, const std::string& InRemoteUrl)
{
    return InModule + "@" + NormalizeRemoteUrl(InRemoteUrl);
}

bool ConstantTimeEquals(const std::string& InA, const std::string& InB)
{
    if(InA.size() != InB.size())
    {
        return false;
    }
    unsigned char Difference = 0;
    for(size_t Index = 0; Index < InA.size(); ++Index)
    {
        Difference |= static_cast<unsigned char>(InA[Index] ^ InB[Index]);
    }
    return Difference == 0;
}

void SortUnique(std::vector<std::string>& InOutList)
{
    std::sort(InOutList.begin(), InOutList.end());
    InOutList.erase(std::unique(InOutList.begin(), InOutList.end()), InOutList.end());
}

void DiffSorted(const std::vector<std::string>& InOld, const std::vector<std::string>& InNew,
                std::vector<std::string>& OutAdded, std::vector<std::string>& OutRemoved)
{
    OutAdded.clear();
    OutRemoved.clear();
    std::set_difference(InNew.begin(), InNew.end(), InOld.begin(), InOld.end(), std::back_inserter(OutAdded));
    std::set_difference(InOld.begin(), InOld.end(), InNew.begin(), InNew.end(), std::back_inserter(OutRemoved));
}

void ApplyDiff(std::vector<std::string>& InOutList, const std::vector<std::string>& InAdded, const std::vector<std::string>& InRemoved)
{
    std::vector<std::string> Removed = InRemoved;
    SortUnique(Removed);
    std::vector<std::string> Kept;
    std::set_difference(InOutList.begin(), InOutList.end(), Removed.begin(), Removed.end(), std::back_inserter(Kept));
    Kept.insert(Kept.end(), InAdded.begin(), InAdded.end());
    SortUnique(Kept);
    InOutList.swap(Kept);
}
}
