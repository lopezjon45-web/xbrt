#pragma once

#include "core/types.h"

#include <array>
#include <string>
#include <vector>

namespace xbrt {

struct XbeSection {
    std::string name;
    GuestVa va;
    uint32_t virtual_size;
    uint32_t raw_offset, raw_size;
    bool writable, preload, executable, inserted_file, head_page_ro, tail_page_ro;
};

struct XbeLibrary {
    std::string name;
    uint16_t major, minor, build, qfe;
    bool debug;
};

enum class XbeKeys : uint8_t { Retail, Debug, Chihiro };

struct XbeImage {
    // Header bytes [0, headers_size), mapped verbatim at the base.
    std::vector<uint8_t> headers;
    GuestVa base;        // 0x10000 for every retail XBE; parse_xbe rejects anything else
    uint32_t image_size;
    GuestVa entry;       // decoded with the key `keys` names
    GuestVa thunk_table;
    XbeKeys keys;
    std::vector<XbeSection> sections;
    std::vector<XbeLibrary> libraries;
    std::vector<Ordinal> imports;   // read from the file at the thunk table, zero-terminated, in slot order
    struct Tls {
        GuestVa raw_start, raw_end, index_var;
        uint32_t zero_fill;
    } tls;
    struct Certificate {
        uint32_t title_id;
        std::string title_name;   // UTF-8, from the UTF-16 certificate field
        uint32_t game_region;
    } cert;
    uint32_t init_flags;
    uint32_t stack_commit;   // the PE stack commit: the main thread's stack size
    std::array<uint8_t, 32> sha256;   // of the whole file

    // The section holding `va`, or nullptr.
    const XbeSection *section_at(GuestVa va) const;
    // Eight hex digits ("FFFF0042"), as LocalState folders and the survey name titles.
    std::string title_id_hex() const;
    // Each thread's TLS block (PsCreateSystemThreadEx's TlsDataSize): the TLS data rounded up to 16 plus a pointer
    // slot, as XAPI computes it; 0 without a TLS directory.
    uint32_t tls_block_size() const;
};

// Validates every field it returns: the magic, the header size, every section's raw extent against the file and
// its virtual extent against [base, base + image_size), the thunk table inside a section, the import list
// terminated within that section, the certificate and library tables inside the headers.
Parsed<XbeImage> parse_xbe(std::span<const uint8_t> file);

// The xboxkrnl export name for an ordinal, or "" for an ordinal no retail kernel exports.
std::string_view kernel_export_name(Ordinal o);

}  // namespace xbrt
