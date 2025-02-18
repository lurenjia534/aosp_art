/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tools/tools.h"

#include <errno.h>
#include <fnmatch.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "android-base/function_ref.h"
#include "android-base/logging.h"
#include "android-base/result.h"
#include "android-base/strings.h"
#include "base/macros.h"
#include "fstab/fstab.h"

namespace art {
namespace tools {

namespace {

using ::android::base::ConsumeSuffix;
using ::android::base::function_ref;
using ::android::base::Result;
using ::android::base::StartsWith;
using ::android::fs_mgr::Fstab;
using ::android::fs_mgr::FstabEntry;
using ::android::fs_mgr::ReadFstabFromProcMounts;
using ::std::placeholders::_1;

// Returns true if `path_prefix` matches `pattern` or can be a prefix of a path that matches
// `pattern` (i.e., `path_prefix` represents a directory that may contain a file whose path matches
// `pattern`).
bool PartialMatch(const std::filesystem::path& pattern, const std::filesystem::path& path_prefix) {
  for (std::filesystem::path::const_iterator pattern_it = pattern.begin(),
                                             path_prefix_it = path_prefix.begin();
       ;  // NOLINT
       pattern_it++, path_prefix_it++) {
    if (path_prefix_it == path_prefix.end()) {
      return true;
    }
    if (pattern_it == pattern.end()) {
      return false;
    }
    if (*pattern_it == "**") {
      return true;
    }
    if (fnmatch(pattern_it->c_str(), path_prefix_it->c_str(), /*flags=*/0) != 0) {
      return false;
    }
  }
}

bool FullMatchRecursive(const std::filesystem::path& pattern,
                        std::filesystem::path::const_iterator pattern_it,
                        const std::filesystem::path& path,
                        std::filesystem::path::const_iterator path_it,
                        bool double_asterisk_visited = false) {
  if (pattern_it == pattern.end() && path_it == path.end()) {
    return true;
  }
  if (pattern_it == pattern.end()) {
    return false;
  }
  if (*pattern_it == "**") {
    DCHECK(!double_asterisk_visited);
    std::filesystem::path::const_iterator next_pattern_it = pattern_it;
    return FullMatchRecursive(
               pattern, ++next_pattern_it, path, path_it, /*double_asterisk_visited=*/true) ||
           (path_it != path.end() && FullMatchRecursive(pattern, pattern_it, path, ++path_it));
  }
  if (path_it == path.end()) {
    return false;
  }
  if (fnmatch(pattern_it->c_str(), path_it->c_str(), /*flags=*/0) != 0) {
    return false;
  }
  return FullMatchRecursive(pattern, ++pattern_it, path, ++path_it);
}

// Returns true if `path` fully matches `pattern`.
bool FullMatch(const std::filesystem::path& pattern, const std::filesystem::path& path) {
  return FullMatchRecursive(pattern, pattern.begin(), path, path.begin());
}

void MatchGlobRecursive(const std::vector<std::filesystem::path>& patterns,
                        const std::filesystem::path& root_dir,
                        /*out*/ std::vector<std::string>* results) {
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           root_dir, std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && it != std::filesystem::end(it);
       it.increment(ec)) {
    const std::filesystem::directory_entry& entry = *it;
    if (std::none_of(patterns.begin(), patterns.end(), std::bind(PartialMatch, _1, entry.path()))) {
      // Avoid unnecessary I/O and SELinux denials.
      it.disable_recursion_pending();
      continue;
    }
    std::error_code ec2;
    if (entry.is_regular_file(ec2) &&
        std::any_of(patterns.begin(), patterns.end(), std::bind(FullMatch, _1, entry.path()))) {
      results->push_back(entry.path());
    }
    if (ec2) {
      // It's expected that we don't have permission to stat some dirs/files, and we don't care
      // about them.
      if (ec2.value() != EACCES) {
        LOG(ERROR) << ART_FORMAT("Unable to lstat '{}': {}", entry.path().string(), ec2.message());
      }
      continue;
    }
  }
  if (ec) {
    LOG(ERROR) << ART_FORMAT("Unable to walk through '{}': {}", root_dir.string(), ec.message());
  }
}

}  // namespace

std::vector<std::string> Glob(const std::vector<std::string>& patterns, std::string_view root_dir) {
  std::vector<std::filesystem::path> parsed_patterns;
  parsed_patterns.reserve(patterns.size());
  for (std::string_view pattern : patterns) {
    parsed_patterns.emplace_back(pattern);
  }
  std::vector<std::string> results;
  MatchGlobRecursive(parsed_patterns, root_dir, &results);
  return results;
}

std::string EscapeGlob(const std::string& str) {
  return std::regex_replace(str, std::regex(R"re(\*|\?|\[)re"), "[$&]");
}

bool PathStartsWith(std::string_view path, std::string_view prefix) {
  CHECK(!prefix.empty() && !path.empty() && prefix[0] == '/' && path[0] == '/')
      << ART_FORMAT("path={}, prefix={}", path, prefix);
  ConsumeSuffix(&prefix, "/");
  return StartsWith(path, prefix) &&
         (path.length() == prefix.length() || path[prefix.length()] == '/');
}

static Result<std::vector<FstabEntry>> GetProcMountsMatches(
    function_ref<bool(std::string_view)> predicate) {
  Fstab fstab;
  if (!ReadFstabFromProcMounts(&fstab)) {
    return Errorf("Failed to read fstab from /proc/mounts");
  }
  std::vector<FstabEntry> entries;
  for (FstabEntry& entry : fstab) {
    // Ignore swap areas as a swap area doesn't have a meaningful `mount_point` (a.k.a., `fs_file`)
    // field, according to fstab(5). In addition, ignore any other entries whose mount points are
    // not absolute paths, just in case there are other fs types that also have an meaningless mount
    // point.
    if (entry.fs_type == "swap" || !StartsWith(entry.mount_point, '/')) {
      continue;
    }
    if (predicate(entry.mount_point)) {
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

Result<std::vector<FstabEntry>> GetProcMountsAncestorsOfPath(std::string_view path) {
  return GetProcMountsMatches(
      [&](std::string_view mount_point) { return PathStartsWith(path, mount_point); });
}

Result<std::vector<FstabEntry>> GetProcMountsDescendantsOfPath(std::string_view path) {
  return GetProcMountsMatches(
      [&](std::string_view mount_point) { return PathStartsWith(mount_point, path); });
}

}  // namespace tools
}  // namespace art
