"""Data model for the code generator's intermediate representation."""

from __future__ import annotations
from dataclasses import dataclass, field


_UNIT_TYPES: dict[str, str] = {
    "minutes": "note::Minutes",
    "seconds": "note::Seconds",
    "milliseconds": "note::Milliseconds",
}


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
    is_required: bool = False  # True if in schema-level "required" array
    is_body: bool = False  # True for type:object fields (use BodyValue)
    unit: str | None = None  # "minutes", "seconds", "milliseconds"
    constants: dict | None = None  # {"reset": {"value": -1, "description": "..."}}
    format: str | None = None  # "voltage-variable"

    @property
    def field_type(self) -> str:
        """C++ type for Field<T> — unit-wrapped if applicable."""
        return _UNIT_TYPES.get(self.unit, self.cpp_type) if self.unit else self.cpp_type

    @property
    def has_format(self) -> bool:
        """True if this property has a custom format (e.g. voltage-variable)."""
        return self.format is not None

    @property
    def has_unit(self) -> bool:
        """True if this property uses a unit type wrapper."""
        return self.unit is not None and self.unit in _UNIT_TYPES

    @property
    def accessor_name(self) -> str:
        """camelCase field/accessor name (e.g. 'note_id' -> 'noteId')."""
        from codegen.naming import accessor_name
        return accessor_name(self.cpp_name)

    @property
    def nested_type_name(self) -> str:
        """Name of the generated per-field functor nested struct (e.g. 'mode_t')."""
        from codegen.naming import nested_type_name
        return nested_type_name(self.cpp_name)

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

# Action verbs for polymorphic operations (used in action-first methods)
_ACTION_VERBS: dict[str, str] = {
    "Get": "get",
    "Set": "set",
    "Delete": "delete",
    "Create": "create",
}


_SKU_RAT_MAP: dict[str, str] = {
    "CELL": "Rat::Cell",
    "WIFI": "Rat::WiFi",
    "LORA": "Rat::LoRa",
    "CELL+WIFI": "Rat::Cell | Rat::WiFi",
    "SKYLO": "Rat::Cell | Rat::WiFi | Rat::Ntn",
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
    skus: list[str] = field(default_factory=list)

    @property
    def required_properties(self) -> list[PropertyDef]:
        """Properties in the schema's required array (not dispatch-required)."""
        return [p for p in self.properties if p.is_required and not p.is_required_by_dispatch]

    @property
    def optional_properties(self) -> list[PropertyDef]:
        """Non-required, non-dispatch, non-body properties."""
        return [p for p in self.properties
                if not p.is_required and not p.is_required_by_dispatch]

    @property
    def has_version_gated_props(self) -> bool:
        """True if any property has a version guard (needs deprecation pragma)."""
        return any(p.version_guard for p in self.properties)

    @property
    def has_version_gated_response_props(self) -> bool:
        """True if any response property has a version guard."""
        return any(p.version_guard for p in self.response.properties)

    @property
    def factory_method(self) -> str:
        """camelCase factory method name for polymorphic sub-type.

        e.g. 'Get' -> 'get', 'Set' -> 'set', 'Delete' -> 'delete_'
        """
        name = self.struct_name[0].lower() + self.struct_name[1:]
        return _FACTORY_METHOD_RENAMES.get(name, name)

    @property
    def action_verb(self) -> str:
        """Action verb for this operation: 'get', 'set', 'delete', etc."""
        return _ACTION_VERBS.get(self.struct_name, self.struct_name[0].lower() + self.struct_name[1:])

    @property
    def skus_rats_expr(self) -> str:
        """C++ expression for the union of all SKU RATs, e.g. 'Rat::Cell | Rat::WiFi'.

        Returns empty string if universal (no SKUs, or all RATs covered).
        """
        if not self.skus:
            return ""
        # Collect individual Rat bits from each SKU
        bits: set[str] = set()
        for sku in self.skus:
            expr = _SKU_RAT_MAP.get(sku)
            if expr:
                for part in expr.split(" | "):
                    bits.add(part)
        if not bits:
            return ""
        # Stable ordering: Cell, WiFi, Ntn, LoRa
        order = ["Rat::Cell", "Rat::WiFi", "Rat::Ntn", "Rat::LoRa"]
        # If all RATs are covered, treat as universal
        if all(b in bits for b in order):
            return ""
        return " | ".join(b for b in order if b in bits)


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

    def action_first_method(self, op: OperationDef) -> str:
        """Action-first method name: verb + endpoint struct.

        e.g. CardTemp + Get -> 'getCardTemp'
             NoteGet + Delete -> 'deleteNoteGet'  (unusual, but consistent)
        """
        verb = op.action_verb
        return verb + self.struct_name

    @property
    def group_name(self) -> str:
        """Resource group name: first segment of wire_name.

        e.g. 'card.version' -> 'card', 'hub.set' -> 'hub'
        """
        from codegen.naming import wire_name_to_group
        return wire_name_to_group(self.wire_name)

    @property
    def group_method(self) -> str:
        """Method name within the resource group.

        e.g. 'card.version' -> 'version', 'card.location.mode' -> 'locationMode'
        """
        from codegen.naming import wire_name_to_group_method
        return wire_name_to_group_method(self.wire_name)

    @property
    def skus_rats_expr(self) -> str:
        """Union of all operation SKU RATs for this endpoint group."""
        bits: set[str] = set()
        for op in self.operations:
            expr = op.skus_rats_expr
            if expr:
                for part in expr.split(" | "):
                    bits.add(part)
        if not bits:
            return ""
        order = ["Rat::Cell", "Rat::WiFi", "Rat::Ntn", "Rat::LoRa"]
        return " | ".join(b for b in order if b in bits)


@dataclass
class ResourceGroup:
    """A resource group (card, hub, note, etc.) containing related endpoints."""
    name: str                        # "card", "hub", "note", etc.
    struct_name: str                 # "CardGroup", "HubGroup", etc.
    endpoints: list[EndpointGroup] = field(default_factory=list)

    @property
    def conflicts_with_flat(self) -> bool:
        """True if this group name clashes with a flat factory method.

        This happens when a bare endpoint has the same name as its group
        (e.g. 'web' is both a group name and a flat method).
        """
        return any(ep.factory_method == self.name for ep in self.endpoints)
