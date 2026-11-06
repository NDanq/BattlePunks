#ifndef AICORE_BLACKBOARD_H
#define AICORE_BLACKBOARD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

/*
 * A brain's memory: typed values by key, declared by its tree. What
 * perception, services and the game write, and what decorators and
 * considerations read. Each write that changes a value bumps the key's
 * revision and the board's, which is what lets a tree re-check its
 * observing decorators only when something they read changed.
 */
namespace AICore
{
    enum class EBlackboardType : uint8_t
    {
        Bool = 0,
        Int,
        Float,
        Vector,
        Entity,
        Name,     // a short string: a state, a tag
        Count,
    };

    AICORE_API const char*     GetBlackboardTypeName(EBlackboardType InType);
    /* By name, case-insensitive; Count if none. */
    AICORE_API EBlackboardType FindBlackboardType(const char* InName);

    /* One key's value. Every field is kept; Type says which one is meant. */
    struct BlackboardValue
    {
        EBlackboardType     Type = EBlackboardType::Bool;
        bool                bValue = false;
        int64_t             IntValue = 0;
        float               FloatValue = 0.0f;
        EngineCore::Vector3 VectorValue;
        GameCore::EntityID  EntityValue;
        std::string         NameValue;
        /* Whether a value has been written (or declared with a default):
         * an IsSet condition. */
        bool                bSet = false;

        /* As a number, whatever the type: bools 0/1, a vector's length, an
         * entity 1 if set. */
        AICORE_API double AsNumber() const;
        AICORE_API std::string ToString() const;
        /* From text, by Type: "true", "3.5", "1,2,3", a name. False if it
         * does not parse (entities never do). */
        AICORE_API bool Parse(const std::string& InText);
        AICORE_API bool Equals(const BlackboardValue& InOther) const;
    };

    struct BlackboardKey
    {
        std::string     Name;
        EBlackboardType Type = EBlackboardType::Bool;
        std::string     Default;   // text, parsed by type; "" for none
    };

    class AICORE_API Blackboard
    {
    public:
        static constexpr int32_t INVALID_KEY = -1;

        Blackboard() = default;

        /* Keys from InKeys (their defaults set); forgets what it held. */
        void Declare(const std::vector<BlackboardKey>& InKeys);

        /* A key, added if it is not declared (a game's own). Its index. */
        int32_t AddKey(const char* InName, EBlackboardType InType);

        int32_t FindKey(const char* InName) const;
        size_t  GetKeyCount() const { return Keys.size(); }
        const BlackboardKey&   GetKey(int32_t InKey) const { return Keys[size_t(InKey)]; }
        const BlackboardValue& GetValue(int32_t InKey) const { return Values[size_t(InKey)]; }
        uint64_t GetKeyRevision(int32_t InKey) const { return Revisions[size_t(InKey)]; }

        /* Bumped by every change of any key. */
        uint64_t GetRevision() const { return Revision; }

        // === By index (fast) ===============================================
        // Writes convert to the key's type; one of a different type is
        // converted where it can be (a number into a bool, an int, a float).
        void SetBool(int32_t InKey, bool bInValue);
        void SetInt(int32_t InKey, int64_t InValue);
        void SetFloat(int32_t InKey, float InValue);
        void SetVector(int32_t InKey, const EngineCore::Vector3& InValue);
        void SetEntity(int32_t InKey, GameCore::EntityID InValue);
        void SetName(int32_t InKey, const std::string& InValue);
        void SetValue(int32_t InKey, const BlackboardValue& InValue);
        /* Back to unset (a cleared target). */
        void Clear(int32_t InKey);
        bool IsSet(int32_t InKey) const;

        // === By name: no-ops (and false/defaults) for a key not declared ===
        void SetBool(const char* InKey, bool bInValue)                      { if(const int32_t K = FindKey(InKey); K >= 0) { SetBool(K, bInValue); } }
        void SetInt(const char* InKey, int64_t InValue)                     { if(const int32_t K = FindKey(InKey); K >= 0) { SetInt(K, InValue); } }
        void SetFloat(const char* InKey, float InValue)                     { if(const int32_t K = FindKey(InKey); K >= 0) { SetFloat(K, InValue); } }
        void SetVector(const char* InKey, const EngineCore::Vector3& InValue) { if(const int32_t K = FindKey(InKey); K >= 0) { SetVector(K, InValue); } }
        void SetEntity(const char* InKey, GameCore::EntityID InValue)       { if(const int32_t K = FindKey(InKey); K >= 0) { SetEntity(K, InValue); } }
        void SetName(const char* InKey, const std::string& InValue)         { if(const int32_t K = FindKey(InKey); K >= 0) { SetName(K, InValue); } }
        void Clear(const char* InKey)                                       { if(const int32_t K = FindKey(InKey); K >= 0) { Clear(K); } }

        bool                GetBool(const char* InKey, bool bInDefault = false) const;
        double              GetNumber(const char* InKey, double InDefault = 0.0) const;
        EngineCore::Vector3 GetVector(const char* InKey) const;
        GameCore::EntityID  GetEntity(const char* InKey) const;
        std::string         GetName(const char* InKey) const;

        /* "Key=Value;Key=Value", each parsed by its key's type; unknown keys
         * are skipped. How many were set. */
        size_t ApplyAssignments(const char* InText);

    private:
        void Touch(int32_t InKey);
        bool Valid(int32_t InKey) const { return InKey >= 0 && size_t(InKey) < Keys.size(); }

        std::vector<BlackboardKey>   Keys;
        std::vector<BlackboardValue> Values;
        std::vector<uint64_t>        Revisions;
        uint64_t                     Revision = 1;
    };
}

#endif //AICORE_BLACKBOARD_H
