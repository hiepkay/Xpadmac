# Test vectors: throwaway keys, not secrets

The files here are **test material only**. They were generated with `openssl` for XPadMac's authentication tests and protect nothing:

- `rsa2048.pem`, `rsa2048_e3.pem`: RSA-2048 keys (exponent 65537 and 3), made with `openssl genpkey`;
- `p256_client.pem`, `p256_host.pem`: P-256 (prime256v1) keys, made with `openssl genpkey`;
- `cert.der`: a self-signed certificate for the simulated pad (`openssl req -x509`), with an empty subject.

`Tests/gen_auth_vectors.py` turns them into `Tests/auth_vectors.h`, and `make -C Tests auth-vectors-check` checks that the two still agree. They are committed so that the vectors are the same on every run.

If a secret scanner (for example GitHub push protection) flags these files, mark the alert as "used in tests".
