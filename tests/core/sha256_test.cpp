#include "core/sha256.h"
#include "tests/core/check.h"

#include <string>
#include <string_view>
#include <vector>

namespace {

std::string hex_digest(std::span<const uint8_t> data)
{
    std::string out;
    for (uint8_t b : xbrt::sha256(data)) {
        char two[3];
        std::snprintf(two, sizeof two, "%02x", b);
        out += two;
    }
    return out;
}

std::string hex_digest(std::string_view s)
{
    return hex_digest(std::span(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
}

}  // namespace

TEST(sha256_matches_the_fips_180_examples)
{
    CHECK_STR(hex_digest(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_STR(hex_digest("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_STR(hex_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::vector<uint8_t> million(1000000, 'a');
    CHECK_STR(hex_digest(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(sha256_pads_lengths_around_the_block_boundary)
{
    CHECK_STR(hex_digest(std::string(55, 'a')), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK_STR(hex_digest(std::string(56, 'a')), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK_STR(hex_digest(std::string(64, 'a')), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}
