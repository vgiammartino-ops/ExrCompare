// OpenEXR access: header info, metadata, AOV discovery (single/multi-part, stereo views),
// and decoding one AOV of one frame into a Frame.
#pragma once

#include "exrc/Types.h"

namespace exrc {

// OpenEXR's internal thread pool size (per-file line-block decoding). 0 = none.
void setExrThreadCount(int threads);

// Builds the AOV list from (part name, channel names). Exposed for unit tests.
struct PartChannels {
    std::string partName;
    std::string view;  // "view" attribute of the part (multi-part stereo), "" if none
    std::vector<std::string> channels;
    bool deep = false;
};
// multiView: the file's "multiView" attribute (empty for mono files); view: view to keep.
std::vector<LayerInfo> buildLayers(const std::vector<PartChannels>& parts,
                                   const std::vector<std::string>& multiView = {},
                                   const std::string& view = {});

// Guesses what an AOV holds from its name (N, P, Z, depth, motion, crypto, id, uv ...).
AovKind classifyAov(const std::string& name);

// Reads header info, metadata and the AOV list. Throws std::exception on unreadable files.
// view: stereo view to list AOVs for ("" = default / first view).
ImageInfo readImageInfo(const fs::path& file, const std::string& view, std::vector<LayerInfo>& layersOut);

struct DecodeOptions {
    bool proxy = false;         // half resolution (reads mip level 1 when the file has one)
    std::string alphaOverride;  // channel to use as alpha instead of the layer's own ("" = none)
};

// Decodes one AOV of one file. Never throws: failures return a frame with missing=true.
FramePtr decodeFrame(const fs::path& file, const LayerInfo& layer, int frameNumber, uint32_t key,
                     const DecodeOptions& options = {});

FramePtr makeMissingFrame(int frameNumber, uint32_t key, std::string reason = {});

float halfToFloat(uint16_t bits);
uint16_t floatToHalf(float value);

}  // namespace exrc
