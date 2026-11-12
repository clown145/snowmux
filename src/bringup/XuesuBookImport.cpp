#if FREEINK_DEVICE_XUESU_300

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WeReadStore.h>
#include <XuesuBookImport.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <expat.h>
#include <mobi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>

// The C decoder owns these allocations until mobi_free/mobi_free_rawml.
extern "C" void* xuesuMobiMalloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
extern "C" void* xuesuMobiCalloc(size_t count, size_t size) {
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
extern "C" void* xuesuMobiRealloc(void* ptr, size_t size) {
  return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

namespace {
constexpr char kMimetype[] = "application/epub+zip";
constexpr char kContainer[] =
    R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">)"
    R"(<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)";
constexpr size_t kCopyBufferBytes = 4096;
constexpr size_t kMaxDocxDocumentBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxDocxImageBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxDocxImages = 64;
// libmobi keeps the source records and reconstructed parts alive together. Larger input can exhaust 8 MiB PSRAM.
constexpr size_t kMaxMobiBytes = 4u * 1024u * 1024u;

bool addText(WeReadStore::StoreOnlyZipWriter& writer, const char* name, std::string_view text) {
  return writer.addBuffer(name, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool writeText(HalFile& output, const std::string_view text) {
  return text.empty() || output.write(text.data(), text.size()) == text.size();
}

bool writeEscaped(HalFile& output, std::string_view text) {
  while (!text.empty()) {
    const size_t pos = text.find_first_of("&<>");
    if (pos == std::string_view::npos) return writeText(output, text);
    const std::string_view replacement = text[pos] == '&' ? "&amp;" : (text[pos] == '<' ? "&lt;" : "&gt;");
    if (!writeText(output, text.substr(0, pos)) || !writeText(output, replacement)) return false;
    text.remove_prefix(pos + 1);
  }
  return true;
}

struct MobiInput {
  HalFile& file;
  uint32_t lastYield = millis();
};

int readMobi(void* cookie, char* dst, int length) {
  auto& input = *static_cast<MobiInput*>(cookie);
  if (millis() - input.lastYield >= 25) {
    delay(1);
    input.lastYield = millis();
  }
  return input.file.read(dst, static_cast<size_t>(length));
}

fpos_t seekMobi(void* cookie, fpos_t offset, int whence) {
  auto& file = static_cast<MobiInput*>(cookie)->file;
  int64_t position = offset;
  switch (whence) {
    case SEEK_SET:
      break;
    case SEEK_CUR:
      position += static_cast<int64_t>(file.position());
      break;
    case SEEK_END:
      position += static_cast<int64_t>(file.fileSize64());
      break;
    default:
      return -1;
  }
  return position >= 0 && static_cast<uint64_t>(position) <= file.fileSize64() && file.seek64(position) ? position : -1;
}

bool addMobiParts(WeReadStore::StoreOnlyZipWriter& writer, const MOBIPart* part, const char* prefix, bool& hasOpf) {
  for (; part; part = part->next) {
    if (part->size == 0) continue;
    const auto meta = mobi_get_filemeta_by_type(part->type);
    char name[64];
    const int size = part->type == T_OPF
                         ? snprintf(name, sizeof(name), "OEBPS/content.opf")
                         : snprintf(name, sizeof(name), "OEBPS/%s%05zu.%s", prefix, part->uid, meta.extension);
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(name) || !writer.addBuffer(name, part->data, part->size)) {
      return false;
    }
    if (part->type == T_OPF) hasOpf = true;
    yield();
  }
  return true;
}

bool convertMobi(const std::string& source, WeReadStore::StoreOnlyZipWriter& writer) {
  std::unique_ptr<MOBIData, decltype(&mobi_free)> book(mobi_init(), mobi_free);
  if (!book) {
    LOG_ERR("IMPORT", "No PSRAM for MOBI decoder");
    return false;
  }
  {
    HalFile file;
    if (!Storage.openFileForRead("IMPORT", source, file) || file.fileSize64() > kMaxMobiBytes) {
      LOG_ERR("IMPORT", "MOBI source is unreadable or exceeds %zu bytes", kMaxMobiBytes);
      return false;
    }
    MobiInput input{file};
    std::unique_ptr<FILE, decltype(&fclose)> stream(funopen(&input, readMobi, nullptr, seekMobi, nullptr), fclose);
    if (!stream) {
      LOG_ERR("IMPORT", "Cannot allocate MOBI input stream");
      return false;
    }
    const auto result = mobi_load_file(book.get(), stream.get());
    if (result != MOBI_SUCCESS || mobi_is_encrypted(book.get()) || mobi_is_replica(book.get())) {
      LOG_ERR("IMPORT", "Unsupported, encrypted or invalid MOBI/KF8 source (result=%d)", result);
      return false;
    }
  }
  std::unique_ptr<MOBIRawml, decltype(&mobi_free_rawml)> raw(mobi_init_rawml(book.get()), mobi_free_rawml);
  if (!raw) {
    LOG_ERR("IMPORT", "No PSRAM for MOBI reconstruction");
    return false;
  }
  const auto result = mobi_parse_rawml_opt(raw.get(), book.get(), true, false, true);
  if (result != MOBI_SUCCESS || !raw->markup) {
    LOG_ERR("IMPORT", "MOBI reconstruction failed (%d)", result);
    return false;
  }
  bool hasOpf = false;
  return addMobiParts(writer, raw->markup, "part", hasOpf) &&
         addMobiParts(writer, raw->flow ? raw->flow->next : nullptr, "flow", hasOpf) &&
         addMobiParts(writer, raw->resources, "resource", hasOpf) && hasOpf;
}

std::string_view wordElement(const XML_Char* name) {
  const std::string_view qualified(name);
  constexpr std::string_view transitional = "http://schemas.openxmlformats.org/wordprocessingml/2006/main|";
  constexpr std::string_view strict = "http://purl.oclc.org/ooxml/wordprocessingml/main|";
  if (qualified.starts_with(transitional)) return qualified.substr(transitional.size());
  if (qualified.starts_with(strict)) return qualified.substr(strict.size());
  return {};
}

class DocxXml : public Print {
 public:
  explicit DocxXml(XML_Parser parser) : parser(parser) {
    XML_SetUserData(parser, this);
    XML_SetStartDoctypeDeclHandler(parser, [](void* self, const XML_Char*, const XML_Char*, const XML_Char*, int) {
      static_cast<DocxXml*>(self)->stop();
    });
  }

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t size) override {
    if (millis() - lastYield >= 25) {
      delay(1);
      lastYield = millis();
    }
    return XML_Parse(parser, reinterpret_cast<const char*>(bytes), static_cast<int>(size), XML_FALSE) == XML_STATUS_OK
               ? size
               : 0;
  }

  bool finish() const { return XML_Parse(parser, nullptr, 0, XML_TRUE) == XML_STATUS_OK; }

 protected:
  void stop() const { XML_StopParser(parser, XML_FALSE); }
  XML_Parser parser;

 private:
  uint32_t lastYield = millis();
};

struct DocxImage {
  char id[40];
  char path[129];
  char name[24];
  bool used;
};

class DocxRelationships final : public DocxXml {
 public:
  DocxRelationships(XML_Parser parser, DocxImage* images) : DocxXml(parser), images(images) {
    XML_SetStartElementHandler(parser, startElement);
  }
  size_t count() const { return imageCount; }

 private:
  DocxImage* images;
  size_t imageCount = 0;

  static void startElement(void* user, const XML_Char* name, const XML_Char** attrs) {
    auto& sink = *static_cast<DocxRelationships*>(static_cast<DocxXml*>(user));
    if (std::string_view(name) != "http://schemas.openxmlformats.org/package/2006/relationships|Relationship" ||
        sink.imageCount >= kMaxDocxImages)
      return;
    std::string_view id, target, type, mode;
    for (size_t i = 0; attrs[i]; i += 2) {
      const std::string_view key(attrs[i]);
      if (key == "Id")
        id = attrs[i + 1];
      else if (key == "Target")
        target = attrs[i + 1];
      else if (key == "Type")
        type = attrs[i + 1];
      else if (key == "TargetMode")
        mode = attrs[i + 1];
    }
    if (id.empty() || id.size() >= sizeof(DocxImage::id) || !type.ends_with("/image") ||
        (!mode.empty() && mode != "Internal"))
      return;
    if (target.starts_with("/word/media/")) target.remove_prefix(1);
    const auto path = target.starts_with("media/") ? "word/" + std::string(target) : std::string(target);
    if (!path.starts_with("word/media/") || path.size() >= sizeof(DocxImage::path) ||
        path.find_first_of("\\?#%:") != std::string::npos || FsHelpers::normalisePath(path) != path)
      return;
    const auto extension = FsHelpers::hasJpgExtension(path)   ? "jpg"
                           : FsHelpers::hasPngExtension(path) ? "png"
                           : FsHelpers::hasGifExtension(path) ? "gif"
                                                              : nullptr;
    if (!extension) return;
    auto& image = sink.images[sink.imageCount++];
    memcpy(image.id, id.data(), id.size());
    image.id[id.size()] = '\0';
    memcpy(image.path, path.c_str(), path.size() + 1);
    snprintf(image.name, sizeof(image.name), "word-%zu.%s", sink.imageCount, extension);
  }
};

class DocxText final : public DocxXml {
 public:
  DocxText(XML_Parser parser, HalFile& output, DocxImage* images, size_t imageCount)
      : DocxXml(parser), output(output), images(images), imageCount(imageCount) {
    XML_SetElementHandler(parser, startElement, endElement);
    XML_SetCharacterDataHandler(parser, characterData);
  }

  bool finish() const { return sawDocument && sawParagraph && DocxXml::finish(); }

 private:
  HalFile& output;
  DocxImage* images;
  size_t imageCount;
  bool sawDocument = false;
  bool inBody = false;
  bool sawParagraph = false;
  bool inText = false;

  void emit(std::string_view text) {
    if (!writeText(output, text)) stop();
  }
  void emitImage(const XML_Char** attrs) {
    for (size_t i = 0; attrs[i]; i += 2) {
      const std::string_view key(attrs[i]);
      if (key != "http://schemas.openxmlformats.org/officeDocument/2006/relationships|embed" &&
          key != "http://purl.oclc.org/ooxml/officeDocument/relationships|embed")
        continue;
      for (size_t j = 0; j < imageCount; ++j) {
        if (strcmp(attrs[i + 1], images[j].id) != 0) continue;
        images[j].used = true;
        if (!writeText(output, "<img src=\"") || !writeText(output, images[j].name) ||
            !writeText(output, "\" alt=\"\"/>"))
          stop();
        return;
      }
    }
  }
  static void startElement(void* user, const XML_Char* name, const XML_Char** attrs) {
    auto& sink = *static_cast<DocxText*>(static_cast<DocxXml*>(user));
    const auto local = wordElement(name);
    if (local == "document") sink.sawDocument = true;
    if (local == "body") sink.inBody = true;
    if (!sink.inBody) return;
    if (local == "p") {
      sink.sawParagraph = true;
      sink.emit("<p>");
    } else if (local == "t") {
      sink.inText = true;
    } else if (local == "br" || local == "cr") {
      sink.emit("<br/>");
    } else if (local == "tab") {
      sink.emit("&#160; ");
    } else if (local == "noBreakHyphen") {
      sink.emit("&#8209;");
    } else if (std::string_view(name) == "http://schemas.openxmlformats.org/drawingml/2006/main|blip" ||
               std::string_view(name) == "http://purl.oclc.org/ooxml/drawingml/main|blip") {
      sink.emitImage(attrs);
    }
  }
  static void endElement(void* user, const XML_Char* name) {
    auto& sink = *static_cast<DocxText*>(static_cast<DocxXml*>(user));
    if (!sink.inBody) return;
    const auto local = wordElement(name);
    if (local == "p") sink.emit("</p>\n");
    if (local == "t") sink.inText = false;
    if (local == "body") sink.inBody = false;
  }
  static void characterData(void* user, const XML_Char* data, int size) {
    auto& sink = *static_cast<DocxText*>(static_cast<DocxXml*>(user));
    if (sink.inText && !writeEscaped(sink.output, {data, static_cast<size_t>(size)})) {
      sink.stop();
    }
  }
};

bool convertDocx(const std::string& source, const std::string& workPath, WeReadStore::StoreOnlyZipWriter& writer) {
  constexpr char kDocument[] = "word/document.xml";
  constexpr char kRelationships[] = "word/_rels/document.xml.rels";
  ZipFile docx(source);
  size_t size = 0;
  if (!docx.getInflatedFileSize(kDocument, &size) || size == 0 || size > kMaxDocxDocumentBytes) return false;
  size_t imageCount = 0;
  std::unique_ptr<DocxImage[]> images;
  if (docx.getInflatedFileSize(kRelationships, &size) && size <= 64 * 1024) {
    // Up to 64 fixed-size records are reused for relationship parsing, XHTML and manifest output.
    images = makeUniqueNoThrow<DocxImage[]>(kMaxDocxImages);
    const auto parser = XML_ParserCreateNS(nullptr, '|');
    if (!images || !parser) {
      if (parser) XML_ParserFree(parser);
      LOG_ERR("IMPORT", "No heap for DOCX relationships (%zu bytes)", kMaxDocxImages * sizeof(DocxImage));
      return false;
    }
    const ScopedCleanup freeParser{[parser] { XML_ParserFree(parser); }};
    DocxRelationships relationships(parser, images.get());
    if (docx.readFileToStream(kRelationships, relationships, 512) && relationships.finish()) {
      imageCount = relationships.count();
    }
  }

  const auto textPath = workPath + ".xhtml";
  const auto opfPath = workPath + ".opf";
  const ScopedCleanup cleanup{[&] {
    Storage.remove(textPath.c_str());
    Storage.remove(opfPath.c_str());
  }};
  {
    HalFile output;
    if (!Storage.openFileForWrite("IMPORT", textPath, output)) return false;
    const auto parser = XML_ParserCreateNS(nullptr, '|');
    if (!parser) {
      LOG_ERR("IMPORT", "No heap for DOCX XML parser");
      return false;
    }
    const ScopedCleanup freeParser{[parser] { XML_ParserFree(parser); }};
    DocxText sink(parser, output, images.get(), imageCount);
    if (!writeText(output, R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title></title></head><body>)") ||
        !docx.readFileToStream(kDocument, sink, 1024) || !sink.finish() || !writeText(output, "</body></html>")) {
      return false;
    }
  }
  {
    HalFile output;
    const auto title = FsHelpers::getFileNameWithoutExtension(source);
    if (!Storage.openFileForWrite("IMPORT", opfPath, output) ||
        !writeText(output, R"(<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="source">)"
                           R"(<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="source">docx)"
                           "</dc:identifier><dc:language>und</dc:language><dc:title>") ||
        !writeEscaped(output, title) ||
        !writeText(output,
                   "</dc:title></metadata><manifest>"
                   R"(<item id="body" href="body.xhtml" media-type="application/xhtml+xml"/>)"))
      return false;
    const auto imagePath = workPath + ".image";
    const ScopedCleanup removeImage{[&] { Storage.remove(imagePath.c_str()); }};
    for (size_t i = 0; i < imageCount; ++i) {
      const auto& image = images[i];
      if (!image.used || !docx.getInflatedFileSize(image.path, &size) || size == 0 || size > kMaxDocxImageBytes)
        continue;
      {
        HalFile extracted;
        if (!Storage.openFileForWrite("IMPORT", imagePath, extracted) ||
            !docx.readFileToStream(image.path, extracted, 2048))
          return false;
      }
      const std::string entry = std::string("OEBPS/") + image.name;
      if (!writer.addFile(entry.c_str(), imagePath)) return false;
      const char* mime = FsHelpers::hasJpgExtension(std::string_view(image.name))   ? "image/jpeg"
                         : FsHelpers::hasPngExtension(std::string_view(image.name)) ? "image/png"
                                                                                    : "image/gif";
      char item[128];
      const int length =
          snprintf(item, sizeof(item), "<item id=\"i%zu\" href=\"%s\" media-type=\"%s\"/>", i, image.name, mime);
      if (length <= 0 || static_cast<size_t>(length) >= sizeof(item) || !writeText(output, item)) return false;
    }
    if (!writeText(output, R"(</manifest><spine><itemref idref="body"/></spine></package>)")) return false;
  }
  return writer.addFile("OEBPS/content.opf", opfPath) && writer.addFile("OEBPS/body.xhtml", textPath);
}
}  // namespace

bool xuesu_book::buildArchive(const std::string& source, const std::string& archive) {
  const auto staging = archive + ".tmp";
  // One shared 4 KiB transfer buffer stays alive until the archive's directory is committed.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kCopyBufferBytes);
  if (!buffer) {
    LOG_ERR("IMPORT", "No heap for %zu-byte archive copy buffer", kCopyBufferBytes);
    return false;
  }
  WeReadStore::StoreOnlyZipWriter writer;
  if (!writer.begin(staging, archive + ".central.tmp", buffer.get(), kCopyBufferBytes)) return false;
  const bool docx = FsHelpers::checkFileExtension(source, ".docx");
  if (!addText(writer, "mimetype", kMimetype) || !addText(writer, "META-INF/container.xml", kContainer) ||
      !(docx ? convertDocx(source, archive, writer) : convertMobi(source, writer)) || !writer.finish()) {
    writer.abort();
    LOG_ERR("IMPORT", "Could not convert %s", source.c_str());
    return false;
  }
  if (Storage.rename(staging.c_str(), archive.c_str())) return true;
  Storage.remove(staging.c_str());
  LOG_ERR("IMPORT", "Could not commit converted book");
  return false;
}

#endif
