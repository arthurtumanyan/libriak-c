# Riak KV 3.x node for the live tests, built from the OpenRiak sources
# (OpenRiak publishes no binaries). One node with the leveled backend
# (secondary indexes), tictac AAE (AAE folds) and a nextgenrepl source queue
# "libriak_q" that receives every change.
#
# The fixes in patches/<RIAK_TAG>/<app>/ are applied to the dependencies
# before the release is built (server bugs found while testing libriak-c and
# offered upstream); --build-arg RIAK_PATCHES=none builds the release as
# published.
#   docker build -t libriak-riak:3.4.1 tests/docker/openriak
#   docker build -t libriak-riak:3.2.6 --build-arg RIAK_TAG=riak-3.2.6 \
#       --build-arg OTP=24 tests/docker/openriak
#   docker run -d --ulimit nofile=65536:65536 -p 8087:8087 -p 8098:8098 libriak-riak:3.4.1
ARG OTP=26
FROM erlang:${OTP} AS build
ARG RIAK_TAG=riak-3.4.1
ARG RIAK_PATCHES=fixes
RUN apt-get update \
 && apt-get install -y --no-install-recommends git cmake libpam0g-dev \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch "${RIAK_TAG}" https://github.com/OpenRiak/riak.git /src
WORKDIR /src
COPY patches /tmp/patches
# rebar3 checks the dependencies out at the commits of rebar.lock; a patch
# that does not apply fails the build
RUN ./rebar3 get-deps \
 && if [ "${RIAK_PATCHES}" != none ] && [ -d "/tmp/patches/${RIAK_TAG}" ]; then \
      for dir in /tmp/patches/${RIAK_TAG}/*/; do \
        app=$(basename "$dir"); \
        for p in "$dir"*.patch; do \
          echo "applying $app/$(basename "$p")"; \
          git -C "_build/default/lib/$app" apply --verbose "$p" || exit 1; \
        done; \
      done; \
    fi
RUN make rel
COPY riak.conf.append /tmp/riak.conf.append
RUN sed -i -e 's/^nodename = .*/nodename = riak@127.0.0.1/' \
           -e 's/^listener\.http\.internal = .*/listener.http.internal = 0.0.0.0:8098/' \
           -e 's/^listener\.protobuf\.internal = .*/listener.protobuf.internal = 0.0.0.0:8087/' \
           -e 's/^storage_backend = .*/storage_backend = leveled/' \
           rel/riak/etc/riak.conf \
 && cat /tmp/riak.conf.append >> rel/riak/etc/riak.conf \
 && grep -q '^storage_backend = leveled$' rel/riak/etc/riak.conf \
 && grep -q '^listener.protobuf.internal = 0.0.0.0:8087$' rel/riak/etc/riak.conf

FROM erlang:${OTP}-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl procps libpam0g \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home-dir /opt/riak --shell /bin/bash riak
COPY --from=build --chown=riak:riak /src/rel/riak /opt/riak
USER riak
ENV PATH=/opt/riak/bin:$PATH
EXPOSE 8087 8098 8443
CMD ["riak", "foreground"]
