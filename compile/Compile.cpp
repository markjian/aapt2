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

#include <dirent.h>

#include <fstream>
#include <string>

#include "ConfigDescription.h"
#include "Diagnostics.h"
#include "Flags.h"
#include "ResourceParser.h"
#include "ResourceTable.h"
#include "compile/IdAssigner.h"
#include "compile/InlineXmlFormatParser.h"
#include "compile/Png.h"
#include "compile/PseudolocaleGenerator.h"
#include "compile/XmlIdCollector.h"
#include "flatten/Archive.h"
#include "flatten/XmlFlattener.h"
#include "proto/ProtoSerialize.h"
#include "util/Files.h"
#include "util/Maybe.h"
#include "util/Util.h"
#include "xml/XmlDom.h"
#include "xml/XmlPullParser.h"

#include "android-base/errors.h"
#include "android-base/file.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

using google::protobuf::io::CopyingOutputStreamAdaptor;
using google::protobuf::io::ZeroCopyOutputStream;

namespace aapt {

struct ResourcePathData {
  Source source;
  std::string resource_dir;
  std::string name;
  std::string extension;

  // Original config str. We keep this because when we parse the config, we may
  // add on
  // version qualifiers. We want to preserve the original input so the output is
  // easily
  // computed before hand.
  std::string config_str;
  ConfigDescription config;
};

/**
 * Resource file paths are expected to look like:
 * [--/res/]type[-config]/name
 */
static Maybe<ResourcePathData> ExtractResourcePathData(const std::string& path,
                                                       std::string* out_error) {
  std::vector<std::string> parts = util::Split(path, file::sDirSep);
  if (parts.size() < 2) {
    if (out_error) *out_error = "bad resource path";
    return {};
  }

  std::string& dir = parts[parts.size() - 2];
  StringPiece dir_str = dir;

  StringPiece config_str;
  ConfigDescription config;
  size_t dash_pos = dir.find('-');
  if (dash_pos != std::string::npos) {
    config_str = dir_str.substr(dash_pos + 1, dir.size() - (dash_pos + 1));
    if (!ConfigDescription::Parse(config_str, &config)) {
      if (out_error) {
        std::stringstream err_str;
        err_str << "invalid configuration '" << config_str << "'";
        *out_error = err_str.str();
      }
      return {};
    }
    dir_str = dir_str.substr(0, dash_pos);
  }

  std::string& filename = parts[parts.size() - 1];
  StringPiece name = filename;
  StringPiece extension;
  size_t dot_pos = filename.find('.');
  if (dot_pos != std::string::npos) {
    extension = name.substr(dot_pos + 1, filename.size() - (dot_pos + 1));
    name = name.substr(0, dot_pos);
  }

  return ResourcePathData{Source(path),          dir_str.ToString(),
                          name.ToString(),       extension.ToString(),
                          config_str.ToString(), config};
}

struct CompileOptions {
  std::string output_path;
  Maybe<std::string> res_dir;
  bool pseudolocalize = false;
  bool legacy_mode = false;
  bool verbose = false;
};

static std::string BuildIntermediateFilename(const ResourcePathData& data) {
  std::stringstream name;
  name << data.resource_dir;
  if (!data.config_str.empty()) {
    name << "-" << data.config_str;
  }
  name << "_" << data.name;
  if (!data.extension.empty()) {
    name << "." << data.extension;
  }
  name << ".flat";
  return name.str();
}

static bool IsHidden(const StringPiece& filename) {
  return util::StartsWith(filename, ".");
}

/**
 * Walks the res directory structure, looking for resource files.
 */
static bool LoadInputFilesFromDir(
    IAaptContext* context, const CompileOptions& options,
    std::vector<ResourcePathData>* out_path_data) {
  const std::string& root_dir = options.res_dir.value();
  std::unique_ptr<DIR, decltype(closedir)*> d(opendir(root_dir.data()),
                                              closedir);
  if (!d) {
    context->GetDiagnostics()->Error(DiagMessage() << strerror(errno));
    return false;
  }

  while (struct dirent* entry = readdir(d.get())) {
    if (IsHidden(entry->d_name)) {
      continue;
    }

    std::string prefix_path = root_dir;
    file::AppendPath(&prefix_path, entry->d_name);

    if (file::GetFileType(prefix_path) != file::FileType::kDirectory) {
      continue;
    }

    std::unique_ptr<DIR, decltype(closedir)*> subdir(
        opendir(prefix_path.data()), closedir);
    if (!subdir) {
      context->GetDiagnostics()->Error(DiagMessage() << strerror(errno));
      return false;
    }

    while (struct dirent* leaf_entry = readdir(subdir.get())) {
      if (IsHidden(leaf_entry->d_name)) {
        continue;
      }

      std::string full_path = prefix_path;
      file::AppendPath(&full_path, leaf_entry->d_name);

      std::string err_str;
      Maybe<ResourcePathData> path_data =
          ExtractResourcePathData(full_path, &err_str);
      if (!path_data) {
        context->GetDiagnostics()->Error(DiagMessage() << err_str);
        return false;
      }

      out_path_data->push_back(std::move(path_data.value()));
    }
  }
  return true;
}

static bool CompileTable(IAaptContext* context, const CompileOptions& options,
                         const ResourcePathData& path_data,
                         IArchiveWriter* writer,
                         const std::string& output_path) {
  ResourceTable table;
  {
    std::ifstream fin(path_data.source.path, std::ifstream::binary);
    if (!fin) {
      context->GetDiagnostics()->Error(DiagMessage(path_data.source)
                                       << strerror(errno));
      return false;
    }

    // Parse the values file from XML.
    xml::XmlPullParser xml_parser(fin);

    ResourceParserOptions parser_options;
    parser_options.error_on_positional_arguments = !options.legacy_mode;

    // If the filename includes donottranslate, then the default translatable is
    // false.
    parser_options.translatable =
        path_data.name.find("donottranslate") == std::string::npos;

    ResourceParser res_parser(context->GetDiagnostics(), &table,
                              path_data.source, path_data.config,
                              parser_options);
    if (!res_parser.Parse(&xml_parser)) {
      return false;
    }

    fin.close();
  }

  if (options.pseudolocalize) {
    // Generate pseudo-localized strings (en-XA and ar-XB).
    // These are created as weak symbols, and are only generated from default
    // configuration
    // strings and plurals.
    PseudolocaleGenerator pseudolocale_generator;
    if (!pseudolocale_generator.Consume(context, &table)) {
      return false;
    }
  }

  // Ensure we have the compilation package at least.
  table.CreatePackage(context->GetCompilationPackage());

  // Assign an ID to any package that has resources.
  for (auto& pkg : table.packages) {
    if (!pkg->id) {
      // If no package ID was set while parsing (public identifiers), auto
      // assign an ID.
      pkg->id = context->GetPackageId();
    }
  }

  // Create the file/zip entry.
  if (!writer->StartEntry(output_path, 0)) {
    context->GetDiagnostics()->Error(DiagMessage(output_path)
                                     << "failed to open");
    return false;
  }

  // Make sure CopyingOutputStreamAdaptor is deleted before we call
  // writer->FinishEntry().
  {
    // Wrap our IArchiveWriter with an adaptor that implements the
    // ZeroCopyOutputStream
    // interface.
    CopyingOutputStreamAdaptor copying_adaptor(writer);

    std::unique_ptr<pb::ResourceTable> pb_table = SerializeTableToPb(&table);
    if (!pb_table->SerializeToZeroCopyStream(&copying_adaptor)) {
      context->GetDiagnostics()->Error(DiagMessage(output_path)
                                       << "failed to write");
      return false;
    }
  }

  if (!writer->FinishEntry()) {
    context->GetDiagnostics()->Error(DiagMessage(output_path)
                                     << "failed to finish entry");
    return false;
  }
  return true;
}

static bool WriteHeaderAndBufferToWriter(const StringPiece& output_path,
                                         const ResourceFile& file,
                                         const BigBuffer& buffer,
                                         IArchiveWriter* writer,
                                         IDiagnostics* diag) {
  // Start the entry so we can write the header.
  if (!writer->StartEntry(output_path, 0)) {
    diag->Error(DiagMessage(output_path) << "failed to open file");
    return false;
  }

  // Make sure CopyingOutputStreamAdaptor is deleted before we call
  // writer->FinishEntry().
  {
    // Wrap our IArchiveWriter with an adaptor that implements the
    // ZeroCopyOutputStream
    // interface.
    CopyingOutputStreamAdaptor copying_adaptor(writer);
    CompiledFileOutputStream output_stream(&copying_adaptor);

    // Number of CompiledFiles.
    output_stream.WriteLittleEndian32(1);

    std::unique_ptr<pb::CompiledFile> compiled_file =
        SerializeCompiledFileToPb(file);
    output_stream.WriteCompiledFile(compiled_file.get());
    output_stream.WriteData(&buffer);

    if (output_stream.HadError()) {
      diag->Error(DiagMessage(output_path) << "failed to write data");
      return false;
    }
  }

  if (!writer->FinishEntry()) {
    diag->Error(DiagMessage(output_path) << "failed to finish writing data");
    return false;
  }
  return true;
}

static bool WriteHeaderAndMmapToWriter(const StringPiece& output_path,
                                       const ResourceFile& file,
                                       const android::FileMap& map,
                                       IArchiveWriter* writer,
                                       IDiagnostics* diag) {
  // Start the entry so we can write the header.
  if (!writer->StartEntry(output_path, 0)) {
    diag->Error(DiagMessage(output_path) << "failed to open file");
    return false;
  }

  // Make sure CopyingOutputStreamAdaptor is deleted before we call
  // writer->FinishEntry().
  {
    // Wrap our IArchiveWriter with an adaptor that implements the
    // ZeroCopyOutputStream interface.
    CopyingOutputStreamAdaptor copying_adaptor(writer);
    CompiledFileOutputStream output_stream(&copying_adaptor);

    // Number of CompiledFiles.
    output_stream.WriteLittleEndian32(1);

    std::unique_ptr<pb::CompiledFile> compiled_file =
        SerializeCompiledFileToPb(file);
    output_stream.WriteCompiledFile(compiled_file.get());
    output_stream.WriteData(map.getDataPtr(), map.getDataLength());

    if (output_stream.HadError()) {
      diag->Error(DiagMessage(output_path) << "failed to write data");
      return false;
    }
  }

  if (!writer->FinishEntry()) {
    diag->Error(DiagMessage(output_path) << "failed to finish writing data");
    return false;
  }
  return true;
}

static bool FlattenXmlToOutStream(IAaptContext* context,
                                  const StringPiece& output_path,
                                  xml::XmlResource* xmlres,
                                  CompiledFileOutputStream* out) {
  BigBuffer buffer(1024);
  XmlFlattenerOptions xml_flattener_options;
  xml_flattener_options.keep_raw_values = true;
  XmlFlattener flattener(&buffer, xml_flattener_options);
  if (!flattener.Consume(context, xmlres)) {
    return false;
  }

  std::unique_ptr<pb::CompiledFile> pb_compiled_file =
      SerializeCompiledFileToPb(xmlres->file);
  out->WriteCompiledFile(pb_compiled_file.get());
  out->WriteData(&buffer);

  if (out->HadError()) {
    context->GetDiagnostics()->Error(DiagMessage(output_path)
                                     << "failed to write data");
    return false;
  }
  return true;
}

static bool CompileXml(IAaptContext* context, const CompileOptions& options,
                       const ResourcePathData& path_data,
                       IArchiveWriter* writer, const std::string& output_path) {
  if (context->IsVerbose()) {
    context->GetDiagnostics()->Note(DiagMessage(path_data.source)
                                    << "compiling XML");
  }

  std::unique_ptr<xml::XmlResource> xmlres;
  {
    std::ifstream fin(path_data.source.path, std::ifstream::binary);
    if (!fin) {
      context->GetDiagnostics()->Error(DiagMessage(path_data.source)
                                       << strerror(errno));
      return false;
    }

    xmlres = xml::Inflate(&fin, context->GetDiagnostics(), path_data.source);

    fin.close();
  }

  if (!xmlres) {
    return false;
  }

  xmlres->file.name = ResourceName(
      {}, *ParseResourceType(path_data.resource_dir), path_data.name);
  xmlres->file.config = path_data.config;
  xmlres->file.source = path_data.source;

  // Collect IDs that are defined here.
  XmlIdCollector collector;
  if (!collector.Consume(context, xmlres.get())) {
    return false;
  }

  // Look for and process any <aapt:attr> tags and create sub-documents.
  InlineXmlFormatParser inline_xml_format_parser;
  if (!inline_xml_format_parser.Consume(context, xmlres.get())) {
    return false;
  }

  // Start the entry so we can write the header.
  if (!writer->StartEntry(output_path, 0)) {
    context->GetDiagnostics()->Error(DiagMessage(output_path)
                                     << "failed to open file");
    return false;
  }

  // Make sure CopyingOutputStreamAdaptor is deleted before we call
  // writer->FinishEntry().
  {
    // Wrap our IArchiveWriter with an adaptor that implements the
    // ZeroCopyOutputStream
    // interface.
    CopyingOutputStreamAdaptor copying_adaptor(writer);
    CompiledFileOutputStream output_stream(&copying_adaptor);

    std::vector<std::unique_ptr<xml::XmlResource>>& inline_documents =
        inline_xml_format_parser.GetExtractedInlineXmlDocuments();

    // Number of CompiledFiles.
    output_stream.WriteLittleEndian32(1 + inline_documents.size());

    if (!FlattenXmlToOutStream(context, output_path, xmlres.get(),
                               &output_stream)) {
      return false;
    }

    for (auto& inline_xml_doc : inline_documents) {
      if (!FlattenXmlToOutStream(context, output_path, inline_xml_doc.get(),
                                 &output_stream)) {
        return false;
      }
    }
  }

  if (!writer->FinishEntry()) {
    context->GetDiagnostics()->Error(DiagMessage(output_path)
                                     << "failed to finish writing data");
    return false;
  }
  return true;
}

class BigBufferOutputStream : public io::OutputStream {
 public:
  explicit BigBufferOutputStream(BigBuffer* buffer) : buffer_(buffer) {}

  bool Next(void** data, int* len) override {
    size_t count;
    *data = buffer_->NextBlock(&count);
    *len = static_cast<int>(count);
    return true;
  }

  void BackUp(int count) override { buffer_->BackUp(count); }

  google::protobuf::int64 ByteCount() const override {
    return buffer_->size();
  }

  bool HadError() const override { return false; }

 private:
  BigBuffer* buffer_;

  DISALLOW_COPY_AND_ASSIGN(BigBufferOutputStream);
};

static bool CompilePng(IAaptContext* context, const CompileOptions& options,
                       const ResourcePathData& path_data,
                       IArchiveWriter* writer, const std::string& output_path) {
  if (context->IsVerbose()) {
    context->GetDiagnostics()->Note(DiagMessage(path_data.source)
                                    << "compiling PNG");
  }

  BigBuffer buffer(4096);
  ResourceFile res_file;
  res_file.name = ResourceName({}, *ParseResourceType(path_data.resource_dir),
                               path_data.name);
  res_file.config = path_data.config;
  res_file.source = path_data.source;

  {
    std::string content;
    if (!android::base::ReadFileToString(path_data.source.path, &content)) {
      context->GetDiagnostics()->Error(
          DiagMessage(path_data.source)
          << android::base::SystemErrorCodeToString(errno));
      return false;
    }

    BigBuffer crunched_png_buffer(4096);
    BigBufferOutputStream crunched_png_buffer_out(&crunched_png_buffer);

    // Ensure that we only keep the chunks we care about if we end up
    // using the original PNG instead of the crunched one.
    PngChunkFilter png_chunk_filter(content);
    std::unique_ptr<Image> image = ReadPng(context, &png_chunk_filter);
    if (!image) {
      return false;
    }

    std::unique_ptr<NinePatch> nine_patch;
    if (path_data.extension == "9.png") {
      std::string err;
      nine_patch = NinePatch::Create(image->rows.get(), image->width,
                                     image->height, &err);
      if (!nine_patch) {
        context->GetDiagnostics()->Error(DiagMessage() << err);
        return false;
      }

      // Remove the 1px border around the NinePatch.
      // Basically the row array is shifted up by 1, and the length is treated
      // as height - 2.
      // For each row, shift the array to the left by 1, and treat the length as
      // width - 2.
      image->width -= 2;
      image->height -= 2;
      memmove(image->rows.get(), image->rows.get() + 1,
              image->height * sizeof(uint8_t**));
      for (int32_t h = 0; h < image->height; h++) {
        memmove(image->rows[h], image->rows[h] + 4, image->width * 4);
      }

      if (context->IsVerbose()) {
        context->GetDiagnostics()->Note(DiagMessage(path_data.source)
                                        << "9-patch: " << *nine_patch);
      }
    }

    // Write the crunched PNG.
    if (!WritePng(context, image.get(), nine_patch.get(),
                  &crunched_png_buffer_out, {})) {
      return false;
    }

    if (nine_patch != nullptr ||
        crunched_png_buffer_out.ByteCount() <= png_chunk_filter.ByteCount()) {
      // No matter what, we must use the re-encoded PNG, even if it is larger.
      // 9-patch images must be re-encoded since their borders are stripped.
      buffer.AppendBuffer(std::move(crunched_png_buffer));
    } else {
      // The re-encoded PNG is larger than the original, and there is
      // no mandatory transformation. Use the original.
      if (context->IsVerbose()) {
        context->GetDiagnostics()->Note(
            DiagMessage(path_data.source)
            << "original PNG is smaller than crunched PNG"
            << ", using original");
      }

      PngChunkFilter png_chunk_filter_again(content);
      BigBuffer filtered_png_buffer(4096);
      BigBufferOutputStream filtered_png_buffer_out(&filtered_png_buffer);
      io::Copy(&filtered_png_buffer_out, &png_chunk_filter_again);
      buffer.AppendBuffer(std::move(filtered_png_buffer));
    }

    if (context->IsVerbose()) {
      // For debugging only, use the legacy PNG cruncher and compare the
      // resulting file sizes.
      // This will help catch exotic cases where the new code may generate
      // larger PNGs.
      std::stringstream legacy_stream(content);
      BigBuffer legacy_buffer(4096);
      Png png(context->GetDiagnostics());
      if (!png.process(path_data.source, &legacy_stream, &legacy_buffer, {})) {
        return false;
      }

      context->GetDiagnostics()->Note(DiagMessage(path_data.source)
                                      << "legacy=" << legacy_buffer.size()
                                      << " new=" << buffer.size());
    }
  }

  if (!WriteHeaderAndBufferToWriter(output_path, res_file, buffer, writer,
                                    context->GetDiagnostics())) {
    return false;
  }
  return true;
}

static bool CompileFile(IAaptContext* context, const CompileOptions& options,
                        const ResourcePathData& path_data,
                        IArchiveWriter* writer,
                        const std::string& output_path) {
  if (context->IsVerbose()) {
    context->GetDiagnostics()->Note(DiagMessage(path_data.source)
                                    << "compiling file");
  }

  BigBuffer buffer(256);
  ResourceFile res_file;
  res_file.name = ResourceName({}, *ParseResourceType(path_data.resource_dir),
                               path_data.name);
  res_file.config = path_data.config;
  res_file.source = path_data.source;

  std::string error_str;
  Maybe<android::FileMap> f = file::MmapPath(path_data.source.path, &error_str);
  if (!f) {
    context->GetDiagnostics()->Error(DiagMessage(path_data.source)
                                     << error_str);
    return false;
  }

  if (!WriteHeaderAndMmapToWriter(output_path, res_file, f.value(), writer,
                                  context->GetDiagnostics())) {
    return false;
  }
  return true;
}

class CompileContext : public IAaptContext {
 public:
  void SetVerbose(bool val) { verbose_ = val; }

  bool IsVerbose() override { return verbose_; }

  IDiagnostics* GetDiagnostics() override { return &diagnostics_; }

  NameMangler* GetNameMangler() override {
    abort();
    return nullptr;
  }

  const std::string& GetCompilationPackage() override {
    static std::string empty;
    return empty;
  }

  uint8_t GetPackageId() override { return 0x0; }

  SymbolTable* GetExternalSymbols() override {
    abort();
    return nullptr;
  }

  int GetMinSdkVersion() override { return 0; }

 private:
  StdErrDiagnostics diagnostics_;
  bool verbose_ = false;
};

/**
 * Entry point for compilation phase. Parses arguments and dispatches to the
 * correct steps.
 */
int Compile(const std::vector<StringPiece>& args) {
  CompileContext context;
  CompileOptions options;

  bool verbose = false;
  Flags flags =
      Flags()
          .RequiredFlag("-o", "Output path", &options.output_path)
          .OptionalFlag("--dir", "Directory to scan for resources",
                        &options.res_dir)
          .OptionalSwitch("--pseudo-localize",
                          "Generate resources for pseudo-locales "
                          "(en-XA and ar-XB)",
                          &options.pseudolocalize)
          .OptionalSwitch(
              "--legacy",
              "Treat errors that used to be valid in AAPT as warnings",
              &options.legacy_mode)
          .OptionalSwitch("-v", "Enables verbose logging", &verbose);
  if (!flags.Parse("aapt2 compile", args, &std::cerr)) {
    return 1;
  }

  context.SetVerbose(verbose);

  std::unique_ptr<IArchiveWriter> archive_writer;

  std::vector<ResourcePathData> input_data;
  if (options.res_dir) {
    if (!flags.GetArgs().empty()) {
      // Can't have both files and a resource directory.
      context.GetDiagnostics()->Error(DiagMessage()
                                      << "files given but --dir specified");
      flags.Usage("aapt2 compile", &std::cerr);
      return 1;
    }

    if (!LoadInputFilesFromDir(&context, options, &input_data)) {
      return 1;
    }

    archive_writer = CreateZipFileArchiveWriter(context.GetDiagnostics(),
                                                options.output_path);

  } else {
    input_data.reserve(flags.GetArgs().size());

    // Collect data from the path for each input file.
    for (const std::string& arg : flags.GetArgs()) {
      std::string error_str;
      if (Maybe<ResourcePathData> path_data =
              ExtractResourcePathData(arg, &error_str)) {
        input_data.push_back(std::move(path_data.value()));
      } else {
        context.GetDiagnostics()->Error(DiagMessage() << error_str << " ("
                                                      << arg << ")");
        return 1;
      }
    }

    archive_writer = CreateDirectoryArchiveWriter(context.GetDiagnostics(),
                                                  options.output_path);
  }

  if (!archive_writer) {
    return 1;
  }

  bool error = false;
  for (ResourcePathData& path_data : input_data) {
    if (options.verbose) {
      context.GetDiagnostics()->Note(DiagMessage(path_data.source)
                                     << "processing");
    }

    if (path_data.resource_dir == "values") {
      // Overwrite the extension.
      path_data.extension = "arsc";

      const std::string output_filename = BuildIntermediateFilename(path_data);
      if (!CompileTable(&context, options, path_data, archive_writer.get(),
                        output_filename)) {
        error = true;
      }

    } else {
      const std::string output_filename = BuildIntermediateFilename(path_data);
      if (const ResourceType* type =
              ParseResourceType(path_data.resource_dir)) {
        if (*type != ResourceType::kRaw) {
          if (path_data.extension == "xml") {
            if (!CompileXml(&context, options, path_data, archive_writer.get(),
                            output_filename)) {
              error = true;
            }
          } else if (path_data.extension == "png" ||
                     path_data.extension == "9.png") {
            if (!CompilePng(&context, options, path_data, archive_writer.get(),
                            output_filename)) {
              error = true;
            }
          } else {
            if (!CompileFile(&context, options, path_data, archive_writer.get(),
                             output_filename)) {
              error = true;
            }
          }
        } else {
          if (!CompileFile(&context, options, path_data, archive_writer.get(),
                           output_filename)) {
            error = true;
          }
        }
      } else {
        context.GetDiagnostics()->Error(
            DiagMessage() << "invalid file path '" << path_data.source << "'");
        error = true;
      }
    }
  }

  if (error) {
    return 1;
  }
  return 0;
}

}  // namespace aapt
