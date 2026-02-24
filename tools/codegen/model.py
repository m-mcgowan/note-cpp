"""Data model for the code generator's intermediate representation."""

from __future__ import annotations
from dataclasses import dataclass, field


@dataclass
class PropertyDef:
    """A single property in a request or response schema."""
    wire_name: str          # Original JSON key: "delete", "note", "mode"
    cpp_name: str           # C++ identifier: "delete_", "note_id", "mode"
    cpp_type: str           # "bool", "int32_t", "double", "note::string_view"
    is_optional: bool       # True for request fields (always optional)
    default_value: str | None = None  # String literal of default, e.g. '"data.qi"'
    description: str = ""
    enum_values: list[str] | None = None
    min_api_version: str | None = None
    is_required_by_dispatch: bool = False  # True if x-dispatch requires this prop
    is_body: bool = False  # True for type:object fields (use BodyValue)

    @property
    def version_guard(self) -> str | None:
        """C preprocessor guard for this property, or None if ungated."""
        if not self.min_api_version:
            return None
        parts = self.min_api_version.split(".")
        return f"NOTE_VERSION({parts[0]}, {parts[1]}, {parts[2]})"

    @property
    def getter(self) -> str:
        """JsonReader getter method suffix: 'bool', 'int', 'double', 'string'."""
        return {
            "bool": "bool",
            "int32_t": "int",
            "double": "double",
            "note::string_view": "string",
        }.get(self.cpp_type, "string")

    @property
    def default_arg(self) -> str:
        """Default argument for JsonReader::get_*() calls."""
        if self.default_value is not None:
            return f", {self.default_value}"
        return ""


@dataclass
class BinaryTransferDef:
    """Binary transfer annotation for endpoints with COBS data."""
    direction: str   # "send" or "receive"
    encoding: str    # "cobs"
    follows: str     # "response"
    when: dict | None = None  # e.g. {"binary": true}


@dataclass
class ResponseDef:
    """Response schema for an operation."""
    properties: list[PropertyDef] = field(default_factory=list)
    has_body: bool = False  # True when response includes a body object


_FACTORY_METHOD_RENAMES: dict[str, str] = {
    "delete": "delete_",
}


@dataclass
class OperationDef:
    """A single operation (sub-struct for polymorphic, or top-level for simple)."""
    struct_name: str         # "Query", "Set", "Delete" (polymorphic) or full name
    notecard_request: str    # Wire name: "note.get"
    safety: str              # "ReadOnly", "Idempotent", "NonIdempotent", "Destructive"
    supports_cmd: bool
    properties: list[PropertyDef] = field(default_factory=list)
    response: ResponseDef = field(default_factory=ResponseDef)
    dispatch: dict | None = None
    binary_transfer: BinaryTransferDef | None = None

    @property
    def factory_method(self) -> str:
        """camelCase factory method name for polymorphic sub-type."""
        name = self.struct_name[0].lower() + self.struct_name[1:]
        return _FACTORY_METHOD_RENAMES.get(name, name)


@dataclass
class EndpointGroup:
    """One endpoint. Simple endpoints have one operation; polymorphic have many."""
    wire_name: str           # "note.get"
    struct_name: str         # "NoteGet"
    header_filename: str     # "note_get.hpp"
    is_polymorphic: bool
    operations: list[OperationDef] = field(default_factory=list)

    @property
    def factory_method(self) -> str:
        """camelCase factory method name: 'NoteGet' -> 'noteGet'."""
        name = self.struct_name
        return name[0].lower() + name[1:]
