import { logger } from '@libp2p/logger'

const log = logger('voyager:add')

const waitForReplication = (db) => {
  return new Promise((resolve, reject) => {
    db.events.once('join', () => resolve())
    db.events.once('error', (err) => reject(err))
  })
}

export default async ({ host, id, addresses }) => {
  const { orbitdb, databases } = host
  for (const address of addresses) {
    log('add database', address)

    let identities = await databases.get(address)
    const hasDb = identities !== undefined

    if (identities) {
      identities.push(id)
    } else {
      identities = [id]
    }

    console.log(`[Pin] Opening database: ${address}...`)
    const db = await orbitdb.open(address, { sync: true })
    await host.registerActiveDb(db)
    host.emit('pinning:database', db)
    console.log(`[Pin] Database opened! Address: ${db.address.toString()} (ID: ${db.log.id})`)

    db.events.on('replicated', (address, heads) => {
        console.log(`[SYNC-ADD] 📥 NEW DATA for ${address} | New Heads: ${heads.length}`)
    })
    
    db.events.on('update', (entry) => {
         console.log(`[SYNC-ADD] 📝 Update received for ${address}`)
    })

        // PROACTIVE DIAL: Force connection to the requesting identity's peer
        if (id) {
            if (!global.activeMesh) global.activeMesh = new Set();
            global.activeMesh.add(id);
            
            console.log(`[Pin] Proactive Dial: Reaching out to requesting peer/identity ${id}...`)
            orbitdb.ipfs.libp2p.dial(id).catch(() => {})
            
            // 🕒 SETTLING DELAY: Give OrbitDB 1.5 seconds to settle before aggressive sync
            setTimeout(async () => {
                try {
                    const peers = await orbitdb.ipfs.libp2p.services.pubsub.getPeers(address)
                    const freshHeads = await db.log.heads()
                    
                    console.log(`[Pin] Topic ${address} has ${peers.length} peers. Hub heads: ${freshHeads.length}`)
                    
                    if (peers.length > 0) {
                        console.log(`[Pin] Topic ${address} has ${peers.length} peers. Hub heads: ${freshHeads.length}. Sync is automatic via GossipSub.`)
                    }
                } catch (err) {
                    console.error(`[Pin] Handshake sync error for ${address}:`, err)
                }
            }, 1500);
        }

    // 🔄 BLOCK PREFETCH + BROADCAST: Walk ALL heads and their entire `next` chains
    // to pull ALL entry blocks into the local blockstore, then broadcast each entry's
    // bytes via pubsub so connected peers receive them directly (bypassing Bitswap).
    // This fixes LoadBlockFailedError for entries on secondary forks that Bitswap
    // between client and hub fails to transfer.
    setTimeout(async () => {
        try {
            let prefetched = 0;
            const visited = new Set();
            const queue = [];
            let allCids = [];
            try {
                const heads = await db.log.heads();
                console.log(`[DIAG] ${address} heads: ${heads.length}`);
                for (const head of heads) {
                    queue.push(head.hash);
                    allCids.push('HEAD:' + head.hash);
                }
            } catch (e) {
                console.error(`[DIAG] Failed to get heads for ${address}: ${e.message}`);
            }
            while (queue.length > 0 && prefetched < 200) {
                const hash = queue.shift();
                if (visited.has(hash)) continue;
                visited.add(hash);
                try {
                    const entry = await db.log.get(hash);
                    const _ = entry.payload;
                    prefetched++;
                    if (entry.next) {
                        for (const next of entry.next) {
                            if (!visited.has(next)) {
                                queue.push(next);
                                allCids.push('NEXT:' + next);
                            }
                        }
                    }
                } catch (e) {
                    console.warn(`[Pin] ⚠️ Block not available for ${hash}: ${e.message}`);
                    allCids.push('MISSING:' + hash);
                }
            }
            if (prefetched > 0) {
                console.log(`[Pin] ✅ Prefetched ${prefetched} entries across all branches for ${address}`);
                console.log(`[DIAG] ALL ${address} CIDs: ${allCids.join(', ')}`);
            }

            // 📢 BROADCAST all prefetched entries via pubsub so clients receive
            // entry bytes directly (bypassing Bitswap). The client's onSynced handler
            // calls log.joinEntry() which stores entries in-memory, making them
            // available to log.get() without hitting the blockstore.
            let broadcast = 0;
            for (const hash of visited) {
                try {
                    const entry = await db.log.get(hash);
                    await db.sync.add(entry);
                    broadcast++;
                    if (broadcast % 10 === 0) {
                        await new Promise(r => setTimeout(r, 100));
                    }
                } catch (e) {
                    console.warn(`[Pin] ⚠️ Broadcast failed for ${hash}: ${e.message}`);
                }
            }
            if (broadcast > 0) {
                console.log(`[Pin] 📡 Broadcast ${broadcast} entries for ${address}`);
            }

            // Also dump iterator entries for comparison
            try {
                let iterCount = 0;
                const iterCids = [];
                for await (const entry of db.iterator({ limit: 50 })) {
                    iterCount++;
                    iterCids.push(entry.hash);
                }
                console.log(`[DIAG] Iterator ${address}: ${iterCount} entries, CIDs: ${iterCids.join(', ')}`);
            } catch (e) {
                console.log(`[DIAG] Iterator failed for ${address}: ${e.message}`);
            }
        } catch (e) {
            console.error(`[Pin] Prefetch error for ${address}:`, e.message);
        }
    }, 3000);

    console.log(`[Pin] Updating index for: ${address}...`)
    await databases.set(address, identities)
    console.log(`[Pin] Index updated!`)

    log('database added', address)
  }
}
