# riakctl command-line client

`riakctl` is a command-line client for Riak KV built on libriak-c. It
reaches nodes over the network, through Protocol Buffers or HTTP, with TLS
and Riak security, and makes their data and operational interfaces
available to shell scripts: objects, listings, secondary indexes, the Query
API, MapReduce, data types, bucket properties, Riak Search, AAE folds,
nextgenrepl queues, statistics, backups and restores. It works with Riak KV
2.0 to 3.4 (OpenRiak).

Riak's own `riak admin` runs on a node and changes the node or the cluster
(membership, bucket type creation, security users and grants, transfers);
Riak offers no remote interface for those operations, so `riakctl` does not
cover them. Riak Control (the OpenRiak web console for deployment, staged
cluster changes and configuration) is a web interface without data access
or a scripting interface. `riakctl` complements both: remote, scriptable,
with JSON output and stable exit codes, for data, AAE and replication work.

## Installing

`riakctl` is built with the library (CMake option `RIAK_BUILD_TOOLS`, on by
default for top-level builds) and installed with its man page,
`riakctl(1)`:

```sh
cmake -S . -B build && cmake --build build
build/tools/riakctl/riakctl --version
sudo cmake --install build             # /usr/local/bin/riakctl
```

The binary packages ship it as `libriak-c-tools` (Debian/Ubuntu and
Fedora/EL), see [Packaging](packaging.md). The demo scripts need `jq`.

## Connecting

```sh
riakctl ping                                            # pb://127.0.0.1:8087
riakctl -u http://riak1:8098 info
riakctl -u pb://riak1:8087,pb://riak2:8087,pb://riak3:8087 ping
riakctl -u 'pb://[2001:db8::7]:8087' ping
```

`--url` takes `scheme://host[:port]` entries separated by commas; every
entry of a list uses the same scheme. A list is a cluster: requests are
balanced over the nodes and retried on the next node after a failure, see
[Clusters](clusters.md).

| Scheme | Transport | Default port |
|---|---|---|
| `pb://` | Protocol Buffers | 8087 |
| `pbs://` | Protocol Buffers with STARTTLS | 8087 |
| `http://` | HTTP | 8098 |
| `https://` | HTTPS | 8098 |

Global options come before the command; each has an environment variable,
and options take precedence over the environment:

| Option | Environment | Meaning |
|---|---|---|
| `-u`, `--url URLS` | `RIAK_URL` | Nodes (default `pb://127.0.0.1:8087`) |
| `--http-port PORT` | `RIAK_HTTP_PORT` | HTTP port for statistics and the Query API over Protocol Buffers (default 8098 for `pb://`) |
| `--user NAME` | `RIAK_USER` | Riak security user; needs `pbs://` or `https://` |
| `--password-file FILE` | `RIAK_PASSWORD` (the password itself) | Password; the first line of the file |
| `--ca FILE` | `RIAK_CA` | CA bundle for certificate verification |
| `--cert FILE`, `--key FILE` | `RIAK_CERT`, `RIAK_KEY` | Client certificate and key |
| `--tls-legacy` | `RIAK_TLS_LEGACY=1` | Interoperability with the TLS stack of Riak 2.x, see [Security](security.md) |
| `--insecure` | | The server certificate is not verified |
| `--timeout MS`, `--connect-timeout MS` | | Request timeout (default none) and connection timeout (default 5000) |
| `--retries N` | | Additional attempts after a failure (default 2) |
| `-t`, `--type TYPE` | | Bucket type for the commands that take one (also accepted after the command) |
| `--pretty`, `--compact` | | JSON layout; indented by default on a terminal |
| `-v`, `--verbose` | | Client log on stderr (every request) |

Passwords are not accepted as options, so they do not show up in process
listings or shell history:

```sh
export RIAK_URL=pbs://riak1:8087 RIAK_USER=ops RIAK_CA=/etc/riak/ca.pem
riakctl --password-file ~/.config/riak-ops.pass info
```

## Output and exit codes

- A single result is one JSON document on stdout.
- Listings, folds and streams are JSON Lines: one JSON value per line,
  written as results arrive, in constant memory. `jq -s` turns them into
  an array.
- `get` writes the value as stored (bytes), unless `--json` asks for the
  object with its metadata. Values in JSON are text when they are valid
  UTF-8 without NUL bytes (`"value"`) and base64 otherwise (`"value_b64"`).
- Errors are written to stderr as `riakctl: <what>: <status> (<detail>)`.

| Exit code | Meaning |
|---|---|
| 0 | Success |
| 1 | Other failure (server error, malformed response, failed restore entries, write error) |
| 2 | Usage error or invalid configuration |
| 3 | Not found |
| 4 | The object has siblings (`get --all` shows them) |
| 5 | Precondition failed (`put --create` on an existing key) |
| 6 | Authentication or permission failure |
| 7 | No node reachable (connection, DNS, timeout or TLS failure) |
| 8 | Not supported by the node's Riak version or by the transport |

The codes are stable; scripts can branch on them:

```sh
if ! riakctl get users alice >/dev/null 2>&1; then
    case $? in
    3) echo "no such user" ;;
    7) echo "cluster unreachable" ;;
    *) echo "failed" ;;
    esac
fi
```

## Commands

`riakctl help` lists the commands and `riakctl help COMMAND` (or
`COMMAND --help`) describes one.

### Nodes

```sh
riakctl ping                       # {"nodes":[{"host","port","up","ms"}],"up":3,"total":3}
riakctl ping --all                 # exit code 7 unless every node answers
riakctl info                       # node name, Riak version, client node health, members
riakctl stats                      # the whole /stats document
riakctl stats node_gets 'node_get_fsm_time_*' ring_members
```

`ping` pings every configured node on its own, without retries, and fails
(exit code 7) when none answers. `stats` filters by exact names or by
prefixes ending in `*`; over Protocol Buffers statistics are fetched from
the HTTP port (`--http-port`).

### Objects

```sh
echo '{"name":"alice"}' | riakctl put -c application/json \
    -m owner=ops -i email_bin=alice@example.com -i age_int=31 users alice
riakctl put --value hello greetings                  # {"key":"<generated>"}
riakctl put -f photo.jpg -c image/jpeg photos p1
riakctl put --create users alice </dev/null           # exit code 5 when it exists
riakctl get users alice                               # the value
riakctl get --json users alice                        # value, content type, vclock, meta, indexes, links
riakctl get --all -t multi orders o-17                # every sibling
riakctl get --head users alice                        # metadata only
riakctl delete users alice
riakctl list buckets
riakctl list --plain keys users | while read -r k; do ...; done
```

`put` reads the value from stdin unless `--value` or `--file` is given.
Repeatable options: `--meta NAME=VALUE`, `--index NAME=VALUE` (the index
name carries its `_bin` or `_int` suffix) and `--link VALUE` (a raw Link
header value). `--vclock` passes the vector clock of the version being
replaced, `--return-body` prints the stored object, and `--w`, `--dw`,
`--pw` (put), `--r`, `--pr` (get) and `--rw` (delete) set quorums as
`one`, `quorum`, `all`, `default` or a replica count.

Key and bucket listings visit every key in the cluster and are expensive;
secondary indexes, AAE folds and object folds are cheaper ways to
enumerate a bucket.

### Secondary indexes and the Query API

```sh
riakctl index users email_bin alice@example.com          # exact match
riakctl index users age_int 30 39 --terms                # {"key","term"} per line
riakctl index users age_int 30 39 --max 100              # one page with a continuation
riakctl index users age_int 30 39 --max 100 --continuation g2gCYQJt...
riakctl index users '$bucket' users                       # every key, from the index
```

`index` streams all matches unless `--max` selects paging; a page is
printed as `{"results": [...], "continuation": ...}`, and the continuation
is passed back with `--continuation` until it is `null`.
`secondary-indexes/query.sh` in the demos pages through a range this way.

The Query API of Riak 3.4 runs one index query given with flags, or any
request given as a JSON document:

```sh
riakctl query people people_bin --start 0 --end 2 \
    --eval 'delim($term, "|", ($dob, $name))' --filter '$name = :n' --sub n=alice
riakctl query people people_bin --start 0 --end 2 --accumulate count
riakctl query people people_bin --start 0 --end z --accumulate queue_raw_keys
riakctl query --request q.json people
```

A request document has the members `parts` (an array of
`{"index", "start", "end", "eval", "filter", "regex", "tag"}`),
`aggregation` (e.g. `"$1 INTERSECT $2"`), `accumulate`, `term`,
`substitutions` (an object), `max_results`, `continuation` and `timeout`;
command-line flags override the file. The response is printed as Riak
returns it. With a queue accumulation (`queue_raw_keys`,
`queue_raw_terms`) the queue is fetched to the end and the results are
printed as JSON Lines.

### MapReduce

```sh
riakctl mapred job.json                 # {"phase": N, "data": ...} per line
riakctl mapred --collect < job.json     # the complete result
```

MapReduce is not available on OpenRiak 4.x and is refused by nodes with
Riak security enabled unless permitted.

### Data types

Data types live in bucket types created with a `datatype` property; the
type is required:

```sh
riakctl -t counters dt incr pages home          # +1
riakctl -t counters dt decr pages home 5
riakctl -t sets dt add tags post-1 riak c cli
riakctl -t sets dt remove tags post-1 cli       # the context is fetched first
riakctl -t maps dt get profiles alice           # {"datatype","value","context"}
echo '{"name_register":"Alice","visits_counter":1,"tags_set":{"add":["admin"]},
       "address_map":{"city_register":"Yerevan"},"remove":["old_flag"]}' |
    riakctl -t maps dt update profiles alice --return-body
```

Map values and map update documents name their fields `<field>_<type>`, as
Riak's HTTP API does: counters take a number (added), registers a string,
flags a boolean, sets `{"add": [...], "remove": [...]}` (or an array of
elements to add) and nested maps a map document. `"remove"` lists fields
to remove. Updates without a key generate one. Removals need the causal
context of a fetch; it is fetched automatically unless `--context` gives
one.

Riak 1.4 counters (in `allow_mult` buckets of the default type) are
handled by `riakctl counter get|incr|decr BUCKET KEY [N]`.

### Bucket and bucket type properties

```sh
riakctl props get users
riakctl props set users '{"n_val": 5, "allow_mult": true}'
echo '{"props":{"search_index":"users_idx"}}' | riakctl props set users
riakctl props reset users
riakctl props type-get maps
riakctl props type-set multi '{"r": "quorum"}'
```

Properties are accepted as `{"props": {...}}` or as the bare object.
Bucket types themselves are created and activated with `riak admin
bucket-type create|activate` on a node.

### Coverage plans and preference lists

```sh
riakctl coverage users            # Protocol Buffers, nodes that serve coverage requests
riakctl preflist users alice      # vnodes holding the replicas (Protocol Buffers)
```

Riak 3.x no longer answers coverage requests (exit code 8).

### Riak Search (Riak 2.x)

```sh
riakctl search index-create users_idx [--schema my_schema] [--n-val 3]
riakctl search index-list
riakctl search index-get users_idx
riakctl search query users_idx 'name_s:ali*' --sort 'age_i desc' --rows 20
riakctl search query users_idx '*:*' --raw     # the Solr response as returned
riakctl search schema-put my_schema schema.xml
riakctl search schema-get my_schema
riakctl search index-delete users_idx
```

A query prints `{"num_found", "max_score", "docs": [{"type", "bucket",
"key", "score", "fields": {...}}]}`; multi-valued fields become arrays.
Index creation completes asynchronously on the server. Riak Search was
removed in Riak 3.0, where these commands end with exit code 8.

### AAE folds

The AAE folds of Riak 2.9 and later (tictac AAE enabled) inspect and
change a bucket without reading its objects. Folds over the cached trees of
an n_val:

```sh
riakctl aae root 3                    # level-one hashes of the n_val 3 tree
riakctl aae branches 3 0 17 42        # level-two blocks
riakctl aae clocks 3 1024 1025        # keys and clocks of segments (JSON Lines)
riakctl aae buckets 3                 # buckets with objects of n_val 3
```

Folds over a bucket range accept a filter: `--start-key`/`--end-key`,
`--segments A,B,...` with `--tree-size`, `--modified-start`/`--modified-end`
(Unix seconds), and `-t TYPE`.

```sh
riakctl aae stats users                        # object count, size and sibling histograms
riakctl aae keys users                         # {"type","bucket","key","vclock"} per line
riakctl aae find users --siblings 5            # keys with more than 5 siblings
riakctl aae find users --object-size 1048576   # keys larger than 1 MiB
riakctl aae tombs users                        # tombstones and their clocks
riakctl aae tree users --size small            # range tree
riakctl aae repair users                       # prompts read repair of the range
riakctl aae repl users q1                      # queues the range for nextgenrepl
riakctl aae reap users                         # counts tombstones; changes nothing
riakctl aae reap --local users                 # tombstones go to each node's reaper
riakctl aae erase --job 7 --start-key a --end-key b users
```

`reap` and `erase` only count unless `--local` (the nodes' own queues) or
`--job ID` (a dedicated worker) is given, so a mistyped command does not
delete data.

`aae compare` compares a bucket on two clusters (`--peer URL`, with the
same credentials) or two buckets (`--peer-bucket NAME`), as nextgenrepl's
full sync does: the range trees are compared first, and only the keys of
differing segments are fetched with their clocks. Each difference is a
line `{"key", "status", "vclock", "peer_vclock"}` with the status
`differs`, `missing_on_peer` or `missing_locally`, and a summary goes to
stderr:

```sh
riakctl -u pb://dc1-riak:8087 aae compare orders --peer pb://dc2-riak:8087
riakctl aae compare orders --peer-bucket orders_migrated --size medium
```

`--size` (default `small`, 65536 segments) trades tree size against the
number of keys fetched per differing segment.

### Replication

```sh
riakctl repl fetch q1 --max 100       # takes changes off a nextgenrepl queue
riakctl aae keys users | riakctl repl push q1   # queues every key of a bucket
riakctl repl members                  # cluster members, as sinks see them (Riak 3.0+)
riakctl repl cluster-id               # riak_repl cluster id
riakctl repl get CLUSTER_ID users alice   # get proxied by riak_repl
```

`repl fetch` prints one line per change: `{"deleted", "object_b64", "crc",
"segment_id", "segment_hash"}`, the object in Riak's internal encoding.
Fetching removes the change from the queue. `repl push` reads lines with
`bucket`, `key`, `vclock` and optionally `type`, which is the output of
`aae keys` and `aae tombs`; it needs the Protocol Buffers transport. The
queue lengths after the last push are printed.

## Backups and restores

```sh
riakctl backup -o users.jsonl users
riakctl -t maps backup profiles > profiles.jsonl
riakctl restore --dry-run users.jsonl
riakctl restore users.jsonl
riakctl restore --bucket users_copy users.jsonl
```

### Format

A backup is a JSON Lines file:

```
{"format":"riakctl-backup","version":1,"bucket":"users","type":"multi","created":"2026-11-02T17:54:56Z","riak_version":"3.4.0"}
{"key":"alice","vclock":"a85hYGBg...","siblings":[{"value":"...","content_type":"application/json","last_modified":"...","meta":{"owner":"ops"},"indexes":{"email_bin":["alice@example.com"]},"links":"..."}]}
{"key":"photo","vclock":"...","siblings":[{"value_b64":"/9j/4AAQ...","content_type":"image/jpeg"}]}
{"key":"profile","datatype":"map","value":{"name_register":"Alice","visits_counter":3}}
{"end":true,"count":3}
```

Every object keeps its vector clock and every sibling with its value,
content type, user metadata, secondary indexes, links and last-modified
time. Data type buckets are saved as values (the bucket's `datatype`
property is looked up). The trailer is written only after the last
entry; a file without it is incomplete, and `restore` reports it with
exit code 1. Tombstones are not saved.

### Enumeration

`--method` selects how the keys are found; by default the first available
one is used:

| Method | Mechanism | Needs |
|---|---|---|
| `fold` | Object fold (`RpbCSBucketReq`): objects with all siblings, in key order, in pages of 1000 | Protocol Buffers, a backend with secondary indexes (leveldb, leveled, memory) |
| `aae` | AAE key and clock fold, then one fetch per key | Riak 2.9+ with tictac AAE |
| `keys` | Key listing, then one fetch per key on a second connection | Any node (expensive on large clusters) |

Data type buckets use `aae` or `keys`. A backup reads a live bucket; it is
not a point-in-time snapshot, and writes made while it runs may or may not
be included.

### Restore semantics

- A restore is idempotent. Each key is read first; a key that already
  holds every sibling of the backup (same value, content type, metadata,
  indexes and links) is counted as `unchanged` and not written. Running a
  restore twice writes nothing the second time.
- A single sibling is written "as is" with the backed-up vector clock, as
  replication does: on a key that does not exist the restored object has
  exactly the original clock. When the key holds a newer version (its
  clock descends the backup's), Riak keeps the newer version; concurrent
  versions become siblings in `allow_mult` buckets.
- A key deleted shortly before still has a tombstone whose clock descends
  the backup's, which would make Riak drop the "as is" write. The write is
  therefore checked, and the object is written again as an update of the
  tombstone; its clock then includes the deletion.
- Several siblings are written one by one, without vector clocks, so they
  become siblings again; only siblings missing on the key are written. Their
  clocks are new (Riak takes one value per write). In a bucket without
  `allow_mult` the last write wins and one sibling remains.
- Counters are restored by adding the difference to the stored value, sets
  and grow-only sets by adding missing elements (elements added since the
  backup stay), maps field by field. HyperLogLogs cannot be restored: only
  their estimate is readable; they are reported as `skipped`.
- `--bucket` and `--type` restore into another bucket or bucket type.
- The result is printed as `{"bucket", "type", "restored", "unchanged",
  "skipped", "failed", "complete"}`; the exit code is 1 when an entry failed
  or the trailer is missing, and 7 when no node answers any more.

### Limitations

- Over HTTP the vector clock of a tombstone cannot be read, so after a
  restore over a recently deleted key of an `allow_mult` bucket the
  tombstone can remain as an additional (deleted) sibling. Protocol
  Buffers has no such limitation.
- Keys and bucket names are C strings in libriak-c: keys containing a NUL
  byte cannot be handled; the object fold starts at the key `"\x01"`.
- Bucket properties and bucket types are not part of a backup; `riakctl
  props get` saves the properties.

## Demo workflows

`examples/cli/` holds scripts that combine commands into complete tasks.
They use `riakctl` from `PATH` (or `RIAKCTL`) and the connection
environment variables, and follow `set -eu`, `jq` and exit codes:

| Script | Workflow |
|---|---|
| `basic/connectivity.sh` | Pings every node, prints name and version, demonstrates the exit codes of a missing key and an unreachable node |
| `basic/backup.sh BUCKET [FILE [TYPE]]` | Backup into a partial file, checked (trailer, entry count) before it is renamed |
| `basic/restore.sh FILE [BUCKET [TYPE]]` | Dry run, then restore; fails on failed entries |
| `automation/health-check.sh [WARN_US [CRIT_US]]` | Nagios/Icinga check: node reachability and 95th percentile GET/PUT latency, with performance data |
| `automation/cluster-stats.sh [INTERVAL [SAMPLES]]` | Selected statistics sampled as CSV (`STATS` selects them) |
| `secondary-indexes/query.sh BUCKET INDEX MIN MAX [PAGE]` | Paged index range query with continuations, each match fetched |
| `operations/aae-inspection.sh [--reap] BUCKET [TYPE]` | Object count and size, sibling and size outliers, tombstones, optional reaping |
| `operations/replication-queues.sh BUCKET QUEUE [TYPE]` | AAE keys pushed onto a nextgenrepl queue, the queue drained and counted |

`run-all.sh` chains them on a fresh bucket and checks every result:
connectivity, seeding (objects with metadata, indexes and a binary value,
siblings, a map), backup, wiping the bucket, restoring, comparing with the
backup, restoring again (nothing written), the paged query, monitoring,
AAE inspection and the replication round trip. Optional parts are detected
from the node (`RIAK_FEATURES`, comma separated `2i`, `aae`,
`nextgenrepl`, skips the detection), and the bucket types come from
`tests/docker/setup-node.sh` (`RIAK_MULTI_TYPE`, `RIAK_MAP_TYPE`,
`RIAK_REPL_QUEUE` name others).

`run-demo.sh VERSION [pb|http]` does everything with one command: it starts
a container from the test image `libriak-riak:VERSION` (built as in the CI
workflow), prepares it, runs `run-all.sh` and removes the container:

```sh
examples/cli/run-demo.sh 3.4.1
examples/cli/run-demo.sh 2.2.3 http
```

## Compatibility

| Command | Needs |
|---|---|
| `ping`, `info`, `get`, `put`, `delete`, `list`, `props`, `index`, `mapred`, `dt`, `counter`, `backup`, `restore` | Riak 2.0+ |
| `stats` | The HTTP port (over Protocol Buffers: `--http-port`) |
| `coverage`, `preflist` | Protocol Buffers; coverage: Riak 2.x |
| `query` | Riak 3.4+ |
| `search` | Riak 2.x with Search enabled |
| `aae`, `repl fetch`, `repl push` | Riak 2.9+ (tictac AAE, nextgenrepl queues); `repl push`: Protocol Buffers |
| `repl members` | Riak 3.0+ |
| `info` members | Riak 3.0+, omitted elsewhere |

[Features](features.md) lists the library calls behind the commands by
transport and Riak version.

## Testing

`ctest` runs `riakctl_cli` against the stub server (options, exit codes,
JSON output, backup and restore) and, with `RIAK_TEST_URL` set,
`riakctl_live`, which runs `examples/cli/run-all.sh` against the live
node. The CI live jobs run both on every Riak version, transport and
security setting; see [Testing](testing.md).
