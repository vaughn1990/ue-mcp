# Replication inspection handler

`read_actor_replication_snapshot` is a read-only, bounded live-world
observation tool for UE-MCP certification and telemetry. It reports generic
`AActor` transform, motion, ownership, role, replication, relevancy, and tag
state without depending on a project's cell, worker, authority, or handoff
classes.

## Input

The request must provide either at least one selector or `all: true`:

```json
{
  "world": "pie",
  "actorLabels": ["PlayerA", "PlayerB"],
  "actorPaths": [],
  "className": "Pawn",
  "tag": "MeshTest",
  "bounds": {
    "min": {"x": 0, "y": 0, "z": -1000},
    "max": {"x": 10000, "y": 10000, "z": 1000}
  },
  "maxResults": 256,
  "maxScanned": 10000
}
```

`world` accepts `pie`, `game`, or `editor`. When omitted, PIE is preferred and
the editor world is used as a fallback. `actorLabels`, `actorPaths`,
`className`, `tag`, and `bounds` are combined as an AND filter. `bounds` is an
axis-aligned inclusive `{min, max}` box in world units. `all: true` is
mutually exclusive with selectors.

`maxResults` defaults to 256 and is capped at 512. `maxScanned` defaults to
and is capped at 10000. Each label/path selector array is capped at 512 values.
The scan stops at the scan cap; no package or actor is modified.

## Output

The response includes `worldType`, `timeSeconds`, `scanned`, `matched`,
`returned`, `truncated`, `scanTruncated`, and an `actors` array. Each actor
contains its path, label, name, class, level, transform, velocity, local and
remote net roles, owner path, replication flags, dormancy, update frequencies,
priority, cull distance, relevancy flags, and sorted tags. Actor records are
sorted by stable UObject path.

The handler runs through the bridge's normal game-thread dispatch and uses the
default handler timeout. It is intentionally observation-only so it can be
used to verify static cell ownership, ghost eligibility, and handoff state
without embedding project-specific server-mesh logic.
