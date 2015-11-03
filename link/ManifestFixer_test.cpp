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

#include "link/ManifestFixer.h"

#include "test/Builders.h"
#include "test/Context.h"

#include <gtest/gtest.h>

namespace aapt {

struct ManifestFixerTest : public ::testing::Test {
    std::unique_ptr<IAaptContext> mContext;

    void SetUp() override {
        mContext = test::ContextBuilder()
                .setCompilationPackage(u"android")
                .setPackageId(0x01)
                .setNameManglerPolicy(NameManglerPolicy{ u"android" })
                .setSymbolTable(test::StaticSymbolTableBuilder()
                        .addSymbol(u"@android:attr/package", ResourceId(0x01010000),
                                   test::AttributeBuilder()
                                        .setTypeMask(android::ResTable_map::TYPE_STRING)
                                        .build())
                        .addSymbol(u"@android:attr/minSdkVersion", ResourceId(0x01010001),
                                   test::AttributeBuilder()
                                        .setTypeMask(android::ResTable_map::TYPE_STRING |
                                                     android::ResTable_map::TYPE_INTEGER)
                                        .build())
                        .addSymbol(u"@android:attr/targetSdkVersion", ResourceId(0x01010002),
                                   test::AttributeBuilder()
                                        .setTypeMask(android::ResTable_map::TYPE_STRING |
                                                     android::ResTable_map::TYPE_INTEGER)
                                        .build())
                        .addSymbol(u"@android:string/str", ResourceId(0x01060000))
                        .build())
                .build();
    }

    std::unique_ptr<XmlResource> verify(const StringPiece& str) {
        return verifyWithOptions(str, {});
    }

    std::unique_ptr<XmlResource> verifyWithOptions(const StringPiece& str,
                                                   const ManifestFixerOptions& options) {
        std::unique_ptr<XmlResource> doc = test::buildXmlDom(str);
        ManifestFixer fixer(options);
        if (fixer.consume(mContext.get(), doc.get())) {
            return doc;
        }
        return {};
    }
};

TEST_F(ManifestFixerTest, EnsureManifestIsRootTag) {
    EXPECT_EQ(nullptr, verify("<other-tag />"));
    EXPECT_EQ(nullptr, verify("<ns:manifest xmlns:ns=\"com\" />"));
    EXPECT_NE(nullptr, verify("<manifest package=\"android\"></manifest>"));
}

TEST_F(ManifestFixerTest, EnsureManifestHasPackage) {
    EXPECT_NE(nullptr, verify("<manifest package=\"android\" />"));
    EXPECT_NE(nullptr, verify("<manifest package=\"com.android\" />"));
    EXPECT_NE(nullptr, verify("<manifest package=\"com.android.google\" />"));
    EXPECT_EQ(nullptr, verify("<manifest package=\"com.android.google.Class$1\" />"));
    EXPECT_EQ(nullptr,
              verify("<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" "
                     "android:package=\"com.android\" />"));
    EXPECT_EQ(nullptr, verify("<manifest package=\"@string/str\" />"));
}



TEST_F(ManifestFixerTest, UseDefaultSdkVersionsIfNonePresent) {
    ManifestFixerOptions options = { std::u16string(u"8"), std::u16string(u"22") };

    std::unique_ptr<XmlResource> doc = verifyWithOptions(R"EOF(
      <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="android">
        <uses-sdk android:minSdkVersion="7" android:targetSdkVersion="21" />
      </manifest>)EOF", options);
    ASSERT_NE(nullptr, doc);

    xml::Element* el;
    xml::Attribute* attr;

    el = xml::findRootElement(doc->root.get());
    ASSERT_NE(nullptr, el);
    el = el->findChild({}, u"uses-sdk");
    ASSERT_NE(nullptr, el);
    attr = el->findAttribute(xml::kSchemaAndroid, u"minSdkVersion");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(u"7", attr->value);
    attr = el->findAttribute(xml::kSchemaAndroid, u"targetSdkVersion");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(u"21", attr->value);

    doc = verifyWithOptions(R"EOF(
      <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="android">
        <uses-sdk android:targetSdkVersion="21" />
      </manifest>)EOF", options);
    ASSERT_NE(nullptr, doc);

    el = xml::findRootElement(doc->root.get());
    ASSERT_NE(nullptr, el);
    el = el->findChild({}, u"uses-sdk");
    ASSERT_NE(nullptr, el);
    attr = el->findAttribute(xml::kSchemaAndroid, u"minSdkVersion");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(u"8", attr->value);
    attr = el->findAttribute(xml::kSchemaAndroid, u"targetSdkVersion");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(u"21", attr->value);

    doc = verifyWithOptions(R"EOF(
      <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="android">
        <uses-sdk />
      </manifest>)EOF", options);
    ASSERT_NE(nullptr, doc);

    el = xml::findRootElement(doc->root.get());
    ASSERT_NE(nullptr, el);
    el = el->findChild({}, u"uses-sdk");
    ASSERT_NE(nullptr, el);
    attr = el->findAttribute(xml::kSchemaAndroid, u"minSdkVersion");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(u"8", attr->value);
    attr = el->findAttribute(xml::kSchemaAndroid, u"targetSdkVersion");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(u"22", attr->value);

    doc = verifyWithOptions(R"EOF(
      <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="android" />)EOF", options);
    ASSERT_NE(nullptr, doc);

    el = xml::findRootElement(doc->root.get());
    ASSERT_NE(nullptr, el);
    el = el->findChild({}, u"uses-sdk");
    ASSERT_NE(nullptr, el);
    attr = el->findAttribute(xml::kSchemaAndroid, u"minSdkVersion");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(u"8", attr->value);
    attr = el->findAttribute(xml::kSchemaAndroid, u"targetSdkVersion");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(u"22", attr->value);
}

} // namespace aapt
