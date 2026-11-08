# OrcaSlicer Remote API

A token-authenticated HTTP + WebSocket API embedded in OrcaSlicer. It exposes the
**running** application for automation: read status, read/write config live, kick
off slicing, and subscribe to a live event stream.

- **Off by default.** Enable it explicitly in Preferences.
- **Single API version:** `v1`, all paths under `/api/v1`.
- **Machine-readable spec:** [`openapi.yaml`](./openapi.yaml).

## Enabling

Preferences → **Remote API**:

| Setting | Meaning |
|---|---|
| **Enable** | Start the server (applied immediately on closing Preferences). |
| **Bind LAN** | Off = listen on `127.0.0.1` only. On = listen on `0.0.0.0` (reachable from other machines on your network). |
| **Port** | Default `13130`. |
| **Token** | 32 hex chars, auto-generated on first run. **Regenerate** invalidates existing clients. |

> Binding to the LAN exposes live control of your slicer to every machine on the
> network. Keep the token secret and prefer localhost unless you need remote access.

## Authentication

- **HTTP:** send the header `X-Api-Token: <token>` on every request. Missing/wrong
  token → `401 {"error":"unauthorized"}`.
- **WebSocket:** pass `?token=<token>` in the URL (browsers can't set WS headers).

## Endpoints

Base URL: `http://<host>:13130/api/v1`

| Method | Path | Purpose |
|---|---|---|
| GET | `/status` | App/project/preset status + slice validity |
| GET | `/config[?keys=a,b]` | Read merged config (secrets redacted) |
| PUT | `/config` | Apply config edits live (atomic) |
| POST | `/slice` | Start slicing the current plate |
| GET | `/slice/status` | Current/last slice state + stats + warnings |
| POST | `/slice/cancel` | Stop a running slice, or clear a stuck `slicing` state |
| GET | `/gcode` | The G-code of the current plate's last slice (`409` until it is sliced) |
| POST | `/model` | Load a model file (`{"path"}`; .stl, .obj, .3mf geometry, .step/.stp) onto the plate |
| GET | `/objects` | Objects on the bed: id, name, size, position, rotation, scale, bounding box, plate |
| DELETE | `/objects/{id}` | Delete an object |
| POST | `/objects/{id}/transform` | Move, rotate (degrees) or scale an object (`translate`, `rotate`, `scale`) |
| POST | `/objects/{id}/duplicate` | Add one more copy of an object |
| PUT | `/objects/{id}/config` | Per-object setting overrides |
| PUT | `/objects/{id}/layer_height` | Adaptive layer height (`{"mode":"adaptive","quality"}`) or reset |
| PUT | `/objects/{id}/height_range` | Settings for a height range of an object (`min_z`, `max_z`, `layer_height`), or `{"clear":true}` |
| POST | `/arrange` | Arrange all (`202`; follow it with `GET /jobs/status`) |
| POST | `/orient` | Auto-orient (`202`; follow it with `GET /jobs/status`) |
| GET | `/jobs/status` | `{"idle"}`: whether an arrange or orient job is still running |
| GET | `/plate/render` | PNG of the current plate: `view=editor\|preview`, `angle=iso\|top\|front\|left\|right\|rear\|bottom`, `frame=plate\|object`, `width`, `height` |
| GET | `/presets` | Preset names per type, with system and selected flags |
| PUT | `/preset` | Select a preset (`{"type":"print\|filament\|printer","name"}`) |
| POST | `/preset/save` | Save the current settings as a user preset (`{"type","name"}`), no dialog |
| POST | `/preset/config` | The full config of a named preset (`{"type","name"}`; credentials redacted) |
| DELETE | `/preset` | Delete a user preset (never a system, selected or inherited-from one) |
| GET | `/printer/slots` | Elegoo Centauri Carbon 2: read the CANVAS trays now |
| GET | `/printer/slot_mapping` | Which tray each filament of the plate will print from |
| PUT | `/printer/slot_mapping` | Set that mapping (`{"mapping":[{"filament","canvas_id","tray_id"}]}`); the Send dialog opens pre-filled |
| WS | `/events?token=…` | Live event stream |
| GET | `/plates` | List the plates: index, name, current, locked, objects on each |
| POST | `/plates` | Add a plate (`{"select": true}` by default) |
| POST | `/plates/{i}/select` | Make plate `i` the current one |
| DELETE | `/plates/{i}` | Delete plate `i` (its objects are parked beside the bed, not deleted) |
| POST | `/objects/{id}/plate` | Move an object onto plate `i` |
| GET | `/project` | Project name, path, dirty flag, plate and object counts |
| POST | `/project/save` | File > Save / Save As (`{"path", "overwrite"}`), no dialogs |
| POST | `/project/open` | File > Open Project (`path` or `recent` index), the unsaved-changes prompt answered by `if_unsaved` |
| POST | `/project/new` | File > New Project, same `if_unsaved`; optional `name` |
| GET | `/project/recent` | File > Recent, most recent first, with whether each file still exists |
| DELETE | `/project/recent` | Forget an entry (`path` or `index`) or the whole list (`all`) |
| GET | `/project/browse?dir=` | The `.3mf` files in a folder, newest first |
| POST | `/project/move` | Move or rename a project file (`{"from", "to", "overwrite"}`); the window follows for the open project |
| DELETE | `/project/file` | Delete a project file from disk (never the open project's) |

### GET /status

```bash
curl -H "X-Api-Token: $TOK" http://localhost:13130/api/v1/status
```
```json
{
  "app": "OrcaSlicer", "app_version": "2.3.2", "api_version": "1.0",
  "capabilities": ["status","config","slice","events"],
  "project": "", "project_name": "Untitled", "project_dirty": false,
  "objects": [{"name":"cube","size_mm":[20,20,20]}],
  "presets": {"printer":"...","print":"...","filaments":["..."]},
  "modified": {"print":["layer_height"],"filament":[],"printer":[]},
  "slice_result_valid": false,
  "slicing": false
}
```

### GET /config

```bash
curl -H "X-Api-Token: $TOK" "http://localhost:13130/api/v1/config?keys=layer_height,wall_loops"
```
```json
{ "config": { "layer_height": "0.2", "wall_loops": "2" } }
```
Omit `?keys=` to get the full merged config. Values are canonical strings (same
form as `.ini`/`.3mf`).

### PUT /config  (atomic)

```bash
curl -X PUT -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"layer_height":0.28,"wall_loops":3}' \
  http://localhost:13130/api/v1/config
```
```json
{ "applied": ["layer_height","wall_loops"], "errors": {} }
```
Applied through the GUI's own edit path (dirty markers + live refresh + slice
invalidation). **All-or-nothing:** if any key is unknown or invalid, nothing is
applied and you get `422` with per-key reasons:
```json
{ "applied": [], "errors": { "layer_height": "value out of range", "nope": "unknown_key" } }
```
Other errors: `400 body_must_be_object`, `400 invalid_json`, `504 ui_timeout`.

#### Per-filament keys

`GET /config` returns the **merged** config, so a vector-valued filament key holds
one run of values per loaded filament slot:

```json
{ "config": { "hot_plate_temp": "70,100,70,70,35" } }
```

`PUT /config`, by contrast, writes the single filament preset open in the Filament
tab. Those two shapes are not interchangeable, so **writing a merged value back is
rejected**:

```json
{ "applied": [], "errors": { "hot_plate_temp": "per_filament_length_mismatch: the filament preset open in the Filament tab holds 1 value(s), got 5. ..." } }
```

Pass only that preset's own value (`{"hot_plate_temp": 70}`), and change the other
slots by selecting their presets in turn. Writing a key back unchanged still works
on a single-filament setup, where merged and per-preset shapes coincide.

Earlier builds accepted the merged string and stored all of it in the one preset.
The next merge then spliced that whole list in at *every* slot using that preset,
so the value grew on each write (5 entries → 13 → …) and the per-slot settings were
lost. If you hit that, re-select the filament preset to restore it.

#### Choosing the filament preset of one slot

`PUT /preset` with `"type": "filament"` switches the preset open in the Filament
tab. On a multi-filament printer, add a 0-based `slot` to choose the preset of one
extruder/tool instead, the same as picking it in that slot's sidebar combo:

```bash
curl -X PUT -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"type":"filament","name":"Generic PETG","slot":1}' \
  http://localhost:13130/api/v1/preset
```
```json
{ "selected": "Generic PETG", "slot": 1, "filaments": ["Generic PLA", "Generic PETG", "Generic PLA", "Generic PLA"] }
```

Only that slot changes; no unsaved edits are discarded. `422` with `slot_out_of_range`
(and `filament_count`), `unknown_preset` or `incompatible_preset` (not compatible with
the selected printer) changes nothing. `slot` with any other `type` is `400 invalid_slot`.
On a single-filament printer `slot: 0` behaves like the request without `slot`.
Builds that support it list `preset_slot` in `GET /status` `capabilities`; older builds
ignore `slot` and switch the Filament tab preset, so check the capability first.

#### Project-scope keys (multi-material / CFS)

Most keys resolve against the edited print, filament or printer preset. A few
belong to the *project* instead, and only these seven are writable:

| Key | Shape | Constraint |
|---|---|---|
| `flush_volumes_matrix` | `,`-separated floats | exactly `filaments² × nozzles` values, each in `[0, 20000]` |
| `flush_multiplier` | `,`-separated floats | one per nozzle, each in `[0, 3]` |
| `filament_colour` | `;`-separated `#RRGGBB` | recolour only — the count is fixed by the filament presets |
| `curr_bed_type` | enum name | `"Default Plate"` is rejected |
| `prime_volume_mode` | enum name | — |
| `wipe_tower_x`, `wipe_tower_y` | float | applies to the current plate only |

```bash
curl -X PUT -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"flush_multiplier":"0.3","flush_volumes_matrix":"0,280,280,280,280,0,280,280,280,280,0,280,280,280,280,0"}' \
  http://localhost:13130/api/v1/config
```

The size and range checks are not decoration: these vectors are indexed by the
slicer with arithmetic bounded by a *different* vector, so a wrong-length write
is an out-of-bounds access rather than a validation error. Values are also vetted
as text before parsing, because the vector deserializers turn an unparsable token
into `0` and report success — which would silently mean "no purge at all".

The remaining project keys stay read-only (they appear in `GET /config` but not
in `PUT`) and report why: `values_are_unchecked_extruder_indices` (`filament_map`,
`filament_volume_map`, `filament_nozzle_map`), `tied_to_filament_map`
(`filament_map_mode`, `nozzle_volume_type`), `indexed_unchecked_by_the_colour_picker`
(`filament_colour_type`, `filament_multi_colour`), `derived_from_flush_volumes_matrix`
(`flush_volumes_vector`), `live_printer_state_not_a_setting`
(`has_filament_switcher`, `enable_filament_dynamic_map`).

### POST /slice

```bash
curl -X POST -H "X-Api-Token: $TOK" http://localhost:13130/api/v1/slice
```
- `202 {"started":true}` — slicing began (watch events or poll `/slice/status`).
- `200 {"started":false,"already_valid":true}` — plate already sliced.
- `409 {"error":"already_slicing"}` — a slice is in progress.
- `422 {"error":"nothing_to_slice"}` — no objects on the plate.

### GET /slice/status

```json
{
  "state": "done", "percent": 100, "message": "",
  "stats": { "estimated_time": "27m 23s", "estimated_time_seconds": 1643,
             "filament_used_mm": 1582, "filament_used_g": 4.72, "total_cost": 0.094 },
  "warnings": [ { "level": 3, "message": "bed temp too high…", "code": "1000C001" } ]
}
```
`state` is one of `idle | slicing | done | error`. `stats` appears once done.

### WS /events

```
ws://<host>:13130/api/v1/events?token=<token>
```
Each message is a JSON object with an `event` field:

| event | payload |
|---|---|
| `slice.started` | `state,percent,message` |
| `slice.progress` | `state:"slicing",percent,message` (stage text) |
| `slice.done` | `state:"done",percent:100,stats{…}` |
| `slice.error` | `state:"error",message` |
| `slice.cancelled` | `state:"idle",message:"cancelled"` |
| `slice.plate_changed` | the current plate's state after a plate route: `done` + `stats`, or `idle` + `message` |
| `config.changed` | `tabs:[print\|filament\|printer\|sla_print\|sla_material\|other]` |
| `project.opened` | `project:"<filename>"` |

> `slice.*` events carry `stats` (on done) but not the slice `warnings` array —
> call `GET /slice/status` for warnings.

## Plates

`slice`, `gcode`, `plate/render` and `jobs/status` all act on the **current** plate.
`arrange` is the toolbar's "Arrange all": it re-packs the objects of every unlocked
plate together and deletes plates left empty at the end, so it undoes a
one-part-per-plate layout (locked plates are left alone). The plate
routes let a client build a multi-plate project the way the toolbar does: every
mutation goes through the same `Plater` calls as a click, so undo, the sidebar plate
list, the background process and the preview follow along. `GET /status` reports
`plates: {count, current}` and lists `plates` in `capabilities`; `GET /objects` adds
a `plate` field per object (0-based, `-1` when it sits off every plate).

`GET /slice/status` follows the current plate. After a plate is selected, added
with `select`, deleted, or an object moves, it reports the current plate's own
result (`done` with its stats when that plate is still sliced, otherwise `idle`
with the message `plate N is not sliced`), and `POST /slice` on a plate that is
already sliced (`already_valid`) refreshes it the same way. Without that, the
status would keep describing whichever plate was sliced last.

### GET /plates

```bash
curl -H "X-Api-Token: $TOK" http://localhost:13130/api/v1/plates
```
```json
{
  "plates": [
    { "index": 0, "name": "", "current": true, "locked": false, "empty": false,
      "slice_result_valid": false,
      "origin": [0,0,0], "bbox_min": [0,0,0], "bbox_max": [256,256,250],
      "objects": [ { "id": 794, "instance": 0, "name": "box_base.stl" } ] },
    { "index": 1, "name": "", "current": false, "locked": false, "empty": true,
      "slice_result_valid": false,
      "origin": [281,0,0], "bbox_min": [281,0,0], "bbox_max": [537,256,250],
      "objects": [] }
  ],
  "count": 2, "current": 0, "max": 36, "can_add": true, "can_delete": true
}
```
Indices are 0-based and shift down when an earlier plate is deleted, exactly as in
the GUI (plate "2" on screen is index 1).

### POST /plates

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"select": true}' http://localhost:13130/api/v1/plates
```
```json
{ "created": true, "index": 1, "count": 2, "current": 1, "plate": { "...": "as in GET /plates" } }
```
The body is optional; `select` defaults to `true` (the new plate becomes current,
as with the toolbar button). Errors: `422 max_plates` (the limit is reported in
`max`), `409 slicing`.

On OrcaSlicer 2.5, a new plate that adds a column to the plate grid (the 2nd, 5th,
10th, ... plate) makes OrcaSlicer rebuild which objects sit on which plate, and every
plate's slice result is dropped, as with the toolbar button. `/slice/status` follows:
it turns `idle` and a `slice.plate_changed` event is sent. Slice again afterwards.

### POST /plates/{i}/select

```json
{ "selected": true, "current": 1, "plate": { "...": "as in GET /plates" } }
```
Errors: `404 unknown_plate`, `409 slicing`, `422 select_failed`.

### DELETE /plates/{i}

```json
{ "deleted": true, "index": 1, "count": 1, "current": 0, "objects_moved_off_plate": 2 }
```
The objects the plate held are **not** deleted: `Plater::delete_plate` parks them
beside the bed, as the toolbar does, and `objects_moved_off_plate` says how many.
Errors: `404 unknown_plate`, `422 last_plate` (the only plate cannot be deleted),
`409 slicing`.

### POST /objects/{id}/plate

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"plate": 1}' http://localhost:13130/api/v1/objects/832/plate
```
```json
{ "moved": true, "id": 832, "instance": 0, "plate": 1, "from_plate": 0, "offset": [409,128,6] }
```
`instance` (default 0) picks the copy when the object has several. An object coming
from another plate keeps its position relative to that plate (front-left of plate 1
lands front-left of plate 2); one coming from off-plate goes to the target's centre.
Already on that plate: `200 {"moved": false, "already_on_plate": true}`. The plate
it left is marked unsliced, as a drag in the window does. If keeping the relative
position would put it outside the target (it straddled the old plate's edge), it is
centred instead. `POST /objects/{id}/transform` and `/duplicate` also re-check which
plate the object (or the new copy) sits on, so an object moved onto another plate by
coordinates is sliced with that plate. A plate's deleted objects are parked where the
next plate appears, so `POST /plates` straight after a delete picks them back up. Errors:
`404 unknown_object | unknown_plate | unknown_instance`, `422 plate_locked` (the
target or the plate it sits on is locked; `plate` names which) `| move_failed`,
`409 slicing`, `400 missing_plate`.

## Project files

File > Save, Open and New each go through modal prompts in the window ("save the
changes first?", "keep the modified presets?", "overwrite?"). A remote caller cannot
see those, so every prompt is answered from the request and the GUI's own
remembered-choice settings are used to keep the dialogs closed for the duration of
the call. The work goes through the same `Plater` calls as the menu, so the window
title, dirty flag, undo stack, the periodic backup and File > Recent follow along.
`GET /status` reports `project` (path, empty until saved), `project_name` and
`project_dirty`, and lists `project` in `capabilities`.

The file routes (recent, browse, move, delete) work on `.3mf` files only, through
paths on the OrcaSlicer host, and never touch the open project's own file except to
rename it. Delete is permanent: there is no trash.

Two Preferences > Remote API boxes guard what these routes can do; both are off
by default and both are answered with `422 blocked_by_remote_api_policy`:

- **Allow deleting and replacing project files.** Without it `DELETE /project/file`
  is refused, and so is a save or move with `overwrite: true` onto a file that is
  already there. Saving to a free name, opening and renaming stay open.
  `GET /status` reports it as `project_file_changes_allowed`.
- **Allow script, G-code and connection edits**, the box `PUT /config` already
  answers to. Without it `POST /project/open` reads the project's settings before
  it changes anything and refuses a project that brings its own post-processing
  script, custom G-code, printer connection, printer model or file name format,
  meaning a value that differs from the local preset the project names, or one
  from a preset that isn't on this computer. The reply lists them in `keys`. With
  `presets: keep_current` only the filament slots the call can't put back are
  checked. OrcaSlicer's own File > Open asks about modified G-code; this check is
  what stands in for that prompt.

Known limits: a 3MF from another slicer, a newer OrcaSlicer, or one that needs a
migration can still raise an informational dialog inside the loader (projects saved
by this build open without one). And `Plater::load_project` empties the window
before it reads the file, so a file that turns out to be unreadable leaves an empty
project behind and answers `422 load_failed`; the preset edits dropped for the open
are gone by then too. Save first when that matters.

### GET /project

```json
{ "name": "float_spinner", "path": "/Users/roy/float_spinner.3mf", "saved": true,
  "dirty": true, "plates": 4, "objects": 9, "modified": ["print"], "recent_count": 6 }
```
`dirty` is what File > Save would have to write: plater edits, modified presets, or
undo-stack changes since the last save. `modified` names the preset groups with
unsaved edits.

### POST /project/save

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"path": "/Users/roy/float_spinner.3mf"}' http://localhost:13130/api/v1/project/save
```
```json
{ "saved": "/Users/roy/float_spinner.3mf", "bytes": 1834021, "project": { "...": "as in GET /project" } }
```
The body is optional. Without `path` (or with `null`) the project is saved to its
own path, as File > Save does; `422 no_project_path` when it has never been saved.
With a `path` it is Save As: an existing file at that path is refused with
`409 exists` unless `overwrite` is `true` (saving the project back onto its own
path is a plain Save and never counts), and replacing it also needs the file
changes box (`422 blocked_by_remote_api_policy`). The file holds what the GUI writes:
plates, placement, per-object and per-plate settings, height ranges, slot
presets and colours, and unsaved config edits. Afterwards `GET /status` reports
the new `project` path and `project_dirty: false`, and the file is at the top of
File > Recent. Errors: `400 not_3mf`, `404 directory_not_found`, `409 slicing`,
`422 save_failed`.

### POST /project/open

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"path": "/Users/roy/float_spinner.3mf", "if_unsaved": "discard"}' \
  http://localhost:13130/api/v1/project/open
```
```json
{ "opened": "/Users/roy/float_spinner.3mf", "plates": 4, "objects": 9,
  "had_unsaved_changes": true, "discarded_changes": [],
  "presets": { "printer": "...", "print": "...", "filaments": ["...", "..."] },
  "project": { "...": "as in GET /project" } }
```
File > Open Project, not a geometry import: plates, objects, per-object and
per-plate settings, slot presets and colours come back. Instead of `path`, `recent`
names an entry of `GET /project/recent` by index (File > Recent). `if_unsaved`
answers the "save changes to the current project?" prompt: `fail` (the default)
refuses with `422 unsaved_changes` and changes nothing, `save` writes the current
project to its own path first (`422 no_project_path` if it has none; the reply
carries `saved`), `discard` goes ahead. Unsaved preset edits are dropped before the
open, because the project's config replaces them in any case; `discarded_changes`
names the groups.

`presets` is `project` (the default: the project's printer, print and filament
presets are selected, as the GUI does) or `keep_current`: the project is opened and
the presets selected before the call are selected again (printer, then print, then
each filament slot, for names that still exist); `presets_restored` says which. The
project's own config edits are not kept in that mode. Errors: `404 file_not_found`
(with a `detail` when the file is a stale recent entry) `| unknown_recent`,
`409 slicing | busy`, `422 unsaved_changes | no_project_path | unsupported_format |
load_failed | blocked_by_remote_api_policy` (with `keys`),
`400 missing_path | bad_recent | bad_if_unsaved | bad_presets`.

### POST /project/new

```json
{ "ok": true, "had_unsaved_changes": false, "kept_changes": [], "project": { "name": "Untitled", "path": "", "...": "" } }
```
File > New Project with the same `if_unsaved` handling (body optional). The GUI's
second prompt, keep the modified presets for the new project or discard them, is
answered with Keep: unsaved preset edits carry over and `kept_changes` names the
groups. Use `PUT /preset` to drop them. An optional `name` gives the project its
title (the window shows it; the file name is chosen at save). Errors as for open.

### GET /project/recent

```json
{ "recent": [
    { "index": 0, "path": "/Users/roy/float_spinner.3mf", "name": "float_spinner", "exists": true,
      "open": true, "size_bytes": 1834021, "modified": "2026-11-04T10:12:03Z" },
    { "index": 1, "path": "/Users/roy/old.3mf", "name": "old", "exists": false, "open": false } ],
  "count": 2 }
```
File > Recent, most recent first. `open` marks the open project's own file;
`size_bytes` and `modified` (UTC) are present when the file exists. `index` is
what `POST /project/open {"recent": i}` and `DELETE /project/recent {"index": i}`
take; the list changes whenever a project is saved or opened, so read and use it
together.

### DELETE /project/recent

```bash
curl -X DELETE -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"path": "/Users/roy/old.3mf"}' http://localhost:13130/api/v1/project/recent
```
```json
{ "forgotten": ["/Users/roy/old.3mf"], "recent": [ "..." ], "count": 1 }
```
Takes an entry off File > Recent by `path` (as listed, or any spelling of the same
file while it exists) or `index`, or clears the list with `{"all": true}`. Nothing
on disk changes. Errors: `404 not_in_recent | unknown_recent`, `400 missing_target
| bad_index`.

### GET /project/browse

```bash
curl -H "X-Api-Token: $TOK" "http://localhost:13130/api/v1/project/browse?dir=/Users/roy/projects"
```
```json
{ "dir": "/Users/roy/projects", "count": 2, "truncated": false,
  "files": [ { "path": "/Users/roy/projects/a.3mf", "name": "a", "exists": true, "open": false,
               "size_bytes": 1834021, "modified": "2026-11-04T10:12:03Z" }, "..." ] }
```
The `.3mf` files in one folder, not recursive, newest first (at most 500; `truncated`
says when there were more). Without `dir` the open project's folder is listed, else
the folder the last file dialog used. Hidden files are skipped. Errors:
`404 directory_not_found`, `400 missing_dir`.

### POST /project/move

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"from": "/Users/roy/a.3mf", "to": "/Users/roy/projects/bracket.3mf"}' \
  http://localhost:13130/api/v1/project/move
```
```json
{ "moved": "/Users/roy/a.3mf", "to": "/Users/roy/projects/bracket.3mf", "bytes": 1834021,
  "was_open": true, "recent_updated": true, "project": { "...": "as in GET /project" } }
```
Renames or moves a project file (across volumes it is copied and the original
removed). When it is the open project's file, the window follows: project name,
title, project folder and File > Recent show the new path and the next Save writes
there; the content and the dirty state are untouched. Otherwise a recent-list entry
naming the old path is pointed at the new one. An existing file at `to` is refused
with `409 exists` unless `overwrite` is `true` and the file changes box is ticked,
and the open project's own file is never overwritten. A rename that only changes
the case of the name works on a case-insensitive disk. Errors:
`404 file_not_found | directory_not_found`, `409 exists | project_is_open`,
`400 not_3mf | same_path | missing_path`, `422 move_failed` (with the OS message) `|
blocked_by_remote_api_policy`.

### DELETE /project/file

```bash
curl -X DELETE -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"path": "/Users/roy/old.3mf"}' http://localhost:13130/api/v1/project/file
```
```json
{ "deleted": "/Users/roy/old.3mf", "bytes": 1834021, "recent_updated": true }
```
Removes a `.3mf` from disk, permanently, and from File > Recent. The open project's
own file is refused with `409 project_is_open`: start a new project or open another
first. Needs the file changes box. Errors: `404 file_not_found`,
`400 not_3mf | missing_path`, `422 delete_failed | blocked_by_remote_api_policy`.

## Notes & limits

- **Single-client oriented.** The server uses one I/O thread and marshals most
  calls onto the GUI thread with a 10s timeout (`504 ui_timeout`). Heavy or
  concurrent calls serialize; a stuck GUI stalls the API for up to 10s.
- **Token strength.** 32 hex chars (128 bits) from `std::random_device` — fine for
  a LAN token, not intended as a cryptographic secret store.
- **Survives GUI recreate.** Changing language/skin rebuilds the GUI; the server
  keeps running and slice-event subscriptions rebind onto the new window, so WS
  clients stay connected.
- **No SO_REUSEADDR pitfalls.** The listener sets `SO_REUSEADDR`, so restarting
  the app rebinds the port cleanly even if a prior socket is in `TIME_WAIT`.
