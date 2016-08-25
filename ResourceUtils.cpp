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

#include "NameMangler.h"
#include "ResourceUtils.h"
#include "SdkConstants.h"
#include "flatten/ResourceTypeExtensions.h"
#include "util/Files.h"
#include "util/Util.h"

#include <androidfw/ResourceTypes.h>
#include <sstream>

namespace aapt {
namespace ResourceUtils {

Maybe<ResourceName> toResourceName(const android::ResTable::resource_name& nameIn) {
    ResourceName nameOut;
    if (!nameIn.package) {
        return {};
    }

    nameOut.package = util::utf16ToUtf8(StringPiece16(nameIn.package, nameIn.packageLen));

    const ResourceType* type;
    if (nameIn.type) {
        type = parseResourceType(util::utf16ToUtf8(StringPiece16(nameIn.type, nameIn.typeLen)));
    } else if (nameIn.type8) {
        type = parseResourceType(StringPiece(nameIn.type8, nameIn.typeLen));
    } else {
        return {};
    }

    if (!type) {
        return {};
    }

    nameOut.type = *type;

    if (nameIn.name) {
        nameOut.entry = util::utf16ToUtf8(StringPiece16(nameIn.name, nameIn.nameLen));
    } else if (nameIn.name8) {
        nameOut.entry = StringPiece(nameIn.name8, nameIn.nameLen).toString();
    } else {
        return {};
    }
    return nameOut;
}

bool extractResourceName(const StringPiece& str, StringPiece* outPackage,
                         StringPiece* outType, StringPiece* outEntry) {
    bool hasPackageSeparator = false;
    bool hasTypeSeparator = false;
    const char* start = str.data();
    const char* end = start + str.size();
    const char* current = start;
    while (current != end) {
        if (outType->size() == 0 && *current == '/') {
            hasTypeSeparator = true;
            outType->assign(start, current - start);
            start = current + 1;
        } else if (outPackage->size() == 0 && *current == ':') {
            hasPackageSeparator = true;
            outPackage->assign(start, current - start);
            start = current + 1;
        }
        current++;
    }
    outEntry->assign(start, end - start);

    return !(hasPackageSeparator && outPackage->empty()) && !(hasTypeSeparator && outType->empty());
}

bool parseResourceName(const StringPiece& str, ResourceNameRef* outRef, bool* outPrivate) {
    if (str.empty()) {
        return false;
    }

    size_t offset = 0;
    bool priv = false;
    if (str.data()[0] == '*') {
        priv = true;
        offset = 1;
    }

    StringPiece package;
    StringPiece type;
    StringPiece entry;
    if (!extractResourceName(str.substr(offset, str.size() - offset), &package, &type, &entry)) {
        return false;
    }

    const ResourceType* parsedType = parseResourceType(type);
    if (!parsedType) {
        return false;
    }

    if (entry.empty()) {
        return false;
    }

    if (outRef) {
        outRef->package = package;
        outRef->type = *parsedType;
        outRef->entry = entry;
    }

    if (outPrivate) {
        *outPrivate = priv;
    }
    return true;
}

bool parseReference(const StringPiece& str, ResourceNameRef* outRef, bool* outCreate,
                       bool* outPrivate) {
    StringPiece trimmedStr(util::trimWhitespace(str));
    if (trimmedStr.empty()) {
        return false;
    }

    bool create = false;
    bool priv = false;
    if (trimmedStr.data()[0] == '@') {
        size_t offset = 1;
        if (trimmedStr.data()[1] == '+') {
            create = true;
            offset += 1;
        }

        ResourceNameRef name;
        if (!parseResourceName(trimmedStr.substr(offset, trimmedStr.size() - offset),
                               &name, &priv)) {
            return false;
        }

        if (create && priv) {
            return false;
        }

        if (create && name.type != ResourceType::kId) {
            return false;
        }

        if (outRef) {
            *outRef = name;
        }

        if (outCreate) {
            *outCreate = create;
        }

        if (outPrivate) {
            *outPrivate = priv;
        }
        return true;
    }
    return false;
}

bool isReference(const StringPiece& str) {
    return parseReference(str, nullptr, nullptr, nullptr);
}

bool parseAttributeReference(const StringPiece& str, ResourceNameRef* outRef) {
    StringPiece trimmedStr(util::trimWhitespace(str));
    if (trimmedStr.empty()) {
        return false;
    }

    if (*trimmedStr.data() == '?') {
        StringPiece package;
        StringPiece type;
        StringPiece entry;
        if (!extractResourceName(trimmedStr.substr(1, trimmedStr.size() - 1),
                                 &package, &type, &entry)) {
            return false;
        }

        if (!type.empty() && type != "attr") {
            return false;
        }

        if (entry.empty()) {
            return false;
        }

        if (outRef) {
            outRef->package = package;
            outRef->type = ResourceType::kAttr;
            outRef->entry = entry;
        }
        return true;
    }
    return false;
}

bool isAttributeReference(const StringPiece& str) {
    return parseAttributeReference(str, nullptr);
}

/*
 * Style parent's are a bit different. We accept the following formats:
 *
 * @[[*]package:][style/]<entry>
 * ?[[*]package:]style/<entry>
 * <[*]package>:[style/]<entry>
 * [[*]package:style/]<entry>
 */
Maybe<Reference> parseStyleParentReference(const StringPiece& str, std::string* outError) {
    if (str.empty()) {
        return {};
    }

    StringPiece name = str;

    bool hasLeadingIdentifiers = false;
    bool privateRef = false;

    // Skip over these identifiers. A style's parent is a normal reference.
    if (name.data()[0] == '@' || name.data()[0] == '?') {
        hasLeadingIdentifiers = true;
        name = name.substr(1, name.size() - 1);
    }

    if (name.data()[0] == '*') {
        privateRef = true;
        name = name.substr(1, name.size() - 1);
    }

    ResourceNameRef ref;
    ref.type = ResourceType::kStyle;

    StringPiece typeStr;
    extractResourceName(name, &ref.package, &typeStr, &ref.entry);
    if (!typeStr.empty()) {
        // If we have a type, make sure it is a Style.
        const ResourceType* parsedType = parseResourceType(typeStr);
        if (!parsedType || *parsedType != ResourceType::kStyle) {
            std::stringstream err;
            err << "invalid resource type '" << typeStr << "' for parent of style";
            *outError = err.str();
            return {};
        }
    }

    if (!hasLeadingIdentifiers && ref.package.empty() && !typeStr.empty()) {
        std::stringstream err;
        err << "invalid parent reference '" << str << "'";
        *outError = err.str();
        return {};
    }

    Reference result(ref);
    result.privateReference = privateRef;
    return result;
}

Maybe<Reference> parseXmlAttributeName(const StringPiece& str) {
    StringPiece trimmedStr = util::trimWhitespace(str);
    const char* start = trimmedStr.data();
    const char* const end = start + trimmedStr.size();
    const char* p = start;

    Reference ref;
    if (p != end && *p == '*') {
        ref.privateReference = true;
        start++;
        p++;
    }

    StringPiece package;
    StringPiece name;
    while (p != end) {
        if (*p == ':') {
            package = StringPiece(start, p - start);
            name = StringPiece(p + 1, end - (p + 1));
            break;
        }
        p++;
    }

    ref.name = ResourceName(package.toString(), ResourceType::kAttr,
                        name.empty() ? trimmedStr.toString() : name.toString());
    return Maybe<Reference>(std::move(ref));
}

std::unique_ptr<Reference> tryParseReference(const StringPiece& str, bool* outCreate) {
    ResourceNameRef ref;
    bool privateRef = false;
    if (parseReference(str, &ref, outCreate, &privateRef)) {
        std::unique_ptr<Reference> value = util::make_unique<Reference>(ref);
        value->privateReference = privateRef;
        return value;
    }

    if (parseAttributeReference(str, &ref)) {
        if (outCreate) {
            *outCreate = false;
        }
        return util::make_unique<Reference>(ref, Reference::Type::kAttribute);
    }
    return {};
}

std::unique_ptr<BinaryPrimitive> tryParseNullOrEmpty(const StringPiece& str) {
    StringPiece trimmedStr(util::trimWhitespace(str));
    android::Res_value value = { };
    if (trimmedStr == "@null") {
        // TYPE_NULL with data set to 0 is interpreted by the runtime as an error.
        // Instead we set the data type to TYPE_REFERENCE with a value of 0.
        value.dataType = android::Res_value::TYPE_REFERENCE;
    } else if (trimmedStr == "@empty") {
        // TYPE_NULL with value of DATA_NULL_EMPTY is handled fine by the runtime.
        value.dataType = android::Res_value::TYPE_NULL;
        value.data = android::Res_value::DATA_NULL_EMPTY;
    } else {
        return {};
    }
    return util::make_unique<BinaryPrimitive>(value);
}

std::unique_ptr<BinaryPrimitive> tryParseEnumSymbol(const Attribute* enumAttr,
                                                    const StringPiece& str) {
    StringPiece trimmedStr(util::trimWhitespace(str));
    for (const Attribute::Symbol& symbol : enumAttr->symbols) {
        // Enum symbols are stored as @package:id/symbol resources,
        // so we need to match against the 'entry' part of the identifier.
        const ResourceName& enumSymbolResourceName = symbol.symbol.name.value();
        if (trimmedStr == enumSymbolResourceName.entry) {
            android::Res_value value = { };
            value.dataType = android::Res_value::TYPE_INT_DEC;
            value.data = symbol.value;
            return util::make_unique<BinaryPrimitive>(value);
        }
    }
    return {};
}

std::unique_ptr<BinaryPrimitive> tryParseFlagSymbol(const Attribute* flagAttr,
                                                    const StringPiece& str) {
    android::Res_value flags = { };
    flags.dataType = android::Res_value::TYPE_INT_HEX;
    flags.data = 0u;

    if (util::trimWhitespace(str).empty()) {
        // Empty string is a valid flag (0).
        return util::make_unique<BinaryPrimitive>(flags);
    }

    for (StringPiece part : util::tokenize(str, '|')) {
        StringPiece trimmedPart = util::trimWhitespace(part);

        bool flagSet = false;
        for (const Attribute::Symbol& symbol : flagAttr->symbols) {
            // Flag symbols are stored as @package:id/symbol resources,
            // so we need to match against the 'entry' part of the identifier.
            const ResourceName& flagSymbolResourceName = symbol.symbol.name.value();
            if (trimmedPart == flagSymbolResourceName.entry) {
                flags.data |= symbol.value;
                flagSet = true;
                break;
            }
        }

        if (!flagSet) {
            return {};
        }
    }
    return util::make_unique<BinaryPrimitive>(flags);
}

static uint32_t parseHex(char c, bool* outError) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'a' && c <= 'f') {
        return c - 'a' + 0xa;
    } else if (c >= 'A' && c <= 'F') {
        return c - 'A' + 0xa;
    } else {
        *outError = true;
        return 0xffffffffu;
    }
}

std::unique_ptr<BinaryPrimitive> tryParseColor(const StringPiece& str) {
    StringPiece colorStr(util::trimWhitespace(str));
    const char* start = colorStr.data();
    const size_t len = colorStr.size();
    if (len == 0 || start[0] != '#') {
        return {};
    }

    android::Res_value value = { };
    bool error = false;
    if (len == 4) {
        value.dataType = android::Res_value::TYPE_INT_COLOR_RGB4;
        value.data = 0xff000000u;
        value.data |= parseHex(start[1], &error) << 20;
        value.data |= parseHex(start[1], &error) << 16;
        value.data |= parseHex(start[2], &error) << 12;
        value.data |= parseHex(start[2], &error) << 8;
        value.data |= parseHex(start[3], &error) << 4;
        value.data |= parseHex(start[3], &error);
    } else if (len == 5) {
        value.dataType = android::Res_value::TYPE_INT_COLOR_ARGB4;
        value.data |= parseHex(start[1], &error) << 28;
        value.data |= parseHex(start[1], &error) << 24;
        value.data |= parseHex(start[2], &error) << 20;
        value.data |= parseHex(start[2], &error) << 16;
        value.data |= parseHex(start[3], &error) << 12;
        value.data |= parseHex(start[3], &error) << 8;
        value.data |= parseHex(start[4], &error) << 4;
        value.data |= parseHex(start[4], &error);
    } else if (len == 7) {
        value.dataType = android::Res_value::TYPE_INT_COLOR_RGB8;
        value.data = 0xff000000u;
        value.data |= parseHex(start[1], &error) << 20;
        value.data |= parseHex(start[2], &error) << 16;
        value.data |= parseHex(start[3], &error) << 12;
        value.data |= parseHex(start[4], &error) << 8;
        value.data |= parseHex(start[5], &error) << 4;
        value.data |= parseHex(start[6], &error);
    } else if (len == 9) {
        value.dataType = android::Res_value::TYPE_INT_COLOR_ARGB8;
        value.data |= parseHex(start[1], &error) << 28;
        value.data |= parseHex(start[2], &error) << 24;
        value.data |= parseHex(start[3], &error) << 20;
        value.data |= parseHex(start[4], &error) << 16;
        value.data |= parseHex(start[5], &error) << 12;
        value.data |= parseHex(start[6], &error) << 8;
        value.data |= parseHex(start[7], &error) << 4;
        value.data |= parseHex(start[8], &error);
    } else {
        return {};
    }
    return error ? std::unique_ptr<BinaryPrimitive>() : util::make_unique<BinaryPrimitive>(value);
}

Maybe<bool> parseBool(const StringPiece& str) {
    StringPiece trimmedStr(util::trimWhitespace(str));
    if (trimmedStr == "true" || trimmedStr == "TRUE" || trimmedStr == "True") {
        return Maybe<bool>(true);
    } else if (trimmedStr == "false" || trimmedStr == "FALSE" || trimmedStr == "False") {
        return Maybe<bool>(false);
    }
    return {};
}

Maybe<uint32_t> parseInt(const StringPiece& str) {
    std::u16string str16 = util::utf8ToUtf16(str);
    android::Res_value value;
    if (android::ResTable::stringToInt(str16.data(), str16.size(), &value)) {
        return value.data;
    }
    return {};
}

Maybe<ResourceId> parseResourceId(const StringPiece& str) {
    StringPiece trimmedStr(util::trimWhitespace(str));

    std::u16string str16 = util::utf8ToUtf16(trimmedStr);
    android::Res_value value;
    if (android::ResTable::stringToInt(str16.data(), str16.size(), &value)) {
        if (value.dataType == android::Res_value::TYPE_INT_HEX) {
            ResourceId id(value.data);
            if (id.isValid()) {
                return id;
            }
        }
    }
    return {};
}

Maybe<int> parseSdkVersion(const StringPiece& str) {
    StringPiece trimmedStr(util::trimWhitespace(str));

    std::u16string str16 = util::utf8ToUtf16(trimmedStr);
    android::Res_value value;
    if (android::ResTable::stringToInt(str16.data(), str16.size(), &value)) {
        return static_cast<int>(value.data);
    }

    // Try parsing the code name.
    std::pair<StringPiece, int> entry = getDevelopmentSdkCodeNameAndVersion();
    if (entry.first == trimmedStr) {
        return entry.second;
    }
    return {};
}

std::unique_ptr<BinaryPrimitive> tryParseBool(const StringPiece& str) {
    if (Maybe<bool> maybeResult = parseBool(str)) {
        android::Res_value value = {};
        value.dataType = android::Res_value::TYPE_INT_BOOLEAN;

        if (maybeResult.value()) {
            value.data = 0xffffffffu;
        } else {
            value.data = 0;
        }
        return util::make_unique<BinaryPrimitive>(value);
    }
    return {};
}

std::unique_ptr<BinaryPrimitive> tryParseInt(const StringPiece& str) {
    std::u16string str16 = util::utf8ToUtf16(str);
    android::Res_value value;
    if (!android::ResTable::stringToInt(str16.data(), str16.size(), &value)) {
        return {};
    }
    return util::make_unique<BinaryPrimitive>(value);
}

std::unique_ptr<BinaryPrimitive> tryParseFloat(const StringPiece& str) {
    std::u16string str16 = util::utf8ToUtf16(str);
    android::Res_value value;
    if (!android::ResTable::stringToFloat(str16.data(), str16.size(), &value)) {
        return {};
    }
    return util::make_unique<BinaryPrimitive>(value);
}

uint32_t androidTypeToAttributeTypeMask(uint16_t type) {
    switch (type) {
    case android::Res_value::TYPE_NULL:
    case android::Res_value::TYPE_REFERENCE:
    case android::Res_value::TYPE_ATTRIBUTE:
    case android::Res_value::TYPE_DYNAMIC_REFERENCE:
        return android::ResTable_map::TYPE_REFERENCE;

    case android::Res_value::TYPE_STRING:
        return android::ResTable_map::TYPE_STRING;

    case android::Res_value::TYPE_FLOAT:
        return android::ResTable_map::TYPE_FLOAT;

    case android::Res_value::TYPE_DIMENSION:
        return android::ResTable_map::TYPE_DIMENSION;

    case android::Res_value::TYPE_FRACTION:
        return android::ResTable_map::TYPE_FRACTION;

    case android::Res_value::TYPE_INT_DEC:
    case android::Res_value::TYPE_INT_HEX:
        return android::ResTable_map::TYPE_INTEGER | android::ResTable_map::TYPE_ENUM
                | android::ResTable_map::TYPE_FLAGS;

    case android::Res_value::TYPE_INT_BOOLEAN:
        return android::ResTable_map::TYPE_BOOLEAN;

    case android::Res_value::TYPE_INT_COLOR_ARGB8:
    case android::Res_value::TYPE_INT_COLOR_RGB8:
    case android::Res_value::TYPE_INT_COLOR_ARGB4:
    case android::Res_value::TYPE_INT_COLOR_RGB4:
        return android::ResTable_map::TYPE_COLOR;

    default:
        return 0;
    };
}

std::unique_ptr<Item> tryParseItemForAttribute(
        const StringPiece& value,
        uint32_t typeMask,
        const std::function<void(const ResourceName&)>& onCreateReference) {
    std::unique_ptr<BinaryPrimitive> nullOrEmpty = tryParseNullOrEmpty(value);
    if (nullOrEmpty) {
        return std::move(nullOrEmpty);
    }

    bool create = false;
    std::unique_ptr<Reference> reference = tryParseReference(value, &create);
    if (reference) {
        if (create && onCreateReference) {
            onCreateReference(reference->name.value());
        }
        return std::move(reference);
    }

    if (typeMask & android::ResTable_map::TYPE_COLOR) {
        // Try parsing this as a color.
        std::unique_ptr<BinaryPrimitive> color = tryParseColor(value);
        if (color) {
            return std::move(color);
        }
    }

    if (typeMask & android::ResTable_map::TYPE_BOOLEAN) {
        // Try parsing this as a boolean.
        std::unique_ptr<BinaryPrimitive> boolean = tryParseBool(value);
        if (boolean) {
            return std::move(boolean);
        }
    }

    if (typeMask & android::ResTable_map::TYPE_INTEGER) {
        // Try parsing this as an integer.
        std::unique_ptr<BinaryPrimitive> integer = tryParseInt(value);
        if (integer) {
            return std::move(integer);
        }
    }

    const uint32_t floatMask = android::ResTable_map::TYPE_FLOAT
            | android::ResTable_map::TYPE_DIMENSION | android::ResTable_map::TYPE_FRACTION;
    if (typeMask & floatMask) {
        // Try parsing this as a float.
        std::unique_ptr<BinaryPrimitive> floatingPoint = tryParseFloat(value);
        if (floatingPoint) {
            if (typeMask & androidTypeToAttributeTypeMask(floatingPoint->value.dataType)) {
                return std::move(floatingPoint);
            }
        }
    }
    return {};
}

/**
 * We successively try to parse the string as a resource type that the Attribute
 * allows.
 */
std::unique_ptr<Item> tryParseItemForAttribute(
        const StringPiece& str, const Attribute* attr,
        const std::function<void(const ResourceName&)>& onCreateReference) {
    const uint32_t typeMask = attr->typeMask;
    std::unique_ptr<Item> value = tryParseItemForAttribute(str, typeMask, onCreateReference);
    if (value) {
        return value;
    }

    if (typeMask & android::ResTable_map::TYPE_ENUM) {
        // Try parsing this as an enum.
        std::unique_ptr<BinaryPrimitive> enumValue = tryParseEnumSymbol(attr, str);
        if (enumValue) {
            return std::move(enumValue);
        }
    }

    if (typeMask & android::ResTable_map::TYPE_FLAGS) {
        // Try parsing this as a flag.
        std::unique_ptr<BinaryPrimitive> flagValue = tryParseFlagSymbol(attr, str);
        if (flagValue) {
            return std::move(flagValue);
        }
    }
    return {};
}

std::string buildResourceFileName(const ResourceFile& resFile, const NameMangler* mangler) {
    std::stringstream out;
    out << "res/" << resFile.name.type;
    if (resFile.config != ConfigDescription{}) {
        out << "-" << resFile.config;
    }
    out << "/";

    if (mangler && mangler->shouldMangle(resFile.name.package)) {
        out << NameMangler::mangleEntry(resFile.name.package, resFile.name.entry);
    } else {
        out << resFile.name.entry;
    }
    out << file::getExtension(resFile.source.path);
    return out.str();
}

} // namespace ResourceUtils
} // namespace aapt
