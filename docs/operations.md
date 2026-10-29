# Operations: AAE folds and replication

These calls serve repair, auditing and replication tooling. They need Riak
2.9 or newer (Riak 3.x for the queue calls) and the node configuration
named in each section; on older nodes they return `RIAK_E_UNSUPPORTED`.
See [Features](features.md) for the full matrix.

## AAE folds

With tictac AAE enabled (`tictacaae_active = active` in `riak.conf`), a
node answers folds over its AAE store without reading objects:

| Call | Result |
|---|---|
| `riak_aae_merge_root()` | The merged root of the cached trees of an n_val |
| `riak_aae_merge_branches()` | Selected branches of those trees |
| `riak_aae_fetch_clocks_nval()` | Keys and vector clocks in selected segments |
| `riak_aae_merge_range()` | A tree built over a bucket and key range |
| `riak_aae_fetch_clocks_range()` | Keys and vector clocks in a range |
| `riak_aae_find_keys()` | Keys with many siblings or large objects |
| `riak_aae_object_stats()` | Object count, total size, size and sibling histograms |
| `riak_aae_find_tombs()` | Tombstones in a range |
| `riak_aae_reap_tombs()`, `riak_aae_erase_keys()` | Reap tombstones or erase keys (or only count them) |
| `riak_aae_repair_keys()` | Read repair of the keys in a range |
| `riak_aae_repl_keys()` | Queue the keys of a range for replication |
| `riak_aae_list_buckets()` | Buckets of an n_val |

Range folds take a `riak_aae_filter`: a bucket (with optional bucket
type), an optional key range, an optional segment filter, an optional
last-modified range and an optional hash initialisation vector:

```c
uint32_t segs[] = {12, 17};
riak_aae_filter f = {.bucket = "users",
                     .start_key = "a", .end_key = "m",
                     .segments = segs, .n_segments = 2,
                     .segment_tree_size = RIAK_AAE_SMALL,
                     .modified_start = 1700000000, .modified_end = 1800000000};
riak_aae_keys keys;
if (riak_aae_fetch_clocks_range(c, &f, &keys) == RIAK_OK) {
    for (size_t i = 0; i < keys.count; i++) {
        printf("%s %s\n", keys.items[i].key, keys.items[i].vclock);
    }
    riak_aae_keys_free(&keys);
}
```

Tree branches arrive compressed and are inflated by the library; the
inflated size is bounded by `riak_config::max_body_bytes`.

Reaping and erasing take a `riak_aae_change`: `RIAK_AAE_COUNT` only counts
(the request is read-only and may be retried), `RIAK_AAE_LOCAL` changes the
data on the receiving node, and `RIAK_AAE_JOB` hands the work to a job with
the given id. Changes, repairs and replication requests are sent once:
when the answer is lost, the error is reported instead of repeating a
request that may have been applied.

## Replication queues (nextgenrepl)

A node with a source queue (`replrtq_enablesrc = enabled`,
`replrtq_srcqueue = <name>:any`) records every change in it:

```c
riak_repl_fetch_result r;
if (riak_repl_fetch(c, "libriak_q", &r) == RIAK_OK) {
    if (!r.queue_empty) {
        /* r.object: the change in Riak's replication encoding;
         * r.deleted and r.deleted_vclock describe tombstones */
    }
    riak_repl_fetch_result_free(&r);
}
```

A fetch takes the entry off the queue, so it is never retried.
`riak_repl_push()` (Protocol Buffers) adds keys with their vector clocks to
a queue and reports the queue lengths. `riak_cluster_membership()` lists the
nodes that serve the replication protocol, with their address and port.

## riak_repl

`riak_get_cluster_id()` returns the cluster id of the node, and
`riak_repl_get()` reads an object through a replication proxy from another
cluster (Protocol Buffers; both need the `riak_repl` application).

## Known server issues

The following Riak issues were found while testing the library. The library
reports them as errors and does not hide them:

- Riak 3.4.1, Query API paging: with `max_results` lower than the number of
  matches, the node fails internally and answers HTTP 503. The query
  succeeds without `max_results`.
- Riak 3.4.1, grow-only set update with `return_body` over Protocol
  Buffers: the node crashes the request handler and closes the connection.
  The library sends such an update without `return_body` and fetches the
  value afterwards.
- Riak 3.4.1, `RpbMembershipReq` with a body: the request handler crashes;
  the library sends the request without one.
- A crashed request handler makes Riak send "Error processing incoming
  message" and close the connection; the library reports `RIAK_E_SERVER`
  and opens a new connection for the next request.
