// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Cancelling a long byte-level conversion stops it promptly: every long loop
// (encoders, compressors, checksums, archive writers, copies) reports progress
// and honours cancellation at least every few MiB, not only between stages.
#include <chrono>
#include <string>
#include <string_view>

#include "engine/convert.hpp"
#include "engine/registry.hpp"
#include "support/test_support.hpp"

using namespace cvt;

TEST_CASE("cancel: large byte-level conversions stop soon after the request") {
  const Bytes binary = test::random_bytes(std::size_t{16} << 20, 11);
  std::string text(std::size_t{16} << 20, 'a');
  for (std::size_t i = 60; i < text.size(); i += 61) text[i] = '\n';
  struct Route {
    std::string_view source;
    std::string_view target;
  };
  for (const Route route : {Route{"txt", "base64"}, Route{"txt", "txt"}, Route{"bin", "hex"}, Route{"bin", "gz"},
                            Route{"bin", "bz2"}, Route{"bin", "xz"}, Route{"bin", "zst"}, Route{"bin", "checksums"},
                            Route{"bin", "zip"}, Route{"bin", "tar"}, Route{"bin", "uue"}}) {
    INFO(route.source, " -> ", route.target);
    ConvertRequest request;
    request.input = route.source == "txt" ? as_bytes(text) : ByteView(binary);
    request.source_id = std::string(route.source);
    request.target_id = std::string(route.target);
    request.filename = "data";
    using clock = std::chrono::steady_clock;
    clock::time_point asked{};
    bool cancelled = false;
    // Cancel as soon as the writer has made visible progress (the write stage
    // covers 55..100 %).
    request.progress = Progress([&](double fraction, std::string_view) {
      if (!cancelled && fraction > 0.56) {
        cancelled = true;
        asked = clock::now();
      }
      return cancelled;
    });
    const auto out = run_conversion(Registry::instance(), request);
    const double latency = std::chrono::duration<double>(clock::now() - asked).count();
    REQUIRE(cancelled);
    REQUIRE(!out.has_value());
    CHECK(out.error().code == Status::Cancelled);
    // Generous for Debug and sanitizer builds (Release stops within a few
    // milliseconds of work; part of this is releasing the buffers).
    CHECK(latency < 3.0);
  }
}
