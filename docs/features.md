# Features by transport and Riak version

Riak KV has changed between releases: Riak Search (Yokozuna) and the
coverage service are gone from Riak 3.x, while the AAE folds, the
replication queues and the Query API arrived with Riak 2.9, 3.0 and 3.4.
Every call exists on both transports. A call that the transport or the
node cannot serve returns `RIAK_E_UNSUPPORTED`, with the reason in
`riak_client_errmsg()`, and the connection stays usable.

The table lists what was verified against Riak KV 2.2.3, 3.2.6 and 3.4.1
(see [Testing](testing.md)).

| Feature | Protocol Buffers | HTTP | 2.2.3 | 3.2.6 | 3.4.1 |
|---|---|---|---|---|---|
| Objects, siblings, conditional requests, bucket types | yes | yes | yes | yes | yes |
| `riak_opts::node_confirms`, `sync_on_write` | yes | yes | accepted, not enforced | yes | yes |
| `riak_opts::return_head`, `deletedvclock` | yes | `RIAK_E_UNSUPPORTED` | yes | yes | yes |
| `riak_opts::asis` | yes | yes | yes | yes | yes |
| `riak_opts::node_confirms` on a delete | `RIAK_E_UNSUPPORTED` | yes | – | yes | yes |
| Bucket and bucket-type properties, `riak_set_type_props()` | yes | yes | yes | yes | yes |
| `riak_get_client_id()` | yes | `RIAK_E_UNSUPPORTED` | yes | yes | yes |
| Key and bucket listings, server information | yes | yes | yes | yes | yes |
| `riak_get_preflist()` | yes | `RIAK_E_UNSUPPORTED` | yes | yes | yes |
| Secondary indexes, `pagination_sort` | yes | yes | yes | yes | yes |
| Coverage plans (`riak_get_coverage()`) | yes | `RIAK_E_UNSUPPORTED` | no | no | no |
| Riak Search and its administration | yes | yes | yes | no | no |
| MapReduce (Erlang phases) | yes | yes | yes | yes | yes |
| Data types: counter, set, map, HyperLogLog | yes | yes | yes | yes | yes |
| Data type: grow-only set | yes | yes | no | yes | yes |
| Legacy counters (`riak_counter_update()`) | yes | yes | yes | yes | yes |
| Object folds (`riak_fold_objects()`) | yes | `RIAK_E_UNSUPPORTED` | yes | yes | yes |
| Link walking (`riak_link_walk()`) | yes | yes | yes | yes | yes |
| Query API (`riak_query_run()`) | through `http_port` | yes | no | no | yes |
| AAE folds (`riak_aae_*`) | yes | yes | no | yes | yes |
| Replication queues (`riak_repl_fetch()`, `riak_repl_push()`) | yes | fetch only | no | yes | yes |
| `riak_cluster_membership()` | yes | yes | no | yes | yes |
| `riak_get_cluster_id()`, `riak_repl_get()` | yes | `RIAK_E_UNSUPPORTED` | no | yes | yes |
| Statistics (`riak_stats()`) | through `http_port` | yes | yes | yes | yes |
| TLS and authentication | STARTTLS | HTTPS | with `tls_legacy` | yes | yes |

Notes:

- "no" means the node does not implement the request: the call returns
  `RIAK_E_UNSUPPORTED`. Riak 2.2 answers newer Protocol Buffers requests
  with "Unknown message code" and newer HTTP resources with an HTML 404;
  both are reported as unsupported, never as a missing object.
- Preference lists are served over Protocol Buffers only. Riak 2.x's HTTP
  resource ignores the key and answers the same list for every key, which
  is why Riak 3.0 removed it.
- Link walking is refused by Riak while Riak security is enabled, on both
  transports (`RIAK_E_UNSUPPORTED`).
- The AAE folds need tictac AAE (`tictacaae_active = active`); the
  replication queue calls need a source queue (`replrtq_enablesrc`,
  `replrtq_srcqueue`). `riak_get_cluster_id()` and `riak_repl_get()` need
  the `riak_repl` application, which OpenRiak 3.x ships.
- Known server issues found while testing are listed in
  [Operations](operations.md#known-server-issues).

Not covered, by design: Riak TS (a separate product with its own protocol),
`RpbRTEReq` (defined in `riak_kv.proto` but registered by no server), and
cluster or security administration, which Riak offers only through
`riak admin` / `riak-admin`.
