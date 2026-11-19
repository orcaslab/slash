# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

"""Tests for the design UUID slashkit stamps into a vbin."""

import io
import os
import sys
import tarfile
import uuid
import xml.etree.ElementTree as ET
from types import SimpleNamespace

import pytest

import slashkit.__main__ as slashkit_main
from slashkit.emit.metadata import design_uuid as du
from slashkit.emit.metadata.prog_image import build_vbin

SYSTEM_MAP = b"""<SystemMap>
  <Platform>Hardware</Platform>
  <ShellType>compute</ShellType>
  <ClockFrequency>250000000</ClockFrequency>
  <Kernel>
    <Name>increment_0</Name>
    <BaseAddress>0x20100000000</BaseAddress>
    <Range>0x1000</Range>
  </Kernel>
</SystemMap>
"""

PDI = bytes(range(256)) * 64
REPORT = b"<report><lut>42</lut></report>\n"


def _children(xml_bytes):
    return [(c.tag, (c.text or "").strip()) for c in ET.fromstring(xml_bytes)]


def _make_vbin(path, system_map=SYSTEM_MAP, mode="w:gz"):
    members = [
        ("images/design_partial.pdi", PDI, 0o644),
        ("report_utilization_design.xml", REPORT, 0o600),
    ]
    if system_map is not None:
        members.append((du.SYSTEM_MAP_NAME, system_map, 0o644))
    with tarfile.open(path, mode) as tf:
        for name, data, perm in members:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = perm
            info.mtime = 1700000000
            tf.addfile(info, io.BytesIO(data))
    return path


def _read_vbin(path):
    with tarfile.open(path, "r:*") as tf:
        return [
            (m.name, m.mode, m.mtime, tf.extractfile(m).read())
            for m in tf.getmembers()
        ]


def _vbin_uuid(path):
    for name, _mode, _mtime, data in _read_vbin(path):
        if name == du.SYSTEM_MAP_NAME:
            return du.read_system_map_uuid(data)
    raise AssertionError("no system map in vbin")


# ---- system_map.xml --------------------------------------------------------


def test_stamp_adds_uuid_directly_under_system_map(tmp_path):
    system_map = tmp_path / "system_map.xml"
    system_map.write_bytes(SYSTEM_MAP)

    stamped = du.stamp_system_map_uuid(system_map)

    root = ET.parse(system_map).getroot()
    assert root.tag == "SystemMap"
    nodes = root.findall("UUID")
    assert len(nodes) == 1
    assert uuid.UUID(nodes[0].text) == stamped
    assert stamped.version == 4
    assert stamped != uuid.UUID(int=0)


def test_stamp_keeps_the_rest_of_the_system_map(tmp_path):
    system_map = tmp_path / "system_map.xml"
    system_map.write_bytes(SYSTEM_MAP)

    du.stamp_system_map_uuid(system_map)

    after = [c for c in _children(system_map.read_bytes()) if c[0] != "UUID"]
    assert after == _children(SYSTEM_MAP)
    kernel = ET.parse(system_map).getroot().find("Kernel")
    assert kernel.findtext("Name") == "increment_0"
    assert kernel.findtext("BaseAddress") == "0x20100000000"


def test_stamp_draws_a_new_uuid_each_time(tmp_path):
    system_map = tmp_path / "system_map.xml"
    system_map.write_bytes(SYSTEM_MAP)

    first = du.stamp_system_map_uuid(system_map)
    second = du.stamp_system_map_uuid(system_map)

    assert first != second
    # The old UUID is replaced, not kept next to the new one.
    assert len(ET.parse(system_map).getroot().findall("UUID")) == 1
    assert du.read_system_map_uuid(system_map.read_bytes()) == second


def test_stamp_uses_the_given_uuid():
    wanted = uuid.UUID("12345678-9abc-def0-0123-456789abcdef")

    xml_bytes, stamped = du.stamp_system_map_xml(SYSTEM_MAP, wanted)

    assert stamped == wanted
    assert b"<UUID>12345678-9abc-def0-0123-456789abcdef</UUID>" in xml_bytes


def test_read_uuid_of_unstamped_system_map_is_none():
    assert du.read_system_map_uuid(SYSTEM_MAP) is None


def test_stamp_rejects_a_file_that_is_not_a_system_map():
    with pytest.raises(ValueError):
        du.stamp_system_map_xml(b"<Report><Platform>Hardware</Platform></Report>")


# ---- restamping a built vbin -------------------------------------------------


def test_restamp_gives_an_unstamped_vbin_a_uuid(tmp_path):
    vbin = _make_vbin(tmp_path / "design.vbin")
    assert _vbin_uuid(vbin) is None

    stamped = du.restamp_vbin(vbin)

    assert _vbin_uuid(vbin) == stamped


def test_restamp_replaces_the_uuid(tmp_path):
    old = uuid.uuid4()
    system_map, _ = du.stamp_system_map_xml(SYSTEM_MAP, old)
    vbin = _make_vbin(tmp_path / "design.vbin", system_map)
    assert _vbin_uuid(vbin) == old

    first = du.restamp_vbin(vbin)
    second = du.restamp_vbin(vbin)

    assert len({old, first, second}) == 3
    assert _vbin_uuid(vbin) == second
    system_maps = [m for m in _read_vbin(vbin) if m[0] == du.SYSTEM_MAP_NAME]
    assert len(system_maps) == 1
    assert len(ET.fromstring(system_maps[0][3]).findall("UUID")) == 1


def test_restamp_leaves_other_members_untouched(tmp_path):
    vbin = _make_vbin(tmp_path / "design.vbin")
    before = _read_vbin(vbin)

    du.restamp_vbin(vbin)

    after = _read_vbin(vbin)
    assert [m[:3] for m in after] == [m[:3] for m in before]
    for old, new in zip(before, after):
        if old[0] != du.SYSTEM_MAP_NAME:
            assert new[3] == old[3]
    # The archive is still what libvrt expects: a gzip-compressed tar.
    assert vbin.read_bytes()[:2] == b"\x1f\x8b"
    assert sorted(p.name for p in tmp_path.iterdir()) == ["design.vbin"]


def test_restamp_keeps_an_uncompressed_vbin_uncompressed(tmp_path):
    vbin = _make_vbin(tmp_path / "design.vbin", mode="w")

    stamped = du.restamp_vbin(vbin)

    assert vbin.read_bytes()[:2] != b"\x1f\x8b"
    assert _vbin_uuid(vbin) == stamped


def test_restamp_finds_a_dot_slash_system_map(tmp_path):
    vbin = tmp_path / "design.vbin"
    with tarfile.open(vbin, "w") as tf:
        info = tarfile.TarInfo("./" + du.SYSTEM_MAP_NAME)
        info.size = len(SYSTEM_MAP)
        tf.addfile(info, io.BytesIO(SYSTEM_MAP))

    stamped = du.restamp_vbin(vbin)

    (name, _mode, _mtime, data), = _read_vbin(vbin)
    assert name == "./" + du.SYSTEM_MAP_NAME
    assert du.read_system_map_uuid(data) == stamped


def test_restamp_without_system_map_fails_and_keeps_the_vbin(tmp_path):
    vbin = _make_vbin(tmp_path / "design.vbin", system_map=None)
    before = vbin.read_bytes()

    with pytest.raises(du.VbinStampError, match="holds no system_map.xml"):
        du.restamp_vbin(vbin)

    assert vbin.read_bytes() == before
    assert sorted(p.name for p in tmp_path.iterdir()) == ["design.vbin"]


def test_restamp_follows_a_symlink_to_the_real_vbin(tmp_path):
    real_dir = tmp_path / "builds"
    real_dir.mkdir()
    vbin = _make_vbin(real_dir / "design.vbin")
    link = tmp_path / "current.vbin"
    link.symlink_to(vbin)

    stamped = du.restamp_vbin(link)

    # The link is still a link, and the file it points to carries the UUID.
    assert link.is_symlink()
    assert link.resolve() == vbin.resolve()
    assert _vbin_uuid(vbin) == stamped
    # The temporary file was made next to the real vbin, and is gone.
    assert sorted(p.name for p in real_dir.iterdir()) == ["design.vbin"]
    assert sorted(p.name for p in tmp_path.iterdir()) == ["builds", "current.vbin"]


def test_restamp_refuses_a_vbin_with_several_hard_links(tmp_path):
    vbin = _make_vbin(tmp_path / "design.vbin")
    other = tmp_path / "copy.vbin"
    os.link(vbin, other)
    before = vbin.read_bytes()

    with pytest.raises(du.VbinStampError, match="2 hard links"):
        du.restamp_vbin(vbin)

    assert vbin.read_bytes() == before
    assert other.read_bytes() == before
    assert os.stat(vbin).st_nlink == 2


def test_restamp_syncs_the_new_archive_before_renaming(tmp_path, monkeypatch):
    vbin = _make_vbin(tmp_path / "design.vbin")
    events = []
    real_fsync = os.fsync
    real_replace = os.replace

    def fsync(fd):
        events.append(("fsync", os.path.basename(os.readlink(f"/proc/self/fd/{fd}"))))
        real_fsync(fd)

    def replace(src, dst):
        events.append(("replace", os.path.basename(str(src))))
        real_replace(src, dst)

    monkeypatch.setattr(du.os, "fsync", fsync)
    monkeypatch.setattr(du.os, "replace", replace)

    du.restamp_vbin(vbin)

    kinds = [kind for kind, _ in events]
    assert kinds == ["fsync", "replace", "fsync"]
    # The file synced first is the one renamed over the vbin.
    assert events[0][1] == events[1][1]
    assert events[0][1].startswith("design.vbin.")
    # Then the directory, so the rename itself is durable.
    assert events[2][1] == tmp_path.name


def test_restamp_of_a_file_that_is_not_a_vbin_fails_cleanly(tmp_path):
    vbin = tmp_path / "design.vbin"
    vbin.write_bytes(b"not a tar archive at all" * 100)

    with pytest.raises(du.VbinStampError, match="not a vbin archive"):
        du.restamp_vbin(vbin)

    assert sorted(p.name for p in tmp_path.iterdir()) == ["design.vbin"]


def test_restamp_of_a_missing_file_fails_cleanly(tmp_path):
    with pytest.raises(du.VbinStampError, match="cannot open"):
        du.restamp_vbin(tmp_path / "missing.vbin")


# ---- link time ---------------------------------------------------------------


def _hw_build_dir(tmp_path, project="design"):
    build_dir = tmp_path / f"{project}.prj"
    (build_dir / "images").mkdir(parents=True)
    (build_dir / "images" / f"top_i_slash_slash_{project}_inst_0_partial.pdi").write_bytes(PDI)
    (build_dir / f"report_utilization_{project}.xml").write_bytes(REPORT)
    (build_dir / "system_map.xml").write_bytes(SYSTEM_MAP)
    return SimpleNamespace(
        build_dir=build_dir,
        project_name=project,
        networking_enabled=False,
        user_clk_freq_hz=250_000_000,
        out_path=tmp_path / f"{project}.vbin",
    )


def test_link_stamps_a_uuid_into_the_vbin(tmp_path):
    config = _hw_build_dir(tmp_path)

    build_vbin(config)

    stamped = _vbin_uuid(config.out_path)
    assert stamped is not None and stamped.version == 4
    # The build directory holds the same system map as the archive.
    assert du.read_system_map_uuid(
        (config.build_dir / "system_map.xml").read_bytes()) == stamped


def test_every_linked_vbin_gets_its_own_uuid(tmp_path):
    config = _hw_build_dir(tmp_path)

    build_vbin(config)
    first = _vbin_uuid(config.out_path)
    # Packaging the same build directory again yields a different identity.
    build_vbin(config)
    second = _vbin_uuid(config.out_path)

    assert first != second


# ---- command line -------------------------------------------------------------


def test_stamp_uuid_command_reports_a_missing_system_map_in_one_line(
        tmp_path, monkeypatch, capsys):
    vbin = _make_vbin(tmp_path / "design.vbin", system_map=None)
    monkeypatch.setattr(sys, "argv", ["slashkit", "stamp-uuid", str(vbin)])

    with pytest.raises(SystemExit) as exit_info:
        slashkit_main.main()

    assert exit_info.value.code == 1
    captured = capsys.readouterr()
    assert captured.out == ""
    assert captured.err.count("\n") == 1
    assert captured.err.startswith("slashkit stamp-uuid: error: ")
    assert "holds no system_map.xml" in captured.err
    assert "Traceback" not in captured.err


def test_stamp_uuid_command_reports_hard_links_in_one_line(tmp_path, monkeypatch, capsys):
    vbin = _make_vbin(tmp_path / "design.vbin")
    os.link(vbin, tmp_path / "copy.vbin")
    monkeypatch.setattr(sys, "argv", ["slashkit", "stamp-uuid", str(vbin)])

    with pytest.raises(SystemExit) as exit_info:
        slashkit_main.main()

    assert exit_info.value.code == 1
    err = capsys.readouterr().err
    assert err.count("\n") == 1
    assert "hard links" in err


def test_stamp_uuid_command_prints_the_new_uuid(tmp_path, monkeypatch, capsys):
    vbin = _make_vbin(tmp_path / "design.vbin")
    monkeypatch.setattr(sys, "argv", ["slashkit", "stamp-uuid", str(vbin)])

    slashkit_main.main()

    printed = uuid.UUID(capsys.readouterr().out.strip())
    assert _vbin_uuid(vbin) == printed
