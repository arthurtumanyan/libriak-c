# Riak KV node for the live tests: leveldb backend (secondary indexes) and
# Riak Search (Solr) enabled.
#   docker build -t libriak-riak tests/docker
#   docker run -d -p 8098:8098 -p 8087:8087 libriak-riak
FROM basho/riak-kv:latest
RUN sed -i -e 's/^storage_backend = bitcask$/storage_backend = leveldb/' \
           -e 's/^search = off$/search = on/' /etc/riak/riak.conf \
 && grep -q '^storage_backend = leveldb$' /etc/riak/riak.conf \
 && grep -q '^search = on$' /etc/riak/riak.conf
