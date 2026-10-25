// Movie pictures (movie.cpp): the guest's player opens a stream, asks for the frame that is due
// each vertical blank, and closes it; the renderer shows the picture on the top screen meanwhile.
#pragma once

#include <cstdint>
#include <string>

namespace khvr::hle {

bool MovieOpen(const std::string &path);  // a NitroFS path, e.g. "mv/802.mods"
void MovieShowFrame(uint32_t frame);
void MovieClose();
// The latest picture (RGBA, 256 x 192); false while no movie has a picture.
bool MovieCopyPicture(uint32_t *rgba256x192);

}  // namespace khvr::hle
