import { join } from 'path'
import fs from 'fs'
import * as crypto from '@libp2p/crypto'
import { createLibp2p } from 'libp2p'
import { createHelia } from 'helia'
import { bitswap } from '@helia/block-brokers'
import { createOrbitDB, Identities, KeyStore } from '@orbitdb/core'
import { LevelBlockstore } from 'blockstore-level'
import { LevelDatastore } from 'datastore-level'
import { pipe } from 'it-pipe'
import Host from './lib/host.js'
import { voyagerRPCProtocol } from './rpc/protocol.js'
import { handleCommand } from './rpc/index.js'
import { Access } from './lib/authorization.js'
import { config as libp2pConfig } from './utils/libp2p-config.js'
import { rpc as rpcId, appPath, rpcPath, app, host as hostId, hostPath } from './utils/id.js'
import { saveConfig } from './utils/config-manager.js'
import { logger, enable } from '@libp2p/logger'

// 🛡️ PRODUCTION STABILIZATION: Global Error Guards
// Prevents fatal crashes from uncaught network errors (like StreamResetError) 
// that frequently occur during peer synchronization.
process.on('unhandledRejection', (reason, promise) => {
  const msg = reason?.message || reason?.toString() || 'Unknown Rejection';
  if (msg.includes('NoValidAddressesError') || msg.includes('no valid addresses')) return;
  if (
    msg.includes('stream reset') || 
    msg.includes('timeout') || 
    msg.includes('reset by peer') ||
    msg.includes("match the log's id") ||
    msg.includes('AbortError') ||
    msg.includes('aborted')
  ) {
    console.warn(`[RECOVERY] Suppressed expected network interruption: ${msg}`);
    return;
  }
  console.error('Unhandled Rejection at:', promise, 'reason:', reason);
});

process.on('uncaughtException', (err) => {
  const msg = err?.message || err?.toString();
  if (msg.includes('stream reset') || msg.includes('reset by peer')) {
    console.warn(`[RECOVERY] Suppressed fatal network exception: ${msg}`);
    return;
  }
  console.error('Uncaught Exception:', err);
  // Optional: process.exit(1) if you want it to restart, but continuing is better for P2P networks
});

const createRPCIdentity = async ({ id, directory }) => {
  const keystore = await KeyStore({ path: join(rpcPath(directory), 'keystore') })
  const identities = await Identities({ keystore })
  const identity = await identities.createIdentity({ id })
  await keystore.close()
  return {
    id: identity.id,
    hash: identity.hash,
    publicKey: identity.publicKey
  }
}

export default async ({ options }) => {
  let host
  console.log('Voyager daemon starting...')
  
  // 🛰️ NETWORK HEALTH MONITOR (Staggered & Robust)
  const SYNC_INTERVAL = 15000;
  const BATCH_SIZE = 5;
  const BATCH_DELAY = 1000;

  setInterval(async () => {
    try {
        const peers = host?.orbitdb?.ipfs?.libp2p?.getPeers() || []
        const topics = host?.orbitdb?.ipfs?.libp2p?.services?.pubsub?.getTopics() || []
        console.log(`[NET-HEALTH] Heartbeat Pulse | Peers: ${peers.length} | Active Topics: ${topics.length}`)
        
        if (host?.activeDbs) {
            const dbs = [...host.activeDbs];
            console.log(`[NET-HEALTH] Proactive pulse for ${dbs.length} databases...`)
            
            for (let i = 0; i < dbs.length; i += BATCH_SIZE) {
                const batch = dbs.slice(i, i + BATCH_SIZE);
                await Promise.all(batch.map(async (db) => {
                    try {
                        if (!db || !db.log || !db.sync || !db.sync.add || db.closed) return;
                        
                        const addr = db.address.toString()
                        
                        // 🛰️ PROACTIVE DIALING: Reach out to known owners of this DB
                        try {
                            const owners = await host.databases.get(addr);
                            if (owners && owners.length > 0) {
                                // console.log(`[NET-DIAL] DB ${addr} has ${owners.length} registered owners. Probing...`)
                                for (const ownerId of owners) {
                                    // Dial peer to ensure libp2p-level connectivity
                                    host.orbitdb.ipfs.libp2p.dial(ownerId).catch(() => {})
                                }
                            }
                        } catch (e) {}

                        const topicPeers = host.orbitdb.ipfs.libp2p.services.pubsub.getPeers(addr)
                        if (topicPeers.length > 0) {
                            for (const p of topicPeers) {
                                await db.sync.add(p).catch(() => {})
                            }
                            
                            // [FIXED] Removed the dangerous pubsub.unsubscribe/subscribe pulse-wake hack.
                            // Directly unsubscribing and resubscribing on libp2p.services.pubsub clears all internal
                            // event listeners registered by OrbitDB's sync module, permanently breaking real-time syncing.
                            // Proactive syncing is already fully achieved via db.sync.add(p) above.
                        }
                    } catch (err) {}
                }));
                if (i + BATCH_SIZE < dbs.length) {
                    await new Promise(r => setTimeout(r, BATCH_DELAY));
                }
            }
        }
    } catch (e) {
        console.error(`[NET-HEALTH] Pulse warning: ${e.message}`)
    }
  }, SYNC_INTERVAL)

  const log = logger('voyager:daemon')

  if (options.verbose > 0) {
    enable('voyager:daemon' + (options.verbose > 1 ? '*' : ':error'))
  }

  const defaultAccess = options.allow ? Access.ALLOW : Access.DENY

  options.verbose = options.verbose || 0
  options.port = options.port || 0
  options.wsport = options.wsport || 0

  log('app:', app)
  log('host:', hostId)
  log('rpc:', rpcId)

  const appDirectory = appPath(options.directory)
  const hostDirectory = hostPath(options.directory)

  log('directory:', hostDirectory)

  const blockstore = new LevelBlockstore(join(hostDirectory, '/', 'ipfs', '/', 'blocks'))
  const datastore = new LevelDatastore(join(hostDirectory, '/', 'ipfs', '/', 'data'))
  
  let privateKey
  const keyFile = '/data/peer-id.json'
  if (fs.existsSync(keyFile)) {
    const raw = fs.readFileSync(keyFile, 'utf8')
    const json = JSON.parse(raw)
    privateKey = await crypto.keys.privateKeyFromProtobuf(Buffer.from(json.privKey, 'base64'))
    console.log('DEBUG: Loaded PeerID from file:', json.id)
  }

  const libp2p = await createLibp2p(libp2pConfig({ port: options.port, websocketPort: options.wsport, privateKey }))

  log('peerid:', libp2p.peerId.toString())

  const addresses = libp2p.getMultiaddrs().map(e => e.toString())
  for (const addr of addresses) {
    console.log(addr)
  }
  for (const addr of addresses) {
    log('listening on', addr)
  }

  const ipfs = await createHelia({ libp2p, datastore, blockstore, blockBrokers: [bitswap()] })
  const orbitdb = await createOrbitDB({ ipfs, directory: hostDirectory, id: hostId })
  host = await Host({ defaultAccess, verbose: options.verbose, orbitdb })

  const authorizedRPCIdentity = await createRPCIdentity({ id: rpcId, directory: options.directory })

  const rpcConfig = {
    address: host.orbitdb.ipfs.libp2p.getMultiaddrs().shift(), // get 127.0.0.1 address
    identities: [authorizedRPCIdentity]
  }
  await saveConfig({ path: appDirectory, config: rpcConfig })

  const handleRPCMessages = async ({ stream }) => {
    await pipe(stream, handleCommand(rpcConfig, host), stream)
  }

  await host.orbitdb.ipfs.libp2p.handle(voyagerRPCProtocol, handleRPCMessages)
  
  // 🛰️ NETWORK HEALTH MONITOR (Event-based)
  host.orbitdb.ipfs.libp2p.addEventListener('peer:connect', (evt) => {
    const peerId = evt.detail.toString()
    console.log(`[NET] 🖇️ Peer Connected: ${peerId}`)
    
    // Auto-add to active mesh for proactive sync
    if (!global.activeMesh) global.activeMesh = new Set();
    global.activeMesh.add(peerId);

    setTimeout(async () => {
        try {
            const protocols = await host.orbitdb.ipfs.libp2p.getProtocols(evt.detail)
            console.log(`[NET] Protocols for ${peerId}:`, protocols.filter(p => p.includes('orbitdb')))
        } catch (e) {}
    }, 2000)
  })

  process.on('SIGINT', async () => {
    await host.stop()
    await blockstore.close()
    await datastore.close()
    process.exit(0)
  })

  console.log('Voyager started')
}
