#!/bin/sh
# Generate a throwaway CA, a server certificate for 127.0.0.1 and client
# certificates for the TLS tests: tests/gen_certs.sh <output-dir>
set -eu
out=${1:?output directory}
mkdir -p "$out"
cd "$out"
q() { "$@" >/dev/null 2>&1; }

q openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=libriak-c test CA" \
    -keyout ca.key -out ca.pem

q openssl req -newkey rsa:2048 -nodes -subj "/CN=127.0.0.1" -keyout server.key -out server.csr
printf 'subjectAltName=IP:127.0.0.1,DNS:localhost\n' > server.ext
q openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 2 \
    -extfile server.ext -out server.pem

q openssl req -newkey rsa:2048 -nodes -subj "/CN=riak-user" -keyout client.key -out client.csr
q openssl x509 -req -in client.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 2 \
    -out client.pem
# the same client key, encrypted, for tls_key_password
q openssl pkey -in client.key -aes256 -passout pass:secret -out client-enc.key

# a valid certificate for another name, to check host name verification
q openssl req -newkey rsa:2048 -nodes -subj "/CN=other.example" -keyout wrong.key -out wrong.csr
printf 'subjectAltName=DNS:other.example\n' > wrong.ext
q openssl x509 -req -in wrong.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 2 \
    -extfile wrong.ext -out wrong.pem

# a CA nobody trusts, to check that verification really happens
q openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=other CA" \
    -keyout other.key -out other.pem
rm -f ./*.csr ./*.srl server.ext wrong.ext
