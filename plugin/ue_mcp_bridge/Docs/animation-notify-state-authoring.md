# Animation Notify-State Authoring

The animation handlers support point notifies and `UAnimNotifyState` instances
through the existing `read_anim_montage`, `add_anim_notify`, and
`remove_anim_notify` commands.

## Read

`read_anim_montage` reports each event's index, GUID, kind, class path, trigger
time, duration, end time, and track index. Editable notify properties are
returned as flattened dotted paths. Instanced subobjects are traversed one level,
so fields such as `RootMotionModifier.WarpTargetName` are directly inspectable.

## Add or upsert

`add_anim_notify` accepts:

- `assetPath`, `notifyName`, and `triggerTime`
- `notifyClass` as a short class name or full `/Script/...` class path
- optional `notifyGuid`, `duration`, and `trackIndex`
- optional `props`, whose keys may be dotted property paths

When a dotted path begins at a null editable instanced UObject property, create
the owned subobject first with `<property>.@class`. The class must derive from
the property's declared type; existing non-null subobjects are never replaced.

The handler creates a point notify or notify state based on the resolved class.
An existing event is updated when its GUID matches, or when class, name, and
trigger time match within one millisecond. Notify states link both their start
and end times and refresh the animation cache before save.

Example:

```json
{
  "assetPath": "/Game/Animations/M_Attack",
  "notifyName": "MotionWarp",
  "notifyClass": "/Script/Example.AnimNotifyState_ExampleWarp",
  "triggerTime": 1.25,
  "duration": 0.8,
  "trackIndex": 2,
  "props": {
    "RootMotionModifier.@class": "/Script/Example.ExampleWarpModifier",
    "RootMotionModifier.WarpTargetName": "Target",
    "RootMotionModifier.bWarpTranslation": true,
    "RootMotionModifier.RotationType": "Facing"
  }
}
```

Created events return a rollback that removes the exact GUID. Updated events
return an upsert rollback containing the previous timing, track, class, and
modified property values.

## Remove

`remove_anim_notify` accepts any combination of `notifyName`, `notifyClass`,
`notifyGuid`, `notifyIndex`, and `triggerTime`; supplied filters are combined.
Class matching works for point notifies and notify states. A single removed
event returns a complete `add_anim_notify` rollback payload.
