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
| GET | `/activity` | The MCP panel's timeline of this project: the API's changes, what was undone, restore points |
| GET | `/user_changes?since=` | What the user changed outside the API since a journal seq (hand edits, preset switches, objects, plates, projects, panel undos) |

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
On a single-filament printer `slot: 0` behaves like the request without `slot`, including
discarding unsaved edits.
Builds that support it list `preset_slot` in `GET /status` `capabilities`; older builds
ignore `slot` and switch the Filament tab preset, so check the capability first.

#### Project-scope keys (multi-material / CFS)

Most keys resolve against the edited print, filament or printer preset. A few
belong to the *project* instead, and only these seven are writable:

| Key | Shape | Constraint |
|---|---|---|
| `flush_volumes_matrix` | `,`-separated floats | exactly `filaments² × nozzles` values, each in `[0, 20000]` |
| `flush_multiplier` | `,`-separated floats | one per nozzle, each in `[0, 3]` |
| `filament_colour` | `;`-separated `#RRGGBB` | recolour only — the count is fixed by the filament presets; a recoloured slot becomes a solid colour, as in the colour picker |
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
| `panel.reverted` | `group,row`: the user undid that row in the MCP panel |
| `panel.restored` | `group`: the user restored the project to that point in the MCP panel (`group:null,undone:true` when they undid the restore) |

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

## MCP panel activity

The OrcaSlicer MCP panel (a dock pane) lists every change the API made, grouped into bursts, with an undo per
row and a restore point per group. Undos and restores there are the user's own actions: they are not refused by
the two Preferences boxes. The API side of it:

### GET /activity

```bash
curl -H "X-Api-Token: $TOK" http://localhost:13130/api/v1/activity
```
```json
{
  "rows": [
    {"id": 1, "group": 1, "ts": 1762680660, "time": "10:31", "kind": "setting",
     "label": "Wall loops", "value": "3", "before": "2", "where": "0.20mm Standard @Printer (unsaved)",
     "undo": {"verb": "Revert", "tip": "Set Wall loops back to 2"}, "undone_at": "",
     "title_hint": null, "result": null, "error": "", "on_disk": false},
    {"id": 2, "group": 1, "ts": 1762680702, "time": "10:31", "kind": "slice",
     "label": "Sliced", "value": "1h 48m · 41.2 g", "before": "", "where": "plate 1",
     "undo": null, "undone_at": "", "title_hint": null,
     "result": {"seconds": 6480, "grams": 41.2}, "error": "", "on_disk": false}
  ],
  "groups": [
    {"id": 0, "ts": 1762680660, "time": "10:31", "restore": {"available": true, "why": ""}},
    {"id": 1, "ts": 1762680660, "time": "10:31", "restore": null}
  ],
  "restored_to": null,
  "restore_failed": false,
  "can_undo_restore": false
}
```
The same shape the panel shows. `kind` is `setting`, `preset`, `model`, `plate`, `slice`, `file` or `refused`.
`before` is the previous value in the same form as `value`; `undone_at` is the time (HH:MM) the user undid the
row in the panel (or the time of a restore that undid its group), else `""`. `error` is `""` or the text of a
refusal or failure. `on_disk` marks changes written to disk (saved or moved projects, saved or deleted presets),
which a restore does not undo. Group `0` is the point before the assistant's first change; a group's `restore`
is `null` for the newest group, `{"available": true}` when its end state can be restored, or `available: false`
with the reason. `restored_to` is the group the user last restored to (until the next API change),
`restore_failed` is `true` when that restore failed after it had started replacing the project (the panel then
says "Couldn't restore to"), and `can_undo_restore` whether the panel still offers to undo that restore. Reading
it changes nothing.

While the user restores the project in the panel, every API request gets
`409 {"error": "busy", "detail": "the user is restoring"}`; retry after a moment.

The timeline is kept per project file in `<data dir>/mcp_history/`, with up to 20 restore points.

### GET /user_changes?since=

What changed in OrcaSlicer outside the API: settings edited by hand, presets switched, objects added, removed,
moved, rotated or scaled, plates added or removed, a project opened or a new one started, and the panel's own
undos and restores. Every API request compares the project with the state the previous request left (settings
of the print, printer and every filament slot's preset, the objects and their copies' placement, the plate
count), so a difference is something the user did in between; at the end of the request the project is taken as
the new reference, so the API's own changes never show up here. An arrange or orient started by the API is the
API's too, even though it finishes after its request.

```bash
curl -H "X-Api-Token: $TOK" "http://localhost:13130/api/v1/user_changes?since=12"
```
```json
{
  "seq": 16,
  "changes": [
    {"seq": 13, "time": "10:42", "kind": "preset", "group": "Print", "text": "Print preset switched to 0.30mm Draft"},
    {"seq": 14, "time": "10:42", "kind": "setting", "group": "Print", "text": "Wall loops 2 → 4", "key": "wall_loops"},
    {"seq": 15, "time": "10:43", "kind": "object", "group": "Model", "text": "Moved bracket.stl"},
    {"seq": 16, "time": "10:44", "kind": "panel", "group": "Panel", "text": "Reverted in the panel: Sparse infill density back to 15%"}
  ]
}
```
`seq` is the newest entry's number (keep it and pass it as `since` next time; `since` defaults to 0, a value that
is not an integer is `400 bad_since`). Seqs start at the clock in milliseconds when OrcaSlicer starts, so they keep
growing across restarts: a `since` from before a restart still returns every entry of the new run. When `since` is
greater than `seq` (a seq this run never handed out, as after the computer's clock was set back), the answer holds
every entry and adds `"reset": true`; take the new `seq` and carry on from it. `changes` are the entries newer
than `since`, oldest first. `kind` is
`setting`, `preset`, `object`, `plate`, `project` or `panel`; `group` is `Print`, `Filament`, `Printer`,
`Model`, `Plates`, `Project` or `Panel`; `key` is the config key of a `setting` entry. The text uses the
timeline's words: the setting's label, the old and new value with their unit, and ` (filament 2)` /
` (filaments 1, 3)` for a filament setting when more than one filament is loaded. Script, G-code and
connection settings (the ones behind "Allow script, G-code and connection edits", passwords and API keys
among them) say only `"<label> changed"`, never a value.

The answer is kept small:
- Net changes only. A setting changed several times appears once, from its first old to its last new value, and
  disappears when it was changed back. When the API set it in between, the entry starts from the API's value.
  An object keeps its strongest change (rotated over scaled over moved); added and then removed is nothing.
- A preset switch is one entry, `"<Print|Printer|Filament> preset switched to <name>"`, followed only by the
  values edited on top of the new preset; earlier setting entries of that preset are dropped (for a filament,
  only for that slot).
- Opening a project (`"Opened project <name>"`) or starting a new one (`"New project"`) is one entry and drops
  every entry before it.
- Camera and view, plate selection and the user's own slices are not reported.

Reading it changes nothing, but like every request it first compares the project, so an edit made just before
appears in the same answer. The journal keeps the latest 200 entries in memory (a restart starts it empty), and
the first request after a start only takes the reference. `GET /slice/status` neither compares nor takes a
reference, so polling it never loses an edit. The comparison and the new reference run on OrcaSlicer's GUI thread
around the request's own work, without the request waiting for them: a busy GUI delays a request by its own
timeout only.

### Test hooks

Only when OrcaSlicer was started with the environment variable `ORCA_MCP_PANEL_TEST=1` (otherwise `404`):
`POST /activity/test/undo {"row"}`, `/restore {"group", "async"?}`, `/undo_restore {}` run the panel's actions
and answer what the panel gets, `{"status": "ok" | "cancelled" | "failed", "reason"?}` (`async: true` starts
the restore as the panel does, outside any request, answers `202 {"status":"started"}`, and
`/result {}` returns its result later); `/clock {"advance_s"}` moves the panel's clock forward;
`/checkpoint_delay {"seconds"}` slows every restore-point save; `/as_user {"changes": {key: value}}` edits the
print, filament or printer settings as the user would by hand (no row, no policy check; `/user_changes` reports it). `/corrupt_point {"group", "from"?}`
spoils a restore point (overwrites its file with `from`, or with bytes that are no 3MF) to test a failed restore.
`/pane {"clear"?: true, "post"?: message | [messages]}` drives the dock pane: `clear` starts recording what the
pane sends to its page, `post` hands the pane page messages as if the page had sent them; the answer is
`{"open": bool, "messages": [...], "posted"?: bool}`.

## MCP panel page and protocol

The panel is one HTML page, `resources/web/orcaslicer_mcp/panel.html`, shared by every host that shows it: this
fork's dock pane (C++, `src/slic3r/GUI/RemoteAPI/McpPanel.cpp`) and the OrcaSlicer MCP plugin for stock
OrcaSlicer 2.5 (Python). The page owns what the user sees: grouping, titles, which undo to offer and why one is
unavailable, the live-value comparison, expand and collapse, hover tags and the restore confirm. A host only
records rows, answers live values, runs undos and restores, and passes messages.

### Hosting the page

- **Vendor it unchanged.** Copy `panel.html` byte for byte and keep a test that fails when your copy differs from
  the fork's file. It loads nothing from outside (no network, no other files), so it works from any base URL.
- **Page to host:** the page calls `window.orca.postMessage(message)` with a plain object. OrcaSlicer 2.5's web
  panels (the `DockPanel` and plugin dock panels) provide `window.orca`; another host defines it before the page's
  scripts run. When `window.orca` is missing the page sends nothing.
- **Host to page:** either call `window.__mcpPanelReceive(message)`, or deliver through `window.orca.onMessage`:
  the page registers the same function there when `window.orca.onMessage` exists (on a 2.5 web panel that is
  what `DockPanel::push_message` reaches). Each message is one plain object with a `type`.
- **Theme:** the page reads `--orca-bg`, `--orca-fg`, `--orca-muted`, `--orca-border`, `--orca-accent` and
  `--orca-font` from its document and falls back to a dark theme without them. 2.5's web panels set them.
- **Width:** laid out for a pane about 300 px wide.

### Protocol version

`window.McpPanelCore.PROTOCOL_VERSION` in the page (now `1`) and the `version` a host puts in every `history`
message. Bump it in the page on any breaking change: a message or field renamed or removed, a field whose meaning
or form changes, or a new field a host must send. A new optional field with a default (as `off_text` and
`restore_failed` are) is not breaking. A host compares the page's `PROTOCOL_VERSION` with the version it
implements (in its drift test, for instance) before it ships a newer copy. The page does not read `version`
today.

### Page to host

| Message | When | The host answers |
|---|---|---|
| `{"action": "ready"}` | When the page has loaded, then again every 500 ms until the first `history` arrives (at most 20 more times) | `state`, then `history`. A repeat is normal (the first may have been lost while the bridge was starting), so answer each one, and coalesce answers that are already pending |
| `{"action": "live"}` | At load and after every `history` it receives | `live` |
| `{"action": "undo", "row": <id>}` | The user clicked a row's undo link | exactly one `result` |
| `{"action": "restore", "group": <id>}` | The user confirmed "Restore to HH:MM?" (`0` is "Before the assistant started") | exactly one `result` |
| `{"action": "undo_restore"}` | The user clicked Undo on the "Restored to" line | exactly one `result` |
| `{"action": "open_settings"}` | The settings icon, or the link in the off line (both only when `capabilities.open_settings` is not `false`) | nothing; open the host's settings (Preferences > Remote API here, the plugin's own settings page in the plugin) |

Ids arrive as JavaScript numbers. Every `undo`, `restore` and `undo_restore` must get a `result`, also when it is
refused or fails: until then the page shows "Reverting…" or "Restoring…". A host that is busy, closing or
rebuilding its window answers `failed` with the reason instead of running the action. Run the action outside the
web view's own message callback (a restore opens a project and pumps events). A host may get `open_settings` twice
in a row (a double click): open one settings window, not one inside another.

### Host to page

**`history`**: the whole timeline, after the page's `ready` and after every change to it.
```json
{"type": "history", "version": 1,
 "rows": [ /* rows, below */ ], "groups": [ /* groups, below */ ],
 "restored_to": null, "restore_failed": false, "can_undo_restore": false,
 "capabilities": {"restore": true, "kinds": ["setting", "preset", "model", "plate", "slice", "file", "refused"],
                  "open_settings": true, "settings_hint": "", "off_text": "The Remote API is off."}}
```
- `rows`, `groups`, `restored_to`, `restore_failed` and `can_undo_restore` are the shape of `GET /activity`
  above (this fork sends exactly that object plus `type`, `version` and `capabilities`).
- `restored_to`: `null`, or the group id of the last restore point the user restored to. The page then shows
  "Restored to HH:MM" at the top (HH:MM is that group's `time`), with " · Undo" when `can_undo_restore` is true,
  and greys out the groups after that point.
- `restore_failed` (optional, default `false`; read only with `restored_to`): the last restore failed after it
  had started replacing the project. The line reads "Couldn't restore to HH:MM" instead, and no group is greyed
  out (nothing was marked undone).
- `capabilities`:
  - `restore` (bool): show restore icons and the confirm. `false` for a host without checkpoints.
  - `kinds` (list): the row kinds this host records. Informational; the page does not read it.
  - `open_settings` (bool, default `true`): the settings icon and the off line's link send `open_settings`.
    With `false` the icon's hover tag and a line under the off text show `settings_hint` instead.
  - `settings_hint` (string): where the settings are, for `open_settings: false`.
  - `off_text` (string, optional): the line shown while `state.api` is `"off"`. Default "The Remote API is
    off."; the plugin sends "Assistant access is off.", this fork sends "The Remote API couldn't start. Check its
    port." when it is enabled but not listening, and nothing otherwise.

**`live`**: the current values of the rows, after the page asks and when settings, objects or the project change.
```json
{"type": "live", "values": {"12": {"value": "20%", "present": true}, "14": {"value": "bracket.stl", "present": false}}}
```
Keys are row ids as strings. `value` is in the row's own display form (`value` of the row, unit included);
`present: false` means the thing the row changed is gone (an object deleted since). Send an entry for every row
that has an undo and is not undone, and for every row of the newest group whose value can be read now; leave out
rows you cannot read. The page compares numbers with the same unit text within 1e-6 (relative) and anything else
exactly, so send the same form the row was recorded in. For geometry (moves, rotations, arranges) the host
compares and sends the row's own `value` when nothing moved, else another text such as `"moved since"`.
In the newest group a different live value is shown with "changed by you", a missing one with "removed since";
in any group an undo is then unavailable ("You changed it since", "Changed again since; restore an earlier point
instead", "<name> is no longer on the plate").

**`state`**: `{"type": "state", "api": "on" | "off"}`, after `ready` and whenever it changes. `on` means the
assistant can reach OrcaSlicer now (this fork: the Remote API server is listening, not only enabled in
Preferences). `off` shows `off_text` above the timeline.

**`result`**: the answer to one action.
```json
{"type": "result", "action": "undo", "row": 12, "status": "failed", "reason": "You changed it since"}
```
`action` is the page's action; `row` (for `undo`) or `group` (for `restore`) echoes the id it sent, and
`undo_restore` carries neither. `status` is `ok`, `cancelled` (the user said no to a prompt on the way) or
`failed`; `reason` (short text, sentence case) comes with every status but `ok`. A failed undo shows the reason
on its row, a failed restore on its group (or on the "Restored to" line for `undo_restore`). Send the new
`history` before the `result` when the action changed the timeline, so the page never shows the old one
without its busy mark.

### Rows

```json
{"id": 12, "group": 3, "ts": 1762680660, "time": "10:31", "kind": "setting",
 "label": "Sparse infill density", "value": "15%", "before": "25%", "where": "0.20mm Standard @Printer (unsaved)",
 "undo": {"verb": "Revert", "tip": "Set Sparse infill density back to 25%"}, "undone_at": "",
 "title_hint": null, "result": null, "error": "", "on_disk": false}
```
- `id` (int): unique within the project's timeline. `group` (int): the group it belongs to.
- `ts` (seconds) and `time` (`"HH:MM"`, local): the host formats the time; the page never does.
- `kind`: `setting`, `preset`, `model`, `plate`, `slice`, `file` or `refused`. It picks the icon and its
  colour; `refused` (and a `slice` row with an `error`) is shown in red.
- `label`: OrcaSlicer's own label (a setting's full label: "Nozzle temperature", not "Other layers").
- `value`: the new value as shown (unit added, enum label). A `setting` row whose value is a colour (`#RGB`,
  `#RRGGBB` or `#RRGGBBAA`, as filament colour rows carry it, label "Filament 2 colour") gets a small colour
  swatch beside it.
- `before`: the previous value in the same form as `value`, or `""`. The page writes "Was <before>"; send no
  prefix. `where`: any other detail (the preset, the plate, why it was refused), shown after it.
- `undo`: `null`, or `{"verb": "Revert", "tip"?: "<hover text>"}`. Without `tip` the hover tag is
  "<verb>: <label> back to <before>" (or "<verb> <value>" with no `before`).
- `undone_at`: `""`, or the `"HH:MM"` the user undid it (also set on the rows a restore undid).
- `title_hint`: `null`, `"added"` or `"opened"`: such a row names its group ("Added bracket.stl and 2 more").
- `result`: `null`, or `{"seconds": n, "grams": n}` on a slice row; the group shows the time of its last slice.
- `error`: `""` or the text of a refusal or failure (any non-empty text marks the group with a warning).
- `on_disk` (bool): the change wrote to disk (a saved or moved project, a saved or deleted preset); the restore
  confirm then adds "Files and presets saved to disk stay."

### Groups

```json
{"id": 3, "ts": 1762680660, "time": "10:31", "restore": {"available": true, "why": ""}}
```
One per burst of changes; `0` is the point before the assistant's first change ("Before the assistant started",
always last). The page orders them newest first by `ts`, then `id`, and opens the newest. `restore` is `null` for
the newest group (and for any group a host cannot restore to), `{"available": true}` for a restore point, or
`{"available": false, "why": "<hover text>"}`. A row whose group is not listed still gets a group, from its own
`ts` and `time`.

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
