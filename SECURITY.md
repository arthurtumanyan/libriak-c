# Security policy

## Supported versions

| Version | Supported |
|---|---|
| 2.x | yes |
| 1.x | no |

## Reporting a vulnerability

Vulnerabilities are reported privately through
[GitHub security advisories](https://github.com/arthurtumanyan/libriak-c/security/advisories/new)
or by e-mail to arthurtumanyan@gmail.com, not through public issues.

A report includes the affected version, a description of the impact and, if
possible, a reproducer. Reports are acknowledged within 7 days. Fixes are
released as a patch version, with a CHANGELOG entry that credits the reporter
unless anonymity is requested.

## Security-relevant behaviour

The library's safeguards (certificate verification, credentials only over
TLS, input encoding, response size limits) are described in
[docs/security.md](docs/security.md).
