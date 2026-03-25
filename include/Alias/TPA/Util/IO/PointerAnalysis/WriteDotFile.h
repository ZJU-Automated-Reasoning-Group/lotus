#pragma once

namespace tpa {
class CFG;
} // namespace tpa

namespace util {
namespace io {

void writeDotFile(const char *filePath, const tpa::CFG &cfg);

} // namespace io
} // namespace util
