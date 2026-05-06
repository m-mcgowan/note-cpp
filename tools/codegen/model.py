"""Data model for the code generator's intermediate representation."""

from __future__ import annotations
from dataclasses import dataclass, field


_UNIT_TYPES: dict[str, str] = {
    "minutes": "note::Minutes",
    "seconds": "note::Seconds",
    "milliseconds": "note::Milliseconds",
}


@dataclass
class SubDescription:
    """A value-specific description for an enum or flag field."""
    const_value: str        # e.g. "periodic", "arm"
    description: str        # Human-readable description


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
    format: str | None = None  # "voltage-variable" or "flags"
    flags: list[str] | None = None  # ["arm", "connected", ...] for x-flags fields
    valid_values: list[str] | None = None  # ["arm", "rearm", ...] for consteval validator (superset of flags)
    sub_descriptions: list[SubDescription] | None = None  # per-value docs
    is_array: bool = False  # True for type:array fields (use ArrayField)
    array_max_items: int = 8  # Max elements for ArrayField (overridable via x-max-items)
    toggle: dict | None = None   # x-toggle metadata for paired boolean semantic methods
    action: str | None = None    # x-action method name for standalone boolean trigger
    max_length: int | None = None  # Max expected string length for arena sizing

    @property
    def field_type(self) -> str:
        """C++ type for Field<T> — unit-wrapped if applicable."""
        return _UNIT_TYPES.get(self.unit, self.cpp_type) if self.unit else self.cpp_type

    @property
    def has_format(self) -> bool:
        """True if this property has a voltage-variable format."""
        return self.format == "voltage-variable"

    @property
    def has_flags(self) -> bool:
        """True if this property is a comma-separated flag field."""
        return self.flags is not None and len(self.flags) > 0

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
    def array_element_type(self) -> str:
        """C++ type for array elements. Uses printable_string_view for string
        arrays so elements can be printed on Arduino without note::println()."""
        if self.cpp_type == "note::string_view":
            return "note::printable_string_view"
        return self.cpp_type

    @property
    def version_guard(self) -> str | None:
        """C preprocessor guard for this property, or None if ungated."""
        if not self.min_api_version:
            return None
        parts = self.min_api_version.split(".")
        return f"NOTE_VERSION({parts[0]}, {parts[1]}, {parts[2]})"

    @property
    def is_integral(self) -> bool:
        """True if this property's C++ type is an integer type."""
        return self.cpp_type in (
            "note::json_int_t", "note::json_time_t",
            "int32_t", "uint32_t",
            "int16_t", "uint16_t", "int8_t", "uint8_t",
        )

    @property
    def getter(self) -> str:
        """JsonReader getter method suffix: 'bool', 'int', 'double', 'string'."""
        if self.cpp_type == "bool":
            return "bool"
        if self.is_integral:
            return "int"
        if self.cpp_type == "double":
            return "double"
        return "string"

    @property
    def default_arg(self) -> str:
        """Default argument for JsonReader::get_*() calls."""
        if self.default_value is not None:
            return f", {self.default_value}"
        return ""


@dataclass
class TogglePairDef:
    """Paired boolean fields with semantic no-arg factory methods."""
    true_method: str        # method name for the "enable" side
    false_method: str       # method name for the "disable" side (empty if no pair)
    true_accessor: str      # C++ field accessor name (e.g. "on", "start")
    false_accessor: str     # C++ field accessor name (e.g. "off", "stop")
    combined: str | None    # combined bool method name, or None


@dataclass
class ActionMethodDef:
    """A boolean field with a semantic no-arg method."""
    method: str             # method name (e.g. "resetCounters")
    accessor_name: str      # C++ field accessor name (e.g. "start")


@dataclass
class ImplicitFieldDef:
    """A field auto-emitted in build() with a fixed value (from x-intents)."""
    wire_name: str        # JSON key: "mode", "start"
    value: object         # Python value: "watchdog", True, 42
    cpp_literal: str      # C++ literal: '"watchdog"', 'true', '42'


@dataclass
class AliasDef:
    """A Layer 2 convenience method on a resource group.

    Pre-sets fields on a Layer 1 builder and returns it.
    """
    method_name: str         # "pop", "readTemp"
    endpoint_struct: str     # "NoteGet", "CardTemp"
    variant: str | None      # "Delete", "Get", None (for monomorphic)
    params: list[str]        # ["file"] — wire names of fields to parameterize
    description: str = ""

    @property
    def qualified_type(self) -> str:
        """Fully qualified C++ type name, e.g. 'api::NoteGet::Delete'."""
        if self.variant:
            return f"api::{self.endpoint_struct}::{self.variant}"
        return f"api::{self.endpoint_struct}"

    def resolve_params(self, endpoint: 'EndpointGroup') -> list['PropertyDef']:
        """Resolve param wire names to PropertyDef objects from the endpoint's operations.

        Returns PropertyDef objects for each param, preserving type info for
        test value generation and template rendering.
        """
        from codegen.naming import accessor_name, property_to_cpp_name

        # Find the operation matching our variant
        op = None
        for o in endpoint.operations:
            if self.variant and o.struct_name == self.variant:
                op = o
                break
        if op is None:
            op = endpoint.operations[0]

        prop_by_wire = {p.wire_name: p for p in op.properties}
        resolved = []
        for wire in self.params:
            prop = prop_by_wire.get(wire)
            if prop:
                resolved.append(prop)
            else:
                # Fallback: create a minimal PropertyDef
                cpp_name = property_to_cpp_name(wire)
                resolved.append(PropertyDef(
                    wire_name=wire,
                    cpp_name=cpp_name,
                    cpp_type="note::string_view",
                    is_optional=True,
                ))
        return resolved


@dataclass
class BinaryTransferDef:
    """Binary transfer annotation for endpoints with COBS data."""
    direction: str   # "send" or "receive"
    encoding: str    # "cobs"
    follows: str     # "response"
    when: dict | None = None  # e.g. {"binary": true}

    @property
    def direction_cpp(self) -> str:
        """C++ Direction enum value."""
        return "Direction::Send" if self.direction == "send" else "Direction::Receive"


@dataclass
class BinaryBufferDef:
    """Binary buffer annotation for endpoints that use card.binary as intermediary."""
    direction: str        # "send" (host fills buffer first) or "receive" (Notecard fills it)
    when: dict | None = None  # e.g. {"binary": true} — condition under which buffer is used

    @property
    def direction_cpp(self) -> str:
        """C++ Direction enum value."""
        return "Direction::Send" if self.direction == "send" else "Direction::Receive"


@dataclass
class ResponseDef:
    """Response schema for an operation."""
    properties: list[PropertyDef] = field(default_factory=list)
    has_body: bool = False  # True when response includes a body object
    description: str = ""  # From 200 response description


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


_SKU_RADIOS_MAP: dict[str, str] = {
    "CELL": "Radios::Cell",
    "WIFI": "Radios::WiFi",
    "LORA": "Radios::LoRa",
    "CELL+WIFI": "Radios::CellWifi",
    "SKYLO": "Radios::Skylo",
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
    binary_buffer: BinaryBufferDef | None = None
    skus: list[str] = field(default_factory=list)
    implicit_fields: list[ImplicitFieldDef] = field(default_factory=list)
    description: str = ""  # From operation summary
    legacy_struct_name: str | None = None  # Old verb-derived name for deprecation alias
    mode_prefix: str | None = None  # e.g. "arm" — prepended to mode field in build()
    mode_field_name: str = "triggers"  # C++ name for mode field when mode_prefix is set
    toggle_pairs: list['TogglePairDef'] = field(default_factory=list)
    action_methods: list['ActionMethodDef'] = field(default_factory=list)
    min_api_version: str | None = None  # Operation-level minimum firmware, e.g. "7.5.1"
    intent_name: str | None = None  # Explicit x-intent-name from spec (e.g. "read", "configure"). None when not set.

    @property
    def legacy_factory_method(self) -> str | None:
        """Deprecated factory method name from the old verb-derived struct name.

        Returns None if no legacy name exists (i.e. no rename happened).
        """
        if not self.legacy_struct_name:
            return None
        name = self.legacy_struct_name[0].lower() + self.legacy_struct_name[1:]
        return _FACTORY_METHOD_RENAMES.get(name, name)

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
        """C++ expression for RadiosSupport::from(Radios::...).

        Returns empty string if universal (no SKUs, or all variants covered).
        """
        if not self.skus:
            return ""
        variants: list[str] = []
        for sku in self.skus:
            expr = _SKU_RADIOS_MAP.get(sku)
            if expr:
                variants.append(expr)
        if not variants:
            return ""
        # If all variants are covered, treat as universal
        if len(variants) == len(_SKU_RADIOS_MAP):
            return ""
        return "RadiosSupport::from(" + ", ".join(variants) + ")"

    @property
    def min_firmware_expr(self) -> str:
        """C++ expression for Firmware{major, minor, patch}.

        Returns empty string if no minimum firmware version.
        """
        if not self.min_api_version:
            return ""
        parts = self.min_api_version.split(".")
        if len(parts) < 2:
            return ""
        major = parts[0]
        minor = parts[1]
        patch = parts[2] if len(parts) > 2 else "0"
        return f"Firmware{{{major}, {minor}, {patch}}}"


@dataclass
class EndpointGroup:
    """One endpoint. Simple endpoints have one operation; polymorphic have many."""
    wire_name: str           # "note.get"
    struct_name: str         # "NoteGet"
    header_filename: str     # "note_get.hpp"
    is_polymorphic: bool
    operations: list[OperationDef] = field(default_factory=list)
    aliases: list[AliasDef] = field(default_factory=list)
    flat_alias: str | None = None  # Top-level Api member name (e.g. "binary" for card.binary)
    children: list[EndpointGroup] = field(default_factory=list)  # Nested endpoints (e.g. card.binary -> card.binary.put, card.binary.get)
    is_virtual: bool = False  # Synthesized parent that has no own wire request — exists only to host children (e.g. card.usage parent over card.usage.get + card.usage.test)

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

    parent: EndpointGroup | None = None  # Set when this is a child of another endpoint

    @property
    def api_accessor(self) -> str:
        """The accessor path from the group to this endpoint.

        For use in tests: h.api.{{ group.name }}.{{ endpoint.api_accessor }}

        e.g. 'card.version' → 'version'  (then caller appends () or (args))
             'card.binary' → 'binary'  (member factory, no parens needed)
             'card.binary.put' → 'binary.put'  (child method on parent factory)
             'card.location.mode' → 'location.mode'  (polymorphic child factory)
        """
        if self.parent and self.parent.needs_factory:
            parent_accessor = self.parent.group_method
            return f"{parent_accessor}.{self.child_method}"
        return self.group_method

    @property
    def is_factory_member(self) -> bool:
        """True if this endpoint is a member object (not function) on its container.

        An endpoint is a member (not function) when:
        - It's a parent with children and needs a factory (nc.card.binary)
        - It's a polymorphic child on a parent factory (nc.card.wireless.penalty)
        This enables dot-chaining: nc.card.binary.put(), nc.card.location.mode.periodic()
        """
        if self.needs_factory and self.has_children:
            return True
        if self.parent and self.parent.needs_factory and self.is_polymorphic:
            return True
        return False

    @property
    def has_children(self) -> bool:
        return len(self.children) > 0

    @property
    def needs_factory(self) -> bool:
        """True if this endpoint needs a factory struct.

        Polymorphic endpoints always need one. Endpoints with children need
        one only if they're 2+ segments (nested under a group, like card.binary).
        1-segment parents (like 'web') keep children as flat methods on the group.
        """
        if self.is_polymorphic:
            return True
        if self.has_children and self.wire_name.count(".") >= 1:
            return True
        return False

    _CPP_KEYWORDS = frozenset({
        "auto", "break", "case", "class", "const", "continue", "default",
        "delete", "do", "double", "else", "enum", "extern", "float", "for",
        "goto", "if", "int", "long", "mutable", "namespace", "new", "operator",
        "private", "protected", "public", "register", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "template", "this",
        "throw", "try", "typedef", "union", "unsigned", "virtual", "void",
        "volatile", "while",
    })

    @property
    def child_method(self) -> str:
        """Method name when used as a child of a parent factory.

        e.g. 'card.binary.put' as child of 'card.binary' -> 'put'
             'card.location.track' as child of 'card.location' -> 'track'
             'web.delete' as child of 'web' -> 'delete_'

        For non-polymorphic single-op endpoints with an explicit x-intent-name
        (e.g. card.usage.get with intent "read"), use the intent name in place
        of the wire-name leaf (-> 'read').
        """
        if (not self.is_polymorphic and len(self.operations) == 1
                and self.operations[0].intent_name):
            name = self.operations[0].intent_name
        else:
            name = self.wire_name.rsplit(".", 1)[-1]
        if name in self._CPP_KEYWORDS:
            name += "_"
        return name

    @property
    def skus_rats_expr(self) -> str:
        """Product support expression for this endpoint group.

        Returns the expression from the first operation that has one,
        since all operations in a group share the same SKU constraints.
        """
        for op in self.operations:
            expr = op.skus_rats_expr
            if expr:
                return expr
        return ""

    @property
    def min_firmware_expr(self) -> str:
        """Firmware version expression for this endpoint group.

        Returns the expression from the first operation that has one.
        For polymorphic endpoints, uses the minimum across operations
        (the endpoint itself was introduced at the earliest version).
        """
        for op in self.operations:
            expr = op.min_firmware_expr
            if expr:
                return expr
        return ""


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
