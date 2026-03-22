#!/usr/bin/env python3
"""Generate C++ request/response types from an OpenAPI spec."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import jinja2

# Allow running as `python3 tools/codegen/generate.py` from repo root
if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from codegen.spec_parser import parse_spec
from codegen.model import ResourceGroup
from codegen.naming import (
    accessor_name as property_to_accessor_name,
    endpoint_to_struct_name,
    operation_suffix_to_struct_name,
    property_to_cpp_name,
    wire_name_to_group,
)


import re
import textwrap


def _strip_markdown(text: str) -> str:
    """Strip markdown formatting for clean doc comments."""
    # Convert markdown links [text](url) to just text
    text = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', text)
    # Strip bold/italic markers
    text = re.sub(r'_\*\*([^*]+)\*\*_', r'\1', text)
    text = re.sub(r'\*\*([^*]+)\*\*', r'\1', text)
    text = re.sub(r'_([^_]+)_', r'\1', text)
    return text


_CPP_KEYWORDS = frozenset({
    "auto", "break", "case", "class", "const", "continue", "default",
    "delete", "do", "double", "else", "enum", "extern", "float", "for",
    "goto", "if", "int", "long", "mutable", "namespace", "new", "operator",
    "private", "protected", "public", "register", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "template", "this",
    "throw", "try", "typedef", "union", "unsigned", "virtual", "void",
    "volatile", "while",
})


def _enum_const_name(value: str) -> str:
    """Sanitize an enum string value into a valid C++ identifier.

    Returns empty string for values that can't be made into identifiers
    (e.g. empty strings, lone punctuation).
    """
    name = value.replace("-", "_").replace("+", "p").replace("?", "unknown")
    if not name:
        return ""
    if name[0].isdigit():
        name = "_" + name
    if name in _CPP_KEYWORDS:
        name = name + "_"
    return name


def _doc_comment_filter(text: str, indent: str = "    ") -> str:
    """Format a description string as wrapped /// doc comment lines.

    Returns a string with NO trailing newline (Jinja2 template handles that).
    """
    if not text:
        return ""
    # Normalize literal \n sequences (some OpenAPI descriptions use escaped
    # newlines instead of real ones)
    text = text.replace("\\n", "\n")
    # Strip markdown formatting for cleaner C++ doc comments
    text = _strip_markdown(text)
    # Collapse markdown-style paragraphs into single lines per paragraph,
    # then wrap each paragraph to ~80 columns accounting for indent + "/// "
    prefix = f"{indent}/// "
    blank = f"{indent}///"
    width = 80
    paragraphs = text.strip().split("\n\n")
    lines = []
    for para in paragraphs:
        if lines:
            lines.append(blank)  # blank comment line between paragraphs
        # Collapse internal newlines within a paragraph
        flat = " ".join(para.split())
        wrapped = textwrap.wrap(flat, width=width - len(prefix))
        for w in wrapped:
            lines.append(f"{prefix}{w}")
    return "\n".join(lines)


def _cpp_request_test_value(prop) -> str:
    """C++ expression for setting a request field setter in generated tests."""
    if prop.has_unit:
        return f'{prop.field_type}{{42}}'
    if prop.is_array:
        return f'note::string_view("x-{prop.wire_name}-item")'
    if prop.has_flags:
        # Use raw string assignment (tests that Field<string_view> still works)
        return f'note::string_view("{prop.flags[0]}")'
    if prop.cpp_type == "bool":
        return "true"
    if prop.cpp_type == "int32_t":
        return "int32_t{42}"
    if prop.cpp_type == "double":
        return "1.5"
    # note::string_view — prefer the first enum value when available
    if prop.enum_values:
        return f'note::string_view("{prop.enum_values[0]}")'
    return f'note::string_view("x-{prop.wire_name}")'


def _reader_test_value(prop) -> str:
    """C++ expression for PopulatedJsonReader::set() in generated response tests."""
    if prop.cpp_type == "bool":
        return "true"
    if prop.cpp_type == "int32_t":
        return "int32_t{42}"
    if prop.cpp_type == "double":
        return "1.5"
    # note::string_view — reader stores std::string
    return f'std::string("x-{prop.wire_name}")'


def _response_match_value(prop) -> str:
    """C++ expression for REQUIRE comparison in generated response tests."""
    if prop.has_unit:
        return f'{prop.field_type}{{42}}'
    if prop.cpp_type == "bool":
        return "true"
    if prop.cpp_type == "int32_t":
        return "42"
    if prop.cpp_type == "double":
        return "1.5"
    # note::string_view — compare against a const char* literal
    return f'"x-{prop.wire_name}"'


def _cpp_literal(value) -> str:
    """Convert a JSON value to a C++ literal string."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return f"int32_t{{{value}}}"
    if isinstance(value, float):
        return f"{value}"
    if isinstance(value, str):
        # Use raw string literal with note:: prefix to handle embedded quotes
        # R"sv(...)sv" avoids escaping issues entirely
        return f'note::string_view(R"sv({value})sv")'
    return f'"{value}"'


def _build_accessor_map(endpoints) -> dict[tuple, str]:
    """Map (notecard_request, struct_name, wire_name) -> accessor_name.

    Used to resolve C++ accessor names for x-validation fields, accounting
    for renames like mode->triggers when mode_prefix is set.
    """
    result = {}
    for ep in endpoints:
        for op in ep.operations:
            for prop in op.properties:
                key = (ep.wire_name, op.struct_name, prop.wire_name)
                result[key] = prop.accessor_name
    return result


def _build_prop_map(endpoints) -> dict[tuple, object]:
    """Map (notecard_request, struct_name_or_None, wire_name) -> PropertyDef.

    Keyed both by struct_name (for polymorphic lookups) and None (as a
    monomorphic fallback when sub_type is None in x-validation).
    """
    result = {}
    for ep in endpoints:
        for op in ep.operations:
            for prop in op.properties:
                result[(ep.wire_name, op.struct_name, prop.wire_name)] = prop
                # None-keyed fallback: first-seen wins (monomorphic endpoints
                # have one op; for polymorphic the fallback is not used).
                fallback = (ep.wire_name, None, prop.wire_name)
                if fallback not in result:
                    result[fallback] = prop
    return result


def _flag_calls(value: str, flags: list[str]) -> list[str] | None:
    """If value is a single recognized flag name, return it as a one-element list.

    Multi-flag values (e.g. 'wifi,cell') are not split into calls because
    FlagSet serializes in flag_defs order, which may differ from the x-validation
    wire value — keeping a raw string assignment is the safe fallback there.

    Returns None if value is not a single recognized flag.
    """
    parts = [p.strip() for p in value.split(",") if p.strip()]
    if len(parts) == 1 and parts[0] in flags:
        return parts
    return None


def _collect_sample_tests(spec_path: Path, endpoints=None) -> list[dict]:
    """Read x-validation from spec samples and build test case objects."""
    with open(spec_path) as f:
        spec = json.load(f)
    accessor_map = _build_accessor_map(endpoints) if endpoints else {}
    prop_map = _build_prop_map(endpoints) if endpoints else {}

    tests = []
    seen = set()  # deduplicate (samples appear on multiple operations)

    for path, path_item in spec.get("paths", {}).items():
        for method, op in path_item.items():
            if method in ("parameters", "summary", "description", "x-aliases", "x-flat-alias"):
                continue

            notecard_request = op.get("x-notecard-request", "")
            struct_name = endpoint_to_struct_name(notecard_request)

            for sample in op.get("x-samples", []):
                validation = sample.get("x-validation", {})
                if "skip" in validation or "wire" not in validation:
                    continue

                # Deduplicate by wire format (same sample on multiple ops)
                wire = validation["wire"]
                dedup_key = (notecard_request, wire)
                if dedup_key in seen:
                    continue
                seen.add(dedup_key)

                title = sample.get("title", "untitled")
                raw_sub_type = validation.get("sub_type")
                sub_type = operation_suffix_to_struct_name(raw_sub_type.lower()) if raw_sub_type else None
                is_command = validation.get("command", False)

                # Build field list
                fields = []
                for field_name, value in (validation.get("fields") or {}).items():
                    cpp_name = property_to_cpp_name(field_name)
                    default_accessor = property_to_accessor_name(cpp_name)
                    # Look up actual accessor name from parsed model (handles
                    # renames like mode->triggers when mode_prefix is set).
                    accessor = accessor_map.get(
                        (notecard_request, sub_type, field_name),
                        default_accessor,
                    )
                    # For flag fields, generate method-call syntax instead of
                    # raw string assignment (e.g. req.connected() not req.triggers="connected").
                    calls = None
                    if isinstance(value, str):
                        prop = prop_map.get(
                            (notecard_request, sub_type, field_name)
                        ) or prop_map.get(
                            (notecard_request, None, field_name)
                        )
                        if prop and prop.has_flags:
                            calls = _flag_calls(value, prop.flags or [])
                    fields.append({
                        "wire_name": field_name,
                        "cpp_name": cpp_name,
                        "accessor_name": accessor,
                        "literal": _cpp_literal(value),
                        "flag_calls": calls,
                    })

                tests.append({
                    "endpoint": notecard_request,
                    "title": title.replace('"', '\\"'),
                    "notecard_request": notecard_request,
                    "struct_name": struct_name,
                    "sub_type": sub_type,
                    "command": is_command,
                    "fields": fields,
                    "wire": wire,
                })

    return tests


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate C++ types from Notecard OpenAPI spec")
    parser.add_argument("spec", help="Path to OpenAPI JSON spec")
    parser.add_argument("-o", "--output-dir",
                        default="include/note/api",
                        help="Output directory for generated headers")
    parser.add_argument("--api",
                        default="include/note/api.hpp",
                        help="Path for the Api class header")
    parser.add_argument("--test-dir",
                        default="tests",
                        help="Directory for generated test files")
    args = parser.parse_args()

    spec_path = Path(args.spec)
    output_dir = Path(args.output_dir)
    api_path = Path(args.api)
    test_dir = Path(args.test_dir)

    # Parse spec
    endpoints = parse_spec(spec_path)
    print(f"Parsed {len(endpoints)} endpoint groups "
          f"({sum(len(e.operations) for e in endpoints)} operations)")

    # Set up Jinja2
    template_dir = Path(__file__).parent / "templates"
    env = jinja2.Environment(
        loader=jinja2.FileSystemLoader(str(template_dir)),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )
    env.filters["doc_comment"] = _doc_comment_filter
    env.filters["first_upper"] = lambda s: (s[0].upper() + s[1:]) if s else s
    env.filters["request_test_value"] = _cpp_request_test_value
    env.filters["reader_test_value"] = _reader_test_value
    env.filters["response_match_value"] = _response_match_value
    env.filters["flag_namespace"] = lambda wire: wire.rsplit(".", 1)[-1]
    env.filters["flag_cpp_name"] = lambda s: s.replace("-", "_")
    env.filters["enum_const_name"] = _enum_const_name

    endpoint_template = env.get_template("endpoint.hpp.j2")

    # Generate per-endpoint headers
    output_dir.mkdir(parents=True, exist_ok=True)
    for endpoint in endpoints:
        has_body_field = any(
            prop.is_body
            for op in endpoint.operations
            for prop in op.properties
        )
        has_body_response = any(
            op.response.has_body
            for op in endpoint.operations
        )
        has_unit_fields = any(
            prop.has_unit
            for op in endpoint.operations
            for prop in op.properties + op.response.properties
        )
        has_format_fields = any(
            prop.has_format
            for op in endpoint.operations
            for prop in op.properties
        )
        has_flags_fields = any(
            prop.has_flags
            for op in endpoint.operations
            for prop in op.properties
        )
        has_array_fields = any(
            prop.is_array
            for op in endpoint.operations
            for prop in op.properties
        )
        has_enum_fields = any(
            prop.enum_values and prop.field_type == "note::string_view"
            for op in endpoint.operations
            for prop in op.properties
        )
        content = endpoint_template.render(
            endpoint=endpoint,
            has_body_field=has_body_field,
            has_body_response=has_body_response,
            has_unit_fields=has_unit_fields,
            has_format_fields=has_format_fields,
            has_flags_fields=has_flags_fields,
            has_array_fields=has_array_fields,
            has_enum_fields=has_enum_fields,
        )
        out_path = output_dir / endpoint.header_filename
        out_path.write_text(content)

    print(f"Generated {len(endpoints)} headers in {output_dir}/")

    # Detect parent-child endpoint relationships.
    # e.g. card.binary -> card.binary.put, card.binary.get
    ep_by_wire = {ep.wire_name: ep for ep in endpoints}
    for ep in endpoints:
        for other in endpoints:
            if other.wire_name.startswith(ep.wire_name + ".") and other != ep:
                ep.children.append(other)
                other.parent = ep
        if ep.children:
            ep.children.sort(key=lambda c: c.wire_name)

    # Build resource groups (card, hub, note, etc.)
    from collections import OrderedDict
    group_map: OrderedDict[str, ResourceGroup] = OrderedDict()
    for ep in endpoints:
        gname = ep.group_name
        if gname not in group_map:
            group_map[gname] = ResourceGroup(
                name=gname,
                struct_name=gname.capitalize() + "Group",
            )
        group_map[gname].endpoints.append(ep)
    resource_groups = list(group_map.values())
    print(f"Grouped into {len(resource_groups)} resource groups: "
          f"{', '.join(g.name for g in resource_groups)}")

    # Generate Api class header
    api_template = env.get_template("api.hpp.j2")
    api_path.parent.mkdir(parents=True, exist_ok=True)
    content = api_template.render(
        endpoints=endpoints,
        resource_groups=resource_groups,
    )
    api_path.write_text(content)
    print(f"Generated Api class header: {api_path}")

    # Generate sample tests
    tests = _collect_sample_tests(spec_path, endpoints)
    if tests:
        test_template = env.get_template("test_samples.cpp.j2")
        test_content = test_template.render(tests=tests)
        test_dir.mkdir(parents=True, exist_ok=True)
        test_path = test_dir / "test_samples.cpp"
        test_path.write_text(test_content)
        print(f"Generated {len(tests)} sample tests in {test_path}")

    # Generate Api factory coverage test
    api_context_test_template = env.get_template("test_api_context.cpp.j2")
    api_context_test_content = api_context_test_template.render(
        endpoints=endpoints,
        resource_groups=resource_groups,
    )
    test_dir.mkdir(parents=True, exist_ok=True)
    api_context_test_path = test_dir / "test_api_context.cpp"
    api_context_test_path.write_text(api_context_test_content)
    print(f"Generated Api factory coverage test in {api_context_test_path}")

    # Generate endpoint request-builder and response-parser coverage tests
    endpoint_cov_template = env.get_template("test_endpoint_coverage.cpp.j2")
    endpoint_cov_content = endpoint_cov_template.render(
        endpoints=endpoints,
    )
    endpoint_cov_path = test_dir / "test_endpoint_coverage.cpp"
    endpoint_cov_path.write_text(endpoint_cov_content)
    print(f"Generated endpoint coverage tests in {endpoint_cov_path}")


if __name__ == "__main__":
    main()
