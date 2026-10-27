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
