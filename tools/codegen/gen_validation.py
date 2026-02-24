#!/usr/bin/env python3
"""Analyze x-samples and generate x-validation metadata for each sample.

For each sample in the OpenAPI spec, determines:
  - Which C++ type to use (handling polymorphic dispatch)
  - Which fields to set (excluding req/cmd and dispatch-required fields)
  - The expected wire JSON (fields in schema property order)

Writes the enriched spec back with x-validation added under each sample.
"""

from __future__ import annotations

import copy
import json
import sys
from collections import OrderedDict
from pathlib import Path


def parse_sample_json(sample_json: str) -> dict | list | None:
    """Parse the sample JSON string, handling edge cases."""
    try:
        return json.loads(sample_json)
    except json.JSONDecodeError:
        return None


def determine_dispatch_variant(
    fields: dict,
    operations: list[tuple[str, dict]],
) -> tuple[str, dict] | None:
    """For a polymorphic endpoint, determine which operation matches the fields.

    Returns (method, operation) or None if no match.
    """
    # Try constrained operations first (more specific), then unconstrained
    fallback = None
    for method, op in operations:
        dispatch = op.get("x-dispatch")
        if not dispatch:
            # No constraints — fallback candidate
            if fallback is None:
                fallback = (method, op)
            continue

        requires = set(dispatch.get("requires", []))
        excludes = set(dispatch.get("excludes", []))
        requires_any = set(dispatch.get("requires_any", []))

        if requires and not requires.issubset(fields.keys()):
            continue
        if excludes and excludes.intersection(fields.keys()):
            continue
        if requires_any and not requires_any.intersection(fields.keys()):
            continue

        return method, op

    return fallback


def get_operation_properties(op: dict) -> dict[str, dict]:
    """Extract the property schemas from an operation."""
    if "parameters" in op:
        return {p["name"]: p.get("schema", {}) for p in op["parameters"]}
    if "requestBody" in op:
        schema = (op.get("requestBody", {})
                  .get("content", {})
                  .get("application/json", {})
                  .get("schema", {}))
        return schema.get("properties", {})
    return {}


class ShortestDoubleEncoder(json.JSONEncoder):
    """Encode floats using the shortest representation that round-trips.

    Matches the C++ TestJsonBuilder behavior: tries increasing precision
    with %g formatting until the value round-trips correctly.
    """
    def default(self, o):
        return super().default(o)

    def encode(self, o):
        return self._encode_value(o)

    def _encode_value(self, o):
        if isinstance(o, float):
            return self._format_double(o)
        if isinstance(o, bool):
            return "true" if o else "false"
        if isinstance(o, int):
            return str(o)
        if isinstance(o, str):
            return json.dumps(o)
        if isinstance(o, dict):
            items = ",".join(
                f"{json.dumps(k)}:{self._encode_value(v)}"
                for k, v in o.items()
            )
            return "{" + items + "}"
        if isinstance(o, list):
            items = ",".join(self._encode_value(v) for v in o)
            return "[" + items + "]"
        if o is None:
            return "null"
        return json.dumps(o)

    @staticmethod
    def _format_double(v: float) -> str:
        for prec in range(1, 18):
            s = f"{v:.{prec}g}"
            if float(s) == v:
                return s
        return f"{v:.17g}"


def compute_wire_json(
    fields: dict,
    prop_schemas: dict[str, dict],
    req_or_cmd: str,
    notecard_request: str,
    dispatch: dict | None,
) -> str:
    """Compute the expected wire JSON in schema property order."""
    wire = OrderedDict()

    # First: req or cmd
    wire[req_or_cmd] = notecard_request

    # Dispatch-required fields come next (in order they appear in dispatch.requires)
    requires = dispatch.get("requires", []) if dispatch else []

    # Then: all fields in schema property order
    for prop_name in prop_schemas:
        if prop_name in fields:
            wire[prop_name] = fields[prop_name]
        elif prop_name in requires:
            wire[prop_name] = True  # dispatch-required, always true

    # Also add any fields not in schema (shouldn't happen, but be safe)
    for k, v in fields.items():
        if k not in wire:
            wire[k] = v

    return json.dumps(wire, separators=(",", ":"), cls=ShortestDoubleEncoder)


def type_name_for_value(value) -> str | None:
    """Determine the C++ setter type hint from a JSON value."""
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int):
        return "int32_t"
    if isinstance(value, float):
        return "double"
    if isinstance(value, str):
        return "string_view"
    return None  # arrays, objects — not settable in V1


# Map of x-prefix keys back to original wire names
X_PREFIX_REVERSE = {
    "x-schema-version": "version",
}


def gen_validation(spec_path: str, output_path: str | None = None) -> dict:
    """Analyze all samples and add x-validation metadata."""
    with open(spec_path) as f:
        spec = json.load(f)

    stats = {"total": 0, "generated": 0, "skipped_multi": 0,
             "skipped_array": 0, "skipped_complex": 0, "skipped_cmd": 0}

    for path, path_item in spec.get("paths", {}).items():
        # Collect all operations for this path (for dispatch matching)
        all_ops = []
        for method in ("get", "put", "post", "delete"):
            if method in path_item:
                all_ops.append((method, path_item[method]))

        for method, op in all_ops:
            samples = op.get("x-samples", [])
            is_polymorphic = len(all_ops) > 1

            for sample in samples:
                stats["total"] += 1
                sample_json = sample.get("json", "")
                parsed = parse_sample_json(sample_json)

                # Skip multi-request samples (arrays of requests)
                if isinstance(parsed, list):
                    stats["skipped_multi"] += 1
                    sample["x-validation"] = {"skip": "multi-request sample"}
                    continue

                if parsed is None:
                    stats["skipped_complex"] += 1
                    sample["x-validation"] = {"skip": "unparseable"}
                    continue

                # Extract req/cmd and fields
                req_or_cmd = "req" if "req" in parsed else "cmd"
                notecard_request = parsed.get(req_or_cmd, "")
                fields = {k: v for k, v in parsed.items()
                          if k not in ("req", "cmd")}

                # Check for unsettable fields (arrays, objects)
                unsettable = {k for k, v in fields.items()
                              if isinstance(v, (list, dict))}

                if unsettable:
                    stats["skipped_array"] += 1
                    sample["x-validation"] = {
                        "skip": f"contains unsettable fields: {sorted(unsettable)}",
                    }
                    continue

                # Determine which operation variant to use
                if is_polymorphic:
                    match = determine_dispatch_variant(fields, all_ops)
                    if match is None:
                        stats["skipped_complex"] += 1
                        sample["x-validation"] = {"skip": "no dispatch match"}
                        continue
                    matched_method, matched_op = match
                else:
                    matched_method, matched_op = method, op

                dispatch = matched_op.get("x-dispatch")
                prop_schemas = get_operation_properties(matched_op)

                # Determine the sub-type name for polymorphic endpoints
                op_id = matched_op.get("operationId", "")
                base_id = notecard_request.replace(".", "_")
                if is_polymorphic and op_id.startswith(base_id + "_"):
                    sub_type = op_id[len(base_id) + 1:].capitalize()
                else:
                    sub_type = None

                # Build settable fields
                requires = set(dispatch.get("requires", []) if dispatch else [])
                is_command = req_or_cmd == "cmd"
                settable_fields = OrderedDict()
                for k, v in fields.items():
                    if not is_command and k in requires and isinstance(v, bool):
                        continue  # dispatch-required boolean, auto-emitted by build()
                    settable_fields[k] = v

                # Compute expected wire JSON
                wire = compute_wire_json(
                    fields, prop_schemas, req_or_cmd,
                    notecard_request, dispatch)

                validation = {
                    "wire": wire,
                    "fields": dict(settable_fields) if settable_fields else None,
                }
                if sub_type:
                    validation["sub_type"] = sub_type
                if req_or_cmd == "cmd":
                    validation["command"] = True

                # Clean up None values
                validation = {k: v for k, v in validation.items() if v is not None}

                sample["x-validation"] = validation
                stats["generated"] += 1

    # Write enriched spec
    out = output_path or spec_path
    with open(out, "w") as f:
        json.dump(spec, f, indent=2)
        f.write("\n")

    return stats


if __name__ == "__main__":
    spec_path = sys.argv[1] if len(sys.argv) > 1 else "notecard-api.openapi.json"
    output_path = sys.argv[2] if len(sys.argv) > 2 else spec_path
    stats = gen_validation(spec_path, output_path)
    print(f"Total samples: {stats['total']}")
    print(f"  Generated validation: {stats['generated']}")
    print(f"  Skipped (multi-request): {stats['skipped_multi']}")
    print(f"  Skipped (arrays/objects): {stats['skipped_array']}")
    print(f"  Skipped (complex/cmd): {stats['skipped_complex']}")
    print(f"  Skipped (cmd-only): {stats['skipped_cmd']}")
