#!/usr/bin/env python3
"""Convert Notecard JSON Schema files to an OpenAPI 3.1 spec.

Reads all *.req.notecard.api.json and *.rsp.notecard.api.json files from the
notecard-schema repo, combines them with safety_semantics.json, and emits a
single notecard-api.openapi.json.

Usage:
    python3 schema_to_openapi.py <schema_dir> [--safety safety_semantics.json] [--binary binary_transfer.json] [--extensions property_extensions.json] [-o output.json]
"""

import argparse
import copy
import json
import subprocess
import sys
from pathlib import Path

# Top-level schema keys that are NOT standard JSON Schema and need x- prefix
CUSTOM_KEYS = {
    "sub-descriptions": "x-sub-descriptions",
    "skus": "x-skus",
    "samples": "x-samples",
    "minApiVersion": "x-min-api-version",
    "annotations": "x-annotations",
    "version": "x-schema-version",
    "apiVersion": "x-api-version",
    "unit": "x-unit",
    "constants": "x-constants",
}

REVERSE_CUSTOM_KEYS = {v: k for k, v in CUSTOM_KEYS.items()}

SAFETY_TO_X = {
    "GET": "readonly",
    "PUT": "idempotent",
    "POST": "non-idempotent",
    "DELETE": "destructive",
}


def endpoint_to_path(endpoint: str) -> str:
    """Convert 'hub.set' to '/hub/set'."""
    return "/" + endpoint.replace(".", "/")


def path_to_endpoint(path: str) -> str:
    """Convert '/hub/set' to 'hub.set'."""
    return path.lstrip("/").replace("/", ".")


def prefix_custom_keys(schema):
    """Recursively rename custom keys with x- prefix in a schema."""
    if isinstance(schema, list):
        return [prefix_custom_keys(item) for item in schema]
    if not isinstance(schema, dict):
        return schema

    result = {}
    for key, value in schema.items():
        new_key = CUSTOM_KEYS.get(key, key)
        result[new_key] = prefix_custom_keys(value)
    return result


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


def strip_req_cmd(schema: dict) -> tuple[dict, bool]:
    """Remove req/cmd properties and the oneOf requiring them.

    Returns (cleaned_schema, supports_cmd).
    """
    schema = copy.deepcopy(schema)
    supports_cmd = False

    props = schema.get("properties", {})
    if "cmd" in props:
        supports_cmd = True

    props.pop("req", None)
    props.pop("cmd", None)

    # Remove the top-level oneOf that just selects req vs cmd
    if "oneOf" in schema:
        oneof = schema["oneOf"]
        is_req_cmd_oneof = (
            len(oneof) == 2
            and any("req" in item.get("required", []) for item in oneof)
            and any("cmd" in item.get("required", []) for item in oneof)
        )
        if is_req_cmd_oneof:
            del schema["oneOf"]

    return schema, supports_cmd


def restore_req_cmd(endpoint: str, schema: dict, supports_cmd: bool) -> dict:
    """Re-add req/cmd properties and the oneOf selecting them."""
    schema = copy.deepcopy(schema)
    props = schema.setdefault("properties", {})

    props["req"] = {
        "description": "Request for the Notecard (expects response)",
        "const": endpoint,
    }
    if supports_cmd:
        props["cmd"] = {
            "description": "Command for the Notecard (no response)",
            "const": endpoint,
        }

    schema["oneOf"] = [
        {"required": ["req"], "properties": {"req": {"const": endpoint}}},
    ]
    if supports_cmd:
        schema["oneOf"].append(
            {"required": ["cmd"], "properties": {"cmd": {"const": endpoint}}}
        )

    return schema


def extract_body_schema(schema: dict) -> dict:
    """Extract a request/response body schema from a cleaned schema."""
    props = schema.get("properties", {})
    if not props:
        return {}

    body_schema = {"type": "object", "properties": props}

    if "required" in schema:
        body_schema["required"] = schema["required"]
    if schema.get("additionalProperties") is not None:
        body_schema["additionalProperties"] = schema["additionalProperties"]
    if "allOf" in schema:
        body_schema["allOf"] = schema["allOf"]

    return body_schema


def build_operation(
    endpoint: str,
    method: str,
    req_schema: dict,
    rsp_schema: dict | None,
    supports_cmd: bool,
    constraints: dict | None,
) -> dict:
    """Build a single OpenAPI operation object."""
    safety = SAFETY_TO_X[method]

    operation = {
        "operationId": endpoint.replace(".", "_"),
        "summary": req_schema.get("description", ""),
        "x-safety": safety,
        "x-notecard-request": endpoint,
    }

    if supports_cmd:
        operation["x-supports-cmd"] = True

    if constraints:
        operation["x-dispatch"] = constraints

    # Binary transfer annotation (set by caller)
    # Handled after build_operation returns

    # Preserve top-level metadata (use original key names, not yet prefixed)
    if "skus" in req_schema:
        operation["x-skus"] = req_schema["skus"]
    if "apiVersion" in req_schema:
        operation["x-api-version"] = req_schema["apiVersion"]
    if "version" in req_schema:
        operation["x-schema-version"] = req_schema["version"]
    if req_schema.get("minApiVersion"):
        operation["x-min-api-version"] = req_schema["minApiVersion"]
    if "title" in req_schema:
        operation["x-title"] = req_schema["title"]

    # Request body / parameters
    props = req_schema.get("properties", {})
    if props:
        body_schema = extract_body_schema(req_schema)
        body_schema = prefix_custom_keys(body_schema)

        if method == "GET":
            parameters = []
            for prop_name, prop_schema in props.items():
                prop_schema = prefix_custom_keys(copy.deepcopy(prop_schema))
                param = {
                    "name": prop_name,
                    "in": "query",
                    "schema": prop_schema,
                }
                if "description" in prop_schema:
                    param["description"] = prop_schema.pop("description")
                parameters.append(param)
            if parameters:
                operation["parameters"] = parameters
        else:
            operation["requestBody"] = {
                "content": {
                    "application/json": {
                        "schema": body_schema,
                    }
                },
            }

    # Samples
    if "samples" in req_schema:
        operation["x-samples"] = req_schema["samples"]

    # Response
    rsp_desc = "Successful response"
    if rsp_schema:
        rsp_desc = rsp_schema.get("description", rsp_desc)

    responses = {"200": {"description": rsp_desc}}
    if rsp_schema:
        rsp_body = extract_body_schema(rsp_schema)
        if rsp_body:
            rsp_body = prefix_custom_keys(rsp_body)
            responses["200"]["content"] = {
                "application/json": {"schema": rsp_body}
            }
        # Preserve response-level metadata
        rsp_meta = {}
        if "title" in rsp_schema:
            rsp_meta["x-title"] = rsp_schema["title"]
        if "samples" in rsp_schema:
            rsp_meta["x-samples"] = rsp_schema["samples"]
        if "skus" in rsp_schema:
            rsp_meta["x-skus"] = rsp_schema["skus"]
        if "version" in rsp_schema:
            rsp_meta["x-schema-version"] = rsp_schema["version"]
        if "apiVersion" in rsp_schema:
            rsp_meta["x-api-version"] = rsp_schema["apiVersion"]
        if rsp_schema.get("minApiVersion"):
            rsp_meta["x-min-api-version"] = rsp_schema["minApiVersion"]
        if rsp_meta:
            responses["200"].update(rsp_meta)

    operation["responses"] = responses

    return operation


def load_schemas(schema_dir: Path) -> tuple[dict, dict]:
    """Load all request and response schemas keyed by endpoint name."""
    requests = {}
    responses = {}

    for path in sorted(schema_dir.glob("*.req.notecard.api.json")):
        endpoint = path.name.replace(".req.notecard.api.json", "")
        with open(path) as f:
            requests[endpoint] = json.load(f)

    for path in sorted(schema_dir.glob("*.rsp.notecard.api.json")):
        endpoint = path.name.replace(".rsp.notecard.api.json", "")
        with open(path) as f:
            responses[endpoint] = json.load(f)

    return requests, responses


def load_safety(safety_path: Path) -> dict:
    """Load safety semantics JSON."""
    with open(safety_path) as f:
        data = json.load(f)
    data.pop("$comment", None)
    return data


def load_binary_transfer(binary_path: Path) -> dict:
    """Load binary transfer metadata JSON."""
    if not binary_path.exists():
        return {}
    with open(binary_path) as f:
        data = json.load(f)
    data.pop("$comment", None)
    return data


def load_property_extensions(extensions_path: Path) -> dict:
    """Load per-property extension overrides JSON.

    Returns {endpoint: {property_name: {ext_key: ext_value, ...}, ...}, ...}.
    """
    if not extensions_path.exists():
        return {}
    with open(extensions_path) as f:
        data = json.load(f)
    data.pop("$comment", None)
    return data


def detect_schema_commit(schema_dir: Path) -> str | None:
    """Auto-detect the current git commit of the schema directory."""
    try:
        result = subprocess.run(
            ["git", "-C", str(schema_dir), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=5,
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return None


def _apply_property_extensions(op: dict, ep_extensions: dict) -> None:
    """Merge per-property extensions into an operation's request schema.

    Works for both GET (parameters) and POST/PUT/DELETE (requestBody).
    ep_extensions maps property wire-names to dicts of extension keys/values.
    """
    # POST/PUT/DELETE: requestBody → content → application/json → schema → properties
    body = (op.get("requestBody", {})
              .get("content", {})
              .get("application/json", {})
              .get("schema", {}))
    props = body.get("properties", {})
    for prop_name, ext_obj in ep_extensions.items():
        if prop_name in props:
            props[prop_name].update(ext_obj)

    # GET: parameters list
    for param in op.get("parameters", []):
        name = param.get("name")
        if name in ep_extensions:
            param.get("schema", {}).update(ep_extensions[name])


def convert(schema_dir: Path, safety_path: Path,
            binary_path: Path | None = None,
            extensions_path: Path | None = None,
            schema_tag: str | None = None,
            schema_commit: str | None = None) -> dict:
    """Main conversion: JSON Schema files + safety semantics -> OpenAPI 3.1."""
    requests, responses = load_schemas(schema_dir)
    safety = load_safety(safety_path)
    binary = load_binary_transfer(binary_path) if binary_path else {}
    extensions = load_property_extensions(extensions_path) if extensions_path else {}

    # Auto-detect schema commit if not provided
    if schema_commit is None:
        schema_commit = detect_schema_commit(schema_dir)

    info = {
        "title": "Notecard API",
        "description": (
            "Machine-generated OpenAPI 3.1 specification for the Blues "
            "Notecard API, derived from the notecard-schema JSON Schema "
            "files with safety semantics annotations."
        ),
        "version": requests.get("card.version", {}).get("apiVersion", "0.0.0"),
        "contact": {"name": "Blues Inc.", "url": "https://blues.com"},
        "license": {
            "name": "Apache-2.0",
            "url": "https://www.apache.org/licenses/LICENSE-2.0",
        },
    }

    # Schema source tracking
    source = {}
    if schema_tag:
        source["tag"] = schema_tag
    if schema_commit:
        source["commit"] = schema_commit
    if source:
        info["x-schema-source"] = source

    openapi = {
        "openapi": "3.1.0",
        "info": info,
        "paths": {},
    }

    missing = []

    for endpoint in sorted(requests.keys()):
        req_schema = requests[endpoint]
        rsp_schema = responses.get(endpoint)

        if endpoint not in safety:
            missing.append(endpoint)
            continue

        req_cleaned, supports_cmd = strip_req_cmd(req_schema)
        semantics = safety[endpoint]
        path = endpoint_to_path(endpoint)
        path_item = {}

        ep_extensions = extensions.get(endpoint, {})

        if isinstance(semantics, str):
            method = semantics.lower()
            op = build_operation(
                endpoint, semantics, req_cleaned, rsp_schema,
                supports_cmd, None,
            )
            if endpoint in binary:
                op["x-binary-transfer"] = binary[endpoint]
            if ep_extensions:
                _apply_property_extensions(op, ep_extensions)
            path_item[method] = op

        elif isinstance(semantics, dict):
            for http_method, constraints in semantics.items():
                method = http_method.lower()
                op = build_operation(
                    endpoint, http_method, req_cleaned, rsp_schema,
                    supports_cmd, constraints if constraints else None,
                )
                suffix = {"GET": "query", "PUT": "set", "POST": "create",
                          "DELETE": "delete"}.get(http_method, method)
                op["operationId"] = f"{endpoint.replace('.', '_')}_{suffix}"
                if endpoint in binary:
                    op["x-binary-transfer"] = binary[endpoint]
                if ep_extensions:
                    _apply_property_extensions(op, ep_extensions)
                path_item[method] = op

        openapi["paths"][path] = path_item

    if missing:
        print(f"WARNING: {len(missing)} endpoints missing from safety_semantics.json:",
              file=sys.stderr)
        for ep in missing:
            print(f"  - {ep}", file=sys.stderr)

    return openapi


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("schema_dir", type=Path,
                        help="Path to notecard-schema repo")
    parser.add_argument("--safety", type=Path,
                        default=Path(__file__).parent / "safety_semantics.json",
                        help="Path to safety_semantics.json")
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).parent / "binary_transfer.json",
                        help="Path to binary_transfer.json")
    parser.add_argument("--extensions", type=Path,
                        default=Path(__file__).parent / "property_extensions.json",
                        help="Path to property_extensions.json")
    parser.add_argument("--schema-tag", type=str, default=None,
                        help="Upstream schema version tag (e.g. v1.2.7)")
    parser.add_argument("--schema-commit", type=str, default=None,
                        help="Schema repo commit (auto-detected if omitted)")
    parser.add_argument("-o", "--output", type=Path,
                        default=None,
                        help="Output file (default: stdout)")
    args = parser.parse_args()

    openapi = convert(args.schema_dir, args.safety, args.binary,
                      args.extensions, args.schema_tag, args.schema_commit)

    output = json.dumps(openapi, indent=2)
    if args.output:
        args.output.write_text(output + "\n")
        print(f"Wrote {args.output} ({len(openapi['paths'])} paths)",
              file=sys.stderr)
    else:
        print(output)


if __name__ == "__main__":
    main()
