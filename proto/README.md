# Riak protocol buffer definitions

The `.proto` files and `riak_pb_messages.csv` are taken unchanged from
[basho/riak_pb](https://github.com/basho/riak_pb), commit
`7a5e535217c13a32f3041888b0d46e9b4476065c` (branch `develop`), except that a
`syntax = "proto2";` line is added to each file to silence protoc warnings.
They are distributed under the Apache License 2.0 (see `LICENSE`).

C code is generated from them with `protoc-c` at build time. The message
codes from `riak_pb_messages.csv` are mirrored in `src/pb.h`.
