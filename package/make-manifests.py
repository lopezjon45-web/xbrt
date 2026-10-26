#!/usr/bin/env python3
"""Writes the package manifests for one build: the runtime's own, and the bisect ladder that separates the console's
0x8027025B start refusal one element at a time.

    make-manifests.py <version a.b.c.d> <out dir>

The base is the One S low-memory probe's manifest, a package that starts on this console (codeGeneration, no
extensions, Windows.Xbox first). Every hello shell built from route 2's manifest was refused at start whatever the
toolchain, so each element the runtime adds gets its own identity, installed side by side with the same exe:

    v0    the probe's manifest verbatim with xbrt.exe        a refusal here is the exe, not the manifest
    v1    + windows.protocol                                 (xbrtbisect1:, not xboxrt:, so the hub's scheme has one handler)
    v2    + removableStorage
    v3    + fileTypeAssociation .iso/.xiso                   the runtime's own manifest has exactly v3's elements
    v4    + Wide310x150 tile                                 route 2's manifest had these two as well
    v5    + rescap namespace
    main  LopezJon.XboxRuntime with xboxrt:, removableStorage and the file types
"""
import sys
from pathlib import Path

TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"{rescap_ns}
         IgnorableNamespaces="uap{rescap_ignorable}">
  <Identity Name="{name}" Publisher="CN=LopezJon" Version="{version}" ProcessorArchitecture="x64" />
  <Properties>
    <DisplayName>{display}</DisplayName>
    <PublisherDisplayName>lopezjon45-web</PublisherDisplayName>
    <Logo>Assets\\StoreLogo.png</Logo>
  </Properties>
  <Dependencies>
    <!-- Windows.Xbox first so the console treats the package as a game (game GPU/CPU allocation). -->
    <TargetDeviceFamily Name="Windows.Xbox" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0" />
    <TargetDeviceFamily Name="Windows.Universal" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0" />
  </Dependencies>
  <Resources>
    <Resource Language="en-us" />
  </Resources>
  <Applications>
    <Application Id="App" Executable="xbrt.exe" EntryPoint="Probe.App">
      <uap:VisualElements DisplayName="{display}" Description="{description}"
                          Square150x150Logo="Assets\\Square150x150Logo.png" Square44x44Logo="Assets\\Square44x44Logo.png"
                          BackgroundColor="#101010">{wide_tile}
        <uap:SplashScreen Image="Assets\\SplashScreen.png" BackgroundColor="#101010" />
      </uap:VisualElements>{extensions}
    </Application>
  </Applications>
  <Capabilities>
    <Capability Name="codeGeneration" />{storage}
  </Capabilities>
</Package>
"""

PROTOCOL = """
        <uap:Extension Category="windows.protocol">
          <uap:Protocol Name="{scheme}" />
        </uap:Extension>"""

FILE_TYPES = """
        <uap:Extension Category="windows.fileTypeAssociation">
          <uap:FileTypeAssociation Name="xboxdisc">
            <uap:SupportedFileTypes>
              <uap:FileType>.iso</uap:FileType>
              <uap:FileType>.xiso</uap:FileType>
            </uap:SupportedFileTypes>
          </uap:FileTypeAssociation>
        </uap:Extension>"""

STORAGE = """
    <uap:Capability Name="removableStorage" />"""
WIDE_TILE = """
        <uap:DefaultTile Wide310x150Logo="Assets\\Wide310x150Logo.png" />"""
RESCAP_NS = """
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\""""

LADDER = [
    ("v0", "LopezJon.XbrtBisect0", "xbrt bisect 0: probe manifest", dict()),
    ("v1", "LopezJon.XbrtBisect1", "xbrt bisect 1: +protocol", dict(protocol="xbrtbisect1")),
    ("v2", "LopezJon.XbrtBisect2", "xbrt bisect 2: +removableStorage", dict(protocol="xbrtbisect2", storage=True)),
    ("v3", "LopezJon.XbrtBisect3", "xbrt bisect 3: +file types", dict(protocol="xbrtbisect3", storage=True, filetypes=True)),
    ("v4", "LopezJon.XbrtBisect4", "xbrt bisect 4: +wide tile",
     dict(protocol="xbrtbisect4", storage=True, filetypes=True, wide=True)),
    ("v5", "LopezJon.XbrtBisect5", "xbrt bisect 5: +rescap",
     dict(protocol="xbrtbisect5", storage=True, filetypes=True, wide=True, rescap=True)),
    ("main", "LopezJon.XboxRuntime", "Xbox Runtime", dict(protocol="xboxrt", storage=True, filetypes=True)),
]


def manifest(version, name, display, protocol=None, storage=False, filetypes=False, wide=False, rescap=False):
    extensions = ""
    if protocol:
        extensions += PROTOCOL.format(scheme=protocol)
    if filetypes:
        extensions += FILE_TYPES
    if extensions:
        extensions = "\n      <Extensions>" + extensions + "\n      </Extensions>"
    return TEMPLATE.format(
        version=version, name=name, display=display,
        description="Original Xbox runtime for the One S: runs an XBE from a disc image on E: by direct execution",
        rescap_ns=RESCAP_NS if rescap else "", rescap_ignorable=" rescap" if rescap else "",
        wide_tile=WIDE_TILE if wide else "", extensions=extensions, storage=STORAGE if storage else "")


def main(version, out_dir):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for tag, name, display, elements in LADDER:
        path = out / f"{tag}.xml"
        path.write_text(manifest(version, name, display, **elements), encoding="utf-8")
        print(f"{path}: {name} {version} ({display})")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
