// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenScarlett Contributors
//
// Clean-room Xbox Series X|S DirectStorage Hardware MDU BCPack Bitstream Decoder
// (DSTORAGE_BCPACK_MODE 1..7: BC1, BC3, BC4, BC5, BC6H, BC7).
//
// Provenance & Permissive MIT Attribution:
//   - Entropy coder & 16-bit banded rANS probability law:
//       * Microsoft `silicon-rans` (`mdu_minirans.v`), MIT License
//       * Tim0219800 `bcpack-decoder` (`docs/SPEC.md`, `src/model.rs`, `src/lanes.rs`),
//         Copyright (c) 2026 Tim0219800, MIT License
//   - Interleaved 12/16-lane 8-byte/cycle scheduler, static codec tables, and
//     BC1/BC3/BC4/BC5/BC6H/BC7 block reconstruction:
//       * wxrrry `leonida-asset-pipeline` (`tools/bcpack.py`, `tools/bcpack_tables.py`,
//         `tools/bcpack_tables_bptc.py`), Copyright (c) 2026 wxrrry, MIT License
//   - Public Khronos / Microsoft D3D11 BPTC (BC6H/BC7) & S3TC/RGTC (BC1..BC5) specs,
//     and US Patent 11,234,023 B2.

#pragma once

#include <cstddef>
#include <cstdint>

#include "openscarlett/core/status.h"
#include "openscarlett/io/bcpack.h"

namespace openscarlett {

// Decodes a headerless Xbox Series X|S hardware DirectStorage MDU BCPack bitstream
// (`DSTORAGE_BCPACK_MODE` 1=BC1, 3=BC3, 4=BC4, 5=BC5, 6=BC6H, 7=BC7) into `dst_size`
// bytes of standard BCn blocks (in GFX10 tiled memory order).
//
// Guarantees:
//   - Rejects `BcFormat::BC2` and unknown formats with `StatusCode::UnsupportedFormat`.
//   - Rejects null/misaligned destination buffers with `StatusCode::InvalidArgument`.
//   - Rejects empty, truncated, corrupt, FIN-slot-violating, non-home-ending, or
//     trailing-nonzero-padded bitstreams with `StatusCode::ParseError`.
//   - Never writes partial or unvalidated data to `dst_blocks` on error.
Status BcPackDecodeXboxMduBitstream(
    BcFormat format,
    const uint8_t* src_bitstream,
    size_t src_size,
    uint8_t* dst_blocks,
    size_t dst_size,
    size_t* out_written = nullptr);

}  // namespace openscarlett
