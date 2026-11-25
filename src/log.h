#pragma once

namespace fpvlog {

// Appends a timestamped line to GTAIV/libfpv/libfpv_gtaiv.log.
void write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace fpvlog
