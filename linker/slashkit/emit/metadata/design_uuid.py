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

"""! @brief Design UUID of a vbin.

Every hardware vbin carries a random UUID in its system_map.xml, as a <UUID>
element directly under <SystemMap>.  vrtd remembers the UUID of the design it
wrote to a board and does not write a vbin with the same UUID again.  The UUID
therefore has to change whenever the PDI changes: it is stamped when the vbin
archive is written, never earlier.
"""

from __future__ import annotations

import io
import logging
import os
import tarfile
import tempfile
import uuid
from pathlib import Path
from typing import Optional
import xml.etree.ElementTree as ET

logger = logging.getLogger(__name__)

SYSTEM_MAP_NAME = "system_map.xml"
UUID_TAG = "UUID"


def _stamp_root(root: ET.Element, design_uuid: uuid.UUID) -> None:
    for node in root.findall(UUID_TAG):
        root.remove(node)
    node = ET.Element(UUID_TAG)
    node.text = str(design_uuid)
    root.insert(0, node)


def _serialize(root: ET.Element) -> bytes:
    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")
    out = io.BytesIO()
    tree.write(out, encoding="utf-8", xml_declaration=True)
    return out.getvalue()


def stamp_system_map_xml(
    xml_bytes: bytes, design_uuid: Optional[uuid.UUID] = None
) -> tuple[bytes, uuid.UUID]:
    """! @brief Put a UUID into the text of a system map.

    Any UUID the system map already has is replaced.

    @param xml_bytes Contents of a system_map.xml file.
    @param design_uuid UUID to stamp; a random one is drawn when omitted.
    @return The new file contents and the stamped UUID.
    """
    if design_uuid is None:
        design_uuid = uuid.uuid4()

    root = ET.fromstring(xml_bytes)
    if root.tag != "SystemMap":
        raise ValueError(
            f"Not a system map: root element is <{root.tag}>, expected <SystemMap>"
        )
    _stamp_root(root, design_uuid)
    return _serialize(root), design_uuid


def stamp_system_map_uuid(
    system_map_path: Path, design_uuid: Optional[uuid.UUID] = None
) -> uuid.UUID:
    """! @brief Put a UUID into a system_map.xml file.

    @param system_map_path File to update in place.
    @param design_uuid UUID to stamp; a random one is drawn when omitted.
    @return The stamped UUID.
    """
    xml_bytes, design_uuid = stamp_system_map_xml(
        system_map_path.read_bytes(), design_uuid
    )
    system_map_path.write_bytes(xml_bytes)
    return design_uuid


def read_system_map_uuid(xml_bytes: bytes) -> Optional[uuid.UUID]:
    """! @brief Read the UUID from the text of a system map.

    @param xml_bytes Contents of a system_map.xml file.
    @return The UUID, or None when the system map has none.
    """
    node = ET.fromstring(xml_bytes).find(UUID_TAG)
    if node is None or node.text is None:
        return None
    return uuid.UUID(node.text.strip())


def _is_system_map(member: tarfile.TarInfo) -> bool:
    return member.isfile() and os.path.normpath(member.name) == SYSTEM_MAP_NAME


def _is_gzip(path: Path) -> bool:
    with open(path, "rb") as f:
        return f.read(2) == b"\x1f\x8b"


def restamp_vbin(
    vbin_path: Path, design_uuid: Optional[uuid.UUID] = None
) -> uuid.UUID:
    """! @brief Give an already-built vbin a fresh UUID without relinking.

    Rewrites the archive with a new UUID in its system_map.xml.  Every other
    member is copied unchanged.  The vbin is replaced only once the new
    archive is complete.

    @param vbin_path The vbin to update in place.
    @param design_uuid UUID to stamp; a random one is drawn when omitted.
    @return The stamped UUID.
    """
    vbin_path = Path(vbin_path)
    if design_uuid is None:
        design_uuid = uuid.uuid4()

    fd, tmp_name = tempfile.mkstemp(
        prefix=vbin_path.name + ".", suffix=".tmp", dir=vbin_path.parent
    )
    os.close(fd)
    tmp_path = Path(tmp_name)

    try:
        stamped = False
        write_mode = "w:gz" if _is_gzip(vbin_path) else "w"
        with tarfile.open(vbin_path, "r:*") as src, \
                tarfile.open(tmp_path, write_mode) as dst:
            for member in src:
                if _is_system_map(member):
                    if stamped:
                        raise ValueError(
                            f"{vbin_path} holds more than one {SYSTEM_MAP_NAME}"
                        )
                    xml_bytes, _ = stamp_system_map_xml(
                        src.extractfile(member).read(), design_uuid
                    )
                    member.size = len(xml_bytes)
                    dst.addfile(member, io.BytesIO(xml_bytes))
                    stamped = True
                elif member.isfile():
                    dst.addfile(member, src.extractfile(member))
                else:
                    dst.addfile(member)

        if not stamped:
            raise ValueError(f"{vbin_path} holds no {SYSTEM_MAP_NAME}")

        os.chmod(tmp_path, vbin_path.stat().st_mode & 0o7777)
        os.replace(tmp_path, vbin_path)
    except BaseException:
        tmp_path.unlink(missing_ok=True)
        raise

    logger.info("Stamped UUID %s into %s", design_uuid, vbin_path)
    return design_uuid
