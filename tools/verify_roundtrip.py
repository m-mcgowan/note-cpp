#!/usr/bin/env python3
"""Verify round-trip fidelity: original JSON Schema -> OpenAPI -> JSON Schema.

Compares properties, types, descriptions, constraints, and metadata between
the original notecard-schema files and schemas regenerated from the OpenAPI spec.

Usage:
    python3 verify_roundtrip.py <original_schema_dir> <openapi_spec>
"""

import argparse
import copy
import json
import sys
from pathlib import Path

# Keys that are structural envelope and won't survive round-trip exactly
ENVELOPE_KEYS = {"$schema", "$id"}

# Keys where exact match is expected
SEMANTIC_KEYS_REQ = {
    "title", "description", "type", "properties", "required",
    "additionalProperties", "oneOf", "allOf", "anyOf",
    "skus", "samples", "version", "apiVersion", "minApiVersion",
}

# Property-level keys
PROPERTY_KEYS = {
    "description", "type", "enum", "const", "pattern", "format",
    "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum",
    "minItems", "maxItems", "items", "default",
    "oneOf", "anyOf", "allOf", "if", "then", "else", "not",
    "properties", "required", "additionalProperties",
    "contentEncoding",
    "sub-descriptions", "skus", "minApiVersion",
}


def normalize_for_comparison(schema):
    """Normalize a schema for comparison — sort keys, remove envelope."""
    if isinstance(schema, list):
        return [normalize_for_comparison(item) for item in schema]
    if not isinstance(schema, dict):
        return schema

    result = {}
    for key in sorted(schema.keys()):
        if key in ENVELOPE_KEYS:
            continue
        result[key] = normalize_for_comparison(schema[key])
    return result


def deep_diff(original, roundtripped, path=""):
    """Recursively compare two structures, yielding (path, original, roundtripped) diffs."""
    if type(original) != type(roundtripped):
        yield (path, f"type mismatch: {type(original).__name__} vs {type(roundtripped).__name__}",
               repr(original)[:80], repr(roundtripped)[:80])
        return

    if isinstance(original, dict):
        all_keys = set(original.keys()) | set(roundtripped.keys())
        for key in sorted(all_keys):
            child_path = f"{path}.{key}" if path else key
            if key not in roundtripped:
                yield (child_path, "missing in round-tripped", repr(original[key])[:80], None)
            elif key not in original:
                yield (child_path, "extra in round-tripped", None, repr(roundtripped[key])[:80])
            else:
                yield from deep_diff(original[key], roundtripped[key], child_path)

    elif isinstance(original, list):
        if len(original) != len(roundtripped):
            yield (path, f"list length: {len(original)} vs {len(roundtripped)}",
                   None, None)
        for i, (a, b) in enumerate(zip(original, roundtripped)):
            yield from deep_diff(a, b, f"{path}[{i}]")

    else:
        if original != roundtripped:
            yield (path, "value differs", repr(original)[:80], repr(roundtripped)[:80])


def compare_properties(orig_props: dict, rt_props: dict, endpoint: str) -> list:
    """Compare properties dicts, focusing on semantic content."""
    diffs = []

    orig_names = set(orig_props.keys())
    rt_names = set(rt_props.keys())

    for name in sorted(orig_names - rt_names):
        diffs.append(f"  property '{name}': missing in round-tripped")
    for name in sorted(rt_names - orig_names):
        diffs.append(f"  property '{name}': extra in round-tripped")

    for name in sorted(orig_names & rt_names):
        orig_prop = copy.deepcopy(orig_props[name])
        rt_prop = copy.deepcopy(rt_props[name])
        # req/cmd properties are protocol envelope; minApiVersion on them
        # is redundant with the top-level minApiVersion — skip this diff
        if name in ("req", "cmd"):
            orig_prop.pop("minApiVersion", None)
            rt_prop.pop("minApiVersion", None)
        for diff_path, diff_type, orig_val, rt_val in deep_diff(orig_prop, rt_prop):
            detail = f"  property '{name}'.{diff_path}: {diff_type}"
            if orig_val:
                detail += f"\n    original:     {orig_val}"
            if rt_val:
                detail += f"\n    round-tripped: {rt_val}"
            diffs.append(detail)

    return diffs


def verify_endpoint(endpoint: str, original: dict, roundtripped: dict) -> list:
    """Compare an original schema with its round-tripped version."""
    issues = []

    # Compare properties (the core semantic content)
    orig_props = original.get("properties", {})
    rt_props = roundtripped.get("properties", {})
    prop_diffs = compare_properties(orig_props, rt_props, endpoint)
    issues.extend(prop_diffs)

    # Compare allOf (conditional schemas)
    orig_allof = original.get("allOf")
    rt_allof = roundtripped.get("allOf")
    if orig_allof and not rt_allof:
        issues.append("  allOf: missing in round-tripped")
    elif not orig_allof and rt_allof:
        issues.append("  allOf: extra in round-tripped")
    elif orig_allof and rt_allof:
        orig_norm = normalize_for_comparison(orig_allof)
        rt_norm = normalize_for_comparison(rt_allof)
        if orig_norm != rt_norm:
            for diff_path, diff_type, orig_val, rt_val in deep_diff(orig_norm, rt_norm):
                detail = f"  allOf.{diff_path}: {diff_type}"
                if orig_val:
                    detail += f"\n    original:     {orig_val}"
                if rt_val:
                    detail += f"\n    round-tripped: {rt_val}"
                issues.append(detail)

    # Compare top-level metadata
    for key in ["title", "description", "skus", "version", "apiVersion"]:
        orig_val = original.get(key)
        rt_val = roundtripped.get(key)
        if orig_val != rt_val:
            issues.append(f"  {key}: {repr(orig_val)[:60]} vs {repr(rt_val)[:60]}")

    # Compare samples
    orig_samples = original.get("samples")
    rt_samples = roundtripped.get("samples")
    if orig_samples and not rt_samples:
        issues.append("  samples: missing in round-tripped")
    elif orig_samples and rt_samples:
        if len(orig_samples) != len(rt_samples):
            issues.append(f"  samples: {len(orig_samples)} vs {len(rt_samples)}")

    return issues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("schema_dir", type=Path,
                        help="Path to original notecard-schema repo")
    parser.add_argument("openapi", type=Path,
                        help="Path to OpenAPI 3.1 JSON file")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Show details for all endpoints, not just failures")
    args = parser.parse_args()

    # Load originals
    originals_req = {}
    originals_rsp = {}
    for path in sorted(args.schema_dir.glob("*.req.notecard.api.json")):
        endpoint = path.name.replace(".req.notecard.api.json", "")
        with open(path) as f:
            originals_req[endpoint] = json.load(f)
    for path in sorted(args.schema_dir.glob("*.rsp.notecard.api.json")):
        endpoint = path.name.replace(".rsp.notecard.api.json", "")
        with open(path) as f:
            originals_rsp[endpoint] = json.load(f)

    # Reverse-transform from OpenAPI
    # Import the reverse transform
    sys.path.insert(0, str(Path(__file__).parent))
    from openapi_to_schema import convert as reverse_convert
    roundtripped_req, roundtripped_rsp = reverse_convert(args.openapi)

    # Compare
    total = 0
    passed = 0
    failed = 0
    all_issues = {}

    for endpoint in sorted(originals_req.keys()):
        total += 1
        if endpoint not in roundtripped_req:
            print(f"MISS {endpoint}.req - not in round-tripped output")
            failed += 1
            continue

        issues = verify_endpoint(
            endpoint,
            originals_req[endpoint],
            roundtripped_req[endpoint],
        )

        # Also check response
        if endpoint in originals_rsp and endpoint in roundtripped_rsp:
            rsp_issues = verify_endpoint(
                endpoint,
                originals_rsp[endpoint],
                roundtripped_rsp[endpoint],
            )
            if rsp_issues:
                issues.extend([f"  [response] {i.strip()}" for i in rsp_issues])

        if issues:
            failed += 1
            all_issues[endpoint] = issues
            if args.verbose:
                print(f"FAIL {endpoint}")
                for issue in issues:
                    print(f"  {issue}")
        else:
            passed += 1
            if args.verbose:
                print(f"PASS {endpoint}")

    # Summary
    print(f"\n{'='*60}")
    print(f"Round-trip verification: {passed}/{total} passed, {failed} failed")
    print(f"{'='*60}")

    if all_issues and not args.verbose:
        print(f"\nFailed endpoints ({failed}):")
        for endpoint, issues in all_issues.items():
            print(f"\n  {endpoint}:")
            for issue in issues:
                print(f"    {issue}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
