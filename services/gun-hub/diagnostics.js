/**
 * Mesh Diagnostics for OrbitDB Hub
 * Provides insight into PubSub topics and peer counts.
 */
export const logMesh = async (orbitdb, stats) => {
    try {
        const topics = orbitdb.ipfs.libp2p.services.pubsub.getTopics();
        console.log(`[Pulse] Connected Peers: ${stats.peers} | Indexed DBs: ${stats.databases} | Total Blocks Synced: ${stats.blocksSynced} | Active Topics: ${topics.length}`);
        
        for (const t of topics) {
            const tp = orbitdb.ipfs.libp2p.services.pubsub.getPeers(t);
            if (tp.length > 0) {
                console.log(`  - Topic: ${t} | Mesh Peers: ${tp.length}`);
            }
        }
    } catch (err) {
        console.warn('[Diagnostics] Pulse error:', err.message);
    }
};
