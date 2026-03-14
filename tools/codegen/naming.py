"""Naming conventions for C++ code generation."""

# C++ reserved words and contextual conflicts that need renaming.
# Maps wire name -> C++ field name.
PROPERTY_RENAMES: dict[str, str] = {
    "delete": "delete_",
    "note": "note_id",
    "template": "template_",
    "class": "class_",
    "new": "new_",
    "operator": "operator_",
    "register": "register_",
    "return": "return_",
}

# Keys in OpenAPI schemas that were renamed from Blues custom keys
# during the forward transform.  The code generator must reverse
# these to recover the original wire names.
X_PREFIX_REVERSE: dict[str, str] = {
    "x-sub-descriptions": "sub-descriptions",
    "x-skus": "skus",
    "x-samples": "samples",
    "x-min-api-version": "minApiVersion",
    "x-annotations": "annotations",
    "x-schema-version": "version",
    "x-api-version": "apiVersion",
}


def endpoint_to_struct_name(endpoint: str) -> str:
    """Convert 'card.location.mode' to 'CardLocationMode'."""
    return "".join(part.capitalize() for part in endpoint.split("."))


def endpoint_to_header_filename(endpoint: str) -> str:
    """Convert 'card.location.mode' to 'card_location_mode.hpp'."""
    return endpoint.replace(".", "_") + ".hpp"


def property_to_cpp_name(wire_name: str) -> str:
    """Convert a JSON property name to a safe C++ identifier."""
    return PROPERTY_RENAMES.get(wire_name, wire_name)


# Map operationId suffixes to user-friendly struct/method names.
# "query" -> "Get" because the action is "get", not "query".
_OPERATION_SUFFIX_MAP: dict[str, str] = {
    "query": "Get",
}


def operation_suffix_to_struct_name(suffix: str) -> str:
    """Convert operationId suffix to a PascalCase sub-struct name.

    e.g. 'query' -> 'Get', 'set' -> 'Set', 'delete' -> 'Delete',
         'create' -> 'Create'
    """
    return _OPERATION_SUFFIX_MAP.get(suffix, suffix.capitalize())


def snake_to_camel(name: str) -> str:
    """Convert snake_case to camelCase: 'note_id' -> 'noteId', 'mode' -> 'mode'."""
    parts = [p for p in name.split("_") if p]
    if not parts:
        return name
    return parts[0] + "".join(p.capitalize() for p in parts[1:])


def accessor_name(cpp_name: str) -> str:
    """camelCase name for the field member and functor accessor.

    Trailing-underscore names (keyword escapes like 'delete_', 'template_')
    are kept as-is.  All other snake_case names are camelCased.

    Examples:
        'mode'      -> 'mode'
        'note_id'   -> 'noteId'
        'delete_'   -> 'delete_'
        'template_' -> 'template_'
    """
    if cpp_name.endswith("_"):
        return cpp_name  # keyword escape — preserve the trailing underscore
    return snake_to_camel(cpp_name)


def nested_type_name(cpp_name: str) -> str:
    """Name of the generated nested functor struct for a property.

    Examples:
        'mode'      -> 'mode_t'
        'note_id'   -> 'noteId_t'
        'delete_'   -> 'delete_t'   (strip trailing _ before appending _t)
    """
    acc = accessor_name(cpp_name)
    return acc.rstrip("_") + "_t"


# C++ keywords and common clashes that need a trailing underscore
# when used as method names on resource groups.
_GROUP_METHOD_RENAMES: dict[str, str] = {
    "delete": "delete_",
    "template": "template_",
    "default": "default_",
    "class": "class_",
    "new": "new_",
}


def wire_name_to_group(wire_name: str) -> str:
    """Extract the group name (first segment) from a wire name.

    e.g. 'card.version' -> 'card', 'hub.sync.status' -> 'hub'
    """
    return wire_name.split(".")[0]


def wire_name_to_group_method(wire_name: str) -> str:
    """Convert wire name to the method name within its resource group.

    Strip the first segment, camelCase the rest, rename keywords.

    e.g. 'card.version' -> 'version'
         'card.location.mode' -> 'locationMode'
         'card.binary.get' -> 'binaryGet'
         'note.delete' -> 'delete_'
         'note.template' -> 'template_'
         'web' -> 'request'  (bare endpoint, no sub-name)
    """
    parts = wire_name.split(".")
    if len(parts) <= 1:
        return "request"  # bare endpoint like "web"
    rest = parts[1:]
    name = rest[0] + "".join(p.capitalize() for p in rest[1:])
    return _GROUP_METHOD_RENAMES.get(name, name)


def schema_key_to_wire_name(key: str) -> str:
    """Reverse x-prefix mapping to recover the original wire name.

    In the OpenAPI spec, some Blues-specific schema property names were
    prefixed with 'x-' (e.g. 'version' -> 'x-schema-version').  When
    generating C++ code that builds/parses JSON, we need the original
    wire name.
    """
    return X_PREFIX_REVERSE.get(key, key)
