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


def operation_suffix_to_struct_name(suffix: str) -> str:
    """Convert operationId suffix to a PascalCase sub-struct name.

    e.g. 'query' -> 'Query', 'set' -> 'Set', 'delete' -> 'Delete',
         'create' -> 'Create'
    """
    return suffix.capitalize()


def schema_key_to_wire_name(key: str) -> str:
    """Reverse x-prefix mapping to recover the original wire name.

    In the OpenAPI spec, some Blues-specific schema property names were
    prefixed with 'x-' (e.g. 'version' -> 'x-schema-version').  When
    generating C++ code that builds/parses JSON, we need the original
    wire name.
    """
    return X_PREFIX_REVERSE.get(key, key)
