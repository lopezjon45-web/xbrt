// Plain CreateFile2 on E: fails with error 5 in an AppContainer; the FromApp call goes through the broker, which
// grants it with removableStorage and the declared .iso/.xiso file types (vault-one-s/probe/FINDINGS.md, 11-42 ms
// per call).
#pragma once

#include "core/types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xbrt {

// nullptr with `error` set ("CreateFile2FromAppW error 5") when the image cannot be opened.
std::unique_ptr<Source> open_disc_file(std::u16string_view path, std::string &error);

// Every *.iso and *.xiso directly in `folder` (FindFirstFileExFromAppW), full paths, sorted; `error` set when the
// folder cannot be listed.
std::vector<std::u16string> list_disc_files(std::u16string_view folder, std::string &error);

}  // namespace xbrt
