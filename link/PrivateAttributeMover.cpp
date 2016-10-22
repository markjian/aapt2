/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "link/Linkers.h"

#include <algorithm>
#include <iterator>

#include "android-base/logging.h"

#include "ResourceTable.h"

namespace aapt {

template <typename InputContainer, typename OutputIterator, typename Predicate>
OutputIterator move_if(InputContainer& input_container, OutputIterator result,
                       Predicate pred) {
  const auto last = input_container.end();
  auto new_end =
      std::find_if(input_container.begin(), input_container.end(), pred);
  if (new_end == last) {
    return result;
  }

  *result = std::move(*new_end);

  auto first = new_end;
  ++first;

  for (; first != last; ++first) {
    if (bool(pred(*first))) {
      // We want to move this guy
      *result = std::move(*first);
      ++result;
    } else {
      // We want to keep this guy, but we will need to move it up the list to
      // replace missing items.
      *new_end = std::move(*first);
      ++new_end;
    }
  }

  input_container.erase(new_end, last);
  return result;
}

bool PrivateAttributeMover::Consume(IAaptContext* context,
                                    ResourceTable* table) {
  for (auto& package : table->packages) {
    ResourceTableType* type = package->FindType(ResourceType::kAttr);
    if (!type) {
      continue;
    }

    if (type->symbol_status.state != SymbolState::kPublic) {
      // No public attributes, so we can safely leave these private attributes
      // where they are.
      return true;
    }

    ResourceTableType* priv_attr_type =
        package->FindOrCreateType(ResourceType::kAttrPrivate);
    CHECK(priv_attr_type->entries.empty());

    move_if(type->entries, std::back_inserter(priv_attr_type->entries),
            [](const std::unique_ptr<ResourceEntry>& entry) -> bool {
              return entry->symbol_status.state != SymbolState::kPublic;
            });
    break;
  }
  return true;
}

}  // namespace aapt
