# flAPI BigQuery agent demo — record from /tmp/bqdemo (flapi.yaml + sqls/).
# Needs: locally built flapi (>= fix #100) on PATH, Google ADC for BigQuery,
# port 8086 free, claude CLI logged in.
Output flapi-demo-bigquery.gif

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

Type `# Put an AI agent on top of BigQuery — one SQL template, one YAML tool:`
Enter
Sleep 1.5s
Type `cat sqls/babynames.sql`
Enter
Sleep 4s

Type `# state/top are typed params -> bound as prepared statements. Serve it:`
Enter
Sleep 1s
Type `flapi -c flapi.yaml --port 8086 > /tmp/flapi.log 2>&1 &`
Enter
Hide
Sleep 60s
Show
Type `claude mcp add --transport http flapi http://localhost:8086/mcp/jsonrpc`
Enter
Sleep 3s

Type `claude -p "What are the five most popular baby names in Texas since 1910? Use the flapi tools." --allowedTools mcp__flapi__top_baby_names`
Enter
Hide
Sleep 90s
Show
Sleep 2s

Type `# The model never writes SQL — it can only call your curated, validated query.`
Enter
Sleep 4s
