#!/usr/bin/env python3
"""Generate C++ request/response types from an OpenAPI spec."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import jinja2

# Allow running as `python3 tools/codegen/generate.py` from repo root
if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from codegen.model import EndpointGroup
from codegen.spec_parser import parse_spec


def _load_spec_with_overlay(spec_path: Path, overlay_path: Path | None) -> dict:
    """Load the OpenAPI spec and merge an overlay if it exists.

    The overlay adds or overrides property-level fields (e.g. enum values)
    without modifying the upstream spec file.
    """
    with open(spec_path) as f:
        spec = json.load(f)

    if overlay_path and overlay_path.exists():
        with open(overlay_path) as f:
            overlay = json.load(f)

        for path_name, methods in overlay.get("paths", {}).items():
            spec_path_obj = spec.get("paths", {}).get(path_name)
            if not spec_path_obj:
                continue
            for method, patches in methods.items():
                spec_method = spec_path_obj.get(method)
                if not spec_method:
                    continue
                # Merge request property-level patches
            props_patches = patches.get("properties", {})
            spec_props = (spec_method
                          .get("requestBody", {})
                          .get("content", {})
                          .get("application/json", {})
                          .get("schema", {})
                          .get("properties", {}))
            for prop_name, prop_patch in props_patches.items():
                if prop_name in spec_props:
                    spec_props[prop_name].update(prop_patch)

            # Merge response property-level patches (add missing fields)
            rsp_patches = patches.get("response-properties", {})
            for code, resp in spec_method.get("responses", {}).items():
                rsp_props = (resp
                             .get("content", {})
                             .get("application/json", {})
                             .get("schema", {})
                             .get("properties", {}))
                for prop_name, prop_def in rsp_patches.items():
                    if prop_name not in rsp_props:
                        rsp_props[prop_name] = prop_def
                    else:
                        rsp_props[prop_name].update(prop_def)

            # Replace or patch operation-level extensions
            for key, value in patches.items():
                if key == "properties":
                    continue  # already handled above
                if key == "x-intent-patches":
                    # Patch individual intents by name (merge, don't replace)
                    existing_intents = spec_method.get("x-intents", [])
                    for intent in existing_intents:
                        patch = value.get(intent.get("name", ""))
                        if patch:
                            intent.update(patch)
                else:
                    spec_method[key] = value

        print(f"Applied overlay: {overlay_path}")

    return spec
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
    # Strip markdown italic (`_text_`) only when not part of a snake_case identifier.
    # Without the lookarounds, `set_response_buffer` collapses to `setresponsebuffer`.
    text = re.sub(r'(?<!\w)_([^_\s][^_]*)_(?!\w)', r'\1', text)
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
    name = value.replace(",", "_").replace("-", "_").replace("+", "p").replace("?", "unknown")
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
    if prop.is_integral:
        return f"{prop.cpp_type}(42)"
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
    if prop.is_integral:
        return f"{prop.cpp_type}(42)"
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
    if prop.is_integral:
        return "42"
    if prop.cpp_type == "double":
        return "1.5"
    # note::string_view — compare against a const char* literal
    return f'"x-{prop.wire_name}"'


def _json_test_value(prop) -> str:
    """JSON-encoded test value for streaming response tests.

    Returns a raw JSON token: true, 42, 1.5, "\"x-fieldname\"", or ["a","b"].
    Used in generated R"(...)" response strings.
    """
    if prop.is_array:
        return f'["x-{prop.wire_name}-a","x-{prop.wire_name}-b"]'
    if prop.cpp_type == "bool":
        return "true"
    if prop.is_integral:
        return "42"
    if prop.cpp_type == "double":
        return "1.5"
    return f'"x-{prop.wire_name}"'


def _wire_value_fragment(prop) -> str:
    """Expected JSON fragment for a field set with its test value.

    Returns a string like '"mode":"periodic"' or '"seconds":42' that
    can be used in a find() assertion on the wire output.
    """
    wire = prop.wire_name
    if prop.has_unit:
        return f'"\\"{wire}\\":42"'
    if prop.is_array:
        return f'"\\"{wire}\\":[\\\"x-{wire}-item\\\"]"'
    if prop.has_flags:
        return f'"\\"{wire}\\":\\"{prop.flags[0]}\\""'
    if prop.cpp_type == "bool":
        return f'"\\"{wire}\\":true"'
    if prop.is_integral:
        return f'"\\"{wire}\\":42"'
    if prop.cpp_type == "double":
        return f'"\\"{wire}\\":1.5"'
    if prop.enum_values:
        return f'"\\"{wire}\\":\\"{prop.enum_values[0]}\\""'
    return f'"\\"{wire}\\":\\"x-{wire}\\""'


def _cpp_literal(value) -> str:
    """Convert a JSON value to a C++ literal string."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return f"note::json_int_t{{{value}}}"
    if isinstance(value, float):
        return f"{value}"
    if isinstance(value, str):
        # Use raw string literal with note:: prefix to handle embedded quotes
        # R"sv(...)sv" avoids escaping issues entirely
        return f'note::string_view(R"sv({value})sv")'
    return f'"{value}"'


def _collect_fw_versions(spec_dict: dict) -> list[dict]:
    """Collect every unique `x-min-api-version` value in the spec.

    Walks the entire raw spec dict so we pick up every gated threshold —
    operation-level, request-/response-property-level, and value-level
    (e.g. individual enum values gated via `x-sub-descriptions` entries).
    Enum-value-level gating isn't yet enforced by codegen, but emitting
    the constant lets users declare the threshold at construction today
    and lets a future codegen pass close the gap without renaming.

    Parses "M.m.p" into a tuple; unparseable values are skipped. Returns
    a sorted deduplicated list of dicts ready for the template:

        [{"name": "v7_5_1", "major": 7, "minor": 5, "patch": 1}, ...]

    Naming: `v{M}_{m}` when patch is zero, else `v{M}_{m}_{p}`.
    """
    versions: set[tuple[int, int, int]] = set()

    def _add(raw) -> None:
        if not isinstance(raw, str):
            return
        parts = raw.split(".")
        if len(parts) < 2:
            return
        try:
            major = int(parts[0])
            minor = int(parts[1])
            patch = int(parts[2]) if len(parts) > 2 else 0
        except ValueError:
            return
        versions.add((major, minor, patch))

    def _walk(node) -> None:
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "x-min-api-version":
                    _add(v)
                else:
                    _walk(v)
        elif isinstance(node, list):
            for item in node:
                _walk(item)

    _walk(spec_dict)

    result = []
    for major, minor, patch in sorted(versions):
        name = f"v{major}_{minor}" if patch == 0 else f"v{major}_{minor}_{patch}"
        result.append({
            "name": name,
            "major": major,
            "minor": minor,
            "patch": patch,
        })
    return result


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


def _collect_sample_tests(spec_path: Path, endpoints=None, spec_dict: dict | None = None) -> list[dict]:
    """Read x-validation from spec samples and build test case objects."""
    if spec_dict is not None:
        spec = spec_dict
    else:
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

                # Extract key-value fragments for order-independent assertions.
                # Parse the wire JSON and produce "key":value fragments.
                import json as _json
                wire_kvs = []
                try:
                    wire_obj = _json.loads(wire)
                    for k, v in wire_obj.items():
                        wire_kvs.append(f'"{k}":{_json.dumps(v, separators=(",", ":"))}')
                except _json.JSONDecodeError:
                    wire_kvs = [wire]  # fallback to full string

                tests.append({
                    "endpoint": notecard_request,
                    "title": title.replace('"', '\\"'),
                    "notecard_request": notecard_request,
                    "struct_name": struct_name,
                    "sub_type": sub_type,
                    "command": is_command,
                    "fields": fields,
                    "wire": wire,
                    "wire_kvs": wire_kvs,
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
    parser.add_argument("--cmake-out",
                        default="cmake/note-cpp-generated.cmake",
                        help="Path for generated CMake file listing")
    parser.add_argument("--overlay",
                        default=None,
                        help="Path to overlay JSON (merged into spec before generation)")
    args = parser.parse_args()

    spec_path = Path(args.spec)
    output_dir = Path(args.output_dir)
    api_path = Path(args.api)
    test_dir = Path(args.test_dir)
    cmake_out = Path(args.cmake_out)

    # Auto-discover overlay if not specified
    overlay_path = Path(args.overlay) if args.overlay else spec_path.with_suffix(".overlay.json")

    # Load spec with overlay merged in memory
    spec_dict = _load_spec_with_overlay(spec_path, overlay_path)

    # Parse spec
    endpoints = parse_spec(spec_path, spec_dict=spec_dict)
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
    env.filters["wire_value_fragment"] = _wire_value_fragment
    env.filters["json_test_value"] = _json_test_value
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
        has_response_array_fields = any(
            prop.is_array
            for op in endpoint.operations
            for prop in op.response.properties
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
            has_response_array_fields=has_response_array_fields,
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

    # Synthesize virtual parents for orphan wire-name prefixes.
    # An "orphan prefix" is a wire-name prefix shared by 2+ endpoints that
    # is not itself a real endpoint, so without synthesis those endpoints
    # would emit as flat camelCase methods on the resource group (e.g.
    # `card.usageGet()` and `card.usageTest()`). With a virtual parent the
    # children become reachable as `api.card.usage.read()` / `.test()`.
    from collections import defaultdict
    prefix_orphans: dict[str, list[EndpointGroup]] = defaultdict(list)
    for ep in endpoints:
        if ep.parent is not None:
            continue
        prefix = ep.wire_name.rsplit(".", 1)[0]
        # Need a 2+ segment prefix: 1-segment prefixes are resource groups
        # (e.g. `card`), not endpoint parents — children there belong on the
        # group struct (`api.card.usageGet()` style), not under a virtual.
        if "." not in prefix:
            continue
        if prefix in ep_by_wire:
            continue  # parent exists as a real endpoint — covered above
        prefix_orphans[prefix].append(ep)

    virtual_eps: list[EndpointGroup] = []
    for prefix, orphans in prefix_orphans.items():
        if len(orphans) < 2:
            continue
        struct_name = "".join(seg[:1].upper() + seg[1:] for seg in prefix.split("."))
        virtual = EndpointGroup(
            wire_name=prefix,
            struct_name=struct_name,
            header_filename="",  # no header — virtual parent has no own request
            is_polymorphic=False,
            is_virtual=True,
            children=sorted(orphans, key=lambda c: c.wire_name),
        )
        for child in orphans:
            child.parent = virtual
        virtual_eps.append(virtual)

    if virtual_eps:
        endpoints.extend(virtual_eps)
        ep_by_wire.update({v.wire_name: v for v in virtual_eps})
        print(f"Synthesized {len(virtual_eps)} virtual parent(s): "
              f"{', '.join(v.wire_name for v in virtual_eps)}")

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
    tests = _collect_sample_tests(spec_path, endpoints, spec_dict=spec_dict)
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

    # Generate endpoint streaming-execute coverage tests
    streaming_template = env.get_template("test_endpoint_streaming.cpp.j2")
    streaming_content = streaming_template.render(
        endpoints=endpoints,
    )
    streaming_path = test_dir / "test_endpoint_streaming.cpp"
    streaming_path.write_text(streaming_content)
    print(f"Generated streaming endpoint tests in {streaming_path}")

    # Generate API reference documentation (Markdown)
    api_ref_template = env.get_template("api_reference.md.j2")
    api_ref_content = api_ref_template.render(
        endpoints=endpoints,
        resource_groups=resource_groups,
    )
    docs_dir = test_dir.parent / "docs"
    docs_dir.mkdir(parents=True, exist_ok=True)
    api_ref_path = docs_dir / "api-reference.md"
    api_ref_path.write_text(api_ref_content)
    print(f"Generated API reference in {api_ref_path}")

    # Generate compile-check for API group accessors and direct types
    compile_check_dir = test_dir / "compile_check"
    compile_check_dir.mkdir(parents=True, exist_ok=True)
    compile_check_template = env.get_template("compile_check_api_groups.cpp.j2")
    compile_check_content = compile_check_template.render(
        endpoints=endpoints,
        resource_groups=resource_groups,
    )
    compile_check_path = compile_check_dir / "api_groups.cpp"
    compile_check_path.write_text(compile_check_content)
    print(f"Generated API compile-check in {compile_check_path}")

    # Generate sizeof report test
    sizeof_template = env.get_template("test_sizeof_report.cpp.j2")
    sizeof_content = sizeof_template.render(endpoints=endpoints)
    sizeof_path = test_dir / "test_sizeof_report.cpp"
    sizeof_path.write_text(sizeof_content)
    print(f"Generated sizeof report in {sizeof_path}")

    # Generate sku_info.hpp from the SKU metadata table.
    sku_metadata_path = Path(__file__).parent / "metadata" / "skus.json"
    with open(sku_metadata_path) as f:
        sku_metadata = json.load(f)
    sku_entries = sku_metadata.get("skus", [])
    sku_template = env.get_template("sku_info.hpp.j2")
    sku_content = sku_template.render(skus=sku_entries)
    sku_out_path = output_dir.parent / "sku_info.hpp"  # include/note/sku_info.hpp
    sku_out_path.write_text(sku_content)
    print(f"Generated {len(sku_entries)} SKU entries in {sku_out_path}")

    # Generate fw_versions.hpp from unique min-firmware values in the spec.
    fw_versions = _collect_fw_versions(spec_dict)
    fw_template = env.get_template("fw_versions.hpp.j2")
    fw_content = fw_template.render(versions=fw_versions)
    fw_out_path = output_dir.parent / "fw_versions.hpp"  # include/note/fw_versions.hpp
    fw_out_path.write_text(fw_content)
    print(f"Generated {len(fw_versions)} firmware version constants in {fw_out_path}")

    # Generate CMake file listing all generated files.
    # Paths are emitted relative to the project root (parent of cmake/)
    # so the checked-in file is machine-independent regardless of whether
    # the generator was invoked with absolute or relative output paths.
    # Uses os.path.relpath (lexical) rather than Path.resolve() to avoid
    # following include/→src/ style symlinks that would flip the prefix.
    cmake_out.parent.mkdir(parents=True, exist_ok=True)
    project_root = str(cmake_out.absolute().parent.parent)

    def _rel_to_root(p):
        return os.path.relpath(str(Path(p).absolute()), project_root)

    generated_headers = sorted(
        [_rel_to_root(output_dir / ep.header_filename) for ep in endpoints if not ep.is_virtual]
        + [_rel_to_root(api_path), _rel_to_root(sku_out_path), _rel_to_root(fw_out_path)]
    )
    generated_tests = sorted([
        "test_samples.cpp",
        "test_api_context.cpp",
        "test_endpoint_coverage.cpp",
        "test_endpoint_streaming.cpp",
        "test_sizeof_report.cpp",
    ])
    lines = [
        "# AUTO-GENERATED by tools/codegen/generate.py -- DO NOT EDIT",
        "#",
        "# Lists all generated headers and test sources.",
        "# Include this alongside cmake/note-cpp-sources.cmake.",
        "",
        "set(NOTE_CPP_GENERATED_HEADERS",
    ]
    for h in generated_headers:
        lines.append(f"    {h}")
    lines.append(")")
    lines.append("")
    lines.append("set(NOTE_CPP_GENERATED_TEST_SOURCES")
    for t in generated_tests:
        lines.append(f"    {t}")
    lines.append(")")
    lines.append("")
    cmake_out.write_text("\n".join(lines))
    print(f"Generated CMake file listing: {cmake_out}")


if __name__ == "__main__":
    main()
