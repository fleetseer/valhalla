# Proposed Routing Customizations

## Request-Scoped Edge Whitelisting

Add a request-scoped way to restrict routing to an allowed set of directed
Valhalla edge IDs.

Desired behavior:

- support route and matrix requests;
- accept directed edge IDs as Valhalla `GraphId` values;
- treat any non-whitelisted directed edge as disallowed during costing;
- apply the check in the normal Sif/Thor routing path rather than by rewriting
  or copying graph tiles;
- leave the binary tile format unchanged;
- return normal no-route responses for disconnected origin/destination pairs.

Implementation candidates to compare:

- sorted `GraphId` vector with binary search;
- tile-local bitsets keyed by tile ID and local edge index;
- preloaded allowlist profile IDs referenced by request;
- request-local allowlist payloads for small ad hoc cases.

The whitelist check is on a hot path, so the chosen structure should avoid
allocation and expensive hashing inside `Allowed()` / `EdgeCost()` loops.

## Ordered Route Edge IDs

Add an opt-in route output field containing ordered directed edge IDs for the
selected path.

Desired behavior:

- expose the route's ordered directed `GraphId` edge sequence;
- make the field optional so default route responses do not change;
- support JSON and pbf output if practical;
- return edge IDs per leg, preserving route order;
- expand shortcut edges to base directed edges, or explicitly document when a
  returned ID is a shortcut.

This should avoid needing a second `trace_attributes` call merely to recover the
edge IDs used by a route.

## Crash Hardening

Replace native crashes in disconnected or closed-graph cases with structured
errors.

Known hardening targets from source inspection:

- `src/thor/map_matcher.cc:210-211` calls `GetGraphTile(edge_id, tile)` and then
  immediately dereferences `tile->directededge(edge_id)`. Add a null check and
  treat a missing tile as a discontinuity or no-match condition.
- `src/thor/route_matcher.cc:99-100` calls `reader.GetGraphTile(graphid)` while
  collecting destination end nodes and immediately dereferences
  `tile->directededge(graphid)`. Add a null check, matching the existing
  opposing-edge branch at `src/thor/route_matcher.cc:105-108`.
- `src/thor/route_action.cc:187-193` fetches shortcut/base-edge tiles in
  `add_shortcut()` and immediately dereferences them. Add explicit checks before
  using `tile->directededge(...)`, and return/throw a normal routing error when
  shortcut recovery cannot safely continue.
- `src/thor/triplegbuilder.cc:1849-1853`, `1937-1940`, and `1969-1972` already
  show the preferred pattern: check for missing graph tiles and throw
  `tile_gone_error_t` instead of dereferencing null.
- `test/worker_nullptr_tiles.cc:37-42` already simulates random
  `GraphReader::GetGraphTile()` null returns. Extend that coverage to
  `trace_attributes`, `trace_route`, and allowlist/closed-graph requests.

Cases to test:

- route request where all candidate paths are closed by an allowlist;
- matrix request with some connected and some disconnected OD pairs;
- sparse trace or `trace_attributes` request with gaps or no valid connection;
- trip-leg construction after no connected path is available.

Expected behavior:

- return a no-route / no-match response instead of crashing;
- include enough diagnostic context to identify the failing request or pair;
- prefer failing closed when access is ambiguous.

## Tests

Add focused Gurka tests for:

- whitelist allows a route when all required edges are present;
- whitelist rejects a route when a required edge is absent;
- matrix returns finite costs for connected pairs and no-route values for
  disconnected pairs;
- optional route edge ID output returns the expected ordered edge sequence;
- disconnected or fully closed requests do not segfault.
