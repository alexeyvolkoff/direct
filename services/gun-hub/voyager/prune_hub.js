import { createOrbitDB } from '@orbitdb/core'
import { LevelBlockstore } from 'blockstore-level'
import { LevelDatastore } from 'datastore-level'
import { hostPath } from './src/utils/id.js'
import fs from 'fs'

const problematicHashes = [
    'zdpuB1rdpeABgk2CkK4MYMiERh91obqEJbNvSMSPFy4cHZhBk',
    'zdpuAxxiQ1KRptechLNHzcsZftwr7D2DWzvYFcRZYgQeJvt9z',
    'zdpuAvkrDRHaBvB2hmyMBbjxYrWsdendTscW7dXXg1qZkfYaN',
    'zdpuApjaBjZGoW9buQM9JnrMWn535Z134Atg9HVZgDYYHQtUL',
    'zdpuAo1T3jKDzuBvc5TmVgLwEDjusHW8m9KMUSbW6Lh4swMKL',
    'zdpuAkmvWxcGsRBuqqbzKwp6HCrp2bpkq4Rej3gGQSeF87t7N',
    'zdpuAoodNHN7rZ6336vgViZDmBFyrb3a6HjFEiF3FshV1jz4f'
];

async function prune() {
    console.log("OrbitDB Hub Pruner starting...");
    const directory = '/data/voyager/host';
    
    const orbitdb = await createOrbitDB({ directory });
    const databases = await orbitdb.open('databases', { type: 'keyvalue' });
    
    console.log("Scanning Hub index...");
    let prunedCount = 0;
    
    for (const hash of problematicHashes) {
        const address = `/orbitdb/${hash}`;
        const owners = await databases.get(address);
        
        if (owners) {
            console.log(`[PRUNE] Removing disowned database: ${address}`);
            await databases.del(address);
            prunedCount++;
            
            // Note: We don't delete the actual data folder here to be safe, 
            // but the Hub will no longer open/sync it on boot.
        } else {
             console.log(`[SKIP] Hash not found in Hub index: ${hash}`);
        }
    }
    
    console.log(`Pruning complete. Removed ${prunedCount} entries.`);
    await databases.close();
    await orbitdb.stop();
}

prune().catch(console.error);
