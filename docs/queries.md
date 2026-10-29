# Queries

## Secondary indexes

Secondary indexes need the leveldb or memory backend; with bitcask, Riak
answers with an error. Index entries are written through
`riak_opts::indexes` (names end in `_int` or `_bin`).

```c
riak_index_query q = {.index = "age_int", .range_min = "18", .range_max = "65",
                      .return_terms = true, .max_results = 100};
riak_index_result page;
char *next = NULL;
do {
    if (riak_index_query_run(c, "users", &q, &page) != RIAK_OK)
        break;
    for (size_t i = 0; i < page.count; i++)
        printf("%s = %s\n", page.keys[i], page.terms[i]);
    free(next);
    next = page.continuation ? strdup(page.continuation) : NULL;
    q.continuation = next;
    riak_index_result_free(&page);
} while (next != NULL);
```

- Exactly one of `value` (exact match) or `range_min`/`range_max` is set.
- `$key` (key range) and `$bucket` (all keys, `value` = bucket name) are
  built-in indexes.
- `return_terms` and `term_regex` apply to range queries.
- `riak_index_query_stream()` delivers matches to a callback as they arrive;
  returning `false` from the callback stops the stream and aborts the
  transfer. The continuation of a paged stream is returned through its last
  parameter.
- Over Protocol Buffers, `cover_context` limits a query to one entry of a
  coverage plan from `riak_get_coverage()`, so a large query can be split
  and run in parallel; see [Transports](transports.md).

## Riak Search

Riak Search (Yokozuna) needs `search = on` in `riak.conf` and a Java runtime
on the nodes.

```c
riak_search_index_create(c, "users_idx", NULL, 0);    /* _yz_default schema */
riak_set_bucket_props(c, "users", "{\"props\":{\"search_index\":\"users_idx\"}}");

riak_search_query q = {.q = "name_s:alice", .sort = "age_i desc", .rows = 10};
riak_search_result r;
if (riak_search(c, "users_idx", &q, &r) == RIAK_OK) {
    for (size_t i = 0; i < r.count; i++)
        printf("%s/%s %.2f\n", r.docs[i].bucket, r.docs[i].key, r.docs[i].score);
    riak_search_result_free(&r);
}
```

- Index creation completes asynchronously on the server. An index is ready
  once `riak_search_index_get()` finds it and a query on it succeeds; objects
  written earlier are indexed later by anti-entropy.
- Documents become searchable after Solr's soft commit, about one second.
- `riak_search_doc::fields` lists every returned field; multi-valued fields
  appear once per value. `riak_search_result::raw` keeps the complete Solr
  response.
- `riak_search_index_list()`, `riak_search_index_delete()`,
  `riak_search_schema_put()` and `riak_search_schema_get()` manage indexes
  and schemas.

## MapReduce

```c
const char *job =
    "{\"inputs\":[[\"users\",\"u1\"],[\"users\",\"u2\"]],"
    "\"query\":[{\"map\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
    "\"function\":\"map_object_value\"}}]}";
char *result;
if (riak_mapred(c, job, &result) == RIAK_OK) {
    puts(result);                        /* JSON array */
    riak_free(result);
}
```

`riak_mapred_stream()` reports each phase result as it arrives. MapReduce is
deprecated in recent Riak releases and removed in OpenRiak 4.x; secondary
indexes or Search are preferred for new code.

## Query API (Riak 3.4)

Riak 3.4 adds aggregated and filtered secondary index queries, served over
HTTP only. On the Protocol Buffers transport `riak_query_run()` is sent to
`riak_config::http_port`, as `riak_stats()` is; without one it returns
`RIAK_E_UNSUPPORTED`.

```c
riak_query_part part = {.index_name = "people_bin",
                        .start_term = "a", .end_term = "n",
                        .evaluation_expression = "delim($term, \"|\", ($name, $city))",
                        .filter_expression = "$city = :city"};
riak_kv_pair subs[] = {{"city", "Yerevan"}};
riak_query q = {.parts = &part, .n_parts = 1,
                .substitutions = subs, .n_substitutions = 1,
                .accumulation_option = "keys"};
riak_query_result r;
if (riak_query_run(c, "people", &q, &r) == RIAK_OK) {
    for (size_t i = 0; i < r.count; i++) {
        puts(r.keys[i]);
    }
    riak_query_result_free(&r);
}
```

- Several parts, each with an `aggregation_tag`, are combined with an
  `aggregation_expression` such as `"$1 INTERSECT $2"`.
- `accumulation_option` selects the result: keys, terms
  (`riak_query_result::terms` parallel to `keys`), counts
  (`riak_query_result::total`) or term counts. `riak_query_result::raw`
  keeps the complete response.
- The `queue_*` options return a `result_queue` reference; its entries are
  read in pages with `riak_query_fetch()`, which removes them from the
  queue and is therefore never retried. `riak_query_result::complete` is
  set once the query has finished and every queued entry has been read.
- Riak 3.4.1 fails internally when `max_results` is lower than the number
  of matches (see [Operations](operations.md#known-server-issues)).

## Object folds

`riak_fold_objects()` (Protocol Buffers) returns the objects of a key range,
values included, in key order and in pages:

```c
static bool on_object(const char *key, riak_object *obj, void *ctx) {
    (void)ctx;
    printf("%s: %.*s\n", key, (int)obj->len, obj->data);
    return true;                          /* false stops the fold */
}

riak_fold_query q = {.start_key = "a", .end_key = "m", .max_results = 100};
char *next = NULL;
riak_status st = riak_fold_objects(c, "users", &q, on_object, NULL, &next);
/* with next set, q.continuation = next fetches the following page */
riak_free(next);
```

The range includes `start_key` and excludes `end_key` unless
`start_exclusive` or `end_inclusive` say otherwise. The callback does not
issue requests on the same client (see [Errors](errors.md#threading)).

## Link walking

`riak_link_walk()` follows the links stored with objects. Each step names a
bucket and a tag (NULL matches any) and whether its objects are returned;
the objects of the last step are always returned:

```c
riak_link_step steps[] = {{.tag = "friend", .keep = true}, {.tag = "employer"}};
riak_link_result res;
if (riak_link_walk(c, "people", "alice", steps, 2, &res) == RIAK_OK) {
    for (size_t p = 0; p < res.n_phases; p++) {
        for (size_t i = 0; i < res.phases[p].count; i++) {
            printf("%zu %s/%s\n", p, res.phases[p].items[i].bucket,
                   res.phases[p].items[i].key);
        }
    }
    riak_link_result_free(&res);
}
```

HTTP uses Riak's link walker; Protocol Buffers runs the same walk as a
MapReduce job of link phases and then reads the objects. Both drop links to
objects that no longer exist and return each object once per phase. Riak
refuses link walking while Riak security is enabled (`RIAK_E_UNSUPPORTED`).
