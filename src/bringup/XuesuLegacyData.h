#pragma once

#include <optional>
#include <string_view>

namespace xuesu_legacy {
std::optional<float> recentProgress(std::string_view bookPath);
// Returns the number of added books, or -1 if the legacy data could not be loaded or saved.
int importRecentBooks();
}  // namespace xuesu_legacy
