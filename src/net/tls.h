// TLS for the built-in HTTP client's own sockets (https:// on platforms
// without an OS HTTP stack that fetchUrl() uses instead).
//
// Linux: OpenSSL (libssl.so.3, else .so.1.1) loaded with dlopen on first
// use, so the library costs nothing in libjsui's size and nothing at all
// when no https:// request is made. Certificates are checked against the
// system trust store (SSL_CTX_set_default_verify_paths) with hostname
// verification. Other platforms: tlsAvailable() is false.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace jsui::net {

// A TLS session over a connected, non-blocking socket it does not own.
class TlsSession {
 public:
  enum : long long { kWantRead = -2, kWantWrite = -3 };
  virtual ~TlsSession() = default;
  // Handshake step: 1 done, 0 in progress (see wantWrite()), -1 failed (*err).
  virtual int handshake(std::string* err) = 0;
  // Like send()/recv() on a non-blocking socket: bytes moved, 0 on a clean
  // close (read), -1 on an error (*err), or kWantRead / kWantWrite.
  virtual long long write(const char* data, size_t size, std::string* err) = 0;
  virtual long long read(char* data, size_t size, std::string* err) = 0;
  // After handshake() returned 0: whether it waits to write (else to read).
  virtual bool wantWrite() const = 0;
};

// Whether https:// works on this client; `why` says what is missing.
bool tlsAvailable(std::string* why);
// Starts a client session for `host` (SNI + certificate name). nullptr with
// *err when TLS is unavailable.
std::unique_ptr<TlsSession> tlsClient(intptr_t socket, const std::string& host, std::string* err);

}  // namespace jsui::net
