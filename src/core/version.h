// =============================================================================
// src/core/version.h — the release this binary was built from
// =============================================================================
// A function rather than a FUNES_VERSION macro: the string comes from
// project() in CMakeLists.txt, through a header generated for version.cpp
// alone.
// As a PUBLIC definition on funes_core it reached every translation unit, so a
// version bump rebuilt the whole tree, tests included, to change two lines.

#pragma once

namespace funes {

const char* version();

} // namespace funes
