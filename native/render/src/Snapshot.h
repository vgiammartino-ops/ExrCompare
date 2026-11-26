// Snapshot export: burn-in text drawn with Direct2D/DirectWrite onto the
// rendered texture, then encoded to PNG or JPEG with WIC.
#pragma once

#include <d3d11.h>

#include <cstdint>

#include <string>
#include <vector>

namespace exrc {

struct Overlay {
    std::vector<std::wstring> burnIn;  // lines, drawn bottom-left
};

// Draws onto a BGRA render-target texture (device needs D3D11_CREATE_DEVICE_BGRA_SUPPORT).
void drawOverlay(ID3D11Texture2D* texture, const Overlay& overlay);

// Writes tightly packed BGRA pixels. jpeg=false -> PNG.
void writeImage(const std::wstring& path, const uint8_t* bgra, int width, int height, bool jpeg);

}  // namespace exrc
