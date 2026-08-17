#include "ReplicationInspectionHandlers.h"

#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
	constexpr int32 DefaultMaxResults = 256;
	constexpr int32 MaxMaxResults = 512;
	constexpr int32 DefaultMaxScanned = 10000;
	constexpr int32 MaxMaxScanned = 10000;
	constexpr int32 MaxSelectorValues = 512;

	FString NetRoleToString(const ENetRole Role)
	{
		switch (Role)
		{
		case ROLE_None:           return TEXT("None");
		case ROLE_SimulatedProxy: return TEXT("SimulatedProxy");
		case ROLE_AutonomousProxy:return TEXT("AutonomousProxy");
		case ROLE_Authority:      return TEXT("Authority");
		default:                  return TEXT("Unknown");
		}
	}

	FString DormancyToString(const ENetDormancy Dormancy)
	{
		switch (Dormancy)
		{
		case DORM_Never:          return TEXT("DORM_Never");
		case DORM_Awake:          return TEXT("DORM_Awake");
		case DORM_DormantAll:     return TEXT("DORM_DormantAll");
		case DORM_DormantPartial: return TEXT("DORM_DormantPartial");
		case DORM_Initial:        return TEXT("DORM_Initial");
		default:                  return TEXT("Unknown");
		}
	}

	FString WorldTypeToString(const UWorld* World)
	{
		if (!World) return TEXT("unknown");
		switch (World->WorldType)
		{
		case EWorldType::PIE:    return TEXT("pie");
		case EWorldType::Game:   return TEXT("game");
		case EWorldType::Editor: return TEXT("editor");
		default:                 return TEXT("other");
		}
	}

	UWorld* FindExactWorld(const EWorldType::Type WorldType)
	{
		if (!GEngine) return nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate && Candidate->WorldType == WorldType)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	bool ReadBoundedInteger(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Key,
		const int32 DefaultValue,
		const int32 MaxValue,
		int32& OutValue,
		FString& OutError)
	{
		OutValue = DefaultValue;
		if (!Params->HasField(Key)) return true;

		double RawValue = 0.0;
		if (!Params->TryGetNumberField(Key, RawValue) || RawValue < 1.0 || RawValue > static_cast<double>(MaxValue) || FMath::FloorToDouble(RawValue) != RawValue)
		{
			OutError = FString::Printf(TEXT("'%s' must be an integer from 1 to %d"), Key, MaxValue);
			return false;
		}

		OutValue = static_cast<int32>(RawValue);
		return true;
	}

	bool ReadStringArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Key,
		TArray<FString>& OutValues,
		bool& bOutPresent,
		FString& OutError)
	{
		bOutPresent = Params->HasField(Key);
		if (!bOutPresent) return true;

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(Key, Values) || !Values)
		{
			OutError = FString::Printf(TEXT("'%s' must be an array of non-empty strings"), Key);
			return false;
		}
		if (Values->Num() > MaxSelectorValues)
		{
			OutError = FString::Printf(
				TEXT("'%s' may contain at most %d values"),
				Key,
				MaxSelectorValues);
			return false;
		}

		OutValues.Reserve(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue) || StringValue.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' must contain only non-empty strings"), Key);
				return false;
			}
			OutValues.Add(StringValue.TrimStartAndEnd());
		}

		return true;
	}

	bool ReadStrictVector(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Key,
		FVector& OutVector,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* VectorObject = nullptr;
		if (!Object->TryGetObjectField(Key, VectorObject) || !VectorObject || !VectorObject->IsValid())
		{
			OutError = FString::Printf(TEXT("bounds.%s must be an object with numeric x, y, and z fields"), Key);
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!(*VectorObject)->TryGetNumberField(TEXT("x"), X) ||
			!(*VectorObject)->TryGetNumberField(TEXT("y"), Y) ||
			!(*VectorObject)->TryGetNumberField(TEXT("z"), Z) ||
			!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
		{
			OutError = FString::Printf(TEXT("bounds.%s must contain numeric x, y, and z fields"), Key);
			return false;
		}

		OutVector = FVector(X, Y, Z);
		return true;
	}

	UClass* ResolveActorClass(const FString& ClassName)
	{
		UClass* TargetClass = FindClassByShortName(ClassName);
		if (!TargetClass) TargetClass = LoadClass<UObject>(nullptr, *ClassName);
		if (!TargetClass) TargetClass = LoadObject<UClass>(nullptr, *ClassName);
		if (!TargetClass && !ClassName.EndsWith(TEXT("_C")) && ClassName.StartsWith(TEXT("/")))
		{
			TargetClass = LoadObject<UClass>(nullptr, *(ClassName + TEXT("_C")));
		}
		return TargetClass;
	}

	bool MatchesClass(const AActor* Actor, const FString& ClassName, const UClass* TargetClass)
	{
		if (ClassName.IsEmpty()) return true;
		if (TargetClass) return Actor->GetClass()->IsChildOf(TargetClass);
		return Actor->GetClass()->GetName() == ClassName || Actor->GetClass()->GetPathName() == ClassName;
	}

	TSharedPtr<FJsonObject> SerializeActor(const AActor* Actor)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), Actor->GetPathName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("name"), Actor->GetName());
		Result->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		Result->SetStringField(TEXT("classPath"), Actor->GetClass()->GetPathName());
		Result->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString());
		Result->SetObjectField(TEXT("location"), MCPVec3ToJsonObject(Actor->GetActorLocation()));
		Result->SetObjectField(TEXT("rotation"), MCPRotatorToJsonObject(Actor->GetActorRotation()));
		Result->SetObjectField(TEXT("scale"), MCPVec3ToJsonObject(Actor->GetActorScale3D()));
		Result->SetObjectField(TEXT("velocity"), MCPVec3ToJsonObject(Actor->GetVelocity()));

		Result->SetStringField(TEXT("localRole"), NetRoleToString(Actor->GetLocalRole()));
		Result->SetStringField(TEXT("remoteRole"), NetRoleToString(Actor->GetRemoteRole()));
		Result->SetBoolField(TEXT("replicates"), Actor->GetIsReplicated());
		Result->SetBoolField(TEXT("replicateMovement"), Actor->IsReplicatingMovement());
		Result->SetStringField(TEXT("dormancy"), DormancyToString(Actor->NetDormancy));
		Result->SetNumberField(TEXT("netDormancy"), static_cast<int32>(Actor->NetDormancy));
		Result->SetNumberField(TEXT("netUpdateFrequency"), Actor->GetNetUpdateFrequency());
		Result->SetNumberField(TEXT("minNetUpdateFrequency"), Actor->GetMinNetUpdateFrequency());
		Result->SetNumberField(TEXT("netPriority"), Actor->NetPriority);
		Result->SetNumberField(TEXT("netCullDistanceSquared"), Actor->GetNetCullDistanceSquared());
		Result->SetBoolField(TEXT("alwaysRelevant"), Actor->bAlwaysRelevant);
		Result->SetBoolField(TEXT("onlyRelevantToOwner"), Actor->bOnlyRelevantToOwner);
		auto Relevancy = MakeShared<FJsonObject>();
		Relevancy->SetBoolField(TEXT("alwaysRelevant"), Actor->bAlwaysRelevant);
		Relevancy->SetBoolField(TEXT("onlyRelevantToOwner"), Actor->bOnlyRelevantToOwner);
		Relevancy->SetNumberField(TEXT("netCullDistanceSquared"), Actor->GetNetCullDistanceSquared());
		Result->SetObjectField(TEXT("relevancy"), Relevancy);
		Result->SetStringField(TEXT("ownerPath"), Actor->GetOwner() ? Actor->GetOwner()->GetPathName() : FString());

		TArray<FString> Tags;
		Tags.Reserve(Actor->Tags.Num());
		for (const FName& Tag : Actor->Tags) Tags.Add(Tag.ToString());
		Tags.Sort();
		TArray<TSharedPtr<FJsonValue>> TagValues;
		TagValues.Reserve(Tags.Num());
		for (const FString& Tag : Tags) TagValues.Add(MakeShared<FJsonValueString>(Tag));
		Result->SetArrayField(TEXT("tags"), TagValues);

		return Result;
	}
}

void FReplicationInspectionHandlers::RegisterHandlers(FMCPHandlerRegistry& Registry)
{
	Registry.RegisterHandler(TEXT("read_actor_replication_snapshot"), &ReadActorReplicationSnapshot);
}

TSharedPtr<FJsonValue> FReplicationInspectionHandlers::ReadActorReplicationSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	const FString RequestedWorld = OptionalString(Params, TEXT("world"), TEXT("pie")).TrimStartAndEnd().ToLower();
	const bool bWorldWasOmitted = !Params->HasField(TEXT("world"));
	UWorld* World = nullptr;
	if (RequestedWorld == TEXT("pie"))
	{
		World = FindExactWorld(EWorldType::PIE);
		if (!World && bWorldWasOmitted)
		{
			World = FindExactWorld(EWorldType::Editor);
		}
	}
	else if (RequestedWorld == TEXT("game"))
	{
		World = FindExactWorld(EWorldType::Game);
	}
	else if (RequestedWorld == TEXT("editor"))
	{
		World = FindExactWorld(EWorldType::Editor);
	}
	else
	{
		return MCPError(TEXT("world must be 'pie', 'game', or 'editor'"));
	}
	if (!World) return MCPError(FString::Printf(TEXT("Requested world is not available: %s"), *RequestedWorld));

	TArray<FString> ActorLabels;
	TArray<FString> ActorPaths;
	bool bHasActorLabels = false;
	bool bHasActorPaths = false;
	FString Error;
	if (!ReadStringArray(Params, TEXT("actorLabels"), ActorLabels, bHasActorLabels, Error)) return MCPError(Error);
	if (!ReadStringArray(Params, TEXT("actorPaths"), ActorPaths, bHasActorPaths, Error)) return MCPError(Error);

	const FString ClassName = OptionalString(Params, TEXT("className")).TrimStartAndEnd();
	const FString TagName = OptionalString(Params, TEXT("tag")).TrimStartAndEnd();
	const bool bHasClassName = !ClassName.IsEmpty();
	const bool bHasTag = !TagName.IsEmpty();
	const bool bAll = OptionalBool(Params, TEXT("all"), false);
	const bool bHasBounds = Params->HasField(TEXT("bounds"));
	const bool bHasSelector = (bHasActorLabels && ActorLabels.Num() > 0) ||
		(bHasActorPaths && ActorPaths.Num() > 0) || bHasClassName || bHasTag || bHasBounds;
	if (bAll && bHasSelector) return MCPError(TEXT("'all' cannot be combined with actor selectors"));
	if (!bAll && !bHasSelector) return MCPError(TEXT("Provide actorLabels, actorPaths, className, tag, bounds, or all=true"));
	if ((bHasActorLabels && ActorLabels.Num() == 0) || (bHasActorPaths && ActorPaths.Num() == 0))
	{
		return MCPError(TEXT("actorLabels and actorPaths must contain at least one value when provided"));
	}

	FBox Bounds(EForceInit::ForceInit);
	if (bHasBounds)
	{
		const TSharedPtr<FJsonObject>* BoundsObject = nullptr;
		if (!Params->TryGetObjectField(TEXT("bounds"), BoundsObject) || !BoundsObject || !BoundsObject->IsValid())
		{
			return MCPError(TEXT("bounds must be an object containing min and max vectors"));
		}
		FVector Min;
		FVector Max;
		if (!ReadStrictVector(*BoundsObject, TEXT("min"), Min, Error) || !ReadStrictVector(*BoundsObject, TEXT("max"), Max, Error))
		{
			return MCPError(Error);
		}
		if (Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
		{
			return MCPError(TEXT("bounds.min must not exceed bounds.max"));
		}
		Bounds = FBox(Min, Max);
	}

	int32 MaxResults = DefaultMaxResults;
	if (!ReadBoundedInteger(Params, TEXT("maxResults"), DefaultMaxResults, MaxMaxResults, MaxResults, Error)) return MCPError(Error);
	int32 MaxScanned = DefaultMaxScanned;
	if (!ReadBoundedInteger(Params, TEXT("maxScanned"), DefaultMaxScanned, MaxMaxScanned, MaxScanned, Error)) return MCPError(Error);

	const UClass* TargetClass = bHasClassName ? ResolveActorClass(ClassName) : nullptr;
	TSet<FString> LabelSet;
	for (const FString& Label : ActorLabels) LabelSet.Add(Label);
	TSet<FString> PathSet;
	for (const FString& Path : ActorPaths) PathSet.Add(Path);

	TArray<AActor*> Matches;
	int32 Scanned = 0;
	int32 Matched = 0;
	bool bScanTruncated = false;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (Scanned >= MaxScanned)
		{
			bScanTruncated = true;
			break;
		}
		++Scanned;

		AActor* Actor = *It;
		if (!Actor) continue;
		if (bHasActorLabels && !LabelSet.Contains(Actor->GetActorLabel())) continue;
		if (bHasActorPaths && !PathSet.Contains(Actor->GetPathName())) continue;
		if (!MatchesClass(Actor, ClassName, TargetClass)) continue;
		if (bHasTag && !Actor->ActorHasTag(FName(*TagName))) continue;
		if (bHasBounds && !Bounds.IsInsideOrOn(Actor->GetActorLocation())) continue;

		++Matched;
		Matches.Add(Actor);
	}

	Matches.Sort([](const AActor& A, const AActor& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	const bool bTruncated = Matches.Num() > MaxResults;
	if (bTruncated) Matches.SetNum(MaxResults);

	TArray<TSharedPtr<FJsonValue>> ActorValues;
	ActorValues.Reserve(Matches.Num());
	for (const AActor* Actor : Matches)
	{
		ActorValues.Add(MakeShared<FJsonValueObject>(SerializeActor(Actor)));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("worldType"), WorldTypeToString(World));
	Result->SetNumberField(TEXT("timeSeconds"), World->GetTimeSeconds());
	Result->SetNumberField(TEXT("scanned"), Scanned);
	Result->SetNumberField(TEXT("matched"), Matched);
	Result->SetNumberField(TEXT("returned"), Matches.Num());
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetBoolField(TEXT("scanTruncated"), bScanTruncated);
	Result->SetArrayField(TEXT("actors"), ActorValues);
	return MCPResult(Result);
}

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReplicationInspectionSnapshotTest,
	"UE.MCP.ReplicationInspection.Snapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FReplicationInspectionSnapshotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FMCPHandlerRegistry Registry;
	FReplicationInspectionHandlers::RegisterHandlers(Registry);
	TestTrue(TEXT("Native snapshot handler is registered"),
		Registry.HasHandler(TEXT("read_actor_replication_snapshot")));

	UWorld* EditorWorld = FindExactWorld(EWorldType::Editor);
	if (!TestNotNull(TEXT("Editor world is available"), EditorWorld))
	{
		return false;
	}
	AWorldSettings* WorldSettings = EditorWorld->GetWorldSettings();
	if (!TestNotNull(TEXT("Editor world settings are available"), WorldSettings))
	{
		return false;
	}

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("world"), TEXT("editor"));
	TArray<TSharedPtr<FJsonValue>> Paths;
	Paths.Add(MakeShared<FJsonValueString>(WorldSettings->GetPathName()));
	Params->SetArrayField(TEXT("actorPaths"), Paths);
	Params->SetNumberField(TEXT("maxResults"), 1);

	const TSharedPtr<FJsonValue> Response = Registry.ExecuteHandler(
		TEXT("read_actor_replication_snapshot"), Params);
	const TSharedPtr<FJsonObject> ResponseObject =
		Response.IsValid() ? Response->AsObject() : nullptr;
	if (!TestTrue(TEXT("Snapshot returns an object"),
		ResponseObject.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Snapshot succeeds"),
		ResponseObject->GetBoolField(TEXT("success")));
	TestEqual(TEXT("Snapshot uses the exact editor world"),
		ResponseObject->GetStringField(TEXT("worldType")),
		FString(TEXT("editor")));
	TestEqual(TEXT("Exact path selector returns one actor"),
		static_cast<int32>(ResponseObject->GetNumberField(TEXT("returned"))),
		1);

	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	if (TestTrue(TEXT("Snapshot contains an actor array"),
		ResponseObject->TryGetArrayField(TEXT("actors"), Actors)
			&& Actors != nullptr && Actors->Num() == 1))
	{
		const TSharedPtr<FJsonObject> Actor = (*Actors)[0]->AsObject();
		TestTrue(TEXT("Serialized actor object is valid"), Actor.IsValid());
		if (Actor.IsValid())
		{
			TestEqual(TEXT("Serialized path is exact"),
				Actor->GetStringField(TEXT("path")),
				WorldSettings->GetPathName());
			TestTrue(TEXT("Replication flag is serialized"),
				Actor->HasTypedField<EJson::Boolean>(TEXT("replicates")));
		}
	}

	auto InvalidBoundsParams = MakeShared<FJsonObject>();
	InvalidBoundsParams->SetStringField(TEXT("world"), TEXT("editor"));
	auto Bounds = MakeShared<FJsonObject>();
	auto Min = MakeShared<FJsonObject>();
	Min->SetNumberField(TEXT("x"), 1.0);
	Min->SetNumberField(TEXT("y"), 0.0);
	Min->SetNumberField(TEXT("z"), 0.0);
	auto Max = MakeShared<FJsonObject>();
	Max->SetNumberField(TEXT("x"), 0.0);
	Max->SetNumberField(TEXT("y"), 0.0);
	Max->SetNumberField(TEXT("z"), 0.0);
	Bounds->SetObjectField(TEXT("min"), Min);
	Bounds->SetObjectField(TEXT("max"), Max);
	InvalidBoundsParams->SetObjectField(TEXT("bounds"), Bounds);
	const TSharedPtr<FJsonObject> InvalidBoundsResponse =
		Registry.ExecuteHandler(
			TEXT("read_actor_replication_snapshot"),
			InvalidBoundsParams)->AsObject();
	TestFalse(TEXT("Inverted bounds fail closed"),
		InvalidBoundsResponse->GetBoolField(TEXT("success")));
	return true;
}

#endif
