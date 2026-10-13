#include "Public/AICore/BehaviorTree.h"

#include <algorithm>
#include <cctype>
#include <map>

/* The behavior tree type registry and the name tables. BehaviorTree.h has
 * the contract. */
namespace AICore
{
    namespace
    {
        bool EqualsNoCase(const char* InA, const char* InB)
        {
            if(!InA || !InB) { return false; }
            while(*InA && *InB)
            {
                if(std::tolower(static_cast<unsigned char>(*InA)) != std::tolower(static_cast<unsigned char>(*InB))) { return false; }
                ++InA;
                ++InB;
            }
            return *InA == *InB;
        }

        struct Entry
        {
            const void* Impl = nullptr;
            std::string Display;
        };

        struct Registry
        {
            std::map<std::string, Entry> Tasks;
            std::map<std::string, Entry> Decorators;
            std::map<std::string, Entry> Services;
        };

        /* Function-local: registrations run during static initialisation,
         * in any order across images. */
        Registry& GetRegistry()
        {
            static Registry Instance;
            return Instance;
        }

        std::vector<std::string> Names(const std::map<std::string, Entry>& InMap)
        {
            std::vector<std::string> Result;
            Result.reserve(InMap.size());
            for(const auto& Pair : InMap) { Result.push_back(Pair.first); }
            return Result;
        }

        const char* const GStatusNames[]    = { "Success", "Failure", "Running", "Aborted" };
        const char* const GCompositeNames[] = { "Sequence", "Selector", "Parallel", "RandomSelector", "UtilitySelector" };
        const char* const GAbortNames[]     = { "None", "Self", "LowerPriority", "Both" };
        const char* const GCurveNames[]     = { "Linear", "InverseLinear", "Quadratic", "Logistic", "Step", "Constant", "Points" };
    }

    const char* GetBTStatusName(const EBTStatus InStatus)
    {
        return uint8_t(InStatus) < 4 ? GStatusNames[uint8_t(InStatus)] : "Failure";
    }

    const char* GetBTCompositeName(const EBTComposite InType)
    {
        return InType < EBTComposite::Count ? GCompositeNames[uint8_t(InType)] : "Sequence";
    }

    EBTComposite FindBTComposite(const char* InName)
    {
        for(uint8_t Index = 0; Index < uint8_t(EBTComposite::Count); ++Index)
        {
            if(EqualsNoCase(InName, GCompositeNames[Index])) { return EBTComposite(Index); }
        }
        if(EqualsNoCase(InName, "Fallback")) { return EBTComposite::Selector; }
        return EBTComposite::Count;
    }

    const char* GetBTAbortModeName(const EBTAbortMode InMode)
    {
        return uint8_t(InMode) < 4 ? GAbortNames[uint8_t(InMode)] : "None";
    }

    EBTAbortMode FindBTAbortMode(const char* InName)
    {
        for(uint8_t Index = 0; Index < 4; ++Index)
        {
            if(EqualsNoCase(InName, GAbortNames[Index])) { return EBTAbortMode(Index); }
        }
        return EBTAbortMode::None;
    }

    const char* GetBTCurveName(const EBTCurve InCurve)
    {
        return InCurve < EBTCurve::Count ? GCurveNames[uint8_t(InCurve)] : "Linear";
    }

    EBTCurve FindBTCurve(const char* InName)
    {
        for(uint8_t Index = 0; Index < uint8_t(EBTCurve::Count); ++Index)
        {
            if(EqualsNoCase(InName, GCurveNames[Index])) { return EBTCurve(Index); }
        }
        return EBTCurve::Count;
    }

    // === Registration =========================================================

    void RegisterBTTask(const char* InName, const BTTask* InTask, const char* InDisplayName)
    {
        if(!InName || !InTask) { return; }
        Entry& E = GetRegistry().Tasks[InName];
        E.Impl = InTask;
        E.Display = InDisplayName && InDisplayName[0] ? InDisplayName : InName;
    }

    void RegisterBTDecorator(const char* InName, const BTDecorator* InDecorator)
    {
        if(!InName || !InDecorator) { return; }
        Entry& E = GetRegistry().Decorators[InName];
        E.Impl = InDecorator;
        E.Display = InName;
    }

    void RegisterBTService(const char* InName, const BTService* InService)
    {
        if(!InName || !InService) { return; }
        Entry& E = GetRegistry().Services[InName];
        E.Impl = InService;
        E.Display = InName;
    }

    void UnregisterBTType(const void* InImpl)
    {
        Registry& R = GetRegistry();
        for(std::map<std::string, Entry>* Map : { &R.Tasks, &R.Decorators, &R.Services })
        {
            for(auto It = Map->begin(); It != Map->end();)
            {
                if(It->second.Impl == InImpl) { It = Map->erase(It); }
                else { ++It; }
            }
        }
    }

    const BTTask* FindBTTask(const char* InName)
    {
        const auto& Map = GetRegistry().Tasks;
        const auto Found = InName ? Map.find(InName) : Map.end();
        return Found != Map.end() ? static_cast<const BTTask*>(Found->second.Impl) : nullptr;
    }

    const BTDecorator* FindBTDecorator(const char* InName)
    {
        const auto& Map = GetRegistry().Decorators;
        const auto Found = InName ? Map.find(InName) : Map.end();
        return Found != Map.end() ? static_cast<const BTDecorator*>(Found->second.Impl) : nullptr;
    }

    const BTService* FindBTService(const char* InName)
    {
        const auto& Map = GetRegistry().Services;
        const auto Found = InName ? Map.find(InName) : Map.end();
        return Found != Map.end() ? static_cast<const BTService*>(Found->second.Impl) : nullptr;
    }

    std::vector<std::string> GetBTTaskNames()      { return Names(GetRegistry().Tasks); }
    std::vector<std::string> GetBTDecoratorNames() { return Names(GetRegistry().Decorators); }
    std::vector<std::string> GetBTServiceNames()   { return Names(GetRegistry().Services); }

    std::string GetBTTaskDisplayName(const char* InName)
    {
        const auto& Map = GetRegistry().Tasks;
        const auto Found = InName ? Map.find(InName) : Map.end();
        return Found != Map.end() ? Found->second.Display : std::string(InName ? InName : "");
    }

    BTRegistration::BTRegistration(const EKind InKind, const char* InName, const void* InImpl, const char* InDisplayName)
        : Impl(InImpl)
    {
        switch(InKind)
        {
            case Task:      RegisterBTTask(InName, static_cast<const BTTask*>(InImpl), InDisplayName); break;
            case Decorator: RegisterBTDecorator(InName, static_cast<const BTDecorator*>(InImpl)); break;
            case Service:   RegisterBTService(InName, static_cast<const BTService*>(InImpl)); break;
        }
    }

    BTRegistration::~BTRegistration()
    {
        UnregisterBTType(Impl);
    }
}
