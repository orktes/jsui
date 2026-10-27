// TLS for the HTTP client's sockets: OpenSSL through dlopen (see tls.h).
#include "net/tls.h"

#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)

#include <dlfcn.h>

#include <algorithm>
#include <mutex>
#include <type_traits>

namespace jsui::net {

namespace {

// The few OpenSSL 1.1 / 3.x entry points the client needs; they have the
// same names and ABI in both. Opaque pointers stand in for SSL / SSL_CTX.
struct OpenSsl {
  void* libssl = nullptr;
  void* libcrypto = nullptr;
  std::string why;
  void* (*TLS_client_method)() = nullptr;
  void* (*SSL_CTX_new)(void*) = nullptr;
  int (*SSL_CTX_set_default_verify_paths)(void*) = nullptr;
  void (*SSL_CTX_set_verify)(void*, int, void*) = nullptr;
  long (*SSL_CTX_ctrl)(void*, int, long, void*) = nullptr;
  void* (*SSL_new)(void*) = nullptr;
  void (*SSL_free)(void*) = nullptr;
  int (*SSL_set_fd)(void*, int) = nullptr;
  long (*SSL_ctrl)(void*, int, long, void*) = nullptr;
  int (*SSL_set1_host)(void*, const char*) = nullptr;
  int (*SSL_connect)(void*) = nullptr;
  int (*SSL_read)(void*, void*, int) = nullptr;
  int (*SSL_write)(void*, const void*, int) = nullptr;
  int (*SSL_get_error)(const void*, int) = nullptr;
  int (*SSL_shutdown)(void*) = nullptr;
  long (*SSL_get_verify_result)(const void*) = nullptr;
  const char* (*X509_verify_cert_error_string)(long) = nullptr;
  unsigned long (*ERR_get_error)() = nullptr;
  void (*ERR_error_string_n)(unsigned long, char*, size_t) = nullptr;
  void (*ERR_clear_error)() = nullptr;
  void* ctx = nullptr;  // one shared client SSL_CTX (thread safe once set up)

  bool load() {
    const char* ssl[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so"};
    const char* crypto[] = {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"};
    for (int i = 0; i < 3 && !libssl; i++) {
      libcrypto = dlopen(crypto[i], RTLD_NOW | RTLD_LOCAL);
      if (!libcrypto) continue;
      libssl = dlopen(ssl[i], RTLD_NOW | RTLD_LOCAL);
      if (!libssl) {
        dlclose(libcrypto);
        libcrypto = nullptr;
      }
    }
    if (!libssl) {
      why = "https:// needs OpenSSL (libssl.so.3 or libssl.so.1.1), which is not installed";
      return false;
    }
    bool ok = true;
    auto sym = [&](void* lib, auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
      if (!fn) {
        ok = false;
        why = std::string("https://: OpenSSL lacks ") + name;
      }
    };
    sym(libssl, TLS_client_method, "TLS_client_method");
    sym(libssl, SSL_CTX_new, "SSL_CTX_new");
    sym(libssl, SSL_CTX_set_default_verify_paths, "SSL_CTX_set_default_verify_paths");
    sym(libssl, SSL_CTX_set_verify, "SSL_CTX_set_verify");
    sym(libssl, SSL_CTX_ctrl, "SSL_CTX_ctrl");
    sym(libssl, SSL_new, "SSL_new");
    sym(libssl, SSL_free, "SSL_free");
    sym(libssl, SSL_set_fd, "SSL_set_fd");
    sym(libssl, SSL_ctrl, "SSL_ctrl");
    sym(libssl, SSL_set1_host, "SSL_set1_host");
    sym(libssl, SSL_connect, "SSL_connect");
    sym(libssl, SSL_read, "SSL_read");
    sym(libssl, SSL_write, "SSL_write");
    sym(libssl, SSL_get_error, "SSL_get_error");
    sym(libssl, SSL_shutdown, "SSL_shutdown");
    sym(libssl, SSL_get_verify_result, "SSL_get_verify_result");
    sym(libcrypto, X509_verify_cert_error_string, "X509_verify_cert_error_string");
    sym(libcrypto, ERR_get_error, "ERR_get_error");
    sym(libcrypto, ERR_error_string_n, "ERR_error_string_n");
    sym(libcrypto, ERR_clear_error, "ERR_clear_error");
    if (!ok) return false;
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
      why = "https://: SSL_CTX_new failed";
      return false;
    }
    constexpr int kVerifyPeer = 1;                  // SSL_VERIFY_PEER
    constexpr int kSetMinProtoVersion = 123;        // SSL_CTRL_SET_MIN_PROTO_VERSION
    constexpr long kTls12 = 0x0303;                 // TLS1_2_VERSION
    SSL_CTX_set_verify(ctx, kVerifyPeer, nullptr);
    SSL_CTX_ctrl(ctx, kSetMinProtoVersion, kTls12, nullptr);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      why = "https://: cannot load the system's CA certificates";
      return false;
    }
    return true;
  }

  std::string lastError(const char* what) {
    std::string out = what;
    unsigned long e = ERR_get_error();
    if (e) {
      char buf[256];
      ERR_error_string_n(e, buf, sizeof buf);
      out += std::string(": ") + buf;
    }
    ERR_clear_error();
    return out;
  }
};

OpenSsl* openssl(std::string* why) {
  static OpenSsl lib;
  static bool ok = false;
  static std::once_flag once;
  std::call_once(once, [] { ok = lib.load(); });
  if (!ok) {
    if (why) *why = lib.why;
    return nullptr;
  }
  return &lib;
}

constexpr int kErrorWantRead = 2, kErrorWantWrite = 3, kErrorZeroReturn = 6;  // SSL_ERROR_*

class OpenSslSession : public TlsSession {
 public:
  OpenSslSession(OpenSsl* lib, void* ssl, std::string host) : lib_(lib), ssl_(ssl), host_(std::move(host)) {}
  ~OpenSslSession() override {
    if (handshaken_) lib_->SSL_shutdown(ssl_);  // best effort close_notify
    lib_->SSL_free(ssl_);
    lib_->ERR_clear_error();
  }

  int handshake(std::string* err) override {
    int r = lib_->SSL_connect(ssl_);
    if (r == 1) {
      handshaken_ = true;
      return 1;
    }
    int e = lib_->SSL_get_error(ssl_, r);
    if (e == kErrorWantRead || e == kErrorWantWrite) {
      wantWrite_ = e == kErrorWantWrite;
      return 0;
    }
    long v = lib_->SSL_get_verify_result(ssl_);
    if (v != 0) {
      *err = "https://" + host_ + ": certificate verification failed (" + lib_->X509_verify_cert_error_string(v) + ")";
      lib_->ERR_clear_error();
    } else {
      *err = lib_->lastError(("TLS handshake with " + host_ + " failed").c_str());
    }
    return -1;
  }

  long long write(const char* data, size_t size, std::string* err) override {
    int n = lib_->SSL_write(ssl_, data, int(std::min<size_t>(size, 1 << 30)));
    if (n > 0) return n;
    return status(n, err, "TLS write failed");
  }

  long long read(char* data, size_t size, std::string* err) override {
    int n = lib_->SSL_read(ssl_, data, int(std::min<size_t>(size, 1 << 30)));
    if (n > 0) return n;
    return status(n, err, "TLS read failed");
  }

  bool wantWrite() const override { return wantWrite_; }

 private:
  long long status(int n, std::string* err, const char* what) {
    int e = lib_->SSL_get_error(ssl_, n);
    if (e == kErrorWantRead) return kWantRead;
    if (e == kErrorWantWrite) return kWantWrite;
    if (e == kErrorZeroReturn) return 0;
    // An EOF without close_notify: servers that send Connection: close often
    // do this; the HTTP framing (Content-Length / chunked) catches truncation.
    if (e == 5 /* SSL_ERROR_SYSCALL */ && lib_->ERR_get_error() == 0) return 0;
    *err = lib_->lastError(what);
    return -1;
  }

  OpenSsl* lib_;
  void* ssl_;
  std::string host_;
  bool wantWrite_ = false;
  bool handshaken_ = false;
};

}  // namespace

bool tlsAvailable(std::string* why) { return openssl(why) != nullptr; }

std::unique_ptr<TlsSession> tlsClient(intptr_t socket, const std::string& host, std::string* err) {
  OpenSsl* lib = openssl(err);
  if (!lib) return nullptr;
  void* ssl = lib->SSL_new(lib->ctx);
  if (!ssl) {
    *err = lib->lastError("SSL_new failed");
    return nullptr;
  }
  constexpr int kSetTlsextHostname = 55, kNameTypeHostName = 0;  // SSL_CTRL_SET_TLSEXT_HOSTNAME
  bool ipLiteral = host.find(':') != std::string::npos ||
                   host.find_first_not_of("0123456789.") == std::string::npos;
  if (!ipLiteral) lib->SSL_ctrl(ssl, kSetTlsextHostname, kNameTypeHostName, const_cast<char*>(host.c_str()));
  if (lib->SSL_set_fd(ssl, int(socket)) != 1 || lib->SSL_set1_host(ssl, host.c_str()) != 1) {
    *err = lib->lastError("TLS setup failed");
    lib->SSL_free(ssl);
    return nullptr;
  }
  return std::make_unique<OpenSslSession>(lib, ssl, host);
}

}  // namespace jsui::net

#else

namespace jsui::net {

bool tlsAvailable(std::string* why) {
#if defined(__ANDROID__)
  // Apps may not load the system's libssl: the host brings https:// (the
  // example host uses java.net.HttpURLConnection, integrations/android).
  if (why) *why = "https:// on Android needs the host's networking: set JsuiConfig.on_request";
#else
  if (why) *why = "https:// is not available in the built-in loader on this platform; set JsuiConfig.on_request";
#endif
  return false;
}

std::unique_ptr<TlsSession> tlsClient(intptr_t, const std::string&, std::string* err) {
  tlsAvailable(err);
  return nullptr;
}

}  // namespace jsui::net

#endif
