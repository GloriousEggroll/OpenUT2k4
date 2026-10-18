"""Unreal Tournament 2004 (UE2.5) package reader and asset exporter.

Reads the user's own locally installed game files and writes intermediate
files (PNG, glTF, WAV/OGG, JSON) for a separate Unreal Editor import step."""

from .package import Package, PackageManager, ObjRef  # noqa: F401

SCALE = 2.0  # UE2 units -> project centimetres
