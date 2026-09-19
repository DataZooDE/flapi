# flAPI self-packaging demo — record from the pack-demo dir containing myapi/
Output flapi-demo-pack.gif

Set FontSize 17
Set Width 1200
Set Height 720
Set Padding 12
Set Theme "Catppuccin Mocha"
Set TypingSpeed 35ms
Set Shell bash

Hide
Type `rm -f flapi-prod /tmp/flapi-prod && clear`
Enter
Sleep 1s
Show

Type `# Your whole API is a folder: YAML config + SQL templates + data`
Enter
Sleep 1s
Type `tree myapi`
Enter
Sleep 2.5s

Type `# Fold it into the flapi binary itself:`
Enter
Sleep 1s
Type `uvx --from flapi-io flapi pack --in ./myapi --out flapi-prod`
Enter
Sleep 4s
Type `./flapi-prod info`
Enter
Sleep 3s

Type `# One self-contained file — the deploy is scp:`
Enter
Sleep 1s
Type `cp flapi-prod /tmp/ && cd /tmp   # stand-in for: scp flapi-prod user@host:`
Enter
Sleep 1.5s
Type `./flapi-prod --port 8085 > flapi.log 2>&1 &`
Enter
Hide
Sleep 9s
Show
Type `curl -s 'localhost:8085/customers?id=42' | jq .data`
Enter
Sleep 3.5s

Type `# config, SQL and data all served from inside one binary — from any cwd`
Enter
Sleep 3s
