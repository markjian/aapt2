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

#include "Maybe.h"
#include "Resolver.h"
#include "Resource.h"
#include "ResourceTable.h"
#include "ResourceValues.h"
#include "Util.h"

#include <androidfw/AssetManager.h>
#include <androidfw/ResourceTypes.h>
#include <memory>
#include <vector>

namespace aapt {

Resolver::Resolver(std::shared_ptr<const ResourceTable> table,
                   std::shared_ptr<const android::AssetManager> sources) :
        mTable(table), mSources(sources) {
}

Maybe<ResourceId> Resolver::findId(const ResourceName& name) {
    Maybe<Entry> result = findAttribute(name);
    if (result) {
        return result.value().id;
    }
    return {};
}

Maybe<Resolver::Entry> Resolver::findAttribute(const ResourceName& name) {
    auto cacheIter = mCache.find(name);
    if (cacheIter != std::end(mCache)) {
        return Entry{ cacheIter->second.id, cacheIter->second.attr.get() };
    }

    const ResourceTableType* type;
    const ResourceEntry* entry;
    std::tie(type, entry) = mTable->findResource(name);
    if (type && entry) {
        Entry result = {};
        if (mTable->getPackageId() != ResourceTable::kUnsetPackageId &&
                type->typeId != ResourceTableType::kUnsetTypeId &&
                entry->entryId != ResourceEntry::kUnsetEntryId) {
            result.id = ResourceId(mTable->getPackageId(), type->typeId, entry->entryId);
        }

        if (!entry->values.empty()) {
            visitFunc<Attribute>(*entry->values.front().value, [&result](Attribute& attr) {
                    result.attr = &attr;
            });
        }
        return result;
    }

    const CacheEntry* cacheEntry = buildCacheEntry(name);
    if (cacheEntry) {
        return Entry{ cacheEntry->id, cacheEntry->attr.get() };
    }
    return {};
}

/**
 * This is called when we need to lookup a resource name in the AssetManager.
 * Since the values in the AssetManager are not parsed like in a ResourceTable,
 * we must create Attribute objects here if we find them.
 */
const Resolver::CacheEntry* Resolver::buildCacheEntry(const ResourceName& name) {
    const android::ResTable& table = mSources->getResources(false);

    const StringPiece16 type16 = toString(name.type);
    ResourceId resId {
        table.identifierForName(
                name.entry.data(), name.entry.size(),
                type16.data(), type16.size(),
                name.package.data(), name.package.size())
    };

    if (!resId.isValid()) {
        return nullptr;
    }

    CacheEntry& entry = mCache[name];
    entry.id = resId;

    //
    // Now check to see if this resource is an Attribute.
    //

    const android::ResTable::bag_entry* bagBegin;
    ssize_t bags = table.lockBag(resId.id, &bagBegin);
    if (bags < 1) {
        table.unlockBag(bagBegin);
        return &entry;
    }

    // Look for the ATTR_TYPE key in the bag and check the types it supports.
    uint32_t attrTypeMask = 0;
    for (ssize_t i = 0; i < bags; i++) {
        if (bagBegin[i].map.name.ident == android::ResTable_map::ATTR_TYPE) {
            attrTypeMask = bagBegin[i].map.value.data;
        }
    }

    entry.attr = util::make_unique<Attribute>(false);

    if (attrTypeMask & android::ResTable_map::TYPE_ENUM ||
            attrTypeMask & android::ResTable_map::TYPE_FLAGS) {
        for (ssize_t i = 0; i < bags; i++) {
            if (Res_INTERNALID(bagBegin[i].map.name.ident)) {
                // Internal IDs are special keys, which are not enum/flag symbols, so skip.
                continue;
            }

            android::ResTable::resource_name symbolName;
            bool result = table.getResourceName(bagBegin[i].map.name.ident, false,
                    &symbolName);
            assert(result);
            const ResourceType* type = parseResourceType(
                    StringPiece16(symbolName.type, symbolName.typeLen));
            assert(type);

            entry.attr->symbols.push_back(Attribute::Symbol{
                    Reference(ResourceNameRef(
                                StringPiece16(symbolName.package, symbolName.packageLen),
                                *type,
                                StringPiece16(symbolName.name, symbolName.nameLen))),
                            bagBegin[i].map.value.data
            });
        }
    }

    entry.attr->typeMask |= attrTypeMask;
    table.unlockBag(bagBegin);
    return &entry;
}

} // namespace aapt
