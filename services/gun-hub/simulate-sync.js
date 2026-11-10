import { createHelia } from 'helia'
import { createLibp2p } from 'libp2p'
import { createOrbitDB } from '@orbitdb/core'
import { webSockets } from '@libp2p/websockets'
import { noise } from '@chainsafe/libp2p-noise'
import { yamux } from '@chainsafe/libp2p-yamux'
import { gossipsub } from '@chainsafe/libp2p-gossipsub'
import { identify } from '@libp2p/identify'
import { webcrypto } from 'node:crypto'
import { multiaddr } from '@multiformats/multiaddr'

if (!global.crypto) {
    global.crypto = webcrypto
}

class CustomEventPolyfill extends Event {
    constructor(type, dict) {
        super(type, dict);
        this.detail = dict?.detail;
    }
}
if (typeof global.CustomEvent === 'undefined') {
    global.CustomEvent = CustomEventPolyfill;
}

if (!Promise.withResolvers) {
    Promise.withResolvers = function() {
        let resolve, reject;
        const promise = new Promise((res, rej) => {
            resolve = res;
            reject = rej;
        });
        return { promise, resolve, reject };
    };
}


const HUB_ADDR = '/ip4/127.0.0.1/tcp/4002/ws/p2p/12D3KooWC98sHo1BYdpQgK1oZDMHeVLzZcG352qaw2b5spfWbD4V'
const ANNOUNCE_TOPIC = 'omd-sync-announcements'

async function createNode(name) {
  const libp2p = await createLibp2p({
    addresses: { listen: ['/ip4/0.0.0.0/tcp/0/ws'] },
    transports: [webSockets()],
    connectionEncrypters: [noise()],
    streamMuxers: [yamux()],
    services: {
      identify: identify(),
      pubsub: gossipsub({ 
        allowPublishToZeroTopicPeers: true,
        globalSignaturePolicy: 'StrictNoSign',
        emitSelf: true 
      })
    }
  })

  const helia = await createHelia({ libp2p })
  const orbitdb = await createOrbitDB({ ipfs: helia, directory: `./test-data/${name}` })

  console.log(`${name} Peer ID:`, libp2p.peerId.toString())
  
  // Connect to Hub
  console.log(`${name} dialing Hub...`)
  await libp2p.dial(multiaddr(HUB_ADDR))
  console.log(`${name} connected to Hub.`)

  setInterval(() => {
    const peers = libp2p.getPeers()
    console.log(`${name} connected peers:`, peers.map(p => p.toString()))
    const topics = libp2p.services.pubsub.getTopics()
    console.log(`${name} subscribed topics:`, topics)
  }, 5000)

  return { libp2p, orbitdb, helia }
}

async function run() {
  const nodeA = await createNode('NodeA')
  const nodeB = await createNode('NodeB')

  const dbName = 'test-chat'
  
  // Node B subscribes to the announcement topic to "hear" about Node A's database
  nodeB.libp2p.services.pubsub.subscribe(ANNOUNCE_TOPIC)
  nodeB.libp2p.services.pubsub.addEventListener('message', async (evt) => {
    if (evt.detail.topic === ANNOUNCE_TOPIC) {
        const data = JSON.parse(new TextDecoder().decode(evt.detail.data))
        console.log(`NodeB heard announcement for:`, data.chatsAddress)
        if (data.chatsAddress) {
            console.log(`NodeB opening database: ${data.chatsAddress}`)
            const dbB = await nodeB.orbitdb.open(data.chatsAddress)
            console.log(`NodeB opened database. Waiting for sync...`)
            dbB.events.on('update', (entry) => {
                console.log(`NodeB RECEIVED SYNCED MESSAGE:`, JSON.stringify(entry.payload.value))
                process.exit(0)
            })
        }
    }
  })

  // Wait a bit for discovery
  await new Promise(r => setTimeout(r, 2000))

  // Node A creates a database and announces it
  const dbA = await nodeA.orbitdb.open(dbName, { type: 'events' })
  console.log(`NodeA created database: ${dbA.address}`)
  
  const announcement = JSON.stringify({ chatsAddress: dbA.address })
  await nodeA.libp2p.services.pubsub.publish(ANNOUNCE_TOPIC, new TextEncoder().encode(announcement))
  console.log(`NodeA announced database: ${dbA.address}`)

  // Wait a bit, then Node A adds a message
  await new Promise(r => setTimeout(r, 5000))
  console.log(`NodeA adding message: "Hello from NodeA!"`)
  await dbA.add({ content: 'Hello from NodeA!' })

  // Keep alive for 15 seconds then fail if no sync
  setTimeout(() => {
    console.error('Simulation timed out without sync!')
    process.exit(1)
  }, 15000)
}

run().catch(console.error)
