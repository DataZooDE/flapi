# flAPI SharePoint-lists agent demo — record from /tmp/spdemo.
# Needs: locally built flapi (>= fix #100) on PATH, erpl-web .env.microsoft
# credentials (Azure app w/ Graph Sites.Read), claude CLI, port 8087 free.
Output flapi-demo-sharepoint.gif

Set FontSize 17
Set Width 1200
Set Height 720
Set Padding 12
Set Theme "Catppuccin Mocha"
Set TypingSpeed 35ms
Set Shell bash

Hide
Type `export PATH=/home/jr/Projects/datazoo/flapi/build/debug:$PATH; set -a; source /home/jr/Projects/datazoo/erpl-web/.env.microsoft; set +a; claude mcp remove flapi >/dev/null 2>&1; clear`
Enter
Sleep 2s
Show

Type `# Your team's SharePoint list as an API + AI tool (DuckDB + erpl-web):`
Enter
Sleep 1.5s
Type `sed -n '/init:/,/list:/p' flapi.yaml`
Enter
Sleep 4.5s

Type `flapi -c flapi.yaml --port 8087 > /tmp/flapi.log 2>&1 &`
Enter
Hide
Sleep 65s
Show
Type `claude mcp add --transport http flapi http://localhost:8087/mcp/jsonrpc`
Enter
Sleep 3s

Type `claude -p "Which projects in our SharePoint tracker are less than 50% complete, and when are they due? Use the flapi tools." --allowedTools mcp__flapi__project_tracker`
Enter
Hide
Sleep 75s
Show
Sleep 2s

Type `# Live Microsoft 365 data — OAuth2 via DuckDB secrets. The model never writes SQL.`
Enter
Sleep 4s
