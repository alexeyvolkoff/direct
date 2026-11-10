import { createHelia } from 'helia';
import { createLibp2p } from 'libp2p';
import { createOrbitDB } from '@orbitdb/core';
import { bitswap } from '@helia/block-brokers';
import { FsBlockstore } from 'blockstore-fs';
import { FsDatastore } from 'datastore-fs';
import fs from 'fs';
import path from 'path';

const ORBITDB_DIR = fs.existsSync('/data/orbitdb') ? '/data/orbitdb' : './data/orbitdb';
const HELIA_DIR = fs.existsSync('/data/helia') ? '/data/helia' : './data/helia';
const PINNED_FILE = path.join(ORBITDB_DIR, 'pinned.json');
const BLOCKSTORE_DIR = HELIA_DIR;
const AUDIT_DIR = '/tmp/orbitdb_audit_final';

async function runAudit() {
    console.log("--- HUB FULL-REGISTRY AUDIT ---");
    
    if (!fs.existsSync(PINNED_FILE)) {
        console.error("Pinned registry not found at", PINNED_FILE);
        return;
    }

    let pinnedList = [];
    try {
        const raw = JSON.parse(fs.readFileSync(PINNED_FILE, 'utf8'));
        pinnedList = raw.map(item => typeof item === 'string' ? { addr: item } : item);
    } catch (e) {
        console.error("Failed to parse pinned.json:", e.message);
        return;
    }

    console.log(`Auditing ${pinnedList.length} databases...`);

    const blockstore = new FsBlockstore(BLOCKSTORE_DIR);
    
    // 1. Create a PURE MEMORY Mock Datastore to bypass all filesystem bugs/shards
    const memoryCache = new Map();
    const datastore = {
        put: async (key, val) => { memoryCache.set(key.toString(), val) },
        get: async (key) => { 
            const val = memoryCache.get(key.toString());
            if (!val) throw new Error('not found');
            return val;
        },
        has: async (key) => memoryCache.has(key.toString()),
        delete: async (key) => { memoryCache.delete(key.toString()) },
        query: async function* () { for (const [k, v] of memoryCache) yield { key: k, value: v } },
        batch: () => ({
            put: (k, v) => memoryCache.set(k.toString(), v),
            delete: (k) => memoryCache.delete(k.toString()),
            commit: async () => {}
        }),
        open: async () => {},
        close: async () => {}
    };

    // 2. Create an OFFLINE libp2p instance 
    const libp2p = await createLibp2p({
        start: false,
        connectionManager: { minConnections: 0 }
    });

    // 3. Initialize Helia with separate temp datastore and MOCKED PINS
    // This avoids the 'pinned-block' ENOENT crash entirely
    const helia = await createHelia({ 
        libp2p, 
        blockstore, 
        datastore,
        pins: {
            add: async () => {},
            ls: async function* () {},
            rm: async () => {},
            check: async () => ({ isPinned: false })
        }
    });
    
    // 4. Initialize OrbitDB engine in a memory-safe way
    const orbitdb = await createOrbitDB({ 
        ipfs: helia, 
        directory: AUDIT_DIR,
    });

    const results = {
        totalDbs: pinnedList.length,
        withData: 0,
        empty: 0,
        errors: 0,
        details: []
    };

    for (const item of pinnedList) {
        process.stdout.write(`Checking ${item.addr.substring(0, 20)}... `);
        try {
            // Open with a short timeout to prevent hanging on missing manifests
            const db = await Promise.race([
                orbitdb.open(item.addr),
                new Promise((_, r) => setTimeout(() => r(new Error('timeout')), 15000))
            ]);

            let count = 0;
            // Iterate briefly to check for data
            for await (const record of db.iterator()) {
                count++;
                if (count >= 100) break; // Cap audit per DB for speed
            }

            if (count > 0) {
                results.withData++;
                console.log(`FOUND ${count}+ records (Owner: ${item.user || 'unknown'})`);
            } else {
                results.empty++;
                console.log(`EMPTY`);
            }

            results.details.push({ addr: item.addr, count, user: item.user, name: item.name });
            await db.close();
        } catch (e) {
            results.errors++;
            console.log(`ERROR: ${e.message}`);
        }
    }

    console.log("\n--- AUDIT SUMMARY ---");
    console.log(`Total Databases: ${results.totalDbs}`);
    console.log(`With Data:       ${results.withData}`);
    console.log(`Empty Shells:    ${results.empty}`);
    console.log(`Open Errors:     ${results.errors}`);
    
    fs.writeFileSync('audit_results.json', JSON.stringify(results, null, 2));
    console.log("\nDetailed results saved to audit_results.json");

    await orbitdb.stop();
    await helia.stop();
    process.exit(0);
}

runAudit().catch(err => {
    console.error("Audit crashed:", err);
    process.exit(1);
});
