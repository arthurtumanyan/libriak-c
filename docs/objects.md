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
`riak_get_type_props()` returns the properties of the type itself.

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

## Bucket properties and listing

`riak_get_bucket_props()`, `riak_set_bucket_props()` and
`riak_reset_bucket_props()` exchange JSON documents of the form
`{"props":{...}}`. `riak_list_buckets()` and `riak_list_keys()` return
NULL-terminated lists; key listing is streamed from the server. Both walk the
whole keyspace and are expensive on large clusters; secondary indexes are
preferred in production. `riak_stats()` returns `/stats` as JSON.
