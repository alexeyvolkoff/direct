# syntax=docker/dockerfile:1

FROM node:20.17 AS build-env
ENV NODE_ENV=production

WORKDIR /app
COPY ["package.json", "package-lock.json*", "./"]
RUN npm ci --only=production

# --- RESILIENCE PATCH ---
# This patch prevents the hub from crashing when encountering incompatible Log IDs or malformed entries.
# We relax the ID check to allow compatibility with newer OrbitDB versions (v3) which might have different entry structures.
RUN node -e "const fs = require('fs'); const path = 'node_modules/@orbitdb/core/src/oplog/log.js'; let code = fs.readFileSync(path, 'utf8'); code = code.replace('await verifyEntry(entry)', 'if (!entry || !entry.next || (entry.id && entry.id !== id)) { console.warn(\"[VOYAGER] Malformed entry aborted. Entry ID: \" + entry.id + \" Log ID: \" + id); return false; } await verifyEntry(entry)'); fs.writeFileSync(path, code);"

# Remove nested @orbitdb/set-db that resolves to an incompatible version (1.2.1+ requires @orbitdb/core@3.x)
# Keep root-level @orbitdb/set-db@1.1.2 which is the correct version for this setup
RUN rm -rf voyager/node_modules/@orbitdb/set-db

# --- FIX: joinEntry infinite recursion in traverseAndVerify ---
# When joinEntry receives a head whose parent entries are not in the local
# index, traverseAndVerify enters an infinite recursive loop because
# .filter(has) removes non-indexed entries and the recursion never makes
# progress. Fix: (1) remove .filter(has) so Bitswap can fetch missing
# entries, and (2) guard recursion with entries.length > 0 to prevent
# infinite loop when entries are truly unreachable (e.g. peer behind NAT).
RUN node -e "const fs = require('fs'); const path = 'node_modules/@orbitdb/core/src/oplog/log.js'; let code = fs.readFileSync(path, 'utf8'); code = code.replace('const getEntries = Array.from(hashesToGet.values()).filter(has).map(get)\n        const entries = await Promise.all(getEntries)', 'const getEntries = Array.from(hashesToGet.values()).map(get)\n        const entries = (await Promise.all(getEntries)).filter(Boolean)'); code = code.replace('if (hashesToGet.size > 0) {\n          await traverseAndVerify()', 'if (hashesToGet.size > 0 && entries.length > 0) {\n          await traverseAndVerify()'); fs.writeFileSync(path, code);"

COPY . .

FROM node:20.17-slim
COPY --from=build-env /app /app
WORKDIR /app
CMD [ "node", "./src/bin/cli.js", "daemon" ]
