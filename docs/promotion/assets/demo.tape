# VHS tape for the flAPI 60-second demo GIF
# Record from the repo root:  vhs docs/promotion/assets/demo.tape
# Requires: vhs, ttyd, ffmpeg, uv (uvx), jq; port 8084 free.

Output docs/promotion/assets/flapi-demo.gif

Set FontSize 17
Set Width 1200
Set Height 720
Set Padding 12
Set Theme "Catppuccin Mocha"
Set TypingSpeed 35ms
Set Shell bash

Type `# flAPI — one SQL template + YAML  =>  REST endpoint + MCP tool`
Enter
Sleep 1.5s

Type `uvx --from flapi-io flapi -c examples/flapi-test.yaml --port 8084 > /tmp/flapi.log 2>&1 &`
Enter
Hide
Sleep 28s
Show
Type `grep -m1 "running at" /tmp/flapi.log`
Enter
Sleep 2s

Type `# REST: typed params, validators, Basic auth, DuckLake cache`
Enter
Sleep 1s
Type `curl -s -u admin:secret 'localhost:8084/customers/?id=42' | jq .data`
Enter
Sleep 3.5s

Type `# ...and the same config exposes it as an MCP tool (per-tool RBAC):`
Enter
Sleep 1s
Type `curl -s -u admin:secret -X POST localhost:8084/mcp/jsonrpc -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | jq -r '.result.tools[].name'`
Enter
Sleep 2.5s

Type `curl -s -u admin:secret -X POST localhost:8084/mcp/jsonrpc -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"customer_lookup","arguments":{"id":42}}}' | jq '.result.content[0].text | fromjson'`
Enter
Sleep 4.5s

Type `# github.com/DataZooDE/flapi — pip install flapi-io`
Enter
Sleep 3s
