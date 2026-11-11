// Test-only stand-ins for the epub open TU (RA8FW-1162). Referenced only by
// build.zig: provides the miniz reader, the miniz arena, the decompression
// policy preflight, the XML shim seams, the path join and the guard exports
// the TU links against.
#include <stddef.h>
#include <stdint.h>

typedef uint16_t ra8_err_t;

// knobs
int teststub_fail_preflight = 0;
int teststub_fail_arena_init = 0;
int teststub_fail_init = 0;
int teststub_fail_extract = 0;
int teststub_fail_guard_archive = 0;
int teststub_locate_found = 1;
uint64_t teststub_entry_uncomp = 32;
int teststub_parse_called = 0;
int teststub_arena_deinit_called = 0;
int teststub_zip_end_called = 0;

struct mz_zip_archive_file_stat_prefix {
  uint32_t m_file_index;
  uint64_t m_central_dir_ofs;
  uint16_t m_version_made_by;
  uint16_t m_version_needed;
  uint16_t m_bit_flag;
  uint16_t m_method;
  uint32_t m_crc32;
  uint64_t m_comp_size;
  uint64_t m_uncomp_size;
};

struct mz_zip_archive;
struct epub_miniz_arena;
struct epub_book;
struct epub_xml_workspace;
struct epub_container_result { char opf_path[192]; };

int mz_zip_reader_locate_file(struct mz_zip_archive* zip, const char* name, const char* comment, uint32_t flags) {
  (void)zip; (void)comment; (void)flags;
  return teststub_locate_found ? 0 : -1;
}
int mz_zip_reader_file_stat(struct mz_zip_archive* zip, uint32_t index, struct mz_zip_archive_file_stat_prefix* stat) {
  (void)zip; (void)index;
  stat->m_file_index = 0;
  stat->m_central_dir_ofs = 0;
  stat->m_version_made_by = 0;
  stat->m_version_needed = 0;
  stat->m_bit_flag = 0;
  stat->m_method = 0;
  stat->m_crc32 = 0;
  stat->m_comp_size = 0;
  stat->m_uncomp_size = teststub_entry_uncomp;
  return 1;
}
int mz_zip_reader_extract_to_mem(struct mz_zip_archive* zip, uint32_t index, void* buf, size_t len, uint32_t flags) {
  (void)zip; (void)index; (void)buf; (void)len; (void)flags;
  return teststub_fail_extract ? 0 : 1;
}
int mz_zip_reader_init_mem(struct mz_zip_archive* zip, const void* mem, size_t size, uint32_t flags) {
  (void)zip; (void)mem; (void)size; (void)flags;
  return teststub_fail_init ? 0 : 1;
}
int mz_zip_reader_init(struct mz_zip_archive* zip, uint64_t size, uint32_t flags) {
  (void)zip; (void)size; (void)flags;
  return teststub_fail_init ? 0 : 1;
}
int mz_zip_reader_end(struct mz_zip_archive* zip) { (void)zip; teststub_zip_end_called = 1; return 1; }

ra8_err_t epub_miniz_arena_init(void* arena, void* workspace, size_t workspace_bytes) {
  (void)arena; (void)workspace; (void)workspace_bytes;
  return teststub_fail_arena_init ? 0x5FF : 0;
}
void epub_miniz_arena_deinit(void* arena) { (void)arena; teststub_arena_deinit_called = 1; }
void* epub_miniz_alloc(void* opaque, size_t items, size_t size) { (void)opaque; (void)items; (void)size; return 0; }
void epub_miniz_free(void* opaque, void* address) { (void)opaque; (void)address; }
void* epub_miniz_realloc(void* opaque, void* address, size_t items, size_t size) { (void)opaque; (void)address; (void)items; (void)size; return 0; }

typedef size_t (*ra8_decomp_read_fn)(void* ctx, uint64_t offset, void* buf, size_t len);
ra8_err_t ra8_decomp_zip_entry_preflight(ra8_decomp_read_fn read, void* ctx, uint64_t archive_size) {
  (void)read; (void)ctx; (void)archive_size;
  return teststub_fail_preflight ? 0x5FE : 0;
}

ra8_err_t priv_epub_zip_guard_archive(struct mz_zip_archive* zip) { (void)zip; return teststub_fail_guard_archive ? 0x507 : 0; }
ra8_err_t priv_epub_zip_guard_entry(const struct mz_zip_archive_file_stat_prefix* stat) { (void)stat; return 0; }

ra8_err_t priv_epub_xml_parse_container(const uint8_t* xml, size_t len, struct epub_container_result* out, struct epub_xml_workspace* ws) {
  (void)xml; (void)len; (void)ws;
  const char opf[] = "sub/book.opf";
  for (int i = 0; i < (int)sizeof(opf); i++) out->opf_path[i] = opf[i];
  teststub_parse_called = 1;
  return 0;
}
ra8_err_t priv_epub_xml_parse_opf(const uint8_t* xml, size_t len, struct epub_book* book) { (void)xml; (void)len; (void)book; teststub_parse_called = 2; return 0; }
ra8_err_t priv_epub_xml_parse_ncx(const uint8_t* xml, size_t len, struct epub_book* book) { (void)xml; (void)len; (void)book; teststub_parse_called = 3; return 0; }
ra8_err_t priv_epub_xml_parse_nav(const uint8_t* xml, size_t len, struct epub_book* book) { (void)xml; (void)len; (void)book; teststub_parse_called = 4; return 0; }

void priv_epub_join_path(const char* dir, const char* name, char* dst, size_t cap) {
  size_t n = 0;
  for (size_t i = 0; dir[i] && n + 1 < cap; i++) dst[n++] = dir[i];
  for (size_t i = 0; name[i] && n + 1 < cap; i++) dst[n++] = name[i];
  dst[n] = 0;
}
