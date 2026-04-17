# Applies -DAPI_STYLE=<value> from the API_STYLE env var, if set.
# When unset, the source's #ifndef guard supplies the default (STYLE=1).
import os
Import("env")

style = os.environ.get("API_STYLE")
if style:
    env.Append(CPPDEFINES=[("API_STYLE", style)])
    print("apply_api_style.py: API_STYLE=%s" % style)
