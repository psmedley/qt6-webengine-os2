// Copyright 2022 The Tint Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>

#include "src/tint/utils/string.h"
#include "src/tint/utils/vector.h"

namespace tint::utils {

size_t Distance(const std::string& str_a, const std::string& str_b) {
    const auto len_a = str_a.size();
    const auto len_b = str_b.size();

    Vector<size_t, 64> mat;
    mat.Reserve((len_a + 1) * (len_b + 1));

    auto at = [&](size_t a, size_t b) -> size_t& { return mat[a + b * (len_a + 1)]; };

    at(0, 0) = 0;
    for (size_t a = 1; a <= len_a; a++) {
        at(a, 0) = a;
    }
    for (size_t b = 1; b <= len_b; b++) {
        at(0, b) = b;
    }
    for (size_t b = 1; b <= len_b; b++) {
        for (size_t a = 1; a <= len_a; a++) {
            bool eq = str_a[a - 1] == str_b[b - 1];
            at(a, b) = std::min({
                at(a - 1, b) + 1,
                at(a, b - 1) + 1,
                at(a - 1, b - 1) + (eq ? 0 : 1),
            });
        }
    }
    return at(len_a, len_b);
}

}  // namespace tint::utils
