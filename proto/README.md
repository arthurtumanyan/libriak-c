# Riak protocol buffer definitions

The `.proto` files and `riak_pb_messages.csv` are taken from the OpenRiak
forks of the Basho repositories, which carry the Riak 3.x additions:

- `riak.proto`, `riak_kv.proto`, `riak_dt.proto`, `riak_search.proto`,
  `riak_yokozuna.proto`, `riak_pb_messages.csv`:
  [OpenRiak/riak_pb](https://github.com/OpenRiak/riak_pb), branch
  `openriak-3.4`, commit `5f6e994cd45db5bdb254a515babb48d38ead485a`
- `riak_repl.proto`: [OpenRiak/riak_repl](https://github.com/OpenRiak/riak_repl),
  branch `openriak-3.4`, commit `774519154717081ea7d1d09ec36c3dfbf95eb760`

The files are unchanged except for a `syntax = "proto2";` line added to each
to silence protoc warnings, and the values of `RpbBucketProps.SyncOnWrite` in
`riak.proto`, renamed from `backend`/`one`/`all` to
`sync_backend`/`sync_one`/`sync_all`: upstream's `backend` collides with the
field of the same name, which protoc rejects (Erlang's gpb accepts it). Enum
values travel as numbers, so the rename does not change the wire format. They are distributed under the Apache License 2.0
(see `LICENSE`).

C code is generated from them with `protoc-c` at build time. The message
codes from `riak_pb_messages.csv` are mirrored in `src/pb.h`.
