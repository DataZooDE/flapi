# flAPI agent demo — Claude Code calling flAPI MCP tools.
# Record from a dir containing examples/ (flapi-test.yaml). Needs the
# locally built flapi (>= fix #100) on PATH and port 8084 free.
Output flapi-demo-agent.gif

Set FontSize 17
Set Width 1200
Set Height 720
Set Padding 12
Set Theme "Catppuccin Mocha"
Set TypingSpeed 35ms
Set Shell bash

Hide
Type `export PATH=/home/jr/Projects/datazoo/flapi/build/debug:$PATH; claude mcp remove flapi >/dev/null 2>&1; clear`
Enter
Sleep 2s
Show

Type `# Every flAPI endpoint is also an MCP tool — let an agent talk to your data:`
Enter
Sleep 1.5s
Type `flapi -c examples/flapi-test.yaml --port 8084 > /tmp/flapi.log 2>&1 &`
Enter
Hide
Sleep 45s
Show
Type `curl -s -m 5 localhost:8084/mcp/health | jq '{status, tools_count}'`
Enter
Sleep 2.5s

Type `# Point Claude Code at it:`
Enter
Sleep 1s
Type `claude mcp add --transport http flapi http://localhost:8084/mcp/jsonrpc --header "Authorization: Basic YWRtaW46c2VjcmV0"`
Enter
Sleep 3s

Type `# Ask questions in plain English:`
Enter
Sleep 1s
Type `claude -p "Which market segment is customer 42 in, and what is their balance? Use the flapi tools." --allowedTools mcp__flapi__customer_lookup`
Enter
Hide
Sleep 60s
Show
Sleep 1.5s

Type `# Same SQL template, same validators, same per-tool RBAC as the REST endpoint.`
Enter
Sleep 3.5s
