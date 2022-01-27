// Copyright 2017 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef RIEGELI_BYTES_FD_INTERNAL_H_
#define RIEGELI_BYTES_FD_INTERNAL_H_

#include <string>

#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

namespace riegeli {
namespace fd_internal {

// Returns the `assumed_filename`. If `absl::nullopt`, then "/dev/stdin",
// "/dev/stdout", "/dev/stderr", or "/proc/self/fd/<fd>" is inferred from `fd`.
std::string ResolveFilename(int fd,
                            absl::optional<std::string>&& assumed_filename);

// Closes a file descriptor, taking interruption by signals into account.
//
// Return value:
//  * 0  - success
//  * -1 - failure (`errno` is set, `fd` is closed anyway)
int Close(int fd);

extern const absl::string_view kCloseFunctionName;

}  // namespace fd_internal
}  // namespace riegeli

#endif  // RIEGELI_BYTES_FD_INTERNAL_H_
