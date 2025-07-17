CC=clang
PROGRAM=fortuna
SRC=$(wildcard src/*.c)
OBJ=$(SRC:src/%.c=obj/%.o)

CFLAGS  =  -O3 -march=native -Wall -Wshadow -Wnull-dereference -Wimplicit-fallthrough -Wundef 
CFLAGS  += -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIC -fPIE 
CFLAGS  += -fno-omit-frame-pointer 

TOPO_SRC = lib/maketopologicf90.c
TOPO     = bin/maketopologicf90

all: $(PROGRAM) $(TOPO)

${PROGRAM}: $(OBJ)
	$(CC) -o ${PROGRAM} $(CFLAGS) $(OBJ)

${TOPO}: $(TOPO_SRC)
	$(CC) -o ${TOPO} $(CFLAGS) $(TOPO_SRC)

obj/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@ 

clean:
	rm -rf obj/*.o

install:
	@echo "Detecting environment..."
	@if command -v bash >/dev/null 2>&1; then \
		echo "Bash found. Running Bash install script..."; \
		bash ./scripts/bash_install.sh; \
	elif command -v powershell >/dev/null 2>&1; then \
		echo "Bash not found. Trying PowerShell..."; \
		powershell  -File ./scripts/win_install.ps1; \
	elif command -v pwsh >/dev/null 2>&1; then \
		echo "Bash not found. Using PowerShell Core..."; \
		pwsh  -File ./scripts/win_install.ps1; \
	else \
		echo "Neither Bash nor PowerShell was found. Cannot modify PATH."; \
	fi