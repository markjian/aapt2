/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "Debug.h"
#include "Diagnostics.h"
#include "Flags.h"
#include "process/IResourceTableConsumer.h"
#include "proto/ProtoSerialize.h"
#include "util/Files.h"
#include "util/StringPiece.h"

#include <vector>

namespace aapt {

//struct DumpOptions {
//
//};

void dumpCompiledFile(const pb::CompiledFile& pbFile, const void* data, size_t len,
                      const Source& source, IAaptContext* context) {
    std::unique_ptr<ResourceFile> file = deserializeCompiledFileFromPb(pbFile, source,
                                                                       context->getDiagnostics());
    if (!file) {
        return;
    }

    std::cout << "Resource: " << file->name << "\n"
              << "Config:   " << file->config << "\n"
              << "Source:   " << file->source << "\n";
}

void dumpCompiledTable(const pb::ResourceTable& pbTable, const Source& source,
                       IAaptContext* context) {
    std::unique_ptr<ResourceTable> table = deserializeTableFromPb(pbTable, source,
                                                                  context->getDiagnostics());
    if (!table) {
        return;
    }

    Debug::printTable(table.get());
}

void tryDumpFile(IAaptContext* context, const std::string& filePath) {
    std::string err;
    Maybe<android::FileMap> file = file::mmapPath(filePath, &err);
    if (!file) {
        context->getDiagnostics()->error(DiagMessage(filePath) << err);
        return;
    }

    android::FileMap* fileMap = &file.value();

    // Try as a compiled table.
    pb::ResourceTable pbTable;
    if (pbTable.ParseFromArray(fileMap->getDataPtr(), fileMap->getDataLength())) {
        dumpCompiledTable(pbTable, Source(filePath), context);
        return;
    }

    // Try as a compiled file.
    CompiledFileInputStream input(fileMap->getDataPtr(), fileMap->getDataLength());
    if (const pb::CompiledFile* pbFile = input.CompiledFile()) {
       dumpCompiledFile(*pbFile, input.data(), input.size(), Source(filePath), context);
       return;
    }
}

class DumpContext : public IAaptContext {
public:
    IDiagnostics* getDiagnostics() override {
        return &mDiagnostics;
    }

    NameMangler* getNameMangler() override {
        abort();
        return nullptr;
    }

    StringPiece16 getCompilationPackage() override {
        return {};
    }

    uint8_t getPackageId() override {
        return 0;
    }

    ISymbolTable* getExternalSymbols() override {
        abort();
        return nullptr;
    }

private:
    StdErrDiagnostics mDiagnostics;
};

/**
 * Entry point for dump command.
 */
int dump(const std::vector<StringPiece>& args) {
    //DumpOptions options;
    Flags flags = Flags();
    if (!flags.parse("aapt2 dump", args, &std::cerr)) {
        return 1;
    }

    DumpContext context;

    for (const std::string& arg : flags.getArgs()) {
        tryDumpFile(&context, arg);
    }
    return 0;
}

} // namespace aapt
