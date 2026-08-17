#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Read-only live-world replication inspection helpers.
 *
 * These handlers deliberately report generic AActor state only. They do not
 * know about a project's cell, worker, authority, or handoff types, which
 * keeps the observation surface reusable across UE-MCP projects.
 */
class FReplicationInspectionHandlers
{
public:
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

private:
	static TSharedPtr<FJsonValue> ReadActorReplicationSnapshot(const TSharedPtr<FJsonObject>& Params);
};
