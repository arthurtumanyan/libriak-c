# Security

## TLS

```c
cfg.tls = true;
cfg.ca_file = "/etc/riak/ca.pem";   /* NULL: system CA store */
```

The server certificate and host name (or IP address) are verified by
default. `tls_verify = false` disables verification and is meant only for
tests. Verification failures are reported as `RIAK_E_TLS`.

Over HTTP the connection is HTTPS. Over Protocol Buffers, TLS is started with
`RpbStartTls` on the normal port, followed by the handshake (TLS 1.2 or
newer, through OpenSSL) and, when a user is configured, `RpbAuthReq`. Riak
accepts STARTTLS only when Riak security is enabled; a node without security
refuses it, which is reported as `RIAK_E_TLS`.

## Nodes on an old Erlang/OTP

Riak KV 2.2 and older run a TLS stack that cannot complete a handshake with
OpenSSL 3 clients, over HTTPS or STARTTLS. The node crashes on RSA-PSS and
EdDSA signature algorithms, lacks secure renegotiation, offers only 1024-bit
DHE groups and has a broken ECDHE. The client sees the connection closed
during the handshake, which is reported as `RIAK_E_TLS` with a hint to
`tls_legacy`. Each such failed handshake also leaks file descriptors inside
the node, so repeated attempts eventually exhaust its descriptor limit.

```c
cfg.tls = true;
cfg.tls_legacy = true;   /* old nodes only */
```

`tls_legacy` limits the connection to TLS 1.2, offers only RSA and ECDSA
signature algorithms with SHA-1/SHA-2, accepts servers without secure
renegotiation and leaves out DHE key exchange. ECDHE is still offered, so a
node with a working ECDHE keeps forward secrecy; the old nodes above fall
back to RSA key exchange, which has none. Certificate and host name
verification are unaffected. The option weakens TLS and is meant only for
such nodes; current Riak releases do not need it.

## Riak security

With Riak security enabled (`riak-admin security enable`), requests are
authenticated either with a password or with a client certificate.

Password (HTTP Basic, or `RpbAuthReq` over Protocol Buffers; TLS only):

```c
cfg.tls = true;
cfg.username = "app";
cfg.password = "secret";
```

Client certificate:

```c
cfg.tls = true;
cfg.tls_cert_file = "client.pem";
cfg.tls_key_file = "client.key";
cfg.tls_key_password = "passphrase";   /* encrypted keys only */
```

- Credentials are refused with `RIAK_E_INVALID_ARG` when `tls` is not set,
  so they are never sent in clear text.
- HTTP 401 and 403, a refused `RpbAuthReq` and Riak's "Permission denied"
  errors are reported as `RIAK_E_UNAUTHORIZED`.
- With Protocol Buffers, a certificate-only user is configured with
  `username` (the certificate's common name) and no password.
- A server that requires a client certificate which was not supplied, or
  rejects the one supplied, is reported as `RIAK_E_TLS`. With TLS 1.3 this
  rejection arrives after the handshake; it is recognized from the
  handshake and not mistaken for a retryable network error.
- The client keeps its own copies of the password and key passphrase and
  overwrites them when it is freed.

## Other safeguards

- Bucket, key and index names are percent-encoded; header values with CR or
  LF are rejected, which prevents header injection.
- Responses larger than `max_body_bytes` (64 MiB by default) are rejected,
  which bounds memory use with a misbehaving server. Over Protocol Buffers
  the limit applies to every message, before it is read.
- Only the `http` and `https` protocols are enabled in libcurl.
- Protocol Buffers sockets never raise `SIGPIPE`, also under TLS.
- The authentication message is wiped from memory after it is sent.

Vulnerabilities are reported as described in `SECURITY.md`.
