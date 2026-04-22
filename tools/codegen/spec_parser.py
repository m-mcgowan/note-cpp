"""Parse an OpenAPI 3.1 spec into the code generator's intermediate model."""

from __future__ import annotations

import json
from collections import defaultdict
from dataclasses import replace
from pathlib import Path

from .model import (
    ActionMethodDef,
    AliasDef,
    BinaryBufferDef,
    BinaryTransferDef,
    EndpointGroup,
    ImplicitFieldDef,
    OperationDef,
    PropertyDef,
    ResponseDef,
    SubDescription,
    TogglePairDef,
)
from .naming import (
    endpoint_to_header_filename,
    endpoint_to_struct_name,
    operation_suffix_to_struct_name,
    property_to_cpp_name,
    schema_key_to_wire_name,
)

SAFETY_MAP: dict[str, str] = {
    "readonly": "ReadOnly",
    "idempotent": "Idempotent",
    "non-idempotent": "NonIdempotent",
    "destructive": "Destructive",
}

# OpenAPI type -> C++ type
TYPE_MAP: dict[str, str] = {
    "boolean": "bool",
    "integer": "note::json_int_t",
    "number": "double",
    "string": "note::string_view",
}

# Default max lengths for response string fields (arena sizing).
# Keyed by wire name. Fields not listed get _DEFAULT_RSP_MAX_LENGTH.
# Override per-field with x-max-length in the spec.
_RSP_STRING_MAX_LENGTHS: dict[str, int] = {
    "status": 80,
    "version": 40,
    "device": 32,
    "sn": 32,
    "sku": 24,
    "board": 32,
    "name": 48,
    "text": 128,
    "payload": 256,
    "mode": 32,
    "why": 80,
    "zone": 48,
    "country": 8,
    "area": 16,
    "email": 64,
    "host": 64,
    "org": 48,
    "product": 64,
    "role": 32,
    "ssid": 48,
    "security": 24,
    "file": 48,
    "note": 48,
    "method": 16,
    "format": 32,
    "movements": 128,
}
_DEFAULT_RSP_MAX_LENGTH = 48


def _load_type_refinements() -> dict[str, str]:
    """Load per-field-name type refinements from metadata/type_refinements.json."""
    path = Path(__file__).parent / "metadata" / "type_refinements.json"
    if not path.exists():
        return {}
    data = json.loads(path.read_text())
    return {k: v["storage"] for k, v in data.items()
            if isinstance(v, dict) and "storage" in v}

_TYPE_REFINEMENTS: dict[str, str] = _load_type_refinements()


def _map_type(schema: dict, wire_name: str = "", extensions: dict | None = None) -> str:
    """Map an OpenAPI property schema to a C++ type string.

    For integer fields, the type is refined in order of precedence:
    1. Per-property x-type.storage in property_extensions.json (highest)
    2. Per-field-name storage in type_refinements.json
    3. Default: note::json_int_t
    """
    t = schema.get("type")
    if isinstance(t, list):
        # Union type like ["string", "object"] — use string_view
        return "note::string_view"
    cpp_type = TYPE_MAP.get(t, "note::string_view")
    # Refine integer types
    if t == "integer":
        # UNIX timestamps get their own type (int64_t even under NOTE_INT32_MATH)
        if schema.get("format") == "unix-time":
            return "note::json_time_t"
        # Per-property override (from property_extensions.json)
        if extensions and "x-type" in extensions:
            storage = extensions["x-type"].get("storage")
            if storage:
                return storage
        # Global per-field-name refinement
        if wire_name in _TYPE_REFINEMENTS:
            return _TYPE_REFINEMENTS[wire_name]
    return cpp_type


def _format_default(value, cpp_type: str) -> str | None:
    """Format a default value as a C++ literal, or None if no default."""
    if value is None:
        return None
    if cpp_type == "bool":
        return "true" if value else "false"
    if cpp_type in ("note::json_int_t", "note::json_time_t", "int32_t", "uint32_t",
                     "int16_t", "uint16_t", "int8_t", "uint8_t"):
        try:
            return str(int(value))
        except (ValueError, TypeError):
            return None
    if cpp_type == "double":
        try:
            return f"{float(value)}"
        except (ValueError, TypeError):
            return None
    if cpp_type == "note::string_view":
        return f'"{value}"'
    return None


def _parse_property(name: str, schema: dict, *,
                    is_request: bool,
                    is_required_by_dispatch: bool = False) -> PropertyDef:
    """Parse a single property schema into a PropertyDef."""
    wire_name = schema_key_to_wire_name(name)
    cpp_type = _map_type(schema, wire_name=wire_name)
    default = schema.get("default")

    # Detect the "body" property with type:object — use BodyValue instead.
    # Only the field named "body" gets this treatment; other object/union
    # fields (like "version" with type:["string","object"]) stay as strings.
    schema_type = schema.get("type")
    is_body = wire_name == "body" and (
        schema_type == "object" or (
            isinstance(schema_type, list) and "object" in schema_type
        )
    )

    # Only auto-emit dispatch-required fields if they're boolean.
    # Non-boolean required fields need user-provided values.
    auto_emit = is_required_by_dispatch and cpp_type == "bool"

    # Parse x-sub-descriptions (per-value docs for enums/flags)
    raw_subs = schema.get("x-sub-descriptions")
    sub_descriptions = None
    if raw_subs:
        sub_descriptions = [
            SubDescription(
                const_value=s["const"],
                description=s.get("description", ""),
            )
            for s in raw_subs
            if s.get("const")  # skip empty-string entries
        ]

    is_array = schema_type == "array"

    # Compute max_length for response string fields (arena sizing)
    max_length = schema.get("x-max-length")
    if not is_request and cpp_type == "note::string_view" and max_length is None:
        max_length = _RSP_STRING_MAX_LENGTHS.get(wire_name, _DEFAULT_RSP_MAX_LENGTH)

    return PropertyDef(
        wire_name=wire_name,
        cpp_name=property_to_cpp_name(wire_name),
        cpp_type=cpp_type,
        is_optional=is_request and not auto_emit,
        default_value=_format_default(default, cpp_type),
        description=schema.get("description", ""),
        enum_values=schema.get("enum"),
        min_api_version=schema.get("x-min-api-version"),
        is_required_by_dispatch=auto_emit,
        is_body=is_body,
        unit=schema.get("x-unit"),
        constants=schema.get("x-constants"),
        format=schema.get("x-format"),
        flags=schema.get("x-flags"),
        valid_values=schema.get("x-valid-values"),
        sub_descriptions=sub_descriptions,
        is_array=is_array,
        array_max_items=schema.get("x-max-items", 8),
        toggle=schema.get("x-toggle"),
        action=schema.get("x-action"),
        max_length=max_length,
    )


def _extract_request_props_from_parameters(
    parameters: list[dict],
    dispatch: dict | None,
) -> list[PropertyDef]:
    """Extract properties from GET-style query parameters."""
    excludes = set(dispatch.get("excludes", [])) if dispatch else set()
    requires = set(dispatch.get("requires", [])) if dispatch else set()

    props = []
    for param in parameters:
        name = param["name"]
        if name in excludes:
            continue
        schema = param.get("schema", {})
        # Merge description from parameter level
        if "description" not in schema and "description" in param:
            schema = {**schema, "description": param["description"]}
        props.append(_parse_property(
            name, schema,
            is_request=True,
            is_required_by_dispatch=name in requires,
        ))
    return props


def _extract_request_props_from_body(
    request_body: dict,
    dispatch: dict | None,
) -> list[PropertyDef]:
    """Extract properties from PUT/POST/DELETE requestBody."""
    excludes = set(dispatch.get("excludes", [])) if dispatch else set()
    requires = set(dispatch.get("requires", [])) if dispatch else set()

    schema = (request_body
              .get("content", {})
              .get("application/json", {})
              .get("schema", {}))
    properties = schema.get("properties", {})
    required_set = set(schema.get("required") or [])

    props = []
    for name, prop_schema in properties.items():
        if name in excludes:
            continue
        prop = _parse_property(
            name, prop_schema,
            is_request=True,
            is_required_by_dispatch=name in requires,
        )
        # Mark schema-level required fields (not dispatch-required, which are
        # handled separately as auto-emitted booleans).
        wire = schema_key_to_wire_name(name)
        if wire in required_set and not prop.is_required_by_dispatch:
            prop.is_required = True
            prop.is_optional = False
        props.append(prop)
    return props


def _extract_response_description(operation: dict) -> str:
    """Extract the 200 response description."""
    resp_200 = operation.get("responses", {}).get("200", {})
    return resp_200.get("description", "")


def _extract_response_props(operation: dict) -> tuple[list[PropertyDef], bool]:
    """Extract response properties from the 200 response.

    Returns (properties, has_body) where has_body is True if the response
    includes a "body" object field.
    """
    resp_200 = operation.get("responses", {}).get("200", {})
    schema = (resp_200
              .get("content", {})
              .get("application/json", {})
              .get("schema", {}))
    properties = schema.get("properties", {})

    props = []
    has_body = False
    for name, prop_schema in properties.items():
        t = prop_schema.get("type")
        wire_name = schema_key_to_wire_name(name)

        # Include "body" object fields as response body accessors
        if wire_name == "body" and (
            t == "object" or (isinstance(t, list) and "object" in t)
        ):
            has_body = True
            continue  # handled specially in template, not as a regular prop

        # Skip nested objects (except body, handled above)
        if t == "object":
            continue
        if isinstance(t, list):
            continue

        # Handle array-of-string response fields
        if t == "array":
            items_type = prop_schema.get("items", {}).get("type")
            if items_type != "string":
                continue  # only string arrays supported for now
        props.append(_parse_property(name, prop_schema, is_request=False))
    return props, has_body


def _compute_semantic_methods(
    properties: list[PropertyDef],
) -> tuple[list[TogglePairDef], list[ActionMethodDef]]:
    """Build TogglePairDef and ActionMethodDef lists from property x-toggle/x-action."""
    by_group: dict[str, list[PropertyDef]] = defaultdict(list)
    for prop in properties:
        # Skip dispatch-required fields — auto-emitted in build(), not user-settable.
        if prop.toggle and not prop.is_required_by_dispatch:
            by_group[prop.toggle["group"]].append(prop)

    pairs = []
    for group_props in by_group.values():
        if len(group_props) == 2:
            # The side with "combined" is the "true" (enable) side
            p_true = next(
                (p for p in group_props if p.toggle.get("combined")),
                group_props[0],
            )
            p_false = next(p for p in group_props if p is not p_true)
            pairs.append(TogglePairDef(
                true_method=p_true.toggle["method"],
                false_method=p_false.toggle["method"],
                true_accessor=p_true.accessor_name,
                false_accessor=p_false.accessor_name,
                combined=p_true.toggle.get("combined"),
            ))
        elif len(group_props) == 1:
            # Single-sided toggle (e.g. ntn.gps) — just a no-arg setter
            p = group_props[0]
            pairs.append(TogglePairDef(
                true_method=p.toggle["method"],
                false_method="",
                true_accessor=p.accessor_name,
                false_accessor="",
                combined=p.toggle.get("combined"),
            ))

    actions = []
    for prop in properties:
        # Skip dispatch-required fields — they are auto-emitted in build(), not
        # declared as user-settable struct members, so a method can't reference them.
        if prop.action and not prop.is_required_by_dispatch:
            actions.append(ActionMethodDef(
                method=prop.action,
                accessor_name=prop.accessor_name,
            ))

    return pairs, actions


def _parse_binary_transfer(bt: dict | None) -> BinaryTransferDef | None:
    """Parse x-binary-transfer extension."""
    if not bt:
        return None
    return BinaryTransferDef(
        direction=bt["direction"],
        encoding=bt["encoding"],
        follows=bt["follows"],
        when=bt.get("when"),
    )


def _parse_binary_buffer(bb: dict | None) -> BinaryBufferDef | None:
    """Parse x-binary-buffer extension."""
    if not bb:
        return None
    return BinaryBufferDef(
        direction=bb["direction"],
        when=bb.get("when"),
    )


def _parse_operation(op: dict, *, suffix: str | None = None) -> OperationDef:
    """Parse a single OpenAPI operation into an OperationDef."""
    notecard_request = op["x-notecard-request"]
    safety = SAFETY_MAP[op["x-safety"]]
    supports_cmd = op.get("x-supports-cmd", False)
    dispatch = op.get("x-dispatch")

    # Extract request properties
    if "parameters" in op:
        req_props = _extract_request_props_from_parameters(
            op["parameters"], dispatch)
    elif "requestBody" in op:
        req_props = _extract_request_props_from_body(
            op["requestBody"], dispatch)
    else:
        req_props = []

    # Extract response properties
    rsp_props, has_body_response = _extract_response_props(op)

    # Determine struct name — x-intent-name overrides the suffix-derived name
    intent_name = op.get("x-intent-name")
    legacy_struct_name = None
    if intent_name:
        struct_name = intent_name[0].upper() + intent_name[1:]
        if suffix:
            old_name = operation_suffix_to_struct_name(suffix)
            if old_name != struct_name:
                legacy_struct_name = old_name
    elif suffix:
        struct_name = operation_suffix_to_struct_name(suffix)
    else:
        struct_name = endpoint_to_struct_name(notecard_request)

    toggle_pairs, action_methods = _compute_semantic_methods(req_props)

    return OperationDef(
        struct_name=struct_name,
        notecard_request=notecard_request,
        safety=safety,
        supports_cmd=supports_cmd,
        properties=req_props,
        response=ResponseDef(
            properties=rsp_props,
            has_body=has_body_response,
            description=_extract_response_description(op),
        ),
        dispatch=dispatch,
        binary_transfer=_parse_binary_transfer(op.get("x-binary-transfer")),
        binary_buffer=_parse_binary_buffer(op.get("x-binary-buffer")),
        skus=op.get("x-skus", []),
        min_api_version=op.get("x-min-api-version"),
        description=op.get("summary", ""),
        legacy_struct_name=legacy_struct_name,
        toggle_pairs=toggle_pairs,
        action_methods=action_methods,
    )


def _make_implicit_field(wire_name: str, value: object) -> ImplicitFieldDef:
    """Create an ImplicitFieldDef from a JSON value."""
    if isinstance(value, bool):
        return ImplicitFieldDef(wire_name, value, "true" if value else "false")
    if isinstance(value, int):
        return ImplicitFieldDef(wire_name, value, str(value))
    if isinstance(value, float):
        return ImplicitFieldDef(wire_name, value, str(value))
    # String
    return ImplicitFieldDef(wire_name, str(value), f'"{value}"')


def _expand_intents(
    base_op: OperationDef,
    intents: list[dict],
    all_req_props: list[PropertyDef],
    all_rsp_props: list[PropertyDef],
    rsp_has_body: bool,
) -> list[OperationDef]:
    """Expand x-intents into per-intent OperationDef objects.

    Each intent selects a subset of request fields, defines implicit field
    values, and selects a subset of response fields.
    """
    operations = []
    # Index properties by wire_name for fast lookup
    req_by_wire = {p.wire_name: p for p in all_req_props}
    rsp_by_wire = {p.wire_name: p for p in all_rsp_props}

    for intent in intents:
        name = intent["name"]
        struct_name = name[0].upper() + name[1:]

        # Implicit fields — auto-emitted in build()
        implicit = intent.get("implicit", {})
        implicit_fields = [
            _make_implicit_field(k, v)
            for k, v in implicit.items()
        ]
        implicit_wire_names = set(implicit.keys())

        # Request fields — subset of base operation's properties
        intent_field_names = set(intent.get("fields", []))
        mode_prefix = intent.get("mode_prefix")
        # mode_field_name: C++ name for the mode field when mode_prefix is set.
        # Defaults to "triggers" (card.attn), but can be overridden per intent
        # (e.g. "notifications" for card.aux.serial notify).
        mode_cpp_name = intent.get("mode_field_name", "triggers")
        # Per-intent flag filtering: if the intent specifies "flags", only
        # those flags are exposed on the renamed mode field. This prevents
        # mode flags (arm, disarm) from appearing as trigger methods, and
        # keeps notification flags scoped to the notify intent.
        intent_flags = intent.get("flags")

        def _maybe_replace_mode(p: PropertyDef) -> PropertyDef:
            if not (mode_prefix and p.wire_name == "mode"):
                return p
            kwargs: dict = {"cpp_name": mode_cpp_name}
            if intent_flags is not None:
                kwargs["flags"] = intent_flags
            return replace(p, **kwargs)

        req_props = [
            _maybe_replace_mode(p)
            for p in all_req_props
            if p.wire_name in intent_field_names
            and p.wire_name not in implicit_wire_names
        ]

        # Response fields — subset of base response properties
        intent_rsp_names = set(intent.get("response", []))
        rsp_props = [
            p for p in all_rsp_props
            if p.wire_name in intent_rsp_names
        ]

        # Safety: map from spec-style lowercase to codegen PascalCase
        raw_safety = intent.get("safety")
        safety = SAFETY_MAP[raw_safety] if raw_safety else base_op.safety

        toggle_pairs, action_methods = _compute_semantic_methods(req_props)

        operations.append(OperationDef(
            struct_name=struct_name,
            notecard_request=base_op.notecard_request,
            safety=safety,
            supports_cmd=intent.get("supports_cmd", base_op.supports_cmd),
            properties=req_props,
            response=ResponseDef(
                properties=rsp_props,
                has_body="body" in intent_rsp_names if intent_rsp_names else rsp_has_body,
                description=intent.get("description", ""),
            ),
            dispatch=base_op.dispatch,
            binary_transfer=base_op.binary_transfer,
            binary_buffer=base_op.binary_buffer,
            skus=base_op.skus,
            implicit_fields=implicit_fields,
            description=intent.get("description", base_op.description),
            mode_prefix=intent.get("mode_prefix"),
            mode_field_name=intent.get("mode_field_name", "triggers"),
            toggle_pairs=toggle_pairs,
            action_methods=action_methods,
        ))

    return operations


def _extract_suffix(operation_id: str, base_id: str) -> str | None:
    """Extract the suffix from a polymorphic operationId.

    e.g. 'note_get_query' with base 'note_get' -> 'query'
         'card_binary_query' with base 'card_binary' -> 'query'
    """
    if operation_id == base_id:
        return None
    prefix = base_id + "_"
    if operation_id.startswith(prefix):
        return operation_id[len(prefix):]
    return None


def parse_spec(spec_path: str | Path, spec_dict: dict | None = None) -> list[EndpointGroup]:
    """Parse the OpenAPI spec and return a list of EndpointGroup objects.

    If spec_dict is provided, it is used directly instead of reading from disk.
    """
    if spec_dict is not None:
        spec = spec_dict
    else:
        with open(spec_path) as f:
            spec = json.load(f)

    # Group operations by x-notecard-request
    groups: dict[str, list[tuple[str, dict]]] = defaultdict(list)
    alias_specs: dict[str, list[dict]] = {}  # req_name -> x-aliases list
    flat_aliases: dict[str, str] = {}         # req_name -> flat alias name
    for path, path_item in spec.get("paths", {}).items():
        # Collect x-aliases and x-flat-alias at the path item level
        path_aliases = path_item.get("x-aliases", [])
        path_flat_alias = path_item.get("x-flat-alias")

        for method, operation in path_item.items():
            if method in ("parameters", "summary", "description", "x-aliases", "x-flat-alias"):
                continue
            req_name = operation.get("x-notecard-request")
            if req_name:
                groups[req_name].append((method, operation))
                if path_aliases and req_name not in alias_specs:
                    alias_specs[req_name] = path_aliases
                if path_flat_alias and req_name not in flat_aliases:
                    flat_aliases[req_name] = path_flat_alias

    endpoints = []
    for wire_name, ops in sorted(groups.items()):
        struct_name = endpoint_to_struct_name(wire_name)
        header_filename = endpoint_to_header_filename(wire_name)
        base_id = wire_name.replace(".", "_")
        is_polymorphic = len(ops) > 1

        operations = []
        for method, op in ops:
            op_id = op.get("operationId", "")
            suffix = _extract_suffix(op_id, base_id) if is_polymorphic else None
            parsed_op = _parse_operation(op, suffix=suffix)

            # Check for x-intents — expand one operation into many.
            intents = op.get("x-intents")
            if intents:
                rsp_props, rsp_has_body = _extract_response_props(op)
                intent_ops = _expand_intents(
                    parsed_op, intents,
                    all_req_props=parsed_op.properties,
                    all_rsp_props=rsp_props,
                    rsp_has_body=rsp_has_body,
                )
                # Keep the base operation (full field set, full response).
                # If the operation already has a dispatch suffix (e.g. "Set"),
                # keep it; otherwise rename to "Request" for the unscoped variant.
                if not suffix:
                    parsed_op.struct_name = "Request"
                operations.append(parsed_op)
                operations.extend(intent_ops)
                is_polymorphic = True
            else:
                operations.append(parsed_op)

        # Parse x-aliases for this endpoint
        aliases = []
        for alias_spec in alias_specs.get(wire_name, []):
            aliases.append(AliasDef(
                method_name=alias_spec["method"],
                endpoint_struct=struct_name,
                variant=alias_spec.get("variant"),
                params=alias_spec.get("params", []),
                description=alias_spec.get("description", ""),
            ))

        endpoints.append(EndpointGroup(
            wire_name=wire_name,
            struct_name=struct_name,
            header_filename=header_filename,
            is_polymorphic=is_polymorphic,
            operations=operations,
            aliases=aliases,
            flat_alias=flat_aliases.get(wire_name),
        ))

    return endpoints
