#pragma once

#include <string>

namespace xuesu_book {

// Imports a supported source into a complete EPUB in its private cache directory.
bool buildArchive(const std::string& source, const std::string& archive);

}  // namespace xuesu_book
