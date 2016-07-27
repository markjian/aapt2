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

#include "ResourceParser.h"
#include "ResourceTable.h"
#include "ResourceUtils.h"
#include "ResourceValues.h"
#include "ValueVisitor.h"
#include "util/ImmutableMap.h"
#include "util/Util.h"
#include "xml/XmlPullParser.h"

#include <functional>
#include <sstream>

namespace aapt {

constexpr const char* sXliffNamespaceUri = "urn:oasis:names:tc:xliff:document:1.2";

/**
 * Returns true if the element is <skip> or <eat-comment> and can be safely ignored.
 */
static bool shouldIgnoreElement(const StringPiece& ns, const StringPiece& name) {
    return ns.empty() && (name == "skip" || name == "eat-comment");
}

static uint32_t parseFormatType(const StringPiece& piece) {
    if (piece == "reference")      return android::ResTable_map::TYPE_REFERENCE;
    else if (piece == "string")    return android::ResTable_map::TYPE_STRING;
    else if (piece == "integer")   return android::ResTable_map::TYPE_INTEGER;
    else if (piece == "boolean")   return android::ResTable_map::TYPE_BOOLEAN;
    else if (piece == "color")     return android::ResTable_map::TYPE_COLOR;
    else if (piece == "float")     return android::ResTable_map::TYPE_FLOAT;
    else if (piece == "dimension") return android::ResTable_map::TYPE_DIMENSION;
    else if (piece == "fraction")  return android::ResTable_map::TYPE_FRACTION;
    else if (piece == "enum")      return android::ResTable_map::TYPE_ENUM;
    else if (piece == "flags")     return android::ResTable_map::TYPE_FLAGS;
    return 0;
}

static uint32_t parseFormatAttribute(const StringPiece& str) {
    uint32_t mask = 0;
    for (StringPiece part : util::tokenize(str, '|')) {
        StringPiece trimmedPart = util::trimWhitespace(part);
        uint32_t type = parseFormatType(trimmedPart);
        if (type == 0) {
            return 0;
        }
        mask |= type;
    }
    return mask;
}

/**
 * A parsed resource ready to be added to the ResourceTable.
 */
struct ParsedResource {
    ResourceName name;
    ConfigDescription config;
    std::string product;
    Source source;
    ResourceId id;
    Maybe<SymbolState> symbolState;
    std::string comment;
    std::unique_ptr<Value> value;
    std::list<ParsedResource> childResources;
};

// Recursively adds resources to the ResourceTable.
static bool addResourcesToTable(ResourceTable* table, IDiagnostics* diag, ParsedResource* res) {
    StringPiece trimmedComment = util::trimWhitespace(res->comment);
    if (trimmedComment.size() != res->comment.size()) {
        // Only if there was a change do we re-assign.
        res->comment = trimmedComment.toString();
    }

    if (res->symbolState) {
        Symbol symbol;
        symbol.state = res->symbolState.value();
        symbol.source = res->source;
        symbol.comment = res->comment;
        if (!table->setSymbolState(res->name, res->id, symbol, diag)) {
            return false;
        }
    }

    if (res->value) {
        // Attach the comment, source and config to the value.
        res->value->setComment(std::move(res->comment));
        res->value->setSource(std::move(res->source));

        if (!table->addResource(res->name, res->id, res->config, res->product,
                                std::move(res->value), diag)) {
            return false;
        }
    }

    bool error = false;
    for (ParsedResource& child : res->childResources) {
        error |= !addResourcesToTable(table, diag, &child);
    }
    return !error;
}

// Convenient aliases for more readable function calls.
enum {
    kAllowRawString = true,
    kNoRawString = false
};

ResourceParser::ResourceParser(IDiagnostics* diag, ResourceTable* table, const Source& source,
                               const ConfigDescription& config,
                               const ResourceParserOptions& options) :
        mDiag(diag), mTable(table), mSource(source), mConfig(config), mOptions(options) {
}

/**
 * Build a string from XML that converts nested elements into Span objects.
 */
bool ResourceParser::flattenXmlSubtree(xml::XmlPullParser* parser, std::string* outRawString,
                                       StyleString* outStyleString) {
    std::vector<Span> spanStack;

    bool error = false;
    outRawString->clear();
    outStyleString->spans.clear();
    util::StringBuilder builder;
    size_t depth = 1;
    while (xml::XmlPullParser::isGoodEvent(parser->next())) {
        const xml::XmlPullParser::Event event = parser->getEvent();
        if (event == xml::XmlPullParser::Event::kEndElement) {
            if (!parser->getElementNamespace().empty()) {
                // We already warned and skipped the start element, so just skip here too
                continue;
            }

            depth--;
            if (depth == 0) {
                break;
            }

            spanStack.back().lastChar = builder.str().size() - 1;
            outStyleString->spans.push_back(spanStack.back());
            spanStack.pop_back();

        } else if (event == xml::XmlPullParser::Event::kText) {
            outRawString->append(parser->getText());
            builder.append(parser->getText());

        } else if (event == xml::XmlPullParser::Event::kStartElement) {
            if (!parser->getElementNamespace().empty()) {
                if (parser->getElementNamespace() != sXliffNamespaceUri) {
                    // Only warn if this isn't an xliff namespace.
                    mDiag->warn(DiagMessage(mSource.withLine(parser->getLineNumber()))
                                << "skipping element '"
                                << parser->getElementName()
                                << "' with unknown namespace '"
                                << parser->getElementNamespace()
                                << "'");
                }
                continue;
            }
            depth++;

            // Build a span object out of the nested element.
            std::string spanName = parser->getElementName();
            const auto endAttrIter = parser->endAttributes();
            for (auto attrIter = parser->beginAttributes(); attrIter != endAttrIter; ++attrIter) {
                spanName += ";";
                spanName += attrIter->name;
                spanName += "=";
                spanName += attrIter->value;
            }

            if (builder.str().size() > std::numeric_limits<uint32_t>::max()) {
                mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                             << "style string '" << builder.str() << "' is too long");
                error = true;
            } else {
                spanStack.push_back(Span{ spanName, static_cast<uint32_t>(builder.str().size()) });
            }

        } else if (event == xml::XmlPullParser::Event::kComment) {
            // Skip
        } else {
            assert(false);
        }
    }
    assert(spanStack.empty() && "spans haven't been fully processed");

    outStyleString->str = builder.str();
    return !error;
}

bool ResourceParser::parse(xml::XmlPullParser* parser) {
    bool error = false;
    const size_t depth = parser->getDepth();
    while (xml::XmlPullParser::nextChildNode(parser, depth)) {
        if (parser->getEvent() != xml::XmlPullParser::Event::kStartElement) {
            // Skip comments and text.
            continue;
        }

        if (!parser->getElementNamespace().empty() || parser->getElementName() != "resources") {
            mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                         << "root element must be <resources>");
            return false;
        }

        error |= !parseResources(parser);
        break;
    };

    if (parser->getEvent() == xml::XmlPullParser::Event::kBadDocument) {
        mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                     << "xml parser error: " << parser->getLastError());
        return false;
    }
    return !error;
}

bool ResourceParser::parseResources(xml::XmlPullParser* parser) {
    std::set<ResourceName> strippedResources;

    bool error = false;
    std::string comment;
    const size_t depth = parser->getDepth();
    while (xml::XmlPullParser::nextChildNode(parser, depth)) {
        const xml::XmlPullParser::Event event = parser->getEvent();
        if (event == xml::XmlPullParser::Event::kComment) {
            comment = parser->getComment();
            continue;
        }

        if (event == xml::XmlPullParser::Event::kText) {
            if (!util::trimWhitespace(parser->getText()).empty()) {
                mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                             << "plain text not allowed here");
                error = true;
            }
            continue;
        }

        assert(event == xml::XmlPullParser::Event::kStartElement);

        if (!parser->getElementNamespace().empty()) {
            // Skip unknown namespace.
            continue;
        }

        std::string elementName = parser->getElementName();
        if (elementName == "skip" || elementName == "eat-comment") {
            comment = "";
            continue;
        }

        ParsedResource parsedResource;
        parsedResource.config = mConfig;
        parsedResource.source = mSource.withLine(parser->getLineNumber());
        parsedResource.comment = std::move(comment);

        // Extract the product name if it exists.
        if (Maybe<StringPiece> maybeProduct = xml::findNonEmptyAttribute(parser, "product")) {
            parsedResource.product = maybeProduct.value().toString();
        }

        // Parse the resource regardless of product.
        if (!parseResource(parser, &parsedResource)) {
            error = true;
            continue;
        }

        if (!addResourcesToTable(mTable, mDiag, &parsedResource)) {
            error = true;
        }
    }

    // Check that we included at least one variant of each stripped resource.
    for (const ResourceName& strippedResource : strippedResources) {
        if (!mTable->findResource(strippedResource)) {
            // Failed to find the resource.
            mDiag->error(DiagMessage(mSource) << "resource '" << strippedResource << "' "
                         "was filtered out but no product variant remains");
            error = true;
        }
    }

    return !error;
}


bool ResourceParser::parseResource(xml::XmlPullParser* parser, ParsedResource* outResource) {
    struct ItemTypeFormat {
        ResourceType type;
        uint32_t format;
    };

    using BagParseFunc = std::function<bool(ResourceParser*, xml::XmlPullParser*, ParsedResource*)>;

    static const auto elToItemMap = ImmutableMap<std::string, ItemTypeFormat>::createPreSorted({
            { "bool",      { ResourceType::kBool, android::ResTable_map::TYPE_BOOLEAN } },
            { "color",     { ResourceType::kColor, android::ResTable_map::TYPE_COLOR } },
            { "dimen",     { ResourceType::kDimen, android::ResTable_map::TYPE_FLOAT
                                                    | android::ResTable_map::TYPE_FRACTION
                                                    | android::ResTable_map::TYPE_DIMENSION } },
            { "drawable",  { ResourceType::kDrawable, android::ResTable_map::TYPE_COLOR } },
            { "fraction",  { ResourceType::kFraction, android::ResTable_map::TYPE_FLOAT
                                                       | android::ResTable_map::TYPE_FRACTION
                                                       | android::ResTable_map::TYPE_DIMENSION } },
            { "integer",   { ResourceType::kInteger, android::ResTable_map::TYPE_INTEGER } },
            { "string",    { ResourceType::kString, android::ResTable_map::TYPE_STRING } },
    });

    static const auto elToBagMap = ImmutableMap<std::string, BagParseFunc>::createPreSorted({
            { "add-resource",      std::mem_fn(&ResourceParser::parseAddResource) },
            { "array",             std::mem_fn(&ResourceParser::parseArray) },
            { "attr",              std::mem_fn(&ResourceParser::parseAttr) },
            { "declare-styleable", std::mem_fn(&ResourceParser::parseDeclareStyleable) },
            { "integer-array",     std::mem_fn(&ResourceParser::parseIntegerArray) },
            { "java-symbol",       std::mem_fn(&ResourceParser::parseSymbol) },
            { "plurals",           std::mem_fn(&ResourceParser::parsePlural) },
            { "public",            std::mem_fn(&ResourceParser::parsePublic) },
            { "public-group",      std::mem_fn(&ResourceParser::parsePublicGroup) },
            { "string-array",      std::mem_fn(&ResourceParser::parseStringArray) },
            { "style",             std::mem_fn(&ResourceParser::parseStyle) },
            { "symbol",            std::mem_fn(&ResourceParser::parseSymbol) },
    });

    std::string resourceType = parser->getElementName();

    // The value format accepted for this resource.
    uint32_t resourceFormat = 0u;

    if (resourceType == "item") {
        // Items have their type encoded in the type attribute.
        if (Maybe<StringPiece> maybeType = xml::findNonEmptyAttribute(parser, "type")) {
            resourceType = maybeType.value().toString();
        } else {
            mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                         << "<item> must have a 'type' attribute");
            return false;
        }

        if (Maybe<StringPiece> maybeFormat = xml::findNonEmptyAttribute(parser, "format")) {
            // An explicit format for this resource was specified. The resource will retain
            // its type in its name, but the accepted value for this type is overridden.
            resourceFormat = parseFormatType(maybeFormat.value());
            if (!resourceFormat) {
                mDiag->error(DiagMessage(outResource->source)
                             << "'" << maybeFormat.value() << "' is an invalid format");
                return false;
            }
        }
    }

    // Get the name of the resource. This will be checked later, because not all
    // XML elements require a name.
    Maybe<StringPiece> maybeName = xml::findNonEmptyAttribute(parser, "name");

    if (resourceType == "id") {
        if (!maybeName) {
            mDiag->error(DiagMessage(outResource->source)
                         << "<" << parser->getElementName() << "> missing 'name' attribute");
            return false;
        }

        outResource->name.type = ResourceType::kId;
        outResource->name.entry = maybeName.value().toString();
        outResource->value = util::make_unique<Id>();
        return true;
    }

    const auto itemIter = elToItemMap.find(resourceType);
    if (itemIter != elToItemMap.end()) {
        // This is an item, record its type and format and start parsing.

        if (!maybeName) {
            mDiag->error(DiagMessage(outResource->source)
                         << "<" << parser->getElementName() << "> missing 'name' attribute");
            return false;
        }

        outResource->name.type = itemIter->second.type;
        outResource->name.entry = maybeName.value().toString();

        // Only use the implicit format for this type if it wasn't overridden.
        if (!resourceFormat) {
            resourceFormat = itemIter->second.format;
        }

        if (!parseItem(parser, outResource, resourceFormat)) {
            return false;
        }
        return true;
    }

    // This might be a bag or something.
    const auto bagIter = elToBagMap.find(resourceType);
    if (bagIter != elToBagMap.end()) {
        // Ensure we have a name (unless this is a <public-group>).
        if (resourceType != "public-group") {
            if (!maybeName) {
                mDiag->error(DiagMessage(outResource->source)
                             << "<" << parser->getElementName() << "> missing 'name' attribute");
                return false;
            }

            outResource->name.entry = maybeName.value().toString();
        }

        // Call the associated parse method. The type will be filled in by the
        // parse func.
        if (!bagIter->second(this, parser, outResource)) {
            return false;
        }
        return true;
    }

    // Try parsing the elementName (or type) as a resource. These shall only be
    // resources like 'layout' or 'xml' and they can only be references.
    const ResourceType* parsedType = parseResourceType(resourceType);
    if (parsedType) {
        if (!maybeName) {
            mDiag->error(DiagMessage(outResource->source)
                         << "<" << parser->getElementName() << "> missing 'name' attribute");
            return false;
        }

        outResource->name.type = *parsedType;
        outResource->name.entry = maybeName.value().toString();
        outResource->value = parseXml(parser, android::ResTable_map::TYPE_REFERENCE, kNoRawString);
        if (!outResource->value) {
            mDiag->error(DiagMessage(outResource->source)
                         << "invalid value for type '" << *parsedType << "'. Expected a reference");
            return false;
        }
        return true;
    }

    mDiag->warn(DiagMessage(outResource->source)
                << "unknown resource type '" << parser->getElementName() << "'");
    return false;
}

bool ResourceParser::parseItem(xml::XmlPullParser* parser, ParsedResource* outResource,
                               const uint32_t format) {
    if (format == android::ResTable_map::TYPE_STRING) {
        return parseString(parser, outResource);
    }

    outResource->value = parseXml(parser, format, kNoRawString);
    if (!outResource->value) {
        mDiag->error(DiagMessage(outResource->source) << "invalid " << outResource->name.type);
        return false;
    }
    return true;
}

/**
 * Reads the entire XML subtree and attempts to parse it as some Item,
 * with typeMask denoting which items it can be. If allowRawValue is
 * true, a RawString is returned if the XML couldn't be parsed as
 * an Item. If allowRawValue is false, nullptr is returned in this
 * case.
 */
std::unique_ptr<Item> ResourceParser::parseXml(xml::XmlPullParser* parser, const uint32_t typeMask,
                                               const bool allowRawValue) {
    const size_t beginXmlLine = parser->getLineNumber();

    std::string rawValue;
    StyleString styleString;
    if (!flattenXmlSubtree(parser, &rawValue, &styleString)) {
        return {};
    }

    if (!styleString.spans.empty()) {
        // This can only be a StyledString.
        return util::make_unique<StyledString>(
                mTable->stringPool.makeRef(styleString, StringPool::Context{ 1, mConfig }));
    }

    auto onCreateReference = [&](const ResourceName& name) {
        // name.package can be empty here, as it will assume the package name of the table.
        std::unique_ptr<Id> id = util::make_unique<Id>();
        id->setSource(mSource.withLine(beginXmlLine));
        mTable->addResource(name, {}, {}, std::move(id), mDiag);
    };

    // Process the raw value.
    std::unique_ptr<Item> processedItem = ResourceUtils::parseItemForAttribute(rawValue, typeMask,
                                                                               onCreateReference);
    if (processedItem) {
        // Fix up the reference.
        if (Reference* ref = valueCast<Reference>(processedItem.get())) {
            transformReferenceFromNamespace(parser, "", ref);
        }
        return processedItem;
    }

    // Try making a regular string.
    if (typeMask & android::ResTable_map::TYPE_STRING) {
        // Use the trimmed, escaped string.
        return util::make_unique<String>(
                mTable->stringPool.makeRef(styleString.str, StringPool::Context{ 1, mConfig }));
    }

    if (allowRawValue) {
        // We can't parse this so return a RawString if we are allowed.
        return util::make_unique<RawString>(
                mTable->stringPool.makeRef(rawValue, StringPool::Context{ 1, mConfig }));
    }
    return {};
}

bool ResourceParser::parseString(xml::XmlPullParser* parser, ParsedResource* outResource) {
    bool formatted = true;
    if (Maybe<StringPiece> formattedAttr = xml::findAttribute(parser, "formatted")) {
        if (!ResourceUtils::tryParseBool(formattedAttr.value(), &formatted)) {
            mDiag->error(DiagMessage(outResource->source)
                         << "invalid value for 'formatted'. Must be a boolean");
            return false;
        }
    }

    bool translateable = mOptions.translatable;
    if (Maybe<StringPiece> translateableAttr = xml::findAttribute(parser, "translatable")) {
        if (!ResourceUtils::tryParseBool(translateableAttr.value(), &translateable)) {
            mDiag->error(DiagMessage(outResource->source)
                         << "invalid value for 'translatable'. Must be a boolean");
            return false;
        }
    }

    outResource->value = parseXml(parser, android::ResTable_map::TYPE_STRING, kNoRawString);
    if (!outResource->value) {
        mDiag->error(DiagMessage(outResource->source) << "not a valid string");
        return false;
    }

    if (String* stringValue = valueCast<String>(outResource->value.get())) {
        stringValue->setTranslateable(translateable);

        if (formatted && translateable) {
            if (!util::verifyJavaStringFormat(*stringValue->value)) {
                DiagMessage msg(outResource->source);
                msg << "multiple substitutions specified in non-positional format; "
                       "did you mean to add the formatted=\"false\" attribute?";
                if (mOptions.errorOnPositionalArguments) {
                    mDiag->error(msg);
                    return false;
                }

                mDiag->warn(msg);
            }
        }

    } else if (StyledString* stringValue = valueCast<StyledString>(outResource->value.get())) {
        stringValue->setTranslateable(translateable);
    }
    return true;
}

bool ResourceParser::parsePublic(xml::XmlPullParser* parser, ParsedResource* outResource) {
    Maybe<StringPiece> maybeType = xml::findNonEmptyAttribute(parser, "type");
    if (!maybeType) {
        mDiag->error(DiagMessage(outResource->source) << "<public> must have a 'type' attribute");
        return false;
    }

    const ResourceType* parsedType = parseResourceType(maybeType.value());
    if (!parsedType) {
        mDiag->error(DiagMessage(outResource->source)
                     << "invalid resource type '" << maybeType.value() << "' in <public>");
        return false;
    }

    outResource->name.type = *parsedType;

    if (Maybe<StringPiece> maybeIdStr = xml::findNonEmptyAttribute(parser, "id")) {
        Maybe<ResourceId> maybeId = ResourceUtils::tryParseResourceId(maybeIdStr.value());
        if (!maybeId) {
            mDiag->error(DiagMessage(outResource->source)
                         << "invalid resource ID '" << maybeId.value() << "' in <public>");
            return false;
        }
        outResource->id = maybeId.value();
    }

    if (*parsedType == ResourceType::kId) {
        // An ID marked as public is also the definition of an ID.
        outResource->value = util::make_unique<Id>();
    }

    outResource->symbolState = SymbolState::kPublic;
    return true;
}

bool ResourceParser::parsePublicGroup(xml::XmlPullParser* parser, ParsedResource* outResource) {
    Maybe<StringPiece> maybeType = xml::findNonEmptyAttribute(parser, "type");
    if (!maybeType) {
        mDiag->error(DiagMessage(outResource->source)
                     << "<public-group> must have a 'type' attribute");
        return false;
    }

    const ResourceType* parsedType = parseResourceType(maybeType.value());
    if (!parsedType) {
        mDiag->error(DiagMessage(outResource->source)
                     << "invalid resource type '" << maybeType.value() << "' in <public-group>");
        return false;
    }

    Maybe<StringPiece> maybeIdStr = xml::findNonEmptyAttribute(parser, "first-id");
    if (!maybeIdStr) {
        mDiag->error(DiagMessage(outResource->source)
                     << "<public-group> must have a 'first-id' attribute");
        return false;
    }

    Maybe<ResourceId> maybeId = ResourceUtils::tryParseResourceId(maybeIdStr.value());
    if (!maybeId) {
        mDiag->error(DiagMessage(outResource->source)
                     << "invalid resource ID '" << maybeIdStr.value() << "' in <public-group>");
        return false;
    }

    ResourceId nextId = maybeId.value();

    std::string comment;
    bool error = false;
    const size_t depth = parser->getDepth();
    while (xml::XmlPullParser::nextChildNode(parser, depth)) {
        if (parser->getEvent() == xml::XmlPullParser::Event::kComment) {
            comment = util::trimWhitespace(parser->getComment()).toString();
            continue;
        } else if (parser->getEvent() != xml::XmlPullParser::Event::kStartElement) {
            // Skip text.
            continue;
        }

        const Source itemSource = mSource.withLine(parser->getLineNumber());
        const std::string& elementNamespace = parser->getElementNamespace();
        const std::string& elementName = parser->getElementName();
        if (elementNamespace.empty() && elementName == "public") {
            Maybe<StringPiece> maybeName = xml::findNonEmptyAttribute(parser, "name");
            if (!maybeName) {
                mDiag->error(DiagMessage(itemSource) << "<public> must have a 'name' attribute");
                error = true;
                continue;
            }

            if (xml::findNonEmptyAttribute(parser, "id")) {
                mDiag->error(DiagMessage(itemSource) << "'id' is ignored within <public-group>");
                error = true;
                continue;
            }

            if (xml::findNonEmptyAttribute(parser, "type")) {
                mDiag->error(DiagMessage(itemSource) << "'type' is ignored within <public-group>");
                error = true;
                continue;
            }

            ParsedResource childResource;
            childResource.name.type = *parsedType;
            childResource.name.entry = maybeName.value().toString();
            childResource.id = nextId;
            childResource.comment = std::move(comment);
            childResource.source = itemSource;
            childResource.symbolState = SymbolState::kPublic;
            outResource->childResources.push_back(std::move(childResource));

            nextId.id += 1;

        } else if (!shouldIgnoreElement(elementNamespace, elementName)) {
            mDiag->error(DiagMessage(itemSource) << ":" << elementName << ">");
            error = true;
        }
    }
    return !error;
}

bool ResourceParser::parseSymbolImpl(xml::XmlPullParser* parser, ParsedResource* outResource) {
    Maybe<StringPiece> maybeType = xml::findNonEmptyAttribute(parser, "type");
    if (!maybeType) {
        mDiag->error(DiagMessage(outResource->source)
                     << "<" << parser->getElementName() << "> must have a 'type' attribute");
        return false;
    }

    const ResourceType* parsedType = parseResourceType(maybeType.value());
    if (!parsedType) {
        mDiag->error(DiagMessage(outResource->source)
                     << "invalid resource type '" << maybeType.value()
                     << "' in <" << parser->getElementName() << ">");
        return false;
    }

    outResource->name.type = *parsedType;
    return true;
}

bool ResourceParser::parseSymbol(xml::XmlPullParser* parser, ParsedResource* outResource) {
    if (parseSymbolImpl(parser, outResource)) {
        outResource->symbolState = SymbolState::kPrivate;
        return true;
    }
    return false;
}

bool ResourceParser::parseAddResource(xml::XmlPullParser* parser, ParsedResource* outResource) {
    if (parseSymbolImpl(parser, outResource)) {
        outResource->symbolState = SymbolState::kUndefined;
        return true;
    }
    return false;
}


bool ResourceParser::parseAttr(xml::XmlPullParser* parser, ParsedResource* outResource) {
    return parseAttrImpl(parser, outResource, false);
}

bool ResourceParser::parseAttrImpl(xml::XmlPullParser* parser, ParsedResource* outResource,
                                   bool weak) {
    outResource->name.type = ResourceType::kAttr;

    // Attributes only end up in default configuration.
    if (outResource->config != ConfigDescription::defaultConfig()) {
        mDiag->warn(DiagMessage(outResource->source) << "ignoring configuration '"
                    << outResource->config << "' for attribute " << outResource->name);
        outResource->config = ConfigDescription::defaultConfig();
    }

    uint32_t typeMask = 0;

    Maybe<StringPiece> maybeFormat = xml::findAttribute(parser, "format");
    if (maybeFormat) {
        typeMask = parseFormatAttribute(maybeFormat.value());
        if (typeMask == 0) {
            mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                         << "invalid attribute format '" << maybeFormat.value() << "'");
            return false;
        }
    }

    Maybe<int32_t> maybeMin, maybeMax;

    if (Maybe<StringPiece> maybeMinStr = xml::findAttribute(parser, "min")) {
        StringPiece minStr = util::trimWhitespace(maybeMinStr.value());
        if (!minStr.empty()) {
            std::u16string minStr16 = util::utf8ToUtf16(minStr);
            android::Res_value value;
            if (android::ResTable::stringToInt(minStr16.data(), minStr16.size(), &value)) {
                maybeMin = static_cast<int32_t>(value.data);
            }
        }

        if (!maybeMin) {
            mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                         << "invalid 'min' value '" << minStr << "'");
            return false;
        }
    }

    if (Maybe<StringPiece> maybeMaxStr = xml::findAttribute(parser, "max")) {
        StringPiece maxStr = util::trimWhitespace(maybeMaxStr.value());
        if (!maxStr.empty()) {
            std::u16string maxStr16 = util::utf8ToUtf16(maxStr);
            android::Res_value value;
            if (android::ResTable::stringToInt(maxStr16.data(), maxStr16.size(), &value)) {
                maybeMax = static_cast<int32_t>(value.data);
            }
        }

        if (!maybeMax) {
            mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                         << "invalid 'max' value '" << maxStr << "'");
            return false;
        }
    }

    if ((maybeMin || maybeMax) && (typeMask & android::ResTable_map::TYPE_INTEGER) == 0) {
        mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                     << "'min' and 'max' can only be used when format='integer'");
        return false;
    }

    struct SymbolComparator {
        bool operator()(const Attribute::Symbol& a, const Attribute::Symbol& b) {
            return a.symbol.name.value() < b.symbol.name.value();
        }
    };

    std::set<Attribute::Symbol, SymbolComparator> items;

    std::string comment;
    bool error = false;
    const size_t depth = parser->getDepth();
    while (xml::XmlPullParser::nextChildNode(parser, depth)) {
        if (parser->getEvent() == xml::XmlPullParser::Event::kComment) {
            comment = util::trimWhitespace(parser->getComment()).toString();
            continue;
        } else if (parser->getEvent() != xml::XmlPullParser::Event::kStartElement) {
            // Skip text.
            continue;
        }

        const Source itemSource = mSource.withLine(parser->getLineNumber());
        const std::string& elementNamespace = parser->getElementNamespace();
        const std::string& elementName = parser->getElementName();
        if (elementNamespace.empty() && (elementName == "flag" || elementName == "enum")) {
            if (elementName == "enum") {
                if (typeMask & android::ResTable_map::TYPE_FLAGS) {
                    mDiag->error(DiagMessage(itemSource)
                                 << "can not define an <enum>; already defined a <flag>");
                    error = true;
                    continue;
                }
                typeMask |= android::ResTable_map::TYPE_ENUM;

            } else if (elementName == "flag") {
                if (typeMask & android::ResTable_map::TYPE_ENUM) {
                    mDiag->error(DiagMessage(itemSource)
                                 << "can not define a <flag>; already defined an <enum>");
                    error = true;
                    continue;
                }
                typeMask |= android::ResTable_map::TYPE_FLAGS;
            }

            if (Maybe<Attribute::Symbol> s = parseEnumOrFlagItem(parser, elementName)) {
                Attribute::Symbol& symbol = s.value();
                ParsedResource childResource;
                childResource.name = symbol.symbol.name.value();
                childResource.source = itemSource;
                childResource.value = util::make_unique<Id>();
                outResource->childResources.push_back(std::move(childResource));

                symbol.symbol.setComment(std::move(comment));
                symbol.symbol.setSource(itemSource);

                auto insertResult = items.insert(std::move(symbol));
                if (!insertResult.second) {
                    const Attribute::Symbol& existingSymbol = *insertResult.first;
                    mDiag->error(DiagMessage(itemSource)
                                 << "duplicate symbol '" << existingSymbol.symbol.name.value().entry
                                 << "'");

                    mDiag->note(DiagMessage(existingSymbol.symbol.getSource())
                                << "first defined here");
                    error = true;
                }
            } else {
                error = true;
            }
        } else if (!shouldIgnoreElement(elementNamespace, elementName)) {
            mDiag->error(DiagMessage(itemSource) << ":" << elementName << ">");
            error = true;
        }

        comment = {};
    }

    if (error) {
        return false;
    }

    std::unique_ptr<Attribute> attr = util::make_unique<Attribute>(weak);
    attr->symbols = std::vector<Attribute::Symbol>(items.begin(), items.end());
    attr->typeMask = typeMask ? typeMask : uint32_t(android::ResTable_map::TYPE_ANY);
    if (maybeMin) {
        attr->minInt = maybeMin.value();
    }

    if (maybeMax) {
        attr->maxInt = maybeMax.value();
    }
    outResource->value = std::move(attr);
    return true;
}

Maybe<Attribute::Symbol> ResourceParser::parseEnumOrFlagItem(xml::XmlPullParser* parser,
                                                             const StringPiece& tag) {
    const Source source = mSource.withLine(parser->getLineNumber());

    Maybe<StringPiece> maybeName = xml::findNonEmptyAttribute(parser, "name");
    if (!maybeName) {
        mDiag->error(DiagMessage(source) << "no attribute 'name' found for tag <" << tag << ">");
        return {};
    }

    Maybe<StringPiece> maybeValue = xml::findNonEmptyAttribute(parser, "value");
    if (!maybeValue) {
        mDiag->error(DiagMessage(source) << "no attribute 'value' found for tag <" << tag << ">");
        return {};
    }

    std::u16string value16 = util::utf8ToUtf16(maybeValue.value());
    android::Res_value val;
    if (!android::ResTable::stringToInt(value16.data(), value16.size(), &val)) {
        mDiag->error(DiagMessage(source) << "invalid value '" << maybeValue.value()
                     << "' for <" << tag << ">; must be an integer");
        return {};
    }

    return Attribute::Symbol{
            Reference(ResourceNameRef({}, ResourceType::kId, maybeName.value())), val.data };
}

static Maybe<Reference> parseXmlAttributeName(StringPiece str) {
    str = util::trimWhitespace(str);
    const char* start = str.data();
    const char* const end = start + str.size();
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
                        name.empty() ? str.toString() : name.toString());
    return Maybe<Reference>(std::move(ref));
}

bool ResourceParser::parseStyleItem(xml::XmlPullParser* parser, Style* style) {
    const Source source = mSource.withLine(parser->getLineNumber());

    Maybe<StringPiece> maybeName = xml::findNonEmptyAttribute(parser, "name");
    if (!maybeName) {
        mDiag->error(DiagMessage(source) << "<item> must have a 'name' attribute");
        return false;
    }

    Maybe<Reference> maybeKey = parseXmlAttributeName(maybeName.value());
    if (!maybeKey) {
        mDiag->error(DiagMessage(source) << "invalid attribute name '" << maybeName.value() << "'");
        return false;
    }

    transformReferenceFromNamespace(parser, "", &maybeKey.value());
    maybeKey.value().setSource(source);

    std::unique_ptr<Item> value = parseXml(parser, 0, kAllowRawString);
    if (!value) {
        mDiag->error(DiagMessage(source) << "could not parse style item");
        return false;
    }

    style->entries.push_back(Style::Entry{ std::move(maybeKey.value()), std::move(value) });
    return true;
}

bool ResourceParser::parseStyle(xml::XmlPullParser* parser, ParsedResource* outResource) {
    outResource->name.type = ResourceType::kStyle;

    std::unique_ptr<Style> style = util::make_unique<Style>();

    Maybe<StringPiece> maybeParent = xml::findAttribute(parser, "parent");
    if (maybeParent) {
        // If the parent is empty, we don't have a parent, but we also don't infer either.
        if (!maybeParent.value().empty()) {
            std::string errStr;
            style->parent = ResourceUtils::parseStyleParentReference(maybeParent.value(), &errStr);
            if (!style->parent) {
                mDiag->error(DiagMessage(outResource->source) << errStr);
                return false;
            }

            // Transform the namespace prefix to the actual package name, and mark the reference as
            // private if appropriate.
            transformReferenceFromNamespace(parser, "", &style->parent.value());
        }

    } else {
        // No parent was specified, so try inferring it from the style name.
        std::string styleName = outResource->name.entry;
        size_t pos = styleName.find_last_of(u'.');
        if (pos != std::string::npos) {
            style->parentInferred = true;
            style->parent = Reference(ResourceName({}, ResourceType::kStyle,
                                                   styleName.substr(0, pos)));
        }
    }

    bool error = false;
    const size_t depth = parser->getDepth();
    while (xml::XmlPullParser::nextChildNode(parser, depth)) {
        if (parser->getEvent() != xml::XmlPullParser::Event::kStartElement) {
            // Skip text and comments.
            continue;
        }

        const std::string& elementNamespace = parser->getElementNamespace();
        const std::string& elementName = parser->getElementName();
        if (elementNamespace == "" && elementName == "item") {
            error |= !parseStyleItem(parser, style.get());

        } else if (!shouldIgnoreElement(elementNamespace, elementName)) {
            mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                         << ":" << elementName << ">");
            error = true;
        }
    }

    if (error) {
        return false;
    }

    outResource->value = std::move(style);
    return true;
}

bool ResourceParser::parseArray(xml::XmlPullParser* parser, ParsedResource* outResource) {
    return parseArrayImpl(parser, outResource, android::ResTable_map::TYPE_ANY);
}

bool ResourceParser::parseIntegerArray(xml::XmlPullParser* parser, ParsedResource* outResource) {
    return parseArrayImpl(parser, outResource, android::ResTable_map::TYPE_INTEGER);
}

bool ResourceParser::parseStringArray(xml::XmlPullParser* parser, ParsedResource* outResource) {
    return parseArrayImpl(parser, outResource, android::ResTable_map::TYPE_STRING);
}

bool ResourceParser::parseArrayImpl(xml::XmlPullParser* parser, ParsedResource* outResource,
                                    const uint32_t typeMask) {
    outResource->name.type = ResourceType::kArray;

    std::unique_ptr<Array> array = util::make_unique<Array>();

    bool translateable = mOptions.translatable;
    if (Maybe<StringPiece> translateableAttr = xml::findAttribute(parser, "translatable")) {
        if (!ResourceUtils::tryParseBool(translateableAttr.value(), &translateable)) {
            mDiag->error(DiagMessage(outResource->source)
                         << "invalid value for 'translatable'. Must be a boolean");
            return false;
        }
    }
    array->setTranslateable(translateable);

    bool error = false;
    const size_t depth = parser->getDepth();
    while (xml::XmlPullParser::nextChildNode(parser, depth)) {
        if (parser->getEvent() != xml::XmlPullParser::Event::kStartElement) {
            // Skip text and comments.
            continue;
        }

        const Source itemSource = mSource.withLine(parser->getLineNumber());
        const std::string& elementNamespace = parser->getElementNamespace();
        const std::string& elementName = parser->getElementName();
        if (elementNamespace.empty() && elementName == "item") {
            std::unique_ptr<Item> item = parseXml(parser, typeMask, kNoRawString);
            if (!item) {
                mDiag->error(DiagMessage(itemSource) << "could not parse array item");
                error = true;
                continue;
            }
            item->setSource(itemSource);
            array->items.emplace_back(std::move(item));

        } else if (!shouldIgnoreElement(elementNamespace, elementName)) {
            mDiag->error(DiagMessage(mSource.withLine(parser->getLineNumber()))
                         << "unknown tag <" << elementNamespace << ":" << elementName << ">");
            error = true;
        }
    }

    if (error) {
        return false;
    }

    outResource->value = std::move(array);
    return true;
}

bool ResourceParser::parsePlural(xml::XmlPullParser* parser, ParsedResource* outResource) {
    outResource->name.type = ResourceType::kPlurals;

    std::unique_ptr<Plural> plural = util::make_unique<Plural>();

    bool error = false;
    const size_t depth = parser->getDepth();
    while (xml::XmlPullParser::nextChildNode(parser, depth)) {
        if (parser->getEvent() != xml::XmlPullParser::Event::kStartElement) {
            // Skip text and comments.
            continue;
        }

        const Source itemSource = mSource.withLine(parser->getLineNumber());
        const std::string& elementNamespace = parser->getElementNamespace();
        const std::string& elementName = parser->getElementName();
        if (elementNamespace.empty() && elementName == "item") {
            Maybe<StringPiece> maybeQuantity = xml::findNonEmptyAttribute(parser, "quantity");
            if (!maybeQuantity) {
                mDiag->error(DiagMessage(itemSource) << "<item> in <plurals> requires attribute "
                             << "'quantity'");
                error = true;
                continue;
            }

            StringPiece trimmedQuantity = util::trimWhitespace(maybeQuantity.value());
            size_t index = 0;
            if (trimmedQuantity == "zero") {
                index = Plural::Zero;
            } else if (trimmedQuantity == "one") {
                index = Plural::One;
            } else if (trimmedQuantity == "two") {
                index = Plural::Two;
            } else if (trimmedQuantity == "few") {
                index = Plural::Few;
            } else if (trimmedQuantity == "many") {
                index = Plural::Many;
            } else if (trimmedQuantity == "other") {
                index = Plural::Other;
            } else {
                mDiag->error(DiagMessage(itemSource)
                             << "<item> in <plural> has invalid value '" << trimmedQuantity
                             << "' for attribute 'quantity'");
                error = true;
                continue;
            }

            if (plural->values[index]) {
                mDiag->error(DiagMessage(itemSource)
                             << "duplicate quantity '" << trimmedQuantity << "'");
                error = true;
                continue;
            }

            if (!(plural->values[index] = parseXml(parser, android::ResTable_map::TYPE_STRING,
                                                   kNoRawString))) {
                error = true;
            }
            plural->values[index]->setSource(itemSource);

        } else if (!shouldIgnoreElement(elementNamespace, elementName)) {
            mDiag->error(DiagMessage(itemSource) << "unknown tag <" << elementNamespace << ":"
                         << elementName << ">");
            error = true;
        }
    }

    if (error) {
        return false;
    }

    outResource->value = std::move(plural);
    return true;
}

bool ResourceParser::parseDeclareStyleable(xml::XmlPullParser* parser,
                                           ParsedResource* outResource) {
    outResource->name.type = ResourceType::kStyleable;

    // Declare-styleable is kPrivate by default, because it technically only exists in R.java.
    outResource->symbolState = SymbolState::kPublic;

    // Declare-styleable only ends up in default config;
    if (outResource->config != ConfigDescription::defaultConfig()) {
        mDiag->warn(DiagMessage(outResource->source) << "ignoring configuration '"
                            << outResource->config << "' for styleable "
                            << outResource->name.entry);
        outResource->config = ConfigDescription::defaultConfig();
    }

    std::unique_ptr<Styleable> styleable = util::make_unique<Styleable>();

    std::string comment;
    bool error = false;
    const size_t depth = parser->getDepth();
    while (xml::XmlPullParser::nextChildNode(parser, depth)) {
        if (parser->getEvent() == xml::XmlPullParser::Event::kComment) {
            comment = util::trimWhitespace(parser->getComment()).toString();
            continue;
        } else if (parser->getEvent() != xml::XmlPullParser::Event::kStartElement) {
            // Ignore text.
            continue;
        }

        const Source itemSource = mSource.withLine(parser->getLineNumber());
        const std::string& elementNamespace = parser->getElementNamespace();
        const std::string& elementName = parser->getElementName();
        if (elementNamespace.empty() && elementName == "attr") {
            Maybe<StringPiece> maybeName = xml::findNonEmptyAttribute(parser, "name");
            if (!maybeName) {
                mDiag->error(DiagMessage(itemSource) << "<attr> tag must have a 'name' attribute");
                error = true;
                continue;
            }

            // If this is a declaration, the package name may be in the name. Separate these out.
            // Eg. <attr name="android:text" />
            Maybe<Reference> maybeRef = parseXmlAttributeName(maybeName.value());
            if (!maybeRef) {
                mDiag->error(DiagMessage(itemSource) << "<attr> tag has invalid name '"
                             << maybeName.value() << "'");
                error = true;
                continue;
            }

            Reference& childRef = maybeRef.value();
            xml::transformReferenceFromNamespace(parser, "", &childRef);

            // Create the ParsedResource that will add the attribute to the table.
            ParsedResource childResource;
            childResource.name = childRef.name.value();
            childResource.source = itemSource;
            childResource.comment = std::move(comment);

            if (!parseAttrImpl(parser, &childResource, true)) {
                error = true;
                continue;
            }

            // Create the reference to this attribute.
            childRef.setComment(childResource.comment);
            childRef.setSource(itemSource);
            styleable->entries.push_back(std::move(childRef));

            outResource->childResources.push_back(std::move(childResource));

        } else if (!shouldIgnoreElement(elementNamespace, elementName)) {
            mDiag->error(DiagMessage(itemSource) << "unknown tag <" << elementNamespace << ":"
                         << elementName << ">");
            error = true;
        }

        comment = {};
    }

    if (error) {
        return false;
    }

    outResource->value = std::move(styleable);
    return true;
}

} // namespace aapt
