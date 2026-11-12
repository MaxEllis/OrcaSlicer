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
| **Token** | 32 hex chars, auto-generated on first run. **Copy** puts the whole token on the clipboard. **Regenerate** invalidates existing clients. |

A status line under **Enable** says what the server is doing while Preferences is open: "On: listening on this
computer, port 13130." (or "on your local network"), "Off.", "Starts when you close Preferences." for a tick not yet
applied, and "Couldn't start: port N may be in use by another program. ..." when it was enabled but could not bind
its port. Changes to the port or LAN setting while it runs say they apply when Preferences closes.

> Binding to the LAN exposes live control of your slicer to every machine on the
> network. Keep the token secret and prefer localhost unless you need remote access.

## Authentication

- **HTTP:** send the header `X-Api-Token: <token>` on every request. Missing/wrong
  token → `401 {"error":"unauthorized"}`.
- **WebSocket:** pass `?token=<token>` in the URL (browsers can't set WS headers).
- **Trial requests:** a request with the header `X-Orca-Mcp-Trial: 1` is a trial run (orcaslicer-mcp's
  `compare_slices` sends it on its trial writes, slices and the final restore). It works as any request, but the
  MCP panel records nothing for it: no rows, no refusals (also not a slice that will not start), no restore points.
  The header counts only on `PUT /config`, `PUT /objects/{id}/config`, `POST /slice` and `POST /slice/cancel`;
  every other route records as usual with it. A client that sends it must put the settings back itself, and should
  report (and note) a restore that fails, since the panel then shows nothing of what was left. The user-change journal treats it as
  an API change, so its changes are never reported as the user's. Add one row for the whole trial afterwards with
  `POST /activity/note`.

## Endpoints

Base URL: `http://<host>:13130/api/v1`

| Method | Path | Purpose |
|---|---|---|
| GET | `/status` | App/project/preset status + slice validity |
| GET | `/config[?keys=a,b]` | Read merged config (secrets redacted) |
| PUT | `/config[?slot=n]` | Apply config edits live (atomic); `slot` writes one filament slot's preset |
| GET | `/filaments` | The project's filament slots: preset, type, vendor, colour, unsaved edits |
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
| POST | `/preset/save` | Save the current settings as a user preset (`{"type","name"}`), no dialog; `slot` saves one filament slot's preset and moves only that slot |
| POST | `/preset/config` | The full config of a named preset (`{"type","name"}`; credentials redacted) |
| DELETE | `/preset` | Delete a user preset (never a system, selected or inherited-from one) |
| GET | `/printer/slots` | The printer's filament trays: an Elegoo Centauri Carbon 2's CANVAS (read now), or the selected Bambu Lab printer's AMS and external spool |
| GET | `/printer/slot_mapping` | Which tray each filament of the plate will print from (on Bambu: the Send dialog's automatic choice) |
| PUT | `/printer/slot_mapping` | CANVAS only: set that mapping (`{"mapping":[{"filament","canvas_id","tray_id"}]}`); the Send dialog opens pre-filled |
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
| POST | `/activity/note` | One row the connector adds to the MCP panel (`{"kind": "slice" \| "refused" \| "note", "text", "value"?, "detail"?}`) |
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
{ "applied": [], "errors": { "wall_loops": "invalid value", "nope": "unknown_key" } }
```
Other errors: `400 body_must_be_object`, `400 invalid_json`, `504 ui_timeout`.

**Ranges.** Every numeric value is checked against its setting's own range before anything is applied, with the
rule OrcaSlicer's settings fields use (`ConfigOptionDef` min and max, `is_value_valid`: with a minimum of 0 any
negative value is out, a value within 0.0001 of a bound is in). Percentages are checked as numbers ("250%" against
0 to 100 for `sparse_infill_density`), and a list setting element by element. A layer height (`layer_height`,
`initial_layer_print_height`, also as an object's own setting) must be at least the printer's lowest
(Printer Settings > Layer height limits > Min, the lowest of its extruders), and never below 0.01 mm when that is 0
or unset; one request that lowers `min_layer_height` and the layer height together is judged against its own new
limit: a layer height of
0.0001 mm passes OrcaSlicer's own checks but needs hundreds of thousands of layers, and the reslice runs out of
memory (the sidebar asks in a dialog, which the API cannot show). The reply's `detail` names those limits. Where
OrcaSlicer's slicing check is stricter than the option's minimum, that rule applies too: `bridge_flow`,
`internal_bridge_flow`, `filament_flow_ratio`, `extruder_clearance_radius`, `extruder_clearance_height_to_rod`,
`extruder_clearance_height_to_lid` and `nozzle_height` must be above 0 (`"min_exclusive": true` in the reply),
`filament_diameter` at least 1, `nozzle_diameter` at least 0.005. A
"mm or %" setting given in mm is not held to the field's "did you mean a percentage?" limit (`seam_slope_start_height:
2` is fine). Only what changes is checked, so writing a value back never fails, even one outside its range (a value
a profile already carries). One value out of range refuses the whole request:
```json
{ "applied": [], "error": "out_of_range", "key": "sparse_infill_density", "value": "250%", "min": 0, "max": 100,
  "detail": "Sparse infill density must be between 0% and 100% (got 250%)",
  "errors": { "sparse_infill_density": "out_of_range: Sparse infill density must be between 0% and 100% (got 250%)" } }
```
`min` or `max` is `null` when that side has no limit (`layer_height` has a minimum of 0 and no maximum); `index`
names the element of a list setting. With several keys out of range, the top level names the first and `errors`
has each. `PUT /objects/{id}/config` answers the same way (`422`), and the MCP panel's undo and restore never write
a value out of range. `PUT /objects/{id}/height_range` keeps its printer limits (`422 layer_height_out_of_range` with `key`, `value`,
`min`, `max`), with the same 0.01 mm floor, so a range's layer height of 0 is refused too.

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

#### Writing one slot's preset

`PUT /config?slot=1` (0-based) writes the filament preset of that slot instead of
whichever preset is open in the Filament tab. It binds the Filament tab to the
slot first, exactly as the slot's edit button in the sidebar does: the tab opens
the slot's preset, and from then on follows it. Nothing else changes, and no
unsaved print or printer edit is dropped.

```bash
curl -X PUT -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"hot_plate_temp":100}' "http://localhost:13130/api/v1/config?slot=1"
```
```json
{ "applied": ["hot_plate_temp"], "errors": {}, "slot": 1, "preset": "Generic ASA" }
```

A preset shared by several slots is still one preset: writing slot 1 changes
every slot loaded with the same preset (the MCP panel row names them, "filament
1, 3"). To change one of them alone, save that slot's preset under its own name
first (`POST /preset/save` with `slot`, below).

Only one filament preset can hold unsaved edits at a time (the one in the tab).
When the tab holds unsaved edits to a *different* preset than the slot's, the
request is refused with nothing changed, because switching the tab would have to
drop them (OrcaSlicer asks about that in a dialog nothing can answer remotely):

```json
{ "applied": [], "error": "filament_edits_unsaved", "preset": "Generic PLA", "modified": ["hot_plate_temp"] }
```
Save them (`/preset/save`) or put the keys back first. `409` for that, `422
slot_out_of_range` (with `filament_count`), `400 invalid_slot` for a value that is
not a 0-based integer. A slot whose preset is the one already open needs no
switch, so unsaved edits to that preset are kept and built on.

#### Saving one slot's preset

OrcaSlicer's own Save As moves *every* slot that used the old preset to the new
name (the sidebar's `update_presets_from_to`). `POST /preset/save` with a 0-based
`slot` saves from that slot's preset and moves only that slot; the others keep
their preset:

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"type":"filament","name":"Polymaker LW-ASA","slot":1}' \
  http://localhost:13130/api/v1/preset/save
```
```json
{ "saved": "Polymaker LW-ASA", "created": true, "slot": 1, "slots_reassigned": [1], "slots_kept": [0, 2],
  "filaments": ["Generic ASA", "Polymaker LW-ASA", "Generic ASA"] }
```
`slots_kept` lists the slots OrcaSlicer had moved and the API put back. The same
binding and refusals as `PUT /config?slot=` apply (`409 filament_edits_unsaved`
when the tab holds unsaved edits to another preset, `422 slot_out_of_range`);
`slot` with a type other than `filament` is `400 invalid_slot`. Without `slot` the
route behaves as before.

### GET /filaments

The project's filament slots, as the sidebar shows them, in one read:

```json
{ "count": 3, "multi_filament": true,
  "filaments": [
    { "index": 0, "preset": "Generic ASA", "filament_type": "ASA", "vendor": "Generic", "colour": "#FFFFFF",
      "system": true, "open_in_tab": false, "modified": false, "modified_keys": [] },
    { "index": 1, "preset": "Polymaker LW-ASA", "filament_type": "ASA", "vendor": "Polymaker", "colour": "#3C3C3C",
      "system": false, "open_in_tab": true, "modified": true, "modified_keys": ["hot_plate_temp"] },
    { "index": 2, "preset": "Generic ASA", "filament_type": "ASA", "vendor": "Generic", "colour": "#FF0000",
      "system": true, "open_in_tab": false, "modified": false, "modified_keys": [] } ],
  "tab": { "preset": "Polymaker LW-ASA", "slot": 1, "modified": true } }
```
`preset` is the slot's preset; `filament_type`, `vendor` and `system` come from it,
`colour` from the project. Only the preset open in the Filament tab can hold
unsaved edits, so `modified` is true for every slot using that preset while it is
dirty, with the keys as `GET /status` lists them. `tab.slot` is the slot the tab is
bound to (null when it follows no slot). Builds with this route list
`filament_slots` in `GET /status` `capabilities`; the `slot` forms of `PUT /config`
and `POST /preset/save` come with it.

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
- `422 {"error":"slice_not_started","detail","key"?}` — OrcaSlicer would not start it. `detail` is OrcaSlicer's own
  reason (`Print::validate()`, the text its error notification shows: a layer height above what the nozzle allows,
  say), or "plate cannot be sliced (object outside bed or invalid state)" when it gives none; `key` is the setting
  it names, when it names one. `/slice/status` then says `error` with the same message.

### GET /slice/status

```json
{
  "state": "done", "percent": 100, "message": "",
  "stats": { "estimated_time": "27m 23s", "estimated_time_seconds": 1643,
             "filament_used_mm": 1582, "filament_used_g": 4.72, "total_cost": 0.094 },
  "warnings": [ { "level": 3, "message": "bed temp too high…", "code": "1000C001" } ]
}
```
`state` is one of `idle | slicing | done | error`. `stats` appears once done. While `done`, `valid` says whether
that result still matches the plate: after a later change (a setting, an object, the user's own edit) it is
`false`, with `"stale": true` and the message "out of date: something changed since this slice; slice again for
current numbers"; `stats` are then the old slice's. The route asks OrcaSlicer's GUI thread for this but never waits
long: without an answer within 300 ms (a dialog open, say), and while a project loads or the panel restores one,
the two fields are left out.

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

## Printer trays

`GET /printer/slots` and `GET /printer/slot_mapping` answer for two kinds of printer, told apart by `kind`:
`"canvas"` (an Elegoo Centauri Carbon 2 preset: the CANVAS unit is read over the network, up to about 18 s)
and `"bambu"` (a Bambu Lab preset: the printer selected in the Device tab, read from the state OrcaSlicer's own
connection keeps, so the answer is instant and nothing is sent to the printer). Any other preset gets
`{"supported": false, "kind": "bambu", "reason": "not_supported"}`; a Bambu preset with no printer selected
`no_printer_selected`, with the selected printer not connected `printer_offline`. Builds that read Bambu printers
list `bambu_ams` in `GET /status` `capabilities`.

### GET /printer/slots on a Bambu Lab printer

```json
{ "supported": true, "kind": "bambu", "connected": true,
  "printer": { "name": "P1S", "model": "Bambu Lab P1S", "connection": "lan" },
  "active": { "ams_id": "0", "tray_id": "1" },
  "ams": [ { "ams_id": "0", "type": "AMS", "extruder": 0, "humidity_level": 2, "humidity_percent": 31,
             "temperature": 24.5, "trays": ["A1", "A2", "A3", "A4"] } ],
  "trays": [
    { "ams_id": "0", "tray_id": "0", "index": 0, "label": "A1", "status": "loaded", "ready": true, "active": false,
      "unknown_filament": false, "filament_type": "ASA", "filament_name": "Bambu ASA", "brand": "Bambu Lab",
      "filament_id": "GFB00", "bambu_filament": true, "filament_color": "#FFFFFF", "remain_percent": 62,
      "min_nozzle_temp": 240, "max_nozzle_temp": 280 },
    { "ams_id": "0", "tray_id": "1", "index": 1, "label": "A2", "status": "loaded", "ready": true, "active": true,
      "unknown_filament": false, "filament_type": "ASA-Aero", "filament_name": "Polymaker PolyLite LW-ASA",
      "brand": "Polymaker", "filament_id": "P4a51d2", "bambu_filament": false, "filament_color": "#3C3C3C",
      "remain_percent": -1, "min_nozzle_temp": 230, "max_nozzle_temp": 270 },
    { "ams_id": "0", "tray_id": "2", "index": 2, "label": "A3", "status": "empty", "ready": false, "active": false,
      "unknown_filament": false, "filament_type": "", "filament_name": "", "brand": "", "filament_id": "",
      "bambu_filament": false, "filament_color": "", "remain_percent": null, "min_nozzle_temp": 0, "max_nozzle_temp": 0 },
    { "ams_id": "255", "tray_id": "0", "index": 255, "label": "Ext", "status": "empty", "ready": false, "active": false,
      "unknown_filament": false, "filament_type": "", "filament_name": "", "brand": "", "filament_id": "",
      "bambu_filament": false, "filament_color": "", "remain_percent": null, "min_nozzle_temp": 0, "max_nozzle_temp": 0 } ] }
```
`trays` lists every slot of every AMS unit in order and then the external spool(s) (`ams_id` 255, label `Ext`; a
second extruder's is 254, `Ext-2`), flat as the CANVAS reply lists them. `label` is the name OrcaSlicer's AMS panel
shows. `status` is `loaded`, `unknown` (a spool is in, but the printer reports no type or colour for it: set it in the
AMS panel) or `empty`; `ready` is true only for `loaded`. `filament_type` is the AMS's own type word (`ASA-Aero`,
`PLA-CF`, `PETG-HF`, ...), `filament_name` the name the AMS panel shows (the printer's, else the preset the tray's
`filament_id` points at), `active` marks the tray feeding the nozzle. `remain_percent` is the AMS's estimate for
Bambu spools, -1 when it has none, null for an empty slot. A `humidity_level` of 1 (dry) to 5 is reported by units
that measure it, null by the others.

### GET /printer/slot_mapping on a Bambu Lab printer

```json
{ "supported": true, "kind": "bambu", "plate": 0,
  "printer": { "name": "P1S", "model": "Bambu Lab P1S", "connection": "lan" },
  "filaments": [
    { "index": 0, "type": "ASA", "name": "Generic ASA", "vendor": "Generic", "color": "#FFFFFF",
      "tray": { "ams_id": "0", "tray_id": "0", "index": 0, "label": "A1", "filament_type": "ASA",
                "filament_name": "Bambu ASA", "brand": "Bambu Lab", "filament_color": "#FFFFFF" },
      "source": "matched", "warnings": [] },
    { "index": 1, "type": "ASA", "name": "Polymaker LW-ASA", "vendor": "Polymaker", "color": "#3C3C3C",
      "tray": null, "source": "none", "warnings": ["No tray holds ASA."] } ],
  "valid": false,
  "problems": ["Filament 2 (Polymaker LW-ASA) has no tray: the Send dialog needs a choice."] }
```
The filaments are the ones the next Send would map (the current plate's; every project filament when the plate
reports none), each with the tray the Send dialog's automatic mapping pre-fills. That mapping takes a tray only when
its `filament_type` equals the slot preset's type (then the nearest colour), so a slot whose type no tray holds
comes back with `tray` null, a warning, a problem and `valid` false: the Send dialog then waits for the user to
pick one. There is no `PUT` for Bambu printers: the choice is made in the Send dialog, which keeps no store this
API could fill.

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
`422 save_failed`, `403 protected_path` (see [MCP panel activity](#mcp-panel-activity)).

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
`400 missing_path | bad_recent | bad_if_unsaved | bad_presets`, `403 protected_path`.

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
blocked_by_remote_api_policy`, `403 protected_path` (for `from` or `to`).

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
`400 not_3mf | missing_path`, `422 delete_failed | blocked_by_remote_api_policy`, `403 protected_path`.

## MCP panel activity

The OrcaSlicer MCP panel (a dock pane) lists every change the API made, grouped into bursts, with an undo per
row and a restore point per group. Undos and restores there are the user's own actions: they are not refused by
the two Preferences boxes. When the user closes the pane with its close button, a banner at the bottom right says
"OrcaSlicer MCP panel closed. Get it back from View > OrcaSlicer MCP panel." with an Undo link that opens it again
(not when OrcaSlicer closes or rebuilds its window, nor while the Remote API is off). The API side of it:

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
     "title_hint": null, "result": null, "error": "", "on_disk": false, "reveal": true},
    {"id": 2, "group": 1, "ts": 1762680702, "time": "10:31", "kind": "slice",
     "label": "Sliced", "value": "1h 48m · 41.2 g", "before": "", "where": "plate 1",
     "undo": null, "undone_at": "", "title_hint": null,
     "result": {"seconds": 6480, "grams": 41.2}, "error": "", "on_disk": false, "reveal": false}
  ],
  "groups": [
    {"id": 0, "ts": 1762680660, "time": "10:31", "restore": {"available": true, "why": ""}},
    {"id": 1, "ts": 1762680660, "time": "10:31", "restore": null}
  ],
  "restored_to": null,
  "restore_failed": false,
  "can_undo_restore": false,
  "slice_valid": true
}
```
The same shape the panel shows. `kind` is `setting`, `preset`, `model`, `plate`, `slice`, `file` or `refused`.
`before` is the previous value in the same form as `value`; `undone_at` is the time (HH:MM) the user undid the
row in the panel (or the time of a restore that undid its group), else `""`. `error` is `""` or the text of a
refusal or failure. `on_disk` marks changes written to disk (saved or moved projects, saved or deleted presets),
which a restore does not undo. `superseded` is `true` on a row the assistant changed again later (a later row, not
undone, changed the same setting, object copy, height range or preset slot): its undo is unavailable ("Changed again
since; restore an earlier point instead"). `dropped` is `true` on a setting row whose unsaved edit the assistant's own
preset switch dropped (`PUT /preset` discards every preset with unsaved edits, `discarded_changes`), whatever was
selected afterwards; such a row is also `superseded`, shows no live value, and its undo is unavailable ("Dropped by
a preset switch; restore an earlier point instead"). `reveal` is `true` on a setting row of the print, filament or printer preset: the
panel can show that setting in the settings sidebar (not per-object settings, project keys such as filament
colours, or other kinds). Group `0` is the point before the assistant's first change (the panel does not list it
as a group of its own; it stays here, and a "Restored to" line for it names its time); a group's `restore`
is `{"available": true}` when its end state can be restored, `available: false` with the reason, or `null` when
it has no restore point yet (the page offers no restore on the newest group, whatever it says).
`restored_to` is the group the user last restored to (until the next API change or refusal),
`restore_failed` is `true` when that restore failed after it had started replacing the project (the panel then
says "Couldn't restore to"), and `can_undo_restore` whether the panel still offers to undo that restore.
`slice_valid` (left out when the timeline has no slice result, or its plate is gone) says whether the newest slice
result still describes its plate (`false` after any change since, the user's own edits in OrcaSlicer included). A
multi-line setting (a G-code template, the post-processing scripts) shows as `<line 1> … (<N> lines)`, or as `line
<k>: <line k> (<N> lines)` when both sides start alike, with `value_tip` / `before_tip` holding a few of its lines
(see "MCP panel page and protocol", Rows). Reading
it records nothing (it does follow the open project first, like every recorder call: a project saved under a new
name since gets its timeline moved or started there).

While the user restores the project in the panel, every API request gets
`409 {"error": "busy", "detail": "the user is restoring"}`; retry after a moment.

The timeline is kept per project file in `<data dir>/mcp_history/` (the file's path, case-folded on Windows and
macOS, picks its folder), with up to 20 restore points: the start point (before the assistant's first change)
always stays, and the 19 newest group points. The timeline itself keeps the latest 500 rows and 100 groups;
beyond that the oldest groups go, those without a restore point first.

A restore opens its file without the API's script / G-code check (it is the user's own earlier state), so the API
cannot reach that folder: `POST /project/open`, `/project/save`, `/project/move` (either path), `DELETE
/project/file` and `POST /model` answer `403 {"error": "protected_path"}` for any path inside
`<data dir>/mcp_history/`, however it is spelled (`..`, symlinks, 8.3 names and letter case of existing folders). Every restore
point also carries the size and SHA-256 of its file, taken when it was written; a restore (or the Undo of one)
whose file no longer matches is refused ("The restore point was changed on disk, so it is not used") and opens
nothing.

Object rows name their object by its id, which OrcaSlicer renews whenever a project is loaded (opened, restored,
or after a restart). After a load the panel finds the object by its name when exactly one object has it;
otherwise the row's undo says "Can't track this object after the project was reloaded".

### POST /activity/note

One row the connector adds to the panel itself: a summary for requests it marked as a trial
(`X-Orca-Mcp-Trial: 1`, which record nothing), or a refusal it made before reaching OrcaSlicer (its own
sensitive-key guard, say).
```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"kind":"slice","text":"Compared 3 options","value":"nothing kept","detail":"layer height 0.2, 0.24, 0.28 mm"}' \
  http://localhost:13130/api/v1/activity/note
```
```json
{"recorded": true}
```
- `kind` `"slice"`: a slice-kind row (`label` = `text`, `value`, `where` = `detail`, `title_hint: "note"`, no undo).
  It always starts a group of its own (never one left open by an earlier change), names it ("Compared 3 options ·
  nothing kept") and ends it the way a slice result does. No restore point is taken.
- `kind` `"refused"`: a refused row (`label` = `text`, `value` default "Not allowed", `where` = `detail`), as for a
  request OrcaSlicer refused.
- `kind` `"note"`: a plain row the assistant writes itself, in the user's language, to explain what it did or
  why (`label` = `text`, `value`, `where` = `detail`, no undo). It joins the group still open from the changes it
  explains (a new group when none is open), does not end it, and is drawn with a neutral note icon. It changes
  nothing and takes no restore point. Builds that take it list `"note"` in the panel's `capabilities.kinds`.

`text` is required (at most 160 bytes), `value` at most 80, `detail` at most 400; line breaks and tabs become
spaces, and control and format characters (C0/C1 controls, bidi overrides and isolates, zero-width characters, the
BOM) are dropped.
`400 bad_kind`, `400 missing_text`, `400 body_must_be_object`, `400 invalid_json`. The note changes nothing in the
project, and it is recorded even when the request carries the trial header.

### GET /user_changes?since=

What changed in OrcaSlicer outside the API: settings edited by hand, presets switched, objects added, removed,
moved, rotated or scaled, plates added or removed, a project opened or a new one started, and the panel's own
undos and restores. Every API request compares the project with the state the previous request left (settings
of the print, printer and every filament slot's preset, the project settings `PUT /config` writes (plate type,
filament colours, flush volumes and multipliers, prime volume mode, wipe tower position), each object's own
settings, the objects and their copies' placement, the plate count), so a difference is something the user did in
between; at the end of the request the project is taken as
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
  only for that slot). A switch that dropped unsaved edits (OrcaSlicer's "Discard"; for a filament, once no slot
  uses the old preset) names them: `"Print preset switched to 0.30mm Draft; dropped the unsaved changes: Top solid
  layers 6, Wall loops 3"` (a script, G-code or connection setting by its label only). A switch away and back that
  dropped edits stays as `"... switched to <name> and back; dropped the unsaved changes: ..."`.
- Opening a project (`"Opened project <name>"`) or starting a new one (`"New project"`) is one entry and drops
  every entry before it.
- Project settings are group `Project` (`"Plate type Cool Plate → Textured PEI Plate"`, a filament colour with
  ` (filament 2)` when more than one is loaded); flush volumes are not reported while the filament count changed,
  nor the wipe tower position while the plate count changed (they resize with it). An object's own settings are
  group `Model`, `"Wall loops of bracket.stl 2 → 4"`, with `default` for a setting the object does not set.
- Camera and view, plate selection and the user's own slices are not reported. Not compared at all: layer height
  profiles, height ranges, the settings of part modifiers and the CANVAS filament slot mapping (the panel's rows
  for those still show "changed by you" when the user changed them).

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
print, filament or printer settings as the user would by hand (no row, no policy check; `/user_changes` reports it). `/corrupt_point {"group", "from"?, "tamper"?}`
spoils a restore point (overwrites its file with `from`, or with bytes that are no 3MF) to test a failed restore;
its fingerprint is taken again, so the restore gets as far as opening it, unless `tamper` is `true`, when the
restore must refuse the changed file.
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
or form changes, or a new field a host must send. A new optional field with a default (as `off_text`,
`restore_failed` and `reveal` are) is not breaking. A host compares the page's `PROTOCOL_VERSION` with the version
it implements (in its drift test, for instance) before it ships a newer copy. The page does not read `version`
today.

### Page to host

| Message | When | The host answers |
|---|---|---|
| `{"action": "ready"}` | When the page has loaded, then again every 500 ms until the first `history` arrives (at most 20 more times) | `state`, then `history`. A repeat is normal (the first may have been lost while the bridge was starting), so answer each one, and coalesce answers that are already pending |
| `{"action": "live"}` | At load and after every `history` it receives | `live` |
| `{"action": "undo", "row": <id>}` | The user clicked a row's undo link | exactly one `result` |
| `{"action": "restore", "group": <id>}` | The user confirmed "Restore to HH:MM?" (a listed group; the page does not list `0`, the start point) | exactly one `result` |
| `{"action": "undo_restore"}` | The user clicked Undo on the "Restored to" line | exactly one `result` |
| `{"action": "reveal", "row": <id>}` | The user opened the detail line of a row with `reveal: true`, only when `capabilities.reveal` is `true` (closing it sends nothing) | nothing; show that setting in the host's settings (this fork: `Sidebar::jump_to_option`, as OrcaSlicer's own "Jump to" links, which switches to the setting's tab and page and highlights it). Ignore it during a restore or a project load, or when the setting is gone (a slice or job running does not stop it) |
| `{"action": "open_settings"}` | The settings icon, or the link in the off line (both only when `capabilities.open_settings` is not `false`) | nothing; open the host's settings (Preferences > Remote API here, the plugin's own settings page in the plugin) |
| `{"action": "open", "url": "<link>"}` | The user clicked a setup route's button that opens a link (only after a `setup` message) | nothing; open the link through the system, but only one of the `open` links the host itself sent in `setup`, compared whole |

Ids arrive as JavaScript numbers. `reveal` never gets a `result` (fire and forget). Every `undo`, `restore` and
`undo_restore` must get a `result`, also when it is refused or fails: until then the page shows "Reverting…" or
"Restoring…". A host that is busy, closing or rebuilding its window answers `failed` with the reason instead of
running the action. Run the action outside the web view's own message callback (a restore opens a project and pumps
events). A host may get `open_settings` twice in a row (a double click): open one settings window, not one inside
another.

### Host to page

**`history`**: the whole timeline, after the page's `ready` and after every change to it.
```json
{"type": "history", "version": 1,
 "rows": [ /* rows, below */ ], "groups": [ /* groups, below */ ],
 "restored_to": null, "restore_failed": false, "can_undo_restore": false, "slice_valid": true,
 "capabilities": {"restore": true, "kinds": ["setting", "preset", "model", "plate", "slice", "file", "refused", "note"],
                  "open_settings": true, "settings_hint": "", "settings_label": "Remote API settings",
                  "off_text": "The Remote API is off.", "reveal": true, "tracked_live": true, "language": "ca_ES"}}
```
- `rows`, `groups`, `restored_to`, `restore_failed` and `can_undo_restore` are the shape of `GET /activity`
  above (this fork sends exactly that object plus `type`, `version` and `capabilities`).
- `restored_to`: `null`, or the group id of the last restore point the user restored to. The page then shows
  "Restored to HH:MM" at the top (HH:MM is that group's `time`), followed by " · Undo" when `can_undo_restore` is
  true, and greys out the groups after that point.
- `restore_failed` (optional, default `false`; read only with `restored_to`): the last restore failed after it
  had started replacing the project. The line reads "Couldn't restore to HH:MM" instead, and no group is greyed
  out (nothing was marked undone).
- `slice_valid` (bool, optional): whether the timeline's newest slice result (the slice row with a `result` and no
  `error`, latest `ts`, then highest `id`) still describes its plate. `false` marks that result "out of date" also
  when no row came after it: the user's own edits in OrcaSlicer add no row. Leave it out when you cannot tell (no
  slice result, its plate gone); the page then judges by the rows alone, as before. A `history` without it clears
  the last one. This fork sends its plate's `is_slice_result_valid()` (the same test as `GET /slice/status`'s
  `valid`, for that plate), also in `GET /activity`.
- `capabilities`:
  - `restore` (bool): show restore icons and the confirm. `false` for a host without checkpoints.
  - `kinds` (list): the row kinds this host records. Informational; the page does not read it.
  - `open_settings` (bool, default `true`): the settings icon and the off line's link send `open_settings`.
    With `false` the icon's hover tag and a line under the off text show `settings_hint` instead.
  - `settings_hint` (string): where the settings are, for `open_settings: false`.
  - `settings_label` (string, optional): the settings icon's hover tag, default "Remote API settings". The off
    line's link reads "Open the " and the label, with an ordinary capitalised first word lower-cased ("Assistant
    settings" gives "Open the assistant settings"); a name keeps its capitals ("Open the Remote API settings",
    "Open the OrcaSlicer MCP settings").
  - `reveal` (bool, optional, default `false`): the host can show a setting in its settings sidebar. With `true`
    the page sends `reveal` when the user opens a row whose `reveal` is `true`, and gives such a row the hover tag
    "Show in settings" on its label. This fork sends `true`; the plugin sends nothing (no such sidebar call).
  - `tracked_live` (bool, optional, default `false`): the host sends `live` entries only for the rows it tracks:
    the latest row of each setting, whichever group it is in (ruling R43; see `live` below). With `true` the page
    shows a row's live state ("changed by you", the user's value, "removed since") whenever the row has an entry;
    without it, only for the rows of the newest group (the earlier rule). This fork sends `true`. A host that turns
    it on must send entries for its tracked rows only, and should mark the earlier rows `superseded`.
  - `off_text` (string, optional): the line shown while `state.api` is `"off"`. Default "The Remote API is
    off."; the plugin sends "Assistant access is off.", this fork sends "The Remote API couldn't start. Check its
    port." when it is enabled but not listening, and nothing otherwise.
  - `language` (string, optional): OrcaSlicer's user interface language (`"ca_ES"`, `"zh_TW"`, `"pt_BR"`;
    `"en_US"`, `""` or none = English). The page draws its words in it (see Translations). This fork sends
    `GUI_App::current_language_code()` (not the `_safe()` form, which turns Catalan, Basque and Hungarian into
    `en_US` and Traditional into Simplified Chinese).

**`live`**: the current values of the rows, after the page asks and when settings, objects or the project change.
```json
{"type": "live", "values": {"12": {"value": "20%", "present": true}, "14": {"value": "bracket.stl", "present": false}},
 "slice_valid": false}
```
`slice_valid` (optional) is the same as in `history`; send it here too, so a hand edit that only refreshes the live
values also marks the slice result. A `live` without it keeps the last one. This fork sends a `live` when a
setting, the model or the plates change, and when a plate's slice result becomes valid or out of date.
Each `live` replaces the previous set (it is not merged), so always send every entry. Keys are row ids as strings.
`value` is in the row's own display form (`value` of the row, unit included; for a multi-line row, see Rows: this
fork judges it on the raw values and sends the row's own `value` or `before` when the setting holds one of them, else
the form against the assistant's value, or `"changed since"` when that reads like either);
`present: false` means the thing the row changed is gone (an object deleted since). Which rows get an entry:
- With `capabilities.tracked_live` (this fork): the tracked rows only. A row is tracked when it is the latest row
  (not undone) of its setting: rows that changed the same thing share a subject (a preset setting: scope, key,
  element and preset; an object's setting: object and key; a copy's place: object and copy; a height range; a
  preset slot; a CANVAS slot). A row with no such subject (a slice, a refusal, an added object) is tracked as before:
  when it has an undo or sits in the newest group. Earlier rows of a subject are `superseded` in `history` and get
  no entry, and so are setting rows written to a preset the assistant later switched away from (its own preset row:
  the newest switch of that type decides; a filament slot switch only for the preset it replaced). A switch the
  user made has no row, so those rows stay tracked and read "changed by you". This fork's rule is `mcp_tracking` in `src/slic3r/GUI/RemoteAPI/McpHistory.cpp`.
- Without it: every row that has an undo and is not undone, and every row of the newest group.
Leave out rows you cannot read. The page compares numbers with the same unit text within 1e-6 (relative) and anything else
exactly and case-sensitively, so send the same form the row was recorded in (a colour recorded as `#F2754E` must
come back as `#F2754E`, not `#f2754e`). For geometry (moves, rotations, arranges) the host
compares and sends the row's own `value` when nothing moved, else another text such as `"moved since"`.
A different live value is shown with "changed by you" and the user's value, a missing one with "removed since"
(with `tracked_live`: on any row with an entry; without it: in the newest group). An undo is then unavailable:
"You changed it since" (with `tracked_live` in any group; without it in the newest group, "Changed again since;
restore an earlier point instead" in older ones), "<name> is no longer on the plate". A `superseded` row's undo
is always unavailable with "Changed again since; restore an earlier point instead" ("Changed again since" when
`capabilities.restore` is `false`), and it never says "changed by you".

**`state`**: `{"type": "state", "api": "on" | "off"}`, after `ready` and whenever it changes. `on` means the
assistant can reach OrcaSlicer now (this fork: the Remote API server is listening, not only enabled in
Preferences). `off` shows `off_text` above the timeline. Until the first `state` the page assumes `on`.

**`setup`** (optional): how to connect an AI app, for a host that wants the empty timeline to do that. The plugin
sends it after `ready` and again when the first app connects; this fork sends none, and the page then shows its
usual empty line.
```json
{"type": "setup", "status": null, "paused": false,
 "apps": [{"id": "claude", "name": "Claude", "where": "Claude", "icon": "<svg ...>",
           "routes": [{"label": "Claude app", "hint": "For chats. Download the extension, then open the file.",
                       "act": "Download", "primary": true, "open": "https://...", "copy": null, "also": "", "off": false}]}]}
```
While the timeline is empty, the page shows a square per app (the first chosen), the routes of the chosen one with
one button each (`open` sends `{"action": "open"}`, else `copy` goes to the clipboard; `off` routes have no button
and a dashed border; `also` is appended to the hint), and "Waiting for <where>…". With `status`
(`{"seen": <unix seconds>, "app": "<name>"}`) it shows "Connected to <app>" and where the changes will appear
instead; the first change replaces either with the timeline. There is no question to ask: the first request from
the app switches the view by itself.

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

Every string a host sends is shown as plain text: the page escapes it, so HTML in it is not interpreted.

```json
{"id": 12, "group": 3, "ts": 1762680660, "time": "10:31", "kind": "setting",
 "label": "Sparse infill density", "value": "15%", "before": "25%", "where": "0.20mm Standard @Printer (unsaved)",
 "undo": {"verb": "Revert", "tip": "Set Sparse infill density back to 25%"}, "undone_at": "",
 "title_hint": null, "result": null, "error": "", "on_disk": false, "reveal": true}
```
- `id` (int): unique within the project's timeline. `group` (int): the group it belongs to.
- `ts` (seconds) and `time` (`"HH:MM"`, local): the host formats the time; the page never does.
- `kind`: `setting`, `preset`, `model`, `plate`, `slice`, `file` or `refused`. It picks the icon and its
  colour; `refused` (and a `slice` row with an `error`) is shown in red. Any other kind is shown with no icon.
- `label`: OrcaSlicer's own label (a setting's full label: "Nozzle temperature", not "Other layers").
- `value`: the new value as shown (unit added, enum label). A `setting` row whose value is a colour (`#RGB`,
  `#RRGGBB` or `#RRGGBBAA`, as filament colour rows carry it, label "Filament 2 colour") gets a small colour
  swatch beside it.
- `before`: the previous value in the same form as `value`, or `""`. The page writes "Was <before>"; send no
  prefix. `where`: any other detail (the preset, the plate, why it was refused), shown after it.
- **Multi-line values** (a G-code template such as Start G-code, the post-processing scripts with one script per
  line): never the raw text with its escaped line breaks. A side with two lines or more is shown as
  `<line 1> … (<N> lines)`, for example `G28 ; home all … (14 lines)`. When both sides start with the same line,
  each side shows the first line that differs instead: `line <k>: <line k> (<N> lines)`, for example
  `line 3: M104 S210 (14 lines)` against `line 3: M104 S200 (14 lines)`, so a change and its change back read
  differently. A side without a line k (the other one is longer) keeps the first form. Rules: a `"\r\n"` counts as
  one break and a final line break only ends the last line; a shown line drops its trailing spaces and tabs, reads
  `(empty line)` when blank, and is cut at 40 characters with `…` (then `<cut line 1>… (<N> lines)`, one `…`); a
  side with fewer than two lines keeps the single-line form. `…` is U+2026. This fork builds it in
  `mcp_multiline_display` (`McpHistory.cpp`); undo works on the raw values (in the row's target).
- `value_tip`, `before_tip` (string, optional): the hover tag of a multi-line `value` / `before`: `Lines <a> to <b>
  of <N>:` (or `Line <a> of <N>:`) and then those lines, one per line (`"\n"`), at most 5, from line k as above or
  else line 1, each cut at 60 characters, then a line `…` when more follow. The page shows the tag of the side the
  row shows (its value, or the before once it is reverted or set back), and `before_tip` on the "Was" line.
- `undo`: `null`, or `{"verb": "Revert", "tip"?: "<hover text>"}`. Without `tip` the hover tag is
  "<verb>: <label> back to <before>" (or "<verb> <value>" with no `before`).
- `undone_at`: `""`, or the `"HH:MM"` the user undid it (also set on the rows a restore undid).
- `title_hint`: `null`, `"added"` or `"opened"`: such a row names its group ("Added bracket.stl and 2 more");
  `"note"` (a connector's note, `POST /activity/note`): it names a group that holds no change ("Compared 3 options
  · nothing kept").
- `superseded` (bool, optional, default `false`): a later row (not undone) changed the same thing; the page offers
  no undo ("Changed again since; restore an earlier point instead") and shows no live state for it.
- `dropped` (bool, optional, default `false`): the assistant's own preset switch dropped this row's edit. The page
  shows the value it set, faint, with "dropped by a preset switch" under the label, never a live value or "changed
  by you", and no undo ("Dropped by a preset switch; restore an earlier point instead", or without the restore part
  when `capabilities.restore` is false). Optional for any host.
- `result`: `null`, or `{"seconds": n, "grams": n}` on a slice row; the group shows the time of its last slice.
- `error`: `""` or the text of a refusal or failure (any non-empty text marks the group with a warning).
- `on_disk` (bool): the change wrote to disk (a saved or moved project, a saved or deleted preset); the restore
  confirm then adds "Files and presets saved to disk stay."
- `reveal` (bool, optional, default `false`): the host can show this row's setting in its settings sidebar
  (read only with `capabilities.reveal`). This fork marks setting rows of the print, filament and printer presets;
  per-object settings, project keys (filament colours, wipe tower position), presets, models and plates are not
  marked.
- `local` (object, optional): the host's own translation of English words in this row, `{"<English>": "<in the
  user's language>"}`: a setting's name and a choice's label, as OrcaSlicer itself shows them ("Wall loops",
  "Classic"). The page uses them for the label, the values and inside templates (a tip "Set Wall loops back to
  2"). Sent only when `capabilities.language` is not English, and only for words that differ.
- The kind `note` (`POST /activity/note` with `"kind": "note"`): the assistant's own words, shown as they are with
  a neutral icon, never translated, never the group's title while the group holds a change.

### Translations

The page translates what it draws (orcaslicer-mcp #21). Everything a host sends stays English in the rows, the API
replies and the assistant's change journal: the assistant and the host's code read them.
- `resources/web/orcaslicer_mcp/i18n/<code>.json` (OrcaSlicer's 22 languages): `{"language", "name",
  "machine_translated", "phrases": {"<English>": "<translation>"}, "review"?: {"<English>": "<what to check>"}}`.
  They start machine-made (with OrcaSlicer's translation glossary and its own wording); corrections are welcome.
- `catalog.json` lists every phrase: the page's own (`t('...')`, `N_('...')` in panel.html), the ones this fork
  sends (`MCP_T("...")` and `mcp_fmt(MCP_T("Set {label} back to {before}"), ...)` in `src/slic3r/GUI/RemoteAPI`,
  see `McpText.hpp`), and `external.json` (the connector's notes).
- `python3 scripts/mcp_panel_i18n.py update` rewrites the catalogue and the page's `panel-i18n` block (all languages
  inline, so the page stays one file for the plugin too); `check` fails on a stale block or a language missing a
  phrase or changing its `{placeholders}`. `tests/remote-api-panel/panel-i18n.test.mjs` checks the same.
- The page picks the table from `capabilities.language`: the code, else its language (`ca`), else a table of that
  language (`pt` -> `pt_BR`), else English. A phrase the host sends is translated when it matches a catalogue
  entry exactly, or a template: `{n}`, `{count}`, `{index}`, `{line}`, `{first}`, `{last}` match digits, `{time}`
  `H:MM`/`HH:MM`, any other name any text, and those parts are translated in turn ("Put Cube back on Plate 2").
  Anything else (object, preset and file names, numbers) is drawn as it came.

### What the page shows (for hosts and testers)

- **Keyboard.** Every control is a focus stop with a visible ring in the theme's focus colour: the fold and
  settings icons, each group's title (a button: Enter or Space opens and closes it), the restore icon of an older
  group, each row (role button: Enter or Space opens its detail line), the undo link inside an open row, the restore
  question's Yes and No, and the "Restored to" line's Undo. Esc answers No to the restore question (the focus goes
  back to the restore icon) and closes the open row the focus is in. Focus stays on the same control when the list
  is redrawn. After Yes the focus goes to the group's title while it restores, then to the "Restored to" line's
  Undo when that arrives with the next `history` (or to the error's OK), also when the host held the focus in
  between. The control the keyboard focused shows its hover tag in one placed element: above the control, or below
  it when there is no room, inside the pane and never over the control (mouse hover tags are unchanged). Ctrl+P and Ctrl+S (Cmd on macOS) do nothing in the page, and `window.print` is a no-op: a web view
  would otherwise open a print or save sheet over the panel. A host that wants its own shortcuts there has to catch
  them before the web view.
- **Colours.** The page reads the body's background, sets `data-theme` (`dark` or `light`) on the root and then
  measures its own colours against that background and the row tints over it (hover and open rows: mid grey at 8 %
  and 12 %). The link, faint and warning colours start from #5ad1c4 / #9a9aa0 / #f28b82 on dark and #00695c /
  #5f5f64 / #b3261e on light and move in lightness until text reaches 4.5:1 there; icons and the focus ring reach
  3:1. On OrcaSlicer's own themes the links stay #5ad1c4 (7.65:1 on #2b2b2b, 6.62:1 on a hovered row) and #00695c
  (6.61:1 on white, 5.78:1 hovered). An action is underlined in that colour; an unavailable one is faint and not
  underlined. The page undoes the element defaults a host injects for plugin pages (every `button` on the accent
  colour with a border and padding): its buttons draw on the row, not on the accent.
- **Reverted rows** show what is there now in the value column, the undone value struck through under the label
  ("~~160 mm/s~~ reverted"), and "Reverted" at the right. A value longer than 18 characters goes on its own line
  under the label. Focus is put back after a redraw only while the page has the keyboard focus. A disabled control
  (a restore icon without a point) does nothing.
- **Out-of-date slices.** A slice result with a later setting, preset, model or plate row after it (or a revert or
  restore the user made since), and the newest one when the host's `slice_valid` is `false`, is faint with "out of
  date", in its row and in its group's title line.
- **Restore** is a dimmed icon on every older group (brighter on hover and focus), with the hover tag "Restore the
  project to how it was at HH:MM (undoes later changes)". The question "Restore to HH:MM? Yes No" shows under the
  group's title. After the user undoes a restore, a faint "Restore undone" line stays until the next change.
- **Rows.** A setting row's label (this fork's history) is the text the settings sidebar draws for that setting
  ("Top shell layers"), with its section only when that is needed to tell it apart ("Acceleration: Normal printing",
  "Other layers speed: Outer wall"), never "X (Y)". Labels wrap to a second line instead of being cut on one. Refusals come after the changes of their group
  and never name it. A setting row's detail line reads
  "Was X"; the preset it was written to ("0.20mm Standard @Printer (unsaved)") is in that line's hover tag. The
  row's hover tag says what a click does ("Show what it was, undo it, and find it in settings" with `reveal`,
  "Show what it was and undo it" without). Long values and reasons wrap; the panel never scrolls sideways.

### Groups

```json
{"id": 3, "ts": 1762680660, "time": "10:31", "restore": {"available": true, "why": ""}}
```
One per burst of changes; `0` is the point before the assistant's first change (always last). Send it with the
others: the page does not list it, but a "Restored to" line for it takes its `time`. The page orders the groups
newest first by `ts`, then `id`, opens the newest and shows its title in bold, the older titles muted (the
"Restored to" line above them is not a group). When a newer group arrives, the one that was newest closes, unless
the user opened it by hand (a click on its title, or Expand all); groups the user opened stay open until the user
closes them (a click, or Collapse all). `restore` is `null` for the newest group (and for any group a host
cannot restore to), `{"available": true}` for a restore point, or `{"available": false, "why": "<hover text>"}`. A
row whose group is not listed still gets a group, from its own `ts` and `time`.

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
