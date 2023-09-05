//
// Copyright 2019 Google LLC
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
//

#include "zetasql/common/function_signature_testutil.h"

#include <vector>

#include "zetasql/public/function.pb.h"

namespace zetasql {

// TODO: Update this test case to test ANY_K and ARRAY_ANY_K for k
// = 4, 5.
std::vector<SignatureArgumentKindGroup> GetRelatedSignatureArgumentGroup() {
  return {
      {.kind = ARG_TYPE_ANY_1, .array_kind = ARG_ARRAY_TYPE_ANY_1},
      {.kind = ARG_TYPE_ANY_2, .array_kind = ARG_ARRAY_TYPE_ANY_2},
      {.kind = ARG_TYPE_ANY_3, .array_kind = ARG_ARRAY_TYPE_ANY_3},
  };
}

}  // namespace zetasql
