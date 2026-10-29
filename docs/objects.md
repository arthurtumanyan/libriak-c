# Objects and buckets

## Reading, writing, deleting

```c
riak_opts o = {.content_type = "application/json", .w = RIAK_Q_QUORUM};
riak_put(c, "users", "u1", json, strlen(json), &o, NULL, NULL);

riak_object obj;
if (riak_get(c, "users", "u1", NULL, &obj) == RIAK_OK) {
    /* obj.data, obj.len, obj.content_type, obj.vclock, obj.etag ... */
    riak_object_free(&obj);
}

riak_delete(c, "users", "u1", NULL);
```

- With a `NULL` key, `riak_put()` lets Riak generate one; it is returned
  through `created_key` and released with `riak_free()`.
- With `return_body` set, the stored object is returned through `out`.
- Updates of an existing object pass the `vclock` of the previous read in
  `riak_opts::vclock`; otherwise siblings may be created in buckets with
  `allow_mult`.
- `server_timeout_ms` sets Riak's own request timeout.
- `head` returns the metadata without the value; `basic_quorum` and
  `notfound_ok` tune how not-found answers count towards the read quorum.
- Over Protocol Buffers, `sloppy_quorum` and `n_val` are also available.
- `node_confirms` (Riak 2.9+) requires acknowledgements from that many
  distinct physical nodes; `sync_on_write` (`RIAK_SYNC_BACKEND`, `_ONE`,
  `_ALL`) asks the vnodes to flush a write to disk before acknowledging it.
  Riak 2.2 accepts both and does not enforce them.
- `asis` stores the object exactly as sent, without the coordinating node
  updating its vector clock (replication tooling).
- Over Protocol Buffers, `return_head` returns the stored object's metadata
  and vector clock without its value, and `deletedvclock` makes a get of a
  deleted key return `RIAK_E_NOT_FOUND` with the tombstone's vector clock in
  `out->vclock` and `out->deleted` set.

## Metadata, secondary index entries, links

```c
riak_kv_pair meta[] = {{"owner", "ops"}};
riak_kv_pair idx[]  = {{"email_bin", "a@example.com"}, {"age_int", "42"}};
const char *links[] = {"</buckets/users/keys/u2>; riaktag=\"friend\""};
riak_opts o = {.meta = meta, .n_meta = 1, .indexes = idx, .n_indexes = 2,
               .links = links, .n_links = 1};
```

Fetched objects carry them back in `meta`, `indexes` and `links`, with the
`X-Riak-Meta-` and `X-Riak-Index-` prefixes removed.

## Bucket types

Object calls address a bucket type through `riak_opts::bucket_type`. The
bucket calls have `riak_type_*` variants: `riak_type_list_buckets()`,
`riak_type_list_keys()`, `riak_type_get_bucket_props()`,
`riak_type_set_bucket_props()` and `riak_type_reset_bucket_props()`.
`riak_get_type_props()` returns the properties of the type itself, and
`riak_set_type_props()` changes those that Riak allows to change after the
type was created:

```c
riak_set_type_props(c, "maps", "{\"props\":{\"n_val\":5}}");
```

Over HTTP the document is validated by the node; over Protocol Buffers an
unknown property is rejected with `RIAK_E_INVALID_ARG` before sending.
The bucket property documents also carry the Riak 2.9+ properties
`node_confirms`, `sync_on_write` and `aae_tree_exclude`.

## Legacy counters

Riak 1.4 counters live in ordinary buckets with `allow_mult` set and are
updated with `riak_counter_update()`; new code uses the counter data type
(see [Data types](datatypes.md)) instead.

```c
int64_t value;
riak_counter_update(c, "hits", "home", 1, NULL, &value);  /* returns the new value */
riak_counter_get(c, "hits", "home", NULL, &value);
```

They are available on both transports, in the default bucket type only.
An update is sent once and not retried, since repeating it would count
twice. `riak_get_client_id()` returns the client id of a Protocol Buffers
connection, base64 encoded.

## Siblings

In buckets with `allow_mult`, concurrent writes without a vector clock
create siblings. A plain `riak_get()` then fails with `RIAK_E_SIBLINGS`.
Siblings are handled in one of two ways:

```c
/* every sibling */
riak_object_set set;
riak_get_siblings(c, "users", "u1", &o, &set);
/* set.items[i] ... set.count, shared set.vclock */
riak_object_set_free(&set);

/* or a resolver that picks one */
static size_t newest(const riak_object *s, size_t n, void *ctx) { ... }
o.resolver = newest;
riak_get(c, "users", "u1", &o, &obj);
```

The resolved object carries the vector clock that covers every sibling;
writing it back with that vclock collapses the siblings. Tombstones are
marked with `riak_object::deleted`.

## Conditional requests

| Option | Effect |
|---|---|
| `if_none_match = etag` | Get returns `RIAK_E_NOT_MODIFIED` when unchanged |
| `if_none_match = "*"` | Put only creates; an existing key gives `RIAK_E_PRECONDITION` |
| `if_match = etag` | Put only replaces that version |
| `if_modified_since`, `if_unmodified_since` | HTTP-date conditions |
| `if_modified = true` + `vclock` | Get returns `RIAK_E_NOT_MODIFIED` when unchanged (Protocol Buffers) |
| `if_not_modified = true` + `vclock` | Put only replaces that version (Protocol Buffers) |

ETag and date conditions are HTTP features and vector clock conditions are
Protocol Buffers features; the other transport returns
`RIAK_E_UNSUPPORTED`, see [Transports](transports.md).

## Bucket properties and listing

`riak_get_bucket_props()`, `riak_set_bucket_props()` and
`riak_reset_bucket_props()` exchange JSON documents of the form
`{"props":{...}}`. `riak_list_buckets()` and `riak_list_keys()` return
NULL-terminated lists; key listing is streamed from the server.
`riak_list_keys_stream()` and `riak_list_buckets_stream()` pass each name to
a callback as it arrives, and stop when the callback returns false. Listings
walk the whole keyspace and are expensive on large clusters; secondary
indexes are preferred in production.

## Node information

- `riak_stats()` returns `/stats` as JSON (over Protocol Buffers through
  `riak_config::http_port`).
- `riak_get_server_info()` returns the node name and Riak version.
- `riak_get_preflist()` returns the vnodes that hold the replicas of a key,
  each with its partition, node and whether it is a primary (Protocol
  Buffers; over HTTP it returns `RIAK_E_UNSUPPORTED`, since Riak's HTTP
  resource ignored the key and was removed in Riak 3.0).
