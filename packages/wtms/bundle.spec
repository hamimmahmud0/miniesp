# wtms bundle (read by tools/mkbundle.py): builds the program, hashes every file, writes pkgs/wtms/ and the journal entry.
name wtms
version 1.0
abi 4
# program SOURCE.c -> DEST on the board (built with programs/build.sh)
program ../../programs/wtms.c -> /esp/.local/bin/wtms.aot
# wtmsd = the same program without `setup` (40 KB instead of 57 KB): used by the service and the web page's CGI, because a big
# program may fail to load while an SSH session has fragmented RAM
program ../../programs/wtms.c -> /esp/.local/bin/wtmsd.aot flags=-DNO_SETUP name=wtmsd.aot
copy /esp/.local/bin/wtmsd.aot /www/cgi-bin/wtms.aot
dir /www/tank
file web/index.html -> /www/tank/index.html
file web/config.html -> /www/tank/config.html
file web/login.html -> /www/tank/login.html
file web/tank.css -> /www/tank/tank.css
file web/tank.js -> /www/tank/tank.js
service wtms
note wtms installed. Next:  wtms setup   (interactive: web port, MQTT broker/port, tank size, pins ...)
note or without prompts:  wtms setup --headless mqtt_host=192.168.0.50 mqtt_pass=SECRET web_port=80 length=100 width=100 height=100 full_cm=15
