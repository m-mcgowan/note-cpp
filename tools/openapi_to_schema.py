#!/usr/bin/env python3
"""Convert an OpenAPI 3.1 Notecard spec back to individual JSON Schema files.

This is the reverse of schema_to_openapi.py, used to verify round-trip fidelity.

Usage:
    python3 openapi_to_schema.py notecard-api.openapi.json -o output_dir/
"""

import argparse
import copy
import json
import sys
from pathlib import Path

# Same mapping as the forward transform
CUSTOM_KEYS = {
    "sub-descriptions": "x-sub-descriptions",
    "skus": "x-skus",
    "samples": "x-samples",
    "minApiVersion": "x-min-api-version",
    "annotations": "x-annotations",
    "version": "x-schema-version",
    "apiVersion": "x-api-version",
}

REVERSE_CUSTOM_KEYS = {v: k for k, v in CUSTOM_KEYS.items()}

SCHEMA_BASE_URL = "https://raw.githubusercontent.com/blues/notecard-schema/master"


def unprefix_custom_keys(schema):
    """Recursively reverse x- prefix on custom keys."""
    if isinstance(schema, list):
        return [unprefix_custom_keys(item) for item in schema]
    if not isinstance(schema, dict):
        return schema

    result = {}
    for key, value in schema.items():
        new_key = REVERSE_CUSTOM_KEYS.get(key, key)
        result[new_key] = unprefix_custom_keys(value)
    return result


def path_to_endpoint(path: str) -> str:
    """Convert '/hub/set' to 'hub.set'."""
    return path.lstrip("/").replace("/", ".")


def extract_properties_from_operation(operation: dict, method: str) -> dict:
    """Extract the request properties from an OpenAPI operation."""
    method_upper = method.upper()

    if method_upper == "GET":
        # Properties stored as query parameters
        props = {}
        for param in operation.get("parameters", []):
            if param.get("in") == "query":
                schema = copy.deepcopy(param["schema"])
                if "description" in param:
                    schema["description"] = param["description"]
                props[param["name"]] = schema
        return props
    else:
        # Properties in requestBody
        rb = operation.get("requestBody", {})
        content = rb.get("content", {})
        json_content = content.get("application/json", {})
        body_schema = json_content.get("schema", {})
        return copy.deepcopy(body_schema.get("properties", {}))


def extract_allof_from_operation(operation: dict, method: str) -> list | None:
    """Extract allOf (conditional schemas) from the request body."""
    if method.upper() == "GET":
        return None
    rb = operation.get("requestBody", {})
    content = rb.get("content", {})
    json_content = content.get("application/json", {})
    body_schema = json_content.get("schema", {})
    allof = body_schema.get("allOf")
    return copy.deepcopy(allof) if allof else None


def extract_response_schema(operation: dict) -> dict | None:
    """Extract response properties from an operation's 200 response."""
    resp = operation.get("responses", {}).get("200", {})
    content = resp.get("content", {})
    json_content = content.get("application/json", {})
    schema = json_content.get("schema")
    return copy.deepcopy(schema) if schema else None


def rebuild_request_schema(endpoint: str, operations: dict) -> dict:
    """Rebuild the original request JSON Schema from OpenAPI operation(s).

    For polymorphic endpoints, merges properties from all method variants.
    """
    # Pick any operation for shared metadata (they all came from the same schema)
    first_op = next(iter(operations.values()))

    # Start with envelope
    schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": f"{SCHEMA_BASE_URL}/{endpoint}.req.notecard.api.json",
    }

    # Restore top-level metadata
    if "x-title" in first_op:
        schema["title"] = first_op["x-title"]
    schema["description"] = first_op.get("summary", "")
    schema["type"] = "object"

    if "x-schema-version" in first_op:
        schema["version"] = first_op["x-schema-version"]
    if "x-api-version" in first_op:
        schema["apiVersion"] = first_op["x-api-version"]
    if "x-skus" in first_op:
        schema["skus"] = first_op["x-skus"]
    if first_op.get("x-min-api-version"):
        schema["minApiVersion"] = first_op["x-min-api-version"]

    # Merge properties from all operations (polymorphic endpoints)
    all_props = {}
    all_allof = None
    for method, op in operations.items():
        props = extract_properties_from_operation(op, method)
        props = unprefix_custom_keys(props)
        all_props.update(props)

        allof = extract_allof_from_operation(op, method)
        if allof:
            all_allof = unprefix_custom_keys(allof)

    # Add req/cmd properties
    supports_cmd = first_op.get("x-supports-cmd", False)
    all_props["req"] = {
        "description": "Request for the Notecard (expects response)",
        "const": endpoint,
    }

    if supports_cmd:
        all_props["cmd"] = {
            "description": "Command for the Notecard (no response)",
            "const": endpoint,
        }

    schema["properties"] = all_props

    # Add req/cmd oneOf
    schema["oneOf"] = [
        {"required": ["req"], "properties": {"req": {"const": endpoint}}},
    ]
    if supports_cmd:
        schema["oneOf"].append(
            {"required": ["cmd"], "properties": {"cmd": {"const": endpoint}}}
        )

    schema["additionalProperties"] = False

    # Restore allOf (conditional schemas)
    if all_allof:
        schema["allOf"] = all_allof

    # Restore samples
    if "x-samples" in first_op:
        schema["samples"] = first_op["x-samples"]

    return schema


def rebuild_response_schema(endpoint: str, operations: dict) -> dict | None:
    """Rebuild the original response JSON Schema from OpenAPI operation(s)."""
    # Use first operation's response (all ops share the same response schema)
    first_op = next(iter(operations.values()))
    resp_schema = extract_response_schema(first_op)
    resp_200 = first_op.get("responses", {}).get("200", {})

    schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": f"{SCHEMA_BASE_URL}/{endpoint}.rsp.notecard.api.json",
    }

    # Response metadata from the 200 response object
    if "x-title" in resp_200:
        schema["title"] = resp_200["x-title"]
    desc = resp_200.get("description")
    if desc and desc != "Successful response":
        schema["description"] = desc
    schema["type"] = "object"

    if "x-schema-version" in resp_200:
        schema["version"] = resp_200["x-schema-version"]
    if "x-api-version" in resp_200:
        schema["apiVersion"] = resp_200["x-api-version"]
    if "x-skus" in resp_200:
        schema["skus"] = resp_200["x-skus"]
    if resp_200.get("x-min-api-version"):
        schema["minApiVersion"] = resp_200["x-min-api-version"]

    if resp_schema:
        resp_schema = unprefix_custom_keys(resp_schema)
        schema["properties"] = resp_schema.get("properties", {})
        if "required" in resp_schema:
            schema["required"] = resp_schema["required"]
    else:
        schema["properties"] = {}

    schema["additionalProperties"] = False

    if "x-samples" in resp_200:
        schema["samples"] = resp_200["x-samples"]

    return schema


def convert(openapi_path: Path) -> tuple[dict, dict]:
    """Convert OpenAPI spec back to individual JSON Schemas.

    Returns (requests, responses) dicts keyed by endpoint name.
    """
    with open(openapi_path) as f:
        spec = json.load(f)

    requests = {}
    responses = {}

    for path, path_item in spec.get("paths", {}).items():
        endpoint = path_to_endpoint(path)

        # Group operations by method
        operations = {}
        for method in ("get", "put", "post", "delete", "patch"):
            if method in path_item:
                operations[method] = path_item[method]

        if not operations:
            continue

        requests[endpoint] = rebuild_request_schema(endpoint, operations)
        rsp = rebuild_response_schema(endpoint, operations)
        if rsp:
            responses[endpoint] = rsp

    return requests, responses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("openapi", type=Path,
                        help="Path to OpenAPI 3.1 JSON file")
    parser.add_argument("-o", "--output-dir", type=Path, required=True,
                        help="Output directory for JSON Schema files")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)

    requests, responses = convert(args.openapi)

    for endpoint, schema in requests.items():
        out_path = args.output_dir / f"{endpoint}.req.notecard.api.json"
        out_path.write_text(json.dumps(schema, indent=4) + "\n")

    for endpoint, schema in responses.items():
        out_path = args.output_dir / f"{endpoint}.rsp.notecard.api.json"
        out_path.write_text(json.dumps(schema, indent=4) + "\n")

    print(f"Wrote {len(requests)} request + {len(responses)} response schemas to {args.output_dir}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
