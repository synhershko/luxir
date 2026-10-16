# Operating Luxir

Luxir runs as one process on one machine and hosts many isolated
collections. Capacity comes from the size of that machine.
Replication, sharding across nodes, failover orchestration, and snapshots are
not built into the server yet; the
[production boundary](#current-production-boundary) at the end of this page
lists what is missing.

The defaults are good for evaluation. Before you keep real data, decide on
persistence and network isolation. The other settings on this page can stay
at their defaults until you have a reason to change them.

## Persistent storage

The default backend is in-memory and loses every collection when the process
exits:

```bash
./luxir
```

Use the filesystem backend on local storage for durable data:

```bash
./luxir --store.backend=fs --store.data-dir=/srv/luxir/data
```

Each collection is an independent index below the data directory. Collections
present there are discovered at startup; a write to a new collection creates
its storage by default.

A commit makes updates durable and visible to searches. After a crash, Luxir
reopens the last durable commit. Updates accepted since that commit can be lost.

Use [`luxir pull`](replication.md#copy-restore-or-promote) for an online copy,
then `luxir promote` to restore it as a writer under new collection incarnations.
Do not restore a raw directory copy over an existing writer identity.

## Read-only nodes

A data directory has exactly one writer, which holds `write.lock` from startup
until exit.

`--read-only` opens an existing data directory without that lock:

```bash
./luxir --read-only --store.backend=fs --store.data-dir=/srv/luxir/data
```

A read-only node requires an existing data directory and writes no files.
It serves searches, schema reads, and `_stats`.

Use it to query a directory another instance is writing, or to inspect one
offline without risking a stray write. Current limitations:

- **The view does not advance.** A read-only node opens its index view at
  startup and never reopens, so later commits are invisible until it restarts.
- **`_stats` describes the same snapshot as searches.** Directory byte totals
  still reflect the files present when stats are requested.
- **Pinned files are not reclaimed.** Segment files are held open by `mmap`, so
  files the writer deletes stay on disk until the read-only node exits. A
  long-lived read-only node against a busy writer holds disk space that `du`
  attributes to no visible file.

Restart the read-only node to pick up newer commits and release pinned files.
For a continuously refreshed copy with its own storage, use
[`--replicate-from http://writer:9400`](replication.md).
Followers own their local data directory and install verified snapshots without
constructing an index writer.

## Collection lifecycle

Create and delete collections with `POST /collections/_create` and
`POST /collections/_delete` (or unary `luxir.Admin/CreateCollection` /
`DeleteCollection`). Deleting a collection also deletes its stored data; there
is no undo. `GET /collections` (canonically `/collections/_list`) returns the
sorted collection names, including any that failed to load; `/_stats` has the
per-collection detail.

Deletion is synchronous and wins over concurrent use. When the call returns,
the name resolves to nothing, the on-disk data is gone, and the name can be
recreated as a fresh empty collection. New requests receive
`collection_unavailable` while deletion is in progress. Searches already
executing keep their index view and complete normally.
Deletion waits for indexing work already accepted, including a running merge,
so deleting a collection mid-merge can take as long as that merge.

On the filesystem backend collections use
`t/<tenant>/<name>/<incarnation>/`, selected by an atomically replaced `CURRENT`
file in `t/<tenant>/<name>/`. Single-tenant deployments use the default tenant
`t0`, so the default collection lives in `t/t0/main/`. An incarnation is 16 hex
digits. `t/LAYOUT` names this layout; a data directory written in another layout
is refused at startup and must be reindexed. Deletion removes and
syncs `CURRENT` before removing the directory tree, so an interrupted deletion
cannot reopen the partially deleted index. If
deletion fails partway (for example an I/O error), the name stays unavailable
with the recorded error and the delete can simply be retried. Deleting a
collection that failed to load at startup is also the supported way to clear
its broken on-disk state.

## Visibility policy

Updates become visible to searches at commit. An
immediate `"commit": {}` waits until a
new index view is published. `commit_within_ms` can coalesce publication and
return before the update is visible while still bounding staleness.

Search `freshness_ms` expresses how stale a reader may be; `0` requires the
latest commit. Decide these together:

- Interactive writes that must be read immediately should use an immediate
  commit.
- Sustained feeds should use a positive commit interval rather than publishing
  a new view for every small update.
- Forced merges are a maintenance action, not part of a routine commit.

`max_segments` and `wait_for_merges` are explicit maintenance controls. A
normal commit lets background merging choose the layout while ingest and search
continue.

## Network ports and security

Defaults:

| Surface | Port | Control |
|---|---:|---|
| HTTP/JSON | 9400 | `--server.http.port` or `-p` |
| gRPC | 9401 | `--server.grpc.port` (defaults to HTTP port plus one) |

A configured nonzero port binds `0.0.0.0`; there is no listen-address option
yet. Disable HTTP with `--no-http`. The gRPC server always starts.

Luxir does **not** provide TLS, authentication, authorization, per-tenant
quotas, or a permission model. Do not expose either port directly to an
untrusted network. Network policy is mandatory: place the
process in a private network namespace, firewall both ports, or front them with
an authenticated TLS proxy/service mesh.

## Health and logging

HTTP liveness:

```bash
curl http://localhost:9400/health
```

```json
{"status":"ok"}
```

gRPC registers the standard health service. The current HTTP check is process
liveness, not a deep read/write check of every collection or storage device.

Use `GET /_stats` for node-wide operational state or
`GET /collections/{collection}/_stats` for one collection. The response
distinguishes writer-visible segments from committed segments and reports
document counts, generations, merge activity, and query-cache counters.
Per-segment records are omitted by default; add `?segments=true` when
diagnosing segment layout. A node-wide sample includes
load-failure tombstones with their `error` and excludes them from totals. Each
collection index is sampled coherently, but a node-wide response is not one
transaction across collections. The same payload is available through unary
`luxir.Admin/Stats`; omit its collection target for the node-wide view.

`GET /_stats?memory=true` (`StatsRequest.memory`) returns only node memory,
without walking collections, so it is cheap enough to poll: the indexing RAM
budget, RAM storage usage, and, when Luxir runs on jemalloc, a `jemalloc`
block with the allocator's process-wide statistics (allocated, active,
resident, retained, dirty) and its dedicated indexing and big-buffer arenas.
It is node-wide, so it is rejected on a collection or tenant route and with
`segments`. `jemalloc.allocated_bytes` counts live bytes allocated through
jemalloc; `resident_bytes` is jemalloc's upper bound including fragmentation
and freed pages awaiting decay; the process RssAnon also counts memory
jemalloc does not manage.

Counts follow the JSON dialect: a zero-valued field is omitted rather than
emitted, so a scraper must read an absent field as zero. Totals appear at the
node, collection, and index levels, and each level sets only the counts that
mean something there - `collections` only on the node total, `shards` only on
node and collection totals.

Totals also report on-disk `bytes`, where the index level counts the whole
directory (manifest and in-flight files), so it can exceed the sum
of segment bytes. With `?segments=true` each segment reports its own `bytes`
(data + deletes + overlays) and each aux entry reports the bytes of its listed
files.

Ids and generations that appear in filenames use their filesystem spelling so
the response correlates directly with a directory listing: each segment's
`seg` is its data-file prefix (e.g. `s0a`), while `live_gen` and aux `gen`
are the sortable strings embedded in filenames (segment `s0a` with `live_gen`
`01` has its deletes in `s0a__L01`). These strings sort in generation order,
and an absent field means none. `schema_gen`, `index_gen`, `core_gen`, and
`update_version` are numeric. The schema and its history are embedded in each `commit_<index_gen>` manifest.

Filesystem publication syncs new data files and then the directory before
writing a new `commit_<index_gen>` manifest with a length and xxh3 checksum
footer. It syncs that manifest and then the directory before acknowledging
the commit. Startup checks the newest manifest's footer and decodes its payload;
by default it does not hash data files (see
[file integrity checks](#file-integrity-checks)). Torn candidates are skipped.
Fallback candidates also require their referenced files to be present with
matching sizes.

Once the new root is durable, obsolete manifests and unreferenced data are removed.
Replication pin counters are described in the
[replication reference](replication.md#reference). Replication file transfers have
a fixed 60 s idle deadline per socket write, reset as writes complete. Ordinary
HTTP responses have no write deadline.

Writer startup removes leftover unreferenced
index files using the directory listing, without reading their contents. If
startup falls back below the highest manifest generation, it logs an error and
skips this sweep, preserving newer files for recovery.

Removals are not directory-synced: a crash may restore obsolete names, but the
newest durable manifest does not reference them. Failed publication candidates
are removed and the directory is synced best-effort. An error response does not
guarantee that the commit is absent: a crash or cleanup failure can leave a
complete, unacknowledged candidate recoverable at startup. The local filename is not part
of the commit identity: clients use the `incarnation:index_gen` token. That
token's manifest is `<incarnation>/commit_<index_gen>`, with the generation in
decimal; tools that list manifests in order sort by the parsed generation.

Set log verbosity with:

```bash
./luxir --log-level=info
```

Valid spdlog levels include `trace`, `debug`, `info`, `warn`, `error`, and
`critical`. Invalid values fail startup instead of disabling diagnostics.
Startup logs include the loaded time-zone database version.

Server log records use UTC timestamps with the date, severity, thread ID and
source location. Newlines and control characters in messages are escaped so
each record occupies one physical line. Request failures include operation,
tenant, collection, request ID and error code where available. Internal failures
are errors; request rejections and resource limits are warnings. A failed server
bind exits with a nonzero status.

## Prometheus monitoring

`GET /metrics` on the HTTP port exports Prometheus text format 0.0.4. It is
available whenever the HTTP server is enabled, including on read-only nodes.
No exporter process or extra runtime dependency is required. This endpoint has
the same network access boundary as the administration API and includes host
information and tenant/collection names.

```yaml
scrape_configs:
  - job_name: luxir
    scrape_interval: 15s
    static_configs:
      - targets: ["localhost:9400"]
```

Prometheus supplies `up`, `job`, and `instance`. Aggregate service traffic over
instances; retain `instance` when diagnosing a specific node. `/metrics` scrapes
are excluded from service request totals and application response-byte counts.
Health checks are counted under `operation="health"`.

### Service and engine metrics

| Metric or family | Meaning |
| --- | --- |
| `luxir_requests_total`, `luxir_requests_in_flight`, `luxir_requests_duration_seconds` | HTTP requests or gRPC calls, by `transport`, bounded `operation`, and completion `outcome`. Duration includes body reception after HTTP headers, execution, waits, and response delivery. A gRPC stream is one call, regardless of its message count. |
| `luxir_request_errors_total` | First application failure per request/call, by transport and fixed error `kind`; includes errors carried inside successful transport responses. |
| `luxir_received_body_bytes_total`, `luxir_response_body_bytes_total` | Application payload throughput; excludes transport framing. Response bytes are counted when submitted for sending, even if delivery subsequently fails. |
| `luxir_search_total`, `luxir_search_in_flight`, `luxir_search_duration_seconds` | Engine search requests, including individual messages on a gRPC stream. Ends when the final response is handed to the transport. |
| `luxir_search_execution_duration_seconds` | Search-body execution, excluding scheduler/visibility waits and later streaming drains. |
| `luxir_search_resource_exhausted_total`, `luxir_search_warnings_total` | Resource-limit failures (including the request memory breaker), and searches with declared degradation warnings. |
| `luxir_index_batches_total`, `luxir_index_batches_in_flight`, `luxir_index_batches_duration_seconds` | Index-batch processing inside the engine, excluding queueing and commit work. HTTP NDJSON can produce many batches per request. |
| `luxir_ingest_documents_total`, `luxir_ingest_delete_ids_total` | Documents accepted into indexing or rejected/rolled back, and accepted delete IDs. Acceptance does not mean durability; commits can still fail. Deletes count submitted IDs, not matched documents. |
| `luxir_stream_pauses_total` | Response backpressure events by transport. |
| `luxir_http_connections`, `luxir_http_io_threads` | Live HTTP sessions and running connection-shard threads; the thread gauge excludes the accept thread. gRPC active calls are in `luxir_requests_in_flight`. |
| `luxir_background_total`, `luxir_background_in_flight`, `luxir_background_duration_seconds` | Flush, commit-finish, merge, vector-build, and reader-refresh stages by `kind`. These are stage attempts; a commit handing off to a forced merge can have multiple finish stages. |
| `luxir_node_memory_budget_bytes`, `luxir_search_request_memory_limit_bytes` | Configured managed-memory budget and tracked per-search ceiling. Neither is a process RSS limit. |
| `luxir_index_ram_*_bytes` | Indexing reservations, budget, draining reservations, and pending merge demand. The budget is soft; pending demand is capped at that budget when limited. |
| `luxir_storage_ram_bytes`, `luxir_storage_ram_limit_bytes` | RAM storage consumption and its cap, separate from filesystem usage. |
| `luxir_allocator_bytes`, `luxir_allocator_arena_*_bytes` | Jemalloc allocated, active, resident, mapped, retained, dirty, and metadata memory, plus indexing/big-buffer arenas. Resident is an allocator estimate, not process RSS. Absent without jemalloc; check `luxir_allocator_stats_available`. |
| `luxir_collections`, `luxir_collection_available` | Collection availability. An available HTTP endpoint does not imply every collection is usable. |
| `luxir_index_documents`, `luxir_index_segments`, `luxir_index_uncommitted_segments` | Flushed segment document/segment state; excludes documents still buffered in inverters. |
| `luxir_index_committed_bytes`, `luxir_index_last_commit_timestamp_seconds` | Current snapshot descriptor bytes and publication timestamp. Bytes exclude temporary/retained files; an old timestamp alone does not indicate a problem on an idle collection. |
| `luxir_inverters`, `luxir_merge_tasks` | Busy, idle, and flushing inverters; outstanding merge work includes queued tasks. |
| `luxir_filter_cache_*` | Hit/miss counters, builds, admissions, rejections, evictions, purges, bypasses, payload/metadata memory, and payload budget. |
| `luxir_snapshot_pins`, `luxir_snapshot_retained_bytes` | Current pins and retired files held by those pins. Scraping does not expire pins or remove files. |
| `luxir_build_info`, `luxir_node_read_only` | Build version and read-only configuration. |

Counters reset when their owner restarts. Collection cache counters also reset
on recreation or explicit cache-control `reset_counters`; use `rate()` or
`increase()` rather than subtracting raw samples. Histograms use seconds and
cumulative buckets, including `+Inf`; compute fleet percentiles from aggregated
buckets, not averages of node percentiles.

Collection metrics use `tenant` and `collection` labels. There are no labels
for raw URLs, query text, field names, document IDs, commit IDs, or segment IDs.
For installations with many collections, `--no-metrics.collections` disables
per-collection series and their metadata gathering, retaining collection counts
and node/service metrics. Scrapes read in-memory index metadata without listing
index directories, loading readers, or opening segment files.

### Process and system metrics

Linux collectors are enabled by default. `--no-metrics.system` disables them
when equivalent metrics are already collected elsewhere. The counters describe
the kernel views available to the Luxir process:

| Family | Coverage |
| --- | --- |
| `process_*` | CPU time, thread count, resident/virtual/anonymous/file/shared/swap/peak memory, open descriptors and soft descriptor limit, start time, voluntary/involuntary context switches, minor/major page faults, storage bytes, and I/O syscall bytes/counts. Context switches and CPU cover all process threads. |
| `node_cpu_seconds_total`, `node_load*`, `node_procs_*`, `node_forks_total`, `node_context_switches_total`, `node_boot_time_seconds` | CPU modes per logical CPU, load averages, runnable/blocked tasks, task creation, context switches, and boot time. |
| `node_memory_*_bytes`, `node_vmstat_*_total` | Total/available/free memory, page cache, swap, dirty/writeback pages, kernel/slab memory, faults, swap events, and OOM kills where supported. |
| `node_filefd_*` | Host-wide allocated file handles and their limit. |
| `node_network_*_total`, `node_netstat_Tcp_*` | Per-interface bytes/packets/errors/drops and TCP established connections, retransmissions, resets, input errors, and failed attempts. These describe the network namespace, not just Luxir traffic. |
| `node_disk_*` | Every visible block device's read/write bytes, operations, merges, cumulative latency, I/O in progress, busy time, and weighted I/O time. Device and partition counters overlap; do not sum them indiscriminately. |
| `luxir_data_filesystem_*` | The configured filesystem data directory's capacity, free/available bytes, total/free inodes, read-only state, and stat errors. Only emitted for filesystem storage. Arbitrary remote mounts are not probed. |
| `node_pressure_stall_seconds_total` | CPU, memory, and I/O pressure stall time, with `some`/`full` labels where supported. |
| `luxir_cgroup_*` | Current cgroup v2 memory usage/high/max, task count/limit, memory pressure/OOM events, CPU time/quota, and CPU throttling time/periods. Memory includes page cache and descendants. |

In containers, host CPU/memory/disk views can be broader than the container;
network counters follow its network namespace. Cgroup metrics locate this
process's membership using procfs and the cgroup mount. They report **local**
limits; ancestor cgroups can impose stricter limits. A cgroup limit of `-1`
means unlimited. Cgroup v1 is not supported by this collector.

`luxir_metrics_collector_success{collector="..."}` is zero when a source is
unavailable, unsupported, or unreadable. Missing measurements are omitted, not
reported as zero. Some earlier measurements from that collector may still be
present. Restricted procfs, disabled PSI, and absent cgroup controllers are
common reasons. `luxir_metrics_collection_errors` counts collection rows that
could not be read, including deletion races; other rows remain available.
`luxir_metrics_collection_duration_seconds` measures collection overhead.

Useful initial alerts are: scrape failures, unavailable collections, rising
internal/unavailable errors, p99 latency above the service target, sustained
indexing-memory pressure, filesystem byte/inode exhaustion, descriptor usage
near its limit, cgroup OOM events/throttling, and failed background stages.
For example, aggregate HTTP search p99 across nodes with:

```promql
histogram_quantile(0.99,
  sum by (le) (rate(luxir_requests_duration_seconds_bucket{
    transport="http", operation="search"
  }[5m]))
)
```

There are no replication metrics in this exporter. Search admission queues,
deadline-driven partial results, and checksum scrubbing are not implemented on
this revision, so the exporter does not invent metrics for those features.

## Threads and streaming backpressure

HTTP defaults to one connection-I/O shard per available logical CPU, with a
minimum of one, plus one dedicated accept thread. gRPC defaults to half the
available logical CPUs, also with a minimum of one. Automatic sizing respects
the process's CPU affinity, so a server restricted to 28 logical CPUs defaults
to 28 HTTP shards and 14 gRPC threads. Set explicit counts with:

```bash
./luxir --server.http.threads=8 --server.grpc.threads=8
```

`--server.http.threads` sets the number of shards, not the total HTTP thread
count; each shard owns one `io_context` and one runner. Connections are
assigned round-robin at accept and remain pinned to their shard.

Search requests default to serial execution directly on the transport thread
that received them (an HTTP io shard or gRPC completion-queue thread). This
avoids a scheduler handoff and idle-worker wakeups but occupies that
connection's thread for the query's duration. The request-level
`max_parallel` changes that: `1` moves the request onto the shared
work-stealing scheduler, still serial (use this for requests expected to be
expensive, so they do not delay other traffic on the same connection thread);
`-1` keeps the request on its transport thread but lets the scheduler's
workers join in with unlimited intra-request parallelism; values above `1` are
reserved for a bounded parallelism budget and not implemented yet. No setting
runs a request in parallel off its transport thread. Two paths leave the
transport thread whatever the setting: a request with `min_commit` continues on
a shared worker once its commit wait ends, and a gRPC search stream paused by
flow control resumes on one. The current gRPC unary update handler waits for
indexing on its completion-queue thread; streaming update and search use their
separate flow-control paths.

`--server.stream_buffer_bytes` sets the per-HTTP-connection and per-gRPC-call
high-water mark for serialized responses (default 1 MiB). Producers pause above
it and resume after the buffer drains below half. Increase it only when more
per-connection buffering is a measured win; total memory scales with the number
of simultaneously slow streams.

## Memory budgets

`--max-ram-mb` is the node-wide budget (MiB) for memory Luxir manages
explicitly. It defaults to 25% of system RAM - or of the cgroup memory limit
when the process runs under one, so a container is sized by what the kernel
actually enforces rather than by the host's RAM. Subsystem budgets are derived
from it, so raising or lowering this one value moves them together. `0` leaves
the node unlimited. The resolved values are logged at startup.

```bash
./luxir --max-ram-mb=16384          # node budget; indexing gets 8192
./luxir --indexing.max-ram-mb=4096  # override just the indexing share
```

## Indexing memory and request limits

```bash
./luxir \
  --indexing.max-ram-mb=8192 \
  --indexing.max-inverter-ram-mb=2048
```

- `indexing.max-ram-mb` is the shared MiB budget for all of a node's indexing
  work: merges reserve against it before they start, and inverters count their
  RAM against it, so a release that finds the budget over its cap flushes the
  largest idle inverter. It defaults to half of `--max-ram-mb`, and to none at
  all on a `--read-only` node, which never indexes. `0` leaves it unlimited.
- `indexing.max-inverter-ram-mb` flushes one inverter to a segment when it grows
  past that size, checked at the end of an update batch. Total indexing RAM
  is bounded by `indexing.max-ram-mb`, so this cap defaults to it:
  one
  inverter may hold the whole indexing budget (with a single stream there is
  nothing else holding it) but no more. It is clamped to 3814 MiB - an inverter's
  memory pool can address at most 4 GiB, and the clamp leaves headroom for one
  batch's overshoot - so an explicit larger value is clamped with a startup
  warning.
- Lower `indexing.max-inverter-ram-mb` to trade merge work for flush latency and
  peak RSS: smaller flushes land sooner and cost less at once, at the price of
  more segments to merge.
- `indexing.parallel-update-chunk-docs` (default 250) and
  `indexing.parallel-update-max-chunks` (default 8) let one large update
  request use several cores: a request that is not `all_or_none` and has at
  least twice the chunk size is split into up to that many chunks, each indexed
  into its own inverter in parallel. Every copy of an id stays in one chunk, so
  results are the same as indexing the request whole. Each chunk flushes to its
  own segment, so splitting adds merge work; `0` chunk docs turns it off.

`--indexing.huge-pages` is enabled by default in jemalloc builds. It advises
transparent huge pages for whole-huge-page indexing allocations; disable it
with `--no-indexing.huge-pages` (or `--indexing.huge-pages=false`). This is a
best-effort hint: a host with THP set to `never` still works using ordinary pages.

The HTTP request limits distinguish bounded material from streams:

```bash
./luxir \
  --indexing.max-request-body=32MB \
  --indexing.max-record=32MB \
  --indexing.stream-batch-size=1MB \
  --indexing.stream-batch-docs=10000 \
  --indexing.max-inflight-batches=0
```

- `max-request-body` caps buffered JSON requests and atomic NDJSON groups.
- `max-record` caps one streamed NDJSON record so a missing newline cannot grow
  memory without bound. It inherits `max-request-body` when unset.
- `stream-batch-size` and `stream-batch-docs` are internal handoff thresholds,
  not limits on the number of documents in one HTTP stream.
- `max-inflight-batches` bounds the number of internal batches one connection
  can submit concurrently. `0` (the default) uses task-arena concurrency plus
  two; `1` makes batch submission serial. Reads pause when the limit is
  reached and resume as batches complete. A larger limit uses more staging
  memory.

gRPC has one request limit, on the size of each received message (a unary
request or one message of a stream):

```bash
./luxir --server.grpc.max-receive-message-size=64MB
```

It defaults to `--indexing.max-request-body`, so the buffered HTTP and gRPC
ceilings move together. A larger message fails with `RESOURCE_EXHAUSTED`
before it is decoded; see [gRPC API](grpc.md#message-size). Replies have no
size limit; search results are streamed in bounded batches.

Search has its own limits:

```bash
./luxir \
  --search.request-memory-max-bytes=0 \
  --search.max-op-depth=8 \
  --search.default-timeout-ms=10000 \
  --search.max-concurrent=32 \
  --search.max-queued=1000 \
  --query-cache-bytes=64MB
```

- `search.request-memory-max-bytes` is a per-request breaker for query memory
  such as facet aggregate state; `0` (the default) is unlimited.
- `search.max-op-depth` bounds how deeply operations may nest in one request.
- `search.default-timeout-ms` is the node's search deadline; `0` (the default)
  is none. A request's `timeout_ms` can shorten it but not extend it. A search
  out of time returns partial results flagged `timed_out`; see
  [Timeouts and partial results](searching.md#timeouts-and-partial-results).
- `search.max-concurrent` caps the searches admitted to run at once; `0` (the
  default) is unlimited. Further searches wait in a FIFO queue of up to
  `search.max-queued` (default `1000`) and start as slots free; beyond that a
  search is rejected with kind `resource_exhausted`, code `search_queue_full`
  (HTTP 429). A search holds its slot from arrival, including any `min_commit`
  wait, until its execution finishes; a streaming response paused by a slow
  reader does not hold one. Queue time counts against the search's deadline.
- `query-cache-bytes` caps each collection's filter cache (default `64MB`; `0`
  disables it). Cached filters are evicted by rebuild cost per byte, so under
  pressure the cache keeps expensive compound filters and drops cheap ones
  first. Cache counters appear in `_stats`.

## Time-zone data

IANA time-zone names require the system zoneinfo database and compatible C++
runtime data. Without it, date queries retain UTC and fixed-offset support but
named zones are unavailable. Keep the zoneinfo data and C++ runtime consistent
across machines if requests must produce identical civil-time boundaries.

## Shutdown and recovery

On `SIGTERM` or `SIGINT` the server stops accepting connections, waits for
in-flight gRPC and HTTP requests to finish, stops background work (scrubbing,
replication), releases the data directory and exits with status `0`. Updates
that were not yet committed are not committed by shutdown: only acknowledged
commits are durable, so before a planned stop quiesce producers and publish an
immediate commit. A second signal during the drain exits immediately with
status `128 + signal`. After an unplanned stop, the filesystem backend
reopens the last durable commit. If recovery selects an older valid root, a writer
publishes it under a new incarnation so followers cannot confuse it with an
older generation of their current source. File retirement does not fsync removals:
reappearing obsolete files are harmless and a later sweep reclaims them.

At startup, a collection whose top-level index metadata cannot be parsed is
kept as a tombstone rather than preventing healthy collections from loading.
Queries and updates to that collection return the recorded load error as an
`unavailable` error (HTTP `503`, code `collection_unavailable`) and the server
will not silently recreate storage over it. Other segment corruption is
detected when the affected reader is opened, or earlier with the
[file integrity checks](#file-integrity-checks) below.

## File integrity checks

Every committed manifest records each file it references with its size and
xxh3-64 digest, computed while the file is written. Three checks use them:

- **Transfers and copies** always verify. A replicated file is hashed while it
  streams in and is installed only if it matches; a local file reused across
  incarnations (a repointed follower, `luxir promote`, or writer recovery from
  an older root) is hashed when it is linked. A follower fetches a mismatching
  file again; promotion fails for that collection and names the file.
- **`--store.verify-on-open=full`** reads every file of each collection's
  committed snapshot at startup, on up to 8 threads per collection, before the
  collection serves. The default, `none`, trusts the manifest footer and file
  sizes. A mismatching, missing or unreadable file fails only that collection:
  it loads as unavailable (HTTP `503`, code `collection_unavailable`) with an
  error naming the file and its expected and actual size and digest, and its
  data is kept. A follower then fetches the damaged files from its source.
  Startup reads the whole index, so expect it to take as long as reading the
  data directory once.
- **`--store.scrub-interval-s=N`** (default `0`, off) re-reads the files of
  every published snapshot every `N` seconds on one low-priority thread,
  paced to `--store.scrub-max-bytes-per-sec` (default `64MB`, `0` for
  unthrottled). A mismatch is logged at error level and makes the collection
  unavailable with the same error until the node restarts; repair it by
  restoring from a follower (`luxir pull` and `luxir promote`), or on a
  follower by restarting with `--store.verify-on-open=full`. `GET /_stats`
  reports node-wide `scrub` counters (`passes`, `files`, `bytes`,
  `mismatches`, `last_pass_end` in Unix milliseconds) while scrubbing is
  enabled.

## Current production boundary

Before treating Luxir as a production service, account explicitly for the
features it does not yet supply:

- no automatic failover, leader election, or distributed query execution;
- no built-in TLS/authentication/authorization;
- no scheduled backups or automatic restore (use `luxir pull` and `luxir promote`);
- pre-1.0 wire and schema interfaces that may change.

None of these can be enabled by configuration. The pieces those layers will
build on are already in place: immutable commits, isolated collections,
transport-independent request messages, and a node-wide scheduler and memory
model.
