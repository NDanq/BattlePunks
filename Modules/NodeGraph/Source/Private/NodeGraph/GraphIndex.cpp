#include "Private/NodeGraph/GraphIndex.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace NodeGraph
{
    GraphIndex::GraphIndex(const Graph& InGraph, const Schema& InSchema)
        : G(InGraph)
        , S(InSchema)
    {
        for(const Node& Each : G.Nodes)
        {
            Nodes.emplace(Each.Id, &Each);   // a duplicate id keeps the first; ValidateGraph reports it
            Sorted.push_back(&Each);
        }
        std::stable_sort(Sorted.begin(), Sorted.end(), [](const Node* L, const Node* R) { return L->Id < R->Id; });

        for(const Link& Each : G.Links)
        {
            Into.emplace(Each.To, &Each);
            OutOf[Each.From].push_back(&Each);
        }
    }

    const Node* GraphIndex::FindNode(const NodeGuid& InId) const
    {
        const auto Found = Nodes.find(InId);
        return Found == Nodes.end() ? nullptr : Found->second;
    }

    const NodeDefinition* GraphIndex::FindDefinition(const Node& InNode) const
    {
        return InNode.IsPlaceholder() ? nullptr : S.FindNode(InNode.Type);
    }

    const Link* GraphIndex::LinkInto(const NodeGuid& InNode, const std::string& InPin) const
    {
        const auto Found = Into.find(PinRef{ InNode, InPin });
        return Found == Into.end() ? nullptr : Found->second;
    }

    const std::vector<const Link*>& GraphIndex::LinksOutOf(const NodeGuid& InNode, const std::string& InPin) const
    {
        static const std::vector<const Link*> None;
        const auto Found = OutOf.find(PinRef{ InNode, InPin });
        return Found == OutOf.end() ? None : Found->second;
    }

    std::string GraphIndex::PinType(const Node& InNode, const PinDefinition& InPin) const
    {
        if(InPin.Type != WILDCARD_TYPE) { return InPin.Type; }

        const auto Cached = WildcardCache.find(&InNode);
        if(Cached != WildcardCache.end()) { return Cached->second; }

        const NodeDefinition* Def = FindDefinition(InNode);
        std::string Type = Def ? S.ResolveWildcard(G, InNode, *Def) : std::string();
        WildcardCache.emplace(&InNode, Type);
        return Type;
    }

    // === Literals ===============================================================

    std::string FormatNumber(const double InValue)
    {
        char Buffer[64];
        const std::to_chars_result Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), InValue);
        return std::string(Buffer, Result.ptr);
    }

    std::string FormatNumber(const float InValue)
    {
        char Buffer[64];
        const std::to_chars_result Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), InValue);
        return std::string(Buffer, Result.ptr);
    }

    bool ParseNumber(const std::string& InText, double& OutValue)
    {
        /* from_chars rejects a leading '+'; a user typing one means a number. */
        const char* Begin = InText.c_str();
        const char* End   = Begin + InText.size();
        if(Begin != End && *Begin == '+') { ++Begin; }
        if(Begin == End) { return false; }

        double Parsed = 0.0;
        const std::from_chars_result Result = std::from_chars(Begin, End, Parsed);
        if(Result.ec != std::errc() || Result.ptr != End || !std::isfinite(Parsed)) { return false; }
        OutValue = Parsed;
        return true;
    }

    bool ParseNumbers(const std::string& InText, const size_t InCount, std::vector<double>& OutValues)
    {
        std::vector<double> Values;
        size_t Pos = 0;
        while(Pos < InText.size())
        {
            while(Pos < InText.size() && (InText[Pos] == ' ' || InText[Pos] == '\t' || InText[Pos] == ',')) { ++Pos; }
            if(Pos >= InText.size()) { break; }

            size_t End = Pos;
            while(End < InText.size() && InText[End] != ' ' && InText[End] != '\t' && InText[End] != ',') { ++End; }

            double Value = 0.0;
            if(!ParseNumber(InText.substr(Pos, End - Pos), Value)) { return false; }
            Values.push_back(Value);
            Pos = End;
        }

        if(Values.empty())
        {
            OutValues.assign(InCount, 0.0);
            return true;
        }
        if(Values.size() == 1)
        {
            OutValues.assign(InCount, Values[0]);
            return true;
        }
        if(Values.size() != InCount) { return false; }
        OutValues = std::move(Values);
        return true;
    }

    std::string SanitizeIdentifier(const std::string& InText)
    {
        std::string Result;
        for(const char Ch : InText)
        {
            const bool bOk = (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') || Ch == '_';
            Result += bOk ? Ch : '_';
        }
        if(Result.empty() || (Result[0] >= '0' && Result[0] <= '9')) { Result.insert(Result.begin(), '_'); }
        return Result;
    }

    std::string ShortGuid(const NodeGuid& InGuid)
    {
        return GuidToString(InGuid).substr(0, 8);
    }
}
