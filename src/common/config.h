#pragma once
#include <cstdint>
#include <cstddef>

namespace minidb {

// ---- On-disk / buffer constants ----
static constexpr int PAGE_SIZE = 4096;        // bytes per page
static constexpr int DEFAULT_POOL_FRAMES = 256; // buffer pool size (pages)
static constexpr int INVALID_PAGE_ID = -1;
static constexpr int HEADER_PAGE_ID = 0;      // page 0 reserved for catalog/meta bootstrap

// ---- Type aliases used across the system ----
using page_id_t = int32_t;
using slot_id_t = int32_t;
using frame_id_t = int32_t;
using txn_id_t = int64_t;
using lsn_t = int64_t;        // log sequence number
using oid_t = int32_t;        // object id (tables, indexes)

static constexpr lsn_t INVALID_LSN = -1;
static constexpr txn_id_t INVALID_TXN_ID = -1;

}  // namespace minidb
