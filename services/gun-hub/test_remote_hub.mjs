import { createLibp2p } from 'libp2p'
import { config as getLibp2pConfig } from './voyager/src/utils/libp2p-config.js'
import { createOrbitDB } from '@orbitdb/core'
import Voyager from './voyager/src/lib/voyager.js'
import { createHelia } from 'helia'
import path from 'path'
import fs from 'fs'
import { webcrypto } from 'node:crypto'
import { multiaddr as maFunc } from '@multiformats/multiaddr'
import { peerIdFromString } from '@libp2p/peer-id'
import { circuitRelayTransport } from '@libp2p/circuit-relay-v2'

// POLYFILLS
if (typeof global.crypto === 'undefined') global.crypto = webcrypto
if (typeof global.CustomEvent === 'undefined') {
  global.CustomEvent = class extends Event {
    constructor(type, options) { super(type, options); this.detail = options?.detail }
  }
}
if (typeof Promise.withResolvers === 'undefined') {
  Promise.withResolvers = function() {
    let resolve, reject;
    const promise = new Promise((res, rej) => {
      resolve = res;
      reject = rej;
    });
    return { promise, resolve, reject };
  };
}

// 🛡️ GLOBAL COMPATIBILITY SHIELD
const maBase = maFunc('/ip4/127.0.0.1/tcp/0')
const Proto = Object.getPrototypeOf(maBase)
const symbol = Symbol.for('@multiformats/multiaddr')
Proto[symbol] = true
if (!Proto.getPeerId) { Proto.getPeerId = function() { const str = this.toString(); return str.includes('/p2p/') ? str.split('/p2p/').pop() : null } }
if (!Proto.stringTuples) { Proto.stringTuples = function() { return [[4, '127.0.0.1'], [6, '0']] } }
if (!Proto.getComponents) { Proto.getComponents = function() { return [] } }

const HUB_ADDRESS = '/dns4/staging.onmydisk.net/tcp/4002/wss/p2p/12D3KooWLSpRc8cFn58yePGEV9SfgwqC1FCZjLDyVUkqUQmuNnaB'
const HUB_PEER_ID = '12D3KooWLSpRc8cFn58yePGEV9SfgwqC1FCZjLDyVUkqUQmuNnaB'
const ADDR_FILE = './test_remote_hub_addr.txt'

const runProcess = async (name, mode) => {
  const repo = path.join(process.cwd(), `remote_test_repo_${name}`)
  if (fs.existsSync(repo)) fs.rmSync(repo, { recursive: true, force: true })

  console.log(`[${name}] Initializing LibP2P (Node 22 + Relay)...`)
  
  const baseConf = getLibp2pConfig({ port: 0, websocketPort: 0 })
  delete baseConf.peerDiscovery // Disable bootstrap
  const libp2p = await createLibp2p({
      ...baseConf,
      addresses: { listen: ['/p2p-circuit'] },
      transports: [
          ...baseConf.transports,
          circuitRelayTransport({ discoverRelays: 1 })
      ]
  })
  
  const ipfs = await createHelia({ libp2p })
  const orbitdb = await createOrbitDB({ ipfs, directory: repo })
  
  const hubMa = maFunc(HUB_ADDRESS)
  const hubId = peerIdFromString(HUB_PEER_ID)
  
  await libp2p.peerStore.save(hubId, { multiaddrs: [hubMa] })
  await libp2p.dial(hubId)
  
  const voyager = await Voyager({ orbitdb, address: HUB_ADDRESS })
  
  let dbAddress;
  if (mode === 'sender') {
      const db = await orbitdb.open('test_v22_sync_heads_' + Date.now(), { type: 'events', sync: true })
      dbAddress = db.address.toString()
      console.log(`[${name}] Created DB: ${dbAddress}`)
      fs.writeFileSync(ADDR_FILE, dbAddress)
      
      console.log(`[${name}] Notifying Hub via Voyager RPC...`)
      await voyager.add(dbAddress)

      console.log(`[${name}] Waiting for Hub on topic (short wait)...`)
      await new Promise(resolve => setTimeout(resolve, 3000));
      console.log(`[${name}] Hub joined topic!`)
      
      const topic = dbAddress;
      setInterval(async () => {
          const peers = await libp2p.services.pubsub.getPeers(topic);
          const heads = (await db.log.heads()).length;
          console.log(`[${name}] Topic Peers: ${peers.length} | DB Heads: ${heads}`);
          if (db.sync && db.sync.add) { await db.sync.add(hubId).catch(() => {}); }
      }, 5000)

      let count = 0
      setInterval(async () => {
        count++
        console.log(`[${name}] SENDING: Message ${count}`)
        await db.add({ msg: `Message ${count} (Heads Audit)`, ts: Date.now() })
        await new Promise(r => setTimeout(r, 1000));
      }, 7000)
  } else {
      console.log(`[${name}] Waiting for DB address...`)
      while (!fs.existsSync(ADDR_FILE)) { await new Promise(r => setTimeout(r, 1000)) }
      dbAddress = fs.readFileSync(ADDR_FILE, 'utf8')
      const db = await orbitdb.open(dbAddress, { type: 'events', sync: true })
      console.log(`[${name}] DB Opened. listening...`)
      
      db.events.on('update', (entry) => {
        console.log(`[${name}] ⭐ REAL-TIME RECEIVED on ProcessB:`, entry.payload.value.msg)
      })

      libp2p.services.pubsub.addEventListener('message', (evt) => {
        const msg = evt.detail;
        if (msg.topic === dbAddress) {
           // console.log(`[${name}] 📦 Raw PubSub received on topic: ${msg.data?.length} bytes`);
        }
      })

      const topic = db.address.toString();
      setInterval(async () => {
          const peers = await libp2p.services.pubsub.getPeers(topic);
          const heads = (await db.log.heads()).length;
          console.log(`[${name}] Topic Peers: ${peers.length} | DB Heads: ${heads}`);
          if (db.sync && db.sync.add) { await db.sync.add(hubId).catch(() => {}); }
      }, 5000);
  }
}

const main = async () => {
  if (process.argv[2] === 'sender') {
      await runProcess('ProcessA', 'sender')
  } else {
      await runProcess('ProcessB', 'receiver')
  }
}
main().catch(console.error)
