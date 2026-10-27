# Data types (CRDTs)

Riak data types merge concurrent updates without siblings. A bucket holding
data types belongs to a bucket type created with a `datatype` property:

```sh
riak-admin bucket-type create maps '{"props":{"datatype":"map"}}'
riak-admin bucket-type activate maps
```

| Type | `datatype` | Riak version |
|---|---|---|
| Counter | `counter` | 2.0+ |
| Set | `set` | 2.0+ |
| Map | `map` | 2.0+ |
| HyperLogLog | `hll` | 2.2+ |
| Grow-only set | `gset` | 2.9+ |

Registers and flags exist only as map fields.

## Updating

Updates are described with an operation and sent with `riak_dt_update()`:

```c
riak_dt_op *op = riak_dt_op_new(RIAK_DT_MAP);
riak_dt_op_map_register(op, "name", "alice");
riak_dt_op_map_flag(op, "admin", true);
riak_dt_op_map_counter(op, "visits", 1);
riak_dt_op_add(riak_dt_op_map_set(op, "tags"), "staff");
riak_dt_op_map_register(riak_dt_op_map_map(op, "address"), "city", "Yerevan");

riak_dt_opts o = {.return_body = true};
riak_datatype user;
riak_status st = riak_dt_update(c, "maps", "users", "u1", op, NULL, &o, &user, NULL);
riak_dt_op_free(op);
```

- Building never fails midway. An allocation failure, a call that does not
  fit the operation's type (for example an increment on a set), a counter
  overflow or maps nested deeper than 32 levels are remembered; the update
  then fails with `RIAK_E_INVALID_ARG` or `RIAK_E_NOMEM` before anything is
  sent.
- Repeated counter increments accumulate; the last register value wins.
- Nested operations returned by `riak_dt_op_map_set()` and
  `riak_dt_op_map_map()` belong to the parent and are freed with it.
- A `NULL` key lets Riak generate one, returned through `created_key`.
- An operation that does not match the bucket type fails with `RIAK_E_HTTP`
  (HTTP 400).

## Removing

Removals of set elements and map fields send the causal context of a
previous fetch:

```c
riak_datatype set;
riak_dt_fetch(c, "sets", "groups", "admins", NULL, &set);
riak_dt_op *rm = riak_dt_op_new(RIAK_DT_SET);
riak_dt_op_remove(rm, "former-member");
riak_dt_update(c, "sets", "groups", "admins", rm, set.context, NULL, NULL, NULL);
riak_dt_op_free(rm);
riak_datatype_free(&set);
```

Without a context, Riak checks that the element exists; removing an absent
element then fails with `RIAK_E_PRECONDITION`. With a context, such a removal
is a no-op.

## Reading

`riak_dt_fetch()` returns a `riak_datatype`: the value tree and the context.
Counters and HyperLogLog cardinalities are found in `value.counter`, set
members in `value.elements`, and map fields in `value.entries` or through
`riak_dt_map_get()`:

```c
const riak_dt_value *visits = riak_dt_map_get(&user.value, "visits", RIAK_DT_COUNTER);
if (visits)
    printf("%lld visits\n", (long long)visits->counter);
```

`riak_dt_opts::no_context` omits the context when only reads are planned.
