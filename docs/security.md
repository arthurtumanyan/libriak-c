# Security

## TLS

```c
cfg.tls = true;
cfg.ca_file = "/etc/riak/ca.pem";   /* NULL: system CA store */
```

The server certificate and host name are verified by default.
`tls_verify = false` disables verification and is meant only for tests.
Verification failures are reported as `RIAK_E_TLS`.

## Riak security

With Riak security enabled (`riak-admin security enable`), requests are
authenticated either with a password or with a client certificate.

Password (HTTP Basic, over TLS only):

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
- HTTP 401 and 403 are reported as `RIAK_E_UNAUTHORIZED`.
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
  which bounds memory use with a misbehaving server.
- Only the `http` and `https` protocols are enabled in libcurl.

Vulnerabilities are reported as described in `SECURITY.md`.
