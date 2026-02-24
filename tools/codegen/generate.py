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
from codegen.naming import (
    endpoint_to_struct_name,
    property_to_cpp_name,
)


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


def _collect_sample_tests(spec_path: Path) -> list[dict]:
    """Read x-validation from spec samples and build test case objects."""
    with open(spec_path) as f:
        spec = json.load(f)

    tests = []
    seen = set()  # deduplicate (samples appear on multiple operations)

    for path, path_item in spec.get("paths", {}).items():
        for method, op in path_item.items():
            if method in ("parameters", "summary", "description"):
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
                sub_type = validation.get("sub_type")
                is_command = validation.get("command", False)

                # Build field list
                fields = []
                for field_name, value in (validation.get("fields") or {}).items():
                    fields.append({
                        "wire_name": field_name,
                        "cpp_name": property_to_cpp_name(field_name),
                        "literal": _cpp_literal(value),
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
    parser.add_argument("--umbrella",
                        default="include/note/api.hpp",
                        help="Path for the umbrella header")
    parser.add_argument("--api-context",
                        default="include/note/api_context.hpp",
                        help="Path for the Api factory header")
    parser.add_argument("--test-dir",
                        default="tests",
                        help="Directory for generated test files")
    args = parser.parse_args()

    spec_path = Path(args.spec)
    output_dir = Path(args.output_dir)
    umbrella_path = Path(args.umbrella)
    api_context_path = Path(args.api_context)
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

    endpoint_template = env.get_template("endpoint.hpp.j2")
    umbrella_template = env.get_template("api.hpp.j2")

    # Generate per-endpoint headers
    output_dir.mkdir(parents=True, exist_ok=True)
    for endpoint in endpoints:
        has_body_field = any(
            prop.is_body
            for op in endpoint.operations
            for prop in op.properties
        )
        content = endpoint_template.render(
            endpoint=endpoint,
            has_body_field=has_body_field,
        )
        out_path = output_dir / endpoint.header_filename
        out_path.write_text(content)

    print(f"Generated {len(endpoints)} headers in {output_dir}/")

    # Generate umbrella header
    umbrella_path.parent.mkdir(parents=True, exist_ok=True)
    content = umbrella_template.render(endpoints=endpoints)
    umbrella_path.write_text(content)
    print(f"Generated umbrella header: {umbrella_path}")

    # Generate Api factory header
    api_context_template = env.get_template("api_context.hpp.j2")
    api_context_path.parent.mkdir(parents=True, exist_ok=True)
    content = api_context_template.render(endpoints=endpoints)
    api_context_path.write_text(content)
    print(f"Generated Api factory: {api_context_path}")

    # Generate sample tests
    tests = _collect_sample_tests(spec_path)
    if tests:
        test_template = env.get_template("test_samples.cpp.j2")
        test_content = test_template.render(tests=tests)
        test_dir.mkdir(parents=True, exist_ok=True)
        test_path = test_dir / "test_samples.cpp"
        test_path.write_text(test_content)
        print(f"Generated {len(tests)} sample tests in {test_path}")


if __name__ == "__main__":
    main()
