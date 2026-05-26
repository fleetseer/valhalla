# Routing Customizations

## Request-Scoped Edge Whitelisting

Route and matrix requests can restrict routing to an allowed set of directed
Valhalla edge IDs without rewriting graph tiles or changing the binary tile
format.

JSON request field:

```json
{
  "locations": [{"lat": 0.0, "lon": 0.0}, {"lat": 0.1, "lon": 0.1}],
  "costing": "auto",
  "edge_whitelist": [123456789]
}
```

Behavior:

- `edge_whitelist` contains directed `GraphId` integer values.
- Empty `edge_whitelist: []` is an enabled allowlist with no allowed edges, so
  the request fails closed.
- The JSON parser copies the root allowlist into each active costing's
  `Costing.Options`.
- PBF clients can set `Costing.Options.edge_whitelist_enabled` and
  `Costing.Options.edge_whitelist` directly.
- `DynamicCost` stores the allowlist as a sorted `GraphId` vector and checks it
  with binary search from the existing user-avoid hooks.
- Route and matrix failures use normal no-path / unreachable behavior.

The hot-path check does not allocate and does not hash.

## Ordered Route And Matrix Edge IDs

Route and matrix responses can include ordered directed edge IDs.

Route request field:

```json
{
  "locations": [{"lat": 0.0, "lon": 0.0}, {"lat": 0.1, "lon": 0.1}],
  "costing": "auto",
  "include_route_edge_ids": true
}
```

Route output:

- Valhalla JSON: `trip.legs[].edge_ids`
- PBF: `DirectionsLeg.edge_id`, and `TripLeg.edge_id` when the `trip` field
  selector is requested.

Matrix request field:

```json
{
  "sources": [{"lat": 0.0, "lon": 0.0}],
  "targets": [{"lat": 0.1, "lon": 0.1}],
  "costing": "auto",
  "include_route_edge_ids": true
}
```

Matrix output:

- Verbose Valhalla JSON: `sources_to_targets[source][target].edge_ids`
- Slim Valhalla JSON: `sources_to_targets.edge_ids[source][target]`
- PBF: `Matrix.edge_ids[cell_index].edge_id`

The IDs are copied from the final `TripLeg` node edges, so normal route
construction returns the leg edge sequence after shortcut recovery. Matrix IDs
are copied from the reconstructed `CostMatrix` path. Default route and matrix
responses are unchanged.

## Crash Hardening

Replace native crashes in disconnected or closed-graph cases with structured
errors.

Hardened targets:

- `src/thor/map_matcher.cc` checks missing tiles before dereferencing matched
  edges, interpolation edges, and end-node tiles.
- `src/thor/route_matcher.cc` checks destination edge tiles while collecting end
  nodes.
- `src/thor/route_action.cc` checks shortcut and base-edge tiles in
  `add_shortcut()`.
- `test/worker_nullptr_tiles.cc` now exercises route, trace route, trace
  attributes, and closed-graph allowlist requests with random null tile reads.

## Tests

Focused tests live in `test/gurka/test_routing_customizations.cc`:

- whitelist allows a route when all required edges are present;
- whitelist rejects a route when a required edge is absent;
- matrix returns finite costs for connected pairs and null costs for pairs
  closed by the allowlist;
- matrix edge ID output matches the route edge ID sequence and fails closed with
  empty sequences when the allowlist is empty;
- optional route edge ID output returns the expected ordered edge sequence in
  Valhalla JSON, default PBF directions output, and PBF trip-selector output.

Run the focused tests:

```bash
cmake --build build --target gurka_routing_customizations worker_nullptr_tiles -j$(nproc)
cmake --build build --target run-gurka_routing_customizations
./build/test/worker_nullptr_tiles
```

## Importing This Fork

Recommended: pin this fork by commit SHA in the consuming project.

Submodule:

```bash
git submodule add git@github.com:<your-org-or-user>/valhalla.git third_party/valhalla
git -C third_party/valhalla fetch origin
git -C third_party/valhalla checkout <valhalla-custom-commit-sha>
git add .gitmodules third_party/valhalla
git commit -m "Pin custom Valhalla fork"
```

CMake `FetchContent`:

```cmake
include(FetchContent)

FetchContent_Declare(
  valhalla
  GIT_REPOSITORY git@github.com:<your-org-or-user>/valhalla.git
  GIT_TAG <valhalla-custom-commit-sha>
)
FetchContent_MakeAvailable(valhalla)
```

Avoid tracking a moving branch from the consuming project. Use a SHA or an
annotated tag so rebuilds are reproducible.

## Keeping The Fork Current

Set upstream once:

```bash
git remote add upstream https://github.com/valhalla/valhalla.git
git fetch upstream
```

Refresh the customization branch:

```bash
git switch feat/routing-customizations
git fetch upstream
git rebase upstream/master
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_CCACHE=OFF -DENABLE_WERROR=OFF -DENABLE_SINGLE_FILES_WERROR=OFF
cmake --build build --target parse_request gurka_routing_customizations worker_nullptr_tiles -j$(nproc)
./build/test/parse_request
cmake --build build --target run-gurka_routing_customizations
./build/test/worker_nullptr_tiles
git push --force-with-lease origin feat/routing-customizations
```

Update the consuming project after the fork has a new tested commit:

```bash
git -C third_party/valhalla fetch origin
git -C third_party/valhalla checkout <new-valhalla-custom-commit-sha>
git add third_party/valhalla
git commit -m "Update custom Valhalla fork"
```
