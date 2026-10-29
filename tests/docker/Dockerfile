# Riak KV 2.x node for the live tests: leveldb backend (secondary indexes)
# and Riak Search (Solr) enabled. RIAK_IMAGE selects an official basho image:
# latest (2.2.3), ubuntu-2.2.0, ubuntu-2.1.4 or ubuntu-2.0.7.
#   docker build -t libriak-riak tests/docker
#   docker build -t libriak-riak:2.1.4 --build-arg RIAK_IMAGE=ubuntu-2.1.4 tests/docker
#   docker run -d -p 8098:8098 -p 8087:8087 libriak-riak
ARG RIAK_IMAGE=latest
FROM basho/riak-kv:${RIAK_IMAGE}
RUN sed -i -e 's/^storage_backend = bitcask$/storage_backend = leveldb/' \
           -e 's/^search = off$/search = on/' /etc/riak/riak.conf \
 && grep -q '^storage_backend = leveldb$' /etc/riak/riak.conf \
 && grep -q '^search = on$' /etc/riak/riak.conf
