#pragma once

#include "mailai/store.h"

namespace mailai::store_detail {
inline constexpr int schema_version = 1;
Status ensure_schema(sqlite3* db, OpenMode mode);
}  // namespace mailai::store_detail
