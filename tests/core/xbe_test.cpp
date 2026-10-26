#include "core/sha256.h"
#include "core/xbe.h"
#include "tests/core/check.h"
#include "tests/core/synth.h"

#include <algorithm>

using namespace synth;

namespace {

std::string error_of(const std::vector<uint8_t> &file)
{
    xbrt::Parsed<xbrt::XbeImage> x = xbrt::parse_xbe(file);
    return x ? std::string("(parsed)") : x.error();
}

}  // namespace

TEST(kernel_export_names_come_from_the_generated_table)
{
    CHECK_STR(std::string(xbrt::kernel_export_name({1})), "AvGetSavedDataAddress");
    CHECK_STR(std::string(xbrt::kernel_export_name({8})), "DbgPrint");
    CHECK_STR(std::string(xbrt::kernel_export_name({326})), "XeImageFileName");
    CHECK_STR(std::string(xbrt::kernel_export_name({366})), "HalWriteSMCScratchRegister");
    CHECK_STR(std::string(xbrt::kernel_export_name({0})), "");
    CHECK_STR(std::string(xbrt::kernel_export_name({1000})), "");
}

TEST(parse_xbe_reads_every_field_of_a_synthetic_xbe)
{
    const char16_t title[] = {u'S', u'Y', u'N', u'T', u'H', u' ', 0x00E9, 0xD83C, 0xDFAE};
    std::vector<uint8_t> file = build_xbe(kRetail, std::u16string_view(title, std::size(title)), 0xFFFF0042);
    xbrt::Parsed<xbrt::XbeImage> parsed = xbrt::parse_xbe(file);
    CHECK(parsed.has_value());
    if (!parsed) return;
    const xbrt::XbeImage &x = *parsed;
    CHECK_EQ(x.headers.size(), size_t(0x1000));
    CHECK(std::equal(x.headers.begin(), x.headers.end(), file.begin()));
    CHECK_EQ(x.base.v, kBase);
    CHECK_EQ(x.image_size, 0x3000u);
    CHECK(x.keys == xbrt::XbeKeys::Retail);
    CHECK_EQ(x.entry.v, kTextVa);
    CHECK_EQ(x.thunk_table.v, kRdataVa);
    CHECK_EQ(x.init_flags, 0x5u);
    CHECK_EQ(x.stack_commit, 0x10000u);

    CHECK_EQ(x.cert.title_id, 0xFFFF0042u);
    CHECK_STR(x.title_id_hex(), "FFFF0042");
    CHECK_STR(x.cert.title_name, "SYNTH \xc3\xa9\xf0\x9f\x8e\xae");
    CHECK_EQ(x.cert.game_region, 1u);

    CHECK_EQ(x.sections.size(), size_t(2));
    if (x.sections.size() == 2) {
        const xbrt::XbeSection &t = x.sections[0], &r = x.sections[1];
        CHECK_STR(t.name, ".text");
        CHECK_EQ(t.va.v, kTextVa);
        CHECK_EQ(t.virtual_size, uint32_t(sizeof kText));
        CHECK_EQ(t.raw_offset, 0x1000u);
        CHECK(t.preload && t.executable && !t.writable && !t.inserted_file && !t.head_page_ro && !t.tail_page_ro);
        CHECK_STR(r.name, ".rdata");
        CHECK_EQ(r.virtual_size, kRdataVsize);
        CHECK_EQ(r.raw_size, 16u);
        CHECK(r.preload && !r.executable);
        CHECK(x.section_at({kRdataVa + 0xFF}) == &r);
        CHECK(x.section_at({kRdataVa + 0x100}) == nullptr);
    }

    CHECK_EQ(x.imports.size(), std::size(kOrdinals));
    for (size_t i = 0; i < x.imports.size() && i < std::size(kOrdinals); i++) CHECK_EQ(x.imports[i].v, kOrdinals[i]);

    CHECK_EQ(x.tls.raw_start.v, 0u);
    CHECK_EQ(x.tls.raw_end.v, 0u);
    CHECK_EQ(x.tls.index_var.v, kTlsIndexVar);
    CHECK_EQ(x.tls.zero_fill, kTlsZeroFill);

    CHECK_EQ(x.libraries.size(), size_t(2));
    if (x.libraries.size() == 2) {
        CHECK_STR(x.libraries[0].name, "XAPILIB");
        CHECK_EQ(x.libraries[0].major, 1);
        CHECK_EQ(x.libraries[0].minor, 0);
        CHECK_EQ(x.libraries[0].build, 5849);
        CHECK_EQ(x.libraries[0].qfe, 1);
        CHECK(!x.libraries[0].debug);
        CHECK_STR(x.libraries[1].name, "D3D8");
        CHECK_EQ(x.libraries[1].minor, 2);
        CHECK_EQ(x.libraries[1].build, 4627);
        CHECK_EQ(x.libraries[1].qfe, 3);
        CHECK(x.libraries[1].debug);
    }

    CHECK(x.sha256 == xbrt::sha256(file));
}

TEST(parse_xbe_picks_the_xor_keys_by_xbe_facts_rule)
{
    xbrt::Parsed<xbrt::XbeImage> debug = xbrt::parse_xbe(build_xbe(kDebug));
    CHECK(debug && debug->keys == xbrt::XbeKeys::Debug && debug->entry.v == kTextVa && debug->thunk_table.v == kRdataVa);
    xbrt::Parsed<xbrt::XbeImage> chihiro = xbrt::parse_xbe(build_xbe(kChihiro));
    CHECK(chihiro && chihiro->keys == xbrt::XbeKeys::Chihiro && chihiro->entry.v == kTextVa &&
          chihiro->thunk_table.v == kRdataVa);
}

TEST(parse_xbe_names_the_field_it_rejects)
{
    std::vector<uint8_t> good = build_xbe();
    CHECK_CONTAINS(error_of(std::vector<uint8_t>(good.begin(), good.begin() + 0x100)), "file size");

    std::vector<uint8_t> magic = good;
    magic[3] = 'X';
    CHECK_CONTAINS(error_of(magic), "magic");

    std::vector<uint8_t> base = good;
    put32(base, kBaseField, 0x20000);
    CHECK_CONTAINS(error_of(base), "base");

    std::vector<uint8_t> short_headers(good.begin(), good.begin() + 0x800);
    CHECK_CONTAINS(error_of(short_headers), "headers_size");

    std::vector<uint8_t> cert = good;
    put32(cert, kCertificateField, kBase + 0xFF0);
    CHECK_CONTAINS(error_of(cert), "certificate_addr");

    std::vector<uint8_t> count = good;
    put32(count, kSectionCountField, 0x1000);
    CHECK_CONTAINS(error_of(count), "section_headers_addr");

    std::vector<uint8_t> past_eof = good;
    put32(past_eof, section_header(1) + 16, 0x1001);
    CHECK_CONTAINS(error_of(past_eof), "section 1 (.rdata) raw extent");

    std::vector<uint8_t> past_image = good;
    put32(past_image, section_header(1) + 8, 0x2000);
    CHECK_CONTAINS(error_of(past_image), "section 1 (.rdata) virtual extent");

    std::vector<uint8_t> name = good;
    put32(name, section_header(0) + 20, kBase + 0x2000);
    CHECK_CONTAINS(error_of(name), "section 0 name_addr");

    std::vector<uint8_t> entry = good;
    put32(entry, kEntryField, 0x20000 ^ kRetail.entry);
    CHECK_CONTAINS(error_of(entry), "entry:");

    std::vector<uint8_t> thunk = good;
    put32(thunk, kKernelThunkField, 0x20000 ^ kRetail.thunk);
    CHECK_CONTAINS(error_of(thunk), "kernel_thunk:");

    std::vector<uint8_t> unterminated = good;
    put32(unterminated, kThunkFileOff + 12, 0x80000002);
    CHECK_CONTAINS(error_of(unterminated), "kernel_thunk imports: no zero terminator");

    std::vector<uint8_t> by_name = good;
    put32(by_name, kThunkFileOff + 4, 0x00012340);
    CHECK_CONTAINS(error_of(by_name), "not an ordinal import");

    std::vector<uint8_t> tls = good;
    put32(tls, kTlsOff + 8, 0x30000);
    CHECK_CONTAINS(error_of(tls), "tls index_var");

    std::vector<uint8_t> libs = good;
    put32(libs, kLibraryCountField, 0x100);
    CHECK_CONTAINS(error_of(libs), "lib_versions_addr");

    std::vector<uint8_t> title = good;
    put16(title, kCertOff + 0xC, 0xDC00);
    CHECK_CONTAINS(error_of(title), "title_name");
}

TEST(parse_xbe_reads_imports_only_from_the_thunk_sections_file_data)
{
    std::vector<uint8_t> file = build_xbe();
    put32(file, section_header(1) + 16, 12);
    CHECK_CONTAINS(error_of(file), "no zero terminator");
}
