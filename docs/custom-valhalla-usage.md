# Custom Valhalla Usage

This fork adds request-level routing controls that are not available in base
Valhalla. Point agents and consuming projects at this note when they need to
call the fork-specific behavior.

## Custom Request Fields

### `edge_whitelist`

Restricts route and matrix expansion to a per-request set of directed Valhalla
`GraphId` edge IDs.

```json
{
  "locations": [{"lat": 0.0, "lon": 0.0}, {"lat": 0.1, "lon": 0.1}],
  "costing": "auto",
  "edge_whitelist": [123456789]
}
```

Notes:

- Values are directed edge IDs, not OSM way IDs.
- The allowlist is request-scoped; different requests can use different edge
  lists.
- `edge_whitelist: []` is enabled and fails closed.
- JSON requests put the field at the root. The parser copies it into each
  active costing option.
- PBF callers set `Costing.Options.edge_whitelist_enabled = true` and fill
  `Costing.Options.edge_whitelist` directly.

### `include_route_edge_ids`

Adds the ordered directed edge IDs used by each route leg or finite matrix
result cell.

```json
{
  "locations": [{"lat": 0.0, "lon": 0.0}, {"lat": 0.1, "lon": 0.1}],
  "costing": "auto",
  "include_route_edge_ids": true
}
```

Route outputs:

- JSON: `trip.legs[].edge_ids`
- Default PBF route response: `directions.routes[].legs[].edge_id`
- PBF with `Options.pbf_field_selector.trip = true`:
  `trip.routes[].legs[].edge_id`

Matrix request:

```json
{
  "sources": [{"lat": 0.0, "lon": 0.0}],
  "targets": [{"lat": 0.1, "lon": 0.1}],
  "costing": "auto",
  "include_route_edge_ids": true
}
```

Matrix outputs:

- Verbose JSON cells: `sources_to_targets[source][target].edge_ids`
- Slim JSON: `sources_to_targets.edge_ids[source][target]`
- PBF: `matrix.edge_ids[cell_index].edge_id`

Matrix cell order matches the existing matrix arrays:
`cell_index = source_index * targets.length + target_index`. Unreachable cells
have an empty edge ID sequence.

The IDs come from the final `TripLeg` after route construction and shortcut
recovery for `/route`, and from the reconstructed `CostMatrix` path for
`/matrix`. They are tied to the exact tile build/extract used for the request.

## Node Binding Usage

Use a Node package built from this fork, not the upstream/base Valhalla package.
The local build must use:

```bash
cmake -S . -B build -DENABLE_NODE_BINDINGS=ON
cmake --build build --target valhalla_node -j$(nproc)
```

Example JSON response:

```js
const { Actor } = require('@valhallajs/valhallajs');

const actor = await Actor.fromConfigFile('/path/to/valhalla.json');
const result = await actor.route({
  locations,
  costing: 'auto',
  edge_whitelist: edgeIds,
  include_route_edge_ids: true,
});

console.log(result.trip.legs[0].edge_ids);

const matrix = await actor.matrix({
  sources,
  targets,
  costing: 'auto',
  edge_whitelist: edgeIds,
  include_route_edge_ids: true,
});

console.log(matrix.sources_to_targets[0][0].edge_ids);
```

Example PBF response:

```js
const pbf = await actor.route({
  locations,
  costing: 'auto',
  edge_whitelist: edgeIds,
  include_route_edge_ids: true,
  format: 'pbf',
});
```

The Node binding returns a `Buffer` for `format: 'pbf'`. Decode it with proto
descriptors from this fork. With protobufjs default camelCase conversion,
`edge_id` appears as `edgeId`.

## Compatibility Notes

- No tile format change is required.
- Existing requests are unchanged unless one of these custom fields is set.
- Saved edge ID lists are only reliable with the same tileset that produced
  them.
- Base Valhalla will not understand these fields or return these edge ID
  arrays.
