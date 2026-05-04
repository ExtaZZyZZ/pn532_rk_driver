savedcmd_pn532.mod := printf '%s\n'   pn532.o | awk '!x[$$0]++ { print("./"$$0) }' > pn532.mod
